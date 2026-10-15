#include "mic_stream.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/i2s_std.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gateway_secrets.h"
#include "media_protocol.h"

#define AUDIO_WSS_URI "wss://renesas.zyxserver.online/api/v1/media/device/audio"
#define MIC_I2S_PORT I2S_NUM_0
#define MIC_BCLK_PIN 14
#define MIC_WS_PIN 21
#define MIC_DATA_PIN 47
#define MIC_SAMPLE_RATE 16000
#define MIC_SAMPLES_PER_FRAME 320
#define MIC_QUEUE_LENGTH 8
#define MEDIA_CONTROL_TEXT_CAPACITY 512U
#define MEDIA_HEARTBEAT_INTERVAL_MS 15000U
#define MEDIA_WSS_PING_INTERVAL_SEC 300U

static const char *TAG = "mic_stream";

typedef struct {
    uint32_t sequence;
    uint64_t timestamp_us;
    int16_t samples[MIC_SAMPLES_PER_FRAME];
} mic_frame_t;

static QueueHandle_t s_audio_queue;
static i2s_chan_handle_t s_rx_channel;
static esp_websocket_client_handle_t s_audio_ws;
static volatile bool s_audio_connected;
static volatile bool s_audio_enabled;
static uint32_t s_sequence = 1;
static uint32_t s_queue_drops;
static uint32_t s_send_failures;
static uint32_t s_frames_sent;
static uint32_t s_disconnects;
static uint32_t s_heartbeat_sequence = 1;
static volatile uint32_t s_session_generation;
static uint32_t s_last_control_sequence;
static bool s_has_control_sequence;
static char s_control_text[MEDIA_CONTROL_TEXT_CAPACITY];
static size_t s_control_text_length;
static size_t s_control_text_expected;
static volatile bool s_discontinuity_pending = true;
static portMUX_TYPE s_discontinuity_mux = portMUX_INITIALIZER_UNLOCKED;

bool mic_stream_is_connected(void)
{
    return s_audio_connected;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void mark_discontinuity(void)
{
    portENTER_CRITICAL(&s_discontinuity_mux);
    s_discontinuity_pending = true;
    portEXIT_CRITICAL(&s_discontinuity_mux);
}

static bool take_discontinuity(void)
{
    bool pending;
    portENTER_CRITICAL(&s_discontinuity_mux);
    pending = s_discontinuity_pending;
    s_discontinuity_pending = false;
    portEXIT_CRITICAL(&s_discontinuity_mux);
    return pending;
}

static int16_t clamp_pcm16(int32_t value)
{
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

static void clear_audio_queue(void)
{
    if (!s_audio_queue) return;
    mic_frame_t discarded;
    while (xQueueReceive(s_audio_queue, &discarded, 0) == pdTRUE) {
    }
}

static void apply_control(const char *text, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(text, length);
    if (!root) return;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(root, "sequence");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "media.control") == 0 &&
        cJSON_IsNumber(version) && version->valueint == 1 && cJSON_IsNumber(sequence) &&
        sequence->valuedouble >= 0 && sequence->valuedouble <= UINT32_MAX && cJSON_IsBool(enabled)) {
        uint32_t control_sequence = (uint32_t)sequence->valuedouble;
        if (s_has_control_sequence && (int32_t)(control_sequence - s_last_control_sequence) <= 0) {
            cJSON_Delete(root);
            return;
        }
        s_last_control_sequence = control_sequence;
        s_has_control_sequence = true;
        bool next = cJSON_IsTrue(enabled);
        if (next != s_audio_enabled) {
            s_audio_enabled = next;
            if (!next) clear_audio_queue();
            if (next) mark_discontinuity();
            ESP_LOGI(TAG, "audio upload %s", next ? "enabled" : "paused");
        }
    }
    cJSON_Delete(root);
}

static void reset_control_session(void)
{
    s_audio_enabled = false;
    s_has_control_sequence = false;
    s_last_control_sequence = 0;
    s_control_text_length = 0;
    s_control_text_expected = 0;
    clear_audio_queue();
    mark_discontinuity();
}

static void receive_control_fragment(const esp_websocket_event_data_t *data)
{
    if (!data || data->op_code != 0x1) return;
    if (data->payload_offset == 0) {
        s_control_text_length = 0;
        s_control_text_expected = data->payload_len;
    }
    if (s_control_text_expected == 0 ||
        s_control_text_expected >= sizeof(s_control_text) ||
        data->payload_offset + data->data_len > s_control_text_expected) {
        ESP_LOGW(TAG, "discarding invalid audio control message length=%u offset=%u chunk=%u",
                 (unsigned)s_control_text_expected, (unsigned)data->payload_offset,
                 (unsigned)data->data_len);
        s_control_text_length = 0;
        s_control_text_expected = 0;
        return;
    }
    memcpy(s_control_text + data->payload_offset, data->data_ptr, data->data_len);
    s_control_text_length = data->payload_offset + data->data_len;
    if (s_control_text_length == s_control_text_expected) {
        s_control_text[s_control_text_length] = '\0';
        apply_control(s_control_text, s_control_text_length);
        s_control_text_length = 0;
        s_control_text_expected = 0;
    }
}

static void audio_ws_event(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;
    esp_websocket_event_data_t *data = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        reset_control_session();
        s_audio_connected = true;
        ++s_session_generation;
        ESP_LOGI(TAG, "audio WSS connected session=%" PRIu32, s_session_generation);
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        if (s_audio_connected) ++s_disconnects;
        s_audio_connected = false;
        reset_control_session();
        ESP_LOGW(TAG, "audio WSS closed event=%" PRIi32 " reconnects=%" PRIu32,
                 event_id, s_disconnects);
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        receive_control_fragment(data);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        if (s_audio_connected) ++s_disconnects;
        s_audio_connected = false;
        reset_control_session();
        ESP_LOGW(TAG,
                 "audio WSS error type=%d esp=%s tls=%d socket_errno=%d",
                 data ? (int)data->error_handle.error_type : -1,
                 data ? esp_err_to_name(data->error_handle.esp_tls_last_esp_err) : "unknown",
                 data ? data->error_handle.esp_tls_stack_err : 0,
                 data ? data->error_handle.esp_transport_sock_errno : 0);
    }
}

static void mic_capture_task(void *argument)
{
    (void)argument;
    int32_t raw[MIC_SAMPLES_PER_FRAME];
    mic_frame_t frame;
    while (true) {
        size_t bytes_read = 0;
        esp_err_t result = i2s_channel_read(s_rx_channel, raw, sizeof(raw), &bytes_read,
                                            pdMS_TO_TICKS(1000));
        if (result != ESP_OK || bytes_read != sizeof(raw)) {
            if (result != ESP_ERR_TIMEOUT) ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(result));
            continue;
        }
        if (!s_audio_enabled || !s_audio_connected) continue;
        frame.sequence = s_sequence++;
        frame.timestamp_us = (uint64_t)esp_timer_get_time();
        for (size_t i = 0; i < MIC_SAMPLES_PER_FRAME; ++i) {
            frame.samples[i] = clamp_pcm16(raw[i] >> 14);
        }
        if (xQueueSend(s_audio_queue, &frame, 0) != pdTRUE) {
            mic_frame_t discarded;
            xQueueReceive(s_audio_queue, &discarded, 0);
            xQueueSend(s_audio_queue, &frame, 0);
            ++s_queue_drops;
            mark_discontinuity();
            if ((s_queue_drops % 100U) == 1U) ESP_LOGW(TAG, "audio queue drops=%" PRIu32, s_queue_drops);
        }
    }
}

static void mic_send_task(void *argument)
{
    (void)argument;
    mic_frame_t frame;
    uint8_t wire[MEDIA_AUDIO_FRAME_SIZE];
    uint32_t last_heartbeat_ms = 0;
    uint32_t active_session = 0;
    uint32_t last_sent_sequence = 0;
    bool has_last_sent_sequence = false;
    while (true) {
        if (s_audio_connected &&
            (!last_heartbeat_ms || now_ms() - last_heartbeat_ms >= MEDIA_HEARTBEAT_INTERVAL_MS)) {
            char heartbeat[128];
            int length = snprintf(
                heartbeat, sizeof(heartbeat),
                "{\"type\":\"media.heartbeat\",\"version\":1,\"sequence\":%" PRIu32
                ",\"device_time_ms\":%" PRIu32 "}",
                s_heartbeat_sequence++, now_ms());
            int sent = length > 0 && length < (int)sizeof(heartbeat)
                           ? esp_websocket_client_send_text(
                                 s_audio_ws, heartbeat, length, pdMS_TO_TICKS(1000))
                           : -1;
            if (sent != length) {
                ++s_send_failures;
                ESP_LOGW(TAG, "audio heartbeat send failed: %d/%d", sent, length);
            }
            last_heartbeat_ms = now_ms();
        }
        if (xQueueReceive(s_audio_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (!s_audio_enabled || !s_audio_connected) continue;
        uint32_t session = s_session_generation;
        bool discontinuity = take_discontinuity();
        if (active_session != session) {
            active_session = session;
            has_last_sent_sequence = false;
            discontinuity = true;
        }
        if (has_last_sent_sequence && frame.sequence != last_sent_sequence + 1U) {
            discontinuity = true;
        }
        media_build_pcm_header(wire, frame.sequence, frame.timestamp_us,
                               discontinuity ? MEDIA_FLAG_DISCONTINUITY : 0);
        memcpy(wire + MEDIA_HEADER_SIZE, frame.samples, MEDIA_AUDIO_PAYLOAD_SIZE);
        int sent = esp_websocket_client_send_bin(s_audio_ws, (const char *)wire, sizeof(wire),
                                                  pdMS_TO_TICKS(1000));
        if (sent == sizeof(wire)) {
            last_sent_sequence = frame.sequence;
            has_last_sent_sequence = true;
            ++s_frames_sent;
            if ((s_frames_sent % 500U) == 1U) {
                ESP_LOGI(TAG,
                         "audio health sent=%" PRIu32 " drops=%" PRIu32
                         " failures=%" PRIu32 " session=%" PRIu32,
                         s_frames_sent, s_queue_drops, s_send_failures, active_session);
            }
        } else {
            ++s_send_failures;
            mark_discontinuity();
            ESP_LOGW(TAG, "audio frame send failed: %d/%u", sent, (unsigned)sizeof(wire));
        }
    }
}

esp_err_t mic_stream_start(void)
{
    s_audio_queue = xQueueCreate(MIC_QUEUE_LENGTH, sizeof(mic_frame_t));
    if (!s_audio_queue) return ESP_ERR_NO_MEM;

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 4;
    channel_config.dma_frame_num = MIC_SAMPLES_PER_FRAME;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, NULL, &s_rx_channel), TAG,
                        "create I2S RX channel");
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK_PIN,
            .ws = MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DATA_PIN,
            .invert_flags = {false, false, false},
        },
    };
    config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_channel, &config), TAG,
                        "configure I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_channel), TAG, "enable I2S RX");

    char headers[256];
    snprintf(headers, sizeof(headers), "X-Device-ID: %s\r\nX-Device-Key: %s\r\n",
             GATEWAY_DEVICE_ID, GATEWAY_DEVICE_KEY);
    esp_websocket_client_config_t ws_config = {
        .uri = AUDIO_WSS_URI,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 1000,
        .task_prio = 6,
        .keep_alive_enable = true,
        .keep_alive_idle = 15,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .enable_close_reconnect = true,
        .ping_interval_sec = MEDIA_WSS_PING_INTERVAL_SEC,
        .disable_pingpong_discon = true,
        .buffer_size = 1024,
    };
    s_audio_ws = esp_websocket_client_init(&ws_config);
    if (!s_audio_ws) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(s_audio_ws, WEBSOCKET_EVENT_ANY,
                                                       audio_ws_event, NULL), TAG,
                        "register audio WSS events");
    ESP_RETURN_ON_ERROR(esp_websocket_client_start(s_audio_ws), TAG, "start audio WSS");
    if (xTaskCreatePinnedToCore(mic_capture_task, "mic_capture", 4096, NULL, 7, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(mic_send_task, "mic_send", 6144, NULL, 6, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "INMP441 ready: BCLK=GPIO%d WS=GPIO%d DIN=GPIO%d, 16kHz PCM16",
             MIC_BCLK_PIN, MIC_WS_PIN, MIC_DATA_PIN);
    return ESP_OK;
}

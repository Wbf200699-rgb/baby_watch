#include "mic_stream.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "media_protocol.h"
#include "realtime_tx_guard.h"

#ifndef GATEWAY_WS_ORIGIN
#define GATEWAY_WS_ORIGIN "wss://renesas.zyxserver.online"
#endif

#define AUDIO_WSS_URI GATEWAY_WS_ORIGIN "/api/v1/media/device/audio"
#define AUDIO_WSS_PATH "/api/v1/media/device/audio"
#define MIC_SAMPLE_RATE 16000
#define MIC_SAMPLES_PER_FRAME 320
#define MIC_CAPTURE_CHANNELS 2
#define MIC_QUEUE_LENGTH 3
#define MIC_SEND_TIMEOUT_MS 250U
#define VOICE_GATE_MIN_GAIN_Q15 12288
#define VOICE_AGC_UNITY_Q12 4096
#define VOICE_AGC_MIN_GAIN_Q12 2048
#define VOICE_AGC_MAX_GAIN_Q12 24576
#define VOICE_AGC_TARGET_RMS 4200

static const char *TAG = "p4x_mic";

typedef struct {
    uint32_t sequence;
    uint64_t timestamp_us;
    int16_t samples[MIC_SAMPLES_PER_FRAME];
} mic_frame_t;

typedef struct {
    int32_t previous_input;
    int32_t previous_output;
    int32_t lowpass_history[4];
    uint32_t noise_floor_rms;
    int32_t gate_gain_q15;
    int32_t agc_gain_q12;
} voice_filter_t;

static QueueHandle_t s_audio_queue;
static esp_websocket_client_handle_t s_audio_ws;
static esp_transport_handle_t s_audio_ssl_transport;
static esp_transport_handle_t s_audio_transport;
static volatile bool s_audio_connected;
static volatile bool s_audio_enabled;
static volatile bool s_audio_transport_suspended;
static bool s_audio_prepared;
static bool s_audio_started;
static uint32_t s_sequence = 1;
static uint32_t s_queue_drops;
static uint32_t s_send_failures;
static uint32_t s_send_timeouts;
static uint32_t s_arbitration_drops;
static uint32_t s_backpressure_drops;
static uint32_t s_nodelay_failures;
static uint32_t s_last_send_ms;
static uint32_t s_max_send_ms;
static uint32_t s_last_control_sequence;
static bool s_has_control_sequence;
static uint32_t s_control_count;
static int64_t s_last_control_at_us;

bool mic_stream_is_connected(void)
{
    return s_audio_connected;
}

void mic_stream_get_stats(mic_stream_stats_t *stats)
{
    if (!stats) {
        return;
    }
    stats->enabled = s_audio_enabled;
    stats->transport_suspended = s_audio_transport_suspended;
    stats->control_count = s_control_count;
    stats->last_control_sequence = s_last_control_sequence;
    stats->last_control_age_ms = s_last_control_at_us > 0
                                     ? (uint32_t)((esp_timer_get_time() -
                                                   s_last_control_at_us) /
                                                  1000)
                                     : UINT32_MAX;
    stats->queue_drops = s_queue_drops;
    stats->send_failures = s_send_failures;
    stats->send_timeouts = s_send_timeouts;
    stats->arbitration_drops = s_arbitration_drops;
    stats->backpressure_drops = s_backpressure_drops;
    stats->nodelay_failures = s_nodelay_failures;
    stats->last_send_ms = s_last_send_ms;
    stats->max_send_ms = s_max_send_ms;
}

static void destroy_audio_transport(void)
{
    if (s_audio_transport) {
        esp_transport_destroy(s_audio_transport);
        s_audio_transport = NULL;
    }
    if (s_audio_ssl_transport) {
        esp_transport_destroy(s_audio_ssl_transport);
        s_audio_ssl_transport = NULL;
    }
}

static esp_err_t create_audio_transport(const char *headers)
{
    s_audio_ssl_transport = esp_transport_ssl_init();
    if (!s_audio_ssl_transport) {
        return ESP_ERR_NO_MEM;
    }
    esp_transport_set_default_port(s_audio_ssl_transport, 443);
    esp_transport_ssl_crt_bundle_attach(s_audio_ssl_transport,
                                        esp_crt_bundle_attach);

    s_audio_transport = esp_transport_ws_init(s_audio_ssl_transport);
    if (!s_audio_transport) {
        destroy_audio_transport();
        return ESP_ERR_NO_MEM;
    }
    esp_transport_set_default_port(s_audio_transport, 443);
    const esp_transport_ws_config_t transport_config = {
        .ws_path = AUDIO_WSS_PATH,
        .headers = headers,
        .propagate_control_frames = true,
    };
    esp_err_t result =
        esp_transport_ws_set_config(s_audio_transport, &transport_config);
    if (result != ESP_OK) {
        destroy_audio_transport();
    }
    return result;
}

static void enable_audio_tcp_nodelay(void)
{
    int socket_fd = esp_transport_get_socket(s_audio_transport);
    int enabled = 1;
    if (socket_fd < 0 ||
        setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                   sizeof(enabled)) != 0) {
        ++s_nodelay_failures;
        ESP_LOGW(TAG,
                 "audio TCP_NODELAY failed: fd=%d errno=%d failures=%" PRIu32,
                 socket_fd, errno, s_nodelay_failures);
        return;
    }
    ESP_LOGI(TAG, "audio TCP_NODELAY enabled");
}

static int16_t clamp_pcm16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static uint32_t integer_sqrt_u32(uint32_t value)
{
    uint32_t result = 0;
    uint32_t bit = 1U << 30;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static void filter_frame(voice_filter_t *filter, int16_t *samples)
{
    uint64_t energy = 0;
    uint32_t peak = 0;

    for (size_t i = 0; i < MIC_SAMPLES_PER_FRAME; ++i) {
        int32_t input = samples[i];
        int32_t highpass = input - filter->previous_input +
                           (int32_t)(((int64_t)filter->previous_output * 32112) >> 15);
        filter->previous_input = input;
        filter->previous_output = highpass;

        int32_t filtered = (highpass +
                            4 * filter->lowpass_history[0] +
                            6 * filter->lowpass_history[1] +
                            4 * filter->lowpass_history[2] +
                            filter->lowpass_history[3]) >> 4;
        filter->lowpass_history[3] = filter->lowpass_history[2];
        filter->lowpass_history[2] = filter->lowpass_history[1];
        filter->lowpass_history[1] = filter->lowpass_history[0];
        filter->lowpass_history[0] = highpass;

        samples[i] = clamp_pcm16(filtered);
        energy += (uint64_t)((int64_t)samples[i] * samples[i]);
        uint32_t magnitude = samples[i] < 0 ? (uint32_t)(-(int32_t)samples[i])
                                            : (uint32_t)samples[i];
        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    uint32_t rms = integer_sqrt_u32((uint32_t)(energy / MIC_SAMPLES_PER_FRAME));
    if (filter->noise_floor_rms == 0) {
        filter->noise_floor_rms = rms > 1 ? rms : 1;
        filter->gate_gain_q15 = VOICE_GATE_MIN_GAIN_Q15;
        filter->agc_gain_q12 = VOICE_AGC_UNITY_Q12;
    } else if (rms < filter->noise_floor_rms) {
        filter->noise_floor_rms = (15U * filter->noise_floor_rms + rms) / 16U;
    } else if (rms < filter->noise_floor_rms * 2U) {
        filter->noise_floor_rms = (255U * filter->noise_floor_rms + rms) / 256U;
    }

    uint32_t close_threshold = filter->noise_floor_rms + filter->noise_floor_rms / 8U;
    uint32_t open_threshold = filter->noise_floor_rms + filter->noise_floor_rms / 2U;
    if (close_threshold < 24U) {
        close_threshold = 24U;
    }
    if (open_threshold < 64U) {
        open_threshold = 64U;
    }

    int32_t target_gate = VOICE_GATE_MIN_GAIN_Q15;
    if (rms >= open_threshold) {
        target_gate = 32768;
    } else if (rms > close_threshold) {
        target_gate += (int32_t)(((uint64_t)(rms - close_threshold) *
                                  (32768U - VOICE_GATE_MIN_GAIN_Q15)) /
                                 (open_threshold - close_threshold));
    }

    int32_t old_gate = filter->gate_gain_q15;
    int32_t new_gate = target_gate > old_gate
                           ? old_gate + (target_gate - old_gate) / 2
                           : old_gate + (target_gate - old_gate) / 12;

    int32_t target_agc = VOICE_AGC_UNITY_Q12;
    if (rms > close_threshold) {
        uint32_t gated_rms = (uint32_t)(((uint64_t)rms * (uint32_t)new_gate) >> 15);
        if (gated_rms > 0) {
            target_agc = (int32_t)(((uint64_t)VOICE_AGC_TARGET_RMS *
                                    VOICE_AGC_UNITY_Q12) / gated_rms);
        }
        if (peak > 0) {
            uint64_t gated_peak = ((uint64_t)peak * (uint32_t)new_gate) >> 15;
            int32_t peak_limit = gated_peak
                                     ? (int32_t)((28000ULL * VOICE_AGC_UNITY_Q12) /
                                                 gated_peak)
                                     : VOICE_AGC_MAX_GAIN_Q12;
            if (target_agc > peak_limit) {
                target_agc = peak_limit;
            }
        }
        if (target_agc < VOICE_AGC_MIN_GAIN_Q12) {
            target_agc = VOICE_AGC_MIN_GAIN_Q12;
        } else if (target_agc > VOICE_AGC_MAX_GAIN_Q12) {
            target_agc = VOICE_AGC_MAX_GAIN_Q12;
        }
    }

    int32_t old_agc = filter->agc_gain_q12 > 0
                          ? filter->agc_gain_q12
                          : VOICE_AGC_UNITY_Q12;
    int32_t new_agc = target_agc < old_agc
                          ? old_agc + (target_agc - old_agc) / 2
                          : old_agc + (target_agc - old_agc) / 16;

    for (size_t i = 0; i < MIC_SAMPLES_PER_FRAME; ++i) {
        int32_t gate = old_gate +
                       (int32_t)(((int64_t)(new_gate - old_gate) * (i + 1)) /
                                 MIC_SAMPLES_PER_FRAME);
        int32_t agc = old_agc +
                      (int32_t)(((int64_t)(new_agc - old_agc) * (i + 1)) /
                                MIC_SAMPLES_PER_FRAME);
        int32_t gated = (int32_t)(((int64_t)samples[i] * gate) >> 15);
        samples[i] = clamp_pcm16((int32_t)(((int64_t)gated * agc) >> 12));
    }
    filter->gate_gain_q15 = new_gate;
    filter->agc_gain_q12 = new_agc;
}

static void clear_audio_queue(void)
{
    mic_frame_t discarded;
    while (xQueueReceive(s_audio_queue, &discarded, 0) == pdTRUE) {
    }
}

esp_err_t mic_stream_set_transport_suspended(bool suspended)
{
    if (!s_audio_ws || suspended == s_audio_transport_suspended) {
        return ESP_OK;
    }

    if (suspended) {
        s_audio_transport_suspended = true;
        clear_audio_queue();
        esp_err_t result = esp_websocket_client_stop(s_audio_ws);
        if (result == ESP_ERR_INVALID_STATE) {
            result = ESP_OK;
        }
        if (result != ESP_OK) {
            s_audio_transport_suspended = false;
            ESP_LOGW(TAG, "pause audio WSS failed: %s", esp_err_to_name(result));
            return result;
        }
        ESP_LOGI(TAG, "audio WSS paused for bulk transfer");
        return ESP_OK;
    }

    s_audio_transport_suspended = false;
    esp_err_t result = esp_websocket_client_start(s_audio_ws);
    if (result == ESP_ERR_INVALID_STATE) {
        result = ESP_OK;
    }
    if (result != ESP_OK) {
        s_audio_transport_suspended = true;
        ESP_LOGW(TAG, "resume audio WSS failed: %s", esp_err_to_name(result));
        return result;
    }
    ESP_LOGI(TAG, "audio WSS resumed after bulk transfer");
    return ESP_OK;
}

static void apply_control(const char *text, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(text, length);
    if (!root) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(root, "sequence");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "media.control") == 0 &&
        cJSON_IsNumber(version) && version->valueint == 1 && cJSON_IsNumber(sequence) &&
        sequence->valuedouble >= 0 && sequence->valuedouble <= UINT32_MAX &&
        cJSON_IsBool(enabled)) {
        uint32_t next_sequence = (uint32_t)sequence->valuedouble;
        if (!s_has_control_sequence ||
            (int32_t)(next_sequence - s_last_control_sequence) > 0) {
            s_last_control_sequence = next_sequence;
            s_has_control_sequence = true;
            ++s_control_count;
            s_last_control_at_us = esp_timer_get_time();
            s_audio_enabled = cJSON_IsTrue(enabled);
            if (!s_audio_enabled) {
                clear_audio_queue();
            }
            ESP_LOGI(TAG, "audio upload %s",
                     s_audio_enabled ? "enabled" : "paused");
        }
    }
    cJSON_Delete(root);
}

static void audio_ws_event(void *args, esp_event_base_t base, int32_t event_id,
                           void *event_data)
{
    (void)args;
    (void)base;
    esp_websocket_event_data_t *data = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        /* Control sequence numbers are scoped to one server process/WSS. */
        s_has_control_sequence = false;
        s_audio_enabled = false;
        s_last_control_at_us = 0;
        clear_audio_queue();
        enable_audio_tcp_nodelay();
        s_audio_connected = true;
        ESP_LOGI(TAG, "audio WSS connected");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_audio_connected = false;
        s_audio_enabled = false;
        s_has_control_sequence = false;
        clear_audio_queue();
        ESP_LOGW(TAG, "audio WSS disconnected");
    } else if (event_id == WEBSOCKET_EVENT_DATA && data &&
               data->op_code == 0x1 && data->payload_offset == 0 &&
               data->payload_len == data->data_len) {
        apply_control(data->data_ptr, data->data_len);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        s_audio_connected = false;
        s_audio_enabled = false;
        s_has_control_sequence = false;
        s_last_control_at_us = 0;
        clear_audio_queue();
        ESP_LOGW(TAG, "audio WSS transport error");
    }
}

static void mic_capture_task(void *argument)
{
    (void)argument;
    int16_t raw[MIC_SAMPLES_PER_FRAME * MIC_CAPTURE_CHANNELS];
    mic_frame_t frame;
    voice_filter_t filter = {0};

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t result = bsp_extra_pdm_i2s_read(raw, sizeof(raw), &bytes_read, 1000);
        if (result != ESP_OK || bytes_read != sizeof(raw)) {
            if (result != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "PDM read failed: %s, bytes=%u",
                         esp_err_to_name(result), (unsigned)bytes_read);
            }
            continue;
        }

        for (size_t i = 0; i < MIC_SAMPLES_PER_FRAME; ++i) {
            frame.samples[i] = raw[i * MIC_CAPTURE_CHANNELS];
        }
        filter_frame(&filter, frame.samples);

        if (s_audio_transport_suspended || !s_audio_enabled || !s_audio_connected ||
            !esp_websocket_client_is_connected(s_audio_ws)) {
            if (s_audio_connected &&
                !esp_websocket_client_is_connected(s_audio_ws)) {
                s_audio_connected = false;
                clear_audio_queue();
            }
            continue;
        }
        frame.sequence = s_sequence++;
        frame.timestamp_us = (uint64_t)esp_timer_get_time();
        if (xQueueSend(s_audio_queue, &frame, 0) != pdTRUE) {
            mic_frame_t discarded;
            xQueueReceive(s_audio_queue, &discarded, 0);
            xQueueSend(s_audio_queue, &frame, 0);
            ++s_queue_drops;
            if ((s_queue_drops % 100U) == 1U) {
                ESP_LOGW(TAG, "audio queue drops=%" PRIu32, s_queue_drops);
            }
        }
    }
}

static void mic_send_task(void *argument)
{
    (void)argument;
    mic_frame_t frame;
    uint8_t wire[MEDIA_AUDIO_FRAME_SIZE];
    for (;;) {
        if (xQueueReceive(s_audio_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_audio_transport_suspended || !s_audio_enabled || !s_audio_connected) {
            continue;
        }
        media_build_pcm_header(wire, frame.sequence, frame.timestamp_us);
        memcpy(wire + MEDIA_HEADER_SIZE, frame.samples, MEDIA_AUDIO_PAYLOAD_SIZE);
        if (!realtime_tx_guard_take(0)) {
            ++s_arbitration_drops;
            if ((s_arbitration_drops % 500U) == 1U) {
                ESP_LOGW(TAG,
                         "audio frame dropped for realtime TX priority: %" PRIu32,
                         s_arbitration_drops);
            }
            continue;
        }
        if (esp_transport_poll_write(s_audio_transport, 0) <= 0) {
            realtime_tx_guard_give();
            ++s_backpressure_drops;
            if ((s_backpressure_drops % 500U) == 1U) {
                ESP_LOGW(TAG, "audio frame dropped for socket backpressure: %" PRIu32,
                         s_backpressure_drops);
            }
            continue;
        }
        int64_t started_us = esp_timer_get_time();
        int sent = esp_websocket_client_send_bin(s_audio_ws, (const char *)wire,
                                                  sizeof(wire),
                                                  pdMS_TO_TICKS(MIC_SEND_TIMEOUT_MS));
        uint32_t elapsed_ms =
            (uint32_t)((esp_timer_get_time() - started_us) / 1000);
        realtime_tx_guard_give();
        s_last_send_ms = elapsed_ms;
        if (elapsed_ms > s_max_send_ms) {
            s_max_send_ms = elapsed_ms;
        }
        if (sent != sizeof(wire)) {
            ++s_send_failures;
            if (elapsed_ms >= MIC_SEND_TIMEOUT_MS) {
                ++s_send_timeouts;
            }
            clear_audio_queue();
            if (!esp_websocket_client_is_connected(s_audio_ws)) {
                s_audio_connected = false;
            }
            if ((s_send_failures % 20U) == 1U) {
                ESP_LOGW(TAG,
                         "audio frame send failed: %d elapsed=%" PRIu32
                         "ms failures=%" PRIu32,
                         sent, elapsed_ms, s_send_failures);
            }
        }
    }
}

esp_err_t mic_stream_prepare(void)
{
    if (s_audio_prepared) return ESP_OK;
    s_audio_queue = xQueueCreate(MIC_QUEUE_LENGTH, sizeof(mic_frame_t));
    if (!s_audio_queue) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(bsp_extra_pdm_codec_init(), TAG,
                        "initialize onboard PDM microphone");
    s_audio_prepared = true;
    ESP_LOGI(TAG, "onboard PDM microphone DMA reserved early");
    return ESP_OK;
}

esp_err_t mic_stream_start(void)
{
    if (s_audio_started) return ESP_OK;
    ESP_RETURN_ON_ERROR(mic_stream_prepare(), TAG,
                        "prepare onboard PDM microphone");
    char headers[256];
    snprintf(headers, sizeof(headers),
             "X-Device-ID: %s\r\nX-Device-Key: %s\r\n",
             GATEWAY_DEVICE_ID, GATEWAY_DEVICE_KEY);
    esp_err_t result = create_audio_transport(headers);
    if (result != ESP_OK) {
        return result;
    }
    esp_websocket_client_config_t ws_config = {
        .uri = AUDIO_WSS_URI,
        .ext_transport = s_audio_transport,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 3000,
        .enable_close_reconnect = true,
        .ping_interval_sec = 20,
    };
    s_audio_ws = esp_websocket_client_init(&ws_config);
    if (!s_audio_ws) {
        destroy_audio_transport();
        return ESP_ERR_NO_MEM;
    }
    result = esp_websocket_register_events(s_audio_ws, WEBSOCKET_EVENT_ANY,
                                           audio_ws_event, NULL);
    if (result != ESP_OK) {
        esp_websocket_client_destroy(s_audio_ws);
        s_audio_ws = NULL;
        destroy_audio_transport();
        return result;
    }
    result = esp_websocket_client_start(s_audio_ws);
    if (result != ESP_OK) {
        esp_websocket_client_destroy(s_audio_ws);
        s_audio_ws = NULL;
        destroy_audio_transport();
        return result;
    }

    if (xTaskCreatePinnedToCore(mic_capture_task, "p4x_mic_capture", 6144,
                                NULL, 7, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(mic_send_task, "p4x_mic_send", 6144,
                                NULL, 4, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_audio_started = true;

    ESP_LOGI(TAG, "onboard PDM microphone ready: GPIO21 data, GPIO22 clock, "
                  "16 kHz mono PCM16, 20 ms/frame");
    return ESP_OK;
}

#include "camera_stream.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gateway_secrets.h"
#include "media_protocol.h"

#define VIDEO_WSS_URI "wss://renesas.zyxserver.online/api/v1/media/device/video"

/* Freenove ESP32-S3-WROOM-CAM and compatible boards use ESP32-S3-EYE mapping. */
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D7 16
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2 8
#define CAM_PIN_D1 9
#define CAM_PIN_D0 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

#define VIDEO_QUEUE_LENGTH 1
#define VIDEO_DEFAULT_FPS 8
#define VIDEO_MIN_FPS 5
#define VIDEO_MAX_FPS 10
#define VIDEO_VGA_QUALITY 14
#define VIDEO_CONGESTED_QUALITY 22
#define MEDIA_CONTROL_TEXT_CAPACITY 512U
#define MEDIA_HEARTBEAT_INTERVAL_MS 15000U
#define MEDIA_WSS_PING_INTERVAL_SEC 300U

static const char *TAG = "camera_stream";

typedef struct {
    camera_fb_t *fb;
    uint32_t sequence;
    uint64_t timestamp_us;
} video_frame_t;

static QueueHandle_t s_video_queue;
static esp_websocket_client_handle_t s_video_ws;
static volatile bool s_video_connected;
static volatile bool s_video_enabled;
static volatile uint8_t s_target_fps = VIDEO_DEFAULT_FPS;
static volatile framesize_t s_desired_size = FRAMESIZE_VGA;
static volatile uint8_t s_desired_quality = VIDEO_VGA_QUALITY;
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

static void clear_video_queue(void)
{
    if (!s_video_queue) return;
    video_frame_t frame;
    while (xQueueReceive(s_video_queue, &frame, 0) == pdTRUE) {
        if (frame.fb) esp_camera_fb_return(frame.fb);
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
    const cJSON *target_fps = cJSON_GetObjectItemCaseSensitive(root, "target_fps");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "media.control") == 0 &&
        cJSON_IsNumber(version) && version->valueint == 1 && cJSON_IsNumber(sequence) &&
        sequence->valuedouble >= 0 && sequence->valuedouble <= UINT32_MAX && cJSON_IsBool(enabled)) {
        uint32_t control_sequence = (uint32_t)sequence->valuedouble;
        if (!s_has_control_sequence || (int32_t)(control_sequence - s_last_control_sequence) > 0) {
            s_last_control_sequence = control_sequence;
            s_has_control_sequence = true;
            uint8_t fps = cJSON_IsNumber(target_fps) ? (uint8_t)target_fps->valueint : VIDEO_DEFAULT_FPS;
            if (fps < VIDEO_MIN_FPS) fps = VIDEO_MIN_FPS;
            if (fps > VIDEO_MAX_FPS) fps = VIDEO_MAX_FPS;
            s_target_fps = fps;
            bool next = cJSON_IsTrue(enabled);
            if (next != s_video_enabled) {
                s_video_enabled = next;
                if (!next) clear_video_queue();
                if (next) mark_discontinuity();
                ESP_LOGI(TAG, "video upload %s, target=%u fps", next ? "enabled" : "paused", fps);
            }
        }
    }
    cJSON_Delete(root);
}

static void reset_control_session(void)
{
    s_video_enabled = false;
    s_has_control_sequence = false;
    s_last_control_sequence = 0;
    s_control_text_length = 0;
    s_control_text_expected = 0;
    clear_video_queue();
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
        ESP_LOGW(TAG, "discarding invalid video control message length=%u offset=%u chunk=%u",
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

static void video_ws_event(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;
    esp_websocket_event_data_t *data = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        reset_control_session();
        s_video_connected = true;
        ++s_session_generation;
        ESP_LOGI(TAG, "video WSS connected session=%" PRIu32, s_session_generation);
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        if (s_video_connected) ++s_disconnects;
        s_video_connected = false;
        reset_control_session();
        ESP_LOGW(TAG, "video WSS closed event=%" PRIi32 " reconnects=%" PRIu32,
                 event_id, s_disconnects);
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        receive_control_fragment(data);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        if (s_video_connected) ++s_disconnects;
        s_video_connected = false;
        reset_control_session();
        ESP_LOGW(TAG,
                 "video WSS error type=%d esp=%s tls=%d socket_errno=%d",
                 data ? (int)data->error_handle.error_type : -1,
                 data ? esp_err_to_name(data->error_handle.esp_tls_last_esp_err) : "unknown",
                 data ? data->error_handle.esp_tls_stack_err : 0,
                 data ? data->error_handle.esp_transport_sock_errno : 0);
    }
}

static void apply_camera_settings(framesize_t *active_size, uint8_t *active_quality)
{
    framesize_t size = s_desired_size;
    uint8_t quality = s_desired_quality;
    if (size == *active_size && quality == *active_quality) return;
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) return;
    if (size != *active_size && sensor->set_framesize(sensor, size) == 0) *active_size = size;
    if (quality != *active_quality && sensor->set_quality(sensor, quality) == 0) *active_quality = quality;
    ESP_LOGI(TAG, "camera profile size=%s quality=%u",
             *active_size == FRAMESIZE_VGA ? "VGA" : "QVGA", *active_quality);
}

static void camera_capture_task(void *argument)
{
    (void)argument;
    framesize_t active_size = FRAMESIZE_VGA;
    uint8_t active_quality = VIDEO_VGA_QUALITY;
    TickType_t last_capture = xTaskGetTickCount();
    while (true) {
        if (!s_video_connected || !s_video_enabled) {
            vTaskDelay(pdMS_TO_TICKS(100));
            last_capture = xTaskGetTickCount();
            continue;
        }
        apply_camera_settings(&active_size, &active_quality);
        uint8_t fps = s_target_fps;
        if (active_size == FRAMESIZE_QVGA && fps > VIDEO_MIN_FPS) fps = VIDEO_MIN_FPS;
        vTaskDelayUntil(&last_capture, pdMS_TO_TICKS(1000U / fps));
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "camera capture failed");
            continue;
        }
        if (fb->format != PIXFORMAT_JPEG || fb->len == 0 ||
            fb->len > MEDIA_VIDEO_MAX_PAYLOAD_SIZE) {
            ESP_LOGW(TAG, "invalid camera frame format=%d bytes=%u", fb->format, (unsigned)fb->len);
            esp_camera_fb_return(fb);
            continue;
        }
        video_frame_t frame = {
            .fb = fb,
            .sequence = s_sequence++,
            .timestamp_us = (uint64_t)esp_timer_get_time(),
        };
        if (xQueueSend(s_video_queue, &frame, 0) != pdTRUE) {
            video_frame_t discarded;
            if (xQueueReceive(s_video_queue, &discarded, 0) == pdTRUE && discarded.fb) {
                esp_camera_fb_return(discarded.fb);
            }
            if (xQueueSend(s_video_queue, &frame, 0) != pdTRUE) esp_camera_fb_return(fb);
            ++s_queue_drops;
            mark_discontinuity();
            if ((s_queue_drops % 50U) == 1U) {
                ESP_LOGW(TAG, "video queue drops=%" PRIu32, s_queue_drops);
            }
        }
    }
}

static void camera_send_task(void *argument)
{
    (void)argument;
    uint8_t *wire = heap_caps_malloc(MEDIA_HEADER_SIZE + MEDIA_VIDEO_MAX_PAYLOAD_SIZE,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wire) {
        ESP_LOGE(TAG, "cannot allocate video send buffer");
        vTaskDelete(NULL);
    }
    unsigned congested = 0;
    unsigned healthy = 0;
    uint32_t last_heartbeat_ms = 0;
    uint32_t active_session = 0;
    uint32_t last_sent_sequence = 0;
    bool has_last_sent_sequence = false;
    video_frame_t frame;
    while (true) {
        if (s_video_connected &&
            (!last_heartbeat_ms || now_ms() - last_heartbeat_ms >= MEDIA_HEARTBEAT_INTERVAL_MS)) {
            char heartbeat[128];
            int length = snprintf(
                heartbeat, sizeof(heartbeat),
                "{\"type\":\"media.heartbeat\",\"version\":1,\"sequence\":%" PRIu32
                ",\"device_time_ms\":%" PRIu32 "}",
                s_heartbeat_sequence++, now_ms());
            int sent = length > 0 && length < (int)sizeof(heartbeat)
                           ? esp_websocket_client_send_text(
                                 s_video_ws, heartbeat, length, pdMS_TO_TICKS(1000))
                           : -1;
            if (sent != length) {
                ++s_send_failures;
                ESP_LOGW(TAG, "video heartbeat send failed: %d/%d", sent, length);
            }
            last_heartbeat_ms = now_ms();
        }
        if (xQueueReceive(s_video_queue, &frame, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (!s_video_enabled || !s_video_connected) {
            esp_camera_fb_return(frame.fb);
            continue;
        }
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
        uint8_t quality = s_desired_quality;
        media_build_jpeg_header(wire, frame.sequence, frame.timestamp_us,
                                discontinuity ? MEDIA_FLAG_DISCONTINUITY : 0, frame.fb->len,
                                frame.fb->width, frame.fb->height, quality);
        memcpy(wire + MEDIA_HEADER_SIZE, frame.fb->buf, frame.fb->len);
        size_t wire_length = MEDIA_HEADER_SIZE + frame.fb->len;
        int64_t started = esp_timer_get_time();
        int sent = esp_websocket_client_send_bin(s_video_ws, (const char *)wire, wire_length,
                                                  pdMS_TO_TICKS(1500));
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
        esp_camera_fb_return(frame.fb);
        bool sent_ok = sent == (int)wire_length;
        bool slow = !sent_ok || elapsed_ms > (1000U / s_target_fps);
        if (sent_ok) {
            last_sent_sequence = frame.sequence;
            has_last_sent_sequence = true;
            ++s_frames_sent;
            if ((s_frames_sent % 160U) == 1U) {
                ESP_LOGI(TAG,
                         "video health sent=%" PRIu32 " drops=%" PRIu32
                         " failures=%" PRIu32 " session=%" PRIu32,
                         s_frames_sent, s_queue_drops, s_send_failures, active_session);
            }
        }
        if (slow) {
            ++congested;
            healthy = 0;
            if (!sent_ok) {
                ++s_send_failures;
                mark_discontinuity();
                ESP_LOGW(TAG, "video frame send failed: %d/%u", sent, (unsigned)wire_length);
            }
            if (congested >= 3) s_desired_quality = VIDEO_CONGESTED_QUALITY;
            if (congested >= 6) s_desired_size = FRAMESIZE_QVGA;
        } else {
            congested = 0;
            if (++healthy >= 160) {
                s_desired_size = FRAMESIZE_VGA;
                s_desired_quality = VIDEO_VGA_QUALITY;
                healthy = 0;
            }
        }
    }
}

esp_err_t camera_stream_start(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = VIDEO_VGA_QUALITY,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    ESP_RETURN_ON_ERROR(esp_camera_init(&config), TAG, "initialize camera");
    sensor_t *sensor = esp_camera_sensor_get();
    ESP_LOGI(TAG, "camera ready: PID=0x%04x VGA JPEG", sensor ? sensor->id.PID : 0);
    camera_fb_t *test_frame = esp_camera_fb_get();
    if (!test_frame) {
        esp_camera_deinit();
        return ESP_FAIL;
    }
    bool test_ok = test_frame->format == PIXFORMAT_JPEG && test_frame->len > 0 &&
                   test_frame->len <= MEDIA_VIDEO_MAX_PAYLOAD_SIZE;
    ESP_LOGI(TAG, "camera self-test: %ux%u JPEG %u bytes",
             test_frame->width, test_frame->height, (unsigned)test_frame->len);
    esp_camera_fb_return(test_frame);
    if (!test_ok) {
        esp_camera_deinit();
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_video_queue = xQueueCreate(VIDEO_QUEUE_LENGTH, sizeof(video_frame_t));
    if (!s_video_queue) return ESP_ERR_NO_MEM;
    char headers[256];
    snprintf(headers, sizeof(headers), "X-Device-ID: %s\r\nX-Device-Key: %s\r\n",
             GATEWAY_DEVICE_ID, GATEWAY_DEVICE_KEY);
    esp_websocket_client_config_t ws_config = {
        .uri = VIDEO_WSS_URI,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 1000,
        .task_prio = 5,
        .keep_alive_enable = true,
        .keep_alive_idle = 15,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .enable_close_reconnect = true,
        .ping_interval_sec = MEDIA_WSS_PING_INTERVAL_SEC,
        .disable_pingpong_discon = true,
        .buffer_size = 4096,
    };
    s_video_ws = esp_websocket_client_init(&ws_config);
    if (!s_video_ws) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(s_video_ws, WEBSOCKET_EVENT_ANY,
                                                       video_ws_event, NULL), TAG,
                        "register video WSS events");
    ESP_RETURN_ON_ERROR(esp_websocket_client_start(s_video_ws), TAG, "start video WSS");
    if (xTaskCreatePinnedToCore(camera_capture_task, "camera_capture", 6144, NULL, 6, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(camera_send_task, "camera_send", 7168, NULL, 5, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

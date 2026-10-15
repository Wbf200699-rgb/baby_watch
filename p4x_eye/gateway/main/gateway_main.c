#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_hosted.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_hosted_ota.h"
#endif

#if !defined(GATEWAY_WIFI_SSID) || !defined(GATEWAY_WIFI_PASSWORD) || \
    !defined(GATEWAY_DEVICE_ID) || !defined(GATEWAY_DEVICE_KEY)
#include "gateway_secrets.h"
#endif
#include "camera_stream.h"
#include "mic_stream.h"
#include "realtime_tx_guard.h"

#define GATEWAY_STRINGIFY_INNER(value) #value
#define GATEWAY_STRINGIFY(value) GATEWAY_STRINGIFY_INNER(value)

#if CONFIG_IDF_TARGET_ESP32P4
#define FIRMWARE_VERSION "esp32p4x-eye-ra8-gateway-4.5.10"
#define HARDWARE_NAME "ESP32-P4X-EYE"
#define MICROPHONE_NAME "P4X-EYE onboard PDM microphone"
#define MICROPHONE_CAPABILITY "p4x_eye_onboard_pdm_microphone"
#define C6_OTA_EXPECTED_VERSION "2.12.12"
#define C6_OTA_CHUNK_SIZE 1400U
#define C6_OTA_MAX_ATTEMPTS 2U
#define C6_OTA_NVS_KEY "c6_ota_try"
#define C6_OTA_REVISION_NVS_KEY "c6_ota_rev"
#define C6_OTA_PENDING_NVS_KEY "c6_ota_pend"
#define C6_OTA_IMAGE_REVISION 1U

extern const uint8_t s_c6_firmware_start[]
    asm("_binary_esp_hosted_c6_2_12_12_bin_start");
extern const uint8_t s_c6_firmware_end[]
    asm("_binary_esp_hosted_c6_2_12_12_bin_end");
#define CAMERA_CAPABILITY "p4x_eye_ov2710_jpeg_wss"
#define CAMERA_DEFAULT "640x360@8fps"
#define RA8_UART_RX_PIN 8
#define RA8_UART_TX_PIN 7
#define RA8_SOFT_RESET_PIN GPIO_NUM_10
#else
#define FIRMWARE_VERSION "esp32s3cam-ra8-gateway-3.4.0"
#define HARDWARE_NAME "ESP32-S3-WROOM-CAM-N16R8"
#define MICROPHONE_NAME "S3 external microphone"
#define MICROPHONE_CAPABILITY "s3_external_microphone"
#define CAMERA_CAPABILITY "s3_camera_jpeg_wss"
#define CAMERA_DEFAULT "640x480@8fps"
#define RA8_UART_RX_PIN 1
#define RA8_UART_TX_PIN 2
#endif

#ifndef GATEWAY_HTTP_ORIGIN
#define GATEWAY_HTTP_ORIGIN "https://renesas.zyxserver.online"
#endif
#ifndef GATEWAY_WS_ORIGIN
#define GATEWAY_WS_ORIGIN "wss://renesas.zyxserver.online"
#endif

#define WSS_URI GATEWAY_WS_ORIGIN "/api/v1/device/stream"
#define MATRIX_WSS_URI GATEWAY_WS_ORIGIN "/api/v1/device/matrices"
#define SERVER_ORIGIN GATEWAY_HTTP_ORIGIN

#define RA8_UART UART_NUM_1
#define RA8_UART_BAUD 921600
#if CONFIG_IDF_TARGET_ESP32P4
#define STORY_BULK_UART UART_NUM_2
#define STORY_BULK_UART_TX_PIN 50
#define STORY_BULK_UART_RX_PIN 51
#define STORY_BULK_UART_BAUD 1000000
#endif

#define RA8_PROTOCOL_VERSION 1
#define RA8_TYPE_STATUS_UPLOAD 0x10
#define RA8_TYPE_THERMAL_CHUNK 0x11
#define RA8_TYPE_PRESSURE_UPLOAD 0x12
#define RA8_TYPE_SAFETY_EVENT 0x13
#define RA8_TYPE_RESOURCE_UPLOAD 0x14
#define RA8_TYPE_CMD 0x20
#define RA8_TYPE_CMD_RESULT 0x21
#define RA8_TYPE_SAFETY_ACK 0x22
#define RA8_TYPE_FILE_BEGIN 0x30
#define RA8_TYPE_FILE_CHUNK 0x31
#define RA8_TYPE_FILE_END 0x32
#define RA8_MAX_PAYLOAD 4096

#define WIFI_CONNECTED_BIT BIT0
#define EVENT_QUEUE_LENGTH 8
#define COMMAND_QUEUE_LENGTH 8
#define COMMAND_ACK_QUEUE_LENGTH 8
#define STORY_QUEUE_LENGTH 1
#define STORY_RESULT_QUEUE_LENGTH 4
#define STORY_PROTOCOL_VERSION 1U
#define STORY_FLAG_PLAY_AFTER BIT0
#define STORY_OPERATION_BEGIN 1U
#define STORY_OPERATION_CHUNK 2U
#define STORY_OPERATION_END 3U
#define STORY_RESULT_OK 0U
#define STORY_RESULT_BUSY 8U
#define STORY_CHUNK_BYTES 448U
#define STORY_MAX_BYTES (4U * 1024U * 1024U)
#define STORY_OPERATION_TIMEOUT_MS 150U
#define STORY_OPERATION_ATTEMPTS 20U
#define STORY_ACK_POLL_MS 10U
#define STORY_ACK_RETRY_BACKOFF_MS 5U
#define STORY_CHUNK_PACING_MS 0U
#define STORY_TASK_PRIORITY 7U
#define STORY_RESET_GRACE_MS 10000U
#define STORY_DOWNLOAD_TIMEOUT_MS 60000U
#define STORY_HTTP_READ_BYTES 8192U
#define STORY_MEDIA_QUIESCE_MS 300U
#define STORY_DOWNLOAD_ATTEMPTS 3U
#define STORY_RA8_TRANSFER_ATTEMPTS 3U
#define STORY_RETRY_BACKOFF_MS 1000U
#define STORY_BULK_MAGIC 0x53424C32U
#define STORY_BULK_VERSION 2U
#define STORY_BULK_HEADER_BYTES 32U
#define STORY_BULK_PAYLOAD_BYTES 4096U
#define STORY_BULK_WINDOW 1U
#define STORY_BULK_TYPE_PING 1U
#define STORY_BULK_TYPE_BEGIN 2U
#define STORY_BULK_TYPE_DATA 3U
#define STORY_BULK_TYPE_END 4U
#define STORY_BULK_TYPE_ABORT 5U
#define STORY_BULK_TYPE_READY 6U
#define STORY_BULK_TYPE_ACK 0x80U
#define STORY_BULK_HEADER_GAP_MS 2U
#define STORY_BULK_FRAME_GAP_MS 2U
#define STORY_BULK_ACK_TIMEOUT_MS 500U
#define TELEMETRY_WSS_BUFFER_BYTES (12U * 1024U)
#define MATRIX_WSS_BUFFER_BYTES (12U * 1024U)
#define TELEMETRY_INTERVAL_MS 250
#define TELEMETRY_STALL_MS 15000
#define TELEMETRY_DISCONNECTED_RECOVERY_MS 30000
#define TELEMETRY_RECOVERY_TIMEOUT_MS 30000
#define TELEMETRY_RECOVERY_GRACE_MS 30000
#define TELEMETRY_RECOVERY_RETRY_MS 10000
#define TELEMETRY_SELF_RESTART_MS 180000
#define TELEMETRY_STABLE_CLEAR_MS 600000
#define TELEMETRY_SELF_RESTART_LIMIT 2U
#define THERMAL_INTERVAL_MS 500
#define PRESSURE_INTERVAL_MS 125
#define RA8_OFFLINE_MS 5000
#define RA8_READY_INTERVAL_MS 2000
#define RA8_UART_RX_RECOVERY_MS 8000
#define RA8_UART_RX_RECOVERY_COOLDOWN_MS 15000
#define RA8_RESET_BOOT_GRACE_MS 30000
#define RA8_RESET_OFFLINE_MS 30000
#define RA8_RESET_LINK_SILENCE_MS 30000
#define RA8_RESET_COOLDOWN_MS 300000
#define RA8_RESET_PULSE_MS 150
#define BATCH_HEADER_SIZE 48U
#define BATCH_FLAG_PRESSURE BIT0
#define BATCH_FLAG_THERMAL BIT1
#define BATCH_RESPONSE_SIZE 4096U

static const char *TAG = "ra8_gateway";

typedef struct {
    uint32_t event_id;
    uint16_t length;
    char json[768];
} safety_event_t;

typedef struct {
    uint32_t id;
    char command[64];
    char args_json[768];
} server_command_t;

typedef struct {
    uint32_t id;
    bool ok;
    char message[128];
} command_ack_t;

typedef struct {
    uint8_t operation;
    uint8_t result;
    uint8_t status;
    uint32_t transfer_id;
    uint32_t next_offset;
    uint16_t request_sequence;
} story_result_t;

typedef struct {
    char path[192];
    char filename[16];
    char sha256[65];
    uint32_t size_bytes;
    uint32_t crc32;
    bool autoplay;
} story_request_t;

typedef struct {
    uint32_t valid_frames;
    uint32_t status_frames;
    uint32_t crc_errors;
    uint32_t length_errors;
    uint32_t event_drops;
    uint32_t uart_rx_bytes;
    uint32_t uart_read_errors;
    uint32_t uart_rx_recovery_count;
    uint32_t uart_rx_recovery_failures;
    uint32_t uart_rx_pin_transitions;
    int32_t uart_rx_pin_level;
    uint32_t ready_tx_count;
    uint32_t ready_tx_errors;
    uint32_t last_ready_tx_ms;
    uint32_t last_uart_rx_ms;
    uint32_t last_uart_recovery_ms;
    uint32_t last_valid_frame_ms;
    uint32_t last_status_ms;
    uint16_t last_transport_sequence;
} gateway_stats_t;

typedef struct {
    int16_t *samples;
    uint8_t *seen;
    uint32_t sequence;
    uint16_t received;
    uint16_t width;
    uint16_t height;
    uint32_t updated_ms;
    bool pending;
} matrix_state_t;

typedef struct {
    uint32_t sequence;
    uint32_t ra8_tick;
    uint32_t window_ms;
    uint32_t cpu0_usage_permille;
    uint32_t cpu1_usage_permille;
    uint32_t npu_usage_permille;
    uint32_t cpu0_heap_free;
    uint32_t cpu0_heap_min;
    uint32_t cpu1_heap_free;
    uint32_t cpu1_heap_min;
    uint32_t npu_busy_ms;
    uint32_t inference_count;
    uint32_t vision_total_ms;
    uint32_t cpu1_tick;
    uint32_t sampled_ms;
    uint8_t flags;
    bool valid;
} ra8_resource_state_t;

typedef enum {
    WS_RECOVERY_NONE = 0,
    WS_RECOVERY_SEND_FAILED,
    WS_RECOVERY_STALLED,
} ws_recovery_reason_t;

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_status_mutex;
static SemaphoreHandle_t s_uart_tx_mutex;
static SemaphoreHandle_t s_matrix_mutex;
static SemaphoreHandle_t s_story_bulk_uart_mutex;
static QueueHandle_t s_event_queue;
static QueueHandle_t s_command_queue;
static QueueHandle_t s_command_ack_queue;
static QueueHandle_t s_story_queue;
static QueueHandle_t s_story_result_queue;
static esp_websocket_client_handle_t s_ws;
static volatile bool s_ws_connected;
static volatile bool s_hello_pending;
static esp_websocket_client_handle_t s_matrix_ws;
static volatile bool s_matrix_ws_connected;
static volatile uint32_t s_server_acked_event;
static gateway_stats_t s_stats = {.uart_rx_pin_level = -1};
static char s_latest_status[RA8_MAX_PAYLOAD + 1] = "{}";
static ra8_resource_state_t s_ra8_resources;
static uint16_t s_uart_tx_sequence = 1;
static uint32_t s_telemetry_sequence = 1;
static uint32_t s_last_command_id;
static volatile uint32_t s_story_pending_command_id;
static volatile uint32_t s_story_transfer_id;
static volatile uint32_t s_story_bytes;
static volatile uint32_t s_story_attempts;
static volatile uint32_t s_story_completed;
static volatile uint32_t s_story_failures;
static volatile uint32_t s_story_last_result;
static volatile uint32_t s_story_last_status;
static volatile uint32_t s_story_exchange_retries;
static volatile uint32_t s_story_exchange_timeouts;
static volatile int32_t s_story_download_error;
static volatile uint32_t s_story_http_status;
static volatile uint32_t s_story_received_bytes;
static volatile uint32_t s_story_media_suspend_failures;
static volatile uint32_t s_story_bulk_handshake_successes;
static volatile uint32_t s_story_bulk_handshake_failures;
static volatile uint32_t s_story_bulk_fallbacks;
static volatile uint32_t s_story_bulk_last_fallback_reason;
static volatile uint32_t s_story_bulk_ready_frames_seen;
static volatile uint32_t s_story_bulk_probe_successes;
static volatile uint32_t s_story_bulk_probe_failures;
static volatile uint32_t s_story_bulk_uart_rx_bytes;
static volatile uint32_t s_story_bulk_uart_tx_bytes;
static volatile uint32_t s_story_bulk_actual_baud;
static volatile uint32_t s_story_bulk_last_rx_ms;
static volatile bool s_story_bulk_link_up;
static volatile bool s_story_active;
static volatile uint32_t s_story_finished_ms;
static volatile uint32_t s_priority_tx_guard_timeouts;
static matrix_state_t s_thermal;
static matrix_state_t s_pressure;
static char *s_ws_text;
static size_t s_ws_text_capacity;
static size_t s_ws_text_length;
static uint32_t s_ws_text_expected;
static esp_http_client_handle_t s_http;
static volatile bool s_http_connected;
static bool s_hello_sent;
static uint32_t s_http_requests;
static uint32_t s_http_failures;
static uint32_t s_last_http_ms;
static uint32_t s_max_http_ms;
static uint32_t s_ws_requests;
static uint32_t s_ws_failures;
static uint32_t s_last_ws_ms;
static uint32_t s_max_ws_ms;
static volatile uint32_t s_last_ws_success_ms;
static volatile uint32_t s_ws_recovery_started_ms;
static volatile uint32_t s_ws_recovery_grace_until_ms;
static volatile uint32_t s_ws_recovery_retry_after_ms;
static volatile uint32_t s_ws_disconnected_since_ms;
static volatile uint32_t s_ws_connected_since_ms;
static volatile bool s_ws_watchdog_armed;
static volatile bool s_ws_recovery_requested;
static volatile bool s_ws_recovery_running;
static volatile bool s_ws_recovery_timeout_reported;
static volatile ws_recovery_reason_t s_ws_recovery_reason;
static uint32_t s_ws_recovery_count;
static uint32_t s_ws_recovery_failures;
static uint32_t s_ws_disconnect_count;
static uint32_t s_ws_error_count;
static uint32_t s_ws_peer_close_count;
static uint32_t s_wifi_disconnect_count;
RTC_DATA_ATTR static uint32_t s_ws_self_restart_attempts;
static uint32_t s_event_ws_attempts;
static uint32_t s_event_ws_failures;
static uint32_t s_command_ack_ws_failures;
#if CONFIG_IDF_TARGET_ESP32P4
static char s_hosted_coprocessor_version[32] = "unknown";
static bool s_hosted_version_compatible;
static esp_err_t s_hosted_fw_query_error = ESP_ERR_NOT_FINISHED;
static uint32_t s_hosted_coprocessor_major;
static uint32_t s_hosted_coprocessor_minor;
static uint32_t s_hosted_ota_attempts;
static uint32_t s_hosted_ota_bytes;
static esp_err_t s_hosted_ota_result = ESP_ERR_NOT_FINISHED;
static bool s_hosted_ota_performed;
static uint32_t s_hosted_ota_applied_revision;
#endif
static uint32_t s_matrix_requests;
static uint32_t s_matrix_failures;
static uint32_t s_last_matrix_ms;
static uint32_t s_max_matrix_ms;
static uint32_t s_pressure_upload_sequence = 1;
static uint32_t s_thermal_upload_sequence = 1;
#if CONFIG_IDF_TARGET_ESP32P4
static uint32_t s_ra8_reset_started_ms;
static uint32_t s_ra8_reset_last_pulse_ms;
static uint32_t s_ra8_reset_pulse_count;
static bool s_ra8_reset_pin_ready;
#endif

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} http_capture_t;

static http_capture_t s_http_capture;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t elapsed_ms(uint32_t now, uint32_t then)
{
    int32_t delta = (int32_t)(now - then);
    return delta < 0 ? 0U : (uint32_t)delta;
}

static bool deadline_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint16_t crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t send_ra8_frame_tracked(uint8_t type, const uint8_t *payload, uint16_t length,
                                        uint16_t *sent_sequence)
{
    if (length > RA8_MAX_PAYLOAD || !s_uart_tx_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_uart_tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint16_t sequence = s_uart_tx_sequence;
    uint8_t header[8] = {
        0xAA, 0x55, RA8_PROTOCOL_VERSION, type,
        (uint8_t)(sequence >> 8), (uint8_t)sequence,
        (uint8_t)(length >> 8), (uint8_t)length,
    };
    uint16_t crc = crc16_ccitt_false(header, sizeof(header));
    for (uint16_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)payload[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    uint8_t trailer[2] = {(uint8_t)(crc >> 8), (uint8_t)crc};
    int written = uart_write_bytes(RA8_UART, header, sizeof(header));
    if (written == sizeof(header) && length) {
        written = uart_write_bytes(RA8_UART, payload, length);
    }
    if ((!length || written == length) && uart_write_bytes(RA8_UART, trailer, sizeof(trailer)) == sizeof(trailer)) {
        ++s_uart_tx_sequence;
        if (sent_sequence) *sent_sequence = sequence;
        xSemaphoreGive(s_uart_tx_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_uart_tx_mutex);
    return ESP_FAIL;
}

static esp_err_t send_ra8_frame(uint8_t type, const uint8_t *payload, uint16_t length)
{
    return send_ra8_frame_tracked(type, payload, length, NULL);
}

static void reset_matrix(matrix_state_t *matrix, uint32_t sequence)
{
    matrix->sequence = sequence;
    matrix->received = 0;
    matrix->pending = false;
    memset(matrix->seen, 0, ((size_t)matrix->width * matrix->height + 7U) / 8U);
}

static void accept_matrix_chunk(uint8_t type, const uint8_t *payload, uint16_t length)
{
    if (length < 12 || !s_matrix_mutex) {
        return;
    }
    uint32_t sequence = read_be32(payload);
    uint16_t width = read_be16(payload + 4);
    uint16_t height = read_be16(payload + 6);
    uint16_t offset = read_be16(payload + 8);
    uint16_t count = read_be16(payload + 10);
    uint32_t total = (uint32_t)width * height;
    matrix_state_t *matrix = NULL;
    if (type == RA8_TYPE_THERMAL_CHUNK && width == 80 && height == 64) {
        matrix = &s_thermal;
    } else if (type == RA8_TYPE_PRESSURE_UPLOAD && width == 16 && height == 16) {
        matrix = &s_pressure;
    }
    if (!matrix || total != (uint32_t)matrix->width * matrix->height ||
        length != 12U + count * 2U || (uint32_t)offset + count > total) {
        return;
    }
    if (xSemaphoreTake(s_matrix_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    if (matrix->sequence != sequence) {
        reset_matrix(matrix, sequence);
    }
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t index = offset + i;
        uint8_t mask = (uint8_t)(1U << (index & 7));
        if (!(matrix->seen[index >> 3] & mask)) {
            matrix->seen[index >> 3] |= mask;
            ++matrix->received;
        }
        matrix->samples[index] = (int16_t)read_be16(payload + 12 + i * 2);
    }
    matrix->pending = matrix->received == total;
    if (matrix->pending) matrix->updated_ms = now_ms();
    xSemaphoreGive(s_matrix_mutex);
}

static uint32_t json_event_id(const uint8_t *payload, uint16_t length)
{
    cJSON *root = cJSON_ParseWithLength((const char *)payload, length);
    if (!root) {
        return 0;
    }
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "event");
    if (!cJSON_IsNumber(id)) {
        id = cJSON_GetObjectItemCaseSensitive(root, "event_id");
    }
    uint32_t value = cJSON_IsNumber(id) && id->valuedouble > 0 ? (uint32_t)id->valuedouble : 0;
    cJSON_Delete(root);
    return value;
}

static bool event_already_queued(uint32_t event_id)
{
    safety_event_t item;
    UBaseType_t count = uxQueueMessagesWaiting(s_event_queue);
    bool found = false;
    for (UBaseType_t i = 0; i < count; ++i) {
        if (xQueueReceive(s_event_queue, &item, 0) != pdTRUE) {
            break;
        }
        if (item.event_id == event_id) {
            found = true;
        }
        xQueueSend(s_event_queue, &item, 0);
    }
    return found;
}

static void handle_ra8_frame(uint8_t type, uint16_t sequence, const uint8_t *payload, uint16_t length)
{
    ++s_stats.valid_frames;
    s_stats.last_valid_frame_ms = now_ms();
    s_stats.last_transport_sequence = sequence;
    if (type == RA8_TYPE_STATUS_UPLOAD) {
        if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            memcpy(s_latest_status, payload, length);
            s_latest_status[length] = '\0';
            xSemaphoreGive(s_status_mutex);
        }
        s_stats.last_status_ms = now_ms();
        ++s_stats.status_frames;
        return;
    }
    if (type == RA8_TYPE_RESOURCE_UPLOAD) {
        if (length != 56U || payload[0] != 1U || read_be16(payload + 2) != 56U) {
            ++s_stats.length_errors;
            return;
        }
        ra8_resource_state_t resource = {
            .sequence = read_be32(payload + 4),
            .ra8_tick = read_be32(payload + 8),
            .window_ms = read_be32(payload + 12),
            .cpu0_usage_permille = read_be16(payload + 16),
            .cpu1_usage_permille = read_be16(payload + 18),
            .npu_usage_permille = read_be16(payload + 20),
            .cpu0_heap_free = read_be32(payload + 24),
            .cpu0_heap_min = read_be32(payload + 28),
            .cpu1_heap_free = read_be32(payload + 32),
            .cpu1_heap_min = read_be32(payload + 36),
            .npu_busy_ms = read_be32(payload + 40),
            .inference_count = read_be32(payload + 44),
            .vision_total_ms = read_be32(payload + 48),
            .cpu1_tick = read_be32(payload + 52),
            .sampled_ms = now_ms(),
            .flags = payload[1],
            .valid = true,
        };
        if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_ra8_resources = resource;
            xSemaphoreGive(s_status_mutex);
        }
        return;
    }
    if (type == RA8_TYPE_CMD_RESULT && length == 16U && payload[0] == STORY_PROTOCOL_VERSION &&
        payload[1] >= STORY_OPERATION_BEGIN && payload[1] <= STORY_OPERATION_END) {
        story_result_t result = {
            .operation = payload[1],
            .result = payload[2],
            .status = payload[3],
            .transfer_id = read_be32(payload + 4),
            .next_offset = read_be32(payload + 8),
            .request_sequence = read_be16(payload + 12),
        };
        if (s_story_result_queue && xQueueSend(s_story_result_queue, &result, 0) != pdTRUE) {
            story_result_t discarded;
            (void)xQueueReceive(s_story_result_queue, &discarded, 0);
            (void)xQueueSend(s_story_result_queue, &result, 0);
        }
        return;
    }
    if (type == RA8_TYPE_THERMAL_CHUNK || type == RA8_TYPE_PRESSURE_UPLOAD) {
        accept_matrix_chunk(type, payload, length);
        return;
    }
    if (type != RA8_TYPE_SAFETY_EVENT) {
        return;
    }
    uint32_t event_id = json_event_id(payload, length);
    if (!event_id) {
        return;
    }
    bool queued = event_already_queued(event_id);
    if (!queued) {
        safety_event_t event = {.event_id = event_id};
        event.length = length < sizeof(event.json) - 1 ? length : sizeof(event.json) - 1;
        memcpy(event.json, payload, event.length);
        event.json[event.length] = '\0';
        queued = xQueueSend(s_event_queue, &event, 0) == pdTRUE;
    }
    if (queued) {
        uint8_t ack[4] = {(uint8_t)(event_id >> 24), (uint8_t)(event_id >> 16),
                          (uint8_t)(event_id >> 8), (uint8_t)event_id};
        send_ra8_frame(RA8_TYPE_SAFETY_ACK, ack, sizeof(ack));
        ESP_LOGI(TAG, "RA8 safety event %lu queued and ACKed", (unsigned long)event_id);
    } else {
        ++s_stats.event_drops;
        ESP_LOGW(TAG, "event queue full; RA8 event %lu left unacknowledged", (unsigned long)event_id);
    }
}

static esp_err_t configure_ra8_uart_driver(void)
{
    uart_config_t config = {
        .baud_rate = RA8_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t result = uart_param_config(RA8_UART, &config);
    if (result != ESP_OK) {
        return result;
    }
    result = uart_set_pin(RA8_UART, RA8_UART_TX_PIN, RA8_UART_RX_PIN,
                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (result != ESP_OK) {
        return result;
    }
    return uart_driver_install(RA8_UART, 16384, 0, 0, NULL, 0);
}

static esp_err_t recover_ra8_uart_rx(void)
{
    if (!s_uart_tx_mutex ||
        xSemaphoreTake(s_uart_tx_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* No valid frame can remain after the silence threshold.  Even if a stale
     * TX descriptor no longer reports completion, deleting the driver is the
     * recovery operation rather than a reason to leave the UART wedged. */
    (void)uart_wait_tx_done(RA8_UART, pdMS_TO_TICKS(100));
    esp_err_t result = uart_driver_delete(RA8_UART);
    if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
        result = configure_ra8_uart_driver();
    }

    xSemaphoreGive(s_uart_tx_mutex);
    return result;
}

static void uart_rx_task(void *arg)
{
    (void)arg;
    enum { WAIT_AA, WAIT_55, READ_HEADER, READ_PAYLOAD, READ_CRC_HI, READ_CRC_LO } state = WAIT_AA;
    uint8_t header[8] = {0};
    uint8_t payload[RA8_MAX_PAYLOAD];
    uint8_t header_pos = 0;
    uint16_t payload_length = 0;
    uint16_t payload_pos = 0;
    uint16_t received_crc = 0;
    uint8_t input[1024];
    uint32_t last_ready_ms = 0;
    uint32_t started_ms = now_ms();
    for (;;) {
        uint32_t now = now_ms();
        if (!last_ready_ms || now - last_ready_ms >= RA8_READY_INTERVAL_MS) {
            static const uint8_t ready[] = "gateway_ready";
            last_ready_ms = now;
            if (send_ra8_frame(RA8_TYPE_CMD, ready, sizeof(ready) - 1U) == ESP_OK) {
                ++s_stats.ready_tx_count;
                s_stats.last_ready_tx_ms = now;
            } else {
                ++s_stats.ready_tx_errors;
            }
        }
        int count = uart_read_bytes(RA8_UART, input, sizeof(input), pdMS_TO_TICKS(20));
        uint32_t sampled_ms = now_ms();
        int32_t rx_pin_level = gpio_get_level((gpio_num_t)RA8_UART_RX_PIN);
        if (s_stats.uart_rx_pin_level >= 0 &&
            s_stats.uart_rx_pin_level != rx_pin_level) {
            ++s_stats.uart_rx_pin_transitions;
        }
        s_stats.uart_rx_pin_level = rx_pin_level;

        if (count > 0) {
            s_stats.uart_rx_bytes += (uint32_t)count;
            s_stats.last_uart_rx_ms = sampled_ms;
        } else if (count < 0) {
            ++s_stats.uart_read_errors;
        }

        for (int n = 0; n < count; ++n) {
            uint8_t value = input[n];
            switch (state) {
            case WAIT_AA:
                if (value == 0xAA) state = WAIT_55;
                break;
            case WAIT_55:
                if (value == 0x55) {
                    header[0] = 0xAA;
                    header[1] = 0x55;
                    header_pos = 2;
                    state = READ_HEADER;
                } else {
                    state = value == 0xAA ? WAIT_55 : WAIT_AA;
                }
                break;
            case READ_HEADER:
                header[header_pos++] = value;
                if (header_pos == sizeof(header)) {
                    payload_length = read_be16(header + 6);
                    payload_pos = 0;
                    if (payload_length > RA8_MAX_PAYLOAD) {
                        ++s_stats.length_errors;
                        state = WAIT_AA;
                    } else {
                        state = payload_length ? READ_PAYLOAD : READ_CRC_HI;
                    }
                }
                break;
            case READ_PAYLOAD:
                payload[payload_pos++] = value;
                if (payload_pos == payload_length) state = READ_CRC_HI;
                break;
            case READ_CRC_HI:
                received_crc = (uint16_t)value << 8;
                state = READ_CRC_LO;
                break;
            case READ_CRC_LO: {
                received_crc |= value;
                uint16_t crc = crc16_ccitt_false(header, sizeof(header));
                for (uint16_t i = 0; i < payload_length; ++i) {
                    crc ^= (uint16_t)payload[i] << 8;
                    for (unsigned bit = 0; bit < 8; ++bit) {
                        crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
                    }
                }
                if (crc == received_crc && header[2] == RA8_PROTOCOL_VERSION) {
                    handle_ra8_frame(header[3], read_be16(header + 4), payload, payload_length);
                } else {
                    ++s_stats.crc_errors;
                }
                state = WAIT_AA;
                break;
            }
        }

        uint32_t rx_silence_ms = s_stats.last_uart_rx_ms ?
                                 elapsed_ms(sampled_ms, s_stats.last_uart_rx_ms) :
                                 elapsed_ms(sampled_ms, started_ms);
        uint32_t recovery_age_ms = s_stats.last_uart_recovery_ms ?
                                   elapsed_ms(sampled_ms, s_stats.last_uart_recovery_ms) :
                                   UINT32_MAX;
        if (last_ready_ms && rx_silence_ms >= RA8_UART_RX_RECOVERY_MS &&
            recovery_age_ms >= RA8_UART_RX_RECOVERY_COOLDOWN_MS) {
            s_stats.last_uart_recovery_ms = sampled_ms;
            esp_err_t recovery = recover_ra8_uart_rx();
            state = WAIT_AA;
            header_pos = 0;
            payload_length = 0;
            payload_pos = 0;
            received_crc = 0;
            if (recovery == ESP_OK) {
                ++s_stats.uart_rx_recovery_count;
                ESP_LOGW(TAG,
                         "RA8 UART RX reinstalled after %lums silence; pin=%ld transitions=%lu",
                         (unsigned long)rx_silence_ms,
                         (long)s_stats.uart_rx_pin_level,
                         (unsigned long)s_stats.uart_rx_pin_transitions);
            } else {
                ++s_stats.uart_rx_recovery_failures;
                ESP_LOGE(TAG, "RA8 UART RX recovery failed: %s",
                         esp_err_to_name(recovery));
            }
        }
    }
}
}

static const char *ws_recovery_reason_name(ws_recovery_reason_t reason)
{
    switch (reason) {
    case WS_RECOVERY_SEND_FAILED:
        return "send_failed";
    case WS_RECOVERY_STALLED:
        return "telemetry_stalled";
    default:
        return "unknown";
    }
}

static void request_telemetry_ws_recovery(ws_recovery_reason_t reason)
{
    if (!s_ws_recovery_requested && !s_ws_recovery_running) {
        s_ws_recovery_reason = reason;
        s_ws_recovery_requested = true;
    }
}

static void telemetry_ws_recovery_task(void *arg)
{
    ws_recovery_reason_t reason = (ws_recovery_reason_t)(intptr_t)arg;
    ESP_LOGW(TAG, "restarting telemetry WSS, reason=%s",
             ws_recovery_reason_name(reason));

    s_ws_connected = false;
    s_hello_pending = true;
    esp_err_t stop_result = esp_websocket_client_stop(s_ws);
    if (stop_result != ESP_OK) {
        ESP_LOGW(TAG, "telemetry WSS stop returned %s",
                 esp_err_to_name(stop_result));
    }

    esp_err_t start_result = esp_websocket_client_start(s_ws);
    if (start_result != ESP_OK) {
        ++s_ws_recovery_failures;
        s_ws_recovery_retry_after_ms = now_ms() + TELEMETRY_RECOVERY_RETRY_MS;
        s_ws_recovery_requested = true;
        ESP_LOGE(TAG, "telemetry WSS restart failed: %s; retrying later",
                 esp_err_to_name(start_result));
    } else {
        s_ws_recovery_retry_after_ms = 0;
    }

    s_ws_recovery_timeout_reported = false;
    s_ws_recovery_running = false;
    vTaskDelete(NULL);
}

static void start_telemetry_ws_recovery(uint32_t now)
{
    ws_recovery_reason_t reason = s_ws_recovery_reason;
    s_ws_recovery_requested = false;
    s_ws_recovery_running = true;
    s_ws_recovery_timeout_reported = false;
    s_ws_recovery_started_ms = now;
    s_ws_recovery_grace_until_ms = now + TELEMETRY_RECOVERY_GRACE_MS;
    ++s_ws_recovery_count;

    if (xTaskCreatePinnedToCore(telemetry_ws_recovery_task, "telemetry_recover",
                                4096, (void *)(intptr_t)reason, 7, NULL, 0) != pdPASS) {
        ++s_ws_recovery_failures;
        s_ws_recovery_running = false;
        s_ws_recovery_requested = true;
        s_ws_recovery_retry_after_ms = now + TELEMETRY_RECOVERY_RETRY_MS;
        ESP_LOGE(TAG, "telemetry WSS recovery task creation failed; retrying later");
    }
}

static bool websocket_send_json(const char *kind, cJSON *payload)
{
    if (!s_ws_connected || !s_ws || !kind || !payload) {
        return false;
    }
    if (!esp_websocket_client_is_connected(s_ws)) {
        s_ws_connected = false;
        if (!s_ws_disconnected_since_ms) s_ws_disconnected_since_ms = now_ms();
        return false;
    }
    cJSON *envelope = cJSON_CreateObject();
    if (!envelope) return false;
    cJSON_AddStringToObject(envelope, "kind", kind);
    cJSON_AddItemReferenceToObject(envelope, "payload", payload);
    char *text = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (!text) return false;
    int length = (int)strlen(text);
    uint32_t started = now_ms();
    /* Every telemetry frame supersedes the previous one. Never queue behind a
     * slow media/matrix send; drop this sample and retry on the next 250 ms tick. */
    if (!realtime_tx_guard_take(0)) {
        ++s_priority_tx_guard_timeouts;
        free(text);
        return false;
    }
    int sent = esp_websocket_client_send_text(s_ws, text, length, pdMS_TO_TICKS(300));
    uint32_t elapsed = now_ms() - started;
    realtime_tx_guard_give();
    s_last_ws_ms = elapsed;
    if (elapsed > s_max_ws_ms) s_max_ws_ms = elapsed;
    ++s_ws_requests;
    if (sent != length) {
        ++s_ws_failures;
        s_ws_recovery_grace_until_ms = now_ms() + TELEMETRY_RECOVERY_GRACE_MS;
    } else {
        s_last_ws_success_ms = now_ms();
        s_ws_watchdog_armed = true;
        s_ws_recovery_grace_until_ms = 0;
    }
    free(text);
    return sent == length;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && s_http_capture.buffer && event->data_len > 0) {
        size_t available = s_http_capture.capacity > s_http_capture.length ?
                           s_http_capture.capacity - s_http_capture.length - 1U : 0U;
        size_t copy = (size_t)event->data_len < available ? (size_t)event->data_len : available;
        if (copy) {
            memcpy(s_http_capture.buffer + s_http_capture.length, event->data, copy);
            s_http_capture.length += copy;
            s_http_capture.buffer[s_http_capture.length] = '\0';
        }
    }
    return ESP_OK;
}

static int http_post(const char *path, const void *data, size_t length,
                     const char *content_type, char *response, size_t response_capacity)
{
    if (!s_http || !path || !data) return -1;
    char url[192];
    snprintf(url, sizeof(url), SERVER_ORIGIN "%s", path);
    s_http_capture = (http_capture_t){.buffer = response, .capacity = response_capacity, .length = 0};
    if (response && response_capacity) response[0] = '\0';
    esp_http_client_set_url(s_http, url);
    esp_http_client_set_method(s_http, HTTP_METHOD_POST);
    esp_http_client_set_header(s_http, "X-Device-ID", GATEWAY_DEVICE_ID);
    esp_http_client_set_header(s_http, "X-Device-Key", GATEWAY_DEVICE_KEY);
    esp_http_client_set_header(s_http, "Content-Type", content_type ? content_type : "application/json");
    esp_http_client_set_post_field(s_http, (const char *)data, (int)length);
    uint32_t started = now_ms();
    esp_err_t result = esp_http_client_perform(s_http);
    uint32_t elapsed = now_ms() - started;
    s_last_http_ms = elapsed;
    if (elapsed > s_max_http_ms) s_max_http_ms = elapsed;
    ++s_http_requests;
    int status = result == ESP_OK ? esp_http_client_get_status_code(s_http) : -1;
    s_http_connected = status >= 200 && status < 300;
    if (!s_http_connected) {
        ++s_http_failures;
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "HTTP %s failed: %s", path, esp_err_to_name(result));
            esp_http_client_close(s_http);
        } else {
            ESP_LOGW(TAG, "HTTP %s returned %d", path, status);
        }
    }
    s_http_capture = (http_capture_t){0};
    return status;
}

static cJSON *create_hello_payload(void)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) return NULL;
    cJSON_AddNumberToObject(body, "protocol_version", 1);
    cJSON_AddStringToObject(body, "firmware", FIRMWARE_VERSION);
    cJSON_AddStringToObject(body, "hardware", HARDWARE_NAME);
    cJSON *caps = cJSON_AddArrayToObject(body, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("ra8_gateway"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("thermal"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("pressure"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("environment_scd41"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("environment_bme688"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("safety_events"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("ra8_audio_collection"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("ra8_camera_collection"));
    cJSON_AddItemToArray(caps, cJSON_CreateString(MICROPHONE_CAPABILITY));
    cJSON_AddItemToArray(caps, cJSON_CreateString(CAMERA_CAPABILITY));
    cJSON_AddItemToArray(caps, cJSON_CreateString("telemetry_wss"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("matrix_wss"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("batch_v2"));
    cJSON *meta = cJSON_AddObjectToObject(body, "metadata");
    esp_netif_ip_info_t ip = {0};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
        char address[16];
        snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(meta, "local_ip", address);
        snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.gw));
        cJSON_AddStringToObject(meta, "gateway_ip", address);
        snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.netmask));
        cJSON_AddStringToObject(meta, "subnet_mask", address);
    }
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        char mac_text[18];
        snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(meta, "mac", mac_text);
    }
    cJSON_AddNumberToObject(meta, "uart_rx_gpio", RA8_UART_RX_PIN);
    cJSON_AddNumberToObject(meta, "uart_tx_gpio", RA8_UART_TX_PIN);
#if CONFIG_IDF_TARGET_ESP32P4
    cJSON_AddNumberToObject(meta, "ra8_soft_reset_gpio", RA8_SOFT_RESET_PIN);
    cJSON_AddStringToObject(meta, "esp_hosted_host_version",
                            GATEWAY_STRINGIFY(ESP_HOSTED_VERSION_MAJOR_1) "."
                            GATEWAY_STRINGIFY(ESP_HOSTED_VERSION_MINOR_1) "."
                            GATEWAY_STRINGIFY(ESP_HOSTED_VERSION_PATCH_1));
    cJSON_AddStringToObject(meta, "esp_hosted_coprocessor_version",
                            s_hosted_coprocessor_version);
    cJSON_AddBoolToObject(meta, "esp_hosted_version_compatible",
                          s_hosted_version_compatible);
    cJSON_AddStringToObject(meta, "esp_hosted_ota_expected_version",
                            C6_OTA_EXPECTED_VERSION);
    cJSON_AddNumberToObject(meta, "esp_hosted_ota_attempts",
                            s_hosted_ota_attempts);
    cJSON_AddNumberToObject(meta, "esp_hosted_ota_result",
                            s_hosted_ota_result);
    cJSON_AddNumberToObject(meta, "esp_hosted_ota_image_revision",
                            C6_OTA_IMAGE_REVISION);
    cJSON_AddNumberToObject(meta, "esp_hosted_ota_applied_revision",
                            s_hosted_ota_applied_revision);
#ifdef CONFIG_ESP_HOSTED_SDIO_CHECKSUM
    cJSON_AddBoolToObject(meta, "esp_hosted_sdio_checksum", true);
#else
    cJSON_AddBoolToObject(meta, "esp_hosted_sdio_checksum", false);
#endif
#endif
    cJSON_AddBoolToObject(meta, "media_enabled", true);
    cJSON_AddStringToObject(meta, "transport", "independent_telemetry_matrix_wss");
    cJSON_AddStringToObject(meta, "camera_format", "JPEG");
    cJSON_AddStringToObject(meta, "camera_default", CAMERA_DEFAULT);
    return body;
}

static bool send_hello(void)
{
    cJSON *body = create_hello_payload();
    if (!body) return false;
    char *json = cJSON_PrintUnformatted(body);
    bool ok = json && http_post("/api/v1/device/hello", json, strlen(json),
                                "application/json", NULL, 0) >= 200;
    free(json);
    cJSON_Delete(body);
    ESP_LOGI(TAG, "HTTP hello %s", ok ? "sent" : "failed");
    return ok;
}

static bool send_websocket_hello(void)
{
    cJSON *body = create_hello_payload();
    if (!body) return false;
    bool ok = websocket_send_json("hello", body);
    cJSON_Delete(body);
    if (ok) {
        s_hello_pending = false;
        ESP_LOGI(TAG, "telemetry WSS hello sent");
    }
    return ok;
}

static void capture_pressure_from_status(cJSON *pressure)
{
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(pressure, "seq");
    cJSON *values = cJSON_GetObjectItemCaseSensitive(pressure, "matrix");
    if (!cJSON_IsNumber(seq) || !cJSON_IsArray(values) || cJSON_GetArraySize(values) != 256) return;
    uint32_t sequence = (uint32_t)seq->valuedouble;
    if (!sequence || xSemaphoreTake(s_matrix_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (s_pressure.sequence != sequence) {
        for (int i = 0; i < 256; ++i) {
            cJSON *value = cJSON_GetArrayItem(values, i);
            s_pressure.samples[i] = cJSON_IsNumber(value) ? (int16_t)value->valueint : 0;
        }
        s_pressure.sequence = sequence;
        s_pressure.received = 256;
        s_pressure.updated_ms = now_ms();
        s_pressure.pending = true;
    }
    xSemaphoreGive(s_matrix_mutex);
}

static void add_duplicate_if_object(cJSON *destination, const char *name, cJSON *source)
{
    if (cJSON_IsObject(source)) {
        cJSON_AddItemToObject(destination, name, cJSON_Duplicate(source, true));
    }
}

static size_t sanitize_utf8_in_place(char *text)
{
    if (!text) return 0;
    uint8_t *bytes = (uint8_t *)text;
    size_t length = strlen(text);
    size_t replaced = 0;

    for (size_t i = 0; i < length;) {
        uint8_t first = bytes[i];
        size_t sequence_length = 0;
        bool valid = false;

        if (first <= 0x7f) {
            ++i;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            sequence_length = 2;
            valid = i + 1 < length &&
                    bytes[i + 1] >= 0x80 && bytes[i + 1] <= 0xbf;
        } else if (first == 0xe0) {
            sequence_length = 3;
            valid = i + 2 < length &&
                    bytes[i + 1] >= 0xa0 && bytes[i + 1] <= 0xbf &&
                    bytes[i + 2] >= 0x80 && bytes[i + 2] <= 0xbf;
        } else if ((first >= 0xe1 && first <= 0xec) ||
                   (first >= 0xee && first <= 0xef)) {
            sequence_length = 3;
            valid = i + 2 < length &&
                    bytes[i + 1] >= 0x80 && bytes[i + 1] <= 0xbf &&
                    bytes[i + 2] >= 0x80 && bytes[i + 2] <= 0xbf;
        } else if (first == 0xed) {
            sequence_length = 3;
            valid = i + 2 < length &&
                    bytes[i + 1] >= 0x80 && bytes[i + 1] <= 0x9f &&
                    bytes[i + 2] >= 0x80 && bytes[i + 2] <= 0xbf;
        } else if (first == 0xf0) {
            sequence_length = 4;
            valid = i + 3 < length &&
                    bytes[i + 1] >= 0x90 && bytes[i + 1] <= 0xbf &&
                    bytes[i + 2] >= 0x80 && bytes[i + 2] <= 0xbf &&
                    bytes[i + 3] >= 0x80 && bytes[i + 3] <= 0xbf;
        } else if (first >= 0xf1 && first <= 0xf3) {
            sequence_length = 4;
            valid = i + 3 < length &&
                    bytes[i + 1] >= 0x80 && bytes[i + 1] <= 0xbf &&
                    bytes[i + 2] >= 0x80 && bytes[i + 2] <= 0xbf &&
                    bytes[i + 3] >= 0x80 && bytes[i + 3] <= 0xbf;
        } else if (first == 0xf4) {
            sequence_length = 4;
            valid = i + 3 < length &&
                    bytes[i + 1] >= 0x80 && bytes[i + 1] <= 0x8f &&
                    bytes[i + 2] >= 0x80 && bytes[i + 2] <= 0xbf &&
                    bytes[i + 3] >= 0x80 && bytes[i + 3] <= 0xbf;
        }

        if (valid) {
            i += sequence_length;
        } else {
            bytes[i++] = '?';
            ++replaced;
        }
    }
    return replaced;
}

static char *build_telemetry_json(void)
{
    char status[RA8_MAX_PAYLOAD + 1];
    ra8_resource_state_t resources = {0};
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(status, s_latest_status, sizeof(status));
        status[sizeof(status) - 1] = '\0';
        resources = s_ra8_resources;
        xSemaphoreGive(s_status_mutex);
    } else {
        strcpy(status, "{}");
    }
    cJSON *ra8 = cJSON_Parse(status);
    if (!ra8) ra8 = cJSON_CreateObject();
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "protocol_version", 1);
    cJSON_AddNumberToObject(body, "sequence", s_telemetry_sequence++);
    cJSON_AddNumberToObject(body, "device_time_ms", now_ms());
    cJSON *tick = cJSON_GetObjectItemCaseSensitive(ra8, "tick");
    cJSON_AddNumberToObject(body, "ra8_tick", cJSON_IsNumber(tick) ? tick->valuedouble : 0);
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(body, "wifi_rssi", ap.rssi);
    cJSON_AddStringToObject(body, "firmware", FIRMWARE_VERSION);

    cJSON *sensors = cJSON_AddObjectToObject(body, "sensors");
    cJSON *thm = cJSON_GetObjectItemCaseSensitive(ra8, "thm");
    cJSON *tmp = cJSON_GetObjectItemCaseSensitive(ra8, "tmp");
    cJSON *scd = cJSON_GetObjectItemCaseSensitive(ra8, "scd");
    cJSON *bme = cJSON_GetObjectItemCaseSensitive(ra8, "bme");
    cJSON *prs = cJSON_GetObjectItemCaseSensitive(ra8, "prs");
    cJSON *mic = cJSON_GetObjectItemCaseSensitive(ra8, "mic");
    cJSON *aud = cJSON_GetObjectItemCaseSensitive(ra8, "aud");
    cJSON *cam = cJSON_GetObjectItemCaseSensitive(ra8, "cam");
    cJSON *light = cJSON_GetObjectItemCaseSensitive(ra8, "light");
    cJSON *pat = cJSON_GetObjectItemCaseSensitive(ra8, "pat");
    add_duplicate_if_object(sensors, "thermal", thm);
    add_duplicate_if_object(sensors, "bed_temperature", tmp);
    if (cJSON_IsObject(scd) || cJSON_IsObject(bme)) {
        cJSON *environment = cJSON_IsObject(scd) ? cJSON_Duplicate(scd, true)
                                                 : cJSON_CreateObject();
        if (environment) {
            if (cJSON_IsObject(bme)) {
                cJSON *bme_copy = cJSON_Duplicate(bme, true);
                if (bme_copy) {
                    cJSON_DeleteItemFromObjectCaseSensitive(environment, "bme688");
                    cJSON_AddItemToObject(environment, "bme688", bme_copy);
                }
            }
            cJSON_AddItemToObject(sensors, "environment", environment);
        }
    }
    if (cJSON_IsObject(prs)) {
        capture_pressure_from_status(prs);
        cJSON *copy = cJSON_Duplicate(prs, true);
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "matrix");
        cJSON_AddItemToObject(sensors, "pressure", copy);
    }
    add_duplicate_if_object(sensors, "microphone", mic);
    add_duplicate_if_object(sensors, "audio_player", aud);
    add_duplicate_if_object(sensors, "camera", cam);
    add_duplicate_if_object(sensors, "warm_light", light);
    add_duplicate_if_object(sensors, "soother", pat);
    cJSON *fusion = cJSON_GetObjectItemCaseSensitive(ra8, "fusion");
    if (cJSON_IsObject(fusion)) cJSON_AddItemToObject(body, "inference", cJSON_Duplicate(fusion, true));
    else cJSON_AddObjectToObject(body, "inference");
    add_duplicate_if_object(body, "vision", cJSON_GetObjectItemCaseSensitive(ra8, "vision"));
    add_duplicate_if_object(body, "safety", cJSON_GetObjectItemCaseSensitive(ra8, "safe"));

    cJSON *health = cJSON_AddObjectToObject(body, "health");
    uint32_t health_now = now_ms();
    uint32_t age = s_stats.last_status_ms ?
                   elapsed_ms(health_now, s_stats.last_status_ms) : UINT32_MAX;
    uint32_t transport_age = s_stats.last_valid_frame_ms ?
                             elapsed_ms(health_now, s_stats.last_valid_frame_ms) : UINT32_MAX;
    uint32_t uart_rx_age = s_stats.last_uart_rx_ms ?
                           elapsed_ms(health_now, s_stats.last_uart_rx_ms) : UINT32_MAX;
    cJSON_AddBoolToObject(health, "ra8_online", s_stats.last_status_ms && age <= RA8_OFFLINE_MS);
    cJSON_AddNumberToObject(health, "ra8_status_age_ms", age);
    cJSON_AddBoolToObject(health, "ra8_transport_online",
                          s_stats.last_valid_frame_ms && transport_age <= RA8_OFFLINE_MS);
    cJSON_AddNumberToObject(health, "ra8_transport_age_ms", transport_age);
    cJSON_AddNumberToObject(health, "uart_valid_frames", s_stats.valid_frames);
    cJSON_AddNumberToObject(health, "uart_status_frames", s_stats.status_frames);
    cJSON_AddNumberToObject(health, "uart_crc_errors", s_stats.crc_errors);
    cJSON_AddNumberToObject(health, "uart_length_errors", s_stats.length_errors);
    cJSON_AddNumberToObject(health, "uart_rx_bytes", s_stats.uart_rx_bytes);
    cJSON_AddNumberToObject(health, "uart_raw_rx_age_ms", uart_rx_age);
    cJSON_AddNumberToObject(health, "uart_read_errors", s_stats.uart_read_errors);
    cJSON_AddNumberToObject(health, "uart_rx_recovery_count",
                            s_stats.uart_rx_recovery_count);
    cJSON_AddNumberToObject(health, "uart_rx_recovery_failures",
                            s_stats.uart_rx_recovery_failures);
    cJSON_AddNumberToObject(health, "uart_rx_pin_level", s_stats.uart_rx_pin_level);
    cJSON_AddNumberToObject(health, "uart_rx_pin_transitions",
                            s_stats.uart_rx_pin_transitions);
    cJSON_AddNumberToObject(health, "ready_tx_count", s_stats.ready_tx_count);
    cJSON_AddNumberToObject(health, "ready_tx_errors", s_stats.ready_tx_errors);
    cJSON_AddNumberToObject(
        health, "ready_tx_age_ms",
        s_stats.last_ready_tx_ms ?
            elapsed_ms(health_now, s_stats.last_ready_tx_ms) : UINT32_MAX);
    cJSON_AddNumberToObject(
        health, "uart_rx_recovery_age_ms",
        s_stats.last_uart_recovery_ms ?
            elapsed_ms(health_now, s_stats.last_uart_recovery_ms) : UINT32_MAX);
#if CONFIG_IDF_TARGET_ESP32P4
    cJSON_AddBoolToObject(health, "ra8_soft_reset_pin_ready", s_ra8_reset_pin_ready);
    cJSON_AddNumberToObject(health, "ra8_soft_reset_pulses", s_ra8_reset_pulse_count);
    cJSON_AddNumberToObject(health, "ra8_soft_reset_last_ms", s_ra8_reset_last_pulse_ms);
#endif
    cJSON_AddNumberToObject(health, "event_queue_drops", s_stats.event_drops);
    cJSON_AddBoolToObject(health, "ws_connected", s_ws_connected);
    cJSON_AddBoolToObject(health, "matrix_ws_connected", s_matrix_ws_connected);
    cJSON_AddBoolToObject(health, "batch_connected", false);
    cJSON_AddNumberToObject(health, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(health, "free_internal_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(health, "min_free_internal_heap",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(health, "largest_internal_block",
                            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(health, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (resources.valid) {
        cJSON *ra8_resources = cJSON_AddObjectToObject(health, "ra8_resources");
        cJSON_AddNumberToObject(ra8_resources, "sequence", resources.sequence);
        cJSON_AddNumberToObject(ra8_resources, "age_ms",
                                elapsed_ms(health_now, resources.sampled_ms));
        cJSON_AddNumberToObject(ra8_resources, "window_ms", resources.window_ms);
        cJSON_AddNumberToObject(ra8_resources, "flags", resources.flags);
        cJSON_AddNumberToObject(ra8_resources, "cpu0_usage_permille",
                                resources.cpu0_usage_permille);
        cJSON_AddNumberToObject(ra8_resources, "cpu1_usage_permille",
                                resources.cpu1_usage_permille);
        cJSON_AddNumberToObject(ra8_resources, "npu_usage_permille",
                                resources.npu_usage_permille);
        cJSON_AddNumberToObject(ra8_resources, "cpu0_heap_free",
                                resources.cpu0_heap_free);
        cJSON_AddNumberToObject(ra8_resources, "cpu0_heap_min",
                                resources.cpu0_heap_min);
        cJSON_AddNumberToObject(ra8_resources, "cpu1_heap_free",
                                resources.cpu1_heap_free);
        cJSON_AddNumberToObject(ra8_resources, "cpu1_heap_min",
                                resources.cpu1_heap_min);
        cJSON_AddNumberToObject(ra8_resources, "npu_busy_ms",
                                resources.npu_busy_ms);
        cJSON_AddNumberToObject(ra8_resources, "inference_count",
                                resources.inference_count);
        cJSON_AddNumberToObject(ra8_resources, "vision_total_ms",
                                resources.vision_total_ms);
        cJSON_AddNumberToObject(ra8_resources, "ra8_tick", resources.ra8_tick);
        cJSON_AddNumberToObject(ra8_resources, "cpu1_tick", resources.cpu1_tick);
    }
    add_duplicate_if_object(health, "safety", cJSON_GetObjectItemCaseSensitive(ra8, "safe"));
    add_duplicate_if_object(health, "ra8_link", cJSON_GetObjectItemCaseSensitive(ra8, "link"));
    cJSON *gateway = cJSON_AddObjectToObject(health, "gateway");
    cJSON_AddStringToObject(gateway, "transport", "independent_telemetry_matrix_wss");
    cJSON_AddNumberToObject(gateway, "uart_rx_bytes", s_stats.uart_rx_bytes);
    cJSON_AddNumberToObject(gateway, "ready_tx_count", s_stats.ready_tx_count);
    cJSON_AddNumberToObject(gateway, "ready_tx_errors", s_stats.ready_tx_errors);
    cJSON_AddNumberToObject(gateway, "ready_tx_age_ms",
                            s_stats.last_ready_tx_ms ?
                                elapsed_ms(health_now, s_stats.last_ready_tx_ms) : UINT32_MAX);
    cJSON_AddNumberToObject(gateway, "uart_active_baud", RA8_UART_BAUD);
    cJSON_AddBoolToObject(gateway, "uart_baud_locked", true);
    cJSON_AddNumberToObject(gateway, "last_http_ms", s_last_http_ms);
    cJSON_AddNumberToObject(gateway, "max_http_ms", s_max_http_ms);
    cJSON_AddNumberToObject(gateway, "http_requests", s_http_requests);
    cJSON_AddNumberToObject(gateway, "http_failures", s_http_failures);
    cJSON_AddNumberToObject(gateway, "last_ws_ms", s_last_ws_ms);
    cJSON_AddNumberToObject(gateway, "max_ws_ms", s_max_ws_ms);
    cJSON_AddNumberToObject(gateway, "ws_requests", s_ws_requests);
    cJSON_AddNumberToObject(gateway, "ws_failures", s_ws_failures);
    cJSON_AddNumberToObject(
        gateway, "last_ws_success_age_ms",
        s_ws_watchdog_armed ? elapsed_ms(health_now, s_last_ws_success_ms) : UINT32_MAX);
    cJSON_AddNumberToObject(gateway, "ws_recovery_count", s_ws_recovery_count);
    cJSON_AddNumberToObject(gateway, "ws_recovery_failures", s_ws_recovery_failures);
    cJSON_AddNumberToObject(gateway, "ws_disconnect_count", s_ws_disconnect_count);
    cJSON_AddNumberToObject(gateway, "ws_error_count", s_ws_error_count);
    cJSON_AddNumberToObject(gateway, "ws_peer_close_count", s_ws_peer_close_count);
    cJSON_AddNumberToObject(gateway, "ws_self_restart_attempts",
                           s_ws_self_restart_attempts);
    cJSON_AddNumberToObject(gateway, "wifi_disconnect_count", s_wifi_disconnect_count);
    cJSON_AddNumberToObject(
        gateway, "ws_disconnected_age_ms",
        s_ws_disconnected_since_ms ? elapsed_ms(health_now, s_ws_disconnected_since_ms) : 0);
    cJSON_AddNumberToObject(gateway, "event_ws_attempts", s_event_ws_attempts);
    cJSON_AddNumberToObject(gateway, "event_ws_failures", s_event_ws_failures);
    cJSON_AddNumberToObject(gateway, "command_ack_ws_failures", s_command_ack_ws_failures);
    cJSON_AddNumberToObject(gateway, "telemetry_ws_buffer_bytes",
                            TELEMETRY_WSS_BUFFER_BYTES);
    cJSON_AddNumberToObject(gateway, "matrix_ws_buffer_bytes",
                            MATRIX_WSS_BUFFER_BYTES);
    cJSON_AddBoolToObject(gateway, "story_active", s_story_active);
    cJSON_AddNumberToObject(gateway, "story_pending_command", s_story_pending_command_id);
    cJSON_AddNumberToObject(gateway, "story_transfer_id", s_story_transfer_id);
    cJSON_AddNumberToObject(gateway, "story_bytes", s_story_bytes);
    cJSON_AddNumberToObject(gateway, "story_attempts", s_story_attempts);
    cJSON_AddNumberToObject(gateway, "story_completed", s_story_completed);
    cJSON_AddNumberToObject(gateway, "story_failures", s_story_failures);
    cJSON_AddNumberToObject(gateway, "story_last_result", s_story_last_result);
    cJSON_AddNumberToObject(gateway, "story_last_status", s_story_last_status);
    cJSON_AddNumberToObject(gateway, "story_exchange_retries", s_story_exchange_retries);
    cJSON_AddNumberToObject(gateway, "story_exchange_timeouts", s_story_exchange_timeouts);
    cJSON_AddNumberToObject(gateway, "story_download_error", s_story_download_error);
    cJSON_AddNumberToObject(gateway, "story_http_status", s_story_http_status);
    cJSON_AddNumberToObject(gateway, "story_received_bytes", s_story_received_bytes);
    cJSON_AddNumberToObject(gateway, "story_media_suspend_failures",
                            s_story_media_suspend_failures);
    cJSON_AddNumberToObject(gateway, "story_bulk_handshake_successes",
                            s_story_bulk_handshake_successes);
    cJSON_AddNumberToObject(gateway, "story_bulk_handshake_failures",
                            s_story_bulk_handshake_failures);
    cJSON_AddNumberToObject(gateway, "story_bulk_fallbacks",
                            s_story_bulk_fallbacks);
    cJSON_AddNumberToObject(gateway, "story_bulk_last_fallback_reason",
                            s_story_bulk_last_fallback_reason);
    cJSON_AddBoolToObject(gateway, "story_bulk_link_up", s_story_bulk_link_up);
    cJSON_AddNumberToObject(gateway, "story_bulk_ready_frames_seen",
                            s_story_bulk_ready_frames_seen);
    cJSON_AddNumberToObject(gateway, "story_bulk_probe_successes",
                            s_story_bulk_probe_successes);
    cJSON_AddNumberToObject(gateway, "story_bulk_probe_failures",
                            s_story_bulk_probe_failures);
    cJSON_AddNumberToObject(gateway, "story_bulk_uart_rx_bytes",
                            s_story_bulk_uart_rx_bytes);
    cJSON_AddNumberToObject(gateway, "story_bulk_uart_tx_bytes",
                            s_story_bulk_uart_tx_bytes);
    cJSON_AddNumberToObject(gateway, "story_bulk_actual_baud",
                            s_story_bulk_actual_baud);
    cJSON_AddNumberToObject(
        gateway, "story_bulk_rx_age_ms",
        s_story_bulk_last_rx_ms ? elapsed_ms(health_now, s_story_bulk_last_rx_ms) : 0U);
#if CONFIG_IDF_TARGET_ESP32P4
    cJSON_AddStringToObject(gateway, "esp_hosted_host_version",
                            GATEWAY_STRINGIFY(ESP_HOSTED_VERSION_MAJOR_1) "."
                            GATEWAY_STRINGIFY(ESP_HOSTED_VERSION_MINOR_1) "."
                            GATEWAY_STRINGIFY(ESP_HOSTED_VERSION_PATCH_1));
    cJSON_AddStringToObject(gateway, "esp_hosted_coprocessor_version",
                            s_hosted_coprocessor_version);
    cJSON_AddBoolToObject(gateway, "esp_hosted_version_compatible",
                          s_hosted_version_compatible);
    cJSON_AddNumberToObject(gateway, "esp_hosted_fw_query_error",
                            s_hosted_fw_query_error);
    cJSON_AddStringToObject(gateway, "esp_hosted_ota_expected_version",
                            C6_OTA_EXPECTED_VERSION);
    cJSON_AddNumberToObject(gateway, "esp_hosted_ota_attempts",
                            s_hosted_ota_attempts);
    cJSON_AddNumberToObject(gateway, "esp_hosted_ota_result",
                            s_hosted_ota_result);
    cJSON_AddNumberToObject(gateway, "esp_hosted_ota_bytes",
                            s_hosted_ota_bytes);
    cJSON_AddBoolToObject(gateway, "esp_hosted_ota_performed",
                          s_hosted_ota_performed);
    cJSON_AddNumberToObject(gateway, "esp_hosted_ota_image_revision",
                            C6_OTA_IMAGE_REVISION);
    cJSON_AddNumberToObject(gateway, "esp_hosted_ota_applied_revision",
                            s_hosted_ota_applied_revision);
#ifdef CONFIG_ESP_HOSTED_SDIO_CHECKSUM
    cJSON_AddBoolToObject(gateway, "esp_hosted_sdio_checksum", true);
#else
    cJSON_AddBoolToObject(gateway, "esp_hosted_sdio_checksum", false);
#endif
#endif
    cJSON_AddBoolToObject(gateway, "ws_recovery_running", s_ws_recovery_running);
    cJSON_AddNumberToObject(gateway, "last_matrix_ms", s_last_matrix_ms);
    cJSON_AddNumberToObject(gateway, "max_matrix_ms", s_max_matrix_ms);
    cJSON_AddNumberToObject(gateway, "matrix_requests", s_matrix_requests);
    cJSON_AddNumberToObject(gateway, "matrix_failures", s_matrix_failures);
    cJSON_AddNumberToObject(gateway, "ra8_status_age_ms", age);
    cJSON_AddNumberToObject(gateway, "telemetry_target_hz",
                            1000.0 / TELEMETRY_INTERVAL_MS);
    cJSON_AddNumberToObject(gateway, "pressure_target_hz",
                            1000.0 / PRESSURE_INTERVAL_MS);
    cJSON_AddNumberToObject(gateway, "thermal_target_hz",
                            1000.0 / THERMAL_INTERVAL_MS);
    cJSON_AddNumberToObject(gateway, "thermal_age_ms",
                             s_thermal.updated_ms ?
                             elapsed_ms(health_now, s_thermal.updated_ms) : UINT32_MAX);
    cJSON_AddNumberToObject(gateway, "pressure_age_ms",
                             s_pressure.updated_ms ?
                             elapsed_ms(health_now, s_pressure.updated_ms) : UINT32_MAX);
    mic_stream_stats_t mic_stats = {0};
    mic_stream_get_stats(&mic_stats);
    cJSON_AddBoolToObject(gateway, "audio_ws_connected", mic_stream_is_connected());
    cJSON_AddBoolToObject(gateway, "audio_enabled", mic_stats.enabled);
    cJSON_AddBoolToObject(gateway, "audio_transport_suspended",
                          mic_stats.transport_suspended);
    cJSON_AddNumberToObject(gateway, "audio_control_count", mic_stats.control_count);
    cJSON_AddNumberToObject(gateway, "audio_control_sequence",
                            mic_stats.last_control_sequence);
    cJSON_AddNumberToObject(gateway, "audio_control_age_ms",
                            mic_stats.last_control_age_ms);
    cJSON_AddNumberToObject(gateway, "audio_queue_drops", mic_stats.queue_drops);
    cJSON_AddNumberToObject(gateway, "audio_send_failures", mic_stats.send_failures);
    cJSON_AddNumberToObject(gateway, "audio_send_timeouts", mic_stats.send_timeouts);
    cJSON_AddNumberToObject(gateway, "audio_arbitration_drops",
                            mic_stats.arbitration_drops);
    cJSON_AddNumberToObject(gateway, "audio_backpressure_drops",
                            mic_stats.backpressure_drops);
    cJSON_AddNumberToObject(gateway, "audio_nodelay_failures",
                            mic_stats.nodelay_failures);
#ifdef CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK
    cJSON_AddBoolToObject(gateway, "audio_separate_tx_lock", true);
#else
    cJSON_AddBoolToObject(gateway, "audio_separate_tx_lock", false);
#endif
    cJSON_AddNumberToObject(gateway, "audio_last_send_ms", mic_stats.last_send_ms);
    cJSON_AddNumberToObject(gateway, "audio_max_send_ms", mic_stats.max_send_ms);
    camera_stream_stats_t camera_stats = {0};
    camera_stream_get_stats(&camera_stats);
    cJSON_AddBoolToObject(gateway, "video_ws_connected", camera_stream_is_connected());
    cJSON_AddBoolToObject(gateway, "video_enabled", camera_stats.enabled);
    cJSON_AddBoolToObject(gateway, "video_transport_suspended",
                          camera_stats.transport_suspended);
    cJSON_AddNumberToObject(gateway, "video_control_count",
                            camera_stats.control_count);
    cJSON_AddNumberToObject(gateway, "video_control_sequence",
                            camera_stats.last_control_sequence);
    cJSON_AddNumberToObject(gateway, "video_control_age_ms",
                            camera_stats.last_control_age_ms);
    cJSON_AddNumberToObject(gateway, "video_queue_drops", camera_stats.queue_drops);
    cJSON_AddNumberToObject(gateway, "video_encode_failures",
                            camera_stats.encode_failures);
    cJSON_AddNumberToObject(gateway, "video_send_failures",
                            camera_stats.send_failures);
    cJSON_AddNumberToObject(gateway, "video_send_timeouts",
                            camera_stats.send_timeouts);
    cJSON_AddNumberToObject(gateway, "video_arbitration_drops",
                            camera_stats.arbitration_drops);
    cJSON_AddNumberToObject(gateway, "video_backpressure_drops",
                            camera_stats.backpressure_drops);
    cJSON_AddNumberToObject(gateway, "video_last_send_ms",
                            camera_stats.last_send_ms);
    cJSON_AddNumberToObject(gateway, "video_max_send_ms",
                            camera_stats.max_send_ms);
    realtime_tx_guard_stats_t tx_guard_stats = {0};
    realtime_tx_guard_get_stats(&tx_guard_stats);
    cJSON_AddNumberToObject(gateway, "tx_guard_acquisitions",
                            tx_guard_stats.acquisitions);
    cJSON_AddNumberToObject(gateway, "tx_guard_contentions",
                            tx_guard_stats.contentions);
    cJSON_AddNumberToObject(gateway, "tx_guard_timeouts",
                            tx_guard_stats.timeouts);
    cJSON_AddNumberToObject(gateway, "tx_guard_max_hold_ms",
                            tx_guard_stats.max_hold_ms);
    cJSON_AddNumberToObject(gateway, "priority_tx_guard_timeouts",
                            s_priority_tx_guard_timeouts);
    cJSON *esp32 = cJSON_AddObjectToObject(health, "esp32");
    cJSON_AddNumberToObject(esp32, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(esp32, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(esp32, "firmware", FIRMWARE_VERSION);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    cJSON_Delete(ra8);
    size_t replacements = sanitize_utf8_in_place(json);
    if (replacements) {
        static uint32_t report_counter;
        if ((report_counter++ % 120U) == 0U) {
            ESP_LOGW(TAG, "replaced %u invalid UTF-8 byte(s) in RA8 telemetry",
                     (unsigned)replacements);
        }
    }
    return json;
}

static bool send_telemetry_websocket(void)
{
    char *json = build_telemetry_json();
    if (!json) return false;
    cJSON *payload = cJSON_Parse(json);
    free(json);
    if (!payload) return false;
    bool ok = websocket_send_json("telemetry", payload);
    cJSON_Delete(payload);
    return ok;
}

static void queue_server_command(cJSON *command_json)
{
    if (!cJSON_IsObject(command_json)) return;
    cJSON *id = cJSON_GetObjectItemCaseSensitive(command_json, "id");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(command_json, "command");
    if (!cJSON_IsNumber(id) || !cJSON_IsString(name)) return;
    server_command_t command = {.id = (uint32_t)id->valuedouble};
    strlcpy(command.command, name->valuestring, sizeof(command.command));
    cJSON *args = cJSON_GetObjectItemCaseSensitive(command_json, "args");
    char *args_text = cJSON_IsObject(args) ? cJSON_PrintUnformatted(args) : strdup("{}");
    if (!args_text) return;
    strlcpy(command.args_json, args_text, sizeof(command.args_json));
    free(args_text);
    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full; command %lu not accepted", (unsigned long)command.id);
    }
}

static bool send_latest_batch(uint32_t *last_pressure, uint32_t *last_thermal)
{
    char *telemetry = build_telemetry_json();
    if (!telemetry) return false;
    size_t json_length = strlen(telemetry);
    if (json_length > UINT16_MAX) {
        free(telemetry);
        return false;
    }
    size_t capacity = BATCH_HEADER_SIZE + json_length + 16U * 16U * 2U + 40U * 32U * 2U;
    uint8_t *packet = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!packet) {
        free(telemetry);
        return false;
    }
    memset(packet, 0, BATCH_HEADER_SIZE);
    memcpy(packet, "RA8B", 4);
    packet[4] = 2;
    put_le16(packet + 6, (uint16_t)json_length);
    memcpy(packet + BATCH_HEADER_SIZE, telemetry, json_length);
    free(telemetry);

    size_t offset = BATCH_HEADER_SIZE + json_length;
    uint8_t flags = 0;
    uint32_t pressure_sequence = 0;
    uint32_t thermal_sequence = 0;
    uint32_t now = now_ms();
    if (xSemaphoreTake(s_matrix_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        if (s_pressure.pending && (!*last_pressure || now - *last_pressure >= PRESSURE_INTERVAL_MS)) {
            flags |= BATCH_FLAG_PRESSURE;
            pressure_sequence = s_pressure.sequence;
            for (size_t i = 0; i < 16U * 16U; ++i) {
                put_le16(packet + offset, (uint16_t)s_pressure.samples[i]);
                offset += 2;
            }
        }
        if (s_thermal.pending && (!*last_thermal || now - *last_thermal >= THERMAL_INTERVAL_MS)) {
            flags |= BATCH_FLAG_THERMAL;
            thermal_sequence = s_thermal.sequence;
            for (size_t row = 0; row < 32U; ++row) {
                for (size_t col = 0; col < 40U; ++col) {
                    size_t i = (row * 2U) * 80U + col * 2U;
                    int32_t average = (int32_t)s_thermal.samples[i] + s_thermal.samples[i + 1U] +
                                      s_thermal.samples[i + 80U] + s_thermal.samples[i + 81U];
                    put_le16(packet + offset, (uint16_t)(int16_t)(average / 4));
                    offset += 2;
                }
            }
        }
        xSemaphoreGive(s_matrix_mutex);
    }
    packet[5] = flags;
    put_le32(packet + 8, pressure_sequence);
    put_le32(packet + 12, thermal_sequence);
    if (flags & BATCH_FLAG_PRESSURE) {
        put_le16(packet + 16, 16);
        put_le16(packet + 18, 16);
    }
    if (flags & BATCH_FLAG_THERMAL) {
        put_le16(packet + 20, 32);
        put_le16(packet + 22, 40);
    }

    char response[BATCH_RESPONSE_SIZE];
    int status = http_post("/api/v1/batch", packet, offset, "application/octet-stream",
                           response, sizeof(response));
    free(packet);
    bool ok = status >= 200 && status < 300;
    if (!ok && response[0] != '\0') {
        ESP_LOGW(TAG, "batch response: %.512s", response);
    }
    if (ok) {
        if (xSemaphoreTake(s_matrix_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if ((flags & BATCH_FLAG_PRESSURE) && s_pressure.sequence == pressure_sequence) {
                s_pressure.pending = false;
                *last_pressure = now;
            }
            if ((flags & BATCH_FLAG_THERMAL) && s_thermal.sequence == thermal_sequence) {
                s_thermal.pending = false;
                *last_thermal = now;
            }
            xSemaphoreGive(s_matrix_mutex);
        }
        cJSON *root = cJSON_Parse(response);
        if (root) {
            queue_server_command(cJSON_GetObjectItemCaseSensitive(root, "command"));
            cJSON_Delete(root);
        }
    }
    return ok;
}

static bool send_pending_event(uint32_t *last_sent_at)
{
    safety_event_t event;
    if (xQueuePeek(s_event_queue, &event, 0) != pdTRUE) return false;

    if (s_server_acked_event == event.event_id) {
        xQueueReceive(s_event_queue, &event, 0);
        s_server_acked_event = 0;
        *last_sent_at = 0;
        ESP_LOGI(TAG, "server accepted safety event %lu", (unsigned long)event.event_id);
        return true;
    }

    uint32_t now = now_ms();
    if (*last_sent_at && now - *last_sent_at < 2000) return true;
    if (!s_ws_connected || s_hello_pending || !s_ws ||
        !esp_websocket_client_is_connected(s_ws)) {
        return true;
    }
    cJSON *source = cJSON_ParseWithLength(event.json, event.length);
    if (!source) return true;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "protocol_version", 1);
    cJSON_AddNumberToObject(body, "event_id", event.event_id);
    cJSON_AddStringToObject(body, "event_type", "ra8_safety");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(source, "state");
    cJSON_AddNumberToObject(body, "state", cJSON_IsNumber(value) ? value->valuedouble : 0);
    value = cJSON_GetObjectItemCaseSensitive(source, "severity");
    cJSON_AddNumberToObject(body, "severity", cJSON_IsNumber(value) ? value->valuedouble : 0);
    value = cJSON_GetObjectItemCaseSensitive(source, "confidence");
    cJSON_AddNumberToObject(body, "confidence", cJSON_IsNumber(value) ? value->valuedouble : 0);
    value = cJSON_GetObjectItemCaseSensitive(source, "tick");
    cJSON_AddNumberToObject(body, "occurred_at_ms", cJSON_IsNumber(value) ? value->valuedouble : now);
    cJSON_AddItemToObject(body, "payload", cJSON_Duplicate(source, true));

    ++s_event_ws_attempts;
    bool sent = websocket_send_json("event", body);
    if (!sent) ++s_event_ws_failures;
    cJSON_Delete(body);
    cJSON_Delete(source);
    *last_sent_at = now;
    return true;
}

static bool send_matrix(matrix_state_t *matrix, uint8_t kind, uint32_t *upload_sequence,
                         uint32_t *last_sent_at, uint32_t interval_ms)
{
    uint32_t now = now_ms();
    if (!s_matrix_ws_connected || !s_matrix_ws) return false;
    if (!esp_websocket_client_is_connected(s_matrix_ws)) {
        s_matrix_ws_connected = false;
        return false;
    }
    if (*last_sent_at && now - *last_sent_at < interval_ms) return false;
    size_t count = (size_t)matrix->width * matrix->height;
    size_t frame_length = 14 + count * sizeof(int16_t);
    uint8_t *frame = heap_caps_malloc(
        frame_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) frame = heap_caps_malloc(frame_length, MALLOC_CAP_8BIT);
    if (!frame) return false;
    uint32_t source_sequence = 0;
    bool pending = false;
    if (xSemaphoreTake(s_matrix_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        pending = matrix->pending;
        source_sequence = matrix->sequence;
        if (pending) memcpy(frame + 14, matrix->samples, count * sizeof(int16_t));
        xSemaphoreGive(s_matrix_mutex);
    }
    if (!pending) {
        free(frame);
        return false;
    }
    memcpy(frame, "RA8M", 4);
    frame[4] = 1;
    frame[5] = kind;
    /* RA8M dimensions are rows then columns. The source is 80 pixels wide
     * and 64 pixels high, so thermal is rows=64 and cols=80. */
    frame[6] = (uint8_t)(matrix->height >> 8);
    frame[7] = (uint8_t)matrix->height;
    frame[8] = (uint8_t)(matrix->width >> 8);
    frame[9] = (uint8_t)matrix->width;
    uint32_t sequence = *upload_sequence;
    frame[10] = (uint8_t)(sequence >> 24); frame[11] = (uint8_t)(sequence >> 16);
    frame[12] = (uint8_t)(sequence >> 8); frame[13] = (uint8_t)sequence;
    uint32_t started = now_ms();
    /* Matrices are latest-only as well. Do not block telemetry while waiting
     * for the shared transport; the pending source frame remains for retry. */
    if (!realtime_tx_guard_take(0)) {
        ++s_priority_tx_guard_timeouts;
        free(frame);
        return false;
    }
    int sent = esp_websocket_client_send_bin(
        s_matrix_ws, (const char *)frame, frame_length, pdMS_TO_TICKS(500));
    uint32_t elapsed = now_ms() - started;
    realtime_tx_guard_give();
    s_last_matrix_ms = elapsed;
    if (elapsed > s_max_matrix_ms) s_max_matrix_ms = elapsed;
    ++s_matrix_requests;
    free(frame);
    if (sent == (int)frame_length) {
        *last_sent_at = now;
        ++*upload_sequence;
        if (!*upload_sequence) *upload_sequence = 1;
        if (xSemaphoreTake(s_matrix_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (matrix->sequence == source_sequence) matrix->pending = false;
            xSemaphoreGive(s_matrix_mutex);
        }
        return true;
    }
    ++s_matrix_failures;
    if (!esp_websocket_client_is_connected(s_matrix_ws)) {
        s_matrix_ws_connected = false;
    }
    return false;
}

static void save_last_command_id(uint32_t id)
{
    nvs_handle_t nvs;
    if (nvs_open("gateway", NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_u32(nvs, "last_cmd", id) == ESP_OK) nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool recording_filename_valid(const char *filename)
{
    if (!filename || !filename[0]) return true;
    size_t length = strlen(filename);
    if (length > 31U) return false;
    for (size_t i = 0; i < length; ++i) {
        char value = filename[i];
        if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.')) return false;
    }
    return true;
}

static char *recording_command(const char *prefix, cJSON *args)
{
    cJSON *filename = cJSON_GetObjectItemCaseSensitive(args, "filename");
    const char *name = cJSON_IsString(filename) ? filename->valuestring : "";
    if (!recording_filename_valid(name)) return NULL;
    size_t length = strlen(prefix) + (name[0] ? strlen(name) + 1U : 0U) + 1U;
    char *result = malloc(length);
    if (!result) return NULL;
    if (name[0]) {
        snprintf(result, length, "%s:%s", prefix, name);
    } else {
        snprintf(result, length, "%s", prefix);
    }
    return result;
}

static char *command_payload_for_ra8(const server_command_t *command)
{
    cJSON *args = cJSON_Parse(command->args_json[0] ? command->args_json : "{}");
    char *result = NULL;
    if (!strcmp(command->command, "status.refresh")) {
        result = strdup("status");
    } else if (!strcmp(command->command, "safety.test")) {
        result = strdup("safety_test");
    } else if (!strcmp(command->command, "soother.set") && args) {
        cJSON *on = cJSON_GetObjectItemCaseSensitive(args, "on");
        if (cJSON_IsBool(on)) result = strdup(cJSON_IsTrue(on) ? "soother_on" : "soother_off");
    } else if (!strcmp(command->command, "ra8.command") && args) {
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(args, "payload");
        if (cJSON_IsString(payload) && payload->valuestring) result = strdup(payload->valuestring);
        else if (cJSON_IsObject(payload)) result = cJSON_PrintUnformatted(payload);
    } else if ((!strcmp(command->command, "collection.start") ||
                !strcmp(command->command, "collection.stop") ||
                !strcmp(command->command, "collection.capture")) && args) {
        cJSON *kinds = cJSON_GetObjectItemCaseSensitive(args, "kinds");
        cJSON *kind;
        cJSON_ArrayForEach(kind, kinds) {
            if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "audio")) {
                if (!strcmp(command->command, "collection.start")) result = recording_command("mic_record", args);
                else if (!strcmp(command->command, "collection.stop")) result = strdup("mic_record_stop");
                break;
            }
            if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "camera")) {
                if (!strcmp(command->command, "collection.start")) result = recording_command("camera_record", args);
                else if (!strcmp(command->command, "collection.stop")) result = strdup("camera_record_stop");
                else result = recording_command("camera_capture", args);
                break;
            }
        }
    }
    cJSON_Delete(args);
    return result;
}

static void send_command_ack(uint32_t id, bool ok, const char *message)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "id", id);
    cJSON_AddBoolToObject(body, "ok", ok);
    cJSON *result = cJSON_AddObjectToObject(body, "result");
    cJSON_AddStringToObject(result, "message", message);
    if (!websocket_send_json("command_ack", body)) {
        ++s_command_ack_ws_failures;
    }
    cJSON_Delete(body);
}

static void queue_command_ack(uint32_t id, bool ok, const char *message)
{
    command_ack_t ack = {.id = id, .ok = ok};
    strlcpy(ack.message, message ? message : "", sizeof(ack.message));
    if (!s_command_ack_queue || xQueueSend(s_command_ack_queue, &ack, 0) != pdTRUE) {
        ++s_command_ack_ws_failures;
    }
}

static uint32_t story_crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
        }
    }
    return crc;
}

static int story_hex_nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool story_sha256_matches(const uint8_t *data, size_t length, const char *expected)
{
    uint8_t digest[32];
    if (!expected || strlen(expected) != 64U || mbedtls_sha256(data, length, digest, 0) != 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(digest); ++i) {
        int high = story_hex_nibble(expected[i * 2U]);
        int low = story_hex_nibble(expected[i * 2U + 1U]);
        if (high < 0 || low < 0 || digest[i] != (uint8_t)((high << 4) | low)) return false;
    }
    return true;
}

static bool story_filename_valid(const char *name)
{
    if (!name || strlen(name) != 12U || name[0] != 'A' || name[1] != 'I' ||
        strcmp(name + 8, ".WAV")) return false;
    for (size_t i = 2; i < 8; ++i) {
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'A' && name[i] <= 'F'))) return false;
    }
    return true;
}

static bool story_path_valid(const char *path)
{
    static const char prefix[] = "/api/v1/ai/stories/";
    if (!path || strncmp(path, prefix, sizeof(prefix) - 1U) || strlen(path) >= 192U ||
        strstr(path, "..") || strchr(path, '?') || strchr(path, '#')) return false;
    const char *suffix = strstr(path + sizeof(prefix) - 1U, "/audio");
    return suffix && !strcmp(suffix, "/audio") && suffix > path + sizeof(prefix) - 1U;
}

static bool story_wav_valid(const uint8_t *data, size_t length)
{
    if (!data || length < 44U || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) return false;
    bool format_ok = false;
    bool data_ok = false;
    size_t offset = 12U;
    while (offset + 8U <= length) {
        uint32_t chunk_length = read_le32(data + offset + 4U);
        size_t content = offset + 8U;
        if (chunk_length > length - content) return false;
        if (!memcmp(data + offset, "fmt ", 4) && chunk_length >= 16U) {
            uint32_t sample_rate = read_le32(data + content + 4U);
            format_ok = read_le16(data + content) == 1U && read_le16(data + content + 2U) == 1U &&
                        (sample_rate == 8000U || sample_rate == 16000U) &&
                        read_le16(data + content + 14U) == 16U;
        } else if (!memcmp(data + offset, "data", 4)) {
            data_ok = chunk_length > 0U && (chunk_length & 1U) == 0U;
        }
        size_t padded = (size_t)chunk_length + ((chunk_length & 1U) ? 1U : 0U);
        if (padded > length - content) return false;
        offset = content + padded;
    }
    return format_ok && data_ok;
}

static bool story_parse_request(const server_command_t *command, story_request_t *request)
{
    memset(request, 0, sizeof(*request));
    cJSON *args = cJSON_Parse(command->args_json[0] ? command->args_json : "{}");
    if (!args) return false;
    cJSON *path = cJSON_GetObjectItemCaseSensitive(args, "path");
    cJSON *filename = cJSON_GetObjectItemCaseSensitive(args, "filename");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(args, "size_bytes");
    cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(args, "sha256");
    cJSON *crc32 = cJSON_GetObjectItemCaseSensitive(args, "crc32");
    cJSON *autoplay = cJSON_GetObjectItemCaseSensitive(args, "autoplay");
    bool valid = cJSON_IsString(path) && cJSON_IsString(filename) && cJSON_IsNumber(size) &&
                 cJSON_IsString(sha256) && cJSON_IsNumber(crc32) &&
                 size->valuedouble > 0 && size->valuedouble <= STORY_MAX_BYTES &&
                 crc32->valuedouble >= 0 && crc32->valuedouble <= UINT32_MAX;
    if (valid) {
        strlcpy(request->path, path->valuestring, sizeof(request->path));
        strlcpy(request->filename, filename->valuestring, sizeof(request->filename));
        strlcpy(request->sha256, sha256->valuestring, sizeof(request->sha256));
        request->size_bytes = (uint32_t)size->valuedouble;
        request->crc32 = (uint32_t)crc32->valuedouble;
        request->autoplay = !cJSON_IsBool(autoplay) || cJSON_IsTrue(autoplay);
        valid = story_path_valid(request->path) && story_filename_valid(request->filename) &&
                strlen(request->sha256) == 64U;
    }
    cJSON_Delete(args);
    return valid;
}

static esp_err_t story_download(const story_request_t *request, uint8_t **output)
{
    s_story_download_error = ESP_OK;
    s_story_http_status = 0U;
    s_story_received_bytes = 0U;
    char url[sizeof(SERVER_ORIGIN) + sizeof(request->path)];
    if (snprintf(url, sizeof(url), "%s%s", SERVER_ORIGIN, request->path) >= (int)sizeof(url)) {
        s_story_download_error = ESP_ERR_INVALID_SIZE;
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *data = heap_caps_malloc(request->size_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        s_story_download_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = STORY_DOWNLOAD_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t result = client ? ESP_OK : ESP_ERR_NO_MEM;
    if (client) {
        esp_http_client_set_header(client, "X-Device-ID", GATEWAY_DEVICE_ID);
        esp_http_client_set_header(client, "X-Device-Key", GATEWAY_DEVICE_KEY);
        result = esp_http_client_open(client, 0);
    }
    int64_t content_length = -1;
    if (result == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        s_story_http_status = (uint32_t)esp_http_client_get_status_code(client);
        if (s_story_http_status != 200U) {
            result = ESP_FAIL;
        } else if (content_length >= 0 && content_length != request->size_bytes) {
            result = ESP_ERR_INVALID_SIZE;
        }
    }
    size_t received = 0;
    while (result == ESP_OK && received < request->size_bytes) {
        size_t remaining = request->size_bytes - received;
        int read_size = (int)(remaining > STORY_HTTP_READ_BYTES
                                  ? STORY_HTTP_READ_BYTES
                                  : remaining);
        int count = esp_http_client_read(client, (char *)data + received, read_size);
        if (count <= 0) {
            result = ESP_FAIL;
            break;
        }
        received += (size_t)count;
        s_story_received_bytes = (uint32_t)received;
        vTaskDelay(1);
    }
    if (result == ESP_OK) {
        uint32_t crc = story_crc32_update(0xFFFFFFFFU, data, received) ^ 0xFFFFFFFFU;
        if (received != request->size_bytes || crc != request->crc32 ||
            !story_sha256_matches(data, received, request->sha256) || !story_wav_valid(data, received)) {
            result = ESP_ERR_INVALID_CRC;
        }
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    s_story_download_error = result;
    if (result != ESP_OK) {
        free(data);
        return result;
    }
    *output = data;
    return ESP_OK;
}

static bool story_download_retryable(esp_err_t result)
{
    if (result == ESP_OK || result == ESP_ERR_NO_MEM || result == ESP_ERR_INVALID_SIZE ||
        result == ESP_ERR_INVALID_CRC) {
        return false;
    }
    /* A real HTTP rejection is terminal. A missing status or an interrupted
     * 200 response is a transport failure and can be retried safely. */
    return s_story_http_status == 0U || s_story_http_status == 200U;
}

static bool story_set_media_suspended(bool suspended)
{
    for (unsigned attempt = 0; attempt < 2U; ++attempt) {
        esp_err_t video_result = camera_stream_set_transport_suspended(suspended);
        esp_err_t audio_result = mic_stream_set_transport_suspended(suspended);
        if (video_result == ESP_OK && audio_result == ESP_OK) {
            return true;
        }
        ++s_story_media_suspend_failures;
        ESP_LOGW(TAG, "%s media WSS failed: video=%s audio=%s",
                 suspended ? "pause" : "resume",
                 esp_err_to_name(video_result), esp_err_to_name(audio_result));
        if (suspended) {
            (void)camera_stream_set_transport_suspended(false);
            (void)mic_stream_set_transport_suspended(false);
        }
        vTaskDelay(pdMS_TO_TICKS(STORY_MEDIA_QUIESCE_MS));
    }
    return false;
}

static void story_result_queue_drain(void)
{
    story_result_t ignored;
    while (s_story_result_queue && xQueueReceive(s_story_result_queue, &ignored, 0) == pdTRUE) {}
}

static bool story_exchange(uint8_t frame_type, uint8_t operation, uint32_t transfer_id,
                           const uint8_t *payload, uint16_t length, uint32_t expected_offset,
                           bool require_offset, story_result_t *reply)
{
    for (unsigned attempt = 0; attempt < STORY_OPERATION_ATTEMPTS; ++attempt) {
        bool retry_busy = false;
        if (attempt > 0U) {
            ++s_story_exchange_retries;
        }
        uint16_t request_sequence = 0;
        story_result_queue_drain();
        if (send_ra8_frame_tracked(frame_type, payload, length, &request_sequence) != ESP_OK) {
            ESP_LOGW(TAG, "story op=%u transfer=%" PRIu32 " offset=%" PRIu32
                          " send failed on attempt %u/%u",
                     operation, transfer_id, expected_offset, attempt + 1U,
                     STORY_OPERATION_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        TickType_t started = xTaskGetTickCount();
        while ((xTaskGetTickCount() - started) < pdMS_TO_TICKS(STORY_OPERATION_TIMEOUT_MS)) {
            story_result_t candidate;
            if (xQueueReceive(s_story_result_queue, &candidate,
                              pdMS_TO_TICKS(STORY_ACK_POLL_MS)) != pdTRUE) continue;
            if (candidate.operation != operation || candidate.transfer_id != transfer_id ||
                candidate.request_sequence != request_sequence) continue;
            *reply = candidate;
            s_story_last_result = candidate.result;
            s_story_last_status = candidate.status;
            if (candidate.result == STORY_RESULT_BUSY) {
                vTaskDelay(pdMS_TO_TICKS(STORY_ACK_POLL_MS));
                retry_busy = true;
                break;
            }
            bool accepted = candidate.result == STORY_RESULT_OK &&
                            (!require_offset || candidate.next_offset == expected_offset);
            if (!accepted) {
                ESP_LOGW(TAG, "story op=%u transfer=%" PRIu32 " offset=%" PRIu32
                              " rejected result=%u status=%u next=%" PRIu32,
                         operation, transfer_id, expected_offset, candidate.result,
                         candidate.status, candidate.next_offset);
            }
            return accepted;
        }
        if (retry_busy) {
            continue;
        }
        ++s_story_exchange_timeouts;
        ESP_LOGW(TAG, "story op=%u transfer=%" PRIu32 " offset=%" PRIu32
                      " ACK timeout on attempt %u/%u",
                 operation, transfer_id, expected_offset, attempt + 1U,
                 STORY_OPERATION_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(STORY_ACK_RETRY_BACKOFF_MS));
    }
    return false;
}

static bool story_transfer_to_ra8(uint32_t transfer_id, const story_request_t *request,
                                  const uint8_t *data)
{
    uint8_t begin[17U + 12U] = {0};
    begin[0] = STORY_PROTOCOL_VERSION;
    begin[1] = request->autoplay ? STORY_FLAG_PLAY_AFTER : 0U;
    put_be32(begin + 4, transfer_id);
    put_be32(begin + 8, request->size_bytes);
    put_be32(begin + 12, request->crc32);
    begin[16] = 12U;
    memcpy(begin + 17, request->filename, 12U);
    story_result_t reply = {0};
    if (!story_exchange(RA8_TYPE_FILE_BEGIN, STORY_OPERATION_BEGIN, transfer_id,
                        begin, sizeof(begin), 0U, false, &reply) || reply.next_offset > request->size_bytes) {
        return false;
    }

    uint32_t offset = reply.next_offset;
    uint8_t chunk[20U + STORY_CHUNK_BYTES];
    while (offset < request->size_bytes) {
        uint32_t count = request->size_bytes - offset;
        if (count > STORY_CHUNK_BYTES) count = STORY_CHUNK_BYTES;
        memset(chunk, 0, 20U);
        chunk[0] = STORY_PROTOCOL_VERSION;
        put_be32(chunk + 4, transfer_id);
        put_be32(chunk + 8, offset);
        put_be16(chunk + 12, (uint16_t)count);
        uint32_t chunk_crc = story_crc32_update(0xFFFFFFFFU, data + offset, count) ^ 0xFFFFFFFFU;
        put_be32(chunk + 16, chunk_crc);
        memcpy(chunk + 20, data + offset, count);
        uint32_t next_offset = offset + count;
        if (!story_exchange(RA8_TYPE_FILE_CHUNK, STORY_OPERATION_CHUNK, transfer_id,
                            chunk, (uint16_t)(20U + count), next_offset, true, &reply)) return false;
        offset = next_offset;
        s_story_bytes = offset;
        if (STORY_CHUNK_PACING_MS > 0U) {
            vTaskDelay(pdMS_TO_TICKS(STORY_CHUNK_PACING_MS));
        }
    }

    uint8_t end[16] = {0};
    end[0] = STORY_PROTOCOL_VERSION;
    end[1] = request->autoplay ? STORY_FLAG_PLAY_AFTER : 0U;
    put_be32(end + 4, transfer_id);
    put_be32(end + 8, request->size_bytes);
    put_be32(end + 12, request->crc32);
    return story_exchange(RA8_TYPE_FILE_END, STORY_OPERATION_END, transfer_id,
                          end, sizeof(end), request->size_bytes, true, &reply);
}

#if CONFIG_IDF_TARGET_ESP32P4
static uint32_t s_story_bulk_sequence;
static bool story_bulk_read_exact(uint8_t *buffer, size_t length, uint32_t timeout_ms);

static esp_err_t story_bulk_uart_init(void)
{
    uart_config_t config = {
        .baud_rate = STORY_BULK_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t result = uart_param_config(STORY_BULK_UART, &config);
    if (result == ESP_OK) {
        result = uart_set_pin(STORY_BULK_UART, STORY_BULK_UART_TX_PIN,
                              STORY_BULK_UART_RX_PIN, UART_PIN_NO_CHANGE,
                              UART_PIN_NO_CHANGE);
    }
    if (result == ESP_OK) {
        result = uart_driver_install(STORY_BULK_UART, 8192, 0, 0, NULL, 0);
    }
    if (result == ESP_OK) {
        result = uart_set_rx_full_threshold(STORY_BULK_UART, 1U);
    }
    if (result == ESP_OK) {
        result = uart_set_rx_timeout(STORY_BULK_UART, 2U);
    }
    if (result == ESP_OK) {
        uart_flush_input(STORY_BULK_UART);
        uint32_t actual_baud = 0U;
        if (uart_get_baudrate(STORY_BULK_UART, &actual_baud) == ESP_OK) {
            s_story_bulk_actual_baud = actual_baud;
        }
        ESP_LOGI(TAG, "story bulk UART ready: TX=GPIO%d RX=GPIO%d baud=%" PRIu32 " 8N1 rx_threshold=1",
                 STORY_BULK_UART_TX_PIN, STORY_BULK_UART_RX_PIN,
                 s_story_bulk_actual_baud);
    }
    return result;
}

static bool story_bulk_wait_ready(uint32_t timeout_ms)
{
    uint8_t frame[STORY_BULK_HEADER_BYTES];
    static const uint8_t magic[4] = {'S', 'B', 'L', '2'};
    size_t fill = 0U;
    TickType_t started = xTaskGetTickCount();
    while ((xTaskGetTickCount() - started) < pdMS_TO_TICKS(timeout_ms)) {
        uint8_t value;
        if (uart_read_bytes(STORY_BULK_UART, &value, 1U, pdMS_TO_TICKS(50)) != 1) {
            continue;
        }
        ++s_story_bulk_uart_rx_bytes;
        s_story_bulk_last_rx_ms = now_ms();
        if (fill < 4U && value != magic[fill]) {
            fill = (value == magic[0]) ? 1U : 0U;
            if (fill == 1U) frame[0] = value;
            continue;
        }
        frame[fill++] = value;
        if (fill == sizeof(frame)) {
            if (frame[4] == STORY_BULK_VERSION && frame[5] == STORY_BULK_TYPE_READY) {
                ++s_story_bulk_ready_frames_seen;
                return true;
            }
            fill = 0U;
        }
    }
    return false;
}

static bool story_bulk_write_frame(uint8_t type, uint8_t flags, uint32_t transfer_id,
                                   uint32_t offset, uint32_t total_bytes,
                                   const uint8_t *payload, uint16_t payload_length,
                                   uint32_t crc32, uint32_t *request_sequence)
{
    uint8_t header[STORY_BULK_HEADER_BYTES] = {0};
    uint32_t sequence = ++s_story_bulk_sequence;
    put_be32(header, STORY_BULK_MAGIC);
    header[4] = STORY_BULK_VERSION;
    header[5] = type;
    header[6] = flags;
    put_be32(header + 8, transfer_id);
    put_be32(header + 12, sequence);
    put_be32(header + 16, offset);
    put_be32(header + 20, total_bytes);
    put_be16(header + 24, payload_length);
    put_be32(header + 28, crc32);
    if (uart_write_bytes(STORY_BULK_UART, header, sizeof(header)) != sizeof(header) ||
        uart_wait_tx_done(STORY_BULK_UART, pdMS_TO_TICKS(50)) != ESP_OK) {
        return false;
    }
    s_story_bulk_uart_tx_bytes += sizeof(header);
    if (payload_length) {
        vTaskDelay(pdMS_TO_TICKS(STORY_BULK_HEADER_GAP_MS));
        if (!payload || uart_write_bytes(STORY_BULK_UART, payload, payload_length) != payload_length ||
            uart_wait_tx_done(STORY_BULK_UART, pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }
        s_story_bulk_uart_tx_bytes += payload_length;
    }
    vTaskDelay(pdMS_TO_TICKS(STORY_BULK_FRAME_GAP_MS));
    if (request_sequence) *request_sequence = sequence;
    return true;
}

static bool story_bulk_read_exact(uint8_t *buffer, size_t length, uint32_t timeout_ms)
{
    size_t received = 0;
    TickType_t started = xTaskGetTickCount();
    while (received < length &&
           (xTaskGetTickCount() - started) < pdMS_TO_TICKS(timeout_ms)) {
        int count = uart_read_bytes(STORY_BULK_UART, buffer + received,
                                    length - received, pdMS_TO_TICKS(20));
        if (count > 0) {
            received += (size_t)count;
            s_story_bulk_uart_rx_bytes += (uint32_t)count;
            s_story_bulk_last_rx_ms = now_ms();
        }
    }
    return received == length;
}

static bool story_bulk_wait_ack(uint8_t request_type, uint32_t transfer_id,
                                uint32_t minimum_offset, uint32_t *next_offset)
{
    uint8_t ack[STORY_BULK_HEADER_BYTES];
    TickType_t started = xTaskGetTickCount();
    while ((xTaskGetTickCount() - started) < pdMS_TO_TICKS(STORY_BULK_ACK_TIMEOUT_MS)) {
        if (!story_bulk_read_exact(ack, sizeof(ack), STORY_BULK_ACK_TIMEOUT_MS)) {
            return false;
        }
        if (read_be32(ack) != STORY_BULK_MAGIC || ack[4] != STORY_BULK_VERSION ||
            ack[5] != STORY_BULK_TYPE_ACK || ack[6] != request_type ||
            read_be32(ack + 8) != transfer_id) {
            continue;
        }
        uint32_t offset = read_be32(ack + 16);
        s_story_last_result = ack[7];
        if (next_offset) *next_offset = offset;
        if (ack[7] != STORY_RESULT_OK) {
            return false;
        }
        if (offset >= minimum_offset) {
            return true;
        }
    }
    return false;
}

static bool story_bulk_handshake(uint32_t transfer_id)
{
    uint32_t ignored_sequence = 0U;
    uint32_t next_offset = 0U;

    /*
     * PING is valid in the RA8 idle state, so send it proactively. Requiring
     * READY first can leave both sides silent from P4's point of view when a
     * READY byte is lost during boot or pin reconfiguration.
     */
    uart_flush_input(STORY_BULK_UART);
    if (story_bulk_write_frame(STORY_BULK_TYPE_PING, 0U, transfer_id, 0U, 0U,
                               NULL, 0U, 0U, &ignored_sequence) &&
        story_bulk_wait_ack(STORY_BULK_TYPE_PING, transfer_id, 0U, &next_offset)) {
        return true;
    }

    uart_flush_input(STORY_BULK_UART);
    if (!story_bulk_wait_ready(3000U)) {
        return false;
    }
    return story_bulk_write_frame(STORY_BULK_TYPE_PING, 0U, transfer_id, 0U, 0U,
                                  NULL, 0U, 0U, &ignored_sequence) &&
           story_bulk_wait_ack(STORY_BULK_TYPE_PING, transfer_id, 0U, &next_offset);
}

static bool story_bulk_transfer_locked(uint32_t transfer_id,
                                       const story_request_t *request,
                                       const uint8_t *data)
{
    uint32_t ignored_sequence;
    uint32_t next_offset = 0U;
    uint8_t flags = request->autoplay ? STORY_FLAG_PLAY_AFTER : 0U;

    if (!story_bulk_handshake(transfer_id)) {
        ++s_story_bulk_handshake_failures;
        ESP_LOGW(TAG, "story UART handshake failed after active PING and READY retry");
        return false;
    }
    ++s_story_bulk_handshake_successes;
    if (!story_bulk_write_frame(STORY_BULK_TYPE_BEGIN, flags, transfer_id, 0U,
                                request->size_bytes, (const uint8_t *)request->filename,
                                13U, request->crc32, &ignored_sequence) ||
        !story_bulk_wait_ack(STORY_BULK_TYPE_BEGIN, transfer_id, 0U, &next_offset) ||
        next_offset > request->size_bytes) {
        return false;
    }

    uint32_t offset = next_offset;
    while (offset < request->size_bytes) {
        uint32_t window_end = offset;
        for (unsigned slot = 0; slot < STORY_BULK_WINDOW &&
                                window_end < request->size_bytes; ++slot) {
            uint32_t count = request->size_bytes - window_end;
            if (count > STORY_BULK_PAYLOAD_BYTES) count = STORY_BULK_PAYLOAD_BYTES;
            uint32_t chunk_crc = story_crc32_update(0xFFFFFFFFU, data + window_end,
                                                     count) ^ 0xFFFFFFFFU;
            if (!story_bulk_write_frame(STORY_BULK_TYPE_DATA, 0U, transfer_id,
                                        window_end, request->size_bytes,
                                        data + window_end, (uint16_t)count,
                                        chunk_crc, &ignored_sequence)) {
                return false;
            }
            window_end += count;
        }
        if (!story_bulk_wait_ack(STORY_BULK_TYPE_DATA, transfer_id, window_end,
                                 &next_offset)) {
            return false;
        }
        offset = next_offset;
        s_story_bytes = offset;
    }

    return story_bulk_write_frame(STORY_BULK_TYPE_END, flags, transfer_id,
                                  request->size_bytes, request->size_bytes,
                                  NULL, 0U, request->crc32, &ignored_sequence) &&
           story_bulk_wait_ack(STORY_BULK_TYPE_END, transfer_id,
                               request->size_bytes, &next_offset);
}

static bool story_bulk_transfer_to_ra8(uint32_t transfer_id,
                                       const story_request_t *request,
                                       const uint8_t *data)
{
    if (!s_story_bulk_uart_mutex ||
        xSemaphoreTake(s_story_bulk_uart_mutex, pdMS_TO_TICKS(4000)) != pdTRUE) {
        return false;
    }
    bool result = story_bulk_transfer_locked(transfer_id, request, data);
    s_story_bulk_link_up = result || s_story_bulk_link_up;
    xSemaphoreGive(s_story_bulk_uart_mutex);
    return result;
}

static void story_bulk_link_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_story_active && s_story_bulk_uart_mutex &&
            xSemaphoreTake(s_story_bulk_uart_mutex, 0) == pdTRUE) {
            uint32_t probe_id = 0xFFFFFF00U | (s_story_bulk_probe_successes & 0xFFU);
            bool ok = story_bulk_handshake(probe_id);
            if (ok) {
                ++s_story_bulk_probe_successes;
                s_story_bulk_link_up = true;
            } else {
                ++s_story_bulk_probe_failures;
                s_story_bulk_link_up = false;
            }
            xSemaphoreGive(s_story_bulk_uart_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
#endif

static void story_task(void *arg)
{
    (void)arg;
    server_command_t command;
    for (;;) {
        if (xQueueReceive(s_story_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        s_story_active = true;
        s_story_transfer_id = command.id ? command.id : 1U;
        s_story_bytes = 0U;
        ++s_story_attempts;
        s_story_download_error = ESP_OK;
        s_story_http_status = 0U;
        s_story_received_bytes = 0U;
        story_request_t request;
        uint8_t *audio = NULL;
        bool ok = story_parse_request(&command, &request);
        char message[128];
        strlcpy(message, ok ? "story request accepted" : "invalid story request",
                sizeof(message));
        if (ok) {
            ok = story_set_media_suspended(true);
            if (!ok) {
                s_story_download_error = ESP_ERR_INVALID_STATE;
                strlcpy(message, "could not pause media transport for story download",
                        sizeof(message));
            }
        }
        if (ok) {
            vTaskDelay(pdMS_TO_TICKS(STORY_MEDIA_QUIESCE_MS));
            esp_err_t result = ESP_FAIL;
            for (unsigned attempt = 0; attempt < STORY_DOWNLOAD_ATTEMPTS; ++attempt) {
                result = story_download(&request, &audio);
                if (result == ESP_OK) break;
                ESP_LOGW(TAG, "story download attempt %u/%u failed: %s http=%" PRIu32
                              " bytes=%" PRIu32 "/%" PRIu32,
                         attempt + 1U, STORY_DOWNLOAD_ATTEMPTS, esp_err_to_name(result),
                         s_story_http_status, s_story_received_bytes, request.size_bytes);
                if (attempt + 1U < STORY_DOWNLOAD_ATTEMPTS && story_download_retryable(result)) {
                    vTaskDelay(pdMS_TO_TICKS(STORY_RETRY_BACKOFF_MS * (attempt + 1U)));
                } else {
                    break;
                }
            }
            bool resumed = story_set_media_suspended(false);
            if (!resumed) {
                ESP_LOGE(TAG, "media WSS did not resume after story download");
            }
            ok = result == ESP_OK;
            if (ok) {
                strlcpy(message, "story downloaded and verified", sizeof(message));
            } else {
                snprintf(message, sizeof(message),
                         "story download failed: %s http=%" PRIu32 " bytes=%" PRIu32 "/%" PRIu32,
                         esp_err_to_name(result), s_story_http_status,
                         s_story_received_bytes, request.size_bytes);
            }
        }
        if (ok) {
            for (unsigned attempt = 0; attempt < STORY_RA8_TRANSFER_ATTEMPTS; ++attempt) {
#if CONFIG_IDF_TARGET_ESP32P4
                ok = story_bulk_transfer_to_ra8(s_story_transfer_id, &request, audio);
                if (!ok && (attempt + 1U >= 3U)) {
                    ++s_story_bulk_fallbacks;
                    s_story_bulk_last_fallback_reason = 1U;
                    ESP_LOGW(TAG, "dedicated story UART failed three handshakes; fallback reason=handshake_timeout");
                    ok = story_transfer_to_ra8(s_story_transfer_id, &request, audio);
                }
#else
                ok = story_transfer_to_ra8(s_story_transfer_id, &request, audio);
#endif
                if (ok) break;
                ESP_LOGW(TAG, "RA8 story transfer attempt %u/%u failed at offset=%" PRIu32
                              " result=%" PRIu32 " status=%" PRIu32,
                         attempt + 1U, STORY_RA8_TRANSFER_ATTEMPTS, s_story_bytes,
                         s_story_last_result, s_story_last_status);
                if (attempt + 1U < STORY_RA8_TRANSFER_ATTEMPTS) {
                    s_story_bytes = 0U;
                    vTaskDelay(pdMS_TO_TICKS(STORY_RETRY_BACKOFF_MS * (attempt + 1U)));
                }
            }
            if (ok) {
                strlcpy(message, "story stored on RA8 and queued for playback",
                        sizeof(message));
            } else {
                snprintf(message, sizeof(message),
                         "RA8 story transfer failed at offset=%" PRIu32
                         " result=%" PRIu32 " status=%" PRIu32,
                         s_story_bytes, s_story_last_result, s_story_last_status);
            }
        }
        free(audio);
        if (ok) {
            ++s_story_completed;
            if (command.id > s_last_command_id) {
                s_last_command_id = command.id;
                save_last_command_id(command.id);
            }
        } else {
            ++s_story_failures;
        }
        queue_command_ack(command.id, ok, message);
        s_story_finished_ms = now_ms();
        s_story_active = false;
        s_story_pending_command_id = 0U;
    }
}

static void process_server_command(const server_command_t *command)
{
    if (command->id == s_last_command_id) {
        send_command_ack(command->id, true, "duplicate already handled");
        return;
    }
    if (!strcmp(command->command, "media.preview.start") ||
        !strcmp(command->command, "media.preview.stop")) {
        s_last_command_id = command->id;
        save_last_command_id(command->id);
        send_command_ack(command->id, true, MICROPHONE_NAME " is controlled by WSS subscribers");
        return;
    }
    if (!strcmp(command->command, "story.audio.play")) {
        if (s_story_pending_command_id == command->id) {
            ESP_LOGI(TAG, "story command %lu is already in progress", (unsigned long)command->id);
            return;
        }
        if (s_story_pending_command_id != 0U) {
            send_command_ack(command->id, false, "another story transfer is in progress");
            return;
        }
        s_story_pending_command_id = command->id;
        if (!s_story_queue || xQueueSend(s_story_queue, command, 0) != pdTRUE) {
            s_story_pending_command_id = 0U;
            send_command_ack(command->id, false, "story queue is busy");
        } else {
            ESP_LOGI(TAG, "story command %lu queued for background transfer", (unsigned long)command->id);
        }
        return;
    }
    char *payload = command_payload_for_ra8(command);
    if (!payload) {
        send_command_ack(command->id, false, "unsupported command");
        return;
    }
    size_t length = strlen(payload);
    esp_err_t result = length <= RA8_MAX_PAYLOAD ?
        send_ra8_frame(RA8_TYPE_CMD, (const uint8_t *)payload, (uint16_t)length) : ESP_ERR_INVALID_SIZE;
    free(payload);
    if (result == ESP_OK) {
        s_last_command_id = command->id;
        save_last_command_id(command->id);
        send_command_ack(command->id, true, "forwarded to RA8");
        ESP_LOGI(TAG, "server command %lu forwarded to RA8", (unsigned long)command->id);
    } else {
        send_command_ack(command->id, false, "UART send failed");
    }
}

static void parse_websocket_text(const char *text, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(text, length);
    if (!root) return;
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "event_ack") && cJSON_IsObject(payload)) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "event_id");
        if (cJSON_IsNumber(id)) s_server_acked_event = (uint32_t)id->valuedouble;
    } else if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "command") && cJSON_IsObject(payload)) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "id");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(payload, "command");
        if (cJSON_IsNumber(id) && cJSON_IsString(name)) {
            server_command_t command = {.id = (uint32_t)id->valuedouble};
            strlcpy(command.command, name->valuestring, sizeof(command.command));
            cJSON *args = cJSON_GetObjectItemCaseSensitive(payload, "args");
            char *args_text = cJSON_IsObject(args) ? cJSON_PrintUnformatted(args) : strdup("{}");
            if (args_text) {
                strlcpy(command.args_json, args_text, sizeof(command.args_json));
                free(args_text);
                if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "command queue full; command %lu not accepted", (unsigned long)command.id);
                }
            }
        }
    } else if (cJSON_IsString(kind) &&
               (!strcmp(kind->valuestring, "hello_ack") || !strcmp(kind->valuestring, "telemetry_ack") ||
                !strcmp(kind->valuestring, "matrix_ack") || !strcmp(kind->valuestring, "command_ack_received"))) {
        ESP_LOGD(TAG, "server message: %s", kind->valuestring);
    } else if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "error")) {
        char *error = cJSON_PrintUnformatted(payload);
        ESP_LOGE(TAG, "server error: %s", error ? error : "unknown");
        free(error);
    }
    cJSON_Delete(root);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_ws_connected = true;
        s_ws_disconnected_since_ms = 0;
        s_ws_connected_since_ms = now_ms();
        s_hello_pending = true;
        s_ws_recovery_requested = false;
        s_ws_recovery_grace_until_ms = now_ms() + TELEMETRY_RECOVERY_GRACE_MS;
        ESP_LOGI(TAG, "WSS connected");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_ws_connected = false;
        s_ws_connected_since_ms = 0;
        if (!s_ws_disconnected_since_ms) s_ws_disconnected_since_ms = now_ms();
        s_ws_recovery_grace_until_ms = now_ms() + TELEMETRY_RECOVERY_GRACE_MS;
        ++s_ws_disconnect_count;
        s_hello_pending = true;
        ESP_LOGW(TAG, "WSS disconnected");
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        s_ws_connected = false;
        s_ws_connected_since_ms = 0;
        if (!s_ws_disconnected_since_ms) s_ws_disconnected_since_ms = now_ms();
        s_ws_recovery_grace_until_ms = now_ms() + TELEMETRY_RECOVERY_GRACE_MS;
        ++s_ws_error_count;
        s_hello_pending = true;
        ESP_LOGE(TAG, "WSS transport error");
    } else if (event_id == WEBSOCKET_EVENT_CLOSED) {
        s_ws_connected = false;
        s_ws_connected_since_ms = 0;
        if (!s_ws_disconnected_since_ms) s_ws_disconnected_since_ms = now_ms();
        s_ws_recovery_grace_until_ms = now_ms() + TELEMETRY_RECOVERY_GRACE_MS;
        ++s_ws_peer_close_count;
        s_hello_pending = true;
        ESP_LOGW(TAG, "WSS closed by peer");
    } else if (event_id == WEBSOCKET_EVENT_DATA && data && data->op_code == 0x1) {
        if (data->payload_offset == 0) {
            s_ws_text_length = 0;
            s_ws_text_expected = data->payload_len;
        }
        if (s_ws_text && data->payload_offset + data->data_len < s_ws_text_capacity) {
            memcpy(s_ws_text + data->payload_offset, data->data_ptr, data->data_len);
            s_ws_text_length = data->payload_offset + data->data_len;
            if (s_ws_text_length == s_ws_text_expected) {
                s_ws_text[s_ws_text_length] = '\0';
                parse_websocket_text(s_ws_text, s_ws_text_length);
            }
        }
    }
}

static void matrix_websocket_event_handler(void *handler_args, esp_event_base_t base,
                                           int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_matrix_ws_connected = true;
        ESP_LOGI(TAG, "matrix WSS connected");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_matrix_ws_connected = false;
        ESP_LOGW(TAG, "matrix WSS disconnected");
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        s_matrix_ws_connected = false;
        ESP_LOGE(TAG, "matrix WSS transport error");
    } else if (event_id == WEBSOCKET_EVENT_DATA && data &&
               data->payload_offset == 0 &&
               data->payload_len == data->data_len) {
        if (data->op_code == 0x1 && data->data_len > 0 &&
            data->data_len < 512) {
            char message[512];
            memcpy(message, data->data_ptr, data->data_len);
            message[data->data_len] = '\0';
            cJSON *root = cJSON_Parse(message);
            cJSON *kind = root
                              ? cJSON_GetObjectItemCaseSensitive(root, "kind")
                              : NULL;
            if (cJSON_IsString(kind) &&
                !strcmp(kind->valuestring, "error")) {
                ESP_LOGE(TAG, "matrix server error: %s", message);
            } else if (!cJSON_IsString(kind) ||
                       strcmp(kind->valuestring, "matrix_ack")) {
                ESP_LOGW(TAG, "matrix server message: %s", message);
            }
            cJSON_Delete(root);
        } else if (data->op_code == 0x8) {
            uint16_t close_code =
                data->data_len >= 2
                    ? ((uint16_t)(uint8_t)data->data_ptr[0] << 8) |
                          (uint8_t)data->data_ptr[1]
                    : 0;
            int reason_length =
                data->data_len > 2 ? data->data_len - 2 : 0;
            if (reason_length > 160) reason_length = 160;
            ESP_LOGW(TAG, "matrix server close code=%u reason=%.*s",
                     (unsigned)close_code, reason_length,
                     reason_length ? data->data_ptr + 2 : "");
        }
    }
}

static void network_task(void *arg)
{
    (void)arg;
    uint32_t last_telemetry = 0;
    uint32_t last_event_send = 0;
    uint32_t last_log = 0;
    for (;;) {
        uint32_t now = now_ms();
        bool story_restart_guard = s_story_active ||
                                   (s_story_finished_ms &&
                                    elapsed_ms(now, s_story_finished_ms) < STORY_RESET_GRACE_MS);
        if (s_ws_recovery_running) {
            uint32_t recovery_elapsed_ms = elapsed_ms(now, s_ws_recovery_started_ms);
            if (!s_ws_recovery_timeout_reported &&
                recovery_elapsed_ms >= TELEMETRY_RECOVERY_TIMEOUT_MS) {
                s_ws_recovery_timeout_reported = true;
                ++s_ws_recovery_failures;
                ESP_LOGE(TAG, "telemetry WSS recovery is still pending after %lums",
                         (unsigned long)recovery_elapsed_ms);
            }
            if (recovery_elapsed_ms >= TELEMETRY_RECOVERY_TIMEOUT_MS &&
                !story_restart_guard &&
                s_ws_self_restart_attempts < TELEMETRY_SELF_RESTART_LIMIT) {
                ++s_ws_self_restart_attempts;
                ESP_LOGE(TAG,
                         "telemetry WSS recovery task is blocked; restarting gateway %lu/%u",
                         (unsigned long)s_ws_self_restart_attempts,
                         (unsigned)TELEMETRY_SELF_RESTART_LIMIT);
                vTaskDelay(pdMS_TO_TICKS(250));
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        uint32_t unrecovered_ms = s_ws_recovery_running ?
                                  elapsed_ms(now, s_ws_recovery_started_ms) :
                                  (s_ws_disconnected_since_ms ?
                                   elapsed_ms(now, s_ws_disconnected_since_ms) : 0U);
        if (unrecovered_ms >= TELEMETRY_SELF_RESTART_MS &&
            !story_restart_guard &&
            s_ws_self_restart_attempts < TELEMETRY_SELF_RESTART_LIMIT) {
            ++s_ws_self_restart_attempts;
            ESP_LOGE(TAG,
                     "telemetry WSS unrecovered for %lums; self restart %lu/%u",
                     (unsigned long)unrecovered_ms,
                     (unsigned long)s_ws_self_restart_attempts,
                     (unsigned)TELEMETRY_SELF_RESTART_LIMIT);
            vTaskDelay(pdMS_TO_TICKS(250));
            esp_restart();
        }
        if (s_ws_connected && s_ws_connected_since_ms &&
            elapsed_ms(now, s_ws_connected_since_ms) >= TELEMETRY_STABLE_CLEAR_MS &&
            s_ws_self_restart_attempts) {
            ESP_LOGI(TAG, "telemetry stable; clearing self-restart limiter");
            s_ws_self_restart_attempts = 0;
        }
        if (s_ws_recovery_requested &&
            (!s_ws_recovery_retry_after_ms ||
             deadline_reached(now, s_ws_recovery_retry_after_ms))) {
            start_telemetry_ws_recovery(now);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (s_ws_connected && esp_websocket_client_is_connected(s_ws) &&
            s_ws_watchdog_armed &&
            (!s_ws_recovery_grace_until_ms ||
             deadline_reached(now, s_ws_recovery_grace_until_ms)) &&
            elapsed_ms(now, s_last_ws_success_ms) >= TELEMETRY_STALL_MS) {
            request_telemetry_ws_recovery(WS_RECOVERY_STALLED);
            continue;
        }
        if (!s_ws_connected && s_ws_disconnected_since_ms &&
            elapsed_ms(now, s_ws_disconnected_since_ms) >=
                TELEMETRY_DISCONNECTED_RECOVERY_MS &&
            (!s_ws_recovery_retry_after_ms ||
             deadline_reached(now, s_ws_recovery_retry_after_ms))) {
            request_telemetry_ws_recovery(WS_RECOVERY_STALLED);
            continue;
        }
        if (s_ws_connected && s_hello_pending) send_websocket_hello();
        command_ack_t ack;
        while (xQueueReceive(s_command_ack_queue, &ack, 0) == pdTRUE) {
            send_command_ack(ack.id, ack.ok, ack.message);
        }
        server_command_t command;
        while (xQueueReceive(s_command_queue, &command, 0) == pdTRUE) process_server_command(&command);
        send_pending_event(&last_event_send);
        now = now_ms();
        if (s_ws_connected && !s_hello_pending &&
            (!last_telemetry || now - last_telemetry >= TELEMETRY_INTERVAL_MS)) {
            /*
             * Anchor the cadence before the send so upload latency is not added
             * to the requested telemetry period.
             */
            last_telemetry = now;
            send_telemetry_websocket();
        }
        if (!last_log || now - last_log >= 5000) {
            uint32_t status_age = s_stats.last_status_ms ?
                                  elapsed_ms(now, s_stats.last_status_ms) : UINT32_MAX;
            uint32_t uart_age = s_stats.last_valid_frame_ms ?
                                elapsed_ms(now, s_stats.last_valid_frame_ms) : UINT32_MAX;
            uint32_t raw_rx_age = s_stats.last_uart_rx_ms ?
                                  elapsed_ms(now, s_stats.last_uart_rx_ms) : UINT32_MAX;
            ESP_LOGI(TAG, "health wifi=up telemetry=%s matrix=%s ra8=%s frames=%lu crc=%lu events=%u ws=%lu/%lu last=%lums recover=%lu/%lu age=%lums uart_age=%lums raw=%lu age=%lums pin=%ld edges=%lu rx_recover=%lu/%lu matrix_tx=%lu/%lu heap=%lu psram=%lu",
                     s_ws_connected ? "up" : "retrying",
                     s_matrix_ws_connected ? "up" : "retrying",
                     s_stats.last_status_ms && status_age <= RA8_OFFLINE_MS ? "online" : "waiting",
                     (unsigned long)s_stats.valid_frames, (unsigned long)s_stats.crc_errors,
                     (unsigned)uxQueueMessagesWaiting(s_event_queue),
                     (unsigned long)s_ws_requests, (unsigned long)s_ws_failures,
                     (unsigned long)s_last_ws_ms,
                     (unsigned long)s_ws_recovery_count,
                     (unsigned long)s_ws_recovery_failures,
                     (unsigned long)(s_ws_watchdog_armed
                                         ? elapsed_ms(now, s_last_ws_success_ms)
                                         : UINT32_MAX),
                     (unsigned long)uart_age,
                     (unsigned long)s_stats.uart_rx_bytes,
                     (unsigned long)raw_rx_age,
                     (long)s_stats.uart_rx_pin_level,
                     (unsigned long)s_stats.uart_rx_pin_transitions,
                     (unsigned long)s_stats.uart_rx_recovery_count,
                     (unsigned long)s_stats.uart_rx_recovery_failures,
                     (unsigned long)s_matrix_requests, (unsigned long)s_matrix_failures,
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            last_log = now;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void matrix_network_task(void *arg)
{
    (void)arg;
    uint32_t last_pressure = 0;
    uint32_t last_thermal = 0;
    bool pressure_first = true;
    for (;;) {
        if ((xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) &&
            s_matrix_ws_connected) {
            /*
             * Alternate the first channel so a temporarily slow send cannot
             * permanently favor pressure over thermal (or vice versa).
             */
            if (pressure_first) {
                send_matrix(&s_pressure, 2, &s_pressure_upload_sequence,
                            &last_pressure, PRESSURE_INTERVAL_MS);
                send_matrix(&s_thermal, 1, &s_thermal_upload_sequence,
                            &last_thermal, THERMAL_INTERVAL_MS);
            } else {
                send_matrix(&s_thermal, 1, &s_thermal_upload_sequence,
                            &last_thermal, THERMAL_INTERVAL_MS);
                send_matrix(&s_pressure, 2, &s_pressure_upload_sequence,
                            &last_pressure, PRESSURE_INTERVAL_MS);
            }
            pressure_first = !pressure_first;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ++s_wifi_disconnect_count;
        s_ws_connected = false;
        if (!s_ws_disconnected_since_ms) s_ws_disconnected_since_ms = now_ms();
        s_matrix_ws_connected = false;
        s_http_connected = false;
        s_hello_sent = false;
        s_ws_recovery_grace_until_ms = now_ms() + TELEMETRY_RECOVERY_GRACE_MS;
        if (s_http) esp_http_client_close(s_http);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&got_ip->ip_info.ip));
        s_ws_recovery_grace_until_ms = now_ms() + TELEMETRY_RECOVERY_GRACE_MS;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

#if CONFIG_IDF_TARGET_ESP32P4
static void log_hosted_coprocessor_version(void)
{
    esp_hosted_coprocessor_fwver_t version = {0};
    s_hosted_fw_query_error = esp_hosted_get_coprocessor_fwversion(&version);
    if (s_hosted_fw_query_error != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted co-processor version query failed: %s",
                 esp_err_to_name(s_hosted_fw_query_error));
        return;
    }

    snprintf(s_hosted_coprocessor_version, sizeof(s_hosted_coprocessor_version),
             "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
             version.major1, version.minor1, version.patch1);
    s_hosted_coprocessor_major = version.major1;
    s_hosted_coprocessor_minor = version.minor1;
    s_hosted_version_compatible =
        version.major1 == ESP_HOSTED_VERSION_MAJOR_1 &&
        version.minor1 == ESP_HOSTED_VERSION_MINOR_1;

    ESP_LOGI(TAG,
             "ESP-Hosted host=%u.%u.%u co-processor=%s compatible=%s",
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
             ESP_HOSTED_VERSION_PATCH_1, s_hosted_coprocessor_version,
             s_hosted_version_compatible ? "yes" : "NO");
}

static uint32_t hosted_ota_load_u32(const char *key)
{
    nvs_handle_t nvs;
    uint32_t value = 0;
    if (nvs_open("gateway", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, key, &value);
        nvs_close(nvs);
    }
    return value;
}

static esp_err_t hosted_ota_store_u32(const char *key, uint32_t value)
{
    nvs_handle_t nvs;
    esp_err_t result = nvs_open("gateway", NVS_READWRITE, &nvs);
    if (result != ESP_OK) return result;
    result = nvs_set_u32(nvs, key, value);
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result;
}

static esp_err_t validate_embedded_c6_firmware(size_t *firmware_size)
{
    size_t size = (size_t)(s_c6_firmware_end - s_c6_firmware_start);
    size_t app_desc_offset = sizeof(esp_image_header_t) +
                             sizeof(esp_image_segment_header_t);
    if (size < app_desc_offset + sizeof(esp_app_desc_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_image_header_t *header =
        (const esp_image_header_t *)s_c6_firmware_start;
    const esp_app_desc_t *app_desc =
        (const esp_app_desc_t *)(s_c6_firmware_start + app_desc_offset);
    if (header->magic != ESP_IMAGE_HEADER_MAGIC || header->segment_count == 0 ||
        app_desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strncmp(app_desc->version, C6_OTA_EXPECTED_VERSION,
                sizeof(app_desc->version)) != 0 ||
        strncmp(app_desc->project_name, "network_adapter",
                sizeof(app_desc->project_name)) != 0) {
        ESP_LOGE(TAG, "embedded C6 image identity mismatch: version='%.*s' project='%.*s'",
                 (int)sizeof(app_desc->version), app_desc->version,
                 (int)sizeof(app_desc->project_name), app_desc->project_name);
        return ESP_ERR_INVALID_STATE;
    }

    *firmware_size = size;
    return ESP_OK;
}

static void maybe_update_hosted_coprocessor(void)
{
    if (s_hosted_fw_query_error != ESP_OK) {
        s_hosted_ota_result = s_hosted_fw_query_error;
        return;
    }
    s_hosted_ota_applied_revision =
        hosted_ota_load_u32(C6_OTA_REVISION_NVS_KEY);
    bool revision_current =
        s_hosted_ota_applied_revision == C6_OTA_IMAGE_REVISION;
    if (s_hosted_version_compatible && revision_current) {
        s_hosted_ota_attempts = 0;
        s_hosted_ota_result = ESP_OK;
        esp_err_t clear_result = hosted_ota_store_u32(C6_OTA_NVS_KEY, 0);
        if (clear_result != ESP_OK) {
            ESP_LOGW(TAG, "failed to clear C6 OTA attempt counter: %s",
                     esp_err_to_name(clear_result));
        }
        return;
    }

    size_t firmware_size = 0;
    s_hosted_ota_result = validate_embedded_c6_firmware(&firmware_size);
    if (s_hosted_ota_result != ESP_OK) {
        ESP_LOGE(TAG, "embedded C6 firmware validation failed: %s",
                 esp_err_to_name(s_hosted_ota_result));
        return;
    }

    uint32_t pending_revision =
        hosted_ota_load_u32(C6_OTA_PENDING_NVS_KEY);
    if (pending_revision != C6_OTA_IMAGE_REVISION) {
        esp_err_t reset_result =
            hosted_ota_store_u32(C6_OTA_NVS_KEY, 0);
        if (reset_result == ESP_OK) {
            reset_result = hosted_ota_store_u32(C6_OTA_PENDING_NVS_KEY,
                                                C6_OTA_IMAGE_REVISION);
        }
        if (reset_result != ESP_OK) {
            s_hosted_ota_result = reset_result;
            ESP_LOGE(TAG, "failed to initialize C6 OTA revision state: %s",
                     esp_err_to_name(reset_result));
            return;
        }
    }

    s_hosted_ota_attempts = hosted_ota_load_u32(C6_OTA_NVS_KEY);
    if (s_hosted_ota_attempts >= C6_OTA_MAX_ATTEMPTS) {
        s_hosted_ota_result = ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG,
                 "C6 OTA blocked after %" PRIu32 " failed/pending attempts; "
                 "direct recovery is required",
                 s_hosted_ota_attempts);
        return;
    }

    ++s_hosted_ota_attempts;
    esp_err_t persist_result =
        hosted_ota_store_u32(C6_OTA_NVS_KEY, s_hosted_ota_attempts);
    if (persist_result != ESP_OK) {
        s_hosted_ota_result = persist_result;
        ESP_LOGE(TAG, "failed to persist C6 OTA attempt: %s",
                 esp_err_to_name(persist_result));
        return;
    }

    ESP_LOGW(TAG,
             "updating ESP-Hosted C6 %s -> %s rev %u (%u bytes), attempt %" PRIu32 "/%u",
             s_hosted_coprocessor_version, C6_OTA_EXPECTED_VERSION,
             (unsigned)C6_OTA_IMAGE_REVISION, (unsigned)firmware_size,
             s_hosted_ota_attempts,
             (unsigned)C6_OTA_MAX_ATTEMPTS);
    s_hosted_ota_result = esp_hosted_slave_ota_begin();
    if (s_hosted_ota_result != ESP_OK) {
        ESP_LOGE(TAG, "C6 OTA begin failed: %s",
                 esp_err_to_name(s_hosted_ota_result));
        return;
    }

    s_hosted_ota_bytes = 0;
    while (s_hosted_ota_bytes < firmware_size) {
        uint32_t remaining = (uint32_t)(firmware_size - s_hosted_ota_bytes);
        uint32_t chunk_size = remaining > C6_OTA_CHUNK_SIZE ?
                              C6_OTA_CHUNK_SIZE : remaining;
        s_hosted_ota_result = esp_hosted_slave_ota_write(
            (uint8_t *)(s_c6_firmware_start + s_hosted_ota_bytes), chunk_size);
        if (s_hosted_ota_result != ESP_OK) {
            ESP_LOGE(TAG, "C6 OTA write failed at %" PRIu32 "/%u: %s",
                     s_hosted_ota_bytes, (unsigned)firmware_size,
                     esp_err_to_name(s_hosted_ota_result));
            esp_hosted_slave_ota_end();
            return;
        }
        s_hosted_ota_bytes += chunk_size;
        if ((s_hosted_ota_bytes % (100U * C6_OTA_CHUNK_SIZE)) < chunk_size) {
            ESP_LOGI(TAG, "C6 OTA progress %" PRIu32 "/%u",
                     s_hosted_ota_bytes, (unsigned)firmware_size);
        }
    }

    s_hosted_ota_result = esp_hosted_slave_ota_end();
    if (s_hosted_ota_result != ESP_OK) {
        ESP_LOGE(TAG, "C6 OTA finalize failed: %s",
                 esp_err_to_name(s_hosted_ota_result));
        return;
    }

    if (s_hosted_coprocessor_major > 2 ||
        (s_hosted_coprocessor_major == 2 && s_hosted_coprocessor_minor >= 6)) {
        s_hosted_ota_result = esp_hosted_slave_ota_activate();
        if (s_hosted_ota_result != ESP_OK) {
            ESP_LOGE(TAG, "C6 OTA activate failed: %s",
                     esp_err_to_name(s_hosted_ota_result));
            return;
        }
    } else {
        ESP_LOGI(TAG, "C6 %s uses legacy OTA activation at finalize",
                 s_hosted_coprocessor_version);
    }

    s_hosted_ota_result =
        hosted_ota_store_u32(C6_OTA_REVISION_NVS_KEY,
                             C6_OTA_IMAGE_REVISION);
    if (s_hosted_ota_result != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist applied C6 OTA revision: %s",
                 esp_err_to_name(s_hosted_ota_result));
        return;
    }
    s_hosted_ota_applied_revision = C6_OTA_IMAGE_REVISION;
    esp_err_t clear_result = hosted_ota_store_u32(C6_OTA_NVS_KEY, 0);
    if (clear_result != ESP_OK) {
        ESP_LOGW(TAG, "failed to clear completed C6 OTA attempt counter: %s",
                 esp_err_to_name(clear_result));
    }

    s_hosted_ota_performed = true;
    ESP_LOGW(TAG, "C6 OTA complete; restarting P4 to resynchronize SDIO");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}
#endif

static void init_wifi(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
#if CONFIG_IDF_TARGET_ESP32P4
    log_hosted_coprocessor_version();
    maybe_update_hosted_coprocessor();
#endif
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, GATEWAY_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, GATEWAY_WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void init_uart(void)
{
    ESP_ERROR_CHECK(configure_ra8_uart_driver());
    ESP_LOGI(TAG, "RA8 UART ready: RX=GPIO%d TX=GPIO%d baud=%d 8N1", RA8_UART_RX_PIN,
             RA8_UART_TX_PIN, RA8_UART_BAUD);
#if CONFIG_IDF_TARGET_ESP32P4
    esp_err_t bulk_result = story_bulk_uart_init();
    if (bulk_result != ESP_OK) {
        ESP_LOGW(TAG, "story bulk UART unavailable: %s; control-UART fallback remains enabled",
                 esp_err_to_name(bulk_result));
    }
#endif
}

#if CONFIG_IDF_TARGET_ESP32P4
static void ra8_soft_reset_pulse(void)
{
    if (!s_ra8_reset_pin_ready) {
        return;
    }

    gpio_set_level(RA8_SOFT_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(RA8_RESET_PULSE_MS));
    gpio_set_level(RA8_SOFT_RESET_PIN, 1);
    s_ra8_reset_last_pulse_ms = now_ms();
    ++s_ra8_reset_pulse_count;
    ESP_LOGW(TAG, "RA8 soft-reset pulse #%lu on GPIO%d",
             (unsigned long)s_ra8_reset_pulse_count, RA8_SOFT_RESET_PIN);
}

static void ra8_soft_reset_watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now = now_ms();
        uint32_t status_age = s_stats.last_status_ms ?
                              elapsed_ms(now, s_stats.last_status_ms) : UINT32_MAX;
        uint32_t transport_age = s_stats.last_valid_frame_ms ?
                                 elapsed_ms(now, s_stats.last_valid_frame_ms) : UINT32_MAX;
        bool boot_grace_expired =
            elapsed_ms(now, s_ra8_reset_started_ms) >= RA8_RESET_BOOT_GRACE_MS;
        bool status_stale = s_stats.last_status_ms ? status_age >= RA8_RESET_OFFLINE_MS
                                                   : boot_grace_expired;
        bool transport_silent = s_stats.last_valid_frame_ms ?
                                transport_age >= RA8_RESET_LINK_SILENCE_MS :
                                boot_grace_expired;
        bool cooldown_expired = !s_ra8_reset_last_pulse_ms ||
                                elapsed_ms(now, s_ra8_reset_last_pulse_ms) >=
                                RA8_RESET_COOLDOWN_MS;
        bool story_reset_guard = s_story_active ||
                                 (s_story_finished_ms &&
                                  (elapsed_ms(now, s_story_finished_ms) < STORY_RESET_GRACE_MS));

        bool local_uart_recovered = s_stats.uart_rx_recovery_count > 0U;

        if (status_stale && transport_silent && local_uart_recovered && boot_grace_expired &&
            cooldown_expired && !story_reset_guard) {
            ra8_soft_reset_pulse();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void init_ra8_soft_reset(void)
{
    /* Preload released before enabling the open-drain output. Level 1 is Hi-Z. */
    ESP_ERROR_CHECK(gpio_set_level(RA8_SOFT_RESET_PIN, 1));
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << RA8_SOFT_RESET_PIN,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_ERROR_CHECK(gpio_set_level(RA8_SOFT_RESET_PIN, 1));
    s_ra8_reset_pin_ready = true;
    s_ra8_reset_started_ms = now_ms();
    ESP_LOGI(TAG, "RA8 soft-reset output released on GPIO%d", RA8_SOFT_RESET_PIN);
}
#endif

static void init_websocket(void)
{
    char headers[256];
    snprintf(headers, sizeof(headers), "X-Device-ID: %s\r\nX-Device-Key: %s\r\n",
             GATEWAY_DEVICE_ID, GATEWAY_DEVICE_KEY);
    esp_websocket_client_config_t config = {
        .uri = WSS_URI,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = TELEMETRY_WSS_BUFFER_BYTES,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 2000,
        .ping_interval_sec = 5,
        .enable_close_reconnect = true,
    };
    s_ws = esp_websocket_client_init(&config);
    if (!s_ws) {
        ESP_LOGE(TAG, "failed to initialize WSS client");
        abort();
    }
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                                   websocket_event_handler, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
    s_last_ws_success_ms = now_ms();
    s_ws_watchdog_armed = true;
    s_ws_recovery_grace_until_ms = s_last_ws_success_ms + TELEMETRY_RECOVERY_GRACE_MS;
}

static void init_matrix_websocket(void)
{
    char headers[256];
    snprintf(headers, sizeof(headers), "X-Device-ID: %s\r\nX-Device-Key: %s\r\n",
             GATEWAY_DEVICE_ID, GATEWAY_DEVICE_KEY);
    esp_websocket_client_config_t config = {
        .uri = MATRIX_WSS_URI,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = MATRIX_WSS_BUFFER_BYTES,
        .network_timeout_ms = 5000,
        /*
         * A matrix relay failure must not create repeated TLS handshakes that
         * steal CPU and airtime from telemetry, audio and video.
         */
        .reconnect_timeout_ms = 10000,
        .ping_interval_sec = 15,
        .enable_close_reconnect = true,
    };
    s_matrix_ws = esp_websocket_client_init(&config);
    if (!s_matrix_ws) {
        ESP_LOGE(TAG, "failed to initialize matrix WSS client");
        abort();
    }
    ESP_ERROR_CHECK(esp_websocket_register_events(
        s_matrix_ws, WEBSOCKET_EVENT_ANY, matrix_websocket_event_handler, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_matrix_ws));
}

static void init_http(void)
{
    esp_http_client_config_t config = {
        .url = SERVER_ORIGIN "/api/v1/batch",
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 5000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = true,
    };
    s_http = esp_http_client_init(&config);
    if (!s_http) {
        ESP_LOGE(TAG, "failed to initialize persistent HTTPS client");
        abort();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "booting %s on %s", FIRMWARE_VERSION, HARDWARE_NAME);
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }
#if CONFIG_IDF_TARGET_ESP32P4
    init_ra8_soft_reset();
#endif
    nvs_handle_t nvs;
    if (nvs_open("gateway", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "last_cmd", &s_last_command_id);
        nvs_close(nvs);
    }
    s_status_mutex = xSemaphoreCreateMutex();
    s_uart_tx_mutex = xSemaphoreCreateMutex();
    s_matrix_mutex = xSemaphoreCreateMutex();
#if CONFIG_IDF_TARGET_ESP32P4
    s_story_bulk_uart_mutex = xSemaphoreCreateMutex();
#endif
    ESP_ERROR_CHECK(realtime_tx_guard_init());
    s_event_queue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(safety_event_t));
    s_command_queue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(server_command_t));
    s_command_ack_queue = xQueueCreate(COMMAND_ACK_QUEUE_LENGTH, sizeof(command_ack_t));
    s_story_queue = xQueueCreate(STORY_QUEUE_LENGTH, sizeof(server_command_t));
    s_story_result_queue = xQueueCreate(STORY_RESULT_QUEUE_LENGTH, sizeof(story_result_t));
    s_thermal.width = 80; s_thermal.height = 64;
    s_pressure.width = 16; s_pressure.height = 16;
    s_thermal.samples = heap_caps_calloc(80U * 64U, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_thermal.seen = heap_caps_calloc((80U * 64U + 7U) / 8U, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pressure.samples = heap_caps_calloc(16U * 16U, sizeof(int16_t), MALLOC_CAP_8BIT);
    s_pressure.seen = heap_caps_calloc((16U * 16U + 7U) / 8U, 1, MALLOC_CAP_8BIT);
    s_ws_text_capacity = 4096;
    s_ws_text = heap_caps_malloc(s_ws_text_capacity, MALLOC_CAP_8BIT);
    if (!s_status_mutex || !s_uart_tx_mutex || !s_matrix_mutex ||
#if CONFIG_IDF_TARGET_ESP32P4
        !s_story_bulk_uart_mutex ||
#endif
        !s_event_queue || !s_command_queue ||
        !s_command_ack_queue || !s_story_queue || !s_story_result_queue ||
        !s_thermal.samples || !s_thermal.seen || !s_pressure.samples || !s_pressure.seen || !s_ws_text) {
        ESP_LOGE(TAG, "gateway allocation failed");
        abort();
    }
    init_uart();
    if (xTaskCreatePinnedToCore(uart_rx_task, "ra8_uart", 8192, NULL, 8, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "UART task creation failed");
        abort();
    }
#if CONFIG_IDF_TARGET_ESP32P4
    if (xTaskCreatePinnedToCore(story_bulk_link_task, "story_uart_link", 3072,
                                NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "story UART link task creation failed");
        abort();
    }
    if (xTaskCreatePinnedToCore(ra8_soft_reset_watchdog_task, "ra8_reset", 3072,
                                NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "RA8 soft-reset watchdog creation failed");
        abort();
    }
#endif
#if CONFIG_IDF_TARGET_ESP32P4
    /*
     * PDM descriptors require internal DMA memory. Reserve them before Wi-Fi,
     * TLS and camera clients fragment the internal heap.
     */
    esp_err_t microphone_prepare_result = mic_stream_prepare();
    if (microphone_prepare_result != ESP_OK) {
        ESP_LOGE(TAG, "microphone DMA reservation failed: %s",
                 esp_err_to_name(microphone_prepare_result));
    }
#endif
    init_wifi();
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    init_http();
    /*
     * Bring up the control/telemetry path first, then stagger every TLS client.
     * This keeps media handshakes from starving telemetry and avoids overlapping
     * P4 AES-DMA operations during boot.
     */
    init_websocket();
    if (xTaskCreatePinnedToCore(network_task, "gateway_net", 12288, NULL, 6, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "network task creation failed");
        abort();
    }
    if (xTaskCreatePinnedToCore(story_task, "story_transfer", 8192, NULL,
                                STORY_TASK_PRIORITY, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "story transfer task creation failed");
        abort();
    }
#if CONFIG_IDF_TARGET_ESP32P4
    vTaskDelay(pdMS_TO_TICKS(1800));
#endif
    init_matrix_websocket();
    if (xTaskCreatePinnedToCore(matrix_network_task, "gateway_matrix", 6144, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "matrix network task creation failed");
        abort();
    }
#if CONFIG_IDF_TARGET_ESP32P4
    vTaskDelay(pdMS_TO_TICKS(1500));
#endif
    esp_err_t camera_result = camera_stream_start();
    if (camera_result != ESP_OK) {
        ESP_LOGE(TAG, "camera stream unavailable: %s", esp_err_to_name(camera_result));
    }
#if CONFIG_IDF_TARGET_ESP32P4
    vTaskDelay(pdMS_TO_TICKS(1500));
#endif
    esp_err_t microphone_result = mic_stream_start();
    if (microphone_result != ESP_OK) {
        ESP_LOGE(TAG, "microphone stream unavailable: %s", esp_err_to_name(microphone_result));
    }
}

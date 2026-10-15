#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
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
#include "nvs.h"
#include "nvs_flash.h"

#include "gateway_secrets.h"
#include "camera_stream.h"
#include "mic_stream.h"

#define FIRMWARE_VERSION "esp32s3cam-ra8-gateway-3.3.0"
#define WSS_URI "wss://renesas.zyxserver.online/api/v1/device/stream"
#define MATRIX_WSS_URI "wss://renesas.zyxserver.online/api/v1/device/matrices"

#define RA8_UART UART_NUM_1
#define RA8_UART_RX_PIN 1
#define RA8_UART_TX_PIN 2
#define RA8_UART_BAUD 921600

#define RA8_PROTOCOL_VERSION 1
#define RA8_TYPE_STATUS_UPLOAD 0x10
#define RA8_TYPE_THERMAL_CHUNK 0x11
#define RA8_TYPE_PRESSURE_UPLOAD 0x12
#define RA8_TYPE_SAFETY_EVENT 0x13
#define RA8_TYPE_CMD 0x20
#define RA8_TYPE_SAFETY_ACK 0x22
#define RA8_MAX_PAYLOAD 4096

#define WIFI_CONNECTED_BIT BIT0
#define EVENT_QUEUE_LENGTH 8
#define COMMAND_QUEUE_LENGTH 8
#define COMMAND_RESULT_CACHE_LENGTH 16
#define TELEMETRY_INTERVAL_MS 500
#define THERMAL_INTERVAL_MS 500
#define PRESSURE_INTERVAL_MS 250
#define HEARTBEAT_INTERVAL_MS 5000
#define WSS_PING_INTERVAL_SEC 300
#define WSS_SEND_TIMEOUT_MS 3000
#define WSS_SEND_RETRY_MS 100
#define TELEMETRY_ACK_TIMEOUT_MS 4000
#define TELEMETRY_ACK_RESTART_THRESHOLD 3
#define MATRIX_WSS_SEND_TIMEOUT_MS 5000
#define MATRIX_ACK_TIMEOUT_MS 2000
#define MATRIX_SEND_RETRY_MS 200
#define MATRIX_TEXT_BUFFER_SIZE 512U
#define TELEMETRY_BUFFER_SIZE 16384U
#define MATRIX_TX_BUFFER_SIZE (14U + 80U * 64U * sizeof(int16_t))
#define RA8_OFFLINE_MS 2000
#define RA8_READY_INTERVAL_MS 2000

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
    bool valid;
    bool ok;
    char message[96];
} command_result_t;

typedef struct {
    uint32_t valid_frames;
    uint32_t status_frames;
    uint32_t crc_errors;
    uint32_t length_errors;
    uint32_t event_drops;
    uint32_t last_status_ms;
    uint16_t last_transport_sequence;
} gateway_stats_t;

typedef struct {
    int16_t *samples;
    uint8_t *seen;
    uint32_t sequence;
    uint32_t completed_ms;
    uint16_t received;
    uint16_t width;
    uint16_t height;
    bool pending;
} matrix_state_t;

typedef struct {
    matrix_state_t *source;
    uint8_t kind;
    uint32_t interval_ms;
    uint32_t upload_sequence;
    uint32_t source_sequence;
    uint32_t sent_at_ms;
    uint32_t last_submission_ms;
    uint32_t last_attempt_ms;
    uint32_t source_frames;
    uint32_t sent_frames;
    uint32_t ack_timeouts;
    uint32_t retries;
    uint32_t send_failures;
    uint32_t last_ack_rtt_ms;
    uint32_t max_ack_rtt_ms;
    size_t frame_length;
    uint8_t *frame;
    bool prepared;
    bool inflight;
} matrix_delivery_t;

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_status_mutex;
static SemaphoreHandle_t s_uart_tx_mutex;
static SemaphoreHandle_t s_matrix_mutex;
static SemaphoreHandle_t s_telemetry_mutex;
static QueueHandle_t s_event_queue;
static QueueHandle_t s_command_queue;
static esp_websocket_client_handle_t s_ws;
static esp_websocket_client_handle_t s_matrix_ws;
static volatile bool s_ws_connected;
static volatile bool s_matrix_ws_connected;
static volatile bool s_ws_restart_needed;
static volatile uint32_t s_ws_session_generation;
static volatile uint32_t s_server_acked_event;
static volatile bool s_server_acked_event_valid;
static volatile uint32_t s_server_acked_telemetry_sequence;
static volatile bool s_server_acked_telemetry_valid;
static portMUX_TYPE s_telemetry_ack_mux = portMUX_INITIALIZER_UNLOCKED;
static gateway_stats_t s_stats;
static char *s_latest_status;
static char *s_status_snapshot;
static uint16_t s_uart_tx_sequence = 1;
static uint32_t s_telemetry_sequence = 1;
static uint32_t s_last_command_id;
static command_result_t s_command_results[COMMAND_RESULT_CACHE_LENGTH];
static size_t s_command_result_cursor;
static matrix_state_t s_thermal;
static matrix_state_t s_pressure;
static matrix_delivery_t s_thermal_delivery;
static matrix_delivery_t s_pressure_delivery;
static char *s_ws_text;
static size_t s_ws_text_capacity;
static size_t s_ws_text_length;
static uint32_t s_ws_text_expected;
static char *s_matrix_text;
static size_t s_matrix_text_length;
static uint32_t s_matrix_text_expected;
static volatile uint32_t s_matrix_acked_sequence[2];
static volatile bool s_matrix_ack_valid[2];
static portMUX_TYPE s_matrix_ack_mux = portMUX_INITIALIZER_UNLOCKED;
static char *s_telemetry_latest;
static char *s_telemetry_tx;
static size_t s_telemetry_latest_length;
static uint32_t s_last_telemetry_produced_ms;
static uint32_t s_telemetry_latest_sequence;
static uint32_t s_telemetry_generation;
static uint32_t s_telemetry_copied_generation;
static uint32_t s_telemetry_sent_generation;
static uint32_t s_telemetry_produced;
static uint32_t s_telemetry_sent;
static uint32_t s_telemetry_overwritten;
static uint32_t s_telemetry_ack_timeouts;
static uint8_t s_consecutive_telemetry_ack_timeouts;
static uint32_t s_ws_send_failures;
static uint32_t s_last_ws_send_ms;
static uint32_t s_max_ws_send_ms;
static uint32_t s_last_matrix_send_ms;
static uint32_t s_max_matrix_send_ms;
static uint32_t s_matrix_ws_send_failures;
static uint32_t s_last_telemetry_ack_rtt_ms;
static uint32_t s_max_telemetry_ack_rtt_ms;
static uint32_t s_heartbeat_sequence = 1;
static bool s_telemetry_inflight;
static uint32_t s_telemetry_inflight_generation;
static uint32_t s_telemetry_inflight_sequence;
static uint32_t s_telemetry_inflight_sent_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
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

static esp_err_t send_ra8_frame(uint8_t type, const uint8_t *payload, uint16_t length)
{
    if (length > RA8_MAX_PAYLOAD || !s_uart_tx_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_uart_tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t header[8] = {
        0xAA, 0x55, RA8_PROTOCOL_VERSION, type,
        (uint8_t)(s_uart_tx_sequence >> 8), (uint8_t)s_uart_tx_sequence,
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
        xSemaphoreGive(s_uart_tx_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_uart_tx_mutex);
    return ESP_FAIL;
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
    if (!matrix->pending && matrix->received == total) {
        matrix->pending = true;
        matrix->completed_ms = now_ms();
    }
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
    for (;;) {
        uint32_t now = now_ms();
        if (!last_ready_ms || now - last_ready_ms >= RA8_READY_INTERVAL_MS) {
            static const uint8_t ready[] = "gateway_ready";
            if (send_ra8_frame(RA8_TYPE_CMD, ready, sizeof(ready) - 1U) == ESP_OK) {
                last_ready_ms = now;
            }
        }
        int count = uart_read_bytes(RA8_UART, input, sizeof(input), pdMS_TO_TICKS(20));
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
        }
    }
}

static bool websocket_send_json(const char *kind, cJSON *payload)
{
    if (!s_ws_connected || !s_ws || !esp_websocket_client_is_connected(s_ws) ||
        !kind || !payload) {
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
    int sent = esp_websocket_client_send_text(
        s_ws, text, length, pdMS_TO_TICKS(WSS_SEND_TIMEOUT_MS));
    uint32_t elapsed = now_ms() - started;
    if (sent != length) {
        ESP_LOGW(TAG, "WSS JSON %s write failed sent=%d/%d elapsed=%lums",
                 kind, sent, length, (unsigned long)elapsed);
    }
    free(text);
    return sent == length;
}

static bool send_hello(void)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "protocol_version", 1);
    cJSON_AddStringToObject(body, "firmware", FIRMWARE_VERSION);
    cJSON_AddStringToObject(body, "hardware", "ESP32-S3-WROOM-CAM-N16R8");
    cJSON *caps = cJSON_AddArrayToObject(body, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("ra8_gateway"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("thermal"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("pressure"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("environment_scd41"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("safety_events"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("ra8_audio_collection"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("ra8_camera_collection"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("s3_external_microphone"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("s3_camera_jpeg_wss"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("wss_telemetry_v1"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("latest_only"));
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
    cJSON_AddBoolToObject(meta, "media_enabled", true);
    cJSON_AddStringToObject(meta, "camera_format", "JPEG");
    cJSON_AddStringToObject(meta, "camera_default", "640x480@8fps");
    bool ok = websocket_send_json("hello", body);
    cJSON_Delete(body);
    ESP_LOGI(TAG, "WSS hello %s", ok ? "sent" : "failed");
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

static void add_sensor_summary(cJSON *sensors,
                               const char *name,
                               cJSON *source,
                               const char *const *fields,
                               size_t field_count)
{
    if (!cJSON_IsObject(source)) return;
    cJSON *summary = cJSON_AddObjectToObject(sensors, name);
    if (!summary) return;
    for (size_t i = 0; i < field_count; ++i) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(source, fields[i]);
        if (value) {
            cJSON_AddItemToObject(summary, fields[i], cJSON_Duplicate(value, true));
        }
    }
}

static char *build_telemetry_json(uint32_t *sequence_out)
{
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(s_status_snapshot, s_latest_status, RA8_MAX_PAYLOAD + 1U);
        s_status_snapshot[RA8_MAX_PAYLOAD] = '\0';
        xSemaphoreGive(s_status_mutex);
    } else {
        strcpy(s_status_snapshot, "{}");
    }
    cJSON *ra8 = cJSON_Parse(s_status_snapshot);
    if (!ra8) ra8 = cJSON_CreateObject();
    cJSON *body = cJSON_CreateObject();
    uint32_t sequence = s_telemetry_sequence++;
    if (sequence_out) *sequence_out = sequence;
    cJSON_AddNumberToObject(body, "protocol_version", 1);
    cJSON_AddNumberToObject(body, "sequence", sequence);
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
    cJSON *prs = cJSON_GetObjectItemCaseSensitive(ra8, "prs");
    cJSON *mic = cJSON_GetObjectItemCaseSensitive(ra8, "mic");
    cJSON *aud = cJSON_GetObjectItemCaseSensitive(ra8, "aud");
    cJSON *cam = cJSON_GetObjectItemCaseSensitive(ra8, "cam");
    static const char *thermal_fields[] = {
        "seq", "s", "rx", "min_cd", "max_cd", "ctr_cd", "avg_cd", "err",
        "uart_err", "ovf", "valid", "crc_rx", "crc_calc", "fifo_ovf",
        "crc_bad", "data_bad", "build"
    };
    static const char *temperature_fields[] = {
        "seq", "s", "ok", "err", "fs", "ch", "fsp", "pca", "t_mc", "addr"
    };
    static const char *environment_fields[] = {
        "seq", "s", "co2", "t_mc", "rh_mp", "ready", "crc_err",
        "i2c_err", "to", "rec", "fsp", "last"
    };
    static const char *pressure_fields[] = {
        "seq", "s", "ok", "err", "fs", "ch", "fsp", "up_ch"
    };
    static const char *microphone_fields[] = {
        "seq", "cmd_seq", "ack", "s", "rate", "bits", "cb", "q", "read",
        "err", "to", "fsp", "ev", "peak", "mean", "nz", "rec", "rec_bytes",
        "rec_sec", "wst", "werr", "wpoll", "wopen", "wwr", "wbytes",
        "wseq", "wchunk", "slot", "shift", "gain",
        /* CPU1 SD diagnostics are published by RA8 inside the mic object.
         * Keep these compact scalars in real-time telemetry; only raw audio
         * samples and large sensor matrices belong on bulk media paths. */
        "sdr", "sds", "sdfs", "sdfl", "sdtry", "sdfail", "sdrec",
        "sdfsp", "sdff", "file"
    };
    static const char *audio_fields[] = {
        "seq", "cmd_seq", "ack", "s", "track", "count", "vol", "elapsed",
        "duration", "rate", "file"
    };
    add_sensor_summary(sensors,
                       "thermal",
                       thm,
                       thermal_fields,
                       sizeof(thermal_fields) / sizeof(thermal_fields[0]));
    add_sensor_summary(sensors,
                       "bed_temperature",
                       tmp,
                       temperature_fields,
                       sizeof(temperature_fields) / sizeof(temperature_fields[0]));
    add_sensor_summary(sensors,
                       "environment",
                       scd,
                       environment_fields,
                       sizeof(environment_fields) / sizeof(environment_fields[0]));
    if (cJSON_IsObject(prs)) {
        capture_pressure_from_status(prs);
        add_sensor_summary(sensors,
                           "pressure",
                           prs,
                           pressure_fields,
                           sizeof(pressure_fields) / sizeof(pressure_fields[0]));
    }
    add_sensor_summary(sensors,
                       "microphone",
                       mic,
                       microphone_fields,
                       sizeof(microphone_fields) / sizeof(microphone_fields[0]));
    add_sensor_summary(sensors,
                       "audio_player",
                       aud,
                       audio_fields,
                       sizeof(audio_fields) / sizeof(audio_fields[0]));
    add_duplicate_if_object(sensors, "camera", cam);
    cJSON *fusion = cJSON_GetObjectItemCaseSensitive(ra8, "fusion");
    if (cJSON_IsObject(fusion)) cJSON_AddItemToObject(body, "inference", cJSON_Duplicate(fusion, true));
    else cJSON_AddObjectToObject(body, "inference");
    /* Vision boxes stay in the serial-camera coordinate system.  The server
     * applies the per-device calibration before overlaying them on S3 video. */
    add_duplicate_if_object(body,
                            "vision",
                            cJSON_GetObjectItemCaseSensitive(ra8, "vision"));
    add_duplicate_if_object(body,
                            "safety",
                            cJSON_GetObjectItemCaseSensitive(ra8, "safe"));

    cJSON *health = cJSON_AddObjectToObject(body, "health");
    uint32_t current_ms = now_ms();
    uint32_t age = s_stats.last_status_ms ? current_ms - s_stats.last_status_ms : UINT32_MAX;
    uint32_t thermal_completed_ms = 0;
    uint32_t pressure_completed_ms = 0;
    if (xSemaphoreTake(s_matrix_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        thermal_completed_ms = s_thermal.completed_ms;
        pressure_completed_ms = s_pressure.completed_ms;
        xSemaphoreGive(s_matrix_mutex);
    }
    uint32_t thermal_age_ms =
        thermal_completed_ms ? current_ms - thermal_completed_ms : UINT32_MAX;
    uint32_t pressure_age_ms =
        pressure_completed_ms ? current_ms - pressure_completed_ms : UINT32_MAX;
    cJSON_AddBoolToObject(health, "ra8_online", s_stats.last_status_ms && age <= RA8_OFFLINE_MS);
    cJSON_AddNumberToObject(health, "ra8_status_age_ms", age);
    cJSON_AddNumberToObject(health, "uart_valid_frames", s_stats.valid_frames);
    cJSON_AddNumberToObject(health, "uart_status_frames", s_stats.status_frames);
    cJSON_AddNumberToObject(health, "uart_crc_errors", s_stats.crc_errors);
    cJSON_AddNumberToObject(health, "uart_length_errors", s_stats.length_errors);
    cJSON_AddNumberToObject(health, "event_queue_drops", s_stats.event_drops);
    cJSON_AddBoolToObject(health, "ws_connected", s_ws_connected);
    cJSON_AddBoolToObject(health, "matrix_ws_connected", s_matrix_ws_connected);
    cJSON_AddBoolToObject(health, "batch_connected", false);
    cJSON_AddNumberToObject(health, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(health, "free_internal_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(health, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    add_duplicate_if_object(health, "safety", cJSON_GetObjectItemCaseSensitive(ra8, "safe"));
    add_duplicate_if_object(health, "ra8_link", cJSON_GetObjectItemCaseSensitive(ra8, "link"));
    cJSON *gateway = cJSON_AddObjectToObject(health, "gateway");
    cJSON_AddStringToObject(gateway, "transport", "independent_telemetry_matrix_wss");
    cJSON_AddNumberToObject(gateway, "last_ws_send_ms", s_last_ws_send_ms);
    cJSON_AddNumberToObject(gateway, "max_ws_send_ms", s_max_ws_send_ms);
    cJSON_AddNumberToObject(gateway, "last_matrix_send_ms", s_last_matrix_send_ms);
    cJSON_AddNumberToObject(gateway, "max_matrix_send_ms", s_max_matrix_send_ms);
    cJSON_AddNumberToObject(gateway, "ws_send_failures", s_ws_send_failures);
    cJSON_AddNumberToObject(gateway, "matrix_ws_send_failures", s_matrix_ws_send_failures);
    cJSON_AddNumberToObject(gateway, "pressure_matrix_sent", s_pressure_delivery.sent_frames);
    cJSON_AddNumberToObject(gateway, "thermal_matrix_sent", s_thermal_delivery.sent_frames);
    cJSON_AddNumberToObject(gateway,
                            "pressure_matrix_source_frames",
                            s_pressure_delivery.source_frames);
    cJSON_AddNumberToObject(gateway,
                            "thermal_matrix_source_frames",
                            s_thermal_delivery.source_frames);
    cJSON_AddNumberToObject(gateway,
                            "pressure_matrix_ack_timeouts",
                            s_pressure_delivery.ack_timeouts);
    cJSON_AddNumberToObject(gateway,
                            "thermal_matrix_ack_timeouts",
                            s_thermal_delivery.ack_timeouts);
    cJSON_AddNumberToObject(gateway, "telemetry_produced", s_telemetry_produced);
    cJSON_AddNumberToObject(gateway, "telemetry_sent", s_telemetry_sent);
    cJSON_AddNumberToObject(gateway, "telemetry_overwritten", s_telemetry_overwritten);
    cJSON_AddNumberToObject(gateway,
                            "telemetry_payload_bytes",
                            s_telemetry_latest_length);
    cJSON_AddNumberToObject(gateway, "telemetry_ack_timeouts", s_telemetry_ack_timeouts);
    cJSON_AddNumberToObject(gateway,
                            "consecutive_telemetry_ack_timeouts",
                            s_consecutive_telemetry_ack_timeouts);
    cJSON_AddNumberToObject(gateway, "ra8_status_age_ms", age);
    cJSON_AddNumberToObject(gateway, "thermal_age_ms", thermal_age_ms);
    cJSON_AddNumberToObject(gateway, "pressure_age_ms", pressure_age_ms);
    cJSON *esp32 = cJSON_AddObjectToObject(health, "esp32");
    cJSON_AddNumberToObject(esp32, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(esp32, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(esp32, "firmware", FIRMWARE_VERSION);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    cJSON_Delete(ra8);
    return json;
}

static void telemetry_producer_task(void *arg)
{
    (void)arg;
    TickType_t next_wake = xTaskGetTickCount();
    for (;;) {
        uint32_t sequence = 0;
        char *payload = build_telemetry_json(&sequence);
        if (payload) {
            size_t payload_length = strlen(payload);
            static const char prefix[] = "{\"kind\":\"telemetry\",\"payload\":";
            size_t required = sizeof(prefix) - 1U + payload_length + 2U;
            if (required <= TELEMETRY_BUFFER_SIZE &&
                xSemaphoreTake(s_telemetry_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (s_telemetry_generation != s_telemetry_copied_generation) {
                    ++s_telemetry_overwritten;
                }
                int written = snprintf(
                    s_telemetry_latest,
                    TELEMETRY_BUFFER_SIZE,
                    "%s%s}",
                    prefix,
                    payload);
                if (written > 0 && (size_t)written < TELEMETRY_BUFFER_SIZE) {
                    s_telemetry_latest_length = (size_t)written;
                    s_last_telemetry_produced_ms = now_ms();
                    s_telemetry_latest_sequence = sequence;
                    ++s_telemetry_generation;
                    ++s_telemetry_produced;
                }
                xSemaphoreGive(s_telemetry_mutex);
            } else if (required > TELEMETRY_BUFFER_SIZE) {
                ESP_LOGE(TAG, "telemetry frame too large: %u bytes", (unsigned)required);
            }
            free(payload);
        }
        vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
    }
}

static bool send_latest_telemetry(uint32_t *last_attempt_at)
{
    uint32_t now = now_ms();
    if (!s_ws_connected ||
        (*last_attempt_at && now - *last_attempt_at < WSS_SEND_RETRY_MS)) {
        return false;
    }

    uint32_t generation = 0;
    uint32_t sequence = 0;
    size_t length = 0;
    if (xSemaphoreTake(s_telemetry_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (!s_telemetry_inflight &&
            s_telemetry_generation != s_telemetry_sent_generation &&
            s_telemetry_latest_length > 0) {
            generation = s_telemetry_generation;
            sequence = s_telemetry_latest_sequence;
            length = s_telemetry_latest_length;
            memcpy(s_telemetry_tx, s_telemetry_latest, length + 1U);
            s_telemetry_copied_generation = generation;
            s_telemetry_inflight = true;
            s_telemetry_inflight_generation = generation;
            s_telemetry_inflight_sequence = sequence;
            s_telemetry_inflight_sent_ms = 0;
        }
        xSemaphoreGive(s_telemetry_mutex);
    }
    if (!generation) return false;

    uint32_t started = now_ms();
    int sent = esp_websocket_client_send_text(
        s_ws, s_telemetry_tx, (int)length, pdMS_TO_TICKS(WSS_SEND_TIMEOUT_MS));
    uint32_t elapsed = now_ms() - started;
    *last_attempt_at = now_ms();
    s_last_ws_send_ms = elapsed;
    if (elapsed > s_max_ws_send_ms) s_max_ws_send_ms = elapsed;
    if (sent == (int)length) {
        /*
         * The WebSocket API is synchronous.  Start the application ACK
         * deadline after the complete frame has reached the transport, not
         * before a potentially slow-but-successful public-network write.
         */
        if (xSemaphoreTake(s_telemetry_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_telemetry_inflight && s_telemetry_inflight_sequence == sequence) {
                s_telemetry_inflight_sent_ms = now_ms();
            }
            xSemaphoreGive(s_telemetry_mutex);
        }
        return true;
    }
    ESP_LOGW(TAG, "telemetry write failed sequence=%lu sent=%d/%u elapsed=%lums",
             (unsigned long)sequence, sent, (unsigned)length, (unsigned long)elapsed);
    if (xSemaphoreTake(s_telemetry_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_telemetry_inflight && s_telemetry_inflight_sequence == sequence) {
            s_telemetry_inflight = false;
        }
        xSemaphoreGive(s_telemetry_mutex);
    }
    ++s_ws_send_failures;
    return false;
}

static void consume_telemetry_ack(void)
{
    if (xSemaphoreTake(s_telemetry_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        bool ack_valid;
        uint32_t sequence;
        portENTER_CRITICAL(&s_telemetry_ack_mux);
        ack_valid = s_server_acked_telemetry_valid;
        sequence = s_server_acked_telemetry_sequence;
        s_server_acked_telemetry_valid = false;
        portEXIT_CRITICAL(&s_telemetry_ack_mux);
        if (ack_valid && s_telemetry_inflight &&
            s_telemetry_inflight_sequence == sequence) {
            uint32_t ack_rtt = s_telemetry_inflight_sent_ms
                                   ? now_ms() - s_telemetry_inflight_sent_ms
                                   : 0;
            s_last_telemetry_ack_rtt_ms = ack_rtt;
            if (ack_rtt > s_max_telemetry_ack_rtt_ms) {
                s_max_telemetry_ack_rtt_ms = ack_rtt;
            }
            s_telemetry_sent_generation = s_telemetry_inflight_generation;
            s_telemetry_inflight = false;
            s_consecutive_telemetry_ack_timeouts = 0;
            ++s_telemetry_sent;
        }
        xSemaphoreGive(s_telemetry_mutex);
    }
}

static void reset_telemetry_delivery_state(void)
{
    if (xSemaphoreTake(s_telemetry_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_telemetry_inflight = false;
        s_telemetry_copied_generation = s_telemetry_sent_generation;
        portENTER_CRITICAL(&s_telemetry_ack_mux);
        s_server_acked_telemetry_valid = false;
        portEXIT_CRITICAL(&s_telemetry_ack_mux);
        xSemaphoreGive(s_telemetry_mutex);
    }
}

static bool telemetry_ack_timed_out(void)
{
    bool timed_out = false;
    if (xSemaphoreTake(s_telemetry_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        portENTER_CRITICAL(&s_telemetry_ack_mux);
        bool matching_ack_waiting = s_server_acked_telemetry_valid &&
                                    s_server_acked_telemetry_sequence ==
                                        s_telemetry_inflight_sequence;
        portEXIT_CRITICAL(&s_telemetry_ack_mux);
        timed_out = s_telemetry_inflight &&
                    s_telemetry_inflight_sent_ms != 0 &&
                    !matching_ack_waiting &&
                    now_ms() - s_telemetry_inflight_sent_ms >= TELEMETRY_ACK_TIMEOUT_MS;
        xSemaphoreGive(s_telemetry_mutex);
    }
    return timed_out;
}

static bool send_pending_event(uint32_t *last_sent_at)
{
    safety_event_t event;
    if (xQueuePeek(s_event_queue, &event, 0) != pdTRUE) return false;
    if (s_server_acked_event_valid && s_server_acked_event == event.event_id) {
        xQueueReceive(s_event_queue, &event, 0);
        s_server_acked_event_valid = false;
        *last_sent_at = 0;
        ESP_LOGI(TAG, "server accepted safety event %lu", (unsigned long)event.event_id);
        return false;
    }
    uint32_t now = now_ms();
    if (!s_ws_connected || (*last_sent_at && now - *last_sent_at < 2000)) return false;
    cJSON *source = cJSON_ParseWithLength(event.json, event.length);
    if (!source) return false;
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
    bool sent = websocket_send_json("event", body);
    cJSON_Delete(body);
    cJSON_Delete(source);
    *last_sent_at = now;
    if (!sent) ++s_ws_send_failures;
    return true;
}

static const char *matrix_kind_name(uint8_t kind)
{
    return kind == 1 ? "thermal" : "pressure";
}

static int matrix_kind_index(const char *kind)
{
    if (!kind) return -1;
    if (!strcmp(kind, "thermal")) return 0;
    if (!strcmp(kind, "pressure")) return 1;
    return -1;
}

static bool prepare_matrix_frame(matrix_delivery_t *delivery)
{
    if (delivery->prepared) return true;
    uint32_t now = now_ms();
    if (delivery->last_submission_ms &&
        now - delivery->last_submission_ms < delivery->interval_ms) {
        return false;
    }

    matrix_state_t *matrix = delivery->source;
    size_t count = (size_t)matrix->width * matrix->height;
    size_t frame_length = 14U + count * sizeof(int16_t);
    if (!delivery->frame || frame_length > MATRIX_TX_BUFFER_SIZE) return false;

    bool pending = false;
    uint32_t source_sequence = 0;
    if (xSemaphoreTake(s_matrix_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        pending = matrix->pending;
        source_sequence = matrix->sequence;
        if (pending) {
            memcpy(delivery->frame + 14, matrix->samples, count * sizeof(int16_t));
        }
        xSemaphoreGive(s_matrix_mutex);
    }
    if (!pending) return false;

    uint32_t sequence = delivery->upload_sequence;
    uint8_t *frame = delivery->frame;
    memcpy(frame, "RA8M", 4);
    frame[4] = 1;
    frame[5] = delivery->kind;
    /* Protocol dimensions are rows x columns.  The thermal sensor's product
     * notation is width x height (80x64), hence rows=64 and cols=80. */
    frame[6] = (uint8_t)(matrix->height >> 8);
    frame[7] = (uint8_t)matrix->height;
    frame[8] = (uint8_t)(matrix->width >> 8);
    frame[9] = (uint8_t)matrix->width;
    frame[10] = (uint8_t)(sequence >> 24);
    frame[11] = (uint8_t)(sequence >> 16);
    frame[12] = (uint8_t)(sequence >> 8);
    frame[13] = (uint8_t)sequence;
    delivery->source_sequence = source_sequence;
    delivery->frame_length = frame_length;
    delivery->sent_at_ms = 0;
    delivery->prepared = true;
    delivery->inflight = false;
    ++delivery->source_frames;
    return true;
}

static void consume_matrix_ack(matrix_delivery_t *delivery)
{
    int index = delivery->kind == 1 ? 0 : 1;
    bool valid;
    uint32_t sequence;
    portENTER_CRITICAL(&s_matrix_ack_mux);
    valid = s_matrix_ack_valid[index];
    sequence = s_matrix_acked_sequence[index];
    s_matrix_ack_valid[index] = false;
    portEXIT_CRITICAL(&s_matrix_ack_mux);

    if (!valid || !delivery->prepared || sequence != delivery->upload_sequence) {
        return;
    }

    uint32_t ack_rtt = delivery->sent_at_ms ? now_ms() - delivery->sent_at_ms : 0;
    delivery->last_ack_rtt_ms = ack_rtt;
    if (ack_rtt > delivery->max_ack_rtt_ms) {
        delivery->max_ack_rtt_ms = ack_rtt;
    }
    ++delivery->sent_frames;
    if (xSemaphoreTake(s_matrix_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (delivery->source->sequence == delivery->source_sequence) {
            delivery->source->pending = false;
        }
        xSemaphoreGive(s_matrix_mutex);
    }
    ++delivery->upload_sequence;
    delivery->prepared = false;
    delivery->inflight = false;
    delivery->sent_at_ms = 0;
}

static bool send_prepared_matrix(matrix_delivery_t *delivery)
{
    consume_matrix_ack(delivery);
    uint32_t now = now_ms();
    if (!s_matrix_ws_connected || !s_matrix_ws ||
        !esp_websocket_client_is_connected(s_matrix_ws)) {
        if (delivery->inflight) {
            delivery->inflight = false;
            ++delivery->retries;
        }
        return false;
    }

    if (delivery->inflight) {
        if (now - delivery->sent_at_ms < MATRIX_ACK_TIMEOUT_MS) {
            return false;
        }
        delivery->inflight = false;
        ++delivery->ack_timeouts;
        ++delivery->retries;
        ESP_LOGW(TAG,
                 "%s matrix ACK timeout upload_sequence=%lu; retrying identical frame",
                 matrix_kind_name(delivery->kind),
                 (unsigned long)delivery->upload_sequence);
    }

    if (!prepare_matrix_frame(delivery)) return false;
    now = now_ms();
    if (delivery->last_attempt_ms &&
        now - delivery->last_attempt_ms < MATRIX_SEND_RETRY_MS) {
        return false;
    }

    bool first_submission = delivery->sent_at_ms == 0;
    uint32_t started = now_ms();
    int sent = esp_websocket_client_send_bin(
        s_matrix_ws,
        (const char *)delivery->frame,
        (int)delivery->frame_length,
        pdMS_TO_TICKS(MATRIX_WSS_SEND_TIMEOUT_MS));
    uint32_t elapsed = now_ms() - started;
    delivery->last_attempt_ms = now_ms();
    s_last_matrix_send_ms = elapsed;
    if (elapsed > s_max_matrix_send_ms) s_max_matrix_send_ms = elapsed;
    if (sent == (int)delivery->frame_length) {
        delivery->inflight = true;
        delivery->sent_at_ms = now_ms();
        if (first_submission) delivery->last_submission_ms = delivery->sent_at_ms;
        return true;
    }

    ++delivery->send_failures;
    ++s_matrix_ws_send_failures;
    ESP_LOGW(TAG,
             "%s matrix write failed upload_sequence=%lu sent=%d/%u elapsed=%lums",
             matrix_kind_name(delivery->kind),
             (unsigned long)delivery->upload_sequence,
             sent,
             (unsigned)delivery->frame_length,
             (unsigned long)elapsed);
    return true;
}

static void parse_matrix_websocket_text(const char *text, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(text, length);
    if (!root) return;
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "matrix_ack") &&
        cJSON_IsObject(payload)) {
        cJSON *matrix_kind = cJSON_GetObjectItemCaseSensitive(payload, "kind");
        cJSON *sequence = cJSON_GetObjectItemCaseSensitive(payload, "sequence");
        int index = cJSON_IsString(matrix_kind)
                        ? matrix_kind_index(matrix_kind->valuestring)
                        : -1;
        if (index >= 0 && cJSON_IsNumber(sequence)) {
            portENTER_CRITICAL(&s_matrix_ack_mux);
            s_matrix_acked_sequence[index] = (uint32_t)sequence->valuedouble;
            s_matrix_ack_valid[index] = true;
            portEXIT_CRITICAL(&s_matrix_ack_mux);
        }
    } else if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "error")) {
        char *error = cJSON_PrintUnformatted(payload);
        ESP_LOGE(TAG, "matrix server error: %s", error ? error : "unknown");
        free(error);
    }
    cJSON_Delete(root);
}

static void matrix_websocket_event_handler(void *handler_args,
                                           esp_event_base_t base,
                                           int32_t event_id,
                                           void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_matrix_ws_connected = true;
        ESP_LOGI(TAG, "matrix WSS connected");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED ||
               event_id == WEBSOCKET_EVENT_CLOSED) {
        s_matrix_ws_connected = false;
        ESP_LOGW(TAG, "matrix WSS disconnected event=%ld", (long)event_id);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        s_matrix_ws_connected = false;
        ESP_LOGE(TAG,
                 "matrix WSS error type=%d esp=%s tls=%d verify=0x%x socket_errno=%d",
                 (int)data->error_handle.error_type,
                 esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
                 data->error_handle.esp_tls_stack_err,
                 data->error_handle.esp_tls_cert_verify_flags,
                 data->error_handle.esp_transport_sock_errno);
    } else if (event_id == WEBSOCKET_EVENT_DATA && data && data->op_code == 0x1) {
        if (data->payload_offset == 0) {
            s_matrix_text_length = 0;
            s_matrix_text_expected = data->payload_len;
        }
        if (s_matrix_text &&
            s_matrix_text_expected < MATRIX_TEXT_BUFFER_SIZE &&
            data->payload_offset + data->data_len < MATRIX_TEXT_BUFFER_SIZE) {
            memcpy(s_matrix_text + data->payload_offset, data->data_ptr, data->data_len);
            s_matrix_text_length = data->payload_offset + data->data_len;
            if (s_matrix_text_length == s_matrix_text_expected) {
                s_matrix_text[s_matrix_text_length] = '\0';
                parse_matrix_websocket_text(s_matrix_text, s_matrix_text_length);
            }
        }
    }
}

static void matrix_network_task(void *arg)
{
    (void)arg;
    bool prefer_thermal = false;
    uint32_t last_log = 0;
    for (;;) {
        consume_matrix_ack(&s_pressure_delivery);
        consume_matrix_ack(&s_thermal_delivery);

        bool attempted;
        if (prefer_thermal) {
            attempted = send_prepared_matrix(&s_thermal_delivery);
            if (!attempted) attempted = send_prepared_matrix(&s_pressure_delivery);
        } else {
            attempted = send_prepared_matrix(&s_pressure_delivery);
            if (!attempted) attempted = send_prepared_matrix(&s_thermal_delivery);
        }
        if (attempted) prefer_thermal = !prefer_thermal;

        uint32_t now = now_ms();
        if (!last_log || now - last_log >= 5000) {
            ESP_LOGI(TAG,
                     "matrix health wss=%s pressure=%lu/%lu timeout=%lu rtt=%lu/%lums "
                     "thermal=%lu/%lu timeout=%lu rtt=%lu/%lums failures=%lu",
                     s_matrix_ws_connected ? "up" : "retrying",
                     (unsigned long)s_pressure_delivery.sent_frames,
                     (unsigned long)s_pressure_delivery.source_frames,
                     (unsigned long)s_pressure_delivery.ack_timeouts,
                     (unsigned long)s_pressure_delivery.last_ack_rtt_ms,
                     (unsigned long)s_pressure_delivery.max_ack_rtt_ms,
                     (unsigned long)s_thermal_delivery.sent_frames,
                     (unsigned long)s_thermal_delivery.source_frames,
                     (unsigned long)s_thermal_delivery.ack_timeouts,
                     (unsigned long)s_thermal_delivery.last_ack_rtt_ms,
                     (unsigned long)s_thermal_delivery.max_ack_rtt_ms,
                     (unsigned long)s_matrix_ws_send_failures);
            last_log = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
        ++s_ws_send_failures;
    }
    cJSON_Delete(body);
}

static command_result_t *find_command_result(uint32_t id)
{
    for (size_t i = 0; i < COMMAND_RESULT_CACHE_LENGTH; ++i) {
        if (s_command_results[i].valid && s_command_results[i].id == id) {
            return &s_command_results[i];
        }
    }
    return NULL;
}

static void remember_command_result(uint32_t id, bool ok, const char *message)
{
    command_result_t *result = &s_command_results[s_command_result_cursor];
    s_command_result_cursor = (s_command_result_cursor + 1U) % COMMAND_RESULT_CACHE_LENGTH;
    result->id = id;
    result->valid = true;
    result->ok = ok;
    strlcpy(result->message, message, sizeof(result->message));
}

static void process_server_command(const server_command_t *command)
{
    command_result_t *cached = find_command_result(command->id);
    if (cached) {
        send_command_ack(cached->id, cached->ok, cached->message);
        return;
    }
    if (command->id == s_last_command_id) {
        static const char duplicate[] = "duplicate already handled";
        remember_command_result(command->id, true, duplicate);
        send_command_ack(command->id, true, duplicate);
        return;
    }
    if (!strcmp(command->command, "media.preview.start") ||
        !strcmp(command->command, "media.preview.stop")) {
        s_last_command_id = command->id;
        save_last_command_id(command->id);
        static const char handled[] = "S3 external microphone is controlled by WSS subscribers";
        remember_command_result(command->id, true, handled);
        send_command_ack(command->id, true, handled);
        return;
    }
    char *payload = command_payload_for_ra8(command);
    if (!payload) {
        static const char unsupported[] = "unsupported command";
        remember_command_result(command->id, false, unsupported);
        send_command_ack(command->id, false, unsupported);
        return;
    }
    size_t length = strlen(payload);
    esp_err_t result = length <= RA8_MAX_PAYLOAD ?
        send_ra8_frame(RA8_TYPE_CMD, (const uint8_t *)payload, (uint16_t)length) : ESP_ERR_INVALID_SIZE;
    free(payload);
    if (result == ESP_OK) {
        s_last_command_id = command->id;
        save_last_command_id(command->id);
        static const char forwarded[] = "forwarded to RA8";
        remember_command_result(command->id, true, forwarded);
        send_command_ack(command->id, true, forwarded);
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
        if (cJSON_IsNumber(id)) {
            s_server_acked_event = (uint32_t)id->valuedouble;
            s_server_acked_event_valid = true;
        }
    } else if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "telemetry_ack") &&
               cJSON_IsObject(payload)) {
        cJSON *sequence = cJSON_GetObjectItemCaseSensitive(payload, "sequence");
        if (cJSON_IsNumber(sequence)) {
            portENTER_CRITICAL(&s_telemetry_ack_mux);
            s_server_acked_telemetry_sequence = (uint32_t)sequence->valuedouble;
            s_server_acked_telemetry_valid = true;
            portEXIT_CRITICAL(&s_telemetry_ack_mux);
        }
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
               (!strcmp(kind->valuestring, "hello_ack") ||
                 !strcmp(kind->valuestring, "matrix_ack") || !strcmp(kind->valuestring, "heartbeat_ack") ||
                 !strcmp(kind->valuestring, "command_ack_received"))) {
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
        s_ws_restart_needed = false;
        ++s_ws_session_generation;
        ESP_LOGI(TAG, "WSS connected session=%lu", (unsigned long)s_ws_session_generation);
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        s_ws_connected = false;
        ESP_LOGW(TAG, "WSS disconnected event=%ld", (long)event_id);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        s_ws_connected = false;
        ESP_LOGE(TAG,
                 "WSS transport error type=%d esp=%s tls=%d verify=0x%x "
                 "socket_errno=%d",
                 (int)data->error_handle.error_type,
                 esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
                 data->error_handle.esp_tls_stack_err,
                 data->error_handle.esp_tls_cert_verify_flags,
                 data->error_handle.esp_transport_sock_errno);
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

static bool send_heartbeat(void)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) return false;
    cJSON_AddNumberToObject(body, "sequence", s_heartbeat_sequence++);
    cJSON_AddNumberToObject(body, "device_time_ms", now_ms());
    bool sent = websocket_send_json("heartbeat", body);
    cJSON_Delete(body);
    if (!sent) ++s_ws_send_failures;
    return sent;
}

static void network_task(void *arg)
{
    (void)arg;
    uint32_t last_event_send = 0;
    uint32_t last_telemetry_attempt = 0;
    uint32_t last_heartbeat = 0;
    uint32_t last_log = 0;
    uint32_t last_hello_attempt = 0;
    uint32_t last_restart_attempt = 0;
    uint32_t hello_session = 0;
    uint32_t prepared_session = 0;
    for (;;) {
        uint32_t now = now_ms();
        bool wifi_connected = xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT;
        if (wifi_connected && !s_ws_connected && s_ws_restart_needed &&
            (!last_restart_attempt || now - last_restart_attempt >= 2000)) {
            esp_err_t restart_result = esp_websocket_client_start(s_ws);
            last_restart_attempt = now;
            if (restart_result == ESP_OK) {
                s_ws_restart_needed = false;
                ESP_LOGI(TAG, "gateway WSS restart scheduled");
            } else {
                ESP_LOGE(TAG, "gateway WSS restart failed: %s",
                         esp_err_to_name(restart_result));
            }
        }
        if (wifi_connected && s_ws_connected) {
            uint32_t session = s_ws_session_generation;
            if (hello_session != session) {
                if (prepared_session != session) {
                    reset_telemetry_delivery_state();
                    s_consecutive_telemetry_ack_timeouts = 0;
                    prepared_session = session;
                }
                if ((!last_hello_attempt || now - last_hello_attempt >= 500) &&
                    send_hello()) {
                    hello_session = session;
                    last_hello_attempt = 0;
                    last_telemetry_attempt = 0;
                    last_heartbeat = now;
                } else if (!last_hello_attempt || now - last_hello_attempt >= 500) {
                    ++s_ws_send_failures;
                    last_hello_attempt = now;
                }
            } else {
                consume_telemetry_ack();
                if (telemetry_ack_timed_out()) {
                    ++s_telemetry_ack_timeouts;
                    ++s_consecutive_telemetry_ack_timeouts;
                    reset_telemetry_delivery_state();
                    last_telemetry_attempt = 0;
                    if (s_consecutive_telemetry_ack_timeouts <
                        TELEMETRY_ACK_RESTART_THRESHOLD) {
                        ESP_LOGW(TAG,
                                 "telemetry ACK timeout; retrying latest on current WSS "
                                 "attempt=%u/%u",
                                 (unsigned)s_consecutive_telemetry_ack_timeouts,
                                 (unsigned)TELEMETRY_ACK_RESTART_THRESHOLD);
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }
                    ESP_LOGW(TAG,
                             "telemetry ACK timeout %u consecutive times; restarting "
                             "gateway WSS",
                             (unsigned)s_consecutive_telemetry_ack_timeouts);
                    ++s_ws_send_failures;
                    s_consecutive_telemetry_ack_timeouts = 0;
                    s_ws_connected = false;
                    esp_err_t stop_result = esp_websocket_client_stop(s_ws);
                    if (stop_result != ESP_OK) {
                        ESP_LOGE(TAG, "gateway WSS stop failed: %s",
                                 esp_err_to_name(stop_result));
                    }
                    esp_err_t start_result = esp_websocket_client_start(s_ws);
                    s_ws_restart_needed = start_result != ESP_OK;
                    if (start_result != ESP_OK) {
                        ESP_LOGE(TAG, "gateway WSS start failed: %s",
                                 esp_err_to_name(start_result));
                    }
                    hello_session = 0;
                    prepared_session = 0;
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }

                /* Matrix traffic has its own WSS and can no longer block status. */
                send_latest_telemetry(&last_telemetry_attempt);

                bool auxiliary_work = false;
                server_command_t command;
                if (xQueueReceive(s_command_queue, &command, 0) == pdTRUE) {
                    process_server_command(&command);
                    auxiliary_work = true;
                }
                if (!auxiliary_work) {
                    auxiliary_work = send_pending_event(&last_event_send);
                }

                bool telemetry_producer_idle =
                    !s_last_telemetry_produced_ms ||
                    now - s_last_telemetry_produced_ms >= HEARTBEAT_INTERVAL_MS;
                if (!auxiliary_work &&
                    telemetry_producer_idle &&
                    (!last_heartbeat || now - last_heartbeat >= HEARTBEAT_INTERVAL_MS)) {
                    send_heartbeat();
                    last_heartbeat = now;
                }
            }
        }
        if (!last_log || now - last_log >= 5000) {
            ESP_LOGI(TAG, "health wifi=%s wss=%s matrix_wss=%s ra8=%s frames=%lu crc=%lu events=%u telemetry=%lu/%lu bytes=%u overwrite=%lu ack_timeout=%lu/%u ack_rtt=%lu/%lums ws_fail=%lu send=%lums matrix=%lu/%lums heap=%lu internal=%lu psram=%lu",
                     wifi_connected ? "up" : "down",
                     s_ws_connected ? "up" : "retrying",
                     s_matrix_ws_connected ? "up" : "retrying",
                     s_stats.last_status_ms && now - s_stats.last_status_ms <= RA8_OFFLINE_MS ? "online" : "waiting",
                     (unsigned long)s_stats.valid_frames, (unsigned long)s_stats.crc_errors,
                     (unsigned)uxQueueMessagesWaiting(s_event_queue),
                     (unsigned long)s_telemetry_sent, (unsigned long)s_telemetry_produced,
                     (unsigned)s_telemetry_latest_length,
                     (unsigned long)s_telemetry_overwritten,
                     (unsigned long)s_telemetry_ack_timeouts,
                     (unsigned)s_consecutive_telemetry_ack_timeouts,
                     (unsigned long)s_last_telemetry_ack_rtt_ms,
                     (unsigned long)s_max_telemetry_ack_rtt_ms,
                     (unsigned long)s_ws_send_failures, (unsigned long)s_last_ws_send_ms,
                     (unsigned long)s_last_matrix_send_ms,
                     (unsigned long)s_max_matrix_send_ms,
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            last_log = now;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ws_connected = false;
        s_matrix_ws_connected = false;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&got_ip->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void init_wifi(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
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
    uart_config_t config = {
        .baud_rate = RA8_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(RA8_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(RA8_UART, RA8_UART_TX_PIN, RA8_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(RA8_UART, 16384, 0, 0, NULL, 0));
    ESP_LOGI(TAG, "RA8 UART ready: RX=GPIO%d TX=GPIO%d baud=%d 8N1", RA8_UART_RX_PIN,
             RA8_UART_TX_PIN, RA8_UART_BAUD);
}

static void init_websocket(void)
{
    char headers[256];
    snprintf(headers, sizeof(headers), "X-Device-ID: %s\r\nX-Device-Key: %s\r\n",
             GATEWAY_DEVICE_ID, GATEWAY_DEVICE_KEY);
    esp_websocket_client_config_t config = {
        .uri = WSS_URI,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 1000,
        .task_prio = 8,
        .keep_alive_enable = true,
        .keep_alive_idle = 15,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        /*
         * Status traffic is present every 500 ms, so frequent protocol PINGs
         * only compete for the TX lock. Transport writes and ACK timeouts
         * already detect a dead connection.
         */
        .ping_interval_sec = WSS_PING_INTERVAL_SEC,
        .disable_pingpong_discon = true,
        .buffer_size = TELEMETRY_BUFFER_SIZE,
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
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 1000,
        .task_prio = 7,
        .keep_alive_enable = true,
        .keep_alive_idle = 15,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .ping_interval_sec = WSS_PING_INTERVAL_SEC,
        .disable_pingpong_discon = true,
        .buffer_size = 1024,
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

void app_main(void)
{
    ESP_LOGI(TAG, "booting %s on ESP32-S3-WROOM-CAM-N16R8", FIRMWARE_VERSION);
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }
    nvs_handle_t nvs;
    if (nvs_open("gateway", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "last_cmd", &s_last_command_id);
        nvs_close(nvs);
    }
    s_status_mutex = xSemaphoreCreateMutex();
    s_uart_tx_mutex = xSemaphoreCreateMutex();
    s_matrix_mutex = xSemaphoreCreateMutex();
    s_telemetry_mutex = xSemaphoreCreateMutex();
    s_event_queue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(safety_event_t));
    s_command_queue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(server_command_t));
    s_thermal.width = 80; s_thermal.height = 64;
    s_pressure.width = 16; s_pressure.height = 16;
    s_thermal_delivery = (matrix_delivery_t){
        .source = &s_thermal,
        .kind = 1,
        .interval_ms = THERMAL_INTERVAL_MS,
        .upload_sequence = 1,
    };
    s_pressure_delivery = (matrix_delivery_t){
        .source = &s_pressure,
        .kind = 2,
        .interval_ms = PRESSURE_INTERVAL_MS,
        .upload_sequence = 1,
    };
    s_thermal.samples = heap_caps_calloc(80U * 64U, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_thermal.seen = heap_caps_calloc((80U * 64U + 7U) / 8U, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pressure.samples = heap_caps_calloc(16U * 16U, sizeof(int16_t), MALLOC_CAP_8BIT);
    s_pressure.seen = heap_caps_calloc((16U * 16U + 7U) / 8U, 1, MALLOC_CAP_8BIT);
    s_ws_text_capacity = 4096;
    s_ws_text = heap_caps_malloc(
        s_ws_text_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_latest_status = heap_caps_malloc(
        RA8_MAX_PAYLOAD + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_status_snapshot = heap_caps_malloc(
        RA8_MAX_PAYLOAD + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_telemetry_latest = heap_caps_malloc(
        TELEMETRY_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_telemetry_tx = heap_caps_malloc(
        TELEMETRY_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_matrix_text = heap_caps_malloc(MATRIX_TEXT_BUFFER_SIZE, MALLOC_CAP_8BIT);
    s_thermal_delivery.frame = heap_caps_malloc(
        MATRIX_TX_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pressure_delivery.frame = heap_caps_malloc(
        14U + 16U * 16U * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_status_mutex || !s_uart_tx_mutex || !s_matrix_mutex || !s_telemetry_mutex ||
        !s_event_queue || !s_command_queue || !s_thermal.samples || !s_thermal.seen ||
        !s_pressure.samples || !s_pressure.seen || !s_ws_text || !s_latest_status ||
        !s_status_snapshot || !s_telemetry_latest || !s_telemetry_tx || !s_matrix_text ||
        !s_thermal_delivery.frame || !s_pressure_delivery.frame) {
        ESP_LOGE(TAG, "gateway allocation failed");
        abort();
    }
    strcpy(s_latest_status, "{}");
    strcpy(s_status_snapshot, "{}");
    init_uart();
    if (xTaskCreatePinnedToCore(uart_rx_task, "ra8_uart", 8192, NULL, 8, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "UART task creation failed");
        abort();
    }
    init_wifi();
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    if (xTaskCreatePinnedToCore(
            telemetry_producer_task, "telemetry_latest", 8192, NULL, 6, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "telemetry producer task creation failed");
        abort();
    }
    if (xTaskCreatePinnedToCore(network_task, "gateway_net", 8192, NULL, 7, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "network task creation failed");
        abort();
    }
    init_websocket();
    ESP_LOGI(TAG, "waiting for priority gateway WSS before starting media");
    while (!s_ws_connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (xTaskCreatePinnedToCore(
            matrix_network_task, "matrix_net", 6144, NULL, 6, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "matrix network task creation failed");
        abort();
    }
    init_matrix_websocket();
    ESP_LOGI(TAG, "matrix WSS started independently from telemetry");
    uint32_t matrix_wait_started = now_ms();
    while (!s_matrix_ws_connected && now_ms() - matrix_wait_started < 5000) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_matrix_ws_connected) {
        ESP_LOGW(TAG, "matrix WSS still reconnecting; continuing with independent media");
    }
    ESP_ERROR_CHECK(mic_stream_start());
    /*
     * Do not make the audio and video TLS handshakes compete for the last
     * block of internal SRAM. Audio remains optional: video still starts
     * after this bounded wait if its peer is unavailable.
     */
    uint32_t audio_wait_started = now_ms();
    while (!mic_stream_is_connected() && now_ms() - audio_wait_started < 8000U) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!mic_stream_is_connected()) {
        ESP_LOGW(TAG, "audio WSS still reconnecting; starting independent video");
    }
    ESP_ERROR_CHECK(camera_stream_start());
}

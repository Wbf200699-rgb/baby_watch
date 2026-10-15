#include "thread_code/ra8_esp32_protocol.h"

#include <stdbool.h>
#include <string.h>

#include "hal_data.h"
#include "semphr.h"
#include "thread_code/audio_cry_inference.h"
#include "thread_code/audio_ipc_shared.h"
#include "thread_code/esp32_uart_driver.h"
#include "thread_code/mic_ipc_shared.h"
#include "thread_code/multimodal_fusion.h"
#include "thread_code/pressure_ipc_shared.h"
#include "thread_code/scd41_ipc_shared.h"
#include "thread_code/sd_ipc_shared.h"
#include "thread_code/serial_camera_driver.h"
#include "thread_code/serial_camera_ipc_shared.h"
#include "thread_code/soother_pat_control.h"
#include "thread_code/thermal_ipc_shared.h"

#define RA8_ESP32_RX_POLL_BUDGET_BYTES (2048U)
#include "thread_code/tmp117_ipc_shared.h"
#include "thread_code/vision_runtime.h"
#include "thread_code/warm_light_control.h"

#define RA8_CPU0_FIRMWARE_BUILD             (0x20260812U)
#define RA8_ESP32_GATEWAY_READY_TIMEOUT_MS  (5000U)
#define RA8_SILENT_RESET_ACK_TIMEOUT_MS      (100U)
#define RA8_SILENT_RESET_DELAY_MS            (250U)
#define RA8_STORY_FILE_PROTOCOL_VERSION       (1U)
#define RA8_STORY_FILE_FLAG_PLAY_AFTER        (1U << 0)
#define RA8_STORY_FILE_BEGIN_FIXED_BYTES      (17U)
#define RA8_STORY_FILE_CHUNK_FIXED_BYTES      (20U)
#define RA8_STORY_FILE_END_BYTES              (16U)
#define RA8_STORY_FILE_RESULT_BYTES           (16U)
#define RA8_RESOURCE_PAYLOAD_VERSION           (1U)
#define RA8_RESOURCE_PAYLOAD_BYTES             (56U)
#define RA8_STORY_RESULT_TIMEOUT_MS           (100U)
#define RA8_STORY_ACTIVITY_TIMEOUT_MS         (30000U)
#define RA8_ESP32_FRAME_MAX_LENGTH \
    (RA8_ESP32_PROTOCOL_HEADER_LENGTH + RA8_ESP32_PROTOCOL_MAX_PAYLOAD + RA8_ESP32_PROTOCOL_CRC_LENGTH)

extern TaskHandle_t camera_thread;
extern TaskHandle_t vision_preprocess_thread;
extern TaskHandle_t vision_infer_thread;
extern TaskHandle_t audio_infer_thread;
extern TaskHandle_t fusion_thread;
extern TaskHandle_t esp32_uart_thread;

static uint16_t g_ra8_esp32_sequence;
static StaticSemaphore_t g_protocol_tx_lock_storage;
static SemaphoreHandle_t g_protocol_tx_lock;
static uint8_t g_protocol_rx_buffer[RA8_ESP32_FRAME_MAX_LENGTH];
static uint32_t g_protocol_rx_length;
static uint8_t g_protocol_tx_frame[RA8_ESP32_FRAME_MAX_LENGTH];
static char g_protocol_status_payload[RA8_ESP32_PROTOCOL_MAX_PAYLOAD];

/* Leave room for counters to gain digits during a long-running deployment.
 * The hard wire limit remains 4096 bytes; this soft limit only decides when
 * to rebuild the status with the next-smaller detail profile. */
#define PROTOCOL_STATUS_PAYLOAD_HEADROOM_BYTES (256U)
#define PROTOCOL_STATUS_PAYLOAD_SOFT_LIMIT \
    (RA8_ESP32_PROTOCOL_MAX_PAYLOAD - PROTOCOL_STATUS_PAYLOAD_HEADROOM_BYTES)
static uint8_t g_protocol_thermal_payload[RA8_ESP32_PROTOCOL_MAX_PAYLOAD];
static uint8_t g_protocol_pressure_payload[RA8_ESP32_PROTOCOL_MAX_PAYLOAD];
static char g_protocol_cmd_result_payload[160U];
static int16_t g_protocol_thermal_snapshot[THERMAL_IPC_POINT_COUNT];
static int16_t g_protocol_pressure_snapshot[PRESSURE_IPC_MATRIX_COUNT];
static scd41_ipc_snapshot_t g_protocol_scd41_snapshot;
static uint32_t g_protocol_bme_source_last_good_tick;
static uint32_t g_protocol_bme_last_good_tick;
static volatile bool g_protocol_gateway_ready;
static volatile uint32_t g_protocol_gateway_ready_count;
static volatile TickType_t g_protocol_gateway_ready_tick;
static volatile uint32_t g_protocol_status_attempt_count;
static volatile uint32_t g_protocol_status_success_count;
static volatile uint32_t g_protocol_status_payload_too_long_count;
static volatile uint32_t g_protocol_status_uart_error_count;
static volatile uint32_t g_protocol_status_payload_length;
static volatile uint32_t g_protocol_status_detail;
static uint32_t g_resource_sequence;
static uint32_t g_resource_previous_counter;
static uint32_t g_resource_previous_idle;
static uint32_t g_resource_previous_tick;
static uint32_t g_resource_previous_npu_busy_ms;
static uint32_t g_resource_previous_inference_count;
static bool g_resource_sample_started;
volatile uint32_t g_protocol_rx_valid_frame_count;
volatile uint32_t g_protocol_rx_crc_error_count;
volatile uint32_t g_protocol_rx_length_error_count;
volatile uint32_t g_protocol_rx_budget_hit_count;
volatile uint32_t g_protocol_rx_last_valid_type;
volatile uint32_t g_protocol_rx_last_valid_payload_length;
static bool g_protocol_story_result_pending;
static uint32_t g_protocol_story_pending_operation_sequence;
static uint16_t g_protocol_story_pending_request_sequence;
static uint8_t g_protocol_story_pending_operation;
static uint32_t g_protocol_story_pending_transfer_id;

typedef enum e_safety_state
{
    SAFETY_STATE_NORMAL = 0,
    SAFETY_STATE_NEAR_EDGE = 1,
    SAFETY_STATE_OUT_OF_BED = 2,
    SAFETY_STATE_OVERHEAT = 3,
    SAFETY_STATE_UNDERHEAT = 4,
    SAFETY_STATE_SENSOR_FAULT = 5,
    SAFETY_STATE_CRYING = 6,
    SAFETY_STATE_FACE_COVERED = 7,
    SAFETY_STATE_STANDING = 8,
    SAFETY_STATE_MOVING = 9,
    SAFETY_STATE_SLEEPING = 10,
    SAFETY_STATE_AWAKE = 11,
    SAFETY_STATE_TEST_ALARM = 255
} safety_state_t;

typedef struct st_safety_runtime
{
    uint32_t sequence;
    uint32_t state_since_tick;
    uint32_t event_id;
    uint32_t pending_event_id;
    uint32_t pending_since_tick;
    uint32_t pending_state;
    uint32_t pending_source_mask;
    uint32_t pending_severity;
    uint32_t pending_confidence;
    uint32_t pending_incident_id;
    uint32_t pending_incident_type;
    uint32_t pending_action;
    uint32_t pending_lifecycle;
    uint32_t pending_level;
    uint32_t global_alarm_level;
    bool pending_from_fusion;
    uint32_t last_send_tick;
    uint32_t ack_count;
    uint32_t retry_count;
    uint32_t source_mask;
    uint32_t severity;
    uint32_t confidence;
    uint32_t sensor_fault_mask;
    uint32_t pressure_total;
    uint32_t pressure_edge;
    uint32_t pressure_x100;
    uint32_t pressure_y100;
    safety_state_t state;
} safety_runtime_t;

typedef enum e_status_detail
{
    STATUS_DETAIL_FULL = 0,
    STATUS_DETAIL_COMPACT = 1,
    STATUS_DETAIL_ESSENTIAL = 2,
    STATUS_DETAIL_EMERGENCY = 3
} status_detail_t;

static safety_runtime_t g_safety;
static bool g_safety_force_test;
static void safety_evaluate(void);

#define SAFETY_SOURCE_SYSTEM       (1U << 7)
#define SAFETY_EVENT_RETRY_TICKS   (pdMS_TO_TICKS(2000U))

static uint32_t protocol_append_char(char * p_buffer, uint32_t offset, uint32_t size, char value)
{
    if (offset < size)
    {
        p_buffer[offset] = value;
    }

    return offset + 1U;
}

static uint32_t protocol_append_text(char * p_buffer, uint32_t offset, uint32_t size, char const * p_text)
{
    while ('\0' != *p_text)
    {
        offset = protocol_append_char(p_buffer, offset, size, *p_text);
        p_text++;
    }

    return offset;
}

static uint32_t protocol_append_json_string(char * p_buffer, uint32_t offset, uint32_t size, char const * p_text)
{
    offset = protocol_append_char(p_buffer, offset, size, '"');

    while ('\0' != *p_text)
    {
        if (('"' == *p_text) || ('\\' == *p_text))
        {
            offset = protocol_append_char(p_buffer, offset, size, '\\');
            offset = protocol_append_char(p_buffer, offset, size, *p_text);
        }
        else if ((uint8_t) *p_text >= 0x20U)
        {
            offset = protocol_append_char(p_buffer, offset, size, *p_text);
        }

        p_text++;
    }

    return protocol_append_char(p_buffer, offset, size, '"');
}

static uint32_t protocol_stack_high_water(TaskHandle_t task)
{
    return (NULL != task) ? (uint32_t) uxTaskGetStackHighWaterMark(task) : 0U;
}

static uint32_t protocol_append_uint(char * p_buffer, uint32_t offset, uint32_t size, uint32_t value)
{
    char digits[10];
    uint32_t digit_count = 0U;

    if (0U == value)
    {
        return protocol_append_char(p_buffer, offset, size, '0');
    }

    while (value > 0U)
    {
        digits[digit_count++] = (char) ('0' + (value % 10U));
        value /= 10U;
    }

    while (digit_count > 0U)
    {
        offset = protocol_append_char(p_buffer, offset, size, digits[--digit_count]);
    }

    return offset;
}

static uint32_t protocol_append_int(char * p_buffer, uint32_t offset, uint32_t size, int32_t value)
{
    uint32_t magnitude = (uint32_t) value;

    if (value < 0)
    {
        offset = protocol_append_char(p_buffer, offset, size, '-');
        magnitude = 0U - magnitude;
    }

    return protocol_append_uint(p_buffer, offset, size, magnitude);
}

static uint32_t protocol_append_hex_bytes(char * p_buffer,
                                          uint32_t offset,
                                          uint32_t size,
                                          volatile uint8_t const * p_data,
                                          uint32_t length)
{
    static char const hex[] = "0123456789ABCDEF";

    offset = protocol_append_char(p_buffer, offset, size, '"');
    for (uint32_t i = 0U; i < length; i++)
    {
        uint8_t value = p_data[i];
        offset = protocol_append_char(p_buffer, offset, size, hex[value >> 4U]);
        offset = protocol_append_char(p_buffer, offset, size, hex[value & 0x0FU]);
    }

    return protocol_append_char(p_buffer, offset, size, '"');
}

static uint32_t protocol_append_json_fixed_string(char * p_buffer,
                                                  uint32_t offset,
                                                  uint32_t size,
                                                  volatile char const * p_text,
                                                  uint32_t max_length)
{
    offset = protocol_append_char(p_buffer, offset, size, '"');
    for (uint32_t i = 0U; (i < max_length) && ('\0' != p_text[i]); i++)
    {
        char value = p_text[i];
        if (('"' == value) || ('\\' == value))
        {
            offset = protocol_append_char(p_buffer, offset, size, '\\');
            offset = protocol_append_char(p_buffer, offset, size, value);
        }
        else if ((uint8_t) value >= 0x20U)
        {
            offset = protocol_append_char(p_buffer, offset, size, value);
        }
    }

    return protocol_append_char(p_buffer, offset, size, '"');
}

static uint32_t protocol_append_camera_response(
    char * p_buffer,
    uint32_t offset,
    uint32_t size,
    char const * p_name,
    volatile serial_camera_response_debug_t const * p_debug)
{
    offset = protocol_append_json_string(p_buffer, offset, size, p_name);
    offset = protocol_append_text(p_buffer, offset, size, ":{\"cmd\":");
    offset = protocol_append_uint(p_buffer, offset, size, p_debug->command_code);
    offset = protocol_append_text(p_buffer, offset, size, ",\"exp\":");
    offset = protocol_append_uint(p_buffer, offset, size, p_debug->expected_bytes);
    offset = protocol_append_text(p_buffer, offset, size, ",\"got\":");
    offset = protocol_append_uint(p_buffer, offset, size, p_debug->received_bytes);
    offset = protocol_append_text(p_buffer, offset, size, ",\"fsp\":");
    offset = protocol_append_uint(p_buffer, offset, size, p_debug->fsp_err);
    offset = protocol_append_text(p_buffer, offset, size, ",\"head\":");
    offset = protocol_append_hex_bytes(p_buffer,
                                       offset,
                                       size,
                                       p_debug->reply,
                                       SERIAL_CAMERA_REPLY_DEBUG_BYTES);
    return protocol_append_char(p_buffer, offset, size, '}');
}

static void protocol_put_be16(uint8_t * p_buffer, uint32_t offset, uint16_t value)
{
    p_buffer[offset] = (uint8_t) (value >> 8);
    p_buffer[offset + 1U] = (uint8_t) value;
}

uint16_t ra8_esp32_protocol_crc16_ccitt(const uint8_t * p_data, uint32_t length)
{
    uint16_t crc = 0xFFFFU;

    for (uint32_t i = 0U; i < length; i++)
    {
        crc ^= (uint16_t) ((uint16_t) p_data[i] << 8);
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            if (0U != (crc & 0x8000U))
            {
                crc = (uint16_t) (((uint32_t) crc << 1) ^ 0x1021U);
            }
            else
            {
                crc = (uint16_t) ((uint32_t) crc << 1);
            }
        }
    }

    return crc;
}

static uint32_t protocol_build_status_payload(char * p_payload,
                                              uint32_t payload_size,
                                              status_detail_t detail)
{
    volatile thermal_ipc_mailbox_t const * p_thm = THERMAL_IPC_MAILBOX;
    volatile tmp117_ipc_mailbox_t const * p_tmp = TMP117_IPC_MAILBOX;
    volatile pressure_ipc_mailbox_t const * p_prs = PRESSURE_IPC_MAILBOX;
    volatile audio_ipc_mailbox_t const * p_aud = AUDIO_IPC_MAILBOX;
    volatile mic_ipc_mailbox_t const * p_mic = MIC_IPC_MAILBOX;
    volatile sd_ipc_mailbox_t const * p_sd = SD_IPC_MAILBOX;
    volatile serial_camera_ipc_mailbox_t const * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    scd41_ipc_snapshot_t scd41_latest;
    scd41_ipc_snapshot_t const * p_scd = &g_protocol_scd41_snapshot;
    multimodal_fusion_snapshot_t fusion = {0};
    vision_runtime_snapshot_t vision = {0};
    esp32_uart_stats_t uart_stats = {0};
    warm_light_control_state_t warm_light = {0};
    soother_pat_control_state_t soother_pat = {0};
    uint32_t offset = 0U;
    uint32_t now = (uint32_t) xTaskGetTickCount();

    (void) multimodal_fusion_get_snapshot(&fusion);
    (void) vision_runtime_get_snapshot(&vision);
    esp32_uart_driver_get_stats(&uart_stats);
    warm_light_control_get_state(&warm_light);
    soother_pat_control_get_state(&soother_pat);
    if (scd41_ipc_read_snapshot(&scd41_latest))
    {
        g_protocol_scd41_snapshot = scd41_latest;
        if ((SCD41_IPC_MAGIC == scd41_latest.magic) &&
            (0U != scd41_latest.bme_last_good_tick) &&
            (scd41_latest.bme_last_good_tick != g_protocol_bme_source_last_good_tick))
        {
            /*
             * CPU1 has an independent tick epoch.  Translate each new BME688
             * sample into CPU0 time before exposing it beside top-level tick.
             */
            g_protocol_bme_source_last_good_tick = scd41_latest.bme_last_good_tick;
            g_protocol_bme_last_good_tick = now;
        }
    }

    offset = protocol_append_text(p_payload, offset, payload_size, "{\"tick\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, now);

    offset = protocol_append_text(p_payload, offset, payload_size, ",\"thm\":{\"seq\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"s\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->status);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rx\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->rx_bytes);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"min_cd\":");
    offset = protocol_append_int(p_payload, offset, payload_size, p_thm->min_cdeg);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"max_cd\":");
    offset = protocol_append_int(p_payload, offset, payload_size, p_thm->max_cdeg);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ctr_cd\":");
    offset = protocol_append_int(p_payload, offset, payload_size, p_thm->center_cdeg);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"avg_cd\":");
    offset = protocol_append_int(p_payload, offset, payload_size, p_thm->avg_cdeg);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"p01_cd\":");
    offset = protocol_append_int(p_payload, offset, payload_size, p_thm->p01_cdeg);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"p99_cd\":");
    offset = protocol_append_int(p_payload, offset, payload_size, p_thm->p99_cdeg);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"mean_cd\":");
    offset = protocol_append_int(p_payload, offset, payload_size, p_thm->mean_cdeg);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"frame_quality\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->frame_quality);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->uart_errors);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"uart_err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->uart_error_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ovf\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->rx_overflow_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"valid\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->valid_points);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"crc_rx\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->crc_received);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"crc_calc\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->crc_calculated);
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"head\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->head_word);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"raw0\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->raw0);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"raw1\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->raw1);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"rawc\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->raw_center);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"ta\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->sensor_raw);
    }
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        /* These cumulative transport and recovery counters are valuable for
         * diagnostics, but are not required to render the current thermal
         * state.  Omitting them from ESSENTIAL leaves enough headroom for
         * long-running counters without dropping into the emergency status
         * payload, which would hide every sensor from the gateway. */
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"pe\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->parity_error_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"fe\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->framing_error_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"fifo_ovf\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->fifo_overflow_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"crc_bad\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->crc_error_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"data_bad\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->data_error_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"baud_set\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->baud_set_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"w0\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->frame_zero_words);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"w1\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->frame_one_words);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wff\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->frame_ffff_words);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"build\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->driver_build);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"cf\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->consecutive_failures);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"rc\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->recovery_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"ro\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->reopen_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"hr\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->hard_reset_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"re\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->recovery_error_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"rr\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->last_recovery_reason);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"lg\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->last_good_tick);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"run\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->run_pin_ready);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"pc\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->power_cycle_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"relay\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->power_relay_ready);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"stale\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->stale_frame_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"same\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->identical_frame_streak);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"transient_drop_count\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->transient_drop_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"generation_retry_count\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->generation_retry_count);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"last_good_sequence\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->last_good_sequence);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"write_generation\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->write_generation);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"mean_abs_diff_cd\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->mean_abs_diff_cdeg);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"changed_pixels\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_thm->changed_pixel_count);
    }
    offset = protocol_append_char(p_payload, offset, payload_size, '}');

    offset = protocol_append_text(p_payload, offset, payload_size, ",\"tmp\":{\"seq\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_tmp->sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"s\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_tmp->status);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ok\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_tmp->connected_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_tmp->error_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fs\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_tmp->fail_step);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ch\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_tmp->fail_channel);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fsp\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_tmp->last_fsp_err);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"pca\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, p_tmp->pca_address);

    offset = protocol_append_text(p_payload, offset, payload_size, ",\"t_mc\":[");
    for (uint32_t i = 0U; i < TMP117_IPC_SENSOR_COUNT; i++)
    {
        if (0U != i)
        {
            offset = protocol_append_char(p_payload, offset, payload_size, ',');
        }
        offset = protocol_append_int(p_payload, offset, payload_size, p_tmp->milli_c[i]);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"addr\":[");
    for (uint32_t i = 0U; i < TMP117_IPC_SENSOR_COUNT; i++)
    {
        if (0U != i)
        {
            offset = protocol_append_char(p_payload, offset, payload_size, ',');
        }
        offset = protocol_append_uint(p_payload, offset, payload_size, p_tmp->address[i]);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, "]}");

    /*
     * Keep this compact SCD41 core contract in every detail mode so gateway
     * health and environment telemetry never disappear during payload fallback.
     */
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"scd\":{\"seq\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ? p_scd->sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"s\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ? p_scd->status : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"co2\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ? p_scd->co2_ppm : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"t_mc\":");
    offset = protocol_append_int(p_payload,
                                 offset,
                                 payload_size,
                                 (SCD41_IPC_MAGIC == p_scd->magic) ?
                                 p_scd->temperature_milli_c : 0);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rh_mp\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  p_scd->humidity_milli_percent : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ready\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ? p_scd->data_ready : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"crc_err\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  p_scd->crc_error_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"i2c_err\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  p_scd->i2c_error_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"to\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ? p_scd->timeout_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rec\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ? p_scd->recovery_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fsp\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ? p_scd->last_fsp_err : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"last\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  fusion.environment_last_good_tick : 0U);
    offset = protocol_append_char(p_payload, offset, payload_size, '}');

    /*
     * Raw compensated BME688 output.  gas_ohm is not BSEC IAQ/VOC; consumers
     * must inspect flags bit 1 (gas-valid) and bit 2 (heater-stable).
     */
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"bme\":{\"s\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  p_scd->bme_status : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"addr\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  p_scd->bme_i2c_address : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"t_mc\":");
    offset = protocol_append_int(p_payload,
                                 offset,
                                 payload_size,
                                 (SCD41_IPC_MAGIC == p_scd->magic) ?
                                 p_scd->bme_temperature_milli_c : 0);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rh_mp\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  p_scd->bme_humidity_milli_percent : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"p_pa\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  p_scd->bme_pressure_pa : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"gas_ohm\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  p_scd->bme_gas_resistance_ohm : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"flags\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  p_scd->bme_flags : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"last\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  g_protocol_bme_last_good_tick : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"err\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (SCD41_IPC_MAGIC == p_scd->magic) ?
                                  p_scd->bme_error_count : 0U);
    offset = protocol_append_char(p_payload, offset, payload_size, '}');

    offset = protocol_append_text(p_payload, offset, payload_size, ",\"prs\":{\"seq\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (PRESSURE_IPC_MAGIC == p_prs->magic) ? p_prs->sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"s\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (PRESSURE_IPC_MAGIC == p_prs->magic) ? p_prs->status : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ok\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (PRESSURE_IPC_MAGIC == p_prs->magic) ? p_prs->connected_mask : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"err\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (PRESSURE_IPC_MAGIC == p_prs->magic) ? p_prs->error_mask : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fs\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (PRESSURE_IPC_MAGIC == p_prs->magic) ? p_prs->fail_step : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ch\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (PRESSURE_IPC_MAGIC == p_prs->magic) ? p_prs->fail_channel : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fsp\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (PRESSURE_IPC_MAGIC == p_prs->magic) ? p_prs->last_fsp_err : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"up_ch\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (PRESSURE_IPC_MAGIC == p_prs->magic) ? p_prs->detected_upstream_channel : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"raw\":[");
    for (uint32_t i = 0U; (STATUS_DETAIL_FULL == detail) &&
                            (PRESSURE_IPC_MAGIC == p_prs->magic) && (i < p_prs->raw_count) &&
                            (i < PRESSURE_IPC_RAW_COUNT); i++)
    {
        if (0U != i)
        {
            offset = protocol_append_char(p_payload, offset, payload_size, ',');
        }
        offset = protocol_append_int(p_payload, offset, payload_size, p_prs->raw[i]);
    }
    offset = protocol_append_char(p_payload, offset, payload_size, ']');
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"matrix\":[");
    for (uint32_t i = 0U; (STATUS_DETAIL_ESSENTIAL != detail) &&
                            (PRESSURE_IPC_MAGIC == p_prs->magic) &&
                            (i < PRESSURE_IPC_MATRIX_COUNT); i++)
    {
        if (0U != i)
        {
            offset = protocol_append_char(p_payload, offset, payload_size, ',');
        }
        offset = protocol_append_int(p_payload, offset, payload_size, p_prs->matrix[i]);
    }
    offset = protocol_append_char(p_payload, offset, payload_size, ']');
    offset = protocol_append_char(p_payload, offset, payload_size, '}');

    offset = protocol_append_text(p_payload, offset, payload_size, ",\"mic\":{\"seq\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cmd_seq\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->command_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ack\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->command_ack_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"s\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->status : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rate\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->sample_rate : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"bits\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->bits_per_word : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cb\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->callback_count : 0U);
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"q\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->queue_count : 0U);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"read\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->read_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"err\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->error_count : 0U);
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"to\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->timeout_count : 0U);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fsp\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->last_fsp_err : 0U);
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"ev\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->last_event : 0U);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"peak\":");
    offset = protocol_append_int(p_payload,
                                 offset,
                                 payload_size,
                                 (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->peak : 0);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"mean\":");
    offset = protocol_append_int(p_payload,
                                 offset,
                                 payload_size,
                                 (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->mean_abs : 0);
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"last\":");
        offset = protocol_append_int(p_payload,
                                     offset,
                                     payload_size,
                                     (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->last_sample : 0);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"nz\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->nonzero_count : 0U);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rec\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->record_active : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rec_bytes\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->record_bytes : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rec_sec\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->record_seconds : 0U);
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wpoll\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_poll_count : 0U);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"wst\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_status : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"werr\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_error : 0U);
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wopen\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_open_count : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wwr\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_write_count : 0U);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"wbytes\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_bytes : 0U);
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wseq\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_record_sequence : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wchunk\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_chunk_sequence : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wgap\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_gap_count : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wclick\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_declick_count : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"slot\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->record_slot_select : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"shift\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->record_sample_shift : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"slew\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->record_slew_limit : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"gain\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->record_gain_percent : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wslot\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_active_slot : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wepk\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_even_peak : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"wopk\":");
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      (MIC_IPC_MAGIC == p_mic->magic) ? p_mic->writer_odd_peak : 0U);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sdr\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SD_IPC_MAGIC == p_sd->magic) ? p_sd->ready : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sds\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SD_IPC_MAGIC == p_sd->magic) ? p_sd->step : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sdfs\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SD_IPC_MAGIC == p_sd->magic) ? p_sd->failure_step : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sdfl\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SD_IPC_MAGIC == p_sd->magic) ? p_sd->flags : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sdtry\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SD_IPC_MAGIC == p_sd->magic) ? p_sd->init_attempts : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sdfail\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SD_IPC_MAGIC == p_sd->magic) ? p_sd->consecutive_failures : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sdrec\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SD_IPC_MAGIC == p_sd->magic) ? p_sd->recovery_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sdfsp\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SD_IPC_MAGIC == p_sd->magic) ? p_sd->last_fsp_err : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sdff\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SD_IPC_MAGIC == p_sd->magic) ? p_sd->last_ff_err : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"file\":");
    if (MIC_IPC_MAGIC == p_mic->magic)
    {
        offset = protocol_append_json_fixed_string(p_payload,
                                                   offset,
                                                   payload_size,
                                                   p_mic->writer_filename,
                                                   sizeof(p_mic->writer_filename));
    }
    else
    {
        offset = protocol_append_json_string(p_payload, offset, payload_size, "");
    }
    offset = protocol_append_char(p_payload, offset, payload_size, '}');

    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cam\":{\"seq\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cmd_seq\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->command_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ack\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->command_ack_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"s\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->status : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fsp\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->last_fsp_err : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"uart_err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->uart_error_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"to\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->timeout_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"proto_err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->protocol_error_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"probe\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->probe_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"probe_ok\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->probe_ok_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cf\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->consecutive_failures : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rc\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->recovery_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rst\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->soft_reset_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ro\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->uart_reopen_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"re\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->recovery_error_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rr\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->last_recovery_reason : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"build\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->driver_build : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cap_cf\":");
    offset = protocol_append_uint(
        p_payload,
        offset,
        payload_size,
        (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ?
        p_cam->capture_consecutive_failures : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"nfr\":");
    offset = protocol_append_uint(
        p_payload,
        offset,
        payload_size,
        (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ?
        p_cam->recoveries_without_frame : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"pc\":");
    offset = protocol_append_uint(
        p_payload,
        offset,
        payload_size,
        (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ?
        p_cam->power_cycle_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"relay\":");
    offset = protocol_append_uint(
        p_payload,
        offset,
        payload_size,
        (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ?
        p_cam->power_relay_ready : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rec\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->capture_active : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"auto\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, SERIAL_CAMERA_AUTO_INFERENCE_ENABLED);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"auto_ms\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, SERIAL_CAMERA_AUTO_INFERENCE_INTERVAL_MS);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"frames\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->capture_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"frame_err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->capture_error_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"stage\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->capture_stage : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fail_stage\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->last_failed_stage : 0U);
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"last_cmd\":");
        offset = protocol_append_uint(p_payload, offset, payload_size,
                                      (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->last_command_code : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"rx_exp\":");
        offset = protocol_append_uint(p_payload, offset, payload_size,
                                      (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->rx_expected_bytes : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"rx_got\":");
        offset = protocol_append_uint(p_payload, offset, payload_size,
                                      (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->rx_received_bytes : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"rx0\":");
        offset = protocol_append_uint(p_payload, offset, payload_size,
                                      (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->last_reply[0] : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"rx1\":");
        offset = protocol_append_uint(p_payload, offset, payload_size,
                                      (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->last_reply[1] : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"rx2\":");
        offset = protocol_append_uint(p_payload, offset, payload_size,
                                      (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->last_reply[2] : 0U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"rx3\":");
        offset = protocol_append_uint(p_payload, offset, payload_size,
                                      (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->last_reply[3] : 0U);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rsp\":{");
    if (STATUS_DETAIL_FULL == detail)
    {
        offset = protocol_append_camera_response(
            p_payload,
            offset,
            payload_size,
            "stop",
            &p_cam->stage_response[SERIAL_CAMERA_CAPTURE_STAGE_STOP_FRAME]);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_camera_response(
            p_payload,
            offset,
            payload_size,
            "size",
            &p_cam->stage_response[SERIAL_CAMERA_CAPTURE_STAGE_GET_SIZE]);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_camera_response(
            p_payload,
            offset,
            payload_size,
            "read",
            &p_cam->stage_response[SERIAL_CAMERA_CAPTURE_STAGE_READ_CHUNK]);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_camera_response(
            p_payload,
            offset,
            payload_size,
            "resume",
            &p_cam->stage_response[SERIAL_CAMERA_CAPTURE_STAGE_RESUME_FRAME]);
    }
    offset = protocol_append_char(p_payload, offset, payload_size, '}');
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"bytes\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->image_size : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"soi\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->jpeg_soi_ok : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"eoi\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->jpeg_eoi_ok : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"wst\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->writer_status : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"werr\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->writer_error : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"saved\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->writer_saved_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"wbytes\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ? p_cam->writer_bytes : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"file\":");
    if (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic)
    {
        offset = protocol_append_json_fixed_string(p_payload,
                                                   offset,
                                                   payload_size,
                                                   p_cam->writer_filename,
                                                   sizeof(p_cam->writer_filename));
    }
    else
    {
        offset = protocol_append_json_string(p_payload, offset, payload_size, "");
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ver\":");
    offset = protocol_append_json_string(p_payload, offset, payload_size,
                                         (SERIAL_CAMERA_IPC_MAGIC == p_cam->magic) ?
                                         (char const *) p_cam->version : "");
    offset = protocol_append_char(p_payload, offset, payload_size, '}');

    offset = protocol_append_text(p_payload, offset, payload_size, ",\"aud\":{\"seq\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->status_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cmd_seq\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->command_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ack\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->command_ack_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"s\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->status : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"track\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->track_index : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"count\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->track_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"vol\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->volume_percent : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"elapsed\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->elapsed_ms : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"duration\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->duration_ms : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rate\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->sample_rate : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"file\":");
    offset = protocol_append_json_string(p_payload,
                                         offset,
                                         payload_size,
                                         (AUDIO_IPC_MAGIC == p_aud->magic) ? (char const *) p_aud->current_name : "");
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"story\":{\"op\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_operation : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"seq\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_operation_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ack\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_operation_ack_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"s\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_status : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"id\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_transfer_id : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"total\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_total_bytes : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rx\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_received_bytes : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"crc\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_running_crc32 : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"exp\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_expected_crc32 : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"res\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_result : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_error_count : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ready\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_ready_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"play_ack\":");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_play_ack_sequence : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"file\":");
    if (AUDIO_IPC_MAGIC == p_aud->magic)
    {
        offset = protocol_append_json_fixed_string(p_payload,
                                                   offset,
                                                   payload_size,
                                                   p_aud->story_name,
                                                   sizeof(p_aud->story_name));
    }
    else
    {
        offset = protocol_append_json_string(p_payload, offset, payload_size, "");
    }
    offset = protocol_append_char(p_payload, offset, payload_size, '}');
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"tracks\":[");

    for (uint32_t i = 0U; (STATUS_DETAIL_FULL == detail) &&
                            (AUDIO_IPC_MAGIC == p_aud->magic) &&
                            (i < p_aud->track_count) && (i < AUDIO_IPC_TRACK_MAX); i++)
    {
        if (0U != i)
        {
            offset = protocol_append_char(p_payload, offset, payload_size, ',');
        }

        offset = protocol_append_text(p_payload, offset, payload_size, "{\"i\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, i + 1U);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"name\":");
        offset = protocol_append_json_string(p_payload, offset, payload_size, (char const *) p_aud->tracks[i].name);
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"duration\":");
        offset = protocol_append_uint(p_payload, offset, payload_size, p_aud->tracks[i].duration_ms);
        offset = protocol_append_char(p_payload, offset, payload_size, '}');
    }

    offset = protocol_append_text(p_payload, offset, payload_size, "]}");
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"safe\":{\"seq\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"state\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, (uint32_t) g_safety.state);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"conf\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.confidence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"severity\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.severity);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sources\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.source_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"since\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.state_since_tick);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"event\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.event_id);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"pending\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.pending_event_id);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"acks\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.ack_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"retries\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.retry_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"faults\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.sensor_fault_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"level\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.global_alarm_level);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"incident\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.pending_incident_id);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"type\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.pending_incident_type);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"action\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.pending_action);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"p_total\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.pressure_total);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"p_edge\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.pressure_edge);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"px100\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.pressure_x100);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"py100\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_safety.pressure_y100);
    offset = protocol_append_text(p_payload, offset, payload_size, "},\"fusion\":{\"seq\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"state\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.state);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"candidate\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.candidate_state);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"conf\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.confidence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"severity\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.severity);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sources\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.source_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"valid\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.valid_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"faults\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.sensor_fault_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"since\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.state_since_tick);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"occupied\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.occupied);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"p_total\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.pressure_total);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"p_peak\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.pressure_peak);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"p_points\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.pressure_active_points);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"p_edge\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.pressure_edge);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"px100\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.pressure_x100);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"py100\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.pressure_y100);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"thm_min_cd\":");
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.thermal_min_cdeg);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"thm_max_cd\":");
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.thermal_max_cdeg);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"bed_min_mc\":");
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.bed_min_milli_c);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"bed_max_mc\":");
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.bed_max_milli_c);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.cry_confidence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_raw\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_probability_x1000);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_run\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_consecutive_windows);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_ok\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_confirmed);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_hold\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_release_hold_windows);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_n\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_inference_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_ms\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_last_inference_ms);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_ready\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_model_ready);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_exp\":1");
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type\":\"");
    offset = protocol_append_text(p_payload, offset, payload_size, audio_cry_type_label());
    offset = protocol_append_text(p_payload, offset, payload_size, "\"");
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_ready\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_type_model_ready);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_ok\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_type_valid);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_id\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_type_id);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_conf\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_type_confidence_x1000);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_margin\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_type_margin_x1000);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_n\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_cry_type_window_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_hu\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  g_cry_type_probabilities_x1000[0]);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_bp\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  g_cry_type_probabilities_x1000[1]);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_dc\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  g_cry_type_probabilities_x1000[2]);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_ti\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  g_cry_type_probabilities_x1000[3]);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_bu\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  g_cry_type_probabilities_x1000[4]);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sleep\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.sleep_confidence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"covered\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.face_covered_confidence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"standing\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.standing_confidence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"movement\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.movement_confidence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"sv\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.schema_version);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ev\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.evidence_sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ctx\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.presence);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.arousal);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.bed_motion);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.airway_view);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.edge_state);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.distress);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.cry_cause_hint);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.body_temperature);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.bed_thermal);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.caregiver);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.system_health);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"alarm\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.global_alarm_level);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.primary_incident_type);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.primary_incident_id);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.primary_incident_lifecycle);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.active_incident_count);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.active_type_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"pv\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.pressure_edge_ratio_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.pressure_activity_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.pressure_move_x100);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.pressure_move_y100);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"ex\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.presence_score_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.presence_source_count);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.pressure_presence_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.vision_presence_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.thermal_presence_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.activity_score_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.airway_risk_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.edge_risk_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.thermal_hotspot_points);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.thermal_hotspot_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.thermal_hotspot_y1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.thermal_hotspot_mean_cdeg);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.bed_left_milli_c);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.bed_center_milli_c);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.bed_right_milli_c);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size, fusion.bed_spread_milli_c);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.bed_hotspot_region);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.thermal_bed_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.thermal_edge_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.position_agreement_x1000);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.calibration_profile_version);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"vv\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.vision_sequence);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.vision_age_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.infant_present);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.head_present);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.infant_at_edge);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.infant_edge_distance_x1000);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"comfort\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.comfort_action_mask);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.comfort_reason_mask);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.comfort_confidence);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.comfort_requires_confirmation);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.comfort_block_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"limits\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  fusion.environment_thresholds.co2_max_ppm);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size,
                                 fusion.environment_thresholds.temperature_max_milli_c);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  fusion.environment_thresholds.humidity_max_milli_percent);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  fusion.environment_thresholds.gas_min_ohm);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size,
                                 fusion.environment_thresholds.body_temperature_max_deci_c);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  fusion.environment_thresholds.revision);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"limits2\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  fusion.environment_thresholds.co2_max_ppm);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size,
                                 fusion.environment_thresholds.temperature_max_milli_c);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  fusion.environment_thresholds.humidity_max_milli_percent);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  fusion.environment_thresholds.gas_min_ohm);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_int(p_payload, offset, payload_size,
                                 fusion.environment_thresholds.body_temperature_max_deci_c);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  fusion.environment_thresholds.humidity_min_milli_percent);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size,
                                  fusion.environment_thresholds.revision);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"drop\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, fusion.incident_event_drop_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ai\":[");
    for (uint32_t i = 0U;
         (STATUS_DETAIL_ESSENTIAL != detail) &&
         (i < fusion.active_incident_count) &&
         (i < MULTIMODAL_FUSION_MAX_ACTIVE_INCIDENTS);
         i++)
    {
        multimodal_incident_summary_t const * p_incident = &fusion.active_incidents[i];
        if (0U != i)
        {
            offset = protocol_append_char(p_payload, offset, payload_size, ',');
        }
        offset = protocol_append_char(p_payload, offset, payload_size, '[');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_incident->type);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_incident->lifecycle);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_incident->level);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_incident->severity);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_incident->confidence);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_incident->source_mask);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_incident->incident_id);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_incident->since_tick);
        offset = protocol_append_char(p_payload, offset, payload_size, ']');
    }
    offset = protocol_append_char(p_payload, offset, payload_size, ']');
    /* The legacy inference.v copy exceeded the 4096-byte UART payload. */
#if 0
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"v\":{\"q\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"f\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.frame_sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"s\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.status);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"r\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.model_ready_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"n\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.inference_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"w\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.image_width);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"h\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.image_height);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"jb\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_bytes);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"os\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.ospi_error);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ml\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.model_load_ms);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"t\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.decode_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.preprocess_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.scene_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.occlusion_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.behavior_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.total_ms);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"e\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_error_count);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.npu_error_count);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.dropped_frame_count);
    offset = protocol_append_char(p_payload, offset, payload_size, ']');
    if (STATUS_DETAIL_ESSENTIAL != detail)
    {
        offset = protocol_append_text(p_payload, offset, payload_size, ",\"jd\":[");
        offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_result);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_decoder_result);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_input_offset);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_source_width);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_source_height);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_marker_flags);
        offset = protocol_append_char(p_payload, offset, payload_size, ']');
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ss\":[");
    for (uint32_t i = 0U; i < VISION_SCENE_CLASS_COUNT; i++)
    {
        if (0U != i)
        {
            offset = protocol_append_char(p_payload, offset, payload_size, ',');
        }
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      vision.scene_scores_x1000[i]);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"oc\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.occlusion_class);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  vision.occlusion_confidence_x1000);
    for (uint32_t i = 0U; i < VISION_OCCLUSION_CLASS_COUNT; i++)
    {
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      vision.occlusion_probabilities_x1000[i]);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"bh\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.behavior_class);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  vision.behavior_confidence_x1000);
    for (uint32_t i = 0U; i < VISION_BEHAVIOR_CLASS_COUNT; i++)
    {
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      vision.behavior_probabilities_x1000[i]);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"fe\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.fusion_eligible);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"dc\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.detection_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"d\":[");
    for (uint32_t i = 0U;
         (i < vision.detection_count) && (i < 4U);
         i++)
    {
        vision_detection_t const * p_detection = &vision.detections[i];
        if (0U != i)
        {
            offset = protocol_append_char(p_payload, offset, payload_size, ',');
        }
        offset = protocol_append_char(p_payload, offset, payload_size, '[');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->class_id);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->score_x1000);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->x);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->y);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->width);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->height);
        offset = protocol_append_char(p_payload, offset, payload_size, ']');
    }
    offset = protocol_append_text(p_payload, offset, payload_size, "]}");
#endif
    offset = protocol_append_text(p_payload, offset, payload_size, "},\"vision\":{\"q\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"f\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.frame_sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"s\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.status);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"r\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.model_ready_mask);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"n\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.inference_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"w\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.image_width);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"h\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.image_height);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"jb\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_bytes);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"os\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.ospi_error);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ml\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.model_load_ms);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"t\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.decode_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.preprocess_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.scene_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.occlusion_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.behavior_ms);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.total_ms);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"e\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_error_count);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.npu_error_count);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.dropped_frame_count);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"jd\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_result);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_decoder_result);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_input_offset);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_source_width);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_source_height);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.jpeg_marker_flags);
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"ss\":[");
    for (uint32_t i = 0U; i < VISION_SCENE_CLASS_COUNT; i++)
    {
        if (0U != i)
        {
            offset = protocol_append_char(p_payload, offset, payload_size, ',');
        }
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      vision.scene_scores_x1000[i]);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"oc\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.occlusion_class);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  vision.occlusion_confidence_x1000);
    for (uint32_t i = 0U; i < VISION_OCCLUSION_CLASS_COUNT; i++)
    {
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      vision.occlusion_probabilities_x1000[i]);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"bh\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.behavior_class);
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  vision.behavior_confidence_x1000);
    for (uint32_t i = 0U; i < VISION_BEHAVIOR_CLASS_COUNT; i++)
    {
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload,
                                      offset,
                                      payload_size,
                                      vision.behavior_probabilities_x1000[i]);
    }
    offset = protocol_append_text(p_payload, offset, payload_size, "],\"fe\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.fusion_eligible);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ov\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.occlusion_valid);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"dc\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, vision.detection_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"d\":[");
    for (uint32_t i = 0U;
         (i < vision.detection_count) && (i < 4U);
         i++)
    {
        vision_detection_t const * p_detection = &vision.detections[i];
        if (0U != i)
        {
            offset = protocol_append_char(p_payload, offset, payload_size, ',');
        }
        offset = protocol_append_char(p_payload, offset, payload_size, '[');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->class_id);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->score_x1000);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->x);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->y);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->width);
        offset = protocol_append_char(p_payload, offset, payload_size, ',');
        offset = protocol_append_uint(p_payload, offset, payload_size, p_detection->height);
        offset = protocol_append_char(p_payload, offset, payload_size, ']');
    }
    offset = protocol_append_text(p_payload, offset, payload_size, "]},\"light\":{\"s\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (0U != warm_light.ready) ? 1U : 2U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"on\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, warm_light.on);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"pin\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, WARM_LIGHT_CONTROL_PIN_LABEL);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ah\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, WARM_LIGHT_CONTROL_ACTIVE_HIGH);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cmd_seq\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, warm_light.command_sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, warm_light.error_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fsp\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, warm_light.last_fsp_error);
    offset = protocol_append_text(p_payload, offset, payload_size, "},\"pat\":{\"s\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (0U != soother_pat.ready) ? 1U : 2U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"on\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, soother_pat.on);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"pin\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, SOOTHER_CONTROL_PIN_LABEL);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ah\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, SOOTHER_CONTROL_ACTIVE_HIGH);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cmd_seq\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, soother_pat.command_sequence);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, soother_pat.error_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fsp\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, soother_pat.last_fsp_error);
    offset = protocol_append_text(p_payload, offset, payload_size, "},\"link\":{\"rx\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, uart_stats.rx_bytes);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"tx\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, uart_stats.tx_complete_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"tx_busy\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, uart_stats.tx_busy_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"tx_to\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, uart_stats.tx_timeout_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, uart_stats.error_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ovf\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, uart_stats.rx_overflow_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"event\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, (uint32_t) uart_stats.last_event);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"fw\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, RA8_CPU0_FIRMWARE_BUILD);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"vision_fw\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, VISION_RUNTIME_BUILD);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rules\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, MULTIMODAL_FUSION_RULE_VERSION);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_fw\":");
    offset = protocol_append_json_string(p_payload,
                                         offset,
                                         payload_size,
                                         audio_cry_model_version());
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"cry_type_fw\":");
    offset = protocol_append_json_string(p_payload,
                                         offset,
                                         payload_size,
                                         audio_cry_type_model_version());
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"heap\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, xPortGetFreeHeapSize());
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"heap_min\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, xPortGetMinimumEverFreeHeapSize());
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"stk\":[");
    offset = protocol_append_uint(p_payload, offset, payload_size, protocol_stack_high_water(camera_thread));
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  protocol_stack_high_water(vision_preprocess_thread));
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  protocol_stack_high_water(vision_infer_thread));
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, protocol_stack_high_water(audio_infer_thread));
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, protocol_stack_high_water(fusion_thread));
    offset = protocol_append_char(p_payload, offset, payload_size, ',');
    offset = protocol_append_uint(p_payload, offset, payload_size, protocol_stack_high_water(esp32_uart_thread));
    offset = protocol_append_char(p_payload, offset, payload_size, ']');
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ready\":");
    offset = protocol_append_uint(
        p_payload, offset, payload_size, ra8_esp32_protocol_gateway_ready() ? 1U : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ready_n\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_protocol_gateway_ready_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ready_age\":");
    offset = protocol_append_uint(
        p_payload,
        offset,
        payload_size,
        (0U != g_protocol_gateway_ready_tick) ?
        (uint32_t) (xTaskGetTickCount() - g_protocol_gateway_ready_tick) : UINT32_MAX);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"recover\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, uart_stats.recovery_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"recover_err\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, uart_stats.recovery_error_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_try\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_protocol_status_attempt_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_ok\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_protocol_status_success_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_len\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_protocol_status_payload_length);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_detail\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_protocol_status_detail);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_long\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_protocol_status_payload_too_long_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_uart\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_protocol_status_uart_error_count);
    offset = protocol_append_text(p_payload, offset, payload_size, "}}");

    return offset;
}

static void protocol_put_be32(uint8_t * p_buffer, uint32_t offset, uint32_t value)
{
    p_buffer[offset] = (uint8_t) (value >> 24);
    p_buffer[offset + 1U] = (uint8_t) (value >> 16);
    p_buffer[offset + 2U] = (uint8_t) (value >> 8);
    p_buffer[offset + 3U] = (uint8_t) value;
}

void resource_runtime_counter_configure(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
}

uint32_t resource_runtime_counter_get(void)
{
    return DWT->CYCCNT;
}

static uint32_t protocol_build_emergency_status_payload(char * p_payload,
                                                        uint32_t payload_size,
                                                        uint32_t oversized_length)
{
    warm_light_control_state_t warm_light = {0};
    soother_pat_control_state_t soother_pat = {0};
    uint32_t offset = 0U;

    warm_light_control_get_state(&warm_light);
    soother_pat_control_get_state(&soother_pat);

    offset = protocol_append_text(p_payload, offset, payload_size, "{\"tick\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, (uint32_t) xTaskGetTickCount());
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"status_truncated\":1,\"link\":{\"fw\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, RA8_CPU0_FIRMWARE_BUILD);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"rules\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, MULTIMODAL_FUSION_RULE_VERSION);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"ready\":");
    offset = protocol_append_uint(
        p_payload, offset, payload_size, ra8_esp32_protocol_gateway_ready() ? 1U : 0U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_try\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_protocol_status_attempt_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_ok\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_protocol_status_success_count);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_len\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, oversized_length);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_detail\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, STATUS_DETAIL_EMERGENCY);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"st_long\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, g_protocol_status_payload_too_long_count);
    offset = protocol_append_text(p_payload, offset, payload_size, "},\"light\":{\"s\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (0U != warm_light.ready) ? 1U : 2U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"on\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, warm_light.on);
    offset = protocol_append_text(p_payload, offset, payload_size, "},\"pat\":{\"s\":");
    offset = protocol_append_uint(p_payload,
                                  offset,
                                  payload_size,
                                  (0U != soother_pat.ready) ? 1U : 2U);
    offset = protocol_append_text(p_payload, offset, payload_size, ",\"on\":");
    offset = protocol_append_uint(p_payload, offset, payload_size, soother_pat.on);
    offset = protocol_append_text(p_payload, offset, payload_size, "}}");

    return offset;
}

static ra8_esp32_protocol_status_t protocol_send_frame(ra8_esp32_msg_type_t type,
                                                       const uint8_t * p_payload,
                                                       uint16_t payload_length,
                                                       TickType_t timeout_ticks)
{
    uint8_t * p_frame = g_protocol_tx_frame;
    uint32_t crc_offset;
    uint16_t crc;
    esp32_uart_status_t uart_status;

    if (payload_length > RA8_ESP32_PROTOCOL_MAX_PAYLOAD)
    {
        return RA8_ESP32_PROTOCOL_STATUS_PAYLOAD_TOO_LONG;
    }

    if ((NULL == g_protocol_tx_lock) ||
        (pdTRUE != xSemaphoreTake(g_protocol_tx_lock, timeout_ticks)))
    {
        return RA8_ESP32_PROTOCOL_STATUS_UART_ERROR;
    }

    p_frame[0] = RA8_ESP32_PROTOCOL_MAGIC_MSB;
    p_frame[1] = RA8_ESP32_PROTOCOL_MAGIC_LSB;
    p_frame[2] = RA8_ESP32_PROTOCOL_VERSION;
    p_frame[3] = (uint8_t) type;
    protocol_put_be16(p_frame, 4U, g_ra8_esp32_sequence++);
    protocol_put_be16(p_frame, 6U, payload_length);

    for (uint32_t i = 0U; i < payload_length; i++)
    {
        p_frame[RA8_ESP32_PROTOCOL_HEADER_LENGTH + i] = p_payload[i];
    }

    crc_offset = RA8_ESP32_PROTOCOL_HEADER_LENGTH + payload_length;
    crc = ra8_esp32_protocol_crc16_ccitt(p_frame, crc_offset);
    protocol_put_be16(p_frame, crc_offset, crc);

    uart_status = esp32_uart_driver_write(p_frame,
                                          crc_offset + RA8_ESP32_PROTOCOL_CRC_LENGTH,
                                          timeout_ticks);
    (void) xSemaphoreGive(g_protocol_tx_lock);
    if (ESP32_UART_STATUS_OK != uart_status)
    {
        return RA8_ESP32_PROTOCOL_STATUS_UART_ERROR;
    }

    return RA8_ESP32_PROTOCOL_STATUS_OK;
}

ra8_esp32_protocol_status_t ra8_esp32_protocol_send_status(TickType_t timeout_ticks)
{
    status_detail_t detail = STATUS_DETAIL_FULL;
    ra8_esp32_protocol_status_t status;

    g_protocol_status_attempt_count++;
    uint32_t payload_length = protocol_build_status_payload(g_protocol_status_payload,
                                                            sizeof(g_protocol_status_payload),
                                                            detail);

    if (payload_length > PROTOCOL_STATUS_PAYLOAD_SOFT_LIMIT)
    {
        detail = STATUS_DETAIL_COMPACT;
        payload_length = protocol_build_status_payload(g_protocol_status_payload,
                                                       sizeof(g_protocol_status_payload),
                                                       detail);
    }

    if (payload_length > PROTOCOL_STATUS_PAYLOAD_SOFT_LIMIT)
    {
        detail = STATUS_DETAIL_ESSENTIAL;
        payload_length = protocol_build_status_payload(g_protocol_status_payload,
                                                       sizeof(g_protocol_status_payload),
                                                       detail);
    }

    g_protocol_status_payload_length = payload_length;
    g_protocol_status_detail = (uint32_t) detail;
    if (payload_length > sizeof(g_protocol_status_payload))
    {
        g_protocol_status_payload_too_long_count++;
        g_protocol_status_detail = (uint32_t) STATUS_DETAIL_EMERGENCY;
        payload_length = protocol_build_emergency_status_payload(g_protocol_status_payload,
                                                                 sizeof(g_protocol_status_payload),
                                                                 payload_length);
        if (payload_length > sizeof(g_protocol_status_payload))
        {
            return RA8_ESP32_PROTOCOL_STATUS_PAYLOAD_TOO_LONG;
        }
    }

    status = protocol_send_frame(RA8_ESP32_MSG_STATUS_UPLOAD,
                                 (const uint8_t *) g_protocol_status_payload,
                                 (uint16_t) payload_length,
                                 timeout_ticks);
    if (RA8_ESP32_PROTOCOL_STATUS_OK == status)
    {
        g_protocol_status_success_count++;
    }
    else if (RA8_ESP32_PROTOCOL_STATUS_UART_ERROR == status)
    {
        g_protocol_status_uart_error_count++;
    }

    return status;
}

ra8_esp32_protocol_status_t ra8_esp32_protocol_send_resources(TickType_t timeout_ticks)
{
    uint8_t payload[RA8_RESOURCE_PAYLOAD_BYTES] = {0};
    uint32_t const tick = (uint32_t) xTaskGetTickCount();
    uint32_t const counter = resource_runtime_counter_get();
    uint32_t const idle = (uint32_t) ulTaskGetIdleRunTimeCounter();
    uint32_t window_ms = 0U;
    uint32_t cpu0_usage_permille = 0U;
    uint32_t cpu1_usage_permille = 0U;
    uint32_t cpu1_heap_free = 0U;
    uint32_t cpu1_heap_min = 0U;
    uint32_t cpu1_tick = 0U;
    uint32_t flags = 0U;
    vision_runtime_snapshot_t vision = {0};

    if (g_resource_sample_started)
    {
        uint32_t const total_delta = counter - g_resource_previous_counter;
        uint32_t const idle_delta = idle - g_resource_previous_idle;
        window_ms = (uint32_t) ((tick - g_resource_previous_tick) * portTICK_PERIOD_MS);
        if (total_delta > 0U)
        {
            uint32_t const idle_permille =
                (uint32_t) ((((uint64_t) idle_delta) * 1000ULL) / total_delta);
            cpu0_usage_permille =
                (idle_permille < 1000U) ? (1000U - idle_permille) : 0U;
            flags |= 1U;
        }
    }

    volatile sd_ipc_mailbox_t const * const p_sd = SD_IPC_MAILBOX;
    if ((SD_IPC_MAGIC == p_sd->magic) && (p_sd->version >= 3U))
    {
        for (uint32_t attempt = 0U; attempt < 3U; attempt++)
        {
            uint32_t const before = p_sd->resource_generation;
            if (0U != (before & 1U))
            {
                continue;
            }
            __DMB();
            uint32_t const sequence = p_sd->resource_sequence;
            cpu1_tick = p_sd->resource_tick;
            cpu1_usage_permille = p_sd->cpu_usage_permille;
            cpu1_heap_free = p_sd->heap_free_bytes;
            cpu1_heap_min = p_sd->heap_min_bytes;
            __DMB();
            uint32_t const after = p_sd->resource_generation;
            if ((before == after) && (0U == (after & 1U)) && (sequence > 0U))
            {
                flags |= 2U;
                break;
            }
        }
    }

    uint32_t npu_busy_delta = 0U;
    uint32_t inference_delta = 0U;
    uint32_t npu_usage_permille = 0U;
    uint32_t vision_total_ms = 0U;
    if (vision_runtime_get_snapshot(&vision))
    {
        npu_busy_delta = vision.npu_busy_ms_total - g_resource_previous_npu_busy_ms;
        inference_delta = vision.inference_count - g_resource_previous_inference_count;
        vision_total_ms = vision.total_ms;
        if (g_resource_sample_started && (window_ms > 0U))
        {
            uint64_t const scaled = ((uint64_t) npu_busy_delta) * 1000ULL;
            npu_usage_permille = (uint32_t) (scaled / window_ms);
            if (npu_usage_permille > 1000U)
            {
                npu_usage_permille = 1000U;
            }
            flags |= 4U;
        }
        g_resource_previous_npu_busy_ms = vision.npu_busy_ms_total;
        g_resource_previous_inference_count = vision.inference_count;
    }

    payload[0] = RA8_RESOURCE_PAYLOAD_VERSION;
    payload[1] = (uint8_t) flags;
    protocol_put_be16(payload, 2U, RA8_RESOURCE_PAYLOAD_BYTES);
    protocol_put_be32(payload, 4U, ++g_resource_sequence);
    protocol_put_be32(payload, 8U, tick);
    protocol_put_be32(payload, 12U, window_ms);
    protocol_put_be16(payload, 16U, (uint16_t) cpu0_usage_permille);
    protocol_put_be16(payload, 18U, (uint16_t) cpu1_usage_permille);
    protocol_put_be16(payload, 20U, (uint16_t) npu_usage_permille);
    protocol_put_be16(payload, 22U, 0U);
    protocol_put_be32(payload, 24U, (uint32_t) xPortGetFreeHeapSize());
    protocol_put_be32(payload, 28U, (uint32_t) xPortGetMinimumEverFreeHeapSize());
    protocol_put_be32(payload, 32U, cpu1_heap_free);
    protocol_put_be32(payload, 36U, cpu1_heap_min);
    protocol_put_be32(payload, 40U, npu_busy_delta);
    protocol_put_be32(payload, 44U, inference_delta);
    protocol_put_be32(payload, 48U, vision_total_ms);
    protocol_put_be32(payload, 52U, cpu1_tick);

    g_resource_previous_counter = counter;
    g_resource_previous_idle = idle;
    g_resource_previous_tick = tick;
    g_resource_sample_started = true;

    return protocol_send_frame(RA8_ESP32_MSG_RESOURCE_UPLOAD,
                               payload,
                               sizeof(payload),
                               timeout_ticks);
}

ra8_esp32_protocol_status_t ra8_esp32_protocol_send_pressure_matrix(TickType_t timeout_ticks)
{
    volatile pressure_ipc_mailbox_t const * p_prs = PRESSURE_IPC_MAILBOX;
    uint8_t * p_payload = g_protocol_pressure_payload;
    uint32_t payload_index = 0U;
    uint32_t sequence = 0U;

    if (PRESSURE_IPC_MAGIC != p_prs->magic)
    {
        return RA8_ESP32_PROTOCOL_STATUS_ERROR;
    }

    for (uint32_t attempt = 0U; attempt < 3U; attempt++)
    {
        uint32_t before = p_prs->sequence;

        __DMB();
        for (uint32_t i = 0U; i < PRESSURE_IPC_MATRIX_COUNT; i++)
        {
            g_protocol_pressure_snapshot[i] = p_prs->matrix[i];
        }
        __DMB();
        if ((before != 0U) && (before == p_prs->sequence))
        {
            sequence = before;
            break;
        }
    }

    if (0U == sequence)
    {
        return RA8_ESP32_PROTOCOL_STATUS_ERROR;
    }

    p_payload[payload_index++] = (uint8_t) (sequence >> 24);
    p_payload[payload_index++] = (uint8_t) (sequence >> 16);
    p_payload[payload_index++] = (uint8_t) (sequence >> 8);
    p_payload[payload_index++] = (uint8_t) sequence;
    protocol_put_be16(p_payload, payload_index, RA8_ESP32_PRESSURE_WIDTH);
    payload_index += 2U;
    protocol_put_be16(p_payload, payload_index, RA8_ESP32_PRESSURE_HEIGHT);
    payload_index += 2U;
    protocol_put_be16(p_payload, payload_index, 0U);
    payload_index += 2U;
    protocol_put_be16(p_payload, payload_index, PRESSURE_IPC_MATRIX_COUNT);
    payload_index += 2U;

    for (uint32_t i = 0U; i < PRESSURE_IPC_MATRIX_COUNT; i++)
    {
        uint16_t sample = (uint16_t) g_protocol_pressure_snapshot[i];
        p_payload[payload_index++] = (uint8_t) (sample >> 8);
        p_payload[payload_index++] = (uint8_t) sample;
    }

    return protocol_send_frame(RA8_ESP32_MSG_PRESSURE_UPLOAD,
                               p_payload,
                               (uint16_t) payload_index,
                               timeout_ticks);
}

static bool protocol_payload_has(uint8_t const * p_payload, uint16_t length, char const * p_token)
{
    size_t token_length = strlen(p_token);

    if ((0U == token_length) || (length < token_length))
    {
        return false;
    }

    for (uint32_t i = 0U; i <= ((uint32_t) length - token_length); i++)
    {
        if (0 == memcmp(&p_payload[i], p_token, token_length))
        {
            return true;
        }
    }

    return false;
}

void ra8_esp32_protocol_init(void)
{
    if (NULL == g_protocol_tx_lock)
    {
        g_protocol_tx_lock = xSemaphoreCreateBinaryStatic(&g_protocol_tx_lock_storage);
        if (NULL != g_protocol_tx_lock)
        {
            (void) xSemaphoreGive(g_protocol_tx_lock);
        }
    }
}

static uint16_t protocol_get_be16(uint8_t const * p_buffer, uint32_t offset)
{
    return (uint16_t) (((uint16_t) p_buffer[offset] << 8) |
                       (uint16_t) p_buffer[offset + 1U]);
}

static uint32_t protocol_get_be32(uint8_t const * p_buffer, uint32_t offset)
{
    return ((uint32_t) p_buffer[offset] << 24) |
           ((uint32_t) p_buffer[offset + 1U] << 16) |
           ((uint32_t) p_buffer[offset + 2U] << 8) |
           (uint32_t) p_buffer[offset + 3U];
}

static bool protocol_payload_equals(uint8_t const * p_payload, uint16_t length, char const * p_text)
{
    size_t text_length = strlen(p_text);

    return ((size_t) length == text_length) &&
           (0 == memcmp(p_payload, p_text, text_length));
}

static bool protocol_parse_environment_thresholds(
    uint8_t const * p_payload,
    uint16_t length,
    multimodal_environment_thresholds_t * p_thresholds)
{
    static char const prefix[] = "environment_thresholds:";
    uint32_t values[5] = {0U};
    size_t prefix_length = sizeof(prefix) - 1U;
    size_t offset = prefix_length;

    if ((NULL == p_payload) || (NULL == p_thresholds) ||
        ((size_t) length <= prefix_length) ||
        (0 != memcmp(p_payload, prefix, prefix_length)))
    {
        return false;
    }

    for (uint32_t field = 0U; field < 5U; field++)
    {
        bool has_digit = false;
        uint32_t value = 0U;

        while (offset < (size_t) length)
        {
            uint8_t character = p_payload[offset];
            if ((uint8_t) ',' == character)
            {
                break;
            }
            if ((character < (uint8_t) '0') || (character > (uint8_t) '9'))
            {
                return false;
            }
            character = (uint8_t) (character - (uint8_t) '0');
            if (value > ((UINT32_MAX - (uint32_t) character) / 10U))
            {
                return false;
            }
            value = (value * 10U) + (uint32_t) character;
            has_digit = true;
            offset++;
        }

        if (!has_digit || ((field < 4U) &&
                           ((offset >= (size_t) length) ||
                            ((uint8_t) ',' != p_payload[offset]))) ||
            ((4U == field) && (offset != (size_t) length)))
        {
            return false;
        }
        values[field] = value;
        if (field < 4U)
        {
            offset++;
        }
    }

    p_thresholds->co2_max_ppm = values[0];
    p_thresholds->temperature_max_milli_c = (int32_t) values[1];
    p_thresholds->humidity_max_milli_percent = values[2];
    p_thresholds->humidity_min_milli_percent = 30000U;
    p_thresholds->gas_min_ohm = values[3];
    p_thresholds->body_temperature_max_deci_c = (int32_t) values[4];
    p_thresholds->revision = 0U;
    return true;
}

static bool protocol_parse_environment_thresholds_v2(
    uint8_t const * p_payload,
    uint16_t length,
    multimodal_environment_thresholds_t * p_thresholds)
{
    static char const prefix[] = "environment_thresholds_v2:";
    uint32_t values[6] = {0U};
    size_t offset = sizeof(prefix) - 1U;

    if ((NULL == p_payload) || (NULL == p_thresholds) ||
        ((size_t) length <= offset) ||
        (0 != memcmp(p_payload, prefix, offset)))
    {
        return false;
    }
    for (uint32_t field = 0U; field < 6U; field++)
    {
        bool has_digit = false;
        uint32_t value = 0U;
        while (offset < (size_t) length)
        {
            uint8_t character = p_payload[offset];
            if ((uint8_t) ',' == character)
            {
                break;
            }
            if ((character < (uint8_t) '0') || (character > (uint8_t) '9'))
            {
                return false;
            }
            character = (uint8_t) (character - (uint8_t) '0');
            if (value > ((UINT32_MAX - (uint32_t) character) / 10U))
            {
                return false;
            }
            value = (value * 10U) + (uint32_t) character;
            has_digit = true;
            offset++;
        }
        if (!has_digit || ((field < 5U) &&
                           ((offset >= (size_t) length) ||
                            ((uint8_t) ',' != p_payload[offset]))) ||
            ((5U == field) && (offset != (size_t) length)))
        {
            return false;
        }
        values[field] = value;
        if (field < 5U)
        {
            offset++;
        }
    }
    p_thresholds->co2_max_ppm = values[0];
    p_thresholds->temperature_max_milli_c = (int32_t) values[1];
    p_thresholds->humidity_max_milli_percent = values[2];
    p_thresholds->gas_min_ohm = values[3];
    p_thresholds->body_temperature_max_deci_c = (int32_t) values[4];
    p_thresholds->humidity_min_milli_percent = values[5];
    p_thresholds->revision = 0U;
    return true;
}

static bool protocol_is_silent_reset_command(uint8_t const * p_payload, uint16_t length)
{
    /*
     * Reset is intentionally stricter than the legacy substring commands.
     * Accept only the documented raw token or canonical JSON object.
     */
    return protocol_payload_equals(p_payload, length, "silent_reset") ||
           protocol_payload_equals(p_payload, length, "{\"cmd\":\"silent_reset\"}");
}

static bool protocol_record_basename_char_valid(uint8_t value)
{
    return ((value >= (uint8_t) 'A') && (value <= (uint8_t) 'Z')) ||
           ((value >= (uint8_t) 'a') && (value <= (uint8_t) 'z')) ||
           ((value >= (uint8_t) '0') && (value <= (uint8_t) '9')) ||
           ((uint8_t) '_' == value) || ((uint8_t) '-' == value);
}

static bool protocol_copy_record_basename(uint8_t const * p_payload,
                                          uint16_t length,
                                          char const * p_command,
                                          volatile char * p_destination,
                                          uint32_t destination_size)
{
    size_t command_length = strlen(p_command);

    if ((0U == destination_size) || (length < command_length) ||
        (0 != memcmp(p_payload, p_command, command_length)))
    {
        return false;
    }

    memset((void *) p_destination, 0, destination_size);
    if (length == command_length)
    {
        return true;
    }
    if ((length <= (command_length + 1U)) ||
        ((uint8_t) ':' != p_payload[command_length]))
    {
        return false;
    }

    uint32_t basename_length = (uint32_t) length - (uint32_t) command_length - 1U;
    if ((basename_length > 8U) || (basename_length >= destination_size))
    {
        return false;
    }
    for (uint32_t i = 0U; i < basename_length; i++)
    {
        uint8_t value = p_payload[command_length + 1U + i];
        if (!protocol_record_basename_char_valid(value))
        {
            memset((void *) p_destination, 0, destination_size);
            return false;
        }
        p_destination[i] = (char) value;
    }
    p_destination[basename_length] = '\0';
    return true;
}

static uint32_t protocol_payload_uint_after(uint8_t const * p_payload,
                                            uint16_t length,
                                            char const * p_token,
                                            uint32_t fallback)
{
    size_t token_length = strlen(p_token);

    if ((0U == token_length) || (length < token_length))
    {
        return fallback;
    }

    for (uint32_t i = 0U; i <= ((uint32_t) length - token_length); i++)
    {
        if (0 == memcmp(&p_payload[i], p_token, token_length))
        {
            uint32_t value = 0U;
            uint32_t j = i + (uint32_t) token_length;
            bool found = false;

            while ((j < length) && ((p_payload[j] < (uint8_t) '0') || (p_payload[j] > (uint8_t) '9')))
            {
                j++;
            }

            while ((j < length) && (p_payload[j] >= (uint8_t) '0') && (p_payload[j] <= (uint8_t) '9'))
            {
                value = (value * 10U) + (uint32_t) (p_payload[j] - (uint8_t) '0');
                found = true;
                j++;
            }

            return found ? value : fallback;
        }
    }

    return fallback;
}

static void protocol_write_audio_command(uint32_t command, uint32_t arg)
{
    volatile audio_ipc_mailbox_t * p_aud = AUDIO_IPC_MAILBOX;

    if (AUDIO_IPC_MAGIC != p_aud->magic)
    {
        p_aud->magic = AUDIO_IPC_MAGIC;
        p_aud->volume_percent = 100U;
    }

    p_aud->command_arg = arg;
    p_aud->command = command;
    p_aud->command_sequence++;
}

static ra8_esp32_protocol_status_t protocol_send_safety_event(TickType_t timeout_ticks)
{
    char payload[512];
    uint32_t offset = 0U;
    uint32_t now = (uint32_t) xTaskGetTickCount();

    offset = protocol_append_text(payload, offset, sizeof(payload), "{\"version\":2,\"event\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pending_event_id);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"tick\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), now);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"state\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pending_state);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"incident\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pending_incident_id);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"type\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pending_incident_type);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"action\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pending_action);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"lifecycle\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pending_lifecycle);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"level\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pending_level);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"global_level\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.global_alarm_level);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"confidence\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pending_confidence);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"severity\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pending_severity);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"sources\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pending_source_mask);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"duration_ms\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), now - g_safety.pending_since_tick);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"faults\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.sensor_fault_mask);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"pressure_total\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pressure_total);
    offset = protocol_append_text(payload, offset, sizeof(payload), ",\"pressure_edge\":");
    offset = protocol_append_uint(payload, offset, sizeof(payload), g_safety.pressure_edge);
    offset = protocol_append_char(payload, offset, sizeof(payload), '}');

    if (offset > sizeof(payload))
    {
        return RA8_ESP32_PROTOCOL_STATUS_PAYLOAD_TOO_LONG;
    }

    return protocol_send_frame(RA8_ESP32_MSG_SAFETY_EVENT,
                               (uint8_t const *) payload,
                               (uint16_t) offset,
                               timeout_ticks);
}

void ra8_esp32_protocol_safety_poll(TickType_t timeout_ticks)
{
    uint32_t now = (uint32_t) xTaskGetTickCount();

    safety_evaluate();
    if ((0U != g_safety.pending_event_id) &&
        ((0U == g_safety.last_send_tick) || ((now - g_safety.last_send_tick) >= SAFETY_EVENT_RETRY_TICKS)))
    {
        if (0U != g_safety.last_send_tick)
        {
            g_safety.retry_count++;
        }
        if (RA8_ESP32_PROTOCOL_STATUS_OK == protocol_send_safety_event(timeout_ticks))
        {
            g_safety.last_send_tick = now;
        }
    }
}

static void safety_evaluate(void)
{
    multimodal_fusion_snapshot_t fusion = {0};
    multimodal_incident_event_t incident_event = {0};
    safety_state_t next = SAFETY_STATE_NORMAL;
    uint32_t source = 0U;
    uint32_t severity = 0U;
    uint32_t confidence = 100U;
    bool force_event = false;
    bool fusion_valid = multimodal_fusion_get_snapshot(&fusion);

    if (fusion_valid)
    {
        g_safety.sensor_fault_mask = fusion.sensor_fault_mask;
        g_safety.pressure_total = fusion.pressure_total;
        g_safety.pressure_edge = fusion.pressure_edge;
        g_safety.pressure_x100 = fusion.pressure_x100;
        g_safety.pressure_y100 = fusion.pressure_y100;
        g_safety.global_alarm_level = fusion.global_alarm_level;
    }

    if (g_safety_force_test)
    {
        next = SAFETY_STATE_TEST_ALARM;
        source = SAFETY_SOURCE_SYSTEM;
        severity = 2U;
        force_event = true;
        g_safety_force_test = false;
    }
    else if (fusion_valid)
    {
        if (fusion.state <= (uint32_t) SAFETY_STATE_AWAKE)
        {
            next = (safety_state_t) fusion.state;
        }
        else
        {
            next = SAFETY_STATE_SENSOR_FAULT;
        }

        source = fusion.source_mask;
        severity = fusion.severity;
        confidence = fusion.confidence;
        if ((SAFETY_STATE_SENSOR_FAULT == next) && (0U == source))
        {
            source = SAFETY_SOURCE_SYSTEM;
        }
    }

    if (next != g_safety.state)
    {
        g_safety.state = next;
        g_safety.state_since_tick = (uint32_t) xTaskGetTickCount();
        g_safety.sequence++;
        if ((SAFETY_STATE_NORMAL != next) && force_event)
        {
            g_safety.event_id = 0x80000000U | ((g_safety.event_id + 1U) & 0x7FFFFFFFU);
            g_safety.pending_event_id = g_safety.event_id;
            g_safety.pending_since_tick = g_safety.state_since_tick;
            g_safety.pending_state = (uint32_t) next;
            g_safety.pending_source_mask = source;
            g_safety.pending_severity = severity;
            g_safety.pending_confidence = confidence;
            g_safety.pending_incident_id = 0U;
            g_safety.pending_incident_type = MULTIMODAL_INCIDENT_NONE;
            g_safety.pending_action = MULTIMODAL_INCIDENT_ACTION_OPEN;
            g_safety.pending_lifecycle = MULTIMODAL_INCIDENT_ACTIVE;
            g_safety.pending_level = 1U;
            g_safety.pending_from_fusion = false;
            g_safety.last_send_tick = 0U;
        }
    }
    g_safety.source_mask = source;
    g_safety.severity = severity;
    g_safety.confidence = confidence;

    if ((0U == g_safety.pending_event_id) && multimodal_fusion_peek_event(&incident_event))
    {
        g_safety.event_id = incident_event.event_id;
        g_safety.pending_event_id = incident_event.event_id;
        g_safety.pending_since_tick = incident_event.occurred_tick;
        g_safety.pending_state = (uint32_t) next;
        g_safety.pending_source_mask = incident_event.source_mask;
        g_safety.pending_severity = incident_event.severity;
        g_safety.pending_confidence = incident_event.confidence;
        g_safety.pending_incident_id = incident_event.incident_id;
        g_safety.pending_incident_type = incident_event.type;
        g_safety.pending_action = incident_event.action;
        g_safety.pending_lifecycle = incident_event.lifecycle;
        g_safety.pending_level = incident_event.level;
        g_safety.pending_from_fusion = true;
        g_safety.last_send_tick = 0U;
    }
}

static void protocol_write_mic_command(uint32_t command,
                                       uint8_t const * p_payload,
                                       uint16_t length,
                                       char const * p_record_command)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;

    if (MIC_IPC_MAGIC != p_mic->magic)
    {
        memset((void *) p_mic, 0, sizeof(*p_mic));
        p_mic->magic = MIC_IPC_MAGIC;
        p_mic->sample_rate = MIC_IPC_SAMPLE_RATE;
        p_mic->bits_per_word = MIC_IPC_BITS_PER_WORD;
        p_mic->buffer_samples = MIC_IPC_PCM_WORDS;
        p_mic->record_slot_select = MIC_IPC_RECORD_SLOT_EVEN;
        p_mic->record_sample_shift = 24U;
        p_mic->record_slew_limit = 32767U;
        p_mic->record_gain_percent = 400U;
    }

    if ((NULL != p_record_command) &&
        !protocol_copy_record_basename(p_payload,
                                       length,
                                       p_record_command,
                                       p_mic->record_basename,
                                       sizeof(p_mic->record_basename)))
    {
        return;
    }

    p_mic->command = command;
    __DMB();
    p_mic->command_sequence++;
}

static void protocol_write_camera_command(uint32_t command,
                                          uint8_t const * p_payload,
                                          uint16_t length,
                                          char const * p_record_command)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;

    if (SERIAL_CAMERA_IPC_MAGIC != p_cam->magic)
    {
        return;
    }

    if ((NULL != p_record_command) &&
        !protocol_copy_record_basename(p_payload,
                                       length,
                                       p_record_command,
                                       p_cam->record_basename,
                                       sizeof(p_cam->record_basename)))
    {
        return;
    }

    p_cam->command = command;
    __DMB();
    p_cam->command_sequence++;
}

static void protocol_set_mic_slot(uint32_t slot)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;

    if (MIC_IPC_MAGIC != p_mic->magic)
    {
        memset((void *) p_mic, 0, sizeof(*p_mic));
        p_mic->magic = MIC_IPC_MAGIC;
        p_mic->sample_rate = MIC_IPC_SAMPLE_RATE;
        p_mic->bits_per_word = MIC_IPC_BITS_PER_WORD;
        p_mic->buffer_samples = MIC_IPC_PCM_WORDS;
        p_mic->record_sample_shift = 24U;
        p_mic->record_slew_limit = 32767U;
        p_mic->record_gain_percent = 400U;
    }

    if (slot > MIC_IPC_RECORD_SLOT_AUTO)
    {
        slot = MIC_IPC_RECORD_SLOT_EVEN;
    }

    p_mic->record_slot_select = slot;
}

static void protocol_set_mic_record_params(uint32_t shift, uint32_t slew_limit)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;

    if (MIC_IPC_MAGIC != p_mic->magic)
    {
        memset((void *) p_mic, 0, sizeof(*p_mic));
        p_mic->magic = MIC_IPC_MAGIC;
        p_mic->sample_rate = MIC_IPC_SAMPLE_RATE;
        p_mic->bits_per_word = MIC_IPC_BITS_PER_WORD;
        p_mic->buffer_samples = MIC_IPC_PCM_WORDS;
        p_mic->record_slot_select = MIC_IPC_RECORD_SLOT_EVEN;
        p_mic->record_sample_shift = 24U;
        p_mic->record_slew_limit = 32767U;
        p_mic->record_gain_percent = 400U;
    }

    if ((shift >= 14U) && (shift <= 24U))
    {
        p_mic->record_sample_shift = shift;
    }

    if (slew_limit <= 32767U)
    {
        p_mic->record_slew_limit = slew_limit;
    }
}

static void protocol_set_mic_gain(uint32_t gain_percent)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;

    if (MIC_IPC_MAGIC != p_mic->magic)
    {
        memset((void *) p_mic, 0, sizeof(*p_mic));
        p_mic->magic = MIC_IPC_MAGIC;
        p_mic->sample_rate = MIC_IPC_SAMPLE_RATE;
        p_mic->bits_per_word = MIC_IPC_BITS_PER_WORD;
        p_mic->buffer_samples = MIC_IPC_PCM_WORDS;
        p_mic->record_slot_select = MIC_IPC_RECORD_SLOT_EVEN;
        p_mic->record_sample_shift = 24U;
        p_mic->record_slew_limit = 32767U;
    }

    if ((gain_percent >= 25U) && (gain_percent <= 1600U))
    {
        p_mic->record_gain_percent = gain_percent;
    }
}

static void protocol_execute_silent_reset(uint16_t request_sequence)
{
    uint32_t offset = 0U;

    offset = protocol_append_text(g_protocol_cmd_result_payload,
                                  offset,
                                  sizeof(g_protocol_cmd_result_payload),
                                  "{\"cmd\":\"silent_reset\",\"ok\":true,\"request_sequence\":");
    offset = protocol_append_uint(g_protocol_cmd_result_payload,
                                  offset,
                                  sizeof(g_protocol_cmd_result_payload),
                                  request_sequence);
    offset = protocol_append_text(g_protocol_cmd_result_payload,
                                  offset,
                                  sizeof(g_protocol_cmd_result_payload),
                                  ",\"reset_delay_ms\":");
    offset = protocol_append_uint(g_protocol_cmd_result_payload,
                                  offset,
                                  sizeof(g_protocol_cmd_result_payload),
                                  RA8_SILENT_RESET_DELAY_MS);
    offset = protocol_append_char(g_protocol_cmd_result_payload,
                                  offset,
                                  sizeof(g_protocol_cmd_result_payload),
                                  '}');

    if (offset <= sizeof(g_protocol_cmd_result_payload))
    {
        /*
         * protocol_send_frame waits for UART TX completion.  Reset proceeds
         * even if the acknowledgement cannot be delivered.
         */
        (void) protocol_send_frame(RA8_ESP32_MSG_CMD_RESULT,
                                   (uint8_t const *) g_protocol_cmd_result_payload,
                                   (uint16_t) offset,
                                   pdMS_TO_TICKS(RA8_SILENT_RESET_ACK_TIMEOUT_MS));
    }

    vTaskDelay(pdMS_TO_TICKS(RA8_SILENT_RESET_DELAY_MS));
    __disable_irq();
    __DSB();
    NVIC_SystemReset();

    for (;;)
    {
        __WFI();
    }
}

static void protocol_handle_cmd(uint8_t const * p_payload,
                                uint16_t length,
                                uint16_t request_sequence)
{
    uint32_t command = AUDIO_IPC_COMMAND_NONE;
    uint32_t arg = 0U;
    multimodal_environment_thresholds_t environment_thresholds;

    if (protocol_is_silent_reset_command(p_payload, length))
    {
        protocol_execute_silent_reset(request_sequence);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "gateway_ready"))
    {
        g_protocol_gateway_ready = true;
        g_protocol_gateway_ready_count++;
        g_protocol_gateway_ready_tick = xTaskGetTickCount();
        return;
    }
    else if (protocol_payload_has(p_payload, length, "safety_test"))
    {
        g_safety_force_test = true;
        return;
    }
    else if (protocol_payload_has(p_payload, length, "fusion_calibrate_empty"))
    {
        multimodal_fusion_reset_pressure_baseline();
        return;
    }
    else if (protocol_parse_environment_thresholds_v2(p_payload, length,
                                                      &environment_thresholds) ||
             protocol_parse_environment_thresholds(p_payload, length,
                                                   &environment_thresholds))
    {
        (void) multimodal_fusion_set_environment_thresholds(&environment_thresholds);
        return;
    }
    else if (protocol_payload_equals(p_payload, length, "light_on") ||
             protocol_payload_equals(p_payload, length, "{\"cmd\":\"light_on\"}"))
    {
        (void) warm_light_control_set(true);
        return;
    }
    else if (protocol_payload_equals(p_payload, length, "light_off") ||
             protocol_payload_equals(p_payload, length, "{\"cmd\":\"light_off\"}"))
    {
        (void) warm_light_control_set(false);
        return;
    }
    else if (protocol_payload_equals(p_payload, length, "soother_on") ||
             protocol_payload_equals(p_payload, length, "{\"cmd\":\"soother_on\"}"))
    {
        (void) soother_pat_control_set(true);
        return;
    }
    else if (protocol_payload_equals(p_payload, length, "soother_off") ||
             protocol_payload_equals(p_payload, length, "{\"cmd\":\"soother_off\"}"))
    {
        (void) soother_pat_control_set(false);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_slot_auto"))
    {
        protocol_set_mic_slot(MIC_IPC_RECORD_SLOT_AUTO);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_slot_odd"))
    {
        protocol_set_mic_slot(MIC_IPC_RECORD_SLOT_ODD);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_slot_even"))
    {
        protocol_set_mic_slot(MIC_IPC_RECORD_SLOT_EVEN);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_shift16"))
    {
        protocol_set_mic_record_params(16U, MIC_IPC_MAILBOX->record_slew_limit);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_shift18"))
    {
        protocol_set_mic_record_params(18U, MIC_IPC_MAILBOX->record_slew_limit);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_shift20"))
    {
        protocol_set_mic_record_params(20U, MIC_IPC_MAILBOX->record_slew_limit);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_shift22"))
    {
        protocol_set_mic_record_params(22U, MIC_IPC_MAILBOX->record_slew_limit);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_shift24"))
    {
        protocol_set_mic_record_params(24U, MIC_IPC_MAILBOX->record_slew_limit);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_slew_off"))
    {
        protocol_set_mic_record_params(MIC_IPC_MAILBOX->record_sample_shift, 32767U);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_slew_on"))
    {
        protocol_set_mic_record_params(MIC_IPC_MAILBOX->record_sample_shift, 4096U);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_gain1"))
    {
        protocol_set_mic_gain(100U);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_gain2"))
    {
        protocol_set_mic_gain(200U);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_gain4"))
    {
        protocol_set_mic_gain(400U);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_gain8"))
    {
        protocol_set_mic_gain(800U);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_record_stop"))
    {
        protocol_write_mic_command(MIC_IPC_COMMAND_RECORD_STOP, NULL, 0U, NULL);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_record"))
    {
        protocol_write_mic_command(MIC_IPC_COMMAND_RECORD_START,
                                   p_payload,
                                   length,
                                   "mic_record");
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_stop"))
    {
        protocol_write_mic_command(MIC_IPC_COMMAND_STOP, NULL, 0U, NULL);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "mic_start"))
    {
        protocol_write_mic_command(MIC_IPC_COMMAND_START, NULL, 0U, NULL);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "camera_record_stop"))
    {
        protocol_write_camera_command(SERIAL_CAMERA_COMMAND_RECORD_STOP, NULL, 0U, NULL);
        return;
    }
    else if (protocol_payload_has(p_payload, length, "camera_record"))
    {
        protocol_write_camera_command(SERIAL_CAMERA_COMMAND_RECORD_START,
                                      p_payload,
                                      length,
                                      "camera_record");
        return;
    }
    else if (protocol_payload_has(p_payload, length, "camera_capture"))
    {
        protocol_write_camera_command(SERIAL_CAMERA_COMMAND_CAPTURE_ONCE,
                                      p_payload,
                                      length,
                                      "camera_capture");
        return;
    }

    if (protocol_payload_has(p_payload, length, "play_boot"))
    {
        command = AUDIO_IPC_COMMAND_PLAY_BOOT;
    }
    else if (protocol_payload_has(p_payload, length, "play"))
    {
        command = AUDIO_IPC_COMMAND_PLAY_TRACK;
        arg = protocol_payload_uint_after(p_payload, length, "track", 1U);
    }
    else if (protocol_payload_has(p_payload, length, "pause"))
    {
        command = AUDIO_IPC_COMMAND_PAUSE;
    }
    else if (protocol_payload_has(p_payload, length, "resume"))
    {
        command = AUDIO_IPC_COMMAND_RESUME;
    }
    else if (protocol_payload_has(p_payload, length, "stop"))
    {
        command = AUDIO_IPC_COMMAND_STOP;
    }
    else if (protocol_payload_has(p_payload, length, "next"))
    {
        command = AUDIO_IPC_COMMAND_NEXT;
    }
    else if (protocol_payload_has(p_payload, length, "prev"))
    {
        command = AUDIO_IPC_COMMAND_PREV;
    }
    else if (protocol_payload_has(p_payload, length, "volume"))
    {
        command = AUDIO_IPC_COMMAND_VOLUME;
        arg = protocol_payload_uint_after(p_payload, length, "volume", 100U);
    }
    else if (protocol_payload_has(p_payload, length, "refresh"))
    {
        command = AUDIO_IPC_COMMAND_REFRESH;
    }

    if (AUDIO_IPC_COMMAND_NONE != command)
    {
        protocol_write_audio_command(command, arg);
    }
}

static bool protocol_story_name_valid(uint8_t const * p_name, uint32_t length)
{
    if ((12U != length) || ('A' != p_name[0]) || ('I' != p_name[1]) ||
        ('.' != p_name[8]) || ('W' != p_name[9]) || ('A' != p_name[10]) || ('V' != p_name[11]))
    {
        return false;
    }

    for (uint32_t i = 2U; i < 8U; i++)
    {
        uint8_t value = p_name[i];
        if (!(((value >= (uint8_t) '0') && (value <= (uint8_t) '9')) ||
              ((value >= (uint8_t) 'A') && (value <= (uint8_t) 'F'))))
        {
            return false;
        }
    }

    return true;
}

static ra8_esp32_protocol_status_t protocol_send_story_result(uint8_t operation,
                                                               uint8_t result,
                                                               uint8_t status,
                                                               uint32_t transfer_id,
                                                               uint32_t next_offset,
                                                               uint16_t request_sequence)
{
    uint8_t payload[RA8_STORY_FILE_RESULT_BYTES] = {0};

    payload[0] = RA8_STORY_FILE_PROTOCOL_VERSION;
    payload[1] = operation;
    payload[2] = result;
    payload[3] = status;
    protocol_put_be32(payload, 4U, transfer_id);
    protocol_put_be32(payload, 8U, next_offset);
    protocol_put_be16(payload, 12U, request_sequence);

    return protocol_send_frame(RA8_ESP32_MSG_CMD_RESULT,
                               payload,
                               sizeof(payload),
                               pdMS_TO_TICKS(RA8_STORY_RESULT_TIMEOUT_MS));
}

static void protocol_reject_story_operation(uint8_t operation,
                                             uint32_t transfer_id,
                                             uint16_t request_sequence,
                                             audio_ipc_story_result_t result)
{
    volatile audio_ipc_mailbox_t const * p_aud = AUDIO_IPC_MAILBOX;
    uint8_t status = (AUDIO_IPC_MAGIC == p_aud->magic) ? (uint8_t) p_aud->story_status :
                     (uint8_t) AUDIO_IPC_STORY_STATUS_ERROR;
    uint32_t next_offset = (AUDIO_IPC_MAGIC == p_aud->magic) ? p_aud->story_received_bytes : 0U;

    (void) protocol_send_story_result(operation,
                                      (uint8_t) result,
                                      status,
                                      transfer_id,
                                      next_offset,
                                      request_sequence);
}

static bool protocol_story_mailbox_available(uint8_t operation,
                                             uint32_t transfer_id,
                                             uint16_t request_sequence)
{
    volatile audio_ipc_mailbox_t const * p_aud = AUDIO_IPC_MAILBOX;

    if ((AUDIO_IPC_MAGIC == p_aud->magic) && !g_protocol_story_result_pending &&
        (p_aud->story_operation_sequence == p_aud->story_operation_ack_sequence))
    {
        return true;
    }

    protocol_reject_story_operation(operation,
                                    transfer_id,
                                    request_sequence,
                                    AUDIO_IPC_STORY_RESULT_BUSY);
    return false;
}

static bool protocol_submit_story_operation(uint8_t operation,
                                            uint32_t transfer_id,
                                            uint16_t request_sequence)
{
    volatile audio_ipc_mailbox_t * p_aud = AUDIO_IPC_MAILBOX;
    uint32_t operation_sequence;

    if ((AUDIO_IPC_MAGIC != p_aud->magic) || g_protocol_story_result_pending ||
        (p_aud->story_operation_sequence != p_aud->story_operation_ack_sequence))
    {
        protocol_reject_story_operation(operation,
                                        transfer_id,
                                        request_sequence,
                                        AUDIO_IPC_STORY_RESULT_BUSY);
        return false;
    }

    operation_sequence = p_aud->story_operation_sequence + 1U;
    if (0U == operation_sequence)
    {
        operation_sequence = 1U;
    }

    p_aud->story_operation = operation;
    __DMB();
    p_aud->story_operation_sequence = operation_sequence;

    g_protocol_story_pending_operation_sequence = operation_sequence;
    g_protocol_story_pending_request_sequence = request_sequence;
    g_protocol_story_pending_operation = operation;
    g_protocol_story_pending_transfer_id = transfer_id;
    g_protocol_story_result_pending = true;
    return true;
}

static void protocol_handle_story_begin(uint8_t const * p_payload,
                                        uint16_t length,
                                        uint16_t request_sequence)
{
    volatile audio_ipc_mailbox_t * p_aud = AUDIO_IPC_MAILBOX;
    uint32_t transfer_id = (length >= 8U) ? protocol_get_be32(p_payload, 4U) : 0U;
    uint32_t total_bytes;
    uint32_t expected_crc;
    uint32_t name_length;

    if ((length < RA8_STORY_FILE_BEGIN_FIXED_BYTES) ||
        (RA8_STORY_FILE_PROTOCOL_VERSION != p_payload[0]) ||
        (0U != (p_payload[1] & (uint8_t) ~RA8_STORY_FILE_FLAG_PLAY_AFTER)) ||
        (0U != p_payload[2]) || (0U != p_payload[3]))
    {
        protocol_reject_story_operation(AUDIO_IPC_STORY_OPERATION_BEGIN,
                                        transfer_id,
                                        request_sequence,
                                        AUDIO_IPC_STORY_RESULT_BAD_REQUEST);
        return;
    }

    total_bytes = protocol_get_be32(p_payload, 8U);
    expected_crc = protocol_get_be32(p_payload, 12U);
    name_length = p_payload[16];
    if ((0U == transfer_id) || (0U == total_bytes) ||
        (total_bytes > AUDIO_IPC_STORY_MAX_BYTES) ||
        (length != (RA8_STORY_FILE_BEGIN_FIXED_BYTES + name_length)) ||
        !protocol_story_name_valid(&p_payload[RA8_STORY_FILE_BEGIN_FIXED_BYTES], name_length))
    {
        protocol_reject_story_operation(AUDIO_IPC_STORY_OPERATION_BEGIN,
                                        transfer_id,
                                        request_sequence,
                                        AUDIO_IPC_STORY_RESULT_BAD_REQUEST);
        return;
    }
    if (!protocol_story_mailbox_available(AUDIO_IPC_STORY_OPERATION_BEGIN,
                                          transfer_id,
                                          request_sequence))
    {
        return;
    }

    p_aud->story_transfer_id = transfer_id;
    p_aud->story_total_bytes = total_bytes;
    p_aud->story_expected_crc32 = expected_crc;
    p_aud->story_received_bytes = 0U;
    p_aud->story_play_after = (0U != (p_payload[1] & RA8_STORY_FILE_FLAG_PLAY_AFTER)) ? 1U : 0U;
    memset((void *) p_aud->story_name, 0, sizeof(p_aud->story_name));
    for (uint32_t i = 0U; i < name_length; i++)
    {
        p_aud->story_name[i] = (char) p_payload[RA8_STORY_FILE_BEGIN_FIXED_BYTES + i];
    }
    (void) protocol_submit_story_operation(AUDIO_IPC_STORY_OPERATION_BEGIN,
                                           transfer_id,
                                           request_sequence);
}

static void protocol_handle_story_chunk(uint8_t const * p_payload,
                                        uint16_t length,
                                        uint16_t request_sequence)
{
    volatile audio_ipc_mailbox_t * p_aud = AUDIO_IPC_MAILBOX;
    uint32_t transfer_id = (length >= 8U) ? protocol_get_be32(p_payload, 4U) : 0U;
    uint32_t offset;
    uint32_t chunk_length;
    uint32_t chunk_crc;

    if ((length < RA8_STORY_FILE_CHUNK_FIXED_BYTES) ||
        (RA8_STORY_FILE_PROTOCOL_VERSION != p_payload[0]) ||
        (0U != p_payload[1]) || (0U != p_payload[2]) || (0U != p_payload[3]))
    {
        protocol_reject_story_operation(AUDIO_IPC_STORY_OPERATION_CHUNK,
                                        transfer_id,
                                        request_sequence,
                                        AUDIO_IPC_STORY_RESULT_BAD_REQUEST);
        return;
    }

    offset = protocol_get_be32(p_payload, 8U);
    chunk_length = protocol_get_be16(p_payload, 12U);
    chunk_crc = protocol_get_be32(p_payload, 16U);
    if ((0U != protocol_get_be16(p_payload, 14U)) ||
        (0U == transfer_id) || (0U == chunk_length) ||
        (chunk_length > AUDIO_IPC_STORY_CHUNK_BYTES) ||
        (length != (RA8_STORY_FILE_CHUNK_FIXED_BYTES + chunk_length)) ||
        (AUDIO_IPC_MAGIC != p_aud->magic) ||
        (transfer_id != p_aud->story_transfer_id))
    {
        protocol_reject_story_operation(AUDIO_IPC_STORY_OPERATION_CHUNK,
                                        transfer_id,
                                        request_sequence,
                                        AUDIO_IPC_STORY_RESULT_BAD_REQUEST);
        return;
    }
    if (!protocol_story_mailbox_available(AUDIO_IPC_STORY_OPERATION_CHUNK,
                                          transfer_id,
                                          request_sequence))
    {
        return;
    }

    p_aud->story_chunk_offset = offset;
    p_aud->story_chunk_length = chunk_length;
    p_aud->story_chunk_crc32 = chunk_crc;
    for (uint32_t i = 0U; i < chunk_length; i++)
    {
        p_aud->story_chunk[i] = p_payload[RA8_STORY_FILE_CHUNK_FIXED_BYTES + i];
    }
    (void) protocol_submit_story_operation(AUDIO_IPC_STORY_OPERATION_CHUNK,
                                           transfer_id,
                                           request_sequence);
}

static void protocol_handle_story_end(uint8_t const * p_payload,
                                      uint16_t length,
                                      uint16_t request_sequence)
{
    volatile audio_ipc_mailbox_t * p_aud = AUDIO_IPC_MAILBOX;
    uint32_t transfer_id = (length >= 8U) ? protocol_get_be32(p_payload, 4U) : 0U;
    uint32_t total_bytes;
    uint32_t expected_crc;

    if ((RA8_STORY_FILE_END_BYTES != length) ||
        (RA8_STORY_FILE_PROTOCOL_VERSION != p_payload[0]) ||
        (0U != (p_payload[1] & (uint8_t) ~RA8_STORY_FILE_FLAG_PLAY_AFTER)) ||
        (0U != p_payload[2]) || (0U != p_payload[3]))
    {
        protocol_reject_story_operation(AUDIO_IPC_STORY_OPERATION_END,
                                        transfer_id,
                                        request_sequence,
                                        AUDIO_IPC_STORY_RESULT_BAD_REQUEST);
        return;
    }

    total_bytes = protocol_get_be32(p_payload, 8U);
    expected_crc = protocol_get_be32(p_payload, 12U);
    if ((0U == transfer_id) || (0U == total_bytes) ||
        (total_bytes > AUDIO_IPC_STORY_MAX_BYTES) ||
        (AUDIO_IPC_MAGIC != p_aud->magic) ||
        (transfer_id != p_aud->story_transfer_id) ||
        (total_bytes != p_aud->story_total_bytes) ||
        (expected_crc != p_aud->story_expected_crc32))
    {
        protocol_reject_story_operation(AUDIO_IPC_STORY_OPERATION_END,
                                        transfer_id,
                                        request_sequence,
                                        AUDIO_IPC_STORY_RESULT_BAD_REQUEST);
        return;
    }
    if (!protocol_story_mailbox_available(AUDIO_IPC_STORY_OPERATION_END,
                                          transfer_id,
                                          request_sequence))
    {
        return;
    }

    p_aud->story_play_after = (0U != (p_payload[1] & RA8_STORY_FILE_FLAG_PLAY_AFTER)) ? 1U :
                              p_aud->story_play_after;
    p_aud->story_chunk_crc32 = expected_crc;
    (void) protocol_submit_story_operation(AUDIO_IPC_STORY_OPERATION_END,
                                           transfer_id,
                                           request_sequence);
}

static void protocol_story_result_poll(void)
{
    volatile audio_ipc_mailbox_t const * p_aud = AUDIO_IPC_MAILBOX;

    if (!g_protocol_story_result_pending || (AUDIO_IPC_MAGIC != p_aud->magic))
    {
        return;
    }

    __DMB();
    if (p_aud->story_operation_ack_sequence != g_protocol_story_pending_operation_sequence)
    {
        return;
    }

    if (RA8_ESP32_PROTOCOL_STATUS_OK ==
        protocol_send_story_result(g_protocol_story_pending_operation,
                                   (uint8_t) p_aud->story_result,
                                   (uint8_t) p_aud->story_status,
                                   g_protocol_story_pending_transfer_id,
                                   p_aud->story_received_bytes,
                                   g_protocol_story_pending_request_sequence))
    {
        g_protocol_story_result_pending = false;
    }
}

bool ra8_esp32_protocol_gateway_ready(void)
{
    if (!g_protocol_gateway_ready)
    {
        return false;
    }

    return (xTaskGetTickCount() - g_protocol_gateway_ready_tick) <=
           pdMS_TO_TICKS(RA8_ESP32_GATEWAY_READY_TIMEOUT_MS);
}

bool ra8_esp32_protocol_story_active(void)
{
    volatile audio_ipc_mailbox_t const * p_aud = AUDIO_IPC_MAILBOX;

    if (g_protocol_story_result_pending)
    {
        return true;
    }
    if ((AUDIO_IPC_MAGIC != p_aud->magic) ||
        (AUDIO_IPC_STORY_STATUS_RECEIVING != p_aud->story_status) ||
        (0U == p_aud->story_last_activity_tick))
    {
        return false;
    }

    return (xTaskGetTickCount() - (TickType_t) p_aud->story_last_activity_tick) <=
           pdMS_TO_TICKS(RA8_STORY_ACTIVITY_TIMEOUT_MS);
}

bool ra8_esp32_protocol_story_result_pending(void)
{
    return g_protocol_story_result_pending;
}

void ra8_esp32_protocol_reset_rx(void)
{
    g_protocol_rx_length = 0U;
}

static void protocol_consume_frame(uint8_t const * p_frame, uint32_t frame_length)
{
    uint16_t payload_length = (uint16_t) (((uint16_t) p_frame[6] << 8) | p_frame[7]);
    uint16_t crc = (uint16_t) (((uint16_t) p_frame[frame_length - 2U] << 8) | p_frame[frame_length - 1U]);

    if (crc != ra8_esp32_protocol_crc16_ccitt(p_frame, frame_length - RA8_ESP32_PROTOCOL_CRC_LENGTH))
    {
        g_protocol_rx_crc_error_count++;
        return;
    }

    if (RA8_ESP32_PROTOCOL_VERSION != p_frame[2])
    {
        g_protocol_rx_length_error_count++;
        return;
    }

    g_protocol_rx_valid_frame_count++;
    g_protocol_rx_last_valid_type = p_frame[3];
    g_protocol_rx_last_valid_payload_length = payload_length;

    if (RA8_ESP32_MSG_CMD == p_frame[3])
    {
        uint16_t request_sequence =
            (uint16_t) (((uint16_t) p_frame[4] << 8) | p_frame[5]);
        protocol_handle_cmd(&p_frame[RA8_ESP32_PROTOCOL_HEADER_LENGTH],
                            payload_length,
                            request_sequence);
    }
    else if (RA8_ESP32_MSG_FILE_BEGIN == p_frame[3])
    {
        protocol_handle_story_begin(&p_frame[RA8_ESP32_PROTOCOL_HEADER_LENGTH],
                                    payload_length,
                                    protocol_get_be16(p_frame, 4U));
    }
    else if (RA8_ESP32_MSG_FILE_CHUNK == p_frame[3])
    {
        protocol_handle_story_chunk(&p_frame[RA8_ESP32_PROTOCOL_HEADER_LENGTH],
                                    payload_length,
                                    protocol_get_be16(p_frame, 4U));
    }
    else if (RA8_ESP32_MSG_FILE_END == p_frame[3])
    {
        protocol_handle_story_end(&p_frame[RA8_ESP32_PROTOCOL_HEADER_LENGTH],
                                  payload_length,
                                  protocol_get_be16(p_frame, 4U));
    }
    else if ((RA8_ESP32_MSG_SAFETY_ACK == p_frame[3]) && (payload_length >= 4U))
    {
        uint8_t const * p = &p_frame[RA8_ESP32_PROTOCOL_HEADER_LENGTH];
        uint32_t event_id = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
                            ((uint32_t) p[2] << 8) | (uint32_t) p[3];
        if ((0U != event_id) && (event_id == g_safety.pending_event_id))
        {
            if (g_safety.pending_from_fusion)
            {
                (void) multimodal_fusion_ack_event(event_id);
            }
            g_safety.pending_event_id = 0U;
            g_safety.pending_incident_id = 0U;
            g_safety.pending_incident_type = MULTIMODAL_INCIDENT_NONE;
            g_safety.pending_action = MULTIMODAL_INCIDENT_ACTION_NONE;
            g_safety.pending_lifecycle = MULTIMODAL_INCIDENT_CLEAR;
            g_safety.pending_level = 0U;
            g_safety.pending_from_fusion = false;
            g_safety.last_send_tick = 0U;
            g_safety.ack_count++;
        }
    }
}

void ra8_esp32_protocol_poll_rx(void)
{
    uint8_t data[64];
    uint32_t read_bytes;
    uint32_t processed_bytes = 0U;

    while ((processed_bytes < RA8_ESP32_RX_POLL_BUDGET_BYTES) &&
           (0U != (read_bytes = esp32_uart_driver_read(data, sizeof(data)))))
    {
        processed_bytes += read_bytes;
        for (uint32_t i = 0U; i < read_bytes; i++)
        {
            if ((0U == g_protocol_rx_length) && (RA8_ESP32_PROTOCOL_MAGIC_MSB != data[i]))
            {
                continue;
            }

            if ((1U == g_protocol_rx_length) && (RA8_ESP32_PROTOCOL_MAGIC_LSB != data[i]))
            {
                g_protocol_rx_length = (RA8_ESP32_PROTOCOL_MAGIC_MSB == data[i]) ? 1U : 0U;
                continue;
            }

            if (g_protocol_rx_length >= sizeof(g_protocol_rx_buffer))
            {
                g_protocol_rx_length = 0U;
                continue;
            }

            g_protocol_rx_buffer[g_protocol_rx_length++] = data[i];

            if (g_protocol_rx_length >= RA8_ESP32_PROTOCOL_HEADER_LENGTH)
            {
                uint16_t payload_length = (uint16_t) (((uint16_t) g_protocol_rx_buffer[6] << 8) |
                                                      g_protocol_rx_buffer[7]);
                uint32_t frame_length = RA8_ESP32_PROTOCOL_HEADER_LENGTH + payload_length +
                                        RA8_ESP32_PROTOCOL_CRC_LENGTH;

                if (payload_length > RA8_ESP32_PROTOCOL_MAX_PAYLOAD)
                {
                    g_protocol_rx_length_error_count++;
                    g_protocol_rx_length = 0U;
                    continue;
                }

                if (g_protocol_rx_length >= frame_length)
                {
                    protocol_consume_frame(g_protocol_rx_buffer, frame_length);
                    g_protocol_rx_length = 0U;
                }
            }
        }
    }

    if (processed_bytes >= RA8_ESP32_RX_POLL_BUDGET_BYTES)
    {
        g_protocol_rx_budget_hit_count++;
    }

    protocol_story_result_poll();
}

ra8_esp32_protocol_status_t ra8_esp32_protocol_send_thermal_matrix(TickType_t timeout_ticks)
{
    volatile thermal_ipc_mailbox_t * p_thm = THERMAL_IPC_MAILBOX;
    uint32_t offset = 0U;
    uint32_t sequence = 0U;
    uint8_t * p_payload = g_protocol_thermal_payload;

    if ((THERMAL_IPC_MAGIC != p_thm->magic) ||
        ((uint32_t) THERMAL_IPC_STATUS_FRAME_OK != p_thm->status) ||
        ((uint32_t) THERMAL_FRAME_QUALITY_NORMAL != p_thm->frame_quality))
    {
        return RA8_ESP32_PROTOCOL_STATUS_ERROR;
    }

    for (uint32_t attempt = 0U; attempt < 3U; attempt++)
    {
        uint32_t generation_before = p_thm->write_generation;
        uint32_t sequence_before;

        if (0U != (generation_before & 1U))
        {
            p_thm->generation_retry_count++;
            continue;
        }

        __DMB();
        sequence_before = p_thm->sequence;
        for (uint32_t i = 0U; i < THERMAL_IPC_POINT_COUNT; i++)
        {
            g_protocol_thermal_snapshot[i] = THERMAL_IPC_POINTS[i];
        }
        __DMB();
        uint32_t generation_after = p_thm->write_generation;
        if ((sequence_before != 0U) &&
            (generation_before == generation_after) &&
            (0U == (generation_after & 1U)) &&
            (sequence_before == p_thm->sequence) &&
            (sequence_before == p_thm->last_good_sequence) &&
            ((uint32_t) THERMAL_IPC_STATUS_FRAME_OK == p_thm->status) &&
            ((uint32_t) THERMAL_FRAME_QUALITY_NORMAL == p_thm->frame_quality))
        {
            sequence = sequence_before;
            break;
        }

        p_thm->generation_retry_count++;
    }

    if (0U == sequence)
    {
        return RA8_ESP32_PROTOCOL_STATUS_ERROR;
    }

    while (offset < THERMAL_IPC_POINT_COUNT)
    {
        uint32_t remaining = THERMAL_IPC_POINT_COUNT - offset;
        uint32_t count = (remaining > RA8_ESP32_THERMAL_CHUNK_SAMPLES) ?
                         RA8_ESP32_THERMAL_CHUNK_SAMPLES : remaining;
        uint32_t payload_index = 0U;
        ra8_esp32_protocol_status_t status;

        p_payload[payload_index++] = (uint8_t) (sequence >> 24);
        p_payload[payload_index++] = (uint8_t) (sequence >> 16);
        p_payload[payload_index++] = (uint8_t) (sequence >> 8);
        p_payload[payload_index++] = (uint8_t) sequence;
        protocol_put_be16(p_payload, payload_index, RA8_ESP32_THERMAL_WIDTH);
        payload_index += 2U;
        protocol_put_be16(p_payload, payload_index, RA8_ESP32_THERMAL_HEIGHT);
        payload_index += 2U;
        protocol_put_be16(p_payload, payload_index, (uint16_t) offset);
        payload_index += 2U;
        protocol_put_be16(p_payload, payload_index, (uint16_t) count);
        payload_index += 2U;

        for (uint32_t i = 0U; i < count; i++)
        {
            int16_t sample = g_protocol_thermal_snapshot[offset + i];
            p_payload[payload_index++] = (uint8_t) (((uint16_t) sample) >> 8);
            p_payload[payload_index++] = (uint8_t) sample;
        }

        status = protocol_send_frame(RA8_ESP32_MSG_ALARM_UPLOAD,
                                     p_payload,
                                     (uint16_t) payload_index,
                                     timeout_ticks);
        if (RA8_ESP32_PROTOCOL_STATUS_OK != status)
        {
            return status;
        }

        offset += count;
    }

    return RA8_ESP32_PROTOCOL_STATUS_OK;
}

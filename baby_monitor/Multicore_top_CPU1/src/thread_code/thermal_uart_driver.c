#include "thread_code/thermal_uart_driver.h"

#include <string.h>

#include "bsp_api.h"
#include "task.h"

#define THERMAL_UART_RX_BUFFER_SIZE (16384U)
#define THERMAL_STREAM_CAPTURE_FRAMES (2U)
#define THERMAL_STREAM_CAPTURE_BYTES  (THERMAL_UART_FRAME_LENGTH * THERMAL_STREAM_CAPTURE_FRAMES)
#define THERMAL_FRAME_HEADER        (0x5AU)
#define THERMAL_REQUEST_LENGTH      (3U)
#define THERMAL_OUTPUT_OFF_COMMAND  (0x00U)
#define THERMAL_AUTO_FULL_COMMAND   (0x01U)
#define THERMAL_REQUEST_FRAME_COMMAND (0x13U)
#define THERMAL_STREAM_DRAIN_MS     (300U)
#define THERMAL_RECOVERY_DRAIN_MS   (400U)
#define THERMAL_RECOVERY_FAILURE_THRESHOLD (3U)
#define THERMAL_AUTO_FALLBACK_FAILURE_THRESHOLD (2U)
#define THERMAL_REOPEN_BEFORE_POWER_CYCLE (2U)
#define THERMAL_UART_REOPEN_DELAY_MS (20U)
#define THERMAL_REINIT_RETRY_MS      (1000U)
#define THERMAL_POWER_RELAY_ACTIVE_HIGH (1U)
#define THERMAL_POWER_RELAY_PIN      BSP_IO_PORT_07_PIN_05 /* P705 / P6-21 */
#if THERMAL_POWER_RELAY_ACTIVE_HIGH
#define THERMAL_RELAY_INACTIVE_LEVEL BSP_IO_LEVEL_LOW
#define THERMAL_RELAY_INACTIVE_CFG   ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                      (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW | \
                                      (uint32_t) IOPORT_CFG_DRIVE_MID)
#else
#define THERMAL_RELAY_INACTIVE_LEVEL BSP_IO_LEVEL_HIGH
#define THERMAL_RELAY_INACTIVE_CFG   ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                      (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH | \
                                      (uint32_t) IOPORT_CFG_DRIVE_MID)
#endif
#define THERMAL_IDENTICAL_FRAME_LIMIT (30U)
#define THERMAL_FNV1A_OFFSET          (2166136261UL)
#define THERMAL_FNV1A_PRIME           (16777619UL)
#define THERMAL_DATA_OFFSET         (4U)
#define THERMAL_HEADER_LENGTH       (3U)
#define THERMAL_SENSOR_TEMP_OFFSET  (10244U)
#define THERMAL_CRC_OFFSET          (10246U)
#define THERMAL_CENTER_INDEX        (((64U / 2U) * 80U) + (80U / 2U))
#define THERMAL_RAW_MIN_VALID       (1732U) /* -100.0 C */
#define THERMAL_RAW_MAX_VALID       (5732U) /*  300.0 C */
#define THERMAL_RAW_VALID_RANGE     (THERMAL_RAW_MAX_VALID - THERMAL_RAW_MIN_VALID + 1U)
/* Legacy *_cdeg fields contain 0.1 C units. */
#define THERMAL_TRANSIENT_MEAN_DELTA_CDEG       (50U) /* 5.0 C */
#define THERMAL_TRANSIENT_PIXEL_DELTA_CDEG      (50U) /* 5.0 C */
#define THERMAL_TRANSIENT_CHANGED_PERCENT       (20U)
#define THERMAL_TRANSIENT_CONFIRM_MEAN_CDEG     (20U) /* 2.0 C */
#define THERMAL_UART_BAUD_RATE      (460800U)
#define THERMAL_UART_BAUD_MAX_ERR_X1000 (5000U)
#define THERMAL_UART_DRIVER_BUILD   (0x20260826U)

typedef enum e_thermal_sync_state
{
    THERMAL_SYNC_WAIT_HEADER = 0,
    THERMAL_SYNC_WAIT_SIZE_HIGH,
    THERMAL_SYNC_WAIT_SIZE_LOW
} thermal_sync_state_t;

typedef struct st_thermal_uart_context
{
    volatile bool initialized;
    volatile bool tx_done;
    volatile bool rx_done;
    volatile bool rx_error;
    volatile bool rx_active;
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
    volatile uint32_t current_baud;
    volatile uint32_t rx_expected_bytes;
    bool dtc_enabled;
    bool irq_rx_fallback;
    bool stream_enabled;
    uint32_t consecutive_failures;
    uint32_t recovery_cycles_since_valid;
    uint32_t recovery_count;
    uint32_t reopen_count;
    uint32_t power_cycle_count;
    uint32_t recovery_error_count;
    uint32_t last_recovery_reason;
    uint32_t last_frame_signature;
    uint32_t identical_frame_streak;
    uint32_t stale_frame_count;
    TickType_t last_good_tick;
    TickType_t last_power_cycle_tick;
    TickType_t last_recovery_attempt_tick;
    bool frame_signature_valid;
    bool power_relay_ready;
    uint8_t rx_buffer[THERMAL_UART_RX_BUFFER_SIZE];
    thermal_uart_stats_t stats;
} thermal_uart_context_t;

static thermal_uart_context_t g_thermal_uart_context;
static uart_cfg_t g_thermal_uart_runtime_cfg;
static sci_b_uart_extended_cfg_t g_thermal_uart_runtime_extend;
static uint8_t g_thermal_frame[THERMAL_UART_FRAME_LENGTH];
static uint8_t g_thermal_stream_capture[THERMAL_STREAM_CAPTURE_BYTES];
static uint16_t g_thermal_histogram[THERMAL_RAW_VALID_RANGE];
static bool g_thermal_has_last_good;
static bool g_thermal_transient_pending;
static int32_t g_thermal_last_good_mean_cdeg;
static int32_t g_thermal_transient_mean_cdeg;

static void thermal_publish_status(thermal_ipc_status_t status, uint32_t rx_bytes);
static bool thermal_uart_request_frame_dtc(TickType_t timeout_ticks);
static uint32_t thermal_uart_stop_dtc_read(void);
static bool thermal_uart_recover(bool power_cycle, thermal_recovery_reason_t reason);

static inline uint16_t thermal_uart_next_index(uint16_t index)
{
    return (uint16_t) ((index + 1U) % THERMAL_UART_RX_BUFFER_SIZE);
}

static void thermal_uart_push_byte(uint8_t byte)
{
    uint16_t next_head = thermal_uart_next_index(g_thermal_uart_context.rx_head);

    if (next_head == g_thermal_uart_context.rx_tail)
    {
        g_thermal_uart_context.stats.rx_overflow_count++;
        return;
    }

    g_thermal_uart_context.rx_buffer[g_thermal_uart_context.rx_head] = byte;
    g_thermal_uart_context.rx_head = next_head;
    g_thermal_uart_context.stats.rx_bytes++;
}

static void thermal_uart_flush_rx(void)
{
    taskENTER_CRITICAL();
    g_thermal_uart_context.rx_tail = g_thermal_uart_context.rx_head;
    taskEXIT_CRITICAL();
}

static fsp_err_t thermal_uart_apply_baud(uint32_t baud_rate)
{
    sci_b_baud_setting_t baud_setting;

    if (g_thermal_uart_context.current_baud == baud_rate)
    {
        return FSP_SUCCESS;
    }

    fsp_err_t err = R_SCI_B_UART_BaudCalculate(baud_rate,
                                               true,
                                               THERMAL_UART_BAUD_MAX_ERR_X1000,
                                               &baud_setting);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = g_thermal_uart.p_api->baudSet(g_thermal_uart.p_ctrl, &baud_setting);
    if (FSP_SUCCESS == err)
    {
        g_thermal_uart_context.current_baud = baud_rate;
        g_thermal_uart_context.stats.baud_set_count++;
    }

    return err;
}

static bool thermal_uart_write_command(const uint8_t * p_command, uint32_t length, TickType_t timeout_ticks)
{
    FSP_PARAMETER_NOT_USED(timeout_ticks);

    if ((NULL == p_command) || (0U == length))
    {
        return false;
    }

    g_thermal_uart_context.tx_done = false;

    if (FSP_SUCCESS != g_thermal_uart.p_api->write(g_thermal_uart.p_ctrl, p_command, length))
    {
        g_thermal_uart_context.tx_done = true;
        thermal_publish_status(THERMAL_IPC_STATUS_UART_ERROR, 0U);
        return false;
    }

    /*
     * The thermal camera command is only three bytes. Waiting for TEI and then
     * aborting on timeout can corrupt the next short command if the TX complete
     * callback is delayed or masked. Give the hardware enough time to shift the
     * command out and let the callback update diagnostics when it arrives.
     */
    vTaskDelay(pdMS_TO_TICKS(2));
    g_thermal_uart_context.tx_done = true;

    return true;
}

static bool thermal_uart_try_pop_byte(uint8_t * p_byte)
{
    bool has_byte = false;

    if (NULL == p_byte)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (g_thermal_uart_context.rx_tail != g_thermal_uart_context.rx_head)
    {
        *p_byte = g_thermal_uart_context.rx_buffer[g_thermal_uart_context.rx_tail];
        g_thermal_uart_context.rx_tail = thermal_uart_next_index(g_thermal_uart_context.rx_tail);
        has_byte = true;
    }
    taskEXIT_CRITICAL();

    return has_byte;
}

static bool thermal_uart_pop_byte(uint8_t * p_byte, TickType_t timeout_ticks)
{
    TickType_t start_ticks = xTaskGetTickCount();

    while (!thermal_uart_try_pop_byte(p_byte))
    {
        if ((timeout_ticks > 0U) && ((xTaskGetTickCount() - start_ticks) >= timeout_ticks))
        {
            return false;
        }

        vTaskDelay(1);
    }

    return true;
}

static bool thermal_uart_find_frame_header(TickType_t timeout_ticks)
{
    TickType_t start_ticks = xTaskGetTickCount();
    thermal_sync_state_t state = THERMAL_SYNC_WAIT_HEADER;
    uint8_t byte = 0U;

    while ((timeout_ticks == 0U) || ((xTaskGetTickCount() - start_ticks) < timeout_ticks))
    {
        TickType_t elapsed_ticks = xTaskGetTickCount() - start_ticks;
        TickType_t remaining_ticks = (timeout_ticks > elapsed_ticks) ? (timeout_ticks - elapsed_ticks) : 1U;

        if (!thermal_uart_pop_byte(&byte, remaining_ticks))
        {
            break;
        }

        switch (state)
        {
            case THERMAL_SYNC_WAIT_HEADER:
            {
                if (THERMAL_FRAME_HEADER == byte)
                {
                    g_thermal_frame[0] = byte;
                    state = THERMAL_SYNC_WAIT_SIZE_HIGH;
                }
                break;
            }

            case THERMAL_SYNC_WAIT_SIZE_HIGH:
            {
                if (0x08U == byte)
                {
                    g_thermal_frame[1] = byte;
                    state = THERMAL_SYNC_WAIT_SIZE_LOW;
                }
                else
                {
                    state = (THERMAL_FRAME_HEADER == byte) ? THERMAL_SYNC_WAIT_SIZE_HIGH : THERMAL_SYNC_WAIT_HEADER;
                    g_thermal_frame[0] = (THERMAL_FRAME_HEADER == byte) ? byte : 0U;
                }
                break;
            }

            case THERMAL_SYNC_WAIT_SIZE_LOW:
            {
                if (0x46U == byte)
                {
                    g_thermal_frame[2] = byte;
                    return true;
                }

                state = (THERMAL_FRAME_HEADER == byte) ? THERMAL_SYNC_WAIT_SIZE_HIGH : THERMAL_SYNC_WAIT_HEADER;
                g_thermal_frame[0] = (THERMAL_FRAME_HEADER == byte) ? byte : 0U;
                break;
            }

            default:
            {
                state = THERMAL_SYNC_WAIT_HEADER;
                break;
            }
        }
    }

    return false;
}

static int16_t thermal_raw_to_cdeg(uint16_t raw)
{
    return (int16_t) (((int32_t) raw) - 2732);
}

static bool thermal_raw_is_valid(uint16_t raw)
{
    return ((raw >= THERMAL_RAW_MIN_VALID) && (raw <= THERMAL_RAW_MAX_VALID));
}

static uint16_t thermal_crc16_modbus(const uint8_t * p_data, uint32_t length)
{
    uint16_t crc = 0xFFFFU;

    for (uint32_t i = 0U; i < length; i++)
    {
        crc ^= p_data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            if (0U != (crc & 0x0001U))
            {
                crc = (uint16_t) ((crc >> 1) ^ 0xA001U);
            }
            else
            {
                crc = (uint16_t) (crc >> 1);
            }
        }
    }

    return crc;
}

static uint32_t thermal_frame_signature(const uint8_t * p_data, uint32_t length)
{
    uint32_t signature = THERMAL_FNV1A_OFFSET;

    for (uint32_t i = 0U; i < length; i++)
    {
        signature ^= p_data[i];
        signature *= THERMAL_FNV1A_PRIME;
    }

    return signature;
}

static uint16_t thermal_frame_u16_le(uint32_t offset)
{
    return (uint16_t) (((uint16_t) g_thermal_frame[offset + 1U] << 8) |
                       g_thermal_frame[offset]);
}

static uint32_t thermal_abs_i32(int32_t value)
{
    return (uint32_t) ((value < 0) ? -value : value);
}

static int16_t thermal_histogram_percentile(uint32_t rank)
{
    uint32_t cumulative = 0U;

    for (uint32_t i = 0U; i < THERMAL_RAW_VALID_RANGE; i++)
    {
        cumulative += g_thermal_histogram[i];
        if (cumulative > rank)
        {
            return thermal_raw_to_cdeg((uint16_t) (THERMAL_RAW_MIN_VALID + i));
        }
    }

    return thermal_raw_to_cdeg(THERMAL_RAW_MAX_VALID);
}

static uint32_t thermal_matrix_write_begin(volatile thermal_ipc_mailbox_t * p_mailbox)
{
    uint32_t generation = p_mailbox->write_generation;

    if (0U != (generation & 1U))
    {
        generation++;
    }
    generation++;
    p_mailbox->write_generation = generation;
    __DMB();
    return generation;
}

static void thermal_matrix_write_end(volatile thermal_ipc_mailbox_t * p_mailbox,
                                     uint32_t odd_generation)
{
    __DMB();
    p_mailbox->write_generation = odd_generation + 1U;
    __DMB();
}

static void thermal_publish_recovery_diagnostics(volatile thermal_ipc_mailbox_t * p_mailbox)
{
    p_mailbox->consecutive_failures = g_thermal_uart_context.consecutive_failures;
    p_mailbox->recovery_count = g_thermal_uart_context.recovery_count;
    p_mailbox->reopen_count = g_thermal_uart_context.reopen_count;
    /* Legacy names remain populated for old CPU0/web builds. */
    p_mailbox->hard_reset_count = g_thermal_uart_context.power_cycle_count;
    p_mailbox->recovery_error_count = g_thermal_uart_context.recovery_error_count;
    p_mailbox->last_recovery_reason = g_thermal_uart_context.last_recovery_reason;
    p_mailbox->last_good_tick = (uint32_t) g_thermal_uart_context.last_good_tick;
    p_mailbox->run_pin_ready = g_thermal_uart_context.power_relay_ready ? 1U : 0U;
    p_mailbox->power_cycle_count = g_thermal_uart_context.power_cycle_count;
    p_mailbox->power_relay_ready = g_thermal_uart_context.power_relay_ready ? 1U : 0U;
    p_mailbox->stale_frame_count = g_thermal_uart_context.stale_frame_count;
    p_mailbox->identical_frame_streak = g_thermal_uart_context.identical_frame_streak;
}

static void thermal_publish_status(thermal_ipc_status_t status, uint32_t rx_bytes)
{
    volatile thermal_ipc_mailbox_t * p_mailbox = THERMAL_IPC_MAILBOX;

    p_mailbox->magic = THERMAL_IPC_MAGIC;
    p_mailbox->status = (uint32_t) status;
    p_mailbox->rx_bytes = rx_bytes;
    if (0U == p_mailbox->last_good_sequence)
    {
        p_mailbox->min_cdeg = 0;
        p_mailbox->max_cdeg = 0;
        p_mailbox->center_cdeg = 0;
        p_mailbox->first_cdeg = 0;
        p_mailbox->avg_cdeg = 0;
        p_mailbox->p01_cdeg = 0;
        p_mailbox->p99_cdeg = 0;
        p_mailbox->mean_cdeg = 0;
    }
    p_mailbox->frame_quality = (uint32_t) THERMAL_FRAME_QUALITY_INVALID;
    p_mailbox->mean_abs_diff_cdeg = 0U;
    p_mailbox->changed_pixel_count = 0U;
    p_mailbox->uart_errors = g_thermal_uart_context.stats.uart_error_count +
                              g_thermal_uart_context.stats.rx_overflow_count;
    p_mailbox->uart_error_count = g_thermal_uart_context.stats.uart_error_count;
    p_mailbox->rx_overflow_count = g_thermal_uart_context.stats.rx_overflow_count;
    p_mailbox->crc_received = 0;
    p_mailbox->crc_calculated = 0;
    p_mailbox->valid_points = 0;
    p_mailbox->head_word = g_thermal_uart_context.current_baud;
    p_mailbox->raw0 = g_thermal_uart_context.stats.rx_bytes;
    p_mailbox->raw1 = (uint32_t) g_thermal_uart_context.stats.last_event;
    p_mailbox->raw_center = g_thermal_uart_context.stats.tx_complete_count;
    p_mailbox->sensor_raw = ((uint32_t) g_thermal_uart_context.rx_head << 16) |
                             g_thermal_uart_context.rx_tail;
    p_mailbox->parity_error_count = g_thermal_uart_context.stats.parity_error_count;
    p_mailbox->framing_error_count = g_thermal_uart_context.stats.framing_error_count;
    p_mailbox->fifo_overflow_count = g_thermal_uart_context.stats.fifo_overflow_count;
    p_mailbox->crc_error_count = g_thermal_uart_context.stats.crc_error_count;
    p_mailbox->data_error_count = g_thermal_uart_context.stats.data_error_count;
    p_mailbox->baud_set_count = g_thermal_uart_context.stats.baud_set_count;
    p_mailbox->frame_zero_words = 0U;
    p_mailbox->frame_one_words = 0U;
    p_mailbox->frame_ffff_words = 0U;
    p_mailbox->driver_build = THERMAL_UART_DRIVER_BUILD;
    thermal_publish_recovery_diagnostics(p_mailbox);
    __DMB();
    p_mailbox->sequence++;
}

static bool thermal_publish_frame(uint32_t frame_length)
{
    volatile thermal_ipc_mailbox_t * p_mailbox = THERMAL_IPC_MAILBOX;
    uint16_t crc_received = 0U;
    uint16_t crc_calculated = thermal_crc16_modbus(&g_thermal_frame[THERMAL_DATA_OFFSET],
                                                   THERMAL_IPC_POINT_COUNT * 2U);
    int16_t min_cdeg = INT16_MAX;
    int16_t max_cdeg = INT16_MIN;
    int32_t sum_cdeg = 0;
    int16_t first_cdeg = 0;
    int16_t center_cdeg = 0;
    int16_t p01_cdeg = 0;
    int16_t p99_cdeg = 0;
    int32_t mean_cdeg = 0;
    uint32_t valid_count = 0U;
    uint16_t first_raw = 0U;
    uint16_t second_raw = 0U;
    uint16_t center_raw = 0U;
    uint16_t sensor_raw = 0U;
    uint32_t zero_words = 0U;
    uint32_t one_words = 0U;
    uint32_t ffff_words = 0U;
    uint32_t frame_signature = 0U;
    uint32_t mean_abs_diff_cdeg = 0U;
    uint32_t changed_pixel_count = 0U;
    uint32_t diff_sum_cdeg = 0U;
    bool crc_valid;
    bool frame_accepted = false;
    bool transient_frame = false;
    thermal_ipc_status_t frame_status;

    memset(g_thermal_histogram, 0, sizeof(g_thermal_histogram));

    if (frame_length >= THERMAL_UART_FRAME_LENGTH)
    {
        crc_received = (uint16_t) (((uint16_t) g_thermal_frame[THERMAL_CRC_OFFSET] << 8) |
                                   g_thermal_frame[THERMAL_CRC_OFFSET + 1U]);
    }

    if (frame_length >= THERMAL_CRC_OFFSET)
    {
        sensor_raw = (uint16_t) (((uint16_t) g_thermal_frame[THERMAL_SENSOR_TEMP_OFFSET + 1U] << 8) |
                                 g_thermal_frame[THERMAL_SENSOR_TEMP_OFFSET]);
    }

    for (uint32_t i = 0U; i < THERMAL_IPC_POINT_COUNT; i++)
    {
        uint32_t offset = THERMAL_DATA_OFFSET + (i * 2U);
        uint16_t raw = thermal_frame_u16_le(offset);
        int16_t cdeg = thermal_raw_to_cdeg(raw);
        bool valid = thermal_raw_is_valid(raw);

        if (0U == raw)
        {
            zero_words++;
        }
        else if (1U == raw)
        {
            one_words++;
        }
        else if (UINT16_MAX == raw)
        {
            ffff_words++;
        }

        if (0U == i)
        {
            first_cdeg = cdeg;
            first_raw = raw;
        }

        if (1U == i)
        {
            second_raw = raw;
        }

        if (THERMAL_CENTER_INDEX == i)
        {
            center_cdeg = cdeg;
            center_raw = raw;
        }

        if (!valid)
        {
            continue;
        }

        g_thermal_histogram[raw - THERMAL_RAW_MIN_VALID]++;

        if (g_thermal_has_last_good)
        {
            uint32_t difference = thermal_abs_i32((int32_t) cdeg - THERMAL_IPC_POINTS[i]);
            diff_sum_cdeg += difference;
            if (difference > THERMAL_TRANSIENT_PIXEL_DELTA_CDEG)
            {
                changed_pixel_count++;
            }
        }

        if (cdeg < min_cdeg)
        {
            min_cdeg = cdeg;
        }

        if (cdeg > max_cdeg)
        {
            max_cdeg = cdeg;
        }

        sum_cdeg += cdeg;
        valid_count++;
    }

    if (0U == valid_count)
    {
        min_cdeg = 0;
        max_cdeg = 0;
        sum_cdeg = 0;
    }
    else
    {
        mean_cdeg = sum_cdeg / (int32_t) valid_count;
        p01_cdeg = thermal_histogram_percentile(((valid_count - 1U) * 1U) / 100U);
        p99_cdeg = thermal_histogram_percentile(((valid_count - 1U) * 99U) / 100U);
        if (g_thermal_has_last_good)
        {
            mean_abs_diff_cdeg = diff_sum_cdeg / valid_count;
        }
    }

    if (!thermal_raw_is_valid(center_raw))
    {
        center_cdeg = 0;
    }

    crc_valid = ((frame_length >= THERMAL_UART_FRAME_LENGTH) &&
                 (crc_received == crc_calculated));
    if (!crc_valid)
    {
        g_thermal_uart_context.stats.crc_error_count++;
        frame_status = THERMAL_IPC_STATUS_CRC_ERROR;
    }
    else if (THERMAL_IPC_POINT_COUNT != valid_count)
    {
        g_thermal_uart_context.stats.data_error_count++;
        frame_status = THERMAL_IPC_STATUS_DATA_ERROR;
    }
    else
    {
        frame_signature = thermal_frame_signature(
            &g_thermal_frame[THERMAL_DATA_OFFSET],
            THERMAL_IPC_POINT_COUNT * 2U);
        if (g_thermal_uart_context.frame_signature_valid &&
            (frame_signature == g_thermal_uart_context.last_frame_signature))
        {
            g_thermal_uart_context.identical_frame_streak++;
        }
        else
        {
            g_thermal_uart_context.last_frame_signature = frame_signature;
            g_thermal_uart_context.identical_frame_streak = 0U;
            g_thermal_uart_context.frame_signature_valid = true;
        }

        if (g_thermal_uart_context.identical_frame_streak >=
            THERMAL_IDENTICAL_FRAME_LIMIT)
        {
            g_thermal_uart_context.stale_frame_count++;
            frame_status = THERMAL_IPC_STATUS_STALE_FRAME;
        }
        else
        {
            frame_status = THERMAL_IPC_STATUS_FRAME_OK;
        }
    }
    /* A complete CRC-valid frame can still be a one-frame NUC/FFC transition.
     * Require two mutually close frames before accepting a large scene-wide
     * change. The transport remains healthy while the previous image is held. */
    if (THERMAL_IPC_STATUS_FRAME_OK == frame_status)
    {
        bool scene_wide_jump =
            g_thermal_has_last_good &&
            (thermal_abs_i32(mean_cdeg - g_thermal_last_good_mean_cdeg) >
             THERMAL_TRANSIENT_MEAN_DELTA_CDEG) &&
            ((changed_pixel_count * 100U) >=
             (THERMAL_IPC_POINT_COUNT * THERMAL_TRANSIENT_CHANGED_PERCENT));

        if (scene_wide_jump)
        {
            if (g_thermal_transient_pending &&
                (thermal_abs_i32(mean_cdeg - g_thermal_transient_mean_cdeg) <=
                 THERMAL_TRANSIENT_CONFIRM_MEAN_CDEG))
            {
                frame_accepted = true;
                g_thermal_transient_pending = false;
            }
            else
            {
                transient_frame = true;
                g_thermal_transient_pending = true;
                g_thermal_transient_mean_cdeg = mean_cdeg;
            }
        }
        else
        {
            frame_accepted = true;
            g_thermal_transient_pending = false;
        }

        if (frame_accepted)
        {
            g_thermal_has_last_good = true;
            g_thermal_last_good_mean_cdeg = mean_cdeg;
            g_thermal_uart_context.consecutive_failures = 0U;
            g_thermal_uart_context.recovery_cycles_since_valid = 0U;
            g_thermal_uart_context.last_good_tick = xTaskGetTickCount();
        }
    }

    p_mailbox->magic = THERMAL_IPC_MAGIC;
    p_mailbox->rx_bytes = frame_length;
    p_mailbox->uart_errors = g_thermal_uart_context.stats.uart_error_count +
                              g_thermal_uart_context.stats.rx_overflow_count;
    p_mailbox->uart_error_count = g_thermal_uart_context.stats.uart_error_count;
    p_mailbox->rx_overflow_count = g_thermal_uart_context.stats.rx_overflow_count;
    p_mailbox->crc_received = crc_received;
    p_mailbox->crc_calculated = crc_calculated;
    p_mailbox->head_word = ((uint32_t) g_thermal_frame[0] << 24) |
                           ((uint32_t) g_thermal_frame[1] << 16) |
                           ((uint32_t) g_thermal_frame[2] << 8) |
                           g_thermal_frame[3];
    p_mailbox->raw0 = ((uint32_t) first_raw << 16) | second_raw;
    p_mailbox->raw1 = ((uint32_t) thermal_frame_u16_le(1024U) << 16) |
                       thermal_frame_u16_le(2048U);
    p_mailbox->raw_center = ((uint32_t) center_raw << 16) |
                             thermal_frame_u16_le(8192U);
    p_mailbox->sensor_raw = ((uint32_t) sensor_raw << 16) |
                             crc_received;
    p_mailbox->parity_error_count = g_thermal_uart_context.stats.parity_error_count;
    p_mailbox->framing_error_count = g_thermal_uart_context.stats.framing_error_count;
    p_mailbox->fifo_overflow_count = g_thermal_uart_context.stats.fifo_overflow_count;
    p_mailbox->crc_error_count = g_thermal_uart_context.stats.crc_error_count;
    p_mailbox->data_error_count = g_thermal_uart_context.stats.data_error_count;
    p_mailbox->baud_set_count = g_thermal_uart_context.stats.baud_set_count;
    p_mailbox->frame_zero_words = zero_words;
    p_mailbox->frame_one_words = one_words;
    p_mailbox->frame_ffff_words = ffff_words;
    p_mailbox->driver_build = THERMAL_UART_DRIVER_BUILD;
    p_mailbox->mean_abs_diff_cdeg = mean_abs_diff_cdeg;
    p_mailbox->changed_pixel_count = changed_pixel_count;
    thermal_publish_recovery_diagnostics(p_mailbox);

    if (frame_accepted)
    {
        uint32_t next_sequence = p_mailbox->sequence + 1U;
        uint32_t odd_generation = thermal_matrix_write_begin(p_mailbox);

        for (uint32_t i = 0U; i < THERMAL_IPC_POINT_COUNT; i++)
        {
            uint32_t offset = THERMAL_DATA_OFFSET + (i * 2U);
            THERMAL_IPC_POINTS[i] = thermal_raw_to_cdeg(thermal_frame_u16_le(offset));
        }
        p_mailbox->status = (uint32_t) THERMAL_IPC_STATUS_FRAME_OK;
        p_mailbox->valid_points = valid_count;
        p_mailbox->min_cdeg = min_cdeg;
        p_mailbox->max_cdeg = max_cdeg;
        p_mailbox->center_cdeg = center_cdeg;
        p_mailbox->first_cdeg = first_cdeg;
        p_mailbox->avg_cdeg = mean_cdeg;
        p_mailbox->p01_cdeg = p01_cdeg;
        p_mailbox->p99_cdeg = p99_cdeg;
        p_mailbox->mean_cdeg = mean_cdeg;
        p_mailbox->frame_quality = (uint32_t) THERMAL_FRAME_QUALITY_NORMAL;
        p_mailbox->last_good_sequence = next_sequence;
        thermal_matrix_write_end(p_mailbox, odd_generation);
        p_mailbox->sequence = next_sequence;
        return true;
    }

    if (transient_frame)
    {
        p_mailbox->status = (uint32_t) THERMAL_IPC_STATUS_FRAME_OK;
        p_mailbox->valid_points = valid_count;
        p_mailbox->frame_quality = (uint32_t) THERMAL_FRAME_QUALITY_TRANSIENT;
        p_mailbox->transient_drop_count++;
        __DMB();
        return true;
    }

    p_mailbox->status = (uint32_t) frame_status;
    p_mailbox->valid_points = valid_count;
    p_mailbox->frame_quality = (uint32_t) THERMAL_FRAME_QUALITY_INVALID;
    __DMB();
    p_mailbox->sequence++;
    return false;
}

static void thermal_uart_close_peripheral(void)
{
    if (!g_thermal_uart_context.initialized)
    {
        return;
    }

    (void) thermal_uart_stop_dtc_read();
    (void) g_thermal_uart.p_api->communicationAbort(g_thermal_uart.p_ctrl, UART_DIR_RX_TX);
    (void) g_thermal_uart.p_api->close(g_thermal_uart.p_ctrl);
    g_thermal_uart_context.initialized = false;
    g_thermal_uart_context.current_baud = 0U;
    g_thermal_uart_context.rx_done = false;
    g_thermal_uart_context.rx_error = false;
    g_thermal_uart_context.rx_active = false;
    g_thermal_uart_context.rx_expected_bytes = 0U;
    g_thermal_uart_context.stream_enabled = false;
    thermal_uart_flush_rx();
}

static fsp_err_t thermal_uart_open_peripheral(bool recovery_open)
{
    uart_cfg_t const * p_open_cfg;

    g_thermal_uart_runtime_cfg = *g_thermal_uart.p_cfg;
    g_thermal_uart_runtime_extend =
        *((sci_b_uart_extended_cfg_t const *) g_thermal_uart.p_cfg->p_extend);
    g_thermal_uart_runtime_extend.noise_cancel = SCI_B_UART_NOISE_CANCELLATION_ENABLE;
    g_thermal_uart_runtime_cfg.p_extend = &g_thermal_uart_runtime_extend;

    if (g_thermal_uart_context.irq_rx_fallback)
    {
        /* SCI1/DTC can remain armed but deliver no completion or error after a
         * recovery cycle. IRQ/FIFO receive is slower but self-resynchronizing
         * and lets the parser recover from arbitrary byte boundaries. */
        g_thermal_uart_runtime_cfg.p_transfer_rx = NULL;
    }

    p_open_cfg = &g_thermal_uart_runtime_cfg;

    fsp_err_t err = g_thermal_uart.p_api->open(g_thermal_uart.p_ctrl, p_open_cfg);

    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return err;
    }

    g_thermal_uart_context.initialized = true;
    g_thermal_uart_context.dtc_enabled = (NULL != p_open_cfg->p_transfer_rx);
    g_thermal_uart_context.current_baud = 0U;
    err = thermal_uart_apply_baud(THERMAL_UART_BAUD_RATE);
    if (FSP_SUCCESS != err)
    {
        thermal_uart_close_peripheral();
        return err;
    }

    if (recovery_open)
    {
        g_thermal_uart_context.reopen_count++;
    }

    return FSP_SUCCESS;
}

static bool thermal_uart_enter_request_mode(void)
{
    static const uint8_t output_off_command[THERMAL_REQUEST_LENGTH] =
    {
        THERMAL_FRAME_HEADER,
        0x01U,
        THERMAL_OUTPUT_OFF_COMMAND
    };
    static const uint8_t auto_full_output_command[THERMAL_REQUEST_LENGTH] =
    {
        THERMAL_FRAME_HEADER,
        0x01U,
        THERMAL_AUTO_FULL_COMMAND
    };

    if (g_thermal_uart_context.dtc_enabled)
    {
        if (!thermal_uart_write_command(output_off_command,
                                        THERMAL_REQUEST_LENGTH,
                                        pdMS_TO_TICKS(50U)))
        {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(THERMAL_STREAM_DRAIN_MS));
        g_thermal_uart_context.stream_enabled = false;
    }
    else
    {
        if (!thermal_uart_write_command(auto_full_output_command,
                                        THERMAL_REQUEST_LENGTH,
                                        pdMS_TO_TICKS(50U)))
        {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
        g_thermal_uart_context.stream_enabled = true;
    }

    g_thermal_uart_context.rx_done = false;
    g_thermal_uart_context.rx_error = false;
    g_thermal_uart_context.rx_active = false;
    g_thermal_uart_context.rx_expected_bytes = 0U;
    thermal_uart_flush_rx();
    return true;
}

static bool thermal_uart_configure_power_relay(void)
{
    fsp_err_t cfg_err = R_IOPORT_PinCfg(&g_ioport_ctrl,
                                       THERMAL_POWER_RELAY_PIN,
                                       THERMAL_RELAY_INACTIVE_CFG);
    fsp_err_t write_err = FSP_ERR_NOT_OPEN;

    if (FSP_SUCCESS == cfg_err)
    {
        write_err = R_IOPORT_PinWrite(&g_ioport_ctrl,
                                     THERMAL_POWER_RELAY_PIN,
                                     THERMAL_RELAY_INACTIVE_LEVEL);
    }

    g_thermal_uart_context.power_relay_ready =
        (FSP_SUCCESS == cfg_err) && (FSP_SUCCESS == write_err);
    return g_thermal_uart_context.power_relay_ready;
}

fsp_err_t thermal_uart_driver_init(void)
{
    fsp_err_t err;
    volatile thermal_ipc_mailbox_t * p_mailbox = THERMAL_IPC_MAILBOX;

    if (g_thermal_uart_context.initialized)
    {
        return FSP_SUCCESS;
    }

    memset(&g_thermal_uart_context, 0, sizeof(g_thermal_uart_context));
    g_thermal_has_last_good = false;
    g_thermal_transient_pending = false;
    g_thermal_last_good_mean_cdeg = 0;
    g_thermal_transient_mean_cdeg = 0;
    p_mailbox->magic = 0U;
    p_mailbox->sequence = 0U;
    p_mailbox->status = (uint32_t) THERMAL_IPC_STATUS_EMPTY;
    p_mailbox->write_generation = 0U;
    p_mailbox->p01_cdeg = 0;
    p_mailbox->p99_cdeg = 0;
    p_mailbox->mean_cdeg = 0;
    p_mailbox->frame_quality = (uint32_t) THERMAL_FRAME_QUALITY_INVALID;
    p_mailbox->transient_drop_count = 0U;
    p_mailbox->generation_retry_count = 0U;
    p_mailbox->last_good_sequence = 0U;
    p_mailbox->mean_abs_diff_cdeg = 0U;
    p_mailbox->changed_pixel_count = 0U;
    __DMB();
    g_thermal_uart_context.tx_done = true;
    if (!thermal_uart_configure_power_relay())
    {
        g_thermal_uart_context.recovery_error_count++;
    }
    /* Keep SCI1 pins under the generated FSP configuration.  The previously
     * stable transport did not power-cycle or remux P706/P707 during startup. */

    err = thermal_uart_open_peripheral(false);
    if (FSP_SUCCESS != err)
    {
        thermal_publish_status(THERMAL_IPC_STATUS_OPEN_ERROR, 0U);
        return err;
    }

    thermal_publish_status(THERMAL_IPC_STATUS_WAITING_FRAME, 0U);
    if (!thermal_uart_enter_request_mode())
    {
        thermal_uart_close_peripheral();
        thermal_publish_status(THERMAL_IPC_STATUS_UART_ERROR, 0U);
        return FSP_ERR_ABORTED;
    }

    return FSP_SUCCESS;
}

static uint32_t thermal_uart_stop_dtc_read(void)
{
    uint32_t remaining_bytes = g_thermal_uart_context.rx_expected_bytes;

    if (g_thermal_uart_context.rx_active)
    {
        if (FSP_SUCCESS != g_thermal_uart.p_api->readStop(g_thermal_uart.p_ctrl, &remaining_bytes))
        {
            (void) g_thermal_uart.p_api->communicationAbort(g_thermal_uart.p_ctrl, UART_DIR_RX);
        }
    }

    g_thermal_uart_context.rx_active = false;
    g_thermal_uart_context.rx_expected_bytes = 0U;
    return remaining_bytes;
}

static bool thermal_uart_recover(bool power_cycle, thermal_recovery_reason_t reason)
{
    volatile thermal_ipc_mailbox_t * p_mailbox = THERMAL_IPC_MAILBOX;
    fsp_err_t open_err;
    bool reset_ok = true;

    g_thermal_uart_context.recovery_count++;
    g_thermal_uart_context.recovery_cycles_since_valid++;
    g_thermal_uart_context.last_recovery_reason = (uint32_t) reason;
    g_thermal_uart_context.last_recovery_attempt_tick = xTaskGetTickCount();
    g_thermal_transient_pending = false;
    thermal_publish_status(THERMAL_IPC_STATUS_RECOVERING, 0U);
    (void) R_BSP_IpcNmiRequestSet();

    /* A physical reset does not repair a zero-byte receive path when SCI/DTC
     * has lost synchronization.  Restore the last stable behavior first:
     * keep SCI1 open, reapply 460800 baud, and re-enter request mode. */
    FSP_PARAMETER_NOT_USED(power_cycle);

    if (g_thermal_uart_context.initialized)
    {
        (void) thermal_uart_stop_dtc_read();
        (void) g_thermal_uart.p_api->communicationAbort(g_thermal_uart.p_ctrl, UART_DIR_RX);
        g_thermal_uart_context.rx_done = false;
        g_thermal_uart_context.rx_error = false;
        thermal_uart_flush_rx();

        g_thermal_uart_context.current_baud = 0U;
        if (FSP_SUCCESS != thermal_uart_apply_baud(THERMAL_UART_BAUD_RATE))
        {
            g_thermal_uart_context.recovery_error_count++;
        }
        else if (thermal_uart_enter_request_mode())
        {
            vTaskDelay(pdMS_TO_TICKS(THERMAL_RECOVERY_DRAIN_MS));
            p_mailbox->consecutive_failures = 0U;
            return true;
        }

        /* Fall through only when the stable soft-resync path itself fails. */
        g_thermal_uart_context.recovery_error_count++;
    }

    thermal_uart_close_peripheral();
    vTaskDelay(pdMS_TO_TICKS(THERMAL_UART_REOPEN_DELAY_MS));

    open_err = thermal_uart_open_peripheral(true);
    if ((FSP_SUCCESS != open_err) || !thermal_uart_enter_request_mode())
    {
        g_thermal_uart_context.recovery_error_count++;
        g_thermal_uart_context.last_recovery_reason = THERMAL_RECOVERY_REOPEN_ERROR;
        thermal_uart_close_peripheral();
        thermal_publish_status(THERMAL_IPC_STATUS_OPEN_ERROR, 0U);
        (void) R_BSP_IpcNmiRequestSet();
        return false;
    }

    /* Give the module and DTC one quiet window before the next exact-length
     * read is armed. */
    vTaskDelay(pdMS_TO_TICKS(THERMAL_RECOVERY_DRAIN_MS));
    p_mailbox->consecutive_failures = 0U;
    return reset_ok;
}

static thermal_recovery_reason_t thermal_recovery_reason_from_status(uint32_t status)
{
    switch ((thermal_ipc_status_t) status)
    {
        case THERMAL_IPC_STATUS_FRAME_TIMEOUT:
            return THERMAL_RECOVERY_FRAME_TIMEOUT;
        case THERMAL_IPC_STATUS_UART_ERROR:
            return THERMAL_RECOVERY_UART_ERROR;
        case THERMAL_IPC_STATUS_CRC_ERROR:
            return THERMAL_RECOVERY_CRC_ERROR;
        case THERMAL_IPC_STATUS_DATA_ERROR:
            return THERMAL_RECOVERY_DATA_ERROR;
        case THERMAL_IPC_STATUS_STALE_FRAME:
            return THERMAL_RECOVERY_STALE_FRAME;
        default:
            return THERMAL_RECOVERY_UART_ERROR;
    }
}

static bool thermal_stream_publish_latest(uint32_t captured_bytes)
{
    uint32_t latest_offset = 0U;
    bool frame_found = false;

    if (captured_bytes < THERMAL_UART_FRAME_LENGTH)
    {
        return false;
    }

    for (uint32_t offset = 0U;
         (offset + THERMAL_UART_FRAME_LENGTH) <= captured_bytes;
         offset++)
    {
        const uint8_t * p_frame = &g_thermal_stream_capture[offset];

        if ((THERMAL_FRAME_HEADER != p_frame[0]) ||
            (0x08U != p_frame[1]) ||
            (0x46U != p_frame[2]))
        {
            continue;
        }

        uint16_t crc_received =
            (uint16_t) (((uint16_t) p_frame[THERMAL_CRC_OFFSET] << 8) |
                        p_frame[THERMAL_CRC_OFFSET + 1U]);
        uint16_t crc_calculated =
            thermal_crc16_modbus(&p_frame[THERMAL_DATA_OFFSET],
                                  THERMAL_IPC_POINT_COUNT * 2U);

        if (crc_received == crc_calculated)
        {
            latest_offset = offset;
            frame_found = true;
        }
    }

    if (!frame_found)
    {
        return false;
    }

    memcpy(g_thermal_frame,
           &g_thermal_stream_capture[latest_offset],
           THERMAL_UART_FRAME_LENGTH);
    return thermal_publish_frame(THERMAL_UART_FRAME_LENGTH);
}

static void thermal_uart_pause_stream(void)
{
    static const uint8_t output_off_command[THERMAL_REQUEST_LENGTH] =
    {
        THERMAL_FRAME_HEADER,
        0x01U,
        THERMAL_OUTPUT_OFF_COMMAND
    };

    if (!g_thermal_uart_context.stream_enabled)
    {
        return;
    }

    /* Once a DTC block completes there is no receive buffer behind it.  Stop
     * the module before parsing/publishing so the SCI FIFO cannot overflow in
     * the gap before the next block is armed. */
    (void) thermal_uart_write_command(output_off_command,
                                      THERMAL_REQUEST_LENGTH,
                                      pdMS_TO_TICKS(50U));
    vTaskDelay(pdMS_TO_TICKS(20U));
    (void) g_thermal_uart.p_api->communicationAbort(g_thermal_uart.p_ctrl,
                                                     UART_DIR_RX);
    g_thermal_uart_context.stream_enabled = false;
    g_thermal_uart_context.rx_done = false;
    g_thermal_uart_context.rx_error = false;
    thermal_uart_flush_rx();
}

static bool thermal_uart_request_frame_dtc(TickType_t timeout_ticks)
{
    static const uint8_t auto_full_output_command[THERMAL_REQUEST_LENGTH] =
    {
        THERMAL_FRAME_HEADER,
        0x01U,
        THERMAL_AUTO_FULL_COMMAND
    };
    TickType_t start_ticks;
    uint32_t remaining_bytes;
    uint32_t received_bytes;
    fsp_err_t err;

    /*
     * The vendor PC application leaves the module in continuous-output mode
     * and resynchronizes in the byte stream. Capture two frame lengths so a
     * complete CRC-valid frame is available even when DTC starts mid-frame.
     */
    if (g_thermal_uart_context.rx_active)
    {
        (void) thermal_uart_stop_dtc_read();
    }

    g_thermal_uart_context.rx_done = false;
    g_thermal_uart_context.rx_error = false;
    g_thermal_uart_context.rx_active = true;
    g_thermal_uart_context.rx_expected_bytes = THERMAL_STREAM_CAPTURE_BYTES;

    err = g_thermal_uart.p_api->read(g_thermal_uart.p_ctrl,
                                     g_thermal_stream_capture,
                                     THERMAL_STREAM_CAPTURE_BYTES);
    if (FSP_SUCCESS != err)
    {
        g_thermal_uart_context.rx_active = false;
        g_thermal_uart_context.rx_expected_bytes = 0U;
        thermal_publish_status(THERMAL_IPC_STATUS_UART_ERROR, 0U);
        (void) R_BSP_IpcNmiRequestSet();
        return false;
    }

    if (!g_thermal_uart_context.stream_enabled &&
        !thermal_uart_write_command(auto_full_output_command,
                                    THERMAL_REQUEST_LENGTH,
                                    pdMS_TO_TICKS(50U)))
    {
        (void) thermal_uart_stop_dtc_read();
        (void) R_BSP_IpcNmiRequestSet();
        return false;
    }
    g_thermal_uart_context.stream_enabled = true;

    start_ticks = xTaskGetTickCount();
    while (!g_thermal_uart_context.rx_done && !g_thermal_uart_context.rx_error)
    {
        if ((xTaskGetTickCount() - start_ticks) >= timeout_ticks)
        {
            remaining_bytes = thermal_uart_stop_dtc_read();
            received_bytes = THERMAL_STREAM_CAPTURE_BYTES - remaining_bytes;
            thermal_uart_pause_stream();
            if (thermal_stream_publish_latest(received_bytes))
            {
                (void) R_BSP_IpcNmiRequestSet();
                return true;
            }
            thermal_publish_status(THERMAL_IPC_STATUS_FRAME_TIMEOUT, received_bytes);
            (void) R_BSP_IpcNmiRequestSet();
            return false;
        }

        vTaskDelay(1);
    }

    if (g_thermal_uart_context.rx_error)
    {
        remaining_bytes = thermal_uart_stop_dtc_read();
        received_bytes = THERMAL_STREAM_CAPTURE_BYTES - remaining_bytes;
        thermal_uart_pause_stream();
        if (thermal_stream_publish_latest(received_bytes))
        {
            (void) R_BSP_IpcNmiRequestSet();
            return true;
        }
        thermal_publish_status(THERMAL_IPC_STATUS_UART_ERROR, received_bytes);
        (void) R_BSP_IpcNmiRequestSet();
        return false;
    }

    g_thermal_uart_context.rx_active = false;
    g_thermal_uart_context.rx_expected_bytes = 0U;
    thermal_uart_pause_stream();

    if (!thermal_stream_publish_latest(THERMAL_STREAM_CAPTURE_BYTES))
    {
        g_thermal_uart_context.stats.data_error_count++;
        thermal_publish_status(THERMAL_IPC_STATUS_DATA_ERROR,
                               THERMAL_STREAM_CAPTURE_BYTES);
        (void) R_BSP_IpcNmiRequestSet();
        return false;
    }

    (void) R_BSP_IpcNmiRequestSet();
    return true;
}

bool thermal_uart_request_frame(TickType_t timeout_ticks)
{
    static const uint8_t request_frame_command[THERMAL_REQUEST_LENGTH] =
    {
        THERMAL_FRAME_HEADER,
        0x01U,
        THERMAL_REQUEST_FRAME_COMMAND
    };
    TickType_t start_ticks;
    uint32_t frame_offset = THERMAL_HEADER_LENGTH;

    if (!g_thermal_uart_context.initialized)
    {
        TickType_t now = xTaskGetTickCount();

        if ((0U == g_thermal_uart_context.last_recovery_attempt_tick) ||
            ((now - g_thermal_uart_context.last_recovery_attempt_tick) >=
             pdMS_TO_TICKS(THERMAL_REINIT_RETRY_MS)))
        {
            (void) thermal_uart_recover(false,
                                        THERMAL_RECOVERY_REOPEN_ERROR);
        }
        return false;
    }

    if (g_thermal_uart_context.dtc_enabled)
    {
        volatile thermal_ipc_mailbox_t * p_mailbox = THERMAL_IPC_MAILBOX;
        bool success = thermal_uart_request_frame_dtc(timeout_ticks);

        if (success)
        {
            g_thermal_uart_context.consecutive_failures = 0U;
            g_thermal_uart_context.recovery_cycles_since_valid = 0U;
        }
        else
        {
            g_thermal_uart_context.consecutive_failures++;
            p_mailbox->consecutive_failures = g_thermal_uart_context.consecutive_failures;
            if (g_thermal_uart_context.consecutive_failures >=
                THERMAL_RECOVERY_FAILURE_THRESHOLD)
            {
                g_thermal_uart_context.consecutive_failures = 0U;

                thermal_recovery_reason_t reason =
                    thermal_recovery_reason_from_status(p_mailbox->status);
                (void) thermal_uart_recover(false, reason);
            }
        }

        return success;
    }

    thermal_uart_flush_rx();
    FSP_PARAMETER_NOT_USED(request_frame_command);

    if (!thermal_uart_find_frame_header(timeout_ticks))
    {
        thermal_publish_status(THERMAL_IPC_STATUS_FRAME_TIMEOUT, 0U);
        (void) R_BSP_IpcNmiRequestSet();
        return false;
    }

    start_ticks = xTaskGetTickCount();

    while (frame_offset < THERMAL_UART_FRAME_LENGTH)
    {
        if ((xTaskGetTickCount() - start_ticks) >= timeout_ticks)
        {
            /*
             * A 10248-byte frame is required by the thermal module protocol.
             * Publishing a near-complete frame as FRAME_OK leaves stale bytes in
             * the tail and can make the parser report valid_points == 0 even
             * though the UART link is receiving data.
             */
            thermal_publish_status(THERMAL_IPC_STATUS_FRAME_TIMEOUT, frame_offset);
            (void) R_BSP_IpcNmiRequestSet();
            return false;
        }

        if (thermal_uart_try_pop_byte(&g_thermal_frame[frame_offset]))
        {
            frame_offset++;
        }
        else
        {
            vTaskDelay(1);
        }
    }

    bool frame_valid = thermal_publish_frame(THERMAL_UART_FRAME_LENGTH);
    (void) R_BSP_IpcNmiRequestSet();
    return frame_valid;
}

void thermal_uart_tx_probe_loop(void)
{
    while (1)
    {
        (void) thermal_uart_request_frame(pdMS_TO_TICKS(1500));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void thermal_uart_get_stats(thermal_uart_stats_t * p_stats)
{
    if (NULL == p_stats)
    {
        return;
    }

    taskENTER_CRITICAL();
    *p_stats = g_thermal_uart_context.stats;
    taskEXIT_CRITICAL();
}

void thermal_uart_callback(uart_callback_args_t * p_args)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (NULL == p_args)
    {
        return;
    }

    g_thermal_uart_context.stats.last_event = p_args->event;

    switch (p_args->event)
    {
        case UART_EVENT_RX_CHAR:
        {
            thermal_uart_push_byte((uint8_t) p_args->data);
            break;
        }

        case UART_EVENT_ERR_PARITY:
        {
            g_thermal_uart_context.stats.uart_error_count++;
            g_thermal_uart_context.stats.parity_error_count++;
            g_thermal_uart_context.rx_error = true;
            g_thermal_uart_context.rx_done = true;
            break;
        }

        case UART_EVENT_ERR_FRAMING:
        {
            g_thermal_uart_context.stats.uart_error_count++;
            g_thermal_uart_context.stats.framing_error_count++;
            g_thermal_uart_context.rx_error = true;
            g_thermal_uart_context.rx_done = true;
            break;
        }

        case UART_EVENT_ERR_OVERFLOW:
        {
            g_thermal_uart_context.stats.uart_error_count++;
            g_thermal_uart_context.stats.fifo_overflow_count++;
            g_thermal_uart_context.rx_error = true;
            g_thermal_uart_context.rx_done = true;
            break;
        }

        case UART_EVENT_TX_COMPLETE:
        {
            g_thermal_uart_context.stats.tx_complete_count++;
            g_thermal_uart_context.tx_done = true;
            break;
        }

        case UART_EVENT_RX_COMPLETE:
        {
            g_thermal_uart_context.stats.rx_bytes += g_thermal_uart_context.rx_expected_bytes;
            g_thermal_uart_context.rx_active = false;
            g_thermal_uart_context.rx_done = true;
            break;
        }

        default:
        {
            break;
        }
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
}

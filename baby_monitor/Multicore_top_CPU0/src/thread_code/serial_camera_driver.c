#include "thread_code/serial_camera_driver.h"

#include <stdbool.h>
#include <string.h>

#include "camera_thread.h"
#include "thread_code/serial_camera_ipc_shared.h"
#include "thread_code/vision_runtime.h"

#define SERIAL_CAMERA_COMMAND_TIMEOUT_TICKS (pdMS_TO_TICKS(1500U))
#define SERIAL_CAMERA_FRAME_COMMAND_TIMEOUT_TICKS (pdMS_TO_TICKS(6000U))
#define SERIAL_CAMERA_CHUNK_TIMEOUT_TICKS   (pdMS_TO_TICKS(3000U))
#define SERIAL_CAMERA_WRITER_TIMEOUT_TICKS  (pdMS_TO_TICKS(5000U))
#define SERIAL_CAMERA_PROBE_PERIOD_TICKS    (pdMS_TO_TICKS(2000U))
#define SERIAL_CAMERA_SUCCESS_WATCHDOG_TICKS (pdMS_TO_TICKS(30000U))
#define SERIAL_CAMERA_FRAME_SETTLE_TICKS    (pdMS_TO_TICKS(300U))
#define SERIAL_CAMERA_RECOVERY_COOLDOWN_TICKS (pdMS_TO_TICKS(5000U))
#define SERIAL_CAMERA_UART_REOPEN_DELAY_TICKS (pdMS_TO_TICKS(20U))
#define SERIAL_CAMERA_FRAME_RELEASE_TICKS (pdMS_TO_TICKS(100U))
#define SERIAL_CAMERA_SOFT_RESET_TIMEOUT_TICKS (pdMS_TO_TICKS(750U))
#define SERIAL_CAMERA_SOFT_RESET_BOOT_TICKS (pdMS_TO_TICKS(2000U))
#define SERIAL_CAMERA_RECOVERY_FAILURE_THRESHOLD (2U)
#define SERIAL_CAMERA_RECOVERIES_BEFORE_POWER_CYCLE (2U)
#define SERIAL_CAMERA_POWER_CUT_TICKS    (pdMS_TO_TICKS(2000U))
#define SERIAL_CAMERA_POWER_BOOT_TICKS   (pdMS_TO_TICKS(4000U))
#define SERIAL_CAMERA_POWER_CYCLE_COOLDOWN_TICKS (pdMS_TO_TICKS(30000U))
#define SERIAL_CAMERA_POWER_RELAY_ACTIVE_HIGH (1U)
#define SERIAL_CAMERA_POWER_RELAY_PIN    BSP_IO_PORT_09_PIN_07 /* P907 / P6-22 */
#define SERIAL_CAMERA_UART_RX_PIN        BSP_IO_PORT_09_PIN_05 /* P905 / RXD3 */
#define SERIAL_CAMERA_UART_TX_PIN        BSP_IO_PORT_03_PIN_10 /* P310 / TXD3 */
#define SERIAL_CAMERA_GPIO_INPUT_CFG     ((uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT)
#define SERIAL_CAMERA_GPIO_IDLE_HIGH_CFG ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                          (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH | \
                                          (uint32_t) IOPORT_CFG_DRIVE_MID)
#define SERIAL_CAMERA_UART_PERIPHERAL_CFG ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | \
                                           (uint32_t) IOPORT_PERIPHERAL_SCI1_3_5_7_9)
#if SERIAL_CAMERA_POWER_RELAY_ACTIVE_HIGH
#define SERIAL_CAMERA_RELAY_ACTIVE_LEVEL   BSP_IO_LEVEL_HIGH
#define SERIAL_CAMERA_RELAY_INACTIVE_LEVEL BSP_IO_LEVEL_LOW
#define SERIAL_CAMERA_RELAY_INACTIVE_CFG   ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                            (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW | \
                                            (uint32_t) IOPORT_CFG_DRIVE_MID)
#else
#define SERIAL_CAMERA_RELAY_ACTIVE_LEVEL   BSP_IO_LEVEL_LOW
#define SERIAL_CAMERA_RELAY_INACTIVE_LEVEL BSP_IO_LEVEL_HIGH
#define SERIAL_CAMERA_RELAY_INACTIVE_CFG   ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                            (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH | \
                                            (uint32_t) IOPORT_CFG_DRIVE_MID)
#endif
#define SERIAL_CAMERA_DEFAULT_INTERVAL_MS   (1000U)
#define SERIAL_CAMERA_MIN_INTERVAL_MS       (1000U)
#define SERIAL_CAMERA_MAX_JPEG_BYTES        (4U * 1024U * 1024U)
#define SERIAL_CAMERA_REPLY_OVERHEAD        (10U)
#define SERIAL_CAMERA_WIRE_CHUNK_BYTES      (1000U)
#define SERIAL_CAMERA_TX_IDLE_TIMEOUT_TICKS (pdMS_TO_TICKS(20U))
#define SERIAL_CAMERA_DRIVER_BUILD          (0x20260822U)

static volatile bool g_camera_uart_rx_complete;
static volatile bool g_camera_uart_tx_complete;
static volatile bool g_camera_uart_error;
static bool g_camera_uart_open;
static bool g_camera_device_ready;
static TickType_t g_camera_last_probe_tick;
static TickType_t g_camera_last_capture_tick;
static TickType_t g_camera_last_success_tick;
static TickType_t g_camera_last_recovery_tick;
static TickType_t g_camera_last_power_cycle_tick;
static uint32_t g_camera_recovery_cycles_without_frame;
static serial_camera_recovery_reason_t g_camera_pending_recovery_reason;
static uint8_t g_camera_chunk_reply[SERIAL_CAMERA_IPC_CHUNK_BYTES + SERIAL_CAMERA_REPLY_OVERHEAD]
    BSP_ALIGN_VARIABLE(4);

static void serial_camera_record_reply(uint8_t const * p_reply,
                                       uint32_t expected,
                                       uint32_t received,
                                       fsp_err_t result)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    uint32_t stage = p_cam->capture_stage;
    uint32_t copy_bytes = received;

    if (copy_bytes > sizeof(p_cam->last_reply))
    {
        copy_bytes = sizeof(p_cam->last_reply);
    }

    p_cam->rx_expected_bytes = expected;
    p_cam->rx_received_bytes = received;
    for (uint32_t i = 0U; i < sizeof(p_cam->last_reply); i++)
    {
        p_cam->last_reply[i] = (i < copy_bytes) ? p_reply[i] : 0U;
    }

    if ((stage > SERIAL_CAMERA_CAPTURE_STAGE_IDLE) &&
        (stage < SERIAL_CAMERA_CAPTURE_STAGE_COUNT))
    {
        volatile serial_camera_response_debug_t * p_debug = &p_cam->stage_response[stage];

        p_debug->command_code = p_cam->last_command_code;
        p_debug->expected_bytes = expected;
        p_debug->received_bytes = received;
        p_debug->fsp_err = (uint32_t) result;
        for (uint32_t i = 0U; i < sizeof(p_debug->reply); i++)
        {
            p_debug->reply[i] = (i < copy_bytes) ? p_reply[i] : 0U;
        }
    }
}

static void serial_camera_put_be32(uint8_t * p_data, uint32_t value)
{
    p_data[0] = (uint8_t) (value >> 24);
    p_data[1] = (uint8_t) (value >> 16);
    p_data[2] = (uint8_t) (value >> 8);
    p_data[3] = (uint8_t) value;
}

static uint32_t serial_camera_get_be32(uint8_t const * p_data)
{
    return ((uint32_t) p_data[0] << 24) |
           ((uint32_t) p_data[1] << 16) |
           ((uint32_t) p_data[2] << 8) |
           (uint32_t) p_data[3];
}

static bool serial_camera_reply_ok(uint8_t const * p_reply, uint32_t length, uint8_t command)
{
    return (length >= 5U) &&
           (0x76U == p_reply[0]) &&
           (0x00U == p_reply[1]) &&
           (command == p_reply[2]) &&
           (0x00U == p_reply[3]);
}

static void serial_camera_mark_stage_error(fsp_err_t err)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    uint32_t stage = p_cam->capture_stage;

    p_cam->last_fsp_err = (uint32_t) err;
    if ((stage > SERIAL_CAMERA_CAPTURE_STAGE_IDLE) &&
        (stage < SERIAL_CAMERA_CAPTURE_STAGE_COUNT))
    {
        p_cam->stage_response[stage].fsp_err = (uint32_t) err;
    }
}

static void serial_camera_note_failure(serial_camera_recovery_reason_t reason)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;

    if (SERIAL_CAMERA_RECOVERY_CAPTURE_FAILURE == reason)
    {
        /* A failed JPEG transaction leaves the VC0703 frame state ambiguous.
         * Do not accept a successful version probe as recovery: rebuild SCI3
         * and reset the camera on the next poll. */
        p_cam->capture_consecutive_failures = SERIAL_CAMERA_RECOVERY_FAILURE_THRESHOLD;
    }
    else
    {
        p_cam->consecutive_failures++;
    }
    g_camera_pending_recovery_reason = reason;
}

static void serial_camera_uart_cleanup(void)
{
    uint32_t remaining = 0U;

    if (g_camera_uart_open)
    {
        (void) g_camera_uart.p_api->readStop(g_camera_uart.p_ctrl, &remaining);
        (void) g_camera_uart.p_api->communicationAbort(g_camera_uart.p_ctrl, UART_DIR_RX_TX);
    }

    g_camera_uart_rx_complete = false;
    g_camera_uart_tx_complete = false;
    g_camera_uart_error = false;
}

static fsp_err_t serial_camera_uart_close(void)
{
    fsp_err_t err = FSP_SUCCESS;

    if (g_camera_uart_open)
    {
        serial_camera_uart_cleanup();
        err = g_camera_uart.p_api->close(g_camera_uart.p_ctrl);
        if (FSP_SUCCESS == err)
        {
            g_camera_uart_open = false;
        }
    }

    return err;
}

static fsp_err_t serial_camera_uart_open_instance(bool recovery_open)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    fsp_err_t err = g_camera_uart.p_api->open(g_camera_uart.p_ctrl, g_camera_uart.p_cfg);

    if (FSP_ERR_ALREADY_OPEN == err)
    {
        g_camera_uart_open = true;
        /*
         * During first initialization this can be harmless, but in a recovery
         * path it proves the preceding close did not actually rebuild SCI3.
         */
        return recovery_open ? FSP_ERR_IN_USE : FSP_SUCCESS;
    }

    if (FSP_SUCCESS == err)
    {
        g_camera_uart_open = true;
        if (recovery_open)
        {
            p_cam->uart_reopen_count++;
        }
    }

    return err;
}

static fsp_err_t serial_camera_uart_reopen(void)
{
    fsp_err_t err = serial_camera_uart_close();

    if (FSP_SUCCESS != err)
    {
        return err;
    }

    vTaskDelay(SERIAL_CAMERA_UART_REOPEN_DELAY_TICKS);
    return serial_camera_uart_open_instance(true);
}

static bool serial_camera_configure_power_relay(void)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    fsp_err_t cfg_err = R_IOPORT_PinCfg(&g_ioport_ctrl,
                                       SERIAL_CAMERA_POWER_RELAY_PIN,
                                       SERIAL_CAMERA_RELAY_INACTIVE_CFG);
    fsp_err_t write_err = FSP_ERR_NOT_OPEN;

    if (FSP_SUCCESS == cfg_err)
    {
        write_err = R_IOPORT_PinWrite(&g_ioport_ctrl,
                                     SERIAL_CAMERA_POWER_RELAY_PIN,
                                     SERIAL_CAMERA_RELAY_INACTIVE_LEVEL);
    }

    p_cam->power_relay_ready =
        ((FSP_SUCCESS == cfg_err) && (FSP_SUCCESS == write_err)) ? 1U : 0U;
    return 0U != p_cam->power_relay_ready;
}

static bool serial_camera_set_uart_pins_isolated(bool isolated)
{
    uint32_t pin_cfg = isolated ? SERIAL_CAMERA_GPIO_INPUT_CFG :
                                  SERIAL_CAMERA_UART_PERIPHERAL_CFG;
    fsp_err_t rx_err = R_IOPORT_PinCfg(&g_ioport_ctrl,
                                      SERIAL_CAMERA_UART_RX_PIN,
                                      pin_cfg);
    fsp_err_t tx_err = R_IOPORT_PinCfg(&g_ioport_ctrl,
                                      SERIAL_CAMERA_UART_TX_PIN,
                                      pin_cfg);

    return (FSP_SUCCESS == rx_err) && (FSP_SUCCESS == tx_err);
}

static bool serial_camera_set_uart_boot_idle(void)
{
    fsp_err_t rx_err = R_IOPORT_PinCfg(&g_ioport_ctrl,
                                       SERIAL_CAMERA_UART_RX_PIN,
                                       SERIAL_CAMERA_GPIO_INPUT_CFG);
    fsp_err_t tx_err = R_IOPORT_PinCfg(&g_ioport_ctrl,
                                       SERIAL_CAMERA_UART_TX_PIN,
                                       SERIAL_CAMERA_GPIO_IDLE_HIGH_CFG);

    return (FSP_SUCCESS == rx_err) && (FSP_SUCCESS == tx_err);
}

static bool serial_camera_power_cycle_module(void)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    bool pins_isolated;
    fsp_err_t active_err;
    fsp_err_t inactive_err;
    fsp_err_t cleanup_err;
    bool boot_idle_set = false;
    bool pins_restored;

    if (0U == p_cam->power_relay_ready)
    {
        return false;
    }

    /*
     * Closing SCI3 alone leaves TXD3 at UART idle-high.  Isolate both UART
     * pins while 5 V is absent so the camera cannot remain partially powered
     * through its RX protection diode.
     */
    pins_isolated = serial_camera_set_uart_pins_isolated(true);
    active_err = R_IOPORT_PinWrite(&g_ioport_ctrl,
                                   SERIAL_CAMERA_POWER_RELAY_PIN,
                                   SERIAL_CAMERA_RELAY_ACTIVE_LEVEL);
    if (FSP_SUCCESS == active_err)
    {
        p_cam->power_cycle_count++;
        vTaskDelay(SERIAL_CAMERA_POWER_CUT_TICKS);
    }

    inactive_err = R_IOPORT_PinWrite(&g_ioport_ctrl,
                                     SERIAL_CAMERA_POWER_RELAY_PIN,
                                     SERIAL_CAMERA_RELAY_INACTIVE_LEVEL);
    /* Force the fail-safe, normally-powered state even if PinWrite failed. */
    cleanup_err = R_IOPORT_PinCfg(&g_ioport_ctrl,
                                  SERIAL_CAMERA_POWER_RELAY_PIN,
                                  SERIAL_CAMERA_RELAY_INACTIVE_CFG);
    g_camera_last_power_cycle_tick = xTaskGetTickCount();

    if ((FSP_SUCCESS == active_err) &&
        (FSP_SUCCESS == inactive_err) &&
        (FSP_SUCCESS == cleanup_err))
    {
        /* Once 5 V is restored, hold the camera RX input at the UART idle
         * level for its complete boot interval.  Leaving the host TX pin as a
         * floating input here can be decoded as random VC0703 commands. */
        boot_idle_set = serial_camera_set_uart_boot_idle();
        vTaskDelay(SERIAL_CAMERA_POWER_BOOT_TICKS);
    }

    pins_restored = serial_camera_set_uart_pins_isolated(false);
    return pins_isolated &&
           boot_idle_set &&
           pins_restored &&
           (FSP_SUCCESS == active_err) &&
           (FSP_SUCCESS == inactive_err) &&
           (FSP_SUCCESS == cleanup_err);
}

static fsp_err_t serial_camera_exchange(uint8_t const * p_command,
                                        uint32_t command_length,
                                        uint8_t * p_reply,
                                        uint32_t reply_length,
                                        TickType_t timeout_ticks)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    TickType_t start_tick;
    fsp_err_t err;

    if (!g_camera_uart_open)
    {
        return FSP_ERR_NOT_OPEN;
    }

    p_cam->last_command_code = (command_length > 2U) ? p_command[2] : 0U;
    serial_camera_record_reply(p_reply, reply_length, 0U, FSP_ERR_IN_USE);
    g_camera_uart_rx_complete = false;
    g_camera_uart_tx_complete = false;
    g_camera_uart_error = false;

    /*
     * R_SCI_B_UART_Write() waits for TEND with no software timeout.  Do not
     * enter it while the previous transaction is still draining, otherwise a
     * bad peripheral state can monopolize this high-priority camera task.
     */
    start_tick = xTaskGetTickCount();
    while (0U == g_camera_uart_ctrl.p_reg->CSR_b.TEND)
    {
        if ((xTaskGetTickCount() - start_tick) >= SERIAL_CAMERA_TX_IDLE_TIMEOUT_TICKS)
        {
            p_cam->timeout_count++;
            p_cam->last_fsp_err = (uint32_t) FSP_ERR_TIMEOUT;
            serial_camera_record_reply(p_reply, reply_length, 0U, FSP_ERR_TIMEOUT);
            return FSP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    err = g_camera_uart.p_api->read(g_camera_uart.p_ctrl, p_reply, reply_length);
    if (FSP_SUCCESS != err)
    {
        serial_camera_record_reply(p_reply, reply_length, 0U, err);
        p_cam->last_fsp_err = (uint32_t) err;
        return err;
    }

    err = g_camera_uart.p_api->write(g_camera_uart.p_ctrl, p_command, command_length);
    if (FSP_SUCCESS != err)
    {
        (void) g_camera_uart.p_api->communicationAbort(g_camera_uart.p_ctrl, UART_DIR_RX);
        serial_camera_record_reply(p_reply, reply_length, 0U, err);
        p_cam->last_fsp_err = (uint32_t) err;
        return err;
    }

    start_tick = xTaskGetTickCount();
    while (!g_camera_uart_rx_complete && !g_camera_uart_error)
    {
        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks)
        {
            uint32_t remaining = reply_length;
            (void) g_camera_uart.p_api->readStop(g_camera_uart.p_ctrl, &remaining);
            if (remaining > reply_length)
            {
                remaining = reply_length;
            }
            serial_camera_record_reply(p_reply,
                                       reply_length,
                                       reply_length - remaining,
                                       FSP_ERR_TIMEOUT);
            p_cam->timeout_count++;
            p_cam->last_fsp_err = (uint32_t) FSP_ERR_TIMEOUT;
            return FSP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    if (g_camera_uart_error)
    {
        uint32_t remaining = reply_length;
        (void) g_camera_uart.p_api->readStop(g_camera_uart.p_ctrl, &remaining);
        if (remaining > reply_length)
        {
            remaining = reply_length;
        }
        serial_camera_record_reply(p_reply,
                                   reply_length,
                                   reply_length - remaining,
                                   FSP_ERR_ABORTED);
        return FSP_ERR_ABORTED;
    }

    serial_camera_record_reply(p_reply, reply_length, reply_length, FSP_SUCCESS);

    start_tick = xTaskGetTickCount();
    while (!g_camera_uart_tx_complete)
    {
        if ((xTaskGetTickCount() - start_tick) >= SERIAL_CAMERA_COMMAND_TIMEOUT_TICKS)
        {
            p_cam->timeout_count++;
            p_cam->last_fsp_err = (uint32_t) FSP_ERR_TIMEOUT;
            serial_camera_record_reply(p_reply, reply_length, reply_length, FSP_ERR_TIMEOUT);
            return FSP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    p_cam->last_fsp_err = (uint32_t) FSP_SUCCESS;
    return FSP_SUCCESS;
}

static fsp_err_t serial_camera_simple_command(uint8_t const * p_command,
                                              uint32_t command_length,
                                              uint8_t command_code,
                                              TickType_t timeout_ticks)
{
    uint8_t reply[5];
    fsp_err_t err = serial_camera_exchange(p_command,
                                           command_length,
                                           reply,
                                           sizeof(reply),
                                           timeout_ticks);

    if ((FSP_SUCCESS == err) && !serial_camera_reply_ok(reply, sizeof(reply), command_code))
    {
        SERIAL_CAMERA_IPC_MAILBOX->protocol_error_count++;
        err = FSP_ERR_INVALID_DATA;
        serial_camera_mark_stage_error(err);
    }

    return err;
}

static fsp_err_t serial_camera_release_frame_state(void)
{
    static uint8_t const resume_frame[] = {0x56U, 0x00U, 0x36U, 0x01U, 0x02U};
    fsp_err_t err;

    SERIAL_CAMERA_IPC_MAILBOX->capture_stage = SERIAL_CAMERA_CAPTURE_STAGE_IDLE;
    err = serial_camera_simple_command(resume_frame,
                                       sizeof(resume_frame),
                                       0x36U,
                                       SERIAL_CAMERA_COMMAND_TIMEOUT_TICKS);
    if (FSP_SUCCESS == err)
    {
        vTaskDelay(SERIAL_CAMERA_FRAME_RELEASE_TICKS);
    }

    return err;
}

static fsp_err_t serial_camera_recover(serial_camera_recovery_reason_t reason)
{
    static uint8_t const soft_reset_command[] = {0x56U, 0x00U, 0x26U, 0x00U};
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    TickType_t now = xTaskGetTickCount();
    fsp_err_t reset_err = FSP_SUCCESS;
    fsp_err_t final_open_err;
    bool power_cycle_due;

    g_camera_device_ready = false;
    p_cam->status = SERIAL_CAMERA_STATUS_RECOVERING;
    p_cam->last_recovery_reason = (uint32_t) reason;
    p_cam->recovery_count++;
    p_cam->sequence++;

    g_camera_recovery_cycles_without_frame++;
    p_cam->recoveries_without_frame = g_camera_recovery_cycles_without_frame;
    power_cycle_due =
        (0U != p_cam->power_relay_ready) &&
        (g_camera_recovery_cycles_without_frame >=
         SERIAL_CAMERA_RECOVERIES_BEFORE_POWER_CYCLE) &&
        ((0U == g_camera_last_power_cycle_tick) ||
         ((now - g_camera_last_power_cycle_tick) >=
          SERIAL_CAMERA_POWER_CYCLE_COOLDOWN_TICKS));

    if (power_cycle_due)
    {
        reset_err = serial_camera_uart_close();
        if (FSP_SUCCESS == reset_err)
        {
            reset_err = serial_camera_power_cycle_module() ?
                        FSP_SUCCESS : FSP_ERR_ABORTED;
        }

        if (FSP_SUCCESS == reset_err)
        {
            g_camera_recovery_cycles_without_frame = 0U;
            p_cam->recoveries_without_frame = 0U;
        }
    }
    else
    {
        fsp_err_t reopen_err;
        fsp_err_t close_err;

        /* Reset SCI3 first so the reset command is not sent through a transfer
         * state left behind by a partial JPEG transaction. */
        reopen_err = serial_camera_uart_reopen();
        reset_err = reopen_err;
        if (FSP_SUCCESS == reopen_err)
        {
            /* A failed JPEG transaction can leave the module in STOP_FRAME.
             * Release that state before reset; some VC0703 variants otherwise
             * keep answering GET_VERSION while GET_FBUF_LEN remains stuck. */
            (void) serial_camera_release_frame_state();
            p_cam->soft_reset_count++;
            reset_err = serial_camera_simple_command(soft_reset_command,
                                                     sizeof(soft_reset_command),
                                                     0x26U,
                                                     SERIAL_CAMERA_SOFT_RESET_TIMEOUT_TICKS);
        }

        /* A VC0703 may reset before its ACK is completely shifted out.  Keep
         * the error as diagnostics below, but still wait and probe it. */

        close_err = serial_camera_uart_close();
        if (FSP_SUCCESS != close_err)
        {
            p_cam->recovery_error_count++;
        }

        vTaskDelay(SERIAL_CAMERA_SOFT_RESET_BOOT_TICKS);
    }

    final_open_err = serial_camera_uart_open_instance(true);
    if (FSP_SUCCESS == final_open_err)
    {
        /* Normalize the frame-buffer state after either a soft reset or an
         * actual relay power cycle before the next probe/capture attempt. */
        fsp_err_t release_err = serial_camera_release_frame_state();
        if (FSP_SUCCESS != release_err)
        {
            p_cam->recovery_error_count++;
        }
    }
    g_camera_last_recovery_tick = xTaskGetTickCount();
    /* Give the recovered module a full watchdog window to produce its first
     * JPEG.  Keeping the pre-failure success timestamp makes the watchdog fire
     * immediately after a successful probe and traps the driver in an endless
     * probe/reset/power-cycle loop. */
    g_camera_last_success_tick = g_camera_last_recovery_tick;
    g_camera_last_probe_tick = g_camera_last_recovery_tick - SERIAL_CAMERA_PROBE_PERIOD_TICKS;
    p_cam->consecutive_failures = 0U;
    p_cam->capture_consecutive_failures = 0U;
    g_camera_pending_recovery_reason = SERIAL_CAMERA_RECOVERY_NONE;

    if (FSP_SUCCESS == final_open_err)
    {
        p_cam->status = SERIAL_CAMERA_STATUS_PROBING;
        p_cam->last_fsp_err = (uint32_t) reset_err;
        if (FSP_SUCCESS != reset_err)
        {
            p_cam->recovery_error_count++;
        }
    }
    else
    {
        p_cam->status = SERIAL_CAMERA_STATUS_UART_ERROR;
        p_cam->last_fsp_err = (uint32_t) final_open_err;
        p_cam->recovery_error_count++;
        p_cam->consecutive_failures = SERIAL_CAMERA_RECOVERY_FAILURE_THRESHOLD;
        g_camera_pending_recovery_reason = SERIAL_CAMERA_RECOVERY_UART_FAILURE;
    }

    p_cam->sequence++;
    return final_open_err;
}

static fsp_err_t serial_camera_get_image_size(uint32_t * p_image_size)
{
    static uint8_t const command[] = {0x56U, 0x00U, 0x34U, 0x01U, 0x00U};
    uint8_t reply[9];
    fsp_err_t err = serial_camera_exchange(command,
                                           sizeof(command),
                                           reply,
                                           sizeof(reply),
                                           SERIAL_CAMERA_COMMAND_TIMEOUT_TICKS);

    if (FSP_SUCCESS != err)
    {
        return err;
    }

    if (!serial_camera_reply_ok(reply, sizeof(reply), 0x34U) || (0x04U != reply[4]))
    {
        SERIAL_CAMERA_IPC_MAILBOX->protocol_error_count++;
        serial_camera_mark_stage_error(FSP_ERR_INVALID_DATA);
        return FSP_ERR_INVALID_DATA;
    }

    *p_image_size = serial_camera_get_be32(&reply[5]);
    if ((*p_image_size < 2U) || (*p_image_size > SERIAL_CAMERA_MAX_JPEG_BYTES))
    {
        SERIAL_CAMERA_IPC_MAILBOX->protocol_error_count++;
        serial_camera_mark_stage_error(FSP_ERR_INVALID_SIZE);
        return FSP_ERR_INVALID_SIZE;
    }

    return FSP_SUCCESS;
}

static fsp_err_t serial_camera_read_image_chunk(uint32_t offset, uint32_t length)
{
    uint8_t command[16] = {0x56U, 0x00U, 0x32U, 0x0CU, 0x00U, 0x0CU};
    uint32_t reply_length = length + SERIAL_CAMERA_REPLY_OVERHEAD;
    fsp_err_t err;

    serial_camera_put_be32(&command[6], offset);
    serial_camera_put_be32(&command[10], length);
    command[14] = 0x00U;
    command[15] = 0xFFU;

    err = serial_camera_exchange(command,
                                 sizeof(command),
                                 g_camera_chunk_reply,
                                 reply_length,
                                 SERIAL_CAMERA_CHUNK_TIMEOUT_TICKS);

    if (FSP_SUCCESS != err)
    {
        return err;
    }

    if (!serial_camera_reply_ok(g_camera_chunk_reply, 5U, 0x32U) ||
        !serial_camera_reply_ok(&g_camera_chunk_reply[5U + length], 5U, 0x32U))
    {
        SERIAL_CAMERA_IPC_MAILBOX->protocol_error_count++;
        serial_camera_mark_stage_error(FSP_ERR_INVALID_DATA);
        return FSP_ERR_INVALID_DATA;
    }

    return FSP_SUCCESS;
}

static fsp_err_t serial_camera_publish_chunk(uint32_t frame_sequence,
                                             uint32_t image_size,
                                             uint32_t offset,
                                             uint32_t length)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    uint32_t chunk_sequence = p_cam->chunk_sequence + 1U;
    TickType_t start_tick;

    for (uint32_t i = 0U; i < length; i++)
    {
        p_cam->chunk[i] = g_camera_chunk_reply[5U + i];
    }

    p_cam->frame_sequence = frame_sequence;
    p_cam->image_size = image_size;
    p_cam->image_offset = offset;
    p_cam->chunk_bytes = length;
    __DMB();
    p_cam->chunk_sequence = chunk_sequence;
    p_cam->sequence++;

    start_tick = xTaskGetTickCount();
    while (p_cam->chunk_ack_sequence != chunk_sequence)
    {
        if ((xTaskGetTickCount() - start_tick) >= SERIAL_CAMERA_WRITER_TIMEOUT_TICKS)
        {
            p_cam->timeout_count++;
            return FSP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    if (SERIAL_CAMERA_WRITER_WRITE_ERROR == p_cam->writer_status ||
        SERIAL_CAMERA_WRITER_OPEN_ERROR == p_cam->writer_status ||
        SERIAL_CAMERA_WRITER_NO_SD == p_cam->writer_status)
    {
        return FSP_ERR_WRITE_FAILED;
    }

    return FSP_SUCCESS;
}

void camera_uart_callback(uart_callback_args_t * p_args)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;

    if (NULL == p_args)
    {
        return;
    }

    p_cam->uart_callback_count++;
    p_cam->last_event = (uint32_t) p_args->event;

    switch (p_args->event)
    {
        case UART_EVENT_RX_COMPLETE:
            g_camera_uart_rx_complete = true;
            break;

        case UART_EVENT_TX_COMPLETE:
            g_camera_uart_tx_complete = true;
            break;

        case UART_EVENT_ERR_PARITY:
        case UART_EVENT_ERR_FRAMING:
        case UART_EVENT_ERR_OVERFLOW:
            p_cam->uart_error_count++;
            g_camera_uart_error = true;
            break;

        default:
            break;
    }
}

fsp_err_t serial_camera_driver_init(void)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    fsp_err_t err;

    memset((void *) p_cam, 0, sizeof(*p_cam));
    p_cam->magic = SERIAL_CAMERA_IPC_MAGIC;
    p_cam->status = SERIAL_CAMERA_STATUS_INIT;
    p_cam->capture_interval_ms = SERIAL_CAMERA_DEFAULT_INTERVAL_MS;
    p_cam->writer_status = SERIAL_CAMERA_WRITER_IDLE;
    p_cam->driver_build = SERIAL_CAMERA_DRIVER_BUILD;
    p_cam->sequence++;

    g_camera_last_power_cycle_tick = 0U;
    g_camera_recovery_cycles_without_frame = 0U;
    if (!serial_camera_configure_power_relay())
    {
        p_cam->recovery_error_count++;
    }
    else if (!serial_camera_power_cycle_module())
    {
        p_cam->recovery_error_count++;
    }

    err = serial_camera_uart_open_instance(false);
    p_cam->last_fsp_err = (uint32_t) err;
    if (FSP_SUCCESS == err)
    {
        TickType_t now = xTaskGetTickCount();
        g_camera_last_probe_tick = now - SERIAL_CAMERA_PROBE_PERIOD_TICKS;
        g_camera_last_capture_tick = now;
        g_camera_last_recovery_tick = now - SERIAL_CAMERA_RECOVERY_COOLDOWN_TICKS;
        g_camera_pending_recovery_reason = SERIAL_CAMERA_RECOVERY_NONE;
    }
    else
    {
        p_cam->status = SERIAL_CAMERA_STATUS_UART_ERROR;
    }

    return err;
}

fsp_err_t serial_camera_probe(void)
{
    static uint8_t const command[] = {0x56U, 0x00U, 0x11U, 0x00U};
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    uint8_t reply[16];
    uint32_t version_length;
    fsp_err_t err;

    p_cam->status = SERIAL_CAMERA_STATUS_PROBING;
    p_cam->probe_count++;
    err = serial_camera_exchange(command,
                                 sizeof(command),
                                 reply,
                                 sizeof(reply),
                                 SERIAL_CAMERA_COMMAND_TIMEOUT_TICKS);
    p_cam->last_fsp_err = (uint32_t) err;
    if ((FSP_SUCCESS != err) || !serial_camera_reply_ok(reply, sizeof(reply), 0x11U))
    {
        if (FSP_SUCCESS == err)
        {
            p_cam->protocol_error_count++;
            err = FSP_ERR_INVALID_DATA;
        }
        g_camera_device_ready = false;
        p_cam->status = SERIAL_CAMERA_STATUS_CAMERA_ERROR;
        serial_camera_note_failure(SERIAL_CAMERA_RECOVERY_PROBE_FAILURE);
        return err;
    }

    memset((void *) p_cam->version, 0, sizeof(p_cam->version));
    version_length = reply[4];
    if (version_length > (sizeof(p_cam->version) - 1U))
    {
        version_length = sizeof(p_cam->version) - 1U;
    }
    if (version_length > (sizeof(reply) - 5U))
    {
        version_length = sizeof(reply) - 5U;
    }
    for (uint32_t i = 0U; i < version_length; i++)
    {
        p_cam->version[i] = (char) reply[5U + i];
    }

    g_camera_device_ready = true;
    /* A version reply proves the UART is alive but not the JPEG path. */
    p_cam->consecutive_failures = 0U;
    if (0U == p_cam->capture_consecutive_failures)
    {
        g_camera_pending_recovery_reason = SERIAL_CAMERA_RECOVERY_NONE;
    }
    p_cam->probe_ok_count++;
    p_cam->status = SERIAL_CAMERA_STATUS_READY;
    p_cam->last_fsp_err = (uint32_t) FSP_SUCCESS;
    p_cam->sequence++;
    return FSP_SUCCESS;
}

static fsp_err_t serial_camera_capture(bool publish_to_writer)
{
    static uint8_t const stop_frame[] = {0x56U, 0x00U, 0x36U, 0x01U, 0x00U};
    static uint8_t const resume_frame[] = {0x56U, 0x00U, 0x36U, 0x01U, 0x02U};
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    uint32_t image_size = 0U;
    uint32_t frame_sequence;
    uint32_t offset = 0U;
    uint8_t previous_byte = 0U;
    fsp_err_t err;
    fsp_err_t writer_err = FSP_SUCCESS;
    bool vision_capture_active = false;
    bool frame_stopped = false;

    if (!g_camera_device_ready)
    {
        return FSP_ERR_NOT_OPEN;
    }

    p_cam->status = SERIAL_CAMERA_STATUS_CAPTURING;
    p_cam->jpeg_soi_ok = 0U;
    p_cam->jpeg_eoi_ok = 0U;
    p_cam->last_failed_stage = SERIAL_CAMERA_CAPTURE_STAGE_IDLE;
    memset((void *) p_cam->stage_response, 0, sizeof(p_cam->stage_response));
    frame_sequence = p_cam->frame_sequence + 1U;
    p_cam->frame_sequence = frame_sequence;

    p_cam->capture_stage = SERIAL_CAMERA_CAPTURE_STAGE_STOP_FRAME;
    err = serial_camera_simple_command(stop_frame,
                                       sizeof(stop_frame),
                                       0x36U,
                                       SERIAL_CAMERA_FRAME_COMMAND_TIMEOUT_TICKS);
    if (FSP_SUCCESS == err)
    {
        frame_stopped = true;
        vTaskDelay(SERIAL_CAMERA_FRAME_SETTLE_TICKS);
        p_cam->capture_stage = SERIAL_CAMERA_CAPTURE_STAGE_GET_SIZE;
        err = serial_camera_get_image_size(&image_size);
        if (FSP_SUCCESS == err)
        {
            /*
             * Mirror the serial JPEG into a CPU0 SDRAM buffer.  This path is
             * intentionally independent of the CPU1 SD writer: model preview
             * can still update when the card is absent or its writer stalls.
             */
            vision_capture_active = vision_runtime_capture_begin(frame_sequence, image_size);
        }
    }
    if (FSP_SUCCESS != err)
    {
        p_cam->last_failed_stage = p_cam->capture_stage;
    }

    while ((FSP_SUCCESS == err) && (offset < image_size))
    {
        uint32_t chunk_length = image_size - offset;
        /*
         * The SCI3 receive transfer can move at most 1024 bytes per request.
         * A VC0703 read reply adds ten protocol bytes around the JPEG payload,
         * so requesting a 1024-byte payload always truncates the reply at 1024.
         */
        if (chunk_length > SERIAL_CAMERA_WIRE_CHUNK_BYTES)
        {
            chunk_length = SERIAL_CAMERA_WIRE_CHUNK_BYTES;
        }

        p_cam->capture_stage = SERIAL_CAMERA_CAPTURE_STAGE_READ_CHUNK;
        err = serial_camera_read_image_chunk(offset, chunk_length);
        if (FSP_SUCCESS != err)
        {
            p_cam->last_failed_stage = p_cam->capture_stage;
            break;
        }

        if (0U == offset)
        {
            p_cam->jpeg_soi_ok = ((0xFFU == g_camera_chunk_reply[5]) &&
                                  (0xD8U == g_camera_chunk_reply[6])) ? 1U : 0U;
        }
        for (uint32_t i = 0U; i < chunk_length; i++)
        {
            uint8_t const current_byte = g_camera_chunk_reply[5U + i];
            if ((0xFFU == previous_byte) && (0xD9U == current_byte))
            {
                /* Some VC0703 firmware revisions include alignment bytes
                 * after EOI in the reported frame-buffer length. */
                p_cam->jpeg_eoi_ok = 1U;
            }
            previous_byte = current_byte;
        }

        if (vision_capture_active &&
            !vision_runtime_capture_chunk(offset,
                                          &g_camera_chunk_reply[5U],
                                          chunk_length))
        {
            vision_runtime_capture_abort();
            vision_capture_active = false;
        }

        if (vision_capture_active &&
            ((offset + chunk_length) == image_size) &&
            (0U != p_cam->jpeg_soi_ok) &&
            (0U != p_cam->jpeg_eoi_ok))
        {
            vision_runtime_capture_finish(true);
            vision_capture_active = false;
        }

        if (publish_to_writer)
        {
            p_cam->capture_stage = SERIAL_CAMERA_CAPTURE_STAGE_PUBLISH_CHUNK;
            fsp_err_t publish_err = serial_camera_publish_chunk(frame_sequence,
                                                                image_size,
                                                                offset,
                                                                chunk_length);
            if (FSP_SUCCESS != publish_err)
            {
                writer_err = publish_err;
                p_cam->last_failed_stage = p_cam->capture_stage;
                /*
                 * CPU1 has already acknowledged and cleaned a failed partial
                 * file.  Stop publishing this frame, but finish reading it so
                 * camera health and the CPU0 vision path remain independent of
                 * SD-card health.
                 */
                publish_to_writer = false;
            }
        }
        offset += chunk_length;
    }

    if (vision_capture_active)
    {
        vision_runtime_capture_abort();
        vision_capture_active = false;
    }

    if (frame_stopped)
    {
        p_cam->capture_stage = SERIAL_CAMERA_CAPTURE_STAGE_RESUME_FRAME;
        fsp_err_t resume_err = serial_camera_simple_command(resume_frame,
                                                            sizeof(resume_frame),
                                                            0x36U,
                                                            SERIAL_CAMERA_FRAME_COMMAND_TIMEOUT_TICKS);
        if ((FSP_SUCCESS == err) && (FSP_SUCCESS != resume_err))
        {
            p_cam->last_failed_stage = p_cam->capture_stage;
            err = resume_err;
        }
    }

    if (FSP_SUCCESS != err)
    {
        p_cam->capture_error_count++;
        p_cam->status = SERIAL_CAMERA_STATUS_CAMERA_ERROR;
        g_camera_device_ready = false;
        serial_camera_note_failure(SERIAL_CAMERA_RECOVERY_CAPTURE_FAILURE);
    }
    else if (FSP_SUCCESS != writer_err)
    {
        /* The camera answered correctly; only the CPU1 SD writer stalled.
         * Keep the device ready so the next scheduled frame retries without a
         * needless camera reprobe or another chain of per-chunk timeouts. */
        p_cam->capture_error_count++;
        p_cam->status = SERIAL_CAMERA_STATUS_READY;
        if ((0U != p_cam->jpeg_soi_ok) && (0U != p_cam->jpeg_eoi_ok))
        {
            p_cam->capture_count++;
            g_camera_last_success_tick = xTaskGetTickCount();
            g_camera_recovery_cycles_without_frame = 0U;
            p_cam->recoveries_without_frame = 0U;
            p_cam->consecutive_failures = 0U;
            p_cam->capture_consecutive_failures = 0U;
            g_camera_pending_recovery_reason = SERIAL_CAMERA_RECOVERY_NONE;
        }
        err = writer_err;
    }
    else if ((0U != p_cam->jpeg_soi_ok) && (0U != p_cam->jpeg_eoi_ok))
    {
        p_cam->capture_count++;
        g_camera_last_success_tick = xTaskGetTickCount();
        g_camera_recovery_cycles_without_frame = 0U;
        p_cam->recoveries_without_frame = 0U;
        p_cam->consecutive_failures = 0U;
        p_cam->capture_consecutive_failures = 0U;
        g_camera_pending_recovery_reason = SERIAL_CAMERA_RECOVERY_NONE;
        p_cam->status = SERIAL_CAMERA_STATUS_READY;
    }
    else
    {
        p_cam->capture_error_count++;
        p_cam->status = SERIAL_CAMERA_STATUS_CAMERA_ERROR;
        g_camera_device_ready = false;
        serial_camera_note_failure(SERIAL_CAMERA_RECOVERY_CAPTURE_FAILURE);
        if (FSP_SUCCESS == err)
        {
            err = FSP_ERR_INVALID_DATA;
            p_cam->last_failed_stage = SERIAL_CAMERA_CAPTURE_STAGE_READ_CHUNK;
            p_cam->stage_response[SERIAL_CAMERA_CAPTURE_STAGE_READ_CHUNK].fsp_err = (uint32_t) err;
        }
    }

    p_cam->last_fsp_err = (uint32_t) err;
    p_cam->capture_stage = SERIAL_CAMERA_CAPTURE_STAGE_IDLE;
    p_cam->sequence++;
    return err;
}

fsp_err_t serial_camera_capture_once(void)
{
    return serial_camera_capture(true);
}

void serial_camera_poll(void)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    TickType_t now = xTaskGetTickCount();

    if (p_cam->command_ack_sequence != p_cam->command_sequence)
    {
        uint32_t command = p_cam->command;
        p_cam->command = SERIAL_CAMERA_COMMAND_NONE;

        if (SERIAL_CAMERA_COMMAND_RECORD_START == command)
        {
            p_cam->capture_active = 1U;
        }
        else if (SERIAL_CAMERA_COMMAND_RECORD_STOP == command)
        {
            p_cam->capture_active = 0U;
        }
        else if (SERIAL_CAMERA_COMMAND_CAPTURE_ONCE == command)
        {
            p_cam->capture_once = 1U;
        }

        p_cam->command_ack_sequence = p_cam->command_sequence;
        p_cam->sequence++;
    }

    if (!g_camera_device_ready)
    {
        if (((p_cam->consecutive_failures >= SERIAL_CAMERA_RECOVERY_FAILURE_THRESHOLD) ||
             (p_cam->capture_consecutive_failures >=
              SERIAL_CAMERA_RECOVERY_FAILURE_THRESHOLD)) &&
            ((now - g_camera_last_recovery_tick) >= SERIAL_CAMERA_RECOVERY_COOLDOWN_TICKS))
        {
            serial_camera_recovery_reason_t reason = g_camera_pending_recovery_reason;
            if (SERIAL_CAMERA_RECOVERY_NONE == reason)
            {
                reason = SERIAL_CAMERA_RECOVERY_UART_FAILURE;
            }
            (void) serial_camera_recover(reason);
            return;
        }

        if ((now - g_camera_last_probe_tick) >= SERIAL_CAMERA_PROBE_PERIOD_TICKS)
        {
            g_camera_last_probe_tick = now;
            (void) serial_camera_probe();
        }
        return;
    }

    if ((0U != g_camera_last_success_tick) &&
        ((now - g_camera_last_success_tick) >= SERIAL_CAMERA_SUCCESS_WATCHDOG_TICKS))
    {
        /* A UART can remain nominally open after the camera stops producing
         * complete JPEG frames.  Drop readiness so the normal two-second
         * probe path re-synchronizes the VC0703 without a board reset. */
        g_camera_device_ready = false;
        g_camera_last_probe_tick = now - SERIAL_CAMERA_PROBE_PERIOD_TICKS;
        p_cam->status = SERIAL_CAMERA_STATUS_PROBING;
        p_cam->capture_consecutive_failures =
            SERIAL_CAMERA_RECOVERY_FAILURE_THRESHOLD;
        g_camera_pending_recovery_reason = SERIAL_CAMERA_RECOVERY_WATCHDOG;
        p_cam->sequence++;
        return;
    }

    uint32_t interval_ms = p_cam->capture_interval_ms;
    if (interval_ms < SERIAL_CAMERA_MIN_INTERVAL_MS)
    {
        interval_ms = SERIAL_CAMERA_MIN_INTERVAL_MS;
    }

    bool capture_requested = (0U != p_cam->capture_once);
    bool record_due = (0U != p_cam->capture_active) &&
                       ((now - g_camera_last_capture_tick) >= pdMS_TO_TICKS(interval_ms));
    bool inference_due = (0U != SERIAL_CAMERA_AUTO_INFERENCE_ENABLED) &&
                         ((now - g_camera_last_capture_tick) >=
                          pdMS_TO_TICKS(SERIAL_CAMERA_AUTO_INFERENCE_INTERVAL_MS));

    if (capture_requested || record_due || inference_due)
    {
        p_cam->capture_once = 0U;
        (void) serial_camera_capture(capture_requested || (0U != p_cam->capture_active));
        /* Enforce an idle interval after the complete JPEG transaction.  At
         * 38400 baud the transfer itself can exceed one second; measuring from
         * its start otherwise launches the next capture immediately. */
        g_camera_last_capture_tick = xTaskGetTickCount();
    }
}

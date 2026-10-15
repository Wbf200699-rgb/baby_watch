#include "thread_code/tmp117_matrix_driver.h"

#define TMP117_DEBUG_MAGIC      (0x544D5044U) /* TMPD */
#define TMP117_BUILD_MARKER     (0x544D5003U)
#define PCA9548A_ADDR           (0x70U)
#define TMP117_USE_PCA9548A     (1U)
#define TMP117_ADDR_FIRST       (0x48U)
#define TMP117_ADDR_LAST        (0x4BU)
#define TMP117_TEMP_REG         (0x00U)
#define TMP117_I2C_TIMEOUT_MS   (50U)
#define TMP117_I2C_RECOVERY_DELAY_MS  (2U)
#define TMP117_I2C_BUS_CLEAR_PULSES   (9U)
#define TMP117_I2C_BUS_CLEAR_DELAY_US (5U)
#define TMP117_CHANNEL_ATTEMPTS       (2U)
#define TMP117_CHANNEL_FAILURE_TOLERANCE (3U)
#define TMP117_I2C_SDA_PIN            BSP_IO_PORT_05_PIN_14
#define TMP117_I2C_SCL_PIN            BSP_IO_PORT_05_PIN_15
#define TMP117_I2C_RELEASE_CFG         ((uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT | \
                                        (uint32_t) IOPORT_CFG_PULLUP_ENABLE)
#define TMP117_I2C_LOW_CFG             ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                        (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW | \
                                        (uint32_t) IOPORT_CFG_NMOS_ENABLE)
#define TMP117_I2C_PERIPHERAL_CFG      ((uint32_t) IOPORT_CFG_DRIVE_MID | \
                                        (uint32_t) IOPORT_CFG_PERIPHERAL_PIN | \
                                        (uint32_t) IOPORT_PERIPHERAL_IIC)

static const uint8_t g_tmp117_mux_channel[TMP117_IPC_SENSOR_COUNT] = {0U, 1U, 2U, 3U, 6U};

typedef struct st_tmp117_i2c_context
{
    volatile bool busy;
    volatile i2c_master_event_t event;
} tmp117_i2c_context_t;

volatile tmp117_matrix_debug_t g_tmp117_debug;
const volatile uint32_t g_tmp117_build_marker = TMP117_BUILD_MARKER;

static tmp117_i2c_context_t g_tmp117_i2c_context;
static bool g_tmp117_opened;
static uint8_t g_tmp117_failure_streak[TMP117_MATRIX_SENSOR_COUNT];

static int32_t tmp117_raw_to_milli_c(int16_t raw)
{
    return ((int32_t) raw * 125) / 16;
}

static void tmp117_debug_reset(void)
{
    g_tmp117_debug.magic = TMP117_DEBUG_MAGIC;
    g_tmp117_debug.build_marker = g_tmp117_build_marker;
    g_tmp117_debug.sequence = 0U;
    g_tmp117_debug.step = TMP117_MATRIX_STEP_NONE;
    g_tmp117_debug.channel = 0U;
    g_tmp117_debug.connected_mask = 0U;
    g_tmp117_debug.error_mask = 0U;
    g_tmp117_debug.fail_step = TMP117_MATRIX_STEP_NONE;
    g_tmp117_debug.fail_channel = 0U;
    g_tmp117_debug.pca_address = 0U;
    g_tmp117_debug.last_fsp_err = FSP_SUCCESS;
    g_tmp117_debug.recovery_count = 0U;
    g_tmp117_debug.recovery_error_count = 0U;
    g_tmp117_debug.retry_success_count = 0U;

    for (uint32_t i = 0U; i < TMP117_MATRIX_SENSOR_COUNT; i++)
    {
        g_tmp117_failure_streak[i] = 0U;
        g_tmp117_debug.channel_err[i] = FSP_SUCCESS;
        g_tmp117_debug.channel_step[i] = TMP117_MATRIX_STEP_NONE;
        g_tmp117_debug.address[i] = 0U;
        g_tmp117_debug.raw[i] = 0;
        g_tmp117_debug.milli_c[i] = 0;
    }
}

static void tmp117_publish(uint32_t connected_mask, uint32_t error_mask, fsp_err_t err)
{
    volatile tmp117_ipc_mailbox_t * p_mailbox = TMP117_IPC_MAILBOX;

    p_mailbox->magic = TMP117_IPC_MAGIC;
    p_mailbox->connected_mask = connected_mask;
    p_mailbox->error_mask = error_mask;
    p_mailbox->fail_step = (uint32_t) g_tmp117_debug.fail_step;
    p_mailbox->fail_channel = g_tmp117_debug.fail_channel;
    p_mailbox->pca_address = g_tmp117_debug.pca_address;
    p_mailbox->last_fsp_err = (uint32_t) err;

    if (0U == connected_mask)
    {
        p_mailbox->status = (uint32_t) TMP117_IPC_STATUS_SCAN_ERROR;
    }
    else if (0U != error_mask)
    {
        p_mailbox->status = (uint32_t) TMP117_IPC_STATUS_SCAN_PARTIAL;
    }
    else
    {
        p_mailbox->status = (uint32_t) TMP117_IPC_STATUS_SCAN_OK;
    }

    for (uint32_t i = 0U; i < TMP117_MATRIX_SENSOR_COUNT; i++)
    {
        p_mailbox->address[i] = g_tmp117_debug.address[i];
        p_mailbox->raw[i] = g_tmp117_debug.raw[i];
        p_mailbox->milli_c[i] = g_tmp117_debug.milli_c[i];
    }

    __DMB();
    p_mailbox->sequence++;
}

static fsp_err_t tmp117_i2c_wait(i2c_master_event_t expected_event)
{
    TickType_t start_tick = xTaskGetTickCount();

    while (g_tmp117_i2c_context.busy)
    {
        if ((xTaskGetTickCount() - start_tick) >= pdMS_TO_TICKS(TMP117_I2C_TIMEOUT_MS))
        {
            (void) g_temp_i2c.p_api->abort(g_temp_i2c.p_ctrl);
            g_tmp117_i2c_context.busy = false;
            g_tmp117_debug.last_fsp_err = FSP_ERR_TIMEOUT;
            return FSP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (expected_event != g_tmp117_i2c_context.event)
    {
        g_tmp117_debug.last_fsp_err = FSP_ERR_ABORTED;
        return FSP_ERR_ABORTED;
    }

    return FSP_SUCCESS;
}

static void tmp117_i2c_bus_clear_delay(void)
{
    R_BSP_SoftwareDelay(TMP117_I2C_BUS_CLEAR_DELAY_US, BSP_DELAY_UNITS_MICROSECONDS);
}

static fsp_err_t tmp117_i2c_bus_clear(void)
{
    fsp_err_t err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, TMP117_I2C_SDA_PIN, TMP117_I2C_RELEASE_CFG);
    if (FSP_SUCCESS == err)
    {
        err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, TMP117_I2C_SCL_PIN, TMP117_I2C_RELEASE_CFG);
    }

    if (FSP_SUCCESS == err)
    {
        tmp117_i2c_bus_clear_delay();

        for (uint32_t pulse = 0U; pulse < TMP117_I2C_BUS_CLEAR_PULSES; pulse++)
        {
            err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, TMP117_I2C_SCL_PIN, TMP117_I2C_LOW_CFG);
            if (FSP_SUCCESS != err)
            {
                break;
            }
            tmp117_i2c_bus_clear_delay();

            err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, TMP117_I2C_SCL_PIN, TMP117_I2C_RELEASE_CFG);
            if (FSP_SUCCESS != err)
            {
                break;
            }
            tmp117_i2c_bus_clear_delay();
        }
    }

    if (FSP_SUCCESS == err)
    {
        /* Generate a STOP after clocking a slave out of an interrupted byte. */
        err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, TMP117_I2C_SDA_PIN, TMP117_I2C_LOW_CFG);
        tmp117_i2c_bus_clear_delay();
    }
    if (FSP_SUCCESS == err)
    {
        err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, TMP117_I2C_SCL_PIN, TMP117_I2C_RELEASE_CFG);
        tmp117_i2c_bus_clear_delay();
    }
    if (FSP_SUCCESS == err)
    {
        err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, TMP117_I2C_SDA_PIN, TMP117_I2C_RELEASE_CFG);
        tmp117_i2c_bus_clear_delay();
    }

    fsp_err_t restore_sda_err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                                       TMP117_I2C_SDA_PIN,
                                                       TMP117_I2C_PERIPHERAL_CFG);
    fsp_err_t restore_scl_err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                                       TMP117_I2C_SCL_PIN,
                                                       TMP117_I2C_PERIPHERAL_CFG);

    if (FSP_SUCCESS == err)
    {
        err = restore_sda_err;
    }
    if (FSP_SUCCESS == err)
    {
        err = restore_scl_err;
    }

    return err;
}

static fsp_err_t tmp117_i2c_recover(void)
{
    fsp_err_t err = FSP_SUCCESS;

    g_tmp117_debug.recovery_count++;
    g_tmp117_i2c_context.busy = false;
    g_tmp117_i2c_context.event = (i2c_master_event_t) 0;

    if (g_tmp117_opened)
    {
        err = g_temp_i2c.p_api->abort(g_temp_i2c.p_ctrl);
        if ((FSP_SUCCESS != err) && (FSP_ERR_NOT_OPEN != err))
        {
            goto recovery_failed;
        }
        vTaskDelay(pdMS_TO_TICKS(TMP117_I2C_RECOVERY_DELAY_MS));

        err = g_temp_i2c.p_api->close(g_temp_i2c.p_ctrl);
        if ((FSP_SUCCESS != err) && (FSP_ERR_NOT_OPEN != err))
        {
            goto recovery_failed;
        }
        g_tmp117_opened = false;
        vTaskDelay(pdMS_TO_TICKS(TMP117_I2C_RECOVERY_DELAY_MS));
    }

    err = tmp117_i2c_bus_clear();
    if (FSP_SUCCESS != err)
    {
        goto recovery_failed;
    }

    err = g_temp_i2c.p_api->open(g_temp_i2c.p_ctrl, g_temp_i2c.p_cfg);
    if (FSP_ERR_ALREADY_OPEN == err)
    {
        err = FSP_SUCCESS;
    }
    if (FSP_SUCCESS != err)
    {
        goto recovery_failed;
    }

    g_tmp117_opened = true;
    g_tmp117_debug.last_fsp_err = FSP_SUCCESS;
    return FSP_SUCCESS;

recovery_failed:
    g_tmp117_opened = false;
    g_tmp117_debug.recovery_error_count++;
    g_tmp117_debug.last_fsp_err = err;
    return err;
}

static fsp_err_t tmp117_i2c_set_address(uint32_t address)
{
    fsp_err_t err = g_temp_i2c.p_api->slaveAddressSet(g_temp_i2c.p_ctrl, address, I2C_MASTER_ADDR_MODE_7BIT);
    g_tmp117_debug.last_fsp_err = err;
    return err;
}

static fsp_err_t tmp117_i2c_write(uint8_t * p_data, uint32_t length, bool restart)
{
    fsp_err_t err;

    g_tmp117_i2c_context.event = (i2c_master_event_t) 0;
    g_tmp117_i2c_context.busy = true;
    err = g_temp_i2c.p_api->write(g_temp_i2c.p_ctrl, p_data, length, restart);
    if (FSP_SUCCESS != err)
    {
        g_tmp117_i2c_context.busy = false;
        g_tmp117_debug.last_fsp_err = err;
        return err;
    }

    return tmp117_i2c_wait(I2C_MASTER_EVENT_TX_COMPLETE);
}

static fsp_err_t tmp117_i2c_read(uint8_t * p_data, uint32_t length, bool restart)
{
    fsp_err_t err;

    g_tmp117_i2c_context.event = (i2c_master_event_t) 0;
    g_tmp117_i2c_context.busy = true;
    err = g_temp_i2c.p_api->read(g_temp_i2c.p_ctrl, p_data, length, restart);
    if (FSP_SUCCESS != err)
    {
        g_tmp117_i2c_context.busy = false;
        g_tmp117_debug.last_fsp_err = err;
        return err;
    }

    return tmp117_i2c_wait(I2C_MASTER_EVENT_RX_COMPLETE);
}

static fsp_err_t tmp117_select_channel(uint32_t channel)
{
#if TMP117_USE_PCA9548A
    uint8_t select = (uint8_t) (1U << channel);
    fsp_err_t err;

    g_tmp117_debug.step = TMP117_MATRIX_STEP_SELECT_MUX;
    g_tmp117_debug.channel = channel;

    err = tmp117_i2c_set_address(PCA9548A_ADDR);
    if (FSP_SUCCESS == err)
    {
        err = tmp117_i2c_write(&select, 1U, false);
    }

    if (FSP_SUCCESS == err)
    {
        g_tmp117_debug.pca_address = PCA9548A_ADDR;
    }

    return err;
#else
    FSP_PARAMETER_NOT_USED(channel);

    g_tmp117_debug.step = TMP117_MATRIX_STEP_SELECT_MUX;
    g_tmp117_debug.channel = 0U;
    g_tmp117_debug.pca_address = 0U;

    return FSP_SUCCESS;
#endif
}

static fsp_err_t tmp117_read_channel(uint32_t channel, uint32_t address, int16_t * p_raw, int32_t * p_milli_c)
{
    uint8_t reg = TMP117_TEMP_REG;
    uint8_t data[2] = {0U, 0U};
    fsp_err_t err;

    g_tmp117_debug.step = TMP117_MATRIX_STEP_READ_TEMP;
    g_tmp117_debug.channel = channel;

    err = tmp117_i2c_set_address(address);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /*
     * Use a STOP between the register pointer write and the data read. This is
     * more conservative with the FSP non-blocking IIC state machine than a
     * repeated-start sequence, and TMP117 keeps the register pointer.
     */
    err = tmp117_i2c_write(&reg, 1U, false);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(1));

    err = tmp117_i2c_read(data, 2U, false);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    *p_raw = (int16_t) (((uint16_t) data[0] << 8) | data[1]);
    *p_milli_c = tmp117_raw_to_milli_c(*p_raw);

    return FSP_SUCCESS;
}

fsp_err_t tmp117_matrix_init(void)
{
    fsp_err_t err;
    volatile tmp117_ipc_mailbox_t * p_mailbox = TMP117_IPC_MAILBOX;

    pressure_i2c_bus_init();
    tmp117_debug_reset();
    p_mailbox->magic = TMP117_IPC_MAGIC;
    p_mailbox->status = (uint32_t) TMP117_IPC_STATUS_EMPTY;
    p_mailbox->sequence = 0U;

    if (g_tmp117_opened)
    {
        return FSP_SUCCESS;
    }

    pressure_i2c_bus_lock();

    g_tmp117_debug.step = TMP117_MATRIX_STEP_OPEN;
    err = g_temp_i2c.p_api->open(g_temp_i2c.p_ctrl, g_temp_i2c.p_cfg);
    if (FSP_ERR_ALREADY_OPEN == err)
    {
        err = FSP_SUCCESS;
    }
    g_tmp117_debug.last_fsp_err = err;
    if (FSP_SUCCESS != err)
    {
        p_mailbox->status = (uint32_t) TMP117_IPC_STATUS_OPEN_ERROR;
        p_mailbox->last_fsp_err = (uint32_t) err;
        pressure_i2c_bus_unlock();
        return err;
    }

    g_tmp117_opened = true;
    pressure_i2c_bus_unlock();
    return FSP_SUCCESS;
}

fsp_err_t tmp117_matrix_scan(void)
{
    uint32_t connected_mask = 0U;
    uint32_t error_mask = 0U;
    fsp_err_t last_err = FSP_SUCCESS;

    if (!g_tmp117_opened)
    {
        last_err = tmp117_matrix_init();
        if (FSP_SUCCESS != last_err)
        {
            return last_err;
        }
    }

    pressure_i2c_bus_lock();

    g_tmp117_debug.fail_step = TMP117_MATRIX_STEP_NONE;
    g_tmp117_debug.fail_channel = 0U;

    for (uint32_t channel = 0U; channel < TMP117_MATRIX_SENSOR_COUNT; channel++)
    {
        uint32_t mux_channel = g_tmp117_mux_channel[channel];
        int16_t raw = 0;
        int32_t milli_c = 0;
        fsp_err_t err = FSP_ERR_ABORTED;
        tmp117_matrix_step_t fail_step = TMP117_MATRIX_STEP_SELECT_MUX;
        for (uint32_t attempt = 0U; attempt < TMP117_CHANNEL_ATTEMPTS; attempt++)
        {
            err = tmp117_select_channel(mux_channel);
            fail_step = TMP117_MATRIX_STEP_SELECT_MUX;

            if (FSP_SUCCESS == err)
            {
                fail_step = TMP117_MATRIX_STEP_READ_TEMP;
                for (uint32_t address = TMP117_ADDR_FIRST; address <= TMP117_ADDR_LAST; address++)
                {
                    err = tmp117_read_channel(mux_channel, address, &raw, &milli_c);
                    if (FSP_SUCCESS == err)
                    {
                        g_tmp117_debug.address[channel] = address;
                        break;
                    }
                }
            }

            if (FSP_SUCCESS == err)
            {
                if (attempt > 0U)
                {
                    g_tmp117_debug.retry_success_count++;
                }
                break;
            }

            if ((attempt + 1U) < TMP117_CHANNEL_ATTEMPTS)
            {
                fsp_err_t recovery_err = tmp117_i2c_recover();
                if (FSP_SUCCESS != recovery_err)
                {
                    err = recovery_err;
                    break;
                }
            }
        }

        if (FSP_SUCCESS == err)
        {
            g_tmp117_failure_streak[channel] = 0U;
            connected_mask |= (1U << channel);
            g_tmp117_debug.channel_err[channel] = FSP_SUCCESS;
            g_tmp117_debug.channel_step[channel] = TMP117_MATRIX_STEP_SCAN_DONE;
            g_tmp117_debug.raw[channel] = raw;
            g_tmp117_debug.milli_c[channel] = milli_c;
        }
        else
        {
            if (g_tmp117_failure_streak[channel] < UINT8_MAX)
            {
                g_tmp117_failure_streak[channel]++;
            }

            /* A mux switch or shared-bus recovery can lose one scan.  Keep the
             * last valid sample online until the same channel fails three
             * consecutive scans; rotating one-scan failures are not sensor
             * disconnects and must not contaminate collection labels. */
            if ((0U != g_tmp117_debug.address[channel]) &&
                (g_tmp117_failure_streak[channel] < TMP117_CHANNEL_FAILURE_TOLERANCE))
            {
                connected_mask |= (1U << channel);
                g_tmp117_debug.channel_err[channel] = FSP_SUCCESS;
                g_tmp117_debug.channel_step[channel] = TMP117_MATRIX_STEP_SCAN_DONE;
                continue;
            }

            g_tmp117_debug.address[channel] = 0U;
            error_mask |= (1U << channel);
            last_err = err;
            g_tmp117_debug.channel_err[channel] = err;
            g_tmp117_debug.channel_step[channel] = fail_step;
            if (TMP117_MATRIX_STEP_NONE == g_tmp117_debug.fail_step)
            {
                g_tmp117_debug.fail_step = fail_step;
                g_tmp117_debug.fail_channel = mux_channel;
            }
        }
    }

    g_tmp117_debug.step = TMP117_MATRIX_STEP_SCAN_DONE;
    g_tmp117_debug.connected_mask = connected_mask;
    g_tmp117_debug.error_mask = error_mask;
    g_tmp117_debug.last_fsp_err = last_err;
    g_tmp117_debug.sequence++;

    tmp117_publish(connected_mask, error_mask, last_err);

    pressure_i2c_bus_unlock();

    return (0U == error_mask) ? FSP_SUCCESS : last_err;
}

void temp_i2c_callback(i2c_master_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    pressure_i2c_callback(p_args);

    if (!g_tmp117_i2c_context.busy)
    {
        return;
    }

    g_tmp117_i2c_context.event = p_args->event;

    if ((I2C_MASTER_EVENT_TX_COMPLETE == p_args->event) ||
        (I2C_MASTER_EVENT_RX_COMPLETE == p_args->event) ||
        (I2C_MASTER_EVENT_ABORTED == p_args->event))
    {
        g_tmp117_i2c_context.busy = false;
    }
}

#include "thread_code/scd41_driver.h"

#include <stdbool.h>
#include <string.h>

#include "thread_code/thermal_ipc_shared.h"
#include "thread_code/tmp117_ipc_shared.h"

#define SCD41_I2C_ADDRESS                    (0x62U)
#define SCD41_CMD_START_PERIODIC             (0x21B1U)
#define SCD41_CMD_READ_MEASUREMENT            (0xEC05U)
#define SCD41_CMD_STOP_PERIODIC              (0x3F86U)
#define SCD41_CMD_GET_DATA_READY             (0xE4B8U)
#define SCD41_CMD_GET_SERIAL                 (0x3682U)
#define SCD41_CMD_REINIT                     (0x3646U)
#define SCD41_CMD_WAKE_UP                    (0x36F6U)

#define SCD41_POWER_UP_DELAY_MS              (1100U)
#define SCD41_COMMAND_DELAY_MS               (1U)
#define SCD41_WAKE_DELAY_MS                  (30U)
#define SCD41_STOP_DELAY_MS                  (500U)
#define SCD41_REINIT_DELAY_MS                (30U)
#define SCD41_I2C_TIMEOUT_MS                 (100U)
#define SCD41_I2C_RECOVERY_DELAY_MS          (2U)
#define SCD41_I2C_BUS_CLEAR_PULSES           (9U)
#define SCD41_I2C_BUS_CLEAR_DELAY_US         (5U)
#define SCD41_RECOVERY_FAILURE_LIMIT         (3U)
#define SCD41_SAMPLE_WATCHDOG_MS             (15000U)

#define SCD41_I2C_SDA_PIN                    BSP_IO_PORT_05_PIN_11
#define SCD41_I2C_SCL_PIN                    BSP_IO_PORT_05_PIN_12
#define SCD41_I2C_RELEASE_CFG                ((uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT | \
                                               (uint32_t) IOPORT_CFG_PULLUP_ENABLE)
#define SCD41_I2C_LOW_CFG                    ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                               (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW | \
                                               (uint32_t) IOPORT_CFG_NMOS_ENABLE)
#define SCD41_I2C_PERIPHERAL_CFG             ((uint32_t) IOPORT_CFG_DRIVE_MID | \
                                               (uint32_t) IOPORT_CFG_PERIPHERAL_PIN | \
                                               (uint32_t) IOPORT_PERIPHERAL_IIC)

typedef char scd41_tmp117_overlap_guard[
    ((TMP117_IPC_ADDRESS + sizeof(tmp117_ipc_mailbox_t)) <= SCD41_IPC_ADDRESS) ? 1 : -1];
typedef char scd41_thermal_points_overlap_guard[
    ((SCD41_IPC_ADDRESS + SCD41_IPC_REGION_SIZE) <= THERMAL_IPC_POINTS_ADDRESS) ? 1 : -1];

typedef struct st_scd41_i2c_context
{
    volatile bool busy;
    volatile i2c_master_event_t event;
    volatile uint32_t last_event;
} scd41_i2c_context_t;

typedef struct st_scd41_runtime
{
    bool opened;
    bool periodic_started;
    bool has_sample;
    uint32_t sample_watchdog_tick;
    uint32_t co2_ppm;
    int32_t temperature_milli_c;
    uint32_t humidity_milli_percent;
    uint32_t raw_co2;
    uint32_t raw_temperature;
    uint32_t raw_humidity;
    uint32_t serial_words[3];
    uint32_t last_good_tick;
    uint32_t read_count;
    uint32_t crc_error_count;
    uint32_t i2c_error_count;
    uint32_t timeout_count;
    uint32_t recovery_count;
    uint32_t consecutive_failures;
    fsp_err_t last_fsp_err;
} scd41_runtime_t;

static scd41_i2c_context_t g_scd41_i2c_context;
static scd41_runtime_t g_scd41_runtime;

static uint32_t scd41_publish_begin(volatile scd41_ipc_mailbox_t * p_mailbox)
{
    uint32_t odd_lock = p_mailbox->publish_lock | 1U;

    p_mailbox->publish_lock = odd_lock;
    __DMB();
    return odd_lock + 1U;
}

static void scd41_publish_image(scd41_ipc_status_t status,
                                bool data_ready,
                                bool reset_bme)
{
    volatile scd41_ipc_mailbox_t * p_mailbox = SCD41_IPC_MAILBOX;
    uint32_t committed_lock = scd41_publish_begin(p_mailbox);
    uint32_t next_sequence = p_mailbox->sequence + 1U;

    p_mailbox->magic = SCD41_IPC_MAGIC;
    p_mailbox->status = (uint32_t) status;
    p_mailbox->co2_ppm = g_scd41_runtime.co2_ppm;
    p_mailbox->temperature_milli_c = g_scd41_runtime.temperature_milli_c;
    p_mailbox->humidity_milli_percent = g_scd41_runtime.humidity_milli_percent;
    p_mailbox->raw_co2 = g_scd41_runtime.raw_co2;
    p_mailbox->raw_temperature = g_scd41_runtime.raw_temperature;
    p_mailbox->raw_humidity = g_scd41_runtime.raw_humidity;
    p_mailbox->data_ready = data_ready ? 1U : 0U;
    p_mailbox->serial_words[0] = g_scd41_runtime.serial_words[0];
    p_mailbox->serial_words[1] = g_scd41_runtime.serial_words[1];
    p_mailbox->serial_words[2] = g_scd41_runtime.serial_words[2];
    p_mailbox->last_good_tick = g_scd41_runtime.last_good_tick;
    p_mailbox->read_count = g_scd41_runtime.read_count;
    p_mailbox->crc_error_count = g_scd41_runtime.crc_error_count;
    p_mailbox->i2c_error_count = g_scd41_runtime.i2c_error_count;
    p_mailbox->timeout_count = g_scd41_runtime.timeout_count;
    p_mailbox->recovery_count = g_scd41_runtime.recovery_count;
    p_mailbox->consecutive_failures = g_scd41_runtime.consecutive_failures;
    p_mailbox->last_fsp_err = (uint32_t) g_scd41_runtime.last_fsp_err;
    p_mailbox->last_event = g_scd41_i2c_context.last_event;
    if (reset_bme)
    {
        p_mailbox->bme_status = (uint32_t) BME688_IPC_STATUS_EMPTY;
        p_mailbox->bme_i2c_address = 0U;
        p_mailbox->bme_temperature_milli_c = 0;
        p_mailbox->bme_humidity_milli_percent = 0U;
        p_mailbox->bme_pressure_pa = 0U;
        p_mailbox->bme_gas_resistance_ohm = 0U;
        p_mailbox->bme_flags = 0U;
        p_mailbox->bme_last_good_tick = 0U;
        p_mailbox->bme_error_count = 0U;
    }
    p_mailbox->sequence = next_sequence;

    __DMB();
    p_mailbox->publish_lock = committed_lock;
}

static void scd41_publish(scd41_ipc_status_t status, bool data_ready)
{
    scd41_publish_image(status, data_ready, false);
}

static void scd41_reset_mailbox(void)
{
    /*
     * g_scd41_runtime is already zeroed. Publish the SCD41 startup image and
     * clear the BME688 tail in one seqlock transaction. A CPU1-only reset can
     * therefore never expose a new SCD41 epoch beside a stale BME688 sample.
     */
    scd41_publish_image(SCD41_IPC_STATUS_STARTING, false, true);
}

uint8_t scd41_crc8(uint8_t const * p_data, uint32_t length)
{
    uint8_t crc = 0xFFU;

    if ((NULL == p_data) && (0U != length))
    {
        return 0U;
    }

    for (uint32_t byte = 0U; byte < length; byte++)
    {
        crc ^= p_data[byte];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            crc = (0U != (crc & 0x80U)) ?
                  (uint8_t) ((uint8_t) (crc << 1U) ^ 0x31U) :
                  (uint8_t) (crc << 1U);
        }
    }

    return crc;
}

static bool scd41_word_crc_valid(uint8_t const * p_word)
{
    return scd41_crc8(p_word, 2U) == p_word[2];
}

static uint16_t scd41_decode_word(uint8_t const * p_word)
{
    return (uint16_t) (((uint16_t) p_word[0] << 8U) | p_word[1]);
}

static int32_t scd41_temperature_to_milli_c(uint16_t raw)
{
    uint64_t scaled = (175000ULL * raw + 32767ULL) / 65535ULL;
    return (int32_t) scaled - 45000;
}

static uint32_t scd41_humidity_to_milli_percent(uint16_t raw)
{
    uint64_t scaled = (100000ULL * raw + 32767ULL) / 65535ULL;
    return (uint32_t) scaled;
}

static fsp_err_t scd41_i2c_wait(i2c_master_event_t expected_event)
{
    TickType_t start_tick = xTaskGetTickCount();

    while (g_scd41_i2c_context.busy)
    {
        if ((xTaskGetTickCount() - start_tick) >= pdMS_TO_TICKS(SCD41_I2C_TIMEOUT_MS))
        {
            (void) g_scd41_i2c.p_api->abort(g_scd41_i2c.p_ctrl);
            g_scd41_i2c_context.busy = false;
            g_scd41_i2c_context.last_event = (uint32_t) I2C_MASTER_EVENT_ABORTED;
            return FSP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    if (expected_event != g_scd41_i2c_context.event)
    {
        return FSP_ERR_ABORTED;
    }

    return FSP_SUCCESS;
}

static fsp_err_t scd41_i2c_select(uint8_t address)
{
    if (!g_scd41_runtime.opened)
    {
        return FSP_ERR_NOT_OPEN;
    }

    return g_scd41_i2c.p_api->slaveAddressSet(g_scd41_i2c.p_ctrl,
                                               address,
                                               I2C_MASTER_ADDR_MODE_7BIT);
}

static fsp_err_t scd41_i2c_write_raw(uint8_t * p_data, uint32_t length, bool restart)
{
    fsp_err_t err;

    g_scd41_i2c_context.event = (i2c_master_event_t) 0;
    g_scd41_i2c_context.busy = true;
    err = g_scd41_i2c.p_api->write(g_scd41_i2c.p_ctrl, p_data, length, restart);
    if (FSP_SUCCESS != err)
    {
        g_scd41_i2c_context.busy = false;
        return err;
    }

    return scd41_i2c_wait(I2C_MASTER_EVENT_TX_COMPLETE);
}

static fsp_err_t scd41_i2c_read_raw(uint8_t * p_data, uint32_t length, bool restart)
{
    fsp_err_t err;

    g_scd41_i2c_context.event = (i2c_master_event_t) 0;
    g_scd41_i2c_context.busy = true;
    err = g_scd41_i2c.p_api->read(g_scd41_i2c.p_ctrl, p_data, length, restart);
    if (FSP_SUCCESS != err)
    {
        g_scd41_i2c_context.busy = false;
        return err;
    }

    return scd41_i2c_wait(I2C_MASTER_EVENT_RX_COMPLETE);
}

static fsp_err_t scd41_i2c_write(uint8_t * p_data, uint32_t length)
{
    fsp_err_t err = scd41_i2c_select(SCD41_I2C_ADDRESS);

    return (FSP_SUCCESS == err) ? scd41_i2c_write_raw(p_data, length, false) : err;
}

static fsp_err_t scd41_i2c_read(uint8_t * p_data, uint32_t length)
{
    fsp_err_t err = scd41_i2c_select(SCD41_I2C_ADDRESS);

    return (FSP_SUCCESS == err) ? scd41_i2c_read_raw(p_data, length, false) : err;
}

fsp_err_t scd41_i2c_shared_write(uint8_t address, uint8_t * p_data, uint32_t length)
{
    fsp_err_t err;

    if ((address > 0x7FU) || (NULL == p_data) || (0U == length))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    err = scd41_i2c_select(address);
    if (FSP_SUCCESS == err)
    {
        err = scd41_i2c_write_raw(p_data, length, false);
    }
    if (FSP_SUCCESS != err)
    {
        (void) g_scd41_i2c.p_api->abort(g_scd41_i2c.p_ctrl);
        g_scd41_i2c_context.busy = false;
    }

    return err;
}

fsp_err_t scd41_i2c_shared_write_read(uint8_t address,
                                     uint8_t * p_write,
                                     uint32_t write_length,
                                     uint8_t * p_read,
                                     uint32_t read_length)
{
    fsp_err_t err;

    if ((address > 0x7FU) ||
        (NULL == p_write) || (0U == write_length) ||
        (NULL == p_read) || (0U == read_length))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    err = scd41_i2c_select(address);
    if (FSP_SUCCESS == err)
    {
        err = scd41_i2c_write_raw(p_write, write_length, true);
    }
    if (FSP_SUCCESS == err)
    {
        err = scd41_i2c_read_raw(p_read, read_length, false);
    }
    if (FSP_SUCCESS != err)
    {
        /*
         * A failed first half may have left a restart transaction armed.
         * Abort it without closing the shared controller; SCD41 remains the
         * sole owner of controller-level recovery.
         */
        (void) g_scd41_i2c.p_api->abort(g_scd41_i2c.p_ctrl);
        g_scd41_i2c_context.busy = false;
    }

    return err;
}

static fsp_err_t scd41_send_command(uint16_t command)
{
    uint8_t bytes[2] =
    {
        (uint8_t) (command >> 8U),
        (uint8_t) command
    };

    return scd41_i2c_write(bytes, sizeof(bytes));
}

static fsp_err_t scd41_read_command(uint16_t command, uint8_t * p_response, uint32_t response_length)
{
    fsp_err_t err = scd41_send_command(command);

    if (FSP_SUCCESS != err)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(SCD41_COMMAND_DELAY_MS));
    return scd41_i2c_read(p_response, response_length);
}

static void scd41_discard_expected_command_error(uint16_t command)
{
    fsp_err_t err = scd41_send_command(command);

    if (FSP_SUCCESS != err)
    {
        (void) g_scd41_i2c.p_api->abort(g_scd41_i2c.p_ctrl);
        g_scd41_i2c_context.busy = false;
        g_scd41_i2c_context.event = (i2c_master_event_t) 0;
        vTaskDelay(pdMS_TO_TICKS(SCD41_I2C_RECOVERY_DELAY_MS));
    }
}

static void scd41_bus_clear_delay(void)
{
    R_BSP_SoftwareDelay(SCD41_I2C_BUS_CLEAR_DELAY_US, BSP_DELAY_UNITS_MICROSECONDS);
}

static fsp_err_t scd41_i2c_bus_clear(void)
{
    fsp_err_t err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, SCD41_I2C_SDA_PIN, SCD41_I2C_RELEASE_CFG);

    if (FSP_SUCCESS == err)
    {
        err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, SCD41_I2C_SCL_PIN, SCD41_I2C_RELEASE_CFG);
    }
    if (FSP_SUCCESS == err)
    {
        scd41_bus_clear_delay();
        for (uint32_t pulse = 0U; pulse < SCD41_I2C_BUS_CLEAR_PULSES; pulse++)
        {
            err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, SCD41_I2C_SCL_PIN, SCD41_I2C_LOW_CFG);
            if (FSP_SUCCESS != err)
            {
                break;
            }
            scd41_bus_clear_delay();

            err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, SCD41_I2C_SCL_PIN, SCD41_I2C_RELEASE_CFG);
            if (FSP_SUCCESS != err)
            {
                break;
            }
            scd41_bus_clear_delay();
        }
    }

    if (FSP_SUCCESS == err)
    {
        err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, SCD41_I2C_SDA_PIN, SCD41_I2C_LOW_CFG);
        scd41_bus_clear_delay();
    }
    if (FSP_SUCCESS == err)
    {
        err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, SCD41_I2C_SCL_PIN, SCD41_I2C_RELEASE_CFG);
        scd41_bus_clear_delay();
    }
    if (FSP_SUCCESS == err)
    {
        err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, SCD41_I2C_SDA_PIN, SCD41_I2C_RELEASE_CFG);
        scd41_bus_clear_delay();
    }

    {
        fsp_err_t restore_sda_err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                                           SCD41_I2C_SDA_PIN,
                                                           SCD41_I2C_PERIPHERAL_CFG);
        fsp_err_t restore_scl_err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                                           SCD41_I2C_SCL_PIN,
                                                           SCD41_I2C_PERIPHERAL_CFG);
        if (FSP_SUCCESS == err)
        {
            err = restore_sda_err;
        }
        if (FSP_SUCCESS == err)
        {
            err = restore_scl_err;
        }
    }

    return err;
}

static fsp_err_t scd41_i2c_open(void)
{
    fsp_err_t err;

    if (g_scd41_runtime.opened)
    {
        return FSP_SUCCESS;
    }

    err = g_scd41_i2c.p_api->open(g_scd41_i2c.p_ctrl, g_scd41_i2c.p_cfg);
    if (FSP_ERR_ALREADY_OPEN == err)
    {
        err = FSP_SUCCESS;
    }
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    g_scd41_runtime.opened = true;
    err = g_scd41_i2c.p_api->slaveAddressSet(g_scd41_i2c.p_ctrl,
                                              SCD41_I2C_ADDRESS,
                                              I2C_MASTER_ADDR_MODE_7BIT);
    if (FSP_SUCCESS != err)
    {
        (void) g_scd41_i2c.p_api->close(g_scd41_i2c.p_ctrl);
        g_scd41_runtime.opened = false;
    }

    return err;
}

static void scd41_i2c_close(void)
{
    if (!g_scd41_runtime.opened)
    {
        return;
    }

    (void) g_scd41_i2c.p_api->abort(g_scd41_i2c.p_ctrl);
    g_scd41_i2c_context.busy = false;
    vTaskDelay(pdMS_TO_TICKS(SCD41_I2C_RECOVERY_DELAY_MS));
    (void) g_scd41_i2c.p_api->close(g_scd41_i2c.p_ctrl);
    g_scd41_runtime.opened = false;
    g_scd41_runtime.periodic_started = false;
}

static fsp_err_t scd41_synchronize_sensor(void)
{
    uint8_t serial[9] = {0U};
    fsp_err_t err;

    /*
     * Make MCU-only resets deterministic. wake_up may NACK by specification;
     * stop_periodic may NACK when the sensor was already idle. Both outcomes
     * are expected and are deliberately not counted as sensor failures.
     */
    vTaskDelay(pdMS_TO_TICKS(SCD41_WAKE_DELAY_MS));
    scd41_discard_expected_command_error(SCD41_CMD_WAKE_UP);
    vTaskDelay(pdMS_TO_TICKS(SCD41_WAKE_DELAY_MS));
    scd41_discard_expected_command_error(SCD41_CMD_STOP_PERIODIC);
    vTaskDelay(pdMS_TO_TICKS(SCD41_STOP_DELAY_MS));

    err = scd41_send_command(SCD41_CMD_REINIT);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(SCD41_REINIT_DELAY_MS));

    err = scd41_read_command(SCD41_CMD_GET_SERIAL, serial, sizeof(serial));
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    for (uint32_t word = 0U; word < 3U; word++)
    {
        if (!scd41_word_crc_valid(&serial[word * 3U]))
        {
            return FSP_ERR_INVALID_DATA;
        }
        g_scd41_runtime.serial_words[word] = scd41_decode_word(&serial[word * 3U]);
    }

    err = scd41_send_command(SCD41_CMD_START_PERIODIC);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    g_scd41_runtime.periodic_started = true;
    g_scd41_runtime.sample_watchdog_tick = (uint32_t) xTaskGetTickCount();
    g_scd41_runtime.last_fsp_err = FSP_SUCCESS;
    scd41_publish(g_scd41_runtime.has_sample ?
                  SCD41_IPC_STATUS_SAMPLE_OK : SCD41_IPC_STATUS_MEASURING,
                  false);
    return FSP_SUCCESS;
}

static fsp_err_t scd41_record_failure(fsp_err_t err, bool initialization)
{
    scd41_ipc_status_t status;

    g_scd41_runtime.last_fsp_err = err;
    g_scd41_runtime.consecutive_failures++;

    if (FSP_ERR_INVALID_DATA == err)
    {
        g_scd41_runtime.crc_error_count++;
        status = SCD41_IPC_STATUS_CRC_ERROR;
    }
    else if (FSP_ERR_TIMEOUT == err)
    {
        g_scd41_runtime.timeout_count++;
        status = SCD41_IPC_STATUS_TIMEOUT;
    }
    else
    {
        g_scd41_runtime.i2c_error_count++;
        status = initialization ? SCD41_IPC_STATUS_NOT_FOUND : SCD41_IPC_STATUS_I2C_ERROR;
    }

    scd41_publish(status, false);
    return err;
}

static fsp_err_t scd41_recover(void)
{
    fsp_err_t err;

    g_scd41_runtime.recovery_count++;
    g_scd41_runtime.last_fsp_err = FSP_SUCCESS;
    scd41_publish(SCD41_IPC_STATUS_RECOVERING, false);

    scd41_i2c_close();
    err = scd41_i2c_bus_clear();
    if (FSP_SUCCESS == err)
    {
        err = scd41_i2c_open();
    }
    if (FSP_SUCCESS == err)
    {
        err = scd41_synchronize_sensor();
    }

    if (FSP_SUCCESS == err)
    {
        g_scd41_runtime.consecutive_failures = 0U;
        g_scd41_runtime.last_fsp_err = FSP_SUCCESS;
        scd41_publish(g_scd41_runtime.has_sample ?
                      SCD41_IPC_STATUS_SAMPLE_OK : SCD41_IPC_STATUS_MEASURING,
                      false);
        return FSP_SUCCESS;
    }

    g_scd41_runtime.periodic_started = false;
    return scd41_record_failure(err, false);
}

fsp_err_t scd41_i2c_shared_recover(void)
{
    /*
     * IIC1 is shared with BME688, but this driver remains the sole owner of
     * controller close/open and bus-clear operations.  A peer can request a
     * recovery without manipulating the FSP controller behind SCD41's back.
     */
    return scd41_recover();
}

fsp_err_t scd41_driver_init(void)
{
    fsp_err_t err;

    memset(&g_scd41_i2c_context, 0, sizeof(g_scd41_i2c_context));
    memset(&g_scd41_runtime, 0, sizeof(g_scd41_runtime));
    g_scd41_runtime.last_fsp_err = FSP_SUCCESS;
    scd41_reset_mailbox();

    /* The vendor datasheet requires up to 1000 ms from VDD rise to idle. */
    vTaskDelay(pdMS_TO_TICKS(SCD41_POWER_UP_DELAY_MS));

    err = scd41_i2c_open();
    if (FSP_SUCCESS == err)
    {
        err = scd41_synchronize_sensor();
    }
    if (FSP_SUCCESS != err)
    {
        g_scd41_runtime.periodic_started = false;
        return scd41_record_failure(err, true);
    }

    return FSP_SUCCESS;
}

fsp_err_t scd41_driver_poll(void)
{
    uint8_t ready_response[3] = {0U};
    uint8_t measurement[9] = {0U};
    uint16_t ready_word;
    uint16_t raw_co2;
    uint16_t raw_temperature;
    uint16_t raw_humidity;
    fsp_err_t err;

    if (!g_scd41_runtime.opened || !g_scd41_runtime.periodic_started)
    {
        return scd41_recover();
    }

    err = scd41_read_command(SCD41_CMD_GET_DATA_READY, ready_response, sizeof(ready_response));
    if (FSP_SUCCESS != err)
    {
        (void) scd41_record_failure(err, false);
        return (g_scd41_runtime.consecutive_failures >= SCD41_RECOVERY_FAILURE_LIMIT) ?
               scd41_recover() : err;
    }
    if (!scd41_word_crc_valid(ready_response))
    {
        err = FSP_ERR_INVALID_DATA;
        (void) scd41_record_failure(err, false);
        return (g_scd41_runtime.consecutive_failures >= SCD41_RECOVERY_FAILURE_LIMIT) ?
               scd41_recover() : err;
    }

    ready_word = scd41_decode_word(ready_response);
    g_scd41_runtime.last_fsp_err = FSP_SUCCESS;
    if (0U == (ready_word & 0x07FFU))
    {
        uint32_t now = (uint32_t) xTaskGetTickCount();

        /*
         * A responsive I2C slave can still have a wedged measurement engine.
         * Three missed 5-second update intervals trigger a full resync.
         */
        if ((now - g_scd41_runtime.sample_watchdog_tick) >=
            pdMS_TO_TICKS(SCD41_SAMPLE_WATCHDOG_MS))
        {
            err = FSP_ERR_TIMEOUT;
            (void) scd41_record_failure(err, false);
            return scd41_recover();
        }

        scd41_publish(g_scd41_runtime.has_sample ?
                      SCD41_IPC_STATUS_SAMPLE_OK : SCD41_IPC_STATUS_MEASURING,
                      false);
        return FSP_SUCCESS;
    }

    err = scd41_read_command(SCD41_CMD_READ_MEASUREMENT, measurement, sizeof(measurement));
    if (FSP_SUCCESS != err)
    {
        (void) scd41_record_failure(err, false);
        return (g_scd41_runtime.consecutive_failures >= SCD41_RECOVERY_FAILURE_LIMIT) ?
               scd41_recover() : err;
    }

    for (uint32_t word = 0U; word < 3U; word++)
    {
        if (!scd41_word_crc_valid(&measurement[word * 3U]))
        {
            err = FSP_ERR_INVALID_DATA;
            (void) scd41_record_failure(err, false);
            return (g_scd41_runtime.consecutive_failures >= SCD41_RECOVERY_FAILURE_LIMIT) ?
                   scd41_recover() : err;
        }
    }

    raw_co2 = scd41_decode_word(&measurement[0]);
    raw_temperature = scd41_decode_word(&measurement[3]);
    raw_humidity = scd41_decode_word(&measurement[6]);

    /* Publish only after all three CRCs pass; never expose a partial frame. */
    g_scd41_runtime.raw_co2 = raw_co2;
    g_scd41_runtime.raw_temperature = raw_temperature;
    g_scd41_runtime.raw_humidity = raw_humidity;
    g_scd41_runtime.co2_ppm = raw_co2;
    g_scd41_runtime.temperature_milli_c = scd41_temperature_to_milli_c(raw_temperature);
    g_scd41_runtime.humidity_milli_percent = scd41_humidity_to_milli_percent(raw_humidity);
    g_scd41_runtime.last_good_tick = (uint32_t) xTaskGetTickCount();
    g_scd41_runtime.sample_watchdog_tick = g_scd41_runtime.last_good_tick;
    g_scd41_runtime.read_count++;
    g_scd41_runtime.consecutive_failures = 0U;
    g_scd41_runtime.last_fsp_err = FSP_SUCCESS;
    g_scd41_runtime.has_sample = true;
    scd41_publish(SCD41_IPC_STATUS_SAMPLE_OK, true);

    return FSP_SUCCESS;
}

void scd41_i2c_callback(i2c_master_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    g_scd41_i2c_context.event = p_args->event;
    g_scd41_i2c_context.last_event = (uint32_t) p_args->event;

    if ((I2C_MASTER_EVENT_TX_COMPLETE == p_args->event) ||
        (I2C_MASTER_EVENT_RX_COMPLETE == p_args->event) ||
        (I2C_MASTER_EVENT_ABORTED == p_args->event))
    {
        g_scd41_i2c_context.busy = false;
    }
}

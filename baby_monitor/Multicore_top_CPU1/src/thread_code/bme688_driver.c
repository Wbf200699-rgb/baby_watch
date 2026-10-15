#include "thread_code/bme688_driver.h"

#include <stdbool.h>
#include <string.h>

#include "thread_code/bme68x.h"
#include "thread_code/scd41_driver.h"
#include "thread_code/scd41_ipc_shared.h"

#define BME688_I2C_ADDRESS_LOW              (0x76U)
#define BME688_I2C_ADDRESS_HIGH             (0x77U)
#define BME688_AMBIENT_TEMPERATURE_C        (25)
#define BME688_HEATER_TEMPERATURE_C         (300U)
#define BME688_HEATER_DURATION_MS           (100U)
#define BME688_REINIT_FAILURE_LIMIT         (3U)
#define BME688_RETRY_INTERVAL_MS            (5000U)
#define BME688_BUS_RECOVERY_FAILURE_LIMIT   (3U)
#define BME688_MAX_BUS_WRITE_BYTES          (32U)

typedef struct st_bme688_runtime
{
    struct bme68x_dev device;
    struct bme68x_conf configuration;
    struct bme68x_heatr_conf heater;
    uint8_t address;
    bool detected;
    bool initialized;
    uint32_t consecutive_failures;
    uint32_t error_count;
    int32_t temperature_milli_c;
    uint32_t humidity_milli_percent;
    uint32_t pressure_pa;
    uint32_t gas_resistance_ohm;
    uint32_t last_good_tick;
    uint32_t last_init_attempt_tick;
    uint32_t last_flags;
    uint32_t init_failure_count;
    uint32_t bus_recovery_count;
    fsp_err_t last_bus_error;
    bool bus_recovery_attempted;
} bme688_runtime_t;

static bme688_runtime_t g_bme688_runtime;

static uint32_t bme688_publish_begin(volatile scd41_ipc_mailbox_t * p_mailbox)
{
    uint32_t odd_lock = p_mailbox->publish_lock | 1U;

    p_mailbox->publish_lock = odd_lock;
    __DMB();
    return odd_lock + 1U;
}

static void bme688_publish(bme688_ipc_status_t status, uint32_t flags)
{
    volatile scd41_ipc_mailbox_t * p_mailbox = SCD41_IPC_MAILBOX;
    uint32_t committed_lock = bme688_publish_begin(p_mailbox);

    p_mailbox->magic = SCD41_IPC_MAGIC;
    p_mailbox->bme_status = (uint32_t) status;
    p_mailbox->bme_i2c_address = g_bme688_runtime.detected ?
                                 (uint32_t) g_bme688_runtime.address : 0U;
    p_mailbox->bme_temperature_milli_c = g_bme688_runtime.temperature_milli_c;
    p_mailbox->bme_humidity_milli_percent = g_bme688_runtime.humidity_milli_percent;
    p_mailbox->bme_pressure_pa = g_bme688_runtime.pressure_pa;
    p_mailbox->bme_gas_resistance_ohm = g_bme688_runtime.gas_resistance_ohm;
    p_mailbox->bme_flags = flags;
    p_mailbox->bme_last_good_tick = g_bme688_runtime.last_good_tick;
    p_mailbox->bme_error_count = g_bme688_runtime.error_count;
    p_mailbox->sequence++;

    __DMB();
    p_mailbox->publish_lock = committed_lock;
}

static BME68X_INTF_RET_TYPE bme688_bus_read(uint8_t register_address,
                                             uint8_t * p_data,
                                             uint32_t length,
                                             void * p_interface)
{
    uint8_t address;
    fsp_err_t err;

    if ((NULL == p_interface) || (NULL == p_data) || (0U == length))
    {
        return (BME68X_INTF_RET_TYPE) -1;
    }

    address = *((uint8_t *) p_interface);
    err = scd41_i2c_shared_write_read(address,
                                      &register_address,
                                      1U,
                                      p_data,
                                      length);
    g_bme688_runtime.last_bus_error = err;
    return (FSP_SUCCESS == err) ? BME68X_INTF_RET_SUCCESS :
           (BME68X_INTF_RET_TYPE) -1;
}

static BME68X_INTF_RET_TYPE bme688_bus_write(uint8_t register_address,
                                              uint8_t const * p_data,
                                              uint32_t length,
                                              void * p_interface)
{
    uint8_t transfer[BME688_MAX_BUS_WRITE_BYTES];
    uint8_t address;
    fsp_err_t err;

    if ((NULL == p_interface) || (NULL == p_data) ||
        (0U == length) || (length >= sizeof(transfer)))
    {
        return (BME68X_INTF_RET_TYPE) -1;
    }

    address = *((uint8_t *) p_interface);
    transfer[0] = register_address;
    memcpy(&transfer[1], p_data, length);

    err = scd41_i2c_shared_write(address, transfer, length + 1U);
    g_bme688_runtime.last_bus_error = err;
    return (FSP_SUCCESS == err) ? BME68X_INTF_RET_SUCCESS :
           (BME68X_INTF_RET_TYPE) -1;
}

static void bme688_delay_us(uint32_t period_us, void * p_interface)
{
    FSP_PARAMETER_NOT_USED(p_interface);

    if (period_us >= 1000U)
    {
        uint32_t period_ms = (period_us + 999U) / 1000U;

        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
    else if (period_us > 0U)
    {
        R_BSP_SoftwareDelay(period_us, BSP_DELAY_UNITS_MICROSECONDS);
    }
}

static fsp_err_t bme688_result_to_fsp(int8_t result)
{
    if (BME68X_OK == result)
    {
        return FSP_SUCCESS;
    }
    if (BME68X_E_DEV_NOT_FOUND == result)
    {
        return FSP_ERR_NOT_FOUND;
    }
    if (BME68X_E_COM_FAIL == result)
    {
        return (FSP_SUCCESS != g_bme688_runtime.last_bus_error) ?
               g_bme688_runtime.last_bus_error : FSP_ERR_ABORTED;
    }

    return FSP_ERR_INVALID_DATA;
}

static void bme688_prepare_device(uint8_t address)
{
    memset(&g_bme688_runtime.device, 0, sizeof(g_bme688_runtime.device));
    g_bme688_runtime.address = address;
    g_bme688_runtime.last_bus_error = FSP_SUCCESS;
    g_bme688_runtime.device.intf = BME68X_I2C_INTF;
    g_bme688_runtime.device.intf_ptr = &g_bme688_runtime.address;
    g_bme688_runtime.device.read = bme688_bus_read;
    g_bme688_runtime.device.write = bme688_bus_write;
    g_bme688_runtime.device.delay_us = bme688_delay_us;
    g_bme688_runtime.device.amb_temp = BME688_AMBIENT_TEMPERATURE_C;
}

static int8_t bme688_configure(void)
{
    int8_t result;

    memset(&g_bme688_runtime.configuration, 0, sizeof(g_bme688_runtime.configuration));
    g_bme688_runtime.configuration.os_hum = BME68X_OS_2X;
    g_bme688_runtime.configuration.os_pres = BME68X_OS_4X;
    g_bme688_runtime.configuration.os_temp = BME68X_OS_2X;
    g_bme688_runtime.configuration.filter = BME68X_FILTER_SIZE_3;
    g_bme688_runtime.configuration.odr = BME68X_ODR_NONE;

    result = bme68x_set_conf(&g_bme688_runtime.configuration,
                             &g_bme688_runtime.device);
    if (BME68X_OK != result)
    {
        return result;
    }

    memset(&g_bme688_runtime.heater, 0, sizeof(g_bme688_runtime.heater));
    g_bme688_runtime.heater.enable = BME68X_ENABLE;
    g_bme688_runtime.heater.heatr_temp = BME688_HEATER_TEMPERATURE_C;
    g_bme688_runtime.heater.heatr_dur = BME688_HEATER_DURATION_MS;

    return bme68x_set_heatr_conf(BME68X_FORCED_MODE,
                                 &g_bme688_runtime.heater,
                                 &g_bme688_runtime.device);
}

static fsp_err_t bme688_try_initialize(void)
{
    static uint8_t const addresses[] =
    {
        BME688_I2C_ADDRESS_LOW,
        BME688_I2C_ADDRESS_HIGH
    };
    bool device_seen = false;
    int8_t result = BME68X_E_DEV_NOT_FOUND;

    g_bme688_runtime.last_init_attempt_tick = (uint32_t) xTaskGetTickCount();
    g_bme688_runtime.detected = false;
    g_bme688_runtime.initialized = false;
    bme688_publish(BME688_IPC_STATUS_STARTING, 0U);

    for (uint32_t i = 0U; i < (sizeof(addresses) / sizeof(addresses[0])); i++)
    {
        bme688_prepare_device(addresses[i]);
        result = bme68x_init(&g_bme688_runtime.device);
        if (BME68X_OK == result)
        {
            device_seen = true;
            g_bme688_runtime.detected = true;
            result = bme688_configure();
        }
        if (BME68X_OK == result)
        {
            g_bme688_runtime.initialized = true;
            g_bme688_runtime.consecutive_failures = 0U;
            g_bme688_runtime.init_failure_count = 0U;
            g_bme688_runtime.bus_recovery_attempted = false;
            bme688_publish(BME688_IPC_STATUS_MEASURING, 0U);
            return FSP_SUCCESS;
        }
        if (device_seen)
        {
            break;
        }
    }

    if (!device_seen)
    {
        g_bme688_runtime.address = 0U;
    }
    g_bme688_runtime.error_count++;
    if (device_seen)
    {
        bme688_ipc_status_t status = BME688_IPC_STATUS_DRIVER_ERROR;

        if (BME68X_E_COM_FAIL == result)
        {
            status = (FSP_ERR_TIMEOUT == g_bme688_runtime.last_bus_error) ?
                     BME688_IPC_STATUS_TIMEOUT : BME688_IPC_STATUS_I2C_ERROR;
        }
        bme688_publish(status, 0U);
    }
    else if (BME68X_E_COM_FAIL == result)
    {
        bme688_ipc_status_t status = BME688_IPC_STATUS_I2C_ERROR;

        if (FSP_ERR_TIMEOUT == g_bme688_runtime.last_bus_error)
        {
            status = BME688_IPC_STATUS_TIMEOUT;
        }
        else if (FSP_ERR_ABORTED == g_bme688_runtime.last_bus_error)
        {
            status = BME688_IPC_STATUS_NOT_FOUND;
        }
        bme688_publish(status, 0U);
    }
    else
    {
        bme688_publish(BME688_IPC_STATUS_NOT_FOUND, 0U);
    }
    return bme688_result_to_fsp(result);
}

static bool bme688_bus_recovery_due(void)
{
    if (g_bme688_runtime.init_failure_count < BME688_BUS_RECOVERY_FAILURE_LIMIT)
    {
        return false;
    }

    /* One controller recovery per outage; a successful probe rearms it. */
    return !g_bme688_runtime.bus_recovery_attempted;
}

static fsp_err_t bme688_try_recover_bus(void)
{
    fsp_err_t err;

    g_bme688_runtime.bus_recovery_attempted = true;
    g_bme688_runtime.bus_recovery_count++;
    bme688_publish(BME688_IPC_STATUS_RECOVERING, 0U);

    err = scd41_i2c_shared_recover();
    if (FSP_SUCCESS != err)
    {
        g_bme688_runtime.last_bus_error = err;
        bme688_publish((FSP_ERR_TIMEOUT == err) ?
                       BME688_IPC_STATUS_TIMEOUT :
                       BME688_IPC_STATUS_I2C_ERROR,
                       0U);
        return err;
    }

    g_bme688_runtime.init_failure_count = 0U;
    return bme688_try_initialize();
}

static fsp_err_t bme688_record_failure(int8_t result)
{
    bme688_ipc_status_t status = BME688_IPC_STATUS_DRIVER_ERROR;

    if (BME68X_E_COM_FAIL == result)
    {
        status = (FSP_ERR_TIMEOUT == g_bme688_runtime.last_bus_error) ?
                 BME688_IPC_STATUS_TIMEOUT : BME688_IPC_STATUS_I2C_ERROR;
    }

    g_bme688_runtime.error_count++;
    g_bme688_runtime.consecutive_failures++;
    if (g_bme688_runtime.consecutive_failures >= BME688_REINIT_FAILURE_LIMIT)
    {
        /*
         * Invalidate the BME instance and let the next retry request recovery
         * through the SCD41 bus owner.  Direct close/open here would corrupt
         * SCD41's view of the shared FSP controller.
         */
        g_bme688_runtime.initialized = false;
        g_bme688_runtime.init_failure_count = BME688_BUS_RECOVERY_FAILURE_LIMIT;
    }
    bme688_publish(status, 0U);
    return bme688_result_to_fsp(result);
}

static bool bme688_data_is_plausible(struct bme68x_data const * p_data)
{
    return (NULL != p_data) &&
           (p_data->temperature >= -40.0f) &&
           (p_data->temperature <= 85.0f) &&
           (p_data->humidity >= 0.0f) &&
           (p_data->humidity <= 100.0f) &&
           (p_data->pressure >= 30000.0f) &&
           (p_data->pressure <= 110000.0f) &&
           (p_data->gas_resistance >= 0.0f) &&
           (p_data->gas_resistance <= 4294967040.0f);
}

fsp_err_t bme688_driver_init(void)
{
    fsp_err_t err;

    memset(&g_bme688_runtime, 0, sizeof(g_bme688_runtime));
    g_bme688_runtime.last_bus_error = FSP_SUCCESS;
    err = bme688_try_initialize();
    if (FSP_SUCCESS != err)
    {
        g_bme688_runtime.init_failure_count = 1U;
    }
    return err;
}

fsp_err_t bme688_driver_poll(void)
{
    struct bme68x_data data;
    uint8_t sample_count = 0U;
    uint32_t measurement_duration_us;
    uint32_t flags = 0U;
    int8_t result;

    if (!g_bme688_runtime.initialized)
    {
        uint32_t now = (uint32_t) xTaskGetTickCount();
        fsp_err_t err;

        if ((now - g_bme688_runtime.last_init_attempt_tick) <
            pdMS_TO_TICKS(BME688_RETRY_INTERVAL_MS))
        {
            return FSP_ERR_NOT_FOUND;
        }

        err = bme688_try_initialize();
        if (FSP_SUCCESS == err)
        {
            return FSP_SUCCESS;
        }

        g_bme688_runtime.init_failure_count++;
        return bme688_bus_recovery_due() ? bme688_try_recover_bus() : err;
    }

    result = bme68x_set_op_mode(BME68X_FORCED_MODE, &g_bme688_runtime.device);
    if (BME68X_OK != result)
    {
        return bme688_record_failure(result);
    }

    measurement_duration_us =
        bme68x_get_meas_dur(BME68X_FORCED_MODE,
                            &g_bme688_runtime.configuration,
                            &g_bme688_runtime.device) +
        ((uint32_t) g_bme688_runtime.heater.heatr_dur * 1000U);
    bme688_delay_us(measurement_duration_us, &g_bme688_runtime.address);

    memset(&data, 0, sizeof(data));
    result = bme68x_get_data(BME68X_FORCED_MODE,
                             &data,
                             &sample_count,
                             &g_bme688_runtime.device);
    if (BME68X_W_NO_NEW_DATA == result)
    {
        bme688_publish((0U != g_bme688_runtime.last_good_tick) ?
                       BME688_IPC_STATUS_SAMPLE_OK : BME688_IPC_STATUS_MEASURING,
                       g_bme688_runtime.last_flags &
                       ~((uint32_t) BME688_IPC_FLAG_NEW_DATA));
        return FSP_SUCCESS;
    }
    if ((BME68X_OK != result) || (0U == sample_count))
    {
        return bme688_record_failure((BME68X_OK == result) ?
                                     BME68X_E_INVALID_LENGTH : result);
    }
    if (!bme688_data_is_plausible(&data))
    {
        return bme688_record_failure(BME68X_E_SELF_TEST);
    }

    if (0U != (data.status & BME68X_NEW_DATA_MSK))
    {
        flags |= BME688_IPC_FLAG_NEW_DATA;
    }
    if (0U != (data.status & BME68X_GASM_VALID_MSK))
    {
        flags |= BME688_IPC_FLAG_GAS_VALID;
    }
    if (0U != (data.status & BME68X_HEAT_STAB_MSK))
    {
        flags |= BME688_IPC_FLAG_HEATER_STABLE;
    }

    g_bme688_runtime.temperature_milli_c =
        (int32_t) ((data.temperature * 1000.0f) +
                   ((data.temperature >= 0.0f) ? 0.5f : -0.5f));
    g_bme688_runtime.humidity_milli_percent =
        (uint32_t) ((data.humidity * 1000.0f) + 0.5f);
    g_bme688_runtime.pressure_pa = (uint32_t) (data.pressure + 0.5f);
    g_bme688_runtime.gas_resistance_ohm =
        (uint32_t) (data.gas_resistance + 0.5f);
    g_bme688_runtime.last_good_tick = (uint32_t) xTaskGetTickCount();
    g_bme688_runtime.last_flags = flags;
    g_bme688_runtime.consecutive_failures = 0U;
    bme688_publish(BME688_IPC_STATUS_SAMPLE_OK, flags);

    return FSP_SUCCESS;
}

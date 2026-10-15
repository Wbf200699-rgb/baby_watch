#ifndef SCD41_IPC_SHARED_H
#define SCD41_IPC_SHARED_H

#include <stdbool.h>
#include <stdint.h>

#include "bsp_api.h"

#define SCD41_IPC_MAGIC        (0x53434434U) /* SCD4 */
#define SCD41_IPC_ADDRESS      (0x221D0180UL)
#define SCD41_IPC_REGION_SIZE  (0x80U)
#define SCD41_IPC_SNAPSHOT_ATTEMPTS (3U)

typedef enum e_scd41_ipc_status
{
    SCD41_IPC_STATUS_EMPTY = 0,
    SCD41_IPC_STATUS_STARTING = 1,
    SCD41_IPC_STATUS_MEASURING = 2,
    SCD41_IPC_STATUS_SAMPLE_OK = 3,
    SCD41_IPC_STATUS_NOT_FOUND = 4,
    SCD41_IPC_STATUS_CRC_ERROR = 5,
    SCD41_IPC_STATUS_I2C_ERROR = 6,
    SCD41_IPC_STATUS_TIMEOUT = 7,
    SCD41_IPC_STATUS_RECOVERING = 8
} scd41_ipc_status_t;

typedef enum e_bme688_ipc_status
{
    BME688_IPC_STATUS_EMPTY = 0,
    BME688_IPC_STATUS_STARTING = 1,
    BME688_IPC_STATUS_MEASURING = 2,
    BME688_IPC_STATUS_SAMPLE_OK = 3,
    BME688_IPC_STATUS_NOT_FOUND = 4,
    BME688_IPC_STATUS_DRIVER_ERROR = 5,
    BME688_IPC_STATUS_I2C_ERROR = 6,
    BME688_IPC_STATUS_TIMEOUT = 7,
    BME688_IPC_STATUS_RECOVERING = 8
} bme688_ipc_status_t;

enum
{
    BME688_IPC_FLAG_NEW_DATA = 1U << 0,
    BME688_IPC_FLAG_GAS_VALID = 1U << 1,
    BME688_IPC_FLAG_HEATER_STABLE = 1U << 2
};

typedef struct st_scd41_ipc_mailbox
{
    uint32_t magic;
    volatile uint32_t publish_lock;
    volatile uint32_t sequence;
    volatile uint32_t status;
    volatile uint32_t co2_ppm;
    volatile int32_t temperature_milli_c;
    volatile uint32_t humidity_milli_percent;
    volatile uint32_t raw_co2;
    volatile uint32_t raw_temperature;
    volatile uint32_t raw_humidity;
    volatile uint32_t data_ready;
    volatile uint32_t serial_words[3];
    volatile uint32_t last_good_tick;
    volatile uint32_t read_count;
    volatile uint32_t crc_error_count;
    volatile uint32_t i2c_error_count;
    volatile uint32_t timeout_count;
    volatile uint32_t recovery_count;
    volatile uint32_t consecutive_failures;
    volatile uint32_t last_fsp_err;
    volatile uint32_t last_event;
    volatile uint32_t bme_status;
    volatile uint32_t bme_i2c_address;
    volatile int32_t bme_temperature_milli_c;
    volatile uint32_t bme_humidity_milli_percent;
    volatile uint32_t bme_pressure_pa;
    volatile uint32_t bme_gas_resistance_ohm;
    volatile uint32_t bme_flags;
    volatile uint32_t bme_last_good_tick;
    volatile uint32_t bme_error_count;
} scd41_ipc_mailbox_t;

typedef struct st_scd41_ipc_snapshot
{
    uint32_t magic;
    uint32_t sequence;
    uint32_t status;
    uint32_t co2_ppm;
    int32_t temperature_milli_c;
    uint32_t humidity_milli_percent;
    uint32_t raw_co2;
    uint32_t raw_temperature;
    uint32_t raw_humidity;
    uint32_t data_ready;
    uint32_t serial_words[3];
    uint32_t last_good_tick;
    uint32_t read_count;
    uint32_t crc_error_count;
    uint32_t i2c_error_count;
    uint32_t timeout_count;
    uint32_t recovery_count;
    uint32_t consecutive_failures;
    uint32_t last_fsp_err;
    uint32_t last_event;
    uint32_t bme_status;
    uint32_t bme_i2c_address;
    int32_t bme_temperature_milli_c;
    uint32_t bme_humidity_milli_percent;
    uint32_t bme_pressure_pa;
    uint32_t bme_gas_resistance_ohm;
    uint32_t bme_flags;
    uint32_t bme_last_good_tick;
    uint32_t bme_error_count;
} scd41_ipc_snapshot_t;

typedef char scd41_ipc_mailbox_size_guard[
    (sizeof(scd41_ipc_mailbox_t) <= SCD41_IPC_REGION_SIZE) ? 1 : -1];
typedef char scd41_ipc_mailbox_layout_guard[
    (sizeof(scd41_ipc_mailbox_t) == SCD41_IPC_REGION_SIZE) ? 1 : -1];

#define SCD41_IPC_MAILBOX ((volatile scd41_ipc_mailbox_t *) SCD41_IPC_ADDRESS)

/*
 * Read one cross-core-consistent mailbox image.  CPU1 holds publish_lock odd
 * while updating fields and commits an even value only after the final DMB.
 */
static inline bool scd41_ipc_read_snapshot(scd41_ipc_snapshot_t * p_snapshot)
{
    volatile scd41_ipc_mailbox_t const * p_mailbox = SCD41_IPC_MAILBOX;

    if (NULL == p_snapshot)
    {
        return false;
    }

    for (uint32_t attempt = 0U; attempt < SCD41_IPC_SNAPSHOT_ATTEMPTS; attempt++)
    {
        uint32_t before = p_mailbox->publish_lock;
        uint32_t after;

        if (0U != (before & 1U))
        {
            continue;
        }

        __DMB();
        p_snapshot->magic = p_mailbox->magic;
        p_snapshot->sequence = p_mailbox->sequence;
        p_snapshot->status = p_mailbox->status;
        p_snapshot->co2_ppm = p_mailbox->co2_ppm;
        p_snapshot->temperature_milli_c = p_mailbox->temperature_milli_c;
        p_snapshot->humidity_milli_percent = p_mailbox->humidity_milli_percent;
        p_snapshot->raw_co2 = p_mailbox->raw_co2;
        p_snapshot->raw_temperature = p_mailbox->raw_temperature;
        p_snapshot->raw_humidity = p_mailbox->raw_humidity;
        p_snapshot->data_ready = p_mailbox->data_ready;
        p_snapshot->serial_words[0] = p_mailbox->serial_words[0];
        p_snapshot->serial_words[1] = p_mailbox->serial_words[1];
        p_snapshot->serial_words[2] = p_mailbox->serial_words[2];
        p_snapshot->last_good_tick = p_mailbox->last_good_tick;
        p_snapshot->read_count = p_mailbox->read_count;
        p_snapshot->crc_error_count = p_mailbox->crc_error_count;
        p_snapshot->i2c_error_count = p_mailbox->i2c_error_count;
        p_snapshot->timeout_count = p_mailbox->timeout_count;
        p_snapshot->recovery_count = p_mailbox->recovery_count;
        p_snapshot->consecutive_failures = p_mailbox->consecutive_failures;
        p_snapshot->last_fsp_err = p_mailbox->last_fsp_err;
        p_snapshot->last_event = p_mailbox->last_event;
        p_snapshot->bme_status = p_mailbox->bme_status;
        p_snapshot->bme_i2c_address = p_mailbox->bme_i2c_address;
        p_snapshot->bme_temperature_milli_c = p_mailbox->bme_temperature_milli_c;
        p_snapshot->bme_humidity_milli_percent = p_mailbox->bme_humidity_milli_percent;
        p_snapshot->bme_pressure_pa = p_mailbox->bme_pressure_pa;
        p_snapshot->bme_gas_resistance_ohm = p_mailbox->bme_gas_resistance_ohm;
        p_snapshot->bme_flags = p_mailbox->bme_flags;
        p_snapshot->bme_last_good_tick = p_mailbox->bme_last_good_tick;
        p_snapshot->bme_error_count = p_mailbox->bme_error_count;
        __DMB();

        after = p_mailbox->publish_lock;
        if ((before == after) && (0U == (after & 1U)))
        {
            return true;
        }
    }

    return false;
}

#endif /* SCD41_IPC_SHARED_H */

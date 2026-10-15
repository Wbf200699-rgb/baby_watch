#ifndef SCD41_DRIVER_H
#define SCD41_DRIVER_H

#include "scd41_thread.h"
#include "thread_code/scd41_ipc_shared.h"

fsp_err_t scd41_driver_init(void);
fsp_err_t scd41_driver_poll(void);
fsp_err_t scd41_i2c_shared_recover(void);
fsp_err_t scd41_i2c_shared_write(uint8_t address, uint8_t * p_data, uint32_t length);
fsp_err_t scd41_i2c_shared_write_read(uint8_t address,
                                     uint8_t * p_write,
                                     uint32_t write_length,
                                     uint8_t * p_read,
                                     uint32_t read_length);
uint8_t scd41_crc8(uint8_t const * p_data, uint32_t length);
void scd41_i2c_callback(i2c_master_callback_args_t * p_args);

#endif /* SCD41_DRIVER_H */

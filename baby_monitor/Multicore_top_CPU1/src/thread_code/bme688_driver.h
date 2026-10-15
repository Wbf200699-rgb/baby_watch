#ifndef BME688_DRIVER_H
#define BME688_DRIVER_H

#include "scd41_thread.h"

fsp_err_t bme688_driver_init(void);
fsp_err_t bme688_driver_poll(void);

#endif /* BME688_DRIVER_H */

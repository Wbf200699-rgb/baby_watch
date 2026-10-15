#ifndef SERIAL_CAMERA_DRIVER_H
#define SERIAL_CAMERA_DRIVER_H

#include "bsp_api.h"

#define SERIAL_CAMERA_AUTO_INFERENCE_ENABLED     (1U)
#define SERIAL_CAMERA_AUTO_INFERENCE_INTERVAL_MS (1000U)

fsp_err_t serial_camera_driver_init(void);
fsp_err_t serial_camera_probe(void);
fsp_err_t serial_camera_capture_once(void);
void serial_camera_poll(void);

#endif /* SERIAL_CAMERA_DRIVER_H */

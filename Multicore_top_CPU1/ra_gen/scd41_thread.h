/* generated thread header file - do not edit */
#ifndef SCD41_THREAD_H_
#define SCD41_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void scd41_thread_entry(void * pvParameters);
                #else
                extern void scd41_thread_entry(void * pvParameters);
                #endif
#include "r_iic_master.h"
#include "r_i2c_master_api.h"
FSP_HEADER
/* I2C Master on IIC Instance. */
extern const i2c_master_instance_t g_scd41_i2c;

/** Access the I2C Master instance using these structures when calling API functions directly (::p_api is not used). */
extern iic_master_instance_ctrl_t g_scd41_i2c_ctrl;
extern const i2c_master_cfg_t g_scd41_i2c_cfg;

#ifndef scd41_i2c_callback
void scd41_i2c_callback(i2c_master_callback_args_t * p_args);
#endif
FSP_FOOTER
#endif /* SCD41_THREAD_H_ */

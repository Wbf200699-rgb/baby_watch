#ifndef TMP117_MATRIX_DRIVER_H
#define TMP117_MATRIX_DRIVER_H

#include "temp_thread.h"
#include "thread_code/pressure_matrix_driver.h"
#include "thread_code/tmp117_ipc_shared.h"

#define TMP117_MATRIX_SENSOR_COUNT TMP117_IPC_SENSOR_COUNT

typedef enum e_tmp117_matrix_step
{
    TMP117_MATRIX_STEP_NONE = 0,
    TMP117_MATRIX_STEP_OPEN,
    TMP117_MATRIX_STEP_SELECT_MUX,
    TMP117_MATRIX_STEP_READ_TEMP,
    TMP117_MATRIX_STEP_SCAN_DONE
} tmp117_matrix_step_t;

typedef struct st_tmp117_matrix_debug
{
    uint32_t magic;
    uint32_t build_marker;
    volatile uint32_t sequence;
    volatile tmp117_matrix_step_t step;
    volatile uint32_t channel;
    volatile uint32_t connected_mask;
    volatile uint32_t error_mask;
    volatile tmp117_matrix_step_t fail_step;
    volatile uint32_t fail_channel;
    volatile uint32_t pca_address;
    volatile fsp_err_t last_fsp_err;
    volatile uint32_t recovery_count;
    volatile uint32_t recovery_error_count;
    volatile uint32_t retry_success_count;
    volatile fsp_err_t channel_err[TMP117_MATRIX_SENSOR_COUNT];
    volatile tmp117_matrix_step_t channel_step[TMP117_MATRIX_SENSOR_COUNT];
    volatile uint32_t address[TMP117_MATRIX_SENSOR_COUNT];
    volatile int16_t raw[TMP117_MATRIX_SENSOR_COUNT];
    volatile int32_t milli_c[TMP117_MATRIX_SENSOR_COUNT];
} tmp117_matrix_debug_t;

extern volatile tmp117_matrix_debug_t g_tmp117_debug;
extern const volatile uint32_t g_tmp117_build_marker;

fsp_err_t tmp117_matrix_init(void);
fsp_err_t tmp117_matrix_scan(void);
void temp_i2c_callback(i2c_master_callback_args_t * p_args);

#endif /* TMP117_MATRIX_DRIVER_H */

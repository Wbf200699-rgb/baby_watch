#ifndef PRESSURE_MATRIX_DRIVER_H
#define PRESSURE_MATRIX_DRIVER_H

#include "temp_thread.h"
#include "r_uart_api.h"
#include "thread_code/pressure_ipc_shared.h"

#define PRESSURE_MATRIX_RAW_COUNT PRESSURE_IPC_RAW_COUNT

typedef enum e_pressure_matrix_step
{
    PRESSURE_MATRIX_STEP_NONE = 0,
    PRESSURE_MATRIX_STEP_OPEN,
    PRESSURE_MATRIX_STEP_SELECT_UPSTREAM,
    PRESSURE_MATRIX_STEP_SELECT_DOWNSTREAM,
    PRESSURE_MATRIX_STEP_DETECT_UPSTREAM,
    PRESSURE_MATRIX_STEP_CONFIG_ADS,
    PRESSURE_MATRIX_STEP_READ_ADS,
    PRESSURE_MATRIX_STEP_SCAN_DONE
} pressure_matrix_step_t;

typedef struct st_pressure_matrix_debug
{
    uint32_t magic;
    uint32_t build_marker;
    volatile uint32_t sequence;
    volatile pressure_matrix_step_t step;
    volatile uint32_t channel;
    volatile uint32_t adc_channel;
    volatile uint32_t connected_mask;
    volatile uint32_t error_mask;
    volatile pressure_matrix_step_t fail_step;
    volatile uint32_t fail_channel;
    volatile fsp_err_t last_fsp_err;
    volatile uint32_t upstream_pca_address;
    volatile uint32_t downstream_pca_address;
    volatile uint32_t detected_upstream_channel;
    volatile uint32_t ads_address[8];
    volatile int16_t raw[PRESSURE_MATRIX_RAW_COUNT];
    volatile int32_t millivolt[PRESSURE_MATRIX_RAW_COUNT];
} pressure_matrix_debug_t;

extern volatile pressure_matrix_debug_t g_pressure_debug;

fsp_err_t pressure_matrix_init(void);
fsp_err_t pressure_matrix_scan(void);
fsp_err_t pressure_matrix_set_mapping(uint32_t raw_index, uint32_t matrix_index);
void pressure_matrix_reset_mapping(void);
void pressure_i2c_callback(i2c_master_callback_args_t * p_args);
void m1616_uart_callback(uart_callback_args_t * p_args);
void pressure_i2c_bus_init(void);
void pressure_i2c_bus_lock(void);
void pressure_i2c_bus_unlock(void);

#endif /* PRESSURE_MATRIX_DRIVER_H */

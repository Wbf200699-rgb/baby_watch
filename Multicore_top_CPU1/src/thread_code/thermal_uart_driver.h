#ifndef THERMAL_UART_DRIVER_H
#define THERMAL_UART_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "bsp_api.h"
#include "FreeRTOS.h"
#include "task.h"
#include "thermal_thread.h"
#include "thread_code/thermal_ipc_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THERMAL_UART_FRAME_LENGTH (10248U)

typedef struct st_thermal_uart_stats
{
    uint32_t rx_bytes;
    uint32_t rx_overflow_count;
    uint32_t uart_error_count;
    uint32_t parity_error_count;
    uint32_t framing_error_count;
    uint32_t fifo_overflow_count;
    uint32_t crc_error_count;
    uint32_t data_error_count;
    uint32_t baud_set_count;
    uint32_t tx_complete_count;
    uart_event_t last_event;
} thermal_uart_stats_t;

fsp_err_t thermal_uart_driver_init(void);
bool thermal_uart_request_frame(TickType_t timeout_ticks);
void thermal_uart_tx_probe_loop(void);
void thermal_uart_get_stats(thermal_uart_stats_t * p_stats);
void thermal_uart_callback(uart_callback_args_t * p_args);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_UART_DRIVER_H */

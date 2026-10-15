#ifndef ESP32_UART_DRIVER_H
#define ESP32_UART_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp_api.h"
#include "FreeRTOS.h"
#include "task.h"
#include "esp32_uart_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP32_UART_RX_BUFFER_SIZE    (32768U)
#define ESP32_UART_RX_TRACE_SIZE     (64U)

#if (0U != (ESP32_UART_RX_BUFFER_SIZE & (ESP32_UART_RX_BUFFER_SIZE - 1U)))
#error "ESP32_UART_RX_BUFFER_SIZE must be a power of two"
#endif

typedef enum e_esp32_uart_status
{
    ESP32_UART_STATUS_OK = 0,
    ESP32_UART_STATUS_ERROR = -1,
    ESP32_UART_STATUS_BUSY = -2,
    ESP32_UART_STATUS_TIMEOUT = -3
} esp32_uart_status_t;

typedef struct st_esp32_uart_stats
{
    uint32_t rx_bytes;
    uint32_t tx_complete_count;
    uint32_t tx_busy_count;
    uint32_t tx_timeout_count;
    uint32_t error_count;
    uint32_t rx_overflow_count;
    uint32_t recovery_count;
    uint32_t recovery_error_count;
    uart_event_t last_event;
} esp32_uart_stats_t;

fsp_err_t esp32_uart_driver_init(void);
fsp_err_t esp32_uart_driver_recover(void);
esp32_uart_status_t esp32_uart_driver_write(const uint8_t * p_data, uint32_t length, TickType_t timeout_ticks);
uint32_t esp32_uart_driver_read(uint8_t * p_buffer, uint32_t buffer_size);
bool esp32_uart_driver_is_initialized(void);
void esp32_uart_driver_get_stats(esp32_uart_stats_t * p_stats);
void esp32_uart_callback(uart_callback_args_t * p_args);

/* Debugger-visible rolling capture of bytes observed by the SCI0 RX ISR. */
extern volatile uint8_t g_esp32_uart_rx_trace[ESP32_UART_RX_TRACE_SIZE];
extern volatile uint32_t g_esp32_uart_rx_trace_count;
extern volatile uint32_t g_esp32_uart_rx_trace_write;

#ifdef __cplusplus
}
#endif

#endif /* ESP32_UART_DRIVER_H */

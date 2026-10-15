#include "thread_code/esp32_uart_driver.h"

#include <string.h>

#include "task.h"

typedef struct st_esp32_uart_context
{
    volatile bool initialized;
    volatile bool tx_done;
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
    uint8_t rx_buffer[ESP32_UART_RX_BUFFER_SIZE];
    esp32_uart_stats_t stats;
} esp32_uart_context_t;

static esp32_uart_context_t g_esp32_uart_context;
volatile uint8_t g_esp32_uart_rx_trace[ESP32_UART_RX_TRACE_SIZE];
volatile uint32_t g_esp32_uart_rx_trace_count;
volatile uint32_t g_esp32_uart_rx_trace_write;

static void esp32_uart_reset_rx_buffer(void)
{
    taskENTER_CRITICAL();
    g_esp32_uart_context.rx_head = 0U;
    g_esp32_uart_context.rx_tail = 0U;
    taskEXIT_CRITICAL();
}

static fsp_err_t esp32_uart_apply_baud(void)
{
    /*
     * Use the baud settings generated from the project's configured SCI
     * clock.  Recalculating here used R_FSP_SciClockHzGet(), which reports a
     * different runtime clock domain on this multicore build and produced
     * BRR=0 instead of the generated BRR=14.  The result was a physically
     * wrong baud rate even though both endpoints reported 921600 baud.
     */
    return g_esp32_uart.p_api->baudSet(g_esp32_uart.p_ctrl,
                                       g_esp32_uart_cfg_extend.p_baud_setting);
}

static inline uint16_t esp32_uart_next_index(uint16_t index)
{
    return (uint16_t) ((index + 1U) & (ESP32_UART_RX_BUFFER_SIZE - 1U));
}

static void esp32_uart_push_byte(uint8_t byte)
{
    uint16_t next_head = esp32_uart_next_index(g_esp32_uart_context.rx_head);
    uint32_t trace_index = g_esp32_uart_rx_trace_write;

    g_esp32_uart_rx_trace[trace_index] = byte;
    g_esp32_uart_rx_trace_write = (trace_index + 1U) % ESP32_UART_RX_TRACE_SIZE;
    g_esp32_uart_rx_trace_count++;

    if (next_head == g_esp32_uart_context.rx_tail)
    {
        /* Keep the newest bytes so a fresh AA 55 frame can resynchronize the
         * parser after a temporary scheduling stall. */
        g_esp32_uart_context.rx_tail = esp32_uart_next_index(g_esp32_uart_context.rx_tail);
        g_esp32_uart_context.stats.rx_overflow_count++;
    }

    g_esp32_uart_context.rx_buffer[g_esp32_uart_context.rx_head] = byte;
    g_esp32_uart_context.rx_head = next_head;
    g_esp32_uart_context.stats.rx_bytes++;
}

fsp_err_t esp32_uart_driver_init(void)
{
    fsp_err_t err;

    if (g_esp32_uart_context.initialized)
    {
        return FSP_SUCCESS;
    }

    g_esp32_uart_context.rx_head = 0U;
    g_esp32_uart_context.rx_tail = 0U;
    g_esp32_uart_context.tx_done = true;
    g_esp32_uart_context.stats.rx_bytes = 0U;
    g_esp32_uart_context.stats.tx_complete_count = 0U;
    g_esp32_uart_context.stats.tx_busy_count = 0U;
    g_esp32_uart_context.stats.tx_timeout_count = 0U;
    g_esp32_uart_context.stats.error_count = 0U;
    g_esp32_uart_context.stats.rx_overflow_count = 0U;
    g_esp32_uart_context.stats.recovery_count = 0U;
    g_esp32_uart_context.stats.recovery_error_count = 0U;
    g_esp32_uart_context.stats.last_event = (uart_event_t) 0;
    g_esp32_uart_rx_trace_count = 0U;
    g_esp32_uart_rx_trace_write = 0U;
    memset((void *) g_esp32_uart_rx_trace, 0, sizeof(g_esp32_uart_rx_trace));

    err = g_esp32_uart.p_api->open(g_esp32_uart.p_ctrl, g_esp32_uart.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return err;
    }

    err = esp32_uart_apply_baud();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    g_esp32_uart_context.initialized = true;

    return FSP_SUCCESS;
}

fsp_err_t esp32_uart_driver_recover(void)
{
    fsp_err_t err;

    if (!g_esp32_uart_context.initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if (!g_esp32_uart_context.tx_done)
    {
        return FSP_ERR_IN_USE;
    }

    (void) g_esp32_uart.p_api->communicationAbort(g_esp32_uart.p_ctrl, UART_DIR_RX);
    esp32_uart_reset_rx_buffer();

    err = esp32_uart_apply_baud();
    if (FSP_SUCCESS == err)
    {
        g_esp32_uart_context.stats.recovery_count++;
    }

    return err;
}

esp32_uart_status_t esp32_uart_driver_write(const uint8_t * p_data, uint32_t length, TickType_t timeout_ticks)
{
    fsp_err_t err;
    TickType_t start_ticks;

    if ((!g_esp32_uart_context.initialized) || (NULL == p_data) || (0U == length))
    {
        return ESP32_UART_STATUS_ERROR;
    }

    if (!g_esp32_uart_context.tx_done)
    {
        g_esp32_uart_context.stats.tx_busy_count++;
        return ESP32_UART_STATUS_BUSY;
    }

    g_esp32_uart_context.tx_done = false;

    err = g_esp32_uart.p_api->write(g_esp32_uart.p_ctrl, p_data, length);
    if (FSP_ERR_IN_USE == err)
    {
        g_esp32_uart_context.stats.tx_busy_count++;
        g_esp32_uart_context.tx_done = true;
        return ESP32_UART_STATUS_BUSY;
    }

    if (FSP_SUCCESS != err)
    {
        g_esp32_uart_context.tx_done = true;
        return ESP32_UART_STATUS_ERROR;
    }

    start_ticks = xTaskGetTickCount();

    while (!g_esp32_uart_context.tx_done)
    {
        if ((timeout_ticks > 0U) && ((xTaskGetTickCount() - start_ticks) >= timeout_ticks))
        {
            g_esp32_uart_context.stats.tx_timeout_count++;
            (void) g_esp32_uart.p_api->communicationAbort(g_esp32_uart.p_ctrl, UART_DIR_TX);
            g_esp32_uart_context.tx_done = true;
            return ESP32_UART_STATUS_TIMEOUT;
        }

        vTaskDelay(1);
    }

    return ESP32_UART_STATUS_OK;
}

uint32_t esp32_uart_driver_read(uint8_t * p_buffer, uint32_t buffer_size)
{
    uint32_t count = 0U;

    if ((NULL == p_buffer) || (0U == buffer_size))
    {
        return 0U;
    }

    taskENTER_CRITICAL();
    while ((g_esp32_uart_context.rx_tail != g_esp32_uart_context.rx_head) && (count < buffer_size))
    {
        p_buffer[count] = g_esp32_uart_context.rx_buffer[g_esp32_uart_context.rx_tail];
        g_esp32_uart_context.rx_tail = esp32_uart_next_index(g_esp32_uart_context.rx_tail);
        count++;
    }
    taskEXIT_CRITICAL();

    return count;
}

bool esp32_uart_driver_is_initialized(void)
{
    return g_esp32_uart_context.initialized;
}

void esp32_uart_driver_get_stats(esp32_uart_stats_t * p_stats)
{
    if (NULL == p_stats)
    {
        return;
    }

    taskENTER_CRITICAL();
    *p_stats = g_esp32_uart_context.stats;
    taskEXIT_CRITICAL();
}

void esp32_uart_callback(uart_callback_args_t * p_args)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (NULL == p_args)
    {
        return;
    }

    g_esp32_uart_context.stats.last_event = p_args->event;

    switch (p_args->event)
    {
        case UART_EVENT_RX_CHAR:
        case UART_EVENT_ERR_PARITY:
        case UART_EVENT_ERR_FRAMING:
        case UART_EVENT_ERR_OVERFLOW:
        {
            if (UART_EVENT_RX_CHAR != p_args->event)
            {
                g_esp32_uart_context.stats.error_count++;
            }

            /* SCI_B supplies the received data together with an error event.
             * Preserve it so the framed protocol can still validate CRC and
             * recover on a marginal high-speed link. */
            esp32_uart_push_byte((uint8_t) p_args->data);
            break;
        }

        case UART_EVENT_TX_COMPLETE:
        {
            g_esp32_uart_context.stats.tx_complete_count++;
            g_esp32_uart_context.tx_done = true;
            break;
        }

        default:
        {
            break;
        }
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
}

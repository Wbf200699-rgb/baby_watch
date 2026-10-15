#ifndef STORY_BULK_UART_H
#define STORY_BULK_UART_H

#include <stdbool.h>
#include <stdint.h>

#include "bsp_api.h"
#include "r_uart_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORY_BULK_BAUD_RATE        (1000000U)
#define STORY_BULK_PAYLOAD_MAX      (4096U)

typedef struct st_story_bulk_uart_debug
{
    uint32_t magic;
    volatile uint32_t initialized;
    volatile uint32_t open_error;
    volatile uint32_t read_error;
    volatile uint32_t write_error;
    volatile uint32_t rx_frames;
    volatile uint32_t rx_bytes;
    volatile uint32_t first_rx_tick;
    volatile uint32_t sync_count;
    volatile uint32_t resync_count;
    volatile uint32_t ring_overflow_count;
    volatile uint32_t ack_frames;
    volatile uint32_t ready_frames;
    volatile uint32_t tx_complete_count;
    volatile uint32_t handshake_state;
    volatile uint32_t actual_baud;
    volatile uint32_t protocol_errors;
    volatile uint32_t crc_errors;
    volatile uint32_t last_transfer_id;
    volatile uint32_t last_sequence;
    volatile uint32_t next_offset;
    volatile uint32_t last_result;
} story_bulk_uart_debug_t;

extern volatile story_bulk_uart_debug_t g_story_bulk_uart_debug;

void story_bulk_uart_init(void);
void story_bulk_uart_poll(bool sd_ready);
bool story_bulk_uart_active(void);
void story_bulk_uart_callback(uart_callback_args_t * p_args);

#ifdef __cplusplus
}
#endif

#endif /* STORY_BULK_UART_H */

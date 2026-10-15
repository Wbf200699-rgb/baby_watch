#ifndef TMP117_IPC_SHARED_H
#define TMP117_IPC_SHARED_H

#include <stdint.h>

#define TMP117_IPC_MAGIC        (0x544D5031U) /* TMP1 */
#define TMP117_IPC_ADDRESS      (0x221D0100UL)
#define TMP117_IPC_SENSOR_COUNT (5U)

typedef enum e_tmp117_ipc_status
{
    TMP117_IPC_STATUS_EMPTY = 0,
    TMP117_IPC_STATUS_OPEN_ERROR,
    TMP117_IPC_STATUS_SCAN_OK,
    TMP117_IPC_STATUS_SCAN_PARTIAL,
    TMP117_IPC_STATUS_SCAN_ERROR
} tmp117_ipc_status_t;

typedef struct st_tmp117_ipc_mailbox
{
    uint32_t magic;
    volatile uint32_t sequence;
    volatile uint32_t status;
    volatile uint32_t connected_mask;
    volatile uint32_t error_mask;
    volatile uint32_t fail_step;
    volatile uint32_t fail_channel;
    volatile uint32_t pca_address;
    volatile uint32_t address[TMP117_IPC_SENSOR_COUNT];
    volatile int16_t raw[TMP117_IPC_SENSOR_COUNT];
    volatile int32_t milli_c[TMP117_IPC_SENSOR_COUNT];
    volatile uint32_t last_fsp_err;
} tmp117_ipc_mailbox_t;

#define TMP117_IPC_MAILBOX ((volatile tmp117_ipc_mailbox_t *) TMP117_IPC_ADDRESS)

#endif /* TMP117_IPC_SHARED_H */

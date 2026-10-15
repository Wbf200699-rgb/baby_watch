#ifndef PRESSURE_IPC_SHARED_H
#define PRESSURE_IPC_SHARED_H

#include <stdint.h>

#define PRESSURE_IPC_MAGIC          (0x50525331U) /* PRS1 */
#define PRESSURE_IPC_ADDRESS        (0x221D3400UL)
#define PRESSURE_IPC_RAW_COUNT      (32U)
#define PRESSURE_IPC_MATRIX_WIDTH   (16U)
#define PRESSURE_IPC_MATRIX_HEIGHT  (16U)
#define PRESSURE_IPC_MATRIX_COUNT   (PRESSURE_IPC_MATRIX_WIDTH * PRESSURE_IPC_MATRIX_HEIGHT)

typedef enum e_pressure_ipc_status
{
    PRESSURE_IPC_STATUS_EMPTY = 0,
    PRESSURE_IPC_STATUS_OPEN_ERROR,
    PRESSURE_IPC_STATUS_SCAN_OK,
    PRESSURE_IPC_STATUS_SCAN_PARTIAL,
    PRESSURE_IPC_STATUS_SCAN_ERROR
} pressure_ipc_status_t;

typedef struct st_pressure_ipc_mailbox
{
    uint32_t magic;
    volatile uint32_t sequence;
    volatile uint32_t status;
    volatile uint32_t raw_count;
    volatile uint32_t connected_mask;
    volatile uint32_t error_mask;
    volatile uint32_t fail_step;
    volatile uint32_t fail_channel;
    volatile uint32_t upstream_pca_address;
    volatile uint32_t downstream_pca_address;
    volatile uint32_t detected_upstream_channel;
    volatile uint32_t ads_address[8];
    volatile uint32_t last_fsp_err;
    volatile int16_t raw[PRESSURE_IPC_RAW_COUNT];
    volatile int32_t millivolt[PRESSURE_IPC_RAW_COUNT];
    volatile int16_t matrix[PRESSURE_IPC_MATRIX_COUNT];
} pressure_ipc_mailbox_t;

#define PRESSURE_IPC_MAILBOX ((volatile pressure_ipc_mailbox_t *) PRESSURE_IPC_ADDRESS)

#endif /* PRESSURE_IPC_SHARED_H */

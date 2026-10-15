#ifndef SD_IPC_SHARED_H
#define SD_IPC_SHARED_H

#include <stdint.h>

#define SD_IPC_MAGIC       (0x53444931U) /* SDI1 */
#define SD_IPC_VERSION     (3U)
#define SD_IPC_ADDRESS     (0x221D3F00UL)

#define SD_IPC_FLAG_OPENED             (1UL << 0)
#define SD_IPC_FLAG_MEDIA_INITIALIZED  (1UL << 1)
#define SD_IPC_FLAG_DISK_INITIALIZED   (1UL << 2)
#define SD_IPC_FLAG_MOUNTED            (1UL << 3)
#define SD_IPC_FLAG_CARD_INSERTED      (1UL << 4)
#define SD_IPC_FLAG_FS_REGISTERED      (1UL << 5)

typedef struct st_sd_ipc_mailbox
{
    volatile uint32_t magic;
    volatile uint32_t version;
    volatile uint32_t sequence;
    volatile uint32_t ready;
    volatile uint32_t step;
    volatile uint32_t failure_step;
    volatile uint32_t failure_line;
    volatile uint32_t flags;
    volatile uint32_t init_attempts;
    volatile uint32_t consecutive_failures;
    volatile uint32_t recovery_count;
    volatile uint32_t last_success_tick;
    volatile uint32_t init_err;
    volatile uint32_t last_fsp_err;
    volatile uint32_t last_ff_err;
    volatile uint32_t close_err;
    volatile uint32_t disk_deinit_err;
    volatile uint32_t unmount_err;
    volatile uint32_t resource_generation;
    volatile uint32_t resource_sequence;
    volatile uint32_t resource_tick;
    volatile uint32_t cpu_usage_permille;
    volatile uint32_t heap_free_bytes;
    volatile uint32_t heap_min_bytes;
} sd_ipc_mailbox_t;

#define SD_IPC_MAILBOX ((volatile sd_ipc_mailbox_t *) SD_IPC_ADDRESS)

#endif /* SD_IPC_SHARED_H */

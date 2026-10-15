#ifndef SD_CARD_DRIVER_H
#define SD_CARD_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "common_data.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_CARD_MOUNT_PATH          "/sd0"
#define SD_CARD_TEST_FILE_PATH      "/sd0/sd_test.txt"
#define SD_CARD_DEBUG_EXPORT_ADDRESS     (0x221D3F80UL)
#define SD_CARD_DEBUG_EXPORT_MAGIC       (0x53444558U) /* SDEX */
#define SD_CARD_DEBUG_EXPORT_VERSION     (1U)
#define SD_CARD_DEBUG_EXPORT_REGION_BYTES (0x80U)
#define SD_CARD_DEBUG_EXPORT_PATH_BYTES  (64U)
#define SD_CARD_DEBUG_EXPORT_BUFFER_BYTES (32768U)

typedef enum e_sd_card_debug_export_operation
{
    SD_CARD_DEBUG_EXPORT_OPERATION_NONE = 0,
    SD_CARD_DEBUG_EXPORT_OPERATION_OPEN = 1,
    SD_CARD_DEBUG_EXPORT_OPERATION_READ = 2,
    SD_CARD_DEBUG_EXPORT_OPERATION_CLOSE = 3
} sd_card_debug_export_operation_t;

typedef enum e_sd_card_debug_export_status
{
    SD_CARD_DEBUG_EXPORT_STATUS_IDLE = 0,
    SD_CARD_DEBUG_EXPORT_STATUS_BUSY = 1,
    SD_CARD_DEBUG_EXPORT_STATUS_OK = 2,
    SD_CARD_DEBUG_EXPORT_STATUS_NOT_READY = 0x80000001U,
    SD_CARD_DEBUG_EXPORT_STATUS_BAD_REQUEST = 0x80000002U,
    SD_CARD_DEBUG_EXPORT_STATUS_OPEN_FAILED = 0x80000003U,
    SD_CARD_DEBUG_EXPORT_STATUS_SEEK_FAILED = 0x80000004U,
    SD_CARD_DEBUG_EXPORT_STATUS_READ_FAILED = 0x80000005U,
    SD_CARD_DEBUG_EXPORT_STATUS_NOT_OPEN = 0x80000006U,
    SD_CARD_DEBUG_EXPORT_STATUS_CLOSE_FAILED = 0x80000007U
} sd_card_debug_export_status_t;

/*
 * Read-only debugger mailbox at the top 128 bytes of shared SRAM.  J-Link
 * writes a request and reads the response plus the regular CPU1 SRAM buffer.
 * The filesystem thread services it while holding the same recursive FAT lock
 * used by all writers, so a debug export cannot race normal file I/O.
 */
typedef struct st_sd_card_debug_export
{
    volatile uint32_t magic;
    volatile uint32_t version;
    volatile uint32_t request_sequence;
    volatile uint32_t response_sequence;
    volatile uint32_t operation;
    volatile uint32_t status;
    volatile uint32_t file_size;
    volatile uint32_t request_offset;
    volatile uint32_t request_length;
    volatile uint32_t response_offset;
    volatile uint32_t bytes_ready;
    volatile uint32_t buffer_address;
    volatile uint32_t buffer_capacity;
    volatile uint32_t chunk_checksum;
    volatile uint32_t last_error;
    volatile uint32_t reserved;
    volatile char path[SD_CARD_DEBUG_EXPORT_PATH_BYTES];
} sd_card_debug_export_t;

#define SD_CARD_DEBUG_EXPORT \
    ((volatile sd_card_debug_export_t *) SD_CARD_DEBUG_EXPORT_ADDRESS)

typedef enum e_sd_card_driver_status
{
    SD_CARD_DRIVER_STATUS_OK = 0,
    SD_CARD_DRIVER_STATUS_ERROR = -1,
    SD_CARD_DRIVER_STATUS_TIMEOUT = -2
} sd_card_driver_status_t;

typedef enum e_sd_card_debug_step
{
    SD_CARD_DEBUG_STEP_NONE = 0,
    SD_CARD_DEBUG_STEP_OPEN,
    SD_CARD_DEBUG_STEP_MEDIA_INIT,
    SD_CARD_DEBUG_STEP_DISK_INIT,
    SD_CARD_DEBUG_STEP_MOUNT,
    SD_CARD_DEBUG_STEP_FS_ADD,
    SD_CARD_DEBUG_STEP_INFO_GET,
    SD_CARD_DEBUG_STEP_STATUS_GET,
    SD_CARD_DEBUG_STEP_FILE_OPEN,
    SD_CARD_DEBUG_STEP_FILE_WRITE,
    SD_CARD_DEBUG_STEP_FILE_CLOSE,
    SD_CARD_DEBUG_STEP_DONE,
    SD_CARD_DEBUG_STEP_RUNTIME_IO
} sd_card_debug_step_t;

typedef struct st_sd_card_info
{
    bool opened;
    bool media_initialized;
    bool disk_initialized;
    bool mounted;
    bool card_inserted;
    uint32_t sector_count;
    uint32_t sector_size_bytes;
    uint32_t free_sectors;
    uint32_t total_sectors;
    fsp_err_t last_fsp_err;
    FF_Error_t last_ff_err;
} sd_card_info_t;

typedef struct st_sd_card_debug_state
{
    uint32_t magic;
    uint32_t build_marker;
    sd_card_debug_step_t step;
    uint32_t line;
    fsp_err_t init_err;
    fsp_err_t last_fsp_err;
    FF_Error_t last_ff_err;
    int fs_add_err;
    int write_result;
    int close_result;
    sd_card_driver_status_t write_status;
    sd_card_info_t info;
    uint32_t init_attempts;
    uint32_t consecutive_failures;
    uint32_t recovery_count;
    uint32_t last_success_tick;
    sd_card_debug_step_t failure_step;
    uint32_t failure_line;
    fsp_err_t close_err;
    fsp_err_t disk_deinit_err;
    FF_Error_t unmount_err;
} sd_card_debug_state_t;

extern volatile sd_card_debug_state_t g_sd_card_debug;
extern volatile const uint32_t g_sd_card_build_marker;

void sd_card_driver_prepare(void);
fsp_err_t sd_card_driver_init(void);
fsp_err_t sd_card_driver_mount(void);
fsp_err_t sd_card_driver_refresh_status(void);
bool sd_card_driver_is_ready(void);
void sd_card_driver_get_info(sd_card_info_t * p_info);
sd_card_driver_status_t sd_card_driver_write_test_file(void);
void sd_card_driver_report_io_error(int io_errno);
bool sd_card_driver_fs_lock(TickType_t timeout_ticks);
void sd_card_driver_fs_unlock(void);
void sd_card_driver_debug_export_poll(bool sd_ready);

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_DRIVER_H */

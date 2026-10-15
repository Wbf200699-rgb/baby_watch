#ifndef SERIAL_CAMERA_IPC_SHARED_H
#define SERIAL_CAMERA_IPC_SHARED_H

#include <stdint.h>

#define SERIAL_CAMERA_IPC_MAGIC       (0x43414D53U) /* CAMS */
#define SERIAL_CAMERA_IPC_ADDRESS     (0x221D2A00UL)
#define SERIAL_CAMERA_IPC_CHUNK_BYTES (1024U)
#define SERIAL_CAMERA_REPLY_DEBUG_BYTES (16U)
#define SERIAL_CAMERA_RECORD_BASENAME_BYTES (32U)
#define SERIAL_CAMERA_WRITER_FILENAME_BYTES (48U)

typedef enum e_serial_camera_status
{
    SERIAL_CAMERA_STATUS_EMPTY = 0,
    SERIAL_CAMERA_STATUS_INIT,
    SERIAL_CAMERA_STATUS_UART_ERROR,
    SERIAL_CAMERA_STATUS_PROBING,
    SERIAL_CAMERA_STATUS_READY,
    SERIAL_CAMERA_STATUS_CAPTURING,
    SERIAL_CAMERA_STATUS_CAMERA_ERROR,
    SERIAL_CAMERA_STATUS_RECOVERING
} serial_camera_status_t;

typedef enum e_serial_camera_recovery_reason
{
    SERIAL_CAMERA_RECOVERY_NONE = 0,
    SERIAL_CAMERA_RECOVERY_PROBE_FAILURE,
    SERIAL_CAMERA_RECOVERY_CAPTURE_FAILURE,
    SERIAL_CAMERA_RECOVERY_WATCHDOG,
    SERIAL_CAMERA_RECOVERY_UART_FAILURE
} serial_camera_recovery_reason_t;

typedef enum e_serial_camera_command
{
    SERIAL_CAMERA_COMMAND_NONE = 0,
    SERIAL_CAMERA_COMMAND_RECORD_START,
    SERIAL_CAMERA_COMMAND_RECORD_STOP,
    SERIAL_CAMERA_COMMAND_CAPTURE_ONCE
} serial_camera_command_t;

typedef enum e_serial_camera_capture_stage
{
    SERIAL_CAMERA_CAPTURE_STAGE_IDLE = 0,
    SERIAL_CAMERA_CAPTURE_STAGE_STOP_FRAME,
    SERIAL_CAMERA_CAPTURE_STAGE_GET_SIZE,
    SERIAL_CAMERA_CAPTURE_STAGE_READ_CHUNK,
    SERIAL_CAMERA_CAPTURE_STAGE_PUBLISH_CHUNK,
    SERIAL_CAMERA_CAPTURE_STAGE_RESUME_FRAME,
    SERIAL_CAMERA_CAPTURE_STAGE_COUNT
} serial_camera_capture_stage_t;

typedef struct st_serial_camera_response_debug
{
    uint32_t command_code;
    uint32_t expected_bytes;
    uint32_t received_bytes;
    uint32_t fsp_err;
    uint8_t reply[SERIAL_CAMERA_REPLY_DEBUG_BYTES];
} serial_camera_response_debug_t;

typedef enum e_serial_camera_writer_status
{
    SERIAL_CAMERA_WRITER_IDLE = 0,
    SERIAL_CAMERA_WRITER_NO_SD,
    SERIAL_CAMERA_WRITER_WAIT_FRAME,
    SERIAL_CAMERA_WRITER_MKDIR,
    SERIAL_CAMERA_WRITER_OPEN_ERROR,
    SERIAL_CAMERA_WRITER_OPEN,
    SERIAL_CAMERA_WRITER_WRITE,
    SERIAL_CAMERA_WRITER_WRITE_ERROR,
    SERIAL_CAMERA_WRITER_CLOSED,
    SERIAL_CAMERA_WRITER_CHECK_PATH,
    SERIAL_CAMERA_WRITER_OPENING,
    SERIAL_CAMERA_WRITER_CLOSING,
    SERIAL_CAMERA_WRITER_RENAMING,
    SERIAL_CAMERA_WRITER_CLEANUP
} serial_camera_writer_status_t;

typedef struct st_serial_camera_ipc_mailbox
{
    volatile uint32_t magic;
    volatile uint32_t sequence;
    volatile uint32_t command;
    volatile uint32_t command_sequence;
    volatile uint32_t command_ack_sequence;
    volatile uint32_t status;
    volatile uint32_t last_fsp_err;
    volatile uint32_t last_event;
    volatile uint32_t uart_callback_count;
    volatile uint32_t uart_error_count;
    volatile uint32_t timeout_count;
    volatile uint32_t protocol_error_count;
    volatile uint32_t probe_count;
    volatile uint32_t probe_ok_count;
    volatile uint32_t consecutive_failures;
    volatile uint32_t recovery_count;
    volatile uint32_t soft_reset_count;
    volatile uint32_t uart_reopen_count;
    volatile uint32_t recovery_error_count;
    volatile uint32_t last_recovery_reason;
    volatile uint32_t driver_build;
    volatile uint32_t capture_active;
    volatile uint32_t capture_once;
    volatile uint32_t capture_interval_ms;
    volatile uint32_t capture_count;
    volatile uint32_t capture_error_count;
    volatile uint32_t capture_stage;
    volatile uint32_t last_failed_stage;
    volatile uint32_t last_command_code;
    volatile uint32_t rx_expected_bytes;
    volatile uint32_t rx_received_bytes;
    volatile uint32_t frame_sequence;
    volatile uint32_t image_size;
    volatile uint32_t image_offset;
    volatile uint32_t chunk_sequence;
    volatile uint32_t chunk_bytes;
    volatile uint32_t chunk_ack_sequence;
    volatile uint32_t jpeg_soi_ok;
    volatile uint32_t jpeg_eoi_ok;
    volatile uint32_t writer_poll_count;
    volatile uint32_t writer_status;
    volatile uint32_t writer_error;
    volatile uint32_t writer_open_count;
    volatile uint32_t writer_write_count;
    volatile uint32_t writer_saved_count;
    volatile uint32_t writer_bytes;
    volatile uint32_t writer_frame_sequence;
    volatile uint32_t writer_chunk_sequence;
    char version[16];
    uint8_t last_reply[SERIAL_CAMERA_REPLY_DEBUG_BYTES];
    serial_camera_response_debug_t stage_response[SERIAL_CAMERA_CAPTURE_STAGE_COUNT];
    char record_basename[SERIAL_CAMERA_RECORD_BASENAME_BYTES];
    char writer_filename[SERIAL_CAMERA_WRITER_FILENAME_BYTES];
    uint8_t chunk[SERIAL_CAMERA_IPC_CHUNK_BYTES];
    /*
     * Keep new diagnostics after chunk so every existing field and the CPU1
     * writer's chunk offset remain binary compatible.  The camera mailbox must
     * stay below the audio mailbox at 0x221D3000.
     */
    volatile uint32_t power_cycle_count;
    volatile uint32_t power_relay_ready;
    volatile uint32_t capture_consecutive_failures;
    volatile uint32_t recoveries_without_frame;
} serial_camera_ipc_mailbox_t;

typedef char serial_camera_ipc_mailbox_must_not_overlap_audio
    [(sizeof(serial_camera_ipc_mailbox_t) <=
      (0x221D3000UL - SERIAL_CAMERA_IPC_ADDRESS)) ? 1 : -1];

#define SERIAL_CAMERA_IPC_MAILBOX ((volatile serial_camera_ipc_mailbox_t *) SERIAL_CAMERA_IPC_ADDRESS)

#endif /* SERIAL_CAMERA_IPC_SHARED_H */

#ifndef AUDIO_IPC_SHARED_H
#define AUDIO_IPC_SHARED_H

#include <stddef.h>
#include <stdint.h>

#define AUDIO_IPC_MAGIC            (0x41554432U) /* AUD2 */
#define AUDIO_IPC_ADDRESS          (0x221D3000UL)
#define AUDIO_IPC_NAME_LENGTH      (32U)
#define AUDIO_IPC_TRACK_MAX        (8U)
#define AUDIO_IPC_STORY_NAME_LENGTH (16U)
#define AUDIO_IPC_STORY_CHUNK_BYTES (448U)
#define AUDIO_IPC_STORY_MAX_BYTES   (4U * 1024U * 1024U)

#define AUDIO_IPC_BOOT_FLAG_STATE_VALID      (1UL << 0)
#define AUDIO_IPC_BOOT_FLAG_COLD_START       (1UL << 1)
#define AUDIO_IPC_BOOT_FLAG_WARM_START       (1UL << 2)
#define AUDIO_IPC_BOOT_FLAG_AUDIO_SUPPRESSED (1UL << 3)
#define AUDIO_IPC_BOOT_FLAG_SOFTWARE_RESET   (1UL << 4)

typedef enum e_audio_ipc_status
{
    AUDIO_IPC_STATUS_IDLE = 0,
    AUDIO_IPC_STATUS_WAIT_SD,
    AUDIO_IPC_STATUS_OPEN_ERROR,
    AUDIO_IPC_STATUS_BAD_WAV,
    AUDIO_IPC_STATUS_I2S_ERROR,
    AUDIO_IPC_STATUS_PLAYING,
    AUDIO_IPC_STATUS_DONE,
    AUDIO_IPC_STATUS_PAUSED,
    AUDIO_IPC_STATUS_STOPPED
} audio_ipc_status_t;

typedef enum e_audio_ipc_command
{
    AUDIO_IPC_COMMAND_NONE = 0,
    AUDIO_IPC_COMMAND_PLAY_BOOT,
    AUDIO_IPC_COMMAND_PLAY_TRACK,
    AUDIO_IPC_COMMAND_PAUSE,
    AUDIO_IPC_COMMAND_RESUME,
    AUDIO_IPC_COMMAND_STOP,
    AUDIO_IPC_COMMAND_NEXT,
    AUDIO_IPC_COMMAND_PREV,
    AUDIO_IPC_COMMAND_VOLUME,
    AUDIO_IPC_COMMAND_REFRESH,
    AUDIO_IPC_COMMAND_PLAY_STORY
} audio_ipc_command_t;

typedef enum e_audio_ipc_story_operation
{
    AUDIO_IPC_STORY_OPERATION_NONE = 0,
    AUDIO_IPC_STORY_OPERATION_BEGIN,
    AUDIO_IPC_STORY_OPERATION_CHUNK,
    AUDIO_IPC_STORY_OPERATION_END,
    AUDIO_IPC_STORY_OPERATION_ABORT
} audio_ipc_story_operation_t;

typedef enum e_audio_ipc_story_status
{
    AUDIO_IPC_STORY_STATUS_IDLE = 0,
    AUDIO_IPC_STORY_STATUS_RECEIVING,
    AUDIO_IPC_STORY_STATUS_READY,
    AUDIO_IPC_STORY_STATUS_PLAYING,
    AUDIO_IPC_STORY_STATUS_COMPLETE,
    AUDIO_IPC_STORY_STATUS_ERROR
} audio_ipc_story_status_t;

typedef enum e_audio_ipc_story_result
{
    AUDIO_IPC_STORY_RESULT_OK = 0,
    AUDIO_IPC_STORY_RESULT_BAD_REQUEST,
    AUDIO_IPC_STORY_RESULT_NO_SD,
    AUDIO_IPC_STORY_RESULT_OPEN_FAILED,
    AUDIO_IPC_STORY_RESULT_OUT_OF_ORDER,
    AUDIO_IPC_STORY_RESULT_CRC_MISMATCH,
    AUDIO_IPC_STORY_RESULT_WRITE_FAILED,
    AUDIO_IPC_STORY_RESULT_FINALIZE_FAILED,
    AUDIO_IPC_STORY_RESULT_BUSY
} audio_ipc_story_result_t;

typedef struct st_audio_ipc_track
{
    char name[AUDIO_IPC_NAME_LENGTH];
    volatile uint32_t duration_ms;
} audio_ipc_track_t;

typedef struct st_audio_ipc_mailbox
{
    uint32_t magic;
    volatile uint32_t status_sequence;
    volatile uint32_t command_sequence;
    volatile uint32_t command_ack_sequence;
    volatile uint32_t command;
    volatile uint32_t command_arg;
    volatile uint32_t status;
    volatile uint32_t track_index;
    volatile uint32_t track_count;
    volatile uint32_t volume_percent;
    volatile uint32_t elapsed_ms;
    volatile uint32_t duration_ms;
    volatile uint32_t sample_rate;
    volatile uint32_t data_bytes;
    volatile uint32_t bytes_read;
    volatile uint32_t last_fsp_err;
    volatile int32_t last_file_error;
    char current_name[AUDIO_IPC_NAME_LENGTH];
    audio_ipc_track_t tracks[AUDIO_IPC_TRACK_MAX];
    volatile uint32_t boot_flags;
    volatile uint32_t boot_capture_count;
    volatile uint32_t silent_boot_count;
    volatile uint32_t boot_cold_warm_flag;
    volatile uint32_t boot_rstsr0;
    volatile uint32_t boot_rstsr1;
    volatile uint32_t boot_rstsr3;
    volatile uint32_t story_operation_sequence;
    volatile uint32_t story_operation_ack_sequence;
    volatile uint32_t story_operation;
    volatile uint32_t story_status;
    volatile uint32_t story_transfer_id;
    volatile uint32_t story_total_bytes;
    volatile uint32_t story_expected_crc32;
    volatile uint32_t story_received_bytes;
    volatile uint32_t story_running_crc32;
    volatile uint32_t story_chunk_offset;
    volatile uint32_t story_chunk_length;
    volatile uint32_t story_chunk_crc32;
    volatile uint32_t story_result;
    volatile uint32_t story_error_count;
    volatile uint32_t story_last_activity_tick;
    volatile uint32_t story_play_after;
    volatile uint32_t story_ready_sequence;
    volatile uint32_t story_play_ack_sequence;
    char story_name[AUDIO_IPC_STORY_NAME_LENGTH];
    uint8_t story_chunk[AUDIO_IPC_STORY_CHUNK_BYTES];
} audio_ipc_mailbox_t;

#define AUDIO_IPC_MAILBOX ((volatile audio_ipc_mailbox_t *) AUDIO_IPC_ADDRESS)

typedef char audio_ipc_mailbox_fits_reserved_region[
    (sizeof(audio_ipc_mailbox_t) <= 0x400U) ? 1 : -1];
typedef char audio_ipc_boot_flags_offset_is_stable[
    (offsetof(audio_ipc_mailbox_t, boot_flags) == 0x184U) ? 1 : -1];

#endif /* AUDIO_IPC_SHARED_H */

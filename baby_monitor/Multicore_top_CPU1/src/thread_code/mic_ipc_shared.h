#ifndef MIC_IPC_SHARED_H
#define MIC_IPC_SHARED_H

#include <stdint.h>

#define MIC_IPC_MAGIC          (0x4D494331U) /* MIC1 */
#define MIC_IPC_ADDRESS        (0x221D3800UL)
#define MIC_IPC_SAMPLE_RATE    (16000U)
#define MIC_IPC_BITS_PER_WORD  (32U)
#define MIC_IPC_PCM_WORDS      (384U)
#define MIC_IPC_REGION_BYTES   (0x700U)
#define MIC_IPC_RECORD_BASENAME_BYTES (32U)
#define MIC_IPC_WRITER_FILENAME_BYTES (48U)

typedef enum e_mic_ipc_status
{
    MIC_IPC_STATUS_IDLE = 0,
    MIC_IPC_STATUS_OPEN_ERROR,
    MIC_IPC_STATUS_RUNNING,
    MIC_IPC_STATUS_RECORDING,
    MIC_IPC_STATUS_STOPPED,
    MIC_IPC_STATUS_I2S_ERROR
} mic_ipc_status_t;

typedef enum e_mic_ipc_command
{
    MIC_IPC_COMMAND_NONE = 0,
    MIC_IPC_COMMAND_START,
    MIC_IPC_COMMAND_STOP,
    MIC_IPC_COMMAND_RECORD_START,
    MIC_IPC_COMMAND_RECORD_STOP
} mic_ipc_command_t;

typedef enum e_mic_ipc_writer_status
{
    MIC_IPC_WRITER_IDLE = 0,
    MIC_IPC_WRITER_NO_SD,
    MIC_IPC_WRITER_BAD_MAGIC,
    MIC_IPC_WRITER_WAIT_RECORD,
    MIC_IPC_WRITER_MKDIR,
    MIC_IPC_WRITER_OPEN_ERROR,
    MIC_IPC_WRITER_OPEN,
    MIC_IPC_WRITER_WRITE,
    MIC_IPC_WRITER_WRITE_ERROR,
    MIC_IPC_WRITER_CLOSED
} mic_ipc_writer_status_t;

typedef enum e_mic_ipc_record_slot
{
    MIC_IPC_RECORD_SLOT_EVEN = 0,
    MIC_IPC_RECORD_SLOT_ODD,
    MIC_IPC_RECORD_SLOT_AUTO
} mic_ipc_record_slot_t;

typedef struct st_mic_ipc_mailbox
{
    volatile uint32_t magic;
    volatile uint32_t sequence;
    volatile uint32_t command;
    volatile uint32_t command_sequence;
    volatile uint32_t command_ack_sequence;
    volatile uint32_t status;
    volatile uint32_t sample_rate;
    volatile uint32_t bits_per_word;
    volatile uint32_t buffer_samples;
    volatile uint32_t callback_count;
    volatile uint32_t queue_count;
    volatile uint32_t read_count;
    volatile uint32_t error_count;
    volatile uint32_t timeout_count;
    volatile uint32_t idle_count;
    volatile uint32_t last_event;
    volatile uint32_t last_fsp_err;
    volatile int32_t peak;
    volatile int32_t mean_abs;
    volatile int32_t last_sample;
    volatile uint32_t nonzero_count;
    volatile uint32_t record_active;
    volatile uint32_t record_sequence;
    volatile uint32_t record_bytes;
    volatile uint32_t record_seconds;
    volatile uint32_t chunk_sequence;
    volatile uint32_t chunk_bytes;
    volatile uint32_t writer_poll_count;
    volatile uint32_t writer_status;
    volatile uint32_t writer_error;
    volatile uint32_t writer_open_count;
    volatile uint32_t writer_write_count;
    volatile uint32_t writer_bytes;
    volatile uint32_t writer_record_sequence;
    volatile uint32_t writer_chunk_sequence;
    volatile uint32_t record_slot_select;
    volatile uint32_t record_sample_shift;
    volatile uint32_t record_slew_limit;
    volatile uint32_t record_gain_percent;
    volatile uint32_t writer_active_slot;
    volatile uint32_t writer_even_peak;
    volatile uint32_t writer_odd_peak;
    char record_basename[MIC_IPC_RECORD_BASENAME_BYTES];
    char writer_filename[MIC_IPC_WRITER_FILENAME_BYTES];
    volatile int32_t pcm[MIC_IPC_PCM_WORDS];
    volatile uint32_t writer_gap_count;
    volatile uint32_t writer_declick_count;
} mic_ipc_mailbox_t;

_Static_assert(sizeof(mic_ipc_mailbox_t) <= MIC_IPC_REGION_BYTES,
               "MIC IPC mailbox overlaps the SD IPC region");

#define MIC_IPC_MAILBOX ((volatile mic_ipc_mailbox_t *) MIC_IPC_ADDRESS)

#endif /* MIC_IPC_SHARED_H */

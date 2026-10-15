#include "thread_code/audio_wav_player.h"

#include <stdio.h>
#include <string.h>

#include "ff_stdio.h"
#include "common_data.h"
#include "sd_audio_player_thread.h"
#include "thread_code/audio_ipc_shared.h"
#include "thread_code/sd_card_driver.h"
#include "thread_code/silent_reset_shared.h"
#include "thread_code/system_logger.h"

#define AUDIO_WAV_PLAYER_MAGIC           (0x41554431U) /* AUD1 */
#define AUDIO_WAV_PLAYER_MUSIC_DIR       SD_CARD_MOUNT_PATH "/MUSIC"
#define AUDIO_WAV_PLAYER_STORY_DIR       SD_CARD_MOUNT_PATH "/STORY"
#define AUDIO_WAV_PLAYER_DEFAULT_PATH    AUDIO_WAV_PLAYER_MUSIC_DIR "/BOOT.WAV"
#define AUDIO_WAV_PLAYER_TRACK_PREFIX    AUDIO_WAV_PLAYER_MUSIC_DIR "/TRACK"
#define AUDIO_WAV_PLAYER_STORY_TEMP_EXT  ".TMP"
#define AUDIO_WAV_PLAYER_TRACK_COUNT_MAX (999U)
#define AUDIO_WAV_PLAYER_READ_BYTES      (16384U)
#define AUDIO_WAV_PLAYER_WRITE_BYTES     (32768U)
#define AUDIO_WAV_PLAYER_WAIT_TICKS      pdMS_TO_TICKS(2000U)
#define AUDIO_WAV_PLAYER_SD_WAIT_TICKS   pdMS_TO_TICKS(250U)
#define AUDIO_WAV_PLAYER_TEST_TONE       (0U)
#define AUDIO_TEST_TONE_WAIT_SD          (0U)
#define AUDIO_TEST_TONE_WAIT_LOGGER      (0U)
#define AUDIO_TEST_PIN_SELF_TEST         (0U)
#define AUDIO_TEST_TONE_POLL_FIFO        (1U)
#define AUDIO_TEST_TONE_RATE_HZ          (16000U)
#define AUDIO_TEST_TONE_FREQ_HZ          (1000U)
#define AUDIO_TEST_TONE_AMPLITUDE        (12000)
#define AUDIO_TEST_TONE_WRITE_BYTES      (AUDIO_WAV_PLAYER_WRITE_BYTES)

typedef struct st_wav_info
{
    uint32_t sample_rate;
    uint32_t data_offset;
    uint32_t data_bytes;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t audio_format;
} wav_info_t;

BSP_DONT_REMOVE volatile audio_wav_player_debug_t g_audio_wav_player_debug;

#if !AUDIO_WAV_PLAYER_TEST_TONE
static uint8_t g_audio_read_buffer[AUDIO_WAV_PLAYER_READ_BYTES] BSP_ALIGN_VARIABLE(4);
#endif
static uint8_t g_audio_write_buffers[2U][AUDIO_WAV_PLAYER_WRITE_BYTES] BSP_ALIGN_VARIABLE(4);
#define g_audio_write_buffer (g_audio_write_buffers[0])
static volatile bool g_audio_i2s_tx_empty;
static volatile bool g_audio_i2s_idle;
static uint32_t g_audio_volume_percent = 100U;
static uint32_t g_audio_current_track_index;
static uint32_t g_audio_pending_command;
static uint32_t g_audio_pending_arg;
static uint32_t g_audio_boot_flags;
static uint32_t g_audio_boot_capture_count;
static uint32_t g_audio_silent_boot_count;
static uint32_t g_audio_boot_cold_warm_flag;
static uint32_t g_audio_boot_rstsr0;
static uint32_t g_audio_boot_rstsr1;
static uint32_t g_audio_boot_rstsr3;
static FF_FILE * gp_audio_story_file;
static uint32_t g_audio_story_transfer_id;
static uint32_t g_audio_story_expected_offset;
static uint32_t g_audio_story_running_crc32 = 0xFFFFFFFFU;
static uint32_t g_audio_story_last_chunk_offset;
static uint32_t g_audio_story_last_chunk_length;
static uint32_t g_audio_story_last_chunk_crc32;
static uint32_t g_audio_story_last_operation_sequence;
static char g_audio_story_final_path[AUDIO_WAV_PLAYER_PATH_LENGTH];
static char g_audio_story_temp_path[AUDIO_WAV_PLAYER_PATH_LENGTH];

#define AUDIO_STORY_RECEIVE_TIMEOUT_MS (30000U)

#if !AUDIO_WAV_PLAYER_TEST_TONE
static bool audio_wav_parse_header(FF_FILE * p_file, wav_info_t * p_info);
static bool audio_wav_read_exact(FF_FILE * p_file, uint8_t * p_dest, uint32_t bytes);
static uint32_t audio_wav_prepare_i2s_data(uint8_t const * p_src, uint32_t src_bytes, uint8_t * p_dest, wav_info_t const * p_info);
static uint32_t audio_wav_read_prepare_chunk(FF_FILE * p_file, wav_info_t const * p_info, uint32_t * p_bytes_left, uint8_t * p_dest);
static bool audio_wav_play_file(char const * p_path);
static void audio_wav_scan_tracks(void);
static bool audio_wav_track_path(uint32_t track_index, char * p_path, uint32_t path_size);
static bool audio_wav_handle_runtime_command(void);
static bool audio_wav_story_autoplay_pending(void);
static uint32_t audio_wav_take_command(uint32_t * p_arg);
static uint32_t audio_wav_duration_ms(wav_info_t const * p_info);
static bool audio_wav_is_boot_name(char const * p_name);
static bool audio_wav_is_wav_name(char const * p_name);
static bool audio_wav_name_equals(char const * p_left, char const * p_right);
static char audio_wav_upper(char value);
static bool audio_story_name_valid(char const * p_name);
static uint32_t audio_story_crc32_update(uint32_t crc, uint8_t const * p_data, uint32_t length);
static void audio_story_close(bool remove_partial);
static void audio_story_ack(volatile audio_ipc_mailbox_t * p_ipc,
                            uint32_t operation_sequence,
                            audio_ipc_story_result_t result);
#endif
#if AUDIO_WAV_PLAYER_TEST_TONE
static void audio_wav_player_test_tone_thread(void);
static void audio_wav_fill_test_tone(uint8_t * p_dest, uint32_t bytes);
#endif
static bool audio_wav_start_audio_clock(uint32_t sample_rate);
static void audio_wav_stop_audio_clock(void);
static bool audio_wav_wait_for_tx_ready(void);
#if AUDIO_WAV_PLAYER_TEST_TONE
static bool audio_wav_wait_for_stopped(void);
static void audio_wav_pin_self_test(void);
static void audio_wav_poll_fifo_test_tone(void);
#endif
static void audio_wav_capture_registers(void);
#if !AUDIO_WAV_PLAYER_TEST_TONE
static uint16_t audio_wav_u16(uint8_t const * p_data);
static uint32_t audio_wav_u32(uint8_t const * p_data);
static int16_t audio_wav_s16(uint8_t const * p_data);
static void audio_wav_store_s16(uint8_t * p_data, int16_t value);
#endif
static void audio_wav_debug_set_path(char const * p_path);
static void audio_wav_boot_state_init(void);
static void audio_wav_ipc_init(void);
static void audio_wav_ipc_publish(void);
static void audio_wav_ipc_set_name(char const * p_path);
#if !AUDIO_WAV_PLAYER_TEST_TONE
static FF_FILE * audio_fs_open(char const * p_path, char const * p_mode);
static int audio_fs_close(FF_FILE * p_file);
static int audio_fs_seek(FF_FILE * p_file, long offset, int origin);
static size_t audio_fs_read(void * p_buffer, size_t size, size_t count, FF_FILE * p_file);
static int audio_fs_find_first(char const * p_directory, FF_FindData_t * p_find_data);
static int audio_fs_find_next(FF_FindData_t * p_find_data);
#endif

#if !AUDIO_WAV_PLAYER_TEST_TONE
static FF_FILE * audio_fs_open(char const * p_path, char const * p_mode)
{
    FF_FILE * p_file;

    if (!sd_card_driver_fs_lock(portMAX_DELAY))
    {
        return NULL;
    }
    p_file = ff_fopen(p_path, p_mode);
    sd_card_driver_fs_unlock();
    return p_file;
}

static int audio_fs_close(FF_FILE * p_file)
{
    int result;

    if (!sd_card_driver_fs_lock(portMAX_DELAY))
    {
        return -1;
    }
    result = ff_fclose(p_file);
    sd_card_driver_fs_unlock();
    return result;
}

static int audio_fs_seek(FF_FILE * p_file, long offset, int origin)
{
    int result;

    if (!sd_card_driver_fs_lock(portMAX_DELAY))
    {
        return -1;
    }
    result = ff_fseek(p_file, offset, origin);
    sd_card_driver_fs_unlock();
    return result;
}

static size_t audio_fs_read(void * p_buffer, size_t size, size_t count, FF_FILE * p_file)
{
    size_t result;

    if (!sd_card_driver_fs_lock(portMAX_DELAY))
    {
        return 0U;
    }
    result = ff_fread(p_buffer, size, count, p_file);
    sd_card_driver_fs_unlock();
    return result;
}

static int audio_fs_find_first(char const * p_directory, FF_FindData_t * p_find_data)
{
    int result;

    if (!sd_card_driver_fs_lock(portMAX_DELAY))
    {
        return -1;
    }
    result = ff_findfirst(p_directory, p_find_data);
    sd_card_driver_fs_unlock();
    return result;
}

static int audio_fs_find_next(FF_FindData_t * p_find_data)
{
    int result;

    if (!sd_card_driver_fs_lock(portMAX_DELAY))
    {
        return -1;
    }
    result = ff_findnext(p_find_data);
    sd_card_driver_fs_unlock();
    return result;
}
#endif

#if !AUDIO_WAV_PLAYER_TEST_TONE
static bool audio_story_name_valid(char const * p_name)
{
    if ((NULL == p_name) || ('A' != p_name[0]) || ('I' != p_name[1]))
    {
        return false;
    }
    for (uint32_t i = 2U; i < 8U; i++)
    {
        char value = p_name[i];
        if (!(((value >= '0') && (value <= '9')) || ((value >= 'A') && (value <= 'F'))))
        {
            return false;
        }
    }
    return ('.' == p_name[8]) && ('W' == p_name[9]) && ('A' == p_name[10]) &&
           ('V' == p_name[11]) && ('\0' == p_name[12]);
}

static uint32_t audio_story_crc32_update(uint32_t crc, uint8_t const * p_data, uint32_t length)
{
    for (uint32_t i = 0U; i < length; i++)
    {
        crc ^= p_data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            crc = (0U != (crc & 1U)) ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
        }
    }
    return crc;
}

#define AUDIO_STORY_WRITE_BUFFER_BYTES (65536U)

static uint8_t  g_audio_story_write_buffer[AUDIO_STORY_WRITE_BUFFER_BYTES];
static uint32_t g_audio_story_write_buffer_length;

static bool audio_story_flush_write_buffer(void)
{
    if (0U == g_audio_story_write_buffer_length)
    {
        return true;
    }
    if ((NULL == gp_audio_story_file) ||
        (g_audio_story_write_buffer_length !=
         ff_fwrite(g_audio_story_write_buffer, 1U, g_audio_story_write_buffer_length,
                   gp_audio_story_file)))
    {
        sd_card_driver_report_io_error(stdioGET_ERRNO());
        return false;
    }
    g_audio_story_write_buffer_length = 0U;
    return true;
}

static void audio_story_close(bool remove_partial)
{
    if (NULL != gp_audio_story_file)
    {
        (void) ff_fclose(gp_audio_story_file);
        gp_audio_story_file = NULL;
    }
    if (remove_partial && ('\0' != g_audio_story_temp_path[0]))
    {
        (void) ff_remove(g_audio_story_temp_path);
    }
    if (remove_partial)
    {
        g_audio_story_temp_path[0] = '\0';
    }
    g_audio_story_write_buffer_length = 0U;
}

static void audio_story_ack(volatile audio_ipc_mailbox_t * p_ipc,
                            uint32_t operation_sequence,
                            audio_ipc_story_result_t result)
{
    p_ipc->story_result = (uint32_t) result;
    p_ipc->story_received_bytes = g_audio_story_expected_offset;
    p_ipc->story_running_crc32 = g_audio_story_running_crc32 ^ 0xFFFFFFFFU;
    p_ipc->story_last_activity_tick = (uint32_t) xTaskGetTickCount();
    if (AUDIO_IPC_STORY_RESULT_OK != result)
    {
        p_ipc->story_status = AUDIO_IPC_STORY_STATUS_ERROR;
        p_ipc->story_error_count++;
    }
    __DMB();
    p_ipc->story_operation_ack_sequence = operation_sequence;
}

void audio_wav_story_writer_poll(bool sd_ready)
{
    volatile audio_ipc_mailbox_t * p_ipc = AUDIO_IPC_MAILBOX;
    uint32_t operation_sequence;
    uint32_t operation;
    audio_ipc_story_result_t result = AUDIO_IPC_STORY_RESULT_OK;

    if (AUDIO_IPC_MAGIC != p_ipc->magic)
    {
        return;
    }
    if ((NULL != gp_audio_story_file) &&
        (AUDIO_IPC_STORY_STATUS_RECEIVING == p_ipc->story_status) &&
        (0U != p_ipc->story_last_activity_tick) &&
        ((xTaskGetTickCount() - (TickType_t) p_ipc->story_last_activity_tick) >
         pdMS_TO_TICKS(AUDIO_STORY_RECEIVE_TIMEOUT_MS)))
    {
        audio_story_close(true);
        g_audio_story_expected_offset = 0U;
        p_ipc->story_result = AUDIO_IPC_STORY_RESULT_OUT_OF_ORDER;
        p_ipc->story_status = AUDIO_IPC_STORY_STATUS_ERROR;
        p_ipc->story_error_count++;
    }
    operation_sequence = p_ipc->story_operation_sequence;
    if ((0U == operation_sequence) || (operation_sequence == g_audio_story_last_operation_sequence))
    {
        return;
    }
    __DMB();
    operation = p_ipc->story_operation;

    if (!sd_ready)
    {
        audio_story_close(true);
        g_audio_story_expected_offset = 0U;
        result = AUDIO_IPC_STORY_RESULT_NO_SD;
    }
    else if (AUDIO_IPC_STORY_OPERATION_BEGIN == operation)
    {
        char name[AUDIO_IPC_STORY_NAME_LENGTH];
        for (uint32_t i = 0U; i < sizeof(name); i++)
        {
            name[i] = p_ipc->story_name[i];
        }
        name[sizeof(name) - 1U] = '\0';

        if ((0U == p_ipc->story_transfer_id) ||
            (0U == p_ipc->story_total_bytes) ||
            (p_ipc->story_total_bytes > AUDIO_IPC_STORY_MAX_BYTES) ||
            !audio_story_name_valid(name))
        {
            result = AUDIO_IPC_STORY_RESULT_BAD_REQUEST;
        }
        else if ((NULL != gp_audio_story_file) &&
                 (g_audio_story_transfer_id == p_ipc->story_transfer_id))
        {
            result = AUDIO_IPC_STORY_RESULT_OK;
        }
        else
        {
            audio_story_close(true);
            (void) ff_mkdir(AUDIO_WAV_PLAYER_STORY_DIR);
            (void) snprintf(g_audio_story_final_path, sizeof(g_audio_story_final_path),
                            "%s/%s", AUDIO_WAV_PLAYER_STORY_DIR, name);
            (void) snprintf(g_audio_story_temp_path, sizeof(g_audio_story_temp_path),
                            "%s/AI%.6s%s", AUDIO_WAV_PLAYER_STORY_DIR, &name[2],
                            AUDIO_WAV_PLAYER_STORY_TEMP_EXT);
            gp_audio_story_file = ff_fopen(g_audio_story_temp_path, "wb");
            if (NULL == gp_audio_story_file)
            {
                result = AUDIO_IPC_STORY_RESULT_OPEN_FAILED;
                sd_card_driver_report_io_error(stdioGET_ERRNO());
            }
            else
            {
                g_audio_story_transfer_id = p_ipc->story_transfer_id;
                g_audio_story_expected_offset = 0U;
                g_audio_story_running_crc32 = 0xFFFFFFFFU;
                g_audio_story_last_chunk_offset = UINT32_MAX;
                g_audio_story_last_chunk_length = 0U;
                g_audio_story_last_chunk_crc32 = 0U;
                g_audio_story_write_buffer_length = 0U;
                p_ipc->story_status = AUDIO_IPC_STORY_STATUS_RECEIVING;
            }
        }
    }
    else if (AUDIO_IPC_STORY_OPERATION_CHUNK == operation)
    {
        uint32_t length = p_ipc->story_chunk_length;
        uint32_t offset = p_ipc->story_chunk_offset;
        uint32_t chunk_crc;

        if ((NULL == gp_audio_story_file) ||
            (g_audio_story_transfer_id != p_ipc->story_transfer_id))
        {
            result = AUDIO_IPC_STORY_RESULT_OUT_OF_ORDER;
        }
        else if ((0U == length) || (length > AUDIO_IPC_STORY_CHUNK_BYTES) ||
                 (offset > p_ipc->story_total_bytes) ||
                 (length > (p_ipc->story_total_bytes - offset)))
        {
            result = AUDIO_IPC_STORY_RESULT_BAD_REQUEST;
        }
        else
        {
            chunk_crc = audio_story_crc32_update(
                0xFFFFFFFFU, (uint8_t const *) p_ipc->story_chunk, length) ^ 0xFFFFFFFFU;
            if (chunk_crc != p_ipc->story_chunk_crc32)
            {
                result = AUDIO_IPC_STORY_RESULT_CRC_MISMATCH;
            }
            else if ((offset == g_audio_story_last_chunk_offset) &&
                     (length == g_audio_story_last_chunk_length) &&
                     (chunk_crc == g_audio_story_last_chunk_crc32) &&
                     ((offset + length) == g_audio_story_expected_offset))
            {
                result = AUDIO_IPC_STORY_RESULT_OK;
            }
            else if (offset != g_audio_story_expected_offset)
            {
                result = AUDIO_IPC_STORY_RESULT_OUT_OF_ORDER;
            }
            else if (((g_audio_story_write_buffer_length + length) >
                      AUDIO_STORY_WRITE_BUFFER_BYTES) &&
                     !audio_story_flush_write_buffer())
            {
                result = AUDIO_IPC_STORY_RESULT_WRITE_FAILED;
            }
            else
            {
                for (uint32_t i = 0U; i < length; i++)
                {
                    g_audio_story_write_buffer[g_audio_story_write_buffer_length + i] =
                        p_ipc->story_chunk[i];
                }
                g_audio_story_write_buffer_length += length;
                g_audio_story_running_crc32 = audio_story_crc32_update(
                    g_audio_story_running_crc32, (uint8_t const *) p_ipc->story_chunk, length);
                g_audio_story_last_chunk_offset = offset;
                g_audio_story_last_chunk_length = length;
                g_audio_story_last_chunk_crc32 = chunk_crc;
                g_audio_story_expected_offset += length;
            }
        }
    }
    else if (AUDIO_IPC_STORY_OPERATION_END == operation)
    {
        uint32_t final_crc = g_audio_story_running_crc32 ^ 0xFFFFFFFFU;
        if ((NULL == gp_audio_story_file) &&
            (g_audio_story_transfer_id == p_ipc->story_transfer_id) &&
            (g_audio_story_expected_offset == p_ipc->story_total_bytes) &&
            (final_crc == p_ipc->story_expected_crc32) &&
            (0U != p_ipc->story_ready_sequence))
        {
            result = AUDIO_IPC_STORY_RESULT_OK;
        }
        else if ((NULL == gp_audio_story_file) ||
                 (g_audio_story_transfer_id != p_ipc->story_transfer_id) ||
                 (g_audio_story_expected_offset != p_ipc->story_total_bytes))
        {
            result = AUDIO_IPC_STORY_RESULT_OUT_OF_ORDER;
        }
        else if ((final_crc != p_ipc->story_expected_crc32) ||
                 (final_crc != p_ipc->story_chunk_crc32))
        {
            result = AUDIO_IPC_STORY_RESULT_CRC_MISMATCH;
        }
        else
        {
            if (!audio_story_flush_write_buffer())
            {
                result = AUDIO_IPC_STORY_RESULT_WRITE_FAILED;
            }
            else if (0 != ff_fclose(gp_audio_story_file))
            {
                gp_audio_story_file = NULL;
                result = AUDIO_IPC_STORY_RESULT_FINALIZE_FAILED;
                sd_card_driver_report_io_error(stdioGET_ERRNO());
            }
            else
            {
                gp_audio_story_file = NULL;
                (void) ff_remove(g_audio_story_final_path);
                if (0 != ff_rename(g_audio_story_temp_path, g_audio_story_final_path, false))
                {
                    result = AUDIO_IPC_STORY_RESULT_FINALIZE_FAILED;
                    sd_card_driver_report_io_error(stdioGET_ERRNO());
                }
                else
                {
                    g_audio_story_temp_path[0] = '\0';
                    p_ipc->story_status = AUDIO_IPC_STORY_STATUS_READY;
                    p_ipc->story_ready_sequence++;
                }
            }
        }
    }
    else if (AUDIO_IPC_STORY_OPERATION_ABORT == operation)
    {
        audio_story_close(true);
        g_audio_story_expected_offset = 0U;
        p_ipc->story_status = AUDIO_IPC_STORY_STATUS_IDLE;
    }
    else
    {
        result = AUDIO_IPC_STORY_RESULT_BAD_REQUEST;
    }

    if ((AUDIO_IPC_STORY_RESULT_OK != result) &&
        (AUDIO_IPC_STORY_OPERATION_BEGIN != operation))
    {
        audio_story_close(true);
    }
    g_audio_story_last_operation_sequence = operation_sequence;
    audio_story_ack(p_ipc, operation_sequence, result);
}

bool audio_wav_story_writer_active(void)
{
    volatile audio_ipc_mailbox_t const * p_ipc = AUDIO_IPC_MAILBOX;

    if (AUDIO_IPC_MAGIC != p_ipc->magic)
    {
        return false;
    }

    return (AUDIO_IPC_STORY_STATUS_RECEIVING == p_ipc->story_status) ||
           (p_ipc->story_operation_sequence != p_ipc->story_operation_ack_sequence);
}

uint32_t audio_wav_story_bulk_begin(bool sd_ready, uint32_t transfer_id,
                                    uint32_t total_bytes, uint32_t expected_crc32,
                                    char const * p_name, bool play_after,
                                    uint32_t * p_next_offset)
{
    volatile audio_ipc_mailbox_t * p_ipc = AUDIO_IPC_MAILBOX;
    audio_ipc_story_result_t result = AUDIO_IPC_STORY_RESULT_OK;

    if (NULL != p_next_offset)
    {
        *p_next_offset = g_audio_story_expected_offset;
    }
    if (!sd_ready)
    {
        return AUDIO_IPC_STORY_RESULT_NO_SD;
    }
    if ((0U == transfer_id) || (0U == total_bytes) ||
        (total_bytes > AUDIO_IPC_STORY_MAX_BYTES) || !audio_story_name_valid(p_name))
    {
        return AUDIO_IPC_STORY_RESULT_BAD_REQUEST;
    }
    if ((NULL != gp_audio_story_file) && (g_audio_story_transfer_id == transfer_id))
    {
        if (NULL != p_next_offset)
        {
            *p_next_offset = g_audio_story_expected_offset;
        }
        return AUDIO_IPC_STORY_RESULT_OK;
    }

    audio_story_close(true);
    (void) ff_mkdir(AUDIO_WAV_PLAYER_STORY_DIR);
    (void) snprintf(g_audio_story_final_path, sizeof(g_audio_story_final_path),
                    "%s/%s", AUDIO_WAV_PLAYER_STORY_DIR, p_name);
    (void) snprintf(g_audio_story_temp_path, sizeof(g_audio_story_temp_path),
                    "%s/AI%.6s%s", AUDIO_WAV_PLAYER_STORY_DIR, &p_name[2],
                    AUDIO_WAV_PLAYER_STORY_TEMP_EXT);
    gp_audio_story_file = ff_fopen(g_audio_story_temp_path, "wb");
    if (NULL == gp_audio_story_file)
    {
        sd_card_driver_report_io_error(stdioGET_ERRNO());
        return AUDIO_IPC_STORY_RESULT_OPEN_FAILED;
    }

    g_audio_story_transfer_id = transfer_id;
    g_audio_story_expected_offset = 0U;
    g_audio_story_running_crc32 = 0xFFFFFFFFU;
    g_audio_story_last_chunk_offset = UINT32_MAX;
    g_audio_story_last_chunk_length = 0U;
    g_audio_story_last_chunk_crc32 = 0U;
    g_audio_story_write_buffer_length = 0U;
    p_ipc->story_transfer_id = transfer_id;
    p_ipc->story_total_bytes = total_bytes;
    p_ipc->story_expected_crc32 = expected_crc32;
    p_ipc->story_received_bytes = 0U;
    p_ipc->story_running_crc32 = 0U;
    p_ipc->story_play_after = play_after ? 1U : 0U;
    for (uint32_t i = 0U; i < AUDIO_IPC_STORY_NAME_LENGTH; i++)
    {
        p_ipc->story_name[i] = p_name[i];
        if ('\0' == p_name[i])
        {
            break;
        }
    }
    p_ipc->story_status = AUDIO_IPC_STORY_STATUS_RECEIVING;
    p_ipc->story_result = result;
    p_ipc->story_last_activity_tick = (uint32_t) xTaskGetTickCount();
    __DMB();
    if (NULL != p_next_offset)
    {
        *p_next_offset = 0U;
    }
    return result;
}

uint32_t audio_wav_story_bulk_chunk(uint32_t transfer_id, uint32_t total_bytes,
                                    uint32_t offset, uint8_t const * p_data,
                                    uint32_t length, uint32_t chunk_crc32,
                                    uint32_t * p_next_offset)
{
    volatile audio_ipc_mailbox_t * p_ipc = AUDIO_IPC_MAILBOX;
    uint32_t calculated_crc;

    if (NULL != p_next_offset)
    {
        *p_next_offset = g_audio_story_expected_offset;
    }
    if ((NULL == gp_audio_story_file) || (g_audio_story_transfer_id != transfer_id))
    {
        return AUDIO_IPC_STORY_RESULT_OUT_OF_ORDER;
    }
    if ((NULL == p_data) || (0U == length) || (length > 4096U) ||
        (offset > total_bytes) || (length > (total_bytes - offset)) ||
        (total_bytes != p_ipc->story_total_bytes))
    {
        return AUDIO_IPC_STORY_RESULT_BAD_REQUEST;
    }
    calculated_crc = audio_story_crc32_update(0xFFFFFFFFU, p_data, length) ^ 0xFFFFFFFFU;
    if (calculated_crc != chunk_crc32)
    {
        return AUDIO_IPC_STORY_RESULT_CRC_MISMATCH;
    }
    /* A cumulative ACK may be lost. Any fully committed older chunk is an
     * idempotent retransmission and can be acknowledged without rewriting it. */
    if ((offset < g_audio_story_expected_offset) &&
        ((offset + length) <= g_audio_story_expected_offset))
    {
        return AUDIO_IPC_STORY_RESULT_OK;
    }
    if (offset != g_audio_story_expected_offset)
    {
        return AUDIO_IPC_STORY_RESULT_OUT_OF_ORDER;
    }
    if (((g_audio_story_write_buffer_length + length) > AUDIO_STORY_WRITE_BUFFER_BYTES) &&
        !audio_story_flush_write_buffer())
    {
        return AUDIO_IPC_STORY_RESULT_WRITE_FAILED;
    }
    memcpy(&g_audio_story_write_buffer[g_audio_story_write_buffer_length], p_data, length);
    g_audio_story_write_buffer_length += length;
    g_audio_story_running_crc32 = audio_story_crc32_update(g_audio_story_running_crc32,
                                                           p_data, length);
    g_audio_story_last_chunk_offset = offset;
    g_audio_story_last_chunk_length = length;
    g_audio_story_last_chunk_crc32 = chunk_crc32;
    g_audio_story_expected_offset += length;
    p_ipc->story_received_bytes = g_audio_story_expected_offset;
    p_ipc->story_running_crc32 = g_audio_story_running_crc32 ^ 0xFFFFFFFFU;
    p_ipc->story_last_activity_tick = (uint32_t) xTaskGetTickCount();
    if (NULL != p_next_offset)
    {
        *p_next_offset = g_audio_story_expected_offset;
    }
    return AUDIO_IPC_STORY_RESULT_OK;
}

uint32_t audio_wav_story_bulk_end(uint32_t transfer_id, uint32_t total_bytes,
                                  uint32_t expected_crc32, uint32_t * p_next_offset)
{
    volatile audio_ipc_mailbox_t * p_ipc = AUDIO_IPC_MAILBOX;
    uint32_t final_crc = g_audio_story_running_crc32 ^ 0xFFFFFFFFU;

    if (NULL != p_next_offset)
    {
        *p_next_offset = g_audio_story_expected_offset;
    }
    if ((NULL == gp_audio_story_file) || (g_audio_story_transfer_id != transfer_id) ||
        (g_audio_story_expected_offset != total_bytes))
    {
        return AUDIO_IPC_STORY_RESULT_OUT_OF_ORDER;
    }
    if ((final_crc != expected_crc32) || (expected_crc32 != p_ipc->story_expected_crc32))
    {
        audio_story_close(true);
        p_ipc->story_status = AUDIO_IPC_STORY_STATUS_ERROR;
        return AUDIO_IPC_STORY_RESULT_CRC_MISMATCH;
    }
    if (!audio_story_flush_write_buffer())
    {
        audio_story_close(true);
        p_ipc->story_status = AUDIO_IPC_STORY_STATUS_ERROR;
        return AUDIO_IPC_STORY_RESULT_WRITE_FAILED;
    }
    if (0 != ff_fclose(gp_audio_story_file))
    {
        gp_audio_story_file = NULL;
        sd_card_driver_report_io_error(stdioGET_ERRNO());
        p_ipc->story_status = AUDIO_IPC_STORY_STATUS_ERROR;
        return AUDIO_IPC_STORY_RESULT_FINALIZE_FAILED;
    }
    gp_audio_story_file = NULL;
    (void) ff_remove(g_audio_story_final_path);
    if (0 != ff_rename(g_audio_story_temp_path, g_audio_story_final_path, false))
    {
        sd_card_driver_report_io_error(stdioGET_ERRNO());
        p_ipc->story_status = AUDIO_IPC_STORY_STATUS_ERROR;
        return AUDIO_IPC_STORY_RESULT_FINALIZE_FAILED;
    }
    g_audio_story_temp_path[0] = '\0';
    p_ipc->story_result = AUDIO_IPC_STORY_RESULT_OK;
    p_ipc->story_status = AUDIO_IPC_STORY_STATUS_READY;
    p_ipc->story_ready_sequence++;
    p_ipc->story_last_activity_tick = (uint32_t) xTaskGetTickCount();
    __DMB();
    return AUDIO_IPC_STORY_RESULT_OK;
}

void audio_wav_story_bulk_abort(uint32_t transfer_id)
{
    volatile audio_ipc_mailbox_t * p_ipc = AUDIO_IPC_MAILBOX;

    if ((0U == transfer_id) || (transfer_id == g_audio_story_transfer_id))
    {
        audio_story_close(true);
        g_audio_story_expected_offset = 0U;
        p_ipc->story_status = AUDIO_IPC_STORY_STATUS_IDLE;
    }
}
#else
void audio_wav_story_writer_poll(bool sd_ready)
{
    FSP_PARAMETER_NOT_USED(sd_ready);
}

bool audio_wav_story_writer_active(void)
{
    return false;
}

uint32_t audio_wav_story_bulk_begin(bool sd_ready, uint32_t transfer_id,
                                    uint32_t total_bytes, uint32_t expected_crc32,
                                    char const * p_name, bool play_after,
                                    uint32_t * p_next_offset)
{
    FSP_PARAMETER_NOT_USED(sd_ready); FSP_PARAMETER_NOT_USED(transfer_id);
    FSP_PARAMETER_NOT_USED(total_bytes); FSP_PARAMETER_NOT_USED(expected_crc32);
    FSP_PARAMETER_NOT_USED(p_name); FSP_PARAMETER_NOT_USED(play_after);
    if (NULL != p_next_offset) *p_next_offset = 0U;
    return 1U;
}

uint32_t audio_wav_story_bulk_chunk(uint32_t transfer_id, uint32_t total_bytes,
                                    uint32_t offset, uint8_t const * p_data,
                                    uint32_t length, uint32_t chunk_crc32,
                                    uint32_t * p_next_offset)
{
    FSP_PARAMETER_NOT_USED(transfer_id); FSP_PARAMETER_NOT_USED(total_bytes);
    FSP_PARAMETER_NOT_USED(offset); FSP_PARAMETER_NOT_USED(p_data);
    FSP_PARAMETER_NOT_USED(length); FSP_PARAMETER_NOT_USED(chunk_crc32);
    if (NULL != p_next_offset) *p_next_offset = 0U;
    return 1U;
}

uint32_t audio_wav_story_bulk_end(uint32_t transfer_id, uint32_t total_bytes,
                                  uint32_t expected_crc32, uint32_t * p_next_offset)
{
    FSP_PARAMETER_NOT_USED(transfer_id); FSP_PARAMETER_NOT_USED(total_bytes);
    FSP_PARAMETER_NOT_USED(expected_crc32);
    if (NULL != p_next_offset) *p_next_offset = 0U;
    return 1U;
}

void audio_wav_story_bulk_abort(uint32_t transfer_id)
{
    FSP_PARAMETER_NOT_USED(transfer_id);
}
#endif

void audio_i2s_callback(i2s_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    if (I2S_EVENT_TX_EMPTY == p_args->event)
    {
        g_audio_i2s_tx_empty = true;
        g_audio_wav_player_debug.tx_empty_count++;
        audio_wav_capture_registers();
    }
    else if (I2S_EVENT_IDLE == p_args->event)
    {
        g_audio_i2s_idle = true;
        g_audio_wav_player_debug.idle_count++;
        audio_wav_capture_registers();
    }
    else
    {
        /* RX is not used for MAX98357 playback. */
    }
}

void audio_wav_player_thread(void)
{
#if AUDIO_WAV_PLAYER_TEST_TONE
    audio_wav_player_test_tone_thread();
#else
    memset((void *) &g_audio_wav_player_debug, 0, sizeof(g_audio_wav_player_debug));
    g_audio_wav_player_debug.magic = AUDIO_WAV_PLAYER_MAGIC;
    g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_WAIT_SD;
    audio_wav_debug_set_path(AUDIO_WAV_PLAYER_DEFAULT_PATH);
    audio_wav_boot_state_init();
    audio_wav_ipc_init();

    while (!sd_card_driver_is_ready())
    {
        audio_wav_ipc_publish();
        vTaskDelay(AUDIO_WAV_PLAYER_SD_WAIT_TICKS);
    }

    audio_wav_scan_tracks();
    if (0U == (g_audio_boot_flags & AUDIO_IPC_BOOT_FLAG_AUDIO_SUPPRESSED))
    {
        uint32_t saved_volume = g_audio_volume_percent;
        g_audio_volume_percent = saved_volume / 2U;
        (void) audio_wav_play_file(AUDIO_WAV_PLAYER_DEFAULT_PATH);
        g_audio_volume_percent = saved_volume;
        audio_wav_ipc_publish();
    }
    else
    {
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_IDLE;
        audio_wav_ipc_publish();
    }

    while (1)
    {
        uint32_t arg = 0U;
        uint32_t command = g_audio_pending_command;

        if (AUDIO_IPC_COMMAND_NONE != command)
        {
            arg = g_audio_pending_arg;
            g_audio_pending_command = AUDIO_IPC_COMMAND_NONE;
        }
        else
        {
            command = audio_wav_take_command(&arg);
        }

        if (audio_wav_story_autoplay_pending())
        {
            char story_path[AUDIO_WAV_PLAYER_PATH_LENGTH];
            char story_name[AUDIO_IPC_STORY_NAME_LENGTH];
            uint32_t ready_sequence = AUDIO_IPC_MAILBOX->story_ready_sequence;
            bool story_completed;

            __DMB();
            for (uint32_t i = 0U; i < sizeof(story_name); i++)
            {
                story_name[i] = AUDIO_IPC_MAILBOX->story_name[i];
            }
            story_name[sizeof(story_name) - 1U] = '\0';
            AUDIO_IPC_MAILBOX->story_play_ack_sequence = ready_sequence;
            AUDIO_IPC_MAILBOX->story_status = AUDIO_IPC_STORY_STATUS_PLAYING;
            (void) snprintf(story_path, sizeof(story_path), "%s/%s",
                            AUDIO_WAV_PLAYER_STORY_DIR, story_name);
            story_completed = audio_wav_play_file(story_path);
            if ((AUDIO_IPC_MAILBOX->story_ready_sequence != ready_sequence) &&
                (AUDIO_IPC_STORY_STATUS_READY == AUDIO_IPC_MAILBOX->story_status))
            {
                /* A newer story became ready while this one was playing or
                 * paused. Preserve its READY state so the outer loop can
                 * autoplay it instead of reporting the interrupted story as
                 * an error. */
                audio_wav_ipc_publish();
                continue;
            }
            if (story_completed)
            {
                AUDIO_IPC_MAILBOX->story_status = AUDIO_IPC_STORY_STATUS_COMPLETE;
            }
            else
            {
                AUDIO_IPC_MAILBOX->story_status = AUDIO_IPC_STORY_STATUS_ERROR;
                AUDIO_IPC_MAILBOX->story_error_count++;
            }
            audio_wav_ipc_publish();
            continue;
        }

        if (AUDIO_IPC_COMMAND_NONE == command)
        {
            g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_IDLE;
            audio_wav_ipc_publish();
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }

        if (AUDIO_IPC_COMMAND_REFRESH == command)
        {
            audio_wav_scan_tracks();
        }
        else if (AUDIO_IPC_COMMAND_VOLUME == command)
        {
            g_audio_volume_percent = (arg > 100U) ? 100U : arg;
        }
        else if (AUDIO_IPC_COMMAND_PLAY_BOOT == command)
        {
            g_audio_current_track_index = 0U;
            (void) audio_wav_play_file(AUDIO_WAV_PLAYER_DEFAULT_PATH);
        }
        else if (AUDIO_IPC_COMMAND_PLAY_TRACK == command)
        {
            char path[AUDIO_WAV_PLAYER_PATH_LENGTH];
            if (audio_wav_track_path(arg, path, sizeof(path)))
            {
                g_audio_current_track_index = arg;
                (void) audio_wav_play_file(path);
            }
        }
        else if (AUDIO_IPC_COMMAND_NEXT == command)
        {
            char path[AUDIO_WAV_PLAYER_PATH_LENGTH];
            uint32_t count = AUDIO_IPC_MAILBOX->track_count;
            uint32_t next = (0U == count) ? 0U : g_audio_current_track_index + 1U;
            if ((0U == next) || (next > count))
            {
                next = 1U;
            }
            if (audio_wav_track_path(next, path, sizeof(path)))
            {
                g_audio_current_track_index = next;
                (void) audio_wav_play_file(path);
            }
        }
        else if (AUDIO_IPC_COMMAND_PREV == command)
        {
            char path[AUDIO_WAV_PLAYER_PATH_LENGTH];
            uint32_t count = AUDIO_IPC_MAILBOX->track_count;
            uint32_t prev = (g_audio_current_track_index <= 1U) ? count : (g_audio_current_track_index - 1U);
            if (audio_wav_track_path(prev, path, sizeof(path)))
            {
                g_audio_current_track_index = prev;
                (void) audio_wav_play_file(path);
            }
        }
        else if ((AUDIO_IPC_COMMAND_STOP == command) || (AUDIO_IPC_COMMAND_PAUSE == command) ||
                 (AUDIO_IPC_COMMAND_RESUME == command))
        {
            g_audio_wav_player_debug.status = (AUDIO_IPC_COMMAND_STOP == command) ?
                                              AUDIO_WAV_PLAYER_STATUS_IDLE :
                                              AUDIO_WAV_PLAYER_STATUS_DONE;
        }

        audio_wav_ipc_publish();
    }
#endif
}

bool audio_wav_player_is_done(void)
{
    return AUDIO_WAV_PLAYER_STATUS_DONE == g_audio_wav_player_debug.status;
}

#if !AUDIO_WAV_PLAYER_TEST_TONE
static bool audio_wav_play_file(char const * p_path)
{
    FF_FILE * p_file = NULL;
    wav_info_t wav_info;
    uint32_t bytes_left;
    bool opened_i2s = false;

    memset((void *) &g_audio_wav_player_debug, 0, sizeof(g_audio_wav_player_debug));
    g_audio_wav_player_debug.magic = AUDIO_WAV_PLAYER_MAGIC;
    g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_WAIT_SD;
    audio_wav_debug_set_path(p_path);
    AUDIO_IPC_MAILBOX->status = (uint32_t) AUDIO_IPC_STATUS_WAIT_SD;
    audio_wav_ipc_publish();

    p_file = audio_fs_open(p_path, "r");
    if (NULL == p_file)
    {
        g_audio_wav_player_debug.last_file_error = stdioGET_ERRNO();
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_OPEN_ERROR;
        audio_wav_ipc_publish();
        return false;
    }

    if (!audio_wav_parse_header(p_file, &wav_info))
    {
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_BAD_WAV;
        (void) audio_fs_close(p_file);
        audio_wav_ipc_publish();
        return false;
    }

    g_audio_wav_player_debug.sample_rate = wav_info.sample_rate;
    g_audio_wav_player_debug.channels = wav_info.channels;
    g_audio_wav_player_debug.bits_per_sample = wav_info.bits_per_sample;
    g_audio_wav_player_debug.data_bytes = wav_info.data_bytes;

    if ((1U != wav_info.audio_format) ||
        (16U != wav_info.bits_per_sample) ||
        ((1U != wav_info.channels) && (2U != wav_info.channels)))
    {
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_BAD_WAV;
        (void) audio_fs_close(p_file);
        audio_wav_ipc_publish();
        return false;
    }

    if (0 != audio_fs_seek(p_file, (long) wav_info.data_offset, FF_SEEK_SET))
    {
        g_audio_wav_player_debug.last_file_error = stdioGET_ERRNO();
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_BAD_WAV;
        (void) audio_fs_close(p_file);
        audio_wav_ipc_publish();
        return false;
    }

    if (!audio_wav_start_audio_clock(wav_info.sample_rate))
    {
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
        (void) audio_fs_close(p_file);
        audio_wav_ipc_publish();
        return false;
    }

    g_audio_wav_player_debug.last_fsp_err = g_audio_i2s.p_api->open(g_audio_i2s.p_ctrl, g_audio_i2s.p_cfg);
    audio_wav_capture_registers();
    if (FSP_SUCCESS != g_audio_wav_player_debug.last_fsp_err)
    {
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
        audio_wav_stop_audio_clock();
        (void) audio_fs_close(p_file);
        audio_wav_ipc_publish();
        return false;
    }

    opened_i2s = true;
    bytes_left = wav_info.data_bytes;
    g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_PLAYING;
    audio_wav_ipc_publish();

    {
        uint32_t active_buffer = 0U;
        uint32_t next_buffer = 1U;
        uint32_t active_write_bytes = audio_wav_read_prepare_chunk(p_file,
                                                                   &wav_info,
                                                                   &bytes_left,
                                                                   g_audio_write_buffers[active_buffer]);
        uint32_t next_write_bytes = 0U;

        while (0U != active_write_bytes)
        {
            if (!sd_card_driver_is_ready())
            {
                g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_WAIT_SD;
                break;
            }

            if (audio_wav_handle_runtime_command())
            {
                break;
            }

            g_audio_i2s_tx_empty = false;
            g_audio_i2s_idle = false;
            g_audio_wav_player_debug.last_fsp_err = g_audio_i2s.p_api->write(g_audio_i2s.p_ctrl,
                                                                              g_audio_write_buffers[active_buffer],
                                                                              active_write_bytes);
            audio_wav_capture_registers();
            if (FSP_SUCCESS != g_audio_wav_player_debug.last_fsp_err)
            {
                g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
                break;
            }

            g_audio_wav_player_debug.bytes_written += active_write_bytes;
            next_write_bytes = audio_wav_read_prepare_chunk(p_file, &wav_info, &bytes_left, g_audio_write_buffers[next_buffer]);

            if (!audio_wav_wait_for_tx_ready())
            {
                g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
                break;
            }

            audio_wav_ipc_publish();
            active_buffer ^= 1U;
            next_buffer ^= 1U;
            active_write_bytes = next_write_bytes;
        }
    }

    if (opened_i2s)
    {
        g_audio_i2s_idle = false;
        (void) g_audio_i2s.p_api->stop(g_audio_i2s.p_ctrl);
        (void) audio_wav_wait_for_tx_ready();
        (void) g_audio_i2s.p_api->close(g_audio_i2s.p_ctrl);
        audio_wav_stop_audio_clock();
    }

    (void) audio_fs_close(p_file);

    if (AUDIO_WAV_PLAYER_STATUS_PLAYING == g_audio_wav_player_debug.status)
    {
        g_audio_wav_player_debug.play_count++;
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_DONE;
    }

    audio_wav_ipc_publish();

    return AUDIO_WAV_PLAYER_STATUS_DONE == g_audio_wav_player_debug.status;
}

static void audio_wav_scan_tracks(void)
{
    FF_FindData_t find_data;
    volatile audio_ipc_mailbox_t * p_ipc = AUDIO_IPC_MAILBOX;
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < AUDIO_IPC_TRACK_MAX; i++)
    {
        p_ipc->tracks[i].name[0] = '\0';
        p_ipc->tracks[i].duration_ms = 0U;
    }

    if (0 != audio_fs_find_first(AUDIO_WAV_PLAYER_MUSIC_DIR, &find_data))
    {
        p_ipc->track_count = 0U;
        audio_wav_ipc_publish();
        return;
    }

    do
    {
        if ((0U == (find_data.ucAttributes & FF_FA_DIREC)) &&
            audio_wav_is_wav_name(find_data.pcFileName) &&
            !audio_wav_is_boot_name(find_data.pcFileName) &&
            !audio_story_name_valid(find_data.pcFileName))
        {
            char path[AUDIO_WAV_PLAYER_PATH_LENGTH];
            char name[AUDIO_IPC_NAME_LENGTH];
            FF_FILE * p_file;
            wav_info_t info;
            uint32_t i;

            for (i = 0U; i < (AUDIO_IPC_NAME_LENGTH - 1U); i++)
            {
                name[i] = find_data.pcFileName[i];
                p_ipc->tracks[count].name[i] = name[i];
                if ('\0' == find_data.pcFileName[i])
                {
                    break;
                }
            }
            name[AUDIO_IPC_NAME_LENGTH - 1U] = '\0';
            p_ipc->tracks[count].name[AUDIO_IPC_NAME_LENGTH - 1U] = '\0';

            path[0] = '\0';
            (void) snprintf(path, sizeof(path), "%s/%s", AUDIO_WAV_PLAYER_MUSIC_DIR, name);
            p_file = audio_fs_open(path, "r");
            if (NULL != p_file)
            {
                if (audio_wav_parse_header(p_file, &info))
                {
                    p_ipc->tracks[count].duration_ms = audio_wav_duration_ms(&info);
                }
                (void) audio_fs_close(p_file);
            }

            count++;
            if (count >= AUDIO_IPC_TRACK_MAX)
            {
                break;
            }
        }
    } while (0 == audio_fs_find_next(&find_data));

    p_ipc->track_count = count;
    audio_wav_ipc_publish();
}

static bool audio_wav_track_path(uint32_t track_index, char * p_path, uint32_t path_size)
{
    volatile audio_ipc_mailbox_t const * p_ipc = AUDIO_IPC_MAILBOX;
    char name[AUDIO_IPC_NAME_LENGTH];

    if ((0U == track_index) || (track_index > p_ipc->track_count) || (track_index > AUDIO_IPC_TRACK_MAX))
    {
        return false;
    }

    for (uint32_t i = 0U; i < AUDIO_IPC_NAME_LENGTH; i++)
    {
        name[i] = p_ipc->tracks[track_index - 1U].name[i];
    }

    (void) snprintf(p_path, path_size, "%s/%s", AUDIO_WAV_PLAYER_MUSIC_DIR, name);
    return true;
}

static bool audio_wav_handle_runtime_command(void)
{
    uint32_t arg = 0U;
    uint32_t command;

    /* Story bulk transfer completion is an audio event in its own right. It
     * must preempt both active playback and the paused wait loop; otherwise a
     * paused player only notices the story after an unrelated command (for
     * example a volume change) happens to release that loop. */
    if (audio_wav_story_autoplay_pending())
    {
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_IDLE;
        AUDIO_IPC_MAILBOX->status = (uint32_t) AUDIO_IPC_STATUS_IDLE;
        audio_wav_ipc_publish();
        return true;
    }

    command = audio_wav_take_command(&arg);

    if (AUDIO_IPC_COMMAND_NONE == command)
    {
        return false;
    }

    if (AUDIO_IPC_COMMAND_VOLUME == command)
    {
        g_audio_volume_percent = (arg > 100U) ? 100U : arg;
        audio_wav_ipc_publish();
        return false;
    }

    if (AUDIO_IPC_COMMAND_PAUSE == command)
    {
        (void) g_audio_i2s.p_api->stop(g_audio_i2s.p_ctrl);
        (void) audio_wav_wait_for_tx_ready();
        (void) g_audio_i2s.p_api->close(g_audio_i2s.p_ctrl);
        audio_wav_stop_audio_clock();
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_IDLE;
        AUDIO_IPC_MAILBOX->status = (uint32_t) AUDIO_IPC_STATUS_PAUSED;
        audio_wav_ipc_publish();
        while (1)
        {
            if (audio_wav_story_autoplay_pending())
            {
                g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_IDLE;
                AUDIO_IPC_MAILBOX->status = (uint32_t) AUDIO_IPC_STATUS_IDLE;
                audio_wav_ipc_publish();
                return true;
            }

            command = audio_wav_take_command(&arg);
            if ((AUDIO_IPC_COMMAND_RESUME == command) || (AUDIO_IPC_COMMAND_NONE == command))
            {
                if (AUDIO_IPC_COMMAND_RESUME == command)
                {
                    if (!audio_wav_start_audio_clock(g_audio_wav_player_debug.sample_rate))
                    {
                        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
                        return true;
                    }

                    g_audio_wav_player_debug.last_fsp_err = g_audio_i2s.p_api->open(g_audio_i2s.p_ctrl,
                                                                                      g_audio_i2s.p_cfg);
                    if (FSP_SUCCESS != g_audio_wav_player_debug.last_fsp_err)
                    {
                        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
                        audio_wav_stop_audio_clock();
                        return true;
                    }

                    g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_PLAYING;
                    AUDIO_IPC_MAILBOX->status = (uint32_t) AUDIO_IPC_STATUS_PLAYING;
                    audio_wav_ipc_publish();
                    return false;
                }
            }
            else
            {
                g_audio_pending_command = command;
                g_audio_pending_arg = arg;
                g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_IDLE;
                if (AUDIO_IPC_COMMAND_STOP == command)
                {
                    AUDIO_IPC_MAILBOX->status = (uint32_t) AUDIO_IPC_STATUS_STOPPED;
                }
                else
                {
                    AUDIO_IPC_MAILBOX->status = (uint32_t) AUDIO_IPC_STATUS_IDLE;
                }
                return true;
            }

            vTaskDelay(pdMS_TO_TICKS(50U));
        }
    }

    if (AUDIO_IPC_COMMAND_STOP == command)
    {
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_IDLE;
        AUDIO_IPC_MAILBOX->status = (uint32_t) AUDIO_IPC_STATUS_STOPPED;
        return true;
    }

    if ((AUDIO_IPC_COMMAND_PLAY_BOOT == command) || (AUDIO_IPC_COMMAND_PLAY_TRACK == command) ||
        (AUDIO_IPC_COMMAND_NEXT == command) || (AUDIO_IPC_COMMAND_PREV == command) ||
        (AUDIO_IPC_COMMAND_REFRESH == command))
    {
        g_audio_pending_command = command;
        g_audio_pending_arg = arg;
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_IDLE;
        return true;
    }

    return false;
}

static bool audio_wav_story_autoplay_pending(void)
{
    volatile audio_ipc_mailbox_t const * p_ipc = AUDIO_IPC_MAILBOX;

    __DMB();
    return (p_ipc->story_ready_sequence != p_ipc->story_play_ack_sequence) &&
           (AUDIO_IPC_STORY_STATUS_READY == p_ipc->story_status) &&
           (0U != p_ipc->story_play_after);
}

static uint32_t audio_wav_take_command(uint32_t * p_arg)
{
    volatile audio_ipc_mailbox_t * p_ipc = AUDIO_IPC_MAILBOX;
    uint32_t sequence = p_ipc->command_sequence;
    uint32_t command;

    if ((AUDIO_IPC_MAGIC != p_ipc->magic) || (sequence == p_ipc->command_ack_sequence))
    {
        return AUDIO_IPC_COMMAND_NONE;
    }

    command = p_ipc->command;
    *p_arg = p_ipc->command_arg;
    p_ipc->command_ack_sequence = sequence;
    return command;
}

static uint32_t audio_wav_duration_ms(wav_info_t const * p_info)
{
    uint32_t bytes_per_second = p_info->sample_rate * p_info->channels * (p_info->bits_per_sample / 8U);

    if (0U == bytes_per_second)
    {
        return 0U;
    }

    return (uint32_t) ((((uint64_t) p_info->data_bytes) * 1000ULL) / bytes_per_second);
}

static bool audio_wav_is_boot_name(char const * p_name)
{
    return audio_wav_name_equals(p_name, "BOOT.WAV");
}

static bool audio_wav_is_wav_name(char const * p_name)
{
    size_t length = strlen(p_name);

    if (length < 5U)
    {
        return false;
    }

    if (audio_wav_name_equals(&p_name[length - 4U], ".WAV"))
    {
        return true;
    }

    return (length >= 6U) && audio_wav_name_equals(&p_name[length - 5U], ".WAVE");
}

static bool audio_wav_name_equals(char const * p_left, char const * p_right)
{
    while (('\0' != *p_left) || ('\0' != *p_right))
    {
        if (audio_wav_upper(*p_left) != audio_wav_upper(*p_right))
        {
            return false;
        }
        p_left++;
        p_right++;
    }

    return true;
}

static char audio_wav_upper(char value)
{
    if ((value >= 'a') && (value <= 'z'))
    {
        return (char) (value - ('a' - 'A'));
    }

    return value;
}

static bool audio_wav_parse_header(FF_FILE * p_file, wav_info_t * p_info)
{
    uint8_t header[12];
    bool found_fmt = false;
    bool found_data = false;

    memset(p_info, 0, sizeof(*p_info));

    if (!audio_wav_read_exact(p_file, header, sizeof(header)))
    {
        return false;
    }

    if ((0 != memcmp(&header[0], "RIFF", 4U)) || (0 != memcmp(&header[8], "WAVE", 4U)))
    {
        return false;
    }

    while ((!found_fmt || !found_data) && !ff_feof(p_file))
    {
        uint8_t chunk_header[8];
        uint32_t chunk_size;
        long payload_pos;

        if (!audio_wav_read_exact(p_file, chunk_header, sizeof(chunk_header)))
        {
            return false;
        }

        chunk_size = audio_wav_u32(&chunk_header[4]);
        payload_pos = ff_ftell(p_file);
        if (payload_pos < 0)
        {
            return false;
        }

        if (0 == memcmp(&chunk_header[0], "fmt ", 4U))
        {
            uint8_t fmt[16];

            if (chunk_size < sizeof(fmt))
            {
                return false;
            }

            if (!audio_wav_read_exact(p_file, fmt, sizeof(fmt)))
            {
                return false;
            }

            p_info->audio_format = audio_wav_u16(&fmt[0]);
            p_info->channels = audio_wav_u16(&fmt[2]);
            p_info->sample_rate = audio_wav_u32(&fmt[4]);
            p_info->bits_per_sample = audio_wav_u16(&fmt[14]);
            found_fmt = true;
        }
        else if (0 == memcmp(&chunk_header[0], "data", 4U))
        {
            p_info->data_offset = (uint32_t) payload_pos;
            p_info->data_bytes = chunk_size;
            found_data = true;
        }
        else
        {
            /* Skip unknown chunks. */
        }

        if (0 != audio_fs_seek(p_file, payload_pos + (long) chunk_size + (long) (chunk_size & 1U), FF_SEEK_SET))
        {
            return false;
        }
    }

    return found_fmt && found_data;
}

static bool audio_wav_read_exact(FF_FILE * p_file, uint8_t * p_dest, uint32_t bytes)
{
    return bytes == (uint32_t) audio_fs_read(p_dest, 1U, bytes, p_file);
}

static uint32_t audio_wav_read_prepare_chunk(FF_FILE * p_file,
                                             wav_info_t const * p_info,
                                             uint32_t * p_bytes_left,
                                             uint8_t * p_dest)
{
    uint32_t request_bytes;
    uint32_t read_bytes;

    if ((NULL == p_file) || (NULL == p_info) || (NULL == p_bytes_left) || (NULL == p_dest) || (0U == *p_bytes_left))
    {
        return 0U;
    }

    request_bytes = (*p_bytes_left > AUDIO_WAV_PLAYER_READ_BYTES) ? AUDIO_WAV_PLAYER_READ_BYTES : *p_bytes_left;
    request_bytes &= ~1UL;
    if (0U == request_bytes)
    {
        return 0U;
    }

    read_bytes = (uint32_t) audio_fs_read(g_audio_read_buffer, 1U, request_bytes, p_file);
    read_bytes &= ~1UL;
    if (0U == read_bytes)
    {
        return 0U;
    }

    g_audio_wav_player_debug.bytes_read += read_bytes;
    *p_bytes_left -= read_bytes;
    return audio_wav_prepare_i2s_data(g_audio_read_buffer, read_bytes, p_dest, p_info);
}

static uint32_t audio_wav_prepare_i2s_data(uint8_t const * p_src, uint32_t src_bytes, uint8_t * p_dest, wav_info_t const * p_info)
{
    if (1U == p_info->channels)
    {
        uint32_t samples = src_bytes / 2U;

        for (uint32_t i = 0U; i < samples; i++)
        {
            int32_t scaled = ((int32_t) audio_wav_s16(&p_src[2U * i]) * (int32_t) g_audio_volume_percent) / 100;
            int16_t sample = (int16_t) scaled;

            audio_wav_store_s16(&p_dest[4U * i], sample);
            audio_wav_store_s16(&p_dest[(4U * i) + 2U], sample);
        }

        return samples * 4U;
    }

    for (uint32_t i = 0U; i < (src_bytes / 2U); i++)
    {
        int32_t scaled = ((int32_t) audio_wav_s16(&p_src[2U * i]) * (int32_t) g_audio_volume_percent) / 100;
        audio_wav_store_s16(&p_dest[2U * i], (int16_t) scaled);
    }

    return src_bytes;
}
#endif

#if AUDIO_WAV_PLAYER_TEST_TONE
static void audio_wav_player_test_tone_thread(void)
{
    audio_wav_fill_test_tone(g_audio_write_buffer, AUDIO_WAV_PLAYER_WRITE_BYTES);

    memset((void *) &g_audio_wav_player_debug, 0, sizeof(g_audio_wav_player_debug));
    g_audio_wav_player_debug.magic = AUDIO_WAV_PLAYER_MAGIC;
    g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_WAIT_SD;
    g_audio_wav_player_debug.sample_rate = AUDIO_TEST_TONE_RATE_HZ;
    g_audio_wav_player_debug.channels = 2U;
    g_audio_wav_player_debug.bits_per_sample = 16U;
    audio_wav_debug_set_path("TEST:1KHZ");

#if AUDIO_TEST_TONE_WAIT_SD
    while (!sd_card_driver_is_ready())
    {
        vTaskDelay(AUDIO_WAV_PLAYER_SD_WAIT_TICKS);
    }
#endif

#if AUDIO_TEST_TONE_WAIT_LOGGER
    while (!system_logger_is_ready())
    {
        vTaskDelay(AUDIO_WAV_PLAYER_SD_WAIT_TICKS);
    }

    vTaskDelay(pdMS_TO_TICKS(1000U));
#endif

#if AUDIO_TEST_PIN_SELF_TEST
    audio_wav_pin_self_test();
#endif

    while (1)
    {
        memset((void *) &g_audio_wav_player_debug, 0, sizeof(g_audio_wav_player_debug));
        g_audio_wav_player_debug.magic = AUDIO_WAV_PLAYER_MAGIC;
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_IDLE;
        g_audio_wav_player_debug.sample_rate = AUDIO_TEST_TONE_RATE_HZ;
        g_audio_wav_player_debug.channels = 2U;
        g_audio_wav_player_debug.bits_per_sample = 16U;
        g_audio_wav_player_debug.data_bytes = 0xFFFFFFFFU;
        audio_wav_debug_set_path("TEST:1KHZ");

        if (!audio_wav_start_audio_clock())
        {
            g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
            audio_wav_capture_registers();
            vTaskDelay(pdMS_TO_TICKS(1000U));
            continue;
        }

        g_audio_wav_player_debug.last_fsp_err = g_audio_i2s.p_api->open(g_audio_i2s.p_ctrl, g_audio_i2s.p_cfg);
        audio_wav_capture_registers();
        if (FSP_SUCCESS != g_audio_wav_player_debug.last_fsp_err)
        {
            g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
            audio_wav_stop_audio_clock();
            vTaskDelay(pdMS_TO_TICKS(1000U));
            continue;
        }

        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_PLAYING;

#if AUDIO_TEST_TONE_POLL_FIFO
        audio_wav_poll_fifo_test_tone();
#else
        while (AUDIO_WAV_PLAYER_STATUS_PLAYING == g_audio_wav_player_debug.status)
        {
            g_audio_i2s_tx_empty = false;
            g_audio_i2s_idle = false;
            g_audio_wav_player_debug.last_fsp_err =
                g_audio_i2s.p_api->write(g_audio_i2s.p_ctrl, g_audio_write_buffer, AUDIO_TEST_TONE_WRITE_BYTES);
            audio_wav_capture_registers();

            if (FSP_ERR_IN_USE == g_audio_wav_player_debug.last_fsp_err)
            {
                (void) audio_wav_wait_for_stopped();
                vTaskDelay(pdMS_TO_TICKS(1U));
                continue;
            }

            if (FSP_SUCCESS != g_audio_wav_player_debug.last_fsp_err)
            {
                g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
                break;
            }

            g_audio_wav_player_debug.bytes_written += AUDIO_TEST_TONE_WRITE_BYTES;
            if (!audio_wav_wait_for_stopped())
            {
                g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(1U));
        }
#endif

        g_audio_i2s_idle = false;
        (void) g_audio_i2s.p_api->stop(g_audio_i2s.p_ctrl);
        (void) audio_wav_wait_for_tx_ready();
        (void) g_audio_i2s.p_api->close(g_audio_i2s.p_ctrl);
        audio_wav_stop_audio_clock();
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void audio_wav_pin_self_test(void)
{
    static const bsp_io_port_pin_t test_pins[] =
    {
        BSP_IO_PORT_04_PIN_03,
        BSP_IO_PORT_04_PIN_04,
        BSP_IO_PORT_04_PIN_05
    };

    for (uint32_t i = 0U; i < (sizeof(test_pins) / sizeof(test_pins[0])); i++)
    {
        (void) g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                      test_pins[i],
                                      IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_DRIVE_HIGH);
    }

    for (uint32_t pulse = 0U; pulse < 500U; pulse++)
    {
        bsp_io_level_t level = (0U == (pulse & 1U)) ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW;

        (void) g_ioport.p_api->pinWrite(g_ioport.p_ctrl, BSP_IO_PORT_04_PIN_03, level);
        (void) g_ioport.p_api->pinWrite(g_ioport.p_ctrl, BSP_IO_PORT_04_PIN_04, level);
        (void) g_ioport.p_api->pinWrite(g_ioport.p_ctrl, BSP_IO_PORT_04_PIN_05, level);
        vTaskDelay(pdMS_TO_TICKS(5U));
    }

    (void) g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                  BSP_IO_PORT_04_PIN_05,
                                  IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_GPT1);

    if (audio_wav_start_audio_clock())
    {
        vTaskDelay(pdMS_TO_TICKS(5000U));
        audio_wav_stop_audio_clock();
    }
    else
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }

    (void) g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                  BSP_IO_PORT_04_PIN_03,
                                  IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_SSI);
    (void) g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                  BSP_IO_PORT_04_PIN_04,
                                  IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_SSI);
    (void) g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                  BSP_IO_PORT_04_PIN_06,
                                  IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_SSI);

    vTaskDelay(pdMS_TO_TICKS(100U));
}

static void audio_wav_poll_fifo_test_tone(void)
{
    uint32_t sample_index = 0U;
    uint32_t period_samples = AUDIO_TEST_TONE_RATE_HZ / AUDIO_TEST_TONE_FREQ_HZ;
    uint32_t half_period = period_samples / 2U;

    if (0U == half_period)
    {
        half_period = 1U;
    }

    g_audio_i2s_tx_empty = false;
    g_audio_i2s_idle = false;
    g_audio_wav_player_debug.last_fsp_err =
        g_audio_i2s.p_api->write(g_audio_i2s.p_ctrl, g_audio_write_buffer, 64U);
    audio_wav_capture_registers();

    if (FSP_SUCCESS != g_audio_wav_player_debug.last_fsp_err)
    {
        g_audio_wav_player_debug.status = AUDIO_WAV_PLAYER_STATUS_I2S_ERROR;
        return;
    }

    while (1)
    {
        uint32_t bytes_written = 0U;

        while (R_SSI0->SSIFSR_b.TDC < BSP_FEATURE_SSI_FIFO_NUM_STAGES)
        {
            int16_t sample = (((sample_index / half_period) & 1U) == 0U) ?
                             (int16_t) AUDIO_TEST_TONE_AMPLITUDE :
                             (int16_t) -AUDIO_TEST_TONE_AMPLITUDE;

            R_SSI0->SSIFTDR16 = (uint16_t) sample;
            sample_index++;
            bytes_written += 2U;
        }

        if (bytes_written > 0U)
        {
            R_SSI0->SSIFSR;
            R_SSI0->SSIFSR = 1U;
            g_audio_wav_player_debug.bytes_written += bytes_written;
            audio_wav_capture_registers();
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

static void audio_wav_fill_test_tone(uint8_t * p_dest, uint32_t bytes)
{
    uint32_t frames = bytes / 4U;
    uint32_t period_frames = AUDIO_TEST_TONE_RATE_HZ / AUDIO_TEST_TONE_FREQ_HZ;
    uint32_t half_period = period_frames / 2U;

    if (0U == half_period)
    {
        half_period = 1U;
    }

    for (uint32_t i = 0U; i < frames; i++)
    {
        int16_t sample = (((i / half_period) & 1U) == 0U) ?
                         (int16_t) AUDIO_TEST_TONE_AMPLITUDE :
                         (int16_t) -AUDIO_TEST_TONE_AMPLITUDE;
        uint16_t raw = (uint16_t) sample;

        p_dest[(4U * i) + 0U] = (uint8_t) (raw & 0xFFU);
        p_dest[(4U * i) + 1U] = (uint8_t) (raw >> 8);
        p_dest[(4U * i) + 2U] = (uint8_t) (raw & 0xFFU);
        p_dest[(4U * i) + 3U] = (uint8_t) (raw >> 8);
    }
}
#endif

static bool audio_wav_wait_for_tx_ready(void)
{
    TickType_t start_tick = xTaskGetTickCount();

    while (!g_audio_i2s_idle)
    {
        if ((xTaskGetTickCount() - start_tick) > AUDIO_WAV_PLAYER_WAIT_TICKS)
        {
            audio_wav_capture_registers();
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    audio_wav_capture_registers();
    return true;
}

#if AUDIO_WAV_PLAYER_TEST_TONE
static bool audio_wav_wait_for_stopped(void)
{
    TickType_t start_tick = xTaskGetTickCount();

    while (1)
    {
        i2s_status_t status;
        fsp_err_t err = g_audio_i2s.p_api->statusGet(g_audio_i2s.p_ctrl, &status);

        audio_wav_capture_registers();
        if (FSP_SUCCESS != err)
        {
            g_audio_wav_player_debug.last_fsp_err = err;
            return false;
        }

        if (I2S_STATE_STOPPED == status.state)
        {
            return true;
        }

        if ((xTaskGetTickCount() - start_tick) > AUDIO_WAV_PLAYER_WAIT_TICKS)
        {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}
#endif

static bool audio_wav_start_audio_clock(uint32_t sample_rate)
{
    fsp_err_t err = g_audio_clk_timer.p_api->open(g_audio_clk_timer.p_ctrl, g_audio_clk_timer.p_cfg);
    timer_info_t timer_info;
    uint64_t desired_clock_hz;
    uint32_t period_counts;
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        g_audio_wav_player_debug.last_fsp_err = err;
        return false;
    }

    err = g_audio_clk_timer.p_api->infoGet(g_audio_clk_timer.p_ctrl, &timer_info);
    if (FSP_SUCCESS != err)
    {
        g_audio_wav_player_debug.last_fsp_err = err;
        return false;
    }

    desired_clock_hz = ((uint64_t) sample_rate) * 64ULL;
    if ((0ULL == desired_clock_hz) || (desired_clock_hz > timer_info.clock_frequency))
    {
        g_audio_wav_player_debug.last_fsp_err = FSP_ERR_INVALID_ARGUMENT;
        return false;
    }

    period_counts = (uint32_t) ((((uint64_t) timer_info.clock_frequency) + (desired_clock_hz / 2ULL)) /
                                desired_clock_hz);
    if (period_counts < 4U)
    {
        period_counts = 4U;
    }

    err = g_audio_clk_timer.p_api->periodSet(g_audio_clk_timer.p_ctrl, period_counts);
    if (FSP_SUCCESS != err)
    {
        g_audio_wav_player_debug.last_fsp_err = err;
        return false;
    }

    err = g_audio_clk_timer.p_api->dutyCycleSet(g_audio_clk_timer.p_ctrl,
                                                period_counts / 2U,
                                                GPT_IO_PIN_GTIOCA);
    if (FSP_SUCCESS != err)
    {
        g_audio_wav_player_debug.last_fsp_err = err;
        return false;
    }

    (void) g_audio_clk_timer.p_api->reset(g_audio_clk_timer.p_ctrl);

    err = g_audio_clk_timer.p_api->start(g_audio_clk_timer.p_ctrl);
    audio_wav_capture_registers();
    if (FSP_SUCCESS != err)
    {
        g_audio_wav_player_debug.last_fsp_err = err;
        return false;
    }

    return true;
}

static void audio_wav_stop_audio_clock(void)
{
    (void) g_audio_clk_timer.p_api->stop(g_audio_clk_timer.p_ctrl);
    (void) g_audio_clk_timer.p_api->close(g_audio_clk_timer.p_ctrl);
    audio_wav_capture_registers();
}

static void audio_wav_capture_registers(void)
{
    g_audio_wav_player_debug.ssi_ssicr = R_SSI0->SSICR;
    g_audio_wav_player_debug.ssi_ssisr = R_SSI0->SSISR;
    g_audio_wav_player_debug.ssi_ssifcr = R_SSI0->SSIFCR;
    g_audio_wav_player_debug.ssi_ssifsr = R_SSI0->SSIFSR;
    g_audio_wav_player_debug.ssi_ssiofr = R_SSI0->SSIOFR;
    g_audio_wav_player_debug.gpt_gtcr = R_GPT1->GTCR;
    g_audio_wav_player_debug.gpt_gtior = R_GPT1->GTIOR;
    g_audio_wav_player_debug.gpt_gtcnt = R_GPT1->GTCNT;
    g_audio_wav_player_debug.gpt_gtpr = R_GPT1->GTPR;
}

#if !AUDIO_WAV_PLAYER_TEST_TONE
static uint16_t audio_wav_u16(uint8_t const * p_data)
{
    return (uint16_t) ((uint16_t) p_data[0] | ((uint16_t) p_data[1] << 8));
}

static uint32_t audio_wav_u32(uint8_t const * p_data)
{
    return (uint32_t) p_data[0] |
           ((uint32_t) p_data[1] << 8) |
           ((uint32_t) p_data[2] << 16) |
           ((uint32_t) p_data[3] << 24);
}

static int16_t audio_wav_s16(uint8_t const * p_data)
{
    return (int16_t) audio_wav_u16(p_data);
}

static void audio_wav_store_s16(uint8_t * p_data, int16_t value)
{
    uint16_t raw = (uint16_t) value;

    p_data[0] = (uint8_t) raw;
    p_data[1] = (uint8_t) (raw >> 8);
}
#endif

static void audio_wav_debug_set_path(char const * p_path)
{
    size_t i;

    for (i = 0U; i < sizeof(g_audio_wav_player_debug.path) - 1U; i++)
    {
        g_audio_wav_player_debug.path[i] = p_path[i];
        if ('\0' == p_path[i])
        {
            return;
        }
    }

    g_audio_wav_player_debug.path[i] = '\0';
}

static void audio_wav_boot_state_init(void)
{
    silent_reset_boot_state_t boot_state = {0};

    g_audio_boot_flags = 0U;
    g_audio_boot_capture_count = 0U;
    g_audio_silent_boot_count = 0U;
    g_audio_boot_cold_warm_flag = 0U;
    g_audio_boot_rstsr0 = 0U;
    g_audio_boot_rstsr1 = 0U;
    g_audio_boot_rstsr3 = 0U;

    if (!silent_reset_boot_state_get(&boot_state))
    {
        return;
    }

    g_audio_boot_flags = AUDIO_IPC_BOOT_FLAG_STATE_VALID;
    g_audio_boot_capture_count = boot_state.capture_count;
    g_audio_silent_boot_count = boot_state.silent_boot_count;
    g_audio_boot_cold_warm_flag = boot_state.cold_warm_flag;
    g_audio_boot_rstsr0 = boot_state.rstsr0;
    g_audio_boot_rstsr1 = boot_state.rstsr1;
    g_audio_boot_rstsr3 = boot_state.rstsr3;

    if (SILENT_RESET_CWSF_WARM == boot_state.cold_warm_flag)
    {
        g_audio_boot_flags |= AUDIO_IPC_BOOT_FLAG_WARM_START |
                              AUDIO_IPC_BOOT_FLAG_AUDIO_SUPPRESSED;
    }
    else
    {
        g_audio_boot_flags |= AUDIO_IPC_BOOT_FLAG_COLD_START;
    }

    if (0U != (boot_state.rstsr1 & SILENT_RESET_RSTSR1_SOFTWARE_RESET))
    {
        g_audio_boot_flags |= AUDIO_IPC_BOOT_FLAG_SOFTWARE_RESET;
    }
}

static void audio_wav_ipc_init(void)
{
    volatile audio_ipc_mailbox_t * p_ipc = AUDIO_IPC_MAILBOX;

    memset((void *) p_ipc, 0, sizeof(*p_ipc));
    p_ipc->magic = AUDIO_IPC_MAGIC;
    p_ipc->volume_percent = g_audio_volume_percent;
    p_ipc->status = (uint32_t) AUDIO_IPC_STATUS_WAIT_SD;
    p_ipc->boot_flags = g_audio_boot_flags;
    p_ipc->boot_capture_count = g_audio_boot_capture_count;
    p_ipc->silent_boot_count = g_audio_silent_boot_count;
    p_ipc->boot_cold_warm_flag = g_audio_boot_cold_warm_flag;
    p_ipc->boot_rstsr0 = g_audio_boot_rstsr0;
    p_ipc->boot_rstsr1 = g_audio_boot_rstsr1;
    p_ipc->boot_rstsr3 = g_audio_boot_rstsr3;
    audio_wav_ipc_set_name(AUDIO_WAV_PLAYER_DEFAULT_PATH);
    __DMB();
    p_ipc->status_sequence++;
}

static void audio_wav_ipc_publish(void)
{
    volatile audio_ipc_mailbox_t * p_ipc = AUDIO_IPC_MAILBOX;
    uint32_t status = (uint32_t) g_audio_wav_player_debug.status;
    char path[AUDIO_WAV_PLAYER_PATH_LENGTH];

    if (AUDIO_IPC_MAGIC != p_ipc->magic)
    {
        audio_wav_ipc_init();
    }

    if ((uint32_t) AUDIO_IPC_STATUS_PAUSED == p_ipc->status)
    {
        status = (uint32_t) AUDIO_IPC_STATUS_PAUSED;
    }
    else if ((uint32_t) AUDIO_IPC_STATUS_STOPPED == p_ipc->status)
    {
        status = (uint32_t) AUDIO_IPC_STATUS_STOPPED;
    }

    p_ipc->status = status;
    p_ipc->track_index = g_audio_current_track_index;
    p_ipc->volume_percent = g_audio_volume_percent;
    p_ipc->sample_rate = g_audio_wav_player_debug.sample_rate;
    p_ipc->data_bytes = g_audio_wav_player_debug.data_bytes;
    p_ipc->bytes_read = g_audio_wav_player_debug.bytes_read;
    p_ipc->last_fsp_err = (uint32_t) g_audio_wav_player_debug.last_fsp_err;
    p_ipc->last_file_error = (int32_t) g_audio_wav_player_debug.last_file_error;
    p_ipc->duration_ms = 0U;
    p_ipc->elapsed_ms = 0U;

    if ((0U != g_audio_wav_player_debug.sample_rate) &&
        (0U != g_audio_wav_player_debug.channels) &&
        (0U != g_audio_wav_player_debug.bits_per_sample))
    {
        uint32_t bytes_per_second =
            g_audio_wav_player_debug.sample_rate *
            (uint32_t) g_audio_wav_player_debug.channels *
            ((uint32_t) g_audio_wav_player_debug.bits_per_sample / 8U);

        if (0U != bytes_per_second)
        {
            p_ipc->duration_ms = (uint32_t) ((((uint64_t) g_audio_wav_player_debug.data_bytes) * 1000ULL) /
                                             bytes_per_second);
            p_ipc->elapsed_ms = (uint32_t) ((((uint64_t) g_audio_wav_player_debug.bytes_read) * 1000ULL) /
                                            bytes_per_second);
        }
    }

    for (uint32_t i = 0U; i < AUDIO_WAV_PLAYER_PATH_LENGTH; i++)
    {
        path[i] = g_audio_wav_player_debug.path[i];
    }

    audio_wav_ipc_set_name(path);
    __DMB();
    p_ipc->status_sequence++;
}

static void audio_wav_ipc_set_name(char const * p_path)
{
    volatile audio_ipc_mailbox_t * p_ipc = AUDIO_IPC_MAILBOX;
    char const * p_name = p_path;
    size_t i;

    for (char const * p = p_path; '\0' != *p; p++)
    {
        if (('/' == *p) || ('\\' == *p))
        {
            p_name = p + 1;
        }
    }

    for (i = 0U; i < (AUDIO_IPC_NAME_LENGTH - 1U); i++)
    {
        p_ipc->current_name[i] = p_name[i];
        if ('\0' == p_name[i])
        {
            return;
        }
    }

    p_ipc->current_name[i] = '\0';
}

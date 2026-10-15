#include "thread_code/mic_record_writer.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "ff_stdio.h"
#include "thread_code/mic_ipc_shared.h"
#include "thread_code/sd_card_driver.h"

#define MIC_RECORD_DIR          SD_CARD_MOUNT_PATH "/MIC"
#define MIC_RECORD_MAX_RUN_ID   (9999999U)
#define MIC_RECORD_CUSTOM_SUFFIX_MAX (999U)
#define MIC_RECORD_83_BASENAME_MAX (8U)
#define MIC_RECORD_COLLISION_PREFIX (4)
#define MIC_RECORD_PATH_LENGTH  (64U)
#define MIC_RECORD_WAV_HEADER_BYTES (44U)
#define MIC_RECORD_DEFAULT_SAMPLE_SHIFT (8U)
#define MIC_RECORD_DEFAULT_SLEW_LIMIT   (32767U)
#define MIC_RECORD_DEFAULT_GAIN_PERCENT (400U)
#define MIC_RECORD_CHANNELS     (1U)
#define MIC_RECORD_BITS         (16U)
#define MIC_RECORD_BLOCK_ALIGN  ((MIC_RECORD_CHANNELS * MIC_RECORD_BITS) / 8U)
#define MIC_RECORD_HP_ALPHA_Q15 (31506)
#define MIC_RECORD_LP_ALPHA_Q15 (25956)
#define MIC_RECORD_CHUNK_AGC_ENABLED (0U)
#define MIC_RECORD_DECLICK_WINDOW (5U)
#define MIC_RECORD_DECLICK_HALF_WINDOW (2U)
#define MIC_RECORD_DECLICK_MIN_DEVIATION (0x00080000U)
#define MIC_RECORD_DECLICK_MAD_MULTIPLIER (8U)
#define MIC_RECORD_DECLICK_MARGIN (0x00010000U)
#if MIC_RECORD_CHUNK_AGC_ENABLED
#define MIC_RECORD_RMS_TARGET   (4096U)
#define MIC_RECORD_RMS_MIN_GAIN_Q12 (2048U)
#define MIC_RECORD_RMS_MAX_GAIN_Q12 (32768U)
#endif
#define MIC_RECORD_QUEUE_BLOCKS (32U)
#define MIC_RECORD_SD_BUFFER_SAMPLES (8192U)
#define MIC_RECORD_MONO_SAMPLES_MAX ((MIC_IPC_PCM_WORDS + 1U) / 2U)

typedef struct st_mic_record_queue_block
{
    uint32_t sequence;
    uint32_t bytes;
    int32_t pcm[MIC_IPC_PCM_WORDS];
} mic_record_queue_block_t;

static FF_FILE * gp_mic_record_file;
static uint32_t g_mic_record_sequence;
static uint32_t g_mic_last_chunk_sequence;
static uint32_t g_mic_record_next_run_id = 1U;
static uint32_t g_mic_record_data_bytes;
static int32_t g_mic_record_dc_estimate[2];
static int32_t g_mic_record_last_output[2];
static int32_t g_mic_record_hp_input[2];
static int32_t g_mic_record_hp_output[2];
static int32_t g_mic_record_lp_output[2];
static int32_t g_mic_record_declick_history[2][MIC_RECORD_DECLICK_HALF_WINDOW];
static uint32_t g_mic_record_declick_history_count[2];
static uint32_t g_mic_record_latched_slot;
static char g_mic_record_path[MIC_RECORD_PATH_LENGTH];
static mic_record_queue_block_t g_mic_record_queue[MIC_RECORD_QUEUE_BLOCKS];
static volatile uint32_t g_mic_record_queue_write;
static volatile uint32_t g_mic_record_queue_read;
static uint32_t g_mic_capture_last_sequence;
static int16_t g_mic_record_sd_buffer[MIC_RECORD_SD_BUFFER_SAMPLES] BSP_ALIGN_VARIABLE(4);
static uint32_t g_mic_record_sd_samples;

static void mic_record_writer_set_status(uint32_t status, uint32_t error)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;

    if (MIC_IPC_MAGIC == p_mic->magic)
    {
        p_mic->writer_status = status;
        p_mic->writer_error = error;
    }
}

static uint32_t mic_record_fs_error(void)
{
    int error = stdioGET_ERRNO();

    sd_card_driver_report_io_error(error);
    return (uint32_t) error;
}

static void mic_record_write_u16_le(uint8_t * p_data, uint16_t value)
{
    p_data[0] = (uint8_t) value;
    p_data[1] = (uint8_t) (value >> 8);
}

static void mic_record_write_u32_le(uint8_t * p_data, uint32_t value)
{
    p_data[0] = (uint8_t) value;
    p_data[1] = (uint8_t) (value >> 8);
    p_data[2] = (uint8_t) (value >> 16);
    p_data[3] = (uint8_t) (value >> 24);
}

static void mic_record_make_wav_header(uint8_t * p_header, uint32_t data_bytes)
{
    uint32_t byte_rate = MIC_IPC_SAMPLE_RATE * MIC_RECORD_BLOCK_ALIGN;
    uint32_t riff_size = data_bytes + MIC_RECORD_WAV_HEADER_BYTES - 8U;

    memset(p_header, 0, MIC_RECORD_WAV_HEADER_BYTES);
    memcpy(&p_header[0], "RIFF", 4);
    mic_record_write_u32_le(&p_header[4], riff_size);
    memcpy(&p_header[8], "WAVE", 4);
    memcpy(&p_header[12], "fmt ", 4);
    mic_record_write_u32_le(&p_header[16], 16U);
    mic_record_write_u16_le(&p_header[20], 1U);
    mic_record_write_u16_le(&p_header[22], MIC_RECORD_CHANNELS);
    mic_record_write_u32_le(&p_header[24], MIC_IPC_SAMPLE_RATE);
    mic_record_write_u32_le(&p_header[28], byte_rate);
    mic_record_write_u16_le(&p_header[32], MIC_RECORD_BLOCK_ALIGN);
    mic_record_write_u16_le(&p_header[34], MIC_RECORD_BITS);
    memcpy(&p_header[36], "data", 4);
    mic_record_write_u32_le(&p_header[40], data_bytes);
}

static void mic_record_write_wav_header(uint32_t data_bytes)
{
    uint8_t header[MIC_RECORD_WAV_HEADER_BYTES];

    if (NULL == gp_mic_record_file)
    {
        return;
    }

    mic_record_make_wav_header(header, data_bytes);
    (void) ff_fseek(gp_mic_record_file, 0L, FF_SEEK_SET);
    (void) ff_fwrite(header, 1U, sizeof(header), gp_mic_record_file);
}

static bool mic_record_flush_sd_buffer(void)
{
    if ((NULL == gp_mic_record_file) || (0U == g_mic_record_sd_samples))
    {
        return true;
    }

    if (g_mic_record_sd_samples !=
        ff_fwrite(g_mic_record_sd_buffer, sizeof(int16_t), g_mic_record_sd_samples, gp_mic_record_file))
    {
        mic_record_writer_set_status(MIC_IPC_WRITER_WRITE_ERROR, mic_record_fs_error());
        return false;
    }

    g_mic_record_data_bytes += g_mic_record_sd_samples * sizeof(int16_t);
    g_mic_record_sd_samples = 0U;
    return true;
}

static uint32_t mic_record_get_sample_shift(volatile mic_ipc_mailbox_t const * p_mic)
{
    uint32_t shift = p_mic->record_sample_shift;

    if ((shift < 8U) || (shift > 24U))
    {
        shift = MIC_RECORD_DEFAULT_SAMPLE_SHIFT;
    }

    return shift;
}

static uint32_t mic_record_get_slew_limit(volatile mic_ipc_mailbox_t const * p_mic)
{
    uint32_t limit = p_mic->record_slew_limit;

    if (limit > 32767U)
    {
        limit = MIC_RECORD_DEFAULT_SLEW_LIMIT;
    }

    return limit;
}

static uint32_t mic_record_get_gain_percent(volatile mic_ipc_mailbox_t const * p_mic)
{
    uint32_t gain_percent = p_mic->record_gain_percent;

    if ((gain_percent < 25U) || (gain_percent > 1600U))
    {
        gain_percent = MIC_RECORD_DEFAULT_GAIN_PERCENT;
    }

    return gain_percent;
}

static int32_t mic_record_sign_extend_sample24(int32_t sample)
{
    uint32_t sample_bits = ((uint32_t) sample) & 0x00FFFFFFU;
    if (0U != (sample_bits & 0x00800000U))
    {
        sample_bits |= 0xFF000000U;
    }

    return (int32_t) sample_bits;
}

static int16_t mic_record_pcm32_to_s16_raw(int32_t sample, uint32_t shift)
{
    sample = mic_record_sign_extend_sample24(sample);
    int32_t value = sample >> shift;

    if (value > INT16_MAX)
    {
        value = INT16_MAX;
    }
    else if (value < INT16_MIN)
    {
        value = INT16_MIN;
    }

    return (int16_t) value;
}

static int16_t mic_record_pcm32_to_s16_filtered(int32_t sample,
                                                uint32_t slot,
                                                uint32_t shift,
                                                uint32_t slew_limit,
                                                uint32_t gain_percent)
{
    sample = mic_record_sign_extend_sample24(sample);
    int32_t value = sample >> shift;
    int32_t centered = value - g_mic_record_dc_estimate[slot & 1U];
    int32_t delta;
    uint32_t slot_index = slot & 1U;

    g_mic_record_dc_estimate[slot_index] += centered >> 8;
    centered = (int32_t) ((((int64_t) centered) * (int64_t) gain_percent) / 100LL);

    delta = centered - g_mic_record_last_output[slot_index];
    if (delta > (int32_t) slew_limit)
    {
        centered = g_mic_record_last_output[slot_index] + (int32_t) slew_limit;
    }
    else if (delta < -((int32_t) slew_limit))
    {
        centered = g_mic_record_last_output[slot_index] - (int32_t) slew_limit;
    }

    if (centered > INT16_MAX)
    {
        centered = INT16_MAX;
    }
    else if (centered < INT16_MIN)
    {
        centered = INT16_MIN;
    }

    g_mic_record_last_output[slot_index] = centered;
    return (int16_t) centered;
}

static uint32_t mic_record_abs_diff_i32(int32_t left, int32_t right)
{
    int64_t difference = (int64_t) left - (int64_t) right;

    return (uint32_t) ((difference < 0) ? -difference : difference);
}

static void mic_record_sort_i32(int32_t * p_values, uint32_t count)
{
    for (uint32_t i = 1U; i < count; i++)
    {
        int32_t value = p_values[i];
        uint32_t j = i;

        while ((j > 0U) && (p_values[j - 1U] > value))
        {
            p_values[j] = p_values[j - 1U];
            j--;
        }
        p_values[j] = value;
    }
}

static void mic_record_sort_u32(uint32_t * p_values, uint32_t count)
{
    for (uint32_t i = 1U; i < count; i++)
    {
        uint32_t value = p_values[i];
        uint32_t j = i;

        while ((j > 0U) && (p_values[j - 1U] > value))
        {
            p_values[j] = p_values[j - 1U];
            j--;
        }
        p_values[j] = value;
    }
}

static int32_t mic_record_declick_window_sample(int32_t const * p_samples,
                                                uint32_t samples,
                                                int32_t index,
                                                uint32_t slot_index)
{
    if (index < 0)
    {
        uint32_t history_count = g_mic_record_declick_history_count[slot_index];
        uint32_t distance = (uint32_t) -index;

        if (history_count >= distance)
        {
            return g_mic_record_declick_history[slot_index][history_count - distance];
        }

        index = -index;
    }
    else if ((uint32_t) index >= samples)
    {
        index = ((int32_t) (samples * 2U) - 2) - index;
    }

    if (index < 0)
    {
        index = 0;
    }
    else if ((uint32_t) index >= samples)
    {
        index = (int32_t) samples - 1;
    }

    return p_samples[index];
}

static void mic_record_declick_chunk(int32_t * p_samples,
                                     uint32_t samples,
                                     uint32_t slot,
                                     volatile mic_ipc_mailbox_t * p_mic)
{
    uint32_t slot_index = slot & 1U;

    if (0U == samples)
    {
        return;
    }

    for (uint32_t i = 0U; i < samples; i++)
    {
        int32_t window[MIC_RECORD_DECLICK_WINDOW];
        int32_t sorted[MIC_RECORD_DECLICK_WINDOW];
        uint32_t deviations[MIC_RECORD_DECLICK_WINDOW];

        for (uint32_t j = 0U; j < MIC_RECORD_DECLICK_WINDOW; j++)
        {
            int32_t offset = (int32_t) j - (int32_t) MIC_RECORD_DECLICK_HALF_WINDOW;
            window[j] = mic_record_declick_window_sample(p_samples,
                                                         samples,
                                                         (int32_t) i + offset,
                                                         slot_index);
            sorted[j] = window[j];
        }

        mic_record_sort_i32(sorted, MIC_RECORD_DECLICK_WINDOW);
        int32_t median = sorted[MIC_RECORD_DECLICK_HALF_WINDOW];
        for (uint32_t j = 0U; j < MIC_RECORD_DECLICK_WINDOW; j++)
        {
            deviations[j] = mic_record_abs_diff_i32(window[j], median);
        }
        mic_record_sort_u32(deviations, MIC_RECORD_DECLICK_WINDOW);

        uint32_t threshold = deviations[MIC_RECORD_DECLICK_HALF_WINDOW] *
                             MIC_RECORD_DECLICK_MAD_MULTIPLIER +
                             MIC_RECORD_DECLICK_MARGIN;
        if (threshold < MIC_RECORD_DECLICK_MIN_DEVIATION)
        {
            threshold = MIC_RECORD_DECLICK_MIN_DEVIATION;
        }

        if (mic_record_abs_diff_i32(p_samples[i], median) > threshold)
        {
            p_samples[i] = median;
            p_mic->writer_declick_count++;
        }
    }

    if (samples >= MIC_RECORD_DECLICK_HALF_WINDOW)
    {
        g_mic_record_declick_history[slot_index][0] =
            p_samples[samples - MIC_RECORD_DECLICK_HALF_WINDOW];
        g_mic_record_declick_history[slot_index][1] = p_samples[samples - 1U];
        g_mic_record_declick_history_count[slot_index] = MIC_RECORD_DECLICK_HALF_WINDOW;
    }
    else
    {
        g_mic_record_declick_history[slot_index][0] = p_samples[0];
        g_mic_record_declick_history[slot_index][1] = p_samples[0];
        g_mic_record_declick_history_count[slot_index] = 1U;
    }
}

static int16_t mic_record_bandpass(int16_t sample, uint32_t slot)
{
    uint32_t slot_index = slot & 1U;
    int32_t input = sample;
    int32_t high_pass = (int32_t) ((((int64_t) MIC_RECORD_HP_ALPHA_Q15) *
                                    ((int64_t) g_mic_record_hp_output[slot_index] +
                                     input - g_mic_record_hp_input[slot_index])) >> 15);
    int32_t low_pass = g_mic_record_lp_output[slot_index] +
                       (int32_t) ((((int64_t) MIC_RECORD_LP_ALPHA_Q15) *
                                  (high_pass - g_mic_record_lp_output[slot_index])) >> 15);

    g_mic_record_hp_input[slot_index] = input;
    g_mic_record_hp_output[slot_index] = high_pass;
    g_mic_record_lp_output[slot_index] = low_pass;

    if (low_pass > INT16_MAX)
    {
        low_pass = INT16_MAX;
    }
    else if (low_pass < INT16_MIN)
    {
        low_pass = INT16_MIN;
    }

    return (int16_t) low_pass;
}

#if MIC_RECORD_CHUNK_AGC_ENABLED
static uint32_t mic_record_isqrt_u64(uint64_t value)
{
    uint64_t bit = (uint64_t) 1U << 62;
    uint64_t result = 0U;

    while (bit > value)
    {
        bit >>= 2;
    }

    while (0U != bit)
    {
        if (value >= (result + bit))
        {
            value -= result + bit;
            result = (result >> 1) + bit;
        }
        else
        {
            result >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t) result;
}

static void mic_record_normalize_rms(int16_t * p_pcm, uint32_t samples)
{
    uint64_t sum_squares = 0U;
    uint32_t gain_q12;

    if (0U == samples)
    {
        return;
    }

    for (uint32_t i = 0U; i < samples; i++)
    {
        int32_t value = p_pcm[i];
        sum_squares += (uint64_t) ((int64_t) value * value);
    }

    uint32_t rms = mic_record_isqrt_u64(sum_squares / samples);
    if (0U == rms)
    {
        return;
    }

    gain_q12 = (uint32_t) ((((uint64_t) MIC_RECORD_RMS_TARGET) << 12) / rms);
    if (gain_q12 < MIC_RECORD_RMS_MIN_GAIN_Q12)
    {
        gain_q12 = MIC_RECORD_RMS_MIN_GAIN_Q12;
    }
    else if (gain_q12 > MIC_RECORD_RMS_MAX_GAIN_Q12)
    {
        gain_q12 = MIC_RECORD_RMS_MAX_GAIN_Q12;
    }

    for (uint32_t i = 0U; i < samples; i++)
    {
        int32_t value = (int32_t) ((((int64_t) p_pcm[i]) * gain_q12) >> 12);
        if (value > INT16_MAX)
        {
            value = INT16_MAX;
        }
        else if (value < INT16_MIN)
        {
            value = INT16_MIN;
        }
        p_pcm[i] = (int16_t) value;
    }
}
#endif

static uint32_t mic_record_abs_s16(int16_t value)
{
    if (INT16_MIN == value)
    {
        return (uint32_t) INT16_MAX;
    }

    return (value < 0) ? (uint32_t) -value : (uint32_t) value;
}

static uint32_t mic_record_analyze_slot_peak(volatile int32_t const * p_pcm, uint32_t words, uint32_t slot, uint32_t shift)
{
    uint32_t peak = 0U;

    for (uint32_t i = slot; i < words; i += 2U)
    {
        uint32_t sample_abs = mic_record_abs_s16(mic_record_pcm32_to_s16_raw(p_pcm[i], shift));
        if (sample_abs > peak)
        {
            peak = sample_abs;
        }
    }

    return peak;
}

static uint32_t mic_record_select_active_slot(volatile mic_ipc_mailbox_t * p_mic,
                                              int32_t const * p_pcm,
                                              uint32_t words)
{
    uint32_t shift = mic_record_get_sample_shift(p_mic);
    uint32_t even_peak = mic_record_analyze_slot_peak(p_pcm, words, 0U, shift);
    uint32_t odd_peak = mic_record_analyze_slot_peak(p_pcm, words, 1U, shift);
    uint32_t slot_select = p_mic->record_slot_select;
    uint32_t active_slot;

    p_mic->writer_even_peak = even_peak;
    p_mic->writer_odd_peak = odd_peak;

    if (MIC_IPC_RECORD_SLOT_ODD == slot_select)
    {
        active_slot = 1U;
    }
    else if (MIC_IPC_RECORD_SLOT_AUTO == slot_select)
    {
        if ((0U == g_mic_record_latched_slot) && (odd_peak > (even_peak * 2U + 256U)))
        {
            g_mic_record_latched_slot = 1U;
        }
        else if ((1U == g_mic_record_latched_slot) && (even_peak > (odd_peak * 2U + 256U)))
        {
            g_mic_record_latched_slot = 0U;
        }

        active_slot = g_mic_record_latched_slot;
    }
    else
    {
        active_slot = 0U;
    }

    p_mic->writer_active_slot = active_slot;
    return active_slot;
}

static bool mic_record_path_exists(char const * p_path)
{
    FF_FILE * p_file = ff_fopen(p_path, "r");
    if (NULL == p_file)
    {
        (void) mic_record_fs_error();
        return false;
    }

    if (0 != ff_fclose(p_file))
    {
        (void) mic_record_fs_error();
    }
    return true;
}

static bool mic_record_basename_char_valid(char value)
{
    return ((value >= 'A') && (value <= 'Z')) ||
           ((value >= 'a') && (value <= 'z')) ||
           ((value >= '0') && (value <= '9')) ||
           ('_' == value) || ('-' == value);
}

static bool mic_record_get_custom_basename(char * p_basename, size_t basename_size)
{
    volatile mic_ipc_mailbox_t const * p_mic = MIC_IPC_MAILBOX;
    size_t length = 0U;

    if (basename_size < 2U)
    {
        return false;
    }
    memset(p_basename, 0, basename_size);
    while (length < sizeof(p_mic->record_basename))
    {
        char value = p_mic->record_basename[length];
        if ('\0' == value)
        {
            return length > 0U;
        }
        if (!mic_record_basename_char_valid(value) ||
            (length >= MIC_RECORD_83_BASENAME_MAX) ||
            ((length + 1U) >= basename_size))
        {
            memset(p_basename, 0, basename_size);
            return false;
        }
        p_basename[length++] = ((value >= 'a') && (value <= 'z')) ? (char) (value - ('a' - 'A')) : value;
    }

    memset(p_basename, 0, basename_size);
    return false;
}

static void mic_record_publish_filename(char const * p_path)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;
    char const * p_filename = strrchr(p_path, '/');
    size_t index = 0U;

    p_filename = (NULL == p_filename) ? p_path : (p_filename + 1);
    memset((void *) p_mic->writer_filename, 0, sizeof(p_mic->writer_filename));
    while (('\0' != p_filename[index]) && ((index + 1U) < sizeof(p_mic->writer_filename)))
    {
        p_mic->writer_filename[index] = p_filename[index];
        index++;
    }
    p_mic->writer_filename[index] = '\0';
}

static bool mic_record_open_file(uint32_t record_sequence)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;
    char custom_basename[MIC_IPC_RECORD_BASENAME_BYTES];
    bool custom_name = mic_record_get_custom_basename(custom_basename, sizeof(custom_basename));
    uint32_t first_run = custom_name ? 0U : g_mic_record_next_run_id;
    uint32_t last_run = custom_name ? MIC_RECORD_CUSTOM_SUFFIX_MAX : MIC_RECORD_MAX_RUN_ID;

    if (0 != ff_mkdir(MIC_RECORD_DIR))
    {
        mic_record_writer_set_status(MIC_IPC_WRITER_MKDIR, mic_record_fs_error());
        return false;
    }

    memset((void *) p_mic->writer_filename, 0, sizeof(p_mic->writer_filename));
    for (uint32_t run = first_run; run <= last_run; run++)
    {
        int path_length;
        if (custom_name && (0U == run))
        {
            path_length = snprintf(g_mic_record_path,
                                   sizeof(g_mic_record_path),
                                   MIC_RECORD_DIR "/%s.WAV",
                                   custom_basename);
        }
        else if (custom_name)
        {
            path_length = snprintf(g_mic_record_path,
                                   sizeof(g_mic_record_path),
                                   MIC_RECORD_DIR "/%.*s_%03lu.WAV",
                                   MIC_RECORD_COLLISION_PREFIX,
                                   custom_basename,
                                   (unsigned long) run);
        }
        else
        {
            path_length = snprintf(g_mic_record_path,
                                   sizeof(g_mic_record_path),
                                   MIC_RECORD_DIR "/M%07lu.WAV",
                                   (unsigned long) run);
        }
        if ((path_length < 0) || ((size_t) path_length >= sizeof(g_mic_record_path)))
        {
            mic_record_writer_set_status(MIC_IPC_WRITER_OPEN_ERROR, (uint32_t) FSP_ERR_INVALID_SIZE);
            return false;
        }
        if (!mic_record_path_exists(g_mic_record_path))
        {
            gp_mic_record_file = ff_fopen(g_mic_record_path, "wb");
            if (NULL != gp_mic_record_file)
            {
                g_mic_record_sequence = record_sequence;
                g_mic_last_chunk_sequence = 0U;
                if (!custom_name)
                {
                    g_mic_record_next_run_id = run + 1U;
                }
                g_mic_record_queue_read = g_mic_record_queue_write;
                g_mic_record_data_bytes = 0U;
                g_mic_record_sd_samples = 0U;
                g_mic_record_dc_estimate[0] = 0;
                g_mic_record_dc_estimate[1] = 0;
                g_mic_record_last_output[0] = 0;
                g_mic_record_last_output[1] = 0;
                g_mic_record_hp_input[0] = 0;
                g_mic_record_hp_input[1] = 0;
                g_mic_record_hp_output[0] = 0;
                g_mic_record_hp_output[1] = 0;
                g_mic_record_lp_output[0] = 0;
                g_mic_record_lp_output[1] = 0;
                memset(g_mic_record_declick_history, 0, sizeof(g_mic_record_declick_history));
                memset(g_mic_record_declick_history_count, 0, sizeof(g_mic_record_declick_history_count));
                g_mic_record_latched_slot = 0U;
                p_mic->writer_gap_count = 0U;
                p_mic->writer_declick_count = 0U;
                if (0U == p_mic->record_sample_shift)
                {
                    p_mic->record_sample_shift = MIC_RECORD_DEFAULT_SAMPLE_SHIFT;
                }
                if (0U == p_mic->record_slew_limit)
                {
                    p_mic->record_slew_limit = MIC_RECORD_DEFAULT_SLEW_LIMIT;
                }
                if (0U == p_mic->record_gain_percent)
                {
                    p_mic->record_gain_percent = MIC_RECORD_DEFAULT_GAIN_PERCENT;
                }
                mic_record_write_wav_header(0U);
                (void) ff_fseek(gp_mic_record_file, 0L, FF_SEEK_END);
                mic_record_publish_filename(g_mic_record_path);
                p_mic->writer_open_count++;
                p_mic->writer_bytes = 0U;
                p_mic->writer_record_sequence = record_sequence;
                p_mic->writer_status = MIC_IPC_WRITER_OPEN;
                p_mic->writer_error = 0U;
                return true;
            }

            mic_record_writer_set_status(MIC_IPC_WRITER_OPEN_ERROR, mic_record_fs_error());
            return false;
        }
    }

    mic_record_writer_set_status(MIC_IPC_WRITER_OPEN_ERROR, 0U);
    return false;
}

static void mic_record_close_file(void)
{
    if (NULL != gp_mic_record_file)
    {
        (void) mic_record_flush_sd_buffer();
        mic_record_write_wav_header(g_mic_record_data_bytes);
        if (0 != ff_fclose(gp_mic_record_file))
        {
            mic_record_writer_set_status(MIC_IPC_WRITER_WRITE_ERROR, mic_record_fs_error());
        }
        gp_mic_record_file = NULL;
        if (MIC_IPC_WRITER_WRITE_ERROR != MIC_IPC_MAILBOX->writer_status)
        {
            mic_record_writer_set_status(MIC_IPC_WRITER_CLOSED, 0U);
        }
    }
}

void mic_record_capture_poll(void)
{
    volatile mic_ipc_mailbox_t const * p_mic = MIC_IPC_MAILBOX;

    if ((MIC_IPC_MAGIC != p_mic->magic) || (0U == p_mic->record_active))
    {
        if (MIC_IPC_MAGIC == p_mic->magic)
        {
            g_mic_capture_last_sequence = p_mic->chunk_sequence;
        }
        return;
    }

    uint32_t sequence = p_mic->chunk_sequence;
    if ((0U == p_mic->chunk_bytes) || (sequence == g_mic_capture_last_sequence))
    {
        return;
    }

    if (0U != g_mic_capture_last_sequence)
    {
        uint32_t sequence_delta = sequence - g_mic_capture_last_sequence;
        if ((sequence_delta > 1U) && (sequence_delta < 0x80000000U))
        {
            MIC_IPC_MAILBOX->writer_gap_count += sequence_delta - 1U;
        }
    }

    uint32_t write_index = g_mic_record_queue_write;
    if ((write_index - g_mic_record_queue_read) >= MIC_RECORD_QUEUE_BLOCKS)
    {
        MIC_IPC_MAILBOX->writer_gap_count++;
        g_mic_capture_last_sequence = sequence;
        return;
    }

    mic_record_queue_block_t * p_block = &g_mic_record_queue[write_index % MIC_RECORD_QUEUE_BLOCKS];
    uint32_t bytes = p_mic->chunk_bytes;
    if (bytes > (MIC_IPC_PCM_WORDS * sizeof(int32_t)))
    {
        bytes = MIC_IPC_PCM_WORDS * sizeof(int32_t);
    }

    uint32_t words = bytes / sizeof(int32_t);
    for (uint32_t i = 0U; i < words; i++)
    {
        p_block->pcm[i] = p_mic->pcm[i];
    }
    __DMB();
    if (sequence != p_mic->chunk_sequence)
    {
        return;
    }

    p_block->sequence = sequence;
    p_block->bytes = bytes;
    __DMB();
    g_mic_record_queue_write = write_index + 1U;
    g_mic_capture_last_sequence = sequence;
}

void mic_record_writer_poll(bool sd_ready)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;

    if (MIC_IPC_MAGIC == p_mic->magic)
    {
        p_mic->writer_poll_count++;
    }

    if (!sd_ready)
    {
        mic_record_close_file();
        mic_record_writer_set_status(MIC_IPC_WRITER_NO_SD, 0U);
        return;
    }

    if (MIC_IPC_MAGIC != p_mic->magic)
    {
        mic_record_close_file();
        return;
    }

    if (0U == p_mic->record_active)
    {
        mic_record_close_file();
        mic_record_writer_set_status(MIC_IPC_WRITER_WAIT_RECORD, 0U);
        return;
    }

    if ((NULL == gp_mic_record_file) || (g_mic_record_sequence != p_mic->record_sequence))
    {
        mic_record_close_file();
        if (!mic_record_open_file(p_mic->record_sequence))
        {
            return;
        }
    }

    if (g_mic_record_queue_read != g_mic_record_queue_write)
    {
        static int16_t pcm16[MIC_IPC_PCM_WORDS];
        static int32_t raw24[MIC_RECORD_MONO_SAMPLES_MAX];
        uint32_t read_index = g_mic_record_queue_read;
        mic_record_queue_block_t const * p_block =
            &g_mic_record_queue[read_index % MIC_RECORD_QUEUE_BLOCKS];
        uint32_t chunk_sequence = p_block->sequence;
        uint32_t bytes = p_block->bytes;
        uint32_t words;
        uint32_t samples = 0U;
        uint32_t shift = mic_record_get_sample_shift(p_mic);
        uint32_t slew_limit = mic_record_get_slew_limit(p_mic);
        uint32_t gain_percent = mic_record_get_gain_percent(p_mic);
        if (bytes > (MIC_IPC_PCM_WORDS * sizeof(int32_t)))
        {
            bytes = MIC_IPC_PCM_WORDS * sizeof(int32_t);
        }

        words = bytes / sizeof(int32_t);
        uint32_t active_slot = mic_record_select_active_slot(p_mic, p_block->pcm, words);
        for (uint32_t i = active_slot; i < words; i += 2U)
        {
            if (samples < MIC_RECORD_MONO_SAMPLES_MAX)
            {
                raw24[samples++] = mic_record_sign_extend_sample24(p_block->pcm[i]);
            }
        }

        mic_record_declick_chunk(raw24, samples, active_slot, p_mic);
        for (uint32_t i = 0U; i < samples; i++)
        {
            int16_t sample = mic_record_pcm32_to_s16_filtered(raw24[i],
                                                              active_slot,
                                                              shift,
                                                              slew_limit,
                                                              gain_percent);
            pcm16[i] = mic_record_bandpass(sample, active_slot);
        }
#if MIC_RECORD_CHUNK_AGC_ENABLED
        /*
         * A gain chosen independently for every 192-sample chunk changes at
         * 83.33 Hz and creates audible pumping and discontinuities. Keep the
         * implementation available for controlled experiments, but record
         * the continuous fixed-gain/filter output by default.
         */
        mic_record_normalize_rms(pcm16, samples);
#endif

        if ((g_mic_record_sd_samples + samples) > MIC_RECORD_SD_BUFFER_SAMPLES)
        {
            if (!mic_record_flush_sd_buffer())
            {
                return;
            }
        }

        memcpy(&g_mic_record_sd_buffer[g_mic_record_sd_samples], pcm16, samples * sizeof(int16_t));
        g_mic_record_sd_samples += samples;
        g_mic_last_chunk_sequence = chunk_sequence;
        g_mic_record_queue_read = read_index + 1U;
        p_mic->writer_write_count++;
        p_mic->writer_bytes = g_mic_record_data_bytes + (g_mic_record_sd_samples * sizeof(int16_t));
        p_mic->writer_chunk_sequence = chunk_sequence;
        p_mic->writer_status = MIC_IPC_WRITER_WRITE;
        p_mic->writer_error = 0U;
    }
}

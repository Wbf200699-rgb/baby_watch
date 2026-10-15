#include "thread_code/audio_cry_inference.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "audio_infer_thread.h"
#include "thread_code/audio_cry_model_data.h"
#include "thread_code/audio_cry_type_model_data.h"
#include "thread_code/multimodal_fusion.h"

#define CRY_SAMPLE_RATE_HZ              (16000U)
#define CRY_WINDOW_SAMPLES              (16000U)
#define CRY_HOP_SAMPLES                 (4000U)
#define CRY_FRAME_SAMPLES               (400U)
#define CRY_FRAME_STEP                  (320U)
#define CRY_FFT_SIZE                    (512U)
#define CRY_FFT_BINS                    (257U)
#define CRY_FRAME_COUNT                 (49U)
#define CRY_MEL_BINS                    (40U)
#define CRY_MFCC_COUNT                  (10U)
#define CRY_FFT_WINDOW_OFFSET           ((CRY_FFT_SIZE - CRY_FRAME_SAMPLES) / 2U)
#define CRY_DECISION_THRESHOLD_X1000    (850U)
#define CRY_VOTE_WINDOW_COUNT           (8U)
#define CRY_VOTE_REQUIRED_HITS          (6U)
#define CRY_PROBABILITY_MEDIAN_WINDOWS  (3U)
#define CRY_RELEASE_HOLD_WINDOWS        (12U)
#define CRY_FUSION_THRESHOLD_PERCENT    (75U)
#define CRY_TYPE_HISTORY_WINDOWS        (8U)
#define CRY_TYPE_MINIMUM_WINDOWS        (3U)
#define CRY_TYPE_CONFIDENCE_X1000       (550U)
#define CRY_TYPE_MARGIN_X1000           (100U)
#define CRY_WORKER_STACK_WORDS          (4096U)
#define CRY_WORKER_PRIORITY             (tskIDLE_PRIORITY + 1U)
#define CRY_CAPTURE_SAMPLE_SHIFT        (8U)
#define CRY_CAPTURE_SLEW_LIMIT          (4096)
#define CRY_CAPTURE_GAIN_PERCENT        (400)
#define CRY_CAPTURE_HP_ALPHA_Q15        (31506)
#define CRY_CAPTURE_LP_ALPHA_Q15        (25956)
#define CRY_CAPTURE_INVALID_ABS         (0x7F0000)
#define CRY_CAPTURE_DECLICK_WINDOW      (5U)
#define CRY_CAPTURE_DECLICK_HALF_WINDOW (2U)
#define CRY_CAPTURE_DECLICK_MIN_DEVIATION (0x00080000U)
#define CRY_CAPTURE_DECLICK_MAD_MULTIPLIER (8U)
#define CRY_CAPTURE_DECLICK_MARGIN      (0x00010000U)
#define CRY_RAW_SLOT                    (0U)
#define CRY_MAX_CHUNK_SAMPLES           (256U)
#define CRY_LOG_AMIN                    (1.0e-10f)
#define CRY_LOG_TOP_DB                  (80.0f)
#define CRY_TARGET_RMS                  (0.1f)
#define CRY_MINIMUM_GAIN                (0.25f)
#define CRY_MAXIMUM_GAIN                (10.0f)

volatile uint32_t g_cry_model_ready;
volatile uint32_t g_cry_inference_count;
volatile uint32_t g_cry_probability_x1000;
volatile uint32_t g_cry_consecutive_windows;
volatile uint32_t g_cry_confirmed;
volatile uint32_t g_cry_release_hold_windows;
volatile uint32_t g_cry_last_inference_ms;
volatile uint32_t g_cry_snapshot_retries;
volatile uint32_t g_cry_instant_probability_x1000;
volatile uint32_t g_cry_task_create_failed;
volatile uint32_t g_cry_type_model_ready;
volatile uint32_t g_cry_type_id;
volatile uint32_t g_cry_type_valid;
volatile uint32_t g_cry_type_confidence_x1000;
volatile uint32_t g_cry_type_margin_x1000;
volatile uint32_t g_cry_type_window_count;
volatile uint32_t g_cry_type_probabilities_x1000[CRY_TYPE_MODEL_CLASS_COUNT];

static StaticTask_t g_cry_worker_memory;
static StackType_t g_cry_worker_stack[CRY_WORKER_STACK_WORDS] BSP_ALIGN_VARIABLE(BSP_STACK_ALIGNMENT);
static TaskHandle_t g_cry_worker_task;

static int16_t g_cry_ring[CRY_WINDOW_SAMPLES] BSP_ALIGN_VARIABLE(4);
static int16_t g_cry_snapshot[CRY_WINDOW_SAMPLES] BSP_ALIGN_VARIABLE(4);
static volatile uint32_t g_cry_ring_sequence;
static volatile uint32_t g_cry_ring_write;
static volatile uint32_t g_cry_ring_count;
static uint32_t g_cry_samples_since_request;
static volatile bool g_cry_request_pending;

static int32_t g_cry_capture_dc_estimate;
static int32_t g_cry_capture_last_output;
static int32_t g_cry_capture_hp_input;
static int32_t g_cry_capture_hp_output;
static int32_t g_cry_capture_lp_output;
static int32_t g_cry_capture_last_valid_sample24;
static int32_t g_cry_capture_declick_history[CRY_CAPTURE_DECLICK_HALF_WINDOW];
static uint32_t g_cry_capture_declick_history_count;

static float g_cry_wave[CRY_WINDOW_SAMPLES] BSP_ALIGN_VARIABLE(4);
static float g_cry_fft_real[CRY_FFT_SIZE] BSP_ALIGN_VARIABLE(4);
static float g_cry_fft_imag[CRY_FFT_SIZE] BSP_ALIGN_VARIABLE(4);
static float g_cry_power[CRY_FFT_BINS] BSP_ALIGN_VARIABLE(4);
static float g_cry_log_mel[CRY_FRAME_COUNT * CRY_MEL_BINS] BSP_ALIGN_VARIABLE(4);
static int8_t g_cry_input[CRY_MODEL_INPUT_COUNT] BSP_ALIGN_VARIABLE(4);
static int8_t g_cry_hidden1[CRY_MODEL_HIDDEN1_COUNT] BSP_ALIGN_VARIABLE(4);
static int8_t g_cry_hidden2[CRY_MODEL_HIDDEN2_COUNT] BSP_ALIGN_VARIABLE(4);
static int8_t g_cry_type_input[CRY_TYPE_MODEL_INPUT_COUNT] BSP_ALIGN_VARIABLE(4);
static int8_t g_cry_type_hidden1[CRY_TYPE_MODEL_HIDDEN1_COUNT] BSP_ALIGN_VARIABLE(4);
static int8_t g_cry_type_hidden2[CRY_TYPE_MODEL_HIDDEN2_COUNT] BSP_ALIGN_VARIABLE(4);
static uint8_t g_cry_vote_history[CRY_VOTE_WINDOW_COUNT];
static uint32_t g_cry_vote_index;
static uint32_t g_cry_probability_history[CRY_PROBABILITY_MEDIAN_WINDOWS];
static uint32_t g_cry_probability_history_index;
static uint32_t g_cry_probability_history_count;
static float g_cry_type_probability_history[CRY_TYPE_HISTORY_WINDOWS]
                                           [CRY_TYPE_MODEL_CLASS_COUNT];
static uint32_t g_cry_type_probability_index;

static int32_t cry_round_to_i32(float value)
{
    return (value >= 0.0f) ? (int32_t) (value + 0.5f) : (int32_t) (value - 0.5f);
}

static int8_t cry_quantize_symmetric(float value, float scale, bool relu)
{
    int32_t quantized = cry_round_to_i32(value / scale);
    int32_t minimum = relu ? 0 : -127;
    if (quantized > 127)
    {
        quantized = 127;
    }
    else if (quantized < minimum)
    {
        quantized = minimum;
    }

    return (int8_t) quantized;
}

static uint32_t cry_smooth_probability(uint32_t probability_x1000)
{
    uint32_t values[CRY_PROBABILITY_MEDIAN_WINDOWS];
    uint32_t count;

    g_cry_probability_history[g_cry_probability_history_index] =
        probability_x1000;
    g_cry_probability_history_index++;
    if (g_cry_probability_history_index >= CRY_PROBABILITY_MEDIAN_WINDOWS)
    {
        g_cry_probability_history_index = 0U;
    }
    if (g_cry_probability_history_count < CRY_PROBABILITY_MEDIAN_WINDOWS)
    {
        g_cry_probability_history_count++;
    }
    count = g_cry_probability_history_count;
    for (uint32_t i = 0U; i < count; i++)
    {
        values[i] = g_cry_probability_history[i];
    }
    for (uint32_t i = 1U; i < count; i++)
    {
        uint32_t value = values[i];
        uint32_t j = i;

        while ((j > 0U) && (values[j - 1U] > value))
        {
            values[j] = values[j - 1U];
            j--;
        }
        values[j] = value;
    }
    return values[count / 2U];
}

static void cry_type_reset(void)
{
    memset(g_cry_type_probability_history, 0, sizeof(g_cry_type_probability_history));
    g_cry_type_probability_index = 0U;
    g_cry_type_id = 0U;
    g_cry_type_valid = 0U;
    g_cry_type_confidence_x1000 = 0U;
    g_cry_type_margin_x1000 = 0U;
    g_cry_type_window_count = 0U;
    for (uint32_t class_index = 0U;
         class_index < CRY_TYPE_MODEL_CLASS_COUNT;
         class_index++)
    {
        g_cry_type_probabilities_x1000[class_index] = 0U;
    }
}

char const * audio_cry_type_label(void)
{
    switch (g_cry_type_id)
    {
        case 1U:
            return "hungry";

        case 2U:
            return "belly_pain";

        case 3U:
            return "discomfort";

        case 4U:
            return "tired";

        case 5U:
            return "burping";

        default:
            return "unknown";
    }
}

static uint32_t cry_capture_abs_diff_i32(int32_t left, int32_t right)
{
    int64_t difference = (int64_t) left - (int64_t) right;

    return (uint32_t) ((difference < 0) ? -difference : difference);
}

static void cry_capture_sort_i32(int32_t * p_values, uint32_t count)
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

static void cry_capture_sort_u32(uint32_t * p_values, uint32_t count)
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

static int32_t cry_capture_decode_sample24(int32_t raw)
{
    uint32_t bits = ((uint32_t) raw) & 0x00FFFFFFU;
    int32_t sample;

    if (0U != (bits & 0x00800000U))
    {
        bits |= 0xFF000000U;
    }
    sample = (int32_t) bits;
    if (-8388608 == sample)
    {
        sample = 0;
    }
    if ((sample >= CRY_CAPTURE_INVALID_ABS) ||
        (sample <= -CRY_CAPTURE_INVALID_ABS))
    {
        return g_cry_capture_last_valid_sample24;
    }
    g_cry_capture_last_valid_sample24 = sample;
    return sample;
}

static int32_t cry_capture_declick_window_sample(int32_t const * p_samples,
                                                 uint32_t samples,
                                                 int32_t index)
{
    if (index < 0)
    {
        uint32_t distance = (uint32_t) -index;

        if (g_cry_capture_declick_history_count >= distance)
        {
            return g_cry_capture_declick_history[
                g_cry_capture_declick_history_count - distance];
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

static void cry_capture_declick_chunk(int32_t * p_samples, uint32_t samples)
{
    if (0U == samples)
    {
        return;
    }
    for (uint32_t i = 0U; i < samples; i++)
    {
        int32_t window[CRY_CAPTURE_DECLICK_WINDOW];
        int32_t sorted[CRY_CAPTURE_DECLICK_WINDOW];
        uint32_t deviations[CRY_CAPTURE_DECLICK_WINDOW];

        for (uint32_t j = 0U; j < CRY_CAPTURE_DECLICK_WINDOW; j++)
        {
            int32_t offset =
                (int32_t) j - (int32_t) CRY_CAPTURE_DECLICK_HALF_WINDOW;
            window[j] = cry_capture_declick_window_sample(
                p_samples, samples, (int32_t) i + offset);
            sorted[j] = window[j];
        }
        cry_capture_sort_i32(sorted, CRY_CAPTURE_DECLICK_WINDOW);
        int32_t median = sorted[CRY_CAPTURE_DECLICK_HALF_WINDOW];
        for (uint32_t j = 0U; j < CRY_CAPTURE_DECLICK_WINDOW; j++)
        {
            deviations[j] = cry_capture_abs_diff_i32(window[j], median);
        }
        cry_capture_sort_u32(deviations, CRY_CAPTURE_DECLICK_WINDOW);

        uint32_t threshold =
            deviations[CRY_CAPTURE_DECLICK_HALF_WINDOW] *
            CRY_CAPTURE_DECLICK_MAD_MULTIPLIER +
            CRY_CAPTURE_DECLICK_MARGIN;
        if (threshold < CRY_CAPTURE_DECLICK_MIN_DEVIATION)
        {
            threshold = CRY_CAPTURE_DECLICK_MIN_DEVIATION;
        }
        if (cry_capture_abs_diff_i32(p_samples[i], median) > threshold)
        {
            p_samples[i] = median;
        }
    }
    if (samples >= CRY_CAPTURE_DECLICK_HALF_WINDOW)
    {
        g_cry_capture_declick_history[0] =
            p_samples[samples - CRY_CAPTURE_DECLICK_HALF_WINDOW];
        g_cry_capture_declick_history[1] = p_samples[samples - 1U];
        g_cry_capture_declick_history_count =
            CRY_CAPTURE_DECLICK_HALF_WINDOW;
    }
    else
    {
        g_cry_capture_declick_history[0] = p_samples[0];
        g_cry_capture_declick_history[1] = p_samples[0];
        g_cry_capture_declick_history_count = 1U;
    }
}

static int16_t cry_capture_filter_sample24(int32_t sample)
{
    int32_t centered;
    int32_t delta;
    int32_t high_pass;
    int32_t low_pass;

    sample >>= CRY_CAPTURE_SAMPLE_SHIFT;
    centered = sample - g_cry_capture_dc_estimate;
    g_cry_capture_dc_estimate += centered >> 8;
    centered = (int32_t) ((((int64_t) centered) * CRY_CAPTURE_GAIN_PERCENT) / 100LL);

    delta = centered - g_cry_capture_last_output;
    if (delta > CRY_CAPTURE_SLEW_LIMIT)
    {
        centered = g_cry_capture_last_output + CRY_CAPTURE_SLEW_LIMIT;
    }
    else if (delta < -CRY_CAPTURE_SLEW_LIMIT)
    {
        centered = g_cry_capture_last_output - CRY_CAPTURE_SLEW_LIMIT;
    }
    if (centered > INT16_MAX)
    {
        centered = INT16_MAX;
    }
    else if (centered < INT16_MIN)
    {
        centered = INT16_MIN;
    }
    g_cry_capture_last_output = centered;

    high_pass = (int32_t) ((((int64_t) CRY_CAPTURE_HP_ALPHA_Q15) *
                            ((int64_t) g_cry_capture_hp_output +
                             centered - g_cry_capture_hp_input)) >> 15);
    low_pass = g_cry_capture_lp_output +
               (int32_t) ((((int64_t) CRY_CAPTURE_LP_ALPHA_Q15) *
                           (high_pass - g_cry_capture_lp_output)) >> 15);
    g_cry_capture_hp_input = centered;
    g_cry_capture_hp_output = high_pass;
    g_cry_capture_lp_output = low_pass;

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

void audio_cry_inference_push_raw_chunk(int32_t const * p_words, uint32_t word_count)
{
    int32_t raw24[CRY_MAX_CHUNK_SAMPLES];
    int16_t samples[CRY_MAX_CHUNK_SAMPLES];
    uint32_t count = 0U;
    uint32_t write;

    if ((NULL == p_words) || (word_count <= CRY_RAW_SLOT))
    {
        return;
    }
    for (uint32_t i = CRY_RAW_SLOT;
         (i < word_count) && (count < CRY_MAX_CHUNK_SAMPLES);
         i += 2U)
    {
        raw24[count++] = cry_capture_decode_sample24(p_words[i]);
    }
    cry_capture_declick_chunk(raw24, count);
    for (uint32_t i = 0U; i < count; i++)
    {
        samples[i] = cry_capture_filter_sample24(raw24[i]);
    }

    g_cry_ring_sequence++;
    __DMB();
    write = g_cry_ring_write;
    for (uint32_t i = 0U; i < count; i++)
    {
        g_cry_ring[write] = samples[i];
        write++;
        if (write >= CRY_WINDOW_SAMPLES)
        {
            write = 0U;
        }
    }
    g_cry_ring_write = write;
    if ((g_cry_ring_count + count) >= CRY_WINDOW_SAMPLES)
    {
        g_cry_ring_count = CRY_WINDOW_SAMPLES;
    }
    else
    {
        g_cry_ring_count += count;
    }
    __DMB();
    g_cry_ring_sequence++;

    if (g_cry_ring_count >= CRY_WINDOW_SAMPLES)
    {
        g_cry_samples_since_request += count;
        if (g_cry_samples_since_request >= CRY_HOP_SAMPLES)
        {
            g_cry_samples_since_request -= CRY_HOP_SAMPLES;
            if (!g_cry_request_pending && (NULL != g_cry_worker_task))
            {
                g_cry_request_pending = true;
                xTaskNotifyGive(g_cry_worker_task);
            }
        }
    }
}

static bool cry_snapshot_ring(void)
{
    for (uint32_t attempt = 0U; attempt < 8U; attempt++)
    {
        uint32_t before = g_cry_ring_sequence;
        uint32_t write;
        uint32_t tail;
        uint32_t after;

        if (0U != (before & 1U))
        {
            g_cry_snapshot_retries++;
            continue;
        }
        __DMB();
        if (g_cry_ring_count < CRY_WINDOW_SAMPLES)
        {
            return false;
        }
        write = g_cry_ring_write;
        tail = CRY_WINDOW_SAMPLES - write;
        memcpy(g_cry_snapshot, &g_cry_ring[write], tail * sizeof(int16_t));
        if (write > 0U)
        {
            memcpy(&g_cry_snapshot[tail], g_cry_ring, write * sizeof(int16_t));
        }
        __DMB();
        after = g_cry_ring_sequence;
        if ((before == after) && (0U == (after & 1U)))
        {
            return true;
        }
        g_cry_snapshot_retries++;
    }

    return false;
}

static void cry_fft_forward(void)
{
    uint32_t j = 0U;

    for (uint32_t i = 1U; i < CRY_FFT_SIZE; i++)
    {
        uint32_t bit = CRY_FFT_SIZE >> 1;
        while (0U != (j & bit))
        {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j)
        {
            float temporary = g_cry_fft_real[i];
            g_cry_fft_real[i] = g_cry_fft_real[j];
            g_cry_fft_real[j] = temporary;
            temporary = g_cry_fft_imag[i];
            g_cry_fft_imag[i] = g_cry_fft_imag[j];
            g_cry_fft_imag[j] = temporary;
        }
    }

    for (uint32_t length = 2U; length <= CRY_FFT_SIZE; length <<= 1)
    {
        uint32_t half = length >> 1;
        uint32_t twiddle_step = CRY_FFT_SIZE / length;
        for (uint32_t base = 0U; base < CRY_FFT_SIZE; base += length)
        {
            for (uint32_t offset = 0U; offset < half; offset++)
            {
                uint32_t twiddle = offset * twiddle_step;
                uint32_t upper = base + offset;
                uint32_t lower = upper + half;
                float wr = g_cry_twiddle_real[twiddle];
                float wi = g_cry_twiddle_imag[twiddle];
                float lower_real = g_cry_fft_real[lower];
                float lower_imag = g_cry_fft_imag[lower];
                float product_real = (wr * lower_real) - (wi * lower_imag);
                float product_imag = (wr * lower_imag) + (wi * lower_real);
                float upper_real = g_cry_fft_real[upper];
                float upper_imag = g_cry_fft_imag[upper];
                g_cry_fft_real[upper] = upper_real + product_real;
                g_cry_fft_imag[upper] = upper_imag + product_imag;
                g_cry_fft_real[lower] = upper_real - product_real;
                g_cry_fft_imag[lower] = upper_imag - product_imag;
            }
        }
    }
}

static void cry_prepare_waveform(void)
{
    float mean = 0.0f;
    float sum_squares = 0.0f;
    float rms;
    float gain = 1.0f;

    for (uint32_t i = 0U; i < CRY_WINDOW_SAMPLES; i++)
    {
        g_cry_wave[i] = ((float) g_cry_snapshot[i]) / 32768.0f;
        mean += g_cry_wave[i];
    }
    mean /= (float) CRY_WINDOW_SAMPLES;
    for (uint32_t i = 0U; i < CRY_WINDOW_SAMPLES; i++)
    {
        float value = g_cry_wave[i] - mean;
        g_cry_wave[i] = value;
        sum_squares += value * value;
    }
    rms = sqrtf(sum_squares / (float) CRY_WINDOW_SAMPLES);
    if (rms > 1.0e-9f)
    {
        gain = CRY_TARGET_RMS / rms;
        if (gain < CRY_MINIMUM_GAIN)
        {
            gain = CRY_MINIMUM_GAIN;
        }
        else if (gain > CRY_MAXIMUM_GAIN)
        {
            gain = CRY_MAXIMUM_GAIN;
        }
    }
    for (uint32_t i = 0U; i < CRY_WINDOW_SAMPLES; i++)
    {
        float value = g_cry_wave[i] * gain;
        if (value > 1.0f)
        {
            value = 1.0f;
        }
        else if (value < -1.0f)
        {
            value = -1.0f;
        }
        g_cry_wave[i] = value;
    }
}

static void cry_extract_quantized_mfcc(void)
{
    float maximum_log_mel = -1.0e30f;

    cry_prepare_waveform();
    for (uint32_t frame = 0U; frame < CRY_FRAME_COUNT; frame++)
    {
        uint32_t sample_offset = frame * CRY_FRAME_STEP;
        uint32_t mel_weight_offset = 0U;

        memset(g_cry_fft_real, 0, sizeof(g_cry_fft_real));
        memset(g_cry_fft_imag, 0, sizeof(g_cry_fft_imag));
        for (uint32_t i = 0U; i < CRY_FRAME_SAMPLES; i++)
        {
            g_cry_fft_real[CRY_FFT_WINDOW_OFFSET + i] =
                g_cry_wave[sample_offset + CRY_FFT_WINDOW_OFFSET + i] * g_cry_hann[i];
        }
        cry_fft_forward();
        for (uint32_t bin = 0U; bin < CRY_FFT_BINS; bin++)
        {
            float real = g_cry_fft_real[bin];
            float imag = g_cry_fft_imag[bin];
            g_cry_power[bin] = (real * real) + (imag * imag);
        }
        for (uint32_t mel = 0U; mel < CRY_MEL_BINS; mel++)
        {
            uint32_t start = g_cry_mel_starts[mel];
            uint32_t length = g_cry_mel_lengths[mel];
            float energy = 0.0f;
            float logarithm;

            for (uint32_t i = 0U; i < length; i++)
            {
                energy += g_cry_power[start + i] *
                          g_cry_mel_weights[mel_weight_offset + i];
            }
            mel_weight_offset += length;
            if (energy < CRY_LOG_AMIN)
            {
                energy = CRY_LOG_AMIN;
            }
            logarithm = 10.0f * log10f(energy);
            g_cry_log_mel[(frame * CRY_MEL_BINS) + mel] = logarithm;
            if (logarithm > maximum_log_mel)
            {
                maximum_log_mel = logarithm;
            }
        }
    }

    for (uint32_t frame = 0U; frame < CRY_FRAME_COUNT; frame++)
    {
        for (uint32_t coefficient = 0U; coefficient < CRY_MFCC_COUNT; coefficient++)
        {
            float value = 0.0f;
            float minimum_log = maximum_log_mel - CRY_LOG_TOP_DB;
            uint32_t feature_index = (frame * CRY_MFCC_COUNT) + coefficient;

            for (uint32_t mel = 0U; mel < CRY_MEL_BINS; mel++)
            {
                float logarithm = g_cry_log_mel[(frame * CRY_MEL_BINS) + mel];
                if (logarithm < minimum_log)
                {
                    logarithm = minimum_log;
                }
                value += logarithm * g_cry_dct[(coefficient * CRY_MEL_BINS) + mel];
            }
            float detection_value =
                (value - g_cry_coefficient_mean[coefficient]) /
                g_cry_coefficient_std[coefficient];
            float type_value =
                (value - g_cry_type_coefficient_mean[coefficient]) /
                g_cry_type_coefficient_std[coefficient];
            g_cry_input[feature_index] =
                cry_quantize_symmetric(detection_value, CRY_MODEL_INPUT_SCALE, false);
            g_cry_type_input[feature_index] =
                cry_quantize_symmetric(type_value, CRY_TYPE_MODEL_INPUT_SCALE, false);
        }
    }
}

static void cry_dense_relu(int8_t const * p_input,
                           uint32_t input_count,
                           float input_scale,
                           int8_t const * p_weights,
                           float const * p_weight_scales,
                           float const * p_bias,
                           uint32_t output_count,
                           float output_scale,
                           int8_t * p_output)
{
    for (uint32_t output = 0U; output < output_count; output++)
    {
        int32_t accumulator = 0;
        uint32_t weight_offset = output * input_count;
        float value;

        for (uint32_t input = 0U; input < input_count; input++)
        {
            accumulator += ((int32_t) p_input[input]) *
                           ((int32_t) p_weights[weight_offset + input]);
        }
        value = ((float) accumulator) * input_scale * p_weight_scales[output] +
                p_bias[output];
        if (value < 0.0f)
        {
            value = 0.0f;
        }
        p_output[output] = cry_quantize_symmetric(value, output_scale, true);
    }
}

static float cry_model_probability(void)
{
    float logits[2];

    cry_dense_relu(g_cry_input,
                   CRY_MODEL_INPUT_COUNT,
                   CRY_MODEL_INPUT_SCALE,
                   g_cry_dense1_weights,
                   g_cry_dense1_scales,
                   g_cry_dense1_bias,
                   CRY_MODEL_HIDDEN1_COUNT,
                   CRY_MODEL_HIDDEN1_SCALE,
                   g_cry_hidden1);
    cry_dense_relu(g_cry_hidden1,
                   CRY_MODEL_HIDDEN1_COUNT,
                   CRY_MODEL_HIDDEN1_SCALE,
                   g_cry_dense2_weights,
                   g_cry_dense2_scales,
                   g_cry_dense2_bias,
                   CRY_MODEL_HIDDEN2_COUNT,
                   CRY_MODEL_HIDDEN2_SCALE,
                   g_cry_hidden2);
    for (uint32_t output = 0U; output < 2U; output++)
    {
        int32_t accumulator = 0;
        uint32_t weight_offset = output * CRY_MODEL_HIDDEN2_COUNT;
        for (uint32_t input = 0U; input < CRY_MODEL_HIDDEN2_COUNT; input++)
        {
            accumulator += ((int32_t) g_cry_hidden2[input]) *
                           ((int32_t) g_cry_dense3_weights[weight_offset + input]);
        }
        logits[output] = ((float) accumulator) *
                         CRY_MODEL_HIDDEN2_SCALE *
                         g_cry_dense3_scales[output] +
                         g_cry_dense3_bias[output];
    }

    float difference = logits[0] - logits[1];
    if (difference > 80.0f)
    {
        difference = 80.0f;
    }
    else if (difference < -80.0f)
    {
        difference = -80.0f;
    }
    return 1.0f / (1.0f + expf(difference));
}

static void cry_type_model_probabilities(float probabilities[CRY_TYPE_MODEL_CLASS_COUNT])
{
    float logits[CRY_TYPE_MODEL_CLASS_COUNT];
    float maximum_logit = -1.0e30f;
    float sum = 0.0f;

    cry_dense_relu(g_cry_type_input,
                   CRY_TYPE_MODEL_INPUT_COUNT,
                   CRY_TYPE_MODEL_INPUT_SCALE,
                   g_cry_type_dense1_weights,
                   g_cry_type_dense1_scales,
                   g_cry_type_dense1_bias,
                   CRY_TYPE_MODEL_HIDDEN1_COUNT,
                   CRY_TYPE_MODEL_HIDDEN1_SCALE,
                   g_cry_type_hidden1);
    cry_dense_relu(g_cry_type_hidden1,
                   CRY_TYPE_MODEL_HIDDEN1_COUNT,
                   CRY_TYPE_MODEL_HIDDEN1_SCALE,
                   g_cry_type_dense2_weights,
                   g_cry_type_dense2_scales,
                   g_cry_type_dense2_bias,
                   CRY_TYPE_MODEL_HIDDEN2_COUNT,
                   CRY_TYPE_MODEL_HIDDEN2_SCALE,
                   g_cry_type_hidden2);
    for (uint32_t output = 0U; output < CRY_TYPE_MODEL_CLASS_COUNT; output++)
    {
        int32_t accumulator = 0;
        uint32_t weight_offset = output * CRY_TYPE_MODEL_HIDDEN2_COUNT;
        for (uint32_t input = 0U; input < CRY_TYPE_MODEL_HIDDEN2_COUNT; input++)
        {
            accumulator +=
                ((int32_t) g_cry_type_hidden2[input]) *
                ((int32_t) g_cry_type_dense3_weights[weight_offset + input]);
        }
        logits[output] =
            ((float) accumulator) *
            CRY_TYPE_MODEL_HIDDEN2_SCALE *
            g_cry_type_dense3_scales[output] +
            g_cry_type_dense3_bias[output];
        if (logits[output] > maximum_logit)
        {
            maximum_logit = logits[output];
        }
    }
    for (uint32_t output = 0U; output < CRY_TYPE_MODEL_CLASS_COUNT; output++)
    {
        probabilities[output] = expf(logits[output] - maximum_logit);
        sum += probabilities[output];
    }
    if (sum <= 0.0f)
    {
        for (uint32_t output = 0U; output < CRY_TYPE_MODEL_CLASS_COUNT; output++)
        {
            probabilities[output] = 0.0f;
        }
        return;
    }
    for (uint32_t output = 0U; output < CRY_TYPE_MODEL_CLASS_COUNT; output++)
    {
        probabilities[output] /= sum;
    }
}

static void cry_type_update(float const probabilities[CRY_TYPE_MODEL_CLASS_COUNT])
{
    float averages[CRY_TYPE_MODEL_CLASS_COUNT];
    uint32_t best_index = 0U;
    uint32_t second_index = 0U;
    uint32_t count = g_cry_type_window_count;

    for (uint32_t class_index = 0U;
         class_index < CRY_TYPE_MODEL_CLASS_COUNT;
         class_index++)
    {
        g_cry_type_probability_history[g_cry_type_probability_index][class_index] =
            probabilities[class_index];
    }
    g_cry_type_probability_index++;
    if (g_cry_type_probability_index >= CRY_TYPE_HISTORY_WINDOWS)
    {
        g_cry_type_probability_index = 0U;
    }
    if (count < CRY_TYPE_HISTORY_WINDOWS)
    {
        count++;
        g_cry_type_window_count = count;
    }

    for (uint32_t class_index = 0U;
         class_index < CRY_TYPE_MODEL_CLASS_COUNT;
         class_index++)
    {
        float sum = 0.0f;
        for (uint32_t window = 0U; window < count; window++)
        {
            sum += g_cry_type_probability_history[window][class_index];
        }
        averages[class_index] = sum / (float) count;
        g_cry_type_probabilities_x1000[class_index] =
            (uint32_t) cry_round_to_i32(averages[class_index] * 1000.0f);
    }

    for (uint32_t class_index = 1U;
         class_index < CRY_TYPE_MODEL_CLASS_COUNT;
         class_index++)
    {
        if (averages[class_index] > averages[best_index])
        {
            second_index = best_index;
            best_index = class_index;
        }
        else if ((second_index == best_index) ||
                 (averages[class_index] > averages[second_index]))
        {
            second_index = class_index;
        }
    }
    g_cry_type_confidence_x1000 =
        (uint32_t) cry_round_to_i32(averages[best_index] * 1000.0f);
    g_cry_type_margin_x1000 =
        (uint32_t) cry_round_to_i32(
            (averages[best_index] - averages[second_index]) * 1000.0f);
    if ((count >= CRY_TYPE_MINIMUM_WINDOWS) &&
        (g_cry_type_confidence_x1000 >= CRY_TYPE_CONFIDENCE_X1000) &&
        (g_cry_type_margin_x1000 >= CRY_TYPE_MARGIN_X1000))
    {
        g_cry_type_id = best_index + 1U;
        g_cry_type_valid = 1U;
    }
    else
    {
        g_cry_type_id = 0U;
        g_cry_type_valid = 0U;
    }
}

static bool cry_update_temporal_decision(uint32_t probability_x1000)
{
    bool hit = probability_x1000 >= CRY_DECISION_THRESHOLD_X1000;

    if (0U != g_cry_vote_history[g_cry_vote_index])
    {
        if (g_cry_consecutive_windows > 0U)
        {
            g_cry_consecutive_windows--;
        }
    }
    g_cry_vote_history[g_cry_vote_index] = hit ? 1U : 0U;
    if (hit)
    {
        g_cry_consecutive_windows++;
    }
    g_cry_vote_index++;
    if (g_cry_vote_index >= CRY_VOTE_WINDOW_COUNT)
    {
        g_cry_vote_index = 0U;
    }

    if (0U == g_cry_confirmed)
    {
        if (g_cry_consecutive_windows >= CRY_VOTE_REQUIRED_HITS)
        {
            g_cry_confirmed = 1U;
            g_cry_release_hold_windows = CRY_RELEASE_HOLD_WINDOWS;
        }
    }
    else if (hit)
    {
        g_cry_release_hold_windows = CRY_RELEASE_HOLD_WINDOWS;
    }
    else
    {
        if (g_cry_release_hold_windows > 0U)
        {
            g_cry_release_hold_windows--;
        }
        if ((0U == g_cry_release_hold_windows) &&
            (g_cry_consecutive_windows < CRY_VOTE_REQUIRED_HITS))
        {
            g_cry_confirmed = 0U;
            cry_type_reset();
        }
    }

    if ((0U == g_cry_confirmed) && (0U == g_cry_consecutive_windows))
    {
        cry_type_reset();
    }
    return hit;
}

static void cry_worker(void * pv_parameters)
{
    FSP_PARAMETER_NOT_USED(pv_parameters);
    g_cry_model_ready = 1U;
    g_cry_type_model_ready = 1U;

    while (1)
    {
        uint32_t start_tick;
        uint32_t instant_probability_x1000;
        uint32_t probability_x1000;
        uint32_t publish_confidence;
        bool cry_hit;
        float probability;
        float type_probabilities[CRY_TYPE_MODEL_CLASS_COUNT];

        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        g_cry_request_pending = false;
        if (!cry_snapshot_ring())
        {
            continue;
        }
        start_tick = xTaskGetTickCount();
        cry_extract_quantized_mfcc();
        probability = cry_model_probability();
        instant_probability_x1000 =
            (uint32_t) cry_round_to_i32(probability * 1000.0f);
        if (instant_probability_x1000 > 1000U)
        {
            instant_probability_x1000 = 1000U;
        }
        g_cry_instant_probability_x1000 = instant_probability_x1000;
        probability_x1000 =
            cry_smooth_probability(instant_probability_x1000);
        g_cry_probability_x1000 = probability_x1000;
        cry_hit = cry_update_temporal_decision(probability_x1000);
        if (cry_hit)
        {
            cry_type_model_probabilities(type_probabilities);
            cry_type_update(type_probabilities);
        }
        publish_confidence = (probability_x1000 + 5U) / 10U;
        if (0U != g_cry_confirmed)
        {
            if (publish_confidence < CRY_FUSION_THRESHOLD_PERCENT)
            {
                publish_confidence = CRY_FUSION_THRESHOLD_PERCENT;
            }
        }
        else if (publish_confidence >= CRY_FUSION_THRESHOLD_PERCENT)
        {
            publish_confidence = CRY_FUSION_THRESHOLD_PERCENT - 1U;
        }
        multimodal_fusion_set_audio_result(publish_confidence, xTaskGetTickCount());
        g_cry_inference_count++;
        g_cry_last_inference_ms =
            (uint32_t) (((xTaskGetTickCount() - start_tick) * 1000U) / configTICK_RATE_HZ);
    }
}

char const * audio_cry_model_version(void)
{
    return CRY_MODEL_SHA256_PREFIX;
}

char const * audio_cry_type_model_version(void)
{
    return CRY_TYPE_MODEL_SHA256_PREFIX;
}

void audio_cry_inference_init(void)
{
    if (NULL != g_cry_worker_task)
    {
        return;
    }
    g_cry_worker_task = xTaskCreateStatic(cry_worker,
                                          "cry_inf",
                                          CRY_WORKER_STACK_WORDS,
                                          NULL,
                                          CRY_WORKER_PRIORITY,
                                          g_cry_worker_stack,
                                          &g_cry_worker_memory);
    if (NULL == g_cry_worker_task)
    {
        g_cry_task_create_failed = 1U;
    }
}

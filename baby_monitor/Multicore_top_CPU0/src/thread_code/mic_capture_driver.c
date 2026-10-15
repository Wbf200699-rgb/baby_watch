#include "thread_code/mic_capture_driver.h"

#include <limits.h>
#include <string.h>

#include "thread_code/audio_cry_inference.h"

extern TaskHandle_t audio_infer_thread;

#if defined(__has_include)
 #if __has_include("r_gpt.h")
  #include "r_gpt.h"
  #define MIC_AUDIO_CLK_TIMER_AVAILABLE (1U)
extern const timer_instance_t g_mic_audio_clk_timer;
 #endif
#endif

#ifndef MIC_AUDIO_CLK_TIMER_AVAILABLE
 #define MIC_AUDIO_CLK_TIMER_AVAILABLE (0U)
#endif

#define MIC_CAPTURE_BUFFER_WORDS  (MIC_IPC_PCM_WORDS)
#define MIC_CAPTURE_BUFFER_BYTES  (MIC_CAPTURE_BUFFER_WORDS * sizeof(int32_t))
#define MIC_CAPTURE_BUFFER_COUNT  (16U)
#define MIC_AUDIO_CLK_HZ          (MIC_IPC_SAMPLE_RATE * 64U)
#define MIC_SAMPLE24_INVALID_ABS  (0x7F0000)
#define MIC_RECORD_DEFAULT_SHIFT  (8U)
#define MIC_RECORD_DEFAULT_SLEW   (32767U)
#define MIC_RECORD_DEFAULT_GAIN   (400U)
#define MIC_SSI_INVERT_BCLK       (0U)
#define MIC_RECOVERY_RETRY_MS     (100U)
#define MIC_CAPTURE_ACTIVE_SLOT   (0U)

static int32_t g_mic_capture_buffers[MIC_CAPTURE_BUFFER_COUNT][MIC_CAPTURE_BUFFER_WORDS] BSP_ALIGN_VARIABLE(4);

static volatile bool g_mic_idle;
static volatile bool g_mic_read_in_progress;
static volatile bool g_mic_ring_paused;
static volatile bool g_mic_requeue_failed;
static volatile fsp_err_t g_mic_requeue_error;
static volatile uint32_t g_mic_capture_completed_write;
static volatile uint32_t g_mic_capture_completed_read;
static bool g_mic_opened;
static volatile bool g_mic_running;
static bool g_mic_audio_clock_started;
static bool g_mic_recording;
static bool g_mic_defaults_applied;
static bool g_mic_recovery_pending;
static uint32_t g_mic_last_command_sequence;
static TickType_t g_mic_record_start_tick;
static volatile TickType_t g_mic_last_read_queue_tick;
static TickType_t g_mic_last_recovery_tick;
static int32_t g_mic_last_valid_sample[2];

static int32_t mic_abs32(int32_t value)
{
    if (value == INT32_MIN)
    {
        return INT32_MAX;
    }

    return (value < 0) ? -value : value;
}

static int32_t mic_raw_to_sample24(int32_t raw)
{
    uint32_t sample_bits = ((uint32_t) raw) & 0x00FFFFFFU;
    if (0U != (sample_bits & 0x00800000U))
    {
        sample_bits |= 0xFF000000U;
    }

    int32_t sample = (int32_t) sample_bits;

    /*
     * A persistent 0x800000 sample is a common symptom of an empty/wrong I2S
     * slot or sign-bit-only capture. Treat it as silence so auto slot selection
     * does not lock onto it as a full-scale audio peak.
     */
    if (-8388608 == sample)
    {
        sample = 0;
    }

    return sample;
}

static int32_t mic_filter_sample24(int32_t raw, uint32_t slot)
{
    int32_t sample = mic_raw_to_sample24(raw);
    int32_t abs_sample = mic_abs32(sample);
    uint32_t slot_index = slot & 1U;

    /*
     * The mic occasionally returns near-full-scale words when the data line is
     * idle or a slot is malformed. They sound like clicks, so replace those
     * isolated invalid words with the previous valid sample for the same slot.
     */
    if (abs_sample >= MIC_SAMPLE24_INVALID_ABS)
    {
        return g_mic_last_valid_sample[slot_index];
    }

    g_mic_last_valid_sample[slot_index] = sample;
    return sample;
}

static fsp_err_t mic_audio_clock_start(void)
{
#if MIC_AUDIO_CLK_TIMER_AVAILABLE
    fsp_err_t err;
    timer_info_t timer_info;
    uint32_t period_counts;

    if (g_mic_audio_clock_started)
    {
        return FSP_SUCCESS;
    }

    err = g_mic_audio_clk_timer.p_api->open(g_mic_audio_clk_timer.p_ctrl,
                                            g_mic_audio_clk_timer.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return err;
    }

    err = g_mic_audio_clk_timer.p_api->infoGet(g_mic_audio_clk_timer.p_ctrl, &timer_info);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    period_counts = (uint32_t) ((((uint64_t) timer_info.clock_frequency) + (MIC_AUDIO_CLK_HZ / 2ULL)) /
                                MIC_AUDIO_CLK_HZ);
    if (period_counts < 4U)
    {
        period_counts = 4U;
    }

    err = g_mic_audio_clk_timer.p_api->periodSet(g_mic_audio_clk_timer.p_ctrl, period_counts);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = g_mic_audio_clk_timer.p_api->dutyCycleSet(g_mic_audio_clk_timer.p_ctrl,
                                                    period_counts / 2U,
                                                    GPT_IO_PIN_GTIOCA);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    (void) g_mic_audio_clk_timer.p_api->reset(g_mic_audio_clk_timer.p_ctrl);

    err = g_mic_audio_clk_timer.p_api->start(g_mic_audio_clk_timer.p_ctrl);
    if (FSP_SUCCESS == err)
    {
        g_mic_audio_clock_started = true;
    }

    return err;
#else
    return FSP_ERR_UNSUPPORTED;
#endif
}

static void mic_audio_clock_stop(void)
{
#if MIC_AUDIO_CLK_TIMER_AVAILABLE
    if (g_mic_audio_clock_started)
    {
        (void) g_mic_audio_clk_timer.p_api->stop(g_mic_audio_clk_timer.p_ctrl);
        (void) g_mic_audio_clk_timer.p_api->close(g_mic_audio_clk_timer.p_ctrl);
        g_mic_audio_clock_started = false;
    }
#endif
}

static void mic_ipc_init_defaults(void)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;
    bool initialized = (MIC_IPC_MAGIC == p_mic->magic);

    if (!initialized)
    {
        memset((void *) p_mic, 0, sizeof(*p_mic));
        p_mic->magic = MIC_IPC_MAGIC;
        p_mic->record_slot_select = MIC_IPC_RECORD_SLOT_EVEN;
        p_mic->record_sample_shift = MIC_RECORD_DEFAULT_SHIFT;
        p_mic->record_slew_limit = MIC_RECORD_DEFAULT_SLEW;
        p_mic->record_gain_percent = MIC_RECORD_DEFAULT_GAIN;
    }

    /*
     * Shared RAM can survive a core-only reset. Apply the firmware defaults
     * once per CPU0 boot so stale tuning from an older image is not reused.
     */
    if (!g_mic_defaults_applied)
    {
        p_mic->record_sample_shift = MIC_RECORD_DEFAULT_SHIFT;
        p_mic->record_slew_limit = MIC_RECORD_DEFAULT_SLEW;
        p_mic->record_gain_percent = MIC_RECORD_DEFAULT_GAIN;
        memset((void *) p_mic->record_basename, 0, sizeof(p_mic->record_basename));
        memset((void *) p_mic->writer_filename, 0, sizeof(p_mic->writer_filename));
        p_mic->writer_gap_count = 0U;
        p_mic->writer_declick_count = 0U;
        g_mic_defaults_applied = true;
    }

    p_mic->sample_rate = MIC_IPC_SAMPLE_RATE;
    p_mic->bits_per_word = MIC_IPC_BITS_PER_WORD;
    p_mic->buffer_samples = MIC_CAPTURE_BUFFER_WORDS;
    if (p_mic->record_slot_select > MIC_IPC_RECORD_SLOT_AUTO)
    {
        p_mic->record_slot_select = MIC_IPC_RECORD_SLOT_EVEN;
    }
    if ((p_mic->record_sample_shift < 8U) || (p_mic->record_sample_shift > 24U))
    {
        p_mic->record_sample_shift = MIC_RECORD_DEFAULT_SHIFT;
    }
    if (p_mic->record_slew_limit > 32767U)
    {
        p_mic->record_slew_limit = MIC_RECORD_DEFAULT_SLEW;
    }
    if ((p_mic->record_gain_percent < 25U) || (p_mic->record_gain_percent > 1600U))
    {
        p_mic->record_gain_percent = MIC_RECORD_DEFAULT_GAIN;
    }
}

static void mic_publish_status(uint32_t status, fsp_err_t err)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;

    mic_ipc_init_defaults();
    p_mic->status = status;
    p_mic->last_fsp_err = (uint32_t) err;
    p_mic->record_active = g_mic_recording ? 1U : 0U;
    if (g_mic_recording)
    {
        p_mic->record_seconds = (uint32_t) ((xTaskGetTickCount() - g_mic_record_start_tick) / configTICK_RATE_HZ);
    }
    __DMB();
    p_mic->sequence++;
}

static void mic_i2s_force_recover(void)
{
    g_mic_running = false;
    g_mic_read_in_progress = false;

    if (g_mic_opened)
    {
        (void) g_mic_i2s.p_api->stop(g_mic_i2s.p_ctrl);
        vTaskDelay(pdMS_TO_TICKS(5));
        (void) g_mic_i2s.p_api->close(g_mic_i2s.p_ctrl);
        g_mic_opened = false;
    }

    g_mic_idle = false;
    g_mic_ring_paused = false;
    g_mic_requeue_failed = false;
    g_mic_requeue_error = FSP_SUCCESS;
    g_mic_capture_completed_write = 0U;
    g_mic_capture_completed_read = 0U;
    g_mic_last_valid_sample[0] = 0;
    g_mic_last_valid_sample[1] = 0;
    mic_audio_clock_stop();
    g_mic_recovery_pending = true;
    g_mic_last_recovery_tick = xTaskGetTickCount();
}

static void mic_process_buffer(int32_t const * p_capture_buffer)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;
    int32_t active_peak = 0;
    int32_t even_peak = 0;
    int32_t odd_peak = 0;
    int32_t last_sample = 0;
    int64_t sum_abs = 0;
    uint32_t nonzero_count = 0U;
    uint32_t active_sample_count = 0U;

    for (uint32_t i = 0U; i < MIC_CAPTURE_BUFFER_WORDS; i++)
    {
        int32_t sample = mic_filter_sample24(p_capture_buffer[i], i);
        int32_t abs_sample = mic_abs32(sample);

        if (0U == (i & 1U))
        {
            if (abs_sample > even_peak)
            {
                even_peak = abs_sample;
            }
        }
        else
        {
            if (abs_sample > odd_peak)
            {
                odd_peak = abs_sample;
            }
        }

        /*
         * SSI1 receives two interleaved slots, but this mono microphone drives
         * only the even slot.  The undriven odd slot can very occasionally
         * contain a fixed 0x400000 word.  It is useful to retain per-slot peak
         * diagnostics below, but it must not be reported as acoustic energy.
         */
        if (MIC_CAPTURE_ACTIVE_SLOT == (i & 1U))
        {
            last_sample = sample;
            active_sample_count++;
            if (abs_sample > active_peak)
            {
                active_peak = abs_sample;
            }
            if (0 != sample)
            {
                nonzero_count++;
            }

            sum_abs += abs_sample;
        }
    }

    p_mic->peak = active_peak;
    p_mic->mean_abs = (active_sample_count > 0U) ?
                      (int32_t) (sum_abs / active_sample_count) :
                      0;
    p_mic->last_sample = last_sample;
    p_mic->nonzero_count = nonzero_count;
    p_mic->writer_even_peak = (uint32_t) even_peak;
    p_mic->writer_odd_peak = (uint32_t) odd_peak;
    p_mic->read_count++;

    audio_cry_inference_push_raw_chunk(p_capture_buffer, MIC_CAPTURE_BUFFER_WORDS);

    if (g_mic_recording)
    {
        for (uint32_t i = 0U; i < MIC_CAPTURE_BUFFER_WORDS; i++)
        {
            /* Preserve the exact SSI FIFO word for RAW32 diagnostics. */
            p_mic->pcm[i] = p_capture_buffer[i];
        }

        p_mic->chunk_bytes = MIC_CAPTURE_BUFFER_BYTES;
        __DMB();
        p_mic->chunk_sequence++;
        p_mic->record_bytes += MIC_CAPTURE_BUFFER_BYTES;
    }

    mic_publish_status(g_mic_recording ? (uint32_t) MIC_IPC_STATUS_RECORDING : (uint32_t) MIC_IPC_STATUS_RUNNING,
                       FSP_SUCCESS);
}

static fsp_err_t mic_capture_submit_read(uint32_t buffer_index)
{
    fsp_err_t err = g_mic_i2s.p_api->read(g_mic_i2s.p_ctrl,
                                          g_mic_capture_buffers[buffer_index],
                                          MIC_CAPTURE_BUFFER_BYTES);

    if (FSP_SUCCESS == err)
    {
        g_mic_read_in_progress = true;
        MIC_IPC_MAILBOX->queue_count++;
    }

    return err;
}

static fsp_err_t mic_capture_queue_initial_read(void)
{
    fsp_err_t err;

    g_mic_idle = false;
    g_mic_read_in_progress = false;
    err = mic_capture_submit_read(0U);
    if (FSP_SUCCESS != err)
    {
        MIC_IPC_MAILBOX->error_count++;
        mic_publish_status(MIC_IPC_STATUS_I2S_ERROR, err);
        if ((FSP_ERR_IN_USE == err) || (FSP_ERR_OVERFLOW == err))
        {
            mic_i2s_force_recover();
        }
    }
    else
    {
        g_mic_last_read_queue_tick = xTaskGetTickCount();
        mic_publish_status(g_mic_recording ? (uint32_t) MIC_IPC_STATUS_RECORDING : (uint32_t) MIC_IPC_STATUS_RUNNING,
                           FSP_SUCCESS);
    }

    return err;
}

fsp_err_t mic_capture_driver_init(void)
{
    fsp_err_t err = FSP_SUCCESS;

    mic_ipc_init_defaults();

    if (!g_mic_opened)
    {
        err = g_mic_i2s.p_api->open(g_mic_i2s.p_ctrl, g_mic_i2s.p_cfg);
        if ((FSP_SUCCESS == err) || (FSP_ERR_ALREADY_OPEN == err))
        {
            g_mic_opened = true;
            err = FSP_SUCCESS;
#if MIC_SSI_INVERT_BCLK
            /*
             * The RAW32 capture showed random high-order bits on the active
             * slot, consistent with sampling while the microphone changes SD.
             * Invert SSI1 BCLK polarity to sample on the opposite edge.
             */
            R_SSI1->SSICR_b.BCKP = 1U;
#endif
        }
        else
        {
            mic_publish_status(MIC_IPC_STATUS_OPEN_ERROR, err);
            return err;
        }
    }

    mic_publish_status(MIC_IPC_STATUS_IDLE, FSP_SUCCESS);
    return err;
}

fsp_err_t mic_capture_start(void)
{
    fsp_err_t err = mic_audio_clock_start();
    if (FSP_SUCCESS != err)
    {
        MIC_IPC_MAILBOX->error_count++;
        mic_publish_status(MIC_IPC_STATUS_I2S_ERROR, err);
        return err;
    }

    err = mic_capture_driver_init();
    if (FSP_SUCCESS != err)
    {
        mic_audio_clock_stop();
        return err;
    }

    if (!g_mic_running)
    {
        g_mic_capture_completed_write = 0U;
        g_mic_capture_completed_read = 0U;
        g_mic_requeue_failed = false;
        g_mic_requeue_error = FSP_SUCCESS;
        g_mic_read_in_progress = false;
        g_mic_ring_paused = false;
        g_mic_running = true;
        err = mic_capture_queue_initial_read();
        if (FSP_SUCCESS != err)
        {
            g_mic_running = false;
            mic_audio_clock_stop();
        }
    }

    if (FSP_SUCCESS == err)
    {
        g_mic_recovery_pending = false;
    }

    return err;
}

fsp_err_t mic_capture_stop(void)
{
    fsp_err_t err = FSP_SUCCESS;

    g_mic_recording = false;
    g_mic_running = false;
    g_mic_read_in_progress = false;
    if (g_mic_opened)
    {
        err = g_mic_i2s.p_api->stop(g_mic_i2s.p_ctrl);
        vTaskDelay(pdMS_TO_TICKS(5));
        (void) g_mic_i2s.p_api->close(g_mic_i2s.p_ctrl);
        g_mic_opened = false;
    }

    g_mic_capture_completed_write = 0U;
    g_mic_capture_completed_read = 0U;
    g_mic_ring_paused = false;
    g_mic_requeue_failed = false;
    g_mic_requeue_error = FSP_SUCCESS;
    g_mic_recovery_pending = false;
    mic_audio_clock_stop();
    mic_publish_status(MIC_IPC_STATUS_STOPPED, err);
    return err;
}

fsp_err_t mic_capture_record_start(void)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;
    fsp_err_t err = mic_capture_start();
    if (FSP_SUCCESS == err)
    {
        g_mic_recording = true;
        g_mic_record_start_tick = xTaskGetTickCount();
        p_mic->record_sequence++;
        p_mic->record_bytes = 0U;
        p_mic->record_seconds = 0U;
        mic_publish_status(MIC_IPC_STATUS_RECORDING, FSP_SUCCESS);
    }

    return err;
}

fsp_err_t mic_capture_record_stop(void)
{
    g_mic_recording = false;
    mic_publish_status(g_mic_running ? (uint32_t) MIC_IPC_STATUS_RUNNING : (uint32_t) MIC_IPC_STATUS_STOPPED,
                       FSP_SUCCESS);
    return FSP_SUCCESS;
}

void mic_capture_poll(void)
{
    volatile mic_ipc_mailbox_t * p_mic = MIC_IPC_MAILBOX;

    mic_ipc_init_defaults();

    if (p_mic->command_sequence != g_mic_last_command_sequence)
    {
        uint32_t command = p_mic->command;
        g_mic_last_command_sequence = p_mic->command_sequence;
        p_mic->command_ack_sequence = g_mic_last_command_sequence;
        p_mic->command = MIC_IPC_COMMAND_NONE;

        if (MIC_IPC_COMMAND_START == command)
        {
            (void) mic_capture_start();
        }
        else if (MIC_IPC_COMMAND_STOP == command)
        {
            (void) mic_capture_stop();
        }
        else if (MIC_IPC_COMMAND_RECORD_START == command)
        {
            (void) mic_capture_record_start();
        }
        else if (MIC_IPC_COMMAND_RECORD_STOP == command)
        {
            (void) mic_capture_record_stop();
        }
    }

    while (g_mic_capture_completed_read != g_mic_capture_completed_write)
    {
        uint32_t read_index = g_mic_capture_completed_read;
        uint32_t buffer_index = read_index % MIC_CAPTURE_BUFFER_COUNT;

        mic_process_buffer(g_mic_capture_buffers[buffer_index]);
        __DMB();
        g_mic_capture_completed_read = read_index + 1U;

        if (g_mic_ring_paused && g_mic_running)
        {
            g_mic_ring_paused = false;
            fsp_err_t err = mic_capture_submit_read(g_mic_capture_completed_write %
                                                    MIC_CAPTURE_BUFFER_COUNT);
            if (FSP_SUCCESS == err)
            {
                g_mic_last_read_queue_tick = xTaskGetTickCount();
            }
            else
            {
                g_mic_requeue_error = err;
                g_mic_requeue_failed = true;
            }
        }
    }

    if (g_mic_requeue_failed && g_mic_running)
    {
        fsp_err_t err = g_mic_requeue_error;

        g_mic_requeue_failed = false;
        p_mic->error_count++;
        mic_publish_status(MIC_IPC_STATUS_I2S_ERROR, err);
        mic_i2s_force_recover();
    }

    if (g_mic_idle && g_mic_running)
    {
        g_mic_idle = false;
        p_mic->error_count++;
        mic_publish_status(MIC_IPC_STATUS_I2S_ERROR, FSP_ERR_ABORTED);
        mic_i2s_force_recover();
    }

    if (g_mic_running && g_mic_read_in_progress)
    {
        TickType_t now = xTaskGetTickCount();
        if ((now - g_mic_last_read_queue_tick) > pdMS_TO_TICKS(500))
        {
            p_mic->timeout_count++;
            g_mic_last_read_queue_tick = now;
            mic_i2s_force_recover();
            (void) mic_capture_start();
        }
    }

    if (g_mic_recovery_pending && !g_mic_running)
    {
        TickType_t now = xTaskGetTickCount();
        if ((now - g_mic_last_recovery_tick) >= pdMS_TO_TICKS(MIC_RECOVERY_RETRY_MS))
        {
            g_mic_last_recovery_tick = now;
            if (FSP_SUCCESS != mic_capture_start())
            {
                g_mic_recovery_pending = true;
            }
        }
    }
}

void mic_i2s_callback(i2s_callback_args_t * p_args)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (NULL == p_args)
    {
        return;
    }

    MIC_IPC_MAILBOX->callback_count++;
    MIC_IPC_MAILBOX->last_event = (uint32_t) p_args->event;

    if (I2S_EVENT_RX_FULL == p_args->event)
    {
        if (g_mic_running && g_mic_read_in_progress)
        {
            uint32_t completed_write;
            uint32_t pending;

            g_mic_read_in_progress = false;
            completed_write = g_mic_capture_completed_write + 1U;
            __DMB();
            g_mic_capture_completed_write = completed_write;
            pending = completed_write - g_mic_capture_completed_read;

            /*
             * R_SSI_Read() may be called after I2S_EVENT_RX_FULL. Re-arm the
             * next DTC destination before leaving the ISR so SSI reception
             * never waits for filtering, cry preprocessing, IPC publication,
             * or SD-card work. The completed buffer remains owned by the
             * audio task until g_mic_capture_completed_read advances.
             */
            if (pending < MIC_CAPTURE_BUFFER_COUNT)
            {
                fsp_err_t err = mic_capture_submit_read(completed_write % MIC_CAPTURE_BUFFER_COUNT);
                if (FSP_SUCCESS == err)
                {
                    g_mic_last_read_queue_tick = xTaskGetTickCountFromISR();
                }
                else
                {
                    g_mic_requeue_error = err;
                    g_mic_requeue_failed = true;
                }
            }
            else
            {
                /*
                 * All ring buffers contain completed data. Pause reception
                 * without discarding them; the audio task frees one buffer,
                 * re-arms DTC, and then drains the remaining backlog.
                 */
                g_mic_ring_paused = true;
            }
        }
    }
    else if (I2S_EVENT_IDLE == p_args->event)
    {
        MIC_IPC_MAILBOX->idle_count++;
        /*
         * Some SSI idle notifications can arrive while the next DTC read is
         * already armed. They are informational in that state and must not
         * reset an in-flight transfer. Request recovery only when there is
         * neither an active read nor a completed buffer waiting for the task;
         * the 500 ms watchdog handles error paths that leave stale state.
         */
        if (g_mic_running &&
            !g_mic_read_in_progress &&
            (g_mic_capture_completed_read == g_mic_capture_completed_write))
        {
            g_mic_idle = true;
        }
    }

    if (NULL != audio_infer_thread)
    {
        vTaskNotifyGiveFromISR(audio_infer_thread, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

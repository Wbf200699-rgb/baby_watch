#include "audio_infer_thread.h"
#include "thread_code/audio_cry_inference.h"
#include "thread_code/mic_capture_driver.h"

#define MIC_TEST_ENABLE (0U)
#define MIC_TEST_WORDS (8U)
#define MIC_WAIT_LOOPS (200U)
#define MIC_AUTO_START_ENABLE (1U)
#define MIC_AUTO_START_DELAY_MS (3000U)
#define MIC_AUTO_START_RETRY_MS (2000U)

#if MIC_TEST_ENABLE
static int32_t g_mic_rx_words[MIC_TEST_WORDS] BSP_ALIGN_VARIABLE(4);
static int32_t g_mic_tx_silence[MIC_TEST_WORDS] BSP_ALIGN_VARIABLE(4);
#endif

volatile uint32_t g_mic_read_debug;
volatile uint32_t g_mic_auto_start_attempts;
volatile uint32_t g_mic_auto_start_err;
#if MIC_TEST_ENABLE
volatile uint32_t g_mic_open_err;
volatile uint32_t g_mic_read_err;
volatile uint32_t g_mic_stop_err;
volatile uint32_t g_mic_close_err;
volatile uint32_t g_mic_cb_count;
volatile uint32_t g_mic_rx_full_count;
volatile uint32_t g_mic_idle_count;
volatile uint32_t g_mic_last_event;
volatile uint32_t g_mic_peak;
volatile uint32_t g_mic_nonzero;
volatile uint32_t g_mic_ssicr;
volatile uint32_t g_mic_ssisr;
volatile uint32_t g_mic_ssifsr;
volatile uint32_t g_mic_left_running;
volatile int32_t  g_mic_first_sample;
volatile int32_t  g_mic_last_sample;

static uint32_t mic_abs32(int32_t value)
{
    return (value < 0) ? (uint32_t) (-value) : (uint32_t) value;
}

static void mic_analyze_test_block(void)
{
    uint32_t peak    = 0U;
    uint32_t nonzero = 0U;

    for (uint32_t i = 0U; i < MIC_TEST_WORDS; i++)
    {
        uint32_t magnitude = mic_abs32(g_mic_rx_words[i]);
        if (magnitude > peak)
        {
            peak = magnitude;
        }

        if (0 != g_mic_rx_words[i])
        {
            nonzero++;
        }
    }

    g_mic_peak         = peak;
    g_mic_nonzero      = nonzero;
    g_mic_first_sample = g_mic_rx_words[0];
    g_mic_last_sample  = g_mic_rx_words[MIC_TEST_WORDS - 1U];
}

static void mic_snapshot_regs(void)
{
    g_mic_ssicr  = g_mic_i2s_ctrl.p_reg->SSICR;
    g_mic_ssisr  = g_mic_i2s_ctrl.p_reg->SSISR;
    g_mic_ssifsr = g_mic_i2s_ctrl.p_reg->SSIFSR;
}

void mic_i2s_callback(i2s_callback_args_t *p_args)
{
    g_mic_cb_count++;

    if (NULL == p_args)
    {
        g_mic_last_event = 0xFFFFFFFFU;
        return;
    }

    g_mic_last_event = (uint32_t) p_args->event;

    if (I2S_EVENT_RX_FULL == p_args->event)
    {
        g_mic_rx_full_count++;
    }
    else if (I2S_EVENT_IDLE == p_args->event)
    {
        g_mic_idle_count++;
    }
}
#endif

/* aud_inf entry function */
/* pvParameters contains TaskHandle_t */
void audio_infer_thread_entry(void *pvParameters)
{
#if (!MIC_TEST_ENABLE) && MIC_AUTO_START_ENABLE
    TickType_t auto_start_tick = xTaskGetTickCount() + pdMS_TO_TICKS(MIC_AUTO_START_DELAY_MS);
    bool auto_start_complete = false;
#endif

    FSP_PARAMETER_NOT_USED (pvParameters);
    audio_cry_inference_init();

#if MIC_TEST_ENABLE
    vTaskDelay(pdMS_TO_TICKS(3000));

    g_mic_read_debug = 1U;
    g_mic_open_err = (uint32_t) g_mic_i2s.p_api->open(g_mic_i2s.p_ctrl, g_mic_i2s.p_cfg);
    if (FSP_SUCCESS == (fsp_err_t) g_mic_open_err)
    {
        g_mic_read_debug = 2U;

        for (uint32_t i = 0U; i < MIC_TEST_WORDS; i++)
        {
            g_mic_rx_words[i] = 0;
            g_mic_tx_silence[i] = 0;
        }

        uint32_t rx_full_before = g_mic_rx_full_count;
        g_mic_read_err = (uint32_t) g_mic_i2s.p_api->writeRead(g_mic_i2s.p_ctrl,
                                                               g_mic_tx_silence,
                                                               g_mic_rx_words,
                                                               sizeof(g_mic_rx_words));
        mic_snapshot_regs();

        if (FSP_SUCCESS == (fsp_err_t) g_mic_read_err)
        {
            g_mic_read_debug = 3U;

            for (uint32_t i = 0U; i < MIC_WAIT_LOOPS; i++)
            {
                if (g_mic_rx_full_count != rx_full_before)
                {
                    mic_analyze_test_block();
                    mic_snapshot_regs();
                    g_mic_read_debug = 4U;
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (3U == g_mic_read_debug)
            {
                mic_snapshot_regs();
                g_mic_read_debug = 6U;
            }

            if (4U == g_mic_read_debug)
            {
                g_mic_stop_err = (uint32_t) g_mic_i2s.p_api->stop(g_mic_i2s.p_ctrl);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            else
            {
                g_mic_left_running = 1U;
            }
        }
        else
        {
            g_mic_read_debug = 5U;
        }

        if (0U == g_mic_left_running)
        {
            g_mic_close_err = (uint32_t) g_mic_i2s.p_api->close(g_mic_i2s.p_ctrl);
        }
    }
    else
    {
        g_mic_read_debug = 7U;
    }
#else
    g_mic_read_debug = 0x100U;
#endif

    while (1)
    {
        mic_capture_poll();
#if (!MIC_TEST_ENABLE) && MIC_AUTO_START_ENABLE
        if (!auto_start_complete && ((int32_t) (xTaskGetTickCount() - auto_start_tick) >= 0))
        {
            fsp_err_t err;

            g_mic_auto_start_attempts++;
            err = mic_capture_start();
            g_mic_auto_start_err = (uint32_t) err;
            if (FSP_SUCCESS == err)
            {
                auto_start_complete = true;
            }
            else
            {
                auto_start_tick = xTaskGetTickCount() + pdMS_TO_TICKS(MIC_AUTO_START_RETRY_MS);
            }
        }
#endif
        (void) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
    }
}

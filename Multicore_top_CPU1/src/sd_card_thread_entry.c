#include "sd_card_thread.h"

#include "thread_code/sd_card_driver.h"
#include "thread_code/system_logger.h"
#include "thread_code/audio_wav_player.h"

/* sd entry function */
/* pvParameters contains TaskHandle_t */
void sd_card_thread_entry(void *pvParameters)
{
    fsp_err_t init_err;
    sd_card_driver_status_t write_status;
    sd_card_info_t sd_info;
    uint32_t poll_delay_ms;

    FSP_PARAMETER_NOT_USED (pvParameters);

    /* Clear a partially open SDHI/FAT stack left by a DC brownout or a warm
     * core restart before any file-system user can observe ready=true. */
    sd_card_driver_prepare();

    /* Let VIN_5V/VSYS_3V3 and the removable card finish ramping before SDHI
     * starts issuing commands.  The board can be powered from either the
     * debug USB path or the external 5 V path, which have different ramps. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    while (1)
    {
        poll_delay_ms = 1000U;

        if (!sd_card_driver_fs_lock(portMAX_DELAY))
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* The generated-story receiver is a stop-and-wait stream. Opening,
         * appending and closing the CSV log while it is active can hold the
         * shared FAT lock longer than the gateway ACK timeout. Defer periodic
         * status refresh and logging until the transfer completes. */
        if (audio_wav_story_writer_active())
        {
            sd_card_driver_fs_unlock();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!sd_card_driver_is_ready())
        {
            system_logger_reset();
            init_err = sd_card_driver_init();
            g_sd_card_debug.init_err = init_err;
            if (FSP_SUCCESS == init_err)
            {
                write_status = sd_card_driver_write_test_file();
                g_sd_card_debug.write_status = write_status;
                if (SD_CARD_DRIVER_STATUS_OK == write_status)
                {
                    sd_card_driver_get_info(&sd_info);
                    system_logger_init(&sd_info);
                }
            }
            else if (g_sd_card_debug.consecutive_failures >= 12U)
            {
                poll_delay_ms = 30000U;
            }
            else if (g_sd_card_debug.consecutive_failures >= 3U)
            {
                poll_delay_ms = 5000U;
            }
        }
        else
        {
            (void) sd_card_driver_refresh_status();
        }

        if (sd_card_driver_is_ready())
        {
            sd_card_driver_get_info(&sd_info);
            system_logger_poll(&sd_info);
        }

        sd_card_driver_fs_unlock();

        vTaskDelay(pdMS_TO_TICKS(poll_delay_ms));
    }
}

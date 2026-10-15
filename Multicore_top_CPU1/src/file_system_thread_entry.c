#include "file_system_thread.h"

#define MIC_RECORD_WRITER_ENABLE (1U)
#define FILE_SYSTEM_NORMAL_PRIORITY (3U)
#define FILE_SYSTEM_STORY_PRIORITY  (configMAX_PRIORITIES - 4U)

#if MIC_RECORD_WRITER_ENABLE
#include "thread_code/mic_record_writer.h"
#include "thread_code/audio_wav_player.h"
#include "thread_code/serial_camera_writer.h"
#include "thread_code/sd_card_driver.h"
#include "thread_code/story_bulk_uart.h"
#endif

/* fs entry function */
/* pvParameters contains TaskHandle_t */
void file_system_thread_entry(void *pvParameters)
{
    bool story_priority_active = false;

    FSP_PARAMETER_NOT_USED (pvParameters);

#if MIC_RECORD_WRITER_ENABLE
    story_bulk_uart_init();
#endif

    while (1)
    {
#if MIC_RECORD_WRITER_ENABLE
        if (sd_card_driver_fs_lock(portMAX_DELAY))
        {
            bool const sd_ready = sd_card_driver_is_ready();
            bool story_active;

            story_bulk_uart_poll(sd_ready);

            /* Story chunks are synchronous: P4 cannot send the next 448-byte
             * chunk until this poll writes the current chunk and CPU1 ACKs it.
             * Service that path first and temporarily outrank normal SD/log
             * work so a generated story does not take several minutes. */
            audio_wav_story_writer_poll(sd_ready);
            story_active = story_bulk_uart_active() || audio_wav_story_writer_active();
            if (story_active != story_priority_active)
            {
                vTaskPrioritySet(NULL, story_active ? FILE_SYSTEM_STORY_PRIORITY :
                                                    FILE_SYSTEM_NORMAL_PRIORITY);
                story_priority_active = story_active;
            }

            if (!story_active)
            {
                sd_card_driver_debug_export_poll(sd_ready);
            }
            mic_record_writer_poll(sd_ready);
            serial_camera_writer_poll(sd_ready);
            sd_card_driver_fs_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
#else
        vTaskDelay(pdMS_TO_TICKS(100));
#endif
    }
}

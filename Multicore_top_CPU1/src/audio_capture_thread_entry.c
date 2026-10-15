#include "audio_capture_thread.h"
#include "thread_code/mic_record_writer.h"
/* aud_cap entry function */
/* pvParameters contains TaskHandle_t */
void audio_capture_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        mic_record_capture_poll();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

#include "speaker_output_thread.h"
/* spk entry function */
/* pvParameters contains TaskHandle_t */
void speaker_output_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

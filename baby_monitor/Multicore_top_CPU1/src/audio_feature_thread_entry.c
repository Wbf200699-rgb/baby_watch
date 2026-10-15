#include "audio_feature_thread.h"
/* aud_feat entry function */
/* pvParameters contains TaskHandle_t */
void audio_feature_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

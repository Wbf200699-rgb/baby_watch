#include "playback_control_thread.h"
/* play_ctrl entry function */
/* pvParameters contains TaskHandle_t */
void playback_control_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

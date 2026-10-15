#include "telemetry_upload_thread.h"
/* telemetry entry function */
/* pvParameters contains TaskHandle_t */
void telemetry_upload_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

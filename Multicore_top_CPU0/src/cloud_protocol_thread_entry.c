#include "cloud_protocol_thread.h"
/* cloud entry function */
/* pvParameters contains TaskHandle_t */
void cloud_protocol_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

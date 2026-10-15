#include "command_dispatch_thread.h"
/* cmd entry function */
/* pvParameters contains TaskHandle_t */
void command_dispatch_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

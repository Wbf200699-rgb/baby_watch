#include "file_update_thread.h"
/* update entry function */
/* pvParameters contains TaskHandle_t */
void file_update_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

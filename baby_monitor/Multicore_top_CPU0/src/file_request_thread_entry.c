#include "file_request_thread.h"
/* file_req entry function */
/* pvParameters contains TaskHandle_t */
void file_request_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

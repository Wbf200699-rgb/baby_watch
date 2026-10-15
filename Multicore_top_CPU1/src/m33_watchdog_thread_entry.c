#include "m33_watchdog_thread.h"
/* wd_m33 entry function */
/* pvParameters contains TaskHandle_t */
void m33_watchdog_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

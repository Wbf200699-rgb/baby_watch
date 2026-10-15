#include "m33_ipc_rx_thread.h"
/* ipc_rx entry function */
/* pvParameters contains TaskHandle_t */
void m33_ipc_rx_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

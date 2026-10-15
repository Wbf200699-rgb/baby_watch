#include "m33_ipc_tx_thread.h"
/* ipc_tx entry function */
/* pvParameters contains TaskHandle_t */
void m33_ipc_tx_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    while (1)
    {
        vTaskSuspend(NULL);
    }
}

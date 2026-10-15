#include "scd41_thread.h"
#include "thread_code/bme688_driver.h"
#include "thread_code/scd41_driver.h"

/* scd41 entry function */
/* pvParameters contains TaskHandle_t */
void scd41_thread_entry(void * pvParameters)
{
    fsp_err_t err;

    FSP_PARAMETER_NOT_USED(pvParameters);

    err = scd41_driver_init();
    (void) bme688_driver_init();

    while (1)
    {
        err = scd41_driver_poll();
        (void) bme688_driver_poll();
        vTaskDelay(pdMS_TO_TICKS((FSP_SUCCESS == err) ? 1000U : 5000U));
    }
}

#include "pressure_thread.h"
#include "thread_code/pressure_matrix_driver.h"

#define PRESSURE_SCAN_ENABLED (1U)

/* pressure entry function */
/* pvParameters contains TaskHandle_t */
void pressure_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

#if PRESSURE_SCAN_ENABLED
    vTaskDelay(pdMS_TO_TICKS(5000));
    (void) pressure_matrix_init();

    while (1)
    {
        (void) pressure_matrix_scan();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#else
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}

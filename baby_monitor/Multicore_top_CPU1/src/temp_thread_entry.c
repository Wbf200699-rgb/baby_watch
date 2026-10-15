#include "temp_thread.h"
#include "thread_code/tmp117_matrix_driver.h"

/* temp entry function */
/* pvParameters contains TaskHandle_t */
void temp_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    (void) tmp117_matrix_init();

    while (1)
    {
        (void) tmp117_matrix_scan();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

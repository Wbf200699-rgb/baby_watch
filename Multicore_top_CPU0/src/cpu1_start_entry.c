#include "cpu1_start.h"
/* cpu1_start entry function */
/* pvParameters contains TaskHandle_t */
void cpu1_start_entry(void *pvParameters) {
	FSP_PARAMETER_NOT_USED(pvParameters);

	R_BSP_SecondaryCoreStart();
	vTaskDelete(NULL);
}

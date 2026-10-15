/* generated thread header file - do not edit */
#ifndef CPU1_START_H_
#define CPU1_START_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void cpu1_start_entry(void * pvParameters);
                #else
                extern void cpu1_start_entry(void * pvParameters);
                #endif
FSP_HEADER
FSP_FOOTER
#endif /* CPU1_START_H_ */

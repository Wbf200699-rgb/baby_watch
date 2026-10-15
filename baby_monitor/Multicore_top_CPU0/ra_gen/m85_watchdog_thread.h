/* generated thread header file - do not edit */
#ifndef M85_WATCHDOG_THREAD_H_
#define M85_WATCHDOG_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void m85_watchdog_thread_entry(void * pvParameters);
                #else
                extern void m85_watchdog_thread_entry(void * pvParameters);
                #endif
FSP_HEADER
FSP_FOOTER
#endif /* M85_WATCHDOG_THREAD_H_ */

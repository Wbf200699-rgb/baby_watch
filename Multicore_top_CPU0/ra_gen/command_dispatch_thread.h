/* generated thread header file - do not edit */
#ifndef COMMAND_DISPATCH_THREAD_H_
#define COMMAND_DISPATCH_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void command_dispatch_thread_entry(void * pvParameters);
                #else
                extern void command_dispatch_thread_entry(void * pvParameters);
                #endif
FSP_HEADER
FSP_FOOTER
#endif /* COMMAND_DISPATCH_THREAD_H_ */

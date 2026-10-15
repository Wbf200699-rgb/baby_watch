/* generated thread header file - do not edit */
#ifndef CLOUD_PROTOCOL_THREAD_H_
#define CLOUD_PROTOCOL_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void cloud_protocol_thread_entry(void * pvParameters);
                #else
                extern void cloud_protocol_thread_entry(void * pvParameters);
                #endif
FSP_HEADER
FSP_FOOTER
#endif /* CLOUD_PROTOCOL_THREAD_H_ */

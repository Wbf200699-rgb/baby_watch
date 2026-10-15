/* generated thread header file - do not edit */
#ifndef FILE_REQUEST_THREAD_H_
#define FILE_REQUEST_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void file_request_thread_entry(void * pvParameters);
                #else
                extern void file_request_thread_entry(void * pvParameters);
                #endif
FSP_HEADER
FSP_FOOTER
#endif /* FILE_REQUEST_THREAD_H_ */

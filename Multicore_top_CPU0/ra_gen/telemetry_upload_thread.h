/* generated thread header file - do not edit */
#ifndef TELEMETRY_UPLOAD_THREAD_H_
#define TELEMETRY_UPLOAD_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void telemetry_upload_thread_entry(void * pvParameters);
                #else
                extern void telemetry_upload_thread_entry(void * pvParameters);
                #endif
FSP_HEADER
FSP_FOOTER
#endif /* TELEMETRY_UPLOAD_THREAD_H_ */

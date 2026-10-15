/* generated thread header file - do not edit */
#ifndef SPEAKER_OUTPUT_THREAD_H_
#define SPEAKER_OUTPUT_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void speaker_output_thread_entry(void * pvParameters);
                #else
                extern void speaker_output_thread_entry(void * pvParameters);
                #endif
FSP_HEADER
FSP_FOOTER
#endif /* SPEAKER_OUTPUT_THREAD_H_ */

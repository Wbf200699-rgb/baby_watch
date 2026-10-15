/* generated thread header file - do not edit */
#ifndef AUDIO_CAPTURE_THREAD_H_
#define AUDIO_CAPTURE_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void audio_capture_thread_entry(void * pvParameters);
                #else
                extern void audio_capture_thread_entry(void * pvParameters);
                #endif
FSP_HEADER
FSP_FOOTER
#endif /* AUDIO_CAPTURE_THREAD_H_ */

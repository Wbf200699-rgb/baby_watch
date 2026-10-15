/* generated thread header file - do not edit */
#ifndef PLAYBACK_CONTROL_THREAD_H_
#define PLAYBACK_CONTROL_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void playback_control_thread_entry(void * pvParameters);
                #else
                extern void playback_control_thread_entry(void * pvParameters);
                #endif
FSP_HEADER
FSP_FOOTER
#endif /* PLAYBACK_CONTROL_THREAD_H_ */

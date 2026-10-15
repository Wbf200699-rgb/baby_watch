/* generated thread header file - do not edit */
#ifndef SD_CARD_THREAD_H_
#define SD_CARD_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void sd_card_thread_entry(void * pvParameters);
                #else
                extern void sd_card_thread_entry(void * pvParameters);
                #endif
FSP_HEADER
FSP_FOOTER
#endif /* SD_CARD_THREAD_H_ */

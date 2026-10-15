/* generated thread header file - do not edit */
#ifndef FILE_SYSTEM_THREAD_H_
#define FILE_SYSTEM_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void file_system_thread_entry(void * pvParameters);
                #else
                extern void file_system_thread_entry(void * pvParameters);
                #endif
#include "r_dtc.h"
#include "r_transfer_api.h"
#include "r_sci_b_uart.h"
            #include "r_uart_api.h"
FSP_HEADER
/* Transfer on DTC Instance. */
extern const transfer_instance_t g_story_bulk_transfer;

/** Access the DTC instance using these structures when calling API functions directly (::p_api is not used). */
extern dtc_instance_ctrl_t g_story_bulk_transfer_ctrl;
extern const transfer_cfg_t g_story_bulk_transfer_cfg;
/** UART on SCI Instance. */
            extern const uart_instance_t      g_story_bulk_uart;

            /** Access the UART instance using these structures when calling API functions directly (::p_api is not used). */
            extern sci_b_uart_instance_ctrl_t     g_story_bulk_uart_ctrl;
            extern const uart_cfg_t g_story_bulk_uart_cfg;
            extern const sci_b_uart_extended_cfg_t g_story_bulk_uart_cfg_extend;

            #ifndef story_bulk_uart_callback
            void story_bulk_uart_callback(uart_callback_args_t * p_args);
            #endif
FSP_FOOTER
#endif /* FILE_SYSTEM_THREAD_H_ */

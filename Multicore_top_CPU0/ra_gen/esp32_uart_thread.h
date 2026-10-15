/* generated thread header file - do not edit */
#ifndef ESP32_UART_THREAD_H_
#define ESP32_UART_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void esp32_uart_thread_entry(void * pvParameters);
                #else
                extern void esp32_uart_thread_entry(void * pvParameters);
                #endif
#include "r_gpt.h"
#include "r_timer_api.h"
#include "r_sci_b_uart.h"
            #include "r_uart_api.h"
FSP_HEADER
/** Timer on GPT Instance. */
extern const timer_instance_t g_mic_audio_clk_timer;

/** Access the GPT instance using these structures when calling API functions directly (::p_api is not used). */
extern gpt_instance_ctrl_t g_mic_audio_clk_timer_ctrl;
extern const timer_cfg_t g_mic_audio_clk_timer_cfg;

#ifndef NULL
void NULL(timer_callback_args_t * p_args);
#endif
/** UART on SCI Instance. */
            extern const uart_instance_t      g_esp32_uart;

            /** Access the UART instance using these structures when calling API functions directly (::p_api is not used). */
            extern sci_b_uart_instance_ctrl_t     g_esp32_uart_ctrl;
            extern const uart_cfg_t g_esp32_uart_cfg;
            extern const sci_b_uart_extended_cfg_t g_esp32_uart_cfg_extend;

            #ifndef esp32_uart_callback
            void esp32_uart_callback(uart_callback_args_t * p_args);
            #endif
FSP_FOOTER
#endif /* ESP32_UART_THREAD_H_ */

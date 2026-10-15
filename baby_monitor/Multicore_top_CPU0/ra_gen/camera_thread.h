/* generated thread header file - do not edit */
#ifndef CAMERA_THREAD_H_
#define CAMERA_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void camera_thread_entry(void * pvParameters);
                #else
                extern void camera_thread_entry(void * pvParameters);
                #endif
#include "r_sci_b_uart.h"
            #include "r_uart_api.h"
#include "r_usb_basic.h"
#include "r_usb_basic_api.h"
FSP_HEADER
/** UART on SCI Instance. */
            extern const uart_instance_t      g_camera_uart;

            /** Access the UART instance using these structures when calling API functions directly (::p_api is not used). */
            extern sci_b_uart_instance_ctrl_t     g_camera_uart_ctrl;
            extern const uart_cfg_t g_camera_uart_cfg;
            extern const sci_b_uart_extended_cfg_t g_camera_uart_cfg_extend;

            #ifndef camera_uart_callback
            void camera_uart_callback(uart_callback_args_t * p_args);
            #endif
/* Basic on USB Instance. */
extern const usb_instance_t g_basic0;

/** Access the USB instance using these structures when calling API functions directly (::p_api is not used). */
extern usb_instance_ctrl_t g_basic0_ctrl;
extern const usb_cfg_t g_basic0_cfg;

#ifndef NULL
void NULL(void *);
#endif

#if 0 == BSP_CFG_RTOS
#ifndef usb_basic_callback
void usb_basic_callback(usb_callback_args_t *);
#endif
#endif

#if 2 == BSP_CFG_RTOS
#ifndef usb_basic_callback
void usb_basic_callback(usb_event_info_t *, usb_hdl_t, usb_onoff_t);
#endif
#endif
FSP_FOOTER
#endif /* CAMERA_THREAD_H_ */

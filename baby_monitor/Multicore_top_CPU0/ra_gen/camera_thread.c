/* generated thread source file - do not edit */
#include "camera_thread.h"

#if 1
                static StaticTask_t camera_thread_memory;
                #if defined(__ARMCC_VERSION)           /* AC6 compiler */
                static uint8_t camera_thread_stack[32768] BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".stack.thread") BSP_ALIGN_VARIABLE(BSP_STACK_ALIGNMENT);
                #else
                static uint8_t camera_thread_stack[32768] BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".stack.camera_thread") BSP_ALIGN_VARIABLE(BSP_STACK_ALIGNMENT);
                #endif
                #endif
                TaskHandle_t camera_thread;
                void camera_thread_create(void);
                static void camera_thread_func(void * pvParameters);
                void rtos_startup_err_callback(void * p_instance, void * p_data);
                void rtos_startup_common_init(void);
sci_b_uart_instance_ctrl_t     g_camera_uart_ctrl;

            sci_b_baud_setting_t               g_camera_uart_baud_setting =
            {
                /* Baud rate calculated with 0.469% error. */ .baudrate_bits_b.abcse = 0, .baudrate_bits_b.abcs = 0, .baudrate_bits_b.bgdm = 1, .baudrate_bits_b.cks = 0, .baudrate_bits_b.brr = 134, .baudrate_bits_b.mddr = (uint8_t) 256, .baudrate_bits_b.brme = false
            };

            /** UART extended configuration for UARTonSCI HAL driver */
            const sci_b_uart_extended_cfg_t g_camera_uart_cfg_extend =
            {
                .clock                = SCI_B_UART_CLOCK_INT,
                .rx_edge_start          = SCI_B_UART_START_BIT_FALLING_EDGE,
                .noise_cancel         = SCI_B_UART_NOISE_CANCELLATION_DISABLE,
                .rx_fifo_trigger        = SCI_B_UART_RX_FIFO_TRIGGER_1,
                .p_baud_setting         = &g_camera_uart_baud_setting,
                .flow_control           = SCI_B_UART_FLOW_CONTROL_RTS,
                #if 0xFF != 0xFF
                .flow_control_pin       = BSP_IO_PORT_FF_PIN_0xFF,
                #else
                .flow_control_pin       = (bsp_io_port_pin_t) UINT16_MAX,
                #endif
                .rs485_setting          = {
                    .enable = SCI_B_UART_RS485_DISABLE,
                    .polarity = SCI_B_UART_RS485_DE_POLARITY_HIGH,
                    .assertion_time = 1,
                    .negation_time = 1,
                },
                .delay_cycles = 0,
            };

            /** UART interface configuration */
            const uart_cfg_t g_camera_uart_cfg =
            {
                .channel             = 3,
                .data_bits           = UART_DATA_BITS_8,
                .parity              = UART_PARITY_OFF,
                .stop_bits           = UART_STOP_BITS_1,
                .p_callback          = camera_uart_callback,
                .p_context           = NULL,
                .p_extend            = &g_camera_uart_cfg_extend,
#define RA_NOT_DEFINED (1)
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
                .p_transfer_tx       = NULL,
#else
                .p_transfer_tx       = &RA_NOT_DEFINED,
#endif
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
                .p_transfer_rx       = NULL,
#else
                .p_transfer_rx       = &RA_NOT_DEFINED,
#endif
#undef RA_NOT_DEFINED
                .rxi_ipl             = (12),
                .txi_ipl             = (12),
                .tei_ipl             = (12),
                .eri_ipl             = (12),
#if defined(VECTOR_NUMBER_SCI3_RXI)
                .rxi_irq             = VECTOR_NUMBER_SCI3_RXI,
#else
                .rxi_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SCI3_TXI)
                .txi_irq             = VECTOR_NUMBER_SCI3_TXI,
#else
                .txi_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SCI3_TEI)
                .tei_irq             = VECTOR_NUMBER_SCI3_TEI,
#else
                .tei_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SCI3_ERI)
                .eri_irq             = VECTOR_NUMBER_SCI3_ERI,
#else
                .eri_irq             = FSP_INVALID_VECTOR,
#endif
            };

/* Instance structure to use this module. */
const uart_instance_t g_camera_uart =
{
    .p_ctrl        = &g_camera_uart_ctrl,
    .p_cfg         = &g_camera_uart_cfg,
    .p_api         = &g_uart_on_sci_b
};
usb_instance_ctrl_t g_basic0_ctrl;

#if !defined(NULL)
extern usb_descriptor_t NULL;
#endif
#define RA_NOT_DEFINED (1)
            const usb_cfg_t g_basic0_cfg =
            {
                .usb_mode  = USB_MODE_HOST,
                .usb_speed = USB_SPEED_HS,
                .module_number = 1,
                .type = USB_CLASS_HVND,
#if defined(NULL)
                .p_usb_reg = NULL,
#else
                .p_usb_reg = &NULL,
#endif
                .usb_complience_cb = NULL,
#if defined(VECTOR_NUMBER_USBFS_INT)
                .irq       = VECTOR_NUMBER_USBFS_INT,
#else
                .irq       = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_USBFS_RESUME)
                .irq_r     = VECTOR_NUMBER_USBFS_RESUME,
#else
                .irq_r     = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_USBFS_FIFO_0)
                .irq_d0    = VECTOR_NUMBER_USBFS_FIFO_0,
#else
                .irq_d0    = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_USBFS_FIFO_1)
                .irq_d1    = VECTOR_NUMBER_USBFS_FIFO_1,
#else
                .irq_d1    = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_USBHS_USB_INT_RESUME)
                .hsirq     = VECTOR_NUMBER_USBHS_USB_INT_RESUME,
#else
                .hsirq     = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_USBHS_FIFO_0)
                .hsirq_d0  = VECTOR_NUMBER_USBHS_FIFO_0,
#else
                .hsirq_d0  = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_USBHS_FIFO_1)
                .hsirq_d1  = VECTOR_NUMBER_USBHS_FIFO_1,
#else
                .hsirq_d1  = FSP_INVALID_VECTOR,
#endif
                .ipl       = (12),
                .ipl_r     = (12),
                .ipl_d0    = (12),
                .ipl_d1    = (12),
                .hsipl     = (12),
                .hsipl_d0  = (12),
                .hsipl_d1  = (12),
#if (BSP_CFG_RTOS == 0) && defined(USB_CFG_HMSC_USE)
                .p_usb_apl_callback = NULL,
#else
                .p_usb_apl_callback = usb_basic_callback,
#endif
#if defined(NULL)
                .p_context = NULL,
#else
                .p_context = (void *) &NULL,
#endif
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
#else
                .p_transfer_tx = &RA_NOT_DEFINED,
#endif
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
#else
                .p_transfer_rx = &RA_NOT_DEFINED,
#endif
            };
#undef RA_NOT_DEFINED

/* Instance structure to use this module. */
const usb_instance_t g_basic0 =
{
    .p_ctrl        = &g_basic0_ctrl,
    .p_cfg         = &g_basic0_cfg,
    .p_api         = &g_usb_on_usb,
};
extern uint32_t g_fsp_common_thread_count;

                const rm_freertos_port_parameters_t camera_thread_parameters =
                {
                    .p_context = (void *) NULL,
                };

                void camera_thread_create (void)
                {
                    /* Increment count so we will know the number of threads created in the RA Configuration editor. */
                    g_fsp_common_thread_count++;

                    /* Initialize each kernel object. */
                    

                    #if 1
                    camera_thread = xTaskCreateStatic(
                    #else
                    BaseType_t camera_thread_create_err = xTaskCreate(
                    #endif
                        camera_thread_func,
                        (const char *)"camera",
                        32768/4, // In words, not bytes
                        (void *) &camera_thread_parameters, //pvParameters
                        6,
                        #if 1
                        (StackType_t *)&camera_thread_stack,
                        (StaticTask_t *)&camera_thread_memory
                        #else
                        & camera_thread
                        #endif
                    );

                    #if 1
                    if (NULL == camera_thread)
                    {
                        rtos_startup_err_callback(camera_thread, 0);
                    }
                    #else
                    if (pdPASS != camera_thread_create_err)
                    {
                        rtos_startup_err_callback(camera_thread, 0);
                    }
                    #endif
                }
                static void camera_thread_func (void * pvParameters)
                {
                    /* Initialize common components */
                    rtos_startup_common_init();

                    /* Initialize each module instance. */
                    

                    #if (1 == BSP_TZ_NONSECURE_BUILD) && (1 == 1)
                    /* When FreeRTOS is used in a non-secure TrustZone application, portALLOCATE_SECURE_CONTEXT must be called prior
                     * to calling any non-secure callable function in a thread. The parameter is unused in the FSP implementation.
                     * If no slots are available then configASSERT() will be called from vPortSVCHandler_C(). If this occurs, the
                     * application will need to either increase the value of the "Process Stack Slots" Property in the rm_tz_context
                     * module in the secure project or decrease the number of threads in the non-secure project that are allocating
                     * a secure context. Users can control which threads allocate a secure context via the Properties tab when
                     * selecting each thread. Note that the idle thread in FreeRTOS requires a secure context so the application
                     * will need at least 1 secure context even if no user threads make secure calls. */
                     portALLOCATE_SECURE_CONTEXT(0);
                    #endif

                    /* Enter user code for this thread. Pass task handle. */
                    camera_thread_entry(pvParameters);
                }

/* generated thread source file - do not edit */
#include "audio_infer_thread.h"

#if 1
                static StaticTask_t audio_infer_thread_memory;
                #if defined(__ARMCC_VERSION)           /* AC6 compiler */
                static uint8_t audio_infer_thread_stack[12288] BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".stack.thread") BSP_ALIGN_VARIABLE(BSP_STACK_ALIGNMENT);
                #else
                static uint8_t audio_infer_thread_stack[12288] BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".stack.audio_infer_thread") BSP_ALIGN_VARIABLE(BSP_STACK_ALIGNMENT);
                #endif
                #endif
                TaskHandle_t audio_infer_thread;
                void audio_infer_thread_create(void);
                static void audio_infer_thread_func(void * pvParameters);
                void rtos_startup_err_callback(void * p_instance, void * p_data);
                void rtos_startup_common_init(void);
dtc_instance_ctrl_t g_mic_i2s_rx_transfer_ctrl;

#if (1 == 1)
transfer_info_t g_mic_i2s_rx_transfer_info DTC_TRANSFER_INFO_ALIGNMENT =
{
    .transfer_settings_word_b.dest_addr_mode = TRANSFER_ADDR_MODE_INCREMENTED,
    .transfer_settings_word_b.repeat_area    = TRANSFER_REPEAT_AREA_DESTINATION,
    .transfer_settings_word_b.irq            = TRANSFER_IRQ_END,
    .transfer_settings_word_b.chain_mode     = TRANSFER_CHAIN_MODE_DISABLED,
    .transfer_settings_word_b.src_addr_mode  = TRANSFER_ADDR_MODE_FIXED,
    .transfer_settings_word_b.size           = TRANSFER_SIZE_4_BYTE,
    .transfer_settings_word_b.mode           = TRANSFER_MODE_BLOCK,
    .p_dest                                  = (void *) NULL,
    .p_src                                   = (void const *) NULL,
    .num_blocks                              = (uint16_t) 0,
    .length                                  = (uint16_t) 0,
};

#elif (1 > 1)
/* User is responsible to initialize the array. */
transfer_info_t g_mic_i2s_rx_transfer_info[1] DTC_TRANSFER_INFO_ALIGNMENT;
#else
/* User must call api::reconfigure before enable DTC transfer. */
#endif

const dtc_extended_cfg_t g_mic_i2s_rx_transfer_cfg_extend =
{
    .activation_source   = VECTOR_NUMBER_SSI1_RXI,
};

const transfer_cfg_t g_mic_i2s_rx_transfer_cfg =
{
#if (1 == 1)
    .p_info              = &g_mic_i2s_rx_transfer_info,
#elif (1 > 1)
    .p_info              = g_mic_i2s_rx_transfer_info,
#else
    .p_info = NULL,
#endif
    .p_extend            = &g_mic_i2s_rx_transfer_cfg_extend,
};

/* Instance structure to use this module. */
const transfer_instance_t g_mic_i2s_rx_transfer =
{
    .p_ctrl        = &g_mic_i2s_rx_transfer_ctrl,
    .p_cfg         = &g_mic_i2s_rx_transfer_cfg,
    .p_api         = &g_transfer_on_dtc
};
ssi_instance_ctrl_t     g_mic_i2s_ctrl;

            /** SSI instance configuration */
            const ssi_extended_cfg_t g_mic_i2s_cfg_extend =
            {
                .audio_clock         = (ssi_audio_clock_t) SSI_AUDIO_CLOCK_INTERNAL,
                .bit_clock_div       = SSI_CLOCK_DIV_1,
            };

            /** I2S interface configuration */
            const i2s_cfg_t g_mic_i2s_cfg =
            {
                .channel             = 1,
                .pcm_width           = I2S_PCM_WIDTH_24_BITS,
                .operating_mode      = I2S_MODE_MASTER,
                .word_length         = I2S_WORD_LENGTH_32_BITS,
                .ws_continue         = I2S_WS_CONTINUE_ON,
                .p_callback          = mic_i2s_callback,
                .p_context           = NULL,
                .p_extend            = &g_mic_i2s_cfg_extend,
#if (BSP_IRQ_DISABLED) != BSP_IRQ_DISABLED
                .txi_irq                 = VECTOR_NUMBER_SSI1_TXI,
#else
                .txi_irq                 = FSP_INVALID_VECTOR,
#endif
#if (12) != BSP_IRQ_DISABLED
                .rxi_irq                 = VECTOR_NUMBER_SSI1_RXI,
#else
                .rxi_irq                 = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SSI1_INT)
                .int_irq                 = VECTOR_NUMBER_SSI1_INT,
#else
                .int_irq                 = FSP_INVALID_VECTOR,
#endif
                .txi_ipl             = (BSP_IRQ_DISABLED),
                .rxi_ipl             = (12),
                .idle_err_ipl        = (12),
#define RA_NOT_DEFINED (1)
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
                .p_transfer_tx       = NULL,
#else
                .p_transfer_tx       = &RA_NOT_DEFINED,
#endif
#if (RA_NOT_DEFINED == g_mic_i2s_rx_transfer)
                .p_transfer_rx       = NULL,
#else
                .p_transfer_rx       = &g_mic_i2s_rx_transfer,
#endif
#undef RA_NOT_DEFINED
            };

/* Instance structure to use this module. */
const i2s_instance_t g_mic_i2s =
{
    .p_ctrl        = &g_mic_i2s_ctrl,
    .p_cfg         = &g_mic_i2s_cfg,
    .p_api         = &g_i2s_on_ssi
};
extern uint32_t g_fsp_common_thread_count;

                const rm_freertos_port_parameters_t audio_infer_thread_parameters =
                {
                    .p_context = (void *) NULL,
                };

                void audio_infer_thread_create (void)
                {
                    /* Increment count so we will know the number of threads created in the RA Configuration editor. */
                    g_fsp_common_thread_count++;

                    /* Initialize each kernel object. */
                    

                    #if 1
                    audio_infer_thread = xTaskCreateStatic(
                    #else
                    BaseType_t audio_infer_thread_create_err = xTaskCreate(
                    #endif
                        audio_infer_thread_func,
                        (const char *)"aud_inf",
                        12288/4, // In words, not bytes
                        (void *) &audio_infer_thread_parameters, //pvParameters
                        7,
                        #if 1
                        (StackType_t *)&audio_infer_thread_stack,
                        (StaticTask_t *)&audio_infer_thread_memory
                        #else
                        & audio_infer_thread
                        #endif
                    );

                    #if 1
                    if (NULL == audio_infer_thread)
                    {
                        rtos_startup_err_callback(audio_infer_thread, 0);
                    }
                    #else
                    if (pdPASS != audio_infer_thread_create_err)
                    {
                        rtos_startup_err_callback(audio_infer_thread, 0);
                    }
                    #endif
                }
                static void audio_infer_thread_func (void * pvParameters)
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
                    audio_infer_thread_entry(pvParameters);
                }

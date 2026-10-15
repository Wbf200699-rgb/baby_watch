/* generated thread header file - do not edit */
#ifndef AUDIO_INFER_THREAD_H_
#define AUDIO_INFER_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void audio_infer_thread_entry(void * pvParameters);
                #else
                extern void audio_infer_thread_entry(void * pvParameters);
                #endif
#include "r_dtc.h"
#include "r_transfer_api.h"
#include "r_i2s_api.h"
            #include "r_ssi.h"
FSP_HEADER
/* Transfer on DTC Instance. */
extern const transfer_instance_t g_mic_i2s_rx_transfer;

/** Access the DTC instance using these structures when calling API functions directly (::p_api is not used). */
extern dtc_instance_ctrl_t g_mic_i2s_rx_transfer_ctrl;
extern const transfer_cfg_t g_mic_i2s_rx_transfer_cfg;
/** SSI Instance. */
            extern const i2s_instance_t      g_mic_i2s;

            /** Access the SSI instance using these structures when calling API functions directly (::p_api is not used). */
            extern ssi_instance_ctrl_t g_mic_i2s_ctrl;
            extern const i2s_cfg_t g_mic_i2s_cfg;

            #ifndef mic_i2s_callback
            void mic_i2s_callback(i2s_callback_args_t * p_args);
            #endif
FSP_FOOTER
#endif /* AUDIO_INFER_THREAD_H_ */

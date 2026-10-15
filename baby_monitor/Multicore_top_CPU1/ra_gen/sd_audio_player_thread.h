/* generated thread header file - do not edit */
#ifndef SD_AUDIO_PLAYER_THREAD_H_
#define SD_AUDIO_PLAYER_THREAD_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void sd_audio_player_thread_entry(void * pvParameters);
                #else
                extern void sd_audio_player_thread_entry(void * pvParameters);
                #endif
#include "r_gpt.h"
#include "r_timer_api.h"
#include "r_i2s_api.h"
            #include "r_ssi.h"
FSP_HEADER
/** Timer on GPT Instance. */
extern const timer_instance_t g_audio_clk_timer;

/** Access the GPT instance using these structures when calling API functions directly (::p_api is not used). */
extern gpt_instance_ctrl_t g_audio_clk_timer_ctrl;
extern const timer_cfg_t g_audio_clk_timer_cfg;

#ifndef NULL
void NULL(timer_callback_args_t * p_args);
#endif
/** SSI Instance. */
            extern const i2s_instance_t      g_audio_i2s;

            /** Access the SSI instance using these structures when calling API functions directly (::p_api is not used). */
            extern ssi_instance_ctrl_t g_audio_i2s_ctrl;
            extern const i2s_cfg_t g_audio_i2s_cfg;

            #ifndef audio_i2s_callback
            void audio_i2s_callback(i2s_callback_args_t * p_args);
            #endif
FSP_FOOTER
#endif /* SD_AUDIO_PLAYER_THREAD_H_ */

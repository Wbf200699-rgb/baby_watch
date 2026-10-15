#ifndef MIC_CAPTURE_DRIVER_H
#define MIC_CAPTURE_DRIVER_H

#include "audio_infer_thread.h"
#include "thread_code/mic_ipc_shared.h"

fsp_err_t mic_capture_driver_init(void);
fsp_err_t mic_capture_start(void);
fsp_err_t mic_capture_stop(void);
fsp_err_t mic_capture_record_start(void);
fsp_err_t mic_capture_record_stop(void);
void mic_capture_poll(void);
void mic_i2s_callback(i2s_callback_args_t * p_args);

#endif /* MIC_CAPTURE_DRIVER_H */

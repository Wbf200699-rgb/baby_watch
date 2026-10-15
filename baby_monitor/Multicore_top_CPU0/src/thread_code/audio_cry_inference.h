#ifndef AUDIO_CRY_INFERENCE_H
#define AUDIO_CRY_INFERENCE_H

#include <stdint.h>

void audio_cry_inference_init(void);
void audio_cry_inference_push_raw_chunk(int32_t const * p_words, uint32_t word_count);
char const * audio_cry_type_label(void);
char const * audio_cry_model_version(void);
char const * audio_cry_type_model_version(void);

extern volatile uint32_t g_cry_model_ready;
extern volatile uint32_t g_cry_inference_count;
extern volatile uint32_t g_cry_probability_x1000;
/* Number of positive windows in the current 8-window vote, retained for protocol compatibility. */
extern volatile uint32_t g_cry_consecutive_windows;
extern volatile uint32_t g_cry_confirmed;
extern volatile uint32_t g_cry_release_hold_windows;
extern volatile uint32_t g_cry_last_inference_ms;
extern volatile uint32_t g_cry_snapshot_retries;
extern volatile uint32_t g_cry_task_create_failed;
extern volatile uint32_t g_cry_type_model_ready;
extern volatile uint32_t g_cry_type_id;
extern volatile uint32_t g_cry_type_valid;
extern volatile uint32_t g_cry_type_confidence_x1000;
extern volatile uint32_t g_cry_type_margin_x1000;
extern volatile uint32_t g_cry_type_window_count;
extern volatile uint32_t g_cry_type_probabilities_x1000[5];

#endif /* AUDIO_CRY_INFERENCE_H */

#include "sd_audio_player_thread.h"
#include "thread_code/audio_wav_player.h"

/* player entry function */
/* pvParameters contains TaskHandle_t */
void sd_audio_player_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    audio_wav_player_thread();
}

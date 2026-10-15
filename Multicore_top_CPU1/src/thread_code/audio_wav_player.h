#ifndef AUDIO_WAV_PLAYER_H
#define AUDIO_WAV_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "bsp_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_WAV_PLAYER_PATH_LENGTH (64U)

typedef enum e_audio_wav_player_status
{
    AUDIO_WAV_PLAYER_STATUS_IDLE = 0,
    AUDIO_WAV_PLAYER_STATUS_WAIT_SD,
    AUDIO_WAV_PLAYER_STATUS_OPEN_ERROR,
    AUDIO_WAV_PLAYER_STATUS_BAD_WAV,
    AUDIO_WAV_PLAYER_STATUS_I2S_ERROR,
    AUDIO_WAV_PLAYER_STATUS_PLAYING,
    AUDIO_WAV_PLAYER_STATUS_DONE
} audio_wav_player_status_t;

typedef struct st_audio_wav_player_debug
{
    uint32_t magic;
    volatile uint32_t play_count;
    volatile uint32_t bytes_read;
    volatile uint32_t bytes_written;
    volatile uint32_t sample_rate;
    volatile uint16_t channels;
    volatile uint16_t bits_per_sample;
    volatile uint32_t data_bytes;
    volatile uint32_t tx_empty_count;
    volatile uint32_t idle_count;
    volatile uint32_t ssi_ssicr;
    volatile uint32_t ssi_ssisr;
    volatile uint32_t ssi_ssifcr;
    volatile uint32_t ssi_ssifsr;
    volatile uint32_t ssi_ssiofr;
    volatile uint32_t gpt_gtcr;
    volatile uint32_t gpt_gtior;
    volatile uint32_t gpt_gtcnt;
    volatile uint32_t gpt_gtpr;
    volatile fsp_err_t last_fsp_err;
    volatile int last_file_error;
    volatile audio_wav_player_status_t status;
    char path[AUDIO_WAV_PLAYER_PATH_LENGTH];
} audio_wav_player_debug_t;

extern volatile audio_wav_player_debug_t g_audio_wav_player_debug;

void audio_wav_player_thread(void);
void audio_wav_story_writer_poll(bool sd_ready);
bool audio_wav_story_writer_active(void);
uint32_t audio_wav_story_bulk_begin(bool sd_ready, uint32_t transfer_id,
                                    uint32_t total_bytes, uint32_t expected_crc32,
                                    char const * p_name, bool play_after,
                                    uint32_t * p_next_offset);
uint32_t audio_wav_story_bulk_chunk(uint32_t transfer_id, uint32_t total_bytes,
                                    uint32_t offset, uint8_t const * p_data,
                                    uint32_t length, uint32_t chunk_crc32,
                                    uint32_t * p_next_offset);
uint32_t audio_wav_story_bulk_end(uint32_t transfer_id, uint32_t total_bytes,
                                  uint32_t expected_crc32, uint32_t * p_next_offset);
void audio_wav_story_bulk_abort(uint32_t transfer_id);
bool audio_wav_player_is_done(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_WAV_PLAYER_H */

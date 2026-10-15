#pragma once

#include <stddef.h>
#include <stdint.h>

#define MEDIA_HEADER_SIZE 32U
#define MEDIA_FLAG_DISCONTINUITY (1U << 0)
#define MEDIA_VIDEO_MAX_PAYLOAD_SIZE (512U * 1024U)
#define MEDIA_AUDIO_PAYLOAD_SIZE 640U
#define MEDIA_AUDIO_FRAME_SIZE (MEDIA_HEADER_SIZE + MEDIA_AUDIO_PAYLOAD_SIZE)

void media_build_jpeg_header(uint8_t header[MEDIA_HEADER_SIZE], uint32_t sequence,
                             uint64_t timestamp_us, uint16_t flags, uint32_t payload_length,
                             uint16_t width, uint16_t height, uint16_t quality);
void media_build_pcm_header(uint8_t header[MEDIA_HEADER_SIZE], uint32_t sequence,
                            uint64_t timestamp_us, uint16_t flags);

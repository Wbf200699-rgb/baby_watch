#include "media_protocol.h"

#include <string.h>

static void put_u16_be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put_u32_be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void put_u64_be(uint8_t *out, uint64_t value)
{
    for (int i = 7; i >= 0; --i) {
        out[i] = (uint8_t)value;
        value >>= 8;
    }
}

void media_build_pcm_header(uint8_t header[MEDIA_HEADER_SIZE], uint32_t sequence,
                            uint64_t timestamp_us, uint16_t flags)
{
    memset(header, 0, MEDIA_HEADER_SIZE);
    memcpy(header, "S3MD", 4);
    header[4] = 1;
    header[5] = 2;
    put_u16_be(header + 6, flags);
    put_u32_be(header + 8, sequence);
    put_u64_be(header + 12, timestamp_us);
    put_u32_be(header + 20, MEDIA_AUDIO_PAYLOAD_SIZE);
    put_u16_be(header + 24, 16000);
    put_u16_be(header + 26, 320);
    put_u16_be(header + 28, 1);
    put_u16_be(header + 30, 16);
}

void media_build_jpeg_header(uint8_t header[MEDIA_HEADER_SIZE], uint32_t sequence,
                             uint64_t timestamp_us, uint16_t flags, uint32_t payload_length,
                             uint16_t width, uint16_t height, uint16_t quality)
{
    memset(header, 0, MEDIA_HEADER_SIZE);
    memcpy(header, "S3MD", 4);
    header[4] = 1;
    header[5] = 1;
    put_u16_be(header + 6, flags);
    put_u32_be(header + 8, sequence);
    put_u64_be(header + 12, timestamp_us);
    put_u32_be(header + 20, payload_length);
    put_u16_be(header + 24, width);
    put_u16_be(header + 26, height);
    put_u16_be(header + 28, quality);
    put_u16_be(header + 30, 0);
}

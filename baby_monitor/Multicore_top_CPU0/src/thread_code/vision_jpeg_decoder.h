#ifndef VISION_JPEG_DECODER_H
#define VISION_JPEG_DECODER_H

#include <stddef.h>
#include <stdint.h>

typedef enum e_vision_jpeg_result
{
    VISION_JPEG_OK = 0,
    VISION_JPEG_INVALID_ARGUMENT,
    VISION_JPEG_PREPARE_FAILED,
    VISION_JPEG_DIMENSION_UNSUPPORTED,
    VISION_JPEG_OUTPUT_TOO_SMALL,
    VISION_JPEG_DECODE_FAILED
} vision_jpeg_result_t;

typedef struct st_vision_jpeg_diagnostics
{
    uint32_t decoder_result;
    uint32_t input_offset;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t scale;
    uint32_t marker_flags;
} vision_jpeg_diagnostics_t;

vision_jpeg_result_t vision_jpeg_decode_rgb888(uint8_t const * p_jpeg,
                                                size_t jpeg_size,
                                                uint8_t * p_rgb,
                                                size_t rgb_capacity,
                                                uint32_t maximum_width,
                                                uint32_t maximum_height,
                                                uint32_t * p_width,
                                                uint32_t * p_height,
                                                vision_jpeg_diagnostics_t * p_diagnostics);

#endif /* VISION_JPEG_DECODER_H */

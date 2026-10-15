#include "thread_code/vision_jpeg_decoder.h"

#include <string.h>

#include "bsp_api.h"
#include "third_party/tjpgd/tjpgd.h"

typedef struct st_vision_jpeg_io
{
    uint8_t const * p_input;
    size_t input_size;
    size_t input_offset;
    uint8_t * p_output;
    uint32_t output_width;
    uint32_t output_height;
} vision_jpeg_io_t;

static uint8_t g_vision_jpeg_work[8192U] BSP_ALIGN_VARIABLE(8);

static size_t vision_jpeg_input(JDEC * p_decoder, uint8_t * p_buffer, size_t bytes)
{
    vision_jpeg_io_t * p_io = (vision_jpeg_io_t *) p_decoder->device;
    size_t available = p_io->input_size - p_io->input_offset;

    if (bytes > available)
    {
        bytes = available;
    }
    if ((NULL != p_buffer) && (bytes > 0U))
    {
        memcpy(p_buffer, &p_io->p_input[p_io->input_offset], bytes);
    }
    p_io->input_offset += bytes;
    return bytes;
}

static int vision_jpeg_output(JDEC * p_decoder, void * p_bitmap, JRECT * p_rectangle)
{
    vision_jpeg_io_t * p_io = (vision_jpeg_io_t *) p_decoder->device;
    uint32_t left = p_rectangle->left;
    uint32_t right = p_rectangle->right;
    uint32_t top = p_rectangle->top;
    uint32_t bottom = p_rectangle->bottom;
    uint32_t row_bytes;
    uint8_t const * p_source = (uint8_t const *) p_bitmap;

    if ((right >= p_io->output_width) || (bottom >= p_io->output_height) ||
        (left > right) || (top > bottom))
    {
        return 0;
    }

    row_bytes = (right - left + 1U) * 3U;
    for (uint32_t y = top; y <= bottom; y++)
    {
        uint8_t * p_destination =
            &p_io->p_output[((y * p_io->output_width) + left) * 3U];
        memcpy(p_destination, p_source, row_bytes);
        p_source += row_bytes;
    }

    return 1;
}

vision_jpeg_result_t vision_jpeg_decode_rgb888(uint8_t const * p_jpeg,
                                                size_t jpeg_size,
                                                uint8_t * p_rgb,
                                                size_t rgb_capacity,
                                                uint32_t maximum_width,
                                                uint32_t maximum_height,
                                                uint32_t * p_width,
                                                uint32_t * p_height,
                                                vision_jpeg_diagnostics_t * p_diagnostics)
{
    JDEC decoder;
    vision_jpeg_io_t io = {
        .p_input = p_jpeg,
        .input_size = jpeg_size,
        .input_offset = 0U,
        .p_output = p_rgb,
        .output_width = 0U,
        .output_height = 0U
    };
    uint32_t output_width = 0U;
    uint32_t output_height = 0U;
    uint8_t scale;
    JRESULT result;

    if (NULL != p_diagnostics)
    {
        memset(p_diagnostics, 0, sizeof(*p_diagnostics));
        if ((NULL != p_jpeg) && (jpeg_size >= 4U))
        {
            p_diagnostics->marker_flags =
                (((0xffU == p_jpeg[0]) && (0xd8U == p_jpeg[1])) ? 1U : 0U) |
                (((0xffU == p_jpeg[jpeg_size - 2U]) &&
                  (0xd9U == p_jpeg[jpeg_size - 1U])) ? 2U : 0U);
        }
    }

    if ((NULL == p_jpeg) || (jpeg_size < 4U) || (NULL == p_rgb) ||
        (0U == maximum_width) || (0U == maximum_height) ||
        (NULL == p_width) || (NULL == p_height))
    {
        return VISION_JPEG_INVALID_ARGUMENT;
    }

    result = jd_prepare(&decoder,
                        vision_jpeg_input,
                        g_vision_jpeg_work,
                        sizeof(g_vision_jpeg_work),
                        &io);
    if (JDR_OK != result)
    {
        if (NULL != p_diagnostics)
        {
            p_diagnostics->decoder_result = (uint32_t) result;
            p_diagnostics->input_offset = (uint32_t) io.input_offset;
        }
        return VISION_JPEG_PREPARE_FAILED;
    }

    if (NULL != p_diagnostics)
    {
        p_diagnostics->source_width = decoder.width;
        p_diagnostics->source_height = decoder.height;
    }

    for (scale = 0U; scale <= 3U; scale++)
    {
        output_width = (uint32_t) decoder.width >> scale;
        output_height = (uint32_t) decoder.height >> scale;
        if ((output_width > 0U) && (output_height > 0U) &&
            (output_width <= maximum_width) && (output_height <= maximum_height))
        {
            break;
        }
    }
    if (scale > 3U)
    {
        if (NULL != p_diagnostics)
        {
            p_diagnostics->input_offset = (uint32_t) io.input_offset;
            p_diagnostics->scale = scale;
        }
        return VISION_JPEG_DIMENSION_UNSUPPORTED;
    }
    if (((size_t) output_width * output_height * 3U) > rgb_capacity)
    {
        if (NULL != p_diagnostics)
        {
            p_diagnostics->input_offset = (uint32_t) io.input_offset;
            p_diagnostics->output_width = output_width;
            p_diagnostics->output_height = output_height;
            p_diagnostics->scale = scale;
        }
        return VISION_JPEG_OUTPUT_TOO_SMALL;
    }

    io.output_width = output_width;
    io.output_height = output_height;
    result = jd_decomp(&decoder, vision_jpeg_output, scale);
    if (NULL != p_diagnostics)
    {
        p_diagnostics->decoder_result = (uint32_t) result;
        p_diagnostics->input_offset = (uint32_t) io.input_offset;
        p_diagnostics->output_width = output_width;
        p_diagnostics->output_height = output_height;
        p_diagnostics->scale = scale;
    }
    if (JDR_OK != result)
    {
        return VISION_JPEG_DECODE_FAILED;
    }

    *p_width = output_width;
    *p_height = output_height;
    return VISION_JPEG_OK;
}

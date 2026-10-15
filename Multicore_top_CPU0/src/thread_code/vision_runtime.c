#include "thread_code/vision_runtime.h"

#include <string.h>

#include "bsp_api.h"
#include "FreeRTOS.h"
#include "task.h"

typedef struct st_vision_jpeg_buffer
{
    uint32_t sequence;
    uint32_t size;
    bool complete;
    bool in_use;
} vision_jpeg_buffer_t;

/*
 * Keep the serial JPEG source in CPU0 SRAM.  The decoder and camera driver
 * both run on CPU0, and using external SDRAM here allowed the first JPEG block
 * to be corrupted while the trailing EOI marker remained intact.  Two 192 KiB
 * slots cover the camera's JPEG output while preserving capture/decode overlap.
 */
static uint8_t g_vision_jpeg_data[2][VISION_RUNTIME_JPEG_BUFFER_BYTES]
    BSP_ALIGN_VARIABLE(32);
static uint8_t g_vision_rgb_data[VISION_RUNTIME_RGB_BUFFER_BYTES]
    BSP_PLACE_IN_SECTION(".sdram_noinit") BSP_ALIGN_VARIABLE(32);

static vision_jpeg_buffer_t g_vision_jpeg_buffers[2];
static int32_t g_vision_capture_index = -1;
static int32_t g_vision_latest_index = -1;
static uint32_t g_vision_capture_expected;
static uint32_t g_vision_capture_written;
static uint32_t g_vision_dropped_frames;

static bool g_vision_preprocessed_ready;
static bool g_vision_preprocessed_claimed;
static vision_preprocessed_frame_t g_vision_preprocessed;

static volatile uint32_t g_vision_snapshot_lock;
static vision_runtime_snapshot_t g_vision_snapshot;

bool vision_runtime_capture_begin(uint32_t frame_sequence, uint32_t image_size)
{
    int32_t selected = -1;

    taskENTER_CRITICAL();
    g_vision_capture_index = -1;
    g_vision_capture_expected = image_size;
    g_vision_capture_written = 0U;

    if ((image_size > 0U) && (image_size <= VISION_RUNTIME_JPEG_BUFFER_BYTES))
    {
        for (int32_t i = 0; i < 2; i++)
        {
            if (!g_vision_jpeg_buffers[i].in_use)
            {
                selected = i;
                if (i != g_vision_latest_index)
                {
                    break;
                }
            }
        }
    }

    if (selected >= 0)
    {
        g_vision_jpeg_buffers[selected].sequence = frame_sequence;
        g_vision_jpeg_buffers[selected].size = image_size;
        g_vision_jpeg_buffers[selected].complete = false;
        g_vision_capture_index = selected;
    }
    else
    {
        g_vision_dropped_frames++;
    }
    taskEXIT_CRITICAL();

    return selected >= 0;
}

bool vision_runtime_capture_chunk(uint32_t offset, uint8_t const * p_data, uint32_t length)
{
    int32_t index = g_vision_capture_index;
    uint8_t * p_destination;

    if ((index < 0) || (NULL == p_data) ||
        (offset != g_vision_capture_written) ||
        (length > (g_vision_capture_expected - g_vision_capture_written)))
    {
        return false;
    }

    p_destination = &g_vision_jpeg_data[index][offset];
    memcpy(p_destination, p_data, length);
    if (0 != memcmp(p_destination, p_data, length))
    {
        return false;
    }
    g_vision_capture_written += length;
    return true;
}

void vision_runtime_capture_finish(bool jpeg_valid)
{
    int32_t index = g_vision_capture_index;

    taskENTER_CRITICAL();
    if ((index >= 0) && jpeg_valid &&
        (g_vision_capture_written == g_vision_capture_expected) &&
        (g_vision_capture_written >= 4U) &&
        (0xffU == g_vision_jpeg_data[index][0]) &&
        (0xd8U == g_vision_jpeg_data[index][1]) &&
        (0xffU == g_vision_jpeg_data[index][g_vision_capture_written - 2U]) &&
        (0xd9U == g_vision_jpeg_data[index][g_vision_capture_written - 1U]))
    {
        int32_t previous = g_vision_latest_index;
        g_vision_jpeg_buffers[index].size = g_vision_capture_written;
        g_vision_jpeg_buffers[index].complete = true;
        __DMB();
        g_vision_latest_index = index;

        if ((previous >= 0) && (previous != index) &&
            !g_vision_jpeg_buffers[previous].in_use)
        {
            g_vision_jpeg_buffers[previous].complete = false;
        }
    }
    else if (index >= 0)
    {
        g_vision_jpeg_buffers[index].complete = false;
        g_vision_dropped_frames++;
    }

    g_vision_capture_index = -1;
    g_vision_capture_expected = 0U;
    g_vision_capture_written = 0U;
    taskEXIT_CRITICAL();
}

void vision_runtime_capture_abort(void)
{
    vision_runtime_capture_finish(false);
}

bool vision_runtime_acquire_jpeg(uint32_t after_sequence, vision_jpeg_frame_t * p_frame)
{
    bool acquired = false;

    if (NULL == p_frame)
    {
        return false;
    }

    taskENTER_CRITICAL();
    int32_t index = g_vision_latest_index;
    if ((index >= 0) &&
        g_vision_jpeg_buffers[index].complete &&
        !g_vision_jpeg_buffers[index].in_use &&
        (g_vision_jpeg_buffers[index].sequence != after_sequence))
    {
        g_vision_jpeg_buffers[index].in_use = true;
        p_frame->p_data = g_vision_jpeg_data[index];
        p_frame->size = g_vision_jpeg_buffers[index].size;
        p_frame->sequence = g_vision_jpeg_buffers[index].sequence;
        p_frame->buffer_index = (uint8_t) index;
        acquired = true;
    }
    taskEXIT_CRITICAL();

    return acquired;
}

void vision_runtime_release_jpeg(vision_jpeg_frame_t const * p_frame)
{
    if ((NULL == p_frame) || (p_frame->buffer_index >= 2U))
    {
        return;
    }

    taskENTER_CRITICAL();
    uint32_t index = p_frame->buffer_index;
    if (g_vision_jpeg_buffers[index].sequence == p_frame->sequence)
    {
        g_vision_jpeg_buffers[index].in_use = false;
        if ((int32_t) index != g_vision_latest_index)
        {
            g_vision_jpeg_buffers[index].complete = false;
        }
    }
    taskEXIT_CRITICAL();
}

uint32_t vision_runtime_dropped_frames(void)
{
    return g_vision_dropped_frames;
}

uint8_t * vision_runtime_rgb_buffer(void)
{
    return g_vision_rgb_data;
}

bool vision_runtime_preprocess_slot_available(void)
{
    return !g_vision_preprocessed_ready;
}

bool vision_runtime_publish_preprocessed(vision_preprocessed_frame_t const * p_frame)
{
    bool published = false;

    if (NULL == p_frame)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (!g_vision_preprocessed_ready)
    {
        g_vision_preprocessed = *p_frame;
        __DMB();
        g_vision_preprocessed_claimed = false;
        g_vision_preprocessed_ready = true;
        published = true;
    }
    taskEXIT_CRITICAL();

    return published;
}

bool vision_runtime_acquire_preprocessed(uint32_t after_sequence, vision_preprocessed_frame_t * p_frame)
{
    bool acquired = false;

    if (NULL == p_frame)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (g_vision_preprocessed_ready &&
        !g_vision_preprocessed_claimed &&
        (g_vision_preprocessed.frame_sequence != after_sequence))
    {
        g_vision_preprocessed_claimed = true;
        *p_frame = g_vision_preprocessed;
        acquired = true;
    }
    taskEXIT_CRITICAL();

    return acquired;
}

void vision_runtime_release_preprocessed(uint32_t frame_sequence)
{
    taskENTER_CRITICAL();
    if (g_vision_preprocessed_ready &&
        (g_vision_preprocessed.frame_sequence == frame_sequence))
    {
        g_vision_preprocessed_ready = false;
        g_vision_preprocessed_claimed = false;
    }
    taskEXIT_CRITICAL();
}

void vision_runtime_publish_snapshot(vision_runtime_snapshot_t * p_snapshot)
{
    if (NULL == p_snapshot)
    {
        return;
    }

    taskENTER_CRITICAL();
    p_snapshot->sequence = g_vision_snapshot.sequence + 1U;
    g_vision_snapshot_lock++;
    __DMB();
    memcpy(&g_vision_snapshot, p_snapshot, sizeof(g_vision_snapshot));
    __DMB();
    g_vision_snapshot_lock++;
    taskEXIT_CRITICAL();
}

bool vision_runtime_get_snapshot(vision_runtime_snapshot_t * p_snapshot)
{
    uint32_t before;
    uint32_t after;

    if (NULL == p_snapshot)
    {
        return false;
    }

    do
    {
        before = g_vision_snapshot_lock;
        if (0U != (before & 1U))
        {
            continue;
        }
        __DMB();
        memcpy(p_snapshot, &g_vision_snapshot, sizeof(*p_snapshot));
        __DMB();
        after = g_vision_snapshot_lock;
    } while ((before != after) || (0U != (after & 1U)));

    return 0U != p_snapshot->sequence;
}

#include "thread_code/vision_model_storage.h"

#include <string.h>

#include "bsp_api.h"
#include "thread_code/vision_external_memory.h"
#include "vision_models/sub_0000behavior_model_data.h"
#include "vision_models/sub_0000occlusion_model_data.h"
#include "vision_models/sub_0000scene_model_data.h"

static uint8_t g_scene_model[VISION_SCENE_MODEL_BYTES]
    BSP_PLACE_IN_SECTION(".sdram_noinit") BSP_ALIGN_VARIABLE(32);
static uint8_t g_behavior_model[VISION_BEHAVIOR_MODEL_BYTES]
    BSP_PLACE_IN_SECTION(".sdram_noinit") BSP_ALIGN_VARIABLE(32);
static uint8_t g_occlusion_model[VISION_OCCLUSION_MODEL_BYTES]
    BSP_PLACE_IN_SECTION(".sdram_noinit") BSP_ALIGN_VARIABLE(32);

static bool g_model_storage_ready;

static bool vision_model_window_has_content(uint8_t const * p_data, size_t size)
{
    bool all_zero = true;
    bool all_erased = true;

    for (size_t i = 0U; i < size; i++)
    {
        all_zero = all_zero && (0x00U == p_data[i]);
        all_erased = all_erased && (0xffU == p_data[i]);
    }

    return !all_zero && !all_erased;
}

static bool vision_model_source_looks_programmed(uint8_t const * p_data, size_t size)
{
    enum
    {
        VISION_MODEL_CHECK_WINDOW_BYTES = 256U,
        VISION_MODEL_CHECK_WINDOW_COUNT = 5U,
        VISION_MODEL_REQUIRED_CONTENT_WINDOWS = 3U
    };
    size_t const last_window = size - VISION_MODEL_CHECK_WINDOW_BYTES;
    size_t const offsets[VISION_MODEL_CHECK_WINDOW_COUNT] = {
        0U,
        size / 4U,
        size / 2U,
        (size * 3U) / 4U,
        last_window
    };
    uint32_t content_windows = 0U;

    if ((NULL == p_data) || (size < (VISION_MODEL_CHECK_WINDOW_BYTES * 4U)))
    {
        return false;
    }

    /*
     * Vela blobs do not share a fixed first byte: the scene model begins with
     * a valid zero-filled header while the two classifiers begin with 0x7f.
     * Sample the whole mapped region instead, so blank/erased or only partly
     * programmed OSPI images are rejected without depending on model format.
     */
    for (uint32_t i = 0U; i < VISION_MODEL_CHECK_WINDOW_COUNT; i++)
    {
        if (vision_model_window_has_content(&p_data[offsets[i]],
                                            VISION_MODEL_CHECK_WINDOW_BYTES))
        {
            content_windows++;
        }
    }

    return content_windows >= VISION_MODEL_REQUIRED_CONTENT_WINDOWS;
}

fsp_err_t vision_model_storage_prepare(void)
{
    if (g_model_storage_ready)
    {
        return FSP_SUCCESS;
    }

    if (!vision_sdram_is_ready())
    {
        return vision_sdram_last_error();
    }

    if (!vision_external_memory_is_ready())
    {
        return vision_external_memory_last_error();
    }

    if ((VISION_SCENE_MODEL_BYTES != sub_0000scene_model_data_size) ||
        (VISION_BEHAVIOR_MODEL_BYTES != sub_0000behavior_model_data_size) ||
        (VISION_OCCLUSION_MODEL_BYTES != sub_0000occlusion_model_data_size))
    {
        return FSP_ERR_INVALID_SIZE;
    }

    if (!vision_model_source_looks_programmed(sub_0000scene_model_data,
                                               sub_0000scene_model_data_size) ||
        !vision_model_source_looks_programmed(sub_0000behavior_model_data,
                                               sub_0000behavior_model_data_size) ||
        !vision_model_source_looks_programmed(sub_0000occlusion_model_data,
                                               sub_0000occlusion_model_data_size))
    {
        return FSP_ERR_INVALID_DATA;
    }

    memcpy(g_scene_model, sub_0000scene_model_data, VISION_SCENE_MODEL_BYTES);
    memcpy(g_behavior_model, sub_0000behavior_model_data, VISION_BEHAVIOR_MODEL_BYTES);
    memcpy(g_occlusion_model, sub_0000occlusion_model_data, VISION_OCCLUSION_MODEL_BYTES);

#if BSP_CFG_DCACHE_ENABLED
    SCB_CleanDCache_by_Addr((uint32_t *) g_scene_model, (int32_t) sizeof(g_scene_model));
    SCB_CleanDCache_by_Addr((uint32_t *) g_behavior_model, (int32_t) sizeof(g_behavior_model));
    SCB_CleanDCache_by_Addr((uint32_t *) g_occlusion_model, (int32_t) sizeof(g_occlusion_model));
#endif

    g_model_storage_ready = true;
    return FSP_SUCCESS;
}

bool vision_model_storage_is_ready(void)
{
    return g_model_storage_ready;
}

uint8_t const * vision_model_storage_scene(void)
{
    return g_scene_model;
}

uint8_t const * vision_model_storage_behavior(void)
{
    return g_behavior_model;
}

uint8_t const * vision_model_storage_occlusion(void)
{
    return g_occlusion_model;
}

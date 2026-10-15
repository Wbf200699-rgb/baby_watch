#include "vision_infer_thread.h"

#include <math.h>
#include <string.h>

#include "common_data.h"
#include "thread_code/vision_external_memory.h"
#include "thread_code/vision_model_storage.h"
#include "thread_code/vision_runtime.h"
#include "vision_models/modelbehavior.h"
#include "vision_models/modelocclusion.h"
#include "vision_models/modelscene.h"
#include "vision_models/postprocessing.h"
#include "vision_models/sub_0000behavior_invoke.h"
#include "vision_models/sub_0000occlusion_invoke.h"
#include "vision_models/sub_0000scene_invoke.h"

#define VISION_INFER_IDLE_TICKS              (pdMS_TO_TICKS(20U))
#define VISION_INFER_ERROR_RETRY_TICKS       (pdMS_TO_TICKS(2000U))
#define VISION_CLASSIFIER_INPUT_WIDTH        (192U)
#define VISION_CLASSIFIER_INPUT_HEIGHT       (192U)
#define VISION_SCENE_SCORE_THRESHOLD         (0.25F)
#define VISION_SCENE_NMS_THRESHOLD           (0.45F)

typedef struct st_vision_roi
{
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} vision_roi_t;

static uint32_t vision_elapsed_ms(TickType_t start)
{
    return (uint32_t) ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
}

static uint16_t vision_float_to_u16(float value, uint32_t maximum)
{
    if (value <= 0.0F)
    {
        return 0U;
    }
    if (value >= (float) maximum)
    {
        return (uint16_t) maximum;
    }
    return (uint16_t) (value + 0.5F);
}

static uint16_t vision_score_to_x1000(float score)
{
    if (score <= 0.0F)
    {
        return 0U;
    }
    if (score >= 1.0F)
    {
        return 1000U;
    }
    return (uint16_t) (score * 1000.0F + 0.5F);
}

static int32_t vision_find_best_detection(Detection_t const * p_detections,
                                          int32_t detection_count,
                                          uint32_t class_id)
{
    int32_t best = -1;
    float best_score = -1.0F;

    for (int32_t i = 0; i < detection_count; i++)
    {
        if ((p_detections[i].cls_id == class_id) &&
            (p_detections[i].score > best_score))
        {
            best = i;
            best_score = p_detections[i].score;
        }
    }

    return best;
}

static bool vision_make_roi(Detection_t const * p_detection,
                            uint32_t image_width,
                            uint32_t image_height,
                            float expansion,
                            vision_roi_t * p_roi)
{
    float center_x;
    float center_y;
    float width;
    float height;
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;

    if ((NULL == p_detection) || (NULL == p_roi) ||
        (0U == image_width) || (0U == image_height))
    {
        return false;
    }

    width = (p_detection->x2 - p_detection->x1) * expansion;
    height = (p_detection->y2 - p_detection->y1) * expansion;
    center_x = (p_detection->x1 + p_detection->x2) * 0.5F;
    center_y = (p_detection->y1 + p_detection->y2) * 0.5F;

    if ((width < 4.0F) || (height < 4.0F))
    {
        return false;
    }

    left = (int32_t) floorf(center_x - width * 0.5F);
    top = (int32_t) floorf(center_y - height * 0.5F);
    right = (int32_t) ceilf(center_x + width * 0.5F);
    bottom = (int32_t) ceilf(center_y + height * 0.5F);

    if (left < 0)
    {
        left = 0;
    }
    if (top < 0)
    {
        top = 0;
    }
    if (right > (int32_t) image_width)
    {
        right = (int32_t) image_width;
    }
    if (bottom > (int32_t) image_height)
    {
        bottom = (int32_t) image_height;
    }

    p_roi->x = left;
    p_roi->y = top;
    p_roi->width = right - left;
    p_roi->height = bottom - top;
    return (p_roi->width >= 4) && (p_roi->height >= 4);
}

static void vision_resize_roi_rgb_to_int8(uint8_t const * p_rgb,
                                          uint32_t image_width,
                                          uint32_t image_height,
                                          vision_roi_t const * p_roi,
                                          int8_t * p_destination)
{
    FSP_PARAMETER_NOT_USED(image_height);

    /*
     * Centre-sampled nearest-neighbour resize.  The classifier contract is
     * RGB uint8 with int8 quantization scale 1 and zero point -128.
     */
    for (uint32_t y = 0U; y < VISION_CLASSIFIER_INPUT_HEIGHT; y++)
    {
        uint32_t source_y = (uint32_t) p_roi->y +
                            (((2U * y + 1U) * (uint32_t) p_roi->height) /
                             (2U * VISION_CLASSIFIER_INPUT_HEIGHT));
        if (source_y >= (uint32_t) (p_roi->y + p_roi->height))
        {
            source_y = (uint32_t) (p_roi->y + p_roi->height - 1);
        }

        for (uint32_t x = 0U; x < VISION_CLASSIFIER_INPUT_WIDTH; x++)
        {
            uint32_t source_x = (uint32_t) p_roi->x +
                                (((2U * x + 1U) * (uint32_t) p_roi->width) /
                                 (2U * VISION_CLASSIFIER_INPUT_WIDTH));
            uint32_t source_index;
            uint32_t destination_index;

            if (source_x >= (uint32_t) (p_roi->x + p_roi->width))
            {
                source_x = (uint32_t) (p_roi->x + p_roi->width - 1);
            }

            source_index = (source_y * image_width + source_x) * 3U;
            destination_index = (y * VISION_CLASSIFIER_INPUT_WIDTH + x) * 3U;
            p_destination[destination_index] =
                (int8_t) ((int32_t) p_rgb[source_index] - 128);
            p_destination[destination_index + 1U] =
                (int8_t) ((int32_t) p_rgb[source_index + 1U] - 128);
            p_destination[destination_index + 2U] =
                (int8_t) ((int32_t) p_rgb[source_index + 2U] - 128);
        }
    }
}

static uint32_t vision_decode_softmax(int8_t const * p_output,
                                      uint32_t class_count,
                                      uint32_t * p_probabilities,
                                      uint32_t * p_confidence)
{
    uint32_t best_class = 0U;
    int32_t best_raw = -129;

    for (uint32_t i = 0U; i < class_count; i++)
    {
        int32_t raw = (int32_t) p_output[i];
        uint32_t probability = (uint32_t) (((raw + 128) * 1000 + 128) / 256);

        if (probability > 1000U)
        {
            probability = 1000U;
        }
        p_probabilities[i] = probability;
        if (raw > best_raw)
        {
            best_raw = raw;
            best_class = i;
        }
    }

    *p_confidence = p_probabilities[best_class];
    return best_class;
}

static void vision_cache_clean(void * p_data, uint32_t size)
{
#if BSP_CFG_DCACHE_ENABLED
    SCB_CleanDCache_by_Addr((uint32_t *) p_data, (int32_t) size);
#else
    FSP_PARAMETER_NOT_USED(p_data);
    FSP_PARAMETER_NOT_USED(size);
#endif
}

static void vision_cache_invalidate(void * p_data, uint32_t size)
{
#if BSP_CFG_DCACHE_ENABLED
    SCB_InvalidateDCache_by_Addr((uint32_t *) p_data, (int32_t) size);
#else
    FSP_PARAMETER_NOT_USED(p_data);
    FSP_PARAMETER_NOT_USED(size);
#endif
}

static void vision_publish_startup_status(vision_runtime_status_t status,
                                          fsp_err_t error,
                                          uint32_t model_load_ms,
                                          uint32_t model_ready_mask)
{
    vision_runtime_snapshot_t snapshot;

    if (!vision_runtime_get_snapshot(&snapshot))
    {
        memset(&snapshot, 0, sizeof(snapshot));
    }
    snapshot.tick = (uint32_t) xTaskGetTickCount();
    snapshot.status = (uint32_t) status;
    snapshot.ospi_error = (uint32_t) error;
    snapshot.model_load_ms = model_load_ms;
    snapshot.model_ready_mask = model_ready_mask;
    snapshot.fusion_eligible = 0U;
    vision_runtime_publish_snapshot(&snapshot);
}

void vision_infer_thread_entry(void * pvParameters)
{
    fsp_err_t err;
    TickType_t model_load_start;
    uint32_t model_load_ms;
    uint32_t last_frame_sequence = 0U;
    uint32_t npu_error_count = 0U;

    FSP_PARAMETER_NOT_USED(pvParameters);

    vision_publish_startup_status(VISION_STATUS_NPU_STARTING,
                                  FSP_SUCCESS,
                                  0U,
                                  0U);

    if (!vision_external_memory_is_ready())
    {
        vision_publish_startup_status(VISION_STATUS_OSPI_ERROR,
                                      vision_external_memory_last_error(),
                                      0U,
                                      0U);
        while (1)
        {
            vTaskDelay(VISION_INFER_ERROR_RETRY_TICKS);
        }
    }

    model_load_start = xTaskGetTickCount();
    err = vision_model_storage_prepare();
    model_load_ms = vision_elapsed_ms(model_load_start);
    if (FSP_SUCCESS != err)
    {
        vision_publish_startup_status(VISION_STATUS_OSPI_ERROR, err, model_load_ms, 0U);
        while (1)
        {
            vTaskDelay(VISION_INFER_ERROR_RETRY_TICKS);
        }
    }

    err = RM_ETHOSU_Open(&g_rm_ethosu0_ctrl, &g_rm_ethosu0_cfg);
    if (FSP_SUCCESS != err)
    {
        vision_publish_startup_status(VISION_STATUS_NPU_ERROR, err, model_load_ms, 0U);
        while (1)
        {
            vTaskDelay(VISION_INFER_ERROR_RETRY_TICKS);
        }
    }

    vision_publish_startup_status(VISION_STATUS_WAITING_FRAME,
                                  FSP_SUCCESS,
                                  model_load_ms,
                                  VISION_MODEL_SCENE |
                                  VISION_MODEL_OCCLUSION |
                                  VISION_MODEL_BEHAVIOR);

    while (1)
    {
        vision_preprocessed_frame_t frame;
        vision_runtime_snapshot_t snapshot;
        Detection_t detections[MAX_DETECTIONS];
        TickType_t total_start;
        TickType_t stage_start;
        int32_t detection_count;
        int32_t behavior_detection;
        int32_t occlusion_detection;
        int scene_result;
        bool frame_failed = false;

        if (!vision_runtime_acquire_preprocessed(last_frame_sequence, &frame))
        {
            vTaskDelay(VISION_INFER_IDLE_TICKS);
            continue;
        }

        last_frame_sequence = frame.frame_sequence;
        total_start = xTaskGetTickCount();
        if (!vision_runtime_get_snapshot(&snapshot))
        {
            memset(&snapshot, 0, sizeof(snapshot));
        }
        snapshot.tick = (uint32_t) xTaskGetTickCount();
        snapshot.frame_sequence = frame.frame_sequence;
        snapshot.jpeg_bytes = frame.jpeg_bytes;
        snapshot.image_width = frame.width;
        snapshot.image_height = frame.height;
        snapshot.decode_ms = frame.decode_ms;
        snapshot.preprocess_ms = frame.preprocess_ms;
        snapshot.status = (uint32_t) VISION_STATUS_INFERENCING;
        snapshot.model_ready_mask = VISION_MODEL_SCENE |
                                    VISION_MODEL_OCCLUSION |
                                    VISION_MODEL_BEHAVIOR;
        snapshot.ospi_error = 0U;
        snapshot.model_load_ms = model_load_ms;
        snapshot.detection_count = 0U;
        memset(snapshot.scene_scores_x1000, 0, sizeof(snapshot.scene_scores_x1000));
        memset(snapshot.detections, 0, sizeof(snapshot.detections));
        vision_runtime_publish_snapshot(&snapshot);

        stage_start = xTaskGetTickCount();
        vision_cache_clean(sub_0000scene_arena, kArenaSize_sub_0000scene);
        scene_result = sub_0000scene_invoke(false);
        vision_cache_invalidate(sub_0000scene_arena, kArenaSize_sub_0000scene);
        snapshot.scene_ms = vision_elapsed_ms(stage_start);

        if (0 != scene_result)
        {
            frame_failed = true;
        }

        if (!frame_failed)
        {
            letterbox_params_t letterbox =
            {
                .scale = frame.letterbox_scale,
                .pad_x = frame.letterbox_pad_x,
                .pad_y = frame.letterbox_pad_y
            };

            detection_count = postprocess(
                GetModelOutputPtrscene_PartitionedCall_0_70468(),
                &letterbox,
                frame.width,
                frame.height,
                VISION_SCENE_SCORE_THRESHOLD,
                VISION_SCENE_NMS_THRESHOLD,
                detections);
            if (detection_count < 0)
            {
                detection_count = 0;
                frame_failed = true;
            }
        }
        else
        {
            detection_count = 0;
        }

        for (int32_t i = 0; i < detection_count; i++)
        {
            uint32_t class_id = detections[i].cls_id;
            uint16_t score = vision_score_to_x1000(detections[i].score);

            if ((class_id < VISION_SCENE_CLASS_COUNT) &&
                (score > snapshot.scene_scores_x1000[class_id]))
            {
                snapshot.scene_scores_x1000[class_id] = score;
            }

            if ((uint32_t) i < VISION_RUNTIME_MAX_DETECTIONS)
            {
                uint16_t x1 = vision_float_to_u16(detections[i].x1, frame.width);
                uint16_t y1 = vision_float_to_u16(detections[i].y1, frame.height);
                uint16_t x2 = vision_float_to_u16(detections[i].x2, frame.width);
                uint16_t y2 = vision_float_to_u16(detections[i].y2, frame.height);

                snapshot.detections[i].x = x1;
                snapshot.detections[i].y = y1;
                snapshot.detections[i].width = (x2 > x1) ? (uint16_t) (x2 - x1) : 0U;
                snapshot.detections[i].height = (y2 > y1) ? (uint16_t) (y2 - y1) : 0U;
                snapshot.detections[i].score_x1000 = score;
                snapshot.detections[i].class_id = (uint8_t) class_id;
            }
        }
        snapshot.detection_count = (uint32_t) detection_count;
        if (snapshot.detection_count > VISION_RUNTIME_MAX_DETECTIONS)
        {
            snapshot.detection_count = VISION_RUNTIME_MAX_DETECTIONS;
        }

        memset(snapshot.behavior_probabilities_x1000,
               0,
               sizeof(snapshot.behavior_probabilities_x1000));
        snapshot.behavior_class = VISION_BEHAVIOR_UNKNOWN;
        snapshot.behavior_confidence_x1000 = 1000U;
        snapshot.behavior_probabilities_x1000[VISION_BEHAVIOR_UNKNOWN] = 1000U;
        snapshot.behavior_ms = 0U;
        memset(snapshot.occlusion_probabilities_x1000,
               0,
               sizeof(snapshot.occlusion_probabilities_x1000));
        snapshot.occlusion_class = VISION_OCCLUSION_NOT_VISIBLE;
        snapshot.occlusion_confidence_x1000 = 0U;
        snapshot.occlusion_valid = 0U;
        snapshot.occlusion_ms = 0U;

        behavior_detection = vision_find_best_detection(detections,
                                                        detection_count,
                                                        VISION_SCENE_INFANT);
        if (!frame_failed && (behavior_detection >= 0))
        {
            vision_roi_t roi;
            if (vision_make_roi(&detections[behavior_detection],
                                frame.width,
                                frame.height,
                                1.12F,
                                &roi))
            {
                int8_t * p_input =
                    GetModelInputPtrbehavior_serving_default_image_0();
                stage_start = xTaskGetTickCount();
                vision_resize_roi_rgb_to_int8(frame.p_rgb,
                                              frame.width,
                                              frame.height,
                                              &roi,
                                              p_input);
                vision_cache_clean(sub_0000behavior_arena,
                                   kArenaSize_sub_0000behavior);
                if (0 != sub_0000behavior_invoke(false))
                {
                    frame_failed = true;
                }
                vision_cache_invalidate(sub_0000behavior_arena,
                                        kArenaSize_sub_0000behavior);
                snapshot.behavior_ms = vision_elapsed_ms(stage_start);
                if (!frame_failed)
                {
                    snapshot.behavior_class = vision_decode_softmax(
                        GetModelOutputPtrbehavior_StatefulPartitionedCall_1_0_70117(),
                        VISION_BEHAVIOR_CLASS_COUNT,
                        snapshot.behavior_probabilities_x1000,
                        &snapshot.behavior_confidence_x1000);
                }
            }
        }

        /* The occlusion model was trained on an expanded infant crop.  The
         * scene model does not reliably provide an independent head class, so
         * requiring HEAD_FACE here suppresses the classifier entirely. */
        occlusion_detection = behavior_detection;

        if (!frame_failed && (occlusion_detection >= 0))
        {
            vision_roi_t roi;
            if (vision_make_roi(&detections[occlusion_detection],
                                frame.width,
                                frame.height,
                                1.25F,
                                &roi))
            {
                int8_t * p_input =
                    GetModelInputPtrocclusion_serving_default_image_0();
                stage_start = xTaskGetTickCount();
                vision_resize_roi_rgb_to_int8(frame.p_rgb,
                                              frame.width,
                                              frame.height,
                                              &roi,
                                              p_input);
                vision_cache_clean(sub_0000occlusion_arena,
                                   kArenaSize_sub_0000occlusion);
                if (0 != sub_0000occlusion_invoke(false))
                {
                    frame_failed = true;
                }
                vision_cache_invalidate(sub_0000occlusion_arena,
                                        kArenaSize_sub_0000occlusion);
                snapshot.occlusion_ms = vision_elapsed_ms(stage_start);
                if (!frame_failed)
                {
                    snapshot.occlusion_class = vision_decode_softmax(
                        GetModelOutputPtrocclusion_StatefulPartitionedCall_1_0_70117(),
                        VISION_OCCLUSION_CLASS_COUNT,
                        snapshot.occlusion_probabilities_x1000,
                        &snapshot.occlusion_confidence_x1000);
                    snapshot.occlusion_valid = 1U;
                }
            }
        }

        if (frame_failed)
        {
            npu_error_count++;
            snapshot.status = (uint32_t) VISION_STATUS_NPU_ERROR;
        }
        else
        {
            snapshot.status = (uint32_t) VISION_STATUS_READY;
            snapshot.inference_count++;
        }

        snapshot.npu_error_count = npu_error_count;
        snapshot.dropped_frame_count = vision_runtime_dropped_frames();
        snapshot.total_ms = frame.decode_ms +
                            frame.preprocess_ms +
                            vision_elapsed_ms(total_start);
        snapshot.npu_busy_ms_total += snapshot.scene_ms +
                                      snapshot.behavior_ms +
                                      snapshot.occlusion_ms;

        /* Failed frames remain observable but must never affect safety decisions. */
        snapshot.fusion_eligible = frame_failed ? 0U : 1U;
        snapshot.tick = (uint32_t) xTaskGetTickCount();
        vision_runtime_publish_snapshot(&snapshot);
        vision_runtime_release_preprocessed(frame.frame_sequence);
    }
}

#include "vision_preprocess_thread.h"

#include <string.h>

#include "thread_code/vision_jpeg_decoder.h"
#include "thread_code/vision_runtime.h"
#include "vision_models/model_metadata.h"
#include "vision_models/modelscene.h"
#include "vision_models/preprocessing.h"

#define VISION_PREPROCESS_IDLE_TICKS (pdMS_TO_TICKS(20U))

static uint32_t vision_preprocess_elapsed_ms(TickType_t start)
{
    return (uint32_t) ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
}

static void vision_preprocess_publish_status(vision_runtime_status_t status,
                                             vision_jpeg_frame_t const * p_frame,
                                             uint32_t jpeg_errors,
                                             vision_jpeg_result_t jpeg_result,
                                             vision_jpeg_diagnostics_t const * p_diagnostics)
{
    vision_runtime_snapshot_t snapshot;

    if (!vision_runtime_get_snapshot(&snapshot))
    {
        memset(&snapshot, 0, sizeof(snapshot));
    }
    snapshot.tick = (uint32_t) xTaskGetTickCount();
    snapshot.status = (uint32_t) status;
    snapshot.jpeg_error_count = jpeg_errors;
    snapshot.jpeg_result = (uint32_t) jpeg_result;
    snapshot.dropped_frame_count = vision_runtime_dropped_frames();
    if (NULL != p_diagnostics)
    {
        snapshot.jpeg_decoder_result = p_diagnostics->decoder_result;
        snapshot.jpeg_input_offset = p_diagnostics->input_offset;
        snapshot.jpeg_source_width = p_diagnostics->source_width;
        snapshot.jpeg_source_height = p_diagnostics->source_height;
        snapshot.jpeg_marker_flags = p_diagnostics->marker_flags;
    }
    if (NULL != p_frame)
    {
        snapshot.frame_sequence = p_frame->sequence;
        snapshot.jpeg_bytes = p_frame->size;
    }
    if ((VISION_STATUS_JPEG_ERROR == status) ||
        (VISION_STATUS_JPEG_TOO_LARGE == status))
    {
        snapshot.image_width = 0U;
        snapshot.image_height = 0U;
        snapshot.detection_count = 0U;
        memset(snapshot.scene_scores_x1000, 0, sizeof(snapshot.scene_scores_x1000));
        memset(snapshot.occlusion_probabilities_x1000,
               0,
               sizeof(snapshot.occlusion_probabilities_x1000));
        memset(snapshot.behavior_probabilities_x1000,
               0,
               sizeof(snapshot.behavior_probabilities_x1000));
    }
    vision_runtime_publish_snapshot(&snapshot);
}

void vision_preprocess_thread_entry(void * pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);
    uint32_t last_frame_sequence = 0U;
    uint32_t jpeg_error_count = 0U;

    vision_preprocess_publish_status(VISION_STATUS_WAITING_FRAME,
                                     NULL,
                                     0U,
                                     VISION_JPEG_OK,
                                     NULL);

    while (1)
    {
        vision_jpeg_frame_t jpeg_frame;
        vision_preprocessed_frame_t preprocessed;
        letterbox_params_t letterbox;
        uint32_t decoded_width = 0U;
        uint32_t decoded_height = 0U;
        TickType_t decode_start;
        TickType_t preprocess_start;
        vision_jpeg_result_t decode_result;
        vision_jpeg_diagnostics_t jpeg_diagnostics;

        if (!vision_runtime_preprocess_slot_available() ||
            !vision_runtime_acquire_jpeg(last_frame_sequence, &jpeg_frame))
        {
            vTaskDelay(VISION_PREPROCESS_IDLE_TICKS);
            continue;
        }

        vision_preprocess_publish_status(VISION_STATUS_DECODING,
                                         &jpeg_frame,
                                         jpeg_error_count,
                                         VISION_JPEG_OK,
                                         NULL);
        decode_start = xTaskGetTickCount();
        decode_result = vision_jpeg_decode_rgb888(jpeg_frame.p_data,
                                                   jpeg_frame.size,
                                                   vision_runtime_rgb_buffer(),
                                                   VISION_RUNTIME_RGB_BUFFER_BYTES,
                                                   VISION_RUNTIME_RGB_MAX_WIDTH,
                                                   VISION_RUNTIME_RGB_MAX_HEIGHT,
                                                   &decoded_width,
                                                   &decoded_height,
                                                   &jpeg_diagnostics);

        memset(&preprocessed, 0, sizeof(preprocessed));
        preprocessed.frame_sequence = jpeg_frame.sequence;
        preprocessed.jpeg_bytes = jpeg_frame.size;
        preprocessed.decode_ms = vision_preprocess_elapsed_ms(decode_start);
        last_frame_sequence = jpeg_frame.sequence;
        vision_runtime_release_jpeg(&jpeg_frame);

        if (VISION_JPEG_OK != decode_result)
        {
            jpeg_error_count++;
            vision_preprocess_publish_status(VISION_STATUS_JPEG_ERROR,
                                             &jpeg_frame,
                                             jpeg_error_count,
                                             decode_result,
                                             &jpeg_diagnostics);
            continue;
        }

        vision_preprocess_publish_status(VISION_STATUS_PREPROCESSING,
                                         &jpeg_frame,
                                         jpeg_error_count,
                                         decode_result,
                                         &jpeg_diagnostics);
        preprocess_start = xTaskGetTickCount();
        preprocess(vision_runtime_rgb_buffer(),
                   (uint16_t) decoded_width,
                   (uint16_t) decoded_height,
                   GetModelInputPtrscene_serving_default_images_0(),
                   MODEL_INPUT_W,
                   MODEL_INPUT_H,
                   &letterbox);

        preprocessed.p_rgb = vision_runtime_rgb_buffer();
        preprocessed.width = decoded_width;
        preprocessed.height = decoded_height;
        preprocessed.preprocess_ms = vision_preprocess_elapsed_ms(preprocess_start);
        preprocessed.letterbox_scale = letterbox.scale;
        preprocessed.letterbox_pad_x = letterbox.pad_x;
        preprocessed.letterbox_pad_y = letterbox.pad_y;

        if (!vision_runtime_publish_preprocessed(&preprocessed))
        {
            vTaskDelay(VISION_PREPROCESS_IDLE_TICKS);
        }
    }
}

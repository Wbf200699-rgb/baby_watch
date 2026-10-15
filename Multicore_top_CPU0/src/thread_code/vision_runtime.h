#ifndef VISION_RUNTIME_H
#define VISION_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VISION_RUNTIME_BUILD                (0x20260729U)
#define VISION_RUNTIME_JPEG_BUFFER_BYTES    (192U * 1024U)
#define VISION_RUNTIME_RGB_MAX_WIDTH        (320U)
#define VISION_RUNTIME_RGB_MAX_HEIGHT       (240U)
#define VISION_RUNTIME_RGB_BUFFER_BYTES     \
    (VISION_RUNTIME_RGB_MAX_WIDTH * VISION_RUNTIME_RGB_MAX_HEIGHT * 3U)
#define VISION_RUNTIME_MAX_DETECTIONS       (8U)

enum
{
    VISION_MODEL_SCENE     = 1U << 0,
    VISION_MODEL_OCCLUSION = 1U << 1,
    VISION_MODEL_BEHAVIOR  = 1U << 2
};

typedef enum e_vision_runtime_status
{
    VISION_STATUS_BOOT = 0,
    VISION_STATUS_WAITING_FRAME,
    VISION_STATUS_DECODING,
    VISION_STATUS_PREPROCESSING,
    VISION_STATUS_NPU_STARTING,
    VISION_STATUS_INFERENCING,
    VISION_STATUS_READY,
    VISION_STATUS_JPEG_TOO_LARGE,
    VISION_STATUS_JPEG_ERROR,
    VISION_STATUS_NPU_ERROR,
    VISION_STATUS_OSPI_ERROR
} vision_runtime_status_t;

typedef enum e_vision_scene_class
{
    VISION_SCENE_INFANT = 0,
    VISION_SCENE_HEAD_FACE,
    VISION_SCENE_BLANKET,
    VISION_SCENE_SOFT_OBJECT,
    VISION_SCENE_CAREGIVER,
    VISION_SCENE_CLASS_COUNT
} vision_scene_class_t;

typedef enum e_vision_occlusion_class
{
    VISION_OCCLUSION_CLEAR = 0,
    VISION_OCCLUSION_PARTIAL,
    VISION_OCCLUSION_COVERED,
    VISION_OCCLUSION_NOT_VISIBLE,
    VISION_OCCLUSION_CLASS_COUNT
} vision_occlusion_class_t;

typedef enum e_vision_behavior_class
{
    VISION_BEHAVIOR_LYING = 0,
    VISION_BEHAVIOR_SITTING,
    VISION_BEHAVIOR_CRAWLING_KNEELING,
    VISION_BEHAVIOR_STANDING,
    VISION_BEHAVIOR_UNKNOWN,
    VISION_BEHAVIOR_CLASS_COUNT
} vision_behavior_class_t;

typedef struct st_vision_detection
{
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t score_x1000;
    uint8_t class_id;
    uint8_t reserved;
} vision_detection_t;

typedef struct st_vision_runtime_snapshot
{
    uint32_t sequence;
    uint32_t tick;
    uint32_t frame_sequence;
    uint32_t status;
    uint32_t model_ready_mask;
    uint32_t inference_count;
    uint32_t jpeg_bytes;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t decode_ms;
    uint32_t preprocess_ms;
    uint32_t scene_ms;
    uint32_t occlusion_ms;
    uint32_t behavior_ms;
    uint32_t total_ms;
    uint32_t npu_busy_ms_total;
    uint32_t jpeg_error_count;
    uint32_t jpeg_result;
    uint32_t jpeg_decoder_result;
    uint32_t jpeg_input_offset;
    uint32_t jpeg_source_width;
    uint32_t jpeg_source_height;
    uint32_t jpeg_marker_flags;
    uint32_t npu_error_count;
    uint32_t dropped_frame_count;
    uint32_t ospi_error;
    uint32_t model_load_ms;
    uint32_t detection_count;
    uint32_t scene_scores_x1000[VISION_SCENE_CLASS_COUNT];
    uint32_t occlusion_class;
    uint32_t occlusion_confidence_x1000;
    uint32_t occlusion_valid;
    uint32_t occlusion_probabilities_x1000[VISION_OCCLUSION_CLASS_COUNT];
    uint32_t behavior_class;
    uint32_t behavior_confidence_x1000;
    uint32_t behavior_probabilities_x1000[VISION_BEHAVIOR_CLASS_COUNT];
    uint32_t fusion_eligible;
    vision_detection_t detections[VISION_RUNTIME_MAX_DETECTIONS];
} vision_runtime_snapshot_t;

typedef struct st_vision_jpeg_frame
{
    uint8_t const * p_data;
    uint32_t size;
    uint32_t sequence;
    uint8_t buffer_index;
} vision_jpeg_frame_t;

typedef struct st_vision_preprocessed_frame
{
    uint8_t * p_rgb;
    uint32_t frame_sequence;
    uint32_t jpeg_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t decode_ms;
    uint32_t preprocess_ms;
    float letterbox_scale;
    int32_t letterbox_pad_x;
    int32_t letterbox_pad_y;
} vision_preprocessed_frame_t;

bool vision_runtime_capture_begin(uint32_t frame_sequence, uint32_t image_size);
bool vision_runtime_capture_chunk(uint32_t offset, uint8_t const * p_data, uint32_t length);
void vision_runtime_capture_finish(bool jpeg_valid);
void vision_runtime_capture_abort(void);

bool vision_runtime_acquire_jpeg(uint32_t after_sequence, vision_jpeg_frame_t * p_frame);
void vision_runtime_release_jpeg(vision_jpeg_frame_t const * p_frame);
uint32_t vision_runtime_dropped_frames(void);

uint8_t * vision_runtime_rgb_buffer(void);
bool vision_runtime_preprocess_slot_available(void);
bool vision_runtime_publish_preprocessed(vision_preprocessed_frame_t const * p_frame);
bool vision_runtime_acquire_preprocessed(uint32_t after_sequence, vision_preprocessed_frame_t * p_frame);
void vision_runtime_release_preprocessed(uint32_t frame_sequence);

void vision_runtime_publish_snapshot(vision_runtime_snapshot_t * p_snapshot);
bool vision_runtime_get_snapshot(vision_runtime_snapshot_t * p_snapshot);

#endif /* VISION_RUNTIME_H */

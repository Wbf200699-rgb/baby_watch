#ifndef VISION_MODEL_STORAGE_H
#define VISION_MODEL_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "bsp_api.h"

/*
 * Model constants are produced by the current RUHMI conversion.  Source
 * blobs live in OSPI0/CS0.  They are copied after the scheduler starts into
 * SDRAM so an OSPI fault does not prevent the rest of the monitor from
 * booting, while NPU inference still runs from fast SDRAM.
 */
#define VISION_SCENE_MODEL_BYTES       (4662032U)
#define VISION_BEHAVIOR_MODEL_BYTES    (548768U)
#define VISION_OCCLUSION_MODEL_BYTES   (548096U)

fsp_err_t vision_model_storage_prepare(void);
bool vision_model_storage_is_ready(void);

uint8_t const * vision_model_storage_scene(void);
uint8_t const * vision_model_storage_behavior(void);
uint8_t const * vision_model_storage_occlusion(void);

#endif /* VISION_MODEL_STORAGE_H */

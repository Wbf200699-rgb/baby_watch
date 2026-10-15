#include "sub_0000scene_tensors.h"

const TensorInfo sub_0000scene_tensors[] = {
  { "_split_1_command_stream", 1, 26028, "COMMAND_STREAM", 0xffffffff },
  { "_split_1_flash", 2, 4662032, "MODEL", 0xffffffff },
  { "_split_1_scratch", 3, 802816, "ARENA", 0x0 },
  { "_split_1_scratch_fast", 4, 802816, "FAST_SCRATCH", 0x0 },
  { "serving_default_images_0", 5, 150528, "INPUT_TENSOR", 0x62000 },
  { "PartitionedCall_0_70468", 0, 10290, "OUTPUT_TENSOR", 0x19440 },
};

const size_t sub_0000scene_tensors_count = sizeof(sub_0000scene_tensors) / sizeof(sub_0000scene_tensors[0]);

// Addresses for each input and output buffer inside of the arena
const uint32_t sub_0000scene_address_serving_default_images_0 = 0x62000;
const uint32_t sub_0000scene_address_PartitionedCall_0_70468 = 0x19440;

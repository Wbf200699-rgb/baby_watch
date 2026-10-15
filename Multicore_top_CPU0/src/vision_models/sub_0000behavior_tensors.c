#include "sub_0000behavior_tensors.h"

const TensorInfo sub_0000behavior_tensors[] = {
  { "_split_1_command_stream", 1, 6724, "COMMAND_STREAM", 0xffffffff },
  { "_split_1_flash", 2, 548768, "MODEL", 0xffffffff },
  { "_split_1_scratch", 3, 258048, "ARENA", 0x0 },
  { "_split_1_scratch_fast", 4, 258048, "FAST_SCRATCH", 0x0 },
  { "serving_default_image_0", 5, 110592, "INPUT_TENSOR", 0x0 },
  { "StatefulPartitionedCall_1_0_70117", 0, 5, "OUTPUT_TENSOR", 0x10 },
};

const size_t sub_0000behavior_tensors_count = sizeof(sub_0000behavior_tensors) / sizeof(sub_0000behavior_tensors[0]);

// Addresses for each input and output buffer inside of the arena
const uint32_t sub_0000behavior_address_serving_default_image_0 = 0x0;
const uint32_t sub_0000behavior_address_StatefulPartitionedCall_1_0_70117 = 0x10;

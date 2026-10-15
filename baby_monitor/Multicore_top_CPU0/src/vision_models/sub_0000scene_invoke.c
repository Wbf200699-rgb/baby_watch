#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "common_data.h"

#include "sub_0000scene_tensors.h"
#include "sub_0000scene_command_stream.h"
#include "sub_0000scene_model_data.h"

#include "sub_0000scene_invoke.h"
#include "thread_code/vision_model_storage.h"

// Include Ethos-U driver headers (Assumed to be available)
#include "ethosu_driver.h"

// Define arenas with allocation and 16-byte alignment
__attribute__((aligned(32), section(".sdram"))) uint8_t sub_0000scene_arena[802816];
// Fast scratch arena not used for Ethos-U55
//  We will not create it for now and reuse the address of the other arena
// __attribute__((aligned(16))) static uint8_t sub_0000scene_fast_scratch[802816];
uint8_t* sub_0000scene_fast_scratch = sub_0000scene_arena;

int sub_0000scene_invoke(bool clean_outputs) {
  // Initialize base addresses and sizes
  uint64_t base_addrs[5] = {0};
  size_t base_addrs_size[5] = {0};
  int num_base_addrs = 5;

  // Variables for command stream
  uint8_t* cms_data = NULL;
  int cms_size = 0;

  // Prepare base_addrs and base_addrs_size arrays
  // Buffer sub_0000scene_model with size 4662032 and address: 4294967295
  base_addrs[0] = (uint64_t)(uintptr_t)vision_model_storage_scene();
  base_addrs_size[0] = sub_0000scene_model_data_size;
  // Buffer sub_0000scene_arena with size 802816 and address: 0
  base_addrs[1] = (uint64_t)(uintptr_t) (sub_0000scene_arena+0);
  base_addrs_size[1] = 802816;

  // Buffer sub_0000scene_fast_scratch with size 802816 and address: 0
  base_addrs[2] = (uint64_t)(uintptr_t) (sub_0000scene_arena+0);
  base_addrs_size[2] = 802816;

  // Buffer input_tensor_0 with size 150528 and address: 401408
  base_addrs[3] = (uint64_t)(uintptr_t) (sub_0000scene_arena+401408);
  base_addrs_size[3] = 150528;

  // Buffer output_tensor_0 with size 10290 and address: 103488
  if (clean_outputs) {
    memset(sub_0000scene_arena + 103488, 0, 10290);
  }
  base_addrs[4] = (uint64_t)(uintptr_t) (sub_0000scene_arena+103488);
  base_addrs_size[4] = 10290;

  // Command stream data
  cms_data = (uint8_t*)sub_0000scene_command_stream;
  cms_size = (int) sub_0000scene_command_stream_size;

  // Invoke the Ethos-U driver
  if (num_base_addrs > 8) {
    num_base_addrs = 8;
  }
  int result = ethosu_invoke_v3(&g_ethosu0, cms_data, cms_size, base_addrs, base_addrs_size, num_base_addrs, NULL);

  if (result == -1) {
    // Ethos-U invocation failed
    return -1;
  }

  return 0;
}

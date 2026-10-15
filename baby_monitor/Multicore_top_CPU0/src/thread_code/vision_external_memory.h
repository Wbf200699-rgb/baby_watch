#ifndef VISION_EXTERNAL_MEMORY_H
#define VISION_EXTERNAL_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

#include "bsp_api.h"

/*
 * Called from R_BSP_WarmStart after I/O and SDRAM initialization.  The
 * function deliberately reports an error instead of halting the system so
 * the existing telemetry/audio/sensor paths can still boot if OSPI is not
 * available.
 */
fsp_err_t vision_external_memory_init(void);
bool vision_external_memory_is_ready(void);
fsp_err_t vision_external_memory_last_error(void);
uint32_t vision_external_memory_jedec_id(void);
bool vision_sdram_is_ready(void);
fsp_err_t vision_sdram_last_error(void);
uint32_t vision_sdram_error_offset(void);

#endif /* VISION_EXTERNAL_MEMORY_H */

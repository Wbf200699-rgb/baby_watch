#ifndef SILENT_RESET_SHARED_H
#define SILENT_RESET_SHARED_H

#include <stdbool.h>
#include <stdint.h>

#define SILENT_RESET_CWSF_WARM                (1U)
#define SILENT_RESET_RSTSR1_SOFTWARE_RESET    (1UL << 2)

/*
 * CPU1 captures this state at BSP_WARM_START_POST_C, after SystemRuntimeInit
 * has initialized .bss but before any RTOS task starts.  Keeping the state in
 * ordinary CPU1 RAM avoids reading or partially writing undefined ECC SRAM
 * during the pre-C reset hook.
 */
typedef struct st_silent_reset_boot_state
{
    uint32_t capture_count;
    uint32_t silent_boot_count;
    uint32_t cold_warm_flag;
    uint32_t rstsr0;
    uint32_t rstsr1;
    uint32_t rstsr3;
} silent_reset_boot_state_t;

bool silent_reset_boot_state_get(silent_reset_boot_state_t * p_state);

#endif /* SILENT_RESET_SHARED_H */

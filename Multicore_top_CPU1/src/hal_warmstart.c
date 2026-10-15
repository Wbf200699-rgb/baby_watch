/*
* Copyright (c) 2020 - 2026 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

#include "hal_data.h"
#include "thread_code/silent_reset_shared.h"

static volatile silent_reset_boot_state_t g_silent_reset_boot_state;
static volatile bool g_silent_reset_boot_state_valid;

static void silent_reset_capture_post_c(void);

FSP_CPP_HEADER
void R_BSP_WarmStart(bsp_warm_start_event_t event);

FSP_CPP_FOOTER

/*******************************************************************************************************************//**
 * This function is called at various points during the startup process.  This implementation uses the event that is
 * called right before main() to set up the pins.
 *
 * @param[in]  event    Where at in the start up process the code is currently at
 **********************************************************************************************************************/
void R_BSP_WarmStart (bsp_warm_start_event_t event)
{
    if (BSP_WARM_START_RESET == event)
    {
#if BSP_FEATURE_FLASH_LP_VERSION != 0

        /* Enable reading from data flash. */
        R_FACI_LP->DFLCTL = 1U;

        /* Would normally have to wait tDSTOP(6us) for data flash recovery. Placing the enable here, before clock and
         * C runtime initialization, should negate the need for a delay since the initialization will typically take more than 6us. */
#endif
    }

#if BSP_CFG_OSPI_B_STARTUP_ENABLED && defined(BSP_CFG_OSPI_B_STARTUP_FN)
    if (BSP_WARM_START_POST_CLOCK == event)
    {
        /* Setup OSPI_B SiP flash and initialize it. */
        R_BSP_OspiBInit(BSP_CFG_OSPI_B_STARTUP_FN, true);
    }
#endif

    if (BSP_WARM_START_POST_C == event)
    {
        /* C runtime environment and system clocks are setup. */

        /*
         * CPU1 is the sole CWSF owner.  SystemRuntimeInit has initialized
         * ordinary CPU1 .bss before this hook, so capturing here avoids any
         * access to undefined POR ECC SRAM while still preceding all tasks.
         */
        silent_reset_capture_post_c();

        /* Configure pins. */
        R_IOPORT_Open(&IOPORT_CFG_CTRL, &IOPORT_CFG_NAME);

#if BSP_CFG_SDRAM_ENABLED

        /* Setup SDRAM and initialize it. Must configure pins first. */
        R_BSP_SdramInit(true);
#endif
    }
}

static void silent_reset_capture_post_c(void)
{
    uint32_t cold_warm_flag = (uint32_t) R_SYSTEM->RSTSR2_b.CWSF;

    /*
     * Publish validity last.  No task can run during POST_C, but this ordering
     * also keeps the accessor well-defined if startup sequencing changes.
     */
    g_silent_reset_boot_state_valid = false;
    g_silent_reset_boot_state.capture_count = 1U;
    g_silent_reset_boot_state.silent_boot_count =
        (SILENT_RESET_CWSF_WARM == cold_warm_flag) ? 1U : 0U;
    g_silent_reset_boot_state.cold_warm_flag = cold_warm_flag;
    g_silent_reset_boot_state.rstsr0 = (uint32_t) R_SYSTEM->RSTSR0;
    g_silent_reset_boot_state.rstsr1 = R_SYSTEM->RSTSR1;
    g_silent_reset_boot_state.rstsr3 = (uint32_t) R_SYSTEM->RSTSR3;
    __DMB();
    g_silent_reset_boot_state_valid = true;
    __DMB();

    /*
     * POR initializes CWSF to zero.  RESET_n, SYSRESETREQ and watchdog resets
     * retain the software-written one, so all warm boots suppress BOOT.WAV.
     */
    R_SYSTEM->RSTSR2_b.CWSF = SILENT_RESET_CWSF_WARM;
    __DSB();
}

bool silent_reset_boot_state_get(silent_reset_boot_state_t * p_state)
{
    if ((NULL == p_state) || !g_silent_reset_boot_state_valid)
    {
        return false;
    }

    __DMB();
    p_state->capture_count = g_silent_reset_boot_state.capture_count;
    p_state->silent_boot_count = g_silent_reset_boot_state.silent_boot_count;
    p_state->cold_warm_flag = g_silent_reset_boot_state.cold_warm_flag;
    p_state->rstsr0 = g_silent_reset_boot_state.rstsr0;
    p_state->rstsr1 = g_silent_reset_boot_state.rstsr1;
    p_state->rstsr3 = g_silent_reset_boot_state.rstsr3;
    __DMB();

    return g_silent_reset_boot_state_valid;
}

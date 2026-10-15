/* generated common source file - do not edit */
#include "common_data.h"
dtc_instance_ctrl_t g_transfer0_ctrl;

#if (1 == 1)
transfer_info_t g_transfer0_info DTC_TRANSFER_INFO_ALIGNMENT =
{
    .transfer_settings_word_b.dest_addr_mode = TRANSFER_ADDR_MODE_FIXED,
    .transfer_settings_word_b.repeat_area    = TRANSFER_REPEAT_AREA_SOURCE,
    .transfer_settings_word_b.irq            = TRANSFER_IRQ_END,
    .transfer_settings_word_b.chain_mode     = TRANSFER_CHAIN_MODE_DISABLED,
    .transfer_settings_word_b.src_addr_mode  = TRANSFER_ADDR_MODE_INCREMENTED,
    .transfer_settings_word_b.size           = TRANSFER_SIZE_4_BYTE,
    .transfer_settings_word_b.mode           = TRANSFER_MODE_NORMAL,
    .p_dest                                  = (void *) NULL,
    .p_src                                   = (void const *) NULL,
    .num_blocks                              = (uint16_t) 0,
    .length                                  = (uint16_t) 128,
};

#elif (1 > 1)
/* User is responsible to initialize the array. */
transfer_info_t g_transfer0_info[1] DTC_TRANSFER_INFO_ALIGNMENT;
#else
/* User must call api::reconfigure before enable DTC transfer. */
#endif

const dtc_extended_cfg_t g_transfer0_cfg_extend =
{
    .activation_source   = VECTOR_NUMBER_SDHIMMC0_DMA_REQ,
};

const transfer_cfg_t g_transfer0_cfg =
{
#if (1 == 1)
    .p_info              = &g_transfer0_info,
#elif (1 > 1)
    .p_info              = g_transfer0_info,
#else
    .p_info = NULL,
#endif
    .p_extend            = &g_transfer0_cfg_extend,
};

/* Instance structure to use this module. */
const transfer_instance_t g_transfer0 =
{
    .p_ctrl        = &g_transfer0_ctrl,
    .p_cfg         = &g_transfer0_cfg,
    .p_api         = &g_transfer_on_dtc
};
#define RA_NOT_DEFINED (UINT32_MAX)
#if (RA_NOT_DEFINED) != (RA_NOT_DEFINED)

/* If the transfer module is DMAC, define a DMAC transfer callback. */
#include "r_dmac.h"
extern void r_sdhi_transfer_callback(sdhi_instance_ctrl_t * p_ctrl);

void g_sdhi_dmac_callback (dmac_callback_args_t * p_args)
{
    r_sdhi_transfer_callback((sdhi_instance_ctrl_t *) p_args->p_context);
}
#endif
#undef RA_NOT_DEFINED

sdhi_instance_ctrl_t g_sdhi_ctrl;
sdmmc_cfg_t g_sdhi_cfg =
{
    .bus_width              = SDMMC_BUS_WIDTH_1_BIT,
    .channel                = 0,
    .p_callback             = rm_block_media_sdmmc_callback,
    .p_context              = &g_rm_block_media0_ctrl,
    .block_size             = 512,
    .card_detect            = SDMMC_CARD_DETECT_CD,
    .write_protect          = SDMMC_WRITE_PROTECT_NONE,

    .p_extend               = NULL,
    .p_lower_lvl_transfer   = &g_transfer0,

    .access_ipl             = (12),
    .sdio_ipl               = BSP_IRQ_DISABLED,
    .card_ipl               = (BSP_IRQ_DISABLED),
    .dma_req_ipl            = (12),
#if defined(VECTOR_NUMBER_SDHIMMC0_ACCS)
    .access_irq             = VECTOR_NUMBER_SDHIMMC0_ACCS,
#else
    .access_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SDHIMMC0_CARD)
    .card_irq               = VECTOR_NUMBER_SDHIMMC0_CARD,
#else
    .card_irq               = FSP_INVALID_VECTOR,
#endif
    .sdio_irq               = FSP_INVALID_VECTOR,
#if defined(VECTOR_NUMBER_SDHIMMC0_DMA_REQ)
    .dma_req_irq            = VECTOR_NUMBER_SDHIMMC0_DMA_REQ,
#else
    .dma_req_irq            = FSP_INVALID_VECTOR,
#endif
};
/* Instance structure to use this module. */
const sdmmc_instance_t g_sdhi =
{
    .p_ctrl        = &g_sdhi_ctrl,
    .p_cfg         = &g_sdhi_cfg,
    .p_api         = &g_sdmmc_on_sdhi
};
const rm_block_media_sdmmc_extended_cfg_t g_rm_block_media0_cfg_extend =
{
    .p_sdmmc = &g_sdhi,
};
const rm_block_media_cfg_t g_rm_block_media0_cfg =
{
    .p_extend   = &g_rm_block_media0_cfg_extend,
    .p_callback = rm_freertos_plus_fat_memory_callback,
    .p_context  = &g_rm_freertos_plus_fat0_ctrl,
};
rm_block_media_sdmmc_instance_ctrl_t g_rm_block_media0_ctrl;
const rm_block_media_instance_t g_rm_block_media0 =
{
    .p_api  = &g_rm_block_media_on_sdmmc,
    .p_ctrl = &g_rm_block_media0_ctrl,
    .p_cfg  = &g_rm_block_media0_cfg,
};
rm_freertos_plus_fat_instance_ctrl_t g_rm_freertos_plus_fat0_ctrl;

const rm_freertos_plus_fat_cfg_t g_rm_freertos_plus_fat0_cfg =
{
    .p_block_media = &g_rm_block_media0,
    .p_callback    = NULL,
};

static uint8_t g_rm_freertos_plus_fat0_cache[1024];
rm_freertos_plus_fat_disk_cfg_t g_rm_freertos_plus_fat0_disk_cfg =
{
    .device.sector_count      = 31293440,
    .device.sector_size_bytes = 512,
    .cache_size_bytes         = 1024,
    .p_cache                  = &g_rm_freertos_plus_fat0_cache[0],
    .partition_number         = 0,
};

/* Instance structure to use this module. */
const rm_freertos_plus_fat_instance_t g_rm_freertos_plus_fat0 =
{
    .p_ctrl        = &g_rm_freertos_plus_fat0_ctrl,
    .p_cfg         = &g_rm_freertos_plus_fat0_cfg,
    .p_api         = &g_fat_on_freertos,
};

ioport_instance_ctrl_t g_ioport_ctrl;
const ioport_instance_t g_ioport =
        {
            .p_api = &g_ioport_on_ioport,
            .p_ctrl = &g_ioport_ctrl,
            .p_cfg = &g_bsp_pin_cfg,
        };


void g_common_init(void) {
}

/* generated common header file - do not edit */
#ifndef COMMON_DATA_H_
#define COMMON_DATA_H_
#include <stdint.h>
#include "bsp_api.h"
#include "r_dtc.h"
#include "r_transfer_api.h"
#include "r_sdhi.h"
#include "r_sdmmc_api.h"
#include "rm_block_media_sdmmc.h"
#include "rm_block_media_api.h"
#include "ff_headers.h"
#include "rm_freertos_plus_fat_api.h"
#include "rm_freertos_plus_fat.h"
#include "ff_headers.h"
#include "r_ioport.h"
#include "bsp_pin_cfg.h"
FSP_HEADER
/* Transfer on DTC Instance. */
extern const transfer_instance_t g_transfer0;

/** Access the DTC instance using these structures when calling API functions directly (::p_api is not used). */
extern dtc_instance_ctrl_t g_transfer0_ctrl;
extern const transfer_cfg_t g_transfer0_cfg;
/** SDMMC on SDMMC Instance. */
extern const sdmmc_instance_t g_sdhi;


/** Access the SDMMC instance using these structures when calling API functions directly (::p_api is not used). */
extern sdhi_instance_ctrl_t g_sdhi_ctrl;
extern sdmmc_cfg_t g_sdhi_cfg;

#ifndef rm_block_media_sdmmc_callback
void rm_block_media_sdmmc_callback(sdmmc_callback_args_t * p_args);
#endif
extern const rm_block_media_instance_t g_rm_block_media0;

/** Access the SDMMC instance using these structures when calling API functions directly (::p_api is not used). */
extern rm_block_media_sdmmc_instance_ctrl_t g_rm_block_media0_ctrl;
extern const rm_block_media_cfg_t g_rm_block_media0_cfg;

#ifndef rm_freertos_plus_fat_memory_callback
void rm_freertos_plus_fat_memory_callback(rm_block_media_callback_args_t * p_args);
#endif
/** fat on freertos Instance. */
extern const rm_freertos_plus_fat_instance_t g_rm_freertos_plus_fat0;
extern rm_freertos_plus_fat_instance_ctrl_t g_rm_freertos_plus_fat0_ctrl;
extern const rm_freertos_plus_fat_cfg_t g_rm_freertos_plus_fat0_cfg;
extern rm_freertos_plus_fat_disk_cfg_t g_rm_freertos_plus_fat0_disk_cfg;

#ifndef NULL
void NULL(rm_freertos_plus_fat_callback_args_t * p_args);
#endif

#define IOPORT_CFG_NAME g_bsp_pin_cfg
#define IOPORT_CFG_OPEN R_IOPORT_Open
#define IOPORT_CFG_CTRL g_ioport_ctrl

/* IOPORT Instance */
extern const ioport_instance_t g_ioport;

/* IOPORT control structure. */
extern ioport_instance_ctrl_t g_ioport_ctrl;
void g_common_init(void);
FSP_FOOTER
#endif /* COMMON_DATA_H_ */

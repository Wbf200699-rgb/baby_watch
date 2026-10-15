#include "thread_code/sd_card_driver.h"

#include <stdio.h>
#include <string.h>

#include "ff_error.h"
#include "ff_ioman.h"
#include "ff_stdio.h"
#include "ff_sys.h"
#include "thread_code/sd_ipc_shared.h"

typedef struct st_sd_card_context
{
    bool opened;
    bool media_initialized;
    bool disk_initialized;
    volatile bool mounted;
    volatile bool validated;
    bool fs_registered;
    sd_card_info_t info;
    rm_freertos_plus_fat_device_t device;
    rm_freertos_plus_fat_info_t fat_info;
    FF_Disk_t disk;
} sd_card_context_t;

BSP_DONT_REMOVE volatile const uint32_t g_sd_card_build_marker = 0x26080201U;
volatile sd_card_debug_state_t g_sd_card_debug;
static sd_card_context_t g_sd_card_context;
static volatile bool g_sd_card_recovery_requested;
static StaticSemaphore_t g_sd_card_fs_mutex_storage;
static SemaphoreHandle_t g_sd_card_fs_mutex;
static FF_FILE * gp_sd_card_debug_export_file;
static uint8_t g_sd_card_debug_export_buffer[SD_CARD_DEBUG_EXPORT_BUFFER_BYTES]
    BSP_ALIGN_VARIABLE(32);

_Static_assert(sizeof(sd_card_debug_export_t) == SD_CARD_DEBUG_EXPORT_REGION_BYTES,
               "SD debug export mailbox must fit the reserved shared SRAM region");
_Static_assert((SD_IPC_ADDRESS + sizeof(sd_ipc_mailbox_t)) <= SD_CARD_DEBUG_EXPORT_ADDRESS,
               "SD debug export mailbox overlaps the SD status mailbox");
_Static_assert((SD_CARD_DEBUG_EXPORT_ADDRESS + SD_CARD_DEBUG_EXPORT_REGION_BYTES) <=
               0x221D4000UL,
               "SD debug export mailbox exceeds shared SRAM");

#define SD_CARD_BUS_PULLUP_CFG ((uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT | \
                                (uint32_t) IOPORT_CFG_PULLUP_ENABLE)
#define SD_CARD_CMD_IDLE_CFG   ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH | \
                                (uint32_t) IOPORT_CFG_DRIVE_HIGH)
#define SD_CARD_CLOCK_IDLE_CFG ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW | \
                                (uint32_t) IOPORT_CFG_DRIVE_HIGH)
#define SD_CARD_SDHI_PIN_CFG   ((uint32_t) IOPORT_CFG_DRIVE_HIGH | \
                                (uint32_t) IOPORT_CFG_PERIPHERAL_PIN | \
                                (uint32_t) IOPORT_PERIPHERAL_SDHI_MMC)

static fsp_err_t sd_card_driver_precondition_bus(void)
{
    static const bsp_io_port_pin_t data_pins[] =
    {
        BSP_IO_PORT_01_PIN_11, /* DAT3 */
        BSP_IO_PORT_13_PIN_01, /* DAT2 */
        BSP_IO_PORT_13_PIN_02, /* DAT1 */
        BSP_IO_PORT_13_PIN_03, /* DAT0 */
    };
    fsp_err_t err;

    /* MCU reset does not remove power from the card or the TXB0108. Hold CLK
     * low and establish the SD idle levels before every guarded SDHI open so
     * a warm reset cannot inherit a translator/card line latch. */
    err = R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_13_PIN_05, SD_CARD_CLOCK_IDLE_CFG);
    FSP_ERROR_RETURN(FSP_SUCCESS == err, err);
    err = R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_13_PIN_04, SD_CARD_CMD_IDLE_CFG);
    FSP_ERROR_RETURN(FSP_SUCCESS == err, err);

    for (uint32_t i = 0U; i < (sizeof(data_pins) / sizeof(data_pins[0])); i++)
    {
        err = R_IOPORT_PinCfg(&g_ioport_ctrl, data_pins[i], SD_CARD_BUS_PULLUP_CFG);
        FSP_ERROR_RETURN(FSP_SUCCESS == err, err);
    }

    /* Supply the card's required idle clocks while CMD is actively high. This
     * also establishes the direction of the auto-sensing TXB0108 before the
     * SDHI peripheral starts CMD0/ACMD41 negotiation. */
    for (uint32_t clock = 0U; clock < 80U; clock++)
    {
        err = R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_13_PIN_05, BSP_IO_LEVEL_HIGH);
        FSP_ERROR_RETURN(FSP_SUCCESS == err, err);
        R_BSP_SoftwareDelay(2U, BSP_DELAY_UNITS_MICROSECONDS);
        err = R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_13_PIN_05, BSP_IO_LEVEL_LOW);
        FSP_ERROR_RETURN(FSP_SUCCESS == err, err);
        R_BSP_SoftwareDelay(2U, BSP_DELAY_UNITS_MICROSECONDS);
    }

    for (uint32_t i = 0U; i < (sizeof(data_pins) / sizeof(data_pins[0])); i++)
    {
        err = R_IOPORT_PinCfg(&g_ioport_ctrl, data_pins[i], SD_CARD_SDHI_PIN_CFG);
        FSP_ERROR_RETURN(FSP_SUCCESS == err, err);
    }

    err = R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_13_PIN_04, SD_CARD_SDHI_PIN_CFG);
    FSP_ERROR_RETURN(FSP_SUCCESS == err, err);

    /* Restore CLK last so CMD and DAT are already at valid idle levels. */
    err = R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_13_PIN_05, SD_CARD_SDHI_PIN_CFG);
    FSP_ERROR_RETURN(FSP_SUCCESS == err, err);
    vTaskDelay(pdMS_TO_TICKS(1U));

    return FSP_SUCCESS;
}

static uint32_t sd_card_driver_flags(void)
{
    uint32_t flags = 0U;

    if (g_sd_card_context.opened)
    {
        flags |= SD_IPC_FLAG_OPENED;
    }
    if (g_sd_card_context.media_initialized)
    {
        flags |= SD_IPC_FLAG_MEDIA_INITIALIZED;
    }
    if (g_sd_card_context.disk_initialized)
    {
        flags |= SD_IPC_FLAG_DISK_INITIALIZED;
    }
    if (g_sd_card_context.mounted)
    {
        flags |= SD_IPC_FLAG_MOUNTED;
    }
    if (g_sd_card_context.info.card_inserted)
    {
        flags |= SD_IPC_FLAG_CARD_INSERTED;
    }
    if (g_sd_card_context.fs_registered)
    {
        flags |= SD_IPC_FLAG_FS_REGISTERED;
    }

    return flags;
}

static void sd_card_driver_publish_ipc(void)
{
    volatile sd_ipc_mailbox_t * p_sd = SD_IPC_MAILBOX;
    uint32_t sequence = (SD_IPC_MAGIC == p_sd->magic) ? p_sd->sequence : 0U;

    p_sd->magic = 0U;
    p_sd->version = SD_IPC_VERSION;
    p_sd->ready = (g_sd_card_context.mounted && g_sd_card_context.validated) ? 1U : 0U;
    p_sd->step = (uint32_t) g_sd_card_debug.step;
    p_sd->failure_step = (uint32_t) g_sd_card_debug.failure_step;
    p_sd->failure_line = g_sd_card_debug.failure_line;
    p_sd->flags = sd_card_driver_flags();
    p_sd->init_attempts = g_sd_card_debug.init_attempts;
    p_sd->consecutive_failures = g_sd_card_debug.consecutive_failures;
    p_sd->recovery_count = g_sd_card_debug.recovery_count;
    p_sd->last_success_tick = g_sd_card_debug.last_success_tick;
    p_sd->init_err = (uint32_t) g_sd_card_debug.init_err;
    p_sd->last_fsp_err = (uint32_t) g_sd_card_debug.last_fsp_err;
    p_sd->last_ff_err = (uint32_t) g_sd_card_debug.last_ff_err;
    p_sd->close_err = (uint32_t) g_sd_card_debug.close_err;
    p_sd->disk_deinit_err = (uint32_t) g_sd_card_debug.disk_deinit_err;
    p_sd->unmount_err = (uint32_t) g_sd_card_debug.unmount_err;
    p_sd->sequence = sequence + 1U;
    __DMB();
    p_sd->magic = SD_IPC_MAGIC;
}

static void sd_card_driver_debug_export_init(void)
{
    volatile sd_card_debug_export_t * p_export = SD_CARD_DEBUG_EXPORT;

    memset((void *) p_export, 0, sizeof(*p_export));
    p_export->version = SD_CARD_DEBUG_EXPORT_VERSION;
    p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_IDLE;
    p_export->buffer_address = (uint32_t) &g_sd_card_debug_export_buffer[0];
    p_export->buffer_capacity = sizeof(g_sd_card_debug_export_buffer);
    __DMB();
    p_export->magic = SD_CARD_DEBUG_EXPORT_MAGIC;
}

static bool sd_card_driver_debug_export_path_valid(char const * p_path)
{
    size_t const mount_length = strlen(SD_CARD_MOUNT_PATH);

    return (0 == strncmp(p_path, SD_CARD_MOUNT_PATH, mount_length)) &&
           ('/' == p_path[mount_length]) &&
           ('\0' != p_path[mount_length + 1U]);
}

static uint32_t sd_card_driver_debug_export_checksum(uint8_t const * p_data,
                                                     uint32_t length)
{
    uint32_t checksum = 2166136261U;

    for (uint32_t i = 0U; i < length; i++)
    {
        checksum ^= p_data[i];
        checksum *= 16777619U;
    }

    return checksum;
}

static int sd_card_driver_debug_export_close(void)
{
    int close_result = 0;

    if (NULL != gp_sd_card_debug_export_file)
    {
        close_result = ff_fclose(gp_sd_card_debug_export_file);
        gp_sd_card_debug_export_file = NULL;
    }

    return close_result;
}

static void sd_card_driver_debug_record(sd_card_debug_step_t step, uint32_t line)
{
    g_sd_card_debug.magic = 0x53444344U;
    g_sd_card_debug.build_marker = g_sd_card_build_marker;
    g_sd_card_debug.step = step;
    g_sd_card_debug.line = line;
    g_sd_card_debug.info.opened = g_sd_card_context.info.opened;
    g_sd_card_debug.info.media_initialized = g_sd_card_context.info.media_initialized;
    g_sd_card_debug.info.disk_initialized = g_sd_card_context.info.disk_initialized;
    g_sd_card_debug.info.mounted = g_sd_card_context.info.mounted;
    g_sd_card_debug.info.card_inserted = g_sd_card_context.info.card_inserted;
    g_sd_card_debug.info.sector_count = g_sd_card_context.info.sector_count;
    g_sd_card_debug.info.sector_size_bytes = g_sd_card_context.info.sector_size_bytes;
    g_sd_card_debug.info.free_sectors = g_sd_card_context.info.free_sectors;
    g_sd_card_debug.info.total_sectors = g_sd_card_context.info.total_sectors;
    g_sd_card_debug.info.last_fsp_err = g_sd_card_context.info.last_fsp_err;
    g_sd_card_debug.info.last_ff_err = g_sd_card_context.info.last_ff_err;
    sd_card_driver_publish_ipc();
}

static void sd_card_driver_record_failure(sd_card_debug_step_t step,
                                          uint32_t line,
                                          fsp_err_t fsp_err,
                                          FF_Error_t ff_err)
{
    g_sd_card_debug.failure_step = step;
    g_sd_card_debug.failure_line = line;
    g_sd_card_debug.init_err = fsp_err;
    g_sd_card_debug.last_fsp_err = fsp_err;
    g_sd_card_debug.last_ff_err = ff_err;
    g_sd_card_debug.consecutive_failures++;
    g_sd_card_context.info.last_fsp_err = fsp_err;
    g_sd_card_context.info.last_ff_err = ff_err;
    sd_card_driver_debug_record(step, line);
}

static void sd_card_driver_reset_stack(void)
{
    fsp_err_t disk_deinit_err = FSP_SUCCESS;
    fsp_err_t close_err = FSP_SUCCESS;
    FF_Error_t unmount_err = FF_ERR_NONE;
    fsp_err_t failure_err = g_sd_card_debug.last_fsp_err;
    FF_Error_t failure_ff_err = g_sd_card_debug.last_ff_err;
    bool const was_opened = g_sd_card_context.opened;
    bool const was_registered = g_sd_card_context.fs_registered;
    bool const had_disk = g_sd_card_context.disk_initialized ||
                          (NULL != g_sd_card_context.disk.pxIOManager);

    /* Make all writers close their files before the next polling pass.  Status
     * failures are invalidated one second before this function is called. */
    g_sd_card_context.mounted = false;
    g_sd_card_context.validated = false;
    g_sd_card_context.info.mounted = false;
    __DMB();

    (void) sd_card_driver_debug_export_close();

    /* BSP parameter checking is disabled in this build.  Every teardown call
     * therefore has to be guarded by application-owned state; calling close()
     * on a zeroed FSP control block dereferences a NULL p_cfg and HardFaults
     * CPU1. */
    if (was_registered)
    {
        FF_FS_Remove(SD_CARD_MOUNT_PATH);
    }
    g_sd_card_context.fs_registered = false;

    if (was_opened && had_disk && (NULL != g_sd_card_context.disk.pxIOManager))
    {
        if (FF_Mounted(g_sd_card_context.disk.pxIOManager))
        {
            unmount_err = FF_Unmount(&g_sd_card_context.disk);
        }

        disk_deinit_err = g_rm_freertos_plus_fat0.p_api->diskDeinit(g_rm_freertos_plus_fat0.p_ctrl,
                                                                    &g_sd_card_context.disk);
    }

    /* Closing the top wrapper closes block-media and SDHI as well.  Never call
     * the lower layers separately: with parameter checking disabled their
     * zeroed control blocks are not safe to close. */
    if (was_opened)
    {
        close_err = g_rm_freertos_plus_fat0.p_api->close(g_rm_freertos_plus_fat0.p_ctrl);
    }

    memset(&g_sd_card_context, 0, sizeof(g_sd_card_context));
    g_sd_card_context.info.last_fsp_err = failure_err;
    g_sd_card_context.info.last_ff_err = failure_ff_err;
    g_sd_card_debug.recovery_count++;
    g_sd_card_debug.close_err = close_err;
    g_sd_card_debug.disk_deinit_err = disk_deinit_err;
    g_sd_card_debug.unmount_err = unmount_err;
    sd_card_driver_debug_record(g_sd_card_debug.failure_step, g_sd_card_debug.failure_line);
}

void sd_card_driver_prepare(void)
{
    /* CPU1 startup clears the FSP control blocks.  Initialise only our own
     * state here; touching an unopened FSP/FAT instance is unsafe because BSP
     * parameter checking is disabled.  R_SDHI_Open() performs the required
     * peripheral software reset on every guarded retry. */
    memset(&g_sd_card_context, 0, sizeof(g_sd_card_context));
    memset((void *) &g_sd_card_debug, 0, sizeof(g_sd_card_debug));
    memset((void *) SD_IPC_MAILBOX, 0, sizeof(sd_ipc_mailbox_t));
    gp_sd_card_debug_export_file = NULL;
    sd_card_driver_debug_export_init();
    g_sd_card_recovery_requested = false;
    g_sd_card_fs_mutex = xSemaphoreCreateRecursiveMutexStatic(&g_sd_card_fs_mutex_storage);

    g_sd_card_debug.magic = 0x53444344U;
    g_sd_card_debug.build_marker = g_sd_card_build_marker;
    g_sd_card_debug.step = SD_CARD_DEBUG_STEP_NONE;
    g_sd_card_debug.init_err = FSP_SUCCESS;
    g_sd_card_debug.last_fsp_err = FSP_SUCCESS;
    g_sd_card_debug.last_ff_err = FF_ERR_NONE;
    g_sd_card_debug.close_err = FSP_SUCCESS;
    g_sd_card_debug.disk_deinit_err = FSP_SUCCESS;
    g_sd_card_debug.unmount_err = FF_ERR_NONE;
    g_sd_card_debug.write_status = SD_CARD_DRIVER_STATUS_ERROR;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_NONE, __LINE__);
}

static fsp_err_t sd_card_driver_open_if_needed(void)
{
    fsp_err_t err;

    if (g_sd_card_context.opened)
    {
        return FSP_SUCCESS;
    }

    err = g_rm_freertos_plus_fat0.p_api->open(g_rm_freertos_plus_fat0.p_ctrl,
                                               g_rm_freertos_plus_fat0.p_cfg);
    g_sd_card_debug.last_fsp_err = err;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_OPEN, __LINE__);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        g_sd_card_context.info.last_fsp_err = err;
        return err;
    }

    g_sd_card_context.opened = true;
    g_sd_card_context.info.opened = true;
    g_sd_card_context.info.last_fsp_err = FSP_SUCCESS;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_OPEN, __LINE__);

    return FSP_SUCCESS;
}

static fsp_err_t sd_card_driver_update_fat_info(void)
{
    FF_IOManager_t * p_iomanager;
    uint64_t free_sectors;

    if (!g_sd_card_context.disk_initialized ||
        (NULL == g_sd_card_context.disk.pxIOManager))
    {
        return FSP_ERR_NOT_OPEN;
    }

    /* FSP infoGet() calls FF_GetFreeSize(). With ffconfigMOUNT_FIND_FREE=0,
     * that scans the complete FAT while holding its lock. On a large card this
     * can block all log/media writers for minutes, so only publish the volume
     * metadata already obtained by FF_Mount(). A zero free count means
     * "not scanned"; free space is not needed by any runtime decision. */
    p_iomanager = g_sd_card_context.disk.pxIOManager;
    free_sectors = (uint64_t) p_iomanager->xPartition.ulFreeClusterCount *
                   (uint64_t) p_iomanager->xPartition.ulSectorsPerCluster;
    g_sd_card_context.fat_info.type =
        (rm_freertos_plus_fat_type_t) p_iomanager->xPartition.ucType;
    g_sd_card_context.fat_info.sector_size = p_iomanager->usSectorSize;
    g_sd_card_context.fat_info.sectors_per_cluster =
        p_iomanager->xPartition.ulSectorsPerCluster;
    g_sd_card_context.fat_info.total_sectors =
        p_iomanager->xPartition.ulTotalSectors;
    g_sd_card_context.fat_info.free_sectors = (uint32_t) free_sectors;
    g_sd_card_context.fat_info.p_volume_label =
        (uint8_t *) p_iomanager->xPartition.pcVolumeLabel;
    g_sd_card_context.fat_info.partition_number =
        g_sd_card_context.disk.xStatus.bPartitionNumber;

    g_sd_card_context.info.free_sectors = g_sd_card_context.fat_info.free_sectors;
    g_sd_card_context.info.total_sectors = g_sd_card_context.fat_info.total_sectors;
    g_sd_card_context.info.last_fsp_err = FSP_SUCCESS;
    g_sd_card_debug.last_fsp_err = FSP_SUCCESS;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_INFO_GET, __LINE__);

    return FSP_SUCCESS;
}

fsp_err_t sd_card_driver_init(void)
{
    fsp_err_t err;

    g_sd_card_debug.init_attempts++;
    g_sd_card_debug.init_err = FSP_SUCCESS;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_NONE, __LINE__);

    /* refresh_status() intentionally leaves the old objects intact for one
     * scheduler period so file writers can observe ready=false and close. */
    if (g_sd_card_context.opened || g_sd_card_context.media_initialized ||
        g_sd_card_context.disk_initialized || g_sd_card_context.fs_registered ||
        (NULL != g_sd_card_context.disk.pxIOManager))
    {
        sd_card_driver_reset_stack();
    }

    err = sd_card_driver_precondition_bus();
    g_sd_card_debug.init_err = err;
    if (FSP_SUCCESS != err)
    {
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_OPEN, __LINE__, err, FF_ERR_NONE);
        return err;
    }

    err = sd_card_driver_open_if_needed();
    g_sd_card_debug.init_err = err;
    if (FSP_SUCCESS != err)
    {
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_OPEN, __LINE__, err, FF_ERR_NONE);
        sd_card_driver_reset_stack();
        return err;
    }

    err = g_rm_freertos_plus_fat0.p_api->mediaInit(g_rm_freertos_plus_fat0.p_ctrl,
                                                    &g_sd_card_context.device);
    g_sd_card_context.info.last_fsp_err = err;
    g_sd_card_debug.init_err = err;
    g_sd_card_debug.last_fsp_err = err;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_MEDIA_INIT, __LINE__);
    if (FSP_SUCCESS != err)
    {
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_MEDIA_INIT, __LINE__, err, FF_ERR_NONE);
        sd_card_driver_reset_stack();
        return err;
    }

    g_sd_card_context.media_initialized = true;
    g_sd_card_context.info.media_initialized = true;
    g_sd_card_context.info.card_inserted = true;
    g_sd_card_context.info.sector_count = g_sd_card_context.device.sector_count;
    g_sd_card_context.info.sector_size_bytes = g_sd_card_context.device.sector_size_bytes;

    /* Do not keep the generated card's capacity.  This also allows a card of a
     * different size to be inserted without regenerating FSP configuration. */
    g_rm_freertos_plus_fat0_disk_cfg.device.sector_count = g_sd_card_context.device.sector_count;
    g_rm_freertos_plus_fat0_disk_cfg.device.sector_size_bytes = g_sd_card_context.device.sector_size_bytes;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_MEDIA_INIT, __LINE__);

    err = g_rm_freertos_plus_fat0.p_api->diskInit(g_rm_freertos_plus_fat0.p_ctrl,
                                                  &g_rm_freertos_plus_fat0_disk_cfg,
                                                  &g_sd_card_context.disk);
    g_sd_card_context.info.last_fsp_err = err;
    g_sd_card_debug.init_err = err;
    g_sd_card_debug.last_fsp_err = err;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_DISK_INIT, __LINE__);
    if (FSP_SUCCESS != err)
    {
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_DISK_INIT, __LINE__, err, FF_ERR_NONE);
        sd_card_driver_reset_stack();
        return err;
    }

    g_sd_card_context.disk_initialized = true;
    g_sd_card_context.info.disk_initialized = true;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_DISK_INIT, __LINE__);

    err = sd_card_driver_mount();
    g_sd_card_debug.init_err = err;
    if (FSP_SUCCESS != err)
    {
        sd_card_driver_reset_stack();
        return err;
    }

    g_sd_card_debug.init_err = FSP_SUCCESS;
    g_sd_card_debug.last_fsp_err = FSP_SUCCESS;
    g_sd_card_debug.consecutive_failures = 0U;
    g_sd_card_debug.last_success_tick = (uint32_t) xTaskGetTickCount();
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_DONE, __LINE__);

    return FSP_SUCCESS;
}

fsp_err_t sd_card_driver_mount(void)
{
    FF_Error_t ff_err;
    int fs_add_err;
    fsp_err_t err;

    if (!g_sd_card_context.disk_initialized)
    {
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_MOUNT, __LINE__, FSP_ERR_NOT_OPEN, FF_ERR_NONE);
        return FSP_ERR_NOT_OPEN;
    }

    if (g_sd_card_context.mounted)
    {
        return sd_card_driver_update_fat_info();
    }

    ff_err = FF_Mount(&g_sd_card_context.disk, 0);
    g_sd_card_context.info.last_ff_err = ff_err;
    g_sd_card_debug.last_ff_err = ff_err;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_MOUNT, __LINE__);
    if (0U != (((uint32_t) ff_err) & FF_ERRFLAG))
    {
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_MOUNT, __LINE__, FSP_ERR_INTERNAL, ff_err);
        return FSP_ERR_INTERNAL;
    }

    fs_add_err = FF_FS_Add(SD_CARD_MOUNT_PATH, &g_sd_card_context.disk);
    g_sd_card_debug.fs_add_err = fs_add_err;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_FS_ADD, __LINE__);
    if (pdFALSE == fs_add_err)
    {
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_FS_ADD, __LINE__, FSP_ERR_INTERNAL, FF_ERR_NONE);
        return FSP_ERR_INTERNAL;
    }

    g_sd_card_context.fs_registered = true;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_FS_ADD, __LINE__);

    err = sd_card_driver_update_fat_info();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* Mounting alone is not enough to publish ready. The SD task validates a
     * real open/write/close cycle before any media or logger task enters FAT. */
    g_sd_card_context.mounted = true;
    g_sd_card_context.validated = false;
    g_sd_card_context.info.mounted = false;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_INFO_GET, __LINE__);

    return FSP_SUCCESS;
}

fsp_err_t sd_card_driver_refresh_status(void)
{
    rm_block_media_status_t status;
    fsp_err_t err;

    if (g_sd_card_recovery_requested)
    {
        g_sd_card_recovery_requested = false;
        g_sd_card_context.mounted = false;
        g_sd_card_context.validated = false;
        g_sd_card_context.info.mounted = false;
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_RUNTIME_IO,
                                      __LINE__,
                                      FSP_ERR_WRITE_FAILED,
                                      g_sd_card_context.info.last_ff_err);
        return FSP_ERR_WRITE_FAILED;
    }

    memset(&status, 0, sizeof(status));
    err = g_rm_block_media0.p_api->statusGet(g_rm_block_media0.p_ctrl, &status);
    g_sd_card_context.info.last_fsp_err = err;
    g_sd_card_debug.last_fsp_err = err;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_STATUS_GET, __LINE__);
    if (FSP_SUCCESS != err)
    {
        g_sd_card_context.mounted = false;
        g_sd_card_context.validated = false;
        g_sd_card_context.info.mounted = false;
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_STATUS_GET, __LINE__, err,
                                      g_sd_card_context.info.last_ff_err);
        return err;
    }

    g_sd_card_context.info.card_inserted = status.media_inserted;
    if (!status.media_inserted)
    {
        g_sd_card_context.mounted = false;
        g_sd_card_context.validated = false;
        g_sd_card_context.info.mounted = false;
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_STATUS_GET, __LINE__,
                                      FSP_ERR_CARD_NOT_INSERTED, g_sd_card_context.info.last_ff_err);
        return FSP_ERR_CARD_NOT_INSERTED;
    }

    if (!status.initialized)
    {
        g_sd_card_context.mounted = false;
        g_sd_card_context.validated = false;
        g_sd_card_context.info.mounted = false;
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_STATUS_GET, __LINE__,
                                      FSP_ERR_CARD_NOT_INITIALIZED, g_sd_card_context.info.last_ff_err);
        return FSP_ERR_CARD_NOT_INITIALIZED;
    }

    g_sd_card_context.info.last_fsp_err = FSP_SUCCESS;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_STATUS_GET, __LINE__);

    return FSP_SUCCESS;
}

bool sd_card_driver_is_ready(void)
{
    return g_sd_card_context.mounted && g_sd_card_context.validated;
}

void sd_card_driver_get_info(sd_card_info_t * p_info)
{
    if (NULL == p_info)
    {
        return;
    }

    *p_info = g_sd_card_context.info;
}

sd_card_driver_status_t sd_card_driver_write_test_file(void)
{
    FF_FILE * p_file;
    int write_result;
    int close_result;
    char test_line[96];

    if (!g_sd_card_context.mounted)
    {
        g_sd_card_debug.write_status = SD_CARD_DRIVER_STATUS_ERROR;
        sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_FILE_OPEN, __LINE__);
        return SD_CARD_DRIVER_STATUS_ERROR;
    }

    p_file = ff_fopen(SD_CARD_TEST_FILE_PATH, "a+");
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_FILE_OPEN, __LINE__);
    if (NULL == p_file)
    {
        g_sd_card_debug.write_status = SD_CARD_DRIVER_STATUS_ERROR;
        g_sd_card_context.mounted = false;
        g_sd_card_context.validated = false;
        g_sd_card_context.info.mounted = false;
        sd_card_driver_record_failure(SD_CARD_DEBUG_STEP_FILE_OPEN, __LINE__,
                                      FSP_ERR_MEDIA_OPEN_FAILED, g_sd_card_context.info.last_ff_err);
        return SD_CARD_DRIVER_STATUS_ERROR;
    }

    snprintf(test_line,
             sizeof(test_line),
             "sd test tick=%lu sectors=%lu\r\n",
             (unsigned long) xTaskGetTickCount(),
             (unsigned long) g_sd_card_context.info.sector_count);

    write_result = ff_fprintf(p_file, "%s", test_line);
    g_sd_card_debug.write_result = write_result;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_FILE_WRITE, __LINE__);
    close_result = ff_fclose(p_file);
    g_sd_card_debug.close_result = close_result;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_FILE_CLOSE, __LINE__);

    if ((write_result < 0) || (0 != close_result))
    {
        sd_card_debug_step_t failure_step = (write_result < 0) ? SD_CARD_DEBUG_STEP_FILE_WRITE :
                                                                SD_CARD_DEBUG_STEP_FILE_CLOSE;
        g_sd_card_debug.write_status = SD_CARD_DRIVER_STATUS_ERROR;
        g_sd_card_context.mounted = false;
        g_sd_card_context.validated = false;
        g_sd_card_context.info.mounted = false;
        sd_card_driver_record_failure(failure_step, __LINE__, FSP_ERR_WRITE_FAILED,
                                      g_sd_card_context.info.last_ff_err);
        return SD_CARD_DRIVER_STATUS_ERROR;
    }

    g_sd_card_context.validated = true;
    g_sd_card_context.info.mounted = true;
    g_sd_card_debug.write_status = SD_CARD_DRIVER_STATUS_OK;
    sd_card_driver_debug_record(SD_CARD_DEBUG_STEP_DONE, __LINE__);

    return SD_CARD_DRIVER_STATUS_OK;
}

void sd_card_driver_report_io_error(int io_errno)
{
    if ((pdFREERTOS_ERRNO_EIO != io_errno) &&
        (pdFREERTOS_ERRNO_ENXIO != io_errno) &&
        (pdFREERTOS_ERRNO_ENODEV != io_errno))
    {
        return;
    }

    /* Defer teardown to the SD task. It first publishes ready=false so file
     * owners can close handles before the stack is reinitialised. */
    g_sd_card_recovery_requested = true;
    __DMB();
}

bool sd_card_driver_fs_lock(TickType_t timeout_ticks)
{
    if (NULL == g_sd_card_fs_mutex)
    {
        return false;
    }

    return pdTRUE == xSemaphoreTakeRecursive(g_sd_card_fs_mutex, timeout_ticks);
}

void sd_card_driver_fs_unlock(void)
{
    if (NULL != g_sd_card_fs_mutex)
    {
        (void) xSemaphoreGiveRecursive(g_sd_card_fs_mutex);
    }
}

void sd_card_driver_debug_export_poll(bool sd_ready)
{
    volatile sd_card_debug_export_t * p_export = SD_CARD_DEBUG_EXPORT;
    uint32_t const request_sequence = p_export->request_sequence;
    uint32_t const operation = p_export->operation;

    if ((SD_CARD_DEBUG_EXPORT_MAGIC != p_export->magic) ||
        (SD_CARD_DEBUG_EXPORT_VERSION != p_export->version))
    {
        (void) sd_card_driver_debug_export_close();
        sd_card_driver_debug_export_init();
        return;
    }

    if (request_sequence == p_export->response_sequence)
    {
        return;
    }

    p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_BUSY;
    p_export->response_offset = 0U;
    p_export->bytes_ready = 0U;
    p_export->chunk_checksum = 0U;
    p_export->last_error = 0U;
    __DMB();

    if (!sd_ready)
    {
        (void) sd_card_driver_debug_export_close();
        p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_NOT_READY;
    }
    else if (SD_CARD_DEBUG_EXPORT_OPERATION_OPEN == operation)
    {
        char path[SD_CARD_DEBUG_EXPORT_PATH_BYTES];
        long file_size;

        for (uint32_t i = 0U; i < sizeof(path); i++)
        {
            path[i] = p_export->path[i];
        }
        path[sizeof(path) - 1U] = '\0';

        (void) sd_card_driver_debug_export_close();
        p_export->file_size = 0U;
        if (!sd_card_driver_debug_export_path_valid(path))
        {
            p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_BAD_REQUEST;
        }
        else
        {
            gp_sd_card_debug_export_file = ff_fopen(path, "rb");
            if (NULL == gp_sd_card_debug_export_file)
            {
                p_export->last_error = (uint32_t) stdioGET_ERRNO();
                p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_OPEN_FAILED;
            }
            else if (0 != ff_fseek(gp_sd_card_debug_export_file, 0L, FF_SEEK_END))
            {
                p_export->last_error = (uint32_t) stdioGET_ERRNO();
                (void) sd_card_driver_debug_export_close();
                p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_SEEK_FAILED;
            }
            else
            {
                file_size = ff_ftell(gp_sd_card_debug_export_file);
                if ((file_size < 0L) ||
                    (0 != ff_fseek(gp_sd_card_debug_export_file, 0L, FF_SEEK_SET)))
                {
                    p_export->last_error = (uint32_t) stdioGET_ERRNO();
                    (void) sd_card_driver_debug_export_close();
                    p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_SEEK_FAILED;
                }
                else
                {
                    p_export->file_size = (uint32_t) file_size;
                    p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_OK;
                }
            }
        }
    }
    else if (SD_CARD_DEBUG_EXPORT_OPERATION_READ == operation)
    {
        uint32_t const offset = p_export->request_offset;
        uint32_t const requested = p_export->request_length;

        if (NULL == gp_sd_card_debug_export_file)
        {
            p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_NOT_OPEN;
        }
        else if ((0U == requested) ||
                 (requested > sizeof(g_sd_card_debug_export_buffer)) ||
                 (offset > p_export->file_size) ||
                 (requested > (p_export->file_size - offset)))
        {
            p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_BAD_REQUEST;
        }
        else if (0 != ff_fseek(gp_sd_card_debug_export_file, (long) offset, FF_SEEK_SET))
        {
            p_export->last_error = (uint32_t) stdioGET_ERRNO();
            p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_SEEK_FAILED;
        }
        else
        {
            size_t const bytes_read = ff_fread(g_sd_card_debug_export_buffer,
                                               1U,
                                               requested,
                                               gp_sd_card_debug_export_file);
            p_export->response_offset = offset;
            p_export->bytes_ready = (uint32_t) bytes_read;
            if (bytes_read != requested)
            {
                p_export->last_error = (uint32_t) stdioGET_ERRNO();
                p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_READ_FAILED;
            }
            else
            {
                p_export->chunk_checksum =
                    sd_card_driver_debug_export_checksum(g_sd_card_debug_export_buffer,
                                                         requested);
                __DMB();
                p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_OK;
            }
        }
    }
    else if (SD_CARD_DEBUG_EXPORT_OPERATION_CLOSE == operation)
    {
        p_export->last_error = (uint32_t) sd_card_driver_debug_export_close();
        p_export->status = (0U == p_export->last_error) ?
                           SD_CARD_DEBUG_EXPORT_STATUS_OK :
                           SD_CARD_DEBUG_EXPORT_STATUS_CLOSE_FAILED;
    }
    else
    {
        p_export->status = SD_CARD_DEBUG_EXPORT_STATUS_BAD_REQUEST;
    }

    __DMB();
    p_export->response_sequence = request_sequence;
}

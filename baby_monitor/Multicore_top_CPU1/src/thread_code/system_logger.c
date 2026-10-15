#include "thread_code/system_logger.h"

#include <stdio.h>
#include <string.h>

#include "ff_stdio.h"
#include "thread_code/audio_wav_player.h"
#include "thread_code/camera_usb_ipc_shared.h"
#include "thread_code/pressure_ipc_shared.h"
#include "thread_code/scd41_ipc_shared.h"
#include "thread_code/serial_camera_ipc_shared.h"
#include "thread_code/thermal_ipc_shared.h"
#include "thread_code/tmp117_ipc_shared.h"

#define SYSTEM_LOGGER_MAGIC       (0x4C4F4731U) /* LOG1 */
#define SYSTEM_LOGGER_BUILD       (0x20260731U)
#define SYSTEM_LOGGER_ENABLE      (1U)
#define SYSTEM_LOGGER_ROOT_ERROR_PATH SD_CARD_MOUNT_PATH "/LOGGER_ERR.TXT"
#define SYSTEM_LOGGER_MAX_RUN_ID  (999999U)

BSP_DONT_REMOVE volatile const uint32_t g_system_logger_build_marker = SYSTEM_LOGGER_BUILD;
volatile system_logger_debug_t g_system_logger_debug;

static bool g_system_logger_initialized;
static char g_system_logger_path[SYSTEM_LOGGER_PATH_LENGTH];
static scd41_ipc_snapshot_t g_system_logger_scd41_snapshot;

static int system_logger_write_boot_header(FF_FILE * p_file, sd_card_info_t const * p_sd_info);
static int system_logger_write_csv_header(FF_FILE * p_file);
static int system_logger_write_csv_row(FF_FILE * p_file, sd_card_info_t const * p_sd_info);
static void system_logger_write_error_file(char const * p_reason);

static int system_logger_fs_error(void)
{
    int error = stdioGET_ERRNO();

    sd_card_driver_report_io_error(error);
    return error;
}

static bool system_logger_path_exists(char const * p_path)
{
    FF_FILE * p_file = ff_fopen(p_path, "r");
    if (NULL == p_file)
    {
        (void) system_logger_fs_error();
        return false;
    }

    (void) ff_fclose(p_file);
    return true;
}

static void system_logger_debug_set_path(char const * p_path)
{
    size_t i;

    for (i = 0U; i < sizeof(g_system_logger_debug.log_path) - 1U; i++)
    {
        g_system_logger_debug.log_path[i] = p_path[i];
        if ('\0' == p_path[i])
        {
            return;
        }
    }

    g_system_logger_debug.log_path[i] = '\0';
}

static bool system_logger_create_run_file(sd_card_info_t const * p_sd_info)
{
    FF_FILE * p_file;
    uint32_t run_index;

    for (run_index = 1U; run_index <= SYSTEM_LOGGER_MAX_RUN_ID; run_index++)
    {
        (void) snprintf(g_system_logger_path,
                        sizeof(g_system_logger_path),
                        SD_CARD_MOUNT_PATH "/R%06lu.CSV",
                        (unsigned long) run_index);
        system_logger_debug_set_path(g_system_logger_path);

        if (!system_logger_path_exists(g_system_logger_path))
        {
            break;
        }
    }

    if (run_index > SYSTEM_LOGGER_MAX_RUN_ID)
    {
        g_system_logger_debug.status = SYSTEM_LOGGER_STATUS_FILE_ERROR;
        return false;
    }

    p_file = ff_fopen(g_system_logger_path, "w");
    if (NULL == p_file)
    {
        g_system_logger_debug.last_errno = system_logger_fs_error();
        g_system_logger_debug.open_fail_count++;
        g_system_logger_debug.status = SYSTEM_LOGGER_STATUS_FILE_ERROR;
        system_logger_write_error_file("run file open failed");
        return false;
    }

    g_system_logger_debug.run_index = run_index;
    g_system_logger_debug.last_write_result = system_logger_write_boot_header(p_file, p_sd_info);
    if (g_system_logger_debug.last_write_result >= 0)
    {
        g_system_logger_debug.last_write_result = system_logger_write_csv_header(p_file);
    }
    if (g_system_logger_debug.last_write_result < 0)
    {
        g_system_logger_debug.last_errno = system_logger_fs_error();
    }

    g_system_logger_debug.last_close_result = ff_fclose(p_file);
    if (0 != g_system_logger_debug.last_close_result)
    {
        g_system_logger_debug.last_errno = system_logger_fs_error();
    }
    if ((g_system_logger_debug.last_write_result < 0) || (0 != g_system_logger_debug.last_close_result))
    {
        if (g_system_logger_debug.last_write_result < 0)
        {
            g_system_logger_debug.write_fail_count++;
        }

        if (0 != g_system_logger_debug.last_close_result)
        {
            g_system_logger_debug.close_fail_count++;
        }

        g_system_logger_debug.status = SYSTEM_LOGGER_STATUS_WRITE_ERROR;
        return false;
    }

    return true;
}

void system_logger_init(sd_card_info_t const * p_sd_info)
{
#if !SYSTEM_LOGGER_ENABLE
    FSP_PARAMETER_NOT_USED(p_sd_info);
    memset((void *) &g_system_logger_debug, 0, sizeof(g_system_logger_debug));
    g_system_logger_debug.magic = SYSTEM_LOGGER_MAGIC;
    g_system_logger_debug.build_marker = g_system_logger_build_marker;
    g_system_logger_debug.status = SYSTEM_LOGGER_STATUS_IDLE;
    return;
#else
    if (g_system_logger_initialized)
    {
        return;
    }

    memset((void *) &g_system_logger_debug, 0, sizeof(g_system_logger_debug));
    memset(g_system_logger_path, 0, sizeof(g_system_logger_path));
    g_system_logger_debug.magic = SYSTEM_LOGGER_MAGIC;
    g_system_logger_debug.build_marker = g_system_logger_build_marker;
    g_system_logger_debug.status = SYSTEM_LOGGER_STATUS_IDLE;

    if ((NULL == p_sd_info) || (!p_sd_info->mounted))
    {
        return;
    }

    /* Write logs in the volume root. A corrupt LOGS directory can break directory enumeration. */
    g_system_logger_debug.dir_exists = pdTRUE;

    if (!system_logger_create_run_file(p_sd_info))
    {
        return;
    }

    g_system_logger_initialized = true;
    g_system_logger_debug.status = SYSTEM_LOGGER_STATUS_READY;
#endif
}

void system_logger_poll(sd_card_info_t const * p_sd_info)
{
#if !SYSTEM_LOGGER_ENABLE
    FSP_PARAMETER_NOT_USED(p_sd_info);
    return;
#else
    FF_FILE * p_file;

    if (!g_system_logger_initialized)
    {
        system_logger_init(p_sd_info);
    }

    if ((!g_system_logger_initialized) || (NULL == p_sd_info) || (!p_sd_info->mounted))
    {
        return;
    }

    p_file = ff_fopen(g_system_logger_path, "a");
    if (NULL == p_file)
    {
        g_system_logger_debug.last_errno = system_logger_fs_error();
        g_system_logger_debug.open_fail_count++;
        g_system_logger_debug.status = SYSTEM_LOGGER_STATUS_FILE_ERROR;
        system_logger_write_error_file("append open failed");
        return;
    }

    g_system_logger_debug.last_tick = xTaskGetTickCount();
    g_system_logger_debug.last_write_result = system_logger_write_csv_row(p_file, p_sd_info);
    if (g_system_logger_debug.last_write_result < 0)
    {
        g_system_logger_debug.last_errno = system_logger_fs_error();
    }
    g_system_logger_debug.last_close_result = ff_fclose(p_file);
    if (0 != g_system_logger_debug.last_close_result)
    {
        g_system_logger_debug.last_errno = system_logger_fs_error();
    }

    if ((g_system_logger_debug.last_write_result < 0) || (0 != g_system_logger_debug.last_close_result))
    {
        if (g_system_logger_debug.last_write_result < 0)
        {
            g_system_logger_debug.write_fail_count++;
        }

        if (0 != g_system_logger_debug.last_close_result)
        {
            g_system_logger_debug.close_fail_count++;
        }

        g_system_logger_debug.status = SYSTEM_LOGGER_STATUS_WRITE_ERROR;
        return;
    }

    g_system_logger_debug.line_count++;
    g_system_logger_debug.status = SYSTEM_LOGGER_STATUS_READY;
#endif
}

void system_logger_reset(void)
{
    g_system_logger_initialized = false;
    g_system_logger_path[0] = '\0';
    g_system_logger_debug.status = SYSTEM_LOGGER_STATUS_IDLE;
}

bool system_logger_is_ready(void)
{
    return g_system_logger_initialized;
}

static int system_logger_accumulate(int current_result, int write_result)
{
    if (current_result < 0)
    {
        return current_result;
    }

    return write_result;
}

static void system_logger_write_error_file(char const * p_reason)
{
    FF_FILE * p_file = ff_fopen(SYSTEM_LOGGER_ROOT_ERROR_PATH, "w");

    if (NULL == p_file)
    {
        (void) system_logger_fs_error();
        return;
    }

    (void) ff_fprintf(p_file,
                      "reason=%s\r\npath=%s\r\nstatus=%lu\r\nmkdir=%d\r\nerrno=%d\r\ndir=%d\r\nopen_fail=%lu\r\nwrite_fail=%lu\r\nclose_fail=%lu\r\n",
                      (NULL != p_reason) ? p_reason : "unknown",
                      g_system_logger_path,
                      (unsigned long) g_system_logger_debug.status,
                      g_system_logger_debug.mkdir_result,
                      g_system_logger_debug.last_errno,
                      g_system_logger_debug.dir_exists,
                      (unsigned long) g_system_logger_debug.open_fail_count,
                      (unsigned long) g_system_logger_debug.write_fail_count,
                      (unsigned long) g_system_logger_debug.close_fail_count);
    (void) ff_fclose(p_file);
}

static int system_logger_write_boot_header(FF_FILE * p_file, sd_card_info_t const * p_sd_info)
{
    int result = 0;
    TickType_t tick = xTaskGetTickCount();

    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "# boot_tick=%lu build=0x%08lx logger=0x%08lx\r\n",
                                                 (unsigned long) tick,
                                                 (unsigned long) SYSTEM_LOGGER_BUILD,
                                                 (unsigned long) g_system_logger_build_marker));
    if (NULL != p_sd_info)
    {
        result = system_logger_accumulate(result,
                                          ff_fprintf(p_file,
                                                     "# sd sectors=%lu sector_size=%lu free=%lu total=%lu\r\n",
                                                     (unsigned long) p_sd_info->sector_count,
                                                     (unsigned long) p_sd_info->sector_size_bytes,
                                                     (unsigned long) p_sd_info->free_sectors,
                                                     (unsigned long) p_sd_info->total_sectors));
    }

    return result;
}

static int system_logger_write_csv_header(FF_FILE * p_file)
{
    int result = 0;

    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "tick,sd_inserted,sd_free,sd_total,sd_fsp,sd_ff,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "thm_seq,thm_status,thm_rx,thm_min_cdeg,thm_max_cdeg,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "thm_center_cdeg,thm_avg_cdeg,thm_err,thm_head,thm_raw0,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "thm_raw1,thm_raw_center,thm_ta_raw,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "thm_valid,thm_crc_rx,thm_crc_calc,thm_uart_err,thm_ovf,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "thm_parity_err,thm_framing_err,thm_fifo_ovf,thm_crc_bad,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                  "thm_data_bad,thm_baud_set,thm_zero_words,thm_one_words,thm_ffff_words,thm_build,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "thm_fail_streak,thm_recovery,thm_reopen,thm_hard_reset,"
                                                 "thm_recovery_err,thm_recovery_reason,thm_last_good,thm_run_ready,"
                                                 "thm_power_cycle,thm_relay_ready,thm_stale,thm_same_streak,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "tmp_seq,tmp_status,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "tmp_ok_mask,tmp_err_mask,tmp_fail_step,tmp_fail_ch,tmp_fsp,tmp_pca,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "tmp0_mc,tmp0_addr,tmp1_mc,tmp1_addr,tmp2_mc,tmp2_addr,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "tmp3_mc,tmp3_addr,tmp4_mc,tmp4_addr,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "scd_seq,scd_status,scd_co2_ppm,scd_temp_mc,scd_rh_mp,scd_ready,"
                                                 "scd_crc_err,scd_i2c_err,scd_timeout,scd_recovery,scd_fsp,scd_last_good,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "bme_status,bme_addr,bme_temp_mc,bme_rh_mp,bme_pressure_pa,"
                                                 "bme_gas_ohm,bme_flags,bme_last_good,bme_err,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "prs_seq,prs_status,prs_ok_mask,prs_err_mask,prs_fail_step,prs_fail_ch,prs_fsp,prs_up_pca,prs_down_pca,"));
    for (uint32_t i = 0U; i < PRESSURE_IPC_RAW_COUNT; i++)
    {
        result = system_logger_accumulate(result,
                                          ff_fprintf(p_file,
                                                     "prs%lu_raw,prs%lu_mv,",
                                                     (unsigned long) i,
                                                     (unsigned long) i));
    }
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "aud_status,aud_fsp,aud_file,aud_rate,aud_ch,aud_bits,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "aud_data,aud_read,aud_written,aud_tx_empty,aud_idle,aud_play,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "aud_ssicr,aud_ssisr,aud_ssifcr,aud_ssifsr,aud_ssiofr,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "aud_gtcr,aud_gtior,aud_gtcnt,aud_gtpr,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_magic,cam_seq,cam_status,cam_heartbeat,cam_fsp,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_open,cam_cb,cam_powered,cam_default,cam_address,cam_config,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_detach,cam_not_support,cam_overcurrent,cam_event,cam_usb_status,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_usb_state,cam_module,cam_addr,cam_pipe,cam_type,cam_data,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_vbus,cam_open_tick,cam_event_tick,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_req_done,cam_desc_ok,cam_desc_err,cam_bcd_usb,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_dev_cls,cam_dev_sub,cam_dev_proto,cam_vid,cam_pid,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_cfg_len,cam_if_count,cam_vid_if,cam_vc_if,cam_vs_if,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_mjpeg,cam_yuy2,cam_frames,cam_width,cam_height,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_ep,cam_ep_attr,cam_ep_pkt,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_syscfg,cam_syssts0,cam_dvstctr0,cam_intsts0,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_dpusr0r,cam_dpusr2r,cam_dpval,cam_dmval,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_typec_cfg_err,cam_typec_wr_err,cam_typec_en_pins,cam_vbus_err,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "cam_typec_i2c,cam_typec_identity,cam_typec_role,cam_typec_status,cam_typec_type,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "scam_seq,scam_status,scam_fsp,scam_uart_err,scam_timeout,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "scam_proto_err,scam_probe,scam_probe_ok,scam_active,scam_frames,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "scam_frame_err,scam_stage,scam_fail_stage,scam_last_cmd,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "scam_rx_exp,scam_rx_got,scam_rx0,scam_rx1,scam_rx2,scam_rx3,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "scam_image_bytes,scam_soi,scam_eoi,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "scam_writer_status,scam_writer_err,scam_saved,scam_writer_bytes,"));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "scam_fail_streak,scam_recovery,scam_soft_reset,scam_reopen,"
                                                 "scam_recovery_err,scam_recovery_reason,scam_build,"
                                                 "scam_capture_fail_streak,scam_recoveries_no_frame,"
                                                 "scam_power_cycle,scam_relay_ready\r\n"));

    return result;
}

static int system_logger_write_csv_row(FF_FILE * p_file, sd_card_info_t const * p_sd_info)
{
    int result = 0;
    volatile thermal_ipc_mailbox_t const * p_thm = THERMAL_IPC_MAILBOX;
    volatile tmp117_ipc_mailbox_t const * p_tmp = TMP117_IPC_MAILBOX;
    volatile pressure_ipc_mailbox_t const * p_prs = PRESSURE_IPC_MAILBOX;
    volatile camera_usb_ipc_mailbox_t const * p_cam = CAMERA_USB_IPC_MAILBOX;
    volatile serial_camera_ipc_mailbox_t const * p_scam = SERIAL_CAMERA_IPC_MAILBOX;
    scd41_ipc_snapshot_t scd41_latest;
    scd41_ipc_snapshot_t const * p_scd = &g_system_logger_scd41_snapshot;

    if (scd41_ipc_read_snapshot(&scd41_latest))
    {
        g_system_logger_scd41_snapshot = scd41_latest;
    }

    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%u,%lu,%lu,%ld,%ld,",
                                                 (unsigned long) xTaskGetTickCount(),
                                                 p_sd_info->card_inserted ? 1U : 0U,
                                                 (unsigned long) p_sd_info->free_sectors,
                                                 (unsigned long) p_sd_info->total_sectors,
                                                 (long) p_sd_info->last_fsp_err,
                                                 (long) p_sd_info->last_ff_err));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%d,%d,%d,%ld,%lu,%lu,%lu,",
                                                 (unsigned long) p_thm->sequence,
                                                 (unsigned long) p_thm->status,
                                                 (unsigned long) p_thm->rx_bytes,
                                                 (int) p_thm->min_cdeg,
                                                 (int) p_thm->max_cdeg,
                                                 (int) p_thm->center_cdeg,
                                                 (long) p_thm->avg_cdeg,
                                                 (unsigned long) p_thm->uart_errors,
                                                 (unsigned long) p_thm->head_word,
                                                 (unsigned long) p_thm->raw0));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,",
                                                 (unsigned long) p_thm->raw1,
                                                 (unsigned long) p_thm->raw_center,
                                                 (unsigned long) p_thm->sensor_raw));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%lu,%lu,",
                                                 (unsigned long) p_thm->valid_points,
                                                 (unsigned long) p_thm->crc_received,
                                                 (unsigned long) p_thm->crc_calculated,
                                                 (unsigned long) p_thm->uart_error_count,
                                                 (unsigned long) p_thm->rx_overflow_count));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                                                 (unsigned long) p_thm->parity_error_count,
                                                 (unsigned long) p_thm->framing_error_count,
                                                 (unsigned long) p_thm->fifo_overflow_count,
                                                 (unsigned long) p_thm->crc_error_count,
                                                 (unsigned long) p_thm->data_error_count,
                                                 (unsigned long) p_thm->baud_set_count,
                                                 (unsigned long) p_thm->frame_zero_words,
                                                 (unsigned long) p_thm->frame_one_words,
                                                 (unsigned long) p_thm->frame_ffff_words,
                                                 (unsigned long) p_thm->driver_build));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                                                 (unsigned long) p_thm->consecutive_failures,
                                                 (unsigned long) p_thm->recovery_count,
                                                 (unsigned long) p_thm->reopen_count,
                                                 (unsigned long) p_thm->hard_reset_count,
                                                 (unsigned long) p_thm->recovery_error_count,
                                                 (unsigned long) p_thm->last_recovery_reason,
                                                 (unsigned long) p_thm->last_good_tick,
                                                 (unsigned long) p_thm->run_pin_ready,
                                                 (unsigned long) p_thm->power_cycle_count,
                                                 (unsigned long) p_thm->power_relay_ready,
                                                 (unsigned long) p_thm->stale_frame_count,
                                                 (unsigned long) p_thm->identical_frame_streak));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                                                 (unsigned long) p_tmp->sequence,
                                                 (unsigned long) p_tmp->status,
                                                 (unsigned long) p_tmp->connected_mask,
                                                 (unsigned long) p_tmp->error_mask,
                                                 (unsigned long) p_tmp->fail_step,
                                                 (unsigned long) p_tmp->fail_channel,
                                                 (unsigned long) p_tmp->last_fsp_err));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,",
                                                 (unsigned long) p_tmp->pca_address));

    for (uint32_t i = 0U; i < TMP117_IPC_SENSOR_COUNT; i++)
    {
        result = system_logger_accumulate(result,
                                          ff_fprintf(p_file,
                                                     "%ld,%lu,",
                                                     (long) p_tmp->milli_c[i],
                                                     (unsigned long) p_tmp->address[i]));
    }

    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%ld,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                                                 (unsigned long) p_scd->sequence,
                                                 (unsigned long) p_scd->status,
                                                 (unsigned long) p_scd->co2_ppm,
                                                 (long) p_scd->temperature_milli_c,
                                                 (unsigned long) p_scd->humidity_milli_percent,
                                                 (unsigned long) p_scd->data_ready,
                                                 (unsigned long) p_scd->crc_error_count,
                                                 (unsigned long) p_scd->i2c_error_count,
                                                 (unsigned long) p_scd->timeout_count,
                                                 (unsigned long) p_scd->recovery_count,
                                                 (unsigned long) p_scd->last_fsp_err,
                                                 (unsigned long) p_scd->last_good_tick));

    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%ld,%lu,%lu,%lu,%lu,%lu,%lu,",
                                                 (unsigned long) p_scd->bme_status,
                                                 (unsigned long) p_scd->bme_i2c_address,
                                                 (long) p_scd->bme_temperature_milli_c,
                                                 (unsigned long) p_scd->bme_humidity_milli_percent,
                                                 (unsigned long) p_scd->bme_pressure_pa,
                                                 (unsigned long) p_scd->bme_gas_resistance_ohm,
                                                 (unsigned long) p_scd->bme_flags,
                                                 (unsigned long) p_scd->bme_last_good_tick,
                                                 (unsigned long) p_scd->bme_error_count));

    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                                                 (unsigned long) p_prs->sequence,
                                                 (unsigned long) p_prs->status,
                                                 (unsigned long) p_prs->connected_mask,
                                                 (unsigned long) p_prs->error_mask,
                                                 (unsigned long) p_prs->fail_step,
                                                 (unsigned long) p_prs->fail_channel,
                                                 (unsigned long) p_prs->last_fsp_err,
                                                 (unsigned long) p_prs->upstream_pca_address,
                                                 (unsigned long) p_prs->downstream_pca_address));

    for (uint32_t i = 0U; i < PRESSURE_IPC_RAW_COUNT; i++)
    {
        result = system_logger_accumulate(result,
                                          ff_fprintf(p_file,
                                                     "%d,%ld,",
                                                     (int) p_prs->raw[i],
                                                     (long) p_prs->millivolt[i]));
    }

    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%ld,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                                                 (unsigned long) g_audio_wav_player_debug.status,
                                                 (long) g_audio_wav_player_debug.last_fsp_err,
                                                 g_audio_wav_player_debug.last_file_error,
                                                 (unsigned long) g_audio_wav_player_debug.sample_rate,
                                                 (unsigned long) g_audio_wav_player_debug.channels,
                                                 (unsigned long) g_audio_wav_player_debug.bits_per_sample,
                                                 (unsigned long) g_audio_wav_player_debug.data_bytes,
                                                 (unsigned long) g_audio_wav_player_debug.bytes_read,
                                                 (unsigned long) g_audio_wav_player_debug.bytes_written,
                                                 (unsigned long) g_audio_wav_player_debug.tx_empty_count,
                                                 (unsigned long) g_audio_wav_player_debug.idle_count,
                                                 (unsigned long) g_audio_wav_player_debug.play_count,
                                                 (unsigned long) g_audio_wav_player_debug.ssi_ssicr,
                                                 (unsigned long) g_audio_wav_player_debug.ssi_ssisr,
                                                 (unsigned long) g_audio_wav_player_debug.ssi_ssifcr,
                                                 (unsigned long) g_audio_wav_player_debug.ssi_ssifsr,
                                                 (unsigned long) g_audio_wav_player_debug.ssi_ssiofr,
                                                 (unsigned long) g_audio_wav_player_debug.gpt_gtcr,
                                                 (unsigned long) g_audio_wav_player_debug.gpt_gtior,
                                                 (unsigned long) g_audio_wav_player_debug.gpt_gtcnt,
                                                 (unsigned long) g_audio_wav_player_debug.gpt_gtpr));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                                                 "%lu,%lu,%lu,",
                                                 (unsigned long) p_cam->magic,
                                                 (unsigned long) p_cam->sequence,
                                                 (unsigned long) p_cam->status,
                                                 (unsigned long) p_cam->heartbeat,
                                                 (unsigned long) p_cam->last_fsp_err,
                                                 (unsigned long) p_cam->open_count,
                                                 (unsigned long) p_cam->callback_count,
                                                 (unsigned long) p_cam->powered_count,
                                                 (unsigned long) p_cam->default_count,
                                                 (unsigned long) p_cam->address_count,
                                                 (unsigned long) p_cam->configured_count,
                                                 (unsigned long) p_cam->detach_count,
                                                 (unsigned long) p_cam->not_support_count,
                                                 (unsigned long) p_cam->overcurrent_count,
                                                 (unsigned long) p_cam->last_event,
                                                 (unsigned long) p_cam->last_usb_status,
                                                 (unsigned long) p_cam->last_usb_state,
                                                 (unsigned long) p_cam->last_module,
                                                 (unsigned long) p_cam->last_address,
                                                 (unsigned long) p_cam->last_pipe,
                                                 (unsigned long) p_cam->last_type,
                                                 (unsigned long) p_cam->last_data_size,
                                                 (unsigned long) p_cam->vbus_enabled,
                                                 (unsigned long) p_cam->open_tick,
                                                 (unsigned long) p_cam->last_event_tick));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                                                 "%lu,%lu,%lu,",
                                                 (unsigned long) p_cam->request_complete_count,
                                                 (unsigned long) p_cam->descriptor_read_count,
                                                 (unsigned long) p_cam->descriptor_error_count,
                                                 (unsigned long) p_cam->device_bcd_usb,
                                                 (unsigned long) p_cam->device_class,
                                                 (unsigned long) p_cam->device_subclass,
                                                 (unsigned long) p_cam->device_protocol,
                                                 (unsigned long) p_cam->vendor_id,
                                                 (unsigned long) p_cam->product_id,
                                                 (unsigned long) p_cam->config_total_length,
                                                 (unsigned long) p_cam->config_interface_count,
                                                 (unsigned long) p_cam->video_interface_count,
                                                 (unsigned long) p_cam->video_control_interface,
                                                 (unsigned long) p_cam->video_stream_interface,
                                                 (unsigned long) p_cam->mjpeg_format_count,
                                                 (unsigned long) p_cam->yuy2_format_count,
                                                 (unsigned long) p_cam->frame_count,
                                                 (unsigned long) p_cam->first_width,
                                                 (unsigned long) p_cam->first_height,
                                                 (unsigned long) p_cam->stream_endpoint,
                                                 (unsigned long) p_cam->stream_endpoint_attr,
                                                 (unsigned long) p_cam->stream_endpoint_packet));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                                                 "%lu,%lu,%lu,%lu,",
                                                 (unsigned long) p_cam->reg_syscfg,
                                                 (unsigned long) p_cam->reg_syssts0,
                                                 (unsigned long) p_cam->reg_dvstctr0,
                                                 (unsigned long) p_cam->reg_intsts0,
                                                 (unsigned long) p_cam->reg_dpusb0r,
                                                 (unsigned long) p_cam->reg_dpusb2r,
                                                 (unsigned long) p_cam->line_dpval,
                                                 (unsigned long) p_cam->line_dmval,
                                                 (unsigned long) p_cam->typec_enable_err,
                                                 (unsigned long) p_cam->typec_write_err,
                                                 (unsigned long) p_cam->typec_enabled,
                                                 (unsigned long) p_cam->vbus_set_err));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%lu,%lu,",
                                                 (unsigned long) p_cam->typec_i2c_status,
                                                 (unsigned long) p_cam->typec_identity,
                                                 (unsigned long) p_cam->typec_portrole,
                                                 (unsigned long) p_cam->typec_status,
                                                 (unsigned long) p_cam->typec_type));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                  "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                                                  "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                                                   "%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                                                 (unsigned long) p_scam->sequence,
                                                 (unsigned long) p_scam->status,
                                                 (unsigned long) p_scam->last_fsp_err,
                                                 (unsigned long) p_scam->uart_error_count,
                                                 (unsigned long) p_scam->timeout_count,
                                                 (unsigned long) p_scam->protocol_error_count,
                                                 (unsigned long) p_scam->probe_count,
                                                 (unsigned long) p_scam->probe_ok_count,
                                                  (unsigned long) p_scam->capture_active,
                                                  (unsigned long) p_scam->capture_count,
                                                  (unsigned long) p_scam->capture_error_count,
                                                  (unsigned long) p_scam->capture_stage,
                                                  (unsigned long) p_scam->last_failed_stage,
                                                  (unsigned long) p_scam->last_command_code,
                                                  (unsigned long) p_scam->rx_expected_bytes,
                                                  (unsigned long) p_scam->rx_received_bytes,
                                                  (unsigned long) p_scam->last_reply[0],
                                                  (unsigned long) p_scam->last_reply[1],
                                                  (unsigned long) p_scam->last_reply[2],
                                                  (unsigned long) p_scam->last_reply[3],
                                                  (unsigned long) p_scam->image_size,
                                                 (unsigned long) p_scam->jpeg_soi_ok,
                                                 (unsigned long) p_scam->jpeg_eoi_ok,
                                                 (unsigned long) p_scam->writer_status,
                                                  (unsigned long) p_scam->writer_error,
                                                  (unsigned long) p_scam->writer_saved_count,
                                                  (unsigned long) p_scam->writer_bytes));
    result = system_logger_accumulate(result,
                                      ff_fprintf(p_file,
                                                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
                                                 (unsigned long) p_scam->consecutive_failures,
                                                 (unsigned long) p_scam->recovery_count,
                                                 (unsigned long) p_scam->soft_reset_count,
                                                 (unsigned long) p_scam->uart_reopen_count,
                                                 (unsigned long) p_scam->recovery_error_count,
                                                 (unsigned long) p_scam->last_recovery_reason,
                                                 (unsigned long) p_scam->driver_build,
                                                 (unsigned long) p_scam->capture_consecutive_failures,
                                                 (unsigned long) p_scam->recoveries_without_frame,
                                                 (unsigned long) p_scam->power_cycle_count,
                                                 (unsigned long) p_scam->power_relay_ready));

    return result;
}

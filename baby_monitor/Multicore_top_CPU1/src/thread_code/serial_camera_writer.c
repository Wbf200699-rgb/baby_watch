#include "thread_code/serial_camera_writer.h"

#include <stdio.h>
#include <string.h>

#include "ff_stdio.h"
#include "thread_code/sd_card_driver.h"
#include "thread_code/serial_camera_ipc_shared.h"

#define SERIAL_CAMERA_RECORD_DIR        SD_CARD_MOUNT_PATH "/CAMERA"
#define SERIAL_CAMERA_RECORD_MAX_RUN_ID (9999999U)
#define SERIAL_CAMERA_CUSTOM_SUFFIX_MAX (999U)
#define SERIAL_CAMERA_83_BASENAME_MAX   (8U)
#define SERIAL_CAMERA_COLLISION_PREFIX  (4)
#define SERIAL_CAMERA_PATH_LENGTH       (72U)

static FF_FILE * gp_serial_camera_file;
static uint32_t g_serial_camera_frame_sequence;
static uint32_t g_serial_camera_expected_offset;
static uint32_t g_serial_camera_last_chunk_sequence;
static uint32_t g_serial_camera_next_run_id = 1U;
static uint32_t g_serial_camera_custom_next_suffix;
static bool g_serial_camera_directory_ready;
static bool g_serial_camera_partial_exists;
static char g_serial_camera_custom_basename[SERIAL_CAMERA_RECORD_BASENAME_BYTES];
static char g_serial_camera_final_path[SERIAL_CAMERA_PATH_LENGTH];
static char g_serial_camera_temp_path[SERIAL_CAMERA_PATH_LENGTH];

static uint32_t serial_camera_fs_error(void)
{
    int error = stdioGET_ERRNO();

    sd_card_driver_report_io_error(error);
    return (uint32_t) error;
}

static bool serial_camera_path_exists(char const * p_path)
{
    FF_FILE * p_file = ff_fopen(p_path, "r");

    if (NULL == p_file)
    {
        (void) serial_camera_fs_error();
        return false;
    }

    (void) ff_fclose(p_file);
    return true;
}

static bool serial_camera_basename_char_valid(char value)
{
    return ((value >= 'A') && (value <= 'Z')) ||
           ((value >= 'a') && (value <= 'z')) ||
           ((value >= '0') && (value <= '9')) ||
           ('_' == value) || ('-' == value);
}

static bool serial_camera_get_custom_basename(char * p_basename, size_t basename_size)
{
    volatile serial_camera_ipc_mailbox_t const * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    size_t length = 0U;

    if (basename_size < 2U)
    {
        return false;
    }
    memset(p_basename, 0, basename_size);
    while (length < sizeof(p_cam->record_basename))
    {
        char value = p_cam->record_basename[length];
        if ('\0' == value)
        {
            return length > 0U;
        }
        if (!serial_camera_basename_char_valid(value) ||
            (length >= SERIAL_CAMERA_83_BASENAME_MAX) ||
            ((length + 1U) >= basename_size))
        {
            memset(p_basename, 0, basename_size);
            return false;
        }
        p_basename[length++] = ((value >= 'a') && (value <= 'z')) ? (char) (value - ('a' - 'A')) : value;
    }

    memset(p_basename, 0, basename_size);
    return false;
}

static void serial_camera_publish_filename(char const * p_path)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    char const * p_filename = strrchr(p_path, '/');
    size_t index = 0U;

    p_filename = (NULL == p_filename) ? p_path : (p_filename + 1);
    memset((void *) p_cam->writer_filename, 0, sizeof(p_cam->writer_filename));
    while (('\0' != p_filename[index]) && ((index + 1U) < sizeof(p_cam->writer_filename)))
    {
        p_cam->writer_filename[index] = p_filename[index];
        index++;
    }
    p_cam->writer_filename[index] = '\0';
}

static void serial_camera_close_file(uint32_t status, uint32_t error, bool remove_partial)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;

    if (NULL != gp_serial_camera_file)
    {
        p_cam->writer_status = SERIAL_CAMERA_WRITER_CLOSING;
        __DMB();
        if (0 != ff_fclose(gp_serial_camera_file))
        {
            (void) serial_camera_fs_error();
        }
        gp_serial_camera_file = NULL;
    }

    if (remove_partial && g_serial_camera_partial_exists && ('\0' != g_serial_camera_temp_path[0]))
    {
        p_cam->writer_status = SERIAL_CAMERA_WRITER_CLEANUP;
        __DMB();
        if (0 != ff_remove(g_serial_camera_temp_path))
        {
            (void) serial_camera_fs_error();
        }
    }
    if (remove_partial)
    {
        g_serial_camera_partial_exists = false;
        g_serial_camera_temp_path[0] = '\0';
    }

    p_cam->writer_status = status;
    p_cam->writer_error = error;
    g_serial_camera_expected_offset = 0U;
}

static bool serial_camera_open_file(uint32_t frame_sequence)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    char custom_basename[SERIAL_CAMERA_RECORD_BASENAME_BYTES];
    bool custom_name = serial_camera_get_custom_basename(custom_basename, sizeof(custom_basename));
    uint32_t first_run;
    uint32_t last_run = custom_name ? SERIAL_CAMERA_CUSTOM_SUFFIX_MAX : SERIAL_CAMERA_RECORD_MAX_RUN_ID;

    if (custom_name)
    {
        if (0 != strcmp(custom_basename, g_serial_camera_custom_basename))
        {
            (void) snprintf(g_serial_camera_custom_basename,
                            sizeof(g_serial_camera_custom_basename),
                            "%s",
                            custom_basename);
            g_serial_camera_custom_next_suffix = 0U;
        }
        first_run = g_serial_camera_custom_next_suffix;
    }
    else
    {
        first_run = g_serial_camera_next_run_id;
    }

    if (!g_serial_camera_directory_ready)
    {
        p_cam->writer_status = SERIAL_CAMERA_WRITER_MKDIR;
        p_cam->writer_error = 0U;
        if (0 != ff_mkdir(SERIAL_CAMERA_RECORD_DIR))
        {
            p_cam->writer_error = serial_camera_fs_error();
            return false;
        }
        g_serial_camera_directory_ready = true;
    }

    memset((void *) p_cam->writer_filename, 0, sizeof(p_cam->writer_filename));
    for (uint32_t run = first_run; run <= last_run; run++)
    {
        int final_length;
        int temp_length;
        if (custom_name && (0U == run))
        {
            final_length = snprintf(g_serial_camera_final_path,
                                    sizeof(g_serial_camera_final_path),
                                    SERIAL_CAMERA_RECORD_DIR "/%s.JPG",
                                    custom_basename);
            temp_length = snprintf(g_serial_camera_temp_path,
                                   sizeof(g_serial_camera_temp_path),
                                   SERIAL_CAMERA_RECORD_DIR "/%s.TMP",
                                   custom_basename);
        }
        else if (custom_name)
        {
            final_length = snprintf(g_serial_camera_final_path,
                                    sizeof(g_serial_camera_final_path),
                                    SERIAL_CAMERA_RECORD_DIR "/%.*s_%03lu.JPG",
                                    SERIAL_CAMERA_COLLISION_PREFIX,
                                    custom_basename,
                                    (unsigned long) run);
            temp_length = snprintf(g_serial_camera_temp_path,
                                   sizeof(g_serial_camera_temp_path),
                                   SERIAL_CAMERA_RECORD_DIR "/%.*s_%03lu.TMP",
                                   SERIAL_CAMERA_COLLISION_PREFIX,
                                   custom_basename,
                                   (unsigned long) run);
        }
        else
        {
            final_length = snprintf(g_serial_camera_final_path,
                                    sizeof(g_serial_camera_final_path),
                                    SERIAL_CAMERA_RECORD_DIR "/C%07lu.JPG",
                                    (unsigned long) run);
            temp_length = snprintf(g_serial_camera_temp_path,
                                   sizeof(g_serial_camera_temp_path),
                                   SERIAL_CAMERA_RECORD_DIR "/C%07lu.TMP",
                                   (unsigned long) run);
        }
        if ((final_length < 0) || ((size_t) final_length >= sizeof(g_serial_camera_final_path)) ||
            (temp_length < 0) || ((size_t) temp_length >= sizeof(g_serial_camera_temp_path)))
        {
            p_cam->writer_status = SERIAL_CAMERA_WRITER_OPEN_ERROR;
            p_cam->writer_error = (uint32_t) FSP_ERR_INVALID_SIZE;
            return false;
        }

        p_cam->writer_status = SERIAL_CAMERA_WRITER_CHECK_PATH;
        p_cam->writer_error = 0U;
        __DMB();
        if (!serial_camera_path_exists(g_serial_camera_final_path))
        {
            serial_camera_publish_filename(g_serial_camera_final_path);
            p_cam->writer_status = SERIAL_CAMERA_WRITER_OPENING;
            __DMB();
            gp_serial_camera_file = ff_fopen(g_serial_camera_temp_path, "wb");
            if (NULL == gp_serial_camera_file)
            {
                p_cam->writer_status = SERIAL_CAMERA_WRITER_OPEN_ERROR;
                p_cam->writer_error = serial_camera_fs_error();
                return false;
            }
            g_serial_camera_partial_exists = true;

            g_serial_camera_frame_sequence = frame_sequence;
            g_serial_camera_expected_offset = 0U;
            if (!custom_name)
            {
                g_serial_camera_next_run_id = run + 1U;
            }
            else
            {
                g_serial_camera_custom_next_suffix = run + 1U;
            }
            p_cam->writer_frame_sequence = frame_sequence;
            p_cam->writer_open_count++;
            p_cam->writer_status = SERIAL_CAMERA_WRITER_OPEN;
            p_cam->writer_error = 0U;
            return true;
        }
    }

    p_cam->writer_status = SERIAL_CAMERA_WRITER_OPEN_ERROR;
    p_cam->writer_error = 0U;
    return false;
}

static bool serial_camera_finalize_file(void)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;

    if (NULL != gp_serial_camera_file)
    {
        p_cam->writer_status = SERIAL_CAMERA_WRITER_CLOSING;
        __DMB();
        if (0 != ff_fclose(gp_serial_camera_file))
        {
            uint32_t error = serial_camera_fs_error();
            gp_serial_camera_file = NULL;
            serial_camera_close_file(SERIAL_CAMERA_WRITER_WRITE_ERROR,
                                     error,
                                     true);
            return false;
        }
        gp_serial_camera_file = NULL;
    }

    p_cam->writer_status = SERIAL_CAMERA_WRITER_RENAMING;
    __DMB();
    if (0 != ff_rename(g_serial_camera_temp_path, g_serial_camera_final_path, false))
    {
        uint32_t error = serial_camera_fs_error();
        serial_camera_close_file(SERIAL_CAMERA_WRITER_WRITE_ERROR,
                                 error,
                                 true);
        return false;
    }

    g_serial_camera_partial_exists = false;
    g_serial_camera_temp_path[0] = '\0';
    p_cam->writer_status = SERIAL_CAMERA_WRITER_CLOSED;
    p_cam->writer_error = 0U;
    p_cam->writer_saved_count++;
    g_serial_camera_expected_offset = 0U;
    return true;
}

void serial_camera_writer_poll(bool sd_ready)
{
    volatile serial_camera_ipc_mailbox_t * p_cam = SERIAL_CAMERA_IPC_MAILBOX;
    uint32_t chunk_sequence;
    uint32_t frame_sequence;
    uint32_t image_size;
    uint32_t offset;
    uint32_t bytes;
    bool write_ok = false;

    if (SERIAL_CAMERA_IPC_MAGIC != p_cam->magic)
    {
        return;
    }

    p_cam->writer_poll_count++;
    chunk_sequence = p_cam->chunk_sequence;
    if (!sd_ready)
    {
        g_serial_camera_directory_ready = false;
        serial_camera_close_file(SERIAL_CAMERA_WRITER_NO_SD, 0U, true);
        if ((0U != chunk_sequence) && (chunk_sequence != g_serial_camera_last_chunk_sequence))
        {
            g_serial_camera_last_chunk_sequence = chunk_sequence;
            __DMB();
            p_cam->chunk_ack_sequence = chunk_sequence;
        }
        return;
    }

    if ((0U == chunk_sequence) || (chunk_sequence == g_serial_camera_last_chunk_sequence))
    {
        if ((0U == p_cam->capture_active) && (NULL == gp_serial_camera_file))
        {
            if ((SERIAL_CAMERA_WRITER_MKDIR != p_cam->writer_status) &&
                (SERIAL_CAMERA_WRITER_OPEN_ERROR != p_cam->writer_status) &&
                (SERIAL_CAMERA_WRITER_WRITE_ERROR != p_cam->writer_status))
            {
                /* The SD driver is authoritative.  Clear a stale NO_SD state
                 * as soon as the shared file-system thread reports mounted. */
                p_cam->writer_status = SERIAL_CAMERA_WRITER_WAIT_FRAME;
                p_cam->writer_error = 0U;
            }
        }
        return;
    }

    __DMB();
    frame_sequence = p_cam->frame_sequence;
    image_size = p_cam->image_size;
    offset = p_cam->image_offset;
    bytes = p_cam->chunk_bytes;

    if ((0U == bytes) || (bytes > SERIAL_CAMERA_IPC_CHUNK_BYTES) ||
        (offset > image_size) || (bytes > (image_size - offset)))
    {
        serial_camera_close_file(SERIAL_CAMERA_WRITER_WRITE_ERROR, (uint32_t) FSP_ERR_INVALID_SIZE, true);
    }
    else
    {
        if ((0U == offset) &&
            ((NULL == gp_serial_camera_file) || (g_serial_camera_frame_sequence != frame_sequence)))
        {
            serial_camera_close_file(SERIAL_CAMERA_WRITER_IDLE, 0U, true);
            (void) serial_camera_open_file(frame_sequence);
        }

        if ((NULL != gp_serial_camera_file) &&
            (g_serial_camera_frame_sequence == frame_sequence) &&
            (g_serial_camera_expected_offset == offset))
        {
            size_t written = ff_fwrite((void const *) p_cam->chunk, 1U, bytes, gp_serial_camera_file);
            if (written == bytes)
            {
                g_serial_camera_expected_offset += bytes;
                p_cam->writer_write_count++;
                p_cam->writer_bytes += bytes;
                p_cam->writer_chunk_sequence = chunk_sequence;
                p_cam->writer_status = SERIAL_CAMERA_WRITER_WRITE;
                p_cam->writer_error = 0U;
                write_ok = true;
            }
            else
            {
                uint32_t error = serial_camera_fs_error();
                serial_camera_close_file(SERIAL_CAMERA_WRITER_WRITE_ERROR,
                                         error,
                                         true);
            }
        }
        else if (NULL != gp_serial_camera_file)
        {
            serial_camera_close_file(SERIAL_CAMERA_WRITER_WRITE_ERROR,
                                     (uint32_t) FSP_ERR_INVALID_DATA,
                                     true);
        }
    }

    if (write_ok && ((offset + bytes) == image_size))
    {
        if ((0U == p_cam->jpeg_soi_ok) || (0U == p_cam->jpeg_eoi_ok))
        {
            serial_camera_close_file(SERIAL_CAMERA_WRITER_WRITE_ERROR,
                                     (uint32_t) FSP_ERR_INVALID_DATA,
                                     true);
        }
        else
        {
            (void) serial_camera_finalize_file();
        }
    }

    g_serial_camera_last_chunk_sequence = chunk_sequence;
    __DMB();
    p_cam->chunk_ack_sequence = chunk_sequence;
}

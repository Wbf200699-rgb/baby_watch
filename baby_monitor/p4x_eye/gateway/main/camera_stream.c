#include "camera_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "media_protocol.h"
#include "realtime_tx_guard.h"

#ifndef GATEWAY_WS_ORIGIN
#define GATEWAY_WS_ORIGIN "wss://renesas.zyxserver.online"
#endif

#define VIDEO_WSS_URI GATEWAY_WS_ORIGIN "/api/v1/media/device/video"
#define VIDEO_WSS_PATH "/api/v1/media/device/video"
#define VIDEO_WIDTH 640U
#define VIDEO_HEIGHT 360U
#define VIDEO_CROP_WIDTH 1707U
#define VIDEO_CROP_HEIGHT 960U
#define VIDEO_PPA_SCALE (3.0f / 8.0f)
#define VIDEO_RAW_BUFFER_COUNT 3U
#define VIDEO_JPEG_BUFFER_COUNT 2U
#define VIDEO_DEFAULT_FPS 10U
#define VIDEO_MIN_FPS 4U
#define VIDEO_MAX_FPS 12U
#define VIDEO_DEFAULT_JPEG_QUALITY 65U
#define VIDEO_CONGESTED_JPEG_QUALITY 52U
#define VIDEO_MAX_SEND_MS 300U
#define VIDEO_FRAGMENT_BYTES 4096U
#define VIDEO_FRAGMENT_TIMEOUT_MS 150U
#define VIDEO_WSS_BUFFER_BYTES (32U * 1024U)

static const char *TAG = "p4x_camera";

typedef struct {
    uint8_t *data;
    uint32_t length;
    uint32_t sequence;
    uint64_t timestamp_us;
    uint16_t width;
    uint16_t height;
    uint8_t quality;
} video_frame_t;

static int s_video_fd = -1;
static void *s_raw_buffers[VIDEO_RAW_BUFFER_COUNT];
static size_t s_raw_buffer_lengths[VIDEO_RAW_BUFFER_COUNT];
static uint32_t s_raw_buffer_count;
static jpeg_encoder_handle_t s_jpeg_encoder;
static ppa_client_handle_t s_ppa;
static uint8_t *s_scaled_buffer;
static size_t s_scaled_buffer_capacity;
static size_t s_jpeg_capacity;
static QueueHandle_t s_video_queue;
static QueueHandle_t s_free_jpeg_queue;
static esp_websocket_client_handle_t s_video_ws;
static esp_transport_handle_t s_video_ssl_transport;
static esp_transport_handle_t s_video_transport;
static volatile bool s_video_connected;
static volatile bool s_video_enabled;
static volatile bool s_video_transport_suspended;
static volatile uint8_t s_target_fps = VIDEO_DEFAULT_FPS;
static volatile uint8_t s_jpeg_quality = VIDEO_DEFAULT_JPEG_QUALITY;
static uint16_t s_active_width = VIDEO_WIDTH;
static uint16_t s_active_height = VIDEO_HEIGHT;
static uint32_t s_sequence = 1;
static uint32_t s_queue_drops;
static uint32_t s_encode_failures;
static uint32_t s_send_failures;
static uint32_t s_send_timeouts;
static uint32_t s_arbitration_drops;
static uint32_t s_backpressure_drops;
static uint32_t s_last_send_ms;
static uint32_t s_max_send_ms;
static uint32_t s_last_control_sequence;
static bool s_has_control_sequence;
static uint32_t s_control_count;
static int64_t s_last_control_at_us;

bool camera_stream_is_connected(void)
{
    return s_video_connected;
}

void camera_stream_get_stats(camera_stream_stats_t *stats)
{
    if (!stats) {
        return;
    }
    stats->enabled = s_video_enabled;
    stats->transport_suspended = s_video_transport_suspended;
    stats->control_count = s_control_count;
    stats->last_control_sequence = s_last_control_sequence;
    stats->last_control_age_ms = s_last_control_at_us > 0
                                     ? (uint32_t)((esp_timer_get_time() -
                                                   s_last_control_at_us) /
                                                  1000)
                                     : UINT32_MAX;
    stats->queue_drops = s_queue_drops;
    stats->encode_failures = s_encode_failures;
    stats->send_failures = s_send_failures;
    stats->send_timeouts = s_send_timeouts;
    stats->arbitration_drops = s_arbitration_drops;
    stats->backpressure_drops = s_backpressure_drops;
    stats->last_send_ms = s_last_send_ms;
    stats->max_send_ms = s_max_send_ms;
}

static void destroy_video_transport(void)
{
    if (s_video_transport) {
        esp_transport_destroy(s_video_transport);
        s_video_transport = NULL;
    }
    if (s_video_ssl_transport) {
        esp_transport_destroy(s_video_ssl_transport);
        s_video_ssl_transport = NULL;
    }
}

static esp_err_t create_video_transport(const char *headers)
{
    s_video_ssl_transport = esp_transport_ssl_init();
    if (!s_video_ssl_transport) {
        return ESP_ERR_NO_MEM;
    }
    esp_transport_set_default_port(s_video_ssl_transport, 443);
    esp_transport_ssl_crt_bundle_attach(s_video_ssl_transport,
                                        esp_crt_bundle_attach);

    s_video_transport = esp_transport_ws_init(s_video_ssl_transport);
    if (!s_video_transport) {
        destroy_video_transport();
        return ESP_ERR_NO_MEM;
    }
    esp_transport_set_default_port(s_video_transport, 443);
    const esp_transport_ws_config_t transport_config = {
        .ws_path = VIDEO_WSS_PATH,
        .headers = headers,
        .propagate_control_frames = true,
    };
    esp_err_t result =
        esp_transport_ws_set_config(s_video_transport, &transport_config);
    if (result != ESP_OK) {
        destroy_video_transport();
    }
    return result;
}

static void recycle_frame(video_frame_t *frame)
{
    if (frame && frame->data && s_free_jpeg_queue) {
        xQueueSend(s_free_jpeg_queue, &frame->data, 0);
        frame->data = NULL;
    }
}

static void clear_video_queue(void)
{
    video_frame_t frame;
    while (s_video_queue &&
           xQueueReceive(s_video_queue, &frame, 0) == pdTRUE) {
        recycle_frame(&frame);
    }
}

esp_err_t camera_stream_set_transport_suspended(bool suspended)
{
    if (!s_video_ws || suspended == s_video_transport_suspended) {
        return ESP_OK;
    }

    if (suspended) {
        s_video_transport_suspended = true;
        clear_video_queue();
        esp_err_t result = esp_websocket_client_stop(s_video_ws);
        if (result == ESP_ERR_INVALID_STATE) {
            result = ESP_OK;
        }
        if (result != ESP_OK) {
            s_video_transport_suspended = false;
            ESP_LOGW(TAG, "pause video WSS failed: %s", esp_err_to_name(result));
            return result;
        }
        ESP_LOGI(TAG, "video WSS paused for bulk transfer");
        return ESP_OK;
    }

    s_video_transport_suspended = false;
    esp_err_t result = esp_websocket_client_start(s_video_ws);
    if (result == ESP_ERR_INVALID_STATE) {
        result = ESP_OK;
    }
    if (result != ESP_OK) {
        s_video_transport_suspended = true;
        ESP_LOGW(TAG, "resume video WSS failed: %s", esp_err_to_name(result));
        return result;
    }
    ESP_LOGI(TAG, "video WSS resumed after bulk transfer");
    return ESP_OK;
}

/*
 * The P4 reaches Wi-Fi through ESP-Hosted on the C6. A single 20-30 KiB TLS
 * write can monopolize that transport for hundreds of milliseconds and make
 * unrelated audio/telemetry reads fail with EINPROGRESS. Keep one logical
 * WebSocket message, but yield the shared transport between 4 KiB fragments.
 */
static bool wait_for_video_fragment_slot(void)
{
    while (s_video_connected &&
           esp_websocket_client_is_connected(s_video_ws)) {
        if (realtime_tx_guard_take(0)) {
            if (esp_transport_poll_write(s_video_transport, 0) > 0) {
                return true;
            }
            realtime_tx_guard_give();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

/* The caller owns the TX guard for the first fragment. */
static int send_video_message(const uint8_t *wire, size_t wire_length)
{
    size_t offset = 0;
    size_t fragment = wire_length < VIDEO_FRAGMENT_BYTES
                          ? wire_length
                          : VIDEO_FRAGMENT_BYTES;
    int sent = esp_websocket_client_send_bin_partial(
        s_video_ws, (const char *)wire, fragment,
        pdMS_TO_TICKS(VIDEO_FRAGMENT_TIMEOUT_MS));
    realtime_tx_guard_give();
    if (sent != (int)fragment) {
        return sent;
    }
    offset += fragment;

    while (offset < wire_length) {
        if (!wait_for_video_fragment_slot()) {
            return -1;
        }
        fragment = wire_length - offset;
        if (fragment > VIDEO_FRAGMENT_BYTES) {
            fragment = VIDEO_FRAGMENT_BYTES;
        }
        sent = esp_websocket_client_send_cont_msg(
            s_video_ws, (const char *)(wire + offset), fragment,
            pdMS_TO_TICKS(VIDEO_FRAGMENT_TIMEOUT_MS));
        realtime_tx_guard_give();
        if (sent != (int)fragment) {
            return sent;
        }
        offset += fragment;
        taskYIELD();
    }

    if (!wait_for_video_fragment_slot()) {
        return -1;
    }
    int fin_sent = esp_websocket_client_send_fin(
        s_video_ws, pdMS_TO_TICKS(VIDEO_FRAGMENT_TIMEOUT_MS));
    realtime_tx_guard_give();
    return fin_sent >= 0 ? (int)wire_length : fin_sent;
}

static void apply_control(const char *text, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(text, length);
    if (!root) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(root, "sequence");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *target_fps =
        cJSON_GetObjectItemCaseSensitive(root, "target_fps");

    if (cJSON_IsString(type) &&
        strcmp(type->valuestring, "media.control") == 0 &&
        cJSON_IsNumber(version) && version->valueint == 1 &&
        cJSON_IsNumber(sequence) && sequence->valuedouble >= 0 &&
        sequence->valuedouble <= UINT32_MAX && cJSON_IsBool(enabled)) {
        uint32_t next_sequence = (uint32_t)sequence->valuedouble;
        if (!s_has_control_sequence ||
            (int32_t)(next_sequence - s_last_control_sequence) > 0) {
            s_last_control_sequence = next_sequence;
            s_has_control_sequence = true;
            ++s_control_count;
            s_last_control_at_us = esp_timer_get_time();

            int fps = cJSON_IsNumber(target_fps)
                          ? target_fps->valueint
                          : VIDEO_DEFAULT_FPS;
            if (fps < VIDEO_MIN_FPS) {
                fps = VIDEO_MIN_FPS;
            } else if (fps > VIDEO_MAX_FPS) {
                fps = VIDEO_MAX_FPS;
            }
            s_target_fps = (uint8_t)fps;
            s_jpeg_quality = VIDEO_DEFAULT_JPEG_QUALITY;
            s_video_enabled = cJSON_IsTrue(enabled);
            if (!s_video_enabled) {
                clear_video_queue();
            }
            ESP_LOGI(TAG, "video upload %s, target=%u fps",
                     s_video_enabled ? "enabled" : "paused",
                     (unsigned)s_target_fps);
        }
    }
    cJSON_Delete(root);
}

static void video_ws_event(void *args, esp_event_base_t base, int32_t event_id,
                           void *event_data)
{
    (void)args;
    (void)base;
    esp_websocket_event_data_t *data = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        /* Control sequence numbers are scoped to one server process/WSS. */
        s_has_control_sequence = false;
        s_video_enabled = false;
        s_last_control_at_us = 0;
        clear_video_queue();
        s_video_connected = true;
        ESP_LOGI(TAG, "video WSS connected");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_video_connected = false;
        s_video_enabled = false;
        s_has_control_sequence = false;
        clear_video_queue();
        ESP_LOGW(TAG, "video WSS disconnected");
    } else if (event_id == WEBSOCKET_EVENT_DATA && data &&
               data->op_code == 0x1 && data->payload_offset == 0 &&
               data->payload_len == data->data_len) {
        apply_control(data->data_ptr, data->data_len);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        s_video_connected = false;
        s_video_enabled = false;
        s_has_control_sequence = false;
        s_last_control_at_us = 0;
        clear_video_queue();
        ESP_LOGW(TAG, "video WSS transport error");
    }
}

static esp_err_t initialize_sensor(void)
{
    ESP_RETURN_ON_ERROR(bsp_p4_eye_init(), TAG, "initialize P4X-EYE board");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "initialize camera I2C");

    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_RETURN_ON_ERROR(bsp_get_i2c_bus_handle(&i2c_bus), TAG,
                        "get camera I2C handle");

    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus,
            .freq = 100000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    esp_video_init_config_t video_config = {
        .csi = &csi_config,
    };
    return esp_video_init(&video_config);
}

static esp_err_t configure_capture_device(void)
{
    s_video_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (s_video_fd < 0) {
        ESP_LOGE(TAG, "open %s failed: errno=%d",
                 ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno);
        return ESP_FAIL;
    }

    struct v4l2_capability capability = {0};
    if (ioctl(s_video_fd, VIDIOC_QUERYCAP, &capability) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed: errno=%d", errno);
        return ESP_FAIL;
    }

    /*
     * OV2710 exposes sensor modes (normally 1920x1080) rather than arbitrary
     * output sizes.  Asking the CSI device for 640x480 makes the driver reject
     * the whole format.  Preserve the active sensor mode and only select the
     * ISP output pixel format, as Espressif's P4-EYE example does.
     */
    struct v4l2_format format = {0};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "get default camera format failed: errno=%d", errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OV2710 sensor mode: %" PRIu32 "x%" PRIu32
                  " pixel_format=0x%08" PRIx32,
             format.fmt.pix.width, format.fmt.pix.height,
             format.fmt.pix.pixelformat);
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(s_video_fd, VIDIOC_S_FMT, &format) != 0) {
            ESP_LOGE(TAG,
                     "set RGB565 at sensor-native resolution failed: errno=%d",
                     errno);
            return ESP_FAIL;
        }

        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s_video_fd, VIDIOC_G_FMT, &format) != 0) {
            ESP_LOGE(TAG, "get active camera format failed: errno=%d", errno);
            return ESP_FAIL;
        }
    }
    s_active_width = (uint16_t)format.fmt.pix.width;
    s_active_height = (uint16_t)format.fmt.pix.height;
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        ESP_LOGE(TAG, "camera did not accept RGB565 format: 0x%08" PRIx32,
                 format.fmt.pix.pixelformat);
        return ESP_ERR_NOT_SUPPORTED;
    }

    struct v4l2_requestbuffers request = {
        .count = VIDEO_RAW_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_video_fd, VIDIOC_REQBUFS, &request) != 0 ||
        request.count < 2) {
        ESP_LOGE(TAG, "request camera buffers failed: count=%" PRIu32
                      " errno=%d",
                 request.count, errno);
        return ESP_FAIL;
    }
    s_raw_buffer_count = request.count < VIDEO_RAW_BUFFER_COUNT
                             ? request.count
                             : VIDEO_RAW_BUFFER_COUNT;

    for (uint32_t i = 0; i < s_raw_buffer_count; ++i) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(s_video_fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "query camera buffer %" PRIu32 " failed", i);
            return ESP_FAIL;
        }
        void *mapped = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                            MAP_SHARED, s_video_fd, buffer.m.offset);
        /*
         * ESP-IDF's V4L2 camera examples return NULL when mmap fails.
         * MAP_FAILED is not exposed by the ESP-IDF sys/mman shim used on P4.
         */
        if (mapped == NULL) {
            ESP_LOGE(TAG, "mmap camera buffer %" PRIu32 " failed", i);
            return ESP_FAIL;
        }
        s_raw_buffers[i] = mapped;
        s_raw_buffer_lengths[i] = buffer.length;
        if (ioctl(s_video_fd, VIDIOC_QBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "queue camera buffer %" PRIu32 " failed", i);
            return ESP_FAIL;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "start camera stream failed: errno=%d", errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OV2710 capture ready: %ux%u RGB565, buffers=%" PRIu32,
             s_active_width, s_active_height, s_raw_buffer_count);
    return ESP_OK;
}

static esp_err_t initialize_jpeg_pool(void)
{
    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_config, &s_ppa), TAG,
                        "register PPA scaler");

    size_t cache_line_size = 0;
    ESP_RETURN_ON_ERROR(
        esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cache_line_size),
        TAG, "failed to query PSRAM cache alignment");
    const size_t scaled_bytes = VIDEO_WIDTH * VIDEO_HEIGHT * 2U;
    s_scaled_buffer_capacity =
        (scaled_bytes + cache_line_size - 1U) & ~(cache_line_size - 1U);
    s_scaled_buffer = heap_caps_aligned_calloc(
        cache_line_size, 1, s_scaled_buffer_capacity,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_scaled_buffer) {
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_engine_cfg_t engine_config = {
        .intr_priority = 0,
        .timeout_ms = 120,
    };
    ESP_RETURN_ON_ERROR(
        jpeg_new_encoder_engine(&engine_config, &s_jpeg_encoder),
        TAG, "create hardware JPEG encoder");

    s_video_queue = xQueueCreate(1, sizeof(video_frame_t));
    s_free_jpeg_queue = xQueueCreate(VIDEO_JPEG_BUFFER_COUNT,
                                     sizeof(uint8_t *));
    if (!s_video_queue || !s_free_jpeg_queue) {
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_memory_alloc_cfg_t memory_config = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    for (size_t i = 0; i < VIDEO_JPEG_BUFFER_COUNT; ++i) {
        size_t allocated = 0;
        uint8_t *buffer = jpeg_alloc_encoder_mem(
            MEDIA_VIDEO_MAX_PAYLOAD_SIZE, &memory_config, &allocated);
        if (!buffer || allocated < MEDIA_VIDEO_MAX_PAYLOAD_SIZE) {
            ESP_LOGE(TAG, "allocate JPEG buffer %u failed: %u bytes",
                     (unsigned)i, (unsigned)allocated);
            return ESP_ERR_NO_MEM;
        }
        if (s_jpeg_capacity == 0 || allocated < s_jpeg_capacity) {
            s_jpeg_capacity = allocated;
        }
        xQueueSend(s_free_jpeg_queue, &buffer, 0);
    }
    return ESP_OK;
}

static void camera_capture_task(void *argument)
{
    (void)argument;
    uint64_t next_capture_us = 0;

    for (;;) {
        struct v4l2_buffer raw = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s_video_fd, VIDIOC_DQBUF, &raw) != 0) {
            ESP_LOGW(TAG, "dequeue camera frame failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (raw.index >= s_raw_buffer_count) {
            ESP_LOGE(TAG, "camera returned invalid buffer index %" PRIu32,
                     raw.index);
            continue;
        }

        bool should_encode = !s_video_transport_suspended &&
                              s_video_enabled && s_video_connected &&
                              esp_timer_get_time() >= next_capture_us;
        if (should_encode) {
            const uint64_t capture_started_us = esp_timer_get_time();
            uint8_t *jpeg_buffer = NULL;
            if (xQueueReceive(s_free_jpeg_queue, &jpeg_buffer, 0) == pdTRUE) {
                ppa_srm_oper_config_t scale_config = {
                    .in.buffer = s_raw_buffers[raw.index],
                    .in.pic_w = s_active_width,
                    .in.pic_h = s_active_height,
                    /*
                     * PPA scales in 64x32 macroblocks.  Scaling the complete
                     * 1920x1080 frame by 1/3 is quantized by the hardware to
                     * 5/16, leaving unfilled strips.  P4 PPA exposes four
                     * fractional scale bits, so use the exactly representable
                     * 6/16 factor with a centered, near-16:9 crop:
                     *   1707 * 6/16 = 640.125 -> 640
                     *    960 * 6/16 = 360
                     */
                    .in.block_w = VIDEO_CROP_WIDTH,
                    .in.block_h = VIDEO_CROP_HEIGHT,
                    .in.block_offset_x =
                        (s_active_width - VIDEO_CROP_WIDTH) / 2U,
                    .in.block_offset_y =
                        (s_active_height - VIDEO_CROP_HEIGHT) / 2U,
                    .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                    .out.buffer = s_scaled_buffer,
                    .out.buffer_size = s_scaled_buffer_capacity,
                    .out.pic_w = VIDEO_WIDTH,
                    .out.pic_h = VIDEO_HEIGHT,
                    .out.block_offset_x = 0,
                    .out.block_offset_y = 0,
                    .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                    .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
                    .scale_x = VIDEO_PPA_SCALE,
                    .scale_y = VIDEO_PPA_SCALE,
                    .rgb_swap = 0,
                    .byte_swap = 0,
                    .mode = PPA_TRANS_MODE_BLOCKING,
                };
                esp_err_t scale_result =
                    ppa_do_scale_rotate_mirror(s_ppa, &scale_config);
                jpeg_encode_cfg_t encode_config = {
                    .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                    .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
                    .image_quality = s_jpeg_quality,
                    .width = VIDEO_WIDTH,
                    .height = VIDEO_HEIGHT,
                };
                uint32_t jpeg_size = 0;
                esp_err_t result = scale_result;
                if (result == ESP_OK) {
                    result = jpeg_encoder_process(
                        s_jpeg_encoder, &encode_config, s_scaled_buffer,
                        s_scaled_buffer_capacity, jpeg_buffer, s_jpeg_capacity,
                        &jpeg_size);
                }
                if (result == ESP_OK && jpeg_size > 0 &&
                    jpeg_size <= MEDIA_VIDEO_MAX_PAYLOAD_SIZE) {
                    video_frame_t frame = {
                        .data = jpeg_buffer,
                        .length = jpeg_size,
                        .sequence = s_sequence++,
                        .timestamp_us = (uint64_t)esp_timer_get_time(),
                        .width = VIDEO_WIDTH,
                        .height = VIDEO_HEIGHT,
                        /*
                         * S3/OV2640 and the deployed server use the sensor
                         * JPEG quality scale (smaller is better, 1..63).
                         * P4's hardware encoder uses the inverse 1..100
                         * scale.  Report the inverse value on the wire so
                         * existing validation/UI stay compatible.
                         */
                        .quality = (uint16_t)(100U - s_jpeg_quality),
                    };
                    video_frame_t stale;
                    if (xQueueReceive(s_video_queue, &stale, 0) == pdTRUE) {
                        recycle_frame(&stale);
                        ++s_queue_drops;
                    }
                    if (xQueueSend(s_video_queue, &frame, 0) != pdTRUE) {
                        recycle_frame(&frame);
                        ++s_queue_drops;
                    }
                } else {
                    xQueueSend(s_free_jpeg_queue, &jpeg_buffer, 0);
                    ++s_encode_failures;
                    if ((s_encode_failures % 30U) == 1U) {
                        ESP_LOGW(TAG, "JPEG encode failed: %s size=%" PRIu32
                                      " failures=%" PRIu32,
                                 esp_err_to_name(result), jpeg_size,
                                 s_encode_failures);
                    }
                }
            } else {
                ++s_queue_drops;
            }
            uint8_t fps = s_target_fps > 0
                              ? s_target_fps
                              : VIDEO_DEFAULT_FPS;
            /*
             * Schedule from the start of this frame.  Scheduling from the end
             * makes the real period target_period + capture/scale/JPEG time.
             */
            next_capture_us = capture_started_us + 1000000ULL / fps;
        }

        if (ioctl(s_video_fd, VIDIOC_QBUF, &raw) != 0) {
            ESP_LOGE(TAG, "requeue camera frame failed: errno=%d", errno);
        }
        if (s_video_transport_suspended || !s_video_enabled || !s_video_connected) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

static void camera_send_task(void *argument)
{
    (void)argument;
    video_frame_t frame;
    unsigned congested = 0;
    uint8_t *wire = heap_caps_malloc(
        MEDIA_HEADER_SIZE + MEDIA_VIDEO_MAX_PAYLOAD_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wire) {
        ESP_LOGE(TAG, "allocate complete video message buffer failed");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        if (xQueueReceive(s_video_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_video_transport_suspended || !s_video_enabled || !s_video_connected ||
            !esp_websocket_client_is_connected(s_video_ws)) {
            recycle_frame(&frame);
            continue;
        }

        if (!realtime_tx_guard_take(0)) {
            ++s_arbitration_drops;
            recycle_frame(&frame);
            if ((s_arbitration_drops % 100U) == 1U) {
                ESP_LOGW(TAG,
                         "video frame dropped for realtime TX priority: %" PRIu32,
                         s_arbitration_drops);
            }
            continue;
        }
        if (esp_transport_poll_write(s_video_transport, 0) <= 0) {
            realtime_tx_guard_give();
            ++s_backpressure_drops;
            recycle_frame(&frame);
            if ((s_backpressure_drops % 100U) == 1U) {
                ESP_LOGW(TAG,
                         "video frame dropped for socket backpressure: %" PRIu32,
                         s_backpressure_drops);
            }
            continue;
        }

        media_build_jpeg_header(wire, frame.sequence, frame.timestamp_us,
                                frame.length, frame.width, frame.height,
                                frame.quality);
        memcpy(wire + MEDIA_HEADER_SIZE, frame.data, frame.length);
        size_t wire_length = MEDIA_HEADER_SIZE + frame.length;
        int64_t started_us = esp_timer_get_time();
        int sent = send_video_message(wire, wire_length);
        uint32_t elapsed_ms =
            (uint32_t)((esp_timer_get_time() - started_us) / 1000);
        s_last_send_ms = elapsed_ms;
        if (elapsed_ms > s_max_send_ms) {
            s_max_send_ms = elapsed_ms;
        }
        bool sent_ok = sent == (int)wire_length;
        recycle_frame(&frame);

        if (!sent_ok || elapsed_ms > VIDEO_MAX_SEND_MS) {
            congested += sent_ok ? 1U : 3U;
            if (congested > 20U) {
                congested = 20U;
            }
            if (congested >= 6U) {
                s_jpeg_quality = VIDEO_CONGESTED_JPEG_QUALITY;
            }
            if (!sent_ok) {
                if (elapsed_ms >= VIDEO_FRAGMENT_TIMEOUT_MS) {
                    ++s_send_timeouts;
                }
                if (!esp_websocket_client_is_connected(s_video_ws)) {
                    s_video_connected = false;
                }
                ++s_send_failures;
                ESP_LOGW(TAG,
                         "video send failed: sent=%d expected=%u elapsed=%" PRIu32
                         "ms failures=%" PRIu32,
                         sent, (unsigned)wire_length, elapsed_ms,
                         s_send_failures);
            }
        } else {
            if (congested > 0) {
                --congested;
            }
            if (congested == 0) {
                s_jpeg_quality = VIDEO_DEFAULT_JPEG_QUALITY;
            }
        }
    }
}

esp_err_t camera_stream_start(void)
{
    ESP_RETURN_ON_ERROR(initialize_sensor(), TAG, "initialize OV2710");
    ESP_RETURN_ON_ERROR(configure_capture_device(), TAG,
                        "configure MIPI-CSI capture");
    ESP_RETURN_ON_ERROR(initialize_jpeg_pool(), TAG,
                        "initialize JPEG pool");

    char headers[256];
    snprintf(headers, sizeof(headers),
             "X-Device-ID: %s\r\nX-Device-Key: %s\r\n",
             GATEWAY_DEVICE_ID, GATEWAY_DEVICE_KEY);
    esp_err_t result = create_video_transport(headers);
    if (result != ESP_OK) {
        return result;
    }
    esp_websocket_client_config_t ws_config = {
        .uri = VIDEO_WSS_URI,
        .ext_transport = s_video_transport,
        .buffer_size = VIDEO_WSS_BUFFER_BYTES,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 3000,
        .enable_close_reconnect = true,
        .ping_interval_sec = 20,
    };
    s_video_ws = esp_websocket_client_init(&ws_config);
    if (!s_video_ws) {
        destroy_video_transport();
        return ESP_ERR_NO_MEM;
    }
    result = esp_websocket_register_events(s_video_ws, WEBSOCKET_EVENT_ANY,
                                           video_ws_event, NULL);
    if (result != ESP_OK) {
        esp_websocket_client_destroy(s_video_ws);
        s_video_ws = NULL;
        destroy_video_transport();
        return result;
    }
    result = esp_websocket_client_start(s_video_ws);
    if (result != ESP_OK) {
        esp_websocket_client_destroy(s_video_ws);
        s_video_ws = NULL;
        destroy_video_transport();
        return result;
    }

    if (xTaskCreatePinnedToCore(camera_capture_task, "p4x_cam_capture",
                                8192, NULL, 6, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(camera_send_task, "p4x_cam_send",
                                8192, NULL, 5, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "camera streaming backend ready: OV2710 1920x1080 -> PPA "
             "%ux%u -> hardware JPEG",
             VIDEO_WIDTH, VIDEO_HEIGHT);
    return ESP_OK;
}

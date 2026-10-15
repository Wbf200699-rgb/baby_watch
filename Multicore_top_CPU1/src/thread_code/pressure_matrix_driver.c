#include "thread_code/pressure_matrix_driver.h"

#include <string.h>

#include "pressure_thread.h"
#include "thread_code/tmp117_ipc_shared.h"

#define PRESSURE_DEBUG_MAGIC           (0x50525344U) /* PRSD */
#define PRESSURE_BUILD_MARKER          (0x50525311U)
#define PRESSURE_USE_M1616_UART        (1U)
#define PRESSURE_UPSTREAM_PCA_ADDR     (0x70U)
#define PRESSURE_UPSTREAM_CHANNEL      (7U)
#define PRESSURE_DOWNSTREAM_PCA_ADDR   (0x71U)
#define PRESSURE_NESTED_PCA_ADDR_AUTO  (0xFFFFFFFFU)
#define PRESSURE_NESTED_PCA_ADDR_FIXED (0x72U)
#define PRESSURE_NESTED_PCA_ADDR_FIRST (0x70U)
#define PRESSURE_NESTED_PCA_ADDR_LAST  (0x77U)
#define PRESSURE_NESTED_HOST_CHANNEL   (7U)
#define PRESSURE_NESTED_UPSTREAM_CHANNEL (5U)
#define PRESSURE_NESTED_FIRST_CHANNEL  (5U)
#define PRESSURE_USE_NESTED_PCA        (0U)
#define PRESSURE_ADS_ADDR_FIRST        (0x48U)
#define PRESSURE_ADS_ADDR_LAST         (0x4BU)
#define PRESSURE_ADS_CHANNELS          (4U)
#define PRESSURE_DOWNSTREAM_CHANNELS   (8U)
#define PRESSURE_UPSTREAM_CHANNEL_AUTO (0xFFFFFFFFU)
#define PRESSURE_I2C_TIMEOUT_MS        (50U)
#define PRESSURE_FAILURE_BACKOFF_MS    (5000U)
#define PRESSURE_PCA_SETTLE_MS         (3U)
#define PRESSURE_ADS_CONVERSION_MS     (10U)
#define PRESSURE_TMP_RECOVERY_MISSES   (3U)
#define PRESSURE_USE_FIXED_UPSTREAM    (1U)
#define PRESSURE_LIMIT_TO_FIRST_CHANNEL (0U)
#define PRESSURE_DOWNSTREAM_SCAN_MASK  (0xFFU)
#define PRESSURE_ADS_CONVERSION_REG    (0x00U)
#define PRESSURE_ADS_CONFIG_REG        (0x01U)
#define PRESSURE_ADS_SINGLE_BASE       (0xC183U)
#define PRESSURE_ADS_MUX_SHIFT         (12U)
#define PRESSURE_ADS_READY_BIT         (0x8000U)

typedef struct st_pressure_i2c_context
{
    volatile bool busy;
    volatile i2c_master_event_t event;
} pressure_i2c_context_t;

volatile pressure_matrix_debug_t g_pressure_debug;

static pressure_i2c_context_t g_pressure_i2c_context;
#if !PRESSURE_USE_M1616_UART
static bool g_pressure_opened;
#endif
static SemaphoreHandle_t g_pressure_i2c_mutex;
static StaticSemaphore_t g_pressure_i2c_mutex_memory;
static uint16_t g_pressure_mapping[PRESSURE_IPC_RAW_COUNT];
#if !PRESSURE_USE_M1616_UART
static uint32_t g_pressure_next_mux_channel;
static uint32_t g_pressure_detected_upstream_channel = PRESSURE_UPSTREAM_CHANNEL_AUTO;
static uint32_t g_pressure_detected_nested_address = PRESSURE_NESTED_PCA_ADDR_FIXED;
static TickType_t g_pressure_backoff_until_tick;
static uint32_t g_pressure_tmp_not_ok_count;
#endif

#if PRESSURE_USE_M1616_UART
#define M1616_RX_BUFFER_SIZE           (8192U)
#define M1616_PACKET_MAX_SIZE          (231U)
#define M1616_HEADER_BYTE              (0xFFU)
#define M1616_MATRIX_ROWS              (16U)
#define M1616_MATRIX_COLS              (16U)
#define M1616_FRAME_PACKET_MASK        (0x07U)
#define M1616_START_COMMAND            "begin\n"
#define M1616_STOP_COMMAND             "end\n"
#define M1616_START_RETRY_MS           (1000U)
#define M1616_FRAME_TIMEOUT_MS         (3000U)

typedef struct st_m1616_context
{
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
    volatile uint32_t rx_bytes;
    volatile uint32_t rx_overflow_count;
    volatile uint32_t uart_error_count;
    volatile uint32_t tx_complete_count;
    volatile uint32_t bad_header_count;
    volatile uint32_t bad_crc_count;
    volatile uint32_t packet_count;
    volatile uint32_t frame_count;
    volatile uint32_t restart_count;
    volatile uint32_t last_event;
    volatile bool tx_done;
    bool opened;
    bool streaming;
    TickType_t last_frame_tick;
    TickType_t last_start_tick;
    uint8_t rx_buffer[M1616_RX_BUFFER_SIZE];
    uint8_t packet[M1616_PACKET_MAX_SIZE];
    uint16_t packet_index;
    uint16_t packet_expected;
    uint8_t packet_mask;
    int16_t matrix[PRESSURE_IPC_MATRIX_COUNT];
} m1616_context_t;

static m1616_context_t g_m1616_context;
#endif

#if !PRESSURE_USE_M1616_UART
static int32_t pressure_raw_to_millivolt(int16_t raw)
{
    return ((int32_t) raw * 1875) / 10000;
}

static uint32_t pressure_next_enabled_channel(uint32_t channel)
{
    for (uint32_t i = 0U; i < PRESSURE_DOWNSTREAM_CHANNELS; i++)
    {
        uint32_t candidate = (channel + i) % PRESSURE_DOWNSTREAM_CHANNELS;
        if (0U != (PRESSURE_DOWNSTREAM_SCAN_MASK & (1U << candidate)))
        {
            return candidate;
        }
    }

    return 0U;
}
#endif

void pressure_i2c_bus_init(void)
{
    if (NULL == g_pressure_i2c_mutex)
    {
        g_pressure_i2c_mutex = xSemaphoreCreateMutexStatic(&g_pressure_i2c_mutex_memory);
    }
}

void pressure_i2c_bus_lock(void)
{
    pressure_i2c_bus_init();
    if (NULL != g_pressure_i2c_mutex)
    {
        (void) xSemaphoreTake(g_pressure_i2c_mutex, portMAX_DELAY);
    }
}

void pressure_i2c_bus_unlock(void)
{
    if (NULL != g_pressure_i2c_mutex)
    {
        (void) xSemaphoreGive(g_pressure_i2c_mutex);
    }
}

static void pressure_debug_reset(void)
{
    memset((void *) &g_pressure_debug, 0, sizeof(g_pressure_debug));
    g_pressure_debug.magic = PRESSURE_DEBUG_MAGIC;
    g_pressure_debug.build_marker = PRESSURE_BUILD_MARKER;
    g_pressure_debug.upstream_pca_address = PRESSURE_UPSTREAM_PCA_ADDR;
    g_pressure_debug.downstream_pca_address = PRESSURE_DOWNSTREAM_PCA_ADDR;
    g_pressure_debug.detected_upstream_channel = PRESSURE_UPSTREAM_CHANNEL_AUTO;

    for (uint32_t i = 0U; i < PRESSURE_DOWNSTREAM_CHANNELS; i++)
    {
        g_pressure_debug.ads_address[i] = PRESSURE_ADS_ADDR_FIRST;
    }
}

void pressure_matrix_reset_mapping(void)
{
    for (uint32_t i = 0U; i < PRESSURE_IPC_RAW_COUNT; i++)
    {
        g_pressure_mapping[i] = (uint16_t) i;
    }
}

fsp_err_t pressure_matrix_set_mapping(uint32_t raw_index, uint32_t matrix_index)
{
    if ((raw_index >= PRESSURE_IPC_RAW_COUNT) || (matrix_index >= PRESSURE_IPC_MATRIX_COUNT))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    g_pressure_mapping[raw_index] = (uint16_t) matrix_index;
    return FSP_SUCCESS;
}

#if !PRESSURE_USE_M1616_UART
static void pressure_publish(uint32_t connected_mask, uint32_t error_mask, fsp_err_t err)
{
    volatile pressure_ipc_mailbox_t * p_mailbox = PRESSURE_IPC_MAILBOX;

    p_mailbox->magic = PRESSURE_IPC_MAGIC;
    p_mailbox->raw_count = PRESSURE_IPC_RAW_COUNT;
    p_mailbox->connected_mask = connected_mask;
    p_mailbox->error_mask = error_mask;
    p_mailbox->fail_step = (uint32_t) g_pressure_debug.fail_step;
    p_mailbox->fail_channel = g_pressure_debug.fail_channel;
    p_mailbox->upstream_pca_address = PRESSURE_UPSTREAM_PCA_ADDR;
    p_mailbox->downstream_pca_address = PRESSURE_DOWNSTREAM_PCA_ADDR;
    p_mailbox->detected_upstream_channel = g_pressure_detected_upstream_channel;
    p_mailbox->last_fsp_err = (uint32_t) err;

    if (0U == connected_mask)
    {
        p_mailbox->status = (uint32_t) PRESSURE_IPC_STATUS_SCAN_ERROR;
    }
    else if (0U != error_mask)
    {
        p_mailbox->status = (uint32_t) PRESSURE_IPC_STATUS_SCAN_PARTIAL;
    }
    else
    {
        p_mailbox->status = (uint32_t) PRESSURE_IPC_STATUS_SCAN_OK;
    }

    for (uint32_t i = 0U; i < PRESSURE_DOWNSTREAM_CHANNELS; i++)
    {
        p_mailbox->ads_address[i] = g_pressure_debug.ads_address[i];
    }

    for (uint32_t i = 0U; i < PRESSURE_IPC_RAW_COUNT; i++)
    {
        p_mailbox->raw[i] = g_pressure_debug.raw[i];
        p_mailbox->millivolt[i] = g_pressure_debug.millivolt[i];
    }

    for (uint32_t i = 0U; i < PRESSURE_IPC_MATRIX_COUNT; i++)
    {
        p_mailbox->matrix[i] = 0;
    }

    for (uint32_t i = 0U; i < PRESSURE_IPC_RAW_COUNT; i++)
    {
        p_mailbox->matrix[g_pressure_mapping[i]] = g_pressure_debug.raw[i];
    }

    __DMB();
    p_mailbox->sequence++;
}

static fsp_err_t pressure_i2c_wait(i2c_master_event_t expected_event)
{
    TickType_t start_tick = xTaskGetTickCount();

    while (g_pressure_i2c_context.busy)
    {
        if ((xTaskGetTickCount() - start_tick) >= pdMS_TO_TICKS(PRESSURE_I2C_TIMEOUT_MS))
        {
            (void) g_temp_i2c.p_api->abort(g_temp_i2c.p_ctrl);
            g_pressure_i2c_context.busy = false;
            g_pressure_debug.last_fsp_err = FSP_ERR_TIMEOUT;
            return FSP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (expected_event != g_pressure_i2c_context.event)
    {
        g_pressure_debug.last_fsp_err = FSP_ERR_ABORTED;
        return FSP_ERR_ABORTED;
    }

    return FSP_SUCCESS;
}

static fsp_err_t pressure_i2c_recover(void)
{
    g_pressure_i2c_context.busy = false;
    g_pressure_i2c_context.event = (i2c_master_event_t) 0;

    (void) g_temp_i2c.p_api->abort(g_temp_i2c.p_ctrl);
    vTaskDelay(pdMS_TO_TICKS(2));

    return FSP_SUCCESS;
}

static fsp_err_t pressure_release_i2c_to_tmp117(void)
{
    return pressure_i2c_recover();
}

static fsp_err_t pressure_i2c_set_address(uint32_t address)
{
    fsp_err_t err = g_temp_i2c.p_api->slaveAddressSet(g_temp_i2c.p_ctrl, address, I2C_MASTER_ADDR_MODE_7BIT);
    g_pressure_debug.last_fsp_err = err;
    return err;
}

static fsp_err_t pressure_i2c_write(uint8_t * p_data, uint32_t length, bool restart)
{
    fsp_err_t err;

    g_pressure_i2c_context.event = (i2c_master_event_t) 0;
    g_pressure_i2c_context.busy = true;
    err = g_temp_i2c.p_api->write(g_temp_i2c.p_ctrl, p_data, length, restart);
    if (FSP_SUCCESS != err)
    {
        g_pressure_i2c_context.busy = false;
        g_pressure_debug.last_fsp_err = err;
        (void) pressure_i2c_recover();
        return err;
    }

    err = pressure_i2c_wait(I2C_MASTER_EVENT_TX_COMPLETE);
    if (FSP_SUCCESS != err)
    {
        (void) pressure_i2c_recover();
    }

    return err;
}

static fsp_err_t pressure_i2c_read(uint8_t * p_data, uint32_t length, bool restart)
{
    fsp_err_t err;

    g_pressure_i2c_context.event = (i2c_master_event_t) 0;
    g_pressure_i2c_context.busy = true;
    err = g_temp_i2c.p_api->read(g_temp_i2c.p_ctrl, p_data, length, restart);
    if (FSP_SUCCESS != err)
    {
        g_pressure_i2c_context.busy = false;
        g_pressure_debug.last_fsp_err = err;
        (void) pressure_i2c_recover();
        return err;
    }

    err = pressure_i2c_wait(I2C_MASTER_EVENT_RX_COMPLETE);
    if (FSP_SUCCESS != err)
    {
        (void) pressure_i2c_recover();
    }

    return err;
}

static fsp_err_t pressure_select_pca(uint32_t address, uint32_t channel, pressure_matrix_step_t step)
{
    uint8_t select = (uint8_t) (1U << channel);
    fsp_err_t err;

    g_pressure_debug.step = step;
    g_pressure_debug.channel = channel;

    err = pressure_i2c_set_address(address);
    if (FSP_SUCCESS == err)
    {
        err = pressure_i2c_write(&select, 1U, false);
    }

    return err;
}

static fsp_err_t pressure_write_pca_mask(uint32_t address, uint8_t mask)
{
    fsp_err_t err = pressure_i2c_set_address(address);
    if (FSP_SUCCESS == err)
    {
        err = pressure_i2c_write(&mask, 1U, false);
    }

    return err;
}

static void pressure_close_pressure_path(void)
{
    fsp_err_t saved_err = g_pressure_debug.last_fsp_err;
    pressure_matrix_step_t saved_step = g_pressure_debug.step;
    uint32_t saved_channel = g_pressure_debug.channel;

    (void) pressure_write_pca_mask(PRESSURE_DOWNSTREAM_PCA_ADDR, 0U);

    if (0U != PRESSURE_USE_NESTED_PCA)
    {
        uint32_t nested_address = g_pressure_detected_nested_address;
        if ((PRESSURE_NESTED_PCA_ADDR_AUTO != nested_address) &&
            (PRESSURE_UPSTREAM_PCA_ADDR != nested_address) &&
            (PRESSURE_DOWNSTREAM_PCA_ADDR != nested_address))
        {
            (void) pressure_write_pca_mask(nested_address, 0U);
        }
    }

    (void) pressure_write_pca_mask(PRESSURE_UPSTREAM_PCA_ADDR, 0U);

    g_pressure_debug.last_fsp_err = saved_err;
    g_pressure_debug.step = saved_step;
    g_pressure_debug.channel = saved_channel;
    vTaskDelay(pdMS_TO_TICKS(PRESSURE_PCA_SETTLE_MS));
}

static void pressure_reset_scan_state(fsp_err_t err)
{
    g_pressure_next_mux_channel = pressure_next_enabled_channel(0U);
    g_pressure_backoff_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(PRESSURE_FAILURE_BACKOFF_MS);
    g_pressure_debug.connected_mask = 0U;
    g_pressure_debug.error_mask = 0U;
    g_pressure_debug.fail_step = PRESSURE_MATRIX_STEP_NONE;
    g_pressure_debug.fail_channel = 0U;
    g_pressure_debug.last_fsp_err = err;
}

static fsp_err_t pressure_select_path(uint32_t downstream_channel)
{
    fsp_err_t err;
    uint32_t upstream_channel = g_pressure_detected_upstream_channel;

#if PRESSURE_USE_FIXED_UPSTREAM
    if (PRESSURE_UPSTREAM_CHANNEL_AUTO == upstream_channel)
    {
        g_pressure_detected_upstream_channel = PRESSURE_UPSTREAM_CHANNEL;
        g_pressure_debug.detected_upstream_channel = PRESSURE_UPSTREAM_CHANNEL;
        upstream_channel = PRESSURE_UPSTREAM_CHANNEL;
    }
#else
    if (PRESSURE_UPSTREAM_CHANNEL_AUTO == upstream_channel)
    {
        g_pressure_debug.step = PRESSURE_MATRIX_STEP_DETECT_UPSTREAM;

        for (uint32_t i = 0U; i < (sizeof(g_pressure_upstream_probe_channels) /
                                    sizeof(g_pressure_upstream_probe_channels[0])); i++)
        {
            uint32_t channel = g_pressure_upstream_probe_channels[i];
            err = pressure_select_pca(PRESSURE_UPSTREAM_PCA_ADDR,
                                      channel,
                                      PRESSURE_MATRIX_STEP_DETECT_UPSTREAM);
            if (FSP_SUCCESS != err)
            {
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(1));

            err = pressure_select_pca(PRESSURE_DOWNSTREAM_PCA_ADDR,
                                      0U,
                                      PRESSURE_MATRIX_STEP_SELECT_DOWNSTREAM);
            if (FSP_SUCCESS == err)
            {
                g_pressure_detected_upstream_channel = channel;
                g_pressure_debug.detected_upstream_channel = channel;
                upstream_channel = channel;
                break;
            }
        }

        if (PRESSURE_UPSTREAM_CHANNEL_AUTO == upstream_channel)
        {
            g_pressure_debug.detected_upstream_channel = PRESSURE_UPSTREAM_CHANNEL_AUTO;
            return FSP_ERR_NOT_FOUND;
        }
    }
#endif

    err = pressure_select_pca(PRESSURE_UPSTREAM_PCA_ADDR,
                              upstream_channel,
                              PRESSURE_MATRIX_STEP_SELECT_UPSTREAM);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(PRESSURE_PCA_SETTLE_MS));

    if ((0U != PRESSURE_USE_NESTED_PCA) && (downstream_channel >= PRESSURE_NESTED_FIRST_CHANNEL))
    {
        uint32_t nested_address = g_pressure_detected_nested_address;

        err = pressure_select_pca(PRESSURE_UPSTREAM_PCA_ADDR,
                                  PRESSURE_NESTED_UPSTREAM_CHANNEL,
                                  PRESSURE_MATRIX_STEP_SELECT_UPSTREAM);
        if (FSP_SUCCESS != err)
        {
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(PRESSURE_PCA_SETTLE_MS));

        if (PRESSURE_NESTED_PCA_ADDR_AUTO == nested_address)
        {
            for (uint32_t address = PRESSURE_NESTED_PCA_ADDR_FIRST;
                 address <= PRESSURE_NESTED_PCA_ADDR_LAST;
                 address++)
            {
                if ((PRESSURE_UPSTREAM_PCA_ADDR == address) ||
                    (PRESSURE_DOWNSTREAM_PCA_ADDR == address))
                {
                    continue;
                }

                err = pressure_select_pca(address,
                                          downstream_channel - PRESSURE_NESTED_FIRST_CHANNEL,
                                          PRESSURE_MATRIX_STEP_SELECT_DOWNSTREAM);
                if (FSP_SUCCESS == err)
                {
                    g_pressure_detected_nested_address = address;
                    nested_address = address;
                    break;
                }
            }

            if (PRESSURE_NESTED_PCA_ADDR_AUTO == nested_address)
            {
                return FSP_ERR_NOT_FOUND;
            }
        }

        return pressure_select_pca(nested_address,
                                   downstream_channel - PRESSURE_NESTED_FIRST_CHANNEL,
                                   PRESSURE_MATRIX_STEP_SELECT_DOWNSTREAM);
    }

    return pressure_select_pca(PRESSURE_DOWNSTREAM_PCA_ADDR,
                               downstream_channel,
                               PRESSURE_MATRIX_STEP_SELECT_DOWNSTREAM);
}

static fsp_err_t pressure_ads_write_config(uint32_t address, uint32_t adc_channel)
{
    uint16_t config = (uint16_t) ((uint32_t) PRESSURE_ADS_SINGLE_BASE |
                                  (((uint32_t) (4U + adc_channel)) << PRESSURE_ADS_MUX_SHIFT));
    uint8_t data[3];
    fsp_err_t err;

    data[0] = PRESSURE_ADS_CONFIG_REG;
    data[1] = (uint8_t) (config >> 8);
    data[2] = (uint8_t) config;

    g_pressure_debug.step = PRESSURE_MATRIX_STEP_CONFIG_ADS;
    g_pressure_debug.adc_channel = adc_channel;

    err = pressure_i2c_set_address(address);
    if (FSP_SUCCESS == err)
    {
        err = pressure_i2c_write(data, sizeof(data), false);
    }

    return err;
}

static fsp_err_t pressure_ads_read_register(uint32_t address, uint8_t reg, uint16_t * p_value)
{
    uint8_t data[2] = {0U, 0U};
    fsp_err_t err;

    err = pressure_i2c_set_address(address);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = pressure_i2c_write(&reg, 1U, false);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(PRESSURE_PCA_SETTLE_MS));

    err = pressure_i2c_read(data, sizeof(data), false);
    if (FSP_SUCCESS == err)
    {
        *p_value = (uint16_t) (((uint16_t) data[0] << 8) | data[1]);
    }

    return err;
}

static fsp_err_t pressure_ads_read_single(uint32_t address, uint32_t adc_channel, int16_t * p_raw)
{
    uint16_t value = 0U;
    fsp_err_t err;

    err = pressure_ads_write_config(address, adc_channel);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(PRESSURE_ADS_CONVERSION_MS));

    g_pressure_debug.step = PRESSURE_MATRIX_STEP_READ_ADS;
    err = pressure_ads_read_register(address, PRESSURE_ADS_CONVERSION_REG, &value);
    if (FSP_SUCCESS == err)
    {
        *p_raw = (int16_t) value;
    }

    return err;
}
#endif

#if PRESSURE_USE_M1616_UART
static uint16_t m1616_next_index(uint16_t index)
{
    return (uint16_t) ((index + 1U) % M1616_RX_BUFFER_SIZE);
}

static void m1616_push_byte(uint8_t byte)
{
    uint16_t next_head = m1616_next_index(g_m1616_context.rx_head);

    if (next_head == g_m1616_context.rx_tail)
    {
        g_m1616_context.rx_overflow_count++;
        return;
    }

    g_m1616_context.rx_buffer[g_m1616_context.rx_head] = byte;
    g_m1616_context.rx_head = next_head;
    g_m1616_context.rx_bytes++;
}

static bool m1616_pop_byte(uint8_t * p_byte)
{
    if (g_m1616_context.rx_tail == g_m1616_context.rx_head)
    {
        return false;
    }

    *p_byte = g_m1616_context.rx_buffer[g_m1616_context.rx_tail];
    g_m1616_context.rx_tail = m1616_next_index(g_m1616_context.rx_tail);
    return true;
}

static uint8_t m1616_crc8_update(uint8_t crc, uint8_t data)
{
    crc ^= data;
    for (uint32_t bit = 0U; bit < 8U; bit++)
    {
        if (0U != (crc & 0x80U))
        {
            crc = (uint8_t) ((((uint32_t) crc) << 1) ^ 0x07U);
        }
        else
        {
            crc = (uint8_t) (((uint32_t) crc) << 1);
        }
    }

    return crc;
}

static uint8_t m1616_packet_crc(uint8_t const * p_packet, uint16_t packet_size)
{
    uint8_t crc = 0xFFU;

    for (uint16_t i = 0U; i < 6U; i++)
    {
        crc = m1616_crc8_update(crc, p_packet[i]);
    }

    for (uint16_t i = 7U; i < packet_size; i++)
    {
        crc = m1616_crc8_update(crc, p_packet[i]);
    }

    return crc;
}

static uint16_t m1616_packet_data_length(uint8_t packet_id)
{
    if (packet_id < 2U)
    {
        return 224U;
    }

    if (2U == packet_id)
    {
        return 64U;
    }

    return 0U;
}

static void m1616_reset_parser(void)
{
    g_m1616_context.packet_index = 0U;
    g_m1616_context.packet_expected = 0U;
}

static void m1616_flush_rx(void)
{
    g_m1616_context.rx_tail = g_m1616_context.rx_head;
    m1616_reset_parser();
    g_m1616_context.packet_mask = 0U;
}

static void pressure_publish_m1616(fsp_err_t err, pressure_ipc_status_t status, bool frame_complete)
{
    volatile pressure_ipc_mailbox_t * p_mailbox = PRESSURE_IPC_MAILBOX;

    p_mailbox->magic = PRESSURE_IPC_MAGIC;
    p_mailbox->raw_count = PRESSURE_IPC_RAW_COUNT;
    p_mailbox->connected_mask = (uint32_t) g_m1616_context.packet_mask;
    p_mailbox->error_mask = (PRESSURE_IPC_STATUS_SCAN_OK == status) ? 0U : 1U;
    p_mailbox->fail_step = (uint32_t) g_pressure_debug.fail_step;
    p_mailbox->fail_channel = g_pressure_debug.fail_channel;
    p_mailbox->upstream_pca_address = 0U;
    p_mailbox->downstream_pca_address = 0U;
    p_mailbox->detected_upstream_channel = g_m1616_context.restart_count;
    p_mailbox->last_fsp_err = (uint32_t) err;
    p_mailbox->status = (uint32_t) status;

    for (uint32_t i = 0U; i < PRESSURE_DOWNSTREAM_CHANNELS; i++)
    {
        p_mailbox->ads_address[i] = 0U;
    }

    if (!frame_complete)
    {
        __DMB();
        return;
    }

    for (uint32_t i = 0U; i < PRESSURE_IPC_RAW_COUNT; i++)
    {
        p_mailbox->raw[i] = g_m1616_context.matrix[i];
        p_mailbox->millivolt[i] = (int32_t) g_m1616_context.matrix[i];
    }

    for (uint32_t i = 0U; i < PRESSURE_IPC_MATRIX_COUNT; i++)
    {
        p_mailbox->matrix[i] = g_m1616_context.matrix[i];
    }

    __DMB();
    p_mailbox->sequence++;
}

static bool m1616_accept_packet(void)
{
    uint8_t const * p_packet = g_m1616_context.packet;
    uint8_t packet_id = p_packet[5];
    uint16_t data_length = m1616_packet_data_length(packet_id);
    uint16_t packet_size = (uint16_t) (7U + data_length);

    if ((M1616_MATRIX_ROWS != p_packet[2]) || (M1616_MATRIX_COLS != p_packet[3]) ||
        (0U == data_length) || (packet_size != g_m1616_context.packet_expected))
    {
        g_m1616_context.bad_header_count++;
        return false;
    }

    if (m1616_packet_crc(p_packet, packet_size) != p_packet[6])
    {
        g_m1616_context.bad_crc_count++;
        return false;
    }

    uint32_t start_row = (uint32_t) packet_id * 7U;
    uint16_t value_count = (uint16_t) (data_length / 2U);

    for (uint16_t i = 0U; i < value_count; i++)
    {
        uint32_t row = start_row + ((uint32_t) i / M1616_MATRIX_COLS);
        uint32_t col = (uint32_t) i % M1616_MATRIX_COLS;

        if (row < M1616_MATRIX_ROWS)
        {
            uint16_t lo = p_packet[7U + (i * 2U)];
            uint16_t hi = p_packet[8U + (i * 2U)];
            g_m1616_context.matrix[(row * M1616_MATRIX_COLS) + col] = (int16_t) ((hi << 8) | lo);
        }
    }

    g_m1616_context.packet_count++;
    g_m1616_context.packet_mask |= (uint8_t) (1U << packet_id);
    if (M1616_FRAME_PACKET_MASK == (g_m1616_context.packet_mask & M1616_FRAME_PACKET_MASK))
    {
        g_m1616_context.frame_count++;
        g_m1616_context.last_frame_tick = xTaskGetTickCount();
        g_pressure_debug.sequence = g_m1616_context.frame_count;
        g_pressure_debug.step = PRESSURE_MATRIX_STEP_SCAN_DONE;
        g_pressure_debug.connected_mask = M1616_FRAME_PACKET_MASK;
        g_pressure_debug.error_mask = 0U;
        g_pressure_debug.last_fsp_err = FSP_SUCCESS;

        for (uint32_t i = 0U; i < PRESSURE_IPC_RAW_COUNT; i++)
        {
            g_pressure_debug.raw[i] = g_m1616_context.matrix[i];
            g_pressure_debug.millivolt[i] = (int32_t) g_m1616_context.matrix[i];
        }

        pressure_publish_m1616(FSP_SUCCESS, PRESSURE_IPC_STATUS_SCAN_OK, true);
        g_m1616_context.packet_mask = 0U;
        return true;
    }

    return false;
}

static bool m1616_parse_byte(uint8_t byte)
{
    uint16_t index = g_m1616_context.packet_index;

    if (0U == index)
    {
        if (M1616_HEADER_BYTE != byte)
        {
            g_m1616_context.bad_header_count++;
            return false;
        }

        g_m1616_context.packet[index] = byte;
        g_m1616_context.packet_index = 1U;
        return false;
    }

    if (1U == index)
    {
        if (M1616_HEADER_BYTE != byte)
        {
            g_m1616_context.bad_header_count++;
            m1616_reset_parser();
            return false;
        }

        g_m1616_context.packet[index] = byte;
        g_m1616_context.packet_index = 2U;
        return false;
    }

    if (index >= M1616_PACKET_MAX_SIZE)
    {
        g_m1616_context.bad_header_count++;
        m1616_reset_parser();
        return false;
    }

    g_m1616_context.packet[index] = byte;
    g_m1616_context.packet_index++;

    if (6U == g_m1616_context.packet_index)
    {
        uint16_t data_length = m1616_packet_data_length(g_m1616_context.packet[5]);
        if (0U == data_length)
        {
            g_m1616_context.bad_header_count++;
            m1616_reset_parser();
            return false;
        }

        g_m1616_context.packet_expected = (uint16_t) (7U + data_length);
    }

    if ((0U != g_m1616_context.packet_expected) &&
        (g_m1616_context.packet_index >= g_m1616_context.packet_expected))
    {
        bool frame_ready = m1616_accept_packet();
        m1616_reset_parser();
        return frame_ready;
    }

    return false;
}

static uint32_t m1616_drain_rx(uint32_t max_bytes)
{
    uint32_t parsed = 0U;
    uint8_t byte = 0U;

    while ((parsed < max_bytes) && m1616_pop_byte(&byte))
    {
        (void) m1616_parse_byte(byte);
        parsed++;
    }

    return parsed;
}

static fsp_err_t m1616_write_command(char const * p_command, uint32_t length)
{
    g_m1616_context.tx_done = false;
    fsp_err_t err = g_m1616_uart.p_api->write(g_m1616_uart.p_ctrl, (uint8_t const *) p_command, length);
    if (FSP_SUCCESS != err)
    {
        g_m1616_context.tx_done = true;
        return err;
    }

    TickType_t start_tick = xTaskGetTickCount();
    while (!g_m1616_context.tx_done)
    {
        if ((xTaskGetTickCount() - start_tick) >= pdMS_TO_TICKS(100))
        {
            g_m1616_context.tx_done = true;
            return FSP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return FSP_SUCCESS;
}

static fsp_err_t m1616_send_start(bool count_restart)
{
    fsp_err_t err;

    m1616_flush_rx();
    g_m1616_context.last_start_tick = xTaskGetTickCount();
    err = m1616_write_command(M1616_START_COMMAND, (uint32_t) (sizeof(M1616_START_COMMAND) - 1U));
    if (FSP_SUCCESS == err)
    {
        g_m1616_context.streaming = true;
        if (count_restart)
        {
            g_m1616_context.restart_count++;
        }
    }
    else
    {
        g_m1616_context.streaming = false;
    }

    return err;
}

fsp_err_t pressure_matrix_init(void)
{
    volatile pressure_ipc_mailbox_t * p_mailbox = PRESSURE_IPC_MAILBOX;
    fsp_err_t err = FSP_SUCCESS;

    pressure_i2c_bus_init();
    pressure_matrix_reset_mapping();
    pressure_debug_reset();
    memset(&g_m1616_context, 0, sizeof(g_m1616_context));
    g_m1616_context.tx_done = true;

    p_mailbox->magic = PRESSURE_IPC_MAGIC;
    p_mailbox->status = (uint32_t) PRESSURE_IPC_STATUS_EMPTY;
    p_mailbox->sequence = 0U;
    p_mailbox->raw_count = PRESSURE_IPC_RAW_COUNT;

    g_pressure_debug.step = PRESSURE_MATRIX_STEP_OPEN;
    err = g_m1616_uart.p_api->open(g_m1616_uart.p_ctrl, g_m1616_uart.p_cfg);
    if ((FSP_SUCCESS == err) || (FSP_ERR_ALREADY_OPEN == err))
    {
        g_m1616_context.opened = true;
        err = FSP_SUCCESS;
    }

    g_pressure_debug.last_fsp_err = err;
    if (FSP_SUCCESS != err)
    {
        pressure_publish_m1616(err, PRESSURE_IPC_STATUS_OPEN_ERROR, false);
        return err;
    }

    g_m1616_context.last_frame_tick = xTaskGetTickCount();
    err = m1616_send_start(false);

    g_pressure_debug.last_fsp_err = err;
    if (FSP_SUCCESS != err)
    {
        pressure_publish_m1616(err, PRESSURE_IPC_STATUS_SCAN_ERROR, false);
    }

    return err;
}

fsp_err_t pressure_matrix_scan(void)
{
    if (!g_m1616_context.opened)
    {
        fsp_err_t init_err = pressure_matrix_init();
        if (FSP_SUCCESS != init_err)
        {
            return init_err;
        }
    }

    TickType_t now = xTaskGetTickCount();

    if (!g_m1616_context.streaming)
    {
        if ((now - g_m1616_context.last_start_tick) < pdMS_TO_TICKS(M1616_START_RETRY_MS))
        {
            pressure_publish_m1616(FSP_ERR_IN_USE, PRESSURE_IPC_STATUS_SCAN_PARTIAL, false);
            return FSP_ERR_IN_USE;
        }

        fsp_err_t start_err = m1616_send_start(true);
        if (FSP_SUCCESS != start_err)
        {
            g_pressure_debug.last_fsp_err = start_err;
            pressure_publish_m1616(start_err, PRESSURE_IPC_STATUS_SCAN_ERROR, false);
            return start_err;
        }
    }

    uint32_t before_frames = g_m1616_context.frame_count;
    uint32_t drained = m1616_drain_rx(2048U);

    if (((now - g_m1616_context.last_frame_tick) >= pdMS_TO_TICKS(M1616_FRAME_TIMEOUT_MS)) &&
        ((now - g_m1616_context.last_start_tick) >= pdMS_TO_TICKS(M1616_START_RETRY_MS)))
    {
        fsp_err_t restart_err = m1616_send_start(true);
        g_pressure_debug.last_fsp_err = restart_err;
        if (FSP_SUCCESS != restart_err)
        {
            pressure_publish_m1616(restart_err, PRESSURE_IPC_STATUS_SCAN_ERROR, false);
            return restart_err;
        }
    }

    g_pressure_debug.channel = g_m1616_context.packet_mask;
    g_pressure_debug.adc_channel = drained;
    g_pressure_debug.connected_mask = (uint32_t) g_m1616_context.packet_mask;
    g_pressure_debug.error_mask = g_m1616_context.bad_crc_count + g_m1616_context.rx_overflow_count;
    g_pressure_debug.fail_step = PRESSURE_MATRIX_STEP_READ_ADS;
    g_pressure_debug.fail_channel = g_m1616_context.bad_header_count;
    g_pressure_debug.last_fsp_err = FSP_SUCCESS;

    if (g_m1616_context.frame_count == before_frames)
    {
        pressure_publish_m1616(FSP_ERR_IN_USE, PRESSURE_IPC_STATUS_SCAN_PARTIAL, false);
    }

    return FSP_SUCCESS;
}

#else
fsp_err_t pressure_matrix_init(void)
{
    fsp_err_t err;
    volatile pressure_ipc_mailbox_t * p_mailbox = PRESSURE_IPC_MAILBOX;

    pressure_i2c_bus_init();
    pressure_matrix_reset_mapping();
    pressure_debug_reset();
    g_pressure_next_mux_channel = 0U;
    g_pressure_detected_upstream_channel = PRESSURE_UPSTREAM_CHANNEL_AUTO;
    g_pressure_detected_nested_address = PRESSURE_NESTED_PCA_ADDR_FIXED;
    g_pressure_backoff_until_tick = 0U;
    g_pressure_tmp_not_ok_count = 0U;

    p_mailbox->magic = PRESSURE_IPC_MAGIC;
    p_mailbox->status = (uint32_t) PRESSURE_IPC_STATUS_EMPTY;
    p_mailbox->sequence = 0U;
    p_mailbox->raw_count = PRESSURE_IPC_RAW_COUNT;

    if (g_pressure_opened)
    {
        return FSP_SUCCESS;
    }

    pressure_i2c_bus_lock();

    g_pressure_debug.step = PRESSURE_MATRIX_STEP_OPEN;
    err = g_temp_i2c.p_api->open(g_temp_i2c.p_ctrl, g_temp_i2c.p_cfg);
    if ((FSP_SUCCESS == err) || (FSP_ERR_ALREADY_OPEN == err))
    {
        g_pressure_opened = true;
        err = FSP_SUCCESS;
    }

    g_pressure_debug.last_fsp_err = err;
    if (FSP_SUCCESS != err)
    {
        p_mailbox->status = (uint32_t) PRESSURE_IPC_STATUS_OPEN_ERROR;
        p_mailbox->last_fsp_err = (uint32_t) err;
    }

    pressure_i2c_bus_unlock();

    return err;
}

fsp_err_t pressure_matrix_scan(void)
{
    uint32_t connected_mask = g_pressure_debug.connected_mask;
    uint32_t error_mask = g_pressure_debug.error_mask;
#if PRESSURE_LIMIT_TO_FIRST_CHANNEL
    uint32_t mux_channel = 0U;
#else
    uint32_t mux_channel = pressure_next_enabled_channel(g_pressure_next_mux_channel);
#endif
    uint32_t channel_mask = 1U << mux_channel;
    uint32_t ads_address = PRESSURE_ADS_ADDR_FIRST;
    fsp_err_t last_err = FSP_SUCCESS;
    fsp_err_t err = FSP_SUCCESS;
    pressure_matrix_step_t fail_step = PRESSURE_MATRIX_STEP_DETECT_UPSTREAM;
    TickType_t now = xTaskGetTickCount();
    volatile tmp117_ipc_mailbox_t const * p_tmp = TMP117_IPC_MAILBOX;

    if ((TMP117_IPC_MAGIC != p_tmp->magic) ||
        ((uint32_t) TMP117_IPC_STATUS_SCAN_OK != p_tmp->status))
    {
        g_pressure_tmp_not_ok_count++;
        if ((g_pressure_tmp_not_ok_count >= PRESSURE_TMP_RECOVERY_MISSES) && g_pressure_opened)
        {
            pressure_i2c_bus_lock();
            pressure_close_pressure_path();
            (void) pressure_i2c_recover();
            pressure_reset_scan_state(FSP_ERR_IN_USE);
            pressure_i2c_bus_unlock();

            connected_mask = 0U;
            error_mask = 0U;
        }

        pressure_publish(connected_mask, error_mask, g_pressure_debug.last_fsp_err);
        return FSP_ERR_IN_USE;
    }

    g_pressure_tmp_not_ok_count = 0U;

    if ((0U != g_pressure_backoff_until_tick) &&
        ((int32_t) (g_pressure_backoff_until_tick - now) > 0))
    {
        pressure_publish(connected_mask, error_mask, g_pressure_debug.last_fsp_err);
        return FSP_ERR_IN_USE;
    }

    if (!g_pressure_opened)
    {
        last_err = pressure_matrix_init();
        if (FSP_SUCCESS != last_err)
        {
            return last_err;
        }
    }

    pressure_i2c_bus_lock();

    connected_mask &= ~channel_mask;
    error_mask &= ~channel_mask;

    err = pressure_select_path(mux_channel);
    fail_step = PRESSURE_MATRIX_STEP_SELECT_DOWNSTREAM;

    if (FSP_SUCCESS == err)
    {
        vTaskDelay(pdMS_TO_TICKS(PRESSURE_PCA_SETTLE_MS));
        bool address_found = false;

        for (uint32_t address = PRESSURE_ADS_ADDR_FIRST; address <= PRESSURE_ADS_ADDR_LAST; address++)
        {
            uint16_t config = 0U;
            err = pressure_ads_read_register(address, PRESSURE_ADS_CONFIG_REG, &config);
            if (FSP_SUCCESS == err)
            {
                ads_address = address;
                address_found = true;
                break;
            }
        }

        if (!address_found)
        {
            err = FSP_ERR_NOT_FOUND;
        }

        fail_step = PRESSURE_MATRIX_STEP_CONFIG_ADS;
    }

    if (FSP_SUCCESS == err)
    {
        g_pressure_debug.ads_address[mux_channel] = ads_address;

        for (uint32_t adc_channel = 0U; adc_channel < PRESSURE_ADS_CHANNELS; adc_channel++)
        {
            uint32_t raw_index = (mux_channel * PRESSURE_ADS_CHANNELS) + adc_channel;
            int16_t raw = 0;

            err = pressure_ads_read_single(ads_address, adc_channel, &raw);
            if (FSP_SUCCESS != err)
            {
                break;
            }

            g_pressure_debug.raw[raw_index] = raw;
            g_pressure_debug.millivolt[raw_index] = pressure_raw_to_millivolt(raw);
        }

        fail_step = PRESSURE_MATRIX_STEP_READ_ADS;
    }

    if (FSP_SUCCESS == err)
    {
        connected_mask |= channel_mask;
        if (0U == (error_mask & ~channel_mask))
        {
            g_pressure_backoff_until_tick = 0U;
        }
    }
    else
    {
        error_mask |= channel_mask;
        last_err = err;
        g_pressure_debug.fail_step = fail_step;
        g_pressure_debug.fail_channel = mux_channel;
        g_pressure_debug.last_fsp_err = err;
        pressure_close_pressure_path();
        (void) pressure_i2c_recover();

        if ((FSP_ERR_IN_USE == err) ||
            (FSP_ERR_ABORTED == err) ||
            (FSP_ERR_TIMEOUT == err) ||
            (PRESSURE_DOWNSTREAM_CHANNELS ==
            __builtin_popcount((unsigned int) (error_mask & ((1U << PRESSURE_DOWNSTREAM_CHANNELS) - 1U))))
        )
        {
            pressure_reset_scan_state(err);
            connected_mask = 0U;
            error_mask = 0U;
        }
    }

#if PRESSURE_LIMIT_TO_FIRST_CHANNEL
    g_pressure_next_mux_channel = 0U;
#else
    g_pressure_next_mux_channel = pressure_next_enabled_channel((mux_channel + 1U) % PRESSURE_DOWNSTREAM_CHANNELS);
#endif

    g_pressure_debug.step = PRESSURE_MATRIX_STEP_SCAN_DONE;
    g_pressure_debug.connected_mask = connected_mask;
    g_pressure_debug.error_mask = error_mask;
    g_pressure_debug.sequence++;

    pressure_publish(connected_mask, error_mask, last_err);

    pressure_close_pressure_path();
    (void) pressure_release_i2c_to_tmp117();

    pressure_i2c_bus_unlock();

    return (0U == error_mask) ? FSP_SUCCESS : last_err;
}
#endif

void pressure_i2c_callback(i2c_master_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    if (!g_pressure_i2c_context.busy)
    {
        return;
    }

    g_pressure_i2c_context.event = p_args->event;

    if ((I2C_MASTER_EVENT_TX_COMPLETE == p_args->event) ||
        (I2C_MASTER_EVENT_RX_COMPLETE == p_args->event) ||
        (I2C_MASTER_EVENT_ABORTED == p_args->event))
    {
        g_pressure_i2c_context.busy = false;
    }
}

#if PRESSURE_USE_M1616_UART
void m1616_uart_callback(uart_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    g_m1616_context.last_event = (uint32_t) p_args->event;

    switch (p_args->event)
    {
        case UART_EVENT_RX_CHAR:
        {
            m1616_push_byte((uint8_t) p_args->data);
            break;
        }

        case UART_EVENT_TX_COMPLETE:
        {
            g_m1616_context.tx_complete_count++;
            g_m1616_context.tx_done = true;
            break;
        }

        case UART_EVENT_ERR_PARITY:
        case UART_EVENT_ERR_FRAMING:
        case UART_EVENT_ERR_OVERFLOW:
        case UART_EVENT_BREAK_DETECT:
        {
            g_m1616_context.uart_error_count++;
            m1616_push_byte((uint8_t) p_args->data);
            break;
        }

        default:
        {
            break;
        }
    }
}
#endif

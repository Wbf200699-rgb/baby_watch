#include "thread_code/story_bulk_uart.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "r_sci_b_uart.h"
#include "thread_code/audio_ipc_shared.h"
#include "thread_code/audio_wav_player.h"
#include "vector_data.h"

#define STORY_BULK_DEBUG_MAGIC       (0x53425532U) /* SBU2 */
#define STORY_BULK_MAGIC             (0x53424C32U) /* SBL2 */
#define STORY_BULK_VERSION           (2U)
#define STORY_BULK_HEADER_BYTES      (32U)
#define STORY_BULK_TYPE_PING         (1U)
#define STORY_BULK_TYPE_BEGIN        (2U)
#define STORY_BULK_TYPE_DATA         (3U)
#define STORY_BULK_TYPE_END          (4U)
#define STORY_BULK_TYPE_ABORT        (5U)
#define STORY_BULK_TYPE_READY        (6U)
#define STORY_BULK_TYPE_ACK          (0x80U)
#define STORY_BULK_FLAG_PLAY_AFTER   (1U << 0)
#define STORY_BULK_RX_RING_BYTES     (8192U)
#define STORY_BULK_READY_PERIOD_MS   (2000U)

typedef enum e_story_bulk_rx_state
{
    STORY_BULK_RX_HEADER = 0,
    STORY_BULK_RX_PAYLOAD
} story_bulk_rx_state_t;

typedef struct st_story_bulk_header
{
    uint8_t type;
    uint8_t flags;
    uint8_t result;
    uint32_t transfer_id;
    uint32_t sequence;
    uint32_t offset;
    uint32_t total_bytes;
    uint16_t payload_length;
    uint32_t crc32;
} story_bulk_header_t;

BSP_DONT_REMOVE volatile story_bulk_uart_debug_t g_story_bulk_uart_debug;

static sci_b_uart_instance_ctrl_t g_story_bulk_uart_ctrl;
static sci_b_baud_setting_t g_story_bulk_baud_setting;
static const sci_b_uart_extended_cfg_t g_story_bulk_uart_extend =
{
    .clock = SCI_B_UART_CLOCK_INT,
    .rx_edge_start = SCI_B_UART_START_BIT_FALLING_EDGE,
    .noise_cancel = SCI_B_UART_NOISE_CANCELLATION_DISABLE,
    .rx_fifo_trigger = SCI_B_UART_RX_FIFO_TRIGGER_MAX,
    .p_baud_setting = &g_story_bulk_baud_setting,
    .flow_control = SCI_B_UART_FLOW_CONTROL_RTS,
    .flow_control_pin = (bsp_io_port_pin_t) UINT16_MAX,
    .rs485_setting = {
        .enable = SCI_B_UART_RS485_DISABLE,
        .polarity = SCI_B_UART_RS485_DE_POLARITY_HIGH,
        .assertion_time = 1U,
        .negation_time = 1U,
    },
    .delay_cycles = 0U,
};
static const uart_cfg_t g_story_bulk_uart_cfg =
{
    .channel = 4U,
    .data_bits = UART_DATA_BITS_8,
    .parity = UART_PARITY_OFF,
    .stop_bits = UART_STOP_BITS_1,
    .p_callback = story_bulk_uart_callback,
    .p_context = NULL,
    .p_extend = &g_story_bulk_uart_extend,
    .p_transfer_tx = NULL,
    .p_transfer_rx = NULL,
    .rxi_ipl = 12U,
    .txi_ipl = 12U,
    .tei_ipl = 12U,
    .eri_ipl = 12U,
    .rxi_irq = VECTOR_NUMBER_SCI4_RXI,
    .txi_irq = VECTOR_NUMBER_SCI4_TXI,
    .tei_irq = VECTOR_NUMBER_SCI4_TEI,
    .eri_irq = VECTOR_NUMBER_SCI4_ERI,
};
static const uart_instance_t g_story_bulk_uart =
{
    .p_ctrl = &g_story_bulk_uart_ctrl,
    .p_cfg = &g_story_bulk_uart_cfg,
    .p_api = &g_uart_on_sci_b,
};

static uint8_t g_story_bulk_header[STORY_BULK_HEADER_BYTES] BSP_ALIGN_VARIABLE(4);
static uint8_t g_story_bulk_payload[STORY_BULK_PAYLOAD_MAX] BSP_ALIGN_VARIABLE(4);
static uint8_t g_story_bulk_ack[STORY_BULK_HEADER_BYTES] BSP_ALIGN_VARIABLE(4);
static uint8_t g_story_bulk_rx_ring[STORY_BULK_RX_RING_BYTES];
static volatile uint32_t g_story_bulk_rx_head;
static volatile uint32_t g_story_bulk_rx_tail;
static uint32_t g_story_bulk_header_fill;
static uint32_t g_story_bulk_payload_fill;
static uint32_t g_story_bulk_last_ready_tick;
static uint32_t g_story_bulk_ready_sequence;
static volatile bool g_story_bulk_rx_complete;
static volatile bool g_story_bulk_tx_complete = true;
static volatile bool g_story_bulk_uart_error;
static story_bulk_rx_state_t g_story_bulk_rx_state;
static story_bulk_header_t g_story_bulk_pending_header;
static bool g_story_bulk_open;
static bool g_story_bulk_receiving;

static uint16_t story_bulk_be16(uint8_t const * p)
{
    return (uint16_t) (((uint16_t) p[0] << 8U) | p[1]);
}

static uint32_t story_bulk_be32(uint8_t const * p)
{
    return ((uint32_t) p[0] << 24U) | ((uint32_t) p[1] << 16U) |
           ((uint32_t) p[2] << 8U) | p[3];
}

static void story_bulk_put_be16(uint8_t * p, uint16_t value)
{
    p[0] = (uint8_t) (value >> 8U);
    p[1] = (uint8_t) value;
}

static void story_bulk_put_be32(uint8_t * p, uint32_t value)
{
    p[0] = (uint8_t) (value >> 24U);
    p[1] = (uint8_t) (value >> 16U);
    p[2] = (uint8_t) (value >> 8U);
    p[3] = (uint8_t) value;
}

static bool story_bulk_parse_header(story_bulk_header_t * p_header)
{
    if ((STORY_BULK_MAGIC != story_bulk_be32(&g_story_bulk_header[0])) ||
        (STORY_BULK_VERSION != g_story_bulk_header[4]))
    {
        return false;
    }
    p_header->type = g_story_bulk_header[5];
    p_header->flags = g_story_bulk_header[6];
    p_header->result = g_story_bulk_header[7];
    p_header->transfer_id = story_bulk_be32(&g_story_bulk_header[8]);
    p_header->sequence = story_bulk_be32(&g_story_bulk_header[12]);
    p_header->offset = story_bulk_be32(&g_story_bulk_header[16]);
    p_header->total_bytes = story_bulk_be32(&g_story_bulk_header[20]);
    p_header->payload_length = story_bulk_be16(&g_story_bulk_header[24]);
    p_header->crc32 = story_bulk_be32(&g_story_bulk_header[28]);
    return (0U == story_bulk_be16(&g_story_bulk_header[26])) &&
           (p_header->payload_length <= STORY_BULK_PAYLOAD_MAX);
}

static uint32_t story_bulk_crc32(uint8_t const * p_data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0U; i < length; i++)
    {
        crc ^= p_data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            crc = (0U != (crc & 1U)) ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static void story_bulk_arm_header(void)
{
    g_story_bulk_rx_state = STORY_BULK_RX_HEADER;
    g_story_bulk_rx_complete = false;
    g_story_bulk_header_fill = 0U;
    g_story_bulk_payload_fill = 0U;
}

static void story_bulk_pump_rx(void)
{
    static const uint8_t magic[4] = {'S', 'B', 'L', '2'};
    while (!g_story_bulk_rx_complete && (g_story_bulk_rx_tail != g_story_bulk_rx_head))
    {
        uint8_t value = g_story_bulk_rx_ring[g_story_bulk_rx_tail];
        g_story_bulk_rx_tail = (g_story_bulk_rx_tail + 1U) % STORY_BULK_RX_RING_BYTES;
        if (STORY_BULK_RX_HEADER == g_story_bulk_rx_state)
        {
            if ((g_story_bulk_header_fill < 4U) && (value != magic[g_story_bulk_header_fill]))
            {
                g_story_bulk_uart_debug.resync_count++;
                g_story_bulk_header_fill = (value == magic[0]) ? 1U : 0U;
                if (1U == g_story_bulk_header_fill)
                {
                    g_story_bulk_header[0] = value;
                }
                continue;
            }
            g_story_bulk_header[g_story_bulk_header_fill++] = value;
            if (STORY_BULK_HEADER_BYTES == g_story_bulk_header_fill)
            {
                g_story_bulk_uart_debug.sync_count++;
                g_story_bulk_rx_complete = true;
            }
        }
        else
        {
            g_story_bulk_payload[g_story_bulk_payload_fill++] = value;
            if (g_story_bulk_payload_fill >= g_story_bulk_pending_header.payload_length)
            {
                g_story_bulk_rx_complete = true;
            }
        }
    }
}

static void story_bulk_send_ready(uint32_t now)
{
    if (!g_story_bulk_tx_complete ||
        ((now - g_story_bulk_last_ready_tick) < STORY_BULK_READY_PERIOD_MS))
    {
        return;
    }
    memset(g_story_bulk_ack, 0, sizeof(g_story_bulk_ack));
    story_bulk_put_be32(&g_story_bulk_ack[0], STORY_BULK_MAGIC);
    g_story_bulk_ack[4] = STORY_BULK_VERSION;
    g_story_bulk_ack[5] = STORY_BULK_TYPE_READY;
    story_bulk_put_be32(&g_story_bulk_ack[12], ++g_story_bulk_ready_sequence);
    g_story_bulk_tx_complete = false;
    if (FSP_SUCCESS == g_story_bulk_uart.p_api->write(g_story_bulk_uart.p_ctrl,
                                                       g_story_bulk_ack,
                                                       sizeof(g_story_bulk_ack)))
    {
        g_story_bulk_uart_debug.ready_frames++;
        g_story_bulk_last_ready_tick = now;
    }
    else
    {
        g_story_bulk_tx_complete = true;
        g_story_bulk_uart_debug.write_error++;
    }
}

static void story_bulk_send_ack(story_bulk_header_t const * p_request,
                                uint32_t result, uint32_t next_offset,
                                uint32_t running_crc)
{
    if (!g_story_bulk_tx_complete)
    {
        return;
    }
    memset(g_story_bulk_ack, 0, sizeof(g_story_bulk_ack));
    story_bulk_put_be32(&g_story_bulk_ack[0], STORY_BULK_MAGIC);
    g_story_bulk_ack[4] = STORY_BULK_VERSION;
    g_story_bulk_ack[5] = STORY_BULK_TYPE_ACK;
    g_story_bulk_ack[6] = p_request->type;
    g_story_bulk_ack[7] = (uint8_t) result;
    story_bulk_put_be32(&g_story_bulk_ack[8], p_request->transfer_id);
    story_bulk_put_be32(&g_story_bulk_ack[12], p_request->sequence);
    story_bulk_put_be32(&g_story_bulk_ack[16], next_offset);
    story_bulk_put_be32(&g_story_bulk_ack[20], p_request->total_bytes);
    story_bulk_put_be16(&g_story_bulk_ack[24], 0U);
    story_bulk_put_be32(&g_story_bulk_ack[28], running_crc);
    g_story_bulk_tx_complete = false;
    fsp_err_t err = g_story_bulk_uart.p_api->write(g_story_bulk_uart.p_ctrl,
                                                    g_story_bulk_ack,
                                                    sizeof(g_story_bulk_ack));
    if (FSP_SUCCESS != err)
    {
        g_story_bulk_tx_complete = true;
        g_story_bulk_uart_debug.write_error = (uint32_t) err;
    }
    else
    {
        g_story_bulk_uart_debug.ack_frames++;
    }
}

void story_bulk_uart_init(void)
{
    fsp_err_t err;

    memset((void *) &g_story_bulk_uart_debug, 0, sizeof(g_story_bulk_uart_debug));
    g_story_bulk_uart_debug.magic = STORY_BULK_DEBUG_MAGIC;
    err = R_SCI_B_UART_BaudCalculate(STORY_BULK_BAUD_RATE, true, 5000U,
                                     &g_story_bulk_baud_setting);
    if (FSP_SUCCESS == err)
    {
        err = g_story_bulk_uart.p_api->open(g_story_bulk_uart.p_ctrl,
                                             g_story_bulk_uart.p_cfg);
    }
    if (FSP_SUCCESS != err)
    {
        g_story_bulk_uart_debug.open_error = (uint32_t) err;
        return;
    }
    g_story_bulk_open = true;
    g_story_bulk_uart_debug.initialized = 1U;
    g_story_bulk_uart_debug.actual_baud = STORY_BULK_BAUD_RATE;
    story_bulk_arm_header();
}

void story_bulk_uart_poll(bool sd_ready)
{
    story_bulk_header_t header;
    uint32_t result = AUDIO_IPC_STORY_RESULT_BAD_REQUEST;
    uint32_t next_offset = g_story_bulk_uart_debug.next_offset;
    uint32_t running_crc = 0U;

    if (!g_story_bulk_open)
    {
        return;
    }
    story_bulk_send_ready((uint32_t) xTaskGetTickCount());
    if (g_story_bulk_uart_error)
    {
        g_story_bulk_uart_error = false;
        g_story_bulk_rx_tail = g_story_bulk_rx_head;
        g_story_bulk_uart_debug.resync_count++;
        story_bulk_arm_header();
        return;
    }
    story_bulk_pump_rx();
    if (!g_story_bulk_rx_complete)
    {
        return;
    }
    g_story_bulk_rx_complete = false;

    if (STORY_BULK_RX_HEADER == g_story_bulk_rx_state)
    {
        if (!story_bulk_parse_header(&header))
        {
            g_story_bulk_uart_debug.protocol_errors++;
            story_bulk_arm_header();
            return;
        }
        g_story_bulk_pending_header = header;
        if (0U != header.payload_length)
        {
            g_story_bulk_rx_state = STORY_BULK_RX_PAYLOAD;
            g_story_bulk_payload_fill = 0U;
            return;
        }
    }
    else
    {
        header = g_story_bulk_pending_header;
    }

    g_story_bulk_uart_debug.rx_frames++;
    g_story_bulk_uart_debug.last_transfer_id = header.transfer_id;
    g_story_bulk_uart_debug.last_sequence = header.sequence;

    if ((0U != header.payload_length) &&
        (story_bulk_crc32(g_story_bulk_payload, header.payload_length) != header.crc32) &&
        (STORY_BULK_TYPE_BEGIN != header.type))
    {
        result = AUDIO_IPC_STORY_RESULT_CRC_MISMATCH;
        g_story_bulk_uart_debug.crc_errors++;
    }
    else if (STORY_BULK_TYPE_PING == header.type)
    {
        result = AUDIO_IPC_STORY_RESULT_OK;
        g_story_bulk_uart_debug.handshake_state = 1U;
    }
    else if ((STORY_BULK_TYPE_BEGIN == header.type) &&
             (13U == header.payload_length) && ('\0' == g_story_bulk_payload[12]))
    {
        result = audio_wav_story_bulk_begin(sd_ready, header.transfer_id,
                                             header.total_bytes, header.crc32,
                                             (char const *) g_story_bulk_payload,
                                             0U != (header.flags & STORY_BULK_FLAG_PLAY_AFTER),
                                             &next_offset);
        g_story_bulk_receiving = (AUDIO_IPC_STORY_RESULT_OK == result);
    }
    else if (STORY_BULK_TYPE_DATA == header.type)
    {
        result = audio_wav_story_bulk_chunk(header.transfer_id, header.total_bytes,
                                             header.offset, g_story_bulk_payload,
                                             header.payload_length, header.crc32,
                                             &next_offset);
    }
    else if ((STORY_BULK_TYPE_END == header.type) && (0U == header.payload_length))
    {
        result = audio_wav_story_bulk_end(header.transfer_id, header.total_bytes,
                                           header.crc32, &next_offset);
        if (AUDIO_IPC_STORY_RESULT_OK == result)
        {
            g_story_bulk_receiving = false;
        }
    }
    else if (STORY_BULK_TYPE_ABORT == header.type)
    {
        audio_wav_story_bulk_abort(header.transfer_id);
        g_story_bulk_receiving = false;
        result = AUDIO_IPC_STORY_RESULT_OK;
        next_offset = 0U;
    }

    g_story_bulk_uart_debug.next_offset = next_offset;
    g_story_bulk_uart_debug.last_result = result;
    if (AUDIO_IPC_MAGIC == AUDIO_IPC_MAILBOX->magic)
    {
        running_crc = AUDIO_IPC_MAILBOX->story_running_crc32;
    }
    story_bulk_arm_header();
    story_bulk_send_ack(&header, result, next_offset, running_crc);
}

bool story_bulk_uart_active(void)
{
    return g_story_bulk_receiving;
}

void story_bulk_uart_callback(uart_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }
    if (UART_EVENT_RX_CHAR == p_args->event)
    {
        uint32_t next = (g_story_bulk_rx_head + 1U) % STORY_BULK_RX_RING_BYTES;
        if (next != g_story_bulk_rx_tail)
        {
            g_story_bulk_rx_ring[g_story_bulk_rx_head] = (uint8_t) p_args->data;
            g_story_bulk_rx_head = next;
            g_story_bulk_uart_debug.rx_bytes++;
            if (0U == g_story_bulk_uart_debug.first_rx_tick)
            {
                g_story_bulk_uart_debug.first_rx_tick = (uint32_t) xTaskGetTickCountFromISR();
            }
        }
        else
        {
            g_story_bulk_uart_debug.ring_overflow_count++;
        }
    }
    else if (UART_EVENT_TX_COMPLETE == p_args->event)
    {
        g_story_bulk_tx_complete = true;
        g_story_bulk_uart_debug.tx_complete_count++;
    }
    else if ((UART_EVENT_ERR_OVERFLOW == p_args->event) ||
             (UART_EVENT_ERR_FRAMING == p_args->event) ||
             (UART_EVENT_ERR_PARITY == p_args->event))
    {
        g_story_bulk_uart_error = true;
        g_story_bulk_uart_debug.protocol_errors++;
    }
}

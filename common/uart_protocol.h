#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include "common_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_FRAME_SOF_HIGH        (0x55U)
#define UART_FRAME_SOF_LOW         (0xAAU)
#define UART_FRAME_HEADER_SIZE     (6U)
#define UART_FRAME_CRC_SIZE        (2U)
#define UART_FRAME_OVERHEAD_SIZE   (UART_FRAME_HEADER_SIZE + UART_FRAME_CRC_SIZE)

typedef enum e_uart_msg_type
{
    UART_MSG_TYPE_HEARTBEAT = 0x01,
    UART_MSG_TYPE_STATUS = 0x02,
    UART_MSG_TYPE_COMMAND = 0x03,
    UART_MSG_TYPE_SENSOR = 0x04,
    UART_MSG_TYPE_AI_RESULT = 0x05,
    UART_MSG_TYPE_FILE = 0x06,
    UART_MSG_TYPE_ACK = 0x7E,
    UART_MSG_TYPE_NACK = 0x7F
} uart_msg_type_t;

typedef enum e_uart_parse_state
{
    UART_PARSE_WAIT_SOF_HIGH = 0,
    UART_PARSE_WAIT_SOF_LOW,
    UART_PARSE_WAIT_TYPE,
    UART_PARSE_WAIT_SEQ,
    UART_PARSE_WAIT_LEN_L,
    UART_PARSE_WAIT_LEN_H,
    UART_PARSE_WAIT_PAYLOAD,
    UART_PARSE_WAIT_CRC_L,
    UART_PARSE_WAIT_CRC_H
} uart_parse_state_t;

typedef struct st_uart_frame
{
    uint8_t type;
    uint8_t sequence;
    uint16_t length;
    uint8_t payload[UART_TX_FRAME_MAX_SIZE];
    uint16_t crc16;
} uart_frame_t;

typedef struct st_uart_parser
{
    uart_parse_state_t state;
    uart_frame_t frame;
    uint16_t payload_index;
    uint16_t crc16_rx;
} uart_parser_t;

#ifdef __cplusplus
}
#endif

#endif /* UART_PROTOCOL_H */

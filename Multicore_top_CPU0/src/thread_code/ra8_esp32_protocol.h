#ifndef RA8_ESP32_PROTOCOL_H
#define RA8_ESP32_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RA8_ESP32_PROTOCOL_VERSION        (1U)
#define RA8_ESP32_PROTOCOL_MAGIC_MSB      (0xAAU)
#define RA8_ESP32_PROTOCOL_MAGIC_LSB      (0x55U)
#define RA8_ESP32_PROTOCOL_HEADER_LENGTH  (8U)
#define RA8_ESP32_PROTOCOL_CRC_LENGTH     (2U)
#define RA8_ESP32_PROTOCOL_MAX_PAYLOAD    (4096U)
#define RA8_ESP32_THERMAL_WIDTH           (80U)
#define RA8_ESP32_THERMAL_HEIGHT          (64U)
#define RA8_ESP32_THERMAL_CHUNK_SAMPLES   (250U)
#define RA8_ESP32_PRESSURE_WIDTH          (16U)
#define RA8_ESP32_PRESSURE_HEIGHT         (16U)

typedef enum e_ra8_esp32_msg_type
{
    RA8_ESP32_MSG_HEARTBEAT = 0x01,
    RA8_ESP32_MSG_STATUS_UPLOAD = 0x10,
    RA8_ESP32_MSG_ALARM_UPLOAD = 0x11,
    RA8_ESP32_MSG_PRESSURE_UPLOAD = 0x12,
    RA8_ESP32_MSG_SAFETY_EVENT = 0x13,
    RA8_ESP32_MSG_RESOURCE_UPLOAD = 0x14,
    RA8_ESP32_MSG_CMD = 0x20,
    RA8_ESP32_MSG_CMD_RESULT = 0x21,
    RA8_ESP32_MSG_SAFETY_ACK = 0x22,
    RA8_ESP32_MSG_FILE_BEGIN = 0x30,
    RA8_ESP32_MSG_FILE_CHUNK = 0x31,
    RA8_ESP32_MSG_FILE_END = 0x32
} ra8_esp32_msg_type_t;

typedef enum e_ra8_esp32_protocol_status
{
    RA8_ESP32_PROTOCOL_STATUS_OK = 0,
    RA8_ESP32_PROTOCOL_STATUS_ERROR = -1,
    RA8_ESP32_PROTOCOL_STATUS_PAYLOAD_TOO_LONG = -2,
    RA8_ESP32_PROTOCOL_STATUS_UART_ERROR = -3
} ra8_esp32_protocol_status_t;

ra8_esp32_protocol_status_t ra8_esp32_protocol_send_status(TickType_t timeout_ticks);
ra8_esp32_protocol_status_t ra8_esp32_protocol_send_thermal_matrix(TickType_t timeout_ticks);
ra8_esp32_protocol_status_t ra8_esp32_protocol_send_pressure_matrix(TickType_t timeout_ticks);
ra8_esp32_protocol_status_t ra8_esp32_protocol_send_resources(TickType_t timeout_ticks);
void ra8_esp32_protocol_init(void);
void ra8_esp32_protocol_poll_rx(void);
void ra8_esp32_protocol_reset_rx(void);
void ra8_esp32_protocol_safety_poll(TickType_t timeout_ticks);
bool ra8_esp32_protocol_gateway_ready(void);
bool ra8_esp32_protocol_story_active(void);
bool ra8_esp32_protocol_story_result_pending(void);
uint16_t ra8_esp32_protocol_crc16_ccitt(const uint8_t * p_data, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* RA8_ESP32_PROTOCOL_H */

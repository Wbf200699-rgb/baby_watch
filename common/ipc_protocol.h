#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include "common_config.h"
#include "system_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum e_ipc_msg_id
{
    IPC_MSG_NONE = 0,
    IPC_MSG_SYSTEM_STATE,
    IPC_MSG_SENSOR_DATA,
    IPC_MSG_AUDIO_FEATURE,
    IPC_MSG_AI_RESULT,
    IPC_MSG_SD_STATUS,
    IPC_MSG_FILE_REQUEST,
    IPC_MSG_FILE_RESPONSE,
    IPC_MSG_CONTROL_CMD,
    IPC_MSG_HEARTBEAT
} ipc_msg_id_t;

typedef struct st_ipc_header
{
    uint8_t src_core;
    uint8_t dst_core;
    uint16_t msg_id;
    uint16_t payload_length;
    uint16_t reserved;
} ipc_header_t;

typedef struct st_ipc_message
{
    ipc_header_t header;
    uint8_t payload[IPC_PAYLOAD_MAX_SIZE];
} ipc_message_t;

typedef struct st_ipc_system_state_payload
{
    system_state_t system_state;
    module_state_t module_state;
    uint32_t timestamp_ms;
} ipc_system_state_payload_t;

typedef struct st_ipc_sensor_payload
{
    int32_t temperature_milli_c;
    int32_t pressure_pa;
    uint32_t sample_index;
} ipc_sensor_payload_t;

#ifdef __cplusplus
}
#endif

#endif /* IPC_PROTOCOL_H */

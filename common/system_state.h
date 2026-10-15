#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#include "common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum e_system_state
{
    SYSTEM_STATE_BOOT = 0,
    SYSTEM_STATE_INIT,
    SYSTEM_STATE_UART_TEST,
    SYSTEM_STATE_UART_READY,
    SYSTEM_STATE_SD_READY,
    SYSTEM_STATE_IPC_READY,
    SYSTEM_STATE_AI_READY,
    SYSTEM_STATE_RUN,
    SYSTEM_STATE_WARN,
    SYSTEM_STATE_ERROR
} system_state_t;

typedef enum e_module_state
{
    MODULE_STATE_IDLE = 0,
    MODULE_STATE_INIT,
    MODULE_STATE_READY,
    MODULE_STATE_BUSY,
    MODULE_STATE_FAULT
} module_state_t;

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_STATE_H */

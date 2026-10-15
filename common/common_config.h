#ifndef COMMON_CONFIG_H
#define COMMON_CONFIG_H

#include "common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_BAUD_RATE             (115200U)
#define UART_TX_PERIOD_MS          (1000U)

#define UART_RING_BUFFER_SIZE      (256U)
#define UART_RX_FRAME_MAX_SIZE     (256U)
#define UART_TX_FRAME_MAX_SIZE     (256U)

#define IPC_QUEUE_LENGTH           (16U)
#define IPC_PAYLOAD_MAX_SIZE       (128U)

#define SD_PATH_MAX_LEN            (128U)
#define FILE_NAME_MAX_LEN          (64U)

#define LOG_ENABLE                 (1U)

#ifdef __cplusplus
}
#endif

#endif /* COMMON_CONFIG_H */

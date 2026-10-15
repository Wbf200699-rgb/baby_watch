#ifndef SYSTEM_LOGGER_H
#define SYSTEM_LOGGER_H

#include <stdbool.h>
#include <stdint.h>

#include "thread_code/sd_card_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYSTEM_LOGGER_PATH_LENGTH (64U)

typedef enum e_system_logger_status
{
    SYSTEM_LOGGER_STATUS_IDLE = 0,
    SYSTEM_LOGGER_STATUS_READY,
    SYSTEM_LOGGER_STATUS_DIR_ERROR,
    SYSTEM_LOGGER_STATUS_FILE_ERROR,
    SYSTEM_LOGGER_STATUS_WRITE_ERROR
} system_logger_status_t;

typedef struct st_system_logger_debug
{
    uint32_t magic;
    uint32_t build_marker;
    volatile uint32_t run_index;
    volatile uint32_t line_count;
    volatile uint32_t last_tick;
    volatile uint32_t open_fail_count;
    volatile uint32_t write_fail_count;
    volatile uint32_t close_fail_count;
    volatile int mkdir_result;
    volatile int last_write_result;
    volatile int last_close_result;
    volatile int last_errno;
    volatile int dir_exists;
    volatile system_logger_status_t status;
    char log_path[SYSTEM_LOGGER_PATH_LENGTH];
} system_logger_debug_t;

extern volatile system_logger_debug_t g_system_logger_debug;
extern volatile const uint32_t g_system_logger_build_marker;

void system_logger_init(sd_card_info_t const * p_sd_info);
void system_logger_poll(sd_card_info_t const * p_sd_info);
void system_logger_reset(void);
bool system_logger_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_LOGGER_H */

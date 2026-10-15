#ifndef THERMAL_IPC_SHARED_H
#define THERMAL_IPC_SHARED_H

#include <stdint.h>

#define THERMAL_IPC_MAGIC       (0x54484D31U) /* THM1 */
#define THERMAL_IPC_ADDRESS        (0x221D0000UL)
#define THERMAL_IPC_POINTS_ADDRESS (0x221D0200UL)
#define THERMAL_IPC_POINT_COUNT (5120U)

typedef enum e_thermal_ipc_status
{
    THERMAL_IPC_STATUS_EMPTY = 0,
    THERMAL_IPC_STATUS_OPEN_ERROR,
    THERMAL_IPC_STATUS_WAITING_FRAME,
    THERMAL_IPC_STATUS_FRAME_OK,
    THERMAL_IPC_STATUS_FRAME_TIMEOUT,
    THERMAL_IPC_STATUS_UART_ERROR,
    THERMAL_IPC_STATUS_CRC_ERROR,
    THERMAL_IPC_STATUS_DATA_ERROR,
    THERMAL_IPC_STATUS_RECOVERING,
    THERMAL_IPC_STATUS_STALE_FRAME
} thermal_ipc_status_t;

typedef enum e_thermal_recovery_reason
{
    THERMAL_RECOVERY_NONE = 0,
    THERMAL_RECOVERY_FRAME_TIMEOUT,
    THERMAL_RECOVERY_UART_ERROR,
    THERMAL_RECOVERY_CRC_ERROR,
    THERMAL_RECOVERY_DATA_ERROR,
    THERMAL_RECOVERY_REOPEN_ERROR,
    THERMAL_RECOVERY_STALE_FRAME
} thermal_recovery_reason_t;

typedef enum e_thermal_frame_quality
{
    THERMAL_FRAME_QUALITY_NORMAL = 0,
    THERMAL_FRAME_QUALITY_TRANSIENT,
    THERMAL_FRAME_QUALITY_INVALID
} thermal_frame_quality_t;

typedef struct st_thermal_ipc_mailbox
{
    uint32_t magic;
    volatile uint32_t sequence;
    volatile uint32_t status;
    volatile uint32_t rx_bytes;
    volatile int16_t min_cdeg;
    volatile int16_t max_cdeg;
    volatile int16_t center_cdeg;
    volatile int16_t first_cdeg;
    volatile int32_t avg_cdeg;
    volatile uint32_t uart_errors;
    volatile uint32_t uart_error_count;
    volatile uint32_t rx_overflow_count;
    volatile uint32_t crc_received;
    volatile uint32_t crc_calculated;
    volatile uint32_t valid_points;
    volatile uint32_t head_word;
    volatile uint32_t raw0;
    volatile uint32_t raw1;
    volatile uint32_t raw_center;
    volatile uint32_t sensor_raw;
    volatile uint32_t parity_error_count;
    volatile uint32_t framing_error_count;
    volatile uint32_t fifo_overflow_count;
    volatile uint32_t crc_error_count;
    volatile uint32_t data_error_count;
    volatile uint32_t baud_set_count;
    volatile uint32_t frame_zero_words;
    volatile uint32_t frame_one_words;
    volatile uint32_t frame_ffff_words;
    volatile uint32_t driver_build;
    volatile uint32_t consecutive_failures;
    volatile uint32_t recovery_count;
    volatile uint32_t reopen_count;
    volatile uint32_t hard_reset_count;
    volatile uint32_t recovery_error_count;
    volatile uint32_t last_recovery_reason;
    volatile uint32_t last_good_tick;
    volatile uint32_t run_pin_ready;
    /*
     * hard_reset_count/run_pin_ready are retained for compatibility with the
     * previous RUN-pin implementation.  These explicit fields describe the
     * relay power-cycle implementation used by current hardware.
     */
    volatile uint32_t power_cycle_count;
    volatile uint32_t power_relay_ready;
    volatile uint32_t stale_frame_count;
    volatile uint32_t identical_frame_streak;
    /*
     * Seqlock protecting THERMAL_IPC_POINTS and accepted-frame statistics.
     * CPU1 makes this odd before writing and even after the complete matrix is
     * visible. CPU0 may only consume an unchanged even generation.
     */
    volatile uint32_t write_generation;
    volatile int16_t p01_cdeg;
    volatile int16_t p99_cdeg;
    volatile int32_t mean_cdeg;
    volatile uint32_t frame_quality;
    volatile uint32_t transient_drop_count;
    volatile uint32_t generation_retry_count;
    volatile uint32_t last_good_sequence;
    volatile uint32_t mean_abs_diff_cdeg;
    volatile uint32_t changed_pixel_count;
} thermal_ipc_mailbox_t;

#define THERMAL_IPC_MAILBOX ((volatile thermal_ipc_mailbox_t *) THERMAL_IPC_ADDRESS)
#define THERMAL_IPC_POINTS  ((volatile int16_t *) THERMAL_IPC_POINTS_ADDRESS)

#endif /* THERMAL_IPC_SHARED_H */

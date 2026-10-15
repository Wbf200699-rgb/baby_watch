#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool enabled;
    bool transport_suspended;
    uint32_t control_count;
    uint32_t last_control_sequence;
    uint32_t last_control_age_ms;
    uint32_t queue_drops;
    uint32_t send_failures;
    uint32_t send_timeouts;
    uint32_t arbitration_drops;
    uint32_t backpressure_drops;
    uint32_t nodelay_failures;
    uint32_t last_send_ms;
    uint32_t max_send_ms;
} mic_stream_stats_t;

esp_err_t mic_stream_start(void);
esp_err_t mic_stream_prepare(void);
esp_err_t mic_stream_set_transport_suspended(bool suspended);
bool mic_stream_is_connected(void);
void mic_stream_get_stats(mic_stream_stats_t *stats);

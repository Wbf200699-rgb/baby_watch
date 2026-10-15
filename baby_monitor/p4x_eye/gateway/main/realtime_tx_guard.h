#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    uint32_t acquisitions;
    uint32_t contentions;
    uint32_t timeouts;
    uint32_t max_hold_ms;
} realtime_tx_guard_stats_t;

esp_err_t realtime_tx_guard_init(void);
bool realtime_tx_guard_take(TickType_t timeout);
void realtime_tx_guard_give(void);
void realtime_tx_guard_get_stats(realtime_tx_guard_stats_t *stats);

#include "realtime_tx_guard.h"

#include "esp_timer.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_guard;
static int64_t s_acquired_at_us;
static volatile uint32_t s_acquisitions;
static volatile uint32_t s_contentions;
static volatile uint32_t s_timeouts;
static volatile uint32_t s_max_hold_ms;

esp_err_t realtime_tx_guard_init(void)
{
    if (s_guard) {
        return ESP_OK;
    }
    s_guard = xSemaphoreCreateMutex();
    return s_guard ? ESP_OK : ESP_ERR_NO_MEM;
}

bool realtime_tx_guard_take(TickType_t timeout)
{
    if (!s_guard || xSemaphoreTake(s_guard, timeout) != pdTRUE) {
        ++s_contentions;
        if (timeout != 0) {
            ++s_timeouts;
        }
        return false;
    }
    ++s_acquisitions;
    s_acquired_at_us = esp_timer_get_time();
    return true;
}

void realtime_tx_guard_give(void)
{
    if (!s_guard) {
        return;
    }
    uint32_t held_ms =
        (uint32_t)((esp_timer_get_time() - s_acquired_at_us) / 1000);
    if (held_ms > s_max_hold_ms) {
        s_max_hold_ms = held_ms;
    }
    xSemaphoreGive(s_guard);
}

void realtime_tx_guard_get_stats(realtime_tx_guard_stats_t *stats)
{
    if (!stats) {
        return;
    }
    stats->acquisitions = s_acquisitions;
    stats->contentions = s_contentions;
    stats->timeouts = s_timeouts;
    stats->max_hold_ms = s_max_hold_ms;
}

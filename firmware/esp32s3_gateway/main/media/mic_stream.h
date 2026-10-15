#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t mic_stream_start(void);
bool mic_stream_is_connected(void);

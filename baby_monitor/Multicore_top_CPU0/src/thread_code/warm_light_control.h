#ifndef WARM_LIGHT_CONTROL_H
#define WARM_LIGHT_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#define WARM_LIGHT_CONTROL_PIN_LABEL     (711U)
#define WARM_LIGHT_CONTROL_ACTIVE_HIGH   (1U)

typedef struct st_warm_light_control_state
{
    uint32_t ready;
    uint32_t on;
    uint32_t command_sequence;
    uint32_t error_count;
    uint32_t last_fsp_error;
} warm_light_control_state_t;

void warm_light_control_init(void);
bool warm_light_control_set(bool on);
void warm_light_control_get_state(warm_light_control_state_t * p_state);

#endif

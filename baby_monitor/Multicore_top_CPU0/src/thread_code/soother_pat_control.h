#ifndef SOOTHER_PAT_CONTROL_H
#define SOOTHER_PAT_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#define SOOTHER_CONTROL_PIN_LABEL       (914U)
#define SOOTHER_CONTROL_ACTIVE_HIGH     (1U)

typedef struct st_soother_pat_control_state
{
    uint32_t ready;
    uint32_t on;
    uint32_t command_sequence;
    uint32_t error_count;
    uint32_t last_fsp_error;
} soother_pat_control_state_t;

void soother_pat_control_init(void);
bool soother_pat_control_set(bool on);
void soother_pat_control_get_state(soother_pat_control_state_t * p_state);

#endif

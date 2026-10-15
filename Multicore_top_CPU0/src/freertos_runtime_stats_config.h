#ifndef FREERTOS_RUNTIME_STATS_CONFIG_H
#define FREERTOS_RUNTIME_STATS_CONFIG_H

void resource_runtime_counter_configure(void);
uint32_t resource_runtime_counter_get(void);

#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() resource_runtime_counter_configure()
#define portGET_RUN_TIME_COUNTER_VALUE()         resource_runtime_counter_get()

#endif /* FREERTOS_RUNTIME_STATS_CONFIG_H */

#include "thermal_thread.h"
#include "thread_code/sd_ipc_shared.h"
#include "thread_code/thermal_uart_driver.h"

static uint32_t g_resource_previous_counter;
static uint32_t g_resource_previous_idle;
static uint32_t g_resource_previous_tick;
static bool g_resource_sample_started;

void resource_runtime_counter_configure(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t resource_runtime_counter_get(void)
{
    return DWT->CYCCNT;
}

static void cpu1_resource_poll(void)
{
    uint32_t const tick = (uint32_t) xTaskGetTickCount();
    if (g_resource_sample_started &&
        ((tick - g_resource_previous_tick) < pdMS_TO_TICKS(900U)))
    {
        return;
    }

    uint32_t const counter = resource_runtime_counter_get();
    uint32_t const idle = (uint32_t) ulTaskGetIdleRunTimeCounter();
    if (!g_resource_sample_started)
    {
        g_resource_previous_counter = counter;
        g_resource_previous_idle = idle;
        g_resource_previous_tick = tick;
        g_resource_sample_started = true;
        return;
    }

    uint32_t const total_delta = counter - g_resource_previous_counter;
    uint32_t const idle_delta = idle - g_resource_previous_idle;
    uint32_t usage_permille = 0U;
    if (total_delta > 0U)
    {
        uint32_t const idle_permille =
            (uint32_t) ((((uint64_t) idle_delta) * 1000ULL) / total_delta);
        usage_permille = (idle_permille < 1000U) ? (1000U - idle_permille) : 0U;
    }

    volatile sd_ipc_mailbox_t * const p_sd = SD_IPC_MAILBOX;
    uint32_t generation = p_sd->resource_generation;
    if (0U != (generation & 1U))
    {
        generation++;
    }
    p_sd->resource_generation = generation + 1U;
    __DMB();
    p_sd->resource_sequence++;
    p_sd->resource_tick = tick;
    p_sd->cpu_usage_permille = usage_permille;
    p_sd->heap_free_bytes = (uint32_t) xPortGetFreeHeapSize();
    p_sd->heap_min_bytes = (uint32_t) xPortGetMinimumEverFreeHeapSize();
    __DMB();
    p_sd->resource_generation = generation + 2U;

    g_resource_previous_counter = counter;
    g_resource_previous_idle = idle;
    g_resource_previous_tick = tick;
}

/* thermal entry function */
/* pvParameters contains TaskHandle_t */
void thermal_thread_entry(void *pvParameters) {
	FSP_PARAMETER_NOT_USED(pvParameters);

	/* The module powers up with the board and needs time to initialize its sensor core. */
	vTaskDelay(pdMS_TO_TICKS(1500));
	(void) thermal_uart_driver_init();

	while (1) {
		(void) thermal_uart_request_frame(pdMS_TO_TICKS(1200));
		cpu1_resource_poll();
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

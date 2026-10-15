#include "esp32_uart_thread.h"

#include "thread_code/esp32_uart_driver.h"
#include "thread_code/pressure_ipc_shared.h"
#include "thread_code/ra8_esp32_protocol.h"
#include "thread_code/soother_pat_control.h"
#include "thread_code/thermal_ipc_shared.h"
#include "thread_code/warm_light_control.h"

#define ESP32_UART_UPLOAD_PRESSURE_MATRIX (1U)
#define ESP32_UART_GATEWAY_WAIT_MS        (10000U)
#define ESP32_UART_STATUS_INTERVAL_MS     (250U)
#define ESP32_UART_RESOURCE_INTERVAL_MS   (1000U)
#define ESP32_UART_THERMAL_INTERVAL_MS    (500U)
#define ESP32_UART_PRESSURE_INTERVAL_MS   (125U)
#define ESP32_UART_FALLBACK_STATUS_MS      (1000U)
#define ESP32_UART_FALLBACK_THERMAL_MS     (1000U)
#define ESP32_UART_FALLBACK_PRESSURE_MS    (500U)
#define ESP32_UART_SAFETY_INTERVAL_MS     (50U)
#define ESP32_UART_STORY_STATUS_INTERVAL_MS (1000U)
#define ESP32_UART_STORY_STATUS_TIMEOUT_MS  (100U)
#define ESP32_UART_ACTIVE_POLL_MS         (5U)
#define ESP32_UART_WAIT_POLL_MS           (20U)
#define ESP32_UART_RX_POLL_MS              (2U)
#define ESP32_UART_RX_TASK_STACK_WORDS     (1536U)
#define ESP32_UART_RX_TASK_PRIORITY        (configMAX_PRIORITIES - 3U)
#define RA8_SOFT_RESET_INPUT_PIN          BSP_IO_PORT_07_PIN_12
#define RA8_SOFT_RESET_ARM_MS             (5000U)
#define RA8_SOFT_RESET_POLL_MS            (10U)
#define RA8_SOFT_RESET_DEBOUNCE_SAMPLES   (5U)
#define RA8_SOFT_RESET_TASK_STACK_WORDS   (256U)
#define RA8_SOFT_RESET_TASK_PRIORITY      (configMAX_PRIORITIES - 2U)
#define WARM_LIGHT_RELAY_PIN               BSP_IO_PORT_07_PIN_11 /* P711 / P7-12 */
#define SOOTHER_RELAY_PIN                  BSP_IO_PORT_09_PIN_14 /* P914 / P7-2 */

#if WARM_LIGHT_CONTROL_ACTIVE_HIGH
#define WARM_LIGHT_RELAY_ACTIVE_LEVEL      BSP_IO_LEVEL_HIGH
#define WARM_LIGHT_RELAY_INACTIVE_LEVEL    BSP_IO_LEVEL_LOW
#define WARM_LIGHT_RELAY_INACTIVE_CFG      ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                            (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW | \
                                            (uint32_t) IOPORT_CFG_DRIVE_MID)
#else
#define WARM_LIGHT_RELAY_ACTIVE_LEVEL      BSP_IO_LEVEL_LOW
#define WARM_LIGHT_RELAY_INACTIVE_LEVEL    BSP_IO_LEVEL_HIGH
#define WARM_LIGHT_RELAY_INACTIVE_CFG      ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                            (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH | \
                                            (uint32_t) IOPORT_CFG_DRIVE_MID)
#endif

#if SOOTHER_CONTROL_ACTIVE_HIGH
#define SOOTHER_RELAY_ACTIVE_LEVEL     BSP_IO_LEVEL_HIGH
#define SOOTHER_RELAY_INACTIVE_LEVEL   BSP_IO_LEVEL_LOW
#define SOOTHER_RELAY_INACTIVE_CFG     ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                        (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW | \
                                        (uint32_t) IOPORT_CFG_DRIVE_MID)
#else
#define SOOTHER_RELAY_ACTIVE_LEVEL     BSP_IO_LEVEL_LOW
#define SOOTHER_RELAY_INACTIVE_LEVEL   BSP_IO_LEVEL_HIGH
#define SOOTHER_RELAY_INACTIVE_CFG     ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                        (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH | \
                                        (uint32_t) IOPORT_CFG_DRIVE_MID)
#endif

volatile uint32_t g_ra8_soft_reset_pin_ready;
volatile uint32_t g_ra8_soft_reset_pin_level = 1U;
volatile uint32_t g_ra8_soft_reset_read_errors;
volatile uint32_t g_ra8_soft_reset_low_samples;
volatile uint32_t g_ra8_soft_reset_trigger_count;
volatile uint32_t g_ra8_soft_reset_task_created;
volatile uint32_t g_esp32_uart_rx_task_created;
volatile uint32_t g_esp32_uart_rx_task_poll_count;
volatile uint32_t g_esp32_uart_rx_error_observed;

static StaticTask_t g_ra8_soft_reset_task_control;
static StackType_t g_ra8_soft_reset_task_stack[RA8_SOFT_RESET_TASK_STACK_WORDS];
static volatile warm_light_control_state_t g_warm_light_state;
static volatile soother_pat_control_state_t g_soother_pat_state;
static StaticTask_t g_esp32_uart_rx_task_control;
static StackType_t g_esp32_uart_rx_task_stack[ESP32_UART_RX_TASK_STACK_WORDS];
static TaskHandle_t g_esp32_uart_rx_task;

static void esp32_uart_rx_task(void * pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    while (1)
    {
        esp32_uart_stats_t stats;

        ra8_esp32_protocol_poll_rx();
        esp32_uart_driver_get_stats(&stats);
        g_esp32_uart_rx_error_observed = stats.error_count;
        g_esp32_uart_rx_task_poll_count++;
        vTaskDelay(pdMS_TO_TICKS(ESP32_UART_RX_POLL_MS));
    }
}

static void esp32_uart_rx_task_start(void)
{
    g_esp32_uart_rx_task = xTaskCreateStatic(esp32_uart_rx_task,
                                              "uart_rx",
                                              ESP32_UART_RX_TASK_STACK_WORDS,
                                              NULL,
                                              ESP32_UART_RX_TASK_PRIORITY,
                                              g_esp32_uart_rx_task_stack,
                                              &g_esp32_uart_rx_task_control);
    g_esp32_uart_rx_task_created = (NULL != g_esp32_uart_rx_task) ? 1U : 0U;
}

void soother_pat_control_init(void)
{
    fsp_err_t err = R_IOPORT_PinCfg(&g_ioport_ctrl,
                                    SOOTHER_RELAY_PIN,
                                    SOOTHER_RELAY_INACTIVE_CFG);

    g_soother_pat_state.ready = (FSP_SUCCESS == err) ? 1U : 0U;
    g_soother_pat_state.on = 0U;
    g_soother_pat_state.last_fsp_error = (uint32_t) err;
    if (FSP_SUCCESS != err)
    {
        g_soother_pat_state.error_count++;
    }
}

bool soother_pat_control_set(bool on)
{
    g_soother_pat_state.command_sequence++;
    if (0U == g_soother_pat_state.ready)
    {
        soother_pat_control_init();
    }

    if (0U == g_soother_pat_state.ready)
    {
        return false;
    }

    bsp_io_level_t const level = on ? SOOTHER_RELAY_ACTIVE_LEVEL :
                                      SOOTHER_RELAY_INACTIVE_LEVEL;
    fsp_err_t err = R_IOPORT_PinWrite(&g_ioport_ctrl, SOOTHER_RELAY_PIN, level);
    g_soother_pat_state.last_fsp_error = (uint32_t) err;
    if (FSP_SUCCESS != err)
    {
        g_soother_pat_state.ready = 0U;
        g_soother_pat_state.error_count++;
        return false;
    }

    g_soother_pat_state.on = on ? 1U : 0U;
    return true;
}

void soother_pat_control_get_state(soother_pat_control_state_t * p_state)
{
    if (NULL == p_state)
    {
        return;
    }

    p_state->ready = g_soother_pat_state.ready;
    p_state->on = g_soother_pat_state.on;
    p_state->command_sequence = g_soother_pat_state.command_sequence;
    p_state->error_count = g_soother_pat_state.error_count;
    p_state->last_fsp_error = g_soother_pat_state.last_fsp_error;
}

void warm_light_control_init(void)
{
    fsp_err_t err = R_IOPORT_PinCfg(&g_ioport_ctrl,
                                    WARM_LIGHT_RELAY_PIN,
                                    WARM_LIGHT_RELAY_INACTIVE_CFG);

    g_warm_light_state.ready = (FSP_SUCCESS == err) ? 1U : 0U;
    g_warm_light_state.on = 0U;
    g_warm_light_state.last_fsp_error = (uint32_t) err;
    if (FSP_SUCCESS != err)
    {
        g_warm_light_state.error_count++;
    }
}

bool warm_light_control_set(bool on)
{
    g_warm_light_state.command_sequence++;
    if (0U == g_warm_light_state.ready)
    {
        warm_light_control_init();
    }

    if (0U == g_warm_light_state.ready)
    {
        return false;
    }

    bsp_io_level_t const level = on ? WARM_LIGHT_RELAY_ACTIVE_LEVEL :
                                      WARM_LIGHT_RELAY_INACTIVE_LEVEL;
    fsp_err_t err = R_IOPORT_PinWrite(&g_ioport_ctrl, WARM_LIGHT_RELAY_PIN, level);
    g_warm_light_state.last_fsp_error = (uint32_t) err;
    if (FSP_SUCCESS != err)
    {
        g_warm_light_state.ready = 0U;
        g_warm_light_state.error_count++;
        return false;
    }

    g_warm_light_state.on = on ? 1U : 0U;
    return true;
}

void warm_light_control_get_state(warm_light_control_state_t * p_state)
{
    if (NULL == p_state)
    {
        return;
    }

    p_state->ready = g_warm_light_state.ready;
    p_state->on = g_warm_light_state.on;
    p_state->command_sequence = g_warm_light_state.command_sequence;
    p_state->error_count = g_warm_light_state.error_count;
    p_state->last_fsp_error = g_warm_light_state.last_fsp_error;
}

static void ra8_soft_reset_monitor_task(void * pvParameters)
{
    TickType_t const started = xTaskGetTickCount();
    uint32_t consecutive_low = 0U;
    bool observed_released = false;

    FSP_PARAMETER_NOT_USED(pvParameters);

    fsp_err_t err = R_IOPORT_PinCfg(&g_ioport_ctrl,
                                    RA8_SOFT_RESET_INPUT_PIN,
                                    (uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT |
                                    (uint32_t) IOPORT_CFG_PULLUP_ENABLE);
    g_ra8_soft_reset_pin_ready = (FSP_SUCCESS == err) ? 1U : 0U;

    while (1)
    {
        bsp_io_level_t level = BSP_IO_LEVEL_HIGH;
        err = R_IOPORT_PinRead(&g_ioport_ctrl, RA8_SOFT_RESET_INPUT_PIN, &level);
        if (FSP_SUCCESS != err)
        {
            g_ra8_soft_reset_read_errors++;
            consecutive_low = 0U;
        }
        else
        {
            g_ra8_soft_reset_pin_level = (uint32_t) level;
            if (BSP_IO_LEVEL_HIGH == level)
            {
                observed_released = true;
                consecutive_low = 0U;
            }
            else if (observed_released &&
                     ((xTaskGetTickCount() - started) >= pdMS_TO_TICKS(RA8_SOFT_RESET_ARM_MS)))
            {
                consecutive_low++;
                g_ra8_soft_reset_low_samples = consecutive_low;
                if (consecutive_low >= RA8_SOFT_RESET_DEBOUNCE_SAMPLES)
                {
                    g_ra8_soft_reset_trigger_count++;
                    __DSB();
                    NVIC_SystemReset();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(RA8_SOFT_RESET_POLL_MS));
    }
}

static void ra8_soft_reset_monitor_start(void)
{
    TaskHandle_t task = xTaskCreateStatic(ra8_soft_reset_monitor_task,
                                          "p4_rst",
                                          RA8_SOFT_RESET_TASK_STACK_WORDS,
                                          NULL,
                                          RA8_SOFT_RESET_TASK_PRIORITY,
                                          g_ra8_soft_reset_task_stack,
                                          &g_ra8_soft_reset_task_control);
    g_ra8_soft_reset_task_created = (NULL != task) ? 1U : 0U;
}

/* uart entry function */
/* pvParameters contains TaskHandle_t */
void esp32_uart_thread_entry(void *pvParameters)
{
    fsp_err_t init_err;
    TickType_t last_matrix_tick = 0U;
    uint32_t last_matrix_sequence = 0U;
    TickType_t last_pressure_tick = 0U;
    uint32_t last_pressure_sequence = 0U;
    TickType_t last_status_tick = 0U;
    TickType_t last_resource_tick = 0U;
    TickType_t last_safety_tick = 0U;
    TickType_t gateway_wait_started = xTaskGetTickCount();
    bool gateway_ever_ready = false;

    FSP_PARAMETER_NOT_USED (pvParameters);

    warm_light_control_init();
    soother_pat_control_init();
    ra8_soft_reset_monitor_start();

    init_err = esp32_uart_driver_init();
    if (FSP_SUCCESS != init_err)
    {
        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ra8_esp32_protocol_init();
    esp32_uart_rx_task_start();

    while (1)
    {
        volatile thermal_ipc_mailbox_t const * p_thm = THERMAL_IPC_MAILBOX;
        volatile pressure_ipc_mailbox_t const * p_prs = PRESSURE_IPC_MAILBOX;
        TickType_t now = xTaskGetTickCount();

        if (NULL == g_esp32_uart_rx_task)
        {
            ra8_esp32_protocol_poll_rx();
        }
        bool gateway_ready = ra8_esp32_protocol_gateway_ready();
        if (gateway_ready)
        {
            gateway_ever_ready = true;
        }

        /*
         * The gateway sends gateway_ready every two seconds.  Resetting SCI0
         * on the same cadence can repeatedly discard that frame at exactly
         * the same phase.  Only actual UART errors trigger recovery above.
         * After the boot grace period, keep a bounded low-rate uplink alive so
         * a reverse-link fault cannot freeze the dashboard matrices forever.
         */
        bool fallback_upload = !gateway_ready &&
                               (gateway_ever_ready ||
                                ((now - gateway_wait_started) >=
                                 pdMS_TO_TICKS(ESP32_UART_GATEWAY_WAIT_MS)));
        if (!gateway_ready && !fallback_upload)
        {
            vTaskDelay(pdMS_TO_TICKS(ESP32_UART_WAIT_POLL_MS));
            continue;
        }

        bool story_active = gateway_ready && ra8_esp32_protocol_story_active();
        if (story_active)
        {
            /*
             * Keep the server-side RA8 lease alive while bulk story chunks
             * own most of the reverse link.  Matrices and safety events stay
             * paused so the short story-result ACK keeps priority on SCI0.
             */
            if (!ra8_esp32_protocol_story_result_pending() &&
                ((0U == last_status_tick) ||
                 ((now - last_status_tick) >=
                  pdMS_TO_TICKS(ESP32_UART_STORY_STATUS_INTERVAL_MS))))
            {
                last_status_tick = now;
                (void) ra8_esp32_protocol_send_status(
                    pdMS_TO_TICKS(ESP32_UART_STORY_STATUS_TIMEOUT_MS));
            }
            vTaskDelay(pdMS_TO_TICKS(ESP32_UART_ACTIVE_POLL_MS));
            continue;
        }

        if (gateway_ready &&
            ((0U == last_safety_tick) ||
             ((now - last_safety_tick) >= pdMS_TO_TICKS(ESP32_UART_SAFETY_INTERVAL_MS))))
        {
            last_safety_tick = now;
            ra8_esp32_protocol_safety_poll(pdMS_TO_TICKS(100));
        }

        TickType_t status_interval = pdMS_TO_TICKS(gateway_ready ?
                                                   ESP32_UART_STATUS_INTERVAL_MS :
                                                   ESP32_UART_FALLBACK_STATUS_MS);
        if ((0U == last_status_tick) ||
            ((now - last_status_tick) >= status_interval))
        {
            last_status_tick = now;
            (void) ra8_esp32_protocol_send_status(pdMS_TO_TICKS(250));
        }

        if ((0U == last_resource_tick) ||
            ((now - last_resource_tick) >=
             pdMS_TO_TICKS(ESP32_UART_RESOURCE_INTERVAL_MS)))
        {
            last_resource_tick = now;
            (void) ra8_esp32_protocol_send_resources(pdMS_TO_TICKS(100));
        }

#if ESP32_UART_UPLOAD_PRESSURE_MATRIX
        TickType_t pressure_interval = pdMS_TO_TICKS(gateway_ready ?
                                                     ESP32_UART_PRESSURE_INTERVAL_MS :
                                                     ESP32_UART_FALLBACK_PRESSURE_MS);
        if ((PRESSURE_IPC_MAGIC == p_prs->magic) &&
            ((uint32_t) PRESSURE_IPC_STATUS_SCAN_OK == p_prs->status) &&
            (last_pressure_sequence != p_prs->sequence) &&
            ((0U == last_pressure_tick) ||
             ((now - last_pressure_tick) >= pressure_interval)))
        {
            last_pressure_tick = now;
            if (RA8_ESP32_PROTOCOL_STATUS_OK ==
                ra8_esp32_protocol_send_pressure_matrix(pdMS_TO_TICKS(100)))
            {
                last_pressure_sequence = p_prs->sequence;
            }
        }
#else
        FSP_PARAMETER_NOT_USED(p_prs);
        FSP_PARAMETER_NOT_USED(last_pressure_tick);
        FSP_PARAMETER_NOT_USED(last_pressure_sequence);
#endif

        TickType_t thermal_interval = pdMS_TO_TICKS(gateway_ready ?
                                                    ESP32_UART_THERMAL_INTERVAL_MS :
                                                    ESP32_UART_FALLBACK_THERMAL_MS);
        if ((THERMAL_IPC_MAGIC == p_thm->magic) &&
            ((uint32_t) THERMAL_IPC_STATUS_FRAME_OK == p_thm->status) &&
            ((uint32_t) THERMAL_FRAME_QUALITY_NORMAL == p_thm->frame_quality) &&
            (p_thm->sequence == p_thm->last_good_sequence) &&
            (last_matrix_sequence != p_thm->sequence) &&
            ((0U == last_matrix_tick) ||
             ((now - last_matrix_tick) >= thermal_interval)))
        {
            last_matrix_tick = now;
            if (RA8_ESP32_PROTOCOL_STATUS_OK ==
                ra8_esp32_protocol_send_thermal_matrix(pdMS_TO_TICKS(100)))
            {
                last_matrix_sequence = p_thm->sequence;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ESP32_UART_ACTIVE_POLL_MS));
    }
}

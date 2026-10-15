#include "fusion_thread.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "thread_code/audio_cry_inference.h"
#include "thread_code/fusion_calibration_generated.h"
#include "thread_code/mic_ipc_shared.h"
#include "thread_code/multimodal_fusion.h"
#include "thread_code/pressure_ipc_shared.h"
#include "thread_code/scd41_ipc_shared.h"
#include "thread_code/thermal_ipc_shared.h"
#include "thread_code/tmp117_ipc_shared.h"
#include "thread_code/vision_runtime.h"

#define FUSION_PERIOD_MS                    (100U)
#define FUSION_BOOT_GRACE_MS                (10000U)
#define FUSION_THERMAL_STALE_MS             (5000U)
#define FUSION_TEMPERATURE_STALE_MS         (10000U)
#define FUSION_PRESSURE_STALE_MS            (5000U)
#define FUSION_MIC_STALE_MS                 (5000U)
#define FUSION_CAMERA_STALE_MS              (12000U)
#define FUSION_ENVIRONMENT_STALE_MS         (15000U)
#define FUSION_CAMERA_BOOT_GRACE_MS         (20000U)
#define FUSION_CAMERA_FREEZE_MS             (15000U)
#define FUSION_MODEL_STALE_MS               (3000U)
#define FUSION_PRESSURE_WARMUP_FRAMES       (20U)
#define FUSION_PRESSURE_OCCUPIED_TOTAL      (2000U)
#define FUSION_PRESSURE_RELEASE_TOTAL       (1200U)
#define FUSION_M1616_OCCUPIED_TOTAL         (1U)
#define FUSION_M1616_RELEASE_TOTAL          (1U)
#define FUSION_M1616_ACTIVE_POINT           FUSION_CAL_PRESSURE_ACTIVE_POINT
#define FUSION_M1616_CONFIRM_FRAMES         FUSION_CAL_PRESSURE_CONFIRM_FRAMES
#define FUSION_M1616_RELEASE_FRAMES         FUSION_CAL_PRESSURE_RELEASE_FRAMES
#define FUSION_PRESSURE_EDGE_X1000          FUSION_CAL_PRESSURE_EDGE_POSITIVE_X1000
#define FUSION_PRESSURE_ACTIVE_X1000        (300U)
#define FUSION_PRESSURE_ROLLING_X1000       (60U)
#define FUSION_EDGE_WIDTH                   FUSION_CAL_PRESSURE_EDGE_WIDTH
#define FUSION_PRESENCE_POSITIVE_X1000      (550U)
#define FUSION_PRESENCE_CONFIRM_X1000       (450U)
#define FUSION_PRESENCE_NEGATIVE_X1000      (200U)
#define FUSION_PRESSURE_WEIGHT              FUSION_CAL_EDGE_PRESSURE_WEIGHT
#define FUSION_VISION_WEIGHT                FUSION_CAL_EDGE_VISION_WEIGHT
#define FUSION_THERMAL_WEIGHT               FUSION_CAL_EDGE_THERMAL_WEIGHT
#define FUSION_THERMAL_CONTRAST_LOW_DECI_C  FUSION_CAL_THERMAL_CONTRAST_LOW_DECI_C
#define FUSION_THERMAL_CONTRAST_HIGH_DECI_C FUSION_CAL_THERMAL_CONTRAST_HIGH_DECI_C
#define FUSION_THERMAL_HOTSPOT_DELTA_DECI_C FUSION_CAL_THERMAL_HOTSPOT_DELTA_DECI_C
#define FUSION_THERMAL_HOTSPOT_P99_GAP_DECI_C FUSION_CAL_THERMAL_HOTSPOT_P99_GAP_DECI_C
#define FUSION_THERMAL_HOTSPOT_MIN_POINTS   FUSION_CAL_THERMAL_HOTSPOT_MIN_POINTS
#define FUSION_THERMAL_HOTSPOT_FULL_POINTS  FUSION_CAL_THERMAL_HOTSPOT_FULL_POINTS
#define FUSION_BODY_FEVER_HYSTERESIS_DECI_C  (5)
#define FUSION_BODY_FEVER_CONFIRM_MS         (30000U)
#define FUSION_BODY_FEVER_RECOVERY_MS        (30000U)
#define FUSION_OVERHEAT_MILLI_C             (38000)
#define FUSION_EXTREME_HEAT_MILLI_C         (42000)
#define FUSION_UNDERHEAT_MILLI_C            (10000)
#define FUSION_EXTREME_COLD_MILLI_C         (5000)
#define FUSION_DEFAULT_CO2_MAX_PPM           (2000U)
#define FUSION_DEFAULT_ENV_TEMP_MAX_MILLI_C  (30000)
#define FUSION_DEFAULT_ENV_RH_MAX_MILLI_PCT  (70000U)
#define FUSION_DEFAULT_ENV_RH_MIN_MILLI_PCT  (30000U)
#define FUSION_DEFAULT_GAS_MIN_OHM           (10000U)
#define FUSION_DEFAULT_BODY_TEMP_MAX_DECI_C  (375)
#define FUSION_THRESHOLD_READ_ATTEMPTS       (3U)
#define FUSION_CRY_THRESHOLD                (75U)
#define FUSION_PERSISTENT_CRY_MS            (30000U)
#define FUSION_ABSENT_CONFIRM_MS            (5000U)
#define FUSION_AWAKE_CONFIRM_MS             (3000U)
#define FUSION_SLEEP_CONFIRM_MS             (45000U)
#define FUSION_NO_CRY_SLEEP_MS              (10000U)
#define FUSION_VISION_SCORE_X1000           (450U)
#define FUSION_VISION_CLASS_X1000           (600U)
#define FUSION_VISION_CLEAR_X1000           (600U)
#define FUSION_VISION_NOT_VISIBLE_X1000     (600U)
#define FUSION_VISION_COVERED_X1000         (600U)
#define FUSION_VISION_PARTIAL_X1000         (500U)
#define FUSION_VISION_UNCERTAIN_X1000       (500U)
#define FUSION_VISION_INFANT_COVER_X1000    (300U)
#define FUSION_VISION_NONCLEAR_HINT_X1000   (150U)
#define FUSION_VISION_HEAD_PROXY_START_X1000 (0U)
#define FUSION_VISION_HEAD_PROXY_WIDTH_X1000 (400U)
#define FUSION_VISION_HEAD_PROXY_TOP_X1000   (100U)
#define FUSION_VISION_HEAD_PROXY_HEIGHT_X1000 (800U)
#define FUSION_VISION_HEAD_PROXY_COVER_X1000 (300U)
#define FUSION_VISION_EDGE_MARGIN_X1000     (180U)
#define FUSION_VISION_MOTION_X1000          (80U)
#define FUSION_AIRWAY_PARTIAL_FRAMES        (3U)
#define FUSION_AIRWAY_COVERED_FRAMES        (3U)
#define FUSION_AIRWAY_NOT_VISIBLE_FRAMES    (4U)
#define FUSION_VISUAL_HIDDEN_FRAMES          (10U)
#define FUSION_ENV_CONFIRM_MS                (10000U)
#define FUSION_ENV_RECOVERY_MS               (30000U)
#define FUSION_ENV_CO2_HYSTERESIS_PPM        (100U)
#define FUSION_ENV_TEMP_HYSTERESIS_MILLI_C   (500)
#define FUSION_ENV_RH_HYSTERESIS_MILLI_PCT   (2000U)
#define FUSION_EVENT_QUEUE_LENGTH           (16U)
#define FUSION_SNAPSHOT_READ_ATTEMPTS        (2U)

typedef struct st_fusion_model_results
{
    volatile uint32_t audio_tick;
    volatile uint32_t cry_confidence;
    volatile uint32_t vision_tick;
    volatile uint32_t sleep_confidence;
    volatile uint32_t face_covered_confidence;
    volatile uint32_t standing_confidence;
    volatile uint32_t movement_confidence;
} fusion_model_results_t;

typedef struct st_fusion_incident_runtime
{
    multimodal_incident_lifecycle_t lifecycle;
    uint32_t lifecycle_since_tick;
    uint32_t opened_tick;
    uint32_t incident_id;
    uint32_t level;
    uint32_t severity;
    uint32_t confidence;
    uint32_t source_mask;
} fusion_incident_runtime_t;

typedef struct st_fusion_runtime
{
    uint32_t boot_tick;
    uint32_t state_since_tick;
    uint32_t pressure_sequence;
    uint32_t pressure_processed_sequence;
    uint32_t pressure_frames;
    uint32_t thermal_sequence;
    uint32_t thermal_update_tick;
    uint32_t thermal_good_tick;
    uint32_t temperature_sequence;
    uint32_t temperature_update_tick;
    uint32_t temperature_good_tick;
    uint32_t pressure_update_tick;
    uint32_t pressure_good_tick;
    uint32_t mic_sequence;
    uint32_t mic_update_tick;
    uint32_t mic_good_tick;
    uint32_t environment_sequence;
    uint32_t environment_update_tick;
    uint32_t environment_good_tick;
    uint32_t environment_sample_tick;
    uint32_t environment_bme_sample_tick;
    uint32_t environment_bme_good_tick;
    uint32_t environment_bme_gas_ohm;
    bool environment_bme_gas_valid;
    uint32_t environment_co2_ppm;
    int32_t environment_temperature_milli_c;
    uint32_t environment_humidity_milli_percent;
    uint32_t pressure_total;
    uint32_t pressure_edge;
    uint32_t pressure_peak;
    uint32_t pressure_active_points;
    uint32_t pressure_edge_ratio_x1000;
    uint32_t pressure_activity_x1000;
    uint32_t pressure_x100;
    uint32_t pressure_y100;
    uint32_t previous_pressure_total;
    uint32_t previous_pressure_x100;
    uint32_t previous_pressure_y100;
    uint32_t previous_edge_ratio_x1000;
    int32_t pressure_move_x100;
    int32_t pressure_move_y100;
    uint32_t hot_sensor_count;
    uint32_t cold_sensor_count;
    uint32_t thermal_feature_sequence;
    uint32_t thermal_presence_x1000;
    uint32_t thermal_hotspot_points;
    uint32_t thermal_hotspot_x1000;
    uint32_t thermal_hotspot_y1000;
    int32_t thermal_hotspot_mean_cdeg;
    uint32_t thermal_bed_x1000;
    uint32_t thermal_edge_x1000;
    int32_t bed_left_milli_c;
    int32_t bed_center_milli_c;
    int32_t bed_right_milli_c;
    int32_t bed_spread_milli_c;
    uint32_t bed_hotspot_region;
    bool occupied;
    bool pressure_is_m1616;
    bool occupied_seen;
    uint32_t pressure_confirm_frames;
    uint32_t pressure_release_frames;
    int32_t pressure_baseline[PRESSURE_IPC_MATRIX_COUNT];

    uint32_t vision_sequence;
    uint32_t vision_frame_sequence;
    uint32_t vision_update_tick;
    uint32_t vision_width;
    uint32_t vision_height;
    uint32_t infant_score_x1000;
    uint32_t head_score_x1000;
    uint32_t caregiver_score_x1000;
    uint32_t infant_edge_distance_x1000;
    uint32_t head_object_overlap_x1000;
    uint32_t infant_object_overlap_x1000;
    uint32_t clear_frames;
    uint32_t partial_frames;
    uint32_t covered_frames;
    uint32_t not_visible_frames;
    uint32_t visual_hidden_frames;
    bool occlusion_valid;
    bool head_proxy_valid;
    uint32_t behavior_class;
    uint32_t behavior_confidence_x1000;
    uint32_t airway_view;
    uint32_t airway_confidence_x1000;
    uint32_t infant_center_x1000;
    uint32_t infant_center_y1000;
    uint32_t visual_motion_x1000;
    bool infant_center_valid;
    bool infant_at_edge;

    uint32_t absence_since_tick;
    uint32_t awake_since_tick;
    uint32_t quiet_since_tick;
    uint32_t cry_since_tick;
    uint32_t last_cry_tick;
    uint32_t next_incident_id;
    uint32_t next_event_id;
    uint32_t incident_event_drop_count;
    multimodal_state_t state;
    fusion_incident_runtime_t incidents[MULTIMODAL_INCIDENT_TYPE_COUNT];
} fusion_runtime_t;

static fusion_runtime_t g_fusion_runtime;
static fusion_model_results_t g_fusion_models;
static volatile uint32_t g_fusion_publish_lock;
static volatile bool g_fusion_reset_pressure;
static multimodal_fusion_snapshot_t g_fusion_snapshot;
static multimodal_incident_event_t g_fusion_event_queue[FUSION_EVENT_QUEUE_LENGTH];
static volatile uint32_t g_fusion_event_head;
static volatile uint32_t g_fusion_event_tail;
static volatile uint32_t g_fusion_threshold_lock;
static multimodal_environment_thresholds_t g_fusion_environment_thresholds =
{
    .co2_max_ppm = FUSION_DEFAULT_CO2_MAX_PPM,
    .temperature_max_milli_c = FUSION_DEFAULT_ENV_TEMP_MAX_MILLI_C,
    .humidity_max_milli_percent = FUSION_DEFAULT_ENV_RH_MAX_MILLI_PCT,
    .humidity_min_milli_percent = FUSION_DEFAULT_ENV_RH_MIN_MILLI_PCT,
    .gas_min_ohm = FUSION_DEFAULT_GAS_MIN_OHM,
    .body_temperature_max_deci_c = FUSION_DEFAULT_BODY_TEMP_MAX_DECI_C,
    .revision = 1U,
};

static uint32_t fusion_clamp_u32(uint32_t value, uint32_t maximum)
{
    return (value > maximum) ? maximum : value;
}

static uint32_t fusion_abs_i32(int32_t value)
{
    return (uint32_t) ((value < 0) ? -value : value);
}

static uint32_t fusion_abs_delta(int16_t value, int32_t baseline)
{
    return fusion_abs_i32((int32_t) value - baseline);
}

static uint32_t fusion_clamp_confidence(uint32_t confidence)
{
    return fusion_clamp_u32(confidence, 100U);
}

static uint32_t fusion_age_ms(uint32_t now, uint32_t tick)
{
    if (0U == tick)
    {
        return UINT32_MAX;
    }

    return (uint32_t) ((((uint64_t) (now - tick)) * 1000ULL) / configTICK_RATE_HZ);
}

static bool fusion_tick_fresh(uint32_t now, uint32_t tick, uint32_t maximum_age_ms)
{
    return fusion_age_ms(now, tick) <= maximum_age_ms;
}

static uint32_t fusion_count_bits(uint32_t value)
{
    uint32_t count = 0U;
    while (value != 0U)
    {
        count += value & 1U;
        value >>= 1;
    }
    return count;
}

void multimodal_fusion_set_audio_result(uint32_t cry_confidence, uint32_t timestamp_tick)
{
    g_fusion_models.cry_confidence = fusion_clamp_confidence(cry_confidence);
    __DMB();
    g_fusion_models.audio_tick = timestamp_tick;
}

void multimodal_fusion_set_vision_result(uint32_t sleep_confidence,
                                         uint32_t face_covered_confidence,
                                         uint32_t standing_confidence,
                                         uint32_t movement_confidence,
                                         uint32_t timestamp_tick)
{
    g_fusion_models.sleep_confidence = fusion_clamp_confidence(sleep_confidence);
    g_fusion_models.face_covered_confidence = fusion_clamp_confidence(face_covered_confidence);
    g_fusion_models.standing_confidence = fusion_clamp_confidence(standing_confidence);
    g_fusion_models.movement_confidence = fusion_clamp_confidence(movement_confidence);
    __DMB();
    g_fusion_models.vision_tick = timestamp_tick;
}

void multimodal_fusion_reset_pressure_baseline(void)
{
    g_fusion_reset_pressure = true;
}

static bool fusion_environment_thresholds_valid(
    multimodal_environment_thresholds_t const * p_thresholds)
{
    return (NULL != p_thresholds) &&
           (p_thresholds->co2_max_ppm >= 400U) &&
           (p_thresholds->co2_max_ppm <= 10000U) &&
           (p_thresholds->temperature_max_milli_c >= 15000) &&
           (p_thresholds->temperature_max_milli_c <= 50000) &&
           (p_thresholds->humidity_max_milli_percent >= 20000U) &&
           (p_thresholds->humidity_max_milli_percent <= 100000U) &&
           (p_thresholds->humidity_min_milli_percent <= 95000U) &&
           (p_thresholds->humidity_min_milli_percent < p_thresholds->humidity_max_milli_percent) &&
           (p_thresholds->gas_min_ohm <= 10000000U) &&
           (p_thresholds->body_temperature_max_deci_c >= 300) &&
           (p_thresholds->body_temperature_max_deci_c <= 450);
}

bool multimodal_fusion_set_environment_thresholds(
    multimodal_environment_thresholds_t const * p_thresholds)
{
    multimodal_environment_thresholds_t next;

    if (!fusion_environment_thresholds_valid(p_thresholds))
    {
        return false;
    }

    next = *p_thresholds;
    next.revision = g_fusion_environment_thresholds.revision + 1U;
    if (0U == next.revision)
    {
        next.revision = 1U;
    }

    taskENTER_CRITICAL();
    g_fusion_threshold_lock++;
    __DMB();
    g_fusion_environment_thresholds = next;
    __DMB();
    g_fusion_threshold_lock++;
    taskEXIT_CRITICAL();
    return true;
}

bool multimodal_fusion_get_environment_thresholds(
    multimodal_environment_thresholds_t * p_thresholds)
{
    if (NULL == p_thresholds)
    {
        return false;
    }

    for (uint32_t attempt = 0U; attempt < FUSION_THRESHOLD_READ_ATTEMPTS; attempt++)
    {
        uint32_t before = g_fusion_threshold_lock;
        uint32_t after;

        if (0U != (before & 1U))
        {
            continue;
        }
        __DMB();
        *p_thresholds = g_fusion_environment_thresholds;
        __DMB();
        after = g_fusion_threshold_lock;
        if ((before == after) && (0U == (after & 1U)))
        {
            return true;
        }
    }

    return false;
}

bool multimodal_fusion_get_snapshot(multimodal_fusion_snapshot_t * p_snapshot)
{
    if (NULL == p_snapshot)
    {
        return false;
    }

    for (uint32_t attempt = 0U; attempt < FUSION_SNAPSHOT_READ_ATTEMPTS; attempt++)
    {
        uint32_t before = g_fusion_publish_lock;
        uint32_t after;

        /*
         * Never spin while the writer owns the sequence lock. This reader is
         * called by the higher-priority ESP32 UART task; spinning here would
         * starve the lower-priority fusion task that must make the lock even.
         */
        if (0U != (before & 1U))
        {
            return false;
        }
        __DMB();
        memcpy(p_snapshot, &g_fusion_snapshot, sizeof(*p_snapshot));
        __DMB();
        after = g_fusion_publish_lock;
        if ((before == after) && (0U == (after & 1U)))
        {
            return (0U != p_snapshot->sequence);
        }
    }

    return false;
}

bool multimodal_fusion_peek_event(multimodal_incident_event_t * p_event)
{
    uint32_t tail;

    if (NULL == p_event)
    {
        return false;
    }

    tail = g_fusion_event_tail;
    if (tail == g_fusion_event_head)
    {
        return false;
    }
    __DMB();
    memcpy(p_event, &g_fusion_event_queue[tail % FUSION_EVENT_QUEUE_LENGTH], sizeof(*p_event));
    __DMB();
    return tail == g_fusion_event_tail;
}

bool multimodal_fusion_ack_event(uint32_t event_id)
{
    uint32_t tail = g_fusion_event_tail;
    multimodal_incident_event_t const * p_event;

    if (tail == g_fusion_event_head)
    {
        return false;
    }
    p_event = &g_fusion_event_queue[tail % FUSION_EVENT_QUEUE_LENGTH];
    if (p_event->event_id != event_id)
    {
        return false;
    }
    __DMB();
    g_fusion_event_tail = tail + 1U;
    return true;
}

static void fusion_queue_event(multimodal_incident_type_t type,
                               multimodal_incident_action_t action,
                               uint32_t now,
                               fusion_incident_runtime_t const * p_incident)
{
    uint32_t head = g_fusion_event_head;
    multimodal_incident_event_t * p_event;

    if ((head - g_fusion_event_tail) >= FUSION_EVENT_QUEUE_LENGTH)
    {
        g_fusion_runtime.incident_event_drop_count++;
        return;
    }

    p_event = &g_fusion_event_queue[head % FUSION_EVENT_QUEUE_LENGTH];
    memset(p_event, 0, sizeof(*p_event));
    p_event->event_id = ++g_fusion_runtime.next_event_id;
    p_event->incident_id = p_incident->incident_id;
    p_event->type = (uint32_t) type;
    p_event->action = (uint32_t) action;
    p_event->lifecycle = (uint32_t) p_incident->lifecycle;
    p_event->level = p_incident->level;
    p_event->severity = p_incident->severity;
    p_event->confidence = p_incident->confidence;
    p_event->source_mask = p_incident->source_mask;
    p_event->occurred_tick = now;
    __DMB();
    g_fusion_event_head = head + 1U;
}

static void fusion_track_sensor_sequences(uint32_t now)
{
    volatile thermal_ipc_mailbox_t const * p_thermal = THERMAL_IPC_MAILBOX;
    volatile tmp117_ipc_mailbox_t const * p_temperature = TMP117_IPC_MAILBOX;
    volatile pressure_ipc_mailbox_t const * p_pressure = PRESSURE_IPC_MAILBOX;
    volatile mic_ipc_mailbox_t const * p_mic = MIC_IPC_MAILBOX;

    if (p_thermal->sequence != g_fusion_runtime.thermal_sequence)
    {
        g_fusion_runtime.thermal_sequence = p_thermal->sequence;
        g_fusion_runtime.thermal_update_tick = now;
        if ((THERMAL_IPC_MAGIC == p_thermal->magic) &&
            ((uint32_t) THERMAL_IPC_STATUS_FRAME_OK == p_thermal->status) &&
            ((uint32_t) THERMAL_FRAME_QUALITY_NORMAL == p_thermal->frame_quality) &&
            (p_thermal->sequence == p_thermal->last_good_sequence) &&
            (THERMAL_IPC_POINT_COUNT == p_thermal->valid_points))
        {
            g_fusion_runtime.thermal_good_tick = now;
        }
    }
    if (p_temperature->sequence != g_fusion_runtime.temperature_sequence)
    {
        g_fusion_runtime.temperature_sequence = p_temperature->sequence;
        g_fusion_runtime.temperature_update_tick = now;
        if ((TMP117_IPC_MAGIC == p_temperature->magic) &&
            (0x1FU == (p_temperature->connected_mask & 0x1FU)) &&
            (0U == (p_temperature->error_mask & 0x1FU)))
        {
            g_fusion_runtime.temperature_good_tick = now;
        }
    }
    if (p_pressure->sequence != g_fusion_runtime.pressure_sequence)
    {
        g_fusion_runtime.pressure_sequence = p_pressure->sequence;
        g_fusion_runtime.pressure_update_tick = now;
        if ((PRESSURE_IPC_MAGIC == p_pressure->magic) &&
            (PRESSURE_IPC_STATUS_SCAN_ERROR != p_pressure->status))
        {
            g_fusion_runtime.pressure_good_tick = now;
        }
    }
    if (p_mic->sequence != g_fusion_runtime.mic_sequence)
    {
        g_fusion_runtime.mic_sequence = p_mic->sequence;
        g_fusion_runtime.mic_update_tick = now;
        if ((MIC_IPC_MAGIC == p_mic->magic) &&
            ((MIC_IPC_STATUS_RUNNING == p_mic->status) ||
             (MIC_IPC_STATUS_RECORDING == p_mic->status)))
        {
            g_fusion_runtime.mic_good_tick = now;
        }
    }

    /*
     * Keep the previous environment state if CPU1 is inside a publish.  The
     * bounded seqlock reader never exposes a partially updated measurement.
     */
    {
        scd41_ipc_snapshot_t environment;

        if (scd41_ipc_read_snapshot(&environment) &&
            (environment.sequence != g_fusion_runtime.environment_sequence))
        {
            g_fusion_runtime.environment_sequence = environment.sequence;
            g_fusion_runtime.environment_update_tick = now;
            if ((SCD41_IPC_MAGIC == environment.magic) &&
                ((uint32_t) SCD41_IPC_STATUS_SAMPLE_OK == environment.status))
            {
                g_fusion_runtime.environment_co2_ppm = environment.co2_ppm;
                g_fusion_runtime.environment_temperature_milli_c =
                    environment.temperature_milli_c;
                g_fusion_runtime.environment_humidity_milli_percent =
                    environment.humidity_milli_percent;
                if ((0U != environment.last_good_tick) &&
                    (environment.last_good_tick != g_fusion_runtime.environment_sample_tick))
                {
                    g_fusion_runtime.environment_sample_tick = environment.last_good_tick;
                    g_fusion_runtime.environment_good_tick = now;
                }
            }
            g_fusion_runtime.environment_bme_gas_valid =
                ((uint32_t) BME688_IPC_STATUS_SAMPLE_OK == environment.bme_status) &&
                (0U != (environment.bme_flags & BME688_IPC_FLAG_GAS_VALID)) &&
                (0U != (environment.bme_flags & BME688_IPC_FLAG_HEATER_STABLE));
            if (g_fusion_runtime.environment_bme_gas_valid)
            {
                g_fusion_runtime.environment_bme_gas_ohm = environment.bme_gas_resistance_ohm;
                if ((0U != environment.bme_last_good_tick) &&
                    (environment.bme_last_good_tick != g_fusion_runtime.environment_bme_sample_tick))
                {
                    g_fusion_runtime.environment_bme_sample_tick = environment.bme_last_good_tick;
                    g_fusion_runtime.environment_bme_good_tick = now;
                }
            }
        }
    }
}

static uint32_t fusion_scale_x1000(uint32_t value, uint32_t low, uint32_t high)
{
    if (value <= low)
    {
        return 0U;
    }
    if ((value >= high) || (high <= low))
    {
        return 1000U;
    }
    return ((value - low) * 1000U) / (high - low);
}

static uint32_t fusion_map_thermal_to_bed_x(uint32_t thermal_x1000)
{
    int64_t mapped_scaled =
        ((((int64_t) FUSION_CAL_THERMAL_BED_X_SLOPE_X1000) * (int64_t) thermal_x1000) / 1000LL) +
        (int64_t) FUSION_CAL_THERMAL_BED_X_INTERCEPT_X1000;
    int32_t mapped = (int32_t) mapped_scaled;

    if (mapped <= 0)
    {
        return 0U;
    }
    if (mapped >= 1000)
    {
        return 1000U;
    }
    return (uint32_t) mapped;
}

static uint32_t fusion_position_agreement(uint32_t first_x1000, uint32_t second_x1000)
{
    uint32_t delta = fusion_abs_i32((int32_t) first_x1000 - (int32_t) second_x1000);
    uint32_t disagreement = fusion_scale_x1000(delta,
                                                FUSION_CAL_POSITION_AGREEMENT_GOOD_X1000,
                                                FUSION_CAL_POSITION_AGREEMENT_BAD_X1000);
    return 1000U - disagreement;
}

static void fusion_update_environment(multimodal_fusion_snapshot_t * p_snapshot)
{
    multimodal_environment_thresholds_t thresholds =
    {
        .co2_max_ppm = FUSION_DEFAULT_CO2_MAX_PPM,
        .temperature_max_milli_c = FUSION_DEFAULT_ENV_TEMP_MAX_MILLI_C,
        .humidity_max_milli_percent = FUSION_DEFAULT_ENV_RH_MAX_MILLI_PCT,
        .humidity_min_milli_percent = FUSION_DEFAULT_ENV_RH_MIN_MILLI_PCT,
        .gas_min_ohm = FUSION_DEFAULT_GAS_MIN_OHM,
        .body_temperature_max_deci_c = FUSION_DEFAULT_BODY_TEMP_MAX_DECI_C,
        .revision = 1U,
    };

    p_snapshot->environment_co2_ppm = g_fusion_runtime.environment_co2_ppm;
    p_snapshot->environment_temperature_milli_c =
        g_fusion_runtime.environment_temperature_milli_c;
    p_snapshot->environment_humidity_milli_percent =
        g_fusion_runtime.environment_humidity_milli_percent;
    /*
     * Translate the CPU1 sample epoch into CPU0's tick domain.  This keeps the
     * externally reported freshness comparable with the status payload tick
     * even if CPU1 is reset independently.
     */
    p_snapshot->environment_last_good_tick =
        g_fusion_runtime.environment_good_tick;
    (void) multimodal_fusion_get_environment_thresholds(&thresholds);
    p_snapshot->environment_thresholds = thresholds;
}

static void fusion_update_pressure(multimodal_fusion_snapshot_t * p_snapshot)
{
    volatile pressure_ipc_mailbox_t const * p_pressure = PRESSURE_IPC_MAILBOX;
    int16_t matrix[PRESSURE_IPC_MATRIX_COUNT];
    uint32_t sequence = 0U;
    uint32_t total = 0U;
    uint32_t edge = 0U;
    uint32_t weighted_x = 0U;
    uint32_t weighted_y = 0U;
    uint32_t peak = 0U;
    uint32_t active_points = 0U;
    bool const m1616 = (0U == p_pressure->upstream_pca_address) &&
                       (0U == p_pressure->downstream_pca_address);

    g_fusion_runtime.pressure_is_m1616 = m1616;

    p_snapshot->pressure_total = g_fusion_runtime.pressure_total;
    p_snapshot->pressure_edge = g_fusion_runtime.pressure_edge;
    p_snapshot->pressure_peak = g_fusion_runtime.pressure_peak;
    p_snapshot->pressure_active_points = g_fusion_runtime.pressure_active_points;
    p_snapshot->pressure_edge_ratio_x1000 = g_fusion_runtime.pressure_edge_ratio_x1000;
    p_snapshot->pressure_activity_x1000 = g_fusion_runtime.pressure_activity_x1000;
    p_snapshot->pressure_x100 = g_fusion_runtime.pressure_x100;
    p_snapshot->pressure_y100 = g_fusion_runtime.pressure_y100;
    p_snapshot->pressure_move_x100 = g_fusion_runtime.pressure_move_x100;
    p_snapshot->pressure_move_y100 = g_fusion_runtime.pressure_move_y100;
    p_snapshot->occupied = g_fusion_runtime.occupied ? 1U : 0U;

    if ((0U == p_pressure->sequence) ||
        (p_pressure->sequence == g_fusion_runtime.pressure_processed_sequence))
    {
        return;
    }

    for (uint32_t attempt = 0U; attempt < 3U; attempt++)
    {
        uint32_t before = p_pressure->sequence;
        __DMB();
        for (uint32_t i = 0U; i < PRESSURE_IPC_MATRIX_COUNT; i++)
        {
            matrix[i] = p_pressure->matrix[i];
        }
        __DMB();
        if ((0U != before) && (before == p_pressure->sequence))
        {
            sequence = before;
            break;
        }
    }

    if ((0U == sequence) || (sequence == g_fusion_runtime.pressure_processed_sequence))
    {
        return;
    }
    g_fusion_runtime.pressure_processed_sequence = sequence;

    if (!m1616 && (g_fusion_runtime.pressure_frames < FUSION_PRESSURE_WARMUP_FRAMES))
    {
        for (uint32_t i = 0U; i < PRESSURE_IPC_MATRIX_COUNT; i++)
        {
            g_fusion_runtime.pressure_baseline[i] += matrix[i];
        }
        g_fusion_runtime.pressure_frames++;
        if (FUSION_PRESSURE_WARMUP_FRAMES == g_fusion_runtime.pressure_frames)
        {
            for (uint32_t i = 0U; i < PRESSURE_IPC_MATRIX_COUNT; i++)
            {
                g_fusion_runtime.pressure_baseline[i] /= (int32_t) FUSION_PRESSURE_WARMUP_FRAMES;
            }
        }
        return;
    }

    for (uint32_t y = 0U; y < PRESSURE_IPC_MATRIX_HEIGHT; y++)
    {
        for (uint32_t x = 0U; x < PRESSURE_IPC_MATRIX_WIDTH; x++)
        {
            uint32_t i = y * PRESSURE_IPC_MATRIX_WIDTH + x;
            uint32_t value = m1616
                                 ? ((matrix[i] > 0) ? (uint32_t) matrix[i] : 0U)
                                 : fusion_abs_delta(matrix[i], g_fusion_runtime.pressure_baseline[i]);
            total += value;
            if (value > peak)
            {
                peak = value;
            }
            if (value >= (m1616 ? FUSION_M1616_ACTIVE_POINT : 1U))
            {
                active_points++;
            }
            weighted_x += value * x;
            weighted_y += value * y;
            if ((x < FUSION_EDGE_WIDTH) || (x >= (PRESSURE_IPC_MATRIX_WIDTH - FUSION_EDGE_WIDTH)) ||
                (y < FUSION_EDGE_WIDTH) || (y >= (PRESSURE_IPC_MATRIX_HEIGHT - FUSION_EDGE_WIDTH)))
            {
                edge += value;
            }
        }
    }

    if (m1616)
    {
        if (g_fusion_runtime.occupied)
        {
            g_fusion_runtime.pressure_confirm_frames = 0U;
            if (total < FUSION_M1616_RELEASE_TOTAL)
            {
                if (++g_fusion_runtime.pressure_release_frames >= FUSION_M1616_RELEASE_FRAMES)
                {
                    g_fusion_runtime.occupied = false;
                    g_fusion_runtime.pressure_release_frames = 0U;
                }
            }
            else
            {
                g_fusion_runtime.pressure_release_frames = 0U;
            }
        }
        else
        {
            g_fusion_runtime.pressure_release_frames = 0U;
            if ((total >= FUSION_M1616_OCCUPIED_TOTAL) && (active_points > 0U))
            {
                if (++g_fusion_runtime.pressure_confirm_frames >= FUSION_M1616_CONFIRM_FRAMES)
                {
                    g_fusion_runtime.occupied = true;
                    g_fusion_runtime.occupied_seen = true;
                    g_fusion_runtime.pressure_confirm_frames = 0U;
                }
            }
            else
            {
                g_fusion_runtime.pressure_confirm_frames = 0U;
            }
        }
    }
    else if (g_fusion_runtime.occupied)
    {
        if (total < FUSION_PRESSURE_RELEASE_TOTAL)
        {
            g_fusion_runtime.occupied = false;
        }
    }
    else if (total >= FUSION_PRESSURE_OCCUPIED_TOTAL)
    {
        g_fusion_runtime.occupied = true;
        g_fusion_runtime.occupied_seen = true;
    }

    g_fusion_runtime.previous_pressure_total = g_fusion_runtime.pressure_total;
    g_fusion_runtime.previous_pressure_x100 = g_fusion_runtime.pressure_x100;
    g_fusion_runtime.previous_pressure_y100 = g_fusion_runtime.pressure_y100;
    g_fusion_runtime.previous_edge_ratio_x1000 = g_fusion_runtime.pressure_edge_ratio_x1000;
    g_fusion_runtime.pressure_total = total;
    g_fusion_runtime.pressure_edge = edge;
    g_fusion_runtime.pressure_peak = peak;
    g_fusion_runtime.pressure_active_points = active_points;
    g_fusion_runtime.pressure_edge_ratio_x1000 =
        (0U == total) ? 0U : fusion_clamp_u32((uint32_t) ((((uint64_t) edge) * 1000ULL) / total), 1000U);
    g_fusion_runtime.pressure_x100 = (0U == total) ? 0U : (weighted_x * 100U) / total;
    g_fusion_runtime.pressure_y100 = (0U == total) ? 0U : (weighted_y * 100U) / total;
    g_fusion_runtime.pressure_move_x100 =
        (int32_t) g_fusion_runtime.pressure_x100 - (int32_t) g_fusion_runtime.previous_pressure_x100;
    g_fusion_runtime.pressure_move_y100 =
        (int32_t) g_fusion_runtime.pressure_y100 - (int32_t) g_fusion_runtime.previous_pressure_y100;

    {
        uint32_t load_change = fusion_abs_i32((int32_t) total - (int32_t) g_fusion_runtime.previous_pressure_total);
        uint32_t position_change = fusion_abs_i32(g_fusion_runtime.pressure_move_x100) +
                                   fusion_abs_i32(g_fusion_runtime.pressure_move_y100);
        uint32_t activity_floor = m1616 ? FUSION_M1616_OCCUPIED_TOTAL : FUSION_PRESSURE_OCCUPIED_TOTAL;
        uint32_t denominator = (total > activity_floor) ? total : activity_floor;
        uint32_t activity = (uint32_t) ((((uint64_t) load_change) * 700ULL) / denominator) +
                            fusion_clamp_u32(position_change * 3U, 300U);
        g_fusion_runtime.pressure_activity_x1000 = fusion_clamp_u32(activity, 1000U);
    }

    p_snapshot->pressure_total = total;
    p_snapshot->pressure_edge = edge;
    p_snapshot->pressure_peak = peak;
    p_snapshot->pressure_active_points = active_points;
    p_snapshot->pressure_edge_ratio_x1000 = g_fusion_runtime.pressure_edge_ratio_x1000;
    p_snapshot->pressure_activity_x1000 = g_fusion_runtime.pressure_activity_x1000;
    p_snapshot->pressure_x100 = g_fusion_runtime.pressure_x100;
    p_snapshot->pressure_y100 = g_fusion_runtime.pressure_y100;
    p_snapshot->pressure_move_x100 = g_fusion_runtime.pressure_move_x100;
    p_snapshot->pressure_move_y100 = g_fusion_runtime.pressure_move_y100;
    p_snapshot->occupied = g_fusion_runtime.occupied ? 1U : 0U;
}

static void fusion_update_thermal_features(multimodal_fusion_snapshot_t * p_snapshot)
{
    volatile thermal_ipc_mailbox_t * p_thermal = THERMAL_IPC_MAILBOX;
    uint32_t accepted_sequence = 0U;
    uint32_t hotspot_points = 0U;
    uint64_t hotspot_weight = 0ULL;
    uint64_t weighted_x = 0ULL;
    uint64_t weighted_y = 0ULL;
    int64_t hotspot_sum = 0LL;
    int32_t p01_cdeg = 0;
    int32_t p99_cdeg = 0;
    int32_t mean_cdeg = 0;

    p_snapshot->thermal_presence_x1000 = g_fusion_runtime.thermal_presence_x1000;
    p_snapshot->thermal_hotspot_points = g_fusion_runtime.thermal_hotspot_points;
    p_snapshot->thermal_hotspot_x1000 = g_fusion_runtime.thermal_hotspot_x1000;
    p_snapshot->thermal_hotspot_y1000 = g_fusion_runtime.thermal_hotspot_y1000;
    p_snapshot->thermal_hotspot_mean_cdeg = g_fusion_runtime.thermal_hotspot_mean_cdeg;
    p_snapshot->thermal_bed_x1000 = g_fusion_runtime.thermal_bed_x1000;
    p_snapshot->thermal_edge_x1000 = g_fusion_runtime.thermal_edge_x1000;

    if ((0U == p_thermal->sequence) ||
        (p_thermal->sequence == g_fusion_runtime.thermal_feature_sequence))
    {
        return;
    }

    for (uint32_t attempt = 0U; attempt < 3U; attempt++)
    {
        uint32_t generation_before = p_thermal->write_generation;
        uint32_t sequence;
        int32_t threshold;

        if (0U != (generation_before & 1U))
        {
            continue;
        }

        __DMB();
        sequence = p_thermal->sequence;
        p01_cdeg = p_thermal->p01_cdeg;
        p99_cdeg = p_thermal->p99_cdeg;
        mean_cdeg = p_thermal->mean_cdeg;
        if (((uint32_t) THERMAL_IPC_STATUS_FRAME_OK != p_thermal->status) ||
            ((uint32_t) THERMAL_FRAME_QUALITY_NORMAL != p_thermal->frame_quality) ||
            (THERMAL_IPC_POINT_COUNT != p_thermal->valid_points) ||
            (sequence != p_thermal->last_good_sequence))
        {
            return;
        }

        /* The thermal shared-matrix ABI uses 0.1 degree Celsius units. */
        threshold = mean_cdeg + (int32_t) FUSION_THERMAL_HOTSPOT_DELTA_DECI_C;
        if ((p99_cdeg - (int32_t) FUSION_THERMAL_HOTSPOT_P99_GAP_DECI_C) > threshold)
        {
            threshold = p99_cdeg - (int32_t) FUSION_THERMAL_HOTSPOT_P99_GAP_DECI_C;
        }

        hotspot_points = 0U;
        hotspot_weight = 0ULL;
        weighted_x = 0ULL;
        weighted_y = 0ULL;
        hotspot_sum = 0LL;
        for (uint32_t y = 0U; y < 64U; y++)
        {
            for (uint32_t x = 0U; x < 80U; x++)
            {
                int32_t value = THERMAL_IPC_POINTS[(y * 80U) + x];
                if (value >= threshold)
                {
                    uint32_t weight = (uint32_t) (value - threshold + 1);
                    hotspot_points++;
                    hotspot_sum += value;
                    hotspot_weight += weight;
                    weighted_x += ((uint64_t) weight) * x;
                    weighted_y += ((uint64_t) weight) * y;
                }
            }
        }
        __DMB();

        if ((generation_before == p_thermal->write_generation) &&
            (0U == (generation_before & 1U)) &&
            (sequence == p_thermal->sequence) &&
            (sequence == p_thermal->last_good_sequence))
        {
            accepted_sequence = sequence;
            break;
        }
    }

    if (0U == accepted_sequence)
    {
        p_thermal->generation_retry_count++;
        return;
    }

    {
        uint32_t contrast = (p99_cdeg > p01_cdeg) ? (uint32_t) (p99_cdeg - p01_cdeg) : 0U;
        uint32_t contrast_score = fusion_scale_x1000(contrast,
                                                      FUSION_THERMAL_CONTRAST_LOW_DECI_C,
                                                      FUSION_THERMAL_CONTRAST_HIGH_DECI_C);
        uint32_t area_score = fusion_scale_x1000(hotspot_points,
                                                  FUSION_THERMAL_HOTSPOT_MIN_POINTS,
                                                  FUSION_THERMAL_HOTSPOT_FULL_POINTS);

        g_fusion_runtime.thermal_feature_sequence = accepted_sequence;
        g_fusion_runtime.thermal_hotspot_points = hotspot_points;
        g_fusion_runtime.thermal_presence_x1000 =
            (hotspot_points < FUSION_THERMAL_HOTSPOT_MIN_POINTS) ? 0U :
            ((contrast_score * FUSION_CAL_THERMAL_CONTRAST_WEIGHT_X1000) +
             (area_score * FUSION_CAL_THERMAL_AREA_WEIGHT_X1000)) / 1000U;
        g_fusion_runtime.thermal_hotspot_mean_cdeg =
            (0U == hotspot_points) ? mean_cdeg : (int32_t) (hotspot_sum / (int64_t) hotspot_points);
        g_fusion_runtime.thermal_hotspot_x1000 =
            (0ULL == hotspot_weight) ? 500U : (uint32_t) ((weighted_x * 1000ULL) / (hotspot_weight * 79ULL));
        g_fusion_runtime.thermal_hotspot_y1000 =
            (0ULL == hotspot_weight) ? 500U : (uint32_t) ((weighted_y * 1000ULL) / (hotspot_weight * 63ULL));
        g_fusion_runtime.thermal_bed_x1000 =
            fusion_map_thermal_to_bed_x(g_fusion_runtime.thermal_hotspot_x1000);
        g_fusion_runtime.thermal_edge_x1000 = fusion_scale_x1000(
            fusion_abs_i32((int32_t) g_fusion_runtime.thermal_bed_x1000 - 500),
            FUSION_CAL_THERMAL_EDGE_LOW_X1000,
            FUSION_CAL_THERMAL_EDGE_HIGH_X1000);
    }

    p_snapshot->thermal_presence_x1000 = g_fusion_runtime.thermal_presence_x1000;
    p_snapshot->thermal_hotspot_points = g_fusion_runtime.thermal_hotspot_points;
    p_snapshot->thermal_hotspot_x1000 = g_fusion_runtime.thermal_hotspot_x1000;
    p_snapshot->thermal_hotspot_y1000 = g_fusion_runtime.thermal_hotspot_y1000;
    p_snapshot->thermal_hotspot_mean_cdeg = g_fusion_runtime.thermal_hotspot_mean_cdeg;
    p_snapshot->thermal_bed_x1000 = g_fusion_runtime.thermal_bed_x1000;
    p_snapshot->thermal_edge_x1000 = g_fusion_runtime.thermal_edge_x1000;
}

static void fusion_update_temperature(multimodal_fusion_snapshot_t * p_snapshot)
{
    volatile thermal_ipc_mailbox_t const * p_thermal = THERMAL_IPC_MAILBOX;
    volatile tmp117_ipc_mailbox_t const * p_temperature = TMP117_IPC_MAILBOX;
    int32_t minimum = INT32_MAX;
    int32_t maximum = INT32_MIN;
    uint32_t hot = 0U;
    uint32_t cold = 0U;
    int32_t left_sum = 0;
    int32_t right_sum = 0;
    uint32_t left_count = 0U;
    uint32_t right_count = 0U;

    p_snapshot->thermal_min_cdeg = p_thermal->min_cdeg;
    p_snapshot->thermal_max_cdeg = p_thermal->max_cdeg;
    for (uint32_t i = 0U; i < TMP117_IPC_SENSOR_COUNT; i++)
    {
        if (0U != (p_temperature->connected_mask & (1U << i)))
        {
            int32_t value = p_temperature->milli_c[i];
            if (value < minimum)
            {
                minimum = value;
            }
            if (value > maximum)
            {
                maximum = value;
            }
            hot += (value >= FUSION_OVERHEAT_MILLI_C) ? 1U : 0U;
            cold += (value <= FUSION_UNDERHEAT_MILLI_C) ? 1U : 0U;

            /* Physical placement: 0/1 left, 3 center, 2/4 right. */
            if (i <= 1U)
            {
                left_sum += value;
                left_count++;
            }
            else if ((2U == i) || (4U == i))
            {
                right_sum += value;
                right_count++;
            }
        }
    }
    p_snapshot->bed_min_milli_c = (INT32_MAX == minimum) ? 0 : minimum;
    p_snapshot->bed_max_milli_c = (INT32_MIN == maximum) ? 0 : maximum;
    g_fusion_runtime.hot_sensor_count = hot;
    g_fusion_runtime.cold_sensor_count = cold;
    g_fusion_runtime.bed_left_milli_c = (0U == left_count) ? 0 : left_sum / (int32_t) left_count;
    g_fusion_runtime.bed_center_milli_c =
        (0U != (p_temperature->connected_mask & (1U << 3))) ? p_temperature->milli_c[3] : 0;
    g_fusion_runtime.bed_right_milli_c = (0U == right_count) ? 0 : right_sum / (int32_t) right_count;
    g_fusion_runtime.bed_spread_milli_c =
        ((INT32_MAX == minimum) || (INT32_MIN == maximum)) ? 0 : maximum - minimum;
    g_fusion_runtime.bed_hotspot_region = 0U;
    if ((left_count > 0U) || (right_count > 0U) ||
        (0U != (p_temperature->connected_mask & (1U << 3))))
    {
        int32_t hottest = g_fusion_runtime.bed_left_milli_c;
        g_fusion_runtime.bed_hotspot_region = 1U;
        if (g_fusion_runtime.bed_center_milli_c > hottest)
        {
            hottest = g_fusion_runtime.bed_center_milli_c;
            g_fusion_runtime.bed_hotspot_region = 2U;
        }
        if (g_fusion_runtime.bed_right_milli_c > hottest)
        {
            g_fusion_runtime.bed_hotspot_region = 3U;
        }
    }

    p_snapshot->bed_left_milli_c = g_fusion_runtime.bed_left_milli_c;
    p_snapshot->bed_center_milli_c = g_fusion_runtime.bed_center_milli_c;
    p_snapshot->bed_right_milli_c = g_fusion_runtime.bed_right_milli_c;
    p_snapshot->bed_spread_milli_c = g_fusion_runtime.bed_spread_milli_c;
    p_snapshot->bed_hotspot_region = g_fusion_runtime.bed_hotspot_region;
}

static uint32_t fusion_detection_overlap_x1000(vision_detection_t const * p_subject,
                                                vision_detection_t const * p_object)
{
    uint32_t left = (p_subject->x > p_object->x) ? p_subject->x : p_object->x;
    uint32_t top = (p_subject->y > p_object->y) ? p_subject->y : p_object->y;
    uint32_t subject_right = (uint32_t) p_subject->x + p_subject->width;
    uint32_t object_right = (uint32_t) p_object->x + p_object->width;
    uint32_t subject_bottom = (uint32_t) p_subject->y + p_subject->height;
    uint32_t object_bottom = (uint32_t) p_object->y + p_object->height;
    uint32_t right = (subject_right < object_right) ? subject_right : object_right;
    uint32_t bottom = (subject_bottom < object_bottom) ? subject_bottom : object_bottom;
    uint32_t subject_area = (uint32_t) p_subject->width * p_subject->height;

    if ((right <= left) || (bottom <= top) || (0U == subject_area))
    {
        return 0U;
    }
    return fusion_clamp_u32(((right - left) * (bottom - top) * 1000U) / subject_area, 1000U);
}

static void fusion_set_airway_frame(uint32_t airway, uint32_t confidence_x1000)
{
    g_fusion_runtime.clear_frames = (MULTIMODAL_AIRWAY_CLEAR == airway) ?
                                    (g_fusion_runtime.clear_frames + 1U) : 0U;
    g_fusion_runtime.partial_frames = (MULTIMODAL_AIRWAY_PARTIAL == airway) ?
                                      (g_fusion_runtime.partial_frames + 1U) : 0U;
    g_fusion_runtime.covered_frames = (MULTIMODAL_AIRWAY_COVERED == airway) ?
                                      (g_fusion_runtime.covered_frames + 1U) : 0U;
    g_fusion_runtime.not_visible_frames = (MULTIMODAL_AIRWAY_NOT_VISIBLE == airway) ?
                                          (g_fusion_runtime.not_visible_frames + 1U) : 0U;
    g_fusion_runtime.airway_view = airway;
    g_fusion_runtime.airway_confidence_x1000 = fusion_clamp_u32(confidence_x1000, 1000U);
}

static void fusion_update_vision(uint32_t now, multimodal_fusion_snapshot_t * p_snapshot)
{
    vision_runtime_snapshot_t vision = {0};
    vision_detection_t head_proxy = {0};
    vision_detection_t const * p_infant = NULL;
    vision_detection_t const * p_head = NULL;
    vision_detection_t const * p_overlap_head = NULL;
    uint32_t covered_probability = 0U;
    uint32_t clear_probability = 0U;
    uint32_t partial_probability = 0U;
    uint32_t not_visible_probability = 0U;
    uint32_t previous_center_x_x1000 = g_fusion_runtime.infant_center_x1000;
    uint32_t previous_center_y_x1000 = g_fusion_runtime.infant_center_y1000;
    bool previous_center_valid = g_fusion_runtime.infant_center_valid;

    if (!vision_runtime_get_snapshot(&vision))
    {
        return;
    }

    if ((vision.sequence == g_fusion_runtime.vision_sequence) ||
        ((uint32_t) VISION_STATUS_READY != vision.status) ||
        (0U == vision.fusion_eligible))
    {
        p_snapshot->vision_sequence = g_fusion_runtime.vision_frame_sequence;
        p_snapshot->vision_age_ms = fusion_age_ms(now, g_fusion_runtime.vision_update_tick);
        p_snapshot->infant_present = (g_fusion_runtime.infant_score_x1000 >= FUSION_VISION_SCORE_X1000) ? 1U : 0U;
        p_snapshot->head_present = (g_fusion_runtime.head_score_x1000 >= FUSION_VISION_SCORE_X1000) ? 1U : 0U;
        p_snapshot->infant_at_edge = g_fusion_runtime.infant_at_edge ? 1U : 0U;
        p_snapshot->infant_edge_distance_x1000 = g_fusion_runtime.infant_edge_distance_x1000;
        return;
    }

    g_fusion_runtime.vision_sequence = vision.sequence;
    g_fusion_runtime.vision_frame_sequence = vision.frame_sequence;
    g_fusion_runtime.vision_update_tick = now;
    g_fusion_runtime.vision_width = vision.image_width;
    g_fusion_runtime.vision_height = vision.image_height;
    g_fusion_runtime.infant_score_x1000 = 0U;
    g_fusion_runtime.head_score_x1000 = 0U;
    g_fusion_runtime.caregiver_score_x1000 = 0U;
    g_fusion_runtime.infant_edge_distance_x1000 = 1000U;
    g_fusion_runtime.head_object_overlap_x1000 = 0U;
    g_fusion_runtime.infant_object_overlap_x1000 = 0U;
    g_fusion_runtime.infant_center_valid = false;
    g_fusion_runtime.infant_at_edge = false;
    g_fusion_runtime.occlusion_valid = false;
    g_fusion_runtime.head_proxy_valid = false;

    for (uint32_t i = 0U; (i < vision.detection_count) && (i < VISION_RUNTIME_MAX_DETECTIONS); i++)
    {
        vision_detection_t const * p_detection = &vision.detections[i];
        if ((VISION_SCENE_INFANT == p_detection->class_id) &&
            (p_detection->score_x1000 > g_fusion_runtime.infant_score_x1000))
        {
            uint32_t center_x = (uint32_t) p_detection->x + (p_detection->width / 2U);
            uint32_t center_y = (uint32_t) p_detection->y + (p_detection->height / 2U);
            uint32_t x_right = (center_x < vision.image_width) ? (vision.image_width - center_x) : 0U;
            uint32_t y_bottom = (center_y < vision.image_height) ? (vision.image_height - center_y) : 0U;
            uint32_t x_distance = (center_x < x_right) ? center_x : x_right;
            uint32_t y_distance = (center_y < y_bottom) ? center_y : y_bottom;
            uint32_t x_norm = (0U == vision.image_width) ? 0U : (x_distance * 2000U) / vision.image_width;
            uint32_t y_norm = (0U == vision.image_height) ? 0U : (y_distance * 2000U) / vision.image_height;
            uint32_t edge_distance = (x_norm < y_norm) ? x_norm : y_norm;

            g_fusion_runtime.infant_score_x1000 = p_detection->score_x1000;
            p_infant = p_detection;
            g_fusion_runtime.infant_edge_distance_x1000 = fusion_clamp_u32(edge_distance, 1000U);
            g_fusion_runtime.infant_center_x1000 = (0U == vision.image_width) ?
                                                   0U : (center_x * 1000U) / vision.image_width;
            g_fusion_runtime.infant_center_y1000 = (0U == vision.image_height) ?
                                                   0U : (center_y * 1000U) / vision.image_height;
            g_fusion_runtime.infant_center_valid = true;
            g_fusion_runtime.infant_at_edge =
                g_fusion_runtime.infant_edge_distance_x1000 <= FUSION_VISION_EDGE_MARGIN_X1000;
        }
        else if ((VISION_SCENE_HEAD_FACE == p_detection->class_id) &&
                 (p_detection->score_x1000 > g_fusion_runtime.head_score_x1000))
        {
            g_fusion_runtime.head_score_x1000 = p_detection->score_x1000;
            p_head = p_detection;
        }
        else if ((VISION_SCENE_CAREGIVER == p_detection->class_id) &&
                 (p_detection->score_x1000 > g_fusion_runtime.caregiver_score_x1000))
        {
            g_fusion_runtime.caregiver_score_x1000 = p_detection->score_x1000;
        }
    }

    if (previous_center_valid && g_fusion_runtime.infant_center_valid)
    {
        uint32_t delta = fusion_abs_i32((int32_t) g_fusion_runtime.infant_center_x1000 -
                                        (int32_t) previous_center_x_x1000) +
                         fusion_abs_i32((int32_t) g_fusion_runtime.infant_center_y1000 -
                                        (int32_t) previous_center_y_x1000);
        g_fusion_runtime.visual_motion_x1000 = fusion_clamp_u32(delta * 4U, 1000U);
    }
    else
    {
        g_fusion_runtime.visual_motion_x1000 = 0U;
    }

    p_overlap_head = p_head;
    if ((NULL == p_overlap_head) && (NULL != p_infant))
    {
        uint32_t proxy_width = ((uint32_t) p_infant->width *
                                FUSION_VISION_HEAD_PROXY_WIDTH_X1000) / 1000U;
        uint32_t proxy_height = ((uint32_t) p_infant->height *
                                 FUSION_VISION_HEAD_PROXY_HEIGHT_X1000) / 1000U;

        if (0U == proxy_width)
        {
            proxy_width = 1U;
        }
        if (0U == proxy_height)
        {
            proxy_height = 1U;
        }
        head_proxy = *p_infant;
        head_proxy.x = (uint16_t) ((uint32_t) p_infant->x +
                                  (((uint32_t) p_infant->width *
                                    FUSION_VISION_HEAD_PROXY_START_X1000) / 1000U));
        head_proxy.y = (uint16_t) ((uint32_t) p_infant->y +
                                  (((uint32_t) p_infant->height *
                                    FUSION_VISION_HEAD_PROXY_TOP_X1000) / 1000U));
        head_proxy.width = (uint16_t) proxy_width;
        head_proxy.height = (uint16_t) proxy_height;
        p_overlap_head = &head_proxy;
        g_fusion_runtime.head_proxy_valid = true;
    }

    if ((NULL != p_overlap_head) || (NULL != p_infant))
    {
        for (uint32_t i = 0U; (i < vision.detection_count) && (i < VISION_RUNTIME_MAX_DETECTIONS); i++)
        {
            vision_detection_t const * p_object = &vision.detections[i];
            if ((VISION_SCENE_BLANKET == p_object->class_id) ||
                (VISION_SCENE_SOFT_OBJECT == p_object->class_id))
            {
                if (NULL != p_overlap_head)
                {
                    uint32_t overlap = fusion_detection_overlap_x1000(p_overlap_head, p_object);
                    if (overlap > g_fusion_runtime.head_object_overlap_x1000)
                    {
                        g_fusion_runtime.head_object_overlap_x1000 = overlap;
                    }
                }
                if (NULL != p_infant)
                {
                    uint32_t overlap = fusion_detection_overlap_x1000(p_infant, p_object);
                    if (overlap > g_fusion_runtime.infant_object_overlap_x1000)
                    {
                        g_fusion_runtime.infant_object_overlap_x1000 = overlap;
                    }
                }
            }
        }
    }

    g_fusion_runtime.occlusion_valid = (0U != vision.occlusion_valid);
    g_fusion_runtime.visual_hidden_frames =
        ((g_fusion_runtime.infant_score_x1000 < FUSION_VISION_SCORE_X1000) &&
         (g_fusion_runtime.head_score_x1000 < FUSION_VISION_SCORE_X1000)) ?
        (g_fusion_runtime.visual_hidden_frames + 1U) : 0U;

    clear_probability = vision.occlusion_probabilities_x1000[VISION_OCCLUSION_CLEAR];
    partial_probability = vision.occlusion_probabilities_x1000[VISION_OCCLUSION_PARTIAL];
    covered_probability = vision.occlusion_probabilities_x1000[VISION_OCCLUSION_COVERED];
    not_visible_probability = vision.occlusion_probabilities_x1000[VISION_OCCLUSION_NOT_VISIBLE];
    bool classifier_clear = g_fusion_runtime.occlusion_valid &&
                            (clear_probability >= FUSION_VISION_CLEAR_X1000) &&
                            (clear_probability > partial_probability) &&
                            (clear_probability > covered_probability) &&
                            (clear_probability > not_visible_probability);
    if (!g_fusion_runtime.occlusion_valid)
    {
        fusion_set_airway_frame(MULTIMODAL_AIRWAY_UNKNOWN, 0U);
    }
    else if (covered_probability >= FUSION_VISION_COVERED_X1000)
    {
        fusion_set_airway_frame(MULTIMODAL_AIRWAY_COVERED, covered_probability);
    }
    else if (classifier_clear)
    {
        /* A confident classifier result is stronger evidence than the coarse
         * whole-infant head proxy.  The proxy is only a fallback when the
         * classifier itself is uncertain. */
        fusion_set_airway_frame(MULTIMODAL_AIRWAY_CLEAR, clear_probability);
    }
    else if (partial_probability >= FUSION_VISION_PARTIAL_X1000)
    {
        fusion_set_airway_frame(MULTIMODAL_AIRWAY_PARTIAL, partial_probability);
    }
    else if (g_fusion_runtime.head_proxy_valid &&
             (g_fusion_runtime.head_object_overlap_x1000 >=
              FUSION_VISION_HEAD_PROXY_COVER_X1000))
    {
        /* The installed camera sees the dummy's head at the left-hand end of
         * the infant box.  Blanket overlap there is supporting evidence only: it
         * raises a debounced PARTIAL warning, never a definite COVERED state. */
        fusion_set_airway_frame(MULTIMODAL_AIRWAY_PARTIAL,
                                g_fusion_runtime.head_object_overlap_x1000);
    }
    else if (not_visible_probability >= FUSION_VISION_NOT_VISIBLE_X1000)
    {
        fusion_set_airway_frame(MULTIMODAL_AIRWAY_NOT_VISIBLE, not_visible_probability);
    }
    else
    {
        fusion_set_airway_frame(MULTIMODAL_AIRWAY_UNKNOWN, 0U);
    }

    g_fusion_runtime.behavior_class = vision.behavior_class;
    g_fusion_runtime.behavior_confidence_x1000 = vision.behavior_confidence_x1000;
    g_fusion_models.sleep_confidence =
        fusion_clamp_confidence((vision.behavior_probabilities_x1000[VISION_BEHAVIOR_LYING] + 5U) / 10U);
    {
        uint32_t face_risk = (covered_probability > partial_probability) ?
                             covered_probability : partial_probability;
        if (classifier_clear)
        {
            face_risk = 0U;
        }
        else if (g_fusion_runtime.head_object_overlap_x1000 > face_risk)
        {
            face_risk = g_fusion_runtime.head_object_overlap_x1000;
        }
        g_fusion_models.face_covered_confidence = fusion_clamp_confidence((face_risk + 5U) / 10U);
    }
    g_fusion_models.standing_confidence =
        fusion_clamp_confidence((vision.behavior_probabilities_x1000[VISION_BEHAVIOR_STANDING] + 5U) / 10U);
    g_fusion_models.movement_confidence =
        fusion_clamp_confidence((g_fusion_runtime.visual_motion_x1000 + 5U) / 10U);
    __DMB();
    g_fusion_models.vision_tick = now;

    p_snapshot->vision_sequence = g_fusion_runtime.vision_frame_sequence;
    p_snapshot->vision_age_ms = 0U;
    p_snapshot->infant_present = (g_fusion_runtime.infant_score_x1000 >= FUSION_VISION_SCORE_X1000) ? 1U : 0U;
    p_snapshot->head_present = (g_fusion_runtime.head_score_x1000 >= FUSION_VISION_SCORE_X1000) ? 1U : 0U;
    p_snapshot->infant_at_edge = g_fusion_runtime.infant_at_edge ? 1U : 0U;
    p_snapshot->infant_edge_distance_x1000 = g_fusion_runtime.infant_edge_distance_x1000;
}

static void fusion_update_health(uint32_t now, multimodal_fusion_snapshot_t * p_snapshot)
{
    bool grace = fusion_age_ms(now, g_fusion_runtime.boot_tick) < FUSION_BOOT_GRACE_MS;
    bool camera_grace = fusion_age_ms(now, g_fusion_runtime.boot_tick) < FUSION_CAMERA_BOOT_GRACE_MS;

    /* Hold the last complete frame across an occasional partial scan.  A
     * source becomes faulty only after it has failed continuously for its
     * own acquisition period, rather than after one transient mailbox state. */
    if (fusion_tick_fresh(now, g_fusion_runtime.thermal_good_tick, FUSION_THERMAL_STALE_MS))
    {
        p_snapshot->valid_mask |= MULTIMODAL_SOURCE_THERMAL;
    }
    else if (!grace)
    {
        p_snapshot->sensor_fault_mask |= MULTIMODAL_FAULT_THERMAL;
    }

    if (fusion_tick_fresh(now, g_fusion_runtime.temperature_good_tick, FUSION_TEMPERATURE_STALE_MS))
    {
        p_snapshot->valid_mask |= MULTIMODAL_SOURCE_TEMPERATURE;
    }
    else if (!grace)
    {
        p_snapshot->sensor_fault_mask |= MULTIMODAL_FAULT_TEMPERATURE;
    }

    if (fusion_tick_fresh(now, g_fusion_runtime.pressure_good_tick, FUSION_PRESSURE_STALE_MS))
    {
        p_snapshot->valid_mask |= MULTIMODAL_SOURCE_PRESSURE;
    }
    else if (!grace)
    {
        p_snapshot->sensor_fault_mask |= MULTIMODAL_FAULT_PRESSURE;
    }

    if (fusion_tick_fresh(now, g_fusion_runtime.mic_good_tick, FUSION_MIC_STALE_MS))
    {
        p_snapshot->valid_mask |= MULTIMODAL_SOURCE_MIC;
    }
    else if (!grace)
    {
        p_snapshot->sensor_fault_mask |= MULTIMODAL_FAULT_MIC;
    }

    if (fusion_tick_fresh(now, g_fusion_runtime.vision_update_tick, FUSION_CAMERA_STALE_MS))
    {
        p_snapshot->valid_mask |= MULTIMODAL_SOURCE_CAMERA;
    }
    else if (!camera_grace)
    {
        p_snapshot->sensor_fault_mask |= MULTIMODAL_FAULT_CAMERA;
    }

    /*
     * Ambient CO2/temperature/humidity only affects source health.  It is not
     * a key blindness source and is deliberately excluded from bed-temperature
     * thresholds, which remain owned by the TMP117 array.
     */
    if (fusion_tick_fresh(now,
                          g_fusion_runtime.environment_good_tick,
                          FUSION_ENVIRONMENT_STALE_MS))
    {
        p_snapshot->valid_mask |= MULTIMODAL_SOURCE_ENVIRONMENT;
    }
    else if (!grace)
    {
        p_snapshot->sensor_fault_mask |= MULTIMODAL_FAULT_ENVIRONMENT;
    }
}

static void fusion_update_context(uint32_t now, multimodal_fusion_snapshot_t * p_snapshot)
{
    bool pressure_valid = 0U != (p_snapshot->valid_mask & MULTIMODAL_SOURCE_PRESSURE);
    bool vision_valid = 0U != (p_snapshot->valid_mask & MULTIMODAL_SOURCE_CAMERA);
    bool thermal_valid = 0U != (p_snapshot->valid_mask & MULTIMODAL_SOURCE_THERMAL);
    bool audio_valid = fusion_tick_fresh(now, g_fusion_models.audio_tick, FUSION_MODEL_STALE_MS);
    bool pressure_present = pressure_valid && (0U != p_snapshot->occupied);
    bool cry = audio_valid && (g_fusion_models.cry_confidence >= FUSION_CRY_THRESHOLD);
    bool visual_active = vision_valid &&
                         ((g_fusion_runtime.visual_motion_x1000 >= FUSION_VISION_MOTION_X1000) ||
                          (((VISION_BEHAVIOR_SITTING == g_fusion_runtime.behavior_class) ||
                            (VISION_BEHAVIOR_CRAWLING_KNEELING == g_fusion_runtime.behavior_class) ||
                            (VISION_BEHAVIOR_STANDING == g_fusion_runtime.behavior_class)) &&
                           (g_fusion_runtime.behavior_confidence_x1000 >= FUSION_VISION_CLASS_X1000)));
    bool visual_quiet = vision_valid &&
                        (VISION_BEHAVIOR_LYING == g_fusion_runtime.behavior_class) &&
                        (g_fusion_runtime.behavior_confidence_x1000 >= FUSION_VISION_PARTIAL_X1000);
    uint32_t weighted_presence = 0U;
    uint32_t presence_weight = 0U;
    uint32_t valid_sources = 0U;
    uint32_t positive_sources = 0U;
    uint32_t negative_sources = 0U;
    uint32_t visual_activity_x1000 = visual_active ?
                                        ((g_fusion_runtime.visual_motion_x1000 >
                                          g_fusion_runtime.behavior_confidence_x1000) ?
                                         g_fusion_runtime.visual_motion_x1000 :
                                         g_fusion_runtime.behavior_confidence_x1000) : 0U;

    p_snapshot->cry_confidence = audio_valid ? g_fusion_models.cry_confidence : 0U;
    p_snapshot->sleep_confidence = vision_valid ? g_fusion_models.sleep_confidence : 0U;
    p_snapshot->face_covered_confidence = vision_valid ? g_fusion_models.face_covered_confidence : 0U;
    p_snapshot->standing_confidence = vision_valid ? g_fusion_models.standing_confidence : 0U;
    p_snapshot->movement_confidence = vision_valid ? g_fusion_models.movement_confidence : 0U;
    p_snapshot->cry_cause_hint = (0U != g_cry_type_valid) ? g_cry_type_id : 0U;
    p_snapshot->cry_cause_confidence = (0U != g_cry_type_valid) ?
                                       ((g_cry_type_confidence_x1000 + 5U) / 10U) : 0U;
    p_snapshot->calibration_profile_version = FUSION_CALIBRATION_PROFILE_VERSION;

    if (pressure_valid)
    {
        p_snapshot->pressure_presence_x1000 = pressure_present ?
            fusion_clamp_u32(620U + (p_snapshot->pressure_active_points * 100U) +
                             fusion_clamp_u32(p_snapshot->pressure_total * 20U, 180U), 1000U) : 0U;
        weighted_presence += p_snapshot->pressure_presence_x1000 * FUSION_PRESSURE_WEIGHT;
        presence_weight += FUSION_PRESSURE_WEIGHT;
        valid_sources++;
    }
    if (vision_valid)
    {
        uint32_t head_evidence = (g_fusion_runtime.head_score_x1000 * 850U) / 1000U;
        p_snapshot->vision_presence_x1000 =
            (g_fusion_runtime.infant_score_x1000 > head_evidence) ?
            g_fusion_runtime.infant_score_x1000 : head_evidence;
        weighted_presence += p_snapshot->vision_presence_x1000 * FUSION_VISION_WEIGHT;
        presence_weight += FUSION_VISION_WEIGHT;
        valid_sources++;
    }
    if (thermal_valid)
    {
        p_snapshot->thermal_presence_x1000 = g_fusion_runtime.thermal_presence_x1000;
        weighted_presence += p_snapshot->thermal_presence_x1000 * FUSION_THERMAL_WEIGHT;
        presence_weight += FUSION_THERMAL_WEIGHT;
        valid_sources++;
    }

    p_snapshot->presence_score_x1000 = (0U == presence_weight) ?
                                       0U : weighted_presence / presence_weight;
    positive_sources += (pressure_valid &&
                         (p_snapshot->pressure_presence_x1000 >= FUSION_PRESENCE_POSITIVE_X1000)) ? 1U : 0U;
    positive_sources += (vision_valid &&
                         (p_snapshot->vision_presence_x1000 >= FUSION_PRESENCE_POSITIVE_X1000)) ? 1U : 0U;
    positive_sources += (thermal_valid &&
                         (p_snapshot->thermal_presence_x1000 >= FUSION_PRESENCE_POSITIVE_X1000)) ? 1U : 0U;
    negative_sources += (pressure_valid &&
                         (p_snapshot->pressure_presence_x1000 <= FUSION_PRESENCE_NEGATIVE_X1000)) ? 1U : 0U;
    negative_sources += (vision_valid &&
                         (p_snapshot->vision_presence_x1000 <= FUSION_PRESENCE_NEGATIVE_X1000)) ? 1U : 0U;
    negative_sources += (thermal_valid &&
                         (p_snapshot->thermal_presence_x1000 <= FUSION_PRESENCE_NEGATIVE_X1000)) ? 1U : 0U;
    p_snapshot->presence_source_count = positive_sources;
    if (pressure_present && thermal_valid &&
        (p_snapshot->thermal_presence_x1000 >= FUSION_PRESENCE_POSITIVE_X1000))
    {
        uint32_t pressure_bed_x1000 = fusion_clamp_u32(
            (uint32_t) ((((uint64_t) p_snapshot->pressure_x100) * 1000ULL) / 1500ULL), 1000U);
        p_snapshot->position_agreement_x1000 = fusion_position_agreement(
            pressure_bed_x1000, p_snapshot->thermal_bed_x1000);
    }

    if ((positive_sources >= 2U) &&
        (p_snapshot->presence_score_x1000 >= FUSION_PRESENCE_CONFIRM_X1000))
    {
        p_snapshot->presence = MULTIMODAL_PRESENCE_PRESENT;
        g_fusion_runtime.absence_since_tick = 0U;
    }
    else if ((valid_sources >= 2U) && (negative_sources >= 2U) && (0U == positive_sources))
    {
        if (0U == g_fusion_runtime.absence_since_tick)
        {
            g_fusion_runtime.absence_since_tick = now;
        }
        p_snapshot->presence =
            (fusion_age_ms(now, g_fusion_runtime.absence_since_tick) >= FUSION_ABSENT_CONFIRM_MS) ?
            MULTIMODAL_PRESENCE_ABSENT : MULTIMODAL_PRESENCE_UNCERTAIN;
    }
    else if ((positive_sources > 0U) || (valid_sources >= 2U))
    {
        p_snapshot->presence = MULTIMODAL_PRESENCE_UNCERTAIN;
        g_fusion_runtime.absence_since_tick = 0U;
    }
    else
    {
        p_snapshot->presence = MULTIMODAL_PRESENCE_UNKNOWN;
    }

    if (cry)
    {
        if (0U == g_fusion_runtime.cry_since_tick)
        {
            g_fusion_runtime.cry_since_tick = now;
        }
        g_fusion_runtime.last_cry_tick = now;
        p_snapshot->distress =
            (fusion_age_ms(now, g_fusion_runtime.cry_since_tick) >= FUSION_PERSISTENT_CRY_MS) ?
            MULTIMODAL_DISTRESS_PERSISTENT_CRY : MULTIMODAL_DISTRESS_CRYING;
    }
    else
    {
        g_fusion_runtime.cry_since_tick = 0U;
        p_snapshot->distress = audio_valid ? MULTIMODAL_DISTRESS_NONE : MULTIMODAL_DISTRESS_UNKNOWN;
    }

    p_snapshot->activity_score_x1000 =
        (visual_activity_x1000 > p_snapshot->pressure_activity_x1000) ?
        visual_activity_x1000 : p_snapshot->pressure_activity_x1000;

    if (p_snapshot->activity_score_x1000 >= FUSION_PRESSURE_ACTIVE_X1000)
    {
        if (0U == g_fusion_runtime.awake_since_tick)
        {
            g_fusion_runtime.awake_since_tick = now;
        }
    }
    else
    {
        g_fusion_runtime.awake_since_tick = 0U;
    }

    if (visual_quiet && (p_snapshot->pressure_activity_x1000 < FUSION_PRESSURE_ROLLING_X1000))
    {
        if (0U == g_fusion_runtime.quiet_since_tick)
        {
            g_fusion_runtime.quiet_since_tick = now;
        }
    }
    else
    {
        g_fusion_runtime.quiet_since_tick = 0U;
    }

    if (cry || ((0U != g_fusion_runtime.awake_since_tick) &&
                (fusion_age_ms(now, g_fusion_runtime.awake_since_tick) >= FUSION_AWAKE_CONFIRM_MS)))
    {
        p_snapshot->arousal = MULTIMODAL_AROUSAL_AWAKE;
    }
    else if ((MULTIMODAL_PRESENCE_PRESENT == p_snapshot->presence) &&
             (0U != g_fusion_runtime.quiet_since_tick) &&
             (fusion_age_ms(now, g_fusion_runtime.quiet_since_tick) >= FUSION_SLEEP_CONFIRM_MS) &&
             ((0U == g_fusion_runtime.last_cry_tick) ||
              (fusion_age_ms(now, g_fusion_runtime.last_cry_tick) >= FUSION_NO_CRY_SLEEP_MS)))
    {
        p_snapshot->arousal = MULTIMODAL_AROUSAL_SLEEPING;
    }
    else if ((MULTIMODAL_PRESENCE_PRESENT == p_snapshot->presence) ||
             (MULTIMODAL_PRESENCE_UNCERTAIN == p_snapshot->presence))
    {
        p_snapshot->arousal = MULTIMODAL_AROUSAL_TRANSITION;
    }
    else
    {
        p_snapshot->arousal = MULTIMODAL_AROUSAL_UNKNOWN;
    }

    if (!pressure_valid)
    {
        p_snapshot->bed_motion = MULTIMODAL_BED_MOTION_UNKNOWN;
    }
    else if (p_snapshot->pressure_activity_x1000 >= FUSION_PRESSURE_ACTIVE_X1000)
    {
        p_snapshot->bed_motion = MULTIMODAL_BED_MOTION_ACTIVE;
    }
    else if (p_snapshot->pressure_activity_x1000 >= FUSION_PRESSURE_ROLLING_X1000)
    {
        p_snapshot->bed_motion = MULTIMODAL_BED_MOTION_ROLLING;
    }
    else
    {
        p_snapshot->bed_motion = MULTIMODAL_BED_MOTION_QUIET;
    }

    p_snapshot->airway_view = vision_valid ? g_fusion_runtime.airway_view : MULTIMODAL_AIRWAY_UNKNOWN;
    if (vision_valid)
    {
        switch (p_snapshot->airway_view)
        {
            case MULTIMODAL_AIRWAY_COVERED:
                p_snapshot->airway_risk_x1000 =
                    (g_fusion_runtime.airway_confidence_x1000 > 700U) ?
                    g_fusion_runtime.airway_confidence_x1000 : 700U;
                break;
            case MULTIMODAL_AIRWAY_PARTIAL:
                p_snapshot->airway_risk_x1000 = g_fusion_runtime.airway_confidence_x1000;
                break;
            case MULTIMODAL_AIRWAY_NOT_VISIBLE:
                p_snapshot->airway_risk_x1000 =
                    (g_fusion_runtime.airway_confidence_x1000 * 800U) / 1000U;
                break;
            default:
                p_snapshot->airway_risk_x1000 = 0U;
                break;
        }
    }

    {
        uint32_t pressure_edge_score = pressure_present ? p_snapshot->pressure_edge_ratio_x1000 : 0U;
        uint32_t vision_edge_score = (vision_valid && g_fusion_runtime.infant_at_edge) ?
                                     (1000U - g_fusion_runtime.infant_edge_distance_x1000) : 0U;
        bool thermal_edge_valid = thermal_valid &&
                                  (p_snapshot->thermal_presence_x1000 >= FUSION_PRESENCE_POSITIVE_X1000);
        uint32_t edge_weight = 0U;
        uint32_t edge_total = 0U;
        if (pressure_valid)
        {
            edge_total += pressure_edge_score * FUSION_CAL_EDGE_PRESSURE_WEIGHT;
            edge_weight += FUSION_CAL_EDGE_PRESSURE_WEIGHT;
        }
        if (vision_valid)
        {
            edge_total += vision_edge_score * FUSION_CAL_EDGE_VISION_WEIGHT;
            edge_weight += FUSION_CAL_EDGE_VISION_WEIGHT;
        }
        if (thermal_edge_valid)
        {
            edge_total += p_snapshot->thermal_edge_x1000 * FUSION_CAL_EDGE_THERMAL_WEIGHT;
            edge_weight += FUSION_CAL_EDGE_THERMAL_WEIGHT;
        }
        p_snapshot->edge_risk_x1000 = (0U == edge_weight) ? 0U : edge_total / edge_weight;
        if ((VISION_BEHAVIOR_STANDING == g_fusion_runtime.behavior_class) &&
            (g_fusion_runtime.behavior_confidence_x1000 >= FUSION_VISION_CLASS_X1000))
        {
            p_snapshot->edge_risk_x1000 =
                fusion_clamp_u32(p_snapshot->edge_risk_x1000 + 150U, 1000U);
        }
    }

    if (g_fusion_runtime.occupied_seen &&
        (MULTIMODAL_PRESENCE_ABSENT == p_snapshot->presence) &&
        (valid_sources >= 2U))
    {
        p_snapshot->edge_state = MULTIMODAL_EDGE_OUTSIDE;
    }
    else if ((p_snapshot->edge_risk_x1000 >= 700U) &&
             (VISION_BEHAVIOR_STANDING == g_fusion_runtime.behavior_class) &&
             (g_fusion_runtime.behavior_confidence_x1000 >= FUSION_VISION_CLASS_X1000))
    {
        p_snapshot->edge_state = MULTIMODAL_EDGE_GRAB;
    }
    else if ((p_snapshot->edge_risk_x1000 >= 600U) && (positive_sources > 0U))
    {
        p_snapshot->edge_state = MULTIMODAL_EDGE_NEAR;
    }
    else if ((MULTIMODAL_PRESENCE_PRESENT == p_snapshot->presence) ||
             (MULTIMODAL_PRESENCE_UNCERTAIN == p_snapshot->presence))
    {
        p_snapshot->edge_state = MULTIMODAL_EDGE_CENTER;
    }
    else
    {
        p_snapshot->edge_state = MULTIMODAL_EDGE_UNKNOWN;
    }

    {
        bool body_temperature_supported = thermal_valid &&
            (MULTIMODAL_PRESENCE_PRESENT == p_snapshot->presence) &&
            (p_snapshot->thermal_presence_x1000 >= FUSION_PRESENCE_POSITIVE_X1000) &&
            (p_snapshot->thermal_hotspot_points >= FUSION_THERMAL_HOTSPOT_MIN_POINTS) &&
            ((0U != p_snapshot->infant_present) || (positive_sources >= 2U));

        if (!body_temperature_supported)
        {
            p_snapshot->body_temperature = MULTIMODAL_TEMPERATURE_UNKNOWN;
        }
        else if (p_snapshot->thermal_hotspot_mean_cdeg >=
                 p_snapshot->environment_thresholds.body_temperature_max_deci_c)
        {
            p_snapshot->body_temperature = MULTIMODAL_TEMPERATURE_HIGH;
        }
        else if (p_snapshot->thermal_hotspot_mean_cdeg <=
                 (p_snapshot->environment_thresholds.body_temperature_max_deci_c -
                  FUSION_BODY_FEVER_HYSTERESIS_DECI_C))
        {
            p_snapshot->body_temperature = MULTIMODAL_TEMPERATURE_NORMAL;
        }
        else
        {
            fusion_incident_runtime_t const * p_fever =
                &g_fusion_runtime.incidents[MULTIMODAL_INCIDENT_SUSPECTED_FEVER];
            p_snapshot->body_temperature =
                (MULTIMODAL_INCIDENT_CLEAR == p_fever->lifecycle) ?
                MULTIMODAL_TEMPERATURE_NORMAL : MULTIMODAL_TEMPERATURE_HIGH;
        }
    }
    if (0U != (p_snapshot->valid_mask & MULTIMODAL_SOURCE_TEMPERATURE))
    {
        if ((g_fusion_runtime.hot_sensor_count >= 2U) ||
            (p_snapshot->bed_center_milli_c >= FUSION_OVERHEAT_MILLI_C))
        {
            p_snapshot->bed_thermal = MULTIMODAL_TEMPERATURE_HIGH;
        }
        else if (g_fusion_runtime.cold_sensor_count >= 2U)
        {
            p_snapshot->bed_thermal = MULTIMODAL_TEMPERATURE_LOW;
        }
        else
        {
            p_snapshot->bed_thermal = MULTIMODAL_TEMPERATURE_NORMAL;
        }
    }
    else
    {
        p_snapshot->bed_thermal = MULTIMODAL_TEMPERATURE_UNKNOWN;
    }

    p_snapshot->caregiver = !vision_valid ? MULTIMODAL_CONTEXT_UNKNOWN :
        ((g_fusion_runtime.caregiver_score_x1000 >= FUSION_VISION_SCORE_X1000) ?
         MULTIMODAL_CONTEXT_PRESENT : MULTIMODAL_CONTEXT_ABSENT);

    {
        uint32_t key_faults = p_snapshot->sensor_fault_mask &
                              (MULTIMODAL_FAULT_THERMAL |
                               MULTIMODAL_FAULT_PRESSURE |
                               MULTIMODAL_FAULT_CAMERA);
        uint32_t fault_count = fusion_count_bits(p_snapshot->sensor_fault_mask);
        if (fusion_count_bits(key_faults) >= 2U)
        {
            p_snapshot->system_health = MULTIMODAL_HEALTH_BLIND;
        }
        else if (fault_count > 0U)
        {
            p_snapshot->system_health = MULTIMODAL_HEALTH_DEGRADED;
        }
        else
        {
            p_snapshot->system_health = MULTIMODAL_HEALTH_OK;
        }
    }
}

static void fusion_update_incident(multimodal_incident_type_t type,
                                   bool condition,
                                   uint32_t level,
                                   uint32_t severity,
                                   uint32_t confidence,
                                   uint32_t source_mask,
                                   uint32_t confirm_ms,
                                   uint32_t recovery_ms,
                                   uint32_t now)
{
    fusion_incident_runtime_t * p_incident = &g_fusion_runtime.incidents[type];

    confidence = fusion_clamp_confidence(confidence);
    switch (p_incident->lifecycle)
    {
        case MULTIMODAL_INCIDENT_CLEAR:
            if (condition)
            {
                p_incident->lifecycle = MULTIMODAL_INCIDENT_CANDIDATE;
                p_incident->lifecycle_since_tick = now;
                p_incident->level = level;
                p_incident->severity = severity;
                p_incident->confidence = confidence;
                p_incident->source_mask = source_mask;
            }
            break;

        case MULTIMODAL_INCIDENT_CANDIDATE:
            if (!condition)
            {
                memset(p_incident, 0, sizeof(*p_incident));
            }
            else if (fusion_age_ms(now, p_incident->lifecycle_since_tick) >= confirm_ms)
            {
                p_incident->lifecycle = MULTIMODAL_INCIDENT_ACTIVE;
                p_incident->lifecycle_since_tick = now;
                p_incident->opened_tick = now;
                p_incident->incident_id = ++g_fusion_runtime.next_incident_id;
                p_incident->level = level;
                p_incident->severity = severity;
                p_incident->confidence = confidence;
                p_incident->source_mask = source_mask;
                fusion_queue_event(type, MULTIMODAL_INCIDENT_ACTION_OPEN, now, p_incident);
            }
            else
            {
                p_incident->level = level;
                p_incident->severity = severity;
                p_incident->confidence = confidence;
                p_incident->source_mask = source_mask;
            }
            break;

        case MULTIMODAL_INCIDENT_ACTIVE:
        case MULTIMODAL_INCIDENT_ESCALATED:
            if (!condition)
            {
                p_incident->lifecycle = MULTIMODAL_INCIDENT_RECOVERY_HOLD;
                p_incident->lifecycle_since_tick = now;
                fusion_queue_event(type, MULTIMODAL_INCIDENT_ACTION_UPDATE, now, p_incident);
            }
            else
            {
                bool escalated = (level > p_incident->level) || (severity > p_incident->severity);
                p_incident->level = level;
                p_incident->severity = severity;
                p_incident->confidence = confidence;
                p_incident->source_mask = source_mask;
                if (escalated)
                {
                    p_incident->lifecycle = MULTIMODAL_INCIDENT_ESCALATED;
                    p_incident->lifecycle_since_tick = now;
                    fusion_queue_event(type, MULTIMODAL_INCIDENT_ACTION_ESCALATE, now, p_incident);
                }
            }
            break;

        case MULTIMODAL_INCIDENT_RECOVERY_HOLD:
            if (condition)
            {
                p_incident->lifecycle = (level > 2U) ?
                                        MULTIMODAL_INCIDENT_ESCALATED : MULTIMODAL_INCIDENT_ACTIVE;
                p_incident->lifecycle_since_tick = now;
                p_incident->level = level;
                p_incident->severity = severity;
                p_incident->confidence = confidence;
                p_incident->source_mask = source_mask;
                fusion_queue_event(type, MULTIMODAL_INCIDENT_ACTION_UPDATE, now, p_incident);
            }
            else if (fusion_age_ms(now, p_incident->lifecycle_since_tick) >= recovery_ms)
            {
                p_incident->lifecycle = MULTIMODAL_INCIDENT_RESOLVED;
                p_incident->lifecycle_since_tick = now;
                fusion_queue_event(type, MULTIMODAL_INCIDENT_ACTION_RESOLVE, now, p_incident);
            }
            break;

        case MULTIMODAL_INCIDENT_RESOLVED:
        default:
            memset(p_incident, 0, sizeof(*p_incident));
            break;
    }
}

static bool fusion_incident_latched(multimodal_incident_type_t type)
{
    multimodal_incident_lifecycle_t lifecycle = g_fusion_runtime.incidents[type].lifecycle;
    return (MULTIMODAL_INCIDENT_CANDIDATE == lifecycle) ||
           (MULTIMODAL_INCIDENT_ACTIVE == lifecycle) ||
           (MULTIMODAL_INCIDENT_ESCALATED == lifecycle) ||
           (MULTIMODAL_INCIDENT_RECOVERY_HOLD == lifecycle);
}

static void fusion_update_incidents(uint32_t now, multimodal_fusion_snapshot_t * p_snapshot)
{
    bool pressure_valid = 0U != (p_snapshot->valid_mask & MULTIMODAL_SOURCE_PRESSURE);
    bool vision_valid = 0U != (p_snapshot->valid_mask & MULTIMODAL_SOURCE_CAMERA);
    bool thermal_valid = 0U != (p_snapshot->valid_mask & MULTIMODAL_SOURCE_THERMAL);
    bool environment_valid = 0U != (p_snapshot->valid_mask & MULTIMODAL_SOURCE_ENVIRONMENT);
    uint32_t positive_sources = p_snapshot->presence_source_count;
    uint32_t negative_sources = 0U;
    uint32_t presence_source_mask = 0U;
    bool airway_subject_supported = vision_valid &&
                                    g_fusion_runtime.occlusion_valid &&
                                    (MULTIMODAL_PRESENCE_PRESENT == p_snapshot->presence) &&
                                    (0U != p_snapshot->infant_present);
    bool camera_fault = 0U != (p_snapshot->sensor_fault_mask & MULTIMODAL_FAULT_CAMERA);
    bool edge_near = MULTIMODAL_EDGE_NEAR == p_snapshot->edge_state;
    bool edge_climbing = MULTIMODAL_EDGE_GRAB == p_snapshot->edge_state;
    bool outside = MULTIMODAL_EDGE_OUTSIDE == p_snapshot->edge_state;
    bool cry = MULTIMODAL_DISTRESS_CRYING == p_snapshot->distress ||
               MULTIMODAL_DISTRESS_PERSISTENT_CRY == p_snapshot->distress;
    bool persistent_cry = MULTIMODAL_DISTRESS_PERSISTENT_CRY == p_snapshot->distress;
    bool subject_may_be_present = (MULTIMODAL_PRESENCE_PRESENT == p_snapshot->presence) ||
                                  (MULTIMODAL_PRESENCE_UNCERTAIN == p_snapshot->presence);
    bool subject_present = MULTIMODAL_PRESENCE_PRESENT == p_snapshot->presence;
    bool overheat = subject_may_be_present &&
                    (MULTIMODAL_TEMPERATURE_HIGH == p_snapshot->bed_thermal);
    bool underheat = subject_may_be_present &&
                     (MULTIMODAL_TEMPERATURE_LOW == p_snapshot->bed_thermal);
    bool extreme_heat = overheat && (p_snapshot->bed_max_milli_c >= FUSION_EXTREME_HEAT_MILLI_C) &&
                        (MULTIMODAL_PRESENCE_PRESENT == p_snapshot->presence);
    bool extreme_cold = underheat && (p_snapshot->bed_min_milli_c <= FUSION_EXTREME_COLD_MILLI_C) &&
                         (MULTIMODAL_PRESENCE_PRESENT == p_snapshot->presence);
    bool bme_gas_valid = g_fusion_runtime.environment_bme_gas_valid &&
                         fusion_tick_fresh(now,
                                           g_fusion_runtime.environment_bme_good_tick,
                                           FUSION_ENVIRONMENT_STALE_MS);
    bool environment_gate = subject_present && environment_valid;
    uint32_t gas_hysteresis = p_snapshot->environment_thresholds.gas_min_ohm / 10U;
    bool environment_co2_high;
    bool environment_temperature_high;
    bool environment_humidity_high;
    bool environment_humidity_low;
    bool environment_voc_suspected;
    bool suspected_fever = (MULTIMODAL_TEMPERATURE_HIGH == p_snapshot->body_temperature);
    uint32_t fever_confidence = suspected_fever ? fusion_clamp_confidence(
        80U + (uint32_t) (p_snapshot->thermal_hotspot_mean_cdeg -
                          p_snapshot->environment_thresholds.body_temperature_max_deci_c)) : 0U;
    uint32_t camera_age = fusion_age_ms(now, g_fusion_runtime.vision_update_tick);
    uint32_t airway_confidence = vision_valid ?
                                  fusion_clamp_confidence((p_snapshot->airway_risk_x1000 + 5U) / 10U) : 0U;

    if (gas_hysteresis < 1000U)
    {
        gas_hysteresis = 1000U;
    }
    environment_co2_high = environment_gate &&
        (p_snapshot->environment_co2_ppm >
         (fusion_incident_latched(MULTIMODAL_INCIDENT_ENVIRONMENT_CO2_HIGH) ?
          (p_snapshot->environment_thresholds.co2_max_ppm - FUSION_ENV_CO2_HYSTERESIS_PPM) :
          p_snapshot->environment_thresholds.co2_max_ppm));
    environment_temperature_high = environment_gate &&
        (p_snapshot->environment_temperature_milli_c >
         (fusion_incident_latched(MULTIMODAL_INCIDENT_ENVIRONMENT_TEMPERATURE_HIGH) ?
          (p_snapshot->environment_thresholds.temperature_max_milli_c - FUSION_ENV_TEMP_HYSTERESIS_MILLI_C) :
          p_snapshot->environment_thresholds.temperature_max_milli_c));
    environment_humidity_high = environment_gate &&
        (p_snapshot->environment_humidity_milli_percent >
         (fusion_incident_latched(MULTIMODAL_INCIDENT_ENVIRONMENT_HUMIDITY_HIGH) ?
          (p_snapshot->environment_thresholds.humidity_max_milli_percent - FUSION_ENV_RH_HYSTERESIS_MILLI_PCT) :
          p_snapshot->environment_thresholds.humidity_max_milli_percent));
    environment_humidity_low = environment_gate &&
        (p_snapshot->environment_humidity_milli_percent <
         (fusion_incident_latched(MULTIMODAL_INCIDENT_ENVIRONMENT_HUMIDITY_LOW) ?
          (p_snapshot->environment_thresholds.humidity_min_milli_percent + FUSION_ENV_RH_HYSTERESIS_MILLI_PCT) :
          p_snapshot->environment_thresholds.humidity_min_milli_percent));
    environment_voc_suspected = environment_gate && bme_gas_valid &&
        (g_fusion_runtime.environment_bme_gas_ohm <
         (fusion_incident_latched(MULTIMODAL_INCIDENT_ENVIRONMENT_VOC_SUSPECTED) ?
          (p_snapshot->environment_thresholds.gas_min_ohm + gas_hysteresis) :
          p_snapshot->environment_thresholds.gas_min_ohm));

    if (!airway_subject_supported)
    {
        fusion_set_airway_frame(MULTIMODAL_AIRWAY_UNKNOWN, 0U);
        p_snapshot->airway_view = MULTIMODAL_AIRWAY_UNKNOWN;
        p_snapshot->airway_risk_x1000 = 0U;
        airway_confidence = 0U;
    }

    if (pressure_valid)
    {
        presence_source_mask |= MULTIMODAL_SOURCE_PRESSURE;
        negative_sources += (p_snapshot->pressure_presence_x1000 <= FUSION_PRESENCE_NEGATIVE_X1000) ? 1U : 0U;
    }
    if (vision_valid)
    {
        presence_source_mask |= MULTIMODAL_SOURCE_CAMERA;
        negative_sources += (p_snapshot->vision_presence_x1000 <= FUSION_PRESENCE_NEGATIVE_X1000) ? 1U : 0U;
    }
    if (thermal_valid)
    {
        presence_source_mask |= MULTIMODAL_SOURCE_THERMAL;
        negative_sources += (p_snapshot->thermal_presence_x1000 <= FUSION_PRESENCE_NEGATIVE_X1000) ? 1U : 0U;
    }

    fusion_update_incident(MULTIMODAL_INCIDENT_PRESENCE_CONFLICT,
                           (positive_sources > 0U) && (negative_sources > 0U), 1U, 1U, 60U,
                           presence_source_mask,
                           5000U, 10000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_SENSOR_DEGRADED,
                           0U != p_snapshot->sensor_fault_mask, 1U, 1U, 100U,
                           p_snapshot->valid_mask, 5000U, 5000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_MONITOR_BLIND,
                           MULTIMODAL_HEALTH_BLIND == p_snapshot->system_health, 2U, 3U, 100U,
                           p_snapshot->valid_mask, 10000U, 10000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_AIRWAY_NOT_VISIBLE,
                           airway_subject_supported &&
                           (positive_sources >= 2U) &&
                           (g_fusion_runtime.not_visible_frames >= FUSION_AIRWAY_NOT_VISIBLE_FRAMES),
                           2U, 3U,
                           airway_confidence,
                           MULTIMODAL_SOURCE_CAMERA | presence_source_mask, 1000U, 5000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_AIRWAY_PARTIAL,
                           airway_subject_supported &&
                           (p_snapshot->airway_risk_x1000 >= FUSION_VISION_PARTIAL_X1000) &&
                           (g_fusion_runtime.partial_frames >= FUSION_AIRWAY_PARTIAL_FRAMES),
                           2U, 3U,
                           airway_confidence,
                           MULTIMODAL_SOURCE_CAMERA | presence_source_mask, 1000U, 5000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_AIRWAY_COVERED,
                           airway_subject_supported &&
                           (p_snapshot->airway_risk_x1000 >= 700U) &&
                           (g_fusion_runtime.covered_frames >= FUSION_AIRWAY_COVERED_FRAMES),
                           3U, 5U,
                           airway_confidence,
                           MULTIMODAL_SOURCE_CAMERA | presence_source_mask, 500U, 10000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_EDGE_NEAR,
                           edge_near, 2U, 3U,
                           fusion_clamp_confidence(p_snapshot->edge_risk_x1000 / 10U),
                           MULTIMODAL_SOURCE_PRESSURE | MULTIMODAL_SOURCE_CAMERA,
                           2000U, 3000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_EDGE_CLIMBING,
                           edge_climbing, 3U, 5U,
                           p_snapshot->standing_confidence,
                           MULTIMODAL_SOURCE_PRESSURE | MULTIMODAL_SOURCE_CAMERA,
                           500U, 5000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_OUT_OF_BED,
                           outside, 3U, 4U, 90U,
                           presence_source_mask,
                           1000U, 5000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_CRY,
                           cry, 1U, 1U, p_snapshot->cry_confidence,
                           MULTIMODAL_SOURCE_MIC, 0U, 3000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_PERSISTENT_CRY,
                           persistent_cry, 2U, 3U, p_snapshot->cry_confidence,
                           MULTIMODAL_SOURCE_MIC, 0U, 10000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_OVERHEAT,
                           overheat, extreme_heat ? 3U : 2U, extreme_heat ? 4U : 3U, 90U,
                           MULTIMODAL_SOURCE_TEMPERATURE,
                           30000U, 30000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_UNDERHEAT,
                           underheat, extreme_cold ? 3U : 2U, extreme_cold ? 4U : 3U, 90U,
                           MULTIMODAL_SOURCE_TEMPERATURE,
                           30000U, 30000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_CAMERA_FROZEN,
                           camera_fault && (camera_age >= FUSION_CAMERA_FREEZE_MS),
                           (camera_age >= 30000U) ? 2U : 1U,
                           (camera_age >= 30000U) ? 2U : 1U, 100U,
                           MULTIMODAL_SOURCE_CAMERA, 0U, 10000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_ENVIRONMENT_AIR,
                           false, 2U, 3U, 0U,
                           MULTIMODAL_SOURCE_ENVIRONMENT, 10000U, 30000U, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_SUSPECTED_FEVER,
                           suspected_fever, 2U, 3U, fever_confidence,
                           MULTIMODAL_SOURCE_THERMAL | presence_source_mask,
                           FUSION_BODY_FEVER_CONFIRM_MS, FUSION_BODY_FEVER_RECOVERY_MS, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_ENVIRONMENT_CO2_HIGH,
                           environment_co2_high, 2U, 3U, environment_co2_high ? 90U : 0U,
                           MULTIMODAL_SOURCE_ENVIRONMENT,
                           FUSION_ENV_CONFIRM_MS, FUSION_ENV_RECOVERY_MS, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_ENVIRONMENT_TEMPERATURE_HIGH,
                           environment_temperature_high, 2U, 3U,
                           environment_temperature_high ? 90U : 0U,
                           MULTIMODAL_SOURCE_ENVIRONMENT,
                           FUSION_ENV_CONFIRM_MS, FUSION_ENV_RECOVERY_MS, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_ENVIRONMENT_HUMIDITY_HIGH,
                           environment_humidity_high, 2U, 3U,
                           environment_humidity_high ? 90U : 0U,
                           MULTIMODAL_SOURCE_ENVIRONMENT,
                           FUSION_ENV_CONFIRM_MS, FUSION_ENV_RECOVERY_MS, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_ENVIRONMENT_HUMIDITY_LOW,
                           environment_humidity_low, 2U, 3U,
                           environment_humidity_low ? 90U : 0U,
                           MULTIMODAL_SOURCE_ENVIRONMENT,
                           FUSION_ENV_CONFIRM_MS, FUSION_ENV_RECOVERY_MS, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_ENVIRONMENT_VOC_SUSPECTED,
                           environment_voc_suspected, 2U, 3U,
                           environment_voc_suspected ? 90U : 0U,
                           MULTIMODAL_SOURCE_ENVIRONMENT,
                           FUSION_ENV_CONFIRM_MS, FUSION_ENV_RECOVERY_MS, now);
    fusion_update_incident(MULTIMODAL_INCIDENT_VISUAL_SUBJECT_HIDDEN,
                           subject_present && pressure_valid && thermal_valid && vision_valid &&
                           (positive_sources >= 2U) &&
                           (g_fusion_runtime.visual_hidden_frames >= FUSION_VISUAL_HIDDEN_FRAMES),
                           2U, 3U, 75U,
                           MULTIMODAL_SOURCE_CAMERA | MULTIMODAL_SOURCE_PRESSURE |
                           MULTIMODAL_SOURCE_THERMAL,
                           1000U, 5000U, now);
}

static bool fusion_incident_visible(fusion_incident_runtime_t const * p_incident)
{
    return (MULTIMODAL_INCIDENT_ACTIVE == p_incident->lifecycle) ||
           (MULTIMODAL_INCIDENT_ESCALATED == p_incident->lifecycle) ||
           (MULTIMODAL_INCIDENT_RECOVERY_HOLD == p_incident->lifecycle);
}

static multimodal_state_t fusion_incident_legacy_state(multimodal_incident_type_t type)
{
    switch (type)
    {
        case MULTIMODAL_INCIDENT_EDGE_NEAR:            return MULTIMODAL_STATE_NEAR_EDGE;
        case MULTIMODAL_INCIDENT_EDGE_CLIMBING:        return MULTIMODAL_STATE_STANDING;
        case MULTIMODAL_INCIDENT_OUT_OF_BED:           return MULTIMODAL_STATE_OUT_OF_BED;
        case MULTIMODAL_INCIDENT_OVERHEAT:             return MULTIMODAL_STATE_OVERHEAT;
        case MULTIMODAL_INCIDENT_UNDERHEAT:            return MULTIMODAL_STATE_UNDERHEAT;
        case MULTIMODAL_INCIDENT_CRY:
        case MULTIMODAL_INCIDENT_PERSISTENT_CRY:       return MULTIMODAL_STATE_CRYING;
        case MULTIMODAL_INCIDENT_AIRWAY_NOT_VISIBLE:
        case MULTIMODAL_INCIDENT_AIRWAY_PARTIAL:
        case MULTIMODAL_INCIDENT_AIRWAY_COVERED:       return MULTIMODAL_STATE_FACE_COVERED;
        case MULTIMODAL_INCIDENT_VISUAL_SUBJECT_HIDDEN:return MULTIMODAL_STATE_FACE_COVERED;
        case MULTIMODAL_INCIDENT_PRESENCE_CONFLICT:
        case MULTIMODAL_INCIDENT_SENSOR_DEGRADED:
        case MULTIMODAL_INCIDENT_MONITOR_BLIND:
        case MULTIMODAL_INCIDENT_CAMERA_FROZEN:        return MULTIMODAL_STATE_SENSOR_FAULT;
        case MULTIMODAL_INCIDENT_ENVIRONMENT_AIR:
        case MULTIMODAL_INCIDENT_ENVIRONMENT_CO2_HIGH:
        case MULTIMODAL_INCIDENT_ENVIRONMENT_TEMPERATURE_HIGH:
        case MULTIMODAL_INCIDENT_ENVIRONMENT_HUMIDITY_HIGH:
        case MULTIMODAL_INCIDENT_ENVIRONMENT_HUMIDITY_LOW:
        case MULTIMODAL_INCIDENT_ENVIRONMENT_VOC_SUSPECTED:
                                                        return MULTIMODAL_STATE_AIR_QUALITY;
        case MULTIMODAL_INCIDENT_SUSPECTED_FEVER:      return MULTIMODAL_STATE_SUSPECTED_FEVER;
        default:                                       return MULTIMODAL_STATE_NORMAL;
    }
}

static void fusion_build_decision(multimodal_fusion_snapshot_t * p_snapshot)
{
    fusion_incident_runtime_t const * p_primary = NULL;
    multimodal_incident_type_t primary_type = MULTIMODAL_INCIDENT_NONE;
    multimodal_incident_type_t candidate_type = MULTIMODAL_INCIDENT_NONE;
    uint32_t visible_count = 0U;

    memset(p_snapshot->active_incidents, 0, sizeof(p_snapshot->active_incidents));
    p_snapshot->active_type_mask = 0U;
    p_snapshot->global_alarm_level = 0U;

    for (uint32_t type = 1U; type < MULTIMODAL_INCIDENT_TYPE_COUNT; type++)
    {
        fusion_incident_runtime_t const * p_incident = &g_fusion_runtime.incidents[type];
        if ((MULTIMODAL_INCIDENT_CANDIDATE == p_incident->lifecycle) &&
            (MULTIMODAL_INCIDENT_NONE == candidate_type))
        {
            candidate_type = (multimodal_incident_type_t) type;
        }
        if (!fusion_incident_visible(p_incident))
        {
            continue;
        }

        p_snapshot->active_type_mask |= 1UL << type;
        if (p_incident->level > p_snapshot->global_alarm_level)
        {
            p_snapshot->global_alarm_level = p_incident->level;
        }
        if ((NULL == p_primary) ||
            (p_incident->severity > p_primary->severity) ||
            ((p_incident->severity == p_primary->severity) &&
             (p_incident->opened_tick < p_primary->opened_tick)))
        {
            p_primary = p_incident;
            primary_type = (multimodal_incident_type_t) type;
        }

        if (visible_count < MULTIMODAL_FUSION_MAX_ACTIVE_INCIDENTS)
        {
            multimodal_incident_summary_t * p_summary = &p_snapshot->active_incidents[visible_count++];
            p_summary->type = type;
            p_summary->lifecycle = p_incident->lifecycle;
            p_summary->level = p_incident->level;
            p_summary->severity = p_incident->severity;
            p_summary->confidence = p_incident->confidence;
            p_summary->source_mask = p_incident->source_mask;
            p_summary->incident_id = p_incident->incident_id;
            p_summary->since_tick = p_incident->lifecycle_since_tick;
        }
    }

    p_snapshot->active_incident_count = visible_count;
    p_snapshot->primary_incident_type = primary_type;
    if (NULL != p_primary)
    {
        p_snapshot->primary_incident_id = p_primary->incident_id;
        p_snapshot->primary_incident_lifecycle = p_primary->lifecycle;
        p_snapshot->confidence = p_primary->confidence;
        p_snapshot->severity = p_primary->severity;
        p_snapshot->source_mask = p_primary->source_mask;
        p_snapshot->state = fusion_incident_legacy_state(primary_type);
    }
    else
    {
        p_snapshot->confidence = 100U;
        p_snapshot->severity = 0U;
        p_snapshot->source_mask = 0U;
        if (MULTIMODAL_AROUSAL_SLEEPING == p_snapshot->arousal)
        {
            p_snapshot->state = MULTIMODAL_STATE_SLEEPING;
        }
        else if (MULTIMODAL_AROUSAL_AWAKE == p_snapshot->arousal)
        {
            p_snapshot->state = MULTIMODAL_STATE_AWAKE;
        }
        else if ((MULTIMODAL_BED_MOTION_ACTIVE == p_snapshot->bed_motion) ||
                 (MULTIMODAL_BED_MOTION_ROLLING == p_snapshot->bed_motion))
        {
            p_snapshot->state = MULTIMODAL_STATE_MOVING;
        }
        else
        {
            p_snapshot->state = MULTIMODAL_STATE_NORMAL;
        }
    }

    p_snapshot->candidate_state = (MULTIMODAL_INCIDENT_NONE == candidate_type) ?
                                  p_snapshot->state : fusion_incident_legacy_state(candidate_type);
    if ((multimodal_state_t) p_snapshot->state != g_fusion_runtime.state)
    {
        g_fusion_runtime.state = (multimodal_state_t) p_snapshot->state;
        g_fusion_runtime.state_since_tick = p_snapshot->tick;
    }
    p_snapshot->state_since_tick = g_fusion_runtime.state_since_tick;
}

static bool fusion_has_active_incident(multimodal_fusion_snapshot_t const * p_snapshot,
                                       multimodal_incident_type_t type)
{
    return 0U != (p_snapshot->active_type_mask & (1UL << (uint32_t) type));
}

static void fusion_build_comfort_recommendation(multimodal_fusion_snapshot_t * p_snapshot)
{
    bool present = (MULTIMODAL_PRESENCE_PRESENT == p_snapshot->presence) ||
                   (MULTIMODAL_PRESENCE_UNCERTAIN == p_snapshot->presence);
    bool cry = fusion_has_active_incident(p_snapshot, MULTIMODAL_INCIDENT_CRY);
    bool persistent_cry = fusion_has_active_incident(p_snapshot, MULTIMODAL_INCIDENT_PERSISTENT_CRY);
    bool blind = MULTIMODAL_HEALTH_BLIND == p_snapshot->system_health;
    bool danger = p_snapshot->global_alarm_level >= 2U;

    p_snapshot->comfort_action_mask = 0U;
    p_snapshot->comfort_reason_mask = 0U;
    p_snapshot->comfort_confidence = 0U;
    p_snapshot->comfort_requires_confirmation = 0U;
    p_snapshot->comfort_block_mask = 0U;

    if (!present)
    {
        p_snapshot->comfort_block_mask |= MULTIMODAL_COMFORT_BLOCK_PAT_NO_PRESENCE;
    }
    if (blind)
    {
        p_snapshot->comfort_block_mask |= MULTIMODAL_COMFORT_BLOCK_PAT_BLIND;
    }
    if (danger)
    {
        p_snapshot->comfort_block_mask |= MULTIMODAL_COMFORT_BLOCK_PAT_DANGER;
    }

    if (blind)
    {
        p_snapshot->comfort_action_mask = MULTIMODAL_COMFORT_ACTION_LIGHT_ON |
                                          MULTIMODAL_COMFORT_ACTION_CAREGIVER_CHECK;
        p_snapshot->comfort_reason_mask = MULTIMODAL_COMFORT_REASON_SENSOR_BLIND;
        p_snapshot->comfort_confidence = 100U;
    }
    else if (danger)
    {
        p_snapshot->comfort_action_mask = MULTIMODAL_COMFORT_ACTION_LIGHT_ON |
                                          MULTIMODAL_COMFORT_ACTION_CAREGIVER_CHECK;
        p_snapshot->comfort_reason_mask = MULTIMODAL_COMFORT_REASON_SAFETY_EVENT;
        if (persistent_cry)
        {
            p_snapshot->comfort_reason_mask |= MULTIMODAL_COMFORT_REASON_PERSISTENT_CRY;
        }
        p_snapshot->comfort_confidence = fusion_clamp_confidence(p_snapshot->confidence);
    }
    else if (cry && present)
    {
        p_snapshot->comfort_action_mask = MULTIMODAL_COMFORT_ACTION_PAT;
        p_snapshot->comfort_reason_mask = MULTIMODAL_COMFORT_REASON_CRY;
        if (MULTIMODAL_AROUSAL_AWAKE == p_snapshot->arousal)
        {
            p_snapshot->comfort_action_mask |= MULTIMODAL_COMFORT_ACTION_LIGHT_ON;
        }
        p_snapshot->comfort_confidence = fusion_clamp_confidence(p_snapshot->cry_confidence);
    }
    else if ((0U == p_snapshot->active_incident_count) &&
             (MULTIMODAL_AROUSAL_SLEEPING == p_snapshot->arousal) && present)
    {
        p_snapshot->comfort_action_mask = MULTIMODAL_COMFORT_ACTION_LIGHT_OFF;
        p_snapshot->comfort_reason_mask = MULTIMODAL_COMFORT_REASON_SLEEPING;
        p_snapshot->comfort_confidence = fusion_clamp_confidence(p_snapshot->sleep_confidence);
    }
    else if ((0U == p_snapshot->active_incident_count) &&
             (MULTIMODAL_PRESENCE_ABSENT == p_snapshot->presence))
    {
        p_snapshot->comfort_action_mask = MULTIMODAL_COMFORT_ACTION_LIGHT_OFF;
        p_snapshot->comfort_reason_mask = MULTIMODAL_COMFORT_REASON_BABY_ABSENT;
        p_snapshot->comfort_confidence = 100U;
    }

    p_snapshot->comfort_requires_confirmation =
        (0U != p_snapshot->comfort_action_mask) ? 1U : 0U;
}

static void fusion_publish(multimodal_fusion_snapshot_t * p_snapshot)
{
    p_snapshot->sequence = g_fusion_snapshot.sequence + 1U;
    p_snapshot->evidence_sequence = p_snapshot->sequence;
    p_snapshot->schema_version = MULTIMODAL_FUSION_SCHEMA_VERSION;
    p_snapshot->incident_event_drop_count = g_fusion_runtime.incident_event_drop_count;

    /*
     * Match the vision snapshot publisher: keep this short copy atomic with
     * respect to task preemption so a higher-priority reader cannot strand the
     * sequence lock in its odd (writer-active) state.
     */
    taskENTER_CRITICAL();
    g_fusion_publish_lock++;
    __DMB();
    memcpy(&g_fusion_snapshot, p_snapshot, sizeof(*p_snapshot));
    __DMB();
    g_fusion_publish_lock++;
    taskEXIT_CRITICAL();
}

void fusion_thread_entry(void * pvParameters)
{
    multimodal_fusion_snapshot_t snapshot;

    FSP_PARAMETER_NOT_USED(pvParameters);
    memset(&g_fusion_runtime, 0, sizeof(g_fusion_runtime));
    memset(&g_fusion_models, 0, sizeof(g_fusion_models));
    memset(&g_fusion_snapshot, 0, sizeof(g_fusion_snapshot));
    memset(g_fusion_event_queue, 0, sizeof(g_fusion_event_queue));
    g_fusion_event_head = 0U;
    g_fusion_event_tail = 0U;
    g_fusion_runtime.boot_tick = (uint32_t) xTaskGetTickCount();
    g_fusion_runtime.state_since_tick = g_fusion_runtime.boot_tick;
    g_fusion_runtime.state = MULTIMODAL_STATE_NORMAL;

    while (1)
    {
        uint32_t now = (uint32_t) xTaskGetTickCount();
        if (g_fusion_reset_pressure)
        {
            g_fusion_reset_pressure = false;
            memset(g_fusion_runtime.pressure_baseline, 0, sizeof(g_fusion_runtime.pressure_baseline));
            g_fusion_runtime.pressure_frames = 0U;
            g_fusion_runtime.pressure_processed_sequence = 0U;
            g_fusion_runtime.pressure_total = 0U;
            g_fusion_runtime.pressure_edge = 0U;
            g_fusion_runtime.pressure_peak = 0U;
            g_fusion_runtime.pressure_active_points = 0U;
            g_fusion_runtime.pressure_edge_ratio_x1000 = 0U;
            g_fusion_runtime.pressure_activity_x1000 = 0U;
            g_fusion_runtime.pressure_x100 = 0U;
            g_fusion_runtime.pressure_y100 = 0U;
            g_fusion_runtime.occupied = false;
            g_fusion_runtime.occupied_seen = false;
            g_fusion_runtime.pressure_confirm_frames = 0U;
            g_fusion_runtime.pressure_release_frames = 0U;
        }

        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.tick = now;
        snapshot.vision_age_ms = UINT32_MAX;

        fusion_track_sensor_sequences(now);
        fusion_update_environment(&snapshot);
        fusion_update_pressure(&snapshot);
        fusion_update_thermal_features(&snapshot);
        fusion_update_temperature(&snapshot);
        fusion_update_vision(now, &snapshot);
        fusion_update_health(now, &snapshot);
        fusion_update_context(now, &snapshot);
        fusion_update_incidents(now, &snapshot);
        fusion_build_decision(&snapshot);
        fusion_build_comfort_recommendation(&snapshot);
        fusion_publish(&snapshot);

        vTaskDelay(pdMS_TO_TICKS(FUSION_PERIOD_MS));
    }
}

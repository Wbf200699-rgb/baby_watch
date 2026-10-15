#ifndef MULTIMODAL_FUSION_H
#define MULTIMODAL_FUSION_H

#include <stdbool.h>
#include <stdint.h>

#define MULTIMODAL_FUSION_SCHEMA_VERSION       (9U)
#define MULTIMODAL_FUSION_RULE_VERSION         (0x20260815U)
#define MULTIMODAL_FUSION_MAX_ACTIVE_INCIDENTS (8U)

typedef enum e_multimodal_state
{
    MULTIMODAL_STATE_NORMAL = 0,
    MULTIMODAL_STATE_NEAR_EDGE = 1,
    MULTIMODAL_STATE_OUT_OF_BED = 2,
    MULTIMODAL_STATE_OVERHEAT = 3,
    MULTIMODAL_STATE_UNDERHEAT = 4,
    MULTIMODAL_STATE_SENSOR_FAULT = 5,
    MULTIMODAL_STATE_CRYING = 6,
    MULTIMODAL_STATE_FACE_COVERED = 7,
    MULTIMODAL_STATE_STANDING = 8,
    MULTIMODAL_STATE_MOVING = 9,
    MULTIMODAL_STATE_SLEEPING = 10,
    MULTIMODAL_STATE_AWAKE = 11,
    MULTIMODAL_STATE_AIR_QUALITY = 12,
    MULTIMODAL_STATE_SUSPECTED_FEVER = 13
} multimodal_state_t;

typedef enum e_multimodal_presence
{
    MULTIMODAL_PRESENCE_UNKNOWN = 0,
    MULTIMODAL_PRESENCE_PRESENT,
    MULTIMODAL_PRESENCE_ABSENT,
    MULTIMODAL_PRESENCE_UNCERTAIN
} multimodal_presence_t;

typedef enum e_multimodal_arousal
{
    MULTIMODAL_AROUSAL_UNKNOWN = 0,
    MULTIMODAL_AROUSAL_SLEEPING,
    MULTIMODAL_AROUSAL_AWAKE,
    MULTIMODAL_AROUSAL_TRANSITION
} multimodal_arousal_t;

typedef enum e_multimodal_bed_motion
{
    MULTIMODAL_BED_MOTION_UNKNOWN = 0,
    MULTIMODAL_BED_MOTION_QUIET,
    MULTIMODAL_BED_MOTION_ROLLING,
    MULTIMODAL_BED_MOTION_ACTIVE
} multimodal_bed_motion_t;

typedef enum e_multimodal_airway_view
{
    MULTIMODAL_AIRWAY_UNKNOWN = 0,
    MULTIMODAL_AIRWAY_CLEAR,
    MULTIMODAL_AIRWAY_PARTIAL,
    MULTIMODAL_AIRWAY_COVERED,
    MULTIMODAL_AIRWAY_NOT_VISIBLE
} multimodal_airway_view_t;

typedef enum e_multimodal_edge_state
{
    MULTIMODAL_EDGE_UNKNOWN = 0,
    MULTIMODAL_EDGE_CENTER,
    MULTIMODAL_EDGE_NEAR,
    MULTIMODAL_EDGE_GRAB,
    MULTIMODAL_EDGE_OUTSIDE
} multimodal_edge_state_t;

typedef enum e_multimodal_distress
{
    MULTIMODAL_DISTRESS_UNKNOWN = 0,
    MULTIMODAL_DISTRESS_NONE,
    MULTIMODAL_DISTRESS_CRYING,
    MULTIMODAL_DISTRESS_PERSISTENT_CRY
} multimodal_distress_t;

typedef enum e_multimodal_temperature_state
{
    MULTIMODAL_TEMPERATURE_UNKNOWN = 0,
    MULTIMODAL_TEMPERATURE_NORMAL,
    MULTIMODAL_TEMPERATURE_HIGH,
    MULTIMODAL_TEMPERATURE_LOW
} multimodal_temperature_state_t;

typedef enum e_multimodal_presence_context
{
    MULTIMODAL_CONTEXT_UNKNOWN = 0,
    MULTIMODAL_CONTEXT_ABSENT,
    MULTIMODAL_CONTEXT_PRESENT
} multimodal_presence_context_t;

typedef enum e_multimodal_system_health
{
    MULTIMODAL_HEALTH_OK = 0,
    MULTIMODAL_HEALTH_DEGRADED,
    MULTIMODAL_HEALTH_BLIND
} multimodal_system_health_t;

typedef enum e_multimodal_incident_type
{
    MULTIMODAL_INCIDENT_NONE = 0,
    MULTIMODAL_INCIDENT_PRESENCE_CONFLICT,
    MULTIMODAL_INCIDENT_SENSOR_DEGRADED,
    MULTIMODAL_INCIDENT_MONITOR_BLIND,
    MULTIMODAL_INCIDENT_AIRWAY_NOT_VISIBLE,
    MULTIMODAL_INCIDENT_AIRWAY_PARTIAL,
    MULTIMODAL_INCIDENT_AIRWAY_COVERED,
    MULTIMODAL_INCIDENT_EDGE_NEAR,
    MULTIMODAL_INCIDENT_EDGE_CLIMBING,
    MULTIMODAL_INCIDENT_OUT_OF_BED,
    MULTIMODAL_INCIDENT_CRY,
    MULTIMODAL_INCIDENT_PERSISTENT_CRY,
    MULTIMODAL_INCIDENT_OVERHEAT,
    MULTIMODAL_INCIDENT_UNDERHEAT,
    MULTIMODAL_INCIDENT_CAMERA_FROZEN,
    MULTIMODAL_INCIDENT_ENVIRONMENT_AIR,
    MULTIMODAL_INCIDENT_SUSPECTED_FEVER,
    MULTIMODAL_INCIDENT_ENVIRONMENT_CO2_HIGH,
    MULTIMODAL_INCIDENT_ENVIRONMENT_TEMPERATURE_HIGH,
    MULTIMODAL_INCIDENT_ENVIRONMENT_HUMIDITY_HIGH,
    MULTIMODAL_INCIDENT_ENVIRONMENT_HUMIDITY_LOW,
    MULTIMODAL_INCIDENT_ENVIRONMENT_VOC_SUSPECTED,
    MULTIMODAL_INCIDENT_VISUAL_SUBJECT_HIDDEN,
    MULTIMODAL_INCIDENT_TYPE_COUNT
} multimodal_incident_type_t;

typedef enum e_multimodal_incident_lifecycle
{
    MULTIMODAL_INCIDENT_CLEAR = 0,
    MULTIMODAL_INCIDENT_CANDIDATE,
    MULTIMODAL_INCIDENT_ACTIVE,
    MULTIMODAL_INCIDENT_ESCALATED,
    MULTIMODAL_INCIDENT_RECOVERY_HOLD,
    MULTIMODAL_INCIDENT_RESOLVED
} multimodal_incident_lifecycle_t;

typedef enum e_multimodal_incident_action
{
    MULTIMODAL_INCIDENT_ACTION_NONE = 0,
    MULTIMODAL_INCIDENT_ACTION_OPEN,
    MULTIMODAL_INCIDENT_ACTION_UPDATE,
    MULTIMODAL_INCIDENT_ACTION_ESCALATE,
    MULTIMODAL_INCIDENT_ACTION_RESOLVE
} multimodal_incident_action_t;

enum
{
    MULTIMODAL_SOURCE_MIC = 1U << 0,
    MULTIMODAL_SOURCE_THERMAL = 1U << 1,
    MULTIMODAL_SOURCE_TEMPERATURE = 1U << 2,
    MULTIMODAL_SOURCE_PRESSURE = 1U << 3,
    MULTIMODAL_SOURCE_CAMERA = 1U << 4,
    MULTIMODAL_SOURCE_ENVIRONMENT = 1U << 5
};

enum
{
    MULTIMODAL_FAULT_THERMAL = 1U << 0,
    MULTIMODAL_FAULT_TEMPERATURE = 1U << 1,
    MULTIMODAL_FAULT_PRESSURE = 1U << 2,
    MULTIMODAL_FAULT_MIC = 1U << 3,
    MULTIMODAL_FAULT_CAMERA = 1U << 4,
    MULTIMODAL_FAULT_ENVIRONMENT = 1U << 5
};

/*
 * Comfort actions are recommendations only.  A user-facing client must ask
 * for confirmation before sending the corresponding actuator command.
 */
enum
{
    MULTIMODAL_COMFORT_ACTION_LIGHT_OFF       = 1U << 0,
    MULTIMODAL_COMFORT_ACTION_LIGHT_ON        = 1U << 1,
    MULTIMODAL_COMFORT_ACTION_PAT             = 1U << 2,
    MULTIMODAL_COMFORT_ACTION_CAREGIVER_CHECK = 1U << 3
};

enum
{
    MULTIMODAL_COMFORT_REASON_SLEEPING       = 1U << 0,
    MULTIMODAL_COMFORT_REASON_CRY            = 1U << 1,
    MULTIMODAL_COMFORT_REASON_PERSISTENT_CRY = 1U << 2,
    MULTIMODAL_COMFORT_REASON_SAFETY_EVENT   = 1U << 3,
    MULTIMODAL_COMFORT_REASON_SENSOR_BLIND   = 1U << 4,
    MULTIMODAL_COMFORT_REASON_BABY_ABSENT    = 1U << 5
};

enum
{
    MULTIMODAL_COMFORT_BLOCK_PAT_DANGER      = 1U << 0,
    MULTIMODAL_COMFORT_BLOCK_PAT_NO_PRESENCE = 1U << 1,
    MULTIMODAL_COMFORT_BLOCK_PAT_BLIND       = 1U << 2
};

typedef struct st_multimodal_incident_summary
{
    uint32_t type;
    uint32_t lifecycle;
    uint32_t level;
    uint32_t severity;
    uint32_t confidence;
    uint32_t source_mask;
    uint32_t incident_id;
    uint32_t since_tick;
} multimodal_incident_summary_t;

typedef struct st_multimodal_incident_event
{
    uint32_t event_id;
    uint32_t incident_id;
    uint32_t type;
    uint32_t action;
    uint32_t lifecycle;
    uint32_t level;
    uint32_t severity;
    uint32_t confidence;
    uint32_t source_mask;
    uint32_t occurred_tick;
} multimodal_incident_event_t;

typedef struct st_multimodal_environment_thresholds
{
    uint32_t co2_max_ppm;
    int32_t temperature_max_milli_c;
    uint32_t humidity_max_milli_percent;
    uint32_t humidity_min_milli_percent;
    uint32_t gas_min_ohm;
    int32_t body_temperature_max_deci_c;
    uint32_t revision;
} multimodal_environment_thresholds_t;

typedef struct st_multimodal_fusion_snapshot
{
    /* Legacy projection retained for existing RA8-S3 clients. */
    uint32_t sequence;
    uint32_t tick;
    uint32_t state_since_tick;
    uint32_t state;
    uint32_t candidate_state;
    uint32_t confidence;
    uint32_t severity;
    uint32_t source_mask;
    uint32_t valid_mask;
    uint32_t sensor_fault_mask;
    uint32_t pressure_total;
    uint32_t pressure_edge;
    uint32_t pressure_x100;
    uint32_t pressure_y100;
    uint32_t occupied;
    int32_t thermal_min_cdeg;
    int32_t thermal_max_cdeg;
    int32_t bed_min_milli_c;
    int32_t bed_max_milli_c;
    uint32_t cry_confidence;
    uint32_t sleep_confidence;
    uint32_t face_covered_confidence;
    uint32_t standing_confidence;
    uint32_t movement_confidence;

    /* Schema v2 parallel context and decision output. */
    uint32_t schema_version;
    uint32_t evidence_sequence;
    uint32_t presence;
    uint32_t arousal;
    uint32_t bed_motion;
    uint32_t airway_view;
    uint32_t edge_state;
    uint32_t distress;
    uint32_t cry_cause_hint;
    uint32_t cry_cause_confidence;
    uint32_t body_temperature;
    uint32_t bed_thermal;
    uint32_t caregiver;
    uint32_t system_health;
    uint32_t global_alarm_level;
    uint32_t active_incident_count;
    uint32_t active_type_mask;
    uint32_t primary_incident_type;
    uint32_t primary_incident_id;
    uint32_t primary_incident_lifecycle;
    uint32_t pressure_edge_ratio_x1000;
    uint32_t pressure_activity_x1000;
    int32_t pressure_move_x100;
    int32_t pressure_move_y100;
    uint32_t vision_sequence;
    uint32_t vision_age_ms;
    uint32_t infant_present;
    uint32_t head_present;
    uint32_t infant_at_edge;
    uint32_t infant_edge_distance_x1000;
    uint32_t incident_event_drop_count;
    multimodal_incident_summary_t active_incidents[MULTIMODAL_FUSION_MAX_ACTIVE_INCIDENTS];

    /* Schema v3 ambient environment telemetry; never used as bed temperature. */
    uint32_t environment_co2_ppm;
    int32_t environment_temperature_milli_c;
    uint32_t environment_humidity_milli_percent;
    uint32_t environment_last_good_tick;

    /* Schema v4 M1616 low-load diagnostics. */
    uint32_t pressure_peak;
    uint32_t pressure_active_points;

    /* Schema v5 explainable evidence and physical bed-zone diagnostics. */
    uint32_t presence_score_x1000;
    uint32_t presence_source_count;
    uint32_t pressure_presence_x1000;
    uint32_t vision_presence_x1000;
    uint32_t thermal_presence_x1000;
    uint32_t activity_score_x1000;
    uint32_t airway_risk_x1000;
    uint32_t edge_risk_x1000;
    uint32_t thermal_hotspot_points;
    uint32_t thermal_hotspot_x1000;
    uint32_t thermal_hotspot_y1000;
    int32_t thermal_hotspot_mean_cdeg;
    int32_t bed_left_milli_c;
    int32_t bed_center_milli_c;
    int32_t bed_right_milli_c;
    int32_t bed_spread_milli_c;
    uint32_t bed_hotspot_region; /* 0=unknown, 1=left, 2=center, 3=right */

    /* Schema v6 dataset-calibrated cross-modal position diagnostics. */
    uint32_t thermal_bed_x1000;
    uint32_t thermal_edge_x1000;
    uint32_t position_agreement_x1000;
    uint32_t calibration_profile_version;

    /* Schema v7 explainable comfort recommendation; never auto-actuates. */
    uint32_t comfort_action_mask;
    uint32_t comfort_reason_mask;
    uint32_t comfort_confidence;
    uint32_t comfort_requires_confirmation;
    uint32_t comfort_block_mask;

    /* Schema v9 hardware-owned, server-synchronised warning thresholds. */
    multimodal_environment_thresholds_t environment_thresholds;
} multimodal_fusion_snapshot_t;

void multimodal_fusion_set_audio_result(uint32_t cry_confidence, uint32_t timestamp_tick);
void multimodal_fusion_set_vision_result(uint32_t sleep_confidence,
                                         uint32_t face_covered_confidence,
                                         uint32_t standing_confidence,
                                         uint32_t movement_confidence,
                                         uint32_t timestamp_tick);
void multimodal_fusion_reset_pressure_baseline(void);
bool multimodal_fusion_set_environment_thresholds(
    multimodal_environment_thresholds_t const * p_thresholds);
bool multimodal_fusion_get_environment_thresholds(
    multimodal_environment_thresholds_t * p_thresholds);
bool multimodal_fusion_get_snapshot(multimodal_fusion_snapshot_t * p_snapshot);
bool multimodal_fusion_peek_event(multimodal_incident_event_t * p_event);
bool multimodal_fusion_ack_event(uint32_t event_id);

#endif /* MULTIMODAL_FUSION_H */

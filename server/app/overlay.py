from __future__ import annotations

import json
import math
from datetime import datetime, timezone
from typing import Any


OVERLAY_SCHEMA_VERSION = 1
OVERLAY_STALE_MS = 12000
OVERLAY_EXPIRE_MS = 30000
IDENTITY_MATRIX = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
DEFAULT_POINTS = [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]]
DEFAULT_THERMAL_POINTS = [[0.25, 0.25], [0.75, 0.75]]

VISION_LABELS = ["infant", "head_face", "blanket", "soft_object", "caregiver"]
LEGACY_STATE_LABELS = [
    "normal",
    "near_edge",
    "out_of_bed",
    "overheat",
    "underheat",
    "sensor_fault",
    "crying",
    "face_covered",
    "standing",
    "moving",
    "sleeping",
    "awake",
    "air_quality",
    "suspected_fever",
]
CONTEXT_LABELS: tuple[tuple[str, ...], ...] = (
    ("unknown", "present", "absent", "uncertain"),
    ("unknown", "sleeping", "awake", "transition"),
    ("unknown", "quiet", "rolling", "active"),
    ("unknown", "clear", "partial", "covered", "not_visible"),
    ("unknown", "center", "near", "grab", "outside"),
    ("unknown", "none", "crying", "persistent_cry"),
    ("unknown", "hungry", "belly_pain", "discomfort", "tired", "burping"),
    ("unknown", "normal", "high", "low"),
    ("unknown", "normal", "high", "low"),
    ("unknown", "absent", "present"),
    ("ok", "degraded", "blind"),
)
CONTEXT_NAMES = (
    "presence",
    "arousal",
    "bed_motion",
    "airway_view",
    "edge_state",
    "distress",
    "cry_cause_hint",
    "body_temperature",
    "bed_thermal",
    "caregiver",
    "system_health",
)
INCIDENT_LABELS = (
    "none",
    "presence_conflict",
    "sensor_degraded",
    "monitor_blind",
    "airway_not_visible",
    "airway_partial",
    "airway_covered",
    "edge_near",
    "edge_climbing",
    "out_of_bed",
    "cry",
    "persistent_cry",
    "overheat",
    "underheat",
    "camera_frozen",
    "environment_air",
    "suspected_fever",
    "environment_co2_high",
    "environment_temperature_high",
    "environment_humidity_high",
    "environment_humidity_low",
    "environment_voc_suspected",
    "visual_subject_hidden",
)
LIFECYCLE_LABELS = ("clear", "candidate", "active", "escalated", "recovery_hold", "resolved")
ALARM_LABELS = ("normal", "L1", "L2", "L3")
COMFORT_ACTION_LABELS = {
    1 << 0: "light_off",
    1 << 1: "light_on",
    1 << 2: "pat",
    1 << 3: "caregiver_check",
}
COMFORT_REASON_LABELS = {
    1 << 0: "sleeping",
    1 << 1: "cry",
    1 << 2: "persistent_cry",
    1 << 3: "safety_event",
    1 << 4: "sensor_blind",
    1 << 5: "baby_absent",
}
COMFORT_BLOCK_LABELS = {
    1 << 0: "pat_blocked_by_danger",
    1 << 1: "pat_blocked_without_presence",
    1 << 2: "pat_blocked_by_monitor_blind",
}


class OverlayCalibrationError(ValueError):
    pass


def _loads(value: Any, fallback: Any) -> Any:
    if value is None:
        return fallback
    if not isinstance(value, str):
        return value
    try:
        return json.loads(value)
    except (TypeError, json.JSONDecodeError):
        return fallback


def _point_list(value: Any, default: list[list[float]]) -> list[list[float]]:
    points = _loads(value, default)
    if not isinstance(points, list) or len(points) != len(default):
        return [point[:] for point in default]
    result: list[list[float]] = []
    for point in points:
        if not isinstance(point, (list, tuple)) or len(point) != 2:
            return [item[:] for item in default]
        result.append([float(point[0]), float(point[1])])
    return result


def _matrix_list(value: Any) -> list[float]:
    matrix = _loads(value, IDENTITY_MATRIX)
    if not isinstance(matrix, list) or len(matrix) != 9:
        return IDENTITY_MATRIX[:]
    try:
        result = [float(item) for item in matrix]
    except (TypeError, ValueError):
        return IDENTITY_MATRIX[:]
    return result if all(math.isfinite(item) for item in result) else IDENTITY_MATRIX[:]


def calibration_from_row(row: dict[str, Any] | None) -> dict[str, Any]:
    if not row:
        return {
            "schema_version": OVERLAY_SCHEMA_VERSION,
            "enabled": True,
            "mode": "identity",
            "matrix": IDENTITY_MATRIX[:],
            "source_points": [point[:] for point in DEFAULT_POINTS],
            "target_points": [point[:] for point in DEFAULT_POINTS],
            "status": "identity_unverified",
            "updated_at": None,
            "thermal": {
                "enabled": False,
                "matrix": IDENTITY_MATRIX[:],
                "source_points": [point[:] for point in DEFAULT_THERMAL_POINTS],
                "target_points": [point[:] for point in DEFAULT_THERMAL_POINTS],
                "opacity": 0.32,
                "status": "uncalibrated",
            },
        }
    mode = str(row.get("mode") or "identity")
    matrix = _matrix_list(row.get("matrix_json"))
    return {
        "schema_version": OVERLAY_SCHEMA_VERSION,
        "enabled": bool(row.get("enabled", 1)),
        "mode": mode,
        "matrix": matrix,
        "source_points": _point_list(row.get("source_points_json"), DEFAULT_POINTS),
        "target_points": _point_list(row.get("target_points_json"), DEFAULT_POINTS),
        "status": "calibrated" if mode == "homography" else "identity_unverified",
        "updated_at": row.get("updated_at"),
        "thermal": {
            "enabled": bool(row.get("thermal_enabled", 0)),
            "matrix": _matrix_list(row.get("thermal_matrix_json")),
            "source_points": _point_list(
                row.get("thermal_source_points_json"), DEFAULT_THERMAL_POINTS
            ),
            "target_points": _point_list(
                row.get("thermal_target_points_json"), DEFAULT_THERMAL_POINTS
            ),
            "opacity": max(0.05, min(0.8, float(row.get("thermal_opacity") or 0.32))),
            "status": "calibrated" if bool(row.get("thermal_enabled", 0)) else "uncalibrated",
        },
    }


def _validate_points(points: list[list[float]], name: str) -> list[list[float]]:
    if len(points) != 4:
        raise OverlayCalibrationError(f"{name} must contain exactly four points")
    result: list[list[float]] = []
    for point in points:
        if len(point) != 2:
            raise OverlayCalibrationError(f"{name} points must be [x, y]")
        x, y = float(point[0]), float(point[1])
        if not (math.isfinite(x) and math.isfinite(y) and 0.0 <= x <= 1.0 and 0.0 <= y <= 1.0):
            raise OverlayCalibrationError(f"{name} coordinates must be finite values in 0..1")
        result.append([x, y])
    return result


def solve_homography(source_points: list[list[float]], target_points: list[list[float]]) -> list[float]:
    source = _validate_points(source_points, "source_points")
    target = _validate_points(target_points, "target_points")
    augmented: list[list[float]] = []
    for (x, y), (u, v) in zip(source, target, strict=True):
        augmented.append([x, y, 1.0, 0.0, 0.0, 0.0, -u * x, -u * y, u])
        augmented.append([0.0, 0.0, 0.0, x, y, 1.0, -v * x, -v * y, v])

    for column in range(8):
        pivot = max(range(column, 8), key=lambda row: abs(augmented[row][column]))
        if abs(augmented[pivot][column]) < 1e-10:
            raise OverlayCalibrationError("calibration points are degenerate")
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]
        divisor = augmented[column][column]
        augmented[column] = [value / divisor for value in augmented[column]]
        for row in range(8):
            if row == column:
                continue
            factor = augmented[row][column]
            if factor:
                augmented[row] = [
                    value - factor * pivot_value
                    for value, pivot_value in zip(augmented[row], augmented[column], strict=True)
                ]
    return [augmented[row][8] for row in range(8)] + [1.0]


def solve_similarity(source_points: list[list[float]], target_points: list[list[float]]) -> list[float]:
    source = _validate_points_count(source_points, "thermal_source_points", 2)
    target = _validate_points_count(target_points, "thermal_target_points", 2)
    (x0, y0), (x1, y1) = source
    (u0, v0), (u1, v1) = target
    dx, dy = x1 - x0, y1 - y0
    du, dv = u1 - u0, v1 - v0
    denominator = dx * dx + dy * dy
    if denominator < 1e-10:
        raise OverlayCalibrationError("thermal calibration points are degenerate")
    real = (du * dx + dv * dy) / denominator
    imag = (dv * dx - du * dy) / denominator
    translate_x = u0 - real * x0 + imag * y0
    translate_y = v0 - imag * x0 - real * y0
    return [real, -imag, translate_x, imag, real, translate_y, 0.0, 0.0, 1.0]


def _validate_points_count(
    points: list[list[float]],
    name: str,
    count: int,
) -> list[list[float]]:
    if len(points) != count:
        raise OverlayCalibrationError(f"{name} must contain exactly {count} points")
    result: list[list[float]] = []
    for point in points:
        if len(point) != 2:
            raise OverlayCalibrationError(f"{name} points must be [x, y]")
        x, y = float(point[0]), float(point[1])
        if not (math.isfinite(x) and math.isfinite(y) and 0.0 <= x <= 1.0 and 0.0 <= y <= 1.0):
            raise OverlayCalibrationError(f"{name} coordinates must be finite values in 0..1")
        result.append([x, y])
    return result


def prepare_thermal_calibration(
    *,
    enabled: bool,
    source_points: list[list[float]],
    target_points: list[list[float]],
    opacity: float,
) -> dict[str, Any]:
    source = _validate_points_count(source_points, "thermal_source_points", 2)
    target = _validate_points_count(target_points, "thermal_target_points", 2)
    if not math.isfinite(opacity) or not 0.05 <= opacity <= 0.8:
        raise OverlayCalibrationError("thermal opacity must be in 0.05..0.8")
    return {
        "enabled": bool(enabled),
        "matrix": solve_similarity(source, target),
        "source_points": source,
        "target_points": target,
        "opacity": float(opacity),
        "status": "calibrated" if enabled else "disabled",
    }


def prepare_calibration(
    *,
    enabled: bool,
    mode: str,
    source_points: list[list[float]],
    target_points: list[list[float]],
) -> dict[str, Any]:
    source = _validate_points(source_points, "source_points")
    target = _validate_points(target_points, "target_points")
    if mode == "identity":
        matrix = IDENTITY_MATRIX[:]
    elif mode == "homography":
        matrix = solve_homography(source, target)
    else:
        raise OverlayCalibrationError("mode must be identity or homography")
    return {
        "schema_version": OVERLAY_SCHEMA_VERSION,
        "enabled": bool(enabled),
        "mode": mode,
        "matrix": matrix,
        "source_points": source,
        "target_points": target,
        "status": "calibrated" if mode == "homography" else "identity_unverified",
        "updated_at": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
    }


def map_point(matrix: list[float], x: float, y: float) -> tuple[float, float]:
    denominator = matrix[6] * x + matrix[7] * y + matrix[8]
    if abs(denominator) < 1e-10:
        raise OverlayCalibrationError("point maps to infinity")
    mapped_x = (matrix[0] * x + matrix[1] * y + matrix[2]) / denominator
    mapped_y = (matrix[3] * x + matrix[4] * y + matrix[5]) / denominator
    return max(0.0, min(1.0, mapped_x)), max(0.0, min(1.0, mapped_y))


def build_vision_overlay(
    telemetry: dict[str, Any] | None,
    calibration: dict[str, Any],
) -> dict[str, Any]:
    telemetry = telemetry or {}
    vision = telemetry.get("vision") if isinstance(telemetry.get("vision"), dict) else {}
    inference = telemetry.get("inference") if isinstance(telemetry.get("inference"), dict) else {}
    width = max(0, int(vision.get("w") or 0))
    height = max(0, int(vision.get("h") or 0))
    detections = vision.get("d") if isinstance(vision.get("d"), list) else []
    boxes: list[dict[str, Any]] = []
    matrix = calibration.get("matrix", IDENTITY_MATRIX)
    if calibration.get("enabled") and width and height:
        for raw in detections[:8]:
            if not isinstance(raw, list) or len(raw) < 6:
                continue
            class_id, score, x, y, box_width, box_height = [int(value) for value in raw[:6]]
            if box_width <= 0 or box_height <= 0:
                continue
            x0, y0 = x / width, y / height
            x1, y1 = (x + box_width) / width, (y + box_height) / height
            try:
                polygon = [
                    map_point(matrix, x0, y0),
                    map_point(matrix, x1, y0),
                    map_point(matrix, x1, y1),
                    map_point(matrix, x0, y1),
                ]
            except OverlayCalibrationError:
                continue
            xs = [point[0] for point in polygon]
            ys = [point[1] for point in polygon]
            left, top, right, bottom = min(xs), min(ys), max(xs), max(ys)
            boxes.append(
                {
                    "class_id": class_id,
                    "label": VISION_LABELS[class_id] if 0 <= class_id < len(VISION_LABELS) else f"class_{class_id}",
                    "score_x1000": score,
                    "x": left,
                    "y": top,
                    "width": max(0.0, right - left),
                    "height": max(0.0, bottom - top),
                    "polygon": [[round(px, 6), round(py, 6)] for px, py in polygon],
                    "source_box": [x, y, box_width, box_height],
                }
            )

    vision_age_ms = 0
    evidence = inference.get("vv")
    if isinstance(evidence, list) and len(evidence) > 1:
        vision_age_ms = int(evidence[1] or 0)
    source_sequence = int(vision.get("q") or 0)
    stale = source_sequence == 0 or vision_age_ms > OVERLAY_STALE_MS
    expired = source_sequence == 0 or vision_age_ms > OVERLAY_EXPIRE_MS
    return {
        "schema_version": OVERLAY_SCHEMA_VERSION,
        "enabled": bool(calibration.get("enabled")),
        "calibration_status": calibration.get("status", "identity_unverified"),
        "source": "ra8_serial_camera",
        "target": "esp32p4x_eye_camera",
        "source_frame": {
            "sequence": source_sequence,
            "frame_sequence": int(vision.get("f") or 0),
            "width": width,
            "height": height,
            "fusion_eligible": bool(vision.get("fe")),
            "age_ms": vision_age_ms,
        },
        "stale": stale,
        "expired": expired,
        "boxes": boxes,
        "thermal": dict(calibration.get("thermal") or {"enabled": False, "status": "uncalibrated"}),
    }


def _label(labels: tuple[str, ...] | list[str], code: int, fallback: str) -> str:
    return labels[code] if 0 <= code < len(labels) else fallback


def _mask_labels(mask: int, labels: dict[int, str]) -> list[str]:
    return [label for bit, label in labels.items() if mask & bit]


def build_fusion_view(telemetry: dict[str, Any] | None) -> dict[str, Any]:
    telemetry = telemetry or {}
    inference = telemetry.get("inference") if isinstance(telemetry.get("inference"), dict) else {}
    contexts = inference.get("ctx") if isinstance(inference.get("ctx"), list) else []
    alarm = inference.get("alarm") if isinstance(inference.get("alarm"), list) else []
    incidents = inference.get("ai") if isinstance(inference.get("ai"), list) else []
    evidence_raw = inference.get("ex") if isinstance(inference.get("ex"), list) else []
    comfort_raw = inference.get("comfort") if isinstance(inference.get("comfort"), list) else []
    legacy_state = int(inference.get("state") or 0)
    legacy_severity = int(inference.get("severity") or 0)
    fallback_level = 0 if legacy_severity == 0 else (1 if legacy_severity == 1 else (2 if legacy_severity <= 3 else 3))
    level = int(alarm[0]) if len(alarm) > 0 else fallback_level
    primary_type = int(alarm[1]) if len(alarm) > 1 else 0

    context_view: dict[str, dict[str, Any]] = {}
    for index, name in enumerate(CONTEXT_NAMES):
        code = int(contexts[index]) if index < len(contexts) else 0
        context_view[name] = {
            "code": code,
            "label": _label(CONTEXT_LABELS[index], code, f"unknown_{code}"),
        }

    active: list[dict[str, Any]] = []
    for raw in incidents:
        if not isinstance(raw, list) or len(raw) < 8:
            continue
        incident_type = int(raw[0])
        lifecycle = int(raw[1])
        active.append(
            {
                "type": incident_type,
                "type_label": _label(INCIDENT_LABELS, incident_type, f"incident_{incident_type}"),
                "lifecycle": lifecycle,
                "lifecycle_label": _label(LIFECYCLE_LABELS, lifecycle, f"lifecycle_{lifecycle}"),
                "level": int(raw[2]),
                "severity": int(raw[3]),
                "confidence": int(raw[4]),
                "source_mask": int(raw[5]),
                "incident_id": int(raw[6]),
                "since_tick": int(raw[7]),
            }
        )
    evidence_values = [int(value or 0) for value in evidence_raw[:21]]
    evidence_values.extend([0] * (21 - len(evidence_values)))
    evidence = {
        "available": len(evidence_raw) >= 17,
        "presence_score_x1000": evidence_values[0],
        "presence_source_count": evidence_values[1],
        "pressure_presence_x1000": evidence_values[2],
        "vision_presence_x1000": evidence_values[3],
        "thermal_presence_x1000": evidence_values[4],
        "activity_score_x1000": evidence_values[5],
        "airway_risk_x1000": evidence_values[6],
        "edge_risk_x1000": evidence_values[7],
        "thermal_hotspot": {
            "points": evidence_values[8],
            "x_x1000": evidence_values[9],
            "y_x1000": evidence_values[10],
            "mean_cdeg": evidence_values[11],
        },
        "bed_temperature": {
            "left_milli_c": evidence_values[12],
            "center_milli_c": evidence_values[13],
            "right_milli_c": evidence_values[14],
            "spread_milli_c": evidence_values[15],
            "hotspot_region": evidence_values[16],
        },
        "calibration": {
            "available": len(evidence_raw) >= 21,
            "thermal_bed_x_x1000": evidence_values[17],
            "thermal_edge_x1000": evidence_values[18],
            "position_agreement_x1000": evidence_values[19],
            "profile_version": evidence_values[20],
        },
    }
    comfort_values = [int(value or 0) for value in comfort_raw[:5]]
    comfort_values.extend([0] * (5 - len(comfort_values)))
    action_mask, reason_mask, comfort_confidence, confirmation, block_mask = comfort_values
    comfort = {
        "available": len(comfort_raw) >= 5,
        "action_mask": action_mask,
        "reason_mask": reason_mask,
        "confidence": max(0, min(100, comfort_confidence)),
        "requires_confirmation": bool(confirmation),
        "block_mask": block_mask,
        "actions": _mask_labels(action_mask, COMFORT_ACTION_LABELS),
        "reasons": _mask_labels(reason_mask, COMFORT_REASON_LABELS),
        "blocked_reasons": _mask_labels(block_mask, COMFORT_BLOCK_LABELS),
        "recommended_light": "on" if action_mask & (1 << 1) else (
            "off" if action_mask & (1 << 0) else None
        ),
        "recommend_pat": bool(action_mask & (1 << 2)),
        "caregiver_check": bool(action_mask & (1 << 3)),
    }
    return {
        "schema_version": int(inference.get("sv") or 1),
        "evidence_sequence": int(inference.get("ev") or 0),
        "legacy": {
            "state": legacy_state,
            "label": _label(LEGACY_STATE_LABELS, legacy_state, f"state_{legacy_state}"),
            "confidence": int(inference.get("conf") or inference.get("confidence") or 0),
            "severity": legacy_severity,
        },
        "contexts": context_view,
        "alarm": {
            "level": level,
            "label": _label(ALARM_LABELS, level, f"L{level}"),
            "primary_type": primary_type,
            "primary_label": _label(INCIDENT_LABELS, primary_type, f"incident_{primary_type}"),
            "primary_incident_id": int(alarm[2]) if len(alarm) > 2 else 0,
            "primary_lifecycle": int(alarm[3]) if len(alarm) > 3 else 0,
            "active_count": int(alarm[4]) if len(alarm) > 4 else len(active),
            "active_type_mask": int(alarm[5]) if len(alarm) > 5 else 0,
        },
        "active_incidents": active,
        "evidence": evidence,
        "comfort": comfort,
    }

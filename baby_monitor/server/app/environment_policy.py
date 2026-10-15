from __future__ import annotations

from typing import Any


DEFAULT_ENVIRONMENT_THRESHOLDS = {
    "co2_max_ppm": 2000,
    "temperature_max_milli_c": 30000,
    "humidity_max_milli_percent": 70000,
    "humidity_min_milli_percent": 30000,
    "gas_min_ohm": 10000,
    "body_temperature_max_deci_c": 375,
}

HARDWARE_THRESHOLD_RANGES = {
    "co2_max_ppm": (400, 10000),
    "temperature_max_milli_c": (15000, 50000),
    "humidity_max_milli_percent": (20000, 100000),
    "humidity_min_milli_percent": (0, 95000),
    "gas_min_ohm": (0, 10000000),
    "body_temperature_max_deci_c": (300, 450),
}


def thresholds_from_row(row: dict[str, Any] | None) -> dict[str, Any]:
    values = dict(DEFAULT_ENVIRONMENT_THRESHOLDS)
    if row:
        for key in values:
            if row.get(key) is not None:
                values[key] = int(row[key])
    values["updated_at"] = row.get("updated_at") if row else None
    return values


def _integer(value: Any) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _baby_in_bed(telemetry: dict[str, Any]) -> bool:
    inference = telemetry.get("inference")
    if not isinstance(inference, dict):
        return False
    fusion = inference.get("fusion")
    if not isinstance(fusion, dict):
        fusion = inference
    contexts = fusion.get("ctx")
    # RA8 multimodal presence: 1=present, 2=absent, 3=uncertain.
    return isinstance(contexts, list) and bool(contexts) and _integer(contexts[0]) == 1


def _fusion_values(telemetry: dict[str, Any], key: str) -> list[Any]:
    inference = telemetry.get("inference")
    if not isinstance(inference, dict):
        return []
    fusion = inference.get("fusion")
    if not isinstance(fusion, dict):
        fusion = inference
    values = fusion.get(key)
    return values if isinstance(values, list) else []


def hardware_thresholds_from_telemetry(
    telemetry: dict[str, Any],
) -> dict[str, Any] | None:
    values = _fusion_values(telemetry, "limits2")
    legacy = len(values) < 7
    if legacy:
        values = _fusion_values(telemetry, "limits")
    if len(values) < 6:
        return None
    thresholds: dict[str, Any] = {}
    keys = [
        "co2_max_ppm", "temperature_max_milli_c",
        "humidity_max_milli_percent", "gas_min_ohm",
        "body_temperature_max_deci_c",
    ]
    if not legacy:
        keys.append("humidity_min_milli_percent")
    for index, key in enumerate(keys):
        limits = HARDWARE_THRESHOLD_RANGES[key]
        value = _integer(values[index])
        if value is None or not limits[0] <= value <= limits[1]:
            return None
        thresholds[key] = value
    if legacy:
        thresholds["humidity_min_milli_percent"] = DEFAULT_ENVIRONMENT_THRESHOLDS[
            "humidity_min_milli_percent"
        ]
    revision = _integer(values[5 if legacy else 6])
    if revision is None or revision <= 0:
        return None
    thresholds["revision"] = revision
    thresholds["source"] = "hardware"
    return thresholds


def thresholds_match_hardware(
    desired: dict[str, Any], active: dict[str, Any] | None
) -> bool:
    return active is not None and all(
        _integer(desired.get(key)) == _integer(active.get(key))
        for key in DEFAULT_ENVIRONMENT_THRESHOLDS
    )


def evaluate_environment(
    telemetry: dict[str, Any], thresholds: dict[str, Any]
) -> dict[str, Any]:
    hardware_thresholds = hardware_thresholds_from_telemetry(telemetry)
    effective_thresholds = hardware_thresholds or thresholds
    sensors = telemetry.get("sensors")
    sensors = sensors if isinstance(sensors, dict) else {}
    environment = sensors.get("environment")
    environment = environment if isinstance(environment, dict) else {}
    bme = environment.get("bme688")
    bme = bme if isinstance(bme, dict) else {}

    co2 = _integer(environment.get("co2", environment.get("co2_ppm")))
    temperature = _integer(environment.get("t_mc"))
    humidity = _integer(environment.get("rh_mp"))
    gas = _integer(bme.get("gas_ohm"))
    bme_flags = _integer(bme.get("flags")) or 0
    bme_state = _integer(bme.get("s"))
    bme_gas_valid = bme_state == 3 and bool(bme_flags & 0x02) and bool(bme_flags & 0x04)
    baby_in_bed = _baby_in_bed(telemetry)
    contexts = _fusion_values(telemetry, "ctx")
    evidence = _fusion_values(telemetry, "ex")
    body_temperature = _integer(evidence[11]) if len(evidence) > 11 else None
    thermal_presence = _integer(evidence[4]) if len(evidence) > 4 else None
    thermal_hotspot_points = _integer(evidence[8]) if len(evidence) > 8 else None
    body_state = _integer(contexts[7]) if len(contexts) > 7 else None
    body_temperature_valid = (
        baby_in_bed
        and body_temperature is not None
        and thermal_presence is not None
        and thermal_presence >= 550
        and thermal_hotspot_points is not None
        and thermal_hotspot_points >= 4
        and body_state in (1, 2)
    )

    exceeded: list[dict[str, Any]] = []
    checks = (
        ("co2", co2, int(effective_thresholds["co2_max_ppm"]), "ppm"),
        ("temperature", temperature, int(effective_thresholds["temperature_max_milli_c"]), "milli_celsius"),
        ("humidity", humidity, int(effective_thresholds["humidity_max_milli_percent"]), "milli_percent_rh"),
    )
    for name, value, threshold, unit in checks:
        if value is not None and value > threshold:
            exceeded.append(
                {"name": name, "value": value, "threshold": threshold, "direction": "high", "unit": unit}
            )
    humidity_min = int(effective_thresholds["humidity_min_milli_percent"])
    if humidity is not None and humidity < humidity_min:
        exceeded.append(
            {"name": "humidity", "value": humidity, "threshold": humidity_min,
             "direction": "low", "unit": "milli_percent_rh"}
        )
    if bme_gas_valid and gas is not None and gas < int(effective_thresholds["gas_min_ohm"]):
        exceeded.append(
            {
                "name": "gas_resistance",
                "value": gas,
                "threshold": int(effective_thresholds["gas_min_ohm"]),
                "direction": "low",
                "unit": "ohm",
            }
        )
    if body_temperature_valid and body_temperature > int(effective_thresholds["body_temperature_max_deci_c"]):
        exceeded.append(
            {
                "name": "body_temperature",
                "value": body_temperature,
                "threshold": int(effective_thresholds["body_temperature_max_deci_c"]),
                "direction": "high",
                "unit": "deci_celsius",
            }
        )

    return {
        "baby_in_bed": baby_in_bed,
        "active": baby_in_bed and bool(exceeded),
        "exceeded": exceeded if baby_in_bed else [],
        "observed": {
            "co2_ppm": co2,
            "temperature_milli_c": temperature,
            "humidity_milli_percent": humidity,
            "gas_ohm": gas,
            "bme_gas_valid": bme_gas_valid,
            "body_temperature_deci_c": body_temperature,
            "body_temperature_valid": body_temperature_valid,
            "body_temperature_state": body_state,
        },
        "thresholds": {
            key: int(effective_thresholds[key]) for key in DEFAULT_ENVIRONMENT_THRESHOLDS
        },
        "threshold_source": "hardware" if hardware_thresholds else "server_fallback",
        "hardware_revision": hardware_thresholds.get("revision") if hardware_thresholds else None,
    }

from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator


class Bme688Telemetry(BaseModel):
    """Compact BME688 status forwarded by the P4 gateway.

    Values intentionally stay in the RA8 integer units so the JSON is
    lossless in SQLite and over SSE/WebSocket.  Physical plausibility and
    freshness are diagnostic concerns: a fault-state sample must still be
    accepted so that the UI can explain the failure.
    """

    model_config = ConfigDict(extra="allow")

    s: int | None = Field(default=None, ge=0, le=255)
    addr: int | None = Field(default=None, ge=0, le=0x7F)
    t_mc: int | None = Field(default=None, ge=-(2**31), le=2**31 - 1)
    rh_mp: int | None = Field(default=None, ge=0, le=0xFFFFFFFF)
    p_pa: int | None = Field(default=None, ge=0, le=0xFFFFFFFF)
    gas_ohm: int | None = Field(default=None, ge=0, le=0xFFFFFFFF)
    flags: int | None = Field(default=None, ge=0, le=0xFF)
    last: int | None = Field(default=None, ge=0, le=0xFFFFFFFF)
    err: int | None = Field(default=None, ge=0, le=0xFFFFFFFF)


def normalize_telemetry_bme688(value: dict[str, Any]) -> dict[str, Any]:
    """Return a telemetry mapping with the BME688 migration alias canonicalized."""

    source_sensors = value.get("sensors")
    if not isinstance(source_sensors, dict):
        return value

    source_environment = source_sensors.get("environment")
    canonical = (
        source_environment.get("bme688")
        if isinstance(source_environment, dict)
        else None
    )
    alias = source_sensors.get("bme688")
    if not isinstance(canonical, dict) and not isinstance(alias, dict):
        return value

    merged: dict[str, Any] = {}
    if isinstance(alias, dict):
        merged.update(alias)
    if isinstance(canonical, dict):
        merged.update(canonical)
    normalized_bme688 = Bme688Telemetry.model_validate(merged).model_dump(
        exclude_none=True
    )

    result = dict(value)
    sensors = dict(source_sensors)
    environment = dict(source_environment) if isinstance(source_environment, dict) else {}
    environment["bme688"] = normalized_bme688
    sensors["environment"] = environment
    sensors.pop("bme688", None)
    result["sensors"] = sensors
    return result


class DeviceHello(BaseModel):
    protocol_version: int = 1
    firmware: str = ""
    hardware: str = "ESP32-CAM"
    capabilities: list[str] = Field(default_factory=list)
    metadata: dict[str, Any] = Field(default_factory=dict)


class TelemetryUpload(BaseModel):
    protocol_version: int = 1
    sequence: int = Field(ge=0)
    device_time_ms: int = Field(default=0, ge=0)
    ra8_tick: int = Field(default=0, ge=0)
    wifi_rssi: int | None = None
    firmware: str = ""
    sensors: dict[str, Any] = Field(default_factory=dict)
    inference: dict[str, Any] = Field(default_factory=dict)
    vision: dict[str, Any] = Field(default_factory=dict)
    health: dict[str, Any] = Field(default_factory=dict)

    @model_validator(mode="before")
    @classmethod
    def normalize_bme688_location(cls, value: Any) -> Any:
        """Accept the migration alias and persist one canonical representation.

        The canonical location extends the existing SCD41 environment object:
        ``sensors.environment.bme688``.  Early gateway builds may send
        ``sensors.bme688``; canonical fields win if both are present.
        """

        if not isinstance(value, dict):
            return value
        return normalize_telemetry_bme688(value)


class EventUpload(BaseModel):
    protocol_version: int = 1
    event_id: int = Field(ge=0)
    event_type: str = Field(min_length=1, max_length=64)
    state: int | str = 0
    severity: int = Field(default=0, ge=0, le=5)
    confidence: float = Field(default=0, ge=0, le=100)
    occurred_at_ms: int = Field(default=0, ge=0)
    sources: list[str] = Field(default_factory=list)
    payload: dict[str, Any] = Field(default_factory=dict)


class LogUpload(BaseModel):
    level: Literal["debug", "info", "warning", "error", "critical"] = "info"
    component: str = Field(default="esp32", max_length=64)
    message: str = Field(min_length=1, max_length=2048)
    device_time_ms: int = Field(default=0, ge=0)
    context: dict[str, Any] = Field(default_factory=dict)


class SessionCreate(BaseModel):
    device_id: str = Field(default="demo-device", min_length=1, max_length=120)
    name: str = Field(min_length=1, max_length=120)
    description: str = Field(default="", max_length=1000)
    target_labels: list[str] = Field(default_factory=list)
    metadata: dict[str, Any] = Field(default_factory=dict)


class SampleUpdate(BaseModel):
    label: str | None = Field(default=None, max_length=120)
    notes: str | None = Field(default=None, max_length=2000)
    quality: Literal["unreviewed", "good", "bad", "uncertain"] | None = None
    split: Literal["unassigned", "train", "validation", "test"] | None = None


class CommandCreate(BaseModel):
    device_id: str = Field(min_length=1, max_length=120)
    command: str = Field(min_length=1, max_length=64)
    args: dict[str, Any] = Field(default_factory=dict)


class MobileCommandCreate(BaseModel):
    action: str = Field(min_length=1, max_length=64)
    args: dict[str, Any] = Field(default_factory=dict)
    request_id: str | None = Field(default=None, min_length=1, max_length=64)


class MediaRecordingControl(BaseModel):
    device_id: str = Field(min_length=1, max_length=120)
    filename: str = Field(default="", max_length=64)


class ResourceRecordingControl(BaseModel):
    device_id: str = Field(min_length=1, max_length=120)


class OverlayCalibrationUpdate(BaseModel):
    enabled: bool = True
    mode: Literal["identity", "homography"] = "identity"
    source_points: list[list[float]] = Field(
        default_factory=lambda: [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
        min_length=4,
        max_length=4,
    )
    target_points: list[list[float]] = Field(
        default_factory=lambda: [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
        min_length=4,
        max_length=4,
    )
    thermal_enabled: bool = False
    thermal_source_points: list[list[float]] = Field(
        default_factory=lambda: [[0.25, 0.25], [0.75, 0.75]],
        min_length=2,
        max_length=2,
    )
    thermal_target_points: list[list[float]] = Field(
        default_factory=lambda: [[0.25, 0.25], [0.75, 0.75]],
        min_length=2,
        max_length=2,
    )
    thermal_opacity: float = Field(default=0.32, ge=0.05, le=0.8)


class EnvironmentThresholdUpdate(BaseModel):
    co2_max_ppm: int = Field(default=2000, ge=400, le=10000)
    temperature_max_milli_c: int = Field(default=30000, ge=15000, le=50000)
    humidity_max_milli_percent: int = Field(default=70000, ge=20000, le=100000)
    humidity_min_milli_percent: int = Field(default=30000, ge=0, le=95000)
    gas_min_ohm: int = Field(default=10000, ge=0, le=10000000)
    body_temperature_max_deci_c: int = Field(default=375, ge=300, le=450)

    @model_validator(mode="after")
    def validate_humidity_range(self) -> "EnvironmentThresholdUpdate":
        if self.humidity_min_milli_percent >= self.humidity_max_milli_percent:
            raise ValueError("humidity minimum must be below humidity maximum")
        return self


class CommandAck(BaseModel):
    ok: bool = True
    result: dict[str, Any] = Field(default_factory=dict)


class StoryCreate(BaseModel):
    theme: str = Field(min_length=1, max_length=120)
    child_age_months: int = Field(default=12, ge=0, le=72)
    duration_seconds: int = Field(default=60, ge=30, le=240)
    language: Literal["zh-CN"] = "zh-CN"
    style: Literal["calm", "gentle", "playful"] = "calm"
    autoplay: bool = True
    request_id: str | None = Field(default=None, min_length=1, max_length=64)

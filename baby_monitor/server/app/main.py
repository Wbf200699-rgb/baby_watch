from __future__ import annotations

import asyncio
import base64
import csv
import hmac
import hashlib
import io
import json
import logging
import mimetypes
import re
import sqlite3
import struct
import time
import zipfile
from contextlib import asynccontextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Query, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import ValidationError

from .ai_service import AiRuntime, AiServiceError
from .config import Settings
from .db import Database, decode_json_columns, utc_now
from .diagnostics import build_sensor_diagnostics
from .environment_policy import (
    DEFAULT_ENVIRONMENT_THRESHOLDS,
    evaluate_environment,
    hardware_thresholds_from_telemetry,
    thresholds_from_row,
    thresholds_match_hardware,
)
from .models import (
    CommandAck,
    CommandCreate,
    DeviceHello,
    EnvironmentThresholdUpdate,
    EventUpload,
    LogUpload,
    MediaRecordingControl,
    MobileCommandCreate,
    OverlayCalibrationUpdate,
    ResourceRecordingControl,
    SampleUpdate,
    SessionCreate,
    StoryCreate,
    TelemetryUpload,
    normalize_telemetry_bme688,
)
from .overlay import (
    OverlayCalibrationError,
    build_fusion_view,
    build_vision_overlay,
    calibration_from_row,
    prepare_calibration,
    prepare_thermal_calibration,
)


logger = logging.getLogger(__name__)
from .mobile import (
    MOBILE_COMMAND_DEFINITIONS,
    MOBILE_PROTOCOL_VERSION,
    MobileCommandError,
    MobilePresenceRegistry,
    build_mobile_snapshot,
    map_mobile_command,
    mobile_command_view,
    mobile_device,
)
from .matrix_stream import (
    MatrixFrame,
    MatrixProtocolError,
    MatrixRealtimeBroker,
    parse_matrix_frame,
    sequence_gap,
)
from .telemetry_stream import TelemetryFrame, TelemetryRealtimeBroker
from .protocol import (
    EventBroker,
    MATRIX_KINDS,
    MediaBroker,
    MediaProtocolError,
    authenticate_device,
    decode_i16_le_matrix,
    decode_s3_audio_frame,
    decode_s3_video_frame,
)


BATCH_HEADER_V1 = struct.Struct("<4sBBHIIHHHH")
BATCH_HEADER_V2 = struct.Struct("<4sBBHIIHHHHIIIIIBBH")
BATCH_MAGIC = b"RA8B"
BATCH_PRESSURE = 1 << 0
BATCH_THERMAL = 1 << 1
BATCH_VIDEO = 1 << 2
BATCH_AUDIO = 1 << 3
MODEL_DATA_KINDS = {"audio", "camera"}
REFERENCE_DATA_KINDS = {"thermal", "pressure", "temperature", "resource"}
IMPORT_EXTENSIONS = {
    "camera": {".jpg", ".jpeg"},
    "audio": {".wav"},
}
RECORDING_BASENAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,7}$")
RECORDING_ACTIONS = {
    "audio": {"start", "stop"},
    "camera": {"capture", "start", "stop"},
}


def _json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _loads(value: str | None, fallback: Any) -> Any:
    if not value:
        return fallback
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return fallback


def _as_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _safe_component(value: str, fallback: str) -> str:
    cleaned = "".join(
        character if character.isalnum() or character in {"-", "_", "."} else "_"
        for character in str(value).strip()
    ).strip("._")
    return cleaned[:120] or fallback


def _recording_basename(kind: str, value: str) -> str:
    name = str(value or "").strip()
    if not name:
        return ""
    if "/" in name or "\\" in name:
        raise HTTPException(status_code=422, detail="filename must not contain a path")

    lower = name.lower()
    suffixes = (".wav",) if kind == "audio" else (".jpg", ".jpeg")
    for suffix in suffixes:
        if lower.endswith(suffix):
            name = name[: -len(suffix)]
            break
    if "." in name or not RECORDING_BASENAME.fullmatch(name):
        raise HTTPException(
            status_code=422,
            detail="filename must be 1-8 ASCII letters, digits, underscores or hyphens",
        )
    return name


def _metadata_counter(metadata: dict[str, Any], key: str) -> int:
    baseline = metadata.get("sd_baseline", {})
    return _as_int(baseline.get(key)) if isinstance(baseline, dict) else 0


def _online(last_seen: str, threshold: int) -> bool:
    try:
        seen = datetime.fromisoformat(last_seen)
        return (datetime.now(timezone.utc) - seen).total_seconds() <= threshold
    except (TypeError, ValueError):
        return False


def _request_ip(request: Request) -> str:
    forwarded = request.headers.get("x-forwarded-for", "").split(",", 1)[0].strip()
    return forwarded or request.headers.get("x-real-ip", "").strip() or (request.client.host if request.client else "")


def _websocket_ip(websocket: WebSocket) -> str:
    forwarded = websocket.headers.get("x-forwarded-for", "").split(",", 1)[0].strip()
    return forwarded or websocket.headers.get("x-real-ip", "").strip() or (
        websocket.client.host if websocket.client else ""
    )


def _basic_auth_valid(authorization: str, expected_user: str, expected_password: str) -> bool:
    try:
        scheme, encoded = authorization.split(" ", 1)
        user, password = base64.b64decode(encoded).decode("utf-8").split(":", 1)
    except (ValueError, UnicodeDecodeError):
        return False
    return (
        scheme.lower() == "basic"
        and hmac.compare_digest(user, expected_user)
        and hmac.compare_digest(password, expected_password)
    )


async def _cancel_tasks(*tasks: asyncio.Task[Any]) -> None:
    for task in tasks:
        if not task.done():
            task.cancel()
    try:
        await asyncio.gather(*tasks, return_exceptions=True)
    except asyncio.CancelledError:
        # The ASGI connection task can be cancelled while its children are
        # already shutting down. Cleanup cancellation must not escape.
        pass


def create_app(settings: Settings | None = None) -> FastAPI:
    app_settings = settings or Settings.from_env()
    database = Database(app_settings.database_path)
    broker = EventBroker()
    media = MediaBroker(video_target_fps=app_settings.video_target_fps)
    mobile_presence = MobilePresenceRegistry()
    maintenance_counts: dict[str, int] = {}
    matrix_runtime: MatrixRealtimeBroker | None = None
    telemetry_runtime: TelemetryRealtimeBroker | None = None
    ai_runtime: AiRuntime | None = None

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        app_settings.data_dir.mkdir(parents=True, exist_ok=True)
        app_settings.artifact_dir.mkdir(parents=True, exist_ok=True)
        database.initialize()
        if ai_runtime is not None:
            await ai_runtime.start()
        try:
            yield
        finally:
            if ai_runtime is not None:
                await ai_runtime.close()
            if matrix_runtime is not None:
                await matrix_runtime.close()
            if telemetry_runtime is not None:
                await telemetry_runtime.close()

    app = FastAPI(title="Baby Monitor Lab", version="0.6.0", lifespan=lifespan)
    app.state.settings = app_settings
    app.state.db = database
    app.state.broker = broker
    app.state.media = media
    app.state.mobile_presence = mobile_presence
    static_dir = app_settings.root_dir / "static"
    app.mount("/static", StaticFiles(directory=static_dir), name="static")

    @app.middleware("http")
    async def dashboard_auth(request: Request, call_next):
        if (
            not app_settings.admin_user
            or request.url.path == "/health"
            or request.url.path.startswith("/api/v1/")
        ):
            response = await call_next(request)
        elif not _basic_auth_valid(
            request.headers.get("authorization", ""),
            app_settings.admin_user,
            app_settings.admin_password,
        ):
            return Response(status_code=401, headers={"WWW-Authenticate": 'Basic realm="Baby Monitor"'})
        else:
            response = await call_next(request)
        if request.url.path.startswith("/static/"):
            response.headers["Cache-Control"] = "no-store"
        return response

    def device_id_for(request: Request) -> str:
        return authenticate_device(request, app_settings)

    def overlay_calibration_for(device_id: str) -> dict[str, Any]:
        return calibration_from_row(
            database.query_one(
                "SELECT * FROM overlay_calibrations WHERE device_id=?",
                (device_id,),
            )
        )

    def overlay_views(
        device_id: str,
        telemetry: dict[str, Any] | None,
    ) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
        calibration = overlay_calibration_for(device_id)
        return calibration, build_vision_overlay(telemetry, calibration), build_fusion_view(telemetry)

    async def publish(kind: str, device_id: str, data: dict[str, Any]) -> None:
        await broker.publish({"kind": kind, "device_id": device_id, "at": utc_now(), "data": data})

    ai_runtime = AiRuntime(app_settings, database, publish)
    app.state.ai_runtime = ai_runtime
    environment_threshold_cache: dict[str, dict[str, Any]] = {}

    def environment_thresholds_for(device_id: str) -> dict[str, Any]:
        cached = environment_threshold_cache.get(device_id)
        if cached is not None:
            return cached
        thresholds = thresholds_from_row(
            database.query_one(
                "SELECT * FROM environment_thresholds WHERE device_id=?", (device_id,)
            )
        )
        environment_threshold_cache[device_id] = thresholds
        return thresholds

    def environment_threshold_payload(thresholds: dict[str, Any]) -> str:
        keys = (
            "co2_max_ppm", "temperature_max_milli_c",
            "humidity_max_milli_percent", "gas_min_ohm",
            "body_temperature_max_deci_c", "humidity_min_milli_percent",
        )
        values = [str(int(thresholds[key])) for key in keys]
        return "environment_thresholds_v2:" + ",".join(values)

    def enqueue_environment_threshold_sync(
        device_id: str, thresholds: dict[str, Any]
    ) -> dict[str, Any]:
        command = {
            "command": "ra8.command",
            "args": {"payload": environment_threshold_payload(thresholds)},
        }
        row, _ = database.enqueue_command(device_id, _json(command), utc_now())
        return row

    def reapply_saved_environment_thresholds(device_id: str) -> None:
        row = database.query_one(
            "SELECT * FROM environment_thresholds WHERE device_id=?", (device_id,)
        )
        if row is not None:
            enqueue_environment_threshold_sync(device_id, thresholds_from_row(row))

    def attach_environment_policy(device_id: str, body: dict[str, Any]) -> dict[str, Any]:
        result = dict(body)
        result["environment_policy"] = evaluate_environment(
            result, environment_thresholds_for(device_id)
        )
        return result

    def matrix_status_for(device_id: str) -> dict[str, object]:
        if matrix_runtime is None:
            return {
                "matrix_connected": False,
                "pressure_fps": 0.0,
                "thermal_fps": 0.0,
                "matrix_drops": 0,
            }
        return matrix_runtime.status(device_id)

    def telemetry_status_for(device_id: str) -> dict[str, Any]:
        if telemetry_runtime is None:
            return {
                "telemetry_connected": False,
                "telemetry_fps": 0.0,
                "telemetry_age_ms": None,
                "telemetry_ack_p95_ms": 0.0,
                "telemetry_database_p95_ms": 0.0,
                "telemetry_drops": 0,
            }
        return telemetry_runtime.status(device_id)

    def add_matrix_status(device_id: str, telemetry: dict[str, Any] | None) -> dict[str, Any] | None:
        if telemetry is None:
            return None
        result = dict(telemetry)
        source_health = telemetry.get("health")
        health = dict(source_health) if isinstance(source_health, dict) else {}
        source_gateway = health.get("gateway")
        gateway = dict(source_gateway) if isinstance(source_gateway, dict) else {}
        gateway.update(matrix_status_for(device_id))
        gateway.update(telemetry_status_for(device_id))
        health["gateway"] = gateway
        result["health"] = health
        return result

    def capture_active(
        device_id: str,
        kind: str,
        source_ref: str,
        features: dict[str, Any],
        *,
        unique: bool = False,
        refresh_existing: bool = False,
        metadata_filter: Any = None,
    ) -> list[int]:
        sample_ids: list[int] = []
        for session in database.active_sessions():
            metadata = _loads(session.get("metadata_json"), {})
            if metadata.get("device_id") != device_id:
                continue
            if metadata_filter is not None and not metadata_filter(metadata):
                continue
            active_kinds = set(metadata.get("active_kinds", []))
            if kind not in active_kinds:
                continue
            labels = _loads(session.get("target_labels_json"), [])
            capture_label = metadata.get("capture_label")
            if not capture_label and len(labels) == 1:
                capture_label = labels[0]
            default_split = str(metadata.get("default_split", "unassigned"))
            if default_split not in {"unassigned", "train", "validation", "test"}:
                default_split = "unassigned"
            existing = None
            if unique or refresh_existing:
                existing = database.query_one(
                    "SELECT id FROM samples WHERE session_id=? AND source_ref=? LIMIT 1",
                    (session["session_id"], source_ref),
                )
            if existing:
                if refresh_existing:
                    database.execute(
                        "UPDATE samples SET features_json=? WHERE id=?",
                        (_json(features), existing["id"]),
                    )
                sample_ids.append(existing["id"])
                continue
            sample_ids.append(database.capture_sample(
                session["session_id"], device_id, kind, source_ref, features, capture_label, default_split
            ))
        return sample_ids

    def resource_recording_session(device_id: str, *, active_only: bool = False) -> dict[str, Any] | None:
        status_clause = "AND status='active'" if active_only else ""
        rows = database.query_all(
            f"SELECT * FROM collection_sessions WHERE 1=1 {status_clause} ORDER BY created_at DESC LIMIT 100"
        )
        for row in rows:
            metadata = _loads(row.get("metadata_json"), {})
            if metadata.get("device_id") != device_id or metadata.get("data_kind") != "resource":
                continue
            row["metadata"] = metadata
            row["sample_count"] = _as_int((database.query_one(
                "SELECT COUNT(*) AS count FROM samples WHERE session_id=? AND kind='resource'",
                (row["session_id"],),
            ) or {}).get("count"))
            row["download_url"] = f"/api/dashboard/resource-recordings/{row['session_id']}/csv"
            return row
        return None

    def capture_sd_artifact_status(
        device_id: str,
        telemetry_sequence: int,
        ra8_tick: int,
        sensors: dict[str, Any],
    ) -> None:
        camera = sensors.get("camera")
        if isinstance(camera, dict):
            command_sequence = _as_int(camera.get("cmd_seq"))
            saved_count = _as_int(camera.get("saved"))
            if (
                command_sequence > 0
                and saved_count > 0
                and _as_int(camera.get("rec")) == 1
                and _as_int(camera.get("soi")) == 1
                and _as_int(camera.get("eoi")) == 1
                and _as_int(camera.get("werr")) == 0
            ):
                for saved_index in range(max(1, saved_count - 999), saved_count + 1):
                    capture_active(
                        device_id,
                        "camera",
                        f"sd:camera:{command_sequence}:{saved_index}",
                        {
                            "storage": "sd",
                            "transfer_status": "pending_import",
                            "expected_path_pattern": "/CAMERA/C*.JPG",
                            "telemetry_sequence": telemetry_sequence,
                            "ra8_tick": ra8_tick,
                            **camera,
                            "saved_index": saved_index,
                        },
                        unique=True,
                        metadata_filter=lambda metadata, index=saved_index, command=command_sequence: (
                            index > _metadata_counter(metadata, "camera_saved")
                            and command > _metadata_counter(metadata, "camera_cmd_seq")
                        ),
                    )

        microphone = sensors.get("microphone")
        if isinstance(microphone, dict):
            command_sequence = _as_int(microphone.get("cmd_seq"))
            writer_state = _as_int(microphone.get("wst"))
            has_sd_file = writer_state in {6, 7, 9} and _as_int(microphone.get("wbytes")) > 0
            if command_sequence > 0 and has_sd_file and _as_int(microphone.get("werr")) == 0:
                capture_active(
                    device_id,
                    "audio",
                    f"sd:audio:{command_sequence}",
                    {
                        "storage": "sd",
                        "transfer_status": "pending_import",
                        "expected_path_pattern": "/MIC/M*.WAV",
                        "telemetry_sequence": telemetry_sequence,
                        "ra8_tick": ra8_tick,
                        **microphone,
                    },
                    unique=True,
                    refresh_existing=True,
                    metadata_filter=lambda metadata, command=command_sequence: (
                        command > _metadata_counter(metadata, "audio_cmd_seq")
                    ),
                )

    def persist_hello(device_id: str, ip_address: str, payload: DeviceHello) -> None:
        database.touch_device(
            device_id,
            firmware=payload.firmware,
            hardware=payload.hardware,
            ip_address=ip_address,
            capabilities=payload.capabilities,
            metadata=payload.metadata,
        )
        database.execute(
            """
            INSERT INTO device_logs(device_id, received_at, device_time_ms, level, component, message, context_json)
            VALUES(?, ?, 0, 'info', 'connection', 'device hello received', ?)
            """,
            (device_id, utc_now(), _json({"observed_ip": ip_address, **payload.metadata})),
        )

    async def store_hello(device_id: str, ip_address: str, payload: DeviceHello) -> dict[str, Any]:
        await asyncio.to_thread(persist_hello, device_id, ip_address, payload)
        await asyncio.to_thread(reapply_saved_environment_thresholds, device_id)
        await publish("device", device_id, {"online": True, "firmware": payload.firmware})
        return {"ok": True, "server_time": utc_now(), "protocol_version": 2}

    def persist_telemetry_record(
        device_id: str,
        ip_address: str,
        body: dict[str, Any],
        received_at: str,
    ) -> int:
        database.touch_device(
            device_id,
            firmware=str(body.get("firmware", "")),
            ip_address=ip_address,
            wifi_rssi=body.get("wifi_rssi"),
        )
        row_id = database.execute(
            """
            INSERT INTO telemetry(device_id, sequence, received_at, device_time_ms, ra8_tick, payload_json)
            VALUES(?, ?, ?, ?, ?, ?)
            """,
            (
                device_id,
                _as_int(body.get("sequence")),
                received_at,
                _as_int(body.get("device_time_ms")),
                _as_int(body.get("ra8_tick")),
                _json(body),
            ),
        )
        sensors = body.get("sensors", {})
        temperature = sensors.get("bed_temperature")
        if isinstance(temperature, dict):
            capture_active(
                device_id,
                "temperature",
                f"telemetry:{row_id}",
                {
                    "sequence": _as_int(body.get("sequence")),
                    "ra8_tick": _as_int(body.get("ra8_tick")),
                    **temperature,
                },
            )
        health = body.get("health", {})
        resources = health.get("ra8_resources") if isinstance(health, dict) else None
        if isinstance(resources, dict) and resources:
            resource_sequence = _as_int(resources.get("sequence"))
            capture_active(
                device_id,
                "resource",
                f"resource:{resource_sequence}",
                {
                    "received_at": received_at,
                    "telemetry_sequence": _as_int(body.get("sequence")),
                    "device_time_ms": _as_int(body.get("device_time_ms")),
                    "ra8_tick": _as_int(body.get("ra8_tick")),
                    **resources,
                },
                unique=True,
            )
        capture_sd_artifact_status(
            device_id,
            _as_int(body.get("sequence")),
            _as_int(body.get("ra8_tick")),
            sensors,
        )
        maintenance_counts[device_id] = maintenance_counts.get(device_id, 0) + 1
        if maintenance_counts[device_id] >= 1200:
            maintenance_counts[device_id] = 0
            database.prune_runtime_data(
                device_id,
                telemetry_rows=app_settings.telemetry_retention_rows,
                log_rows=app_settings.log_retention_rows,
                matrix_rows=app_settings.matrix_retention_rows,
            )
        return row_id

    async def store_telemetry(device_id: str, ip_address: str, payload: TelemetryUpload) -> dict[str, Any]:
        body = attach_environment_policy(device_id, payload.model_dump())
        received_at = utc_now()
        row_id = await asyncio.to_thread(
            persist_telemetry_record,
            device_id,
            ip_address,
            body,
            received_at,
        )
        live_body = {**body, "received_at": received_at}
        live_body = add_matrix_status(device_id, live_body) or live_body
        if ai_runtime is not None and mobile_presence.connected(device_id):
            ai_runtime.offer_telemetry(device_id, live_body)
        await publish("telemetry", device_id, live_body)
        return {"ok": True, "id": row_id, "received_at": received_at}

    def persist_event_record(
        device_id: str,
        ip_address: str,
        payload: EventUpload,
    ) -> tuple[dict[str, Any], dict[str, Any], bool]:
        received_at = utc_now()
        body = payload.model_dump()
        body_json = _json(body)
        duplicate_after = (datetime.now(timezone.utc) - timedelta(minutes=10)).isoformat(timespec="milliseconds")
        row = database.query_one(
            """
            SELECT id, acknowledged FROM events
            WHERE device_id=? AND payload_json=? AND received_at>=?
            ORDER BY id DESC LIMIT 1
            """,
            (device_id, body_json, duplicate_after),
        )
        duplicate = row is not None
        if not duplicate:
            source_event_id = payload.event_id
            collision = database.query_one(
                "SELECT id FROM events WHERE device_id=? AND source_event_id=?",
                (device_id, source_event_id),
            )
            if collision is not None:
                latest = database.query_one(
                    "SELECT COALESCE(MAX(source_event_id), 0) AS value FROM events WHERE device_id=?",
                    (device_id,),
                )
                source_event_id = int((latest or {}).get("value") or 0) + 1
            row_id = database.execute(
                """
                INSERT INTO events(device_id, source_event_id, event_type, state, severity, confidence,
                                   occurred_at_ms, received_at, payload_json)
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    device_id,
                    source_event_id,
                    payload.event_type,
                    str(payload.state),
                    payload.severity,
                    payload.confidence,
                    payload.occurred_at_ms,
                    received_at,
                    body_json,
                ),
            )
            row = {"id": row_id, "acknowledged": 0}
        if not duplicate:
            capture_active(device_id, "event", f"event:{row['id']}", body)
        return dict(row), body, duplicate

    async def store_event(device_id: str, ip_address: str, payload: EventUpload) -> dict[str, Any]:
        row, body, duplicate = await asyncio.to_thread(
            persist_event_record, device_id, ip_address, payload
        )
        if not duplicate:
            await publish("event", device_id, {"id": row["id"], **body})
            if ai_runtime is not None and mobile_presence.connected(device_id):
                ai_runtime.offer_event(device_id, body)
        return {"ok": True, "id": row["id"], "duplicate": duplicate}

    def persist_matrix_record(
        device_id: str,
        ip_address: str,
        kind: str,
        sequence: int,
        rows: int,
        cols: int,
        unit: str,
        raw: bytes,
        *,
        received_at: str | None = None,
        touch_device: bool = True,
    ) -> dict[str, Any]:
        if kind not in MATRIX_KINDS:
            raise HTTPException(status_code=404, detail="unknown matrix kind")
        values = decode_i16_le_matrix(raw, rows, cols)
        if touch_device:
            database.touch_device(device_id, ip_address=ip_address)
        stored_at = received_at or utc_now()
        minimum = min(values)
        maximum = max(values)
        row_id = database.execute(
            """
            INSERT INTO matrices(device_id, kind, sequence, rows, cols, unit, received_at, minimum, maximum, data)
            VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (device_id, kind, sequence, rows, cols, unit, stored_at, minimum, maximum, raw),
        )
        capture_active(
            device_id,
            kind,
            f"matrix:{row_id}",
            {"sequence": sequence, "rows": rows, "cols": cols, "unit": unit, "min": minimum, "max": maximum},
        )
        return {
            "ok": True,
            "id": row_id,
            "kind": kind,
            "sequence": sequence,
            "rows": rows,
            "cols": cols,
            "unit": unit,
            "received_at": stored_at,
            "minimum": minimum,
            "maximum": maximum,
            "values": values,
        }

    async def store_matrix(
        device_id: str,
        ip_address: str,
        kind: str,
        sequence: int,
        rows: int,
        cols: int,
        unit: str,
        raw: bytes,
    ) -> dict[str, Any]:
        result = await asyncio.to_thread(
            persist_matrix_record,
            device_id,
            ip_address,
            kind,
            sequence,
            rows,
            cols,
            unit,
            raw,
        )
        await publish(
            "matrix",
            device_id,
            {key: value for key, value in result.items() if key != "ok"},
        )
        return {
            "ok": True,
            "id": result["id"],
            "minimum": result["minimum"],
            "maximum": result["maximum"],
        }

    def matrix_collection_active_sync(device_id: str, kind: str) -> bool:
        for session in database.active_sessions():
            metadata = _loads(session.get("metadata_json"), {})
            if metadata.get("device_id") != device_id:
                continue
            if kind in set(metadata.get("active_kinds", [])):
                return True
        return False

    async def matrix_collection_active(device_id: str, kind: str) -> bool:
        return await asyncio.to_thread(matrix_collection_active_sync, device_id, kind)

    async def persist_realtime_matrix(frame: MatrixFrame) -> None:
        await asyncio.to_thread(
            persist_matrix_record,
            frame.device_id,
            frame.ip_address,
            frame.kind,
            frame.sequence,
            frame.rows,
            frame.cols,
            frame.unit,
            frame.payload,
            received_at=frame.received_at,
            touch_device=False,
        )

    async def publish_realtime_matrix(frame: MatrixFrame) -> None:
        values = frame.values()
        data: dict[str, Any] = {
            "kind": frame.kind,
            "sequence": frame.sequence,
            "rows": frame.rows,
            "cols": frame.cols,
            "unit": frame.unit,
            "received_at": frame.received_at,
            "minimum": min(values),
            "maximum": max(values),
            "values": values,
        }
        if matrix_runtime is not None:
            data.update(matrix_runtime.kind_status(frame.device_id, frame.kind))
        await publish("matrix", frame.device_id, data)

    matrix_runtime = MatrixRealtimeBroker(
        persist=persist_realtime_matrix,
        publish=publish_realtime_matrix,
        capture_active=matrix_collection_active,
    )
    app.state.matrix_runtime = matrix_runtime

    def telemetry_collection_active_sync(device_id: str) -> bool:
        for session in database.active_sessions():
            metadata = _loads(session.get("metadata_json"), {})
            if metadata.get("device_id") != device_id:
                continue
            if "temperature" in set(metadata.get("active_kinds", [])):
                return True
        return False

    async def telemetry_collection_active(device_id: str) -> bool:
        return await asyncio.to_thread(telemetry_collection_active_sync, device_id)

    async def persist_realtime_telemetry(frame: TelemetryFrame) -> None:
        await asyncio.to_thread(
            persist_telemetry_record,
            frame.device_id,
            frame.ip_address,
            frame.payload,
            frame.received_at,
        )

    async def publish_realtime_telemetry(frame: TelemetryFrame) -> None:
        live_body = {**frame.payload, "received_at": frame.received_at}
        live_body = add_matrix_status(frame.device_id, live_body) or live_body
        if ai_runtime is not None and mobile_presence.connected(frame.device_id):
            ai_runtime.offer_telemetry(frame.device_id, live_body)
        await publish("telemetry", frame.device_id, live_body)

    telemetry_runtime = TelemetryRealtimeBroker(
        persist=persist_realtime_telemetry,
        publish=publish_realtime_telemetry,
        capture_active=telemetry_collection_active,
    )
    app.state.telemetry_runtime = telemetry_runtime

    def claim_command(device_id: str) -> dict[str, Any] | None:
        now = datetime.now(timezone.utc)
        command = database.claim_command(
            device_id,
            (now - timedelta(seconds=app_settings.command_redelivery_seconds)).isoformat(timespec="milliseconds"),
            now.isoformat(timespec="milliseconds"),
        )
        if not command:
            return None
        return {
            "id": command["id"],
            "delivery_attempt": command["delivery_attempts"],
            **_loads(command["command_json"], {}),
        }

    def persist_command_ack(device_id: str, command_id: int, payload: CommandAck) -> None:
        command = database.query_one("SELECT * FROM commands WHERE id=? AND device_id=?", (command_id, device_id))
        if not command:
            raise HTTPException(status_code=404, detail="command not found")
        database.execute(
            "UPDATE commands SET status=?, acked_at=?, result_json=? WHERE id=?",
            ("acked" if payload.ok else "failed", utc_now(), _json(payload.result), command_id),
        )

    async def store_command_ack(device_id: str, command_id: int, payload: CommandAck) -> None:
        await asyncio.to_thread(persist_command_ack, device_id, command_id, payload)
        await publish("command", device_id, {"id": command_id, "ok": payload.ok})
        if ai_runtime is not None:
            asyncio.create_task(
                ai_runtime.command_acked(device_id, command_id, payload.ok, payload.result)
            )

    @app.get("/")
    def index() -> FileResponse:
        return FileResponse(static_dir / "index.html", headers={"Cache-Control": "no-store"})

    @app.get("/health")
    def health() -> dict[str, Any]:
        return {
            "ok": True,
            "time": utc_now(),
            "version": app.version,
            "ai": ai_runtime.status() if ai_runtime is not None else {"enabled": False},
        }

    @app.get("/api/v1/ai/stories/{story_id}/audio")
    async def device_story_audio(story_id: str, request: Request) -> FileResponse:
        device_id = device_id_for(request)
        try:
            path, row = await ai_runtime.get_story_audio(device_id, story_id)
        except AiServiceError as error:
            raise HTTPException(status_code=404, detail=str(error)) from error
        return FileResponse(
            path,
            media_type="audio/wav",
            filename=path.name,
            headers={
                "Cache-Control": "private, max-age=3600",
                "X-Content-SHA256": str(row.get("audio_sha256", "")),
                "X-Content-CRC32": str(int(row.get("audio_crc32") or 0)),
            },
        )

    @app.post("/api/v1/device/hello")
    async def device_hello(request: Request, payload: DeviceHello) -> dict[str, Any]:
        device_id = device_id_for(request)
        return await store_hello(device_id, _request_ip(request), payload)

    @app.post("/api/v1/telemetry")
    async def ingest_telemetry(request: Request, payload: TelemetryUpload) -> dict[str, Any]:
        device_id = device_id_for(request)
        return await store_telemetry(device_id, _request_ip(request), payload)

    @app.post("/api/v1/batch")
    async def ingest_latest_batch(request: Request) -> dict[str, Any]:
        """Ingest one latest-only gateway snapshot in a single keep-alive request."""
        device_id = device_id_for(request)
        ip_address = _request_ip(request)
        raw = await request.body()
        if len(raw) < BATCH_HEADER_V1.size:
            raise HTTPException(status_code=400, detail="batch is shorter than its header")

        magic = raw[:4]
        version = raw[4]
        if magic != BATCH_MAGIC or version not in {1, 2}:
            raise HTTPException(status_code=400, detail="invalid batch header")
        if version == 1:
            magic, version, flags, json_length, pressure_sequence, thermal_sequence, \
                pressure_rows, pressure_cols, thermal_rows, thermal_cols = BATCH_HEADER_V1.unpack_from(raw)
            video_sequence = video_length = audio_sequence = audio_length = 0
            audio_sample_rate, audio_channels, audio_bits = 16000, 1, 16
            header_size = BATCH_HEADER_V1.size
            valid_flags = BATCH_PRESSURE | BATCH_THERMAL
        else:
            if len(raw) < BATCH_HEADER_V2.size:
                raise HTTPException(status_code=400, detail="batch is shorter than its v2 header")
            magic, version, flags, json_length, pressure_sequence, thermal_sequence, \
                pressure_rows, pressure_cols, thermal_rows, thermal_cols, video_sequence, video_length, \
                audio_sequence, audio_length, audio_sample_rate, audio_channels, audio_bits, \
                _reserved = BATCH_HEADER_V2.unpack_from(raw)
            header_size = BATCH_HEADER_V2.size
            valid_flags = BATCH_PRESSURE | BATCH_THERMAL | BATCH_VIDEO | BATCH_AUDIO
        if flags & ~valid_flags:
            raise HTTPException(status_code=400, detail="invalid batch flags")

        offset = header_size
        json_end = offset + json_length
        if json_end > len(raw):
            raise HTTPException(status_code=400, detail="truncated batch telemetry")
        try:
            telemetry = TelemetryUpload.model_validate_json(raw[offset:json_end])
        except ValidationError as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc
        offset = json_end

        result: dict[str, Any] = {
            "ok": True,
            "telemetry": await store_telemetry(device_id, ip_address, telemetry),
            "matrices": {},
        }
        matrix_specs = (
            (BATCH_PRESSURE, "pressure", pressure_sequence, pressure_rows, pressure_cols, "raw"),
            (BATCH_THERMAL, "thermal", thermal_sequence, thermal_rows, thermal_cols, "deci_c"),
        )
        for flag, kind, sequence, rows, cols, unit in matrix_specs:
            if not flags & flag:
                continue
            if not rows or not cols or rows > 128 or cols > 128:
                raise HTTPException(status_code=400, detail=f"invalid {kind} dimensions")
            byte_count = rows * cols * 2
            matrix_end = offset + byte_count
            if matrix_end > len(raw):
                raise HTTPException(status_code=400, detail=f"truncated {kind} matrix")
            result["matrices"][kind] = await store_matrix(
                device_id, ip_address, kind, sequence, rows, cols, unit, raw[offset:matrix_end]
            )
            offset = matrix_end
        if flags & BATCH_VIDEO:
            video_end = offset + video_length
            if not video_length or video_length > 512 * 1024 or video_end > len(raw):
                raise HTTPException(status_code=400, detail="invalid batch JPEG")
            await media.publish_video(device_id, video_sequence, "image/jpeg", raw[offset:video_end])
            result["video"] = {"sequence": video_sequence, "bytes": video_length}
            offset = video_end
        if flags & BATCH_AUDIO:
            audio_end = offset + audio_length
            if (
                not audio_length
                or audio_length > 128 * 1024
                or audio_length % 2
                or audio_end > len(raw)
                or not 8000 <= audio_sample_rate <= 48000
                or audio_channels not in {1, 2}
                or audio_bits != 16
            ):
                raise HTTPException(status_code=400, detail="invalid batch PCM audio")
            await media.publish_audio(
                device_id, audio_sequence, audio_sample_rate, audio_channels, audio_bits, raw[offset:audio_end]
            )
            result["audio"] = {"sequence": audio_sequence, "bytes": audio_length}
            offset = audio_end
        if offset != len(raw):
            raise HTTPException(status_code=400, detail="unexpected bytes after batch payload")
        result["command"] = claim_command(device_id)
        return result

    @app.post("/api/v1/events")
    async def ingest_event(request: Request, payload: EventUpload) -> dict[str, Any]:
        device_id = device_id_for(request)
        return await store_event(device_id, _request_ip(request), payload)

    @app.post("/api/v1/logs")
    async def ingest_log(request: Request, payload: LogUpload) -> dict[str, Any]:
        device_id = device_id_for(request)
        database.touch_device(device_id, ip_address=_request_ip(request))
        row_id = database.execute(
            """
            INSERT INTO device_logs(device_id, received_at, device_time_ms, level, component, message, context_json)
            VALUES(?, ?, ?, ?, ?, ?, ?)
            """,
            (
                device_id,
                utc_now(),
                payload.device_time_ms,
                payload.level,
                payload.component,
                payload.message,
                _json(payload.context),
            ),
        )
        await publish("log", device_id, {"id": row_id, **payload.model_dump()})
        return {"ok": True, "id": row_id}

    @app.post("/api/v1/matrices/{kind}")
    async def ingest_matrix(
        kind: str,
        request: Request,
        sequence: int = Query(ge=0),
        rows: int = Query(gt=0, le=128),
        cols: int = Query(gt=0, le=128),
        unit: str = Query(default="raw", max_length=24),
    ) -> dict[str, Any]:
        if kind not in MATRIX_KINDS:
            raise HTTPException(status_code=404, detail="unknown matrix kind")
        device_id = device_id_for(request)
        raw = await request.body()
        return await store_matrix(device_id, _request_ip(request), kind, sequence, rows, cols, unit, raw)

    @app.post("/api/v1/artifacts/{kind}")
    async def ingest_artifact(
        kind: str,
        request: Request,
        session_id: str | None = None,
        captured_at_ms: int = 0,
        metadata: str = "{}",
    ) -> dict[str, Any]:
        if not app_settings.allow_artifact_upload:
            raise HTTPException(status_code=403, detail="artifact upload disabled; store training data on SD card")
        device_id = device_id_for(request)
        raw = await request.body()
        if not raw or len(raw) > app_settings.max_artifact_bytes:
            raise HTTPException(status_code=413, detail="artifact is empty or too large")
        if session_id and not database.query_one("SELECT session_id FROM collection_sessions WHERE session_id=?", (session_id,)):
            raise HTTPException(status_code=404, detail="collection session not found")
        artifact_id = database.new_id("art")
        content_type = request.headers.get("content-type", "application/octet-stream").split(";", 1)[0]
        extension = mimetypes.guess_extension(content_type) or ".bin"
        relative = Path(device_id) / f"{artifact_id}{extension}"
        destination = app_settings.artifact_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
        digest = hashlib.sha256(raw).hexdigest()
        meta = _loads(metadata, {})
        captured_at = utc_now()
        database.execute(
            """
            INSERT INTO artifacts(artifact_id, session_id, device_id, kind, mime_type, relative_path,
                                  sha256, size_bytes, captured_at, metadata_json)
            VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (artifact_id, session_id, device_id, kind, content_type, str(relative), digest, len(raw), captured_at, _json(meta)),
        )
        if session_id:
            database.capture_sample(
                session_id,
                device_id,
                kind,
                f"artifact:{artifact_id}",
                {"artifact_id": artifact_id, "captured_at_ms": captured_at_ms, "sha256": digest, **meta},
            )
        await publish("artifact", device_id, {"artifact_id": artifact_id, "kind": kind, "size": len(raw)})
        return {"ok": True, "artifact_id": artifact_id, "sha256": digest, "size": len(raw)}

    @app.post("/api/v1/media/video")
    async def ingest_live_video(
        request: Request,
        sequence: int = Query(ge=0),
    ) -> dict[str, Any]:
        device_id = device_id_for(request)
        content_type = request.headers.get("content-type", "").split(";", 1)[0].lower()
        if content_type not in {"image/jpeg", "image/jpg"}:
            raise HTTPException(status_code=415, detail="live video must be JPEG")
        raw = await request.body()
        if not raw or len(raw) > 512 * 1024:
            raise HTTPException(status_code=413, detail="live JPEG is empty or too large")
        database.touch_device(device_id, ip_address=_request_ip(request))
        await media.publish_video(device_id, sequence, "image/jpeg", raw)
        return {"ok": True, "sequence": sequence, "bytes": len(raw)}

    @app.post("/api/v1/media/audio")
    async def ingest_live_audio(
        request: Request,
        sequence: int = Query(ge=0),
        sample_rate: int = Query(default=16000, ge=8000, le=48000),
        channels: int = Query(default=1, ge=1, le=2),
        bits: int = Query(default=16, ge=16, le=16),
    ) -> dict[str, Any]:
        device_id = device_id_for(request)
        raw = await request.body()
        if not raw or len(raw) > 128 * 1024 or len(raw) % 2:
            raise HTTPException(status_code=413, detail="invalid PCM audio chunk")
        database.touch_device(device_id, ip_address=_request_ip(request))
        await media.publish_audio(device_id, sequence, sample_rate, channels, bits, raw)
        return {"ok": True, "sequence": sequence, "bytes": len(raw)}

    @app.get("/api/v1/commands/poll")
    async def poll_command(request: Request) -> dict[str, Any]:
        device_id = device_id_for(request)
        database.touch_device(device_id, ip_address=_request_ip(request))
        command = claim_command(device_id)
        if not command:
            return {"ok": True, "command": None}
        return {"ok": True, "command": command}

    @app.post("/api/v1/commands/{command_id}/ack")
    async def ack_command(command_id: int, request: Request, payload: CommandAck) -> dict[str, Any]:
        device_id = device_id_for(request)
        await store_command_ack(device_id, command_id, payload)
        return {"ok": True}

    @app.websocket("/api/v1/device/stream")
    async def device_stream(websocket: WebSocket) -> None:
        device_id = websocket.headers.get("x-device-id", "")
        supplied_key = websocket.headers.get("x-device-key", "")
        expected_key = app_settings.device_keys.get(device_id)
        if not expected_key or not hmac.compare_digest(supplied_key, expected_key):
            await websocket.close(code=1008, reason="invalid device credentials")
            return

        ip_address = _websocket_ip(websocket)
        send_lock = asyncio.Lock()
        matrix_sequences: dict[str, int] = {}
        telemetry_sequence: int | None = None
        await websocket.accept()
        telemetry_runtime.connect(device_id)

        async def send_message(kind: str, payload: dict[str, Any]) -> None:
            async with send_lock:
                await websocket.send_json({"kind": kind, "payload": payload})

        async def receive_messages() -> None:
            nonlocal telemetry_sequence
            while True:
                message = await websocket.receive()
                if message["type"] == "websocket.disconnect":
                    raise WebSocketDisconnect(message.get("code", 1000))
                if message.get("text") is not None:
                    try:
                        envelope = json.loads(message["text"])
                        kind = envelope.get("kind")
                        payload_data = envelope.get("payload") or {}
                        if kind == "hello":
                            result = await store_hello(
                                device_id, ip_address, DeviceHello.model_validate(payload_data)
                            )
                            await send_message("hello_ack", result)
                        elif kind == "telemetry":
                            ack_started = time.perf_counter()
                            telemetry = TelemetryUpload.model_validate(payload_data)
                            body = telemetry.model_dump()
                            received_at = utc_now()
                            received_monotonic = time.monotonic()
                            missing, duplicate = sequence_gap(telemetry_sequence, telemetry.sequence)
                            if not duplicate:
                                telemetry_sequence = telemetry.sequence
                                telemetry_runtime.accept(
                                    TelemetryFrame(
                                        device_id=device_id,
                                        ip_address=ip_address,
                                        sequence=telemetry.sequence,
                                        payload=body,
                                        received_at=received_at,
                                        received_monotonic=received_monotonic,
                                    ),
                                    missing_frames=missing,
                                )
                            await send_message(
                                "telemetry_ack",
                                {"sequence": telemetry.sequence, "received_at": received_at},
                            )
                            telemetry_runtime.record_ack(
                                device_id,
                                (time.perf_counter() - ack_started) * 1000.0,
                            )
                        elif kind == "event":
                            result = await store_event(
                                device_id, ip_address, EventUpload.model_validate(payload_data)
                            )
                            await send_message(
                                "event_ack",
                                {"event_id": payload_data.get("event_id"), "duplicate": result["duplicate"]},
                            )
                        elif kind == "command_ack":
                            command_id = int(payload_data.get("id", 0))
                            ack = CommandAck.model_validate(payload_data)
                            await store_command_ack(device_id, command_id, ack)
                            await send_message("command_ack_received", {"id": command_id})
                        elif kind == "heartbeat":
                            await asyncio.to_thread(
                                database.touch_device, device_id, ip_address=ip_address
                            )
                            await send_message("heartbeat_ack", {"server_time": utc_now()})
                        else:
                            await send_message("error", {"message": f"unknown text frame kind: {kind}"})
                    except (json.JSONDecodeError, ValidationError, ValueError,
                            HTTPException, sqlite3.OperationalError) as exc:
                        await send_message("error", {"message": str(exc)})
                    continue

                raw = message.get("bytes")
                if raw is None:
                    continue
                ack_started = time.perf_counter()
                try:
                    frame = parse_matrix_frame(
                        raw,
                        device_id=device_id,
                        ip_address=ip_address,
                        strict_dimensions=False,
                    )
                    missing, duplicate = sequence_gap(matrix_sequences.get(frame.kind), frame.sequence)
                    if not duplicate:
                        matrix_sequences[frame.kind] = frame.sequence
                        matrix_runtime.accept(frame, missing_frames=missing)
                    await send_message(
                        "matrix_ack",
                        {
                            "kind": frame.kind,
                            "sequence": frame.sequence,
                            "received_at": frame.received_at,
                        },
                    )
                    matrix_runtime.record_ack(
                        frame.device_id,
                        frame.kind,
                        (time.perf_counter() - ack_started) * 1000.0,
                    )
                except (MatrixProtocolError, ValueError, HTTPException) as exc:
                    await send_message("error", {"message": str(exc)})

        async def push_commands() -> None:
            while True:
                try:
                    command = await asyncio.to_thread(claim_command, device_id)
                except Exception:
                    logger.exception("command polling failed for device %s", device_id)
                    await asyncio.sleep(1.0)
                    continue
                if command:
                    await send_message("command", command)
                await asyncio.sleep(0.5)

        receiver = asyncio.create_task(receive_messages())
        command_sender = asyncio.create_task(push_commands())
        close_code: int | None = None
        try:
            done, pending = await asyncio.wait(
                {receiver, command_sender}, return_when=asyncio.FIRST_COMPLETED
            )
            for task in done:
                task.result()
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
        except WebSocketDisconnect as exc:
            close_code = exc.code
        except asyncio.CancelledError:
            pass
        finally:
            await _cancel_tasks(receiver, command_sender)
            telemetry_runtime.disconnect(device_id, close_code)

    @app.websocket("/api/v1/device/matrices")
    async def device_matrix_stream(websocket: WebSocket) -> None:
        device_id = websocket.headers.get("x-device-id", "")
        supplied_key = websocket.headers.get("x-device-key", "")
        expected_key = app_settings.device_keys.get(device_id)
        if not expected_key or not hmac.compare_digest(supplied_key, expected_key):
            await websocket.close(code=1008, reason="invalid device credentials")
            return

        ip_address = _websocket_ip(websocket)
        sequences: dict[str, int] = {}
        await websocket.accept()
        matrix_runtime.connect(device_id)
        await publish("matrix_status", device_id, matrix_runtime.status(device_id))
        close_code: int | None = None
        try:
            while True:
                message = await websocket.receive()
                if message["type"] == "websocket.disconnect":
                    raise WebSocketDisconnect(message.get("code", 1000))
                raw = message.get("bytes")
                if raw is None:
                    await websocket.send_json(
                        {"kind": "error", "payload": {"message": "matrix endpoint accepts binary frames only"}}
                    )
                    close_code = 1003
                    await websocket.close(code=1003, reason="binary matrix frame required")
                    return
                ack_started = time.perf_counter()
                try:
                    frame = parse_matrix_frame(
                        raw,
                        device_id=device_id,
                        ip_address=ip_address,
                        strict_dimensions=True,
                    )
                    missing, duplicate = sequence_gap(sequences.get(frame.kind), frame.sequence)
                    if not duplicate:
                        sequences[frame.kind] = frame.sequence
                        matrix_runtime.accept(frame, missing_frames=missing)
                    await websocket.send_json(
                        {
                            "kind": "matrix_ack",
                            "payload": {
                                "kind": frame.kind,
                                "sequence": frame.sequence,
                                "received_at": frame.received_at,
                            },
                        }
                    )
                    matrix_runtime.record_ack(
                        frame.device_id,
                        frame.kind,
                        (time.perf_counter() - ack_started) * 1000.0,
                    )
                except MatrixProtocolError as exc:
                    matrix_runtime.note_invalid(device_id, str(exc))
                    await websocket.send_json({"kind": "error", "payload": {"message": str(exc)}})
                    close_code = 1003
                    await websocket.close(code=1003, reason="invalid matrix frame")
                    return
        except WebSocketDisconnect as exc:
            close_code = exc.code
        except (RuntimeError, asyncio.CancelledError):
            pass
        finally:
            matrix_runtime.disconnect(device_id, close_code)
            await publish("matrix_status", device_id, matrix_runtime.status(device_id))

    @app.websocket("/api/v1/media/device/video")
    async def device_video_stream(websocket: WebSocket) -> None:
        device_id = websocket.headers.get("x-device-id", "")
        supplied_key = websocket.headers.get("x-device-key", "")
        expected_key = app_settings.device_keys.get(device_id)
        if not expected_key or not hmac.compare_digest(supplied_key, expected_key):
            await websocket.close(code=1008, reason="invalid device credentials")
            return

        ip_address = _websocket_ip(websocket)
        send_lock = asyncio.Lock()
        await websocket.accept()
        controls = await media.register_video_uploader(device_id)
        await asyncio.to_thread(database.touch_device, device_id, ip_address=ip_address)

        async def send_json(payload: dict[str, Any]) -> None:
            async with send_lock:
                await websocket.send_json(payload)

        async def receive_video_frames() -> None:
            nonlocal close_code
            while True:
                message = await websocket.receive()
                if message["type"] == "websocket.disconnect":
                    close_code = message.get("code", 1000)
                    raise WebSocketDisconnect(message.get("code", 1000))

                raw = message.get("bytes")
                if raw is not None:
                    try:
                        frame = decode_s3_video_frame(raw)
                    except MediaProtocolError as exc:
                        media.record_video_error(device_id, str(exc))
                        await send_json({
                            "type": "media.error",
                            "version": 1,
                            "message": str(exc),
                        })
                        close_code = 1003
                        await websocket.close(code=close_code, reason=str(exc)[:120])
                        return
                    await media.publish_s3_video(device_id, frame)
                    continue

                text = message.get("text")
                if text is None:
                    continue
                try:
                    payload = json.loads(text)
                except json.JSONDecodeError:
                    await send_json({"type": "media.error", "version": 1, "message": "invalid JSON control frame"})
                    continue
                if payload.get("type") == "media.heartbeat":
                    await asyncio.to_thread(
                        database.touch_device, device_id, ip_address=ip_address
                    )
                    await send_json({
                        "type": "media.heartbeat_ack",
                        "version": 1,
                        "sequence": payload.get("sequence"),
                        "server_time": utc_now(),
                    })
                else:
                    await send_json({"type": "media.error", "version": 1, "message": "unknown device text frame"})

        async def push_controls() -> None:
            while True:
                await send_json(await media.next_video_control(device_id, controls))

        close_code: int | None = None
        receiver = asyncio.create_task(receive_video_frames())
        control_sender = asyncio.create_task(push_controls())
        try:
            done, pending = await asyncio.wait(
                {receiver, control_sender}, return_when=asyncio.FIRST_COMPLETED
            )
            for task in done:
                task.result()
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
        except WebSocketDisconnect as exc:
            close_code = exc.code
        except (RuntimeError, asyncio.CancelledError):
            pass
        finally:
            await _cancel_tasks(receiver, control_sender)
            media.unregister_video_uploader(device_id, controls, close_code)

    @app.websocket("/api/v1/media/device/audio")
    async def device_audio_stream(websocket: WebSocket) -> None:
        device_id = websocket.headers.get("x-device-id", "")
        supplied_key = websocket.headers.get("x-device-key", "")
        expected_key = app_settings.device_keys.get(device_id)
        if not expected_key or not hmac.compare_digest(supplied_key, expected_key):
            await websocket.close(code=1008, reason="invalid device credentials")
            return

        ip_address = _websocket_ip(websocket)
        send_lock = asyncio.Lock()
        await websocket.accept()
        controls = await media.register_audio_uploader(device_id)
        await asyncio.to_thread(database.touch_device, device_id, ip_address=ip_address)

        async def send_json(payload: dict[str, Any]) -> None:
            async with send_lock:
                await websocket.send_json(payload)

        async def receive_audio_frames() -> None:
            nonlocal close_code
            while True:
                message = await websocket.receive()
                if message["type"] == "websocket.disconnect":
                    close_code = message.get("code", 1000)
                    raise WebSocketDisconnect(message.get("code", 1000))

                raw = message.get("bytes")
                if raw is not None:
                    try:
                        frame = decode_s3_audio_frame(raw)
                    except MediaProtocolError as exc:
                        media.record_audio_error(device_id, str(exc))
                        await send_json({
                            "type": "media.error",
                            "version": 1,
                            "message": str(exc),
                        })
                        close_code = 1003
                        await websocket.close(code=close_code, reason=str(exc)[:120])
                        return
                    await media.publish_s3_audio(device_id, frame)
                    continue

                text = message.get("text")
                if text is None:
                    continue
                try:
                    payload = json.loads(text)
                except json.JSONDecodeError:
                    await send_json({"type": "media.error", "version": 1, "message": "invalid JSON control frame"})
                    continue
                if payload.get("type") == "media.heartbeat":
                    await asyncio.to_thread(
                        database.touch_device, device_id, ip_address=ip_address
                    )
                    await send_json({
                        "type": "media.heartbeat_ack",
                        "version": 1,
                        "sequence": payload.get("sequence"),
                        "server_time": utc_now(),
                    })
                else:
                    await send_json({"type": "media.error", "version": 1, "message": "unknown device text frame"})

        async def push_controls() -> None:
            while True:
                await send_json(await media.next_audio_control(device_id, controls))

        close_code: int | None = None
        receiver = asyncio.create_task(receive_audio_frames())
        control_sender = asyncio.create_task(push_controls())
        try:
            done, pending = await asyncio.wait(
                {receiver, control_sender}, return_when=asyncio.FIRST_COMPLETED
            )
            for task in done:
                task.result()
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
        except WebSocketDisconnect as exc:
            close_code = exc.code
        except (RuntimeError, asyncio.CancelledError):
            pass
        finally:
            await _cancel_tasks(receiver, control_sender)
            media.unregister_audio_uploader(device_id, controls, close_code)

    def mobile_device_row(device_id: str) -> dict[str, Any]:
        device = database.query_one("SELECT * FROM devices WHERE device_id=?", (device_id,))
        if not device:
            raise HTTPException(status_code=404, detail="device not found")
        live_telemetry = telemetry_runtime.latest(device_id)
        if live_telemetry is not None:
            device["last_seen"] = live_telemetry["received_at"]
        device["online"] = _online(device["last_seen"], app_settings.offline_after_seconds)
        decode_json_columns(device, "capabilities_json", "metadata_json")
        device["matrix_status"] = matrix_status_for(device_id)
        device["matrix_connected"] = device["matrix_status"]["matrix_connected"]
        return device

    def mobile_latest_data(device_id: str) -> tuple[dict[str, Any] | None, dict[str, Any]]:
        telemetry = telemetry_runtime.latest(device_id)
        if telemetry is None:
            row = database.query_one(
                "SELECT * FROM telemetry WHERE device_id=? ORDER BY id DESC LIMIT 1",
                (device_id,),
            )
            if row:
                telemetry = normalize_telemetry_bme688(_loads(row["payload_json"], {}))
                telemetry["received_at"] = row["received_at"]
        telemetry = add_matrix_status(device_id, telemetry)

        matrices: dict[str, Any] = {}
        for kind in sorted(MATRIX_KINDS):
            live_matrix = matrix_runtime.latest(device_id, kind)
            if live_matrix is not None:
                matrices[kind] = live_matrix
                continue
            matrix = database.query_one(
                "SELECT * FROM matrices WHERE device_id=? AND kind=? ORDER BY id DESC LIMIT 1",
                (device_id, kind),
            )
            if matrix:
                count = matrix["rows"] * matrix["cols"]
                matrix["values"] = list(struct.unpack(f"<{count}h", matrix.pop("data")))
                matrices[kind] = matrix

        if telemetry and "pressure" not in matrices:
            pressure = telemetry.get("sensors", {}).get("pressure", {})
            values = pressure.get("matrix") or pressure.get("raw")
            if isinstance(values, list) and len(values) >= 256:
                pressure_values = values[:256]
                matrices["pressure"] = {
                    "kind": "pressure",
                    "sequence": pressure.get("seq", 0),
                    "rows": 16,
                    "cols": 16,
                    "unit": "raw",
                    "received_at": telemetry.get("received_at"),
                    "minimum": min(pressure_values),
                    "maximum": max(pressure_values),
                    "values": pressure_values,
                }
        return telemetry, matrices

    def mobile_snapshot_for(device_id: str) -> dict[str, Any]:
        device = mobile_device_row(device_id)
        telemetry, matrices = mobile_latest_data(device_id)
        calibration, vision_overlay, fusion_view = overlay_views(device_id, telemetry)
        diagnostics = build_sensor_diagnostics(
            telemetry,
            matrices,
            device_online=bool(device["online"]),
        )
        return build_mobile_snapshot(
            server_time=utc_now(),
            device=device,
            telemetry=telemetry,
            matrices=matrices,
            diagnostics=diagnostics,
            media_status=media.status(device_id),
            fusion=fusion_view,
            vision_overlay=vision_overlay,
            overlay_calibration=calibration,
        )

    @app.get("/api/dashboard/devices")
    def list_devices() -> list[dict[str, Any]]:
        rows = database.query_all("SELECT * FROM devices ORDER BY last_seen DESC")
        for row in rows:
            live_telemetry = telemetry_runtime.latest(row["device_id"])
            if live_telemetry is not None:
                row["last_seen"] = live_telemetry["received_at"]
            row["online"] = _online(row["last_seen"], app_settings.offline_after_seconds)
            decode_json_columns(row, "capabilities_json", "metadata_json")
            row["matrix_status"] = matrix_status_for(row["device_id"])
            row["matrix_connected"] = row["matrix_status"]["matrix_connected"]
        return rows

    @app.get("/api/dashboard/devices/{device_id}/matrix-status")
    def matrix_status(device_id: str) -> dict[str, object]:
        mobile_device_row(device_id)
        return {"device_id": device_id, **matrix_status_for(device_id)}

    @app.get("/api/dashboard/devices/{device_id}/live-state")
    def dashboard_live_state(device_id: str) -> dict[str, Any]:
        """Return volatile dashboard state without history or database lists."""
        device = mobile_device_row(device_id)
        telemetry, matrices = mobile_latest_data(device_id)
        return {
            "server_time": utc_now(),
            "device_id": device_id,
            "device": {
                "online": bool(device["online"]),
                "last_seen": device.get("last_seen"),
            },
            "telemetry_status": telemetry_status_for(device_id),
            "matrix_status": matrix_status_for(device_id),
            "sensor_diagnostics": build_sensor_diagnostics(
                telemetry,
                matrices,
                device_online=bool(device["online"]),
            ),
            "resource_recording": resource_recording_session(device_id),
        }

    @app.get("/api/dashboard/devices/{device_id}/transport-status")
    def transport_status(device_id: str) -> dict[str, Any]:
        mobile_device_row(device_id)
        return {
            "device_id": device_id,
            "telemetry": telemetry_status_for(device_id),
            "matrices": matrix_status_for(device_id),
            "media": media.status(device_id),
        }

    @app.get("/api/dashboard/devices/{device_id}/overlay-calibration")
    def get_overlay_calibration(device_id: str) -> dict[str, Any]:
        mobile_device_row(device_id)
        calibration = overlay_calibration_for(device_id)
        telemetry, _ = mobile_latest_data(device_id)
        return {
            "device_id": device_id,
            "calibration": calibration,
            "vision_overlay": build_vision_overlay(telemetry, calibration),
        }

    @app.get("/api/dashboard/devices/{device_id}/environment-thresholds")
    def get_environment_thresholds(device_id: str) -> dict[str, Any]:
        mobile_device_row(device_id)
        telemetry, _ = mobile_latest_data(device_id)
        desired = environment_thresholds_for(device_id)
        active = hardware_thresholds_from_telemetry(telemetry or {})
        return {
            "device_id": device_id,
            "thresholds": active or desired,
            "desired_thresholds": desired,
            "hardware_synced": thresholds_match_hardware(desired, active),
            "evaluation": evaluate_environment(telemetry or {}, desired),
        }

    @app.put("/api/dashboard/devices/{device_id}/environment-thresholds")
    def update_environment_thresholds(
        device_id: str, payload: EnvironmentThresholdUpdate
    ) -> dict[str, Any]:
        mobile_device_row(device_id)
        updated_at = utc_now()
        values = payload.model_dump()
        database.execute(
            """
            INSERT INTO environment_thresholds(
                device_id, co2_max_ppm, temperature_max_milli_c,
                humidity_max_milli_percent, gas_min_ohm,
                body_temperature_max_deci_c, humidity_min_milli_percent, updated_at
            ) VALUES(?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                co2_max_ppm=excluded.co2_max_ppm,
                temperature_max_milli_c=excluded.temperature_max_milli_c,
                humidity_max_milli_percent=excluded.humidity_max_milli_percent,
                humidity_min_milli_percent=excluded.humidity_min_milli_percent,
                gas_min_ohm=excluded.gas_min_ohm,
                body_temperature_max_deci_c=excluded.body_temperature_max_deci_c,
                updated_at=excluded.updated_at
            """,
            (
                device_id,
                values["co2_max_ppm"],
                values["temperature_max_milli_c"],
                values["humidity_max_milli_percent"],
                values["gas_min_ohm"],
                values["body_temperature_max_deci_c"],
                values["humidity_min_milli_percent"],
                updated_at,
            ),
        )
        thresholds = {**values, "updated_at": updated_at}
        environment_threshold_cache[device_id] = thresholds
        command = enqueue_environment_threshold_sync(device_id, thresholds)
        telemetry, _ = mobile_latest_data(device_id)
        active = hardware_thresholds_from_telemetry(telemetry or {})
        return {
            "device_id": device_id,
            "thresholds": thresholds,
            "desired_thresholds": thresholds,
            "hardware_synced": thresholds_match_hardware(thresholds, active),
            "command_id": command["id"],
            "evaluation": evaluate_environment(telemetry or {}, thresholds),
        }

    @app.put("/api/dashboard/devices/{device_id}/overlay-calibration")
    def update_overlay_calibration(
        device_id: str,
        payload: OverlayCalibrationUpdate,
    ) -> dict[str, Any]:
        mobile_device_row(device_id)
        try:
            calibration = prepare_calibration(
                enabled=payload.enabled,
                mode=payload.mode,
                source_points=payload.source_points,
                target_points=payload.target_points,
            )
            calibration["thermal"] = prepare_thermal_calibration(
                enabled=payload.thermal_enabled,
                source_points=payload.thermal_source_points,
                target_points=payload.thermal_target_points,
                opacity=payload.thermal_opacity,
            )
        except OverlayCalibrationError as error:
            raise HTTPException(status_code=422, detail=str(error)) from error
        database.execute(
            """
            INSERT INTO overlay_calibrations(
                device_id, enabled, mode, matrix_json, source_points_json,
                target_points_json, thermal_enabled, thermal_matrix_json,
                thermal_source_points_json, thermal_target_points_json,
                thermal_opacity, updated_at
            ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                enabled=excluded.enabled,
                mode=excluded.mode,
                matrix_json=excluded.matrix_json,
                source_points_json=excluded.source_points_json,
                target_points_json=excluded.target_points_json,
                thermal_enabled=excluded.thermal_enabled,
                thermal_matrix_json=excluded.thermal_matrix_json,
                thermal_source_points_json=excluded.thermal_source_points_json,
                thermal_target_points_json=excluded.thermal_target_points_json,
                thermal_opacity=excluded.thermal_opacity,
                updated_at=excluded.updated_at
            """,
            (
                device_id,
                1 if calibration["enabled"] else 0,
                calibration["mode"],
                _json(calibration["matrix"]),
                _json(calibration["source_points"]),
                _json(calibration["target_points"]),
                1 if calibration["thermal"]["enabled"] else 0,
                _json(calibration["thermal"]["matrix"]),
                _json(calibration["thermal"]["source_points"]),
                _json(calibration["thermal"]["target_points"]),
                calibration["thermal"]["opacity"],
                calibration["updated_at"],
            ),
        )
        telemetry, _ = mobile_latest_data(device_id)
        return {
            "device_id": device_id,
            "calibration": calibration,
            "vision_overlay": build_vision_overlay(telemetry, calibration),
        }

    @app.get("/api/dashboard/snapshot")
    def dashboard_snapshot(device_id: str | None = None) -> dict[str, Any]:
        devices = list_devices()
        selected = device_id or (devices[0]["device_id"] if devices else None)
        telemetry: dict[str, Any] | None = None
        matrices: dict[str, Any] = {}
        if selected:
            telemetry, matrices = mobile_latest_data(selected)
        events = database.query_all(
            "SELECT * FROM events WHERE (? IS NULL OR device_id=?) ORDER BY id DESC LIMIT 60",
            (selected, selected),
        )
        for row in events:
            decode_json_columns(row, "payload_json")
        logs = database.query_all(
            "SELECT * FROM device_logs WHERE (? IS NULL OR device_id=?) ORDER BY id DESC LIMIT 100",
            (selected, selected),
        )
        for row in logs:
            decode_json_columns(row, "context_json")
        commands = database.query_all(
            "SELECT * FROM commands WHERE (? IS NULL OR device_id=?) ORDER BY id DESC LIMIT 40",
            (selected, selected),
        )
        for row in commands:
            decode_json_columns(row, "command_json", "result_json")
        sessions = database.query_all(
            """
            SELECT s.*, COUNT(p.id) AS sample_count,
                   COALESCE(SUM(CASE WHEN p.label IS NOT NULL AND p.label != '' THEN 1 ELSE 0 END), 0) AS labeled_count,
                   COALESCE(SUM(CASE WHEN p.source_ref LIKE 'artifact:%' THEN 1 ELSE 0 END), 0) AS imported_artifact_count,
                   COALESCE(SUM(CASE WHEN p.source_ref LIKE 'sd:%' THEN 1 ELSE 0 END), 0) AS pending_artifact_count,
                   COALESCE(SUM(CASE WHEN p.split='train' THEN 1 ELSE 0 END), 0) AS train_count,
                   COALESCE(SUM(CASE WHEN p.split='validation' THEN 1 ELSE 0 END), 0) AS validation_count,
                   COALESCE(SUM(CASE WHEN p.split='test' THEN 1 ELSE 0 END), 0) AS test_count
            FROM collection_sessions s LEFT JOIN samples p ON p.session_id=s.session_id
            GROUP BY s.session_id ORDER BY s.created_at DESC LIMIT 30
            """
        )
        for row in sessions:
            decode_json_columns(row, "target_labels_json", "metadata_json")
        models = database.query_all("SELECT * FROM model_packages ORDER BY created_at DESC")
        for row in models:
            decode_json_columns(row, "metrics_json", "metadata_json")
        selected_device = next((device for device in devices if device["device_id"] == selected), None)
        calibration, vision_overlay, fusion_view = (
            overlay_views(selected, telemetry)
            if selected
            else (calibration_from_row(None), build_vision_overlay(None, calibration_from_row(None)), build_fusion_view(None))
        )
        return {
            "server_time": utc_now(),
            "selected_device": selected,
            "devices": devices,
            "telemetry": telemetry,
            "matrices": matrices,
            "events": events,
            "logs": logs,
            "commands": commands,
            "sessions": sessions,
            "models": models,
            "media": media.status(selected) if selected else {},
            "telemetry_status": telemetry_status_for(selected) if selected else {},
            "matrix_status": matrix_status_for(selected) if selected else {},
            "fusion_view": fusion_view,
            "vision_overlay": vision_overlay,
            "overlay_calibration": calibration,
            "environment_policy": (
                {
                    "thresholds": (
                        hardware_thresholds_from_telemetry(telemetry or {})
                        or environment_thresholds_for(selected)
                    ),
                    "desired_thresholds": environment_thresholds_for(selected),
                    "hardware_synced": thresholds_match_hardware(
                        environment_thresholds_for(selected),
                        hardware_thresholds_from_telemetry(telemetry or {}),
                    ),
                    "evaluation": evaluate_environment(
                        telemetry or {}, environment_thresholds_for(selected)
                    ),
                }
                if selected
                else {}
            ),
            "sensor_diagnostics": build_sensor_diagnostics(
                telemetry,
                matrices,
                device_online=bool(selected_device and selected_device["online"]),
            ),
            "resource_recording": resource_recording_session(selected) if selected else None,
        }

    @app.get("/api/mobile/v1/capabilities")
    def mobile_capabilities() -> dict[str, Any]:
        return {
            "type": "mobile.capabilities",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "server_time": utc_now(),
            "authentication": {
                "scheme": "basic",
                "required": bool(app_settings.admin_user),
                "tls_required": True,
            },
            "sensor_stream": {"transport": "sse", "heartbeat_seconds": 15},
            "environment_sensors": {
                "bme688": {
                    "path": "telemetry.sensors.environment.bme688",
                    "units": {
                        "t_mc": "milli_celsius",
                        "rh_mp": "milli_percent_rh",
                        "p_pa": "pascal",
                        "gas_ohm": "ohm",
                    },
                    "gas_semantics": "raw_resistance_only",
                    "bsec": False,
                }
            },
            "actuators": {
                "warm_light": {
                    "state_path": "telemetry.sensors.warm_light",
                    "command_action": "light.set",
                    "control": "switch",
                    "label": "暖光灯",
                    "boot_default": "off",
                    "active_high": True,
                    "pin": "P711",
                    "recommendation_path": "fusion.comfort.recommended_light",
                    "requires_user_confirmation": True,
                    "confirmation_timeout_ms": 8000,
                },
                "soother_pat": {
                    "state_path": "telemetry.sensors.soother",
                    "command_action": "soother.set",
                    "control": "switch",
                    "label": "拍一拍电机",
                    "mode": "maintained",
                    "boot_default": "off",
                    "active_high": True,
                    "pin": "P914",
                    "physical_feedback": False,
                    "recommendation_path": "fusion.comfort.recommend_pat",
                    "blocked_reasons_path": "fusion.comfort.blocked_reasons",
                    "requires_user_confirmation": True,
                    "confirmation_value_path": "telemetry.sensors.soother.on",
                    "confirmation_timeout_ms": 8000,
                },
                "comfort_recommendation": {
                    "path": "fusion.comfort",
                    "advisory_only": True,
                    "never_auto_execute": True,
                },
            },
            "media": {
                "transport": "websocket",
                "frame_protocol": "S3MD/1",
                "video": "JPEG",
                "audio": "PCM16LE 16000 Hz mono",
            },
            "matrix_kinds": sorted(MATRIX_KINDS),
            "ai": {
                "enabled": ai_runtime.enabled if ai_runtime is not None else False,
                "story_tts": {
                    "languages": ["zh-CN"],
                    "duration_seconds": {"minimum": 30, "maximum": 240},
                    "autoplay": True,
                },
                "telemetry_advisory": {
                    "levels": ["normal", "attention", "urgent"],
                    "advisory_only": True,
                    "local_safety_remains_authoritative": True,
                    "automatic_requires_mobile_stream": True,
                },
            },
            "commands": list(MOBILE_COMMAND_DEFINITIONS),
        }

    @app.get("/api/dashboard/devices/{device_id}/ai")
    async def dashboard_ai(device_id: str) -> dict[str, Any]:
        mobile_device_row(device_id)
        return {
            "device_id": device_id,
            "status": ai_runtime.status(),
            "stories": await ai_runtime.list_stories(device_id, 20),
            "advisories": await ai_runtime.list_advisories(device_id, 30),
        }

    @app.post("/api/dashboard/devices/{device_id}/ai/stories")
    async def dashboard_create_story(device_id: str, payload: StoryCreate) -> dict[str, Any]:
        mobile_device_row(device_id)
        try:
            story, duplicate = await ai_runtime.create_story(device_id, payload.model_dump())
        except AiServiceError as error:
            status = 503 if not ai_runtime.enabled else 429
            raise HTTPException(status_code=status, detail=str(error)) from error
        return {"device_id": device_id, "duplicate": duplicate, "story": story}

    @app.post("/api/dashboard/devices/{device_id}/ai/stories/{story_id}/replay")
    async def dashboard_replay_story(device_id: str, story_id: str) -> dict[str, Any]:
        mobile_device_row(device_id)
        try:
            story = await ai_runtime.replay_story(device_id, story_id)
        except AiServiceError as error:
            raise HTTPException(status_code=409, detail=str(error)) from error
        return {"device_id": device_id, "story": story}

    @app.post("/api/dashboard/devices/{device_id}/ai/assessment")
    async def dashboard_request_assessment(device_id: str) -> dict[str, Any]:
        mobile_device_row(device_id)
        telemetry, _ = mobile_latest_data(device_id)
        try:
            result = ai_runtime.request_assessment(device_id, telemetry)
        except AiServiceError as error:
            raise HTTPException(status_code=503, detail=str(error)) from error
        return {"device_id": device_id, **result}

    @app.post("/api/mobile/v1/devices/{device_id}/stories")
    async def mobile_create_story(device_id: str, payload: StoryCreate) -> dict[str, Any]:
        mobile_device_row(device_id)
        try:
            story, duplicate = await ai_runtime.create_story(device_id, payload.model_dump())
        except AiServiceError as error:
            status = 503 if not ai_runtime.enabled else 429
            raise HTTPException(status_code=status, detail=str(error)) from error
        return {
            "type": "mobile.ai_story",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "duplicate": duplicate,
            "story": story,
        }

    @app.get("/api/mobile/v1/devices/{device_id}/stories")
    async def mobile_stories(
        device_id: str,
        limit: int = Query(default=20, ge=1, le=100),
    ) -> dict[str, Any]:
        mobile_device_row(device_id)
        return {
            "type": "mobile.ai_stories",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "server_time": utc_now(),
            "device_id": device_id,
            "stories": await ai_runtime.list_stories(device_id, limit),
        }

    @app.get("/api/mobile/v1/devices/{device_id}/stories/{story_id}")
    async def mobile_story(device_id: str, story_id: str) -> dict[str, Any]:
        mobile_device_row(device_id)
        story = await ai_runtime.get_story(device_id, story_id)
        if not story:
            raise HTTPException(status_code=404, detail="story not found")
        return {
            "type": "mobile.ai_story",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "story": story,
        }

    @app.post("/api/mobile/v1/devices/{device_id}/stories/{story_id}/replay")
    async def mobile_replay_story(device_id: str, story_id: str) -> dict[str, Any]:
        mobile_device_row(device_id)
        try:
            story = await ai_runtime.replay_story(device_id, story_id)
        except AiServiceError as error:
            raise HTTPException(status_code=409, detail=str(error)) from error
        return {
            "type": "mobile.ai_story",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "story": story,
        }

    @app.get("/api/mobile/v1/devices/{device_id}/advisories")
    async def mobile_advisories(
        device_id: str,
        limit: int = Query(default=50, ge=1, le=200),
    ) -> dict[str, Any]:
        mobile_device_row(device_id)
        return {
            "type": "mobile.ai_advisories",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "server_time": utc_now(),
            "device_id": device_id,
            "advisories": await ai_runtime.list_advisories(device_id, limit),
        }

    @app.post("/api/mobile/v1/devices/{device_id}/advisories/{advisory_id}/ack")
    async def mobile_acknowledge_advisory(device_id: str, advisory_id: str) -> dict[str, Any]:
        mobile_device_row(device_id)
        if not await ai_runtime.acknowledge_advisory(device_id, advisory_id):
            raise HTTPException(status_code=404, detail="advisory not found")
        await publish("ai_advisory_ack", device_id, {"advisory_id": advisory_id})
        return {
            "type": "mobile.ai_advisory_ack",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "ok": True,
            "device_id": device_id,
            "advisory_id": advisory_id,
        }

    @app.get("/api/mobile/v1/devices")
    def mobile_devices() -> dict[str, Any]:
        return {
            "type": "mobile.devices",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "server_time": utc_now(),
            "devices": [mobile_device(device) for device in list_devices()],
        }

    @app.get("/api/mobile/v1/devices/{device_id}/snapshot")
    def mobile_snapshot(device_id: str) -> dict[str, Any]:
        return mobile_snapshot_for(device_id)

    @app.get("/api/mobile/v1/devices/{device_id}/matrices/{kind}")
    def mobile_matrix(device_id: str, kind: str) -> dict[str, Any]:
        mobile_device_row(device_id)
        if kind not in MATRIX_KINDS:
            raise HTTPException(status_code=404, detail="unknown matrix kind")
        _, matrices = mobile_latest_data(device_id)
        matrix = matrices.get(kind)
        if not matrix:
            raise HTTPException(status_code=404, detail="matrix not available")
        return {
            "type": "mobile.matrix",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "server_time": utc_now(),
            "device_id": device_id,
            "matrix": matrix,
        }

    @app.get("/api/mobile/v1/devices/{device_id}/events")
    def mobile_events(
        device_id: str,
        limit: int = Query(default=50, ge=1, le=200),
    ) -> dict[str, Any]:
        mobile_device_row(device_id)
        rows = database.query_all(
            "SELECT * FROM events WHERE device_id=? ORDER BY id DESC LIMIT ?",
            (device_id, limit),
        )
        for row in rows:
            decode_json_columns(row, "payload_json")
        return {
            "type": "mobile.events",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "server_time": utc_now(),
            "device_id": device_id,
            "events": rows,
        }

    @app.post("/api/mobile/v1/devices/{device_id}/events/{event_id}/ack")
    async def mobile_acknowledge_event(device_id: str, event_id: int) -> dict[str, Any]:
        event = database.query_one(
            "SELECT id FROM events WHERE id=? AND device_id=?",
            (event_id, device_id),
        )
        if not event:
            raise HTTPException(status_code=404, detail="event not found")
        database.execute("UPDATE events SET acknowledged=1 WHERE id=?", (event_id,))
        await broker.publish({
            "kind": "event_ack",
            "device_id": device_id,
            "at": utc_now(),
            "data": {"id": event_id},
        })
        return {
            "type": "mobile.event_ack",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "ok": True,
            "device_id": device_id,
            "event_id": event_id,
        }

    @app.post("/api/mobile/v1/devices/{device_id}/commands")
    async def mobile_create_command(device_id: str, payload: MobileCommandCreate) -> dict[str, Any]:
        mobile_device_row(device_id)
        try:
            mapped = map_mobile_command(payload.action, payload.args)
        except MobileCommandError as error:
            raise HTTPException(status_code=422, detail=str(error)) from error

        action = payload.action.strip().lower()
        request_id = payload.request_id.strip() if payload.request_id else None
        if payload.request_id is not None and not request_id:
            raise HTTPException(status_code=422, detail="request_id must not be blank")
        stored_command = {**mapped, "mobile_action": action}
        row, duplicate = database.enqueue_command(
            device_id,
            _json(stored_command),
            utc_now(),
            request_id,
        )
        stored = _loads(row.get("command_json"), {})
        command = mobile_command_view(row, stored)
        if not duplicate:
            await publish("command", device_id, {"id": row["id"], "status": "queued", "action": action})
        return {
            "type": "mobile.command",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "duplicate": duplicate,
            "command": command,
        }

    @app.get("/api/mobile/v1/devices/{device_id}/commands/{command_id}")
    def mobile_get_command(device_id: str, command_id: int) -> dict[str, Any]:
        row = database.query_one(
            "SELECT * FROM commands WHERE id=? AND device_id=?",
            (command_id, device_id),
        )
        if not row:
            raise HTTPException(status_code=404, detail="command not found")
        stored = _loads(row.get("command_json"), {})
        result = _loads(row.get("result_json"), None)
        row["result"] = result
        return {
            "type": "mobile.command",
            "protocol_version": MOBILE_PROTOCOL_VERSION,
            "command": mobile_command_view(row, stored),
        }

    @app.get("/api/dashboard/history")
    def dashboard_history(
        device_id: str,
        limit: int = Query(default=180, ge=10, le=1000),
    ) -> list[dict[str, Any]]:
        rows = database.query_all(
            "SELECT sequence, received_at, device_time_ms, ra8_tick, payload_json "
            "FROM telemetry WHERE device_id=? ORDER BY id DESC LIMIT ?",
            (device_id, limit),
        )
        return [
            {
                "received_at": row["received_at"],
                **normalize_telemetry_bme688(_loads(row["payload_json"], {})),
            }
            for row in reversed(rows)
        ]

    @app.get("/api/dashboard/media/{device_id}/video")
    async def dashboard_video(device_id: str) -> StreamingResponse:
        async def generate():
            async for frame in media.subscribe_video(device_id):
                yield (
                    b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                    + str(len(frame.data)).encode("ascii")
                    + b"\r\n\r\n"
                    + frame.data
                    + b"\r\n"
                )

        return StreamingResponse(
            generate(),
            media_type="multipart/x-mixed-replace; boundary=frame",
            headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"},
        )

    @app.websocket("/api/mobile/v1/devices/{device_id}/media/video/ws")
    @app.websocket("/api/dashboard/media/{device_id}/video/ws")
    async def dashboard_video_websocket(websocket: WebSocket, device_id: str) -> None:
        if app_settings.admin_user and not _basic_auth_valid(
            websocket.headers.get("authorization", ""),
            app_settings.admin_user,
            app_settings.admin_password,
        ):
            await websocket.close(code=1008, reason="client authentication required")
            return

        await websocket.accept()
        send_lock = asyncio.Lock()
        subscription = media.subscribe_s3_video(device_id)

        async def push_video() -> None:
            async for frame in subscription:
                async with send_lock:
                    await websocket.send_bytes(frame)

        async def receive_client_messages() -> None:
            while True:
                message = await websocket.receive()
                if message["type"] == "websocket.disconnect":
                    raise WebSocketDisconnect(message.get("code", 1000))
                text = message.get("text")
                if text is None:
                    continue
                try:
                    payload = json.loads(text)
                except json.JSONDecodeError:
                    continue
                if payload.get("type") == "media.heartbeat":
                    async with send_lock:
                        await websocket.send_json({
                            "type": "media.heartbeat_ack",
                            "version": 1,
                            "sequence": payload.get("sequence"),
                            "server_time": utc_now(),
                        })

        sender = asyncio.create_task(push_video())
        receiver = asyncio.create_task(receive_client_messages())
        try:
            done, pending = await asyncio.wait(
                {sender, receiver}, return_when=asyncio.FIRST_COMPLETED
            )
            for task in done:
                task.result()
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
        except (WebSocketDisconnect, RuntimeError, asyncio.CancelledError):
            pass
        finally:
            await _cancel_tasks(sender, receiver)
            await subscription.aclose()

    @app.get("/api/dashboard/media/{device_id}/audio")
    async def dashboard_audio(device_id: str) -> StreamingResponse:
        return StreamingResponse(
            media.subscribe_audio(device_id),
            media_type="application/octet-stream",
            headers={
                "Cache-Control": "no-store",
                "X-Audio-Format": "s16le;rate=16000;channels=1",
                "X-Accel-Buffering": "no",
            },
        )

    @app.websocket("/api/mobile/v1/devices/{device_id}/media/audio/ws")
    @app.websocket("/api/dashboard/media/{device_id}/audio/ws")
    async def dashboard_audio_websocket(websocket: WebSocket, device_id: str) -> None:
        if app_settings.admin_user and not _basic_auth_valid(
            websocket.headers.get("authorization", ""),
            app_settings.admin_user,
            app_settings.admin_password,
        ):
            await websocket.close(code=1008, reason="client authentication required")
            return

        await websocket.accept()
        send_lock = asyncio.Lock()
        subscription = media.subscribe_s3_audio(device_id)

        async def push_audio() -> None:
            async for frame in subscription:
                async with send_lock:
                    await websocket.send_bytes(frame)

        async def receive_client_messages() -> None:
            while True:
                message = await websocket.receive()
                if message["type"] == "websocket.disconnect":
                    raise WebSocketDisconnect(message.get("code", 1000))
                text = message.get("text")
                if text is None:
                    continue
                try:
                    payload = json.loads(text)
                except json.JSONDecodeError:
                    continue
                if payload.get("type") == "media.heartbeat":
                    async with send_lock:
                        await websocket.send_json({
                            "type": "media.heartbeat_ack",
                            "version": 1,
                            "sequence": payload.get("sequence"),
                            "server_time": utc_now(),
                        })

        sender = asyncio.create_task(push_audio())
        receiver = asyncio.create_task(receive_client_messages())
        try:
            done, pending = await asyncio.wait(
                {sender, receiver}, return_when=asyncio.FIRST_COMPLETED
            )
            for task in done:
                task.result()
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
        except (WebSocketDisconnect, RuntimeError, asyncio.CancelledError):
            pass
        finally:
            await _cancel_tasks(sender, receiver)
            await subscription.aclose()

    @app.post("/api/dashboard/events/{event_id}/ack")
    async def acknowledge_event(event_id: int) -> dict[str, Any]:
        if not database.query_one("SELECT id FROM events WHERE id=?", (event_id,)):
            raise HTTPException(status_code=404, detail="event not found")
        database.execute("UPDATE events SET acknowledged=1 WHERE id=?", (event_id,))
        await broker.publish({"kind": "event_ack", "at": utc_now(), "data": {"id": event_id}})
        return {"ok": True}

    @app.get("/api/dashboard/resource-recordings/status")
    def resource_recording_status(device_id: str) -> dict[str, Any]:
        session = resource_recording_session(device_id, active_only=True)
        if session is None:
            session = resource_recording_session(device_id)
        return {"active": bool(session and session["status"] == "active"), "session": session}

    @app.post("/api/dashboard/resource-recordings/start")
    async def start_resource_recording(payload: ResourceRecordingControl) -> dict[str, Any]:
        active = resource_recording_session(payload.device_id, active_only=True)
        if active is not None:
            return {"ok": True, "already_active": True, "session": active}
        session_id = database.new_id("resource")
        started_at = utc_now()
        metadata = {
            "device_id": payload.device_id,
            "data_kind": "resource",
            "workflow": "reference_signal",
            "model_family": "system_performance",
            "storage": "server",
            "active_kinds": ["resource"],
        }
        database.execute(
            """
            INSERT INTO collection_sessions(session_id, name, description, status, created_at,
                                            started_at, target_labels_json, metadata_json)
            VALUES(?, ?, ?, 'active', ?, ?, '[]', ?)
            """,
            (
                session_id,
                f"RA8 performance {started_at}",
                "CPU0, CPU1, NPU, heap and inference runtime metrics",
                started_at,
                started_at,
                _json(metadata),
            ),
        )
        session = resource_recording_session(payload.device_id, active_only=True)
        await broker.publish({
            "kind": "resource_recording", "device_id": payload.device_id,
            "at": started_at, "data": {"active": True, "session": session},
        })
        return {"ok": True, "already_active": False, "session": session}

    @app.post("/api/dashboard/resource-recordings/stop")
    async def stop_resource_recording(payload: ResourceRecordingControl) -> dict[str, Any]:
        session = resource_recording_session(payload.device_id, active_only=True)
        if session is None:
            latest = resource_recording_session(payload.device_id)
            return {"ok": True, "already_stopped": True, "session": latest}
        ended_at = utc_now()
        metadata = dict(session["metadata"])
        metadata["active_kinds"] = []
        database.execute(
            "UPDATE collection_sessions SET status='complete', ended_at=?, metadata_json=? WHERE session_id=?",
            (ended_at, _json(metadata), session["session_id"]),
        )
        completed = resource_recording_session(payload.device_id)
        await broker.publish({
            "kind": "resource_recording", "device_id": payload.device_id,
            "at": ended_at, "data": {"active": False, "session": completed},
        })
        return {"ok": True, "already_stopped": False, "session": completed}

    @app.get("/api/dashboard/resource-recordings/{session_id}/csv")
    def export_resource_recording_csv(session_id: str) -> StreamingResponse:
        session = database.query_one(
            "SELECT * FROM collection_sessions WHERE session_id=?", (session_id,)
        )
        if session is None:
            raise HTTPException(status_code=404, detail="resource recording not found")
        metadata = _loads(session.get("metadata_json"), {})
        if metadata.get("data_kind") != "resource":
            raise HTTPException(status_code=409, detail="session is not a resource recording")
        rows = database.query_all(
            "SELECT captured_at, features_json FROM samples "
            "WHERE session_id=? AND kind='resource' ORDER BY id",
            (session_id,),
        )
        columns = [
            "captured_at", "received_at", "telemetry_sequence", "sequence",
            "device_time_ms", "ra8_tick", "cpu1_tick", "age_ms", "window_ms", "flags",
            "cpu0_usage_percent", "cpu1_usage_percent", "npu_usage_percent",
            "cpu0_heap_free", "cpu0_heap_min", "cpu1_heap_free", "cpu1_heap_min",
            "npu_busy_ms", "inference_count", "vision_total_ms",
        ]
        output = io.StringIO(newline="")
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        for row in rows:
            values = _loads(row.get("features_json"), {})
            def usage_percent(field: str) -> float | str:
                value = values.get(field)
                return "" if value is None else _as_int(value) / 10

            writer.writerow({
                "captured_at": row["captured_at"],
                "received_at": values.get("received_at", ""),
                "telemetry_sequence": values.get("telemetry_sequence", ""),
                "sequence": values.get("sequence", ""),
                "device_time_ms": values.get("device_time_ms", ""),
                "ra8_tick": values.get("ra8_tick", ""),
                "cpu1_tick": values.get("cpu1_tick", ""),
                "age_ms": values.get("age_ms", ""),
                "window_ms": values.get("window_ms", ""),
                "flags": values.get("flags", ""),
                "cpu0_usage_percent": usage_percent("cpu0_usage_permille"),
                "cpu1_usage_percent": usage_percent("cpu1_usage_permille"),
                "npu_usage_percent": usage_percent("npu_usage_permille"),
                "cpu0_heap_free": values.get("cpu0_heap_free", ""),
                "cpu0_heap_min": values.get("cpu0_heap_min", ""),
                "cpu1_heap_free": values.get("cpu1_heap_free", ""),
                "cpu1_heap_min": values.get("cpu1_heap_min", ""),
                "npu_busy_ms": values.get("npu_busy_ms", ""),
                "inference_count": values.get("inference_count", ""),
                "vision_total_ms": values.get("vision_total_ms", ""),
            })
        return StreamingResponse(
            iter(["\ufeff" + output.getvalue()]),
            media_type="text/csv; charset=utf-8",
            headers={"Content-Disposition": f'attachment; filename="{session_id}.csv"'},
        )

    @app.post("/api/dashboard/sessions")
    def create_session(payload: SessionCreate) -> dict[str, Any]:
        session_id = database.new_id("session")
        metadata = dict(payload.metadata)
        data_kind = str(metadata.get("data_kind", "")).lower()
        storage = "sd" if data_kind in MODEL_DATA_KINDS else "server"
        if not metadata.get("workflow"):
            if data_kind in MODEL_DATA_KINDS:
                metadata["workflow"] = "model_dataset"
            elif data_kind in REFERENCE_DATA_KINDS:
                metadata["workflow"] = "reference_signal"
            else:
                metadata["workflow"] = "legacy"
        if not metadata.get("model_family"):
            metadata["model_family"] = {
                "audio": "cry_audio",
                "camera": "vision",
                "thermal": "rule_reference",
                "pressure": "rule_reference",
                "temperature": "rule_reference",
                "resource": "system_performance",
            }.get(data_kind, "unspecified")
        database.execute(
            """
            INSERT INTO collection_sessions(session_id, name, description, status, created_at,
                                            target_labels_json, metadata_json)
            VALUES(?, ?, ?, 'draft', ?, ?, ?)
            """,
            (
                session_id,
                payload.name,
                payload.description,
                utc_now(),
                _json(payload.target_labels),
                _json({**metadata, "device_id": payload.device_id, "storage": storage}),
            ),
        )
        return {"ok": True, "session_id": session_id}

    @app.post("/api/dashboard/sessions/{session_id}/{action}")
    async def session_action(
        session_id: str,
        action: str,
        kind: str = Query(default="all", pattern="^(all|camera|audio|thermal|pressure|temperature|resource)$"),
    ) -> dict[str, Any]:
        session = database.query_one("SELECT * FROM collection_sessions WHERE session_id=?", (session_id,))
        if not session:
            raise HTTPException(status_code=404, detail="session not found")
        if action not in {"start", "stop"}:
            raise HTTPException(status_code=404, detail="unknown action")
        metadata = _loads(session["metadata_json"], {})
        active_kinds = set(metadata.get("active_kinds", []))
        selected = {"camera", "audio", "thermal", "pressure", "temperature", "resource"} if kind == "all" else {kind}
        if action == "start":
            sd_selected = selected.intersection(MODEL_DATA_KINDS)
            if sd_selected:
                latest = database.query_one(
                    "SELECT payload_json FROM telemetry WHERE device_id=? ORDER BY id DESC LIMIT 1",
                    (metadata.get("device_id", "demo-device"),),
                )
                latest_sensors = _loads(latest["payload_json"], {}).get("sensors", {}) if latest else {}
                sd_baseline = dict(metadata.get("sd_baseline", {})) if isinstance(metadata.get("sd_baseline"), dict) else {}
                if "camera" in sd_selected:
                    camera = latest_sensors.get("camera", {}) if isinstance(latest_sensors, dict) else {}
                    if isinstance(camera, dict):
                        sd_baseline["camera_saved"] = _as_int(camera.get("saved"))
                        sd_baseline["camera_cmd_seq"] = _as_int(camera.get("cmd_seq"))
                if "audio" in sd_selected:
                    microphone = latest_sensors.get("microphone", {}) if isinstance(latest_sensors, dict) else {}
                    if isinstance(microphone, dict):
                        sd_baseline["audio_cmd_seq"] = _as_int(microphone.get("cmd_seq"))
                metadata["sd_baseline"] = sd_baseline
            active_kinds.update(selected)
        else:
            active_kinds.difference_update(selected)
        metadata["active_kinds"] = sorted(active_kinds)
        status = "active" if active_kinds else ("complete" if session["started_at"] else "draft")
        database.execute(
            "UPDATE collection_sessions SET status=?, started_at=COALESCE(started_at, ?), ended_at=?, metadata_json=? WHERE session_id=?",
            (status, utc_now(), utc_now() if status == "complete" else None, _json(metadata), session_id),
        )
        device_id = metadata.get("device_id", "demo-device")
        command_id = None
        # Thermal, pressure and temperature are already streamed by RA8 and are
        # gated into independent server-side sessions here. Audio and the TTL
        # camera need device commands because their artifacts live on RA8 SD.
        sd_kinds = selected.intersection({"audio", "camera"})
        if sd_kinds:
            command = "collection.start" if action == "start" else "collection.stop"
            for sd_kind in sorted(sd_kinds):
                command_id = database.execute(
                    "INSERT INTO commands(device_id, command_json, status, created_at) VALUES(?, ?, 'queued', ?)",
                    (
                        device_id,
                        _json({
                            "command": command,
                            "args": {
                                "session_id": session_id,
                                "name": session["name"],
                                "labels": _loads(session["target_labels_json"], []),
                                "storage": "sd",
                                "kinds": [sd_kind],
                            },
                        }),
                        utc_now(),
                    ),
                )
        await broker.publish({"kind": "session", "at": utc_now(), "data": {"session_id": session_id, "action": action, "data_kind": kind}})
        return {
            "ok": True,
            "command_id": command_id,
            "storage": "sd" if sd_kinds else "server",
            "active_kinds": sorted(active_kinds),
        }

    @app.get("/api/dashboard/sessions/{session_id}/samples")
    def list_samples(session_id: str, limit: int = Query(default=200, ge=1, le=2000)) -> list[dict[str, Any]]:
        rows = database.query_all("SELECT * FROM samples WHERE session_id=? ORDER BY id DESC LIMIT ?", (session_id, limit))
        for row in rows:
            decode_json_columns(row, "features_json")
        return rows

    @app.post("/api/dashboard/sessions/{session_id}/artifacts/import")
    async def import_session_artifact(
        session_id: str,
        request: Request,
        kind: str = Query(pattern="^(camera|audio)$"),
        filename: str = Query(min_length=1, max_length=255),
        label: str | None = Query(default=None, max_length=120),
    ) -> dict[str, Any]:
        session = database.query_one("SELECT * FROM collection_sessions WHERE session_id=?", (session_id,))
        if not session:
            raise HTTPException(status_code=404, detail="collection session not found")
        session_metadata = _loads(session["metadata_json"], {})
        session_kind = session_metadata.get("data_kind")
        if session_kind and session_kind != kind:
            raise HTTPException(status_code=409, detail=f"this session collects {session_kind}, not {kind}")

        original_name = Path(filename.replace("\\", "/")).name
        suffix = Path(original_name).suffix.lower()
        if not original_name or suffix not in IMPORT_EXTENSIONS[kind]:
            expected = "JPEG" if kind == "camera" else "WAV"
            raise HTTPException(status_code=415, detail=f"{kind} import requires a {expected} file")
        raw = await request.body()
        if not raw or len(raw) > app_settings.max_artifact_bytes:
            raise HTTPException(status_code=413, detail="artifact is empty or too large")
        if kind == "camera" and (len(raw) < 4 or not raw.startswith(b"\xff\xd8") or b"\xff\xd9" not in raw[-32:]):
            raise HTTPException(status_code=422, detail="JPEG markers are missing")
        if kind == "audio" and (len(raw) < 12 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE"):
            raise HTTPException(status_code=422, detail="invalid WAV header")

        digest = hashlib.sha256(raw).hexdigest()
        existing_artifact = database.query_one(
            "SELECT * FROM artifacts WHERE session_id=? AND kind=? AND sha256=? LIMIT 1",
            (session_id, kind, digest),
        )
        if existing_artifact:
            existing_sample = database.query_one(
                "SELECT id FROM samples WHERE session_id=? AND source_ref=? LIMIT 1",
                (session_id, f"artifact:{existing_artifact['artifact_id']}"),
            )
            return {
                "ok": True,
                "duplicate": True,
                "artifact_id": existing_artifact["artifact_id"],
                "sample_id": existing_sample["id"] if existing_sample else None,
                "matched_placeholder": bool(existing_sample),
            }

        pending = database.query_one(
            "SELECT * FROM samples WHERE session_id=? AND kind=? AND source_ref LIKE ? ORDER BY id LIMIT 1",
            (session_id, kind, f"sd:{kind}:%"),
        )
        artifact_id = database.new_id("art")
        device_id = session_metadata.get("device_id", "demo-device")
        relative = Path(_safe_component(device_id, "device")) / _safe_component(session_id, "session") / f"{artifact_id}{suffix}"
        destination = app_settings.artifact_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
        mime_type = "image/jpeg" if kind == "camera" else "audio/wav"
        captured_at = pending["captured_at"] if pending else utc_now()
        artifact_metadata = {
            "original_filename": original_name,
            "imported_at": utc_now(),
            "source_sd_ref": pending["source_ref"] if pending else None,
        }
        database.execute(
            """
            INSERT INTO artifacts(artifact_id, session_id, device_id, kind, mime_type, relative_path,
                                  sha256, size_bytes, captured_at, metadata_json)
            VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                artifact_id,
                session_id,
                device_id,
                kind,
                mime_type,
                str(relative),
                digest,
                len(raw),
                captured_at,
                _json(artifact_metadata),
            ),
        )
        imported_features = {
            "artifact_id": artifact_id,
            "original_filename": original_name,
            "sha256": digest,
            "size_bytes": len(raw),
            "storage": "server",
            "transfer_status": "imported",
            "source_sd_ref": pending["source_ref"] if pending else None,
        }
        if pending:
            imported_features = {**_loads(pending["features_json"], {}), **imported_features}
            database.execute(
                "UPDATE samples SET source_ref=?, features_json=?, label=COALESCE(?, label) WHERE id=?",
                (f"artifact:{artifact_id}", _json(imported_features), label, pending["id"]),
            )
            sample_id = pending["id"]
        else:
            target_labels = _loads(session["target_labels_json"], [])
            capture_label = label or session_metadata.get("capture_label")
            if not capture_label and len(target_labels) == 1:
                capture_label = target_labels[0]
            sample_id = database.capture_sample(
                session_id,
                device_id,
                kind,
                f"artifact:{artifact_id}",
                imported_features,
                capture_label,
                str(session_metadata.get("default_split", "unassigned"))
                if str(session_metadata.get("default_split", "unassigned")) in
                {"unassigned", "train", "validation", "test"} else "unassigned",
            )
        await publish(
            "artifact",
            device_id,
            {"artifact_id": artifact_id, "sample_id": sample_id, "kind": kind, "size": len(raw)},
        )
        return {
            "ok": True,
            "duplicate": False,
            "artifact_id": artifact_id,
            "sample_id": sample_id,
            "matched_placeholder": bool(pending),
            "sha256": digest,
            "size": len(raw),
        }

    @app.patch("/api/dashboard/samples/{sample_id}")
    def update_sample(sample_id: int, payload: SampleUpdate) -> dict[str, Any]:
        sample = database.query_one("SELECT id FROM samples WHERE id=?", (sample_id,))
        if not sample:
            raise HTTPException(status_code=404, detail="sample not found")
        current = database.query_one("SELECT label, notes, quality, split FROM samples WHERE id=?", (sample_id,)) or {}
        database.execute(
            "UPDATE samples SET label=?, notes=?, quality=?, split=? WHERE id=?",
            (
                payload.label if payload.label is not None else current.get("label"),
                payload.notes if payload.notes is not None else current.get("notes", ""),
                payload.quality if payload.quality is not None else current.get("quality", "unreviewed"),
                payload.split if payload.split is not None else current.get("split", "unassigned"),
                sample_id,
            ),
        )
        return {"ok": True}

    @app.get("/api/dashboard/sessions/{session_id}/export")
    def export_session(session_id: str) -> StreamingResponse:
        session = database.query_one("SELECT * FROM collection_sessions WHERE session_id=?", (session_id,))
        if not session:
            raise HTTPException(status_code=404, detail="session not found")
        samples = database.query_all("SELECT * FROM samples WHERE session_id=? ORDER BY id", (session_id,))
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("manifest.json", _json(decode_json_columns(session, "target_labels_json", "metadata_json")))
            archive.writestr(
                "DATASET_README.txt",
                "samples.jsonl 是完整样本索引。\n"
                "已导入的 JPEG/WAV 位于 dataset/<kind>/<split>/<label>/。\n"
                "source_ref 以 sd: 开头的记录仍只存在于 RA8 SD 卡，导出包不会伪造媒体文件。\n"
                "矩阵原始数据为 little-endian int16，位于 matrices/。\n",
            )
            lines: list[str] = []
            for sample in samples:
                decode_json_columns(sample, "features_json")
                lines.append(_json(sample))
                if sample["source_ref"].startswith("artifact:"):
                    artifact_id = sample["source_ref"].split(":", 1)[1]
                    artifact = database.query_one("SELECT * FROM artifacts WHERE artifact_id=?", (artifact_id,))
                    if artifact:
                        source = app_settings.artifact_dir / artifact["relative_path"]
                        if source.is_file():
                            artifact_metadata = _loads(artifact.get("metadata_json"), {})
                            original_name = _safe_component(
                                artifact_metadata.get("original_filename", source.name), source.name
                            )
                            split = sample.get("split")
                            if split not in {"train", "validation", "test"}:
                                split = "unassigned"
                            label = _safe_component(sample.get("label") or "unlabeled", "unlabeled")
                            kind = _safe_component(sample.get("kind") or artifact["kind"], "unknown")
                            archive.write(
                                source,
                                f"dataset/{kind}/{split}/{label}/{sample['id']:06d}_{original_name}",
                            )
                elif sample["source_ref"].startswith("matrix:"):
                    matrix_id = int(sample["source_ref"].split(":", 1)[1])
                    matrix = database.query_one("SELECT * FROM matrices WHERE id=?", (matrix_id,))
                    if matrix:
                        archive.writestr(f"matrices/{matrix_id}_{matrix['kind']}.i16le", matrix["data"])
            archive.writestr("samples.jsonl", "\n".join(lines) + ("\n" if lines else ""))
        output.seek(0)
        return StreamingResponse(
            output,
            media_type="application/zip",
            headers={"Content-Disposition": f'attachment; filename="{session_id}.zip"'},
        )

    @app.post("/api/dashboard/commands")
    def create_command(payload: CommandCreate) -> dict[str, Any]:
        row, _ = database.enqueue_command(
            payload.device_id,
            _json({"command": payload.command, "args": payload.args}),
            utc_now(),
        )
        return {"ok": True, "id": row["id"]}

    @app.post("/api/dashboard/recordings/{kind}/{action}")
    def control_media_recording(
        kind: str,
        action: str,
        payload: MediaRecordingControl,
    ) -> dict[str, Any]:
        allowed_actions = RECORDING_ACTIONS.get(kind)
        if not allowed_actions or action not in allowed_actions:
            raise HTTPException(status_code=404, detail="unsupported recording action")

        basename = _recording_basename(kind, payload.filename) if action != "stop" else ""
        command = "collection.capture" if action == "capture" else f"collection.{action}"
        args: dict[str, Any] = {
            "kinds": [kind],
            "storage": "sd",
            "purpose": "media_test",
        }
        if basename:
            args["filename"] = basename

        row, _ = database.enqueue_command(
            payload.device_id,
            _json({"command": command, "args": args}),
            utc_now(),
        )
        folder = "MIC" if kind == "audio" else "CAMERA"
        extension = "WAV" if kind == "audio" else "JPG"
        expected_path = None
        if action != "stop":
            expected_path = (
                f"/{folder}/{basename}.{extension}"
                if basename else f"/{folder}/自动编号.{extension}"
            )
        return {
            "ok": True,
            "id": row["id"],
            "command": command,
            "args": args,
            "expected_path": expected_path,
        }

    @app.post("/api/dashboard/models")
    async def upload_model(
        request: Request,
        name: str,
        version: str,
        target: str = "ra8p1",
        task: str = "classification",
        metrics: str = "{}",
        metadata: str = "{}",
    ) -> dict[str, Any]:
        raw = await request.body()
        if not raw or len(raw) > 100 * 1024 * 1024:
            raise HTTPException(status_code=413, detail="model package is empty or too large")
        try:
            with zipfile.ZipFile(io.BytesIO(raw)) as archive:
                if archive.testzip() is not None or "manifest.json" not in archive.namelist():
                    raise ValueError("invalid model ZIP")
        except (zipfile.BadZipFile, ValueError) as error:
            raise HTTPException(status_code=422, detail=str(error)) from error
        model_id = database.new_id("model")
        model_dir = app_settings.data_dir / "models"
        model_dir.mkdir(parents=True, exist_ok=True)
        relative = Path("models") / f"{model_id}.zip"
        (app_settings.data_dir / relative).write_bytes(raw)
        digest = hashlib.sha256(raw).hexdigest()
        database.execute(
            """
            INSERT INTO model_packages(model_id, name, version, target, task, relative_path, sha256,
                                       size_bytes, created_at, metrics_json, metadata_json)
            VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (model_id, name, version, target, task, str(relative), digest, len(raw), utc_now(), metrics, metadata),
        )
        return {"ok": True, "model_id": model_id, "sha256": digest}

    @app.get("/api/dashboard/models/{model_id}/download")
    def download_model(model_id: str) -> FileResponse:
        model = database.query_one("SELECT * FROM model_packages WHERE model_id=?", (model_id,))
        if not model:
            raise HTTPException(status_code=404, detail="model not found")
        return FileResponse(
            app_settings.data_dir / model["relative_path"],
            media_type="application/zip",
            filename=f"{model['name']}_{model['version']}_{model['target']}.zip",
        )

    @app.get("/api/dashboard/artifacts/{artifact_id}")
    def download_artifact(artifact_id: str) -> FileResponse:
        artifact = database.query_one("SELECT * FROM artifacts WHERE artifact_id=?", (artifact_id,))
        if not artifact:
            raise HTTPException(status_code=404, detail="artifact not found")
        metadata = _loads(artifact.get("metadata_json"), {})
        filename = Path(str(metadata.get("original_filename") or artifact["relative_path"])).name
        return FileResponse(
            app_settings.artifact_dir / artifact["relative_path"],
            media_type=artifact["mime_type"],
            filename=filename,
            content_disposition_type="inline",
        )

    @app.get("/api/mobile/v1/devices/{device_id}/stream")
    async def mobile_stream(device_id: str) -> StreamingResponse:
        mobile_device_row(device_id)

        async def generate():
            mobile_presence.connect(device_id)
            iterator = broker.subscribe().__aiter__()
            pending = asyncio.create_task(anext(iterator))
            last_snapshot_at = 0.0
            try:
                await asyncio.sleep(0)
                initial = await asyncio.to_thread(mobile_snapshot_for, device_id)
                yield f"event: snapshot\ndata: {_json(initial)}\n\n"
                while True:
                    done, _ = await asyncio.wait({pending}, timeout=15)
                    if pending not in done:
                        yield ": keepalive\n\n"
                        continue

                    message = pending.result()
                    pending = asyncio.create_task(anext(iterator))
                    if message.get("device_id") != device_id:
                        continue

                    kind = str(message.get("kind", "update"))
                    event_name = kind
                    data: dict[str, Any]
                    if kind in {"telemetry", "device"}:
                        now = time.monotonic()
                        if kind == "telemetry" and now - last_snapshot_at < 1.0:
                            continue
                        event_name = "snapshot"
                        data = await asyncio.to_thread(mobile_snapshot_for, device_id)
                        last_snapshot_at = now
                    elif kind == "matrix":
                        data = {
                            "type": "mobile.matrix",
                            "protocol_version": MOBILE_PROTOCOL_VERSION,
                            "server_time": message.get("at", utc_now()),
                            "device_id": device_id,
                            "matrix": message.get("data", {}),
                        }
                    elif kind == "command":
                        command_id = _as_int(message.get("data", {}).get("id"))
                        row = database.query_one(
                            "SELECT * FROM commands WHERE id=? AND device_id=?",
                            (command_id, device_id),
                        )
                        if not row:
                            continue
                        row["result"] = _loads(row.get("result_json"), None)
                        data = {
                            "type": "mobile.command",
                            "protocol_version": MOBILE_PROTOCOL_VERSION,
                            "command": mobile_command_view(row, _loads(row.get("command_json"), {})),
                        }
                    else:
                        data = {
                            "type": f"mobile.{kind}",
                            "protocol_version": MOBILE_PROTOCOL_VERSION,
                            "server_time": message.get("at", utc_now()),
                            "device_id": device_id,
                            "data": message.get("data", {}),
                        }
                    yield f"event: {event_name}\ndata: {_json(data)}\n\n"
            finally:
                pending.cancel()
                await asyncio.gather(pending, return_exceptions=True)
                await iterator.aclose()
                mobile_presence.disconnect(device_id)

        return StreamingResponse(
            generate(),
            media_type="text/event-stream",
            headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"},
        )

    @app.get("/api/dashboard/stream")
    async def dashboard_stream() -> StreamingResponse:
        async def generate():
            iterator = broker.subscribe().__aiter__()
            pending = asyncio.create_task(anext(iterator))
            try:
                while True:
                    done, _ = await asyncio.wait({pending}, timeout=15)
                    if pending in done:
                        message = pending.result()
                        yield f"event: update\ndata: {_json(message)}\n\n"
                        pending = asyncio.create_task(anext(iterator))
                    else:
                        yield ": keepalive\n\n"
            finally:
                pending.cancel()
                await asyncio.gather(pending, return_exceptions=True)
                await iterator.aclose()

        return StreamingResponse(
            generate(), media_type="text/event-stream",
            headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"},
        )

    @app.websocket("/api/dashboard/realtime/ws")
    async def dashboard_realtime_websocket(websocket: WebSocket) -> None:
        if app_settings.admin_user and not _basic_auth_valid(
            websocket.headers.get("authorization", ""),
            app_settings.admin_user,
            app_settings.admin_password,
        ):
            await websocket.close(code=1008, reason="client authentication required")
            return

        await websocket.accept()
        iterator = broker.subscribe().__aiter__()
        send_lock = asyncio.Lock()

        async def push_updates() -> None:
            pending = asyncio.create_task(anext(iterator))
            try:
                while True:
                    done, _ = await asyncio.wait({pending}, timeout=10.0)
                    if pending in done:
                        message = pending.result()
                        pending = asyncio.create_task(anext(iterator))
                    else:
                        message = {"kind": "heartbeat", "at": utc_now()}
                    async with send_lock:
                        await websocket.send_json(message)
            finally:
                pending.cancel()
                await asyncio.gather(pending, return_exceptions=True)

        async def receive_client_messages() -> None:
            while True:
                message = await websocket.receive()
                if message["type"] == "websocket.disconnect":
                    raise WebSocketDisconnect(message.get("code", 1000))
                text = message.get("text")
                if text is None:
                    continue
                try:
                    payload = json.loads(text)
                except json.JSONDecodeError:
                    continue
                if payload.get("type") == "dashboard.ping":
                    async with send_lock:
                        await websocket.send_json({
                            "kind": "heartbeat_ack",
                            "sequence": payload.get("sequence"),
                            "at": utc_now(),
                        })

        sender = asyncio.create_task(push_updates())
        receiver = asyncio.create_task(receive_client_messages())
        try:
            done, pending = await asyncio.wait(
                {sender, receiver}, return_when=asyncio.FIRST_COMPLETED
            )
            for task in done:
                task.result()
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
        except (WebSocketDisconnect, RuntimeError, asyncio.CancelledError):
            pass
        finally:
            await _cancel_tasks(sender, receiver)
            await iterator.aclose()

    @app.exception_handler(HTTPException)
    async def http_error(_: Request, exc: HTTPException) -> JSONResponse:
        return JSONResponse(status_code=exc.status_code, content={"ok": False, "error": exc.detail})

    return app


app = create_app()

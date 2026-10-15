from __future__ import annotations

import re
from typing import Any
from urllib.parse import quote


MOBILE_PROTOCOL_VERSION = 1
_RECORDING_BASENAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,7}$")


MOBILE_COMMAND_DEFINITIONS: tuple[dict[str, Any], ...] = (
    {"action": "device.refresh", "args": {}, "description": "Request a fresh RA8 status frame."},
    {"action": "audio.play_boot", "args": {}, "description": "Play BOOT.WAV once."},
    {
        "action": "audio.play_track",
        "args": {"track": {"type": "integer", "minimum": 1, "maximum": 9999, "required": True}},
        "description": "Play one indexed SD-card track.",
    },
    {"action": "audio.pause", "args": {}, "description": "Pause playback."},
    {"action": "audio.resume", "args": {}, "description": "Resume playback."},
    {"action": "audio.stop", "args": {}, "description": "Stop playback."},
    {"action": "audio.next", "args": {}, "description": "Play the next track."},
    {"action": "audio.previous", "args": {}, "description": "Play the previous track."},
    {
        "action": "audio.set_volume",
        "args": {"percent": {"type": "integer", "minimum": 0, "maximum": 100, "required": True}},
        "description": "Set playback volume in percent.",
    },
    {"action": "audio.refresh_library", "args": {}, "description": "Rescan SD-card tracks."},
    {
        "action": "light.set",
        "args": {"on": {"type": "boolean", "required": True}},
        "description": "Switch the warm light relay on or off.",
        "control": "switch",
        "idempotent": True,
        "confirmation": "telemetry.sensors.warm_light.on",
    },
    {
        "action": "soother.set",
        "args": {"on": {"type": "boolean", "required": True}},
        "description": "Switch the soothing motor relay on or off.",
        "control": "switch",
        "idempotent": True,
        "confirmation": "telemetry.sensors.soother.on",
    },
    {"action": "microphone.start", "args": {}, "description": "Start RA8 microphone sampling."},
    {"action": "microphone.stop", "args": {}, "description": "Stop RA8 microphone sampling."},
    {
        "action": "microphone.record.start",
        "args": {"filename": {"type": "string", "pattern": "[A-Za-z0-9_-]{1,8}", "required": False}},
        "description": "Start a WAV recording on the RA8 SD card.",
    },
    {"action": "microphone.record.stop", "args": {}, "description": "Stop the active WAV recording."},
    {
        "action": "camera.capture",
        "args": {"filename": {"type": "string", "pattern": "[A-Za-z0-9_-]{1,8}", "required": False}},
        "description": "Capture one JPEG on the RA8 SD card.",
    },
    {
        "action": "camera.record.start",
        "args": {"filename": {"type": "string", "pattern": "[A-Za-z0-9_-]{1,8}", "required": False}},
        "description": "Start serial-camera collection on the RA8 SD card.",
    },
    {"action": "camera.record.stop", "args": {}, "description": "Stop serial-camera collection."},
)


class MobileCommandError(ValueError):
    pass


class MobilePresenceRegistry:
    """Tracks active mobile SSE streams without treating dashboard clients as phones."""

    def __init__(self) -> None:
        self._counts: dict[str, int] = {}

    def connect(self, device_id: str) -> None:
        self._counts[device_id] = self._counts.get(device_id, 0) + 1

    def disconnect(self, device_id: str) -> None:
        count = self._counts.get(device_id, 0)
        if count <= 1:
            self._counts.pop(device_id, None)
        else:
            self._counts[device_id] = count - 1

    def connected(self, device_id: str) -> bool:
        return self._counts.get(device_id, 0) > 0

    def count(self, device_id: str) -> int:
        return self._counts.get(device_id, 0)


def _reject_unknown_args(args: dict[str, Any], allowed: set[str]) -> None:
    unknown = sorted(set(args) - allowed)
    if unknown:
        raise MobileCommandError(f"unsupported arguments: {', '.join(unknown)}")


def _integer_arg(args: dict[str, Any], name: str, minimum: int, maximum: int) -> int:
    value = args.get(name)
    if isinstance(value, bool) or not isinstance(value, int):
        raise MobileCommandError(f"{name} must be an integer")
    if value < minimum or value > maximum:
        raise MobileCommandError(f"{name} must be between {minimum} and {maximum}")
    return value


def _recording_args(args: dict[str, Any], kind: str) -> dict[str, Any]:
    _reject_unknown_args(args, {"filename"})
    filename = str(args.get("filename", "")).strip()
    if filename and not _RECORDING_BASENAME.fullmatch(filename):
        raise MobileCommandError("filename must be 1-8 ASCII letters, digits, underscores or hyphens")
    result: dict[str, Any] = {"kinds": [kind], "storage": "sd", "purpose": "mobile"}
    if filename:
        result["filename"] = filename
    return result


def map_mobile_command(action: str, args: dict[str, Any]) -> dict[str, Any]:
    action = action.strip().lower()
    args = dict(args)

    no_arg_payloads = {
        "audio.play_boot": "play_boot",
        "audio.pause": "pause",
        "audio.resume": "resume",
        "audio.stop": "stop",
        "audio.next": "next",
        "audio.previous": "prev",
        "audio.refresh_library": "refresh",
        "microphone.start": "mic_start",
        "microphone.stop": "mic_stop",
    }
    if action in no_arg_payloads:
        _reject_unknown_args(args, set())
        return {"command": "ra8.command", "args": {"payload": no_arg_payloads[action]}}

    if action == "device.refresh":
        _reject_unknown_args(args, set())
        return {"command": "status.refresh", "args": {}}
    if action == "audio.play_track":
        _reject_unknown_args(args, {"track"})
        track = _integer_arg(args, "track", 1, 9999)
        return {"command": "ra8.command", "args": {"payload": f"play track{track}"}}
    if action == "audio.set_volume":
        _reject_unknown_args(args, {"percent"})
        percent = _integer_arg(args, "percent", 0, 100)
        return {"command": "ra8.command", "args": {"payload": f"volume{percent}"}}
    if action == "light.set":
        _reject_unknown_args(args, {"on"})
        on = args.get("on")
        if not isinstance(on, bool):
            raise MobileCommandError("on must be a boolean")
        return {"command": "ra8.command", "args": {"payload": "light_on" if on else "light_off"}}
    if action == "soother.set":
        _reject_unknown_args(args, {"on"})
        on = args.get("on")
        if not isinstance(on, bool):
            raise MobileCommandError("on must be a boolean")
        return {"command": "soother.set", "args": {"on": on}}
    if action == "microphone.record.start":
        return {"command": "collection.start", "args": _recording_args(args, "audio")}
    if action == "microphone.record.stop":
        _reject_unknown_args(args, set())
        return {"command": "collection.stop", "args": {"kinds": ["audio"], "storage": "sd", "purpose": "mobile"}}
    if action == "camera.capture":
        return {"command": "collection.capture", "args": _recording_args(args, "camera")}
    if action == "camera.record.start":
        return {"command": "collection.start", "args": _recording_args(args, "camera")}
    if action == "camera.record.stop":
        _reject_unknown_args(args, set())
        return {"command": "collection.stop", "args": {"kinds": ["camera"], "storage": "sd", "purpose": "mobile"}}

    raise MobileCommandError(f"unsupported action: {action}")


def mobile_device(device: dict[str, Any]) -> dict[str, Any]:
    return {
        "id": device.get("device_id"),
        "name": device.get("name") or device.get("device_id"),
        "online": bool(device.get("online")),
        "first_seen": device.get("first_seen"),
        "last_seen": device.get("last_seen"),
        "firmware": device.get("firmware", ""),
        "hardware": device.get("hardware", ""),
        "wifi_rssi": device.get("wifi_rssi"),
        "capabilities": device.get("capabilities", []),
        "matrix_connected": bool(device.get("matrix_connected", False)),
        "matrix_status": device.get("matrix_status", {}),
    }


def mobile_matrix_summary(matrices: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for kind, matrix in matrices.items():
        result[kind] = {
            "available": True,
            "sequence": matrix.get("sequence", 0),
            "rows": matrix.get("rows", 0),
            "cols": matrix.get("cols", 0),
            "unit": matrix.get("unit", ""),
            "received_at": matrix.get("received_at"),
            "minimum": matrix.get("minimum", matrix.get("min")),
            "maximum": matrix.get("maximum", matrix.get("max")),
            "fps": matrix.get("fps", 0.0),
            "frame_age_ms": matrix.get("frame_age_ms"),
            "drops": matrix.get("drops", 0),
            "delayed": bool(matrix.get("delayed", False)),
        }
    return result


def mobile_media_status(status: dict[str, Any]) -> dict[str, Any]:
    return {
        "video": {
            "available": int(status.get("video_uploaders", 0)) > 0,
            "listeners": int(status.get("video_listeners", 0)),
            "latest": status.get("video"),
        },
        "audio": {
            "available": int(status.get("audio_uploaders", 0)) > 0,
            "listeners": int(status.get("audio_listeners", 0)),
            "latest": status.get("audio"),
        },
    }


def build_mobile_snapshot(
    *,
    server_time: str,
    device: dict[str, Any],
    telemetry: dict[str, Any] | None,
    matrices: dict[str, Any],
    diagnostics: dict[str, Any],
    media_status: dict[str, Any],
    fusion: dict[str, Any],
    vision_overlay: dict[str, Any],
    overlay_calibration: dict[str, Any],
) -> dict[str, Any]:
    device_id = str(device.get("device_id", ""))
    encoded_id = quote(device_id, safe="")
    return {
        "type": "mobile.snapshot",
        "protocol_version": MOBILE_PROTOCOL_VERSION,
        "server_time": server_time,
        "device": mobile_device(device),
        "telemetry": telemetry,
        "matrices": mobile_matrix_summary(matrices),
        "diagnostics": diagnostics,
        "fusion": fusion,
        "vision_overlay": vision_overlay,
        "overlay_calibration": overlay_calibration,
        "media": mobile_media_status(media_status),
        "links": {
            "stream": f"/api/mobile/v1/devices/{encoded_id}/stream",
            "thermal_matrix": f"/api/mobile/v1/devices/{encoded_id}/matrices/thermal",
            "pressure_matrix": f"/api/mobile/v1/devices/{encoded_id}/matrices/pressure",
            "video_ws": f"/api/mobile/v1/devices/{encoded_id}/media/video/ws",
            "audio_ws": f"/api/mobile/v1/devices/{encoded_id}/media/audio/ws",
            "commands": f"/api/mobile/v1/devices/{encoded_id}/commands",
            "stories": f"/api/mobile/v1/devices/{encoded_id}/stories",
            "advisories": f"/api/mobile/v1/devices/{encoded_id}/advisories",
        },
    }


def mobile_command_view(row: dict[str, Any], stored_command: dict[str, Any]) -> dict[str, Any]:
    result = row.get("result")
    if result is None:
        result = row.get("result_json")
    return {
        "id": int(row["id"]),
        "device_id": row.get("device_id"),
        "request_id": row.get("client_request_id"),
        "action": stored_command.get("mobile_action", ""),
        "status": row.get("status", "queued"),
        "created_at": row.get("created_at"),
        "sent_at": row.get("sent_at"),
        "acked_at": row.get("acked_at"),
        "delivery_attempts": int(row.get("delivery_attempts") or 0),
        "result": result if isinstance(result, dict) else None,
    }

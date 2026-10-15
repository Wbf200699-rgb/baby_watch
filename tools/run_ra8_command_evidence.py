#!/usr/bin/env python3
"""Exercise the deployed server -> gateway -> RA8 command path.

The script uses the production mobile API, waits for the gateway ACK, and then
checks the corresponding RA8 status fields. Credentials are read locally and
are never written to the evidence output.
"""

from __future__ import annotations

import argparse
import base64
import json
import time
import uuid
from datetime import datetime
from pathlib import Path
from typing import Any, Callable
from urllib.request import Request, urlopen


def load_credentials(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    required = {"URL", "ADMIN_USER", "ADMIN_PASSWORD", "DEVICE_ID"}
    missing = sorted(required - values.keys())
    if missing:
        raise RuntimeError(f"missing credential fields: {', '.join(missing)}")
    return values


class MobileApi:
    def __init__(self, credentials: dict[str, str]) -> None:
        self.base_url = credentials["URL"].rstrip("/")
        self.device_id = credentials["DEVICE_ID"]
        raw = f'{credentials["ADMIN_USER"]}:{credentials["ADMIN_PASSWORD"]}'.encode()
        self.authorization = "Basic " + base64.b64encode(raw).decode("ascii")

    def request(self, method: str, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        body = None if payload is None else json.dumps(payload).encode("utf-8")
        request = Request(
            self.base_url + path,
            data=body,
            method=method,
            headers={
                "Authorization": self.authorization,
                "Content-Type": "application/json",
                "Accept": "application/json",
            },
        )
        with urlopen(request, timeout=15) as response:
            return json.load(response)

    def snapshot(self) -> dict[str, Any]:
        return self.request("GET", f"/api/mobile/v1/devices/{self.device_id}/snapshot")

    def command(self, action: str, args: dict[str, Any] | None = None) -> dict[str, Any]:
        payload = {
            "action": action,
            "args": args or {},
            "request_id": f"hwtest-{uuid.uuid4()}",
        }
        response = self.request(
            "POST",
            f"/api/mobile/v1/devices/{self.device_id}/commands",
            payload,
        )
        return response["command"]

    def command_status(self, command_id: int) -> dict[str, Any]:
        response = self.request(
            "GET",
            f"/api/mobile/v1/devices/{self.device_id}/commands/{command_id}",
        )
        return response["command"]


def sensors(snapshot: dict[str, Any]) -> dict[str, Any]:
    telemetry = snapshot.get("telemetry") or {}
    value = telemetry.get("sensors") or {}
    return value if isinstance(value, dict) else {}


def wait_until(
    description: str,
    predicate: Callable[[], tuple[bool, dict[str, Any]]],
    timeout: float,
    interval: float = 0.25,
) -> tuple[bool, dict[str, Any], float]:
    started = time.monotonic()
    latest: dict[str, Any] = {}
    while time.monotonic() - started < timeout:
        ok, latest = predicate()
        if ok:
            return True, latest, round((time.monotonic() - started) * 1000, 1)
        time.sleep(interval)
    return False, latest, round((time.monotonic() - started) * 1000, 1)


def wait_command(api: MobileApi, command_id: int, timeout: float = 12.0) -> tuple[bool, dict[str, Any], float]:
    return wait_until(
        f"command {command_id} ACK",
        lambda: (
            (status := api.command_status(command_id)).get("status") in {"acked", "failed"},
            status,
        ),
        timeout,
    )


def field_int(value: Any, key: str) -> int:
    try:
        return int((value or {}).get(key, 0))
    except (TypeError, ValueError):
        return 0


def run_action(
    api: MobileApi,
    action: str,
    args: dict[str, Any] | None,
    status_check: Callable[[dict[str, Any]], tuple[bool, dict[str, Any]]],
    timeout: float,
) -> dict[str, Any]:
    issued_at = datetime.now().astimezone().isoformat()
    command = api.command(action, args)
    ack_ok, ack, ack_ms = wait_command(api, int(command["id"]))
    ack_ok = ack_ok and ack.get("status") == "acked"
    state_ok = False
    state: dict[str, Any] = {}
    state_ms = 0.0
    if ack_ok:
        state_ok, state, state_ms = wait_until(action, lambda: status_check(api.snapshot()), timeout)
    return {
        "action": action,
        "args": args or {},
        "issued_at": issued_at,
        "command_id": command["id"],
        "gateway_status": ack.get("status", "timeout"),
        "delivery_attempts": ack.get("delivery_attempts"),
        "gateway_ack_ms": ack_ms,
        "ra8_state_ok": state_ok,
        "ra8_state_ms": state_ms,
        "ra8_evidence": state,
        "pass": ack_ok and state_ok,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--credentials",
        type=Path,
        default=Path("server/data/deployment_credentials.txt"),
    )
    parser.add_argument("--output", type=Path, default=Path("test_results"))
    parser.add_argument("--record-seconds", type=float, default=3.0)
    args = parser.parse_args()

    api = MobileApi(load_credentials(args.credentials))
    initial = api.snapshot()
    health = ((initial.get("telemetry") or {}).get("health") or {})
    device_online = bool((initial.get("device") or {}).get("online"))
    ra8_online = bool(health.get("ra8_online"))
    if not device_online or not ra8_online:
        raise RuntimeError(f"device is not ready: device_online={device_online}, ra8_online={ra8_online}")

    results: list[dict[str, Any]] = []
    initial_sequence = field_int(initial.get("telemetry"), "sequence")
    results.append(
        run_action(
            api,
            "device.refresh",
            {},
            lambda snap: (
                field_int(snap.get("telemetry"), "sequence") > initial_sequence,
                {"telemetry_sequence": field_int(snap.get("telemetry"), "sequence")},
            ),
            8.0,
        )
    )

    before_audio = sensors(api.snapshot()).get("audio_player") or {}
    audio_command_sequence = field_int(before_audio, "cmd_seq")
    results.append(
        run_action(
            api,
            "audio.play_boot",
            {},
            lambda snap: (
                (player := sensors(snap).get("audio_player") or {}).get("file") == "BOOT.WAV"
                and field_int(player, "cmd_seq") > audio_command_sequence
                and field_int(player, "ack") == field_int(player, "cmd_seq"),
                {
                    "cmd_seq": field_int(player, "cmd_seq"),
                    "ack": field_int(player, "ack"),
                    "state": field_int(player, "s"),
                    "file": player.get("file", ""),
                    "elapsed_ms": field_int(player, "elapsed"),
                },
            ),
            10.0,
        )
    )

    mic_name = "T" + datetime.now().strftime("%H%M%S")[-6:]
    before_mic = sensors(api.snapshot()).get("microphone") or {}
    mic_start_sequence = field_int(before_mic, "cmd_seq")
    results.append(
        run_action(
            api,
            "microphone.record.start",
            {"filename": mic_name},
            lambda snap: (
                (mic := sensors(snap).get("microphone") or {}).get("rec") == 1
                and field_int(mic, "cmd_seq") > mic_start_sequence
                and field_int(mic, "ack") == field_int(mic, "cmd_seq"),
                {
                    "cmd_seq": field_int(mic, "cmd_seq"),
                    "ack": field_int(mic, "ack"),
                    "record_active": field_int(mic, "rec"),
                    "writer_state": field_int(mic, "wst"),
                    "writer_error": field_int(mic, "werr"),
                    "file": mic.get("file", ""),
                },
            ),
            12.0,
        )
    )
    if results[-1]["pass"]:
        time.sleep(max(1.0, args.record_seconds))

    before_stop = sensors(api.snapshot()).get("microphone") or {}
    mic_stop_sequence = field_int(before_stop, "cmd_seq")
    results.append(
        run_action(
            api,
            "microphone.record.stop",
            {},
            lambda snap: (
                (mic := sensors(snap).get("microphone") or {}).get("rec") == 0
                and field_int(mic, "cmd_seq") > mic_stop_sequence
                and field_int(mic, "ack") == field_int(mic, "cmd_seq")
                and field_int(mic, "wbytes") > 44
                and field_int(mic, "werr") == 0,
                {
                    "cmd_seq": field_int(mic, "cmd_seq"),
                    "ack": field_int(mic, "ack"),
                    "record_active": field_int(mic, "rec"),
                    "writer_state": field_int(mic, "wst"),
                    "writer_error": field_int(mic, "werr"),
                    "writer_bytes": field_int(mic, "wbytes"),
                    "file": mic.get("file", ""),
                },
            ),
            15.0,
        )
    )

    camera_name = "T" + datetime.now().strftime("%H%M%S")[-6:]
    before_camera = sensors(api.snapshot()).get("camera") or {}
    camera_sequence = field_int(before_camera, "cmd_seq")
    camera_saved = field_int(before_camera, "saved")
    results.append(
        run_action(
            api,
            "camera.capture",
            {"filename": camera_name},
            lambda snap: (
                (camera := sensors(snap).get("camera") or {})
                and field_int(camera, "cmd_seq") > camera_sequence
                and field_int(camera, "ack") == field_int(camera, "cmd_seq")
                and field_int(camera, "saved") > camera_saved
                and field_int(camera, "werr") == 0,
                {
                    "cmd_seq": field_int(camera, "cmd_seq"),
                    "ack": field_int(camera, "ack"),
                    "saved": field_int(camera, "saved"),
                    "writer_error": field_int(camera, "werr"),
                    "writer_bytes": field_int(camera, "wbytes"),
                    "file": camera.get("file", ""),
                    "soi": field_int(camera, "soi"),
                    "eoi": field_int(camera, "eoi"),
                },
            ),
            20.0,
        )
    )

    continuity_before = api.snapshot()
    before_sensors = sensors(continuity_before)
    before_mic = before_sensors.get("microphone") or {}
    before_camera = before_sensors.get("camera") or {}
    before_inference = (continuity_before.get("telemetry") or {}).get("inference") or {}
    before_vision = continuity_before.get("vision") or (continuity_before.get("telemetry") or {}).get("vision") or {}

    def continuity_check() -> tuple[bool, dict[str, Any]]:
        snapshot = api.snapshot()
        current_sensors = sensors(snapshot)
        current_mic = current_sensors.get("microphone") or {}
        current_camera = current_sensors.get("camera") or {}
        current_inference = (snapshot.get("telemetry") or {}).get("inference") or {}
        current_vision = snapshot.get("vision") or (snapshot.get("telemetry") or {}).get("vision") or {}
        evidence = {
            "microphone_status": field_int(current_mic, "s"),
            "microphone_last_fsp_error": field_int(current_mic, "fsp"),
            "microphone_read_delta": field_int(current_mic, "read") - field_int(before_mic, "read"),
            "cry_inference_delta": field_int(current_inference, "cry_n") - field_int(before_inference, "cry_n"),
            "camera_frame_delta": field_int(current_camera, "frames") - field_int(before_camera, "frames"),
            "vision_inference_delta": field_int(current_vision, "n") - field_int(before_vision, "n"),
        }
        ok = (
            evidence["microphone_status"] in {2, 3}
            and evidence["microphone_last_fsp_error"] == 0
            and evidence["microphone_read_delta"] > 0
            and evidence["cry_inference_delta"] > 0
            and evidence["camera_frame_delta"] > 0
            and evidence["vision_inference_delta"] > 0
        )
        return ok, evidence

    continuity_ok, continuity_evidence, continuity_ms = wait_until(
        "post-command continuity", continuity_check, 25.0, 0.5
    )
    final_snapshot = api.snapshot()
    results.append(
        {
            "action": "post_command.continuity",
            "args": {"timeout_seconds": 25},
            "issued_at": datetime.now().astimezone().isoformat(),
            "command_id": None,
            "gateway_status": "not_applicable",
            "delivery_attempts": None,
            "gateway_ack_ms": 0.0,
            "ra8_state_ok": continuity_ok,
            "ra8_state_ms": continuity_ms,
            "ra8_evidence": continuity_evidence,
            "pass": continuity_ok,
        }
    )

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = args.output / f"command_{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=True)
    report = {
        "generated_at": datetime.now().astimezone().isoformat(),
        "device_id": api.device_id,
        "device_online": device_online,
        "ra8_online": ra8_online,
        "results": results,
        "pass_count": sum(1 for item in results if item["pass"]),
        "test_count": len(results),
        "pass": all(item["pass"] for item in results),
    }
    (output_dir / "command_evidence.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    (output_dir / "final_snapshot.json").write_text(
        json.dumps(final_snapshot, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

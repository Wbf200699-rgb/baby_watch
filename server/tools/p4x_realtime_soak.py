from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import statistics
import struct
import time
from collections import defaultdict
from pathlib import Path
from typing import Any
from urllib.parse import quote, urlsplit, urlunsplit

import httpx
from websockets.asyncio.client import connect


S3MD_HEADER = struct.Struct(">4sBBHIQIHHHH")


def percentile(values: list[float], percent: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int((len(ordered) - 1) * percent)))
    return round(ordered[index], 3)


def websocket_url(server: str, path: str) -> str:
    parsed = urlsplit(server)
    scheme = "wss" if parsed.scheme == "https" else "ws"
    return urlunsplit((scheme, parsed.netloc, path, "", ""))


def basic_headers(user: str, password: str) -> dict[str, str]:
    if not user:
        return {}
    token = base64.b64encode(f"{user}:{password}".encode()).decode()
    return {"Authorization": f"Basic {token}"}


async def consume_media(
    server: str,
    device_id: str,
    kind: str,
    headers: dict[str, str],
    stop: asyncio.Event,
    counters: dict[str, int],
) -> None:
    path = f"/api/dashboard/media/{quote(device_id, safe='')}/{kind}/ws"
    url = websocket_url(server, path)
    last_sequence: int | None = None
    while not stop.is_set():
        try:
            async with connect(
                url,
                additional_headers=headers,
                max_size=1024 * 1024,
                ping_interval=20,
                ping_timeout=20,
                close_timeout=5,
            ) as websocket:
                counters[f"{kind}_connections"] += 1
                while not stop.is_set():
                    try:
                        message = await asyncio.wait_for(websocket.recv(), timeout=5.0)
                    except TimeoutError:
                        continue
                    if not isinstance(message, bytes) or len(message) < S3MD_HEADER.size:
                        counters[f"{kind}_invalid"] += 1
                        continue
                    magic, version, media_type, _, sequence, _, payload_len, *_ = S3MD_HEADER.unpack_from(message)
                    if magic != b"S3MD" or version != 1 or len(message) != S3MD_HEADER.size + payload_len:
                        counters[f"{kind}_invalid"] += 1
                        continue
                    expected_type = 1 if kind == "video" else 2
                    if media_type != expected_type:
                        counters[f"{kind}_invalid"] += 1
                        continue
                    if last_sequence is not None:
                        delta = (sequence - last_sequence) & 0xFFFFFFFF
                        if 1 < delta < 0x80000000:
                            counters[f"{kind}_sequence_gaps"] += delta - 1
                    last_sequence = sequence
                    counters[f"{kind}_frames"] += 1
        except asyncio.CancelledError:
            raise
        except Exception:
            counters[f"{kind}_disconnects"] += 1
            try:
                await asyncio.wait_for(stop.wait(), timeout=1.0)
            except TimeoutError:
                pass


async def run(args: argparse.Namespace) -> dict[str, Any]:
    headers = basic_headers(args.admin_user, args.admin_password)
    auth = (args.admin_user, args.admin_password) if args.admin_user else None
    stop = asyncio.Event()
    counters: dict[str, int] = defaultdict(int)
    samples: dict[str, list[float]] = defaultdict(list)
    connected_samples: dict[str, int] = defaultdict(int)
    queue_max: dict[str, int] = defaultdict(int)
    first_status: dict[str, Any] | None = None
    last_status: dict[str, Any] | None = None
    media_tasks: list[asyncio.Task[None]] = []
    if not args.no_media:
        for kind in ("video", "audio"):
            media_tasks.append(
                asyncio.create_task(
                    consume_media(args.server, args.device, kind, headers, stop, counters)
                )
            )

    started = time.monotonic()
    deadline = started + args.duration
    endpoint = f"/api/dashboard/devices/{quote(args.device, safe='')}/transport-status"
    try:
        async with httpx.AsyncClient(
            base_url=args.server.rstrip("/"),
            auth=auth,
            timeout=httpx.Timeout(10.0),
        ) as client:
            while time.monotonic() < deadline:
                try:
                    response = await client.get(endpoint)
                    response.raise_for_status()
                    status = response.json()
                    first_status = first_status or status
                    last_status = status
                    telemetry = status.get("telemetry", {})
                    matrices = status.get("matrices", {})
                    media = status.get("media", {})
                    video = media.get("video_diagnostics", {})
                    audio = media.get("audio_diagnostics", {})
                    for key, value in (
                        ("telemetry_fps", telemetry.get("telemetry_fps")),
                        ("pressure_fps", matrices.get("pressure_fps")),
                        ("thermal_fps", matrices.get("thermal_fps")),
                        ("telemetry_ack_ms", telemetry.get("telemetry_ack_p95_ms")),
                        ("pressure_ack_ms", matrices.get("pressure_ack_p95_ms")),
                        ("thermal_ack_ms", matrices.get("thermal_ack_p95_ms")),
                        ("video_fps", video.get("received_fps")),
                        ("audio_fps", audio.get("received_fps")),
                    ):
                        if isinstance(value, (int, float)):
                            samples[key].append(float(value))
                    for key, value in (
                        ("telemetry", telemetry.get("telemetry_connected")),
                        ("matrix", matrices.get("matrix_connected")),
                        ("video", video.get("video_device_connections", 0)),
                        ("audio", audio.get("audio_device_connections", 0)),
                    ):
                        if bool(value):
                            connected_samples[key] += 1
                    for key, value in (
                        ("telemetry", telemetry.get("telemetry_queue_depth", 0)),
                        ("telemetry_db", telemetry.get("telemetry_persistence_queue_depth", 0)),
                        ("pressure", matrices.get("pressure_queue_depth", 0)),
                        ("pressure_db", matrices.get("pressure_persistence_queue_depth", 0)),
                        ("thermal", matrices.get("thermal_queue_depth", 0)),
                        ("thermal_db", matrices.get("thermal_persistence_queue_depth", 0)),
                        ("video", video.get("queue_depth", 0)),
                        ("audio", audio.get("queue_depth", 0)),
                    ):
                        queue_max[key] = max(queue_max[key], int(value or 0))
                    counters["status_samples"] += 1
                except Exception:
                    counters["status_errors"] += 1
                remaining = max(0.0, deadline - time.monotonic())
                if remaining:
                    await asyncio.sleep(min(args.poll_interval, remaining))
    finally:
        stop.set()
        for task in media_tasks:
            task.cancel()
        if media_tasks:
            await asyncio.gather(*media_tasks, return_exceptions=True)

    elapsed = max(0.001, time.monotonic() - started)
    sample_count = max(1, counters["status_samples"])
    rates = {
        key: {
            "median": round(statistics.median(values), 3) if values else 0.0,
            "p05": percentile(values, 0.05),
            "p95": percentile(values, 0.95),
        }
        for key, values in samples.items()
    }
    result = {
        "device_id": args.device,
        "duration_seconds": round(elapsed, 3),
        "status_samples": counters["status_samples"],
        "status_errors": counters["status_errors"],
        "connected_ratio": {
            key: round(value / sample_count, 4) for key, value in connected_samples.items()
        },
        "rates": rates,
        "queue_max": dict(queue_max),
        "listener_frames": {
            "video": counters["video_frames"],
            "audio": counters["audio_frames"],
            "video_fps": round(counters["video_frames"] / elapsed, 3),
            "audio_fps": round(counters["audio_frames"] / elapsed, 3),
            "video_invalid": counters["video_invalid"],
            "audio_invalid": counters["audio_invalid"],
            "video_sequence_gaps": counters["video_sequence_gaps"],
            "audio_sequence_gaps": counters["audio_sequence_gaps"],
            "video_disconnects": counters["video_disconnects"],
            "audio_disconnects": counters["audio_disconnects"],
        },
        "counter_delta": counter_delta(first_status, last_status),
    }
    result["acceptance"] = acceptance(result, include_media=not args.no_media)
    return result


def nested_int(data: dict[str, Any] | None, *path: str) -> int:
    value: Any = data or {}
    for key in path:
        value = value.get(key, {}) if isinstance(value, dict) else {}
    return int(value) if isinstance(value, (int, float)) else 0


def counter_delta(first: dict[str, Any] | None, last: dict[str, Any] | None) -> dict[str, int]:
    paths = {
        "telemetry_drops": ("telemetry", "telemetry_drops"),
        "matrix_drops": ("matrices", "matrix_drops"),
        "telemetry_reconnects": ("telemetry", "telemetry_reconnects"),
        "matrix_reconnects": ("matrices", "matrix_reconnects"),
        "video_reconnects": ("media", "video_diagnostics", "reconnects"),
        "audio_reconnects": ("media", "audio_diagnostics", "reconnects"),
    }
    return {
        name: max(0, nested_int(last, *path) - nested_int(first, *path))
        for name, path in paths.items()
    }


def acceptance(result: dict[str, Any], *, include_media: bool) -> dict[str, bool]:
    rates = result.get("rates", {})
    connected = result.get("connected_ratio", {})

    def p05(name: str) -> float:
        return float(rates.get(name, {}).get("p05", 0.0))

    checks = {
        "status_poll_reliable": result["status_errors"] == 0,
        "telemetry_connected": connected.get("telemetry", 0.0) >= 0.99,
        "matrix_connected": connected.get("matrix", 0.0) >= 0.99,
        "telemetry_at_least_3_5_hz": p05("telemetry_fps") >= 3.5,
        "pressure_at_least_7_hz": p05("pressure_fps") >= 7.0,
        "thermal_at_least_3_5_hz": p05("thermal_fps") >= 3.5,
        "bounded_server_queues": max(result.get("queue_max", {}).values(), default=0) <= 8,
    }
    if include_media:
        listener = result.get("listener_frames", {})
        checks.update(
            {
                "video_connected": connected.get("video", 0.0) >= 0.99,
                "audio_connected": connected.get("audio", 0.0) >= 0.99,
                "video_at_least_7_fps": float(listener.get("video_fps", 0.0)) >= 7.0,
                "audio_at_least_45_fps": float(listener.get("audio_fps", 0.0)) >= 45.0,
                "media_frames_valid": not listener.get("video_invalid") and not listener.get("audio_invalid"),
            }
        )
    return checks


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32-P4X-EYE concurrent realtime soak monitor")
    parser.add_argument("--server", default=os.getenv("BABY_MONITOR_SERVER", "http://127.0.0.1:8088"))
    parser.add_argument("--device", default=os.getenv("BABY_MONITOR_DEVICE_ID", "esp32cam-01"))
    parser.add_argument("--admin-user", default=os.getenv("BABY_MONITOR_ADMIN_USER", ""))
    parser.add_argument("--admin-password", default=os.getenv("BABY_MONITOR_ADMIN_PASSWORD", ""))
    parser.add_argument("--duration", type=float, default=1800.0)
    parser.add_argument("--poll-interval", type=float, default=2.0)
    parser.add_argument("--no-media", action="store_true")
    parser.add_argument("--strict", action="store_true", help="return nonzero when any acceptance check fails")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = asyncio.run(run(args))
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    if args.strict and not all(result["acceptance"].values()):
        raise SystemExit(1)


if __name__ == "__main__":
    main()

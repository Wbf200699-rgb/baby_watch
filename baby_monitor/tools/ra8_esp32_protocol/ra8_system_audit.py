from __future__ import annotations

import argparse
import base64
import json
import math
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CREDENTIALS = REPO_ROOT / "server/data/deployment_credentials.txt"


def _load_credentials(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        separator = "=" if "=" in line else ":" if ":" in line else ""
        if not separator:
            continue
        key, value = line.split(separator, 1)
        values[key.strip()] = value.strip()
    return values


def _get(data: Any, *path: str, default: Any = None) -> Any:
    current = data
    for key in path:
        if not isinstance(current, dict) or key not in current:
            return default
        current = current[key]
    return current


def _as_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _percentile(values: list[int], percentile: float) -> int | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(percentile * len(ordered)) - 1))
    return ordered[index]


def _check(check_id: str, status: str, detail: str, **metrics: Any) -> dict[str, Any]:
    return {"id": check_id, "status": status, "detail": detail, "metrics": metrics}


def _counter_progress(samples: list[dict[str, Any]], *path: str) -> tuple[int, int, bool]:
    values = [_as_int(_get(sample, *path)) for sample in samples]
    return values[0], values[-1], any(value != values[0] for value in values[1:])


def _fetch_snapshot(url: str, device_id: str, user: str, password: str, timeout: float) -> dict[str, Any]:
    endpoint = f"{url.rstrip('/')}/api/dashboard/snapshot?{urllib.parse.urlencode({'device_id': device_id})}"
    request = urllib.request.Request(endpoint)
    if user:
        token = base64.b64encode(f"{user}:{password}".encode("utf-8")).decode("ascii")
        request.add_header("Authorization", f"Basic {token}")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def _build_report(snapshots: list[dict[str, Any]], device_id: str, server_url: str) -> dict[str, Any]:
    last_snapshot = snapshots[-1]
    telemetry_samples = [
        sample.get("telemetry") if isinstance(sample.get("telemetry"), dict) else {}
        for sample in snapshots
    ]
    telemetry = telemetry_samples[-1]
    health = _get(telemetry, "health", default={})
    gateway = _get(health, "gateway", default={})
    link = _get(health, "ra8_link", default={})
    sensors = _get(telemetry, "sensors", default={})
    inference = _get(telemetry, "inference", default={})
    vision = _get(telemetry, "vision", default={})
    devices = last_snapshot.get("devices") if isinstance(last_snapshot.get("devices"), list) else []
    device = next((item for item in devices if item.get("device_id") == device_id), {})
    checks: list[dict[str, Any]] = []

    gateway_online = bool(device.get("online"))
    checks.append(
        _check(
            "gateway_online",
            "pass" if gateway_online else "fail",
            "ESP32-S3 server session is online" if gateway_online else "ESP32-S3 is offline at the server",
            firmware=device.get("firmware"),
        )
    )

    ra8_online = bool(_get(health, "ra8_online"))
    status_age = _as_int(_get(health, "ra8_status_age_ms"), 0xFFFFFFFF)
    checks.append(
        _check(
            "ra8_online",
            "pass" if ra8_online and status_age <= 2000 else "fail",
            f"RA8 online={int(ra8_online)}, status age={status_age} ms",
            status_age_ms=status_age,
        )
    )

    status_first, status_last, status_progress = _counter_progress(
        telemetry_samples, "health", "uart_status_frames"
    )
    checks.append(
        _check(
            "status_progress",
            "pass" if status_progress else "fail",
            f"S3 status frames {status_first} -> {status_last}",
            first=status_first,
            last=status_last,
        )
    )

    tick_first, tick_last, tick_progress = _counter_progress(telemetry_samples, "ra8_tick")
    checks.append(
        _check(
            "ra8_tick_progress",
            "pass" if tick_progress else "fail",
            f"RA8 tick {tick_first} -> {tick_last}",
            first=tick_first,
            last=tick_last,
        )
    )

    if isinstance(gateway, dict) and "ready_tx_count" in gateway:
        ready_tx = _as_int(gateway.get("ready_tx_count"))
        ready_errors = _as_int(gateway.get("ready_tx_errors"))
        uart_rx_bytes = _as_int(gateway.get("uart_rx_bytes"))
        valid_frames = _as_int(health.get("uart_valid_frames"))
        active_baud = _as_int(gateway.get("uart_active_baud"))
        baud_locked = bool(gateway.get("uart_baud_locked"))
        sync_headers = _as_int(gateway.get("uart_sync_headers"))
        sample_hex = str(gateway.get("uart_sample_hex") or "")
        link_ok = ready_tx > 0 and ready_errors == 0 and uart_rx_bytes > 0 and valid_frames > 0
        checks.append(
            _check(
                "s3_uart_readiness",
                "pass" if link_ok else "fail",
                (
                    f"ready sent={ready_tx}, errors={ready_errors}, UART RX={uart_rx_bytes} B, "
                    f"valid={valid_frames}, baud={active_baud or '-'}, locked={int(baud_locked)}, "
                    f"sync={sync_headers}, sample={sample_hex or '-'}"
                ),
                ready_tx_count=ready_tx,
                ready_tx_errors=ready_errors,
                uart_rx_bytes=uart_rx_bytes,
                valid_frames=valid_frames,
                active_baud=active_baud,
                baud_locked=baud_locked,
                sync_headers=sync_headers,
                sample_hex=sample_hex,
            )
        )
    else:
        checks.append(
            _check(
                "s3_uart_readiness",
                "unavailable",
                "S3 firmware does not expose gateway UART readiness counters",
            )
        )

    if isinstance(link, dict) and "st_try" in link:
        attempts = _as_int(link.get("st_try"))
        successes = _as_int(link.get("st_ok"))
        too_long = _as_int(link.get("st_long"))
        uart_errors = _as_int(link.get("st_uart"))
        payload_length = _as_int(link.get("st_len"))
        checks.append(
            _check(
                "ra8_status_sender",
                "pass" if attempts > 0 and successes > 0 and too_long == 0 and uart_errors == 0 else "fail",
                (
                    f"status success/attempt={successes}/{attempts}, length={payload_length}, "
                    f"too-long={too_long}, UART errors={uart_errors}"
                ),
                attempts=attempts,
                successes=successes,
                payload_length=payload_length,
                payload_too_long=too_long,
                uart_errors=uart_errors,
                status_detail=_as_int(link.get("st_detail")),
            )
        )
    else:
        checks.append(_check("ra8_status_sender", "unavailable", "RA8 link test fields are absent"))

    thermal_samples = [
        _get(sample, "sensors", "thermal", default={}) for sample in telemetry_samples
    ]
    thermal = thermal_samples[-1] if thermal_samples else {}

    def thermal_sample_valid(sample: dict[str, Any]) -> bool:
        return (
            _as_int(sample.get("s"), -1) == 3
            and _as_int(sample.get("rx")) == 10248
            and _as_int(sample.get("valid")) == 5120
            and sample.get("crc_rx") is not None
            and _as_int(sample.get("crc_rx")) == _as_int(sample.get("crc_calc"))
        )

    thermal_valid_samples = sum(1 for sample in thermal_samples if thermal_sample_valid(sample))
    thermal_sequences = [_as_int(sample.get("seq")) for sample in thermal_samples]
    thermal_sequence_progress = len(set(thermal_sequences)) > 1
    thermal_error_values = [_as_int(sample.get("uart_err")) for sample in thermal_samples]
    thermal_error_progress = any(value != thermal_error_values[0] for value in thermal_error_values[1:])
    thermal_ok = (
        bool(thermal_samples)
        and thermal_valid_samples == len(thermal_samples)
        and thermal_sequence_progress
        and not thermal_error_progress
    )
    checks.append(
        _check(
            "thermal_frame",
            "pass" if thermal_ok else "fail",
            (
                f"valid samples={thermal_valid_samples}/{len(thermal_samples)}, "
                f"sequence={thermal_sequences[0] if thermal_sequences else 0}->"
                f"{thermal_sequences[-1] if thermal_sequences else 0}, "
                f"bytes={_as_int(thermal.get('rx'))}, pixels={_as_int(thermal.get('valid'))}, "
                f"CRC={thermal.get('crc_rx')}/{thermal.get('crc_calc')}, "
                f"UART errors={thermal_error_values[0] if thermal_error_values else 0}->"
                f"{thermal_error_values[-1] if thermal_error_values else 0}"
            ),
            valid_samples=thermal_valid_samples,
            samples=len(thermal_samples),
            first_sequence=thermal_sequences[0] if thermal_sequences else 0,
            last_sequence=thermal_sequences[-1] if thermal_sequences else 0,
            frame_bytes=_as_int(thermal.get("rx")),
            valid_pixels=_as_int(thermal.get("valid")),
            crc_rx=_as_int(thermal.get("crc_rx")),
            crc_calc=_as_int(thermal.get("crc_calc")),
            uart_errors_first=thermal_error_values[0] if thermal_error_values else 0,
            uart_errors_last=thermal_error_values[-1] if thermal_error_values else 0,
        )
    )

    temperature = sensors.get("bed_temperature") if isinstance(sensors.get("bed_temperature"), dict) else {}
    connected = _as_int(temperature.get("ok")) & 0x1F
    temp_errors = _as_int(temperature.get("err")) & 0x1F
    checks.append(
        _check(
            "temperature_channels",
            "pass" if connected == 0x1F and temp_errors == 0 else "fail",
            f"TMP117 connected=0x{connected:02X}, errors=0x{temp_errors:02X}",
            connected_mask=connected,
            error_mask=temp_errors,
        )
    )

    pressure = sensors.get("pressure") if isinstance(sensors.get("pressure"), dict) else {}
    pressure_state = _as_int(pressure.get("s"), -1)
    checks.append(
        _check(
            "pressure_stream",
            "pass" if pressure_state in (2, 3) else "fail",
            f"pressure state={pressure_state}, last error={_as_int(pressure.get('fsp'))}",
        )
    )

    microphone = sensors.get("microphone") if isinstance(sensors.get("microphone"), dict) else {}
    mic_first, mic_last, mic_progress = _counter_progress(
        telemetry_samples, "sensors", "microphone", "read"
    )
    mic_error_first, mic_error_last, mic_error_progress = _counter_progress(
        telemetry_samples, "sensors", "microphone", "err"
    )
    mic_state = _as_int(microphone.get("s"), -1)
    checks.append(
        _check(
            "microphone_capture",
            "pass" if mic_state in (2, 3) and mic_progress and not mic_error_progress else "fail",
            (
                f"mic state={mic_state}, read={mic_first}->{mic_last}, "
                f"cumulative errors={mic_error_first}->{mic_error_last}"
            ),
            peak=_as_int(microphone.get("peak")),
            mean=_as_int(microphone.get("mean")),
            cumulative_errors=mic_error_last,
        )
    )

    if "sdr" in microphone:
        sd_ready = _as_int(microphone.get("sdr")) == 1
        sd_step = _as_int(microphone.get("sds"))
        sd_failure = any(
            _as_int(microphone.get(field)) != 0 for field in ("sdfs", "sdfsp", "sdff", "sdfail")
        )
        checks.append(
            _check(
                "sd_ready",
                "pass" if sd_ready and not sd_failure and sd_step not in (4, 5, 6) else "fail",
                f"SD ready={int(sd_ready)}, step={sd_step}, failure={int(sd_failure)}",
            )
        )
    else:
        checks.append(_check("sd_ready", "unavailable", "SD diagnostic fields are absent"))

    camera = sensors.get("camera") if isinstance(sensors.get("camera"), dict) else {}
    if camera:
        camera_ok = bool(_as_int(camera.get("probe_ok"))) and _as_int(camera.get("s"), -1) in (4, 5)
        checks.append(
            _check(
                "serial_camera",
                "pass" if camera_ok else "fail",
                (
                    f"camera state={_as_int(camera.get('s'), -1)}, probe={_as_int(camera.get('probe_ok'))}, "
                    f"saved={_as_int(camera.get('saved'))}, writer error={_as_int(camera.get('werr'))}"
                ),
            )
        )
    else:
        checks.append(_check("serial_camera", "unavailable", "Serial-camera status is absent"))

    vision_ready = _as_int(vision.get("r"))
    vision_first, vision_last, vision_progress = _counter_progress(telemetry_samples, "vision", "n")
    vision_errors = vision.get("e") if isinstance(vision.get("e"), list) else []
    npu_errors = _as_int(vision_errors[1]) if len(vision_errors) > 1 else 0
    checks.append(
        _check(
            "vision_runtime",
            "pass" if vision_ready == 7 and vision_progress and npu_errors == 0 else "fail",
            f"models=0x{vision_ready:X}, inference={vision_first}->{vision_last}, NPU errors={npu_errors}",
        )
    )

    if isinstance(link, dict) and "heap_min" in link:
        heap_free = _as_int(link.get("heap"))
        heap_min = _as_int(link.get("heap_min"))
        stack = link.get("stk") if isinstance(link.get("stk"), list) else []
        stack_values = [_as_int(value) for value in stack]
        stack_ok = len(stack_values) == 6 and min(stack_values, default=0) >= 128
        heap_available = heap_free > 0 and heap_min > 0
        resource_status = (
            "pass"
            if heap_available and heap_min >= 8192 and stack_ok
            else "unavailable"
            if not heap_available and stack_ok
            else "fail"
        )
        checks.append(
            _check(
                "runtime_resources",
                resource_status,
                (
                    f"heap={heap_free}, minimum={heap_min}, stack high-water={stack_values}; "
                    + ("heap statistics unavailable" if not heap_available else "heap statistics available")
                ),
                heap_free=heap_free,
                heap_minimum=heap_min,
                stack_high_water=stack_values,
            )
        )
    else:
        checks.append(_check("runtime_resources", "unavailable", "Heap/stack test fields are absent"))

    total_times: list[int] = []
    for sample in telemetry_samples:
        timings = _get(sample, "vision", "t", default=[])
        if isinstance(timings, list) and len(timings) >= 6:
            total_times.append(_as_int(timings[5]))
    latency_metrics = {
        "samples": len(total_times),
        "mean_ms": round(sum(total_times) / len(total_times), 2) if total_times else None,
        "p95_ms": _percentile(total_times, 0.95),
        "max_ms": max(total_times) if total_times else None,
    }
    checks.append(
        _check(
            "vision_latency_observed",
            "pass" if total_times else "unavailable",
            f"vision latency samples={len(total_times)}, mean={latency_metrics['mean_ms']} ms, P95={latency_metrics['p95_ms']} ms",
            **latency_metrics,
        )
    )

    counts = {
        status: sum(1 for check in checks if check["status"] == status)
        for status in ("pass", "fail", "unavailable")
    }
    safe_server = urllib.parse.urlsplit(server_url)
    return {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "server": f"{safe_server.scheme}://{safe_server.netloc}",
        "device_id": device_id,
        "sample_count": len(snapshots),
        "summary": counts,
        "checks": checks,
        "manual_tests_remaining": [
            "tagged precision/recall/F1 and confusion matrices",
            "float vs INT8 vs board-output consistency",
            "Level-3 alarm speaker/ACK/server/mobile closed loop",
            "fault injection and recovery timing",
            "multi-hour stability and SD file-integrity run",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit the live RA8 -> S3 -> server telemetry chain")
    parser.add_argument("--credentials", type=Path, default=DEFAULT_CREDENTIALS)
    parser.add_argument("--url")
    parser.add_argument("--device-id")
    parser.add_argument("--user")
    parser.add_argument("--password")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    credentials = _load_credentials(args.credentials) if args.credentials.exists() else {}
    url = args.url or credentials.get("URL", "")
    device_id = args.device_id or credentials.get("DEVICE_ID", "")
    user = args.user if args.user is not None else credentials.get("ADMIN_USER", "")
    password = args.password if args.password is not None else credentials.get("ADMIN_PASSWORD", "")
    if not url or not device_id:
        parser.error("URL and device ID are required via arguments or credentials file")
    if args.samples < 2:
        parser.error("--samples must be at least 2 to verify counter progress")

    snapshots = []
    for index in range(args.samples):
        snapshots.append(_fetch_snapshot(url, device_id, user, password, args.timeout))
        if index + 1 < args.samples:
            time.sleep(args.interval)

    report = _build_report(snapshots, device_id, url)
    rendered = json.dumps(report, ensure_ascii=False, indent=2)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 2 if args.strict and report["summary"]["fail"] else 0


if __name__ == "__main__":
    raise SystemExit(main())

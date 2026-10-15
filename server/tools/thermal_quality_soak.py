from __future__ import annotations

import argparse
import base64
import json
import math
import os
import statistics
import time
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from urllib.parse import quote
from urllib.request import Request, urlopen


THERMAL_FRAME_BYTES = 10248
THERMAL_POINT_COUNT = 5120
THERMAL_ROWS = 64
THERMAL_COLS = 80
THERMAL_BUILD = 0x20260731


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int((len(ordered) - 1) * fraction)))
    return round(ordered[index], 3)


def u32_delta(first: int, last: int) -> int:
    return (last - first) & 0xFFFFFFFF


def as_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def fetch_json(url: str, authorization: str, timeout: float) -> dict[str, Any]:
    request = Request(url, headers={"Authorization": authorization})
    with urlopen(request, timeout=timeout) as response:
        return json.load(response)


def main() -> None:
    parser = argparse.ArgumentParser(description="Monitor RA8 thermal frame quality")
    parser.add_argument("--server", default=os.getenv("BABY_MONITOR_SERVER", "http://127.0.0.1:8088"))
    parser.add_argument("--device", default=os.getenv("BABY_MONITOR_DEVICE_ID", "esp32cam-01"))
    parser.add_argument("--admin-user", default=os.getenv("BABY_MONITOR_ADMIN_USER", ""))
    parser.add_argument("--admin-password", default=os.getenv("BABY_MONITOR_ADMIN_PASSWORD", ""))
    parser.add_argument("--duration", type=float, default=1800.0)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if not args.admin_user:
        raise SystemExit("admin credentials are required")

    token = base64.b64encode(f"{args.admin_user}:{args.admin_password}".encode()).decode()
    authorization = f"Basic {token}"
    endpoint = (
        f"{args.server.rstrip('/')}/api/mobile/v1/devices/"
        f"{quote(args.device, safe='')}/snapshot"
    )

    started_wall = datetime.now(timezone.utc)
    started = time.monotonic()
    deadline = started + max(0.0, args.duration)
    quality_counts: Counter[str] = Counter()
    violations: Counter[str] = Counter()
    builds: Counter[str] = Counter()
    matrix_fps: list[float] = []
    accepted_means: list[int] = []
    accepted_p01: list[int] = []
    accepted_p99: list[int] = []
    unique_sequences: list[int] = []
    first_counters: dict[str, int] | None = None
    last_counters: dict[str, int] | None = None
    samples = 0
    http_errors = 0
    last_unique_sequence: int | None = None

    counter_names = (
        "err",
        "uart_err",
        "ovf",
        "crc_bad",
        "data_bad",
        "transient_drop_count",
        "generation_retry_count",
    )

    while time.monotonic() < deadline:
        loop_started = time.monotonic()
        try:
            snapshot = fetch_json(endpoint, authorization, args.timeout)
            thermal = (((snapshot.get("telemetry") or {}).get("sensors") or {}).get("thermal") or {})
            matrix = ((snapshot.get("matrices") or {}).get("thermal") or {})
            samples += 1

            state = as_int(thermal.get("s"), -1)
            received = as_int(thermal.get("rx"), -1)
            valid = as_int(thermal.get("valid"), -1)
            sequence = as_int(thermal.get("seq")) & 0xFFFFFFFF
            generation = as_int(thermal.get("write_generation")) & 0xFFFFFFFF
            quality = as_int(thermal.get("frame_quality"), 2)
            last_good = as_int(thermal.get("last_good_sequence")) & 0xFFFFFFFF
            crc_rx = as_int(thermal.get("crc_rx"), -1)
            crc_calc = as_int(thermal.get("crc_calc"), -2)
            build = as_int(thermal.get("build")) & 0xFFFFFFFF

            quality_counts[str(quality)] += 1
            builds[f"0x{build:08X}"] += 1
            if state != 3:
                violations["state"] += 1
            if received != THERMAL_FRAME_BYTES:
                violations["frame_length"] += 1
            if valid != THERMAL_POINT_COUNT:
                violations["valid_points"] += 1
            if crc_rx != crc_calc:
                violations["crc"] += 1
            if generation & 1:
                violations["odd_generation"] += 1
            if quality == 0 and last_good != sequence:
                violations["last_good_sequence"] += 1
            if build != THERMAL_BUILD:
                violations["build"] += 1

            rows = as_int(matrix.get("rows"), -1)
            cols = as_int(matrix.get("cols"), -1)
            if matrix and (rows != THERMAL_ROWS or cols != THERMAL_COLS):
                violations["matrix_dimensions"] += 1
            fps = matrix.get("fps")
            if isinstance(fps, (int, float)) and math.isfinite(float(fps)):
                matrix_fps.append(float(fps))

            counters = {name: as_int(thermal.get(name)) & 0xFFFFFFFF for name in counter_names}
            first_counters = first_counters or counters
            last_counters = counters

            if quality == 0 and sequence != last_unique_sequence:
                last_unique_sequence = sequence
                unique_sequences.append(sequence)
                accepted_means.append(as_int(thermal.get("mean_cd")))
                accepted_p01.append(as_int(thermal.get("p01_cd")))
                accepted_p99.append(as_int(thermal.get("p99_cd")))
        except Exception:
            http_errors += 1

        delay = args.interval - (time.monotonic() - loop_started)
        if delay > 0:
            time.sleep(min(delay, max(0.0, deadline - time.monotonic())))

    elapsed = max(0.001, time.monotonic() - started)
    counter_delta = {
        name: u32_delta((first_counters or {}).get(name, 0), (last_counters or {}).get(name, 0))
        for name in counter_names
    }
    mean_steps = [abs(right - left) for left, right in zip(accepted_means, accepted_means[1:])]
    source_frames = (
        u32_delta(unique_sequences[0], unique_sequences[-1])
        if len(unique_sequences) >= 2
        else 0
    )
    transport_error_growth = sum(counter_delta[name] for name in ("err", "uart_err", "ovf", "crc_bad", "data_bad"))

    acceptance = {
        "http_polling_reliable": http_errors == 0,
        "new_thermal_build_active": violations["build"] == 0 and samples > 0,
        "complete_crc_valid_frames": not any(
            violations[name] for name in ("state", "frame_length", "valid_points", "crc")
        ),
        "seqlock_never_exposed_odd_generation": violations["odd_generation"] == 0,
        "normal_frames_match_last_good_sequence": violations["last_good_sequence"] == 0,
        "matrix_layout_is_64_by_80": violations["matrix_dimensions"] == 0,
        "transport_error_counters_stable": transport_error_growth == 0,
        "thermal_display_rate_at_least_1_5_hz": (
            statistics.median(matrix_fps) if matrix_fps else 0.0
        ) >= 1.5,
    }

    result = {
        "device_id": args.device,
        "started_at": started_wall.isoformat(),
        "finished_at": datetime.now(timezone.utc).isoformat(),
        "duration_seconds": round(elapsed, 3),
        "samples": samples,
        "http_errors": http_errors,
        "quality_counts": dict(quality_counts),
        "builds": dict(builds),
        "violations": dict(violations),
        "counter_delta": counter_delta,
        "accepted_unique_samples": len(unique_sequences),
        "source_sequence_delta": source_frames,
        "source_sequence_rate_hz": round(source_frames / elapsed, 3),
        "accepted_temperature_c": {
            "p01_min": round(min(accepted_p01) / 10.0, 1) if accepted_p01 else None,
            "p01_max": round(max(accepted_p01) / 10.0, 1) if accepted_p01 else None,
            "p99_min": round(min(accepted_p99) / 10.0, 1) if accepted_p99 else None,
            "p99_max": round(max(accepted_p99) / 10.0, 1) if accepted_p99 else None,
            "mean_min": round(min(accepted_means) / 10.0, 1) if accepted_means else None,
            "mean_max": round(max(accepted_means) / 10.0, 1) if accepted_means else None,
            "maximum_mean_step": round(max(mean_steps, default=0) / 10.0, 1),
        },
        "matrix_fps": {
            "median": round(statistics.median(matrix_fps), 3) if matrix_fps else None,
            "p05": percentile(matrix_fps, 0.05),
            "p95": percentile(matrix_fps, 0.95),
        },
        "acceptance": acceptance,
        "passed": all(acceptance.values()),
    }

    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

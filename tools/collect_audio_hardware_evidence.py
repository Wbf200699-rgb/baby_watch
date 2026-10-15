#!/usr/bin/env python3
"""Collect live RA8 microphone and cry-inference timing evidence."""

from __future__ import annotations

import argparse
import base64
import csv
import json
import math
import time
from datetime import datetime
from pathlib import Path
from typing import Any
from urllib.error import URLError
from urllib.request import Request, urlopen


def credentials(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def integer(value: Any) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", type=int, default=50)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--credentials", type=Path, default=Path("server/data/deployment_credentials.txt"))
    parser.add_argument("--output", type=Path, default=Path("test_results"))
    parser.add_argument(
        "--expect-no-cry",
        action="store_true",
        help="Fail when a quiet/non-cry run contains a confirmed cry or too many raw hits.",
    )
    parser.add_argument("--raw-threshold", type=int, default=650)
    parser.add_argument("--allowed-raw-hits", type=int, default=0)
    parser.add_argument("--allowed-confirmed-samples", type=int, default=0)
    args = parser.parse_args()

    cfg = credentials(args.credentials)
    token = base64.b64encode(f'{cfg["ADMIN_USER"]}:{cfg["ADMIN_PASSWORD"]}'.encode()).decode()
    url = cfg["URL"].rstrip("/") + f'/api/mobile/v1/devices/{cfg["DEVICE_ID"]}/snapshot'
    headers = {"Authorization": "Basic " + token, "Accept": "application/json"}

    samples: list[dict[str, Any]] = []
    seen: set[int] = set()
    request_errors = 0
    started = time.monotonic()
    while len(samples) < args.frames and time.monotonic() - started < args.timeout:
        try:
            with urlopen(Request(url, headers=headers), timeout=15) as response:
                snapshot = json.load(response)
        except (OSError, TimeoutError, URLError) as error:
            request_errors += 1
            print(f"Snapshot request failed ({request_errors}): {error}")
            time.sleep(0.5)
            continue
        telemetry = snapshot.get("telemetry") or {}
        mic = (telemetry.get("sensors") or {}).get("microphone") or {}
        inference = telemetry.get("inference") or {}
        sequence = integer(inference.get("cry_n"))
        if sequence > 0 and sequence not in seen:
            seen.add(sequence)
            samples.append(
                {
                    "observed_at": datetime.now().astimezone().isoformat(),
                    "cry_sequence": sequence,
                    "cry_ms": integer(inference.get("cry_ms")),
                    "cry_score": integer(inference.get("cry")),
                    "cry_raw": integer(inference.get("cry_raw")),
                    "mic_status": integer(mic.get("s")),
                    "mic_fsp": integer(mic.get("fsp")),
                    "mic_reads": integer(mic.get("read")),
                    "mic_errors": integer(mic.get("err")),
                    "mic_peak": integer(mic.get("peak")),
                    "vision_sequence": integer((snapshot.get("vision") or telemetry.get("vision") or {}).get("n")),
                }
            )
            print(f"Captured cry inference {sequence} ({len(samples)}/{args.frames})")
        time.sleep(0.1)

    if not samples:
        raise RuntimeError("no cry inference samples captured")
    timings = [integer(item["cry_ms"]) for item in samples]
    raw_scores = [integer(item["cry_raw"]) for item in samples]
    confirmed_scores = [integer(item["cry_score"]) for item in samples]
    read_delta = integer(samples[-1]["mic_reads"]) - integer(samples[0]["mic_reads"])
    vision_delta = integer(samples[-1]["vision_sequence"]) - integer(samples[0]["vision_sequence"])
    summary = {
        "generated_at": datetime.now().astimezone().isoformat(),
        "target_frames": args.frames,
        "captured_frames": len(samples),
        "first_sequence": samples[0]["cry_sequence"],
        "last_sequence": samples[-1]["cry_sequence"],
        "microphone_read_delta": read_delta,
        "vision_inference_delta": vision_delta,
        "cry_ms_average": round(sum(timings) / len(timings), 2),
        "cry_ms_p95": percentile(timings, 0.95),
        "cry_raw_p50": percentile(raw_scores, 0.50),
        "cry_raw_p90": percentile(raw_scores, 0.90),
        "cry_raw_p95": percentile(raw_scores, 0.95),
        "cry_raw_maximum": max(raw_scores),
        "raw_threshold": args.raw_threshold,
        "raw_hit_samples": sum(score >= args.raw_threshold for score in raw_scores),
        "confirmed_cry_samples": sum(score >= 75 for score in confirmed_scores),
        "max_mic_error_count": max(integer(item["mic_errors"]) for item in samples),
        "status_fault_samples": sum(
            1 for item in samples if integer(item["mic_status"]) not in {2, 3} or integer(item["mic_fsp"]) != 0
        ),
        "snapshot_request_errors": request_errors,
    }
    summary["pass"] = (
        len(samples) == args.frames
        and read_delta > 0
        and summary["status_fault_samples"] == 0
        and summary["cry_ms_p95"] > 0
    )
    if args.expect_no_cry:
        summary["pass"] = (
            summary["pass"]
            and summary["raw_hit_samples"] <= args.allowed_raw_hits
            and summary["confirmed_cry_samples"] <= args.allowed_confirmed_samples
        )

    output_dir = args.output / f'audio_{datetime.now().strftime("%Y%m%d_%H%M%S")}'
    output_dir.mkdir(parents=True, exist_ok=True)
    with (output_dir / "audio_frames.csv").open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(samples[0].keys()))
        writer.writeheader()
        writer.writerows(samples)
    (output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

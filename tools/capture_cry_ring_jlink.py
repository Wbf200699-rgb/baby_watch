#!/usr/bin/env python3
"""Capture coherent one-second PCM windows from the live cry-inference ring."""

from __future__ import annotations

import argparse
import csv
import json
import locale
import re
import struct
import subprocess
import tempfile
import time
import wave
from datetime import datetime
from pathlib import Path


DEFAULT_JLINK = Path(r"D:\Tools\SEGGER\JLink_V938a\JLink.exe")
RING_CONTROL_ADDRESS = 0x22007938
RING_SAMPLES = 16000
RING_DUMP_BYTES = 8 + RING_SAMPLES * 2 + 4
CRY_STATE_ADDRESS = 0x220136B4
CRY_STATE_BYTES = 8 * 4
JLINK_FAILURE = re.compile(
    r"Cannot connect to the probe/programmer|"
    r"Connecting to J-Link via USB\.\.\.FAILED|"
    r"Could not find emulator|"
    r"Script processing was aborted|"
    r"Error while reading memory",
    re.IGNORECASE,
)


def jlink_path(path: Path) -> str:
    return path.resolve().as_posix().replace('"', "")


def capture_once(
    jlink: Path,
    temp_dir: Path,
    index: int,
) -> tuple[bytes, bytes]:
    ring_path = temp_dir / f"ring_{index:04d}.bin"
    state_path = temp_dir / f"state_{index:04d}.bin"
    script_path = temp_dir / f"capture_{index:04d}.jlink"
    script_path.write_text(
        "\n".join(
            [
                "ExitOnError 1",
                "device R7KA8P1KF_CPU0",
                "si SWD",
                "speed 4000",
                "connect",
                "h",
                f'savebin "{jlink_path(ring_path)}", '
                f"0x{RING_CONTROL_ADDRESS:08X}, 0x{RING_DUMP_BYTES:X}",
                f'savebin "{jlink_path(state_path)}", '
                f"0x{CRY_STATE_ADDRESS:08X}, 0x{CRY_STATE_BYTES:X}",
                "g",
                "exit",
                "",
            ]
        ),
        encoding="ascii",
    )
    result = subprocess.run(
        [str(jlink), "-NoGui", "1", "-CommanderScript", str(script_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding=locale.getpreferredencoding(False) or "utf-8",
        errors="replace",
        check=False,
        timeout=30,
    )
    if result.returncode or JLINK_FAILURE.search(result.stdout):
        raise RuntimeError(
            f"J-Link snapshot {index} failed with {result.returncode}\n"
            f"{result.stdout[-5000:]}"
        )
    if "Script processing completed." not in result.stdout:
        raise RuntimeError(f"J-Link snapshot {index} did not complete")
    return ring_path.read_bytes(), state_path.read_bytes()


def decode_ring(payload: bytes) -> tuple[int, int, int, bytes]:
    if len(payload) != RING_DUMP_BYTES:
        raise RuntimeError(
            f"ring dump has {len(payload)} bytes, expected {RING_DUMP_BYTES}"
        )
    sequence, write = struct.unpack_from("<II", payload)
    count = struct.unpack_from("<I", payload, 8 + RING_SAMPLES * 2)[0]
    if sequence & 1:
        raise RuntimeError("ring was halted during a producer update")
    if write >= RING_SAMPLES or count > RING_SAMPLES:
        raise RuntimeError(f"invalid ring metadata write={write}, count={count}")
    raw = payload[8:8 + RING_SAMPLES * 2]
    if count < RING_SAMPLES:
        pcm = raw[:count * 2]
    else:
        split = write * 2
        pcm = raw[split:] + raw[:split]
    return sequence, write, count, pcm


def write_wav(path: Path, pcm: bytes) -> None:
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(16000)
        stream.writeframes(pcm)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--interval-seconds", type=float, default=1.0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jlink", type=Path, default=DEFAULT_JLINK)
    args = parser.parse_args()
    if args.samples <= 0 or args.interval_seconds < 0:
        parser.error("--samples must be positive and --interval-seconds non-negative")
    if not args.jlink.is_file():
        raise FileNotFoundError(f"J-Link Commander not found: {args.jlink}")
    args.output.mkdir(parents=True, exist_ok=True)

    rows: list[dict[str, int | str]] = []
    retries = 0
    with tempfile.TemporaryDirectory(prefix="ra8_cry_ring_") as value:
        temp_dir = Path(value)
        while len(rows) < args.samples:
            if rows and args.interval_seconds:
                time.sleep(args.interval_seconds)
            ring_payload, state_payload = capture_once(
                args.jlink, temp_dir, len(rows) + retries
            )
            try:
                sequence, write, count, pcm = decode_ring(ring_payload)
            except RuntimeError as error:
                if "producer update" not in str(error) or retries >= 10:
                    raise
                retries += 1
                continue
            state = struct.unpack("<8I", state_payload)
            filename = f"ring_{len(rows):04d}.wav"
            write_wav(args.output / filename, pcm)
            rows.append(
                {
                    "captured_at": datetime.now().astimezone().isoformat(),
                    "file": filename,
                    "ring_sequence": sequence,
                    "ring_write": write,
                    "ring_count": count,
                    "model_ready": state[0],
                    "inference_count": state[1],
                    "cry_probability_x1000": state[2],
                    "vote_hits": state[3],
                    "cry_confirmed": state[4],
                    "release_hold_windows": state[5],
                    "inference_ms": state[6],
                    "snapshot_retries": state[7],
                }
            )
            print(
                f"{len(rows)}/{args.samples}: score={state[2]} "
                f"votes={state[3]} confirmed={state[4]} file={filename}"
            )

    with (args.output / "captures.csv").open(
        "w", newline="", encoding="utf-8-sig"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    summary = {
        "generated_at": datetime.now().astimezone().isoformat(),
        "samples": len(rows),
        "interval_seconds": args.interval_seconds,
        "producer_retries": retries,
        "model_ready_samples": sum(int(row["model_ready"]) != 0 for row in rows),
        "maximum_score_x1000": max(
            int(row["cry_probability_x1000"]) for row in rows
        ),
        "confirmed_samples": sum(int(row["cry_confirmed"]) != 0 for row in rows),
        "output": str(args.output.resolve()),
    }
    (args.output / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

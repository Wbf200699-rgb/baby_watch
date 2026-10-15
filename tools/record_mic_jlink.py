#!/usr/bin/env python3
"""Record the live RA8P1 microphone to SD by driving its IPC mailbox via J-Link."""

from __future__ import annotations

import argparse
import json
import locale
import re
import struct
import subprocess
import tempfile
import time
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_JLINK = Path(r"D:\Tools\SEGGER\JLink_V938a\JLink.exe")
MIC_ADDRESS = 0x221D3800
MIC_MAGIC = 0x4D494331
MIC_REGION_BYTES = 0x700
RECORD_BASENAME_OFFSET = 0xA8
COMMAND_OFFSET = 0x08
COMMAND_SEQUENCE_OFFSET = 0x0C
COMMAND_RECORD_START = 3
COMMAND_RECORD_STOP = 4
WRITER_OPEN = 6
WRITER_WRITE = 7
WRITER_WRITE_ERROR = 8
WRITER_CLOSED = 9
WRITER_WAIT_RECORD = 3
JLINK_FAILURE = re.compile(
    r"Cannot connect to the probe/programmer|"
    r"Connecting to J-Link via USB\.\.\.FAILED|"
    r"Could not find emulator|"
    r"Script processing was aborted|"
    r"Error while reading memory",
    re.IGNORECASE,
)


def next_sequence(value: int) -> int:
    result = (value + 1) & 0xFFFFFFFF
    return result if result else 1


def jlink_path(path: Path) -> str:
    return path.resolve().as_posix().replace('"', "")


def run_jlink(
    jlink: Path,
    temp_dir: Path,
    name: str,
    commands: list[str],
) -> str:
    script = temp_dir / f"{name}.jlink"
    script.write_text(
        "\n".join(
            [
                "ExitOnError 1",
                "device R7KA8P1KF_CPU0",
                "si SWD",
                "speed 4000",
                "connect",
                "g",
                *commands,
                "exit",
                "",
            ]
        ),
        encoding="ascii",
    )
    result = subprocess.run(
        [str(jlink), "-NoGui", "1", "-CommanderScript", str(script)],
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
            f"J-Link command {name} failed with {result.returncode}\n"
            f"{result.stdout[-5000:]}"
        )
    if "Script processing completed." not in result.stdout:
        raise RuntimeError(f"J-Link command {name} did not complete")
    return result.stdout


def parse_metadata(payload: bytes) -> dict[str, Any]:
    if len(payload) != MIC_REGION_BYTES:
        raise RuntimeError(
            f"microphone mailbox has {len(payload)} bytes, expected {MIC_REGION_BYTES}"
        )
    words = struct.unpack_from("<42I", payload)
    basename = payload[RECORD_BASENAME_OFFSET:RECORD_BASENAME_OFFSET + 32]
    filename = payload[
        RECORD_BASENAME_OFFSET + 32:RECORD_BASENAME_OFFSET + 32 + 48
    ]
    result: dict[str, Any] = {
        "magic": words[0],
        "sequence": words[1],
        "command": words[2],
        "command_sequence": words[3],
        "command_ack_sequence": words[4],
        "status": words[5],
        "sample_rate": words[6],
        "callback_count": words[9],
        "queue_count": words[10],
        "read_count": words[11],
        "error_count": words[12],
        "timeout_count": words[13],
        "last_event": words[15],
        "last_fsp_err": words[16],
        "peak": struct.unpack_from("<i", payload, 17 * 4)[0],
        "mean_abs": struct.unpack_from("<i", payload, 18 * 4)[0],
        "record_active": words[21],
        "record_sequence": words[22],
        "record_bytes": words[23],
        "record_seconds": words[24],
        "chunk_sequence": words[25],
        "writer_poll_count": words[27],
        "writer_status": words[28],
        "writer_error": words[29],
        "writer_open_count": words[30],
        "writer_write_count": words[31],
        "writer_bytes": words[32],
        "writer_record_sequence": words[33],
        "writer_chunk_sequence": words[34],
        "record_slot_select": words[35],
        "record_sample_shift": words[36],
        "record_slew_limit": words[37],
        "record_gain_percent": words[38],
        "writer_active_slot": words[39],
        "writer_even_peak": words[40],
        "writer_odd_peak": words[41],
        "writer_gap_count": struct.unpack_from("<I", payload, 0x6F8)[0],
        "writer_declick_count": struct.unpack_from("<I", payload, 0x6FC)[0],
        "record_basename": basename.split(b"\0", 1)[0].decode("ascii", "replace"),
        "writer_filename": filename.split(b"\0", 1)[0].decode("ascii", "replace"),
    }
    if result["magic"] != MIC_MAGIC:
        raise RuntimeError(
            f"microphone IPC magic is 0x{result['magic']:08X}, expected 0x{MIC_MAGIC:08X}"
        )
    return result


def read_metadata(
    jlink: Path,
    temp_dir: Path,
    name: str,
    settle_ms: int = 100,
) -> dict[str, Any]:
    output = temp_dir / f"{name}.bin"
    run_jlink(
        jlink,
        temp_dir,
        name,
        [
            f"sleep {settle_ms}",
            f'savebin "{jlink_path(output)}", 0x{MIC_ADDRESS:08X}, 0x{MIC_REGION_BYTES:X}',
        ],
    )
    return parse_metadata(output.read_bytes())


def write_command(
    *,
    jlink: Path,
    temp_dir: Path,
    name: str,
    command: int,
    sequence: int,
    basename: str | None = None,
) -> dict[str, Any]:
    commands: list[str] = []
    if basename is not None:
        encoded = basename.encode("ascii")
        padded = encoded + bytes(32 - len(encoded))
        for index, value in enumerate(struct.unpack("<8I", padded)):
            commands.append(
                f"w4 0x{MIC_ADDRESS + RECORD_BASENAME_OFFSET + index * 4:08X}, "
                f"0x{value:08X}"
            )
    commands.extend(
        [
            f"w4 0x{MIC_ADDRESS + COMMAND_OFFSET:08X}, 0x{command:08X}",
            # Commit the request last so CPU0 never sees a partial command.
            f"w4 0x{MIC_ADDRESS + COMMAND_SEQUENCE_OFFSET:08X}, 0x{sequence:08X}",
            "sleep 500",
        ]
    )
    output = temp_dir / f"{name}.bin"
    commands.append(
        f'savebin "{jlink_path(output)}", 0x{MIC_ADDRESS:08X}, 0x{MIC_REGION_BYTES:X}'
    )
    run_jlink(jlink, temp_dir, name, commands)
    return parse_metadata(output.read_bytes())


def wait_for(
    *,
    jlink: Path,
    temp_dir: Path,
    prefix: str,
    predicate: Any,
    timeout: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    attempt = 0
    last: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        last = read_metadata(jlink, temp_dir, f"{prefix}_{attempt}")
        if predicate(last):
            return last
        if last["writer_status"] == WRITER_WRITE_ERROR:
            raise RuntimeError(
                f"SD writer failed with error {last['writer_error']}"
            )
        attempt += 1
        time.sleep(0.2)
    raise TimeoutError(f"timed out waiting for {prefix}: {last}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--basename", default="QTEST")
    parser.add_argument("--jlink", type=Path, default=DEFAULT_JLINK)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.duration <= 0:
        parser.error("--duration must be positive")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,8}", args.basename):
        parser.error("--basename must contain 1-8 ASCII letters, digits, '_' or '-'")
    if not args.jlink.is_file():
        raise FileNotFoundError(f"J-Link Commander not found: {args.jlink}")

    started_at = datetime.now().astimezone()
    start_state: dict[str, Any] | None = None
    end_state: dict[str, Any] | None = None
    stop_required = False
    with tempfile.TemporaryDirectory(prefix="ra8_mic_record_") as value:
        temp_dir = Path(value)
        initial = read_metadata(args.jlink, temp_dir, "initial")
        if initial["record_active"]:
            raise RuntimeError("microphone recording is already active")
        start_sequence = next_sequence(
            max(initial["command_sequence"], initial["command_ack_sequence"])
        )
        try:
            write_command(
                jlink=args.jlink,
                temp_dir=temp_dir,
                name="start",
                command=COMMAND_RECORD_START,
                sequence=start_sequence,
                basename=args.basename,
            )
            stop_required = True
            start_state = wait_for(
                jlink=args.jlink,
                temp_dir=temp_dir,
                prefix="wait_start",
                predicate=lambda state: (
                    state["command_ack_sequence"] == start_sequence
                    and state["record_active"] == 1
                    and state["writer_status"] in {WRITER_OPEN, WRITER_WRITE}
                    and bool(state["writer_filename"])
                ),
                timeout=10.0,
            )
            time.sleep(args.duration)
        finally:
            if stop_required:
                latest = read_metadata(args.jlink, temp_dir, "pre_stop")
                stop_sequence = next_sequence(
                    max(latest["command_sequence"], latest["command_ack_sequence"])
                )
                write_command(
                    jlink=args.jlink,
                    temp_dir=temp_dir,
                    name="stop",
                    command=COMMAND_RECORD_STOP,
                    sequence=stop_sequence,
                )
                end_state = wait_for(
                    jlink=args.jlink,
                    temp_dir=temp_dir,
                    prefix="wait_stop",
                    predicate=lambda state: (
                        state["command_ack_sequence"] == stop_sequence
                        and state["record_active"] == 0
                        and state["writer_status"]
                        in {WRITER_CLOSED, WRITER_WAIT_RECORD}
                    ),
                    timeout=10.0,
                )

    if start_state is None or end_state is None:
        raise RuntimeError("recording did not produce complete start/stop evidence")
    writer_bytes = int(end_state["writer_bytes"])
    expected_minimum = int(args.duration * 16000 * 2 * 0.75)
    summary = {
        "generated_at": datetime.now().astimezone().isoformat(),
        "started_at": started_at.isoformat(),
        "requested_duration_seconds": args.duration,
        "filename": end_state["writer_filename"] or start_state["writer_filename"],
        "writer_bytes": writer_bytes,
        "writer_error": end_state["writer_error"],
        "writer_gap_count": end_state.get("writer_gap_count", 0),
        "writer_declick_count": end_state.get("writer_declick_count", 0),
        "read_count_delta": end_state["read_count"] - start_state["read_count"],
        "writer_write_count_delta": (
            end_state["writer_write_count"] - start_state["writer_write_count"]
        ),
        "start_state": start_state,
        "end_state": end_state,
        "pass": (
            end_state["writer_error"] == 0
            and writer_bytes >= expected_minimum
            and end_state["read_count"] > start_state["read_count"]
            and bool(end_state["writer_filename"] or start_state["writer_filename"])
        ),
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(summary, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

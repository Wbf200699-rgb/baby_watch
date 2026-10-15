#!/usr/bin/env python3
"""Export a closed file from the mounted RA8P1 SD card through J-Link.

The CPU1 firmware services a read-only mailbox at 0x221D3F80.  This tool asks
CPU1 to read one 32 KiB chunk into regular SRAM, uses J-Link SaveBin to copy the
stable chunk to the PC, and verifies sequence/offset/length/FNV-1a before
appending it to the destination file.  It never writes to the SD card.
"""

from __future__ import annotations

import argparse
import hashlib
import locale
import os
import re
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


CONTROL_ADDRESS = 0x221D3F80
CONTROL_BYTES = 0x80
CONTROL_MAGIC = 0x53444558  # SDEX
CONTROL_VERSION = 1
PATH_BYTES = 64

OPERATION_OPEN = 1
OPERATION_READ = 2
OPERATION_CLOSE = 3

STATUS_OK = 2

FIELD_REQUEST_SEQUENCE = 0x08
FIELD_OPERATION = 0x10
FIELD_REQUEST_OFFSET = 0x1C
FIELD_REQUEST_LENGTH = 0x20
FIELD_PATH = 0x40

DEFAULT_JLINK = Path(r"D:\Tools\SEGGER\JLink_V938a\JLink.exe")
JLINK_FAILURE = re.compile(
    r"Cannot connect to the probe/programmer|"
    r"Connecting to J-Link via USB\.\.\.FAILED|"
    r"Could not find emulator|"
    r"Script processing was aborted|"
    r"Error while reading memory",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class ExportControl:
    magic: int
    version: int
    request_sequence: int
    response_sequence: int
    operation: int
    status: int
    file_size: int
    request_offset: int
    request_length: int
    response_offset: int
    bytes_ready: int
    buffer_address: int
    buffer_capacity: int
    chunk_checksum: int
    last_error: int
    reserved: int
    path: bytes

    @classmethod
    def read(cls, path: Path) -> "ExportControl":
        payload = path.read_bytes()
        if len(payload) != CONTROL_BYTES:
            raise RuntimeError(
                f"debug mailbox read returned {len(payload)} bytes, expected {CONTROL_BYTES}"
            )
        values = struct.unpack("<16I64s", payload)
        return cls(*values)


def u32(value: int) -> int:
    return value & 0xFFFFFFFF


def next_sequence(value: int) -> int:
    result = u32(value + 1)
    return 1 if result == 0 else result


def fnv1a32(payload: bytes) -> int:
    checksum = 2166136261
    for value in payload:
        checksum ^= value
        checksum = u32(checksum * 16777619)
    return checksum


def jlink_path(path: Path) -> str:
    return path.resolve().as_posix().replace('"', "")


def base_commands(speed_khz: int) -> list[str]:
    return [
        "ExitOnError 1",
        "device R7KA8P1KF_CPU0",
        "si SWD",
        f"speed {speed_khz}",
        "connect",
        "g",
    ]


def run_jlink(
    jlink_exe: Path,
    temp_dir: Path,
    name: str,
    commands: list[str],
) -> str:
    script_path = temp_dir / f"{name}.jlink"
    script_path.write_text("\n".join([*commands, "exit", ""]) , encoding="ascii")
    encoding = locale.getpreferredencoding(False) or "utf-8"
    result = subprocess.run(
        [str(jlink_exe), "-NoGui", "1", "-CommanderScript", str(script_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding=encoding,
        errors="replace",
        check=False,
    )
    output = result.stdout
    if result.returncode != 0:
        raise RuntimeError(f"J-Link exited with {result.returncode}\n{output}")
    if JLINK_FAILURE.search(output):
        raise RuntimeError(f"J-Link reported a target/memory error\n{output}")
    if "Script processing completed." not in output:
        raise RuntimeError(f"J-Link did not complete its command script\n{output}")
    return output


def read_control(
    jlink_exe: Path,
    temp_dir: Path,
    speed_khz: int,
    name: str,
    settle_ms: int = 20,
) -> ExportControl:
    meta_path = temp_dir / f"{name}.bin"
    commands = base_commands(speed_khz)
    commands.extend(
        [
            f"sleep {settle_ms}",
            f'savebin "{jlink_path(meta_path)}", 0x{CONTROL_ADDRESS:08X}, 0x{CONTROL_BYTES:X}',
        ]
    )
    run_jlink(jlink_exe, temp_dir, name, commands)
    return ExportControl.read(meta_path)


def write_request_commands(
    *,
    operation: int,
    sequence: int,
    offset: int = 0,
    length: int = 0,
) -> list[str]:
    return [
        f"w4 0x{CONTROL_ADDRESS + FIELD_REQUEST_OFFSET:08X}, 0x{offset:08X}",
        f"w4 0x{CONTROL_ADDRESS + FIELD_REQUEST_LENGTH:08X}, 0x{length:08X}",
        f"w4 0x{CONTROL_ADDRESS + FIELD_OPERATION:08X}, 0x{operation:08X}",
        # Sequence is the commit word and must be written last.
        f"w4 0x{CONTROL_ADDRESS + FIELD_REQUEST_SEQUENCE:08X}, 0x{sequence:08X}",
    ]


def validate_service(control: ExportControl) -> None:
    if control.magic != CONTROL_MAGIC:
        raise RuntimeError(
            f"SD debug mailbox magic is 0x{control.magic:08X}, expected 0x{CONTROL_MAGIC:08X}"
        )
    if control.version != CONTROL_VERSION:
        raise RuntimeError(
            f"SD debug mailbox version is {control.version}, expected {CONTROL_VERSION}"
        )
    if control.buffer_capacity == 0 or control.buffer_address == 0:
        raise RuntimeError("SD debug export buffer is not initialized")
    buffer_end = control.buffer_address + control.buffer_capacity
    if not (0x22000000 <= control.buffer_address < buffer_end <= 0x221D3800):
        raise RuntimeError(
            "SD debug export buffer is outside the safe CPU1 SRAM range: "
            f"0x{control.buffer_address:08X}..0x{buffer_end - 1:08X}"
        )


def validate_response(
    control: ExportControl,
    *,
    sequence: int,
    operation: int,
) -> None:
    validate_service(control)
    if control.response_sequence != sequence:
        raise RuntimeError(
            "firmware did not acknowledge request "
            f"{sequence} (response={control.response_sequence})"
        )
    if control.operation != operation:
        raise RuntimeError(
            f"response operation is {control.operation}, expected {operation}"
        )
    if control.status != STATUS_OK:
        raise RuntimeError(
            f"firmware export status=0x{control.status:08X}, "
            f"last_error=0x{control.last_error:08X}"
        )


def open_source(
    *,
    source: str,
    initial: ExportControl,
    jlink_exe: Path,
    temp_dir: Path,
    speed_khz: int,
    settle_ms: int,
) -> ExportControl:
    encoded = source.encode("ascii")
    if not source.startswith("/sd0/") or source.endswith("/"):
        raise ValueError("source must be an absolute file path below /sd0/")
    if len(encoded) >= PATH_BYTES:
        raise ValueError(f"source path must be shorter than {PATH_BYTES} ASCII bytes")

    padded = encoded + bytes(PATH_BYTES - len(encoded))
    commands = base_commands(speed_khz)
    for index, word in enumerate(struct.unpack("<16I", padded)):
        commands.append(
            f"w4 0x{CONTROL_ADDRESS + FIELD_PATH + index * 4:08X}, 0x{word:08X}"
        )
    sequence = next_sequence(initial.response_sequence)
    commands.extend(write_request_commands(operation=OPERATION_OPEN, sequence=sequence))
    commands.extend(
        [
            f"sleep {settle_ms}",
            f'savebin "{jlink_path(temp_dir / "open_meta.bin")}", '
            f"0x{CONTROL_ADDRESS:08X}, 0x{CONTROL_BYTES:X}",
        ]
    )
    run_jlink(jlink_exe, temp_dir, "open", commands)
    response = ExportControl.read(temp_dir / "open_meta.bin")
    validate_response(response, sequence=sequence, operation=OPERATION_OPEN)
    return response


def read_chunk(
    *,
    sequence: int,
    offset: int,
    length: int,
    index: int,
    buffer_address: int,
    jlink_exe: Path,
    temp_dir: Path,
    speed_khz: int,
    settle_ms: int,
) -> tuple[bytes, ExportControl]:
    chunk_path = temp_dir / f"chunk_{index:05d}.bin"
    meta_path = temp_dir / f"chunk_{index:05d}_meta.bin"
    commands = base_commands(speed_khz)
    commands.extend(
        write_request_commands(
            operation=OPERATION_READ,
            sequence=sequence,
            offset=offset,
            length=length,
        )
    )
    commands.extend(
        [
            f"sleep {settle_ms}",
            f'savebin "{jlink_path(chunk_path)}", 0x{buffer_address:08X}, 0x{length:X}',
            f'savebin "{jlink_path(meta_path)}", 0x{CONTROL_ADDRESS:08X}, 0x{CONTROL_BYTES:X}',
        ]
    )
    run_jlink(jlink_exe, temp_dir, f"read_{index:05d}", commands)

    response = ExportControl.read(meta_path)
    validate_response(response, sequence=sequence, operation=OPERATION_READ)
    payload = chunk_path.read_bytes()
    if response.response_offset != offset:
        raise RuntimeError(
            f"chunk {index} response offset={response.response_offset}, expected {offset}"
        )
    if response.bytes_ready != length or len(payload) != length:
        raise RuntimeError(
            f"chunk {index} length mismatch: mailbox={response.bytes_ready}, "
            f"SaveBin={len(payload)}, expected={length}"
        )
    checksum = fnv1a32(payload)
    if response.chunk_checksum != checksum:
        raise RuntimeError(
            f"chunk {index} checksum mismatch: mailbox=0x{response.chunk_checksum:08X}, "
            f"host=0x{checksum:08X}"
        )
    return payload, response


def close_source(
    *,
    response_sequence: int,
    jlink_exe: Path,
    temp_dir: Path,
    speed_khz: int,
    settle_ms: int,
) -> ExportControl:
    sequence = next_sequence(response_sequence)
    meta_path = temp_dir / "close_meta.bin"
    commands = base_commands(speed_khz)
    commands.extend(write_request_commands(operation=OPERATION_CLOSE, sequence=sequence))
    commands.extend(
        [
            f"sleep {settle_ms}",
            f'savebin "{jlink_path(meta_path)}", 0x{CONTROL_ADDRESS:08X}, 0x{CONTROL_BYTES:X}',
        ]
    )
    run_jlink(jlink_exe, temp_dir, "close", commands)
    response = ExportControl.read(meta_path)
    validate_response(response, sequence=sequence, operation=OPERATION_CLOSE)
    return response


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, help="Board path, for example /sd0/MIC/FIX731B.WAV")
    parser.add_argument("--output", type=Path, required=True, help="PC destination path")
    parser.add_argument("--jlink", type=Path, default=DEFAULT_JLINK)
    parser.add_argument("--speed", type=int, default=4000, help="SWD speed in kHz")
    parser.add_argument(
        "--settle-ms",
        type=int,
        default=250,
        help="Time allowed for CPU1 to service each request",
    )
    parser.add_argument("--force", action="store_true", help="Replace an existing output file")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.jlink.is_file():
        raise FileNotFoundError(f"J-Link Commander not found: {args.jlink}")
    if args.output.exists() and not args.force:
        raise FileExistsError(f"output already exists (use --force): {args.output}")
    if args.speed <= 0:
        raise ValueError("--speed must be positive")
    if args.settle_ms < 20:
        raise ValueError("--settle-ms must be at least 20")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    partial = args.output.with_name(f"{args.output.name}.{os.getpid()}.partial")
    if partial.exists():
        raise FileExistsError(f"temporary output already exists: {partial}")

    opened = False
    last_response_sequence = 0
    temp_context: tempfile.TemporaryDirectory[str] | None = None
    temp_dir: Path | None = None
    try:
        temp_context = tempfile.TemporaryDirectory(prefix="ra8_sd_export_")
        temp_dir = Path(temp_context.name)
        initial = read_control(
            args.jlink,
            temp_dir,
            args.speed,
            "initial_meta",
        )
        validate_service(initial)
        opened_control = open_source(
            source=args.source,
            initial=initial,
            jlink_exe=args.jlink,
            temp_dir=temp_dir,
            speed_khz=args.speed,
            settle_ms=args.settle_ms,
        )
        opened = True
        last_response_sequence = opened_control.response_sequence
        file_size = opened_control.file_size
        if file_size == 0:
            raise RuntimeError("source file is empty")

        chunk_bytes = opened_control.buffer_capacity
        chunks = (file_size + chunk_bytes - 1) // chunk_bytes
        digest = hashlib.sha256()
        with partial.open("xb") as output:
            offset = 0
            for index in range(chunks):
                length = min(chunk_bytes, file_size - offset)
                sequence = next_sequence(last_response_sequence)
                payload, response = read_chunk(
                    sequence=sequence,
                    offset=offset,
                    length=length,
                    index=index,
                    buffer_address=opened_control.buffer_address,
                    jlink_exe=args.jlink,
                    temp_dir=temp_dir,
                    speed_khz=args.speed,
                    settle_ms=args.settle_ms,
                )
                output.write(payload)
                digest.update(payload)
                offset += length
                last_response_sequence = response.response_sequence
                print(
                    f"\rExported {offset}/{file_size} bytes "
                    f"({index + 1}/{chunks} chunks)",
                    end="",
                    flush=True,
                )
            output.flush()
            os.fsync(output.fileno())
        print()

        close_source(
            response_sequence=last_response_sequence,
            jlink_exe=args.jlink,
            temp_dir=temp_dir,
            speed_khz=args.speed,
            settle_ms=args.settle_ms,
        )
        opened = False

        if partial.stat().st_size != file_size:
            raise RuntimeError(
                f"assembled file size is {partial.stat().st_size}, expected {file_size}"
            )
        os.replace(partial, args.output)
        print(f"Source: {args.source}")
        print(f"Output: {args.output.resolve()}")
        print(f"Bytes: {file_size}")
        print(f"SHA256: {digest.hexdigest().upper()}")
    except BaseException:
        if partial.exists():
            partial.unlink()
        raise
    finally:
        if opened and temp_dir is not None:
            try:
                close_source(
                    response_sequence=last_response_sequence,
                    jlink_exe=args.jlink,
                    temp_dir=temp_dir,
                    speed_khz=args.speed,
                    settle_ms=args.settle_ms,
                )
            except Exception as exc:
                print(
                    "Warning: could not close the read-only debug handle; it "
                    f"will be released on the next export or firmware restart: {exc}",
                    file=sys.stderr,
                )
        if temp_context is not None:
            temp_context.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

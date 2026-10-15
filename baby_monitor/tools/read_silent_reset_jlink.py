#!/usr/bin/env python3
"""Read and validate the RA8 cold/warm-start audio decision through J-Link."""

from __future__ import annotations

import argparse
import json
import locale
import re
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_JLINK = Path(r"D:\Tools\SEGGER\JLink_V938a\JLink.exe")
RSTSR1_SOFTWARE_RESET = 1 << 2

AUDIO_BOOT_ADDRESS = 0x221D3184
AUDIO_BOOT_WORDS = 7
AUDIO_STATE_VALID = 1 << 0
AUDIO_COLD_START = 1 << 1
AUDIO_WARM_START = 1 << 2
AUDIO_SUPPRESSED = 1 << 3
AUDIO_SOFTWARE_RESET = 1 << 4

AUDIO_FIELDS = (
    "boot_flags",
    "boot_capture_count",
    "silent_boot_count",
    "boot_cold_warm_flag",
    "boot_rstsr0",
    "boot_rstsr1",
    "boot_rstsr3",
)
JLINK_FAILURE = re.compile(
    r"Cannot connect to the probe/programmer|"
    r"Connecting to J-Link via USB\.\.\.FAILED|"
    r"Could not find emulator|"
    r"Script processing was aborted|"
    r"Error while reading memory",
    re.IGNORECASE,
)


def run_jlink(jlink: Path, settle_ms: int) -> str:
    commands = [
        "device R7KA8P1KF_CPU0",
        "si SWD",
        "speed 4000",
        "connect",
        f"sleep {settle_ms}",
        f"mem32 0x{AUDIO_BOOT_ADDRESS:08X},{AUDIO_BOOT_WORDS}",
        "exit",
    ]
    completed = subprocess.run(
        [str(jlink), "-NoGui", "1"],
        input="\n".join(commands) + "\n",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding=locale.getpreferredencoding(False) or "utf-8",
        errors="replace",
        timeout=max(30.0, settle_ms / 1000.0 + 30.0),
        check=False,
    )
    if completed.returncode or JLINK_FAILURE.search(completed.stdout):
        raise RuntimeError(
            f"J-Link failed with exit code {completed.returncode}\n"
            f"{completed.stdout[-5000:]}"
        )
    if "Cortex-M85 identified." not in completed.stdout:
        raise RuntimeError(
            "J-Link did not identify the RA8P1 CPU0 target\n"
            f"{completed.stdout[-5000:]}"
        )
    return completed.stdout


def parse_words(output: str, address: int, count: int) -> list[int]:
    words: list[int] = []
    expected_address = address
    memory_line = re.compile(
        r"^\s*(?:J-Link>)?([0-9A-Fa-f]{8})\s*=\s*"
        r"([0-9A-Fa-f \t]+)\r?$",
        re.MULTILINE,
    )
    for match in memory_line.finditer(output):
        line_address = int(match.group(1), 16)
        if line_address != expected_address:
            continue
        line_words = [int(value, 16) for value in match.group(2).split()]
        words.extend(line_words)
        expected_address += 4 * len(line_words)
        if len(words) >= count:
            return words[:count]
    raise RuntimeError(
        f"could not decode {count} words at 0x{address:08X}\n{output[-5000:]}"
    )


def validate(
    audio: dict[str, int],
    expect_suppressed: bool | None,
    expect_software_reset: bool,
) -> list[str]:
    errors: list[str] = []
    if not audio["boot_flags"] & AUDIO_STATE_VALID:
        errors.append("audio thread did not publish a valid boot state")
    if audio["boot_capture_count"] != 1:
        errors.append(
            "boot capture count is not one "
            f"({audio['boot_capture_count']} != 1)"
        )

    suppressed = bool(audio["boot_flags"] & AUDIO_SUPPRESSED)
    software_reset = bool(audio["boot_rstsr1"] & RSTSR1_SOFTWARE_RESET)
    audio_software_reset = bool(audio["boot_flags"] & AUDIO_SOFTWARE_RESET)
    if software_reset != audio_software_reset:
        errors.append("software-reset flag does not match boot_rstsr1")
    if expect_suppressed is not None and suppressed != expect_suppressed:
        errors.append(
            f"audio suppression is {suppressed}, expected {expect_suppressed}"
        )
    if expect_software_reset and (not software_reset or not audio_software_reset):
        errors.append("software-reset status was not captured")

    if audio["boot_cold_warm_flag"] == 0:
        if audio["boot_flags"] & AUDIO_WARM_START:
            errors.append("cold start is also marked warm")
        if not audio["boot_flags"] & AUDIO_COLD_START:
            errors.append("cold start lacks the cold-start flag")
        if suppressed:
            errors.append("cold start unexpectedly suppresses BOOT.WAV")
        if audio["silent_boot_count"] != 0:
            errors.append("cold start has a nonzero silent-boot count")
    elif audio["boot_cold_warm_flag"] == 1:
        if audio["boot_flags"] & AUDIO_COLD_START:
            errors.append("warm start is also marked cold")
        if not audio["boot_flags"] & AUDIO_WARM_START:
            errors.append("warm start lacks the warm-start flag")
        if not suppressed:
            errors.append("warm start did not suppress BOOT.WAV")
        if audio["silent_boot_count"] != 1:
            errors.append("warm start silent-boot count is not one")
    else:
        errors.append(
            f"invalid CWSF value {audio['boot_cold_warm_flag']}"
        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jlink", type=Path, default=DEFAULT_JLINK)
    parser.add_argument("--settle-ms", type=int, default=2000)
    suppression = parser.add_mutually_exclusive_group()
    suppression.add_argument(
        "--expect-suppressed",
        dest="expect_suppressed",
        action="store_true",
    )
    suppression.add_argument(
        "--expect-audible",
        dest="expect_suppressed",
        action="store_false",
    )
    parser.set_defaults(expect_suppressed=None)
    parser.add_argument("--expect-software-reset", action="store_true")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("test_results/silent_reset_jlink.json"),
    )
    args = parser.parse_args()
    if args.settle_ms < 0:
        parser.error("--settle-ms must be non-negative")
    if not args.jlink.is_file():
        raise FileNotFoundError(f"J-Link Commander not found: {args.jlink}")

    raw_output = run_jlink(args.jlink, args.settle_ms)
    audio = dict(zip(AUDIO_FIELDS, parse_words(
        raw_output, AUDIO_BOOT_ADDRESS, AUDIO_BOOT_WORDS
    )))
    errors = validate(
        audio,
        args.expect_suppressed,
        args.expect_software_reset,
    )
    result: dict[str, Any] = {
        "generated_at": datetime.now().astimezone().isoformat(),
        "passed": not errors,
        "audio": audio,
        "interpretation": {
            "warm_start": audio["boot_cold_warm_flag"] == 1,
            "boot_audio_suppressed": bool(
                audio["boot_flags"] & AUDIO_SUPPRESSED
            ),
            "software_reset": bool(
                audio["boot_rstsr1"] & RSTSR1_SOFTWARE_RESET
            ),
        },
        "errors": errors,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())

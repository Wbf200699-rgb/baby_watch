#!/usr/bin/env python3
"""Validate live SCD41 and BME688 shared-memory telemetry through J-Link."""

from __future__ import annotations

import argparse
import csv
import json
import locale
import re
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_JLINK = Path(r"D:\Tools\SEGGER\JLink_V938a\JLink.exe")
MAILBOX_ADDRESS = 0x221D0180
MAILBOX_MAGIC = 0x53434434
MAILBOX_WORDS = 32
BME_SAMPLE_OK = 3
BME_NEW_DATA = 1 << 0
BME_GAS_VALID = 1 << 1
BME_HEATER_STABLE = 1 << 2

FIELDS = (
    "magic",
    "publish_lock",
    "sequence",
    "scd_status",
    "scd_co2_ppm",
    "scd_temperature_milli_c",
    "scd_humidity_milli_percent",
    "scd_raw_co2",
    "scd_raw_temperature",
    "scd_raw_humidity",
    "scd_data_ready",
    "scd_serial_0",
    "scd_serial_1",
    "scd_serial_2",
    "scd_last_good_tick",
    "scd_read_count",
    "scd_crc_error_count",
    "scd_i2c_error_count",
    "scd_timeout_count",
    "scd_recovery_count",
    "scd_consecutive_failures",
    "scd_last_fsp_err",
    "scd_last_event",
    "bme_status",
    "bme_address",
    "bme_temperature_milli_c",
    "bme_humidity_milli_percent",
    "bme_pressure_pa",
    "bme_gas_resistance_ohm",
    "bme_flags",
    "bme_last_good_tick",
    "bme_error_count",
)

JLINK_FAILURE = re.compile(
    r"Cannot connect to the probe/programmer|"
    r"Connecting to J-Link via USB\.\.\.FAILED|"
    r"Could not find emulator|"
    r"Script processing was aborted|"
    r"Error while reading memory",
    re.IGNORECASE,
)


def signed32(value: int) -> int:
    return value - 0x100000000 if value & 0x80000000 else value


def run_jlink(jlink: Path, samples: int, interval_ms: int) -> str:
    commands = [
        "device R7KA8P1KF_CPU0",
        "si SWD",
        "speed 4000",
        "connect",
    ]
    for _ in range(samples):
        commands.append(f"sleep {interval_ms}")
        commands.append(f"mem32 0x{MAILBOX_ADDRESS:08X},{MAILBOX_WORDS}")
    commands.append("exit")
    completed = subprocess.run(
        [str(jlink), "-NoGui", "1"],
        input="\n".join(commands) + "\n",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding=locale.getpreferredencoding(False) or "utf-8",
        errors="replace",
        timeout=max(30.0, samples * interval_ms / 1000.0 + 30.0),
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


def parse_word_blocks(output: str) -> list[list[int]]:
    blocks: list[list[int]] = []
    current: list[int] | None = None
    memory_line = re.compile(
        r"^\s*(?:J-Link>)?([0-9A-Fa-f]{8})\s*=\s*"
        r"([0-9A-Fa-f \t]+)\r?$",
        re.MULTILINE,
    )
    for match in memory_line.finditer(output):
        address = int(match.group(1), 16)
        words = [int(value, 16) for value in match.group(2).split()]
        if address == MAILBOX_ADDRESS:
            if current is not None and len(current) >= MAILBOX_WORDS:
                blocks.append(current[:MAILBOX_WORDS])
            current = words
        elif current is not None and address == MAILBOX_ADDRESS + len(current) * 4:
            current.extend(words)
    if current is not None and len(current) >= MAILBOX_WORDS:
        blocks.append(current[:MAILBOX_WORDS])
    return blocks


def decode_samples(blocks: list[list[int]]) -> list[dict[str, int]]:
    rows: list[dict[str, int]] = []
    for words in blocks:
        row = dict(zip(FIELDS, words))
        row["scd_temperature_milli_c"] = signed32(
            row["scd_temperature_milli_c"]
        )
        row["bme_temperature_milli_c"] = signed32(
            row["bme_temperature_milli_c"]
        )
        rows.append(row)
    return rows


def value_range(values: list[int]) -> dict[str, int]:
    return {"minimum": min(values), "maximum": max(values)}


def build_summary(
    rows: list[dict[str, int]],
    interval_ms: int,
) -> tuple[dict[str, Any], list[str]]:
    coherent_rows = [
        row
        for row in rows
        if row["magic"] == MAILBOX_MAGIC and not row["publish_lock"] & 1
    ]
    valid_bme = [
        row
        for row in coherent_rows
        if row["bme_status"] == BME_SAMPLE_OK
        and row["bme_address"] in (0x76, 0x77)
        and -40000 <= row["bme_temperature_milli_c"] <= 85000
        and 0 <= row["bme_humidity_milli_percent"] <= 100000
        and 30000 <= row["bme_pressure_pa"] <= 110000
        and row["bme_gas_resistance_ohm"] > 0
        and row["bme_flags"] & BME_NEW_DATA
    ]
    errors: list[str] = []
    if len(rows) < 2:
        errors.append("fewer than two mailbox samples were decoded")
    if len(coherent_rows) < 2:
        errors.append("fewer than two coherent mailbox samples were observed")
    if (
        len(coherent_rows) >= 2
        and len({row["sequence"] for row in coherent_rows}) < 2
    ):
        errors.append("environment mailbox sequence did not advance")
    if not valid_bme:
        errors.append("no complete, plausible BME688 sample was observed")
    if valid_bme and len({row["bme_last_good_tick"] for row in valid_bme}) < 2:
        errors.append("BME688 last-good tick did not advance")
    if valid_bme and not any(
        row["bme_flags"] & BME_GAS_VALID for row in valid_bme
    ):
        errors.append("BME688 never reported gas-valid")
    if valid_bme and not any(
        row["bme_flags"] & BME_HEATER_STABLE for row in valid_bme
    ):
        errors.append("BME688 heater never reported stable")

    valid_scd = [
        row
        for row in coherent_rows
        if row["scd_status"] == 3
        and 250 <= row["scd_co2_ppm"] <= 10000
        and -40000 <= row["scd_temperature_milli_c"] <= 85000
        and 0 <= row["scd_humidity_milli_percent"] <= 100000
    ]
    if not valid_scd:
        errors.append("no complete, plausible SCD41 sample was observed")

    summary: dict[str, Any] = {
        "generated_at": datetime.now().astimezone().isoformat(),
        "passed": not errors,
        "samples": len(rows),
        "coherent_samples": len(coherent_rows),
        "discarded_in_progress_samples": sum(
            bool(row["publish_lock"] & 1) for row in rows
        ),
        "invalid_magic_samples": sum(
            row["magic"] != MAILBOX_MAGIC for row in rows
        ),
        "interval_ms": interval_ms,
        "duration_seconds": round(len(rows) * interval_ms / 1000.0, 3),
        "mailbox_sequence": (
            value_range([row["sequence"] for row in coherent_rows])
            if coherent_rows
            else None
        ),
        "scd41": {
            "valid_samples": len(valid_scd),
            "status_values": sorted(
                {row["scd_status"] for row in coherent_rows}
            ),
            "read_count": (
                value_range(
                    [row["scd_read_count"] for row in coherent_rows]
                )
                if coherent_rows
                else None
            ),
            "co2_ppm": (
                value_range([row["scd_co2_ppm"] for row in valid_scd])
                if valid_scd
                else None
            ),
            "crc_error_delta": (
                coherent_rows[-1]["scd_crc_error_count"]
                - coherent_rows[0]["scd_crc_error_count"]
                if coherent_rows
                else None
            ),
            "i2c_error_delta": (
                coherent_rows[-1]["scd_i2c_error_count"]
                - coherent_rows[0]["scd_i2c_error_count"]
                if coherent_rows
                else None
            ),
        },
        "bme688": {
            "valid_samples": len(valid_bme),
            "status_values": sorted(
                {row["bme_status"] for row in coherent_rows}
            ),
            "addresses": sorted({row["bme_address"] for row in valid_bme}),
            "temperature_milli_c": (
                value_range(
                    [row["bme_temperature_milli_c"] for row in valid_bme]
                )
                if valid_bme
                else None
            ),
            "humidity_milli_percent": (
                value_range(
                    [row["bme_humidity_milli_percent"] for row in valid_bme]
                )
                if valid_bme
                else None
            ),
            "pressure_pa": (
                value_range([row["bme_pressure_pa"] for row in valid_bme])
                if valid_bme
                else None
            ),
            "gas_resistance_ohm": (
                value_range(
                    [row["bme_gas_resistance_ohm"] for row in valid_bme]
                )
                if valid_bme
                else None
            ),
            "gas_valid_samples": sum(
                bool(row["bme_flags"] & BME_GAS_VALID) for row in valid_bme
            ),
            "heater_stable_samples": sum(
                bool(row["bme_flags"] & BME_HEATER_STABLE) for row in valid_bme
            ),
            "error_delta": (
                coherent_rows[-1]["bme_error_count"]
                - coherent_rows[0]["bme_error_count"]
                if coherent_rows
                else None
            ),
        },
        "errors": errors,
    }
    return summary, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=24)
    parser.add_argument("--interval-ms", type=int, default=1000)
    parser.add_argument("--jlink", type=Path, default=DEFAULT_JLINK)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("test_results/environment_jlink"),
    )
    args = parser.parse_args()
    if args.samples < 2 or args.interval_ms <= 0:
        parser.error("--samples must be at least 2 and --interval-ms positive")
    if not args.jlink.is_file():
        raise FileNotFoundError(f"J-Link Commander not found: {args.jlink}")

    output = run_jlink(args.jlink, args.samples, args.interval_ms)
    rows = decode_samples(parse_word_blocks(output))
    if len(rows) != args.samples:
        raise RuntimeError(
            f"expected {args.samples} samples, decoded {len(rows)}\n"
            f"{output[-8000:]}"
        )

    summary, errors = build_summary(rows, args.interval_ms)
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "samples.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    (args.output / "jlink.log").write_text(output, encoding="utf-8")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())

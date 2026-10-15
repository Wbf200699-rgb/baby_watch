#!/usr/bin/env python3
"""Read live cry-detector state through J-Link without halting the RA8P1."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_JLINK = Path(r"D:\Tools\SEGGER\JLink_V938a\JLink.exe")
DEFAULT_MAP = Path(r"Multicore_top_CPU0\Debug\Multicore_top_CPU0.map")
MIC_IPC_ADDRESS = 0x221D3800
MIC_IPC_MAGIC = 0x4D494331
SYMBOLS = (
    "g_cry_model_ready",
    "g_cry_inference_count",
    "g_cry_probability_x1000",
    "g_cry_consecutive_windows",
    "g_cry_confirmed",
    "g_cry_release_hold_windows",
    "g_cry_last_inference_ms",
    "g_cry_snapshot_retries",
    "g_cry_instant_probability_x1000",
)
MIC_FIELDS = {
    "mic_magic": 0,
    "mic_sequence": 1,
    "mic_status": 5,
    "mic_sample_rate": 6,
    "mic_callback_count": 9,
    "mic_queue_count": 10,
    "mic_read_count": 11,
    "mic_error_count": 12,
    "mic_peak": 17,
    "mic_mean_abs": 18,
    "mic_nonzero_count": 20,
}
MIC_WORD_COUNT = max(MIC_FIELDS.values()) + 1


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def symbol_addresses(map_path: Path) -> dict[str, int]:
    text = map_path.read_text(encoding="utf-8", errors="replace")
    addresses: dict[str, int] = {}
    for symbol in SYMBOLS:
        pattern = re.compile(
            rf"^\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+\d+\s+\d+"
            rf".*\.bss\.{re.escape(symbol)}\)",
            re.MULTILINE,
        )
        match = pattern.search(text)
        if not match:
            raise RuntimeError(f"{symbol} was not found in {map_path}")
        addresses[symbol] = int(match.group(1), 16)
    base = addresses[SYMBOLS[0]]
    expected = {symbol: base + index * 4 for index, symbol in enumerate(SYMBOLS)}
    if addresses != expected:
        raise RuntimeError(
            "cry state symbols are not one contiguous 8-word block: "
            + json.dumps({key: hex(value) for key, value in addresses.items()})
        )
    return addresses


def run_jlink(
    jlink: Path,
    base_address: int,
    samples: int,
    interval_ms: int,
) -> str:
    commands = [
        "device R7KA8P1KF_CPU0",
        "si SWD",
        "speed 4000",
        "connect",
    ]
    for _ in range(samples):
        commands.append(f"sleep {interval_ms}")
        commands.append(f"mem32 0x{base_address:08X},{len(SYMBOLS)}")
        commands.append(f"mem32 0x{MIC_IPC_ADDRESS:08X},{MIC_WORD_COUNT}")
    commands.append("exit")
    completed = subprocess.run(
        [str(jlink), "-NoGui", "1"],
        input="\n".join(commands) + "\n",
        text=True,
        capture_output=True,
        timeout=max(30.0, samples * interval_ms / 1000.0 + 30.0),
        check=False,
    )
    output = completed.stdout + completed.stderr
    if completed.returncode != 0:
        raise RuntimeError(
            f"J-Link exited with {completed.returncode}\n{output[-4000:]}"
        )
    if "Cortex-M85 identified." not in output:
        raise RuntimeError(f"J-Link did not identify the RA8P1 target\n{output[-4000:]}")
    return output


def parse_word_blocks(output: str, base_address: int, word_count: int) -> list[list[int]]:
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
        if address == base_address:
            if current is not None and len(current) >= word_count:
                blocks.append(current[:word_count])
            current = words
        elif current is not None and address == base_address + len(current) * 4:
            current.extend(words)
    if current is not None and len(current) >= word_count:
        blocks.append(current[:word_count])
    return blocks


def parse_samples(output: str, base_address: int) -> list[dict[str, int]]:
    cry_blocks = parse_word_blocks(output, base_address, len(SYMBOLS))
    mic_blocks = parse_word_blocks(output, MIC_IPC_ADDRESS, MIC_WORD_COUNT)
    if len(cry_blocks) != len(mic_blocks):
        raise RuntimeError(
            "J-Link cry/MIC sample counts differ: "
            f"{len(cry_blocks)} != {len(mic_blocks)}"
        )
    rows: list[dict[str, int]] = []
    for cry_words, mic_words in zip(cry_blocks, mic_blocks):
        row = dict(zip(SYMBOLS, cry_words))
        row.update(
            {
                field: mic_words[offset]
                for field, offset in MIC_FIELDS.items()
            }
        )
        rows.append(row)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", type=int, default=120)
    parser.add_argument("--interval-ms", type=int, default=250)
    parser.add_argument("--map", type=Path, default=DEFAULT_MAP)
    parser.add_argument("--jlink", type=Path, default=DEFAULT_JLINK)
    parser.add_argument("--output", type=Path, default=Path("test_results/cry_jlink"))
    parser.add_argument("--expect-no-cry", action="store_true")
    parser.add_argument("--raw-threshold", type=int, default=650)
    parser.add_argument("--allowed-raw-hits", type=int, default=0)
    parser.add_argument("--allowed-confirmed-samples", type=int, default=0)
    args = parser.parse_args()

    if args.samples <= 0 or args.interval_ms <= 0:
        parser.error("--samples and --interval-ms must be positive")
    addresses = symbol_addresses(args.map)
    output = run_jlink(
        args.jlink,
        addresses[SYMBOLS[0]],
        args.samples,
        args.interval_ms,
    )
    samples = parse_samples(output, addresses[SYMBOLS[0]])
    if len(samples) != args.samples:
        raise RuntimeError(
            f"expected {args.samples} memory samples, parsed {len(samples)}\n"
            f"{output[-8000:]}"
        )

    raw_scores = [row["g_cry_probability_x1000"] for row in samples]
    instant_scores = [
        row["g_cry_instant_probability_x1000"] for row in samples
    ]
    inference_counts = [row["g_cry_inference_count"] for row in samples]
    mic_peaks = [row["mic_peak"] for row in samples]
    mic_means = [row["mic_mean_abs"] for row in samples]
    mic_read_counts = [row["mic_read_count"] for row in samples]
    summary: dict[str, Any] = {
        "generated_at": datetime.now().astimezone().isoformat(),
        "samples": len(samples),
        "interval_ms": args.interval_ms,
        "duration_seconds": round(len(samples) * args.interval_ms / 1000.0, 3),
        "model_ready_samples": sum(
            row["g_cry_model_ready"] != 0 for row in samples
        ),
        "first_inference_count": inference_counts[0],
        "last_inference_count": inference_counts[-1],
        "inference_count_delta": inference_counts[-1] - inference_counts[0],
        "cry_raw_p50": percentile(raw_scores, 0.50),
        "cry_raw_p90": percentile(raw_scores, 0.90),
        "cry_raw_p95": percentile(raw_scores, 0.95),
        "cry_raw_p99": percentile(raw_scores, 0.99),
        "cry_raw_maximum": max(raw_scores),
        "cry_instant_p50": percentile(instant_scores, 0.50),
        "cry_instant_p90": percentile(instant_scores, 0.90),
        "cry_instant_p95": percentile(instant_scores, 0.95),
        "cry_instant_p99": percentile(instant_scores, 0.99),
        "cry_instant_maximum": max(instant_scores),
        "raw_threshold": args.raw_threshold,
        "raw_hit_samples": sum(score >= args.raw_threshold for score in raw_scores),
        "instant_raw_hit_samples": sum(
            score >= args.raw_threshold for score in instant_scores
        ),
        "confirmed_cry_samples": sum(
            row["g_cry_confirmed"] != 0 for row in samples
        ),
        "maximum_vote_hits": max(
            row["g_cry_consecutive_windows"] for row in samples
        ),
        "inference_ms_p95": percentile(
            [row["g_cry_last_inference_ms"] for row in samples],
            0.95,
        ),
        "snapshot_retries_delta": (
            samples[-1]["g_cry_snapshot_retries"]
            - samples[0]["g_cry_snapshot_retries"]
        ),
        "mic_valid_samples": sum(
            row["mic_magic"] == MIC_IPC_MAGIC
            and row["mic_sample_rate"] == 16000
            for row in samples
        ),
        "mic_read_count_delta": mic_read_counts[-1] - mic_read_counts[0],
        "mic_peak_p50": percentile(mic_peaks, 0.50),
        "mic_peak_p90": percentile(mic_peaks, 0.90),
        "mic_peak_p95": percentile(mic_peaks, 0.95),
        "mic_peak_maximum": max(mic_peaks),
        "mic_mean_abs_p50": percentile(mic_means, 0.50),
        "mic_mean_abs_p90": percentile(mic_means, 0.90),
        "mic_mean_abs_p95": percentile(mic_means, 0.95),
        "mic_mean_abs_maximum": max(mic_means),
    }
    summary["pass"] = (
        summary["model_ready_samples"] == len(samples)
        and summary["inference_count_delta"] > 0
        and summary["snapshot_retries_delta"] >= 0
        and summary["mic_valid_samples"] == len(samples)
        and summary["mic_read_count_delta"] > 0
    )
    if args.expect_no_cry:
        summary["pass"] = (
            summary["pass"]
            and summary["raw_hit_samples"] <= args.allowed_raw_hits
            and summary["confirmed_cry_samples"] <= args.allowed_confirmed_samples
        )

    output_dir = args.output / f'cry_{datetime.now().strftime("%Y%m%d_%H%M%S")}'
    output_dir.mkdir(parents=True, exist_ok=True)
    with (output_dir / "samples.csv").open(
        "w", newline="", encoding="utf-8-sig"
    ) as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=list(SYMBOLS) + list(MIC_FIELDS),
        )
        writer.writeheader()
        writer.writerows(samples)
    (output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f"Evidence: {output_dir}")
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from audio_pipeline.common import DATA_ROOT, ROOT, load_config, read_csv, write_csv, write_json
from audio_pipeline.dataset import build_feature_packages
from audio_pipeline.manifest import (
    MANIFEST_FIELDS,
    build_recording_manifest,
    prepare_recordings,
)


def summarize(rows: list[dict[str, str]]) -> dict[str, Any]:
    accepted = [row for row in rows if row["quality_status"] == "accepted"]
    rejected = [row for row in rows if row["quality_status"] != "accepted"]
    group_splits: dict[str, set[str]] = defaultdict(set)
    for row in accepted:
        group_splits[row["split_group"]].add(row["split"])
    leakage = {
        group: sorted(splits)
        for group, splits in group_splits.items()
        if len(splits) > 1
    }
    if leakage:
        raise RuntimeError(f"group leakage detected: {leakage}")

    split_detection: dict[str, Counter[str]] = defaultdict(Counter)
    split_types: dict[str, Counter[str]] = defaultdict(Counter)
    for row in accepted:
        split_detection[row["split"]][row["detection_label"]] += 1
        if row["classification_label"]:
            split_types[row["split"]][row["classification_label"]] += 1
    quality_reasons = Counter(
        reason
        for row in rejected
        for reason in row["quality_reasons"].split(";")
        if reason
    )
    return {
        "recordings_total": len(rows),
        "accepted": len(accepted),
        "rejected": len(rejected),
        "sources": dict(Counter(row["source"] for row in accepted)),
        "splits": dict(Counter(row["split"] for row in accepted)),
        "detection_by_split": {
            split: dict(counts) for split, counts in sorted(split_detection.items())
        },
        "type_by_split": {
            split: dict(counts) for split, counts in sorted(split_types.items())
        },
        "rejection_reasons": dict(quality_reasons),
        "unique_split_groups": len(group_splits),
        "group_leakage_count": 0,
    }


def run_index(args: argparse.Namespace, config: dict[str, Any]) -> Path:
    discovered = build_recording_manifest(config, args.data_root)
    if not discovered:
        raise RuntimeError(
            "no audio was discovered; run download_audio_datasets.py or add self-recorded data"
        )
    prepared = prepare_recordings(
        discovered,
        config,
        args.data_root,
        ffmpeg=args.ffmpeg,
        force=args.force,
        limit=args.limit_recordings,
    )
    manifest = args.data_root / "manifests" / "recordings.csv"
    write_csv(manifest, prepared, MANIFEST_FIELDS)
    report = summarize(prepared)
    write_json(args.data_root / "manifests" / "dataset_summary.json", report)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    print(f"Recording manifest: {manifest}")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Standardize, audit, split, and featurize cry audio without group leakage."
    )
    parser.add_argument("action", choices=("index", "features", "all", "audit"))
    parser.add_argument("--config", type=Path, default=ROOT / "audio_two_stage.yaml")
    parser.add_argument("--data-root", type=Path, default=DATA_ROOT)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--limit-recordings", type=int)
    parser.add_argument(
        "--augment-copies",
        type=int,
        help="Generic train-only copies per window (default: config value)",
    )
    parser.add_argument(
        "--device-chain-copies",
        type=int,
        help="RA8 capture-chain copies per public train window (default: config value)",
    )
    parser.add_argument(
        "--short-cry-copies",
        type=int,
        help="Short low-SNR cry copies per detector train window (default: config value)",
    )
    parser.add_argument(
        "--task",
        action="append",
        dest="tasks",
        help="Build only this feature task (may be repeated)",
    )
    args = parser.parse_args()
    args.data_root = args.data_root.resolve()
    config = load_config(args.config)
    manifest = args.data_root / "manifests" / "recordings.csv"

    if args.action in {"index", "all"}:
        manifest = run_index(args, config)
    if args.action == "audit":
        if not manifest.exists():
            raise SystemExit(f"manifest not found: {manifest}")
        report = summarize(read_csv(manifest))
        print(json.dumps(report, ensure_ascii=False, indent=2))
    if args.action in {"features", "all"}:
        if not manifest.exists():
            raise SystemExit(f"manifest not found: {manifest}; run index first")
        report = build_feature_packages(
            manifest,
            config,
            args.data_root,
            augment_copies=args.augment_copies,
            device_chain_copies=args.device_chain_copies,
            short_cry_copies=args.short_cry_copies,
            tasks=args.tasks,
            limit_recordings=args.limit_recordings,
        )
        print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

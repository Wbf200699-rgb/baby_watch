from __future__ import annotations

import argparse
import csv
import json
import shutil
import wave
from collections import Counter
from pathlib import Path

from audio_pipeline.common import DATA_ROOT, read_csv, resolve_data_path


DBL_TIER2_SOURCE = "infantcry_dbl_tier2"
LEGACY_TYPE_SOURCES = {"donateacry", "mendeley_infants_cry"}
PUBLIC_SOURCES = {
    "esc50",
    "donateacry",
    "mendeley_infants_cry",
    DBL_TIER2_SOURCE,
}
SPLITS = ("train", "validation", "test")
V1_TYPE_LABELS = {"hungry", "discomfort", "tired"}
DBL_TYPE_LABELS = {
    "hungry",
    "belly_pain",
    "discomfort",
    "tired",
    "burping",
}
INDEX_FIELDS = [
    "playback_path",
    "split",
    "classification_label",
    "source",
    "recording_id",
    "group_id",
    "split_group",
    "original_label",
    "source_path",
    "standardized_path",
    "duration_seconds",
    "license",
]


def safe_component(value: str, fallback: str) -> str:
    cleaned = "".join(
        character if character.isascii() and (character.isalnum() or character in "_-") else "_"
        for character in value.strip()
    ).strip("_")
    return cleaned or fallback


def write_playlist(path: Path, relative_paths: list[str]) -> None:
    path.write_text(
        "#EXTM3U\n" + "\n".join(relative_paths) + ("\n" if relative_paths else ""),
        encoding="utf-8",
    )


def validate_playable_wav(path: Path) -> None:
    with wave.open(str(path), "rb") as handle:
        actual = (
            handle.getframerate(),
            handle.getnchannels(),
            handle.getsampwidth(),
            handle.getcomptype(),
        )
    expected = (16000, 1, 2, "NONE")
    if actual != expected:
        raise ValueError(f"{path}: expected 16 kHz mono PCM16 WAV, got {actual}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Create a directly playable, leakage-safe WAV library from public cry datasets."
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DATA_ROOT / "manifests" / "recordings.csv",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DATA_ROOT / "ready_to_play" / "public_cry_wav",
    )
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    manifest = args.manifest.resolve()
    output = args.output.resolve()
    if not manifest.exists():
        raise SystemExit(f"recording manifest not found: {manifest}")
    output.mkdir(parents=True, exist_ok=True)

    selected = [
        row
        for row in read_csv(manifest)
        if row["quality_status"] == "accepted"
        and row["detection_label"] == "cry"
        and row["source"] in PUBLIC_SOURCES
        and row["split"] in SPLITS
    ]
    if not selected:
        raise SystemExit("no accepted public cry recordings were found")

    index_rows: list[dict[str, str]] = []
    playlists: dict[str, list[str]] = {
        "all": [],
        "classification_v1_all": [],
        "dbl_tier2_all": [],
    }
    for split in SPLITS:
        playlists[split] = []
        playlists[f"classification_v1_{split}"] = []
        playlists[f"dbl_tier2_{split}"] = []
        for label in DBL_TYPE_LABELS:
            playlists[f"dbl_tier2_{split}_{label}"] = []

    copied = skipped = 0
    for row in sorted(
        selected,
        key=lambda item: (
            SPLITS.index(item["split"]),
            item["classification_label"] or "untyped_cry",
            item["source"],
            item["recording_id"],
        ),
    ):
        split = row["split"]
        label = safe_component(row["classification_label"], "untyped_cry")
        source_name = safe_component(row["source"], "public")
        filename = f"{source_name}_{row['recording_id']}.wav"
        destination = output / split / label / filename
        destination.parent.mkdir(parents=True, exist_ok=True)
        source = resolve_data_path(row["standardized_path"], DATA_ROOT)
        if not source.is_file():
            raise FileNotFoundError(f"standardized WAV is missing: {source}")
        if (
            not args.force
            and destination.exists()
            and destination.stat().st_size == source.stat().st_size
        ):
            skipped += 1
        else:
            shutil.copy2(source, destination)
            copied += 1
        validate_playable_wav(destination)

        relative = destination.relative_to(output).as_posix()
        item = {
            "playback_path": relative,
            "split": split,
            "classification_label": row["classification_label"],
            "source": row["source"],
            "recording_id": row["recording_id"],
            "group_id": row["group_id"],
            "split_group": row["split_group"],
            "original_label": row["original_label"],
            "source_path": row["source_path"],
            "standardized_path": row["standardized_path"],
            "duration_seconds": row["duration_seconds"],
            "license": row["license"],
        }
        index_rows.append(item)
        playlists["all"].append(relative)
        playlists[split].append(relative)
        if (
            row["source"] in LEGACY_TYPE_SOURCES
            and row["classification_label"] in V1_TYPE_LABELS
        ):
            playlists["classification_v1_all"].append(relative)
            playlists[f"classification_v1_{split}"].append(relative)
        if row["source"] == DBL_TIER2_SOURCE:
            label_name = row["classification_label"]
            if label_name not in DBL_TYPE_LABELS:
                raise ValueError(
                    f"unexpected InfantCry-DBL Tier 2 label: {label_name!r}"
                )
            playlists["dbl_tier2_all"].append(relative)
            playlists[f"dbl_tier2_{split}"].append(relative)
            playlists[f"dbl_tier2_{split}_{label_name}"].append(relative)

    with (output / "index.csv").open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=INDEX_FIELDS)
        writer.writeheader()
        writer.writerows(index_rows)

    write_playlist(output / "all_public_cry.m3u", playlists["all"])
    legacy_playlist = output / "classification_v1_hungry_discomfort_tired.m3u"
    if legacy_playlist.exists():
        legacy_playlist.unlink()
    write_playlist(
        output / "classification_v1_all_for_listening_only.m3u",
        playlists["classification_v1_all"],
    )
    write_playlist(
        output / "dbl_tier2_all_for_listening_only.m3u",
        playlists["dbl_tier2_all"],
    )
    for split in SPLITS:
        write_playlist(output / f"{split}.m3u", playlists[split])
        write_playlist(
            output / f"classification_v1_{split}.m3u",
            playlists[f"classification_v1_{split}"],
        )
        write_playlist(
            output / f"dbl_tier2_{split}.m3u",
            playlists[f"dbl_tier2_{split}"],
        )
        for label in sorted(DBL_TYPE_LABELS):
            write_playlist(
                output / f"dbl_tier2_{split}_{label}.m3u",
                playlists[f"dbl_tier2_{split}_{label}"],
            )

    by_split = Counter(row["split"] for row in index_rows)
    by_label = Counter(row["classification_label"] or "untyped_cry" for row in index_rows)
    by_source = Counter(row["source"] for row in index_rows)
    summary = {
        "recordings": len(index_rows),
        "copied": copied,
        "already_present": skipped,
        "format": "WAV, 16000 Hz, mono, PCM16",
        "by_split": dict(by_split),
        "by_classification_label": dict(by_label),
        "by_source": dict(by_source),
        "classification_v1_labels": sorted(V1_TYPE_LABELS),
        "classification_v1_recordings": len(playlists["classification_v1_all"]),
        "dbl_tier2_labels": sorted(DBL_TYPE_LABELS),
        "dbl_tier2_recordings": len(playlists["dbl_tier2_all"]),
        "dbl_tier2_test_recordings": len(playlists["dbl_tier2_test"]),
    }
    (output / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    (output / "README_zh-CN.txt").write_text(
        "RA8P1 可直接播放的公开婴儿哭声 WAV\n"
        "===================================\n\n"
        "全部文件已统一为 16000 Hz、单声道、16-bit PCM WAV。\n"
        "目录已经按 train / validation / test 固定划分，请勿互换。\n"
        "train.m3u、validation.m3u、test.m3u 可直接拖入常见播放器。\n"
        "classification_v1_train/validation/test.m3u 是第一版三分类的独立列表。\n"
        "classification_v1_all_for_listening_only.m3u 只能试听，不能整份用于训练。\n"
        "dbl_tier2_test_类别名.m3u 是新版五分类逐类板端测试列表。\n"
        "首次测试只能播放 dbl_tier2_test_*.m3u，不能播放 train/validation 列表。\n"
        "untyped_cry 为 ESC-50 哭声，只用于哭声检测，不用于原因分类。\n"
        "index.csv 保存每个 WAV 的原始来源、标签、分组和许可信息。\n\n"
        "这些文件只用于本项目的研究和教学。不要提交到 Git，也不要对外分发。\n"
        "ESC-50 全集为非商业许可；Donate-a-Cry、Mendeley、InfantCry-DBL "
        "的许可见 index.csv。\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f"Playable library: {output}")


if __name__ == "__main__":
    main()

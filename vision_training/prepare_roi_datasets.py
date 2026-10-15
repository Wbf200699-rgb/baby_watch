from __future__ import annotations

import argparse
import csv
import json
import shutil
from collections import Counter
from pathlib import Path

from PIL import Image

from vision_pipeline.image_utils import difference_hash, expanded_bbox, hamming_distance
from vision_pipeline.splits import assert_group_isolation, stable_group_split


ROOT = Path(__file__).resolve().parent
TASKS = {
    "occlusion": {
        "labels": ("clear", "partial", "covered", "not_visible"),
        "bbox_prefix": "head",
        "scale": 1.25,
    },
    "behavior": {
        "labels": ("quiet", "active", "edge_grab", "caregiver", "unknown"),
        "bbox_prefix": "infant",
        "scale": 1.12,
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="按场次隔离切分并生成两个 MobileNetV3 ROI 分类数据集")
    parser.add_argument(
        "--annotations",
        type=Path,
        default=ROOT / "data" / "raw" / "self_capture" / "frame_annotations.csv",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "data" / "processed" / "roi_classification",
    )
    parser.add_argument("--task", choices=("occlusion", "behavior", "all"), default="all")
    parser.add_argument("--seed", type=int, default=20260725)
    parser.add_argument("--near-duplicate-distance", type=int, default=2)
    parser.add_argument("--force", action="store_true", help="删除并重建所选任务的输出目录")
    return parser.parse_args()


def truthy(value: str) -> bool:
    return value.strip().lower() not in {"", "0", "false", "no", "n", "skip"}


def resolve_image(csv_path: Path, value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = (csv_path.parent / path).resolve()
    return path


def parse_bbox(row: dict[str, str], prefix: str) -> list[float] | None:
    values = []
    for suffix in ("x", "y", "w", "h"):
        value = row.get(f"{prefix}_{suffix}", "").strip()
        if not value:
            return None
        values.append(float(value))
    return values


def choose_group(row: dict[str, str]) -> str:
    for key in ("real_infant_id", "source_video_id", "doll_id", "subject_id", "session_id"):
        value = row.get(key, "").strip()
        if value:
            return f"{key}:{value}"
    raise ValueError(f"缺少可用于防泄漏切分的 group 字段: {row.get('image_path', '')}")


def main() -> int:
    args = parse_args()
    if not args.annotations.is_file():
        raise SystemExit(f"找不到标注 CSV: {args.annotations}")
    with args.annotations.open("r", newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))

    tasks = TASKS if args.task == "all" else {args.task: TASKS[args.task]}
    report: dict[str, object] = {"source_csv": str(args.annotations), "tasks": {}}
    for task_name, spec in tasks.items():
        task_output = args.output / task_name
        if task_output.exists() and any(task_output.iterdir()):
            if not args.force:
                raise SystemExit(f"输出目录非空；确认后加 --force: {task_output}")
            shutil.rmtree(task_output)
        manifest_rows: list[dict[str, str]] = []
        counts: Counter[tuple[str, str]] = Counter()
        last_hash_by_group_label: dict[tuple[str, str], int] = {}
        skipped = Counter()

        for row_number, row in enumerate(rows, start=2):
            if not truthy(row.get("include", "1")):
                skipped["include_false"] += 1
                continue
            label = row.get(f"{task_name}_label", "").strip()
            if not label:
                skipped["label_empty"] += 1
                continue
            if label not in spec["labels"]:
                raise ValueError(f"第 {row_number} 行 {task_name}_label 非法: {label}")
            image_path = resolve_image(args.annotations, row.get("image_path", ""))
            if not image_path.is_file():
                raise FileNotFoundError(f"第 {row_number} 行图片不存在: {image_path}")
            bbox = parse_bbox(row, str(spec["bbox_prefix"]))
            if bbox is None:
                skipped["bbox_empty"] += 1
                continue
            group = choose_group(row)
            explicit_split = row.get("split", "").strip().lower()
            split = explicit_split or stable_group_split(group, seed=args.seed)
            if split == "validation":
                split = "val"
            if split not in {"train", "val", "test"}:
                raise ValueError(f"第 {row_number} 行 split 非法: {split}")

            with Image.open(image_path) as source_image:
                source_image = source_image.convert("RGB")
                crop_box = expanded_bbox(
                    bbox,
                    source_image.width,
                    source_image.height,
                    float(spec["scale"]),
                )
                if crop_box is None:
                    skipped["bbox_invalid"] += 1
                    continue
                x, y, width, height = crop_box
                crop = source_image.crop(
                    (round(x), round(y), round(x + width), round(y + height))
                )
                image_hash = difference_hash(crop)
                dedup_key = (group, label)
                previous_hash = last_hash_by_group_label.get(dedup_key)
                if (
                    previous_hash is not None
                    and hamming_distance(previous_hash, image_hash) <= args.near_duplicate_distance
                ):
                    skipped["near_duplicate"] += 1
                    continue
                last_hash_by_group_label[dedup_key] = image_hash

                destination_dir = args.output / task_name / split / label
                destination_dir.mkdir(parents=True, exist_ok=True)
                destination = destination_dir / f"{task_name}_{row_number:07d}.jpg"
                crop.resize((192, 192), Image.Resampling.BILINEAR).save(
                    destination,
                    quality=92,
                    optimize=True,
                )

            manifest_rows.append(
                {
                    "image_path": str(destination.resolve()),
                    "source_image": str(image_path.resolve()),
                    "group_id": group,
                    "split": split,
                    "label": label,
                    "session_id": row.get("session_id", ""),
                    "quality": row.get("quality", ""),
                    "notes": row.get("notes", ""),
                }
            )
            counts[(split, label)] += 1

        assert_group_isolation(manifest_rows)
        manifest_path = args.output / task_name / "manifest.csv"
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        with manifest_path.open("w", newline="", encoding="utf-8-sig") as handle:
            fieldnames = [
                "image_path",
                "source_image",
                "group_id",
                "split",
                "label",
                "session_id",
                "quality",
                "notes",
            ]
            writer = csv.DictWriter(handle, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(manifest_rows)

        report["tasks"][task_name] = {
            "samples": len(manifest_rows),
            "counts": {
                f"{split}/{label}": count for (split, label), count in sorted(counts.items())
            },
            "skipped": dict(skipped),
            "manifest": str(manifest_path),
        }
        print(f"{task_name}: {len(manifest_rows)} 个 ROI")

    report_path = args.output / "roi_dataset_audit.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"审计报告: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

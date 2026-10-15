from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
from collections import Counter
from collections import defaultdict
from pathlib import Path
from typing import Any

from vision_pipeline.coco import (
    TARGET_CATEGORIES,
    TARGET_CATEGORY_IDS,
    SceneImage,
    discover_coco_jsons,
    load_coco_sources,
    select_syrip_jsons,
)
from vision_pipeline.splits import assert_group_isolation


ROOT = Path(__file__).resolve().parent


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="将 CribHD、SyRIP 和自采 COCO 标注合并为 YOLOX 使用的统一 COCO 数据集"
    )
    parser.add_argument("--cribhd-root", type=Path, default=ROOT / "data" / "raw" / "CribHD")
    parser.add_argument("--syrip-root", type=Path, default=ROOT / "data" / "raw" / "SyRIP")
    parser.add_argument("--self-root", type=Path, default=ROOT / "data" / "raw" / "self_capture")
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "data" / "processed" / "scene_coco",
    )
    parser.add_argument("--link-mode", choices=("hardlink", "copy"), default="hardlink")
    parser.add_argument("--force", action="store_true", help="仅删除并重建 --output 指定目录")
    parser.add_argument("--allow-missing-source", action="store_true")
    return parser.parse_args()


def safe_name(record: SceneImage) -> str:
    clean = re.sub(r"[^A-Za-z0-9_.-]+", "_", record.image_path.name)
    digest = hashlib.sha1(
        f"{record.source}:{record.image_path.resolve()}".encode("utf-8")
    ).hexdigest()[:12]
    return f"{record.source}_{digest}_{clean}"


def materialize(source: Path, destination: Path, mode: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        return
    if mode == "hardlink":
        try:
            os.link(source, destination)
            return
        except OSError:
            pass
    shutil.copy2(source, destination)


def write_split(records: list[SceneImage], output: Path, split: str, link_mode: str) -> dict[str, Any]:
    image_dir_name = {"train": "train2017", "val": "val2017", "test": "test2017"}[split]
    image_dir = output / image_dir_name
    image_dir.mkdir(parents=True, exist_ok=True)
    images: list[dict[str, Any]] = []
    annotations: list[dict[str, Any]] = []
    class_counts: Counter[str] = Counter()

    for image_id, record in enumerate(records, start=1):
        destination_name = safe_name(record)
        materialize(record.image_path, image_dir / destination_name, link_mode)
        images.append(
            {
                "id": image_id,
                "file_name": destination_name,
                "width": record.width,
                "height": record.height,
                "source": record.source,
                "source_image_id": record.source_image_id,
                "group_id": record.group_id,
            }
        )
        for obj in record.objects:
            annotation_id = len(annotations) + 1
            x, y, width, height = obj.bbox
            annotations.append(
                {
                    "id": annotation_id,
                    "image_id": image_id,
                    "category_id": TARGET_CATEGORY_IDS[obj.category],
                    "bbox": [round(x, 3), round(y, 3), round(width, 3), round(height, 3)],
                    "area": round(width * height, 3),
                    "iscrowd": 0,
                    "attributes": {
                        "source_category": obj.source_category,
                        **obj.attributes,
                    },
                }
            )
            class_counts[obj.category] += 1

    document = {
        "info": {
            "description": "RA8P1 baby-monitor unified scene dataset",
            "version": "1",
            "split": split,
        },
        "licenses": [],
        "categories": [
            {"id": TARGET_CATEGORY_IDS[name], "name": name, "supercategory": "scene"}
            for name in TARGET_CATEGORIES
        ],
        "images": images,
        "annotations": annotations,
    }
    annotation_dir = output / "annotations"
    annotation_dir.mkdir(parents=True, exist_ok=True)
    annotation_path = annotation_dir / f"instances_{image_dir_name}.json"
    annotation_path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    return {
        "images": len(images),
        "annotations": len(annotations),
        "classes": dict(sorted(class_counts.items())),
        "annotation_file": str(annotation_path),
    }


def enforce_group_isolation(records: list[SceneImage]) -> dict[str, Any]:
    """Quarantine overlapping public sources and reject self-capture leakage."""
    grouped: dict[tuple[str, str], list[SceneImage]] = defaultdict(list)
    for record in records:
        grouped[(record.source, record.group_id)].append(record)

    conflict_groups = 0
    moved_images: Counter[str] = Counter()
    examples: list[dict[str, Any]] = []
    for (source, group_id), items in grouped.items():
        splits = {item.split for item in items}
        if len(splits) <= 1:
            continue
        if source.lower().startswith("self"):
            raise ValueError(
                f"自采场次跨数据划分: source={source} group={group_id} splits={sorted(splits)}"
            )
        conflict_groups += 1
        # Holdout wins: never move validation/test frames back into training.
        destination = "test" if "test" in splits else "val"
        for item in items:
            if item.split != destination:
                moved_images[f"{item.split}->{destination}"] += 1
                item.split = destination
        if len(examples) < 20:
            examples.append(
                {
                    "source": source,
                    "group_id": group_id,
                    "original_splits": sorted(splits),
                    "destination": destination,
                    "images": len(items),
                }
            )

    assert_group_isolation(
        {
            "group_id": f"{record.source}:{record.group_id}",
            "split": record.split,
        }
        for record in records
    )
    return {
        "conflict_groups_quarantined": conflict_groups,
        "moved_images": dict(sorted(moved_images.items())),
        "examples": examples,
        "policy": "public overlap -> holdout split; self-capture overlap -> hard failure",
    }


def main() -> int:
    args = parse_args()
    sources: list[tuple[str, Path, list[Path]]] = []
    if args.cribhd_root.exists():
        sources.append(("cribhd", args.cribhd_root, discover_coco_jsons(args.cribhd_root)))
    elif not args.allow_missing_source:
        raise SystemExit(f"找不到 CribHD: {args.cribhd_root}")

    if args.syrip_root.exists():
        sources.append(("syrip", args.syrip_root, select_syrip_jsons(args.syrip_root)))
    elif not args.allow_missing_source:
        raise SystemExit(f"找不到 SyRIP: {args.syrip_root}")

    if args.self_root.exists():
        self_jsons = discover_coco_jsons(args.self_root)
        if self_jsons:
            sources.append(("self_capture", args.self_root, self_jsons))

    if not sources:
        raise SystemExit("没有找到任何可处理的 COCO 数据源")

    if args.output.exists() and any(args.output.iterdir()):
        if not args.force:
            raise SystemExit(f"输出目录非空；确认后加 --force: {args.output}")
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True, exist_ok=True)

    records: list[SceneImage] = []
    audits: list[dict[str, Any]] = []
    for source_name, root, json_paths in sources:
        converted, audit = load_coco_sources(
            root,
            source=source_name,
            json_paths=json_paths,
            derive_syrip_heads=True,
            # Empty self-capture frames are valuable hard negatives for infant
            # detection.  Public datasets are still filtered to annotated
            # records because their empty records may merely be incomplete.
            keep_empty=source_name == "self_capture",
        )
        records.extend(converted)
        audits.append(audit.as_dict())
        print(
            f"{source_name}: json={len(json_paths)} images={len(converted)} "
            f"annotations={audit.annotations_kept}"
        )

    try:
        group_isolation = enforce_group_isolation(records)
    except ValueError as exc:
        raise SystemExit(f"场次隔离检查失败: {exc}") from exc

    split_records = {
        split: [record for record in records if record.split == split]
        for split in ("train", "val", "test")
    }
    summaries = {
        split: write_split(split_records[split], args.output, split, args.link_mode)
        for split in ("train", "val", "test")
    }
    report = {
        "target_classes": list(TARGET_CATEGORIES),
        "sources": audits,
        "group_isolation": group_isolation,
        "splits": summaries,
        "notes": [
            "SyRIP head_face boxes are weak labels derived from visible face keypoints.",
            "CribHD-C images without annotations are not treated as negative samples.",
            "Public official splits are preserved unless one source group crosses splits; overlapping groups are quarantined to holdout.",
            "Self-capture split leakage is a hard error and is never repaired silently.",
        ],
    }
    report_path = args.output / "dataset_audit.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"完成: {args.output}")
    print(json.dumps(summaries, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

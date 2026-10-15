from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any

from vision_pipeline.coco import TARGET_CATEGORIES, TARGET_CATEGORY_IDS


ROOT = Path(__file__).resolve().parent


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="YOLOX 基础训练/玩偶微调前的数据合约检查")
    parser.add_argument(
        "--data-root",
        type=Path,
        default=ROOT / "data" / "processed" / "scene_coco",
    )
    parser.add_argument("--stage", required=True, choices=("base", "finetune"))
    parser.add_argument("--report", type=Path)
    return parser.parse_args()


def inspect_split(data_root: Path, split: str) -> dict[str, Any]:
    directory = {"train": "train2017", "val": "val2017", "test": "test2017"}[split]
    annotation_path = data_root / "annotations" / f"instances_{directory}.json"
    if not annotation_path.is_file():
        raise FileNotFoundError(annotation_path)
    document = json.loads(annotation_path.read_text(encoding="utf-8-sig"))
    categories = {
        int(category["id"]): str(category["name"])
        for category in document.get("categories", [])
    }
    expected_categories = {
        TARGET_CATEGORY_IDS[name]: name for name in TARGET_CATEGORIES
    }
    failures: list[str] = []
    if categories != expected_categories:
        failures.append(
            f"{split}: 类别 ID/顺序不一致: {categories} != {expected_categories}"
        )

    images = {int(image["id"]): image for image in document.get("images", [])}
    missing_files: list[str] = []
    for image in images.values():
        path = data_root / directory / str(image["file_name"])
        if not path.is_file():
            missing_files.append(str(image["file_name"]))
    if missing_files:
        failures.append(f"{split}: 缺少 {len(missing_files)} 张物化图片")

    counts: Counter[str] = Counter()
    invalid_boxes = 0
    unknown_image_ids = 0
    for annotation in document.get("annotations", []):
        image = images.get(int(annotation.get("image_id", -1)))
        if image is None:
            unknown_image_ids += 1
            continue
        category = categories.get(int(annotation.get("category_id", -1)))
        if category is None:
            continue
        bbox = annotation.get("bbox", [])
        if len(bbox) != 4:
            invalid_boxes += 1
            continue
        x, y, width, height = (float(value) for value in bbox)
        image_width = float(image["width"])
        image_height = float(image["height"])
        if (
            width <= 0
            or height <= 0
            or x < 0
            or y < 0
            or x + width > image_width + 0.01
            or y + height > image_height + 0.01
        ):
            invalid_boxes += 1
            continue
        counts[category] += 1
    if invalid_boxes:
        failures.append(f"{split}: {invalid_boxes} 个非法 bbox")
    if unknown_image_ids:
        failures.append(f"{split}: {unknown_image_ids} 个标注引用未知 image_id")

    sources = Counter(str(image.get("source", "unknown")) for image in images.values())
    return {
        "split": split,
        "images": len(images),
        "annotations": sum(counts.values()),
        "class_counts": {name: counts[name] for name in TARGET_CATEGORIES},
        "sources": dict(sorted(sources.items())),
        "missing_files_count": len(missing_files),
        "invalid_boxes": invalid_boxes,
        "failures": failures,
    }


def validate_stage(data_root: Path, stage: str) -> dict[str, Any]:
    splits = {
        split: inspect_split(data_root, split)
        for split in ("train", "val", "test")
    }
    failures = [
        failure
        for split_report in splits.values()
        for failure in split_report["failures"]
    ]
    warnings: list[str] = []
    required = (
        TARGET_CATEGORIES[:-1] if stage == "base" else TARGET_CATEGORIES
    )
    for split in ("train", "val"):
        for category in required:
            if splits[split]["class_counts"][category] <= 0:
                failures.append(f"{stage}/{split}: 缺少类别 {category}")

    if stage == "base":
        if any(
            splits[split]["sources"].get("self_capture", 0) > 0
            for split in ("train", "val")
        ):
            warnings.append("base 阶段已包含 self_capture；确认这不是误把微调数据提前混入")
        if (
            splits["train"]["class_counts"]["caregiver"] == 0
            or splits["val"]["class_counts"]["caregiver"] == 0
        ):
            warnings.append("base 阶段允许 caregiver 为空；该类必须在 finetune 前补齐")
    else:
        for split in ("train", "val"):
            if splits[split]["sources"].get("self_capture", 0) <= 0:
                failures.append(f"finetune/{split}: 没有 self_capture 图片")
        if splits["test"]["sources"].get("self_capture", 0) <= 0:
            warnings.append("test 没有独立 self_capture 场次，不能作为最终设备域测试")
        for category in TARGET_CATEGORIES:
            if splits["test"]["class_counts"][category] <= 0:
                warnings.append(f"test 缺少类别 {category}")

    return {
        "stage": stage,
        "data_root": str(data_root.resolve()),
        "target_classes": list(TARGET_CATEGORIES),
        "splits": splits,
        "failures": failures,
        "warnings": warnings,
        "passed": not failures,
    }


def main() -> int:
    args = parse_args()
    try:
        report = validate_stage(args.data_root, args.stage)
    except (FileNotFoundError, json.JSONDecodeError) as exc:
        raise SystemExit(f"数据集读取失败: {exc}") from exc
    report_path = args.report or args.data_root / f"preflight_{args.stage}.json"
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    for warning in report["warnings"]:
        print(f"警告: {warning}")
    if report["failures"]:
        for failure in report["failures"]:
            print(f"错误: {failure}")
        print(f"报告: {report_path}")
        return 2
    print(f"{args.stage} 数据预检通过: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

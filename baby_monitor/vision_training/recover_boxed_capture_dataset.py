from __future__ import annotations

import argparse
import csv
import json
import math
import re
import shutil
from collections import Counter
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

import cv2

from audit_boxed_capture_dataset import (
    COPY_MARKER,
    decode_image,
    difference_hash,
    hamming_distance,
    red_rectangle,
)
from vision_pipeline.coco import TARGET_CATEGORIES, TARGET_CATEGORY_IDS


ROOT = Path(__file__).resolve().parent
IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
POSITIVE_FOLDERS = {
    "baby_arms",
    "baby_covered",
    "baby_no_sheet",
    "baby_sheet",
    "near_side",
}
OCCLUSION_LABELS = {
    "baby_arms": "clear",
    "baby_no_sheet": "clear",
    "near_side": "clear",
    # baby_sheet mixes clear faces, turned-away faces and body-only blanket
    # contact.  It is useful for detection/behavior, but is not a defensible
    # frame-level mouth/nose "partial" label.
    "baby_sheet": "",
    "baby_covered": "covered",
    "non_baby_day": "not_visible",
    "non_baby_night": "not_visible",
}
BEHAVIOR_LABELS = {
    "baby_no_sheet": "quiet",
    "baby_sheet": "quiet",
    "baby_covered": "quiet",
    "baby_arms": "active",
    "near_side": "edge_grab",
    "non_baby_day": "unknown",
    "non_baby_night": "unknown",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "把红框 JPG 的坐标恢复到同名 SD 原图，生成不含红框像素的 "
            "COCO 检测集和弱标签 ROI CSV"
        )
    )
    parser.add_argument(
        "--archive",
        type=Path,
        default=ROOT.parent / "ml_training" / "data" / "打框数据集.zip",
    )
    parser.add_argument(
        "--raw-root",
        type=Path,
        default=ROOT / "data" / "raw" / "self_capture_unboxed",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "data" / "raw" / "self_capture" / "prototype_v1",
    )
    parser.add_argument(
        "--stride",
        type=int,
        default=3,
        help="每个连续序列每 N 帧取一帧，降低相邻重复帧占比",
    )
    parser.add_argument(
        "--guard-frames",
        type=int,
        default=5,
        help="训练/验证/测试时间分界两侧丢弃的帧数",
    )
    parser.add_argument("--max-dhash-distance", type=int, default=16)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def natural_key(name: str) -> list[int | str]:
    return [
        int(token) if token.isdigit() else token.casefold()
        for token in re.split(r"(\d+)", name)
    ]


def split_for_index(
    index: int,
    count: int,
    guard_frames: int,
) -> str | None:
    train_end = max(1, math.floor(count * 0.70))
    val_end = max(train_end + 1, math.floor(count * 0.85))
    if abs(index - train_end) < guard_frames or abs(index - val_end) < guard_frames:
        return None
    if index < train_end:
        return "train"
    if index < val_end:
        return "val"
    return "test"


def archive_images(archive: ZipFile) -> dict[tuple[str, str], str]:
    result: dict[tuple[str, str], str] = {}
    for info in archive.infolist():
        path = PurePosixPath(info.filename)
        if (
            info.is_dir()
            or path.suffix.lower() not in IMAGE_SUFFIXES
            or COPY_MARKER in path.stem
            or len(path.parts) < 2
        ):
            continue
        key = (path.parts[-2], path.name)
        if key in result:
            raise ValueError(f"ZIP 中出现重复目录/文件名: {key}")
        result[key] = info.filename
    return result


def raw_images(root: Path) -> dict[tuple[str, str], Path]:
    result: dict[tuple[str, str], Path] = {}
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in IMAGE_SUFFIXES:
            continue
        key = (path.parent.name, path.name)
        if key in result:
            raise ValueError(f"原图中出现重复目录/文件名: {key}")
        result[key] = path
    return result


def materialize(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        destination.hardlink_to(source)
    except OSError:
        shutil.copy2(source, destination)


def write_coco(
    output: Path,
    split: str,
    images: list[dict[str, object]],
    annotations: list[dict[str, object]],
) -> Path:
    split_root = output / "coco" / split / f"prototype_v1_{split}"
    annotation_path = split_root / "instances.json"
    annotation_path.parent.mkdir(parents=True, exist_ok=True)
    document = {
        "info": {
            "description": (
                "RA8P1 self-capture prototype; coordinates recovered from "
                "boxed copies and applied to unboxed SD originals"
            ),
            "version": "prototype_v1",
            "split": split,
            "limitations": (
                "Split by guarded chronological ranges from the same capture "
                "folders; not an independent-session final evaluation."
            ),
        },
        "licenses": [],
        "categories": [
            {
                "id": TARGET_CATEGORY_IDS[name],
                "name": name,
                "supercategory": "scene",
            }
            for name in TARGET_CATEGORIES
        ],
        "images": images,
        "annotations": annotations,
    }
    annotation_path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    return annotation_path


def main() -> int:
    args = parse_args()
    if args.stride <= 0:
        raise SystemExit("--stride 必须大于 0")
    if args.guard_frames < 0:
        raise SystemExit("--guard-frames 不能为负数")
    if not args.archive.is_file():
        raise SystemExit(f"找不到红框 ZIP: {args.archive}")
    if not args.raw_root.is_dir():
        raise SystemExit(f"找不到 SD 原图目录: {args.raw_root}")
    if args.output.exists():
        if not args.force:
            raise SystemExit(f"输出目录已存在；确认重建后加 --force: {args.output}")
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True, exist_ok=True)

    raw_by_key = raw_images(args.raw_root)
    roi_rows: list[dict[str, str | int]] = []
    split_images: dict[str, list[dict[str, object]]] = {
        "train": [],
        "val": [],
        "test": [],
    }
    split_annotations: dict[str, list[dict[str, object]]] = {
        "train": [],
        "val": [],
        "test": [],
    }
    counts: Counter[str] = Counter()
    dhash_distances: Counter[int] = Counter()
    mismatch_samples: list[dict[str, object]] = []
    raw_rectangle_samples: list[str] = []
    skipped = Counter()

    with ZipFile(args.archive) as archive:
        boxed_by_key = archive_images(archive)
        missing_raw = sorted(set(boxed_by_key) - set(raw_by_key))
        extra_raw = sorted(set(raw_by_key) - set(boxed_by_key))
        if missing_raw or extra_raw:
            raise SystemExit(
                f"红框图/原图不能一一对应: missing_raw={len(missing_raw)} "
                f"extra_raw={len(extra_raw)}"
            )

        folders = sorted({folder for folder, _ in boxed_by_key})
        for folder in folders:
            keys = sorted(
                (key for key in boxed_by_key if key[0] == folder),
                key=lambda item: natural_key(item[1]),
            )
            # baby_arms is the only available proxy for active motion and is
            # much smaller than the quiet folders, so retain every frame there.
            folder_stride = 1 if folder == "baby_arms" else args.stride
            for index, key in enumerate(keys):
                if index % folder_stride:
                    skipped["stride"] += 1
                    continue
                split = split_for_index(index, len(keys), args.guard_frames)
                if split is None:
                    skipped["split_guard"] += 1
                    continue

                boxed = decode_image(archive.read(boxed_by_key[key]))
                raw_path = raw_by_key[key]
                raw = cv2.imread(str(raw_path), cv2.IMREAD_COLOR)
                if raw is None:
                    skipped["raw_decode"] += 1
                    continue
                if raw.shape != boxed.shape:
                    skipped["dimension_mismatch"] += 1
                    mismatch_samples.append(
                        {
                            "folder": folder,
                            "image": key[1],
                            "boxed_shape": list(boxed.shape),
                            "raw_shape": list(raw.shape),
                        }
                    )
                    continue

                distance = hamming_distance(
                    difference_hash(boxed),
                    difference_hash(raw),
                )
                dhash_distances[distance] += 1
                if distance > args.max_dhash_distance:
                    skipped["dhash_mismatch"] += 1
                    if len(mismatch_samples) < 30:
                        mismatch_samples.append(
                            {
                                "folder": folder,
                                "image": key[1],
                                "dhash_distance": distance,
                            }
                        )
                    continue

                box, support = red_rectangle(boxed)
                raw_box, _ = red_rectangle(raw)
                if raw_box is not None and len(raw_rectangle_samples) < 30:
                    raw_rectangle_samples.append(f"{folder}/{key[1]}")
                positive = folder in POSITIVE_FOLDERS
                if positive and box is None:
                    skipped["positive_without_box"] += 1
                    continue
                if not positive and box is not None:
                    skipped["negative_with_box"] += 1
                    continue

                image_id = len(split_images[split]) + 1
                session_id = f"prototype_v1_{folder}_{split}"
                destination_name = f"{folder}_{key[1]}"
                destination = (
                    args.output
                    / "coco"
                    / split
                    / f"prototype_v1_{split}"
                    / "images"
                    / destination_name
                )
                materialize(raw_path, destination)
                relative_to_output = destination.relative_to(args.output).as_posix()
                height, width = raw.shape[:2]
                split_images[split].append(
                    {
                        "id": image_id,
                        "file_name": destination.name,
                        "width": width,
                        "height": height,
                        "session_id": session_id,
                        "source_folder": folder,
                        "source_file": key[1],
                    }
                )

                if positive and box is not None:
                    annotation_id = len(split_annotations[split]) + 1
                    x, y, box_width, box_height = box
                    split_annotations[split].append(
                        {
                            "id": annotation_id,
                            "image_id": image_id,
                            "category_id": TARGET_CATEGORY_IDS["infant"],
                            "bbox": box,
                            "area": box_width * box_height,
                            "iscrowd": 0,
                            "attributes": {
                                "source": "recovered_red_rectangle",
                                "perimeter_support": round(support, 4),
                                "weak_annotation": True,
                            },
                        }
                    )
                    roi_box = box
                else:
                    roi_box = [0, 0, width, height]

                x, y, box_width, box_height = roi_box
                roi_rows.append(
                    {
                        "image_path": relative_to_output,
                        "session_id": session_id,
                        # The historical folders do not carry a trustworthy
                        # doll/session identity.  Leave higher-priority group
                        # fields blank so the explicit prototype segment ID is
                        # used and the limitation stays visible in the report.
                        "doll_id": "",
                        "real_infant_id": "",
                        "source_video_id": "",
                        "split": split,
                        "include": 1,
                        # These are infant/scene ROIs, not final manually
                        # annotated anatomical head ROIs.
                        "head_x": x,
                        "head_y": y,
                        "head_w": box_width,
                        "head_h": box_height,
                        "infant_x": x,
                        "infant_y": y,
                        "infant_w": box_width,
                        "infant_h": box_height,
                        "occlusion_label": OCCLUSION_LABELS[folder],
                        "behavior_label": BEHAVIOR_LABELS[folder],
                        "quality": "weak_prototype",
                        "notes": (
                            "无框 SD 原图；bbox 从同名红框副本恢复。"
                            "分类 ROI 是婴儿/场景框，不是最终人工头部框。"
                        ),
                    }
                )
                counts[f"selected/{split}/{folder}"] += 1
                if OCCLUSION_LABELS[folder]:
                    counts[f"occlusion/{split}/{OCCLUSION_LABELS[folder]}"] += 1
                counts[f"behavior/{split}/{BEHAVIOR_LABELS[folder]}"] += 1

    annotation_paths = {
        split: str(
            write_coco(
                args.output,
                split,
                split_images[split],
                split_annotations[split],
            )
        )
        for split in ("train", "val", "test")
    }
    roi_csv = args.output / "frame_annotations.csv"
    fieldnames = [
        "image_path",
        "session_id",
        "doll_id",
        "real_infant_id",
        "source_video_id",
        "split",
        "include",
        "head_x",
        "head_y",
        "head_w",
        "head_h",
        "infant_x",
        "infant_y",
        "infant_w",
        "infant_h",
        "occlusion_label",
        "behavior_label",
        "quality",
        "notes",
    ]
    with roi_csv.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(roi_rows)

    report = {
        "archive": str(args.archive.resolve()),
        "raw_root": str(args.raw_root.resolve()),
        "output": str(args.output.resolve()),
        "boxed_unique_images": len(boxed_by_key),
        "raw_images": len(raw_by_key),
        "selected_images": len(roi_rows),
        "sampling": {
            "stride": args.stride,
            "per_folder_stride_override": {"baby_arms": 1},
            "guard_frames": args.guard_frames,
            "policy": "guarded chronological ranges within each source folder",
        },
        "counts": dict(sorted(counts.items())),
        "skipped": dict(sorted(skipped.items())),
        "dhash_distance_histogram": {
            str(distance): count for distance, count in sorted(dhash_distances.items())
        },
        "max_accepted_dhash_distance": args.max_dhash_distance,
        "mismatch_samples": mismatch_samples,
        "raw_red_rectangle_detector_hits": len(raw_rectangle_samples),
        "raw_red_rectangle_detector_hit_samples": raw_rectangle_samples,
        "coco_annotations": annotation_paths,
        "roi_csv": str(roi_csv),
        "prototype_capabilities": {
            "scene": ["infant", "empty-frame negative"],
            "occlusion": ["clear", "covered", "not_visible"],
            "behavior": ["quiet", "active", "edge_grab", "unknown"],
        },
        "known_limitations": [
            "所有划分来自同一批连续采集目录，不是独立场次，指标只能用于原型筛选。",
            "caregiver 没有样本，行为原型不训练 caregiver 类。",
            "covered/not_visible 是目录级弱标签，尚未逐帧复核；partial 因目录内语义混杂而主动排除。",
            "遮挡分类使用婴儿/场景框而非统一人工头部框。",
            "near_side 只能作为靠边代理样本，不能证明真实抓栏或攀爬能力。",
            "不能把这版原型结果直接接入高等级安全告警。",
        ],
    }
    report_path = args.output / "recovery_report.json"
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(json.dumps(report, ensure_ascii=True, indent=2))
    print(f"恢复报告: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

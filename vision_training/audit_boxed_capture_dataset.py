from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter, defaultdict
from io import BytesIO
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

import cv2
import numpy as np
from PIL import Image


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
ANNOTATION_SUFFIXES = {".json", ".xml", ".txt", ".csv", ".yaml", ".yml"}
COPY_MARKER = " - 副本"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="审计把红框直接画进图片的自采视觉 ZIP，不把像素框误当机器标注"
    )
    parser.add_argument("archive", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--sample-limit", type=int, default=20)
    return parser.parse_args()


def decode_image(data: bytes) -> np.ndarray:
    encoded = np.frombuffer(data, dtype=np.uint8)
    image = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError("OpenCV 无法解码图片")
    return image


def red_rectangle(image: np.ndarray) -> tuple[list[int] | None, float]:
    """Return the most likely hand-drawn red rectangle and perimeter support."""

    blue = image[:, :, 0].astype(np.int16)
    green = image[:, :, 1].astype(np.int16)
    red = image[:, :, 2].astype(np.int16)
    mask = (
        (red >= 170)
        & (green <= 125)
        & (blue <= 125)
        & ((red - green) >= 60)
        & ((red - blue) >= 60)
    ).astype(np.uint8)
    mask = cv2.morphologyEx(
        mask,
        cv2.MORPH_CLOSE,
        np.ones((3, 3), dtype=np.uint8),
    )

    contours, _ = cv2.findContours(mask, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
    image_height, image_width = mask.shape
    image_area = image_height * image_width
    best_box: list[int] | None = None
    best_support = 0.0
    best_score = 0.0

    for contour in contours:
        x, y, width, height = cv2.boundingRect(contour)
        box_area = width * height
        if width < 24 or height < 24:
            continue
        if box_area < image_area * 0.015 or box_area > image_area * 0.98:
            continue

        thickness = 3
        x2 = min(image_width, x + width)
        y2 = min(image_height, y + height)
        top = mask[y : min(y2, y + thickness), x:x2]
        bottom = mask[max(y, y2 - thickness) : y2, x:x2]
        left = mask[y:y2, x : min(x2, x + thickness)]
        right = mask[y:y2, max(x, x2 - thickness) : x2]
        supports = [
            float(top.mean()) if top.size else 0.0,
            float(bottom.mean()) if bottom.size else 0.0,
            float(left.mean()) if left.size else 0.0,
            float(right.mean()) if right.size else 0.0,
        ]
        support = sum(supports) / len(supports)
        score = support * (box_area / image_area) ** 0.5
        if support >= 0.30 and score > best_score:
            best_box = [int(x), int(y), int(width), int(height)]
            best_support = support
            best_score = score

    return best_box, best_support


def difference_hash(image: np.ndarray) -> int:
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    resized = cv2.resize(gray, (9, 8), interpolation=cv2.INTER_AREA)
    comparisons = resized[:, 1:] > resized[:, :-1]
    value = 0
    for bit in comparisons.reshape(-1):
        value = (value << 1) | int(bit)
    return value


def hamming_distance(left: int, right: int) -> int:
    return (left ^ right).bit_count()


def copy_original_name(path: PurePosixPath) -> str:
    return str(path.with_name(path.stem.replace(COPY_MARKER, "") + path.suffix))


def copy_mate_name(path: PurePosixPath) -> str:
    return str(path.with_name(path.stem + COPY_MARKER + path.suffix))


def main() -> int:
    args = parse_args()
    if not args.archive.is_file():
        raise SystemExit(f"找不到 ZIP: {args.archive}")

    report_path = args.report or args.archive.with_suffix(".audit.json")
    folder_reports: dict[str, object] = {}
    invalid_images: list[dict[str, str]] = []
    annotation_files: list[str] = []
    all_dimensions: Counter[str] = Counter()

    with ZipFile(args.archive) as archive:
        image_names = [
            info.filename
            for info in archive.infolist()
            if not info.is_dir()
            and PurePosixPath(info.filename).suffix.lower() in IMAGE_SUFFIXES
        ]
        annotation_files = [
            info.filename
            for info in archive.infolist()
            if not info.is_dir()
            and PurePosixPath(info.filename).suffix.lower() in ANNOTATION_SUFFIXES
        ]
        by_folder: dict[str, list[str]] = defaultdict(list)
        for name in image_names:
            parts = PurePosixPath(name).parts
            folder = parts[-2] if len(parts) >= 2 else "."
            by_folder[folder].append(name)

        for folder, names in sorted(by_folder.items()):
            name_set = set(names)
            exact_hashes: Counter[str] = Counter()
            rectangle_count = 0
            rectangle_samples: list[dict[str, object]] = []
            no_rectangle_samples: list[str] = []
            dimensions: Counter[str] = Counter()
            adjacent_near_duplicates = 0
            previous_hash: int | None = None

            copy_names = [
                name for name in names if COPY_MARKER in PurePosixPath(name).stem
            ]
            original_names = [
                name for name in names if COPY_MARKER not in PurePosixPath(name).stem
            ]
            byte_identical_pairs = 0
            paired_copies = 0
            for name in copy_names:
                original = copy_original_name(PurePosixPath(name))
                if original in name_set:
                    paired_copies += 1
                    if archive.read(name) == archive.read(original):
                        byte_identical_pairs += 1

            for name in sorted(original_names):
                data = archive.read(name)
                exact_hashes[hashlib.sha256(data).hexdigest()] += 1
                try:
                    image = decode_image(data)
                except ValueError as exc:
                    invalid_images.append({"image": name, "error": str(exc)})
                    continue
                height, width = image.shape[:2]
                dimensions[f"{width}x{height}"] += 1
                all_dimensions[f"{width}x{height}"] += 1
                box, support = red_rectangle(image)
                if box is not None:
                    rectangle_count += 1
                    if len(rectangle_samples) < args.sample_limit:
                        rectangle_samples.append(
                            {
                                "image": PurePosixPath(name).name,
                                "box_xywh": box,
                                "perimeter_support": round(support, 4),
                            }
                        )
                elif len(no_rectangle_samples) < args.sample_limit:
                    no_rectangle_samples.append(PurePosixPath(name).name)

                current_hash = difference_hash(image)
                if (
                    previous_hash is not None
                    and hamming_distance(previous_hash, current_hash) <= 2
                ):
                    adjacent_near_duplicates += 1
                previous_hash = current_hash

            exact_duplicate_images = sum(
                count - 1 for count in exact_hashes.values() if count > 1
            )
            usable_count = len(original_names) - len(
                [
                    item
                    for item in invalid_images
                    if PurePosixPath(item["image"]).parts[-2] == folder
                ]
            )
            folder_reports[folder] = {
                "archive_images": len(names),
                "unique_named_images_after_copy_removal": len(original_names),
                "copy_images": len(copy_names),
                "paired_copy_images": paired_copies,
                "byte_identical_copy_pairs": byte_identical_pairs,
                "decodable_images": usable_count,
                "dimensions": dict(sorted(dimensions.items())),
                "red_rectangle_images": rectangle_count,
                "red_rectangle_fraction": (
                    round(rectangle_count / usable_count, 4) if usable_count else None
                ),
                "exact_duplicate_images": exact_duplicate_images,
                "adjacent_near_duplicates_dhash_le_2": adjacent_near_duplicates,
                "rectangle_samples": rectangle_samples,
                "no_rectangle_samples": no_rectangle_samples,
            }

    positive_folders = [
        folder for folder in folder_reports if not folder.startswith("non_baby")
    ]
    positives = sum(
        int(folder_reports[folder]["decodable_images"]) for folder in positive_folders
    )
    positives_with_rectangle = sum(
        int(folder_reports[folder]["red_rectangle_images"]) for folder in positive_folders
    )

    report = {
        "archive": str(args.archive.resolve()),
        "archive_size_bytes": args.archive.stat().st_size,
        "images_in_archive": sum(
            int(item["archive_images"]) for item in folder_reports.values()
        ),
        "images_after_identical_named_copy_removal": sum(
            int(item["unique_named_images_after_copy_removal"])
            for item in folder_reports.values()
        ),
        "annotation_files": annotation_files,
        "machine_readable_annotation_count": len(annotation_files),
        "dimensions": dict(sorted(all_dimensions.items())),
        "folders": folder_reports,
        "invalid_images": invalid_images,
        "positive_red_rectangle_fraction": (
            round(positives_with_rectangle / positives, 4) if positives else None
        ),
        "blocking_findings": [
            "ZIP 内没有 COCO/YOLO/XML/CSV 坐标标注；红框已烧进 JPG 像素。",
            "带红框正样本不能直接训练检测器，否则模型会学习红框本身。",
            "没有 session_id/视频 ID，不能证明 train/val/test 按独立场次隔离。",
            "计划中的 caregiver、partial、not_visible、unknown 和真实 edge_grab/active 类别没有完整独立标注。",
            "covered 场景缺少被遮挡头部的统一 ROI 坐标，不能直接生成口鼻遮盖分类裁剪。",
        ],
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    # Keep stdout ASCII-safe because Windows SSH/cmd may still use a legacy code page.
    print(json.dumps(report, ensure_ascii=True, indent=2))
    print(f"审计报告: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import argparse
import json
import math
import shutil
from collections import Counter
from pathlib import Path

from PIL import Image


SPLITS = ("train", "val", "test")
SCENE_DIRS = {"train": "train2017", "val": "val2017", "test": "test2017"}
NEW_CATEGORY_MAP = {1: 1, 2: 3}  # baby -> infant, blanket -> blanket
PARTIAL_OCCLUSION_GROUPS = {
    "head_occluded_capture_02",
    "head_occluded_capture_05",
    "head_occluded_capture_07",
    "head_occluded_capture_08",
    "head_occluded_capture_09",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Merge grouped sleep/occlusion captures into the deployed five-class vision dataset"
    )
    parser.add_argument("--base-root", type=Path, required=True)
    parser.add_argument("--new-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument(
        "--legacy-train-repeat",
        type=int,
        default=1,
        help="Repeat only legacy ROI training samples; validation and test sets remain unchanged",
    )
    parser.add_argument(
        "--new-occluded-class",
        choices=("audited", "covered"),
        default="audited",
        help="Use visually audited partial/covered groups, or conservatively map every binary occluded label to covered",
    )
    return parser.parse_args()


def copy_tree(source: Path, destination: Path) -> None:
    if not source.is_dir():
        raise SystemExit(f"Missing source directory: {source}")
    shutil.copytree(source, destination, dirs_exist_ok=True)


def expanded_crop(image_path: Path, bbox: list[float], expansion: float = 1.25) -> Image.Image:
    image = Image.open(image_path).convert("RGB")
    x, y, width, height = bbox
    center_x = x + width * 0.5
    center_y = y + height * 0.5
    expanded_width = width * expansion
    expanded_height = height * expansion
    left = max(0, math.floor(center_x - expanded_width * 0.5))
    top = max(0, math.floor(center_y - expanded_height * 0.5))
    right = min(image.width, math.ceil(center_x + expanded_width * 0.5))
    bottom = min(image.height, math.ceil(center_y + expanded_height * 0.5))
    return image.crop((left, top, right, bottom))


def occlusion_class(state: str, visual_group: str) -> str:
    if state == "normal_sleep":
        return "clear"
    if state != "head_occluded":
        raise ValueError(f"Unknown sleep state: {state}")
    return "partial" if visual_group in PARTIAL_OCCLUSION_GROUPS else "covered"


def main() -> int:
    args = parse_args()
    base_root = args.base_root.resolve()
    new_root = args.new_root.resolve()
    output_root = args.output_root.resolve()
    if output_root.exists():
        raise SystemExit(f"Output already exists: {output_root}")

    base_scene = base_root / "scene_coco"
    base_roi = base_root / "roi_classification"
    output_scene = output_root / "scene_coco"
    output_roi = output_root / "roi_classification"
    copy_tree(base_scene, output_scene)
    copy_tree(base_roi, output_roi)
    if args.legacy_train_repeat < 1:
        raise SystemExit("--legacy-train-repeat must be at least 1")

    if args.legacy_train_repeat > 1:
        for task in ("occlusion", "behavior"):
            train_root = output_roi / task / "train"
            for class_root in sorted(path for path in train_root.iterdir() if path.is_dir()):
                legacy_files = sorted(path for path in class_root.iterdir() if path.is_file())
                for repeat_index in range(1, args.legacy_train_repeat):
                    for source in legacy_files:
                        destination = class_root / f"legacy_repeat{repeat_index}_{source.name}"
                        shutil.copy2(source, destination)

    new_coco_path = new_root / "annotations_coco.json"
    new_coco = json.loads(new_coco_path.read_text(encoding="utf-8"))
    new_images = {int(image["id"]): image for image in new_coco["images"]}
    annotations_by_image: dict[int, list[dict]] = {}
    for annotation in new_coco["annotations"]:
        annotations_by_image.setdefault(int(annotation["image_id"]), []).append(annotation)

    audit: dict[str, object] = {
        "source": str(new_root),
        "label_policy": {
            "normal_sleep": "clear",
            "head_occluded": (
                "partial or covered, assigned per visually audited capture group"
                if args.new_occluded_class == "audited"
                else "covered (source archive only provides a binary occluded label)"
            ),
            "partial_groups": sorted(PARTIAL_OCCLUSION_GROUPS),
            "behavior": "all new samples are lying",
            "legacy_train_repeat": args.legacy_train_repeat,
        },
        "splits": {},
    }

    groups_by_split: dict[str, set[str]] = {split: set() for split in SPLITS}
    for split in SPLITS:
        annotation_path = output_scene / "annotations" / f"instances_{split}2017.json"
        merged = json.loads(annotation_path.read_text(encoding="utf-8"))
        next_image_id = max((int(image["id"]) for image in merged["images"]), default=0) + 1
        next_annotation_id = max((int(item["id"]) for item in merged["annotations"]), default=0) + 1
        state_counts: Counter[str] = Counter()

        candidates = [image for image in new_images.values() if image.get("split") == split]
        for new_image in sorted(candidates, key=lambda item: item["file_name"]):
            source_name = Path(new_image["file_name"]).name
            source_image = new_root / "images" / split / source_name
            if not source_image.is_file():
                raise SystemExit(f"Missing new image: {source_image}")
            state = str(new_image["state_name"])
            group = str(new_image["visual_group"])
            groups_by_split[split].add(group)
            state_counts[state] += 1
            destination_name = f"sleep20260821_{source_name}"
            shutil.copy2(source_image, output_scene / SCENE_DIRS[split] / destination_name)

            merged_image = dict(new_image)
            merged_image.update({"id": next_image_id, "file_name": destination_name})
            merged["images"].append(merged_image)

            infant_bbox: list[float] | None = None
            for source_annotation in annotations_by_image.get(int(new_image["id"]), []):
                source_category = int(source_annotation["category_id"])
                if source_category not in NEW_CATEGORY_MAP:
                    continue
                merged_annotation = dict(source_annotation)
                merged_annotation.update(
                    {
                        "id": next_annotation_id,
                        "image_id": next_image_id,
                        "category_id": NEW_CATEGORY_MAP[source_category],
                    }
                )
                merged["annotations"].append(merged_annotation)
                next_annotation_id += 1
                if source_category == 1:
                    infant_bbox = [float(value) for value in source_annotation["bbox"]]

            if infant_bbox is None:
                raise SystemExit(f"No infant annotation for {source_image}")
            crop = expanded_crop(source_image, infant_bbox)
            crop_name = f"sleep20260821_{Path(destination_name).stem}.jpg"
            occlusion_label = (
                "covered"
                if state == "head_occluded" and args.new_occluded_class == "covered"
                else occlusion_class(state, group)
            )
            crop.save(output_roi / "occlusion" / split / occlusion_label / crop_name, quality=95)
            crop.save(output_roi / "behavior" / split / "lying" / crop_name, quality=95)
            next_image_id += 1

        annotation_path.write_text(json.dumps(merged, ensure_ascii=False), encoding="utf-8")
        audit["splits"][split] = {
            "new_images": len(candidates),
            "states": dict(state_counts),
            "visual_groups": sorted(groups_by_split[split]),
            "merged_scene_images": len(merged["images"]),
            "merged_scene_annotations": len(merged["annotations"]),
        }

    overlaps = {
        "train_val": sorted(groups_by_split["train"] & groups_by_split["val"]),
        "train_test": sorted(groups_by_split["train"] & groups_by_split["test"]),
        "val_test": sorted(groups_by_split["val"] & groups_by_split["test"]),
    }
    audit["group_overlaps"] = overlaps
    if any(overlaps.values()):
        raise SystemExit(f"Visual-group leakage detected: {overlaps}")

    (output_root / "incremental_dataset_audit.json").write_text(
        json.dumps(audit, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(audit, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

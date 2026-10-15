from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import random
import shutil
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent
DEFAULT_V2_ROOT = (
    ROOT.parent
    / "ml_training"
    / "data"
    / "2"
    / "baby_scene_yolo_dataset_v2_checked"
    / "baby_scene_yolo_dataset_v2_checked"
)
DEFAULT_SCENE_ROOT = ROOT / "data" / "processed" / "scene_coco"
DEFAULT_OUTPUT_ROOT = ROOT / "data" / "processed" / "vision_v2_checked"
DEFAULT_SYRIP_ROOT = ROOT / "data" / "raw" / "SyRIP" / "SyRIP"
DEFAULT_CLIP_PROBE = ROOT / "artifacts" / "v2_posture_clip_probe.csv"
DEFAULT_LEGACY_ROI = ROOT / "data" / "processed" / "prototype_v1_roi"

SCENE_CLASSES = ("infant", "head_face", "blanket", "soft_object", "caregiver")
OCCLUSION_CLASSES = ("clear", "partial", "covered", "not_visible")
POSTURE_CLASSES = ("lying", "sitting", "crawling_kneeling", "standing", "unknown")
SPLITS = ("train", "val", "test")
SPLIT_RATIOS = {"train": 0.70, "val": 0.15, "test": 0.15}

OCCLUSION_SCENES = {
    "01_normal_supine": "clear",
    "05_blanket_body": "clear",
    "06_mouth_partial": "partial",
    "07_face_covered": "covered",
    "08_head_not_visible": "not_visible",
}

# Scene 09 is a standing infant whose body is partly outside the serial-camera
# field of view. It is not a single-frame "edge grab" label.
POSTURE_SCENES = {
    "01_normal_supine": "lying",
    "02_left_side": "lying",
    "03_right_side": "lying",
    "04_prone": "lying",
    "05_blanket_body": "lying",
    "06_mouth_partial": "unknown",
    "07_face_covered": "unknown",
    "08_head_not_visible": "unknown",
    "09_near_bed_edge": "standing",
    "10_caregiver_holding": "unknown",
}


@dataclass
class V2Sample:
    index: int
    file_name: str
    scene: str
    original_path: str
    image_path: Path
    label_path: Path
    group_id: str = ""
    split: str = ""


class UnionFind:
    def __init__(self, size: int) -> None:
        self.parent = list(range(size))
        self.rank = [0] * size

    def find(self, value: int) -> int:
        while self.parent[value] != value:
            self.parent[value] = self.parent[self.parent[value]]
            value = self.parent[value]
        return value

    def union(self, left: int, right: int) -> None:
        left_root = self.find(left)
        right_root = self.find(right)
        if left_root == right_root:
            return
        if self.rank[left_root] < self.rank[right_root]:
            left_root, right_root = right_root, left_root
        self.parent[right_root] = left_root
        if self.rank[left_root] == self.rank[right_root]:
            self.rank[left_root] += 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Prepare leak-resistant V2 vision datasets")
    parser.add_argument("--v2-root", type=Path, default=DEFAULT_V2_ROOT)
    parser.add_argument("--scene-root", type=Path, default=DEFAULT_SCENE_ROOT)
    parser.add_argument("--syrip-root", type=Path, default=DEFAULT_SYRIP_ROOT)
    parser.add_argument("--legacy-roi", type=Path, default=DEFAULT_LEGACY_ROI)
    parser.add_argument("--clip-probe", type=Path, default=DEFAULT_CLIP_PROBE)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--seed", type=int, default=20260725)
    parser.add_argument("--dhash-distance", type=int, default=4)
    parser.add_argument("--unknown-cap", type=int, default=180)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def dhash(path: Path) -> int:
    with Image.open(path) as image:
        pixels = list(image.convert("L").resize((9, 8), Image.Resampling.BILINEAR).getdata())
    value = 0
    for row in range(8):
        for column in range(8):
            value = (value << 1) | int(pixels[row * 9 + column] > pixels[row * 9 + column + 1])
    return value


def load_v2_samples(root: Path) -> list[V2Sample]:
    metadata_path = root / "metadata.csv"
    if not metadata_path.is_file():
        raise SystemExit(f"Missing V2 metadata: {metadata_path}")
    image_lookup: dict[str, Path] = {}
    label_lookup: dict[str, Path] = {}
    for split in SPLITS:
        for path in (root / "yolo_dataset" / "images" / split).glob("*.jpg"):
            image_lookup[path.name] = path
        for path in (root / "yolo_dataset" / "labels" / split).glob("*.txt"):
            label_lookup[path.stem] = path
    samples: list[V2Sample] = []
    with metadata_path.open(newline="", encoding="utf-8-sig") as handle:
        for row in csv.DictReader(handle):
            image_path = image_lookup.get(row["file_name"])
            if image_path is None:
                raise SystemExit(f"Image listed by metadata is missing: {row['file_name']}")
            samples.append(
                V2Sample(
                    index=len(samples),
                    file_name=row["file_name"],
                    scene=row["scene"],
                    original_path=row.get("original_path", ""),
                    image_path=image_path,
                    label_path=label_lookup.get(image_path.stem, image_path.with_suffix(".txt")),
                )
            )
    return samples


def cluster_v2_samples(samples: list[V2Sample], max_distance: int) -> dict[str, list[V2Sample]]:
    union = UnionFind(len(samples))
    by_content: dict[str, int] = {}
    hashes: list[int] = []
    for sample in samples:
        content_hash = hashlib.sha256(sample.image_path.read_bytes()).hexdigest()
        if content_hash in by_content:
            union.union(sample.index, by_content[content_hash])
        else:
            by_content[content_hash] = sample.index
        hashes.append(dhash(sample.image_path))
    for left in range(len(samples)):
        for right in range(left + 1, len(samples)):
            if (
                samples[left].scene == samples[right].scene
                and (hashes[left] ^ hashes[right]).bit_count() <= max_distance
            ):
                union.union(left, right)
    groups: dict[int, list[V2Sample]] = defaultdict(list)
    for sample in samples:
        groups[union.find(sample.index)].append(sample)
    result: dict[str, list[V2Sample]] = {}
    for members in groups.values():
        digest = hashlib.sha1("\n".join(sorted(x.file_name for x in members)).encode()).hexdigest()[:12]
        group_id = f"v2_{digest}"
        for sample in members:
            sample.group_id = group_id
        result[group_id] = members
    return result


def assign_groups(groups: dict[str, list[V2Sample]], seed: int) -> None:
    randomizer = random.Random(seed)
    by_scene: dict[str, list[tuple[str, list[V2Sample]]]] = defaultdict(list)
    for group_id, members in groups.items():
        scene = Counter(sample.scene for sample in members).most_common(1)[0][0]
        by_scene[scene].append((group_id, members))
    for scene, scene_groups in sorted(by_scene.items()):
        randomizer.shuffle(scene_groups)
        scene_groups.sort(key=lambda item: len(item[1]), reverse=True)
        total = sum(len(members) for _, members in scene_groups)
        target = {split: total * SPLIT_RATIOS[split] for split in SPLITS}
        counts = {split: 0 for split in SPLITS}
        for _, members in scene_groups:
            split = min(SPLITS, key=lambda name: counts[name] / max(target[name], 1.0))
            counts[split] += len(members)
            for sample in members:
                sample.split = split


def parse_yolo(path: Path, width: int, height: int) -> list[tuple[int, list[float]]]:
    if not path.is_file():
        return []
    result: list[tuple[int, list[float]]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        fields = raw.strip().split()
        if len(fields) != 5:
            continue
        class_id = int(fields[0])
        center_x, center_y, box_width, box_height = map(float, fields[1:])
        x = max(0.0, (center_x - box_width / 2.0) * width)
        y = max(0.0, (center_y - box_height / 2.0) * height)
        w = min(width - x, box_width * width)
        h = min(height - y, box_height * height)
        if w > 1.0 and h > 1.0:
            result.append((class_id, [x, y, w, h]))
    return result


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def build_scene_dataset(samples: list[V2Sample], source_root: Path, output_root: Path) -> dict:
    destination = output_root / "scene_coco"
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(source_root, destination)
    category_by_name: dict[str, int] = {}
    split_counts: dict[str, Counter[str]] = {}
    for split in SPLITS:
        coco_split = "val" if split == "val" else split
        annotation_path = destination / "annotations" / f"instances_{coco_split}2017.json"
        document = json.loads(annotation_path.read_text(encoding="utf-8"))
        category_by_name = {item["name"]: int(item["id"]) for item in document["categories"]}
        next_image_id = max((int(item["id"]) for item in document["images"]), default=0) + 1
        next_annotation_id = max((int(item["id"]) for item in document["annotations"]), default=0) + 1
        counts: Counter[str] = Counter()
        for sample in (item for item in samples if item.split == split):
            with Image.open(sample.image_path) as image:
                width, height = image.size
            file_name = f"v2_{sample.file_name}"
            copy_file(sample.image_path, destination / f"{coco_split}2017" / file_name)
            document["images"].append(
                {
                    "id": next_image_id,
                    "file_name": file_name,
                    "width": width,
                    "height": height,
                    "source": "baby_scene_yolo_dataset_v2_checked",
                    "source_image_id": sample.file_name,
                    "group_id": sample.group_id,
                    "scene": sample.scene,
                }
            )
            for source_class, bbox in parse_yolo(sample.label_path, width, height):
                if source_class not in (0, 1):
                    continue
                category = "infant" if source_class == 0 else "blanket"
                document["annotations"].append(
                    {
                        "id": next_annotation_id,
                        "image_id": next_image_id,
                        "category_id": category_by_name[category],
                        "bbox": bbox,
                        "area": bbox[2] * bbox[3],
                        "iscrowd": 0,
                        "segmentation": [],
                    }
                )
                next_annotation_id += 1
                counts[category] += 1
            next_image_id += 1
            counts["images"] += 1
        annotation_path.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
        split_counts[split] = counts
    return {split: dict(counts) for split, counts in split_counts.items()}


def expanded_infant_crop(sample: V2Sample, margin: float = 0.10) -> Image.Image:
    image = Image.open(sample.image_path).convert("RGB")
    boxes = [bbox for class_id, bbox in parse_yolo(sample.label_path, *image.size) if class_id == 0]
    if not boxes:
        return image
    x, y, width, height = max(boxes, key=lambda box: box[2] * box[3])
    pad = max(width, height) * margin
    left = max(0, int(math.floor(x - pad)))
    top = max(0, int(math.floor(y - pad)))
    right = min(image.width, int(math.ceil(x + width + pad)))
    bottom = min(image.height, int(math.ceil(y + height + pad)))
    return image.crop((left, top, right, bottom))


def save_roi(image: Image.Image, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    image.resize((192, 192), Image.Resampling.LANCZOS).save(destination, "JPEG", quality=94)


def load_clip_probe(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise SystemExit(
            f"Missing CLIP posture audit: {path}. Generate it with the documented probe command first."
        )
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def clip_score(row: dict[str, str], label: str) -> float:
    scores = dict(item.split(":", 1) for item in row["scores"].split(";"))
    return float(scores[label])


def load_syrip_group_map(root: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in (
        root / "annotations" / "200R" / "person_keypoints_train_infant.json",
        root / "annotations" / "validate500" / "person_keypoints_validate_infant.json",
    ):
        document = json.loads(path.read_text(encoding="utf-8"))
        for image in document["images"]:
            source = str(image.get("image_source") or "").strip()
            result[image["file_name"]] = source or image["file_name"]
    return result


def assign_external_splits(
    rows: list[dict[str, str]], label: str, seed: int, group_map: dict[str, str]
) -> list[tuple[dict[str, str], str]]:
    randomizer = random.Random(seed ^ int(hashlib.sha1(label.encode()).hexdigest()[:8], 16))
    groups: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        groups[group_map.get(Path(row["file"]).name, Path(row["file"]).name)].append(row)
    shuffled_groups = list(groups.values())
    randomizer.shuffle(shuffled_groups)
    shuffled_groups.sort(key=len, reverse=True)
    target = {split: len(rows) * SPLIT_RATIOS[split] for split in SPLITS}
    counts = {split: 0 for split in SPLITS}
    result: list[tuple[dict[str, str], str]] = []
    for group in shuffled_groups:
        split = min(SPLITS, key=lambda name: counts[name] / max(target[name], 1.0))
        counts[split] += len(group)
        result.extend((row, split) for row in group)
    return result


def build_roi_datasets(
    samples: list[V2Sample],
    clip_rows: list[dict[str, str]],
    syrip_root: Path,
    legacy_roi: Path,
    output_root: Path,
    seed: int,
    unknown_cap: int,
) -> dict:
    roi_root = output_root / "roi_classification"
    if roi_root.exists():
        shutil.rmtree(roi_root)
    counts: dict[str, dict[str, Counter[str]]] = {
        "occlusion": {split: Counter() for split in SPLITS},
        "behavior": {split: Counter() for split in SPLITS},
    }
    for sample in samples:
        occlusion = OCCLUSION_SCENES.get(sample.scene)
        if occlusion:
            destination = roi_root / "occlusion" / sample.split / occlusion / f"v2_{sample.file_name}"
            save_roi(expanded_infant_crop(sample), destination)
            counts["occlusion"][sample.split][occlusion] += 1

    for split in SPLITS:
        for label in ("clear", "covered", "not_visible"):
            for index, source in enumerate(sorted((legacy_roi / "occlusion" / split / label).glob("*.jpg"))):
                destination = (
                    roi_root
                    / "occlusion"
                    / split
                    / label
                    / f"legacy_{label}_{index:04d}.jpg"
                )
                copy_file(source, destination)
                counts["occlusion"][split][label] += 1

    unknown_samples = [sample for sample in samples if POSTURE_SCENES.get(sample.scene) == "unknown"]
    random.Random(seed).shuffle(unknown_samples)
    unknown_keep = {sample.index for sample in unknown_samples[:unknown_cap]}
    for sample in samples:
        posture = POSTURE_SCENES.get(sample.scene)
        if not posture or (posture == "unknown" and sample.index not in unknown_keep):
            continue
        destination = roi_root / "behavior" / sample.split / posture / f"v2_{sample.file_name}"
        save_roi(expanded_infant_crop(sample), destination)
        counts["behavior"][sample.split][posture] += 1

    # CLIP is used only to select clearly visible real-world posture examples.
    # Device-domain scene 09 remains the authoritative standing class.
    quotas = {"standing": 15, "sitting": 55, "crawling_kneeling": 100}
    syrip_group_map = load_syrip_group_map(syrip_root)
    used_files: set[str] = set()
    for posture in ("standing", "sitting", "crawling_kneeling"):
        ranked = sorted(clip_rows, key=lambda row: clip_score(row, posture), reverse=True)
        selected = []
        for row in ranked:
            if row["file"] in used_files:
                continue
            selected.append(row)
            used_files.add(row["file"])
            if len(selected) >= quotas[posture]:
                break
        for index, (row, split) in enumerate(
            assign_external_splits(selected, posture, seed, syrip_group_map)
        ):
            source = Path(row["file"])
            box = tuple(int(value) for value in row["bbox"].split(","))
            image = Image.open(source).convert("RGB").crop(box)
            destination = roi_root / "behavior" / split / posture / f"syrip_{posture}_{index:04d}.jpg"
            save_roi(image, destination)
            counts["behavior"][split][posture] += 1

    for task, classes in (("occlusion", OCCLUSION_CLASSES), ("behavior", POSTURE_CLASSES)):
        for split in SPLITS:
            for label in classes:
                (roi_root / task / split / label).mkdir(parents=True, exist_ok=True)
    return {
        task: {split: dict(counter) for split, counter in split_counts.items()}
        for task, split_counts in counts.items()
    }


def create_contact_sheets(output_root: Path) -> None:
    roi_root = output_root / "roi_classification"
    audit_root = output_root / "audit"
    audit_root.mkdir(parents=True, exist_ok=True)
    for task, classes in (("occlusion", OCCLUSION_CLASSES), ("behavior", POSTURE_CLASSES)):
        for label in classes:
            paths = sorted((roi_root / task / "train" / label).glob("*.jpg"))[:48]
            if not paths:
                continue
            columns = 8
            rows = math.ceil(len(paths) / columns)
            sheet = Image.new("RGB", (columns * 128, rows * 146), "white")
            draw = ImageDraw.Draw(sheet)
            for index, path in enumerate(paths):
                image = Image.open(path).convert("RGB").resize((128, 128), Image.Resampling.BILINEAR)
                x = (index % columns) * 128
                y = (index // columns) * 146
                sheet.paste(image, (x, y))
                draw.text((x + 2, y + 130), path.stem[:19], fill="black")
            sheet.save(audit_root / f"{task}_{label}.jpg", quality=90)


def main() -> int:
    args = parse_args()
    output_root = args.output_root.resolve()
    if output_root.exists() and not args.force:
        raise SystemExit(f"Output exists; pass --force to rebuild: {output_root}")
    output_root.mkdir(parents=True, exist_ok=True)
    samples = load_v2_samples(args.v2_root.resolve())
    groups = cluster_v2_samples(samples, args.dhash_distance)
    assign_groups(groups, args.seed)
    scene_counts = build_scene_dataset(samples, args.scene_root.resolve(), output_root)
    clip_rows = load_clip_probe(args.clip_probe.resolve())
    roi_counts = build_roi_datasets(
        samples,
        clip_rows,
        args.syrip_root.resolve(),
        args.legacy_roi.resolve(),
        output_root,
        args.seed,
        args.unknown_cap,
    )
    create_contact_sheets(output_root)

    split_counts = Counter(sample.split for sample in samples)
    scene_split_counts: dict[str, Counter[str]] = defaultdict(Counter)
    for sample in samples:
        scene_split_counts[sample.scene][sample.split] += 1
    audit = {
        "source": str(args.v2_root.resolve()),
        "seed": args.seed,
        "dhash_distance": args.dhash_distance,
        "samples": len(samples),
        "duplicate_groups": len(groups),
        "split_counts": dict(split_counts),
        "scene_split_counts": {
            scene: dict(counter) for scene, counter in sorted(scene_split_counts.items())
        },
        "scene_dataset_added": scene_counts,
        "roi_counts": roi_counts,
        "posture_semantics": {
            "09_near_bed_edge": "standing",
            "edge_grab": "fusion-only event; not a single-frame class",
        },
        "classes": {
            "scene": list(SCENE_CLASSES),
            "occlusion": list(OCCLUSION_CLASSES),
            "behavior_field_semantics": list(POSTURE_CLASSES),
        },
    }
    (output_root / "dataset_audit.json").write_text(
        json.dumps(audit, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(audit, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

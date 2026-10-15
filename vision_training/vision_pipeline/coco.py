from __future__ import annotations

import json
import re
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

from .image_utils import IMAGE_SUFFIXES, clip_bbox
from .splits import canonical_group_id


TARGET_CATEGORIES = ("infant", "head_face", "blanket", "soft_object", "caregiver")
TARGET_CATEGORY_IDS = {name: index + 1 for index, name in enumerate(TARGET_CATEGORIES)}

_ALIASES = {
    "infant": "infant",
    "baby": "infant",
    "babies": "infant",
    "child": "infant",
    "body": "infant",
    "person infant": "infant",
    "head": "head_face",
    "face": "head_face",
    "head face": "head_face",
    "baby head": "head_face",
    "infant head": "head_face",
    "blanket": "blanket",
    "blankets": "blanket",
    "quilt": "blanket",
    "comforter": "blanket",
    "duvet": "blanket",
    "sheet": "blanket",
    "toy": "soft_object",
    "toys": "soft_object",
    "soft object": "soft_object",
    "soft toy": "soft_object",
    "stuffed animal": "soft_object",
    "plush toy": "soft_object",
    "pillow": "soft_object",
    "caregiver": "caregiver",
    "adult": "caregiver",
    "parent": "caregiver",
    "human hand": "caregiver",
    "caregiver hand": "caregiver",
}


@dataclass
class SceneObject:
    category: str
    bbox: list[float]
    source_category: str
    attributes: dict[str, Any] = field(default_factory=dict)


@dataclass
class SceneImage:
    source: str
    split: str
    image_path: Path
    width: int
    height: int
    source_image_id: str
    objects: list[SceneObject]
    group_id: str = ""


@dataclass
class CocoAudit:
    source: str
    json_files: list[str] = field(default_factory=list)
    images_seen: int = 0
    images_kept: int = 0
    annotations_seen: int = 0
    annotations_kept: int = 0
    derived_head_boxes: int = 0
    missing_images: list[str] = field(default_factory=list)
    skipped_categories: Counter[str] = field(default_factory=Counter)
    mapped_categories: Counter[str] = field(default_factory=Counter)
    invalid_boxes: int = 0

    def as_dict(self) -> dict[str, Any]:
        return {
            "source": self.source,
            "json_files": self.json_files,
            "images_seen": self.images_seen,
            "images_kept": self.images_kept,
            "annotations_seen": self.annotations_seen,
            "annotations_kept": self.annotations_kept,
            "derived_head_boxes": self.derived_head_boxes,
            "missing_images_count": len(self.missing_images),
            "missing_images_sample": self.missing_images[:30],
            "skipped_categories": dict(sorted(self.skipped_categories.items())),
            "mapped_categories": dict(sorted(self.mapped_categories.items())),
            "invalid_boxes": self.invalid_boxes,
        }


def normalize_category_name(name: str) -> str:
    value = re.sub(r"[_\-/]+", " ", str(name).strip().lower())
    return re.sub(r"\s+", " ", value)


def map_category(name: str, source: str) -> str | None:
    normalized = normalize_category_name(name)
    source_normalized = source.strip().lower()
    if source_normalized.startswith("syrip") and normalized == "person":
        return "infant"
    if source_normalized.startswith("self") and normalized == "person":
        return "caregiver"
    return _ALIASES.get(normalized)


def infer_split(path: Path) -> str:
    tokens = [normalize_category_name(part) for part in path.parts]
    for token in reversed(tokens):
        if token in {"test", "testing", "test2017"} or token.startswith("test"):
            return "test"
        if (
            token in {"valid", "validation", "validate", "val", "val2017", "validate infant"}
            or token.startswith("validate")
            or token.startswith("validation")
            or token.startswith("val ")
        ):
            return "val"
        if token in {"train", "training", "train2017", "train infant"} or token.startswith("train"):
            return "train"
    return "train"


def discover_coco_jsons(root: Path) -> list[Path]:
    candidates: list[Path] = []
    for path in root.rglob("*.json"):
        try:
            with path.open("r", encoding="utf-8-sig") as handle:
                document = json.load(handle)
        except (OSError, UnicodeError, json.JSONDecodeError):
            continue
        if (
            isinstance(document, dict)
            and isinstance(document.get("images"), list)
            and (
                isinstance(document.get("annotations"), list)
                or isinstance(document.get("categories"), list)
            )
        ):
            candidates.append(path)
    return sorted(candidates)


def select_syrip_jsons(root: Path) -> list[Path]:
    """Pick non-overlapping official SyRIP annotation variants."""
    paths = discover_coco_jsons(root)
    train_paths = [path for path in paths if "train" in path.name.lower()]
    validation_paths = [
        path for path in paths if any(token in str(path).lower() for token in ("validate", "validation"))
    ]

    def choose(paths_to_score: list[Path], priorities: Sequence[str]) -> Path | None:
        if not paths_to_score:
            return None
        return max(
            paths_to_score,
            key=lambda path: tuple(int(token.lower() in str(path).lower()) for token in priorities),
        )

    selected: list[Path] = []
    train = choose(train_paths, ("200r_1000s", "expanded", "train_infant"))
    validation = choose(validation_paths, ("validate500", "expanded", "validate_infant"))
    if train is not None:
        selected.append(train)
    if validation is not None and validation != train:
        selected.append(validation)
    return selected or paths


def build_image_index(root: Path) -> tuple[dict[str, Path], dict[str, list[Path]]]:
    by_relative: dict[str, Path] = {}
    by_name: dict[str, list[Path]] = defaultdict(list)
    for path in root.rglob("*"):
        if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES:
            relative = path.relative_to(root).as_posix().lower()
            by_relative[relative] = path
            by_name[path.name.lower()].append(path)
    return by_relative, by_name


def resolve_image_path(
    root: Path,
    json_path: Path,
    file_name: str,
    by_relative: dict[str, Path],
    by_name: dict[str, list[Path]],
) -> Path | None:
    normalized = str(file_name).replace("\\", "/").lstrip("./")
    direct_candidates = (
        json_path.parent / normalized,
        json_path.parent / "images" / normalized,
        root / normalized,
        root / "images" / normalized,
    )
    for candidate in direct_candidates:
        try:
            if candidate.is_file():
                return candidate
        except OSError:
            # Some CribHD annotation file names exceed the component/path limit
            # of macOS network mounts.  The indexed basename/relative-path
            # lookup below may still resolve the real extracted image on
            # Windows, so an inaccessible speculative path is not fatal.
            continue

    relative_key = normalized.lower()
    if relative_key in by_relative:
        return by_relative[relative_key]
    matches = by_name.get(Path(normalized).name.lower(), [])
    if len(matches) == 1:
        return matches[0]
    if matches:
        split = infer_split(json_path)
        split_matches = [path for path in matches if infer_split(path) == split]
        if len(split_matches) == 1:
            return split_matches[0]
    return None


def _head_bbox_from_keypoints(
    annotation: dict[str, Any],
    width: int,
    height: int,
) -> list[float] | None:
    explicit = annotation.get("bbox_head")
    if isinstance(explicit, list) and len(explicit) == 4:
        clipped = clip_bbox(explicit, width, height, min_size=8.0)
        if clipped is not None:
            return clipped

    keypoints = annotation.get("keypoints")
    if not isinstance(keypoints, list) or len(keypoints) < 15:
        return None
    visible: list[tuple[float, float]] = []
    for index in range(5):
        x, y, visibility = keypoints[index * 3 : index * 3 + 3]
        if float(visibility) > 0 and float(x) >= 0 and float(y) >= 0:
            visible.append((float(x), float(y)))
    if len(visible) < 2:
        return None

    infant_bbox = clip_bbox(annotation.get("bbox", []), width, height)
    if infant_bbox is None:
        return None
    bx, by, bw, bh = infant_bbox
    xs = [point[0] for point in visible]
    ys = [point[1] for point in visible]
    center_x = sum(xs) / len(xs)
    center_y = sum(ys) / len(ys)
    point_w = max(xs) - min(xs)
    point_h = max(ys) - min(ys)
    head_w = max(16.0, point_w * 1.9, bw * 0.22)
    head_h = max(16.0, point_h * 2.2, bh * 0.20)
    center_y += head_h * 0.06
    return clip_bbox(
        [center_x - head_w / 2.0, center_y - head_h / 2.0, head_w, head_h],
        width,
        height,
        min_size=8.0,
    )


def load_coco_sources(
    root: Path,
    *,
    source: str,
    json_paths: Iterable[Path] | None = None,
    derive_syrip_heads: bool = True,
    keep_empty: bool = False,
) -> tuple[list[SceneImage], CocoAudit]:
    root = root.resolve()
    audit = CocoAudit(source=source)
    by_relative, by_name = build_image_index(root)
    selected_jsons = list(json_paths) if json_paths is not None else discover_coco_jsons(root)
    audit.json_files = [str(path) for path in selected_jsons]
    records_by_key: dict[tuple[str, str], SceneImage] = {}

    for json_path in selected_jsons:
        with json_path.open("r", encoding="utf-8-sig") as handle:
            coco = json.load(handle)
        categories = {
            int(category["id"]): str(category.get("name", category["id"]))
            for category in coco.get("categories", [])
        }
        annotations_by_image: dict[int, list[dict[str, Any]]] = defaultdict(list)
        for annotation in coco.get("annotations", []):
            annotations_by_image[int(annotation["image_id"])].append(annotation)

        split = infer_split(json_path)
        for image in coco.get("images", []):
            audit.images_seen += 1
            image_id = int(image["id"])
            file_name = str(image.get("file_name", ""))
            image_path = resolve_image_path(root, json_path, file_name, by_relative, by_name)
            if image_path is None:
                audit.missing_images.append(file_name)
                continue
            width = int(image.get("width", 0))
            height = int(image.get("height", 0))
            if width <= 0 or height <= 0:
                from .image_utils import image_size

                width, height = image_size(image_path)

            objects: list[SceneObject] = []
            for annotation in annotations_by_image.get(image_id, []):
                audit.annotations_seen += 1
                source_category = categories.get(int(annotation.get("category_id", -1)), "unknown")
                target_category = map_category(source_category, source)
                if target_category is None:
                    audit.skipped_categories[normalize_category_name(source_category)] += 1
                    continue
                bbox = clip_bbox(annotation.get("bbox", []), width, height)
                if bbox is None:
                    audit.invalid_boxes += 1
                    continue
                objects.append(
                    SceneObject(
                        category=target_category,
                        bbox=bbox,
                        source_category=source_category,
                    )
                )
                audit.annotations_kept += 1
                audit.mapped_categories[target_category] += 1

                if source.lower().startswith("syrip") and derive_syrip_heads:
                    head_bbox = _head_bbox_from_keypoints(annotation, width, height)
                    if head_bbox is not None:
                        objects.append(
                            SceneObject(
                                category="head_face",
                                bbox=head_bbox,
                                source_category="derived_from_syrip_keypoints",
                                attributes={"weak_annotation": True},
                            )
                        )
                        audit.annotations_kept += 1
                        audit.derived_head_boxes += 1
                        audit.mapped_categories["head_face"] += 1

            if not objects and not keep_empty:
                continue
            key = (source, str(image_path.resolve()).lower())
            if key in records_by_key:
                existing = records_by_key[key]
                object_keys = {
                    (item.category, *(round(value, 2) for value in item.bbox))
                    for item in existing.objects
                }
                for item in objects:
                    object_key = (item.category, *(round(value, 2) for value in item.bbox))
                    if object_key not in object_keys:
                        existing.objects.append(item)
                continue

            records_by_key[key] = SceneImage(
                source=source,
                split=split,
                image_path=image_path,
                width=width,
                height=height,
                source_image_id=str(image_id),
                objects=objects,
                group_id=canonical_group_id(
                    str(
                        image.get("session_id")
                        or image.get("source_video_id")
                        or image.get("image_source")
                        or (
                            _self_capture_session(root, json_path)
                            if source.lower().startswith("self")
                            else ""
                        )
                        or file_name
                    )
                ),
            )

    records = list(records_by_key.values())
    audit.images_kept = len(records)
    return records, audit


def _self_capture_session(root: Path, json_path: Path) -> str:
    """Infer `<split>/<session_id>` from the documented self-capture layout."""
    try:
        parts = json_path.resolve().relative_to(root).parts
    except ValueError:
        parts = json_path.parts
    for index, part in enumerate(parts):
        token = normalize_category_name(part)
        is_split_directory = (
            token in {
                "train",
                "training",
                "train2017",
                "val",
                "valid",
                "validation",
                "validate",
                "val2017",
                "test",
                "testing",
                "test2017",
            }
            or token.startswith(("train ", "val ", "valid ", "test "))
        )
        if is_split_directory and index + 1 < len(parts):
            candidate = parts[index + 1]
            if normalize_category_name(candidate) not in {"annotations", "images"}:
                return f"session:{candidate}"
    return ""

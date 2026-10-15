from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from capture_session import save_if_distinct
from convert_yolox_int8 import sha256_file
from generate_scene_embedded_support import (
    SCENE_CLASSES,
    SceneTensorContract,
    c_float,
    render_labels_header,
    render_model_metadata,
)
from vision_pipeline.coco import (
    SceneImage,
    TARGET_CATEGORIES,
    _self_capture_session,
    discover_coco_jsons,
    infer_split,
    load_coco_sources,
    map_category,
    resolve_image_path,
    select_syrip_jsons,
)
from vision_pipeline.image_utils import clip_bbox, difference_hash, hamming_distance
from vision_pipeline.splits import (
    assert_group_isolation,
    canonical_group_id,
    stable_group_split,
)
from prepare_scene_dataset import enforce_group_isolation
from recover_boxed_capture_dataset import split_for_index
from train_mobilenetv3 import metrics_from_confusion


def write_image(path: Path, color=(80, 120, 160)) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.new("RGB", (320, 240), color).save(path)


def write_coco(path: Path, document: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document), encoding="utf-8")


def test_target_category_order_is_deployment_contract() -> None:
    assert TARGET_CATEGORIES == (
        "infant",
        "head_face",
        "blanket",
        "soft_object",
        "caregiver",
    )


def test_sha256_file_is_stable(tmp_path: Path) -> None:
    artifact = tmp_path / "model.onnx"
    artifact.write_bytes(b"source model must not be mutated")
    assert (
        sha256_file(artifact)
        == "e9ff4165bb4bfa4699745d138301793511f3a393f8e760033baa5d0fd9c8098c"
    )


@pytest.mark.parametrize(
    ("name", "source", "expected"),
    [
        ("Blanket", "cribhd", "blanket"),
        ("stuffed_animal", "cribhd", "soft_object"),
        ("person", "syrip", "infant"),
        ("person", "self_capture", "caregiver"),
        ("unknown-object", "cribhd", None),
    ],
)
def test_category_mapping(name: str, source: str, expected: str | None) -> None:
    assert map_category(name, source) == expected


def test_split_inference_handles_syrip_names() -> None:
    assert infer_split(Path("annotations/validate500/person_keypoints_validate_infant.json")) == "val"
    assert infer_split(Path("annotations/200R_1000S/person_keypoints_train_infant.json")) == "train"
    assert infer_split(Path("CribHD-T/test/_annotations.coco.json")) == "test"


def test_coco_discovery_does_not_require_annotations_in_first_4k(tmp_path: Path) -> None:
    annotation_path = tmp_path / "person_keypoints_train_infant.json"
    write_coco(
        annotation_path,
        {
            "images": [
                {"id": index, "file_name": f"{'long_name_' * 20}{index}.jpg"}
                for index in range(100)
            ],
            "annotations": [],
            "categories": [{"id": 1, "name": "person"}],
        },
    )
    assert annotation_path.stat().st_size > 4096
    assert discover_coco_jsons(tmp_path) == [annotation_path]


def test_load_cribhd_maps_objects_and_skips_unknown(tmp_path: Path) -> None:
    root = tmp_path / "CribHD"
    image_path = root / "CribHD-T" / "train" / "images" / "frame.jpg"
    write_image(image_path)
    annotation_path = root / "CribHD-T" / "train" / "_annotations.coco.json"
    write_coco(
        annotation_path,
        {
            "images": [{"id": 1, "file_name": "frame.jpg", "width": 320, "height": 240}],
            "categories": [
                {"id": 1, "name": "toy"},
                {"id": 2, "name": "blanket"},
                {"id": 3, "name": "hazard"},
            ],
            "annotations": [
                {"id": 1, "image_id": 1, "category_id": 1, "bbox": [10, 20, 40, 50]},
                {"id": 2, "image_id": 1, "category_id": 2, "bbox": [60, 70, 80, 90]},
                {"id": 3, "image_id": 1, "category_id": 3, "bbox": [1, 1, 10, 10]},
            ],
        },
    )
    records, audit = load_coco_sources(root, source="cribhd")
    assert len(records) == 1
    assert {obj.category for obj in records[0].objects} == {"blanket", "soft_object"}
    assert audit.skipped_categories["hazard"] == 1
    assert records[0].split == "train"


def test_syrip_derives_head_box_and_selects_combined_annotations(tmp_path: Path) -> None:
    root = tmp_path / "SyRIP"
    image_path = root / "images" / "train_infant" / "train00001.jpg"
    write_image(image_path)
    base_document = {
        "images": [
            {
                "id": 1,
                "file_name": "train00001.jpg",
                "width": 320,
                "height": 240,
                "image_source": "video_001",
            }
        ],
        "categories": [{"id": 1, "name": "person"}],
        "annotations": [
            {
                "id": 1,
                "image_id": 1,
                "category_id": 1,
                "bbox": [50, 20, 200, 200],
                "keypoints": [
                    150,
                    55,
                    2,
                    140,
                    50,
                    2,
                    160,
                    50,
                    2,
                    130,
                    55,
                    2,
                    170,
                    55,
                    2,
                ]
                + [0, 0, 0] * 12,
            }
        ],
    }
    combined = root / "annotations" / "200R_1000S" / "person_keypoints_train_infant.json"
    write_coco(combined, base_document)
    write_coco(
        root / "annotations" / "200R" / "person_keypoints_train_infant.json",
        base_document,
    )
    selected = select_syrip_jsons(root)
    assert combined in selected
    records, audit = load_coco_sources(root, source="syrip", json_paths=selected)
    assert len(records) == 1
    assert {obj.category for obj in records[0].objects} == {"infant", "head_face"}
    assert audit.derived_head_boxes == 1
    head = next(obj for obj in records[0].objects if obj.category == "head_face")
    assert head.attributes["weak_annotation"] is True


def test_group_split_is_stable_and_isolation_detects_leakage() -> None:
    first = stable_group_split("session:day_a", seed=7)
    assert first == stable_group_split("session:day_a", seed=7)
    assert_group_isolation(
        [
            {"group_id": "a", "split": "train"},
            {"group_id": "a", "split": "train"},
            {"group_id": "b", "split": "test"},
        ]
    )
    with pytest.raises(ValueError, match="group leakage"):
        assert_group_isolation(
            [
                {"group_id": "a", "split": "train"},
                {"group_id": "a", "split": "val"},
            ]
        )


def test_prototype_chronological_split_has_guard_bands() -> None:
    assert split_for_index(10, 100, guard_frames=5) == "train"
    assert split_for_index(69, 100, guard_frames=5) is None
    assert split_for_index(75, 100, guard_frames=5) == "val"
    assert split_for_index(84, 100, guard_frames=5) is None
    assert split_for_index(90, 100, guard_frames=5) == "test"


def test_youtube_group_variants_are_canonicalized() -> None:
    assert (
        canonical_group_id("https://www.youtube.com/watch?v=abc_123&t=21s")
        == canonical_group_id("https://youtu.be/abc_123")
        == "youtube:abc_123"
    )


def test_self_capture_session_is_inferred_from_split_layout(tmp_path: Path) -> None:
    annotation = (
        tmp_path
        / "coco"
        / "train"
        / "night_set_a_01"
        / "annotations"
        / "instances_default.json"
    )
    assert _self_capture_session(tmp_path, annotation) == "session:night_set_a_01"


def test_public_overlap_moves_to_holdout_but_self_overlap_fails(tmp_path: Path) -> None:
    public_train = SceneImage(
        source="syrip",
        split="train",
        image_path=tmp_path / "a.jpg",
        width=10,
        height=10,
        source_image_id="1",
        objects=[],
        group_id="youtube:same",
    )
    public_val = SceneImage(
        source="syrip",
        split="val",
        image_path=tmp_path / "b.jpg",
        width=10,
        height=10,
        source_image_id="2",
        objects=[],
        group_id="youtube:same",
    )
    report = enforce_group_isolation([public_train, public_val])
    assert public_train.split == public_val.split == "val"
    assert report["conflict_groups_quarantined"] == 1

    self_train = SceneImage(
        source="self_capture",
        split="train",
        image_path=tmp_path / "c.jpg",
        width=10,
        height=10,
        source_image_id="3",
        objects=[],
        group_id="session:one",
    )
    self_test = SceneImage(
        source="self_capture",
        split="test",
        image_path=tmp_path / "d.jpg",
        width=10,
        height=10,
        source_image_id="4",
        objects=[],
        group_id="session:one",
    )
    with pytest.raises(ValueError, match="自采场次跨数据划分"):
        enforce_group_isolation([self_train, self_test])


def test_bbox_clipping_and_near_duplicate_hash() -> None:
    assert clip_bbox([-5, -5, 20, 20], 100, 100) == [0.0, 0.0, 15.0, 15.0]
    image = Image.new("RGB", (40, 40), (10, 20, 30))
    same = Image.new("RGB", (40, 40), (10, 20, 30))
    left = difference_hash(image)
    right = difference_hash(same)
    assert hamming_distance(left, right) == 0


def test_image_resolver_skips_overlong_speculative_path(tmp_path: Path) -> None:
    json_path = tmp_path / "annotations" / "train" / "labels.json"
    json_path.parent.mkdir(parents=True)
    overlong_name = f"{'x' * 300}.jpg"
    assert (
        resolve_image_path(
            tmp_path,
            json_path,
            overlong_name,
            by_relative={},
            by_name={},
        )
        is None
    )


def test_custom_scene_metadata_uses_five_class_tensor_contract() -> None:
    contract = SceneTensorContract(
        input_scale=1.0,
        input_zero_point=-128,
        output_scale=0.03125,
        output_zero_point=-17,
    )
    metadata = render_model_metadata(
        contract,
        score_threshold=0.25,
        nms_threshold=0.45,
        max_detections=100,
    )
    assert "#define MODEL_NUM_CLASSES       (5U)" in metadata
    assert "#define MODEL_OUTPUT_CHANNELS   (10U)" in metadata
    assert "#define MODEL_OUTPUT_SCALE      (0.03125f)" in metadata
    assert "#define MODEL_OUTPUT_ZERO_POINT (-17)" in metadata
    labels = render_labels_header()
    assert SCENE_CLASSES == (
        "infant",
        "head_face",
        "blanket",
        "soft_object",
        "caregiver",
    )
    for label in SCENE_CLASSES:
        assert f'"{label}"' in labels
    assert c_float(1.0) == "1.0f"


def test_capture_dedup_keeps_last_saved_hash_as_reference(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr("capture_session.difference_hash", lambda _image: 101)
    kept, reference_hash = save_if_distinct(
        Image.new("RGB", (8, 8)),
        tmp_path,
        index=1,
        previous_hash=100,
        maximum_distance=2,
    )
    assert kept is False
    assert reference_hash == 100
    assert not list(tmp_path.iterdir())


def test_classification_metrics_report_risk_class_recall() -> None:
    confusion = np.asarray([[8, 2], [1, 9]], dtype=np.int64)
    metrics = metrics_from_confusion(confusion, ["clear", "covered"])
    assert metrics["samples"] == 20
    assert metrics["accuracy"] == pytest.approx(0.85)
    assert metrics["per_class"]["covered"]["recall"] == pytest.approx(0.9)
    assert metrics["per_class"]["clear"]["precision"] == pytest.approx(8 / 9)

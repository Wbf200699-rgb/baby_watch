from __future__ import annotations

import numpy as np

from audio_pipeline.common import load_config
from audio_pipeline.dataset import _short_cry_mix, _simulate_device_capture
from audio_pipeline.decision import classify_cry_type, find_confirmed_cry, find_voted_cry
from audio_pipeline.features import extract_mfcc, extract_mfcc_batch
from audio_pipeline.manifest import (
    _assign_splits,
    _apply_frozen_public_partitions,
    _attach_split_groups,
    _donate_metadata,
    _reject_duplicate_content,
    _selected_detection_metadata,
    _selected_type_metadata,
    scan_device_selected,
    scan_infantcry_dbl,
    scan_ravdess_speech,
    scan_tut_rare_events,
)


def test_mfcc_contract_is_49_by_10() -> None:
    config = load_config()
    sample_rate = config["audio"]["sample_rate_hz"]
    time = np.arange(sample_rate, dtype=np.float32) / sample_rate
    waveform = 0.2 * np.sin(2.0 * np.pi * 440.0 * time)
    features = extract_mfcc(waveform, config)
    assert features.shape == (49, 10, 1)
    assert features.dtype == np.float32
    assert np.isfinite(features).all()


def test_batched_mfcc_matches_individual_windows() -> None:
    config = load_config()
    sample_rate = config["audio"]["sample_rate_hz"]
    time = np.arange(sample_rate, dtype=np.float32) / sample_rate
    windows = np.stack(
        [
            0.2 * np.sin(2.0 * np.pi * 440.0 * time),
            0.05 * np.sin(2.0 * np.pi * 880.0 * time),
        ]
    )
    batched = extract_mfcc_batch(windows, config)
    individual = np.stack([extract_mfcc(window, config) for window in windows])
    np.testing.assert_allclose(batched, individual, rtol=1.0e-5, atol=1.0e-5)


def test_donate_filename_keeps_app_instance_as_group() -> None:
    from pathlib import Path

    reason, instance, gender, age = _donate_metadata(
        Path("0D1AD73E-4C5E-45F3-85C4-9A3CB71E8856-1430742197-1.0-m-04-hu.caf")
    )
    assert reason == "hu"
    assert instance == "0d1ad73e-4c5e-45f3-85c4-9a3cb71e8856"
    assert gender == "m"
    assert age == "04"


def test_selected_device_naming_fixes_label_and_partition(tmp_path) -> None:
    cry = tmp_path / "1" / "cry" / "CTR_001.wav"
    quiet = tmp_path / "1" / "QT" / "NVA_QT_001.WAV"
    talk = tmp_path / "1" / "TK" / "NTE_TK_001.WAV"
    assert _selected_detection_metadata(cry, tmp_path) == ("cry", "train", "cry")
    assert _selected_detection_metadata(quiet, tmp_path) == (
        "non_cry",
        "validation",
        "quiet",
    )
    assert _selected_detection_metadata(talk, tmp_path) == ("non_cry", "test", "talk")


def test_selected_device_naming_rejects_label_mismatch(tmp_path) -> None:
    misplaced = tmp_path / "1" / "cry" / "NTR_001.wav"
    assert _selected_detection_metadata(misplaced, tmp_path) == ("", "", "")


def test_selected_device_rollovers_share_capture_session(tmp_path) -> None:
    first = tmp_path / "selected" / "1" / "cry" / "CTR_001.wav"
    second = tmp_path / "selected" / "1" / "cry" / "CTR_002.WAV"
    first.parent.mkdir(parents=True)
    first.touch()
    second.touch()
    config = {
        "datasets": {
            "device_selected": {
                "directory": "selected",
                "license": "private",
            }
        }
    }
    rows = scan_device_selected(config, tmp_path)
    assert len(rows) == 2
    assert rows[0]["group_id"] == rows[1]["group_id"]
    assert rows[0]["session_id"] == "batch_1_train_cry"


def test_selected_type_naming_is_fixed_to_training(tmp_path) -> None:
    hungry = tmp_path / "2" / "hungry" / "hungry_001.wav"
    discomfort = tmp_path / "2" / "discomfort" / "discomfort_011.wav"
    assert _selected_type_metadata(hungry, tmp_path) == ("hungry", "train")
    assert _selected_type_metadata(discomfort, tmp_path) == ("discomfort", "train")


def test_selected_type_naming_rejects_unknown_or_misplaced_file(tmp_path) -> None:
    unknown = tmp_path / "2" / "unknown" / "unknown_001.wav"
    misplaced = tmp_path / "2" / "tired" / "hungry_001.wav"
    assert _selected_type_metadata(unknown, tmp_path) == ("", "")
    assert _selected_type_metadata(misplaced, tmp_path) == ("", "")


def test_public_playback_index_freezes_original_split(tmp_path) -> None:
    index = tmp_path / "ready_to_play" / "public_cry_wav" / "index.csv"
    index.parent.mkdir(parents=True)
    index.write_text(
        "source,recording_id,split\n"
        "donateacry,recording-a,validation\n",
        encoding="utf-8",
    )
    rows = [
        {
            "source": "donateacry",
            "recording_id": "recording-a",
            "source_partition": "",
        }
    ]
    _apply_frozen_public_partitions(rows, tmp_path)
    assert rows[0]["source_partition"] == "validation"


def test_infantcry_dbl_uses_folder_label_and_tier_policy(tmp_path) -> None:
    root = tmp_path / "raw" / "infantcry_dbl_v1"
    tier1 = root / "Tier 1" / "neh" / "000001_F_neh.wav"
    tier2 = root / "Tier 2" / "owh" / "000002_M_owh.wav"
    tier1.parent.mkdir(parents=True)
    tier2.parent.mkdir(parents=True)
    tier1.touch()
    tier2.touch()
    (root / "metadata.csv").write_text(
        "filename,class,tier,clip_id,gender\n"
        "000001_F_neh.wav,neh,1,1,F\n"
        "000002_M_owh.wav,owh,2,2,M\n",
        encoding="utf-8",
    )
    config = {
        "datasets": {
            "infantcry_dbl": {
                "directory": "infantcry_dbl_v1",
                "license": "CC-BY-4.0",
            }
        }
    }
    rows = scan_infantcry_dbl(config, tmp_path)
    assert [row["classification_label"] for row in rows] == ["hungry", "tired"]
    assert rows[0]["source"] == "infantcry_dbl_tier1"
    assert rows[0]["source_partition"] == "train"
    assert rows[1]["source"] == "infantcry_dbl_tier2"
    assert rows[1]["source_partition"] == ""
    assert rows[0]["group_id"] != rows[1]["group_id"]


def test_tut_scanner_uses_only_annotated_segments_and_groups_source(tmp_path) -> None:
    root = tmp_path / "raw" / "tut_rare_events_2017"
    segments = root / "segments"
    segments.mkdir(parents=True)
    (segments / "107351_001.wav").touch()
    (segments / "107351_002.wav").touch()
    (root / "segments.csv").write_text(
        "segment_file,source_file,segment_index,start_seconds,end_seconds,license,source_url,username\n"
        "segments/107351_001.wav,107351.wav,1,0.18,2.74,CC-BY-3.0,https://example.test/a,user\n"
        "segments/107351_002.wav,107351.wav,2,4.0,4.5,CC-BY-3.0,https://example.test/a,user\n",
        encoding="utf-8",
    )
    config = {
        "datasets": {
            "tut_rare_events": {
                "directory": "tut_rare_events_2017",
                "license": "per-file",
            }
        }
    }
    rows = scan_tut_rare_events(config, tmp_path)
    assert len(rows) == 2
    assert {row["detection_label"] for row in rows} == {"cry"}
    assert {row["classification_label"] for row in rows} == {""}
    assert rows[0]["group_id"] == rows[1]["group_id"]


def test_ravdess_scanner_labels_adult_shouts_and_groups_by_actor(tmp_path) -> None:
    root = tmp_path / "raw" / "ravdess_speech_v1"
    actor_1 = root / "Actor_01"
    actor_2 = root / "Actor_21"
    actor_1.mkdir(parents=True)
    actor_2.mkdir(parents=True)
    (actor_1 / "03-01-05-02-01-01-01.wav").touch()
    (actor_1 / "03-01-06-02-02-02-01.wav").touch()
    (actor_2 / "03-01-01-01-01-01-21.wav").touch()
    (actor_2 / "not-ravdess.wav").touch()
    config = {
        "datasets": {
            "ravdess_speech": {
                "directory": "ravdess_speech_v1",
                "license": "CC-BY-NC-SA-4.0",
            }
        }
    }

    rows = scan_ravdess_speech(config, tmp_path)

    assert len(rows) == 3
    assert {row["detection_label"] for row in rows} == {"non_cry"}
    assert rows[0]["group_id"] == rows[1]["group_id"]
    assert rows[0]["group_id"] != rows[2]["group_id"]
    assert rows[0]["original_label"] == "adult_angry_strong"
    assert {row["source_partition"] for row in rows} == {"train", "test"}


def test_device_capture_augmentation_is_finite_and_nonzero() -> None:
    config = load_config()
    sample_rate = int(config["audio"]["sample_rate_hz"])
    time = np.arange(sample_rate, dtype=np.float32) / sample_rate
    waveform = 0.03 * np.sin(2.0 * np.pi * 440.0 * time)
    result = _simulate_device_capture(waveform, config)
    assert result.shape == waveform.shape
    assert np.isfinite(result).all()
    assert float(np.sqrt(np.mean(np.square(result)))) > 0.01


def test_short_cry_mix_keeps_event_inside_one_second() -> None:
    config = load_config()
    sample_rate = int(config["audio"]["sample_rate_hz"])
    cry = np.zeros(sample_rate, dtype=np.float32)
    cry[2000:8000] = 0.2 * np.sin(
        2.0 * np.pi * 500.0 * np.arange(6000, dtype=np.float32) / sample_rate
    )
    background = np.full(sample_rate, 0.001, dtype=np.float32)
    mixed = _short_cry_mix(
        cry,
        background,
        np.random.default_rng(1234),
        sample_rate,
        config,
    )
    assert mixed.shape == cry.shape
    assert np.isfinite(mixed).all()
    assert np.max(np.abs(mixed - background)) > 0.002


def test_duplicate_content_prefers_existing_source_over_dbl() -> None:
    rows = [
        {
            "source": "donateacry",
            "source_path": "old.wav",
            "wave_sha256": "same",
            "classification_label": "hungry",
            "quality_status": "accepted",
            "quality_reasons": "",
        },
        {
            "source": "infantcry_dbl_tier1",
            "source_path": "new.wav",
            "wave_sha256": "same",
            "classification_label": "hungry",
            "quality_status": "accepted",
            "quality_reasons": "",
        },
    ]
    _reject_duplicate_content(rows)
    assert rows[0]["quality_status"] == "accepted"
    assert rows[1]["quality_status"] == "rejected"
    assert rows[1]["quality_reasons"] == "duplicate_content"


def test_conflicting_duplicate_labels_are_all_rejected() -> None:
    rows = [
        {
            "source": "donateacry",
            "source_path": "hungry.wav",
            "wave_sha256": "same",
            "classification_label": "hungry",
            "quality_status": "accepted",
            "quality_reasons": "",
        },
        {
            "source": "infantcry_dbl_tier2",
            "source_path": "tired.wav",
            "wave_sha256": "same",
            "classification_label": "tired",
            "quality_status": "accepted",
            "quality_reasons": "",
        },
    ]
    _reject_duplicate_content(rows)
    assert {row["quality_status"] for row in rows} == {"rejected"}
    assert {
        row["quality_reasons"] for row in rows
    } == {"duplicate_label_conflict"}


def test_exact_duplicate_connects_independent_groups() -> None:
    rows = [
        {
            "quality_status": "accepted",
            "group_id": "baby-a",
            "wave_sha256": "same-wave",
            "split_group": "",
        },
        {
            "quality_status": "accepted",
            "group_id": "baby-b",
            "wave_sha256": "same-wave",
            "split_group": "",
        },
        {
            "quality_status": "accepted",
            "group_id": "baby-c",
            "wave_sha256": "different-wave",
            "split_group": "",
        },
    ]
    _attach_split_groups(rows)
    assert rows[0]["split_group"] == rows[1]["split_group"]
    assert rows[0]["split_group"] != rows[2]["split_group"]


def test_three_consecutive_windows_confirm_cry() -> None:
    result = find_confirmed_cry([0.1, 0.82, 0.90, 0.88, 0.2])
    assert result["confirmed"] is True
    assert result["first_confirmed_window"] == 3
    assert result["longest_run"] == 3


def test_three_of_eight_vote_tolerates_interrupted_cry() -> None:
    result = find_voted_cry(
        [0.10, 0.62, 0.22, 0.71, 0.18, 0.65, 0.12],
        threshold=0.40,
        vote_window_count=8,
        required_hits=3,
    )
    assert result["confirmed"] is True
    assert result["first_confirmed_window"] == 5
    assert result["maximum_hits"] == 3


def test_three_of_eight_vote_rejects_isolated_spike() -> None:
    result = find_voted_cry(
        [0.10, 0.82, 0.12, 0.18, 0.15, 0.75, 0.10, 0.12, 0.11, 0.09, 0.72],
        threshold=0.40,
        vote_window_count=8,
        required_hits=3,
    )
    assert result["confirmed"] is False
    assert result["maximum_hits"] == 2


def test_type_vote_rejects_small_margin() -> None:
    classes = ["hungry", "belly_pain", "discomfort", "tired", "burping", "unknown"]
    probabilities = np.zeros((4, 6), dtype=np.float32)
    probabilities[:, 0] = 0.55
    probabilities[:, 2] = 0.45
    result = classify_cry_type(
        probabilities,
        classes,
        confidence_threshold=0.50,
        margin_threshold=0.15,
    )
    assert result["label"] == "uncertain"
    assert result["status"] == "small_margin"


def test_largest_rare_group_is_kept_in_training() -> None:
    config = load_config()
    rows = []
    for group, count in (("unknown-large", 5), ("unknown-small-a", 1), ("unknown-small-b", 1)):
        for _ in range(count):
            rows.append(
                {
                    "quality_status": "accepted",
                    "split_group": group,
                    "detection_label": "cry",
                    "classification_label": "unknown",
                    "source_partition": "",
                    "split": "",
                }
            )
    for index in range(12):
        rows.append(
            {
                "quality_status": "accepted",
                "split_group": f"negative-{index}",
                "detection_label": "non_cry",
                "classification_label": "",
                "source_partition": "",
                "split": "",
            }
        )
    _assign_splits(rows, config)
    unknown_counts = {
        split: sum(
            row["classification_label"] == "unknown" and row["split"] == split
            for row in rows
        )
        for split in ("train", "validation", "test")
    }
    assert unknown_counts["train"] == 5
    assert unknown_counts["validation"] == 1
    assert unknown_counts["test"] == 1

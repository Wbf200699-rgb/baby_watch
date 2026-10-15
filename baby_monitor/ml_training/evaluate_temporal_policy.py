from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

import numpy as np
import tensorflow as tf
import yaml
from sklearn.metrics import classification_report, confusion_matrix

from audio_pipeline.common import ROOT
from audio_pipeline.decision import classify_cry_type, find_confirmed_cry


def load_dataset(path: Path) -> dict[str, np.ndarray]:
    with np.load(path) as data:
        required = {"x", "y", "recording_id", "window_start_samples", "classes"}
        missing = required - set(data.files)
        if missing:
            raise ValueError(f"{path}: missing arrays {sorted(missing)}")
        return {name: np.asarray(data[name]) for name in required}


def grouped_indices(recording_ids: np.ndarray, starts: np.ndarray) -> dict[str, list[int]]:
    groups: dict[str, list[int]] = defaultdict(list)
    for index, recording_id in enumerate(recording_ids):
        groups[str(recording_id)].append(index)
    for indices in groups.values():
        indices.sort(key=lambda index: int(starts[index]))
    return dict(groups)


def evaluate_detection(
    model: tf.keras.Model,
    dataset: dict[str, np.ndarray],
    decision: dict,
    sample_rate: int,
) -> dict:
    classes = [str(value) for value in dataset["classes"]]
    cry_index = classes.index("cry")
    probabilities = model.predict(dataset["x"], batch_size=256, verbose=0)
    groups = grouped_indices(dataset["recording_id"], dataset["window_start_samples"])
    truth: list[int] = []
    predicted: list[int] = []
    non_cry_seconds = 0.0
    false_positive_recordings = 0
    for indices in groups.values():
        labels = set(int(dataset["y"][index]) for index in indices)
        if len(labels) != 1:
            raise ValueError("one recording has multiple detection labels")
        actual = int(next(iter(labels)) == cry_index)
        trigger = find_confirmed_cry(
            [float(probabilities[index, cry_index]) for index in indices],
            threshold=float(decision["detection_threshold"]),
            consecutive_windows=int(decision["consecutive_detection_windows"]),
        )
        prediction = int(trigger["confirmed"])
        truth.append(actual)
        predicted.append(prediction)
        if not actual:
            duration = (
                int(dataset["window_start_samples"][indices[-1]]) / sample_rate + 1.0
            )
            non_cry_seconds += duration
            false_positive_recordings += prediction
    report = classification_report(
        truth,
        predicted,
        labels=[0, 1],
        target_names=["non_cry", "cry"],
        output_dict=True,
        zero_division=0,
    )
    return {
        "recordings": len(groups),
        "confusion_matrix": confusion_matrix(truth, predicted, labels=[0, 1]).tolist(),
        "per_class": report,
        "false_positive_recordings": false_positive_recordings,
        "evaluated_non_cry_hours": non_cry_seconds / 3600.0,
        "false_positive_recordings_per_hour": (
            false_positive_recordings / (non_cry_seconds / 3600.0)
            if non_cry_seconds
            else None
        ),
    }


def evaluate_type(
    model: tf.keras.Model,
    dataset: dict[str, np.ndarray],
    decision: dict,
) -> dict:
    classes = [str(value) for value in dataset["classes"]]
    probabilities = model.predict(dataset["x"], batch_size=256, verbose=0)
    groups = grouped_indices(dataset["recording_id"], dataset["window_start_samples"])
    truth: list[str] = []
    predicted: list[str] = []
    status_counts: dict[str, int] = defaultdict(int)
    maximum_windows = int(decision["type_window_count"])
    for indices in groups.values():
        labels = set(int(dataset["y"][index]) for index in indices)
        if len(labels) != 1:
            raise ValueError("one recording has multiple cry-type labels")
        selected = indices[:maximum_windows]
        result = classify_cry_type(
            probabilities[selected],
            classes,
            confidence_threshold=float(decision["type_confidence_threshold"]),
            margin_threshold=float(decision["type_margin_threshold"]),
        )
        truth.append(classes[next(iter(labels))])
        predicted.append(result["label"])
        status_counts[result["status"]] += 1

    extended_classes = classes + ["uncertain"]
    report = classification_report(
        truth,
        predicted,
        labels=extended_classes,
        target_names=extended_classes,
        output_dict=True,
        zero_division=0,
    )
    return {
        "recordings": len(groups),
        "windows_per_decision": maximum_windows,
        "status_counts": dict(status_counts),
        "coverage_without_uncertain": (
            sum(label != "uncertain" for label in predicted) / len(predicted)
            if predicted
            else 0.0
        ),
        "confusion_matrix_labels": extended_classes,
        "confusion_matrix": confusion_matrix(
            truth, predicted, labels=extended_classes
        ).tolist(),
        "per_class": report,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Evaluate the multi-window cry policy")
    parser.add_argument("--detection-model", type=Path, required=True)
    parser.add_argument("--detection-data", type=Path, required=True)
    parser.add_argument("--type-model", type=Path, required=True)
    parser.add_argument("--type-data", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=ROOT / "audio_two_stage.yaml")
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "artifacts" / "temporal_policy_report.json",
    )
    args = parser.parse_args()

    config = yaml.safe_load(args.config.read_text(encoding="utf-8"))
    report = {
        "decision": config["decision"],
        "detection": evaluate_detection(
            tf.keras.models.load_model(args.detection_model),
            load_dataset(args.detection_data),
            config["decision"],
            int(config["audio"]["sample_rate_hz"]),
        ),
        "cry_type": evaluate_type(
            tf.keras.models.load_model(args.type_model),
            load_dataset(args.type_data),
            config["decision"],
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

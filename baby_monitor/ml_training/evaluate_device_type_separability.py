from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import accuracy_score, balanced_accuracy_score, confusion_matrix
from sklearn.neighbors import KNeighborsClassifier


ROOT = Path(__file__).resolve().parent


def metrics(y: np.ndarray, prediction: np.ndarray, classes: list[str]) -> dict:
    matrix = confusion_matrix(y, prediction, labels=np.arange(len(classes)))
    per_class = {}
    for class_id, label in enumerate(classes):
        mask = y == class_id
        per_class[label] = {
            "windows": int(np.sum(mask)),
            "correct": int(np.sum(prediction[mask] == class_id)),
            "recall": float(np.mean(prediction[mask] == class_id)) if np.any(mask) else None,
        }
    return {
        "windows": int(len(y)),
        "accuracy": float(accuracy_score(y, prediction)),
        "balanced_accuracy": float(balanced_accuracy_score(y, prediction)),
        "confusion_matrix": matrix.tolist(),
        "per_class": per_class,
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Check whether long RA8P1 playback captures contain separable cry types."
    )
    parser.add_argument(
        "--data",
        type=Path,
        default=ROOT / "data" / "features" / "cry_type_deploy_train.npz",
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    with np.load(args.data) as data:
        required = {"x", "y", "source", "recording_id", "classes"}
        missing = required - set(data.files)
        if missing:
            raise ValueError(f"{args.data}: missing arrays {sorted(missing)}")
        arrays = {name: np.asarray(data[name]) for name in required}
    classes = [str(value) for value in arrays["classes"]]
    x = arrays["x"].reshape(len(arrays["x"]), -1)
    sources = arrays["source"].astype(str)
    device_mask = sources == "device_selected_ra8p1_type"
    public_mask = np.isin(sources, ["donateacry", "mendeley_infants_cry"])
    if not np.any(device_mask) or not np.any(public_mask):
        raise ValueError("both public and device-selected training windows are required")

    # Public-original nearest neighbours show whether the microphone captures
    # preserve enough source acoustics to recover the folder label.
    neighbours = KNeighborsClassifier(
        n_neighbors=5,
        weights="distance",
        metric="cosine",
        n_jobs=-1,
    )
    neighbours.fit(x[public_mask], arrays["y"][public_mask])
    public_to_device = neighbours.predict(x[device_mask])

    # A simple linear domain-transfer baseline is less able to memorize an
    # individual waveform than nearest neighbours.
    linear = LogisticRegression(
        class_weight="balanced",
        max_iter=1000,
        solver="lbfgs",
    )
    linear.fit(x[public_mask], arrays["y"][public_mask])
    linear_public_to_device = linear.predict(x[device_mask])

    # Two folds alternate complete 30-second recorder files. All windows from
    # one file stay together, so this does not randomly leak overlapping windows.
    device_indices = np.flatnonzero(device_mask)
    groups: dict[int, list[str]] = defaultdict(list)
    group_label: dict[str, int] = {}
    for index in device_indices:
        recording = str(arrays["recording_id"][index])
        label = int(arrays["y"][index])
        if recording not in group_label:
            groups[label].append(recording)
            group_label[recording] = label
    fold_by_group = {
        recording: position % 2
        for label in sorted(groups)
        for position, recording in enumerate(sorted(groups[label]))
    }
    cross_truth: list[int] = []
    cross_prediction: list[int] = []
    for fold in (0, 1):
        test = np.asarray(
            [
                index
                for index in device_indices
                if fold_by_group[str(arrays["recording_id"][index])] == fold
            ],
            dtype=np.int32,
        )
        train = np.asarray(
            [
                index
                for index in device_indices
                if fold_by_group[str(arrays["recording_id"][index])] != fold
            ],
            dtype=np.int32,
        )
        classifier = LogisticRegression(
            class_weight="balanced",
            max_iter=1000,
            solver="lbfgs",
        )
        classifier.fit(x[train], arrays["y"][train])
        cross_truth.extend(int(value) for value in arrays["y"][test])
        cross_prediction.extend(int(value) for value in classifier.predict(x[test]))

    device_recordings = Counter(
        classes[group_label[recording]] for recording in group_label
    )
    report = {
        "classes": classes,
        "device_recordings": dict(device_recordings),
        "device_windows": int(np.sum(device_mask)),
        "public_reference_windows": int(np.sum(public_mask)),
        "public_knn_to_device": metrics(
            arrays["y"][device_mask], public_to_device, classes
        ),
        "public_linear_to_device": metrics(
            arrays["y"][device_mask], linear_public_to_device, classes
        ),
        "device_file_two_fold_linear": metrics(
            np.asarray(cross_truth), np.asarray(cross_prediction), classes
        ),
        "interpretation": (
            "The two-fold result keeps complete 30-second files together, but files "
            "from one label were recorded in the same playback session and are not "
            "an independent-infant clinical test."
        ),
    }
    payload = json.dumps(report, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
    print(payload)


if __name__ == "__main__":
    main()

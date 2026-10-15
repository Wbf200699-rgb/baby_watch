from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

import numpy as np
import tensorflow as tf
from sklearn.metrics import accuracy_score, balanced_accuracy_score, confusion_matrix


def _strings(values: np.ndarray) -> np.ndarray:
    return np.asarray(values).astype(str)


def _class_metrics(truth: np.ndarray, predicted: np.ndarray, classes: list[str]) -> dict[str, dict[str, float | int]]:
    result: dict[str, dict[str, float | int]] = {}
    for class_id, name in enumerate(classes):
        mask = truth == class_id
        total = int(np.count_nonzero(mask))
        correct = int(np.count_nonzero(predicted[mask] == class_id)) if total else 0
        result[name] = {
            "count": total,
            "recall": float(correct / total) if total else 0.0,
        }
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description="Evaluate a cry-type model by window and recording")
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--features", type=Path, required=True)
    parser.add_argument("--source", action="append", default=[])
    parser.add_argument("--augmentation-kind", action="append", default=[])
    parser.add_argument("--vote-windows", type=int, default=8)
    parser.add_argument("--confidence-threshold", type=float, default=0.65)
    parser.add_argument("--margin-threshold", type=float, default=0.15)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    with np.load(args.features, allow_pickle=False) as package:
        x = np.asarray(package["x"], dtype=np.float32)
        truth = np.asarray(package["y"], dtype=np.int64)
        classes = _strings(package["classes"]).tolist()
        sources = _strings(package["source"])
        group_key = "group_id" if "group_id" in package.files else "recording_id"
        group_ids = _strings(package[group_key])
        kinds = _strings(package["augmentation_kind"])

    mask = np.ones(len(x), dtype=bool)
    if args.source:
        mask &= np.isin(sources, args.source)
    selected_kinds = args.augmentation_kind or ["base"]
    mask &= np.isin(kinds, selected_kinds)
    if not np.any(mask):
        raise ValueError("no windows matched the requested filters")

    x = x[mask]
    truth = truth[mask]
    sources = sources[mask]
    group_ids = group_ids[mask]
    kinds = kinds[mask]

    model = tf.keras.models.load_model(args.model, compile=False)
    probabilities = np.asarray(model.predict(x, batch_size=256, verbose=0), dtype=np.float32)
    predicted = np.argmax(probabilities, axis=1)

    grouped_indices: dict[str, list[int]] = defaultdict(list)
    for index, group_id in enumerate(group_ids):
        grouped_indices[group_id].append(index)

    recording_truth: list[int] = []
    recording_predicted: list[int] = []
    recording_policy: list[int] = []
    confidences: list[float] = []
    margins: list[float] = []
    unknown_id = len(classes)
    for indices in grouped_indices.values():
        selected = indices[: args.vote_windows]
        means = probabilities[selected].mean(axis=0)
        order = np.argsort(means)
        best = int(order[-1])
        confidence = float(means[best])
        margin = float(means[best] - means[int(order[-2])])
        recording_truth.append(int(truth[selected[0]]))
        recording_predicted.append(best)
        recording_policy.append(
            best
            if confidence >= args.confidence_threshold and margin >= args.margin_threshold
            else unknown_id
        )
        confidences.append(confidence)
        margins.append(margin)

    rec_truth = np.asarray(recording_truth, dtype=np.int64)
    rec_pred = np.asarray(recording_predicted, dtype=np.int64)
    rec_policy = np.asarray(recording_policy, dtype=np.int64)
    accepted = rec_policy != unknown_id
    report = {
        "model": str(args.model.resolve()),
        "features": str(args.features.resolve()),
        "filters": {
            "source": args.source,
            "augmentation_kind": selected_kinds,
        },
        "windows": int(len(x)),
        "window_accuracy": float(accuracy_score(truth, predicted)),
        "window_balanced_accuracy": float(balanced_accuracy_score(truth, predicted)),
        "window_class_metrics": _class_metrics(truth, predicted, classes),
        "window_confusion_matrix": confusion_matrix(
            truth, predicted, labels=np.arange(len(classes))
        ).tolist(),
        "recordings": int(len(rec_truth)),
        "recording_accuracy": float(accuracy_score(rec_truth, rec_pred)),
        "recording_balanced_accuracy": float(balanced_accuracy_score(rec_truth, rec_pred)),
        "recording_class_metrics": _class_metrics(rec_truth, rec_pred, classes),
        "recording_confusion_matrix": confusion_matrix(
            rec_truth, rec_pred, labels=np.arange(len(classes))
        ).tolist(),
        "policy": {
            "confidence_threshold": args.confidence_threshold,
            "margin_threshold": args.margin_threshold,
            "coverage": float(np.mean(accepted)),
            "accepted_accuracy": float(np.mean(rec_policy[accepted] == rec_truth[accepted]))
            if np.any(accepted)
            else 0.0,
            "accuracy_with_abstention": float(np.mean(rec_policy == rec_truth)),
            "mean_confidence": float(np.mean(confidences)),
            "mean_margin": float(np.mean(margins)),
            "confusion_matrix_labels": classes + ["unknown"],
            "confusion_matrix": confusion_matrix(
                rec_truth, rec_policy, labels=np.arange(unknown_id + 1)
            ).tolist(),
        },
        "classes": classes,
        "sources": sorted(set(sources.tolist())),
        "augmentation_kinds": sorted(set(kinds.tolist())),
    }

    encoded = json.dumps(report, ensure_ascii=True, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")


if __name__ == "__main__":
    main()

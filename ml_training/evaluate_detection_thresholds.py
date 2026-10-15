from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

import numpy as np
import tensorflow as tf
from sklearn.metrics import confusion_matrix

from audio_pipeline.decision import find_confirmed_cry, find_voted_cry


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Scan cry thresholds on complete recordings, optionally for one source."
    )
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--source")
    parser.add_argument(
        "--thresholds",
        default="0.10,0.20,0.30,0.40,0.50,0.60,0.70,0.80,0.90",
    )
    parser.add_argument("--consecutive", type=int, default=3)
    parser.add_argument(
        "--vote-window",
        type=int,
        help="Use a sliding N-window vote instead of strict consecutive windows.",
    )
    parser.add_argument("--required-hits", type=int, default=3)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    with np.load(args.data) as data:
        required = {
            "x",
            "y",
            "classes",
            "source",
            "recording_id",
            "window_start_samples",
        }
        missing = required - set(data.files)
        if missing:
            raise ValueError(f"dataset is missing arrays: {sorted(missing)}")
        arrays = {key: np.asarray(data[key]) for key in required}

    mask = np.ones(len(arrays["y"]), dtype=bool)
    if args.source:
        mask &= arrays["source"].astype(str) == args.source
    if not np.any(mask):
        raise ValueError(f"no windows matched source {args.source!r}")
    arrays = {key: value[mask] for key, value in arrays.items() if key != "classes"} | {
        "classes": arrays["classes"]
    }

    classes = [str(value) for value in arrays["classes"]]
    cry_index = classes.index("cry")
    model = tf.keras.models.load_model(args.model)
    probabilities = model.predict(arrays["x"], batch_size=256, verbose=0)[:, cry_index]

    quantiles: dict[str, dict[str, float | int]] = {}
    for class_index, label in enumerate(classes):
        values = probabilities[arrays["y"] == class_index]
        quantiles[label] = {"windows": int(len(values))}
        if len(values):
            quantiles[label].update({
                f"p{percentile:02d}": float(np.percentile(values, percentile))
                for percentile in (0, 5, 10, 25, 50, 75, 90, 95, 100)
            })

    groups: dict[str, list[int]] = defaultdict(list)
    for index, recording_id in enumerate(arrays["recording_id"].astype(str)):
        groups[recording_id].append(index)
    for indices in groups.values():
        indices.sort(key=lambda index: int(arrays["window_start_samples"][index]))

    scans = []
    for threshold in (float(value) for value in args.thresholds.split(",")):
        truth: list[int] = []
        predicted: list[int] = []
        for indices in groups.values():
            labels = {int(arrays["y"][index]) for index in indices}
            if len(labels) != 1:
                raise ValueError("one recording has multiple labels")
            truth.append(int(next(iter(labels)) == cry_index))
            if args.vote_window:
                trigger = find_voted_cry(
                    probabilities[indices],
                    threshold=threshold,
                    vote_window_count=args.vote_window,
                    required_hits=args.required_hits,
                )
            else:
                trigger = find_confirmed_cry(
                    probabilities[indices],
                    threshold=threshold,
                    consecutive_windows=args.consecutive,
                )
            predicted.append(int(trigger["confirmed"]))
        matrix = confusion_matrix(truth, predicted, labels=[0, 1])
        tn, fp, fn, tp = (int(value) for value in matrix.ravel())
        scans.append(
            {
                "threshold": threshold,
                "confusion_matrix": matrix.tolist(),
                "non_cry_false_positive_rate": fp / (tn + fp) if tn + fp else None,
                "cry_recording_recall": tp / (tp + fn) if tp + fn else None,
            }
        )

    report = {
        "model": str(args.model),
        "data": str(args.data),
        "source": args.source,
        "policy": (
            {
                "kind": "sliding_vote",
                "vote_window_count": args.vote_window,
                "required_hits": args.required_hits,
            }
            if args.vote_window
            else {
                "kind": "consecutive",
                "consecutive_windows": args.consecutive,
            }
        ),
        "recordings": len(groups),
        "probability_quantiles": quantiles,
        "threshold_scan": scans,
    }
    payload = json.dumps(report, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
    print(payload)


if __name__ == "__main__":
    main()

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import tensorflow as tf
import yaml


ROOT = Path(__file__).resolve().parent
TASK_TO_CONFIG = {
    "occlusion": "occlusion_classifier",
    "behavior": "behavior_classifier",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Evaluate one INT8 classifier class at fixed score thresholds")
    parser.add_argument("--task", required=True, choices=tuple(TASK_TO_CONFIG))
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--data-root", required=True, type=Path)
    parser.add_argument("--positive-class", required=True)
    parser.add_argument("--thresholds", nargs="+", type=float, default=[0.4, 0.5, 0.6, 0.7, 0.75, 0.8])
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    config = yaml.safe_load((ROOT / "vision_models.yaml").read_text(encoding="utf-8"))
    classes = list(config[TASK_TO_CONFIG[args.task]]["classes"])
    if args.positive_class not in classes:
        raise SystemExit(f"Unknown class {args.positive_class!r}; expected one of {classes}")
    positive_index = classes.index(args.positive_class)
    test_root = args.data_root / args.task / "test"
    dataset = tf.keras.utils.image_dataset_from_directory(
        test_root,
        labels="inferred",
        label_mode="categorical",
        class_names=classes,
        color_mode="rgb",
        batch_size=1,
        image_size=(192, 192),
        shuffle=False,
        interpolation="bilinear",
    )
    interpreter = tf.lite.Interpreter(model_path=str(args.model))
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    input_scale, input_zero = input_detail["quantization"]
    output_scale, output_zero = output_detail["quantization"]
    scores: list[float] = []
    expected_positive: list[bool] = []
    for images, labels in dataset:
        image = images.numpy()[0]
        quantized = np.clip(np.rint(image / input_scale + input_zero), -128, 127).astype(np.int8)
        interpreter.set_tensor(input_detail["index"], quantized[None, ...])
        interpreter.invoke()
        raw = interpreter.get_tensor(output_detail["index"])[0]
        probabilities = (raw.astype(np.float32) - output_zero) * output_scale
        scores.append(float(probabilities[positive_index]))
        expected_positive.append(int(np.argmax(labels.numpy()[0])) == positive_index)

    rows = []
    truth = np.asarray(expected_positive, dtype=bool)
    values = np.asarray(scores, dtype=np.float32)
    for threshold in args.thresholds:
        predicted = values >= threshold
        tp = int(np.sum(predicted & truth))
        fp = int(np.sum(predicted & ~truth))
        fn = int(np.sum(~predicted & truth))
        tn = int(np.sum(~predicted & ~truth))
        precision = tp / (tp + fp) if tp + fp else None
        recall = tp / (tp + fn) if tp + fn else None
        rows.append(
            {
                "threshold": threshold,
                "tp": tp,
                "fp": fp,
                "fn": fn,
                "tn": tn,
                "precision": precision,
                "recall": recall,
            }
        )
    report = {
        "task": args.task,
        "model": str(args.model),
        "positive_class": args.positive_class,
        "samples": len(scores),
        "positive_samples": int(np.sum(truth)),
        "thresholds": rows,
    }
    rendered = json.dumps(report, ensure_ascii=False, indent=2)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

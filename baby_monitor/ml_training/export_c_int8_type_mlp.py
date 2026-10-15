from __future__ import annotations

import argparse
import hashlib
import json
from collections import defaultdict
from pathlib import Path

import numpy as np
import tensorflow as tf
from sklearn.metrics import (
    balanced_accuracy_score,
    confusion_matrix,
    f1_score,
)

from audio_pipeline.common import sha256_file
from export_c_int8_mlp import (
    activation_scale,
    dense_quantized,
    format_c_array,
    quantize_per_output,
    quantize_symmetric,
)


ROOT = Path(__file__).resolve().parent
EXPECTED_CLASSES = ["hungry", "belly_pain", "discomfort", "tired", "burping"]


def resolve_frozen_contract(model_path: Path, explicit: Path | None) -> Path:
    path = explicit or (model_path.resolve().parent / "preprocessing_contract.json")
    if not path.exists():
        raise FileNotFoundError(
            "preprocessing_contract.json is missing next to the model; retrain first"
        )
    return path


def verify_contract_hash(path: Path, expected: str) -> None:
    with np.load(path) as data:
        if "preprocessing_sha256" not in data.files:
            raise ValueError(f"{path}: preprocessing_sha256 is missing")
        actual = str(np.asarray(data["preprocessing_sha256"]).item())
    if actual != expected:
        raise ValueError(f"{path}: preprocessing contract mismatch")


def quantized_probabilities(
    x: np.ndarray,
    input_scale: float,
    hidden1_scale: float,
    hidden2_scale: float,
    weights: list[np.ndarray],
    weight_scales: list[np.ndarray],
    biases: list[np.ndarray],
) -> np.ndarray:
    q0 = quantize_symmetric(x, input_scale, relu=False)
    h1 = np.maximum(
        dense_quantized(
            q0.reshape(len(q0), -1),
            input_scale,
            weights[0],
            weight_scales[0],
            biases[0],
        ),
        0.0,
    )
    q1 = quantize_symmetric(h1, hidden1_scale, relu=True)
    h2 = np.maximum(
        dense_quantized(q1, hidden1_scale, weights[1], weight_scales[1], biases[1]),
        0.0,
    )
    q2 = quantize_symmetric(h2, hidden2_scale, relu=True)
    logits = dense_quantized(
        q2, hidden2_scale, weights[2], weight_scales[2], biases[2]
    )
    logits -= np.max(logits, axis=1, keepdims=True)
    exponentials = np.exp(np.clip(logits, -80.0, 0.0))
    return exponentials / np.sum(exponentials, axis=1, keepdims=True)


def recording_report(
    probabilities: np.ndarray,
    arrays: dict[str, np.ndarray],
    *,
    minimum_windows: int,
    maximum_windows: int,
    confidence_threshold: float,
    margin_threshold: float,
) -> dict:
    groups: dict[str, list[int]] = defaultdict(list)
    for index, recording_id in enumerate(arrays["recording_id"].astype(str)):
        groups[recording_id].append(index)

    truth: list[int] = []
    predicted: list[int] = []
    confidence_values: list[float] = []
    margin_values: list[float] = []
    short_recordings = 0
    for indices in groups.values():
        indices.sort(key=lambda index: int(arrays["window_start_samples"][index]))
        selected = indices[:maximum_windows]
        labels = {int(arrays["y"][index]) for index in selected}
        if len(labels) != 1:
            raise ValueError("one recording has multiple cry-type labels")
        means = np.mean(probabilities[selected], axis=0)
        order = np.argsort(means)[::-1]
        best = int(order[0])
        confidence = float(means[best])
        margin = float(means[best] - means[int(order[1])])
        accepted = (
            len(selected) >= minimum_windows
            and confidence >= confidence_threshold
            and margin >= margin_threshold
        )
        if len(selected) < minimum_windows:
            short_recordings += 1
        truth.append(next(iter(labels)))
        predicted.append(best if accepted else len(EXPECTED_CLASSES))
        confidence_values.append(confidence)
        margin_values.append(margin)

    unknown_id = len(EXPECTED_CLASSES)
    accepted_mask = np.asarray(predicted) != unknown_id
    correct = np.asarray(predicted) == np.asarray(truth)
    per_class = {}
    for class_id, label in enumerate(EXPECTED_CLASSES):
        mask = np.asarray(truth) == class_id
        per_class[label] = {
            "recordings": int(np.sum(mask)),
            "accepted": int(np.sum(mask & accepted_mask)),
            "correct": int(np.sum(mask & correct)),
            "recall_with_abstention": (
                float(np.mean(correct[mask])) if np.any(mask) else None
            ),
        }
    return {
        "recordings": len(groups),
        "minimum_windows": minimum_windows,
        "maximum_windows": maximum_windows,
        "short_recordings": short_recordings,
        "eligible_recordings": len(groups) - short_recordings,
        "confidence_threshold": confidence_threshold,
        "margin_threshold": margin_threshold,
        "coverage": float(np.mean(accepted_mask)) if len(truth) else 0.0,
        "accuracy_with_abstention": float(np.mean(correct)) if len(truth) else 0.0,
        "accepted_accuracy": (
            float(np.mean(correct[accepted_mask])) if np.any(accepted_mask) else None
        ),
        "mean_confidence": float(np.mean(confidence_values)),
        "mean_margin": float(np.mean(margin_values)),
        "confusion_matrix_labels": EXPECTED_CLASSES + ["unknown"],
        "confusion_matrix": confusion_matrix(
            truth, predicted, labels=np.arange(unknown_id + 1)
        ).tolist(),
        "per_class": per_class,
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Export the five-class cry reason MLP as an INT8 Cortex-M85 fallback."
    )
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--train", type=Path, required=True)
    parser.add_argument("--test", type=Path, required=True)
    parser.add_argument("--activation-percentile", type=float, default=99.99)
    parser.add_argument(
        "--contract",
        type=Path,
    )
    parser.add_argument("--decision-windows", type=int, default=8)
    parser.add_argument("--minimum-windows", type=int, default=1)
    parser.add_argument(
        "--train-source",
        action="append",
        default=[],
        help="Use only this calibration source (may be repeated)",
    )
    parser.add_argument(
        "--test-source",
        action="append",
        default=[],
        help="Use only this evaluation source (may be repeated)",
    )
    parser.add_argument("--confidence-threshold", type=float, default=0.50)
    parser.add_argument("--margin-threshold", type=float, default=0.10)
    parser.add_argument("--output-header", type=Path, required=True)
    parser.add_argument("--output-report", type=Path, required=True)
    args = parser.parse_args()
    if args.minimum_windows < 1:
        parser.error("--minimum-windows must be at least 1")
    if args.decision_windows < args.minimum_windows:
        parser.error("--decision-windows must be >= --minimum-windows")
    args.contract = resolve_frozen_contract(args.model, args.contract)
    contract_sha256 = sha256_file(args.contract)
    verify_contract_hash(args.train, contract_sha256)
    verify_contract_hash(args.test, contract_sha256)

    model = tf.keras.models.load_model(args.model)
    dense_layers = [
        layer for layer in model.layers if isinstance(layer, tf.keras.layers.Dense)
    ]
    if [layer.units for layer in dense_layers] != [64, 16, len(EXPECTED_CLASSES)]:
        raise ValueError(
            f"expected Dense widths [64, 16, {len(EXPECTED_CLASSES)}]"
        )

    quantized_weights: list[np.ndarray] = []
    weight_scales: list[np.ndarray] = []
    biases: list[np.ndarray] = []
    for layer in dense_layers:
        weights, bias = layer.get_weights()
        quantized, scales = quantize_per_output(weights)
        quantized_weights.append(quantized)
        weight_scales.append(scales)
        biases.append(np.asarray(bias, dtype=np.float32))

    with np.load(args.train) as train_data:
        train_x = np.asarray(train_data["x"], dtype=np.float32)
        train_classes = [str(value) for value in train_data["classes"]]
        if args.train_source:
            if "source" not in train_data.files:
                raise ValueError("train dataset has no source array")
            train_mask = np.isin(
                np.asarray(train_data["source"]).astype(str), args.train_source
            )
            if not np.any(train_mask):
                raise ValueError(
                    f"no train windows matched sources {args.train_source!r}"
                )
            train_x = train_x[train_mask]
    if train_classes != EXPECTED_CLASSES:
        raise ValueError(f"unexpected train class order {train_classes}")

    input_scale = activation_scale(train_x, args.activation_percentile)
    q0 = quantize_symmetric(train_x, input_scale, relu=False)
    h1 = np.maximum(
        dense_quantized(
            q0.reshape(len(q0), -1),
            input_scale,
            quantized_weights[0],
            weight_scales[0],
            biases[0],
        ),
        0.0,
    )
    hidden1_scale = activation_scale(h1, args.activation_percentile)
    q1 = quantize_symmetric(h1, hidden1_scale, relu=True)
    h2 = np.maximum(
        dense_quantized(
            q1,
            hidden1_scale,
            quantized_weights[1],
            weight_scales[1],
            biases[1],
        ),
        0.0,
    )
    hidden2_scale = activation_scale(h2, args.activation_percentile)

    with np.load(args.test) as test_data:
        required = {
            "x",
            "y",
            "recording_id",
            "window_start_samples",
            "classes",
            "source",
        }
        missing = required - set(test_data.files)
        if missing:
            raise ValueError(f"test dataset missing arrays {sorted(missing)}")
        arrays = {name: np.asarray(test_data[name]) for name in required}
    classes = [str(value) for value in arrays["classes"]]
    if classes != EXPECTED_CLASSES:
        raise ValueError(f"unexpected test class order {classes}")
    if args.test_source:
        mask = np.isin(arrays["source"].astype(str), args.test_source)
        if not np.any(mask):
            raise ValueError(f"no test windows matched sources {args.test_source!r}")
        arrays = {
            name: value if name == "classes" else value[mask]
            for name, value in arrays.items()
        }

    probabilities = quantized_probabilities(
        np.asarray(arrays["x"], dtype=np.float32),
        input_scale,
        hidden1_scale,
        hidden2_scale,
        quantized_weights,
        weight_scales,
        biases,
    )
    predictions = np.argmax(probabilities, axis=1)
    model_digest = hashlib.sha256(args.model.read_bytes()).hexdigest()
    contract = json.loads(args.contract.read_text(encoding="utf-8"))
    coefficient_mean = np.asarray(contract["coefficient_mean"], dtype=np.float32)
    coefficient_std = np.asarray(contract["coefficient_std"], dtype=np.float32)
    report = {
        "model": str(args.model),
        "model_sha256": model_digest,
        "preprocessing_contract": str(args.contract),
        "preprocessing_sha256": contract_sha256,
        "classes": EXPECTED_CLASSES,
        "activation_percentile": args.activation_percentile,
        "train_source": args.train_source,
        "test_source": args.test_source,
        "input_scale": input_scale,
        "hidden1_scale": hidden1_scale,
        "hidden2_scale": hidden2_scale,
        "test_windows": int(len(arrays["y"])),
        "window_accuracy": float(np.mean(predictions == arrays["y"])),
        "window_balanced_accuracy": float(
            balanced_accuracy_score(arrays["y"], predictions)
        ),
        "window_macro_f1": float(
            f1_score(
                arrays["y"],
                predictions,
                labels=np.arange(len(EXPECTED_CLASSES)),
                average="macro",
                zero_division=0,
            )
        ),
        "window_confusion_matrix": confusion_matrix(
            arrays["y"], predictions, labels=np.arange(len(EXPECTED_CLASSES))
        ).tolist(),
        "recording_decision": recording_report(
            probabilities,
            arrays,
            minimum_windows=args.minimum_windows,
            maximum_windows=args.decision_windows,
            confidence_threshold=args.confidence_threshold,
            margin_threshold=args.margin_threshold,
        ),
    }

    sections = [
        "#ifndef AUDIO_CRY_TYPE_MODEL_DATA_H\n",
        "#define AUDIO_CRY_TYPE_MODEL_DATA_H\n\n",
        "#include <stdint.h>\n\n",
        f"#define CRY_TYPE_MODEL_SHA256_PREFIX \"{model_digest[:16]}\"\n",
        "#define CRY_TYPE_MODEL_INPUT_COUNT (490U)\n",
        "#define CRY_TYPE_MODEL_HIDDEN1_COUNT (64U)\n",
        "#define CRY_TYPE_MODEL_HIDDEN2_COUNT (16U)\n",
        f"#define CRY_TYPE_MODEL_CLASS_COUNT ({len(EXPECTED_CLASSES)}U)\n",
        f"#define CRY_TYPE_MODEL_INPUT_SCALE ({input_scale:.9g}f)\n",
        f"#define CRY_TYPE_MODEL_HIDDEN1_SCALE ({hidden1_scale:.9g}f)\n",
        f"#define CRY_TYPE_MODEL_HIDDEN2_SCALE ({hidden2_scale:.9g}f)\n\n",
        format_c_array(
            "g_cry_type_coefficient_mean", coefficient_mean, "float"
        ),
        format_c_array(
            "g_cry_type_coefficient_std", coefficient_std, "float"
        ),
    ]
    for index, (weights, scales, bias) in enumerate(
        zip(quantized_weights, weight_scales, biases), start=1
    ):
        sections.append(
            format_c_array(
                f"g_cry_type_dense{index}_weights",
                weights.T,
                "int8_t",
                values_per_line=20,
            )
        )
        sections.append(
            format_c_array(f"g_cry_type_dense{index}_scales", scales, "float")
        )
        sections.append(
            format_c_array(f"g_cry_type_dense{index}_bias", bias, "float")
        )
    sections.append("\n#endif /* AUDIO_CRY_TYPE_MODEL_DATA_H */\n")
    args.output_header.parent.mkdir(parents=True, exist_ok=True)
    args.output_header.write_text("".join(sections), encoding="utf-8", newline="\n")
    args.output_report.parent.mkdir(parents=True, exist_ok=True)
    args.output_report.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

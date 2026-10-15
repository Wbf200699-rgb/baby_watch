from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any

import numpy as np
import yaml


ROOT = Path(__file__).resolve().parent
TASK_TO_CONFIG = {
    "occlusion": "occlusion_classifier",
    "behavior": "behavior_classifier",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="训练 MobileNetV3-Small minimalistic 分类器并导出严格全 INT8 TFLite"
    )
    parser.add_argument("--task", required=True, choices=tuple(TASK_TO_CONFIG))
    parser.add_argument(
        "--data-root",
        type=Path,
        default=ROOT / "data" / "processed" / "roi_classification",
    )
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts")
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--head-epochs", type=int, default=15)
    parser.add_argument("--fine-epochs", type=int, default=10)
    parser.add_argument("--fine-layers", type=int, default=30)
    parser.add_argument("--representative-samples", type=int, default=256)
    parser.add_argument(
        "--critical-weight-multiplier",
        type=float,
        default=1.0,
        help="Multiply YAML critical-class weights after inverse-frequency balancing",
    )
    parser.add_argument("--seed", type=int, default=20260725)
    parser.add_argument("--weights", choices=("imagenet", "none"), default="imagenet")
    parser.add_argument(
        "--initial-model",
        type=Path,
        help="Adapt an existing compatible Keras model instead of rebuilding its classifier head",
    )
    parser.add_argument(
        "--classes",
        nargs="+",
        help=(
            "显式覆盖 YAML 类别顺序，仅用于类别尚不完整的原型训练；"
            "最终部署应省略此参数并使用 vision_models.yaml 合约"
        ),
    )
    return parser.parse_args()


def require_tensorflow():
    try:
        import tensorflow as tf
    except ImportError as exc:
        raise SystemExit("缺少 TensorFlow；请在 vision_training 下运行 .\\setup.ps1") from exc
    return tf


def count_class_files(root: Path, classes: list[str]) -> dict[str, int]:
    suffixes = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
    return {
        name: sum(1 for path in (root / name).rglob("*") if path.suffix.lower() in suffixes)
        for name in classes
    }


def make_dataset(tf, root: Path, classes: list[str], batch_size: int, seed: int, shuffle: bool):
    if not root.is_dir():
        raise SystemExit(f"数据目录不存在: {root}")
    dataset = tf.keras.utils.image_dataset_from_directory(
        root,
        labels="inferred",
        label_mode="categorical",
        class_names=classes,
        color_mode="rgb",
        batch_size=batch_size,
        image_size=(192, 192),
        shuffle=shuffle,
        seed=seed,
        interpolation="bilinear",
    )
    return dataset.prefetch(tf.data.AUTOTUNE)


def build_model(tf, class_count: int, imagenet: bool):
    inputs = tf.keras.Input(shape=(192, 192, 3), dtype=tf.float32, name="image")
    augmentation = tf.keras.Sequential(
        [
            tf.keras.layers.RandomFlip("horizontal"),
            tf.keras.layers.RandomRotation(0.035),
            tf.keras.layers.RandomZoom(0.10),
            tf.keras.layers.RandomContrast(0.15),
        ],
        name="augmentation",
    )
    x = augmentation(inputs)
    base = tf.keras.applications.MobileNetV3Small(
        input_shape=(192, 192, 3),
        alpha=1.0,
        minimalistic=True,
        include_top=False,
        weights="imagenet" if imagenet else None,
        pooling="avg",
        include_preprocessing=True,
    )
    base.trainable = False
    x = base(x, training=False)
    x = tf.keras.layers.Dropout(0.20)(x)
    outputs = tf.keras.layers.Dense(class_count, activation="softmax", name="probabilities")(x)
    return tf.keras.Model(inputs, outputs), base


def class_weights_from_counts(classes: list[str], counts: dict[str, int]) -> dict[int, float]:
    if any(counts[name] == 0 for name in classes):
        missing = [name for name in classes if counts[name] == 0]
        raise SystemExit(f"训练集缺少类别: {missing}")
    total = sum(counts.values())
    return {
        index: total / (len(classes) * counts[name])
        for index, name in enumerate(classes)
    }


def export_int8(tf, model, representative_dataset, destination: Path, sample_count: int) -> None:
    def representative():
        emitted = 0
        for images, _ in representative_dataset:
            for image in images:
                yield [tf.expand_dims(tf.cast(image, tf.float32), axis=0)]
                emitted += 1
                if emitted >= sample_count:
                    return

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    destination.write_bytes(converter.convert())


def metrics_from_confusion(confusion: np.ndarray, classes: list[str]) -> dict[str, Any]:
    if confusion.shape != (len(classes), len(classes)):
        raise ValueError(
            f"confusion shape {confusion.shape} does not match {len(classes)} classes"
        )
    total = int(confusion.sum())
    per_class: dict[str, dict[str, float | int | None]] = {}
    for index, name in enumerate(classes):
        true_positive = int(confusion[index, index])
        support = int(confusion[index, :].sum())
        predicted = int(confusion[:, index].sum())
        precision = true_positive / predicted if predicted else None
        recall = true_positive / support if support else None
        f1 = (
            2.0 * precision * recall / (precision + recall)
            if precision is not None and recall is not None and precision + recall > 0
            else None
        )
        per_class[name] = {
            "support": support,
            "precision": precision,
            "recall": recall,
            "f1": f1,
        }
    return {
        "samples": total,
        "accuracy": float(np.trace(confusion) / total) if total else None,
        "confusion_matrix": confusion.tolist(),
        "per_class": per_class,
    }


def evaluate_float(model, dataset, classes: list[str]) -> dict[str, Any]:
    confusion = np.zeros((len(classes), len(classes)), dtype=np.int64)
    for images, labels in dataset:
        expected = np.argmax(labels.numpy(), axis=1)
        predicted = np.argmax(model(images, training=False).numpy(), axis=1)
        for expected_index, predicted_index in zip(expected, predicted, strict=True):
            confusion[int(expected_index), int(predicted_index)] += 1
    return metrics_from_confusion(confusion, classes)


def evaluate_tflite(tf, model_path: Path, dataset, classes: list[str]) -> dict[str, Any]:
    interpreter = tf.lite.Interpreter(model_path=str(model_path))
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    if input_detail["dtype"] != np.int8 or output_detail["dtype"] != np.int8:
        raise RuntimeError("导出模型不是 strict INT8 输入/输出")
    input_scale, input_zero = input_detail["quantization"]
    output_scale, output_zero = output_detail["quantization"]
    confusion = np.zeros((len(classes), len(classes)), dtype=np.int64)
    for images, labels in dataset:
        images_np = images.numpy()
        labels_np = np.argmax(labels.numpy(), axis=1)
        for image, expected in zip(images_np, labels_np, strict=True):
            quantized = np.clip(
                np.rint(image / input_scale + input_zero),
                -128,
                127,
            ).astype(np.int8)
            interpreter.set_tensor(input_detail["index"], quantized[None, ...])
            interpreter.invoke()
            raw = interpreter.get_tensor(output_detail["index"])[0]
            scores = (raw.astype(np.float32) - output_zero) * output_scale
            predicted = int(np.argmax(scores))
            confusion[int(expected), predicted] += 1
    metrics = metrics_from_confusion(confusion, classes)
    metrics.update(
        {
        "input": {
            "shape": input_detail["shape"].tolist(),
            "dtype": str(input_detail["dtype"]),
            "scale": float(input_scale),
            "zero_point": int(input_zero),
        },
        "output": {
            "shape": output_detail["shape"].tolist(),
            "dtype": str(output_detail["dtype"]),
            "scale": float(output_scale),
            "zero_point": int(output_zero),
        },
        }
    )
    return metrics


def main() -> int:
    args = parse_args()
    tf = require_tensorflow()
    tf.keras.utils.set_random_seed(args.seed)
    try:
        tf.config.experimental.enable_op_determinism()
    except Exception:
        pass

    config = yaml.safe_load((ROOT / "vision_models.yaml").read_text(encoding="utf-8"))
    model_config = config[TASK_TO_CONFIG[args.task]]
    classes = list(args.classes or model_config["classes"])
    if len(classes) < 2 or len(set(classes)) != len(classes):
        raise SystemExit("--classes 至少需要两个互不重复的类别")
    unknown_classes = sorted(set(classes) - set(model_config["classes"]))
    if unknown_classes:
        raise SystemExit(
            f"--classes 含有不属于 {args.task} 合约的类别: {unknown_classes}"
        )
    task_root = args.data_root / args.task
    train_root = task_root / "train"
    val_root = task_root / "val"
    test_root = task_root / "test"
    train_counts = count_class_files(train_root, classes)
    print("训练集类别计数:", train_counts)

    train_ds = make_dataset(tf, train_root, classes, args.batch_size, args.seed, True)
    val_ds = make_dataset(tf, val_root, classes, args.batch_size, args.seed, False)
    evaluation_root = test_root if test_root.exists() and any(test_root.rglob("*.jpg")) else val_root
    evaluation_ds = make_dataset(tf, evaluation_root, classes, 1, args.seed, False)

    if args.initial_model:
        if not args.initial_model.is_file():
            raise SystemExit(f"Initial model does not exist: {args.initial_model}")
        model = tf.keras.models.load_model(args.initial_model)
        if model.output_shape[-1] != len(classes):
            raise SystemExit(
                f"Initial model has {model.output_shape[-1]} outputs; expected {len(classes)}"
            )
        base = next(
            (
                layer
                for layer in model.layers
                if isinstance(layer, tf.keras.Model) and "mobilenet" in layer.name.lower()
            ),
            None,
        )
        if base is None:
            raise SystemExit("Initial model does not contain a MobileNet base model")
    else:
        model, base = build_model(tf, len(classes), args.weights == "imagenet")
    output_dir = args.output / args.task
    output_dir.mkdir(parents=True, exist_ok=True)
    checkpoint = output_dir / "best_float.keras"
    callbacks = [
        tf.keras.callbacks.ModelCheckpoint(checkpoint, monitor="val_loss", save_best_only=True),
        tf.keras.callbacks.EarlyStopping(
            monitor="val_loss",
            patience=5,
            restore_best_weights=True,
        ),
        tf.keras.callbacks.ReduceLROnPlateau(
            monitor="val_loss",
            factor=0.3,
            patience=2,
            min_lr=1e-6,
        ),
    ]
    weights = class_weights_from_counts(classes, train_counts)
    if args.critical_weight_multiplier <= 0:
        raise SystemExit("--critical-weight-multiplier must be positive")
    critical_classes = [
        name for name in model_config.get("critical_classes", []) if name in classes
    ]
    for name in critical_classes:
        weights[classes.index(name)] *= args.critical_weight_multiplier
    head_history = None
    if not args.initial_model:
        model.compile(
            optimizer=tf.keras.optimizers.Adam(1e-3),
            loss="categorical_crossentropy",
            metrics=["accuracy"],
        )
        head_history = model.fit(
            train_ds,
            validation_data=val_ds,
            epochs=args.head_epochs,
            class_weight=weights,
            callbacks=callbacks,
        )

    base.trainable = True
    freeze_until = max(0, len(base.layers) - args.fine_layers)
    for layer in base.layers[:freeze_until]:
        layer.trainable = False
    for layer in base.layers[freeze_until:]:
        if isinstance(layer, tf.keras.layers.BatchNormalization):
            layer.trainable = False
    model.compile(
        optimizer=tf.keras.optimizers.Adam(1e-5),
        loss="categorical_crossentropy",
        metrics=["accuracy"],
    )
    fine_history = model.fit(
        train_ds,
        validation_data=val_ds,
        epochs=args.fine_epochs,
        class_weight=weights,
        callbacks=callbacks,
    )
    model = tf.keras.models.load_model(checkpoint)
    final_keras = output_dir / f"{args.task}_mobilenetv3_small_192.keras"
    model.save(final_keras)

    int8_path = output_dir / f"{args.task}_mobilenetv3_small_192_int8.tflite"
    export_int8(tf, model, train_ds, int8_path, args.representative_samples)
    keras_metrics = model.evaluate(evaluation_ds, verbose=0, return_dict=True)
    float_metrics = evaluate_float(model, evaluation_ds, classes)
    float_metrics["keras_metrics"] = {
        name: float(value) for name, value in keras_metrics.items()
    }
    int8_metrics = evaluate_tflite(tf, int8_path, evaluation_ds, classes)
    recall_delta = {
        name: (
            int8_metrics["per_class"][name]["recall"]
            - float_metrics["per_class"][name]["recall"]
        )
        if (
            int8_metrics["per_class"][name]["recall"] is not None
            and float_metrics["per_class"][name]["recall"] is not None
        )
        else None
        for name in classes
    }
    report = {
        "task": args.task,
        "classes": classes,
        "prototype_class_override": args.classes is not None,
        "deployment_contract_classes": list(model_config["classes"]),
        "architecture": "MobileNetV3Small(minimalistic=True, include_preprocessing=True)",
        "input_contract": "RGB uint8 pixels 0..255; TFLite int8 quantization in model metadata",
        "train_counts": train_counts,
        "class_weights": weights,
        "critical_weight_multiplier": args.critical_weight_multiplier,
        "initial_model": str(args.initial_model) if args.initial_model else None,
        "evaluation_source": str(evaluation_root),
        "float_metrics": float_metrics,
        "int8_metrics": int8_metrics,
        "quantization_delta": {
            "accuracy": (
                int8_metrics["accuracy"] - float_metrics["accuracy"]
                if int8_metrics["accuracy"] is not None and float_metrics["accuracy"] is not None
                else None
            ),
            "recall_by_class": recall_delta,
            "critical_classes": critical_classes,
        },
        "int8_bytes": int8_path.stat().st_size,
        "history": {
            "head": (
                {name: [float(value) for value in values] for name, values in head_history.history.items()}
                if head_history is not None
                else {}
            ),
            "fine": {name: [float(value) for value in values] for name, values in fine_history.history.items()},
        },
    }
    report_path = output_dir / "training_report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"INT8 模型: {int8_path}")
    print(f"报告: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import argparse
import json
import shutil
from collections import Counter
from pathlib import Path
from typing import Any

import numpy as np
import tensorflow as tf
import yaml
from sklearn.metrics import (
    accuracy_score,
    balanced_accuracy_score,
    classification_report,
    confusion_matrix,
    f1_score,
)

from audio_pipeline.common import sha256_file
from models import BUILDERS


ROOT = Path(__file__).resolve().parent


def load_spec(name: str) -> dict:
    with (ROOT / "model_specs.yaml").open(encoding="utf-8") as handle:
        specs = yaml.safe_load(handle)["models"]
    if name not in specs:
        raise KeyError(f"model {name!r} has no model_specs.yaml entry")
    return specs[name]


def load_npz(
    path: Path,
    shape: tuple[int, ...],
    expected_classes: list[str],
) -> tuple[np.ndarray, np.ndarray, dict[str, np.ndarray]]:
    with np.load(path) as data:
        x = np.asarray(data["x"], dtype=np.float32)
        y = np.asarray(data["y"], dtype=np.int32)
        metadata = {
            key: np.asarray(data[key])
            for key in (
                "source",
                "recording_id",
                "window_rms_dbfs",
                "recording_duration_seconds",
                "augmentation_kind",
                "augmented",
            )
            if key in data
        }
        if "classes" in data:
            actual_classes = [str(value) for value in data["classes"]]
            if actual_classes != expected_classes:
                raise ValueError(
                    f"{path}: class order {actual_classes} does not match {expected_classes}"
                )
    if x.shape[1:] != shape:
        raise ValueError(f"{path}: expected x shape [N,{shape}], got {x.shape}")
    if len(x) != len(y):
        raise ValueError(f"{path}: x/y sample counts differ")
    if len(y) == 0:
        raise ValueError(f"{path}: dataset is empty")
    if y.min() < 0 or y.max() >= len(expected_classes):
        raise ValueError(f"{path}: class IDs are outside the configured range")
    return x, y, metadata


def verify_feature_contract(path: Path, expected_sha256: str) -> None:
    with np.load(path) as data:
        if "preprocessing_sha256" not in data.files:
            raise ValueError(
                f"{path}: preprocessing_sha256 is missing; rebuild feature packages"
            )
        actual = str(np.asarray(data["preprocessing_sha256"]).item())
    if actual != expected_sha256:
        raise ValueError(
            f"{path}: preprocessing contract mismatch: {actual} != {expected_sha256}"
        )


def balanced_class_weights(y: np.ndarray, class_count: int) -> dict[int, float]:
    counts = np.bincount(y, minlength=class_count)
    missing = np.flatnonzero(counts == 0)
    if len(missing):
        raise ValueError(f"training data is missing class IDs {missing.tolist()}")
    total = float(len(y))
    return {
        index: total / (class_count * float(count))
        for index, count in enumerate(counts)
    }


def filter_sources(
    x: np.ndarray,
    y: np.ndarray,
    metadata: dict[str, np.ndarray],
    requested_sources: list[str],
    split_name: str,
) -> tuple[np.ndarray, np.ndarray, dict[str, np.ndarray]]:
    if not requested_sources:
        return x, y, metadata
    sources = metadata.get("source")
    if sources is None or len(sources) != len(y):
        raise ValueError(
            f"{split_name} NPZ has no source metadata for source filtering"
        )
    source_names = sources.astype(str)
    requested = set(requested_sources)
    available = set(source_names)
    missing = sorted(requested - available)
    if missing:
        raise ValueError(
            f"{split_name} source filter did not match {missing}; "
            f"available sources: {sorted(available)}"
        )
    mask = np.isin(source_names, sorted(requested))
    if not np.any(mask):
        raise ValueError(f"{split_name} source filter produced an empty dataset")
    filtered_metadata = {
        key: values[mask] if len(values) == len(mask) else values
        for key, values in metadata.items()
    }
    return x[mask], y[mask], filtered_metadata


def evaluation_report(
    model: tf.keras.Model,
    x: np.ndarray,
    y: np.ndarray,
    classes: list[str],
) -> dict:
    probabilities = model.predict(x, batch_size=256, verbose=0)
    predictions = probabilities.argmax(axis=1)
    return {
        "samples": int(len(y)),
        "accuracy": float(accuracy_score(y, predictions)),
        "balanced_accuracy": float(balanced_accuracy_score(y, predictions)),
        "macro_f1": float(f1_score(y, predictions, average="macro", zero_division=0)),
        "confusion_matrix": confusion_matrix(
            y, predictions, labels=np.arange(len(classes))
        ).tolist(),
        "per_class": classification_report(
            y,
            predictions,
            labels=np.arange(len(classes)),
            target_names=classes,
            output_dict=True,
            zero_division=0,
        ),
    }


def evaluation_reports_by_source(
    model: tf.keras.Model,
    x: np.ndarray,
    y: np.ndarray,
    metadata: dict[str, np.ndarray],
    classes: list[str],
) -> dict[str, Any]:
    sources = metadata.get("source")
    if sources is None or len(sources) != len(y):
        return {}
    result: dict[str, Any] = {}
    for source in sorted({str(value) for value in sources}):
        indices = np.flatnonzero(sources.astype(str) == source)
        result[source] = evaluation_report(model, x[indices], y[indices], classes)
    return result


def evaluation_reports_by_slice(
    model: tf.keras.Model,
    x: np.ndarray,
    y: np.ndarray,
    metadata: dict[str, np.ndarray],
    classes: list[str],
) -> dict[str, Any]:
    masks: dict[str, np.ndarray] = {"all": np.ones(len(y), dtype=bool)}
    durations = metadata.get("recording_duration_seconds")
    if durations is not None and len(durations) == len(y):
        masks["recording_under_1s"] = durations.astype(np.float32) < 1.0
    rms_values = metadata.get("window_rms_dbfs")
    if rms_values is not None and len(rms_values) == len(y):
        masks["window_below_minus_35_dbfs"] = rms_values.astype(np.float32) < -35.0
    augmented = metadata.get("augmented")
    if augmented is not None and len(augmented) == len(y):
        masks["unaugmented"] = augmented.astype(np.uint8) == 0
    return {
        name: evaluation_report(model, x[mask], y[mask], classes)
        for name, mask in masks.items()
        if np.any(mask)
    }


def parse_source_weights(values: list[str]) -> dict[str, float]:
    result: dict[str, float] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"source weight must use SOURCE=WEIGHT, got {value!r}")
        source, raw_weight = value.rsplit("=", 1)
        source = source.strip()
        weight = float(raw_weight)
        if not source or weight <= 0.0:
            raise ValueError(f"invalid source weight {value!r}")
        result[source] = weight
    return result


def parse_source_class_weights(
    values: list[str],
    classes: list[str],
) -> dict[tuple[str, str], float]:
    result: dict[tuple[str, str], float] = {}
    for value in values:
        if "=" not in value or ":" not in value.rsplit("=", 1)[0]:
            raise ValueError(
                f"source/class weight must use SOURCE:CLASS=WEIGHT, got {value!r}"
            )
        key, raw_weight = value.rsplit("=", 1)
        source, class_name = key.rsplit(":", 1)
        source = source.strip()
        class_name = class_name.strip()
        weight = float(raw_weight)
        if not source or class_name not in classes or weight <= 0.0:
            raise ValueError(f"invalid source/class weight {value!r}")
        result[(source, class_name)] = weight
    return result


def representative_samples(
    x: np.ndarray,
    y: np.ndarray,
    class_count: int,
    maximum: int,
    seed: int,
) -> np.ndarray:
    rng = np.random.default_rng(seed)
    selected: list[int] = []
    quota = max(1, maximum // class_count)
    for class_id in range(class_count):
        indices = np.flatnonzero(y == class_id)
        rng.shuffle(indices)
        selected.extend(indices[:quota].tolist())
    if len(selected) < min(maximum, len(x)):
        remaining = np.setdiff1d(np.arange(len(x)), np.asarray(selected), assume_unique=False)
        rng.shuffle(remaining)
        selected.extend(remaining[: maximum - len(selected)].tolist())
    rng.shuffle(selected)
    return np.asarray(x[selected[:maximum]], dtype=np.float32)


def main() -> None:
    parser = argparse.ArgumentParser(description="Train a deployment-sized RA8P1 model")
    parser.add_argument("model", choices=sorted(BUILDERS))
    parser.add_argument("--train", type=Path, required=True, help="NPZ containing x and integer y")
    parser.add_argument("--val", type=Path, required=True)
    parser.add_argument("--test", type=Path)
    parser.add_argument("--epochs", type=int, default=80)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--learning-rate", type=float, default=1.0e-3)
    parser.add_argument("--seed", type=int, default=20260724)
    parser.add_argument("--no-class-weights", action="store_true")
    parser.add_argument(
        "--train-source",
        action="append",
        default=[],
        metavar="SOURCE",
        help="Keep only this source in the training NPZ (may be repeated)",
    )
    parser.add_argument(
        "--validation-source",
        action="append",
        default=[],
        metavar="SOURCE",
        help="Keep only this source in the validation NPZ (may be repeated)",
    )
    parser.add_argument(
        "--test-source",
        action="append",
        default=[],
        metavar="SOURCE",
        help="Keep only this source in the test NPZ (may be repeated)",
    )
    parser.add_argument(
        "--source-weight",
        action="append",
        default=[],
        metavar="SOURCE=WEIGHT",
        help="Multiply the training weight for one source (may be repeated)",
    )
    parser.add_argument(
        "--source-class-weight",
        action="append",
        default=[],
        metavar="SOURCE:CLASS=WEIGHT",
        help="Additional multiplier for one class from one source (may be repeated)",
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    tf.keras.utils.set_random_seed(args.seed)
    spec = load_spec(args.model)
    shape = tuple(int(value) for value in spec["input_shape"])
    classes = list(spec["classes"])
    preprocessing = spec.get("preprocessing_contract")
    if not preprocessing:
        raise ValueError(f"model {args.model!r} has no preprocessing contract")
    contract_path = ROOT / preprocessing
    if not contract_path.exists():
        raise FileNotFoundError(f"preprocessing contract is missing: {contract_path}")
    contract_sha256 = sha256_file(contract_path)
    test_path = args.test or args.val
    for feature_path in (args.train, args.val, test_path):
        verify_feature_contract(feature_path, contract_sha256)
    x_train, y_train, train_metadata = load_npz(args.train, shape, classes)
    x_val, y_val, val_metadata = load_npz(args.val, shape, classes)
    x_test, y_test, test_metadata = load_npz(test_path, shape, classes)
    x_train, y_train, train_metadata = filter_sources(
        x_train,
        y_train,
        train_metadata,
        args.train_source,
        "training",
    )
    x_val, y_val, val_metadata = filter_sources(
        x_val,
        y_val,
        val_metadata,
        args.validation_source,
        "validation",
    )
    x_test, y_test, test_metadata = filter_sources(
        x_test,
        y_test,
        test_metadata,
        args.test_source,
        "test",
    )
    for split_name, labels in (
        ("training", y_train),
        ("validation", y_val),
        ("test", y_test),
    ):
        missing_classes = np.flatnonzero(
            np.bincount(labels, minlength=len(classes)) == 0
        )
        if len(missing_classes):
            raise ValueError(
                f"{split_name} source filter removed classes "
                f"{[classes[index] for index in missing_classes]}"
            )

    model = BUILDERS[args.model](shape, len(classes))
    model.compile(
        optimizer=tf.keras.optimizers.Adam(args.learning_rate),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    output = (args.output or ROOT / "artifacts" / args.model).resolve()
    output.mkdir(parents=True, exist_ok=True)
    frozen_contract = output / "preprocessing_contract.json"
    frozen_config = output / "audio_two_stage.yaml"
    shutil.copy2(contract_path, frozen_contract)
    shutil.copy2(ROOT / "audio_two_stage.yaml", frozen_config)
    callbacks = [
        tf.keras.callbacks.EarlyStopping(
            monitor="val_loss", patience=12, restore_best_weights=True
        ),
        tf.keras.callbacks.ReduceLROnPlateau(
            monitor="val_loss", patience=5, factor=0.5, min_lr=1.0e-6
        ),
        tf.keras.callbacks.ModelCheckpoint(
            output / "best.keras", monitor="val_loss", save_best_only=True
        ),
        tf.keras.callbacks.CSVLogger(output / "training_log.csv"),
    ]
    class_weights = (
        None
        if args.no_class_weights
        else balanced_class_weights(y_train, len(classes))
    )
    source_weights = parse_source_weights(args.source_weight)
    source_class_weights = parse_source_class_weights(args.source_class_weight, classes)
    sample_weights = np.ones(len(y_train), dtype=np.float32)
    if class_weights:
        sample_weights *= np.asarray(
            [class_weights[int(class_id)] for class_id in y_train],
            dtype=np.float32,
        )
    train_sources = train_metadata.get("source")
    if (source_weights or source_class_weights) and train_sources is None:
        raise ValueError("training NPZ has no source metadata for source weighting")
    if train_sources is not None:
        source_names = train_sources.astype(str)
        missing_sources = sorted(set(source_weights) - set(source_names))
        if missing_sources:
            raise ValueError(f"source weights did not match training data: {missing_sources}")
        for source, weight in source_weights.items():
            sample_weights[source_names == source] *= np.float32(weight)
        for (source, class_name), weight in source_class_weights.items():
            if source not in set(source_names):
                raise ValueError(f"source/class weight did not match source {source!r}")
            class_id = classes.index(class_name)
            sample_weights[(source_names == source) & (y_train == class_id)] *= np.float32(weight)
    history = model.fit(
        x_train,
        y_train,
        validation_data=(x_val, y_val),
        epochs=args.epochs,
        batch_size=args.batch_size,
        callbacks=callbacks,
        sample_weight=sample_weights,
        verbose=2,
    )
    best_model = tf.keras.models.load_model(output / "best.keras")
    saved_model = output / "saved_model"
    if saved_model.exists():
        shutil.rmtree(saved_model)
    best_model.export(saved_model)

    representative = representative_samples(
        x_train, y_train, len(classes), maximum=500, seed=args.seed
    )
    np.save(output / "representative.npy", representative)
    history_json = {
        key: [float(value) for value in values]
        for key, values in history.history.items()
    }
    (output / "history.json").write_text(
        json.dumps(history_json, indent=2), encoding="utf-8"
    )

    report = {
        "model": args.model,
        "classes": classes,
        "input_shape": list(shape),
        "parameters": int(best_model.count_params()),
        "train_samples": int(len(y_train)),
        "validation_samples": int(len(y_val)),
        "test_samples": int(len(y_test)),
        "source_filters": {
            "train": args.train_source,
            "validation": args.validation_source,
            "test": args.test_source,
        },
        "train_class_counts": {
            classes[index]: int(count)
            for index, count in enumerate(np.bincount(y_train, minlength=len(classes)))
        },
        "class_weights": (
            {classes[index]: float(value) for index, value in class_weights.items()}
            if class_weights
            else None
        ),
        "source_weights": source_weights,
        "source_class_weights": {
            f"{source}:{class_name}": weight
            for (source, class_name), weight in source_class_weights.items()
        },
        "test": evaluation_report(best_model, x_test, y_test, classes),
        "test_by_source": evaluation_reports_by_source(
            best_model, x_test, y_test, test_metadata, classes
        ),
        "test_slices": evaluation_reports_by_slice(
            best_model, x_test, y_test, test_metadata, classes
        ),
        "validation_by_source": evaluation_reports_by_source(
            best_model, x_val, y_val, val_metadata, classes
        ),
        "validation_slices": evaluation_reports_by_slice(
            best_model, x_val, y_val, val_metadata, classes
        ),
        "feature_packages": {
            "train": {"path": str(args.train), "sha256": sha256_file(args.train)},
            "validation": {"path": str(args.val), "sha256": sha256_file(args.val)},
            "test": {"path": str(test_path), "sha256": sha256_file(test_path)},
        },
        "preprocessing_contract": frozen_contract.name,
        "preprocessing_sha256": contract_sha256,
        "training_config": frozen_config.name,
        "training_config_sha256": sha256_file(frozen_config),
    }
    (output / "training_report.json").write_text(
        json.dumps(report, indent=2), encoding="utf-8"
    )
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

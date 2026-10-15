from __future__ import annotations

import csv
import json
from collections import Counter, defaultdict
from functools import lru_cache
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf
from scipy.signal import lfilter

from .common import (
    DATA_ROOT,
    read_csv,
    resolve_data_path,
    sha256_file,
    stable_id,
    write_csv,
    write_json,
)
from .features import CoefficientStats, dbfs, extract_mfcc_batch, normalize_features


WINDOW_FIELDS = [
    "task",
    "split",
    "recording_id",
    "source",
    "label",
    "class_id",
    "window_start_samples",
    "window_start_seconds",
    "augmented",
    "augmentation_kind",
    "window_rms_dbfs",
    "recording_duration_seconds",
]


def _window_offsets(sample_count: int, config: dict[str, Any]) -> list[int]:
    audio = config["audio"]
    window = int(audio["window_samples"])
    stride = int(audio["stride_samples"])
    maximum = int(audio["max_windows_per_recording"])
    if sample_count <= window:
        return [0]
    offsets = list(range(0, sample_count - window + 1, stride))
    last = sample_count - window
    if offsets[-1] != last:
        offsets.append(last)
    if len(offsets) > maximum:
        indices = np.linspace(0, len(offsets) - 1, maximum)
        offsets = [offsets[int(round(index))] for index in indices]
        offsets = list(dict.fromkeys(offsets))
    return offsets


def _slice_window(samples: np.ndarray, offset: int, window_samples: int) -> np.ndarray:
    values = np.asarray(samples, dtype=np.float32).reshape(-1)
    result = values[offset: offset + window_samples]
    if len(result) < window_samples:
        result = np.pad(result, (0, window_samples - len(result)))
    return np.asarray(result, dtype=np.float32)


def _load_audio(path: Path, expected_sample_rate: int) -> np.ndarray:
    samples, sample_rate = sf.read(path, dtype="float32", always_2d=False)
    if sample_rate != expected_sample_rate:
        raise ValueError(f"{path}: expected {expected_sample_rate} Hz, got {sample_rate}")
    values = np.asarray(samples, dtype=np.float32)
    if values.ndim != 1:
        raise ValueError(f"{path}: standardized audio must be mono")
    return values


def _augment_window(
    window: np.ndarray,
    rng: np.random.Generator,
    noise_window: np.ndarray | None,
    sample_rate: int,
    snr_range: tuple[float, float],
) -> np.ndarray:
    values = np.asarray(window, dtype=np.float32).copy()
    shift = int(rng.integers(-sample_rate // 10, sample_rate // 10 + 1))
    if shift > 0:
        values[shift:] = values[:-shift]
        values[:shift] = 0.0
    elif shift < 0:
        amount = -shift
        values[:-amount] = values[amount:]
        values[-amount:] = 0.0

    values *= np.float32(10.0 ** (rng.uniform(-6.0, 6.0) / 20.0))
    noise = (
        np.asarray(noise_window, dtype=np.float32)
        if noise_window is not None
        else rng.normal(0.0, 1.0, size=values.shape).astype(np.float32)
    )
    signal_rms = float(np.sqrt(np.mean(np.square(values), dtype=np.float64)))
    noise_rms = float(np.sqrt(np.mean(np.square(noise), dtype=np.float64)))
    if signal_rms > 1.0e-7 and noise_rms > 1.0e-7:
        snr_db = float(rng.uniform(*snr_range))
        scale = signal_rms / (noise_rms * 10.0 ** (snr_db / 20.0))
        values += noise * np.float32(scale)
    return np.clip(values, -1.0, 1.0).astype(np.float32, copy=False)


def _simulate_device_capture(
    window: np.ndarray,
    config: dict[str, Any],
) -> np.ndarray:
    """Approximate the CPU0/CPU1 capture filter before the shared MFCC stage."""
    settings = config["augmentation"]
    pcm = np.rint(np.clip(window, -1.0, 1.0) * 32767.0).astype(np.float64)
    centered = lfilter([1.0, -1.0], [1.0, -(255.0 / 256.0)], pcm)
    centered *= 4.0
    # The firmware's limiter is only active on unusually large discontinuities.
    # A vectorized delta clamp preserves that behavior closely enough for domain
    # augmentation without making feature generation hours slower.
    deltas = np.diff(np.concatenate(([0.0], centered)))
    limited = np.cumsum(np.clip(deltas, -4096.0, 4096.0))
    limited = np.clip(limited, -32768.0, 32767.0)
    hp_alpha = 31506.0 / 32768.0
    lp_alpha = 25956.0 / 32768.0
    high_pass = lfilter([hp_alpha, -hp_alpha], [1.0, -hp_alpha], limited)
    output = lfilter([lp_alpha], [1.0, -(1.0 - lp_alpha)], high_pass)

    block_samples = int(settings["device_block_samples"])
    target_rms = float(settings["device_block_target_rms"]) * 32768.0
    minimum_gain = float(settings["device_block_minimum_gain"])
    maximum_gain = float(settings["device_block_maximum_gain"])
    for start in range(0, len(output), block_samples):
        block = output[start: start + block_samples]
        rms = float(np.sqrt(np.mean(np.square(block), dtype=np.float64)))
        if rms > 1.0e-9:
            gain = float(np.clip(target_rms / rms, minimum_gain, maximum_gain))
            block *= gain
    return (np.clip(output, -32768.0, 32767.0) / 32768.0).astype(np.float32)


def _short_cry_mix(
    cry_window: np.ndarray,
    background_window: np.ndarray | None,
    rng: np.random.Generator,
    sample_rate: int,
    config: dict[str, Any],
) -> np.ndarray:
    settings = config["augmentation"]
    minimum_ms, maximum_ms = settings["short_cry_duration_ms"]
    duration = int(round(rng.uniform(float(minimum_ms), float(maximum_ms)) * sample_rate / 1000.0))
    duration = max(1, min(duration, len(cry_window)))
    values = np.asarray(cry_window, dtype=np.float32)
    squared = np.square(values, dtype=np.float64)
    cumulative = np.concatenate(([0.0], np.cumsum(squared, dtype=np.float64)))
    energy = cumulative[duration:] - cumulative[:-duration]
    source_start = int(np.argmax(energy)) if len(energy) else 0
    event = values[source_start: source_start + duration].copy()
    fade = min(duration // 4, max(1, sample_rate // 100))
    ramp = np.linspace(0.0, 1.0, fade, endpoint=False, dtype=np.float32)
    event[:fade] *= ramp
    event[-fade:] *= ramp[::-1]

    if background_window is None:
        mixed = rng.normal(0.0, 0.003, size=values.shape).astype(np.float32)
    else:
        mixed = np.asarray(background_window, dtype=np.float32).copy()
    target_start = int(rng.integers(0, len(mixed) - duration + 1))
    noise_region = mixed[target_start: target_start + duration]
    event_rms = float(np.sqrt(np.mean(np.square(event), dtype=np.float64)))
    noise_rms = float(np.sqrt(np.mean(np.square(noise_region), dtype=np.float64)))
    if noise_rms < 1.0e-6:
        noise_rms = 1.0e-6
    if event_rms > 1.0e-7:
        snr_db = float(rng.uniform(*settings["short_cry_snr_db"]))
        event *= np.float32(noise_rms * 10.0 ** (snr_db / 20.0) / event_rms)
    mixed[target_start: target_start + duration] += event
    return np.clip(mixed, -1.0, 1.0).astype(np.float32, copy=False)


def _config_digest(config: dict[str, Any]) -> str:
    clean = {key: value for key, value in config.items() if not key.startswith("_")}
    return stable_id(json.dumps(clean, ensure_ascii=False, sort_keys=True), length=32)


def build_feature_packages(
    manifest_path: Path,
    config: dict[str, Any],
    data_root: Path = DATA_ROOT,
    *,
    augment_copies: int | None = None,
    device_chain_copies: int | None = None,
    short_cry_copies: int | None = None,
    tasks: list[str] | None = None,
    limit_recordings: int | None = None,
) -> dict[str, Any]:
    rows = [
        row for row in read_csv(manifest_path)
        if row["quality_status"] == "accepted" and row["split"] in {"train", "validation", "test"}
    ]
    if limit_recordings:
        rows = rows[:limit_recordings]
    if not rows:
        raise ValueError(f"no accepted recordings in {manifest_path}")

    augmentation = config.get("augmentation", {})
    generic_copies = int(
        augmentation.get("generic_copies", 0)
        if augment_copies is None
        else augment_copies
    )
    device_copies = int(
        augmentation.get("device_chain_copies", 0)
        if device_chain_copies is None
        else device_chain_copies
    )
    short_copies = int(
        augmentation.get("short_cry_copies", 0)
        if short_cry_copies is None
        else short_cry_copies
    )
    if min(generic_copies, device_copies, short_copies) < 0:
        raise ValueError("augmentation copy counts must be non-negative")
    selected_tasks = list(tasks or config["tasks"])
    unknown_tasks = sorted(set(selected_tasks) - set(config["tasks"]))
    if unknown_tasks:
        raise ValueError(f"unknown feature task(s): {unknown_tasks}")

    sample_rate = int(config["audio"]["sample_rate_hz"])
    window_samples = int(config["audio"]["window_samples"])
    minimum_window_dbfs = float(config["quality"]["minimum_window_rms_dbfs"])
    coefficient_count = int(config["mfcc"]["mfcc_coefficients"])
    stats = CoefficientStats(coefficient_count)
    raw_features: dict[tuple[str, int], np.ndarray] = {}
    offsets_by_recording: dict[str, list[int]] = {}
    row_by_id = {row["recording_id"]: row for row in rows}
    audio_by_id: dict[str, np.ndarray] = {}
    skipped_silent = 0

    for row_index, row in enumerate(rows, start=1):
        path = resolve_data_path(row["standardized_path"], data_root)
        samples = _load_audio(path, sample_rate)
        audio_by_id[row["recording_id"]] = samples
        valid_offsets: list[int] = []
        valid_windows: list[np.ndarray] = []
        for offset in _window_offsets(len(samples), config):
            window = _slice_window(samples, offset, window_samples)
            if (
                dbfs(window) < minimum_window_dbfs
                and row["detection_label"] == "cry"
            ):
                skipped_silent += 1
                continue
            valid_windows.append(window)
            valid_offsets.append(offset)
        if valid_windows:
            recording_features = extract_mfcc_batch(np.stack(valid_windows), config)
        else:
            recording_features = np.empty(
                (0, *tuple(config["mfcc"]["expected_shape"])), dtype=np.float32
            )
        for offset, features in zip(valid_offsets, recording_features):
            raw_features[(row["recording_id"], offset)] = features
            if row["split"] == "train":
                stats.update(features)
        offsets_by_recording[row["recording_id"]] = valid_offsets
        if row_index % 100 == 0 or row_index == len(rows):
            print(
                f"extracted base MFCC {row_index}/{len(rows)} recordings",
                flush=True,
            )

    mean, std = stats.finish(float(config["normalization"]["feature_std_floor"]))
    contract = {
        "version": 1,
        "config_sha256_prefix": _config_digest(config),
        "sample_rate_hz": sample_rate,
        "window_samples": window_samples,
        "stride_samples": int(config["audio"]["stride_samples"]),
        "mfcc": {
            key: value
            for key, value in config["mfcc"].items()
        },
        "waveform_normalization": dict(config["normalization"]),
        "coefficient_mean": [float(value) for value in mean],
        "coefficient_std": [float(value) for value in std],
        "shared_by_models": ["cry_detection", "cry_type"],
    }
    feature_root = data_root / "features"
    contract_path = feature_root / "preprocessing_contract.json"
    write_json(contract_path, contract)
    contract_sha256 = sha256_file(contract_path)

    negative_train = [
        row for row in rows
        if row["split"] == "train"
        and row["detection_label"] == "non_cry"
        and offsets_by_recording.get(row["recording_id"])
    ]

    task_summary: dict[str, Any] = {}
    for task_name in selected_tasks:
        task_config = config["tasks"][task_name]
        classes = list(task_config["classes"])
        excluded_classes = set(task_config.get("exclude_classes", []))
        class_to_id = {label: index for index, label in enumerate(classes)}
        label_field = "detection_label" if task_name == "cry_detection" else "classification_label"
        task_summary[task_name] = {}
        for split in ("train", "validation", "test"):
            x_values: list[np.ndarray] = []
            y_values: list[int] = []
            recording_ids: list[str] = []
            starts: list[int] = []
            sources: list[str] = []
            augmented_values: list[int] = []
            augmentation_kinds: list[str] = []
            window_rms_values: list[float] = []
            recording_duration_values: list[float] = []
            window_rows: list[dict[str, str | int | float]] = []

            for row in rows:
                label = row[label_field]
                if row["split"] != split or not label:
                    continue
                if label not in class_to_id:
                    if label in excluded_classes:
                        continue
                    raise ValueError(f"{task_name}: unknown label {label!r}")
                row_entries: list[tuple[np.ndarray, int, str, float]] = []
                row_augmented_waves: list[np.ndarray] = []
                row_augmented_metadata: list[tuple[int, str, float]] = []
                for offset in offsets_by_recording.get(row["recording_id"], []):
                    base = raw_features[(row["recording_id"], offset)]
                    normalized = normalize_features(base, mean, std)
                    source_window = _slice_window(
                        audio_by_id[row["recording_id"]], offset, window_samples
                    )
                    row_entries.append((normalized, offset, "base", dbfs(source_window)))
                    if split != "train":
                        continue

                    def make_rng(kind: str, copy_index: int) -> np.random.Generator:
                        seed = int(
                            stable_id(
                                config["split"]["seed"],
                                task_name,
                                row["recording_id"],
                                offset,
                                kind,
                                copy_index,
                                length=16,
                            ),
                            16,
                        )
                        return np.random.default_rng(seed)

                    def select_noise(rng: np.random.Generator) -> np.ndarray | None:
                        if not negative_train:
                            return None
                        noise_row = negative_train[int(rng.integers(0, len(negative_train)))]
                        noise_offsets = offsets_by_recording[noise_row["recording_id"]]
                        noise_offset = noise_offsets[int(rng.integers(0, len(noise_offsets)))]
                        return _slice_window(
                            audio_by_id[noise_row["recording_id"]],
                            noise_offset,
                            window_samples,
                        )

                    generic_snr = tuple(
                        float(value) for value in augmentation.get("generic_snr_db", [8.0, 30.0])
                    )
                    for copy_index in range(generic_copies):
                        rng = make_rng("generic", copy_index)
                        wave = _augment_window(
                            source_window,
                            rng,
                            select_noise(rng),
                            sample_rate,
                            generic_snr,
                        )
                        row_augmented_waves.append(wave)
                        row_augmented_metadata.append((offset, "generic", dbfs(wave)))

                    if not row["source"].startswith("device_selected_"):
                        for copy_index in range(device_copies):
                            rng = make_rng("device_chain", copy_index)
                            wave = _augment_window(
                                source_window,
                                rng,
                                select_noise(rng),
                                sample_rate,
                                generic_snr,
                            )
                            wave = _simulate_device_capture(wave, config)
                            row_augmented_waves.append(wave)
                            row_augmented_metadata.append((offset, "device_chain", dbfs(wave)))

                    if task_name == "cry_detection" and label == "cry":
                        for copy_index in range(short_copies):
                            rng = make_rng("short_cry", copy_index)
                            wave = _short_cry_mix(
                                source_window,
                                select_noise(rng),
                                rng,
                                sample_rate,
                                config,
                            )
                            wave = _simulate_device_capture(wave, config)
                            row_augmented_waves.append(wave)
                            row_augmented_metadata.append((offset, "short_cry_device", dbfs(wave)))

                if row_augmented_waves:
                    augmented_features = extract_mfcc_batch(
                        np.stack(row_augmented_waves), config
                    )
                    for features, (offset, kind, rms_value) in zip(
                        augmented_features, row_augmented_metadata
                    ):
                        row_entries.append(
                            (normalize_features(features, mean, std), offset, kind, rms_value)
                        )

                for features, offset, augmentation_kind, window_rms in row_entries:
                        augmented = int(augmentation_kind != "base")
                        x_values.append(features)
                        y_values.append(class_to_id[label])
                        recording_ids.append(row["recording_id"])
                        starts.append(offset)
                        sources.append(row["source"])
                        augmented_values.append(augmented)
                        augmentation_kinds.append(augmentation_kind)
                        window_rms_values.append(window_rms)
                        recording_duration = float(row.get("duration_seconds") or 0.0)
                        recording_duration_values.append(recording_duration)
                        window_rows.append(
                            {
                                "task": task_name,
                                "split": split,
                                "recording_id": row["recording_id"],
                                "source": row["source"],
                                "label": label,
                                "class_id": class_to_id[label],
                                "window_start_samples": offset,
                                "window_start_seconds": f"{offset / sample_rate:.3f}",
                                "augmented": augmented,
                                "augmentation_kind": augmentation_kind,
                                "window_rms_dbfs": f"{window_rms:.4f}",
                                "recording_duration_seconds": f"{recording_duration:.6f}",
                            }
                        )

            shape = tuple(int(value) for value in config["mfcc"]["expected_shape"])
            x = (
                np.stack(x_values).astype(np.float32)
                if x_values
                else np.empty((0, *shape), dtype=np.float32)
            )
            y = np.asarray(y_values, dtype=np.int32)
            output = feature_root / f"{task_name}_{split}.npz"
            np.savez_compressed(
                output,
                x=x,
                y=y,
                recording_id=np.asarray(recording_ids),
                window_start_samples=np.asarray(starts, dtype=np.int32),
                source=np.asarray(sources),
                augmented=np.asarray(augmented_values, dtype=np.uint8),
                augmentation_kind=np.asarray(augmentation_kinds),
                window_rms_dbfs=np.asarray(window_rms_values, dtype=np.float32),
                recording_duration_seconds=np.asarray(
                    recording_duration_values, dtype=np.float32
                ),
                classes=np.asarray(classes),
                preprocessing_sha256=np.asarray(contract_sha256),
                config_sha256_prefix=np.asarray(contract["config_sha256_prefix"]),
            )
            write_csv(
                data_root / "manifests" / "windows" / f"{task_name}_{split}.csv",
                window_rows,
                WINDOW_FIELDS,
            )
            counts = Counter(classes[index] for index in y_values)
            task_summary[task_name][split] = {
                "windows": int(len(y_values)),
                "recordings": len(set(recording_ids)),
                "class_windows": {label: counts[label] for label in classes},
                "npz": str(output),
            }

        missing_train = [
            label for label, count in task_summary[task_name]["train"]["class_windows"].items()
            if count == 0
        ]
        if missing_train:
            raise ValueError(f"{task_name}: training split has no samples for {missing_train}")

    summary = {
        "manifest": str(manifest_path),
        "accepted_recordings": len(rows),
        "raw_feature_windows": len(raw_features),
        "skipped_silent_windows": skipped_silent,
        "preprocessing_sha256": contract_sha256,
        "augmentation": {
            "generic_copies": generic_copies,
            "device_chain_copies": device_copies,
            "short_cry_copies": short_copies,
        },
        "tasks": task_summary,
    }
    write_json(feature_root / "feature_summary.json", summary)
    return summary

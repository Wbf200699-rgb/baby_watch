from __future__ import annotations

import argparse
import hashlib
import json
from collections import defaultdict
from pathlib import Path

import librosa
import numpy as np
import tensorflow as tf
import yaml
from sklearn.metrics import accuracy_score, confusion_matrix

from audio_pipeline.common import sha256_file
from audio_pipeline.decision import find_confirmed_cry


ROOT = Path(__file__).resolve().parent


def resolve_frozen_file(model_path: Path, explicit: Path | None, filename: str) -> Path:
    path = explicit or (model_path.resolve().parent / filename)
    if not path.exists():
        raise FileNotFoundError(
            f"{filename} is missing next to the model; retrain with the reproducible pipeline"
        )
    return path


def verify_contract_hash(path: Path, expected: str) -> None:
    with np.load(path) as data:
        if "preprocessing_sha256" not in data.files:
            raise ValueError(f"{path}: preprocessing_sha256 is missing")
        actual = str(np.asarray(data["preprocessing_sha256"]).item())
    if actual != expected:
        raise ValueError(f"{path}: preprocessing contract mismatch")


def quantize_per_output(weights: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    values = np.asarray(weights, dtype=np.float32)
    scales = np.max(np.abs(values), axis=0) / 127.0
    scales = np.maximum(scales, np.finfo(np.float32).eps).astype(np.float32)
    quantized = np.clip(np.rint(values / scales), -127, 127).astype(np.int8)
    return quantized, scales


def activation_scale(values: np.ndarray, percentile: float) -> float:
    limit = float(np.percentile(np.abs(values), percentile))
    return max(limit / 127.0, 1.0e-7)


def quantize_symmetric(values: np.ndarray, scale: float, *, relu: bool) -> np.ndarray:
    lower = 0 if relu else -127
    return np.clip(np.rint(values / scale), lower, 127).astype(np.int8)


def dense_quantized(
    inputs: np.ndarray,
    input_scale: float,
    weights: np.ndarray,
    weight_scales: np.ndarray,
    bias: np.ndarray,
) -> np.ndarray:
    accumulators = inputs.astype(np.int32) @ weights.astype(np.int32)
    return (
        accumulators.astype(np.float32)
        * np.float32(input_scale)
        * weight_scales.reshape(1, -1)
        + bias.reshape(1, -1)
    )


def format_c_array(
    name: str,
    values: np.ndarray,
    c_type: str,
    *,
    values_per_line: int = 12,
) -> str:
    flat = np.asarray(values).reshape(-1)
    rendered: list[str] = []
    if np.issubdtype(flat.dtype, np.floating):
        for value in flat:
            text = f"{float(value):.9g}"
            if ("." not in text) and ("e" not in text.lower()):
                text += ".0"
            rendered.append(text + "f")
    else:
        rendered = [str(int(value)) for value in flat]
    lines = [
        "    " + ", ".join(rendered[index:index + values_per_line])
        for index in range(0, len(rendered), values_per_line)
    ]
    return (
        f"static const {c_type} {name}[{len(flat)}] =\n"
        "{\n"
        + ",\n".join(lines)
        + "\n};\n"
    )


def temporal_report(
    probabilities: np.ndarray,
    arrays: dict[str, np.ndarray],
    threshold: float,
    consecutive: int,
    source: str | None,
) -> dict:
    mask = np.ones(len(probabilities), dtype=bool)
    if source:
        mask &= arrays["source"].astype(str) == source
    groups: dict[str, list[int]] = defaultdict(list)
    indices = np.flatnonzero(mask)
    for index in indices:
        groups[str(arrays["recording_id"][index])].append(int(index))
    truth: list[int] = []
    predicted: list[int] = []
    for group_indices in groups.values():
        group_indices.sort(key=lambda index: int(arrays["window_start_samples"][index]))
        labels = {int(arrays["y"][index]) for index in group_indices}
        if len(labels) != 1:
            raise ValueError("one recording has multiple labels")
        truth.append(next(iter(labels)))
        decision = find_confirmed_cry(
            probabilities[group_indices],
            threshold=threshold,
            consecutive_windows=consecutive,
        )
        predicted.append(int(decision["confirmed"]))
    matrix = confusion_matrix(truth, predicted, labels=[0, 1])
    return {
        "source": source,
        "recordings": len(groups),
        "confusion_matrix": matrix.tolist(),
    }


def generate_feature_constants(config: dict, contract: dict) -> dict[str, np.ndarray]:
    mfcc = config["mfcc"]
    frame_length = int(mfcc["frame_length_samples"])
    fft_length = int(mfcc["fft_length"])
    mel_bins = int(mfcc["mel_bins"])
    coefficients = int(mfcc["mfcc_coefficients"])
    sample_rate = int(config["audio"]["sample_rate_hz"])

    hann = librosa.filters.get_window(
        str(mfcc["window_function"]),
        frame_length,
        fftbins=bool(mfcc["window_periodic"]),
    ).astype(np.float32)
    mel = librosa.filters.mel(
        sr=sample_rate,
        n_fft=fft_length,
        n_mels=mel_bins,
        fmin=float(mfcc["lower_frequency_hz"]),
        fmax=float(mfcc["upper_frequency_hz"]),
        htk=str(mfcc["mel_formula"]).lower() == "htk",
        norm=str(mfcc["mel_normalization"]),
    ).astype(np.float32)
    mel_starts: list[int] = []
    mel_lengths: list[int] = []
    mel_values: list[float] = []
    for row in mel:
        nonzero = np.flatnonzero(row)
        if len(nonzero) == 0:
            raise ValueError("empty Mel band")
        start = int(nonzero[0])
        end = int(nonzero[-1]) + 1
        mel_starts.append(start)
        mel_lengths.append(end - start)
        mel_values.extend(float(value) for value in row[start:end])

    bins = np.arange(mel_bins, dtype=np.float64)
    dct = np.empty((coefficients, mel_bins), dtype=np.float32)
    for coefficient in range(coefficients):
        scale = np.sqrt(1.0 / mel_bins) if coefficient == 0 else np.sqrt(2.0 / mel_bins)
        dct[coefficient] = (
            scale
            * np.cos(np.pi / mel_bins * (bins + 0.5) * coefficient)
        ).astype(np.float32)

    angle = -2.0 * np.pi * np.arange(fft_length // 2) / fft_length
    return {
        "hann": hann,
        "mel_starts": np.asarray(mel_starts, dtype=np.uint16),
        "mel_lengths": np.asarray(mel_lengths, dtype=np.uint16),
        "mel_values": np.asarray(mel_values, dtype=np.float32),
        "dct": dct,
        "twiddle_real": np.cos(angle).astype(np.float32),
        "twiddle_imag": np.sin(angle).astype(np.float32),
        "coefficient_mean": np.asarray(contract["coefficient_mean"], dtype=np.float32),
        "coefficient_std": np.asarray(contract["coefficient_std"], dtype=np.float32),
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Export the compact cry MLP as an independently testable INT8 C fallback."
    )
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--train", type=Path, required=True)
    parser.add_argument("--test", type=Path, required=True)
    parser.add_argument(
        "--config",
        type=Path,
    )
    parser.add_argument(
        "--contract",
        type=Path,
    )
    parser.add_argument("--activation-percentile", type=float, default=99.99)
    parser.add_argument("--threshold", type=float, default=0.40)
    parser.add_argument("--consecutive", type=int, default=3)
    parser.add_argument("--output-header", type=Path, required=True)
    parser.add_argument("--output-report", type=Path, required=True)
    args = parser.parse_args()

    args.config = resolve_frozen_file(args.model, args.config, "audio_two_stage.yaml")
    args.contract = resolve_frozen_file(
        args.model, args.contract, "preprocessing_contract.json"
    )
    contract_sha256 = sha256_file(args.contract)
    verify_contract_hash(args.train, contract_sha256)
    verify_contract_hash(args.test, contract_sha256)

    model = tf.keras.models.load_model(args.model)
    dense_layers = [layer for layer in model.layers if isinstance(layer, tf.keras.layers.Dense)]
    if [layer.units for layer in dense_layers] != [64, 16, 2]:
        raise ValueError("expected Dense layer widths [64, 16, 2]")
    parameters = [layer.get_weights() for layer in dense_layers]
    quantized_weights: list[np.ndarray] = []
    weight_scales: list[np.ndarray] = []
    biases: list[np.ndarray] = []
    for weights, bias in parameters:
        quantized, scales = quantize_per_output(weights)
        quantized_weights.append(quantized)
        weight_scales.append(scales)
        biases.append(np.asarray(bias, dtype=np.float32))

    with np.load(args.train) as train_data:
        train_x = np.asarray(train_data["x"], dtype=np.float32)
    input_scale = activation_scale(train_x, args.activation_percentile)
    q0 = quantize_symmetric(train_x, input_scale, relu=False)
    h1 = np.maximum(
        dense_quantized(q0.reshape(len(q0), -1), input_scale,
                        quantized_weights[0], weight_scales[0], biases[0]),
        0.0,
    )
    hidden1_scale = activation_scale(h1, args.activation_percentile)
    q1 = quantize_symmetric(h1, hidden1_scale, relu=True)
    h2 = np.maximum(
        dense_quantized(q1, hidden1_scale,
                        quantized_weights[1], weight_scales[1], biases[1]),
        0.0,
    )
    hidden2_scale = activation_scale(h2, args.activation_percentile)

    with np.load(args.test) as test_data:
        required = {"x", "y", "source", "recording_id", "window_start_samples", "classes"}
        missing = required - set(test_data.files)
        if missing:
            raise ValueError(f"test dataset missing arrays {sorted(missing)}")
        arrays = {name: np.asarray(test_data[name]) for name in required}
    test_x = np.asarray(arrays["x"], dtype=np.float32)
    tq0 = quantize_symmetric(test_x, input_scale, relu=False)
    th1 = np.maximum(
        dense_quantized(tq0.reshape(len(tq0), -1), input_scale,
                        quantized_weights[0], weight_scales[0], biases[0]),
        0.0,
    )
    tq1 = quantize_symmetric(th1, hidden1_scale, relu=True)
    th2 = np.maximum(
        dense_quantized(tq1, hidden1_scale,
                        quantized_weights[1], weight_scales[1], biases[1]),
        0.0,
    )
    tq2 = quantize_symmetric(th2, hidden2_scale, relu=True)
    logits = dense_quantized(
        tq2, hidden2_scale, quantized_weights[2], weight_scales[2], biases[2]
    )
    logit_delta = np.clip(logits[:, 0] - logits[:, 1], -80.0, 80.0)
    cry_probability = 1.0 / (1.0 + np.exp(logit_delta))
    predictions = (cry_probability >= 0.5).astype(np.int32)

    config = yaml.safe_load(args.config.read_text(encoding="utf-8"))
    contract = json.loads(args.contract.read_text(encoding="utf-8"))
    constants = generate_feature_constants(config, contract)
    model_digest = hashlib.sha256(args.model.read_bytes()).hexdigest()

    report = {
        "model": str(args.model),
        "model_sha256": model_digest,
        "preprocessing_contract": str(args.contract),
        "preprocessing_sha256": contract_sha256,
        "training_config": str(args.config),
        "training_config_sha256": sha256_file(args.config),
        "activation_percentile": args.activation_percentile,
        "input_scale": input_scale,
        "hidden1_scale": hidden1_scale,
        "hidden2_scale": hidden2_scale,
        "test_windows": int(len(arrays["y"])),
        "test_accuracy": float(accuracy_score(arrays["y"], predictions)),
        "window_confusion_matrix": confusion_matrix(
            arrays["y"], predictions, labels=[0, 1]
        ).tolist(),
        "decision_threshold": args.threshold,
        "consecutive_windows": args.consecutive,
        "temporal_all": temporal_report(
            cry_probability, arrays, args.threshold, args.consecutive, None
        ),
        "temporal_device": temporal_report(
            cry_probability,
            arrays,
            args.threshold,
            args.consecutive,
            "device_selected_ra8p1",
        ),
        "mel_nonzero_weights": int(len(constants["mel_values"])),
    }

    sections = [
        "#ifndef AUDIO_CRY_MODEL_DATA_H\n",
        "#define AUDIO_CRY_MODEL_DATA_H\n\n",
        "#include <stdint.h>\n\n",
        f"#define CRY_MODEL_SHA256_PREFIX \"{model_digest[:16]}\"\n",
        "#define CRY_MODEL_INPUT_COUNT (490U)\n",
        "#define CRY_MODEL_HIDDEN1_COUNT (64U)\n",
        "#define CRY_MODEL_HIDDEN2_COUNT (16U)\n",
        f"#define CRY_MODEL_INPUT_SCALE ({input_scale:.9g}f)\n",
        f"#define CRY_MODEL_HIDDEN1_SCALE ({hidden1_scale:.9g}f)\n",
        f"#define CRY_MODEL_HIDDEN2_SCALE ({hidden2_scale:.9g}f)\n\n",
    ]
    for index, (weights, scales, bias) in enumerate(
        zip(quantized_weights, weight_scales, biases), start=1
    ):
        sections.append(
            format_c_array(
                f"g_cry_dense{index}_weights",
                weights.T,
                "int8_t",
                values_per_line=20,
            )
        )
        sections.append(format_c_array(f"g_cry_dense{index}_scales", scales, "float"))
        sections.append(format_c_array(f"g_cry_dense{index}_bias", bias, "float"))
    sections.extend(
        [
            format_c_array("g_cry_hann", constants["hann"], "float"),
            format_c_array("g_cry_mel_starts", constants["mel_starts"], "uint16_t"),
            format_c_array("g_cry_mel_lengths", constants["mel_lengths"], "uint16_t"),
            format_c_array("g_cry_mel_weights", constants["mel_values"], "float"),
            format_c_array("g_cry_dct", constants["dct"], "float"),
            format_c_array("g_cry_twiddle_real", constants["twiddle_real"], "float"),
            format_c_array("g_cry_twiddle_imag", constants["twiddle_imag"], "float"),
            format_c_array(
                "g_cry_coefficient_mean", constants["coefficient_mean"], "float"
            ),
            format_c_array(
                "g_cry_coefficient_std", constants["coefficient_std"], "float"
            ),
            "\n#endif /* AUDIO_CRY_MODEL_DATA_H */\n",
        ]
    )
    args.output_header.parent.mkdir(parents=True, exist_ok=True)
    args.output_header.write_text("".join(sections), encoding="utf-8", newline="\n")
    args.output_report.parent.mkdir(parents=True, exist_ok=True)
    args.output_report.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

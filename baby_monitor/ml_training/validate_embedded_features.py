from __future__ import annotations

import argparse
import json
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf
import yaml

from audio_pipeline.features import extract_mfcc, preprocess_waveform


ROOT = Path(__file__).resolve().parent


def embedded_reference_mfcc(
    samples: np.ndarray,
    config: dict,
    contract: dict,
) -> np.ndarray:
    mfcc = config["mfcc"]
    values = preprocess_waveform(samples, config)
    frame_length = int(mfcc["frame_length_samples"])
    frame_step = int(mfcc["frame_step_samples"])
    fft_length = int(mfcc["fft_length"])
    frame_count = 1 + (len(values) - fft_length) // frame_step
    hann = librosa.filters.get_window(
        str(mfcc["window_function"]),
        frame_length,
        fftbins=bool(mfcc["window_periodic"]),
    ).astype(np.float32)
    offset = (fft_length - frame_length) // 2
    power = np.empty((frame_count, fft_length // 2 + 1), dtype=np.float32)
    for frame in range(frame_count):
        block = np.zeros(fft_length, dtype=np.float32)
        start = frame * frame_step
        block[offset:offset + frame_length] = (
            values[start + offset:start + offset + frame_length] * hann
        )
        spectrum = np.fft.rfft(block)
        power[frame] = np.square(spectrum.real) + np.square(spectrum.imag)

    mel = librosa.filters.mel(
        sr=int(config["audio"]["sample_rate_hz"]),
        n_fft=fft_length,
        n_mels=int(mfcc["mel_bins"]),
        fmin=float(mfcc["lower_frequency_hz"]),
        fmax=float(mfcc["upper_frequency_hz"]),
        htk=str(mfcc["mel_formula"]).lower() == "htk",
        norm=str(mfcc["mel_normalization"]),
    ).astype(np.float32)
    energies = power @ mel.T
    log_mel = 10.0 * np.log10(
        np.maximum(energies, float(mfcc["power_to_db_amin"]))
    )
    log_mel = np.maximum(log_mel, np.max(log_mel) - float(mfcc["power_to_db_top_db"]))
    bins = np.arange(int(mfcc["mel_bins"]), dtype=np.float64)
    dct = np.empty(
        (int(mfcc["mfcc_coefficients"]), int(mfcc["mel_bins"])),
        dtype=np.float32,
    )
    for coefficient in range(len(dct)):
        scale = np.sqrt(1.0 / len(bins)) if coefficient == 0 else np.sqrt(2.0 / len(bins))
        dct[coefficient] = (
            scale * np.cos(np.pi / len(bins) * (bins + 0.5) * coefficient)
        ).astype(np.float32)
    features = log_mel @ dct.T
    mean = np.asarray(contract["coefficient_mean"], dtype=np.float32)
    std = np.asarray(contract["coefficient_std"], dtype=np.float32)
    return ((features - mean) / std)[..., np.newaxis].astype(np.float32)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compare the generated Cortex-M85 MFCC math with the training contract."
    )
    parser.add_argument("wav", nargs="+", type=Path)
    parser.add_argument(
        "--config", type=Path, default=ROOT / "audio_two_stage.yaml"
    )
    parser.add_argument(
        "--contract",
        type=Path,
        default=ROOT / "data" / "features" / "preprocessing_contract.json",
    )
    args = parser.parse_args()
    config = yaml.safe_load(args.config.read_text(encoding="utf-8"))
    contract = json.loads(args.contract.read_text(encoding="utf-8"))
    reports = []
    for path in args.wav:
        samples, sample_rate = sf.read(path, dtype="float32", always_2d=False)
        if sample_rate != int(config["audio"]["sample_rate_hz"]):
            raise ValueError(f"{path}: expected 16 kHz, got {sample_rate}")
        samples = np.asarray(samples, dtype=np.float32).reshape(-1)
        window = np.zeros(int(config["audio"]["window_samples"]), dtype=np.float32)
        copied = min(len(samples), len(window))
        window[:copied] = samples[:copied]
        expected = extract_mfcc(window, config)
        mean = np.asarray(contract["coefficient_mean"], dtype=np.float32).reshape(1, -1, 1)
        std = np.asarray(contract["coefficient_std"], dtype=np.float32).reshape(1, -1, 1)
        expected = (expected - mean) / std
        actual = embedded_reference_mfcc(window, config, contract)
        difference = np.abs(actual - expected)
        reports.append(
            {
                "wav": str(path),
                "shape": list(actual.shape),
                "maximum_absolute_error": float(np.max(difference)),
                "mean_absolute_error": float(np.mean(difference)),
            }
        )
    print(json.dumps(reports, indent=2))
    if any(item["maximum_absolute_error"] > 2.0e-4 for item in reports):
        raise SystemExit("embedded MFCC parity gate failed")


if __name__ == "__main__":
    main()

from __future__ import annotations

import math
from typing import Any

import librosa
import numpy as np


def dbfs(samples: np.ndarray) -> float:
    values = np.asarray(samples, dtype=np.float64)
    rms = float(np.sqrt(np.mean(np.square(values)))) if values.size else 0.0
    return 20.0 * math.log10(max(rms, 1.0e-12))


def preprocess_waveform(samples: np.ndarray, config: dict[str, Any]) -> np.ndarray:
    return preprocess_waveforms(np.asarray(samples, dtype=np.float32).reshape(1, -1), config)[0]


def preprocess_waveforms(samples: np.ndarray, config: dict[str, Any]) -> np.ndarray:
    values = np.asarray(samples, dtype=np.float32).copy()
    if values.ndim != 2:
        raise ValueError(f"expected [windows,samples], got {values.shape}")
    norm = config["normalization"]
    if norm["remove_dc"] and values.shape[1]:
        values -= np.mean(values, axis=1, dtype=np.float64, keepdims=True)
    pre_emphasis = float(norm.get("pre_emphasis", 0.0))
    if pre_emphasis and values.shape[1] > 1:
        original = values.copy()
        values[:, 1:] = original[:, 1:] - pre_emphasis * original[:, :-1]

    rms = np.sqrt(np.mean(np.square(values), axis=1, dtype=np.float64))
    target = 10.0 ** (float(norm["target_rms_dbfs"]) / 20.0)
    gain = np.ones_like(rms)
    valid = rms > 1.0e-9
    gain[valid] = np.clip(
        target / rms[valid],
        float(norm["minimum_gain"]),
        float(norm["maximum_gain"]),
    )
    values *= gain[:, np.newaxis].astype(np.float32)
    return np.clip(values, -1.0, 1.0).astype(np.float32, copy=False)


def extract_mfcc(samples: np.ndarray, config: dict[str, Any]) -> np.ndarray:
    values = np.asarray(samples, dtype=np.float32).reshape(1, -1)
    return extract_mfcc_batch(values, config)[0]


def extract_mfcc_batch(samples: np.ndarray, config: dict[str, Any]) -> np.ndarray:
    audio = config["audio"]
    mfcc_config = config["mfcc"]
    values = preprocess_waveforms(samples, config)
    if mfcc_config["window_zero_padding"] != "centered_in_fft_frame":
        raise ValueError("only centered_in_fft_frame window padding is implemented")
    analysis_window = librosa.filters.get_window(
        str(mfcc_config["window_function"]),
        int(mfcc_config["frame_length_samples"]),
        fftbins=bool(mfcc_config["window_periodic"]),
    )
    mel_spectrogram = librosa.feature.melspectrogram(
        y=values,
        sr=int(audio["sample_rate_hz"]),
        n_fft=int(mfcc_config["fft_length"]),
        hop_length=int(mfcc_config["frame_step_samples"]),
        win_length=int(mfcc_config["frame_length_samples"]),
        window=analysis_window,
        center=bool(mfcc_config["center"]),
        pad_mode="constant",
        power=float(mfcc_config["spectrum_power"]),
        n_mels=int(mfcc_config["mel_bins"]),
        fmin=float(mfcc_config["lower_frequency_hz"]),
        fmax=float(mfcc_config["upper_frequency_hz"]),
        htk=str(mfcc_config["mel_formula"]).lower() == "htk",
        norm=str(mfcc_config["mel_normalization"]),
    )
    amin = float(mfcc_config["power_to_db_amin"])
    reference = max(float(mfcc_config["power_to_db_reference"]), amin)
    log_mel = 10.0 * np.log10(np.maximum(mel_spectrogram, amin))
    log_mel -= 10.0 * math.log10(reference)
    top_db = float(mfcc_config["power_to_db_top_db"])
    peak = np.max(log_mel, axis=(-2, -1), keepdims=True)
    log_mel = np.maximum(log_mel, peak - top_db)
    features = librosa.feature.mfcc(
        S=log_mel,
        n_mfcc=int(mfcc_config["mfcc_coefficients"]),
        dct_type=int(mfcc_config["dct_type"]),
        norm=str(mfcc_config["dct_norm"]),
        lifter=0,
    )
    result = np.asarray(np.swapaxes(features, -2, -1)[..., np.newaxis], dtype=np.float32)
    expected = tuple(int(value) for value in mfcc_config["expected_shape"])
    if result.shape[1:] != expected:
        raise ValueError(f"MFCC contract expected [N,{expected}], got {result.shape}")
    return result


def normalize_features(
    features: np.ndarray,
    coefficient_mean: np.ndarray,
    coefficient_std: np.ndarray,
) -> np.ndarray:
    values = np.asarray(features, dtype=np.float32)
    mean = np.asarray(coefficient_mean, dtype=np.float32).reshape(1, -1, 1)
    std = np.asarray(coefficient_std, dtype=np.float32).reshape(1, -1, 1)
    return ((values - mean) / std).astype(np.float32, copy=False)


class CoefficientStats:
    def __init__(self, coefficients: int) -> None:
        self.count = 0
        self.sum = np.zeros(coefficients, dtype=np.float64)
        self.sum_squares = np.zeros(coefficients, dtype=np.float64)

    def update(self, features: np.ndarray) -> None:
        values = np.asarray(features, dtype=np.float64)[:, :, 0]
        self.count += values.shape[0]
        self.sum += values.sum(axis=0)
        self.sum_squares += np.square(values).sum(axis=0)

    def finish(self, floor: float) -> tuple[np.ndarray, np.ndarray]:
        if self.count == 0:
            raise ValueError("cannot compute normalization without training features")
        mean = self.sum / self.count
        variance = np.maximum(self.sum_squares / self.count - np.square(mean), 0.0)
        std = np.maximum(np.sqrt(variance), float(floor))
        return mean.astype(np.float32), std.astype(np.float32)

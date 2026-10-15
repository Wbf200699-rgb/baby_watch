from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Sequence

import numpy as np


@dataclass(frozen=True)
class CryTrigger:
    confirmed: bool
    first_confirmed_window: int | None
    longest_run: int
    threshold: float
    required_consecutive_windows: int


@dataclass(frozen=True)
class CryVoteTrigger:
    confirmed: bool
    first_confirmed_window: int | None
    maximum_hits: int
    threshold: float
    vote_window_count: int
    required_hits: int


@dataclass(frozen=True)
class TypeDecision:
    label: str
    status: str
    confidence: float
    margin: float
    mean_probabilities: list[float]
    windows_used: int


def find_confirmed_cry(
    cry_probabilities: Sequence[float],
    threshold: float = 0.50,
    consecutive_windows: int = 3,
) -> dict:
    if consecutive_windows < 1:
        raise ValueError("consecutive_windows must be at least one")
    run = 0
    longest = 0
    first: int | None = None
    for index, probability in enumerate(cry_probabilities):
        if not np.isfinite(probability):
            run = 0
            continue
        run = run + 1 if probability >= threshold else 0
        longest = max(longest, run)
        if run >= consecutive_windows and first is None:
            first = index
    return asdict(
        CryTrigger(
            confirmed=first is not None,
            first_confirmed_window=first,
            longest_run=longest,
            threshold=float(threshold),
            required_consecutive_windows=int(consecutive_windows),
        )
    )


def find_voted_cry(
    cry_probabilities: Sequence[float],
    threshold: float = 0.50,
    vote_window_count: int = 8,
    required_hits: int = 3,
) -> dict:
    """Confirm intermittent cry from N hits in a sliding window instead of a strict run."""
    if vote_window_count < 1:
        raise ValueError("vote_window_count must be at least one")
    if required_hits < 1 or required_hits > vote_window_count:
        raise ValueError("required_hits must be between one and vote_window_count")
    history = [False] * vote_window_count
    history_index = 0
    history_count = 0
    hits = 0
    maximum_hits = 0
    first: int | None = None
    for index, probability in enumerate(cry_probabilities):
        hit = bool(np.isfinite(probability) and probability >= threshold)
        if history_count == vote_window_count:
            hits -= int(history[history_index])
        else:
            history_count += 1
        history[history_index] = hit
        history_index = (history_index + 1) % vote_window_count
        hits += int(hit)
        maximum_hits = max(maximum_hits, hits)
        if hits >= required_hits and first is None:
            first = index
    return asdict(
        CryVoteTrigger(
            confirmed=first is not None,
            first_confirmed_window=first,
            maximum_hits=maximum_hits,
            threshold=float(threshold),
            vote_window_count=int(vote_window_count),
            required_hits=int(required_hits),
        )
    )


def classify_cry_type(
    probabilities: Sequence[Sequence[float]],
    classes: Sequence[str],
    confidence_threshold: float = 0.65,
    margin_threshold: float = 0.15,
) -> dict:
    values = np.asarray(probabilities, dtype=np.float64)
    if values.ndim != 2 or values.shape[1] != len(classes):
        raise ValueError(f"expected [windows,{len(classes)}] probabilities, got {values.shape}")
    if values.shape[0] == 0:
        raise ValueError("at least one type window is required")
    if not np.isfinite(values).all():
        raise ValueError("probabilities contain NaN or infinity")

    means = values.mean(axis=0)
    order = np.argsort(means)[::-1]
    best = int(order[0])
    second = int(order[1]) if len(order) > 1 else best
    confidence = float(means[best])
    margin = float(means[best] - means[second]) if len(order) > 1 else confidence

    if confidence < confidence_threshold:
        label, status = "unknown", "low_confidence"
    elif margin < margin_threshold:
        label, status = "uncertain", "small_margin"
    else:
        label, status = str(classes[best]), "accepted"

    return asdict(
        TypeDecision(
            label=label,
            status=status,
            confidence=confidence,
            margin=margin,
            mean_probabilities=[float(item) for item in means],
            windows_used=int(values.shape[0]),
        )
    )

from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent


def sequence_offsets(count: int, length: int, stride: int, training: bool) -> list[list[int]]:
    if not training or count <= length:
        return [list(range(count))]
    starts = list(range(0, count - length + 1, stride))
    last = count - length
    if starts[-1] != last:
        starts.append(last)
    return [list(range(start, start + length)) for start in starts]


def convert(
    source: Path,
    output: Path,
    *,
    sequence_length: int,
    stride: int,
    training: bool,
) -> dict:
    with np.load(source) as data:
        required = {
            "x",
            "y",
            "recording_id",
            "window_start_samples",
            "source",
            "classes",
        }
        missing = required - set(data.files)
        if missing:
            raise ValueError(f"{source}: missing arrays {sorted(missing)}")
        arrays = {name: np.asarray(data[name]) for name in required}

    groups: dict[str, list[int]] = defaultdict(list)
    for index, recording_id in enumerate(arrays["recording_id"].astype(str)):
        groups[recording_id].append(index)
    x_values: list[np.ndarray] = []
    y_values: list[int] = []
    recording_values: list[str] = []
    source_values: list[str] = []
    windows_used: list[int] = []
    for recording_id, indices in sorted(groups.items()):
        indices.sort(key=lambda index: int(arrays["window_start_samples"][index]))
        labels = {int(arrays["y"][index]) for index in indices}
        sources = {str(arrays["source"][index]) for index in indices}
        if len(labels) != 1 or len(sources) != 1:
            raise ValueError(f"{recording_id}: inconsistent recording metadata")
        for selected_positions in sequence_offsets(
            len(indices), sequence_length, stride, training
        ):
            selected = [indices[position] for position in selected_positions]
            values = np.asarray(arrays["x"][selected], dtype=np.float32)
            mean = np.mean(values, axis=0)
            std = np.std(values, axis=0)
            x_values.append(np.concatenate([mean, std], axis=-1))
            y_values.append(next(iter(labels)))
            recording_values.append(recording_id)
            source_values.append(next(iter(sources)))
            windows_used.append(len(selected))

    x = np.stack(x_values).astype(np.float32)
    y = np.asarray(y_values, dtype=np.int32)
    output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        output,
        x=x,
        y=y,
        recording_id=np.asarray(recording_values),
        source=np.asarray(source_values),
        windows_used=np.asarray(windows_used, dtype=np.int32),
        classes=arrays["classes"],
    )
    return {
        "input": str(source),
        "output": str(output),
        "recordings": len(groups),
        "sequences": len(y),
        "shape": list(x.shape),
        "class_sequences": np.bincount(
            y, minlength=len(arrays["classes"])
        ).tolist(),
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Aggregate multiple cry windows into mean/std type-classification inputs."
    )
    parser.add_argument("--input-prefix", default="cry_type_v1")
    parser.add_argument("--output-prefix", default="cry_type_sequence")
    parser.add_argument("--sequence-length", type=int, default=8)
    parser.add_argument("--stride", type=int, default=4)
    args = parser.parse_args()

    feature_root = ROOT / "data" / "features"
    reports = []
    for split in ("train", "validation", "test"):
        reports.append(
            convert(
                feature_root / f"{args.input_prefix}_{split}.npz",
                feature_root / f"{args.output_prefix}_{split}.npz",
                sequence_length=args.sequence_length,
                stride=args.stride,
                training=split == "train",
            )
        )
    for report in reports:
        print(report)


if __name__ == "__main__":
    main()

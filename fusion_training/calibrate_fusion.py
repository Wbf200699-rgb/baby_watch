#!/usr/bin/env python3
"""Build the explainable RA8 fusion calibration from exported capture sessions.

The capture export stores each session as a ZIP inside an outer ZIP.  Pressure,
thermal, and bed-temperature sessions are intentionally independent, so this
tool calibrates per-modality evidence functions instead of pretending the
samples are synchronized multimodal observations.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import statistics
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

import numpy as np


PROFILE_VERSION = 0x20260803
PRESSURE_ROWS = 16
PRESSURE_COLS = 16
THERMAL_ROWS = 64
THERMAL_COLS = 80
THERMAL_HOTSPOT_DELTA_DECI_C = 15
THERMAL_HOTSPOT_P99_GAP_DECI_C = 25
THERMAL_HOTSPOT_MIN_POINTS = 8
THERMAL_CONTRAST_WEIGHT_X1000 = 850
THERMAL_AREA_WEIGHT_X1000 = 150


@dataclass(frozen=True)
class PressureFrame:
    label: str
    total: int
    peak: int
    active_points: int
    center_x: float | None
    center_y: float | None
    edge_ratio_x1000: int


@dataclass(frozen=True)
class ThermalFrame:
    label: str
    p01_deci_c: int
    p99_deci_c: int
    mean_deci_c: int
    contrast_deci_c: int
    hotspot_points: int
    hotspot_x_x1000: int
    hotspot_y_x1000: int


@dataclass(frozen=True)
class BedTemperatureSample:
    label: str
    values_milli_c: tuple[int, int, int, int, int]


@dataclass
class Dataset:
    pressure: list[PressureFrame]
    thermal: list[ThermalFrame]
    bed_temperature: list[BedTemperatureSample]
    sessions: list[dict[str, object]]
    errors: list[str]


def _rank_percentile(values: np.ndarray, percent: int) -> int:
    """Match the integer rank percentile used by the embedded thermal driver."""
    ordered = np.sort(values.reshape(-1))
    rank = ((len(ordered) - 1) * percent) // 100
    return int(ordered[rank])


def _pressure_frame(label: str, raw: bytes) -> PressureFrame:
    if len(raw) != PRESSURE_ROWS * PRESSURE_COLS * 2:
        raise ValueError(f"pressure payload is {len(raw)} bytes")
    matrix = np.frombuffer(raw, dtype="<i2").reshape(PRESSURE_ROWS, PRESSURE_COLS).astype(np.int32)
    matrix = np.maximum(matrix, 0)
    total = int(matrix.sum())
    peak = int(matrix.max())
    active_points = int(np.count_nonzero(matrix))
    center_x: float | None = None
    center_y: float | None = None
    edge_ratio_x1000 = 0
    if total:
        yy, xx = np.indices(matrix.shape)
        center_x = float((matrix * xx).sum() / total)
        center_y = float((matrix * yy).sum() / total)
        edge = np.zeros_like(matrix, dtype=bool)
        edge[:2, :] = True
        edge[-2:, :] = True
        edge[:, :2] = True
        edge[:, -2:] = True
        edge_ratio_x1000 = min(1000, int(matrix[edge].sum() * 1000 // total))
    return PressureFrame(label, total, peak, active_points, center_x, center_y, edge_ratio_x1000)


def _thermal_frame(label: str, raw: bytes) -> ThermalFrame:
    if len(raw) != THERMAL_ROWS * THERMAL_COLS * 2:
        raise ValueError(f"thermal payload is {len(raw)} bytes")
    matrix = np.frombuffer(raw, dtype="<i2").reshape(THERMAL_ROWS, THERMAL_COLS).astype(np.int32)
    p01 = _rank_percentile(matrix, 1)
    p99 = _rank_percentile(matrix, 99)
    mean = int(matrix.sum() // matrix.size)
    threshold = max(mean + THERMAL_HOTSPOT_DELTA_DECI_C, p99 - THERMAL_HOTSPOT_P99_GAP_DECI_C)
    mask = matrix >= threshold
    hotspot_points = int(mask.sum())
    hotspot_x = 500
    hotspot_y = 500
    if hotspot_points:
        weights = np.where(mask, matrix - threshold + 1, 0).astype(np.int64)
        yy, xx = np.indices(matrix.shape)
        weight_sum = int(weights.sum())
        hotspot_x = int((weights * xx).sum() * 1000 // (weight_sum * (THERMAL_COLS - 1)))
        hotspot_y = int((weights * yy).sum() * 1000 // (weight_sum * (THERMAL_ROWS - 1)))
    return ThermalFrame(
        label=label,
        p01_deci_c=p01,
        p99_deci_c=p99,
        mean_deci_c=mean,
        contrast_deci_c=max(0, p99 - p01),
        hotspot_points=hotspot_points,
        hotspot_x_x1000=max(0, min(1000, hotspot_x)),
        hotspot_y_x1000=max(0, min(1000, hotspot_y)),
    )


def _read_json_lines(raw: bytes) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for line in raw.decode("utf-8-sig").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def load_dataset(archive: Path) -> Dataset:
    pressure: list[PressureFrame] = []
    thermal: list[ThermalFrame] = []
    bed_temperature: list[BedTemperatureSample] = []
    sessions: list[dict[str, object]] = []
    errors: list[str] = []

    with zipfile.ZipFile(archive) as outer:
        for outer_info in outer.infolist():
            if not outer_info.filename.lower().endswith(".zip"):
                continue
            path_parts = outer_info.filename.replace("\\", "/").split("/")
            if len(path_parts) < 4:
                errors.append(f"cannot infer modality and label: {outer_info.filename}")
                continue
            modality, label = path_parts[-3], path_parts[-2]
            with zipfile.ZipFile(io.BytesIO(outer.read(outer_info))) as inner:
                names = inner.namelist()
                manifest = json.loads(inner.read("manifest.json")) if "manifest.json" in names else {}
                matrix_names = sorted(name for name in names if name.lower().endswith(".i16le"))
                samples_name = next((name for name in names if name.lower().endswith(".jsonl")), None)
                sample_count = 0
                if modality == "压力":
                    for name in matrix_names:
                        try:
                            pressure.append(_pressure_frame(label, inner.read(name)))
                            sample_count += 1
                        except ValueError as exc:
                            errors.append(f"{outer_info.filename}:{name}: {exc}")
                elif modality == "热成像":
                    for name in matrix_names:
                        try:
                            thermal.append(_thermal_frame(label, inner.read(name)))
                            sample_count += 1
                        except ValueError as exc:
                            errors.append(f"{outer_info.filename}:{name}: {exc}")
                elif modality == "床温" and samples_name:
                    for sample in _read_json_lines(inner.read(samples_name)):
                        features = sample.get("features")
                        if not isinstance(features, dict):
                            encoded = sample.get("features_json")
                            features = json.loads(encoded) if isinstance(encoded, str) else {}
                        values = features.get("t_mc")
                        if isinstance(values, list) and len(values) == 5 and int(features.get("ok") or 0) != 0:
                            bed_temperature.append(
                                BedTemperatureSample(label, tuple(int(value) for value in values))
                            )
                            sample_count += 1
                sessions.append(
                    {
                        "archive": outer_info.filename,
                        "session_id": manifest.get("session_id"),
                        "modality": modality,
                        "label": label,
                        "sample_count": sample_count,
                        "started_at": manifest.get("started_at"),
                        "ended_at": manifest.get("ended_at"),
                    }
                )
    return Dataset(pressure, thermal, bed_temperature, sessions, errors)


def _round_up(value: float, step: int) -> int:
    return int(math.ceil(value / step) * step)


def _scale_x1000(value: int, low: int, high: int) -> int:
    if value <= low:
        return 0
    if value >= high:
        return 1000
    return (value - low) * 1000 // (high - low)


def thermal_presence_score(frame: ThermalFrame, profile: dict[str, object]) -> int:
    contrast = _scale_x1000(
        frame.contrast_deci_c,
        int(profile["thermal_contrast_low_deci_c"]),
        int(profile["thermal_contrast_high_deci_c"]),
    )
    area = _scale_x1000(
        frame.hotspot_points,
        int(profile["thermal_hotspot_min_points"]),
        int(profile["thermal_hotspot_full_points"]),
    )
    if frame.hotspot_points < int(profile["thermal_hotspot_min_points"]):
        return 0
    return min(
        1000,
        (
            contrast * int(profile["thermal_contrast_weight_x1000"])
            + area * int(profile["thermal_area_weight_x1000"])
        )
        // 1000,
    )


def _semantic_side(label: str) -> str:
    if "左侧" in label:
        return "left"
    if "右侧" in label:
        return "right"
    return "center"


def derive_profile(dataset: Dataset, archive_sha256: str) -> dict[str, object]:
    negative_thermal = [
        frame
        for frame in dataset.thermal
        if frame.label == "空床" or "无加热" in frame.label
    ]
    stable_heated = [
        frame
        for frame in dataset.thermal
        if "加热" in frame.label and "过程" not in frame.label
    ]
    if not negative_thermal or not stable_heated:
        raise ValueError("thermal calibration requires empty/unheated and stable heated sessions")

    negative_contrast = np.asarray([frame.contrast_deci_c for frame in negative_thermal])
    heated_contrast = np.asarray([frame.contrast_deci_c for frame in stable_heated])
    heated_hotspots = np.asarray([frame.hotspot_points for frame in stable_heated])
    contrast_low = _round_up(float(negative_contrast.max()), 5)
    contrast_high = max(contrast_low + 20, _round_up(float(np.percentile(heated_contrast, 25)), 5))
    # A face may be the only exposed heated area.  Keep only a small physical
    # speckle gate here; the robust P99-P1 contrast remains the dominant gate.
    hotspot_min = THERMAL_HOTSPOT_MIN_POINTS
    hotspot_full = max(hotspot_min + 64, _round_up(float(np.percentile(heated_hotspots, 90)), 16))

    provisional: dict[str, object] = {
        "profile_version": PROFILE_VERSION,
        "archive_sha256": archive_sha256,
        "pressure_active_point": 1,
        "pressure_confirm_frames": 2,
        "pressure_release_frames": 32,
        "pressure_edge_width": 2,
        "pressure_edge_positive_x1000": 550,
        "thermal_contrast_low_deci_c": contrast_low,
        "thermal_contrast_high_deci_c": contrast_high,
        "thermal_hotspot_delta_deci_c": THERMAL_HOTSPOT_DELTA_DECI_C,
        "thermal_hotspot_p99_gap_deci_c": THERMAL_HOTSPOT_P99_GAP_DECI_C,
        "thermal_hotspot_min_points": hotspot_min,
        "thermal_hotspot_full_points": hotspot_full,
        "thermal_contrast_weight_x1000": THERMAL_CONTRAST_WEIGHT_X1000,
        "thermal_area_weight_x1000": THERMAL_AREA_WEIGHT_X1000,
    }

    pressure_x_by_side: dict[str, list[float]] = {"left": [], "center": [], "right": []}
    for frame in dataset.pressure:
        if frame.center_x is not None:
            pressure_x_by_side[_semantic_side(frame.label)].append(frame.center_x / (PRESSURE_COLS - 1))

    thermal_x_by_side: dict[str, list[float]] = {"left": [], "center": [], "right": []}
    for frame in stable_heated:
        if thermal_presence_score(frame, provisional) >= 550:
            thermal_x_by_side[_semantic_side(frame.label)].append(frame.hotspot_x_x1000 / 1000.0)

    sides = ("left", "center", "right")
    if any(not pressure_x_by_side[side] or not thermal_x_by_side[side] for side in sides):
        raise ValueError("left, center, and right anchors are required for thermal-to-bed mapping")
    thermal_anchors = np.asarray([statistics.median(thermal_x_by_side[side]) for side in sides])
    pressure_anchors = np.asarray([statistics.median(pressure_x_by_side[side]) for side in sides])
    design = np.column_stack((thermal_anchors, np.ones(3)))
    slope, intercept = np.linalg.lstsq(design, pressure_anchors, rcond=None)[0]
    mapped = np.clip(slope * thermal_anchors + intercept, 0.0, 1.0)
    edge_distances = np.abs(mapped - 0.5)
    center_distance = float(edge_distances[1])
    side_distance = float(min(edge_distances[0], edge_distances[2]))
    edge_low = _round_up((center_distance + side_distance) * 500.0, 10)
    edge_high = max(edge_low + 80, _round_up(float(max(edge_distances[0], edge_distances[2])) * 1000.0, 10))

    provisional.update(
        {
            "thermal_bed_x_slope_x1000": int(round(slope * 1000)),
            "thermal_bed_x_intercept_x1000": int(round(intercept * 1000)),
            "thermal_edge_low_x1000": edge_low,
            "thermal_edge_high_x1000": min(500, edge_high),
            "edge_pressure_weight": 450,
            "edge_vision_weight": 400,
            "edge_thermal_weight": 150,
            "position_agreement_good_x1000": 120,
            "position_agreement_bad_x1000": 400,
            "thermal_anchor_x": {side: round(float(thermal_anchors[index]), 6) for index, side in enumerate(sides)},
            "pressure_anchor_x": {side: round(float(pressure_anchors[index]), 6) for index, side in enumerate(sides)},
            "mapped_anchor_x": {side: round(float(mapped[index]), 6) for index, side in enumerate(sides)},
        }
    )
    return provisional


def _group(items: Iterable[object], key: str) -> dict[str, list[object]]:
    grouped: dict[str, list[object]] = {}
    for item in items:
        grouped.setdefault(str(getattr(item, key)), []).append(item)
    return grouped


def build_metrics(dataset: Dataset, profile: dict[str, object]) -> dict[str, object]:
    pressure_rows: dict[str, object] = {}
    for label, frames_raw in _group(dataset.pressure, "label").items():
        frames = [frame for frame in frames_raw if isinstance(frame, PressureFrame)]
        active = [frame for frame in frames if frame.active_points > 0]
        pressure_rows[label] = {
            "frames": len(frames),
            "active_frames": len(active),
            "active_coverage": round(len(active) / len(frames), 6) if frames else 0.0,
            "median_total_active": round(float(statistics.median(frame.total for frame in active)), 3) if active else 0.0,
            "median_edge_ratio_x1000_active": round(
                float(statistics.median(frame.edge_ratio_x1000 for frame in active)), 3
            ) if active else 0.0,
        }

    thermal_rows: dict[str, object] = {}
    for label, frames_raw in _group(dataset.thermal, "label").items():
        frames = [frame for frame in frames_raw if isinstance(frame, ThermalFrame)]
        scores = [thermal_presence_score(frame, profile) for frame in frames]
        thermal_rows[label] = {
            "frames": len(frames),
            "median_contrast_deci_c": round(float(statistics.median(frame.contrast_deci_c for frame in frames)), 3),
            "median_score_x1000": round(float(statistics.median(scores)), 3),
            "positive_coverage": round(sum(score >= 550 for score in scores) / len(scores), 6),
        }

    empty_pressure = [frame for frame in dataset.pressure if frame.label == "空床"]
    negative_thermal = [
        frame for frame in dataset.thermal if frame.label == "空床" or "无加热" in frame.label
    ]
    return {
        "session_count": len(dataset.sessions),
        "pressure_frame_count": len(dataset.pressure),
        "thermal_frame_count": len(dataset.thermal),
        "bed_temperature_sample_count": len(dataset.bed_temperature),
        "invalid_payload_count": len(dataset.errors),
        "pressure_empty_false_positive_frames": sum(frame.active_points > 0 for frame in empty_pressure),
        "pressure_empty_frames": len(empty_pressure),
        "thermal_negative_positive_frames": sum(
            thermal_presence_score(frame, profile) >= 550 for frame in negative_thermal
        ),
        "thermal_negative_frames": len(negative_thermal),
        "pressure_by_label": pressure_rows,
        "thermal_by_label": thermal_rows,
    }


def _define(name: str, value: int, signed: bool = False) -> str:
    suffix = "" if signed and value < 0 else "U"
    return f"#define {name:<54} ({value}{suffix})"


def _define_hex(name: str, value: int) -> str:
    return f"#define {name:<54} (0x{value:08X}U)"


def render_header(profile: dict[str, object], metrics: dict[str, object]) -> str:
    rows = [
        "#ifndef FUSION_CALIBRATION_GENERATED_H",
        "#define FUSION_CALIBRATION_GENERATED_H",
        "",
        "/* Generated by fusion_training/calibrate_fusion.py. Do not hand edit. */",
        f'#define FUSION_CALIBRATION_DATASET_SHA256 "{profile["archive_sha256"]}"',
        _define_hex("FUSION_CALIBRATION_PROFILE_VERSION", int(profile["profile_version"])),
        _define("FUSION_CALIBRATION_SESSION_COUNT", int(metrics["session_count"])),
        _define("FUSION_CALIBRATION_PRESSURE_FRAME_COUNT", int(metrics["pressure_frame_count"])),
        _define("FUSION_CALIBRATION_THERMAL_FRAME_COUNT", int(metrics["thermal_frame_count"])),
        _define("FUSION_CAL_PRESSURE_ACTIVE_POINT", int(profile["pressure_active_point"])),
        _define("FUSION_CAL_PRESSURE_CONFIRM_FRAMES", int(profile["pressure_confirm_frames"])),
        _define("FUSION_CAL_PRESSURE_RELEASE_FRAMES", int(profile["pressure_release_frames"])),
        _define("FUSION_CAL_PRESSURE_EDGE_WIDTH", int(profile["pressure_edge_width"])),
        _define("FUSION_CAL_PRESSURE_EDGE_POSITIVE_X1000", int(profile["pressure_edge_positive_x1000"])),
        _define("FUSION_CAL_THERMAL_CONTRAST_LOW_DECI_C", int(profile["thermal_contrast_low_deci_c"])),
        _define("FUSION_CAL_THERMAL_CONTRAST_HIGH_DECI_C", int(profile["thermal_contrast_high_deci_c"])),
        _define("FUSION_CAL_THERMAL_HOTSPOT_DELTA_DECI_C", int(profile["thermal_hotspot_delta_deci_c"])),
        _define("FUSION_CAL_THERMAL_HOTSPOT_P99_GAP_DECI_C", int(profile["thermal_hotspot_p99_gap_deci_c"])),
        _define("FUSION_CAL_THERMAL_HOTSPOT_MIN_POINTS", int(profile["thermal_hotspot_min_points"])),
        _define("FUSION_CAL_THERMAL_HOTSPOT_FULL_POINTS", int(profile["thermal_hotspot_full_points"])),
        _define("FUSION_CAL_THERMAL_CONTRAST_WEIGHT_X1000", int(profile["thermal_contrast_weight_x1000"])),
        _define("FUSION_CAL_THERMAL_AREA_WEIGHT_X1000", int(profile["thermal_area_weight_x1000"])),
        _define("FUSION_CAL_THERMAL_BED_X_SLOPE_X1000", int(profile["thermal_bed_x_slope_x1000"]), signed=True),
        _define("FUSION_CAL_THERMAL_BED_X_INTERCEPT_X1000", int(profile["thermal_bed_x_intercept_x1000"])),
        _define("FUSION_CAL_THERMAL_EDGE_LOW_X1000", int(profile["thermal_edge_low_x1000"])),
        _define("FUSION_CAL_THERMAL_EDGE_HIGH_X1000", int(profile["thermal_edge_high_x1000"])),
        _define("FUSION_CAL_EDGE_PRESSURE_WEIGHT", int(profile["edge_pressure_weight"])),
        _define("FUSION_CAL_EDGE_VISION_WEIGHT", int(profile["edge_vision_weight"])),
        _define("FUSION_CAL_EDGE_THERMAL_WEIGHT", int(profile["edge_thermal_weight"])),
        _define("FUSION_CAL_POSITION_AGREEMENT_GOOD_X1000", int(profile["position_agreement_good_x1000"])),
        _define("FUSION_CAL_POSITION_AGREEMENT_BAD_X1000", int(profile["position_agreement_bad_x1000"])),
        "",
        "#endif /* FUSION_CALIBRATION_GENERATED_H */",
        "",
    ]
    return "\n".join(rows)


def render_report(archive: Path, profile: dict[str, object], metrics: dict[str, object], dataset: Dataset) -> str:
    lines = [
        "# 融合数据集 2.0 标定报告",
        "",
        f"- 输入：`{archive}`",
        f"- SHA-256：`{profile['archive_sha256']}`",
        f"- 会话：{metrics['session_count']} 个",
        f"- 压力矩阵：{metrics['pressure_frame_count']} 帧",
        f"- 热成像矩阵：{metrics['thermal_frame_count']} 帧",
        f"- 床温时序：{metrics['bed_temperature_sample_count']} 条有效样本",
        f"- 非法矩阵：{metrics['invalid_payload_count']} 帧",
        "",
        "## 标定结论",
        "",
        f"- M1616 空床 {metrics['pressure_empty_frames']} 帧全部为零；非零点阈值保持 1，空床误触发 {metrics['pressure_empty_false_positive_frames']} 帧。",
        f"- 热成像矩阵实际单位是 0.1°C。原融合层把阈值当成 0.01°C，导致真实热源证据几乎恒为零；现按 {profile['thermal_contrast_low_deci_c'] / 10:.1f} 至 {profile['thermal_contrast_high_deci_c'] / 10:.1f}°C 的稳健对比度标定。",
        f"- 空床/未加热热成像共 {metrics['thermal_negative_frames']} 帧，标定后热源阳性误报 {metrics['thermal_negative_positive_frames']} 帧。",
        f"- 热源面积只过滤少于 {profile['thermal_hotspot_min_points']} 点的孤立噪声；热证据以稳健温差为主，保留只露出小面积加热脸部的场景。",
        f"- 热成像横坐标到床面压力横坐标的仿射映射为 `x_bed = {profile['thermal_bed_x_slope_x1000'] / 1000:.3f} * x_thermal + {profile['thermal_bed_x_intercept_x1000'] / 1000:.3f}`。",
        "- 压力、热成像和床温并非同步采集，不能据此训练端到端联合分类器；本版只标定各模态证据，再由 RA8 状态机做融合。视觉和哭声模型未修改。",
        "",
        "## 压力会话回放",
        "",
        "| 标签 | 帧数 | 非零覆盖率 | 非零帧载荷中位数 | 边缘比例中位数 |",
        "|---|---:|---:|---:|---:|",
    ]
    for label, row in metrics["pressure_by_label"].items():
        lines.append(
            f"| {label} | {row['frames']} | {row['active_coverage'] * 100:.1f}% | "
            f"{row['median_total_active']:.1f} | {row['median_edge_ratio_x1000_active'] / 10:.1f}% |"
        )
    lines.extend(
        [
            "",
            "压力侧站立/躺卧会话存在 18 至 78 秒的零输出区段，说明这是接触缺失而不是可通过阈值继续降低解决的问题。融合层保留 2 帧确认和 32 帧释放保持，不用超长保持伪造压力；视觉和热成像继续参与在床判定。",
            "",
            "## 热成像会话回放",
            "",
            "| 标签 | 帧数 | P99-P1 中位数 | 热源分数中位数 | 阳性覆盖率 |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for label, row in metrics["thermal_by_label"].items():
        lines.append(
            f"| {label} | {row['frames']} | {row['median_contrast_deci_c'] / 10:.1f}°C | "
            f"{row['median_score_x1000'] / 10:.1f}% | {row['positive_coverage'] * 100:.1f}% |"
        )
    lines.extend(
        [
            "",
            "## 数据限制",
            "",
            "- 所有样本仍是 `unreviewed/unassigned`，文件夹名称是本次标定采用的真实场景标签。",
            "- 床温“空床”会话没有有效样本，因此不能从本包学习空床基线或医学温度阈值。",
            "- 左侧站立加热会话多数帧没有形成可区分热源；融合层将其视为热成像无强证据，而不是错误判为无人。",
            "- 数据只来自当前假人、配重、加热器和床面布置；换床垫、压力垫位置、摄像头角度或加热功率后必须重新标定。",
        ]
    )
    if dataset.errors:
        lines.extend(["", "## 读取错误", ""] + [f"- {error}" for error in dataset.errors])
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    digest = hashlib.sha256(args.archive.read_bytes()).hexdigest().upper()
    dataset = load_dataset(args.archive)
    profile = derive_profile(dataset, digest)
    metrics = build_metrics(dataset, profile)
    payload = {
        "profile": profile,
        "metrics": metrics,
        "sessions": dataset.sessions,
        "errors": dataset.errors,
    }
    if args.header:
        args.header.parent.mkdir(parents=True, exist_ok=True)
        args.header.write_text(render_header(profile, metrics), encoding="ascii")
    if args.profile:
        args.profile.parent.mkdir(parents=True, exist_ok=True)
        args.profile.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(render_report(args.archive, profile, metrics, dataset), encoding="utf-8")
    print(json.dumps(payload, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

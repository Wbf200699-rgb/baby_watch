from __future__ import annotations

import argparse
import csv
import json
import time
from datetime import datetime
from pathlib import Path

import numpy as np

FUSION_STATE_NAMES = {
    0: "normal", 1: "near-edge", 2: "out-of-bed", 3: "overheat",
    4: "underheat", 5: "sensor-fault", 6: "crying", 7: "face-covered",
    8: "standing", 9: "moving", 10: "sleeping", 11: "awake",
}

from ra8_protocol import (
    FrameParser,
    TYPE_ALARM_UPLOAD,
    TYPE_PRESSURE_UPLOAD,
    TYPE_STATUS_UPLOAD,
    decode_pressure_chunk,
    decode_thermal_chunk,
)


def require_serial():
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("Missing dependency: pip install pyserial") from exc
    return serial


def require_matplotlib():
    try:
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise SystemExit("Missing dependency: pip install matplotlib numpy") from exc
    return plt


class ThermalReassembler:
    def __init__(self) -> None:
        self.sequence: int | None = None
        self.width = 0
        self.height = 0
        self.samples: np.ndarray | None = None
        self.valid: np.ndarray | None = None
        self.chunk_count = 0
        self.completed_count = 0
        self.invalid_count = 0
        self.valid_percent = 0.0

    def add_chunk(self, payload: bytes) -> np.ndarray | None:
        sequence, width, height, offset, samples = decode_thermal_chunk(payload)
        total = width * height

        if self.sequence != sequence or self.samples is None or self.samples.size != total:
            self.sequence = sequence
            self.width = width
            self.height = height
            self.samples = np.full(total, np.nan, dtype=np.float32)
            self.valid = np.zeros(total, dtype=bool)
            self.chunk_count = 0

        count = len(samples)
        end = min(offset + count, total)
        if end <= offset:
            return None

        values = np.asarray(samples[: end - offset], dtype=np.float32) / 10.0
        values[values <= -3000.0] = np.nan
        self.samples[offset:end] = values
        self.valid[offset:end] = True
        self.chunk_count += 1

        if bool(np.all(self.valid)):
            matrix = self.samples.reshape((height, width)).copy()
            self.valid[:] = False
            self.completed_count += 1
            self.invalid_count = int(np.count_nonzero(~np.isfinite(matrix)))
            self.valid_percent = 100.0 * float(total - self.invalid_count) / float(total)
            return matrix

        return None


class PressureReassembler:
    def __init__(self) -> None:
        self.sequence: int | None = None
        self.width = 0
        self.height = 0
        self.samples: np.ndarray | None = None
        self.valid: np.ndarray | None = None
        self.completed_count = 0

    def add_chunk(self, payload: bytes) -> np.ndarray | None:
        sequence, width, height, offset, samples = decode_pressure_chunk(payload)
        total = width * height

        if self.sequence != sequence or self.samples is None or self.samples.size != total:
            self.sequence = sequence
            self.width = width
            self.height = height
            self.samples = np.full(total, np.nan, dtype=np.float32)
            self.valid = np.zeros(total, dtype=bool)

        count = len(samples)
        end = min(offset + count, total)
        if end <= offset:
            return None

        self.samples[offset:end] = np.asarray(samples[: end - offset], dtype=np.float32)
        self.valid[offset:end] = True

        if bool(np.all(self.valid)):
            matrix = self.samples.reshape((height, width)).copy()
            self.valid[:] = False
            self.completed_count += 1
            return matrix

        return None


def temp_x_matrix(status: dict) -> np.ndarray:
    matrix = np.full((3, 3), np.nan, dtype=np.float32)
    tmp = status.get("tmp", {})
    values = tmp.get("t_mc", [])
    mask = int(tmp.get("ok", 0))
    positions = [(0, 0), (0, 2), (1, 1), (2, 0), (2, 2)]

    for index, (row, col) in enumerate(positions):
        if index < len(values) and (mask & (1 << index)):
            matrix[row, col] = float(values[index]) / 1000.0

    return matrix


def pressure_raw_matrix(status: dict) -> np.ndarray | None:
    prs = status.get("prs", {})
    raw = prs.get("matrix") or prs.get("raw", [])
    if not raw:
        return None

    matrix = np.full((16, 16), np.nan, dtype=np.float32)
    flat = matrix.reshape(-1)
    for index, value in enumerate(raw[: flat.size]):
        try:
            flat[index] = float(value)
        except (TypeError, ValueError):
            flat[index] = np.nan

    return matrix


def pressure_display_matrix(
    matrix: np.ndarray,
    baseline: np.ndarray | None,
    gain: float,
    use_abs_delta: bool,
) -> np.ndarray:
    display = matrix.astype(np.float32, copy=True)
    if baseline is not None:
        display = display - baseline
        if use_abs_delta:
            display = np.abs(display)
    display *= gain
    return display


def open_logs(log_dir: Path):
    log_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    status_file = (log_dir / f"status_{stamp}.jsonl").open("a", encoding="utf-8")
    tmp_file = (log_dir / f"tmp117_{stamp}.csv").open("a", newline="", encoding="utf-8")
    tmp_writer = csv.writer(tmp_file)
    tmp_writer.writerow(["pc_time", "tick", "seq", "status", "ok_mask", "t0", "t1", "t2", "t3", "t4"])
    pressure_file = (log_dir / f"pressure_{stamp}.csv").open("a", newline="", encoding="utf-8")
    pressure_writer = csv.writer(pressure_file)
    pressure_writer.writerow(["pc_time", "sequence", "status", "ok_mask", "err_mask", *[f"p{i}" for i in range(256)]])
    return stamp, status_file, tmp_file, tmp_writer, pressure_file, pressure_writer


def main() -> int:
    parser = argparse.ArgumentParser(description="RA8P1 ESP32 protocol live parser/logger/visualizer")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM16")
    parser.add_argument("--baud", type=int, default=921600, help="UART baudrate")
    parser.add_argument("--log-dir", type=Path, default=Path(__file__).with_name("logs"))
    parser.add_argument("--no-gui", action="store_true", help="Only save logs, do not show matplotlib windows")
    parser.add_argument("--draw-hz", type=float, default=10.0, help="Maximum GUI redraw rate")
    parser.add_argument("--save-every", type=int, default=10, help="Save thermal NPZ every N completed frames")
    parser.add_argument("--pressure-gain", type=float, default=1.0, help="GUI-only pressure display gain")
    parser.add_argument(
        "--pressure-gamma",
        type=float,
        default=0.45,
        help="Pressure color gamma; values below 1 expand weak signals",
    )
    parser.add_argument(
        "--pressure-baseline-frames",
        type=int,
        default=20,
        help="Average this many startup frames for the micro-pressure view; 0 disables baseline subtraction",
    )
    parser.add_argument(
        "--pressure-signed",
        action="store_true",
        help="Show signed pressure delta instead of absolute delta when baseline is enabled",
    )
    args = parser.parse_args()

    serial = require_serial()
    plt = None if args.no_gui else require_matplotlib()

    stamp, status_file, tmp_file, tmp_writer, pressure_file, pressure_writer = open_logs(args.log_dir)
    thermal_npz_path = args.log_dir / f"thermal_frames_{stamp}.npz"
    thermal_frames: list[np.ndarray] = []

    parser_state = FrameParser(max_payload=4096)
    thermal = ThermalReassembler()
    pressure = PressureReassembler()
    last_status: dict | None = None
    last_thermal: np.ndarray | None = None
    last_pressure: np.ndarray | None = None
    pressure_baseline_accum: np.ndarray | None = None
    pressure_baseline: np.ndarray | None = None
    pressure_baseline_count = 0
    last_draw_time = 0.0

    if plt is not None:
        from matplotlib.colors import Normalize, PowerNorm

        plt.ion()
        fig, (ax_tmp, ax_thm, ax_prs, ax_prs_micro) = plt.subplots(1, 4, figsize=(19, 5))
        tmp_image = ax_tmp.imshow(np.full((3, 3), np.nan), cmap="coolwarm", vmin=20, vmax=40)
        thm_image = ax_thm.imshow(np.full((64, 80), np.nan), cmap="coolwarm", vmin=20, vmax=40)
        prs_image = ax_prs.imshow(
            np.zeros((16, 16), dtype=np.float32),
            cmap="viridis",
            norm=PowerNorm(gamma=max(args.pressure_gamma, 0.05), vmin=0.0, vmax=1.0),
        )
        prs_micro_image = ax_prs_micro.imshow(
            np.zeros((16, 16), dtype=np.float32),
            cmap="magma" if not args.pressure_signed else "coolwarm",
            norm=PowerNorm(gamma=max(args.pressure_gamma, 0.05), vmin=0.0, vmax=1.0),
        )
        ax_tmp.set_title("TMP117 X Matrix")
        ax_thm.set_title("Thermal 80x64")
        ax_prs.set_title("Pressure Absolute 16x16")
        ax_prs_micro.set_title("Pressure Micro 16x16")
        fig.colorbar(tmp_image, ax=ax_tmp, fraction=0.046)
        fig.colorbar(thm_image, ax=ax_thm, fraction=0.046)
        fig.colorbar(prs_image, ax=ax_prs, fraction=0.046)
        fig.colorbar(prs_micro_image, ax=ax_prs_micro, fraction=0.046)
        fig.tight_layout()
    else:
        Normalize = PowerNorm = None
        fig = tmp_image = thm_image = prs_image = prs_micro_image = None

    try:
        with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
            print(f"listening {args.port} @ {args.baud}")
            while True:
                data = ser.read(4096)
                if not data:
                    if plt is not None:
                        plt.pause(0.01)
                    continue

                for frame in parser_state.feed(data):
                    if frame.msg_type == TYPE_STATUS_UPLOAD:
                        status = json.loads(frame.payload.decode("utf-8"))
                        last_status = status
                        status_file.write(json.dumps(status, separators=(",", ":")) + "\n")
                        status_file.flush()

                        tmp = status.get("tmp", {})
                        temps = list(tmp.get("t_mc", []))
                        while len(temps) < 5:
                            temps.append("")
                        tmp_writer.writerow([
                            time.time(),
                            status.get("tick", ""),
                            tmp.get("seq", ""),
                            tmp.get("s", ""),
                            tmp.get("ok", ""),
                            *temps[:5],
                        ])
                        tmp_file.flush()

                        matrix = pressure_raw_matrix(status)
                        if matrix is not None:
                            last_pressure = matrix
                            if (
                                args.pressure_baseline_frames > 0
                                and pressure_baseline is None
                                and pressure_baseline_count < args.pressure_baseline_frames
                            ):
                                if pressure_baseline_accum is None:
                                    pressure_baseline_accum = np.zeros_like(matrix, dtype=np.float32)
                                pressure_baseline_accum += matrix
                                pressure_baseline_count += 1
                                if pressure_baseline_count >= args.pressure_baseline_frames:
                                    pressure_baseline = pressure_baseline_accum / float(pressure_baseline_count)
                            prs = status.get("prs", {})
                            pressure_writer.writerow([
                                time.time(),
                                prs.get("seq", ""),
                                prs.get("s", ""),
                                prs.get("ok", ""),
                                prs.get("err", ""),
                                *matrix.reshape(-1).astype(np.float32).tolist(),
                            ])
                            pressure_file.flush()

                    elif frame.msg_type == TYPE_ALARM_UPLOAD:
                        matrix = thermal.add_chunk(frame.payload)
                        if matrix is not None:
                            last_thermal = matrix
                            thermal_frames.append(matrix)
                            if args.save_every > 0 and (len(thermal_frames) % args.save_every) == 0:
                                np.savez_compressed(thermal_npz_path, frames=np.asarray(thermal_frames, dtype=np.float32))

                    elif frame.msg_type == TYPE_PRESSURE_UPLOAD:
                        matrix = pressure.add_chunk(frame.payload)
                        if matrix is not None:
                            last_pressure = matrix
                            if (
                                args.pressure_baseline_frames > 0
                                and pressure_baseline is None
                                and pressure_baseline_count < args.pressure_baseline_frames
                            ):
                                if pressure_baseline_accum is None:
                                    pressure_baseline_accum = np.zeros_like(matrix, dtype=np.float32)
                                pressure_baseline_accum += matrix
                                pressure_baseline_count += 1
                                if pressure_baseline_count >= args.pressure_baseline_frames:
                                    pressure_baseline = pressure_baseline_accum / float(pressure_baseline_count)
                            pressure_writer.writerow([
                                time.time(),
                                pressure.sequence,
                                "",
                                "",
                                "",
                                *matrix.reshape(-1).astype(np.int32).tolist(),
                            ])
                            pressure_file.flush()

                now = time.monotonic()
                draw_period = 1.0 / max(args.draw_hz, 0.1)
                if plt is not None and (now - last_draw_time) >= draw_period:
                    if last_status is not None:
                        tmp_image.set_data(temp_x_matrix(last_status))
                        fusion = last_status.get("fusion", {})
                        state_value = fusion.get("state", 0)
                        candidate_value = fusion.get("candidate", 0)
                        fig.suptitle(
                            f"Fusion {FUSION_STATE_NAMES.get(state_value, state_value)} "
                            f"candidate={FUSION_STATE_NAMES.get(candidate_value, candidate_value)} "
                            f"confidence={fusion.get('conf', 0)}% severity={fusion.get('severity', 0)} "
                            f"valid=0x{fusion.get('valid', 0):02X} faults=0x{fusion.get('faults', 0):02X} "
                            f"occupied={fusion.get('occupied', 0)}"
                        )
                    if last_thermal is not None:
                        finite = last_thermal[np.isfinite(last_thermal)]
                        if finite.size:
                            lo = max(0.0, float(np.nanpercentile(finite, 2)) - 1.0)
                            hi = min(80.0, float(np.nanpercentile(finite, 98)) + 1.0)
                            thm_image.set_clim(lo, hi)
                        thm_image.set_data(last_thermal)
                        ax_thm.set_title(
                            f"Thermal 80x64 valid={thermal.valid_percent:.1f}% "
                            f"frame={thermal.completed_count} chunks={thermal.chunk_count} "
                            f"crc_bad={parser_state.bad_crc_count}"
                        )
                    if last_pressure is not None:
                        raw_finite = last_pressure[np.isfinite(last_pressure)]
                        raw_min = float(np.nanmin(raw_finite)) if raw_finite.size else 0.0
                        raw_max = float(np.nanmax(raw_finite)) if raw_finite.size else 0.0
                        raw_sum = float(np.nansum(np.maximum(last_pressure, 0.0)))
                        raw_active = int(np.count_nonzero(np.abs(raw_finite) > 0.0))
                        raw_display = np.maximum(last_pressure, 0.0)
                        raw_positive = raw_display[np.isfinite(raw_display) & (raw_display > 0.0)]
                        raw_hi = max(
                            1.0,
                            float(np.nanpercentile(raw_positive, 99.0)) if raw_positive.size else 1.0,
                        )
                        prs_image.set_norm(
                            PowerNorm(
                                gamma=max(args.pressure_gamma, 0.05),
                                vmin=0.0,
                                vmax=raw_hi,
                            )
                        )
                        prs_image.set_data(raw_display)

                        pressure_micro = pressure_display_matrix(
                            last_pressure,
                            pressure_baseline,
                            args.pressure_gain,
                            not args.pressure_signed,
                        )
                        micro_finite = pressure_micro[np.isfinite(pressure_micro)]
                        micro_abs = np.abs(micro_finite)
                        micro_max = float(np.nanmax(micro_abs)) if micro_abs.size else 0.0
                        micro_rms = float(np.sqrt(np.nanmean(np.square(micro_finite)))) if micro_finite.size else 0.0
                        micro_active = int(np.count_nonzero(micro_abs > 0.0))
                        micro_hi = max(
                            1.0,
                            float(np.nanpercentile(micro_abs[micro_abs > 0.0], 99.0))
                            if np.any(micro_abs > 0.0)
                            else 1.0,
                        )
                        if args.pressure_signed:
                            prs_micro_image.set_norm(Normalize(vmin=-micro_hi, vmax=micro_hi))
                        else:
                            prs_micro_image.set_norm(
                                PowerNorm(
                                    gamma=max(args.pressure_gamma, 0.05),
                                    vmin=0.0,
                                    vmax=micro_hi,
                                )
                            )
                        prs_micro_image.set_data(pressure_micro)
                        prs = (last_status or {}).get("prs", {})
                        ax_prs.set_title(
                            f"Pressure Absolute frame={pressure.completed_count} "
                            f"s={prs.get('s', '')} ok={prs.get('ok', '')} "
                            f"err={prs.get('err', '')} fsp={prs.get('fsp', '')} "
                            f"raw={raw_min:.0f}..{raw_max:.0f} sum={raw_sum:.0f} "
                            f"active={raw_active} gamma={args.pressure_gamma:g}"
                        )
                        ax_prs_micro.set_title(
                            f"Pressure Micro base={pressure_baseline_count}/{args.pressure_baseline_frames} "
                            f"gain={args.pressure_gain:g} max={micro_max:.1f} "
                            f"rms={micro_rms:.2f} active={micro_active}"
                        )
                    fig.canvas.draw_idle()
                    plt.pause(0.01)
                    last_draw_time = now

    except KeyboardInterrupt:
        print("stopped")
    finally:
        if thermal_frames:
            np.savez_compressed(thermal_npz_path, frames=np.asarray(thermal_frames, dtype=np.float32))
        status_file.close()
        tmp_file.close()
        pressure_file.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

from pathlib import Path
import sys
from unittest import TestCase


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.diagnostics import build_sensor_diagnostics  # noqa: E402


class TransientSensorDiagnosticsTest(TestCase):
    def test_thermal_transition_is_warning_and_exposes_quality_counters(self) -> None:
        diagnostics = build_sensor_diagnostics(
            {
                "sensors": {
                    "thermal": {
                        "s": 3,
                        "rx": 10248,
                        "valid": 5120,
                        "crc_rx": 1234,
                        "crc_calc": 1234,
                        "frame_quality": 1,
                        "p01_cd": 286,
                        "p99_cd": 319,
                        "mean_cd": 301,
                        "transient_drop_count": 7,
                        "generation_retry_count": 3,
                        "last_good_sequence": 91,
                    }
                },
                "inference": {"valid": 1 << 1, "faults": 0},
                "health": {"ra8_online": True},
            },
            {},
            device_online=True,
        )
        item = next(item for item in diagnostics["items"] if item["id"] == "thermal")
        self.assertEqual(item["status"], "warning")
        self.assertIn("过渡帧已过滤", item["summary"])
        self.assertEqual(item["metrics"]["transient_drop_count"], 7)
        self.assertEqual(item["metrics"]["generation_retry_count"], 3)

    def test_partial_temperature_scan_is_warning_while_last_good_frame_is_valid(self) -> None:
        diagnostics = build_sensor_diagnostics(
            {
                "sensors": {
                    "bed_temperature": {
                        "s": 3,
                        "ok": 0x1B,
                        "err": 0x04,
                        "t_mc": [24125, 24250, 0, 24400, 24500],
                        "addr": [0x48, 0x48, 0, 0x48, 0x48],
                    }
                },
                "inference": {"valid": 1 << 2, "faults": 0},
                "health": {"ra8_online": True},
            },
            {},
            device_online=True,
        )
        item = next(item for item in diagnostics["items"] if item["id"] == "bed_temperature")
        self.assertEqual(item["status"], "warning")
        self.assertIn("沿用上一轮完整扫描", item["detail"])

    def test_single_camera_failure_is_warning_until_fusion_timeout(self) -> None:
        telemetry = {
            "sensors": {
                "camera": {
                    "s": 6,
                    "probe": 2,
                    "probe_ok": 1,
                    "fsp": 20,
                    "ver": "VC0703 1.00",
                }
            },
            "inference": {"valid": 1 << 4, "faults": 0, "vv": [7, 6000]},
            "health": {"ra8_online": True},
        }
        diagnostics = build_sensor_diagnostics(telemetry, {}, device_online=True)
        item = next(item for item in diagnostics["items"] if item["id"] == "camera")
        self.assertEqual(item["status"], "warning")
        self.assertIn("自动重探测", item["summary"])

        telemetry["inference"] = {"valid": 0, "faults": 1 << 4, "vv": [7, 16000]}
        diagnostics = build_sensor_diagnostics(telemetry, {}, device_online=True)
        item = next(item for item in diagnostics["items"] if item["id"] == "camera")
        self.assertEqual(item["status"], "fault")
        self.assertEqual(item["summary"], "摄像头持续无有效帧")

    def test_local_matrix_sensor_is_not_reported_realtime_when_upload_has_no_frames(self) -> None:
        telemetry = {
            "sensors": {
                "thermal": {
                    "s": 3,
                    "rx": 10248,
                    "valid": 5120,
                    "crc_rx": 1234,
                    "crc_calc": 1234,
                    "frame_quality": 0,
                },
                "pressure": {"s": 3, "ok": 7, "fsp": 0},
            },
            "inference": {"valid": (1 << 1) | (1 << 3), "faults": 0},
            "health": {
                "ra8_online": True,
                "gateway": {
                    "matrix_connected": True,
                    "thermal_received_frames": 0,
                    "thermal_age_ms": None,
                    "thermal_fps": 0.0,
                    "pressure_received_frames": 0,
                    "pressure_age_ms": None,
                    "pressure_fps": 0.0,
                },
            },
        }

        diagnostics = build_sensor_diagnostics(telemetry, {}, device_online=True)
        thermal = next(item for item in diagnostics["items"] if item["id"] == "thermal")
        pressure = next(item for item in diagnostics["items"] if item["id"] == "pressure")

        self.assertEqual(thermal["status"], "warning")
        self.assertIn("本地正常", thermal["summary"])
        self.assertEqual(pressure["status"], "warning")
        self.assertIn("本地正常", pressure["summary"])
        self.assertEqual(thermal["metrics"]["matrix_received_frames"], 0)
        self.assertEqual(pressure["metrics"]["matrix_received_frames"], 0)
        pressure_uart = next(channel for channel in pressure["channels"] if channel["id"] == "uart")
        pressure_matrix = next(channel for channel in pressure["channels"] if channel["id"] == "matrix")
        self.assertEqual(pressure_uart["status"], "ok")
        self.assertEqual(pressure_matrix["status"], "warning")

    def test_fresh_matrix_upload_preserves_healthy_sensor_status(self) -> None:
        diagnostics = build_sensor_diagnostics(
            {
                "sensors": {
                    "pressure": {"s": 3, "ok": 7, "fsp": 0},
                },
                "inference": {"valid": 1 << 3, "faults": 0},
                "health": {
                    "ra8_online": True,
                    "gateway": {
                        "matrix_connected": True,
                        "pressure_received_frames": 12,
                        "pressure_age_ms": 125,
                        "pressure_fps": 7.8,
                    },
                },
            },
            {},
            device_online=True,
        )
        pressure = next(item for item in diagnostics["items"] if item["id"] == "pressure")
        self.assertEqual(pressure["status"], "ok")
        self.assertEqual(pressure["metrics"]["matrix_age_ms"], 125)

    def test_mounted_global_sd_clears_stale_camera_no_sd_diagnostic(self) -> None:
        diagnostics = build_sensor_diagnostics(
            {
                "sensors": {
                    "microphone": {"sdr": 1, "sds": 7},
                    "camera": {
                        "s": 4,
                        "probe": 1,
                        "probe_ok": 1,
                        "ver": "VC0703 1.00",
                        "wst": 1,
                        "werr": 0,
                    },
                },
                "inference": {"valid": 1 << 4, "faults": 0},
                "health": {"ra8_online": True},
            },
            {},
            device_online=True,
        )

        camera = next(item for item in diagnostics["items"] if item["id"] == "camera")
        writer = next(channel for channel in camera["channels"] if channel["id"] == "sd_writer")
        self.assertEqual(camera["status"], "ok")
        self.assertEqual(writer["status"], "ok")
        self.assertIn("等待图像", writer["detail"])

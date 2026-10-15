from pathlib import Path
import sys
from typing import Any
from unittest import TestCase


SERVER_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVER_ROOT))

from app.diagnostics import build_sensor_diagnostics  # noqa: E402
from app.models import TelemetryUpload  # noqa: E402


def environment_item(
    environment: dict[str, Any],
    *,
    runtime_ms: int = 120_000,
    valid_mask: int = 1 << 5,
    fault_mask: int = 0,
) -> dict:
    diagnostics = build_sensor_diagnostics(
        {
            "ra8_tick": runtime_ms,
            "sensors": {"environment": environment},
            "inference": {"valid": valid_mask, "faults": fault_mask},
            "health": {"ra8_online": True},
        },
        {},
        device_online=True,
    )
    return next(item for item in diagnostics["items"] if item["id"] == "environment")


class EnvironmentSensorDiagnosticsTest(TestCase):
    def test_complete_fresh_sample_is_healthy_despite_historical_errors(self) -> None:
        item = environment_item(
            {
                "seq": 17,
                "s": 3,
                "co2": 812,
                "t_mc": 24_375,
                "rh_mp": 48_250,
                "ready": 1,
                "crc_err": 2,
                "i2c_err": 1,
                "to": 3,
                "rec": 3,
                "fsp": 0,
                "last": 115_000,
            }
        )

        self.assertEqual(item["status"], "ok")
        self.assertEqual(item["summary"], "CO₂ 812 ppm")
        self.assertEqual(item["metrics"]["sample_age_ms"], 5_000)
        self.assertEqual(item["metrics"]["crc_error_count"], 2)
        self.assertFalse(item["metrics"]["stale"])

    def test_recovering_state_is_transient_without_fusion_fault(self) -> None:
        item = environment_item(
            {
                "seq": 18,
                "s": 8,
                "co2": 812,
                "t_mc": 24_375,
                "rh_mp": 48_250,
                "ready": 0,
                "crc_err": 1,
                "i2c_err": 1,
                "to": 1,
                "rec": 1,
                "fsp": 0,
                "last": 119_000,
            },
            valid_mask=0,
        )

        self.assertEqual(item["status"], "warning")
        self.assertEqual(item["summary"], "正在恢复")

    def test_driver_crc_error_and_fusion_fault_are_reported(self) -> None:
        item = environment_item(
            {
                "seq": 19,
                "s": 5,
                "co2": 812,
                "t_mc": 24_375,
                "rh_mp": 48_250,
                "ready": 0,
                "crc_err": 4,
                "i2c_err": 0,
                "to": 0,
                "rec": 1,
                "fsp": 0,
                "last": 119_000,
            },
            valid_mask=0,
            fault_mask=1 << 5,
        )

        self.assertEqual(item["status"], "fault")
        self.assertIn("CRC 错误", item["detail"])
        self.assertTrue(item["metrics"]["fusion_fault"])

    def test_fusion_fault_remains_visible_when_gateway_sensor_object_is_missing(self) -> None:
        diagnostics = build_sensor_diagnostics(
            {
                "ra8_tick": 120_000,
                "sensors": {},
                "inference": {"valid": 0, "faults": 1 << 5},
                "health": {"ra8_online": True},
            },
            {},
            device_online=True,
        )
        item = next(item for item in diagnostics["items"] if item["id"] == "environment")

        self.assertEqual(item["status"], "fault")
        self.assertEqual(item["summary"], "融合层报告环境传感器故障")

    def test_old_last_good_tick_is_stale(self) -> None:
        item = environment_item(
            {
                "seq": 20,
                "s": 3,
                "co2": 690,
                "t_mc": 23_000,
                "rh_mp": 50_000,
                "ready": 1,
                "crc_err": 0,
                "i2c_err": 0,
                "to": 0,
                "rec": 0,
                "fsp": 0,
                "last": 40_000,
            },
            runtime_ms=120_000,
        )

        self.assertEqual(item["status"], "fault")
        self.assertTrue(item["metrics"]["stale"])
        self.assertEqual(item["metrics"]["sample_age_ms"], 80_000)

    def test_tick_domain_restart_does_not_create_false_stale_alarm(self) -> None:
        item = environment_item(
            {
                "seq": 1,
                "s": 3,
                "co2": 650,
                "t_mc": 22_500,
                "rh_mp": 45_000,
                "ready": 1,
                "crc_err": 0,
                "i2c_err": 0,
                "to": 0,
                "rec": 0,
                "fsp": 0,
                "last": 90_000,
            },
            runtime_ms=1_000,
        )

        self.assertEqual(item["status"], "ok")
        self.assertIsNone(item["metrics"]["sample_age_ms"])
        self.assertFalse(item["metrics"]["stale"])

    def test_co2_above_accuracy_band_is_still_a_valid_sensor_output(self) -> None:
        item = environment_item(
            {
                "seq": 21,
                "s": 3,
                "co2": 8_000,
                "t_mc": 23_500,
                "rh_mp": 47_000,
                "ready": 1,
                "crc_err": 0,
                "i2c_err": 0,
                "to": 0,
                "rec": 0,
                "fsp": 0,
                "last": 119_000,
            }
        )

        self.assertEqual(item["status"], "ok")
        self.assertEqual(item["summary"], "CO₂ 8000 ppm")

    def test_bme688_complete_sample_is_combined_with_scd41(self) -> None:
        item = environment_item(
            {
                "seq": 22,
                "s": 3,
                "co2": 720,
                "t_mc": 23_500,
                "rh_mp": 47_000,
                "ready": 1,
                "last": 119_000,
                "bme688": {
                    "s": 3,
                    "addr": 0x76,
                    "t_mc": 24_125,
                    "rh_mp": 48_500,
                    "p_pa": 100_825,
                    "gas_ohm": 125_400,
                    "flags": 0x07,
                    "last": 118_000,
                    "err": 2,
                },
            }
        )

        self.assertEqual(item["status"], "ok")
        self.assertIn("气压 1008.25 hPa", item["summary"])
        channels = {channel["id"]: channel for channel in item["channels"]}
        self.assertEqual(channels["bme688_i2c"]["status"], "ok")
        self.assertTrue(item["metrics"]["bme688"]["gas_measurement_valid"])
        self.assertEqual(item["metrics"]["bme688"]["gas_resistance_ohm"], 125_400)
        self.assertEqual(item["metrics"]["bme688"]["error_count"], 2)
        self.assertEqual(
            item["metrics"]["bme688"]["gas_semantics"],
            "raw_resistance_only_no_bsec_voc_or_iaq",
        )
        self.assertIn("不能解释为 VOC 或 IAQ", item["detail"])

    def test_bme688_heater_warmup_is_warning_not_fake_air_quality(self) -> None:
        item = environment_item(
            {
                "seq": 23,
                "s": 3,
                "co2": 690,
                "t_mc": 23_000,
                "rh_mp": 50_000,
                "ready": 1,
                "last": 119_000,
                "bme688": {
                    "s": 3,
                    "addr": 0x77,
                    "t_mc": 23_400,
                    "rh_mp": 49_800,
                    "p_pa": 100_600,
                    "gas_ohm": 0,
                    "flags": 0x01,
                    "last": 119_000,
                    "err": 0,
                },
            }
        )

        self.assertEqual(item["status"], "warning")
        self.assertIn("气体加热器预热中", item["summary"])
        self.assertFalse(item["metrics"]["bme688"]["gas_measurement_valid"])
        self.assertTrue(item["metrics"]["bme688"]["climate_values_valid"])
        self.assertFalse(item["metrics"]["bme688"]["gas_value_valid"])

    def test_fresh_bme688_sample_stays_healthy_between_measurement_frames(self) -> None:
        item = environment_item(
            {
                "seq": 23,
                "s": 3,
                "co2": 690,
                "t_mc": 23_000,
                "rh_mp": 50_000,
                "ready": 1,
                "last": 119_000,
                "bme688": {
                    "s": 3,
                    "addr": 0x77,
                    "t_mc": 23_400,
                    "rh_mp": 49_800,
                    "p_pa": 100_600,
                    "gas_ohm": 80_000,
                    "flags": 0x06,
                    "last": 119_000,
                    "err": 0,
                },
            }
        )

        self.assertEqual(item["status"], "ok")
        self.assertFalse(item["metrics"]["bme688"]["new_data"])
        self.assertTrue(item["metrics"]["bme688"]["gas_measurement_valid"])

    def test_stale_bme688_sample_is_a_fault(self) -> None:
        item = environment_item(
            {
                "seq": 24,
                "s": 3,
                "co2": 700,
                "t_mc": 23_100,
                "rh_mp": 49_000,
                "ready": 1,
                "last": 119_000,
                "bme688": {
                    "s": 3,
                    "addr": 0x76,
                    "t_mc": 23_800,
                    "rh_mp": 49_500,
                    "p_pa": 100_700,
                    "gas_ohm": 90_000,
                    "flags": 0x07,
                    "last": 50_000,
                    "err": 0,
                },
            },
        )

        self.assertEqual(item["status"], "fault")
        self.assertTrue(item["metrics"]["bme688"]["stale"])
        self.assertEqual(item["metrics"]["bme688"]["sample_age_ms"], 70_000)


class Bme688TelemetryModelTest(TestCase):
    def test_migration_alias_is_normalized_under_environment(self) -> None:
        bme688 = {
            "s": 3,
            "addr": 0x76,
            "t_mc": 24_125,
            "rh_mp": 48_500,
            "p_pa": 100_825,
            "gas_ohm": 125_400,
            "flags": 0x07,
            "last": 118_000,
            "err": 0,
        }
        payload = TelemetryUpload.model_validate(
            {
                "sequence": 30,
                "sensors": {
                    "environment": {"co2": 720},
                    "bme688": bme688,
                },
            }
        ).model_dump()

        self.assertNotIn("bme688", payload["sensors"])
        self.assertEqual(payload["sensors"]["environment"]["co2"], 720)
        self.assertEqual(payload["sensors"]["environment"]["bme688"], bme688)

    def test_canonical_fields_win_when_alias_is_also_present(self) -> None:
        payload = TelemetryUpload.model_validate(
            {
                "sequence": 31,
                "sensors": {
                    "bme688": {"s": 2, "addr": 0x77, "p_pa": 99_000},
                    "environment": {
                        "bme688": {"s": 3, "addr": 0x76, "p_pa": 100_100},
                    },
                },
            }
        ).model_dump()

        bme688 = payload["sensors"]["environment"]["bme688"]
        self.assertEqual(bme688["s"], 3)
        self.assertEqual(bme688["addr"], 0x76)
        self.assertEqual(bme688["p_pa"], 100_100)


class EnvironmentSensorUiContractTest(TestCase):
    def test_dashboard_exposes_environment_cards_and_field_mapping(self) -> None:
        index = (SERVER_ROOT / "static" / "index.html").read_text(encoding="utf-8")
        script = (SERVER_ROOT / "static" / "app.js").read_text(encoding="utf-8")

        self.assertIn('id="environment-co2"', index)
        self.assertIn('id="environment-climate"', index)
        self.assertIn('id="bme688-climate"', index)
        self.assertIn('id="bme688-pressure"', index)
        self.assertIn('id="bme688-gas"', index)
        self.assertIn('id="bme688-gas-chart"', index)
        self.assertIn("thermal-display.js?v=20260731-thermal-quality-1", index)
        self.assertIn("matrix-display.js?v=20260801-pressure-ai-1", index)
        self.assertIn("app.js?v=20260821-resource-recording-1", index)
        self.assertIn("telemetry.sensors?.environment", script)
        self.assertIn("environment.co2", script)
        self.assertIn("environment.t_mc", script)
        self.assertIn("environment.rh_mp", script)
        self.assertIn('get(telemetry, "sensors.environment.bme688")', script)
        self.assertIn("bme688.t_mc", script)
        self.assertIn("bme688.rh_mp", script)
        self.assertIn("bme688.p_pa", script)
        self.assertIn("bme688.gas_ohm", script)
        self.assertIn("未运行 BSEC，不是 VOC/IAQ", script)
        self.assertIn('environment: "环境传感器"', script)
        self.assertIn("Number(inference.valid || 0) & (1 << 5)", script)
        self.assertIn("Number(inference.faults || 0) & (1 << 5)", script)

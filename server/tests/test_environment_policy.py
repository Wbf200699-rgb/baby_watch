from pathlib import Path
import sys
from unittest import TestCase


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.environment_policy import (  # noqa: E402
    DEFAULT_ENVIRONMENT_THRESHOLDS,
    evaluate_environment,
)


def telemetry(*, presence: int, flags: int = 0x06) -> dict[str, object]:
    return {
        "sensors": {
            "environment": {
                "co2": 2500,
                "t_mc": 31_000,
                "rh_mp": 75_000,
                "bme688": {"s": 3, "flags": flags, "gas_ohm": 8_000},
            }
        },
        "inference": {"fusion": {"ctx": [presence]}},
    }


class EnvironmentPolicyTest(TestCase):
    def test_all_single_thresholds_trigger_only_when_baby_is_in_bed(self) -> None:
        result = evaluate_environment(telemetry(presence=1), DEFAULT_ENVIRONMENT_THRESHOLDS)

        self.assertTrue(result["baby_in_bed"])
        self.assertTrue(result["active"])
        self.assertEqual(
            {item["name"] for item in result["exceeded"]},
            {"co2", "temperature", "humidity", "gas_resistance"},
        )

    def test_empty_bed_suppresses_environment_warning(self) -> None:
        result = evaluate_environment(telemetry(presence=2), DEFAULT_ENVIRONMENT_THRESHOLDS)

        self.assertFalse(result["baby_in_bed"])
        self.assertFalse(result["active"])
        self.assertEqual(result["exceeded"], [])

    def test_invalid_bme_gas_sample_cannot_trigger_gas_threshold(self) -> None:
        result = evaluate_environment(telemetry(presence=1, flags=0), DEFAULT_ENVIRONMENT_THRESHOLDS)

        self.assertFalse(result["observed"]["bme_gas_valid"])
        self.assertNotIn("gas_resistance", {item["name"] for item in result["exceeded"]})

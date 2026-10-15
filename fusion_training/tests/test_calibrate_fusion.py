import json
import sys
import unittest
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "fusion_training"))

from calibrate_fusion import (  # noqa: E402
    ThermalFrame,
    _rank_percentile,
    _thermal_frame,
    thermal_presence_score,
)


class FusionCalibrationTest(unittest.TestCase):
    def test_checked_in_profile_is_traceable_and_rejects_negative_sessions(self) -> None:
        payload = json.loads(
            (ROOT / "fusion_training/artifacts/fusion_dataset_2_0_profile.json").read_text(encoding="utf-8")
        )
        profile = payload["profile"]
        metrics = payload["metrics"]
        self.assertEqual(profile["profile_version"], 0x20260803)
        self.assertEqual(
            profile["archive_sha256"],
            "F8EB1703083CB9F6E644F6500295AC449F1A79A218F733848AEA7E725DF44FCB",
        )
        self.assertEqual(metrics["invalid_payload_count"], 0)
        self.assertEqual(metrics["pressure_empty_false_positive_frames"], 0)
        self.assertEqual(metrics["thermal_negative_positive_frames"], 0)
        self.assertEqual(profile["thermal_hotspot_min_points"], 8)

    def test_rank_percentile_matches_embedded_integer_rank(self) -> None:
        values = np.arange(100, dtype=np.int16)
        self.assertEqual(_rank_percentile(values, 1), 0)
        self.assertEqual(_rank_percentile(values, 99), 98)

    def test_thermal_frame_uses_deci_c_hotspot_thresholds(self) -> None:
        matrix = np.full((64, 80), 300, dtype="<i2")
        matrix[20:30, 30:40] = 410
        frame = _thermal_frame("heated", matrix.tobytes())
        self.assertGreater(frame.contrast_deci_c, 90)
        self.assertGreater(frame.hotspot_points, 50)
        self.assertLess(frame.hotspot_x_x1000, 600)

    def test_presence_score_rejects_flat_frame_and_accepts_hotspot(self) -> None:
        profile = {
            "thermal_contrast_low_deci_c": 35,
            "thermal_contrast_high_deci_c": 90,
            "thermal_hotspot_min_points": 40,
            "thermal_hotspot_full_points": 224,
            "thermal_contrast_weight_x1000": 850,
            "thermal_area_weight_x1000": 150,
        }
        flat = ThermalFrame("empty", 300, 330, 310, 30, 80, 500, 500)
        heated = ThermalFrame("heated", 300, 420, 320, 120, 120, 500, 500)
        self.assertLess(thermal_presence_score(flat, profile), 200)
        self.assertGreaterEqual(thermal_presence_score(heated, profile), 550)


if __name__ == "__main__":
    unittest.main()

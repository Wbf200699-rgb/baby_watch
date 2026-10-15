from pathlib import Path
import sys
from unittest import TestCase


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.overlay import (  # noqa: E402
    build_fusion_view,
    build_vision_overlay,
    map_point,
    prepare_calibration,
    prepare_thermal_calibration,
    solve_homography,
    solve_similarity,
)


class OverlayTest(TestCase):
    def test_identity_homography_preserves_coordinates(self) -> None:
        points = [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]]
        matrix = solve_homography(points, points)
        x, y = map_point(matrix, 0.25, 0.75)
        self.assertAlmostEqual(x, 0.25, places=7)
        self.assertAlmostEqual(y, 0.75, places=7)

    def test_two_point_thermal_similarity_maps_face_and_body(self) -> None:
        source = [[0.426, 0.206], [0.541, 0.541]]
        target = [[0.78, 0.27], [0.47, 0.43]]
        matrix = solve_similarity(source, target)
        for expected, point in zip(target, source):
            mapped = map_point(matrix, *point)
            self.assertAlmostEqual(mapped[0], expected[0], places=6)
            self.assertAlmostEqual(mapped[1], expected[1], places=6)

        thermal = prepare_thermal_calibration(
            enabled=True,
            source_points=source,
            target_points=target,
            opacity=0.32,
        )
        self.assertTrue(thermal["enabled"])
        self.assertEqual(thermal["status"], "calibrated")
        self.assertEqual(len(thermal["matrix"]), 9)

    def test_calibrated_detection_box_is_mapped_to_s3_coordinates(self) -> None:
        calibration = prepare_calibration(
            enabled=True,
            mode="homography",
            source_points=[[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
            target_points=[[0.1, 0.2], [0.9, 0.2], [0.9, 0.8], [0.1, 0.8]],
        )
        overlay = build_vision_overlay(
            {
                "vision": {"q": 4, "f": 3, "w": 320, "h": 240, "fe": 1, "d": [[0, 900, 80, 60, 160, 120]]},
                "inference": {"vv": [4, 100, 1, 1, 0, 200]},
            },
            calibration,
        )
        self.assertFalse(overlay["stale"])
        self.assertEqual(len(overlay["boxes"]), 1)
        box = overlay["boxes"][0]
        self.assertAlmostEqual(box["x"], 0.3, places=6)
        self.assertAlmostEqual(box["y"], 0.35, places=6)
        self.assertAlmostEqual(box["width"], 0.4, places=6)
        self.assertAlmostEqual(box["height"], 0.3, places=6)
        self.assertFalse(overlay["thermal"]["enabled"])

    def test_stale_detection_remains_visible_until_hard_expiry(self) -> None:
        calibration = prepare_calibration(
            enabled=True,
            mode="identity",
            source_points=[[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
            target_points=[[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
        )
        telemetry = {
            "vision": {"q": 4, "f": 3, "w": 320, "h": 240, "d": [[0, 900, 80, 60, 160, 120]]},
            "inference": {"vv": [4, 12001, 1, 1, 0, 200]},
        }
        overlay = build_vision_overlay(telemetry, calibration)
        self.assertTrue(overlay["stale"])
        self.assertFalse(overlay["expired"])
        self.assertEqual(len(overlay["boxes"]), 1)

        telemetry["inference"]["vv"][1] = 30001
        self.assertTrue(build_vision_overlay(telemetry, calibration)["expired"])

    def test_normal_six_second_inference_interval_is_not_stale(self) -> None:
        calibration = prepare_calibration(
            enabled=True,
            mode="identity",
            source_points=[[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
            target_points=[[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
        )
        overlay = build_vision_overlay(
            {
                "vision": {"q": 8, "f": 7, "w": 320, "h": 240, "d": []},
                "inference": {"vv": [8, 6000, 1, 1, 0, 300]},
            },
            calibration,
        )
        self.assertFalse(overlay["stale"])
        self.assertFalse(overlay["expired"])

    def test_parallel_context_and_incident_are_semantically_exposed(self) -> None:
        view = build_fusion_view(
            {
                "inference": {
                    "sv": 7,
                    "ev": 33,
                    "state": 7,
                    "severity": 5,
                    "ctx": [1, 2, 3, 3, 2, 2, 0, 1, 1, 0, 1],
                    "alarm": [3, 6, 42, 3, 1, 64],
                    "ai": [[6, 3, 3, 5, 92, 16, 42, 1200]],
                    "ex": [720, 2, 800, 650, 410, 90, 780, 330, 125, 430, 280,
                           324, 30200, 31500, 30900, 1300, 2, 727, 640, 910, 0x20260803],
                    "comfort": [10, 8, 92, 1, 1],
                }
            }
        )
        self.assertEqual(view["contexts"]["airway_view"]["label"], "covered")
        self.assertEqual(view["alarm"]["label"], "L3")
        self.assertEqual(view["active_incidents"][0]["type_label"], "airway_covered")
        self.assertTrue(view["evidence"]["available"])
        self.assertEqual(view["evidence"]["presence_source_count"], 2)
        self.assertEqual(view["evidence"]["thermal_hotspot"]["x_x1000"], 430)
        self.assertEqual(view["evidence"]["bed_temperature"]["hotspot_region"], 2)
        self.assertEqual(view["evidence"]["calibration"]["thermal_bed_x_x1000"], 727)
        self.assertEqual(view["evidence"]["calibration"]["position_agreement_x1000"], 910)
        self.assertEqual(view["evidence"]["calibration"]["profile_version"], 0x20260803)
        self.assertEqual(view["comfort"]["recommended_light"], "on")
        self.assertFalse(view["comfort"]["recommend_pat"])
        self.assertTrue(view["comfort"]["caregiver_check"])
        self.assertEqual(view["comfort"]["blocked_reasons"], ["pat_blocked_by_danger"])
        self.assertTrue(view["comfort"]["requires_confirmation"])

from __future__ import annotations

import io
import json
import zipfile
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from unittest import TestCase

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app


class TrainingWorkflowTest(TestCase):
    def setUp(self) -> None:
        self.temp = TemporaryDirectory()
        data = Path(self.temp.name)
        root = Path(__file__).resolve().parents[1]
        settings = Settings(
            root,
            data,
            data / "db.sqlite3",
            data / "artifacts",
            {"dev": "secret"},
        )
        self.context = TestClient(create_app(settings))
        self.client = self.context.__enter__()
        self.headers = {"X-Device-ID": "dev", "X-Device-Key": "secret"}

    def tearDown(self) -> None:
        self.context.__exit__(None, None, None)
        self.temp.cleanup()

    def create_session(self, kind: str, label: str = "", default_split: str = "unassigned") -> str:
        response = self.client.post(
            "/api/dashboard/sessions",
            json={
                "device_id": "dev",
                "name": f"{kind} trial",
                "target_labels": [label] if label else [],
                "metadata": {
                    "data_kind": kind,
                    "capture_label": label or None,
                    "default_split": default_split,
                },
            },
        )
        response.raise_for_status()
        return response.json()["session_id"]

    def telemetry(self, sequence: int, sensors: dict) -> None:
        self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={"sequence": sequence, "ra8_tick": sequence * 10, "sensors": sensors},
        ).raise_for_status()

    def test_workflow_defaults_distinguish_model_and_reference_data(self) -> None:
        camera_id = self.create_session("camera", "sleeping")
        temperature_id = self.create_session("temperature")
        sessions = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["sessions"]
        by_id = {session["session_id"]: session for session in sessions}

        self.assertEqual(by_id[camera_id]["metadata"]["workflow"], "model_dataset")
        self.assertEqual(by_id[camera_id]["metadata"]["model_family"], "vision")
        self.assertEqual(by_id[camera_id]["metadata"]["storage"], "sd")
        self.assertEqual(by_id[temperature_id]["metadata"]["workflow"], "reference_signal")
        self.assertEqual(by_id[temperature_id]["metadata"]["model_family"], "rule_reference")
        self.assertEqual(by_id[temperature_id]["metadata"]["storage"], "server")

    def test_camera_saved_counter_creates_only_new_deduplicated_placeholders(self) -> None:
        self.telemetry(1, {"camera": {"cmd_seq": 4, "rec": 0, "saved": 5, "soi": 1, "eoi": 1, "werr": 0}})
        session_id = self.create_session("camera", "face_visible")
        self.client.post(f"/api/dashboard/sessions/{session_id}/start?kind=camera").raise_for_status()

        self.telemetry(2, {"camera": {"cmd_seq": 5, "rec": 1, "saved": 5, "soi": 1, "eoi": 1, "werr": 0}})
        self.assertEqual(self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json(), [])

        successful = {"cmd_seq": 5, "rec": 1, "saved": 6, "frames": 1, "bytes": 1200,
                      "soi": 1, "eoi": 1, "werr": 0}
        self.telemetry(3, {"camera": successful})
        self.telemetry(4, {"camera": successful})
        self.telemetry(5, {"camera": {**successful, "saved": 8, "frames": 3}})

        samples = self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json()
        self.assertEqual(len(samples), 3)
        self.assertEqual({sample["source_ref"] for sample in samples}, {
            "sd:camera:5:6", "sd:camera:5:7", "sd:camera:5:8",
        })
        self.assertTrue(all(sample["label"] == "face_visible" for sample in samples))
        self.assertTrue(all(sample["features"]["transfer_status"] == "pending_import" for sample in samples))

    def test_audio_recording_is_one_refreshable_sd_sample_per_command(self) -> None:
        self.telemetry(1, {"microphone": {"cmd_seq": 2, "rec": 0, "rec_bytes": 0, "wbytes": 0}})
        session_id = self.create_session("audio", "crying")
        self.client.post(f"/api/dashboard/sessions/{session_id}/start?kind=audio").raise_for_status()
        self.telemetry(2, {"microphone": {"cmd_seq": 3, "rec": 1, "rec_bytes": 512, "wst": 1, "wbytes": 0, "werr": 0}})
        self.assertEqual(self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json(), [])
        self.telemetry(3, {"microphone": {"cmd_seq": 3, "rec": 1, "rec_bytes": 1024, "wst": 7, "wbytes": 900, "werr": 0}})
        self.telemetry(4, {"microphone": {"cmd_seq": 3, "rec": 1, "rec_bytes": 4096, "wst": 7, "wbytes": 3900, "werr": 0}})

        samples = self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json()
        self.assertEqual(len(samples), 1)
        self.assertEqual(samples[0]["source_ref"], "sd:audio:3")
        self.assertEqual(samples[0]["features"]["rec_bytes"], 4096)
        self.assertEqual(samples[0]["label"], "crying")

    def test_manual_jpeg_import_matches_placeholder_and_exports_training_layout(self) -> None:
        session_id = self.create_session("camera", "sleeping")
        self.client.post(f"/api/dashboard/sessions/{session_id}/start?kind=camera").raise_for_status()
        self.telemetry(1, {"camera": {
            "cmd_seq": 1, "rec": 1, "saved": 1, "frames": 1, "bytes": 12,
            "soi": 1, "eoi": 1, "werr": 0,
        }})
        jpeg = b"\xff\xd8\xff\xe0training-image\xff\xd9"
        imported = self.client.post(
            f"/api/dashboard/sessions/{session_id}/artifacts/import?kind=camera&filename=CAM000001.JPG",
            headers={"Content-Type": "image/jpeg"},
            content=jpeg,
        )
        imported.raise_for_status()
        result = imported.json()
        self.assertTrue(result["matched_placeholder"])
        self.assertFalse(result["duplicate"])

        duplicate = self.client.post(
            f"/api/dashboard/sessions/{session_id}/artifacts/import?kind=camera&filename=CAM000001.JPG",
            headers={"Content-Type": "image/jpeg"},
            content=jpeg,
        ).json()
        self.assertTrue(duplicate["duplicate"])
        self.assertEqual(duplicate["sample_id"], result["sample_id"])

        sample = self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json()[0]
        self.assertEqual(sample["features"]["original_filename"], "CAM000001.JPG")
        self.assertEqual(sample["features"]["transfer_status"], "imported")
        self.client.patch(
            f"/api/dashboard/samples/{sample['id']}",
            json={"split": "train", "quality": "good", "label": "sleeping"},
        ).raise_for_status()

        artifact = self.client.get(f"/api/dashboard/artifacts/{result['artifact_id']}")
        artifact.raise_for_status()
        self.assertEqual(artifact.content, jpeg)
        self.assertIn("inline", artifact.headers["content-disposition"])

        package = self.client.get(f"/api/dashboard/sessions/{session_id}/export")
        package.raise_for_status()
        with zipfile.ZipFile(io.BytesIO(package.content)) as archive:
            names = archive.namelist()
            self.assertIn("DATASET_README.txt", names)
            media_names = [name for name in names if name.startswith("dataset/camera/train/sleeping/")]
            self.assertEqual(len(media_names), 1)
            self.assertEqual(archive.read(media_names[0]), jpeg)
            sample_rows = [json.loads(line) for line in archive.read("samples.jsonl").decode().splitlines()]
            self.assertEqual(sample_rows[0]["split"], "train")

        snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
        session = next(item for item in snapshot["sessions"] if item["session_id"] == session_id)
        self.assertEqual(session["pending_artifact_count"], 0)
        self.assertEqual(session["imported_artifact_count"], 1)
        self.assertEqual(session["train_count"], 1)

    def test_reference_signal_can_be_collected_without_a_training_label(self) -> None:
        session_id = self.create_session("temperature")
        self.client.post(f"/api/dashboard/sessions/{session_id}/start?kind=temperature").raise_for_status()
        self.telemetry(1, {"bed_temperature": {"seq": 9, "t_mc": [23500, 23600]}})
        sample = self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json()[0]
        self.assertIsNone(sample["label"])
        self.assertEqual(sample["kind"], "temperature")
        self.assertEqual(sample["split"], "unassigned")

    def test_model_session_split_is_applied_to_new_sd_placeholders(self) -> None:
        self.telemetry(1, {"camera": {"cmd_seq": 6, "rec": 0, "saved": 2, "soi": 1, "eoi": 1}})
        session_id = self.create_session("camera", "infant", "validation")
        self.client.post(f"/api/dashboard/sessions/{session_id}/start?kind=camera").raise_for_status()
        self.telemetry(2, {"camera": {
            "cmd_seq": 7, "rec": 1, "saved": 3, "frames": 1, "bytes": 1200,
            "soi": 1, "eoi": 1, "werr": 0,
        }})

        samples = self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json()
        self.assertEqual(len(samples), 1)
        self.assertEqual(samples[0]["label"], "infant")
        self.assertEqual(samples[0]["split"], "validation")

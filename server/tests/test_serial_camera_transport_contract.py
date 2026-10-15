from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "Multicore_top_CPU0" / "src" / "thread_code" / "serial_camera_driver.c"


def _define(source: str, name: str) -> int:
    match = re.search(rf"#define\s+{name}\s+\((\d+)U\)", source)
    assert match is not None, f"missing {name}"
    return int(match.group(1))


class SerialCameraTransportContractTests(unittest.TestCase):
    def test_camera_wire_reply_fits_single_sci_transfer(self) -> None:
        source = DRIVER.read_text(encoding="utf-8")
        payload_bytes = _define(source, "SERIAL_CAMERA_WIRE_CHUNK_BYTES")
        overhead_bytes = _define(source, "SERIAL_CAMERA_REPLY_OVERHEAD")

        self.assertLessEqual(payload_bytes + overhead_bytes, 1024)
        self.assertIn("chunk_length > SERIAL_CAMERA_WIRE_CHUNK_BYTES", source)
        self.assertIn("chunk_length = SERIAL_CAMERA_WIRE_CHUNK_BYTES", source)

    def test_camera_uart_uses_generated_timing(self) -> None:
        source = DRIVER.read_text(encoding="utf-8")
        self.assertIn("g_camera_uart.p_ctrl, g_camera_uart.p_cfg", source)
        self.assertNotIn("g_camera_uart_runtime_extend.noise_cancel =", source)

    def test_capture_failure_forces_frame_state_recovery(self) -> None:
        source = DRIVER.read_text(encoding="utf-8")
        start = source.index("static void serial_camera_note_failure")
        end = source.index("static void serial_camera_uart_cleanup", start)
        note_failure = source[start:end]
        self.assertIn(
            "p_cam->capture_consecutive_failures = SERIAL_CAMERA_RECOVERY_FAILURE_THRESHOLD",
            note_failure,
        )

    def test_probe_and_chunk_do_not_retry_after_uart_reopen_only(self) -> None:
        source = DRIVER.read_text(encoding="utf-8")
        self.assertNotIn("SERIAL_CAMERA_PROBE_ATTEMPTS", source)
        self.assertNotIn("SERIAL_CAMERA_CHUNK_ATTEMPTS", source)
        self.assertIn("serial_camera_release_frame_state();", source)
        self.assertIn("serial_camera_recover(reason);", source)

    def test_recovery_restarts_success_watchdog_window(self) -> None:
        source = DRIVER.read_text(encoding="utf-8")
        self.assertIn(
            "g_camera_last_success_tick = g_camera_last_recovery_tick;",
            source,
        )


if __name__ == "__main__":
    unittest.main()

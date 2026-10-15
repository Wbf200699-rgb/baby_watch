from pathlib import Path
from unittest import TestCase


ROOT = Path(__file__).resolve().parents[2]
CPU1_DRIVER = ROOT / "Multicore_top_CPU1/src/thread_code/thermal_uart_driver.c"
CPU0_PROTOCOL = ROOT / "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c"
CPU0_HEADER = ROOT / "Multicore_top_CPU0/src/thread_code/thermal_ipc_shared.h"
CPU1_HEADER = ROOT / "Multicore_top_CPU1/src/thread_code/thermal_ipc_shared.h"


def classify_frame(previous: list[int] | None, current: list[int], pending_mean: int | None = None,
                   *, crc_ok: bool = True) -> tuple[str, int | None]:
    if not crc_ok or not current:
        return "invalid", pending_mean
    mean = sum(current) // len(current)
    if previous is None:
        return "accepted", None
    previous_mean = sum(previous) // len(previous)
    changed = sum(abs(left - right) > 50 for left, right in zip(previous, current, strict=True))
    scene_wide = abs(mean - previous_mean) > 50 and changed * 100 >= len(current) * 20
    if not scene_wide:
        return "accepted", None
    if pending_mean is not None and abs(mean - pending_mean) <= 20:
        return "accepted", None
    return "transient", mean


class Ra8ThermalQualityContractTest(TestCase):
    def test_single_bad_pixel_does_not_reject_a_frame(self) -> None:
        previous = [300] * 100
        current = previous.copy()
        current[0] = -922
        self.assertEqual(classify_frame(previous, current)[0], "accepted")

    def test_one_frame_scene_drop_is_held_and_recovery_is_accepted(self) -> None:
        previous = [300] * 100
        transient = [220] * 100
        result, pending = classify_frame(previous, transient)
        self.assertEqual(result, "transient")
        self.assertEqual(classify_frame(previous, previous, pending)[0], "accepted")

    def test_two_close_frames_confirm_a_real_scene_change(self) -> None:
        previous = [300] * 100
        first = [220] * 100
        second = [225] * 100
        result, pending = classify_frame(previous, first)
        self.assertEqual(result, "transient")
        self.assertEqual(classify_frame(previous, second, pending)[0], "accepted")

    def test_crc_failure_is_never_accepted(self) -> None:
        self.assertEqual(classify_frame([300] * 100, [300] * 100, crc_ok=False)[0], "invalid")

    def test_cpu_headers_and_seqlock_contract_stay_synchronized(self) -> None:
        self.assertEqual(CPU0_HEADER.read_text(), CPU1_HEADER.read_text())
        cpu1 = CPU1_DRIVER.read_text()
        accepted = cpu1[cpu1.index("if (frame_accepted)") : cpu1.index("if (transient_frame)")]
        self.assertLess(accepted.index("thermal_matrix_write_begin"), accepted.index("THERMAL_IPC_POINTS[i]"))
        self.assertLess(accepted.index("THERMAL_IPC_POINTS[i]"), accepted.index("thermal_matrix_write_end"))
        cpu0 = CPU0_PROTOCOL.read_text()
        reader = cpu0[cpu0.index("ra8_esp32_protocol_send_thermal_matrix") :]
        self.assertIn("generation_before & 1U", reader)
        self.assertIn("generation_before == generation_after", reader)
        self.assertIn("sequence_before == p_thm->last_good_sequence", reader)

    def test_continuous_dtc_arms_before_enabling_stream(self) -> None:
        cpu1 = CPU1_DRIVER.read_text()
        start = cpu1.rindex("static bool thermal_uart_request_frame_dtc(TickType_t timeout_ticks)")
        end = cpu1.index("bool thermal_uart_request_frame(TickType_t timeout_ticks)", start)
        request = cpu1[start:end]
        active = request.index("if (g_thermal_uart_context.rx_active)")
        stop = request.index("thermal_uart_stop_dtc_read()", active)
        read = request.index("g_thermal_uart.p_api->read")
        write = request.index("thermal_uart_write_command", read)
        self.assertLess(active, stop)
        self.assertNotIn("communicationAbort(g_thermal_uart.p_ctrl, UART_DIR_RX)", request)
        self.assertLess(read, write)

    def test_startup_keeps_generated_sci_pin_configuration(self) -> None:
        cpu1 = CPU1_DRIVER.read_text()
        start = cpu1.index("fsp_err_t thermal_uart_driver_init(void)")
        end = cpu1.index("static uint32_t thermal_uart_stop_dtc_read", start)
        init = cpu1[start:end]
        self.assertIn("thermal_uart_configure_power_relay()", init)
        self.assertNotIn("thermal_uart_power_cycle_module()", init)

    def test_recovery_soft_resynchronizes_before_reopening_uart(self) -> None:
        cpu1 = CPU1_DRIVER.read_text()
        recovery = cpu1[cpu1.index("static bool thermal_uart_recover(bool power_cycle,") :]
        soft = recovery.index("if (g_thermal_uart_context.initialized)")
        request_mode = recovery.index("thermal_uart_enter_request_mode()", soft)
        close = recovery.index("thermal_uart_close_peripheral()", request_mode)
        self.assertLess(soft, request_mode)
        self.assertLess(request_mode, close)

    def test_dtc_uses_documented_continuous_output_mode(self) -> None:
        cpu1 = CPU1_DRIVER.read_text()
        start = cpu1.rindex("static bool thermal_uart_request_frame_dtc(TickType_t timeout_ticks)")
        end = cpu1.index("bool thermal_uart_request_frame(TickType_t timeout_ticks)", start)
        request = cpu1[start:end]
        self.assertIn("THERMAL_AUTO_FULL_COMMAND", request)
        self.assertIn("THERMAL_STREAM_CAPTURE_BYTES", request)
        self.assertIn("thermal_stream_publish_latest", request)
        self.assertLess(request.index("g_thermal_uart.p_api->read"),
                        request.index("thermal_uart_write_command(auto_full_output_command"))

    def test_stream_resynchronizes_with_two_frames_and_crc(self) -> None:
        cpu1 = CPU1_DRIVER.read_text()
        self.assertIn("THERMAL_STREAM_CAPTURE_FRAMES (2U)", cpu1)
        start = cpu1.index("static bool thermal_stream_publish_latest")
        end = cpu1.index("static bool thermal_uart_request_frame_dtc", start)
        parser = cpu1[start:end]
        self.assertIn("offset + THERMAL_UART_FRAME_LENGTH", parser)
        self.assertIn("thermal_crc16_modbus", parser)
        self.assertIn("memcpy(g_thermal_frame", parser)
        self.assertIn("SCI_B_UART_NOISE_CANCELLATION_ENABLE", cpu1)

    def test_stream_is_paused_between_dtc_blocks(self) -> None:
        cpu1 = CPU1_DRIVER.read_text()
        start = cpu1.index("static void thermal_uart_pause_stream")
        end = cpu1.index("static bool thermal_uart_request_frame_dtc", start)
        pause = cpu1[start:end]
        self.assertIn("THERMAL_OUTPUT_OFF_COMMAND", pause)
        self.assertIn("communicationAbort", pause)
        self.assertIn("stream_enabled = false", pause)

        request_start = cpu1.rindex("static bool thermal_uart_request_frame_dtc")
        request_end = cpu1.index("\nbool thermal_uart_request_frame", request_start)
        request = cpu1[request_start:request_end]
        self.assertGreaterEqual(request.count("thermal_uart_pause_stream();"), 3)

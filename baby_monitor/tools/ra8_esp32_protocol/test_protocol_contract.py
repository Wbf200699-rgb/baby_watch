from __future__ import annotations

import sys
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))

from ra8_protocol import FrameParser, TYPE_CMD, encode_frame  # noqa: E402


def _without_if_zero(source: str) -> str:
    output: list[str] = []
    disabled_depth = 0
    for line in source.splitlines():
        stripped = line.strip()
        if disabled_depth:
            if stripped.startswith("#if"):
                disabled_depth += 1
            elif stripped.startswith("#endif"):
                disabled_depth -= 1
            continue
        if stripped in {"#if 0", "#if (0)"}:
            disabled_depth = 1
            continue
        output.append(line)
    return "\n".join(output)


class ProtocolContractTests(unittest.TestCase):
    def test_m1616_low_pressure_fusion_keeps_debounce_and_release_hold(self) -> None:
        source = (
            REPO / "Multicore_top_CPU0/src/fusion_thread_entry.c"
        ).read_text(encoding="utf-8")
        self.assertIn("#define FUSION_M1616_OCCUPIED_TOTAL         (1U)", source)
        self.assertIn("#define FUSION_M1616_ACTIVE_POINT           FUSION_CAL_PRESSURE_ACTIVE_POINT", source)
        self.assertIn("#define FUSION_M1616_CONFIRM_FRAMES         FUSION_CAL_PRESSURE_CONFIRM_FRAMES", source)
        self.assertIn("#define FUSION_M1616_RELEASE_FRAMES         FUSION_CAL_PRESSURE_RELEASE_FRAMES", source)

        calibration = (
            REPO / "Multicore_top_CPU0/src/thread_code/fusion_calibration_generated.h"
        ).read_text(encoding="utf-8")
        self.assertIn("FUSION_CAL_PRESSURE_ACTIVE_POINT", calibration)
        self.assertIn("FUSION_CAL_PRESSURE_CONFIRM_FRAMES", calibration)
        self.assertIn("FUSION_CAL_PRESSURE_RELEASE_FRAMES", calibration)

    def test_gateway_ready_reference_frame(self) -> None:
        expected = bytes.fromhex(
            "AA 55 01 20 00 01 00 0D "
            "67 61 74 65 77 61 79 5F 72 65 61 64 79 3F 10"
        )
        encoded = encode_frame(TYPE_CMD, b"gateway_ready", sequence=1)
        self.assertEqual(encoded, expected)
        frames = list(FrameParser().feed(encoded))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].payload, b"gateway_ready")

    def test_silent_reset_reference_frame_and_firmware_contract(self) -> None:
        payload = b'{"cmd":"silent_reset"}'
        expected = bytes.fromhex(
            "AA 55 01 20 12 34 00 16 "
            "7B 22 63 6D 64 22 3A 22 73 69 6C 65 6E 74 5F 72 "
            "65 73 65 74 22 7D D0 CF"
        )
        encoded = encode_frame(TYPE_CMD, payload, sequence=0x1234)
        self.assertEqual(encoded, expected)
        frame = list(FrameParser().feed(encoded))[0]
        self.assertEqual(frame.sequence, 0x1234)
        self.assertEqual(frame.payload, payload)

        source = (
            REPO / "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c"
        ).read_text(encoding="utf-8")
        self.assertIn('"{\\"cmd\\":\\"silent_reset\\"}"', source)
        self.assertIn("RA8_ESP32_MSG_CMD_RESULT", source)
        self.assertIn('\\"request_sequence\\":', source)
        self.assertIn("RA8_SILENT_RESET_DELAY_MS", source)
        self.assertIn("NVIC_SystemReset();", source)
        self.assertIn("(uint16_t) (((uint16_t) p_frame[4] << 8) | p_frame[5])", source)

    def test_silent_boot_is_owned_by_cpu1_and_debug_visible(self) -> None:
        cpu0_shared_path = (
            REPO / "Multicore_top_CPU0/src/thread_code/silent_reset_shared.h"
        )
        cpu1_shared = (
            REPO / "Multicore_top_CPU1/src/thread_code/silent_reset_shared.h"
        ).read_text(encoding="utf-8")
        cpu0_audio = (
            REPO / "Multicore_top_CPU0/src/thread_code/audio_ipc_shared.h"
        ).read_bytes()
        cpu1_audio = (
            REPO / "Multicore_top_CPU1/src/thread_code/audio_ipc_shared.h"
        ).read_bytes()
        self.assertFalse(cpu0_shared_path.exists())
        self.assertEqual(cpu0_audio, cpu1_audio)
        self.assertNotIn("SILENT_RESET_SNAPSHOT_ADDRESS", cpu1_shared)
        self.assertIn("silent_reset_boot_state_get", cpu1_shared)
        self.assertIn(b"boot_flags", cpu1_audio)
        self.assertIn(b"silent_boot_count", cpu1_audio)

        cpu0_warmstart = (
            REPO / "Multicore_top_CPU0/src/hal_warmstart.c"
        ).read_text(encoding="utf-8")
        cpu1_warmstart = (
            REPO / "Multicore_top_CPU1/src/hal_warmstart.c"
        ).read_text(encoding="utf-8")
        self.assertNotIn("RSTSR2_b.CWSF", cpu0_warmstart)
        self.assertIn("RSTSR2_b.CWSF", cpu1_warmstart)
        self.assertNotIn("SILENT_RESET_SNAPSHOT", cpu1_warmstart)
        self.assertLess(
            cpu1_warmstart.index("if (BSP_WARM_START_POST_C == event)"),
            cpu1_warmstart.index("        silent_reset_capture_post_c();"),
        )
        self.assertIn("ordinary CPU1 .bss", cpu1_warmstart)

        player = (
            REPO / "Multicore_top_CPU1/src/thread_code/audio_wav_player.c"
        ).read_text(encoding="utf-8")
        self.assertIn("audio_wav_boot_state_init();", player)
        self.assertIn("AUDIO_IPC_BOOT_FLAG_AUDIO_SUPPRESSED", player)
        self.assertIn("g_audio_silent_boot_count", player)
        self.assertIn("audio_wav_play_file(AUDIO_WAV_PLAYER_DEFAULT_PATH)", player)

        jlink_reader = (
            REPO / "tools/read_silent_reset_jlink.py"
        ).read_text(encoding="utf-8")
        self.assertIn("AUDIO_BOOT_ADDRESS = 0x221D3184", jlink_reader)
        self.assertIn("AUDIO_BOOT_WORDS = 7", jlink_reader)
        self.assertIn(
            'f"mem32 0x{AUDIO_BOOT_ADDRESS:08X},{AUDIO_BOOT_WORDS}"',
            jlink_reader,
        )
        self.assertIn('audio["boot_capture_count"] != 1', jlink_reader)
        self.assertNotIn("0x221D3F50", jlink_reader)
        self.assertNotIn("RESET_MAGIC", jlink_reader)
        self.assertNotIn("RESET_ADDRESS", jlink_reader)

    def test_ra8_status_is_not_gated_by_gateway_ready(self) -> None:
        source = (REPO / "Multicore_top_CPU0/src/esp32_uart_thread_entry.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("ra8_esp32_protocol_poll_rx();", source)
        self.assertIn("ra8_esp32_protocol_send_status", source)
        self.assertNotIn("if (!ra8_esp32_protocol_gateway_ready())", source)

    def test_warm_light_uses_accessible_safe_default_gpio_and_exact_commands(self) -> None:
        uart_thread = (
            REPO / "Multicore_top_CPU0/src/esp32_uart_thread_entry.c"
        ).read_text(encoding="utf-8")
        protocol = (
            REPO / "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c"
        ).read_text(encoding="utf-8")
        header = (
            REPO / "Multicore_top_CPU0/src/thread_code/warm_light_control.h"
        ).read_text(encoding="utf-8")

        self.assertIn("BSP_IO_PORT_07_PIN_11", uart_thread)
        self.assertIn("WARM_LIGHT_RELAY_INACTIVE_CFG", uart_thread)
        self.assertIn("warm_light_control_init();", uart_thread)
        self.assertIn("WARM_LIGHT_CONTROL_PIN_LABEL     (711U)", header)
        self.assertIn("WARM_LIGHT_CONTROL_ACTIVE_HIGH   (1U)", header)
        self.assertIn('protocol_payload_equals(p_payload, length, "light_on")', protocol)
        self.assertIn('protocol_payload_equals(p_payload, length, "light_off")', protocol)
        self.assertIn('\\"light\\":{\\"s\\":', protocol)

    def test_soother_relay_is_an_idempotent_maintained_switch_on_p914(self) -> None:
        uart_thread = (
            REPO / "Multicore_top_CPU0/src/esp32_uart_thread_entry.c"
        ).read_text(encoding="utf-8")
        protocol = (
            REPO / "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c"
        ).read_text(encoding="utf-8")
        header = (
            REPO / "Multicore_top_CPU0/src/thread_code/soother_pat_control.h"
        ).read_text(encoding="utf-8")

        self.assertIn("BSP_IO_PORT_09_PIN_14", uart_thread)
        self.assertIn("SOOTHER_RELAY_INACTIVE_CFG", uart_thread)
        self.assertNotIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)", uart_thread)
        self.assertIn("SOOTHER_CONTROL_PIN_LABEL       (914U)", header)
        self.assertIn('protocol_payload_equals(p_payload, length, "soother_on")', protocol)
        self.assertIn('protocol_payload_equals(p_payload, length, "soother_off")', protocol)
        self.assertIn("soother_pat_control_set(true)", protocol)
        self.assertIn("soother_pat_control_set(false)", protocol)
        self.assertIn('\\"pat\\":{\\"s\\":', protocol)

    def test_fusion_v7_recommends_comfort_actions_without_driving_relays(self) -> None:
        fusion = (REPO / "Multicore_top_CPU0/src/fusion_thread_entry.c").read_text(
            encoding="utf-8"
        )
        header = (
            REPO / "Multicore_top_CPU0/src/thread_code/multimodal_fusion.h"
        ).read_text(encoding="utf-8")
        protocol = (
            REPO / "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c"
        ).read_text(encoding="utf-8")

        self.assertIn("MULTIMODAL_FUSION_SCHEMA_VERSION       (9U)", header)
        self.assertIn("fusion_build_comfort_recommendation", fusion)
        self.assertIn("comfort_requires_confirmation", header)
        self.assertNotIn("warm_light_control_set", fusion)
        self.assertNotIn("soother_pat_control_press", fusion)
        self.assertIn("fusion.comfort_action_mask", protocol)
        self.assertIn("fusion.comfort_block_mask", protocol)

    def test_story_transfer_protects_uart_and_partial_file(self) -> None:
        uart_driver = (
            REPO / "Multicore_top_CPU0/src/thread_code/esp32_uart_driver.c"
        ).read_text(encoding="utf-8")
        uart_thread = (
            REPO / "Multicore_top_CPU0/src/esp32_uart_thread_entry.c"
        ).read_text(encoding="utf-8")
        protocol = (
            REPO / "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c"
        ).read_text(encoding="utf-8")
        player = (
            REPO / "Multicore_top_CPU1/src/thread_code/audio_wav_player.c"
        ).read_text(encoding="utf-8")

        self.assertIn("R_SCI_B_UART_BaudCalculate(ESP32_UART_BAUD_RATE,", uart_driver)
        self.assertIn("ESP32_UART_BAUD_RATE          (921600U)", uart_driver)
        self.assertIn("                                                true,", uart_driver)
        self.assertIn("esp32_uart_rx_task_start();", uart_thread)
        self.assertNotIn("uart_stats.error_count != last_uart_error_count", uart_thread)
        self.assertIn("ra8_esp32_protocol_story_active()", uart_thread)
        self.assertIn("RA8_STORY_ACTIVITY_TIMEOUT_MS", protocol)
        self.assertIn("AUDIO_STORY_RECEIVE_TIMEOUT_MS", player)
        self.assertIn("audio_story_close(true);", player)

    def test_story_autoplay_wakes_active_and_paused_player(self) -> None:
        player = (
            REPO / "Multicore_top_CPU1/src/thread_code/audio_wav_player.c"
        ).read_text(encoding="utf-8")

        self.assertIn("static bool audio_wav_story_autoplay_pending(void)", player)
        self.assertGreaterEqual(player.count("if (audio_wav_story_autoplay_pending())"), 3)
        self.assertIn("AUDIO_IPC_MAILBOX->status = (uint32_t) AUDIO_IPC_STATUS_IDLE;", player)
        self.assertIn(
            "AUDIO_IPC_MAILBOX->story_ready_sequence != ready_sequence", player
        )
        self.assertIn("Preserve its READY state", player)

    def test_story_transfer_keeps_status_heartbeat_without_bulk_uploads(self) -> None:
        uart_thread = (
            REPO / "Multicore_top_CPU0/src/esp32_uart_thread_entry.c"
        ).read_text(encoding="utf-8")

        self.assertIn("ESP32_UART_STORY_STATUS_INTERVAL_MS (1000U)", uart_thread)
        self.assertIn("ESP32_UART_STORY_STATUS_TIMEOUT_MS  (100U)", uart_thread)
        story_branch = uart_thread[
            uart_thread.index("if (story_active)") :
            uart_thread.index("if (gateway_ready &&", uart_thread.index("if (story_active)"))
        ]
        self.assertIn("!ra8_esp32_protocol_story_result_pending()", story_branch)
        self.assertIn("ra8_esp32_protocol_send_status", story_branch)
        self.assertIn("continue;", story_branch)
        self.assertNotIn("ra8_esp32_protocol_send_pressure_matrix", story_branch)
        self.assertNotIn("ra8_esp32_protocol_send_thermal_matrix", story_branch)

    def test_cpu1_prioritizes_story_chunks_over_debug_sd_work(self) -> None:
        fs_thread = (
            REPO / "Multicore_top_CPU1/src/file_system_thread_entry.c"
        ).read_text(encoding="utf-8")
        sd_thread = (
            REPO / "Multicore_top_CPU1/src/sd_card_thread_entry.c"
        ).read_text(encoding="utf-8")
        player = (
            REPO / "Multicore_top_CPU1/src/thread_code/audio_wav_player.c"
        ).read_text(encoding="utf-8")

        self.assertLess(
            fs_thread.index("audio_wav_story_writer_poll(sd_ready)"),
            fs_thread.index("sd_card_driver_debug_export_poll(sd_ready)"),
        )
        self.assertIn("FILE_SYSTEM_STORY_PRIORITY  (configMAX_PRIORITIES - 4U)", fs_thread)
        self.assertIn("vTaskPrioritySet", fs_thread)
        self.assertIn("if (!story_active)", fs_thread)
        self.assertIn("bool audio_wav_story_writer_active(void)", player)
        self.assertIn("AUDIO_STORY_WRITE_BUFFER_BYTES (65536U)", player)
        self.assertIn("audio_story_flush_write_buffer", player)
        self.assertIn("g_audio_story_write_buffer_length += length", player)
        self.assertIn("if (audio_wav_story_writer_active())", sd_thread)
        story_guard = sd_thread[
            sd_thread.index("if (audio_wav_story_writer_active())") :
            sd_thread.index("if (!sd_card_driver_is_ready())")
        ]
        self.assertIn("sd_card_driver_fs_unlock();", story_guard)
        self.assertIn("continue;", story_guard)

    def test_story_bulk_uart_uses_free_sci4_header_pins_and_interrupt_resync(self) -> None:
        bulk = (
            REPO / "Multicore_top_CPU1/src/thread_code/story_bulk_uart.c"
        ).read_text(encoding="utf-8")
        bulk_header = (
            REPO / "Multicore_top_CPU1/src/thread_code/story_bulk_uart.h"
        ).read_text(encoding="utf-8")
        pins = (REPO / "Multicore_top_CPU1/ra_gen/pin_data.c").read_text(
            encoding="utf-8"
        )
        vectors = (REPO / "Multicore_top_CPU1/ra_gen/vector_data.h").read_text(
            encoding="utf-8"
        )
        fs_thread = (
            REPO / "Multicore_top_CPU1/src/file_system_thread_entry.c"
        ).read_text(encoding="utf-8")

        self.assertIn("STORY_BULK_BAUD_RATE        (1000000U)", bulk_header)
        self.assertIn("STORY_BULK_PAYLOAD_MAX      (4096U)", bulk_header)
        self.assertIn(".channel = 4U", bulk)
        self.assertIn(".p_transfer_rx = NULL", bulk)
        self.assertIn("UART_EVENT_RX_CHAR", bulk)
        self.assertIn("STORY_BULK_RX_RING_BYTES", bulk)
        self.assertIn("STORY_BULK_TYPE_READY", bulk)
        self.assertIn("resync_count", bulk_header)
        self.assertNotIn("g_story_bulk_transfer", bulk)
        self.assertIn("BSP_IO_PORT_07_PIN_14", pins)
        self.assertIn("BSP_IO_PORT_07_PIN_15", pins)
        self.assertIn("VECTOR_NUMBER_SCI4_RXI", vectors)
        self.assertIn("story_bulk_uart_init();", fs_thread)
        self.assertIn("story_bulk_uart_poll(sd_ready);", fs_thread)

    def test_status_has_one_active_vision_object_and_payload_fallbacks(self) -> None:
        source = (REPO / "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c").read_text(
            encoding="utf-8"
        )
        active = _without_if_zero(source)
        self.assertEqual(active.count('\\"vision\\":'), 1)
        self.assertIn("STATUS_DETAIL_FULL", active)
        self.assertIn("STATUS_DETAIL_COMPACT", active)
        self.assertIn("STATUS_DETAIL_ESSENTIAL", active)
        self.assertIn("STATUS_DETAIL_EMERGENCY", active)
        self.assertIn("protocol_build_emergency_status_payload", active)
        self.assertIn("if (STATUS_DETAIL_ESSENTIAL != detail)", active)
        for field in (
            '"sdr"',
            '"sds"',
            '"sdfs"',
            '"sdfl"',
            '"sdtry"',
            '"sdfail"',
            '"sdrec"',
            '"sdfsp"',
            '"sdff"',
        ):
            self.assertIn(field.replace('"', '\\"'), active)
        self.assertIn('\\"status_truncated\\":1', active)
        for field in (
            '"st_try"',
            '"st_ok"',
            '"st_len"',
            '"st_long"',
            '"st_uart"',
            '"heap_min"',
            '"stk"',
        ):
            self.assertIn(field.replace('"', '\\"'), active)

    def test_stack_watermark_is_enabled_in_configuration_source(self) -> None:
        configuration = (REPO / "Multicore_top_CPU0/configuration.xml").read_text(encoding="utf-8")
        self.assertIn(
            'include_uxtaskgetstackhighwatermark.enabled',
            configuration,
        )

    def test_scd41_status_contract_and_shared_mailbox_match(self) -> None:
        source = (REPO / "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c").read_text(
            encoding="utf-8"
        )
        active = _without_if_zero(source)
        start = active.index(',\\"scd\\":')
        end = active.index(',\\"prs\\":', start)
        scd = active[start:end]

        for field in (
            "seq",
            "s",
            "co2",
            "t_mc",
            "rh_mp",
            "ready",
            "crc_err",
            "i2c_err",
            "to",
            "rec",
            "fsp",
            "last",
        ):
            self.assertIn(f'\\"{field}\\"', scd)
        self.assertIn("fusion.environment_last_good_tick", scd)
        self.assertNotIn("p_scd->last_good_tick", scd)
        self.assertNotIn("p_scd->last_event", scd)
        self.assertIn("scd41_ipc_read_snapshot(&scd41_latest)", active)

        cpu0_header = (
            REPO / "Multicore_top_CPU0/src/thread_code/scd41_ipc_shared.h"
        ).read_bytes()
        cpu1_header = (
            REPO / "Multicore_top_CPU1/src/thread_code/scd41_ipc_shared.h"
        ).read_bytes()
        self.assertEqual(cpu0_header, cpu1_header)
        self.assertIn(b"#define SCD41_IPC_ADDRESS      (0x221D0180UL)", cpu0_header)
        self.assertIn(b"volatile uint32_t publish_lock;", cpu0_header)
        self.assertIn(b"scd41_ipc_read_snapshot", cpu0_header)

        producer = (
            REPO / "Multicore_top_CPU1/src/thread_code/scd41_driver.c"
        ).read_text(encoding="utf-8")
        fusion_source = (
            REPO / "Multicore_top_CPU0/src/fusion_thread_entry.c"
        ).read_text(encoding="utf-8")
        logger_source = (
            REPO / "Multicore_top_CPU1/src/thread_code/system_logger.c"
        ).read_text(encoding="utf-8")
        self.assertIn("scd41_publish_begin", producer)
        self.assertIn("p_mailbox->publish_lock = committed_lock;", producer)
        reset_start = producer.index("static void scd41_reset_mailbox(void)")
        reset_end = producer.index("\n}", reset_start)
        reset_body = producer[reset_start:reset_end]
        self.assertIn(
            "scd41_publish_image(SCD41_IPC_STATUS_STARTING, false, true);",
            reset_body,
        )
        self.assertNotIn("scd41_publish_begin", reset_body)
        self.assertIn("if (reset_bme)", producer)
        self.assertIn("p_mailbox->bme_error_count = 0U;", producer)
        self.assertIn("scd41_ipc_read_snapshot(&environment)", fusion_source)
        self.assertIn("scd41_ipc_read_snapshot(&scd41_latest)", logger_source)

    def test_bme688_status_contract_uses_the_shared_environment_bus(self) -> None:
        source = (REPO / "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c").read_text(
            encoding="utf-8"
        )
        adapter_protocol = (
            REPO / "tools/ra8_esp32_protocol/ESP32_ADAPTER_PROTOCOL.md"
        ).read_text(encoding="utf-8")
        shape_start = adapter_protocol.index("Top-level shape:")
        shape_end = adapter_protocol.index("The sender first builds", shape_start)
        self.assertIn('"bme": {},', adapter_protocol[shape_start:shape_end])

        active = _without_if_zero(source)
        start = active.index(',\\"bme\\":')
        end = active.index(',\\"prs\\":', start)
        bme = active[start:end]

        for field in (
            "s",
            "addr",
            "t_mc",
            "rh_mp",
            "p_pa",
            "gas_ohm",
            "flags",
            "last",
            "err",
        ):
            self.assertIn(f'\\"{field}\\"', bme)
        self.assertIn("g_protocol_bme_last_good_tick", bme)
        self.assertNotIn("p_scd->bme_last_good_tick", bme)

        cpu0_header = (
            REPO / "Multicore_top_CPU0/src/thread_code/scd41_ipc_shared.h"
        ).read_bytes()
        cpu1_header = (
            REPO / "Multicore_top_CPU1/src/thread_code/scd41_ipc_shared.h"
        ).read_bytes()
        self.assertEqual(cpu0_header, cpu1_header)
        for field in (
            b"bme_status",
            b"bme_i2c_address",
            b"bme_temperature_milli_c",
            b"bme_humidity_milli_percent",
            b"bme_pressure_pa",
            b"bme_gas_resistance_ohm",
            b"bme_flags",
            b"bme_last_good_tick",
            b"bme_error_count",
        ):
            self.assertIn(field, cpu0_header)

        driver = (
            REPO / "Multicore_top_CPU1/src/thread_code/bme688_driver.c"
        ).read_text(encoding="utf-8")
        bus_owner = (
            REPO / "Multicore_top_CPU1/src/thread_code/scd41_driver.c"
        ).read_text(encoding="utf-8")
        thread = (
            REPO / "Multicore_top_CPU1/src/scd41_thread_entry.c"
        ).read_text(encoding="utf-8")
        logger = (
            REPO / "Multicore_top_CPU1/src/thread_code/system_logger.c"
        ).read_text(encoding="utf-8")

        self.assertIn("BME688_I2C_ADDRESS_LOW", driver)
        self.assertIn("BME688_I2C_ADDRESS_HIGH", driver)
        self.assertIn("bme68x_get_data", driver)
        self.assertIn("scd41_i2c_shared_write_read", driver)
        self.assertIn("scd41_i2c_shared_recover", driver)
        self.assertIn("fsp_err_t scd41_i2c_shared_recover(void)", bus_owner)
        self.assertIn("return !g_bme688_runtime.bus_recovery_attempted;", driver)
        self.assertIn("scd41_i2c_select(SCD41_I2C_ADDRESS)", bus_owner)
        self.assertLess(thread.index("scd41_driver_poll()"), thread.index("bme688_driver_poll()"))
        self.assertIn("bme_pressure_pa", logger)
        self.assertIn("bme_gas_ohm", logger)


if __name__ == "__main__":
    unittest.main()

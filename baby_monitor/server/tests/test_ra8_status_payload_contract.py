from pathlib import Path
from unittest import TestCase


REPO_ROOT = Path(__file__).resolve().parents[2]
PROTOCOL_SOURCE_CANDIDATES = (
    REPO_ROOT / "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c",
    REPO_ROOT / "ra8/Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c",
)
PROTOCOL_SOURCE = next(
    (candidate for candidate in PROTOCOL_SOURCE_CANDIDATES if candidate.is_file()),
    PROTOCOL_SOURCE_CANDIDATES[0],
)


class Ra8StatusPayloadContractTest(TestCase):
    def test_status_fallback_reserves_wire_headroom(self) -> None:
        source = PROTOCOL_SOURCE.read_text(encoding="utf-8")

        self.assertIn("PROTOCOL_STATUS_PAYLOAD_HEADROOM_BYTES (256U)", source)
        self.assertGreaterEqual(
            source.count("payload_length > PROTOCOL_STATUS_PAYLOAD_SOFT_LIMIT"), 2
        )
        self.assertIn(
            "payload_length > sizeof(g_protocol_status_payload)", source
        )

    def test_essential_status_keeps_live_thermal_state_not_verbose_counters(self) -> None:
        source = PROTOCOL_SOURCE.read_text(encoding="utf-8")
        essential_gate = source.index(
            "These cumulative transport and recovery counters are valuable"
        )
        thermal_close = source.index(
            "protocol_append_char(p_payload, offset, payload_size, '}');",
            essential_gate,
        )
        gated_block = source[essential_gate:thermal_close]
        live_prefix = source[source.index(r'\"p01_cd\"'):essential_gate]

        for field in (
            r'\"p01_cd\"',
            r'\"p99_cd\"',
            r'\"mean_cd\"',
            r'\"frame_quality\"',
            r'\"valid\"',
            r'\"crc_rx\"',
            r'\"crc_calc\"',
        ):
            self.assertIn(field, live_prefix)
        for field in (
            r'\"transient_drop_count\"',
            r'\"generation_retry_count\"',
            r'\"write_generation\"',
            r'\"mean_abs_diff_cd\"',
        ):
            self.assertIn(field, gated_block)

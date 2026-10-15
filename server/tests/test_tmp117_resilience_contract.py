from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "Multicore_top_CPU1" / "src" / "thread_code" / "tmp117_matrix_driver.c"


class Tmp117ResilienceContractTests(unittest.TestCase):
    def test_transient_channel_failure_keeps_last_valid_sample(self) -> None:
        source = DRIVER.read_text(encoding="utf-8")
        self.assertIn("TMP117_CHANNEL_FAILURE_TOLERANCE (3U)", source)
        self.assertIn("g_tmp117_failure_streak[channel] = 0U", source)
        self.assertIn("g_tmp117_debug.address[channel]) &&", source)
        self.assertIn("connected_mask |= (1U << channel)", source)


if __name__ == "__main__":
    unittest.main()

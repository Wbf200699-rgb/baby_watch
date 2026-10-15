from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "Multicore_top_CPU0" / "src" / "thread_code" / "esp32_uart_driver.c"


class Ra8GatewayUartBaudContractTests(unittest.TestCase):
    def test_gateway_uart_reuses_fsp_generated_baud_settings(self) -> None:
        source = DRIVER.read_text(encoding="utf-8")

        self.assertIn("g_esp32_uart_cfg_extend.p_baud_setting", source)
        self.assertNotIn("R_SCI_B_UART_BaudCalculate", source)


if __name__ == "__main__":
    unittest.main()

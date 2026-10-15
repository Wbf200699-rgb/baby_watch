import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class Ra8ResourceMetricsContractTest(unittest.TestCase):
    def read(self, relative_path: str) -> str:
        return (ROOT / relative_path).read_text(encoding="utf-8")

    def test_freertos_runtime_stats_survive_fsp_regeneration(self):
        for core in ("CPU0", "CPU1"):
            config = self.read(f"Multicore_top_{core}/configuration.xml")
            generated = self.read(f"Multicore_top_{core}/ra_cfg/aws/FreeRTOSConfig.h")
            custom = self.read(
                f"Multicore_top_{core}/src/freertos_runtime_stats_config.h"
            )
            self.assertIn("configgenerate_run_time_stats.enabled", config)
            self.assertIn("include_xtaskgetidletaskhandle.enabled", config)
            self.assertIn(
                'id="config.awsfreertos.custom_freertosconfig" '
                'value="freertos_runtime_stats_config.h"',
                config,
            )
            self.assertIn("#define configGENERATE_RUN_TIME_STATS (1)", generated)
            self.assertIn("#define INCLUDE_xTaskGetIdleTaskHandle (1)", generated)
            self.assertIn('#include "freertos_runtime_stats_config.h"', generated)
            self.assertIn("portCONFIGURE_TIMER_FOR_RUN_TIME_STATS", custom)
            self.assertIn("portGET_RUN_TIME_COUNTER_VALUE", custom)

    def test_resource_frame_is_independent_and_rate_limited(self):
        protocol_h = self.read(
            "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.h"
        )
        protocol_c = self.read(
            "Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.c"
        )
        uart_thread = self.read(
            "Multicore_top_CPU0/src/esp32_uart_thread_entry.c"
        )
        self.assertIn("RA8_ESP32_MSG_RESOURCE_UPLOAD = 0x14", protocol_h)
        self.assertIn("RA8_RESOURCE_PAYLOAD_BYTES             (56U)", protocol_c)
        self.assertIn("CoreDebug_DEMCR_TRCENA_Msk", protocol_c)
        self.assertIn("DWT_CTRL_CYCCNTENA_Msk", protocol_c)
        self.assertIn("return DWT->CYCCNT;", protocol_c)
        self.assertIn("ESP32_UART_RESOURCE_INTERVAL_MS   (1000U)", uart_thread)

    def test_cpu1_resource_snapshot_uses_seqlock(self):
        for core in ("CPU0", "CPU1"):
            shared = self.read(
                f"Multicore_top_{core}/src/thread_code/sd_ipc_shared.h"
            )
            self.assertIn("#define SD_IPC_VERSION     (3U)", shared)
            self.assertIn("resource_generation", shared)
            self.assertIn("cpu_usage_permille", shared)
            self.assertIn("heap_min_bytes", shared)

    def test_p4_and_dashboard_expose_resource_metrics(self):
        gateway = self.read("p4x_eye/gateway/main/gateway_main.c")
        page = self.read("server/static/index.html")
        javascript = self.read("server/static/app.js")
        self.assertIn("RA8_TYPE_RESOURCE_UPLOAD 0x14", gateway)
        self.assertIn('"ra8_resources"', gateway)
        self.assertIn('id="ra8-utilization-chart"', page)
        self.assertIn('id="ra8-heap-chart"', page)
        self.assertIn('id="resource-recording-start"', page)
        self.assertIn("/api/dashboard/resource-recordings/${action}", javascript)
        self.assertIn("cpu0_usage_percent", self.read("server/app/main.py"))
        self.assertIn("health.ra8_resources.cpu0_usage_permille", javascript)


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("demo_fallback.py")
SPEC = importlib.util.spec_from_file_location("demo_fallback", MODULE_PATH)
assert SPEC and SPEC.loader
demo = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(demo)


class DemoFallbackTest(unittest.TestCase):
    def base_config(self) -> dict[str, object]:
        return {
            "wifi_ssid": "ssid",
            "wifi_password": "password",
            "device_id": "device-1",
            "device_key": "key",
            "local_server_ip": "192.168.50.10",
            "local_server_port": 8088,
        }

    def test_local_gateway_origins_use_configured_lan_server(self) -> None:
        env, description = demo.gateway_environment(self.base_config(), "local")
        self.assertEqual("http://192.168.50.10:8088", description)
        self.assertEqual(description, env["GATEWAY_HTTP_ORIGIN"])
        self.assertEqual("ws://192.168.50.10:8088", env["GATEWAY_WS_ORIGIN"])

    def test_cloud_gateway_origins_are_fixed_to_public_tls(self) -> None:
        env, description = demo.gateway_environment(self.base_config(), "cloud")
        self.assertEqual(demo.CLOUD_HTTP_ORIGIN, description)
        self.assertEqual(demo.CLOUD_HTTP_ORIGIN, env["GATEWAY_HTTP_ORIGIN"])
        self.assertEqual(demo.CLOUD_WS_ORIGIN, env["GATEWAY_WS_ORIGIN"])

    def test_check_only_never_invokes_idf(self) -> None:
        args = type("Args", (), {"target": "local", "port": "", "check_only": True})()
        with mock.patch.object(demo, "load_config", return_value=self.base_config()), \
             mock.patch.object(demo, "find_idf_path", return_value=Path("C:/idf")), \
             mock.patch.object(demo, "idf_command") as command:
            demo.flash_gateway(args)
        command.assert_not_called()

    @mock.patch.object(demo.shutil, "which", side_effect=lambda name: "C:/pwsh.exe" if name == "pwsh" else None)
    @mock.patch.object(demo.subprocess, "run")
    def test_windows_idf_prefers_pwsh_and_stabilizes_platform_environment(
        self, run: mock.Mock, _which: mock.Mock
    ) -> None:
        with mock.patch.object(demo.os, "name", "nt"):
            demo.idf_command(Path("C:/idf"), ["build"], {"MSYSTEM": "MINGW64"})
        command_env = run.call_args.kwargs["env"]
        self.assertEqual("AMD64", command_env["PROCESSOR_ARCHITECTURE"])
        self.assertNotIn("MSYSTEM", command_env)
        self.assertEqual("C:/pwsh.exe", run.call_args.args[0][0])

    def test_configuration_is_stored_outside_repository(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / ".baby_monitor_demo.json"
            with mock.patch.object(demo, "CONFIG_PATH", config_path):
                demo.save_config(self.base_config())
                loaded = demo.load_config()
        self.assertEqual("device-1", loaded["device_id"])
        self.assertNotEqual(demo.ROOT, config_path.parent)


if __name__ == "__main__":
    unittest.main()

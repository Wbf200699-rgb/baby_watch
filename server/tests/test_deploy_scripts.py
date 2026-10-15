from pathlib import Path
import subprocess
import sys
from unittest import TestCase


SERVER_ROOT = Path(__file__).resolve().parents[1]
DEPLOY_ROOT = SERVER_ROOT / "deploy"


class DeployScriptTest(TestCase):
    def test_posix_deploy_files_use_unix_line_endings(self) -> None:
        paths = [
            *DEPLOY_ROOT.glob("*.sh"),
            *DEPLOY_ROOT.glob("*.service"),
            *DEPLOY_ROOT.glob("*.timer"),
        ]
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(path=path.name):
                self.assertNotIn(
                    b"\r\n",
                    path.read_bytes(),
                    f"{path.name} must use LF so /bin/sh and systemd can execute it",
                )

    def test_pi_recovery_always_uses_the_port_publishing_overlay(self) -> None:
        script = (DEPLOY_ROOT / "baby-monitor-recover.sh").read_text(encoding="utf-8")
        self.assertIn("-f compose.yaml -f compose.pi.yaml", script)
        self.assertIn("http://127.0.0.1:8088/health", script)

    def test_windows_publisher_strips_carriage_returns_from_ssh_command(self) -> None:
        script = (DEPLOY_ROOT / "publish_to_pi.ps1").read_text(encoding="utf-8")
        self.assertIn('$Command = $Command.Replace("`r", "")', script)

    def test_publishers_verify_archive_and_both_health_endpoints(self) -> None:
        windows = (DEPLOY_ROOT / "publish_to_pi.ps1").read_text(encoding="utf-8")
        posix = (DEPLOY_ROOT / "publish_to_pi.sh").read_text(encoding="utf-8")

        for script in (windows, posix):
            with self.subTest(script="powershell" if script is windows else "posix"):
                self.assertIn("sha256sum -c baby-monitor-server.tgz.sha256", script)
                self.assertIn("http://127.0.0.1:8088/health", script)
                self.assertIn("/health", script)
                self.assertIn("--wait-timeout 120", script)
                self.assertIn("data", script)

    def test_publishers_have_read_only_preflight_mode(self) -> None:
        windows = (DEPLOY_ROOT / "publish_to_pi.ps1").read_text(encoding="utf-8")
        posix = (DEPLOY_ROOT / "publish_to_pi.sh").read_text(encoding="utf-8")

        self.assertIn("[switch]$CheckOnly", windows)
        self.assertIn("if ($CheckOnly)", windows)
        self.assertIn('--check-only', posix)
        self.assertIn('CHECK_ONLY=1', posix)

    def test_p4_wifi_flash_scripts_keep_passwords_off_command_line(self) -> None:
        helper = (DEPLOY_ROOT / "flash_p4_wifi.py").read_text(encoding="utf-8")
        windows = (DEPLOY_ROOT / "flash_p4_wifi.ps1").read_text(encoding="utf-8")
        posix = (DEPLOY_ROOT / "flash_p4_wifi.sh").read_text(encoding="utf-8")

        self.assertIn('getpass.getpass', helper)
        self.assertIn('P4_WIFI_PASSWORD', helper)
        self.assertNotIn('--password"', helper)
        self.assertIn('Device credentials: loaded (values hidden)', helper)
        self.assertIn('flash_p4_wifi.py', windows)
        self.assertIn('flash_p4_wifi.py', posix)

    def test_p4_wifi_helper_supports_source_and_runtime_layouts(self) -> None:
        helper = (DEPLOY_ROOT / "flash_p4_wifi.py").read_text(encoding="utf-8")
        self.assertIn('PROJECT_ROOT / "p4x_eye" / "gateway"', helper)
        self.assertIn('PROJECT_ROOT / "p4" / "gateway"', helper)
        self.assertIn('"reconfigure", "build"', helper)
        self.assertIn('flash_args.append("flash")', helper)
        self.assertIn('env.setdefault("LOCALAPPDATA"', helper)

    def test_p4_wifi_helper_cli_is_valid(self) -> None:
        result = subprocess.run(
            [sys.executable, str(DEPLOY_ROOT / "flash_p4_wifi.py"), "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("--ssid", result.stdout)
        self.assertIn("--build-only", result.stdout)

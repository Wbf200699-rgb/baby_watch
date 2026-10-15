#!/usr/bin/env python3
"""Build and flash ESP32-P4X-EYE for a selected Wi-Fi and server."""

from __future__ import annotations

import argparse
import getpass
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from urllib.parse import urlsplit, urlunsplit


PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG_PATH = Path.home() / ".baby_monitor_demo.json"
DEFAULT_SERVER = "https://renesas.zyxserver.online"


def find_gateway_dir() -> Path:
    candidates = (
        PROJECT_ROOT / "p4x_eye" / "gateway",
        PROJECT_ROOT / "p4" / "gateway",
    )
    for candidate in candidates:
        if (candidate / "CMakeLists.txt").is_file() and (
            candidate / "sdkconfig.defaults"
        ).is_file():
            return candidate.resolve()
    searched = ", ".join(str(path) for path in candidates)
    raise SystemExit(f"P4 gateway source was not found. Searched: {searched}")


def config_path() -> Path:
    configured = os.getenv("P4_FLASH_CONFIG", "").strip()
    return Path(configured).expanduser() if configured else DEFAULT_CONFIG_PATH


def load_external_config() -> dict[str, object]:
    path = config_path()
    if not path.is_file():
        return {}
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"Cannot read external P4 configuration {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise SystemExit(f"External P4 configuration must be a JSON object: {path}")
    return value


def save_wifi(config: dict[str, object], ssid: str, password: str) -> None:
    path = config_path()
    config["wifi_ssid"] = ssid
    config["wifi_password"] = password
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    try:
        path.chmod(0o600)
    except OSError:
        pass
    print(f"Wi-Fi profile saved outside the repository: {path}")


def required_secret(name: str, config_key: str, config: dict[str, object]) -> str:
    value = os.getenv(name, "").strip() or str(config.get(config_key, "")).strip()
    if not value:
        raise SystemExit(
            f"{name} is missing. Run tools/demo_fallback.py configure once or set "
            f"{name} in the environment."
        )
    return value


def wifi_password(args: argparse.Namespace, config: dict[str, object]) -> str:
    value = os.getenv(args.password_env, "")
    if value:
        return value
    if str(config.get("wifi_ssid", "")) == args.ssid:
        saved = str(config.get("wifi_password", ""))
        if saved:
            return saved
    if args.check_only:
        return ""
    value = getpass.getpass(f"Wi-Fi password for {args.ssid}: ")
    if not value:
        raise SystemExit("Wi-Fi password cannot be empty")
    return value


def normalize_origins(server: str) -> tuple[str, str]:
    parsed = urlsplit(server.strip())
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise SystemExit("--server must be an http:// or https:// origin")
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise SystemExit("--server must not contain credentials, query, or fragment")
    path = parsed.path.rstrip("/")
    http_origin = urlunsplit((parsed.scheme, parsed.netloc, path, "", ""))
    ws_scheme = "wss" if parsed.scheme == "https" else "ws"
    ws_origin = urlunsplit((ws_scheme, parsed.netloc, path, "", ""))
    return http_origin, ws_origin


def find_idf_path(config: dict[str, object]) -> Path:
    candidates: list[Path] = []
    for value in (os.getenv("IDF_PATH"), config.get("idf_path")):
        if value:
            candidates.append(Path(str(value)).expanduser())
    if os.name == "nt":
        registry = Path.home() / ".espressif" / "idf-env.json"
        if registry.is_file():
            try:
                data = json.loads(registry.read_text(encoding="utf-8"))
                for key, entry in data.get("idfInstalled", {}).items():
                    candidates.extend((Path(str(entry.get("path", ""))), Path(key)))
            except (OSError, json.JSONDecodeError):
                pass
    else:
        candidates.extend((Path.home() / "esp" / "esp-idf", Path("/opt/esp/idf")))
    export_script = "export.ps1" if os.name == "nt" else "export.sh"
    for candidate in candidates:
        if str(candidate) and (candidate / export_script).is_file():
            return candidate.resolve()
    raise SystemExit("ESP-IDF was not found. Set IDF_PATH or idf_path in the external config.")


def run_idf(
    idf_path: Path, gateway_dir: Path, arguments: list[str], env: dict[str, str]
) -> None:
    if os.name == "nt":
        env.setdefault("LOCALAPPDATA", str(Path.home() / "AppData" / "Local"))
        env.setdefault("PROCESSOR_ARCHITECTURE", "AMD64")
        env.pop("MSYSTEM", None)
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            raise SystemExit("PowerShell was not found")
        quoted = subprocess.list2cmdline(["idf.py", *arguments])
        command = f"& '{idf_path / 'export.ps1'}'; {quoted}"
        subprocess.run(
            [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
            cwd=gateway_dir,
            env=env,
            check=True,
        )
        return
    command = 'source "$IDF_PATH/export.sh" >/dev/null && exec idf.py "$@"'
    subprocess.run(
        ["bash", "-lc", command, "idf.py", *arguments],
        cwd=gateway_dir,
        env=env,
        check=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssid", required=True, help="Wi-Fi SSID to compile into P4")
    parser.add_argument("--server", default=DEFAULT_SERVER, help="HTTP(S) server origin")
    parser.add_argument("--port", default="", help="P4 USB serial/JTAG port")
    parser.add_argument(
        "--password-env",
        default="P4_WIFI_PASSWORD",
        help="environment variable containing the Wi-Fi password",
    )
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--save-wifi", action="store_true")
    parser.add_argument("--build-dir", default="")
    args = parser.parse_args()

    if not args.ssid.strip():
        raise SystemExit("--ssid cannot be empty")
    config = load_external_config()
    gateway_dir = find_gateway_dir()
    idf_path = find_idf_path(config)
    device_id = required_secret("GATEWAY_DEVICE_ID", "device_id", config)
    device_key = required_secret("GATEWAY_DEVICE_KEY", "device_key", config)
    password = wifi_password(args, config)
    http_origin, ws_origin = normalize_origins(args.server)
    build_dir = (
        Path(args.build_dir).expanduser().resolve()
        if args.build_dir
        else gateway_dir / "build_p4_wifi"
    )

    print(f"P4 gateway source: {gateway_dir}")
    print(f"ESP-IDF: {idf_path}")
    print(f"Wi-Fi SSID: {args.ssid}")
    print(f"Server: {http_origin}")
    print(f"Build directory: {build_dir}")
    print("Device credentials: loaded (values hidden)")
    if args.check_only:
        print("Preflight OK; no build, flash, or configuration write was performed.")
        return

    env = os.environ.copy()
    env.update(
        {
            "GATEWAY_WIFI_SSID": args.ssid,
            "GATEWAY_WIFI_PASSWORD": password,
            "GATEWAY_DEVICE_ID": device_id,
            "GATEWAY_DEVICE_KEY": device_key,
            "GATEWAY_HTTP_ORIGIN": http_origin,
            "GATEWAY_WS_ORIGIN": ws_origin,
            "IDF_TARGET": "esp32p4",
            "SDKCONFIG_DEFAULTS": str((gateway_dir / "sdkconfig.defaults").resolve()),
        }
    )
    sdkconfig = build_dir / "sdkconfig"
    run_idf(
        idf_path,
        gateway_dir,
        ["-B", str(build_dir), "-D", f"SDKCONFIG={sdkconfig}", "reconfigure", "build"],
        env,
    )
    if not args.build_only:
        flash_args = ["-B", str(build_dir)]
        if args.port.strip():
            flash_args.extend(("-p", args.port.strip()))
        flash_args.append("flash")
        run_idf(idf_path, gateway_dir, flash_args, env)
        print("P4 flash completed successfully.")
    else:
        print("P4 alternate-Wi-Fi build completed; flash was intentionally skipped.")
    if args.save_wifi:
        save_wifi(config, args.ssid, password)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode) from exc

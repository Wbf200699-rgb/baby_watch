#!/usr/bin/env python3
"""Portable local-demo server and reversible ESP32-P4X-EYE flashing helper."""

from __future__ import annotations

import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import sys
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
SERVER_DIR = ROOT / "server"
GATEWAY_DIR = ROOT / "p4x_eye" / "gateway"
CONFIG_PATH = Path.home() / ".baby_monitor_demo.json"
CLOUD_HTTP_ORIGIN = "https://renesas.zyxserver.online"
CLOUD_WS_ORIGIN = "wss://renesas.zyxserver.online"
REQUIRED_GATEWAY_KEYS = (
    "wifi_ssid",
    "wifi_password",
    "device_id",
    "device_key",
)


def load_config(required: bool = True) -> dict[str, object]:
    if not CONFIG_PATH.exists():
        if required:
            raise SystemExit(
                f"Configuration is missing: {CONFIG_PATH}\n"
                "Run the configure script once before starting or flashing."
            )
        return {}
    config = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    missing = [key for key in REQUIRED_GATEWAY_KEYS if not config.get(key)]
    if required and missing:
        raise SystemExit(f"Configuration is missing: {', '.join(missing)}")
    return config


def save_config(config: dict[str, object]) -> None:
    CONFIG_PATH.write_text(
        json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    try:
        CONFIG_PATH.chmod(0o600)
    except OSError:
        pass
    print(f"Configuration saved outside the repository: {CONFIG_PATH}")


def discover_lan_ip() -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("1.1.1.1", 53))
        value = sock.getsockname()[0]
        if value and not value.startswith("127."):
            return value
    except OSError:
        pass
    finally:
        sock.close()
    return ""


def import_gateway_config_from_build(config: dict[str, object]) -> bool:
    compile_commands = GATEWAY_DIR / "build_codex_p4" / "compile_commands.json"
    if not compile_commands.exists():
        return False
    rows = json.loads(compile_commands.read_text(encoding="utf-8"))
    command = next(
        (
            row.get("command", "")
            for row in rows
            if row.get("file", "").replace("\\", "/").endswith("/gateway_main.c")
        ),
        "",
    )
    mapping = {
        "GATEWAY_WIFI_SSID": "wifi_ssid",
        "GATEWAY_WIFI_PASSWORD": "wifi_password",
        "GATEWAY_DEVICE_ID": "device_id",
        "GATEWAY_DEVICE_KEY": "device_key",
    }
    imported = False
    for macro, key in mapping.items():
        match = re.search(rf'-D{macro}=\\?"(.*?)\\?"(?:\s|$)', command)
        if match:
            config[key] = match.group(1).replace(r'\"', '"').replace(r"\\", "\\")
            imported = True
    return imported


def prompt_value(label: str, current: object = "", secret: bool = False) -> str:
    suffix = " [keep current]" if current else ""
    prompt = f"{label}{suffix}: "
    value = getpass.getpass(prompt) if secret else input(prompt)
    return value.strip() or str(current or "")


def configure(args: argparse.Namespace) -> None:
    config = load_config(required=False)
    if args.import_existing_build and import_gateway_config_from_build(config):
        print("Imported existing gateway credentials without displaying them.")
    suggested_ip = str(config.get("local_server_ip") or discover_lan_ip())
    config["wifi_ssid"] = prompt_value("P4 Wi-Fi SSID", config.get("wifi_ssid"))
    config["wifi_password"] = prompt_value(
        "P4 Wi-Fi password", config.get("wifi_password"), secret=True
    )
    config["device_id"] = prompt_value("Device ID", config.get("device_id"))
    config["device_key"] = prompt_value(
        "Device key", config.get("device_key"), secret=True
    )
    config["local_server_ip"] = prompt_value("Laptop LAN IPv4", suggested_ip)
    port_default = str(config.get("local_server_port", 8088))
    config["local_server_port"] = int(prompt_value("Local server port", port_default))
    config["admin_user"] = prompt_value(
        "Dashboard user (blank disables login)", config.get("admin_user")
    )
    config["admin_password"] = prompt_value(
        "Dashboard password", config.get("admin_password"), secret=True
    )
    config.setdefault("p4_port", "")
    missing = [key for key in REQUIRED_GATEWAY_KEYS if not config.get(key)]
    if missing:
        raise SystemExit(f"Required values are still missing: {', '.join(missing)}")
    save_config(config)


def health_ok(url: str) -> bool:
    try:
        with urllib.request.urlopen(url, timeout=2) as response:
            return response.status == 200
    except (OSError, urllib.error.URLError):
        return False


def venv_python() -> Path:
    if os.name == "nt":
        return SERVER_DIR / ".venv" / "Scripts" / "python.exe"
    return SERVER_DIR / ".venv" / "bin" / "python"


def ensure_server_venv() -> Path:
    python = venv_python()
    if not python.exists():
        subprocess.run([sys.executable, "-m", "venv", str(SERVER_DIR / ".venv")], check=True)
    requirements = SERVER_DIR / "requirements.txt"
    digest = hashlib.sha256(requirements.read_bytes()).hexdigest()
    stamp = python.parent.parent / ".demo_requirements.sha256"
    if not stamp.exists() or stamp.read_text(encoding="ascii").strip() != digest:
        subprocess.run(
            [str(python), "-m", "pip", "install", "-r", str(requirements)], check=True
        )
        stamp.write_text(digest + "\n", encoding="ascii")
    return python


def start_server(args: argparse.Namespace) -> None:
    config = load_config()
    port = int(args.port or config.get("local_server_port", 8088))
    ip = str(args.ip or config.get("local_server_ip") or discover_lan_ip())
    if not ip:
        raise SystemExit("No LAN IPv4 configured. Run configure again.")
    local_url = f"http://{ip}:{port}"
    if health_ok(f"http://127.0.0.1:{port}/health"):
        print(f"Local server is already healthy: {local_url}")
        return
    python = ensure_server_venv()
    env = os.environ.copy()
    env.update(
        {
            "BABY_MONITOR_DEVICE_KEYS": f"{config['device_id']}:{config['device_key']}",
            "BABY_MONITOR_ADMIN_USER": str(config.get("admin_user", "")),
            "BABY_MONITOR_ADMIN_PASSWORD": str(config.get("admin_password", "")),
            "BABY_MONITOR_DATA_DIR": str(SERVER_DIR / "data" / "demo_fallback"),
            "BABY_MONITOR_AI_ENABLED": "0",
        }
    )
    print(f"Dashboard: {local_url}")
    print("Keep this window open during the demonstration.")
    subprocess.run(
        [
            str(python),
            "-m",
            "uvicorn",
            "app.main:app",
            "--host",
            "0.0.0.0",
            "--port",
            str(port),
        ],
        cwd=SERVER_DIR,
        env=env,
        check=True,
    )


def find_idf_path(config: dict[str, object]) -> Path:
    candidates: list[Path] = []
    for value in (os.getenv("IDF_PATH"), config.get("idf_path")):
        if value:
            candidates.append(Path(str(value)).expanduser())
    if os.name == "nt":
        registry = Path.home() / ".espressif" / "idf-env.json"
        if registry.exists():
            data = json.loads(registry.read_text(encoding="utf-8"))
            for key, entry in data.get("idfInstalled", {}).items():
                candidates.extend([Path(str(entry.get("path", ""))), Path(key)])
    else:
        candidates.extend([Path.home() / "esp" / "esp-idf", Path("/opt/esp/idf")])
    script = "export.ps1" if os.name == "nt" else "export.sh"
    for candidate in candidates:
        if candidate and (candidate / script).exists():
            return candidate.resolve()
    raise SystemExit("ESP-IDF was not found. Set IDF_PATH or idf_path in the demo config.")


def idf_command(idf_path: Path, arguments: list[str], env: dict[str, str]) -> None:
    if os.name == "nt":
        env.setdefault("LOCALAPPDATA", str(Path.home() / "AppData" / "Local"))
        # ESP-IDF's platform detection can become "Windows-" under legacy
        # Windows PowerShell when PROCESSOR_ARCHITECTURE is not propagated.
        # Prefer PowerShell 7 (the path used by the verified workstation flow)
        # and provide an explicit architecture fallback.
        env.setdefault("PROCESSOR_ARCHITECTURE", "AMD64")
        env.pop("MSYSTEM", None)
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            raise SystemExit("PowerShell was not found; install PowerShell 7 or Windows PowerShell.")
        quoted = subprocess.list2cmdline(["idf.py", *arguments])
        command = f"& '{idf_path / 'export.ps1'}'; {quoted}"
        subprocess.run(
            [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
            cwd=GATEWAY_DIR,
            env=env,
            check=True,
        )
    else:
        command = 'source "$IDF_PATH/export.sh" >/dev/null && exec idf.py "$@"'
        subprocess.run(
            ["bash", "-lc", command, "idf.py", *arguments],
            cwd=GATEWAY_DIR,
            env=env,
            check=True,
        )


def gateway_environment(config: dict[str, object], target: str) -> tuple[dict[str, str], str]:
    env = os.environ.copy()
    env.update(
        {
            "GATEWAY_WIFI_SSID": str(config["wifi_ssid"]),
            "GATEWAY_WIFI_PASSWORD": str(config["wifi_password"]),
            "GATEWAY_DEVICE_ID": str(config["device_id"]),
            "GATEWAY_DEVICE_KEY": str(config["device_key"]),
            "IDF_TARGET": "esp32p4",
            "SDKCONFIG_DEFAULTS": str((GATEWAY_DIR / "sdkconfig.defaults").resolve()),
        }
    )
    if target == "local":
        ip = str(config.get("local_server_ip") or "")
        port = int(config.get("local_server_port", 8088))
        if not ip:
            raise SystemExit("local_server_ip is missing; run configure again")
        env["GATEWAY_HTTP_ORIGIN"] = f"http://{ip}:{port}"
        env["GATEWAY_WS_ORIGIN"] = f"ws://{ip}:{port}"
        description = env["GATEWAY_HTTP_ORIGIN"]
    else:
        env["GATEWAY_HTTP_ORIGIN"] = CLOUD_HTTP_ORIGIN
        env["GATEWAY_WS_ORIGIN"] = CLOUD_WS_ORIGIN
        description = CLOUD_HTTP_ORIGIN
    return env, description


def flash_gateway(args: argparse.Namespace) -> None:
    config = load_config()
    idf_path = find_idf_path(config)
    env, description = gateway_environment(config, args.target)
    build_dir = GATEWAY_DIR / f"build_demo_{args.target}"
    sdkconfig = build_dir / "sdkconfig"
    print(f"P4 target server: {description}")
    print(f"Build directory: {build_dir}")
    if args.check_only:
        print(f"ESP-IDF: {idf_path}")
        print("Preflight OK; no build or flash was performed.")
        return
    idf_command(
        idf_path,
        ["-B", str(build_dir), "-D", f"SDKCONFIG={sdkconfig}", "reconfigure", "build"],
        env,
    )
    port = str(args.port or config.get("p4_port") or "").strip()
    flash_args = ["-B", str(build_dir)]
    if port:
        flash_args.extend(["-p", port])
    flash_args.append("flash")
    idf_command(idf_path, flash_args, env)
    print(f"P4 flash complete: {args.target} -> {description}")


def status(_: argparse.Namespace) -> None:
    config = load_config()
    ip = str(config.get("local_server_ip") or "")
    port = int(config.get("local_server_port", 8088))
    local = f"http://{ip}:{port}" if ip else "not configured"
    print(f"Configuration: {CONFIG_PATH}")
    print(f"Device ID: {config.get('device_id')}")
    print(f"Local server: {local}")
    print(f"Cloud server: {CLOUD_HTTP_ORIGIN}")
    print(f"Platform: {platform.system()} {platform.machine()}")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    configure_parser = sub.add_parser("configure")
    configure_parser.add_argument("--import-existing-build", action="store_true")
    configure_parser.set_defaults(func=configure)
    server_parser = sub.add_parser("server")
    server_parser.add_argument("--ip")
    server_parser.add_argument("--port", type=int)
    server_parser.set_defaults(func=start_server)
    flash_parser = sub.add_parser("flash")
    flash_parser.add_argument("target", choices=("local", "cloud"))
    flash_parser.add_argument("--port")
    flash_parser.add_argument("--check-only", action="store_true")
    flash_parser.set_defaults(func=flash_gateway)
    status_parser = sub.add_parser("status")
    status_parser.set_defaults(func=status)
    return result


if __name__ == "__main__":
    arguments = parser().parse_args()
    arguments.func(arguments)

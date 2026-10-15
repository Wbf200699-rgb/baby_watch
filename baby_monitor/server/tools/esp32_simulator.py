from __future__ import annotations

import argparse
import itertools
import random
import time

import httpx


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32 server protocol simulator")
    parser.add_argument("--server", default="http://127.0.0.1:8088")
    parser.add_argument("--device", default="demo-device")
    parser.add_argument("--key", default="dev-secret")
    args = parser.parse_args()
    headers = {"X-Device-ID": args.device, "X-Device-Key": args.key}
    active: set[str] = set()
    with httpx.Client(base_url=args.server, headers=headers, timeout=5) as client:
        client.post("/api/v1/device/hello", json={"firmware": "sim-1.0", "capabilities": ["sd_collection"]}).raise_for_status()
        for sequence in itertools.count(1):
            command = client.get("/api/v1/commands/poll").json().get("command")
            if command:
                kinds = set(command["args"].get("kinds", []))
                if command["command"] == "collection.start":
                    active.update(kinds)
                elif command["command"] == "collection.stop":
                    active.difference_update(kinds)
                client.post(f"/api/v1/commands/{command['id']}/ack", json={"ok": True, "result": {"active_kinds": sorted(active)}})
            client.post("/api/v1/telemetry", json={
                "sequence": sequence,
                "device_time_ms": sequence * 500,
                "wifi_rssi": -48,
                "firmware": "sim-1.0",
                "sensors": {"thermal_valid": 5120, "pressure_active": random.randint(0, 12)},
                "inference": {"state": "sleeping", "confidence": 96.0},
                "health": {"sd_ready": True, "sd_free_mb": 15000, "active_kinds": sorted(active)},
            }).raise_for_status()
            print(f"seq={sequence} active={','.join(sorted(active)) or '-'}")
            time.sleep(0.5)


if __name__ == "__main__":
    main()

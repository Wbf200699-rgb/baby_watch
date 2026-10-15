import asyncio
from pathlib import Path
import sys
from unittest import IsolatedAsyncioTestCase


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.protocol import EventBroker  # noqa: E402


class EventBrokerTest(IsolatedAsyncioTestCase):
    async def test_high_rate_state_is_coalesced_to_latest_value(self) -> None:
        broker = EventBroker()
        iterator = broker.subscribe().__aiter__()
        pending = asyncio.create_task(anext(iterator))
        await asyncio.sleep(0)

        for sequence in range(100):
            await broker.publish(
                {"kind": "telemetry", "device_id": "dev", "data": {"sequence": sequence}}
            )

        message = await asyncio.wait_for(pending, timeout=1)
        self.assertEqual(message["data"]["sequence"], 99)
        await iterator.aclose()

    async def test_pressure_and_thermal_keep_independent_latest_frames(self) -> None:
        broker = EventBroker()
        iterator = broker.subscribe().__aiter__()
        pending = asyncio.create_task(anext(iterator))
        await asyncio.sleep(0)

        await broker.publish(
            {"kind": "matrix", "device_id": "dev", "data": {"kind": "pressure", "sequence": 1}}
        )
        await broker.publish(
            {"kind": "matrix", "device_id": "dev", "data": {"kind": "thermal", "sequence": 2}}
        )
        await broker.publish(
            {"kind": "matrix", "device_id": "dev", "data": {"kind": "pressure", "sequence": 3}}
        )

        first = await asyncio.wait_for(pending, timeout=1)
        second = await asyncio.wait_for(anext(iterator), timeout=1)
        by_kind = {item["data"]["kind"]: item["data"]["sequence"] for item in (first, second)}
        self.assertEqual(by_kind, {"thermal": 2, "pressure": 3})
        await iterator.aclose()

    async def test_non_state_events_are_not_coalesced(self) -> None:
        broker = EventBroker()
        iterator = broker.subscribe().__aiter__()
        pending = asyncio.create_task(anext(iterator))
        await asyncio.sleep(0)

        await broker.publish({"kind": "event", "device_id": "dev", "data": {"id": 1}})
        await broker.publish({"kind": "event", "device_id": "dev", "data": {"id": 2}})

        first = await asyncio.wait_for(pending, timeout=1)
        second = await asyncio.wait_for(anext(iterator), timeout=1)
        self.assertEqual([first["data"]["id"], second["data"]["id"]], [1, 2])
        await iterator.aclose()

    async def test_alert_burst_cannot_delay_latest_telemetry(self) -> None:
        broker = EventBroker()
        iterator = broker.subscribe().__aiter__()
        pending = asyncio.create_task(anext(iterator))
        await asyncio.sleep(0)

        for event_id in range(64):
            await broker.publish({
                "kind": "event",
                "device_id": "dev",
                "data": {"id": event_id, "event_type": "crying"},
            })
        await broker.publish({
            "kind": "telemetry",
            "device_id": "dev",
            "data": {"sequence": 9001},
        })

        message = await asyncio.wait_for(pending, timeout=1)
        self.assertEqual(message["kind"], "telemetry")
        self.assertEqual(message["data"]["sequence"], 9001)
        await iterator.aclose()

"""Session tests against a replay transport built from the recorded HCI session.

No watch is involved. The frames are the ones the real GBX-100 produced during
the 2026-10-27 connection (``work/evidence/att_frames_current.txt``).
"""

from __future__ import annotations

import asyncio
import sys
import unittest
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "py"))

from wristctl_proto import session  # noqa: E402
from wristctl_proto.generated import codecs  # noqa: E402

NAME = bytes.fromhex("23434153494f204742582d313030000000000000")
FEATURES = bytes.fromhex("105146214402da7f09030fffffffff27000000")
BLE_SETTINGS = bytes.fromhex("110000000600000000000100ffff00")
ADVERTISE = bytes.fromhex("3b50000a4001050540060a02a000054001051c00")
CONNECTION = bytes.fromhex("3a003401400104005802")
MODULE = bytes.fromhex("26e454e8411e3b122fe796000001eaffff")
CONDITION = bytes.fromhex("28061e2300ba010001")
VERSION = bytes.fromhex("2015050006000000000000000317000000000000")
SD_START = bytes.fromhex("4702")
SD_DONE = bytes.fromhex("4701")


class ReplayTransport:
    address = "DA:02:44:21:46:51"

    def __init__(self, frames: list[bytes]) -> None:
        self.frames = deque(frames)
        self.writes: list[tuple[str, object]] = []

    async def next_frame(self, timeout: float) -> bytes:
        del timeout
        if not self.frames:
            raise asyncio.TimeoutError
        return self.frames.popleft()

    async def write_request(self, frame: bytes) -> None:
        self.writes.append(("request", frame.hex()))

    async def write_all(self, frame: bytes) -> None:
        self.writes.append(("all", frame.hex()))

    async def write_notification(self, frame: bytes) -> None:
        self.writes.append(("notification", frame.hex()))

    async def write_link_loss_alert(self, level: int) -> None:
        self.writes.append(("lls", level))

    async def close(self) -> None:
        self.writes.append(("close", None))


class FakePairer:
    def __init__(self, result: bool = True) -> None:
        self.result = result
        self.calls: list[str] = []

    async def pair(self, address: str) -> bool:
        self.calls.append(address)
        return self.result


def standard_frames(*extra: bytes) -> list[bytes]:
    return [NAME, FEATURES, BLE_SETTINGS, ADVERTISE, CONNECTION, MODULE, CONDITION, VERSION, *extra]


class HandshakeTests(unittest.IsolatedAsyncioTestCase):
    async def test_gbx100_handshake(self) -> None:
        transport = ReplayTransport(standard_frames(SD_START, SD_DONE))
        fake_pairer = FakePairer()
        watch = session.WatchSession(transport, app_id=b"\x01" * 10, timeout=0.1)

        identity = await watch.handshake(fake_pairer, service_discovery_timeout=0.1)

        self.assertEqual(identity.name, "CASIO GBX-100")
        self.assertIsNotNone(identity.model)
        assert identity.model is not None
        self.assertEqual(identity.model.name, "GBX_100")
        self.assertEqual(identity.condition.mtu, 442)  # type: ignore[union-attr]
        self.assertEqual(identity.pairing, "bond_requested")
        self.assertEqual(fake_pairer.calls, ["DA:02:44:21:46:51"])
        self.assertEqual(identity.module, MODULE)

        kinds = [kind for kind, _ in transport.writes]
        self.assertEqual(
            kinds,
            ["request", "request", "all", "request", "request", "request", "request", "request", "request"],
        )
        self.assertEqual(transport.writes[0], ("request", "23"))
        self.assertEqual(transport.writes[1], ("request", "10"))
        app_info = codecs.decode_app_info(bytes.fromhex(transport.writes[2][1]))  # type: ignore[arg-type]
        self.assertEqual(app_info.app_id, b"\x01" * 10)
        self.assertEqual(app_info.os, 2)

    async def test_service_discovery_start_is_parked_until_needed(self) -> None:
        frames = [NAME, FEATURES, BLE_SETTINGS, ADVERTISE, CONNECTION, MODULE, SD_START, CONDITION, VERSION, SD_DONE]
        transport = ReplayTransport(frames)
        watch = session.WatchSession(transport, app_id=b"\x02" * 10, timeout=0.1)
        identity = await watch.handshake(FakePairer(), service_discovery_timeout=0.1)
        self.assertEqual(identity.pairing, "bond_requested")

    async def test_no_service_discovery_is_reported(self) -> None:
        transport = ReplayTransport(standard_frames())
        watch = session.WatchSession(transport, app_id=b"\x03" * 10, timeout=0.1)
        identity = await watch.handshake(FakePairer(), service_discovery_timeout=0.1)
        self.assertEqual(identity.pairing, "not_observed")

    async def test_unknown_model_still_completes(self) -> None:
        unknown = bytes.fromhex("2341434d4520574154434800000000000000000000")
        frames = [unknown, FEATURES, BLE_SETTINGS, ADVERTISE, CONNECTION, MODULE, CONDITION, VERSION, SD_START, SD_DONE]
        transport = ReplayTransport(frames)
        watch = session.WatchSession(transport, app_id=b"\x04" * 10, timeout=0.1)
        identity = await watch.handshake(None, service_discovery_timeout=0.1)
        self.assertEqual(identity.name, "ACME WATCH")
        self.assertIsNone(identity.model)
        self.assertEqual(identity.pairing, "link_loss_alert")


class CorrelationTests(unittest.IsolatedAsyncioTestCase):
    async def test_error_frame_raises(self) -> None:
        transport = ReplayTransport([bytes.fromhex("ff8109")])
        watch = session.WatchSession(transport, timeout=0.1)
        with self.assertRaises(session.SessionError):
            await watch.request(0x09)

    async def test_unrelated_frames_are_parked(self) -> None:
        transport = ReplayTransport([CONDITION, bytes.fromhex("3d3000"), VERSION])
        watch = session.WatchSession(transport, timeout=0.1)
        version = await watch.request(0x20)
        self.assertEqual(version, VERSION)
        self.assertEqual(watch.pending[0x28], [CONDITION])
        self.assertEqual(watch.pending[0x3D], [bytes.fromhex("3d3000")])

    async def test_request_bytes_with_unit(self) -> None:
        frame = bytes.fromhex("1d00010202b5760000ffffffffffff")
        transport = ReplayTransport([frame])
        watch = session.WatchSession(transport, timeout=0.1)
        result = await watch.request_bytes(bytes([0x1D, 0x00]))
        self.assertEqual(result, frame)
        self.assertEqual(transport.writes, [("request", "1d00")])

    async def test_timeout(self) -> None:
        transport = ReplayTransport([])
        watch = session.WatchSession(transport, timeout=0.01)
        with self.assertRaises(session.SessionError):
            await watch.request(0x23)


if __name__ == "__main__":
    unittest.main()

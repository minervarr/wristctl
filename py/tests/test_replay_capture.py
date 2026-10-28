"""End-to-end session replay against the real recorded HCI session.

Feeds every notification the watch sent on the settings characteristic, in
order, and runs the handshake. Skipped when the evidence tree is absent so the
suite still runs on a clean checkout.
"""

from __future__ import annotations

import asyncio
import re
import sys
import unittest
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "py"))

from wristctl_proto import session  # noqa: E402

CAPTURE = ROOT.parent / "GSHOCK_DECOMP" / "work" / "evidence" / "att_frames_current.txt"
LINE_RE = re.compile(r"^Handle Value Notification\t0x000e\t([0-9a-f]+)$")


def capture_frames(path: Path) -> list[bytes]:
    frames = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = LINE_RE.match(line)
        if match:
            frames.append(bytes.fromhex(match.group(1)))
    return frames


class CaptureTransport:
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
        pass


class CapturePairer:
    def __init__(self) -> None:
        self.calls: list[str] = []

    async def pair(self, address: str) -> bool:
        self.calls.append(address)
        return True


@unittest.skipUnless(CAPTURE.exists(), f"capture not present: {CAPTURE}")
class CaptureReplayTests(unittest.IsolatedAsyncioTestCase):
    async def test_handshake_against_recorded_session(self) -> None:
        frames = capture_frames(CAPTURE)
        self.assertGreater(len(frames), 15)
        transport = CaptureTransport(frames)
        pairer = CapturePairer()
        watch = session.WatchSession(transport, app_id=b"\x07" * 10, timeout=0.05)

        identity = await watch.handshake(pairer, service_discovery_timeout=0.05)

        self.assertEqual(identity.name, "CASIO GBX-100")
        self.assertIsNotNone(identity.model)
        assert identity.model is not None
        self.assertEqual(identity.model.name, "GBX_100")
        self.assertEqual(identity.pairing, "bond_requested")
        self.assertEqual(pairer.calls, ["DA:02:44:21:46:51"])
        self.assertEqual(identity.condition.mtu, 442)  # type: ignore[union-attr]

        # The vendor sequence starts with name, features, app info, BLE settings.
        self.assertEqual(transport.writes[0], ("request", "23"))
        self.assertEqual(transport.writes[1], ("request", "10"))
        self.assertEqual(transport.writes[2][0], "all")
        self.assertTrue(transport.writes[2][1].startswith("22"))  # type: ignore[union-attr]
        self.assertEqual(transport.writes[3], ("request", "11"))

        # Frames that were not asked for are parked, not lost.
        self.assertIn(0x1D, watch.pending)
        self.assertIn(0x1E, watch.pending)
        self.assertIn(0x2E, watch.pending)


if __name__ == "__main__":
    unittest.main()

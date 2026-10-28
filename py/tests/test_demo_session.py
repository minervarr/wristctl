"""The UI's write paths, exercised against the simulator (no GTK, no watch)."""

from __future__ import annotations

import sys
import unittest
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "py"))

from wristctl_proto import session, timeutil  # noqa: E402
from wristctl_proto.generated import codecs  # noqa: E402
from wristctl_proto.ui.demo import DemoTransport  # noqa: E402


class DemoTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.transport = DemoTransport()
        self.watch = session.WatchSession(
            self.transport, app_id=b"\x05" * 10, timeout=0.01
        )
        self.identity = await self.watch.handshake(None, service_discovery_timeout=0.01)

    async def test_identity(self) -> None:
        self.assertEqual(self.identity.name, "CASIO GBX-100")
        self.assertIsNotNone(self.identity.model)
        assert self.identity.model is not None
        self.assertEqual(self.identity.model.name, "GBX_100")
        self.assertEqual(self.identity.pairing, "bond_requested")

    async def test_basic_settings_round_trip(self) -> None:
        basic = await self.watch.read_basic_settings()
        self.assertEqual(basic.flags, 0x44)
        basic.time_format_24h = True
        packet = codecs.encode_basic_setting(basic)
        await self.watch.write_all(packet)
        again = await self.watch.read_basic_settings()
        self.assertTrue(again.time_format_24h)
        self.assertTrue(again.auto_light_off)  # unknown bits preserved

    async def test_time_write(self) -> None:
        dt = datetime(2026, 10, 27, 16, 41, 9, 765625)
        packet = codecs.encode_current_time(timeutil.to_current_time(dt))
        await self.watch.write_time(packet)
        self.assertEqual(self.transport.state[0x09], packet)
        self.assertEqual(packet.hex(), "09ea070a1b10290902c401")

    async def test_alarms_round_trip(self) -> None:
        first_raw, rest_raw = await self.watch.read_alarms()
        first = codecs.decode_alarm_first(first_raw)
        rest = codecs.decode_alarm_rest(rest_raw)
        self.assertEqual((first.hour, first.minute, first.enabled), (7, 30, True))

        first.enabled = False
        rest.alarms[0].enabled = True
        rest.alarms[0].hour = 6
        rest.alarms[0].minute = 15
        await self.watch.write_all(codecs.encode_alarm_first(first))
        await self.watch.write_all(codecs.encode_alarm_rest(rest))

        first_raw, rest_raw = await self.watch.read_alarms()
        again_first = codecs.decode_alarm_first(first_raw)
        again_rest = codecs.decode_alarm_rest(rest_raw)
        self.assertFalse(again_first.enabled)
        self.assertTrue(again_rest.alarms[0].enabled)
        self.assertEqual(again_rest.alarms[0].hour, 6)
        self.assertEqual(again_rest.alarms[0].minute, 15)

    async def test_program_timer_round_trip(self) -> None:
        timer = await self.watch.read_program_timer()
        self.assertEqual(timer.repeat, 1)
        entry = codecs.ProgramTimerWorkoutsItem(minutes=5, seconds=0, beep=0)
        value = codecs.ProgramTimer(
            repeat=0,
            workouts=[entry] + [codecs.ProgramTimerWorkoutsItem(0, 0, 0)] * 5,
        )
        await self.watch.write_all(codecs.encode_program_timer(value))
        again = await self.watch.read_program_timer()
        self.assertEqual(again.repeat, 0)
        self.assertEqual((again.workouts[0].minutes, again.workouts[0].seconds), (5, 0))

    async def test_notification(self) -> None:
        packet = await self.watch.send_notification(app="Demo", title="Hi", text="There")
        self.assertEqual(packet[0], 0xFF)
        self.assertEqual(self.transport.writes[-1], ("notification", packet))


if __name__ == "__main__":
    unittest.main()

"""Tests for the variable-length pieces: notifications and time conversion."""

from __future__ import annotations

import sys
import unittest
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "py"))

from wristctl_proto import notify, timeutil  # noqa: E402


class TimeTests(unittest.TestCase):
    def test_live_capture_round_trip(self) -> None:
        dt = datetime(2026, 10, 27, 16, 41, 9, 765625)
        value = timeutil.to_current_time(dt)
        self.assertEqual(value.weekday, 2)  # Tuesday, Sunday=0
        self.assertEqual(value.nano, 196)
        self.assertEqual(
            timeutil.codecs.encode_current_time(value).hex(),
            "09ea070a1b10290902c401",
        )

    def test_zone_name_is_usable(self) -> None:
        name = timeutil.local_zone_name()
        self.assertIsInstance(name, str)
        self.assertTrue(name)


class NotificationTests(unittest.TestCase):
    def test_round_trip(self) -> None:
        packet = notify.encode_notification(
            app="Test",
            title="Hello",
            text="World",
            short_text="Hi",
            notif_type=1,
            at="20261027T164109",
        )
        self.assertEqual(packet[0], 0xFF)  # header 0x00 XORed with 0xFF
        decoded = notify.decode_notification(packet)
        self.assertEqual(decoded["type"], 1)
        self.assertEqual(decoded["timestamp"], "20261027T164109")
        self.assertEqual(decoded["app"], "Test")
        self.assertEqual(decoded["title"], "Hello")
        self.assertEqual(decoded["short_text"], "Hi")
        self.assertEqual(decoded["text"], "World")

    def test_utf8(self) -> None:
        packet = notify.encode_notification(app="Aplicación", text="café")
        self.assertEqual(notify.decode_notification(packet)["text"], "café")

    def test_field_limit(self) -> None:
        with self.assertRaises(ValueError):
            notify.encode_notification(app="x" * 256)

    def test_timestamp_limit(self) -> None:
        with self.assertRaises(ValueError):
            notify.encode_notification(app="a", at="2026")


if __name__ == "__main__":
    unittest.main()

"""Time conversion helpers shared by the CLI and the GTK app."""

from __future__ import annotations

import os
from datetime import datetime
from zoneinfo import ZoneInfo

from .generated import codecs


def to_current_time(dt: datetime) -> codecs.CurrentTime:
    """Build the watch value; weekday is Sunday=0, nano is the capture's byte."""
    nanos = dt.microsecond * 1000
    return codecs.CurrentTime(
        year=dt.year,
        month=dt.month,
        day=dt.day,
        hour=dt.hour,
        minute=dt.minute,
        second=dt.second,
        weekday=(dt.weekday() + 1) % 7,
        nano=(nanos * 256 // 1_000_000_000) & 0xFF,
    )


def local_zone_name() -> str:
    """Best-effort IANA name for the host timezone."""
    target = ""
    try:
        target = os.path.realpath("/etc/localtime")
    except OSError:
        pass
    marker = "zoneinfo/"
    if marker in target:
        return target.split(marker, 1)[1]
    return os.environ.get("TZ") or "UTC"


def zone(name: str) -> ZoneInfo:
    return ZoneInfo(name.strip() or local_zone_name())

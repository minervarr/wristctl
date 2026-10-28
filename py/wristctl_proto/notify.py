"""App notifications (class 0x30).

The payload is variable length, so it is a procedure rather than a spec class.
Layout, from the capture and the vendor app:

    00 00 00 00 00 01
    type (1)
    timestamp (15 ASCII, YYYYMMDDTHHMMSS)
    app     [len][00][utf8]
    title   [len][00][utf8]
    short   [len][00][utf8]
    text    [len][00][utf8]

Every byte is XORed with 0xFF on the wire.
"""

from __future__ import annotations

from datetime import datetime

XOR_KEY = 0xFF
HEADER = b"\x00\x00\x00\x00\x00\x01"


def timestamp(dt: datetime | None = None) -> str:
    return (dt or datetime.now()).strftime("%Y%m%dT%H%M%S")


def encode_notification(
    *,
    app: str,
    title: str = "",
    text: str = "",
    short_text: str = "",
    notif_type: int = 0,
    at: str | None = None,
) -> bytes:
    stamp = at or timestamp()
    if len(stamp) != 15:
        raise ValueError("timestamp must be 15 characters, e.g. 20261027T164109")
    if not 0 <= notif_type <= 0xFF:
        raise ValueError("notification type out of range")
    parts = bytearray(HEADER)
    parts.append(notif_type)
    parts += stamp.encode("ascii")
    for value in (app, title, short_text, text):
        raw = value.encode("utf-8")
        if len(raw) > 255:
            raise ValueError("notification field exceeds 255 bytes")
        parts += bytes([len(raw), 0x00]) + raw
    return bytes(byte ^ XOR_KEY for byte in parts)


def decode_notification(frame: bytes) -> dict:
    raw = bytes(byte ^ XOR_KEY for byte in frame)
    if len(raw) < len(HEADER) + 1 + 15 or not raw.startswith(HEADER):
        raise ValueError("not a notification frame")
    offset = len(HEADER)
    notif_type = raw[offset]
    offset += 1
    stamp = raw[offset : offset + 15].decode("ascii", "replace")
    offset += 15
    values = {}
    for name in ("app", "title", "short_text", "text"):
        if offset + 2 > len(raw):
            raise ValueError("truncated notification field header")
        length = raw[offset]
        offset += 2
        if offset + length > len(raw):
            raise ValueError("truncated notification field body")
        values[name] = raw[offset : offset + length].decode("utf-8", "replace")
        offset += length
    return {"type": notif_type, "timestamp": stamp, **values}

"""A watch simulator for the UI, built from the recorded session.

It answers the handshake and the read classes from fixed frames, records every
write, and feeds back the last written value for the classes it understands, so
the window can be exercised without hardware.

    python3 -m wristctl_proto.ui --demo
"""

from __future__ import annotations

import asyncio
from collections import deque

NAME = bytes.fromhex("23434153494f204742582d313030000000000000")
FEATURES = bytes.fromhex("105146214402da7f09030fffffffff27000000")
BLE_SETTINGS = bytes.fromhex("110000000600000000000100ffff00")
ADVERTISE = bytes.fromhex("3b50000a4001050540060a02a000054001051c00")
CONNECTION = bytes.fromhex("3a003401400104005802")
MODULE = bytes.fromhex("26e454e8411e3b122fe796000001eaffff")
CONDITION = bytes.fromhex("28061e2300ba010001")
VERSION = bytes.fromhex("2015050006000000000000000317000000000000")
BASIC = bytes.fromhex("134400000000000000000002")
ALARM_FIRST = bytes.fromhex("154040071e")
ALARM_REST = bytes.fromhex("1600400000004000000040000000400000")
PROGRAM_TIMER = bytes([0x2A, 0x01]) + bytes([0x00, 0x25, 0x00]) + bytes([0x00, 0x05, 0x00]) + bytes(12)


class DemoTransport:
    address = "DE:MO:00:00:00:01"

    def __init__(self) -> None:
        self.queue: deque[bytes] = deque(
            [
                NAME,
                FEATURES,
                BLE_SETTINGS,
                ADVERTISE,
                CONNECTION,
                MODULE,
                CONDITION,
                VERSION,
                bytes.fromhex("4702"),
                bytes.fromhex("4701"),
            ]
        )
        self.state: dict[int, bytes] = {
            0x13: BASIC,
            0x15: ALARM_FIRST,
            0x16: ALARM_REST,
            0x2A: PROGRAM_TIMER,
        }
        self.last_class: int | None = None
        self.writes: list[tuple[str, bytes]] = []

    async def connect(self) -> None:
        self.writes.append(("connect", b""))

    async def next_frame(self, timeout: float) -> bytes:
        del timeout
        if self.queue:
            return self.queue.popleft()
        if self.last_class is not None and self.last_class in self.state:
            return self.state[self.last_class]
        raise asyncio.TimeoutError

    async def write_request(self, frame: bytes) -> None:
        self.last_class = frame[0]
        self.writes.append(("request", frame))

    async def write_all(self, frame: bytes) -> None:
        self.writes.append(("all", frame))
        if frame[0] in (0x09, 0x13, 0x15, 0x16, 0x22, 0x2A):
            self.state[frame[0]] = frame

    async def write_notification(self, frame: bytes) -> None:
        self.writes.append(("notification", frame))

    async def write_link_loss_alert(self, level: int) -> None:
        self.writes.append(("lls", bytes([level])))

    async def close(self) -> None:
        self.writes.append(("close", b""))

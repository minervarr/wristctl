"""BLE transport (Bleak/BlueZ) and the Linux pairing backend.

This module is deliberately untested against hardware in this repository: the
watch was not advertising when it was written. The session layer above it is
covered by replay tests. ``tools/probe.py`` is the hardware gate.
"""

from __future__ import annotations

import asyncio
import re
import subprocess
from typing import Any

try:  # keep the package importable without the optional dependency
    from bleak import BleakClient, BleakScanner
    from bleak.backends.characteristic import BleakGATTCharacteristic
    from bleak.backends.device import BLEDevice
except ImportError:  # pragma: no cover - exercised only on hosts without bleak
    BleakClient = None  # type: ignore[assignment]
    BleakScanner = None  # type: ignore[assignment]
    BleakGATTCharacteristic = Any  # type: ignore[misc]
    BLEDevice = Any  # type: ignore[misc]

from .session import SessionError

SERVICE_UUID = "26eb000d-b012-49a8-b1f8-394fb2032b0f"
REQUEST_UUID = "26eb002c-b012-49a8-b1f8-394fb2032b0f"
ALL_FEATURES_UUID = "26eb002d-b012-49a8-b1f8-394fb2032b0f"
NOTIFICATION_UUID = "26eb0030-b012-49a8-b1f8-394fb2032b0f"

LINK_LOSS_SERVICE_UUID = "00001803-0000-1000-8000-00805f9b34fb"
LINK_LOSS_ALERT_UUID = "00002a06-0000-1000-8000-00805f9b34fb"

NAME_PREFIXES = ("CASIO", "GBX", "GBD", "GPR", "GWF", "GST", "GMW", "MRG", "DW-", "GM-")


def _name_matches(name: str | None, needle: str | None) -> bool:
    if not name:
        return False
    if needle:
        return needle.upper() in name.upper()
    upper = name.upper()
    return any(upper.startswith(prefix) for prefix in NAME_PREFIXES)


class BleakTransport:
    def __init__(
        self,
        address: str | None = None,
        *,
        name: str | None = None,
        timeout: float = 20.0,
        debug: bool = False,
    ) -> None:
        self.address = address or ""
        self.name = name
        self.timeout = timeout
        self.debug = debug
        self.client: Any = None
        self.queue: asyncio.Queue[bytes] = asyncio.Queue()
        self.notification_chars: list[str] = []

    @staticmethod
    async def scan(
        timeout: float = 12.0, *, name: str | None = None, address: str | None = None
    ) -> list[Any]:
        if BleakScanner is None:
            raise SessionError("bleak is not installed; install the 'ble' extra")
        found: dict[str, Any] = {}

        def callback(device: Any, advertisement: Any) -> None:
            del advertisement
            if address:
                match = device.address.upper() == address.upper()
            else:
                match = _name_matches(device.name, name)
            if match:
                found[device.address] = device

        scanner = BleakScanner(detection_callback=callback)
        await scanner.start()
        try:
            await asyncio.sleep(timeout)
        finally:
            await scanner.stop()
        return list(found.values())

    async def connect(self, device: Any | None = None) -> None:
        if BleakClient is None or BleakScanner is None:
            raise SessionError("bleak is not installed; install the 'ble' extra")
        if device is None:
            devices = await self.scan(12.0, name=self.name, address=self.address or None)
            if not devices and self.address:
                # The watch advertises in short windows; try once more.
                devices = await self.scan(20.0, name=self.name, address=self.address)
            if not devices:
                raise SessionError(
                    "watch not found. Free it from the phone (Bluetooth off or "
                    "disconnect in MOVE), then press a watch button to wake advertising."
                )
            device = devices[0]
        self.address = device.address
        self.client = BleakClient(device, timeout=self.timeout)
        await self.client.connect()
        if not self.client.is_connected:
            raise SessionError(f"connect to {self.address} did not reach connected state")
        for service in self.client.services:
            for char in service.characteristics:
                if "notify" in char.properties or "indicate" in char.properties:
                    try:
                        await self.client.start_notify(char, self._on_notify)
                        self.notification_chars.append(char.uuid)
                    except Exception as exc:  # noqa: BLE001 - backend specific
                        if self.debug:
                            print(f"notify subscribe failed for {char.uuid}: {exc}")
        if not self.notification_chars:
            raise SessionError("watch exposed no notifying characteristic")

    def _on_notify(self, characteristic: Any, data: bytearray) -> None:
        frame = bytes(data)
        if self.debug:
            print(f"<- {characteristic.uuid} {frame.hex()}")
        self.queue.put_nowait(frame)

    async def next_frame(self, timeout: float) -> bytes:
        return await asyncio.wait_for(self.queue.get(), timeout)

    async def write_request(self, frame: bytes) -> None:
        await self._write(REQUEST_UUID, frame, response=False)

    async def write_all(self, frame: bytes) -> None:
        await self._write(ALL_FEATURES_UUID, frame, response=True)

    async def write_notification(self, frame: bytes) -> None:
        await self._write(NOTIFICATION_UUID, frame, response=False)

    async def write_link_loss_alert(self, level: int) -> None:
        if self.client is None:
            raise SessionError("not connected")
        await self.client.write_gatt_char(
            LINK_LOSS_ALERT_UUID, bytes([level & 0xFF]), response=True
        )

    async def _write(self, uuid: str, frame: bytes, *, response: bool) -> None:
        if self.client is None:
            raise SessionError("not connected")
        if self.debug:
            print(f"-> {uuid} {frame.hex()} response={response}")
        await self.client.write_gatt_char(uuid, frame, response=response)

    async def close(self) -> None:
        if self.client is not None:
            try:
                await self.client.disconnect()
            finally:
                self.client = None

    # -- bond state ------------------------------------------------------

    def bonded(self) -> bool:
        return bluez_bonded(self.address)


def bluez_bonded(address: str) -> bool:
    try:
        result = subprocess.run(
            ["bluetoothctl", "info", address],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False
    return re.search(r"^\s*Bonded:\s*yes\s*$", result.stdout, re.MULTILINE) is not None


class BluezPairer:
    """Bond through bluetoothctl with a NoInputNoOutput agent.

    Legacy PIN pairing (some models call ``setPin([0])``) cannot be expressed
    with this agent. If the watch demands a PIN, run pairing by hand and re-run
    the probe; the result reports the bond state either way.
    """

    def __init__(self, *, timeout: float = 45.0) -> None:
        self.timeout = timeout

    async def pair(self, address: str) -> bool:
        script = f"agent NoInputNoOutput\ndefault-agent\npair {address}\nquit\n"
        try:
            subprocess.run(
                ["bluetoothctl"],
                input=script,
                text=True,
                capture_output=True,
                timeout=self.timeout,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired):
            return False
        return bluez_bonded(address)

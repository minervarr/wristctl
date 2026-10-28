"""Transport-agnostic watch session.

The session owns request/response correlation and the connection handshake. It
does not know how bytes reach the watch: ``ble.BleakTransport`` does that on a
real host, and the tests use a replay transport built from the recorded HCI
session.

Order of operations follows the observed vendor session
(``work/evidence/att_frames_current.txt``): identify, configure, then answer the
watch's service-discovery request (``0x47 02``) with either a bond or a Link
Loss Alert write, and wait for ``0x47 01``.
"""

from __future__ import annotations

import asyncio
import os
import random
from dataclasses import dataclass, field
from typing import Protocol

from . import notify
from .generated import codecs, models

CLASS_WATCH_NAME = 0x23
CLASS_BLE_FEATURES = 0x10
CLASS_BLE_SETTING = 0x11
CLASS_APP_INFO = 0x22
CLASS_ADVERTISE = 0x3B
CLASS_CONNECTION = 0x3A
CLASS_MODULE = 0x26
CLASS_CONDITION = 0x28
CLASS_VERSION = 0x20
CLASS_SERVICE_DISCOVERY = 0x47
CLASS_ERROR = 0xFF

SERVICE_DISCOVERY_START = 2
SERVICE_DISCOVERY_DONE = 1


class SessionError(RuntimeError):
    """The watch rejected a request or the link did not behave as expected."""


class Transport(Protocol):
    """What a session needs from a BLE backend."""

    address: str

    async def write_request(self, frame: bytes) -> None:
        """Write to the request characteristic (write without response)."""

    async def write_all(self, frame: bytes) -> None:
        """Write to the settings characteristic (write with response)."""

    async def write_notification(self, frame: bytes) -> None:
        """Write to the notification characteristic (write without response)."""

    async def write_link_loss_alert(self, level: int) -> None:
        """Write the standard Link Loss Alert level, if the watch exposes it."""

    async def next_frame(self, timeout: float) -> bytes:
        """Return the next notification frame."""

    async def close(self) -> None:
        """Close the link."""


class Pairer(Protocol):
    async def pair(self, address: str) -> bool:
        """Bond with the watch. Returns True when the bond is established."""


@dataclass
class Identity:
    name: str = ""
    model: models.Model | None = None
    address: str = ""
    ble_features: bytes = b""
    ble_settings: bytes = b""
    advertise_parameters: bytes = b""
    connection_parameters: bytes = b""
    module: bytes = b""
    version: bytes = b""
    condition: codecs.WatchCondition | None = None
    condition_raw: bytes = b""
    pairing: str = "unknown"
    extras: dict[str, bytes] = field(default_factory=dict)


def new_app_id() -> bytes:
    """A per-install 10-byte identifier, mirroring the vendor app's random ID."""
    return bytes(random.SystemRandom().getrandbits(8) for _ in range(10))


class WatchSession:
    def __init__(
        self,
        transport: Transport,
        *,
        app_id: bytes | None = None,
        timeout: float = 5.0,
    ) -> None:
        self.transport = transport
        self.timeout = timeout
        self.app_id = app_id or new_app_id()
        if len(self.app_id) != 10:
            raise ValueError("app_id must be 10 bytes")
        self.pending: dict[int, list[bytes]] = {}
        self.max_parked = 32

    # -- framing ---------------------------------------------------------

    def _park(self, frame: bytes) -> None:
        parked = self.pending.setdefault(frame[0], [])
        if len(parked) >= self.max_parked:
            parked.pop(0)
        parked.append(frame)

    async def _take(self, timeout: float | None = None) -> bytes:
        remaining = self.timeout if timeout is None else timeout
        try:
            return await self.transport.next_frame(remaining)
        except asyncio.TimeoutError as exc:
            raise SessionError(f"timed out waiting for a frame ({remaining:.1f}s)") from exc

    async def wait_for(self, class_id: int, timeout: float | None = None) -> bytes:
        """Return the next frame of ``class_id``, parking unrelated frames."""
        if self.pending.get(class_id):
            return self.pending[class_id].pop(0)
        while True:
            frame = await self._take(timeout)
            if not frame:
                continue
            if frame[0] == class_id:
                return frame
            if frame[0] == CLASS_ERROR:
                error = codecs.decode_error(frame)
                raise SessionError(
                    f"watch rejected class 0x{error.class_id:02x} "
                    f"(status 0x{error.status:02x})"
                )
            self._park(frame)

    async def request(self, class_id: int, timeout: float | None = None) -> bytes:
        await self.transport.write_request(bytes([class_id]))
        return await self.wait_for(class_id, timeout)

    async def request_bytes(self, payload: bytes, timeout: float | None = None) -> bytes:
        await self.transport.write_request(payload)
        return await self.wait_for(payload[0], timeout)

    async def write_all(self, frame: bytes) -> None:
        await self.transport.write_all(frame)

    # -- handshake -------------------------------------------------------

    async def handshake(
        self,
        pairer: Pairer | None = None,
        *,
        expect_service_discovery: bool = True,
        service_discovery_timeout: float = 20.0,
    ) -> Identity:
        identity = Identity(address=getattr(self.transport, "address", ""))

        name_frame = await self.request(CLASS_WATCH_NAME)
        identity.name = codecs.decode_watch_name(name_frame).name
        identity.model = models.lookup(identity.name)
        self._model_cache = identity.model

        identity.ble_features = await self.request(CLASS_BLE_FEATURES)

        await self.write_all(
            codecs.encode_app_info(codecs.AppInfo(app_id=self.app_id, os=2))
        )

        identity.ble_settings = await self.request(CLASS_BLE_SETTING)
        identity.advertise_parameters = await self.request(CLASS_ADVERTISE)
        identity.connection_parameters = await self.request(CLASS_CONNECTION)
        identity.module = await self.request(CLASS_MODULE)

        condition = await self.request(CLASS_CONDITION)
        identity.condition_raw = condition
        try:
            identity.condition = codecs.decode_watch_condition(condition)
        except codecs.ProtoError:
            identity.condition = None

        identity.version = await self.request(CLASS_VERSION)

        if expect_service_discovery:
            identity.pairing = await self._service_discovery(pairer, service_discovery_timeout)
        return identity

    async def _service_discovery(
        self, pairer: Pairer | None, timeout: float
    ) -> str:
        try:
            start = await self.wait_for(CLASS_SERVICE_DISCOVERY, timeout)
        except SessionError:
            return "not_observed"
        command = start[1] if len(start) > 1 else 0
        if command != SERVICE_DISCOVERY_START:
            return f"unexpected_command_{command}"

        model = await self._model()
        if model is not None and models.flag(model, "isPairingInsteadOfLlsAl"):
            if pairer is not None:
                await pairer.pair(getattr(self.transport, "address", ""))
            outcome = "bond_requested"
        else:
            try:
                await self.transport.write_link_loss_alert(1)
                outcome = "link_loss_alert"
            except Exception:  # noqa: BLE001 - the characteristic may be absent
                outcome = "link_loss_alert_unavailable"

        done = await self.wait_for(CLASS_SERVICE_DISCOVERY, timeout)
        if len(done) > 1 and done[1] != SERVICE_DISCOVERY_DONE:
            return f"{outcome}_done_command_{done[1]}"
        return outcome

    async def _model(self) -> models.Model | None:
        return getattr(self, "_model_cache", None)

    # -- convenience -----------------------------------------------------

    async def read_basic_settings(self) -> codecs.BasicSetting:
        return codecs.decode_basic_setting(await self.request(0x13))

    async def read_alarms(self) -> tuple[bytes, bytes]:
        first = await self.request(0x15)
        rest = await self.request(0x16)
        return first, rest

    async def read_program_timer(self) -> codecs.ProgramTimer:
        return codecs.decode_program_timer(await self.request(0x2A))

    async def write_time(self, packet: bytes) -> None:
        await self.write_all(packet)

    async def send_notification(self, **kwargs) -> bytes:
        packet = notify.encode_notification(**kwargs)
        await self.transport.write_notification(packet)
        return packet

    async def close(self) -> None:
        await self.transport.close()


def load_app_id(path: str) -> bytes:
    """Read a persisted app id or create one. Keeps the watch's view stable."""
    try:
        with open(path, "rb") as handle:
            data = handle.read()
        if len(data) == 10:
            return data
    except FileNotFoundError:
        pass
    app_id = new_app_id()
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(app_id)
    return app_id

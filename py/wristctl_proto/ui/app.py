"""GTK4 desktop app for wristctl.

The UI never touches the BLE stack directly: every coroutine runs on the
AsyncRunner thread and results come back through GLib.idle_add.

    python3 -m wristctl_proto.ui              # real watch
    python3 -m wristctl_proto.ui --demo       # recorded session, no hardware
    python3 -m wristctl_proto.ui --demo --screenshot /tmp/wristctl.png
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from datetime import datetime
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import GLib, Gtk  # noqa: E402

from .. import notify, session, timeutil  # noqa: E402
from ..generated import codecs, models  # noqa: E402
from .bridge import AsyncRunner  # noqa: E402
from .demo import DemoTransport  # noqa: E402

APP_ID = "io.github.wristctl.Wristctl"
CONFIG = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "wristctl" / "app-id"


def section(title: str) -> tuple[Gtk.Box, Gtk.Box]:
    outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
    outer.set_margin_top(10)
    outer.set_margin_bottom(10)
    outer.set_margin_start(14)
    outer.set_margin_end(14)
    label = Gtk.Label()
    label.set_markup(f"<b>{GLib.markup_escape_text(title)}</b>")
    label.set_xalign(0.0)
    outer.append(label)
    body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
    outer.append(body)
    return outer, body


def labelled(text: str, widget: Gtk.Widget) -> Gtk.Box:
    box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
    label = Gtk.Label(label=text)
    label.set_xalign(0.0)
    label.set_size_request(120, -1)
    box.append(label)
    box.append(widget)
    return box


class MainWindow(Gtk.ApplicationWindow):
    def __init__(self, application: Gtk.Application, *, demo: bool) -> None:
        super().__init__(application=application)
        self.demo = demo
        self.runner = AsyncRunner()
        self.session: session.WatchSession | None = None
        self.transport = None
        self.identity: session.Identity | None = None
        self.basic: codecs.BasicSetting | None = None

        self.set_title("wristctl")
        self.set_default_size(780, 640)

        header = Gtk.HeaderBar()
        self.connect_btn = Gtk.Button(label="Connect")
        self.connect_btn.connect("clicked", self.on_connect)
        self.disconnect_btn = Gtk.Button(label="Disconnect")
        self.disconnect_btn.connect("clicked", self.on_disconnect)
        self.disconnect_btn.set_sensitive(False)
        self.refresh_btn = Gtk.Button(label="Refresh")
        self.refresh_btn.connect("clicked", self.on_refresh)
        self.refresh_btn.set_sensitive(False)
        header.pack_start(self.connect_btn)
        header.pack_start(self.disconnect_btn)
        header.pack_end(self.refresh_btn)
        self.set_titlebar(header)

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.stack = Gtk.Stack()
        self.stack.set_vexpand(True)
        switcher = Gtk.StackSwitcher()
        switcher.set_stack(self.stack)
        switcher.set_margin_top(6)
        root.append(switcher)
        root.append(self.stack)

        self.stack.add_titled(self._watch_page(), "watch", "Watch")
        self.stack.add_titled(self._clock_page(), "clock", "Clock")
        self.stack.add_titled(self._alarm_page(), "alarms", "Alarms")
        self.stack.add_titled(self._timer_page(), "timer", "Timer")
        self.stack.add_titled(self._notify_page(), "notify", "Notify")
        self.stack.add_titled(self._console_page(), "console", "Console")

        self.status = Gtk.Label(label="not connected")
        self.status.set_xalign(0.0)
        self.status.set_selectable(True)
        self.status.set_margin_top(6)
        self.status.set_margin_bottom(6)
        self.status.set_margin_start(14)
        self.status.set_margin_end(14)
        root.append(self.status)
        self.set_child(root)

        self._tick()
        GLib.timeout_add_seconds(1, self._tick)
        self.connect("close-request", self._on_close)
        if demo:
            GLib.timeout_add(250, lambda: (self.on_connect(None), False)[1])

    # -- pages -----------------------------------------------------------

    def _watch_page(self) -> Gtk.Widget:
        outer, body = section("Connection")
        self.lbl_name = Gtk.Label(label="—")
        self.lbl_model = Gtk.Label(label="—")
        self.lbl_address = Gtk.Label(label="—")
        self.lbl_bond = Gtk.Label(label="—")
        self.lbl_pairing = Gtk.Label(label="—")
        self.lbl_condition = Gtk.Label(label="—")
        for label in (
            self.lbl_name,
            self.lbl_model,
            self.lbl_address,
            self.lbl_bond,
            self.lbl_pairing,
            self.lbl_condition,
        ):
            label.set_xalign(0.0)
            label.set_selectable(True)
        body.append(labelled("Name", self.lbl_name))
        body.append(labelled("Model", self.lbl_model))
        body.append(labelled("Address", self.lbl_address))
        body.append(labelled("Bond", self.lbl_bond))
        body.append(labelled("Pairing", self.lbl_pairing))
        body.append(labelled("Condition", self.lbl_condition))
        note = Gtk.Label(
            label=(
                "Pairing follows the watch: after the settings pass, the watch sends "
                "0x47 02. Models that request a bond are bonded; others get a Link Loss "
                "Alert write. The watch then sends 0x47 01."
            )
        )
        note.set_wrap(True)
        note.set_xalign(0.0)
        note.add_css_class("dim-label")
        body.append(note)

        outer2, body2 = section("Model capability")
        self.lbl_caps = Gtk.Label(label="connect to see the model")
        self.lbl_caps.set_wrap(True)
        self.lbl_caps.set_xalign(0.0)
        self.lbl_caps.set_selectable(True)
        body2.append(self.lbl_caps)

        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        page.append(outer)
        page.append(outer2)
        return page

    def _clock_page(self) -> Gtk.Widget:
        outer, body = section("Watch clock")
        self.clock_label = Gtk.Label(label="—")
        self.clock_label.add_css_class("title-2")
        self.clock_label.set_xalign(0.0)
        body.append(self.clock_label)

        self.tz_entry = Gtk.Entry()
        self.tz_entry.set_text(timeutil.local_zone_name())
        self.tz_entry.set_hexpand(True)
        local_btn = Gtk.Button(label="Local")
        local_btn.connect("clicked", lambda _b: self.tz_entry.set_text(timeutil.local_zone_name()))
        tz_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        tz_row.append(self.tz_entry)
        tz_row.append(local_btn)
        body.append(labelled("Timezone", tz_row))

        set_btn = Gtk.Button(label="Set watch time")
        set_btn.connect("clicked", self.on_set_time)
        body.append(set_btn)

        self.switch_24h = Gtk.Switch()
        self.switch_ddmm = Gtk.Switch()
        body.append(labelled("24-hour clock", self.switch_24h))
        body.append(labelled("Date DD:MM", self.switch_ddmm))
        self.basic_label = Gtk.Label(label="settings not read yet")
        self.basic_label.set_xalign(0.0)
        self.basic_label.set_selectable(True)
        body.append(self.basic_label)
        apply_btn = Gtk.Button(label="Apply format settings")
        apply_btn.connect("clicked", self.on_apply_basic)
        body.append(apply_btn)

        note = Gtk.Label(
            label=(
                "Only the local clock fields are written. Home city, DST rule and the "
                "watch's auto-DST behaviour are not changed here; that needs the 0x21 "
                "DST transaction, which is not implemented yet."
            )
        )
        note.set_wrap(True)
        note.set_xalign(0.0)
        note.add_css_class("dim-label")
        body.append(note)
        return outer

    def _alarm_page(self) -> Gtk.Widget:
        outer, body = section("Alarms")
        grid = Gtk.Grid()
        grid.set_column_spacing(10)
        grid.set_row_spacing(6)
        for column, title in enumerate(("", "On", "Hour", "Minute")):
            label = Gtk.Label()
            label.set_markup(f"<b>{title}</b>" if title else "")
            label.set_xalign(0.0 if column == 0 else 0.5)
            grid.attach(label, column, 0, 1, 1)
        self.alarm_rows: list[tuple[Gtk.Switch, Gtk.SpinButton, Gtk.SpinButton]] = []
        for index in range(5):
            switch = Gtk.Switch()
            hour = Gtk.SpinButton.new_with_range(0, 23, 1)
            minute = Gtk.SpinButton.new_with_range(0, 59, 1)
            grid.attach(Gtk.Label(label=f"Alarm {index}"), 0, index + 1, 1, 1)
            grid.attach(switch, 1, index + 1, 1, 1)
            grid.attach(hour, 2, index + 1, 1, 1)
            grid.attach(minute, 3, index + 1, 1, 1)
            self.alarm_rows.append((switch, hour, minute))
        body.append(grid)
        apply_btn = Gtk.Button(label="Apply alarms")
        apply_btn.connect("clicked", self.on_apply_alarms)
        body.append(apply_btn)
        note = Gtk.Label(
            label="The watch exposes one alarm plus four more; the hourly-chime bit is not exposed yet."
        )
        note.set_wrap(True)
        note.set_xalign(0.0)
        note.add_css_class("dim-label")
        body.append(note)
        return outer

    def _timer_page(self) -> Gtk.Widget:
        outer, body = section("Program timer")
        self.timer_min = Gtk.SpinButton.new_with_range(0, 99, 1)
        self.timer_sec = Gtk.SpinButton.new_with_range(0, 59, 1)
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        row.append(Gtk.Label(label="Minutes"))
        row.append(self.timer_min)
        row.append(Gtk.Label(label="Seconds"))
        row.append(self.timer_sec)
        body.append(row)
        apply_btn = Gtk.Button(label="Replace program timer")
        apply_btn.connect("clicked", self.on_apply_timer)
        body.append(apply_btn)
        warning = Gtk.Label(
            label="This replaces all six program-timer entries with one entry and repeat 0."
        )
        warning.set_wrap(True)
        warning.set_xalign(0.0)
        warning.add_css_class("dim-label")
        body.append(warning)
        self.timer_info = Gtk.Label(label="timer not read yet")
        self.timer_info.set_xalign(0.0)
        self.timer_info.set_selectable(True)
        body.append(self.timer_info)

        outer2, body2 = section("Stopwatch")
        self.stopwatch_label = Gtk.Label(
            label="Connect to see whether this model exposes stopwatch control."
        )
        self.stopwatch_label.set_wrap(True)
        self.stopwatch_label.set_xalign(0.0)
        body2.append(self.stopwatch_label)

        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        page.append(outer)
        page.append(outer2)
        return page

    def _notify_page(self) -> Gtk.Widget:
        outer, body = section("Send a notification")
        self.notify_app = Gtk.Entry()
        self.notify_app.set_text("wristctl")
        self.notify_title = Gtk.Entry()
        self.notify_title.set_text("Hello")
        self.notify_text = Gtk.Entry()
        self.notify_text.set_text("Sent from the desktop")
        body.append(labelled("App", self.notify_app))
        body.append(labelled("Title", self.notify_title))
        body.append(labelled("Text", self.notify_text))
        send_btn = Gtk.Button(label="Send")
        send_btn.connect("clicked", self.on_send_notification)
        body.append(send_btn)
        return outer

    def _console_page(self) -> Gtk.Widget:
        outer, body = section("Raw frames")
        self.console_entry = Gtk.Entry()
        self.console_entry.set_placeholder_text("hex, e.g. 23 or 134400000000000000000002")
        body.append(self.console_entry)
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        for label, handler in (
            ("Request (0x2C)", self.on_console_request),
            ("Write (0x2D)", self.on_console_write),
            ("Notify (0x30)", self.on_console_notify),
        ):
            button = Gtk.Button(label=label)
            button.connect("clicked", handler)
            row.append(button)
        body.append(row)
        self.console_view = Gtk.TextView()
        self.console_view.set_editable(False)
        self.console_view.set_monospace(True)
        scroller = Gtk.ScrolledWindow()
        scroller.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        scroller.set_child(self.console_view)
        scroller.set_vexpand(True)
        scroller.set_min_content_height(220)
        body.append(scroller)
        return outer

    # -- helpers ---------------------------------------------------------

    def set_status(self, text: str) -> None:
        self.status.set_text(text)

    def log(self, text: str) -> None:
        buffer = self.console_view.get_buffer()
        end = buffer.get_end_iter()
        buffer.insert(end, text.rstrip() + "\n")

    def _run(self, coro, on_ok, what: str) -> None:
        future = self.runner.submit(coro)
        future.add_done_callback(lambda f: GLib.idle_add(self._finished, f, on_ok, what))

    def _finished(self, future, on_ok, what: str) -> bool:
        try:
            result = future.result()
        except Exception as exc:  # noqa: BLE001 - surfaced in the status bar
            self.set_status(f"{what}: {exc}")
            self.connect_btn.set_sensitive(self.session is None)
            return False
        try:
            on_ok(result)
        except Exception as exc:  # noqa: BLE001
            self.set_status(f"{what}: {exc}")
        return False

    def _tick(self) -> bool:
        try:
            now = datetime.now(timeutil.zone(self.tz_entry.get_text()))
            self.clock_label.set_text(now.strftime("%Y-%m-%d   %H:%M:%S   %Z"))
        except Exception:  # noqa: BLE001 - entry may be half-typed
            self.clock_label.set_text("invalid timezone")
        return True

    def _on_close(self, *_args) -> bool:
        self.runner.stop()
        return False

    # -- connection ------------------------------------------------------

    def on_connect(self, _button) -> None:
        if self.session is not None:
            return
        self.connect_btn.set_sensitive(False)
        self.set_status("scanning for a CASIO watch…")
        self._run(self._connect(), self._connected, "connect")

    async def _connect(self):
        bonded = None
        if self.demo:
            transport = DemoTransport()
            await transport.connect()
        else:
            from ..ble import BleakTransport, BluezPairer, bluez_bonded

            transport = BleakTransport(name="CASIO", timeout=8)
            device = None
            deadline = time.monotonic() + 90
            while device is None:
                found = await BleakTransport.scan(4.0, name="CASIO")
                if found:
                    device = found[0]
                    break
                if time.monotonic() >= deadline:
                    raise session.SessionError(
                        "watch is not advertising. Press a watch button and connect again."
                    )
                GLib.idle_add(self.set_status, "waiting for the watch — press a button")
            await transport.connect(device)
            bonded = bluez_bonded(device.address)
        watch = session.WatchSession(
            transport, app_id=session.load_app_id(str(CONFIG)), timeout=8
        )
        pairer = None
        if not self.demo:
            from ..ble import BluezPairer

            pairer = BluezPairer()
        identity = await watch.handshake(pairer, service_discovery_timeout=25)
        return watch, transport, identity, bonded

    def _connected(self, result) -> None:
        self.session, self.transport, self.identity, bonded = result
        identity = self.identity
        self.lbl_name.set_text(identity.name or "—")
        if identity.model:
            self.lbl_model.set_text(
                f"{identity.model.name}  (generation {identity.model.generation})"
            )
        else:
            self.lbl_model.set_text("unknown model — generic mode")
        self.lbl_address.set_text(identity.address or "—")
        self.lbl_pairing.set_text(identity.pairing)
        if self.demo:
            self.lbl_bond.set_text("n/a (demo)")
        elif bonded is None:
            self.lbl_bond.set_text("unknown")
        else:
            self.lbl_bond.set_text("yes" if bonded else "no")
        if identity.condition:
            condition = identity.condition
            self.lbl_condition.set_text(
                f"battery={condition.battery}  temperature={condition.temperature}  "
                f"flash={condition.flash_status}  mtu={condition.mtu}  "
                f"log={condition.log_status}"
            )
        else:
            self.lbl_condition.set_text(identity.condition_raw.hex())
        self.disconnect_btn.set_sensitive(True)
        self.refresh_btn.set_sensitive(True)
        self.connect_btn.set_sensitive(False)
        self._update_capabilities(identity.model)
        self.set_status(f"connected to {identity.name} ({identity.pairing})")
        self.on_refresh(None)

    def _update_capabilities(self, model: models.Model | None) -> None:
        if model is None:
            self.lbl_caps.set_text("Unknown model: only generic reads and the raw console are safe.")
            self.stopwatch_label.set_text("Stopwatch control is unknown for this model.")
            return
        wanted = [
            "isEnableAlmSetting",
            "isEnableTmrSetting",
            "isEnableProgramTmrSetting",
            "isEnableStwTargetTimeSetting",
            "isEnableNotificationManager",
            "isUseAllFeatures",
            "isUseReadAndWriteTideData",
            "isPartTimeLink",
            "isNeedCreateBond",
        ]
        lines = []
        for name in wanted:
            lines.append(f"{name} = {models.flag(model, name)}")
        self.lbl_caps.set_text("\n".join(lines))
        if models.flag(model, "isEnableStwTargetTimeSetting"):
            self.stopwatch_label.set_text(
                "This model advertises stopwatch target settings (0x29/0x2C), but the payload "
                "layout is not implemented yet."
            )
        else:
            self.stopwatch_label.set_text(
                "This generation has no remote stopwatch control: the stopwatch stays on the "
                "watch. Use the program timer for timed intervals."
            )

    def on_disconnect(self, _button) -> None:
        if self.session is None:
            return
        watch = self.session
        self.session = None
        self.disconnect_btn.set_sensitive(False)
        self.refresh_btn.set_sensitive(False)
        self.connect_btn.set_sensitive(True)
        self._run(watch.close(), lambda _r: self.set_status("disconnected"), "disconnect")

    # -- reads -----------------------------------------------------------

    def on_refresh(self, _button) -> None:
        if self.session is None:
            return
        self._run(self._read_all(), self._refreshed, "refresh")

    async def _read_all(self):
        assert self.session is not None
        basic = await self.session.read_basic_settings()
        first = rest = None
        try:
            first, rest = await self.session.read_alarms()
        except session.SessionError:
            pass
        timer = None
        try:
            timer = await self.session.read_program_timer()
        except session.SessionError:
            pass
        return basic, first, rest, timer

    def _refreshed(self, result) -> None:
        basic, first, rest, timer = result
        self.basic = basic
        self.switch_24h.set_active(basic.time_format_24h)
        self.switch_ddmm.set_active(bool(basic.date_format_ddmm))
        self.basic_label.set_text(
            f"flags=0x{basic.flags:02x}  light={basic.light_duration}  "
            f"language={basic.language}  reserved={basic.reserved_tail.hex()}"
        )
        if first is not None and rest is not None:
            alarms = [codecs.decode_alarm_first(first)] + list(
                codecs.decode_alarm_rest(rest).alarms
            )
            for index, (switch, hour, minute) in enumerate(self.alarm_rows):
                if index >= len(alarms):
                    break
                alarm = alarms[index]
                switch.set_active(bool(alarm.enabled))
                hour.set_value(alarm.hour)
                minute.set_value(alarm.minute)
        if timer is None:
            self.timer_info.set_text("program timer not readable on this model")
        else:
            self.timer_info.set_text(
                "repeat="
                + str(timer.repeat)
                + "  "
                + "  ".join(
                    f"{w.minutes:02d}:{w.seconds:02d}"
                    for w in timer.workouts
                    if w.minutes or w.seconds
                )
            )
        self.set_status("state refreshed")

    # -- writes ----------------------------------------------------------

    def on_set_time(self, _button) -> None:
        if self.session is None:
            return
        try:
            dt = datetime.now(timeutil.zone(self.tz_entry.get_text()))
        except Exception as exc:  # noqa: BLE001
            self.set_status(f"timezone: {exc}")
            return
        packet = codecs.encode_current_time(timeutil.to_current_time(dt))
        self._run(
            self.session.write_time(packet),
            lambda _r: self.set_status(f"time set to {dt:%Y-%m-%d %H:%M:%S} ({dt.tzname()})"),
            "set time",
        )

    def on_apply_basic(self, _button) -> None:
        if self.session is None or self.basic is None:
            self.set_status("read the watch settings first")
            return
        self.basic.time_format_24h = self.switch_24h.get_active()
        self.basic.date_format_ddmm = 1 if self.switch_ddmm.get_active() else 0
        packet = codecs.encode_basic_setting(self.basic)
        self._run(
            self.session.write_all(packet),
            lambda _r: self.set_status(f"settings written: {packet.hex()}"),
            "apply settings",
        )

    def on_apply_alarms(self, _button) -> None:
        if self.session is None:
            return
        first = None
        rest = []
        for index, (switch, hour, minute) in enumerate(self.alarm_rows):
            enabled = switch.get_active()
            if index == 0:
                first = codecs.AlarmFirst(
                    flag=0,
                    enabled=enabled,
                    one_time=False,
                    hourly_chime=False,
                    hour=hour.get_value_as_int(),
                    minute=minute.get_value_as_int(),
                )
            else:
                rest.append(
                    codecs.AlarmRestAlarmsItem(
                        flag=0,
                        enabled=enabled,
                        one_time=False,
                        hourly_chime=False,
                        hour=hour.get_value_as_int(),
                        minute=minute.get_value_as_int(),
                    )
                )
        assert first is not None
        packets = [
            codecs.encode_alarm_first(first),
            codecs.encode_alarm_rest(codecs.AlarmRest(alarms=rest)),
        ]
        self._run(self._write_frames(packets), lambda _r: self.set_status("alarms written"), "apply alarms")

    async def _write_frames(self, packets: list[bytes]) -> None:
        assert self.session is not None
        for packet in packets:
            await self.session.write_all(packet)

    def on_apply_timer(self, _button) -> None:
        if self.session is None:
            return
        entry = codecs.ProgramTimerWorkoutsItem(
            minutes=self.timer_min.get_value_as_int(),
            seconds=self.timer_sec.get_value_as_int(),
            beep=0,
        )
        value = codecs.ProgramTimer(
            repeat=0,
            workouts=[entry] + [codecs.ProgramTimerWorkoutsItem(0, 0, 0)] * 5,
        )
        packet = codecs.encode_program_timer(value)
        self._run(
            self.session.write_all(packet),
            lambda _r: (self.set_status(f"timer replaced: {packet.hex()}"), self.on_refresh(None)),
            "apply timer",
        )

    def on_send_notification(self, _button) -> None:
        if self.session is None:
            return
        packet = notify.encode_notification(
            app=self.notify_app.get_text(),
            title=self.notify_title.get_text(),
            text=self.notify_text.get_text(),
        )
        self._run(
            self.session.send_notification(
                app=self.notify_app.get_text(),
                title=self.notify_title.get_text(),
                text=self.notify_text.get_text(),
            ),
            lambda sent: self.set_status(f"notification sent ({len(sent)} bytes)"),
            "notify",
        )

    # -- console ---------------------------------------------------------

    def _hex_entry(self) -> bytes | None:
        raw = self.console_entry.get_text().replace("0x", "").replace(" ", "").strip()
        try:
            return bytes.fromhex(raw)
        except ValueError as exc:
            self.set_status(f"hex: {exc}")
            return None

    def on_console_request(self, _button) -> None:
        if self.session is None:
            return
        payload = self._hex_entry()
        if not payload:
            return
        self._run(self.session.request_bytes(payload), self._console_result, "request")

    def on_console_write(self, _button) -> None:
        if self.session is None:
            return
        payload = self._hex_entry()
        if not payload:
            return
        self._run(self.session.write_all(payload), lambda _r: self.set_status(f"wrote {payload.hex()}"), "write")

    def on_console_notify(self, _button) -> None:
        if self.session is None:
            return
        payload = self._hex_entry()
        if not payload:
            return
        self._run(
            self.transport.write_notification(payload),
            lambda _r: self.set_status(f"notification {payload.hex()}"),
            "notify",
        )

    def _console_result(self, frame: bytes) -> None:
        line = f"<- {frame.hex()}"
        try:
            decoded = codecs.decode_frame(frame)
            line += f"   {decoded}"
        except codecs.ProtoError:
            pass
        self.log(line)
        self.set_status(f"{len(frame)} bytes")


class App(Gtk.Application):
    def __init__(self, *, demo: bool, screenshot: str | None, page: str | None = None) -> None:
        super().__init__(application_id=APP_ID)
        self.demo = demo
        self.screenshot = screenshot
        self.page = page
        self.window: MainWindow | None = None
        self._shots = 0

    def do_activate(self) -> None:  # noqa: N802 - GTK override
        if self.window is None:
            self.window = MainWindow(self, demo=self.demo)
            if self.page:
                self.window.stack.set_visible_child_name(self.page)
            self.window.present()
            if self.screenshot:
                GLib.timeout_add(400, self._grab)

    def _grab(self) -> bool:
        assert self.window is not None
        self._shots += 1
        paintable = Gtk.WidgetPaintable.new(self.window)
        snapshot = Gtk.Snapshot.new()
        paintable.snapshot(snapshot, self.window.get_width(), self.window.get_height())
        node = snapshot.to_node()
        renderer = self.window.get_renderer()
        if node is not None and renderer is not None:
            texture = renderer.render_texture(node)
            texture.save_to_png(self.screenshot)
            print(f"screenshot: {self.screenshot}")
            self.window.close()
            return False
        if self._shots > 12:
            print("screenshot: window never produced a frame", file=sys.stderr)
            self.window.close()
            return False
        GLib.timeout_add(200, self._grab)
        return False


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="wristctl GTK app")
    parser.add_argument("--demo", action="store_true", help="use the recorded session instead of a watch")
    parser.add_argument("--screenshot", default=None, help="save the window and quit")
    parser.add_argument("--page", default=None, help="stack page to show: watch, clock, alarms, timer, notify, console")
    args = parser.parse_args(argv)
    app = App(demo=args.demo, screenshot=args.screenshot, page=args.page)
    return app.run([])


if __name__ == "__main__":
    raise SystemExit(main())

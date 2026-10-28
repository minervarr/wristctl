#!/usr/bin/env python3
"""Connect to a watch, run the handshake, and report what happened.

Read-only apart from the app-info write and, when the watch asks for it, a bond
or Link Loss Alert write. This is the hardware gate for P1: it answers whether
`0x47 02` requires a bond on a watch that is not paired to anything.

    python3 tools/probe.py --address DA:02:44:21:46:51
    python3 tools/probe.py --scan
    python3 tools/probe.py --address ... --json

Requires bleak in the interpreter (the gshock_ctl venv has it, or install the
'ble' extra).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))

from wristctl_proto import session  # noqa: E402
from wristctl_proto.ble import BleakTransport, BluezPairer, bluez_bonded  # noqa: E402

DEFAULT_APP_ID = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "wristctl" / "app-id"


def describe(identity: session.Identity) -> dict:
    return {
        "address": identity.address,
        "name": identity.name,
        "model": identity.model.name if identity.model else None,
        "generation": identity.model.generation if identity.model else None,
        "pairing": identity.pairing,
        "bonded_before": None,
        "ble_features": identity.ble_features.hex(),
        "ble_settings": identity.ble_settings.hex(),
        "advertise_parameters": identity.advertise_parameters.hex(),
        "connection_parameters": identity.connection_parameters.hex(),
        "module": identity.module.hex(),
        "version": identity.version.hex(),
        "condition": (
            {
                "battery": identity.condition.battery,
                "temperature": identity.condition.temperature,
                "flash_status": identity.condition.flash_status,
                "mtu": identity.condition.mtu,
                "log_status": identity.condition.log_status,
                "modified_bt": identity.condition.modified_bt,
                "modified_ht_city_dst": identity.condition.modified_ht_city_dst,
                "status_not_permitted": identity.condition.status_not_permitted,
            }
            if identity.condition
            else None
        ),
    }


async def run(args: argparse.Namespace) -> int:
    if args.scan:
        devices = await BleakTransport.scan(args.scan_timeout, name=args.name)
        if not devices:
            print("no CASIO watch advertising")
            return 1
        for device in devices:
            print(f"{device.address}  {device.name}")
        return 0

    app_id = session.load_app_id(str(args.app_id_file))
    device = None
    if args.wait > 0:
        deadline = time.monotonic() + args.wait
        while device is None:
            found = await BleakTransport.scan(4.0, name=args.name, address=args.address or None)
            if found:
                device = found[0]
            elif time.monotonic() >= deadline:
                print("no CASIO watch advertising before --wait expired", file=sys.stderr)
                return 1
    transport = BleakTransport(
        args.address or (device.address if device else ""),
        name=args.name,
        timeout=args.timeout,
        debug=args.debug,
    )
    pairer = None if args.no_pair else BluezPairer()
    watch = session.WatchSession(transport, app_id=app_id, timeout=args.timeout)

    bonded_before = bluez_bonded(args.address) if args.address else False
    try:
        await transport.connect(device)
        identity = await watch.handshake(
            pairer,
            service_discovery_timeout=args.pairing_timeout,
        )
    finally:
        await watch.close()

    payload = describe(identity)
    payload["bonded_before"] = bonded_before
    payload["bonded_after"] = bluez_bonded(identity.address or args.address or "")
    if args.json:
        print(json.dumps(payload, indent=2))
    else:
        print(f"address:   {payload['address']}")
        print(f"name:      {payload['name']}")
        print(f"model:     {payload['model']} (generation {payload['generation']})")
        print(f"pairing:   {payload['pairing']}")
        print(f"bonded:    before={payload['bonded_before']} after={payload['bonded_after']}")
        if payload["condition"]:
            condition = payload["condition"]
            print(
                f"condition: battery={condition['battery']} temp={condition['temperature']} "
                f"flash={condition['flash_status']} mtu={condition['mtu']} "
                f"log={condition['log_status']}"
            )
        print(f"module:    {identity.module.hex()}")
        print(f"version:   {identity.version.hex()}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", default="", help="watch BLE address")
    parser.add_argument("--name", default=None, help="name substring to scan for")
    parser.add_argument("--scan", action="store_true", help="list advertising CASIO watches")
    parser.add_argument("--scan-timeout", type=float, default=12.0)
    parser.add_argument("--wait", type=float, default=0.0, help="keep scanning for this many seconds")
    parser.add_argument("--timeout", type=float, default=10.0, help="per-frame timeout")
    parser.add_argument("--pairing-timeout", type=float, default=25.0)
    parser.add_argument("--no-pair", action="store_true", help="observe without bonding")
    parser.add_argument("--app-id-file", type=Path, default=DEFAULT_APP_ID)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        return asyncio.run(run(args))
    except session.SessionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())

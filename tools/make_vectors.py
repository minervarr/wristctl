#!/usr/bin/env python3
"""Author the golden vector corpus.

Vectors are written by hand (byte lists and expected decoded values), then
checked against the class lengths in ``spec/classes.json``. The codecs are
never used to produce a vector, so a bug cannot make a vector agree with
itself.

Run from the repository root:

    python3 tools/make_vectors.py
"""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / "libs" / "firstparty" / "wristctl_core"
VECTOR_DIR = CORE / "spec" / "vectors"
CLASSES = CORE / "spec" / "classes.json"


def b(*values: int) -> bytes:
    return bytes(values)


def pad(prefix: list[int], total: int) -> bytes:
    assert len(prefix) <= total, (len(prefix), total)
    return bytes(prefix + [0] * (total - len(prefix)))


def name_frame(name: str, total: int = 20) -> bytes:
    raw = name.encode("ascii")
    return bytes([0x23]) + raw + bytes(total - 1 - len(raw))


VECTORS = [
    {
        "id": "time-live-capture",
        "class": "current_time",
        "source": "observed: work/evidence/att_frames_current.txt:63 (2026-10-27T16:41:09.765625)",
        "bytes": bytes.fromhex("09ea070a1b10290902c401"),
        "decoded": {
            "year": 2026, "month": 10, "day": 27, "hour": 16, "minute": 41,
            "second": 9, "weekday": 2, "nano": 196,
        },
    },
    {
        "id": "time-sync-target",
        "class": "current_time",
        "source": "observed: work/MANIFEST.md NTP sync target 2026-10-27T18:02:01-05:00",
        "bytes": bytes.fromhex("09ea070a1b120201020001"),
        "decoded": {
            "year": 2026, "month": 10, "day": 27, "hour": 18, "minute": 2,
            "second": 1, "weekday": 2, "nano": 0,
        },
    },
    {
        "id": "basic-setting-capture",
        "class": "basic_setting",
        "source": "observed: work/evidence/att_frames_current.txt:69",
        "bytes": bytes.fromhex("134400000000000000000002"),
        "decoded": {
            "flags": 0x44,
            "time_format_24h": False,
            "button_tone_off": False,
            "auto_light_off": True,
            "power_saving_off": False,
            "light_duration": 0,
            "reserved_3": 0,
            "date_format_ddmm": 0,
            "language": 0,
            "reserved_tail": "000000000002",
        },
    },
    {
        "id": "alarm-first-enabled",
        "class": "alarm_first",
        "source": "derived: layout from RemoteCasioWatchFeaturesService + gshock_api; 07:30 enabled",
        "bytes": b(0x15, 0x40, 0x40, 0x07, 0x1E),
        "decoded": {
            "flag": 0x40, "enabled": True, "one_time": False,
            "hourly_chime": False, "hour": 7, "minute": 30,
        },
    },
    {
        "id": "alarm-rest-all-off",
        "class": "alarm_rest",
        "source": "derived: four disabled entries",
        "bytes": b(0x16) + b(0x00, 0x40, 0x00, 0x00) * 4,
        "decoded": {
            "alarms": [
                {"flag": 0x00, "enabled": False, "one_time": False, "hourly_chime": False, "hour": 0, "minute": 0}
                for _ in range(4)
            ]
        },
    },
    {
        "id": "alarm-rest-one-enabled",
        "class": "alarm_rest",
        "source": "derived: entry 1 enabled at 06:15, others off",
        "bytes": b(0x16)
        + b(0x00, 0x40, 0x00, 0x00)
        + b(0x40, 0x40, 0x06, 0x0F)
        + b(0x00, 0x40, 0x00, 0x00) * 2,
        "decoded": {
            "alarms": [
                {"flag": 0x00, "enabled": False, "one_time": False, "hourly_chime": False, "hour": 0, "minute": 0},
                {"flag": 0x40, "enabled": True, "one_time": False, "hourly_chime": False, "hour": 6, "minute": 15},
                {"flag": 0x00, "enabled": False, "one_time": False, "hourly_chime": False, "hour": 0, "minute": 0},
                {"flag": 0x00, "enabled": False, "one_time": False, "hourly_chime": False, "hour": 0, "minute": 0},
            ]
        },
    },
    {
        "id": "watch-name-capture",
        "class": "watch_name",
        "source": "observed: work/evidence/att_frames_current.txt:3",
        "bytes": name_frame("CASIO GBX-100"),
        "decoded": {"name": "CASIO GBX-100"},
    },
    {
        "id": "watch-condition-capture",
        "class": "watch_condition",
        "source": "observed: work/evidence/att_frames_current.txt:20",
        "bytes": bytes.fromhex("28061e2300ba010001"),
        "decoded": {
            "battery": 6, "temperature": 30, "flash_status": 0x23,
            "reserved_4": 0, "mtu": 442, "log_status": 0,
            "flags": 0x01, "modified_bt": True,
            "modified_ht_city_dst": False, "status_not_permitted": False,
        },
    },
    {
        "id": "program-timer-original",
        "class": "program_timer",
        "source": "observed: work/MANIFEST.md restored watch state, repeat=1, 00:25 and 00:05",
        "bytes": b(0x2A, 0x01) + b(0x00, 0x25, 0x00) + b(0x00, 0x05, 0x00) + b(0, 0, 0) * 4,
        "decoded": {
            "repeat": 1,
            "workouts": [
                {"minutes": 0, "seconds": 25, "beep": 0},
                {"minutes": 0, "seconds": 5, "beep": 0},
                {"minutes": 0, "seconds": 0, "beep": 0},
                {"minutes": 0, "seconds": 0, "beep": 0},
                {"minutes": 0, "seconds": 0, "beep": 0},
                {"minutes": 0, "seconds": 0, "beep": 0},
            ],
        },
    },
    {
        "id": "program-timer-single",
        "class": "program_timer",
        "source": "derived: single 05:00 entry with repeat=0, the shape gshockctl timer-set writes",
        "bytes": b(0x2A, 0x00) + b(0x05, 0x00, 0x00) + b(0, 0, 0) * 5,
        "decoded": {
            "repeat": 0,
            "workouts": [
                {"minutes": 5, "seconds": 0, "beep": 0},
                {"minutes": 0, "seconds": 0, "beep": 0},
                {"minutes": 0, "seconds": 0, "beep": 0},
                {"minutes": 0, "seconds": 0, "beep": 0},
                {"minutes": 0, "seconds": 0, "beep": 0},
                {"minutes": 0, "seconds": 0, "beep": 0},
            ],
        },
    },
    {
        "id": "dst-watch-state-capture",
        "class": "dst_watch_state",
        "source": "observed: work/evidence/att_frames_current.txt:38",
        "bytes": bytes.fromhex("1d00010202b5760000ffffffffffff"),
        "decoded": {
            "watch_no_home": 0, "watch_no_world": 1,
            "state_home": 0x02, "dst_on_home": False, "dst_auto_home": True, "dst_auto_rep_home": False,
            "state_world": 0x02, "dst_on_world": False, "dst_auto_world": True, "dst_auto_rep_world": False,
            "city_home": 0x76B5, "city_world": 0, "city_extra_1": 0xFFFF,
            "city_extra_2": 0xFFFF, "city_extra_3": 0xFFFF,
        },
    },
    {
        "id": "dst-setting-ht",
        "class": "dst_setting",
        "source": "observed: work/evidence/att_frames_current.txt:44 (home city, UTC-5, DST +60)",
        "bytes": bytes.fromhex("1e00b576ec0400"),
        "decoded": {"unit": 0, "city": 0x76B5, "timezone": -20, "dst_diff": 4, "dst_rule": 0},
    },
    {
        "id": "dst-setting-wt",
        "class": "dst_setting",
        "source": "observed: work/evidence/att_frames_current.txt:45 (world-time unit cleared)",
        "bytes": bytes.fromhex("1e010000000000"),
        "decoded": {"unit": 1, "city": 0, "timezone": 0, "dst_diff": 0, "dst_rule": 0},
    },
    {
        "id": "city-name-read",
        "class": "city_name",
        "source": "observed: work/evidence/att_frames_current.txt:49",
        "bytes": bytes.fromhex("1f00544f4b594f00000000000000000000000000"),
        "decoded": {"unit": 0, "name": "TOKYO"},
    },
    {
        "id": "city-name-write",
        "class": "city_name",
        "source": "observed: work/evidence/att_frames_current.txt:52 (the app writes a GMT offset name)",
        "bytes": bytes.fromhex("1f00474d542b3500000000000000000000000000"),
        "decoded": {"unit": 0, "name": "GMT+5"},
    },
    {
        "id": "app-info-capture",
        "class": "app_info",
        "source": "observed: work/evidence/att_frames_current.txt:7 (random per-install ID, OS 2 = Android)",
        "bytes": bytes.fromhex("22d5d6d017797e2ca55f1802"),
        "decoded": {"app_id": "d5d6d017797e2ca55f18", "os": 2},
    },
    {
        "id": "service-discovery-start",
        "class": "service_discovery",
        "source": "observed: work/evidence/att_frames_current.txt:64 (the watch asks the app to start discovery)",
        "bytes": bytes.fromhex("4702"),
        "decoded": {"command": 2},
    },
    {
        "id": "service-discovery-done",
        "class": "service_discovery",
        "source": "observed: work/evidence/att_frames_current.txt:67",
        "bytes": bytes.fromhex("4701"),
        "decoded": {"command": 1},
    },
    {
        "id": "error-class-rejected",
        "class": "error",
        "source": "observed: work/evidence/att_frames_current.txt:75 (0x5A rejected as unsupported)",
        "bytes": bytes.fromhex("ff815a"),
        "decoded": {"status": 0x81, "class_id": 0x5A},
    },
    {
        "id": "error-time-read-rejected",
        "class": "error",
        "source": "observed: work/MANIFEST.md (0x09 read returns ff8109 on the GBX-100)",
        "bytes": bytes.fromhex("ff8109"),
        "decoded": {"status": 0x81, "class_id": 0x09},
    },
]


def main() -> int:
    spec = json.loads(CLASSES.read_text(encoding="utf-8"))
    lengths = {cls["name"]: cls["length"]["max"] for cls in spec["classes"]}
    VECTOR_DIR.mkdir(parents=True, exist_ok=True)
    for index, vector in enumerate(VECTORS, start=1):
        expected = lengths[vector["class"]]
        if len(vector["bytes"]) != expected:
            raise SystemExit(
                f"{vector['id']}: {len(vector['bytes'])} bytes, class {vector['class']} is {expected}"
            )
        payload = {
            "id": vector["id"],
            "class": vector["class"],
            "source": vector["source"],
            "bytes": vector["bytes"].hex(),
            "decoded": vector["decoded"],
        }
        path = VECTOR_DIR / f"{index:02d}-{vector['id']}.json"
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        print(f"{path.name}  {len(vector['bytes'])} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Decode the recorded ATT frames with the generated codecs and report coverage.

This is the honest way to see how much of the real handshake the spec explains.
Frames whose class has no codec are listed with their id so the next spec entry
is obvious.

    python3 tools/decode_capture.py [PATH]
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))

from wristctl_proto.generated import codecs  # noqa: E402

DECOMP = Path(os.environ.get("GSHOCK_DECOMP", ROOT.parent / "GSHOCK_DECOMP"))
DEFAULT = DECOMP / "work/evidence/att_frames_current.txt"
LINE_RE = re.compile(r"^(?P<kind>[A-Za-z ]+?)\t(?P<handle>0x[0-9a-f]{4})\t(?P<value>[0-9a-f]*)$")

ALL_FEATURES = 0x000E
READ_REQUEST = 0x000C
SKIP_HANDLES = {0x000F, 0x0012, 0x0015, 0x0011, 0x0014, 0x0030}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, nargs="?", default=DEFAULT)
    args = parser.parse_args(argv)

    known: Counter[str] = Counter()
    unknown: Counter[int] = Counter()
    skipped = 0
    requests = 0
    bad = 0

    for line in args.path.read_text(encoding="utf-8").splitlines():
        match = LINE_RE.match(line)
        if not match:
            continue
        handle = int(match.group("handle"), 16)
        value = bytes.fromhex(match.group("value"))
        if not value:
            continue
        if handle in SKIP_HANDLES:
            skipped += 1
            continue
        if handle == READ_REQUEST:
            requests += 1
            info = codecs.CLASS_INFO.get(value[0])
            label = info["name"] if info else "unknown"
            print(f"request  0x{value[0]:02x}  {label}")
            if info is None:
                unknown[value[0]] += 1
            continue
        if handle != ALL_FEATURES:
            continue
        try:
            decoded = codecs.decode_frame(value)
        except codecs.ProtoError as exc:
            unknown[value[0]] += 1
            bad += 1
            print(f"uncovered 0x{value[0]:02x}  {value.hex()}  ({exc})")
            continue
        known[type(decoded).__name__] += 1
        print(f"ok        {type(decoded).__name__:<20} {value.hex()}")

    total = sum(known.values()) + bad
    print()
    print(f"decoded frames: {total}")
    print(f"covered classes: {len(known)} ({sum(known.values())} frames)")
    for name, count in sorted(known.items()):
        print(f"  {name:<24} {count}")
    print(f"unknown class ids: {len(unknown)}")
    for cls_id, count in sorted(unknown.items()):
        print(f"  0x{cls_id:02x}  {count} frame(s)")
    print(f"skipped non-setting handles: {skipped}")
    print(f"read requests: {requests}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

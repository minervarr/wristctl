#!/usr/bin/env python3
"""Generate the Python codecs from spec/classes.json."""

from __future__ import annotations

import argparse
from pathlib import Path

import spec_lib

CORE = Path(__file__).resolve().parent.parent
WRISTCTL = CORE.parents[2]
DEFAULT_SPEC = CORE / "spec" / "classes.json"
DEFAULT_OUT = WRISTCTL / "py" / "wristctl_proto" / "generated" / "codecs.py"


class Emitter:
    def __init__(self) -> None:
        self.lines: list[str] = []

    def w(self, level: int, text: str = "") -> None:
        self.lines.append(("    " * level + text) if text else "")

    def text(self) -> str:
        return "\n".join(self.lines) + "\n"


def range_check(e: Emitter, path: str, var: str, field: dict, level: int) -> None:
    if "range" not in field:
        return
    lo, hi = field["range"]
    e.w(level, f"_check_range({path!r}, {var}, {lo}, {hi})")


def emit_constant_checks(
    e: Emitter, fields: list[dict], frame: str, base: int, level: int, ctor: str
) -> None:
    for field in fields:
        if "expected" not in field:
            continue
        offset = base + field["offset"]
        e.w(level, f"if {frame}[{offset}] != {field['expected']}:")
        e.w(
            level + 1,
            f"raise ProtoError(f\"{ctor}: byte {offset} is {{{frame}[{offset}]:#04x}}, "
            f"expected {field['expected']:#04x}\")",
        )


def emit_decode_fields(
    e: Emitter, fields: list[dict], frame: str, base: int, level: int, ctor: str
) -> None:
    """Emit constructor keyword arguments for one scope."""
    for field in fields:
        name = field["name"]
        kind = field["type"]
        if kind == "bit":
            e.w(level, f"{name}=bool({frame}[{base + _offset_of(fields, field['container'])}] & {field['mask']}),")
            continue
        offset = base + field["offset"]
        if "expected" in field:
            continue
        if kind == "u8":
            e.w(level, f"{name}={frame}[{offset}],")
        elif kind == "i8":
            e.w(level, f"{name}=_i8({frame}[{offset}]),")
        elif kind == "bcd8":
            e.w(level, f"{name}=_bcd_decode({frame}[{offset}]),")
        elif kind == "u16le":
            e.w(level, f"{name}={frame}[{offset}] | ({frame}[{offset + 1}] << 8),")
        elif kind == "bytes":
            e.w(level, f"{name}=bytes({frame}[{offset}:{offset + field['length']}]),")
        elif kind == "string":
            end = offset + field["length"]
            e.w(level, f"{name}=bytes({frame}[{offset}:{end}]).split(b\"\\x00\", 1)[0].decode(\"utf-8\", \"replace\"),")
        elif kind == "array":
            item = f"{spec_lib.camel(name)}Item"
            stride = field["stride"]
            start = base + field["offset"]
            e.w(level, f"{name}=[")
            e.w(level + 1, f"_decode_{ctor}_{name}_item({frame}[{start} + {stride} * _i:{start} + {stride} * (_i + 1)])")
            e.w(level + 1, f"for _i in range({field['count']})")
            e.w(level, "],")
        else:  # pragma: no cover - validated by spec_lib
            raise AssertionError(kind)


def _offset_of(fields: list[dict], name: str) -> int:
    for field in fields:
        if field["name"] == name:
            return field["offset"]
    raise KeyError(name)


def emit_encode_fields(
    e: Emitter,
    fields: list[dict],
    value: str,
    out: str,
    base: str,
    level: int,
    cls: str,
) -> None:
    groups = spec_lib.bits_by_container(fields)
    for field in fields:
        name = field["name"]
        kind = field["type"]
        if kind == "bit":
            continue
        index = f"{base}{field['offset']}" if base else str(field["offset"])
        offset = field["offset"]
        if "expected" in field:
            e.w(level, f"{out}[{index}] = {field['expected']}")
            continue
        if kind == "u8":
            if name in groups:
                continue
            range_check(e, f"{cls}.{name}", f"{value}.{name}", field, level)
            e.w(level, f"{out}[{index}] = {value}.{name}")
        elif kind == "i8":
            range_check(e, f"{cls}.{name}", f"{value}.{name}", field, level)
            e.w(level, f"{out}[{index}] = {value}.{name} & 0xFF")
        elif kind == "bcd8":
            e.w(level, f"{out}[{index}] = _bcd_encode({value}.{name})")
        elif kind == "u16le":
            range_check(e, f"{cls}.{name}", f"{value}.{name}", field, level)
            next_index = f"{base}{field['offset'] + 1}" if base else str(field["offset"] + 1)
            e.w(level, f"{out}[{index}] = {value}.{name} & 0xFF")
            e.w(level, f"{out}[{next_index}] = ({value}.{name} >> 8) & 0xFF")
        elif kind == "bytes":
            e.w(level, f"if len({value}.{name}) != {field['length']}:")
            e.w(level + 1, f"raise ProtoError(\"{cls}.{name}: expected {field['length']} bytes\")")
            e.w(level, f"{out}[{index}:{index} + {field['length']}] = {value}.{name}")
        elif kind == "string":
            length = field["length"]
            e.w(level, f"_put_string({cls!r}, {name!r}, {value}.{name}, {out}, {index}, {length})")
        elif kind == "array":
            stride = field["stride"]
            start = f"{base}{offset}" if base else str(offset)
            e.w(level, f"if len({value}.{name}) != {field['count']}:")
            e.w(level + 1, f"raise ProtoError(\"{cls}.{name}: expected {field['count']} entries\")")
            e.w(level, f"for _i, _item in enumerate({value}.{name}):")
            e.w(
                level + 1,
                f"_encode_{cls.replace('.', '_')}_{name}_item(_item, {out}, {start} + {stride} * _i)",
            )
            if groups.get(name):
                raise AssertionError("bits on array itself are not supported")
        else:  # pragma: no cover
            raise AssertionError(kind)
    # containers with bit fields are written once, after their scalar pass
    for container, bits in groups.items():
        index = f"{base}{_offset_of(fields, container)}" if base else str(_offset_of(fields, container))
        e.w(level, f"_flags = {value}.{container} & 0xFF")
        for bit in bits:
            mask = bit["mask"]
            inv = (~mask) & 0xFF
            e.w(level, f"_flags = (_flags | {mask:#04x}) if {value}.{bit['name']} else (_flags & {inv:#04x})")
        e.w(level, f"{out}[{index}] = _flags")


def emit_class(e: Emitter, cls: dict) -> None:
    name = cls["name"]
    total = cls["length"]["max"]
    fields = cls["fields"]
    cam = spec_lib.camel(name)

    for field in fields:
        if field["type"] != "array":
            continue
        item = f"{cam}{spec_lib.camel(field['name'])}Item"
        fn = f"{name}_{field['name']}"
        e.w(0, "@dataclass")
        e.w(0, f"class {item}:")
        for sub in field["fields"]:
            if "expected" in sub:
                continue
            if sub["type"] == "bit":
                e.w(1, f"{sub['name']}: bool")
            elif sub["type"] == "bytes":
                e.w(1, f"{sub['name']}: bytes")
            elif sub["type"] == "string":
                e.w(1, f"{sub['name']}: str")
            else:
                e.w(1, f"{sub['name']}: int")
        e.w(0)
        e.w(0, f"def _decode_{fn}_item(chunk: bytes) -> {item}:")
        emit_constant_checks(e, field["fields"], "chunk", 0, 1, fn)
        e.w(1, f"return {item}(")
        emit_decode_fields(e, field["fields"], "chunk", 0, 2, fn)
        e.w(1, ")")
        e.w(0)
        e.w(0, f"def _encode_{fn}_item(value: {item}, out: bytearray, base: int) -> None:")
        emit_encode_fields(e, field["fields"], "value", "out", "base + ", 1, fn)
        e.w(0)

    e.w(0, "@dataclass")
    e.w(0, f"class {cam}:")
    if not fields:
        e.w(1, "pass")
    for field in fields:
        if "expected" in field:
            continue
        if field["type"] == "array":
            e.w(1, f"{field['name']}: list[{cam}{spec_lib.camel(field['name'])}Item]")
        else:
            e.w(1, f"{field['name']}: {spec_lib.py_default_type(field)}")
    e.w(0)
    e.w(0, f"def decode_{name}(frame: bytes) -> {cam}:")
    e.w(1, f"if len(frame) < {total}:")
    e.w(2, f"raise ProtoError(f\"{name}: need {total} bytes, got {{len(frame)}}\")")
    e.w(1, f"if frame[0] != {cls['id']}:")
    e.w(2, f"raise ProtoError(f\"{name}: class byte {{frame[0]:#04x}}, expected {cls['id']:#04x}\")")
    emit_constant_checks(e, fields, "frame", 0, 1, name)
    e.w(1, f"return {cam}(")
    emit_decode_fields(e, fields, "frame", 0, 2, name)
    e.w(1, ")")
    e.w(0)
    e.w(0, f"def encode_{name}(value: {cam}) -> bytes:")
    e.w(1, f"out = bytearray({total})")
    e.w(1, f"out[0] = {cls['id']:#04x}")
    emit_encode_fields(e, fields, "value", "out", "", 1, name)
    e.w(1, "return bytes(out)")
    e.w(0)


def generate(spec: dict) -> str:
    e = Emitter()
    e.w(0, "# Generated by gen/gen_py.py from spec/classes.json. Do not edit.")
    e.w(0, "from __future__ import annotations")
    e.w(0)
    e.w(0, "from dataclasses import dataclass")
    e.w(0)
    e.w(0)
    e.w(0, "class ProtoError(ValueError):")
    e.w(1, "\"\"\"A frame does not match the spec, or a value is out of range.\"\"\"")
    e.w(0)
    e.w(0)
    e.w(0, "def _i8(raw: int) -> int:")
    e.w(1, "return raw - 256 if raw > 127 else raw")
    e.w(0)
    e.w(0)
    e.w(0, "def _bcd_decode(raw: int) -> int:")
    e.w(1, "return ((raw >> 4) & 0x0F) * 10 + (raw & 0x0F)")
    e.w(0)
    e.w(0)
    e.w(0, "def _bcd_encode(value: int) -> int:")
    e.w(1, "if not 0 <= value <= 99:")
    e.w(2, "raise ProtoError(f\"BCD value out of range: {value}\")")
    e.w(1, "return ((value // 10) << 4) | (value % 10)")
    e.w(0)
    e.w(0)
    e.w(0, "def _check_range(path: str, value: int, lo: int, hi: int) -> None:")
    e.w(1, "if not lo <= value <= hi:")
    e.w(2, "raise ProtoError(f\"{path}: {value} outside {lo}..{hi}\")")
    e.w(0)
    e.w(0)
    e.w(0, "def _put_string(cls: str, name: str, value: str, out: bytearray, offset: int, length: int) -> None:")
    e.w(1, "encoded = value.encode(\"utf-8\")")
    e.w(1, "if len(encoded) > length:")
    e.w(2, "raise ProtoError(f\"{cls}.{name}: {len(encoded)} bytes exceeds {length}\")")
    e.w(1, "out[offset:offset + len(encoded)] = encoded")
    e.w(0)
    e.w(0)

    for cls in spec["classes"]:
        emit_class(e, cls)

    e.w(0, "CLASS_INFO: dict[int, dict] = {")
    for cls in spec["classes"]:
        e.w(
            1,
            f"{cls['id']}: {{'name': {cls['name']!r}, 'title': {cls['title']!r}, "
            f"'direction': {cls['direction']!r}, 'confidence': {cls['confidence']!r}, "
            f"'length': {cls['length']['max']}}},",
        )
    e.w(0, "}")
    e.w(0)
    e.w(0, "_DECODERS = {")
    for cls in spec["classes"]:
        e.w(1, f"{cls['id']}: decode_{cls['name']},")
    e.w(0, "}")
    e.w(0, "_ENCODERS = {")
    for cls in spec["classes"]:
        e.w(1, f"{cls['id']}: encode_{cls['name']},")
    e.w(0, "}")
    e.w(0)
    e.w(0, "def decode_frame(frame: bytes):")
    e.w(1, "if not frame:")
    e.w(2, "raise ProtoError(\"empty frame\")")
    e.w(1, "try:")
    e.w(2, "fn = _DECODERS[frame[0]]")
    e.w(1, "except KeyError:")
    e.w(2, "raise ProtoError(f\"no codec for class {frame[0]:#04x}\") from None")
    e.w(1, "return fn(frame)")
    e.w(0)
    e.w(0, "def encode_frame(value) -> bytes:")
    e.w(1, "for cls_id, fn in _ENCODERS.items():")
    e.w(2, "if isinstance(value, _ENCODER_TYPES[cls_id]):")
    e.w(3, "return fn(value)")
    e.w(1, "raise ProtoError(f\"no encoder for {type(value).__name__}\")")
    e.w(0)
    e.w(0, "_ENCODER_TYPES = {")
    for cls in spec["classes"]:
        e.w(1, f"{cls['id']}: {spec_lib.camel(cls['name'])}, ")
    e.w(0, "}")
    return e.text()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=Path, default=DEFAULT_SPEC)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args(argv)
    spec = spec_lib.load(args.spec)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(generate(spec), encoding="utf-8")
    print(f"wrote {args.out} ({len(spec['classes'])} classes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

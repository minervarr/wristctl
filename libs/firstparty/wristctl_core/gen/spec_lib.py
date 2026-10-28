"""Loader and validator for spec/classes.json.

Both generators and the vector builder share this module so the schema is
enforced in one place. A spec that does not validate never reaches a codec.
"""

from __future__ import annotations

import json
import keyword
from pathlib import Path

C_KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
    "int", "long", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned",
    "void", "volatile", "while", "_Bool", "bool", "true", "false",
}

SCALAR_SIZE = {"u8": 1, "i8": 1, "u16le": 2, "bcd8": 1}


class SpecError(ValueError):
    pass


def load(path: Path) -> dict:
    spec = json.loads(Path(path).read_text(encoding="utf-8"))
    validate(spec)
    return spec


def ident(name: str, where: str) -> None:
    if not name or not (name[0].isalpha() or name[0] == "_"):
        raise SpecError(f"{where}: invalid identifier {name!r}")
    if not all(c.isalnum() or c == "_" for c in name):
        raise SpecError(f"{where}: invalid identifier {name!r}")
    if keyword.iskeyword(name):
        raise SpecError(f"{where}: {name!r} is a Python keyword")
    if name in C_KEYWORDS:
        raise SpecError(f"{where}: {name!r} is a C keyword")


def field_size(field: dict) -> int:
    kind = field["type"]
    if kind in SCALAR_SIZE:
        return SCALAR_SIZE[kind]
    if kind == "bytes":
        return field["length"]
    if kind == "string":
        return field["length"]
    raise SpecError(f"field {field['name']}: no fixed size for {kind}")


def validate_fields(fields: list[dict], limit: int, where: str) -> None:
    seen: set[str] = set()
    for field in fields:
        name = field["name"]
        ident(name, where)
        if name in seen:
            raise SpecError(f"{where}: duplicate field {name}")
        seen.add(name)
        kind = field["type"]
        if kind == "bit":
            if "offset" in field:
                raise SpecError(f"{where}.{name}: bit fields use 'container', not 'offset'")
            if field["mask"] <= 0 or field["mask"] & (field["mask"] - 1):
                raise SpecError(f"{where}.{name}: mask must be a single bit")
            continue
        offset = field["offset"]
        if offset < 0:
            raise SpecError(f"{where}.{name}: negative offset")
        if kind == "array":
            end = offset + field["count"] * field["stride"]
            if end > limit:
                raise SpecError(f"{where}.{name}: array ends at {end} > {limit}")
            if field["count"] < 1 or field["stride"] < 1:
                raise SpecError(f"{where}.{name}: bad array geometry")
            validate_fields(field["fields"], field["stride"], f"{where}.{name}")
            continue
        if kind not in SCALAR_SIZE and kind not in ("bytes", "string"):
            raise SpecError(f"{where}.{name}: unknown type {kind}")
        size = field_size(field)
        if offset + size > limit:
            raise SpecError(f"{where}.{name}: ends at {offset + size} > {limit}")
        if "range" in field:
            lo, hi = field["range"]
            low, high = {
                "u16le": (0, 0xFFFF),
                "bcd8": (0, 99),
                "i8": (-128, 127),
            }.get(kind, (0, 0xFF))
            if not low <= lo <= hi <= high:
                raise SpecError(f"{where}.{name}: bad range {field['range']}")
    # bit fields must reference a u8 container declared in the same scope
    containers = {f["name"]: f for f in fields if f["type"] == "u8"}
    for field in fields:
        if field["type"] != "bit":
            continue
        container = containers.get(field["container"])
        if container is None:
            raise SpecError(f"{where}.{field['name']}: container {field['container']!r} is not a u8 field")
        if field["mask"] > 0xFF:
            raise SpecError(f"{where}.{field['name']}: mask exceeds container")


def validate(spec: dict) -> None:
    if spec.get("version") != 1:
        raise SpecError("unsupported spec version")
    ids: set[int] = set()
    names: set[str] = set()
    for cls in spec["classes"]:
        cls_id = cls["id"]
        if not 0 <= cls_id <= 0xFF:
            raise SpecError(f"class {cls_id}: id out of range")
        if cls_id in ids:
            raise SpecError(f"duplicate class id {cls_id}")
        ids.add(cls_id)
        ident(cls["name"], "class")
        if cls["name"] in names:
            raise SpecError(f"duplicate class name {cls['name']}")
        names.add(cls["name"])
        length = cls["length"]
        if length["min"] != length["max"]:
            raise SpecError(f"{cls['name']}: variable-length frames are not supported yet")
        if length["min"] < 1:
            raise SpecError(f"{cls['name']}: length must include the class byte")
        if cls["direction"] not in ("read", "write", "readwrite"):
            raise SpecError(f"{cls['name']}: bad direction")
        if "confidence" not in cls:
            raise SpecError(f"{cls['name']}: missing confidence")
        validate_fields(cls["fields"], length["max"], cls["name"])


def camel(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def bits_by_container(fields: list[dict]) -> dict[str, list[dict]]:
    result: dict[str, list[dict]] = {}
    for field in fields:
        if field["type"] == "bit":
            result.setdefault(field["container"], []).append(field)
    return result


def py_default_type(field: dict) -> str:
    kind = field["type"]
    if kind == "bit":
        return "bool"
    if kind == "bytes":
        return "bytes"
    if kind == "string":
        return "str"
    if kind == "array":
        return f"list[{camel(field['name'])}Item]"
    return "int"


def c_type(field: dict) -> str:
    kind = field["type"]
    if kind == "bit":
        return "uint8_t"
    if kind == "u16le":
        return "uint16_t"
    if kind == "i8":
        return "int8_t"
    if kind in ("u8", "bcd8"):
        return "uint8_t"
    if kind == "bytes":
        return f"uint8_t {field['name']}[{field['length']}]"
    if kind == "string":
        return f"char {field['name']}[{field['length'] + 1}]"
    raise SpecError(f"no C type for {kind}")


def scalar_fields(fields: list[dict]) -> list[dict]:
    return [f for f in fields if f["type"] not in ("bit", "array")]

#!/usr/bin/env python3
"""Extract the Casio model table from the decompiled G-SHOCK MOVE app.

The vendor app identifies a watch by its advertised BLE name
(``CasioLibUtil.DeviceType.getDeviceType``), then gates protocol behaviour on
``DeviceType`` predicate methods. This script parses the enum constants and
those predicates out of the JADX output and evaluates them for every model, so
the registry is derived rather than hand-transcribed.

Method bodies are evaluated with a small interpreter for the subset of Java
that the table actually uses:

    return <expr>;
    if (<expr>) { return <expr>; }
    int x = ...;              (ignored)

Anything outside that subset (two methods use a synthetic switch map) is
reported as unresolved instead of being guessed.

Usage:
    python3 gen/extract_models.py [--java PATH] [--out PATH] [--check]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

CORE = Path(__file__).resolve().parent.parent
WRISTCTL = CORE.parents[2]
PROJECT = WRISTCTL.parent
DECOMP = Path(os.environ.get("GSHOCK_DECOMP", PROJECT / "GSHOCK_DECOMP"))

DEFAULT_JAVA = DECOMP / (
    "work/jadx/base/sources/com/casio/casiolib/util/CasioLibUtil.java"
)
DEFAULT_OUT = CORE / "spec" / "models.json"

# Constants are declared one per line in the enum body.
CONST_RE = re.compile(
    r"^\s*([A-Z][A-Z0-9_]*)\s*\(\s*(\d+)\s*,\s*new String\[\]\s*\{(.*?)\}\s*"
    r"(?:,\s*(-?\d+)\s*)?\)\s*[;,]?\s*$",
    re.MULTILINE,
)
METHOD_RE = re.compile(r"public boolean (\w+)\(\)\s*\{")

TOKEN_RE = re.compile(
    r"\s*(?:"
    r"(?P<ident>[A-Za-z_][A-Za-z0-9_]*)"
    r"|(?P<number>\d+)"
    r"|(?P<op>&&|\|\||==|!=|[!()?:,;{}])"
    r")"
)


class Unresolved(Exception):
    """The method body uses Java the interpreter does not model."""


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def brace_body(text: str, start: int) -> tuple[str, int]:
    """Return the body of the block starting at ``text[start] == '{'``."""
    assert text[start] == "{"
    depth = 0
    i = start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1 : i], i + 1
        i += 1
    raise ValueError("unbalanced braces")


def tokenize(expr: str) -> list[str]:
    tokens: list[str] = []
    pos = 0
    while pos < len(expr):
        if expr[pos].isspace():
            pos += 1
            continue
        match = TOKEN_RE.match(expr, pos)
        if not match or match.end() == pos:
            raise Unresolved(f"cannot tokenize at {expr[pos:pos + 20]!r}")
        tokens.append(match.group(match.lastgroup))  # type: ignore[arg-type]
        pos = match.end()
    return tokens


class ExprParser:
    def __init__(self, tokens: list[str], model: "Model", registry: "Registry"):
        self.tokens = tokens
        self.pos = 0
        self.model = model
        self.registry = registry

    def peek(self) -> str | None:
        return self.tokens[self.pos] if self.pos < len(self.tokens) else None

    def take(self, expected: str | None = None) -> str:
        token = self.peek()
        if token is None or (expected is not None and token != expected):
            raise Unresolved(f"expected {expected!r}, got {token!r}")
        self.pos += 1
        return token

    def parse(self) -> bool:
        value = self.ternary()
        if self.pos != len(self.tokens):
            raise Unresolved(f"trailing tokens {self.tokens[self.pos:]}")
        return value

    def ternary(self) -> bool:
        cond = self.or_expr()
        if self.peek() == "?":
            self.take("?")
            yes = self.ternary()
            self.take(":")
            no = self.ternary()
            return yes if cond else no
        return cond

    def or_expr(self) -> bool:
        value = self.and_expr()
        while self.peek() == "||":
            self.take("||")
            rhs = self.and_expr()
            value = value or rhs
        return value

    def and_expr(self) -> bool:
        value = self.unary()
        while self.peek() == "&&":
            self.take("&&")
            rhs = self.unary()
            value = value and rhs
        return value

    def unary(self) -> bool:
        if self.peek() == "!":
            self.take("!")
            return not self.unary()
        return self.primary()

    def primary(self) -> bool:
        token = self.peek()
        if token == "(":
            self.take("(")
            value = self.ternary()
            self.take(")")
            return value
        if token in ("true", "false"):
            self.take()
            return token == "true"
        if token == "this":
            self.take("this")
            if self.peek() == "==":
                self.take("==")
                target = self.take()
                return self.model.name == target
            raise Unresolved("bare 'this'")
        ident = self.take()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", ident):
            raise Unresolved(f"unexpected token {ident!r}")
        if self.peek() == "(":
            self.take("(")
            args = []
            while self.peek() != ")":
                args.append(self.take())
                if self.peek() == ",":
                    self.take(",")
            self.take(")")
            return self.call(ident, args)
        raise Unresolved(f"bare identifier {ident!r}")

    def call(self, name: str, args: list[str]) -> bool:
        if name == "isEqualsModel":
            target = self.registry.get(args[0])
            return target.generation == self.model.generation
        if name == "isAfter":
            target = self.registry.get(args[0])
            return target.generation <= self.model.generation
        if name == "isBefore":
            target = self.registry.get(args[0])
            return self.model.generation <= target.generation
        if name in self.registry.methods and not args:
            return self.registry.evaluate(name, self.model)
        raise Unresolved(f"unsupported call {name}({', '.join(args)})")


class Model:
    __slots__ = ("name", "generation", "service_size", "aliases", "flags")

    def __init__(self, name: str, generation: int, service_size: int, aliases: list[str]):
        self.name = name
        self.generation = generation
        self.service_size = service_size
        self.aliases = aliases
        self.flags: dict[str, bool] = {}


class Registry:
    def __init__(self, models: list[Model], methods: dict[str, str]):
        self.models = models
        self.by_name = {m.name: m for m in models}
        self.methods = methods
        self.unresolved: dict[str, str] = {}
        self.cache: dict[tuple[str, str], bool] = {}

    def get(self, name: str) -> Model:
        if name not in self.by_name:
            raise Unresolved(f"unknown model constant {name}")
        return self.by_name[name]

    def evaluate(self, method: str, model: Model) -> bool:
        key = (method, model.name)
        if key in self.cache:
            return self.cache[key]
        try:
            value = self._run(self.methods[method], model)
        except Unresolved as exc:
            self.unresolved.setdefault(method, str(exc))
            raise
        self.cache[key] = value
        return value

    def _run(self, body: str, model: Model) -> bool:
        body = strip_comments(body).strip()
        if body.startswith("switch") or "switch (" in body:
            raise Unresolved("switch statement")
        tokens = tokenize(body)
        pos = 0
        while pos < len(tokens):
            token = tokens[pos]
            if token == "return":
                value, pos = self._return(tokens, pos + 1, model)
                return value
            if token == "if":
                cond, pos = self._paren(tokens, pos + 1, model)
                if tokens[pos] != "{":
                    raise Unresolved("if without block")
                inside, pos = self._block(tokens, pos)
                # `if (cond) { return ...; }` is the only form in this table.
                if cond:
                    return self._run(" ".join(inside), model)
                continue
            # int x = ...; or other local declarations: skip to ';'
            while pos < len(tokens) and tokens[pos] != ";":
                pos += 1
            pos += 1
        raise Unresolved("no return")

    def _paren(self, tokens: list[str], pos: int, model: Model) -> tuple[bool, int]:
        if tokens[pos] != "(":
            raise Unresolved("expected (")
        depth = 0
        start = pos + 1
        while pos < len(tokens):
            if tokens[pos] == "(":
                depth += 1
            elif tokens[pos] == ")":
                depth -= 1
                if depth == 0:
                    return ExprParser(tokens[start:pos], model, self).parse(), pos + 1
            pos += 1
        raise Unresolved("unbalanced (")

    def _block(self, tokens: list[str], pos: int) -> tuple[list[str], int]:
        assert tokens[pos] == "{"
        depth = 0
        start = pos + 1
        while pos < len(tokens):
            if tokens[pos] == "{":
                depth += 1
            elif tokens[pos] == "}":
                depth -= 1
                if depth == 0:
                    return tokens[start:pos], pos + 1
            pos += 1
        raise Unresolved("unbalanced {")

    def _return(self, tokens: list[str], pos: int, model: Model) -> tuple[bool, int]:
        start = pos
        while pos < len(tokens) and tokens[pos] != ";":
            pos += 1
        return ExprParser(tokens[start:pos], model, self).parse(), pos + 1


def provenance_path(java_path: Path) -> str:
    """Record the decompiled source as a project-relative path when possible."""
    try:
        return java_path.resolve().relative_to(PROJECT).as_posix()
    except ValueError:
        return str(java_path)


def parse_constants(source: str) -> list[Model]:
    models = []
    for match in CONST_RE.finditer(source):
        name, generation, strings, service_size = match.groups()
        aliases = re.findall(r'"([^"]*)"', strings)
        models.append(
            Model(
                name=name,
                generation=int(generation),
                service_size=int(service_size) if service_size is not None else -1,
                aliases=aliases,
            )
        )
    return models


def parse_methods(source: str, start: int) -> dict[str, str]:
    methods: dict[str, str] = {}
    for match in METHOD_RE.finditer(source, start):
        body, _ = brace_body(source, source.index("{", match.start()))
        methods[match.group(1)] = body
    return methods


def build(java_path: Path) -> dict:
    source = java_path.read_text(encoding="utf-8", errors="replace")
    models = parse_constants(source)
    if not models:
        raise SystemExit("no DeviceType constants found")
    enum_start = source.index("public enum DeviceType {")
    methods = parse_methods(source, enum_start)
    registry = Registry(models, methods)

    names = sorted(methods)
    for model in models:
        flags = {}
        for name in names:
            try:
                flags[name] = registry.evaluate(name, model)
            except Unresolved:
                flags[name] = None
        model.flags = flags

    resolved = [n for n in names if n not in registry.unresolved]
    unresolved = {n: registry.unresolved[n] for n in sorted(registry.unresolved)}
    width = (len(resolved) + 7) // 8
    entries = []
    for model in models:
        bits = bytearray(width)
        for index, name in enumerate(resolved):
            if model.flags.get(name):
                bits[index // 8] |= 1 << (index % 8)
        entries.append(
            {
                "name": model.name,
                "aliases": model.aliases,
                "generation": model.generation,
                "service_size": model.service_size,
                "bits": bits.hex(),
            }
        )
    return {
        "version": 1,
        "source": provenance_path(java_path),
        "note": (
            "Generated by gen/extract_models.py from the JADX output of "
            "com.casio.gshockmove 3.6.0. Do not edit by hand."
        ),
        "flag_names": resolved,
        "unresolved_methods": unresolved,
        "generations": generation_map(entries),
        "models": entries,
    }


def generation_map(entries: list[dict]) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for entry in entries:
        result.setdefault(str(entry["generation"]), []).append(entry["name"])
    return dict(sorted(result.items(), key=lambda kv: int(kv[0])))


def flag_get(model: dict, flags: list[str], name: str) -> bool:
    index = flags.index(name)
    raw = bytes.fromhex(model["bits"])
    return bool(raw[index // 8] & (1 << (index % 8)))


def check(spec: dict) -> list[str]:
    """Assert facts already established from the decompiled sources."""
    flags = spec["flag_names"]
    by_name = {m["name"]: m for m in spec["models"]}
    failures = []

    def expect(model: str, flag: str, want: bool) -> None:
        got = flag_get(by_name[model], flags, flag)
        if got != want:
            failures.append(f"{model}.{flag} = {got}, expected {want}")

    if len(spec["models"]) != 137:
        failures.append(f"model count {len(spec['models'])}, expected 137")
    if len(spec["generations"]) != 37:
        failures.append(f"generation count {len(spec['generations'])}, expected 37")
    if sorted(spec["unresolved_methods"]) != ["isUseCEPData", "isUseDstWatchStateAutoDstRep"]:
        failures.append(f"unexpected unresolved set {sorted(spec['unresolved_methods'])}")
    tide = [m["name"] for m in spec["models"] if flag_get(m, flags, "isEnableTideData")]
    if tide != ["GBX_100"]:
        failures.append(f"tide models {tide}, expected ['GBX_100']")
    expect("GBX_100", "isEnableTideData", True)
    expect("GBX_100", "isUseReadAndWriteTideData", True)
    expect("GBD_H1000", "isEnableTideData", False)
    expect("GBX_100", "isPartTimeLink", True)
    expect("GBX_100", "isPairingInsteadOfLlsAl", True)
    expect("GBX_100", "isNeedCreateBond", False)
    expect("GBX_100", "isNoPairing", True)
    expect("GBD_H2000", "isNeedCreateBond", True)
    expect("GBD_H1000", "isUseMultiple", True)
    expect("GBX_100", "isUseMultiple", True)
    expect("GBX_100", "isUseAllFeatures", True)
    expect("GSHOCK_TYPE_A_2KEY", "isUseAllFeatures", False)
    expect("GWF_300", "isUseAllFeatures", True)
    expect("GBX_100", "isEnableProgramTmrSetting", True)
    expect("GBX_100", "isEnableNotificationManager", True)
    expect("GBD_H1000", "isEnableNotificationManager", True)
    expect("GSHOCK_TYPE_A_2KEY", "isEnableNotificationManager", False)
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java", type=Path, default=DEFAULT_JAVA)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--check", action="store_true", help="verify known facts")
    args = parser.parse_args(argv)

    spec = build(args.java)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8")
    print(
        f"models={len(spec['models'])} generations={len(spec['generations'])} "
        f"flags={len(spec['flag_names'])} unresolved={len(spec['unresolved_methods'])}"
    )
    for name, reason in spec["unresolved_methods"].items():
        print(f"  unresolved {name}: {reason}")
    if args.check:
        failures = check(spec)
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        if failures:
            return 1
        print("check: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

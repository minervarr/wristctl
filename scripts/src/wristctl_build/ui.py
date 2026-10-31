"""Console output: steps, notes, warnings, failures.

Colour is only used on a real terminal and honours ``NO_COLOR``.
"""

from __future__ import annotations

import os
import sys
from typing import TextIO

_RESET = "\033[0m"
_COLORS = {
    "step": "\033[36m",  # cyan
    "ok": "\033[32m",  # green
    "warn": "\033[33m",  # yellow
    "fail": "\033[31m",  # red
}

_colour: bool | None = None


def use_colour() -> bool:
    """Whether to emit ANSI colour (cached; safe to call often)."""
    global _colour
    if _colour is None:
        _colour = False
        if not os.environ.get("NO_COLOR"):
            try:
                _colour = bool(sys.stdout.isatty())
            except Exception:
                _colour = False
    return _colour


def _emit(stream: TextIO, role: str, text: str) -> None:
    if use_colour():
        print(f"{_COLORS[role]}{text}{_RESET}", file=stream, flush=True)
    else:
        print(text, file=stream, flush=True)


def step(text: str) -> None:
    """A numbered progress line: ``==> [2/3] Configuring...``."""
    _emit(sys.stdout, "step", text)


def note(text: str) -> None:
    """A detail line, printed verbatim (callers keep the indentation)."""
    print(text, flush=True)


def ok(text: str) -> None:
    _emit(sys.stdout, "ok", text)


def warn(text: str) -> None:
    _emit(sys.stderr, "warn", f"  ! {text}")


def fail(text: str) -> None:
    _emit(sys.stderr, "fail", f"error: {text}")

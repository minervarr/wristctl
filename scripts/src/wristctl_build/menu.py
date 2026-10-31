"""The start menu: build type, microarchitecture, GUI.

A convenience over the flags, never a separate code path — the answers are the
same values the command line accepts.
"""

from __future__ import annotations

from collections.abc import Callable

from .errors import UsageError
from .variants import NATIVE, sanitize_arch

Reader = Callable[[str], str]

BUILD_TYPE_OPTIONS: tuple[tuple[str, str], ...] = (
    ("Debug", "asserts and symbols, tests included (the dev loop)"),
    ("Release", "optimised, no tests"),
)

ARCH_OPTIONS: tuple[tuple[str, str], ...] = (
    ("", "portable baseline"),
    (NATIVE, "tuned to this exact CPU (-march=native)"),
    ("custom", "enter a specific -march value (v3, v4, znver4, ...)"),
)


def choose(
    prompt: str,
    options: tuple[tuple[str, str], ...],
    *,
    default: str,
    reader: Reader = input,
) -> str:
    """Print a numbered menu and return the chosen key."""
    print(f"\n{prompt}")
    for index, (key, description) in enumerate(options, start=1):
        marker = " (default)" if key == default else ""
        shown = key if key else "baseline"
        print(f"  {index}) {shown} - {description}{marker}")
    while True:
        try:
            answer = reader(f"Choice [1-{len(options)}]: ").strip()
        except EOFError as exc:
            raise UsageError("no input available: pass the matching flag") from exc
        if not answer:
            return default
        if answer.isdigit() and 1 <= int(answer) <= len(options):
            return options[int(answer) - 1][0]
        for key, _ in options:
            if key and answer.lower() == key.lower():
                return key
        print(f"Please enter 1-{len(options)}.")


def choose_build_type(*, reader: Reader = input) -> str:
    """Return ``Debug`` or ``Release``."""
    return choose("Select build type:", BUILD_TYPE_OPTIONS, default="Debug", reader=reader)


def choose_arch(*, reader: Reader = input) -> str:
    """Return a ``-march`` value, or ``""`` for the portable baseline."""
    choice = choose(
        "Select microarchitecture:",
        ARCH_OPTIONS,
        default="",
        reader=reader,
    )
    if choice != "custom":
        return choice
    try:
        value = reader("Enter -march value (e.g. v3, znver4): ").strip()
    except EOFError as exc:
        raise UsageError("no input available: pass --arch <value>") from exc
    return sanitize_arch(value)


def confirm(prompt: str, *, default: bool, reader: Reader = input) -> bool:
    """Ask a yes/no question; empty input takes ``default``."""
    marker = "[Y/n]" if default else "[y/N]"
    while True:
        try:
            answer = reader(f"{prompt} {marker}: ").strip().lower()
        except EOFError as exc:
            raise UsageError("no input available: pass --gui or --no-gui") from exc
        if not answer:
            return default
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False
        print("Please answer y or n.")

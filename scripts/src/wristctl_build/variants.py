"""Microarchitecture variants — the one place arch names live.

The build directory carries the variant, like the sibling projects:
``build/linux`` (portable Release), ``build/linux_debug``,
``build/linux_native`` and ``build/linux_custom-<arch>[_debug]``.
"""

from __future__ import annotations

import re

from .errors import BuildError

NATIVE = "native"
_ARCH_RE = re.compile(r"^[A-Za-z0-9_.+-]+$")


def sanitize_arch(value: str) -> str:
    """Validate a user-supplied ``-march`` value.

    The value doubles as part of a directory name, so it is restricted to
    characters that are safe in a path.
    """
    arch = value.strip()
    if not arch:
        raise BuildError("--arch needs a value, e.g. native, v3, znver4")
    if not _ARCH_RE.match(arch) or ".." in arch:
        raise BuildError(
            f"refusing {value!r} as a -march value: it is also used as a "
            "directory name, so only letters, digits and . _ + - are allowed"
        )
    return arch


def arch_suffix(arch: str) -> str:
    """Build-directory suffix for a single-variant build."""
    if not arch:
        return ""
    if arch == NATIVE:
        return "_native"
    return f"_custom-{arch}"


def march_value(arch: str) -> str:
    """The value GCC/Clang expect after ``-march=``."""
    if arch in ("v2", "v3", "v4"):
        return f"x86-64-{arch}"
    return arch


def cmake_args(arch: str) -> tuple[str, ...]:
    """CMake cache entries for one microarchitecture, empty for the baseline."""
    if not arch:
        return ()
    value = march_value(arch)
    return (
        f"-DCMAKE_C_FLAGS=-march={value}",
        f"-DCMAKE_CXX_FLAGS=-march={value}",
    )

"""Repository layout: the root, the build variants, the manifest.

Everything derived from the repository's own names lives here. The build
output is one ``build/`` root with one subdirectory per variant, matching the
``build/linux_*`` layout used by the sibling minervarr projects.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any

from .errors import BuildError
from .variants import arch_suffix

CMAKE_FILE = "CMakeLists.txt"
MANIFEST_FILE = "manifest.json"
CLI_BINARY = "wristctl_cli"
GUI_BINARY = "wristctl_gui"
_BUILD_ROOT = "build"


def current_target() -> str:
    """The platform being targeted. Only ``linux`` is supported today."""
    if os.name == "nt":
        raise BuildError("Windows is not supported by this tool (wristctl is Linux/Android)")
    if sys.platform == "darwin":
        raise BuildError(
            "macOS is not supported: wristctl is Linux-first; Android uses Gradle"
        )
    return "linux"


def _default_bases() -> list[Path]:
    """Where to start looking for the checkout: the cwd, then the launcher.

    The launcher matters for the ``.pyz``: run as ``scripts/wristctl_builder.pyz``
    from an unrelated directory, its own location is what finds the root.
    """
    bases = [Path.cwd().resolve()]
    argv0 = sys.argv[0] if sys.argv and sys.argv[0] else ""
    if argv0:
        try:
            bases.append(Path(argv0).resolve().parent)
        except OSError:
            pass
    return bases


def find_repo_root(start: Path | None = None) -> Path:
    """Walk up to the directory holding both ``CMakeLists.txt`` and ``manifest.json``."""
    bases = [Path(start).resolve()] if start is not None else _default_bases()
    for base in bases:
        for parent in (base, *base.parents):
            if (parent / CMAKE_FILE).is_file() and (parent / MANIFEST_FILE).is_file():
                return parent
    raise BuildError(
        f"not inside a wristctl checkout: no directory with both {CMAKE_FILE} "
        f"and {MANIFEST_FILE} above {', '.join(str(b) for b in bases)}"
    )


def build_dir(root: Path, *, arch: str = "", debug: bool = False) -> Path:
    """Build directory for one variant: ``build/linux_debug``, ``build/linux_native``..."""
    return root / _BUILD_ROOT / f"linux{arch_suffix(arch)}{'_debug' if debug else ''}"


def cli_binary_path(build_dir: Path) -> Path:
    """The headless CLI a build directory is expected to contain."""
    return build_dir / "platform" / "linux" / CLI_BINARY


def gui_binary_path(build_dir: Path) -> Path:
    """The desktop GUI, when it was built."""
    return build_dir / "gui" / GUI_BINARY


def display(root: Path, path: Path) -> str:
    """*path* relative to *root* when possible, for shorter console output."""
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def load_manifest(root: Path) -> dict[str, Any]:
    """Parse ``manifest.json`` — the single source of truth for the version."""
    try:
        return json.loads((root / MANIFEST_FILE).read_text(encoding="utf-8"))
    except OSError as exc:
        raise BuildError(f"cannot read {MANIFEST_FILE}: {exc}") from exc
    except ValueError as exc:
        raise BuildError(f"{MANIFEST_FILE} is not valid JSON: {exc}") from exc


def version(root: Path) -> str:
    """The project's version string, straight from ``manifest.json``."""
    return str(load_manifest(root).get("version", "unknown"))

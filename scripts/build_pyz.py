#!/usr/bin/env python3
"""Build ``wristctl_builder.pyz`` — the single-file build tool.

Usage:
    python build_pyz.py [output.pyz] [--force] [--run [args...]]

Runs on a plain interpreter: no ``uv``, no third-party imports, which is why
this bootstrap stays outside the zipapp. ``--run`` builds the archive and then
executes it with the same interpreter, passing on the remaining arguments —
that is what ``build.sh`` uses.

Ported from Klend-ario's ``scripts/build_pyz.py``, itself a port of
git_Wrapper's.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import zipapp
from pathlib import Path

HERE = Path(__file__).resolve().parent
PACKAGE = "wristctl_build"
SOURCE = HERE / "src" / PACKAGE
DEFAULT_OUTPUT = HERE / "wristctl_builder.pyz"
INTERPRETER = "/usr/bin/env python3"
_IGNORED = shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo")

# Written by hand rather than passed to zipapp as ``main=``: that shortcut
# generates ``<module>.<fn>()`` and throws the return value away, so the archive
# would always exit 0.
ARCHIVE_MAIN = f'''"""Entry point of the wristctl build tool."""
import sys

from {PACKAGE}.cli import main

if __name__ == "__main__":
    sys.exit(main())
'''


def source_files() -> list[Path]:
    """Every file that goes into the archive, plus this bootstrap."""
    files = [HERE / "build_pyz.py"]
    files += [path for path in SOURCE.rglob("*.py") if "__pycache__" not in path.parts]
    return files


def is_stale(output: Path) -> bool:
    """Whether *output* is missing or older than any source file."""
    if not output.is_file():
        return True
    newest = max(path.stat().st_mtime for path in source_files())
    return newest > output.stat().st_mtime


def build(output: Path = DEFAULT_OUTPUT) -> Path:
    """Zip the package into *output* and make it executable."""
    if not SOURCE.is_dir():
        raise SystemExit(f"error: package directory not found: {SOURCE}")
    with tempfile.TemporaryDirectory() as staging:
        staged = Path(staging)
        shutil.copytree(SOURCE, staged / PACKAGE, ignore=_IGNORED)
        (staged / "__main__.py").write_text(ARCHIVE_MAIN, encoding="utf-8")
        zipapp.create_archive(
            staged,
            target=output,
            interpreter=INTERPRETER,
            compressed=True,
        )
    output.chmod(output.stat().st_mode | 0o111)
    return output


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)

    run_requested = "--run" in args
    run_args: list[str] = []
    if run_requested:
        split = args.index("--run")
        run_args = args[split + 1 :]
        args = args[:split]

    force = "--force" in args
    args = [arg for arg in args if arg != "--force"]
    output = Path(args[0]).resolve() if args else DEFAULT_OUTPUT

    if force or is_stale(output):
        built = build(output)
        size = built.stat().st_size
        print(f"built {built} ({size / 1024:.0f} KiB)", flush=True)
    elif not run_requested:
        print(f"{output} is up to date (use --force to rebuild it)", flush=True)

    if not run_requested:
        return 0
    return subprocess.run(
        [sys.executable, str(output), *run_args], check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())

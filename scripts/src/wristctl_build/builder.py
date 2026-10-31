"""cmake configure + build: the only place that spells out cmake commands."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from pathlib import Path

from . import paths
from .process import run_checked
from .toolchain import Toolchain
from .ui import step


def configure_command(
    root: Path,
    build_dir: Path,
    *,
    toolchain: Toolchain,
    build_type: str,
    extra: Sequence[str] = (),
) -> list[str]:
    """The exact ``cmake`` configure line (CMake + Ninja only)."""
    cmd = [toolchain.cmake, "-S", str(root), "-B", str(build_dir), "-G", "Ninja"]
    cmd.append(f"-DCMAKE_BUILD_TYPE={build_type}")
    cmd += [str(arg) for arg in extra]
    return cmd


def build_command(toolchain: Toolchain, build_dir: Path, jobs: int) -> list[str]:
    """``cmake --build`` for an already-configured directory."""
    return [toolchain.cmake, "--build", str(build_dir), "-j", str(jobs)]


def configure(
    root: Path,
    build_dir: Path,
    *,
    toolchain: Toolchain,
    build_type: str,
    extra: Sequence[str] = (),
    env: Mapping[str, str] | None = None,
    dry_run: bool = False,
    step_label: str = "[2/4]",
) -> None:
    """Run the configure step, echoing the command first."""
    where = paths.display(root, build_dir)
    step(f"{step_label} Configuring ({build_type}) -> {where}")
    run_checked(
        configure_command(
            root,
            build_dir,
            toolchain=toolchain,
            build_type=build_type,
            extra=extra,
        ),
        what="cmake configure",
        env=env,
        dry_run=dry_run,
    )


def compile(
    build_dir: Path,
    *,
    toolchain: Toolchain,
    jobs: int,
    env: Mapping[str, str] | None = None,
    dry_run: bool = False,
    step_label: str = "[3/4]",
) -> None:
    """Run the build step, echoing the command first."""
    step(f"{step_label} Building ({jobs} jobs)...")
    run_checked(
        build_command(toolchain, build_dir, jobs),
        what="cmake build",
        env=env,
        dry_run=dry_run,
    )


def build_variant(
    root: Path,
    build_dir: Path,
    *,
    toolchain: Toolchain,
    build_type: str,
    jobs: int,
    extra: Sequence[str] = (),
    env: Mapping[str, str] | None = None,
    dry_run: bool = False,
) -> Path:
    """Configure and build one variant; return where the CLI should be."""
    configure(
        root,
        build_dir,
        toolchain=toolchain,
        build_type=build_type,
        extra=extra,
        env=env,
        dry_run=dry_run,
    )
    compile(
        build_dir,
        toolchain=toolchain,
        jobs=jobs,
        env=env,
        dry_run=dry_run,
    )
    return paths.cli_binary_path(build_dir)

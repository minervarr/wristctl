"""Command line: parse arguments, then hand off to exactly one workflow.

Exit codes match the rest of the project: ``0`` success, ``1`` a step failed,
``2`` the command line was wrong.
"""

from __future__ import annotations

import argparse
import os
import sys
from collections.abc import Sequence

from . import builder, menu, paths, toolchain, ui, variants
from .errors import BuildError, UsageError
from .process import run_checked

PROG = "wristctl-build"
COMMANDS = ("build", "test", "menu")
MENU_COMMANDS = ("", "menu")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog=PROG,
        description=(
            "Build and test wristctl through cmake + ninja. With no arguments "
            "and a terminal, a menu asks for the build type and "
            "microarchitecture; with no terminal it builds Debug."
        ),
    )
    parser.add_argument(
        "command",
        nargs="?",
        default=None,
        choices=COMMANDS,
        help="build one variant (default), build Debug and run the tests, or open the menu",
    )
    parser.add_argument(
        "--debug",
        dest="build_type",
        action="store_const",
        const="Debug",
        help="Debug build into build/linux_debug (default; tests included)",
    )
    parser.add_argument(
        "--release",
        dest="build_type",
        action="store_const",
        const="Release",
        help="Release build into build/linux (no tests)",
    )
    parser.add_argument(
        "--arch",
        default="",
        metavar="NAME",
        help="microarchitecture: native, v3, v4, znver4, ... (default: portable)",
    )
    parser.add_argument(
        "--ninja",
        default="",
        metavar="PATH",
        help="path to ninja (default: $WRISTCTL_NINJA, then PATH)",
    )
    gui = parser.add_mutually_exclusive_group()
    gui.add_argument(
        "--gui",
        dest="gui",
        action="store_true",
        help="build the desktop GUI (needs the Vulkan SDK, slangc/glslc and Wayland/xkb)",
    )
    gui.add_argument(
        "--no-gui",
        dest="gui",
        action="store_false",
        help="skip the desktop GUI even when its prerequisites are present",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the cmake commands and run nothing (works without a toolchain)",
    )
    parser.add_argument(
        "--version",
        action="store_true",
        help="print the project version from manifest.json",
    )
    parser.set_defaults(build_type=None, gui=None)
    return parser


def _passthrough(parser: argparse.ArgumentParser, extra: Sequence[str]) -> list[str]:
    """Extra arguments become extra cmake ``-D`` cache entries."""
    rest = list(extra)
    if rest[:1] == ["--"]:
        rest = rest[1:]
    for arg in rest:
        if not arg.startswith("-D"):
            parser.error(
                f"unrecognised argument: {arg} "
                "(extra cmake arguments must be written as -D<VAR>=<value>)"
            )
    return rest


def _version_or_unknown() -> str:
    try:
        return paths.version(paths.find_repo_root())
    except BuildError:
        return "unknown (run inside the wristctl checkout)"


def _resolve_command(
    args: argparse.Namespace, *, reader=input
) -> tuple[str, str, str]:
    """Return ``(command, build_type, arch)``, asking the menu when none was given."""
    command = args.command or ""
    build_type = args.build_type or "Debug"
    arch = variants.sanitize_arch(args.arch) if args.arch else ""

    if command in MENU_COMMANDS:
        if command == "menu" and not sys.stdin.isatty():
            raise UsageError("`menu` needs a terminal: use `build` or `test`")
        if not sys.stdin.isatty():
            command = "build"
            ui.note("    no terminal: building Debug (see --help for the other modes)")
        else:
            arch = menu.choose_arch(reader=reader)
            build_type = menu.choose_build_type(reader=reader)
            command = "build"

    if command == "test" and build_type == "Release":
        raise UsageError("tests are Debug-only: drop --release")
    return command, build_type, arch


def _resolve_gui(
    args: argparse.Namespace, root, *, reader=input
) -> bool:
    """Whether this build includes the desktop GUI.

    An explicit flag wins. Otherwise the builder probes for the GUI's build
    prerequisites and asks in the menu — a machine without the Vulkan SDK must
    still be able to build the core, and a machine with it should not need to
    know that a flag exists.
    """
    if args.gui is not None:
        if args.gui:
            ui.note("    gui:   on (desktop frontend, --no-gui skips it)")
        else:
            ui.note("    gui:   off (explicit --no-gui)")
        return bool(args.gui)

    available, reason = toolchain.gui_prerequisites(root)
    interactive_menu = (
        sys.stdin.isatty() and (args.command or "") in MENU_COMMANDS
    )
    if interactive_menu:
        chosen = menu.confirm(
            "Build the desktop GUI?", default=available, reader=reader
        )
    else:
        chosen = available

    if chosen:
        ui.note("    gui:   on (desktop frontend, --no-gui skips it)")
        if not available:
            ui.warn(f"GUI requested but {reason}; the configure step will fail")
    else:
        ui.note(f"    gui:   off ({reason}; --gui overrides)")
    return chosen


def _generate(root, *, dry_run: bool) -> None:
    """Regenerate the committed codecs before a test run, like `make test`."""
    ui.step("[1/5] Generating codecs...")
    gen = root / "libs" / "firstparty" / "wristctl_core" / "gen"
    steps = (
        ("extract_models.py", ["--check"]),
        ("gen_py.py", []),
        ("gen_c.py", []),
        ("gen_models.py", []),
        ("gen_vectors.py", []),
    )
    for script, extra in steps:
        run_checked(
            [sys.executable, str(gen / script), *extra],
            what=f"generate ({script})",
            cwd=str(root),
            dry_run=dry_run,
        )


def _run_tests(
    root,
    build_dir,
    *,
    tools: toolchain.Toolchain,
    env: dict[str, str],
    dry_run: bool,
) -> None:
    ui.step("[5/5] Running tests...")
    run_checked(
        [tools.ctest, "--test-dir", str(build_dir), "--output-on-failure"],
        what="ctest",
        env=env,
        cwd=str(root),
        dry_run=dry_run,
    )
    python_env = dict(env)
    python_env["PYTHONPATH"] = str(root / "py")
    run_checked(
        [sys.executable, "-m", "unittest", "discover", "-s", "py/tests"],
        what="python tests",
        env=python_env,
        cwd=str(root),
        dry_run=dry_run,
    )
    if dry_run:
        ui.note("\nDry run: tests not executed.")
    else:
        ui.ok("\nTests passed.")


def run_command(args: argparse.Namespace, extra: Sequence[str]) -> int:
    """Execute one parsed command."""
    paths.current_target()  # Linux-only for now; raises on anything else.
    root = paths.find_repo_root()
    command, build_type, arch = _resolve_command(args)
    debug = build_type == "Debug"
    testing = command == "test"
    total = 5 if testing else 3

    env = dict(os.environ)
    if testing:
        _generate(root, dry_run=args.dry_run)

    ui.step(f"[{2 if testing else 1}/{total}] Checking toolchain...")
    tools = toolchain.discover(ninja=args.ninja or None, dry_run=args.dry_run)
    jobs = toolchain.jobs(env)

    gui = _resolve_gui(args, root)

    configure_extra = [
        f"-DWRISTCTL_BUILD_TESTS={'ON' if debug else 'OFF'}",
        f"-DWRISTCTL_BUILD_GUI={'ON' if gui else 'OFF'}",
        *variants.cmake_args(arch),
        *extra,
    ]

    build_path = paths.build_dir(root, arch=arch, debug=debug)
    builder.configure(
        root,
        build_path,
        toolchain=tools,
        build_type=build_type,
        extra=configure_extra,
        env=env,
        dry_run=args.dry_run,
        step_label=f"[{3 if testing else 2}/{total}]",
    )
    builder.compile(
        build_path,
        toolchain=tools,
        jobs=jobs,
        env=env,
        dry_run=args.dry_run,
        step_label=f"[{4 if testing else 3}/{total}]",
    )

    binary = paths.cli_binary_path(build_path)
    suffix = " (dry run)" if args.dry_run else ""
    ui.ok(f"\nDone: {paths.display(root, binary)}{suffix}")
    if gui:
        ui.note(f"  gui:  {paths.display(root, paths.gui_binary_path(build_path))}")
    if debug:
        ui.note(
            f"  test: ctest --test-dir {paths.display(root, build_path)} "
            "--output-on-failure"
        )

    if command == "test":
        _run_tests(
            root,
            build_path,
            tools=tools,
            env=env,
            dry_run=args.dry_run,
        )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args, extra = parser.parse_known_args(None if argv is None else list(argv))
    try:
        passthrough = _passthrough(parser, extra)
        if args.version:
            print(f"{PROG} {_version_or_unknown()}")
            return 0
        return run_command(args, passthrough)
    except UsageError as exc:
        ui.fail(str(exc))
        return 2
    except BuildError as exc:
        ui.fail(str(exc))
        return 1
    except KeyboardInterrupt:
        ui.fail("interrupted")
        return 130

"""Subprocess plumbing: one place that echoes a command and runs it."""

from __future__ import annotations

import subprocess
from collections.abc import Mapping, Sequence

from .errors import BuildError
from .ui import step


def describe(cmd: Sequence[str]) -> str:
    """A copy-pasteable rendering of *cmd* for the console."""
    return " ".join(f'"{part}"' if " " in part else part for part in cmd)


def run(
    cmd: Sequence[str],
    *,
    env: Mapping[str, str] | None = None,
    cwd: str | None = None,
    capture: bool = False,
    dry_run: bool = False,
    echo: bool = True,
) -> subprocess.CompletedProcess[str]:
    """Run *cmd* and report it before spawning, unless *echo* is off.

    Never raises on a non-zero exit: callers decide what a failure means. With
    *dry_run* the command is printed and success is faked.
    """
    if echo:
        step("$ " + describe(cmd))
    if dry_run:
        return subprocess.CompletedProcess(list(cmd), 0, "", "")
    return subprocess.run(
        list(cmd),
        env=dict(env) if env is not None else None,
        cwd=cwd,
        text=True,
        capture_output=capture,
        check=False,
    )


def run_checked(
    cmd: Sequence[str],
    *,
    what: str,
    env: Mapping[str, str] | None = None,
    cwd: str | None = None,
    capture: bool = False,
    dry_run: bool = False,
    echo: bool = True,
) -> subprocess.CompletedProcess[str]:
    """Like :func:`run`, but a non-zero exit becomes a :class:`BuildError`.

    The tool's own stderr is carried into the message, so a cmake error is not
    swallowed behind a bare exit code.
    """
    result = run(cmd, env=env, cwd=cwd, capture=capture, dry_run=dry_run, echo=echo)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise BuildError(
            f"{what} failed (exit {result.returncode})"
            + (f"\n{detail}" if detail else "")
        )
    return result

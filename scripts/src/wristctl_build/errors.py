"""Exceptions shared by every module.

Two levels, because they exit differently: a :class:`UsageError` means the
command line was wrong (exit 2), anything else is a failed step (exit 1).
"""

from __future__ import annotations


class BuildError(RuntimeError):
    """A step could not run or failed."""


class UsageError(BuildError):
    """The requested command does not make sense."""

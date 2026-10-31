"""Allow ``python -m wristctl_build`` as well as the zipapp entry point."""

from __future__ import annotations

import sys

from .cli import main

if __name__ == "__main__":
    sys.exit(main())

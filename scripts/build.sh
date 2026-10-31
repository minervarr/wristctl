#!/usr/bin/env bash
# Linux entry point for wristctl's build tooling: a thin shim, so
# `scripts/build.sh` keeps working for muscle memory and manifest.json. It
# prefers python3 (the zipapp is stdlib-only), then uv as a fallback. The real
# work lives in wristctl_builder.pyz, built by build_pyz.py if needed.
#
# Everything after this script's name is passed straight to the builder, e.g.
#   ./scripts/build.sh                  # menu, or Debug when not a terminal
#   ./scripts/build.sh build --release  # build/linux
#   ./scripts/build.sh test             # build/linux_debug + ctest + Python
#   ./scripts/build.sh build --dry-run
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

if command -v python3 >/dev/null 2>&1; then
    exec python3 build_pyz.py --run "$@"
fi
if command -v uv >/dev/null 2>&1; then
    exec uv run --no-project python build_pyz.py --run "$@"
fi

echo "error: neither python3 nor uv is on PATH" >&2
echo "       install Python 3.10+: https://www.python.org/ (or uv: https://docs.astral.sh/uv/)" >&2
exit 1

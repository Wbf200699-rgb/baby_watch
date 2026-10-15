#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV="${RA8P1_RUHMI_VENV:-$HOME/.venvs/ra8p1_ruhmi_mera}"
BOOTSTRAP_VENV="${RA8P1_RUHMI_BOOTSTRAP_VENV:-$HOME/.venvs/ra8p1_ruhmi_bootstrap}"
shopt -s nullglob
WHEELS=("$ROOT"/data/mera-*-cp310-cp310-manylinux_2_27_x86_64.whl)
shopt -u nullglob
if [[ ${#WHEELS[@]} -ne 1 ]]; then
  echo "Expected exactly one Linux CPython 3.10 MERA wheel in $ROOT/data; found ${#WHEELS[@]}." >&2
  exit 2
fi

PYTHON310="$(command -v python3.10 || true)"
if [[ -z "$PYTHON310" ]]; then
  # Ubuntu 24.04 no longer packages CPython 3.10, while the official current
  # RUHMI/MERA wheel is intentionally built for cp310.  Use uv only to install
  # that exact interpreter; do not replace or downgrade the distro Python.
  python3 -m venv "$BOOTSTRAP_VENV"
  "$BOOTSTRAP_VENV/bin/python" -m pip install --upgrade pip uv
  "$BOOTSTRAP_VENV/bin/uv" python install 3.10
  PYTHON310="$("$BOOTSTRAP_VENV/bin/uv" python find 3.10)"
fi

"$PYTHON310" -m venv "$VENV"
"$VENV/bin/python" -m pip install --upgrade pip
"$VENV/bin/python" -m pip install \
  attrs \
  cmake \
  decorator \
  junitparser \
  numpy==1.26.4 \
  psutil \
  pybind11 \
  pyyaml \
  typing_extensions
"$VENV/bin/python" -m pip install "${WHEELS[0]}"
"$VENV/bin/python" -c \
  "import mera; print('RUHMI/MERA compiler:', getattr(mera, '__version__', 'installed'))"
echo "RUHMI/MERA environment ready: $VENV"

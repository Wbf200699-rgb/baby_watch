#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV="${RA8P1_CONVERT_VENV:-$HOME/.venvs/ra8p1_vision_convert}"
if [[ ! -x "$VENV/bin/python" ]]; then
  echo "Missing WSL conversion environment. Run setup_conversion_wsl.ps1 first." >&2
  exit 2
fi
exec "$VENV/bin/python" "$ROOT/convert_yolox_int8.py" "$@"

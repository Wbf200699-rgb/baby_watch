#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV="${RA8P1_RUHMI_VENV:-$HOME/.venvs/ra8p1_ruhmi_mera}"
MODEL=""
OUTPUT=""
TARGET="npu"
SUFFIX=""
EXTERNAL=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --model) MODEL="$2"; shift 2 ;;
    --output) OUTPUT="$2"; shift 2 ;;
    --target) TARGET="$2"; shift 2 ;;
    --suffix) SUFFIX="$2"; shift 2 ;;
    --external) EXTERNAL=1; shift ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ ! -x "$VENV/bin/python" ]]; then
  echo "Missing RUHMI environment. Run setup_ruhmi_wsl.ps1 first." >&2
  exit 2
fi
if [[ ! -f "$MODEL" ]]; then
  echo "Model not found: $MODEL" >&2
  exit 2
fi
if [[ "$TARGET" != "npu" && "$TARGET" != "cpu" ]]; then
  echo "Target must be npu or cpu." >&2
  exit 2
fi
# MERA invokes the Vela console entry point through a child shell.  Calling the
# venv Python by absolute path does not activate that shell, so expose the venv
# executables explicitly.
export PATH="$VENV/bin:$PATH"

COMPILER="$ROOT/vendor/ruhmi-model-zoo/ruhmi_tools/mcu_compile.py"
if [[ ! -f "$COMPILER" ]]; then
  echo "RUHMI compiler driver not found: $COMPILER" >&2
  exit 2
fi
mkdir -p "$OUTPUT"
ARGS=("$COMPILER" "$MODEL" "$OUTPUT" "--$TARGET")
if [[ -n "$SUFFIX" ]]; then
  ARGS+=("--suffix" "$SUFFIX")
fi
if [[ $EXTERNAL -eq 1 ]]; then
  ARGS+=("--external")
fi
exec "$VENV/bin/python" "${ARGS[@]}"

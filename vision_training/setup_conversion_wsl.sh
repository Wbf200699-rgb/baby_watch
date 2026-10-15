#!/usr/bin/env bash
set -euo pipefail

VENV="${RA8P1_CONVERT_VENV:-$HOME/.venvs/ra8p1_vision_convert}"
python3 -m venv "$VENV"
"$VENV/bin/python" -m pip install --upgrade pip
"$VENV/bin/python" -m pip install \
  numpy==1.26.4 \
  onnx==1.19.1 \
  onnxruntime==1.23.0 \
  opencv-python-headless==4.11.0.86 \
  onnxsim==0.4.36 \
  onnxoptimizer==0.4.2 \
  tensorflow==2.19.0 \
  tf-keras==2.19.0 \
  onnx-graphsurgeon==0.5.8 \
  sng4onnx==2.0.1 \
  ai-edge-litert==2.1.2 \
  protobuf==4.25.5 \
  h5py==3.11.0 \
  ml-dtypes==0.5.1 \
  flatbuffers==25.12.19 \
  requests==2.32.5 \
  psutil==5.9.5
"$VENV/bin/python" -m pip install --no-deps onnx2tf==1.29.24
"$VENV/bin/python" -c \
  "import ai_edge_litert, onnx2tf, onnx_graphsurgeon, tensorflow; print('WSL conversion stack:', tensorflow.__version__)"
"$VENV/bin/onnxsim" --help >/dev/null
echo "WSL conversion environment ready: $VENV"

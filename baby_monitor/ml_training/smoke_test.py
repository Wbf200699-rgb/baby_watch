from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import tensorflow as tf
import yaml

from models import BUILDERS


ROOT = Path(__file__).resolve().parent


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", choices=BUILDERS, default="cry_detection")
    args = parser.parse_args()
    specs = yaml.safe_load((ROOT / "model_specs.yaml").read_text(encoding="utf-8"))["models"]
    spec = specs[args.model]
    shape = tuple(spec["input_shape"])
    classes = len(spec["classes"])
    model = BUILDERS[args.model](shape, classes)
    output = model(np.zeros((1, *shape), dtype=np.float32), training=False).numpy()
    assert output.shape == (1, classes)
    assert np.isfinite(output).all()
    print(f"{model.name}: params={model.count_params():,}, output={output.shape}")


if __name__ == "__main__":
    main()

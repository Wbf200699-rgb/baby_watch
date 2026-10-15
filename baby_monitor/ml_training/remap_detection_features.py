from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent


def parse_float_array(header: str, name: str) -> np.ndarray:
    match = re.search(
        rf"{re.escape(name)}\s*\[[^\]]+\]\s*=\s*\{{(?P<body>.*?)\}};",
        header,
        re.DOTALL,
    )
    if not match:
        raise ValueError(f"could not find {name} in generated model header")
    values = [
        float(token.rstrip("fF"))
        for token in re.findall(
            r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?[fF]?",
            match.group("body"),
        )
    ]
    return np.asarray(values, dtype=np.float32)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Remap normalized MFCC packages to the frozen deployed detector contract."
    )
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--current-contract",
        type=Path,
        default=ROOT / "data" / "features" / "preprocessing_contract.json",
    )
    parser.add_argument(
        "--deployed-header",
        type=Path,
        default=(
            ROOT.parent
            / "Multicore_top_CPU0"
            / "src"
            / "thread_code"
            / "audio_cry_model_data.h"
        ),
    )
    args = parser.parse_args()

    contract = json.loads(args.current_contract.read_text(encoding="utf-8"))
    current_mean = np.asarray(contract["coefficient_mean"], dtype=np.float32)
    current_std = np.asarray(contract["coefficient_std"], dtype=np.float32)
    header = args.deployed_header.read_text(encoding="utf-8")
    deployed_mean = parse_float_array(header, "g_cry_coefficient_mean")
    deployed_std = parse_float_array(header, "g_cry_coefficient_std")
    if (
        current_mean.shape != deployed_mean.shape
        or current_std.shape != deployed_std.shape
    ):
        raise ValueError("coefficient contracts have different shapes")

    with np.load(args.input) as data:
        arrays = {name: np.asarray(data[name]) for name in data.files}
    x = np.asarray(arrays["x"], dtype=np.float32)
    shape = (1,) * (x.ndim - 2) + (len(current_mean), 1)
    raw = x * current_std.reshape(shape) + current_mean.reshape(shape)
    arrays["x"] = (
        (raw - deployed_mean.reshape(shape)) / deployed_std.reshape(shape)
    ).astype(np.float32)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.output, **arrays)
    print(
        json.dumps(
            {
                "input": str(args.input),
                "output": str(args.output),
                "windows": int(len(x)),
                "maximum_abs_remapped": float(np.max(np.abs(arrays["x"]))),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()

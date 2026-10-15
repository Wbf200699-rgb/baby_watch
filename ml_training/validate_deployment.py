from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import tensorflow as tf
import yaml

from models import BUILDERS


ROOT = Path(__file__).resolve().parent


def convert(name: str, spec: dict, output_dir: Path) -> dict:
    shape = tuple(spec["input_shape"])
    model = BUILDERS[name](shape, len(spec["classes"]))
    rng = np.random.default_rng(20260712)
    representative = rng.normal(size=(32, *shape)).astype(np.float32)

    def samples():
        for sample in representative:
            yield [sample[np.newaxis, ...]]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = samples
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    payload = converter.convert()
    source = output_dir / f"{name}_int8.tflite"
    source.write_bytes(payload)

    interpreter = tf.lite.Interpreter(model_content=payload)
    interpreter.allocate_tensors()
    tensor_types = {item["dtype"] for item in interpreter.get_tensor_details()}
    forbidden = sorted(str(item) for item in tensor_types if item not in (np.int8, np.int32))
    if forbidden:
        raise RuntimeError(f"{name}: non-integer tensors: {forbidden}")
    if len(payload) > spec["max_tflite_bytes"]:
        raise RuntimeError(f"{name}: {len(payload)} bytes exceeds model budget")

    vela_dir = output_dir / f"{name}_vela"
    vela_dir.mkdir(exist_ok=True)
    python_dir = Path(sys.executable).parent
    vela_candidates = [
        python_dir / "vela.exe",
        python_dir / "Scripts" / "vela.exe",
    ]
    path_vela = shutil.which("vela")
    if path_vela:
        vela_candidates.append(Path(path_vela))
    vela = next((candidate for candidate in vela_candidates if candidate.exists()), None)
    if vela is None:
        raise RuntimeError("Vela executable was not found in the active Python environment")
    command = [
        str(vela), str(source),
        "--accelerator-config", "ethos-u55-256",
        "--optimise", "Performance",
        "--memory-mode", "Shared_Sram",
        "--system-config", "Ethos_U55_High_End_Embedded",
        "--output-dir", str(vela_dir),
        "--show-cpu-operations",
    ]
    run = subprocess.run(command, text=True, capture_output=True, check=False)
    log = run.stdout + run.stderr
    (vela_dir / "compile.log").write_text(log, encoding="utf-8")
    if run.returncode:
        raise RuntimeError(f"{name}: Vela failed; see {vela_dir / 'compile.log'}")
    cpu_match = re.search(r"CPU operators\s*=\s*(\d+)", log)
    sram_match = re.search(r"Total SRAM used\s+([0-9.]+)\s+KiB", log)
    if not cpu_match or int(cpu_match.group(1)) != 0:
        raise RuntimeError(f"{name}: Vela reported CPU fallback")
    if not sram_match:
        raise RuntimeError(f"{name}: Vela SRAM usage was not reported")
    sram_bytes = round(float(sram_match.group(1)) * 1024)
    if sram_bytes > spec["max_arena_bytes"]:
        raise RuntimeError(f"{name}: Vela SRAM {sram_bytes} exceeds arena budget")
    return {
        "model": name,
        "parameters": model.count_params(),
        "tflite_bytes": len(payload),
        "model_budget_bytes": spec["max_tflite_bytes"],
        "vela_compiled": True,
        "vela_cpu_operators": int(cpu_match.group(1)),
        "vela_sram_bytes": sram_bytes,
        "arena_budget_bytes": spec["max_arena_bytes"],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("models", nargs="*")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    specs = yaml.safe_load((ROOT / "model_specs.yaml").read_text(encoding="utf-8"))["models"]
    names = args.models or list(BUILDERS)
    unknown = set(names) - set(BUILDERS)
    if unknown:
        parser.error(f"unknown models: {', '.join(sorted(unknown))}")
    output_dir = (
        args.output_dir.resolve()
        if args.output_dir
        else ROOT / "artifacts" / "deployment_validation"
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    reports = [convert(name, specs[name], output_dir) for name in names]
    report_path = output_dir / "report.json"
    report_path.write_text(json.dumps(reports, indent=2), encoding="utf-8")
    print(json.dumps(reports, indent=2))


if __name__ == "__main__":
    main()

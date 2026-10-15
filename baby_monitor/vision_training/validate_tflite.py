from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import yaml


ROOT = Path(__file__).resolve().parent
MODEL_CONFIG_KEYS = {
    "scene": "scene_detector",
    "occlusion": "occlusion_classifier",
    "behavior": "behavior_classifier",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="验证视觉 TFLite 的尺寸、形状与 strict INT8 合约")
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--kind", required=True, choices=tuple(MODEL_CONFIG_KEYS))
    parser.add_argument("--report", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        import tensorflow as tf
    except ImportError as exc:
        raise SystemExit("缺少 TensorFlow；请先运行 setup.ps1") from exc

    config = yaml.safe_load((ROOT / "vision_models.yaml").read_text(encoding="utf-8"))
    spec = config[MODEL_CONFIG_KEYS[args.kind]]
    interpreter = tf.lite.Interpreter(model_path=str(args.model))
    interpreter.allocate_tensors()
    inputs = interpreter.get_input_details()
    outputs = interpreter.get_output_details()
    if len(inputs) != 1 or len(outputs) != 1:
        raise SystemExit(f"期望单输入单输出，实际 inputs={len(inputs)} outputs={len(outputs)}")
    input_detail = inputs[0]
    output_detail = outputs[0]
    expected_input = [1, *spec["input_shape"]]
    failures: list[str] = []
    if input_detail["shape"].tolist() != expected_input:
        failures.append(f"输入形状 {input_detail['shape'].tolist()} != {expected_input}")
    if input_detail["dtype"] != np.int8:
        failures.append(f"输入 dtype {input_detail['dtype']} 不是 int8")
    if output_detail["dtype"] != np.int8:
        failures.append(f"输出 dtype {output_detail['dtype']} 不是 int8")
    if args.kind != "scene":
        expected_output = [1, len(spec["classes"])]
        if output_detail["shape"].tolist() != expected_output:
            failures.append(f"输出形状 {output_detail['shape'].tolist()} != {expected_output}")
    else:
        expected_output = list(spec["output_shape"])
        if output_detail["shape"].tolist() != expected_output:
            failures.append(f"输出形状 {output_detail['shape'].tolist()} != {expected_output}")
    if args.model.stat().st_size > int(spec["max_tflite_bytes"]):
        failures.append(
            f"模型 {args.model.stat().st_size} bytes 超过预算 {spec['max_tflite_bytes']}"
        )

    input_scale, input_zero = input_detail["quantization"]
    if input_scale <= 0:
        failures.append("输入量化 scale 非正数")
    sample = np.full(input_detail["shape"], input_zero, dtype=np.int8)
    interpreter.set_tensor(input_detail["index"], sample)
    interpreter.invoke()
    output = interpreter.get_tensor(output_detail["index"])
    operators = [
        detail.get("op_name", "unknown")
        for detail in getattr(interpreter, "_get_ops_details", lambda: [])()
    ]
    report = {
        "model": str(args.model.resolve()),
        "kind": args.kind,
        "bytes": args.model.stat().st_size,
        "input": {
            "name": input_detail["name"],
            "shape": input_detail["shape"].tolist(),
            "dtype": str(input_detail["dtype"]),
            "quantization": [float(input_scale), int(input_zero)],
        },
        "output": {
            "name": output_detail["name"],
            "shape": output_detail["shape"].tolist(),
            "dtype": str(output_detail["dtype"]),
            "quantization": [
                float(output_detail["quantization"][0]),
                int(output_detail["quantization"][1]),
            ],
            "smoke_min": int(output.min()),
            "smoke_max": int(output.max()),
        },
        "operators": operators,
        "failures": failures,
        "ruhmi_compile_still_required": True,
    }
    report_path = args.report or args.model.with_suffix(".validation.json")
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    if failures:
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 2
    print(f"strict INT8 合约通过: {args.model}")
    print(f"报告: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

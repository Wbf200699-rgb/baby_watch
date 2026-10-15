from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from train_mobilenetv3 import build_model, require_tensorflow


ROOT = Path(__file__).resolve().parent


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="构建并 strict-INT8 转换一个随机权重 MobileNetV3，验证本机工具链"
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "artifacts" / "smoke" / "mobilenetv3_small_192_int8.tflite",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    tf = require_tensorflow()
    tf.keras.utils.set_random_seed(20260725)
    model, _ = build_model(tf, class_count=4, imagenet=False)
    sample = np.zeros((1, 192, 192, 3), dtype=np.float32)
    float_output = model(sample, training=False).numpy()
    if float_output.shape != (1, 4):
        raise SystemExit(f"浮点输出形状异常: {float_output.shape}")

    def representative_dataset():
        for value in np.linspace(0.0, 255.0, num=16, dtype=np.float32):
            yield [np.full((1, 192, 192, 3), value, dtype=np.float32)]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(converter.convert())

    interpreter = tf.lite.Interpreter(model_path=str(args.output))
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    failures: list[str] = []
    if input_detail["shape"].tolist() != [1, 192, 192, 3]:
        failures.append(f"输入形状异常: {input_detail['shape'].tolist()}")
    if output_detail["shape"].tolist() != [1, 4]:
        failures.append(f"输出形状异常: {output_detail['shape'].tolist()}")
    if input_detail["dtype"] != np.int8 or output_detail["dtype"] != np.int8:
        failures.append("输入或输出不是 strict int8")
    interpreter.set_tensor(
        input_detail["index"],
        np.full(input_detail["shape"], input_detail["quantization"][1], dtype=np.int8),
    )
    interpreter.invoke()
    output = interpreter.get_tensor(output_detail["index"])
    report = {
        "tensorflow": tf.__version__,
        "model": str(args.output.resolve()),
        "bytes": args.output.stat().st_size,
        "input_shape": input_detail["shape"].tolist(),
        "input_quantization": list(input_detail["quantization"]),
        "output_shape": output_detail["shape"].tolist(),
        "output_quantization": list(output_detail["quantization"]),
        "output_min": int(output.min()),
        "output_max": int(output.max()),
        "failures": failures,
        "note": "Random weights; this validates the build/quantization stack, not accuracy.",
    }
    report_path = args.output.with_suffix(".json")
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    if failures:
        raise SystemExit("; ".join(failures))
    print(f"MobileNetV3 strict INT8 冒烟测试通过: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import tensorflow as tf
import yaml
from sklearn.metrics import accuracy_score, f1_score

from models import BUILDERS


ROOT = Path(__file__).resolve().parent


def evaluate_int8(
    payload: bytes,
    dataset: Path,
    classes: list[str],
    float_model_path: Path,
) -> dict:
    with np.load(dataset) as data:
        x = np.asarray(data["x"], dtype=np.float32)
        y = np.asarray(data["y"], dtype=np.int32)
    interpreter = tf.lite.Interpreter(model_content=payload)
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    input_scale, input_zero = input_detail["quantization"]
    output_scale, output_zero = output_detail["quantization"]
    if input_scale <= 0 or output_scale <= 0:
        raise RuntimeError("INT8 model is missing input/output quantization scales")

    predictions: list[int] = []
    for sample in x:
        quantized = np.clip(
            np.rint(sample / input_scale + input_zero), -128, 127
        ).astype(np.int8)[np.newaxis, ...]
        interpreter.set_tensor(input_detail["index"], quantized)
        interpreter.invoke()
        output = interpreter.get_tensor(output_detail["index"])[0]
        dequantized = (output.astype(np.float32) - output_zero) * output_scale
        predictions.append(int(np.argmax(dequantized)))

    float_model = tf.keras.models.load_model(float_model_path)
    float_predictions = float_model.predict(x, batch_size=256, verbose=0).argmax(axis=1)
    int8_accuracy = float(accuracy_score(y, predictions))
    float_accuracy = float(accuracy_score(y, float_predictions))
    return {
        "dataset": str(dataset),
        "samples": int(len(y)),
        "float_accuracy": float_accuracy,
        "int8_accuracy": int8_accuracy,
        "accuracy_drop": float(float_accuracy - int8_accuracy),
        "int8_macro_f1": float(
            f1_score(y, predictions, labels=np.arange(len(classes)), average="macro", zero_division=0)
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Export and gate a strict INT8 TFLite model for RA8P1/Ethos-U55"
    )
    parser.add_argument("model", choices=sorted(BUILDERS))
    parser.add_argument("--artifact-dir", type=Path)
    parser.add_argument("--test", type=Path, help="Optional held-out NPZ for INT8 accuracy gating")
    args = parser.parse_args()
    folder = (args.artifact_dir or ROOT / "artifacts" / args.model).resolve()
    samples = np.load(folder / "representative.npy").astype(np.float32)

    def representative_data():
        for sample in samples:
            yield [sample[np.newaxis, ...]]

    converter = tf.lite.TFLiteConverter.from_saved_model(str(folder / "saved_model"))
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_data
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    payload = converter.convert()
    output = folder / f"{args.model}_int8.tflite"
    output.write_bytes(payload)

    interpreter = tf.lite.Interpreter(model_content=payload)
    interpreter.allocate_tensors()
    tensors = interpreter.get_tensor_details()
    allowed_types = (np.int8, np.int32)
    non_integer = sorted(
        {str(item["dtype"]) for item in tensors if item["dtype"] not in allowed_types}
    )
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    with (ROOT / "model_specs.yaml").open(encoding="utf-8") as handle:
        spec = yaml.safe_load(handle)["models"][args.model]
    report = {
        "model": args.model,
        "bytes": len(payload),
        "budget_bytes": spec["max_tflite_bytes"],
        "within_model_budget": len(payload) <= spec["max_tflite_bytes"],
        "non_integer_tensor_types": non_integer,
        "input": input_detail["shape"].tolist(),
        "input_dtype": str(input_detail["dtype"]),
        "input_quantization": [float(input_detail["quantization"][0]), int(input_detail["quantization"][1])],
        "output": output_detail["shape"].tolist(),
        "output_dtype": str(output_detail["dtype"]),
        "output_quantization": [float(output_detail["quantization"][0]), int(output_detail["quantization"][1])],
    }
    if args.test:
        report["held_out_evaluation"] = evaluate_int8(
            payload, args.test, list(spec["classes"]), folder / "best.keras"
        )

    failures: list[str] = []
    if non_integer:
        failures.append(f"non-integer tensors: {non_integer}")
    if input_detail["dtype"] != np.int8 or output_detail["dtype"] != np.int8:
        failures.append("input/output tensors are not INT8")
    if not report["within_model_budget"]:
        failures.append("model exceeds flash budget")
    if args.test:
        maximum_drop = float(spec.get("max_int8_accuracy_drop", 1.0))
        if report["held_out_evaluation"]["accuracy_drop"] > maximum_drop:
            failures.append(
                f"INT8 accuracy drop exceeds {maximum_drop:.3f}"
            )
    report["passed"] = not failures
    report["failures"] = failures
    (folder / "int8_report.json").write_text(
        json.dumps(report, indent=2), encoding="utf-8"
    )
    print(json.dumps(report, indent=2))
    if failures:
        raise SystemExit(f"RA8P1 export gate failed: {failures}")


if __name__ == "__main__":
    main()

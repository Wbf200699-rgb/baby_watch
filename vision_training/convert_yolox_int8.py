from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent
IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="按 Renesas YOLOX 参考流程执行 ONNX→SavedModel→strict INT8 TFLite"
    )
    parser.add_argument("--onnx", required=True, type=Path)
    parser.add_argument(
        "--calibration-dir",
        type=Path,
        default=ROOT / "data" / "processed" / "scene_coco" / "train2017",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "artifacts" / "scene" / "baby_yolox_tiny_224_int8.tflite",
    )
    parser.add_argument("--samples", type=int, default=256)
    parser.add_argument("--keep-saved-model", action="store_true")
    parser.add_argument("--force", action="store_true", help="重建同名转换临时目录和输出")
    return parser.parse_args()


def preprocess(path: Path) -> np.ndarray | None:
    import cv2

    image = cv2.imread(str(path))
    if image is None:
        return None
    height, width = image.shape[:2]
    scale = min(224 / width, 224 / height)
    resized_width, resized_height = int(width * scale), int(height * scale)
    resized = cv2.resize(image, (resized_width, resized_height), interpolation=cv2.INTER_LINEAR)
    padded = np.full((224, 224, 3), 114, dtype=np.uint8)
    pad_x = (224 - resized_width) // 2
    pad_y = (224 - resized_height) // 2
    padded[pad_y : pad_y + resized_height, pad_x : pad_x + resized_width] = resized
    return padded.astype(np.float32)[None, ...]


def main() -> int:
    args = parse_args()
    if not args.onnx.is_file():
        raise SystemExit(f"找不到 ONNX: {args.onnx}")
    onnx_path = args.onnx.resolve()
    source_onnx_sha256 = sha256_file(onnx_path)
    calibration_dir = args.calibration_dir.resolve()
    output_path = args.output.resolve()
    images = sorted(
        path
        for path in calibration_dir.rglob("*")
        if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES
    )[: args.samples]
    if len(images) < 30:
        raise SystemExit(f"校准图像至少需要 30 张，当前 {len(images)}")

    try:
        import onnx2tf
        import tensorflow as tf
    except ImportError as exc:
        raise SystemExit(
            "缺少转换依赖。请先运行 setup_yolox.ps1；转换环境应与 Renesas "
            "ruhmi-model-zoo/yolox_tiny/python/requirements.txt 一致。"
        ) from exc

    output_path.parent.mkdir(parents=True, exist_ok=True)
    saved_model_dir = output_path.parent / f"{output_path.stem}_saved_model"
    if saved_model_dir.exists():
        if not args.force:
            raise SystemExit(f"转换临时目录已存在；确认后加 --force: {saved_model_dir}")
        shutil.rmtree(saved_model_dir)
    if output_path.exists() and not args.force:
        raise SystemExit(f"输出已存在；确认后加 --force: {output_path}")
    # onnx2tf looks for this fixed-name internal validation array in cwd.
    # Renesas' reference script creates it before conversion.  Use a local
    # temporary directory so failed network fallback content cannot poison the
    # conversion and the helper file never pollutes the repository.
    previous_cwd = Path.cwd()
    with tempfile.TemporaryDirectory(prefix="baby_yolox_onnx2tf_") as work_dir:
        work_model = Path(work_dir) / onnx_path.name
        # onnx2tf invokes onnxsim with the same input/output path.  Convert a
        # private copy so the trained ONNX artifact remains byte-for-byte
        # unchanged and can still be audited or compared later.
        shutil.copy2(onnx_path, work_model)
        np.save(
            Path(work_dir) / "calibration_image_sample_data_20x128x128x3_float32.npy",
            np.random.RandomState(0).rand(20, 128, 128, 3).astype(np.float32),
        )
        os.chdir(work_dir)
        try:
            onnx2tf.convert(
                input_onnx_file_path=str(work_model),
                output_folder_path=str(saved_model_dir),
                copy_onnx_input_output_names_to_tflite=True,
                # YOLOX Focus/Concat layout conversion requires the onnxsim
                # optimization pass.  The supported environment is installed
                # by setup_conversion_wsl.ps1.
                not_use_onnxsim=False,
                non_verbose=True,
            )
        finally:
            os.chdir(previous_cwd)
    source_onnx_sha256_after = sha256_file(onnx_path)
    if source_onnx_sha256_after != source_onnx_sha256:
        raise SystemExit(
            "转换过程意外修改了源 ONNX；已停止，"
            f"before={source_onnx_sha256} after={source_onnx_sha256_after}"
        )

    loaded = tf.saved_model.load(str(saved_model_dir))
    concrete = loaded.signatures["serving_default"]
    input_spec = list(concrete.structured_input_signature[1].values())[0]
    is_nhwc = len(input_spec.shape) == 4 and input_spec.shape[-1] == 3

    def representative_dataset():
        for image_path in images:
            blob = preprocess(image_path)
            if blob is None:
                continue
            if not is_nhwc:
                blob = blob.transpose(0, 3, 1, 2)
            yield [blob.astype(np.float32)]

    converter = tf.lite.TFLiteConverter.from_saved_model(str(saved_model_dir))
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    output_path.write_bytes(converter.convert())

    interpreter = tf.lite.Interpreter(model_path=str(output_path))
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    failures = []
    if input_detail["dtype"] != np.int8 or output_detail["dtype"] != np.int8:
        failures.append("输入或输出不是 strict int8")
    if input_detail["shape"].tolist() != [1, 224, 224, 3]:
        failures.append(f"输入形状异常: {input_detail['shape'].tolist()}")
    if output_detail["shape"].tolist() != [1, 1029, 10]:
        failures.append(f"输出形状异常: {output_detail['shape'].tolist()}")
    report = {
        "onnx": str(onnx_path),
        "source_onnx_sha256": source_onnx_sha256,
        "source_onnx_preserved": True,
        "tflite": str(output_path),
        "bytes": output_path.stat().st_size,
        "calibration_images": len(images),
        "calibration_color_order": "bgr",
        "onnxsim_used": True,
        "input_shape": input_detail["shape"].tolist(),
        "input_dtype": str(input_detail["dtype"]),
        "input_quantization": list(input_detail["quantization"]),
        "output_shape": output_detail["shape"].tolist(),
        "output_dtype": str(output_detail["dtype"]),
        "output_quantization": list(output_detail["quantization"]),
        "failures": failures,
        "next_step": "run validate_tflite.py, then RUHMI compile and on-device parity test",
    }
    output_path.with_suffix(".conversion.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    if not args.keep_saved_model:
        shutil.rmtree(saved_model_dir)
    if failures:
        raise SystemExit("; ".join(failures))
    print(f"strict INT8 TFLite: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="验证 YOLOX 自定义实验和 CUDA 单次前向")
    parser.add_argument("--cpu", action="store_true", help="即使 CUDA 可用也只使用 CPU")
    parser.add_argument(
        "--data-root",
        type=Path,
        default=ROOT / "data" / "processed" / "scene_coco",
    )
    parser.add_argument(
        "--export-onnx",
        action="store_true",
        help="另外导出随机权重 ONNX，验证 PyTorch 2.x 导出兼容性",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        import torch
    except ImportError as exc:
        raise SystemExit("缺少 PyTorch；请先运行 setup_yolox.ps1") from exc
    from yolox.data import COCODataset, ValTransform
    from yolox_baby_tiny_exp import Exp

    dataset = COCODataset(
        data_dir=str(args.data_root),
        json_file="instances_train2017.json",
        name="train2017",
        img_size=(224, 224),
        preproc=ValTransform(legacy=False),
    )
    expected_classes = (
        "infant",
        "head_face",
        "blanket",
        "soft_object",
        "caregiver",
    )
    if dataset._classes != expected_classes:
        raise SystemExit(f"COCO 类别顺序异常: {dataset._classes}")
    image, _target, _image_info, _image_id = dataset[0]
    if list(image.shape) != [3, 224, 224]:
        raise SystemExit(f"YOLOX 数据张量形状异常: {list(image.shape)}")

    use_cuda = torch.cuda.is_available() and not args.cpu
    device = torch.device("cuda" if use_cuda else "cpu")
    model = Exp().get_model().to(device).eval()
    sample = torch.zeros((1, 3, 224, 224), dtype=torch.float32, device=device)
    with torch.no_grad():
        output = model(sample)
    if not isinstance(output, torch.Tensor):
        raise SystemExit(f"YOLOX 输出类型异常: {type(output)}")
    shape = list(output.shape)
    if shape != [1, 1029, 10]:
        raise SystemExit(f"YOLOX 输出形状 {shape} != [1, 1029, 10]")
    report = {
        "torch": torch.__version__,
        "cuda_available": torch.cuda.is_available(),
        "device": str(device),
        "gpu": torch.cuda.get_device_name(0) if use_cuda else None,
        "input_shape": [1, 3, 224, 224],
        "output_shape": shape,
        "classes": 5,
        "dataset_images": len(dataset),
        "dataset_classes": list(dataset._classes),
        "note": "Random weights; this validates the custom experiment/CUDA stack, not accuracy.",
    }
    if args.export_onnx:
        from export_yolox_onnx import export_checkpoint

        checkpoint = ROOT / "artifacts" / "smoke" / "random_yolox_tiny.pth"
        onnx_path = ROOT / "artifacts" / "smoke" / "random_yolox_tiny_224.onnx"
        checkpoint.parent.mkdir(parents=True, exist_ok=True)
        torch.save({"model": model.to("cpu").state_dict()}, checkpoint)
        report["onnx_export"] = export_checkpoint(checkpoint, onnx_path)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

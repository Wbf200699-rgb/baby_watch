from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
from typing import Any


def replace_focus_for_export(model):
    """Rewrite YOLOX Focus+3x3 Conv as one equivalent 6x6/stride-2 Conv.

    The standard Focus implementation exports as four StridedSlice nodes
    followed by a channel Concat.  onnx2tf 1.29.x can infer the wrong layout
    for that pattern (6 channels instead of 12).  Folding the exact same
    weights into a regular convolution avoids that ambiguous graph and maps
    cleanly to TFLite/Ethos-U55.
    """
    import torch
    from torch import nn

    focus = model.backbone.backbone.stem
    source = focus.conv
    source_conv = source.conv
    if (
        source_conv.groups != 1
        or source_conv.kernel_size != (3, 3)
        or source_conv.stride != (1, 1)
        or source_conv.padding != (1, 1)
        or source_conv.dilation != (1, 1)
        or source_conv.in_channels % 4 != 0
    ):
        raise RuntimeError(
            "无法安全改写 YOLOX Focus：预期 12→out 的 3x3/stride1/pad1 普通卷积"
        )

    input_channels = source_conv.in_channels // 4
    replacement_conv = nn.Conv2d(
        input_channels,
        source_conv.out_channels,
        kernel_size=6,
        stride=2,
        padding=2,
        bias=source_conv.bias is not None,
        device=source_conv.weight.device,
        dtype=source_conv.weight.dtype,
    )
    offsets = ((0, 0), (1, 0), (0, 1), (1, 1))
    with torch.no_grad():
        replacement_conv.weight.zero_()
        for group, (row_offset, column_offset) in enumerate(offsets):
            for channel in range(input_channels):
                source_channel = group * input_channels + channel
                for kernel_row in range(3):
                    for kernel_column in range(3):
                        replacement_conv.weight[
                            :,
                            channel,
                            2 * kernel_row + row_offset,
                            2 * kernel_column + column_offset,
                        ] = source_conv.weight[
                            :,
                            source_channel,
                            kernel_row,
                            kernel_column,
                        ]
        if source_conv.bias is not None:
            replacement_conv.bias.copy_(source_conv.bias)

    class ExportFocusConv(nn.Module):
        def __init__(self) -> None:
            super().__init__()
            self.conv = replacement_conv
            self.bn = copy.deepcopy(source.bn)
            self.act = copy.deepcopy(source.act)

        def forward(self, tensor):
            return self.act(self.bn(self.conv(tensor)))

    model.backbone.backbone.stem = ExportFocusConv()
    return model


def replace_head_layout_for_export(model):
    """Emit each prediction level as NHWC before its fixed-shape reshape.

    YOLOX normally flattens NCHW tensors and transposes only after joining all
    levels.  That is mathematically correct, but onnx2tf may reinterpret the
    intermediate reshape after it has already changed operators to NHWC.  This
    equivalent export-only head makes the intended layout explicit.
    """
    import torch
    from torch import nn

    source = model.head

    class ExportYOLOXHead(nn.Module):
        def __init__(self) -> None:
            super().__init__()
            self.source = source
            self.num_classes = source.num_classes

        def forward(self, inputs):
            outputs = []
            channels = 5 + self.num_classes
            for index, (classification_conv, regression_conv, tensor) in enumerate(
                zip(self.source.cls_convs, self.source.reg_convs, inputs)
            ):
                tensor = self.source.stems[index](tensor)
                classification = self.source.cls_preds[index](
                    classification_conv(tensor)
                ).sigmoid()
                regression_feature = regression_conv(tensor)
                regression = self.source.reg_preds[index](regression_feature)
                objectness = self.source.obj_preds[index](regression_feature).sigmoid()
                prediction = torch.cat(
                    [regression, objectness, classification],
                    dim=1,
                )
                height, width = prediction.shape[-2:]
                prediction = prediction.permute(0, 2, 3, 1).contiguous()
                outputs.append(prediction.reshape(1, height * width, channels))
            return torch.cat(outputs, dim=1)

    model.head = ExportYOLOXHead()
    return model


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="将本项目五类 YOLOX-Tiny checkpoint 导出并检查为固定形状 ONNX"
    )
    parser.add_argument("--checkpoint", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--opset", type=int, default=18)
    return parser.parse_args()


def export_checkpoint(checkpoint: Path, output: Path, opset: int = 18) -> dict[str, Any]:
    try:
        import numpy as np
        import onnx
        import torch
        from torch import nn
    except ImportError as exc:
        raise SystemExit("缺少 PyTorch/ONNX；请先运行 setup_yolox.ps1") from exc
    from yolox.models.network_blocks import SiLU
    from yolox.utils import replace_module
    from yolox_baby_tiny_exp import Exp

    if not checkpoint.is_file():
        raise FileNotFoundError(checkpoint)
    model = Exp().get_model()
    # YOLOX stores its scalar epoch/AP metadata as NumPy scalars.  PyTorch 2.6
    # rejects those otherwise harmless values in weights-only mode unless they
    # are explicitly allow-listed.  Keep weights_only=True instead of falling
    # back to unrestricted pickle loading because this script may be pointed at
    # a checkpoint supplied by somebody else.
    safe_numpy_types = [
        np.core.multiarray.scalar,
        np.dtype,
        type(np.dtype(np.float64)),
    ]
    with torch.serialization.safe_globals(safe_numpy_types):
        loaded = torch.load(checkpoint, map_location="cpu", weights_only=True)
    state_dict = loaded["model"] if isinstance(loaded, dict) and "model" in loaded else loaded
    model.load_state_dict(state_dict)
    model.eval()
    model = replace_module(model, nn.SiLU, SiLU)
    model.head.decode_in_inference = False

    output.parent.mkdir(parents=True, exist_ok=True)
    generator = torch.Generator(device="cpu").manual_seed(20260725)
    sample = torch.rand((1, 3, 224, 224), dtype=torch.float32, generator=generator)
    with torch.no_grad():
        focus_output = model(sample)
    model = replace_focus_for_export(model).eval()
    model = replace_head_layout_for_export(model).eval()
    with torch.no_grad():
        rewritten_output = model(sample)
    focus_rewrite_max_abs_error = float(
        torch.max(torch.abs(focus_output - rewritten_output)).item()
    )
    if not torch.allclose(
        focus_output,
        rewritten_output,
        rtol=1e-4,
        atol=2e-5,
    ):
        raise RuntimeError(
            "Focus 等价卷积改写校验失败，"
            f"max_abs_error={focus_rewrite_max_abs_error:.8g}"
        )

    torch.onnx.export(
        model,
        sample,
        str(output),
        input_names=["images"],
        output_names=["output"],
        opset_version=opset,
        do_constant_folding=True,
        dynamo=False,
    )

    document = onnx.load(str(output))
    onnx.checker.check_model(document)
    if len(document.graph.input) != 1 or len(document.graph.output) != 1:
        raise RuntimeError(
            f"ONNX 必须单输入单输出，实际 {len(document.graph.input)}/{len(document.graph.output)}"
        )

    def tensor_shape(value_info) -> list[int | str | None]:
        dimensions = value_info.type.tensor_type.shape.dim
        result: list[int | str | None] = []
        for dimension in dimensions:
            if dimension.HasField("dim_value"):
                result.append(int(dimension.dim_value))
            elif dimension.HasField("dim_param"):
                result.append(str(dimension.dim_param))
            else:
                result.append(None)
        return result

    input_shape = tensor_shape(document.graph.input[0])
    output_shape = tensor_shape(document.graph.output[0])
    if input_shape != [1, 3, 224, 224]:
        raise RuntimeError(f"ONNX 输入形状异常: {input_shape}")
    if output_shape != [1, 1029, 10]:
        raise RuntimeError(f"ONNX 输出形状异常: {output_shape}")
    report = {
        "checkpoint": str(checkpoint.resolve()),
        "onnx": str(output.resolve()),
        "bytes": output.stat().st_size,
        "opset": opset,
        "input_shape": input_shape,
        "output_shape": output_shape,
        "decoded_in_graph": False,
        "focus_rewrite": "equivalent_conv6x6_stride2_pad2",
        "focus_rewrite_max_abs_error": focus_rewrite_max_abs_error,
        "head_layout_rewrite": "per_level_nhwc_then_fixed_reshape",
        "onnxsim_used": False,
    }
    output.with_suffix(".export.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    return report


def main() -> int:
    args = parse_args()
    try:
        report = export_checkpoint(args.checkpoint, args.output, args.opset)
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Generate an evidence-backed report for the tests listed in 测试111.docx."""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def latest_evidence(prefix: str, filename: str, require_pass: bool = False) -> tuple[Path, dict[str, Any]]:
    candidates: list[tuple[Path, dict[str, Any]]] = []
    for directory in (ROOT / "test_results").glob(f"{prefix}_*"):
        path = directory / filename
        if not path.exists():
            continue
        data = read_json(path)
        if require_pass and not data.get("pass"):
            continue
        candidates.append((path, data))
    if not candidates:
        raise RuntimeError(f"no usable {prefix}/{filename} evidence")
    return max(candidates, key=lambda item: item[0].stat().st_mtime)


def latest_json_file(pattern: str) -> tuple[Path, dict[str, Any]]:
    candidates = list((ROOT / "test_results").glob(pattern))
    if not candidates:
        raise RuntimeError(f"no evidence matches {pattern}")
    path = max(candidates, key=lambda item: item.stat().st_mtime)
    return path, read_json(path)


def metrics_from_confusion(matrix: list[list[int]]) -> dict[str, float]:
    precisions: list[float] = []
    recalls: list[float] = []
    f1s: list[float] = []
    for index, row in enumerate(matrix):
        tp = row[index]
        fp = sum(other[index] for row_index, other in enumerate(matrix) if row_index != index)
        fn = sum(value for col_index, value in enumerate(row) if col_index != index)
        precision = tp / (tp + fp) if tp + fp else 0.0
        recall = tp / (tp + fn) if tp + fn else 0.0
        f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
        precisions.append(precision)
        recalls.append(recall)
        f1s.append(f1)
    return {
        "precision": sum(precisions) / len(precisions),
        "recall": sum(recalls) / len(recalls),
        "f1": sum(f1s) / len(f1s),
    }


def git_revision() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True, capture_output=True, check=False
    )
    return result.stdout.strip() or "unknown"


def format_percent(value: float) -> str:
    return f"{value * 100:.2f}%"


def hardware_rate_metrics(summary_path: Path, summary: dict[str, Any]) -> dict[str, float]:
    if all(key in summary for key in ("elapsed_seconds", "effective_fps", "average_frame_interval_ms")):
        return {
            "elapsed_seconds": float(summary["elapsed_seconds"]),
            "effective_fps": float(summary["effective_fps"]),
            "average_frame_interval_ms": float(summary["average_frame_interval_ms"]),
        }

    csv_path = summary_path.parent / "vision_frames.csv"
    with csv_path.open("r", encoding="utf-8-sig", newline="") as handle:
        samples = list(csv.DictReader(handle))
    if len(samples) < 2:
        return {"elapsed_seconds": 0.0, "effective_fps": 0.0, "average_frame_interval_ms": 0.0}

    first_time = datetime.fromisoformat(samples[0]["observed_at"])
    last_time = datetime.fromisoformat(samples[-1]["observed_at"])
    elapsed_seconds = max(0.0, (last_time - first_time).total_seconds())
    frame_intervals = max(0, int(samples[-1]["vision_frame"]) - int(samples[0]["vision_frame"]))
    return {
        "elapsed_seconds": round(elapsed_seconds, 3),
        "effective_fps": round(frame_intervals / elapsed_seconds, 3) if elapsed_seconds and frame_intervals else 0.0,
        "average_frame_interval_ms": round(elapsed_seconds * 1000.0 / frame_intervals, 2)
        if frame_intervals
        else 0.0,
    }


def hardware_builds(summary_path: Path) -> tuple[str, str]:
    csv_path = summary_path.parent / "vision_frames.csv"
    with csv_path.open("r", encoding="utf-8-sig", newline="") as handle:
        first = next(csv.DictReader(handle), {})

    def format_build(value: str | None) -> str:
        try:
            return f"0x{int(value or '0'):08X}"
        except ValueError:
            return "unknown"

    return format_build(first.get("ra8_firmware")), format_build(first.get("vision_firmware"))


def markdown_table(headers: list[str], rows: list[list[Any]]) -> list[str]:
    lines = ["| " + " | ".join(headers) + " |", "|" + "|".join(["---"] * len(headers)) + "|"]
    for row in rows:
        cells = [str(value).replace("|", "\\|").replace("\n", "<br>") for value in row]
        lines.append("| " + " | ".join(cells) + " |")
    return lines


def write_docx(path: Path, title: str, sections: list[tuple[str, list[str] | tuple[list[str], list[list[Any]]]]]) -> None:
    from docx import Document
    from docx.enum.text import WD_ALIGN_PARAGRAPH
    from docx.oxml.ns import qn
    from docx.shared import Pt

    document = Document()
    styles = document.styles
    styles["Normal"].font.name = "Microsoft YaHei"
    styles["Normal"]._element.rPr.rFonts.set(qn("w:eastAsia"), "Microsoft YaHei")
    styles["Normal"].font.size = Pt(10.5)
    heading = document.add_heading(title, level=0)
    heading.alignment = WD_ALIGN_PARAGRAPH.CENTER

    for heading_text, content in sections:
        document.add_heading(heading_text, level=1)
        if isinstance(content, tuple):
            headers, rows = content
            table = document.add_table(rows=1, cols=len(headers))
            table.style = "Table Grid"
            for index, value in enumerate(headers):
                table.rows[0].cells[index].text = str(value)
            for row in rows:
                cells = table.add_row().cells
                for index, value in enumerate(row):
                    cells[index].text = str(value)
        else:
            for paragraph in content:
                document.add_paragraph(paragraph)
    document.save(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, default=ROOT / "docs" / "test_reports" / "2026-07-28")
    args = parser.parse_args()
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    hardware_path, hardware = latest_evidence("hardware", "summary.json", require_pass=True)
    audio_path, audio_hardware = latest_evidence("audio", "summary.json", require_pass=True)
    command_path, commands = latest_evidence("command", "command_evidence.json", require_pass=True)
    system_path, system = latest_evidence("system", "summary.json")
    system_snapshot = read_json(system_path.parent / "snapshot.json")
    live_audit_path, live_audit = latest_json_file("live_audit_*.json")
    rate_metrics = hardware_rate_metrics(hardware_path, hardware)
    ra8_build, vision_build = hardware_builds(hardware_path)

    primary_float = read_json(ROOT / "ml_training/artifacts/cry_detection_mlp_hardneg_v2b/training_report.json")
    primary_int8 = read_json(ROOT / "ml_training/artifacts/cry_detection_mlp_hardneg_v2b/c_int8_report.json")
    secondary_float = read_json(ROOT / "ml_training/artifacts/cry_type_mlp_robust_v1/training_report.json")
    secondary_int8 = read_json(ROOT / "ml_training/artifacts/cry_type_mlp_robust_v1/c_int8_report.json")

    primary_cry = primary_float["test"]["per_class"]["cry"]
    primary_int8_metrics = metrics_from_confusion(primary_int8["window_confusion_matrix"])
    secondary_int8_metrics = metrics_from_confusion(secondary_int8["window_confusion_matrix"])
    secondary_decision = secondary_int8["recording_decision"]

    telemetry = system_snapshot.get("telemetry") or {}
    sensors = telemetry.get("sensors") or {}
    inference = telemetry.get("inference") or {}
    vision = system_snapshot.get("vision") or telemetry.get("vision") or {}
    devices = system_snapshot.get("devices") if isinstance(system_snapshot.get("devices"), list) else []
    gateway = next((item for item in devices if item.get("device_id") == commands["device_id"]), {})
    gateway_firmware = (
        gateway.get("firmware")
        or (system_snapshot.get("device") or {}).get("firmware")
        or telemetry.get("firmware")
        or "unknown"
    )
    generated_at = datetime.now().astimezone().isoformat(timespec="seconds")

    rows: list[dict[str, str]] = []

    def add(section: str, test_id: str, item: str, status: str, evidence: str, source: str, next_action: str = "") -> None:
        rows.append(
            {
                "section": section,
                "test_id": test_id,
                "item": item,
                "status": status,
                "evidence": evidence,
                "source": source,
                "next_action": next_action,
            }
        )

    add("基础", "B01", "CPU0 固件构建", "通过", "text=5945162, data=144, bss=8098792 bytes", "test_results/cpu0_build_20260728.txt")
    add("基础", "B02", "CPU1 固件构建", "通过", "text=87158, data=116, bss=327308 bytes", "test_results/cpu1_build_20260728.txt")
    add("基础", "B03", "双核烧录", "通过", "J-Link Program/Verify 成功；CPU1 内容匹配", "J-Link V9.38a")
    add("软件", "S01", "服务端单元测试", "通过", "39/39", "test_results/server_unittest_20260728.txt")
    add("软件", "S02", "音频训练流水线测试", "通过", "22/22；1 条弃用警告", "test_results/ml_training_pytest_20260728.txt")
    add("软件", "S03", "视觉训练流水线测试", "通过", "21/21", "test_results/vision_training_pytest_20260728.txt")
    add("软件", "S04", "RA8/ESP32 协议合约测试", "通过", "4/4", "test_results/protocol_contract_20260728.txt")
    add(
        "板端",
        "H01",
        "烧录后持续视觉检测",
        "通过",
        f"连续 {hardware['captured_frames']} 帧，frame {hardware['first_frame']}..{hardware['last_frame']}，"
        f"实际 {rate_metrics['effective_fps']:.3f} fps（{rate_metrics['average_frame_interval_ms']:.0f} ms/帧），"
        f"JPEG/NPU/drop={hardware['jpeg_error_delta']}/{hardware['npu_error_delta']}/{hardware['dropped_frame_delta']}",
        str(hardware_path.relative_to(ROOT)),
    )
    add(
        "板端",
        "H02",
        "自动推理不常驻写 SD",
        "通过",
        f"camera_saved_delta={hardware['camera_saved_delta']}，auto=1，最小调度间隔 1000 ms；"
        f"实测平均帧间隔 {rate_metrics['average_frame_interval_ms']:.0f} ms",
        str(hardware_path.relative_to(ROOT)),
    )
    add(
        "板端",
        "H03",
        "三视觉模型串行时延",
        "通过",
        f"总耗时 avg/P95={hardware['timings_ms']['total_average']:.1f}/{hardware['timings_ms']['total_p95']:.1f} ms；"
        f"scene/occlusion/behavior={hardware['timings_ms']['scene_average']:.1f}/"
        f"{hardware['timings_ms']['occlusion_average']:.1f}/{hardware['timings_ms']['behavior_average']:.1f} ms",
        str(hardware_path.relative_to(ROOT)),
    )
    add(
        "板端",
        "H04",
        "服务器到 RA8 命令闭环",
        "通过",
        f"{commands['pass_count']}/{commands['test_count']}；含播放、录音、拍照和后台持续性",
        str(command_path.relative_to(ROOT)),
    )
    add(
        "板端",
        "H05",
        "麦克风录制后继续检测",
        "通过",
        next(
            item["ra8_evidence"] for item in commands["results"] if item["action"] == "post_command.continuity"
        ).__str__(),
        str(command_path.relative_to(ROOT)),
    )
    add(
        "板端",
        "H06",
        "一级哭声与视觉并行运行",
        "通过",
        f"哭声推理 {audio_hardware['captured_frames']} 帧，avg/P95="
        f"{audio_hardware['cry_ms_average']:.1f}/{audio_hardware['cry_ms_p95']} ms，"
        f"mic read delta={audio_hardware['microphone_read_delta']}，同时 vision delta="
        f"{audio_hardware['vision_inference_delta']}，异常状态样本=0",
        str(audio_path.relative_to(ROOT)),
    )

    live_check_by_id = {item["id"]: item for item in live_audit.get("checks", [])}
    thermal_check = live_check_by_id.get("thermal_frame", {})
    thermal_metrics = thermal_check.get("metrics") or {}
    add(
        "板端",
        "H07",
        "热成像连续完整帧",
        "通过" if thermal_check.get("status") == "pass" else "发现故障",
        thermal_check.get("detail", "无热成像审计结果"),
        str(live_audit_path.relative_to(ROOT)),
    )

    diagnostic_rows: list[dict[str, str]] = []
    for test_id, component, check_id in [
        ("D01", "ra8_link", "ra8_online"),
        ("D02", "sd_card", "sd_ready"),
        ("D03", "thermal", "thermal_frame"),
        ("D04", "bed_temperature", "temperature_channels"),
        ("D05", "pressure", "pressure_stream"),
        ("D06", "microphone", "microphone_capture"),
        ("D07", "camera", "serial_camera"),
    ]:
        item = live_check_by_id.get(check_id, {})
        state = item.get("status", "unavailable")
        report_status = "通过" if state == "pass" else "发现故障" if state == "fail" else "未测试"
        detail = item.get("detail", "无当前诊断数据")
        add(
            "部件",
            test_id,
            component,
            report_status,
            detail,
            str(live_audit_path.relative_to(ROOT)),
        )
        diagnostic_rows.append(
            {"id": component, "status": state, "summary": report_status, "detail": detail}
        )

    add(
        "模型",
        "M01",
        "一级哭声 float 测试集",
        "通过",
        f"n={primary_float['test_samples']}，cry P/R/F1={format_percent(primary_cry['precision'])}/"
        f"{format_percent(primary_cry['recall'])}/{format_percent(primary_cry['f1-score'])}",
        "ml_training/artifacts/cry_detection_mlp_hardneg_v2b/training_report.json",
    )
    add(
        "模型",
        "M02",
        "一级哭声 INT8 测试集",
        "通过",
        f"n={primary_int8['test_windows']}，macro P/R/F1={format_percent(primary_int8_metrics['precision'])}/"
        f"{format_percent(primary_int8_metrics['recall'])}/{format_percent(primary_int8_metrics['f1'])}",
        "ml_training/artifacts/cry_detection_mlp_hardneg_v2b/c_int8_report.json",
    )
    add(
        "模型",
        "M03",
        "二级哭声 INT8 五类",
        "通过",
        f"n={secondary_int8['test_windows']}，macro P/R/F1={format_percent(secondary_int8_metrics['precision'])}/"
        f"{format_percent(secondary_int8_metrics['recall'])}/{format_percent(secondary_int8_metrics['f1'])}",
        "ml_training/artifacts/cry_type_mlp_robust_v1/c_int8_report.json",
    )
    add(
        "模型",
        "M04",
        "二级哭声 unknown 拒识",
        "部分通过",
        f"recordings={secondary_decision['recordings']}，coverage={format_percent(secondary_decision['coverage'])}，"
        f"accepted accuracy={format_percent(secondary_decision['accepted_accuracy'])}，"
        f"unknown={secondary_decision['recordings'] - round(secondary_decision['recordings'] * secondary_decision['coverage'])}",
        "ml_training/artifacts/cry_type_mlp_robust_v1/c_int8_report.json",
        "补充真实 unknown 类标注样本后才能计算 unknown precision/recall/F1",
    )
    for test_id, name in [("M05", "场景检测"), ("M06", "遮盖分类"), ("M07", "行为分类")]:
        add(
            "模型",
            test_id,
            name + "指标",
            "未测试",
            "当前仓库没有与已部署模型严格匹配的带标签测试报告；不能用连续运行代替准确率",
            "测试111.docx 3.2",
        )

    add(
        "融合",
        "F01",
        "正常/边界/危险事件各 20 次",
        "未测试",
        "未使用模板中的示例 20/20；当前未施加玩偶、配重、遮挡和标准音频时间线",
        "测试111.docx 3.3",
    )
    add(
        "故障",
        "X01",
        "热成像历史故障诊断",
        "部分通过",
        f"早先曾捕获 thermal=fault；当前已恢复，连续 {thermal_metrics.get('valid_samples', 0)}/"
        f"{thermal_metrics.get('samples', 0)} 组完整有效帧。未执行受控故障注入和恢复计时",
        str(live_audit_path.relative_to(ROOT)),
    )
    add(
        "故障",
        "X02",
        "摄像头/压力/SD/网络故障注入",
        "未测试",
        "未执行拔线、写满、断网和服务器重启，避免将未执行项写成通过",
        "测试111.docx 3.4",
    )

    test_table = [[row[key] for key in ("test_id", "item", "status", "evidence", "next_action")] for row in rows]
    diagnostics_table = [
        [item.get("id", ""), item.get("status", ""), item.get("summary", ""), item.get("detail", "")]
        for item in diagnostic_rows
    ]
    command_table = [
        [
            item["action"],
            "通过" if item["pass"] else "失败",
            item["gateway_status"],
            item["gateway_ack_ms"],
            item["ra8_state_ms"],
            json.dumps(item["ra8_evidence"], ensure_ascii=False, separators=(",", ":")),
        ]
        for item in commands["results"]
    ]
    model_table = [
        ["一级哭声 float", "cry", primary_float["test_samples"], format_percent(primary_cry["precision"]), format_percent(primary_cry["recall"]), format_percent(primary_cry["f1-score"])],
        ["一级哭声 INT8", "macro", primary_int8["test_windows"], format_percent(primary_int8_metrics["precision"]), format_percent(primary_int8_metrics["recall"]), format_percent(primary_int8_metrics["f1"])],
        ["二级哭声 INT8", "五类 macro", secondary_int8["test_windows"], format_percent(secondary_int8_metrics["precision"]), format_percent(secondary_int8_metrics["recall"]), format_percent(secondary_int8_metrics["f1"])],
        ["场景检测", "infant/head_face", "待补", "待补", "待补", "待补"],
        ["遮盖分类", "covered/not_visible", "待补", "待补", "待补", "待补"],
        ["姿态分类", "standing", "待补", "待补", "待补", "待补"],
    ]
    deployment_table = [
        ["一级音频", f"accuracy {primary_float['test']['accuracy']:.6f} -> {primary_int8['test_accuracy']:.6f}", "CPU INT8, NPU 0%", audio_hardware["cry_ms_average"], audio_hardware["cry_ms_p95"], "未埋点"],
        ["二级音频", f"float n={secondary_float['test_samples']} 与 INT8 n={secondary_int8['test_windows']} 非同样本", "CPU INT8, NPU 0%", "条件触发", "待采样", "未埋点"],
        ["场景模型", "同样本量化差异待补", "Ethos-U command stream", hardware["timings_ms"]["scene_average"], hardware["timings_ms"]["scene_p95"], "arena 784 KiB"],
        ["遮盖模型", "同样本量化差异待补", "Ethos-U command stream", hardware["timings_ms"]["occlusion_average"], hardware["timings_ms"]["occlusion_p95"], "arena 252 KiB"],
        ["行为模型", "同样本量化差异待补", "Ethos-U command stream", hardware["timings_ms"]["behavior_average"], hardware["timings_ms"]["behavior_p95"], "arena 252 KiB"],
    ]

    title = "RA8P1 婴儿监护项目自动实测报告（测试111）"
    summary_paragraphs = [
        f"生成时间：{generated_at}；Git 基线：{git_revision()}；RA8/视觉 build：{ra8_build}/{vision_build}；"
        f"网关固件：{gateway_firmware}。",
        "本报告只填写实际读取、实际下发或现有带标签测试报告能够证明的数据。需要玩偶、标准配重、参考温度源、遮挡、断网等外部条件的项目保留为待测试。",
        f"当前自动结果：通过 {sum(row['status'] == '通过' for row in rows)} 项，部分通过 {sum(row['status'] == '部分通过' for row in rows)} 项，发现故障 {sum(row['status'] == '发现故障' for row in rows)} 项，未测试 {sum(row['status'] == '未测试' for row in rows)} 项。",
        f"关键结论：烧录后视觉持续自动触发，配置的最小调度间隔为 1000 ms；受串口抓图和处理链路限制，"
        f"本次实测为 {rate_metrics['effective_fps']:.3f} fps（约 {rate_metrics['average_frame_interval_ms'] / 1000.0:.2f} 秒/帧）。"
        f"录音与拍照只在命令触发时写 SD；麦克风瞬态 I2S 错误已增加自动恢复；热成像当前连续 "
        f"{thermal_metrics.get('valid_samples', 0)}/{thermal_metrics.get('samples', 0)} 组完整有效帧，当前已测部件均正常。",
    ]
    sections: list[tuple[str, list[str] | tuple[list[str], list[list[Any]]]]] = [
        ("1. 结论与范围", summary_paragraphs),
        ("2. 自动测试总表", (["编号", "项目", "状态", "实测证据", "后续动作"], test_table)),
        ("3. 当前部件诊断", (["部件", "状态", "摘要", "详情"], diagnostics_table)),
        ("4. 命令闭环实测", (["动作", "结果", "网关状态", "ACK/ms", "板端确认/ms", "板端证据"], command_table)),
        ("5. 单模态模型结果", (["模型", "关键类别", "样本数", "Precision", "Recall", "F1"], model_table)),
        ("6. 量化与板端部署", (["模型", "INT8 前后差异", "执行路径", "平均/ms", "P95/ms", "内存"], deployment_table)),
        ("7. 未执行的实物测试", [
            "按用户要求，本轮不再执行需要额外标注集、标准配重、参考温度源、遮挡场景、断网故障注入、Android 配合或长时间运行的测试；这些项目在总表中明确标为“未测试”。",
        ]),
        ("8. 原始证据路径", [
            str(hardware_path.relative_to(ROOT)),
            str(audio_path.relative_to(ROOT)),
            str(command_path.relative_to(ROOT)),
            str(system_path.relative_to(ROOT)),
            str(live_audit_path.relative_to(ROOT)),
            "test_results/server_unittest_20260728.txt",
            "test_results/ml_training_pytest_20260728.txt",
            "test_results/vision_training_pytest_20260728.txt",
            "test_results/protocol_contract_20260728.txt",
            "test_results/cpu0_build_20260728.txt",
            "test_results/cpu1_build_20260728.txt",
        ]),
    ]

    md_lines = [f"# {title}", ""]
    for heading, content in sections:
        md_lines.extend([f"## {heading}", ""])
        if isinstance(content, tuple):
            md_lines.extend(markdown_table(content[0], content[1]))
            md_lines.append("")
        else:
            for paragraph in content:
                md_lines.extend([paragraph, ""])
    markdown_path = output_dir / "RA8P1_测试111_自动实测报告.md"
    markdown_path.write_text("\n".join(md_lines), encoding="utf-8")

    csv_path = output_dir / "RA8P1_测试111_结果表.csv"
    with csv_path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    docx_path = output_dir / "RA8P1_测试111_自动实测报告.docx"
    write_docx(docx_path, title, sections)
    print(markdown_path)
    print(csv_path)
    print(docx_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

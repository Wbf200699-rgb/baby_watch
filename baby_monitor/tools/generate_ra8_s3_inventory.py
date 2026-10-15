#!/usr/bin/env python3
"""Generate the RA8 dual-core and ESP32-S3 source/program inventory."""

from __future__ import annotations

import csv
import datetime as dt
import subprocess
from collections import Counter
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
SOURCE_CSV = DOCS / "ra8_s3_source_inventory.csv"
PROGRAM_CSV = DOCS / "ra8_s3_program_inventory.csv"
SUMMARY_MD = DOCS / "RA8_S3_SOURCE_AND_PROGRAM_INVENTORY_zh-CN.md"

INCLUDED_PREFIXES = (
    "Multicore_top/",
    "Multicore_top_CPU0/",
    "Multicore_top_CPU1/",
    "common/",
    "firmware/esp32s3_gateway/",
    "tools/ra8_esp32_protocol/",
)

INCLUDED_FILES = {
    "Multicore_top_CPU1 Debug_Multicore.launch",
    "tools/build_vision_flash_images.ps1",
    "tools/collect_audio_hardware_evidence.py",
    "tools/collect_ra8_hardware_evidence.ps1",
    "tools/generate_ra8_s3_inventory.py",
    "tools/program_ra8_internal.jlink",
    "tools/program_ra8_internal.ps1",
    "tools/program_vision_models.jlink",
    "tools/program_vision_models.ps1",
    "tools/run_ra8_command_evidence.py",
    "tools/verify_project.ps1",
}

TEXT_EXTENSIONS = {
    ".api_xml",
    ".asm",
    ".c",
    ".cc",
    ".cfg",
    ".cproject",
    ".cpp",
    ".csv",
    ".gitignore",
    ".h",
    ".hh",
    ".hpp",
    ".ini",
    ".jlink",
    ".json",
    ".launch",
    ".ld",
    ".lld",
    ".lock",
    ".md",
    ".project",
    ".ps1",
    ".py",
    ".s",
    ".secure_azone",
    ".secure_rzone",
    ".secure_xml",
    ".sh",
    ".toml",
    ".txt",
    ".xml",
    ".yaml",
    ".yml",
}

SPECIAL_NAMES = {"CMakeLists.txt", "Makefile", "sdkconfig.defaults"}

EXCLUDED_PARTS = {
    ".git",
    ".metadata",
    ".pytest_cache",
    ".settings",
    ".venv",
    "Debug",
    "Release",
    "__pycache__",
    "artifacts",
    "build",
    "data",
    "dist",
    "node_modules",
    "test-results",
    "test_results",
    "venv",
}

LANGUAGES = {
    ".api_xml": "XML/FSP",
    ".asm": "Assembly",
    ".c": "C",
    ".cc": "C++",
    ".cfg": "Configuration",
    ".cproject": "XML/e2 studio",
    ".cpp": "C++",
    ".csv": "CSV",
    ".gitignore": "Git ignore",
    ".h": "C/C++ header",
    ".hh": "C++ header",
    ".hpp": "C++ header",
    ".ini": "INI",
    ".jlink": "J-Link command",
    ".json": "JSON",
    ".launch": "XML/e2 studio launch",
    ".ld": "Linker script",
    ".lld": "Linker script",
    ".lock": "Dependency lock",
    ".md": "Markdown",
    ".project": "XML/e2 studio",
    ".ps1": "PowerShell",
    ".py": "Python",
    ".s": "Assembly",
    ".secure_azone": "XML/e2 studio security",
    ".secure_rzone": "XML/e2 studio security",
    ".secure_xml": "XML/e2 studio security",
    ".sh": "Shell",
    ".toml": "TOML",
    ".txt": "Text configuration",
    ".xml": "XML",
    ".yaml": "YAML",
    ".yml": "YAML",
}

PROGRAMS = [
    {
        "program_id": "ra8_cpu0_m85",
        "name": "RA8P1 CPU0 主控固件",
        "target": "R7KA8P1 Cortex-M85 + Ethos-U55",
        "entry_points": "Multicore_top_CPU0/ra_gen/main.c; Multicore_top_CPU0/src/*_thread_entry.c",
        "toolchain": "Renesas e2 studio/FSP + GNU Arm Embedded",
        "build_or_run": "cd Multicore_top_CPU0/Debug; make all -j8",
        "outputs": "Multicore_top_CPU0.elf; Multicore_top_CPU0_internal.srec; vision_models_ospi0_cs0.bin",
        "purpose": "视觉和哭声推理、多模态融合、分级告警、S3 UART、串口摄像头和双核控制",
        "status": "生产主固件",
    },
    {
        "program_id": "ra8_cpu1_m33",
        "name": "RA8P1 CPU1 外设固件",
        "target": "R7KA8P1 Cortex-M33",
        "entry_points": "Multicore_top_CPU1/ra_gen/main.c; Multicore_top_CPU1/src/*_thread_entry.c",
        "toolchain": "Renesas e2 studio/FSP + GNU Arm Embedded",
        "build_or_run": "cd Multicore_top_CPU1/Debug; make all -j8",
        "outputs": "Multicore_top_CPU1.elf; Multicore_top_CPU1.srec",
        "purpose": "热成像、压力矩阵、TMP117、SD/FAT、系统日志、录音写盘和扬声器播放",
        "status": "生产主固件",
    },
    {
        "program_id": "esp32s3_gateway",
        "name": "ESP32-S3 网络与媒体网关",
        "target": "ESP32-S3 CAM",
        "entry_points": "firmware/esp32s3_gateway/main/gateway_main.c",
        "toolchain": "ESP-IDF 5.x",
        "build_or_run": "idf.py set-target esp32s3; idf.py build; idf.py -p COMx flash monitor",
        "outputs": "ESP-IDF bootloader/partition/application binaries",
        "purpose": "RA8 UART 网关、联网遥测、命令转发和按需 JPEG/PCM 预览",
        "status": "当前 S3 网关",
    },
    {
        "program_id": "ra8_s3_protocol_tools",
        "name": "RA8-S3 串口协议与可视化工具",
        "target": "Windows/Linux PC + CH340",
        "entry_points": "tools/ra8_esp32_protocol/ra8_protocol.py; tools/ra8_esp32_protocol/ra8_live_monitor.py",
        "toolchain": "Python, pyserial, matplotlib",
        "build_or_run": "python tools/ra8_esp32_protocol/ra8_live_monitor.py --help",
        "outputs": "协议抓包、实时状态、矩阵图和诊断记录",
        "purpose": "模拟 S3 读取 RA8、发命令并验证传感器和协议",
        "status": "硬件调试工具",
    },
    {
        "program_id": "ra8_flash_tools",
        "name": "RA8 双核与视觉模型烧录工具",
        "target": "Windows + J-Link",
        "entry_points": "tools/program_ra8_internal.ps1; tools/program_vision_models.ps1",
        "toolchain": "PowerShell, J-Link Commander",
        "build_or_run": "powershell -File tools/program_ra8_internal.ps1",
        "outputs": "内部 Flash 烧录结果和 OSPI 模型烧录结果",
        "purpose": "避免组合 SREC 的 0x80000000 超范围，并分别烧录 CPU 固件和视觉模型",
        "status": "构建/烧录工具",
    },
]

CPU0_THREADS = [
    ("audio_infer", "src/audio_infer_thread_entry.c", "哭声一级/二级推理与时序策略"),
    ("camera", "src/camera_thread_entry.c", "串口摄像头探测、JPEG 分块和诊断"),
    ("esp32_uart", "src/esp32_uart_thread_entry.c", "RA8-S3 帧收发、冷启动握手和可靠事件"),
    ("fusion", "src/fusion_thread_entry.c", "多模态证据、事件状态机和分级告警"),
    ("vision_preprocess", "src/vision_preprocess_thread_entry.c", "JPEG 解码、ROI 和视觉预处理"),
    ("vision_infer", "src/vision_infer_thread_entry.c", "三视觉模型/NPU 调度与结果发布"),
    ("cpu1_start", "src/cpu1_start_entry.c", "CPU1 启动和双核就绪协调"),
    ("command_dispatch", "src/command_dispatch_thread_entry.c", "命令分发扩展入口"),
    ("cloud_protocol", "src/cloud_protocol_thread_entry.c", "云协议扩展入口"),
    ("file_request", "src/file_request_thread_entry.c", "跨核文件请求扩展入口"),
    ("playback_control", "src/playback_control_thread_entry.c", "播放控制扩展入口"),
    ("telemetry_upload", "src/telemetry_upload_thread_entry.c", "遥测上传扩展入口"),
    ("m85_watchdog", "src/m85_watchdog_thread_entry.c", "M85 看门狗和健康监测"),
]

CPU1_THREADS = [
    ("thermal", "src/thermal_thread_entry.c", "HTPA 80x64 UART 整帧接收与发布"),
    ("pressure", "src/pressure_thread_entry.c", "M1616 16x16 压力矩阵 UART"),
    ("temp", "src/temp_thread_entry.c", "PCA9548A + 5 路 TMP117 温度"),
    ("sd_card", "src/sd_card_thread_entry.c", "SDHI 初始化、恢复和文件系统就绪"),
    ("file_system", "src/file_system_thread_entry.c", "共享 FAT 写入、日志、录音和图片落盘"),
    ("sd_audio_player", "src/sd_audio_player_thread_entry.c", "SD WAV 文件播放控制"),
    ("audio_capture", "src/audio_capture_thread_entry.c", "音频采集兼容/扩展入口"),
    ("audio_feature", "src/audio_feature_thread_entry.c", "音频特征兼容/扩展入口"),
    ("speaker_output", "src/speaker_output_thread_entry.c", "SSI 扬声器输出兼容/扩展入口"),
    ("m33_ipc_rx", "src/m33_ipc_rx_thread_entry.c", "跨核 IPC 接收扩展入口"),
    ("m33_ipc_tx", "src/m33_ipc_tx_thread_entry.c", "跨核 IPC 发送扩展入口"),
    ("file_update", "src/file_update_thread_entry.c", "文件更新扩展入口"),
    ("m33_watchdog", "src/m33_watchdog_thread_entry.c", "M33 看门狗和健康监测"),
]


def git_paths(*args: str) -> set[str]:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, check=True, stdout=subprocess.PIPE
    )
    return {
        item.decode("utf-8", errors="surrogateescape").replace("\\", "/")
        for item in result.stdout.split(b"\0")
        if item
    }


def git_text(*args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=ROOT,
        check=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
    )
    return result.stdout.strip()


def in_scope(path: str) -> bool:
    return path in INCLUDED_FILES or path.startswith(INCLUDED_PREFIXES)


def should_include(path_text: str) -> bool:
    if not in_scope(path_text):
        return False
    path = Path(path_text)
    if any(part in EXCLUDED_PARTS or part.startswith(".tmp") for part in path.parts):
        return False
    if path.name.startswith("._"):
        return False
    return path.name in SPECIAL_NAMES or path.suffix.lower() in TEXT_EXTENSIONS


def component_for(path: str) -> str:
    if path.startswith("Multicore_top_CPU0/"):
        return "RA8 CPU0/M85"
    if path.startswith("Multicore_top_CPU1/"):
        return "RA8 CPU1/M33"
    if path.startswith("Multicore_top/"):
        return "RA8 solution"
    if path.startswith("common/"):
        return "RA8 shared common"
    if path.startswith("firmware/esp32s3_gateway/"):
        return "ESP32-S3 gateway"
    if path.startswith("tools/ra8_esp32_protocol/"):
        return "RA8-S3 protocol tools"
    if path.startswith("tools/"):
        return "RA8 build/test tools"
    return "RA8-S3 root"


def provenance_for(path: str) -> str:
    lowered = path.lower()
    name = Path(path).name.lower()
    if "/ra_gen/" in lowered:
        return "FSP-generated"
    if "/ra_cfg/" in lowered:
        return "FSP-generated-config"
    if "/ra/" in lowered:
        return "vendor/FSP/RTOS"
    if "/src/third_party/" in lowered:
        return "third-party"
    if "/src/vision_models/" in lowered or name.endswith("_model_data.h"):
        return "embedded-model-generated"
    if name == "dependencies.lock":
        return "tool-generated"
    return "project-authored"


def language_for(path: str) -> str:
    name = Path(path).name
    if name == "CMakeLists.txt":
        return "CMake"
    if name == "Makefile":
        return "Make"
    return LANGUAGES.get(Path(path).suffix.lower(), "Text")


def category_for(path: str, provenance: str, language: str) -> str:
    lowered = path.lower()
    name = Path(path).name.lower()
    if name.startswith("test_") or "/tests/" in lowered:
        return "test"
    if language == "Markdown":
        return "documentation/protocol"
    if "protocol" in name or "ipc_shared" in name:
        return "protocol/IPC"
    if provenance == "embedded-model-generated":
        return "embedded-model"
    if provenance in {"FSP-generated", "vendor/FSP/RTOS", "third-party"}:
        return "generated/vendor-source"
    if language in {
        "Configuration",
        "CSV",
        "Dependency lock",
        "Git ignore",
        "INI",
        "JSON",
        "Linker script",
        "Text configuration",
        "TOML",
        "XML",
        "XML/FSP",
        "XML/e2 studio",
        "XML/e2 studio launch",
        "XML/e2 studio security",
        "YAML",
        "CMake",
        "Make",
    }:
        return "configuration/build"
    if language in {"J-Link command", "PowerShell", "Python", "Shell"}:
        return "tool/script"
    return "application-source"


def git_state_for(
    path: str,
    tracked: set[str],
    untracked: set[str],
    modified: set[str],
    staged: set[str],
) -> str:
    if path in untracked:
        return "untracked"
    states: list[str] = []
    if path in staged:
        states.append("staged")
    if path in modified:
        states.append("modified")
    if states:
        return "+".join(states)
    return "tracked-clean" if path in tracked else "workspace"


def line_count(path: Path) -> int:
    data = path.read_bytes()
    if not data:
        return 0
    return data.count(b"\n") + (0 if data.endswith(b"\n") else 1)


def source_rows() -> list[dict[str, object]]:
    tracked = git_paths("ls-files", "-z")
    untracked = git_paths("ls-files", "--others", "--exclude-standard", "-z")
    modified = git_paths("diff", "--name-only", "-z")
    staged = git_paths("diff", "--cached", "--name-only", "-z")
    rows: list[dict[str, object]] = []
    for relative_path in sorted(tracked | untracked, key=str.casefold):
        if not should_include(relative_path):
            continue
        absolute_path = ROOT / Path(relative_path)
        if not absolute_path.is_file():
            continue
        provenance = provenance_for(relative_path)
        language = language_for(relative_path)
        rows.append(
            {
                "path": relative_path,
                "component": component_for(relative_path),
                "category": category_for(relative_path, provenance, language),
                "language": language,
                "provenance": provenance,
                "git_state": git_state_for(relative_path, tracked, untracked, modified, staged),
                "bytes": absolute_path.stat().st_size,
                "lines": line_count(absolute_path),
            }
        )
    return rows


def write_csv(path: Path, rows: Iterable[dict[str, object]], fields: list[str]) -> None:
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def markdown_table(headers: list[str], rows: Iterable[Iterable[object]]) -> str:
    def clean(value: object) -> str:
        return str(value).replace("|", "\\|").replace("\n", " ")

    result = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    result.extend("| " + " | ".join(clean(value) for value in row) + " |" for row in rows)
    return "\n".join(result)


def write_summary(rows: list[dict[str, object]]) -> None:
    generated_at = dt.datetime.now().astimezone().isoformat(timespec="seconds")
    branch = git_text("branch", "--show-current") or "detached"
    revision = git_text("rev-parse", "--short", "HEAD")
    by_component = Counter(str(row["component"]) for row in rows)
    by_category = Counter(str(row["category"]) for row in rows)
    by_provenance = Counter(str(row["provenance"]) for row in rows)
    by_state = Counter(str(row["git_state"]) for row in rows)
    total_lines = sum(int(row["lines"]) for row in rows)
    authored_lines = sum(
        int(row["lines"])
        for row in rows
        if row["provenance"] == "project-authored"
        and row["category"] != "documentation/protocol"
    )

    content = [
        "# RA8 双核与 ESP32-S3 源代码及程序清单",
        "",
        f"> 生成时间：`{generated_at}`  ",
        f"> Git 基线：`{branch}` / `{revision}`  ",
        "> 生成工具：`tools/generate_ra8_s3_inventory.py`",
        "",
        "## 1. 范围",
        "",
        "本清单只收录 RA8P1 双核固件、ESP32-S3 网关、RA8-S3 协议工具以及 RA8 构建/烧录工具。",
        "不收录服务端代码、网页、旧 ESP32-CAM、音频/视觉训练代码、数据集、训练产物、虚拟环境和运行数据。",
        "",
        "RA8 固件编译所必需的已嵌入模型 C/H 和 NPU 编译输出仍然收录，并明确标记为",
        "`embedded-model-generated`；它们是固件构建输入，不是训练代码或训练数据。",
        "",
        f"- 源码/配置文件：**{len(rows)}**",
        f"- 总行数：**{total_lines:,}**（含 FSP/RTOS 和嵌入模型生成代码）",
        f"- 项目自编代码/配置：**{authored_lines:,}** 行（不含协议文档）",
        "- Git 状态：" + "；".join(f"{name} {count}" for name, count in sorted(by_state.items())),
        "",
        "完整逐文件清单：[`ra8_s3_source_inventory.csv`](ra8_s3_source_inventory.csv)  ",
        "程序级清单：[`ra8_s3_program_inventory.csv`](ra8_s3_program_inventory.csv)",
        "",
        "## 2. 程序清单",
        "",
        markdown_table(
            ["ID", "程序", "目标", "入口", "用途", "状态"],
            (
                (
                    item["program_id"],
                    item["name"],
                    item["target"],
                    item["entry_points"],
                    item["purpose"],
                    item["status"],
                )
                for item in PROGRAMS
            ),
        ),
        "",
        "## 3. 源码规模",
        "",
        "### 3.1 按组件",
        "",
        markdown_table(["组件", "文件数"], by_component.most_common()),
        "",
        "### 3.2 按类别",
        "",
        markdown_table(["类别", "文件数"], by_category.most_common()),
        "",
        "### 3.3 按来源",
        "",
        markdown_table(["来源", "文件数"], by_provenance.most_common()),
        "",
        "## 4. CPU0 / Cortex-M85 线程",
        "",
        markdown_table(
            ["线程/入口", "文件", "职责"],
            ((name, f"`Multicore_top_CPU0/{path}`", role) for name, path, role in CPU0_THREADS),
        ),
        "",
        "CPU0 的应用驱动位于 `Multicore_top_CPU0/src/thread_code/`，视觉模型运行输入位于",
        "`Multicore_top_CPU0/src/vision_models/`。FSP 线程、引脚、时钟和中断由 `configuration.xml` 生成。",
        "",
        "## 5. CPU1 / Cortex-M33 线程",
        "",
        markdown_table(
            ["线程/入口", "文件", "职责"],
            ((name, f"`Multicore_top_CPU1/{path}`", role) for name, path, role in CPU1_THREADS),
        ),
        "",
        "CPU1 的设备驱动位于 `Multicore_top_CPU1/src/thread_code/`。SD/FAT 是日志、麦克风 WAV、",
        "串口摄像头 JPEG 和播放文件的共同资源，写入必须使用现有共享锁和就绪状态。",
        "",
        "## 6. ESP32-S3 网关模块",
        "",
        markdown_table(
            ["文件", "职责"],
            [
                ("`firmware/esp32s3_gateway/main/gateway_main.c`", "Wi-Fi、RA8 UART、HTTPS 批次、命令和健康状态"),
                ("`firmware/esp32s3_gateway/main/media/camera_stream.[ch]`", "OV5640 JPEG 采集和按订阅上传"),
                ("`firmware/esp32s3_gateway/main/media/mic_stream.[ch]`", "S3 预览麦克风 PCM 上传"),
                ("`firmware/esp32s3_gateway/main/media/media_protocol.[ch]`", "S3MD 二进制媒体帧和控制消息"),
                ("`firmware/esp32s3_gateway/sdkconfig.defaults`", "ESP-IDF 默认配置"),
                ("`firmware/esp32s3_gateway/partitions.csv`", "Flash 分区"),
                ("`firmware/esp32s3_gateway/main/gateway_secrets.example.h`", "不含真实密钥的配置模板"),
            ],
        ),
        "",
        "真实 Wi-Fi 和设备密钥文件不得进入 Git 或清单。S3 的 RA8 UART 接收应先于网络和媒体初始化，",
        "并持续发送 `gateway_ready`，防止双端冷启动时 RA8 先发送大帧造成接收环形缓冲溢出。",
        "",
        "## 7. RA8-S3 协议和共享内存",
        "",
        markdown_table(
            ["接口", "权威文件", "说明"],
            [
                ("双核公共类型", "`common/*.h`", "公共错误、状态和基础 IPC/UART 定义"),
                ("双核共享 SRAM", "`Multicore_top_CPU[01]/src/thread_code/*_ipc_shared.h`", "热阵列、压力、温度、SD、音频和摄像头邮箱"),
                ("RA8-S3 帧协议", "`Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.[ch]`", "状态、矩阵、告警、命令和 ACK"),
                ("S3 适配协议", "`tools/ra8_esp32_protocol/ESP32_ADAPTER_PROTOCOL.md`", "网关字段和冷启动握手约束"),
                ("PC 协议解析", "`tools/ra8_esp32_protocol/ra8_protocol.py`", "CH340 调试时的权威解析实现"),
            ],
        ),
        "",
        "双核共享头必须在 CPU0/CPU1 中保持字段顺序、大小、共享地址和发布顺序一致。RA8-S3 协议",
        "变更必须同步修改 RA8、S3、PC 解析器和协议文档。",
        "",
        "## 8. 构建与烧录产物",
        "",
        markdown_table(
            ["产物", "生成方式", "用途"],
            [
                ("`Multicore_top_CPU0/Debug/Multicore_top_CPU0.elf`", "CPU0 make/e2 studio", "M85 调试"),
                ("`Multicore_top_CPU0/Debug/Multicore_top_CPU0_internal.srec`", "`tools/build_vision_flash_images.ps1`", "CPU0 内部 Flash"),
                ("`Multicore_top_CPU0/Debug/vision_models_ospi0_cs0.bin`", "`tools/build_vision_flash_images.ps1`", "外部 OSPI 视觉模型"),
                ("`Multicore_top_CPU1/Debug/Multicore_top_CPU1.elf/.srec`", "CPU1 make/e2 studio", "M33 调试和烧录"),
                ("ESP32-S3 binaries", "`idf.py build`", "S3 bootloader、分区和应用"),
            ],
        ),
        "",
        "构建二进制可再生且通常被 Git 忽略，因此不写入逐文件源码 CSV。CPU0 含外部 OSPI 地址的组合 SREC",
        "不能作为普通内部 Flash 文件烧录，必须使用内部 SREC 和 OSPI BIN 分开烧录。",
        "",
        "## 9. 当前工作区说明",
        "",
        "清单以当前工作区为准，包含尚未提交但实际参与 RA8/S3 构建的源码，并在 CSV 的 `git_state`",
        "列中标明。正式归档前应先完成双核、S3 和串口闭环验证并提交 Git，然后重新执行：",
        "",
        "```powershell",
        "python tools/generate_ra8_s3_inventory.py",
        "```",
        "",
    ]
    SUMMARY_MD.write_text("\n".join(content), encoding="utf-8")


def main() -> None:
    DOCS.mkdir(parents=True, exist_ok=True)
    rows = source_rows()
    write_csv(
        SOURCE_CSV,
        rows,
        ["path", "component", "category", "language", "provenance", "git_state", "bytes", "lines"],
    )
    write_csv(
        PROGRAM_CSV,
        PROGRAMS,
        [
            "program_id",
            "name",
            "target",
            "entry_points",
            "toolchain",
            "build_or_run",
            "outputs",
            "purpose",
            "status",
        ],
    )
    write_summary(rows)
    print(f"wrote {SUMMARY_MD.relative_to(ROOT)}")
    print(f"wrote {SOURCE_CSV.relative_to(ROOT)} ({len(rows)} files)")
    print(f"wrote {PROGRAM_CSV.relative_to(ROOT)} ({len(PROGRAMS)} programs)")


if __name__ == "__main__":
    main()

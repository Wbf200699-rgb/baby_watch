from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parent
DEFAULT_DATA_ROOT = ROOT / "data" / "raw"
REGISTRY_PATH = ROOT / "dataset_registry.yaml"


def sha256_file(path: Path, block_size: int = 4 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while block := handle.read(block_size):
            digest.update(block)
    return digest.hexdigest()


def download_with_resume(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_suffix(destination.suffix + ".part")
    curl = shutil.which("curl") or shutil.which("curl.exe")
    if curl is None:
        raise RuntimeError("找不到 curl/curl.exe；Windows 10/11 默认自带 curl。")
    command = [
        curl,
        "--fail",
        "--location",
        "--retry",
        "6",
        "--retry-delay",
        "3",
        "--continue-at",
        "-",
        "--output",
        str(partial),
        url,
    ]
    print(f"下载 {url}")
    subprocess.run(command, check=True)
    partial.replace(destination)


def safe_extract_zip(archive: Path, destination: Path, password: str | None = None) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    password_bytes = password.encode("utf-8") if password else None
    base = destination.resolve()
    with zipfile.ZipFile(archive) as zipped:
        for member in zipped.infolist():
            target = (destination / member.filename).resolve()
            if target != base and base not in target.parents:
                raise RuntimeError(f"拒绝解压不安全路径: {member.filename}")
        zipped.extractall(destination, pwd=password_bytes)


def archive_is_encrypted(archive: Path) -> bool:
    with zipfile.ZipFile(archive) as zipped:
        return any(member.flag_bits & 0x1 for member in zipped.infolist())


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="下载并安全解压 CribHD / SyRIP 官方数据集")
    parser.add_argument(
        "--dataset",
        action="append",
        choices=("cribhd", "syrip", "syrip_expanded", "all"),
        help="可重复指定；默认下载 cribhd 和原始 syrip",
    )
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument("--download-only", action="store_true")
    parser.add_argument("--force-download", action="store_true")
    parser.add_argument(
        "--expanded",
        action="store_true",
        help="将 expanded SyRIP 加入默认列表；解压密码仅从 SYRIP_ARCHIVE_PASSWORD 读取",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with REGISTRY_PATH.open("r", encoding="utf-8") as handle:
        registry = yaml.safe_load(handle)["datasets"]

    requested = args.dataset or ["cribhd", "syrip"]
    if "all" in requested:
        requested = ["cribhd", "syrip", "syrip_expanded"]
    elif args.expanded and "syrip_expanded" not in requested:
        requested.append("syrip_expanded")

    downloads = args.data_root / "downloads"
    for name in dict.fromkeys(requested):
        entry = registry[name]
        archive = downloads / entry["archive_name"]
        if args.force_download or not archive.exists():
            download_with_resume(entry["archive_url"], archive)
        else:
            print(f"已存在，跳过下载: {archive}")

        actual_bytes = archive.stat().st_size
        actual_sha256 = sha256_file(archive)
        expected_bytes = entry.get("archive_bytes")
        expected_sha256 = entry.get("sha256")
        if expected_bytes is not None and actual_bytes != int(expected_bytes):
            raise SystemExit(
                f"{name} 文件大小校验失败: {actual_bytes} != {expected_bytes}；"
                "删除损坏压缩包后重新下载"
            )
        if expected_sha256 and actual_sha256.lower() != str(expected_sha256).lower():
            raise SystemExit(
                f"{name} SHA-256 校验失败: {actual_sha256}；"
                "删除损坏压缩包后重新下载"
            )
        print(
            f"{name}: bytes={actual_bytes} sha256={actual_sha256} "
            f"encrypted={archive_is_encrypted(archive)}"
        )
        if args.download_only:
            continue

        destination = args.data_root / entry["extract_dir"]
        if destination.exists() and any(destination.iterdir()):
            print(f"已解压，跳过: {destination}")
            continue

        password = None
        if entry.get("access") == "password_required":
            password = os.environ.get(entry["password_environment_variable"])
            if not password:
                print(
                    "expanded SyRIP 已下载但未解压。向作者取得密码后，设置临时环境变量 "
                    "SYRIP_ARCHIVE_PASSWORD 再运行本命令。"
                )
                continue
        try:
            safe_extract_zip(archive, destination, password)
        except (RuntimeError, zipfile.BadZipFile) as exc:
            print(f"解压失败: {exc}", file=sys.stderr)
            return 2
        print(f"已解压到 {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

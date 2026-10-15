from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import time
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image

from vision_pipeline.image_utils import IMAGE_SUFFIXES, difference_hash, hamming_distance


ROOT = Path(__file__).resolve().parent
DEFAULT_OUTPUT = ROOT / "data" / "raw" / "self_capture"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="从最终摄像头录像、图片目录或本机摄像头建立采集场次")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--video", type=Path)
    source.add_argument("--image-dir", type=Path)
    source.add_argument("--camera", type=int, metavar="INDEX")
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--doll-id", required=True)
    parser.add_argument("--camera-id", required=True, help="例如 ra8_csi_v1 或 esp32s3_provisional")
    parser.add_argument("--lighting", required=True, choices=("day", "dim", "night_ir"))
    parser.add_argument("--bed-set", required=True)
    parser.add_argument("--distance", default="fixed_mount")
    parser.add_argument("--target-fps", type=float, default=2.0)
    parser.add_argument("--duration", type=float, default=60.0, help="--camera 模式的秒数")
    parser.add_argument("--near-duplicate-distance", type=int, default=2)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--notes", default="")
    return parser.parse_args()


def validate_identifier(value: str, field: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", value):
        raise SystemExit(f"{field} 只能使用字母、数字、点、下划线和短横线，长度 1~64")
    return value


def append_session_row(root: Path, metadata: dict[str, object]) -> None:
    path = root / "capture_sessions.csv"
    fields = [
        "session_id",
        "doll_id",
        "camera_id",
        "lighting",
        "bed_set",
        "distance",
        "captured_at_utc",
        "frame_count",
        "source",
        "notes",
    ]
    exists = path.exists()
    with path.open("a", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        if not exists:
            writer.writeheader()
        writer.writerow({field: metadata.get(field, "") for field in fields})


def save_if_distinct(
    image: Image.Image,
    frame_dir: Path,
    index: int,
    previous_hash: int | None,
    maximum_distance: int,
) -> tuple[bool, int]:
    current_hash = difference_hash(image)
    if previous_hash is not None and hamming_distance(previous_hash, current_hash) <= maximum_distance:
        # Keep comparing against the last image that was actually saved.  If
        # every skipped frame became the new reference, gradual movement could
        # be suppressed forever even after it accumulated into a useful pose.
        return False, previous_hash
    image.convert("RGB").save(frame_dir / f"frame_{index:06d}.jpg", quality=92, optimize=True)
    return True, current_hash


def capture_from_images(args: argparse.Namespace, frame_dir: Path) -> int:
    paths = sorted(
        path for path in args.image_dir.rglob("*") if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES
    )
    saved = 0
    previous_hash = None
    for path in paths:
        with Image.open(path) as image:
            kept, previous_hash = save_if_distinct(
                image,
                frame_dir,
                saved,
                previous_hash,
                args.near_duplicate_distance,
            )
            saved += int(kept)
    return saved


def capture_with_opencv(args: argparse.Namespace, frame_dir: Path) -> int:
    try:
        import cv2
    except ImportError as exc:
        raise SystemExit("请先运行 setup.ps1 安装 OpenCV") from exc

    source = str(args.video) if args.video is not None else args.camera
    capture = cv2.VideoCapture(source)
    if not capture.isOpened():
        raise SystemExit(f"无法打开采集源: {source}")

    source_fps = capture.get(cv2.CAP_PROP_FPS)
    if not source_fps or source_fps <= 0:
        source_fps = 30.0
    interval = max(1, round(source_fps / args.target_fps))
    deadline = time.monotonic() + args.duration if args.camera is not None else None
    frame_index = 0
    saved = 0
    previous_hash = None
    try:
        while deadline is None or time.monotonic() < deadline:
            ok, frame = capture.read()
            if not ok:
                break
            if frame_index % interval == 0:
                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                image = Image.fromarray(rgb)
                kept, previous_hash = save_if_distinct(
                    image,
                    frame_dir,
                    saved,
                    previous_hash,
                    args.near_duplicate_distance,
                )
                saved += int(kept)
            frame_index += 1
    finally:
        capture.release()
    return saved


def main() -> int:
    args = parse_args()
    args.session_id = validate_identifier(args.session_id, "session-id")
    args.doll_id = validate_identifier(args.doll_id, "doll-id")
    if args.target_fps <= 0 or args.target_fps > 10:
        raise SystemExit("target-fps 应在 (0, 10] 范围")

    session_dir = args.output_root / "sessions" / args.session_id
    if session_dir.exists():
        raise SystemExit(f"场次已存在，不覆盖: {session_dir}")
    frame_dir = session_dir / "frames"
    frame_dir.mkdir(parents=True)

    try:
        if args.image_dir is not None:
            saved = capture_from_images(args, frame_dir)
            source = str(args.image_dir.resolve())
        else:
            saved = capture_with_opencv(args, frame_dir)
            source = str(args.video.resolve()) if args.video is not None else f"camera:{args.camera}"
    except Exception:
        shutil.rmtree(session_dir, ignore_errors=True)
        raise

    metadata: dict[str, object] = {
        "session_id": args.session_id,
        "doll_id": args.doll_id,
        "camera_id": args.camera_id,
        "lighting": args.lighting,
        "bed_set": args.bed_set,
        "distance": args.distance,
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "frame_count": saved,
        "source": source,
        "target_fps": args.target_fps,
        "near_duplicate_distance": args.near_duplicate_distance,
        "notes": args.notes,
    }
    (session_dir / "session.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    append_session_row(args.output_root, metadata)
    print(f"场次完成: {args.session_id}, 保留 {saved} 帧，目录 {frame_dir}")
    print("下一步：用 CVAT 标场景框；再复制 templates/frame_annotations.csv 填写分类 ROI 和标签。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

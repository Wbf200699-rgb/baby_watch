from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

import soundfile as sf
import yaml

from audio_pipeline.common import DATA_ROOT, ROOT, load_config, sha256_file, write_json
from audio_pipeline.manifest import AUDIO_EXTENSIONS


MENDELEY_ARCHIVE = "mendeley_infants_cry_v1.zip"
INFANTCRY_DBL_ARCHIVE = "infantcry_dbl_v1.zip"
TUT_SEGMENT_FIELDS = [
    "segment_file",
    "source_file",
    "segment_index",
    "start_seconds",
    "end_seconds",
    "license",
    "source_url",
    "username",
]
SUPPORTED_DATASETS = {
    "esc50",
    "donateacry",
    "mendeley",
    "infantcry_dbl",
    "tut_rare_events",
    "ravdess_speech",
}


def run_git(arguments: list[str]) -> str:
    run = subprocess.run(
        ["git", *arguments],
        text=True,
        capture_output=True,
        check=False,
    )
    if run.returncode:
        raise RuntimeError(run.stderr.strip() or "git failed")
    return run.stdout.strip()


def clone_repository(url: str, destination: Path) -> dict:
    if (destination / ".git").exists():
        state = "already_present"
    else:
        if destination.exists() and any(destination.iterdir()):
            raise RuntimeError(f"{destination} exists but is not a Git repository")
        destination.parent.mkdir(parents=True, exist_ok=True)
        run_git(["clone", "--depth", "1", url, str(destination)])
        state = "downloaded"
    revision = run_git(["-C", str(destination), "rev-parse", "HEAD"])
    return {"state": state, "revision": revision}


def download_file(url: str, destination: Path) -> dict:
    if destination.exists() and destination.stat().st_size:
        return {
            "state": "already_present",
            "bytes": destination.stat().st_size,
            "sha256": sha256_file(destination),
        }
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "RA8P1-audio-dataset-preparer/1"})
    with urllib.request.urlopen(request, timeout=120) as response, temporary.open("wb") as output:
        shutil.copyfileobj(response, output, length=1024 * 1024)
    temporary.replace(destination)
    return {
        "state": "downloaded",
        "bytes": destination.stat().st_size,
        "sha256": sha256_file(destination),
    }


def safe_extract(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    root = destination.resolve()
    with zipfile.ZipFile(archive) as handle:
        for member in handle.infolist():
            target = (destination / member.filename).resolve()
            if root != target and root not in target.parents:
                raise RuntimeError(f"unsafe archive path: {member.filename}")
        handle.extractall(destination)


def md5_file(path: Path) -> str:
    digest = hashlib.md5()  # noqa: S324 - required to verify the published archive.
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def extract_tut_babycry_sources(archive: Path, destination: Path) -> int:
    """Extract only the annotated baby-cry source recordings from the 639 MB archive."""
    destination.mkdir(parents=True, exist_ok=True)
    extracted = 0
    pattern = re.compile(r"/events/babycry/([^/]+\.(?:wav|yaml))$", re.IGNORECASE)
    with zipfile.ZipFile(archive) as handle:
        for member in handle.infolist():
            match = pattern.search(member.filename.replace("\\", "/"))
            if not match:
                continue
            target = destination / match.group(1)
            if target.exists() and target.stat().st_size == member.file_size:
                extracted += 1
                continue
            with handle.open(member) as source, target.open("wb") as output:
                shutil.copyfileobj(source, output, length=1024 * 1024)
            extracted += 1
    return extracted


def _load_tut_metadata(path: Path) -> dict:
    # The original Python-2 YAML uses an unsafe legacy unicode tag. Removing
    # that scalar tag preserves the text while keeping parsing in safe mode.
    text = path.read_text(encoding="utf-8")
    text = text.replace("!!python/unicode ", "")
    value = yaml.safe_load(text)
    if not isinstance(value, dict):
        raise RuntimeError(f"invalid TUT metadata: {path}")
    return value


def prepare_tut_segments(destination: Path) -> list[dict[str, str]]:
    """Materialize only annotated event spans; source files contain non-event audio too."""
    segment_root = destination / "segments"
    segment_root.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, str]] = []
    for metadata_path in sorted(destination.glob("*.yaml")):
        metadata = _load_tut_metadata(metadata_path)
        source_path = metadata_path.with_suffix(".wav")
        if not source_path.exists():
            raise RuntimeError(f"TUT source WAV is missing: {source_path}")
        samples, sample_rate = sf.read(source_path, dtype="float32", always_2d=True)
        for index, segment in enumerate(metadata.get("valid_segments", []), start=1):
            if not isinstance(segment, list) or len(segment) != 2:
                raise RuntimeError(f"invalid TUT segment in {metadata_path}: {segment!r}")
            start_seconds, end_seconds = (float(segment[0]), float(segment[1]))
            start = max(0, int(round(start_seconds * sample_rate)))
            end = min(len(samples), int(round(end_seconds * sample_rate)))
            if end <= start:
                raise RuntimeError(f"empty TUT segment in {metadata_path}: {segment!r}")
            segment_path = segment_root / f"{source_path.stem}_{index:03d}.wav"
            sf.write(segment_path, samples[start:end], sample_rate, subtype="PCM_16")
            rows.append(
                {
                    "segment_file": segment_path.relative_to(destination).as_posix(),
                    "source_file": source_path.name,
                    "segment_index": str(index),
                    "start_seconds": f"{start_seconds:.6f}",
                    "end_seconds": f"{end_seconds:.6f}",
                    "license": str(metadata.get("license", "")),
                    "source_url": str(metadata.get("url", "")),
                    "username": str(metadata.get("username", "")),
                }
            )
    index_path = destination / "segments.csv"
    with index_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=TUT_SEGMENT_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    return rows


def audio_count(root: Path) -> int:
    if not root.exists():
        return 0
    return sum(
        path.is_file() and path.suffix.lower() in AUDIO_EXTENSIONS
        for path in root.rglob("*")
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Download public audio datasets used by the two-stage cry models."
    )
    parser.add_argument(
        "datasets",
        nargs="*",
        default=None,
        metavar="DATASET",
        help=(
            "Any of: esc50, donateacry, mendeley, infantcry_dbl, tut_rare_events, "
            "ravdess_speech "
            "(default: all)"
        ),
    )
    parser.add_argument("--config", type=Path, default=ROOT / "audio_two_stage.yaml")
    parser.add_argument("--data-root", type=Path, default=DATA_ROOT)
    args = parser.parse_args()

    config = load_config(args.config)
    raw = args.data_root.resolve() / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    selected = args.datasets or [
        "esc50",
        "donateacry",
        "mendeley",
        "infantcry_dbl",
        "tut_rare_events",
        "ravdess_speech",
    ]
    unknown = sorted(set(selected) - SUPPORTED_DATASETS)
    if unknown:
        parser.error(f"unknown dataset(s): {', '.join(unknown)}")
    receipt: dict[str, object] = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "datasets": {},
    }

    if "esc50" in selected:
        spec = config["datasets"]["esc50"]
        destination = raw / spec["directory"]
        result = clone_repository(spec["url"], destination)
        result.update(
            {
                "audio_files": audio_count(destination / "audio"),
                "license": spec["license"],
                "source_url": spec["url"],
            }
        )
        receipt["datasets"]["esc50"] = result

    if "donateacry" in selected:
        spec = config["datasets"]["donateacry"]
        destination = raw / spec["directory"]
        result = clone_repository(spec["url"], destination)
        result.update(
            {
                "audio_files": audio_count(destination),
                "license": spec["license"],
                "source_url": spec["url"],
            }
        )
        receipt["datasets"]["donateacry"] = result

    if "mendeley" in selected:
        spec = config["datasets"]["mendeley_infants_cry"]
        archive = raw / MENDELEY_ARCHIVE
        destination = raw / spec["directory"]
        result = download_file(spec["url"], archive)
        if not destination.exists() or not any(destination.iterdir()):
            safe_extract(archive, destination)
            result["extract_state"] = "extracted"
        else:
            result["extract_state"] = "already_present"
        result.update(
            {
                "audio_files": audio_count(destination),
                "license": spec["license"],
                "source_url": spec["url"],
                "doi": spec["doi"],
            }
        )
        receipt["datasets"]["mendeley_infants_cry"] = result

    if "infantcry_dbl" in selected:
        spec = config["datasets"]["infantcry_dbl"]
        archive = raw / INFANTCRY_DBL_ARCHIVE
        destination = raw / spec["directory"]
        result = download_file(spec["url"], archive)
        expected = int(spec["expected_audio_files"])
        before = audio_count(destination)
        if before != expected:
            safe_extract(archive, destination)
            result["extract_state"] = "extracted"
        else:
            result["extract_state"] = "already_present"
        found = audio_count(destination)
        if found != expected:
            raise RuntimeError(
                f"InfantCry-DBL expected {expected} audio files, found {found}"
            )
        result.update(
            {
                "audio_files": found,
                "license": spec["license"],
                "source_url": spec["url"],
                "doi": spec["doi"],
            }
        )
        receipt["datasets"]["infantcry_dbl"] = result

    if "tut_rare_events" in selected:
        spec = config["datasets"]["tut_rare_events"]
        archive = raw / spec["archive"]
        destination = raw / spec["directory"]
        result = download_file(spec["url"], archive)
        actual_md5 = md5_file(archive)
        expected_md5 = str(spec["expected_archive_md5"]).lower()
        if actual_md5 != expected_md5:
            raise RuntimeError(
                f"TUT archive MD5 mismatch: expected {expected_md5}, got {actual_md5}"
            )
        extracted_files = extract_tut_babycry_sources(archive, destination)
        segments = prepare_tut_segments(destination)
        expected_segments = int(spec["expected_segments"])
        if len(segments) != expected_segments:
            raise RuntimeError(
                f"TUT expected {expected_segments} annotated segments, found {len(segments)}"
            )
        result.update(
            {
                "archive_md5": actual_md5,
                "extracted_source_files": extracted_files,
                "annotated_segments": len(segments),
                "license": spec["license"],
                "source_url": spec["url"],
                "doi": spec["doi"],
            }
        )
        receipt["datasets"]["tut_rare_events"] = result

    if "ravdess_speech" in selected:
        spec = config["datasets"]["ravdess_speech"]
        archive = raw / spec["archive"]
        destination = raw / spec["directory"]
        result = download_file(spec["url"], archive)
        actual_md5 = md5_file(archive)
        expected_md5 = str(spec["expected_archive_md5"]).lower()
        if actual_md5 != expected_md5:
            raise RuntimeError(
                f"RAVDESS archive MD5 mismatch: expected {expected_md5}, got {actual_md5}"
            )
        expected = int(spec["expected_audio_files"])
        if audio_count(destination) != expected:
            safe_extract(archive, destination)
            result["extract_state"] = "extracted"
        else:
            result["extract_state"] = "already_present"
        found = audio_count(destination)
        if found != expected:
            raise RuntimeError(f"RAVDESS expected {expected} audio files, found {found}")
        result.update(
            {
                "archive_md5": actual_md5,
                "audio_files": found,
                "license": spec["license"],
                "source_url": spec["url"],
                "doi": spec["doi"],
            }
        )
        receipt["datasets"]["ravdess_speech"] = result

    receipt_path = raw / "download_receipt.json"
    write_json(receipt_path, receipt)
    print(json.dumps(receipt, ensure_ascii=False, indent=2))
    print(f"Receipt: {receipt_path}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, zipfile.BadZipFile) as exc:
        print(f"Dataset download failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc

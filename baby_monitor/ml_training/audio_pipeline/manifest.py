from __future__ import annotations

import csv
import hashlib
import math
import re
import subprocess
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable

import numpy as np
import soundfile as sf

from .common import DATA_ROOT, read_csv, relative_or_absolute, resolve_data_path, stable_id
from .features import dbfs


AUDIO_EXTENSIONS = {".wav", ".caf", ".3gp", ".m4a", ".mp3", ".aac", ".flac", ".ogg"}
TYPE_LABELS = {"hungry", "belly_pain", "discomfort", "tired", "burping", "unknown"}
DONATE_REASON_MAP = {
    "hu": "hungry",
    "bp": "belly_pain",
    "dc": "discomfort",
    "ti": "tired",
    "bu": "burping",
    "lo": "unknown",
    "ch": "unknown",
    "sc": "unknown",
    "dk": "unknown",
}
MENDELEY_LABEL_MAP = {
    "hungry": "hungry",
    "tired": "tired",
    "uncomfortable": "discomfort",
    "discomfort": "discomfort",
}
INFANTCRY_DBL_LABEL_MAP = {
    "neh": "hungry",
    "eairh": "belly_pain",
    "heh": "discomfort",
    "owh": "tired",
    "eh": "burping",
}
DONATE_PATTERN = re.compile(
    r"(?P<instance>[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"
    r"-(?P<timestamp>\d+)-(?P<version>[0-9.]+)-(?P<gender>[mf])-(?P<age>04|48|26|72|22)"
    r"-(?P<reason>hu|bu|bp|dc|ti|lo|ch|sc|dk)(?:\.|$)",
    re.IGNORECASE,
)
SELECTED_PARTITION_PATTERN = re.compile(r"^[CN](TR|VA|TE)(?:_|$)", re.IGNORECASE)
SELECTED_PARTITIONS = {
    "TR": "train",
    "VA": "validation",
    "TE": "test",
}
SELECTED_TYPE_LABELS = {
    "hungry",
    "belly_pain",
    "discomfort",
    "tired",
    "burping",
}
RAVDESS_EMOTIONS = {
    "01": "neutral",
    "02": "calm",
    "03": "happy",
    "04": "sad",
    "05": "angry",
    "06": "fearful",
    "07": "disgust",
    "08": "surprised",
}
RAVDESS_PATTERN = re.compile(
    r"^03-01-(?P<emotion>0[1-8])-(?P<intensity>0[12])-"
    r"(?P<statement>0[12])-(?P<repetition>0[12])-(?P<actor>\d{2})\.wav$",
    re.IGNORECASE,
)

MANIFEST_FIELDS = [
    "recording_id",
    "source",
    "source_path",
    "standardized_path",
    "source_sha256",
    "wave_sha256",
    "detection_label",
    "classification_label",
    "original_label",
    "label_confidence",
    "group_id",
    "split_group",
    "baby_id",
    "session_id",
    "split",
    "license",
    "sample_rate_hz",
    "channels",
    "duration_seconds",
    "rms_dbfs",
    "peak",
    "clipped_fraction",
    "dc_offset",
    "quality_status",
    "quality_reasons",
    "source_partition",
]


def _base_row(
    source: str,
    source_path: Path,
    data_root: Path,
    *,
    detection_label: str,
    classification_label: str = "",
    original_label: str = "",
    label_confidence: str = "",
    group_id: str = "",
    baby_id: str = "",
    session_id: str = "",
    license_name: str = "",
    source_partition: str = "",
) -> dict[str, str]:
    relative = relative_or_absolute(source_path, data_root)
    recording_id = stable_id(source, relative)
    return {
        "recording_id": recording_id,
        "source": source,
        "source_path": relative,
        "standardized_path": "",
        "source_sha256": "",
        "wave_sha256": "",
        "detection_label": detection_label,
        "classification_label": classification_label,
        "original_label": original_label,
        "label_confidence": label_confidence,
        "group_id": group_id or f"{source}:recording:{recording_id}",
        "split_group": "",
        "baby_id": baby_id,
        "session_id": session_id,
        "split": "",
        "license": license_name,
        "sample_rate_hz": "",
        "channels": "",
        "duration_seconds": "",
        "rms_dbfs": "",
        "peak": "",
        "clipped_fraction": "",
        "dc_offset": "",
        "quality_status": "pending",
        "quality_reasons": "",
        "source_partition": source_partition,
    }


def _audio_files(root: Path) -> list[Path]:
    if not root.exists():
        return []
    return sorted(
        path for path in root.rglob("*")
        if path.is_file() and path.suffix.lower() in AUDIO_EXTENSIONS
    )


def scan_esc50(config: dict[str, Any], data_root: Path) -> list[dict[str, str]]:
    dataset = config["datasets"]["esc50"]
    root = data_root / "raw" / dataset["directory"]
    metadata = root / "meta" / "esc50.csv"
    if not metadata.exists():
        return []
    rows: list[dict[str, str]] = []
    with metadata.open(newline="", encoding="utf-8") as handle:
        for item in csv.DictReader(handle):
            path = root / "audio" / item["filename"]
            if not path.exists():
                continue
            category = item["category"].strip()
            rows.append(
                _base_row(
                    "esc50",
                    path,
                    data_root,
                    detection_label="cry" if category == "crying_baby" else "non_cry",
                    original_label=category,
                    label_confidence="curated",
                    group_id=f"esc50:source:{item.get('src_file') or path.stem}",
                    license_name=dataset["license"],
                    source_partition=item.get("fold", ""),
                )
            )
    return rows


def _donate_metadata(path: Path) -> tuple[str, str, str, str]:
    match = DONATE_PATTERN.search(path.name)
    if match:
        reason = match.group("reason").lower()
        instance = match.group("instance").lower()
        return reason, instance, match.group("gender").lower(), match.group("age")
    reason_match = re.search(r"(?:^|[-_])(hu|bu|bp|dc|ti|lo|ch|sc|dk)(?:[-_.]|$)", path.name, re.I)
    if reason_match:
        return reason_match.group(1).lower(), "", "", ""
    for part in reversed(path.parts):
        key = part.lower().strip().replace(" ", "_")
        if key in DONATE_REASON_MAP:
            return key, "", "", ""
        if key in TYPE_LABELS:
            reverse = {
                "hungry": "hu",
                "belly_pain": "bp",
                "discomfort": "dc",
                "tired": "ti",
                "burping": "bu",
                "unknown": "dk",
            }
            return reverse[key], "", "", ""
    return "", "", "", ""


def scan_donateacry(config: dict[str, Any], data_root: Path) -> list[dict[str, str]]:
    dataset = config["datasets"]["donateacry"]
    root = data_root / "raw" / dataset["directory"]
    preferred = root / "donateacry_corpus_cleaned_and_updated_data"
    scan_root = preferred if _audio_files(preferred) else root
    rows: list[dict[str, str]] = []
    for path in _audio_files(scan_root):
        reason, instance, _gender, _age = _donate_metadata(path)
        if not reason:
            continue
        classification = DONATE_REASON_MAP[reason]
        recording_key = stable_id(relative_or_absolute(path, root))
        rows.append(
            _base_row(
                "donateacry",
                path,
                data_root,
                detection_label="cry",
                classification_label=classification,
                original_label=reason,
                label_confidence="contributor_reported",
                group_id=f"donateacry:app:{instance}" if instance else f"donateacry:recording:{recording_key}",
                baby_id=instance,
                license_name=dataset["license"],
            )
        )
    return rows


def scan_mendeley(config: dict[str, Any], data_root: Path) -> list[dict[str, str]]:
    dataset = config["datasets"]["mendeley_infants_cry"]
    root = data_root / "raw" / dataset["directory"]
    rows: list[dict[str, str]] = []
    for path in _audio_files(root):
        label = ""
        for part in reversed(path.parts):
            normalized = part.lower().strip().replace(" ", "_")
            if normalized in MENDELEY_LABEL_MAP:
                label = MENDELEY_LABEL_MAP[normalized]
                break
        if not label:
            continue
        stem = path.stem.lower().replace(" ", "_")
        identity_match = re.match(r"(?:hung|uncom)_([a-z]+)", stem)
        if identity_match:
            identity = re.sub(r"\d+$", "", identity_match.group(1))
        else:
            aud_match = re.search(r"aud-(\d{8})", stem)
            baby_match = re.search(r"baby[-_ ]?(\d+)", stem)
            if aud_match:
                identity = f"aud_session_{aud_match.group(1)}"
            elif baby_match:
                identity = f"baby_{baby_match.group(1)}"
            else:
                identity = f"recording_{stable_id(relative_or_absolute(path, root))}"
        anonymous_identity = stable_id("mendeley", identity, length=16)
        rows.append(
            _base_row(
                "mendeley_infants_cry",
                path,
                data_root,
                detection_label="cry",
                classification_label=label,
                original_label=label,
                label_confidence="dataset_label",
                group_id=f"mendeley:identity:{anonymous_identity}",
                license_name=dataset["license"],
            )
        )
    return rows


def scan_infantcry_dbl(
    config: dict[str, Any],
    data_root: Path,
) -> list[dict[str, str]]:
    """Read InfantCry-DBL using its authoritative folder labels.

    The v1 archive has one known filename/folder disagreement, and its
    ``metadata.csv`` spelling for the Tier-2 directory differs from the archive.
    Joining metadata by the globally unique filename avoids both failure modes.
    Tier 1 is canonical instructional audio with no usable infant identity, so
    it is training-only. Tier-2 clips are split as a group by the anonymised
    clip/infant ID recommended by the dataset authors.
    """
    dataset = config["datasets"].get("infantcry_dbl")
    if not dataset:
        return []
    root = data_root / "raw" / dataset["directory"]
    metadata_paths = sorted(root.rglob("metadata.csv")) if root.exists() else []
    if not metadata_paths:
        return []
    if len(metadata_paths) != 1:
        raise ValueError(
            f"{root}: expected one InfantCry-DBL metadata.csv, found "
            f"{len(metadata_paths)}"
        )

    metadata_rows = read_csv(metadata_paths[0])
    by_filename: dict[str, dict[str, str]] = {}
    for item in metadata_rows:
        filename = item.get("filename", "").strip()
        if not filename:
            raise ValueError(f"{metadata_paths[0]}: row without filename")
        key = filename.casefold()
        if key in by_filename:
            raise ValueError(f"{metadata_paths[0]}: duplicate filename {filename!r}")
        by_filename[key] = item

    audio_paths = _audio_files(root)
    by_audio_name: dict[str, Path] = {}
    for path in audio_paths:
        key = path.name.casefold()
        if key in by_audio_name:
            raise ValueError(f"{root}: duplicate audio filename {path.name!r}")
        by_audio_name[key] = path
    missing = sorted(set(by_filename) - set(by_audio_name))
    unexpected = sorted(set(by_audio_name) - set(by_filename))
    if missing or unexpected:
        raise ValueError(
            f"{root}: InfantCry-DBL manifest/audio mismatch "
            f"(missing={len(missing)}, unexpected={len(unexpected)})"
        )

    rows: list[dict[str, str]] = []
    for filename_key, item in sorted(by_filename.items()):
        path = by_audio_name[filename_key]
        folder_label = path.parent.name.strip().lower()
        classification = INFANTCRY_DBL_LABEL_MAP.get(folder_label)
        if not classification:
            raise ValueError(
                f"{path}: unsupported InfantCry-DBL folder label {folder_label!r}"
            )
        metadata_label = item.get("class", "").strip().lower()
        if metadata_label and metadata_label != folder_label:
            raise ValueError(
                f"{path}: folder label {folder_label!r} disagrees with "
                f"metadata label {metadata_label!r}"
            )

        tier = item.get("tier", "").strip()
        clip_id = item.get("clip_id", "").strip()
        gender = item.get("gender", "").strip().upper()
        if tier not in {"1", "2"} or not clip_id:
            raise ValueError(f"{path}: invalid tier/clip_id metadata")
        normalized_clip_id = (
            str(int(clip_id)) if clip_id.isdigit() else clip_id.casefold()
        )
        if tier == "1":
            source = "infantcry_dbl_tier1"
            group_id = (
                "infantcry_dbl:tier1:clip:"
                f"{folder_label}:{normalized_clip_id}"
            )
            baby_id = ""
            confidence = "canonical_dbl_exemplar"
            partition = "train"
        else:
            source = "infantcry_dbl_tier2"
            identity = stable_id(
                "infantcry_dbl",
                "tier2",
                normalized_clip_id,
                gender,
                length=16,
            )
            group_id = f"infantcry_dbl:tier2:infant:{identity}"
            baby_id = identity
            confidence = "dual_annotator_consensus"
            partition = ""
        rows.append(
            _base_row(
                source,
                path,
                data_root,
                detection_label="cry",
                classification_label=classification,
                original_label=folder_label,
                label_confidence=confidence,
                group_id=group_id,
                baby_id=baby_id,
                license_name=dataset["license"],
                source_partition=partition,
            )
        )
    return rows


def scan_tut_rare_events(
    config: dict[str, Any],
    data_root: Path,
) -> list[dict[str, str]]:
    """Read the annotated baby-cry spans without labeling surrounding silence."""
    dataset = config["datasets"].get("tut_rare_events")
    if not dataset:
        return []
    root = data_root / "raw" / dataset["directory"]
    index_path = root / "segments.csv"
    if not index_path.exists():
        return []
    rows: list[dict[str, str]] = []
    for item in read_csv(index_path):
        segment_value = item.get("segment_file", "").strip()
        source_file = item.get("source_file", "").strip()
        if not segment_value or not source_file:
            continue
        path = root / Path(segment_value)
        if not path.exists():
            continue
        rows.append(
            _base_row(
                "tut_rare_events_babycry",
                path,
                data_root,
                detection_label="cry",
                original_label="babycry",
                label_confidence="expert_temporal_annotation",
                group_id=f"tut_rare_events:source:{Path(source_file).stem}",
                license_name=item.get("license", dataset["license"]),
            )
        )
    return rows


def scan_ravdess_speech(
    config: dict[str, Any],
    data_root: Path,
) -> list[dict[str, str]]:
    """Add acted adult speech and shouting as grouped non-cry hard negatives."""
    dataset = config["datasets"].get("ravdess_speech")
    if not dataset:
        return []
    root = data_root / "raw" / dataset["directory"]
    rows: list[dict[str, str]] = []
    for path in _audio_files(root):
        match = RAVDESS_PATTERN.match(path.name)
        if not match:
            continue
        actor = match.group("actor")
        actor_number = int(actor)
        emotion = RAVDESS_EMOTIONS[match.group("emotion")]
        intensity = "strong" if match.group("intensity") == "02" else "normal"
        if actor_number <= 16:
            partition = "train"
        elif actor_number <= 20:
            partition = "validation"
        else:
            partition = "test"
        rows.append(
            _base_row(
                "ravdess_speech",
                path,
                data_root,
                detection_label="non_cry",
                original_label=f"adult_{emotion}_{intensity}",
                label_confidence="curated_acted_speech",
                group_id=f"ravdess_speech:actor:{actor}",
                session_id=f"actor_{actor}",
                license_name=dataset["license"],
                source_partition=partition,
            )
        )
    return rows


def scan_self_recorded(config: dict[str, Any], data_root: Path) -> list[dict[str, str]]:
    dataset = config["datasets"]["self_recorded"]
    root = data_root / "raw" / dataset["directory"]
    metadata = root / "metadata.csv"
    if not metadata.exists():
        return []
    rows: list[dict[str, str]] = []
    for item in read_csv(metadata):
        filename = item.get("filename", "").strip()
        if not filename:
            continue
        path = root / filename
        if not path.exists():
            continue
        classification = item.get("classification_label", item.get("label", "")).strip()
        if classification and classification not in TYPE_LABELS:
            continue
        detection = item.get("detection_label", "").strip()
        if not detection:
            detection = "cry" if classification else "non_cry"
        baby_id = item.get("baby_id", "").strip()
        session_id = item.get("session_id", "").strip()
        identity = baby_id or session_id or stable_id(filename)
        explicit_group = item.get("group_id", "").strip()
        rows.append(
            _base_row(
                "self_recorded",
                path,
                data_root,
                detection_label=detection,
                classification_label=classification,
                original_label=item.get("original_label", classification),
                label_confidence=item.get("label_confidence", "self_recorded"),
                group_id=explicit_group or f"self:{identity}",
                baby_id=baby_id,
                session_id=session_id,
                license_name=item.get("license", dataset["license"]),
                source_partition=item.get("source_partition", ""),
            )
        )
    return rows


def _selected_detection_metadata(path: Path, root: Path) -> tuple[str, str, str]:
    """Return detection label, fixed split and scene for the compact field naming scheme."""
    relative = path.relative_to(root)
    folders = {part.lower() for part in relative.parts[:-1]}
    if "cry" in folders:
        label = "cry"
        scene = "cry"
    elif "qt" in folders:
        label = "non_cry"
        scene = "quiet"
    elif "tk" in folders:
        label = "non_cry"
        scene = "talk"
    else:
        return "", "", ""

    match = SELECTED_PARTITION_PATTERN.match(path.stem)
    if not match:
        return "", "", ""
    partition = SELECTED_PARTITIONS[match.group(1).upper()]

    # The leading C/N is a second label guard against a misplaced file.
    if (path.stem[0].upper() == "C") != (label == "cry"):
        return "", "", ""
    return label, partition, scene


def _selected_type_metadata(path: Path, root: Path) -> tuple[str, str]:
    """Return the verified reason label and fixed split for RA8P1 playback captures."""
    relative = path.relative_to(root)
    if len(relative.parts) < 3 or relative.parts[0] != "2":
        return "", ""
    label = relative.parts[1].lower()
    if label not in SELECTED_TYPE_LABELS:
        return "", ""
    if path.parent.name.lower() != label:
        return "", ""
    if not re.fullmatch(rf"{re.escape(label)}_\d+", path.stem, re.IGNORECASE):
        return "", ""

    # Batch 2 was recorded by replaying only the already-fixed public train
    # playlists. Keep every microphone-domain derivative in train so transformed
    # copies of public validation/test recordings cannot leak into training.
    return label, "train"


def scan_device_selected(config: dict[str, Any], data_root: Path) -> list[dict[str, str]]:
    dataset = config["datasets"].get("device_selected")
    if not dataset:
        return []
    root = data_root / dataset["directory"]
    rows: list[dict[str, str]] = []
    for path in _audio_files(root):
        detection, partition, scene = _selected_detection_metadata(path, root)
        relative = relative_or_absolute(path, root)
        relative_parts = path.relative_to(root).parts
        batch = relative_parts[0] if len(relative_parts) > 1 else "default"
        if detection:
            rows.append(
                _base_row(
                    "device_selected_ra8p1",
                    path,
                    data_root,
                    detection_label=detection,
                    original_label=scene,
                    label_confidence="user_verified",
                    group_id=f"device_selected:session:{batch}:{partition}:{scene}",
                    session_id=f"batch_{batch}_{partition}_{scene}",
                    license_name=dataset["license"],
                    source_partition=partition,
                )
            )
            continue

        classification, partition = _selected_type_metadata(path, root)
        if classification:
            rows.append(
                _base_row(
                    "device_selected_ra8p1_type",
                    path,
                    data_root,
                    detection_label="cry",
                    classification_label=classification,
                    original_label=classification,
                    label_confidence="user_verified_playback",
                    group_id=f"device_selected:type_session:{batch}:{classification}",
                    session_id=f"batch_{batch}_{classification}",
                    license_name=dataset["license"],
                    source_partition=partition,
                )
            )
    return rows


def scan_external_manifest(config: dict[str, Any], data_root: Path) -> list[dict[str, str]]:
    manifest_name = config["datasets"]["external"]["manifest"]
    metadata = data_root / "raw" / manifest_name
    if not metadata.exists():
        return []
    rows: list[dict[str, str]] = []
    for item in read_csv(metadata):
        value = item.get("source_path", item.get("filename", "")).strip()
        if not value:
            continue
        path = resolve_data_path(value, data_root)
        if not path.exists():
            alternate = data_root / "raw" / value
            path = alternate if alternate.exists() else path
        if not path.exists() or path.suffix.lower() not in AUDIO_EXTENSIONS:
            continue
        source = item.get("source", "external").strip() or "external"
        classification = item.get("classification_label", "").strip()
        detection = item.get("detection_label", "").strip()
        if not detection:
            detection = "cry" if classification else "non_cry"
        rows.append(
            _base_row(
                source,
                path,
                data_root,
                detection_label=detection,
                classification_label=classification,
                original_label=item.get("original_label", classification),
                label_confidence=item.get("label_confidence", ""),
                group_id=item.get("group_id", ""),
                baby_id=item.get("baby_id", ""),
                session_id=item.get("session_id", ""),
                license_name=item.get("license", ""),
                source_partition=item.get("source_partition", ""),
            )
        )
    return rows


def _apply_frozen_public_partitions(
    rows: list[dict[str, str]],
    data_root: Path,
) -> None:
    """Keep the public split used to create the playback playlists immutable."""
    index_path = (
        data_root
        / "ready_to_play"
        / "public_cry_wav"
        / "index.csv"
    )
    if not index_path.exists():
        return
    frozen = {
        (item.get("source", ""), item.get("recording_id", "")): item.get("split", "")
        for item in read_csv(index_path)
        if item.get("split", "") in SELECTED_PARTITIONS.values()
    }
    for row in rows:
        partition = frozen.get((row["source"], row["recording_id"]))
        if partition:
            row["source_partition"] = partition


def build_recording_manifest(
    config: dict[str, Any],
    data_root: Path = DATA_ROOT,
) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for scanner in (
        scan_esc50,
        scan_donateacry,
        scan_mendeley,
        scan_infantcry_dbl,
        scan_tut_rare_events,
        scan_ravdess_speech,
        scan_self_recorded,
        scan_device_selected,
        scan_external_manifest,
    ):
        rows.extend(scanner(config, data_root))
    _apply_frozen_public_partitions(rows, data_root)
    unique: dict[tuple[str, str], dict[str, str]] = {}
    for row in rows:
        unique[(row["source"], row["source_path"].lower())] = row
    return sorted(unique.values(), key=lambda row: (row["source"], row["source_path"]))


def _source_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _wave_sha256(samples: np.ndarray) -> str:
    pcm = np.round(np.clip(samples, -1.0, 1.0) * 32767.0).astype("<i2")
    return hashlib.sha256(pcm.tobytes()).hexdigest()


def _standardize_one(
    row: dict[str, str],
    config: dict[str, Any],
    data_root: Path,
    ffmpeg: str,
    force: bool,
) -> dict[str, str]:
    result = dict(row)
    source = resolve_data_path(result["source_path"], data_root)
    output = data_root / "standardized" / result["source"] / f"{result['recording_id']}.wav"
    output.parent.mkdir(parents=True, exist_ok=True)
    reasons: list[str] = []
    try:
        if force or not output.exists():
            temporary = output.with_suffix(".tmp.wav")
            command = [
                ffmpeg,
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-i",
                str(source),
                "-vn",
                "-ac",
                str(config["audio"]["channels"]),
                "-ar",
                str(config["audio"]["sample_rate_hz"]),
                "-c:a",
                "pcm_s16le",
                str(temporary),
            ]
            run = subprocess.run(command, text=True, capture_output=True, check=False)
            if run.returncode:
                raise RuntimeError(run.stderr.strip() or f"ffmpeg returned {run.returncode}")
            temporary.replace(output)
        info = sf.info(output)
        samples, sample_rate = sf.read(output, dtype="float32", always_2d=False)
        values = np.asarray(samples, dtype=np.float32).reshape(-1)
        duration = len(values) / float(sample_rate)
        rms_value = dbfs(values)
        peak = float(np.max(np.abs(values))) if values.size else 0.0
        clipped = float(np.mean(np.abs(values) >= 0.999)) if values.size else 0.0
        dc_offset = float(np.mean(values, dtype=np.float64)) if values.size else 0.0
        quality = config["quality"]
        if info.channels != int(config["audio"]["channels"]):
            reasons.append("channels")
        if sample_rate != int(config["audio"]["sample_rate_hz"]):
            reasons.append("sample_rate")
        if duration < float(quality["minimum_recording_seconds"]):
            reasons.append("too_short")
        if rms_value < float(quality["minimum_rms_dbfs"]):
            reasons.append("digital_silence")
        if clipped > float(quality["maximum_clipped_fraction"]):
            reasons.append("severe_clipping")
        if result["detection_label"] not in {"cry", "non_cry"}:
            reasons.append("invalid_detection_label")
        if result["classification_label"] and result["classification_label"] not in TYPE_LABELS:
            reasons.append("invalid_classification_label")

        result.update(
            {
                "standardized_path": relative_or_absolute(output, data_root),
                "source_sha256": _source_sha256(source),
                "wave_sha256": _wave_sha256(values),
                "sample_rate_hz": str(sample_rate),
                "channels": str(info.channels),
                "duration_seconds": f"{duration:.6f}",
                "rms_dbfs": f"{rms_value:.4f}",
                "peak": f"{peak:.7f}",
                "clipped_fraction": f"{clipped:.8f}",
                "dc_offset": f"{dc_offset:.8f}",
            }
        )
    except Exception as exc:  # Keep the failure visible in the audit manifest.
        reasons.append(f"decode:{type(exc).__name__}")
    result["quality_status"] = "accepted" if not reasons else "rejected"
    result["quality_reasons"] = ";".join(reasons)
    return result


class _UnionFind:
    def __init__(self) -> None:
        self.parent: dict[str, str] = {}

    def find(self, item: str) -> str:
        self.parent.setdefault(item, item)
        if self.parent[item] != item:
            self.parent[item] = self.find(self.parent[item])
        return self.parent[item]

    def union(self, left: str, right: str) -> None:
        a, b = self.find(left), self.find(right)
        if a != b:
            self.parent[max(a, b)] = min(a, b)


def _attach_split_groups(rows: list[dict[str, str]]) -> None:
    union = _UnionFind()
    for row in rows:
        if row["quality_status"] != "accepted":
            continue
        identity = f"identity:{row['group_id']}"
        content = f"wave:{row['wave_sha256']}"
        union.union(identity, content)
    members: dict[str, list[str]] = defaultdict(list)
    for row in rows:
        if row["quality_status"] != "accepted":
            row["split_group"] = ""
            continue
        root = union.find(f"identity:{row['group_id']}")
        members[root].append(row["group_id"])
    component_names = {
        root: f"group_{stable_id(*sorted(set(values)), length=16)}"
        for root, values in members.items()
    }
    for row in rows:
        if row["quality_status"] == "accepted":
            row["split_group"] = component_names[union.find(f"identity:{row['group_id']}")]


def _reject_duplicate_content(rows: list[dict[str, str]]) -> None:
    """Keep exact audio only once and reject contradictory duplicate labels.

    InfantCry-DBL incorporates several upstream public sources, so an identical
    waveform can already exist in the frozen Donate-a-Cry partitions. Keeping
    both copies would overweight that clip, while forcing the new Tier-1 copy
    into train could leak a frozen validation/test waveform. Existing sources
    therefore win over newly imported DBL copies. If two copies disagree on
    the reason label, every copy is rejected rather than selecting a label.
    """
    by_hash: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["quality_status"] == "accepted" and row["wave_sha256"]:
            by_hash[row["wave_sha256"]].append(row)

    def priority(row: dict[str, str]) -> tuple[int, str, str]:
        source = row["source"]
        if not source.startswith("infantcry_dbl_"):
            rank = 0
        elif source == "infantcry_dbl_tier2":
            rank = 1
        else:
            rank = 2
        return rank, source, row["source_path"]

    for duplicate_rows in by_hash.values():
        if len(duplicate_rows) < 2:
            continue
        labels = {
            row["classification_label"]
            for row in duplicate_rows
            if row["classification_label"]
        }
        if len(labels) > 1:
            for row in duplicate_rows:
                row["quality_status"] = "rejected"
                row["quality_reasons"] = "duplicate_label_conflict"
            continue
        keep = min(duplicate_rows, key=priority)
        for row in duplicate_rows:
            if row is keep:
                continue
            row["quality_status"] = "rejected"
            row["quality_reasons"] = "duplicate_content"


def _group_vectors(
    rows: list[dict[str, str]],
) -> tuple[dict[str, Counter[str]], Counter[str]]:
    vectors: dict[str, Counter[str]] = defaultdict(Counter)
    totals: Counter[str] = Counter()
    for row in rows:
        if row["quality_status"] != "accepted":
            continue
        labels = [f"detection:{row['detection_label']}"]
        if row["classification_label"]:
            labels.append(f"type:{row['classification_label']}")
        for label in labels:
            vectors[row["split_group"]][label] += 1
            totals[label] += 1
        vectors[row["split_group"]]["__rows__"] += 1
        totals["__rows__"] += 1
    return dict(vectors), totals


def _split_cost(
    split_name: str,
    vector: Counter[str],
    current: dict[str, Counter[str]],
    targets: dict[str, dict[str, float]],
    labels: Iterable[str],
) -> float:
    score = 0.0
    for label in labels:
        target = targets[split_name][label]
        before = float(current[split_name][label])
        after = before + float(vector[label])
        score += ((after - target) ** 2 - (before - target) ** 2) / (target + 1.0)
    return score


def _assign_splits(rows: list[dict[str, str]], config: dict[str, Any]) -> None:
    split_config = config["split"]
    ratios = {
        "train": float(split_config["train"]),
        "validation": float(split_config["validation"]),
        "test": float(split_config["test"]),
    }
    if not math.isclose(sum(ratios.values()), 1.0, abs_tol=1.0e-8):
        raise ValueError("train/validation/test ratios must sum to one")

    vectors, totals = _group_vectors(rows)
    labels = sorted(totals)
    targets = {
        split: {label: totals[label] * ratio for label in labels}
        for split, ratio in ratios.items()
    }
    rarity = {
        group: sum(vector[label] / max(totals[label], 1) for label in labels if vector[label])
        for group, vector in vectors.items()
    }
    seed = int(split_config["seed"])
    order = sorted(
        vectors,
        key=lambda group: (
            -rarity[group],
            -vectors[group]["__rows__"],
            stable_id(seed, group),
        ),
    )
    current = {split: Counter() for split in ratios}
    assignment: dict[str, str] = {}
    forced: dict[str, str] = {}
    forced_details: dict[str, list[tuple[str, str, str]]] = defaultdict(list)
    for row in rows:
        requested = row.get("source_partition", "").strip().lower()
        if row["quality_status"] != "accepted" or requested not in ratios:
            continue
        group = row["split_group"]
        forced_details[group].append(
            (row["source"], row["source_path"], requested)
        )
        if group in forced and forced[group] != requested:
            details = "; ".join(
                f"{source}:{partition}:{path}"
                for source, path, partition in forced_details[group][:8]
            )
            raise ValueError(
                f"split group {group} has conflicting fixed partitions: "
                f"{forced[group]} and {requested}; members: {details}"
            )
        forced[group] = requested
    for group, split in sorted(forced.items()):
        assignment[group] = split
        current[split].update(vectors[group])

    for group in order:
        if group in assignment:
            continue
        vector = vectors[group]
        ranked = sorted(
            ratios,
            key=lambda split: (
                _split_cost(split, vector, current, targets, labels),
                stable_id(seed, group, split),
            ),
        )
        chosen = ranked[0]
        assignment[group] = chosen
        current[chosen].update(vector)

    # If a label has at least three independent groups, try to put it in every split.
    label_groups: dict[str, set[str]] = defaultdict(set)
    for group, vector in vectors.items():
        for label in labels:
            if label != "__rows__" and vector[label]:
                label_groups[label].add(group)
    for label, groups in sorted(label_groups.items(), key=lambda item: len(item[1])):
        if len(groups) < 3:
            continue
        for missing in ratios:
            if any(assignment[group] == missing for group in groups):
                continue
            candidates: list[tuple[float, str]] = []
            for group in groups:
                if group in forced:
                    continue
                donor = assignment[group]
                if sum(assignment[item] == donor for item in groups) <= 1:
                    continue
                vector = vectors[group]
                move_cost = (
                    _split_cost(missing, vector, current, targets, labels)
                    - _split_cost(donor, vector, current, targets, labels)
                )
                candidates.append((move_cost, group))
            if candidates:
                _, group = min(candidates, key=lambda item: (item[0], stable_id(seed, item[1], missing)))
                donor = assignment[group]
                current[donor].subtract(vectors[group])
                current[missing].update(vectors[group])
                assignment[group] = missing

    # With very small classes, one group can contain most recordings. Keep the
    # largest available group in train while preserving per-split coverage.
    protected_groups: set[str] = set()
    for label, groups in sorted(label_groups.items(), key=lambda item: len(item[1])):
        train_groups = [
            group for group in groups
            if assignment[group] == "train"
            and group not in forced
            and group not in protected_groups
        ]
        other_groups = [
            group for group in groups
            if assignment[group] != "train"
            and group not in forced
            and group not in protected_groups
        ]
        if not train_groups or not other_groups:
            continue
        train_group = min(
            train_groups, key=lambda group: (vectors[group][label], stable_id(seed, group))
        )
        other_group = max(
            other_groups, key=lambda group: (vectors[group][label], stable_id(seed, group))
        )
        if vectors[other_group][label] <= vectors[train_group][label]:
            continue
        other_split = assignment[other_group]
        coverage_preserved = True
        for check_label, check_groups in label_groups.items():
            if len(check_groups) < 3:
                continue
            new_train = (
                current["train"][check_label]
                - vectors[train_group][check_label]
                + vectors[other_group][check_label]
            )
            new_other = (
                current[other_split][check_label]
                - vectors[other_group][check_label]
                + vectors[train_group][check_label]
            )
            if new_train <= 0 or new_other <= 0:
                coverage_preserved = False
                break
        if coverage_preserved:
            current["train"].subtract(vectors[train_group])
            current["train"].update(vectors[other_group])
            current[other_split].subtract(vectors[other_group])
            current[other_split].update(vectors[train_group])
            assignment[train_group] = other_split
            assignment[other_group] = "train"
            protected_groups.update((train_group, other_group))

    for row in rows:
        row["split"] = (
            assignment[row["split_group"]]
            if row["quality_status"] == "accepted"
            else "rejected"
        )


def prepare_recordings(
    rows: list[dict[str, str]],
    config: dict[str, Any],
    data_root: Path = DATA_ROOT,
    *,
    ffmpeg: str = "ffmpeg",
    force: bool = False,
    limit: int | None = None,
) -> list[dict[str, str]]:
    selected = rows[:limit] if limit else rows
    prepared: list[dict[str, str]] = []
    for index, row in enumerate(selected, start=1):
        prepared.append(_standardize_one(row, config, data_root, ffmpeg, force))
        if index % 100 == 0 or index == len(selected):
            print(f"standardized/audited {index}/{len(selected)} recordings", flush=True)
    _reject_duplicate_content(prepared)
    _attach_split_groups(prepared)
    _assign_splits(prepared, config)
    return prepared

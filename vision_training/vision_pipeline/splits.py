from __future__ import annotations

import hashlib
import re
from collections.abc import Iterable
from urllib.parse import parse_qs, urlparse


def canonical_group_id(value: str) -> str:
    """Normalize source identifiers so URL variants cannot cross data splits."""
    normalized = value.strip()
    if not normalized:
        return normalized
    candidate = normalized
    if "youtube.com/" in candidate.lower() or "youtu.be/" in candidate.lower():
        if "://" not in candidate:
            candidate = f"https://{candidate.lstrip('/')}"
        parsed = urlparse(candidate)
        host = parsed.netloc.lower().removeprefix("www.")
        video_id = ""
        if host.endswith("youtube.com"):
            video_id = parse_qs(parsed.query).get("v", [""])[0]
        elif host == "youtu.be":
            video_id = parsed.path.strip("/").split("/", 1)[0]
        video_id = re.sub(r"[^A-Za-z0-9_-]", "", video_id)
        if video_id:
            return f"youtube:{video_id}"
    return normalized


def stable_group_split(
    group_id: str,
    *,
    seed: int = 20260725,
    train_ratio: float = 0.70,
    validation_ratio: float = 0.15,
) -> str:
    """Assign a whole capture/source group to a deterministic split."""
    if not group_id.strip():
        raise ValueError("group_id must not be empty")
    if train_ratio <= 0 or validation_ratio <= 0 or train_ratio + validation_ratio >= 1:
        raise ValueError("split ratios must leave a non-empty test fraction")

    digest = hashlib.sha256(f"{seed}:{group_id}".encode("utf-8")).digest()
    fraction = int.from_bytes(digest[:8], "big") / float(1 << 64)
    if fraction < train_ratio:
        return "train"
    if fraction < train_ratio + validation_ratio:
        return "val"
    return "test"


def assert_group_isolation(rows: Iterable[dict[str, str]], group_key: str = "group_id") -> None:
    """Raise if the same group occurs in more than one data split."""
    seen: dict[str, str] = {}
    for row in rows:
        group = row.get(group_key, "").strip()
        split = row.get("split", "").strip()
        if not group or split not in {"train", "val", "test"}:
            raise ValueError(f"invalid group/split row: {row}")
        previous = seen.setdefault(group, split)
        if previous != split:
            raise ValueError(f"group leakage: {group!r} occurs in {previous!r} and {split!r}")

from __future__ import annotations

from pathlib import Path
from typing import Sequence

from PIL import Image


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


def clip_bbox(
    bbox: Sequence[float],
    width: int,
    height: int,
    *,
    min_size: float = 2.0,
) -> list[float] | None:
    if len(bbox) != 4 or width <= 0 or height <= 0:
        return None
    x, y, w, h = (float(value) for value in bbox)
    x1 = max(0.0, min(float(width), x))
    y1 = max(0.0, min(float(height), y))
    x2 = max(0.0, min(float(width), x + max(0.0, w)))
    y2 = max(0.0, min(float(height), y + max(0.0, h)))
    if x2 - x1 < min_size or y2 - y1 < min_size:
        return None
    return [x1, y1, x2 - x1, y2 - y1]


def expanded_bbox(
    bbox: Sequence[float],
    width: int,
    height: int,
    scale: float,
) -> list[float] | None:
    clipped = clip_bbox(bbox, width, height)
    if clipped is None or scale <= 0:
        return None
    x, y, w, h = clipped
    cx, cy = x + w / 2.0, y + h / 2.0
    return clip_bbox(
        [cx - w * scale / 2.0, cy - h * scale / 2.0, w * scale, h * scale],
        width,
        height,
    )


def difference_hash(image: Image.Image, hash_size: int = 8) -> int:
    gray = image.convert("L").resize((hash_size + 1, hash_size), Image.Resampling.BILINEAR)
    pixels = list(gray.getdata())
    result = 0
    for y in range(hash_size):
        offset = y * (hash_size + 1)
        for x in range(hash_size):
            result = (result << 1) | int(pixels[offset + x] > pixels[offset + x + 1])
    return result


def hamming_distance(left: int, right: int) -> int:
    return (left ^ right).bit_count()


def image_size(path: Path) -> tuple[int, int]:
    with Image.open(path) as image:
        return image.size

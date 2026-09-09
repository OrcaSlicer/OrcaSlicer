"""Conservative local quality checks for image-to-3D reference images.

The evaluator intentionally uses only Pillow.  It is suitable for offline
preflight and benchmark reporting; callers should treat warnings as guidance
and only use ``model_input_eligible`` as a gate for severe, measurable faults.
"""

from __future__ import annotations

from collections import deque
from io import BytesIO
from pathlib import Path
import re
from typing import Any, Iterable

from PIL import Image, ImageFilter


QUALITY_SCHEMA_VERSION = 1
ANALYSIS_EDGE = 192
STYLE_RECOMMENDATION_SCHEMA_VERSION = 1
RECOMMENDABLE_STYLES = (
    "portrait_sketch", "ink_relief", "cartoon", "sculpture", "low_poly", "relief", "realistic", "diorama",
)

_SUBJECT_KEYWORDS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("scene", ("场景", "群像", "多人", "多物体", "街景", "风景", "scene", "group", "landscape")),
    ("flat_graphic", ("logo", "标志", "图标", "文字", "字体", "徽章", "标牌", "海报", "icon", "badge", "sign")),
    ("effects", ("烟雾", "火焰", "液体", "水花", "透明", "玻璃", "smoke", "fire", "flame", "liquid", "glass")),
    ("portrait", ("人像", "人物", "头像", "肖像", "自拍", "男士", "女士", "男孩", "女孩", "portrait", "person", "people", "face", "selfie", "man", "woman", "boy", "girl")),
    ("animal", ("宠物", "动物", "猫", "狗", "兔", "鸟", "鱼", "马", "熊", "龙", "cat", "dog", "pet", "animal", "rabbit", "bird", "horse", "bear", "dragon")),
    ("architecture", ("建筑", "房屋", "大楼", "塔", "桥", "寺庙", "城堡", "亭", "architecture", "building", "house", "tower", "bridge", "temple", "castle")),
    ("hard_surface", ("汽车", "车辆", "机器人", "机甲", "机器", "产品", "家具", "相机", "手机", "工具", "vehicle", "car", "robot", "machine", "product", "furniture", "camera", "phone", "tool")),
    ("organic", ("花", "植物", "树", "盆景", "食物", "蛋糕", "水果", "蔬菜", "plant", "flower", "tree", "bonsai", "food", "cake", "fruit", "vegetable")),
)


class ModelInputImageQualityError(RuntimeError):
    pass


def _median(values: Iterable[int]) -> int:
    ordered = sorted(values)
    if not ordered:
        return 0
    return ordered[len(ordered) // 2]


def _corner_colors(pixels: list[tuple[int, int, int, int]], width: int, height: int) -> list[tuple[int, int, int]]:
    block = min(width, height, max(2, min(width, height) // 16))
    colors: list[tuple[int, int, int]] = []
    for left, top in ((0, 0), (width - block, 0), (0, height - block), (width - block, height - block)):
        sample = [
            pixels[y * width + x]
            for y in range(top, top + block)
            for x in range(left, left + block)
            if pixels[y * width + x][3] >= 240
        ]
        if sample:
            colors.append(tuple(_median(pixel[channel] for pixel in sample) for channel in range(3)))
    return colors or [(255, 255, 255)]


def _color_distance_squared(pixel: tuple[int, int, int, int], color: tuple[int, int, int]) -> int:
    return sum((pixel[channel] - color[channel]) ** 2 for channel in range(3))


def _component_areas(mask: list[bool], width: int, height: int) -> list[int]:
    visited = bytearray(width * height)
    areas: list[int] = []
    for start, foreground in enumerate(mask):
        if not foreground or visited[start]:
            continue
        visited[start] = 1
        queue = deque([start])
        area = 0
        while queue:
            offset = queue.popleft()
            area += 1
            x = offset % width
            y = offset // width
            if x and mask[offset - 1] and not visited[offset - 1]:
                visited[offset - 1] = 1
                queue.append(offset - 1)
            if x + 1 < width and mask[offset + 1] and not visited[offset + 1]:
                visited[offset + 1] = 1
                queue.append(offset + 1)
            if y and mask[offset - width] and not visited[offset - width]:
                visited[offset - width] = 1
                queue.append(offset - width)
            if y + 1 < height and mask[offset + width] and not visited[offset + width]:
                visited[offset + width] = 1
                queue.append(offset + width)
        areas.append(area)
    return sorted(areas, reverse=True)


def _component_shapes(
    mask: list[bool], width: int, height: int
) -> list[dict[str, Any]]:
    visited = bytearray(width * height)
    shapes: list[dict[str, Any]] = []
    for start, foreground in enumerate(mask):
        if not foreground or visited[start]:
            continue
        visited[start] = 1
        queue = deque([start])
        area = 0
        left = right = start % width
        top = bottom = start // width
        while queue:
            offset = queue.popleft()
            area += 1
            x = offset % width
            y = offset // width
            left, right = min(left, x), max(right, x)
            top, bottom = min(top, y), max(bottom, y)
            if x and mask[offset - 1] and not visited[offset - 1]:
                visited[offset - 1] = 1
                queue.append(offset - 1)
            if x + 1 < width and mask[offset + 1] and not visited[offset + 1]:
                visited[offset + 1] = 1
                queue.append(offset + 1)
            if y and mask[offset - width] and not visited[offset - width]:
                visited[offset - width] = 1
                queue.append(offset - width)
            if y + 1 < height and mask[offset + width] and not visited[offset + width]:
                visited[offset + width] = 1
                queue.append(offset + width)
        box_area = (right - left + 1) * (bottom - top + 1)
        shapes.append({
            "area": area,
            "bbox": [left, top, right, bottom],
            "rectangularity": area / max(1, box_area),
        })
    return sorted(shapes, key=lambda item: item["area"], reverse=True)


def assess_model_input_image(
    path: str | Path | bytes | bytearray, *, reject_rectangular_cutouts: bool = False
) -> dict[str, Any]:
    """Return stable metrics, warnings and severe blockers for a reference image."""

    try:
        source = BytesIO(path) if isinstance(path, (bytes, bytearray)) else path
        with Image.open(source) as opened:
            opened.load()
            original_width, original_height = opened.size
            image = opened.convert("RGBA")
    except (OSError, ValueError) as exc:
        raise ModelInputImageQualityError("The model reference image cannot be decoded.") from exc

    if original_width < 1 or original_height < 1:
        raise ModelInputImageQualityError("The model reference image has no pixels.")
    image.thumbnail((ANALYSIS_EDGE, ANALYSIS_EDGE), Image.Resampling.LANCZOS)
    width, height = image.size
    raw = image.tobytes()
    pixels = list(zip(raw[0::4], raw[1::4], raw[2::4], raw[3::4]))
    total = len(pixels)

    transparent_pixel_ratio = sum(pixel[3] < 240 for pixel in pixels) / total
    uses_transparency = transparent_pixel_ratio >= 0.01
    corner_colors = _corner_colors(pixels, width, height)
    background_threshold_squared = 42 ** 2
    if uses_transparency:
        mask = [pixel[3] >= 32 for pixel in pixels]
    else:
        mask = [
            min(_color_distance_squared(pixel, color) for color in corner_colors) > background_threshold_squared
            for pixel in pixels
        ]

    foreground_offsets = [offset for offset, foreground in enumerate(mask) if foreground]
    foreground_count = len(foreground_offsets)
    foreground_ratio = foreground_count / total
    soft_alpha_count = sum(16 < pixel[3] < 240 for pixel in pixels)
    soft_alpha_ratio = soft_alpha_count / max(1, foreground_count)

    border_offsets = set(range(width))
    border_offsets.update(range((height - 1) * width, height * width))
    border_offsets.update(y * width for y in range(height))
    border_offsets.update(y * width + width - 1 for y in range(height))
    border_foreground_ratio = sum(mask[offset] for offset in border_offsets) / max(1, len(border_offsets))
    border_background_ratio = sum(
        min(_color_distance_squared(pixels[offset], color) for color in corner_colors) <= background_threshold_squared
        for offset in border_offsets
    ) / max(1, len(border_offsets))

    if foreground_offsets:
        xs = [offset % width for offset in foreground_offsets]
        ys = [offset // width for offset in foreground_offsets]
        left, right = min(xs), max(xs)
        top, bottom = min(ys), max(ys)
        bbox_width = right - left + 1
        bbox_height = bottom - top + 1
        bbox_area_ratio = bbox_width * bbox_height / total
        margins = {
            "left": left / width,
            "right": (width - 1 - right) / width,
            "top": top / height,
            "bottom": (height - 1 - bottom) / height,
        }
    else:
        bbox_area_ratio = 0.0
        margins = {"left": 0.0, "right": 0.0, "top": 0.0, "bottom": 0.0}

    areas = _component_areas(mask, width, height)
    meaningful_threshold = max(4, int(foreground_count * 0.005))
    meaningful_areas = [area for area in areas if area >= meaningful_threshold]
    largest_component_ratio = areas[0] / max(1, foreground_count) if areas else 0.0

    # A coherent alpha silhouette can still contain a large square bite caused
    # by a failed generative edit. It remains part of the same connected
    # component, so ordinary fragmentation checks miss it. A conservative 5px
    # morphological close exposes only abrupt, compact notches; natural smooth
    # portrait contours produce at most a few filled pixels at this scale.
    rectangular_cutouts: list[dict[str, Any]] = []
    if uses_transparency and foreground_count:
        binary = Image.new("L", (width, height), 0)
        binary.putdata([255 if foreground else 0 for foreground in mask])
        closed = binary.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5))
        closed_pixels = list(
            closed.get_flattened_data() if hasattr(closed, "get_flattened_data") else closed.getdata()
        )
        filled = [closed_pixels[index] >= 128 and not mask[index] for index in range(total)]
        threshold = max(20, int(foreground_count * 0.003))
        rectangular_cutouts = [
            shape for shape in _component_shapes(filled, width, height)
            if shape["area"] >= threshold and shape["rectangularity"] >= 0.70
        ]

    warnings: list[str] = []
    if min(original_width, original_height) < 256:
        warnings.append("low_resolution")
    blockers: list[str] = []
    if foreground_ratio < 0.03:
        blockers.append("subject_not_detected")
    elif foreground_ratio < 0.08:
        blockers.append("subject_too_small")
    elif foreground_ratio < 0.16:
        warnings.append("subject_small")
    if foreground_ratio > 0.94:
        blockers.append("subject_or_background_fills_frame")
    elif foreground_ratio > 0.88:
        warnings.append("subject_nearly_fills_frame")
    if border_foreground_ratio > 0.18:
        blockers.append("subject_cropped")
    elif border_foreground_ratio > 0.04:
        warnings.append("subject_close_to_frame")
    # Alpha describes the visible silhouette. RGB distance from corner colors
    # only estimates it: light clothing can match the backdrop, while painted
    # transparency tiles can look like separate foreground pieces. Preserve
    # those measurements as an uncertainty warning, not proof of disconnection.
    fragmented_mask = largest_component_ratio < 0.60 and len(meaningful_areas) > 1
    if fragmented_mask and uses_transparency:
        blockers.append("fragmented_subject")
    elif fragmented_mask:
        warnings.append("foreground_segmentation_uncertain")
    elif largest_component_ratio < 0.88 or len(meaningful_areas) > 4:
        warnings.append("multiple_or_fragmented_components")
    if uses_transparency and soft_alpha_ratio > 0.30:
        blockers.append("excessive_semitransparency")
    elif uses_transparency and soft_alpha_ratio > 0.08:
        warnings.append("soft_transparent_edges")
    if rectangular_cutouts:
        (blockers if reject_rectangular_cutouts else warnings).append(
            "subject_has_rectangular_cutout"
        )
    if not uses_transparency and border_background_ratio < 0.45:
        blockers.append("background_not_isolated")
    elif not uses_transparency and border_background_ratio < 0.75:
        warnings.append("background_may_be_complex")

    score = 100.0
    penalty_by_flag = {
        "low_resolution": 0,
        "subject_not_detected": 70,
        "subject_too_small": 45,
        "subject_small": 15,
        "subject_or_background_fills_frame": 40,
        "subject_nearly_fills_frame": 15,
        "subject_cropped": 35,
        "subject_close_to_frame": 12,
        "fragmented_subject": 35,
        "foreground_segmentation_uncertain": 12,
        "multiple_or_fragmented_components": 12,
        "excessive_semitransparency": 30,
        "soft_transparent_edges": 8,
        "subject_has_rectangular_cutout": 45,
        "background_not_isolated": 30,
        "background_may_be_complex": 10,
    }
    for flag in blockers + warnings:
        score -= penalty_by_flag[flag]

    return {
        "schema_version": QUALITY_SCHEMA_VERSION,
        "score": round(max(0.0, score), 1),
        "model_input_eligible": not blockers,
        "blockers": blockers,
        "warnings": warnings,
        "metrics": {
            "analysis_width": width,
            "analysis_height": height,
            "foreground_mask_source": "alpha" if uses_transparency else "color_distance",
            "foreground_ratio": round(foreground_ratio, 6),
            "foreground_bbox_area_ratio": round(bbox_area_ratio, 6),
            "border_foreground_ratio": round(border_foreground_ratio, 6),
            "border_background_ratio": round(border_background_ratio, 6),
            "largest_component_ratio": round(largest_component_ratio, 6),
            "meaningful_component_count": len(meaningful_areas),
            "transparent_pixel_ratio": round(transparent_pixel_ratio, 6),
            "soft_alpha_ratio": round(soft_alpha_ratio, 6),
            "rectangular_cutout_count": len(rectangular_cutouts),
            "largest_rectangular_cutout": (
                {
                    "area": rectangular_cutouts[0]["area"],
                    "bbox": rectangular_cutouts[0]["bbox"],
                    "rectangularity": round(rectangular_cutouts[0]["rectangularity"], 4),
                }
                if rectangular_cutouts else {}
            ),
            "margins": {name: round(value, 6) for name, value in margins.items()},
        },
    }


def _keyword_category(text: str) -> str:
    normalized = text.casefold()
    latin_tokens = set(re.findall(r"[a-z0-9]+", normalized))
    for category, keywords in _SUBJECT_KEYWORDS:
        for keyword in keywords:
            if keyword.isascii():
                if keyword in latin_tokens:
                    return category
            elif keyword in normalized:
                return category
    return ""


def _style_image_metrics(image_data: bytes | bytearray) -> dict[str, float | int]:
    try:
        with Image.open(BytesIO(image_data)) as opened:
            opened.load()
            image = opened.convert("RGBA")
    except (OSError, ValueError) as exc:
        raise ModelInputImageQualityError("The style reference image cannot be decoded.") from exc

    image.thumbnail((160, 160), Image.Resampling.LANCZOS)
    rgb = image.convert("RGB")
    quantized = rgb.quantize(colors=16)
    histogram = quantized.histogram()
    total = max(1, image.width * image.height)
    meaningful_colors = sum(count >= max(2, int(total * 0.01)) for count in histogram)

    edges = rgb.convert("L").filter(ImageFilter.FIND_EDGES)
    if edges.width > 4 and edges.height > 4:
        edges = edges.crop((2, 2, edges.width - 2, edges.height - 2))
    edge_values = list(
        edges.get_flattened_data() if hasattr(edges, "get_flattened_data") else edges.getdata()
    )
    edge_density = sum(value >= 48 for value in edge_values) / max(1, len(edge_values))

    rgba_pixels = list(
        image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()
    )
    ycbcr = rgb.convert("YCbCr")
    ycbcr_pixels = list(
        ycbcr.get_flattened_data() if hasattr(ycbcr, "get_flattened_data") else ycbcr.getdata()
    )
    skin_mask: list[bool] = []
    for rgba, (_, cb, cr) in zip(rgba_pixels, ycbcr_pixels):
        red, green, blue, alpha = rgba
        skin_mask.append(
            alpha >= 32
            and red >= 70
            and green >= 40
            and blue >= 20
            and red > green
            and red > blue
            and max(red, green, blue) - min(red, green, blue) >= 15
            and 70 <= cb <= 135
            and 130 <= cr <= 180
        )
    skin_count = sum(skin_mask)
    skin_ratio = skin_count / total
    portrait_likelihood = 0.0
    if 0.008 <= skin_ratio <= 0.38:
        for component in _component_shapes(skin_mask, image.width, image.height):
            left, top, right, bottom = component["bbox"]
            box_width_ratio = (right - left + 1) / image.width
            box_height_ratio = (bottom - top + 1) / image.height
            center_x = (left + right + 1) / (2 * image.width)
            center_y = (top + bottom + 1) / (2 * image.height)
            aspect_ratio = box_width_ratio / max(0.001, box_height_ratio)
            if (
                component["area"] >= max(10, int(total * 0.003))
                and 0.07 <= box_width_ratio <= 0.48
                and 0.06 <= box_height_ratio <= 0.45
                and 0.45 <= aspect_ratio <= 1.9
                and 0.22 <= center_x <= 0.78
                and 0.06 <= center_y <= 0.55
                and component["rectangularity"] >= 0.20
            ):
                portrait_likelihood = 1.0
                break
    return {
        "meaningful_color_count": meaningful_colors,
        "edge_density": round(edge_density, 6),
        "portrait_likelihood": portrait_likelihood,
    }


def recommend_printable_style(
    image_data: bytes | bytearray,
    *,
    prompt: str = "",
    filename: str = "",
) -> dict[str, Any]:
    """Recommend printable output styles locally without calling a remote model."""

    if not isinstance(image_data, (bytes, bytearray)) or not image_data:
        raise ModelInputImageQualityError("A reference image is required for style recommendation.")
    quality = assess_model_input_image(image_data)
    image_metrics = _style_image_metrics(image_data)
    category = _keyword_category(f"{prompt} {filename}")
    confidence = "high" if category else "medium"
    if not category and image_metrics["portrait_likelihood"] >= 1.0:
        category = "portrait"

    mapping: dict[str, tuple[str, tuple[str, str], str]] = {
        "portrait": ("portrait_sketch", ("realistic", "cartoon"), "portrait"),
        "animal": ("cartoon", ("sculpture", "low_poly"), "animal"),
        "flat_graphic": ("ink_relief", ("relief", "low_poly"), "flat_graphic"),
        "effects": ("ink_relief", ("relief", "sculpture"), "effects"),
        "architecture": ("realistic", ("low_poly", "relief"), "architecture"),
        "hard_surface": ("realistic", ("low_poly", "sculpture"), "hard_surface"),
        "organic": ("cartoon", ("relief", "sculpture"), "organic"),
        "scene": ("diorama", ("low_poly", "relief"), "scene"),
    }
    if category:
        primary, alternatives, reason = mapping[category]
    else:
        metrics = quality["metrics"]
        severe = {"subject_not_detected", "subject_too_small", "subject_cropped"}
        if metrics["meaningful_component_count"] >= 2 and metrics["largest_component_ratio"] < 0.72:
            primary, alternatives, reason = "diorama", ("low_poly", "relief"), "multiple_subjects"
        elif severe.intersection(quality["blockers"]):
            primary, alternatives, reason = "low_poly", ("relief", "sculpture"), "limited_reference"
        elif metrics["meaningful_component_count"] >= 4:
            primary, alternatives, reason = "diorama", ("low_poly", "relief"), "multiple_subjects"
        elif image_metrics["meaningful_color_count"] <= 6 and image_metrics["edge_density"] <= 0.16:
            primary, alternatives, reason = "ink_relief", ("relief", "low_poly"), "flat_graphic"
        elif image_metrics["edge_density"] >= 0.27:
            primary, alternatives, reason = "realistic", ("low_poly", "sculpture"), "structured_subject"
        else:
            primary, alternatives, reason = "cartoon", ("sculpture", "low_poly"), "generic_safe"
            confidence = "low"

    ordered = (primary, *alternatives)
    if len(set(ordered)) != 3 or any(style not in RECOMMENDABLE_STYLES for style in ordered):
        raise ModelInputImageQualityError("The style recommendation is internally inconsistent.")
    return {
        "schema_version": STYLE_RECOMMENDATION_SCHEMA_VERSION,
        "primary": primary,
        "alternatives": list(alternatives),
        "reason": reason,
        "subject": category or "unknown",
        "confidence": confidence,
        "local_only": True,
    }

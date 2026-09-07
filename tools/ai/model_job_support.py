from __future__ import annotations

from pathlib import Path
from typing import Any

from model_input_image_quality import ModelInputImageQualityError, assess_model_input_image


def image_type(data: bytes) -> str | None:
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if data.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    return None


def file_info(path: Path | None) -> tuple[bool, int]:
    if path is None:
        return False, 0
    try:
        size = path.stat().st_size
    except OSError:
        return False, 0
    return size > 0, size


def stored_image_type(path: Path | None) -> str:
    if path is None:
        return ""
    try:
        with path.open("rb") as stream:
            return image_type(stream.read(16)) or ""
    except OSError:
        return ""


def preprocess_failure_payload(error: Exception) -> dict[str, Any]:
    return {
        "code": str(getattr(error, "code", "image_preprocess_failed")),
        "retryable": getattr(error, "retryable", False) is True,
        "ambiguous": getattr(error, "ambiguous", False) is True,
    }


def generation_prompt(
    prompt: str,
    palette: tuple[str, ...],
    *,
    max_prompt_bytes: int,
    constrain_palette: bool = True,
) -> str:
    suffix = (
        " Generate a watertight printable model with a stable flat base. Preserve meaningful separate parts "
        "and material regions in their original relative positions; do not create unintended floating debris, "
        "internal shells, holes, or non-manifold geometry."
    )
    if palette and constrain_palette:
        suffix += " Use only these printable filament colors: " + ", ".join(palette) + "."
    else:
        suffix += " Preserve coherent natural material relationships with broad, clean regions."
    max_prefix_bytes = max_prompt_bytes - len(suffix.encode("utf-8"))
    prefix = prompt.strip().encode("utf-8")[: max(0, max_prefix_bytes)].decode("utf-8", errors="ignore").rstrip()
    return prefix + suffix


def assess_job_model_reference(job: Any) -> dict[str, Any]:
    reference = (job.raw_preview_path if job.image_metrics.get("design_reference") == "ai-design-v1"
                 else job.model_reference_path or job.preview_path)
    if reference is None:
        raise ModelInputImageQualityError("The model reference image is unavailable.")
    quality = assess_model_input_image(reference)
    job.image_metrics["model_input_quality"] = quality
    return quality


def model_input_quality_message(quality: dict[str, Any]) -> str:
    blockers = quality.get("blockers", [])
    primary = str(blockers[0]) if blockers else ""
    return {
        "subject_not_detected": "No clear subject was found; use a clearer image or regenerate the preview.",
        "subject_too_small": "The subject is too small; enlarge it and regenerate the preview.",
        "subject_or_background_fills_frame": "The subject or background fills the frame; regenerate with clear margins.",
        "subject_cropped": "The subject touches the frame and may be cropped; regenerate with the complete silhouette visible.",
        "fragmented_subject": "The reference contains disconnected subjects or fragments; regenerate with one connected subject.",
        "excessive_semitransparency": "The subject contains too much transparency for reliable 3D generation.",
        "background_not_isolated": "The background is too complex; regenerate on a transparent or plain background.",
        "subject_has_rectangular_cutout": "The portrait contains a large square cutout or missing body region; regenerate the preview before paying for 3D generation.",
        "portrait_shoulder_silhouette_unverified": "The portrait shoulder silhouette still contains an unverified gap or background remnant; regenerate the preview before paying for 3D generation.",
        "preview_identity_mismatch": "The prepared face differs too much from the original; regenerate the portrait preview.",
        "preview_face_geometry_drift": "The prepared face shape or landmarks drifted; regenerate the portrait preview.",
        "preview_age_expression_drift": "The prepared age or expression changed; regenerate the portrait preview.",
        "preview_material_mixing": "Skin, clothing, hair, or base colors are mixed; regenerate the portrait preview.",
        "preview_base_mixing": "The pedestal contains another material color; regenerate the portrait preview.",
        "preview_pose_clothing_drift": "The prepared pose or clothing differs from the original; regenerate the portrait preview.",
        "preview_modeling_reference_unclear": "The portrait is not a reliable 3D reference; regenerate the preview.",
    }.get(primary, "The preview does not meet the image-to-3D input requirements; regenerate it.")


def printable_preview_message(job: Any, fallback: str) -> str:
    model_input_quality = job.image_metrics.get("model_input_quality", {})
    if isinstance(model_input_quality, dict) and not bool(model_input_quality.get("model_input_eligible", True)):
        return model_input_quality_message(model_input_quality)
    generation_input_quality = job.image_metrics.get("generation_input_quality", {})
    if isinstance(generation_input_quality, dict) and not bool(
        generation_input_quality.get("model_input_eligible", True)
    ):
        return model_input_quality_message(generation_input_quality)
    if (job.image_metrics.get("design_reference") != "ai-design-v1"
            and job.palette and not bool(job.image_metrics.get("palette_quality_ok", True))):
        subject_ratio = float(job.image_metrics.get("printable_subject_area_ratio", 0.0))
        continuity = float(job.image_metrics.get("largest_subject_component_ratio", 0.0))
        detached_span = float(job.image_metrics.get("largest_detached_subject_diagonal_ratio", 0.0))
        if subject_ratio < 0.18:
            return "The printable subject is too small in the preview; regenerate with a larger subject."
        if continuity < 0.90:
            return "The printable subject is disconnected; regenerate with one connected subject."
        if detached_span >= 0.08:
            return "A long thin structure is detached from the subject; reconnect handles, branches, or supports and regenerate."
        if not bool(job.image_metrics.get("material_fragmentation_ok", True)):
            return "Skin or garment colors are fragmented into incorrect patches; regenerate the portrait preview before 3D."
        return "The printable preview failed its geometry quality check; regenerate the preview."
    return fallback


def apply_legacy_material_fragmentation_gate(job: Any) -> None:
    """Conservatively block severe pre-gate portrait previews after an upgrade."""

    metrics = job.image_metrics
    if "material_fragmentation_ok" in metrics:
        return
    metrics["material_fragmentation_ok"] = True
    if job.style != "realistic" or len(job.palette) < 3:
        return
    palette_rgb = [tuple(int(color[index:index + 2], 16) for index in (1, 3, 5)) for color in job.palette]
    has_bright_neutral = any(
        max(color) - min(color) <= 32 and sum(color) / 3 >= 180 for color in palette_rgb
    )
    has_warm_skin = any(
        red > green >= blue
        and 12 <= red - green <= 82
        and 28 <= red - blue <= 118
        and green >= 70
        and blue >= 45
        for red, green, blue in palette_rgb
    )
    counts = metrics.get("subject_color_component_count", {})
    ratios = metrics.get("secondary_subject_color_component_ratio", {})
    if not has_bright_neutral or not has_warm_skin or not isinstance(counts, dict) or not isinstance(ratios, dict):
        return
    severe_colors = [
        color for color in job.palette
        if int(counts.get(color, 0)) >= 12 and float(ratios.get(color, 0.0)) >= 0.025
    ]
    if not severe_colors:
        return
    metrics["material_fragmentation_ok"] = False
    metrics["palette_quality_ok"] = False
    metrics["severe_fragmented_palette_colors"] = severe_colors
    warnings = metrics.get("quality_warnings", [])
    if not isinstance(warnings, list):
        warnings = []
    if "portrait_material_fragmentation_blocks_3d" not in warnings:
        warnings.append("portrait_material_fragmentation_blocks_3d")
    metrics["quality_warnings"] = warnings

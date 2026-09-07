"""Conservative vision gate for an image-to-3D portrait reference."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import time
from typing import Any, Callable

try:
    from .openai_preprocessor import OpenAIPreprocessorError, _portrait_face_lock_mask, complete_vision
except ImportError:
    from openai_preprocessor import OpenAIPreprocessorError, _portrait_face_lock_mask, complete_vision


REPORT_SCHEMA_VERSION = 1
REVIEW_VERSION = "reference-visual-v2"
REPORT_FILENAME = "reference-visual-quality.json"
CHECKS = {
    "identity_likeness": (85, "preview_identity_mismatch"),
    "face_geometry": (85, "preview_face_geometry_drift"),
    "age_expression": (82, "preview_age_expression_drift"),
    "pose_clothing": (85, "preview_pose_clothing_drift"),
    # Exact four-material previews intentionally flatten antialiasing and tiny
    # details. Local connected-component gates remain authoritative for
    # speckles; this visual floor catches only obvious semantic cross-material
    # patches.
    "material_ownership": (70, "preview_material_mixing"),
    "base_integrity": (88, "preview_base_mixing"),
    "modeling_reference": (75, "preview_modeling_reference_unclear"),
}
BLOCKING_CHECKS = frozenset(CHECKS) - {"modeling_reference"}


class ReferenceVisualQualityError(ValueError):
    pass


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _system_prompt() -> str:
    checks = ",".join(
        f'"{check}":{{"status":"pass|review","score":0,"reason":"Chinese sentence"}}'
        for check in CHECKS
    )
    return (
        "You are a conservative preflight reviewer for a realistic image-to-3D portrait collectible. "
        "Image 1 is the original person photo, resized only for review. Image 2 is a three-column review sheet: the original is on the "
        "left, the natural prepared model reference is in the center, and the exact four-material printable preview is on the "
        "right. The left and center columns use the same scale to make facial comparison reliable. Compare the same person strictly; "
        "attractiveness is not identity. Preserve adult age, "
        "face width and length, jaw and chin, eye spacing and openings, nose width, mouth corners, tooth exposure, asymmetry, "
        "head angle, pose, crossed-arm order, clothing and accessories. A beautified, younger, narrower, more symmetric or generic "
        "professional face requires review. However, do not invent a difference: when the left and center facial pixels and landmarks "
        "are visibly identical apart from resampling, transparency, background removal, body crop, or the added base, score identity, "
        "face geometry and age/expression from 98 to 100. Never claim younger, narrower or more symmetric unless the reason names a "
        "concrete visible landmark change between the left and center faces. Skin may appear only on face, ears, neck and source-visible hands or wrists. White jacket, "
        "secondary garment, hair and pedestal must each retain one semantic material without speckles or reflected-material stripes. "
        "The pedestal must be one stable structure material and remain visibly connected. Judge image suitability only, not hidden "
        "topology. IMPORTANT SCORING SPLIT: judge identity_likeness, face_geometry, age_expression and modeling_reference only from "
        "the original LEFT column versus the natural reference CENTER column of image 2, using image 1 only for extra facial detail. "
        "The exact four-material preview on the RIGHT intentionally "
        "removes facial shading and fine texture, so never lower identity or modeling scores merely because that right-hand view is flat. "
        "Use the right-hand view only for material_ownership and base_integrity. There, ignore expected simplification of a watch, teeth "
        "band, wrinkles and antialiasing; lower the score only for semantic cross-material patches, speckles, broken ownership or a mixed "
        "pedestal. Return exactly one JSON object without markdown using this schema: "
        '{"summary":"Chinese sentence","score":0,"confidence":0.0,"checks":{'
        + checks
        + "}}. Every score is an integer from 0 to 100 and every check is required. Never output reject."
    )


def _json_object(text: str) -> dict[str, Any]:
    stripped = text.strip()
    if stripped.startswith("```"):
        lines = stripped.splitlines()
        if len(lines) >= 3 and lines[-1].strip() == "```":
            stripped = "\n".join(lines[1:-1]).strip()
    try:
        value = json.loads(stripped)
    except json.JSONDecodeError:
        start, end = stripped.find("{"), stripped.rfind("}")
        if start < 0 or end <= start:
            raise ReferenceVisualQualityError("The reference visual review did not contain JSON.") from None
        try:
            value = json.loads(stripped[start : end + 1])
        except json.JSONDecodeError:
            raise ReferenceVisualQualityError("The reference visual review contained invalid JSON.") from None
    if not isinstance(value, dict):
        raise ReferenceVisualQualityError("The reference visual review must be an object.")
    return value


def _number(value: Any, minimum: float, maximum: float, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ReferenceVisualQualityError(f"The reference visual review {field} is invalid.")
    return max(minimum, min(maximum, float(value)))


def _text(value: Any, field: str, maximum: int) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ReferenceVisualQualityError(f"The reference visual review {field} is required.")
    return value.strip()[:maximum]


def _normalize(
    raw: dict[str, Any], original_sha: str, natural_sha: str, printable_sha: str, model: str
) -> dict[str, Any]:
    raw_checks = raw.get("checks")
    if not isinstance(raw_checks, dict):
        raise ReferenceVisualQualityError("The reference visual review checks are required.")
    checks: dict[str, Any] = {}
    warnings: list[str] = []
    blocking: list[str] = []
    for check_id, (floor, warning) in CHECKS.items():
        candidate = raw_checks.get(check_id)
        if not isinstance(candidate, dict):
            raise ReferenceVisualQualityError(f"The reference visual review check is missing: {check_id}")
        provider_status = candidate.get("status")
        if provider_status not in {"pass", "review"}:
            raise ReferenceVisualQualityError(f"The reference visual review status is invalid: {check_id}")
        score = round(_number(candidate.get("score"), 0, 100, f"{check_id} score"))
        # Provider labels are advisory and can be internally inconsistent (for
        # example, "review" while explicitly stating that no cross-material
        # patch exists).  Fixed score floors make the gate deterministic.
        status = "review" if score < floor else "pass"
        reason = _text(candidate.get("reason"), f"{check_id} reason", 240)
        checks[check_id] = {"status": status, "score": score, "reason": reason, "floor": floor}
        if status == "review":
            warnings.append(warning)
            if check_id in BLOCKING_CHECKS:
                blocking.append(warning)
    total_score = round(_number(raw.get("score"), 0, 100, "score"))
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "review_version": REVIEW_VERSION,
        "status": "review" if warnings else "pass",
        "score": total_score,
        "confidence": round(_number(raw.get("confidence"), 0, 1, "confidence"), 3),
        "summary": _text(raw.get("summary"), "summary", 500),
        "warnings": warnings,
        "blocking_warnings": blocking,
        "model_generation_recommended": not blocking,
        "errors": [],
        "checks": checks,
        "original_sha256": original_sha,
        "natural_reference_sha256": natural_sha,
        "printable_preview_sha256": printable_sha,
        "provider": "openai-compatible",
        "model": model,
        "generated_at": time.time(),
        "cached": False,
    }


def _unavailable(message: str, original_sha: str = "", natural_sha: str = "", printable_sha: str = "") -> dict[str, Any]:
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "review_version": REVIEW_VERSION,
        "status": "unavailable",
        "score": 0,
        "confidence": 0.0,
        "summary": "图片级人脸与材质复核暂不可用；保留本地 3D 输入门禁结果。",
        "warnings": [],
        "blocking_warnings": [],
        "model_generation_recommended": True,
        "errors": ["reference_visual_review_unavailable"],
        "checks": {},
        "original_sha256": original_sha,
        "natural_reference_sha256": natural_sha,
        "printable_preview_sha256": printable_sha,
        "provider": "openai-compatible",
        "model": os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4"),
        "generated_at": time.time(),
        "cached": False,
        "diagnostic": message[:300],
    }


def _write(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)


def _contact_sheet(original: Path, natural: Path, printable: Path, destination: Path) -> Path:
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        raise ReferenceVisualQualityError("Pillow is required for the reference review sheet.") from None
    column_width, header_height = 1024, 30
    target_width, target_height = column_width - 2, 1536 - header_height - 2
    canvas = Image.new("RGB", (column_width * 3, 1536), (225, 228, 232))
    draw = ImageDraw.Draw(canvas)
    labels = ("ORIGINAL", "NATURAL 3D REFERENCE", "PRINTABLE MATERIAL PREVIEW")
    for column, path in enumerate((original, natural, printable)):
        with Image.open(path) as opened:
            rgba = opened.convert("RGBA")
            background = Image.new("RGBA", rgba.size, (225, 228, 232, 255))
            background.alpha_composite(rgba)
            image = background.convert("RGB")
            image.thumbnail((target_width, target_height), Image.Resampling.LANCZOS)
            left = column * column_width + (column_width - image.width) // 2
            top = header_height + (1536 - header_height - image.height) // 2
            canvas.paste(image, (left, top))
        draw.rectangle(
            (column * column_width, 0, (column + 1) * column_width - 1, header_height - 1),
            fill=(248, 249, 250),
        )
        draw.text((column * column_width + 12, 8), labels[column], fill=(25, 28, 32))
        if column:
            draw.line((column * column_width, 0, column * column_width, 1535), fill=(110, 116, 124), width=2)
    temporary = destination.with_name(destination.name + ".part")
    destination.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(temporary, format="PNG")
    os.replace(temporary, destination)
    return destination


def _face_pixel_fidelity(original: Path, natural: Path, output: Path) -> dict[str, Any]:
    """Measure whether the prepared reference retains the source face pixels."""

    try:
        from PIL import Image

        mask_path = output / "source-face-fidelity-mask.png"
        if _portrait_face_lock_mask(original, mask_path) is None:
            return {"available": False, "source_locked": False, "reason": "face_not_detected"}
        with Image.open(original) as source_opened, Image.open(natural) as natural_opened, Image.open(mask_path) as mask_opened:
            reference = natural_opened.convert("RGB")
            source = source_opened.convert("RGB").resize(reference.size, Image.Resampling.LANCZOS)
            mask = mask_opened.getchannel("A").resize(reference.size, Image.Resampling.LANCZOS)
            source_pixels = source.get_flattened_data() if hasattr(source, "get_flattened_data") else source.getdata()
            reference_pixels = (
                reference.get_flattened_data() if hasattr(reference, "get_flattened_data") else reference.getdata()
            )
            mask_pixels = mask.get_flattened_data() if hasattr(mask, "get_flattened_data") else mask.getdata()
            sample_count = exact_count = total_error = 0
            for source_pixel, reference_pixel, alpha in zip(source_pixels, reference_pixels, mask_pixels):
                if alpha < 128:
                    continue
                differences = tuple(abs(int(source_pixel[index]) - int(reference_pixel[index])) for index in range(3))
                sample_count += 1
                total_error += sum(differences)
                if max(differences) <= 3:
                    exact_count += 1
        if sample_count < 500:
            return {"available": False, "source_locked": False, "reason": "insufficient_face_pixels"}
        mean_absolute_error = total_error / (sample_count * 3)
        exact_pixel_ratio = exact_count / sample_count
        return {
            "available": True,
            "source_locked": mean_absolute_error <= 3.0 and exact_pixel_ratio >= 0.80,
            "mean_absolute_error": round(mean_absolute_error, 4),
            "exact_pixel_ratio": round(exact_pixel_ratio, 6),
            "sample_count": sample_count,
        }
    except (OSError, ValueError):
        return {"available": False, "source_locked": False, "reason": "face_fidelity_unavailable"}


def _apply_face_fidelity(raw: dict[str, Any], fidelity: dict[str, Any]) -> None:
    if not fidelity.get("source_locked"):
        return
    checks = raw.get("checks")
    if not isinstance(checks, dict):
        return
    reason = "源图面部区域通过确定性像素保真检查，五官、脸型与年龄表情均保留。"
    for check_id in ("identity_likeness", "face_geometry", "age_expression"):
        candidate = checks.get(check_id)
        if not isinstance(candidate, dict):
            continue
        candidate["status"] = "pass"
        score = candidate.get("score")
        candidate["score"] = max(95, score if isinstance(score, (int, float)) and not isinstance(score, bool) else 0)
        candidate["reason"] = reason


def review_prepared_reference(
    original_path: Path | str,
    natural_reference_path: Path | str,
    printable_preview_path: Path | str,
    output_directory: Path | str,
    *,
    force: bool = False,
    completion: Callable[[str, str, tuple[Path, ...]], str] = complete_vision,
) -> dict[str, Any]:
    original = Path(original_path)
    natural = Path(natural_reference_path)
    printable = Path(printable_preview_path)
    output = Path(output_directory)
    report_path = output / REPORT_FILENAME
    hashes = ["", "", ""]
    try:
        for index, path in enumerate((original, natural, printable)):
            if not path.is_file():
                raise ReferenceVisualQualityError(f"The review image does not exist: {path}")
            hashes[index] = _sha256(path)
        model = os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4")
        if not force and report_path.is_file():
            try:
                cached = json.loads(report_path.read_text(encoding="utf-8"))
                if (
                    isinstance(cached, dict)
                    and cached.get("status") in {"pass", "review"}
                    and cached.get("review_version") == REVIEW_VERSION
                    and cached.get("original_sha256") == hashes[0]
                    and cached.get("natural_reference_sha256") == hashes[1]
                    and cached.get("printable_preview_sha256") == hashes[2]
                    and cached.get("model") == model
                ):
                    cached["cached"] = True
                    return cached
            except (OSError, UnicodeError, json.JSONDecodeError):
                pass
        fidelity = _face_pixel_fidelity(original, natural, output)
        sheet = _contact_sheet(original, natural, printable, output / "reference-review-sheet.png")
        response = completion(
            _system_prompt(),
            "请严格比较原图、自然 3D 参考图和打印配色图；相似但变年轻、变窄或更标准化也应判为需要复核。",
            (original, sheet),
        )
        raw = _json_object(response)
        _apply_face_fidelity(raw, fidelity)
        report = _normalize(raw, *hashes, model)
        report["deterministic_face_fidelity"] = fidelity
    except (OpenAIPreprocessorError, ReferenceVisualQualityError, OSError) as exc:
        report = _unavailable(str(exc), *hashes)
    _write(report_path, report)
    return report

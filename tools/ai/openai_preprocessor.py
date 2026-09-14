from __future__ import annotations

import base64
import binascii
import http.client
from io import BytesIO
import ipaddress
import json
import os
import shutil
import socket
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

try:
    from .ai_diagnostics import classify_connection_error, event as diagnostic_event, exception_details, safe_endpoint
except ImportError:
    from ai_diagnostics import classify_connection_error, event as diagnostic_event, exception_details, safe_endpoint

try:
    from .network_policy import build_opener as build_network_opener, network_diagnostics
except ImportError:
    from network_policy import build_opener as build_network_opener, network_diagnostics

try:
    from .printable_palette import (
        LEGACY_DEFAULT_PRINTABLE_COLORS,
        PrintablePaletteError,
        active_palette_roles,
        assign_palette_roles,
        normalize_palette,
    )
except ImportError:
    from printable_palette import (
        LEGACY_DEFAULT_PRINTABLE_COLORS,
        PrintablePaletteError,
        active_palette_roles,
        assign_palette_roles,
        normalize_palette,
    )

_DEFAULT_BASE_URL = "https://api.openai.com/v1"
_MAX_JSON_BYTES = 32 * 1024 * 1024
_MAX_IMAGE_BYTES = 20 * 1024 * 1024
_TIMEOUT_SECONDS = 120
_VISION_MAX_EDGE = 2048
_IMAGE_QUALITY_VALUES = {"low", "medium", "high", "auto"}
# Keep the historical filename for portrait detection/crop consumers. This mask
# is evidence only; it must never be used to paste source pixels into a preview.
PORTRAIT_FACE_LOCK_FILENAME = "portrait-face-restore-mask.png"

STYLE_PROFILES = {
    "sculpture": (
        "Change only the visible material treatment into one monochrome museum-quality plaster, clay, stone, or matte resin "
        "sculpture. Preserve the source subject one-for-one: the same identity, face, age, expression, body proportions, pose, "
        "silhouette, crop, clothing, accessories, objects, count, placement, and visible details. Do not redesign, beautify, "
        "exaggerate, add, remove, reveal, or reconstruct anything. Use gentle carved planes and broad connected forms only where "
        "needed for a printable single-material result. Keep every source-visible opening, handle, wheel, limb, tier, and base; "
        "monochrome means one material, not fewer components."
    ),
    "realistic": (
        "Create a multi-color realistic collectible while changing as little as possible beyond material and color treatment. "
        "Preserve the source subject one-for-one: the same recognizable identity, face, age, expression, anatomy, proportions, "
        "pose, silhouette, crop, clothing, accessories, objects, count, placement, and visible details. Use believable naturally colored "
        "materials and restrained realistic modeling; do not stylize facial proportions, invent detail, genericize manufactured "
        "parts, or alter the composition. When the subject is a real person, use the shape language of a highly faithful "
        "polychrome portrait sculpture or faithful 3D scan: carry identity in the actual face silhouette and sculpted anatomical "
        "planes while keeping source-faithful large material colors. Prioritize likeness over idealized attractiveness: retain "
        "the person's natural adult facial asymmetry and landmark proportions instead of applying a beauty-filter, toy, game-avatar, "
        "or generic commercial-character face. Mildly groom skin and hair surfaces only; never enlarge the eyes or irises, lift both "
        "brows into a stock expression, narrow the nose, widen the smile, taper the jaw into a V, or shorten the lower face. Do not "
        "pursue photographic beauty lighting or painted skin detail at the expense of recognizable three-dimensional facial geometry."
    ),
    "portrait_sketch": (
        "Restyle the same real person as an identity-first portrait sketch sculpture. Preserve the exact recognizable identity, "
        "adult age, expression, facial silhouette, landmark spacing, asymmetry, hairstyle, pose, crop, clothing, and accessories. "
        "Use restrained exaggeration only to clarify an already-present brow, cheek, smile fold, jaw transition, or gesture; never "
        "replace the person with a generic caricature, idealized celebrity, doll, anime face, or stock street-artist template. Translate "
        "tone into expressive material or relief forms with continuous sculptural modeling. Prefer a few confident, "
        "printer-width contour grooves and connected planes over drawn texture. Use only semantically truthful masses, and do not force every "
        "selected color to appear when that would create a tiny or false region."
    ),
    "cartoon": (
        "Restyle the same subject as a friendly cute cartoon collectible, especially for portraits that look harsh when rendered "
        "realistically. Preserve recognizable identity, age, expression, hairstyle, pose, clothing, accessories, visible objects, "
        "subject count, crop, and composition. Use rounded connected forms, clean large shapes, and modest playful simplification. "
        "Do not replace the face with a generic doll or anime face, do not enlarge eyes excessively, and do not add or remove "
        "elements. Make the result cute through expression, clean curves, and material treatment rather than changing identity, "
        "age, anatomy, or the subject's distinctive proportions."
    ),
    "low_poly": (
        "Restyle the same subject as a deliberate low-poly printable model built from broad, clean planar facets. Preserve the "
        "recognizable silhouette, viewpoint, component count, pose, and identity-defining proportions, but replace fragile surface "
        "detail, fur, foliage, fabric texture, and shallow ornament with a small number of sturdy geometric planes. Keep every "
        "load-bearing connection visibly fused and avoid random triangulation noise or razor-thin spikes."
    ),
    "relief": (
        "Convert the source into a printable shallow bas-relief mounted on one simple solid plaque. Preserve the source-facing "
        "silhouette and recognizable internal contours while expressing depth with a few broad raised levels. Do not reconstruct "
        "an unseen back side, create undercuts, detach foreground elements, or add a decorative frame unless it is already requested."
    ),
    "ink_relief": (
        "Convert the complete visible composition into one printmaking- and woodcut-inspired ink relief on a simple solid backing plaque. "
        "Preserve subject identity, viewpoint, count, silhouette, and recognizable internal contours. Organize positive and negative space "
        "into two to four broad value masses, and turn only the most important brush or cut marks into connected, printer-width embossed or "
        "engraved strokes. Use shallow stepped depth and solid opaque regions; forbid translucent washes, wet-on-wet gradients, gray mist, "
        "micro-halftone dots, stippling, hairline hatching, and floating ink flecks. Use only semantically truthful masses, and do not force "
        "every selected color to appear when that would create a tiny or false region."
    ),
    "diorama": (
        "Restyle the complete visible composition as one compact printable miniature diorama. Preserve the main subjects, their "
        "relative placement, viewpoint, and scene identity, but merge the ground and supporting elements into one stable base. "
        "Simplify distant detail into layered masses, keep subject count unchanged, and avoid floating props, loose foliage, thin "
        "rails, or deep hidden cavities."
    ),
}

LEGACY_STYLE_ALIASES = {
    "q_cartoon": "cartoon",
    "cel_shaded": "cartoon",
    "enamel_inlay": "realistic",
}
STYLE_PROFILES.update({legacy: STYLE_PROFILES[canonical] for legacy, canonical in LEGACY_STYLE_ALIASES.items()})
IDENTITY_FIRST_PORTRAIT_STYLES = frozenset({"realistic", "portrait_sketch"})
PRINT_NATIVE_ARTISTIC_STYLES = frozenset({"portrait_sketch", "ink_relief"})

CUSTOM_STYLE_ID = "custom"
MAX_CUSTOM_STYLE_BYTES = 1000
MAX_PALETTE_RECOMMENDATION_SUMMARY_BYTES = 400
MAX_PALETTE_RECOMMENDATION_NAME_BYTES = 80
MAX_PALETTE_RECOMMENDATION_USAGE_BYTES = 160
MAX_PALETTE_RECOMMENDATION_REASON_BYTES = 400


class OpenAIPreprocessorError(RuntimeError):
    """An OpenAI-compatible preprocessing request failed safely."""

    def __init__(
        self,
        message: str,
        *,
        code: str = "image_preprocess_failed",
        retryable: bool = False,
        ambiguous: bool = False,
    ):
        super().__init__(message)
        self.code = code
        self.retryable = retryable
        self.ambiguous = ambiguous


@dataclass(frozen=True)
class ImageProviderConfig:
    base_url: str
    api_key: str
    model: str
    source: str


@dataclass(frozen=True)
class PrintablePaletteRecommendationColor:
    hex: str
    name: str
    role: str
    usage: str
    reason: str

    def as_dict(self) -> dict[str, str]:
        return {
            "hex": self.hex,
            "name": self.name,
            "role": self.role,
            "usage": self.usage,
            "reason": self.reason,
        }


@dataclass(frozen=True)
class PrintablePaletteRecommendation:
    summary: str
    colors: tuple[PrintablePaletteRecommendationColor, ...]

    def as_dict(self) -> dict[str, Any]:
        return {"summary": self.summary, "colors": [color.as_dict() for color in self.colors]}


def _normalized_base_url(raw_base: str, environment_name: str) -> str:
    raw_base = raw_base.strip()
    parsed = urllib.parse.urlsplit(raw_base)
    if (
        parsed.scheme.lower() != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise OpenAIPreprocessorError(
            f"{environment_name} must be a credential-free HTTPS URL.",
            code="image_provider_misconfigured" if environment_name == "OPENAI_PRO_URL" else "image_preprocess_failed",
        )
    path = parsed.path.rstrip("/") or "/v1"
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, path, "", ""))


def _config() -> tuple[str, str, str, str]:
    """Return the legacy text/vision provider without consulting Image2 PRO settings."""
    key = os.environ.get("OPENAI_API_KEY", "")
    raw_base = os.environ.get("OPENAI_BASE_URL", _DEFAULT_BASE_URL).strip()
    if not key:
        raise OpenAIPreprocessorError("OPENAI_API_KEY is not configured.")
    base = _normalized_base_url(raw_base, "OPENAI_BASE_URL")
    return (
        base,
        key,
        os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4"),
        os.environ.get("OPENAI_IMAGE_MODEL", "gpt-image-2"),
    )


def _image_config() -> ImageProviderConfig:
    """Resolve Image2 once, before a request, without cross-channel retry.

    Any presence of a PRO setting selects the PRO channel and requires the pair
    to be complete. Legacy settings are consulted only when PRO is completely
    absent, so a partially deployed PRO configuration cannot silently bill a
    different provider.
    """
    pro_key = os.environ.get("OPENAI_PRO_API", "").strip()
    pro_url = os.environ.get("OPENAI_PRO_URL", "").strip()
    if pro_key or pro_url:
        if not pro_key or not pro_url:
            raise OpenAIPreprocessorError(
                "OPENAI_PRO_API and OPENAI_PRO_URL must both be configured for Image2.",
                code="image_provider_misconfigured",
            )
        return ImageProviderConfig(
            base_url=_normalized_base_url(pro_url, "OPENAI_PRO_URL"),
            api_key=pro_key,
            model=os.environ.get("OPENAI_IMAGE_MODEL", "gpt-image-2"),
            source="pro",
        )

    legacy_key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not legacy_key:
        raise OpenAIPreprocessorError(
            "Image2 is not configured. Set OPENAI_PRO_API and OPENAI_PRO_URL.",
            code="image_provider_not_configured",
        )
    legacy_url = os.environ.get("OPENAI_BASE_URL", _DEFAULT_BASE_URL).strip()
    return ImageProviderConfig(
        base_url=_normalized_base_url(legacy_url, "OPENAI_BASE_URL"),
        api_key=legacy_key,
        model=os.environ.get("OPENAI_IMAGE_MODEL", "gpt-image-2"),
        source="legacy",
    )


def image_provider_status() -> dict[str, Any]:
    """Return non-secret Image2 route capability metadata for local health checks."""
    try:
        config = _image_config()
    except OpenAIPreprocessorError:
        if (
            os.environ.get("OPENAI_PRO_API", "").strip()
            or os.environ.get("OPENAI_PRO_URL", "").strip()
        ):
            source = "pro"
        elif os.environ.get("OPENAI_API_KEY", "").strip():
            source = "legacy"
        else:
            source = "missing"
        return {
            "available": False,
            "source": source,
            "base_url": "",
            "endpoints": {"generations": "", "edits": ""},
            "automatic_retry": False,
            "max_billable_requests_per_action": 1,
        }
    base = safe_endpoint(config.base_url).rstrip("/")
    return {
        "available": True,
        "source": config.source,
        "base_url": base,
        "endpoints": {
            "generations": base + "/images/generations",
            "edits": base + "/images/edits",
        },
        "automatic_retry": False,
        "max_billable_requests_per_action": 1,
    }


def _image_quality() -> str:
    value = os.environ.get("OPENAI_IMAGE_QUALITY", "high").strip().lower()
    if value not in _IMAGE_QUALITY_VALUES:
        raise OpenAIPreprocessorError("OPENAI_IMAGE_QUALITY must be low, medium, high, or auto.")
    return value


def _read_json_response(response: Any) -> dict[str, Any]:
    content_length = response.headers.get("Content-Length")
    if content_length:
        try:
            if int(content_length) > _MAX_JSON_BYTES:
                raise OpenAIPreprocessorError("The preprocessing service returned an oversized response.")
        except ValueError:
            pass
    raw = response.read(_MAX_JSON_BYTES + 1)
    if len(raw) > _MAX_JSON_BYTES:
        raise OpenAIPreprocessorError("The preprocessing service returned an oversized response.")
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise OpenAIPreprocessorError("The preprocessing service returned an invalid response.") from None
    if not isinstance(value, dict):
        raise OpenAIPreprocessorError("The preprocessing service returned an invalid response.")
    return value


def _request_with_provider(
    path: str,
    body: bytes,
    content_type: str,
    *,
    base: str,
    key: str,
    provider_source: str,
) -> dict[str, Any]:
    endpoint = base + path
    request = urllib.request.Request(endpoint, data=body, method="POST")
    request.add_header("Authorization", "Bearer " + key)
    request.add_header("Content-Type", content_type)
    request.add_header("Accept", "application/json")
    started = time.monotonic()
    network = network_diagnostics(endpoint)
    diagnostic_event(
        "provider.request.started",
        endpoint=safe_endpoint(endpoint),
        request_bytes=len(body),
        content_type=content_type,
        timeout_seconds=_TIMEOUT_SECONDS,
        provider_source=provider_source,
        network=network,
    )
    try:
        with build_network_opener(_RejectRedirects()).open(request, timeout=_TIMEOUT_SECONDS) as response:
            result = _read_json_response(response)
            diagnostic_event(
                "provider.request.completed",
                endpoint=safe_endpoint(endpoint),
                http_status=getattr(response, "status", 200),
                elapsed_ms=round((time.monotonic() - started) * 1000),
                network=network,
            )
            return result
    except OpenAIPreprocessorError:
        diagnostic_event(
            "provider.response.invalid",
            level="ERROR",
            endpoint=safe_endpoint(endpoint),
            elapsed_ms=round((time.monotonic() - started) * 1000),
            network=network,
            provider_source=provider_source,
        )
        raise
    except urllib.error.HTTPError as exc:
        if exc.code in (401, 403):
            message = "The preprocessing service credentials or model permissions are invalid."
            code = "image_auth_failed"
            retryable = False
            ambiguous = False
        elif exc.code == 429:
            message = "The preprocessing service is rate limiting requests; try again later."
            code = "image_rate_limited"
            retryable = True
            ambiguous = False
        elif 400 <= exc.code < 500:
            message = "The preprocessing service rejected the request."
            code = "image_rejected"
            retryable = False
            ambiguous = False
        else:
            message = "The preprocessing service is temporarily unavailable."
            code = "image_service_unavailable"
            retryable = True
            ambiguous = True
        safe_headers = {
            name: exc.headers.get(name, "")
            for name in ("x-request-id", "request-id", "cf-ray", "retry-after")
            if exc.headers and exc.headers.get(name)
        }
        diagnostic_event(
            "provider.http.failed",
            level="ERROR",
            endpoint=safe_endpoint(endpoint),
            http_status=exc.code,
            response_headers=safe_headers,
            elapsed_ms=round((time.monotonic() - started) * 1000),
            network=network,
            provider_source=provider_source,
            exception_chain=exception_details(exc),
        )
        raise OpenAIPreprocessorError(
            message, code=code, retryable=retryable, ambiguous=ambiguous
        ) from None
    except (urllib.error.URLError, TimeoutError, OSError, http.client.HTTPException) as exc:
        diagnostic_event(
            "provider.connection.failed",
            level="ERROR",
            endpoint=safe_endpoint(endpoint),
            elapsed_ms=round((time.monotonic() - started) * 1000),
            network=network,
            provider_source=provider_source,
            failure_kind=classify_connection_error(exc),
            exception_chain=exception_details(exc),
        )
        raise OpenAIPreprocessorError(
            "Could not connect to the preprocessing service.",
            code="image_connection_failed",
            retryable=True,
            ambiguous=True,
        ) from None


class _RejectRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> None:
        return None


def _content_text(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "".join(_content_text(part) for part in value)
    if isinstance(value, dict):
        for key in ("text", "output_text", "content"):
            if key in value:
                return _content_text(value[key])
    return ""


def complete_text(system_prompt: str, user_content: str) -> str:
    if not isinstance(system_prompt, str) or not system_prompt.strip():
        raise OpenAIPreprocessorError("A system prompt is required.")
    if not isinstance(user_content, str) or not user_content.strip():
        raise OpenAIPreprocessorError("User content is required.")
    _, _, model, _ = _config()
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_content},
        ],
    }
    result = _provider_request(
        "/chat/completions",
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"),
        "application/json",
    )
    content = _completion_content(result)
    if not content.strip():
        raise OpenAIPreprocessorError("The preprocessing service returned an empty response.")
    return content.strip()


def _completion_content(result: dict[str, Any]) -> str:
    choices = result.get("choices")
    content = ""
    if isinstance(choices, list) and choices and isinstance(choices[0], dict):
        message = choices[0].get("message")
        content = _content_text(message.get("content") if isinstance(message, dict) else message)
    if not content:
        content = _content_text(result.get("output_text")) or _content_text(result.get("output"))
    return content


def _style_profile(style: str, custom_style: str = "") -> str:
    if style == CUSTOM_STYLE_ID:
        if not isinstance(custom_style, str) or not custom_style.strip():
            raise OpenAIPreprocessorError("A custom style description is required.")
        description = custom_style.strip()
        if len(description.encode("utf-8")) > MAX_CUSTOM_STYLE_BYTES:
            raise OpenAIPreprocessorError("The custom style description exceeds the 1000-byte limit.")
        return (
            "Apply this user-defined visual style to the existing subject: "
            + description
            + ". Treat it only as appearance and shape-language direction. The subject identity, image-to-3D composition, "
            "structural connections, stable base, and other hard constraints in this request take priority."
        )
    profile = STYLE_PROFILES.get(LEGACY_STYLE_ALIASES.get(style, style))
    if profile is None:
        raise OpenAIPreprocessorError("The selected style is not supported.")
    return profile


def _designer_toy_profile(style: str, custom_style: str = "") -> str:
    canonical_style = LEGACY_STYLE_ALIASES.get(style, style)
    if canonical_style == "realistic":
        return (
            _style_profile(style, custom_style)
            + " Render the existing subject as a museum-grade polychrome portrait maquette, faithful full-color scan, or "
            "anatomically realistic scale collectible made from separate solid-color materials. Preserve real adult facial "
            "proportions and identity ahead of symmetry, smoothness, youthfulness, beauty, or commercial appeal. This must not "
            "look like a designer toy, game avatar, animation character, waxy doll, or generic smiling spokesperson. Keep "
            "source-specific eyelid heights, eye size, nose width, cheek fullness, jaw width, chin length, smile line and small "
            "asymmetries; mild beautification may clean skin and hair texture but must not move or resize facial landmarks. "
            "Use intentional printable color blocking without flattening the sculptural planes that carry likeness."
        )
    if canonical_style == "portrait_sketch":
        return (
            _style_profile(style, custom_style)
            + " Treat the broad values as solid printable material families, not painted lighting. Preserve continuous form evidence "
            "for the face and use a selected color only where it owns a meaningful connected portrait region."
        )
    if canonical_style == "low_poly":
        return (
            _style_profile(style, custom_style)
            + " Assign each broad facet or connected facet group one allowed solid material color. Keep color boundaries aligned "
            "with major planes; do not use gradients, mottling, tiny checker patterns, or photographic texture."
        )
    if canonical_style in {"relief", "ink_relief"}:
        return (
            _style_profile(style, custom_style)
            + " Use the allowed material colors only for a few large relief levels or semantic regions. Keep the plaque, raised "
            "subject, and every colored region physically connected as one printable object."
        )
    if canonical_style == "diorama":
        return (
            _style_profile(style, custom_style)
            + " Use the allowed material colors as large scene and subject regions. Every figure, prop, and terrain mass must "
            "connect to the shared base; do not use lighting gradients as material boundaries."
        )
    return (
        _style_profile(style, custom_style)
        + " Render the existing subject as a premium full-color designer toy made from separate solid-color materials. "
        "Keep the selected profile's geometry, but replace monochrome stone, plaster, photographic, or natural-material "
        "treatment with intentional printable color blocking. The printable color treatment overrides any monochrome "
        "material wording in the base style profile."
    )


def _designer_toy_palette_direction(
    palette: tuple[str, ...],
    shadow_color: str = "",
    palette_roles: Mapping[str, str] | None = None,
    *,
    allow_meaningful_subset: bool = False,
) -> str:
    del shadow_color  # Accepted for backward-compatible callers; roles are now based on actual filament colors.
    try:
        assignment = assign_palette_roles(palette, palette_roles)
    except PrintablePaletteError as exc:
        raise OpenAIPreprocessorError(str(exc)) from None
    required = min(3, len(palette))
    role_text = ", ".join(f"{role}={color}" for role, color in assignment.color_by_role.items())
    active_roles = set(assignment.color_by_role)
    coverage_roles = "primary and structure materials" if "structure" in active_roles else "primary material"
    connector_role = "structure" if "structure" in active_roles else "primary"
    role_descriptions = {
        "primary": "the primary color covers the largest subject material",
        "structure": "the structure color covers rear surfaces, seams, boundaries, and load-bearing regions",
        "light": "the light color is reserved for enclosed highlights or a face panel",
        "accent": "the accent color marks one secondary semantic part",
        "secondary": "the secondary color covers another broad semantic part",
        "detail": "the detail color marks a bounded, printable identifying feature",
    }
    role_usage = "; ".join(role_descriptions[role] for role in assignment.color_by_role) + ". "
    coverage_direction = (
        "Use only listed colors that own clear, meaningful subject regions. It is acceptable to use a meaningful subset of "
        "the selected palette; never invent a false region, speckle, or tiny accent merely to consume another color. "
        if allow_meaningful_subset
        else "Use at least "
        + str(required)
        + " listed colors in clearly visible, meaningful regions; do not hide required colors in speckles or tiny accents. "
    )
    face_direction = (
        "For a person or character, use the light material for the face only when a source-faithful hairline and a separate neck "
        "or collar provide readable boundaries. "
        if "light" in active_roles else ""
    )
    accent_direction = (
        "A secondary garment assigned to the accent material must stay one continuous accent-colour region: express folds as "
        "shallow geometry and soft illumination, never as broad structure-colour or skin-colour patches inside that garment. "
        if "accent" in active_roles else ""
    )
    connector_direction = (
        "Render every thin load-bearing shaft, rib, spoke, rail, handle, branch, cable, antenna, and support in the "
        + connector_role
        + " color as one flat opaque material from end to end. Never render such a connector as silver, white, translucent, "
        + "reflective, highlighted, or background-colored, because palette mapping must not break its silhouette. "
    )
    signature_direction = (
        "Render identity-defining engraved lines, emblem ridges, grille bars, panel divisions, and part boundaries as a few "
        "continuous, printer-width "
        + connector_role
        + "-colored grooves or bands. Do not express signature details only with highlights, shadows, or subtle tone changes "
        + "that will disappear during exact-palette mapping. "
    )
    return (
        "Treat these colors as the allowed printable palette: "
        + ", ".join(palette)
        + ". Every visible subject surface must use one of these material colors. Keep the outer silhouette, neck, limbs, "
        "load-bearing connections, base contact, and stacked architectural tiers visibly joined by opaque palette-colored geometry; "
        "never cut structural connections out as background or transparency. Cover at least 65 percent of the visible subject with "
        + coverage_roles
        + ". Assign the remaining colors to large semantic parts rather than lighting gradients. "
        + face_direction
        + "Preserve source-visible ears and the open jawline. Never extend hair, a collar, "
        "or a hood around the cheeks or under the chin unless that exact enclosure exists in the source. The collar must connect "
        "the neck to the torso, not encircle the face. Never retain natural flesh tones unless the exact skin-like shade is listed. "
        "Do not introduce beige, gray, black, off-white, natural skin tones, or dark substitutes unless that exact shade is one of "
        "the listed printable colors; every listed color remains valid regardless of its common color name. "
        + coverage_direction
        + "Use broad contiguous regions with hard readable boundaries. A deterministic print-mapping step will convert the "
        "result to exact filament colors. Use these perceptual material roles derived from the actual loaded filaments: "
        + role_text
        + ". "
        + role_usage
        + connector_direction
        + "For a plant, bonsai, coral, antler, feather fan, or other branching organic subject, merge small leaves or repeated "
        "tips into fewer overlapping solid clusters. Visibly fuse every cluster through a sturdy branch or stem to the trunk, "
        "body, or base; never leave an isolated leaf pad, floating frond, or contact-only branch shell. "
        "Keep each semantic material consistent across its whole visible part, including surfaces turning toward the side; never "
        "scatter a material into isolated freckles, inferred rear patches, edge highlights, or random speckles. "
        + accent_direction
        + "Treat each semantic part as one stable base material assignment. Preserve restrained neutral studio illumination and "
        "broad sculptural light-to-shadow modeling so the nose, eyelids, cheekbones, folds, joints, and silhouette remain legible "
        "to image-to-3D, but never paint that illumination as a second material, a colored rim, a freckle, or a hard color patch. "
        "Keep the underlying hue and semantic ownership of each part unambiguous across lit, side-facing, and occluded surfaces; "
        "the deterministic print-mapping step, not the generated image, will collapse illumination to exact filament colors. "
        "Once a face material is selected, keep it continuous across the face, ears, neck, and visible hands; never break skin "
        "with clothing-colored forehead, nose, cheek, chin, ear, neck, or hand highlights. A smile may use one small connected "
        "teeth band, but not scattered tooth or highlight islands. "
        + signature_direction
        + "Place the subject on a genuinely transparent alpha background. Never draw, paint, or simulate a transparency checkerboard, "
        "grid, checker pattern, or white-and-gray tiles into the RGB image. If alpha transparency is unavailable, use one uniform "
        "studio background with no "
        "shadow whose color is clearly separated from every listed palette color and every subject region. The fallback background "
        "must not use or resemble a palette color and must not be used on the subject. "
    )


def _portrait_display_base_direction() -> str:
    return (
        "Portrait stability rule: when the selected primary subject is a real person or a human character, always add exactly "
        "one low, simple, integrated display base even when the source portrait has no base. This is the only category-specific "
        "exception to preserving a source-visible base and to the general prohibition on adding elements. Give the base a flat "
        "underside, keep it visually subordinate to the person, and fuse it with opaque load-bearing geometry: there must be no "
        "gap, floating foot, floating torso, or shadow-only contact. For a head-and-shoulders, chest, waist, or other cropped human "
        "portrait, keep the exact visible anatomical extent, finish the existing lower torso or clothing cleanly, and fuse that "
        "boundary to a compact round, oval, or softly polygonal bust plinth; do not invent a pelvis, legs, or feet. For a complete, "
        "standing, seated, or crouched person, preserve the full pose and connect the actual lowest visible contacts such as feet, "
        "garment, or seat to one low base without moving the limbs. For an explicitly requested pair or group of people, use one "
        "shared low base while preserving count, identities, left-right order, spacing, poses, and accessories. In multicolor mode, "
        "make the base one broad printable palette region; in monochrome mode, keep the same single material as the portrait. "
        "Do not apply this portrait exception to an animal, product, vehicle, machine, building, or other non-human subject. "
    )


def _portrait_identity_geometry_direction() -> str:
    return (
        "Real-person identity geometry rule: preserve adult age, source-visible feminine or masculine presentation, face width "
        "and length, hair part and sweep, hairline, visible ears, eye spacing and "
        "shape, eyebrow arc, nose bridge/width/tip, mouth width, smile asymmetry, cheek volume, jaw contour, and chin length. "
        "Match the source landmark ratios rather than a memorized attractive face: inter-eye distance, visible eye opening, brow-to-eye "
        "distance, nose width and projection, nose-to-mouth distance, mouth width, upper-to-lower lip balance, cheek width, and the lower "
        "facial third must stay source-faithful. Preserve small left-right differences in eyelids, brows, smile corners, cheeks, and jaw. "
        "Encode the eyelids, nose, cheekbones, smile folds, mouth corners, and jaw transition as restrained modelable relief and "
        "silhouette, not only as gradients, highlights, makeup, or thin painted lines. Keep teeth as one shallow readable smile "
        "band rather than many tiny separate teeth. Never turn an adult into a big-eyed childlike, game-avatar, beauty-filtered, or generic "
        "doll face. Do not make both eyes wider or rounder than the source, and do not replace natural facial asymmetry with perfect symmetry. Preserve "
        "the exact source-visible crossed-arm order, exposed wrist and hand count, jacket lapels, and inner neckline; group fingers "
        "into sturdy forms, fuse wrists to sleeves and forearms, and never mirror or invent a second hand. A watch may be "
        "simplified into one solid fitted band. Preserve garment coverage exactly around every clothed arm: if a jacket or sleeve "
        "covers an elbow, upper arm, forearm, or wrist in the source, continue that same garment as one closed tube around the "
        "underside, side, and occluded back. Never invent a bare elbow, upper arm, or forearm behind a crossed arm, and never use "
        "skin color as an inferred shadow on clothing. Skin is allowed only on source-visible face, ears, neck, hands, and exposed "
        "wrist areas. With crossed arms, preserve every clearly visible hand, but do not turn a thin partially occluded skin sliver "
        "into a stripe across the jacket: either show it as one compact, anatomically bounded hand/wrist region or keep that ambiguous "
        "sliver fully tucked beneath the existing sleeve without changing the arm order. Do not age the person up, change their presentation, broaden or narrow the face, "
        "or replace an asymmetric hairstyle with a generic centered cap of hair. "
    )


def _difficult_structure_direction() -> str:
    return (
        "Difficult-structure rule: for a vehicle, machine, tool, or articulated product, keep every wheel, bucket, blade, lens, "
        "mirror, handle, and moving attachment connected through visibly overlapping solid pin housings, axles, arms, or thick "
        "opaque rods. Each structural joint must be a positive-volume union: extend every axle, piston rod, hinge pin, arm, and "
        "brace visibly inside the receiving housing, with generous overlap on both sides. Butt contact, near-touching tips, cast "
        "shadows, painted lines, and a loose pin beside the machine do not count as a connection. Keep a bucket or blade merged "
        "to its final arm through one thickened joint block; keep every linkage merged back to the main chassis. Preserve readable "
        "joint gaps as shallow recessed grooves instead of separating an attachment into another island. If a realistic linkage "
        "cannot remain fused, simplify it into one solid load-bearing brace while preserving the outer silhouette and function. "
        "For a fan, feather screen, wing, sail, umbrella canopy, leaf, or other broad thin surface, give the surface visible finite "
        "thickness and fuse its ribs into a continuous rim, hub, body, or trunk; never use a paper-thin single sheet. For an open "
        "umbrella, make the central shaft penetrate and fuse into the canopy hub and the lowest support, embed every rib along the "
        "canopy instead of leaving wire-like struts, and use shallow panel grooves rather than separated fabric panels. For a fan "
        "tail or feather display, fuse the screen to a broad body or support mass instead of relying on isolated feather tips or "
        "thin legs alone. "
        "When an explicitly requested group contains two or more separate people, characters, or animals as one display model, "
        "place every subject on one shared low integrated base while preserving exact count, spacing, left-right order, pose, and "
        "individual silhouettes; do not fuse their bodies together merely to obtain connectivity. "
    )


def _style_support_override(style: str) -> str:
    canonical_style = LEGACY_STYLE_ALIASES.get(style, style)
    if canonical_style in {"relief", "ink_relief"}:
        return (
            "RELIEF SUPPORT OVERRIDE — highest priority for this style: the one simple solid backing plaque is mandatory, "
            "including for people, animals, products, machines, furniture, and complete scenes. This overrides both the portrait "
            "display-base rule and the non-human base-free rule. Compress the entire visible composition into a shallow front-facing "
            "relief fused across broad contact areas to that plaque; never return a free-standing figurine, product, or diorama. "
            "Keep a clean visible plaque margin around the raised subject and do not add a second pedestal, floor, or decorative frame. "
        )
    if canonical_style == "diorama":
        return (
            "DIORAMA SUPPORT OVERRIDE — highest priority for this style: one shared low terrain or floor base with a flat underside "
            "is mandatory and overrides the non-human base-free rule. Fuse every requested subject and prop to that one base. Preserve "
            "only source-visible or explicitly requested scene elements. For an isolated subject, use a minimal plain contact platform "
            "without inventing rocks, plants, furniture, buildings, signs, or other decorative scenery. Never return a base-free product "
            "shot or an ordinary display figurine with no scene-level ground relationship. "
        )
    return ""


def _non_realistic_text_cleanup_direction(style: str) -> str:
    if LEGACY_STYLE_ALIASES.get(style, style) == "realistic":
        return ""
    return (
        "NON-REALISTIC TEXT CLEANUP — high priority: remove every readable word, brand, logo, serial number, label, watermark, "
        "and pseudo-letter from the subject as well as the background. Preserve the panel, badge, or engraving footprint only as "
        "one blank recessed panel, broad unlettered groove, or solid color block. Do not copy source glyphs and do not invent "
        "plausible substitute spelling. "
    )


def _solid_background_direction() -> str:
    return (
        "Use one uniform opaque solid-color studio background, preferably neutral mid-gray. "
        "Choose a different uniform tone if needed to clearly separate the background from every subject region, "
        "including white clothing, hair and the base; never recolor the subject to create contrast. "
        "Do not request transparency or draw a transparency checkerboard, checker pattern, grid, tiles, "
        "background texture, gradient, scenery, floor shadow or halo. Replace any such backdrop in the source image. "
        "Preserve the complete subject and base with clear empty margins on all sides. "
    )


def _image_to_3d_composition_direction(transparent_background: bool = False, style: str = "") -> str:
    support_override = _style_support_override(style)
    if support_override:
        support_direction = support_override
    else:
        support_direction = (
            "Except for the mandatory portrait base rule below, a non-human standing, seated, crouched, lying, wheeled, "
            "naturally stable, or cleanly cropped subject must remain base-free when the source is base-free. Do not add a disc, "
            "plinth, stand, platform, presentation base, floor slab, or pedestal to a non-human subject unless the user explicitly "
            "requests one. "
        )
    return (
        "Recompose the selected primary subject as a clean product-shot reference for image-to-3D rather than editing the "
        "photograph in place. Center the exact requested subject or explicitly requested subject group as one readable composition on "
        + ("a transparent background" if transparent_background else "a uniform opaque solid-color background")
        + ", show a coherent complete silhouette, and use a front or gentle three-quarter view. Preserve any base, support, floor "
        "slab, or contact surface that is visibly part of the selected source subject. "
        + support_direction
        + _portrait_display_base_direction()
        + _difficult_structure_direction()
        + "If a thin visible part would otherwise become disconnected, use the smallest integrated material bridge or subtle "
        "thickening needed for continuity rather than adding a display base. Remove scenery, floor shadows, text, logos, watermarks, camera UI, "
        "color cards, and unrelated people, plants, props, or landmarks. Do not combine separate scene elements into one object. "
        "Choose every subject named by the user when visible. If the user explicitly requests multiple subjects, a pair, a group, "
        "or a set, preserve the exact requested count, identities, left-right order, relative spacing, poses, and accessories as one "
        "closed composition; never silently drop, merge, duplicate, or replace a requested member. Otherwise choose the visually "
        "dominant foreground subject. "
        "Apply these source-dependent framing rules: if the complete person, animal, or object is visible, preserve the complete "
        "head-to-toe or whole-object form and its existing pose. If a person is cropped before the knees or only the upper body is "
        "visible, create a deliberately finished bust or half-body collectible: preserve only the visible head, torso, arms, and "
        "clothing, end the lower torso with a clean sculpted boundary fused to the required compact portrait base, and do not invent "
        "a pelvis, legs, or feet. If the source is a multi-subject scenic photograph and the user did not explicitly request a "
        "pair, group, set, or exact subject count, isolate exactly one requested or dominant subject and omit all secondary subjects "
        "and background scenery. Never duplicate a face, limb, tower, statue, accessory, or architectural element. "
        "Determine this source crop and visible anatomical extent before applying style or palette. Changing palette mode, palette "
        "colors, or print constraints must not change full-body versus bust framing or reveal anatomy outside the source crop. "
    )


def preprocess_text(
    instruction: str,
    palette: tuple[str, ...] = (),
    style: str = "sculpture",
    custom_style: str = "",
) -> str:
    if not isinstance(instruction, str) or not instruction.strip():
        raise OpenAIPreprocessorError("A text instruction is required.")
    palette_instruction = (
        " Preserve the user's color intent, natural gradients and material detail. "
        "Do not restrict colors to a printer palette or reduce the color count."
    )
    content = complete_text(
        (
            "Rewrite the user's request as one concise prompt for a text-to-3D model. "
            "Describe exactly one fused connected watertight printable object with stable geometry, a flat base, "
            "adequate wall thickness, and no unsupported details. Preserve the requested subject "
            "and style. Apply this visual profile: " + _style_profile(style, custom_style) + palette_instruction +
            " Return only the prompt, without markdown."
        ),
        instruction,
    )
    prompt = content.strip()
    if prompt.startswith("```") and prompt.endswith("```"):
        lines = prompt.splitlines()
        prompt = "\n".join(lines[1:-1]).strip()
    if not prompt:
        raise OpenAIPreprocessorError("The preprocessing service returned an empty prompt.")
    return prompt


def _image_kind(path: Path) -> tuple[str, str]:
    try:
        with path.open("rb") as stream:
            signature = stream.read(16)
    except OSError:
        raise OpenAIPreprocessorError("The input image could not be read.") from None
    if signature.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png", "png"
    if signature.startswith(b"\xff\xd8\xff"):
        return "image/jpeg", "jpg"
    raise OpenAIPreprocessorError("The input image must be PNG or JPEG.")


def _vision_image_data(path: Path) -> tuple[str, bytes]:
    """Bound review uploads without altering the original or model reference."""
    from PIL import Image, ImageOps

    try:
        with Image.open(path) as source:
            image = ImageOps.exif_transpose(source).convert("RGBA")
            image.thumbnail((_VISION_MAX_EDGE, _VISION_MAX_EDGE), Image.Resampling.LANCZOS)
            background = Image.new("RGB", image.size, "white")
            background.paste(image, mask=image.getchannel("A"))
            output = BytesIO()
            background.save(output, format="JPEG", quality=92)
            return "image/jpeg", output.getvalue()
    except (OSError, ValueError, Image.DecompressionBombError):
        raise OpenAIPreprocessorError("A review image could not be decoded.") from None


def complete_vision(system_prompt: str, user_content: str, image_paths: tuple[Path, ...]) -> str:
    if not isinstance(system_prompt, str) or not system_prompt.strip():
        raise OpenAIPreprocessorError("A system prompt is required.")
    if not isinstance(user_content, str) or not user_content.strip():
        raise OpenAIPreprocessorError("User content is required.")
    if not image_paths or len(image_paths) > 2:
        raise OpenAIPreprocessorError("One or two review images are required.")
    total_size = 0
    content: list[dict[str, Any]] = [{"type": "text", "text": user_content}]
    for raw_path in image_paths:
        path = Path(raw_path)
        _image_kind(path)
        mime_type, image = _vision_image_data(path)
        total_size += len(image)
        if not image or total_size > _MAX_IMAGE_BYTES:
            raise OpenAIPreprocessorError("The combined review images exceed the 20 MB limit.")
        content.append(
            {
                "type": "image_url",
                "image_url": {
                    "url": f"data:{mime_type};base64," + base64.b64encode(image).decode("ascii"),
                    "detail": "high",
                },
            }
        )
    _, _, model, _ = _config()
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": content},
        ],
    }
    result = _provider_request(
        "/chat/completions",
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"),
        "application/json",
    )
    response = _completion_content(result).strip()
    if not response:
        raise OpenAIPreprocessorError("The preprocessing service returned an empty response.")
    return response


def _bounded_recommendation_text(value: Any, field: str, maximum_bytes: int) -> str:
    if not isinstance(value, str) or not value.strip():
        raise OpenAIPreprocessorError(f"The palette recommendation {field} is required.")
    text = value.strip()
    if len(text.encode("utf-8")) > maximum_bytes:
        raise OpenAIPreprocessorError(f"The palette recommendation {field} is too long.")
    return text


def _palette_recommendation_payload(response: str) -> dict[str, Any]:
    text = response.strip()
    if text.startswith("```") and text.endswith("```"):
        lines = text.splitlines()
        if len(lines) >= 3:
            text = "\n".join(lines[1:-1]).strip()
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        raise OpenAIPreprocessorError("The palette recommendation is not valid JSON.") from None
    if not isinstance(value, dict):
        raise OpenAIPreprocessorError("The palette recommendation must be a JSON object.")
    return value


def recommend_printable_palette(
    instruction: str,
    style: str,
    custom_style: str = "",
    image_path: Path | None = None,
    color_count: int = LEGACY_DEFAULT_PRINTABLE_COLORS,
) -> PrintablePaletteRecommendation:
    """Recommend a provider-neutral palette without binding Orca filament slots."""

    prompt = instruction.strip() if isinstance(instruction, str) else ""
    if not prompt and image_path is None:
        raise OpenAIPreprocessorError("A text instruction or reference image is required.")
    try:
        roles = active_palette_roles(color_count)
    except PrintablePaletteError as exc:
        raise OpenAIPreprocessorError(str(exc)) from None
    style_direction = _style_profile(style, custom_style)
    palette_label = "four-color" if color_count == LEGACY_DEFAULT_PRINTABLE_COLORS else f"{color_count}-color"
    role_schema = "|".join(f'"{role}"' for role in roles)
    role_directions = {
        "primary": "primary should cover the largest semantic region",
        "structure": "structure should support silhouette and boundaries",
        "light": "light should provide a readable light material",
        "accent": "accent should distinguish one secondary semantic part",
        "secondary": "secondary should cover another broad semantic region",
        "detail": "detail should mark a small but still printable identifying region",
    }
    role_guidance = "; ".join(role_directions[role] for role in roles) + ". "
    contrast_guidance = (
        "The accent should normally use a clearly different hue family from primary, not a lighter or darker substitute for "
        "the same material; only keep related hues when the subject semantics make that distinction unmistakable. "
        if "accent" in roles else ""
    )
    value_guidance = (
        "Make structure visibly dark, light visibly bright, and keep primary and accent as medium-value colors from clearly "
        "different hue families so no two roles look interchangeable as physical materials. "
        if {"structure", "light", "accent"}.issubset(roles) else ""
    )
    portrait_guidance = (
        "For a real-person portrait, reserve light for the continuous skin material on face, ears, neck and visible hands; use "
        "primary for the largest garment or base material, structure for hair and deep boundaries, and accent for one secondary "
        "garment. Never assign skin to primary when a larger garment region is visible. "
        if {"structure", "light", "accent"}.issubset(roles) else ""
    )
    system_prompt = (
        "You are a color designer for printable 3D collectibles. Return exactly one JSON object and no markdown. "
        f"Recommend exactly one {palette_label} palette for the primary subject. Return exactly {color_count} color records. "
        "The colors are ideal design targets, not known "
        "physical filaments. Use this schema: "
        '{"summary":string,"colors":[{"hex":"#RRGGBB","name":string,'
        f'"role":{role_schema},"usage":string,"reason":string}}]}}. '
        "Return every listed role exactly once. Choose broad solid material regions with strong perceptual separation; avoid "
        "gradients, near-duplicate shades, tiny accents, transparency, metallic effects and colors that only work as lighting. "
        + role_guidance
        + contrast_guidance
        + "This full-color printable palette requirement "
        "overrides any monochrome stone, plaster, clay, metal, or grayscale wording in the selected style; keep the style's shape "
        "language while assigning visibly distinct material colors. "
        + value_guidance
        + portrait_guidance
        + "Use concise Chinese for summary, "
        "name, usage and reason. Apply this style direction: "
        + style_direction
    )
    user_content = prompt or "Analyze the primary visible subject in the reference image."
    if image_path is None:
        response = complete_text(system_prompt, user_content)
    else:
        response = complete_vision(system_prompt, user_content, (Path(image_path),))

    payload = _palette_recommendation_payload(response)
    summary = _bounded_recommendation_text(
        payload.get("summary"), "summary", MAX_PALETTE_RECOMMENDATION_SUMMARY_BYTES
    )
    raw_colors = payload.get("colors")
    if not isinstance(raw_colors, list) or len(raw_colors) != color_count:
        raise OpenAIPreprocessorError(
            f"The palette recommendation must contain exactly {color_count} colors."
        )

    records_by_role: dict[str, PrintablePaletteRecommendationColor] = {}
    raw_hex: list[str] = []
    for value in raw_colors:
        if not isinstance(value, dict):
            raise OpenAIPreprocessorError("Each palette recommendation color must be an object.")
        role = value.get("role")
        if not isinstance(role, str) or role not in roles or role in records_by_role:
            raise OpenAIPreprocessorError("The palette recommendation roles must be unique and complete.")
        raw_color = value.get("hex")
        if not isinstance(raw_color, str):
            raise OpenAIPreprocessorError("The palette recommendation color must use #RRGGBB format.")
        try:
            color = normalize_palette((raw_color,))[0]
        except PrintablePaletteError as exc:
            raise OpenAIPreprocessorError(str(exc)) from None
        raw_hex.append(color)
        records_by_role[role] = PrintablePaletteRecommendationColor(
            hex=color,
            name=_bounded_recommendation_text(
                value.get("name"), "color name", MAX_PALETTE_RECOMMENDATION_NAME_BYTES
            ),
            role=role,
            usage=_bounded_recommendation_text(
                value.get("usage"), "color usage", MAX_PALETTE_RECOMMENDATION_USAGE_BYTES
            ),
            reason=_bounded_recommendation_text(
                value.get("reason"), "color reason", MAX_PALETTE_RECOMMENDATION_REASON_BYTES
            ),
        )
    try:
        palette = normalize_palette(raw_hex)
        assignment = assign_palette_roles(palette, {role: records_by_role[role].hex for role in roles})
    except PrintablePaletteError as exc:
        raise OpenAIPreprocessorError(str(exc)) from None
    if len(palette) != color_count:
        raise OpenAIPreprocessorError("The palette recommendation colors must be unique.")
    if assignment.low_contrast:
        raise OpenAIPreprocessorError("The palette recommendation does not provide enough color contrast.")
    return PrintablePaletteRecommendation(summary, tuple(records_by_role[role] for role in roles))


def _multipart_image(
    path: Path,
    instruction: str,
    model: str,
    *,
    background: str | None = None,
) -> tuple[bytes, str]:
    mime_type, extension = _image_kind(path)
    try:
        if path.stat().st_size > _MAX_IMAGE_BYTES:
            raise OpenAIPreprocessorError("The input image exceeds the 20 MB limit.")
        image = path.read_bytes()
    except OpenAIPreprocessorError:
        raise
    except OSError:
        raise OpenAIPreprocessorError("The input image could not be read.") from None
    boundary = "----OrcaAI" + uuid.uuid4().hex
    chunks: list[bytes] = []

    def field(name: str, value: str) -> None:
        chunks.extend(
            [
                f"--{boundary}\r\n".encode("ascii"),
                f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode("ascii"),
                value.encode("utf-8"),
                b"\r\n",
            ]
        )

    field("model", model)
    field("prompt", instruction)
    field("quality", _image_quality())
    # Portrait edits are identity-sensitive.  The provider otherwise defaults
    # to a lower input fidelity and may replace a real face with a cleaner but
    # generic professional-portrait face even when the prompt explicitly locks
    # the landmarks.  GPT Image high fidelity spends more effort matching the
    # supplied image, especially facial features.
    field("input_fidelity", "high")
    # Let GPT Image 2 preserve the reference aspect ratio instead of forcing a
    # square canvas, which can subtly move portrait landmarks and body framing.
    field("size", "auto")
    if background is not None:
        field("background", background)
    chunks.extend(
        [
            f"--{boundary}\r\n".encode("ascii"),
            f'Content-Disposition: form-data; name="image"; filename="input.{extension}"\r\n'.encode("ascii"),
            f"Content-Type: {mime_type}\r\n\r\n".encode("ascii"),
            image,
            b"\r\n",
        ]
    )
    chunks.append(f"--{boundary}--\r\n".encode("ascii"))
    return b"".join(chunks), "multipart/form-data; boundary=" + boundary


def _portrait_face_lock_mask(source: Path, destination: Path) -> Path | None:
    """Locate a source face for portrait routing and quality-review crops."""

    try:
        from collections import deque
        from PIL import Image, ImageDraw, ImageFilter
    except ImportError:
        return None
    try:
        with Image.open(source) as opened:
            rgb = opened.convert("RGB")
    except (OSError, ValueError):
        return None
    scale = min(1.0, 384.0 / max(rgb.size))
    analysis = rgb.resize(
        (max(1, round(rgb.width * scale)), max(1, round(rgb.height * scale))),
        Image.Resampling.BILINEAR,
    ).convert("YCbCr")
    width, height = analysis.size
    skin = bytearray(width * height)
    for offset, (luma, blue_difference, red_difference) in enumerate(analysis.getdata()):
        if luma >= 45 and 76 <= blue_difference <= 135 and 134 <= red_difference <= 180:
            skin[offset] = 1
    visited = bytearray(len(skin))
    components: list[dict[str, float]] = []
    for seed, enabled in enumerate(skin):
        if not enabled or visited[seed]:
            continue
        visited[seed] = 1
        pending = deque([seed])
        area = 0
        sum_x = sum_y = 0
        left = right = seed % width
        top = bottom = seed // width
        while pending:
            offset = pending.popleft()
            area += 1
            x, y = offset % width, offset // width
            sum_x += x
            sum_y += y
            left, right = min(left, x), max(right, x)
            top, bottom = min(top, y), max(bottom, y)
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor >= 0 and skin[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    pending.append(neighbor)
        component_width = right - left + 1
        component_height = bottom - top + 1
        center_x = sum_x / area
        center_y = sum_y / area
        if (
            area >= width * height * 0.002
            and width * 0.25 <= center_x <= width * 0.75
            and center_y <= height * 0.42
            and width * 0.08 <= component_width <= width * 0.50
            and height * 0.06 <= component_height <= height * 0.58
        ):
            components.append({
                "area": area,
                "left": left,
                "right": right,
                "top": top,
                "bottom": bottom,
                "center_y": center_y,
            })
    if not components:
        return None
    face = max(components, key=lambda value: value["area"] - value["center_y"] * 0.15)
    inverse_scale = 1.0 / scale
    left = face["left"] * inverse_scale
    right = (face["right"] + 1) * inverse_scale
    top = face["top"] * inverse_scale
    face_width = max(1.0, right - left)
    left = max(0, left - face_width * 0.08)
    right = min(rgb.width, right + face_width * 0.08)
    top = max(0, top - face_width * 0.06)
    bottom = min(rgb.height, top + face_width * 1.45)
    alpha = Image.new("L", rgb.size, 0)
    ImageDraw.Draw(alpha).ellipse((round(left), round(top), round(right), round(bottom)), fill=255)
    alpha = alpha.filter(ImageFilter.GaussianBlur(max(3, round(face_width * 0.035))))
    mask = Image.new("RGBA", rgb.size, (0, 0, 0, 0))
    mask.putalpha(alpha)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".part")
    mask.save(temporary, format="PNG")
    os.replace(temporary, destination)
    return destination


def _validate_artifact_url(url: str) -> None:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme.lower() != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise OpenAIPreprocessorError("The preprocessing service returned an unsafe image location.")
    try:
        addresses = socket.getaddrinfo(parsed.hostname, parsed.port or 443, type=socket.SOCK_STREAM)
    except socket.gaierror:
        raise OpenAIPreprocessorError("The result image host could not be resolved.") from None
    if not addresses:
        raise OpenAIPreprocessorError("The result image host could not be resolved.")
    for address in addresses:
        try:
            ip = ipaddress.ip_address(address[4][0].split("%", 1)[0])
        except ValueError:
            raise OpenAIPreprocessorError("The preprocessing service returned an unsafe image location.") from None
        if not ip.is_global:
            raise OpenAIPreprocessorError("The preprocessing service returned an unsafe image location.")


class _SafeArtifactRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> Any:
        _validate_artifact_url(newurl)
        return urllib.request.Request(newurl, headers={"Accept": "image/*"}, method="GET")


def _atomic_write(path: Path, data: bytes) -> Path:
    if len(data) > _MAX_IMAGE_BYTES:
        raise OpenAIPreprocessorError("The result image exceeds the 20 MB limit.")
    part = path.with_name(path.name + ".part")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with part.open("wb") as stream:
            stream.write(data)
        os.replace(part, path)
    except OSError:
        try:
            part.unlink(missing_ok=True)
        except OSError:
            pass
        raise OpenAIPreprocessorError("The result image could not be written.") from None
    return path


def _download_image(url: str, output_path: Path) -> Path:
    started = time.monotonic()
    network = network_diagnostics(url)
    diagnostic_event(
        "provider.artifact_download.started",
        endpoint=safe_endpoint(url),
        timeout_seconds=_TIMEOUT_SECONDS,
        network=network,
    )
    part = output_path.with_name(output_path.name + ".part")
    request = urllib.request.Request(url, headers={"Accept": "image/*"}, method="GET")
    opener = build_network_opener(_SafeArtifactRedirects())
    for attempt in range(2):
        try:
            _validate_artifact_url(url)
            output_path.parent.mkdir(parents=True, exist_ok=True)
            with opener.open(request, timeout=_TIMEOUT_SECONDS) as response:
                content_length = response.headers.get("Content-Length")
                expected_size = int(content_length) if content_length and content_length.isdigit() else None
                if content_length:
                    try:
                        if int(content_length) > _MAX_IMAGE_BYTES:
                            raise OpenAIPreprocessorError("The result image exceeds the 20 MB limit.")
                    except ValueError:
                        pass
                total = 0
                with part.open("wb") as stream:
                    while chunk := response.read(min(64 * 1024, _MAX_IMAGE_BYTES + 1 - total)):
                        total += len(chunk)
                        if total > _MAX_IMAGE_BYTES:
                            raise OpenAIPreprocessorError("The result image exceeds the 20 MB limit.")
                        stream.write(chunk)
                if total == 0 or (expected_size is not None and total != expected_size):
                    raise http.client.IncompleteRead(b"", max(1, (expected_size or 1) - total))
            os.replace(part, output_path)
            diagnostic_event(
                "provider.artifact_download.completed",
                endpoint=safe_endpoint(url),
                response_bytes=total,
                elapsed_ms=round((time.monotonic() - started) * 1000),
                network=network,
            )
            return output_path
        except OpenAIPreprocessorError as exc:
            try:
                part.unlink(missing_ok=True)
            except OSError:
                pass
            diagnostic_event(
                "provider.artifact_download.failed",
                level="ERROR",
                endpoint=safe_endpoint(url),
                elapsed_ms=round((time.monotonic() - started) * 1000),
                network=network,
                exception_chain=exception_details(exc),
            )
            raise
        except urllib.error.HTTPError as exc:
            try:
                part.unlink(missing_ok=True)
            except OSError:
                pass
            if attempt == 0 and (exc.code == 429 or exc.code >= 500):
                diagnostic_event(
                    "provider.artifact_download.retry",
                    level="WARNING",
                    endpoint=safe_endpoint(url),
                    http_status=exc.code,
                    network=network,
                )
                continue
            failure = exc
            break
        except (urllib.error.URLError, TimeoutError, OSError, http.client.HTTPException) as exc:
            try:
                part.unlink(missing_ok=True)
            except OSError:
                pass
            if attempt == 0:
                diagnostic_event(
                    "provider.artifact_download.retry",
                    level="WARNING",
                    endpoint=safe_endpoint(url),
                    failure_kind=classify_connection_error(exc),
                    network=network,
                )
                continue
            failure = exc
            break
    diagnostic_event(
        "provider.artifact_download.failed",
        level="ERROR",
        endpoint=safe_endpoint(url),
        elapsed_ms=round((time.monotonic() - started) * 1000),
        network=network,
        failure_kind=classify_connection_error(failure),
        exception_chain=exception_details(failure),
    )
    raise OpenAIPreprocessorError(
        "The result image could not be downloaded.",
        code="image_download_failed",
        retryable=True,
        ambiguous=False,
    ) from None


def _provider_request(path: str, body: bytes, content_type: str) -> dict[str, Any]:
    base, key, _, _ = _config()
    # Text/vision analysis has no persistent generated artifact. One bounded
    # retry can recover a gateway/TLS interruption without creating a new image.
    # Image POSTs retain their one-attempt, ambiguous-result policy.
    for attempt in range(2):
        try:
            return _request_with_provider(
                path, body, content_type, base=base, key=key, provider_source="legacy_text"
            )
        except OpenAIPreprocessorError as exc:
            if path != "/chat/completions" or attempt != 0 or exc.code not in {
                "image_connection_failed", "image_service_unavailable"
            }:
                raise
            diagnostic_event("provider.analysis.retry", level="WARNING",
                             endpoint=safe_endpoint(base + path), failure_code=exc.code, attempt=2)
            time.sleep(1)


def _image_provider_request(path: str, body: bytes, content_type: str) -> dict[str, Any]:
    config = _image_config()
    return _request_with_provider(
        path,
        body,
        content_type,
        base=config.base_url,
        key=config.api_key,
        provider_source=config.source,
    )


def _style_preview_prompt(
    instruction: str,
    palette: tuple[str, ...],
    style: str = "sculpture",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
    geometry_reference: bool = False,
) -> str:
    canonical_style = LEGACY_STYLE_ALIASES.get(style, style)
    style_profile = (
        _style_profile(style, custom_style)
        if geometry_reference
        else _designer_toy_profile(style, custom_style) if palette else _style_profile(style, custom_style)
    )
    realistic_identity_lock = (
        "IDENTITY-FIRST PORTRAIT LOCK — highest priority when the source contains a real person: make the smallest "
        "possible face edit. Treat the source head and face as a locked geometric reference, not inspiration for a newly "
        "drawn attractive person. Keep the face bounding box relative to the shoulders, head angle, eye centers and eyelid "
        "openings, brow heights, nose tip and nostril width, mouth corners, tooth exposure, cheek outline, jaw corners, and "
        "chin endpoint aligned to the source at the same scale. Do not substitute a generic professional portrait, narrow or "
        "symmetrize the face, enlarge both eyes, or widen the smile. Preserve identity through modelable facial planes and "
        "relief, not a photographic face overlay. Render the face in the same coherent sculptural material and lighting as "
        "the head and body; surface simplification must not move, resize, or reshape identity landmarks. Before returning, compare the source "
        "and result face at equal size and correct any landmark drift. "
        if canonical_style in IDENTITY_FIRST_PORTRAIT_STYLES and (palette or geometry_reference) else ""
    )
    color_direction = (
        _designer_toy_palette_direction(
            palette,
            shadow_color,
            palette_roles,
            allow_meaningful_subset=canonical_style in PRINT_NATIVE_ARTISTIC_STYLES,
        )
        if palette
        else "Use coherent natural colors that fit the subject and selected style. Preserve useful tonal modeling with broad, "
        "contiguous color regions for shape readability. "
    )
    return (
        "Transform the supplied reference into a polished designer-ready style preview for later image-to-3D. "
        "The supplied source image is the authority for the primary subject's identity and recognizable structure. "
        + realistic_identity_lock
        + "User style and subject direction: "
        + instruction.strip()
        + "\nSelected style profile: "
        + style_profile
        + " "
        + _non_realistic_text_cleanup_direction(canonical_style)
        + "\nImage-to-3D composition contract: "
        + _image_to_3d_composition_direction(bool(palette), canonical_style)
        + (_solid_background_direction() if not palette else "")
        + "Treat the source as a closed visual inventory. Preserve its exact viewpoint (front, three-quarter, side, or rear), "
        "facing direction, left-right arrangement, silhouette, component count, negative spaces, and all identity-defining "
        "asymmetry. Never mirror the subject or substitute a more typical example of its category. "
        + "Preserve the chosen subject's recognizable identity, facial expression, hairstyle, signature clothing or structural "
        "features, and visible pose. Simplify fine hair strands, fingers, jewelry, fabric patterns, foliage-like texture, and shallow "
        "surface noise into a few sturdy, connected, modelable forms. Do not turn the chosen person, animal, statue, building, or "
        "object into a different subject. For a person, preserve the exact head angle and gaze direction, adult or child age, face "
        "aspect ratio, cheekbone placement, chin length, jaw contour, skin-tone relationships, hairline, curls, braids, facial hair, "
        "eyewear, headwear, visible hands, finger grouping, and hand-to-object contact; do not "
        "enlarge the eyes, shrink the nose or mouth, narrow the jaw, or replace the face with a generic doll face. For an animal, "
        "preserve its species or breed cues, ear shape and count, muzzle length, eye color, limb count, tail pose, and exact coat, "
        "feather, shell, or scale markings; do not invent a white muzzle, chest patch, socks, blaze, or spots that are absent from "
        "the source. For a product, vehicle, machine, or prop, preserve the exact number and relative placement of wheels, handles, "
        "openings, windows, lenses, dials, buttons, straps, tools, rods, and antennas; do not merge, duplicate, swap, or genericize "
        "them. For architecture or a statue, preserve tier and opening counts, gestures, symmetry or deliberate asymmetry, and any "
        "source-visible base; never invent a pedestal when none exists. Keep each meaningful thin support, spoke, cable, rail, branch, "
        "or antenna connected; if printability requires it, thicken it subtly instead of deleting or duplicating it. "
        "For a plant, bonsai, coral, antler, feather fan, or other branching organic subject, use fewer overlapping solid clusters "
        "and visibly fuse every cluster through sturdy branches or stems to the trunk, body, or base; do not leave contact-only "
        "shells or isolated leaf pads. "
        "Do not invent unseen anatomy; use the explicit bust treatment for cropped people instead. "
        + _portrait_identity_geometry_direction()
        + color_direction
        + (
            "This is the geometry reference. Preserve continuous tonal modeling and soft broad diffuse "
            "lighting so silhouette, facial landmarks, joints, folds, and sculptural planes remain legible. Preserve broad natural "
            "material groups together with their gradients, texture and fine color detail. Do not bake lighting or cast shadows into geometry. "
            if geometry_reference else ""
        )
        + "Avoid dithering and tiny color speckles. Do not return the unchanged source as a whole. Preserve a person's "
        "identity in the generated shape, not by pasting original photo pixels onto the face. Do not copy source-background "
        "shadows, gray halos, or photographic texture around the head into the generated reference."
    )


def build_style_preview_prompt(
    instruction: str,
    palette: tuple[str, ...],
    style: str = "sculpture",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
) -> str:
    """Return the exact provider prompt used for image-to-image previews.

    Benchmark and support tooling use this public boundary to persist an
    auditable request without duplicating the production prompt contract.
    """
    return _unrestricted_creation_prompt(instruction, style, custom_style, from_image=True)


def build_geometry_reference_prompt(
    instruction: str,
    style: str = "sculpture",
    custom_style: str = "",
) -> str:
    """Return the unrestricted creation prompt used by the production path."""
    return _unrestricted_creation_prompt(instruction, style, custom_style, from_image=True)


def _unrestricted_creation_prompt(
    instruction: str, style: str, custom_style: str, *, from_image: bool
) -> str:
    if not isinstance(instruction, str) or not instruction.strip():
        raise OpenAIPreprocessorError("An image-generation instruction is required.")
    # Keep the tested identity, composition and geometry rules. Only the color
    # policy changes; a new palette mode must not redesign the subject.
    prompt = (
        _style_preview_prompt(instruction, (), style, custom_style=custom_style, geometry_reference=True)
        if from_image else
        _text_image_prompt(instruction, (), style, custom_style=custom_style, geometry_reference=True)
    )
    portrait_surface = (
        " For a real-person subject, refine the surface conservatively while keeping the locked facial geometry. "
        "Reduce capture noise, isolated specular glare and transient skin blemishes; retain age cues, characteristic "
        "creases, moles, freckles and deliberate makeup. Do not smooth away the eyelid rim, nostril boundary, lip edge "
        "or the individual shape of the mouth. Keep lips, eyebrows, irises, sclera, hairline and clothing boundaries "
        "distinct at the intended reference-image scale without enlarging or outlining them. "
        "Use neutral white-balanced, broad diffuse illumination: enough gentle shading to read facial planes, with "
        "no colored rim light, hard cast shadow across the face, oily highlight or beauty-filter whitening. "
        "Keep the person's natural skin hue consistent across face, ears, neck and visible hands, including shaded "
        "areas; preserve natural local variation rather than making skin one flat swatch. Lighting darkness must not "
        "be interpreted as a different skin material, painted dirt, a deep wrinkle or a geometric hole. "
        "For multiple people, retain each person's own facial proportions and skin tone independently; never average "
        "their faces or swap their features. A user's explicitly requested lighting or intentional color remains controlling. "
        if LEGACY_STYLE_ALIASES.get(style, style) in IDENTITY_FIRST_PORTRAIT_STYLES else ""
    )
    return prompt + portrait_surface + (
        " Preserve natural colors, continuous gradients, texture, subtle skin tones and material detail. "
        "There is no printer color palette or color-count limit. Do not quantize, posterize, flatten colors, "
        "or impose solid-color regions for printing. A color explicitly requested by the user remains intentional. "
    ) + (
        "The selected monochrome sculpture style intentionally keeps its single-material appearance; preserve tonal shading within that style."
        if LEGACY_STYLE_ALIASES.get(style, style) == "sculpture" else ""
    )


def _text_image_prompt(
    instruction: str,
    palette: tuple[str, ...],
    style: str = "cartoon",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
    geometry_reference: bool = False,
) -> str:
    if not isinstance(instruction, str) or not instruction.strip():
        raise OpenAIPreprocessorError("An image-generation prompt is required.")
    canonical_style = LEGACY_STYLE_ALIASES.get(style, style)
    color_direction = (
        _designer_toy_palette_direction(
            palette,
            shadow_color,
            palette_roles,
            allow_meaningful_subset=canonical_style in PRINT_NATIVE_ARTISTIC_STYLES,
        )
        if palette
        else "Use coherent natural colors with broad, clean material regions. "
    )
    return (
        "Create one polished reference image for later image-to-3D generation. Subject: "
        + instruction.strip()
        + "\nSelected style profile: "
        + (
            _style_profile(style, custom_style)
            if geometry_reference
            else _designer_toy_profile(style, custom_style) if palette else _style_profile(style, custom_style)
        )
        + " "
        + _style_support_override(canonical_style)
        + _non_realistic_text_cleanup_direction(canonical_style)
        + "\nPrintable composition constraints: Use one clearly readable primary subject, a complete silhouette, a stable pose, "
        "simple depth layering, natural color transitions, and structurally meaningful details. "
        "Treat the user description as a closed component inventory: do not add plausible category features, accessories, handles, "
        "tools, rods, decorations, or secondary objects that were not explicitly requested. Simplify ambiguous details instead of inventing them. "
        + _portrait_display_base_direction()
        + _difficult_structure_direction()
        + _portrait_identity_geometry_direction()
        + ("Use a genuine alpha-transparent background with no cast shadow; never paint a checkerboard or grid to imitate transparency. " if palette else _solid_background_direction())
        + color_direction
        + ("Use palette colors only as solid semantic material regions, never as lighting highlights, reflections, rim light, or shading bands. " if palette else "")
        + (
            "Preserve continuous tonal modeling with soft broad diffuse lighting. Use broad natural material groups while keeping their "
            "ownership clear across lit and shaded surfaces. Preserve gradients, texture and fine color detail. Do not bake cast shadows "
            "into geometry or add blur, watermark, frame, or decorative clutter. This is a geometry reference."
            if geometry_reference
            else "Do not use gradients, semi-transparent subject materials, soft shadows, photographic reflections, depth of field, blur, dithering, "
            "halftone dots, random noise, tiny isolated regions, dense texture, text, watermark, frame, or decorative clutter. "
            "The deterministic print pipeline will enforce the exact palette, so prioritize shape readability over tonal realism."
        )
    )


def build_text_image_prompt(
    instruction: str,
    palette: tuple[str, ...],
    style: str = "cartoon",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
) -> str:
    """Return the exact provider prompt used for text-to-image references.

    Quality benchmarks persist this public prompt boundary so text and image
    inputs have the same auditable, hash-frozen paid-call semantics.
    """
    return _unrestricted_creation_prompt(instruction, style, custom_style, from_image=False)


def build_text_geometry_reference_prompt(
    instruction: str,
    style: str = "cartoon",
    custom_style: str = "",
) -> str:
    """Return the unrestricted prompt used by text-to-image creation."""
    return _unrestricted_creation_prompt(instruction, style, custom_style, from_image=False)


def _save_provider_image(result: dict[str, Any], destination: Path) -> Path:
    data = result.get("data")
    item = data[0] if isinstance(data, list) and data and isinstance(data[0], dict) else None
    if not item:
        raise OpenAIPreprocessorError("The preprocessing service returned no image.")
    encoded = item.get("b64_json")
    if isinstance(encoded, str) and encoded:
        try:
            image = base64.b64decode(encoded, validate=True)
        except (ValueError, binascii.Error):
            raise OpenAIPreprocessorError("The preprocessing service returned invalid image data.") from None
        return _atomic_write(destination, image)
    url = item.get("url")
    if isinstance(url, str) and url:
        return _download_image(url, destination)
    raise OpenAIPreprocessorError("The preprocessing service returned no usable image.")


def generate_image(
    instruction: str,
    output_path: str | os.PathLike[str],
    palette: tuple[str, ...],
    style: str = "cartoon",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
) -> Path:
    destination = Path(output_path)
    model = _image_config().model
    payload = json.dumps(
        {
            "model": model,
            "prompt": build_text_image_prompt(
                instruction, palette, style, shadow_color, palette_roles, custom_style
            ),
            "background": "opaque",
            "size": "1024x1024",
            "quality": _image_quality(),
            "n": 1,
            "response_format": "b64_json",
        },
        ensure_ascii=False,
    ).encode("utf-8")
    return _save_provider_image(
        _image_provider_request("/images/generations", payload, "application/json"), destination
    )


def _design_material_direction(palette: tuple[str, ...], palette_roles: Mapping[str, str] | None) -> str:
    if not palette:
        return ""
    try:
        roles = assign_palette_roles(palette, palette_roles).color_by_role
    except PrintablePaletteError as exc:
        raise OpenAIPreprocessorError(str(exc)) from None
    return (
        "\nDesign material colors: " + ", ".join(palette) + ". Semantic material roles: "
        + ", ".join(f"{role}: {color}" for role, color in roles.items())
        + ". Use these hues for broad, coherent material regions while preserving the selected style and clear shape. "
        "These are material colors, not an exact pixel palette: preserve continuous diffuse shading and geometric detail. "
        "Never posterize, flatten the relief, or add color swatches, labels, palette grids, or a checkerboard. "
        "With one color, render the entire subject in that single material."
    )


def generate_geometry_reference_image(
    instruction: str,
    output_path: str | os.PathLike[str],
    style: str = "cartoon",
    custom_style: str = "",
    *,
    palette: tuple[str, ...] = (),
    palette_roles: Mapping[str, str] | None = None,
) -> Path:
    destination = Path(output_path)
    payload = json.dumps(
        {
            "model": _image_config().model,
            "prompt": _unrestricted_creation_prompt(instruction, style, custom_style, from_image=False),
            "background": "opaque",
            "size": "1024x1024",
            "quality": _image_quality(),
            "n": 1,
            "response_format": "b64_json",
        },
        ensure_ascii=False,
    ).encode("utf-8")
    return _save_provider_image(
        _image_provider_request("/images/generations", payload, "application/json"), destination
    )


def preprocess_image(
    input_path: str | os.PathLike[str],
    instruction: str,
    output_path: str | os.PathLike[str],
    palette: tuple[str, ...],
    style: str = "sculpture",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
    geometry_output_path: str | os.PathLike[str] | None = None,
) -> Path:
    if not isinstance(instruction, str) or not instruction.strip():
        raise OpenAIPreprocessorError("An image-edit instruction is required.")
    canonical_style = LEGACY_STYLE_ALIASES.get(style, style)
    result = edit_image(
        input_path,
        build_geometry_reference_prompt(instruction, style, custom_style),
        output_path,
        # Match the solid-backdrop prompt at the transport boundary so a
        # compatible provider is never asked to simulate transparency.
        background="opaque",
    )
    if canonical_style in IDENTITY_FIRST_PORTRAIT_STYLES:
        # Retain detection evidence for portrait routing/crops, not compositing.
        # A source-space oval cannot align a newly generated head and can copy
        # photographic background into the geometry reference as a gray halo.
        _portrait_face_lock_mask(
            Path(input_path), Path(output_path).with_name(PORTRAIT_FACE_LOCK_FILENAME)
        )
        # A user-confirmed preview action may issue at most one billed Image2
        # request. Keep its pixels intact and reuse that result for geometry;
        # source likeness belongs in generated shape, not a photo overlay.
        if geometry_output_path is not None:
            geometry_output = Path(geometry_output_path)
            try:
                geometry_output.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(result, geometry_output)
            except OSError:
                raise OpenAIPreprocessorError(
                    "The sculptural portrait reference could not be saved."
                ) from None
    return result


def edit_image(
    input_path: str | os.PathLike[str],
    prompt: str,
    output_path: str | os.PathLike[str],
    *,
    background: str | None = None,
) -> Path:
    """Edit one image with an exact caller-owned prompt.

    Domain modules such as multiview preparation own their prompt contract while
    this adapter remains responsible only for the OpenAI-compatible transport.
    """
    if not isinstance(prompt, str) or not prompt.strip():
        raise OpenAIPreprocessorError("An image-edit prompt is required.")
    source = Path(input_path)
    destination = Path(output_path)
    model = _image_config().model
    body, content_type = _multipart_image(source, prompt.strip(), model, background=background)
    result = _image_provider_request("/images/edits", body, content_type)
    return _save_provider_image(result, destination)

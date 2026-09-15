#!/usr/bin/env python3
from __future__ import annotations

import atexit
from array import array
from collections import Counter, deque
from io import BytesIO
import hmac
import math
import json
import os
import re
import secrets
import shutil
import socket
import ssl
import stat
import sys
import threading
import time
import traceback
import uuid
import zipfile
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import asdict, dataclass, field
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, BinaryIO, Callable, Mapping

from ai_diagnostics import diagnostic_context, event as diagnostic_event, exception_details, safe_endpoint
from color_intent import (
    COLOR_INTENT_FILENAME,
    MAX_MANIFEST_BYTES as MAX_COLOR_INTENT_BYTES,
    SCHEMA_ID as COLOR_INTENT_SCHEMA,
    ColorIntentError,
    verify_color_intent_manifest_file,
    write_color_intent_manifest,
)
from network_policy import network_diagnostics
from glb_artifact import Glb, GlbError, prepare_generated_glb, write_analysis_obj

from openai_preprocessor import (
    IDENTITY_FIRST_PORTRAIT_STYLES,
    OpenAIPreprocessorError,
    complete_vision,
    complete_text,
    edit_image,
    generate_geometry_reference_image,
    image_provider_status,
    PORTRAIT_FACE_LOCK_FILENAME,
    preprocess_image,
    recommend_printable_palette,
)
from printable_image_pipeline import (
    PrintSettings,
    PrintableImageError,
    _portrait_source_subject_mask,
    process_printable_image,
)
from printable_model_views import (
    ModelViewError,
    ModelViewSettings,
    render_model_views,
)
from printable_multiview_reference import (
    HIGH_QUALITY_PORTRAIT_CANVAS_SIZE,
    MULTIVIEW_NORMALIZATION_VERSION,
    MultiviewReferenceError,
    PORTRAIT_MATERIAL_GATE_VERSION,
    VIEW_ORDER as MULTIVIEW_ORDER,
    build_multiview_input_sheet,
    evaluate_multiview_review_acceptance,
    evaluate_portrait_material_gate,
    normalize_multiview_inputs,
    process_multiview_crops,
    review_multiview_sheet,
    split_multiview_sheet,
    write_multiview_manifest,
)
from portrait_multiview_cleanup import (
    PortraitProjectionError,
    project_front_portrait_materials,
    project_geometry_aligned_portrait_materials,
    quantize_geometry_aligned_material_reference,
)
from model_input_image_quality import (
    ModelInputImageQualityError,
    assess_model_input_image,
    recommend_printable_style,
)
from model_job_support import (
    apply_legacy_material_fragmentation_gate as _apply_legacy_material_fragmentation_gate,
    assess_job_model_reference as _assess_job_model_reference,
    file_info as _file_info,
    generation_prompt as _generation_prompt,
    image_type as _image_type,
    model_input_quality_message as _model_input_quality_message,
    preprocess_failure_payload,
    printable_preview_message as _printable_preview_message,
    stored_image_type as _stored_image_type,
)
from model_provider_gateway import (
    ModelProviderGateway,
    ModelTaskRequest,
    PaidTaskAuthorization,
    ProviderGatewayError,
    TextureTaskRequest,
    provider_policy,
)
from model_refinement import build_model_refinement_advice
from hunyuan_provider_gateway import HunyuanModelProviderGateway, validate_options as validate_hunyuan_options
from printable_model_quality import (
    GATE_VERSION as MODEL_QUALITY_GATE_VERSION,
    ModelQualityError,
    ModelQualityThresholds,
    analyze_printable_obj,
    write_model_quality_report,
)
from printable_visual_quality import REPORT_FILENAME as VISUAL_QUALITY_FILENAME, review_model_visual_quality
from printable_reference_visual_quality import review_prepared_reference
from printable_palette import (
    LEGACY_DEFAULT_PRINTABLE_COLORS,
    MAX_PRINTABLE_COLORS,
    MIN_PRINTABLE_COLORS,
    PrintablePaletteError,
    active_palette_roles,
    assign_palette_roles,
    normalize_palette_color_count,
)
from tripo_client import TripoError, validate_generation_option_values, validate_generation_options

_MODEL_PROVIDER_GATEWAY = ModelProviderGateway()
_HUNYUAN_PROVIDER_GATEWAY = HunyuanModelProviderGateway()


def _model_gateway(provider: str):
    return _HUNYUAN_PROVIDER_GATEWAY if provider == "hunyuan" else _MODEL_PROVIDER_GATEWAY

HOST = os.environ.get("ORCASLICER_AI_SIDECAR_HOST", "127.0.0.1")
PORT = int(os.environ.get("ORCASLICER_AI_SIDECAR_PORT", "18764"))
SIDECAR_VERSION = "orcaslicer-ai-sidecar-v9"
SIDECAR_INSTANCE_ID = str(uuid.uuid4())
SIDECAR_SESSION_NONCE = secrets.token_hex(32)
MAX_REQUEST_BYTES = 256 * 1024
MAX_CHANGES = 8
MAX_PROMPT_BYTES = 2000
MAX_CUSTOM_STYLE_BYTES = 1000
MAX_IMAGE_BYTES = 20 * 1024 * 1024
MAX_MULTIPART_BYTES = MAX_IMAGE_BYTES + 256 * 1024
MIN_SOURCE_IMAGE_EDGE = 64
MIN_MODEL_REFERENCE_EDGE = 256
MAX_ARTIFACT_BYTES = 768 * 1024 * 1024
MAX_ARCHIVE_FILES = 128
MAX_UNPACKED_BYTES = 1024 * 1024 * 1024
MAX_TEXTURE_PIXELS = 64 * 1024 * 1024
MAX_PALETTE_COLORS = MAX_PRINTABLE_COLORS
MIN_PALETTE_COLORS = MIN_PRINTABLE_COLORS
DEFAULT_PALETTE_COLORS = LEGACY_DEFAULT_PRINTABLE_COLORS
MAX_MODEL_FACES = 2000000
MIN_MODEL_FACE_RATIO = 0.90
MAX_MODEL_FACE_RATIO = 1.25
MODEL_FACE_LIMITS = (100000, 300000, 500000, 1000000, 2000000)
DEFAULT_MODEL_FACE_LIMIT = 300000
GENERATION_PROFILES = ("quality", "performance")
DEFAULT_GENERATION_PROFILE = "quality"
GENERATION_PROFILE_FACE_LIMITS = {"quality": 1000000, "performance": 300000}
MAX_GENERATION_ATTEMPTS = 1
JOB_STATE_FILENAME = "job.json"
JOB_STATE_VERSION = 1
MAX_JOB_STATE_BYTES = 64 * 1024
JOURNEY_EVENT_FILENAME = "journey-events.jsonl"
MAX_JOURNEY_EVENT_FILE_BYTES = 5 * 1024 * 1024
JOURNEY_EVENT_NAMES = frozenset({
    "preview_requested",
    "preview_ready",
    "preview_failed",
    "preview_regenerated",
    "preview_accepted",
    "model_submitted",
    "model_ready",
    "model_failed",
    "model_imported",
    "slice_requested",
    "print_feedback_success",
    "print_feedback_issue",
})
MAX_LOCAL_REPAIR_DIAGONAL_RATIO = 0.05
MAX_LOCAL_REPAIR_FACE_RATIO = 0.01
MAX_LOCAL_BOUNDARY_EDGES = 64
MAX_NOISE_COMPONENT_FACE_RATIO = 0.0001
MAX_NOISE_COMPONENT_DIAGONAL_RATIO = 0.01
MAX_TINY_COLOR_COMPONENT_AREA_RATIO = 0.0001
MAX_TINY_COLOR_COMPONENT_VERTEX_RATIO = 0.0005
MAX_COLOR_CLEANUP_SOURCE_AREA_RATIO = 0.10
MAX_COLOR_CLEANUP_SURFACE_AREA_RATIO = 0.005
MEANINGFUL_COLOR_SURFACE_AREA_RATIO = 0.02
MAX_COLOR_CLEANUP_PASSES = 2
MAX_COLOR_BOUNDARY_SURFACE_AREA_RATIO = 0.0025
MAX_COLOR_BOUNDARY_SOURCE_AREA_RATIO = 0.02
MIN_COLOR_BOUNDARY_SUPPORT_RATIO = 1.25
MAX_COLOR_BOUNDARY_SOURCE_NEIGHBORS = 1
MAX_COLOR_BOUNDARY_PASSES = 2
PORTRAIT_GARMENT_SMOOTHING_PASSES = 4
PORTRAIT_HAND_BOUNDARY_PASSES = 4
PORTRAIT_HAND_BOUNDARY_MAX_REMOVAL_RATIO = 0.25
PORTRAIT_HAND_BOUNDARY_MIN_PRIMARY_SUPPORT = 2
PORTRAIT_HAND_COMPACT_EXTENT_RATIO = 0.45
PORTRAIT_HAND_DIFFUSE_SIZE_RATIO = 2.5
PORTRAIT_HAND_MIN_HEIGHT_RATIO = 0.30
PORTRAIT_REAR_GARMENT_HEIGHT_RATIO = 0.70
PORTRAIT_REAR_HAIR_HEIGHT_RATIO = 0.58
PORTRAIT_FRONT_SURFACE_QUANTILE = 0.50
PORTRAIT_STRUCTURE_FRONT_QUANTILE = 0.60
PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO = 0.74
PORTRAIT_FACE_DETAIL_MAX_HEIGHT_RATIO = 0.94
PORTRAIT_FACE_DETAIL_HALF_WIDTH_RATIO = 0.23
PORTRAIT_FACE_DETAIL_SURFACE_TOLERANCE_MM = 0.35
PORTRAIT_FACE_DETAIL_GRID_SIZE = 256
PORTRAIT_GEOMETRY_PROVIDER_FILENAME = "geometry-provider-reference.png"
PORTRAIT_HEAD_PREVIEW_FILENAME = "portrait-head-shoulders-preview.png"
PORTRAIT_GEOMETRY_MAX_SUBJECT_OCCUPANCY = 0.88
PORTRAIT_HEAD_GEOMETRY_MAX_SUBJECT_OCCUPANCY = 0.96
PORTRAIT_REAR_PLATE_MIN_RUN_RATIO = 0.60
PORTRAIT_REAR_PLATE_MAX_START_RATIO = 0.15
DEFAULT_MODEL_SIZE_MM = 100.0
MODEL_ARTIFACT_FORMAT = "glb"
MODEL_QUALITY_FILENAME = "model-quality.json"
STYLE_IDS = (
    "sculpture", "realistic", "portrait_sketch", "cartoon", "low_poly", "relief", "ink_relief", "diorama", "custom",
)
LEGACY_STYLE_ALIASES = {
    "q_cartoon": "cartoon",
    "cel_shaded": "cartoon",
    "enamel_inlay": "realistic",
}
DEFAULT_IMAGE_INSTRUCTION = (
    "Stylize only the content already visible in the reference image. Preserve the exact crop, framing, visible regions, "
    "occlusions, subjects, objects, and background; do not add, remove, reveal, reconstruct, or extend anything."
)
_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}
_JOBS_LOCK = threading.RLock()
_GEOMETRY_REFERENCE_LOCK = threading.RLock()
_JOBS: dict[str, "Job"] = {}
_JOURNEY_EVENT_LOCK = threading.Lock()
_DESIGN_EXECUTOR = ThreadPoolExecutor(max_workers=1, thread_name_prefix="orca-design-job")
_MODEL_EXECUTOR = ThreadPoolExecutor(max_workers=1, thread_name_prefix="orca-model-job")
_SHUTDOWN_LOCK = threading.Lock()
_SHUT_DOWN = False


def _environment_flag(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {"1", "true", "yes", "on"}


def _preprocess_fallback_enabled() -> bool:
    return _environment_flag("ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK")


def _runtime_network_metadata() -> dict[str, dict[str, object]]:
    image_endpoint = (
        os.environ.get("OPENAI_PRO_URL", "").strip()
        or os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
    )
    return {
        "openai": network_diagnostics(
            os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
        ),
        "image2": network_diagnostics(image_endpoint),
        "tripo": network_diagnostics(
            os.environ.get("TRIPO_API_BASE", "https://openapi.tripo3d.com/v3")
        ),
    }


def _configured_session_token() -> str | None:
    value = os.environ.get("ORCASLICER_AI_SESSION_TOKEN", "")
    if not value:
        return ""
    return value if re.fullmatch(r"[0-9A-Fa-f]{64}", value) else None


def _session_required() -> bool:
    return (
        _environment_flag("ORCASLICER_AI_REQUIRE_SESSION")
        or os.environ.get("ORCASLICER_AI_CONFIG_MODE") == "internal_locked"
        or os.environ.get("ORCASLICER_AI_DISTRIBUTION_CHANNEL") in {"internal", "commercial"}
    )


def _session_hmac(token: str, message: str) -> str:
    return hmac.new(token.encode("ascii"), message.encode("ascii"), "sha256").hexdigest()


def _safe_runtime_identity() -> dict[str, str]:
    version = os.environ.get("ORCASLICER_AI_APP_VERSION", "unknown")
    if not re.fullmatch(r"[0-9A-Za-z._+-]{1,128}", version):
        version = "unknown"
    commit = os.environ.get("ORCASLICER_AI_APP_COMMIT", "unknown")
    if commit != "unknown" and not re.fullmatch(r"[0-9A-Fa-f]{40}", commit):
        commit = "unknown"
    revision = os.environ.get("ORCASLICER_AI_PACKAGE_REVISION", "unknown")
    if not re.fullmatch(r"[0-9A-Za-z._-]{1,128}", revision):
        revision = "unknown"
    channel = os.environ.get("ORCASLICER_AI_DISTRIBUTION_CHANNEL", "developer")
    if channel not in {"developer", "internal", "commercial"}:
        channel = "developer"
    return {
        "application_version": version,
        "application_commit": commit,
        "package_revision": revision,
        "distribution_channel": channel,
    }


def _configured_parent_pid() -> int | None:
    value = os.environ.get("ORCASLICER_AI_PARENT_PID", "").strip()
    if not value:
        return None
    if not value.isascii() or not value.isdecimal():
        return None
    parent_pid = int(value)
    return parent_pid if 0 < parent_pid <= 0xFFFFFFFF else None


def _parent_process_alive(parent_pid: int) -> bool:
    if os.name == "nt":
        # os.kill(pid, 0) is not a harmless existence probe on Windows. Query a
        # synchronize-only process handle and never inherit it into children.
        import ctypes

        synchronize = 0x00100000
        wait_timeout = 0x00000102
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        kernel32.WaitForSingleObject.restype = ctypes.c_uint32
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        kernel32.CloseHandle.restype = ctypes.c_int
        handle = kernel32.OpenProcess(synchronize, False, parent_pid)
        if not handle:
            return False
        try:
            return kernel32.WaitForSingleObject(handle, 0) == wait_timeout
        finally:
            kernel32.CloseHandle(handle)

    try:
        os.kill(parent_pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _open_parent_process_handle(parent_pid: int) -> int | None:
    if os.name != "nt":
        return None
    import ctypes

    synchronize = 0x00100000
    wait_timeout = 0x00000102
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    handle = kernel32.OpenProcess(synchronize, False, parent_pid)
    if not handle:
        return None
    if kernel32.WaitForSingleObject(handle, 0) != wait_timeout:
        kernel32.CloseHandle(handle)
        return None
    return int(handle)


def _close_parent_process_handle(handle: int | None) -> None:
    if os.name != "nt" or handle is None:
        return
    import ctypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    kernel32.CloseHandle(handle)


def _parent_process_handle_alive(handle: int) -> bool:
    import ctypes

    wait_timeout = 0x00000102
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    return kernel32.WaitForSingleObject(handle, 0) == wait_timeout


def _monitor_parent(
    server: ThreadingHTTPServer,
    parent_pid: int,
    parent_handle: int | None = None,
) -> None:
    if os.name == "nt":
        # Keep one synchronize-only handle for the whole Sidecar lifetime. A
        # handle continues to identify the original Orca process even if its PID
        # is later reused by Windows.
        import ctypes

        synchronize = 0x00100000
        wait_object_0 = 0x00000000
        wait_timeout = 0x00000102
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        kernel32.WaitForSingleObject.restype = ctypes.c_uint32
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        kernel32.CloseHandle.restype = ctypes.c_int
        handle = parent_handle or kernel32.OpenProcess(synchronize, False, parent_pid)
        if not handle:
            diagnostic_event("sidecar.parent.unavailable", level="ERROR", parent_pid=parent_pid)
            server.shutdown()
            return
        try:
            while not _SHUT_DOWN:
                result = kernel32.WaitForSingleObject(handle, 2000)
                if result == wait_timeout:
                    continue
                event = "sidecar.parent.exited" if result == wait_object_0 else "sidecar.parent.wait_failed"
                diagnostic_event(event, level="INFO" if result == wait_object_0 else "ERROR", parent_pid=parent_pid)
                server.shutdown()
                return
        finally:
            kernel32.CloseHandle(handle)
        return

    while not _SHUT_DOWN:
        if not _parent_process_alive(parent_pid):
            diagnostic_event("sidecar.parent.exited", parent_pid=parent_pid)
            server.shutdown()
            return
        time.sleep(2.0)


@dataclass
class Job:
    id: str
    source: str
    directory: Path
    state: str = "preprocessing"
    phase: str = "preprocessing"
    message: str = "Preparing model generation request."
    progress: int = 5
    palette: tuple[str, ...] = field(default_factory=tuple)
    palette_roles: dict[str, str] = field(default_factory=dict)
    palette_color_count: int = DEFAULT_PALETTE_COLORS
    print_settings: dict[str, Any] = field(default_factory=lambda: asdict(PrintSettings()))
    style: str = "sculpture"
    custom_style: str = ""
    face_limit: int = DEFAULT_MODEL_FACE_LIMIT
    generation_profile: str = DEFAULT_GENERATION_PROFILE
    geometry_quality: str | None = None
    texture_quality: str = "standard"
    output_format: str = "glb"
    provider: str = "tripo"
    user_prompt: str = ""
    prepared_prompt: str = ""
    input_path: Path | None = None
    raw_preview_path: Path | None = None
    strict_preview_path: Path | None = None
    preview_path: Path | None = None
    model_reference_path: Path | None = None
    geometry_reference_path: Path | None = None
    preview_content_type: str = ""
    heatmap_path: Path | None = None
    metadata_path: Path | None = None
    background_mask_path: Path | None = None
    subject_mask_path: Path | None = None
    mask_paths: dict[str, Path] = field(default_factory=dict)
    image_metrics: dict[str, Any] = field(default_factory=dict)
    preprocess_failure: dict[str, Any] = field(default_factory=dict)
    artifact_path: Path | None = None
    artifact_format: str = ""
    color_intent_path: Path | None = None
    color_intent_schema: str = ""
    color_intent_sha256: str = ""
    palette_recommendation: dict[str, Any] = field(default_factory=dict)
    palette_recommendation_confirmed: bool = False
    generate_image: bool = False
    attempts: list[dict[str, Any]] = field(default_factory=list)
    updated_at: float = field(default_factory=time.time)
    stop_event: threading.Event = field(default_factory=threading.Event, repr=False)
    delete_requested: bool = field(default=False, repr=False)
    future: Future[Any] | None = field(default=None, repr=False)


class RequestError(Exception):
    def __init__(self, code: str, message: str, status: int, retryable: bool = False):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status
        self.retryable = retryable


class JobStopped(Exception):
    pass


class SidecarRestart(Exception):
    """Stops local work while keeping a paid remote task resumable."""

    pass


class PortraitMultiviewPreparationError(TripoError):
    """A recoverable, pre-paid portrait view preparation failure."""

    pass


class PortraitGeometryGateError(TripoError):
    """A paid portrait mesh has visually invalid large-scale geometry."""

    pass


def extract_allowed_keys(request: dict[str, Any]) -> dict[str, set[str]]:
    allowed: dict[str, set[str]] = {}
    scopes = request.get("allowed_changes", {}).get("scopes", {})
    if not isinstance(scopes, dict):
        return allowed

    for scope, scope_def in scopes.items():
        if scope not in ("print", "filament") or not isinstance(scope_def, dict):
            continue
        keys = scope_def.get("keys", {})
        if isinstance(keys, dict):
            allowed[scope] = {str(key) for key in keys}
    return allowed


def build_system_prompt(request: dict[str, Any]) -> str:
    allowed_changes = request.get("allowed_changes", {})
    guidance = request.get("optimization_guidance", [])
    return (
        "You are a conservative OrcaSlicer print-parameter proposal engine. "
        "Return exactly one JSON object and no markdown or text outside it. "
        "Use this schema: "
        '{"summary":string,"changes":[{"scope":"print"|"filament",'
        '"key":string,"new_value":string|number|boolean,"reason":string}],'
        '"questions":[string]}. '
        f"Return at most {MAX_CHANGES} changes. Only use scope/key pairs present in allowed_changes. "
        "Treat current config values as authoritative. Do not return unchanged values. "
        "Never propose printer or machine geometry, nozzle or bed changes, firmware, custom G-code, "
        "network or host settings, credentials, paths, file operations, profile writes, or commands. "
        "When available information is insufficient for a safe parameter change, explain that in "
        "summary or questions instead of guessing. Use Chinese for summary, reason, and questions.\n\n"
        "allowed_changes:\n"
        + json.dumps(allowed_changes, ensure_ascii=False, separators=(",", ":"))
        + "\n\noptimization_guidance:\n"
        + json.dumps(guidance, ensure_ascii=False, separators=(",", ":"))
    )


def build_user_payload(request: dict[str, Any]) -> dict[str, Any]:
    return {
        "request_id": request.get("request_id", ""),
        "user_message": request.get("user_message", ""),
        "model": request.get("model", {}),
        "config": request.get("config", {}),
    }


def provider_request(request: dict[str, Any]) -> dict[str, Any]:
    try:
        content = complete_text(
            build_system_prompt(request),
            json.dumps(build_user_payload(request), ensure_ascii=False, separators=(",", ":")),
        )
    except OpenAIPreprocessorError as exc:
        raise RuntimeError(str(exc)) from None
    return extract_json_object(content)


def extract_json_object(text: str) -> dict[str, Any]:
    stripped = text.strip()
    if stripped.startswith("```"):
        stripped = re.sub(r"^```(?:json)?\s*", "", stripped, flags=re.IGNORECASE)
        stripped = re.sub(r"\s*```$", "", stripped)

    try:
        parsed = json.loads(stripped)
    except json.JSONDecodeError:
        start = stripped.find("{")
        end = stripped.rfind("}")
        if start < 0 or end <= start:
            raise RuntimeError("The AI service response did not contain a JSON object")
        try:
            parsed = json.loads(stripped[start : end + 1])
        except json.JSONDecodeError as exc:
            raise RuntimeError("The AI service response contained invalid JSON") from exc

    if not isinstance(parsed, dict):
        raise RuntimeError("The AI service response was not a JSON object")
    return parsed


def normalize_proposal(raw: dict[str, Any], request: dict[str, Any]) -> dict[str, Any]:
    allowed = extract_allowed_keys(request)
    normalized: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    changes = raw.get("changes", [])

    if isinstance(changes, list):
        for change in changes:
            if not isinstance(change, dict):
                continue
            scope = str(change.get("scope", ""))
            key = str(change.get("key", ""))
            identity = (scope, key)
            if key not in allowed.get(scope, set()) or identity in seen:
                continue
            value = change.get("new_value", change.get("value"))
            if not isinstance(value, (str, int, float, bool)) or value is None:
                continue
            normalized.append(
                {
                    "scope": scope,
                    "key": key,
                    "new_value": value,
                    "reason": str(change.get("reason", "")),
                }
            )
            seen.add(identity)
            if len(normalized) >= MAX_CHANGES:
                break

    summary = raw.get("summary", "")
    questions = raw.get("questions", [])
    assistant_parts = [str(summary).strip()] if str(summary).strip() else []
    if isinstance(questions, list):
        assistant_parts.extend(str(question).strip() for question in questions if str(question).strip())
    if not assistant_parts:
        assistant_parts.append("AI service did not return a displayable explanation.")

    return {
        "request_id": str(request.get("request_id", "")),
        "assistant_text": "\n".join(assistant_parts),
        "proposal": {"changes": normalized},
    }


def _text_field(value: Any, name: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        raise RequestError("invalid_request", f"{name} must be a string.", 400)
    value = value.strip()
    if not allow_empty and not value:
        raise RequestError("invalid_request", f"{name} is required.", 400)
    if len(value.encode("utf-8")) > MAX_PROMPT_BYTES:
        raise RequestError("invalid_request", f"{name} exceeds the 2000-byte limit.", 400)
    return value


def _boolean_field(value: Any, name: str, *, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, str) and value.strip().lower() in {"true", "false"}:
        return value.strip().lower() == "true"
    raise RequestError("invalid_request", f"{name} must be a boolean.", 400)


def _normalize_palette(value: Any) -> tuple[str, ...]:
    if not isinstance(value, list) or len(value) > MAX_PALETTE_COLORS:
        raise RequestError(
            "invalid_palette",
            f"palette must contain between 0 and {MAX_PALETTE_COLORS} colors.",
            400,
        )
    normalized: list[str] = []
    seen: set[str] = set()
    for color in value:
        if not isinstance(color, str) or re.fullmatch(r"#[0-9A-Fa-f]{6}", color) is None:
            raise RequestError("invalid_palette", "palette colors must use #RRGGBB format.", 400)
        canonical = color.upper()
        if canonical not in seen:
            seen.add(canonical)
            normalized.append(canonical)
    return tuple(normalized)


def _normalize_palette_color_count(value: Any) -> int:
    try:
        return normalize_palette_color_count(value)
    except PrintablePaletteError as exc:
        raise RequestError("invalid_palette_color_count", str(exc), 400) from None


def _multipart_palette(value: Any) -> tuple[str, ...]:
    if not isinstance(value, str):
        raise RequestError("invalid_palette", "palette is required.", 400)
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError:
        raise RequestError("invalid_palette", "palette must be a JSON color array.", 400) from None
    return _normalize_palette(parsed)


def _normalize_palette_roles(value: Any, palette: tuple[str, ...]) -> dict[str, str]:
    if not palette:
        if value in (None, {}):
            return {}
        raise RequestError("invalid_palette_roles", "palette roles require printable colors.", 400)
    if value is None:
        value = {}
    if not isinstance(value, dict) or any(not isinstance(key, str) or not isinstance(color, str) for key, color in value.items()):
        raise RequestError("invalid_palette_roles", "palette_roles must be a color-role object.", 400)
    try:
        return assign_palette_roles(palette, value).color_by_role
    except PrintablePaletteError as exc:
        raise RequestError("invalid_palette_roles", str(exc), 400) from None


def _normalize_palette_recommendation(value: Any, expected_color_count: int = DEFAULT_PALETTE_COLORS) -> dict[str, Any]:
    if value in (None, {}):
        return {}
    expected_color_count = _normalize_palette_color_count(expected_color_count)
    if not isinstance(value, dict):
        raise RequestError("invalid_palette_recommendation", "palette recommendation must be an object.", 400)
    summary = value.get("summary")
    colors = value.get("colors")
    if not isinstance(summary, str) or not summary.strip() or len(summary.strip().encode("utf-8")) > 400:
        raise RequestError("invalid_palette_recommendation", "palette recommendation summary is invalid.", 400)
    if not isinstance(colors, list) or len(colors) != expected_color_count:
        raise RequestError(
            "invalid_palette_recommendation",
            f"palette recommendation must contain {expected_color_count} colors.",
            400,
        )
    roles = active_palette_roles(expected_color_count)
    by_role: dict[str, dict[str, str]] = {}
    palette_values: list[str] = []
    limits = {"name": 80, "usage": 160, "reason": 400}
    for item in colors:
        if not isinstance(item, dict):
            raise RequestError("invalid_palette_recommendation", "palette recommendation color is invalid.", 400)
        role = item.get("role")
        if role not in roles or role in by_role:
            raise RequestError("invalid_palette_recommendation", "palette recommendation roles are invalid.", 400)
        color = item.get("hex")
        palette = _normalize_palette([color])
        fields: dict[str, str] = {"hex": palette[0], "role": role}
        for name, maximum in limits.items():
            text = item.get(name)
            if not isinstance(text, str) or not text.strip() or len(text.strip().encode("utf-8")) > maximum:
                raise RequestError("invalid_palette_recommendation", f"palette recommendation {name} is invalid.", 400)
            fields[name] = text.strip()
        by_role[role] = fields
        palette_values.append(palette[0])
    palette = _normalize_palette(palette_values)
    if len(palette) != expected_color_count or set(by_role) != set(roles):
        raise RequestError("invalid_palette_recommendation", "palette recommendation colors and roles must be unique.", 400)
    try:
        assignment = assign_palette_roles(palette, {role: by_role[role]["hex"] for role in roles})
    except PrintablePaletteError as exc:
        raise RequestError("invalid_palette_recommendation", str(exc), 400) from None
    if assignment.low_contrast:
        raise RequestError("invalid_palette_recommendation", "palette recommendation colors have insufficient contrast.", 400)
    return {"summary": summary.strip(), "colors": [by_role[role] for role in roles]}


def _multipart_palette_roles(value: Any, palette: tuple[str, ...]) -> dict[str, str]:
    if value in (None, ""):
        return _normalize_palette_roles(None, palette)
    if not isinstance(value, str):
        raise RequestError("invalid_palette_roles", "palette_roles must be valid JSON.", 400)
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError:
        raise RequestError("invalid_palette_roles", "palette_roles must be valid JSON.", 400) from None
    return _normalize_palette_roles(parsed, palette)


@dataclass(frozen=True)
class ValidatedImage:
    content_type: str
    width: int
    height: int


def _validate_image_data(
    data: bytes,
    *,
    minimum_edge: int,
    require_visual_detail: bool = False,
) -> ValidatedImage:
    """Check transport integrity; image quality belongs to advisory reports.

    Keep legacy quality arguments compatible with existing callers, but small,
    blank and transparent decodable images are allowed to continue.
    """
    if not data or len(data) > MAX_IMAGE_BYTES:
        raise ValueError("The image is empty or exceeds the 20 MB limit.")
    content_type = _image_type(data[:16])
    if content_type is None:
        raise ValueError("The image must be PNG or JPEG.")
    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError:
        raise ValueError("Pillow is required to validate the image.") from None
    try:
        with Image.open(BytesIO(data)) as opened:
            expected_format = "PNG" if content_type == "image/png" else "JPEG"
            if opened.format != expected_format:
                raise ValueError("The image format does not match its file signature.")
            width, height = opened.size
            if width * height > MAX_TEXTURE_PIXELS:
                raise ValueError("The image must contain no more than 64 megapixels.")
            opened.load()
    except ValueError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
        raise ValueError("The image is damaged or could not be decoded completely.") from None
    return ValidatedImage(content_type, width, height)


def _validate_image_file(
    path: Path | None,
    *,
    minimum_edge: int,
    require_visual_detail: bool = False,
) -> ValidatedImage:
    if path is None:
        raise ValueError("The image file is unavailable.")
    try:
        size = path.stat().st_size
        if size <= 0 or size > MAX_IMAGE_BYTES:
            raise ValueError("The image is empty or exceeds the 20 MB limit.")
        data = path.read_bytes()
    except ValueError:
        raise
    except OSError:
        raise ValueError("The image file could not be read.") from None
    return _validate_image_data(
        data,
        minimum_edge=minimum_edge,
        require_visual_detail=require_visual_detail,
    )


def _normalize_style(value: Any) -> str:
    if value is None or value == "":
        return "sculpture"
    if isinstance(value, str):
        value = LEGACY_STYLE_ALIASES.get(value, value)
    if not isinstance(value, str) or value not in STYLE_IDS:
        raise RequestError(
            "invalid_style",
            "style must be sculpture, realistic, portrait_sketch, cartoon, low_poly, relief, ink_relief, diorama, or custom.",
            400,
        )
    return value


def _normalize_custom_style(value: Any, style: str) -> str:
    if value is None:
        value = ""
    if not isinstance(value, str):
        raise RequestError("invalid_custom_style", "custom_style must be a string.", 400)
    description = value.strip()
    if style == "custom":
        if not description:
            raise RequestError("invalid_custom_style", "custom_style is required when style is custom.", 400)
        if len(description.encode("utf-8")) > MAX_CUSTOM_STYLE_BYTES:
            raise RequestError("invalid_custom_style", "custom_style exceeds the 1000-byte limit.", 400)
        return description
    if description:
        raise RequestError("invalid_custom_style", "custom_style is only allowed when style is custom.", 400)
    return ""


def _normalize_face_limit(value: Any) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value not in MODEL_FACE_LIMITS:
        raise RequestError(
            "invalid_face_limit",
            "face_limit must be 100000, 300000, 500000, 1000000, or 2000000 triangles.",
            400,
        )
    return value


def _normalize_generation_profile(value: Any) -> str:
    if not isinstance(value, str) or value not in GENERATION_PROFILES:
        raise RequestError(
            "invalid_generation_profile",
            "generation_profile must be quality or performance.",
            400,
        )
    return value


def _generation_options(payload: dict[str, Any], face_limit: int, *,
                        validate_provider_constraints: bool = True) -> tuple[str | None, str, str]:
    provider = _generation_provider(payload)
    geometry = payload.get("geometry_quality")
    texture = payload.get("texture_quality", "standard")
    output = payload.get("output_format", "glb")
    try:
        validate_generation_option_values(geometry, texture)
        if validate_provider_constraints:
            if provider == "hunyuan":
                validate_hunyuan_options(face_limit, geometry, texture)
            else:
                validate_generation_options(face_limit, geometry, texture)
    except (TripoError, ProviderGatewayError) as exc:
        raise RequestError("invalid_generation_options", str(exc), 400) from None
    if output not in ("glb", "obj"):
        raise RequestError("invalid_generation_options", "Output format must be glb or obj.", 400)
    return geometry, texture, output


def _generation_provider(payload: dict[str, Any]) -> str:
    provider = payload.get("provider", "tripo")
    if not isinstance(provider, str) or provider not in ("tripo", "hunyuan"):
        raise RequestError("invalid_provider", "Provider must be tripo or hunyuan.", 400)
    return provider


def _validate_face_target(face_count: int, face_limit: int) -> str:
    maximum = min(MAX_MODEL_FACES, math.ceil(face_limit * MAX_MODEL_FACE_RATIO))
    if face_count > maximum:
        return (
            f"The generated OBJ contains {face_count} triangles; the {face_limit}-triangle target allows at most {maximum}."
        )
    return ""


def _legacy_face_error_is_recoverable(message: str, face_limit: int) -> bool:
    match = re.search(r"contains\s+(\d+)\s+triangles;\s+at least\s+(\d+)\s+are required", message, re.IGNORECASE)
    if match is None:
        return False
    face_count = int(match.group(1))
    minimum = math.floor(face_limit * MIN_MODEL_FACE_RATIO)
    maximum = min(MAX_MODEL_FACES, math.ceil(face_limit * MAX_MODEL_FACE_RATIO))
    return minimum <= face_count <= maximum


def _stale_high_quality_face_gate_is_recoverable(message: str, face_limit: int) -> bool:
    normalized = message.strip().lower()
    return (
        face_limit > 1_000_000
        and face_limit <= MAX_MODEL_FACES
        and "structural quality gate" in normalized
        and "too_many_faces" in normalized
    )


def _normalize_image_instruction(value: Any) -> str:
    if value is None:
        return DEFAULT_IMAGE_INSTRUCTION
    if not isinstance(value, str):
        raise RequestError("invalid_request", "instruction must be UTF-8 text.", 400)
    return value.strip() or DEFAULT_IMAGE_INSTRUCTION


def _user_image_instruction(value: Any) -> str:
    """Return only text the user actually entered; keep internal defaults private."""
    if value is None:
        return ""
    if not isinstance(value, str):
        raise RequestError("invalid_request", "instruction must be UTF-8 text.", 400)
    return value.strip()


def _normalize_print_settings(value: Any) -> dict[str, Any]:
    try:
        return asdict(PrintSettings.from_mapping(value))
    except PrintableImageError as exc:
        raise RequestError("invalid_print_settings", str(exc), 400) from None


def _model_output_root() -> Path:
    return Path(os.environ.get("ORCASLICER_AI_OUTPUT_DIR", Path.cwd() / "generated_models")).resolve()


def _record_journey_event(request: dict[str, Any]) -> dict[str, Any]:
    if set(request) - {"event", "job_id"}:
        raise RequestError(
            "invalid_journey_event",
            "Journey events only accept event and job_id.",
            400,
        )
    event = request.get("event")
    if not isinstance(event, str) or event not in JOURNEY_EVENT_NAMES:
        raise RequestError("invalid_journey_event", "Journey event is not allowed.", 400)
    job_id = request.get("job_id", "")
    if not isinstance(job_id, str):
        raise RequestError("invalid_journey_event", "job_id must be a UUID string.", 400)
    if job_id:
        try:
            parsed_job_id = uuid.UUID(job_id)
        except ValueError:
            raise RequestError("invalid_journey_event", "job_id must be a UUID string.", 400) from None
        if str(parsed_job_id) != job_id.lower():
            raise RequestError("invalid_journey_event", "job_id must be a canonical UUID string.", 400)
        job_id = job_id.lower()

    record = {
        "version": 1,
        "event": event,
        "job_id": job_id,
        "recorded_at": time.time(),
    }
    encoded = json.dumps(record, ensure_ascii=True, separators=(",", ":")) + "\n"
    output_root = _model_output_root()
    destination = output_root / JOURNEY_EVENT_FILENAME
    rotated = output_root / f"{JOURNEY_EVENT_FILENAME}.1"
    try:
        with _JOURNEY_EVENT_LOCK:
            output_root.mkdir(parents=True, exist_ok=True)
            if destination.is_file() and destination.stat().st_size + len(encoded) > MAX_JOURNEY_EVENT_FILE_BYTES:
                rotated.unlink(missing_ok=True)
                os.replace(destination, rotated)
            with destination.open("a", encoding="ascii", newline="\n") as stream:
                stream.write(encoded)
    except OSError:
        raise RequestError("journey_event_unavailable", "Local journey event log is unavailable.", 503) from None
    return record


def _new_job(
    source: str,
    palette: tuple[str, ...] = (),
    palette_roles: dict[str, str] | None = None,
    style: str = "sculpture",
    custom_style: str = "",
    print_settings: dict[str, Any] | None = None,
    palette_color_count: int | None = None,
    provider: str = "tripo",
    generation_options: dict[str, Any] | None = None,
) -> Job:
    options = dict(generation_options or {}, provider=provider)
    face_limit = options.get("face_limit", DEFAULT_MODEL_FACE_LIMIT)
    if isinstance(face_limit, str) and len(face_limit) <= 7 and face_limit.isascii() and face_limit.isdecimal():
        face_limit = int(face_limit)
    face_limit = _normalize_face_limit(face_limit)
    # A design preview has no 3D provider request. Preserve the draft choices;
    # the explicit model confirmation validates their provider-specific combination.
    geometry, texture, output = _generation_options(options, face_limit, validate_provider_constraints=False)
    job_id = str(uuid.uuid4())
    output_root = _model_output_root()
    directory = output_root / job_id
    try:
        directory.mkdir(parents=True, exist_ok=False)
    except OSError:
        raise RequestError("service_unavailable", "The generated-model directory could not be created.", 503, True) from None
    job = Job(
        id=job_id,
        source=source,
        directory=directory,
        palette=palette,
        palette_roles=dict(palette_roles or {}),
        palette_color_count=_normalize_palette_color_count(
            palette_color_count if palette_color_count is not None else (len(palette) or None)
        ),
        style=style,
        custom_style=custom_style,
        print_settings=print_settings or asdict(PrintSettings()),
        provider=provider,
        face_limit=face_limit,
        generation_profile="quality" if face_limit >= 500000 else "performance",
        geometry_quality=geometry,
        texture_quality=texture,
        output_format=output,
    )
    _persist_job(job)
    return job


def _job_path_value(job: Job, path: Path | None) -> str:
    if path is None:
        return ""
    try:
        return path.resolve().relative_to(job.directory.resolve()).as_posix()
    except (OSError, ValueError):
        return ""


def _job_file(job: Job, value: Any) -> Path | None:
    if not isinstance(value, str) or not value:
        return None
    candidate = (job.directory / value).resolve()
    try:
        candidate.relative_to(job.directory.resolve())
    except ValueError:
        return None
    return candidate


def _copy_job_file(source: Path | None, job: Job, name: str) -> Path | None:
    if source is None:
        return None
    try:
        resolved = source.resolve(strict=True)
        if not resolved.is_file():
            return None
        suffix = resolved.suffix.lower()
        destination = job.directory / f"{name}{suffix}"
        shutil.copy2(resolved, destination)
        return destination
    except OSError:
        raise RequestError(
            "service_unavailable",
            "The reference files could not be copied for texture generation.",
            503,
            True,
        ) from None


def _persist_job(job: Job, *, touch: bool = True, required: bool = False) -> None:
    if touch:
        job.updated_at = time.time()
    payload = {
        "version": JOB_STATE_VERSION,
        "id": job.id,
        "source": job.source,
        "state": job.state,
        "phase": job.phase,
        "message": job.message,
        "progress": job.progress,
        "palette": list(job.palette),
        "palette_roles": job.palette_roles,
        "palette_color_count": job.palette_color_count,
        "print_settings": job.print_settings,
        "style": job.style,
        "custom_style": job.custom_style,
        "face_limit": job.face_limit,
        "generation_profile": job.generation_profile,
        "geometry_quality": job.geometry_quality,
        "texture_quality": job.texture_quality,
        "output_format": job.output_format,
        "provider": job.provider,
        "user_prompt": "" if job.source == "image" and job.user_prompt == DEFAULT_IMAGE_INSTRUCTION else job.user_prompt,
        "prepared_prompt": job.prepared_prompt,
        "input_path": _job_path_value(job, job.input_path),
        "raw_preview_path": _job_path_value(job, job.raw_preview_path),
        "strict_preview_path": _job_path_value(job, job.strict_preview_path),
        "preview_path": _job_path_value(job, job.preview_path),
        "model_reference_path": _job_path_value(job, job.model_reference_path),
        "geometry_reference_path": _job_path_value(job, job.geometry_reference_path),
        "preview_content_type": job.preview_content_type,
        "heatmap_path": _job_path_value(job, job.heatmap_path),
        "metadata_path": _job_path_value(job, job.metadata_path),
        "background_mask_path": _job_path_value(job, job.background_mask_path),
        "subject_mask_path": _job_path_value(job, job.subject_mask_path),
        "mask_paths": {key: _job_path_value(job, path) for key, path in job.mask_paths.items()},
        "image_metrics": job.image_metrics,
        "preprocess_failure": job.preprocess_failure,
        "artifact_path": _job_path_value(job, job.artifact_path),
        "artifact_format": job.artifact_format,
        "color_intent_path": _job_path_value(job, job.color_intent_path),
        "color_intent_schema": job.color_intent_schema,
        "color_intent_sha256": job.color_intent_sha256,
        "palette_recommendation": job.palette_recommendation,
        "palette_recommendation_confirmed": job.palette_recommendation_confirmed,
        "generate_image": job.generate_image,
        "attempts": job.attempts,
        "updated_at": job.updated_at,
    }
    temporary = job.directory / f"{JOB_STATE_FILENAME}.part"
    destination = job.directory / JOB_STATE_FILENAME
    try:
        encoded = json.dumps(payload, ensure_ascii=False, indent=2)
        if len(encoded.encode("utf-8")) > MAX_JOB_STATE_BYTES:
            raise OSError("job state exceeds its size limit")
        temporary.write_text(encoded, encoding="utf-8")
        os.replace(temporary, destination)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        if required:
            raise TripoError("The job state could not be saved.") from None


def _load_job(directory: Path) -> Job | None:
    state_path = directory / JOB_STATE_FILENAME
    try:
        if not state_path.is_file() or state_path.stat().st_size > MAX_JOB_STATE_BYTES:
            return None
        payload = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict) or payload.get("version") != JOB_STATE_VERSION:
        return None
    job_id = payload.get("id")
    source = payload.get("source")
    if not isinstance(job_id, str) or directory.name != job_id or source not in {"text", "image"}:
        return None
    try:
        palette = _normalize_palette(payload.get("palette", []))
        palette_roles = _normalize_palette_roles(payload.get("palette_roles"), palette)
        palette_color_count = _normalize_palette_color_count(payload.get("palette_color_count"))
        style = _normalize_style(payload.get("style"))
        custom_style = _normalize_custom_style(payload.get("custom_style"), style)
        face_limit = _normalize_face_limit(payload.get("face_limit", DEFAULT_MODEL_FACE_LIMIT))
        # Drafts with an incompatible model choice must remain recoverable so the
        # user can correct that choice before submitting any paid model task.
        geometry_quality, texture_quality, output_format = _generation_options(
            payload, face_limit, validate_provider_constraints=False)
        raw_generation_profile = payload.get("generation_profile")
        generation_profile = _normalize_generation_profile(raw_generation_profile) if raw_generation_profile is not None else \
            ("quality" if face_limit >= 500000 else "performance")
        print_settings = _normalize_print_settings(payload.get("print_settings"))
        palette_recommendation = _normalize_palette_recommendation(
            payload.get("palette_recommendation"), palette_color_count
        )
    except RequestError:
        return None
    attempts = payload.get("attempts", [])
    if not isinstance(attempts, list) or any(not isinstance(attempt, dict) for attempt in attempts):
        return None
    job = Job(
        id=job_id,
        source=source,
        directory=directory,
        palette=palette,
        palette_roles=palette_roles,
        palette_color_count=palette_color_count,
        style=style,
        custom_style=custom_style,
        face_limit=face_limit,
        generation_profile=generation_profile,
        geometry_quality=geometry_quality,
        texture_quality=texture_quality,
        output_format=output_format,
        provider=_generation_provider(payload),
        print_settings=print_settings,
    )
    job.state = str(payload.get("state", "failed"))
    job.phase = str(payload.get("phase", job.state))
    job.message = str(payload.get("message", "Recovered model job."))
    job.progress = max(0, min(int(payload.get("progress", 0)), 100))
    job.user_prompt = str(payload.get("user_prompt", ""))
    job.prepared_prompt = str(payload.get("prepared_prompt", ""))
    job.generate_image = payload.get("generate_image") is True
    job.input_path = _job_file(job, payload.get("input_path"))
    job.raw_preview_path = _job_file(job, payload.get("raw_preview_path"))
    job.strict_preview_path = _job_file(job, payload.get("strict_preview_path"))
    job.preview_path = _job_file(job, payload.get("preview_path"))
    job.model_reference_path = _job_file(job, payload.get("model_reference_path"))
    job.geometry_reference_path = _job_file(job, payload.get("geometry_reference_path"))
    job.preview_content_type = str(payload.get("preview_content_type", ""))
    job.heatmap_path = _job_file(job, payload.get("heatmap_path"))
    job.metadata_path = _job_file(job, payload.get("metadata_path"))
    job.background_mask_path = _job_file(job, payload.get("background_mask_path"))
    job.subject_mask_path = _job_file(job, payload.get("subject_mask_path"))
    raw_masks = payload.get("mask_paths", {})
    if isinstance(raw_masks, dict):
        job.mask_paths = {
            str(key): path for key, value in raw_masks.items()
            if (path := _job_file(job, value)) is not None
        }
    raw_metrics = payload.get("image_metrics", {})
    job.image_metrics = raw_metrics if isinstance(raw_metrics, dict) else {}
    _apply_legacy_material_fragmentation_gate(job)
    raw_preprocess_failure = payload.get("preprocess_failure", {})
    job.preprocess_failure = raw_preprocess_failure if isinstance(raw_preprocess_failure, dict) else {}
    job.artifact_path = _job_file(job, payload.get("artifact_path"))
    job.artifact_format = str(payload.get("artifact_format", ""))
    job.color_intent_path = _job_file(job, payload.get("color_intent_path"))
    job.color_intent_schema = str(payload.get("color_intent_schema", ""))
    job.color_intent_sha256 = str(payload.get("color_intent_sha256", ""))
    if job.color_intent_path is not None:
        try:
            verified = verify_color_intent_manifest_file(
                job.color_intent_path, job.artifact_path,
                expected_schema=job.color_intent_schema, expected_sha256=job.color_intent_sha256,
            )
            job.color_intent_schema, job.color_intent_sha256 = verified.schema, verified.sha256
        except ColorIntentError:
            job.color_intent_path, job.color_intent_schema, job.color_intent_sha256 = None, "", ""
    else:
        job.color_intent_schema, job.color_intent_sha256 = "", ""
    job.palette_recommendation = palette_recommendation
    job.palette_recommendation_confirmed = bool(payload.get("palette_recommendation_confirmed", False))
    job.attempts = attempts
    try:
        job.updated_at = float(payload.get("updated_at", state_path.stat().st_mtime))
    except (TypeError, ValueError, OSError):
        job.updated_at = time.time()
    return job


def _restore_jobs(*, resume_jobs: bool = True) -> list[Job]:
    output_root = _model_output_root()
    try:
        directories = [path for path in output_root.iterdir() if path.is_dir()]
    except OSError:
        return []
    restored: list[Job] = []
    for directory in directories:
        job = _load_job(directory)
        if job is None:
            continue
        if job.input_path is not None:
            try:
                _validate_image_file(job.input_path, minimum_edge=MIN_SOURCE_IMAGE_EDGE)
            except ValueError:
                job.input_path = None
        for attribute in (
            "raw_preview_path", "strict_preview_path", "preview_path", "model_reference_path",
            "geometry_reference_path",
        ):
            path = getattr(job, attribute)
            if path is None:
                continue
            try:
                _validate_image_file(
                    path,
                    minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                    require_visual_detail=True,
                )
            except ValueError:
                setattr(job, attribute, None)
        if job.state == "awaiting_confirmation" and (job.model_reference_path or job.preview_path) is not None:
            try:
                quality, generation_quality = _assess_reference_advice(job)
                cached_visual_quality = job.image_metrics.get("preview_visual_quality")
                if isinstance(cached_visual_quality, dict):
                    _apply_preview_visual_quality_gate(job, cached_visual_quality)
                    quality = job.image_metrics.get("model_input_quality", quality)
                    generation_quality = job.image_metrics.get(
                        "generation_input_quality", generation_quality
                    )
                if not bool(generation_quality.get("model_input_eligible", False)):
                    job.message = _model_input_quality_message(generation_quality)
                elif not bool(quality.get("model_input_eligible", False)):
                    job.message = _model_input_quality_message(quality)
                elif job.phase == "awaiting_confirmation":
                    job.message = _printable_preview_message(job, "Review the prepared image before generation.")
            except ModelInputImageQualityError:
                job.state = "failed"
                job.phase = "failed"
                job.message = "The saved image preview could not be checked. Generate the preview again."
                job.progress = 0
        if (
            job.source == "image"
            and job.state in {"recommending_palette", "preprocessing", "awaiting_palette_confirmation", "awaiting_confirmation"}
            and job.input_path is None
        ):
            job.state = "failed"
            job.phase = "failed"
            job.message = "The saved reference image is missing or damaged. Select the image again."
            job.progress = 0
        elif job.source == "image" and job.state == "awaiting_confirmation" and job.preview_path is None:
            job.state = "failed"
            job.phase = "failed"
            job.message = "The saved image preview is missing or damaged. Generate the preview again."
            job.progress = 0
        latest_attempt = job.attempts[-1] if job.attempts else {}
        has_paid_model_task = any(
            isinstance(attempt.get("generation_task_id"), str) and bool(attempt.get("generation_task_id"))
            for attempt in job.attempts
        )
        recoverable_multiview_failure = (
            job.state == "failed"
            and not has_paid_model_task
            and not job.attempts
            and job.source == "image"
            and job.style == "realistic"
            and job.generation_profile == "quality"
            and not _identity_preserving_portrait_geometry_enabled(job)
            and job.model_reference_path is not None
            and job.progress >= 10
        )
        if recoverable_multiview_failure:
            job.state = "awaiting_confirmation"
            job.phase = "multiview_retry"
            job.message = (
                "Four-view portrait preparation stopped before any paid Tripo task was created. "
                "The approved preview is preserved and can be retried."
            )
            job.progress = max(17, job.progress)
            job.image_metrics["multiview_retry"] = {
                "required": True,
                "reason": "legacy_prepaid_multiview_failure",
                "paid_task_created": False,
            }
        if _can_manually_retry_hunyuan(job):
            job.state = "awaiting_confirmation"
            job.phase = "model_retry"
            job.message = "The previous Hunyuan submission was rejected. Review the saved design and explicitly confirm a new paid submission."
            job.progress = 15
        recoverable_error = str(latest_attempt.get("error", "")).lower()
        can_retry_download = (
            job.state == "failed"
            and isinstance(latest_attempt.get("generation_task_id"), str)
            and bool(latest_attempt.get("generation_task_id"))
            and (
                (isinstance(latest_attempt.get("conversion_task_id"), str)
                 and bool(latest_attempt.get("conversion_task_id")))
                or (job.provider == "tripo" and job.output_format == "glb"
                    and not latest_attempt.get("conversion_submission_started"))
            )
            and (any(marker in recoverable_error for marker in (
                "unsafe artifact location",
                "invalid obj package",
                "artifact host could not be resolved",
                "artifact could not be downloaded",
                "temporarily unavailable",
                "rate limiting",
                "deadline expired",
            )) or _legacy_face_error_is_recoverable(recoverable_error, job.face_limit)
                or _stale_high_quality_face_gate_is_recoverable(recoverable_error, job.face_limit))
        )
        if can_retry_download:
            job.state = "queued"
            job.phase = "resuming"
            job.message = "Retrying the existing remote artifact download after restart."
            job.progress = max(75, job.progress)
        if (job.provider == "hunyuan" and job.state == "failed" and has_paid_model_task
                and latest_attempt.get("provider_error_retryable") is True
                and latest_attempt.get("provider_error_ambiguous") is not True):
            job.state = "queued"
            job.phase = "resuming"
            job.message = "Resuming the existing Hunyuan task after a temporary query or download failure."
        if job.state in {"preprocessing", "recommending_palette"}:
            job.state = "failed"
            job.phase = "failed"
            job.message = "The sidecar restarted during a local AI step. Start that step again manually."
            job.progress = 0
        # ``stopping`` is written only for an explicit user stop.  Treat it as
        # durable intent: after a crash or forced app exit, never resurrect the
        # already-paid task and surprise the user with more local processing.
        if job.state == "stopping":
            job.state = "stopped"
            job.phase = "stopped"
            job.message = "Model generation stopped."
            job.progress = 0
            _clear_job_artifact(job)
        elif job.state in {"queued", "running"}:
            generation_id = next(
                (attempt.get("generation_task_id") for attempt in reversed(job.attempts)
                 if isinstance(attempt.get("generation_task_id"), str) and attempt.get("generation_task_id")),
                "",
            )
            if generation_id:
                job.state = "queued"
                job.phase = "resuming"
                job.message = "Resuming the existing paid model task after restart."
                job.progress = max(20, job.progress)
            else:
                job.state = "failed"
                job.phase = "failed"
                job.message = "The sidecar restarted before the paid task reference was saved. Start a new generation manually."
                job.progress = 0
        restored.append(job)
    with _JOBS_LOCK:
        for job in restored:
            _JOBS[job.id] = job
            _persist_job(job, touch=False)
    if resume_jobs:
        _resume_restored_jobs(restored)
    return restored


def _resume_restored_jobs(restored: list[Job]) -> None:
    for job in restored:
        if job.state == "queued" and job.phase == "resuming":
            latest_attempt = job.attempts[-1] if job.attempts else {}
            if latest_attempt.get("provider_operation") == "model_texture":
                source_job_id = str(latest_attempt.get("source_job_id", ""))
                source_task_id = str(latest_attempt.get("source_task_id", ""))
                if source_job_id and source_task_id:
                    _submit(job, _retexture_job, source_job_id, source_task_id, True)
                else:
                    _fail_job(job, "The preserved geometry reference is unavailable; start a new task manually.")
            else:
                _submit(job, _generate_job, job.prepared_prompt, True)


def _adopt_legacy_completed_job(job_id: str) -> Job | None:
    """Register a pre-manifest model library entry without accepting an arbitrary path."""
    try:
        if str(uuid.UUID(job_id)) != job_id.lower():
            return None
    except ValueError:
        return None
    output_root = _model_output_root().resolve()
    try:
        directory = (output_root / job_id).resolve(strict=True)
        directory.relative_to(output_root)
        if not directory.is_dir():
            return None
        artifact = (directory / "model-vertex-color.obj").resolve(strict=True)
        artifact.relative_to(directory)
        artifact_size = artifact.stat().st_size
        if not artifact.is_file() or artifact_size <= 0 or artifact_size > MAX_ARTIFACT_BYTES:
            return None
    except (OSError, ValueError):
        return None

    job = Job(id=job_id, source="image", directory=directory)
    job.state = "ready"
    job.phase = "ready"
    job.message = "Recovered historical model library entry."
    job.progress = 100
    job.artifact_path = artifact
    job.artifact_format = "obj"  # This adoption path is for legacy OBJ jobs.
    attempts_path = directory / "attempts.json"
    try:
        if attempts_path.is_file() and attempts_path.stat().st_size <= MAX_JOB_STATE_BYTES:
            attempts_payload = json.loads(attempts_path.read_text(encoding="utf-8"))
            attempts = attempts_payload.get("attempts", []) if isinstance(attempts_payload, dict) else []
            if isinstance(attempts, list) and all(isinstance(attempt, dict) for attempt in attempts):
                job.attempts = attempts
    except (OSError, UnicodeError, json.JSONDecodeError):
        pass
    preview = directory / "preview.png"
    try:
        resolved_preview = preview.resolve(strict=True)
        resolved_preview.relative_to(directory)
        if resolved_preview.is_file() and resolved_preview.stat().st_size > 0:
            job.preview_path = resolved_preview
            job.preview_content_type = "image/png"
    except (OSError, ValueError):
        pass
    try:
        job.updated_at = artifact.stat().st_mtime
    except OSError:
        pass

    with _JOBS_LOCK:
        existing = _JOBS.get(job_id)
        if existing is not None:
            return existing
        _JOBS[job_id] = job
    return job


def _latest_job_is_restorable(job: Job) -> bool:
    """Return whether the native UI can reconstruct a useful journey state.

    Prepared-only text fixtures and abandoned internal jobs have no user input
    that the panel can display.  Letting one of those win `/latest` clears a
    newer user's recoverable image journey after restart.
    """

    if job.state not in {
        "recommending_palette", "awaiting_palette_confirmation", "preprocessing",
        "awaiting_confirmation", "queued", "running", "stopping", "ready",
    }:
        return False
    if job.state == "ready":
        return _file_info(job.artifact_path)[0]
    if job.source == "image":
        return _file_info(job.input_path)[0]
    return bool(job.user_prompt.strip())


def _read_job_report(job: Job, filename: str) -> dict[str, Any]:
    path = job.directory / filename
    try:
        if path.is_file() and path.stat().st_size <= MAX_JOB_STATE_BYTES:
            candidate = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(candidate, dict):
                return candidate
    except (OSError, UnicodeError, json.JSONDecodeError):
        pass
    return {}


def _public_job(job: Job) -> dict[str, Any]:
    input_ready, input_size = _file_info(job.input_path)
    preview_ready, preview_size = _file_info(job.preview_path)
    raw_preview_ready, raw_preview_size = _file_info(job.raw_preview_path)
    strict_preview_ready, strict_preview_size = _file_info(job.strict_preview_path)
    submitted_reference = _geometry_generation_reference(job)
    model_reference_ready, model_reference_size = _file_info(submitted_reference)
    geometry_reference_ready, geometry_reference_size = _file_info(job.geometry_reference_path)
    heatmap_ready, heatmap_size = _file_info(job.heatmap_path)
    metadata_ready, metadata_size = _file_info(job.metadata_path)
    artifact_ready, artifact_size = _file_info(job.artifact_path)
    color_intent_ready, color_intent_size = _file_info(job.color_intent_path)
    model_quality = _read_job_report(job, MODEL_QUALITY_FILENAME)
    visual_quality = _read_job_report(job, VISUAL_QUALITY_FILENAME)
    refinement = build_model_refinement_advice(model_quality, visual_quality)
    model_view_sheet_ready, model_view_sheet_size = _file_info(job.directory / "model-view-sheet.png")
    artifact_filename = ""
    if artifact_ready:
        artifact_filename = f"orcaslicer-model-{job.id}.{job.artifact_format}"
    provider_failure: dict[str, Any] = {}
    latest_attempt = job.attempts[-1] if job.attempts else {}
    provider_attempt = next(
        (
            attempt for attempt in reversed(job.attempts)
            if isinstance(attempt.get("generation_task_id"), str)
            and bool(attempt.get("generation_task_id"))
        ),
        {},
    )
    provider_tasks = {
        "provider": job.provider,
        "generation_task_id": str(provider_attempt.get("generation_task_id", "")),
        "conversion_task_id": str(provider_attempt.get("conversion_task_id", "")),
    } if provider_attempt else {}
    code = latest_attempt.get("provider_error_code")
    if isinstance(code, str) and code:
        category = latest_attempt.get("provider_error_category")
        provider_failure = {
            "code": code,
            "category": category if isinstance(category, str) else "",
            "retryable": latest_attempt.get("provider_error_retryable") is True,
            "ambiguous": latest_attempt.get("provider_error_ambiguous") is True,
        }
    elif job.preprocess_failure:
        provider_failure = {
            "code": str(job.preprocess_failure.get("code", "")),
            "category": "image_preprocessing",
            "retryable": job.preprocess_failure.get("retryable") is True,
            "ambiguous": job.preprocess_failure.get("ambiguous") is True,
        }
    return {
        "id": job.id,
        "source": job.source,
        "style": job.style,
        "custom_style": job.custom_style,
        "face_limit": job.face_limit,
        "generation_profile": job.generation_profile,
        "geometry_quality": job.geometry_quality,
        "texture_quality": job.texture_quality,
        "output_format": job.output_format,
        "provider": job.provider,
        "state": job.state,
        "phase": job.phase,
        "message": job.message,
        "progress": job.progress,
        "attempt": len(job.attempts),
        "max_attempts": MAX_GENERATION_ATTEMPTS,
        "prepared_prompt": job.prepared_prompt if job.source == "text" else "",
        "user_prompt": "" if job.source == "image" and job.user_prompt == DEFAULT_IMAGE_INSTRUCTION else job.user_prompt,
        "palette": list(job.palette),
        "palette_roles": job.palette_roles,
        "palette_color_count": job.palette_color_count,
        "palette_recommendation": job.palette_recommendation,
        "palette_recommendation_confirmed": job.palette_recommendation_confirmed,
        "print": job.print_settings,
        "image_metrics": job.image_metrics,
        "model_quality": model_quality,
        "visual_quality": visual_quality,
        "refinement": refinement,
        "provider_failure": provider_failure,
        "provider_tasks": provider_tasks,
        "model_views": {
            "ready": model_view_sheet_ready,
            "size_bytes": model_view_sheet_size if model_view_sheet_ready else 0,
        },
        "updated_at": job.updated_at,
        "input": {
            "ready": input_ready,
            "content_type": _stored_image_type(job.input_path) if input_ready else "",
            "size_bytes": input_size if input_ready else 0,
        },
        "preview": {
            "ready": preview_ready,
            "content_type": job.preview_content_type if preview_ready else "",
            "size_bytes": preview_size if preview_ready else 0,
        },
        "image_outputs": {
            "raw_preview": {"ready": raw_preview_ready, "size_bytes": raw_preview_size},
            "strict_preview": {"ready": strict_preview_ready, "size_bytes": strict_preview_size},
            "clean_preview": {"ready": preview_ready, "size_bytes": preview_size},
            "model_reference": {"ready": model_reference_ready, "size_bytes": model_reference_size},
            "geometry_reference": {
                "ready": geometry_reference_ready,
                "size_bytes": geometry_reference_size,
            },
            "heatmap": {"ready": heatmap_ready, "size_bytes": heatmap_size},
            "metadata": {"ready": metadata_ready, "size_bytes": metadata_size},
            "masks": sorted(job.mask_paths),
        },
        "artifact": {
            "ready": artifact_ready,
            "format": job.artifact_format if artifact_ready else "",
            "color_encoding": ("textures_or_vertex_colors" if job.artifact_format == "glb" else "vertex_colors") if artifact_ready else "",
            "filename": artifact_filename,
            "size_bytes": artifact_size if artifact_ready else 0,
            "color_intent": {
                "ready": color_intent_ready,
                "schema": job.color_intent_schema if color_intent_ready else "",
                "sha256": job.color_intent_sha256 if color_intent_ready else "",
                "filename": f"orcaslicer-color-intent-{job.id}.json" if color_intent_ready else "",
                "size_bytes": color_intent_size if color_intent_ready else 0,
            },
        },
    }


def _cleanup_job(job: Job) -> None:
    # Job removal only releases in-memory state. Generated inputs, previews, and
    # model resources are user artifacts and remain available on disk.
    return


def _remove_job_state(job: Job) -> None:
    for name in (JOB_STATE_FILENAME, f"{JOB_STATE_FILENAME}.part"):
        try:
            (job.directory / name).unlink(missing_ok=True)
        except OSError:
            pass


def _finish_deleted(job: Job) -> None:
    with _JOBS_LOCK:
        if job.delete_requested:
            _JOBS.pop(job.id, None)
            _remove_job_state(job)
        else:
            _persist_job(job)


def _clear_job_artifact(job: Job) -> None:
    job.artifact_path, job.artifact_format = None, ""
    job.color_intent_path, job.color_intent_schema, job.color_intent_sha256 = None, "", ""


def _mark_stopped(job: Job) -> None:
    with _JOBS_LOCK:
        job.state = "stopped"
        job.phase = "stopped"
        job.message = "Model generation stopped."
        job.progress = 0
        _clear_job_artifact(job)
        _persist_job(job)


def _stop_boundary(job: Job) -> None:
    if job.stop_event.is_set():
        if _SHUT_DOWN:
            raise SidecarRestart()
        _mark_stopped(job)
        raise JobStopped()


def _fail_job(job: Job, message: str) -> None:
    diagnostic_event("job.failed", level="ERROR", phase=job.phase, failure_message=message)
    with _JOBS_LOCK:
        if job.stop_event.is_set():
            job.state = "stopped"
            job.phase = "stopped"
            job.message = "Model generation stopped."
            job.progress = 0
        else:
            job.state = "failed"
            job.phase = "failed"
            job.message = message
        _clear_job_artifact(job)
        _persist_job(job)


def _return_to_portrait_multiview_retry(job: Job, message: str) -> None:
    """Keep the approved preview retryable when local four-view checks fail."""

    with _JOBS_LOCK:
        if job.stop_event.is_set():
            job.state = "stopped"
            job.phase = "stopped"
            job.message = "Model generation stopped."
            job.progress = 0
        else:
            job.state = "awaiting_confirmation"
            job.phase = "multiview_retry"
            job.message = message
            job.progress = max(17, job.progress)
            job.image_metrics["multiview_retry"] = {
                "required": True,
                "reason": message,
                "paid_task_created": False,
            }
        _clear_job_artifact(job)
        _persist_job(job)


def _fail_preprocess_job(job: Job, error: OpenAIPreprocessorError) -> None:
    job.preprocess_failure = preprocess_failure_payload(error)
    _fail_job(job, str(error))
def _apply_printable_image_pipeline(job: Job, raw_preview: Path) -> dict[str, int]:
    try:
        result = process_printable_image(
            raw_preview,
            job.directory,
            job.palette,
            job.print_settings,
            palette_roles=job.palette_roles,
            subject_reference_path=job.input_path if job.source == "image" else None,
        )
    except PrintableImageError as exc:
        raise OpenAIPreprocessorError(str(exc)) from None
    job.raw_preview_path = raw_preview
    job.strict_preview_path = result.strict_preview
    job.preview_path = result.clean_preview
    job.model_reference_path = result.model_reference
    job.heatmap_path = result.heatmap
    job.metadata_path = result.metadata
    job.background_mask_path = result.background_mask
    job.subject_mask_path = result.subject_mask
    job.mask_paths = result.masks
    job.image_metrics = result.metrics
    portrait_cleanup = result.metrics.get("portrait_skin_cleanup", {})
    if isinstance(portrait_cleanup, dict) and portrait_cleanup.get("activated") == 1:
        garment_color = str(portrait_cleanup.get("garment_color", "")).upper()
        skin_color = str(portrait_cleanup.get("skin_color", "")).upper()
        if garment_color in job.palette and skin_color in job.palette and garment_color != skin_color:
            # Persist the semantic recovery so a restarted native client does
            # not re-submit an inverted skin/garment mapping on the next image.
            job.palette_roles = assign_palette_roles(
                job.palette,
                {"primary": garment_color, "light": skin_color},
            ).color_by_role
    return result.palette_usage


def _write_job_color_intent(job: Job, artifact: Path) -> None:
    if not job.palette or artifact.suffix.lower() == ".glb":
        job.color_intent_path, job.color_intent_schema, job.color_intent_sha256 = None, "", ""
        return
    appearance = job.raw_preview_path or job.model_reference_path
    material = job.preview_path or job.strict_preview_path
    if appearance is None or material is None:
        job.color_intent_path, job.color_intent_schema, job.color_intent_sha256 = None, "", ""
        return
    if not job.palette_roles:
        job.palette_roles = assign_palette_roles(job.palette).color_by_role
    try:
        result = write_color_intent_manifest(
            job.directory / COLOR_INTENT_FILENAME, artifact, appearance, material,
            job.palette, job.palette_roles,
            geometry_reference_path=_geometry_generation_reference(job),
        )
    except ColorIntentError as exc:
        raise TripoError(f"The color-intent manifest could not be published: {exc}") from None
    job.color_intent_path, job.color_intent_schema, job.color_intent_sha256 = (
        result.path, result.schema, result.sha256
    )


def _model_generation_reference(job: Job) -> Path | None:
    """Submit the approved AI design, retaining legacy reference routing for old jobs."""
    if job.image_metrics.get("design_reference") == "ai-design-v1":
        return job.raw_preview_path
    if job.model_reference_path is not None:
        return job.model_reference_path
    if job.palette and job.raw_preview_path is not None:
        return job.raw_preview_path
    return job.preview_path


def _identity_preserving_portrait_geometry_enabled(job: Job) -> bool:
    cleanup = job.image_metrics.get("portrait_skin_cleanup", {})
    portrait_geometry = job.image_metrics.get("portrait_geometry", {})
    portrait_detected = (
        isinstance(portrait_geometry, dict)
        and portrait_geometry.get("detected") is True
    ) or (
        isinstance(cleanup, dict)
        and cleanup.get("activated") == 1
    )
    return (
        job.source == "image"
        and job.style in IDENTITY_FIRST_PORTRAIT_STYLES
        and job.generation_profile == "quality"
        and portrait_detected
        and job.input_path is not None
        and job.input_path.is_file()
    )


def _geometry_generation_reference(job: Job) -> Path | None:
    """New designs retain the approved composition; old jobs keep their evidence."""

    if job.image_metrics.get("design_reference") == "ai-design-v1":
        return job.raw_preview_path

    if _identity_preserving_portrait_geometry_enabled(job):
        # The chooser is used concurrently by paid submission, public status,
        # preview download and restart recovery. Serialize the small atomic
        # image rewrite so those routes cannot replace one another's temporary
        # files while a request is being confirmed.
        with _GEOMETRY_REFERENCE_LOCK:
            # Repair here—not only during initial preprocessing—so every route
            # receives the same validated provider image.
            _synchronize_geometry_reference_alpha(job)
            # Preserve the historical geometry contract for existing jobs.
            if job.geometry_reference_path is not None and job.geometry_reference_path.is_file():
                return _prepare_portrait_geometry_provider_reference(job)
        return _model_generation_reference(job) or job.input_path
    return _model_generation_reference(job)


def _refine_portrait_head_silhouette(
    geometry: Any,
    alpha: Any,
    source_alpha: Any | None = None,
) -> tuple[Any, dict[str, Any]]:
    """Remove attached light backdrop halos around a portrait head.

    Image generators sometimes draw a checkerboard or soft white oval instead
    of emitting true transparency.  Generic connected-component cleanup cannot
    remove it because the oval touches the hair and shoulders.  In the upper
    portrait region, however, real hair/skin supplies a strong non-neutral
    span on every row.  Trim only light-neutral pixels outside that span, stop
    when the silhouette widens into the shoulders, and leave the white jacket
    and the rest of the bust completely untouched.
    """

    subject_bbox = alpha.getbbox()
    report: dict[str, Any] = {
        "version": "portrait-silhouette-v6",
        "status": "not_needed",
        "removed_pixels": 0,
        "removed_detached_pixels": 0,
        "source_head_backdrop_removed_pixels": 0,
        "source_head_backdrop_component_count": 0,
        "light_backdrop_removed_pixels": 0,
        "light_backdrop_component_count": 0,
        "source_neck_removed_pixels": 0,
    }
    if subject_bbox is None:
        return alpha, report
    left, top, right, bottom = subject_bbox
    subject_width = right - left
    subject_height = bottom - top
    if subject_width < 32 or subject_height < 64:
        return alpha, report

    alpha_pixels = alpha.load()
    geometry_pixels = geometry.load()
    row_bounds: dict[int, tuple[int, int, int]] = {}
    for y in range(top, bottom):
        row = [x for x in range(left, right) if alpha_pixels[x, y] >= 128]
        if row:
            row_bounds[y] = (row[0], row[-1], len(row))
    subject_area = max(1, sum(item[2] for item in row_bounds.values()))

    shoulder_threshold = max(24, int(round(subject_width * 0.55)))
    search_start = top + int(round(subject_height * 0.22))
    search_end = min(bottom, top + int(round(subject_height * 0.58)))
    shoulder_y: int | None = None
    for y in range(search_start, search_end):
        current = row_bounds.get(y)
        if current is None or current[2] < shoulder_threshold:
            continue
        preceding = [
            row_bounds[row][2]
            for row in range(max(top, y - 36), max(top, y - 6))
            if row in row_bounds
        ]
        following = [
            row_bounds[row][2]
            for row in range(y, min(bottom, y + 8))
            if row in row_bounds
        ]
        if (
            preceding
            and following
            and min(following) >= shoulder_threshold
            and current[2] >= min(preceding) * 1.18
        ):
            shoulder_y = y
            break
    if shoulder_y is None:
        report["reason"] = "shoulder_transition_not_detected"
        return alpha, report

    refined = alpha.copy()
    refined_pixels = refined.load()
    margin = max(1, int(round(subject_width * 0.002)))
    removed_pixels = 0
    cleaned_rows = 0
    source_mask_used = False
    source_removed_pixels = 0
    source_head_backdrop_removed_pixels = 0
    source_head_backdrop_component_count = 0
    light_backdrop_removed_pixels = 0
    light_backdrop_component_count = 0
    light_backdrop_end = 0
    source_neck_removed_pixels = 0
    source_neck_margin = 0
    source_neck_end = 0
    source_torso_removed_pixels = 0
    source_torso_margin = 0
    source_torso_end = 0
    source_head_end = max(top, shoulder_y - max(8, round(subject_height * 0.04)))
    if source_alpha is not None and getattr(source_alpha, "size", None) == alpha.size:
        source_pixels = source_alpha.load()
        source_count = 0
        overlap = 0
        for y in range(top, source_head_end):
            for x in range(left, right):
                if source_pixels[x, y] >= 128:
                    source_count += 1
                    overlap += alpha_pixels[x, y] >= 128
        if source_count >= 32 and overlap >= source_count * 0.70:
            from PIL import ImageFilter

            source_mask_used = True
            for y in range(top, source_head_end):
                for x in range(left, right):
                    if refined_pixels[x, y] >= 128 and source_pixels[x, y] < 128:
                        refined_pixels[x, y] = 0
                        source_removed_pixels += 1
            removed_pixels += source_removed_pixels

            # The aligned source mask is intentionally expanded by a few pixels
            # so it cannot shave real hair. That safety rim can also retain a
            # narrow opaque checkerboard crescent beside dark hair. Detect only
            # large light-neutral components that touch the refined head edge
            # and live mostly outside an eroded source core. Real white or gray
            # hair extends well into that core and is therefore preserved.
            source_core_kernel = max(
                3,
                min(9, (round(min(alpha.width, alpha.height) * 0.008) | 1)),
            )
            source_core = source_alpha.filter(ImageFilter.MinFilter(source_core_kernel))
            source_core_pixels = source_core.load()
            head_candidates = bytearray(alpha.width * alpha.height)
            for y in range(top, source_head_end):
                row = y * alpha.width
                for x in range(left, right):
                    if refined_pixels[x, y] < 128:
                        continue
                    red, green, blue = geometry_pixels[x, y][:3]
                    if min(red, green, blue) >= 200 and max(red, green, blue) - min(red, green, blue) <= 32:
                        head_candidates[row + x] = 1

            visited_head = bytearray(len(head_candidates))
            removable_head_components: list[list[int]] = []
            minimum_head_component = max(48, round(subject_area * 0.0001))
            for seed, is_candidate in enumerate(head_candidates):
                if not is_candidate or visited_head[seed]:
                    continue
                visited_head[seed] = 1
                pending: deque[int] = deque([seed])
                component: list[int] = []
                outside_core_count = 0
                touches_alpha_edge = False
                while pending:
                    offset = pending.popleft()
                    component.append(offset)
                    x = offset % alpha.width
                    y = offset // alpha.width
                    outside_core_count += source_core_pixels[x, y] < 128
                    for neighbor in (
                        offset - 1 if x else -1,
                        offset + 1 if x + 1 < alpha.width else -1,
                        offset - alpha.width if y else -1,
                        offset + alpha.width if y + 1 < alpha.height else -1,
                    ):
                        if neighbor < 0:
                            touches_alpha_edge = True
                            continue
                        neighbor_x = neighbor % alpha.width
                        neighbor_y = neighbor // alpha.width
                        if refined_pixels[neighbor_x, neighbor_y] < 128:
                            touches_alpha_edge = True
                        elif head_candidates[neighbor] and not visited_head[neighbor]:
                            visited_head[neighbor] = 1
                            pending.append(neighbor)
                area = len(component)
                if (
                    area >= minimum_head_component
                    and outside_core_count >= round(area * 0.55)
                    and touches_alpha_edge
                ):
                    removable_head_components.append(component)
            head_removal = sum(len(component) for component in removable_head_components)
            if head_removal <= round(subject_area * 0.005):
                for component in removable_head_components:
                    for offset in component:
                        refined_pixels[offset % alpha.width, offset // alpha.width] = 0
                source_head_backdrop_removed_pixels = head_removal
                source_head_backdrop_component_count = len(removable_head_components)
                removed_pixels += head_removal

            # The source-locked head removes the large hair halo, but white
            # checkerboard patches can remain attached to shoulders and crossed
            # arms. Tripo turns those neutral patches into small connected
            # plates. Real sculptural skin and garments in this reference are
            # warm gray; use large, boundary-connected neutral-white components
            # as seeds and require that each component visibly escapes the
            # source silhouette. This keeps a white jacket and the generated
            # base while removing the backdrop before a paid submission.
            source_bbox = source_alpha.getbbox()
            if source_bbox is not None:
                light_backdrop_end = min(
                    bottom,
                    round(alpha.height * 0.76),
                    source_bbox[3] + max(12, round(subject_height * 0.05)),
                )
                candidates = bytearray(alpha.width * alpha.height)
                seeds = bytearray(alpha.width * alpha.height)
                for y in range(source_head_end, light_backdrop_end):
                    row = y * alpha.width
                    for x in range(left, right):
                        if refined_pixels[x, y] < 128:
                            continue
                        red, green, blue = geometry_pixels[x, y][:3]
                        minimum = min(red, green, blue)
                        spread = max(red, green, blue) - minimum
                        offset = row + x
                        if minimum >= 225 and spread <= 10:
                            candidates[offset] = 1
                        if minimum >= 235 and spread <= 6:
                            seeds[offset] = 1

                visited = bytearray(len(candidates))
                removable_components: list[list[int]] = []
                minimum_component_area = max(128, round(subject_area * 0.0005))
                for seed, is_candidate in enumerate(candidates):
                    if not is_candidate or visited[seed]:
                        continue
                    visited[seed] = 1
                    pending: deque[int] = deque([seed])
                    component: list[int] = []
                    seed_count = 0
                    outside_source_count = 0
                    touches_alpha_edge = False
                    while pending:
                        offset = pending.popleft()
                        component.append(offset)
                        x = offset % alpha.width
                        y = offset // alpha.width
                        seed_count += bool(seeds[offset])
                        outside_source_count += source_pixels[x, y] < 128
                        for neighbor in (
                            offset - 1 if x else -1,
                            offset + 1 if x + 1 < alpha.width else -1,
                            offset - alpha.width if y else -1,
                            offset + alpha.width if y + 1 < alpha.height else -1,
                        ):
                            if neighbor < 0:
                                touches_alpha_edge = True
                                continue
                            neighbor_x = neighbor % alpha.width
                            neighbor_y = neighbor // alpha.width
                            if alpha_pixels[neighbor_x, neighbor_y] < 128:
                                touches_alpha_edge = True
                            elif candidates[neighbor] and not visited[neighbor]:
                                visited[neighbor] = 1
                                pending.append(neighbor)
                    area = len(component)
                    if (
                        area >= minimum_component_area
                        and seed_count >= max(32, round(area * 0.15))
                        and outside_source_count >= max(24, round(area * 0.03))
                        and touches_alpha_edge
                    ):
                        removable_components.append(component)

                candidate_removal = sum(len(component) for component in removable_components)
                if candidate_removal <= round(subject_area * 0.06):
                    for component in removable_components:
                        for offset in component:
                            refined_pixels[offset % alpha.width, offset // alpha.width] = 0
                    light_backdrop_removed_pixels = candidate_removal
                    light_backdrop_component_count = len(removable_components)
                    removed_pixels += candidate_removal

                # Neutral-component cleanup deliberately ignores warm-gray
                # pixels, but a studio-shadow fringe at the neck can share that
                # warmer tone and remain attached to the generated shoulders.
                # On a single-view image-to-3D request even a small triangle in
                # this narrow transition becomes a vertical plate behind the
                # collar.  The edit is composition-locked to the user's photo,
                # so constrain only this short neck-to-shoulder band to the
                # aligned source silhouette.  A modest row-wise margin avoids
                # clipping hair or lapels; the operation stops before the main
                # jacket and crossed arms, and a hard area cap rejects a badly
                # aligned source mask.
                source_neck_margin = max(4, min(12, round(subject_width * 0.015)))
                source_neck_end = min(
                    light_backdrop_end,
                    shoulder_y + max(16, round(subject_height * 0.024)),
                )
                neck_offsets: list[int] = []
                for y in range(source_head_end, source_neck_end):
                    source_row = [
                        x for x in range(left, right) if source_pixels[x, y] >= 128
                    ]
                    if not source_row:
                        continue
                    keep_left = max(left, source_row[0] - source_neck_margin)
                    keep_right = min(right - 1, source_row[-1] + source_neck_margin)
                    row = y * alpha.width
                    for x in range(left, keep_left):
                        if refined_pixels[x, y] >= 128:
                            neck_offsets.append(row + x)
                    for x in range(keep_right + 1, right):
                        if refined_pixels[x, y] >= 128:
                            neck_offsets.append(row + x)
                if len(neck_offsets) <= round(subject_area * 0.01):
                    for offset in neck_offsets:
                        refined_pixels[offset % alpha.width, offset // alpha.width] = 0
                    source_neck_removed_pixels = len(neck_offsets)
                    removed_pixels += source_neck_removed_pixels

                # A neutral plate can carry warm-gray shadows along the neck,
                # so the color seed intentionally leaves a small triangular
                # remainder.  In only the shared shoulder/upper-torso band,
                # apply a generously dilated source silhouette as a second
                # guard. The margin scales with the portrait and the operation
                # stops well before the generated bust finish or base.
                source_torso_margin = max(8, min(24, round(subject_width * 0.03)))
                source_torso_end = min(
                    light_backdrop_end,
                    max(source_head_end, source_bbox[3] - source_torso_margin * 2),
                )
                source_torso_alpha = source_alpha.filter(
                    ImageFilter.MaxFilter(source_torso_margin * 2 + 1)
                )
                source_torso_pixels = source_torso_alpha.load()
                torso_offsets: list[int] = []
                for y in range(source_head_end, source_torso_end):
                    row = y * alpha.width
                    for x in range(left, right):
                        if refined_pixels[x, y] >= 128 and source_torso_pixels[x, y] < 128:
                            torso_offsets.append(row + x)
                if len(torso_offsets) <= round(subject_area * 0.015):
                    for offset in torso_offsets:
                        refined_pixels[offset % alpha.width, offset // alpha.width] = 0
                    source_torso_removed_pixels = len(torso_offsets)
                    removed_pixels += source_torso_removed_pixels

    # The aligned source silhouette is more reliable than color thresholds and
    # preserves white/gray hair. Use the row-wise neutral-halo fallback only
    # when a trustworthy source mask could not be recovered.
    for y in (() if source_mask_used else range(top, shoulder_y)):
        bounds = row_bounds.get(y)
        if bounds is None:
            continue
        row_left, row_right, row_count = bounds
        content: list[int] = []
        for x in range(row_left, row_right + 1):
            if alpha_pixels[x, y] < 128:
                continue
            red, green, blue = geometry_pixels[x, y][:3]
            # The endpoint often paints a soft studio shadow around an opaque
            # checkerboard.  Its inner edge can be medium gray (roughly 170),
            # not merely near-white.  Treat the smooth neutral ramp as empty
            # until real hair/skin chroma or darker sculptural detail begins.
            # Cleanup is still limited to rows above the detected shoulders,
            # so a cream or white jacket is never trimmed by this threshold.
            light_neutral_background = (
                min(red, green, blue) >= 170
                and max(red, green, blue) - min(red, green, blue) <= 32
            )
            if not light_neutral_background:
                content.append(x)
        if len(content) < max(8, int(round(row_count * 0.25))):
            continue
        keep_left = max(row_left, content[0] - margin)
        keep_right = min(row_right, content[-1] + margin)
        if keep_left <= row_left and keep_right >= row_right:
            continue
        for x in range(row_left, keep_left):
            if refined_pixels[x, y] >= 128:
                refined_pixels[x, y] = 0
                removed_pixels += 1
        for x in range(keep_right + 1, row_right + 1):
            if refined_pixels[x, y] >= 128:
                refined_pixels[x, y] = 0
                removed_pixels += 1
        cleaned_rows += 1

    # A single opaque checkerboard dash is enough for image-to-3D to create a
    # thin spike beside the base. Portrait geometry is expected to be one
    # connected bust/base silhouette, so retain only the largest 4-connected
    # alpha component after the halo trim. Use two bounded flood-fill passes to
    # avoid retaining a large per-component pixel list for megapixel images.
    foreground = bytes(value >= 128 for value in refined.tobytes())
    visited = bytearray(len(foreground))
    largest_seed = -1
    largest_area = 0
    for seed, is_foreground in enumerate(foreground):
        if not is_foreground or visited[seed]:
            continue
        visited[seed] = 1
        pending: deque[int] = deque([seed])
        area = 0
        while pending:
            offset = pending.popleft()
            area += 1
            x = offset % refined.width
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < refined.width else -1,
                offset - refined.width if offset >= refined.width else -1,
                offset + refined.width if offset + refined.width < len(foreground) else -1,
            ):
                if neighbor >= 0 and foreground[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    pending.append(neighbor)
        if area > largest_area:
            largest_seed = seed
            largest_area = area

    detached_pixels = 0
    if largest_seed >= 0 and largest_area:
        keep = bytearray(len(foreground))
        keep[largest_seed] = 1
        pending = deque([largest_seed])
        while pending:
            offset = pending.popleft()
            x = offset % refined.width
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < refined.width else -1,
                offset - refined.width if offset >= refined.width else -1,
                offset + refined.width if offset + refined.width < len(foreground) else -1,
            ):
                if neighbor >= 0 and foreground[neighbor] and not keep[neighbor]:
                    keep[neighbor] = 1
                    pending.append(neighbor)
        for offset, is_foreground in enumerate(foreground):
            if is_foreground and not keep[offset]:
                refined_pixels[offset % refined.width, offset // refined.width] = 0
                detached_pixels += 1
        removed_pixels += detached_pixels

    report.update({
        "status": "refined" if removed_pixels else "not_needed",
        "shoulder_y": shoulder_y,
        "cleaned_rows": cleaned_rows,
        "margin_px": margin,
        "removed_pixels": removed_pixels,
        "removed_detached_pixels": detached_pixels,
        "source_mask_used": source_mask_used,
        "source_removed_pixels": source_removed_pixels,
        "source_head_backdrop_removed_pixels": source_head_backdrop_removed_pixels,
        "source_head_backdrop_component_count": source_head_backdrop_component_count,
        "source_head_end": source_head_end,
        "light_backdrop_removed_pixels": light_backdrop_removed_pixels,
        "light_backdrop_component_count": light_backdrop_component_count,
        "light_backdrop_end": light_backdrop_end,
        "source_neck_removed_pixels": source_neck_removed_pixels,
        "source_neck_margin": source_neck_margin,
        "source_neck_end": source_neck_end,
        "source_torso_removed_pixels": source_torso_removed_pixels,
        "source_torso_margin": source_torso_margin,
        "source_torso_end": source_torso_end,
        "removed_subject_ratio": round(
            removed_pixels / subject_area, 6
        ),
    })
    return refined, report


def _synchronize_geometry_reference_alpha(job: Job) -> None:
    """Give the sculptural RGB reference the pipeline's hard subject silhouette.

    Some OpenAI-compatible image endpoints render a checkerboard into an opaque
    RGB result even when transparent output was requested.  The printable-image
    pipeline already recovers a clean, connected binary subject mask.  Prefer
    that mask over ``model_reference.png`` alpha: the latter can intentionally
    retain a soft portrait shadow which single-view image-to-3D may extrude into
    a person-shaped rear plate.  Reusing only the hard silhouette preserves the
    provider's sculptural face planes while preventing checkerboards, shadows or
    tiny detached fragments from reaching image-to-3D.

    This runs during assessment as well as initial preprocessing so restored
    Beta jobs created by an older sidecar are repaired before a paid task can be
    submitted.
    """

    geometry_reference = job.geometry_reference_path
    model_reference = job.model_reference_path
    subject_mask = job.subject_mask_path
    if (
        not _identity_preserving_portrait_geometry_enabled(job)
        or geometry_reference is None
        or model_reference is None
        or not geometry_reference.is_file()
        or not model_reference.is_file()
    ):
        return
    previous_cleanup = job.image_metrics.get("geometry_silhouette_cleanup", {})
    if (
        isinstance(previous_cleanup, Mapping)
        and previous_cleanup.get("version") == "portrait-silhouette-v6"
        and previous_cleanup.get("alpha_synced") is True
    ):
        cached_cleanup = dict(previous_cleanup)
        cached_cleanup["revalidated"] = True
        job.image_metrics["geometry_silhouette_cleanup"] = cached_cleanup
        return
    try:
        from PIL import Image, ImageChops, UnidentifiedImageError
    except ImportError:
        raise ModelInputImageQualityError(
            "Pillow is required to prepare the portrait geometry reference."
        ) from None

    temporary = geometry_reference.with_name(
        f"{geometry_reference.name}.{uuid.uuid4().hex}.alpha.tmp"
    )
    try:
        mask_path = (
            subject_mask
            if subject_mask is not None and subject_mask.is_file()
            else model_reference
        )
        with Image.open(geometry_reference) as geometry_opened, Image.open(mask_path) as mask_opened:
            if geometry_opened.size != mask_opened.size:
                raise ModelInputImageQualityError(
                    "The portrait geometry reference and validated silhouette have different sizes."
                )
            validated_alpha = (
                mask_opened.convert("L")
                if mask_path == subject_mask
                else mask_opened.getchannel("A")
                if "A" in mask_opened.getbands()
                else None
            )
            if validated_alpha is None:
                return
            # A hard edge is intentional here. Semi-transparent antialiasing or
            # a portrait drop shadow is useful for 2D preview, but is ambiguous
            # geometry evidence and can become a printable vertical sheet.
            geometry = geometry_opened.convert("RGBA")
            validated_alpha = validated_alpha.point(lambda value: 255 if value >= 128 else 0)
            # Never re-open pixels removed by an earlier cleanup. Restored jobs
            # can be assessed repeatedly (startup, UI download, paid submit),
            # and replacing alpha with the original broad mask would otherwise
            # turn the sanitized transparent-black halo back into opaque black
            # geometry on the next pass.
            current_alpha = geometry.getchannel("A").point(
                lambda value: 255 if value >= 128 else 0
            )
            validated_alpha = ImageChops.darker(validated_alpha, current_alpha)
            already_refined = (
                isinstance(previous_cleanup, Mapping)
                and previous_cleanup.get("version") == "portrait-silhouette-v6"
                and previous_cleanup.get("status") in {"refined", "not_needed"}
            )
            if already_refined:
                silhouette_report = dict(previous_cleanup)
                silhouette_report["revalidated"] = True
            else:
                source_alpha = None
                if job.input_path is not None and job.input_path.is_file():
                    source_mask_data = _portrait_source_subject_mask(
                        geometry.width,
                        geometry.height,
                        job.input_path,
                    )
                    if source_mask_data:
                        source_alpha = Image.frombytes("L", geometry.size, source_mask_data)
                validated_alpha, silhouette_report = _refine_portrait_head_silhouette(
                    geometry,
                    validated_alpha,
                    source_alpha,
                )
            geometry.putalpha(validated_alpha)

            # Alpha alone is not sufficient for provider interoperability.
            # Some upload/conversion paths flatten PNGs after discarding alpha,
            # exposing the checkerboard or white RGB values that were hidden in
            # transparent pixels. Image-to-3D can then extrude that hidden image
            # into a large rear plate. Keep the complete subject unchanged, but
            # make every non-subject pixel transparent black so both alpha-aware
            # and RGB-only decoders see an unambiguous empty background.
            sanitized = Image.new("RGBA", geometry.size, (0, 0, 0, 0))
            sanitized.paste(geometry, (0, 0), validated_alpha)
            if geometry_opened.convert("RGBA").tobytes() == sanitized.tobytes():
                silhouette_report["alpha_synced"] = True
                job.image_metrics["geometry_silhouette_cleanup"] = silhouette_report
                return
            sanitized.save(temporary, format="PNG")
        os.replace(temporary, geometry_reference)
        silhouette_report["alpha_synced"] = True
        job.image_metrics["geometry_silhouette_cleanup"] = silhouette_report
    except ModelInputImageQualityError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
        raise ModelInputImageQualityError(
            "The sculptural portrait reference could not inherit the validated subject silhouette."
        ) from None
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _copy_portrait_into_continuous_silhouette(
    portrait: Any,
    current_alpha: Any,
    target_alpha: Any,
) -> Any:
    """Keep valid portrait pixels exact and synthesize only repaired mask gaps.

    A paid image-to-3D request must never expose transparent holes or the RGB
    checkerboard hidden behind them.  Pixels already belonging to the validated
    subject are copied byte-for-byte (apart from hardening alpha).  A newly
    filled gap inherits the nearest valid pixel on the same scanline, which is
    deliberately less imaginative than inpainting and cannot alter the face.
    """

    from bisect import bisect_left
    from PIL import Image

    if portrait.size != current_alpha.size or portrait.size != target_alpha.size:
        raise ModelInputImageQualityError(
            "The portrait silhouette repair images have different sizes."
        )
    source = portrait.convert("RGBA")
    current = current_alpha.point(lambda value: 255 if value >= 128 else 0)
    target = target_alpha.point(lambda value: 255 if value >= 128 else 0)
    source_pixels = source.load()
    current_pixels = current.load()
    target_pixels = target.load()
    repaired = Image.new("RGBA", source.size, (0, 0, 0, 0))
    repaired_pixels = repaired.load()
    interior_offset = max(2, min(6, round(source.width * 0.008)))
    for y in range(source.height):
        valid = [
            x
            for x in range(source.width)
            if current_pixels[x, y] >= 128 and target_pixels[x, y] >= 128
        ]
        if not valid:
            continue
        for x in range(source.width):
            if target_pixels[x, y] < 128:
                continue
            if current_pixels[x, y] >= 128:
                red, green, blue, _ = source_pixels[x, y]
            else:
                insertion = bisect_left(valid, x)
                if insertion == 0:
                    sample_index = min(len(valid) - 1, interior_offset)
                elif insertion == len(valid):
                    sample_index = max(0, len(valid) - 1 - interior_offset)
                else:
                    left_index = insertion - 1
                    right_index = insertion
                    if x - valid[left_index] <= valid[right_index] - x:
                        sample_index = max(0, left_index - interior_offset)
                    else:
                        sample_index = min(
                            len(valid) - 1, right_index + interior_offset
                        )
                red, green, blue, _ = source_pixels[valid[sample_index], y]
            repaired_pixels[x, y] = (red, green, blue, 255)
    return repaired


def _repair_portrait_head_shoulders_silhouette(
    portrait: Any,
    portrait_alpha: Any,
    source_alpha: Any | None,
    *,
    protected_bounds: tuple[int, int, int, int] | None = None,
) -> tuple[Any, Any, dict[str, Any]]:
    """Remove exterior checkerboard remnants and close shoulder/neck notches.

    The compact portrait is intentionally a single frontal bust silhouette.
    This lets us use three independent pieces of evidence before a paid task:
    the generated hard alpha, the composition-locked source-photo silhouette,
    and real sculptural texture (as opposed to near-white checkerboard RGB).
    Each accepted scanline is made continuous only between its trusted left and
    right edges.  Thus exterior ghosts disappear, interior square cutouts close,
    and every already-valid identity RGB pixel remains untouched.
    """

    try:
        from PIL import Image, ImageChops, ImageFilter
    except ImportError:
        raise ModelInputImageQualityError(
            "Pillow is required to repair the portrait shoulder silhouette."
        ) from None

    report: dict[str, Any] = {
        "version": "portrait-head-shoulders-silhouette-v1",
        "status": "unverified",
        "source_mask_used": False,
        "removed_external_pixels": 0,
        "removed_light_edge_pixels": 0,
        "filled_notch_pixels": 0,
        "remaining_row_gap_pixels": 0,
        "protected_pixels_removed": 0,
        "protected_removed_ratio": 0.0,
        "central_identity_pixels_removed": 0,
        "identity_rgb_pixels_changed": 0,
    }
    current = portrait_alpha.point(lambda value: 255 if value >= 128 else 0)
    current_area = sum(value >= 128 for value in current.tobytes())
    if current_area <= 0:
        report["reason"] = "empty_portrait_silhouette"
        return portrait, current, report
    if source_alpha is None or source_alpha.size != portrait.size:
        report["reason"] = "source_portrait_silhouette_unavailable"
        return portrait, current, report

    source = source_alpha.point(lambda value: 255 if value >= 128 else 0)
    # Allow a few pixels of generator/source alignment drift while still using
    # the source to reject large attached remnants. MaxFilter requires an odd
    # kernel and is bounded so it cannot turn the source into a broad halo.
    alignment_margin = max(3, min(11, round(min(portrait.size) * 0.008) | 1))
    source = source.filter(ImageFilter.MaxFilter(alignment_margin))
    bounded = ImageChops.darker(current, source)
    bounded_pixels = bounded.load()
    portrait_pixels = portrait.convert("RGBA").load()
    target = Image.new("L", portrait.size, 0)
    target_pixels = target.load()
    trusted_rows = 0
    fallback_rows = 0
    removed_light_edge_pixels = 0
    edge_margin = max(1, round(min(portrait.size) * 0.003))
    proposed_bounds: dict[int, tuple[int, int]] = {}
    for y in range(portrait.height):
        candidate = [x for x in range(portrait.width) if bounded_pixels[x, y] >= 128]
        if not candidate:
            continue
        textured: list[int] = []
        for x in candidate:
            red, green, blue, _ = portrait_pixels[x, y]
            # Generated checkerboards and their white matte are nearly neutral
            # and much brighter than even the light jacket's shaded edge.
            light_neutral_matte = (
                min(red, green, blue) >= 242
                and max(red, green, blue) - min(red, green, blue) <= 12
            )
            if not light_neutral_matte:
                textured.append(x)
        if textured:
            left = max(candidate[0], textured[0] - edge_margin)
            right = min(candidate[-1], textured[-1] + edge_margin)
            removed_light_edge_pixels += sum(
                x < left or x > right for x in candidate
            )
            trusted_rows += 1
        else:
            # Keep a very bright hair/highlight scanline when both independent
            # masks agree; adjacent textured rows still constrain its extent.
            left, right = candidate[0], candidate[-1]
            fallback_rows += 1
        proposed_bounds[y] = (left, right)

    # Below the narrowest neck row, a frontal bust must widen smoothly toward
    # the shoulders. Collar/background fragments can otherwise create a sudden
    # square inward step even when each scanline is individually continuous.
    neck_search_top = round(portrait.height * 0.38)
    neck_search_bottom = round(portrait.height * 0.62)
    neck_candidates = [
        y for y in proposed_bounds if neck_search_top <= y < neck_search_bottom
    ]
    neck_row = (
        min(
            neck_candidates,
            key=lambda y: proposed_bounds[y][1] - proposed_bounds[y][0],
        )
        if neck_candidates
        else None
    )
    maximum_expansion = max(2, round(portrait.width * 0.006))
    boundary_adjustment_pixels = 0
    if neck_row is not None:
        previous_left, previous_right = proposed_bounds[neck_row]
        for y in range(neck_row + 1, portrait.height):
            if y not in proposed_bounds:
                continue
            raw_left, raw_right = proposed_bounds[y]
            left = max(
                min(raw_left, previous_left),
                previous_left - maximum_expansion,
            )
            right = min(
                max(raw_right, previous_right),
                previous_right + maximum_expansion,
            )
            boundary_adjustment_pixels += abs(left - raw_left) + abs(right - raw_right)
            proposed_bounds[y] = (left, right)
            previous_left, previous_right = left, right
    for y, (left, right) in proposed_bounds.items():
        for x in range(left, right + 1):
            target_pixels[x, y] = 255

    target_area = sum(value >= 128 for value in target.tobytes())
    current_pixels = current.load()
    removed_external = 0
    filled_notches = 0
    remaining_gaps = 0
    for y in range(portrait.height):
        row = [x for x in range(portrait.width) if target_pixels[x, y] >= 128]
        if row:
            remaining_gaps += sum(
                target_pixels[x, y] < 128 for x in range(row[0], row[-1] + 1)
            )
        for x in range(portrait.width):
            was_subject = current_pixels[x, y] >= 128
            is_subject = target_pixels[x, y] >= 128
            removed_external += int(was_subject and not is_subject)
            filled_notches += int(is_subject and not was_subject)

    protected_count = 0
    protected_removed = 0
    central_identity_removed = 0
    central_removed_left = portrait.width
    central_removed_top = portrait.height
    central_removed_right = -1
    central_removed_bottom = -1
    if protected_bounds is not None:
        left, top, right, bottom = protected_bounds
        left = max(0, min(portrait.width, left))
        right = max(left, min(portrait.width, right))
        top = max(0, min(portrait.height, top))
        bottom = max(top, min(portrait.height, bottom))
        protected_width = right - left
        protected_height = bottom - top
        central_left = left + round(protected_width * 0.12)
        central_right = right - round(protected_width * 0.12)
        central_top = top + round(protected_height * 0.08)
        # Protect the eyes/nose/mouth interior absolutely. The lower part of the
        # detector's broad warm-skin box includes neck and collar pixels, where
        # source-aligned trimming is precisely what removes the square tabs.
        central_bottom = top + round(protected_height * 0.58)
        for y in range(top, bottom):
            for x in range(left, right):
                if current_pixels[x, y] >= 128:
                    protected_count += 1
                    protected_removed += int(target_pixels[x, y] < 128)
                    if (
                        central_left <= x < central_right
                        and central_top <= y < central_bottom
                        and target_pixels[x, y] < 128
                    ):
                        central_identity_removed += 1
                        central_removed_left = min(central_removed_left, x)
                        central_removed_top = min(central_removed_top, y)
                        central_removed_right = max(central_removed_right, x)
                        central_removed_bottom = max(central_removed_bottom, y)

    area_ratio = target_area / max(1, current_area)
    protected_removed_ratio = protected_removed / max(1, protected_count)
    verified = (
        trusted_rows >= max(16, round(portrait.height * 0.45))
        and fallback_rows <= round(portrait.height * 0.20)
        and 0.72 <= area_ratio <= 1.10
        and removed_external <= current_area * 0.22
        and filled_notches <= current_area * 0.12
        and protected_removed_ratio <= 0.04
        and central_identity_removed == 0
        and remaining_gaps == 0
    )
    report.update({
        "status": "pass" if verified else "unverified",
        "reason": "continuous_source_aligned_silhouette" if verified else "unsafe_repair_extent",
        "source_mask_used": True,
        "source_alignment_margin_px": alignment_margin,
        "trusted_rows": trusted_rows,
        "fallback_rows": fallback_rows,
        "neck_row": neck_row,
        "maximum_shoulder_expansion_px": maximum_expansion,
        "shoulder_boundary_adjustment_pixels": boundary_adjustment_pixels,
        "current_area": current_area,
        "target_area": target_area,
        "target_to_current_ratio": round(area_ratio, 6),
        "removed_external_pixels": removed_external,
        "removed_light_edge_pixels": removed_light_edge_pixels,
        "filled_notch_pixels": filled_notches,
        "remaining_row_gap_pixels": remaining_gaps,
        "protected_pixels_removed": protected_removed,
        "protected_removed_ratio": round(protected_removed_ratio, 6),
        "central_identity_pixels_removed": central_identity_removed,
        "central_identity_removed_bounds": (
            [
                central_removed_left,
                central_removed_top,
                central_removed_right + 1,
                central_removed_bottom + 1,
            ]
            if central_identity_removed
            else None
        ),
    })
    if not verified:
        return portrait, current, report
    repaired = _copy_portrait_into_continuous_silhouette(
        portrait, current, target
    )
    return repaired, target, report


def _prepare_portrait_geometry_provider_reference(job: Job) -> Path:
    """Build an identity-preserving sculptural portrait on a safe square canvas.

    The portrait Image2 endpoint returns a 2:3 image.  In real Tripo runs that
    tall transparent canvas repeatedly produced a thin loop behind the head and
    a full-height rear plate, even after every hidden transparent RGB pixel was
    cleared. A subsequent 1.89M-face real run proved that a colour-photo-like
    half-body input can still collapse a faithful 2D face into a generic 3D one.
    Geometry therefore follows the relief-rich sculptural reference; the natural
    colour reference remains the authority for post-generation materials.

    When reliable face and base bounds are available, crop at the shoulders,
    preserve every remaining portrait pixel at native resolution, and draw one
    solid overlapping plinth. This raises the head's share of the actual square
    provider input above one half and removes the copied white transition that
    created a second base ring. Center the result on transparent black so no
    portrait frame can be interpreted as geometry.

    The original ``geometry-reference.png`` remains untouched after alpha
    sanitization.  Keeping the provider-specific derivative separate makes the
    operation idempotent and lets restored jobs rebuild it safely.
    """

    source = job.geometry_reference_path
    if source is None or not source.is_file():
        raise ModelInputImageQualityError("The sculptural portrait reference is unavailable.")
    try:
        from PIL import Image, ImageDraw, UnidentifiedImageError
    except ImportError:
        raise ModelInputImageQualityError(
            "Pillow is required to prepare the portrait geometry provider image."
        ) from None

    destination = job.directory / PORTRAIT_GEOMETRY_PROVIDER_FILENAME
    temporary = destination.with_name(
        f"{destination.name}.{uuid.uuid4().hex}.tmp"
    )
    previous_canvas = job.image_metrics.get("geometry_provider_canvas", {})
    # A provider image is part of a paid request's immutable evidence.  Status
    # polling and restart recovery call this chooser again, so a newer layout
    # strategy must never rewrite the image that an existing Tripo task saw.
    if job.attempts and destination.is_file() and isinstance(previous_canvas, Mapping):
        frozen_size = previous_canvas.get("output_size")
        if (
            isinstance(frozen_size, list)
            and len(frozen_size) == 2
            and all(isinstance(value, int) and value > 0 for value in frozen_size)
        ):
            try:
                with Image.open(destination) as frozen:
                    if frozen.size == tuple(frozen_size) and "A" in frozen.getbands():
                        return destination
            except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
                pass
    try:
        with Image.open(source) as opened:
            sculptural_geometry = opened.convert("RGBA")
        alpha = sculptural_geometry.getchannel("A")
        geometry = sculptural_geometry
        appearance_source = "sculptural_geometry_reference"
        appearance_path = source
        original_size = list(geometry.size)
        compaction: dict[str, Any] = {
            "applied": False,
            "reason": "not_eligible",
            "original_size": original_size,
        }
        cleanup = job.image_metrics.get("portrait_skin_cleanup", {})
        face_bounds = cleanup.get("face_bounds") if isinstance(cleanup, Mapping) else None
        base_bounds = cleanup.get("base_bounds") if isinstance(cleanup, Mapping) else None
        if isinstance(face_bounds, Mapping) and isinstance(base_bounds, Mapping):
            try:
                face_left = int(face_bounds["left"])
                face_right = int(face_bounds["right"])
                face_top = int(face_bounds["top"])
                face_bottom = int(face_bounds["bottom"])
                base_top = int(base_bounds["top"])
            except (KeyError, TypeError, ValueError, OverflowError):
                face_left = face_right = face_top = face_bottom = base_top = -1
            width, height = geometry.size
            face_width = face_right - face_left
            skin_component_height = face_bottom - face_top
            subject_bbox = alpha.getbbox()
            # A connected warm component can extend through the neck. Face
            # width is the more stable scale cue, so cap the estimated head at
            # a conservative real-portrait aspect before choosing a shoulder
            # cut. No source portrait pixel is resampled.
            head_height = min(skin_component_height, round(face_width * 1.55))
            if (
                subject_bbox is not None
                and face_width >= round(width * 0.16)
                and head_height >= round(height * 0.22)
                and 0 <= face_left < face_right <= width
                and 0 <= face_top < face_bottom <= base_top <= height
            ):
                center_x = (face_left + face_right) // 2
                crop_top = max(
                    subject_bbox[1],
                    face_top - round(head_height * 0.14),
                )
                head_bottom = min(face_bottom, face_top + head_height)
                crop_bottom = min(
                    base_top,
                    head_bottom + round(head_height * 0.50),
                )
                crop_width = min(
                    width,
                    max(face_width * 2, round(head_height * 1.66)),
                )
                crop_left = max(0, min(width - crop_width, center_x - crop_width // 2))
                crop_right = crop_left + crop_width
                if crop_bottom <= crop_top + head_height:
                    compaction["reason"] = "portrait_shoulders_not_available"
                else:
                    portrait = geometry.crop((crop_left, crop_top, crop_right, crop_bottom))
                    portrait_source_alpha = portrait.getchannel("A").point(
                        lambda value: 255 if value >= 128 else 0
                    )
                    source_portrait_alpha = None
                    if job.input_path is not None and job.input_path.is_file():
                        source_mask_data = _portrait_source_subject_mask(
                            width, height, job.input_path
                        )
                        if source_mask_data:
                            source_portrait_alpha = Image.frombytes(
                                "L", (width, height), source_mask_data
                            ).crop((crop_left, crop_top, crop_right, crop_bottom))
                    portrait, portrait_alpha, shoulder_silhouette = (
                        _repair_portrait_head_shoulders_silhouette(
                            portrait,
                            portrait_source_alpha,
                            source_portrait_alpha,
                            protected_bounds=(
                                face_left - crop_left,
                                face_top - crop_top,
                                face_right - crop_left,
                                face_bottom - crop_top,
                            ),
                        )
                    )
                    lower_start = round(portrait.height * 0.72)
                    lower_bbox = portrait_alpha.crop(
                        (0, lower_start, portrait.width, portrait.height)
                    ).getbbox()
                    lower_width = (
                        lower_bbox[2] - lower_bbox[0]
                        if lower_bbox is not None
                        else face_width * 2
                    )
                    base_height = max(28, round(head_height * 0.13))
                    overlap = max(5, round(head_height * 0.025))
                    bottom_margin = max(4, round(head_height * 0.015))
                    prepared_height = portrait.height + base_height - overlap + bottom_margin
                    prepared = Image.new(
                        "RGBA", (portrait.width, prepared_height), (0, 0, 0, 0)
                    )
                    base_width = min(
                        portrait.width - bottom_margin * 2,
                        max(round(head_height * 1.60), round(lower_width * 1.12)),
                    )
                    relative_center_x = center_x - crop_left
                    base_left = max(
                        bottom_margin,
                        min(
                            portrait.width - bottom_margin - base_width,
                            relative_center_x - base_width // 2,
                        ),
                    )
                    base_right = base_left + base_width
                    base_destination_top = portrait.height - overlap
                    structure = str(job.palette_roles.get("structure", "#555555")).strip()
                    if re.fullmatch(r"#[0-9A-Fa-f]{6}", structure):
                        plinth_rgb = tuple(
                            int(structure[index:index + 2], 16) for index in (1, 3, 5)
                        )
                    else:
                        plinth_rgb = (85, 85, 85)
                    prepared.paste(portrait, (0, 0), portrait_alpha)
                    draw = ImageDraw.Draw(prepared)
                    ellipse_height = max(8, round(base_height * 0.36))
                    draw.ellipse(
                        (
                            base_left,
                            base_destination_top,
                            base_right - 1,
                            base_destination_top + ellipse_height,
                        ),
                        fill=(*plinth_rgb, 255),
                    )
                    draw.rectangle(
                        (
                            base_left,
                            base_destination_top + ellipse_height // 2,
                            base_right - 1,
                            base_destination_top + base_height - ellipse_height // 2,
                        ),
                        fill=(*plinth_rgb, 255),
                    )
                    draw.ellipse(
                        (
                            base_left,
                            base_destination_top + base_height - ellipse_height,
                            base_right - 1,
                            base_destination_top + base_height,
                        ),
                        fill=(*plinth_rgb, 255),
                    )
                    # The one-piece plinth overlaps the native shoulder cut.
                    # This hides antialiased garment fringe and prevents a
                    # second white transition ring; the face remains untouched.
                    geometry = prepared
                    alpha = geometry.getchannel("A")
                    compaction = {
                        "applied": True,
                        "reason": "portrait_head_shoulders_identity",
                        "original_size": original_size,
                        "prepared_size": list(geometry.size),
                        "crop_bounds": [crop_left, crop_top, crop_right, crop_bottom],
                        "removed_original_base": True,
                        "head_height": head_height,
                        "head_bounds": [face_left, crop_top, face_right, head_bottom],
                        "base_source": "single_solid_structure_plinth",
                        "base_bounds": [
                            base_left,
                            base_destination_top,
                            base_right,
                            base_destination_top + base_height,
                        ],
                        "base_overlap": overlap,
                        "base_ellipse_height": ellipse_height,
                        "base_color": "#{:02X}{:02X}{:02X}".format(*plinth_rgb),
                        "shoulder_silhouette": shoulder_silhouette,
                        "face_canvas_ratio_before": round(
                            head_height / max(1, height), 6
                        ),
                        "face_prepared_ratio": round(
                            head_height / max(1, geometry.height), 6
                        ),
                        "identity_pixels_resampled": False,
                    }
            else:
                compaction["reason"] = "portrait_not_safely_head_croppable"
        subject_bbox = alpha.getbbox()
        if subject_bbox is None:
            raise ModelInputImageQualityError(
                "The sculptural portrait reference has no visible subject."
            )
        subject_width = subject_bbox[2] - subject_bbox[0]
        subject_height = subject_bbox[3] - subject_bbox[1]
        width, height = geometry.size
        maximum_occupancy = (
            PORTRAIT_HEAD_GEOMETRY_MAX_SUBJECT_OCCUPANCY
            if compaction["applied"]
            else PORTRAIT_GEOMETRY_MAX_SUBJECT_OCCUPANCY
        )
        target_side = max(
            width,
            height,
            int(math.ceil(subject_width / maximum_occupancy)),
            int(math.ceil(subject_height / maximum_occupancy)),
        )
        offset_x = (target_side - width) // 2
        offset_y = (target_side - height) // 2
        canvas_version = (
            "square-transparent-black-head-shoulders-v9"
            if compaction["applied"]
            else "square-transparent-black-v2"
        )
        if compaction["applied"]:
            compaction["face_provider_ratio"] = round(
                int(compaction["head_height"]) / max(1, target_side), 6
            )
        job.image_metrics["geometry_provider_canvas"] = {
            "version": canvas_version,
            "source_size": original_size,
            "prepared_size": [width, height],
            "output_size": [target_side, target_side],
            "subject_bbox": list(subject_bbox),
            "subject_occupancy": round(
                max(subject_width, subject_height) / max(1, target_side), 6
            ),
            "offset": [offset_x, offset_y],
            "appearance_source": appearance_source,
            "portrait_compaction": compaction,
        }
        if compaction["applied"] and job.preview_path is not None:
            printable_destination = job.directory / PORTRAIT_HEAD_PREVIEW_FILENAME
            preview_source = job.preview_path
            previous_provider_preview = job.image_metrics.get(
                "portrait_provider_preview", {}
            )
            if preview_source.resolve() == printable_destination.resolve():
                # A restored older job points preview_path at the already-cropped
                # derivative. Rebuild v6 from the pipeline's lossless full-size
                # clean preview instead of trying to crop the crop or silently
                # leaving its old shoulder defect in place.
                for candidate in (
                    job.directory / "clean_preview.png",
                    job.strict_preview_path,
                ):
                    if candidate is None or not candidate.is_file():
                        continue
                    try:
                        with Image.open(candidate) as candidate_opened:
                            if candidate_opened.size == tuple(original_size):
                                preview_source = candidate
                                break
                    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
                        continue
            if preview_source.resolve() != printable_destination.resolve():
                with Image.open(preview_source) as preview_opened:
                    printable = preview_opened.convert("RGBA")
                if printable.size != tuple(original_size):
                    raise ModelInputImageQualityError(
                        "The printable portrait preview does not match the sculptural reference."
                    )
                crop_left, crop_top, crop_right, crop_bottom = (
                    int(value) for value in compaction["crop_bounds"]
                )
                printable_portrait = printable.crop(
                    (crop_left, crop_top, crop_right, crop_bottom)
                )
                # Geometry alpha remains the silhouette authority. The exact
                # printable colors are clipped to that silhouette so the image
                # users approve describes the same head-and-shoulders object.
                printable_portrait = _copy_portrait_into_continuous_silhouette(
                    printable_portrait,
                    portrait_source_alpha,
                    portrait_alpha,
                )
                printable_prepared = Image.new(
                    "RGBA", (width, height), (0, 0, 0, 0)
                )
                printable_prepared.paste(
                    printable_portrait,
                    (0, 0),
                    printable_portrait.getchannel("A"),
                )
                base_left, base_top, base_right, base_bottom = (
                    int(value) for value in compaction["base_bounds"]
                )
                base_color = str(compaction["base_color"])
                base_rgb = tuple(
                    int(base_color[index:index + 2], 16) for index in (1, 3, 5)
                )
                ellipse_height = int(compaction["base_ellipse_height"])
                preview_draw = ImageDraw.Draw(printable_prepared)
                preview_draw.ellipse(
                    (base_left, base_top, base_right - 1, base_top + ellipse_height),
                    fill=(*base_rgb, 255),
                )
                preview_draw.rectangle(
                    (
                        base_left,
                        base_top + ellipse_height // 2,
                        base_right - 1,
                        base_bottom - ellipse_height // 2,
                    ),
                    fill=(*base_rgb, 255),
                )
                preview_draw.ellipse(
                    (
                        base_left,
                        base_bottom - ellipse_height,
                        base_right - 1,
                        base_bottom,
                    ),
                    fill=(*base_rgb, 255),
                )
                printable_canvas = Image.new(
                    "RGBA", (target_side, target_side), (0, 0, 0, 0)
                )
                printable_canvas.paste(
                    printable_prepared,
                    (offset_x, offset_y),
                    printable_prepared.getchannel("A"),
                )
                printable_temporary = printable_destination.with_name(
                    f"{printable_destination.name}.{uuid.uuid4().hex}.tmp"
                )
                try:
                    printable_canvas.save(printable_temporary, format="PNG")
                    os.replace(printable_temporary, printable_destination)
                finally:
                    printable_temporary.unlink(missing_ok=True)
            elif (
                not printable_destination.is_file()
                or not isinstance(previous_provider_preview, Mapping)
                or previous_provider_preview.get("version")
                != "portrait-head-shoulders-preview-v6"
            ):
                raise ModelInputImageQualityError(
                    "The prepared portrait preview is unavailable."
                )
            job.preview_path = printable_destination
            job.image_metrics["portrait_provider_preview"] = {
                "version": "portrait-head-shoulders-preview-v6",
                "path": printable_destination.name,
                "output_size": [target_side, target_side],
                "matches_geometry_crop": True,
                "single_material_base": True,
                "continuous_silhouette": bool(
                    isinstance(compaction.get("shoulder_silhouette"), Mapping)
                    and compaction["shoulder_silhouette"].get("status") == "pass"
                ),
            }
        if (
            width == target_side
            and height == target_side
            and appearance_source == "sculptural_geometry_reference"
        ):
            destination.unlink(missing_ok=True)
            return source

        # Public status is polled every few seconds during a long remote job.
        # Re-encoding the unchanged 1–2K portrait on every GET adds avoidable
        # CPU, disk churn and file-lock exposure.  A provider derivative newer
        # than its immutable sanitized source and with the expected canvas is
        # already authoritative for this job.
        if destination.is_file():
            try:
                source_revision = max(
                    source.stat().st_mtime_ns,
                    appearance_path.stat().st_mtime_ns,
                )
                cache_matches = (
                    isinstance(previous_canvas, Mapping)
                    and previous_canvas.get("version") == canvas_version
                    and previous_canvas.get("appearance_source") == appearance_source
                )
                if cache_matches and destination.stat().st_mtime_ns >= source_revision:
                    with Image.open(destination) as cached:
                        if cached.size == (target_side, target_side) and "A" in cached.getbands():
                            return destination
            except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
                pass

        canvas = Image.new("RGBA", (target_side, target_side), (0, 0, 0, 0))
        canvas.paste(geometry, (offset_x, offset_y), geometry.getchannel("A"))
        canvas.save(temporary, format="PNG")
        os.replace(temporary, destination)
        return destination
    except ModelInputImageQualityError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
        raise ModelInputImageQualityError(
            "The sculptural portrait reference could not be normalized for image-to-3D."
        ) from None
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _assess_reference_advice(job: Job) -> tuple[dict[str, Any], dict[str, Any]]:
    """Retain cheap image findings without making heuristic failures a gate.

    Image decoding and provider format/size validation stay at the input
    boundary. An unavailable quality assessment is itself only advice.
    """
    reports = []
    for key, assess in (
        ("model_input_quality", _assess_job_model_reference),
        ("generation_input_quality", _assess_job_generation_reference),
    ):
        try:
            report = assess(job)
        except ModelInputImageQualityError:
            report = {
                "model_input_eligible": True,
                "warnings": ["reference_quality_unavailable"],
                "blockers": [],
                "status": "unavailable",
            }
            job.image_metrics[key] = report
        reports.append(report)
    return reports[0], reports[1]


def _assess_job_generation_reference(job: Job) -> dict[str, Any]:
    reference = _geometry_generation_reference(job)
    if reference is None:
        raise ModelInputImageQualityError("The model generation image is unavailable.")
    quality = assess_model_input_image(
        reference,
        reject_rectangular_cutouts=_identity_preserving_portrait_geometry_enabled(job),
    )
    if job.image_metrics.get("design_reference") == "ai-design-v1":
        strategy = "ai_design"
    elif _identity_preserving_portrait_geometry_enabled(job):
        provider_reference = job.directory / PORTRAIT_GEOMETRY_PROVIDER_FILENAME
        provider_canvas = job.image_metrics.get("geometry_provider_canvas", {})
        color_provider = (
            reference == provider_reference
            and isinstance(provider_canvas, Mapping)
            and provider_canvas.get("appearance_source")
            == "identity_color_model_reference"
        )
        strategy = (
            "identity_color_geometry_reference"
            if color_provider
            else "identity_sculpted_geometry_reference"
            if reference == job.geometry_reference_path or reference == provider_reference
            else "identity_locked_model_reference"
            if reference != job.input_path
            else "original_identity_image"
        )
    elif reference == job.raw_preview_path and bool(job.palette):
        strategy = "raw_preview"
    else:
        strategy = "model_reference"
    # Generic cutout analysis downscales the reference and detects enclosed
    # rectangles well, but an exterior shoulder notch is open to the background
    # and can disappear at that resolution. The portrait-specific native-size
    # repair is therefore an independent, paid-task hard gate. Existing paid
    # attempts keep their immutable evidence and are not retroactively rejected.
    if identity_locked := strategy in {
        "identity_color_geometry_reference",
        "identity_sculpted_geometry_reference",
        "identity_locked_model_reference",
        "original_identity_image",
    }:
        provider_canvas = job.image_metrics.get("geometry_provider_canvas", {})
        compaction = (
            provider_canvas.get("portrait_compaction", {})
            if isinstance(provider_canvas, Mapping)
            else {}
        )
        shoulder_silhouette = (
            compaction.get("shoulder_silhouette", {})
            if isinstance(compaction, Mapping)
            else {}
        )
        compact_portrait = bool(
            isinstance(compaction, Mapping) and compaction.get("applied") is True
        )
        silhouette_verified = bool(
            isinstance(shoulder_silhouette, Mapping)
            and shoulder_silhouette.get("status") == "pass"
            and shoulder_silhouette.get("source_mask_used") is True
            and int(shoulder_silhouette.get("remaining_row_gap_pixels", -1)) == 0
        )
        if compact_portrait and not job.attempts and not silhouette_verified:
            existing = quality.get("blockers", [])
            blocker_codes = (
                [str(code) for code in existing]
                if isinstance(existing, list)
                else []
            )
            quality["blockers"] = list(dict.fromkeys(
                ["portrait_shoulder_silhouette_unverified"] + blocker_codes
            ))
            quality["model_input_eligible"] = False
            quality["score"] = min(float(quality.get("score", 100)), 60.0)
    job.image_metrics["generation_input_quality"] = quality
    job.image_metrics["generation_reference"] = strategy
    job.image_metrics["geometry_strategy"] = {
        "version": (
            "portrait-sculpted-head-shoulders-front-v15"
            if strategy == "identity_sculpted_geometry_reference"
            and isinstance(job.image_metrics.get("geometry_provider_canvas"), Mapping)
            and job.image_metrics["geometry_provider_canvas"].get("version")
            == "square-transparent-black-head-shoulders-v9"
            else "portrait-identity-color-front-v8"
            if strategy == "identity_color_geometry_reference"
            else
            "portrait-identity-sculpted-front-v7"
            if strategy == "identity_sculpted_geometry_reference"
            else "portrait-identity-front-v4"
            if identity_locked
            else "default-v1"
        ),
        "reference": strategy,
        "multiview_geometry": False if identity_locked else None,
        "post_generation_material_turntable": True if identity_locked else None,
        "identity_review_reference": strategy if identity_locked else None,
        "provider_autofix": False if identity_locked else None,
    }
    return quality


def _apply_preview_visual_quality_gate(job: Job, report: Mapping[str, Any]) -> list[str]:
    """Merge a live or persisted visual decision into both paid-input gates."""

    for key in ("model_input_quality", "generation_input_quality"):
        quality = job.image_metrics.get(key)
        if isinstance(quality, dict):
            warning = "reference_visual_review_unavailable"
            warnings = [code for code in quality.get("warnings", []) if code != warning]
            if report.get("status") == "unavailable":
                warnings.append(warning)
            quality["warnings"] = warnings
    blockers = [str(code) for code in report.get("blocking_warnings", [])]
    if bool(report.get("model_generation_recommended", True)) or not blockers:
        return []
    priority = (
        "preview_identity_mismatch",
        "preview_face_geometry_drift",
        "preview_age_expression_drift",
        "preview_material_mixing",
        "preview_base_mixing",
        "preview_pose_clothing_drift",
        "preview_modeling_reference_unclear",
    )
    ordered = [code for code in priority if code in blockers]
    ordered.extend(code for code in blockers if code not in ordered)
    for key in ("model_input_quality", "generation_input_quality"):
        quality = job.image_metrics.get(key)
        if not isinstance(quality, dict):
            continue
        existing = quality.get("blockers", [])
        existing_codes = [str(code) for code in existing] if isinstance(existing, list) else []
        quality["blockers"] = list(dict.fromkeys(ordered + existing_codes))
        quality["model_input_eligible"] = False
        quality["score"] = min(float(quality.get("score", 100)), float(report.get("score", 0)))
    return ordered


def _assess_job_preview_visual_quality(job: Job, original: Path) -> dict[str, Any] | None:
    """Gate high-quality realistic portrait previews before paid 3D submission."""

    if (
        job.source != "image"
        or job.style not in IDENTITY_FIRST_PORTRAIT_STYLES
        or job.generation_profile != "quality"
        or job.model_reference_path is None
        or job.preview_path is None
        or not os.environ.get("OPENAI_API_KEY", "")
    ):
        return None
    with _JOBS_LOCK:
        job.phase = "checking_image"
        job.message = "Comparing the prepared face and material ownership with the original image."
        job.progress = 14
        _persist_job(job)
    # Gate the exact image that will be submitted for geometry. The restored
    # face preview can look nearly pixel-identical while hiding an identity
    # drift in the sculptural Image2 output that Tripo actually receives.
    submitted_reference = _geometry_generation_reference(job) or job.model_reference_path
    report = review_prepared_reference(
        original,
        submitted_reference,
        submitted_reference if job.image_metrics.get("design_reference") == "ai-design-v1" else job.preview_path,
        job.directory / "preview-visual-review",
    )
    job.image_metrics["preview_visual_quality"] = report
    _apply_preview_visual_quality_gate(job, report)
    return report


def _quality_portrait_multiview_enabled(job: Job) -> bool:
    cleanup = job.image_metrics.get("portrait_skin_cleanup", {})
    return (
        job.source == "image"
        and job.style == "realistic"
        and job.generation_profile == "quality"
        and isinstance(cleanup, dict)
        and cleanup.get("activated") == 1
        and job.model_reference_path is not None
    )


def _portrait_multiview_prompt(background_instruction: str | None = None) -> str:
    # Keep this below conservative image-edit prompt limits. The supplied image
    # owns identity, materials, pose and base; the prompt owns only view layout.
    background_direction = background_instruction or (
        "Use a genuine alpha-transparent background in every panel; do not paint a gray, white, checkerboard, studio or gradient "
        "background and do not add shadows. "
    )
    return (
        "Create one 2x2 orthographic turntable sheet of this exact finished portrait bust. "
        "Panels must be top-left front, top-right subject-left, bottom-left back, bottom-right subject-right. "
        "Render one unchanged sculpture at yaw 0, 90, 180 and 270 degrees, same scale, zero pitch and roll, full object visible. "
        "Preserve the exact real-person identity, adult age, head and face proportions, eyes, nose, cheeks, smile, lips, jaw, hair, "
        "pose, hands, clothing, accessories, material boundaries and integrated base. Never substitute a generic, doll, anime or "
        "caricature face. The front head must remain level with both eyes and the complete source smile readable; keep facial relief "
        "and natural asymmetry consistent across the side views. Reconstruct genuine rounded depth for the skull, hair, shoulders, "
        "jacket and torso in side and back views. No flat backing board, person-shaped wall, silhouette plate, photo cutout, rear sheet "
        "or straight vertical panel may exist behind the portrait; the back must be the actual curved back of the same bust. Keep broad "
        "clean printable color regions with no skin color on "
        "clothing and no black shadow patches inside a colored blouse. A partially hidden hand must be either one compact bounded "
        "skin region or fully tucked under the existing sleeve, never a skin stripe on the jacket. "
        + background_direction
        +
        "No labels, borders, extra views, perspective, added objects or changed geometry."
    )


def _portrait_geometry_material_prompt(palette_roles: Mapping[str, str]) -> str:
    try:
        primary = str(palette_roles["primary"]).strip().upper()
        structure = str(palette_roles["structure"]).strip().upper()
        skin = str(palette_roles["light"]).strip().upper()
        accent = str(palette_roles["accent"]).strip().upper()
    except KeyError:
        raise PortraitProjectionError("The portrait material roles are incomplete.") from None
    if any(not re.fullmatch(r"#[0-9A-F]{6}", color) for color in (primary, structure, skin, accent)):
        raise PortraitProjectionError("The portrait material roles contain an invalid color.")
    return (
        "Repaint this exact 2x2 orthographic turntable sheet as a material-ID reference for a four-filament 3D print. "
        "Preserve every panel, camera, silhouette, geometry, proportions, face landmarks, expression, crossed-arm order, "
        "clothing edge, base, scale and panel placement pixel-for-pixel as closely as possible. Change color only; do not "
        "redraw, beautify, reshape, smooth, move, crop, add, remove or relight the model. Use flat solid semantic materials "
        "with hard clean boundaries and no gradients, shading, texture, highlights, reflections, speckles, color fringes or "
        f"cast shadows. All blazer, jacket, lapels, sleeves, cuffs, pockets and rear outer clothing are {primary}. Only the "
        f"actual face, ears, neck, hands and genuinely exposed wrist skin are {skin}. In the back panel, the narrow exposed nape "
        f"immediately below the dark hairline remains {skin}; only the outward collar below it is {primary}. Hair, watch and the entire pedestal are "
        f"{structure}. The inner blouse only is {accent}. Treat the realistic face like a finely sculpted portrait: the face, "
        f"ears, nose, cheeks, lips and mouth interior stay in the same continuous {skin} material. Preserve only thin, connected, "
        f"source-faithful eyebrows, pupils and upper eyelids in {structure}; never make black eye sockets, eye whites, eyeliner wings, "
        f"a black mouth cavity or dark cheek seams. Preserve the visible teeth as one compact {primary} band bounded strictly inside "
        f"the original smile; never enlarge it into a white mouth block or place white on lips, cheeks or eyes. Never use {accent} on "
        f"any face, lips, teeth, skin, hand or neck. Never put skin on clothing, especially shoulder, "
        "upper arm, elbow, forearm, cuff, jacket back, collar or lapel. Never put accent or dark shading inside outer clothing. "
        "Keep the background one uniform very light neutral color, distinct from the subject, with no floor or shadow. Return "
        "exactly the same 2x2 layout and nothing else."
    )


def _prepare_portrait_geometry_material_views(
    natural_turntable: Path,
    output_directory: Path,
    palette_roles: Mapping[str, str],
) -> tuple[dict[str, Path], dict[str, Any]]:
    views_directory = natural_turntable / "model-views"
    masks_directory = natural_turntable / "model-masks"
    source_views = {
        view: views_directory / f"{view}.png"
        for view in MULTIVIEW_ORDER
    }
    if any(not path.is_file() for path in source_views.values()) or any(
        not (masks_directory / f"{view}.png").is_file() for view in MULTIVIEW_ORDER
    ):
        raise PortraitProjectionError("The exact portrait turntable or its masks are incomplete.")
    output_directory.mkdir(parents=True, exist_ok=True)
    source_sheet = build_multiview_input_sheet(
        source_views, output_directory / "natural-turntable-sheet.png"
    )
    semantic_sheet = output_directory / "image2-material-sheet.png"
    edit_image(
        source_sheet,
        _portrait_geometry_material_prompt(palette_roles),
        semantic_sheet,
    )
    crops = split_multiview_sheet(semantic_sheet, output_directory / "image2-crops")
    prepared: dict[str, Path] = {}
    view_reports: dict[str, Any] = {}
    for view in MULTIVIEW_ORDER:
        view_directory = output_directory / "views" / view
        view_reports[view] = quantize_geometry_aligned_material_reference(
            crops[view],
            masks_directory / f"{view}.png",
            view_directory,
            palette_roles,
            view_name=view,
        )
        prepared[view] = view_directory
    report = {
        "status": "prepared",
        "version": "image2-semantic-material-v1",
        "source_sheet": str(source_sheet.name),
        "semantic_sheet": str(semantic_sheet.name),
        "views": view_reports,
    }
    _write_mesh_repair_report(output_directory / "semantic-material-report.json", report)
    return prepared, report


def _create_portrait_multiview_sheet(job: Job, sheet: Path) -> None:
    """Create one isolated sheet with exactly one potentially billed edit."""
    source = _geometry_generation_reference(job) or job.model_reference_path
    if source is None:
        raise PortraitMultiviewPreparationError(
            "The identity-locked portrait front view is unavailable."
        )
    edit_image(
        source,
        _portrait_multiview_prompt(),
        sheet,
        background="transparent",
    )


def _multiview_paths_from_metrics(job: Job, key: str) -> dict[str, Path] | None:
    value = job.image_metrics.get("multiview_reference", {})
    stored = value.get(key) if isinstance(value, dict) else None
    if not isinstance(stored, dict) or set(stored) != set(MULTIVIEW_ORDER):
        return None
    result: dict[str, Path] = {}
    root = job.directory.resolve()
    for view in MULTIVIEW_ORDER:
        relative = stored.get(view)
        if not isinstance(relative, str) or not relative:
            return None
        try:
            candidate = (job.directory / relative).resolve()
            candidate.relative_to(root)
        except (OSError, ValueError):
            return None
        if not candidate.is_file():
            return None
        result[view] = candidate
    return result


def _multiview_sheet_fingerprint(sheet: Path) -> str:
    try:
        stat_result = sheet.stat()
    except OSError:
        return ""
    return f"{stat_result.st_size}:{stat_result.st_mtime_ns}"


def _mark_multiview_candidate_rejected(job: Job, sheet: Path, reason: str) -> None:
    with _JOBS_LOCK:
        job.image_metrics["multiview_candidate_rejected"] = {
            "fingerprint": _multiview_sheet_fingerprint(sheet),
            "reason": reason,
            "material_gate_version": PORTRAIT_MATERIAL_GATE_VERSION,
        }
        _persist_job(job)


def _can_reuse_multiview_candidate(job: Job, sheet: Path) -> bool:
    if not sheet.is_file() or job.model_reference_path is None:
        return False
    source = _geometry_generation_reference(job) or job.model_reference_path
    try:
        if sheet.stat().st_mtime_ns < source.stat().st_mtime_ns:
            return False
    except OSError:
        return False
    rejected = job.image_metrics.get("multiview_candidate_rejected")
    if isinstance(rejected, Mapping) and rejected.get("fingerprint") == _multiview_sheet_fingerprint(sheet):
        rejected_reason = str(rejected.get("reason", ""))
        if rejected_reason.startswith("portrait_material_gate:"):
            # Re-evaluate an older candidate exactly once after material-gate
            # rules improve.  A candidate rejected by this version is still
            # regenerated on the next user retry.
            return rejected.get("material_gate_version") != PORTRAIT_MATERIAL_GATE_VERSION
        if rejected_reason != "identity_consistency_gate":
            return False
        stored_review = job.image_metrics.get("multiview_candidate_review")
        # Old jobs did not persist the review details. Recheck that exact sheet
        # once under the newer acceptance rules, then persist the result.
        if not isinstance(stored_review, Mapping):
            return True
        return evaluate_multiview_review_acceptance(stored_review).get("status") == "pass"
    return True


def _ensure_portrait_multiview(job: Job) -> dict[str, Path] | None:
    if not _quality_portrait_multiview_enabled(job):
        return None
    cached = _multiview_paths_from_metrics(job, "generation_views")
    stored_reference = job.image_metrics.get("multiview_reference", {})
    stored_normalization = (
        stored_reference.get("normalization", {})
        if isinstance(stored_reference, Mapping)
        else {}
    )
    cache_has_quality_resolution = (
        isinstance(stored_normalization, Mapping)
        and stored_normalization.get("version") == MULTIVIEW_NORMALIZATION_VERSION
        and stored_normalization.get("canvas_size") == list(HIGH_QUALITY_PORTRAIT_CANVAS_SIZE)
    )
    if cached is not None and cache_has_quality_resolution:
        return cached
    assert job.model_reference_path is not None
    locked_front = _geometry_generation_reference(job) or job.model_reference_path
    output = job.directory / "multiview"
    output.mkdir(parents=True, exist_ok=True)
    sheet = output / "multiview-sheet.png"
    _stop_boundary(job)
    with _JOBS_LOCK:
        job.phase = "preparing_multiview"
        job.message = "Preparing identity-preserving portrait views."
        job.progress = 12
        _persist_job(job)
    try:
        reuse_candidate = _can_reuse_multiview_candidate(job, sheet)
        if not reuse_candidate:
            # A neutral painted background is not sufficient for light-clothed
            # portraits: its border gradient can overlap the jacket colour and
            # make deterministic subject extraction punch holes through the
            # torso. Prefer real alpha and fall back to a palette-aware chroma
            # key when an OpenAI-compatible proxy lacks transparent edits.
            _create_portrait_multiview_sheet(job, sheet)
            with _JOBS_LOCK:
                job.image_metrics.pop("multiview_candidate_rejected", None)
                _persist_job(job)
        _validate_image_file(sheet, minimum_edge=512, require_visual_detail=True)
        crops = split_multiview_sheet(sheet, output / "crops")
        references, generation_references, metrics = process_multiview_crops(
            crops,
            output / "views",
            job.palette,
            job.print_settings,
            palette_roles=job.palette_roles,
        )
        normalization = normalize_multiview_inputs(
            references,
            generation_references,
            locked_front_material=job.preview_path,
            # Lock only the central facial identity pixels. Normalization keeps
            # the reviewed turntable front's complete silhouette and fills any
            # white-garment holes from it, so the source face cannot introduce
            # the checkerboard gaps that previously disabled identity locking.
            locked_front_generation=locked_front,
            target_canvas_size=HIGH_QUALITY_PORTRAIT_CANVAS_SIZE,
        )
        review_sheet = build_multiview_input_sheet(
            generation_references, output / "multiview-input-sheet.png"
        )
        rejected_views: list[str] = []
        material_review_views: list[str] = []
        for view in MULTIVIEW_ORDER:
            gate = evaluate_portrait_material_gate(
                metrics[view], job.palette_roles, view_name=view,
            )
            metrics[view]["multiview_material_gate"] = gate
            if gate.get("status") != "pass":
                if gate.get("status") == "review":
                    material_review_views.append(view)
                else:
                    rejected_views.append(view)
        if rejected_views:
            _mark_multiview_candidate_rejected(
                job, sheet, "portrait_material_gate:" + ",".join(rejected_views)
            )
            raise PortraitMultiviewPreparationError(
                "One or more portrait views still contain unsafe skin, garment, or detached material regions."
            )
        for view in MULTIVIEW_ORDER:
            quality = assess_model_input_image(generation_references[view])
            if not bool(quality.get("model_input_eligible", False)):
                _mark_multiview_candidate_rejected(job, sheet, f"model_input_gate:{view}")
                raise PortraitMultiviewPreparationError(
                    f"The {view} portrait view is not suitable for 3D generation."
                )
        _stop_boundary(job)
        with _JOBS_LOCK:
            job.message = "Checking portrait identity across four views."
            job.progress = 17
            _persist_job(job)
        stored_review = job.image_metrics.get("multiview_candidate_review")
        if (
            reuse_candidate
            and isinstance(stored_review, Mapping)
            and evaluate_multiview_review_acceptance(stored_review).get("status") == "pass"
        ):
            review = dict(stored_review)
        else:
            review_description = (
                "The exact approved real-person portrait bust, with unchanged identity, pose, clothing, "
                "accessories and base."
            )
            if material_review_views:
                review_description += (
                    " Give extra scrutiny to skin-versus-garment ownership and detached colour fragments in "
                    + ", ".join(material_review_views)
                    + " profile view(s), whose frontal face cleanup was intentionally deferred."
                )
            review = review_multiview_sheet(
                review_sheet,
                review_description,
                source_path=locked_front,
                completion=complete_vision,
            )
        review["material_review_views"] = material_review_views
        review_acceptance = evaluate_multiview_review_acceptance(review)
        review["acceptance"] = review_acceptance
        with _JOBS_LOCK:
            job.image_metrics["multiview_candidate_review"] = review
            _persist_job(job)
        if review_acceptance.get("status") != "pass":
            _mark_multiview_candidate_rejected(job, sheet, "identity_consistency_gate")
            raise PortraitMultiviewPreparationError(
                "The generated portrait views did not preserve identity consistently."
            )
        manifest = write_multiview_manifest(
            output,
            sheet=review_sheet,
            references=references,
            generation_references=generation_references,
            metrics=metrics,
            review=review,
            palette=job.palette,
            settings=PrintSettings.from_mapping(job.print_settings),
        )
    except (
        OpenAIPreprocessorError,
        PrintableImageError,
        ModelInputImageQualityError,
        MultiviewReferenceError,
        ValueError,
    ) as exc:
        _mark_multiview_candidate_rejected(job, sheet, "view_preparation_error")
        raise PortraitMultiviewPreparationError(
            f"The high-quality portrait views could not be prepared: {exc}"
        ) from None

    def relative_paths(paths: Mapping[str, Path]) -> dict[str, str]:
        return {view: paths[view].resolve().relative_to(job.directory.resolve()).as_posix() for view in MULTIVIEW_ORDER}

    with _JOBS_LOCK:
        job.image_metrics.pop("multiview_candidate_rejected", None)
        job.image_metrics.pop("multiview_candidate_review", None)
        job.image_metrics.pop("multiview_retry", None)
        job.image_metrics["multiview_reference"] = {
            "status": "pass",
            "score": int(review.get("score", 0)),
            "sheet": review_sheet.resolve().relative_to(job.directory.resolve()).as_posix(),
            "provider_sheet": sheet.resolve().relative_to(job.directory.resolve()).as_posix(),
            "normalization": normalization,
            "manifest": manifest.resolve().relative_to(job.directory.resolve()).as_posix(),
            "generation_views": relative_paths(generation_references),
            "material_views": relative_paths(references),
        }
        _persist_job(job)
    return dict(generation_references)


def _recommend_palette_job(job: Job) -> None:
    # A queued legacy recommendation resumes into creation without another
    # color-recommendation request or printer palette.
    if job.source == "text":
        _preprocess_text_job(job, job.user_prompt)
    elif job.input_path is not None:
        _preprocess_image_job(job, job.input_path, _normalize_image_instruction(job.user_prompt))
    else:
        _fail_job(job, "The stored reference image is unavailable.")


def _use_unrestricted_creation(job: Job) -> None:
    """Apply the current policy only when starting a new creation stage.

    Completed history is never rewritten and restoring an already submitted
    provider task never creates another task. Old pending palette previews can
    continue with their unquantized design image.
    """
    job.palette = ()
    job.palette_roles = {}
    job.palette_recommendation = {}
    job.palette_recommendation_confirmed = False
    job.image_metrics["creation_color_policy"] = "unrestricted-v1"
    if job.raw_preview_path is not None and job.raw_preview_path.is_file():
        job.image_metrics["design_reference"] = "ai-design-v1"


def _preprocess_text_job(job: Job, prompt: str) -> None:
    _use_unrestricted_creation(job)
    try:
        _stop_boundary(job)
        prepared = _generation_prompt(
            prompt,
            job.palette,
            max_prompt_bytes=MAX_PROMPT_BYTES,
            constrain_palette=False,
        )
        if not prepared or len(prepared.encode("utf-8")) > MAX_PROMPT_BYTES:
            raise OpenAIPreprocessorError("The prepared prompt is empty or exceeds the 2000-byte limit.")
        raw_preview = job.directory / "style-preview-raw.png"
        generate_geometry_reference_image(
            prompt, raw_preview, job.style, job.custom_style,
            palette=job.palette, palette_roles=job.palette_roles,
        )
        _validate_image_file(
            raw_preview,
            minimum_edge=MIN_MODEL_REFERENCE_EDGE,
            require_visual_detail=True,
        )
        job.raw_preview_path = raw_preview
        if job.palette:
            color_usage = _apply_printable_image_pipeline(job, raw_preview)
        else:
            preview = job.directory / "preview.png"
            shutil.copyfile(raw_preview, preview)
            job.preview_path = preview
            color_usage = {}
        job.image_metrics["design_reference"] = "ai-design-v1"
        job.image_metrics["portrait_geometry"] = {
            "detected": False,
            "evidence": "not_applicable",
        }
        validated = _validate_image_file(
            raw_preview,
            minimum_edge=MIN_MODEL_REFERENCE_EDGE,
            require_visual_detail=True,
        )
        _assess_reference_advice(job)
        (job.directory / "preview-colors.json").write_text(
            json.dumps(
                {
                    "style": job.style,
                    "palette_constrained": bool(job.palette),
                    "palette_pixels": color_usage,
                    "palette_roles": job.palette_roles,
                    "print": job.print_settings,
                    "metrics": job.image_metrics,
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )
        job.preview_content_type = validated.content_type
        with _JOBS_LOCK:
            if job.stop_event.is_set():
                raise JobStopped()
            job.prepared_prompt = prepared
            job.preprocess_failure = {}
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            job.message = _printable_preview_message(job, "Review the prepared image before generation.")
            job.progress = 15
    except JobStopped:
        _mark_stopped(job)
    except ValueError as exc:
        _fail_job(job, str(exc))
    except OpenAIPreprocessorError as exc:
        if not _preprocess_fallback_enabled():
            _fail_preprocess_job(job, exc)
        else:
            with _JOBS_LOCK:
                job.prepared_prompt = _generation_prompt(
                    prompt, job.palette, max_prompt_bytes=MAX_PROMPT_BYTES
                )
                job.preprocess_failure = {}
                job.state = "awaiting_confirmation"
                job.phase = "awaiting_confirmation"
                job.message = "Preprocessing is unavailable; review the original prompt before generation."
                job.progress = 15
    except Exception:
        _fail_job(job, "Text preprocessing failed.")
    finally:
        _finish_deleted(job)


def _preprocess_image_job(job: Job, input_path: Path, instruction: str) -> None:
    _use_unrestricted_creation(job)
    raw_preview = job.directory / "style-preview-raw.png"
    geometry_reference = job.directory / "geometry-reference.png"
    preview = job.directory / "preview.png"
    try:
        _stop_boundary(job)
        with _JOBS_LOCK:
            job.phase = "image_generation"
            job.message = "The image service is preparing the printable portrait; this usually takes one to three minutes."
            job.progress = 11
            _persist_job(job)
        preprocess_image(
            input_path,
            instruction,
            raw_preview,
            job.palette,
            job.style,
            str(job.print_settings.get("shadow_color", "blue")),
            job.palette_roles,
            job.custom_style,
            geometry_reference,
        )
        # Test adapters and older compatible preprocessors may not implement
        # the optional sculptural snapshot yet. Preserve the previous behavior
        # instead of failing the whole image journey.
        if not geometry_reference.is_file():
            shutil.copyfile(raw_preview, geometry_reference)
        with _JOBS_LOCK:
            job.phase = "checking_image"
            job.message = "The generated portrait is being checked for identity, framing, and usable detail."
            job.progress = 12
            _persist_job(job)
        _validate_image_file(
            raw_preview,
            minimum_edge=MIN_MODEL_REFERENCE_EDGE,
            require_visual_detail=True,
        )
        job.raw_preview_path = raw_preview
        job.geometry_reference_path = geometry_reference
        if job.palette:
            with _JOBS_LOCK:
                job.phase = "printability_check"
                job.message = "Skin, clothing, accent, and structure colors are being separated into printable regions."
                job.progress = 13
                _persist_job(job)
            color_usage = _apply_printable_image_pipeline(job, raw_preview)
            preview = job.preview_path or preview
        else:
            shutil.copyfile(raw_preview, preview)
            job.preview_path = preview
            color_usage = {}
        face_lock = job.directory / PORTRAIT_FACE_LOCK_FILENAME
        job.image_metrics["design_reference"] = "ai-design-v1"
        legacy_cleanup = job.image_metrics.get("portrait_skin_cleanup", {})
        portrait_detected = face_lock.is_file() or (
            isinstance(legacy_cleanup, dict) and legacy_cleanup.get("activated") == 1
        )
        job.image_metrics["portrait_geometry"] = {
            "detected": portrait_detected,
            "evidence": "source_face_lock" if face_lock.is_file() else "legacy_material_cleanup" if portrait_detected else "none",
        }
        _assess_reference_advice(job)
        preview = job.preview_path or preview
        validated = _validate_image_file(
            raw_preview,
            minimum_edge=MIN_MODEL_REFERENCE_EDGE,
            require_visual_detail=True,
        )
        _assess_job_preview_visual_quality(job, input_path)
        (job.directory / "preview-colors.json").write_text(
            json.dumps(
                {
                    "style": job.style,
                    "palette_constrained": bool(job.palette),
                    "palette_pixels": color_usage,
                    "palette_roles": job.palette_roles,
                    "print": job.print_settings,
                    "metrics": job.image_metrics,
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )
        _stop_boundary(job)
        with _JOBS_LOCK:
            if job.stop_event.is_set():
                raise JobStopped()
            job.preview_path = preview
            job.preview_content_type = validated.content_type
            job.preprocess_failure = {}
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            job.message = _printable_preview_message(job, "Review the prepared image before generation.")
            job.progress = 15
    except JobStopped:
        _mark_stopped(job)
    except ValueError as exc:
        _fail_job(job, str(exc))
    except OpenAIPreprocessorError as exc:
        _fail_preprocess_job(job, exc)
    except Exception:
        traceback.print_exc()
        _fail_preprocess_job(
            job,
            OpenAIPreprocessorError(
                "The image was generated, but its local validation failed.",
                code="local_image_processing_failed",
                retryable=True,
            ),
        )
    finally:
        _finish_deleted(job)


def _progress_callback(job: Job, start: int, end: int) -> Callable[[int | float | None], None]:
    def update(value: int | float | None) -> None:
        try:
            fraction = max(0.0, min(float(value), 100.0)) / 100.0
        except (TypeError, ValueError):
            return
        with _JOBS_LOCK:
            if not job.stop_event.is_set():
                job.progress = start + int((end - start) * fraction)
                _persist_job(job)

    return update


def _automatic_visual_review(job: Job, artifact: Path) -> dict[str, Any] | None:
    """Reuse existing advice; the explicit visual-review action performs new reviews.

    Full-mesh turntables plus a remote review took over five minutes for a
    two-million-face model. They must not delay an otherwise usable result.
    """
    return _read_job_report(job, VISUAL_QUALITY_FILENAME) or None


def _safe_package_path(root: Path, name: str) -> Path:
    normalized = name.replace("\\", "/")
    if not normalized or normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized):
        raise TripoError("The generated OBJ package contains an unsafe path.")
    parts = normalized.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise TripoError("The generated OBJ package contains an unsafe path.")
    destination = root.joinpath(*parts).resolve()
    try:
        destination.relative_to(root.resolve())
    except ValueError:
        raise TripoError("The generated OBJ package contains an unsafe path.") from None
    return destination


def _extract_obj_package(archive: Path, package_dir: Path) -> Path:
    try:
        with zipfile.ZipFile(archive) as bundle:
            members = bundle.infolist()
            if not members or len(members) > MAX_ARCHIVE_FILES:
                raise TripoError("The generated OBJ package contains an invalid number of files.")
            total_size = 0
            destinations: set[Path] = set()
            for member in members:
                mode = member.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise TripoError("The generated OBJ package contains a symbolic link.")
                file_type = stat.S_IFMT(mode)
                if file_type not in {0, stat.S_IFREG, stat.S_IFDIR}:
                    raise TripoError("The generated OBJ package contains an unsupported file type.")
                if member.flag_bits & 0x1:
                    raise TripoError("The generated OBJ package must not be encrypted.")
                destination = _safe_package_path(package_dir, member.filename)
                if destination in destinations:
                    raise TripoError("The generated OBJ package contains duplicate paths.")
                destinations.add(destination)
                if not member.is_dir():
                    total_size += member.file_size
                    if total_size > MAX_UNPACKED_BYTES:
                        raise TripoError("The generated OBJ package is too large after extraction.")

            package_dir.mkdir(parents=True, exist_ok=False)
            extracted_size = 0
            for member in members:
                destination = _safe_package_path(package_dir, member.filename)
                if member.is_dir():
                    destination.mkdir(parents=True, exist_ok=True)
                    continue
                destination.parent.mkdir(parents=True, exist_ok=True)
                try:
                    source = bundle.open(member)
                    target = destination.open("xb")
                except (OSError, RuntimeError, zipfile.BadZipFile):
                    raise TripoError("The generated OBJ package could not be extracted.") from None
                with source, target:
                    while True:
                        chunk = source.read(1024 * 1024)
                        if not chunk:
                            break
                        extracted_size += len(chunk)
                        if extracted_size > MAX_UNPACKED_BYTES:
                            raise TripoError("The generated OBJ package is too large after extraction.")
                        target.write(chunk)
    except TripoError:
        raise
    except (OSError, RuntimeError, zipfile.BadZipFile, zipfile.LargeZipFile):
        raise TripoError("Tripo returned an invalid OBJ package.") from None

    objects = [path for path in package_dir.rglob("*") if path.is_file() and path.suffix.lower() == ".obj"]
    if len(objects) != 1:
        raise TripoError("The generated OBJ package must contain exactly one OBJ model.")
    return objects[0]


def _obj_dependency_path(package_dir: Path, parent: Path, value: str, kind: str) -> Path:
    value = value.strip().strip('"')
    if not value:
        raise TripoError(f"The generated OBJ has an invalid {kind} reference.")
    normalized = value.replace("\\", "/")
    if normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized):
        raise TripoError(f"The generated OBJ has an unsafe {kind} reference.")
    destination = parent.joinpath(*normalized.split("/")).resolve()
    try:
        destination.relative_to(package_dir.resolve())
    except ValueError:
        raise TripoError(f"The generated OBJ has an unsafe {kind} reference.") from None
    if not destination.is_file():
        raise TripoError(f"The generated OBJ is missing its {kind} file.")
    return destination


def _read_obj_geometry(obj_path: Path) -> tuple[array, array, list[str]]:
    positions = array("d")
    texcoords = array("d")
    material_libraries: list[str] = []
    try:
        with obj_path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) < 4:
                        raise TripoError("The generated OBJ has an invalid vertex.")
                    try:
                        values = [float(value) for value in fields[1:4]]
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex.") from None
                    if not all(math.isfinite(value) for value in values):
                        raise TripoError("The generated OBJ has an invalid vertex.")
                    positions.extend(values)
                elif keyword == "vt":
                    if len(fields) < 3:
                        raise TripoError("The generated OBJ has an invalid texture coordinate.")
                    try:
                        values = [float(value) for value in fields[1:3]]
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid texture coordinate.") from None
                    if not all(math.isfinite(value) for value in values):
                        raise TripoError("The generated OBJ has an invalid texture coordinate.")
                    texcoords.extend(values)
                elif keyword == "mtllib":
                    reference = stripped[len(fields[0]) :].strip()
                    if reference:
                        material_libraries.append(reference)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if not positions or not texcoords:
        raise TripoError("The generated OBJ does not contain textured geometry.")
    if not material_libraries:
        raise TripoError("The generated OBJ is missing its material library.")
    return positions, texcoords, material_libraries


def _read_material_textures(obj_path: Path, package_dir: Path, references: list[str]) -> dict[str, Path]:
    textures: dict[str, Path] = {}
    for reference in references:
        material_path = _obj_dependency_path(package_dir, obj_path.parent, reference, "material")
        current_material = ""
        try:
            with material_path.open("r", encoding="utf-8", errors="strict") as stream:
                for line in stream:
                    stripped = line.strip()
                    if not stripped or stripped.startswith("#"):
                        continue
                    fields = stripped.split(maxsplit=1)
                    keyword = fields[0].lower()
                    value = fields[1].strip() if len(fields) > 1 else ""
                    if keyword == "newmtl":
                        current_material = value
                    elif keyword == "map_kd" and current_material:
                        # Tripo emits a plain filename. Taking the final token also
                        # tolerates standard map_Kd options such as -s or -o.
                        texture_reference = value.strip().strip('"')
                        direct = material_path.parent / texture_reference
                        if not direct.is_file():
                            texture_reference = value.split()[-1].strip('"') if value.split() else ""
                        textures[current_material] = _obj_dependency_path(
                            package_dir, material_path.parent, texture_reference, "base-color texture"
                        )
        except UnicodeDecodeError:
            raise TripoError("The generated material library is not valid UTF-8 text.") from None
        except OSError:
            raise TripoError("The generated material library could not be read.") from None
    if not textures:
        raise TripoError("The generated OBJ is missing its base-color texture.")
    return textures


def _resolve_obj_index(value: str, count: int, kind: str) -> int:
    try:
        index = int(value)
    except ValueError:
        raise TripoError(f"The generated OBJ has an invalid {kind} index.") from None
    if index == 0:
        raise TripoError(f"The generated OBJ has an invalid {kind} index.")
    resolved = index - 1 if index > 0 else count + index
    if resolved < 0 or resolved >= count:
        raise TripoError(f"The generated OBJ references a missing {kind}.")
    return resolved


def _srgb_to_lab(color: tuple[int, int, int]) -> tuple[float, float, float]:
    linear = []
    for channel in color:
        value = channel / 255.0
        linear.append(value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4)
    red, green, blue = linear
    x = (0.4124564 * red + 0.3575761 * green + 0.1804375 * blue) / 0.95047
    y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue
    z = (0.0193339 * red + 0.1191920 * green + 0.9503041 * blue) / 1.08883

    def transform(value: float) -> float:
        return value ** (1.0 / 3.0) if value > 0.008856 else 7.787 * value + 16.0 / 116.0

    fx, fy, fz = transform(x), transform(y), transform(z)
    return 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)


def _palette_data(palette: tuple[str, ...]) -> tuple[list[tuple[int, int, int]], list[tuple[float, float, float]]]:
    colors = [tuple(int(color[index : index + 2], 16) for index in (1, 3, 5)) for color in palette]
    return colors, [_srgb_to_lab(color) for color in colors]


def _vertex_color_data(
    palette: tuple[str, ...],
) -> tuple[list[tuple[int, int, int]], list[tuple[float, float, float]]]:
    return _palette_data(palette) if palette else ([], [])


def _nearest_palette_index(
    color: tuple[int, int, int],
    palette_lab: list[tuple[float, float, float]],
    cache: dict[tuple[int, int, int], int],
) -> int:
    cached = cache.get(color)
    if cached is not None:
        return cached
    lab = _srgb_to_lab(color)
    index = min(
        range(len(palette_lab)),
        key=lambda item: sum((lab[channel] - palette_lab[item][channel]) ** 2 for channel in range(3)),
    )
    cache[color] = index
    return index


def _portrait_material_palette_indices(
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None,
    portrait_materials: bool,
) -> tuple[int, int] | None:
    """Return the neutral garment and skin indices for an explicitly detected portrait.

    This deliberately requires both upstream portrait evidence and a very specific
    material palette. It must never change generic nearest-colour behaviour for
    non-portrait jobs or palettes where white and warm skin are not unambiguous.
    """
    if not portrait_materials or not palette or not palette_roles:
        return None
    try:
        primary = str(palette_roles["primary"]).strip().upper()
        skin = str(palette_roles["light"]).strip().upper()
        primary_index = palette.index(primary)
        skin_index = palette.index(skin)
    except (KeyError, ValueError):
        return None
    if primary_index == skin_index:
        return None
    _, labs = _palette_data(palette)
    primary_lab = labs[primary_index]
    skin_lab = labs[skin_index]
    primary_chroma = math.hypot(primary_lab[1], primary_lab[2])
    skin_chroma = math.hypot(skin_lab[1], skin_lab[2])
    if primary_lab[0] < 82.0 or primary_chroma > 12.0:
        return None
    if skin_lab[1] < 5.0 or skin_lab[2] < 8.0 or skin_chroma < 16.0:
        return None
    return primary_index, skin_index


def _portrait_material_role_indices(
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None,
    portrait_materials: bool,
) -> dict[str, int] | None:
    if _portrait_material_palette_indices(palette, palette_roles, portrait_materials) is None:
        return None
    try:
        result = {
            role: palette.index(str((palette_roles or {})[role]).strip().upper())
            for role in ("primary", "structure", "light", "accent")
        }
    except (KeyError, ValueError):
        return None
    return result if len(set(result.values())) == 4 else None


def _semantic_palette_index(
    color: tuple[int, int, int],
    palette_lab: list[tuple[float, float, float]],
    cache: dict[tuple[int, int, int], int],
    portrait_indices: tuple[int, int] | None,
    portrait_role_indices: Mapping[str, int] | None = None,
) -> int:
    nearest = _nearest_palette_index(color, palette_lab, cache)
    if portrait_indices is None:
        return nearest
    if portrait_role_indices is not None and nearest == portrait_role_indices.get("structure"):
        accent_index = portrait_role_indices.get("accent")
        if accent_index is not None:
            lab = _srgb_to_lab(color)
            accent_lab = palette_lab[accent_index]
            source_chroma = math.hypot(lab[1], lab[2])
            accent_chroma = math.hypot(accent_lab[1], accent_lab[2])
            hue_similarity = (
                (lab[1] * accent_lab[1] + lab[2] * accent_lab[2])
                / (source_chroma * accent_chroma)
                if source_chroma > 1e-9 and accent_chroma > 1e-9
                else -1.0
            )
            # Deep folds of a green blouse can be perceptually nearer to black
            # than to the available green filament. Keep genuinely neutral hair,
            # base and watch pixels black, but preserve a chromatic sample whose
            # hue and channel dominance clearly match the accent material.
            if (
                lab[0] >= 8.0
                and source_chroma >= 4.0
                and accent_chroma >= 8.0
                and hue_similarity >= 0.90
                and color[1] >= color[0] + 3
                and color[1] >= color[2] + 3
            ):
                return accent_index
    primary_index, skin_index = portrait_indices
    if nearest != skin_index:
        return nearest
    lab = _srgb_to_lab(color)
    primary_distance = math.sqrt(
        sum((lab[channel] - palette_lab[primary_index][channel]) ** 2 for channel in range(3))
    )
    skin_distance = math.sqrt(
        sum((lab[channel] - palette_lab[skin_index][channel]) ** 2 for channel in range(3))
    )
    # Warm-white cloth shadows sit very close to the neutral/skin Voronoi
    # boundary. Bias only those low-chroma ambiguous samples back to the garment;
    # clearly saturated skin remains mapped to skin.
    if (
        lab[0] >= 35.0
        and max(color) - min(color) <= 36
        and math.hypot(lab[1], lab[2]) <= 18.0
        and primary_distance <= max(1e-9, skin_distance) * 1.35
    ):
        return primary_index
    return nearest


def _bake_obj_texture_to_vertex_colors(
    obj_path: Path,
    package_dir: Path,
    destination: Path,
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None = None,
    portrait_materials: bool = False,
    natural_destination: Path | None = None,
) -> None:
    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError:
        raise TripoError("Pillow is required to convert OBJ textures into printable vertex colors.") from None

    positions, texcoords, material_references = _read_obj_geometry(obj_path)
    texture_paths = _read_material_textures(obj_path, package_dir, material_references)
    palette_rgb, palette_lab = _vertex_color_data(palette)
    nearest_cache: dict[tuple[int, int, int], int] = {}
    portrait_indices = _portrait_material_palette_indices(
        palette, palette_roles, portrait_materials
    )
    portrait_role_indices = _portrait_material_role_indices(
        palette, palette_roles, portrait_materials
    )
    images: dict[str, Any] = {}
    try:
        for material, texture_path in texture_paths.items():
            try:
                image = Image.open(texture_path)
                if image.width <= 0 or image.height <= 0 or image.width * image.height > MAX_TEXTURE_PIXELS:
                    image.close()
                    raise TripoError("The generated base-color texture has an invalid size.")
                image.load()
                images[material] = image.convert("RGB")
                image.close()
            except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
                raise TripoError("The generated base-color texture could not be decoded.") from None

        face_path = destination.with_suffix(".faces.tmp")
        output_sources = array("I")
        output_color_counts: list[list[int]] = []
        output_color_sums = array("Q")
        output_sample_counts = array("I")
        output_vertices: dict[int, int] = {}
        face_vertices = array("I")
        face_count = 0
        current_material = ""
        try:
            source_stream = obj_path.open("r", encoding="utf-8", errors="strict")
            face_stream = face_path.open("w", encoding="ascii", newline="\n")
        except OSError:
            raise TripoError("The generated OBJ could not be converted.") from None
        with source_stream, face_stream:
            for line in source_stream:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                keyword = fields[0].lower()
                if keyword == "usemtl":
                    current_material = stripped[len(fields[0]) :].strip()
                    safe_material = re.sub(r"[^A-Za-z0-9_.-]+", "_", current_material).strip("_") or "material"
                    face_stream.write("g material_" + safe_material + "\n")
                    continue
                if keyword in {"o", "g"}:
                    name = stripped[len(fields[0]) :].strip()
                    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_") or keyword
                    face_stream.write(keyword + " " + safe_name + "\n")
                    continue
                if keyword != "f":
                    continue
                if len(fields) != 4:
                    raise TripoError("The generated OBJ must contain only triangular faces.")
                material = current_material
                if not material and len(images) == 1:
                    material = next(iter(images))
                image = images.get(material)
                if image is None:
                    raise TripoError("The generated OBJ face is missing a base-color material.")
                pixels = image.load()
                face_indices: list[str] = []
                for field in fields[1:]:
                    indices = field.split("/")
                    if len(indices) < 2 or not indices[0] or not indices[1]:
                        raise TripoError("The generated OBJ face is missing texture coordinates.")
                    vertex_index = _resolve_obj_index(indices[0], len(positions) // 3, "vertex")
                    texcoord_index = _resolve_obj_index(indices[1], len(texcoords) // 2, "texture coordinate")
                    output_index = output_vertices.get(vertex_index)
                    if output_index is None:
                        output_sources.append(vertex_index)
                        output_index = len(output_sources)
                        output_vertices[vertex_index] = output_index
                        output_color_counts.append([0] * len(palette_rgb))
                        output_color_sums.extend((0, 0, 0))
                        output_sample_counts.append(0)
                    u = max(0.0, min(1.0, texcoords[texcoord_index * 2]))
                    v = max(0.0, min(1.0, texcoords[texcoord_index * 2 + 1]))
                    x = min(image.width - 1, max(0, round(u * (image.width - 1))))
                    y = min(image.height - 1, max(0, round((1.0 - v) * (image.height - 1))))
                    sampled = tuple(pixels[x, y])
                    if palette_rgb:
                        palette_index = _semantic_palette_index(
                            sampled,
                            palette_lab,
                            nearest_cache,
                            portrait_indices,
                            portrait_role_indices,
                        )
                        output_color_counts[output_index - 1][palette_index] += 1
                    if not palette_rgb or natural_destination is not None:
                        color_offset = (output_index - 1) * 3
                        for channel in range(3):
                            output_color_sums[color_offset + channel] += sampled[channel]
                        output_sample_counts[output_index - 1] += 1
                    face_indices.append(str(output_index))
                    face_vertices.append(output_index - 1)
                face_stream.write("f " + " ".join(face_indices) + "\n")
                face_count += 1

        if not output_sources or not face_count:
            raise TripoError("The generated OBJ does not contain textured faces.")
        output_palette_indices: list[int] = []
        if palette_rgb:
            output_palette_indices = [
                max(range(len(counts)), key=lambda item: (counts[item], -item))
                for counts in output_color_counts
            ]
            # Orca intentionally uses two-color triangles to encode an MMU boundary. Three-color triangles are much harder to
            # print predictably. Relabel the highest palette index in each offending face to one of its two lower labels. Every
            # edit is monotonic, so adjacent faces cannot oscillate forever; shared vertices and watertight topology stay intact.
            vertex_faces: list[list[int]] = [[] for _ in output_palette_indices]
            pending: deque[int] = deque()
            queued = bytearray(face_count)
            for face_index, offset in enumerate(range(0, len(face_vertices), 3)):
                vertices = face_vertices[offset:offset + 3]
                for vertex in vertices:
                    vertex_faces[vertex].append(face_index)
                if len({output_palette_indices[index] for index in vertices}) == 3:
                    pending.append(face_index)
                    queued[face_index] = 1
            changes = 0
            max_changes = len(output_palette_indices) * max(1, len(palette_rgb) - 1)
            while pending:
                face_index = pending.popleft()
                queued[face_index] = 0
                offset = face_index * 3
                vertices = face_vertices[offset:offset + 3]
                labels = [output_palette_indices[index] for index in vertices]
                if len(set(labels)) < 3:
                    continue
                current = max(labels)
                corner = labels.index(current)
                vertex = vertices[corner]
                counts = output_color_counts[vertex]
                targets = [label for label in labels if label < current]
                target = max(targets, key=lambda label: (counts[label], -label))
                output_palette_indices[vertex] = target
                changes += 1
                if changes > max_changes:
                    raise TripoError("The printable vertex-color pass did not converge.")
                for adjacent_face in vertex_faces[vertex]:
                    if not queued[adjacent_face]:
                        pending.append(adjacent_face)
                        queued[adjacent_face] = 1
            remaining_three_color_faces = sum(
                len({output_palette_indices[index] for index in face_vertices[offset:offset + 3]}) == 3
                for offset in range(0, len(face_vertices), 3)
            )
            if remaining_three_color_faces:
                raise TripoError(
                    f"The printable vertex-color pass left {remaining_three_color_faces} three-color triangles."
                )
        def write_output(output_path: Path, *, use_palette: bool) -> None:
            with output_path.open("w", encoding="ascii", newline="\n") as output:
                output.write("# OrcaSlicer AI vertex-color OBJ\n")
                output.write(f"# Source package: {obj_path.name}\n")
                for output_index, source_index in enumerate(output_sources):
                    offset = source_index * 3
                    if use_palette:
                        palette_index = output_palette_indices[output_index]
                        red, green, blue = palette_rgb[palette_index]
                    else:
                        samples = max(1, output_sample_counts[output_index])
                        color_offset = output_index * 3
                        red, green, blue = (
                            round(output_color_sums[color_offset + channel] / samples) for channel in range(3)
                        )
                    output.write(
                        "v {:.9g} {:.9g} {:.9g} {:.6f} {:.6f} {:.6f}\n".format(
                            positions[offset], positions[offset + 1], positions[offset + 2],
                            red / 255.0,
                            green / 255.0,
                            blue / 255.0,
                        )
                    )
                with face_path.open("r", encoding="ascii") as faces:
                    shutil.copyfileobj(faces, output, length=1024 * 1024)

        try:
            write_output(destination, use_palette=bool(palette_rgb))
            if natural_destination is not None:
                if natural_destination == destination:
                    raise TripoError("The natural portrait reference path must be separate from the printable model.")
                write_output(natural_destination, use_palette=False)
        except OSError:
            raise TripoError("The vertex-color OBJ could not be saved.") from None
        finally:
            try:
                face_path.unlink(missing_ok=True)
            except OSError:
                pass
    finally:
        for image in images.values():
            image.close()


def _quantize_vertex_color_obj(
    source: Path,
    destination: Path,
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None = None,
    portrait_materials: bool = False,
) -> None:
    if not palette:
        try:
            shutil.copyfile(source, destination)
        except OSError:
            raise TripoError("The generated OBJ could not be copied.") from None
        return
    palette_rgb, palette_lab = _vertex_color_data(palette)
    nearest_cache: dict[tuple[int, int, int], int] = {}
    portrait_indices = _portrait_material_palette_indices(
        palette, palette_roles, portrait_materials
    )
    portrait_role_indices = _portrait_material_role_indices(
        palette, palette_roles, portrait_materials
    )
    try:
        with source.open("r", encoding="utf-8", errors="strict") as input_stream, destination.open(
            "w", encoding="ascii", newline="\n"
        ) as output:
            output.write(
                "# OrcaSlicer AI palette-constrained vertex-color OBJ\n"
                if palette
                else "# OrcaSlicer AI natural vertex-color OBJ\n"
            )
            for line in input_stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    try:
                        values = [float(value) for value in fields[1:7]]
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex.") from None
                    sampled = tuple(round(max(0.0, min(1.0, value)) * 255) for value in values[3:6])
                    palette_index = _semantic_palette_index(
                        sampled,
                        palette_lab,
                        nearest_cache,
                        portrait_indices,
                        portrait_role_indices,
                    )
                    red, green, blue = palette_rgb[palette_index]
                    output.write(
                        "v {:.9g} {:.9g} {:.9g} {:.6f} {:.6f} {:.6f}\n".format(
                            values[0], values[1], values[2], red / 255.0, green / 255.0, blue / 255.0
                        )
                    )
                elif keyword == "f":
                    output.write("f " + " ".join(field.split("/", 1)[0] for field in fields[1:]) + "\n")
                elif keyword in {"o", "g"}:
                    output.write(" ".join(fields) + "\n")
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be color constrained.") from None


def _normalize_obj_for_orca(path: Path, target_size_mm: float = DEFAULT_MODEL_SIZE_MM) -> None:
    minimum = [math.inf, math.inf, math.inf]
    maximum = [-math.inf, -math.inf, -math.inf]
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].lower() != "v" or len(fields) not in {7, 8}:
                    continue
                values = [float(value) for value in fields[1:4]]
                for axis, value in enumerate(values):
                    minimum[axis] = min(minimum[axis], value)
                    maximum[axis] = max(maximum[axis], value)
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be normalized for OrcaSlicer.") from None
    spans = [maximum[axis] - minimum[axis] for axis in range(3)]
    largest_span = max(spans)
    if not math.isfinite(largest_span) or largest_span <= 1e-9 or target_size_mm <= 0:
        raise TripoError("The generated OBJ has invalid dimensions.")

    scale = target_size_mm / largest_span
    center_x = (minimum[0] + maximum[0]) * 0.5
    center_z = (minimum[2] + maximum[2]) * 0.5
    temporary = path.with_name(path.name + ".normalized")
    try:
        with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
            "w", encoding="ascii", newline="\n"
        ) as output:
            output.write("# OrcaSlicer AI normalized: Z-up, centered, on-bed, 100 mm maximum dimension\n")
            for line in source:
                fields = line.strip().split()
                if fields and fields[0].lower() == "v" and len(fields) in {7, 8}:
                    values = [float(value) for value in fields[1:]]
                    x = (values[0] - center_x) * scale
                    y = -(values[2] - center_z) * scale
                    z = (values[1] - minimum[1]) * scale
                    output.write(
                        "v {:.9g} {:.9g} {:.9g} {}\n".format(
                            x, y, z, " ".join("{:.6f}".format(value) for value in values[3:])
                        )
                    )
                elif fields and fields[0].lower() == "f":
                    output.write("f " + " ".join(fields[1:]) + "\n")
                elif fields and fields[0].lower() in {"o", "g"}:
                    output.write(" ".join(fields) + "\n")
        os.replace(temporary, path)
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be normalized for OrcaSlicer.") from None
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _stabilize_portrait_obj_materials(
    path: Path,
    report_path: Path,
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None,
    enabled: bool,
) -> dict[str, Any]:
    """Lock a detected portrait bust's bottom base to its semantic material.

    Tripo can paint a source-invisible base with garment, skin, or accent colours.
    This pass is intentionally narrower than generic colour cleanup: it requires
    upstream portrait evidence, an explicit neutral-garment/skin palette, and a
    substantial bottom band that is already predominantly the dark structure colour.
    """
    portrait_indices = _portrait_material_palette_indices(palette, palette_roles, enabled)
    if portrait_indices is None or not palette_roles:
        return {"status": "not_applicable", "recolored_vertices": 0}
    try:
        primary_hex = str(palette_roles["primary"]).strip().upper()
        structure_hex = str(palette_roles["structure"]).strip().upper()
        primary = tuple(int(primary_hex[index:index + 2], 16) for index in (1, 3, 5))
        structure = tuple(int(structure_hex[index:index + 2], 16) for index in (1, 3, 5))
    except (KeyError, ValueError):
        return {"status": "not_applicable", "recolored_vertices": 0}

    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].lower() != "v":
                    continue
                if len(fields) not in {7, 8}:
                    raise TripoError("The generated OBJ does not provide valid vertex colors.")
                position = tuple(float(value) for value in fields[1:4])
                color = tuple(round(float(value) * 255) for value in fields[4:7])
                if not all(math.isfinite(value) for value in position) or not all(
                    0 <= value <= 255 for value in color
                ):
                    raise TripoError("The generated OBJ has an invalid colored vertex.")
                positions.append(position)
                colors.append(color)
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be read for portrait material cleanup.") from None
    if not positions:
        raise TripoError("The generated OBJ has no vertices for portrait material cleanup.")

    minimum_z = min(position[2] for position in positions)
    maximum_z = max(position[2] for position in positions)
    span_z = maximum_z - minimum_z
    if span_z <= 1e-9:
        raise TripoError("The generated portrait has invalid dimensions for material cleanup.")
    base_top = minimum_z + span_z * 0.065
    base_indices = [index for index, position in enumerate(positions) if position[2] <= base_top]
    base_structure_ratio = (
        sum(colors[index] == structure for index in base_indices) / len(base_indices)
        if base_indices else 0.0
    )
    primary_ratio = sum(color == primary for color in colors) / len(colors)
    activated = (
        len(base_indices) / len(positions) >= 0.02
        and base_structure_ratio >= 0.65
        and primary_ratio >= 0.25
    )
    report: dict[str, Any] = {
        "status": "not_applicable",
        "activated": activated,
        "vertex_count": len(positions),
        "primary_vertex_ratio": round(primary_ratio, 6),
        "base_vertex_ratio": round(len(base_indices) / len(positions), 6),
        "base_structure_ratio": round(base_structure_ratio, 6),
        "base_height_ratio": 0.065,
        "recolored_vertices": 0,
        "recolored_by_rule": {"base": 0},
        "recolored_by_source": {},
    }
    if not activated:
        _write_mesh_repair_report(report_path, report)
        return report

    changed_by_rule: Counter[str] = Counter()
    changed_by_source: Counter[tuple[int, int, int]] = Counter()
    updated = list(colors)
    for index, (position, source) in enumerate(zip(positions, colors)):
        target = source
        rule = ""
        if position[2] <= base_top:
            target = structure
            rule = "base"
        if target != source:
            updated[index] = target
            changed_by_rule[rule] += 1
            changed_by_source[source] += 1

    if changed_by_rule:
        temporary = path.with_name(path.name + ".portrait-materials")
        vertex_index = 0
        try:
            with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
                "w", encoding="ascii", newline="\n"
            ) as output:
                for line in source:
                    fields = line.strip().split()
                    if fields and fields[0].lower() == "v":
                        red, green, blue = updated[vertex_index]
                        fields[4:7] = [f"{channel / 255.0:.6f}" for channel in (red, green, blue)]
                        output.write(" ".join(fields) + "\n")
                        vertex_index += 1
                    else:
                        output.write(line if line.endswith("\n") else line + "\n")
            os.replace(temporary, path)
        except (OSError, UnicodeDecodeError):
            raise TripoError("Portrait materials could not be stabilized safely.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    report["status"] = "stabilized" if changed_by_rule else "not_needed"
    report["recolored_vertices"] = sum(changed_by_rule.values())
    report["recolored_by_rule"] = {
        "base": changed_by_rule["base"],
    }
    report["recolored_by_source"] = {
        "#{:02X}{:02X}{:02X}".format(*color): count
        for color, count in sorted(changed_by_source.items())
    }
    _write_mesh_repair_report(report_path, report)
    return report


def _stabilize_portrait_obj_garment_regions(
    path: Path,
    report_path: Path,
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None,
    enabled: bool,
) -> dict[str, Any]:
    """Remove sparse Tripo material noise from a detected portrait bust.

    The generic colour-island pass intentionally has a small global budget and
    cannot clean thin colour tendrils that remain connected through shared
    vertices. A portrait bust gives us stronger evidence: below the head, its
    source-invisible rear surface belongs to the dominant garment, while real
    hands and the front accent garment form locally coherent regions. This pass
    erodes only non-garment vertices that are surrounded by garment vertices and
    cleans weakly supported rear projections. It stays disabled for every model
    without the upstream portrait and dark-base evidence.
    """
    portrait_indices = _portrait_material_palette_indices(palette, palette_roles, enabled)
    if portrait_indices is None or not palette_roles:
        return {"status": "not_applicable", "activated": False, "recolored_vertices": 0}
    try:
        role_colors = {
            role: tuple(
                int(str(palette_roles[role]).strip().upper()[index:index + 2], 16)
                for index in (1, 3, 5)
            )
            for role in ("primary", "structure", "light", "accent")
        }
    except (KeyError, ValueError):
        return {"status": "not_applicable", "activated": False, "recolored_vertices": 0}
    if len(set(role_colors.values())) != 4:
        return {"status": "not_applicable", "activated": False, "recolored_vertices": 0}

    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    position = tuple(float(value) for value in fields[1:4])
                    color = tuple(round(float(value) * 255) for value in fields[4:7])
                    if not all(math.isfinite(value) for value in position) or not all(
                        0 <= value <= 255 for value in color
                    ):
                        raise TripoError("The generated OBJ has an invalid colored vertex.")
                    positions.append(position)
                    colors.append(color)
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    faces.append(tuple(_resolve_obj_index(value, len(positions), "vertex") for value in fields[1:]))
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be read for portrait garment cleanup.") from None
    if not positions or not faces:
        raise TripoError("The generated OBJ has no usable geometry for portrait garment cleanup.")

    minimum_x = min(position[0] for position in positions)
    maximum_x = max(position[0] for position in positions)
    minimum_z = min(position[2] for position in positions)
    maximum_z = max(position[2] for position in positions)
    span_x = maximum_x - minimum_x
    span_z = maximum_z - minimum_z
    if span_x <= 1e-9 or span_z <= 1e-9:
        raise TripoError("The generated portrait has invalid dimensions for garment cleanup.")

    primary = role_colors["primary"]
    structure = role_colors["structure"]
    base_top = minimum_z + span_z * 0.065
    base_indices = [index for index, position in enumerate(positions) if position[2] <= base_top]
    base_structure_ratio = (
        sum(colors[index] == structure for index in base_indices) / len(base_indices)
        if base_indices else 0.0
    )
    primary_ratio = sum(color == primary for color in colors) / len(colors)
    activated = (
        len(base_indices) / len(positions) >= 0.02
        and base_structure_ratio >= 0.65
        and primary_ratio >= 0.25
    )
    report: dict[str, Any] = {
        "status": "not_applicable",
        "activated": activated,
        "vertex_count": len(positions),
        "face_count": len(faces),
        "primary_vertex_ratio": round(primary_ratio, 6),
        "base_structure_ratio": round(base_structure_ratio, 6),
        "rear_garment_height_ratio": PORTRAIT_REAR_GARMENT_HEIGHT_RATIO,
        "passes": [],
        "recolored_vertices": 0,
        "recolored_by_source": {},
    }
    if not activated:
        _write_mesh_repair_report(report_path, report)
        return report

    body_x = sorted(
        position[0]
        for position in positions
        if base_top < position[2] < minimum_z + span_z * PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
    )
    rear_threshold = (
        body_x[round((len(body_x) - 1) * PORTRAIT_FRONT_SURFACE_QUANTILE)]
        if body_x else minimum_x + span_x * PORTRAIT_FRONT_SURFACE_QUANTILE
    )
    body_y = [
        position[1]
        for position in positions
        if base_top < position[2] < minimum_z + span_z * PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
    ]
    minimum_body_y = min(body_y) if body_y else min(position[1] for position in positions)
    maximum_body_y = max(body_y) if body_y else max(position[1] for position in positions)
    body_y_center = (minimum_body_y + maximum_body_y) * 0.5
    body_y_span = max(maximum_body_y - minimum_body_y, 1e-9)
    changed_by_source: Counter[tuple[int, int, int]] = Counter()
    changed_by_rule: Counter[str] = Counter()
    pass_reports: list[dict[str, Any]] = []
    for pass_index in range(PORTRAIT_GARMENT_SMOOTHING_PASSES):
        same_support = array("I", [0]) * len(positions)
        primary_support = array("I", [0]) * len(positions)
        for face in faces:
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                left_color, right_color = colors[left], colors[right]
                if left_color == right_color:
                    same_support[left] += 1
                    same_support[right] += 1
                if right_color == primary:
                    primary_support[left] += 1
                if left_color == primary:
                    primary_support[right] += 1

        changed_indices: list[int] = []
        rear_changes = 0
        support_changes = 0
        for index, (position, source) in enumerate(zip(positions, colors)):
            if source == primary or position[2] <= base_top:
                continue
            height_ratio = (position[2] - minimum_z) / span_z
            if height_ratio >= PORTRAIT_REAR_GARMENT_HEIGHT_RATIO:
                continue
            rear = position[0] < rear_threshold
            front_accent_region = (
                source == role_colors["accent"]
                and position[0] >= rear_threshold - span_x * 0.05
                and abs(position[1] - body_y_center) <= body_y_span * 0.22
                and 0.18 <= height_ratio < PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
            )
            weak_rear_projection = (
                source != role_colors["light"]
                and rear
                and (
                    source == role_colors["accent"]
                    or height_ratio < PORTRAIT_REAR_HAIR_HEIGHT_RATIO
                    or same_support[index] < 4
                )
            )
            surrounded_by_garment = (
                source != role_colors["light"]
                and not front_accent_region
                and primary_support[index] >= 4
                and primary_support[index] >= same_support[index] + 2
                and primary_support[index] * 2 >= same_support[index] * 3
            )
            if not weak_rear_projection and not surrounded_by_garment:
                continue
            changed_indices.append(index)
            rear_changes += int(weak_rear_projection)
            support_changes += int(not weak_rear_projection and surrounded_by_garment)

        for index in changed_indices:
            changed_by_source[colors[index]] += 1
            colors[index] = primary
        changed_by_rule["rear_or_sparse_garment"] += len(changed_indices)
        pass_reports.append({
            "pass": pass_index + 1,
            "recolored_vertices": len(changed_indices),
            "rear_garment_vertices": rear_changes,
            "surrounded_vertices": support_changes,
        })
        if not changed_indices:
            break

    parent = list(range(len(positions)))

    def find(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    def unite(left: int, right: int) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parent[right_root] = left_root

    for face in faces:
        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            if colors[left] == colors[right]:
                unite(left, right)

    component_vertices: Counter[int] = Counter()
    component_color: dict[int, tuple[int, int, int]] = {}
    component_minimum_y: dict[int, float] = {}
    component_maximum_y: dict[int, float] = {}
    component_minimum_z: dict[int, float] = {}
    component_maximum_z: dict[int, float] = {}
    component_sum_x: Counter[int] = Counter()
    component_sum_y: Counter[int] = Counter()
    component_sum_z: Counter[int] = Counter()
    for index, (position, color) in enumerate(zip(positions, colors)):
        root = find(index)
        component_vertices[root] += 1
        component_color[root] = color
        component_minimum_y[root] = min(component_minimum_y.get(root, position[1]), position[1])
        component_maximum_y[root] = max(component_maximum_y.get(root, position[1]), position[1])
        component_minimum_z[root] = min(component_minimum_z.get(root, position[2]), position[2])
        component_maximum_z[root] = max(component_maximum_z.get(root, position[2]), position[2])
        component_sum_x[root] += position[0]
        component_sum_y[root] += position[1]
        component_sum_z[root] += position[2]

    skin = role_colors["light"]
    skin_components = sorted(
        (root for root, color in component_color.items() if color == skin),
        key=lambda root: (-component_vertices[root], root),
    )
    protected_skin_roots: set[int] = set()
    face_skin_root: int | None = None
    for root in skin_components:
        maximum_height = (component_maximum_z[root] - minimum_z) / span_z
        if maximum_height >= 0.65:
            protected_skin_roots.add(root)
            face_skin_root = root
            break
    minimum_hand_vertices = max(8, int(len(positions) * 0.001))
    for root in skin_components:
        if root in protected_skin_roots or len(protected_skin_roots) >= 3:
            continue
        minimum_height = (component_minimum_z[root] - minimum_z) / span_z
        maximum_height = (component_maximum_z[root] - minimum_z) / span_z
        centroid_x = component_sum_x[root] / component_vertices[root]
        if (
            component_vertices[root] >= minimum_hand_vertices
            and minimum_height >= PORTRAIT_HAND_MIN_HEIGHT_RATIO
            and maximum_height <= 0.65
            and centroid_x >= rear_threshold
        ):
            protected_skin_roots.add(root)

    protected_hand_roots = set(protected_skin_roots)
    if face_skin_root is not None:
        protected_hand_roots.discard(face_skin_root)
    compact_hand_roots = {
        root for root in protected_hand_roots
        if component_vertices[root] >= minimum_hand_vertices
        and (
            (component_maximum_y[root] - component_minimum_y[root]) / body_y_span
            + (component_maximum_z[root] - component_minimum_z[root]) / span_z
        ) <= PORTRAIT_HAND_COMPACT_EXTENT_RATIO
    }
    largest_compact_hand = max(
        (component_vertices[root] for root in compact_hand_roots),
        default=0,
    )
    discarded_diffuse_skin_roots = {
        root for root in protected_hand_roots
        if largest_compact_hand > 0
        and root not in compact_hand_roots
        and component_vertices[root] >= largest_compact_hand * PORTRAIT_HAND_DIFFUSE_SIZE_RATIO
        and (
            (component_maximum_y[root] - component_minimum_y[root]) / body_y_span
            + (component_maximum_z[root] - component_minimum_z[root]) / span_z
        ) > PORTRAIT_HAND_COMPACT_EXTENT_RATIO
    }
    protected_hand_roots.difference_update(discarded_diffuse_skin_roots)
    protected_skin_roots.difference_update(discarded_diffuse_skin_roots)
    structure_front_threshold = (
        body_x[round((len(body_x) - 1) * PORTRAIT_STRUCTURE_FRONT_QUANTILE)]
        if body_x else rear_threshold
    )
    accent = role_colors["accent"]
    accent_candidates: list[tuple[int, int, float]] = []
    # Tripo frequently splits one coherent scarf or shirt panel across UV seams.
    # Keep small front-centre fragments long enough to be grouped semantically;
    # off-centre and rear accent speckles are still removed below.
    minimum_accent_vertices = max(4, int(len(positions) * 0.00001))
    for root, color in component_color.items():
        if color != accent:
            continue
        count = component_vertices[root]
        centroid_x = component_sum_x[root] / count
        centroid_y = component_sum_y[root] / count
        centroid_z_ratio = (component_sum_z[root] / count - minimum_z) / span_z
        if (
            centroid_x >= rear_threshold - span_x * 0.05
            and abs(centroid_y - body_y_center) <= body_y_span * 0.22
            and 0.18 <= centroid_z_ratio < PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
            and count >= minimum_accent_vertices
        ):
            accent_candidates.append((root, count, centroid_z_ratio))
    protected_accent_roots: set[int] = set()
    accent_anchor_height_ratio = 0.0
    if accent_candidates:
        anchor_root, anchor_count, _ = max(accent_candidates, key=lambda item: (item[1], -item[0]))
        accent_anchor_height_ratio = (component_minimum_z[anchor_root] - minimum_z) / span_z
        minimum_related_height = max(0.18, accent_anchor_height_ratio - 0.07)
        protected_accent_roots = {
            root
            for root, count, centroid_z_ratio in accent_candidates
            if root == anchor_root
            or centroid_z_ratio >= minimum_related_height
            or count >= anchor_count * 0.75
        }
    component_neighbor_colors: dict[int, Counter[tuple[int, int, int]]] = {}
    for face in faces:
        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            left_root, right_root = find(left), find(right)
            if left_root == right_root:
                continue
            component_neighbor_colors.setdefault(left_root, Counter())[colors[right]] += 1
            component_neighbor_colors.setdefault(right_root, Counter())[colors[left]] += 1
    maximum_accent_shadow_vertices = max(24, int(len(positions) * 0.003))
    enclosed_accent_shadow_roots: set[int] = set()
    for root, color in component_color.items():
        if color != structure:
            continue
        count = component_vertices[root]
        centroid_x = component_sum_x[root] / count
        centroid_y = component_sum_y[root] / count
        centroid_z_ratio = (component_sum_z[root] / count - minimum_z) / span_z
        accent_boundary = component_neighbor_colors.get(root, Counter())[accent]
        required_boundary = max(4, min(16, round(math.sqrt(count))))
        if (
            count <= maximum_accent_shadow_vertices
            and accent_boundary >= required_boundary
            and centroid_x >= structure_front_threshold
            and abs(centroid_y - body_y_center) <= body_y_span * 0.18
            and 0.24 <= centroid_z_ratio < PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
            and (component_maximum_y[root] - component_minimum_y[root]) <= body_y_span * 0.16
            and (component_maximum_z[root] - component_minimum_z[root]) <= span_z * 0.12
        ):
            enclosed_accent_shadow_roots.add(root)
    semantic_updated = list(colors)
    for index, (position, source) in enumerate(zip(positions, colors)):
        if source == primary or position[2] <= base_top:
            continue
        height_ratio = (position[2] - minimum_z) / span_z
        front_centered = (
            position[0] >= structure_front_threshold
            and abs(position[1] - body_y_center) <= body_y_span * 0.22
        )
        keep = True
        if source == skin:
            root = find(index)
            keep = root in protected_skin_roots
        elif source == accent:
            keep = find(index) in protected_accent_roots
        elif source == structure:
            if find(index) in enclosed_accent_shadow_roots:
                semantic_updated[index] = accent
                changed_by_source[source] += 1
                changed_by_rule["enclosed_accent_shadow"] += 1
                continue
            keep = (
                height_ratio >= PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
                or (
                    front_centered
                    and 0.26 <= height_ratio < PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
                )
            )
        if not keep:
            semantic_updated[index] = primary
            changed_by_source[source] += 1
            changed_by_rule[
                "diffuse_skin_component" if source == skin and root in discarded_diffuse_skin_roots
                else "skin_component" if source == skin
                else "accent_ownership" if source == accent
                else "structure_ownership"
            ] += 1
    hand_component_sizes = Counter(
        find(index)
        for index, source in enumerate(semantic_updated)
        if source == skin and find(index) in protected_hand_roots
    )
    hand_trimmed_by_root: Counter[int] = Counter()
    hand_pass_reports: list[dict[str, Any]] = []
    for pass_index in range(PORTRAIT_HAND_BOUNDARY_PASSES):
        hand_same_support = array("I", [0]) * len(positions)
        hand_primary_support = array("I", [0]) * len(positions)
        for face in faces:
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                left_color, right_color = semantic_updated[left], semantic_updated[right]
                if left_color == right_color:
                    hand_same_support[left] += 1
                    hand_same_support[right] += 1
                if right_color == primary:
                    hand_primary_support[left] += 1
                if left_color == primary:
                    hand_primary_support[right] += 1

        candidates_by_root: dict[int, list[int]] = {}
        for index, source in enumerate(semantic_updated):
            if source != skin:
                continue
            root = find(index)
            if root not in protected_hand_roots:
                continue
            primary_neighbors = hand_primary_support[index]
            same_neighbors = hand_same_support[index]
            if (
                primary_neighbors >= PORTRAIT_HAND_BOUNDARY_MIN_PRIMARY_SUPPORT
                and primary_neighbors > same_neighbors
            ):
                candidates_by_root.setdefault(root, []).append(index)

        changed_this_pass = 0
        for root, candidates in candidates_by_root.items():
            maximum_trim = max(
                1,
                int(hand_component_sizes[root] * PORTRAIT_HAND_BOUNDARY_MAX_REMOVAL_RATIO),
            )
            remaining_budget = maximum_trim - hand_trimmed_by_root[root]
            if remaining_budget <= 0:
                continue
            candidates.sort(
                key=lambda index: (
                    hand_primary_support[index] - hand_same_support[index],
                    hand_primary_support[index],
                    -hand_same_support[index],
                    -index,
                ),
                reverse=True,
            )
            selected = candidates[:remaining_budget]
            for index in selected:
                semantic_updated[index] = primary
                changed_by_source[skin] += 1
            hand_trimmed_by_root[root] += len(selected)
            changed_this_pass += len(selected)
        hand_pass_reports.append({
            "pass": pass_index + 1,
            "candidate_vertices": sum(len(items) for items in candidates_by_root.values()),
            "recolored_vertices": changed_this_pass,
        })
        changed_by_rule["hand_boundary"] += changed_this_pass
        if not changed_this_pass:
            break

    minimum_face_detail_vertices = max(4, int(len(positions) * 0.0001))
    maximum_face_detail_vertices = max(16, int(len(positions) * 0.002))
    face_primary_roots = []
    for root, color in component_color.items():
        if color != primary:
            continue
        count = component_vertices[root]
        if not minimum_face_detail_vertices <= count <= maximum_face_detail_vertices:
            continue
        centroid_x = component_sum_x[root] / count
        centroid_y = component_sum_y[root] / count
        minimum_height = (component_minimum_z[root] - minimum_z) / span_z
        maximum_height = (component_maximum_z[root] - minimum_z) / span_z
        if (
            centroid_x >= structure_front_threshold
            and abs(centroid_y - body_y_center) <= body_y_span * 0.25
            and minimum_height >= 0.78
        ):
            face_primary_roots.append((root, centroid_y, minimum_height, maximum_height))

    forehead_highlight_roots = {
        root for root, _, minimum_height, _ in face_primary_roots
        if minimum_height >= 0.86
    }
    eye_candidates = [
        item for item in face_primary_roots
        if 0.78 <= item[2] and item[3] <= 0.86
    ]
    left_eyes = [item for item in eye_candidates if item[1] < body_y_center]
    right_eyes = [item for item in eye_candidates if item[1] > body_y_center]
    paired_eye_roots: set[int] = set()
    if left_eyes and right_eyes:
        pair = min(
            ((left, right) for left in left_eyes for right in right_eyes),
            key=lambda items: (
                abs((items[0][2] + items[0][3]) - (items[1][2] + items[1][3])),
                abs(math.log(max(1, component_vertices[items[0][0]]) / max(1, component_vertices[items[1][0]]))),
            ),
        )
        left_root, right_root = pair[0][0], pair[1][0]
        size_ratio = max(component_vertices[left_root], component_vertices[right_root]) / max(
            1, min(component_vertices[left_root], component_vertices[right_root])
        )
        if size_ratio <= 3.0:
            paired_eye_roots.update((left_root, right_root))

    eye_upper_cuts = {
        root: component_minimum_z[root]
        + (component_maximum_z[root] - component_minimum_z[root]) * 0.45
        for root in paired_eye_roots
    }
    face_highlight_changes = 0
    eye_upper_changes = 0
    for index, (position, source) in enumerate(zip(positions, semantic_updated)):
        if source != primary:
            continue
        root = find(index)
        if root in forehead_highlight_roots:
            semantic_updated[index] = skin
            changed_by_source[primary] += 1
            changed_by_rule["face_highlight"] += 1
            face_highlight_changes += 1
        elif root in paired_eye_roots and position[2] > eye_upper_cuts[root]:
            semantic_updated[index] = skin
            changed_by_source[primary] += 1
            changed_by_rule["eye_upper_white"] += 1
            eye_upper_changes += 1
    colors = semantic_updated

    recolored_vertices = sum(changed_by_source.values())
    if recolored_vertices:
        temporary = path.with_name(path.name + ".portrait-garment")
        vertex_index = 0
        try:
            with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
                "w", encoding="ascii", newline="\n"
            ) as output:
                for line in source:
                    fields = line.strip().split()
                    if fields and fields[0].lower() == "v":
                        red, green, blue = colors[vertex_index]
                        fields[4:7] = [f"{channel / 255.0:.6f}" for channel in (red, green, blue)]
                        output.write(" ".join(fields) + "\n")
                        vertex_index += 1
                    else:
                        output.write(line if line.endswith("\n") else line + "\n")
            os.replace(temporary, path)
        except (OSError, UnicodeDecodeError):
            raise TripoError("Portrait garment regions could not be stabilized safely.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    report["status"] = "stabilized" if recolored_vertices else "not_needed"
    report["passes"] = pass_reports
    report["rear_threshold_x"] = round(rear_threshold, 6)
    report["structure_front_threshold_x"] = round(structure_front_threshold, 6)
    report["protected_skin_components"] = len(protected_skin_roots)
    report["protected_hand_components"] = len(protected_hand_roots)
    report["discarded_diffuse_skin_components"] = len(discarded_diffuse_skin_roots)
    report["discarded_diffuse_skin_vertices"] = sum(
        component_vertices[root] for root in discarded_diffuse_skin_roots
    )
    report["hand_compact_extent_ratio"] = PORTRAIT_HAND_COMPACT_EXTENT_RATIO
    report["hand_diffuse_size_ratio"] = PORTRAIT_HAND_DIFFUSE_SIZE_RATIO
    report["hand_minimum_height_ratio"] = PORTRAIT_HAND_MIN_HEIGHT_RATIO
    report["hand_boundary_passes"] = hand_pass_reports
    report["hand_boundary_max_removal_ratio"] = PORTRAIT_HAND_BOUNDARY_MAX_REMOVAL_RATIO
    report["forehead_highlight_components"] = len(forehead_highlight_roots)
    report["paired_eye_white_components"] = len(paired_eye_roots)
    report["face_highlight_recolored_vertices"] = face_highlight_changes
    report["eye_upper_recolored_vertices"] = eye_upper_changes
    report["protected_accent_components"] = len(protected_accent_roots)
    report["accent_candidate_components"] = len(accent_candidates)
    report["accent_anchor_height_ratio"] = round(accent_anchor_height_ratio, 6)
    report["minimum_accent_vertices"] = minimum_accent_vertices
    report["enclosed_accent_shadow_components"] = len(enclosed_accent_shadow_roots)
    report["maximum_accent_shadow_vertices"] = maximum_accent_shadow_vertices
    report["enclosed_accent_shadow_recolored_vertices"] = changed_by_rule["enclosed_accent_shadow"]
    report["recolored_vertices"] = recolored_vertices
    report["recolored_by_rule"] = dict(sorted(changed_by_rule.items()))
    report["recolored_by_source"] = {
        "#{:02X}{:02X}{:02X}".format(*color): count
        for color, count in sorted(changed_by_source.items())
    }
    _write_mesh_repair_report(report_path, report)
    return report


def _capture_portrait_front_face_details(
    path: Path,
    palette_roles: Mapping[str, str] | None,
) -> tuple[dict[int, tuple[int, int, int]], dict[str, Any]]:
    """Remember trusted textured details on the actually visible front face.

    The provider texture already aligns eyes, brows, mouth and teeth with the
    generated geometry. Generic printable-colour cleanup can erase those tiny
    but meaningful regions. Capturing the original vertex labels after mesh
    repair lets us restore the same vertices later without projecting a flat
    image through the sides of a three-dimensional head.
    """

    try:
        role_colors = {
            role: tuple(
                int(str((palette_roles or {})[role]).strip().upper()[index:index + 2], 16)
                for index in (1, 3, 5)
            )
            for role in ("primary", "structure", "light", "accent")
        }
    except (KeyError, ValueError):
        return {}, {"status": "not_applicable", "reason": "palette_roles_missing"}
    if len(set(role_colors.values())) != 4:
        return {}, {"status": "not_applicable", "reason": "palette_roles_ambiguous"}

    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].lower() != "v":
                    continue
                if len(fields) not in {7, 8}:
                    raise TripoError("The generated OBJ does not provide valid vertex colors.")
                position = tuple(float(value) for value in fields[1:4])
                color = tuple(round(float(value) * 255) for value in fields[4:7])
                if not all(math.isfinite(value) for value in position) or not all(
                    0 <= value <= 255 for value in color
                ):
                    raise TripoError("The generated OBJ has an invalid colored vertex.")
                positions.append(position)
                colors.append(color)
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The portrait face details could not be captured safely.") from None
    if not positions:
        raise TripoError("The generated OBJ has no vertices for portrait face-detail capture.")

    minimum_y = min(position[1] for position in positions)
    maximum_y = max(position[1] for position in positions)
    minimum_z = min(position[2] for position in positions)
    maximum_z = max(position[2] for position in positions)
    span_y = maximum_y - minimum_y
    span_z = maximum_z - minimum_z
    if span_y <= 1e-9 or span_z <= 1e-9:
        raise TripoError("The generated portrait has invalid dimensions for face-detail capture.")
    center_y = (minimum_y + maximum_y) * 0.5

    def face_cell(position: tuple[float, float, float]) -> tuple[int, int]:
        horizontal = round(
            (position[1] - minimum_y) / span_y * (PORTRAIT_FACE_DETAIL_GRID_SIZE - 1)
        )
        vertical = round(
            (position[2] - minimum_z) / span_z * (PORTRAIT_FACE_DETAIL_GRID_SIZE - 1)
        )
        return (
            max(0, min(PORTRAIT_FACE_DETAIL_GRID_SIZE - 1, horizontal)),
            max(0, min(PORTRAIT_FACE_DETAIL_GRID_SIZE - 1, vertical)),
        )

    front_surface: dict[tuple[int, int], float] = {}
    for position in positions:
        height_ratio = (position[2] - minimum_z) / span_z
        if (
            not PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO <= height_ratio < PORTRAIT_FACE_DETAIL_MAX_HEIGHT_RATIO
            or abs(position[1] - center_y) > span_y * PORTRAIT_FACE_DETAIL_HALF_WIDTH_RATIO
        ):
            continue
        cell = face_cell(position)
        front_surface[cell] = max(front_surface.get(cell, -math.inf), position[0])

    captured: dict[int, tuple[int, int, int]] = {}
    captured_by_target: Counter[tuple[int, int, int]] = Counter()
    for index, (position, color) in enumerate(zip(positions, colors)):
        if color not in {role_colors["structure"], role_colors["primary"]}:
            continue
        height_ratio = (position[2] - minimum_z) / span_z
        minimum_height = (
            PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO + 0.015
            if color == role_colors["structure"]
            else PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO
        )
        if (
            not minimum_height <= height_ratio < PORTRAIT_FACE_DETAIL_MAX_HEIGHT_RATIO
            or abs(position[1] - center_y) > span_y * PORTRAIT_FACE_DETAIL_HALF_WIDTH_RATIO
        ):
            continue
        maximum_front_x = front_surface.get(face_cell(position))
        if (
            maximum_front_x is None
            or position[0] < maximum_front_x - PORTRAIT_FACE_DETAIL_SURFACE_TOLERANCE_MM
        ):
            continue
        captured[index] = color
        captured_by_target[color] += 1

    report = {
        "status": "captured" if captured else "not_needed",
        "vertex_count": len(positions),
        "captured_vertices": len(captured),
        "captured_by_target": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(captured_by_target.items())
        },
        "minimum_height_ratio": PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO,
        "structure_minimum_height_ratio": PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO + 0.015,
        "maximum_height_ratio": PORTRAIT_FACE_DETAIL_MAX_HEIGHT_RATIO,
        "half_width_ratio": PORTRAIT_FACE_DETAIL_HALF_WIDTH_RATIO,
        "surface_tolerance_mm": PORTRAIT_FACE_DETAIL_SURFACE_TOLERANCE_MM,
        "surface_grid_size": PORTRAIT_FACE_DETAIL_GRID_SIZE,
    }
    return captured, report


def _restore_portrait_front_face_details(
    path: Path,
    report_path: Path,
    captured: Mapping[int, tuple[int, int, int]],
    capture_report: Mapping[str, Any],
    *,
    allowed_targets: set[tuple[int, int, int]] | None = None,
) -> dict[str, Any]:
    report = dict(capture_report)
    report["allowed_targets"] = (
        ["#{:02X}{:02X}{:02X}".format(*color) for color in sorted(allowed_targets)]
        if allowed_targets is not None else "all"
    )
    if not captured:
        report["recolored_vertices"] = 0
        _write_mesh_repair_report(report_path, report)
        return report

    changed_by_source: Counter[tuple[int, int, int]] = Counter()
    changed_by_target: Counter[tuple[int, int, int]] = Counter()
    vertex_index = 0
    temporary = path.with_name(path.name + ".face-details")
    try:
        with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
            "w", encoding="ascii", newline="\n"
        ) as output:
            for line in source:
                fields = line.strip().split()
                if fields and fields[0].lower() == "v":
                    target = captured.get(vertex_index)
                    if target is not None and (
                        allowed_targets is None or target in allowed_targets
                    ):
                        current = tuple(round(float(value) * 255) for value in fields[4:7])
                        if current != target:
                            fields[4:7] = [f"{channel / 255.0:.6f}" for channel in target]
                            changed_by_source[current] += 1
                            changed_by_target[target] += 1
                        output.write(" ".join(fields) + "\n")
                    else:
                        output.write(line if line.endswith("\n") else line + "\n")
                    vertex_index += 1
                else:
                    output.write(line if line.endswith("\n") else line + "\n")
        if vertex_index != int(capture_report.get("vertex_count", -1)):
            raise TripoError("Portrait face details no longer match the repaired mesh.")
        os.replace(temporary, path)
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The portrait face details could not be restored safely.") from None
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass

    recolored_vertices = sum(changed_by_target.values())
    report["status"] = "restored" if recolored_vertices else "not_needed"
    report["recolored_vertices"] = recolored_vertices
    report["recolored_by_source"] = {
        "#{:02X}{:02X}{:02X}".format(*color): count
        for color, count in sorted(changed_by_source.items())
    }
    report["recolored_by_target"] = {
        "#{:02X}{:02X}{:02X}".format(*color): count
        for color, count in sorted(changed_by_target.items())
    }
    _write_mesh_repair_report(report_path, report)
    return report


def _review_portrait_rear_plate_masks(mask_directory: Path) -> dict[str, Any]:
    """Reject the long near-vertical silhouette left by an extruded backdrop."""

    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError:
        raise PortraitGeometryGateError(
            "Pillow is required to inspect portrait geometry silhouettes."
        ) from None

    report: dict[str, Any] = {
        "version": "portrait-rear-plate-v1",
        "status": "pass",
        "views": {},
        "warnings": [],
    }
    try:
        for view in ("right", "left"):
            with Image.open(mask_directory / f"{view}.png") as opened:
                mask = opened.convert("L")
            bbox = mask.getbbox()
            if bbox is None:
                raise PortraitGeometryGateError(
                    f"The {view} portrait silhouette is empty."
                )
            left, top, right, bottom = bbox
            object_height = bottom - top
            analysis_bottom = min(bottom, top + int(round(object_height * 0.88)))
            tolerance = max(3, int(round(mask.width * 0.01)))
            rows: list[tuple[int, int, int]] = []
            pixels = mask.load()
            for y in range(top, analysis_bottom):
                occupied = [x for x in range(left, right) if pixels[x, y] >= 128]
                if occupied:
                    rows.append((y, occupied[0], occupied[-1]))

            view_report: dict[str, Any] = {
                "bbox": list(bbox),
                "tolerance_px": tolerance,
                "edges": {},
            }
            for edge_name, field_index in (("left", 1), ("right", 2)):
                best_run = 0
                best_center = 0
                best_start = top
                best_end = top
                centers = sorted({row[field_index] for row in rows})
                for center in centers:
                    run = 0
                    run_start = top
                    previous_y: int | None = None
                    for row in rows:
                        y = row[0]
                        matches = abs(row[field_index] - center) <= tolerance
                        if matches and (previous_y is None or y == previous_y + 1):
                            if run == 0:
                                run_start = y
                            run += 1
                        elif matches:
                            run = 1
                            run_start = y
                        else:
                            run = 0
                        previous_y = y
                        if run > best_run:
                            best_run = run
                            best_center = center
                            best_start = run_start
                            best_end = y
                run_ratio = best_run / max(1, object_height)
                start_ratio = (best_start - top) / max(1, object_height)
                suspicious = (
                    run_ratio >= PORTRAIT_REAR_PLATE_MIN_RUN_RATIO
                    and start_ratio <= PORTRAIT_REAR_PLATE_MAX_START_RATIO
                )
                view_report["edges"][edge_name] = {
                    "x": best_center,
                    "start_y": best_start,
                    "end_y": best_end,
                    "run_px": best_run,
                    "run_ratio": round(run_ratio, 6),
                    "start_ratio": round(start_ratio, 6),
                    "suspicious": suspicious,
                }
                if suspicious:
                    report["warnings"].append(f"{view}_{edge_name}_rear_plate")
            report["views"][view] = view_report
    except PortraitGeometryGateError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
        raise PortraitGeometryGateError(
            "The portrait side silhouettes could not be inspected."
        ) from None
    if report["warnings"]:
        report["status"] = "reject"
    return report


def _prepare_obj_artifact(
    raw_download: Path,
    job_directory: Path,
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None = None,
    portrait_materials: bool = False,
    front_material_reference: Path | None = None,
    status_callback: Callable[[str, int], None] | None = None,
    build_aligned_portrait_reference: bool = False,
) -> Path:
    def report_status(message: str, progress: int) -> None:
        if status_callback is not None:
            status_callback(message, progress)

    try:
        with raw_download.open("rb") as stream:
            signature = stream.read(4)
    except OSError:
        raise TripoError("The generated artifact could not be read.") from None
    destination = job_directory / "model-vertex-color.obj"
    natural_portrait_obj = (
        job_directory / "portrait-natural-reference.tmp.obj"
        if build_aligned_portrait_reference and palette and portrait_materials
        else None
    )
    report_status("Converting textures into printable colors.", 96)
    if signature.startswith(b"PK\x03\x04"):
        archive = job_directory / "artifact-raw.zip"
        raw_download.replace(archive)
        obj_path = _extract_obj_package(archive, job_directory / "package")
        _bake_obj_texture_to_vertex_colors(
            obj_path,
            job_directory / "package",
            destination,
            palette,
            palette_roles,
            portrait_materials,
            natural_portrait_obj,
        )
    else:
        raw_obj = job_directory / "artifact-raw.obj"
        raw_download.replace(raw_obj)
        _validate_obj_vertex_colors(raw_obj)
        _quantize_vertex_color_obj(
            raw_obj, destination, palette, palette_roles, portrait_materials
        )
    _normalize_obj_for_orca(destination)
    effective_front_reference = front_material_reference
    geometry_aligned_reference = False
    geometry_aligned_view_directories: dict[str, Path] = {}
    if natural_portrait_obj is not None:
        aligned_report_path = job_directory / "portrait-aligned-reference.json"
        try:
            _normalize_obj_for_orca(natural_portrait_obj)
            aligned_directory = job_directory / "portrait-aligned-reference"
            natural_turntable = aligned_directory / "natural-turntable"
            # Rendering every triangle in four directions is intentionally
            # expensive for the quality profile.  Persist the stage before the
            # render starts so the desktop UI does not appear frozen on the
            # earlier texture-conversion message for several minutes.
            report_status("Rendering exact portrait material reference views.", 97)
            render_report = render_model_views(
                natural_portrait_obj,
                natural_turntable,
                ModelViewSettings(
                    width=768,
                    height=768,
                    margin_ratio=0.06,
                    # Material ownership must see every generated triangle.
                    # QA views may be sampled, but a sampled semantic pass left
                    # visible skin/garment pinholes on high-detail portraits.
                    max_render_faces=MAX_MODEL_FACES,
                ),
                force=True,
                include_isometric=False,
                include_masks=True,
                progress_callback=lambda _view, completed, total: report_status(
                    f"Rendering exact portrait material reference views ({completed}/{total}).",
                    97,
                ),
            )
            rear_plate_report = _review_portrait_rear_plate_masks(
                natural_turntable / "model-masks"
            )
            _write_mesh_repair_report(
                aligned_directory / "rear-plate-gate.json", rear_plate_report
            )
            natural_front = aligned_directory / "natural-front.png"
            shutil.copyfile(natural_turntable / "model-views" / "front.png", natural_front)
            semantic_error = ""
            try:
                report_status("Classifying skin and garment ownership in four views.", 97)
                geometry_aligned_view_directories, semantic_report = (
                    _prepare_portrait_geometry_material_views(
                        natural_turntable,
                        aligned_directory / "semantic-materials",
                        palette_roles or {},
                    )
                )
                effective_front_reference = (
                    geometry_aligned_view_directories["front"] / "aligned_reference.png"
                )
                accepted = True
                report = {
                    "status": "used",
                    "reason": "image2_semantic_material_gate_passed",
                    "render": render_report,
                    "semantic_materials": semantic_report,
                    "reference": str(effective_front_reference.relative_to(job_directory)),
                    "views": {
                        view: {
                            "reference": str(
                                (directory / "aligned_reference.png").relative_to(job_directory)
                            ),
                            "mask": str(
                                (directory / "mask_subject.png").relative_to(job_directory)
                            ),
                        }
                        for view, directory in geometry_aligned_view_directories.items()
                    },
                }
            except (OpenAIPreprocessorError, PortraitProjectionError, MultiviewReferenceError, OSError) as exc:
                # A paid geometry result remains recoverable if Image2 is
                # temporarily unavailable. Keep the deterministic path as a
                # fallback; the final visual gate still blocks mixed materials.
                semantic_error = str(exc)
                processed_views: dict[str, Any] = {}
                geometry_aligned_view_directories = {}
                for view in ("front", "right", "back", "left"):
                    view_directory = aligned_directory / "processed" / view
                    processed_views[view] = process_printable_image(
                        natural_turntable / "model-views" / f"{view}.png",
                        view_directory,
                        palette,
                        None,
                        palette_roles=palette_roles,
                    )
                    geometry_aligned_view_directories[view] = view_directory
                processed = processed_views["front"]
                accepted = bool(
                    processed.metrics.get("palette_quality_ok")
                    and processed.metrics.get("material_fragmentation_ok", True)
                )
                report = {
                    "status": "fallback" if accepted else "rejected",
                    "reason": (
                        "deterministic_material_fallback"
                        if accepted else "printable_reference_quality_gate_failed"
                    ),
                    "semantic_material_error": semantic_error,
                    "render": render_report,
                    "palette_quality_ok": bool(processed.metrics.get("palette_quality_ok")),
                    "material_fragmentation_ok": bool(
                        processed.metrics.get("material_fragmentation_ok", True)
                    ),
                    "quality_warnings": list(processed.metrics.get("quality_warnings", [])),
                    "reference": str(processed.clean_preview.relative_to(job_directory)),
                    "views": {
                        view: {
                            "reference": str(result.clean_preview.relative_to(job_directory)),
                            "portrait_skin_cleanup": result.metrics.get("portrait_skin_cleanup", {}),
                        }
                        for view, result in processed_views.items()
                    },
                }
                if accepted:
                    effective_front_reference = processed.clean_preview
            _write_mesh_repair_report(aligned_report_path, report)
            if accepted:
                geometry_aligned_reference = True
            else:
                geometry_aligned_view_directories = {}
        except (PortraitGeometryGateError, ModelViewError, PrintableImageError, TripoError, OSError) as exc:
            _write_mesh_repair_report(
                aligned_report_path,
                {
                    "status": "fallback",
                    "reason": str(exc),
                    "reference": (
                        str(front_material_reference.relative_to(job_directory))
                        if front_material_reference is not None
                        and front_material_reference.is_relative_to(job_directory)
                        else ""
                    ),
                },
            )
        finally:
            try:
                natural_portrait_obj.unlink(missing_ok=True)
            except OSError:
                pass
    report_status("Separating portrait skin and garment materials.", 97)
    if palette and portrait_materials and effective_front_reference is not None:
        try:
            project_front_portrait_materials(
                destination,
                effective_front_reference,
                job_directory / "front-material-projection.json",
                palette_roles or {},
            )
        except PortraitProjectionError as exc:
            raise TripoError(str(exc)) from None
    if palette and portrait_materials:
        _stabilize_portrait_obj_materials(
            destination,
            job_directory / "portrait-material-cleanup.json",
            palette,
            palette_roles,
            True,
        )
    repair_report = _remove_small_detached_obj_components(destination, job_directory / "mesh-repair.json")
    _repair_small_obj_topology_defects(destination, job_directory / "mesh-repair.json", repair_report)
    captured_face_details: dict[int, tuple[int, int, int]] = {}
    face_detail_capture_report: dict[str, Any] = {"status": "not_applicable"}
    if palette and portrait_materials and effective_front_reference is None:
        captured_face_details, face_detail_capture_report = _capture_portrait_front_face_details(
            destination, palette_roles
        )
    report_status("Cleaning printable color regions.", 98)
    if palette:
        report_status("Merging tiny printable color islands.", 98)
        _consolidate_tiny_obj_color_components(destination, job_directory / "vertex-color-cleanup.json")
        report_status("Smoothing printable material boundaries.", 98)
        _regularize_obj_color_boundaries(destination, job_directory / "color-boundary-cleanup.json")
        if portrait_materials:
            report_status("Keeping skin, sleeves and garments in their own materials.", 98)
            _stabilize_portrait_obj_garment_regions(
                destination,
                job_directory / "portrait-garment-cleanup.json",
                palette,
                palette_roles,
                True,
            )
        # Restore only dark vertices in the strict front-centre garment region.
        # This recovers a real scarf or shirt that generic cleanup mapped to hair
        # colour without allowing the reference to repaint skin, white sleeves,
        # the head, the back, or the base.
        if portrait_materials and effective_front_reference is not None:
            report_status("Restoring the approved front garment material.", 98)
            try:
                project_front_portrait_materials(
                    destination,
                    effective_front_reference,
                    job_directory / "front-accent-projection.json",
                    palette_roles or {},
                    repair_skin=False,
                    restore_accent=True,
                )
            except PortraitProjectionError as exc:
                raise TripoError(str(exc)) from None
            _consolidate_tiny_obj_color_components(
                destination,
                job_directory / "front-material-color-cleanup.json",
            )
            _regularize_obj_color_boundaries(
                destination,
                job_directory / "front-material-boundary-cleanup.json",
            )
            # A front projection can reconnect real blouse panels, but it may
            # also recreate dark lighting folds. Re-run the semantic ownership
            # pass before touching the face so the final garment is continuous.
            report_status("Removing remaining portrait material cross-colour.", 98)
            _stabilize_portrait_obj_garment_regions(
                destination,
                job_directory / "portrait-final-cleanup.json",
                palette,
                palette_roles,
                True,
            )
            # The semantic pass above intentionally removes suspicious accent
            # islands, but on crossed-arm portraits it can also reopen a thin
            # white seam through a real continuous blouse. Re-apply only the
            # high-confidence front-centre accent label from the approved
            # reference, then clean its boundary before touching the face.
            try:
                report_status("Finalizing the approved front garment boundary.", 98)
                project_front_portrait_materials(
                    destination,
                    effective_front_reference,
                    job_directory / "front-final-accent-projection.json",
                    palette_roles or {},
                    repair_skin=False,
                    restore_accent=True,
                )
            except PortraitProjectionError as exc:
                raise TripoError(str(exc)) from None
            _consolidate_tiny_obj_color_components(
                destination,
                job_directory / "front-final-accent-color-cleanup.json",
            )
            _regularize_obj_color_boundaries(
                destination,
                job_directory / "front-final-accent-boundary-cleanup.json",
            )
            # Normalize only the actually visible central face from the exact,
            # same-source material reference. This removes false eye/skin
            # islands, restores high-confidence dark linework, and permits only
            # geometry-aligned lower-face tooth evidence (never white eye patches).
            try:
                report_status("Restoring source-faithful facial material details.", 98)
                normalization_options: dict[str, Any] = {
                    "repair_skin": False,
                    "normalize_face_details": True,
                }
                if geometry_aligned_reference:
                    normalization_options["reference_is_geometry_aligned"] = True
                project_front_portrait_materials(
                    destination,
                    effective_front_reference,
                    job_directory / "front-face-normalization.json",
                    palette_roles or {},
                    **normalization_options,
                )
            except PortraitProjectionError as exc:
                raise TripoError(str(exc)) from None
            # The face projection deliberately adds a small amount of dark
            # printable linework. Consolidate it once more so isolated pixels
            # cannot survive as freckles, eyeliner fragments, or cheek seams;
            # coherent brows and pupils remain protected by component area.
            _consolidate_tiny_obj_color_components(
                destination,
                job_directory / "front-face-color-cleanup.json",
            )
            _regularize_obj_color_boundaries(
                destination,
                job_directory / "front-face-boundary-cleanup.json",
            )
        elif portrait_materials:
            # Older and single-view jobs have no exact material reference. Keep
            # their provider-aligned face labels on the original visible mesh
            # vertices rather than projecting an unrelated image through the
            # cheeks or rear of the head.
            _restore_portrait_front_face_details(
                destination,
                job_directory / "front-face-detail-restoration.json",
                captured_face_details,
                face_detail_capture_report,
            )
        if portrait_materials and len(geometry_aligned_view_directories) >= 2:
            try:
                report_status("Applying four-view skin and garment ownership.", 98)
                project_geometry_aligned_portrait_materials(
                    destination,
                    geometry_aligned_view_directories,
                    job_directory / "geometry-material-projection.json",
                    palette_roles or {},
                    margin_ratio=0.06,
                )
            except PortraitProjectionError as exc:
                raise TripoError(str(exc)) from None
            # The semantic projection is the final source of material truth.
            # Clean after—not before—it, otherwise a late projection can
            # recreate the exact freckles and wrist/base leakage we removed.
            _consolidate_tiny_obj_color_components(
                destination,
                job_directory / "geometry-material-color-cleanup.json",
            )
            _regularize_obj_color_boundaries(
                destination,
                job_directory / "geometry-material-boundary-cleanup.json",
            )
            # Do not run the legacy connected-component garment heuristic after
            # exact-mesh four-view projection. On crossed-arm portraits it
            # classified both correctly projected hands as jacket noise and
            # erased them. Accent and base ownership are already guarded by the
            # geometry projection itself.
            # The four-view projection already resolved visibility against the
            # actual mesh and is therefore the authoritative source for pupils,
            # brows, teeth and garment ownership.  A former planar face pass
            # ran after this cleanup and erased the correctly projected eyes on
            # a real head/shoulder beta model because its fixed vertical bands
            # were calibrated for a different bust proportion.  Finish with
            # conservative component and boundary cleanup instead: it removes
            # isolated freckles while retaining coherent, mesh-visible facial
            # features from the four-view result.
            _consolidate_tiny_obj_color_components(
                destination,
                job_directory / "geometry-face-color-cleanup.json",
            )
            _regularize_obj_color_boundaries(
                destination,
                job_directory / "geometry-face-boundary-cleanup.json",
            )
        _validate_obj_palette(destination, palette)
    else:
        _validate_obj_vertex_colors(destination)
    _write_obj_vertex_color_metrics(destination, job_directory / "vertex-color-metrics.json")
    _validate_artifact(destination, "obj", allow_repairable_obj=True)
    report_status("Checking the high-detail mesh for printing.", 99)
    quality = analyze_printable_obj(
        destination,
        ModelQualityThresholds(max_faces=MAX_MODEL_FACES),
        allow_repairable_topology=True,
        target_palette=palette,
    )
    try:
        write_model_quality_report(quality, job_directory / MODEL_QUALITY_FILENAME)
    except ModelQualityError:
        diagnostic_event("model.quality_report.unavailable", level="WARNING")
    return destination


def _persist_attempts(job: Job) -> None:
    temporary = job.directory / "attempts.json.part"
    destination = job.directory / "attempts.json"
    try:
        temporary.write_text(json.dumps({"attempts": job.attempts}, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, destination)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _promote_attempt_artifact(candidate: Path, artifact: Path) -> None:
    """Publish an accepted attempt together with its local quality evidence.

    Conversion and structural analysis run inside ``attempt-XX`` (or a recovery
    directory), while the public job contract reads reports next to the final
    artifact.  Moving only the OBJ made the completed UI say "not checked" even
    though the gate had just finished.  Copy the small reports atomically before
    the job becomes ready so restart recovery and the live result card agree.
    """

    try:
        if candidate.resolve() != artifact.resolve():
            shutil.copyfile(candidate, artifact)
        for filename in (MODEL_QUALITY_FILENAME, "vertex-color-metrics.json", "analysis-model.obj", "provider-model.glb"):
            source = candidate.parent / filename
            destination = artifact.parent / filename
            if not source.is_file() or source.resolve() == destination.resolve():
                continue
            temporary = destination.with_name(destination.name + ".part")
            try:
                shutil.copyfile(source, temporary)
                os.replace(temporary, destination)
            finally:
                temporary.unlink(missing_ok=True)
    except OSError:
        raise TripoError("The accepted model and its quality report could not be published.") from None


def _record_attempt(job: Job, attempt_number: int, **updates: Any) -> None:
    with _JOBS_LOCK:
        while len(job.attempts) < attempt_number:
            job.attempts.append({"attempt": len(job.attempts) + 1})
        job.attempts[attempt_number - 1].update(updates)
        _persist_attempts(job)
        _persist_job(job)


def _refresh_stale_face_limit_report(path: Path, palette: tuple[str, ...]) -> None:
    """Recheck an already-processed high-detail OBJ rejected by the old 1M gate."""

    report_path = path.parent / MODEL_QUALITY_FILENAME
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return
    if not isinstance(report, Mapping):
        return
    errors = report.get("errors", [])
    thresholds = report.get("thresholds", {})
    previous_limit = thresholds.get("max_faces", 0) if isinstance(thresholds, Mapping) else 0
    if not isinstance(errors, list) or "too_many_faces" not in errors:
        return
    try:
        previous_limit = int(previous_limit)
    except (TypeError, ValueError):
        previous_limit = 0
    if previous_limit >= MAX_MODEL_FACES and report.get("gate_version") == MODEL_QUALITY_GATE_VERSION:
        return

    quality = analyze_printable_obj(
        path,
        ModelQualityThresholds(max_faces=MAX_MODEL_FACES),
        allow_repairable_topology=True,
        target_palette=palette,
    )
    try:
        write_model_quality_report(quality, report_path)
    except ModelQualityError as exc:
        raise TripoError(str(exc)) from None


def _analysis_artifact(artifact: Path) -> Path:
    if artifact.suffix.lower() != ".glb":
        return artifact
    analysis = artifact.parent / "analysis-model.obj"
    if not analysis.is_file() or analysis.stat().st_mtime_ns < artifact.stat().st_mtime_ns:
        try:
            write_analysis_obj(artifact, analysis)
        except (GlbError, OSError, ValueError, KeyError, TypeError) as exc:
            raise TripoError(f"The GLB could not be read for model checks: {exc}") from None
    return analysis


def _download_generation_artifact(job: Job, generation_id: str, attempt_number: int = 1, resume: bool = False) -> Path:
    existing = job.attempts[attempt_number - 1] if len(job.attempts) >= attempt_number else {}
    # GLB consumes the original generation result. OBJ explicitly includes one
    # basic provider conversion; frozen conversions keep their existing task ID.
    if job.provider == "tripo" and (existing.get("conversion_task_id") or job.output_format == "obj"):
        return _download_conversion(job, generation_id, "obj", attempt_number, resume)
    directory = job.directory / f"attempt-{attempt_number:02d}"
    directory.mkdir(exist_ok=True)
    candidate = directory / "model.glb"
    if resume and candidate.is_file():
        try:
            _validate_artifact(candidate, "glb")
            _analysis_artifact(candidate)
            return candidate
        except TripoError:
            diagnostic_event("model.glb_cache.invalid", level="WARNING")
    with _JOBS_LOCK:
        job.phase = "downloading_artifact"
        job.message = "Downloading the generated model."
        job.progress = 75
        _persist_job(job)
    gateway = _model_gateway(job.provider)
    result = gateway.wait_for_task(generation_id, stop_event=job.stop_event)
    _stop_boundary(job)
    raw = directory / "artifact-raw.download"
    if job.provider == "hunyuan":
        gateway.download_artifact(result, raw, MAX_ARTIFACT_BYTES, output_format=job.output_format)
    else:
        gateway.download_artifact(result, raw, MAX_ARTIFACT_BYTES)
    _stop_boundary(job)
    with raw.open("rb") as stream:
        is_glb = stream.read(4) == b"glTF"
    if not is_glb:
        # Retain compatibility with providers/frozen jobs that return OBJ/ZIP.
        if resume and (directory / "package").exists():
            # Keep an interrupted extraction intact and retry in a fresh directory.
            recovery_number = 1
            while (directory / f"recovery-{recovery_number:02d}").exists():
                recovery_number += 1
            directory = directory / f"recovery-{recovery_number:02d}"
            directory.mkdir(parents=False, exist_ok=False)
            raw = raw.replace(directory / "artifact-raw.download")
        return _prepare_obj_artifact(raw, directory, job.palette, job.palette_roles)
    original = directory / "provider-model.glb"
    raw.replace(original)
    analysis = directory / "analysis-model.obj"
    try:
        prepare_generated_glb(original, candidate, analysis, DEFAULT_MODEL_SIZE_MM)
    except (GlbError, OSError, ValueError, KeyError, TypeError) as exc:
        raise TripoError(f"The generated GLB could not be prepared: {exc}") from None
    _stop_boundary(job)
    with _JOBS_LOCK:
        job.phase = "checking_model"
        job.message = "Checking model geometry and colors."
        job.progress = 99
        _persist_job(job)
    quality = analyze_printable_obj(analysis, ModelQualityThresholds(max_faces=MAX_MODEL_FACES),
                                    allow_repairable_topology=True)
    try:
        write_model_quality_report(quality, directory / MODEL_QUALITY_FILENAME)
    except ModelQualityError:
        diagnostic_event("model.quality_report.unavailable", level="WARNING")
    _write_obj_vertex_color_metrics(analysis, directory / "vertex-color-metrics.json")
    return candidate


def _download_conversion(
    job: Job, generation_id: str, format_name: str, attempt_number: int = 1, resume: bool = False
) -> Path:
    with _JOBS_LOCK:
        job.state = "running"
        job.phase = "converting"
        job.message = f"Converting generated geometry to {format_name.upper()}."
        job.progress = 75
        _persist_job(job)
    existing = job.attempts[attempt_number - 1] if len(job.attempts) >= attempt_number else {}
    conversion_id = existing.get("conversion_task_id", "")
    if not conversion_id:
        if job.output_format != "obj" or existing.get("conversion_submission_started"):
            raise TripoError("Conversion submission has no confirmed task ID; it will not be submitted again.")
        _stop_boundary(job)
        # Persist before the paid POST. An interrupted response must never create
        # another conversion during recovery, even if the server accepted it.
        _record_attempt(job, attempt_number, conversion_submission_started=True)
        _persist_job(job, required=True)
    conversion_ref = _MODEL_PROVIDER_GATEWAY.start_or_reuse_conversion(
        generation_id,
        format_name,
        existing_task_id=conversion_id if isinstance(conversion_id, str) else "",
        allow_create=job.output_format == "obj" and not conversion_id,
    )
    conversion_id = conversion_ref.task_id
    if not conversion_ref.reused:
        _record_attempt(job, attempt_number, conversion_task_id=conversion_id)
    attempt_directory = job.directory / f"attempt-{attempt_number:02d}"
    attempt_directory.mkdir(parents=False, exist_ok=True)
    if resume and format_name == "obj":
        candidates = sorted(attempt_directory.rglob("model-vertex-color.obj"), reverse=True)
        for candidate in candidates:
            try:
                candidate.resolve().relative_to(attempt_directory.resolve())
                _validate_artifact(candidate, "obj", allow_repairable_obj=True)
                _refresh_stale_face_limit_report(candidate, job.palette)
            except (OSError, TripoError, ValueError):
                continue
            return candidate
    _stop_boundary(job)
    result = _MODEL_PROVIDER_GATEWAY.wait_for_task(
        conversion_id,
        stop_event=job.stop_event,
        progress=_progress_callback(job, 75, 95),
    )
    _stop_boundary(job)
    with _JOBS_LOCK:
        job.phase = "downloading_artifact"
        job.message = "Preparing the generated artifact."
        job.progress = 95
        _persist_job(job)
    work_directory = attempt_directory
    if resume:
        recovery_number = 1
        while (attempt_directory / f"recovery-{recovery_number:02d}").exists():
            recovery_number += 1
        work_directory = attempt_directory / f"recovery-{recovery_number:02d}"
        work_directory.mkdir(parents=False, exist_ok=False)
    destination = work_directory / "artifact-raw.download"
    _MODEL_PROVIDER_GATEWAY.download_artifact(result, destination, MAX_ARTIFACT_BYTES)
    _stop_boundary(job)
    if format_name == "obj":
        portrait_cleanup = job.image_metrics.get("portrait_skin_cleanup", {})
        portrait_materials = (
            job.style == "realistic"
            and isinstance(portrait_cleanup, dict)
            and portrait_cleanup.get("activated") == 1
        )
        effective_palette_roles = dict(job.palette_roles)
        if portrait_materials:
            garment_color = str(portrait_cleanup.get("garment_color", "")).strip().upper()
            skin_color = str(portrait_cleanup.get("skin_color", "")).strip().upper()
            if garment_color in job.palette and skin_color in job.palette and garment_color != skin_color:
                effective_palette_roles["primary"] = garment_color
                effective_palette_roles["light"] = skin_color
        material_views = _multiview_paths_from_metrics(job, "material_views")
        build_aligned_reference = bool(
            portrait_materials
            and job.generation_profile == "quality"
            and job.palette
        )
        def artifact_status(message: str, progress: int) -> None:
            _stop_boundary(job)
            with _JOBS_LOCK:
                job.phase = "checking_model"
                job.message = message
                job.progress = progress
                _persist_job(job)
            _stop_boundary(job)

        prepare_options: dict[str, Any] = {}
        if build_aligned_reference:
            prepare_options["build_aligned_portrait_reference"] = True
        if material_views is not None:
            return _prepare_obj_artifact(
                destination,
                work_directory,
                job.palette,
                effective_palette_roles,
                portrait_materials,
                material_views["front"],
                artifact_status,
                **prepare_options,
            )
        return _prepare_obj_artifact(
            destination,
            work_directory,
            job.palette,
            effective_palette_roles,
            portrait_materials,
            None,
            artifact_status,
            **prepare_options,
        )
    _validate_artifact(destination, format_name)
    return destination


def _validate_obj_vertex_colors(path: Path) -> None:
    vertices: list[bool] = []
    referenced_vertices: set[int] = set()
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                keyword = fields[0].lower()
                if keyword in {"mtllib", "usemtl", "vt", "vn", "map_kd"}:
                    raise TripoError("The generated OBJ depends on external materials or textures.")
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        vertices.append(False)
                        continue
                    try:
                        values = [float(value) for value in fields[1:]]
                    except ValueError:
                        vertices.append(False)
                        continue
                    if not all(math.isfinite(value) for value in values):
                        vertices.append(False)
                        continue
                    colors = values[3:]
                    vertices.append(all(0.0 <= value <= 1.0 for value in colors))
                elif keyword == "f":
                    if len(fields) < 4:
                        raise TripoError("The generated OBJ has an invalid face.")
                    for field in fields[1:]:
                        if "/" in field:
                            raise TripoError("The generated OBJ contains unsupported texture or normal references.")
                        try:
                            index = int(field)
                        except ValueError:
                            raise TripoError("The generated OBJ has an invalid vertex index.") from None
                        if index == 0:
                            raise TripoError("The generated OBJ has an invalid vertex index.")
                        resolved = index - 1 if index > 0 else len(vertices) + index
                        if resolved < 0 or resolved >= len(vertices):
                            raise TripoError("The generated OBJ references a missing vertex.")
                        referenced_vertices.add(resolved)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if not referenced_vertices or any(not vertices[index] for index in referenced_vertices):
        raise TripoError("The generated OBJ does not provide valid vertex colors.")


def _validate_obj_palette(path: Path, palette: tuple[str, ...]) -> None:
    allowed = set(_palette_data(palette)[0])
    found = False
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].lower() != "v":
                    continue
                if len(fields) not in {7, 8}:
                    raise TripoError("The generated OBJ does not provide valid vertex colors.")
                try:
                    color = tuple(round(float(value) * 255) for value in fields[4:7])
                except ValueError:
                    raise TripoError("The generated OBJ has an invalid vertex color.") from None
                if color not in allowed:
                    raise TripoError("The generated OBJ contains colors outside the printable filament palette.")
                found = True
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if not found:
        raise TripoError("The generated OBJ does not provide valid vertex colors.")


def _obj_vertex_color_metrics(path: Path) -> dict[str, Any]:
    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                if fields[0].lower() == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    positions.append(tuple(float(value) for value in fields[1:4]))
                    colors.append(tuple(round(float(value) * 255) for value in fields[4:7]))
                elif fields[0].lower() == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    faces.append(tuple(_resolve_obj_index(value, len(colors), "vertex") for value in fields[1:]))
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ color metrics could not be calculated.") from None
    distribution = Counter(len({colors[index] for index in face}) for face in faces)
    vertex_usage = Counter(colors)
    face_areas = [_obj_triangle_area(positions, face) for face in faces]
    surface_area = sum(face_areas)
    mixed_surface_area = sum(
        area for face, area in zip(faces, face_areas) if len({colors[index] for index in face}) > 1
    )
    total = max(1, len(faces))
    return {
        "vertex_count": len(colors),
        "face_count": len(faces),
        "vertex_color_count": len(vertex_usage),
        "uniform_faces": distribution[1],
        "two_color_faces": distribution[2],
        "three_color_faces": distribution[3],
        "two_color_face_ratio": round(distribution[2] / total, 6),
        "three_color_face_ratio": round(distribution[3] / total, 6),
        "mixed_face_count": distribution[2] + distribution[3],
        "mixed_face_ratio": round((distribution[2] + distribution[3]) / total, 6),
        "surface_area_mm2": round(surface_area, 6),
        "mixed_face_surface_area_mm2": round(mixed_surface_area, 6),
        "mixed_face_surface_area_ratio": round(mixed_surface_area / surface_area, 6) if surface_area > 0.0 else 0.0,
        "vertex_color_usage": {
            "#{:02X}{:02X}{:02X}".format(*color): count for color, count in sorted(vertex_usage.items())
        },
    }


def _write_obj_vertex_color_metrics(path: Path, report_path: Path) -> dict[str, Any]:
    metrics = _obj_vertex_color_metrics(path)
    temporary = report_path.with_suffix(report_path.suffix + ".part")
    try:
        temporary.write_text(json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, report_path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise TripoError("The generated OBJ color metrics could not be saved.") from None
    return metrics


def _obj_triangle_area(
    positions: list[tuple[float, float, float]], face: tuple[int, int, int]
) -> float:
    left, right, third = (positions[index] for index in face)
    ab = tuple(right[axis] - left[axis] for axis in range(3))
    ac = tuple(third[axis] - left[axis] for axis in range(3))
    cross = (
        ab[1] * ac[2] - ab[2] * ac[1],
        ab[2] * ac[0] - ab[0] * ac[2],
        ab[0] * ac[1] - ab[1] * ac[0],
    )
    return 0.5 * math.sqrt(sum(value * value for value in cross))


def _color_boundary_metrics(
    colors: list[tuple[int, int, int]],
    faces: list[tuple[int, int, int]],
    face_areas: list[float],
) -> dict[str, Any]:
    mixed = [
        (face, area) for face, area in zip(faces, face_areas)
        if len({colors[index] for index in face}) > 1
    ]
    surface_area = sum(face_areas)
    mixed_area = sum(area for _face, area in mixed)
    return {
        "mixed_face_count": len(mixed),
        "mixed_face_ratio": round(len(mixed) / len(faces), 6) if faces else 0.0,
        "mixed_face_surface_area_mm2": round(mixed_area, 6),
        "mixed_face_surface_area_ratio": round(mixed_area / surface_area, 6) if surface_area > 0.0 else 0.0,
    }


def _regularize_obj_color_boundaries(path: Path, report_path: Path) -> dict[str, Any]:
    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    position = tuple(float(value) for value in fields[1:4])
                    color = tuple(round(float(value) * 255) for value in fields[4:7])
                    if not all(math.isfinite(value) for value in position) or not all(
                        0 <= value <= 255 for value in color
                    ):
                        raise TripoError("The generated OBJ has an invalid colored vertex.")
                    positions.append(position)
                    colors.append(color)
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    faces.append(tuple(_resolve_obj_index(value, len(positions), "vertex") for value in fields[1:]))
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be read for color-boundary cleanup.") from None
    if not positions or not faces:
        raise TripoError("The generated OBJ does not contain usable colored geometry.")

    face_areas = [_obj_triangle_area(positions, face) for face in faces]
    surface_area = sum(face_areas)
    if not math.isfinite(surface_area) or surface_area <= 0.0:
        raise TripoError("The generated OBJ has invalid surface area for color-boundary cleanup.")

    incident_faces: list[list[int]] = [[] for _ in positions]
    vertex_surface_area = [0.0] * len(positions)
    edge_lengths: dict[tuple[int, int], float] = {}
    for face_index, (face, area) in enumerate(zip(faces, face_areas)):
        for vertex in face:
            incident_faces[vertex].append(face_index)
            vertex_surface_area[vertex] += area / 3.0
        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            edge = (left, right) if left < right else (right, left)
            if edge in edge_lengths:
                continue
            length = math.sqrt(sum((positions[left][axis] - positions[right][axis]) ** 2 for axis in range(3)))
            if length > 0.0 and math.isfinite(length):
                edge_lengths[edge] = length
    neighbors: list[list[tuple[int, float]]] = [[] for _ in positions]
    for (left, right), length in edge_lengths.items():
        neighbors[left].append((right, length))
        neighbors[right].append((left, length))

    original_color_area: Counter[tuple[int, int, int]] = Counter()
    for color, area in zip(colors, vertex_surface_area):
        original_color_area[color] += area
    current_color_area = original_color_area.copy()
    before = _color_boundary_metrics(colors, faces, face_areas)
    global_budget = surface_area * MAX_COLOR_BOUNDARY_SURFACE_AREA_RATIO
    changed_area_by_color: Counter[tuple[int, int, int]] = Counter()
    changed_surface_area = 0.0
    recolored_vertices = 0
    protected_meaningful_candidates = 0
    budget_limited_candidates = 0
    pass_reports: list[dict[str, Any]] = []

    def is_mixed(face_index: int, override_vertex: int = -1, override_color: tuple[int, int, int] | None = None) -> bool:
        face_colors = {
            override_color if vertex == override_vertex else colors[vertex]
            for vertex in faces[face_index]
        }
        return len(face_colors) > 1

    def evaluate(vertex: int) -> tuple[float, float, tuple[int, int, int]] | None:
        source = colors[vertex]
        source_neighbors = sum(1 for neighbor, _length in neighbors[vertex] if colors[neighbor] == source)
        if source_neighbors > MAX_COLOR_BOUNDARY_SOURCE_NEIGHBORS:
            return None
        candidates = {colors[neighbor] for neighbor, _length in neighbors[vertex] if colors[neighbor] != source}
        if not candidates:
            return None
        before_area = sum(face_areas[index] for index in incident_faces[vertex] if is_mixed(index))
        source_support = sum(length for neighbor, length in neighbors[vertex] if colors[neighbor] == source)
        best: tuple[float, float, tuple[int, int, int]] | None = None
        for target in candidates:
            target_support = sum(length for neighbor, length in neighbors[vertex] if colors[neighbor] == target)
            if target_support <= source_support * MIN_COLOR_BOUNDARY_SUPPORT_RATIO + 1e-12:
                continue
            before_color_counts = [len({colors[index] for index in faces[face_index]}) for face_index in incident_faces[vertex]]
            after_color_counts = [
                len({target if index == vertex else colors[index] for index in faces[face_index]})
                for face_index in incident_faces[vertex]
            ]
            if any(after > before for before, after in zip(before_color_counts, after_color_counts)):
                continue
            after_area = sum(
                face_areas[index] for index, color_count in zip(incident_faces[vertex], after_color_counts)
                if color_count > 1
            )
            improvement = before_area - after_area
            if improvement <= 1e-12:
                continue
            candidate = (improvement, target_support, target)
            if best is None or (-candidate[0], -candidate[1], candidate[2]) < (-best[0], -best[1], best[2]):
                best = candidate
        return best

    for pass_index in range(MAX_COLOR_BOUNDARY_PASSES):
        candidates = []
        for vertex in range(len(positions)):
            evaluated = evaluate(vertex)
            if evaluated is not None:
                improvement, support, target = evaluated
                candidates.append((-improvement, -support, vertex, target))
        candidates.sort()
        changed_this_pass = 0
        improved_area = 0.0
        for _negative_improvement, _negative_support, vertex, _target in candidates:
            evaluated = evaluate(vertex)
            if evaluated is None:
                continue
            improvement, _support, target = evaluated
            source = colors[vertex]
            contribution = vertex_surface_area[vertex]
            meaningful_floor = surface_area * MEANINGFUL_COLOR_SURFACE_AREA_RATIO
            if (
                original_color_area[source] >= meaningful_floor
                and current_color_area[source] - contribution < meaningful_floor - 1e-12
            ):
                protected_meaningful_candidates += 1
                continue
            source_budget = min(
                original_color_area[source],
                original_color_area[source] * MAX_COLOR_BOUNDARY_SOURCE_AREA_RATIO
                if original_color_area[source] >= meaningful_floor else original_color_area[source],
            )
            if (
                changed_surface_area + contribution > global_budget + 1e-12
                or changed_area_by_color[source] + contribution > source_budget + 1e-12
            ):
                budget_limited_candidates += 1
                continue
            colors[vertex] = target
            current_color_area[source] -= contribution
            current_color_area[target] += contribution
            changed_area_by_color[source] += contribution
            changed_surface_area += contribution
            recolored_vertices += 1
            changed_this_pass += 1
            improved_area += improvement
        pass_reports.append({
            "pass": pass_index + 1,
            "candidate_vertices": len(candidates),
            "recolored_vertices": changed_this_pass,
            "mixed_surface_area_improvement_mm2": round(improved_area, 6),
        })
        if not changed_this_pass:
            break

    if recolored_vertices:
        temporary = path.with_name(path.name + ".boundaries")
        vertex_index = 0
        try:
            with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
                "w", encoding="ascii", newline="\n"
            ) as output:
                for line in source:
                    fields = line.strip().split()
                    if fields and fields[0].lower() == "v":
                        red, green, blue = colors[vertex_index]
                        fields[4:7] = [f"{channel / 255.0:.6f}" for channel in (red, green, blue)]
                        output.write(" ".join(fields) + "\n")
                        vertex_index += 1
                    else:
                        output.write(line if line.endswith("\n") else line + "\n")
            os.replace(temporary, path)
        except (OSError, UnicodeDecodeError):
            raise TripoError("Printable color boundaries could not be regularized safely.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    after = _color_boundary_metrics(colors, faces, face_areas)
    report = {
        "status": "regularized" if recolored_vertices else "not_needed",
        "passes": pass_reports,
        "recolored_vertices": recolored_vertices,
        "changed_surface_area_mm2": round(changed_surface_area, 6),
        "changed_surface_area_ratio": round(changed_surface_area / surface_area, 6),
        "maximum_surface_area_ratio": MAX_COLOR_BOUNDARY_SURFACE_AREA_RATIO,
        "maximum_source_area_ratio": MAX_COLOR_BOUNDARY_SOURCE_AREA_RATIO,
        "minimum_neighbor_support_ratio": MIN_COLOR_BOUNDARY_SUPPORT_RATIO,
        "maximum_source_same_color_neighbors": MAX_COLOR_BOUNDARY_SOURCE_NEIGHBORS,
        "meaningful_color_surface_area_ratio": MEANINGFUL_COLOR_SURFACE_AREA_RATIO,
        "protected_meaningful_candidates": protected_meaningful_candidates,
        "budget_limited_candidates": budget_limited_candidates,
        "before": before,
        "after": after,
        "changed_surface_area_by_color_mm2": {
            "#{:02X}{:02X}{:02X}".format(*color): round(area, 6)
            for color, area in sorted(changed_area_by_color.items())
        },
    }
    temporary_report = report_path.with_suffix(report_path.suffix + ".part")
    try:
        temporary_report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary_report, report_path)
    except OSError:
        raise TripoError("The color-boundary cleanup report could not be saved.") from None
    finally:
        try:
            temporary_report.unlink(missing_ok=True)
        except OSError:
            pass
    return report


def _write_mesh_repair_report(path: Path, report: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".part")
    try:
        temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _consolidate_tiny_obj_color_components(path: Path, report_path: Path) -> dict[str, Any]:
    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    try:
                        position = tuple(float(value) for value in fields[1:4])
                        color = tuple(round(float(value) * 255) for value in fields[4:7])
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid colored vertex.") from None
                    if not all(math.isfinite(value) for value in position) or not all(
                        0 <= value <= 255 for value in color
                    ):
                        raise TripoError("The generated OBJ has an invalid colored vertex.")
                    positions.append(position)
                    colors.append(color)
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    faces.append(tuple(_resolve_obj_index(value, len(positions), "vertex") for value in fields[1:]))
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read for color cleanup.") from None
    if not positions or not faces:
        raise TripoError("The generated OBJ does not contain usable colored geometry.")

    def triangle_area(face: tuple[int, int, int]) -> float:
        left = positions[face[0]]
        right = positions[face[1]]
        third = positions[face[2]]
        ab = tuple(right[axis] - left[axis] for axis in range(3))
        ac = tuple(third[axis] - left[axis] for axis in range(3))
        cross = (
            ab[1] * ac[2] - ab[2] * ac[1],
            ab[2] * ac[0] - ab[0] * ac[2],
            ab[0] * ac[1] - ab[1] * ac[0],
        )
        return 0.5 * math.sqrt(sum(value * value for value in cross))

    face_areas = [triangle_area(face) for face in faces]
    surface_area = sum(face_areas)
    if not math.isfinite(surface_area) or surface_area <= 0.0:
        raise TripoError("The generated OBJ has invalid surface area for color cleanup.")
    original_usage = Counter(colors)
    total_merged_components = 0
    total_recolored_vertices = 0
    merged_area_by_color: Counter[tuple[int, int, int]] = Counter()
    pass_reports: list[dict[str, Any]] = []

    for pass_index in range(MAX_COLOR_CLEANUP_PASSES):
        parent = list(range(len(positions)))

        def find(index: int) -> int:
            while parent[index] != index:
                parent[index] = parent[parent[index]]
                index = parent[index]
            return index

        def unite(left: int, right: int) -> None:
            left_root, right_root = find(left), find(right)
            if left_root != right_root:
                parent[right_root] = left_root

        for face in faces:
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                if colors[left] == colors[right]:
                    unite(left, right)

        component_area: Counter[int] = Counter()
        component_vertices: Counter[int] = Counter()
        color_area: Counter[tuple[int, int, int]] = Counter()
        for face, area in zip(faces, face_areas):
            contribution = area / 3.0
            for vertex in face:
                root = find(vertex)
                component_area[root] += contribution
                color_area[colors[vertex]] += contribution
        for index in range(len(positions)):
            component_vertices[find(index)] += 1

        boundary: dict[int, Counter[tuple[int, int, int]]] = {}
        for face in faces:
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                left_root, right_root = find(left), find(right)
                if left_root == right_root or colors[left] == colors[right]:
                    continue
                edge_length = math.sqrt(sum(
                    (positions[left][axis] - positions[right][axis]) ** 2 for axis in range(3)
                ))
                if edge_length <= 0.0 or not math.isfinite(edge_length):
                    continue
                boundary.setdefault(left_root, Counter())[colors[right]] += edge_length
                boundary.setdefault(right_root, Counter())[colors[left]] += edge_length

        candidates: list[tuple[float, int, tuple[int, int, int]]] = []
        for root, area in component_area.items():
            neighbors = boundary.get(root)
            if not neighbors:
                continue
            if (
                area / surface_area <= MAX_TINY_COLOR_COMPONENT_AREA_RATIO
                and component_vertices[root] / len(positions) <= MAX_TINY_COLOR_COMPONENT_VERTEX_RATIO
            ):
                target = min(neighbors, key=lambda color: (-neighbors[color], color))
                candidates.append((area, root, target))
        candidates.sort(key=lambda item: (item[0], item[1], item[2]))

        merged_this_pass: dict[int, tuple[int, int, int]] = {}
        pass_area_by_color: Counter[tuple[int, int, int]] = Counter()
        for area, root, target in candidates:
            source = colors[root]
            if color_area[source] < surface_area * MEANINGFUL_COLOR_SURFACE_AREA_RATIO:
                budget = min(color_area[source], surface_area * MAX_COLOR_CLEANUP_SURFACE_AREA_RATIO)
            else:
                budget = min(
                    color_area[source] * MAX_COLOR_CLEANUP_SOURCE_AREA_RATIO,
                    surface_area * MAX_COLOR_CLEANUP_SURFACE_AREA_RATIO,
                    color_area[source] - surface_area * MEANINGFUL_COLOR_SURFACE_AREA_RATIO,
                )
            if pass_area_by_color[source] + area > budget + 1e-12:
                continue
            merged_this_pass[root] = target
            pass_area_by_color[source] += area

        recolored_this_pass = 0
        for index in range(len(colors)):
            target = merged_this_pass.get(find(index))
            if target is not None and colors[index] != target:
                colors[index] = target
                recolored_this_pass += 1
        pass_reports.append({
            "pass": pass_index + 1,
            "candidate_components": len(candidates),
            "merged_components": len(merged_this_pass),
            "recolored_vertices": recolored_this_pass,
            "merged_surface_area_mm2": round(sum(pass_area_by_color.values()), 6),
        })
        total_merged_components += len(merged_this_pass)
        total_recolored_vertices += recolored_this_pass
        merged_area_by_color.update(pass_area_by_color)
        if not recolored_this_pass:
            break

    if total_recolored_vertices:
        temporary = path.with_name(path.name + ".colors")
        vertex_index = 0
        try:
            with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
                "w", encoding="ascii", newline="\n"
            ) as output:
                for line in source:
                    fields = line.strip().split()
                    if fields and fields[0].lower() == "v":
                        red, green, blue = colors[vertex_index]
                        fields[4:7] = [f"{channel / 255.0:.6f}" for channel in (red, green, blue)]
                        output.write(" ".join(fields) + "\n")
                        vertex_index += 1
                    else:
                        output.write(line if line.endswith("\n") else line + "\n")
            os.replace(temporary, path)
        except (OSError, UnicodeDecodeError):
            raise TripoError("Tiny printable-color regions could not be consolidated safely.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    final_usage = Counter(colors)
    report = {
        "status": "consolidated" if total_recolored_vertices else "not_needed",
        "passes": pass_reports,
        "merged_components": total_merged_components,
        "recolored_vertices": total_recolored_vertices,
        "surface_area_mm2": round(surface_area, 6),
        "maximum_component_area_ratio": MAX_TINY_COLOR_COMPONENT_AREA_RATIO,
        "maximum_component_vertex_ratio": MAX_TINY_COLOR_COMPONENT_VERTEX_RATIO,
        "maximum_source_area_ratio": MAX_COLOR_CLEANUP_SOURCE_AREA_RATIO,
        "maximum_surface_area_ratio": MAX_COLOR_CLEANUP_SURFACE_AREA_RATIO,
        "original_vertex_color_usage": {
            "#{:02X}{:02X}{:02X}".format(*color): count for color, count in sorted(original_usage.items())
        },
        "final_vertex_color_usage": {
            "#{:02X}{:02X}{:02X}".format(*color): count for color, count in sorted(final_usage.items())
        },
        "merged_surface_area_by_color_mm2": {
            "#{:02X}{:02X}{:02X}".format(*color): round(area, 6)
            for color, area in sorted(merged_area_by_color.items())
        },
    }
    _write_mesh_repair_report(report_path, report)
    return report


def _remove_small_detached_obj_components(path: Path, report_path: Path) -> dict[str, Any]:
    vertex_lines: list[str] = []
    vertices: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    face_sections: list[tuple[str, str]] = []
    current_object = ""
    current_group = ""
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    try:
                        position = tuple(float(value) for value in fields[1:4])
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex position.") from None
                    if not all(math.isfinite(value) for value in position):
                        raise TripoError("The generated OBJ has an invalid vertex position.")
                    vertices.append(position)
                    vertex_lines.append(" ".join(fields))
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    face = tuple(_resolve_obj_index(value, len(vertices), "vertex") for value in fields[1:])
                    faces.append(face)
                    face_sections.append((current_object, current_group))
                elif keyword == "o":
                    current_object = " ".join(fields)
                elif keyword == "g":
                    current_group = " ".join(fields)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None

    if not vertices or not faces:
        raise TripoError("The generated OBJ does not contain usable geometry.")

    parent = list(range(len(vertices)))

    def find(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    def unite(left: int, right: int) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parent[right_root] = left_root

    for face in faces:
        unite(face[0], face[1])
        unite(face[1], face[2])

    component_faces: dict[int, list[tuple[int, int, int]]] = {}
    component_vertices: dict[int, set[int]] = {}
    for face in faces:
        root = find(face[0])
        component_faces.setdefault(root, []).append(face)
        component_vertices.setdefault(root, set()).update(face)

    def diagonal(indices: set[int]) -> float:
        minimum = [min(vertices[index][axis] for index in indices) for axis in range(3)]
        maximum = [max(vertices[index][axis] for index in indices) for axis in range(3)]
        return math.sqrt(sum((maximum[axis] - minimum[axis]) ** 2 for axis in range(3)))

    components = sorted(
        component_faces,
        key=lambda root: (len(component_faces[root]), len(component_vertices[root])),
        reverse=True,
    )
    main = components[0]
    main_face_count = len(component_faces[main])
    mesh_vertices = {index for indices in component_vertices.values() for index in indices}
    mesh_diagonal = diagonal(mesh_vertices)
    removable: set[int] = set()
    component_report: list[dict[str, Any]] = []
    for root in components:
        face_count = len(component_faces[root])
        vertex_count = len(component_vertices[root])
        component_diagonal = diagonal(component_vertices[root])
        face_ratio = face_count / len(faces)
        diagonal_ratio = component_diagonal / mesh_diagonal if mesh_diagonal > 0.0 else 0.0
        remove = (
            root != main
            and face_ratio <= MAX_NOISE_COMPONENT_FACE_RATIO
            and diagonal_ratio <= MAX_NOISE_COMPONENT_DIAGONAL_RATIO
        )
        if remove:
            removable.add(root)
        component_report.append({
            "faces": face_count,
            "vertices": vertex_count,
            "diagonal_mm": round(component_diagonal, 6),
            "face_ratio": round(face_ratio, 8),
            "diagonal_ratio": round(diagonal_ratio, 8),
            "removed": remove,
        })

    kept_face_records = [
        (face, section)
        for face, section in zip(faces, face_sections)
        if find(face[0]) not in removable
    ]
    if not kept_face_records:
        raise TripoError("Detached-component cleanup would remove all generated geometry.")
    kept_vertex_indices = sorted({index for face, _section in kept_face_records for index in face})
    vertex_map = {old: new for new, old in enumerate(kept_vertex_indices)}
    removed_vertices = len(vertices) - len(kept_vertex_indices)
    if removable or removed_vertices:
        temporary = path.with_name(path.name + ".components")
        try:
            with temporary.open("w", encoding="ascii", newline="\n") as output:
                output.write("# OrcaSlicer AI removed bounded detached mesh noise\n")
                for old_index in kept_vertex_indices:
                    output.write(vertex_lines[old_index] + "\n")
                previous_section = ("", "")
                for face, section in kept_face_records:
                    if section != previous_section:
                        if section[0]:
                            output.write(section[0] + "\n")
                        if section[1]:
                            output.write(section[1] + "\n")
                        previous_section = section
                    output.write("f {} {} {}\n".format(*(vertex_map[index] + 1 for index in face)))
            os.replace(temporary, path)
        except OSError:
            raise TripoError("Detached mesh noise could not be removed safely.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    removed_faces = sum(len(component_faces[root]) for root in removable)
    report: dict[str, Any] = {
        "status": (
            "removed"
            if removable or removed_vertices
            else "not_needed" if len(components) == 1 else "preserved"
        ),
        "original_components": len(components),
        "kept_vertices": len(kept_vertex_indices),
        "kept_faces": len(kept_face_records),
        "removed_components": len(removable),
        "removed_vertices": removed_vertices,
        "removed_faces": removed_faces,
        "largest_component_faces": main_face_count,
        "largest_component_diagonal": diagonal(component_vertices[main]),
        "maximum_noise_component_face_ratio": MAX_NOISE_COMPONENT_FACE_RATIO,
        "maximum_noise_component_diagonal_ratio": MAX_NOISE_COMPONENT_DIAGONAL_RATIO,
        "components": component_report,
    }

    _write_mesh_repair_report(report_path, report)
    return report


def _repair_small_obj_topology_defects(
    path: Path, report_path: Path, report: dict[str, Any] | None = None
) -> dict[str, Any]:
    report = dict(report or {})
    vertex_lines: list[str] = []
    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[str, ...]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    try:
                        position = tuple(float(value) for value in fields[1:4])
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex position.") from None
                    if not all(math.isfinite(value) for value in position):
                        raise TripoError("The generated OBJ has an invalid vertex position.")
                    vertex_lines.append(" ".join(fields))
                    positions.append(position)
                    colors.append(tuple(fields[4:]))
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    face = tuple(_resolve_obj_index(value, len(positions), "vertex") for value in fields[1:])
                    faces.append(face)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None

    def edge_usage(source_faces: list[tuple[int, int, int]]) -> dict[tuple[int, int], list[tuple[int, int, int]]]:
        usage: dict[tuple[int, int], list[tuple[int, int, int]]] = {}
        for face_index, face in enumerate(source_faces):
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                edge = (left, right) if left < right else (right, left)
                usage.setdefault(edge, []).append((face_index, left, right))
        return usage

    if any(len(set(face)) != 3 for face in faces):
        report.update(topology_status="deferred", topology_deferred_reason="degenerate_faces")
        _write_mesh_repair_report(report_path, report)
        return report
    original_usage = edge_usage(faces)
    original_boundary = sum(len(uses) == 1 for uses in original_usage.values())
    original_non_manifold = sum(len(uses) > 2 for uses in original_usage.values())
    original_inconsistent_winding = sum(
        len(uses) == 2 and uses[0][1:] == uses[1][1:]
        for uses in original_usage.values()
    )
    report.update(
        topology_status=(
            "not_needed"
            if not original_boundary and not original_non_manifold and not original_inconsistent_winding
            else "deferred"
        ),
        original_boundary_edges=original_boundary,
        original_non_manifold_edges=original_non_manifold,
        original_inconsistent_winding_edges=original_inconsistent_winding,
        removed_non_manifold_faces=0,
        flipped_winding_faces=0,
        removed_topology_vertices=0,
        filled_boundary_loops=0,
        added_vertices=0,
        added_faces=0,
        remaining_invalid_edges=original_boundary + original_non_manifold + original_inconsistent_winding,
    )
    if not original_boundary and not original_non_manifold and not original_inconsistent_winding:
        _write_mesh_repair_report(report_path, report)
        return report

    mesh_minimum = [min(position[axis] for position in positions) for axis in range(3)]
    mesh_maximum = [max(position[axis] for position in positions) for axis in range(3)]
    mesh_diagonal = math.sqrt(sum((mesh_maximum[axis] - mesh_minimum[axis]) ** 2 for axis in range(3)))
    if mesh_diagonal <= 0.0:
        _write_mesh_repair_report(report_path, report)
        return report

    working_faces = list(faces)
    removed_face_count = 0
    if original_non_manifold:
        remove_indices = {
            face_index
            for uses in original_usage.values()
            if len(uses) > 2
            for face_index, _left, _right in uses
        }
        max_removed_faces = max(64, int(len(faces) * MAX_LOCAL_REPAIR_FACE_RATIO))
        if len(remove_indices) > max_removed_faces:
            report["topology_deferred_reason"] = "too_many_non_manifold_faces"
            _write_mesh_repair_report(report_path, report)
            return report

        vertex_faces: dict[int, list[int]] = {}
        for face_index in remove_indices:
            for vertex in faces[face_index]:
                vertex_faces.setdefault(vertex, []).append(face_index)
        remaining_faces = set(remove_indices)
        defect_regions: list[set[int]] = []
        while remaining_faces:
            pending = [min(remaining_faces)]
            remaining_faces.remove(pending[0])
            region: set[int] = set()
            while pending:
                face_index = pending.pop()
                region.add(face_index)
                for vertex in faces[face_index]:
                    for adjacent in vertex_faces[vertex]:
                        if adjacent in remaining_faces:
                            remaining_faces.remove(adjacent)
                            pending.append(adjacent)
            defect_regions.append(region)

        max_region_diagonal_ratio = 0.0
        for region in defect_regions:
            defect_vertices = {vertex for face_index in region for vertex in faces[face_index]}
            defect_minimum = [min(positions[index][axis] for index in defect_vertices) for axis in range(3)]
            defect_maximum = [max(positions[index][axis] for index in defect_vertices) for axis in range(3)]
            defect_diagonal = math.sqrt(
                sum((defect_maximum[axis] - defect_minimum[axis]) ** 2 for axis in range(3))
            )
            max_region_diagonal_ratio = max(max_region_diagonal_ratio, defect_diagonal / mesh_diagonal)
        report.update(
            non_manifold_regions=len(defect_regions),
            max_non_manifold_region_diagonal_ratio=max_region_diagonal_ratio,
        )
        if max_region_diagonal_ratio > MAX_LOCAL_REPAIR_DIAGONAL_RATIO:
            report["topology_deferred_reason"] = "non_manifold_region_too_large"
            _write_mesh_repair_report(report_path, report)
            return report
        working_faces = [face for index, face in enumerate(faces) if index not in remove_indices]
        removed_face_count = len(remove_indices)

    def normalize_face_winding(
        source_faces: list[tuple[int, int, int]],
    ) -> tuple[list[tuple[int, int, int]] | None, int, int]:
        usage = edge_usage(source_faces)
        adjacency: list[list[tuple[int, bool]]] = [[] for _ in source_faces]
        inconsistent_before = 0
        for uses in usage.values():
            if len(uses) != 2:
                continue
            left_face, left_from, left_to = uses[0]
            right_face, right_from, right_to = uses[1]
            same_direction = left_from == right_from and left_to == right_to
            inconsistent_before += int(same_direction)
            adjacency[left_face].append((right_face, same_direction))
            adjacency[right_face].append((left_face, same_direction))

        flips: list[bool | None] = [None] * len(source_faces)
        for start in range(len(source_faces)):
            if flips[start] is not None:
                continue
            flips[start] = False
            pending = [start]
            component: list[int] = []
            while pending:
                face_index = pending.pop()
                component.append(face_index)
                for adjacent, must_differ in adjacency[face_index]:
                    expected = bool(flips[face_index]) ^ must_differ
                    if flips[adjacent] is None:
                        flips[adjacent] = expected
                        pending.append(adjacent)
                    elif flips[adjacent] != expected:
                        return None, 0, inconsistent_before
            if sum(bool(flips[index]) for index in component) > len(component) // 2:
                for index in component:
                    flips[index] = not bool(flips[index])

        oriented_faces = [
            (face[0], face[2], face[1]) if flips[index] else face
            for index, face in enumerate(source_faces)
        ]
        oriented_usage = edge_usage(oriented_faces)
        inconsistent_after = sum(
            len(uses) == 2 and uses[0][1:] == uses[1][1:]
            for uses in oriented_usage.values()
        )
        return oriented_faces, sum(bool(value) for value in flips), inconsistent_after

    oriented_faces, flipped_face_count, remaining_inconsistent_winding = normalize_face_winding(working_faces)
    report.update(
        flipped_winding_faces=flipped_face_count,
        remaining_inconsistent_winding_edges=remaining_inconsistent_winding,
    )
    if oriented_faces is None or remaining_inconsistent_winding:
        report["topology_deferred_reason"] = "non_orientable_face_winding"
        _write_mesh_repair_report(report_path, report)
        return report
    working_faces = oriented_faces

    usage = edge_usage(working_faces)
    if any(len(uses) > 2 for uses in usage.values()):
        _write_mesh_repair_report(report_path, report)
        return report
    boundary = {edge: uses[0][1:] for edge, uses in usage.items() if len(uses) == 1}

    outgoing: dict[int, list[tuple[int, tuple[int, int]]]] = {}
    incoming_count: Counter[int] = Counter()
    outgoing_count: Counter[int] = Counter()
    for edge, (left, right) in boundary.items():
        outgoing.setdefault(left, []).append((right, edge))
        outgoing_count[left] += 1
        incoming_count[right] += 1
    boundary_vertices = set(incoming_count) | set(outgoing_count)
    if any(
        incoming_count[index] != outgoing_count[index] or incoming_count[index] not in {1, 2}
        for index in boundary_vertices
    ):
        _write_mesh_repair_report(report_path, report)
        return report
    for entries in outgoing.values():
        entries.sort(reverse=True)

    unused = set(boundary)
    circuits: list[list[int]] = []
    while unused:
        start = min(left for edge, (left, _right) in boundary.items() if edge in unused)
        stack = [start]
        circuit: list[int] = []
        while stack:
            current = stack[-1]
            entries = outgoing.get(current, [])
            while entries and entries[-1][1] not in unused:
                entries.pop()
            if entries:
                right, edge = entries.pop()
                unused.remove(edge)
                stack.append(right)
            else:
                circuit.append(stack.pop())
        circuit.reverse()
        if len(circuit) < 4 or circuit[0] != circuit[-1]:
            _write_mesh_repair_report(report_path, report)
            return report
        circuits.append(circuit)

    cycles: list[list[int]] = []
    for circuit in circuits:
        remainder = circuit
        while len(remainder) > 1:
            seen: dict[int, int] = {}
            for index, vertex in enumerate(remainder):
                if vertex not in seen:
                    seen[vertex] = index
                    continue
                begin = seen[vertex]
                cycle = remainder[begin:index + 1]
                if len(cycle) < 4:
                    _write_mesh_repair_report(report_path, report)
                    return report
                cycles.append(cycle)
                remainder = remainder[:begin + 1] + remainder[index + 1:]
                break
            else:
                _write_mesh_repair_report(report_path, report)
                return report

    if sum(len(cycle) - 1 for cycle in cycles) != len(boundary):
        _write_mesh_repair_report(report_path, report)
        return report
    for cycle in cycles:
        cycle_vertices = cycle[:-1]
        cycle_minimum = [min(positions[index][axis] for index in cycle_vertices) for axis in range(3)]
        cycle_maximum = [max(positions[index][axis] for index in cycle_vertices) for axis in range(3)]
        cycle_diagonal = math.sqrt(
            sum((cycle_maximum[axis] - cycle_minimum[axis]) ** 2 for axis in range(3))
        )
        if len(cycle_vertices) > MAX_LOCAL_BOUNDARY_EDGES or cycle_diagonal > mesh_diagonal * MAX_LOCAL_REPAIR_DIAGONAL_RATIO:
            _write_mesh_repair_report(report_path, report)
            return report

    patched_faces = list(working_faces)
    for cycle in cycles:
        cycle_vertices = cycle[:-1]
        center = tuple(
            sum(positions[index][axis] for index in cycle_vertices) / len(cycle_vertices)
            for axis in range(3)
        )
        color = Counter(colors[index] for index in cycle_vertices).most_common(1)[0][0]
        center_index = len(positions)
        positions.append(center)
        colors.append(color)
        vertex_lines.append(
            "v " + " ".join(f"{value:.9g}" for value in center) + " " + " ".join(color)
        )
        patched_faces.extend(
            (cycle[index + 1], cycle[index], center_index)
            for index in range(len(cycle_vertices))
        )

    final_usage = edge_usage(patched_faces)
    remaining_invalid = sum(len(uses) != 2 for uses in final_usage.values())
    if remaining_invalid or len(patched_faces) > MAX_MODEL_FACES:
        _write_mesh_repair_report(report_path, report)
        return report

    referenced = sorted({vertex for face in patched_faces for vertex in face})
    remap = {old_index: new_index + 1 for new_index, old_index in enumerate(referenced)}
    temporary = path.with_suffix(path.suffix + ".part")
    try:
        with temporary.open("w", encoding="ascii", newline="\n") as output:
            output.write("# OrcaSlicer AI repaired small local mesh defects\n")
            for index in referenced:
                output.write(vertex_lines[index] + "\n")
            for face in patched_faces:
                output.write("f " + " ".join(str(remap[index]) for index in face) + "\n")
        os.replace(temporary, path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise TripoError("The generated OBJ could not be rewritten after topology repair.") from None

    original_vertex_count = len(vertex_lines) - len(cycles)
    removed_vertex_count = original_vertex_count - sum(index < original_vertex_count for index in referenced)
    report.update(
        status="repaired",
        topology_status="repaired",
        kept_vertices=len(referenced),
        kept_faces=len(patched_faces),
        removed_non_manifold_faces=removed_face_count,
        removed_topology_vertices=removed_vertex_count,
        filled_boundary_loops=len(cycles),
        added_vertices=len(cycles),
        added_faces=sum(len(cycle) - 1 for cycle in cycles),
        remaining_inconsistent_winding_edges=0,
        remaining_invalid_edges=0,
    )
    report.pop("topology_deferred_reason", None)
    _write_mesh_repair_report(report_path, report)
    return report


def _validate_obj_topology(
    path: Path, allow_repairable: bool = False, *, quality_advisory: bool = False
) -> tuple[int, int, int]:
    vertex_count = 0
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    vertex_count += 1
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    face = tuple(_resolve_obj_index(value, vertex_count, "vertex") for value in fields[1:])
                    if len(set(face)) != 3 and not quality_advisory:
                        raise TripoError("The generated OBJ contains a degenerate triangle.")
                    faces.append(face)
                    if len(faces) > MAX_MODEL_FACES:
                        raise TripoError(f"The generated OBJ exceeds the {MAX_MODEL_FACES}-triangle limit.")
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if vertex_count == 0 or not faces:
        raise TripoError("The generated OBJ does not contain usable geometry.")

    # Production checks file integrity here. The quality report already
    # calculates components and edge topology; repeating its large edge map
    # twice during delivery both wastes time and turns advice into a gate.
    if quality_advisory:
        return len(faces), 0, 0

    parent = list(range(vertex_count))

    def find(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    def unite(left: int, right: int) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parent[right_root] = left_root

    referenced: set[int] = set()
    edge_uses: dict[tuple[int, int], list[tuple[int, int]]] = {}
    for face in faces:
        referenced.update(face)
        unite(face[0], face[1])
        unite(face[1], face[2])
        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            edge = (left, right) if left < right else (right, left)
            edge_uses.setdefault(edge, []).append((left, right))

    component_count = len({find(index) for index in referenced})
    invalid_edges = sum(
        len(uses) != 2 or (len(uses) == 2 and uses[0] == uses[1])
        for uses in edge_uses.values()
    )
    repairable_edge_limit = max(64, len(faces) // 100)
    if invalid_edges and (
        not allow_repairable or len(faces) < 4 or invalid_edges > repairable_edge_limit
    ):
        raise TripoError(
            "Tripo generated a non-watertight, non-manifold, or inconsistently wound mesh. "
            "Regenerate before importing into OrcaSlicer."
        )
    return len(faces), component_count, invalid_edges


def _validate_artifact(path: Path, format_name: str, allow_repairable_obj: bool = False) -> int:
    try:
        size = path.stat().st_size
        with path.open("rb") as stream:
            signature = stream.read(84)
    except OSError:
        raise TripoError("The generated artifact could not be read.") from None
    if size <= 0 or size > MAX_ARTIFACT_BYTES:
        raise TripoError("The generated artifact has an invalid size.")
    if format_name == "obj":
        _validate_obj_vertex_colors(path)
        _validate_obj_topology(path, allow_repairable=allow_repairable_obj, quality_advisory=True)
    if format_name == "glb":
        try:
            Glb(path).mesh(with_colors=False)
        except (GlbError, OSError, ValueError, KeyError, TypeError) as exc:
            raise TripoError(f"The generated GLB is invalid: {exc}") from None
    if format_name == "3mf" and not signature.startswith(b"PK\x03\x04"):
        raise TripoError("Tripo returned an invalid 3MF artifact.")
    if format_name == "stl":
        ascii_stl = signature.lstrip().lower().startswith(b"solid")
        binary_stl = len(signature) >= 84 and int.from_bytes(signature[80:84], "little") > 0
        if not ascii_stl and not binary_stl:
            raise TripoError("Tripo returned an invalid STL artifact.")
    return size


def _can_manually_retry_hunyuan(job: Job) -> bool:
    """A confirmed rejection may be retried only by a new user confirmation."""
    return (
        job.provider == "hunyuan" and (job.state == "failed" or
            (job.state == "awaiting_confirmation" and job.phase == "model_retry")) and bool(job.attempts)
        and all(attempt.get("status") == "rejected"
                and attempt.get("provider") == "hunyuan"
                and attempt.get("provider_error_category") == "validation"
                and attempt.get("provider_error_ambiguous") is False
                and not attempt.get("generation_task_id")
                and not attempt.get("conversion_task_id") for attempt in job.attempts)
        and _model_generation_reference(job) is not None
    )


def _generate_job(
    job: Job,
    prepared_prompt: str,
    resume: bool = False,
    authorization: PaidTaskAuthorization | None = None,
) -> None:
    _use_unrestricted_creation(job)
    gateway = _model_gateway(job.provider)
    active_attempt = 0
    try:
        artifact: Path | None = None
        last_quality_error: TripoError | ProviderGatewayError | None = None
        first_attempt = 1
        if job.provider == "hunyuan" and job.attempts:
            first_attempt = len(job.attempts) if resume else len(job.attempts) + 1
        last_attempt = first_attempt + MAX_GENERATION_ATTEMPTS - 1
        for attempt_number in range(first_attempt, last_attempt + 1):
            active_attempt = attempt_number
            _stop_boundary(job)
            with _JOBS_LOCK:
                job.state = "running"
                job.phase = "generating"
                job.message = f"Generating printable model (attempt {attempt_number} of {last_attempt})."
                job.progress = 20
                _persist_job(job)
            existing = job.attempts[attempt_number - 1] if resume and len(job.attempts) >= attempt_number else {}
            generation_id = existing.get("generation_task_id", "")
            if not isinstance(generation_id, str):
                generation_id = ""
            if resume and not generation_id:
                raise TripoError("The paid model task reference is unavailable; start a new generation manually.")
            identity_geometry = _identity_preserving_portrait_geometry_enabled(job)
            preview = _geometry_generation_reference(job)
            # Four generated portrait views can be individually attractive yet
            # geometrically inconsistent. In real Tripo validation this produced
            # a Janus mesh with a second face on the back. The source-faithful,
            # hard-alpha front preserved identity and yielded one coherent head.
            # Build the material turntable only after geometry exists, when every
            # view is rendered from that exact mesh and therefore cannot disagree.
            multiview_paths = None if identity_geometry else (
                _multiview_paths_from_metrics(job, "generation_views")
                if generation_id
                else _ensure_portrait_multiview(job)
            )
            request_source = (
                "multiview" if multiview_paths is not None
                else "text" if job.source == "text" and preview is None
                else "image"
            )
            if request_source == "image":
                try:
                    _validate_image_file(
                        preview,
                        minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                        require_visual_detail=True,
                    )
                except ValueError as exc:
                    raise ProviderGatewayError(
                        f"The generated image is not suitable for 3D input: {exc}",
                        code="invalid_model_request",
                        category="validation",
                        provider=job.provider,
                        operation="model_generation",
                    ) from None
            with _JOBS_LOCK:
                job.phase = "generating"
                job.message = (
                    f"Generating the high-quality portrait from four views (attempt {attempt_number} of "
                    f"{last_attempt})."
                    if request_source == "multiview"
                    else f"Generating identity-first portrait geometry from the approved front view (attempt {attempt_number} of "
                    f"{last_attempt})."
                    if identity_geometry
                    else f"Generating printable model (attempt {attempt_number} of {last_attempt})."
                )
                job.progress = 20
                _persist_job(job)
            if not generation_id:
                if authorization is None:
                    raise ProviderGatewayError(
                        "Explicit confirmation is required before creating a paid model task.",
                        code="authorization_required",
                        category="authorization",
                        provider=job.provider,
                        operation="model_generation",
                    )
                _record_attempt(
                    job,
                    attempt_number,
                    provider=job.provider,
                    provider_operation="model_generation",
                    provider_request_id=authorization.request_id,
                    provider_model="hy-3d-3.1" if job.provider == "hunyuan" else "",
                    status="creating",
                    error="",
                )
                _persist_job(job, required=True)
            task_ref = gateway.start_or_reuse_model_task(
                ModelTaskRequest(
                    source=request_source,
                    prompt=prepared_prompt,
                    image_path=preview,
                    image_paths=multiview_paths,
                    face_limit=job.face_limit,
                    generation_profile=job.generation_profile,
                    geometry_quality=job.geometry_quality,
                    texture_quality=job.texture_quality,
                    output_format=job.output_format,
                ),
                existing_task_id=generation_id,
                authorization=authorization,
            )
            generation_id = task_ref.task_id
            if not task_ref.reused:
                _record_attempt(job, attempt_number, generation_task_id=generation_id, status="running")
            _stop_boundary(job)
            gateway.wait_for_task(
                generation_id,
                stop_event=job.stop_event,
                progress=_progress_callback(job, 20, 70),
            )
            _stop_boundary(job)
            try:
                candidate = _download_generation_artifact(job, generation_id, attempt_number, resume)
                face_count, _, _ = _validate_obj_topology(_analysis_artifact(candidate), quality_advisory=True)
                warning = _validate_face_target(face_count, job.face_limit)
                job.image_metrics["model_delivery_warnings"] = [warning] if warning else []
                artifact = job.directory / ("model.glb" if candidate.suffix.lower() == ".glb" else "model-vertex-color.obj")
                _promote_attempt_artifact(candidate, artifact)
                _record_attempt(job, attempt_number, status="accepted", artifact=str(candidate.name), error="")
                break
            except (TripoError, ProviderGatewayError) as exc:
                if _SHUT_DOWN:
                    raise SidecarRestart() from None
                message = str(exc)
                retryable_quality_error = any(
                    marker in message.lower()
                    for marker in ("triangle limit", "non-watertight", "non-manifold", "degenerate triangle")
                )
                updates: dict[str, Any] = {"status": "rejected", "error": message}
                if isinstance(exc, ProviderGatewayError):
                    updates.update(
                        provider_error_code=exc.code,
                        provider_error_category=exc.category,
                        provider_error_retryable=exc.retryable,
                        provider_error_ambiguous=exc.ambiguous,
                    )
                _record_attempt(job, attempt_number, **updates)
                if not retryable_quality_error or attempt_number == last_attempt:
                    raise
                last_quality_error = exc
        if artifact is None:
            raise last_quality_error or TripoError("No printable model passed validation.")

        visual_quality = _automatic_visual_review(job, artifact)
        _write_job_color_intent(job, artifact)
        with _JOBS_LOCK:
            if job.stop_event.is_set():
                raise JobStopped()
            job.artifact_path = artifact
            job.artifact_format = artifact.suffix.lower().lstrip(".")
            job.state = "ready"
            job.phase = "ready"
            job.message = (
                "Generated model is ready, but visual review found identity or material risks."
                if visual_quality is not None and not visual_quality.get("import_recommended", True)
                else "Generated model is ready. Quality checks are advisory; visual review is available on request."
            )
            job.progress = 100
            _persist_job(job)
    except SidecarRestart:
        with _JOBS_LOCK:
            job.state = "queued"
            job.phase = "resuming"
            job.message = "The existing paid model task will resume when the sidecar restarts."
            _persist_job(job)
    except JobStopped:
        _mark_stopped(job)
    except PortraitMultiviewPreparationError as exc:
        _return_to_portrait_multiview_retry(job, str(exc))
    except ProviderGatewayError as exc:
        if _SHUT_DOWN:
            # ``wait_for_task`` uses the job stop event to interrupt its local
            # polling loop during shutdown.  That interruption does not cancel
            # the already-paid remote task, so do not persist it as a provider
            # rejection.  A restarted sidecar must see the attempt as running
            # and resume the same task id instead of presenting a contradictory
            # "rejected" attempt beside a live progress bar.
            with _JOBS_LOCK:
                job.state = "queued"
                job.phase = "resuming"
                job.message = "The existing paid model task will resume when the sidecar restarts."
                _persist_job(job)
        else:
            if active_attempt:
                _record_attempt(
                    job,
                    active_attempt,
                    status="rejected",
                    error=str(exc),
                    provider_error_code=exc.code,
                    provider_error_category=exc.category,
                    provider_error_retryable=exc.retryable,
                    provider_error_ambiguous=exc.ambiguous,
                )
            _fail_job(job, str(exc))
    except TripoError as exc:
        if _SHUT_DOWN:
            with _JOBS_LOCK:
                job.state = "queued"
                job.phase = "resuming"
                job.message = "The existing paid model task will resume when the sidecar restarts."
                _persist_job(job)
        else:
            _fail_job(job, str(exc))
    except Exception as exc:
        print(
            f"[orca-ai] unexpected model-generation error for job {job.id}: "
            f"{type(exc).__name__}: {exc}",
            file=sys.stderr,
            flush=True,
        )
        traceback.print_exc()
        if (
            not job.attempts
            and _quality_portrait_multiview_enabled(job)
            and not _identity_preserving_portrait_geometry_enabled(job)
            and job.model_reference_path is not None
        ):
            _return_to_portrait_multiview_retry(
                job,
                "Four-view portrait preparation hit a local error before any paid Tripo task was created. Retry generation.",
            )
        else:
            _fail_job(job, "Model generation failed.")
    finally:
        _finish_deleted(job)


def _retexture_job(
    job: Job,
    source_job_id: str,
    source_task_id: str,
    resume: bool = False,
    authorization: PaidTaskAuthorization | None = None,
) -> None:
    _use_unrestricted_creation(job)
    try:
        _stop_boundary(job)
        with _JOBS_LOCK:
            job.state = "running"
            job.phase = "texturing"
            job.message = "Preserving the selected geometry and applying the current portrait colors."
            job.progress = 20
            _persist_job(job)
        existing = job.attempts[0] if resume and job.attempts else {}
        generation_id = existing.get("generation_task_id", "")
        if not isinstance(generation_id, str):
            generation_id = ""
        if resume and not generation_id:
            raise TripoError("The paid texture task reference is unavailable; start a new task manually.")
        reference = _model_generation_reference(job)
        try:
            _validate_image_file(
                reference,
                minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                require_visual_detail=True,
            )
        except ValueError as exc:
            raise ProviderGatewayError(
                f"The texture reference image is not suitable: {exc}",
                code="invalid_texture_request",
                category="validation",
                provider="tripo",
                operation="model_texture",
            ) from None
        if not generation_id:
            if authorization is None:
                raise ProviderGatewayError(
                    "Explicit confirmation is required before creating a paid texture task.",
                    code="authorization_required",
                    category="authorization",
                    provider="tripo",
                    operation="model_texture",
                )
            _record_attempt(
                job,
                1,
                provider="tripo",
                provider_operation="model_texture",
                provider_request_id=authorization.request_id,
                source_job_id=source_job_id,
                source_task_id=source_task_id,
                status="creating",
                error="",
            )
        task_ref = _MODEL_PROVIDER_GATEWAY.start_or_reuse_texture_task(
            TextureTaskRequest(
                source_task_id=source_task_id,
                image_path=reference,
                texture_alignment="geometry",
                texture_quality="standard",
            ),
            existing_task_id=generation_id,
            authorization=authorization,
        )
        generation_id = task_ref.task_id
        if not task_ref.reused:
            _record_attempt(job, 1, generation_task_id=generation_id, status="running")
        _stop_boundary(job)
        _MODEL_PROVIDER_GATEWAY.wait_for_task(
            generation_id,
            stop_event=job.stop_event,
            progress=_progress_callback(job, 20, 70),
        )
        _stop_boundary(job)
        candidate = _download_generation_artifact(job, generation_id, 1, resume)
        face_count, _, _ = _validate_obj_topology(_analysis_artifact(candidate), quality_advisory=True)
        warning = _validate_face_target(face_count, job.face_limit)
        job.image_metrics["model_delivery_warnings"] = [warning] if warning else []
        artifact = job.directory / ("model.glb" if candidate.suffix.lower() == ".glb" else "model-vertex-color.obj")
        _promote_attempt_artifact(candidate, artifact)
        _record_attempt(job, 1, status="accepted", artifact=str(candidate.name), error="")
        visual_quality = _automatic_visual_review(job, artifact)
        _write_job_color_intent(job, artifact)
        with _JOBS_LOCK:
            if job.stop_event.is_set():
                raise JobStopped()
            job.artifact_path = artifact
            job.artifact_format = artifact.suffix.lower().lstrip(".")
            job.state = "ready"
            job.phase = "ready"
            job.message = (
                "The preserved-geometry portrait is ready, but visual review found identity or material risks."
                if visual_quality is not None and not visual_quality.get("import_recommended", True)
                else "The preserved-geometry portrait model is ready."
            )
            job.progress = 100
            _persist_job(job)
    except SidecarRestart:
        with _JOBS_LOCK:
            job.state = "queued"
            job.phase = "resuming"
            job.message = "The existing paid texture task will resume when the sidecar restarts."
            _persist_job(job)
    except JobStopped:
        _mark_stopped(job)
    except ProviderGatewayError as exc:
        _record_attempt(
            job,
            1,
            status="rejected",
            error=str(exc),
            provider_error_code=exc.code,
            provider_error_category=exc.category,
            provider_error_retryable=exc.retryable,
            provider_error_ambiguous=exc.ambiguous,
        )
        if _SHUT_DOWN:
            with _JOBS_LOCK:
                job.state = "queued"
                job.phase = "resuming"
                job.message = "The existing paid texture task will resume when the sidecar restarts."
                _persist_job(job)
        else:
            _fail_job(job, str(exc))
    except TripoError as exc:
        if _SHUT_DOWN:
            with _JOBS_LOCK:
                job.state = "queued"
                job.phase = "resuming"
                job.message = "The existing paid texture task will resume when the sidecar restarts."
                _persist_job(job)
        else:
            _record_attempt(job, 1, status="rejected", error=str(exc))
            _fail_job(job, str(exc))
    except Exception:
        _record_attempt(job, 1, status="rejected", error="Texture generation failed.")
        _fail_job(job, "Texture generation failed.")
    finally:
        _finish_deleted(job)


def _run_worker_with_diagnostics(job: Job, worker: Callable[..., None], args: tuple[Any, ...]) -> None:
    with diagnostic_context(job.id):
        started = time.monotonic()
        worker_name = getattr(worker, "__name__", type(worker).__name__)
        diagnostic_event("job.worker.started", worker=worker_name, source=job.source, phase=job.phase)
        try:
            worker(job, *args)
        except Exception as exc:
            diagnostic_event(
                "job.worker.unhandled_exception",
                level="ERROR",
                worker=worker_name,
                exception_chain=exception_details(exc),
            )
            raise
        finally:
            diagnostic_event(
                "job.worker.completed",
                worker=worker_name,
                state=job.state,
                phase=job.phase,
                elapsed_ms=round((time.monotonic() - started) * 1000),
            )


def _executor_for(worker: Callable[..., None]) -> ThreadPoolExecutor:
    # Provider geometry is intentionally serialized in its own lane so a long
    # paid task cannot starve palette recommendation or Image2 preprocessing.
    return _MODEL_EXECUTOR if worker in {_generate_job, _retexture_job} else _DESIGN_EXECUTOR


def _submit(job: Job, worker: Callable[..., None], *args: Any) -> None:
    try:
        future = _executor_for(worker).submit(_run_worker_with_diagnostics, job, worker, args)
    except RuntimeError:
        raise RequestError("service_unavailable", "The model job service is shutting down.", 503, True) from None
    with _JOBS_LOCK:
        job.future = future


def shutdown_sidecar() -> None:
    global _SHUT_DOWN
    with _SHUTDOWN_LOCK:
        if _SHUT_DOWN:
            return
        _SHUT_DOWN = True
    with _JOBS_LOCK:
        jobs = list(_JOBS.values())
        for job in jobs:
            job.stop_event.set()
    _DESIGN_EXECUTOR.shutdown(wait=True, cancel_futures=False)
    _MODEL_EXECUTOR.shutdown(wait=True, cancel_futures=False)
    with _JOBS_LOCK:
        _JOBS.clear()


atexit.register(shutdown_sidecar)


class Handler(BaseHTTPRequestHandler):
    server_version = "OrcaAISidecar/1.0"

    def log_message(self, fmt: str, *args: Any) -> None:
        diagnostic_event("http.access", detail=fmt % args, client_host=self.client_address[0])

    def send_json(self, status: int, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        try:
            self.wfile.write(encoded)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            # The native client may cancel an obsolete poll while a newer one is
            # already in flight.  A closed response socket is not a job failure
            # and must not surface as an alarming server-side traceback.
            pass

    def send_bytes(
        self,
        stream: BinaryIO,
        size: int,
        content_type: str,
        filename: str | None = None,
    ) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(size))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", "no-store")
        if filename is not None:
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.end_headers()
        try:
            while chunk := stream.read(64 * 1024):
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            pass

    def read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return {}
        if length > MAX_REQUEST_BYTES:
            raise ValueError("request body too large")
        parsed = json.loads(self.rfile.read(length).decode("utf-8"))
        if not isinstance(parsed, dict):
            raise ValueError("request body must be a JSON object")
        return parsed

    def _read_body(self, limit: int) -> bytes:
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            raise RequestError("invalid_request", "Content-Length is required.", 400)
        try:
            length = int(raw_length)
        except ValueError:
            raise RequestError("invalid_request", "Content-Length is invalid.", 400) from None
        if length < 0:
            raise RequestError("invalid_request", "Content-Length is invalid.", 400)
        if length > limit:
            raise RequestError("request_too_large", "Request body is too large.", 413)
        body = self.rfile.read(length)
        if len(body) != length:
            raise RequestError("invalid_request", "Request body is incomplete.", 400)
        return body

    def _read_model_json(self) -> dict[str, Any]:
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type != "application/json":
            raise RequestError("unsupported_media_type", "Content-Type must be application/json.", 415)
        body = self._read_body(MAX_REQUEST_BYTES)
        try:
            parsed = json.loads(body.decode("utf-8")) if body else {}
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise RequestError("invalid_json", "Request body contains malformed JSON.", 400) from None
        if not isinstance(parsed, dict):
            raise RequestError("invalid_request", "Request body must be a JSON object.", 400)
        return parsed

    def _read_image_multipart(self) -> tuple[dict[str, str], bytes, str]:
        content_type = self.headers.get("Content-Type", "")
        if len(content_type) > 1024 or not content_type.lower().startswith("multipart/form-data;"):
            raise RequestError("unsupported_media_type", "Content-Type must be multipart/form-data.", 415)
        body = self._read_body(MAX_MULTIPART_BYTES)
        try:
            header = b"Content-Type: " + content_type.encode("latin-1") + b"\r\nMIME-Version: 1.0\r\n\r\n"
        except UnicodeEncodeError:
            raise RequestError("invalid_multipart", "Multipart Content-Type is invalid.", 400) from None
        message = BytesParser(policy=policy.default).parsebytes(header + body)
        if not message.is_multipart():
            raise RequestError("invalid_multipart", "Multipart request is malformed.", 400)

        fields: dict[str, str] = {}
        image: bytes | None = None
        image_content_type = ""
        seen: set[str] = set()
        for part in message.iter_parts():
            if part.is_multipart() or part.get_content_disposition() != "form-data":
                raise RequestError("invalid_multipart", "Nested or invalid multipart data is not supported.", 400)
            name = part.get_param("name", header="content-disposition")
            if name not in {
                "request_id", "instruction", "palette", "palette_roles", "palette_recommendation_confirmed", "provider",
                "face_limit", "geometry_quality", "texture_quality", "output_format",
                "palette_color_count", "style", "custom_style", "print", "image", "generate_image",
            } or name in seen:
                raise RequestError("invalid_multipart", "Multipart fields are unexpected or duplicated.", 400)
            seen.add(name)
            payload = part.get_payload(decode=True) or b""
            if name == "image":
                image = payload
                image_content_type = part.get_content_type().lower()
            else:
                if len(payload) > MAX_PROMPT_BYTES:
                    raise RequestError("invalid_request", f"{name} exceeds the 2000-byte limit.", 400)
                try:
                    fields[name] = payload.decode(part.get_content_charset() or "utf-8")
                except (LookupError, UnicodeDecodeError):
                    raise RequestError("invalid_request", f"{name} must be UTF-8 text.", 400) from None
        if image is None:
            raise RequestError("invalid_request", "image is required.", 400)
        return fields, image, image_content_type

    def _require_session(self) -> bool:
        expected = _configured_session_token()
        if expected is None:
            self._model_error(503, "session_configuration_invalid", "AI Sidecar session protection is invalid.")
            return False
        if not expected and _session_required():
            self._model_error(503, "session_configuration_missing", "AI Sidecar session protection is required.")
            return False
        if expected:
            expected_proof = _session_hmac(expected, f"client:{SIDECAR_SESSION_NONCE}")
            provided = self.headers.get("X-OrcaSlicer-Session-Proof", "")
            if len(provided) != len(expected_proof) or not hmac.compare_digest(provided, expected_proof):
                self._model_error(401, "session_required", "A valid OrcaSlicer AI session is required.")
                return False
        return True

    def _require_native_client(self) -> bool:
        if self.headers.get("X-OrcaSlicer-Client") != "native":
            self._model_error(401, "client_required", "X-OrcaSlicer-Client must be native.")
            return False
        return self._require_session()

    def _model_error(
        self,
        status: int,
        code: str,
        message: str,
        retryable: bool = False,
    ) -> None:
        self.send_json(status, {"error": {"code": code, "message": message, "retryable": retryable}})

    @staticmethod
    def _job_route(path: str) -> tuple[str | None, str | None]:
        prefix = "/v1/orcaslicer/model-jobs/"
        if not path.startswith(prefix):
            return None, None
        parts = path[len(prefix) :].split("/")
        if len(parts) == 1 and parts[0]:
            action = "status"
        elif len(parts) == 2 and parts[0] and (
            parts[1] in {
                "input", "raw-preview", "strict-preview", "preview", "model-reference", "heatmap", "metadata",
                "background-mask", "subject-mask", "generate", "retexture", "stop", "artifact", "color-intent",
                "recheck", "visual-review", "model-view-sheet", "confirm-palette", "generation-options",
            }
            or re.fullmatch(r"mask-[a-z0-9_]+", parts[1])
        ):
            action = parts[1]
        else:
            return None, None
        try:
            parsed = uuid.UUID(parts[0])
        except ValueError:
            return None, None
        if str(parsed) != parts[0].lower():
            return None, None
        return parts[0].lower(), action

    def _get_job(self, job_id: str) -> Job | None:
        with _JOBS_LOCK:
            return _JOBS.get(job_id)

    def do_GET(self) -> None:
        if self.path == "/v1/orcaslicer/session-challenge":
            token = _configured_session_token()
            if token is None:
                self._model_error(503, "session_configuration_invalid", "AI Sidecar session protection is invalid.")
                return
            client_nonce = self.headers.get("X-OrcaSlicer-Client-Nonce", "")
            if not token or not re.fullmatch(r"[0-9A-Fa-f]{64}", client_nonce):
                self._model_error(401, "session_challenge_required", "A valid session challenge is required.")
                return
            self.send_json(
                200,
                {
                    "ok": True,
                    "protocol_version": 2,
                    "sidecar_version": SIDECAR_VERSION,
                    "session_protected": True,
                    "server_nonce": SIDECAR_SESSION_NONCE,
                    "server_proof": _session_hmac(
                        token, f"server:{client_nonce}:{SIDECAR_SESSION_NONCE}"
                    ),
                },
            )
            return
        if self.path == "/health":
            if not self._require_session():
                return
            config = os.environ.get("OPENAI_API_KEY", "")
            image_provider = image_provider_status()
            text_preprocessing = image_provider["available"] or _preprocess_fallback_enabled()
            generation_preprocessing = text_preprocessing
            policy = provider_policy()
            self.send_json(
                200,
                {
                    "ok": True,
                    "protocol_version": 2,
                    "sidecar_version": SIDECAR_VERSION,
                    "runtime": {
                        "health_schema_version": 2,
                        "instance_id": SIDECAR_INSTANCE_ID,
                        "session_protected": bool(_configured_session_token()),
                        "build": _safe_runtime_identity(),
                        "openai_base_url": safe_endpoint(
                            os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
                        ).rstrip("/"),
                        "image_provider_base_url": image_provider["base_url"],
                        "image_provider_source": image_provider["source"],
                        "tripo_base_url": safe_endpoint(
                            os.environ.get("TRIPO_API_BASE", "https://openapi.tripo3d.com/v3")
                        ).rstrip("/"),
                        "configuration_mode": "internal_locked"
                        if os.environ.get("ORCASLICER_AI_CONFIG_MODE") == "internal_locked"
                        else "external",
                        "network": _runtime_network_metadata(),
                    },
                    "capabilities": {
                        "config_proposal": {"available": bool(config)},
                        "model_generation": {
                            "available": generation_preprocessing and any(
                                _model_gateway(name).model_generation_available() for name in policy.geometry_providers),
                            "providers": {name: {"available": _model_gateway(name).model_generation_available()}
                                          for name in policy.geometry_providers},
                            "sources": ["text", "image"],
                            "styles": list(STYLE_IDS),
                            "artifact_formats": ["glb", "obj"],
                            "face_limits": [300000, 1000000, 2000000],
                            "geometry_qualities": ["standard", "detailed"],
                            "texture_qualities": ["standard", "detailed", "extreme"],
                            "output_formats": ["glb", "obj"],
                            "default_face_limit": GENERATION_PROFILE_FACE_LIMITS[DEFAULT_GENERATION_PROFILE],
                            "generation_profiles": list(GENERATION_PROFILES),
                            "default_generation_profile": DEFAULT_GENERATION_PROFILE,
                            "provider_policy": {
                                "design_providers": list(policy.design_providers),
                                "geometry_provider": policy.geometry_provider,
                                "geometry_providers": list(policy.geometry_providers),
                                "automatic_fallback": policy.automatic_fallback,
                                "max_paid_model_tasks_per_confirmation":
                                    policy.max_paid_model_tasks_per_confirmation,
                            },
                            "source_availability": {
                                "text": text_preprocessing,
                                "image": image_provider["available"],
                            },
                            "image_provider": image_provider,
                            "palette_recommendation": {
                                "available": False,
                                "reason": "creation_preserves_natural_colors",
                            },
                            "style_recommendation": {
                                "available": True,
                                "local_only": True,
                            },
                            "printable_image_pipeline": {
                                "available": False,
                                "print_modes": ["solid_regions"],
                                "color_distances": ["ciede2000", "delta_e76"],
                                "outputs": [
                                    "raw_preview", "strict_preview", "clean_preview", "model_reference",
                                    "heatmap", "masks", "metadata",
                                ],
                            },
                        },
                    },
                },
            )
            return

        if not self.path.startswith("/v1/orcaslicer/model-jobs"):
            self.send_json(404, {"error": "not found"})
            return
        if not self._require_native_client():
            return
        if self.path == "/v1/orcaslicer/model-jobs/latest":
            with _JOBS_LOCK:
                candidates = [job for job in _JOBS.values() if _latest_job_is_restorable(job)]
                response = _public_job(max(candidates, key=lambda item: item.updated_at)) if candidates else None
            self.send_json(200, {"job": response})
            return
        job_id, action = self._job_route(self.path)
        downloadable = {
            "status", "input", "raw-preview", "strict-preview", "preview", "model-reference", "heatmap", "metadata",
            "background-mask", "subject-mask", "artifact", "color-intent", "model-view-sheet",
        }
        if not job_id or (action not in downloadable and not (action or "").startswith("mask-")):
            self._model_error(404, "not_found", "Model job route not found.")
            return
        job = self._get_job(job_id)
        if job is None and action == "status":
            # Model-library entries created before durable job manifests still
            # retain their bounded provider attempt log.  Adopt them lazily so
            # the native history UI can backfill the real 3D provider task ID
            # without scanning or reviving every legacy model at startup.
            job = _adopt_legacy_completed_job(job_id)
        if job is None:
            self._model_error(404, "job_not_found", "Model job not found.")
            return
        if action == "status":
            with _JOBS_LOCK:
                response = _public_job(job)
            self.send_json(200, {"job": response})
            return
        self._download_job_file(job, action)

    def do_POST(self) -> None:
        if self.path == "/v1/orcaslicer/shutdown":
            if not self._require_native_client():
                return
            self.send_json(202, {"ok": True, "state": "stopping"})
            threading.Thread(
                target=self.server.shutdown,
                name="orca-sidecar-shutdown",
                daemon=True,
            ).start()
            return

        if self.path == "/v1/orcaslicer/config-proposal":
            if not self._require_native_client():
                return
            try:
                request = self.read_json()
                if not str(request.get("user_message", "")).strip():
                    self.send_json(400, {"error": "user_message is required"})
                    return
                if not extract_allowed_keys(request):
                    self.send_json(400, {"error": "allowed_changes is required"})
                    return
                self.send_json(200, normalize_proposal(provider_request(request), request))
            except ValueError as exc:
                self.send_json(400, {"error": str(exc)})
            except Exception as exc:
                self.send_json(502, {"error": str(exc)})
            return

        if self.path == "/v1/orcaslicer/journey-events":
            if not self._require_native_client():
                return
            try:
                record = _record_journey_event(self._read_model_json())
                self.send_json(201, {"event": record})
            except RequestError as exc:
                self._model_error(exc.status, exc.code, exc.message, exc.retryable)
            return

        if self.path == "/v1/orcaslicer/model-style-recommendation":
            if not self._require_native_client():
                return
            try:
                self._recommend_model_style()
            except RequestError as exc:
                self._model_error(exc.status, exc.code, exc.message, exc.retryable)
            return

        if not self.path.startswith("/v1/orcaslicer/model-jobs"):
            self.send_json(404, {"error": "not found"})
            return
        if not self._require_native_client():
            return
        try:
            if self.path == "/v1/orcaslicer/model-jobs/text":
                self._create_text_job()
                return
            if self.path == "/v1/orcaslicer/model-jobs/image":
                self._create_image_job()
                return
            if self.path == "/v1/orcaslicer/model-jobs/recommend-text-palette":
                self._create_text_palette_recommendation()
                return
            if self.path == "/v1/orcaslicer/model-jobs/recommend-image-palette":
                self._create_image_palette_recommendation()
                return
            job_id, action = self._job_route(self.path)
            if not job_id or action not in {
                "generate", "retexture", "stop", "recheck", "visual-review", "confirm-palette", "generation-options"
            }:
                self._model_error(404, "not_found", "Model job route not found.")
                return
            if action == "generate":
                self._generate(job_id)
            elif action == "generation-options":
                self._set_generation_options(job_id)
            elif action == "retexture":
                self._retexture(job_id)
            elif action == "confirm-palette":
                self._confirm_palette(job_id)
            elif action == "recheck":
                self._recheck(job_id)
            elif action == "visual-review":
                self._visual_review(job_id)
            else:
                self._stop(job_id)
        except RequestError as exc:
            self._model_error(exc.status, exc.code, exc.message, exc.retryable)

    def do_DELETE(self) -> None:
        if not self.path.startswith("/v1/orcaslicer/model-jobs"):
            self.send_json(404, {"error": "not found"})
            return
        if not self._require_native_client():
            return
        job_id, action = self._job_route(self.path)
        if not job_id or action != "status":
            self._model_error(404, "not_found", "Model job route not found.")
            return
        with _JOBS_LOCK:
            job = _JOBS.get(job_id)
            if job is None:
                self._model_error(404, "job_not_found", "Model job not found.")
                return
            if job.state in {"recommending_palette", "preprocessing", "queued", "running", "stopping"}:
                job.delete_requested = True
                job.stop_event.set()
                job.state = "stopping"
                job.phase = "stopping"
                job.message = "Stopping model generation."
            else:
                _JOBS.pop(job_id)
                _remove_job_state(job)
        self.send_response(204)
        self.end_headers()

    def _create_text_job(self) -> None:
        if not image_provider_status()["available"] and not _preprocess_fallback_enabled():
            raise RequestError("feature_unavailable", "AI style preview generation is not configured.", 503)
        request = self._read_model_json()
        _text_field(request.get("request_id"), "request_id")
        prompt = _text_field(request.get("prompt"), "prompt")
        palette = ()
        palette_roles = {}
        style = _normalize_style(request.get("style"))
        custom_style = _normalize_custom_style(request.get("custom_style"), style)
        print_settings = _normalize_print_settings(request.get("print"))
        job = _new_job("text", palette, palette_roles, style, custom_style, print_settings,
                       provider=_generation_provider(request), generation_options=request)
        job.palette_recommendation_confirmed = _boolean_field(
            request.get("palette_recommendation_confirmed"), "palette_recommendation_confirmed"
        )
        job.user_prompt = prompt
        _persist_job(job)
        with _JOBS_LOCK:
            _JOBS[job.id] = job
        try:
            _submit(job, _preprocess_text_job, prompt)
        except RequestError:
            with _JOBS_LOCK:
                _JOBS.pop(job.id, None)
            _cleanup_job(job)
            raise
        with _JOBS_LOCK:
            response = _public_job(job)
        self.send_json(202, {"job": response})

    def _create_text_palette_recommendation(self) -> None:
        # Compatibility route: old clients now create an unrestricted design.
        self._create_text_job()

    def _recommend_model_style(self) -> None:
        fields, image, declared_type = self._read_image_multipart()
        prompt = fields.get("instruction", "").strip()
        if len(image) > MAX_IMAGE_BYTES:
            raise RequestError("image_too_large", "Image exceeds the 20 MB limit.", 413)
        detected_type = _image_type(image)
        if detected_type is None:
            raise RequestError("unsupported_image", "Image must be PNG or JPEG.", 415)
        if declared_type not in {"application/octet-stream", detected_type}:
            raise RequestError("unsupported_image", "Image Content-Type does not match its data.", 415)
        try:
            _validate_image_data(image, minimum_edge=MIN_SOURCE_IMAGE_EDGE)
            recommendation = recommend_printable_style(image, prompt=prompt)
        except (ValueError, ModelInputImageQualityError) as exc:
            raise RequestError("invalid_image", str(exc), 415) from None
        self.send_json(200, {"recommendation": recommendation})

    def _create_image_job(self) -> None:
        if not image_provider_status()["available"]:
            raise RequestError("feature_unavailable", "AI style preview generation is not configured.", 503)
        fields, image, declared_type = self._read_image_multipart()
        _text_field(fields.get("request_id"), "request_id")
        user_instruction = _user_image_instruction(fields.get("instruction"))
        instruction = _normalize_image_instruction(user_instruction)
        palette = ()
        palette_roles = {}
        style = _normalize_style(fields.get("style"))
        custom_style = _normalize_custom_style(fields.get("custom_style"), style)
        palette_recommendation_confirmed = _boolean_field(
            fields.get("palette_recommendation_confirmed"), "palette_recommendation_confirmed"
        )
        try:
            print_payload = json.loads(fields.get("print", "{}"))
        except json.JSONDecodeError:
            raise RequestError("invalid_print_settings", "print settings must be valid JSON", 400) from None
        print_settings = _normalize_print_settings(print_payload)
        if len(image) > MAX_IMAGE_BYTES:
            raise RequestError("image_too_large", "Image exceeds the 20 MB limit.", 413)
        detected_type = _image_type(image)
        if detected_type is None:
            raise RequestError("unsupported_image", "Image must be PNG or JPEG.", 415)
        if declared_type not in {"application/octet-stream", detected_type}:
            raise RequestError("unsupported_image", "Image Content-Type does not match its data.", 415)
        try:
            _validate_image_data(image, minimum_edge=MIN_SOURCE_IMAGE_EDGE)
        except ValueError as exc:
            raise RequestError("invalid_image", str(exc), 415) from None
        job = _new_job("image", palette, palette_roles, style, custom_style, print_settings,
                       provider=_generation_provider(fields), generation_options=fields)
        job.palette_recommendation_confirmed = palette_recommendation_confirmed
        job.user_prompt = user_instruction
        suffix = ".png" if detected_type == "image/png" else ".jpg"
        input_path = job.directory / f"input-{uuid.uuid4().hex}{suffix}"
        try:
            input_path.write_bytes(image)
        except OSError:
            _cleanup_job(job)
            raise RequestError("service_unavailable", "The uploaded image could not be stored.", 503, True) from None
        job.input_path = input_path
        _persist_job(job)
        with _JOBS_LOCK:
            _JOBS[job.id] = job
        try:
            _submit(job, _preprocess_image_job, input_path, instruction)
        except RequestError:
            with _JOBS_LOCK:
                _JOBS.pop(job.id, None)
            _cleanup_job(job)
            raise
        with _JOBS_LOCK:
            response = _public_job(job)
        self.send_json(202, {"job": response})

    def _create_image_palette_recommendation(self) -> None:
        # Compatibility route: old clients now create an unrestricted design.
        self._create_image_job()

    def _confirm_palette(self, job_id: str) -> None:
        request = self._read_model_json()
        job = self._get_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with _JOBS_LOCK:
            if job.state != "awaiting_palette_confirmation" or not job.palette_recommendation:
                raise RequestError("invalid_job_state", "Job is not awaiting palette confirmation.", 409)
            previous_recommendation = job.palette_recommendation
            _use_unrestricted_creation(job)
            job.state = "preprocessing"
            job.phase = "preprocessing"
            job.message = "An unrestricted design preview is being prepared."
            job.progress = 10
            _persist_job(job)
        try:
            if job.source == "text":
                _submit(job, _preprocess_text_job, job.user_prompt)
            elif job.input_path is not None:
                _submit(job, _preprocess_image_job, job.input_path, _normalize_image_instruction(job.user_prompt))
            else:
                raise RequestError("input_unavailable", "The stored reference image is unavailable.", 409)
        except RequestError:
            with _JOBS_LOCK:
                job.palette_recommendation_confirmed = False
                job.palette_recommendation = previous_recommendation
                job.state = "awaiting_palette_confirmation"
                job.phase = "awaiting_palette_confirmation"
                job.message = "Review and confirm the recommended design colors."
                job.progress = 10
                _persist_job(job)
            raise
        with _JOBS_LOCK:
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _set_generation_options(self, job_id: str) -> None:
        request = self._read_model_json()
        if set(request) != {"provider", "face_limit", "geometry_quality", "texture_quality", "output_format"}:
            raise RequestError("invalid_request", "Complete generation options are required.", 400)
        provider = _generation_provider(request)
        face_limit = _normalize_face_limit(request["face_limit"])
        geometry, texture, output = _generation_options(request, face_limit)
        with _JOBS_LOCK:
            job = self._get_job(job_id)
            if job is None:
                raise RequestError("job_not_found", "Model job not found.", 404)
            retry_design = _can_manually_retry_hunyuan(job)
            if (job.state != "awaiting_confirmation" or job.attempts) and not retry_design:
                raise RequestError("invalid_job_state", "Only an unsubmitted design can change generation options.", 409)
            if retry_design and provider != job.provider:
                raise RequestError("invalid_request", "A rejected Hunyuan design must keep its provider for retry.", 409)
            names = ("provider", "face_limit", "generation_profile", "geometry_quality", "texture_quality", "output_format")
            previous = tuple(getattr(job, name) for name in names)
            values = (provider, face_limit, "quality" if face_limit >= 500000 else "performance", geometry, texture, output)
            for name, value in zip(names, values):
                setattr(job, name, value)
            try:
                _persist_job(job, required=True)
            except TripoError:
                for name, value in zip(names, previous):
                    setattr(job, name, value)
                raise RequestError("state_save_failed", "Generation options could not be saved.", 503, True) from None
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _generate(self, job_id: str) -> None:
        request = self._read_model_json()
        if "prepared_prompt" not in request:
            raise RequestError("invalid_request", "prepared_prompt is required.", 400)
        raw_prompt = request.get("prepared_prompt")
        if not isinstance(raw_prompt, str):
            raise RequestError("invalid_request", "prepared_prompt must be a string.", 400)
        if len(raw_prompt.strip().encode("utf-8")) > MAX_PROMPT_BYTES:
            raise RequestError("invalid_request", "prepared_prompt exceeds the 2000-byte limit.", 400)
        job = self._get_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        if "generation_profile" in request and "face_limit" not in request:
            generation_profile = _normalize_generation_profile(request.get("generation_profile"))
            face_limit = GENERATION_PROFILE_FACE_LIMITS[generation_profile]
        else:
            face_limit = _normalize_face_limit(request.get("face_limit", DEFAULT_MODEL_FACE_LIMIT))
            generation_profile = "quality" if face_limit >= 500000 else "performance"
        geometry_quality, texture_quality, output_format = _generation_options(request, face_limit)
        provider = _generation_provider(request)
        with _JOBS_LOCK:
            manual_retry = _can_manually_retry_hunyuan(job)
            if job.state != "awaiting_confirmation" and not manual_retry:
                raise RequestError("invalid_job_state", "Job is not awaiting confirmation.", 409)
            if manual_retry and provider != job.provider:
                raise RequestError("invalid_request", "A rejected Hunyuan design must keep its provider for retry.", 409)
            _use_unrestricted_creation(job)
            prepared_prompt = raw_prompt.strip()
            reference = _model_generation_reference(job)
            if job.source == "text" and reference is None and not prepared_prompt:
                raise RequestError("invalid_request", "prepared_prompt is required for text generation.", 400)
            if job.source == "image" or reference is not None:
                try:
                    _validate_image_file(
                        reference,
                        minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                        require_visual_detail=True,
                    )
                except ValueError as exc:
                    raise RequestError(
                        "invalid_model_reference",
                        f"The generated image is not suitable for 3D input: {exc}",
                        409,
                    ) from None
                _assess_reference_advice(job)
            if not _model_gateway(provider).model_generation_available():
                raise RequestError("feature_unavailable", "Model generation is not configured.", 503)
            next_attempt = len(job.attempts) + 1 if provider == "hunyuan" else 1
            authorization = PaidTaskAuthorization.confirmed(f"{job.id}:model:{next_attempt}", provider)
            job.provider = provider
            job.prepared_prompt = prepared_prompt if job.source == "text" else ""
            job.face_limit = face_limit
            job.generation_profile = generation_profile
            job.geometry_quality = geometry_quality
            job.texture_quality = texture_quality
            job.output_format = output_format
            job.state = "queued"
            job.phase = "generating"
            job.message = "Generation queued."
            job.progress = 20
            _clear_job_artifact(job)
            _persist_job(job)
        try:
            _submit(job, _generate_job, prepared_prompt, False, authorization)
        except RequestError:
            with _JOBS_LOCK:
                job.state = "failed" if manual_retry else "awaiting_confirmation"
                job.phase = job.state
                job.message = "Review the prepared request before generation."
                job.progress = 15
                _persist_job(job)
            raise
        with _JOBS_LOCK:
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _retexture(self, reference_job_id: str) -> None:
        request = self._read_model_json()
        if set(request) != {"geometry_job_id"}:
            raise RequestError(
                "invalid_request",
                "Retexture requests require only geometry_job_id.",
                400,
            )
        geometry_job_id = request.get("geometry_job_id")
        if not isinstance(geometry_job_id, str):
            raise RequestError("invalid_request", "geometry_job_id must be a UUID string.", 400)
        try:
            parsed_geometry_id = uuid.UUID(geometry_job_id)
        except ValueError:
            raise RequestError("invalid_request", "geometry_job_id must be a UUID string.", 400) from None
        if str(parsed_geometry_id) != geometry_job_id.lower():
            raise RequestError("invalid_request", "geometry_job_id must be a canonical UUID string.", 400)
        geometry_job_id = geometry_job_id.lower()

        reference_job = self._get_job(reference_job_id)
        geometry_job = self._get_job(geometry_job_id)
        if geometry_job is None:
            geometry_job = _adopt_legacy_completed_job(geometry_job_id)
        if reference_job is None or geometry_job is None:
            raise RequestError("job_not_found", "The reference or geometry model job was not found.", 404)
        if geometry_job.provider != "tripo":
            raise RequestError("unsupported_provider_operation",
                               "Hunyuan history does not support preserved-geometry texturing.", 400)
        with _JOBS_LOCK:
            if reference_job.state not in {"awaiting_confirmation", "ready"}:
                raise RequestError(
                    "invalid_job_state",
                    "The current image reference is not ready for preserved-geometry texturing.",
                    409,
                )
            if geometry_job.state != "ready" or not _file_info(geometry_job.artifact_path)[0]:
                raise RequestError(
                    "geometry_not_ready",
                    "The selected historical model geometry is not ready.",
                    409,
                )
            reference = _model_generation_reference(reference_job)
            try:
                _validate_image_file(
                    reference,
                    minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                    require_visual_detail=True,
                )
            except ValueError as exc:
                raise RequestError("invalid_model_reference", str(exc), 409) from None
            source_attempt = next(
                (
                    attempt for attempt in reversed(geometry_job.attempts)
                    if isinstance(attempt.get("generation_task_id"), str)
                    and attempt.get("generation_task_id")
                    and attempt.get("status") in {"accepted", "running"}
                ),
                None,
            )
            source_task_id = str(source_attempt.get("generation_task_id", "")) if source_attempt else ""
            if not source_task_id:
                raise RequestError(
                    "geometry_source_unavailable",
                    "The selected historical model has no reusable provider geometry reference.",
                    409,
                )
            if not _MODEL_PROVIDER_GATEWAY.model_generation_available():
                raise RequestError("feature_unavailable", "Model texturing is not configured.", 503)

        child = _new_job(
            reference_job.source,
            reference_job.palette,
            reference_job.palette_roles,
            reference_job.style,
            reference_job.custom_style,
            reference_job.print_settings,
            palette_color_count=reference_job.palette_color_count,
        )
        try:
            child.user_prompt = reference_job.user_prompt
            child.prepared_prompt = reference_job.prepared_prompt
            child.face_limit = geometry_job.face_limit
            child.generation_profile = geometry_job.generation_profile
            child.geometry_quality = geometry_job.geometry_quality
            child.texture_quality = geometry_job.texture_quality
            child.output_format = geometry_job.output_format
            child.palette_recommendation = json.loads(json.dumps(reference_job.palette_recommendation))
            child.palette_recommendation_confirmed = reference_job.palette_recommendation_confirmed
            child.image_metrics = json.loads(json.dumps(reference_job.image_metrics))
            child.preview_content_type = reference_job.preview_content_type
            child.input_path = _copy_job_file(reference_job.input_path, child, "input")
            child.raw_preview_path = _copy_job_file(reference_job.raw_preview_path, child, "style-preview-raw")
            _use_unrestricted_creation(child)
            child.strict_preview_path = _copy_job_file(reference_job.strict_preview_path, child, "four-color-preview")
            child.preview_path = _copy_job_file(reference_job.preview_path, child, "clean-preview")
            child.model_reference_path = _copy_job_file(reference, child, "model-reference")
            child.geometry_reference_path = _copy_job_file(
                reference_job.geometry_reference_path, child, "geometry-reference"
            )
            child.heatmap_path = _copy_job_file(reference_job.heatmap_path, child, "unprintable-heatmap")
            child.metadata_path = _copy_job_file(reference_job.metadata_path, child, "metadata")
            child.background_mask_path = _copy_job_file(reference_job.background_mask_path, child, "mask-background")
            child.subject_mask_path = _copy_job_file(reference_job.subject_mask_path, child, "mask-subject")
            child.mask_paths = {
                role: copied
                for role, path in reference_job.mask_paths.items()
                if (copied := _copy_job_file(path, child, f"mask-{role}")) is not None
            }
            child.state = "queued"
            child.phase = "texturing"
            child.message = "Preserved-geometry texture generation queued."
            child.progress = 20
            _record_attempt(
                child,
                1,
                provider="tripo",
                provider_operation="model_texture",
                provider_request_id=f"{child.id}:texture:1",
                source_job_id=geometry_job.id,
                source_task_id=source_task_id,
                status="creating",
                error="",
            )
            with _JOBS_LOCK:
                _JOBS[child.id] = child
                _persist_job(child)
            authorization = PaidTaskAuthorization.confirmed_texture(f"{child.id}:texture:1")
            _submit(child, _retexture_job, geometry_job.id, source_task_id, False, authorization)
        except RequestError:
            with _JOBS_LOCK:
                _JOBS.pop(child.id, None)
            _remove_job_state(child)
            raise
        with _JOBS_LOCK:
            response = _public_job(child)
        self.send_json(202, {"job": response})

    def _stop(self, job_id: str) -> None:
        self._read_model_json()
        job = self._get_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with _JOBS_LOCK:
            if job.state in {"recommending_palette", "preprocessing", "queued", "running", "stopping"}:
                job.stop_event.set()
                job.state = "stopping"
                job.phase = "stopping"
                job.message = "Stopping model generation."
                _persist_job(job)
            elif job.state in {"awaiting_palette_confirmation", "awaiting_confirmation"}:
                job.stop_event.set()
                job.state = "stopped"
                job.phase = "stopped"
                job.message = "Model generation stopped."
                job.progress = 0
                _persist_job(job)
            elif job.state != "stopped":
                raise RequestError("invalid_job_state", "Job cannot be stopped in its current state.", 409)
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _recheck(self, job_id: str) -> None:
        self._read_model_json()
        job = self._get_job(job_id)
        if job is None:
            job = _adopt_legacy_completed_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with _JOBS_LOCK:
            if job.state in {"preprocessing", "queued", "running", "stopping"}:
                raise RequestError("invalid_job_state", "Model quality cannot be checked while the job is running.", 409)
            artifact = job.artifact_path
            artifact_format = job.artifact_format
        if artifact is None or artifact_format not in {"obj", "glb"}:
            raise RequestError("artifact_not_ready", "The model OBJ is not available for quality checking.", 409)
        try:
            resolved_artifact = artifact.resolve(strict=True)
            resolved_artifact.relative_to(job.directory.resolve(strict=True))
        except (OSError, ValueError):
            raise RequestError("artifact_not_ready", "The registered model OBJ is unavailable.", 409) from None
        quality = analyze_printable_obj(
            _analysis_artifact(resolved_artifact),
            allow_repairable_topology=True,
            target_palette=job.palette,
        )
        try:
            write_model_quality_report(quality, job.directory / MODEL_QUALITY_FILENAME)
        except ModelQualityError as exc:
            raise RequestError("quality_report_unavailable", str(exc), 503, True) from None
        with _JOBS_LOCK:
            if _JOBS.get(job_id) is not job:
                raise RequestError("job_not_found", "Model job is no longer available.", 404)
            _persist_job(job)
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _visual_review(self, job_id: str) -> None:
        request = self._read_model_json()
        force = request.get("force", False)
        if not isinstance(force, bool):
            raise RequestError("invalid_request", "force must be a boolean.", 400)
        job = self._get_job(job_id)
        if job is None:
            job = _adopt_legacy_completed_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with _JOBS_LOCK:
            if job.state in {"preprocessing", "queued", "running", "stopping"}:
                raise RequestError("job_busy", "Model generation is still running.", 409)
            artifact = job.artifact_path
        if artifact is None or job.artifact_format not in {"obj", "glb"}:
            raise RequestError("artifact_not_ready", "The model OBJ is not available for visual review.", 409)
        try:
            resolved_directory = job.directory.resolve(strict=True)
            resolved_artifact = artifact.resolve(strict=True)
            resolved_artifact.relative_to(resolved_directory)
        except (OSError, ValueError):
            raise RequestError("artifact_not_ready", "The registered model OBJ is unavailable.", 409) from None
        reference: Path | None = None
        if job.source == "image" and job.input_path is not None:
            try:
                candidate = job.input_path.resolve(strict=True)
                candidate.relative_to(resolved_directory)
                reference = candidate
            except (OSError, ValueError):
                reference = None
        review_model_visual_quality(
            _analysis_artifact(resolved_artifact),
            resolved_directory,
            description=job.user_prompt,
            style=job.style,
            reference_path=reference,
            force=force,
        )
        with _JOBS_LOCK:
            if _JOBS.get(job_id) is not job:
                raise RequestError("job_not_found", "Model job is no longer available.", 404)
            _persist_job(job)
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _download_job_file(self, job: Job, kind: str) -> None:
        with _JOBS_LOCK:
            fixed_paths = {
                "input": job.input_path,
                "raw-preview": job.raw_preview_path,
                "strict-preview": job.strict_preview_path,
                "preview": job.preview_path,
                # The UI labels this as the actual 3D-service input. Keep that
                # promise by serving the sculptural geometry reference when the
                # quality portrait strategy selects it.
                "model-reference": _geometry_generation_reference(job),
                "heatmap": job.heatmap_path,
                "metadata": job.metadata_path,
                "background-mask": job.background_mask_path,
                "subject-mask": job.subject_mask_path,
                "artifact": job.artifact_path,
                "color-intent": job.color_intent_path,
                "model-view-sheet": job.directory / "model-view-sheet.png",
            }
            path = job.mask_paths.get(kind[5:]) if kind.startswith("mask-") else fixed_paths.get(kind)
            ready, size = _file_info(path)
            if not ready or path is None:
                self._model_error(409, f"{kind}_not_ready", f"Model job {kind} is not ready.", True)
                return
            if kind == "color-intent" and size > MAX_COLOR_INTENT_BYTES:
                self._model_error(409, "color_intent_invalid", "The color-intent manifest exceeds its size limit.")
                return
            image_kinds = {
                "input", "raw-preview", "strict-preview", "preview", "model-reference", "heatmap",
                "background-mask", "subject-mask", "model-view-sheet",
            }
            content_type = _stored_image_type(path) if kind in image_kinds or kind.startswith("mask-") else \
                "application/json; charset=utf-8" if kind in {"metadata", "color-intent"} else {
                "obj": "model/obj",
                "glb": "model/gltf-binary",
                "3mf": "application/vnd.ms-package.3dmanufacturing-3dmodel+xml",
                "stl": "model/stl",
            }.get(job.artifact_format, "application/octet-stream")
            filename = f"orcaslicer-model-{job.id}.{job.artifact_format}" if kind == "artifact" else \
                f"orcaslicer-color-intent-{job.id}.json" if kind == "color-intent" else None
            try:
                stream = path.open("rb")
            except OSError:
                self._model_error(503, "file_unavailable", "Model job file is unavailable.", True)
                return
        with stream:
            self.send_bytes(stream, size, content_type, filename)


class LoopbackServer(ThreadingHTTPServer):
    daemon_threads = False


def main() -> int:
    host = HOST.strip().lower()
    if host not in _LOOPBACK_HOSTS:
        print("ORCASLICER_AI_SIDECAR_HOST must be 127.0.0.1, localhost, or ::1.", file=sys.stderr)
        return 2
    if _configured_session_token() is None:
        print("ORCASLICER_AI_SESSION_TOKEN must be a 64-character hexadecimal capability.", file=sys.stderr)
        return 2
    if _session_required() and not _configured_session_token():
        print("This AI Sidecar runtime requires an authenticated OrcaSlicer session.", file=sys.stderr)
        return 2
    parent_pid = _configured_parent_pid()
    if os.environ.get("ORCASLICER_AI_PARENT_PID", "").strip() and parent_pid is None:
        print("ORCASLICER_AI_PARENT_PID must identify the owning OrcaSlicer process.", file=sys.stderr)
        return 2
    if _session_required() and parent_pid is None:
        print("This AI Sidecar runtime requires an owning OrcaSlicer process.", file=sys.stderr)
        return 2
    parent_handle: int | None = None
    if parent_pid is not None:
        if os.name == "nt":
            parent_handle = _open_parent_process_handle(parent_pid)
            parent_alive = parent_handle is not None
        else:
            parent_alive = _parent_process_alive(parent_pid)
        if not parent_alive:
            print("The owning OrcaSlicer process is no longer running.", file=sys.stderr)
            return 2
    if host == "::1":
        LoopbackServer.address_family = socket.AF_INET6
    image_provider = image_provider_status()
    diagnostic_event(
        "sidecar.starting",
        sidecar_version=SIDECAR_VERSION,
        instance_id=SIDECAR_INSTANCE_ID,
        session_protected=bool(_configured_session_token()),
        build=_safe_runtime_identity(),
        endpoint=safe_endpoint(f"http://{HOST}:{PORT}"),
        python_version=sys.version.split()[0],
        openssl_version=ssl.OPENSSL_VERSION,
        output_directory=str(_model_output_root()),
        openai_configured=bool(os.environ.get("OPENAI_API_KEY", "")),
        image_provider_configured=image_provider["available"],
        image_provider_source=image_provider["source"],
        tripo_configured=bool(os.environ.get("TRIPO_API_KEY", "")),
        configuration_mode="internal_locked"
        if os.environ.get("ORCASLICER_AI_CONFIG_MODE") == "internal_locked"
        else "external",
        openai_endpoint=safe_endpoint(os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")),
        image_provider_endpoint=image_provider["base_url"],
        tripo_endpoint=safe_endpoint(os.environ.get("TRIPO_API_BASE", "https://openapi.tripo3d.com/v3")),
        network=_runtime_network_metadata(),
    )
    # Restore local state first, but do not touch an existing remote task until
    # the owning Orca process has been rechecked and the listener is ready.
    try:
        restored_jobs = _restore_jobs(resume_jobs=False)
    except Exception:
        _close_parent_process_handle(parent_handle)
        raise
    try:
        server = LoopbackServer((HOST, PORT), Handler)
    except OSError as exc:
        _close_parent_process_handle(parent_handle)
        diagnostic_event(
            "sidecar.bind.failed",
            level="ERROR",
            endpoint=safe_endpoint(f"http://{HOST}:{PORT}"),
            exception_chain=exception_details(exc),
        )
        raise
    if parent_pid is not None:
        if os.name == "nt":
            parent_alive = parent_handle is not None and _parent_process_handle_alive(parent_handle)
        else:
            parent_alive = _parent_process_alive(parent_pid)
        if not parent_alive:
            _close_parent_process_handle(parent_handle)
            server.server_close()
            diagnostic_event("sidecar.parent.unavailable", level="ERROR", parent_pid=parent_pid)
            return 2
        threading.Thread(
            target=_monitor_parent,
            args=(server, parent_pid, parent_handle),
            name="orca-parent-monitor",
            daemon=True,
        ).start()
        parent_handle = None  # The monitor thread owns and closes this handle.
    _resume_restored_jobs(restored_jobs)
    diagnostic_event("sidecar.listening", endpoint=safe_endpoint(f"http://{HOST}:{PORT}"))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        _close_parent_process_handle(parent_handle)
        server.server_close()
        shutdown_sidecar()
        diagnostic_event("sidecar.stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

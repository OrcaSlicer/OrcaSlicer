#!/usr/bin/env python3
import json
import os
import re
import struct
import threading
import time
import uuid
import zlib
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

HOST = os.environ.get("ORCASLICER_AI_SIDECAR_HOST", "127.0.0.1")
PORT = int(os.environ.get("ORCASLICER_AI_SIDECAR_PORT", "18764"))
MAX_JSON_BYTES = 1024 * 1024
MAX_IMAGE_BYTES = 20 * 1024 * 1024
MAX_MULTIPART_BYTES = MAX_IMAGE_BYTES + 256 * 1024
MAX_PROMPT_BYTES = 64 * 1024
MIN_PALETTE_COLORS = 1
MAX_PALETTE_COLORS = 6
DEFAULT_PALETTE_COLORS = 4
MODEL_FACE_LIMITS = (100000, 300000, 500000, 1000000, 2000000)
DEFAULT_MODEL_FACE_LIMIT = 300000
GENERATION_PROFILES = ("quality", "performance")
DEFAULT_GENERATION_PROFILE = "quality"
GENERATION_PROFILE_FACE_LIMITS = {"quality": 1000000, "performance": 300000}
MOCK_PALETTE_RECOMMENDATION = {
    "summary": "暖色主体配合深色结构、浅色层次和冷色点缀",
    "colors": [
        {"hex": "#D96B43", "name": "陶土橙", "role": "primary", "usage": "主体区域", "reason": "形成稳定的视觉中心"},
        {"hex": "#2B2422", "name": "深棕", "role": "structure", "usage": "轮廓与承力结构", "reason": "增强边界可读性"},
        {"hex": "#F2D7B5", "name": "暖白", "role": "light", "usage": "面部与浅色区域", "reason": "保持清晰明暗层次"},
        {"hex": "#2F6B5F", "name": "墨绿", "role": "accent", "usage": "配件与底座点缀", "reason": "提供冷暖对比"},
        {"hex": "#3267A8", "name": "钴蓝", "role": "secondary", "usage": "次要大色区", "reason": "补充清晰分区"},
        {"hex": "#9B3F77", "name": "莓紫", "role": "detail", "usage": "可打印识别细节", "reason": "强化主体特征"},
    ],
}


def _png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def _mock_png():
    width = height = 128
    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            body = 28 < x < 100 and 38 < y < 106
            head = 44 < x < 84 and 14 < y < 54
            if body or head:
                row.extend((66, 135, 192, 255))
            else:
                row.extend((242, 246, 250, 255))
        rows.append(bytes(row))
    raw = b"".join(rows)
    return (b"\x89PNG\r\n\x1a\n" +
            _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            _png_chunk(b"IDAT", zlib.compress(raw)) +
            _png_chunk(b"IEND", b""))


TINY_PNG = _mock_png()
TINY_OBJ = b"""# orcaslicer mock vertex-color tetrahedron
v 0 0 0 1 0 0
v 10 0 0 0 1 0
v 0 10 0 0 0 1
v 0 0 10 1 1 0
f 1 3 2
f 1 2 4
f 1 4 3
f 2 3 4
"""


def _load_mock_obj():
    configured_path = os.environ.get("ORCASLICER_AI_MOCK_MODEL_PATH", os.environ.get("ORCASLICER_AI_MOCK_OBJ_PATH", "")).strip()
    if not configured_path:
        return TINY_OBJ
    try:
        with open(configured_path, "rb") as stream:
            payload = stream.read()
    except OSError as exc:
        raise RuntimeError("ORCASLICER_AI_MOCK_OBJ_PATH could not be read: %s" % exc) from exc
    if not payload:
        raise RuntimeError("ORCASLICER_AI_MOCK_OBJ_PATH points to an empty file.")
    return payload


MOCK_OBJ = _load_mock_obj()
MOCK_FORMAT = "glb" if MOCK_OBJ.startswith(b"glTF") else "obj"

_jobs = {}
_jobs_lock = threading.Lock()


def first_config_value(request, scope, key, default):
    return str(request.get("config", {}).get(scope, {}).get(key, default))


def proposal_for_message(request):
    raw_message = str(request.get("user_message") or request.get("message") or "")
    message = raw_message.lower()
    changes = []
    notes = []

    if any(word in message for word in ["strong", "strength", "坚固", "强度", "结实"]):
        changes.extend([
            {"scope": "print", "key": "wall_loops", "new_value": "4", "reason": "Increase perimeters for stronger shells."},
            {"scope": "print", "key": "sparse_infill_density", "new_value": "25%", "reason": "Use moderate infill for better internal strength."},
        ])
        notes.append("已按强度优先给出外墙和填充建议。")

    if any(word in message for word in ["quality", "surface", "detail", "精细", "质量", "表面"]):
        changes.extend([
            {"scope": "print", "key": "layer_height", "new_value": "0.16", "reason": "Use a smaller layer height for finer detail."},
            {"scope": "print", "key": "outer_wall_speed", "new_value": "50%", "reason": "Slow visible walls for better surface quality."},
            {"scope": "print", "key": "top_surface_speed", "new_value": "50%", "reason": "Slow top surfaces to reduce visible artifacts."},
        ])
        notes.append("已按表面质量优先给出层高和可见面速度建议。")

    if any(word in message for word in ["fast", "speed", "quick", "快速", "速度", "加速"]):
        changes.extend([
            {"scope": "print", "key": "sparse_infill_speed", "new_value": "120%", "reason": "Increase infill speed before changing visible walls."},
            {"scope": "filament", "key": "filament_max_volumetric_speed", "new_value": "15", "reason": "Raise volumetric limit only within the filament preset range."},
        ])
        notes.append("已按速度优先提高内部填充速度，并保留外观面速度。")

    if any(word in message for word in ["adhesion", "warp", "warping", "first layer", "翘边", "粘附", "首层"]):
        changes.extend([
            {"scope": "print", "key": "brim_width", "new_value": "5", "reason": "Add brim width to improve bed adhesion."},
            {"scope": "print", "key": "initial_layer_speed", "new_value": "30", "reason": "Slow the first layer for better adhesion."},
            {"scope": "print", "key": "slow_down_layers", "new_value": "3", "reason": "Keep early layers slower while adhesion stabilizes."},
        ])
        notes.append("已按首层和附着优先给出 brim 与首层速度建议。")

    if any(word in message for word in ["support", "overhang", "悬垂", "支撑"]):
        changes.extend([
            {"scope": "print", "key": "enable_support", "new_value": "1", "reason": "Enable support for likely overhangs."},
            {"scope": "print", "key": "support_top_z_distance", "new_value": "0.2", "reason": "Balance support removability and underside quality."},
            {"scope": "print", "key": "support_interface_top_layers", "new_value": "2", "reason": "Add interface layers for cleaner supported surfaces."},
        ])
        notes.append("已按支撑质量给出保守支撑参数建议。")

    if not changes:
        current_wall_loops = first_config_value(request, "print", "wall_loops", "2")
        changes.append({
            "scope": "print",
            "key": "wall_loops",
            "new_value": "3" if current_wall_loops != "3" else "4",
            "reason": "Default mock suggestion: improve part robustness with one more perimeter."
        })
        notes.append("这是 mock sidecar 默认建议；真实模型接入后会按你的完整描述生成。")

    unique = []
    seen = set()
    for change in changes:
        key = (change["scope"], change["key"])
        if key not in seen:
            unique.append(change)
            seen.add(key)
        if len(unique) >= 6:
            break

    return {
        "request_id": request.get("request_id", ""),
        "assistant_text": "\n".join(notes),
        "proposal": {"changes": unique},
    }


def empty_artifact():
    return {"ready": False, "format": "", "filename": "", "size_bytes": 0}


def normalize_palette(value):
    if not isinstance(value, list) or len(value) > MAX_PALETTE_COLORS:
        raise ValueError("palette must contain between 0 and %d colors" % MAX_PALETTE_COLORS)
    normalized = []
    for color in value:
        if not isinstance(color, str) or re.fullmatch(r"#[0-9A-Fa-f]{6}", color) is None:
            raise ValueError("palette colors must use #RRGGBB format")
        color = color.upper()
        if color not in normalized:
            normalized.append(color)
    return normalized


def normalize_palette_color_count(value):
    if value is None:
        return DEFAULT_PALETTE_COLORS
    if isinstance(value, bool):
        parsed = -1
    elif isinstance(value, int):
        parsed = value
    elif isinstance(value, str) and re.fullmatch(r"[0-9]+", value.strip()):
        parsed = int(value.strip())
    else:
        parsed = -1
    if not MIN_PALETTE_COLORS <= parsed <= MAX_PALETTE_COLORS:
        raise ValueError(
            "palette_color_count must be between %d and %d" % (MIN_PALETTE_COLORS, MAX_PALETTE_COLORS)
        )
    return parsed


def mock_palette_recommendation(color_count):
    result = json.loads(json.dumps(MOCK_PALETTE_RECOMMENDATION))
    result["colors"] = result["colors"][:normalize_palette_color_count(color_count)]
    return result


def normalize_face_limit(value):
    if isinstance(value, bool) or not isinstance(value, int) or value not in MODEL_FACE_LIMITS:
        raise ValueError("face_limit must be 100000, 300000, 500000, 1000000, or 2000000 triangles")
    return value


def normalize_generation_profile(value):
    if not isinstance(value, str) or value not in GENERATION_PROFILES:
        raise ValueError("generation_profile must be quality or performance")
    return value


def multipart_palette(value):
    if not isinstance(value, str):
        raise ValueError("palette is required")
    try:
        return normalize_palette(json.loads(value))
    except json.JSONDecodeError as exc:
        raise ValueError("palette must be a JSON color array") from exc


def new_job(source, prepared_prompt, palette, palette_color_count=None):
    job_id = str(uuid.uuid4())
    job = {
        "id": job_id,
        "source": source,
        "state": "preprocessing",
        "phase": "preprocessing",
        "message": "Preparing model generation request.",
        "progress": 5,
        "prepared_prompt": "",
        "user_prompt": "",
        "style": "sculpture",
        "custom_style": "",
        "face_limit": DEFAULT_MODEL_FACE_LIMIT,
        "generation_profile": DEFAULT_GENERATION_PROFILE,
        "geometry_quality": None,
        "texture_quality": "standard",
        "output_format": "glb",
        "palette": list(palette),
        "palette_roles": {},
        "palette_color_count": normalize_palette_color_count(
            palette_color_count if palette_color_count is not None else (len(palette) or None)
        ),
        "palette_recommendation": {},
        "palette_recommendation_confirmed": False,
        "_palette": palette,
        "preview": {"ready": False, "content_type": "", "size_bytes": 0},
        "artifact": empty_artifact(),
        "_next_prompt": prepared_prompt,
        "_stage_started": time.monotonic(),
        "_status_calls": 0,
    }
    _jobs[job_id] = job
    return job


def advance_job(job, status_call=False):
    if status_call:
        job["_status_calls"] += 1
    elapsed = time.monotonic() - job["_stage_started"]

    if job["state"] == "preprocessing" and (elapsed >= 0.10 or job["_status_calls"] >= 1):
        job.update({
            "state": "awaiting_confirmation",
            "phase": "awaiting_confirmation",
            "message": "Review the prepared image before generation.",
            "progress": 15,
            "prepared_prompt": job["_next_prompt"],
        })
        job["preview"] = {"ready": True, "content_type": "image/png", "size_bytes": len(TINY_PNG)}
        return

    if job["state"] not in ("queued", "running"):
        return

    elapsed_step = min(4, int(elapsed / 0.15))
    step = max(elapsed_step, job["_status_calls"])
    if step <= 0:
        job.update(state="queued", phase="generating", message="Generation queued.", progress=20)
    elif step == 1:
        job.update(state="running", phase="generating", message="Generating model geometry.", progress=40)
    elif step == 2:
        job.update(state="running", phase="generating", message="Finishing generated geometry.", progress=70)
    elif step == 3:
        job.update(state="running", phase="downloading_artifact", message="Preparing the generated artifact.", progress=90)
    else:
        job.update(
            state="ready",
            phase="ready",
            message="Generated model is ready.",
            progress=100,
            artifact={
                "ready": True,
                "format": MOCK_FORMAT,
                "color_encoding": "textures_or_vertex_colors" if MOCK_FORMAT == "glb" else "vertex_colors",
                "filename": "orcaslicer-model-%s.%s" % (job["id"], MOCK_FORMAT),
                "size_bytes": len(MOCK_OBJ),
            },
        )


def public_job(job):
    return {
        "id": job["id"],
        "source": job["source"],
        "state": job["state"],
        "phase": job["phase"],
        "message": job["message"],
        "progress": job["progress"],
        "prepared_prompt": job["prepared_prompt"] if job["source"] == "text" else "",
        "user_prompt": job["user_prompt"],
        "style": job["style"],
        "custom_style": job["custom_style"],
        "face_limit": job["face_limit"],
        "generation_profile": job["generation_profile"],
        "geometry_quality": job.get("geometry_quality"),
        "texture_quality": job.get("texture_quality", "standard"),
        "output_format": job.get("output_format", "glb"),
        "palette": list(job["palette"]),
        "palette_roles": dict(job["palette_roles"]),
        "palette_color_count": job["palette_color_count"],
        "palette_recommendation": dict(job["palette_recommendation"]),
        "palette_recommendation_confirmed": job["palette_recommendation_confirmed"],
        "preview": dict(job["preview"]),
        "artifact": dict(job["artifact"]),
    }


def text_field(value, name, allow_empty=False):
    if not isinstance(value, str):
        raise ValueError("%s must be a string" % name)
    value = value.strip()
    if not allow_empty and not value:
        raise ValueError("%s is required" % name)
    if len(value.encode("utf-8")) > MAX_PROMPT_BYTES:
        raise ValueError("%s is too large" % name)
    return value


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        path = urlsplit(self.path).path
        if path in ("/v1/orcaslicer/config-proposal", "/v1/orcaslicer/chat"):
            self.handle_existing_post(path)
            return

        if path == "/v1/orcaslicer/journey-events":
            if not self.require_native_client():
                return
            try:
                request = self.read_json()
                if set(request) - {"event", "job_id"} or not isinstance(request.get("event"), str):
                    raise RequestError("invalid_journey_event", "Only event and job_id are accepted.", 400)
            except RequestError as exc:
                self.model_error(exc.code, exc.message, exc.status)
                return
            self.send_json({"event": request}, 201)
            return

        if path.startswith("/v1/orcaslicer/model-jobs"):
            if not self.require_native_client():
                return
            if path == "/v1/orcaslicer/model-jobs/text":
                self.create_text_job()
                return
            if path == "/v1/orcaslicer/model-jobs/image":
                self.create_image_job()
                return
            if path == "/v1/orcaslicer/model-jobs/recommend-text-palette":
                self.create_text_palette_recommendation()
                return
            if path == "/v1/orcaslicer/model-jobs/recommend-image-palette":
                self.create_image_palette_recommendation()
                return
            job_id, action = self.job_route(path)
            if job_id and action in ("generate", "stop", "confirm-palette"):
                if action == "generate":
                    self.generate_job(job_id)
                elif action == "confirm-palette":
                    self.confirm_palette(job_id)
                else:
                    self.stop_job(job_id)
                return
            self.model_error("not_found", "Model job route not found.", 404)
            return

        self.send_json({"error": "not found"}, 404)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/health":
            self.send_json({
                "ok": True,
                "protocol_version": 2,
                "sidecar_version": "orcaslicer-ai-sidecar-mock-v1",
                "capabilities": {
                    "config_proposal": {"available": True},
                    "model_generation": {
                        "available": True,
                        "sources": ["text", "image"],
                        "styles": [
                            "sculpture", "realistic", "portrait_sketch", "cartoon", "low_poly",
                            "relief", "ink_relief", "diorama", "custom",
                        ],
                        "style_recommendation": {"available": True, "local_only": True},
                        "artifact_formats": ["glb", "obj"],
                        "face_limits": [300000, 1000000, 2000000],
                        "geometry_qualities": ["standard", "detailed"],
                        "texture_qualities": ["standard", "detailed", "extreme"],
                        "output_formats": ["glb", "obj"],
                        "default_face_limit": GENERATION_PROFILE_FACE_LIMITS[DEFAULT_GENERATION_PROFILE],
                        "generation_profiles": list(GENERATION_PROFILES),
                        "default_generation_profile": DEFAULT_GENERATION_PROFILE,
                        "source_availability": {"text": True, "image": True},
                        "image_provider": {
                            "available": True,
                            "source": "mock",
                            "base_url": "https://mock.example/v1",
                            "endpoints": {
                                "generations": "https://mock.example/v1/images/generations",
                                "edits": "https://mock.example/v1/images/edits",
                            },
                            "automatic_retry": False,
                            "max_billable_requests_per_action": 1,
                        },
                        "palette_recommendation": {
                            "available": True,
                            "min_colors": MIN_PALETTE_COLORS,
                            "max_colors": MAX_PALETTE_COLORS,
                            "default_colors": DEFAULT_PALETTE_COLORS,
                        },
                        "printable_image_pipeline": {
                            "available": True,
                            "print_modes": ["solid_regions"],
                            "color_distances": ["ciede2000", "delta_e76"],
                            "outputs": [
                                "raw_preview", "strict_preview", "clean_preview", "model_reference",
                                "heatmap", "masks", "metadata",
                            ],
                        },
                    },
                },
            }, 200)
            return

        if path.startswith("/v1/orcaslicer/model-jobs"):
            if not self.require_native_client():
                return
            if path == "/v1/orcaslicer/model-jobs/latest":
                with _jobs_lock:
                    job = next(reversed(_jobs.values()), None) if _jobs else None
                    response = public_job(job) if job is not None else None
                self.send_json({"job": response}, 200)
                return
            job_id, action = self.job_route(path)
            if not job_id:
                self.model_error("not_found", "Model job route not found.", 404)
                return
            if action == "status":
                self.get_job_status(job_id)
            elif action in ("preview", "artifact"):
                self.download_job_file(job_id, action)
            else:
                self.model_error("not_found", "Model job route not found.", 404)
            return

        self.send_json({"error": "not found"}, 404)

    def do_DELETE(self):
        path = urlsplit(self.path).path
        if not path.startswith("/v1/orcaslicer/model-jobs"):
            self.send_json({"error": "not found"}, 404)
            return
        if not self.require_native_client():
            return
        job_id, action = self.job_route(path)
        if not job_id or action != "status":
            self.model_error("not_found", "Model job route not found.", 404)
            return
        with _jobs_lock:
            if _jobs.pop(job_id, None) is None:
                self.model_error("job_not_found", "Model job not found.", 404)
                return
        self.send_response(204)
        self.end_headers()

    def handle_existing_post(self, path):
        try:
            request = self.read_json()
        except Exception as exc:
            self.send_json({"error": "invalid request: %s" % exc}, 400)
            return

        response = proposal_for_message(request)
        if path == "/v1/orcaslicer/chat":
            response = {
                "reply": response["assistant_text"],
                "proposal": response["proposal"],
                "sidecar_version": "mock-config-proposal-v1",
            }
        self.send_json(response, 200)

    def create_text_job(self):
        try:
            request = self.read_json()
            request_id = text_field(request.get("request_id"), "request_id")
            prompt = text_field(request.get("prompt"), "prompt")
            palette = normalize_palette(request.get("palette"))
        except Exception as exc:
            self.model_error("invalid_request", str(exc), 400)
            return

        prepared = "Create a printable 3D model from this description: %s" % prompt
        with _jobs_lock:
            job = new_job("text", prepared, palette)
            job.update(
                user_prompt=prompt,
                style=request.get("style", "sculpture"),
                custom_style=request.get("custom_style", ""),
            )
            response = public_job(job)
        self.send_json({"job": response}, 202)

    def create_image_job(self):
        try:
            fields, image, image_type = self.read_image_multipart()
            request_id = text_field(fields.get("request_id"), "request_id")
            instruction = text_field(fields.get("instruction"), "instruction")
            palette = multipart_palette(fields.get("palette"))
            if not image or len(image) > MAX_IMAGE_BYTES:
                raise RequestError("image_too_large", "Image must be no larger than 20 MB.", 413)
            detected = self.detect_image_type(image)
            if detected is None:
                raise RequestError("unsupported_image", "Image must be PNG or JPEG.", 415)
            if image_type not in ("application/octet-stream", detected):
                raise RequestError("unsupported_image", "Image content type does not match its data.", 415)
        except RequestError as exc:
            self.model_error(exc.code, exc.message, exc.status)
            return
        except Exception as exc:
            self.model_error("invalid_request", str(exc), 400)
            return

        prepared = "Create a printable 3D model based on the uploaded image. Instruction: %s" % instruction
        with _jobs_lock:
            job = new_job("image", prepared, palette)
            job.update(
                user_prompt=instruction,
                style=fields.get("style", "sculpture"),
                custom_style=fields.get("custom_style", ""),
            )
            response = public_job(job)
        self.send_json({"job": response}, 202)

    def create_text_palette_recommendation(self):
        try:
            request = self.read_json()
            text_field(request.get("request_id"), "request_id")
            prompt = text_field(request.get("prompt"), "prompt")
            palette_color_count = normalize_palette_color_count(request.get("palette_color_count"))
        except Exception as exc:
            self.model_error("invalid_request", str(exc), 400)
            return
        with _jobs_lock:
            job = new_job(
                "text",
                "Create a printable 3D model from this description: %s" % prompt,
                [],
                palette_color_count,
            )
            job.update(
                state="awaiting_palette_confirmation",
                phase="awaiting_palette_confirmation",
                message="Review and confirm the recommended design colors.",
                progress=10,
                user_prompt=prompt,
                style=request.get("style", "sculpture"),
                custom_style=request.get("custom_style", ""),
                palette_recommendation=mock_palette_recommendation(palette_color_count),
            )
            response = public_job(job)
        self.send_json({"job": response}, 202)

    def create_image_palette_recommendation(self):
        try:
            fields, image, image_type = self.read_image_multipart()
            text_field(fields.get("request_id"), "request_id")
            instruction = text_field(fields.get("instruction"), "instruction")
            palette_color_count = normalize_palette_color_count(fields.get("palette_color_count"))
            detected = self.detect_image_type(image)
            if detected is None or image_type not in ("application/octet-stream", detected):
                raise RequestError("unsupported_image", "Image must be PNG or JPEG.", 415)
        except RequestError as exc:
            self.model_error(exc.code, exc.message, exc.status)
            return
        except Exception as exc:
            self.model_error("invalid_request", str(exc), 400)
            return
        with _jobs_lock:
            job = new_job(
                "image",
                "Create a printable 3D model based on the uploaded image. Instruction: %s" % instruction,
                [],
                palette_color_count,
            )
            job.update(
                state="awaiting_palette_confirmation",
                phase="awaiting_palette_confirmation",
                message="Review and confirm the recommended design colors.",
                progress=10,
                user_prompt=instruction,
                style=fields.get("style", "sculpture"),
                custom_style=fields.get("custom_style", ""),
                palette_recommendation=mock_palette_recommendation(palette_color_count),
            )
            response = public_job(job)
        self.send_json({"job": response}, 202)

    def confirm_palette(self, job_id):
        try:
            request = self.read_json()
            palette = normalize_palette(request.get("palette"))
            if not palette:
                raise ValueError("at least one confirmed color is required")
            palette_roles = request.get("palette_roles", {})
            if not isinstance(palette_roles, dict):
                raise ValueError("palette_roles must be an object")
        except Exception as exc:
            self.model_error("invalid_request", str(exc), 400)
            return
        with _jobs_lock:
            job = _jobs.get(job_id)
            if job is None:
                self.model_error("job_not_found", "Model job not found.", 404)
                return
            if job["state"] != "awaiting_palette_confirmation":
                self.model_error("invalid_job_state", "Job is not awaiting palette confirmation.", 409)
                return
            job.update(
                state="preprocessing",
                phase="preprocessing",
                message="Confirmed colors are being applied to the printable preview.",
                progress=10,
                palette=list(palette),
                palette_roles=dict(palette_roles),
                palette_recommendation_confirmed=True,
                _palette=list(palette),
                _stage_started=time.monotonic(),
                _status_calls=0,
            )
            response = public_job(job)
        self.send_json({"job": response}, 200)

    def get_job_status(self, job_id):
        with _jobs_lock:
            job = _jobs.get(job_id)
            if job is None:
                self.model_error("job_not_found", "Model job not found.", 404)
                return
            advance_job(job, status_call=True)
            response = public_job(job)
        self.send_json({"job": response}, 200)

    def generate_job(self, job_id):
        try:
            request = self.read_json()
            prepared_prompt = text_field(request.get("prepared_prompt", ""), "prepared_prompt", allow_empty=True)
            palette = normalize_palette(request.get("palette"))
            if "generation_profile" in request and "face_limit" not in request:
                generation_profile = normalize_generation_profile(request.get("generation_profile"))
                face_limit = GENERATION_PROFILE_FACE_LIMITS[generation_profile]
            else:
                face_limit = normalize_face_limit(request.get("face_limit", DEFAULT_MODEL_FACE_LIMIT))
                generation_profile = "quality" if face_limit >= 500000 else "performance"
            geometry = request.get("geometry_quality")
            texture = request.get("texture_quality", "standard")
            output = request.get("output_format", "glb")
            if geometry not in (None, "standard", "detailed") or texture not in ("standard", "detailed", "extreme") or output not in ("obj", "glb"):
                raise ValueError("Invalid generation options")
            if geometry == "standard" and face_limit == 2000000:
                raise ValueError("The 2-million-face target requires detailed geometry")
        except Exception as exc:
            self.model_error("invalid_request", str(exc), 400)
            return

        with _jobs_lock:
            job = _jobs.get(job_id)
            if job is None:
                self.model_error("job_not_found", "Model job not found.", 404)
                return
            advance_job(job)
            if job["state"] != "awaiting_confirmation":
                self.model_error("invalid_job_state", "Job is not awaiting confirmation.", 409)
                return
            if palette != job["_palette"]:
                self.model_error("palette_changed", "The filament palette changed after preview.", 409)
                return
            job.update(
                state="queued",
                phase="generating",
                message="Generation queued.",
                progress=20,
                prepared_prompt=prepared_prompt,
                face_limit=face_limit,
                generation_profile=generation_profile,
                geometry_quality=geometry,
                texture_quality=texture,
                output_format=output,
                artifact=empty_artifact(),
                _stage_started=time.monotonic(),
                _status_calls=0,
            )
            response = public_job(job)
        self.send_json({"job": response}, 200)

    def stop_job(self, job_id):
        try:
            self.read_json()
        except Exception as exc:
            self.model_error("invalid_request", str(exc), 400)
            return
        with _jobs_lock:
            job = _jobs.get(job_id)
            if job is None:
                self.model_error("job_not_found", "Model job not found.", 404)
                return
            job.update(
                state="stopped",
                phase="stopped",
                message="Model generation stopped.",
                progress=0,
                artifact=empty_artifact(),
            )
            response = public_job(job)
        self.send_json({"job": response}, 200)

    def download_job_file(self, job_id, kind):
        with _jobs_lock:
            job = _jobs.get(job_id)
            if job is None:
                self.model_error("job_not_found", "Model job not found.", 404)
                return
            advance_job(job)
            ready = job[kind]["ready"]
            filename = job["artifact"]["filename"] if kind == "artifact" else "model-preview.png"
        if not ready:
            self.model_error("%s_not_ready" % kind, "Model job %s is not ready." % kind, 409, True)
            return
        if kind == "preview":
            self.send_bytes(TINY_PNG, "image/png", filename)
        else:
            self.send_bytes(MOCK_OBJ, "model/gltf-binary" if MOCK_FORMAT == "glb" else "model/obj", filename)

    def require_native_client(self):
        if self.headers.get("X-OrcaSlicer-Client") != "native":
            self.model_error("client_required", "X-OrcaSlicer-Client must be native.", 403)
            return False
        return True

    def read_body(self, limit):
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            raise RequestError("content_length_required", "Content-Length is required.", 411)
        try:
            length = int(raw_length)
        except ValueError:
            raise RequestError("invalid_request", "Invalid Content-Length.", 400)
        if length < 0:
            raise RequestError("invalid_request", "Invalid Content-Length.", 400)
        if length > limit:
            raise RequestError("request_too_large", "Request body is too large.", 413)
        body = self.rfile.read(length)
        if len(body) != length:
            raise RequestError("invalid_request", "Request body is incomplete.", 400)
        return body

    def read_json(self):
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type and content_type != "application/json":
            raise RequestError("unsupported_media_type", "Content-Type must be application/json.", 415)
        body = self.read_body(MAX_JSON_BYTES)
        try:
            value = json.loads(body.decode("utf-8")) if body else {}
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise RequestError("invalid_json", "Malformed JSON: %s" % exc, 400)
        if not isinstance(value, dict):
            raise RequestError("invalid_request", "JSON body must be an object.", 400)
        return value

    def read_image_multipart(self):
        content_type = self.headers.get("Content-Type", "")
        if not content_type.lower().startswith("multipart/form-data;"):
            raise RequestError("unsupported_media_type", "Content-Type must be multipart/form-data.", 415)
        body = self.read_body(MAX_MULTIPART_BYTES)
        header = b"Content-Type: " + content_type.encode("latin-1") + b"\r\nMIME-Version: 1.0\r\n\r\n"
        message = BytesParser(policy=policy.default).parsebytes(header + body)
        if not message.is_multipart():
            raise RequestError("invalid_multipart", "Malformed multipart request.", 400)

        fields = {}
        image = None
        image_type = "application/octet-stream"
        for part in message.iter_parts():
            if part.is_multipart():
                raise RequestError("invalid_multipart", "Nested multipart data is not supported.", 400)
            name = part.get_param("name", header="content-disposition")
            if name not in (
                "request_id", "instruction", "palette", "palette_roles", "palette_color_count",
                "style", "custom_style", "print", "image",
            ) or name in fields or (name == "image" and image is not None):
                raise RequestError("invalid_multipart", "Unexpected or duplicate multipart field.", 400)
            payload = part.get_payload(decode=True) or b""
            if name == "image":
                image = payload
                image_type = part.get_content_type().lower()
            else:
                if len(payload) > MAX_PROMPT_BYTES:
                    raise RequestError("invalid_request", "%s is too large." % name, 400)
                try:
                    fields[name] = payload.decode(part.get_content_charset() or "utf-8")
                except (LookupError, UnicodeDecodeError):
                    raise RequestError("invalid_request", "%s must be UTF-8 text." % name, 400)
        if image is None:
            raise RequestError("invalid_request", "image is required.", 400)
        return fields, image, image_type

    @staticmethod
    def detect_image_type(data):
        if data.startswith(b"\x89PNG\r\n\x1a\n"):
            return "image/png"
        if data.startswith(b"\xff\xd8\xff"):
            return "image/jpeg"
        return None

    @staticmethod
    def job_route(path):
        prefix = "/v1/orcaslicer/model-jobs/"
        if not path.startswith(prefix):
            return None, None
        parts = path[len(prefix):].split("/")
        if len(parts) == 1 and parts[0]:
            return parts[0], "status"
        if len(parts) == 2 and parts[0] and parts[1] in ("preview", "generate", "stop", "artifact", "confirm-palette"):
            return parts[0], parts[1]
        return None, None

    def model_error(self, code, message, status, retryable=False):
        self.send_json({"error": {"code": code, "message": message, "retryable": retryable}}, status)

    def send_json(self, data, status):
        payload = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def send_bytes(self, payload, content_type, filename):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Disposition", 'attachment; filename="%s"' % filename)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        print("%s - %s" % (self.address_string(), fmt % args))


class RequestError(Exception):
    def __init__(self, code, message, status):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


if __name__ == "__main__":
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print("OrcaSlicer mock AI sidecar listening on http://%s:%s" % (HOST, PORT))
    print("Config endpoints: POST /v1/orcaslicer/config-proposal, POST /v1/orcaslicer/chat")
    print("Model jobs: /v1/orcaslicer/model-jobs/*")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

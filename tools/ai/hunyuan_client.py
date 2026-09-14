"""Tencent TokenHub HY-3D jobs; no automatic paid submission retries.

API contract: https://cloud.tencent.com/document/product/1823/137181
"""
from __future__ import annotations

import base64
import http.client
import io
import json
import math
import os
from pathlib import Path
import re
import tempfile
import time
from typing import Any, Callable, Mapping
import urllib.error
import urllib.parse
import urllib.request

try:
    from .network_policy import build_opener as build_network_opener
except ImportError:
    from network_policy import build_opener as build_network_opener

_BASE_URL = "https://tokenhub.tencentmaas.com/v1/api/3d"
_MODEL = "hy-3d-3.1"
_MAX_IMAGE_BYTES = 6 * 1024 * 1024
_MAX_JSON_BYTES = 4 * 1024 * 1024
_REQUEST_TIMEOUT = 60.0
_DEFAULT_DEADLINE = 900.0
_POLL_INTERVAL = 2.0


class HunyuanError(RuntimeError):
    def __init__(self, message: str, *, code: str = "provider_failed", category: str = "provider",
                 retryable: bool = False, ambiguous: bool = False) -> None:
        super().__init__(message)
        self.code = code
        self.category = category
        self.retryable = retryable
        self.ambiguous = ambiguous


def _invalid(message: str) -> HunyuanError:
    return HunyuanError(message, code="invalid_request", category="validation")


def _api_key() -> str:
    key = os.environ.get("HY3D_API", "").strip()
    if key or os.name != "nt":
        return key
    try:
        import winreg

        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as environment:
            value, kind = winreg.QueryValueEx(environment, "HY3D_API")
        if kind in {winreg.REG_SZ, winreg.REG_EXPAND_SZ} and isinstance(value, str):
            return value.strip()
    except (ImportError, OSError):
        pass
    return ""


def _config() -> tuple[str, str]:
    key = _api_key()
    if not key:
        raise HunyuanError("The Hunyuan3D TokenHub API key (HY3D_API) is not configured.",
                           code="provider_not_configured", category="configuration")
    if any(ord(char) < 33 or ord(char) > 126 for char in key):
        raise HunyuanError("The Hunyuan3D TokenHub API key contains invalid characters.",
                           code="invalid_configuration", category="configuration")
    # Pin the model for both creation and recovery; environment changes must not
    # accidentally send an existing task ID to another model's query endpoint.
    return key, _MODEL


def model_generation_available() -> bool:
    try:
        _config()
        return True
    except HunyuanError:
        return False


def _image_bytes(path: Any, *, multiview: bool) -> bytes:
    from PIL import Image

    try:
        with Path(path).open("rb") as stream:
            data = stream.read(_MAX_IMAGE_BYTES + 1)
        if len(data) > _MAX_IMAGE_BYTES:
            raise _invalid("Hunyuan3D input images must total at most 6 MB.")
        with Image.open(io.BytesIO(data)) as image:
            formats = {"PNG", "JPEG"} if multiview else {"PNG", "JPEG", "WEBP"}
            if image.format not in formats:
                raise _invalid("Hunyuan3D requires PNG/JPEG images (single images also allow WebP).")
            minimum, maximum = (129, 4999) if multiview else (128, 5000)
            if any(side < minimum or side > maximum for side in image.size):
                raise _invalid(f"Hunyuan3D image dimensions must be between {minimum} and {maximum} pixels.")
            image.verify()
        return data
    except HunyuanError:
        raise
    except (OSError, TypeError, ValueError, Image.DecompressionBombError):
        raise _invalid("The Hunyuan3D input image could not be read or is invalid.") from None


def prepare_model_payload(request: Any) -> dict[str, Any]:
    """Validate and snapshot all local inputs before consuming paid authorization."""
    _, model = _config()
    face_count = request.face_limit
    if type(face_count) is not int or not 3000 <= face_count <= 1500000:
        raise _invalid("Hunyuan3D supports between 3,000 and 1,500,000 faces.")
    if request.generation_profile not in {"quality", "performance"}:
        raise _invalid("Unsupported Hunyuan3D generation profile.")
    if request.geometry_quality not in {None, "standard"} or request.texture_quality != "standard":
        raise _invalid("Hunyuan3D currently supports standard geometry and texture quality only.")
    payload: dict[str, Any] = {
        # TokenHub's live validator requires Tencent's case-sensitive enum.
        "model": model, "face_count": face_count, "generate_type": "Normal", "enable_pbr": False,
    }
    if request.source == "text":
        prompt = request.prompt
        if not isinstance(prompt, str) or not prompt.strip() or len(prompt) > 1024:
            raise _invalid("Hunyuan3D requires a prompt of 1 to 1024 characters.")
        if request.image_path is not None or request.image_paths:
            raise _invalid("Hunyuan3D text input cannot include images.")
        payload["prompt"] = prompt
    elif request.source == "image":
        if request.image_paths:
            raise _invalid("Single-image Hunyuan3D input cannot include additional views.")
        payload["image_base64"] = base64.b64encode(_image_bytes(request.image_path, multiview=False)).decode("ascii")
    elif request.source == "multiview":
        views = request.image_paths
        if not isinstance(views, Mapping) or set(views) != {"front", "left", "back", "right"}:
            raise _invalid("Hunyuan3D multiview input requires front, left, back and right images.")
        images = {view: _image_bytes(views[view], multiview=True) for view in ("front", "left", "back", "right")}
        if sum(map(len, images.values())) > _MAX_IMAGE_BYTES:
            raise _invalid("Hunyuan3D input images must total at most 6 MB.")
        encoded = {view: base64.b64encode(data).decode("ascii") for view, data in images.items()}
        payload["image_base64"] = encoded["front"]
        payload["multi_view_images"] = [
            {"view_type": view, "view_image_base64": encoded[view]} for view in ("left", "back", "right")
        ]
    else:
        raise _invalid("Unsupported Hunyuan3D input source.")
    return payload


class _RejectRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> None:
        return None


def _provider_error(value: Any, *, http_status: int | None = None) -> HunyuanError:
    code = value.get("code", "provider_failed") if isinstance(value, Mapping) else "provider_failed"
    if not isinstance(code, str) or not re.fullmatch(r"[A-Za-z0-9_.]{1,100}", code):
        code = "provider_failed"
    category = "provider"
    retryable = False
    lowered = code.lower()
    if code.startswith(("AuthFailure", "UnauthorizedOperation")) or lowered in {"invalid_api_key", "invalidapikey", "unauthorized", "permission_denied"}:
        category = "authentication"
    elif any(marker in lowered for marker in ("balance", "arrears", "insufficientcredit")):
        category = "billing"
    elif code.startswith(("InvalidParameter", "MissingParameter")):
        category = "validation"
    elif code.startswith(("LimitExceeded", "RequestLimitExceeded")):
        category, retryable = "rate_limit", True
    elif code.startswith(("InternalError", "ResourceUnavailable")):
        category, retryable = "unavailable", True
    if category == "provider" and http_status is not None:
        category = "authentication" if http_status in {401, 403} else "rate_limit" if http_status == 429 else "network"
        retryable = http_status == 429 or http_status >= 500
    # Provider messages may echo prompt text or secrets; retain only its bounded code.
    return HunyuanError(f"Hunyuan3D rejected the request ({code}).", code=code, category=category, retryable=retryable)


def _submission_error(value: Any, payload: Mapping[str, Any], key: str, request_id: Any,
                      *, http_status: int | None = None) -> HunyuanError:
    """Keep actionable validation details without copying submitted content into logs."""
    error = _provider_error(value, http_status=http_status)
    suffix = ""
    if error.category == "validation" and isinstance(value, Mapping):
        detail = value.get("message")
        if isinstance(detail, str):
            # Redact before truncating, including JSON-escaped request values.
            private = [key]
            def collect(item: Any) -> None:
                if isinstance(item, Mapping):
                    for field, content in item.items():
                        if field in {"prompt", "image_base64", "view_image_base64", "image_url"} and isinstance(content, str):
                            private.append(content)
                        elif isinstance(content, (Mapping, list)):
                            collect(content)
                elif isinstance(item, list):
                    for content in item:
                        collect(content)
            collect(payload)
            for content in sorted(set(private), key=len, reverse=True):
                if content:
                    for spelling in (content, json.dumps(content, ensure_ascii=True)[1:-1]):
                        detail = detail.replace(spelling, "[redacted]")
            detail = re.sub(r"https?://[^\s\"<>]+", "[url redacted]", detail)
            detail = re.sub(r"(?i)bearer\s+\S+", "Bearer [redacted]", detail)
            detail = re.sub(r"[A-Za-z0-9_+/=-]{64,}", "[data redacted]", detail)
            detail = " ".join(detail.split())[:500]
            if detail:
                suffix += " " + detail
    if isinstance(request_id, str) and re.fullmatch(r"[A-Za-z0-9_-]{1,128}", request_id) and request_id != key:
        suffix += f" Request ID: {request_id}."
    error.args = (str(error) + suffix,)
    return error


def _request(action: str, payload: Mapping[str, Any], *, timeout: float = _REQUEST_TIMEOUT) -> dict[str, Any]:
    key, _ = _config()
    if action not in {"submit", "query"}:
        raise _invalid("Unsupported Hunyuan3D TokenHub operation.")
    creating = action == "submit"
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    headers = {
        "Authorization": "Bearer " + key, "Content-Type": "application/json", "Accept": "application/json",
    }
    request = urllib.request.Request(_BASE_URL + "/" + action, data=body, headers=headers, method="POST")
    opener = build_network_opener(_RejectRedirects())
    try:
        with opener.open(request, timeout=timeout) as response:
            data = response.read(_MAX_JSON_BYTES + 1)
    except urllib.error.HTTPError as exc:
        status = exc.code
        provider_error = None
        try:
            error_data = exc.read(_MAX_JSON_BYTES + 1)
            if len(error_data) <= _MAX_JSON_BYTES:
                envelope = json.loads(error_data.decode("utf-8"))
                error = envelope.get("error") if isinstance(envelope, dict) else None
                code = error.get("code") if isinstance(error, Mapping) else None
                if isinstance(code, str) and re.fullmatch(r"[A-Za-z0-9_.]{1,100}", code):
                    provider_error = _submission_error(error, payload, key, envelope.get("request_id"),
                                                       http_status=status) if creating else _provider_error({"code": code}, http_status=status)
        except (UnicodeError, ValueError, OSError, http.client.HTTPException):
            pass
        finally:
            exc.close()
        if provider_error is not None:
            # Even a structured 5xx cannot prove that a paid job was not created.
            provider_error.ambiguous = creating and (status >= 500 or status in {408, 409})
            provider_error.retryable = not creating and provider_error.retryable
            raise provider_error from None
        category = "authentication" if status in {401, 403} else "rate_limit" if status == 429 else "network"
        raise HunyuanError(f"Hunyuan3D returned HTTP {status}.", code="provider_http_error",
                           category=category, retryable=not creating and (status == 429 or status >= 500),
                           ambiguous=creating and (status >= 500 or status in {408, 409})) from None
    except (urllib.error.URLError, TimeoutError, OSError, http.client.HTTPException):
        raise HunyuanError("Could not connect to Hunyuan3D.", code="provider_connection_failed",
                           category="network", retryable=not creating, ambiguous=creating) from None
    try:
        if len(data) > _MAX_JSON_BYTES:
            raise ValueError()
        result = json.loads(data.decode("utf-8"))
        if not isinstance(result, dict):
            raise ValueError()
    except (UnicodeError, ValueError):
        raise HunyuanError("Hunyuan3D returned an invalid response.", code="invalid_provider_response",
                           ambiguous=creating) from None
    if "error" in result:
        raise (_submission_error(result["error"], payload, key, result.get("request_id"))
               if creating else _provider_error(result["error"]))
    return result


def submit_model_task(payload: Mapping[str, Any]) -> str:
    result = _request("submit", payload)
    job_id = result.get("id")
    if not isinstance(job_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", job_id):
        raise HunyuanError("Hunyuan3D did not return a task reference; check Tencent Cloud before retrying.",
                           code="missing_task_reference", ambiguous=True)
    return job_id


def _check_wait(stop_event: Any, end: float) -> None:
    if stop_event is not None and stop_event.is_set():
        raise HunyuanError("The operation was cancelled; the remote Hunyuan3D task may continue.",
                           code="cancelled", category="cancelled")
    if time.monotonic() >= end:
        raise HunyuanError("The Hunyuan3D task deadline expired; its existing task can be checked again.",
                           code="provider_timeout", category="timeout", retryable=True)


def wait_for_task(task_id: str, stop_event: Any = None,
                  progress: Callable[[int | float | None], None] | None = None,
                  deadline: float | None = None) -> dict[str, Any]:
    if not isinstance(task_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", task_id):
        raise _invalid("A Hunyuan3D task reference is required.")
    try:
        duration = float(deadline if deadline is not None else os.environ.get("HUNYUAN3D_TASK_DEADLINE_SECONDS", _DEFAULT_DEADLINE))
        if not math.isfinite(duration) or duration < 0:
            raise ValueError()
    except (TypeError, ValueError):
        raise _invalid("Hunyuan3D task deadline must be a finite nonnegative duration.") from None
    end = time.monotonic() + duration
    while True:
        _check_wait(stop_event, end)
        _, model = _config()
        result = _request("query", {"model": model, "id": task_id}, timeout=min(_REQUEST_TIMEOUT, max(0.001, end - time.monotonic())))
        _check_wait(stop_event, end)
        status = result.get("status")
        if status == "failed":
            raise _provider_error(result.get("error"))
        if status == "completed":
            files = result.get("data")
            if not isinstance(files, list):
                raise HunyuanError("Hunyuan3D returned no model artifact.", code="missing_artifact")
            for kind in ("GLB", "OBJ"):
                for file in files:
                    if isinstance(file, Mapping) and isinstance(file.get("type"), str) and file["type"].upper() == kind and isinstance(file.get("url"), str):
                        _validate_artifact_url(file["url"])
                        if progress is not None:
                            progress(100)
                        compatibility_files = [
                            {"Type": item["type"].upper(), "Url": item["url"],
                             "PreviewImageUrl": item.get("preview_image_url", "")}
                            for item in files if isinstance(item, Mapping) and isinstance(item.get("type"), str)
                            and isinstance(item.get("url"), str)
                        ]
                        return {**result, "task_id": task_id, "status": "success", "provider": "hunyuan",
                                "provider_status": status, "ResultFile3Ds": compatibility_files,
                                "output": {"model": file["url"]}, "output_format": kind.lower()}
            raise HunyuanError("Hunyuan3D returned no supported GLB or OBJ artifact.", code="missing_artifact")
        if status not in {"queued", "in_progress"}:
            raise HunyuanError("Hunyuan3D returned an unknown task state.", code="invalid_provider_response")
        if progress is not None:
            progress(None)
        delay = max(0.0, min(_POLL_INTERVAL, end - time.monotonic()))
        if stop_event is not None:
            stop_event.wait(delay)
        else:
            time.sleep(delay)


def _validate_artifact_url(url: str) -> None:
    try:
        parsed = urllib.parse.urlsplit(url)
        hostname = parsed.hostname or ""
        # COS virtual-hosted buckets and COS endpoints used by the official API examples.
        allowed = re.fullmatch(r"(?:[a-z0-9-]+\.)?cos\.[a-z0-9-]+\.(?:myqcloud\.com|tencentcos\.cn)", hostname)
        if parsed.scheme != "https" or not allowed or parsed.username or parsed.password or parsed.port not in {None, 443} or parsed.fragment:
            raise ValueError()
    except (TypeError, ValueError):
        raise HunyuanError("Hunyuan3D returned an unsafe artifact location.", code="unsafe_artifact", category="validation") from None


def download_task_artifact(task_result: Mapping[str, Any], destination: Path,
                           max_bytes: int = 512 * 1024 * 1024) -> Path:
    output = task_result.get("output", {})
    url = output.get("model") if isinstance(output, Mapping) else None
    if not isinstance(url, str):
        raise HunyuanError("Hunyuan3D returned no downloadable artifact.", code="missing_artifact")
    _validate_artifact_url(url)
    if type(max_bytes) is not int or max_bytes <= 0:
        raise _invalid("The artifact download limit must be positive.")
    target = Path(destination)
    part: Path | None = None
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        request = urllib.request.Request(url, headers={"Accept": "application/octet-stream"}, method="GET")
        with build_network_opener(_RejectRedirects()).open(request, timeout=_REQUEST_TIMEOUT) as response:
            if getattr(response, "status", 200) != 200:
                raise HunyuanError("The Hunyuan3D artifact response was incomplete.", code="artifact_download_failed")
            length = response.headers.get("Content-Length")
            try:
                expected = int(length) if length is not None else None
            except ValueError:
                raise HunyuanError("The Hunyuan3D artifact response has an invalid length.", code="artifact_download_failed") from None
            if expected is not None and (expected < 0 or expected > max_bytes):
                raise HunyuanError("The Hunyuan3D artifact exceeds the download limit.", code="artifact_too_large")
            with tempfile.NamedTemporaryFile(dir=target.parent, prefix=target.name + ".", suffix=".part", delete=False) as stream:
                part = Path(stream.name)
                size = 0
                while True:
                    chunk = response.read(min(1024 * 1024, max_bytes - size + 1))
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > max_bytes:
                        raise HunyuanError("The Hunyuan3D artifact exceeds the download limit.", code="artifact_too_large")
                    stream.write(chunk)
            if not size or (expected is not None and size != expected):
                raise HunyuanError("The Hunyuan3D artifact download was incomplete.", code="artifact_download_failed", retryable=True)
        os.replace(part, target)
        return target
    except (urllib.error.URLError, TimeoutError, OSError, http.client.HTTPException):
        raise HunyuanError("The Hunyuan3D artifact could not be downloaded.", code="artifact_download_failed",
                           category="network", retryable=True) from None
    finally:
        if part is not None:
            try:
                part.unlink(missing_ok=True)
            except OSError:
                pass

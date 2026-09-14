from __future__ import annotations

import email.utils
import ipaddress
import json
import os
import socket
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

try:
    from .ai_diagnostics import classify_connection_error, event as diagnostic_event, exception_details, safe_endpoint
    from .network_policy import build_opener as build_network_opener, network_diagnostics
except ImportError:
    from ai_diagnostics import classify_connection_error, event as diagnostic_event, exception_details, safe_endpoint
    from network_policy import build_opener as build_network_opener, network_diagnostics

_DEFAULT_BASE_URL = "https://openapi.tripo3d.com/v3"
_ARTIFACT_HOSTS = {"openapi.cdn.tripo3d.com"}
_DEFAULT_MODEL = "v3.1-20260211"
_DEFAULT_DEADLINE = 900.0
_POLL_INTERVAL = 1.5
_REQUEST_TIMEOUT = 120.0
_MAX_JSON_BYTES = 4 * 1024 * 1024
_MAX_UPLOAD_BYTES = 20 * 1024 * 1024
_TRANSIENT_CODES = {408, 425, 429, 500, 502, 503, 504}
_ARTIFACT_DOWNLOAD_ATTEMPTS = 3


class TripoError(RuntimeError):
    """A Tripo request or task failed safely."""


class _ArtifactDownloadInterrupted(RuntimeError):
    """The CDN ended a successful response before its declared length."""


def _config() -> tuple[str, str, str]:
    key = os.environ.get("TRIPO_API_KEY", "")
    base = os.environ.get("TRIPO_API_BASE", _DEFAULT_BASE_URL).rstrip("/")
    model = os.environ.get("TRIPO_MODEL", _DEFAULT_MODEL)
    if not key:
        raise TripoError("TRIPO_API_KEY is not configured.")
    parsed = urllib.parse.urlsplit(base)
    if parsed.scheme.lower() != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise TripoError("TRIPO_API_BASE must be a credential-free HTTPS URL.")
    return base, key, model


class _RejectRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> None:
        return None


def _read_json(response: Any) -> dict[str, Any]:
    content_length = response.headers.get("Content-Length")
    if content_length:
        try:
            if int(content_length) > _MAX_JSON_BYTES:
                raise TripoError("Tripo returned an oversized response.")
        except ValueError:
            pass
    body = response.read(_MAX_JSON_BYTES + 1)
    if len(body) > _MAX_JSON_BYTES:
        raise TripoError("Tripo returned an oversized response.")
    try:
        value = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise TripoError("Tripo returned an invalid response.") from None
    if not isinstance(value, dict):
        raise TripoError("Tripo returned an invalid response.")
    return value


def _data(envelope: Mapping[str, Any]) -> dict[str, Any]:
    code = envelope.get("code")
    data = envelope.get("data")
    if code != 0 or not isinstance(data, dict):
        safe_code: int | str = code if type(code) is int else "<invalid>"
        if isinstance(code, str) and len(code) <= 64 and all(char.isalnum() or char in "._-" for char in code):
            safe_code = code
        diagnostic_event(
            "tripo.response.rejected",
            level="ERROR",
            provider_code=safe_code,
            valid_data=isinstance(data, dict),
        )
        raise TripoError("Tripo rejected the request.")
    return data


def _retry_after(headers: Any, attempt: int) -> float:
    value = headers.get("Retry-After") if headers else None
    if value:
        try:
            return max(0.0, min(float(value), 30.0))
        except ValueError:
            try:
                parsed = email.utils.parsedate_to_datetime(value)
                if parsed.tzinfo is None:
                    parsed = parsed.replace(tzinfo=timezone.utc)
                return max(0.0, min((parsed - datetime.now(timezone.utc)).total_seconds(), 30.0))
            except (TypeError, ValueError, OverflowError):
                pass
    return min(0.75 * (2**attempt), 8.0)


def _request(
    method: str,
    path: str,
    body: bytes | None = None,
    content_type: str | None = None,
    *,
    status_retries: int = 0,
    stop_event: Any = None,
    deadline: float | None = None,
) -> dict[str, Any]:
    base, key, _ = _config()
    endpoint = base + path
    network = network_diagnostics(endpoint)
    opener = build_network_opener(_RejectRedirects())
    for attempt in range(status_retries + 1):
        if stop_event is not None and stop_event.is_set():
            raise TripoError("The operation was cancelled.")
        request = urllib.request.Request(endpoint, data=body, method=method)
        request.add_header("Authorization", "Bearer " + key)
        request.add_header("Accept", "application/json")
        if content_type:
            request.add_header("Content-Type", content_type)
        timeout = _REQUEST_TIMEOUT
        if deadline is not None:
            timeout = min(timeout, max(0.1, deadline - time.monotonic()))
        started = time.monotonic()
        diagnostic_event(
            "tripo.request.started",
            endpoint=safe_endpoint(endpoint),
            method=method,
            request_bytes=len(body) if body is not None else 0,
            timeout_seconds=timeout,
            attempt=attempt + 1,
            max_attempts=status_retries + 1,
            network=network,
        )
        try:
            with opener.open(request, timeout=timeout) as response:
                result = _read_json(response)
                diagnostic_event(
                    "tripo.request.completed",
                    endpoint=safe_endpoint(endpoint),
                    method=method,
                    http_status=getattr(response, "status", 200),
                    elapsed_ms=round((time.monotonic() - started) * 1000),
                    attempt=attempt + 1,
                    network=network,
                )
                return result
        except TripoError as exc:
            diagnostic_event(
                "tripo.response.invalid",
                level="ERROR",
                endpoint=safe_endpoint(endpoint),
                method=method,
                elapsed_ms=round((time.monotonic() - started) * 1000),
                attempt=attempt + 1,
                network=network,
                exception_chain=exception_details(exc),
            )
            raise
        except urllib.error.HTTPError as exc:
            retrying = exc.code in _TRANSIENT_CODES and attempt < status_retries
            safe_headers = {
                name: exc.headers.get(name, "")
                for name in ("x-request-id", "request-id", "cf-ray", "retry-after")
                if exc.headers and exc.headers.get(name)
            }
            diagnostic_event(
                "tripo.http.failed",
                level="ERROR",
                endpoint=safe_endpoint(endpoint),
                method=method,
                http_status=exc.code,
                response_headers=safe_headers,
                elapsed_ms=round((time.monotonic() - started) * 1000),
                attempt=attempt + 1,
                retrying=retrying,
                network=network,
                exception_chain=exception_details(exc),
            )
            if retrying:
                delay = _retry_after(exc.headers, attempt)
            else:
                if exc.code == 429:
                    message = "Tripo is rate limiting requests; try again later."
                elif 400 <= exc.code < 500:
                    message = "Tripo rejected the request."
                else:
                    message = "Tripo is temporarily unavailable."
                raise TripoError(message) from None
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            retrying = attempt < status_retries
            diagnostic_event(
                "tripo.connection.failed",
                level="ERROR",
                endpoint=safe_endpoint(endpoint),
                method=method,
                elapsed_ms=round((time.monotonic() - started) * 1000),
                attempt=attempt + 1,
                retrying=retrying,
                network=network,
                failure_kind=classify_connection_error(exc),
                exception_chain=exception_details(exc),
            )
            if attempt < status_retries:
                delay = _retry_after(None, attempt)
            else:
                raise TripoError("Could not connect to Tripo.") from None
        if deadline is not None and time.monotonic() + delay >= deadline:
            raise TripoError("The Tripo task deadline expired.")
        if stop_event is not None:
            if stop_event.wait(delay):
                raise TripoError("The operation was cancelled.")
        else:
            time.sleep(delay)
    raise TripoError("Could not connect to Tripo.")


def _post_json(path: str, payload: Mapping[str, Any]) -> dict[str, Any]:
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return _data(_request("POST", path, body, "application/json"))


def _task_id(data: Mapping[str, Any]) -> str:
    task_id = data.get("task_id")
    if not isinstance(task_id, str) or not task_id:
        raise TripoError("Tripo did not return a task reference.")
    return task_id


_ALLOWED_FACE_LIMITS = (100000, 300000, 500000, 1000000, 2000000)
_GENERATION_PROFILES = ("quality", "performance")


def validate_generation_options(face_limit: int, geometry_quality: str | None, texture_quality: str) -> None:
    if geometry_quality not in (None, "standard", "detailed"):
        raise TripoError("Geometry quality must be standard or detailed.")
    if texture_quality not in ("standard", "detailed", "extreme"):
        raise TripoError("Texture quality must be standard, detailed, or extreme.")
    if geometry_quality == "standard" and face_limit > 1500000:
        raise TripoError("The 2-million-face target requires detailed geometry.")


def _generation_payload(model: str, face_limit: int, generation_profile: str,
                        geometry_quality: str | None = None, texture_quality: str = "standard") -> dict[str, Any]:
    if face_limit not in _ALLOWED_FACE_LIMITS:
        raise TripoError(
            "The model face target must be 100000, 300000, 500000, 1000000, or 2000000 triangles."
        )
    if generation_profile not in _GENERATION_PROFILES:
        raise TripoError("The generation profile must be quality or performance.")
    validate_generation_options(face_limit, geometry_quality, texture_quality)
    # Old callers did not select geometry explicitly and used a capped standard
    # request. Never turn a frozen 2M request into a costlier Ultra task implicitly.
    effective_faces = min(face_limit, 1000000) if geometry_quality is None else face_limit
    if geometry_quality is not None and effective_faces == 2000000 and model != "v3.1-20260211":
        raise TripoError("The 2-million-face option requires Tripo v3.1 and detailed geometry.")
    return {
        "model": model,
        "smart_low_poly": False,
        "face_limit": effective_faces,
        "texture": True,
        "pbr": True,
        "texture_quality": texture_quality,
        "geometry_quality": geometry_quality or "standard",
        "quad": False,
        "export_uv": True,
    }


def create_text_task(prompt: str, face_limit: int = 1000000, generation_profile: str = "quality", *,
                     geometry_quality: str | None = None, texture_quality: str = "standard") -> str:
    if not isinstance(prompt, str) or not prompt.strip():
        raise TripoError("A text prompt is required.")
    _, _, model = _config()
    payload = _generation_payload(model, face_limit, generation_profile, geometry_quality, texture_quality)
    payload["prompt"] = prompt
    return _task_id(_post_json("/generation/text-to-model", payload))


def _image_kind(path: Path) -> tuple[str, str]:
    try:
        with path.open("rb") as stream:
            signature = stream.read(16)
    except OSError:
        raise TripoError("The input image could not be read.") from None
    if signature.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png", "png"
    if signature.startswith(b"\xff\xd8\xff"):
        return "image/jpeg", "jpg"
    raise TripoError("The input image must be PNG or JPEG.")


def upload_image(path: str | os.PathLike[str]) -> str:
    source = Path(path)
    mime_type, extension = _image_kind(source)
    try:
        if source.stat().st_size > _MAX_UPLOAD_BYTES:
            raise TripoError("The input image exceeds the 20 MB limit.")
        image = source.read_bytes()
    except TripoError:
        raise
    except OSError:
        raise TripoError("The input image could not be read.") from None
    boundary = "----OrcaAI" + uuid.uuid4().hex
    body = b"".join(
        [
            f"--{boundary}\r\n".encode("ascii"),
            f'Content-Disposition: form-data; name="file"; filename="input.{extension}"\r\n'.encode("ascii"),
            f"Content-Type: {mime_type}\r\n\r\n".encode("ascii"),
            image,
            b"\r\n",
            f"--{boundary}--\r\n".encode("ascii"),
        ]
    )
    data = _data(_request("POST", "/files", body, "multipart/form-data; boundary=" + boundary))
    token = data.get("file_token")
    if not isinstance(token, str) or not token:
        raise TripoError("Tripo did not return a file reference.")
    return token


def create_image_task(file_token: str, face_limit: int = 1000000, generation_profile: str = "quality", *,
                      geometry_quality: str | None = None, texture_quality: str = "standard") -> str:
    if not isinstance(file_token, str) or not file_token:
        raise TripoError("An uploaded image reference is required.")
    _, _, model = _config()
    payload = _generation_payload(model, face_limit, generation_profile, geometry_quality, texture_quality)
    payload.update({
        "input": file_token,
        "texture_alignment": "original_image",
        # Inputs have already passed Orca's identity, silhouette and material
        # gates. Provider autofix changed a real face in validation, while the
        # same v3.1 detailed request with autofix disabled preserved it.
        "enable_image_autofix": False,
    })
    return _task_id(_post_json("/generation/image-to-model", payload))


_MULTIVIEW_ORDER = ("front", "left", "back", "right")


def create_multiview_task(
    view_tokens: Mapping[str, str],
    face_limit: int = 1000000,
    generation_profile: str = "quality",
    *, geometry_quality: str | None = None, texture_quality: str = "standard",
) -> str:
    if not isinstance(view_tokens, Mapping):
        raise TripoError("Named multiview inputs are required.")
    unknown = set(view_tokens) - set(_MULTIVIEW_ORDER)
    if unknown:
        raise TripoError("Multiview inputs contain an unsupported view name.")
    normalized: dict[str, str] = {}
    for view, token in view_tokens.items():
        if not isinstance(token, str) or not token.strip():
            raise TripoError("Each multiview input requires an uploaded image reference.")
        normalized[view] = token.strip()
    if "front" not in normalized or len(normalized) < 2:
        raise TripoError("Multiview generation requires front and at least one additional view.")
    _, _, model = _config()
    payload = _generation_payload(model, face_limit, generation_profile, geometry_quality, texture_quality)
    payload["inputs"] = [{view: normalized[view]} for view in _MULTIVIEW_ORDER if view in normalized]
    payload["texture_alignment"] = "original_image"
    payload["orientation"] = "align_image"
    payload["enable_image_autofix"] = False
    return _task_id(_post_json("/generation/multiview-to-model", payload))


_TEXTURE_ALIGNMENTS = ("original_image", "geometry")
_TEXTURE_QUALITIES = ("standard", "detailed", "extreme")


def create_texture_task(
    source_task_id: str,
    image_token: str | Sequence[str],
    *,
    texture_alignment: str = "original_image",
    texture_quality: str = "standard",
    texture_seed: int | None = None,
) -> str:
    """Regenerate only the texture of an existing Tripo geometry task."""

    if not isinstance(source_task_id, str) or not source_task_id.strip():
        raise TripoError("A source model task reference is required.")
    if isinstance(image_token, str):
        if not image_token.strip():
            raise TripoError("An uploaded texture image reference is required.")
        texture_prompt = {"image": image_token.strip()}
    elif isinstance(image_token, Sequence):
        if len(image_token) != 4:
            raise TripoError("Multiview texturing requires exactly four image references.")
        normalized_tokens = []
        for token in image_token:
            if not isinstance(token, str) or not token.strip():
                raise TripoError("Each multiview texture image reference is required.")
            normalized_tokens.append(token.strip())
        # Tripo's texture API defines this positional order explicitly.
        texture_prompt = {"images": normalized_tokens}
    else:
        raise TripoError("An uploaded texture image reference is required.")
    if texture_alignment not in _TEXTURE_ALIGNMENTS:
        raise TripoError("Texture alignment must be original_image or geometry.")
    if texture_quality not in _TEXTURE_QUALITIES:
        raise TripoError("Texture quality must be standard, detailed, or extreme.")
    if texture_seed is not None and (isinstance(texture_seed, bool) or not isinstance(texture_seed, int)):
        raise TripoError("Texture seed must be an integer.")
    payload: dict[str, Any] = {
        "input": source_task_id.strip(),
        # Tripo recommends its v3.0 texture model for geometry generated by
        # either v3.0 or v3.1.
        "model": "v3.0-20250812",
        "texture_prompt": texture_prompt,
        "pbr": True,
        "texture_alignment": texture_alignment,
        "texture_quality": texture_quality,
        "bake": True,
    }
    if texture_seed is not None:
        payload["texture_seed"] = texture_seed
    return _task_id(_post_json("/models/texture", payload))


def get_task(task_id: str) -> dict[str, Any]:
    if not isinstance(task_id, str) or not task_id:
        raise TripoError("A task reference is required.")
    quoted = urllib.parse.quote(task_id, safe="")
    return _data(_request("GET", "/tasks/" + quoted, status_retries=3))


def wait_for_task(
    task_id: str,
    stop_event: Any = None,
    progress: Callable[[int | float | None], None] | None = None,
    deadline: float | None = None,
) -> dict[str, Any]:
    if not isinstance(task_id, str) or not task_id:
        raise TripoError("A task reference is required.")
    if deadline is None:
        try:
            duration = float(os.environ.get("TRIPO_TASK_DEADLINE_SECONDS", str(_DEFAULT_DEADLINE)))
        except ValueError:
            duration = _DEFAULT_DEADLINE
        end = time.monotonic() + max(0.0, duration)
    else:
        end = time.monotonic() + max(0.0, float(deadline))
    quoted = urllib.parse.quote(task_id, safe="")
    while True:
        if stop_event is not None and stop_event.is_set():
            raise TripoError("The operation was cancelled.")
        if time.monotonic() >= end:
            raise TripoError("The Tripo task deadline expired.")
        result = _data(
            _request(
                "GET",
                "/tasks/" + quoted,
                status_retries=3,
                stop_event=stop_event,
                deadline=end,
            )
        )
        state_value = result.get("status", result.get("state", ""))
        state = str(state_value).strip().lower()
        if progress is not None:
            try:
                progress(result.get("progress"))
            except Exception:
                raise TripoError("The progress callback failed.") from None
        if state == "success":
            return result
        if state == "failed":
            raise TripoError("The Tripo task failed.")
        if state == "cancelled" or state == "canceled":
            raise TripoError("The Tripo task was cancelled.")
        if state not in {"queued", "running"}:
            raise TripoError("The Tripo task ended in an unknown state.")
        remaining = end - time.monotonic()
        delay = min(_POLL_INTERVAL, remaining)
        if delay <= 0:
            raise TripoError("The Tripo task deadline expired.")
        if stop_event is not None:
            if stop_event.wait(delay):
                raise TripoError("The operation was cancelled.")
        else:
            time.sleep(delay)


def create_conversion(task_id: str, format: str) -> str:
    if not isinstance(task_id, str) or not task_id:
        raise TripoError("A source task reference is required.")
    if not isinstance(format, str) or not format.strip():
        raise TripoError("A conversion format is required.")
    return _task_id(_post_json("/models/convert", {"input": task_id, "format": format.upper()}))


def _artifact_url(task_result: Mapping[str, Any]) -> str:
    output = task_result.get("output", task_result)
    if not isinstance(output, Mapping):
        raise TripoError("The Tripo task returned no downloadable artifact.")
    preferred = ("model_url", "url", "base_model", "pbr_model", "rendered_image")
    for key in preferred:
        value = output.get(key)
        if isinstance(value, str) and value:
            return value
    for value in output.values():
        if isinstance(value, str) and value.lower().startswith("https://"):
            return value
        if isinstance(value, Mapping):
            try:
                return _artifact_url(value)
            except TripoError:
                pass
    raise TripoError("The Tripo task returned no downloadable artifact.")


def _validate_artifact_url(url: str) -> None:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme.lower() != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise TripoError("Tripo returned an unsafe artifact location.")
    hostname = parsed.hostname.lower().rstrip(".")
    if hostname not in _ARTIFACT_HOSTS:
        raise TripoError("Tripo returned an unsafe artifact location.")
    try:
        addresses = socket.getaddrinfo(parsed.hostname, parsed.port or 443, type=socket.SOCK_STREAM)
    except socket.gaierror:
        raise TripoError("The artifact host could not be resolved.") from None
    if not addresses:
        raise TripoError("The artifact host could not be resolved.")
    for address in addresses:
        try:
            ip = ipaddress.ip_address(address[4][0].split("%", 1)[0])
        except ValueError:
            raise TripoError("Tripo returned an unsafe artifact location.") from None
        # Windows proxy clients may return an RFC 2544 fake IP for an exact,
        # allowlisted HTTPS host. TLS still authenticates the hostname, while
        # lookalike and redirect hosts remain rejected above.
        if not ip.is_global and hostname not in _ARTIFACT_HOSTS:
            raise TripoError("Tripo returned an unsafe artifact location.")


class _SafeArtifactRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> Any:
        _validate_artifact_url(newurl)
        request_headers = {"Accept": "application/octet-stream"}
        byte_range = req.get_header("Range")
        if byte_range:
            request_headers["Range"] = byte_range
        return urllib.request.Request(newurl, headers=request_headers, method="GET")


def _artifact_response_size(response: Any, offset: int, resumed: bool) -> int | None:
    content_length = response.headers.get("Content-Length")
    expected_length: int | None = None
    if content_length:
        try:
            expected_length = int(content_length)
        except ValueError:
            expected_length = None
        if expected_length is not None and expected_length < 0:
            raise _ArtifactDownloadInterrupted("The artifact response has an invalid content length.")

    if not resumed:
        return expected_length

    content_range = response.headers.get("Content-Range", "")
    try:
        unit, value = content_range.split(" ", 1)
        interval, total_value = value.split("/", 1)
        start_value, end_value = interval.split("-", 1)
        start, end, total = int(start_value), int(end_value), int(total_value)
    except (AttributeError, TypeError, ValueError):
        raise _ArtifactDownloadInterrupted("The artifact resume response has an invalid byte range.") from None
    if unit.lower() != "bytes" or start != offset or end < start or total <= end:
        raise _ArtifactDownloadInterrupted("The artifact resume response has an invalid byte range.")
    if expected_length is not None and expected_length != end - start + 1:
        raise _ArtifactDownloadInterrupted("The artifact resume response has an inconsistent content length.")
    return total


def download_task_artifact(
    task_result: Mapping[str, Any], output_path: str | os.PathLike[str], max_bytes: int = 500 * 1024 * 1024
) -> Path:
    if max_bytes <= 0:
        raise TripoError("The artifact size limit must be positive.")
    url = _artifact_url(task_result)
    network = network_diagnostics(url)
    started = time.monotonic()
    diagnostic_event(
        "tripo.artifact_download.started",
        endpoint=safe_endpoint(url),
        max_bytes=max_bytes,
        timeout_seconds=_REQUEST_TIMEOUT,
        network=network,
    )
    _validate_artifact_url(url)
    destination = Path(output_path)
    part = destination.with_name(destination.name + ".part")
    last_error: BaseException | None = None
    for _ in range(_ARTIFACT_DOWNLOAD_ATTEMPTS):
        try:
            offset = part.stat().st_size if part.is_file() else 0
            if offset > max_bytes:
                raise TripoError("The artifact exceeds the configured size limit.")
            request_headers = {"Accept": "application/octet-stream"}
            if offset:
                request_headers["Range"] = f"bytes={offset}-"
            request = urllib.request.Request(url, headers=request_headers, method="GET")
            with build_network_opener(_SafeArtifactRedirects()).open(
                request, timeout=_REQUEST_TIMEOUT
            ) as response:
                resumed = offset > 0 and getattr(response, "status", 200) == 206
                if not resumed:
                    offset = 0
                expected_size = _artifact_response_size(response, offset, resumed)
                if expected_size is not None and expected_size > max_bytes:
                    raise TripoError("The artifact exceeds the configured size limit.")
                total = offset
                with part.open("ab" if resumed else "wb") as stream:
                    while chunk := response.read(min(64 * 1024, max_bytes + 1 - total)):
                        total += len(chunk)
                        if total > max_bytes:
                            raise TripoError("The artifact exceeds the configured size limit.")
                        stream.write(chunk)
            if expected_size is not None and total != expected_size:
                raise _ArtifactDownloadInterrupted(
                    f"The artifact download stopped at {total} of {expected_size} bytes."
                )
            if total <= 0:
                raise _ArtifactDownloadInterrupted("The artifact download returned no data.")
            os.replace(part, destination)
            diagnostic_event(
                "tripo.artifact_download.completed",
                endpoint=safe_endpoint(url),
                response_bytes=total,
                elapsed_ms=round((time.monotonic() - started) * 1000),
                attempt=_ + 1,
                network=network,
            )
            return destination
        except TripoError as exc:
            try:
                part.unlink(missing_ok=True)
            except OSError:
                pass
            diagnostic_event(
                "tripo.artifact_download.failed",
                level="ERROR",
                endpoint=safe_endpoint(url),
                elapsed_ms=round((time.monotonic() - started) * 1000),
                attempt=_ + 1,
                network=network,
                exception_chain=exception_details(exc),
            )
            raise
        except (
            urllib.error.HTTPError,
            urllib.error.URLError,
            TimeoutError,
            OSError,
            _ArtifactDownloadInterrupted,
        ) as exc:
            last_error = exc
            diagnostic_event(
                "tripo.artifact_download.retry",
                level="WARNING",
                endpoint=safe_endpoint(url),
                elapsed_ms=round((time.monotonic() - started) * 1000),
                attempt=_ + 1,
                retrying=_ + 1 < _ARTIFACT_DOWNLOAD_ATTEMPTS,
                network=network,
                failure_kind=classify_connection_error(exc),
                exception_chain=exception_details(exc),
            )

    try:
        part.unlink(missing_ok=True)
    except OSError:
        pass
    if isinstance(last_error, _ArtifactDownloadInterrupted):
        raise TripoError("The artifact download was incomplete after retrying.") from None
    raise TripoError("The artifact could not be downloaded after retrying.") from None

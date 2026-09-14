from __future__ import annotations

from contextlib import contextmanager
from contextvars import ContextVar
from datetime import datetime, timezone
import json
import os
import re
import socket
import ssl
import sys
import threading
from typing import Any, Iterator
import urllib.parse


_JOB_ID: ContextVar[str] = ContextVar("orcaslicer_ai_job_id", default="")
_WRITE_LOCK = threading.Lock()
_MAX_TEXT = 500
_SENSITIVE_TEXT_FIELD = (
    r"(?:hy3d[_-]?api|openai[_-]?pro[_-]?api|[A-Za-z0-9_.-]*(?:api[_-]?key|token|secret|password|passwd|pwd|"
    r"authorization|cookie|credential|private[_-]?key|session[_-]?proof|nonce)"
    r")"
)
_QUOTED_SECRET_PATTERN = re.compile(
    rf"(?P<prefix>[\"']{_SENSITIVE_TEXT_FIELD}[\"']\s*:\s*)"
    rf"(?P<quote>[\"'])(?P<value>(?:\\.|(?!(?P=quote))[\s\S])*)(?P=quote)",
    re.IGNORECASE,
)
_QUOTED_BARE_SECRET_PATTERN = re.compile(
    rf"(?P<prefix>[\"']{_SENSITIVE_TEXT_FIELD}[\"']\s*:\s*)"
    r"(?P<value>(?![\"'])[^\s,}\]]+)",
    re.IGNORECASE,
)
_SECRET_PATTERNS = (
    (
        re.compile(r"(?i)(authorization\s*[:=]\s*(?:(?:bearer|basic)\s+)?)[^\s,;}\]]+"),
        r"\1<redacted>",
    ),
    (re.compile(r"(?i)(bearer\s+)[^\s,;]+"), r"\1<redacted>"),
    (
        re.compile(
            r"(?i)((?:hy3d[_-]?api|openai[_-]?pro[_-]?api|api[_-]?key|token|secret|password|passwd|pwd|cookie|credential|"
            r"private[_-]?key|session[_-]?proof|nonce)\s*[:=]\s*)[^\s,;}\]]+"
        ),
        r"\1<redacted>",
    ),
    (re.compile(r"\bsk-[A-Za-z0-9_-]{8,}\b"), "<redacted>"),
)
_URL_PATTERN = re.compile(r"https?://[^\s\"'<>]+", re.IGNORECASE)


def _normalized_field_name(value: Any) -> str:
    text = str(value)
    text = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", text)
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", text)
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def _is_sensitive_field(value: Any) -> bool:
    name = _normalized_field_name(value)
    if not name:
        return False
    if name in {
        "api_key",
        "openai_pro_api",
        "hy3d_api",
        "hy3_d_api",
        "token",
        "secret",
        "password",
        "passwd",
        "pwd",
        "authorization",
        "authorization_header",
        "proxy_authorization",
        "proxy_authorization_header",
        "cookie",
        "cookies",
        "cookie_header",
        "set_cookie",
        "credential",
        "credentials",
        "private_key",
        "session_proof",
        "nonce",
    }:
        return True
    return name.endswith(
        (
            "_api_key",
            "_token",
            "_secret",
            "_password",
            "_passwd",
            "_pwd",
            "_authorization",
            "_authorization_header",
            "_cookie",
            "_cookies",
            "_cookie_header",
            "_credential",
            "_credentials",
            "_private_key",
            "_session_proof",
            "_nonce",
        )
    )


def safe_endpoint(value: str) -> str:
    """Return a credential-free URL without query or fragment data."""
    try:
        parsed = urllib.parse.urlsplit(str(value))
        if parsed.scheme.lower() not in {"http", "https"} or not parsed.hostname:
            return "<invalid-endpoint>"
        host = parsed.hostname
        if ":" in host and not host.startswith("["):
            host = f"[{host}]"
        try:
            port = f":{parsed.port}" if parsed.port is not None else ""
        except ValueError:
            return "<invalid-endpoint>"
        path = parsed.path or "/"
        return urllib.parse.urlunsplit((parsed.scheme.lower(), host + port, path, "", ""))
    except Exception:
        return "<invalid-endpoint>"


def redact_text(value: Any) -> str:
    text = str(value)
    stripped = text.lstrip()
    if stripped.startswith(("{", "[")):
        try:
            parsed = json.loads(text)
            if isinstance(parsed, (dict, list)):
                text = json.dumps(_safe_value(parsed), ensure_ascii=False, separators=(",", ":"))
        except (TypeError, ValueError):
            pass
    text = _QUOTED_SECRET_PATTERN.sub(
        lambda match: f"{match.group('prefix')}{match.group('quote')}<redacted>{match.group('quote')}",
        text,
    )
    text = _QUOTED_BARE_SECRET_PATTERN.sub(r"\g<prefix><redacted>", text)
    for pattern, replacement in _SECRET_PATTERNS:
        text = pattern.sub(replacement, text)
    text = _URL_PATTERN.sub(lambda match: safe_endpoint(match.group(0).rstrip(".,);]")), text)
    if len(text) > _MAX_TEXT:
        text = text[:_MAX_TEXT] + "..."
    return text


def _safe_value(value: Any, *, field_name: Any = None) -> Any:
    if field_name is not None and _is_sensitive_field(field_name):
        return "<redacted>"
    if value is None or isinstance(value, (bool, int, float)):
        return value
    if isinstance(value, dict):
        return {redact_text(key): _safe_value(item, field_name=key) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [_safe_value(item) for item in value]
    return redact_text(value)


def exception_details(error: BaseException) -> list[dict[str, Any]]:
    details: list[dict[str, Any]] = []
    seen: set[int] = set()
    current: BaseException | None = error
    while current is not None and len(details) < 5 and id(current) not in seen:
        seen.add(id(current))
        item: dict[str, Any] = {
            "type": type(current).__name__,
            "message": redact_text(current),
        }
        error_number = getattr(current, "errno", None)
        if isinstance(error_number, int):
            item["errno"] = error_number
        details.append(item)
        reason = getattr(current, "reason", None)
        if isinstance(reason, BaseException) and id(reason) not in seen:
            current = reason
        else:
            current = current.__cause__ or current.__context__
    return details


def classify_connection_error(error: BaseException) -> str:
    seen: set[int] = set()
    current: BaseException | None = error
    while current is not None and id(current) not in seen:
        seen.add(id(current))
        message = str(current).lower()
        if isinstance(current, ssl.SSLCertVerificationError):
            return "tls_certificate"
        if isinstance(current, ssl.SSLError):
            return "tls"
        if isinstance(current, socket.gaierror):
            return "dns"
        if isinstance(current, (TimeoutError, socket.timeout)):
            return "timeout"
        if isinstance(current, ConnectionRefusedError):
            return "connection_refused"
        if isinstance(current, ConnectionResetError):
            return "connection_reset"
        if "proxy" in message or "tunnel connection failed" in message:
            return "proxy"
        reason = getattr(current, "reason", None)
        current = reason if isinstance(reason, BaseException) else current.__cause__ or current.__context__
    return "network"


@contextmanager
def diagnostic_context(job_id: str) -> Iterator[None]:
    token = _JOB_ID.set(redact_text(job_id))
    try:
        yield
    finally:
        _JOB_ID.reset(token)


def event(name: str, *, level: str = "INFO", **fields: Any) -> None:
    """Write one best-effort JSON event; diagnostics must never break production work."""
    try:
        payload: dict[str, Any] = {
            "timestamp": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"),
            "level": redact_text(level).upper(),
            "event": redact_text(name),
            "pid": os.getpid(),
            "thread": threading.current_thread().name,
        }
        job_id = _JOB_ID.get()
        if job_id:
            payload["job_id"] = job_id
        for key, value in fields.items():
            safe_key = re.sub(r"[^a-zA-Z0-9_.-]", "_", str(key))[:80]
            payload[safe_key] = _safe_value(value, field_name=key)
        encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        with _WRITE_LOCK:
            sys.stderr.write(encoded + "\n")
            sys.stderr.flush()
    except Exception:
        pass

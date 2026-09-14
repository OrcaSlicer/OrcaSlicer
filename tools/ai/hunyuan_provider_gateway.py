"""Tencent Hunyuan adapter for Orca's confirmed, recoverable generation flow."""
from __future__ import annotations

from pathlib import Path
from typing import Any, Mapping

try:
    from . import hunyuan_client as client
    from .model_provider_gateway import ModelTaskRequest, PaidTaskAuthorization, ProviderGatewayError, ProviderTaskRef
except ImportError:
    import hunyuan_client as client
    from model_provider_gateway import ModelTaskRequest, PaidTaskAuthorization, ProviderGatewayError, ProviderTaskRef


def _error(error: client.HunyuanError, operation: str) -> ProviderGatewayError:
    return ProviderGatewayError(str(error), code=error.code, category=error.category,
                                provider="hunyuan", operation=operation,
                                retryable=error.retryable, ambiguous=error.ambiguous)


def validate_options(face_limit: int, geometry: str | None, texture: str) -> None:
    if face_limit not in (300000, 1000000) or geometry not in (None, "standard") or texture != "standard":
        raise ProviderGatewayError(
            "Hunyuan 3D supports 300000 or 1000000 faces here, with standard geometry and texture.",
            code="invalid_model_request", category="validation", provider="hunyuan", operation="model_generation")


class HunyuanModelProviderGateway:
    def model_generation_available(self) -> bool:
        return client.model_generation_available()

    def start_or_reuse_model_task(self, request: ModelTaskRequest, *, existing_task_id: str = "",
                                 authorization: PaidTaskAuthorization | None = None) -> ProviderTaskRef:
        if not isinstance(existing_task_id, str):
            raise ProviderGatewayError("Invalid Hunyuan task reference.", code="invalid_task_reference",
                                       category="validation", provider="hunyuan")
        if existing_task_id.strip():
            return ProviderTaskRef("hunyuan", existing_task_id.strip(), True)
        validate_options(request.face_limit, request.geometry_quality, request.texture_quality)
        if request.output_format not in ("glb", "obj"):
            raise ProviderGatewayError("Unsupported output format.", code="invalid_model_request",
                                       category="validation", provider="hunyuan")
        try:
            payload = client.prepare_model_payload(request)
        except client.HunyuanError as exc:
            raise _error(exc, "model_generation") from None
        if authorization is None:
            raise ProviderGatewayError("Explicit confirmation is required before creating a paid model task.",
                                       code="authorization_required", category="authorization", provider="hunyuan")
        authorization.consume("hunyuan", "model_generation")
        try:
            task_id = client.submit_model_task(payload)
        except client.HunyuanError as exc:
            raise _error(exc, "model_generation") from None
        return ProviderTaskRef("hunyuan", task_id, False)

    def wait_for_task(self, task_id: str, *, stop_event: Any = None, progress: Any = None) -> dict[str, Any]:
        try:
            return client.wait_for_task(task_id, stop_event=stop_event, progress=progress)
        except client.HunyuanError as exc:
            raise _error(exc, "task_poll") from None

    def download_artifact(self, task_result: Mapping[str, Any], output_path: str | Path, max_bytes: int,
                          *, output_format: str = "glb") -> Path:
        # Normal generation includes both formats; selecting a returned file never
        # starts a conversion or another billable task.
        selected = next((item for item in task_result.get("ResultFile3Ds", [])
                         if isinstance(item, Mapping) and str(item.get("Type", "")).lower() == output_format
                         and isinstance(item.get("Url"), str) and item["Url"]), None)
        if selected is None:
            raise ProviderGatewayError("Hunyuan did not return the selected model format.",
                                       code="invalid_provider_result", category="validation", provider="hunyuan",
                                       operation="artifact_download")
        result = dict(task_result, output={"model": selected["Url"]})
        try:
            return client.download_task_artifact(result, output_path, max_bytes)
        except client.HunyuanError as exc:
            raise _error(exc, "artifact_download") from None

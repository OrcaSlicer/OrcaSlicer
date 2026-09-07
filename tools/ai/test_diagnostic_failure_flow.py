from __future__ import annotations

import hashlib
import hmac
import json
from io import BytesIO
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import urllib.request

from PIL import Image


TOOLS_AI = Path(__file__).resolve().parent
BOOTSTRAP = TOOLS_AI / "orca_ai_installed_bootstrap.py"
LOCAL_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _offline_environment() -> dict[str, str]:
    # A developer's higher-priority Image2 credentials must never escape into
    # this connection-refused test, even when legacy OpenAI values are replaced.
    provider_prefixes = ("OPENAI_", "TRIPO_", "TRIPO3D_", "ORCASLICER_AI_")
    return {name: value for name, value in os.environ.items()
            if not name.upper().startswith(provider_prefixes)}


def _session_headers(base_url: str, token: str) -> dict[str, str]:
    client_nonce = "b" * 64
    challenge = urllib.request.Request(
        f"{base_url}/v1/orcaslicer/session-challenge",
        headers={"X-OrcaSlicer-Client-Nonce": client_nonce},
    )
    with LOCAL_OPENER.open(challenge, timeout=1) as response:
        payload = json.loads(response.read())
    server_nonce = payload["server_nonce"]
    expected_server_proof = hmac.new(
        token.encode("ascii"),
        f"server:{client_nonce}:{server_nonce}".encode("ascii"),
        hashlib.sha256,
    ).hexdigest()
    if not hmac.compare_digest(payload["server_proof"], expected_server_proof):
        raise RuntimeError("Sidecar returned an invalid server proof")
    client_proof = hmac.new(
        token.encode("ascii"),
        f"client:{server_nonce}".encode("ascii"),
        hashlib.sha256,
    ).hexdigest()
    return {
        "X-OrcaSlicer-Client": "native",
        "X-OrcaSlicer-Session-Proof": client_proof,
    }


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def _multipart_image() -> tuple[bytes, str]:
    boundary = "----OrcaDiagnosticFailure"
    parts: list[bytes] = []
    fields = {
        "request_id": "offline-diagnostic-test",
        "instruction": "preserve the subject",
        "style": "sculpture",
        "custom_style": "",
        "palette": "[]",
        "palette_roles": "{}",
        "print": "{}",
    }
    for name, value in fields.items():
        parts.append(
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n{value}\r\n".encode()
        )
    parts.append(
        f"--{boundary}\r\nContent-Disposition: form-data; name=\"image\"; filename=\"input.png\"\r\n"
        "Content-Type: image/png\r\n\r\n".encode()
        + _valid_png_bytes()
        + b"\r\n"
    )
    parts.append(f"--{boundary}--\r\n".encode())
    return b"".join(parts), boundary


def _valid_png_bytes() -> bytes:
    output = BytesIO()
    Image.new("RGB", (96, 96), (230, 230, 230)).save(output, format="PNG")
    return output.getvalue()


class DiagnosticFailureFlowTests(unittest.TestCase):
    def test_offline_environment_drops_inherited_provider_configuration(self) -> None:
        inherited = {
            "OPENAI_API_KEY": "unused-test-key",
            "OPENAI_PRO_API": "unused-pro-key",
            "OPENAI_PRO_URL": "https://provider.invalid/v1",
            "TRIPO_API_KEY": "unused-tripo-key",
            "TRIPO_API_BASE": "https://provider.invalid/v3",
            "ORCASLICER_AI_CONFIG_MODE": "internal_locked",
            "SYSTEMROOT": "test-system-root",
        }
        with mock.patch.dict(os.environ, inherited, clear=True):
            self.assertEqual(_offline_environment(), {"SYSTEMROOT": "test-system-root"})

    def test_installed_sidecar_correlates_connection_failure_with_job_id(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            data_dir = Path(directory)
            packaged_tools = data_dir / "runtime" / "tools" / "ai"
            shutil.copytree(
                TOOLS_AI,
                packaged_tools,
                ignore=shutil.ignore_patterns("test_*.py", "__pycache__", "orca_ai_internal_defaults.json"),
            )
            (packaged_tools / "orca_ai_build_info.json").write_text(
                json.dumps({
                    "schema_version": 1,
                    "application_version": "2.5.0-test",
                    "application_commit": "f" * 40,
                    "package_revision": "diagnostic-test",
                    "distribution_channel": "internal",
                    "sidecar_protocol_version": 2,
                    "sidecar_version": "orcaslicer-ai-sidecar-v9",
                }),
                encoding="utf-8",
            )
            port = _free_port()
            session_token = "a" * 64
            environment = _offline_environment()
            environment.update(
                OPENAI_API_KEY="test-openai",
                TRIPO_API_KEY="test-tripo",
                OPENAI_BASE_URL="https://127.0.0.1:1/v1",
                TRIPO_API_BASE="https://127.0.0.1:1/v3",
                ORCASLICER_AI_SIDECAR_HOST="127.0.0.1",
                ORCASLICER_AI_SIDECAR_PORT=str(port),
                ORCASLICER_AI_OUTPUT_DIR=str(data_dir / "generated_models"),
                ORCASLICER_AI_PARENT_PID=str(os.getpid()),
                ORCASLICER_AI_SESSION_TOKEN=session_token,
                NO_PROXY="127.0.0.1,localhost",
                no_proxy="127.0.0.1,localhost",
            )
            process = subprocess.Popen(
                [sys.executable, str(packaged_tools / BOOTSTRAP.name), str(data_dir)],
                cwd=packaged_tools,
                env=environment,
            )
            try:
                base_url = f"http://127.0.0.1:{port}"
                health_url = f"{base_url}/health"
                session_headers: dict[str, str] = {}
                deadline = time.time() + 10
                while True:
                    try:
                        session_headers = _session_headers(base_url, session_token)
                        health = urllib.request.Request(health_url, headers=session_headers)
                        with LOCAL_OPENER.open(health, timeout=1) as response:
                            self.assertEqual(response.status, 200)
                            break
                    except OSError:
                        if process.poll() is not None or time.time() >= deadline:
                            log_path = data_dir / "log" / "orca-ai-sidecar.log"
                            detail = log_path.read_text(encoding="utf-8") if log_path.is_file() else "log missing"
                            self.fail(f"Installed diagnostic Sidecar did not become healthy:\n{detail}")
                        time.sleep(0.05)

                body, boundary = _multipart_image()
                create = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/image",
                    data=body,
                    method="POST",
                    headers={
                        **session_headers,
                        "Content-Type": f"multipart/form-data; boundary={boundary}",
                    },
                )
                with LOCAL_OPENER.open(create, timeout=5) as response:
                    job_id = json.loads(response.read())["job"]["id"]

                status_url = f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job_id}"
                deadline = time.time() + 10
                state = ""
                while time.time() < deadline:
                    status = urllib.request.Request(status_url, headers=session_headers)
                    with LOCAL_OPENER.open(status, timeout=2) as response:
                        job = json.loads(response.read())["job"]
                    state = job["state"]
                    if state == "failed":
                        break
                    time.sleep(0.05)
                log_path = data_dir / "log" / "orca-ai-sidecar.log"
                log_detail = log_path.read_text(encoding="utf-8") if log_path.is_file() else "log missing"
                self.assertEqual(state, "failed", log_detail)
                self.assertEqual(job["message"], "Could not connect to the preprocessing service.")

                events = [
                    json.loads(line)
                    for line in log_path.read_text(encoding="utf-8").splitlines()
                    if line.startswith("{")
                ]
                failures = [event for event in events if event.get("event") == "provider.connection.failed"]
                self.assertEqual(len(failures), 1)
                self.assertEqual(failures[0]["job_id"], job_id)
                self.assertEqual(failures[0]["failure_kind"], "connection_refused")
                self.assertEqual(failures[0]["endpoint"], "https://127.0.0.1:1/v1/images/edits")
                serialized = json.dumps(events)
                self.assertNotIn("test-openai", serialized)
                self.assertNotIn("test-tripo", serialized)
                self.assertNotIn("preserve the subject", serialized)
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()

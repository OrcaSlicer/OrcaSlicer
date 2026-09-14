#!/usr/bin/env python3
import contextlib
import hmac
import importlib.util
from io import BytesIO
import json
import os
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
import urllib.parse
import uuid
from http.server import ThreadingHTTPServer
from pathlib import Path
from unittest import mock

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(TOOLS_AI))


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


MOCK = load_module("orca_ai_sidecar_mock_contract", ROOT / "ai_sidecar_mock.py")
PRODUCTION = load_module("orca_ai_sidecar_production_contract", TOOLS_AI / "orca_ai_sidecar.py")


def valid_png_bytes(size: int = 96) -> bytes:
    output = BytesIO()
    image = Image.new("RGB", (size, size), "white")
    image.paste("navy", (size // 4, size // 4, size * 3 // 4, size * 3 // 4))
    image.save(output, format="PNG")
    return output.getvalue()


def multipart_image_request(fields: dict[str, str], image: bytes) -> tuple[bytes, str]:
    boundary = f"orca-{uuid.uuid4().hex}"
    parts: list[bytes] = []
    for name, value in fields.items():
        parts.extend([
            f"--{boundary}\r\n".encode(),
            f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode(),
            value.encode("utf-8"),
            b"\r\n",
        ])
    parts.extend([
        f"--{boundary}\r\n".encode(),
        b'Content-Disposition: form-data; name="image"; filename="input.png"\r\n',
        b"Content-Type: image/png\r\n\r\n",
        image,
        b"\r\n",
        f"--{boundary}--\r\n".encode(),
    ])
    return b"".join(parts), f"multipart/form-data; boundary={boundary}"


@contextlib.contextmanager
def sidecar_server(handler):
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


@contextlib.contextmanager
def temporary_environment(**values):
    original = {key: os.environ.get(key) for key in values}
    try:
        for key, value in values.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        yield
    finally:
        for key, value in original.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


class SidecarHealthContractTests(unittest.TestCase):
    def setUp(self):
        original_open = urllib.request.OpenerDirector.open
        def local_only(opener, request, *args, **kwargs):
            url = request.full_url if isinstance(request, urllib.request.Request) else str(request)
            if urllib.parse.urlsplit(url).hostname not in {"localhost", "127.0.0.1", "::1"}:
                raise AssertionError("Contract tests must mock external provider requests.")
            return original_open(opener, request, *args, **kwargs)
        guard = mock.patch.object(urllib.request.OpenerDirector, "open", local_only)
        guard.start()
        self.addCleanup(guard.stop)

    def test_parent_pid_validation_and_current_process_probe(self):
        with temporary_environment(ORCASLICER_AI_PARENT_PID=None):
            self.assertIsNone(PRODUCTION._configured_parent_pid())
        for value in ("zero", "0", "-1", "4294967296", " 1x "):
            with self.subTest(value=value), temporary_environment(ORCASLICER_AI_PARENT_PID=value):
                self.assertIsNone(PRODUCTION._configured_parent_pid())
        with temporary_environment(ORCASLICER_AI_PARENT_PID=str(os.getpid())):
            self.assertEqual(PRODUCTION._configured_parent_pid(), os.getpid())
            self.assertTrue(PRODUCTION._parent_process_alive(os.getpid()))
        if os.name == "nt":
            handle = PRODUCTION._open_parent_process_handle(os.getpid())
            self.assertIsNotNone(handle)
            try:
                self.assertTrue(PRODUCTION._parent_process_handle_alive(handle))
            finally:
                PRODUCTION._close_parent_process_handle(handle)

    def test_glb_download_recheck_and_visual_review_keep_the_registered_artifact(self):
        from test_glb_artifact import fixture
        with tempfile.TemporaryDirectory() as directory, temporary_environment(ORCASLICER_AI_OUTPUT_DIR=directory):
            job = PRODUCTION._new_job("text", ())
            artifact = job.directory / "model.glb"
            fixture(artifact)
            original = artifact.read_bytes()
            job.artifact_path = artifact
            job.artifact_format = "glb"
            job.state = job.phase = "ready"
            with PRODUCTION._JOBS_LOCK:
                PRODUCTION._JOBS[job.id] = job
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    url = f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}"
                    headers = {"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"}
                    with urllib.request.urlopen(urllib.request.Request(url + "/artifact", headers=headers), timeout=5) as response:
                        self.assertEqual(response.headers.get_content_type(), "model/gltf-binary")
                        self.assertEqual(response.read(), original)
                    with urllib.request.urlopen(urllib.request.Request(url + "/recheck", data=b"{}", headers=headers), timeout=5) as response:
                        self.assertEqual(json.load(response)["job"]["artifact"]["format"], "glb")
                    with mock.patch.object(PRODUCTION, "review_model_visual_quality") as review:
                        with urllib.request.urlopen(urllib.request.Request(url + "/visual-review", data=b"{}", headers=headers), timeout=5) as response:
                            self.assertEqual(response.status, 200)
                        self.assertEqual(review.call_args.args[0].suffix, ".obj")
                        self.assertTrue(review.call_args.args[0].is_file())
                self.assertEqual(artifact.read_bytes(), original)
                self.assertEqual(job.artifact_path, artifact)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.pop(job.id, None)

    def test_parent_monitor_stops_server_after_parent_exits(self):
        stopped = threading.Event()

        class FakeServer:
            def shutdown(self):
                stopped.set()

        with mock.patch.object(PRODUCTION.os, "name", "posix"), \
             mock.patch.object(PRODUCTION, "_parent_process_alive", return_value=False):
            PRODUCTION._monitor_parent(FakeServer(), 12345)
        self.assertTrue(stopped.is_set())

    def test_session_capability_protects_health_and_does_not_leak(self):
        token = "a1" * 32
        with temporary_environment(ORCASLICER_AI_SESSION_TOKEN=token):
            with sidecar_server(PRODUCTION.Handler) as port:
                endpoint = f"http://127.0.0.1:{port}/health"
                with self.assertRaises(urllib.error.HTTPError) as missing:
                    urllib.request.urlopen(endpoint, timeout=5)
                self.assertEqual(missing.exception.code, 401)

                wrong = urllib.request.Request(endpoint, headers={"X-OrcaSlicer-Session": token})
                with self.assertRaises(urllib.error.HTTPError) as rejected:
                    urllib.request.urlopen(wrong, timeout=5)
                self.assertEqual(rejected.exception.code, 401)

                client_nonce = "c3" * 32
                challenge_request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/session-challenge",
                    headers={"X-OrcaSlicer-Client-Nonce": client_nonce},
                )
                with urllib.request.urlopen(challenge_request, timeout=5) as response:
                    challenge = json.loads(response.read())
                expected_server_proof = hmac.new(
                    token.encode("ascii"),
                    f"server:{client_nonce}:{challenge['server_nonce']}".encode("ascii"),
                    "sha256",
                ).hexdigest()
                self.assertTrue(hmac.compare_digest(challenge["server_proof"], expected_server_proof))
                session_proof = hmac.new(
                    token.encode("ascii"),
                    f"client:{challenge['server_nonce']}".encode("ascii"),
                    "sha256",
                ).hexdigest()
                accepted = urllib.request.Request(
                    endpoint, headers={"X-OrcaSlicer-Session-Proof": session_proof}
                )
                with urllib.request.urlopen(accepted, timeout=5) as response:
                    health = json.loads(response.read())

        self.assertTrue(health["runtime"]["session_protected"])
        self.assertNotIn(token, json.dumps(health))

    def test_installed_runtime_requires_nonempty_session_capability(self):
        with temporary_environment(
            ORCASLICER_AI_SESSION_TOKEN=None,
            ORCASLICER_AI_REQUIRE_SESSION="1",
        ):
            self.assertEqual(PRODUCTION.main(), 2)

    def test_installed_runtime_requires_parent_process_identity(self):
        with temporary_environment(
            ORCASLICER_AI_SESSION_TOKEN="b2" * 32,
            ORCASLICER_AI_REQUIRE_SESSION="1",
            ORCASLICER_AI_PARENT_PID=None,
        ):
            self.assertEqual(PRODUCTION.main(), 2)

    def test_installed_runtime_checks_parent_before_restoring_jobs(self):
        with temporary_environment(
            ORCASLICER_AI_SESSION_TOKEN="b3" * 32,
            ORCASLICER_AI_REQUIRE_SESSION="1",
            ORCASLICER_AI_PARENT_PID="12345",
        ), mock.patch.object(PRODUCTION.os, "name", "posix"), \
             mock.patch.object(PRODUCTION, "_parent_process_alive", return_value=False), \
             mock.patch.object(PRODUCTION, "_restore_jobs") as restore_jobs:
            self.assertEqual(PRODUCTION.main(), 2)
        restore_jobs.assert_not_called()

    def test_authenticated_shutdown_stops_the_loopback_server(self):
        token = "d4" * 32
        with temporary_environment(ORCASLICER_AI_SESSION_TOKEN=token):
            with sidecar_server(PRODUCTION.Handler) as port:
                endpoint = f"http://127.0.0.1:{port}/v1/orcaslicer/shutdown"
                missing = urllib.request.Request(
                    endpoint,
                    data=b"",
                    method="POST",
                    headers={"X-OrcaSlicer-Client": "native"},
                )
                with self.assertRaises(urllib.error.HTTPError) as rejected:
                    urllib.request.urlopen(missing, timeout=5)
                self.assertEqual(rejected.exception.code, 401)

                client_nonce = "e5" * 32
                challenge_request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/session-challenge",
                    headers={"X-OrcaSlicer-Client-Nonce": client_nonce},
                )
                with urllib.request.urlopen(challenge_request, timeout=5) as response:
                    challenge = json.loads(response.read())
                session_proof = hmac.new(
                    token.encode("ascii"),
                    f"client:{challenge['server_nonce']}".encode("ascii"),
                    "sha256",
                ).hexdigest()
                accepted = urllib.request.Request(
                    endpoint,
                    data=b"{}",
                    method="POST",
                    headers={
                        "Content-Type": "application/json",
                        "X-OrcaSlicer-Client": "native",
                        "X-OrcaSlicer-Session-Proof": session_proof,
                    },
                )
                with urllib.request.urlopen(accepted, timeout=5) as response:
                    payload = json.loads(response.read())
                self.assertEqual(response.status, 202)
                self.assertEqual(payload, {"ok": True, "state": "stopping"})

    def test_health_exposes_bounded_build_identity_for_support(self):
        with temporary_environment(
            ORCASLICER_AI_APP_VERSION="2.5.0-dev",
            ORCASLICER_AI_APP_COMMIT="c" * 40,
            ORCASLICER_AI_PACKAGE_REVISION="candidate-17",
            ORCASLICER_AI_DISTRIBUTION_CHANNEL="commercial",
            ORCASLICER_AI_SESSION_TOKEN="c1" * 32,
        ):
            health = self.fetch_health(PRODUCTION.Handler)

        self.assertEqual(health["runtime"]["health_schema_version"], 2)
        self.assertRegex(health["runtime"]["instance_id"], r"^[0-9a-f-]{36}$")
        self.assertEqual(health["runtime"]["build"], {
            "application_version": "2.5.0-dev",
            "application_commit": "c" * 40,
            "package_revision": "candidate-17",
            "distribution_channel": "commercial",
        })

    def test_style_recommendation_is_local_and_does_not_require_api_key(self):
        body, content_type = multipart_image_request(
            {"instruction": "一张宠物猫照片"}, valid_png_bytes()
        )
        with temporary_environment(OPENAI_API_KEY=None):
            with sidecar_server(PRODUCTION.Handler) as port:
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/model-style-recommendation",
                    data=body,
                    method="POST",
                    headers={"X-OrcaSlicer-Client": "native", "Content-Type": content_type},
                )
                with urllib.request.urlopen(request, timeout=5) as response:
                    payload = json.loads(response.read())

        recommendation = payload["recommendation"]
        self.assertEqual(recommendation["primary"], "cartoon")
        self.assertEqual(recommendation["alternatives"], ["sculpture", "low_poly"])
        self.assertTrue(recommendation["local_only"])

    def test_production_json_response_ignores_native_client_disconnect(self):
        handler = object.__new__(PRODUCTION.Handler)
        handler.send_response = mock.Mock()
        handler.send_header = mock.Mock()
        handler.end_headers = mock.Mock()
        handler.wfile = mock.Mock()
        handler.wfile.write.side_effect = ConnectionAbortedError(10053, "client cancelled obsolete poll")

        handler.send_json(200, {"job": {"state": "preprocessing"}})

        handler.wfile.write.assert_called_once()

    def test_local_journey_event_log_accepts_only_privacy_safe_fields(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory, temporary_environment(
            ORCASLICER_AI_OUTPUT_DIR=directory
        ):
            with sidecar_server(PRODUCTION.Handler) as port:
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/journey-events",
                    data=json.dumps({"event": "model_imported", "job_id": job_id}).encode(),
                    method="POST",
                    headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                )
                with urllib.request.urlopen(request, timeout=5) as response:
                    self.assertEqual(response.status, 201)

            records = [
                json.loads(line)
                for line in (Path(directory) / PRODUCTION.JOURNEY_EVENT_FILENAME).read_text(
                    encoding="ascii"
                ).splitlines()
            ]
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["event"], "model_imported")
        self.assertEqual(records[0]["job_id"], job_id)
        self.assertEqual(set(records[0]), {"version", "event", "job_id", "recorded_at"})

    def test_local_journey_event_log_rejects_content_and_paths(self):
        with tempfile.TemporaryDirectory() as directory, temporary_environment(
            ORCASLICER_AI_OUTPUT_DIR=directory
        ):
            with sidecar_server(PRODUCTION.Handler) as port:
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/journey-events",
                    data=json.dumps({
                        "event": "preview_requested",
                        "job_id": str(uuid.uuid4()),
                        "prompt": "private prompt",
                        "image_path": "C:/private/image.png",
                    }).encode(),
                    method="POST",
                    headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                )
                with self.assertRaises(urllib.error.HTTPError) as raised:
                    urllib.request.urlopen(request, timeout=5)
                self.assertEqual(raised.exception.code, 400)
            self.assertFalse((Path(directory) / PRODUCTION.JOURNEY_EVENT_FILENAME).exists())

    def test_design_work_uses_a_separate_executor_from_paid_model_generation(self):
        self.assertIs(PRODUCTION._executor_for(PRODUCTION._generate_job), PRODUCTION._MODEL_EXECUTOR)
        self.assertIs(PRODUCTION._executor_for(PRODUCTION._retexture_job), PRODUCTION._MODEL_EXECUTOR)
        self.assertIs(PRODUCTION._executor_for(PRODUCTION._recommend_palette_job), PRODUCTION._DESIGN_EXECUTOR)
        self.assertIs(PRODUCTION._executor_for(PRODUCTION._preprocess_image_job), PRODUCTION._DESIGN_EXECUTOR)
        self.assertIsNot(PRODUCTION._MODEL_EXECUTOR, PRODUCTION._DESIGN_EXECUTOR)

    def test_blocked_model_lane_does_not_starve_palette_lane(self):
        model_started = threading.Event()
        release_model = threading.Event()
        palette_finished = threading.Event()

        def blocking_model(_job):
            model_started.set()
            release_model.wait(timeout=5)

        def recommend_palette(_job):
            palette_finished.set()

        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            PRODUCTION, "_generate_job", blocking_model
        ):
            root = Path(directory)
            model_job = PRODUCTION.Job(id=str(uuid.uuid4()), source="text", directory=root / "model")
            palette_job = PRODUCTION.Job(id=str(uuid.uuid4()), source="text", directory=root / "palette")
            try:
                PRODUCTION._submit(model_job, blocking_model)
                self.assertTrue(model_started.wait(timeout=1))
                PRODUCTION._submit(palette_job, recommend_palette)
                self.assertTrue(palette_finished.wait(timeout=1))
            finally:
                release_model.set()
                if model_job.future is not None:
                    model_job.future.result(timeout=2)
                if palette_job.future is not None:
                    palette_job.future.result(timeout=2)

    def test_public_job_exposes_latest_structured_provider_failure(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.attempts = [
                {"attempt": 1, "status": "rejected", "error": "old quality failure"},
                {
                    "attempt": 2,
                    "status": "rejected",
                    "provider_error_code": "provider_timeout",
                    "provider_error_category": "availability",
                    "provider_error_retryable": True,
                    "provider_error_ambiguous": True,
                },
            ]

            public = PRODUCTION._public_job(job)

            self.assertEqual(public["provider_failure"]["code"], "provider_timeout")
            self.assertEqual(public["provider_failure"]["category"], "availability")
            self.assertTrue(public["provider_failure"]["retryable"])
            self.assertTrue(public["provider_failure"]["ambiguous"])
            job.attempts.append({"attempt": 3, "status": "rejected", "error": "quality gate"})
            self.assertEqual(PRODUCTION._public_job(job)["provider_failure"], {})

    def test_public_job_exposes_latest_paid_3d_task_ids(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory)
            job.attempts = [
                {
                    "attempt": 1,
                    "generation_task_id": "older-generation-task",
                    "conversion_task_id": "older-conversion-task",
                },
                {"attempt": 2, "status": "rejected", "error": "local quality gate"},
                {
                    "attempt": 3,
                    "generation_task_id": "final-generation-task",
                    "conversion_task_id": "final-conversion-task",
                    "status": "ready",
                },
            ]

            public = PRODUCTION._public_job(job)

            self.assertEqual(public["provider_tasks"], {
                "provider": "tripo",
                "generation_task_id": "final-generation-task",
                "conversion_task_id": "final-conversion-task",
            })

            job.attempts = []
            self.assertEqual(PRODUCTION._public_job(job)["provider_tasks"], {})

    def test_legacy_ready_job_status_recovers_provider_task_ids(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            job_directory = root / job_id
            job_directory.mkdir()
            (job_directory / "model-vertex-color.obj").write_text(
                "v 0 0 0 1 1 1\nf 1 1 1\n", encoding="ascii"
            )
            (job_directory / "attempts.json").write_text(
                json.dumps({
                    "attempts": [{
                        "attempt": 1,
                        "provider": "tripo",
                        "generation_task_id": "legacy-generation-task",
                        "conversion_task_id": "legacy-conversion-task",
                    }],
                }),
                encoding="utf-8",
            )
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
            try:
                with (
                    mock.patch.object(PRODUCTION, "_model_output_root", return_value=root),
                    sidecar_server(PRODUCTION.Handler) as port,
                ):
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job_id}",
                        headers={"X-OrcaSlicer-Client": "native"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        self.assertEqual(response.status, 200)
                        payload = json.loads(response.read())

                self.assertEqual(payload["job"]["provider_tasks"], {
                    "provider": "tripo",
                    "generation_task_id": "legacy-generation-task",
                    "conversion_task_id": "legacy-conversion-task",
                })
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def fetch_health(self, handler):
        with sidecar_server(handler) as port:
            headers = {}
            token = os.environ.get("ORCASLICER_AI_SESSION_TOKEN", "")
            if token:
                client_nonce = "d4" * 32
                challenge_request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/session-challenge",
                    headers={"X-OrcaSlicer-Client-Nonce": client_nonce},
                )
                with urllib.request.urlopen(challenge_request, timeout=5) as response:
                    challenge = json.loads(response.read())
                headers["X-OrcaSlicer-Session-Proof"] = hmac.new(
                    token.encode("ascii"),
                    f"client:{challenge['server_nonce']}".encode("ascii"),
                    "sha256",
                ).hexdigest()
            health_request = urllib.request.Request(
                f"http://127.0.0.1:{port}/health", headers=headers
            )
            with urllib.request.urlopen(health_request, timeout=5) as response:
                self.assertEqual(response.status, 200)
                return json.loads(response.read())

    def assert_contract(self, health):
        self.assertTrue(health["ok"])
        self.assertEqual(health["protocol_version"], 2)
        self.assertIsInstance(health["sidecar_version"], str)
        self.assertNotEqual(health["sidecar_version"], "")
        self.assertNotIn("features", health)

        capabilities = health["capabilities"]
        self.assertEqual(set(capabilities), {"config_proposal", "model_generation"})
        self.assertIsInstance(capabilities["config_proposal"]["available"], bool)

        generation = capabilities["model_generation"]
        self.assertIsInstance(generation["available"], bool)
        self.assertEqual(generation["sources"], ["text", "image"])
        self.assertEqual(
            generation["styles"],
            [
                "sculpture", "realistic", "portrait_sketch", "cartoon", "low_poly",
                "relief", "ink_relief", "diorama", "custom",
            ],
        )
        self.assertEqual(
            generation["style_recommendation"],
            {"available": True, "local_only": True},
        )
        self.assertEqual(generation["artifact_formats"], ["glb", "obj"])
        self.assertEqual(generation["face_limits"], [300000, 1000000, 2000000])
        self.assertEqual(generation["default_face_limit"], 1000000)
        self.assertEqual(generation["generation_profiles"], ["quality", "performance"])
        self.assertEqual(generation["default_generation_profile"], "quality")
        self.assertIn("model_reference", generation["printable_image_pipeline"]["outputs"])
        self.assertIsInstance(generation["palette_recommendation"]["available"], bool)
        if health["sidecar_version"] == PRODUCTION.SIDECAR_VERSION:
            self.assertFalse(generation["palette_recommendation"]["available"])
            self.assertFalse(generation["printable_image_pipeline"]["available"])
            self.assertNotIn("max_colors", generation["palette_recommendation"])
        self.assertEqual(set(generation["source_availability"]), {"text", "image"})
        self.assertIsInstance(generation["source_availability"]["text"], bool)
        self.assertIsInstance(generation["source_availability"]["image"], bool)
        image_provider = generation["image_provider"]
        self.assertEqual(
            set(image_provider),
            {"available", "source", "base_url", "endpoints", "automatic_retry", "max_billable_requests_per_action"},
        )
        self.assertEqual(set(image_provider["endpoints"]), {"generations", "edits"})
        self.assertFalse(image_provider["automatic_retry"])
        self.assertEqual(image_provider["max_billable_requests_per_action"], 1)

        payload = json.dumps(health)
        for secret in ("OPENAI_API_KEY", "TRIPO_API_KEY", "test-openai", "test-tripo"):
            self.assertNotIn(secret, payload)

    def test_custom_style_contract_is_explicit_and_bounded(self):
        self.assertEqual(
            PRODUCTION._normalize_custom_style("  粗线条木刻玩具  ", "custom"),
            "粗线条木刻玩具",
        )
        with self.assertRaisesRegex(PRODUCTION.RequestError, "required"):
            PRODUCTION._normalize_custom_style("", "custom")
        with self.assertRaisesRegex(PRODUCTION.RequestError, "only allowed"):
            PRODUCTION._normalize_custom_style("woodcut", "q_cartoon")
        with self.assertRaisesRegex(PRODUCTION.RequestError, "1000-byte"):
            PRODUCTION._normalize_custom_style("a" * 1001, "custom")

    def test_generation_profile_contract_has_only_two_public_values(self):
        self.assertEqual(PRODUCTION._normalize_generation_profile("quality"), "quality")
        self.assertEqual(PRODUCTION._normalize_generation_profile("performance"), "performance")
        with self.assertRaisesRegex(PRODUCTION.RequestError, "quality or performance"):
            PRODUCTION._normalize_generation_profile("turbo")

    def test_palette_policy_accepts_one_through_six_and_keeps_legacy_default(self):
        colors = [f"#{index:06X}" for index in range(1, 8)]
        for count in range(1, 7):
            with self.subTest(count=count):
                self.assertEqual(PRODUCTION._normalize_palette(colors[:count]), tuple(colors[:count]))
                self.assertEqual(PRODUCTION._normalize_palette_color_count(count), count)
                self.assertEqual(PRODUCTION._normalize_palette_color_count(str(count)), count)
        self.assertEqual(PRODUCTION._normalize_palette_color_count(None), 4)
        for invalid in (0, 7, True, "1.5"):
            with self.subTest(invalid=invalid), self.assertRaises(PRODUCTION.RequestError):
                PRODUCTION._normalize_palette_color_count(invalid)
        with self.assertRaises(PRODUCTION.RequestError):
            PRODUCTION._normalize_palette(colors)

    def test_custom_style_is_persisted_and_exposed_for_task_recovery(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(
                id=job_id,
                source="text",
                directory=job_directory,
                style="custom",
                custom_style="复古木刻版画",
            )
            PRODUCTION._persist_job(job)
            restored = PRODUCTION._load_job(job_directory)
            self.assertIsNotNone(restored)
            self.assertEqual(restored.custom_style, "复古木刻版画")
            self.assertEqual(restored.palette_color_count, 4)
            self.assertEqual(PRODUCTION._public_job(restored)["custom_style"], "复古木刻版画")

    def test_color_intent_is_persisted_exposed_and_downloadable(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            artifact = job_directory / "model-vertex-color.obj"
            artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="ascii")
            appearance = job_directory / "style-preview-raw.png"
            material = job_directory / "preview.png"
            geometry = job_directory / "geometry-reference.png"
            Image.new("RGB", (16, 16), "indianred").save(appearance)
            Image.new("RGB", (16, 16), "#E8DDD0").save(material)
            Image.new("RGB", (16, 16), "gray").save(geometry)
            job = PRODUCTION.Job(
                id=job_id,
                source="image",
                directory=job_directory,
                palette=("#E8DDD0",),
                palette_roles={"primary": "#E8DDD0"},
            )
            job.raw_preview_path = appearance
            job.preview_path = material
            job.model_reference_path = geometry
            job.artifact_path = artifact
            job.artifact_format = "obj"
            job.state = "ready"
            job.phase = "ready"

            PRODUCTION._write_job_color_intent(job, artifact)
            PRODUCTION._persist_job(job)
            restored = PRODUCTION._load_job(job_directory)

            self.assertIsNotNone(restored)
            self.assertTrue(restored.color_intent_path.is_file())
            public = PRODUCTION._public_job(restored)
            color_intent = public["artifact"]["color_intent"]
            self.assertTrue(color_intent["ready"])
            self.assertEqual(color_intent["schema"], PRODUCTION.COLOR_INTENT_SCHEMA)
            self.assertRegex(color_intent["sha256"], r"^[0-9a-f]{64}$")
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = restored
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/color-intent",
                        headers={"X-OrcaSlicer-Client": "native"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        downloaded = response.read()
                        self.assertEqual(response.headers.get_content_type(), "application/json")
                self.assertEqual(downloaded, restored.color_intent_path.read_bytes())
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_corrupt_color_intent_is_ignored_without_hiding_legacy_artifact(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            artifact = job_directory / "model-vertex-color.obj"
            artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="ascii")
            manifest = job_directory / "color-intent.v1.json"
            manifest.write_text('{"schema":"corrupt"}', encoding="utf-8")
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "ready"
            job.artifact_path = artifact
            job.artifact_format = "obj"
            job.color_intent_path = manifest
            job.color_intent_schema = PRODUCTION.COLOR_INTENT_SCHEMA
            job.color_intent_sha256 = "0" * 64
            PRODUCTION._persist_job(job)

            restored = PRODUCTION._load_job(job_directory)

        self.assertIsNotNone(restored)
        self.assertIsNotNone(restored.artifact_path)
        self.assertIsNone(restored.color_intent_path)
        self.assertFalse(PRODUCTION._public_job(restored)["artifact"]["color_intent"]["ready"])

    def test_palette_recommendation_is_persisted_and_exposed_for_task_recovery(self):
        job_id = str(uuid.uuid4())
        recommendation = {
            "summary": "温暖主体配合深色结构",
            "colors": [
                {"hex": "#D96B43", "name": "陶土橙", "role": "primary", "usage": "主体", "reason": "视觉中心"},
                {"hex": "#2B2422", "name": "深棕", "role": "structure", "usage": "结构", "reason": "稳定轮廓"},
                {"hex": "#F2D7B5", "name": "暖白", "role": "light", "usage": "高光", "reason": "明暗层次"},
                {"hex": "#2F6B5F", "name": "墨绿", "role": "accent", "usage": "点缀", "reason": "冷暖对比"},
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "awaiting_palette_confirmation"
            job.phase = "awaiting_palette_confirmation"
            job.palette = ("#D96B43", "#2B2422", "#F2D7B5")
            job.palette_recommendation = recommendation
            PRODUCTION._persist_job(job)
            state_path = job_directory / PRODUCTION.JOB_STATE_FILENAME
            persisted = json.loads(state_path.read_text(encoding="utf-8"))
            persisted.pop("palette_color_count")
            state_path.write_text(json.dumps(persisted), encoding="utf-8")

            restored = PRODUCTION._load_job(job_directory)

            self.assertIsNotNone(restored)
            self.assertEqual(restored.palette_color_count, 4)
            self.assertEqual(restored.palette_recommendation, recommendation)
            public = PRODUCTION._public_job(restored)
            self.assertEqual(public["palette_recommendation"], recommendation)
            self.assertFalse(public["palette_recommendation_confirmed"])

    def test_legacy_recommendation_worker_preprocesses_without_recommending_colors(self):
        with tempfile.TemporaryDirectory() as directory:
            job = PRODUCTION.Job(id=str(uuid.uuid4()), source="image", directory=Path(directory), user_prompt="portrait")
            job.input_path = Path(directory)/"input.png"
            job.input_path.write_bytes(valid_png_bytes())
            with mock.patch.object(PRODUCTION, "_preprocess_image_job") as preprocess, mock.patch.object(PRODUCTION, "recommend_printable_palette") as recommend:
                PRODUCTION._recommend_palette_job(job)
            preprocess.assert_called_once_with(job, job.input_path, "portrait")
            recommend.assert_not_called()
            self.assertEqual(job.palette, ())

    def test_legacy_recommendation_routes_create_unrestricted_design_jobs(self):
        for source in ("text", "image"):
            with self.subTest(source=source), tempfile.TemporaryDirectory() as directory, temporary_environment(OPENAI_API_KEY="test-openai", ORCASLICER_AI_OUTPUT_DIR=directory), mock.patch.object(PRODUCTION, "_submit") as submit, mock.patch.object(PRODUCTION, "image_provider_status", return_value={"available": True}), mock.patch.dict(PRODUCTION._JOBS, {}, clear=True), sidecar_server(PRODUCTION.Handler) as port:
                fields = {"request_id": "legacy", "style": "realistic", "palette_color_count": "20", "provider": "hunyuan"}
                if source == "image":
                    fields["instruction"] = "portrait"
                    body, content_type = multipart_image_request(fields, valid_png_bytes())
                else:
                    fields["prompt"] = "portrait"
                    body, content_type = json.dumps(fields).encode(), "application/json"
                request = urllib.request.Request(f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/recommend-{source}-palette", data=body, headers={"X-OrcaSlicer-Client":"native", "Content-Type":content_type})
                with urllib.request.urlopen(request, timeout=5) as response:
                    public = json.loads(response.read())["job"]
                job = PRODUCTION._JOBS[public["id"]]
                self.assertEqual(job.palette, ())
                self.assertEqual(job.palette_recommendation, {})
                self.assertEqual(public["state"], "preprocessing")
                self.assertEqual(public["provider"], "hunyuan")
                self.assertEqual(PRODUCTION._load_job(job.directory).provider, "hunyuan")
                self.assertIs(submit.call_args.args[1], PRODUCTION._preprocess_image_job if source == "image" else PRODUCTION._preprocess_text_job)
                if source == "image": self.assertEqual(job.input_path.read_bytes(), valid_png_bytes())

    def test_new_design_routes_preserve_all_generation_options(self):
        cases = (
            {"provider": "hunyuan", "face_limit": 1000000, "geometry_quality": "standard", "texture_quality": "standard", "output_format": "obj"},
            {"provider": "tripo", "face_limit": 2000000, "geometry_quality": "detailed", "texture_quality": "extreme", "output_format": "obj"},
            {},
        )
        for route in ("text", "image", "recommend-text-palette", "recommend-image-palette"):
            for options in cases:
                with self.subTest(route=route, options=options), tempfile.TemporaryDirectory() as directory, \
                     mock.patch.object(PRODUCTION, "_model_output_root", return_value=Path(directory)), \
                     mock.patch.dict(PRODUCTION._JOBS, {}, clear=True), \
                     mock.patch.object(PRODUCTION, "image_provider_status", return_value={"available": True}), \
                     mock.patch.object(PRODUCTION, "_submit") as submit, sidecar_server(PRODUCTION.Handler) as port:
                    fields = {"request_id": "offline-options", "style": "realistic", **options}
                    if "image" in route:
                        fields["instruction"] = "test object"
                        body, content_type = multipart_image_request({key: str(value) for key, value in fields.items()}, valid_png_bytes())
                    else:
                        fields["prompt"] = "test object"
                        body, content_type = json.dumps(fields).encode(), "application/json"
                    request = urllib.request.Request(f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{route}", data=body,
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": content_type})
                    with urllib.request.urlopen(request, timeout=5) as response:
                        public = json.loads(response.read())["job"]
                    restored = PRODUCTION._load_job(PRODUCTION._JOBS[public["id"]].directory)
                    expected = {"provider": "tripo", "face_limit": PRODUCTION.DEFAULT_MODEL_FACE_LIMIT,
                                "geometry_quality": None, "texture_quality": "standard", "output_format": "glb", **options}
                    for key, value in expected.items():
                        self.assertEqual(public[key], value)
                        self.assertEqual(getattr(restored, key), value)
                    self.assertEqual(restored.attempts, [])
                    self.assertEqual(restored.generation_profile, "quality" if expected["face_limit"] >= 500000 else "performance")
                    submit.assert_called_once()
                    self.assertIs(submit.call_args.args[1], PRODUCTION._preprocess_image_job if "image" in route else PRODUCTION._preprocess_text_job)

    def test_generation_options_http_saves_provider_without_submitting(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(PRODUCTION, "_model_output_root", return_value=Path(directory)), \
             mock.patch.dict(PRODUCTION._JOBS, {}, clear=True), \
             mock.patch.object(PRODUCTION, "_submit") as submit, sidecar_server(PRODUCTION.Handler) as port:
            job = PRODUCTION._new_job("text", ())
            job.state = "awaiting_confirmation"
            PRODUCTION._JOBS[job.id] = job
            options = {"provider": "hunyuan", "face_limit": 300000, "geometry_quality": "standard",
                       "texture_quality": "standard", "output_format": "glb"}
            request = urllib.request.Request(
                f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/generation-options",
                data=json.dumps(options).encode(), headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=5) as response:
                public = json.loads(response.read())["job"]
            self.assertEqual(public["provider"], "hunyuan")
            self.assertEqual(PRODUCTION._load_job(job.directory).provider, "hunyuan")
            self.assertEqual(job.attempts, [])
            submit.assert_not_called()

    def test_legacy_palette_confirmation_continues_same_job_and_recovers_scheduling_failure(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(PRODUCTION._JOBS, {}, clear=True):
            job = PRODUCTION.Job(id=str(uuid.uuid4()), source="text", directory=Path(directory), user_prompt="portrait")
            job.state = "awaiting_palette_confirmation"
            job.palette_recommendation = {"colors": [{"hex": "#FF0000"}]}
            job.palette = ("#FF0000",)
            PRODUCTION._JOBS[job.id] = job
            handler = PRODUCTION.Handler.__new__(PRODUCTION.Handler)
            handler._read_model_json = mock.Mock(return_value={"palette": ["#FF0000"]})
            handler.send_json = mock.Mock()
            with mock.patch.object(PRODUCTION, "_submit", side_effect=PRODUCTION.RequestError("queue_full", "busy", 503)):
                with self.assertRaises(PRODUCTION.RequestError): handler._confirm_palette(job.id)
            self.assertEqual(job.state, "awaiting_palette_confirmation")
            self.assertTrue(job.palette_recommendation)
            with mock.patch.object(PRODUCTION, "_submit") as submit:
                handler._confirm_palette(job.id)
            self.assertIs(submit.call_args.args[0], job)
            self.assertEqual(job.state, "preprocessing")
            self.assertEqual(job.palette, ())
            self.assertEqual(job.palette_recommendation, {})

    def test_image_preview_route_persists_confirmed_ai_palette_origin(self):
        boundary = "----OrcaPreviewPaletteOriginTest"
        parts = []
        for name, value in {
            "request_id": "preview-r1",
            "instruction": "保留主体",
            "palette": '["#E8DDD0", "#1A1A1A", "#1F5A45"]',
            "palette_roles": "{}",
            "palette_recommendation_confirmed": "true",
            "style": "realistic",
            "custom_style": "",
            "print": "{}",
        }.items():
            parts.append(
                f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n{value}\r\n".encode("utf-8")
            )
        parts.append(
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"image\"; filename=\"input.png\"\r\n"
            "Content-Type: image/png\r\n\r\n".encode("ascii")
            + valid_png_bytes()
            + b"\r\n"
        )
        parts.append(f"--{boundary}--\r\n".encode("ascii"))
        with tempfile.TemporaryDirectory() as directory, temporary_environment(
            OPENAI_API_KEY="test-openai", ORCASLICER_AI_OUTPUT_DIR=directory
        ), mock.patch.object(PRODUCTION, "_preprocess_image_job"):
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/image",
                        data=b"".join(parts),
                        method="POST",
                        headers={
                            "X-OrcaSlicer-Client": "native",
                            "Content-Type": f"multipart/form-data; boundary={boundary}",
                        },
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        public = json.loads(response.read())["job"]
                self.assertTrue(public["palette_recommendation_confirmed"])
                persisted = PRODUCTION._load_job(Path(directory) / public["id"])
                self.assertIsNotNone(persisted)
                self.assertTrue(persisted.palette_recommendation_confirmed)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_confirm_palette_rejects_wrong_job_state(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory, state="awaiting_confirmation")
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job_id] = job
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job_id}/confirm-palette",
                        data=json.dumps({"palette": ["#D96B43"]}).encode(),
                        method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with self.assertRaises(urllib.error.HTTPError) as error:
                        urllib.request.urlopen(request, timeout=5)
                self.assertEqual(error.exception.code, 409)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_latest_job_endpoint_returns_persisted_job_without_secrets(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            job.user_prompt = "printable object"
            job.prepared_prompt = "printable object"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/latest",
                        headers={"X-OrcaSlicer-Client": "native"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        payload = json.loads(response.read())
                self.assertEqual(payload["job"]["id"], job.id)
                self.assertEqual(payload["job"]["prepared_prompt"], "printable object")
                self.assertNotIn("API_KEY", json.dumps(payload))
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_latest_job_ignores_newer_prepared_only_internal_text_job(self):
        restorable_id = str(uuid.uuid4())
        internal_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            restorable_directory = root / restorable_id
            internal_directory = root / internal_id
            restorable_directory.mkdir()
            internal_directory.mkdir()
            restorable = PRODUCTION.Job(id=restorable_id, source="text", directory=restorable_directory)
            restorable.state = "awaiting_confirmation"
            restorable.user_prompt = "用户输入的雕塑"
            restorable.prepared_prompt = "prepared user prompt"
            restorable.updated_at = 10.0
            internal = PRODUCTION.Job(id=internal_id, source="text", directory=internal_directory)
            internal.state = "awaiting_confirmation"
            internal.user_prompt = ""
            internal.prepared_prompt = "internal calibration fixture"
            internal.updated_at = 20.0
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[restorable.id] = restorable
                PRODUCTION._JOBS[internal.id] = internal
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/latest",
                        headers={"X-OrcaSlicer-Client": "native"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        payload = json.loads(response.read())
                self.assertEqual(payload["job"]["id"], restorable.id)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_legacy_realistic_portrait_with_severe_material_islands_is_blocked_on_restore(self):
        with tempfile.TemporaryDirectory() as directory:
            job = PRODUCTION.Job(
                id=str(uuid.uuid4()),
                source="image",
                directory=Path(directory),
                palette=("#F2C7A5", "#1C1A1B", "#F7F6F2", "#4F6B5A"),
                style="realistic",
            )
            job.image_metrics = {
                "palette_quality_ok": True,
                "subject_color_component_count": {
                    "#F2C7A5": 20,
                    "#1C1A1B": 19,
                    "#F7F6F2": 661,
                    "#4F6B5A": 23,
                },
                "secondary_subject_color_component_ratio": {
                    "#F2C7A5": 0.16,
                    "#1C1A1B": 0.07,
                    "#F7F6F2": 0.11,
                    "#4F6B5A": 0.04,
                },
                "quality_warnings": ["palette_material_is_fragmented"],
            }

            PRODUCTION._apply_legacy_material_fragmentation_gate(job)

        self.assertFalse(job.image_metrics["material_fragmentation_ok"])
        self.assertFalse(job.image_metrics["palette_quality_ok"])
        self.assertIn("portrait_material_fragmentation_blocks_3d", job.image_metrics["quality_warnings"])

    def test_generate_route_passes_one_shot_authorization_after_validation(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            gateway = mock.Mock()
            gateway.model_generation_available.return_value = True
            try:
                with (
                    mock.patch.object(PRODUCTION, "_MODEL_PROVIDER_GATEWAY", gateway),
                    mock.patch.object(PRODUCTION, "_submit") as submit,
                    sidecar_server(PRODUCTION.Handler) as port,
                ):
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/generate",
                        data=json.dumps(
                            {"prepared_prompt": "printable object", "palette": [], "generation_profile": "quality"}
                        ).encode(),
                        method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        payload = json.loads(response.read())

                self.assertEqual(payload["job"]["state"], "queued")
                self.assertEqual(payload["job"]["generation_profile"], "quality")
                self.assertEqual(payload["job"]["face_limit"], 1000000)
                submit.assert_called_once()
                args = submit.call_args.args
                self.assertIs(args[0], job)
                self.assertIs(args[1], PRODUCTION._generate_job)
                self.assertEqual(args[2:4], ("printable object", False))
                authorization = args[4]
                self.assertEqual(authorization.request_id, f"{job.id}:model:1")
                self.assertFalse(authorization.consumed)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_retexture_route_creates_child_job_and_one_scoped_paid_task(self):
        reference_id = str(uuid.uuid4())
        geometry_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory, temporary_environment(
            ORCASLICER_AI_OUTPUT_DIR=directory
        ):
            root = Path(directory)
            reference_directory = root / reference_id
            geometry_directory = root / geometry_id
            reference_directory.mkdir()
            geometry_directory.mkdir()
            reference_image = reference_directory / "model-reference.png"
            Image.new("RGB", (512, 512), "white").save(reference_image)
            geometry_artifact = geometry_directory / "model-vertex-color.obj"
            geometry_artifact.write_text("v 0 0 0 1 1 1\nf 1 1 1\n", encoding="ascii")
            reference_job = PRODUCTION.Job(
                id=reference_id,
                source="image",
                directory=reference_directory,
                palette=("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B"),
                palette_roles={
                    "primary": "#F4F4F0",
                    "structure": "#1F1B1C",
                    "light": "#F2C9AE",
                    "accent": "#4E6F5B",
                },
                style="realistic",
                palette_color_count=6,
            )
            reference_job.state = "awaiting_confirmation"
            reference_job.phase = "awaiting_confirmation"
            reference_job.model_reference_path = reference_image
            reference_job.image_metrics = {"palette_quality_ok": True}
            geometry_job = PRODUCTION.Job(id=geometry_id, source="image", directory=geometry_directory)
            geometry_job.state = "ready"
            geometry_job.phase = "ready"
            geometry_job.artifact_path = geometry_artifact
            geometry_job.artifact_format = "obj"
            geometry_job.face_limit = 1000000
            geometry_job.generation_profile = "quality"
            geometry_job.attempts = [{
                "attempt": 1,
                "provider": "tripo",
                "provider_operation": "model_generation",
                "generation_task_id": "provider-geometry-task",
                "status": "accepted",
            }]
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[reference_id] = reference_job
                PRODUCTION._JOBS[geometry_id] = geometry_job
            gateway = mock.Mock()
            gateway.model_generation_available.return_value = True
            try:
                with (
                    mock.patch.object(PRODUCTION, "_MODEL_PROVIDER_GATEWAY", gateway),
                    mock.patch.object(PRODUCTION, "_validate_image_file"),
                    mock.patch.object(PRODUCTION, "_submit") as submit,
                    sidecar_server(PRODUCTION.Handler) as port,
                ):
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{reference_id}/retexture",
                        data=json.dumps({"geometry_job_id": geometry_id}).encode(),
                        method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        self.assertEqual(response.status, 202)
                        payload = json.loads(response.read())

                child = PRODUCTION._JOBS[payload["job"]["id"]]
                self.assertEqual(child.state, "queued")
                self.assertEqual(child.model_reference_path.read_bytes(), reference_image.read_bytes())
                self.assertEqual(child.face_limit, 1000000)
                self.assertEqual(child.palette_color_count, 6)
                self.assertEqual(child.attempts[0]["source_job_id"], geometry_id)
                self.assertEqual(child.attempts[0]["source_task_id"], "provider-geometry-task")
                submit.assert_called_once()
                args = submit.call_args.args
                self.assertIs(args[0], child)
                self.assertIs(args[1], PRODUCTION._retexture_job)
                self.assertEqual(args[2:5], (geometry_id, "provider-geometry-task", False))
                self.assertEqual(args[5].operation, "model_texture")
                self.assertFalse(args[5].consumed)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_generate_route_keeps_poor_reference_advice_and_submits_once(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            reference = job_directory / "preview.png"
            image = Image.new("RGBA", (512, 512), (255, 255, 255, 0))
            image.paste((200, 60, 40, 255), (246, 246, 266, 266))
            image.save(reference)
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory)
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            job.preview_path = reference
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            gateway = mock.Mock()
            gateway.model_generation_available.return_value = True
            try:
                with (
                    mock.patch.object(PRODUCTION, "_MODEL_PROVIDER_GATEWAY", gateway),
                    mock.patch.object(PRODUCTION, "_submit") as submit,
                    sidecar_server(PRODUCTION.Handler) as port,
                ):
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/generate",
                        data=json.dumps(
                            {"prepared_prompt": "", "palette": [], "generation_profile": "quality"}
                        ).encode(),
                        method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        self.assertEqual(response.status, 200)
                        payload = json.load(response)
                    with self.assertRaises(urllib.error.HTTPError) as duplicate:
                        urllib.request.urlopen(request, timeout=5)
                    self.assertEqual(duplicate.exception.code, 409)

                self.assertEqual(payload["job"]["state"], "queued")
                submit.assert_called_once()
                self.assertFalse(job.image_metrics["model_input_quality"]["model_input_eligible"])
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_generate_route_allows_small_blank_and_alpha_images_with_advice(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(PRODUCTION._JOBS, {}, clear=True):
            gateway = mock.Mock()
            gateway.model_generation_available.return_value = True
            with (
                mock.patch.object(PRODUCTION, "_MODEL_PROVIDER_GATEWAY", gateway),
                mock.patch.object(PRODUCTION, "_submit") as submit,
                sidecar_server(PRODUCTION.Handler) as port,
            ):
                for mode, size, color in (("RGB", 32, "white"), ("RGB", 512, "white"), ("RGBA", 1, (0, 0, 0, 0))):
                    with self.subTest(mode=mode, size=size):
                        root = Path(directory) / str(uuid.uuid4())
                        root.mkdir()
                        reference = root / "preview.png"
                        Image.new(mode, (size, size), color).save(reference)
                        job = PRODUCTION.Job(id=root.name, source="image", directory=root)
                        job.state = job.phase = "awaiting_confirmation"
                        job.preview_path = reference
                        PRODUCTION._JOBS[job.id] = job
                        request = urllib.request.Request(
                            f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/generate",
                            data=json.dumps({"prepared_prompt": "", "palette": [], "generation_profile": "quality"}).encode(),
                            method="POST", headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                        )
                        with urllib.request.urlopen(request, timeout=5) as response:
                            self.assertEqual(response.status, 200)
                        self.assertEqual(job.state, "queued")
                        advice = job.image_metrics["model_input_quality"]
                        self.assertTrue(advice["blockers"] or advice["warnings"])
                        if size < 256:
                            self.assertIn("low_resolution", advice["warnings"])
                        with self.assertRaises(urllib.error.HTTPError) as duplicate:
                            urllib.request.urlopen(request, timeout=5)
                        self.assertEqual(duplicate.exception.code, 409)
                self.assertEqual(submit.call_count, 3)

    def test_restored_opaque_design_rechecks_uncertain_silhouette_and_submits_once(self):
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / str(uuid.uuid4())
            job_directory.mkdir()
            reference = job_directory / "style-preview-raw.png"
            # One connected pale sculpture. Color thresholding loses its white
            # body and incorrectly separates the head, shirt and dark base.
            image = Image.new("RGB", (512, 768), "white")
            image.paste((225, 165, 135), (180, 60, 332, 230))
            image.paste((248, 248, 248), (120, 220, 392, 650))
            image.paste((45, 75, 60), (200, 285, 312, 485))
            image.paste((55, 55, 60), (110, 645, 402, 720))
            image.save(reference)
            original_bytes = reference.read_bytes()
            job = PRODUCTION.Job(id=job_directory.name, source="image", directory=job_directory)
            job.state = job.phase = "awaiting_confirmation"
            job.input_path = job.raw_preview_path = job.preview_path = reference
            job.image_metrics = {
                "design_reference": "ai-design-v1",
                "model_input_quality": {"model_input_eligible": False, "blockers": ["fragmented_subject"]},
                "generation_input_quality": {"model_input_eligible": False, "blockers": ["fragmented_subject"]},
            }
            job.message = PRODUCTION._model_input_quality_message(job.image_metrics["model_input_quality"])
            stale_message = job.message
            PRODUCTION._persist_job(job)
            gateway = mock.Mock()
            gateway.model_generation_available.return_value = True
            with (
                mock.patch.dict(PRODUCTION._JOBS, {}, clear=True),
                mock.patch.object(PRODUCTION, "_model_output_root", return_value=Path(directory)),
                mock.patch.object(PRODUCTION, "_MODEL_PROVIDER_GATEWAY", gateway),
                mock.patch.object(PRODUCTION, "_submit") as submit,
                sidecar_server(PRODUCTION.Handler) as port,
            ):
                restored = PRODUCTION._restore_jobs()
                self.assertEqual(len(restored), 1)
                restored_job = restored[0]
                self.assertEqual(restored_job.state, "awaiting_confirmation")
                self.assertNotEqual(restored_job.message, stale_message)
                for quality_key in ("model_input_quality", "generation_input_quality"):
                    quality = restored_job.image_metrics[quality_key]
                    self.assertTrue(quality["model_input_eligible"], quality)
                    self.assertIn("foreground_segmentation_uncertain", quality["warnings"])
                submit.assert_not_called()
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/generate",
                    data=json.dumps({"prepared_prompt": "", "palette": [], "generation_profile": "quality"}).encode(),
                    method="POST",
                    headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                )
                with urllib.request.urlopen(request, timeout=5) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.loads(response.read())["job"]["state"], "queued")
                with self.assertRaises(urllib.error.HTTPError) as duplicate:
                    urllib.request.urlopen(request, timeout=5)
                self.assertEqual(duplicate.exception.code, 409)
                submit.assert_called_once()
                self.assertIs(PRODUCTION._geometry_generation_reference(restored_job), restored_job.raw_preview_path)
                gateway.create_image_task.assert_not_called()
                self.assertEqual(reference.read_bytes(), original_bytes)

    def test_generate_route_ignores_retired_palette_fragmentation_gate(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            reference = job_directory / "preview.png"
            Image.new("RGB", (512, 512), (225, 155, 115)).save(reference)
            job = PRODUCTION.Job(
                id=job_id,
                source="image",
                directory=job_directory,
                palette=("#F2C7A5", "#1C1A1B", "#F7F6F2", "#4F6B5A"),
            )
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            job.preview_path = reference
            job.raw_preview_path = reference
            job.model_reference_path = reference
            job.image_metrics = {
                "palette_quality_ok": False,
                "material_fragmentation_ok": False,
                "printable_subject_area_ratio": 0.5,
                "largest_subject_component_ratio": 1.0,
                "largest_detached_subject_diagonal_ratio": 0.0,
            }
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            gateway = mock.Mock()
            gateway.model_generation_available.return_value = True
            eligible = {"model_input_eligible": True, "blockers": [], "warnings": []}
            try:
                with (
                    mock.patch.object(PRODUCTION, "_MODEL_PROVIDER_GATEWAY", gateway),
                    mock.patch.object(PRODUCTION, "_validate_image_file"),
                    mock.patch.object(PRODUCTION, "_assess_job_model_reference", return_value=eligible),
                    mock.patch.object(PRODUCTION, "_assess_job_generation_reference", return_value=eligible),
                    mock.patch.object(PRODUCTION, "_submit") as submit,
                    sidecar_server(PRODUCTION.Handler) as port,
                ):
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/generate",
                        data=json.dumps(
                            {
                                "prepared_prompt": "",
                                "palette": list(job.palette),
                                "generation_profile": "quality",
                            }
                        ).encode(),
                        method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        payload = json.loads(response.read())
                self.assertEqual(payload["job"]["state"], "queued")
                self.assertEqual(job.palette, ())
                submit.assert_called_once()
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_public_job_exposes_persisted_model_quality_report(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            quality = {
                "schema_version": 1,
                "gate_version": "structural-v1",
                "status": "review",
                "errors": [],
                "warnings": ["tiny_detached_components"],
                "metrics": {"component_count": 3},
            }
            (job_directory / PRODUCTION.MODEL_QUALITY_FILENAME).write_text(
                json.dumps(quality), encoding="utf-8"
            )
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory)

            public = PRODUCTION._public_job(job)

            self.assertEqual(public["model_quality"], quality)

    def test_public_job_exposes_persisted_visual_quality_and_view_sheet(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            visual = {
                "schema_version": 1,
                "review_version": "visual-v1",
                "status": "review",
                "score": 72,
                "warnings": ["visual_silhouette_unclear"],
            }
            (job_directory / PRODUCTION.VISUAL_QUALITY_FILENAME).write_text(json.dumps(visual), encoding="utf-8")
            (job_directory / "model-view-sheet.png").write_bytes(b"\x89PNG\r\n\x1a\nexample")
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory)

            public = PRODUCTION._public_job(job)

            self.assertEqual(public["visual_quality"], visual)
            self.assertTrue(public["model_views"]["ready"])

    def test_public_job_exposes_model_refinement_advice(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            quality = {
                "schema_version": 1,
                "gate_version": "structural-v9",
                "status": "review",
                "errors": [],
                "warnings": ["thin_local_wall_regions", "localized_overhang_regions"],
            }
            visual = {
                "schema_version": 1,
                "review_version": "visual-v1",
                "status": "review",
                "warnings": ["visual_color_regions_unclear"],
            }
            (job_directory / PRODUCTION.MODEL_QUALITY_FILENAME).write_text(
                json.dumps(quality), encoding="utf-8"
            )
            (job_directory / PRODUCTION.VISUAL_QUALITY_FILENAME).write_text(
                json.dumps(visual), encoding="utf-8"
            )
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory)

            public = PRODUCTION._public_job(job)

            self.assertTrue(public["refinement"]["available"])
            self.assertEqual(
                [issue["code"] for issue in public["refinement"]["issues"]],
                ["thin_local_wall_regions", "localized_overhang_regions", "visual_color_regions_unclear"],
            )
            self.assertIn("打印优化要求", public["refinement"]["prompt_suffix"])

    def test_public_job_refinement_is_unavailable_for_passing_quality(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            quality = {
                "schema_version": 1,
                "gate_version": "structural-v9",
                "status": "pass",
                "errors": [],
                "warnings": [],
            }
            (job_directory / PRODUCTION.MODEL_QUALITY_FILENAME).write_text(
                json.dumps(quality), encoding="utf-8"
            )
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)

            public = PRODUCTION._public_job(job)

            self.assertFalse(public["refinement"]["available"])
            self.assertEqual(public["refinement"]["issues"], [])

    def test_job_status_endpoint_returns_registered_job(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}",
                        headers={"X-OrcaSlicer-Client": "native"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        payload = json.loads(response.read())
                self.assertEqual(payload["job"]["id"], job.id)
                self.assertEqual(payload["job"]["state"], "awaiting_confirmation")
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_image_job_input_can_be_downloaded_for_native_recovery(self):
        job_id = str(uuid.uuid4())
        input_bytes = b"\x89PNG\r\n\x1a\nrestored-input"
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            input_path = job_directory / "input.png"
            input_path.write_bytes(input_bytes)
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory)
            job.input_path = input_path
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/input",
                        headers={"X-OrcaSlicer-Client": "native"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        self.assertEqual(response.status, 200)
                        self.assertEqual(response.headers.get_content_type(), "image/png")
                        self.assertEqual(response.read(), input_bytes)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_recheck_analyzes_only_the_registered_obj_without_paid_calls(self):
        job_id = str(uuid.uuid4())
        tetrahedron = (
            "v 0 0 0 1 0 0\n"
            "v 10 0 0 1 0 0\n"
            "v 0 10 0 1 0 0\n"
            "v 0 0 10 1 0 0\n"
            "f 1 3 2\n"
            "f 1 2 4\n"
            "f 1 4 3\n"
            "f 2 3 4\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            artifact = job_directory / "model-vertex-color.obj"
            artifact.write_text(tetrahedron, encoding="ascii")
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory)
            job.state = "ready"
            job.phase = "ready"
            job.palette = ("#FF0000",)
            job.artifact_path = artifact
            job.artifact_format = "obj"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            try:
                gateway = mock.Mock()
                with mock.patch.object(PRODUCTION, "_MODEL_PROVIDER_GATEWAY", gateway):
                    with sidecar_server(PRODUCTION.Handler) as port:
                        request = urllib.request.Request(
                            f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/recheck",
                            data=b"{}",
                            method="POST",
                            headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                        )
                        with urllib.request.urlopen(request, timeout=5) as response:
                            payload = json.loads(response.read())
                    self.assertEqual(payload["job"]["model_quality"]["status"], "pass")
                    metrics = payload["job"]["model_quality"]["metrics"]
                    self.assertTrue(metrics["target_palette_metrics_available"])
                    self.assertEqual(metrics["target_palette_color_count"], 1)
                    self.assertEqual(metrics["meaningful_target_palette_color_count"], 1)
                    self.assertAlmostEqual(metrics["target_palette_surface_coverage_ratio"], 1.0)
                    self.assertTrue((job_directory / PRODUCTION.MODEL_QUALITY_FILENAME).is_file())
                    gateway.start_or_reuse_model_task.assert_not_called()
                    gateway.start_or_reuse_conversion.assert_not_called()
                    gateway.wait_for_task.assert_not_called()
                    gateway.download_artifact.assert_not_called()
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_recheck_rejects_an_artifact_outside_the_registered_job_directory(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            job_directory = root / job_id
            job_directory.mkdir()
            artifact = root / "outside.obj"
            artifact.write_text("v 0 0 0\n", encoding="ascii")
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "ready"
            job.phase = "ready"
            job.artifact_path = artifact
            job.artifact_format = "obj"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/recheck",
                        data=b"{}",
                        method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with self.assertRaises(urllib.error.HTTPError) as raised:
                        urllib.request.urlopen(request, timeout=5)
                self.assertEqual(raised.exception.code, 409)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_recheck_adopts_a_legacy_model_library_entry_without_paid_calls(self):
        job_id = str(uuid.uuid4())
        tetrahedron = (
            "v 0 0 0 1 0 0\n"
            "v 10 0 0 1 0 0\n"
            "v 0 10 0 1 0 0\n"
            "v 0 0 10 1 0 0\n"
            "f 1 3 2\n"
            "f 1 2 4\n"
            "f 1 4 3\n"
            "f 2 3 4\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            job_directory = root / job_id
            job_directory.mkdir()
            (job_directory / "model-vertex-color.obj").write_text(tetrahedron, encoding="ascii")
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
            try:
                gateway = mock.Mock()
                with temporary_environment(ORCASLICER_AI_OUTPUT_DIR=str(root)):
                    with mock.patch.object(PRODUCTION, "_MODEL_PROVIDER_GATEWAY", gateway):
                        with sidecar_server(PRODUCTION.Handler) as port:
                            request = urllib.request.Request(
                                f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job_id}/recheck",
                                data=b"{}",
                                method="POST",
                                headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                            )
                            with urllib.request.urlopen(request, timeout=5) as response:
                                payload = json.loads(response.read())
                        self.assertEqual(payload["job"]["state"], "ready")
                        self.assertEqual(payload["job"]["model_quality"]["status"], "pass")
                        self.assertTrue((job_directory / PRODUCTION.MODEL_QUALITY_FILENAME).is_file())
                        self.assertTrue((job_directory / PRODUCTION.JOB_STATE_FILENAME).is_file())
                        gateway.start_or_reuse_model_task.assert_not_called()
                        gateway.start_or_reuse_conversion.assert_not_called()
                        gateway.wait_for_task.assert_not_called()
                        gateway.download_artifact.assert_not_called()
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_visual_review_uses_only_registered_artifact_and_returns_advisory_report(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            artifact = job_directory / "model-vertex-color.obj"
            artifact.write_text(
                "v 0 0 0 1 0 0\nv 10 0 0 0 1 0\nv 0 10 0 0 0 1\nf 1 2 3\n",
                encoding="ascii",
            )
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "ready"
            job.phase = "ready"
            job.artifact_path = artifact
            job.artifact_format = "obj"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job

            def review(obj_path, root, **kwargs):
                self.assertEqual(Path(obj_path), artifact.resolve())
                report = {
                    "schema_version": 1,
                    "review_version": "visual-v1",
                    "status": "review",
                    "score": 70,
                    "confidence": 0.8,
                    "summary": "建议检查轮廓。",
                    "warnings": ["visual_silhouette_unclear"],
                    "errors": [],
                    "checks": {},
                }
                (Path(root) / PRODUCTION.VISUAL_QUALITY_FILENAME).write_text(json.dumps(report), encoding="utf-8")
                return report

            try:
                with mock.patch.object(PRODUCTION, "review_model_visual_quality", side_effect=review) as visual_review:
                    with sidecar_server(PRODUCTION.Handler) as port:
                        request = urllib.request.Request(
                            f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/visual-review",
                            data=b"{}",
                            method="POST",
                            headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                        )
                        with urllib.request.urlopen(request, timeout=5) as response:
                            payload = json.loads(response.read())
                self.assertEqual(payload["job"]["visual_quality"]["status"], "review")
                visual_review.assert_called_once()
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_mock_health_contract(self):
        self.assert_contract(self.fetch_health(MOCK.Handler))

    def test_mock_palette_recommendation_supports_offline_gui_flow(self):
        with MOCK._jobs_lock:
            previous = dict(MOCK._jobs)
            MOCK._jobs.clear()
        try:
            with sidecar_server(MOCK.Handler) as port:
                create = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/recommend-text-palette",
                    data=json.dumps({"request_id": "mock", "prompt": "一只机械麒麟"}).encode(),
                    method="POST",
                    headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                )
                with urllib.request.urlopen(create, timeout=5) as response:
                    created = json.loads(response.read())["job"]
                self.assertEqual(created["state"], "awaiting_palette_confirmation")
                self.assertEqual(len(created["palette_recommendation"]["colors"]), 4)

                confirm = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{created['id']}/confirm-palette",
                    data=json.dumps({"palette": ["#D96B43", "#2B2422", "#F2D7B5", "#2F6B5F"]}).encode(),
                    method="POST",
                    headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                )
                with urllib.request.urlopen(confirm, timeout=5) as response:
                    confirmed = json.loads(response.read())["job"]
            self.assertTrue(confirmed["palette_recommendation_confirmed"])
            self.assertEqual(confirmed["state"], "preprocessing")
        finally:
            with MOCK._jobs_lock:
                MOCK._jobs.clear()
                MOCK._jobs.update(previous)

    def test_production_health_contract_without_credentials(self):
        with temporary_environment(
            OPENAI_PRO_API=None, OPENAI_PRO_URL=None, OPENAI_API_KEY=None, TRIPO_API_KEY=None
        ):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assert_contract(health)
        self.assertFalse(health["capabilities"]["config_proposal"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["palette_recommendation"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["image_provider"]["available"])

    def test_production_health_exposes_quality_first_provider_policy(self):
        with temporary_environment(OPENAI_API_KEY=None, TRIPO_API_KEY=None):
            health = self.fetch_health(PRODUCTION.Handler)

        policy = health["capabilities"]["model_generation"]["provider_policy"]
        self.assertEqual(policy["design_providers"], ["gpt", "image2"])
        self.assertEqual(policy["geometry_provider"], "tripo")
        self.assertFalse(policy["automatic_fallback"])
        self.assertEqual(policy["max_paid_model_tasks_per_confirmation"], 1)

    def test_windows_installer_manifest_includes_model_runtime_dependencies(self):
        repository = TOOLS_AI.parents[1]
        cmake_manifest = (repository / "CMakeLists.txt").read_text(encoding="utf-8")

        for module in (
            "model_input_image_quality.py",
            "model_job_support.py",
            "model_provider_gateway.py",
            "hunyuan_client.py",
            "hunyuan_provider_gateway.py",
            "model_refinement.py",
            "sampled_local_thickness.py",
        ):
            self.assertIn(f'"${{CMAKE_SOURCE_DIR}}/tools/ai/{module}"', cmake_manifest)

    def test_production_health_contract_with_openai_only(self):
        with temporary_environment(OPENAI_API_KEY="test-openai", TRIPO_API_KEY=None), \
             mock.patch.object(PRODUCTION._HUNYUAN_PROVIDER_GATEWAY, "model_generation_available", return_value=False):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assert_contract(health)
        self.assertTrue(health["capabilities"]["config_proposal"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["palette_recommendation"]["available"])

    def test_production_health_contract_with_generation_credentials(self):
        with temporary_environment(
            OPENAI_API_KEY="test-openai",
            TRIPO_API_KEY="test-tripo",
            ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK=None,
        ):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assert_contract(health)
        self.assertTrue(health["capabilities"]["config_proposal"]["available"])
        self.assertTrue(health["capabilities"]["model_generation"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["palette_recommendation"]["available"])

    def test_production_health_enables_text_image_creation_without_legacy_analysis(self):
        with temporary_environment(
            OPENAI_PRO_API="test-pro",
            OPENAI_PRO_URL="https://v.3dprint.beer/managed-ai/v1",
            OPENAI_API_KEY=None,
            OPENAI_BASE_URL=None,
            TRIPO_API_KEY="test-tripo",
            ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK=None,
        ):
            health = self.fetch_health(PRODUCTION.Handler)

        generation = health["capabilities"]["model_generation"]
        self.assertFalse(health["capabilities"]["config_proposal"]["available"])
        self.assertFalse(generation["palette_recommendation"]["available"])
        self.assertEqual(generation["source_availability"], {"text": True, "image": True})
        self.assertTrue(generation["available"])
        self.assertEqual(generation["image_provider"], {
            "available": True,
            "source": "pro",
            "base_url": "https://v.3dprint.beer/managed-ai/v1",
            "endpoints": {
                "generations": "https://v.3dprint.beer/managed-ai/v1/images/generations",
                "edits": "https://v.3dprint.beer/managed-ai/v1/images/edits",
            },
            "automatic_retry": False,
            "max_billable_requests_per_action": 1,
        })
        self.assertNotIn("test-pro", json.dumps(health))

    def test_production_health_reports_provider_url_without_credentials(self):
        with temporary_environment(
            OPENAI_BASE_URL="https://openai-user:openai-secret@laotie.dev/v1?token=hidden",
            TRIPO_API_BASE="https://tripo-user:tripo-secret@openapi.tripo3d.com/v3?token=hidden",
            OPENAI_API_KEY="test-openai",
            TRIPO_API_KEY="test-tripo",
            ORCASLICER_AI_CONFIG_MODE="internal_locked",
            ORCASLICER_AI_SESSION_TOKEN="e5" * 32,
        ):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assertEqual(health["runtime"]["openai_base_url"], "https://laotie.dev/v1")
        self.assertEqual(health["runtime"]["tripo_base_url"], "https://openapi.tripo3d.com/v3")
        self.assertEqual(health["runtime"]["configuration_mode"], "internal_locked")
        serialized = json.dumps(health)
        self.assertNotIn("test-openai", serialized)
        self.assertNotIn("openai-secret", serialized)
        self.assertNotIn("tripo-secret", serialized)
        self.assertNotIn("hidden", serialized)

    def test_production_health_exposes_only_safe_network_metadata(self):
        proxy = "http://proxy-user:proxy-secret@proxy-sensitive.example:8443"
        with temporary_environment(
            HTTP_PROXY=proxy,
            HTTPS_PROXY=proxy,
            http_proxy=proxy,
            https_proxy=proxy,
            NO_PROXY="127.0.0.1,localhost",
            no_proxy="127.0.0.1,localhost",
        ):
            health = self.fetch_health(PRODUCTION.Handler)

        network = health["runtime"]["network"]
        self.assertEqual(set(network), {"openai", "image2", "tripo"})
        for provider in network.values():
            self.assertEqual(set(provider), {"source", "schemes", "bypass"})
            self.assertIsInstance(provider["source"], str)
            self.assertIsInstance(provider["schemes"], list)
            self.assertIsInstance(provider["bypass"], bool)
            self.assertIn("https", provider["schemes"])
        serialized = json.dumps(network)
        for secret in ("proxy-user", "proxy-secret", "proxy-sensitive.example", "8443"):
            self.assertNotIn(secret, serialized)

    def test_production_health_contract_with_tripo_and_preprocess_fallback(self):
        with temporary_environment(
            OPENAI_API_KEY=None,
            TRIPO_API_KEY="test-tripo",
            ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK="1",
        ):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assert_contract(health)
        self.assertFalse(health["capabilities"]["config_proposal"]["available"])
        self.assertTrue(health["capabilities"]["model_generation"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["palette_recommendation"]["available"])


if __name__ == "__main__":
    unittest.main()

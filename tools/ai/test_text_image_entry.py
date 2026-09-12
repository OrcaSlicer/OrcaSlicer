"""Exercise text creation with only the Image2 provider configured, offline."""
import base64
import json
from pathlib import Path
import sys
import tempfile
import unittest
import urllib.error
import urllib.parse
import urllib.request
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import openai_preprocessor as preprocessor
import orca_ai_sidecar as sidecar
from test_sidecar_contract import sidecar_server, temporary_environment, valid_png_bytes


class TextImageEntryTests(unittest.TestCase):
    def setUp(self):
        directory = self.enterContext(tempfile.TemporaryDirectory())
        self.enterContext(temporary_environment(
            OPENAI_API_KEY=None, OPENAI_PRO_API="offline-image-key",
            OPENAI_PRO_URL="https://image.invalid/v1", OPENAI_IMAGE_MODEL="gpt-image-2",
            ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK=None,
            ORCASLICER_AI_OUTPUT_DIR=directory, ORCASLICER_AI_SESSION_TOKEN=None,
        ))
        self.enterContext(mock.patch.dict(sidecar._JOBS, {}, clear=True))
        original_open = urllib.request.OpenerDirector.open

        def local_only(opener, request, *args, **kwargs):
            url = request.full_url if isinstance(request, urllib.request.Request) else str(request)
            if urllib.parse.urlsplit(url).hostname != "127.0.0.1":
                raise AssertionError("Text image tests must mock provider transport.")
            return original_open(opener, request, *args, **kwargs)

        self.enterContext(mock.patch.object(urllib.request.OpenerDirector, "open", local_only))
        self.image_bytes = valid_png_bytes(512)
        self.transport = self.enterContext(mock.patch.object(
            preprocessor, "_request_with_provider",
            return_value={"data": [{"b64_json": base64.b64encode(self.image_bytes).decode()}]},
        ))

    def assert_design(self, job):
        self.assertEqual(job.state, "awaiting_confirmation", job.message)
        self.assertEqual(job.preview_path.read_bytes(), self.image_bytes)
        self.assertEqual(job.raw_preview_path.read_bytes(), self.image_bytes)
        self.assertIn("蓝色围巾", job.prepared_prompt)
        self.transport.assert_called_once()
        call = self.transport.call_args
        self.assertEqual(call.args[0], "/images/generations")
        self.assertEqual(call.kwargs["provider_source"], "pro")
        payload = json.loads(call.args[1])
        self.assertEqual(payload["n"], 1)
        self.assertIn("蓝色围巾", payload["prompt"])

    def test_worker_uses_image_provider_without_legacy_text_credentials(self):
        job = sidecar._new_job("text", (), style="cartoon")
        sidecar._preprocess_text_job(job, "戴蓝色围巾的小猫")
        self.assert_design(job)

    def test_broken_legacy_text_provider_cannot_block_image_creation(self):
        with (temporary_environment(OPENAI_API_KEY="unusable-legacy-key"),
              mock.patch.object(preprocessor, "complete_text", side_effect=AssertionError("No text rewrite request")) as text):
            job = sidecar._new_job("text", (), style="cartoon")
            sidecar._preprocess_text_job(job, "戴蓝色围巾的小猫")
        text.assert_not_called()
        self.assert_design(job)

    def test_http_text_routes_complete_with_only_image_provider(self):
        for route in ("text", "recommend-text-palette"):
            with self.subTest(route=route):
                self.transport.reset_mock()
                with (mock.patch.object(sidecar, "_submit", side_effect=lambda job, worker, *args: worker(job, *args)),
                      sidecar_server(sidecar.Handler) as port):
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{route}",
                        data=json.dumps({"request_id": route, "prompt": "戴蓝色围巾的小猫", "style": "cartoon"}).encode(),
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=5) as response:
                        self.assertEqual(response.status, 202)
                        public = json.load(response)["job"]
                job = sidecar._JOBS[public["id"]]
                self.assert_design(job)
                sidecar._persist_job(job)
                restored = sidecar._load_job(job.directory)
                self.assertEqual(restored.state, "awaiting_confirmation")
                with mock.patch.object(sidecar, "_submit") as submit:
                    sidecar._resume_restored_jobs([restored])
                submit.assert_not_called()

    def test_unavailable_image_provider_rejects_before_creating_job(self):
        for pro_key, legacy_key in ((None, None), ("partial-pro", "legacy-must-not-fallback")):
            with (self.subTest(pro_key=pro_key),
                  temporary_environment(OPENAI_PRO_API=pro_key, OPENAI_PRO_URL=None, OPENAI_API_KEY=legacy_key)):
                handler = sidecar.Handler.__new__(sidecar.Handler)
                with self.assertRaises(sidecar.RequestError) as failure:
                    handler._create_text_job()
                self.assertEqual(failure.exception.status, 503)
                with sidecar_server(sidecar.Handler) as port:
                    with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(
                            f"http://127.0.0.1:{port}/health", timeout=5) as response:
                        generation = json.load(response)["capabilities"]["model_generation"]
                self.assertEqual(generation["source_availability"], {"text": False, "image": False})
        self.transport.assert_not_called()
        self.assertEqual(sidecar._JOBS, {})


if __name__ == "__main__":
    unittest.main()

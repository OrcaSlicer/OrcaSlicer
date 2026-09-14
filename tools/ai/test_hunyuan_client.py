"""Offline TokenHub contract checks. All provider and download transport is mocked."""
import base64
import http.client
import io
import json
import os
from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock
import urllib.error
import urllib.request

from PIL import Image

from tools.ai import hunyuan_client as client


ARTIFACT_HOST = "hunyuan-prod-1258344699.cos.ap-guangzhou.tencentcos.cn"
GLB_URL = f"https://{ARTIFACT_HOST}/3d/output/model.glb"
OBJ_URL = f"https://{ARTIFACT_HOST}/3d/output/model.zip"


def request(**changes):
    return SimpleNamespace(**{
        "source": "text", "prompt": "一只小猫", "image_path": None, "image_paths": None,
        "face_limit": 1000000, "generation_profile": "quality", "geometry_quality": "standard",
        "texture_quality": "standard", **changes,
    })


def response(value=None, *, body=None, headers=None, status=200):
    stream = io.BytesIO(body if body is not None else json.dumps(value).encode("utf-8"))
    stream.headers = headers or {}
    stream.status = status
    return stream


def completed():
    return {
        "status": "completed", "request_id": "request-1", "created_at": 1784884760,
        "data": [{"type": "obj", "url": OBJ_URL, "preview_image_url": GLB_URL + ".png"},
                 {"type": "glb", "url": GLB_URL}],
    }


class HunyuanClientTests(unittest.TestCase):
    def setUp(self):
        self.environment = mock.patch.dict(os.environ, {"HY3D_API": "offline-test-key"}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.transport = mock.patch.object(client, "build_network_opener")
        self.opener = self.transport.start().return_value
        self.opener.open.side_effect = AssertionError("Unexpected transport call in offline test")
        self.addCleanup(self.transport.stop)

    def use_response(self, value=None, **kwargs):
        self.opener.open.side_effect = None
        self.opener.open.return_value = response(value, **kwargs)

    def image(self, root, name="image.png", size=(256, 256), format="PNG"):
        path = Path(root) / name
        Image.new("RGB", size, "white").save(path, format=format)
        return path

    def test_availability_is_local_and_requires_tokenhub_key(self):
        self.assertTrue(client.model_generation_available())
        with mock.patch.object(client, "_api_key", return_value=""):
            self.assertFalse(client.model_generation_available())
        self.opener.open.assert_not_called()

    def test_process_key_wins_without_registry_read(self):
        registry = mock.Mock()
        with mock.patch.object(client.os, "name", "nt"), mock.patch.dict("sys.modules", {"winreg": registry}):
            self.assertEqual(client._api_key(), "offline-test-key")
        registry.OpenKey.assert_not_called()

    def test_windows_fallback_reads_only_one_user_variable(self):
        registry = mock.MagicMock()
        registry.REG_SZ, registry.REG_EXPAND_SZ = 1, 2
        registry.QueryValueEx.return_value = ("user-test-key", 1)
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(client.os, "name", "nt"), mock.patch.dict("sys.modules", {"winreg": registry}):
            self.assertEqual(client._api_key(), "user-test-key")
            self.assertNotIn("HY3D_API", os.environ)
        registry.OpenKey.assert_called_once_with(registry.HKEY_CURRENT_USER, "Environment")
        registry.QueryValueEx.assert_called_once_with(registry.OpenKey.return_value.__enter__.return_value, "HY3D_API")

    def test_missing_windows_user_key_is_unavailable(self):
        registry = mock.Mock()
        registry.OpenKey.side_effect = FileNotFoundError()
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(client.os, "name", "nt"), mock.patch.dict("sys.modules", {"winreg": registry}):
            self.assertFalse(client.model_generation_available())

    def test_text_payload_matches_tokenhub_and_never_embeds_credentials(self):
        payload = client.prepare_model_payload(request())
        self.assertEqual(payload, {"model": "hy-3d-3.1", "face_count": 1000000,
                                   "generate_type": "Normal", "enable_pbr": False, "prompt": "一只小猫"})
        self.opener.open.assert_not_called()

    def test_model_is_fixed_for_creation_and_recovery_despite_environment_changes(self):
        with mock.patch.dict(os.environ, {"HUNYUAN3D_MODEL": "hy-3d-3.0"}):
            self.assertEqual(client.prepare_model_payload(request())["model"], "hy-3d-3.1")
            self.use_response(completed())
            client.wait_for_task("12345")
            sent = self.opener.open.call_args.args[0]
            self.assertEqual(json.loads(sent.data)["model"], "hy-3d-3.1")

    def test_invalid_inputs_are_rejected_before_network(self):
        for changes in ({"face_limit": 2000000}, {"face_limit": 2999}, {"face_limit": True},
                        {"prompt": ""}, {"prompt": "中" * 1025}, {"image_path": Path("input.png")},
                        {"geometry_quality": "detailed"}, {"texture_quality": "extreme"},
                        {"generation_profile": "unsupported"}, {"source": "unknown"}):
            with self.subTest(changes=changes), self.assertRaises(client.HunyuanError) as raised:
                client.prepare_model_payload(request(**changes))
            self.assertEqual(raised.exception.category, "validation")
        self.opener.open.assert_not_called()

    def test_image_payload_snapshots_exact_file_and_omits_prompt(self):
        with tempfile.TemporaryDirectory() as root:
            path = self.image(root)
            expected = path.read_bytes()
            payload = client.prepare_model_payload(request(source="image", image_path=path))
            path.write_bytes(b"subsequent edit")
        self.assertEqual(base64.b64decode(payload["image_base64"]), expected)
        self.assertEqual(payload["generate_type"], "Normal")
        self.assertNotIn("prompt", payload)

    def test_multiview_order_and_names_follow_documented_structure(self):
        with tempfile.TemporaryDirectory() as root:
            path = self.image(root)
            paths = {view: path for view in ("right", "left", "back", "front")}
            payload = client.prepare_model_payload(request(source="multiview", image_paths=paths))
        self.assertEqual([view["view_type"] for view in payload["multi_view_images"]], ["left", "back", "right"])
        self.assertTrue(all(set(view) == {"view_type", "view_image_base64"} for view in payload["multi_view_images"]))
        self.assertIn("image_base64", payload)

    def test_image_limits_and_corruption_are_local_errors(self):
        with tempfile.TemporaryDirectory() as root:
            for size in ((127, 256), (5001, 256)):
                path = self.image(root, size=size)
                with self.assertRaises(client.HunyuanError):
                    client.prepare_model_payload(request(source="image", image_path=path))
            path.write_bytes(b"not an image")
            with self.assertRaises(client.HunyuanError):
                client.prepare_model_payload(request(source="image", image_path=path))
            path.write_bytes(b"x" * (client._MAX_IMAGE_BYTES + 1))
            with self.assertRaises(client.HunyuanError):
                client.prepare_model_payload(request(source="image", image_path=path))
        self.opener.open.assert_not_called()

    def test_multiview_aggregate_image_limit(self):
        paths = {view: Path(view + ".png") for view in ("front", "left", "back", "right")}
        with mock.patch.object(client, "_image_bytes", return_value=b"x" * (2 * 1024 * 1024)):
            with self.assertRaisesRegex(client.HunyuanError, "total"):
                client.prepare_model_payload(request(source="multiview", image_paths=paths))

    def test_submit_posts_bearer_json_once_to_fixed_official_endpoint(self):
        self.use_response({"id": "12345", "status": "queued", "object": "3d_job"})
        payload = client.prepare_model_payload(request())
        self.assertEqual(client.submit_model_task(payload), "12345")
        sent = self.opener.open.call_args.args[0]
        self.assertEqual(sent.full_url, "https://tokenhub.tencentmaas.com/v1/api/3d/submit")
        self.assertEqual(sent.get_header("Authorization"), "Bearer offline-test-key")
        self.assertEqual(json.loads(sent.data), payload)
        self.assertEqual(sent.get_method(), "POST")
        self.opener.open.assert_called_once()

    def test_creation_transport_and_malformed_responses_are_ambiguous_without_retry(self):
        failures = [TimeoutError("secret echo"), urllib.error.URLError("secret echo"),
                    http.client.IncompleteRead(b"partial")]
        for error in failures:
            with self.subTest(error=type(error).__name__):
                self.opener.open.reset_mock()
                self.opener.open.side_effect = error
                with self.assertRaises(client.HunyuanError) as raised:
                    client.submit_model_task({"prompt": "cat"})
                self.assertTrue(raised.exception.ambiguous)
                self.assertFalse(raised.exception.retryable)
                self.assertNotIn("secret echo", str(raised.exception))
                self.opener.open.assert_called_once()
        for body in (b"not json", b"[]", b"{}"):
            self.use_response(body=body)
            with self.assertRaises(client.HunyuanError) as raised:
                client.submit_model_task({"prompt": "cat"})
            self.assertTrue(raised.exception.ambiguous)

    def test_creation_server_errors_are_ambiguous_but_auth_rejection_is_not(self):
        for status, category, ambiguous in ((500, "network", True), (401, "authentication", False), (429, "rate_limit", False)):
            with self.subTest(status=status):
                self.opener.open.side_effect = urllib.error.HTTPError(client._BASE_URL, status, "body with secret", {}, None)
                with self.assertRaises(client.HunyuanError) as raised:
                    client.submit_model_task({})
                self.assertEqual(raised.exception.category, category)
                self.assertEqual(raised.exception.ambiguous, ambiguous)
                self.assertNotIn("secret", str(raised.exception))

    def test_credentials_reject_header_injection_without_network(self):
        with mock.patch.dict(os.environ, {"HY3D_API": "key\r\nHost: evil.test"}):
            with self.assertRaises(client.HunyuanError):
                client.submit_model_task({})
        self.opener.open.assert_not_called()

    def test_http_errors_expose_only_bounded_safe_provider_codes(self):
        cases = (("AuthFailure.InvalidApiKey", 401, "authentication"),
                 ("UnauthorizedOperation", 403, "authentication"),
                 ("FailedOperation.InsufficientBalance", 400, "billing"),
                 ("FailedOperation.JobNotFound", 400, "network"))
        for code, status, category in cases:
            with self.subTest(code=code):
                body = io.BytesIO(json.dumps({"error": {"code": code, "message": "secret-key-and-prompt"}}).encode())
                self.opener.open.side_effect = urllib.error.HTTPError(client._BASE_URL, status, "secret-message", {}, body)
                with self.assertRaises(client.HunyuanError) as raised:
                    client.wait_for_task("12345")
                self.assertEqual(raised.exception.code, code)
                self.assertEqual(raised.exception.category, category)
                self.assertNotIn("secret", str(raised.exception))
        for code in ("bad\r\nsecret", "x" * 101, 123, "https://secret.test"):
            with self.subTest(code=code):
                body = io.BytesIO(json.dumps({"error": {"code": code, "message": "secret"}}).encode())
                self.opener.open.side_effect = urllib.error.HTTPError(client._BASE_URL, 401, "secret-message", {}, body)
                with self.assertRaises(client.HunyuanError) as raised:
                    client.wait_for_task("12345")
                self.assertEqual(raised.exception.code, "provider_http_error")
                self.assertNotIn("secret", str(raised.exception))

    def test_structured_submit_server_error_is_still_ambiguous(self):
        body = io.BytesIO(json.dumps({"error": {"code": "InternalError", "message": "secret"}}).encode())
        self.opener.open.side_effect = urllib.error.HTTPError(client._BASE_URL, 503, "secret-message", {}, body)
        with self.assertRaises(client.HunyuanError) as raised:
            client.submit_model_task({})
        self.assertTrue(raised.exception.ambiguous)
        self.assertFalse(raised.exception.retryable)
        self.opener.open.assert_called_once()

    def test_submit_validation_details_and_request_id_survive_both_response_paths(self):
        envelope = {"error": {"code": "InvalidParameter.InvalidParameter",
                              "message": "【GenerateType】仅支持Normal,LowPoly,Geometry,Sketch，请重新输入。"},
                    "request_id": "request-123"}
        for status in (200, 400):
            with self.subTest(status=status):
                self.opener.open.reset_mock()
                if status == 200:
                    self.use_response(envelope)
                else:
                    body = io.BytesIO(json.dumps(envelope).encode())
                    self.opener.open.side_effect = urllib.error.HTTPError(client._BASE_URL, status, "error", {}, body)
                with self.assertRaises(client.HunyuanError) as raised:
                    client.submit_model_task({"face_count": 1000000})
                self.assertIn("【GenerateType】仅支持Normal,LowPoly,Geometry,Sketch", str(raised.exception))
                self.assertIn("Request ID: request-123", str(raised.exception))
                self.assertEqual(raised.exception.category, "validation")
                self.assertFalse(raised.exception.retryable)
                self.assertFalse(raised.exception.ambiguous)
                self.opener.open.assert_called_once()

    def test_submit_diagnostics_redact_key_prompt_images_and_urls_before_bounding(self):
        prompt = "私人婚照"
        encoded = "sensitive-image-content"
        detail = ("Invalid image_base64: offline-test-key " + prompt + " "
                  + json.dumps(prompt)[1:-1] + " " + encoded + " "
                  + "https://example.test/?token=private Bearer another-key "
                  + "x" * 100 + "\n" + "more " * 200)
        self.use_response({"error": {"code": "InvalidParameter.Image", "message": detail},
                           "request_id": "bad\nidentifier"})
        with self.assertRaises(client.HunyuanError) as raised:
            client.submit_model_task({"prompt": prompt, "multi_view_images": [{"view_image_base64": encoded}]})
        message = str(raised.exception)
        self.assertIn("Invalid image_base64", message)
        for private in ("offline-test-key", prompt, json.dumps(prompt)[1:-1], encoded,
                        "example.test", "another-key", "x" * 64, "bad", "\n"):
            self.assertNotIn(private, message)
        self.assertLess(len(message), 600)
        self.opener.open.assert_called_once()

    def test_redirect_handler_never_forwards_bearer_header(self):
        request_ = urllib.request.Request(client._BASE_URL, headers={"Authorization": "Bearer secret"})
        self.assertIsNone(client._RejectRedirects().redirect_request(request_, None, 307, "redirect", {}, "https://evil.test"))

    def test_query_normalizes_artifacts_and_preserves_provider_metadata(self):
        raw = completed()
        self.use_response(raw)
        progress = mock.Mock()
        result = client.wait_for_task("12345", progress=progress)
        self.assertEqual(result["output"], {"model": GLB_URL})
        self.assertEqual(result["output_format"], "glb")
        self.assertEqual(result["status"], "success")
        self.assertEqual(result["provider_status"], "completed")
        self.assertEqual(result["data"], raw["data"])
        self.assertEqual(result["request_id"], raw["request_id"])
        self.assertEqual(result["ResultFile3Ds"][0]["Type"], "OBJ")
        self.assertEqual(result["ResultFile3Ds"][1]["Url"], GLB_URL)
        sent = self.opener.open.call_args.args[0]
        self.assertEqual(json.loads(sent.data), {"model": "hy-3d-3.1", "id": "12345"})
        self.assertTrue(sent.full_url.endswith("/query"))
        progress.assert_called_once_with(100)

    def test_query_accepts_obj_only_without_conversion(self):
        raw = completed()
        raw["data"].pop()
        self.use_response(raw)
        result = client.wait_for_task("12345")
        self.assertEqual(result["output"]["model"], OBJ_URL)
        self.assertEqual(result["output_format"], "obj")
        self.opener.open.assert_called_once()

    def test_polling_handles_queued_and_in_progress(self):
        self.opener.open.side_effect = [response({"status": "queued"}), response({"status": "in_progress"}), response(completed())]
        progress = mock.Mock()
        with mock.patch.object(client.time, "sleep"):
            client.wait_for_task("12345", progress=progress)
        self.assertEqual(progress.call_args_list, [mock.call(None), mock.call(None), mock.call(100)])

    def test_unknown_failed_and_empty_results_do_not_look_successful(self):
        for result in ({"status": "failed", "error": {"code": "provider_failure", "message": "secret"}},
                       {"status": "surprise"}, {"status": "completed", "data": []}):
            with self.subTest(result=result):
                self.use_response(result)
                with self.assertRaises(client.HunyuanError) as raised:
                    client.wait_for_task("12345")
                self.assertNotIn("secret", str(raised.exception))

    def test_cancellation_and_expired_deadline_do_not_query(self):
        stop = threading.Event()
        stop.set()
        with self.assertRaises(client.HunyuanError) as raised:
            client.wait_for_task("12345", stop_event=stop)
        self.assertEqual(raised.exception.code, "cancelled")
        for duration in (0, -1, float("inf"), float("nan")):
            with self.subTest(duration=duration), self.assertRaises(client.HunyuanError):
                client.wait_for_task("12345", deadline=duration)
        self.opener.open.assert_not_called()

    def test_cancellation_after_query_keeps_remote_task_out_of_success(self):
        stop = threading.Event()
        def finish_and_cancel(*args, **kwargs):
            stop.set()
            return response(completed())
        self.opener.open.side_effect = finish_and_cancel
        with self.assertRaises(client.HunyuanError) as raised:
            client.wait_for_task("12345", stop_event=stop)
        self.assertEqual(raised.exception.code, "cancelled")

    def test_download_restricts_official_cos_hosts_and_urls(self):
        for url in ("https://evil.test/model.glb", "http://" + ARTIFACT_HOST + "/model.glb",
                    "https://" + ARTIFACT_HOST + ".evil.test/model.glb", "https://127.0.0.1/model.glb",
                    "https://user:pass@" + ARTIFACT_HOST + "/model.glb", GLB_URL + "#fragment",
                    "https://" + ARTIFACT_HOST + ":8443/model.glb", "https://" + ARTIFACT_HOST + ":bad/model.glb"):
            with self.subTest(url=url), self.assertRaises(client.HunyuanError):
                client.download_task_artifact({"output": {"model": url}}, Path("unused.glb"))
        self.opener.open.assert_not_called()

    def test_download_is_atomic_bounded_and_has_no_provider_credentials(self):
        self.use_response(body=b"glTF artifact", headers={"Content-Length": "13"})
        with tempfile.TemporaryDirectory() as root:
            target = Path(root) / "model.glb"
            self.assertEqual(client.download_task_artifact({"output": {"model": GLB_URL}}, target), target)
            self.assertEqual(target.read_bytes(), b"glTF artifact")
            self.assertEqual(list(Path(root).iterdir()), [target])
        sent = self.opener.open.call_args.args[0]
        self.assertIsNone(sent.get_header("Authorization"))
        self.assertEqual(sent.get_method(), "GET")

    def test_download_failure_preserves_existing_file_and_removes_partial(self):
        for headers, body, limit in (({"Content-Length": "100"}, b"short", 200), ({}, b"oversized", 3),
                                     ({"Content-Length": "1000"}, b"", 100), ({}, b"", 100)):
            with self.subTest(headers=headers, body=body):
                self.use_response(body=body, headers=headers)
                with tempfile.TemporaryDirectory() as root:
                    target = Path(root) / "model.glb"
                    target.write_bytes(b"existing")
                    with self.assertRaises(client.HunyuanError):
                        client.download_task_artifact({"output": {"model": GLB_URL}}, target, max_bytes=limit)
                    self.assertEqual(target.read_bytes(), b"existing")
                    self.assertEqual(list(Path(root).iterdir()), [target])


if __name__ == "__main__":
    unittest.main()

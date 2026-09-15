"""Generation options and recovery; all provider boundaries are mocked."""
import json
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from PIL import Image

import model_provider_gateway as gateway_module
import orca_ai_sidecar as sidecar
import tripo_client as tripo


class GenerationPayloadTests(unittest.TestCase):
    def setUp(self):
        network = mock.patch("urllib.request.OpenerDirector.open", side_effect=AssertionError("offline test"))
        network.start()
        self.addCleanup(network.stop)

    def test_all_explicit_options_reach_each_generation_endpoint(self):
        sources = (
            (tripo.create_text_task, "test object", "/generation/text-to-model"),
            (tripo.create_image_task, "file-token", "/generation/image-to-model"),
            (tripo.create_multiview_task, {"front": "front-token", "back": "back-token"},
             "/generation/multiview-to-model"),
        )
        with mock.patch.object(tripo, "_config", return_value=("mock-key", "unused", "v3.1-20260211")), \
             mock.patch.object(tripo, "_post_json", return_value={"task_id": "model-id"}) as post:
            for create, source, endpoint in sources:
                for geometry, faces in (("standard", 1000000), ("detailed", 1000000), ("detailed", 2000000)):
                    for texture in ("standard", "detailed", "extreme"):
                        with self.subTest(endpoint=endpoint, geometry=geometry, faces=faces, texture=texture):
                            post.reset_mock()
                            self.assertEqual(create(source, faces, geometry_quality=geometry,
                                                    texture_quality=texture), "model-id")
                            post.assert_called_once()
                            route, payload = post.call_args.args
                            self.assertEqual(route, endpoint)
                            self.assertEqual(payload["geometry_quality"], geometry)
                            self.assertEqual(payload["texture_quality"], texture)
                            self.assertEqual(payload["face_limit"], faces)
                            self.assertTrue(payload["texture"])
                            self.assertTrue(payload["pbr"])
                            if create is not tripo.create_text_task:
                                self.assertFalse(payload["enable_image_autofix"])

    def test_legacy_two_million_request_remains_capped_standard(self):
        with mock.patch.object(tripo, "_config", return_value=("mock-key", "unused", "v3.1-20260211")), \
             mock.patch.object(tripo, "_post_json", return_value={"task_id": "model-id"}) as post:
            tripo.create_text_task("legacy object", 2000000)
        self.assertEqual(post.call_args.args[1]["face_limit"], 1000000)
        self.assertEqual(post.call_args.args[1]["geometry_quality"], "standard")

    def test_two_million_with_older_model_is_rejected_before_post(self):
        with mock.patch.object(tripo, "_config", return_value=("mock-key", "unused", "v2.5-20250123")), \
             mock.patch.object(tripo, "_post_json") as post:
            with self.assertRaises(tripo.TripoError):
                tripo.create_text_task("object", 2000000, geometry_quality="detailed")
        post.assert_not_called()

    def test_gateway_rejects_invalid_options_without_consuming_authorization(self):
        create = mock.Mock(return_value="model-id")
        gateway = gateway_module.ModelProviderGateway(create_text_task=create)
        for options in (
            {"geometry_quality": "ultra"}, {"texture_quality": "low"},
            {"geometry_quality": "standard", "face_limit": 2000000},
        ):
            with self.subTest(options=options):
                authorization = gateway_module.PaidTaskAuthorization.confirmed("offline:model:1")
                with self.assertRaises(gateway_module.ProviderGatewayError) as error:
                    gateway.start_or_reuse_model_task(
                        gateway_module.ModelTaskRequest(source="text", prompt="object", **options),
                        authorization=authorization,
                    )
                self.assertEqual(error.exception.code, "invalid_model_request")
                create.assert_not_called()
                # A valid request can still consume exactly the same authorization.
                gateway.start_or_reuse_model_task(
                    gateway_module.ModelTaskRequest(source="text", prompt="object",
                                                   geometry_quality="detailed", texture_quality="extreme"),
                    authorization=authorization,
                )
                create.assert_called_once_with("object", 1000000, "quality",
                                               geometry_quality="detailed", texture_quality="extreme")
                create.reset_mock()

    def test_gateway_forwards_options_for_image_and_multiview(self):
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "reference.png"
            reference.write_bytes(b"local reference; upload is mocked")
            for source in ("image", "multiview"):
                with self.subTest(source=source):
                    upload, image, multiview = (mock.Mock(return_value=value) for value in
                                               ("file-token", "image-id", "multiview-id"))
                    gateway = gateway_module.ModelProviderGateway(upload_image=upload,
                        create_image_task=image, create_multiview_task=multiview)
                    views = {view: reference for view in ("front", "left", "back", "right")}
                    gateway.start_or_reuse_model_task(gateway_module.ModelTaskRequest(
                        source=source, image_path=reference, image_paths=views, face_limit=2000000,
                        geometry_quality="detailed", texture_quality="extreme"),
                        authorization=gateway_module.PaidTaskAuthorization.confirmed("offline:model:1"))
                    create = image if source == "image" else multiview
                    inputs = "file-token" if source == "image" else {view: "file-token" for view in views}
                    create.assert_called_once_with(inputs, 2000000, "quality",
                        geometry_quality="detailed", texture_quality="extreme")
                    self.assertEqual(upload.call_count, 1 if source == "image" else 4)


class GenerationOptionsRecoveryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        environment = mock.patch.dict(os.environ, {"ORCASLICER_AI_OUTPUT_DIR": temporary.name})
        environment.start()
        self.addCleanup(environment.stop)
        network = mock.patch("urllib.request.OpenerDirector.open", side_effect=AssertionError("offline test"))
        network.start()
        self.addCleanup(network.stop)
        jobs = mock.patch.dict(sidecar._JOBS, {}, clear=True)
        jobs.start()
        self.addCleanup(jobs.stop)
        self.job = sidecar._new_job("text", ())

    def test_design_creation_preserves_model_choices_until_model_confirmation(self):
        image = io.BytesIO()
        Image.new("RGB", (512, 512), "gray").save(image, format="PNG")
        for source in ("text", "image"):
            for provider, geometry, texture in (("tripo", "standard", "standard"),
                                                 ("tripo", "detailed", "extreme"),
                                                 ("hunyuan", "detailed", "extreme")):
                with self.subTest(source=source, provider=provider, geometry=geometry):
                    options = {"provider": provider, "face_limit": 2000000,
                               "geometry_quality": geometry, "texture_quality": texture, "output_format": "obj"}
                    handler = object.__new__(sidecar.Handler)
                    handler._read_model_json = mock.Mock(return_value={
                        "request_id": "offline-design", "prompt": "test object", **options})
                    handler._read_image_multipart = mock.Mock(return_value=({
                        "request_id": "offline-design", "instruction": "test object", **options,
                        "face_limit": "2000000"}, image.getvalue(), "image/png"))
                    handler.send_json = mock.Mock()
                    with mock.patch.object(sidecar, "image_provider_status", return_value={"available": True}), \
                         mock.patch.object(sidecar, "_submit") as submit, \
                         mock.patch.object(sidecar.PaidTaskAuthorization, "confirmed") as authorize:
                        getattr(handler, "_create_" + source + "_job")()
                    authorize.assert_not_called()
                    submit.assert_called_once()
                    job = submit.call_args.args[0]
                    self.assertIs(submit.call_args.args[1], getattr(sidecar, "_preprocess_" + source + "_job"))
                    self.assertEqual(handler.send_json.call_args.args[0], 202)
                    restored = sidecar._load_job(job.directory)
                    self.assertIsNotNone(restored)
                    for key, expected in options.items():
                        self.assertEqual(getattr(restored, key), expected)

    def test_design_creation_still_rejects_malformed_model_option_fields(self):
        for options in ({"geometry_quality": "ultra"}, {"texture_quality": "low"},
                        {"output_format": "stl"}, {"face_limit": 123}, {"provider": "unknown"}):
            with self.subTest(options=options):
                with self.assertRaises(sidecar.RequestError):
                    sidecar._new_job("text", provider=options.get("provider", "tripo"), generation_options=options)

    def test_invalid_saved_design_can_be_corrected_before_any_paid_submission(self):
        self.job.geometry_quality = "standard"
        self.job.face_limit = 2000000
        self.job.state = "awaiting_confirmation"
        sidecar._persist_job(self.job)
        restored = sidecar._load_job(self.job.directory)
        self.assertIsNotNone(restored)
        handler = object.__new__(sidecar.Handler)
        handler._get_job = mock.Mock(return_value=restored)
        handler.send_json = mock.Mock()
        handler._read_model_json = mock.Mock(return_value={
            "prepared_prompt": "offline object", "provider": "tripo", "face_limit": 2000000,
            "geometry_quality": "standard", "texture_quality": "standard", "output_format": "glb"})
        with mock.patch.object(sidecar.PaidTaskAuthorization, "confirmed") as authorize, \
             mock.patch.object(sidecar, "_submit") as submit:
            with self.assertRaisesRegex(sidecar.RequestError, "2-million-face"):
                handler._generate(restored.id)
            authorize.assert_not_called()
            submit.assert_not_called()
            self.assertEqual(restored.state, "awaiting_confirmation")
            handler._read_model_json.return_value.pop("prepared_prompt")
            handler._read_model_json.return_value["face_limit"] = 1000000
            handler._set_generation_options(restored.id)
            authorize.assert_not_called()
            submit.assert_not_called()
        self.assertEqual(restored.face_limit, 1000000)
        self.assertEqual(restored.geometry_quality, "standard")
        self.assertEqual(sidecar._load_job(restored.directory).face_limit, 1000000)

    def test_legacy_persisted_job_does_not_gain_costlier_options(self):
        state_path = self.job.directory / sidecar.JOB_STATE_FILENAME
        saved = json.loads(state_path.read_text(encoding="utf-8"))
        saved["face_limit"] = 2000000
        for key in ("geometry_quality", "texture_quality", "output_format"):
            saved.pop(key, None)
        state_path.write_text(json.dumps(saved), encoding="utf-8")
        restored = sidecar._load_job(self.job.directory)
        self.assertEqual(restored.face_limit, 2000000)
        self.assertIsNone(restored.geometry_quality)
        self.assertEqual(restored.texture_quality, "standard")
        self.assertEqual(restored.output_format, "glb")

    def test_generation_worker_passes_frozen_options_to_gateway(self):
        self.job.face_limit = 2000000
        self.job.geometry_quality = "detailed"
        self.job.texture_quality = "extreme"
        gateway = mock.Mock()
        gateway.start_or_reuse_model_task.side_effect = gateway_module.ProviderGatewayError(
            "offline stop after observing request", code="provider_unavailable", category="availability",
            provider="tripo", operation="model_generation")
        with mock.patch.object(sidecar, "_MODEL_PROVIDER_GATEWAY", gateway):
            sidecar._generate_job(self.job, "frozen object", False,
                sidecar.PaidTaskAuthorization.confirmed("offline:model:1"))
        gateway.start_or_reuse_model_task.assert_called_once()
        request = gateway.start_or_reuse_model_task.call_args.args[0]
        self.assertEqual((request.face_limit, request.geometry_quality, request.texture_quality),
                         (2000000, "detailed", "extreme"))
        self.assertEqual(self.job.state, "failed")

    def test_generate_route_persists_and_exposes_options_before_scheduling(self):
        self.job.state = "awaiting_confirmation"
        handler = object.__new__(sidecar.Handler)
        handler._read_model_json = mock.Mock(return_value={
            "prepared_prompt": "offline model", "face_limit": 2000000,
            "geometry_quality": "detailed", "texture_quality": "extreme", "output_format": "obj",
        })
        handler._get_job = mock.Mock(return_value=self.job)
        handler.send_json = mock.Mock()

        def assert_frozen(job, *args):
            saved = json.loads((job.directory / sidecar.JOB_STATE_FILENAME).read_text(encoding="utf-8"))
            for key, expected in (("face_limit", 2000000), ("geometry_quality", "detailed"),
                                  ("texture_quality", "extreme"), ("output_format", "obj")):
                self.assertEqual(saved[key], expected)

        with mock.patch.object(sidecar, "_MODEL_PROVIDER_GATEWAY") as gateway, \
             mock.patch.object(sidecar, "_submit", side_effect=assert_frozen) as submit:
            gateway.model_generation_available.return_value = True
            handler._generate(self.job.id)
        submit.assert_called_once()
        restored = sidecar._load_job(self.job.directory)
        self.assertIsNotNone(restored)
        public = sidecar._public_job(restored)
        for key in ("face_limit", "geometry_quality", "texture_quality", "output_format"):
            self.assertEqual(public[key], handler._read_model_json.return_value[key])

    def test_corrected_design_requires_one_explicit_confirmation_and_does_not_submit_twice(self):
        self.job.state = "awaiting_confirmation"
        self.job.geometry_quality = "standard"
        self.job.face_limit = 2000000
        sidecar._persist_job(self.job)
        handler = object.__new__(sidecar.Handler)
        handler._get_job = mock.Mock(return_value=sidecar._load_job(self.job.directory))
        handler.send_json = mock.Mock()
        handler._read_model_json = mock.Mock(return_value={
            "provider": "tripo", "face_limit": 2000000,
            "geometry_quality": "detailed", "texture_quality": "extreme", "output_format": "obj"})
        with mock.patch.object(sidecar, "_model_gateway") as gateway, \
             mock.patch.object(sidecar.PaidTaskAuthorization, "confirmed") as authorize, \
             mock.patch.object(sidecar, "_submit") as submit:
            handler._set_generation_options(self.job.id)
            gateway.assert_not_called()
            authorize.assert_not_called()
            submit.assert_not_called()
            restored = sidecar._load_job(self.job.directory)
            self.assertEqual((restored.face_limit, restored.geometry_quality, restored.texture_quality),
                             (2000000, "detailed", "extreme"))
            handler._get_job.return_value = restored
            handler._read_model_json.return_value["prepared_prompt"] = "offline corrected design"
            handler._generate(self.job.id)
            authorize.assert_called_once_with(f"{self.job.id}:model:1", "tripo")
            submit.assert_called_once()
            with self.assertRaisesRegex(sidecar.RequestError, "not awaiting confirmation"):
                handler._generate(self.job.id)
            authorize.assert_called_once()
            submit.assert_called_once()
            self.assertEqual(restored.state, "queued")

    def test_invalid_route_options_do_not_authorize_or_change_job(self):
        for options in ({"geometry_quality": "standard", "face_limit": 2000000},
                        {"texture_quality": "low"}, {"output_format": "stl"},
                        {"provider": "hunyuan", "face_limit": 2000000},
                        {"provider": "hunyuan", "geometry_quality": "detailed"}):
            with self.subTest(options=options):
                self.job.state = "awaiting_confirmation"
                before = (self.job.directory / sidecar.JOB_STATE_FILENAME).read_bytes()
                handler = object.__new__(sidecar.Handler)
                handler._read_model_json = mock.Mock(return_value={"prepared_prompt": "object", **options})
                handler._get_job = mock.Mock(return_value=self.job)
                with mock.patch.object(sidecar.PaidTaskAuthorization, "confirmed") as authorize, \
                     mock.patch.object(sidecar, "_model_gateway") as gateway, \
                     mock.patch.object(sidecar, "_submit") as submit:
                    with self.assertRaises(sidecar.RequestError) as error:
                        handler._generate(self.job.id)
                self.assertEqual(error.exception.code, "invalid_generation_options")
                authorize.assert_not_called()
                submit.assert_not_called()
                self.assertEqual(gateway.mock_calls, [])
                self.assertEqual(self.job.state, "awaiting_confirmation")
                self.assertEqual((self.job.directory / sidecar.JOB_STATE_FILENAME).read_bytes(), before)

    def test_obj_conversion_persists_intent_and_reuses_id_after_restart(self):
        self.job.output_format = "obj"
        self.job.attempts = [{"attempt": 1, "generation_task_id": "model-id"}]

        def create(source, output):
            saved = json.loads((self.job.directory / sidecar.JOB_STATE_FILENAME).read_text(encoding="utf-8"))
            self.assertTrue(saved["attempts"][0]["conversion_submission_started"])
            self.assertEqual((source, output), ("model-id", "obj"))
            return "conversion-id"

        create_conversion = mock.Mock(side_effect=create)
        wait = mock.Mock(return_value={"status": "success"})
        gateway = gateway_module.ModelProviderGateway(create_conversion=create_conversion,
            wait_for_task=wait, download_task_artifact=mock.Mock())
        with mock.patch.object(sidecar, "_MODEL_PROVIDER_GATEWAY", gateway), \
             mock.patch.object(sidecar, "_prepare_obj_artifact", return_value=Path("offline.obj")):
            sidecar._download_generation_artifact(self.job, "model-id")
            restored = sidecar._load_job(self.job.directory)
            self.assertEqual(restored.attempts[0]["conversion_task_id"], "conversion-id")
            sidecar._download_generation_artifact(restored, "model-id", resume=True)
        create_conversion.assert_called_once()
        self.assertEqual([call.args[0] for call in wait.call_args_list], ["conversion-id", "conversion-id"])

    def test_ambiguous_obj_conversion_never_resubmits_after_restart(self):
        self.job.output_format = "obj"
        self.job.attempts = [{"attempt": 1, "generation_task_id": "model-id"}]
        create = mock.Mock(side_effect=tripo.TripoError("Could not connect to Tripo."))
        gateway = gateway_module.ModelProviderGateway(create_conversion=create)
        with mock.patch.object(sidecar, "_MODEL_PROVIDER_GATEWAY", gateway):
            with self.assertRaises(gateway_module.ProviderGatewayError):
                sidecar._download_generation_artifact(self.job, "model-id")
            restored = sidecar._load_job(self.job.directory)
            self.assertTrue(restored.attempts[0]["conversion_submission_started"])
            for job in (self.job, restored):
                with self.assertRaisesRegex(tripo.TripoError, "will not be submitted again"):
                    sidecar._download_generation_artifact(job, "model-id", resume=True)
        create.assert_called_once_with("model-id", "obj")

    def test_obj_conversion_is_not_submitted_when_intent_cannot_be_saved(self):
        self.job.output_format = "obj"
        self.job.attempts = [{"attempt": 1, "generation_task_id": "model-id"}]
        with mock.patch.object(sidecar, "_MODEL_PROVIDER_GATEWAY") as gateway, \
             mock.patch.object(sidecar.os, "replace", side_effect=OSError("disk unavailable")):
            with self.assertRaisesRegex(tripo.TripoError, "job state could not be saved"):
                sidecar._download_generation_artifact(self.job, "model-id")
        gateway.start_or_reuse_conversion.assert_not_called()


if __name__ == "__main__":
    unittest.main()

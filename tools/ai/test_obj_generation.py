#!/usr/bin/env python3
import importlib.util
import hashlib
import io
import json
import math
import os
import stat
import sys
import tempfile
import unittest
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest import mock

from PIL import Image, ImageDraw


TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


SIDECAR = load_module("orca_ai_sidecar_obj_generation", TOOLS_AI / "orca_ai_sidecar.py")
TRIPO = load_module("tripo_client_low_poly", TOOLS_AI / "tripo_client.py")


class TripoGenerationProfileRequestTests(unittest.TestCase):
    def setUp(self):
        self.environment = mock.patch.dict(
            os.environ,
            {"TRIPO_API_KEY": "test-key", "TRIPO_MODEL": "test-model"},
        )
        self.environment.start()

    def tearDown(self):
        self.environment.stop()

    def assert_profile_payload(self, payload, profile="quality", face_limit=1000000):
        self.assertEqual(payload["model"], "test-model")
        self.assertFalse(payload["smart_low_poly"])
        self.assertEqual(payload["face_limit"], face_limit)
        self.assertTrue(payload["texture"])
        self.assertTrue(payload["pbr"])
        self.assertEqual(payload["texture_quality"], "standard")
        self.assertEqual(payload["geometry_quality"], "standard")
        self.assertFalse(payload["quad"])
        self.assertTrue(payload["export_uv"])

    def test_text_generation_requests_colored_high_detail_model(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "text-id"}) as post:
            self.assertEqual(TRIPO.create_text_task("one watertight figurine"), "text-id")

        path, payload = post.call_args.args
        self.assertEqual(path, "/generation/text-to-model")
        self.assertEqual(payload["prompt"], "one watertight figurine")
        self.assert_profile_payload(payload)
        self.assertNotIn("texture_alignment", payload)

    def test_image_generation_requests_original_image_texture_alignment(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "image-id"}) as post:
            self.assertEqual(TRIPO.create_image_task("file-token"), "image-id")

        path, payload = post.call_args.args
        self.assertEqual(path, "/generation/image-to-model")
        self.assertEqual(payload["input"], "file-token")
        self.assert_profile_payload(payload)
        self.assertEqual(payload["texture_alignment"], "original_image")
        self.assertNotIn("orientation", payload)
        self.assertFalse(payload["enable_image_autofix"])

    def test_performance_profile_keeps_texture_and_pbr_with_standard_quality(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "image-id"}) as post:
            TRIPO.create_image_task("file-token", 300000, "performance")

        self.assert_profile_payload(post.call_args.args[1], "performance", 300000)
        self.assertFalse(post.call_args.args[1]["enable_image_autofix"])
        self.assertNotIn("orientation", post.call_args.args[1])

    def test_multiview_generation_uses_named_canonical_views(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "multiview-id"}) as post:
            task_id = TRIPO.create_multiview_task({
                "right": "right-token",
                "front": "front-token",
                "back": "back-token",
            })
        self.assertEqual(task_id, "multiview-id")
        path, payload = post.call_args.args
        self.assertEqual(path, "/generation/multiview-to-model")
        self.assertEqual(payload["inputs"], [
            {"front": "front-token"},
            {"back": "back-token"},
            {"right": "right-token"},
        ])
        self.assertEqual(payload["texture_alignment"], "original_image")
        self.assertEqual(payload["orientation"], "align_image")
        self.assertFalse(payload["enable_image_autofix"])
        self.assert_profile_payload(payload)

    def test_multiview_generation_requires_front_and_another_view(self):
        with mock.patch.object(TRIPO, "_post_json") as post:
            with self.assertRaisesRegex(TRIPO.TripoError, "front"):
                TRIPO.create_multiview_task({"left": "left-token", "back": "back-token"})
            with self.assertRaisesRegex(TRIPO.TripoError, "additional"):
                TRIPO.create_multiview_task({"front": "front-token"})
        post.assert_not_called()

    def test_texture_task_defaults_to_standard_texture(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "texture-id"}) as post:
            TRIPO.create_texture_task("source-model-task", "image-token")
        self.assertEqual(post.call_args.args[1]["texture_quality"], "standard")

    def test_texture_task_reuses_geometry_with_one_reference_image(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "texture-id"}) as post:
            task_id = TRIPO.create_texture_task(
                "source-model-task",
                "image-token",
                texture_alignment="geometry",
                texture_quality="extreme",
                texture_seed=41,
            )

        self.assertEqual(task_id, "texture-id")
        path, payload = post.call_args.args
        self.assertEqual(path, "/models/texture")
        self.assertEqual(payload, {
            "input": "source-model-task",
            "model": "v3.0-20250812",
            "texture_prompt": {"image": "image-token"},
            "pbr": True,
            "texture_alignment": "geometry",
            "texture_quality": "extreme",
            "texture_seed": 41,
            "bake": True,
        })

    def test_texture_task_accepts_four_ordered_reference_images(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "texture-id"}) as post:
            task_id = TRIPO.create_texture_task(
                "source-model-task",
                ["front-token", "left-token", "back-token", "right-token"],
                texture_alignment="geometry",
                texture_quality="detailed",
            )

        self.assertEqual(task_id, "texture-id")
        path, payload = post.call_args.args
        self.assertEqual(path, "/models/texture")
        self.assertEqual(payload["texture_prompt"], {
            "images": ["front-token", "left-token", "back-token", "right-token"],
        })

    def test_texture_task_rejects_incomplete_multiview_reference(self):
        with mock.patch.object(TRIPO, "_post_json") as post:
            with self.assertRaisesRegex(TRIPO.TripoError, "exactly four"):
                TRIPO.create_texture_task("source-task", ["front-token", "left-token"])
            with self.assertRaisesRegex(TRIPO.TripoError, "image reference"):
                TRIPO.create_texture_task(
                    "source-task", ["front-token", "left-token", "", "right-token"]
                )
        post.assert_not_called()

    def test_texture_task_rejects_invalid_settings_before_request(self):
        with mock.patch.object(TRIPO, "_post_json") as post:
            with self.assertRaisesRegex(TRIPO.TripoError, "source model"):
                TRIPO.create_texture_task("", "image-token")
            with self.assertRaisesRegex(TRIPO.TripoError, "image reference"):
                TRIPO.create_texture_task("source-task", "")
            with self.assertRaisesRegex(TRIPO.TripoError, "alignment"):
                TRIPO.create_texture_task("source-task", "image-token", texture_alignment="nearest")
            with self.assertRaisesRegex(TRIPO.TripoError, "[Tt]exture quality"):
                TRIPO.create_texture_task("source-task", "image-token", texture_quality="ultra")
        post.assert_not_called()

    def test_supported_face_target_is_forwarded(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "text-id"}) as post:
            TRIPO.create_text_task("printable figure", 1000000)
        self.assert_profile_payload(post.call_args.args[1], "quality", 1000000)

    def test_unsupported_generation_profile_is_rejected_before_request(self):
        with mock.patch.object(TRIPO, "_post_json") as post:
            with self.assertRaisesRegex(TRIPO.TripoError, "generation profile"):
                TRIPO.create_text_task("printable figure", 300000, "turbo")
        post.assert_not_called()

    def test_unsupported_face_target_is_rejected_before_request(self):
        with mock.patch.object(TRIPO, "_post_json") as post:
            with self.assertRaisesRegex(TRIPO.TripoError, "face target"):
                TRIPO.create_text_task("printable figure", 20000)
        post.assert_not_called()


class AutomaticVisualDeliveryGateTests(unittest.TestCase):
    def test_unavailable_reference_checks_return_advice(self):
        with tempfile.TemporaryDirectory() as directory:
            job = SIDECAR.Job(id="reference-advice", source="image", directory=Path(directory))
            with (
                mock.patch.object(SIDECAR, "_assess_job_model_reference", side_effect=SIDECAR.ModelInputImageQualityError("unavailable")),
                mock.patch.object(SIDECAR, "_assess_job_generation_reference", side_effect=SIDECAR.ModelInputImageQualityError("unavailable")),
            ):
                reports = SIDECAR._assess_reference_advice(job)
            for report in reports:
                self.assertEqual(report["status"], "unavailable")
                self.assertIn("reference_quality_unavailable", report["warnings"])
                self.assertTrue(report["model_input_eligible"])

    def test_preview_visual_review_retains_identity_drift_advice(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = root / "input.png"
            natural = root / "model-reference.png"
            printable = root / "clean-preview.png"
            for path in (original, natural, printable):
                path.write_bytes(b"image")
            job = SIDECAR.Job(id="preview-review", source="image", directory=root)
            job.style = "realistic"
            job.generation_profile = "quality"
            job.model_reference_path = natural
            job.preview_path = printable
            job.image_metrics = {
                "model_input_quality": {"score": 92, "model_input_eligible": True, "blockers": []},
                "generation_input_quality": {"score": 92, "model_input_eligible": True, "blockers": []},
            }
            report = {
                "status": "review",
                "score": 82,
                "model_generation_recommended": False,
                "blocking_warnings": ["preview_identity_mismatch"],
            }

            with mock.patch.dict(os.environ, {"OPENAI_API_KEY": "test-key"}), \
                 mock.patch.object(SIDECAR, "review_prepared_reference", return_value=report):
                self.assertIs(SIDECAR._assess_job_preview_visual_quality(job, original), report)

            self.assertEqual(job.phase, "checking_image")
            self.assertEqual(job.progress, 14)
            self.assertFalse(job.image_metrics["model_input_quality"]["model_input_eligible"])
            self.assertEqual(
                job.image_metrics["model_input_quality"]["blockers"][0],
                "preview_identity_mismatch",
            )

    def test_quality_image_job_does_not_render_or_call_provider_before_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = root / "input.png"
            reference.write_bytes(b"reference")
            modeling_reference = root / "model-reference.png"
            modeling_reference.write_bytes(b"model-reference")
            artifact = root / "model.obj"
            artifact.write_text("v 0 0 0 1 1 1\n", encoding="ascii")
            job = SIDECAR.Job(id="automatic-review", source="image", directory=root)
            job.input_path = reference
            job.model_reference_path = modeling_reference
            job.user_prompt = "保持本人五官"
            job.style = "realistic"
            job.generation_profile = "quality"
            report = {"status": "review", "import_recommended": False}

            with mock.patch.dict(os.environ, {"OPENAI_API_KEY": "test-key"}), \
                 mock.patch.object(SIDECAR, "review_model_visual_quality", return_value=report) as review:
                self.assertIsNone(SIDECAR._automatic_visual_review(job, artifact))
            review.assert_not_called()
            self.assertNotEqual(job.phase, "checking_visual")

            (root / SIDECAR.VISUAL_QUALITY_FILENAME).write_text(json.dumps(report), encoding="utf-8")
            with mock.patch.object(SIDECAR, "review_model_visual_quality") as review:
                self.assertEqual(SIDECAR._automatic_visual_review(job, artifact), report)
            review.assert_not_called()

    def test_performance_image_job_skips_automatic_visual_review(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            job = SIDECAR.Job(id="performance-review", source="image", directory=root)
            job.generation_profile = "performance"
            with mock.patch.dict(os.environ, {"OPENAI_API_KEY": "test-key"}), \
                 mock.patch.object(SIDECAR, "review_model_visual_quality") as review:
                self.assertIsNone(SIDECAR._automatic_visual_review(job, root / "model.obj"))
            review.assert_not_called()


class TripoArtifactSecurityTests(unittest.TestCase):
    def test_exact_official_cdn_allows_proxy_fake_ip(self):
        address = [(2, 1, 6, "", ("198.18.0.1", 443))]
        with mock.patch.object(TRIPO.socket, "getaddrinfo", return_value=address):
            TRIPO._validate_artifact_url("https://openapi.cdn.tripo3d.com/tasks/model.zip?signature=test")

    def test_cdn_lookalike_is_rejected_before_dns(self):
        with mock.patch.object(TRIPO.socket, "getaddrinfo") as resolve:
            with self.assertRaisesRegex(TRIPO.TripoError, "unsafe artifact"):
                TRIPO._validate_artifact_url("https://openapi.cdn.tripo3d.com.attacker.invalid/model.zip")
        resolve.assert_not_called()

    def test_non_https_official_cdn_is_rejected(self):
        with self.assertRaisesRegex(TRIPO.TripoError, "unsafe artifact"):
            TRIPO._validate_artifact_url("http://openapi.cdn.tripo3d.com/model.zip")

    def test_interrupted_download_resumes_without_restarting_the_task(self):
        first = mock.MagicMock()
        first.status = 200
        first.headers = {"Content-Length": "8"}
        first.read = io.BytesIO(b"abc").read
        first.__enter__.return_value = first

        resumed = mock.MagicMock()
        resumed.status = 206
        resumed.headers = {"Content-Length": "5", "Content-Range": "bytes 3-7/8"}
        resumed.read = io.BytesIO(b"defgh").read
        resumed.__enter__.return_value = resumed

        opener = mock.Mock()
        opener.open.side_effect = [first, resumed]
        task = {"output": {"model_url": "https://openapi.cdn.tripo3d.com/model.zip"}}
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(TRIPO, "_validate_artifact_url"), \
             mock.patch.object(TRIPO.urllib.request, "build_opener", return_value=opener):
            output = TRIPO.download_task_artifact(task, Path(directory) / "model.zip", max_bytes=16)
            self.assertEqual(output.read_bytes(), b"abcdefgh")

        self.assertEqual(opener.open.call_count, 2)
        resumed_request = opener.open.call_args_list[1].args[0]
        self.assertEqual(resumed_request.get_header("Range"), "bytes=3-")

    def test_persistently_short_download_is_rejected_and_cleaned_up(self):
        responses = []
        for _ in range(TRIPO._ARTIFACT_DOWNLOAD_ATTEMPTS):
            response = mock.MagicMock()
            response.status = 200
            response.headers = {"Content-Length": "8"}
            response.read = io.BytesIO(b"abc").read
            response.__enter__.return_value = response
            responses.append(response)
        opener = mock.Mock()
        opener.open.side_effect = responses
        task = {"output": {"model_url": "https://openapi.cdn.tripo3d.com/model.zip"}}
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(TRIPO, "_validate_artifact_url"), \
             mock.patch.object(TRIPO.urllib.request, "build_opener", return_value=opener):
            output = Path(directory) / "model.zip"
            with self.assertRaisesRegex(TRIPO.TripoError, "incomplete after retrying"):
                TRIPO.download_task_artifact(task, output, max_bytes=16)
            self.assertFalse(output.exists())
            self.assertFalse(output.with_name("model.zip.part").exists())

    def test_safe_redirect_preserves_resume_range(self):
        request = TRIPO.urllib.request.Request(
            "https://openapi.cdn.tripo3d.com/first.zip",
            headers={"Accept": "application/octet-stream", "Range": "bytes=42-"},
        )
        with mock.patch.object(TRIPO, "_validate_artifact_url"):
            redirected = TRIPO._SafeArtifactRedirects().redirect_request(
                request, None, 302, "Found", {}, "https://openapi.cdn.tripo3d.com/second.zip"
            )
        self.assertEqual(redirected.get_header("Range"), "bytes=42-")


class PrintablePaletteTests(unittest.TestCase):
    def test_palette_is_normalized_and_deduplicated_in_slot_order(self):
        self.assertEqual(
            SIDECAR._normalize_palette(["#ff0000", "#00FF00", "#FF0000"]),
            ("#FF0000", "#00FF00"),
        )

    def test_palette_can_be_disabled_with_an_empty_array(self):
        self.assertEqual(SIDECAR._normalize_palette([]), ())

    def test_palette_must_be_an_array(self):
        for value in (None, "#FF0000"):
            with self.subTest(value=value):
                with self.assertRaises(SIDECAR.RequestError):
                    SIDECAR._normalize_palette(value)

    def test_palette_rejects_invalid_colors_and_too_many_slots(self):
        for value in (["red"], ["#12345G"], ["#000000"] * 17):
            with self.subTest(value=value):
                with self.assertRaises(SIDECAR.RequestError):
                    SIDECAR._normalize_palette(value)

    def test_style_ids_are_strict_and_default_to_sculpture(self):
        self.assertEqual(SIDECAR._normalize_style(None), "sculpture")
        self.assertEqual(SIDECAR._normalize_style("realistic"), "realistic")
        self.assertEqual(SIDECAR._normalize_style("cartoon"), "cartoon")
        self.assertEqual(SIDECAR._normalize_style("sculpture"), "sculpture")
        self.assertEqual(SIDECAR._normalize_style("custom"), "custom")
        self.assertEqual(SIDECAR._normalize_style("q_cartoon"), "cartoon")
        self.assertEqual(SIDECAR._normalize_style("low_poly"), "low_poly")
        self.assertEqual(SIDECAR._normalize_style("cel_shaded"), "cartoon")
        self.assertEqual(SIDECAR._normalize_style("enamel_inlay"), "realistic")
        with self.assertRaises(SIDECAR.RequestError):
            SIDECAR._normalize_style("classical")

    def test_image_instruction_is_optional(self):
        self.assertEqual(SIDECAR._normalize_image_instruction(None), SIDECAR.DEFAULT_IMAGE_INSTRUCTION)
        self.assertEqual(SIDECAR._normalize_image_instruction("   "), SIDECAR.DEFAULT_IMAGE_INSTRUCTION)
        self.assertEqual(SIDECAR._normalize_image_instruction(" preserve pose "), "preserve pose")
        self.assertIn("Preserve the exact crop, framing", SIDECAR.DEFAULT_IMAGE_INSTRUCTION)
        self.assertIn("do not add, remove, reveal, reconstruct, or extend anything", SIDECAR.DEFAULT_IMAGE_INSTRUCTION)

    def test_user_image_instruction_keeps_internal_default_out_of_the_ui(self):
        self.assertEqual(SIDECAR._user_image_instruction(None), "")
        self.assertEqual(SIDECAR._user_image_instruction("   "), "")
        self.assertEqual(SIDECAR._user_image_instruction(" preserve pose "), "preserve pose")


class ObjGenerationTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.output_root = Path(self.temporary_directory.name) / "generated_models"
        self.environment = mock.patch.dict(os.environ, {"ORCASLICER_AI_OUTPUT_DIR": str(self.output_root)})
        self.environment.start()
        self.palette = ("#FF0000", "#00FF00", "#0000FF", "#FFFF00")
        self.job = SIDECAR._new_job("text", self.palette)

    def tearDown(self):
        self.environment.stop()
        self.temporary_directory.cleanup()

    def _provider_gateway(self, generation_id="generation-id", conversion_id="existing-conversion"):
        gateway = mock.Mock()
        gateway.start_or_reuse_model_task.return_value = mock.Mock(
            provider="tripo", task_id=generation_id, reused=False
        )
        gateway.start_or_reuse_texture_task.return_value = mock.Mock(
            provider="tripo", task_id=generation_id, reused=False
        )
        gateway.start_or_reuse_conversion.return_value = mock.Mock(
            provider="tripo", task_id=conversion_id, reused=True
        )
        gateway.wait_for_task.return_value = {}
        return gateway

    def _paid_authorization(self):
        return SIDECAR.PaidTaskAuthorization.confirmed(f"{self.job.id}:model:1")

    def test_multicolor_generation_uses_silhouette_clean_detail_reference(self):
        raw = self.job.directory / "style-preview-raw.png"
        exact = self.job.directory / "model-reference.png"
        raw.write_bytes(b"raw")
        exact.write_bytes(b"exact")
        self.job.raw_preview_path = raw
        self.job.model_reference_path = exact

        self.assertEqual(SIDECAR._model_generation_reference(self.job), exact)

        self.job.palette = ()
        self.assertEqual(SIDECAR._model_generation_reference(self.job), exact)

    def test_identity_first_portrait_geometry_supports_one_through_six_colors(self):
        self.job.source = "image"
        self.job.generation_profile = "quality"
        self.job.image_metrics = {
            "portrait_geometry": {"detected": True, "evidence": "source_face_lock"}
        }
        original = self.job.directory / "original.png"
        original.write_bytes(b"image")
        self.job.input_path = original
        colors = ("#111111", "#333333", "#555555", "#777777", "#999999", "#BBBBBB")

        for style in ("realistic", "portrait_sketch"):
            for count in range(1, 7):
                with self.subTest(style=style, count=count):
                    self.job.style = style
                    self.job.palette = colors[:count]
                    self.assertTrue(SIDECAR._identity_preserving_portrait_geometry_enabled(self.job))

    def test_identity_first_portrait_geometry_requires_independent_portrait_evidence(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        original = self.job.directory / "original.png"
        original.write_bytes(b"image")
        self.job.input_path = original
        self.job.image_metrics = {"portrait_geometry": {"detected": False}}

        self.assertFalse(SIDECAR._identity_preserving_portrait_geometry_enabled(self.job))
        self.job.image_metrics["portrait_geometry"]["detected"] = True
        self.job.palette = tuple(f"#{index:06X}" for index in range(7))
        self.assertTrue(SIDECAR._identity_preserving_portrait_geometry_enabled(self.job))
        self.job.palette = ()
        self.assertTrue(SIDECAR._identity_preserving_portrait_geometry_enabled(self.job))

    def _write_package(self, archive, *, obj=None, mtl=None, texture=True, extra=None):
        obj = obj or (
            "mtllib model.mtl\n"
            "v 0 0 0\n"
            "v 1 0 0\n"
            "v 0 1 0\n"
            "v 0 0 1\n"
            "vt 0 1\n"
            "vt 1 1\n"
            "vt 0 0\n"
            "vt 1 0\n"
            "usemtl painted\n"
            "f 1/1 3/3 2/2\n"
            "f 1/4 2/2 4/3\n"
            "f 1/1 4/3 3/3\n"
            "f 2/2 3/3 4/3\n"
        )
        mtl = mtl or "newmtl painted\nmap_Kd base.png\n"
        texture_path = Path(self.temporary_directory.name) / "base.png"
        image = Image.new("RGB", (2, 2))
        image.putdata([(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)])
        image.save(texture_path)
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr("model.obj", obj)
            bundle.writestr("model.mtl", mtl)
            if texture:
                bundle.write(texture_path, "base.png")
            for name, data in extra or []:
                bundle.writestr(name, data)

    def _vertex_color_torus(
        self, *, omit_faces=(), add_tiny_component=False, duplicate_faces=(), major_segments=8, minor_segments=8
    ):
        vertices = []
        faces = []
        for major in range(major_segments):
            u = 2.0 * math.pi * major / major_segments
            for minor in range(minor_segments):
                v = 2.0 * math.pi * minor / minor_segments
                radius = 10.0 + 3.0 * math.cos(v)
                vertices.append((radius * math.cos(u), radius * math.sin(u), 3.0 * math.sin(v)))
        for major in range(major_segments):
            next_major = (major + 1) % major_segments
            for minor in range(minor_segments):
                next_minor = (minor + 1) % minor_segments
                a = major * minor_segments + minor
                b = next_major * minor_segments + minor
                c = next_major * minor_segments + next_minor
                d = major * minor_segments + next_minor
                faces.extend(((a, b, c), (a, c, d)))
        faces = [face for index, face in enumerate(faces) if index not in set(omit_faces)]
        faces.extend(faces[index] for index in duplicate_faces)
        if add_tiny_component:
            start = len(vertices)
            vertices.extend(((0, 0, 0), (0.1, 0, 0), (0, 0.1, 0), (0, 0, 0.1)))
            faces.extend(
                (
                    (start, start + 2, start + 1),
                    (start, start + 1, start + 3),
                    (start, start + 3, start + 2),
                    (start + 1, start + 2, start + 3),
                )
            )
        vertex_lines = [f"v {x:.9g} {y:.9g} {z:.9g} 1 0 0" for x, y, z in vertices]
        face_lines = [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]
        return "\n".join(vertex_lines + face_lines) + "\n"

    def _vertex_color_grid(self, size, color_for_vertex):
        vertices = [
            f"v {x} {y} 0 {color_for_vertex(x, y)}"
            for y in range(size)
            for x in range(size)
        ]
        faces = []
        for y in range(size - 1):
            for x in range(size - 1):
                left = y * size + x
                right = left + 1
                upper = left + size
                upper_right = upper + 1
                faces.extend(((left, upper, right), (right, upper, upper_right)))
        return "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n"

    def test_new_job_uses_persistent_output_directory(self):
        self.assertEqual(self.job.directory.parent, self.output_root.resolve())
        self.assertTrue(self.job.directory.is_dir())
        self.assertTrue((self.job.directory / SIDECAR.JOB_STATE_FILENAME).is_file())

    def test_ready_job_round_trips_through_persistent_state(self):
        preview = self.job.directory / "preview.png"
        preview.write_bytes(b"\x89PNG\r\n\x1a\npreview")
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="ascii")
        self.job.prepared_prompt = "persistent printable object"
        self.job.user_prompt = "original object"
        self.job.preview_path = preview
        self.job.preview_content_type = "image/png"
        self.job.artifact_path = artifact
        self.job.artifact_format = "obj"
        self.job.state = "ready"
        self.job.phase = "ready"
        SIDECAR._persist_job(self.job)

        restored = SIDECAR._load_job(self.job.directory)

        self.assertIsNotNone(restored)
        self.assertEqual(restored.state, "ready")
        self.assertEqual(restored.prepared_prompt, "persistent printable object")
        self.assertEqual(restored.user_prompt, "original object")
        self.assertEqual(restored.preview_path, preview)
        self.assertEqual(restored.artifact_path, artifact)
        self.assertEqual(restored.palette, self.palette)

    def test_restore_reuses_paid_generation_reference_without_creating_a_new_task(self):
        self.job.state = "running"
        self.job.phase = "generating"
        self.job.prepared_prompt = "resume printable object"
        self.job.attempts = [{"attempt": 1, "generation_task_id": "paid-generation-id", "status": "running"}]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "queued")
        self.assertEqual(restored.phase, "resuming")
        submit.assert_called_once_with(restored, SIDECAR._generate_job, "resume printable object", True)

    def test_restore_does_not_resume_an_explicitly_stopping_job(self):
        self.job.state = "stopping"
        self.job.phase = "stopping"
        self.job.progress = 98
        self.job.prepared_prompt = "do not resume this paid task"
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "already-paid-generation-id",
            "status": "running",
        }]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "stopped")
        self.assertEqual(restored.phase, "stopped")
        self.assertEqual(restored.progress, 0)
        self.assertIsNone(restored.artifact_path)
        submit.assert_not_called()

    def test_restore_keeps_persisted_preview_visual_blocker(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.state = "awaiting_confirmation"
        self.job.phase = "awaiting_confirmation"
        original = self.job.directory / "input.png"
        preview = self.job.directory / "preview.png"
        model_reference = self.job.directory / "model-reference.png"
        for path in (original, preview, model_reference):
            image = Image.new("RGB", (512, 512), (236, 238, 240))
            image.paste((80, 110, 95), (96, 64, 416, 448))
            image.save(path)
        self.job.input_path = original
        self.job.preview_path = preview
        self.job.model_reference_path = model_reference
        self.job.image_metrics = {
            "preview_visual_quality": {
                "status": "review",
                "score": 83,
                "model_generation_recommended": False,
                "blocking_warnings": ["preview_base_mixing"],
            }
        }
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        def assess_model(job):
            quality = {"score": 100.0, "model_input_eligible": True, "blockers": []}
            job.image_metrics["model_input_quality"] = quality
            return quality

        def assess_generation(job):
            quality = {"score": 100.0, "model_input_eligible": True, "blockers": []}
            job.image_metrics["generation_input_quality"] = quality
            return quality

        with mock.patch.object(SIDECAR, "_assess_job_model_reference", side_effect=assess_model), \
             mock.patch.object(SIDECAR, "_assess_job_generation_reference", side_effect=assess_generation), \
             mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertFalse(restored.image_metrics["model_input_quality"]["model_input_eligible"])
        self.assertFalse(restored.image_metrics["generation_input_quality"]["model_input_eligible"])
        self.assertEqual(
            restored.image_metrics["model_input_quality"]["blockers"][0],
            "preview_base_mixing",
        )
        self.assertIn("pedestal", restored.message.lower())
        submit.assert_not_called()

    def test_restore_never_repeats_ambiguous_paid_request_without_saved_reference(self):
        self.job.state = "running"
        self.job.phase = "generating"
        self.job.prepared_prompt = "ambiguous printable object"
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "failed")
        self.assertIn("Start a new generation manually", restored.message)
        submit.assert_not_called()

    def test_restore_reuses_paid_texture_task_without_creating_another_task(self):
        reference = self.job.directory / "model-reference.png"
        image = Image.new("RGB", (512, 512), "white")
        image.paste((80, 45, 30), (96, 64, 416, 448))
        image.save(reference)
        self.job.source = "image"
        self.job.input_path = reference
        self.job.model_reference_path = reference
        self.job.state = "running"
        self.job.phase = "texturing"
        self.job.attempts = [{
            "attempt": 1,
            "provider_operation": "model_texture",
            "source_job_id": "source-job",
            "source_task_id": "source-task",
            "generation_task_id": "paid-texture-task",
            "status": "running",
        }]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "queued")
        self.assertEqual(restored.phase, "resuming")
        submit.assert_called_once_with(
            restored,
            SIDECAR._retexture_job,
            "source-job",
            "source-task",
            True,
        )

    def test_restore_retries_download_with_existing_remote_ids(self):
        self.job.state = "failed"
        self.job.phase = "failed"
        self.job.progress = 95
        self.job.prepared_prompt = "resume artifact download"
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
            "status": "rejected",
            "error": "Tripo returned an unsafe artifact location.",
        }]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "queued")
        self.assertEqual(restored.phase, "resuming")
        submit.assert_called_once_with(restored, SIDECAR._generate_job, "resume artifact download", True)

    def test_restore_retries_invalid_obj_package_with_existing_remote_ids(self):
        self.job.state = "failed"
        self.job.phase = "failed"
        self.job.progress = 95
        self.job.prepared_prompt = "resume invalid local package"
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
            "status": "rejected",
            "error": "Tripo returned an invalid OBJ package.",
        }]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "queued")
        self.assertEqual(restored.phase, "resuming")
        submit.assert_called_once_with(restored, SIDECAR._generate_job, "resume invalid local package", True)

    def test_face_limit_accepts_lower_adaptive_meshes_and_warns_about_overshoot(self):
        for face_count, face_limit in (
            (1, 100000),
            (90000, 100000),
            (95338, 100000),
            (125000, 100000),
            (1000, 300000),
            (270000, 300000),
            (375000, 300000),
        ):
            with self.subTest(face_count=face_count, face_limit=face_limit):
                self.assertEqual(SIDECAR._validate_face_target(face_count, face_limit), "")

        for face_count, face_limit in (
            (125001, 100000),
            (375001, 300000),
        ):
            with self.subTest(face_count=face_count, face_limit=face_limit):
                self.assertIn("triangles", SIDECAR._validate_face_target(face_count, face_limit))

    def test_restore_accepts_legacy_strict_face_error_without_new_paid_task(self):
        self.job.state = "failed"
        self.job.phase = "failed"
        self.job.face_limit = 100000
        self.job.progress = 95
        self.job.prepared_prompt = "resume accepted near-target artifact"
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
            "status": "rejected",
            "error": "The generated OBJ contains 95338 triangles; at least 100000 are required.",
        }]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "queued")
        self.assertEqual(restored.phase, "resuming")
        submit.assert_called_once_with(
            restored,
            SIDECAR._generate_job,
            "resume accepted near-target artifact",
            True,
        )

    def test_restore_rejects_legacy_face_error_outside_tolerance(self):
        self.assertFalse(
            SIDECAR._legacy_face_error_is_recoverable(
                "The generated OBJ contains 85000 triangles; at least 100000 are required.",
                100000,
            )
        )

    def test_restore_rechecks_high_quality_model_rejected_by_stale_one_million_gate(self):
        self.job.state = "failed"
        self.job.phase = "failed"
        self.job.face_limit = 2000000
        self.job.progress = 99
        self.job.prepared_prompt = "resume high-detail local validation"
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
            "status": "rejected",
            "error": "The generated OBJ failed the structural quality gate: too_many_faces.",
        }]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "queued")
        self.assertEqual(restored.phase, "resuming")
        submit.assert_called_once_with(
            restored,
            SIDECAR._generate_job,
            "resume high-detail local validation",
            True,
        )

    def test_resumed_download_reuses_valid_local_obj(self):
        attempt_directory = self.job.directory / "attempt-01"
        attempt_directory.mkdir()
        candidate = attempt_directory / "model-vertex-color.obj"
        candidate.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="ascii")
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
        }]

        gateway = self._provider_gateway()
        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_validate_artifact") as validate,
        ):
            result = SIDECAR._download_conversion(self.job, "existing-generation", "obj", 1, True)

        self.assertEqual(result, candidate)
        validate.assert_called_once_with(candidate, "obj", allow_repairable_obj=True)
        gateway.wait_for_task.assert_not_called()
        gateway.download_artifact.assert_not_called()

    def test_cached_high_detail_obj_refreshes_stale_face_limit_report(self):
        candidate = self.job.directory / "model-vertex-color.obj"
        candidate.write_text(
            "v 0 0 0 1 0 0\n"
            "v 1 0 0 1 0 0\n"
            "v 0 1 0 1 0 0\n"
            "v 0 0 1 1 0 0\n"
            "f 1 3 2\n"
            "f 1 2 4\n"
            "f 2 3 4\n"
            "f 3 1 4\n",
            encoding="ascii",
        )
        report_path = self.job.directory / SIDECAR.MODEL_QUALITY_FILENAME
        report_path.write_text(json.dumps({
            "gate_version": "structural-v10",
            "status": "reject",
            "errors": ["too_many_faces"],
            "thresholds": {"max_faces": 1000000},
        }), encoding="utf-8")

        SIDECAR._refresh_stale_face_limit_report(candidate, self.palette)

        refreshed = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(refreshed["gate_version"], SIDECAR.MODEL_QUALITY_GATE_VERSION)
        self.assertEqual(refreshed["thresholds"]["max_faces"], SIDECAR.MAX_MODEL_FACES)
        self.assertNotEqual(refreshed["status"], "reject")

    def test_resumed_download_uses_new_recovery_workspace_when_local_obj_is_invalid(self):
        attempt_directory = self.job.directory / "attempt-01"
        attempt_directory.mkdir()
        candidate = attempt_directory / "model-vertex-color.obj"
        candidate.write_text("invalid", encoding="ascii")
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
        }]

        def write_download(_result, destination, _limit):
            destination.write_bytes(b"download")

        recovered = attempt_directory / "recovery-01" / "model-vertex-color.obj"
        gateway = self._provider_gateway()
        gateway.wait_for_task.return_value = {"output": {}}
        gateway.download_artifact.side_effect = write_download
        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_validate_artifact", side_effect=SIDECAR.TripoError("invalid")),
            mock.patch.object(SIDECAR, "_prepare_obj_artifact", return_value=recovered) as prepare,
        ):
            result = SIDECAR._download_conversion(self.job, "existing-generation", "obj", 1, True)

        self.assertEqual(result, recovered)
        gateway.wait_for_task.assert_called_once()
        destination = gateway.download_artifact.call_args.args[1]
        self.assertEqual(destination.parent, attempt_directory / "recovery-01")
        prepare.assert_called_once_with(
            destination,
            attempt_directory / "recovery-01",
            self.job.palette,
            self.job.palette_roles,
            False,
            None,
            mock.ANY,
        )

    def test_generation_downloads_obj_artifact(self):
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="utf-8")

        gateway = self._provider_gateway()
        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_download_generation_artifact", return_value=artifact) as download,
            mock.patch.object(SIDECAR, "_validate_obj_topology", return_value=(300000, 2, 0)),
        ):
            SIDECAR._generate_job(self.job, "printable object", False, self._paid_authorization())

        download.assert_called_once_with(self.job, "generation-id", 1, False)
        self.assertEqual(self.job.state, "ready")
        self.assertEqual(self.job.artifact_format, "obj")
        self.assertEqual(self.job.artifact_path, artifact)

    def test_generation_publishes_color_intent_bound_to_final_obj(self):
        raw = self.job.directory / "style-preview-raw.png"
        material = self.job.directory / "preview.png"
        source = Image.new("RGB", (512, 512), (210, 180, 160))
        source.paste((55, 70, 95), (64, 64, 448, 448))
        source.save(raw)
        mapped = Image.new("RGB", (512, 512), self.palette[0])
        for index, color in enumerate(self.palette):
            mapped.paste(color, (index * 128, 0, (index + 1) * 128, 512))
        mapped.save(material)
        self.job.raw_preview_path = raw
        self.job.model_reference_path = raw
        self.job.preview_path = material
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="utf-8")
        gateway = self._provider_gateway()

        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_download_generation_artifact", return_value=artifact),
            mock.patch.object(SIDECAR, "_validate_obj_topology", return_value=(300000, 2, 0)),
            mock.patch.object(SIDECAR, "_automatic_visual_review", return_value=None),
        ):
            SIDECAR._generate_job(self.job, "printable object", False, self._paid_authorization())

        self.assertEqual(self.job.state, "ready")
        self.assertIsNone(self.job.color_intent_path)
        self.assertEqual(self.job.palette, ())
        self.assertEqual(artifact.read_text(encoding="utf-8"), "v 0 0 0 1 0 0\nf 1 1 1\n")

    def test_accepted_attempt_publishes_structural_report_with_final_artifact(self):
        attempt = self.job.directory / "attempt-01"
        attempt.mkdir()
        candidate = attempt / "model-vertex-color.obj"
        candidate.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="ascii")
        quality = {"status": "review", "warnings": ["thin_local_wall_regions"]}
        metrics = {"vertex_count": 1, "face_count": 1}
        (attempt / SIDECAR.MODEL_QUALITY_FILENAME).write_text(
            json.dumps(quality), encoding="utf-8"
        )
        (attempt / "vertex-color-metrics.json").write_text(
            json.dumps(metrics), encoding="utf-8"
        )
        artifact = self.job.directory / "model-vertex-color.obj"

        SIDECAR._promote_attempt_artifact(candidate, artifact)

        self.assertEqual(artifact.read_text(encoding="ascii"), candidate.read_text(encoding="ascii"))
        self.assertEqual(
            json.loads((self.job.directory / SIDECAR.MODEL_QUALITY_FILENAME).read_text(encoding="utf-8")),
            quality,
        )
        self.assertEqual(
            json.loads((self.job.directory / "vertex-color-metrics.json").read_text(encoding="utf-8")),
            metrics,
        )

    def test_retexture_preserves_geometry_task_and_creates_one_texture_task(self):
        reference = self.job.directory / "model-reference.png"
        image = Image.new("RGB", (512, 512), "white")
        image.paste((80, 45, 30), (96, 64, 416, 448))
        image.save(reference)
        self.job.source = "image"
        self.job.model_reference_path = reference
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="utf-8")
        gateway = self._provider_gateway("texture-task")
        authorization = SIDECAR.PaidTaskAuthorization.confirmed_texture(f"{self.job.id}:texture:1")

        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_download_generation_artifact", return_value=artifact) as download,
            mock.patch.object(SIDECAR, "_validate_obj_topology", return_value=(300000, 1, 0)),
        ):
            SIDECAR._retexture_job(
                self.job,
                "geometry-job",
                "geometry-task",
                False,
                authorization,
            )

        request = gateway.start_or_reuse_texture_task.call_args.args[0]
        self.assertEqual(request.source_task_id, "geometry-task")
        self.assertEqual(request.image_path, reference)
        self.assertEqual(request.texture_alignment, "geometry")
        self.assertEqual(request.texture_quality, "standard")
        download.assert_called_once_with(self.job, "texture-task", 1, False)
        self.assertEqual(self.job.state, "ready")
        self.assertEqual(self.job.attempts[0]["provider_operation"], "model_texture")
        self.assertEqual(self.job.attempts[0]["source_job_id"], "geometry-job")
        self.assertEqual(self.job.attempts[0]["source_task_id"], "geometry-task")

    def test_multicolor_image_generation_submits_raw_design_without_printer_palette(self):
        self.job.source = "image"
        raw = self.job.directory / "style-preview-raw.png"
        exact = self.job.directory / "model-reference.png"
        image = Image.new("RGB", (512, 512), "white")
        image.paste((180, 80, 40), (64, 48, 448, 464))
        image.save(raw)
        image.save(exact)
        self.job.raw_preview_path = raw
        self.job.model_reference_path = exact
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="utf-8")
        gateway = self._provider_gateway()

        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_download_generation_artifact", return_value=artifact),
            mock.patch.object(SIDECAR, "_validate_obj_topology", return_value=(300000, 1, 0)),
        ):
            SIDECAR._generate_job(self.job, "", False, self._paid_authorization())

        request = gateway.start_or_reuse_model_task.call_args.args[0]
        self.assertEqual(request.source, "image")
        self.assertEqual(request.image_path, raw)
        self.assertEqual(self.job.palette, ())

    def test_quality_realistic_portrait_submits_identity_front_as_one_model_task(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.face_limit = 1000000
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        original = self.job.directory / "original.png"
        reference = self.job.directory / "model-reference.png"
        geometry_reference = self.job.directory / "geometry-reference.png"
        original_image = Image.new("RGB", (512, 512), "white")
        original_image.paste((80, 120, 90), (96, 64, 416, 464))
        original_image.save(original)
        reference_image = Image.new("RGB", (512, 512), "white")
        reference_image.paste((180, 110, 80), (96, 64, 416, 464))
        reference_image.save(reference)
        geometry_image = Image.new("RGB", (512, 512), "white")
        geometry_image.paste((145, 105, 95), (96, 64, 416, 464))
        geometry_image.save(geometry_reference)
        self.job.input_path = original
        self.job.model_reference_path = reference
        self.job.geometry_reference_path = geometry_reference
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="utf-8")
        gateway = self._provider_gateway()

        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_ensure_portrait_multiview") as multiview,
            mock.patch.object(SIDECAR, "_download_generation_artifact", return_value=artifact),
            mock.patch.object(SIDECAR, "_validate_obj_topology", return_value=(935200, 1, 0)),
        ):
            SIDECAR._generate_job(self.job, "", False, self._paid_authorization())

        request = gateway.start_or_reuse_model_task.call_args.args[0]
        self.assertEqual(request.source, "image")
        self.assertEqual(
            request.image_path,
            self.job.directory / SIDECAR.PORTRAIT_GEOMETRY_PROVIDER_FILENAME,
        )
        with Image.open(request.image_path) as provider_reference:
            self.assertEqual(provider_reference.size[0], provider_reference.size[1])
        self.assertIsNone(request.image_paths)
        multiview.assert_not_called()
        gateway.start_or_reuse_model_task.assert_called_once()

    def test_quality_realistic_portrait_uses_sculptural_identity_front_without_generated_views(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.face_limit = 2000000
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        original = self.job.directory / "original.png"
        reference = self.job.directory / "model-reference.png"
        geometry_reference = self.job.directory / "geometry-reference.png"
        original_image = Image.new("RGB", (512, 768), "white")
        original_image.paste((80, 120, 90), (96, 80, 416, 704))
        original_image.save(original)
        reference_image = Image.new("RGB", (512, 768), "white")
        reference_image.paste((180, 110, 80), (96, 80, 416, 704))
        reference_image.save(reference)
        geometry_image = Image.new("RGB", (512, 768), "white")
        geometry_image.paste((145, 105, 95), (96, 80, 416, 704))
        geometry_image.save(geometry_reference)
        self.job.input_path = original
        self.job.model_reference_path = reference
        self.job.geometry_reference_path = geometry_reference
        SIDECAR._assess_job_generation_reference(self.job)
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="utf-8")
        gateway = self._provider_gateway()

        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_ensure_portrait_multiview") as multiview,
            mock.patch.object(SIDECAR, "_download_generation_artifact", return_value=artifact),
            mock.patch.object(SIDECAR, "_validate_obj_topology", return_value=(1900000, 1, 0)),
        ):
            SIDECAR._generate_job(self.job, "", False, self._paid_authorization())

        request = gateway.start_or_reuse_model_task.call_args.args[0]
        self.assertEqual(request.source, "image")
        self.assertEqual(
            request.image_path,
            self.job.directory / SIDECAR.PORTRAIT_GEOMETRY_PROVIDER_FILENAME,
        )
        with Image.open(request.image_path) as provider_reference:
            self.assertEqual(provider_reference.size, (873, 873))
            self.assertEqual(provider_reference.getpixel((436, 436))[:3], (145, 105, 95))
        self.assertIsNone(request.image_paths)
        multiview.assert_not_called()
        self.assertEqual(
            self.job.image_metrics.get("geometry_strategy", {}).get("version"),
            "portrait-identity-sculpted-front-v7",
        )
        self.assertEqual(
            self.job.image_metrics["geometry_provider_canvas"]["appearance_source"],
            "sculptural_geometry_reference",
        )
        self.assertFalse(self.job.image_metrics["geometry_strategy"]["multiview_geometry"])
        self.assertTrue(
            self.job.image_metrics["geometry_strategy"]["post_generation_material_turntable"]
        )

    def test_head_shoulders_silhouette_removes_matte_and_repairs_open_notch(self):
        portrait = Image.new("RGBA", (180, 220), (0, 0, 0, 0))
        draw = ImageDraw.Draw(portrait)
        draw.ellipse((50, 8, 130, 104), fill=(132, 112, 101, 255))
        draw.polygon(
            ((20, 90), (160, 90), (175, 215), (5, 215)),
            fill=(190, 185, 178, 255),
        )
        # An attached near-white matte is inside the source-photo envelope, so
        # alpha intersection alone cannot remove it.
        draw.rectangle((5, 135, 26, 190), fill=(249, 249, 249, 255))
        # This transparent square is open toward the neck but has real shoulder
        # pixels on its right, matching the production defect generic analysis
        # missed after downsampling.
        draw.rectangle((120, 96, 140, 122), fill=(0, 0, 0, 0))
        source = Image.new("L", portrait.size, 0)
        source_draw = ImageDraw.Draw(source)
        source_draw.ellipse((48, 6, 132, 106), fill=255)
        source_draw.polygon(((12, 88), (168, 88), (179, 218), (1, 218)), fill=255)

        repaired, alpha, report = SIDECAR._repair_portrait_head_shoulders_silhouette(
            portrait,
            portrait.getchannel("A"),
            source,
            protected_bounds=(50, 8, 131, 105),
        )

        self.assertEqual(report["status"], "pass")
        self.assertTrue(report["source_mask_used"])
        self.assertGreater(report["removed_external_pixels"], 0)
        self.assertGreater(report["filled_notch_pixels"], 0)
        self.assertEqual(report["remaining_row_gap_pixels"], 0)
        self.assertEqual(report["identity_rgb_pixels_changed"], 0)
        self.assertEqual(alpha.getpixel((10, 150)), 0)
        self.assertEqual(alpha.getpixel((130, 108)), 255)
        self.assertEqual(repaired.getpixel((90, 50)), portrait.getpixel((90, 50)))

    def test_quality_portrait_uses_native_sculpted_head_and_one_clean_plinth(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.face_limit = 2000000
        self.job.image_metrics["portrait_skin_cleanup"] = {
            "activated": 1,
            "face_bounds": {"left": 176, "right": 336, "top": 80, "bottom": 360},
            "base_bounds": {"left": 96, "right": 416, "top": 650, "bottom": 734},
        }
        self.job.image_metrics["geometry_silhouette_cleanup"] = {
            "version": "portrait-silhouette-v6",
            "status": "not_needed",
            "alpha_synced": True,
        }
        original = self.job.directory / "original.png"
        reference = self.job.directory / "model-reference.png"
        geometry_reference = self.job.directory / "geometry-reference.png"
        clean_preview = self.job.directory / "clean-preview.png"
        source_photo = Image.new("RGB", (512, 768), "white")
        ImageDraw.Draw(source_photo).rectangle(
            (96, 80, 415, 649), fill=(80, 70, 65)
        )
        source_photo.save(original)
        geometry = Image.new("RGBA", (512, 768), (0, 0, 0, 0))
        geometry.paste((145, 105, 95, 255), (96, 80, 416, 650))
        geometry.paste((25, 24, 26, 255), (96, 650, 416, 734))
        geometry.save(geometry_reference)
        color = Image.new("RGBA", (512, 768), (0, 0, 0, 0))
        color.paste((180, 110, 80, 255), (96, 80, 416, 650))
        color.paste((25, 24, 26, 255), (96, 650, 416, 734))
        color.save(reference)
        color.save(clean_preview)
        self.job.input_path = original
        self.job.model_reference_path = reference
        self.job.geometry_reference_path = geometry_reference
        self.job.preview_path = clean_preview
        self.job.palette_roles = {"structure": self.palette[1]}

        SIDECAR._assess_job_generation_reference(self.job)

        canvas = self.job.image_metrics["geometry_provider_canvas"]
        self.assertEqual(canvas["version"], "square-transparent-black-head-shoulders-v9")
        self.assertEqual(canvas["source_size"], [512, 768])
        compaction = canvas["portrait_compaction"]
        self.assertEqual(compaction["crop_bounds"], [50, 80, 462, 452])
        self.assertEqual(compaction["base_source"], "single_solid_structure_plinth")
        self.assertTrue(compaction["removed_original_base"])
        self.assertFalse(compaction["identity_pixels_resampled"])
        self.assertGreaterEqual(compaction["face_provider_ratio"], 0.55)
        self.assertEqual(compaction["shoulder_silhouette"]["status"], "pass")
        self.assertEqual(
            compaction["shoulder_silhouette"]["remaining_row_gap_pixels"], 0
        )
        self.assertEqual(
            self.job.image_metrics["geometry_strategy"]["version"],
            "portrait-sculpted-head-shoulders-front-v15",
        )
        self.assertEqual(canvas["appearance_source"], "sculptural_geometry_reference")
        provider_path = self.job.directory / SIDECAR.PORTRAIT_GEOMETRY_PROVIDER_FILENAME
        with Image.open(provider_path) as provider:
            self.assertEqual(provider.size[0], provider.size[1])
            crop_left, crop_top, _, _ = compaction["crop_bounds"]
            offset_x, offset_y = canvas["offset"]
            # The relief-rich face is copied byte-for-byte, not resampled,
            # recolored from the natural image, or regenerated.
            provider_point = (
                offset_x + 256 - crop_left,
                offset_y + 200 - crop_top,
            )
            self.assertEqual(provider.getpixel(provider_point), (145, 105, 95, 255))
        self.assertEqual(
            self.job.preview_path,
            self.job.directory / SIDECAR.PORTRAIT_HEAD_PREVIEW_FILENAME,
        )
        with Image.open(self.job.preview_path) as printable_preview:
            self.assertEqual(printable_preview.size, provider.size)
            base_left, base_top, base_right, _ = compaction["base_bounds"]
            base_point = (
                canvas["offset"][0] + (base_left + base_right) // 2,
                canvas["offset"][1] + base_top + 2,
            )
            self.assertEqual(printable_preview.getpixel(base_point), (0, 255, 0, 255))

    def test_paid_portrait_attempt_freezes_the_exact_existing_provider_canvas(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        self.job.image_metrics["geometry_provider_canvas"] = {
            "version": "square-transparent-black-v2",
            "output_size": [640, 640],
            "appearance_source": "identity_color_model_reference",
        }
        self.job.attempts = [{"attempt": 1, "generation_task_id": "paid-task"}]
        original = self.job.directory / "original.png"
        reference = self.job.directory / "model-reference.png"
        geometry_reference = self.job.directory / "geometry-reference.png"
        Image.new("RGB", (512, 768), "white").save(original)
        Image.new("RGBA", (512, 768), (180, 110, 80, 255)).save(reference)
        Image.new("RGBA", (512, 768), (145, 105, 95, 255)).save(geometry_reference)
        self.job.input_path = original
        self.job.model_reference_path = reference
        self.job.geometry_reference_path = geometry_reference
        provider_path = self.job.directory / SIDECAR.PORTRAIT_GEOMETRY_PROVIDER_FILENAME
        Image.new("RGBA", (640, 640), (7, 8, 9, 255)).save(provider_path)

        selected = SIDECAR._prepare_portrait_geometry_provider_reference(self.job)

        self.assertEqual(selected, provider_path)
        with Image.open(provider_path) as frozen:
            self.assertEqual(frozen.getpixel((320, 320)), (7, 8, 9, 255))
        self.assertEqual(
            self.job.image_metrics["geometry_provider_canvas"]["version"],
            "square-transparent-black-v2",
        )

    def test_compact_portrait_without_source_silhouette_is_blocked_before_payment(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.image_metrics["portrait_skin_cleanup"] = {
            "activated": 1,
            "face_bounds": {"left": 176, "right": 336, "top": 80, "bottom": 360},
            "base_bounds": {"left": 96, "right": 416, "top": 650, "bottom": 734},
        }
        self.job.image_metrics["geometry_silhouette_cleanup"] = {
            "version": "portrait-silhouette-v6",
            "status": "not_needed",
            "alpha_synced": True,
        }
        original = self.job.directory / "original.png"
        reference = self.job.directory / "model-reference.png"
        geometry_reference = self.job.directory / "geometry-reference.png"
        preview = self.job.directory / "clean-preview.png"
        # A blank source cannot independently verify an Image2 shoulder mask.
        Image.new("RGB", (512, 768), "white").save(original)
        geometry = Image.new("RGBA", (512, 768), (0, 0, 0, 0))
        geometry.paste((145, 105, 95, 255), (96, 80, 416, 650))
        geometry.paste((25, 24, 26, 255), (96, 650, 416, 734))
        geometry.save(geometry_reference)
        color = Image.new("RGBA", (512, 768), (0, 0, 0, 0))
        color.paste((180, 110, 80, 255), (96, 80, 416, 650))
        color.save(reference)
        color.save(preview)
        self.job.input_path = original
        self.job.model_reference_path = reference
        self.job.geometry_reference_path = geometry_reference
        self.job.preview_path = preview
        self.job.palette_roles = {"structure": self.palette[1]}

        quality = SIDECAR._assess_job_generation_reference(self.job)

        self.assertFalse(quality["model_input_eligible"])
        self.assertEqual(
            quality["blockers"][0],
            "portrait_shoulder_silhouette_unverified",
        )
        repair = self.job.image_metrics["geometry_provider_canvas"][
            "portrait_compaction"
        ]["shoulder_silhouette"]
        self.assertEqual(repair["status"], "unverified")
        self.assertFalse(repair["source_mask_used"])

    def test_sculptural_identity_front_inherits_hard_subject_alpha_and_clears_hidden_background(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        original = self.job.directory / "original.png"
        reference = self.job.directory / "model-reference.png"
        geometry_reference = self.job.directory / "geometry-reference.png"
        subject_mask = self.job.directory / "mask-subject.png"
        Image.new("RGB", (512, 768), "white").save(original)
        Image.new("RGBA", (512, 768), (255, 255, 255, 0)).save(reference)
        mask = Image.new("L", (512, 768), 0)
        mask.paste(255, (128, 64, 384, 704))
        mask.save(subject_mask)
        geometry = Image.new("RGBA", (512, 768), (238, 238, 238, 0))
        geometry.paste((145, 105, 95, 255), (128, 64, 384, 704))
        # The alpha is already correct. The regression is the non-zero RGB
        # hidden behind transparent pixels, which an RGB-only provider decoder
        # can otherwise turn into a printable rear plate.
        geometry.save(geometry_reference)
        self.job.input_path = original
        self.job.model_reference_path = reference
        self.job.geometry_reference_path = geometry_reference
        self.job.subject_mask_path = subject_mask

        SIDECAR._assess_job_generation_reference(self.job)

        with Image.open(geometry_reference) as repaired:
            self.assertEqual(repaired.mode, "RGBA")
            alpha = repaired.getchannel("A")
            self.assertEqual(alpha.getpixel((0, 0)), 0)
            self.assertEqual(alpha.getpixel((256, 256)), 255)
            self.assertEqual(set(alpha.get_flattened_data()), {0, 255})
            self.assertEqual(repaired.getpixel((0, 0)), (0, 0, 0, 0))
            self.assertEqual(repaired.getpixel((256, 256)), (145, 105, 95, 255))
        provider_path = self.job.directory / SIDECAR.PORTRAIT_GEOMETRY_PROVIDER_FILENAME
        with Image.open(provider_path) as provider:
            self.assertEqual(provider.mode, "RGBA")
            self.assertEqual(provider.size, (768, 768))
            self.assertEqual(provider.getchannel("A").getbbox(), (256, 64, 512, 704))
            self.assertEqual(provider.getpixel((0, 0)), (0, 0, 0, 0))
            self.assertEqual(provider.getpixel((384, 256)), (145, 105, 95, 255))
        self.assertEqual(
            self.job.image_metrics["geometry_provider_canvas"]["version"],
            "square-transparent-black-v2",
        )
        self.assertEqual(
            self.job.image_metrics["geometry_provider_canvas"]["appearance_source"],
            "sculptural_geometry_reference",
        )
        self.assertEqual(
            self.job.image_metrics["geometry_provider_canvas"]["output_size"],
            [768, 768],
        )

    def test_sculptural_identity_front_removes_attached_head_halo_without_erasing_jacket(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        original = self.job.directory / "original.png"
        reference = self.job.directory / "model-reference.png"
        geometry_reference = self.job.directory / "geometry-reference.png"
        subject_mask = self.job.directory / "mask-subject.png"
        Image.new("RGB", (512, 768), "white").save(original)
        Image.new("RGBA", (512, 768), (0, 0, 0, 0)).save(reference)

        mask = Image.new("L", (512, 768), 0)
        mask_draw = ImageDraw.Draw(mask)
        mask_draw.ellipse((176, 64, 336, 360), fill=255)
        mask_draw.rectangle((220, 300, 292, 430), fill=255)
        mask_draw.polygon(((56, 400), (456, 400), (432, 704), (80, 704)), fill=255)
        mask.save(subject_mask)

        geometry = Image.new("RGBA", (512, 768), (245, 245, 245, 0))
        geometry_draw = ImageDraw.Draw(geometry)
        # The light oval models a fake checkerboard/softbox backdrop attached
        # to the head in the real Image2 result.
        # Medium-gray inner halo is representative of the real opaque
        # checkerboard shadow; near-white-only cleanup leaves it extrudable.
        geometry_draw.ellipse((176, 64, 336, 360), fill=(190, 190, 190, 255))
        geometry_draw.ellipse((200, 72, 312, 340), fill=(218, 164, 132, 255))
        geometry_draw.pieslice((200, 72, 312, 250), 180, 360, fill=(28, 24, 22, 255))
        geometry_draw.rectangle((225, 300, 287, 430), fill=(218, 164, 132, 255))
        # Off-white clothing deliberately touches the outer silhouette. It must
        # survive because cleanup stops at the detected shoulder transition.
        geometry_draw.polygon(
            ((56, 400), (456, 400), (432, 704), (80, 704)),
            fill=(240, 235, 225, 255),
        )
        # Detached opaque dashes otherwise become spikes beside the base.
        mask_draw.rectangle((12, 20, 13, 28), fill=255)
        mask.save(subject_mask)
        geometry_draw.rectangle((12, 20, 13, 28), fill=(160, 160, 160, 255))
        geometry.save(geometry_reference)
        self.job.input_path = original
        self.job.model_reference_path = reference
        self.job.geometry_reference_path = geometry_reference
        self.job.subject_mask_path = subject_mask

        SIDECAR._assess_job_generation_reference(self.job)

        with Image.open(geometry_reference) as repaired:
            alpha = repaired.getchannel("A")
            self.assertEqual(alpha.getpixel((180, 200)), 0)
            self.assertEqual(alpha.getpixel((205, 200)), 255)
            self.assertEqual(alpha.getpixel((80, 500)), 255)
            self.assertEqual(alpha.getpixel((432, 500)), 255)
            self.assertEqual(alpha.getpixel((12, 24)), 0)
        cleanup = self.job.image_metrics["geometry_silhouette_cleanup"]
        self.assertEqual(cleanup["status"], "refined")
        self.assertGreater(cleanup["removed_pixels"], 1000)
        self.assertEqual(cleanup["removed_detached_pixels"], 18)
        self.assertGreaterEqual(cleanup["shoulder_y"], 395)
        self.assertLessEqual(cleanup["shoulder_y"], 405)

        repaired_once = geometry_reference.read_bytes()
        provider_once = (
            self.job.directory / SIDECAR.PORTRAIT_GEOMETRY_PROVIDER_FILENAME
        ).read_bytes()
        SIDECAR._assess_job_generation_reference(self.job)
        self.assertEqual(geometry_reference.read_bytes(), repaired_once)
        self.assertEqual(
            (self.job.directory / SIDECAR.PORTRAIT_GEOMETRY_PROVIDER_FILENAME).read_bytes(),
            provider_once,
        )
        self.assertTrue(
            self.job.image_metrics["geometry_silhouette_cleanup"]["revalidated"]
        )
        self.assertTrue(
            self.job.image_metrics["geometry_silhouette_cleanup"]["alpha_synced"]
        )
        with mock.patch.object(
            SIDECAR.os, "replace", wraps=SIDECAR.os.replace
        ) as replace:
            self.assertEqual(
                SIDECAR._geometry_generation_reference(self.job),
                self.job.directory / SIDECAR.PORTRAIT_GEOMETRY_PROVIDER_FILENAME,
            )
        replace.assert_not_called()

    def test_sculptural_identity_front_uses_source_silhouette_for_dark_halo_and_white_hair(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        original = self.job.directory / "source-portrait.png"
        reference = self.job.directory / "model-reference.png"
        geometry_reference = self.job.directory / "geometry-reference.png"
        subject_mask = self.job.directory / "mask-subject.png"

        source = Image.new("RGB", (512, 768), (170, 180, 190))
        source_draw = ImageDraw.Draw(source)
        source_draw.ellipse((200, 72, 312, 340), fill=(235, 235, 232))
        source_draw.rectangle((225, 300, 287, 430), fill=(218, 164, 132))
        source_draw.polygon(
            ((56, 400), (456, 400), (432, 704), (80, 704)),
            fill=(245, 242, 235),
        )
        source.save(original)
        Image.new("RGBA", (512, 768), (0, 0, 0, 0)).save(reference)

        mask = Image.new("L", (512, 768), 0)
        mask_draw = ImageDraw.Draw(mask)
        mask_draw.ellipse((176, 64, 336, 360), fill=255)
        mask_draw.rectangle((220, 300, 292, 430), fill=255)
        mask_draw.polygon(((56, 400), (456, 400), (432, 704), (80, 704)), fill=255)
        mask.save(subject_mask)

        geometry = Image.new("RGBA", (512, 768), (0, 0, 0, 0))
        geometry_draw = ImageDraw.Draw(geometry)
        # This halo is too dark for a near-white-only rule. The real source
        # silhouette must remove it while retaining deliberately white hair.
        geometry_draw.ellipse((176, 64, 336, 360), fill=(145, 145, 145, 255))
        geometry_draw.ellipse((200, 72, 312, 340), fill=(235, 235, 232, 255))
        geometry_draw.ellipse((210, 145, 302, 325), fill=(218, 164, 132, 255))
        geometry_draw.rectangle((225, 300, 287, 430), fill=(218, 164, 132, 255))
        geometry_draw.polygon(
            ((56, 400), (456, 400), (432, 704), (80, 704)),
            fill=(245, 242, 235, 255),
        )
        geometry.save(geometry_reference)
        self.job.input_path = original
        self.job.model_reference_path = reference
        self.job.geometry_reference_path = geometry_reference
        self.job.subject_mask_path = subject_mask

        SIDECAR._assess_job_generation_reference(self.job)

        with Image.open(geometry_reference) as repaired:
            alpha = repaired.getchannel("A")
            self.assertEqual(alpha.getpixel((180, 200)), 0)
            self.assertEqual(alpha.getpixel((256, 100)), 255)
            self.assertEqual(alpha.getpixel((80, 500)), 255)
        cleanup = self.job.image_metrics["geometry_silhouette_cleanup"]
        self.assertTrue(cleanup["source_mask_used"])
        self.assertGreater(cleanup["source_removed_pixels"], 1000)

    def test_portrait_silhouette_removes_light_source_safety_rim_but_keeps_white_hair(self):
        alpha = Image.new("L", (512, 768), 0)
        alpha_draw = ImageDraw.Draw(alpha)
        # A deliberately simple source-aligned portrait makes the safety rim
        # deterministic: the three-pixel strip is part of the expanded source
        # silhouette but disappears from its eroded core.
        alpha_draw.rectangle((200, 100, 300, 330), fill=255)
        alpha_draw.rectangle((197, 160, 199, 260), fill=255)
        alpha_draw.rectangle((220, 300, 280, 430), fill=255)
        alpha_draw.rectangle((50, 400, 462, 704), fill=255)
        source_alpha = alpha.copy()

        geometry = Image.new("RGBA", (512, 768), (0, 0, 0, 0))
        geometry_draw = ImageDraw.Draw(geometry)
        geometry_draw.rectangle((200, 100, 300, 330), fill=(115, 115, 115, 255))
        # White hair is well inside the source core and must remain intact.
        geometry_draw.rectangle((230, 110, 270, 150), fill=(232, 232, 230, 255))
        # The opaque checkerboard strip touches the silhouette edge and lives
        # entirely in the conservative expansion rim, so it must be removed.
        geometry_draw.rectangle((197, 160, 199, 260), fill=(225, 225, 225, 255))
        geometry_draw.rectangle((220, 300, 280, 430), fill=(180, 135, 110, 255))
        geometry_draw.rectangle((50, 400, 462, 704), fill=(242, 236, 225, 255))

        refined, report = SIDECAR._refine_portrait_head_silhouette(
            geometry,
            alpha,
            source_alpha,
        )

        self.assertEqual(refined.getpixel((198, 200)), 0)
        self.assertEqual(refined.getpixel((250, 130)), 255)
        self.assertEqual(report["source_head_backdrop_removed_pixels"], 303)
        self.assertEqual(report["source_head_backdrop_component_count"], 1)

    def test_portrait_silhouette_removes_neutral_shoulder_backdrop_but_keeps_warm_jacket(self):
        original = self.job.directory / "source-portrait.png"
        source = Image.new("RGB", (512, 768), (170, 180, 190))
        source_draw = ImageDraw.Draw(source)
        source_draw.ellipse((200, 72, 312, 340), fill=(218, 164, 132))
        source_draw.rectangle((225, 300, 287, 430), fill=(218, 164, 132))
        source_draw.polygon(
            ((100, 400), (412, 400), (390, 700), (122, 700)),
            fill=(242, 236, 225),
        )
        source.save(original)

        alpha = Image.new("L", (512, 768), 0)
        alpha_draw = ImageDraw.Draw(alpha)
        alpha_draw.ellipse((176, 64, 336, 360), fill=255)
        alpha_draw.rectangle((220, 300, 292, 430), fill=255)
        alpha_draw.polygon(
            ((100, 400), (412, 400), (390, 700), (122, 700)),
            fill=255,
        )
        # Opaque neutral checkerboard remnants remain connected to each
        # shoulder, so a largest-component pass alone cannot remove them.
        alpha_draw.rectangle((70, 430, 100, 500), fill=255)
        alpha_draw.rectangle((412, 430, 442, 500), fill=255)
        # A warm studio-shadow triangle is attached to the collar. It is not a
        # neutral-white component, but must still be removed before image-to-3D
        # turns it into a plate behind the neck.
        alpha_draw.polygon(((160, 374), (220, 374), (220, 420)), fill=255)

        geometry = Image.new("RGBA", (512, 768), (0, 0, 0, 0))
        geometry_draw = ImageDraw.Draw(geometry)
        geometry_draw.ellipse((176, 64, 336, 360), fill=(185, 185, 185, 255))
        geometry_draw.ellipse((200, 72, 312, 340), fill=(218, 164, 132, 255))
        geometry_draw.rectangle((225, 300, 287, 430), fill=(218, 164, 132, 255))
        geometry_draw.polygon(
            ((100, 400), (412, 400), (390, 700), (122, 700)),
            fill=(242, 236, 225, 255),
        )
        geometry_draw.rectangle((70, 430, 100, 500), fill=(244, 244, 244, 255))
        geometry_draw.rectangle((412, 430, 442, 500), fill=(244, 244, 244, 255))
        geometry_draw.polygon(
            ((160, 374), (220, 374), (220, 420)), fill=(214, 204, 194, 255)
        )

        source_mask = SIDECAR._portrait_source_subject_mask(512, 768, original)
        refined, report = SIDECAR._refine_portrait_head_silhouette(
            geometry,
            alpha,
            Image.frombytes("L", (512, 768), source_mask),
        )

        self.assertEqual(refined.getpixel((80, 470)), 0)
        self.assertEqual(refined.getpixel((430, 470)), 0)
        self.assertEqual(refined.getpixel((180, 385)), 0)
        self.assertEqual(refined.getpixel((150, 500)), 255)
        self.assertEqual(refined.getpixel((360, 500)), 255)
        self.assertGreater(report["light_backdrop_removed_pixels"], 1000)
        self.assertEqual(report["light_backdrop_component_count"], 2)
        self.assertGreater(report["source_neck_removed_pixels"], 100)

    def test_portrait_provider_reference_is_safe_under_status_and_submit_concurrency(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        original = self.job.directory / "original.png"
        reference = self.job.directory / "model-reference.png"
        geometry_reference = self.job.directory / "geometry-reference.png"
        subject_mask = self.job.directory / "mask-subject.png"
        Image.new("RGB", (512, 768), "white").save(original)
        Image.new("RGBA", (512, 768), (0, 0, 0, 0)).save(reference)
        mask = Image.new("L", (512, 768), 0)
        mask.paste(255, (128, 64, 384, 704))
        mask.save(subject_mask)
        geometry = Image.new("RGBA", (512, 768), (240, 240, 240, 0))
        geometry.paste((145, 105, 95, 255), (128, 64, 384, 704))
        geometry.save(geometry_reference)
        self.job.input_path = original
        self.job.model_reference_path = reference
        self.job.geometry_reference_path = geometry_reference
        self.job.subject_mask_path = subject_mask

        with ThreadPoolExecutor(max_workers=8) as executor:
            results = list(executor.map(
                lambda _index: SIDECAR._geometry_generation_reference(self.job),
                range(24),
            ))

        expected = self.job.directory / SIDECAR.PORTRAIT_GEOMETRY_PROVIDER_FILENAME
        self.assertEqual(set(results), {expected})
        with Image.open(expected) as provider:
            provider.verify()
        self.assertFalse(list(self.job.directory.glob("*.tmp")))

    def test_portrait_rear_plate_gate_distinguishes_backdrop_from_normal_torso(self):
        bad_masks = self.job.directory / "bad-masks"
        good_masks = self.job.directory / "good-masks"
        bad_masks.mkdir()
        good_masks.mkdir()

        bad = Image.new("L", (256, 256), 0)
        bad_draw = ImageDraw.Draw(bad)
        bad_draw.ellipse((92, 20, 162, 92), fill=255)
        bad_draw.polygon(((76, 82), (180, 82), (190, 210), (66, 210)), fill=255)
        bad_draw.rectangle((190, 20, 194, 176), fill=255)
        bad_draw.rectangle((48, 210, 208, 230), fill=255)
        good = Image.new("L", (256, 256), 0)
        good_draw = ImageDraw.Draw(good)
        good_draw.ellipse((92, 20, 162, 92), fill=255)
        good_draw.polygon(((76, 82), (180, 82), (190, 210), (66, 210)), fill=255)
        good_draw.rectangle((48, 210, 208, 230), fill=255)
        for view in ("right", "left"):
            bad.save(bad_masks / f"{view}.png")
            good.save(good_masks / f"{view}.png")

        rejected = SIDECAR._review_portrait_rear_plate_masks(bad_masks)
        accepted = SIDECAR._review_portrait_rear_plate_masks(good_masks)

        self.assertEqual(rejected["status"], "reject")
        self.assertTrue(rejected["warnings"])
        self.assertEqual(accepted["status"], "pass")
        self.assertFalse(accepted["warnings"])

    def test_portrait_multiview_sheet_uses_sculptural_identity_front(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        model_reference = self.job.directory / "model-reference.png"
        geometry_reference = self.job.directory / "geometry-reference.png"
        original = self.job.directory / "original.png"
        Image.new("RGB", (512, 512), "white").save(model_reference)
        Image.new("RGB", (512, 512), "gray").save(geometry_reference)
        Image.new("RGB", (512, 512), "white").save(original)
        self.job.input_path = original
        self.job.model_reference_path = model_reference
        self.job.geometry_reference_path = geometry_reference
        sheet = self.job.directory / "multiview-sheet.png"

        def edit(source, _prompt, destination, *, background=None):
            self.assertEqual(
                source,
                self.job.directory / SIDECAR.PORTRAIT_GEOMETRY_PROVIDER_FILENAME,
            )
            self.assertEqual(background, "transparent")
            Image.new("RGBA", (512, 512), (255, 255, 255, 0)).save(destination)
            return destination

        with mock.patch.object(SIDECAR, "edit_image", side_effect=edit) as image_edit:
            SIDECAR._create_portrait_multiview_sheet(self.job, sheet)

        image_edit.assert_called_once()
        self.assertTrue(sheet.is_file())

    def test_prepaid_multiview_failure_returns_to_confirmation_without_provider_call(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.face_limit = 1000000
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        reference = self.job.directory / "model-reference.png"
        Image.new("RGB", (512, 512), "white").save(reference)
        self.job.model_reference_path = reference
        gateway = self._provider_gateway()

        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(
                SIDECAR,
                "_ensure_portrait_multiview",
                side_effect=SIDECAR.PortraitMultiviewPreparationError("identity check failed"),
            ),
        ):
            SIDECAR._generate_job(self.job, "", False, self._paid_authorization())

        self.assertEqual(self.job.state, "awaiting_confirmation")
        self.assertEqual(self.job.phase, "multiview_retry")
        self.assertFalse(self.job.image_metrics["multiview_retry"]["paid_task_created"])
        self.assertEqual(self.job.attempts, [])
        gateway.start_or_reuse_model_task.assert_not_called()

    def test_unexpected_prepaid_multiview_error_remains_directly_retryable(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.face_limit = 1000000
        self.job.palette_roles = {
            "primary": self.palette[0], "structure": self.palette[1],
            "light": self.palette[2], "accent": self.palette[3],
        }
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        reference = self.job.directory / "model-reference.png"
        Image.new("RGB", (512, 512), "white").save(reference)
        self.job.model_reference_path = reference
        gateway = self._provider_gateway()

        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_ensure_portrait_multiview", side_effect=TypeError("manifest settings")),
        ):
            SIDECAR._generate_job(self.job, "", False, self._paid_authorization())

        self.assertEqual(self.job.state, "awaiting_confirmation")
        self.assertEqual(self.job.phase, "multiview_retry")
        self.assertEqual(self.job.attempts, [])
        self.assertFalse(self.job.image_metrics["multiview_retry"]["paid_task_created"])
        gateway.start_or_reuse_model_task.assert_not_called()

    def test_unreviewed_existing_multiview_sheet_can_be_rechecked_without_image_call(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        reference = self.job.directory / "model-reference.png"
        Image.new("RGB", (512, 512), "white").save(reference)
        self.job.model_reference_path = reference
        sheet = self.job.directory / "multiview" / "multiview-sheet.png"
        sheet.parent.mkdir(parents=True)
        Image.new("RGB", (1024, 1024), "white").save(sheet)

        self.assertTrue(SIDECAR._can_reuse_multiview_candidate(self.job, sheet))
        SIDECAR._mark_multiview_candidate_rejected(self.job, sheet, "identity_consistency_gate")
        self.assertTrue(SIDECAR._can_reuse_multiview_candidate(self.job, sheet))
        self.job.image_metrics["multiview_candidate_review"] = {
            "status": "review",
            "score": 70,
            "warnings": ["identity"],
            "checks": {},
        }
        self.assertFalse(SIDECAR._can_reuse_multiview_candidate(self.job, sheet))

    def test_quality_portrait_multiview_is_persisted_and_reused(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.face_limit = 1000000
        self.job.palette_roles = {
            "primary": self.palette[0], "structure": self.palette[1],
            "light": self.palette[2], "accent": self.palette[3],
        }
        self.job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}
        reference = self.job.directory / "model-reference.png"
        Image.new("RGB", (512, 512), "white").save(reference)
        self.job.model_reference_path = reference
        output = self.job.directory / "multiview"
        material_views = {}
        generation_views = {}
        for view in SIDECAR.MULTIVIEW_ORDER:
            material_views[view] = output / "views" / view / "model_reference.png"
            generation_views[view] = output / "views" / view / "generation_reference.png"
            material_views[view].parent.mkdir(parents=True, exist_ok=True)
            Image.new("RGBA", (512, 512), "white").save(material_views[view])
            Image.new("RGBA", (512, 512), "white").save(generation_views[view])

        def create_sheet(_source, _prompt, destination, *, background=None):
            self.assertEqual(background, "transparent")
            Image.new("RGBA", (512, 512), (255, 255, 255, 0)).save(destination)
            return destination

        manifest = output / "multiview-reference.json"

        def write_manifest(*_args, **_kwargs):
            manifest.write_text("{}", encoding="utf-8")
            return manifest

        with (
            mock.patch.object(SIDECAR, "edit_image", side_effect=create_sheet) as edit,
            mock.patch.object(SIDECAR, "_validate_image_file"),
            mock.patch.object(SIDECAR, "split_multiview_sheet", return_value=material_views),
            mock.patch.object(
                SIDECAR,
                "process_multiview_crops",
                return_value=(
                    material_views,
                    generation_views,
                    {
                        view: {
                            "portrait_skin_cleanup": {
                                "activated": 1,
                                "garment_color": self.palette[0],
                                "skin_color": self.palette[2],
                                "accent_color": self.palette[3],
                            },
                            "meaningful_subject_color_count": 4,
                            "palette_diversity_ok": True,
                            "printable_subject_area_ratio": 0.3,
                            "largest_subject_component_ratio": 1.0,
                            "largest_detached_subject_diagonal_ratio": 0.0,
                            "small_region_ratio_after": 0.0,
                            "severe_fragmented_palette_roles": [],
                            "secondary_subject_color_component_ratio": {},
                        }
                        for view in SIDECAR.MULTIVIEW_ORDER
                    },
                ),
            ),
            mock.patch.object(
                SIDECAR, "assess_model_input_image", return_value={"model_input_eligible": True}
            ),
            mock.patch.object(
                SIDECAR,
                "review_multiview_sheet",
                return_value={"status": "pass", "score": 97, "checks": {}},
            ),
            mock.patch.object(SIDECAR, "write_multiview_manifest", side_effect=write_manifest) as manifest_writer,
        ):
            first = SIDECAR._ensure_portrait_multiview(self.job)
            second = SIDECAR._ensure_portrait_multiview(self.job)

        self.assertEqual(first, generation_views)
        self.assertEqual(second, generation_views)
        edit.assert_called_once()
        self.assertIn("genuine alpha-transparent background", edit.call_args.args[1])
        self.assertIn("No flat backing board", edit.call_args.args[1])
        self.assertEqual(edit.call_args.kwargs["background"], "transparent")
        self.assertIsInstance(manifest_writer.call_args.kwargs["settings"], SIDECAR.PrintSettings)
        stored = self.job.image_metrics["multiview_reference"]
        self.assertEqual(stored["status"], "pass")
        self.assertEqual(stored["score"], 97)
        self.assertTrue(stored["normalization"]["views"]["front"]["source_locked"])

    def test_quality_portrait_multiview_rejection_does_not_retry_a_paid_edit(self):
        self.job.source = "image"
        self.job.style = "realistic"
        self.job.generation_profile = "quality"
        self.job.palette = ("#F4F4F1", "#1E1B1C", "#F2C9AE", "#4F6F62")
        reference = self.job.directory / "model-reference.png"
        Image.new("RGB", (512, 512), "white").save(reference)
        self.job.model_reference_path = reference
        sheet = self.job.directory / "multiview-sheet.png"

        def edit(_source, prompt, destination, *, background=None):
            del prompt, destination
            self.assertEqual(background, "transparent")
            raise SIDECAR.OpenAIPreprocessorError(
                "unsupported", code="image_rejected", retryable=False
            )

        with mock.patch.object(SIDECAR, "edit_image", side_effect=edit) as image_edit, \
                self.assertRaises(SIDECAR.OpenAIPreprocessorError):
            SIDECAR._create_portrait_multiview_sheet(self.job, sheet)

        self.assertFalse(sheet.exists())
        image_edit.assert_called_once()

    def test_generation_persists_provider_intent_before_paid_creation(self):
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="utf-8")
        gateway = self._provider_gateway()
        def start_task(*_args, **_kwargs):
            attempt = self.job.attempts[0]
            self.assertEqual(attempt["status"], "creating")
            self.assertEqual(attempt["provider_request_id"], f"{self.job.id}:model:1")
            self.assertNotIn("generation_task_id", attempt)
            return mock.Mock(provider="tripo", task_id="generation-id", reused=False)
        gateway.start_or_reuse_model_task.side_effect = start_task

        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_download_generation_artifact", return_value=artifact),
            mock.patch.object(SIDECAR, "_validate_obj_topology", return_value=(300000, 2, 0)),
        ):
            SIDECAR._generate_job(
                self.job,
                "printable object",
                False,
                self._paid_authorization(),
            )

        attempt = self.job.attempts[0]
        self.assertEqual(attempt["provider"], "tripo")
        self.assertEqual(attempt["provider_operation"], "model_generation")
        self.assertEqual(attempt["provider_request_id"], f"{self.job.id}:model:1")
        self.assertEqual(attempt["generation_task_id"], "generation-id")
        gateway.start_or_reuse_model_task.assert_called_once()

    def test_ambiguous_provider_creation_failure_is_recorded_without_retry(self):
        gateway = mock.Mock()
        gateway.start_or_reuse_model_task.side_effect = SIDECAR.ProviderGatewayError(
            "Could not connect to Tripo.",
            code="provider_unavailable",
            category="availability",
            provider="tripo",
            operation="model_generation",
            retryable=True,
            ambiguous=True,
        )

        with mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway):
            SIDECAR._generate_job(
                self.job,
                "printable object",
                False,
                self._paid_authorization(),
            )

        self.assertEqual(gateway.start_or_reuse_model_task.call_count, 1)
        self.assertEqual(self.job.state, "failed")
        attempt = self.job.attempts[0]
        self.assertEqual(attempt["provider_error_code"], "provider_unavailable")
        self.assertEqual(attempt["provider_error_category"], "availability")
        self.assertTrue(attempt["provider_error_retryable"])
        self.assertTrue(attempt["provider_error_ambiguous"])

    def test_shutdown_poll_interruption_keeps_paid_attempt_resumable(self):
        gateway = self._provider_gateway("existing-generation")
        gateway.wait_for_task.side_effect = SIDECAR.ProviderGatewayError(
            "The operation was cancelled.",
            code="provider_cancelled",
            category="cancellation",
            provider="tripo",
            operation="model_generation",
        )

        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_SHUT_DOWN", True),
        ):
            SIDECAR._generate_job(
                self.job,
                "printable object",
                False,
                self._paid_authorization(),
            )

        self.assertEqual(self.job.state, "queued")
        self.assertEqual(self.job.phase, "resuming")
        attempt = self.job.attempts[0]
        self.assertEqual(attempt["status"], "running")
        self.assertEqual(attempt["error"], "")
        self.assertEqual(attempt["generation_task_id"], "existing-generation")

    def test_resumed_generation_reuses_remote_ids_without_paid_creation(self):
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="utf-8")
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
            "status": "running",
            "error": "old recoverable download failure",
        }]
        gateway = self._provider_gateway("existing-generation")
        gateway.start_or_reuse_model_task.return_value = mock.Mock(
            provider="tripo", task_id="existing-generation", reused=True
        )
        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_download_generation_artifact", return_value=artifact) as download,
            mock.patch.object(SIDECAR, "_validate_obj_topology", return_value=(300000, 2, 0)),
        ):
            SIDECAR._generate_job(self.job, "printable object", True)

        gateway.start_or_reuse_model_task.assert_called_once()
        self.assertIsNone(gateway.start_or_reuse_model_task.call_args.kwargs["authorization"])
        download.assert_called_once_with(self.job, "existing-generation", 1, True)
        self.assertEqual(self.job.state, "ready")
        self.assertEqual(self.job.attempts[0]["error"], "")

    def test_obj_conversion_failure_does_not_fall_back(self):
        error = SIDECAR.TripoError("OBJ conversion failed")
        gateway = self._provider_gateway()
        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_download_generation_artifact", side_effect=error) as download,
        ):
            SIDECAR._generate_job(self.job, "printable object", False, self._paid_authorization())

        download.assert_called_once_with(self.job, "generation-id", 1, False)
        self.assertEqual(self.job.state, "failed")
        self.assertEqual(self.job.message, "OBJ conversion failed")
        self.assertEqual(len(self.job.attempts), 1)
        self.assertEqual(self.job.attempts[0]["status"], "rejected")

    def test_quality_failure_does_not_create_a_second_paid_task(self):
        quality_error = SIDECAR.TripoError(
            "Tripo generated a non-watertight or non-manifold mesh. Regenerate before importing into OrcaSlicer."
        )
        gateway = self._provider_gateway("generation-1")
        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_download_generation_artifact", side_effect=quality_error) as download,
        ):
            SIDECAR._generate_job(self.job, "printable object", False, self._paid_authorization())

        self.assertEqual(gateway.start_or_reuse_model_task.call_count, 1)
        self.assertEqual(download.call_count, 1)
        self.assertEqual(self.job.state, "failed")
        self.assertEqual([attempt["status"] for attempt in self.job.attempts], ["rejected"])
        attempts = (self.job.directory / "attempts.json").read_text(encoding="utf-8")
        self.assertIn("generation-1", attempts)

    def test_quality_failure_exhausts_single_attempt_budget(self):
        quality_error = SIDECAR.TripoError("The generated OBJ exceeds the 1000000-triangle limit.")
        gateway = self._provider_gateway("generation-1")
        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_download_generation_artifact", side_effect=quality_error) as download,
        ):
            SIDECAR._generate_job(self.job, "printable object", False, self._paid_authorization())

        self.assertEqual(gateway.start_or_reuse_model_task.call_count, SIDECAR.MAX_GENERATION_ATTEMPTS)
        self.assertEqual(download.call_count, SIDECAR.MAX_GENERATION_ATTEMPTS)
        self.assertEqual(self.job.state, "failed")
        self.assertEqual(len(self.job.attempts), SIDECAR.MAX_GENERATION_ATTEMPTS)
        self.assertTrue(all(attempt["status"] == "rejected" for attempt in self.job.attempts))

    def test_obj_is_normalized_to_z_up_100mm_and_on_bed(self):
        source = self.job.directory / "normalize.obj"
        source.write_text(
            "v -0.2 -0.5 -0.1 1 0 0\n"
            "v 0.2 -0.5 0.1 0 1 0\n"
            "v 0 0.5 0 0 0 1\n"
            "f 1 2 3\n",
            encoding="ascii",
        )

        SIDECAR._normalize_obj_for_orca(source)

        vertices = [
            tuple(float(value) for value in line.split()[1:4])
            for line in source.read_text(encoding="ascii").splitlines()
            if line.startswith("v ")
        ]
        self.assertAlmostEqual(min(vertex[2] for vertex in vertices), 0.0)
        self.assertAlmostEqual(max(vertex[2] for vertex in vertices), 100.0)
        self.assertLessEqual(max(abs(vertex[0]) for vertex in vertices), 20.0)
        self.assertLessEqual(max(abs(vertex[1]) for vertex in vertices), 10.0)
        self.assertEqual(self.job.artifact_format, "")
        self.assertIsNone(self.job.artifact_path)

    def test_portrait_palette_biases_only_ambiguous_warm_neutrals_to_garment(self):
        palette = ("#F2EFE6", "#1F2937", "#D8A17C", "#356B52")
        roles = {
            "primary": "#F2EFE6", "structure": "#1F2937",
            "light": "#D8A17C", "accent": "#356B52",
        }
        _, palette_lab = SIDECAR._palette_data(palette)
        portrait_indices = SIDECAR._portrait_material_palette_indices(palette, roles, True)
        portrait_role_indices = SIDECAR._portrait_material_role_indices(palette, roles, True)
        self.assertEqual(portrait_indices, (0, 2))
        self.assertEqual(portrait_role_indices, {
            "primary": 0, "structure": 1, "light": 2, "accent": 3,
        })

        warm_white_shadow = (195, 175, 166)
        natural_skin = (205, 145, 105)
        deep_green_fold = (40, 53, 45)
        neutral_black = (28, 29, 28)
        self.assertEqual(
            SIDECAR._nearest_palette_index(warm_white_shadow, palette_lab, {}), 2
        )
        self.assertEqual(
            SIDECAR._semantic_palette_index(
                warm_white_shadow, palette_lab, {}, portrait_indices
            ),
            0,
        )
        self.assertEqual(
            SIDECAR._semantic_palette_index(natural_skin, palette_lab, {}, portrait_indices),
            2,
        )
        self.assertEqual(SIDECAR._nearest_palette_index(deep_green_fold, palette_lab, {}), 1)
        self.assertEqual(
            SIDECAR._semantic_palette_index(
                deep_green_fold, palette_lab, {}, portrait_indices, portrait_role_indices
            ),
            3,
        )
        self.assertEqual(
            SIDECAR._semantic_palette_index(
                neutral_black, palette_lab, {}, portrait_indices, portrait_role_indices
            ),
            1,
        )
        self.assertIsNone(SIDECAR._portrait_material_palette_indices(palette, roles, False))
        self.assertIsNone(SIDECAR._portrait_material_role_indices(palette, roles, False))

    def test_portrait_material_mapping_is_forwarded_for_detected_realistic_job(self):
        self.job.output_format = "obj"
        self.job.generation_profile = "quality"
        destination = self.job.directory / "artifact-raw.download"
        destination.write_text(
            "v 0 0 0 1 1 1\nv 1 0 0 1 1 1\nv 0 1 0 1 1 1\nf 1 2 3\n",
            encoding="ascii",
        )
        self.job.style = "realistic"
        self.job.palette_roles = {
            "primary": self.palette[0], "structure": self.palette[1],
            "light": self.palette[2], "accent": self.palette[3],
        }
        self.job.image_metrics = {"portrait_skin_cleanup": {"activated": 1}}
        gateway = self._provider_gateway()
        gateway.download_artifact.side_effect = lambda result, path, limit: path.write_text(
            destination.read_text(encoding="ascii"), encoding="ascii"
        )
        with (
            mock.patch.object(SIDECAR, "_MODEL_PROVIDER_GATEWAY", gateway),
            mock.patch.object(SIDECAR, "_prepare_obj_artifact", return_value=Path("model.obj")) as prepare,
        ):
            SIDECAR._download_conversion(self.job, "generation", "obj")
        self.assertEqual(prepare.call_args.args[3], self.job.palette_roles)
        self.assertTrue(prepare.call_args.args[4])
        self.assertTrue(prepare.call_args.kwargs["build_aligned_portrait_reference"])

    def test_portrait_pipeline_repairs_skin_before_cleanup_and_normalizes_reference_face_after(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            "v 0 0 0 1 1 1\nv 1 0 0 1 1 1\nv 0 1 0 1 1 1\nf 1 2 3\n",
            encoding="ascii",
        )
        front = self.job.directory / "front-reference.png"
        front.write_bytes(b"reference")
        roles = {
            "primary": self.palette[0], "structure": self.palette[1],
            "light": self.palette[2], "accent": self.palette[3],
        }
        events = []

        def quantize(source, destination, *_args):
            destination.write_bytes(source.read_bytes())

        def project(*_args, **kwargs):
            events.append(("project", kwargs))
            return {"status": "not_needed"}

        with (
            mock.patch.object(SIDECAR, "_validate_obj_vertex_colors"),
            mock.patch.object(SIDECAR, "_quantize_vertex_color_obj", side_effect=quantize),
            mock.patch.object(SIDECAR, "_normalize_obj_for_orca"),
            mock.patch.object(SIDECAR, "project_front_portrait_materials", side_effect=project),
            mock.patch.object(
                SIDECAR, "_stabilize_portrait_obj_materials",
                side_effect=lambda *_args: events.append(("material", {})),
            ),
            mock.patch.object(SIDECAR, "_remove_small_detached_obj_components", return_value={}),
            mock.patch.object(SIDECAR, "_repair_small_obj_topology_defects"),
            mock.patch.object(
                SIDECAR,
                "_capture_portrait_front_face_details",
                return_value=({7: (1, 2, 3)}, {"status": "captured", "vertex_count": 3}),
            ) as capture_face,
            mock.patch.object(SIDECAR, "_restore_portrait_front_face_details") as restore_face,
            mock.patch.object(
                SIDECAR, "_consolidate_tiny_obj_color_components",
                side_effect=lambda _path, report: events.append(("consolidate", {"report": report.name})),
            ),
            mock.patch.object(SIDECAR, "_regularize_obj_color_boundaries"),
            mock.patch.object(
                SIDECAR, "_stabilize_portrait_obj_garment_regions",
                side_effect=lambda *_args: events.append(("garment", {})),
            ),
            mock.patch.object(SIDECAR, "_validate_obj_palette"),
            mock.patch.object(SIDECAR, "_write_obj_vertex_color_metrics"),
            mock.patch.object(SIDECAR, "_validate_artifact"),
            mock.patch.object(SIDECAR, "analyze_printable_obj", return_value={"status": "pass"}),
            mock.patch.object(SIDECAR, "write_model_quality_report"),
        ):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette, roles, True, front)

        project_events = [(index, item[1]) for index, item in enumerate(events) if item[0] == "project"]
        self.assertEqual(len(project_events), 4)
        self.assertLess(project_events[0][0], next(index for index, item in enumerate(events) if item[0] == "material"))
        self.assertGreater(project_events[1][0], next(index for index, item in enumerate(events) if item[0] == "garment"))
        self.assertEqual(project_events[0][1], {})
        self.assertEqual(project_events[1][1], {"repair_skin": False, "restore_accent": True})
        self.assertEqual(project_events[2][1], {"repair_skin": False, "restore_accent": True})
        self.assertEqual(
            project_events[3][1],
            {"repair_skin": False, "normalize_face_details": True},
        )
        self.assertGreater(
            project_events[3][0],
            project_events[2][0],
        )
        self.assertEqual(sum(item[0] == "garment" for item in events), 2)
        capture_face.assert_not_called()
        restore_face.assert_not_called()

    def test_four_view_portrait_finishes_without_a_late_planar_face_repaint(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            "v 0 0 0 1 1 1\nv 1 0 0 1 1 1\nv 0 1 0 1 1 1\nf 1 2 3\n",
            encoding="ascii",
        )
        roles = {
            "primary": self.palette[0], "structure": self.palette[1],
            "light": self.palette[2], "accent": self.palette[3],
        }
        semantic_views = {}
        for view in ("front", "right", "back", "left"):
            directory = self.job.directory / "semantic" / view
            directory.mkdir(parents=True)
            (directory / "aligned_reference.png").write_bytes(b"reference")
            (directory / "mask_subject.png").write_bytes(b"mask")
            semantic_views[view] = directory
        events = []

        def quantize(source, destination, *_args):
            destination.write_bytes(source.read_bytes())

        def render(_source, destination, *_args, **_kwargs):
            views = destination / "model-views"
            masks = destination / "model-masks"
            views.mkdir(parents=True, exist_ok=True)
            masks.mkdir(parents=True, exist_ok=True)
            (views / "front.png").write_bytes(b"front")
            return {"status": "rendered", "views": []}

        with (
            mock.patch.object(SIDECAR, "_validate_obj_vertex_colors"),
            mock.patch.object(SIDECAR, "_quantize_vertex_color_obj", side_effect=quantize),
            mock.patch.object(SIDECAR, "_normalize_obj_for_orca"),
            mock.patch.object(SIDECAR, "render_model_views", side_effect=render),
            mock.patch.object(
                SIDECAR, "_review_portrait_rear_plate_masks", return_value={"status": "pass"},
            ),
            mock.patch.object(SIDECAR, "_write_mesh_repair_report"),
            mock.patch.object(
                SIDECAR, "_prepare_portrait_geometry_material_views",
                return_value=(semantic_views, {"status": "prepared"}),
            ),
            mock.patch.object(
                SIDECAR, "project_front_portrait_materials",
                side_effect=lambda *_args, **kwargs: events.append(("front", kwargs)),
            ),
            mock.patch.object(
                SIDECAR, "project_geometry_aligned_portrait_materials",
                side_effect=lambda *_args, **_kwargs: events.append(("geometry", {})),
            ),
            mock.patch.object(SIDECAR, "_stabilize_portrait_obj_materials"),
            mock.patch.object(SIDECAR, "_remove_small_detached_obj_components", return_value={}),
            mock.patch.object(SIDECAR, "_repair_small_obj_topology_defects"),
            mock.patch.object(SIDECAR, "_consolidate_tiny_obj_color_components"),
            mock.patch.object(SIDECAR, "_regularize_obj_color_boundaries"),
            mock.patch.object(SIDECAR, "_stabilize_portrait_obj_garment_regions"),
            mock.patch.object(SIDECAR, "_validate_obj_palette"),
            mock.patch.object(SIDECAR, "_write_obj_vertex_color_metrics"),
            mock.patch.object(SIDECAR, "_validate_artifact"),
            mock.patch.object(SIDECAR, "analyze_printable_obj", return_value={"status": "pass"}),
            mock.patch.object(SIDECAR, "write_model_quality_report"),
        ):
            SIDECAR._prepare_obj_artifact(
                raw,
                self.job.directory,
                self.palette,
                roles,
                True,
                build_aligned_portrait_reference=True,
            )

        self.assertEqual(sum(name == "front" for name, _ in events), 4)
        self.assertEqual(sum(name == "geometry" for name, _ in events), 1)
        geometry_index = next(index for index, item in enumerate(events) if item[0] == "geometry")
        self.assertTrue(all(index < geometry_index for index, item in enumerate(events) if item[0] == "front"))

    def test_portrait_face_detail_restore_uses_original_visible_vertices_only(self):
        path = self.job.directory / "portrait-face.obj"
        roles = {
            "primary": "#F6F5F1",
            "structure": "#1E1A1C",
            "light": "#EBC6AA",
            "accent": "#4F6B5A",
        }

        def vertex(x, y, z, color):
            rgb = [int(color[index:index + 2], 16) / 255.0 for index in (1, 3, 5)]
            return f"v {x} {y} {z} " + " ".join(f"{value:.6f}" for value in rgb)

        original = [
            vertex(0, -10, 0, roles["light"]),
            vertex(0, 10, 100, roles["light"]),
            vertex(5, -2, 85, roles["structure"]),
            vertex(0, -2, 85, roles["structure"]),
            vertex(5, 1, 75, roles["primary"]),
            vertex(5, 2, 85, roles["accent"]),
            vertex(5, 0, 70, roles["structure"]),
        ]
        path.write_text("\n".join(original) + "\n", encoding="ascii")
        captured, capture_report = SIDECAR._capture_portrait_front_face_details(path, roles)

        self.assertIn(2, captured)
        self.assertNotIn(3, captured)
        self.assertIn(4, captured)
        self.assertNotIn(5, captured)
        self.assertNotIn(6, captured)

        path.write_text(
            "\n".join(vertex(*map(float, line.split()[1:4]), roles["light"]) for line in original) + "\n",
            encoding="ascii",
        )
        report = SIDECAR._restore_portrait_front_face_details(
            path,
            self.job.directory / "face-detail-restoration.json",
            captured,
            capture_report,
        )
        colors = [
            tuple(round(float(value) * 255) for value in line.split()[4:7])
            for line in path.read_text(encoding="ascii").splitlines()
        ]
        self.assertEqual(colors[2], (30, 26, 28))
        self.assertEqual(colors[3], (235, 198, 170))
        self.assertEqual(colors[4], (246, 245, 241))
        self.assertEqual(report["recolored_vertices"], 2)

        path.write_text(
            "\n".join(vertex(*map(float, line.split()[1:4]), roles["light"]) for line in original) + "\n",
            encoding="ascii",
        )
        filtered_report = SIDECAR._restore_portrait_front_face_details(
            path,
            self.job.directory / "face-structure-restoration.json",
            captured,
            capture_report,
            allowed_targets={(30, 26, 28)},
        )
        filtered_colors = [
            tuple(round(float(value) * 255) for value in line.split()[4:7])
            for line in path.read_text(encoding="ascii").splitlines()
        ]
        self.assertEqual(filtered_colors[2], (30, 26, 28))
        self.assertEqual(filtered_colors[4], (235, 198, 170))
        self.assertEqual(filtered_report["recolored_vertices"], 1)
        self.assertEqual(filtered_report["allowed_targets"], ["#1E1A1C"])

    def test_portrait_bust_materials_lock_detected_base_only(self):
        path = self.job.directory / "portrait.obj"
        palette = ("#F2EFE6", "#1F2937", "#D8A17C", "#356B52")
        roles = {
            "primary": "#F2EFE6", "structure": "#1F2937",
            "light": "#D8A17C", "accent": "#356B52",
        }

        def vertex(x, z, color):
            red, green, blue = (int(color[index:index + 2], 16) / 255.0 for index in (1, 3, 5))
            return f"v {x} 0 {z} {red:.6f} {green:.6f} {blue:.6f}"

        lines = []
        lines.extend(vertex(-1 + index * 0.1, 0, roles["structure"]) for index in range(20))
        lines.append(vertex(0, 0, roles["accent"]))
        lines.extend(vertex((-1) ** index * 2, 40, roles["primary"]) for index in range(40))
        lines.extend(vertex(-2, 50, roles["light"]) for _ in range(10))
        lines.extend(vertex(-2, 50, roles["accent"]) for _ in range(10))
        lines.extend(vertex(-2, 50, roles["structure"]) for _ in range(10))
        lines.extend(vertex(2, 85, roles["light"]) for _ in range(3))
        lines.extend(vertex(-2, 90, roles["structure"]) for _ in range(3))
        lines.extend(vertex(2, 50, roles["accent"]) for _ in range(3))
        path.write_text("\n".join(lines) + "\n", encoding="ascii")

        report = SIDECAR._stabilize_portrait_obj_materials(
            path, self.job.directory / "portrait-material-cleanup.json", palette, roles, True
        )
        output = [line.split() for line in path.read_text(encoding="ascii").splitlines()]
        colors = [tuple(round(float(value) * 255) for value in fields[4:7]) for fields in output]
        primary = (242, 239, 230)
        structure = (31, 41, 55)
        skin = (216, 161, 124)
        accent = (53, 107, 82)
        self.assertEqual(report["status"], "stabilized")
        self.assertEqual(colors[20], structure)
        self.assertEqual(colors[21:61], [primary] * 40)
        self.assertEqual(colors[61:71], [skin] * 10)
        self.assertEqual(colors[71:81], [accent] * 10)
        self.assertEqual(colors[81:91], [structure] * 10)
        self.assertEqual(colors[91:94], [skin] * 3)
        self.assertEqual(colors[94:97], [structure] * 3)
        self.assertEqual(colors[97:100], [accent] * 3)

    def test_portrait_material_lock_requires_a_detected_dark_base(self):
        path = self.job.directory / "no-base.obj"
        path.write_text(
            "v -1 0 0 0.949020 0.937255 0.901961\n"
            "v 1 0 0 0.847059 0.631373 0.486275\n"
            "v -1 0 100 0.207843 0.419608 0.321569\n",
            encoding="ascii",
        )
        roles = {
            "primary": "#F2EFE6", "structure": "#1F2937",
            "light": "#D8A17C", "accent": "#356B52",
        }
        before = path.read_bytes()
        report = SIDECAR._stabilize_portrait_obj_materials(
            path,
            self.job.directory / "no-base-report.json",
            tuple(roles.values()),
            roles,
            True,
        )
        self.assertEqual(report["status"], "not_applicable")
        self.assertFalse(report["activated"])
        self.assertEqual(path.read_bytes(), before)

    def test_portrait_garment_cleanup_removes_sparse_noise_and_preserves_features(self):
        path = self.job.directory / "portrait-garment.obj"
        palette = ("#F2EFE6", "#1F2937", "#D8A17C", "#356B52")
        roles = {
            "primary": palette[0], "structure": palette[1],
            "light": palette[2], "accent": palette[3],
        }
        size = 31

        def rgb(hex_color):
            return " ".join(
                f"{int(hex_color[index:index + 2], 16) / 255.0:.6f}" for index in (1, 3, 5)
            )

        colors = [[roles["primary"] for _ in range(size)] for _ in range(size)]
        for row in range(3):
            for column in range(size):
                colors[row][column] = roles["structure"]
        for row in range(22, 30):
            for column in range(10, 21):
                colors[row][column] = roles["light"]
        for row in (24, 25):
            for column in (11, 12, 18, 19):
                colors[row][column] = roles["primary"]
        for row in (26, 27):
            for column in (14, 15):
                colors[row][column] = roles["primary"]
        for row in range(28, 31):
            for column in range(10, 21):
                colors[row][column] = roles["structure"]
        for row in range(10, 20):
            for column in range(13, 18):
                colors[row][column] = roles["accent"]
        # A narrow dark UV island fully enclosed by the green blouse models
        # the vertical black seam seen in the real Fangfei texture result.
        # It is garment shadow, not an independent black feature.
        for row in range(14, 17):
            colors[row][15] = roles["structure"]
        # A second small but coherent front-centre patch models a real garment
        # split by a UV seam. It must not be mistaken for random green noise.
        for row in (12, 13):
            for column in (10, 11):
                colors[row][column] = roles["accent"]
        for row in range(6, 10):
            for column in range(1, 17):
                colors[row][column] = roles["light"]
        for row in range(14, 18):
            for column in (*range(5, 9), *range(22, 26)):
                colors[row][column] = roles["light"]
        # A one-vertex-wide skin tendril models Tripo blending a real hand
        # into the adjacent sleeve. It shares the hand component, so generic
        # tiny-component cleanup cannot remove it.
        for column in range(9, 13):
            colors[16][column] = roles["light"]
        colors[26][13] = roles["structure"]
        colors[26][17] = roles["structure"]
        for column in range(14, 17):
            colors[24][column] = roles["primary"]

        noise = {
            (4, 12): roles["accent"],
            (27, 11): roles["light"],
            (3, 18): roles["structure"],
        }
        for (column, row), color in noise.items():
            colors[row][column] = color

        vertices = [
            f"v 1 {column} {row} {rgb(colors[row][column])}"
            for row in range(size)
            for column in range(size)
        ]
        vertices.append(f"v -1 0 {size - 1} {rgb(roles['primary'])}")
        faces = []
        for row in range(size - 1):
            for column in range(size - 1):
                left = row * size + column
                right = left + 1
                upper = left + size
                upper_right = upper + 1
                faces.extend(((left, upper, right), (right, upper, upper_right)))
        path.write_text(
            "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
            encoding="ascii",
        )

        report = SIDECAR._stabilize_portrait_obj_garment_regions(
            path, self.job.directory / "portrait-garment-cleanup.json", palette, roles, True
        )
        output = [
            tuple(round(float(value) * 255) for value in line.split()[4:7])
            for line in path.read_text(encoding="ascii").splitlines()
            if line.startswith("v ")
        ]
        expected = {
            role: tuple(int(color[index:index + 2], 16) for index in (1, 3, 5))
            for role, color in roles.items()
        }
        self.assertEqual(report["status"], "stabilized")
        self.assertGreaterEqual(report["recolored_vertices"], len(noise))
        for column, row in noise:
            self.assertEqual(output[row * size + column], expected["primary"])
        self.assertEqual(output[8 * size + 5], expected["primary"])
        self.assertEqual(output[15 * size + 6], expected["light"])
        self.assertEqual(output[16 * size + 12], expected["primary"])
        self.assertEqual(output[15 * size + 15], expected["accent"])
        self.assertGreaterEqual(report["enclosed_accent_shadow_components"], 1)
        self.assertGreaterEqual(report["enclosed_accent_shadow_recolored_vertices"], 3)
        self.assertEqual(output[12 * size + 10], expected["accent"])
        self.assertEqual(output[24 * size + 11], expected["primary"])
        self.assertEqual(output[25 * size + 11], expected["light"])
        self.assertEqual(output[27 * size + 14], expected["light"])
        self.assertEqual(output[26 * size + 13], expected["structure"])
        self.assertEqual(output[24 * size + 15], expected["primary"])

    def test_portrait_garment_cleanup_does_not_mistake_low_waist_skin_band_for_hand(self):
        path = self.job.directory / "portrait-low-skin-band.obj"
        palette = ("#F2EFE6", "#1F2937", "#D8A17C", "#356B52")
        roles = {
            "primary": palette[0], "structure": palette[1],
            "light": palette[2], "accent": palette[3],
        }
        size = 21

        def rgb(hex_color):
            return " ".join(
                f"{int(hex_color[index:index + 2], 16) / 255.0:.6f}" for index in (1, 3, 5)
            )

        colors = [[roles["primary"] for _ in range(size)] for _ in range(size)]
        for row in range(2):
            for column in range(size):
                colors[row][column] = roles["structure"]
        for row in range(15, 21):
            for column in range(6, 15):
                colors[row][column] = roles["light"]
        # This broad low patch is the exact failure mode seen on the portrait:
        # it is compact enough to look like a hand to component-only logic, but
        # sits at the waist just above the base and must remain jacket material.
        for row in range(4, 7):
            for column in range(5, 16):
                colors[row][column] = roles["light"]

        vertices = [
            f"v 1 {column} {row} {rgb(colors[row][column])}"
            for row in range(size)
            for column in range(size)
        ]
        vertices.append(f"v -1 0 {size - 1} {rgb(roles['primary'])}")
        faces = []
        for row in range(size - 1):
            for column in range(size - 1):
                left = row * size + column
                right = left + 1
                upper = left + size
                upper_right = upper + 1
                faces.extend(((left, upper, right), (right, upper, upper_right)))
        path.write_text(
            "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
            encoding="ascii",
        )

        report = SIDECAR._stabilize_portrait_obj_garment_regions(
            path, self.job.directory / "portrait-low-band-report.json", palette, roles, True
        )
        output = [
            tuple(round(float(value) * 255) for value in line.split()[4:7])
            for line in path.read_text(encoding="ascii").splitlines()
            if line.startswith("v ")
        ]
        expected_primary = tuple(int(palette[0][index:index + 2], 16) for index in (1, 3, 5))
        expected_skin = tuple(int(palette[2][index:index + 2], 16) for index in (1, 3, 5))

        self.assertEqual(output[5 * size + 10], expected_primary)
        self.assertEqual(output[18 * size + 10], expected_skin)
        self.assertEqual(report["protected_hand_components"], 0)
        self.assertEqual(report["hand_minimum_height_ratio"], SIDECAR.PORTRAIT_HAND_MIN_HEIGHT_RATIO)

    def test_zip_texture_is_baked_into_vertex_colors(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        self.assertEqual(artifact.name, "model-vertex-color.obj")
        self.assertTrue((self.job.directory / "artifact-raw.zip").is_file())
        self.assertTrue((self.job.directory / "package" / "model.obj").is_file())
        lines = artifact.read_text(encoding="ascii").splitlines()
        vertices = [line for line in lines if line.startswith("v ")]
        self.assertEqual(len(vertices), 4)
        output_colors = {tuple(round(float(value) * 255) for value in line.split()[4:7]) for line in vertices}
        self.assertLessEqual(output_colors, {(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)})
        forbidden = ("mtllib ", "usemtl ", "vt ", "vn ")
        self.assertFalse(any(line.lower().startswith(forbidden) for line in lines))
        metrics = json.loads((self.job.directory / "vertex-color-metrics.json").read_text(encoding="utf-8"))
        self.assertEqual(metrics["face_count"], 4)
        self.assertEqual(metrics["three_color_faces"], 0)
        cleanup = json.loads((self.job.directory / "vertex-color-cleanup.json").read_text(encoding="utf-8"))
        self.assertIn(cleanup["status"], {"not_needed", "consolidated"})
        boundary = json.loads((self.job.directory / "color-boundary-cleanup.json").read_text(encoding="utf-8"))
        self.assertIn(boundary["status"], {"not_needed", "regularized"})
        quality = json.loads((self.job.directory / SIDECAR.MODEL_QUALITY_FILENAME).read_text(encoding="utf-8"))
        self.assertEqual(quality["status"], "review")
        self.assertEqual(quality["metrics"]["face_count"], 4)
        self.assertTrue(quality["metrics"]["target_palette_metrics_available"])
        self.assertEqual(quality["metrics"]["target_palette_color_count"], 4)
        self.assertEqual(quality["metrics"]["meaningful_target_palette_color_count"], 2)
        self.assertFalse(quality["metrics"]["target_palette_diversity_ok"])
        self.assertAlmostEqual(quality["metrics"]["target_palette_surface_coverage_ratio"], 1.0)
        self.assertEqual(len(quality["evidence"]["target_palette_surface_usage"]), 4)
        self.assertIn("too_few_meaningful_target_palette_colors", quality["warnings"])
        SIDECAR._validate_obj_vertex_colors(artifact)
        SIDECAR._validate_obj_topology(artifact)

    def test_model_quality_report_write_failure_keeps_readable_artifact(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)
        failure = SIDECAR.ModelQualityError("report_write_failed", "quality report unavailable")

        with mock.patch.object(SIDECAR, "write_model_quality_report", side_effect=failure):
            artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)
        SIDECAR._validate_artifact(artifact, "obj")

    def test_rejected_quality_report_is_advice_and_keeps_readable_artifact(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)
        report = {"status": "reject", "errors": ["floating_components", "thin_structural_parts"]}
        with mock.patch.object(SIDECAR, "analyze_printable_obj", return_value=report):
            artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, ())
        self.assertTrue(artifact.is_file())
        self.assertEqual(json.loads((self.job.directory / SIDECAR.MODEL_QUALITY_FILENAME).read_text()), report)
        SIDECAR._validate_artifact(artifact, "obj")

    def test_delivery_accepts_open_and_degenerate_mesh_but_rejects_missing_indices(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            "v 0 0 0 0.7 0.2 0.1\nv 1 0 0 0.2 0.6 0.4\nv 0 1 1 0.3 0.1 0.8\n"
            "f 1 2 3\nf 1 1 2\n", encoding="ascii",
        )
        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, ())
        SIDECAR._validate_artifact(artifact, "obj")
        quality = json.loads((self.job.directory / SIDECAR.MODEL_QUALITY_FILENAME).read_text())
        self.assertEqual(quality["status"], "reject")
        artifact.write_text("v 0 0 0 1 0 0\nf 1 2 3\n", encoding="ascii")
        with self.assertRaisesRegex(SIDECAR.TripoError, "missing vertex"):
            SIDECAR._validate_artifact(artifact, "obj")

    def test_zip_texture_preserves_natural_vertex_colors_without_printable_palette(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, ())

        lines = artifact.read_text(encoding="ascii").splitlines()
        output_colors = {
            tuple(round(float(value) * 255) for value in line.split()[4:7])
            for line in lines
            if line.startswith("v ")
        }
        self.assertGreaterEqual(len(output_colors), 2)
        self.assertTrue(any(channel not in range(0, 256, 51) for color in output_colors for channel in color))
        self.assertFalse((self.job.directory / "vertex-color-cleanup.json").exists())
        self.assertFalse((self.job.directory / "color-boundary-cleanup.json").exists())
        SIDECAR._validate_obj_vertex_colors(artifact)
        SIDECAR._validate_obj_topology(artifact)

    def test_texture_bake_can_emit_aligned_natural_and_printable_models_in_one_pass(self):
        archive = self.job.directory / "dual-output.zip"
        package = self.job.directory / "dual-output-package"
        self._write_package(archive)
        obj = SIDECAR._extract_obj_package(archive, package)
        printable = self.job.directory / "dual-printable.obj"
        natural = self.job.directory / "dual-natural.obj"
        printable_palette = ("#FFFFFF", "#000000", "#C0C0C0", "#808080")

        SIDECAR._bake_obj_texture_to_vertex_colors(
            obj,
            package,
            printable,
            printable_palette,
            natural_destination=natural,
        )

        def colors(path):
            return {
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in path.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            }

        palette_rgb = {
            tuple(int(color[index:index + 2], 16) for index in (1, 3, 5))
            for color in printable_palette
        }
        self.assertTrue(printable.is_file())
        self.assertTrue(natural.is_file())
        self.assertTrue(colors(printable).issubset(palette_rgb))
        self.assertFalse(colors(natural).issubset(palette_rgb))

    def test_object_group_and_material_boundaries_are_preserved_as_groups(self):
        raw = self.job.directory / "artifact-raw.download"
        obj = (
            "mtllib model.mtl\n"
            "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
            "vt 0 1\nvt 1 1\nvt 0 0\n"
            "o body shell\ng head part\nusemtl painted\n"
            "f 1/1 3/3 2/2\nf 1/1 2/2 4/3\nf 1/1 4/3 3/3\nf 2/2 3/3 4/3\n"
        )
        self._write_package(raw, obj=obj)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, ())

        lines = artifact.read_text(encoding="ascii").splitlines()
        self.assertIn("o body_shell", lines)
        self.assertIn("g head_part", lines)
        self.assertIn("g material_painted", lines)

    def test_uv_seams_do_not_duplicate_geometry_vertices(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        vertices = [line for line in artifact.read_text(encoding="ascii").splitlines() if line.startswith("v ")]
        self.assertEqual(len(vertices), 4)
        self.assertEqual(SIDECAR._validate_obj_topology(artifact), (4, 1, 0))

    def test_disconnected_parts_are_preserved_before_import(self):
        raw = self.job.directory / "artifact-raw.download"
        vertices = "\n".join(
            f"v {x} {y} {z}" for x, y, z in (
                (0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1),
                (3, 0, 0), (4, 0, 0), (3, 1, 0), (3, 0, 1),
            )
        )
        faces = "\n".join((
            "f 1/1 3/3 2/2", "f 1/1 2/2 4/3", "f 1/1 4/3 3/3", "f 2/2 3/3 4/3",
            "f 5/1 7/3 6/2", "f 5/1 6/2 8/3", "f 5/1 8/3 7/3", "f 6/2 7/3 8/3",
        ))
        obj = f"mtllib model.mtl\n{vertices}\nvt 0 1\nvt 1 1\nvt 0 0\nusemtl painted\n{faces}\n"
        self._write_package(raw, obj=obj)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)
        self.assertEqual(SIDECAR._validate_obj_topology(artifact), (8, 2, 0))

    def test_tiny_detached_component_is_preserved_and_reported(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(self._vertex_color_torus(add_tiny_component=True), encoding="ascii")

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        self.assertEqual(SIDECAR._validate_obj_topology(artifact), (132, 2, 0))
        report = json.loads((self.job.directory / "mesh-repair.json").read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "preserved")
        self.assertEqual(report["original_components"], 2)
        self.assertEqual(report["removed_components"], 0)

    def test_bounded_detached_noise_is_removed_from_dense_model(self):
        source = self.job.directory / "dense-with-noise.obj"
        size = 151
        vertices = [f"v {x} {y} 0 1 0 0" for y in range(size) for x in range(size)]
        faces = []
        for y in range(size - 1):
            for x in range(size - 1):
                left = y * size + x
                right = left + 1
                upper = left + size
                upper_right = upper + 1
                faces.extend(((left, upper, right), (right, upper, upper_right)))
        noise_start = len(vertices)
        vertices.extend((
            "v 75 75 1 0 1 0",
            "v 75.05 75 1 0 1 0",
            "v 75 75.05 1 0 1 0",
            "v 75 75 1.05 0 1 0",
        ))
        faces.extend((
            (noise_start, noise_start + 2, noise_start + 1),
            (noise_start, noise_start + 1, noise_start + 3),
            (noise_start, noise_start + 3, noise_start + 2),
            (noise_start + 1, noise_start + 2, noise_start + 3),
        ))
        source.write_text(
            "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
            encoding="ascii",
        )

        report = SIDECAR._remove_small_detached_obj_components(
            source, self.job.directory / "dense-mesh-repair.json"
        )

        self.assertEqual(report["status"], "removed")
        self.assertEqual(report["removed_components"], 1)
        self.assertEqual(report["removed_vertices"], 4)
        self.assertEqual(report["removed_faces"], 4)
        self.assertEqual(report["kept_faces"], (size - 1) * (size - 1) * 2)
        self.assertNotIn("v 75 75 1 0 1 0", source.read_text(encoding="ascii"))

    def test_unreferenced_vertex_is_removed_without_touching_main_component(self):
        source = self.job.directory / "unreferenced-vertex.obj"
        source.write_text(
            "v 0 0 0 1 0 0\n"
            "v 1 0 0 1 0 0\n"
            "v 0 1 0 1 0 0\n"
            "v 0 0 1 1 0 0\n"
            "v 99 99 99 0 1 0\n"
            "f 1 3 2\n"
            "f 1 2 4\n"
            "f 1 4 3\n"
            "f 2 3 4\n",
            encoding="ascii",
        )

        report = SIDECAR._remove_small_detached_obj_components(
            source, self.job.directory / "unreferenced-mesh-repair.json"
        )

        self.assertEqual(report["status"], "removed")
        self.assertEqual(report["removed_components"], 0)
        self.assertEqual(report["removed_vertices"], 1)
        self.assertEqual(SIDECAR._validate_obj_topology(source), (4, 1, 0))

    def test_tiny_vertex_color_component_is_merged_into_strongest_neighbor(self):
        source = self.job.directory / "tiny-color-island.obj"
        size = 101
        center = (size // 2) * size + size // 2
        vertices = [
            f"v {x} {y} 0 " + ("0 0 1" if y * size + x == center else "1 0 0")
            for y in range(size)
            for x in range(size)
        ]
        faces = []
        for y in range(size - 1):
            for x in range(size - 1):
                left = y * size + x
                right = left + 1
                upper = left + size
                upper_right = upper + 1
                faces.extend(((left, upper, right), (right, upper, upper_right)))
        source.write_text(
            "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
            encoding="ascii",
        )

        report = SIDECAR._consolidate_tiny_obj_color_components(
            source, self.job.directory / "tiny-color-cleanup.json"
        )

        self.assertEqual(report["status"], "consolidated")
        self.assertEqual(report["merged_components"], 1)
        self.assertEqual(report["recolored_vertices"], 1)
        self.assertEqual(report["final_vertex_color_usage"], {"#FF0000": size * size})
        SIDECAR._validate_obj_palette(source, ("#FF0000", "#0000FF"))

    def test_isolated_color_boundary_spike_is_regularized(self):
        source = self.job.directory / "boundary-spike.obj"
        size = 21
        center = size // 2
        source.write_text(
            self._vertex_color_grid(
                size,
                lambda x, y: "0 0 1" if (x, y) == (center, center) else "1 0 0",
            ),
            encoding="ascii",
        )

        report = SIDECAR._regularize_obj_color_boundaries(
            source, self.job.directory / "boundary-spike-cleanup.json"
        )

        self.assertEqual(report["status"], "regularized")
        self.assertEqual(report["recolored_vertices"], 1)
        self.assertGreater(report["before"]["mixed_face_count"], 0)
        self.assertEqual(report["after"]["mixed_face_count"], 0)
        self.assertLess(
            report["after"]["mixed_face_surface_area_mm2"],
            report["before"]["mixed_face_surface_area_mm2"],
        )
        self.assertNotIn(" 0.000000 0.000000 1.000000", source.read_text(encoding="ascii"))
        self.assertEqual(SIDECAR._obj_vertex_color_metrics(source)["three_color_faces"], 0)

    def test_coherent_color_boundary_is_preserved(self):
        source = self.job.directory / "coherent-boundary.obj"
        size = 21
        source.write_text(
            self._vertex_color_grid(size, lambda x, _y: "1 0 0" if x < size // 2 else "0 0 1"),
            encoding="ascii",
        )
        original = source.read_bytes()

        report = SIDECAR._regularize_obj_color_boundaries(
            source, self.job.directory / "coherent-boundary-cleanup.json"
        )

        self.assertEqual(report["status"], "not_needed")
        self.assertEqual(report["recolored_vertices"], 0)
        self.assertEqual(report["before"]["mixed_face_count"], report["after"]["mixed_face_count"])
        self.assertEqual(source.read_bytes(), original)

    def test_meaningful_palette_color_is_protected_from_boundary_cleanup(self):
        source = self.job.directory / "meaningful-spike.obj"
        size = 5
        center = size // 2
        source.write_text(
            self._vertex_color_grid(
                size,
                lambda x, y: "0 0 1" if (x, y) == (center, center) else "1 0 0",
            ),
            encoding="ascii",
        )

        with (
            mock.patch.object(SIDECAR, "MAX_COLOR_BOUNDARY_SURFACE_AREA_RATIO", 1.0),
            mock.patch.object(SIDECAR, "MAX_COLOR_BOUNDARY_SOURCE_AREA_RATIO", 1.0),
        ):
            report = SIDECAR._regularize_obj_color_boundaries(
                source, self.job.directory / "meaningful-spike-cleanup.json"
            )

        self.assertEqual(report["status"], "not_needed")
        self.assertEqual(report["recolored_vertices"], 0)
        self.assertGreater(report["protected_meaningful_candidates"], 0)
        self.assertIn("v 2 2 0 0 0 1", source.read_text(encoding="ascii"))

    def test_repairable_open_edges_are_deferred_to_orca(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(self._vertex_color_torus(omit_faces=(0,)), encoding="ascii")

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        self.assertEqual(SIDECAR._validate_obj_topology(artifact, allow_repairable=True), (127, 1, 3))
        with self.assertRaisesRegex(SIDECAR.TripoError, "watertight"):
            SIDECAR._validate_obj_topology(artifact)

    def test_inconsistent_face_winding_is_normalized_before_orca_import(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            "v 0 0 0 1 0 0\n"
            "v 1 0 0 0 1 0\n"
            "v 0 1 0 0 0 1\n"
            "v 0 0 1 1 1 0\n"
            "f 1 2 3\n"
            "f 1 2 4\n"
            "f 1 4 3\n"
            "f 2 3 4\n",
            encoding="ascii",
        )

        with self.assertRaisesRegex(SIDECAR.TripoError, "inconsistently wound"):
            SIDECAR._validate_obj_topology(raw)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        self.assertEqual(SIDECAR._validate_obj_topology(artifact), (4, 1, 0))
        report = json.loads((self.job.directory / "mesh-repair.json").read_text(encoding="utf-8"))
        self.assertEqual(report["topology_status"], "repaired")
        self.assertEqual(report["original_inconsistent_winding_edges"], 3)
        self.assertEqual(report["flipped_winding_faces"], 1)
        self.assertEqual(report["remaining_inconsistent_winding_edges"], 0)
        self.assertEqual(report["remaining_invalid_edges"], 0)

    def test_small_non_manifold_defect_is_repaired_with_palette_colors(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            self._vertex_color_torus(duplicate_faces=(0,), major_segments=128, minor_segments=64),
            encoding="ascii",
        )

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        face_count, components, invalid_edges = SIDECAR._validate_obj_topology(artifact)
        self.assertLessEqual(face_count, SIDECAR.MAX_MODEL_FACES)
        self.assertEqual((components, invalid_edges), (1, 0))
        report = json.loads((self.job.directory / "mesh-repair.json").read_text(encoding="utf-8"))
        self.assertEqual(report["topology_status"], "repaired")
        self.assertGreater(report["removed_non_manifold_faces"], 0)
        self.assertGreater(report["filled_boundary_loops"], 0)
        self.assertEqual(report["remaining_invalid_edges"], 0)
        SIDECAR._validate_obj_palette(artifact, self.palette)

    def test_separated_small_non_manifold_defects_are_repaired_independently(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            self._vertex_color_torus(duplicate_faces=(0, 8192), major_segments=128, minor_segments=64),
            encoding="ascii",
        )

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        face_count, components, invalid_edges = SIDECAR._validate_obj_topology(artifact)
        self.assertLessEqual(face_count, SIDECAR.MAX_MODEL_FACES)
        self.assertEqual((components, invalid_edges), (1, 0))
        report = json.loads((self.job.directory / "mesh-repair.json").read_text(encoding="utf-8"))
        self.assertEqual(report["topology_status"], "repaired")
        self.assertEqual(report["non_manifold_regions"], 2)
        self.assertLess(
            report["max_non_manifold_region_diagonal_ratio"],
            SIDECAR.MAX_LOCAL_REPAIR_DIAGONAL_RATIO,
        )
        self.assertEqual(report["remaining_invalid_edges"], 0)
        SIDECAR._validate_obj_palette(artifact, self.palette)

    def test_excessive_open_edges_are_reported_without_blocking_artifact(self):
        raw = self.job.directory / "artifact-raw.download"
        vertices = ["v 0 0 0 1 0 0"]
        faces = []
        for index in range(30):
            angle = 2.0 * math.pi * index / 30
            vertices.extend(
                (
                    f"v {math.cos(angle):.9g} {math.sin(angle):.9g} 0 1 0 0",
                    f"v {math.cos(angle):.9g} {math.sin(angle):.9g} 1 1 0 0",
                )
            )
            faces.append(f"f 1 {2 * index + 2} {2 * index + 3}")
        raw.write_text("\n".join(vertices + faces) + "\n", encoding="ascii")

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)
        quality = json.loads((self.job.directory / SIDECAR.MODEL_QUALITY_FILENAME).read_text())
        self.assertEqual(quality["status"], "reject")
        SIDECAR._validate_artifact(artifact, "obj")

    def test_open_mesh_is_reported_without_blocking_artifact(self):
        raw = self.job.directory / "artifact-raw.download"
        obj = (
            "mtllib model.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "vt 0 1\nvt 1 1\nvt 0 0\nusemtl painted\nf 1/1 2/2 3/3\n"
        )
        self._write_package(raw, obj=obj)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)
        quality = json.loads((self.job.directory / SIDECAR.MODEL_QUALITY_FILENAME).read_text())
        self.assertEqual(quality["status"], "reject")
        SIDECAR._validate_artifact(artifact, "obj")

    def test_plain_vertex_color_obj_is_preserved(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nv 0 0 1 1 1 0\n"
            "f 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n",
            encoding="ascii",
        )

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        self.assertTrue((self.job.directory / "artifact-raw.obj").is_file())
        SIDECAR._validate_obj_vertex_colors(artifact)

    def test_plain_natural_vertex_colors_are_not_quantized(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            "o colored_part\n"
            "v 0 0 0 0.123 0.456 0.789\nv 1 0 0 0.2 0.3 0.4\n"
            "v 0 1 0 0.5 0.6 0.7\nv 0 0 1 0.8 0.9 0.1\n"
            "f 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n",
            encoding="ascii",
        )

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, ())

        text = artifact.read_text(encoding="ascii")
        self.assertIn("0.123000 0.456000 0.789000", text)
        self.assertIn("o colored_part", text)

    def test_zip_path_traversal_is_rejected(self):
        raw = self.job.directory / "artifact-raw.download"
        with zipfile.ZipFile(raw, "w") as bundle:
            bundle.writestr("../escape.obj", "v 0 0 0\n")
        with self.assertRaisesRegex(SIDECAR.TripoError, "unsafe path"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)
        self.assertFalse((self.job.directory.parent / "escape.obj").exists())

    def test_zip_symbolic_link_is_rejected(self):
        raw = self.job.directory / "artifact-raw.download"
        link = zipfile.ZipInfo("model.obj")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(raw, "w") as bundle:
            bundle.writestr(link, "target.obj")
        with self.assertRaisesRegex(SIDECAR.TripoError, "symbolic link"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_zip_file_count_limit_is_enforced(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)
        with mock.patch.object(SIDECAR, "MAX_ARCHIVE_FILES", 2):
            with self.assertRaisesRegex(SIDECAR.TripoError, "number of files"):
                SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_zip_unpacked_size_limit_is_enforced(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)
        with mock.patch.object(SIDECAR, "MAX_UNPACKED_BYTES", 4):
            with self.assertRaisesRegex(SIDECAR.TripoError, "too large"):
                SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_missing_obj_is_rejected(self):
        raw = self.job.directory / "artifact-raw.download"
        with zipfile.ZipFile(raw, "w") as bundle:
            bundle.writestr("model.mtl", "newmtl painted\n")
        with self.assertRaisesRegex(SIDECAR.TripoError, "exactly one OBJ"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_missing_material_is_rejected(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw, obj="v 0 0 0\nvt 0 0\nmtllib missing.mtl\nf 1/1 1/1 1/1\n")
        with self.assertRaisesRegex(SIDECAR.TripoError, "missing its material"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_missing_base_color_texture_is_rejected(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw, texture=False)
        with self.assertRaisesRegex(SIDECAR.TripoError, "base-color texture"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_cleanup_keeps_generated_files(self):
        artifact = self.job.directory / "kept.obj"
        artifact.write_text("kept", encoding="ascii")
        SIDECAR._cleanup_job(self.job)
        self.assertTrue(artifact.is_file())

    def test_deleted_job_keeps_generated_files(self):
        artifact = self.job.directory / "kept-after-delete.obj"
        artifact.write_text("kept", encoding="ascii")
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS[self.job.id] = self.job
            self.job.delete_requested = True
        SIDECAR._finish_deleted(self.job)
        self.assertNotIn(self.job.id, SIDECAR._JOBS)
        self.assertTrue(artifact.is_file())
        self.assertFalse((self.job.directory / SIDECAR.JOB_STATE_FILENAME).exists())

    def test_zz_shutdown_keeps_generated_files(self):
        artifact = self.job.directory / "kept-after-shutdown.obj"
        artifact.write_text("kept", encoding="ascii")
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS[self.job.id] = self.job
        SIDECAR.shutdown_sidecar()
        self.assertTrue(artifact.is_file())


if __name__ == "__main__":
    unittest.main()

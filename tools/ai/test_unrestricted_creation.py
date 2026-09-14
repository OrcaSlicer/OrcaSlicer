"""Production-boundary regressions for creation without printer color limits."""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_job_support
import openai_preprocessor as preprocessor
import orca_ai_sidecar as sidecar


def rich_image(path: Path) -> Path:
    image = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
    for y in range(64, 448):
        for x in range(96, 416):
            image.putpixel((x, y), (x % 256, y % 256, (x + y) % 256, 255))
    image.save(path)
    return path


class UnrestrictedCreationTests(unittest.TestCase):
    def test_all_creation_entry_points_request_a_solid_backdrop_once(self):
        for style in ("sculpture", "realistic", "cartoon"):
            for entry in ("generate_image", "generate_geometry_reference_image", "preprocess_image"):
                with self.subTest(style=style, entry=entry), tempfile.TemporaryDirectory() as directory:
                    source = Path(directory) / "source.png"
                    Image.new("RGB", (64, 64), "white").save(source)
                    original = source.read_bytes()
                    destination = Path(directory) / "result.png"
                    with mock.patch.object(preprocessor, "_image_config", return_value=mock.Mock(model="fixture")), \
                         mock.patch.object(preprocessor, "_image_provider_request", return_value={}) as request, \
                         mock.patch.object(preprocessor, "_save_provider_image", return_value=destination), \
                         mock.patch.object(preprocessor, "_portrait_face_lock_mask"):
                        if entry == "preprocess_image":
                            preprocessor.preprocess_image(source, "white jacket portrait", destination, (), style)
                        elif entry == "generate_image":
                            preprocessor.generate_image("white jacket portrait", destination, (), style)
                        else:
                            preprocessor.generate_geometry_reference_image("white jacket portrait", destination, style)
                    request.assert_called_once()
                    route, body, content_type = request.call_args.args
                    if entry == "preprocess_image":
                        self.assertEqual(route, "/images/edits")
                        self.assertIn(b'name="background"\r\n\r\nopaque', body)
                        prompt = body.decode("utf-8", errors="replace")
                    else:
                        self.assertEqual(route, "/images/generations")
                        payload = json.loads(body)
                        self.assertEqual(payload["background"], "opaque")
                        prompt = payload["prompt"]
                    self.assertIn("uniform opaque solid-color studio background", prompt)
                    self.assertIn("never recolor the subject", prompt)
                    self.assertIn("Do not request transparency or draw a transparency checkerboard", prompt)
                    self.assertNotIn("a transparent background", prompt)
                    self.assertNotIn("Use a genuine alpha-transparent background", prompt)
                    self.assertEqual(source.read_bytes(), original)

    def test_monochrome_is_only_applied_to_the_selected_sculpture_style(self):
        for style in ("sculpture", "realistic", "cartoon"):
            prompt = preprocessor.build_text_image_prompt("a portrait", ("#123456",), style)
            self.assertEqual("selected monochrome sculpture style" in prompt, style == "sculpture")
            self.assertNotIn("#123456", prompt)

    def job(self, directory: str, source="text"):
        root = Path(directory) / "00000000-0000-0000-0000-000000000001"
        root.mkdir(exist_ok=True)
        return sidecar.Job(id=root.name, source=source, directory=root, style="realistic",
                           palette=("#FF0000", "#000000"), palette_roles={"primary": "#FF0000"},
                           print_settings=sidecar._normalize_print_settings({}))

    def test_text_preprocessing_ignores_legacy_printer_palette_and_keeps_user_color(self):
        with mock.patch.object(preprocessor, "complete_text", return_value="a white statue with a red scarf") as complete:
            preprocessor.preprocess_text("a white statue with a red scarf", ("#FF00FF",), "sculpture")
        prompt, user = complete.call_args.args
        self.assertNotIn("#FF00FF", prompt)
        self.assertIn("one monochrome", prompt)
        self.assertEqual(user, "a white statue with a red scarf")
        generated = model_job_support.generation_prompt(user, ("#FF00FF",), max_prompt_bytes=2000)
        self.assertNotIn("#FF00FF", generated)
        self.assertIn("red scarf", generated)

    def test_image_provider_prompt_is_independent_of_device_color_count(self):
        with tempfile.TemporaryDirectory() as directory:
            source = rich_image(Path(directory) / "source.png")
            prompts = []
            for count in (1, 4, 6):
                with mock.patch.object(preprocessor, "edit_image", return_value=source) as edit, \
                     mock.patch.object(preprocessor, "_portrait_face_lock_mask"):
                    preprocessor.preprocess_image(source, "保留人物和红色围巾", Path(directory)/f"out{count}.png",
                                                  ("#F0137A",) * count, "realistic")
                prompts.append(edit.call_args.args[1])
                self.assertNotIn("#F0137A", prompts[-1])
                self.assertIn("保留人物和红色围巾", prompts[-1])
                self.assertNotIn("selected filament palette is applied", prompts[-1])
            self.assertEqual(prompts[0], prompts[1])
            self.assertEqual(prompts[1], prompts[2])

    def test_text_image_transport_has_no_hidden_palette_material_direction(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(preprocessor, "_image_config", return_value=mock.Mock(model="fixture-image")), \
             mock.patch.object(preprocessor, "_image_provider_request", return_value={}) as request, \
             mock.patch.object(preprocessor, "_save_provider_image", return_value=Path(directory)/"out.png"):
            preprocessor.generate_geometry_reference_image("彩色人像", Path(directory)/"out.png", "realistic",
                                                          palette=("#C03191",), palette_roles={"primary": "#C03191"})
        payload = json.loads(request.call_args.args[1])
        self.assertNotIn("#C03191", payload["prompt"])
        self.assertNotIn("Design material colors:", payload["prompt"])
        self.assertEqual(payload["n"], 1)

    def test_creation_preserves_design_pixels_and_never_runs_print_color_processing(self):
        for source in ("text", "image"):
            with self.subTest(source=source), tempfile.TemporaryDirectory() as directory:
                job = self.job(directory, source)
                job.input_path = rich_image(Path(directory)/"source.png")
                with mock.patch.object(sidecar, "generate_geometry_reference_image", side_effect=lambda _p, out, *_a, **_k: rich_image(out)), \
                     mock.patch.object(sidecar, "preprocess_image", side_effect=lambda _i, _p, out, *_a: rich_image(out)), \
                     mock.patch.object(sidecar, "_assess_job_preview_visual_quality"), \
                     mock.patch.object(sidecar, "process_printable_image", side_effect=AssertionError("generation must not quantize")) as quantize:
                    if source == "text": sidecar._preprocess_text_job(job, "portrait")
                    else: sidecar._preprocess_image_job(job, job.input_path, "portrait")
                self.assertEqual(job.state, "awaiting_confirmation", job.message)
                self.assertEqual(job.palette, ())
                self.assertEqual(job.palette_roles, {})
                self.assertIsNone(job.strict_preview_path)
                self.assertIsNone(job.heatmap_path)
                self.assertEqual(job.raw_preview_path.read_bytes(), job.preview_path.read_bytes())
                with Image.open(job.preview_path) as image:
                    self.assertGreater(len(image.getcolors(512*512)), 256)
                self.assertEqual(sidecar._model_generation_reference(job), job.raw_preview_path)
                quantize.assert_not_called()

    def test_legacy_create_routes_ignore_palette_fields_including_unsupported_counts(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(sidecar, "_model_output_root", return_value=Path(directory)), \
             mock.patch.dict(sidecar.os.environ, {"OPENAI_API_KEY": "test-openai"}), \
             mock.patch.object(sidecar, "image_provider_status", return_value={"available": True}), \
             mock.patch.object(sidecar, "_submit") as submit, \
             mock.patch.dict(sidecar._JOBS, {}, clear=True):
            handler = sidecar.Handler.__new__(sidecar.Handler)
            handler._read_model_json = mock.Mock(return_value={"request_id": "fixture", "prompt": "rainbow", "style": "realistic",
                                                               "palette": ["#FE1234"] * 20, "palette_color_count": 20})
            handler.send_json = mock.Mock()
            handler._create_text_palette_recommendation()
            job, function, prompt = submit.call_args.args
            self.assertEqual(job.palette, ())
            self.assertEqual(function, sidecar._preprocess_text_job)
            self.assertEqual(prompt, "rainbow")
            submit.assert_called_once()

    def test_pending_legacy_generation_can_continue_without_palette_and_cannot_double_submit(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.job(directory)
            job.state = "awaiting_confirmation"
            handler = sidecar.Handler.__new__(sidecar.Handler)
            handler._get_job = mock.Mock(return_value=job)
            handler._read_model_json = mock.Mock(return_value={"prepared_prompt": "a colorful portrait", "palette": []})
            handler.send_json = mock.Mock()
            with mock.patch.object(sidecar._MODEL_PROVIDER_GATEWAY, "model_generation_available", return_value=True), \
                 mock.patch.object(sidecar, "_submit") as submit:
                handler._generate(job.id)
                self.assertEqual(job.palette, ())
                self.assertEqual(job.image_metrics["creation_color_policy"], "unrestricted-v1")
                with self.assertRaises(sidecar.RequestError) as error:
                    handler._generate(job.id)
                self.assertEqual(error.exception.code, "invalid_job_state")
                submit.assert_called_once()

    def test_restoring_completed_legacy_history_preserves_its_original_palette_record(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.job(directory)
            job.state = "ready"
            sidecar._persist_job(job)
            snapshot = (job.directory/"job.json").read_bytes()
            restored = sidecar._load_job(job.directory)
            self.assertIsNotNone(restored)
            self.assertEqual(restored.palette, job.palette)
            self.assertEqual((job.directory/"job.json").read_bytes(), snapshot)


if __name__ == "__main__":
    unittest.main()

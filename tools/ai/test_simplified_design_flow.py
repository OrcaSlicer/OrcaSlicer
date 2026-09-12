import json
from pathlib import Path
import sys
import tempfile
import unittest
import urllib.request
from unittest import mock

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import orca_ai_sidecar as sidecar
import openai_preprocessor as preprocessor
from model_job_support import assess_job_model_reference, printable_preview_message
from test_sidecar_contract import multipart_image_request, sidecar_server, temporary_environment, valid_png_bytes


class SimplifiedDesignFlowTests(unittest.TestCase):
    def make_job(self, directory, source="image"):
        return sidecar.Job(id="00000000-0000-0000-0000-000000000001", source=source,
                           directory=Path(directory), style="realistic", user_prompt="a portrait")

    def test_legacy_recommendation_creates_one_design_without_printer_palette(self):
        for source in ("text", "image"):
            with self.subTest(source=source), tempfile.TemporaryDirectory() as directory:
                job_directory = Path(directory) / "00000000-0000-0000-0000-000000000001"
                job_directory.mkdir()
                job = self.make_job(job_directory, source)
                job.palette_color_count = 2
                job.palette = ("#D96B43", "#202020")
                job.palette_roles = {"primary": "#D96B43"}
                job.palette_recommendation_confirmed = True
                job.generate_image = True
                job.input_path = job.directory / "input.png"
                job.input_path.write_bytes(valid_png_bytes(512))

                def reference(destination):
                    destination.write_bytes(valid_png_bytes(512))
                    return destination

                with (
                    mock.patch.object(sidecar, "recommend_printable_palette") as recommend,
                    mock.patch.object(preprocessor, "complete_text") as prepare,
                    mock.patch.object(sidecar, "generate_geometry_reference_image",
                                      side_effect=lambda _prompt, destination, *_args, **_kwargs: reference(destination)) as text,
                    mock.patch.object(sidecar, "preprocess_image",
                                      side_effect=lambda _source, _prompt, destination, *_args, **_kwargs: reference(destination)) as image,
                    mock.patch.object(sidecar, "_assess_job_preview_visual_quality"),
                    mock.patch.object(sidecar, "_apply_printable_image_pipeline",
                                      side_effect=AssertionError("Creation must not quantize the design")),
                    mock.patch.object(sidecar, "_submit") as submit,
                ):
                    sidecar._recommend_palette_job(job)
                    sidecar._persist_job(job)
                    restored = sidecar._load_job(job.directory)
                    self.assertIsNotNone(restored)
                    sidecar._resume_restored_jobs([restored])
                recommend.assert_not_called()
                submit.assert_not_called()
                prepare.assert_not_called()
                self.assertEqual(text.call_count, int(source == "text"))
                self.assertEqual(image.call_count, int(source == "image"))
                self.assertFalse(job.palette_recommendation_confirmed)
                self.assertEqual(job.palette, ())
                self.assertEqual(job.palette_roles, {})
                self.assertEqual(job.state, "awaiting_confirmation")
                self.assertEqual(restored.state, "awaiting_confirmation")
                self.assertEqual(job.preview_path.read_bytes(), job.raw_preview_path.read_bytes())

    def test_missing_stopped_or_failed_legacy_design_does_not_continue(self):
        for failure in ("missing_input", "stopped", "provider_failure"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                job = self.make_job(directory)
                job.generate_image = True
                if failure != "missing_input":
                    job.input_path = Path(directory) / "input.png"
                    job.input_path.write_bytes(valid_png_bytes(512))
                if failure == "stopped":
                    job.stop_event.set()
                with (
                    mock.patch.object(sidecar, "recommend_printable_palette") as recommend,
                    mock.patch.object(sidecar, "preprocess_image",
                                      side_effect=sidecar.OpenAIPreprocessorError("offline fixture")) as image,
                    mock.patch.object(sidecar, "_MODEL_PROVIDER_GATEWAY") as model,
                ):
                    sidecar._recommend_palette_job(job)
                recommend.assert_not_called()
                model.start_or_reuse_model_task.assert_not_called()
                self.assertEqual(image.call_count, int(failure == "provider_failure"))
                self.assertEqual(job.state, "stopped" if failure == "stopped" else "failed")
                self.assertIsNone(job.preview_path)
                self.assertIsNone(job.artifact_path)

    def test_legacy_http_routes_create_once_without_palette_confirmation(self):
        for source in ("text", "image"):
            for auto_generate in (False, True):
                with (self.subTest(source=source, auto_generate=auto_generate),
                      tempfile.TemporaryDirectory() as directory,
                      temporary_environment(OPENAI_API_KEY="offline-test", ORCASLICER_AI_OUTPUT_DIR=directory),
                      mock.patch.dict(sidecar._JOBS, {}, clear=True),
                      mock.patch.object(sidecar, "image_provider_status", return_value={"available": True}),
                      mock.patch.object(sidecar, "recommend_printable_palette") as recommend,
                      mock.patch.object(sidecar, "_submit", side_effect=lambda job, worker, *args: worker(job, *args)) as submit,
                      mock.patch.object(sidecar, "_preprocess_text_job") as text,
                      mock.patch.object(sidecar, "_preprocess_image_job") as image,
                      sidecar_server(sidecar.Handler) as port):
                    fields = {"request_id": "offline", "style": "cartoon", "palette_color_count": 2}
                    if source == "text":
                        fields["prompt"] = "a mechanical cat"
                        if auto_generate:
                            fields["generate_image"] = True
                        body, content_type = json.dumps(fields).encode(), "application/json"
                    else:
                        fields = {key: str(value) for key, value in fields.items()}
                        fields["instruction"] = "a mechanical cat"
                        if auto_generate:
                            fields["generate_image"] = "true"
                        body, content_type = multipart_image_request(fields, valid_png_bytes())
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/recommend-{source}-palette",
                        data=body, method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": content_type})
                    with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=5) as response:
                        public = json.loads(response.read())["job"]
                    job = sidecar._JOBS[public["id"]]
                    recommend.assert_not_called()
                    submit.assert_called_once()
                    self.assertEqual(text.call_count, int(source == "text"))
                    self.assertEqual(image.call_count, int(source == "image"))
                    worker = text if source == "text" else image
                    self.assertIs(worker.call_args.args[0], job)
                    self.assertIn("a mechanical cat", worker.call_args.args[-1])
                    self.assertEqual(public["state"], "preprocessing")
                    self.assertEqual(job.palette, ())
                    self.assertEqual(job.palette_roles, {})
                    restored = sidecar._load_job(job.directory)
                    self.assertIsNotNone(restored)
                    self.assertFalse(restored.generate_image)
                    self.assertFalse(restored.palette_recommendation_confirmed)
                    sidecar._resume_restored_jobs([restored])
                    submit.assert_called_once()
                    self.assertEqual(text.call_count + image.call_count, 1)

    def test_legacy_palette_does_not_change_design_or_replace_user_colors(self):
        palette = ("#D96B43", "#202020")
        roles = {"primary": palette[0], "structure": palette[1]}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "design.png"
            prompts = {"edit": [], "generate": []}
            for selected_palette, selected_roles in (((), {}), (palette, roles)):
                with mock.patch.object(preprocessor, "edit_image", return_value=output) as edit:
                    preprocessor.preprocess_image(output, "cat with a blue scarf", output, selected_palette,
                                                  "cartoon", palette_roles=selected_roles)
                edit.assert_called_once()
                prompts["edit"].append(edit.call_args.args[1])
                with (mock.patch.object(preprocessor, "_image_config", return_value=mock.Mock(model="offline")),
                      mock.patch.object(preprocessor, "_image_provider_request", return_value={}) as request,
                      mock.patch.object(preprocessor, "_save_provider_image", return_value=output)):
                    preprocessor.generate_geometry_reference_image("cat with a blue scarf", output,
                                                                  palette=selected_palette, palette_roles=selected_roles)
                request.assert_called_once()
                payload = json.loads(request.call_args.args[1])
                prompts["generate"].append(payload["prompt"])
                self.assertEqual(payload["n"], 1)
            for entry, pair in prompts.items():
                with self.subTest(entry=entry):
                    self.assertEqual(pair[0], pair[1])
                    self.assertIn("cat with a blue scarf", pair[0])
                    self.assertIn("continuous tonal modeling", pair[0])
                    self.assertIn("natural colors, continuous gradients, texture", pair[0])
                    self.assertNotIn("#D96B43", pair[0])
                    self.assertNotIn("#202020", pair[0])
                    self.assertNotIn("allowed printable palette", pair[0])

    def test_design_reference_keeps_full_provider_image_and_ignores_palette_draft(self):
        for palette in ((), ("#FFFFFF",), ("#FFFFFF", "#111111", "#F0C8AA", "#315B48")):
            with self.subTest(palette=palette), tempfile.TemporaryDirectory() as directory:
                job = self.make_job(directory)
                job.palette = palette
                job.image_metrics = {"design_reference": "ai-design-v1", "portrait_geometry": {"detected": True}}
                job.input_path = Path(directory) / "input.png"
                job.raw_preview_path = Path(directory) / "raw.png"
                job.model_reference_path = Path(directory) / "strict.png"
                for path in (job.input_path, job.raw_preview_path, job.model_reference_path):
                    image = Image.new("RGBA", (512, 768), (0, 0, 0, 0))
                    ImageDraw.Draw(image).rectangle((100, 70, 410, 700), fill=(180, 150, 120))
                    image.save(path)
                before = job.raw_preview_path.read_bytes()
                with mock.patch.object(sidecar, "_prepare_portrait_geometry_provider_reference", side_effect=AssertionError("Do not crop")):
                    self.assertEqual(sidecar._geometry_generation_reference(job), job.raw_preview_path)
                self.assertEqual(job.raw_preview_path.read_bytes(), before)
                self.assertEqual(sidecar._public_job(job)["image_outputs"]["model_reference"]["size_bytes"], len(before))
                with (mock.patch.object(sidecar, "_MODEL_PROVIDER_GATEWAY") as gateway,
                      mock.patch.object(sidecar, "_ensure_portrait_multiview", return_value=None)):
                    gateway.start_or_reuse_model_task.side_effect = sidecar.JobStopped()
                    sidecar._generate_job(job, "", authorization=sidecar.PaidTaskAuthorization.confirmed("offline-test"))
                    gateway.start_or_reuse_model_task.assert_called_once()
                    request = gateway.start_or_reuse_model_task.call_args.args[0]
                    self.assertEqual(request.source, "image")
                    self.assertEqual(request.image_path, job.raw_preview_path)
                    self.assertEqual(request.image_path.read_bytes(), before)
                with mock.patch("model_job_support.assess_model_input_image", return_value={"model_input_eligible": True}) as assess:
                    assess_job_model_reference(job)
                    assess.assert_called_once_with(job.raw_preview_path)

    def test_design_reference_does_not_gate_geometry_on_palette_draft_quality(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory)
            job.palette = ("#FFFFFF",)
            job.image_metrics = {"design_reference": "ai-design-v1", "palette_quality_ok": False}
            self.assertEqual(printable_preview_message(job, "Review the AI design."), "Review the AI design.")

    def test_visual_review_compares_original_to_the_actual_design_not_the_color_draft(self):
        with (tempfile.TemporaryDirectory() as directory, temporary_environment(OPENAI_API_KEY="offline-test"),
              mock.patch.object(sidecar, "review_prepared_reference", return_value={}) as review):
            job = self.make_job(directory)
            job.generation_profile = "quality"
            job.image_metrics = {"design_reference": "ai-design-v1"}
            job.raw_preview_path = Path(directory) / "raw.png"
            job.model_reference_path = Path(directory) / "reference.png"
            job.preview_path = Path(directory) / "draft.png"
            original = Path(directory) / "original.png"
            sidecar._assess_job_preview_visual_quality(job, original)
            self.assertEqual(review.call_args.args[:3], (original, job.raw_preview_path, job.raw_preview_path))

    def test_unlimited_color_edit_uses_one_geometry_reference_with_solid_background(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / "input.png", Path(directory) / "output.png"
            with mock.patch.object(preprocessor, "edit_image", return_value=output) as edit:
                preprocessor.preprocess_image(source, "mechanical cat", output, (), "cartoon")
            edit.assert_called_once()
            self.assertIn("geometry reference", edit.call_args.args[1])
            self.assertIn("continuous tonal modeling", edit.call_args.args[1])
            self.assertEqual(edit.call_args.kwargs, {"background": "opaque"})
            self.assertIn("uniform opaque solid-color studio background", edit.call_args.args[1])
            self.assertIn("never recolor the subject", edit.call_args.args[1])
            self.assertIn("Do not request transparency or draw a transparency checkerboard", edit.call_args.args[1])
            self.assertNotIn("allowed printable palette", edit.call_args.args[1])


if __name__ == "__main__":
    unittest.main()

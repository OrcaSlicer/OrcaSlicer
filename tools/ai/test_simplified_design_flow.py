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

    def recommendation(self):
        return mock.Mock(as_dict=mock.Mock(return_value={
            "summary": "Two broad materials",
            "colors": [
                {"hex": "#D96B43", "name": "clay", "role": "primary", "usage": "body", "reason": "clear"},
                {"hex": "#202020", "name": "dark", "role": "structure", "usage": "base", "reason": "clear"},
            ],
        }))

    def test_recommend_and_generate_continues_once_without_confirmation(self):
        for source in ("text", "image"):
            with self.subTest(source=source), tempfile.TemporaryDirectory() as directory:
                job = self.make_job(directory, source)
                job.palette_color_count = 2
                job.generate_image = True
                job.input_path = Path(directory) / "input.png"
                Image.new("RGB", (128, 128), "white").save(job.input_path)
                def finish(*_args):
                    self.assertEqual(job.state, "preprocessing")
                    self.assertTrue(job.palette_recommendation_confirmed)
                    job.state = "awaiting_confirmation"
                with (
                    mock.patch.object(sidecar, "recommend_printable_palette", return_value=self.recommendation()) as recommend,
                    mock.patch.object(sidecar, "_preprocess_text_job", side_effect=finish) as text,
                    mock.patch.object(sidecar, "_preprocess_image_job", side_effect=finish) as image,
                ):
                    sidecar._recommend_palette_job(job)
                    sidecar._recommend_palette_job(job)
                recommend.assert_called_once()
                self.assertEqual(text.call_count, int(source == "text"))
                self.assertEqual(image.call_count, int(source == "image"))
                self.assertTrue(job.palette_recommendation_confirmed)
                self.assertEqual(job.palette, ("#D96B43", "#202020"))
                self.assertEqual(job.palette_roles, {"primary": "#D96B43", "structure": "#202020"})
                self.assertEqual(job.state, "awaiting_confirmation")

    def test_failed_or_stopped_recommendation_never_generates_an_image(self):
        for stopped in (False, True):
            with self.subTest(stopped=stopped), tempfile.TemporaryDirectory() as directory:
                job = self.make_job(directory)
                job.generate_image = True
                if stopped:
                    job.stop_event.set()
                with (
                    mock.patch.object(sidecar, "recommend_printable_palette", side_effect=sidecar.OpenAIPreprocessorError("offline")),
                    mock.patch.object(sidecar, "_preprocess_image_job") as image,
                ):
                    sidecar._recommend_palette_job(job)
                image.assert_not_called()

    def test_http_one_click_flag_and_legacy_confirmation_contract(self):
        for source in ("text", "image"):
            for auto_generate in (False, True):
                with (self.subTest(source=source, auto_generate=auto_generate),
                      tempfile.TemporaryDirectory() as directory,
                      temporary_environment(OPENAI_API_KEY="offline-test", ORCASLICER_AI_OUTPUT_DIR=directory),
                      mock.patch.dict(sidecar._JOBS, {}, clear=True),
                      mock.patch.object(sidecar, "recommend_printable_palette", return_value=self.recommendation()) as recommend,
                      mock.patch.object(sidecar, "_submit", side_effect=lambda job, worker: worker(job)),
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
                    with urllib.request.urlopen(request, timeout=5) as response:
                        public = json.loads(response.read())["job"]
                    job = sidecar._JOBS[public["id"]]
                    recommend.assert_called_once()
                    self.assertEqual(text.call_count + image.call_count, int(auto_generate))
                    self.assertEqual(public["state"], "preprocessing" if auto_generate else "awaiting_palette_confirmation")
                    restored = sidecar._load_job(job.directory)
                    self.assertIsNotNone(restored)
                    self.assertEqual(restored.generate_image, auto_generate)
                    self.assertEqual(restored.palette_recommendation_confirmed, auto_generate)
                    if auto_generate:
                        sidecar._recommend_palette_job(restored)
                        recommend.assert_called_once()
                        self.assertEqual(text.call_count + image.call_count, 1)

    def test_palette_colors_guide_materials_without_flattening_design(self):
        palette = ("#D96B43", "#202020")
        roles = {"primary": palette[0], "structure": palette[1]}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "design.png"
            with mock.patch.object(preprocessor, "edit_image", return_value=output) as edit:
                preprocessor.preprocess_image(output, "cat", output, palette, "cartoon", palette_roles=roles)
            prompt = edit.call_args.args[1]
            self.assertIn("primary: #D96B43", prompt)
            self.assertIn("continuous diffuse shading", prompt)
            self.assertNotIn("allowed printable palette", prompt)
            with (mock.patch.object(preprocessor, "_image_config", return_value=mock.Mock(model="offline")),
                  mock.patch.object(preprocessor, "_image_provider_request", return_value={}) as request,
                  mock.patch.object(preprocessor, "_save_provider_image", return_value=output)):
                preprocessor.generate_geometry_reference_image("cat", output, palette=palette, palette_roles=roles)
            payload = json.loads(request.call_args.args[1])
            self.assertIn("primary: #D96B43", payload["prompt"])
            self.assertEqual(payload["n"], 1)

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

    def test_unlimited_color_edit_uses_geometry_quality_prompt_and_transparency(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / "input.png", Path(directory) / "output.png"
            with mock.patch.object(preprocessor, "edit_image", return_value=output) as edit:
                preprocessor.preprocess_image(source, "mechanical cat", output, (), "cartoon")
            self.assertIn("geometry reference", edit.call_args.args[1])
            self.assertIn("continuous tonal modeling", edit.call_args.args[1])
            self.assertEqual(edit.call_args.kwargs, {"background": "transparent"})
            self.assertNotIn("allowed printable palette", edit.call_args.args[1])


if __name__ == "__main__":
    unittest.main()

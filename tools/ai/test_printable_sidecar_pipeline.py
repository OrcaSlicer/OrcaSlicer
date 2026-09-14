#!/usr/bin/env python3
import json
import sys
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from PIL import Image


TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))
import orca_ai_sidecar as sidecar  # noqa: E402


PALETTE = ("#D93632", "#3B8C54", "#315CA8", "#F2F1EA")
JOB_ID = "00000000-0000-0000-0000-000000000001"


def hex_rgb(color):
    return tuple(int(color[index:index + 2], 16) for index in (1, 3, 5))


def synthetic_preview(path, *_args, **_kwargs):
    image = Image.new("RGB", (512, 512), (244, 242, 235))
    for x in range(86, 426):
        for y in range(64, 448):
            image.putpixel((x, y), (210, 55, 50) if x < 256 else (45, 105, 175))
    for x in range(240, 272):
        for y in range(235, 267):
            image.putpixel((x, y), (40, 150, 80))
    image.save(path)
    return Path(path)


class PrintableSidecarIntegrationTests(unittest.TestCase):
    def make_job(self, directory, source="text"):
        job_directory = Path(directory) / JOB_ID
        job_directory.mkdir(exist_ok=True)
        return sidecar.Job(
            id=JOB_ID,
            source=source,
            directory=job_directory,
            palette=PALETTE,
            style="cartoon",
            print_settings=sidecar._normalize_print_settings({
                "width_mm": 96,
                "nozzle_mm": 0.4,
                "line_width_mm": 0.4,
                "minimum_feature_mm": 2.0,
                "shadow_color": "blue",
            }),
        )

    def test_text_job_keeps_design_pixels_without_print_color_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory)
            job.user_prompt = "一只机械麒麟"
            with mock.patch.object(
                     sidecar,
                     "generate_geometry_reference_image",
                     side_effect=lambda _instruction, output, *_args, **_kwargs: synthetic_preview(output),
                 ) as generate:
                sidecar._preprocess_text_job(job, job.user_prompt)

            self.assertEqual(job.state, "awaiting_confirmation")
            self.assertTrue(job.raw_preview_path.is_file())
            self.assertIsNone(job.strict_preview_path)
            self.assertTrue(job.preview_path.is_file())
            self.assertIsNone(job.heatmap_path)
            self.assertIsNone(job.metadata_path)
            self.assertEqual(job.mask_paths, {})
            self.assertEqual(job.palette, ())
            self.assertEqual(job.raw_preview_path.read_bytes(), job.preview_path.read_bytes())
            self.assertTrue(job.image_metrics["model_input_quality"]["model_input_eligible"])
            self.assertTrue(job.image_metrics["generation_input_quality"]["model_input_eligible"])
            self.assertEqual(job.image_metrics["generation_reference"], "ai_design")
            self.assertEqual(generate.call_args.args[0], job.user_prompt)
            self.assertEqual(generate.call_args.args[-2], job.style)
            self.assertEqual(generate.call_args.args[-1], "")

    def test_image_job_uses_unquantized_ai_design_and_preserves_colors(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory, "image")
            input_path = Path(directory) / "input.png"
            Image.new("RGB", (96, 96), "white").save(input_path)
            job.input_path = input_path
            with mock.patch.object(sidecar, "preprocess_image", side_effect=lambda _source, _instruction, output, *_args: synthetic_preview(output)):
                sidecar._preprocess_image_job(job, input_path, "保留主体")

            self.assertNotEqual(job.raw_preview_path, job.preview_path)
            self.assertTrue(job.geometry_reference_path.is_file())
            self.assertEqual(job.preview_path.name, "preview.png")
            self.assertEqual(sidecar._model_generation_reference(job), job.raw_preview_path)
            with Image.open(job.preview_path) as preview:
                self.assertEqual(preview.mode, "RGB")
                self.assertFalse(set(preview.getdata()) <= {hex_rgb(color) for color in PALETTE})
            self.assertEqual(job.raw_preview_path.read_bytes(), job.preview_path.read_bytes())
            with Image.open(sidecar._model_generation_reference(job)) as reference:
                self.assertEqual(reference.getpixel((100, 100))[:3], (210, 55, 50))

    def test_quality_portrait_geometry_reference_inherits_validated_alpha(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory, "image")
            job.style = "realistic"
            job.generation_profile = "quality"
            job.image_metrics["portrait_skin_cleanup"] = {"activated": 1}

            original = Path(directory) / "input.png"
            Image.new("RGB", (512, 768), "white").save(original)
            job.input_path = original

            geometry = job.directory / "geometry-reference.png"
            checker = Image.new("RGB", (512, 768), (244, 244, 244))
            checker.paste((220, 170, 145), (96, 64, 416, 704))
            checker.save(geometry)
            job.geometry_reference_path = geometry

            model_reference = job.directory / "model_reference.png"
            validated = Image.new("RGBA", (512, 768), (240, 240, 235, 0))
            validated.paste((220, 170, 145, 255), (96, 64, 416, 704))
            # The user-facing reference keeps a deliberately soft halo; this
            # must never become rear-plate geometry in the paid model input.
            validated.putpixel((80, 384), (220, 170, 145, 96))
            validated.save(model_reference)
            job.model_reference_path = model_reference

            subject_mask = job.directory / "mask_subject.png"
            hard_mask = Image.new("L", (512, 768), 0)
            hard_mask.paste(255, (96, 64, 416, 704))
            hard_mask.save(subject_mask)
            job.subject_mask_path = subject_mask

            quality = sidecar._assess_job_generation_reference(job)

            with Image.open(geometry) as repaired:
                self.assertEqual(repaired.mode, "RGBA")
                self.assertEqual(repaired.getpixel((0, 0))[3], 0)
                self.assertEqual(repaired.getpixel((80, 384))[3], 0)
                self.assertEqual(repaired.getpixel((256, 384))[3], 255)
            self.assertTrue(quality["model_input_eligible"])
            self.assertEqual(
                job.image_metrics["generation_reference"],
                "identity_sculpted_geometry_reference",
            )
            provider = job.directory / sidecar.PORTRAIT_GEOMETRY_PROVIDER_FILENAME
            with Image.open(provider) as submitted:
                self.assertEqual(submitted.getpixel((384, 384))[:3], (220, 170, 145))

    def test_printable_outputs_round_trip_through_job_state(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory)
            raw = job.directory / "style-preview-raw.png"
            synthetic_preview(raw)
            sidecar._apply_printable_image_pipeline(job, raw)
            job.geometry_reference_path = raw
            sidecar._assess_job_model_reference(job)
            sidecar._assess_job_generation_reference(job)
            job.state = "awaiting_confirmation"
            sidecar._persist_job(job)

            restored = sidecar._load_job(job.directory)

            self.assertIsNotNone(restored)
            self.assertEqual(restored.print_settings, job.print_settings)
            self.assertEqual(restored.image_metrics, job.image_metrics)
            self.assertTrue(restored.geometry_reference_path.samefile(raw))
            self.assertEqual(set(restored.mask_paths), set(job.mask_paths))
            public = sidecar._public_job(restored)
            self.assertTrue(public["image_outputs"]["strict_preview"]["ready"])
            self.assertTrue(public["image_outputs"]["metadata"]["ready"])
            self.assertEqual(public["image_metrics"]["minimum_feature_px"], job.image_metrics["minimum_feature_px"])
            self.assertEqual(
                public["image_metrics"]["model_input_quality"],
                job.image_metrics["model_input_quality"],
            )
            self.assertEqual(public["image_metrics"]["generation_reference"], "model_reference")

            restored.source = "image"
            restored.user_prompt = sidecar.DEFAULT_IMAGE_INSTRUCTION
            self.assertEqual(sidecar._public_job(restored)["user_prompt"], "")

    def test_unavailable_visual_review_warns_without_replacing_local_quality_decisions(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory, "image")
            for key in ("model_input_quality", "generation_input_quality"):
                job.image_metrics[key] = {"model_input_eligible": False, "blockers": ["subject_cropped"], "warnings": []}
            for _ in range(2):
                sidecar._apply_preview_visual_quality_gate(job, {"status": "unavailable"})
            for key in ("model_input_quality", "generation_input_quality"):
                quality = job.image_metrics[key]
                self.assertFalse(quality["model_input_eligible"])
                self.assertEqual(quality["blockers"], ["subject_cropped"])
                self.assertEqual(quality["warnings"], ["reference_visual_review_unavailable"])
            sidecar._apply_preview_visual_quality_gate(job, {"status": "pass"})
            self.assertEqual(job.image_metrics["model_input_quality"]["warnings"], [])

    def test_portrait_pipeline_persists_recovered_skin_and_garment_roles(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory, "image")
            job.palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
            job.palette_roles = {
                "primary": "#F2C9AE",
                "structure": "#1F1B1C",
                "light": "#F4F4F0",
                "accent": "#4E6F5B",
            }
            raw = job.directory / "style-preview-raw.png"
            raw.write_bytes(b"preview")
            result = mock.Mock(
                strict_preview=job.directory / "four_color_preview.png",
                clean_preview=job.directory / "clean_preview.png",
                model_reference=job.directory / "model_reference.png",
                heatmap=job.directory / "unprintable_heatmap.png",
                metadata=job.directory / "metadata.json",
                background_mask=job.directory / "mask_background.png",
                subject_mask=job.directory / "mask_subject.png",
                masks={},
                palette_usage={},
                metrics={
                    "portrait_skin_cleanup": {
                        "activated": 1,
                        "garment_color": "#F4F4F0",
                        "skin_color": "#F2C9AE",
                    }
                },
            )

            with mock.patch.object(sidecar, "process_printable_image", return_value=result):
                sidecar._apply_printable_image_pipeline(job, raw)

            self.assertEqual(job.palette_roles["primary"], "#F4F4F0")
            self.assertEqual(job.palette_roles["light"], "#F2C9AE")
            self.assertEqual(set(job.palette_roles.values()), set(job.palette))

    def test_all_documented_output_routes_are_loopback_download_routes(self):
        job_id = "00000000-0000-0000-0000-000000000001"
        for action in (
            "raw-preview", "strict-preview", "preview", "model-reference", "heatmap", "metadata",
            "background-mask", "subject-mask", "mask-red", "mask-white",
        ):
            self.assertEqual(
                sidecar.Handler._job_route(f"/v1/orcaslicer/model-jobs/{job_id}/{action}"),
                (job_id, action),
            )

    def test_preprocess_failure_round_trip_preserves_actionable_category(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory)
            error = sidecar.OpenAIPreprocessorError(
                "service unavailable",
                code="image_service_unavailable",
                retryable=True,
                ambiguous=True,
            )
            sidecar._fail_preprocess_job(job, error)
            restored = sidecar._load_job(job.directory)
            public = sidecar._public_job(restored)

        self.assertEqual(public["state"], "failed")
        self.assertEqual(public["provider_failure"]["code"], "image_service_unavailable")
        self.assertTrue(public["provider_failure"]["retryable"])
        self.assertTrue(public["provider_failure"]["ambiguous"])


if __name__ == "__main__":
    unittest.main()

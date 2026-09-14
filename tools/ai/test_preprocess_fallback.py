#!/usr/bin/env python3
import contextlib
import importlib.util
import os
import sys
import unittest
from pathlib import Path
from unittest import mock


TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


SIDECAR = load_module("orca_ai_sidecar_preprocess_fallback", TOOLS_AI / "orca_ai_sidecar.py")


@contextlib.contextmanager
def fallback_environment(value):
    name = "ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK"
    original = os.environ.get(name)
    try:
        if value is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = value
        yield
    finally:
        if original is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = original


class PreprocessFallbackTests(unittest.TestCase):
    def tearDown(self):
        with SIDECAR._JOBS_LOCK:
            jobs = list(SIDECAR._JOBS.values())
            SIDECAR._JOBS.clear()
        for job in jobs:
            SIDECAR._cleanup_job(job)

    def new_job(self, source):
        job = SIDECAR._new_job(source, ("#FF0000", "#00FF00"))
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS[job.id] = job
        return job

    def test_text_preprocessing_remains_fail_closed_by_default(self):
        job = self.new_job("text")
        error = SIDECAR.OpenAIPreprocessorError("preprocessor unavailable")
        with fallback_environment(None), mock.patch.object(SIDECAR, "generate_geometry_reference_image", side_effect=error):
            SIDECAR._preprocess_text_job(job, "printable calibration cube")

        self.assertEqual(job.state, "failed")
        self.assertEqual(job.message, "preprocessor unavailable")

    def test_text_fallback_retains_subject_and_adds_print_constraints(self):
        job = self.new_job("text")
        error = SIDECAR.OpenAIPreprocessorError("preprocessor unavailable")
        with fallback_environment("1"), \
             mock.patch.object(SIDECAR, "generate_geometry_reference_image", side_effect=error) as generate:
            SIDECAR._preprocess_text_job(job, "printable calibration cube with a red stripe")

        self.assertEqual(job.state, "awaiting_confirmation")
        self.assertTrue(job.prepared_prompt.startswith("printable calibration cube with a red stripe"))
        self.assertIn("watertight printable model", job.prepared_prompt)
        self.assertIn("Preserve meaningful separate parts", job.prepared_prompt)
        self.assertNotIn("#FF0000", job.prepared_prompt)
        self.assertNotIn("#00FF00", job.prepared_prompt)
        self.assertIn("natural gradients, textures and material detail without reducing colors", job.prepared_prompt)
        self.assertEqual(job.palette, ())
        generate.assert_called_once()
        self.assertIn("original prompt", job.message)

    def test_generation_prompt_ignores_legacy_palette_flags_but_keeps_user_color(self):
        prompts = []
        for constrained in (False, True):
            for palette in ((), ("#FF0000", "#00FF00")):
                prompt = SIDECAR._generation_prompt(
                    "one faithful portrait with a blue scarf",
                    palette,
                    max_prompt_bytes=SIDECAR.MAX_PROMPT_BYTES,
                    constrain_palette=constrained,
                )
                prompts.append(prompt)
                self.assertTrue(prompt.startswith("one faithful portrait with a blue scarf"))
                self.assertIn("natural gradients, textures and material detail without reducing colors", prompt)
                self.assertNotIn("#FF0000", prompt)
                self.assertNotIn("#00FF00", prompt)
        self.assertEqual(1, len(set(prompts)))

    def test_image_preprocessing_never_uses_original_as_style_preview(self):
        job = self.new_job("image")
        image = b"\xff\xd8\xff\xe0original-jpeg"
        input_path = job.directory / "input.jpg"
        input_path.write_bytes(image)
        error = SIDECAR.OpenAIPreprocessorError("preprocessor unavailable")
        with fallback_environment("true"), mock.patch.object(SIDECAR, "preprocess_image", side_effect=error):
            SIDECAR._preprocess_image_job(job, input_path, "make it printable")

        self.assertEqual(job.state, "failed")
        self.assertEqual(job.message, "preprocessor unavailable")
        self.assertIsNone(job.preview_path)


if __name__ == "__main__":
    unittest.main()

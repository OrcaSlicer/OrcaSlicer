#!/usr/bin/env python3
import base64
import contextlib
import http.client
from io import BytesIO
import os
import json
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

from PIL import Image

from tools.ai import openai_preprocessor as preprocessor


@contextlib.contextmanager
def configured_base_url(value):
    names = ("OPENAI_PRO_API", "OPENAI_PRO_URL", "OPENAI_API_KEY", "OPENAI_BASE_URL")
    original = {name: os.environ.get(name) for name in names}
    try:
        os.environ["OPENAI_API_KEY"] = "test-key"
        os.environ["OPENAI_BASE_URL"] = value
        os.environ.pop("OPENAI_PRO_API", None)
        os.environ.pop("OPENAI_PRO_URL", None)
        yield
    finally:
        for name, previous in original.items():
            if previous is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = previous


class OpenAIBaseUrlTests(unittest.TestCase):
    def configured_url(self, value):
        with configured_base_url(value):
            base, _, _, _ = preprocessor._config()
        return base

    def test_domain_root_uses_standard_v1_prefix(self):
        self.assertEqual(self.configured_url("https://laotie.dev"), "https://laotie.dev/v1")

    def test_domain_root_with_trailing_slash_uses_standard_v1_prefix(self):
        self.assertEqual(self.configured_url("https://laotie.dev/"), "https://laotie.dev/v1")

    def test_existing_v1_prefix_is_preserved(self):
        self.assertEqual(self.configured_url("https://laotie.dev/v1/"), "https://laotie.dev/v1")

    def test_custom_compatible_prefix_is_preserved(self):
        self.assertEqual(
            self.configured_url("https://gateway.example/openai/v1/"),
            "https://gateway.example/openai/v1",
        )

    def test_query_string_is_rejected(self):
        with configured_base_url("https://laotie.dev?token=unsafe"):
            with self.assertRaises(preprocessor.OpenAIPreprocessorError):
                preprocessor._config()

    def test_image_quality_defaults_to_high_and_rejects_unknown_values(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("OPENAI_IMAGE_QUALITY", None)
            self.assertEqual(preprocessor._image_quality(), "high")
        with mock.patch.dict(os.environ, {"OPENAI_IMAGE_QUALITY": "ultra"}):
            with self.assertRaises(preprocessor.OpenAIPreprocessorError):
                preprocessor._image_quality()

    def test_provider_errors_expose_safe_retry_and_ambiguity_semantics(self):
        cases = (
            (401, "image_auth_failed", False, False),
            (403, "image_auth_failed", False, False),
            (429, "image_rate_limited", True, False),
            (400, "image_rejected", False, False),
            (503, "image_service_unavailable", True, True),
        )
        for status, code, retryable, ambiguous in cases:
            opener = mock.Mock()
            opener.open.side_effect = urllib.error.HTTPError("https://example.invalid", status, "error", None, None)
            with (
                self.subTest(status=status),
                configured_base_url("https://laotie.dev"),
                mock.patch.object(preprocessor, "build_network_opener", return_value=opener),
                self.assertRaises(preprocessor.OpenAIPreprocessorError) as raised,
            ):
                preprocessor._provider_request("/images/generations", b"{}", "application/json")
            self.assertEqual(raised.exception.code, code)
            self.assertEqual(raised.exception.retryable, retryable)
            self.assertEqual(raised.exception.ambiguous, ambiguous)

    def test_connection_failure_is_ambiguous_and_not_retried(self):
        opener = mock.Mock()
        opener.open.side_effect = urllib.error.URLError("offline")
        with (
            configured_base_url("https://laotie.dev"),
            mock.patch.object(preprocessor, "build_network_opener", return_value=opener),
            self.assertRaises(preprocessor.OpenAIPreprocessorError) as raised,
        ):
            preprocessor._provider_request("/images/generations", b"{}", "application/json")

        self.assertEqual(raised.exception.code, "image_connection_failed")
        self.assertTrue(raised.exception.retryable)
        self.assertTrue(raised.exception.ambiguous)
        opener.open.assert_called_once()


class ImageProviderSelectionTests(unittest.TestCase):
    ENVIRONMENT_NAMES = (
        "OPENAI_PRO_API",
        "OPENAI_PRO_URL",
        "OPENAI_API_KEY",
        "OPENAI_BASE_URL",
        "OPENAI_IMAGE_MODEL",
    )

    def image_config(self, **environment):
        original = {name: os.environ.get(name) for name in self.ENVIRONMENT_NAMES}
        try:
            for name in self.ENVIRONMENT_NAMES:
                os.environ.pop(name, None)
            os.environ.update(environment)
            return preprocessor._image_config()
        finally:
            for name, value in original.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value

    def test_pro_image_provider_has_priority_over_legacy_text_provider(self):
        config = self.image_config(
            OPENAI_PRO_API="test-pro",
            OPENAI_PRO_URL="https://v.3dprint.beer/managed-ai/v1/",
            OPENAI_API_KEY="test-legacy",
            OPENAI_BASE_URL="https://legacy.example/v1",
        )

        self.assertEqual(config.base_url, "https://v.3dprint.beer/managed-ai/v1")
        self.assertEqual(config.api_key, "test-pro")
        self.assertEqual(config.source, "pro")

    def test_pro_image_settings_do_not_change_text_or_vision_provider(self):
        names = self.ENVIRONMENT_NAMES
        original = {name: os.environ.get(name) for name in names}
        try:
            for name in names:
                os.environ.pop(name, None)
            os.environ.update({
                "OPENAI_PRO_API": "test-pro",
                "OPENAI_PRO_URL": "https://v.3dprint.beer/managed-ai/v1",
                "OPENAI_API_KEY": "test-legacy",
                "OPENAI_BASE_URL": "https://legacy.example/v1",
            })
            base, key, _, _ = preprocessor._config()
        finally:
            for name, value in original.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value

        self.assertEqual(base, "https://legacy.example/v1")
        self.assertEqual(key, "test-legacy")

    def test_legacy_image_provider_is_used_only_when_pro_is_absent(self):
        config = self.image_config(
            OPENAI_API_KEY="test-legacy",
            OPENAI_BASE_URL="https://legacy.example",
        )

        self.assertEqual(config.base_url, "https://legacy.example/v1")
        self.assertEqual(config.api_key, "test-legacy")
        self.assertEqual(config.source, "legacy")

    def test_partial_pro_configuration_fails_without_legacy_fallback(self):
        for environment in (
            {"OPENAI_PRO_API": "test-pro"},
            {"OPENAI_PRO_URL": "https://v.3dprint.beer/managed-ai/v1"},
        ):
            with self.subTest(environment=tuple(environment)):
                with self.assertRaises(preprocessor.OpenAIPreprocessorError) as raised:
                    self.image_config(
                        **environment,
                        OPENAI_API_KEY="test-legacy",
                        OPENAI_BASE_URL="https://legacy.example/v1",
                    )
                self.assertEqual(raised.exception.code, "image_provider_misconfigured")

    def test_missing_image_configuration_is_actionable(self):
        with self.assertRaises(preprocessor.OpenAIPreprocessorError) as raised:
            self.image_config()

        self.assertEqual(raised.exception.code, "image_provider_not_configured")

    def test_pro_request_uses_existing_v1_path_and_never_logs_the_key(self):
        class Response(BytesIO):
            headers = {}
            status = 200

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                self.close()

        names = self.ENVIRONMENT_NAMES
        original = {name: os.environ.get(name) for name in names}
        opener = mock.Mock()
        opener.open.return_value = Response(b"{}")
        events = mock.Mock()
        try:
            for name in names:
                os.environ.pop(name, None)
            os.environ.update({
                "OPENAI_PRO_API": "test-pro-secret",
                "OPENAI_PRO_URL": "https://v.3dprint.beer/managed-ai/v1",
            })
            with mock.patch.object(preprocessor, "build_network_opener", return_value=opener), \
                    mock.patch.object(preprocessor, "diagnostic_event", events):
                preprocessor._image_provider_request("/images/generations", b"{}", "application/json")
        finally:
            for name, value in original.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value

        request = opener.open.call_args.args[0]
        self.assertEqual(request.full_url, "https://v.3dprint.beer/managed-ai/v1/images/generations")
        self.assertEqual(request.get_header("Authorization"), "Bearer test-pro-secret")
        self.assertNotIn("test-pro-secret", repr(events.call_args_list))


class StylePreviewPromptTests(unittest.TestCase):
    def test_portrait_color_refinement_keeps_identity_and_material_boundaries(self):
        for builder in (preprocessor.build_geometry_reference_prompt,
                        preprocessor.build_text_geometry_reference_prompt):
            for style in ("realistic", "portrait_sketch"):
                with self.subTest(builder=builder.__name__, style=style):
                    prompt = builder("两个人，保留本人肤色和唇色", style)
                    self.assertIn("locked facial geometry", prompt)
                    self.assertIn("moles, freckles and deliberate makeup", prompt)
                    self.assertIn("lip edge", prompt)
                    self.assertIn("neutral white-balanced", prompt)
                    self.assertIn("face, ears, neck and visible hands", prompt)
                    self.assertIn("never average their faces", prompt)
                    self.assertIn("no printer color palette or color-count limit", prompt)
                    self.assertIn("rather than making skin one flat swatch", prompt)

    def test_portrait_refinement_does_not_add_skin_recoloring_to_monochrome_sculpture(self):
        prompt = preprocessor.build_geometry_reference_prompt("石膏人像", "sculpture")
        self.assertIn("single-material appearance", prompt)
        self.assertNotIn("natural skin hue consistent", prompt)

    def test_portrait_prompts_preserve_identity_as_shape_not_a_photo_overlay(self):
        for style in ("realistic", "portrait_sketch"):
            prompts = (
                preprocessor.build_geometry_reference_prompt("preserve identity", style),
                preprocessor.build_style_preview_prompt("preserve identity", ("#FFFFFF",), style),
            )
            for prompt in prompts:
                with self.subTest(style=style, prompt=prompt[:80]):
                    self.assertIn("modelable facial planes and relief", prompt)
                    self.assertIn("not a photographic face overlay", prompt)
                    self.assertIn("same coherent sculptural material and lighting", prompt)
                    self.assertIn("not by pasting original photo pixels", prompt)
                    self.assertIn("Do not copy source-background shadows, gray halos", prompt)
                    self.assertNotIn("protected face is allowed and preferred to remain unchanged", prompt)

    def test_geometry_reference_keeps_continuous_tone_without_selected_filament_colors(self):
        prompt = preprocessor.build_geometry_reference_prompt(
            "preserve this person's identity",
            "realistic",
        )

        self.assertIn("continuous tonal modeling", prompt)
        self.assertIn("soft broad diffuse", prompt)
        self.assertIn("geometry reference", prompt)
        self.assertNotIn("selected filament palette is applied later", prompt)
        self.assertIn("Preserve natural colors, continuous gradients", prompt)
        self.assertNotIn("allowed printable palette", prompt)
        self.assertNotIn("Do not use gradients", prompt)

    def test_text_geometry_reference_uses_natural_material_groups_without_palette_language(self):
        prompt = preprocessor.build_text_geometry_reference_prompt(
            "一只正在奔跑的机械麒麟",
            "cartoon",
        )

        self.assertIn("一只正在奔跑的机械麒麟", prompt)
        self.assertIn("continuous tonal modeling", prompt)
        self.assertIn("broad natural material groups", prompt)
        self.assertNotIn("physical filament palette", prompt)
        self.assertNotIn("flat opaque material", prompt)

    def test_short_user_instruction_is_wrapped_with_preview_constraints(self):
        prompt = preprocessor._style_preview_prompt(
            "  make it cartoon style  ",
            ("#FF0000", "#00FF00"),
            "q_cartoon",
        )

        self.assertIn("make it cartoon style", prompt)
        self.assertIn("designer-ready style preview", prompt)
        self.assertIn("recognizable identity", prompt)
        self.assertIn("clean product-shot reference for image-to-3D", prompt)
        self.assertIn("non-human", prompt)
        self.assertIn("base-free when the source is base-free", prompt)
        self.assertIn("mandatory portrait base rule", prompt)
        self.assertIn("always add exactly one low, simple, integrated display base", prompt)
        self.assertIn("only category-specific exception", prompt)
        self.assertIn("flat underside", prompt)
        self.assertIn("shared low base", prompt)
        self.assertIn("smallest integrated material bridge", prompt)
        self.assertIn("finished bust or half-body collectible", prompt)
        self.assertIn("fused to the required compact portrait base", prompt)
        self.assertIn("do not invent a pelvis, legs, or feet", prompt)
        self.assertIn("Changing palette mode", prompt)
        self.assertIn("must not change full-body versus bust framing", prompt)
        self.assertIn("Center the exact requested subject or explicitly requested subject group", prompt)
        self.assertIn("preserve the exact requested count, identities, left-right order", prompt)
        self.assertIn("did not explicitly request a pair, group, set, or exact subject count", prompt)
        self.assertIn("isolate exactly one requested or dominant subject", prompt)
        self.assertIn("Remove scenery, floor shadows, text, logos, watermarks, camera UI", prompt)
        self.assertIn("friendly cute cartoon collectible", prompt)
        self.assertIn("Preserve recognizable identity, age, expression", prompt)
        self.assertIn("do not enlarge eyes excessively", prompt)
        self.assertIn("do not add or remove elements", prompt)
        self.assertIn("do not enlarge the eyes", prompt)
        self.assertIn("do not invent a white muzzle", prompt)
        self.assertIn("Do not return the unchanged source", prompt)
        self.assertIn("#FF0000, #00FF00", prompt)
        self.assertIn("allowed printable palette", prompt)
        self.assertIn("friendly cute cartoon collectible", prompt)
        self.assertIn("full-color designer toy", prompt)
        self.assertIn("Use at least 2 listed colors", prompt)
        self.assertIn("Cover at least 65 percent of the visible subject", prompt)
        self.assertIn("Never retain natural flesh tones", prompt)
        self.assertIn("open jawline", prompt)
        self.assertIn("Never extend hair", prompt)
        self.assertIn("collar must connect the neck to the torso", prompt)
        self.assertIn("outer silhouette, neck, limbs", prompt)
        self.assertIn("visibly joined by opaque palette-colored geometry", prompt)
        self.assertIn("clearly separated from every listed palette color", prompt)
        self.assertIn("identity-defining engraved lines", prompt)
        self.assertIn("stable base material assignment", prompt)
        self.assertIn("restrained neutral studio illumination", prompt)
        self.assertIn("keep it continuous across the face, ears, neck, and visible hands", prompt)
        self.assertIn("not scattered tooth or highlight islands", prompt)
        self.assertIn("will disappear during exact-palette mapping", prompt)
        self.assertIn("Real-person identity geometry rule", prompt)
        self.assertIn("never mirror or invent a second hand", prompt)
        self.assertIn("Never invent a bare elbow, upper arm, or forearm", prompt)
        self.assertIn("shared low integrated base", prompt)
        self.assertIn("paper-thin single sheet", prompt)
        self.assertIn("overlapping solid pin housings", prompt)
        self.assertIn("positive-volume union", prompt)
        self.assertIn("Butt contact, near-touching tips", prompt)
        self.assertIn("bucket or blade merged", prompt)
        self.assertIn("central shaft penetrate and fuse", prompt)
        self.assertIn("source-visible feminine or masculine presentation", prompt)
        self.assertIn("Do not age the person up", prompt)
        self.assertIn("Do not turn the chosen person, animal, statue, building, or object into a different subject", prompt)
        self.assertNotIn("Preserve the exact canvas, aspect ratio, crop, framing", prompt)
        self.assertNotIn("one fused connected object", prompt)

    def test_each_style_has_distinct_printable_direction(self):
        realistic = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "realistic")
        cartoon = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "cartoon")
        sculpture = preprocessor._style_preview_prompt("preserve pose", (), "sculpture")

        self.assertIn("changing as little as possible", realistic)
        self.assertIn("polychrome portrait sculpture or faithful 3D scan", realistic)
        self.assertIn("IDENTITY-FIRST PORTRAIT LOCK", realistic)
        self.assertIn("compare the source and result face at equal size", realistic)
        self.assertNotIn("IDENTITY-FIRST PORTRAIT LOCK", cartoon)
        self.assertIn("friendly cute cartoon collectible", cartoon)
        self.assertIn("one monochrome museum-quality", sculpture)
        for prompt in (realistic, cartoon, sculpture):
            self.assertIn("Preserve the chosen subject's recognizable identity", prompt)
            self.assertIn("Do not turn the chosen", prompt)
            self.assertIn("closed visual inventory", prompt)
            self.assertIn("exact viewpoint", prompt)
            self.assertIn("eyewear, headwear, visible hands", prompt)
            self.assertIn("wheels, handles, openings, windows, lenses, dials, buttons", prompt)
            self.assertIn("preserve tier and opening counts", prompt)
            self.assertIn("thicken it subtly instead of deleting or duplicating it", prompt)
            self.assertIn("branching organic subject", prompt)
            self.assertIn("isolated leaf pads", prompt)
        self.assertIn("allowed printable palette", realistic)
        self.assertIn("museum-grade polychrome portrait maquette", realistic)
        self.assertIn("must not look like a designer toy", realistic)
        self.assertIn("mild beautification may clean skin and hair texture", realistic)
        self.assertNotIn("premium full-color designer toy", realistic)
        self.assertNotIn("allowed printable palette", sculpture)
        self.assertIn("monochrome means one material, not fewer components", sculpture)
        self.assertIn("rather than changing identity", cartoon)

    def test_relief_and_diorama_support_rules_override_generic_base_rules(self):
        relief = preprocessor._style_preview_prompt("preserve machine", ("#FFFFFF", "#000000"), "relief")
        diorama = preprocessor._style_preview_prompt("preserve product", ("#FFFFFF", "#000000"), "diorama")
        cartoon = preprocessor._style_preview_prompt("preserve product", ("#FFFFFF", "#000000"), "cartoon")

        self.assertIn("RELIEF SUPPORT OVERRIDE", relief)
        self.assertIn("backing plaque is mandatory", relief)
        self.assertIn("overrides both the portrait display-base rule and the non-human base-free rule", relief)
        self.assertNotIn("must remain base-free when the source is base-free", relief)
        self.assertIn("DIORAMA SUPPORT OVERRIDE", diorama)
        self.assertIn("shared low terrain or floor base", diorama)
        self.assertIn("without inventing rocks, plants, furniture, buildings", diorama)
        self.assertNotIn("must remain base-free when the source is base-free", diorama)
        self.assertIn("must remain base-free when the source is base-free", cartoon)

    def test_text_generation_receives_the_same_style_support_overrides(self):
        relief = preprocessor._text_image_prompt("one reading corner", ("#FFFFFF", "#000000"), "relief")
        diorama = preprocessor._text_image_prompt("one reading corner", ("#FFFFFF", "#000000"), "diorama")

        self.assertIn("RELIEF SUPPORT OVERRIDE", relief)
        self.assertIn("never return a free-standing figurine", relief)
        self.assertIn("DIORAMA SUPPORT OVERRIDE", diorama)
        self.assertIn("Fuse every requested subject and prop to that one base", diorama)

    def test_non_realistic_styles_promote_text_cleanup_without_changing_realistic_contract(self):
        cartoon = preprocessor._style_preview_prompt("preserve labeled drill", ("#FFFFFF", "#000000"), "cartoon")
        relief = preprocessor._style_preview_prompt("preserve labeled robot", ("#FFFFFF", "#000000"), "relief")
        realistic = preprocessor._style_preview_prompt("preserve labeled product", ("#FFFFFF", "#000000"), "realistic")

        for prompt in (cartoon, relief):
            self.assertIn("NON-REALISTIC TEXT CLEANUP", prompt)
            self.assertIn("blank recessed panel", prompt)
            self.assertIn("do not invent plausible substitute spelling", prompt)
        self.assertNotIn("NON-REALISTIC TEXT CLEANUP", realistic)
        self.assertNotIn("RELIEF SUPPORT OVERRIDE", realistic)
        self.assertIn("must remain base-free when the source is base-free", realistic)

    def test_legacy_and_print_specific_style_profiles_resolve(self):
        self.assertEqual(
            preprocessor._style_profile("q_cartoon"),
            preprocessor._style_profile("cartoon"),
        )
        self.assertIn("broad, clean planar facets", preprocessor._style_profile("low_poly"))
        self.assertIn("shallow bas-relief", preprocessor._style_profile("relief"))
        self.assertIn("miniature diorama", preprocessor._style_profile("diorama"))

    def test_print_native_artistic_profiles_use_modelable_abstraction(self):
        portrait = preprocessor._style_preview_prompt(
            "preserve this person", ("#F0C7A6", "#1A1A1A", "#F4F1E8", "#315B48", "#A64B3C", "#6A5B8E"),
            "portrait_sketch",
        )
        ink = preprocessor._style_preview_prompt(
            "preserve this landscape", ("#111111", "#F4F1E8", "#69736A", "#9A3E34", "#315B48", "#6A5B8E"),
            "ink_relief",
        )

        self.assertIn("identity-first portrait sketch sculpture", portrait)
        self.assertIn("restrained exaggeration", portrait)
        self.assertIn("expressive material or relief forms", portrait)
        self.assertIn("generic caricature", portrait)
        self.assertIn("do not force every selected color", portrait)
        self.assertIn("IDENTITY-FIRST PORTRAIT LOCK", portrait)
        self.assertIn("acceptable to use a meaningful subset", portrait)
        self.assertNotIn("Use at least 3 listed colors", portrait)
        self.assertIn("printmaking- and woodcut-inspired ink relief", ink)
        self.assertIn("negative space", ink)
        self.assertIn("two to four broad value masses", ink)
        self.assertIn("embossed or engraved strokes", ink)
        self.assertIn("translucent washes", ink)
        self.assertIn("micro-halftone dots", ink)
        self.assertIn("do not force every selected color", ink)
        self.assertIn("RELIEF SUPPORT OVERRIDE", ink)
        self.assertIn("acceptable to use a meaningful subset", ink)
        self.assertNotIn("Use at least 3 listed colors", ink)

    def test_print_native_artistic_material_prompts_accept_one_through_six_colors(self):
        colors = ("#111111", "#F4F1E8", "#69736A", "#9A3E34", "#315B48", "#6A5B8E")
        for style in ("portrait_sketch", "ink_relief"):
            for count in range(1, 7):
                with self.subTest(style=style, count=count):
                    prompt = preprocessor._text_image_prompt("preserve the subject", colors[:count], style)
                    self.assertIn("acceptable to use a meaningful subset", prompt)
                    self.assertNotIn(f"Use at least {min(3, count)} listed colors", prompt)
                    for color in colors[:count]:
                        self.assertIn(color, prompt)

    def test_artistic_geometry_prompts_keep_continuous_tone_and_defer_exact_colors(self):
        for style in ("portrait_sketch", "ink_relief"):
            with self.subTest(style=style):
                prompt = preprocessor.build_geometry_reference_prompt("preserve the subject", style)
                self.assertIn("continuous tonal modeling", prompt)
                self.assertNotIn("selected filament palette is applied later", prompt)
                self.assertIn("Preserve natural colors, continuous gradients", prompt)
                self.assertNotIn("#F0C7A6", prompt)
                self.assertNotIn("Use at least 3 listed colors", prompt)

    def test_natural_color_mode_omits_printable_palette_constraint(self):
        prompt = preprocessor._style_preview_prompt("preserve pose", (), "q_cartoon")

        self.assertIn("coherent natural colors", prompt)
        self.assertNotIn("allowed printable palette", prompt)
        self.assertNotIn("deterministic print-mapping", prompt)
        self.assertNotIn("full-color designer toy", prompt)

    def test_unknown_style_is_rejected(self):
        with self.assertRaises(preprocessor.OpenAIPreprocessorError):
            preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF",), "unknown")

    def test_custom_style_is_wrapped_without_overriding_print_constraints(self):
        prompt = preprocessor._style_preview_prompt(
            "preserve pose",
            ("#FF0000", "#FFFFFF"),
            "custom",
            custom_style="复古木刻版画，粗轮廓和大块阴影",
        )

        self.assertIn("复古木刻版画，粗轮廓和大块阴影", prompt)
        self.assertIn("appearance and shape-language direction", prompt)
        self.assertIn("hard constraints in this request take priority", prompt)
        self.assertIn("allowed printable palette", prompt)
        self.assertIn("always add exactly one low, simple, integrated display base", prompt)
        self.assertIn("Do not apply this portrait exception to an animal, product, vehicle", prompt)

    def test_custom_style_requires_nonempty_bounded_description(self):
        with self.assertRaisesRegex(preprocessor.OpenAIPreprocessorError, "required"):
            preprocessor._text_image_prompt("a toy", (), "custom", custom_style="  ")
        with self.assertRaisesRegex(preprocessor.OpenAIPreprocessorError, "1000-byte"):
            preprocessor._text_image_prompt("a toy", (), "custom", custom_style="a" * 1001)


class TextImagePromptTests(unittest.TestCase):
    def test_palette_direction_describes_extended_roles_and_degrades_for_one_color(self):
        six = preprocessor._designer_toy_palette_direction(
            ("#D93632", "#252525", "#F2F1EA", "#315CA8", "#3B8C54", "#9B3F77")
        )
        self.assertIn("secondary=", six)
        self.assertIn("detail=", six)
        self.assertIn("secondary color covers another broad semantic part", six)
        self.assertIn("detail color marks a bounded, printable identifying feature", six)

        one = preprocessor._designer_toy_palette_direction(("#D93632",))
        self.assertIn("primary material", one)
        self.assertIn("primary color as one flat opaque material", one)
        self.assertNotIn("structure color as one flat opaque material", one)

    def test_printable_text_image_prompt_uses_large_regions_and_rejects_dithering(self):
        prompt = preprocessor._text_image_prompt(
            "一只正在奔跑的机械麒麟",
            ("#D93632", "#3B8C54", "#315CA8", "#F2F1EA"),
            "q_cartoon",
        )

        self.assertIn("一只正在奔跑的机械麒麟", prompt)
        self.assertIn("#D93632, #3B8C54, #315CA8, #F2F1EA", prompt)
        self.assertIn("natural color transitions", prompt)
        self.assertIn("Do not use gradients", prompt)
        self.assertIn("dithering", prompt)
        self.assertIn("deterministic print pipeline", prompt)
        self.assertIn("structure=#315CA8", prompt)
        self.assertIn("primary=#D93632", prompt)
        self.assertIn("full-color designer toy", prompt)
        self.assertIn("Use at least 3 listed colors", prompt)
        self.assertIn("transparent background", prompt)
        self.assertIn("load-bearing connections, base contact", prompt)
        self.assertIn("always add exactly one low, simple, integrated display base", prompt)
        self.assertIn("head-and-shoulders, chest, waist, or other cropped human portrait", prompt)
        self.assertIn("Do not apply this portrait exception to an animal, product, vehicle", prompt)
        self.assertIn("unless that exact shade is one of the listed printable colors", prompt)
        self.assertIn("closed component inventory", prompt)
        self.assertIn("never as lighting highlights", prompt)
        self.assertIn("thin load-bearing shaft, rib, spoke", prompt)
        self.assertIn("structure color as one flat opaque material from end to end", prompt)
        self.assertIn("stable base material assignment", prompt)
        self.assertIn("restrained neutral studio illumination", prompt)
        self.assertIn("never break skin", prompt)
        self.assertIn("contact-only branch shell", prompt)
        self.assertIn("Real-person identity geometry rule", prompt)
        self.assertIn("Never invent a bare elbow, upper arm, or forearm", prompt)
        self.assertIn("shared low integrated base", prompt)
        self.assertIn("paper-thin single sheet", prompt)

    def test_text_image_prompt_omits_palette_language_in_natural_mode(self):
        prompt = preprocessor._text_image_prompt("a toy dragon", (), "low_poly")
        self.assertIn("coherent natural colors", prompt)
        self.assertNotIn("physical filament palette", prompt)

    def test_public_text_prompt_matches_generation_contract(self):
        palette = ("#C95B43", "#253B5E", "#F2E5C4", "#D6A72C")
        self.assertEqual(
            preprocessor.build_text_image_prompt("two coworkers", palette, "cartoon"),
            preprocessor.build_text_geometry_reference_prompt("two coworkers", "cartoon"),
        )


class VisionCompletionTests(unittest.TestCase):
    def test_sends_text_and_compressed_review_image_to_compatible_chat_endpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "sheet.png"
            Image.new("RGB", (8, 8), "red").save(image_path)
            captured = {}

            def provider(path, body, content_type):
                captured.update(path=path, payload=json.loads(body), content_type=content_type)
                return {"choices": [{"message": {"content": '{"score":88}'}}]}

            with configured_base_url("https://laotie.dev"), mock.patch.object(preprocessor, "_provider_request", side_effect=provider):
                response = preprocessor.complete_vision("review system", "review this model", (image_path,))

        self.assertEqual(response, '{"score":88}')
        self.assertEqual(captured["path"], "/chat/completions")
        parts = captured["payload"]["messages"][1]["content"]
        self.assertEqual(parts[0], {"type": "text", "text": "review this model"})
        self.assertTrue(parts[1]["image_url"]["url"].startswith("data:image/jpeg;base64,"))

    def test_review_upload_is_bounded_and_keeps_source_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "large.png"
            Image.new("RGBA", (3072, 1536), (0, 0, 0, 0)).save(path)
            before = path.read_bytes()
            mime, data = preprocessor._vision_image_data(path)
            self.assertEqual(path.read_bytes(), before)
            self.assertEqual(mime, "image/jpeg")
            with Image.open(BytesIO(data)) as encoded:
                self.assertEqual(encoded.size, (2048, 1024))
                self.assertEqual(encoded.getpixel((0, 0)), (255, 255, 255))

    def test_analysis_retries_a_transient_gateway_or_tls_failure_once(self):
        for code in ("image_connection_failed", "image_service_unavailable"):
            failure = preprocessor.OpenAIPreprocessorError("temporary", code=code, retryable=True, ambiguous=True)
            with (
                self.subTest(code=code), configured_base_url("https://laotie.dev"),
                mock.patch.object(preprocessor, "_request_with_provider", side_effect=[failure, {"ok": True}]) as request,
                mock.patch.object(preprocessor.time, "sleep") as sleep,
            ):
                self.assertEqual(preprocessor._provider_request("/chat/completions", b"{}", "application/json"), {"ok": True})
                self.assertEqual(request.call_count, 2)
                self.assertEqual(request.call_args_list[0], request.call_args_list[1])
                sleep.assert_called_once_with(1)

    def test_analysis_retries_are_bounded_and_do_not_retry_auth_or_rate_limits(self):
        for code, calls in (("image_connection_failed", 2), ("image_auth_failed", 1), ("image_rate_limited", 1)):
            with (
                self.subTest(code=code), configured_base_url("https://laotie.dev"),
                mock.patch.object(preprocessor, "_request_with_provider", side_effect=preprocessor.OpenAIPreprocessorError("failed", code=code)) as request,
                mock.patch.object(preprocessor.time, "sleep"),
                self.assertRaises(preprocessor.OpenAIPreprocessorError),
            ):
                preprocessor._provider_request("/chat/completions", b"{}", "application/json")
            self.assertEqual(request.call_count, calls)

    def test_truncated_image_post_response_is_ambiguous_without_resubmission(self):
        opener = mock.Mock()
        opener.open.side_effect = http.client.IncompleteRead(b"partial")
        with (
            configured_base_url("https://laotie.dev"),
            mock.patch.object(preprocessor, "build_network_opener", return_value=opener),
            self.assertRaises(preprocessor.OpenAIPreprocessorError) as raised,
        ):
            preprocessor._image_provider_request("/images/edits", b"body", "multipart/form-data")
        self.assertEqual(raised.exception.code, "image_connection_failed")
        self.assertTrue(raised.exception.ambiguous)
        opener.open.assert_called_once()

    def test_rejects_more_than_two_review_images(self):
        path = Path("unused.png")
        with self.assertRaises(preprocessor.OpenAIPreprocessorError):
            preprocessor.complete_vision("system", "user", (path, path, path))


class PrintablePaletteRecommendationTests(unittest.TestCase):
    def recommendation_json(self, **overrides):
        value = {
            "summary": "温暖主体配合深色结构和冷色点缀",
            "colors": [
                {"hex": "#D96B43", "name": "陶土橙", "role": "primary", "usage": "主体服装", "reason": "形成视觉中心"},
                {"hex": "#2B2422", "name": "深棕", "role": "structure", "usage": "头发与轮廓", "reason": "稳定结构边界"},
                {"hex": "#F2D7B5", "name": "暖白", "role": "light", "usage": "面部与高光", "reason": "保持明暗层次"},
                {"hex": "#2F6B5F", "name": "墨绿", "role": "accent", "usage": "配件与底座", "reason": "增加冷暖对比"},
            ],
        }
        value.update(overrides)
        return json.dumps(value, ensure_ascii=False)

    def test_text_recommendation_accepts_fenced_json_and_preserves_roles(self):
        response = "```json\n" + self.recommendation_json() + "\n```"
        with mock.patch.object(preprocessor, "complete_text", return_value=response) as complete:
            result = preprocessor.recommend_printable_palette("一只机械麒麟", "q_cartoon")

        self.assertEqual(result.summary, "温暖主体配合深色结构和冷色点缀")
        self.assertEqual([color.hex for color in result.colors], ["#D96B43", "#2B2422", "#F2D7B5", "#2F6B5F"])
        self.assertEqual([color.role for color in result.colors], ["primary", "structure", "light", "accent"])
        self.assertIn("four-color palette", complete.call_args.args[0])
        self.assertIn("different hue family from primary", complete.call_args.args[0])
        self.assertIn("overrides any monochrome", complete.call_args.args[0])
        self.assertIn("structure visibly dark", complete.call_args.args[0])
        self.assertIn("reserve light for the continuous skin material", complete.call_args.args[0])
        self.assertIn("Never assign skin to primary", complete.call_args.args[0])

    def test_image_recommendation_uses_vision_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "reference.png"
            Image.new("RGB", (8, 8), "red").save(image_path)
            with mock.patch.object(preprocessor, "complete_vision", return_value=self.recommendation_json()) as complete:
                result = preprocessor.recommend_printable_palette(
                    "保留角色身份", "cel_shaded", image_path=image_path
                )

        self.assertEqual(len(result.colors), 4)
        self.assertEqual(complete.call_args.args[2], (image_path,))

    def test_recommendation_supports_requested_cardinalities_one_through_six(self):
        value = json.loads(self.recommendation_json())
        value["colors"].extend([
            {"hex": "#3267A8", "name": "钴蓝", "role": "secondary", "usage": "次要大色区", "reason": "补充分区"},
            {"hex": "#9B3F77", "name": "莓紫", "role": "detail", "usage": "可打印细节", "reason": "强化识别"},
        ])
        roles = ("primary", "structure", "light", "accent", "secondary", "detail")
        for count in range(1, 7):
            payload = {**value, "colors": value["colors"][:count]}
            with self.subTest(count=count), mock.patch.object(
                preprocessor, "complete_text", return_value=json.dumps(payload, ensure_ascii=False)
            ) as complete:
                result = preprocessor.recommend_printable_palette("一只机械麒麟", "q_cartoon", color_count=count)
                self.assertEqual(len(result.colors), count)
                self.assertEqual(tuple(color.role for color in result.colors), roles[:count])
                self.assertIn(f"Return exactly {count} color records", complete.call_args.args[0])

    def test_recommendation_rejects_unsupported_requested_cardinality_before_provider_call(self):
        with mock.patch.object(preprocessor, "complete_text") as complete:
            for count in (0, 7, True):
                with self.subTest(count=count), self.assertRaises(preprocessor.OpenAIPreprocessorError):
                    preprocessor.recommend_printable_palette("a toy", "low_poly", color_count=count)
        complete.assert_not_called()

    def test_rejects_invalid_provider_responses(self):
        valid = json.loads(self.recommendation_json())
        cases = {
            "malformed": "not json",
            "duplicate_color": json.dumps({**valid, "colors": [*valid["colors"][:3], {**valid["colors"][3], "hex": "#D96B43"}]}),
            "invalid_hex": json.dumps({**valid, "colors": [{**valid["colors"][0], "hex": "orange"}, *valid["colors"][1:]]}),
            "duplicate_role": json.dumps({**valid, "colors": [*valid["colors"][:3], {**valid["colors"][3], "role": "primary"}]}),
            "missing_role": json.dumps({**valid, "colors": valid["colors"][:3]}),
            "long_reason": json.dumps({**valid, "colors": [{**valid["colors"][0], "reason": "x" * 401}, *valid["colors"][1:]]}),
        }
        for name, response in cases.items():
            with self.subTest(name=name), mock.patch.object(preprocessor, "complete_text", return_value=response):
                with self.assertRaises(preprocessor.OpenAIPreprocessorError):
                    preprocessor.recommend_printable_palette("a toy", "low_poly")

    def test_rejects_low_contrast_recommendation_without_retry(self):
        value = json.loads(self.recommendation_json())
        for color, hex_value in zip(value["colors"], ("#777777", "#787878", "#797979", "#7A7A7A")):
            color["hex"] = hex_value
        with mock.patch.object(preprocessor, "complete_text", return_value=json.dumps(value)) as complete:
            with self.assertRaisesRegex(preprocessor.OpenAIPreprocessorError, "contrast"):
                preprocessor.recommend_printable_palette("a toy", "low_poly")
        complete.assert_called_once()


class ExactImageEditTests(unittest.TestCase):
    def test_new_design_generation_requests_solid_background_for_each_product_mode(self):
        for style in ("sculpture", "realistic", "cartoon"):
            with self.subTest(style=style), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "design.png"
                with (
                    mock.patch.object(preprocessor, "_image_config", return_value=mock.Mock(model="offline")),
                    mock.patch.object(preprocessor, "_image_quality", return_value="high"),
                    mock.patch.object(preprocessor, "_image_provider_request", return_value={}) as request,
                    mock.patch.object(preprocessor, "_save_provider_image", return_value=output),
                ):
                    preprocessor.generate_geometry_reference_image("white jacket on a checkerboard", output, style)
                request.assert_called_once()
                payload = json.loads(request.call_args.args[1])
                self.assertEqual(payload["background"], "opaque")
                self.assertEqual(payload["n"], 1)
                self.assertIn("uniform opaque solid-color", payload["prompt"])
                self.assertIn("Do not request transparency or draw a transparency checkerboard", payload["prompt"])
                self.assertIn("never recolor the subject", payload["prompt"])
                self.assertIn("Replace any such backdrop in the source image", payload["prompt"])

    def test_new_design_edit_requests_solid_background_without_rewriting_source_or_provider_pixels(self):
        for style in ("sculpture", "realistic", "cartoon"):
            with self.subTest(style=style), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source, output = root / "source.png", root / "design.png"
                image = Image.new("RGB", (32, 32), (240, 240, 240))
                image.paste((210, 210, 210), (0, 0, 16, 16))
                image.paste((210, 210, 210), (16, 16, 32, 32))
                image.save(source)
                original = source.read_bytes()
                generated = BytesIO()
                Image.new("RGB", (32, 32), (110, 110, 110)).save(generated, format="PNG")
                provider_pixels = generated.getvalue()

                def edit(_source, _prompt, destination, **_kwargs):
                    Path(destination).write_bytes(provider_pixels)
                    return Path(destination)

                with (
                    mock.patch.object(preprocessor, "edit_image", side_effect=edit) as image_edit,
                    mock.patch.object(preprocessor, "_portrait_face_lock_mask", return_value=None),
                ):
                    preprocessor.preprocess_image(source, "retain the white jacket", output, (), style)
                image_edit.assert_called_once()
                self.assertEqual(image_edit.call_args.args[0], source)
                self.assertEqual(image_edit.call_args.kwargs, {"background": "opaque"})
                self.assertIn("uniform opaque solid-color", image_edit.call_args.args[1])
                self.assertIn("Do not request transparency or draw a transparency checkerboard", image_edit.call_args.args[1])
                self.assertEqual(source.read_bytes(), original)
                self.assertEqual(output.read_bytes(), provider_pixels)

    def test_exact_edit_creates_destination_directory_before_saving(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.png"
            destination = root / "nested" / "result.png"
            Image.new("RGB", (8, 8), "red").save(source)

            def provider(_path, _body, _content_type):
                encoded = __import__("base64").b64encode(source.read_bytes()).decode("ascii")
                return {"data": [{"b64_json": encoded}]}

            with configured_base_url("https://laotie.dev"), mock.patch.object(
                preprocessor, "_image_provider_request", side_effect=provider
            ):
                preprocessor.edit_image(source, "UPSCALE ONLY", destination)

            self.assertTrue(destination.is_file())

    def test_exact_prompt_is_sent_without_style_wrapper(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.png"
            destination = Path(directory) / "result.png"
            Image.new("RGB", (8, 8), "red").save(source)
            captured = {}

            def provider(path, body, content_type):
                captured.update(path=path, body=body, content_type=content_type)
                encoded = __import__("base64").b64encode(source.read_bytes()).decode("ascii")
                return {"data": [{"b64_json": encoded}]}

            with configured_base_url("https://laotie.dev"), mock.patch.object(preprocessor, "_image_provider_request", side_effect=provider):
                preprocessor.edit_image(source, "EXACT MULTIVIEW PROMPT", destination)

            self.assertTrue(destination.is_file())

        self.assertEqual(captured["path"], "/images/edits")
        self.assertIn(b"EXACT MULTIVIEW PROMPT", captured["body"])
        self.assertIn(b'name="quality"\r\n\r\nhigh', captured["body"])
        self.assertIn(b'name="input_fidelity"\r\n\r\nhigh', captured["body"])
        self.assertIn(b'name="size"\r\n\r\nauto', captured["body"])
        self.assertNotIn(b'name="background"', captured["body"])
        self.assertNotIn(b"designer-ready style preview", captured["body"])

    def test_exact_edit_can_request_real_transparent_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.png"
            destination = Path(directory) / "result.png"
            Image.new("RGB", (8, 8), "red").save(source)
            captured = {}

            def provider(path, body, content_type):
                captured.update(path=path, body=body, content_type=content_type)
                encoded = __import__("base64").b64encode(source.read_bytes()).decode("ascii")
                return {"data": [{"b64_json": encoded}]}

            with configured_base_url("https://laotie.dev"), mock.patch.object(
                preprocessor, "_image_provider_request", side_effect=provider
            ):
                preprocessor.edit_image(
                    source,
                    "TURN TABLE",
                    destination,
                    background="transparent",
                )

        self.assertIn(b'name="background"\r\n\r\ntransparent', captured["body"])

    def test_transparent_edit_rejection_never_issues_a_second_billable_request(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.png"
            destination = Path(directory) / "result.png"
            Image.new("RGB", (32, 32), (120, 130, 140)).save(source)
            bodies = []

            def request(_path, body, _content_type):
                bodies.append(body)
                raise preprocessor.OpenAIPreprocessorError(
                    "rejected", code="image_rejected", retryable=False
                )

            with configured_base_url("https://laotie.dev"), mock.patch.object(
                preprocessor, "_image_provider_request", side_effect=request
            ), self.assertRaises(preprocessor.OpenAIPreprocessorError):
                preprocessor.edit_image(source, "KEEP SUBJECT", destination, background="transparent")

        self.assertEqual(len(bodies), 1)
        self.assertIn(b'name="background"\r\n\r\ntransparent', bodies[0])

    def test_portrait_preview_never_pastes_source_pixels_into_provider_result(self):
        from PIL import ImageDraw

        palette = ("#FFFFFF", "#111111", "#F0C8AA", "#315B48", "#888888", "#776655")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.png"
            source_image = Image.new("RGB", (240, 360), (185, 190, 195))
            draw = ImageDraw.Draw(source_image)
            draw.ellipse((80, 45, 160, 150), fill=(218, 164, 124))
            draw.rectangle((45, 150, 195, 350), fill=(242, 240, 235))
            source_image.save(source)
            original_bytes = source.read_bytes()

            for style in ("realistic", "portrait_sketch"):
                for color_count in range(1, 7):
                    for mode in ("RGB", "RGBA"):
                        for copy_geometry in (False, True):
                            with self.subTest(style=style, colors=color_count, mode=mode, geometry=copy_geometry):
                                case_root = root / f"{style}-{color_count}-{mode}-{copy_geometry}"
                                case_root.mkdir()
                                destination = case_root / "result.png"
                                geometry = case_root / "geometry.png" if copy_geometry else None
                                # Different framing and a displaced head: a source-space
                                # ellipse must never be used to paste a photographic face.
                                background = (245, 245, 245, 0) if mode == "RGBA" else "white"
                                provider_image = Image.new(mode, (256, 256), background)
                                ImageDraw.Draw(provider_image).ellipse((140, 40, 220, 145), fill=(70, 110, 150))
                                encoded = BytesIO()
                                provider_image.save(encoded, format="PNG")
                                provider_bytes = encoded.getvalue()

                                def edit(_source, _prompt, output, **_kwargs):
                                    Path(output).write_bytes(provider_bytes)
                                    return Path(output)

                                with mock.patch.object(preprocessor, "edit_image", side_effect=edit) as image_edit:
                                    result = preprocessor.preprocess_image(
                                        source, "preserve this person", destination,
                                        palette[:color_count], style, geometry_output_path=geometry,
                                    )

                                self.assertEqual(source.read_bytes(), original_bytes)
                                self.assertEqual(result, destination)
                                image_edit.assert_called_once()
                                self.assertEqual(image_edit.call_args.args[0], source)
                                self.assertEqual(image_edit.call_args.kwargs, {"background": "opaque"})
                                self.assertTrue((case_root / preprocessor.PORTRAIT_FACE_LOCK_FILENAME).is_file())
                                self.assertEqual(destination.read_bytes(), provider_bytes)
                                if geometry is not None:
                                    self.assertEqual(geometry.read_bytes(), provider_bytes)

    def test_style_preview_keeps_realistic_identity_direction_and_detection_evidence(self):
        from PIL import ImageDraw

        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.png"
            destination = Path(directory) / "result.png"
            image = Image.new("RGB", (240, 360), (185, 190, 195))
            draw = ImageDraw.Draw(image)
            draw.ellipse((80, 45, 160, 150), fill=(218, 164, 124))
            draw.rectangle((45, 150, 195, 350), fill=(242, 240, 235))
            image.save(source)
            with mock.patch.object(preprocessor, "edit_image", return_value=destination) as edit:
                result = preprocessor.preprocess_image(
                    source,
                    "preserve this person",
                    destination,
                    ("#FFFFFF", "#111111", "#F0C8AA", "#315B48"),
                    "realistic",
                )
            self.assertTrue((Path(directory) / preprocessor.PORTRAIT_FACE_LOCK_FILENAME).is_file())

        self.assertEqual(result, destination)
        self.assertEqual(edit.call_args.kwargs.get("background"), "opaque")
        self.assertIn("IDENTITY-FIRST PORTRAIT LOCK", edit.call_args.args[1])
        self.assertIn("continuous tonal modeling", edit.call_args.args[1])
        for color in ("#FFFFFF", "#111111", "#F0C8AA", "#315B48"):
            self.assertNotIn(color, edit.call_args.args[1])
        self.assertIn("There is no printer color palette or color-count limit", edit.call_args.args[1])

    def test_portrait_geometry_copy_without_detected_face_uses_only_the_provider_result(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.png"
            destination = root / "result.png"
            geometry = root / "geometry.png"
            Image.new("RGB", (64, 96), (185, 190, 195)).save(source)

            def edit(_source, _prompt, output, **_kwargs):
                Image.new("RGB", (64, 96), (35, 95, 145)).save(output)
                return Path(output)

            with mock.patch.object(preprocessor, "edit_image", side_effect=edit) as image_edit:
                preprocessor.preprocess_image(
                    source,
                    "preserve this person",
                    destination,
                    ("#FFFFFF", "#111111", "#F0C8AA", "#315B48"),
                    "realistic",
                    geometry_output_path=geometry,
                )

            with Image.open(destination) as image:
                self.assertEqual(image.getpixel((10, 10)), (35, 95, 145))
            self.assertEqual(geometry.read_bytes(), destination.read_bytes())
            self.assertFalse((root / preprocessor.PORTRAIT_FACE_LOCK_FILENAME).exists())
            image_edit.assert_called_once()
            self.assertEqual(image_edit.call_args.kwargs, {"background": "opaque"})

    def test_portrait_face_lock_mask_is_opaque_on_face_and_transparent_outside(self):
        from PIL import ImageDraw

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.png"
            image = Image.new("RGB", (240, 360), (185, 190, 195))
            draw = ImageDraw.Draw(image)
            draw.ellipse((80, 45, 160, 150), fill=(218, 164, 124))
            draw.rectangle((105, 140, 135, 205), fill=(210, 154, 116))
            draw.rectangle((45, 150, 195, 350), fill=(242, 240, 235))
            image.save(source)

            mask = preprocessor._portrait_face_lock_mask(source, root / "mask.png")

            self.assertIsNotNone(mask)
            with Image.open(mask) as opened:
                alpha = opened.getchannel("A")
                self.assertGreater(alpha.getpixel((120, 95)), 240)
                self.assertLess(alpha.getpixel((20, 300)), 10)


class ImageDownloadTests(unittest.TestCase):
    class Response(BytesIO):
        headers = {}

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            self.close()

    def test_transient_download_failure_retries_only_the_artifact_get(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "result.png"
            opener = mock.Mock()
            opener.open.side_effect = [urllib.error.URLError("temporary"), self.Response(b"png-bytes")]
            with (
                mock.patch.object(preprocessor, "_validate_artifact_url"),
                mock.patch.object(preprocessor, "build_network_opener", return_value=opener),
            ):
                result = preprocessor._download_image("https://cdn.example/result.png", destination)

            self.assertEqual(result, destination)
            self.assertEqual(destination.read_bytes(), b"png-bytes")
        self.assertEqual(opener.open.call_count, 2)

    def test_download_failure_is_actionable_after_one_safe_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "result.png"
            opener = mock.Mock()
            opener.open.side_effect = urllib.error.URLError("offline")
            with (
                mock.patch.object(preprocessor, "_validate_artifact_url"),
                mock.patch.object(preprocessor, "build_network_opener", return_value=opener),
                self.assertRaises(preprocessor.OpenAIPreprocessorError) as raised,
            ):
                preprocessor._download_image("https://cdn.example/result.png", destination)

        self.assertEqual(raised.exception.code, "image_download_failed")
        self.assertTrue(raised.exception.retryable)
        self.assertFalse(raised.exception.ambiguous)
        self.assertEqual(opener.open.call_count, 2)

    def test_download_does_not_retry_a_permanent_client_error(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "result.png"
            opener = mock.Mock()
            opener.open.side_effect = urllib.error.HTTPError(
                "https://cdn.example/result.png", 404, "missing", None, None
            )
            with (
                mock.patch.object(preprocessor, "_validate_artifact_url"),
                mock.patch.object(preprocessor, "build_network_opener", return_value=opener),
                self.assertRaises(preprocessor.OpenAIPreprocessorError),
            ):
                preprocessor._download_image("https://cdn.example/result.png", destination)

        opener.open.assert_called_once()

    def test_truncated_download_preserves_previous_file_and_retries_get(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "result.png"
            destination.write_bytes(b"previous")
            truncated = self.Response(b"partial")
            truncated.headers = {"Content-Length": "100"}
            opener = mock.Mock()
            opener.open.side_effect = [truncated, self.Response(b"complete")]
            with (
                mock.patch.object(preprocessor, "_validate_artifact_url"),
                mock.patch.object(preprocessor, "build_network_opener", return_value=opener),
            ):
                preprocessor._download_image("https://cdn.example/result.png", destination)
            self.assertEqual(destination.read_bytes(), b"complete")
            self.assertFalse(destination.with_name("result.png.part").exists())
            self.assertEqual(opener.open.call_count, 2)

    def test_empty_download_does_not_replace_an_existing_preview(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "result.png"
            destination.write_bytes(b"previous")
            opener = mock.Mock()
            opener.open.side_effect = [self.Response(b""), self.Response(b"")]
            with (
                mock.patch.object(preprocessor, "_validate_artifact_url"),
                mock.patch.object(preprocessor, "build_network_opener", return_value=opener),
                self.assertRaises(preprocessor.OpenAIPreprocessorError) as raised,
            ):
                preprocessor._download_image("https://cdn.example/result.png", destination)
            self.assertEqual(raised.exception.code, "image_download_failed")
            self.assertEqual(destination.read_bytes(), b"previous")


if __name__ == "__main__":
    unittest.main()

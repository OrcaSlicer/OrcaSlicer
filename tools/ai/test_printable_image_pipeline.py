#!/usr/bin/env python3
import json
from pathlib import Path
import tempfile
import unittest

from PIL import Image

from tools.ai import printable_image_pipeline as pipeline


RGBW = ("#D93632", "#3B8C54", "#315CA8", "#F2F1EA")


class PrintSettingsTests(unittest.TestCase):
    def test_defaults_follow_two_line_minimum(self):
        settings = pipeline.PrintSettings.from_mapping(None)
        self.assertEqual(settings.minimum_feature_mm, 0.8)
        self.assertEqual(settings.line_width_mm, 0.4)
        self.assertEqual(settings.print_mode, "solid_regions")

    def test_rejects_non_solid_mode_and_too_small_feature(self):
        with self.assertRaises(pipeline.PrintableImageError):
            pipeline.PrintSettings.from_mapping({"print_mode": "halftone"})
        with self.assertRaises(pipeline.PrintableImageError):
            pipeline.PrintSettings.from_mapping({"line_width_mm": 0.5, "minimum_feature_mm": 0.4})


class PrintableImagePipelineTests(unittest.TestCase):
    def test_color_component_stats_excludes_background_and_measures_secondary_islands(self):
        width = height = 6
        indices = bytearray([0] * (width * height))
        background = bytearray([255] * (width * height))
        for y in range(1, 5):
            for x in range(1, 5):
                background[y * width + x] = 0
                indices[y * width + x] = 1
        indices[1 * width + 1] = 0
        indices[4 * width + 4] = 0

        counts, secondary = pipeline._color_component_stats(indices, background, width, height, 2)

        self.assertEqual(counts, [2, 1])
        self.assertAlmostEqual(secondary[0], 1 / 16)
        self.assertEqual(secondary[1], 0.0)
    def run_pipeline(self, image, **settings):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / "source.png"
        image.save(source)
        result = pipeline.process_printable_image(source, root / "result", RGBW, settings or None)
        return result

    def test_outputs_exact_palette_and_complete_exclusive_masks(self):
        image = Image.new("RGB", (80, 60), (245, 245, 238))
        for x in range(10, 70):
            for y in range(8, 52):
                image.putpixel((x, y), ((x * 3) % 255, (y * 5) % 255, ((x + y) * 2) % 255))

        result = self.run_pipeline(image)

        with Image.open(result.clean_preview).convert("RGB") as clean:
            self.assertLessEqual(set(clean.getdata()), {pipeline._hex_rgb(color) for color in RGBW})
        masks = [Image.open(path).convert("L") for path in result.masks.values()]
        try:
            for values in zip(*(mask.getdata() for mask in masks)):
                self.assertEqual(sum(value == 255 for value in values), 1)
        finally:
            for mask in masks:
                mask.close()
        self.assertEqual(set(result.masks), {"primary", "structure", "light", "accent"})
        self.assertTrue(result.background_mask.is_file())
        self.assertTrue(result.subject_mask.is_file())
        self.assertTrue(result.heatmap.is_file())

        self.assertTrue(result.model_reference.is_file())
        with Image.open(result.model_reference) as reference:
            self.assertEqual(reference.mode, "RGBA")
            self.assertEqual(reference.getpixel((0, 0))[3], 0)
            self.assertEqual(reference.getpixel((31, 19))[:3], image.getpixel((31, 19)))
        with Image.open(result.clean_preview) as clean:
            self.assertEqual(clean.mode, "RGBA")
            self.assertEqual(clean.getpixel((0, 0))[3], 0)

    def test_pipeline_preserves_five_and_six_color_role_metadata(self):
        colors = ("#D93632", "#252525", "#F2F1EA", "#315CA8", "#3B8C54", "#9B3F77")
        role_order = ("primary", "structure", "light", "accent", "secondary", "detail")
        for count in (5, 6):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                image = Image.new("RGBA", (180, 120), (0, 0, 0, 0))
                band_width = 140 // count
                for index, color in enumerate(colors[:count]):
                    left = 20 + index * band_width
                    right = 160 if index == count - 1 else left + band_width
                    image.paste(pipeline._hex_rgb(color) + (255,), (left, 20, right, 100))
                source = root / "source.png"
                image.save(source)

                result = pipeline.process_printable_image(source, root / "result", colors[:count])
                metadata = json.loads(result.metadata.read_text(encoding="utf-8"))

                self.assertEqual(len(metadata["palette"]), count)
                self.assertEqual(tuple(metadata["palette_roles"]["color_by_role"]), role_order[:count])
                self.assertEqual(set(result.masks), set(role_order[:count]))
                with Image.open(result.clean_preview).convert("RGBA") as clean:
                    opaque = {pixel[:3] for pixel in clean.getdata() if pixel[3] == 255}
                self.assertLessEqual(opaque, {pipeline._hex_rgb(color) for color in colors[:count]})
                self.assertTrue(result.model_reference.is_file())

    def test_palette_mapping_preserves_solid_regions_without_dithering(self):
        palette = ("#FF0000", "#00FF00", "#0000FF")
        for count in (2, 3):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                image = Image.new("RGBA", (120, 80), (0, 0, 0, 0))
                for index, color in enumerate(((230, 40, 40), (30, 220, 40), (30, 40, 220))[:count]):
                    image.paste(color + (255,), (15 + index * 30, 15, 45 + index * 30, 65))
                source = root / "source.png"
                image.save(source)

                result = pipeline.process_printable_image(source, root / "result", palette[:count])

                with Image.open(result.clean_preview) as clean:
                    opaque = {pixel[:3] for pixel in clean.getdata() if pixel[3] == 255}
                    self.assertEqual(opaque, {pipeline._hex_rgb(color) for color in palette[:count]})
                    for index, color in enumerate(palette[:count]):
                        region = clean.crop((18 + index * 30, 18, 42 + index * 30, 62))
                        self.assertEqual(set(region.getdata()), {pipeline._hex_rgb(color) + (255,)})

    def test_portrait_preserves_skin_armor_and_base_without_forcing_unused_colors(self):
        palette = ("#DCDBD7", "#DAAE8C", "#EA0006", "#83B771", "#242421", "#0102FF")
        roles = dict(zip(("primary", "light", "secondary", "accent", "structure", "detail"), palette))
        image = Image.new("RGB", (160, 200), (187, 188, 192))
        image.paste((219, 177, 160), (62, 20, 98, 62))
        image.paste((235, 225, 210), (38, 72, 122, 145))
        image.paste((210, 80, 45), (38, 90, 60, 130))
        image.paste((120, 175, 105), (50, 90, 110, 126))
        image.paste((36, 36, 33), (25, 150, 135, 188))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "portrait.png"
            image.save(source)

            result = pipeline.process_printable_image(source, root / "result", palette, palette_roles=roles)

            with Image.open(result.clean_preview) as clean:
                self.assertEqual(clean.getpixel((0, 0))[3], 0)
                for point, color in (((80, 40), palette[1]), ((110, 80), palette[0]),
                                     ((45, 110), palette[2]), ((80, 105), palette[3]), ((80, 170), palette[4])):
                    self.assertEqual(clean.getpixel(point), pipeline._hex_rgb(color) + (255,))
                opaque = {pixel[:3] for pixel in clean.getdata() if pixel[3] == 255}
                self.assertNotIn(pipeline._hex_rgb(palette[5]), opaque)

    def test_minimum_feature_merges_tiny_island_into_neighbor(self):
        image = Image.new("RGB", (100, 100), pipeline._hex_rgb(RGBW[3]))
        for x in range(20, 80):
            for y in range(20, 80):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[0]))
        for x in range(49, 52):
            for y in range(49, 52):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[2]))

        result = self.run_pipeline(image, width_mm=100, minimum_feature_mm=4.0)

        with Image.open(result.strict_preview) as strict, Image.open(result.clean_preview).convert("RGB") as clean:
            self.assertEqual(strict.getpixel((50, 50)), pipeline._hex_rgb(RGBW[2]))
            self.assertEqual(clean.getpixel((50, 50)), pipeline._hex_rgb(RGBW[0]))
        self.assertGreater(result.metrics["changed_pixel_ratio"], 0)
        self.assertEqual(result.metrics["minimum_feature_px"], 4)

    def test_physical_print_size_changes_cleanup_result(self):
        image = Image.new("RGB", (200, 100), pipeline._hex_rgb(RGBW[3]))
        for x in range(20, 180):
            for y in range(40, 43):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[1]))

        wide = self.run_pipeline(image, width_mm=400, minimum_feature_mm=0.8)
        narrow = self.run_pipeline(image, width_mm=40, minimum_feature_mm=0.8)

        self.assertLess(wide.metrics["minimum_feature_px"], narrow.metrics["minimum_feature_px"])
        self.assertLess(wide.metrics["changed_pixel_ratio"], narrow.metrics["changed_pixel_ratio"])

    def test_cleanup_removes_long_line_thinner_than_printable_feature(self):
        image = Image.new("RGB", (120, 100), pipeline._hex_rgb(RGBW[3]))
        for x in range(15, 105):
            for y in range(20, 80):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[2]))
        for x in range(20, 100):
            for y in range(49, 51):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[1]))

        result = self.run_pipeline(image, width_mm=24, minimum_feature_mm=1.2)

        with Image.open(result.strict_preview) as strict, Image.open(result.clean_preview).convert("RGB") as clean:
            self.assertEqual(strict.getpixel((60, 50)), pipeline._hex_rgb(RGBW[1]))
            self.assertEqual(clean.getpixel((60, 50)), pipeline._hex_rgb(RGBW[2]))

    def test_metadata_records_palette_print_settings_and_metrics(self):
        result = self.run_pipeline(Image.new("RGB", (40, 30), (220, 60, 55)), width_mm=120, nozzle_mm=0.6,
                                   line_width_mm=0.6, minimum_feature_mm=1.2)

        metadata = json.loads(result.metadata.read_text(encoding="utf-8"))
        self.assertEqual(metadata["schema_version"], 2)
        self.assertEqual(set(metadata["palette_roles"]["color_by_role"]), {"primary", "structure", "light", "accent"})
        self.assertEqual(metadata["outputs"]["model_reference"], "model_reference.png")
        self.assertEqual([entry["hex"] for entry in metadata["palette"]], list(RGBW))
        self.assertEqual(metadata["print"]["width_mm"], 120.0)
        self.assertEqual(metadata["metrics"]["minimum_feature_px"], result.metrics["minimum_feature_px"])
        self.assertEqual(metadata["outputs"]["masks"].keys(), result.masks.keys())

    def test_transparency_is_composited_to_brightest_filament(self):
        image = Image.new("RGBA", (30, 30), (0, 0, 0, 0))
        image.paste((210, 45, 45, 255), (10, 10, 20, 20))
        result = self.run_pipeline(image)
        with Image.open(result.clean_preview) as clean:
            self.assertEqual(clean.getpixel((0, 0))[3], 0)

    def test_source_alpha_preserves_light_structural_connector(self):
        image = Image.new("RGBA", (140, 100), (0, 0, 0, 0))
        image.paste(pipeline._hex_rgb(RGBW[2]) + (255,), (15, 20, 55, 80))
        image.paste(pipeline._hex_rgb(RGBW[2]) + (255,), (85, 20, 125, 80))
        # This printable light brace has the same RGB value as the composited
        # transparent background. The source alpha, not border color, must own
        # the silhouette or the two opaque bodies become disconnected.
        image.paste(pipeline._hex_rgb(RGBW[3]) + (255,), (55, 47, 85, 53))

        result = self.run_pipeline(image, width_mm=140, minimum_feature_mm=0.8)

        with Image.open(result.model_reference) as reference:
            self.assertEqual(reference.getpixel((70, 50))[3], 255)
            self.assertEqual(reference.getpixel((0, 0))[3], 0)
        metadata = json.loads(result.metadata.read_text(encoding="utf-8"))
        self.assertEqual(metadata["background_detection"], "source_alpha")
        self.assertEqual(result.metrics["meaningful_subject_component_count"], 1)
        self.assertTrue(result.metrics["palette_quality_ok"])

    def test_light_gray_studio_background_maps_to_brightest_filament(self):
        image = Image.new("RGB", (100, 80), (155, 161, 167))
        for x, color in ((15, RGBW[0]), (40, RGBW[1]), (65, RGBW[2])):
            for px in range(x, x + 20):
                for py in range(15, 65):
                    image.putpixel((px, py), pipeline._hex_rgb(color))

        result = self.run_pipeline(image)

        with Image.open(result.clean_preview) as clean:
            self.assertEqual(clean.getpixel((0, 0))[3], 0)
            self.assertEqual(clean.getpixel((99, 79))[3], 0)

    def test_painted_checkerboard_does_not_swallow_white_filament_subject(self):
        image = Image.new("RGB", (160, 140), (255, 255, 255))
        for y in range(140):
            for x in range(160):
                shade = 246 if (x // 10 + y // 10) % 2 else 255
                image.putpixel((x, y), (shade, shade, shade))
        # A red outline separates a white printable face from the painted
        # checkerboard, matching the light-subject failure seen in Image2.
        for x in range(35, 125):
            for y in range(20, 120):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[0]))
        for x in range(43, 117):
            for y in range(28, 112):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[3]))
        for x in range(50, 75):
            for y in range(75, 105):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[1]))
        for x in range(85, 110):
            for y in range(75, 105):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[2]))

        result = self.run_pipeline(image)

        with Image.open(result.clean_preview) as clean:
            self.assertEqual(clean.getpixel((0, 0))[3], 0)
            self.assertEqual(clean.getpixel((80, 50)), pipeline._hex_rgb(RGBW[3]) + (255,))
        self.assertTrue(result.metrics["palette_quality_ok"])
        self.assertEqual(result.metrics["meaningful_subject_color_count"], 4)

    def test_portrait_mask_repair_closes_checkerboard_leak_without_filling_outer_background(self):
        width, height = 240, 300
        # 255 means background. The rectangular portrait silhouette contains a
        # large light-garment hole connected to the outer background through a
        # narrow checkerboard-colored slit.
        background = bytearray([255] * (width * height))
        for y in range(35, 280):
            for x in range(45, 195):
                background[y * width + x] = 0
        for y in range(130, 210):
            for x in range(80, 145):
                background[y * width + x] = 255
        for y in range(120, 136):
            for x in range(80, 86):
                background[y * width + x] = 255
        for y in range(35, 121):
            for x in range(80, 86):
                background[y * width + x] = 255

        repaired, recovered = pipeline._repair_portrait_background_mask(
            bytes(background), width, height
        )

        self.assertGreater(recovered, 0)
        self.assertEqual(repaired[170 * width + 110], 0)
        self.assertEqual(repaired[10 * width + 10], 255)

    def test_source_mask_recovers_large_white_garment_but_not_generated_base(self):
        width, height = 160, 200
        background = bytearray([255] * (width * height))
        # Current generated mask sees the face, inner garment and base, but an
        # opaque checkerboard swallowed the broad white blazer between them.
        for y in range(20, 70):
            for x in range(58, 102):
                background[y * width + x] = 0
        for y in range(60, 145):
            for x in range(72, 88):
                background[y * width + x] = 0
        for y in range(75, 145):
            for x in list(range(30, 48)) + list(range(112, 130)):
                background[y * width + x] = 0
        for y in range(165, 190):
            for x in range(30, 130):
                background[y * width + x] = 0

        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        reference_path = Path(temporary.name) / "source.png"
        reference = Image.new("RGB", (width, height), (150, 155, 160))
        # Source upper body is easy to separate from its gray studio backdrop.
        reference.paste((25, 25, 25), (58, 20, 102, 70))
        reference.paste((245, 243, 238), (30, 60, 130, 150))
        reference.paste((50, 90, 70), (72, 60, 88, 145))
        reference.save(reference_path)

        repaired, recovered = pipeline._repair_portrait_mask_from_source(
            bytes(background), width, height, reference_path
        )

        self.assertGreater(recovered, 0)
        self.assertEqual(repaired[100 * width + 45], 0)
        self.assertEqual(repaired[10 * width + 10], 255)
        self.assertEqual(repaired[180 * width + 80], 0)

    def test_quality_gate_allows_legitimate_single_color_subject_with_diversity_warning(self):
        image = Image.new("RGB", (120, 100), pipeline._hex_rgb(RGBW[3]))
        for x in range(25, 95):
            for y in range(20, 85):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[2]))

        result = self.run_pipeline(image)

        self.assertTrue(result.metrics["palette_quality_ok"])
        self.assertFalse(result.metrics["palette_diversity_ok"])
        # The white studio background is transparent in the 3D reference and no longer counts as a model color.
        self.assertEqual(result.metrics["meaningful_palette_count"], 1)
        self.assertIn("too_few_meaningful_palette_colors", result.metrics["quality_warnings"])

    def test_single_filament_preserves_subject_silhouette_and_uses_exactly_one_color(self):
        image = Image.new("RGB", (120, 100), (250, 250, 248))
        for x in range(20, 101):
            for y in range(15, 86):
                image.putpixel((x, y), (45 + x, 35 + y, 90))
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / "source.png"
        image.save(source)

        result = pipeline.process_printable_image(source, root / "result", ("#237B45",), None)

        with Image.open(result.clean_preview) as clean:
            alpha = clean.getchannel("A")
            self.assertEqual(alpha.getbbox(), (20, 15, 101, 86))
            opaque_colors = {pixel[:3] for pixel in clean.getdata() if pixel[3] == 255}
            self.assertEqual(opaque_colors, {(0x23, 0x7B, 0x45)})
        self.assertEqual(result.metrics["meaningful_subject_color_count"], 1)
        self.assertTrue(result.metrics["palette_quality_ok"])

    def test_quality_gate_accepts_three_meaningful_designer_toy_colors(self):
        image = Image.new("RGB", (120, 100), pipeline._hex_rgb(RGBW[3]))
        for x, color in ((20, RGBW[0]), (45, RGBW[1]), (70, RGBW[2])):
            for px in range(x, x + 25):
                for py in range(20, 85):
                    image.putpixel((px, py), pipeline._hex_rgb(color))

        result = self.run_pipeline(image)

        self.assertTrue(result.metrics["palette_quality_ok"])
        self.assertGreaterEqual(result.metrics["meaningful_palette_count"], 3)
        self.assertGreaterEqual(result.metrics["meaningful_subject_color_count"], 2)
        self.assertGreaterEqual(result.metrics["largest_subject_component_ratio"], 0.90)

    def test_quality_gate_rejects_subject_split_by_background_color(self):
        image = Image.new("RGB", (120, 120), pipeline._hex_rgb(RGBW[3]))
        for x in range(20, 100):
            for y in range(10, 55):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[0]))
        for x in range(45, 75):
            for y in range(20, 45):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[1]))
        for x in range(25, 95):
            for y in range(80, 110):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[2]))

        result = self.run_pipeline(image)

        self.assertFalse(result.metrics["palette_quality_ok"])
        self.assertEqual(result.metrics["meaningful_subject_component_count"], 2)
        self.assertLess(result.metrics["largest_subject_component_ratio"], 0.90)
        self.assertIn("printable_subject_is_disconnected", result.metrics["quality_warnings"])

    def test_quality_gate_rejects_long_thin_detached_structure_even_when_area_is_small(self):
        image = Image.new("RGB", (140, 120), pipeline._hex_rgb(RGBW[3]))
        for x in range(15, 90):
            for y in range(20, 100):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[0]))
        # A two-pixel-wide detached handle has little area, but losing its
        # length materially changes the object supplied to image-to-3D.
        for x in range(110, 112):
            for y in range(30, 90):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[1]))

        result = self.run_pipeline(image)

        self.assertGreaterEqual(result.metrics["largest_subject_component_ratio"], 0.90)
        self.assertGreaterEqual(result.metrics["largest_detached_subject_diagonal_ratio"], 0.08)
        self.assertFalse(result.metrics["palette_quality_ok"])
        self.assertIn(
            "printable_subject_has_large_detached_structure",
            result.metrics["quality_warnings"],
        )

    def test_quality_gate_rejects_background_color_that_swallows_subject(self):
        image = Image.new("RGB", (120, 100), pipeline._hex_rgb(RGBW[3]))
        for x, color, width in ((35, RGBW[0], 12), (47, RGBW[1], 8), (55, RGBW[2], 14)):
            for px in range(x, x + width):
                for py in range(25, 65):
                    image.putpixel((px, py), pipeline._hex_rgb(color))

        result = self.run_pipeline(image)

        # Only subject colors count; the boundary-connected white background is excluded.
        self.assertEqual(result.metrics["meaningful_palette_count"], 3)
        self.assertFalse(result.metrics["palette_quality_ok"])
        self.assertLess(result.metrics["printable_subject_area_ratio"], 0.18)
        self.assertIn("printable_subject_area_below_18_percent", result.metrics["quality_warnings"])

    def test_dominant_red_toy_keeps_smaller_green_and_blue_materials_separate(self):
        image = Image.new("RGB", (200, 120), (254, 254, 254))
        for x in range(20, 100):
            red = 180 + (x % 12) * 6
            for y in range(15, 110):
                image.putpixel((x, y), (red, 12 + y % 14, 12 + x % 9))
        for x in range(100, 125):
            for y in range(20, 100):
                image.putpixel((x, y), (5, 130 + y % 35, 45 + x % 20))
        for x in range(125, 150):
            for y in range(20, 100):
                image.putpixel((x, y), (5 + x % 15, 75 + y % 30, 175 + x % 45))

        result = self.run_pipeline(image)

        self.assertGreater(result.palette_usage[RGBW[0]], 0)
        self.assertGreater(result.palette_usage[RGBW[1]], 0)
        self.assertGreater(result.palette_usage[RGBW[2]], 0)

    def test_portrait_skin_stabilizer_keeps_face_and_hand_but_removes_warm_jacket_shadow(self):
        palette = ("#F2EFE6", "#1F2937", "#D8A17C", "#356B52")
        roles = {
            "primary": "#F2EFE6",
            "structure": "#1F2937",
            "light": "#D8A17C",
            "accent": "#356B52",
        }
        image = Image.new("RGBA", (160, 200), (0, 0, 0, 0))
        image.paste((235, 229, 220, 255), (30, 65, 130, 190))
        image.paste((42, 45, 48, 255), (55, 12, 105, 40))
        image.paste((214, 159, 118, 255), (50, 30, 110, 95))
        image.paste((48, 98, 76, 255), (68, 82, 92, 165))
        image.paste((228, 176, 135, 255), (35, 115, 60, 135))
        # This low-chroma warm jacket shadow is perceptually close to the skin
        # filament, but material ownership must remain with the white jacket.
        image.paste((163, 150, 130, 255), (58, 120, 120, 155))
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / "portrait.png"
        image.save(source)

        result = pipeline.process_printable_image(
            source,
            root / "result",
            palette,
            None,
            palette_roles=roles,
        )

        with Image.open(result.clean_preview).convert("RGB") as reference:
            self.assertEqual(reference.getpixel((80, 55)), pipeline._hex_rgb("#D8A17C"))
            self.assertEqual(reference.getpixel((45, 125)), pipeline._hex_rgb("#D8A17C"))
            self.assertEqual(reference.getpixel((95, 135)), pipeline._hex_rgb("#F2EFE6"))
        with Image.open(result.model_reference).convert("RGB") as reference:
            self.assertEqual(reference.getpixel((80, 55)), (214, 159, 118))
            self.assertEqual(reference.getpixel((95, 135)), (163, 150, 130))
        self.assertEqual(result.metrics["portrait_skin_cleanup"]["activated"], 1)
        self.assertGreater(result.metrics["portrait_skin_cleanup"]["recolored_pixels"], 0)

    def test_portrait_skin_stabilizer_recovers_when_ai_swaps_skin_and_garment_roles(self):
        palette = ("#F2C7A5", "#1C1A1B", "#F7F6F2", "#4F6B5A")
        swapped_roles = {
            "primary": "#F2C7A5",
            "structure": "#1C1A1B",
            "light": "#F7F6F2",
            "accent": "#4F6B5A",
        }
        image = Image.new("RGBA", (160, 200), (0, 0, 0, 0))
        image.paste((235, 229, 220, 255), (30, 65, 130, 190))
        image.paste((42, 45, 48, 255), (55, 12, 105, 40))
        image.paste((224, 172, 132, 255), (50, 30, 110, 95))
        image.paste((62, 88, 73, 255), (68, 82, 92, 165))
        image.paste((228, 176, 135, 255), (35, 115, 60, 135))
        image.paste((163, 150, 130, 255), (58, 120, 120, 155))
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / "portrait-swapped.png"
        image.save(source)

        result = pipeline.process_printable_image(
            source,
            root / "result",
            palette,
            None,
            palette_roles=swapped_roles,
        )

        cleanup = result.metrics["portrait_skin_cleanup"]
        self.assertEqual(cleanup["activated"], 1)
        self.assertEqual(cleanup["role_fallback_used"], 1)
        self.assertEqual(cleanup["garment_color"], "#F7F6F2")
        self.assertEqual(cleanup["skin_color"], "#F2C7A5")
        metadata = json.loads(result.metadata.read_text(encoding="utf-8"))
        self.assertEqual(metadata["palette_roles"]["color_by_role"]["primary"], "#F7F6F2")
        self.assertEqual(metadata["palette_roles"]["color_by_role"]["light"], "#F2C7A5")
        with Image.open(result.clean_preview).convert("RGB") as reference:
            self.assertEqual(reference.getpixel((80, 55)), pipeline._hex_rgb("#F2C7A5"))
            self.assertEqual(reference.getpixel((95, 135)), pipeline._hex_rgb("#F7F6F2"))

    def test_portrait_skin_stabilizer_separates_profile_face_and_hand_from_connected_jacket_shadow(self):
        palette = ("#F7F6F1", "#1B1A1E", "#F3D2BE", "#4E6B5A")
        roles = {
            "primary": "#F7F6F1",
            "structure": "#1B1A1E",
            "light": "#F3D2BE",
            "accent": "#4E6B5A",
        }
        width, height = 100, 140
        background = bytearray([1] * (width * height))
        indices = bytearray([0] * (width * height))
        source_pixels = [(0, 0, 0)] * (width * height)

        def paint(box, index, color):
            for y in range(box[1], box[3]):
                for x in range(box[0], box[2]):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = color

        paint((20, 20, 85, 130), 0, (239, 235, 229))
        paint((57, 12, 82, 54), 2, (218, 166, 130))
        # A low-chroma cream shadow touches the face and continues down the
        # jacket, creating one diffuse skin-labelled component.
        paint((68, 48, 84, 120), 2, (218, 204, 190))
        paint((22, 72, 38, 88), 2, (224, 171, 135))

        cleaned, report = pipeline._stabilize_portrait_skin_components(
            bytes(indices), bytes(background), source_pixels, width, height, palette, roles
        )

        self.assertEqual(report["activated"], 1)
        self.assertEqual(report["strong_skin_fallback_used"], 1)
        self.assertEqual(cleaned[30 * width + 70], 2)
        self.assertEqual(cleaned[80 * width + 30], 2)
        self.assertEqual(cleaned[105 * width + 76], 0)

    def test_portrait_skin_stabilizer_handles_rear_view_without_front_face(self):
        palette = ("#F7F6F1", "#1B1A1E", "#F3D2BE", "#4E6B5A")
        roles = {
            "primary": "#F7F6F1",
            "structure": "#1B1A1E",
            "light": "#F3D2BE",
            "accent": "#4E6B5A",
        }
        width, height = 100, 140
        background = bytearray([1] * (width * height))
        indices = bytearray([0] * (width * height))
        source_pixels = [(0, 0, 0)] * (width * height)

        def paint(box, index, color):
            for y in range(box[1], box[3]):
                for x in range(box[0], box[2]):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = color

        paint((18, 22, 82, 130), 0, (240, 236, 230))
        paint((34, 8, 66, 36), 1, (27, 26, 30))
        paint((38, 30, 62, 48), 2, (205, 153, 119))
        paint((72, 70, 84, 91), 2, (222, 169, 132))
        paint((68, 48, 78, 124), 2, (216, 202, 188))
        paint((20, 62, 28, 118), 2, (220, 205, 190))

        cleaned, report = pipeline._stabilize_portrait_skin_components(
            bytes(indices), bytes(background), source_pixels, width, height, palette, roles
        )

        self.assertEqual(report["activated"], 1)
        self.assertEqual(cleaned[38 * width + 50], 2)
        self.assertEqual(cleaned[80 * width + 78], 2)
        self.assertEqual(cleaned[110 * width + 72], 0)
        self.assertEqual(cleaned[90 * width + 24], 0)

    def test_portrait_semantic_recovery_keeps_all_roles_unique_after_arbitrary_role_edits(self):
        palette = ("#F7F6F2", "#1E1B1C", "#F2C8A9", "#4E6B5A")
        edited_roles = {
            "primary": "#1E1B1C",
            "structure": "#F2C8A9",
            "light": "#F7F6F2",
            "accent": "#4E6B5A",
        }
        image = Image.new("RGBA", (160, 200), (0, 0, 0, 0))
        image.paste((235, 229, 220, 255), (30, 65, 130, 190))
        image.paste((42, 45, 48, 255), (55, 12, 105, 40))
        image.paste((224, 172, 132, 255), (50, 30, 110, 95))
        image.paste((62, 88, 73, 255), (68, 82, 92, 165))
        image.paste((228, 176, 135, 255), (35, 115, 60, 135))
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / "portrait-arbitrary-roles.png"
        image.save(source)

        result = pipeline.process_printable_image(
            source,
            root / "result",
            palette,
            None,
            palette_roles=edited_roles,
        )

        metadata = json.loads(result.metadata.read_text(encoding="utf-8"))
        recovered = metadata["palette_roles"]["color_by_role"]
        self.assertEqual(recovered["primary"], "#F7F6F2")
        self.assertEqual(recovered["light"], "#F2C8A9")
        self.assertEqual(set(recovered.values()), set(palette))
        self.assertEqual(set(result.masks), {"primary", "structure", "light", "accent"})

    def test_portrait_accent_stabilizer_removes_connected_warm_neck_bleed(self):
        palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
        width, height = 100, 140
        indices = bytearray([0] * (width * height))
        background = bytearray([1] * (width * height))
        source_pixels = [(244, 244, 240)] * (width * height)

        def paint(box, index, source_color):
            left, top, right, bottom = box
            for y in range(top, bottom):
                for x in range(left, right):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = source_color

        paint((20, 10, 80, 130), 0, (238, 235, 228))
        # Real green blouse plus a connected brown neck-shadow patch which the
        # fixed palette mapped to the same accent material.
        paint((40, 58, 62, 120), 3, (67, 104, 80))
        paint((55, 35, 62, 70), 3, (154, 116, 88))
        report = {
            "activated": 1,
            "garment_color": "#F4F4F0",
            "skin_color": "#F2C9AE",
            "face_bounds": {"left": 30, "right": 70, "top": 15, "bottom": 70},
        }

        cleanup = pipeline._stabilize_portrait_accent_components(
            indices,
            background,
            source_pixels,
            width,
            height,
            palette,
            report,
        )

        self.assertEqual(indices[45 * width + 58], 2)
        self.assertEqual(indices[90 * width + 50], 3)
        self.assertGreater(cleanup["accent_skin_recolored_pixels"], 0)

    def test_portrait_accent_stabilizer_merges_dark_same_hue_folds_but_keeps_black_accessory(self):
        palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
        width, height = 100, 140
        indices = bytearray([0] * (width * height))
        background = bytearray([1] * (width * height))
        source_pixels = [(244, 244, 240)] * (width * height)

        def paint(box, index, source_color):
            left, top, right, bottom = box
            for y in range(top, bottom):
                for x in range(left, right):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = source_color

        paint((10, 5, 90, 135), 0, (238, 235, 228))
        paint((38, 56, 64, 118), 3, (68, 102, 80))
        # Both patches were mapped to the black filament.  Only the dark green
        # folds belong to the accent blouse; the small neutral watch remains
        # black even though it is also inside the garment envelope.
        paint((43, 72, 58, 92), 1, (30, 48, 36))
        paint((39, 96, 48, 114), 1, (30, 29, 29))
        paint((55, 101, 61, 108), 1, (30, 29, 29))
        report = {
            "activated": 1,
            "garment_color": "#F4F4F0",
            "skin_color": "#F2C9AE",
            "face_bounds": {"left": 30, "right": 70, "top": 15, "bottom": 55},
        }

        cleanup = pipeline._stabilize_portrait_accent_components(
            indices, background, source_pixels, width, height, palette, report
        )

        self.assertEqual(indices[80 * width + 50], 3)
        self.assertEqual(indices[104 * width + 43], 3)
        self.assertEqual(indices[104 * width + 58], 1)
        self.assertGreater(cleanup["accent_shadow_recolored_pixels"], 0)
        self.assertGreater(cleanup["accent_neutral_shadow_recolored_pixels"], 0)

    def test_portrait_accent_stabilizer_seeds_a_fully_dark_green_blouse(self):
        palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
        width, height = 100, 140
        indices = bytearray([0] * (width * height))
        background = bytearray([1] * (width * height))
        source_pixels = [(244, 244, 240)] * (width * height)

        def paint(box, index, source_color):
            left, top, right, bottom = box
            for y in range(top, bottom):
                for x in range(left, right):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = source_color

        paint((10, 5, 90, 135), 0, (238, 235, 228))
        # The whole blouse was assigned to black even though its source hue is
        # green. A neutral black watch inside the same torso window stays black.
        paint((38, 55, 64, 118), 1, (25, 48, 35))
        paint((44, 99, 49, 104), 1, (28, 28, 28))
        report = {
            "activated": 1,
            "garment_color": "#F4F4F0",
            "skin_color": "#F2C9AE",
            "face_bounds": {"left": 30, "right": 70, "top": 15, "bottom": 55},
        }

        cleanup = pipeline._stabilize_portrait_accent_components(
            indices, background, source_pixels, width, height, palette, report
        )

        self.assertEqual(indices[80 * width + 55], 3)
        self.assertEqual(indices[101 * width + 46], 1)
        self.assertGreater(cleanup["accent_seeded_pixels"], 0)

    def test_portrait_accent_stabilizer_uses_source_hue_for_profile_and_rear_views(self):
        palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
        width, height = 100, 140
        indices = bytearray([0] * (width * height))
        background = bytearray([1] * (width * height))
        source_pixels = [(244, 244, 240)] * (width * height)

        def paint(box, index, source_color):
            left, top, right, bottom = box
            for y in range(top, bottom):
                for x in range(left, right):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = source_color

        paint((10, 5, 90, 135), 0, (238, 235, 228))
        paint((42, 50, 63, 108), 1, (29, 55, 39))
        # A green reflection on the pedestal has no source-hue evidence and
        # must not survive simply because it is the largest side-view island.
        paint((12, 122, 88, 134), 3, (30, 29, 29))
        report = {
            "activated": 1,
            "strong_skin_fallback_used": 1,
            "garment_color": "#F4F4F0",
            "skin_color": "#F2C9AE",
            "face_bounds": {"left": 30, "right": 70, "top": 15, "bottom": 55},
        }

        cleanup = pipeline._stabilize_portrait_accent_components(
            indices, background, source_pixels, width, height, palette, report
        )

        self.assertEqual(indices[80 * width + 52], 3)
        self.assertEqual(indices[128 * width + 50], 0)
        self.assertGreater(cleanup["accent_seeded_pixels"], 0)
        self.assertGreater(cleanup["accent_removed_components"], 0)

    def test_portrait_accent_stabilizer_prefers_blouse_over_larger_base_reflection(self):
        palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
        width, height = 100, 140
        indices = bytearray([0] * (width * height))
        background = bytearray([1] * (width * height))
        source_pixels = [(244, 244, 240)] * (width * height)

        def paint(box, index, source_color):
            left, top, right, bottom = box
            for y in range(top, bottom):
                for x in range(left, right):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = source_color

        paint((10, 5, 90, 135), 0, (238, 235, 228))
        paint((39, 50, 62, 106), 3, (55, 93, 68))
        # A wider base reflection is closer to the accent filament but its
        # source is neutral charcoal, so it must not become the anchor.
        paint((12, 118, 88, 133), 3, (35, 34, 34))
        report = {
            "activated": 1,
            "garment_color": "#F4F4F0",
            "skin_color": "#F2C9AE",
            "face_bounds": {"left": 30, "right": 70, "top": 15, "bottom": 55},
        }

        cleanup = pipeline._stabilize_portrait_accent_components(
            indices, background, source_pixels, width, height, palette, report
        )

        self.assertEqual(indices[75 * width + 50], 3)
        self.assertNotEqual(indices[125 * width + 50], 3)
        self.assertEqual(cleanup["accent_anchor_in_torso"], 1)
        self.assertGreater(cleanup["accent_anchor_source_hue_pixels"], 0)

    def test_portrait_skin_stabilizer_keeps_compact_hand_and_removes_long_warm_crease(self):
        palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
        width, height = 100, 140
        indices = bytearray([0] * (width * height))
        background = bytearray([1] * (width * height))
        source_pixels = [(244, 244, 240)] * (width * height)

        def paint(box, index, source_color):
            left, top, right, bottom = box
            for y in range(top, bottom):
                for x in range(left, right):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = source_color

        paint((10, 5, 90, 135), 0, (238, 235, 228))
        paint((35, 15, 65, 55), 2, (218, 159, 118))
        paint((24, 74, 42, 88), 2, (158, 135, 104))
        paint((45, 98, 90, 101), 2, (177, 156, 123))

        report = {
            "activated": 1,
            "garment_color": "#F4F4F0",
            "skin_color": "#F2C9AE",
            "face_bounds": {"left": 35, "right": 64, "top": 15, "bottom": 54},
        }
        cleanup = pipeline._stabilize_clean_portrait_skin_components(
            indices,
            background,
            source_pixels,
            width,
            height,
            palette,
            report,
        )

        self.assertEqual(indices[80 * width + 30], 2)
        self.assertEqual(indices[99 * width + 70], 0)
        self.assertGreater(cleanup["postclean_skin_recolored_pixels"], 0)

    def test_portrait_base_stabilizer_removes_dark_green_and_skin_reflections(self):
        palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
        width, height = 120, 160
        indices = bytearray([0] * (width * height))
        background = bytearray([1] * (width * height))
        source_pixels = [(244, 244, 240)] * (width * height)

        def paint(box, index, source_color):
            left, top, right, bottom = box
            for y in range(top, bottom):
                for x in range(left, right):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = source_color

        paint((30, 15, 90, 132), 0, (238, 235, 228))
        paint((42, 20, 78, 68), 2, (220, 163, 122))
        paint((10, 124, 110, 153), 1, (30, 28, 29))
        # Two dark reflections were assigned to garment/skin materials.
        paint((18, 127, 58, 138), 3, (45, 65, 55))
        paint((68, 140, 82, 147), 2, (92, 79, 70))
        paint((28, 145, 52, 147), 0, (225, 225, 222))
        # The bright jacket intentionally overlaps the top of the pedestal.
        paint((48, 120, 72, 134), 0, (231, 228, 220))
        report = {
            "activated": 1,
            "garment_color": palette[0],
            "skin_color": palette[2],
            "face_bounds": {"left": 42, "right": 77, "top": 20, "bottom": 67},
        }

        cleanup = pipeline._stabilize_portrait_base_components(
            indices, background, source_pixels, width, height, palette, report
        )

        self.assertEqual(indices[130 * width + 24], 1)
        self.assertEqual(indices[143 * width + 75], 1)
        self.assertEqual(indices[145 * width + 35], 1)
        self.assertEqual(indices[128 * width + 55], 0)
        self.assertGreater(cleanup["base_recolored_pixels"], 0)
        self.assertEqual(cleanup["base_color"], palette[1])

    def test_portrait_skin_stabilizer_removes_small_ambiguous_skin_sliver_beside_clear_hand(self):
        palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
        width, height = 120, 160
        indices = bytearray([0] * (width * height))
        background = bytearray([1] * (width * height))
        source_pixels = [(244, 244, 240)] * (width * height)

        def paint(box, index, source_color):
            left, top, right, bottom = box
            for y in range(top, bottom):
                for x in range(left, right):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = source_color

        paint((10, 5, 110, 155), 0, (238, 235, 228))
        paint((40, 15, 80, 65), 2, (220, 163, 122))
        paint((20, 88, 48, 108), 2, (218, 159, 118))
        # A much smaller partially occluded patch is visually ambiguous after
        # 3D reconstruction and becomes a skin-coloured stripe on the jacket.
        paint((76, 104, 88, 111), 2, (215, 157, 116))
        report = {
            "activated": 1,
            "garment_color": "#F4F4F0",
            "skin_color": "#F2C9AE",
            "face_bounds": {"left": 40, "right": 79, "top": 15, "bottom": 64},
        }

        cleanup = pipeline._stabilize_clean_portrait_skin_components(
            indices, background, source_pixels, width, height, palette, report
        )

        self.assertEqual(indices[96 * width + 30], 2)
        self.assertEqual(indices[107 * width + 82], 0)
        self.assertGreater(cleanup["postclean_skin_recolored_pixels"], 0)

    def test_portrait_skin_stabilizer_fills_warm_face_highlights_but_keeps_teeth_and_jacket(self):
        palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
        width, height = 100, 140
        indices = bytearray([0] * (width * height))
        background = bytearray([1] * (width * height))
        source_pixels = [(244, 244, 240)] * (width * height)

        def paint(box, index, source_color):
            left, top, right, bottom = box
            for y in range(top, bottom):
                for x in range(left, right):
                    offset = y * width + x
                    background[offset] = 0
                    indices[offset] = index
                    source_pixels[offset] = source_color

        paint((10, 5, 90, 135), 0, (238, 235, 228))
        paint((30, 15, 70, 70), 2, (218, 159, 118))
        # A warm forehead highlight was mapped to white; teeth and eye whites
        # should remain white even in warm light, as should the lower jacket.
        paint((40, 24, 52, 31), 0, (249, 230, 218))
        # A disconnected dark dot on the forehead is not a legitimate facial
        # feature and would become a visible black material plug.
        paint((56, 25, 59, 28), 1, (28, 26, 26))
        paint((35, 32, 41, 37), 1, (35, 31, 31))
        paint((36, 33, 40, 36), 0, (210, 193, 182))
        paint((45, 50, 55, 56), 1, (35, 31, 31))
        paint((46, 51, 54, 55), 0, (210, 193, 182))
        paint((42, 63, 58, 68), 0, (226, 205, 188))
        report = {
            "activated": 1,
            "garment_color": "#F4F4F0",
            "skin_color": "#F2C9AE",
            "face_bounds": {"left": 30, "right": 69, "top": 15, "bottom": 69},
        }

        cleanup = pipeline._stabilize_clean_portrait_skin_components(
            indices,
            background,
            source_pixels,
            width,
            height,
            palette,
            report,
        )

        self.assertEqual(indices[27 * width + 45], 2)
        self.assertEqual(indices[26 * width + 57], 2)
        self.assertEqual(indices[34 * width + 38], 0)
        self.assertEqual(indices[53 * width + 50], 0)
        self.assertEqual(indices[65 * width + 50], 0)
        self.assertGreater(cleanup["postclean_face_skin_recolored_pixels"], 0)
        self.assertGreater(cleanup["postclean_face_structure_recolored_pixels"], 0)

    def test_portrait_smile_restore_keeps_teeth_but_never_restores_white_eyes(self):
        palette = ("#F4F4F0", "#1F1B1C", "#F2C9AE", "#4E6F5B")
        width, height = 100, 140
        skin_index = 2
        primary_index = 0
        cleaned = bytearray([skin_index] * (width * height))
        strict = bytearray(cleaned)
        background = bytearray([0] * (width * height))
        # Bright eye islands and a connected tooth band were both present
        # before generic feature-size cleanup.
        for y in range(29, 32):
            for x in range(38, 44):
                strict[y * width + x] = primary_index
            for x in range(56, 62):
                strict[y * width + x] = primary_index
        for y in range(43, 47):
            for x in range(43, 58):
                strict[y * width + x] = primary_index
        report = {
            "activated": 1,
            "garment_color": palette[0],
            "skin_color": palette[2],
            "face_bounds": {"left": 30, "right": 70, "top": 15, "bottom": 70},
        }

        result = pipeline._restore_clean_portrait_smile(
            cleaned, bytes(strict), bytes(background), width, height, palette, report
        )

        self.assertEqual(cleaned[45 * width + 50], primary_index)
        self.assertEqual(cleaned[30 * width + 40], skin_index)
        self.assertGreater(result["smile_restored_pixels"], 0)


if __name__ == "__main__":
    unittest.main()

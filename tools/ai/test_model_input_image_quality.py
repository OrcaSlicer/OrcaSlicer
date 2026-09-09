import tempfile
import unittest
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw

from tools.ai.model_input_image_quality import assess_model_input_image, recommend_printable_style


class ModelInputImageQualityTests(unittest.TestCase):
    def save_subject(self, root: Path, box: tuple[int, int, int, int]) -> Path:
        path = root / "subject.png"
        image = Image.new("RGBA", (512, 512), (255, 255, 255, 0))
        ImageDraw.Draw(image).rectangle(box, fill=(190, 80, 50, 255))
        image.save(path)
        return path

    def test_centered_connected_subject_is_eligible(self):
        with tempfile.TemporaryDirectory() as directory:
            result = assess_model_input_image(self.save_subject(Path(directory), (128, 96, 383, 447)))

        self.assertTrue(result["model_input_eligible"])
        self.assertGreaterEqual(result["score"], 90)
        self.assertGreater(result["metrics"]["largest_component_ratio"], 0.99)

    def test_tiny_subject_is_blocked(self):
        with tempfile.TemporaryDirectory() as directory:
            result = assess_model_input_image(self.save_subject(Path(directory), (246, 246, 265, 265)))

        self.assertFalse(result["model_input_eligible"])
        self.assertIn("subject_not_detected", result["blockers"])

    def test_subject_crossing_multiple_edges_is_cropped(self):
        with tempfile.TemporaryDirectory() as directory:
            result = assess_model_input_image(self.save_subject(Path(directory), (0, 0, 320, 511)))

        self.assertFalse(result["model_input_eligible"])
        self.assertIn("subject_cropped", result["blockers"])

    def test_disconnected_equal_subjects_are_blocked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "fragments.png"
            image = Image.new("RGBA", (512, 512), (255, 255, 255, 0))
            draw = ImageDraw.Draw(image)
            draw.rectangle((70, 140, 210, 370), fill=(190, 80, 50, 255))
            draw.rectangle((300, 140, 440, 370), fill=(60, 100, 200, 255))
            image.save(path)
            result = assess_model_input_image(path)

        self.assertFalse(result["model_input_eligible"])
        self.assertIn("fragmented_subject", result["blockers"])
        self.assertEqual(result["metrics"]["foreground_mask_source"], "alpha")
        self.assertNotIn("foreground_segmentation_uncertain", result["warnings"])

    def portrait_image(self, background: str) -> Image.Image:
        image = Image.new(
            "RGBA" if background == "transparent" else "RGB",
            (256, 384),
            (255, 255, 255, 0) if background == "transparent" else
            (240, 240, 240) if background == "gray" else (255, 255, 255),
        )
        draw = ImageDraw.Draw(image)
        if background == "checkerboard":
            for y in range(0, image.height, 8):
                for x in range(0, image.width, 8):
                    if (x // 8 + y // 8) % 2 == 0:
                        draw.rectangle((x, y, x + 7, y + 7), fill=(240, 240, 240))
        # The pale jacket physically connects the head, dark shirt and pedestal.
        draw.ellipse((91, 32, 165, 132), fill=(40, 30, 20))
        draw.rectangle((108, 120, 148, 145), fill=(215, 168, 134))
        draw.polygon(
            [(96, 133), (58, 165), (45, 306), (67, 332),
             (195, 332), (210, 306), (195, 165), (161, 133)],
            fill=(245, 245, 245),
        )
        draw.polygon([(107, 136), (149, 136), (144, 233), (112, 233)], fill=(38, 68, 51))
        draw.rounded_rectangle((46, 325, 211, 357), radius=12, fill=(68, 68, 68))
        return image

    def test_pale_connected_portrait_on_opaque_background_warns_without_mutating_image(self):
        for background in ("white", "gray", "checkerboard"):
            with self.subTest(background=background), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "portrait.png"
                self.portrait_image(background).save(path)
                original = path.read_bytes()

                result = assess_model_input_image(path)

                self.assertEqual(path.read_bytes(), original)
                self.assertTrue(result["model_input_eligible"], result)
                self.assertNotIn("fragmented_subject", result["blockers"])
                self.assertIn("foreground_segmentation_uncertain", result["warnings"])
                self.assertEqual(result["metrics"]["foreground_mask_source"], "color_distance")

    def test_same_pale_portrait_with_alpha_has_reliable_connected_silhouette(self):
        output = BytesIO()
        self.portrait_image("transparent").save(output, format="PNG")

        result = assess_model_input_image(output.getvalue())

        self.assertTrue(result["model_input_eligible"], result)
        self.assertEqual(result["metrics"]["foreground_mask_source"], "alpha")
        self.assertGreater(result["metrics"]["largest_component_ratio"], 0.99)
        self.assertNotIn("foreground_segmentation_uncertain", result["warnings"])

    def test_opaque_color_regions_do_not_prove_disconnected_geometry(self):
        image = Image.new("RGB", (256, 256), "white")
        draw = ImageDraw.Draw(image)
        draw.rectangle((24, 48, 104, 220), fill=(190, 80, 50))
        draw.rectangle((152, 48, 232, 220), fill=(60, 100, 200))
        output = BytesIO()
        image.save(output, format="PNG")

        result = assess_model_input_image(output.getvalue())

        self.assertTrue(result["model_input_eligible"], result)
        self.assertIn("foreground_segmentation_uncertain", result["warnings"])
        self.assertNotIn("fragmented_subject", result["blockers"])
        self.assertEqual(result["metrics"]["foreground_mask_source"], "color_distance")

    def test_portrait_rectangular_alpha_cutout_is_blocked_before_3d(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "damaged-portrait.png"
            image = Image.new("RGBA", (128, 192), (255, 255, 255, 0))
            draw = ImageDraw.Draw(image)
            draw.rectangle((20, 10, 110, 180), fill=(190, 80, 50, 255))
            draw.rectangle((70, 50, 90, 80), fill=(0, 0, 0, 0))
            draw.rectangle((20, 64, 70, 65), fill=(0, 0, 0, 0))
            image.save(path)

            result = assess_model_input_image(path, reject_rectangular_cutouts=True)

        self.assertFalse(result["model_input_eligible"])
        self.assertIn("subject_has_rectangular_cutout", result["blockers"])
        self.assertGreaterEqual(result["metrics"]["rectangular_cutout_count"], 1)

    def image_bytes(self, *, split: bool = False) -> bytes:
        output = BytesIO()
        image = Image.new("RGBA", (256, 256), (255, 255, 255, 0))
        draw = ImageDraw.Draw(image)
        if split:
            draw.rectangle((24, 48, 104, 220), fill=(190, 80, 50, 255))
            draw.rectangle((152, 48, 232, 220), fill=(60, 100, 200, 255))
        else:
            draw.rounded_rectangle((48, 24, 208, 232), radius=24, fill=(190, 80, 50, 255))
        image.save(output, format="PNG")
        return output.getvalue()

    def test_portrait_recommends_identity_sketch_and_pet_recommends_figurine_without_remote_service(self):
        portrait = recommend_printable_style(self.image_bytes(), prompt="一张清晰的人像照片")
        pet = recommend_printable_style(self.image_bytes(), prompt="我的宠物猫")

        self.assertEqual(portrait["primary"], "portrait_sketch")
        self.assertEqual(portrait["alternatives"], ["realistic", "cartoon"])
        self.assertEqual(portrait["reason"], "portrait")
        self.assertEqual(pet["primary"], "cartoon")
        self.assertEqual(pet["reason"], "animal")
        self.assertTrue(portrait["local_only"])

    def test_face_like_image_recommends_figurine_without_prompt(self):
        output = BytesIO()
        image = Image.new("RGBA", (256, 320), (255, 255, 255, 0))
        draw = ImageDraw.Draw(image)
        draw.ellipse((82, 28, 174, 126), fill=(35, 28, 25, 255))
        draw.ellipse((94, 44, 162, 120), fill=(228, 178, 145, 255))
        draw.rectangle((70, 112, 186, 286), fill=(238, 238, 232, 255))
        draw.rectangle((112, 112, 144, 150), fill=(228, 178, 145, 255))
        image.save(output, format="PNG")

        result = recommend_printable_style(output.getvalue())

        self.assertEqual(result["subject"], "portrait")
        self.assertEqual(result["primary"], "portrait_sketch")
        self.assertEqual(result["reason"], "portrait")

    def test_large_tan_product_shape_is_not_mistaken_for_portrait(self):
        output = BytesIO()
        image = Image.new("RGBA", (256, 256), (255, 255, 255, 0))
        ImageDraw.Draw(image).rounded_rectangle(
            (42, 38, 214, 222), radius=18, fill=(188, 126, 78, 255)
        )
        image.save(output, format="PNG")

        result = recommend_printable_style(output.getvalue())

        self.assertNotEqual(result["subject"], "portrait")

    def test_non_portrait_categories_have_distinct_recommendations(self):
        image = self.image_bytes()
        cases = {
            "公司 Logo": "ink_relief",
            "一辆复古汽车": "realistic",
            "一座古塔建筑": "realistic",
            "多人街景场景": "diorama",
            "一盆开花植物": "cartoon",
            "透明玻璃和水花": "ink_relief",
        }

        for prompt, expected in cases.items():
            with self.subTest(prompt=prompt):
                result = recommend_printable_style(image, prompt=prompt)
                self.assertEqual(result["primary"], expected)
                self.assertEqual(len(result["alternatives"]), 2)
                self.assertEqual(len({result["primary"], *result["alternatives"]}), 3)

    def test_multiple_disconnected_subjects_recommend_diorama(self):
        result = recommend_printable_style(self.image_bytes(split=True))

        self.assertEqual(result["primary"], "diorama")
        self.assertEqual(result["reason"], "multiple_subjects")

    def test_tiny_reference_recommends_low_poly_fallback(self):
        output = BytesIO()
        image = Image.new("RGBA", (256, 256), (255, 255, 255, 0))
        ImageDraw.Draw(image).rectangle((124, 124, 130, 130), fill=(0, 0, 0, 255))
        image.save(output, format="PNG")

        result = recommend_printable_style(output.getvalue())

        self.assertEqual(result["primary"], "low_poly")
        self.assertEqual(result["reason"], "limited_reference")


if __name__ == "__main__":
    unittest.main()

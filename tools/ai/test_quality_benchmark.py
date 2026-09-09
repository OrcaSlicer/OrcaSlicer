from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.ai.quality_benchmark import benchmark_score, load_candidates
from tools.ai.openai_preprocessor import build_style_preview_prompt
from tools.ai.printable_palette import assign_palette_roles


class QualityBenchmarkTests(unittest.TestCase):
    def test_load_candidates_expands_cases_palettes_and_variants(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "input.png").write_bytes(b"not-opened-by-loader")
            manifest = {
                "palettes": {"p": {"colors": ["#FF0000", "#00FF00", "#0000FF", "#FFFFFF"]}},
                "variants": [
                    {"id": "identity", "instruction_suffix": "Keep identity."},
                    {"id": "print", "instruction_suffix": "Use large regions."},
                ],
                "cases": [{
                    "id": "portrait",
                    "source": "input.png",
                    "instruction": "Create a bust.",
                    "palette_ids": ["p"],
                }],
            }
            path = root / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            candidates = load_candidates(path)
            self.assertEqual([item.candidate_id for item in candidates], [
                "portrait__p__identity", "portrait__p__print",
            ])
            self.assertIn("Keep identity.", candidates[0].instruction)

    def test_manifest_rejects_source_outside_its_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = {
                "palettes": {"p": {"colors": ["#000000", "#FFFFFF"]}},
                "variants": [{"id": "one", "instruction_suffix": ""}],
                "cases": [{"id": "bad", "source": "../escape.png", "instruction": "x"}],
            }
            path = root / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "stay inside"):
                load_candidates(path)

    def test_benchmark_score_penalizes_failed_printability(self) -> None:
        good = benchmark_score({
            "score": 0.2,
            "palette_quality_ok": True,
            "meaningful_subject_color_count": 4,
            "printable_subject_area_ratio": 0.4,
            "largest_subject_component_ratio": 0.98,
        })
        failed = benchmark_score({
            "score": 0.1,
            "palette_quality_ok": False,
            "meaningful_subject_color_count": 1,
            "printable_subject_area_ratio": 0.1,
            "largest_subject_component_ratio": 0.5,
        })
        self.assertLess(good, failed)

    def test_benchmark_score_does_not_penalize_valid_monochrome_subject(self) -> None:
        monochrome = benchmark_score({
            "score": 0.15,
            "palette_quality_ok": True,
            "meaningful_subject_color_count": 1,
            "printable_subject_area_ratio": 0.4,
            "largest_subject_component_ratio": 1.0,
        })
        colorful = benchmark_score({
            "score": 0.15,
            "palette_quality_ok": True,
            "meaningful_subject_color_count": 4,
            "printable_subject_area_ratio": 0.4,
            "largest_subject_component_ratio": 1.0,
        })
        self.assertEqual(monochrome, colorful)

    def test_benchmark_palette_metadata_does_not_limit_generation_colors(self) -> None:
        colors = ("#E83B36", "#2F9E62", "#3178C6", "#F5F3EA")
        assignment = assign_palette_roles(colors)
        prompt = build_style_preview_prompt(
            "Create one toy with a #123456 jacket.", colors, "q_cartoon", palette_roles=assignment.color_by_role
        )
        for role, color in assignment.color_by_role.items():
            self.assertNotIn(f"{role}={color}", prompt)
            self.assertNotIn(color, prompt)
        self.assertIn("#123456 jacket", prompt)
        self.assertIn("There is no printer color palette or color-count limit", prompt)


if __name__ == "__main__":
    unittest.main()

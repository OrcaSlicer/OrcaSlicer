#!/usr/bin/env python3
import math
import tempfile
import unittest
from pathlib import Path
import sys

TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))
from printable_model_quality import ModelQualityThresholds, analyze_printable_obj, write_model_quality_report


def tetrahedron(offset=(0.0, 0.0, 0.0), scale=(10.0, 10.0, 10.0)) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    ox, oy, oz = offset
    sx, sy, sz = scale
    vertices = [(ox, oy, oz), (ox + sx, oy, oz), (ox, oy + sy, oz), (ox, oy, oz + sz)]
    faces = [(1, 3, 2), (1, 2, 4), (1, 4, 3), (2, 3, 4)]
    return vertices, faces


def box(offset=(0.0, 0.0, 0.0), size=(10.0, 10.0, 10.0)) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    ox, oy, oz = offset
    sx, sy, sz = size
    vertices = [
        (ox, oy, oz),
        (ox + sx, oy, oz),
        (ox + sx, oy + sy, oz),
        (ox, oy + sy, oz),
        (ox, oy, oz + sz),
        (ox + sx, oy, oz + sz),
        (ox + sx, oy + sy, oz + sz),
        (ox, oy + sy, oz + sz),
    ]
    faces = [
        (1, 3, 2), (1, 4, 3),
        (5, 6, 7), (5, 7, 8),
        (1, 2, 6), (1, 6, 5),
        (2, 3, 7), (2, 7, 6),
        (3, 4, 8), (3, 8, 7),
        (4, 1, 5), (4, 5, 8),
    ]
    return vertices, faces


def rotate_part(part, y_degrees=37.0, z_degrees=23.0):
    vertices, faces = part
    y_radians = math.radians(y_degrees)
    z_radians = math.radians(z_degrees)
    cy, sy = math.cos(y_radians), math.sin(y_radians)
    cz, sz = math.cos(z_radians), math.sin(z_radians)
    rotated = []
    for x, y, z in vertices:
        x1, z1 = cy * x + sy * z, -sy * x + cy * z
        rotated.append((cz * x1 - sz * y, sz * x1 + cz * y, z1))
    return rotated, faces


def attached_thin_neck(thickness=0.4) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    """Closed 10 mm body with a 5 x 10 mm thin neck in the same component."""
    x_coordinates = (0.0, 10.0, 15.0)
    y_coordinates = (0.0, 10.0)
    z_coordinates = (0.0, 5.0 - thickness * 0.5, 5.0 + thickness * 0.5, 10.0)
    occupied = {(0, 0, 0), (0, 0, 1), (0, 0, 2), (1, 0, 1)}
    vertices: list[tuple[float, float, float]] = []
    vertex_indices: dict[tuple[float, float, float], int] = {}
    faces: list[tuple[int, int, int]] = []

    def vertex_index(point: tuple[float, float, float]) -> int:
        if point not in vertex_indices:
            vertex_indices[point] = len(vertices) + 1
            vertices.append(point)
        return vertex_indices[point]

    side_corners = {
        (-1, 0, 0): ("010", "000", "001", "011"),
        (1, 0, 0): ("100", "110", "111", "101"),
        (0, -1, 0): ("000", "100", "101", "001"),
        (0, 1, 0): ("110", "010", "011", "111"),
        (0, 0, -1): ("000", "010", "110", "100"),
        (0, 0, 1): ("001", "101", "111", "011"),
    }
    for ix, iy, iz in sorted(occupied):
        x0, x1 = x_coordinates[ix], x_coordinates[ix + 1]
        y0, y1 = y_coordinates[iy], y_coordinates[iy + 1]
        z0, z1 = z_coordinates[iz], z_coordinates[iz + 1]
        corners = {
            "000": (x0, y0, z0), "100": (x1, y0, z0),
            "110": (x1, y1, z0), "010": (x0, y1, z0),
            "001": (x0, y0, z1), "101": (x1, y0, z1),
            "111": (x1, y1, z1), "011": (x0, y1, z1),
        }
        for direction, names in side_corners.items():
            neighbor = (ix + direction[0], iy + direction[1], iz + direction[2])
            if neighbor in occupied:
                continue
            quad = tuple(vertex_index(corners[name]) for name in names)
            faces.extend(((quad[0], quad[1], quad[2]), (quad[0], quad[2], quad[3])))
    return vertices, faces


def obj_text(parts) -> str:
    lines = []
    vertex_offset = 0
    for part in parts:
        vertices, faces = part[:2]
        color = part[2] if len(part) > 2 else (1.0, 0.0, 0.0)
        if color is None:
            lines.extend("v {} {} {}".format(*vertex) for vertex in vertices)
        else:
            lines.extend("v {} {} {} {} {} {}".format(*vertex, *color) for vertex in vertices)
        lines.extend("f {} {} {}".format(*(index + vertex_offset for index in face)) for face in faces)
        vertex_offset += len(vertices)
    return "\n".join(lines) + "\n"


def split_face_parts(part):
    """Fixed per-corner OBJ fixture: exact positions with alternating face colors."""
    vertices, faces = part
    return [
        ([vertices[index - 1] for index in face], [(1, 2, 3)],
         (1.0, 0.0, 0.0) if face_index // 2 % 2 == 0 else (0.0, 0.0, 1.0))
        for face_index, face in enumerate(faces)
    ]


class PrintableModelQualityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def analyze(self, content: str, thresholds=None, target_palette=()):
        source = self.root / "model.obj"
        source.write_text(content, encoding="ascii")
        return analyze_printable_obj(source, thresholds, target_palette=target_palette)

    def test_closed_grounded_tetrahedron_passes(self):
        report = self.analyze(obj_text([tetrahedron()]))
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["metrics"]["component_count"], 1)
        self.assertEqual(report["metrics"]["boundary_edges"], 0)
        self.assertFalse(report["metrics"]["repairable_topology"])

    def test_open_mesh_is_rejected(self):
        vertices, faces = tetrahedron()
        report = self.analyze(obj_text([(vertices, faces[:-1])]))
        self.assertEqual(report["status"], "reject")
        self.assertIn("boundary_edges", report["errors"])

    def test_exact_color_seams_preserve_closed_topology_and_corner_colors(self):
        content = obj_text(split_face_parts(box()))
        report = self.analyze(content, target_palette=("#FF0000", "#0000FF"))
        metrics = report["metrics"]

        self.assertEqual(report["status"], "pass")
        self.assertEqual(metrics["boundary_edges"], 0)
        self.assertEqual(metrics["indexed_boundary_edges"], 36)
        self.assertEqual(metrics["exact_seam_edge_pairs"], 18)
        self.assertEqual(metrics["non_manifold_edges"], 0)
        self.assertEqual(metrics["inconsistent_winding_edges"], 0)
        self.assertEqual(metrics["component_count"], 1)
        self.assertTrue(metrics["component_thickness_available"])
        self.assertTrue(metrics["local_thickness_available"])
        self.assertEqual(metrics["vertex_count"], 36)
        self.assertEqual(metrics["face_count"], 12)
        self.assertEqual(metrics["surface_area_mm2"], 600.0)
        self.assertEqual(metrics["printable_color_count"], 2)
        self.assertEqual(metrics["color_region_count"], 2)
        self.assertEqual(
            [item["surface_ratio"] for item in report["evidence"]["target_palette_surface_usage"]],
            [0.5, 0.5],
        )
        self.assertEqual((self.root / "model.obj").read_text(encoding="ascii"), content)

    def test_single_split_triangle_seals_only_its_exact_seams(self):
        vertices, faces = box()
        report = self.analyze(obj_text([(vertices, faces[:-1])] + split_face_parts((vertices, faces[-1:]))))
        self.assertEqual(report["metrics"]["boundary_edges"], 0)
        self.assertEqual(report["metrics"]["component_count"], 1)

    def test_nonuniform_seam_copies_preserve_component_thickness(self):
        vertices, faces = rotate_part(box(size=(20.0, 10.0, 0.4)))
        baseline = self.analyze(obj_text([(vertices, faces)]))
        split = self.analyze(obj_text([(vertices, faces[:-1])] + split_face_parts((vertices, faces[-1:]))))
        self.assertEqual(split["metrics"]["thin_component_count"], 1)
        self.assertEqual(
            split["metrics"]["minimum_component_thickness_mm"],
            baseline["metrics"]["minimum_component_thickness_mm"],
        )

    def test_split_color_seams_do_not_close_a_real_hole(self):
        vertices, faces = box()
        report = self.analyze(obj_text(split_face_parts((vertices, faces[:-1]))))
        self.assertEqual(report["status"], "reject")
        self.assertEqual(report["metrics"]["boundary_edges"], 3)
        self.assertEqual(report["metrics"]["component_count"], 1)
        self.assertFalse(report["metrics"]["local_thickness_available"])

    def test_nearby_seams_are_not_tolerance_welded(self):
        parts = split_face_parts(box())
        vertices, faces, color = parts[-1]
        parts[-1] = ([(x + 1e-7, y, z) for x, y, z in vertices], faces, color)
        report = self.analyze(obj_text(parts))
        self.assertEqual(report["status"], "reject")
        self.assertEqual(report["metrics"]["boundary_edges"], 6)

    def test_split_same_direction_seams_remain_rejected(self):
        vertices, faces = box()
        faces[-1] = tuple(reversed(faces[-1]))
        report = self.analyze(obj_text(split_face_parts((vertices, faces))))
        self.assertEqual(report["status"], "reject")
        self.assertEqual(report["metrics"]["boundary_edges"], 6)
        self.assertEqual(report["metrics"]["inconsistent_winding_edges"], 3)
        self.assertIn("inconsistent_winding_edges", report["errors"])

    def test_split_non_manifold_edges_remain_rejected(self):
        vertices, faces = box()
        report = self.analyze(obj_text(split_face_parts((vertices, faces + faces[:1]))))
        self.assertEqual(report["status"], "reject")
        self.assertEqual(report["metrics"]["boundary_edges"], 9)
        self.assertEqual(report["metrics"]["non_manifold_edges"], 3)
        self.assertIn("non_manifold_edges", report["errors"])

    def test_boundary_seams_cannot_hide_a_preexisting_closed_edge(self):
        vertices, faces = box()
        for extra_faces in (faces[:1], [faces[0], tuple(reversed(faces[0]))]):
            with self.subTest(extra_faces=extra_faces):
                report = self.analyze(obj_text([(vertices, faces)] + split_face_parts((vertices, extra_faces))))
                self.assertEqual(report["status"], "reject")
                self.assertEqual(report["metrics"]["boundary_edges"], len(extra_faces) * 3)
                self.assertEqual(report["metrics"]["non_manifold_edges"], 3)

    def test_exact_point_touch_does_not_join_independent_closed_shells(self):
        shells = [box(), box((10.0, 10.0, 10.0))]
        for split in (False, True):
            with self.subTest(split=split):
                parts = [part for shell in shells for part in split_face_parts(shell)] if split else shells
                report = self.analyze(obj_text(parts))
                self.assertEqual(report["metrics"]["boundary_edges"], 0)
                self.assertEqual(report["metrics"]["component_count"], 2)
                self.assertIn("unwelded_structural_components", report["warnings"])

    def test_exact_edge_touch_does_not_join_already_closed_shells(self):
        report = self.analyze(obj_text([box(), box((10.0, 10.0, 0.0))]))
        self.assertEqual(report["metrics"]["boundary_edges"], 0)
        self.assertEqual(report["metrics"]["component_count"], 2)
        self.assertIn("unwelded_structural_components", report["warnings"])

    def test_small_open_mesh_can_be_deferred_to_a_declared_repair_consumer(self):
        vertices, faces = tetrahedron()
        source = self.root / "repairable.obj"
        source.write_text(obj_text([(vertices, faces[:-1])]), encoding="ascii")

        report = analyze_printable_obj(source, allow_repairable_topology=True)

        self.assertEqual(report["status"], "review")
        self.assertEqual(report["errors"], [])
        self.assertIn("repairable_boundary_edges", report["warnings"])
        self.assertTrue(report["metrics"]["repairable_topology"])

    def test_same_direction_shared_edge_is_rejected(self):
        vertices, faces = tetrahedron()
        faces[1] = tuple(reversed(faces[1]))
        report = self.analyze(obj_text([(vertices, faces)]))
        self.assertEqual(report["status"], "reject")
        self.assertIn("inconsistent_winding_edges", report["errors"])

    def test_degenerate_triangle_is_rejected(self):
        vertices, faces = tetrahedron()
        faces.append((1, 1, 2))
        report = self.analyze(obj_text([(vertices, faces)]))
        self.assertEqual(report["status"], "reject")
        self.assertIn("degenerate_faces", report["errors"])

    def test_tiny_detached_component_requires_review(self):
        thresholds = ModelQualityThresholds(tiny_component_face_ratio=0.6)
        report = self.analyze(
            obj_text([tetrahedron(), tetrahedron((5, 5, 10), (0.01, 0.01, 0.01))]),
            thresholds,
        )
        self.assertEqual(report["status"], "review")
        self.assertIn("tiny_detached_components", report["warnings"])
        self.assertEqual(report["metrics"]["component_count"], 2)

    def test_equal_size_floating_component_requires_review(self):
        report = self.analyze(
            obj_text([
                tetrahedron(),
                tetrahedron((20.0, 0.0, 15.0)),
            ])
        )

        self.assertEqual(report["status"], "review")
        self.assertIn("floating_disconnected_components", report["warnings"])
        self.assertEqual(report["metrics"]["floating_component_count"], 1)
        self.assertGreater(report["metrics"]["minimum_floating_clearance_mm"], 10.0)

    def test_nearby_separate_shell_is_treated_as_supported(self):
        report = self.analyze(
            obj_text([
                tetrahedron(),
                tetrahedron((10.1, 0.0, 5.0)),
            ]),
            ModelQualityThresholds(component_contact_tolerance_mm=0.2),
        )

        self.assertEqual(report["status"], "review")
        self.assertNotIn("floating_disconnected_components", report["warnings"])
        self.assertIn("unwelded_structural_components", report["warnings"])
        self.assertIn("localized_overhang_regions", report["warnings"])
        self.assertEqual(report["metrics"]["floating_component_count"], 0)
        self.assertIsNone(report["metrics"]["minimum_floating_clearance_mm"])

    def test_grounded_nontrivial_shells_require_weld_review(self):
        report = self.analyze(obj_text([box(), box((12.0, 0.0, 0.0))]))

        self.assertEqual(report["status"], "review")
        self.assertEqual(report["metrics"]["structural_component_count"], 2)
        self.assertIn("unwelded_structural_components", report["warnings"])
        self.assertNotIn("floating_disconnected_components", report["warnings"])

    def test_weak_contact_requires_review_without_rejecting_topology(self):
        vertices, faces = tetrahedron(scale=(10.0, 10.0, 10.0))
        vertices[1] = (10.0, 0.0, 1.0)
        vertices[2] = (0.0, 10.0, 1.0)
        report = self.analyze(obj_text([(vertices, faces)]))
        self.assertEqual(report["status"], "review")
        self.assertIn("weak_bed_contact", report["warnings"])

    def test_widely_spaced_point_contacts_use_actual_projected_area(self):
        report = self.analyze(
            obj_text([
                tetrahedron(scale=(0.1, 0.1, 1.0)),
                tetrahedron((100.0, 0.0, 0.0), (0.1, 0.1, 1.0)),
            ])
        )

        self.assertEqual(report["metrics"]["contact_span_ratio"], 1.0)
        self.assertLess(report["metrics"]["bed_contact_area_ratio"], 0.002)
        self.assertIn("weak_bed_contact", report["warnings"])

    def test_wide_flat_base_has_full_projected_contact_area(self):
        report = self.analyze(obj_text([box()]))

        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["metrics"]["bed_contact_area_mm2"], 100.0)
        self.assertEqual(report["metrics"]["bed_contact_area_ratio"], 1.0)

    def test_localized_elevated_overhang_is_reported_before_global_ratio(self):
        report = self.analyze(
            obj_text([
                box(size=(10.0, 10.0, 20.0)),
                box((-5.0, -5.0, 20.0), (20.0, 20.0, 1.0)),
            ])
        )

        self.assertLess(report["metrics"]["downward_surface_ratio"], 0.35)
        self.assertNotIn("high_downward_surface_ratio", report["warnings"])
        self.assertIn("localized_overhang_regions", report["warnings"])
        self.assertEqual(report["metrics"]["significant_overhang_region_count"], 1)
        self.assertGreater(report["metrics"]["lowest_overhang_clearance_mm"], 19.0)

    def test_thin_closed_component_requires_review(self):
        report = self.analyze(obj_text([box(size=(20.0, 10.0, 0.4))]))

        self.assertEqual(report["status"], "review")
        self.assertTrue(report["metrics"]["component_thickness_available"])
        self.assertEqual(report["metrics"]["thin_component_count"], 1)
        self.assertAlmostEqual(report["metrics"]["minimum_component_thickness_mm"], 0.4, places=5)
        self.assertIn("thin_structural_components", report["warnings"])
        self.assertGreater(report["metrics"]["downward_surface_ratio"], 0.35)
        self.assertEqual(report["metrics"]["elevated_downward_surface_ratio"], 0.0)
        self.assertNotIn("high_downward_surface_ratio", report["warnings"])

    def test_thin_component_detection_is_rotation_invariant(self):
        report = self.analyze(obj_text([rotate_part(box(size=(20.0, 10.0, 0.4)))]))

        self.assertTrue(report["metrics"]["component_thickness_available"])
        self.assertEqual(report["metrics"]["thin_component_count"], 1)
        self.assertAlmostEqual(report["metrics"]["minimum_component_thickness_mm"], 0.4, places=5)

    def test_small_chunky_component_is_not_mistaken_for_a_thin_plate(self):
        report = self.analyze(obj_text([box(size=(2.0, 2.0, 2.0))]))

        self.assertEqual(report["status"], "pass")
        self.assertTrue(report["metrics"]["component_thickness_available"])
        self.assertEqual(report["metrics"]["thin_component_count"], 0)
        self.assertAlmostEqual(report["metrics"]["minimum_component_thickness_mm"], 2.0, places=5)

    def test_open_mesh_leaves_component_thickness_unknown(self):
        vertices, faces = box(size=(20.0, 10.0, 0.4))
        source = self.root / "open-thin.obj"
        source.write_text(obj_text([(vertices, faces[:-1])]), encoding="ascii")

        report = analyze_printable_obj(source, allow_repairable_topology=True)

        self.assertFalse(report["metrics"]["component_thickness_available"])
        self.assertEqual(report["metrics"]["thin_component_count"], 0)
        self.assertIsNone(report["metrics"]["minimum_component_thickness_mm"])
        self.assertNotIn("thin_structural_components", report["warnings"])

    def test_attached_thin_neck_requires_local_thickness_review(self):
        report = self.analyze(obj_text([attached_thin_neck()]))

        self.assertEqual(report["gate_version"], "structural-v12")
        self.assertEqual(report["status"], "review")
        self.assertEqual(report["metrics"]["thin_component_count"], 0)
        self.assertTrue(report["metrics"]["local_thickness_available"])
        self.assertGreaterEqual(report["metrics"]["thin_local_surface_sample_count"], 2)
        self.assertAlmostEqual(report["metrics"]["minimum_sampled_local_thickness_mm"], 0.4, places=4)
        self.assertIn("thin_local_wall_regions", report["warnings"])
        evidence = report["evidence"]["thin_local_face_indices"]
        self.assertEqual(evidence, sorted(set(evidence)))
        self.assertGreaterEqual(len(evidence), 2)
        self.assertTrue(all(0 <= index < report["metrics"]["face_count"] for index in evidence))
        self.assertEqual(report["metrics"]["thin_local_region_count"], 1)
        self.assertEqual(report["metrics"]["reported_thin_local_region_count"], 1)
        regions = report["evidence"]["thin_local_regions"]
        self.assertEqual(len(regions), 1)
        self.assertEqual(regions[0]["sample_count"], 4)
        self.assertAlmostEqual(regions[0]["minimum_thickness_mm"], 0.4, places=4)
        self.assertEqual(regions[0]["face_indices"], evidence)

    def test_local_thickness_evidence_is_bounded(self):
        report = self.analyze(
            obj_text([attached_thin_neck()]),
            thresholds=ModelQualityThresholds(local_thickness_evidence_limit=2),
        )

        self.assertEqual(len(report["evidence"]["thin_local_face_indices"]), 2)
        self.assertGreater(report["metrics"]["thin_local_surface_sample_count"], 2)
        self.assertEqual(len(report["evidence"]["thin_local_regions"][0]["face_indices"]), 2)
        self.assertEqual(report["evidence"]["thin_local_regions"][0]["sample_count"], 4)

    def test_distant_local_thickness_hits_form_stably_ordered_regions(self):
        first = attached_thin_neck()
        vertices, faces = attached_thin_neck(0.2)
        second = ([(x + 40.0, y, z) for x, y, z in vertices], faces)
        report = self.analyze(obj_text([first, second]))

        self.assertEqual(report["metrics"]["thin_local_region_count"], 2)
        regions = report["evidence"]["thin_local_regions"]
        self.assertEqual(len(regions), 2)
        self.assertEqual([region["sample_count"] for region in regions], [4, 4])
        self.assertAlmostEqual(regions[0]["minimum_thickness_mm"], 0.2, places=4)
        self.assertAlmostEqual(regions[1]["minimum_thickness_mm"], 0.4, places=4)
        self.assertGreater(regions[0]["representative_face_index"], regions[1]["representative_face_index"])
        self.assertEqual(
            sorted(index for region in regions for index in region["face_indices"]),
            report["evidence"]["thin_local_face_indices"],
        )

    def test_sampled_local_thickness_is_rotation_invariant(self):
        report = self.analyze(obj_text([rotate_part(attached_thin_neck())]))

        self.assertTrue(report["metrics"]["local_thickness_available"])
        self.assertAlmostEqual(report["metrics"]["minimum_sampled_local_thickness_mm"], 0.4, places=4)
        self.assertIn("thin_local_wall_regions", report["warnings"])

    def test_chunky_box_has_no_local_thickness_warning(self):
        report = self.analyze(obj_text([box()]))

        self.assertTrue(report["metrics"]["local_thickness_available"])
        self.assertEqual(report["metrics"]["thin_local_surface_sample_count"], 0)
        self.assertIsNone(report["metrics"]["minimum_sampled_local_thickness_mm"])
        self.assertNotIn("thin_local_wall_regions", report["warnings"])
        self.assertEqual(report["evidence"]["thin_local_face_indices"], [])
        self.assertEqual(report["evidence"]["thin_local_regions"], [])
        self.assertEqual(report["metrics"]["thin_local_region_count"], 0)

    def test_open_mesh_leaves_local_thickness_unknown(self):
        vertices, faces = attached_thin_neck()
        source = self.root / "open-local-thin.obj"
        source.write_text(obj_text([(vertices, faces[:-1])]), encoding="ascii")

        report = analyze_printable_obj(source, allow_repairable_topology=True)

        self.assertFalse(report["metrics"]["local_thickness_available"])
        self.assertEqual(report["metrics"]["local_thickness_sample_count"], 0)
        self.assertEqual(report["metrics"]["thin_local_surface_sample_count"], 0)
        self.assertIsNone(report["metrics"]["minimum_sampled_local_thickness_mm"])
        self.assertNotIn("thin_local_wall_regions", report["warnings"])

    def test_coherent_printable_color_regions_pass(self):
        red_vertices, red_faces = tetrahedron()
        blue_vertices, blue_faces = tetrahedron((20.0, 0.0, 0.0))
        report = self.analyze(
            obj_text([
                (red_vertices, red_faces, (1.0, 0.0, 0.0)),
                (blue_vertices, blue_faces, (0.0, 0.0, 1.0)),
            ])
        )

        self.assertEqual(report["status"], "review")
        self.assertIn("unwelded_structural_components", report["warnings"])
        self.assertTrue(report["metrics"]["has_complete_vertex_colors"])
        self.assertEqual(report["metrics"]["printable_color_count"], 2)
        self.assertEqual(report["metrics"]["color_region_count"], 2)
        self.assertEqual(report["metrics"]["tiny_color_region_count"], 0)

    def test_color_regions_cross_exact_duplicate_vertex_seams(self):
        source = self.root / "seamed.obj"
        source.write_text(
            "\n".join(
                [
                    "v 0 0 0 1 0 0",
                    "v 1 0 0 1 0 0",
                    "v 0 1 0 1 0 0",
                    "v 1 0 0 1 0 0",
                    "v 1 1 0 1 0 0",
                    "v 0 1 0 1 0 0",
                    "f 1 2 3",
                    "f 4 5 6",
                ]
            )
            + "\n",
            encoding="ascii",
        )

        report = analyze_printable_obj(source, allow_repairable_topology=True)

        self.assertTrue(report["metrics"]["has_complete_vertex_colors"])
        self.assertEqual(report["metrics"]["printable_color_count"], 1)
        self.assertEqual(report["metrics"]["color_region_count"], 1)

    def test_tiny_printable_color_island_requires_review(self):
        red_vertices, red_faces = tetrahedron()
        blue_vertices, blue_faces = tetrahedron((5.0, 5.0, 0.0), (0.1, 0.1, 0.1))
        thresholds = ModelQualityThresholds(
            tiny_color_region_face_ratio=0.6,
            tiny_color_region_area_ratio=0.001,
        )
        report = self.analyze(
            obj_text([
                (red_vertices, red_faces, (1.0, 0.0, 0.0)),
                (blue_vertices, blue_faces, (0.0, 0.0, 1.0)),
            ]),
            thresholds,
        )

        self.assertEqual(report["status"], "review")
        self.assertIn("tiny_printable_color_regions", report["warnings"])
        self.assertEqual(report["metrics"]["tiny_color_region_count"], 1)
        self.assertEqual(report["metrics"]["printable_color_count"], 2)

    def test_uncolored_obj_keeps_structural_behavior(self):
        vertices, faces = tetrahedron()
        report = self.analyze(obj_text([(vertices, faces, None)]))

        self.assertEqual(report["status"], "pass")
        self.assertFalse(report["metrics"]["has_complete_vertex_colors"])
        self.assertEqual(report["metrics"]["printable_color_count"], 0)
        self.assertEqual(report["metrics"]["color_region_count"], 0)
        self.assertNotIn("tiny_printable_color_regions", report["warnings"])

    def test_byte_vertex_colors_are_normalized(self):
        red_vertices, red_faces = tetrahedron()
        blue_vertices, blue_faces = tetrahedron((20.0, 0.0, 0.0))
        report = self.analyze(
            obj_text([
                (red_vertices, red_faces, (255.0, 0.0, 0.0)),
                (blue_vertices, blue_faces, (0.0, 0.0, 255.0)),
            ])
        )

        self.assertEqual(report["status"], "review")
        self.assertIn("unwelded_structural_components", report["warnings"])
        self.assertTrue(report["metrics"]["has_complete_vertex_colors"])
        self.assertEqual(report["metrics"]["printable_color_count"], 2)

    def test_incomplete_or_invalid_vertex_colors_disable_color_metrics(self):
        vertices, faces = tetrahedron()
        second_vertices, second_faces = tetrahedron((20.0, 0.0, 0.0))
        cases = (
            obj_text([
                (vertices, faces, (1.0, 0.0, 0.0)),
                (second_vertices, second_faces, None),
            ]),
            obj_text([(vertices, faces, (256.0, 0.0, 0.0))]),
        )
        for content in cases:
            with self.subTest(content=content):
                report = self.analyze(content)
                expected = "review" if report["metrics"]["component_count"] > 1 else "pass"
                self.assertEqual(report["status"], expected)
                self.assertFalse(report["metrics"]["has_complete_vertex_colors"])
                self.assertEqual(report["metrics"]["printable_color_count"], 0)
                self.assertEqual(report["metrics"]["color_region_count"], 0)

    def test_three_meaningful_target_colors_preserve_four_color_palette_signal(self):
        colors = (
            ("#FF0000", (1.0, 0.0, 0.0), (0.0, 0.0, 0.0), (10.0, 10.0, 10.0)),
            ("#00FF00", (0.0, 1.0, 0.0), (20.0, 0.0, 0.0), (10.0, 10.0, 10.0)),
            ("#0000FF", (0.0, 0.0, 1.0), (40.0, 0.0, 0.0), (10.0, 10.0, 10.0)),
            ("#FFFF00", (1.0, 1.0, 0.0), (60.0, 0.0, 0.0), (1.0, 1.0, 1.0)),
        )
        parts = []
        for _, rgb, offset, scale in colors:
            vertices, faces = tetrahedron(offset, scale)
            parts.append((vertices, faces, rgb))
        report = self.analyze(obj_text(parts), target_palette=tuple(color for color, *_ in colors))

        metrics = report["metrics"]
        self.assertTrue(metrics["target_palette_metrics_available"])
        self.assertEqual(metrics["target_palette_color_count"], 4)
        self.assertEqual(metrics["used_target_palette_color_count"], 4)
        self.assertEqual(metrics["meaningful_target_palette_color_count"], 3)
        self.assertEqual(metrics["required_meaningful_target_palette_color_count"], 3)
        self.assertTrue(metrics["target_palette_diversity_ok"])
        self.assertAlmostEqual(metrics["target_palette_surface_coverage_ratio"], 1.0)
        self.assertNotIn("too_few_meaningful_target_palette_colors", report["warnings"])
        usage = report["evidence"]["target_palette_surface_usage"]
        self.assertEqual([entry["color"] for entry in usage], [color for color, *_ in colors])
        self.assertEqual([entry["meaningful"] for entry in usage], [True, True, True, False])

    def test_two_meaningful_target_colors_require_review(self):
        palette = ("#FF0000", "#00FF00", "#0000FF", "#FFFF00")
        parts = [
            (*tetrahedron((0.0, 0.0, 0.0)), (1.0, 0.0, 0.0)),
            (*tetrahedron((20.0, 0.0, 0.0)), (0.0, 1.0, 0.0)),
            (*tetrahedron((40.0, 0.0, 0.0), (1.0, 1.0, 1.0)), (0.0, 0.0, 1.0)),
            (*tetrahedron((50.0, 0.0, 0.0), (1.0, 1.0, 1.0)), (1.0, 1.0, 0.0)),
        ]
        report = self.analyze(obj_text(parts), target_palette=palette)

        self.assertEqual(report["metrics"]["meaningful_target_palette_color_count"], 2)
        self.assertFalse(report["metrics"]["target_palette_diversity_ok"])
        self.assertIn("too_few_meaningful_target_palette_colors", report["warnings"])

    def test_target_palette_is_optional_and_outside_colors_reduce_coverage(self):
        red_vertices, red_faces = tetrahedron()
        green_vertices, green_faces = tetrahedron((20.0, 0.0, 0.0))
        content = obj_text([
            (red_vertices, red_faces, (1.0, 0.0, 0.0)),
            (green_vertices, green_faces, (0.0, 1.0, 0.0)),
        ])

        without_palette = self.analyze(content)
        self.assertFalse(without_palette["metrics"]["target_palette_metrics_available"])
        self.assertEqual(without_palette["evidence"]["target_palette_surface_usage"], [])

        with_palette = self.analyze(content, target_palette=("#FF0000", "#0000FF"))
        self.assertAlmostEqual(with_palette["metrics"]["target_palette_surface_coverage_ratio"], 0.5)
        self.assertIn("colors_outside_target_palette", with_palette["warnings"])

    def test_face_limit_is_a_hard_rejection(self):
        report = self.analyze(obj_text([tetrahedron()]), ModelQualityThresholds(max_faces=3))
        self.assertEqual(report["status"], "reject")
        self.assertEqual(report["errors"], ["too_many_faces"])

    def test_default_face_limit_matches_high_quality_provider_profile(self):
        self.assertEqual(ModelQualityThresholds().max_faces, 2_000_000)

    def test_report_is_written_as_json(self):
        report = self.analyze(obj_text([tetrahedron()]))
        destination = write_model_quality_report(report, self.root / "model-quality.json")
        self.assertTrue(destination.is_file())
        self.assertIn('"gate_version": "structural-v12"', destination.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()

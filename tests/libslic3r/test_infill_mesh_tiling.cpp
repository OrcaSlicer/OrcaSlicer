#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/Fill/InfillMeshTiling.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("Infill cell tiling indices", "[InfillMeshTiling]") {
    // A 4 x 2 x 1 mm rectangular-prism cell at the origin.
    const BoundingBoxf3 cell(Vec3d(0., 0., 0.), Vec3d(4., 2., 1.));

    SECTION("covers the target on the cell's own pitch per axis") {
        const InfillCellTiling t = infill_cell_tiling(cell, BoundingBoxf3(Vec3d(-5., -5., 0.), Vec3d(5., 5., 3.)), 100000);
        REQUIRE_FALSE(t.truncated);
        // x: tiles at -8..8 step 4 -> [-8,-4], [-4,0], [0,4], [4,8] overlap [-5,5] => i = -2..1
        REQUIRE(t.lo.x() == -2); REQUIRE(t.hi.x() == 1);
        // y: pitch 2 over [-5,5] => j = -3..2
        REQUIRE(t.lo.y() == -3); REQUIRE(t.hi.y() == 2);
        // z: pitch 1 over [0,3] => k = 0..2
        REQUIRE(t.lo.z() == 0);  REQUIRE(t.hi.z() == 2);
        REQUIRE(t.count() == 4 * 6 * 3);
    }
    SECTION("a flat axis is not tiled") {
        const BoundingBoxf3 flat(Vec3d(0., 0., 0.), Vec3d(4., 2., 0.));
        const InfillCellTiling t = infill_cell_tiling(flat, BoundingBoxf3(Vec3d(0., 0., 0.), Vec3d(4., 2., 10.)), 100000);
        REQUIRE(t.lo.z() == 0); REQUIRE(t.hi.z() == 0);
        REQUIRE(t.count() == 1);
    }
    SECTION("runaway guard falls back to the single cell") {
        const InfillCellTiling t = infill_cell_tiling(cell, BoundingBoxf3(Vec3d(-500., -500., 0.), Vec3d(500., 500., 500.)), 4096);
        REQUIRE(t.truncated);
        REQUIRE(t.count() == 1);
    }
}

TEST_CASE("Infill lattice from a rotated rectangular cell", "[InfillMeshTiling]") {
    const indexed_triangle_set cell = its_make_cube(4., 2., 1.);
    const BoundingBoxf3 target(Vec3d(-10., -10., 0.), Vec3d(10., 10., 6.));

    SECTION("unrotated: tiles are exact translates and cover the target") {
        InfillCellTiling tiling;
        const indexed_triangle_set lattice = build_infill_lattice(cell, Transform3d::Identity(), target, kMaxInfillTiles, &tiling);
        REQUIRE(lattice.indices.size() == cell.indices.size() * tiling.count());
        const BoundingBoxf3 bb = bounding_box(lattice);
        REQUIRE(bb.min.x() <= target.min.x()); REQUIRE(bb.max.x() >= target.max.x());
        REQUIRE(bb.min.y() <= target.min.y()); REQUIRE(bb.max.y() >= target.max.y());
        REQUIRE(bb.min.z() <= target.min.z()); REQUIRE(bb.max.z() >= target.max.z());
        // Pitch is the cell's own box: 4 x 2 x 1, not a cube.
        REQUIRE(tiling.hi.x() - tiling.lo.x() + 1 == 6);   // 20 / 4, ends not on the pitch
        REQUIRE(tiling.hi.y() - tiling.lo.y() + 1 == 10);  // 20 / 2, exact
        REQUIRE(tiling.hi.z() - tiling.lo.z() + 1 == 6);   // 6 / 1, exact
    }

    SECTION("rotated cell: the lattice rotates as a whole and still covers the target") {
        const Transform3d cell_to_target = Transform3d(Eigen::AngleAxisd(M_PI / 4., Vec3d::UnitZ()));
        InfillCellTiling tiling;
        const indexed_triangle_set lattice = build_infill_lattice(cell, cell_to_target, target, kMaxInfillTiles, &tiling);
        REQUIRE_FALSE(lattice.indices.empty());
        REQUIRE_FALSE(tiling.truncated);
        const BoundingBoxf3 bb = bounding_box(lattice);
        REQUIRE(bb.min.x() <= target.min.x() + 1e-6); REQUIRE(bb.max.x() >= target.max.x() - 1e-6);
        REQUIRE(bb.min.y() <= target.min.y() + 1e-6); REQUIRE(bb.max.y() >= target.max.y() - 1e-6);
        // Every tile is the cell rotated: a cell edge along local X maps to the 45 degree diagonal.
        const Vec3f e = lattice.vertices[1] - lattice.vertices[0];
        const Vec3f e0 = cell.vertices[1] - cell.vertices[0];
        const Vec3f expected = cell_to_target.cast<float>().linear() * e0;
        REQUIRE_THAT((e - expected).norm(), WithinAbs(0., 1e-4));
    }

    SECTION("empty cell yields an empty lattice") {
        REQUIRE(build_infill_lattice(indexed_triangle_set(), Transform3d::Identity(), target, kMaxInfillTiles).indices.empty());
    }
}

TEST_CASE("Open-surface cells slice to chained lines", "[InfillMeshTiling]") {
    // Two vertical 10 x 10 mm sheets meeting along the edge x = 10: an L in plan view.
    indexed_triangle_set sheets;
    sheets.vertices = { {0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, {10.f, 0.f, 10.f}, {0.f, 0.f, 10.f},
                        {10.f, 10.f, 0.f}, {10.f, 10.f, 10.f} };
    sheets.indices  = { {0, 1, 2}, {0, 2, 3}, {1, 4, 5}, {1, 5, 2} };
    REQUIRE(its_is_open_surface(sheets));
    REQUIRE_FALSE(its_is_open_surface(its_make_cube(1., 1., 1.)));

    SECTION("one polyline per layer through the sheets, joined at the shared edge") {
        const std::vector<Polylines> slices = slice_sheet_mesh(sheets, { 2.f, 5.f, 9.5f }, scaled<coord_t>(0.002));
        REQUIRE(slices.size() == 3);
        for (const Polylines &layer : slices) {
            REQUIRE(layer.size() == 1);
            REQUIRE_THAT(unscaled<double>(layer.front().length()), Catch::Matchers::WithinAbs(20., 1e-3));
            // Two triangles per sheet leave a collinear point mid-sheet: corners plus those.
            REQUIRE(layer.front().points.size() >= 3);
            REQUIRE(layer.front().points.size() <= 5);
        }
    }
    SECTION("planes through vertices and outside the mesh") {
        const std::vector<Polylines> slices = slice_sheet_mesh(sheets, { 0.f, 10.f, 12.f }, scaled<coord_t>(0.002));
        // z = 0: vertices on the plane count as above, the sheets are entirely above -> nothing.
        REQUIRE(slices[0].empty());
        // z = 10: the top edges lie in the plane, the sheets are below and on it -> the edges.
        double len = 0.;
        for (const Polyline &pl : slices[1]) len += unscaled<double>(pl.length());
        REQUIRE_THAT(len, Catch::Matchers::WithinAbs(20., 1e-3));
        REQUIRE(slices[2].empty());
    }
    SECTION("a tilted sheet") {
        indexed_triangle_set tilted;
        tilted.vertices = { {0.f, 0.f, 0.f}, {10.f, 0.f, 10.f}, {10.f, 10.f, 10.f}, {0.f, 10.f, 0.f} };
        tilted.indices  = { {0, 1, 2}, {0, 2, 3} };
        const std::vector<Polylines> slices = slice_sheet_mesh(tilted, { 2.5f }, scaled<coord_t>(0.002));
        REQUIRE(slices.front().size() == 1);
        // The section line at z = 2.5 is x = 2.5, y from 0 to 10.
        REQUIRE_THAT(unscaled<double>(slices.front().front().length()), Catch::Matchers::WithinAbs(10., 1e-3));
        for (const Point &p : slices.front().front().points)
            REQUIRE_THAT(unscaled<double>(p.x()), Catch::Matchers::WithinAbs(2.5, 1e-3));
    }
}

TEST_CASE("Endpoint chaining", "[InfillMeshTiling]") {
    Polylines pls;
    pls.emplace_back(Points{ Point(0, 0), Point(100, 0) });
    pls.emplace_back(Points{ Point(200, 0), Point(101, 1) });   // reversed, one unit off
    pls.emplace_back(Points{ Point(200, 1), Point(300, 0) });
    pls.emplace_back(Points{ Point(1000, 1000), Point(1100, 1000) });
    join_polylines_at_endpoints(pls, 2);
    REQUIRE(pls.size() == 2);
    const Polyline &joined = pls.front().points.size() > 2 ? pls.front() : pls.back();
    REQUIRE(joined.points.size() == 4);
    // A loop closes on itself and stops there.
    Polylines square;
    square.emplace_back(Points{ Point(0, 0), Point(10, 0) });
    square.emplace_back(Points{ Point(10, 0), Point(10, 10) });
    square.emplace_back(Points{ Point(10, 10), Point(0, 10) });
    square.emplace_back(Points{ Point(0, 10), Point(0, 0) });
    join_polylines_at_endpoints(square, 1);
    REQUIRE(square.size() == 1);
    REQUIRE(square.front().points.size() == 5);
    REQUIRE(square.front().points.front() == square.front().points.back());
}

TEST_CASE("Infill mesh cell from a ModelObject", "[InfillMeshTiling]") {
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)), ModelVolumeType::MODEL_PART);

    SECTION("no infill mesh volume") {
        Transform3d m;
        REQUIRE(infill_mesh_cell(*object, "", m).indices.empty());
    }
    SECTION("a mesh file stands in for the volume") {
        Transform3d m;
        const std::string path = (boost::filesystem::path(TEST_DATA_DIR) / "20mm_cube.obj").string();
        const indexed_triangle_set cell = infill_mesh_cell(*object, path, m);
        REQUIRE(cell.indices.size() == 12);
        REQUIRE(m.isApprox(Transform3d::Identity()));
        REQUIRE(infill_mesh_cell(*object, "/nonexistent/cell.stl", m).indices.empty());
    }
    SECTION("the first infill mesh volume's frame is the cell frame") {
        ModelVolume *a = object->add_volume(TriangleMesh(its_make_cube(2., 2., 2.)), ModelVolumeType::INFILL_MESH);
        a->set_offset(Vec3d(1., 2., 3.));
        a->set_rotation(Vec3d(0., 0., M_PI / 6.));
        ModelVolume *b = object->add_volume(TriangleMesh(its_make_cube(1., 1., 1.)), ModelVolumeType::INFILL_MESH);
        b->set_offset(Vec3d(5., 0., 0.));
        Transform3d cell_to_object;
        const indexed_triangle_set cell = infill_mesh_cell(*object, "", cell_to_object);
        REQUIRE(cell.indices.size() == 24);
        REQUIRE(cell_to_object.isApprox(a->get_matrix()));
        // b's vertices are expressed in a's frame: mapping them back through a's matrix gives
        // b's world placement.
        const Vec3f b_vertex_in_cell = cell.vertices[8];
        const Vec3d back = a->get_matrix() * b_vertex_in_cell.cast<double>();
        const Vec3d expected = b->get_matrix() * Vec3d(b->mesh().its.vertices[0].cast<double>());
        REQUIRE_THAT((back - expected).norm(), WithinAbs(0., 1e-6));
    }
}

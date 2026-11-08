#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/CutPlaneSegmentation.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// Build a one-object model with the given part meshes and a single default instance.
// Planes are authored in the object-local frame == the meshes' own coordinates
// (add_volume() recenters the mesh but compensates through the volume matrix,
// which derive_cut_segments applies).
static ModelObject *make_object(Model &model, std::initializer_list<TriangleMesh> parts)
{
    ModelObject *mo = model.add_object();
    mo->name = "test_object";
    for (const TriangleMesh &part : parts)
        mo->add_volume(part);
    mo->add_instance();
    return mo;
}

static CutPlane plane_at(const Vec3d &point, const Vec3d &normal)
{
    CutPlane cp;
    cp.point  = point;
    cp.normal = normal;
    return cp;
}

TEST_CASE("B/C angles and build normal round-trip", "[cutplanes]") {
    // Representative normals in every quadrant plus the degenerate poles.
    const std::vector<Vec3d> normals = {
        Vec3d(0, 0, 1), Vec3d(0, 0, -1),
        Vec3d(1, 0, 0), Vec3d(0, 1, 0), Vec3d(-1, 0, 0), Vec3d(0, -1, 0),
        Vec3d(1, 1, 1).normalized(), Vec3d(-1, 2, 0.5).normalized(),
        Vec3d(0.3, -0.4, -0.6).normalized(), Vec3d(-2, -3, 4).normalized(),
    };
    for (const Vec3d &n : normals) {
        double b = 0., c = 0.;
        Geometry::bc_from_build_normal(n, b, c);
        CHECK(b <= 1e-9);      // b in [-180, 0]
        CHECK(b >= -180. - 1e-9);
        const Vec3d back = Geometry::build_normal_from_bc(b, c);
        for (int i = 0; i < 3; ++ i)
            CHECK_THAT(back[i], WithinAbs(n[i], 1e-9));
        // The layer rotation must map the build normal onto +Z.
        const Vec3d rotated = Geometry::layer_rotation_matrix(b, c) * n;
        CHECK_THAT(rotated.x(), WithinAbs(0., 1e-9));
        CHECK_THAT(rotated.y(), WithinAbs(0., 1e-9));
        CHECK_THAT(rotated.z(), WithinAbs(1., 1e-9));
    }
}

TEST_CASE("Cube with one mid plane derives two segments", "[cutplanes]") {
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 10), Vec3d::UnitZ()));

    const auto segs = derive_cut_segments(*mo);
    REQUIRE(segs.size() == 2);

    const CutSegmentDescriptor &root = segs[0];
    CHECK(root.index == 0);
    CHECK(root.parent == -1);
    CHECK(root.base_plane == -1);
    CHECK(root.is_root());

    const CutSegmentDescriptor &upper = segs[1];
    CHECK(upper.index == 1);
    CHECK(upper.parent == 0);
    CHECK(upper.base_plane == 0);
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(upper.build_normal[i], WithinAbs(Vec3d::UnitZ()[i], 1e-12));

    // One clip each, opposite half-spaces of the same plane.
    REQUIRE(root.clips.size() == 1);
    REQUIRE(upper.clips.size() == 1);
    CHECK_THAT(root.clips[0].normal.dot(Vec3d(0, 0, 1)), WithinAbs(1., 1e-12));   // keep z <= 10
    CHECK_THAT(root.clips[0].offset, WithinAbs(10., 1e-9));
    CHECK_THAT(upper.clips[0].normal.dot(Vec3d(0, 0, 1)), WithinAbs(-1., 1e-12)); // keep z >= 10
    CHECK_THAT(upper.clips[0].offset, WithinAbs(-10., 1e-9));
}

TEST_CASE("Cube with two stacked planes derives a three-segment chain", "[cutplanes]") {
    // The middle band contains no mesh vertices (the cube has corner vertices only) — this
    // exercises the edge-midpoint cell sampling.
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 6),  Vec3d::UnitZ()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 14), Vec3d::UnitZ()));

    const auto segs = derive_cut_segments(*mo);
    REQUIRE(segs.size() == 3);
    CHECK(segs[0].is_root());

    const CutSegmentDescriptor &mid = segs[1];
    CHECK(mid.parent == 0);
    CHECK(mid.base_plane == 0);      // bounded below by the z=6 plane
    const CutSegmentDescriptor &top = segs[2];
    CHECK(top.parent == 1);          // chain: root -> mid -> top
    CHECK(top.base_plane == 1);      // bounded below by the z=14 plane

    // Root and middle band are clipped by both planes; the top chunk peels last, so only its own
    // plane clips it -- it owns everything beyond z=14 regardless of the earlier cut.
    CHECK(segs[0].clips.size() == 2);
    CHECK(segs[1].clips.size() == 2);
    CHECK(segs[2].clips.size() == 1);
}

TEST_CASE("A 3x3x3 grid of planes peels into seven chunks conserving volume", "[cutplanes]") {
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    for (const Vec3d normal : { Vec3d::UnitX(), Vec3d::UnitY(), Vec3d::UnitZ() })
        for (double offset : { 5., 15. })
            mo->cut_planes.planes.push_back(plane_at(normal * offset, normal));

    // Six planes cut the cube into a 3x3x3 arrangement of 27 cells, but sections follow the peel
    // semantics: one chunk per plane plus the root. The interior cells (including the central one,
    // which touches no original mesh vertex or edge -- materialized occupancy discovers it) merge
    // into those chunks, and the whole volume is conserved across the partition.
    const auto segs = derive_cut_segments(*mo);
    REQUIRE(segs.size() == 7);
    CHECK(segs[0].is_root());

    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const auto pieces = segment_meshes(cube, segs);
    double total = 0.;
    for (const indexed_triangle_set &piece : pieces)
        total += its_volume(piece);
    CHECK_THAT(total, WithinAbs(double(its_volume(cube)), 1.0));
}

TEST_CASE("Bed-side normalization is derived per instance transform", "[cutplanes]") {
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10., 10., 10.), Vec3d::UnitZ()));

    const auto upright = derive_cut_segments(*mo, Transform3d::Identity());
    Transform3d upside_down = Transform3d::Identity();
    upside_down.rotate(Eigen::AngleAxisd(M_PI, Vec3d::UnitX()));
    const auto inverted = derive_cut_segments(*mo, upside_down);

    REQUIRE(upright.size() == 2);
    REQUIRE(inverted.size() == 2);
    CHECK_THAT(upright[0].clips[0].normal.z(), WithinAbs(1., 1e-12));
    CHECK_THAT(inverted[0].clips[0].normal.z(), WithinAbs(-1., 1e-12));
    CHECK_THAT(upright[1].build_normal.z(), WithinAbs(1., 1e-12));
    CHECK_THAT(inverted[1].build_normal.z(), WithinAbs(-1., 1e-12));
}

TEST_CASE("U shape with two planes peels into three sections", "[cutplanes]") {
    // Base bar [0,30]x[0,10]x[0,10]; tower A [0,10] footprint, tower B [20,30] footprint,
    // both rising to z=30. Plane 0 (x=15) splits the bar; plane 1 (z=10) splits both towers
    // from the bar. Cells: root (left bar), right bar {far0}, tower A {far1}, tower B {far0,far1}.
    // Tower B sits on plane 1 (it never touches x=15), so plane 1 peels after plane 0 and BOTH
    // towers merge into plane 1's chunk: three sections, printed root -> right bar -> towers.
    TriangleMesh bar = make_cube(30., 10., 10.);
    TriangleMesh tower_a = make_cube(10., 10., 20.);
    tower_a.translate(0.f, 0.f, 10.f);
    TriangleMesh tower_b = make_cube(10., 10., 20.);
    tower_b.translate(20.f, 0.f, 10.f);

    Model model;
    ModelObject *mo = make_object(model, { bar, tower_a, tower_b });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(15, 5, 5),  Vec3d::UnitX()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(15, 5, 10), Vec3d::UnitZ()));

    CutSegmentationStatus             status = CutSegmentationStatus::StackInactive;
    std::vector<indexed_triangle_set> pieces;
    const auto segs = derive_cut_segments(merged_model_part_mesh(*mo), mo->cut_planes.planes,
                                          Transform3d::Identity(), 0., /*user_ordered*/ false,
                                          &status, &pieces, mo->name);
    REQUIRE(status == CutSegmentationStatus::Ok);
    REQUIRE(segs.size() == 3);
    CHECK(segs[0].is_root());

    CHECK(segs[1].parent == 0);
    CHECK(segs[1].base_plane == 0);
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs[1].build_normal[i], WithinAbs(Vec3d::UnitX()[i], 1e-12));

    CHECK(segs[2].base_plane == 1);
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs[2].build_normal[i], WithinAbs(Vec3d::UnitZ()[i], 1e-12));

    // The towers chunk hands back one mesh holding both shells, and the partition conserves the
    // model's volume: bar 30x10x10 plus two 10x10x20 towers.
    REQUIRE(pieces.size() == 3);
    double total = 0.;
    for (const indexed_triangle_set &piece : pieces) {
        CHECK(! piece.indices.empty());
        total += its_volume(piece);
    }
    CHECK_THAT(total, WithinAbs(30. * 10. * 10. + 2. * 10. * 10. * 20., 0.5));
}

TEST_CASE("Two crossing planes peel into three sections seated correctly", "[cutplanes]") {
    // Box 30x10x20. Plane 0: x=5 (normal +X), plane 1: z=15 (normal +Z). The planes cross inside
    // the box, so the arrangement has four cells -- but the far-of-both wedge (x>5, z>15) SITS on
    // plane 1 (a 25x10 cut face, against a 10x5 sliver of plane 0), so plane 1 peels after plane 0
    // and the wedge merges into plane 1's chunk: three sections, not four. The merged top chunk
    // builds along +Z and rests on the far-of-plane-0 segment, where most of its floor lies.
    Model model;
    ModelObject *mo = make_object(model, { make_cube(30., 10., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(5, 5, 10),  Vec3d::UnitX()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(15, 5, 15), Vec3d::UnitZ()));

    const auto segs = derive_cut_segments(*mo);
    REQUIRE(segs.size() == 3);
    CHECK(segs[0].is_root());

    CHECK(segs[1].parent == 0);
    CHECK(segs[1].base_plane == 0);
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs[1].build_normal[i], WithinAbs(Vec3d::UnitX()[i], 1e-12));

    CHECK(segs[2].parent == 1);
    CHECK(segs[2].base_plane == 1);
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs[2].build_normal[i], WithinAbs(Vec3d::UnitZ()[i], 1e-12));

    // The merged chunk peels last, so only its own plane clips it; the x-chunk keeps a ceiling
    // against the later plane. The x-chunk also stands on the build plate while building along +X,
    // so it carries the plate as a third, Bed clip; the merged top chunk starts at z = 15 and does not.
    REQUIRE(segs[2].clips.size() == 1);
    CHECK(segs[2].clips[0].plane_index == 1);
    CHECK(segs[2].clips[0].role == CutClipRole::Floor);
    CHECK(! segs[2].has_bed_clip());
    REQUIRE(segs[1].clips.size() == 3);
    CHECK(segs[1].clips[0].role == CutClipRole::Floor);
    CHECK(segs[1].clips[0].plane_index == 0);
    CHECK(segs[1].clips[1].role == CutClipRole::Ceiling);
    CHECK(segs[1].clips[1].plane_index == 1);
    CHECK(segs[1].clips[2].role == CutClipRole::Bed);
    CHECK(segs[1].clips[2].plane_index == -1);
}

TEST_CASE("Peel order follows the seats, not the authoring order", "[cutplanes]") {
    // The same crossing geometry with the planes authored in the opposite order: plane 0 is now
    // z=15, plane 1 is x=5. The wedge still sits on the z=15 face, so the z plane must peel last
    // even though it was authored first -- the x chunk prints first and keeps a ceiling against
    // the z plane, and the merged top chunk parents onto it.
    Model model;
    ModelObject *mo = make_object(model, { make_cube(30., 10., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(15, 5, 15), Vec3d::UnitZ()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(5, 5, 10),  Vec3d::UnitX()));

    const auto segs = derive_cut_segments(*mo);
    REQUIRE(segs.size() == 3);
    CHECK(segs[0].is_root());

    CHECK(segs[1].parent == 0);
    CHECK(segs[1].base_plane == 1);      // the x=5 cut prints first
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs[1].build_normal[i], WithinAbs(Vec3d::UnitX()[i], 1e-12));

    CHECK(segs[2].parent == 1);
    CHECK(segs[2].base_plane == 0);      // the z=15 cut peels last and takes the wedge
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs[2].build_normal[i], WithinAbs(Vec3d::UnitZ()[i], 1e-12));
}

TEST_CASE("A user-ordered stack overrides the seat rule", "[cutplanes]") {
    // Same crossing geometry the seat rule resolves to "z plane peels last". With
    // user_ordered set, the LIST ORDER alone decides: the first plane in the list peels/prints
    // last and claims the wedge -- so reordering the planes visibly changes the partition, which
    // is exactly what the automatic seat constraints would otherwise pin.
    Model model;
    ModelObject *mo = make_object(model, { make_cube(30., 10., 20.) });
    mo->cut_planes.enabled      = true;
    mo->cut_planes.user_ordered = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(5, 5, 10),  Vec3d::UnitX()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(15, 5, 15), Vec3d::UnitZ()));

    // First in the list is the x=5 plane: it peels last, so the wedge builds along +X -- the
    // opposite of the seat rule's choice.
    const auto segs = derive_cut_segments(*mo);
    REQUIRE(segs.size() == 3);
    CHECK(segs[1].base_plane == 1);
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs[1].build_normal[i], WithinAbs(Vec3d::UnitZ()[i], 1e-12));
    CHECK(segs[2].base_plane == 0);
    CHECK(segs[2].parent == 0);          // the big far-of-x piece seats the chunk on the root
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs[2].build_normal[i], WithinAbs(Vec3d::UnitX()[i], 1e-12));

    // Reordering the list flips the outcome back to the seat rule's partition.
    std::swap(mo->cut_planes.planes[0], mo->cut_planes.planes[1]);
    const auto swapped = derive_cut_segments(*mo);
    REQUIRE(swapped.size() == 3);
    CHECK(swapped[1].base_plane == 1);   // now the x=5 plane (index 1) prints first
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(swapped[1].build_normal[i], WithinAbs(Vec3d::UnitX()[i], 1e-12));
    CHECK(swapped[2].base_plane == 0);   // the z=15 plane heads the list: prints last, owns the wedge
    CHECK(swapped[2].parent == 1);
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(swapped[2].build_normal[i], WithinAbs(Vec3d::UnitZ()[i], 1e-12));
}

TEST_CASE("Duplicate planes are ignored", "[cutplanes]") {
    // The same geometric plane authored twice -- exactly, then again with the opposite facing --
    // behaves as a single cut: the first authored plane wins. Without dedup the exact duplicate
    // can shave a float-thin sliver on the second cut, and the anti-parallel pair of vertical
    // planes leaves no all-near root cell (feature inert).
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 8), Vec3d::UnitZ()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(5, 5, 8),  Vec3d::UnitZ())); // same plane, other anchor
    const auto segs = derive_cut_segments(*mo, Transform3d::Identity(), 0.2);
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].is_root());
    CHECK(segs[1].base_plane == 0);

    // Anti-parallel duplicate of a vertical plane: same geometric plane, opposite facing.
    mo->cut_planes.planes.clear();
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 10), Vec3d::UnitX()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 3, 3),   -Vec3d::UnitX()));
    const auto segs2 = derive_cut_segments(*mo, Transform3d::Identity(), 0.2);
    REQUIRE(segs2.size() == 2);
    CHECK(segs2[0].is_root());
    CHECK(segs2[1].base_plane == 0);
    // The first authored facing wins: +X is the far (printed-second) side.
    CHECK_THAT(segs2[1].build_normal[0], WithinAbs(1., 1e-12));
}

TEST_CASE("Nearly coplanar planes thinner than a layer disable segmentation", "[cutplanes]") {
    // Two parallel planes a hair apart make a sliver segment. With a 0.2 mm first layer the whole
    // feature goes inert for the object (dropping just the sliver cell would lose material);
    // without the bound the sliver derives as a segment of its own.
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 8.),   Vec3d::UnitZ()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 8.05), Vec3d::UnitZ()));
    CutSegmentationStatus status = CutSegmentationStatus::Ok;
    CHECK(derive_cut_segments(*mo, Transform3d::Identity(), 0.2, &status).empty());
    CHECK(status == CutSegmentationStatus::SegmentTooThin);
    CHECK(derive_cut_segments(*mo, Transform3d::Identity(), 0., &status).size() == 3);
    CHECK(status == CutSegmentationStatus::Ok);
}

TEST_CASE("Planes that do not split the mesh are inert", "[cutplanes]") {
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 50), Vec3d::UnitZ())); // above the mesh

    CHECK(derive_cut_segments(*mo).empty());

    // Disabled stack is inert even with a splitting plane.
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 10), Vec3d::UnitZ()));
    mo->cut_planes.enabled = false;
    CHECK(derive_cut_segments(*mo).empty());

    // A disabled plane is skipped individually.
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes[1].enabled = false;
    CHECK(derive_cut_segments(*mo).empty());
    mo->cut_planes.planes[1].enabled = true;
    CHECK(derive_cut_segments(*mo).size() == 2);
}

TEST_CASE("Flipped authored normals give the identical segmentation", "[cutplanes]") {
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 10), Vec3d::UnitZ()));
    const auto segs_up = derive_cut_segments(*mo);

    mo->cut_planes.planes[0].normal = -Vec3d::UnitZ(); // author pointed it at the bed
    const auto segs_down = derive_cut_segments(*mo);

    REQUIRE(segs_up.size() == 2);
    REQUIRE(segs_down.size() == 2);
    // Same tree, same clips, and the default build normal is normalized away from the bed
    // in both cases.
    CHECK(segs_up == segs_down);
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs_down[1].build_normal[i], WithinAbs(Vec3d::UnitZ()[i], 1e-12));
}

TEST_CASE("Build-normal override propagates to the far segment", "[cutplanes]") {
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    CutPlane cp = plane_at(Vec3d(10, 10, 10), Vec3d::UnitZ());
    cp.build_normal     = Vec3d(1., 0., 1.); // intentionally unnormalized
    cp.has_build_normal = true;
    mo->cut_planes.planes.push_back(cp);

    const auto segs = derive_cut_segments(*mo);
    REQUIRE(segs.size() == 2);
    const Vec3d expected = Vec3d(1., 0., 1.).normalized();
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs[1].build_normal[i], WithinAbs(expected[i], 1e-12));
}

TEST_CASE("Clip roles follow the side of the plane the segment is on", "[cutplanes]") {
    // Same three-segment chain as above, read through the support-facing view: a segment rests on
    // its own plane (Floor) and sits below every later-peeling plane (Ceiling); planes peeled
    // earlier do not clip it. Look clips up by the authored plane they came from.
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 6),  Vec3d::UnitZ()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 14), Vec3d::UnitZ()));

    const auto segs = derive_cut_segments(*mo);
    REQUIRE(segs.size() == 3);

    auto role_of = [](const CutSegmentDescriptor &seg, int plane_index) {
        for (const CutSegmentClip &clip : seg.clips)
            if (clip.plane_index == plane_index)
                return clip.role;
        FAIL("segment carries no clip for plane " << plane_index);
        return CutClipRole::Ceiling;
    };

    // Root: below both planes.
    CHECK(role_of(segs[0], 0) == CutClipRole::Ceiling);
    CHECK(role_of(segs[0], 1) == CutClipRole::Ceiling);
    // Middle band: rests on the lower plane, sits below the upper one.
    CHECK(role_of(segs[1], 0) == CutClipRole::Floor);
    CHECK(role_of(segs[1], 1) == CutClipRole::Ceiling);
    // Top: peels last, so it carries a single clip -- the plane it rests on. The earlier z=6
    // plane no longer bounds it at all.
    REQUIRE(segs[2].clips.size() == 1);
    CHECK(segs[2].clips[0].plane_index == 1);
    CHECK(role_of(segs[2], 1) == CutClipRole::Floor);

    // A Floor clip is the negated plane, so its offset is the negated plane offset.
    CHECK_THAT(segs[2].clips[0].normal.dot(Vec3d(0, 0, 1)), WithinAbs(-1., 1e-12));
    CHECK_THAT(segs[2].clips[0].offset, WithinAbs(-14., 1e-9));
}

TEST_CASE("clip_expolygons_by_half_plane clips a square in all four axis directions", "[cutplanes]") {
    // 10x10 mm square at the origin.
    Polygon square({ Point::new_scale(0., 0.), Point::new_scale(10., 0.),
                     Point::new_scale(10., 10.), Point::new_scale(0., 10.) });
    const ExPolygons src = { ExPolygon(square) };
    const BoundingBox bbox = get_extents(square);
    const double full = area(src);

    struct Case { Vec2d ab; double c; double frac; };
    const std::vector<Case> cases = {
        { Vec2d( 1,  0),  5.0, 0.5 },  // keep x <= 5
        { Vec2d(-1,  0), -5.0, 0.5 },  // keep x >= 5
        { Vec2d( 0,  1),  2.5, 0.25 }, // keep y <= 2.5
        { Vec2d( 0, -1), -2.5, 0.75 }, // keep y >= 2.5
        { Vec2d( 1,  0), 20.0, 1.0 },  // keep-all
        { Vec2d( 1,  0), -5.0, 0.0 },  // drop-all
    };
    for (const Case &tc : cases) {
        ExPolygons clipped = clip_expolygons_by_half_plane(ExPolygons(src), tc.ab, tc.c, bbox);
        CHECK_THAT(area(clipped) / full, WithinAbs(tc.frac, 1e-6));
    }
    // Non-unit-length (a,b) must behave identically to the normalized form.
    ExPolygons clipped = clip_expolygons_by_half_plane(ExPolygons(src), Vec2d(2, 0), 10.0, bbox);
    CHECK_THAT(area(clipped) / full, WithinAbs(0.5, 1e-6));
}

TEST_CASE("segment_meshes conserves volume across the partition", "[cutplanes]") {
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 6),  Vec3d::UnitZ()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 14), Vec3d(0., 0.2, 1.).normalized()));

    const auto segs = derive_cut_segments(*mo);
    REQUIRE(segs.size() == 3);

    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const auto pieces = segment_meshes(cube, segs);
    REQUIRE(pieces.size() == segs.size());

    double total = 0.;
    for (const auto &piece : pieces) {
        CHECK(! piece.indices.empty());
        total += its_volume(piece);
    }
    CHECK_THAT(total, WithinAbs(double(its_volume(cube)), 0.5));
}


TEST_CASE("Derivation hands back the pieces it cut", "[cutplanes]") {
    // The chunk preview renders these meshes. They must be the partition the descriptors describe:
    // one non-empty piece per segment, every vertex inside its own segment's clips, and the whole
    // original volume accounted for. Reconstructing them separately (segment_meshes) cuts each
    // physical plane twice, once per side in a different rotated frame, so its pieces only
    // approximately agree with each other -- this path cannot.
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 6),  Vec3d::UnitZ()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 14), Vec3d(0., 0.2, 1.).normalized()));

    CutSegmentationStatus             status = CutSegmentationStatus::StackInactive;
    std::vector<indexed_triangle_set> pieces;
    const auto segs = derive_cut_segments(*mo, Transform3d::Identity(), 0., &status, &pieces);
    REQUIRE(status == CutSegmentationStatus::Ok);
    REQUIRE(segs.size() == 3);
    REQUIRE(pieces.size() == segs.size());

    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    double total = 0.;
    for (size_t i = 0; i < pieces.size(); ++ i) {
        CHECK(! pieces[i].indices.empty());
        total += its_volume(pieces[i]);
        // Every vertex must satisfy every clip of its own segment: nothing of segment i may stick
        // out past a cut plane into a neighbour, which is what shows up as one section's colour
        // bleeding across the plane.
        for (const stl_vertex &v : pieces[i].vertices) {
            const Vec3d p = v.cast<double>();
            for (const CutSegmentClip &clip : segs[i].clips)
                CHECK(clip.normal.dot(p) - clip.offset <= 1e-3);
        }
    }
    // Tighter than the segment_meshes test's 0.5 mm^3 on the same 8000 mm^3 cube.
    CHECK_THAT(total, WithinAbs(double(its_volume(cube)), 1e-2));
}

TEST_CASE("A part with its own build direction becomes its own segment", "[cutplanes]") {
    // Two cubes side by side; the right one is told to build along +X (its own machine angles).
    TriangleMesh left  = make_cube(10., 10., 10.);
    TriangleMesh right = make_cube(10., 10., 10.);
    right.translate(20.f, 0.f, 0.f);

    Model model;
    ModelObject *mo       = make_object(model, { left, right });
    ModelVolume *directed = mo->volumes[1];
    double b = 0., c = 0.;
    Geometry::bc_from_build_normal(Vec3d::UnitX(), b, c);
    directed->config.set("layer_rotation_b", b);
    directed->config.set("layer_rotation_c", c);
    REQUIRE(model_volume_has_build_direction(*directed));
    REQUIRE(! model_volume_has_build_direction(*mo->volumes[0]));
    REQUIRE(object_has_directed_parts(*mo));

    // No planes at all: the undirected part is the root, the directed one a part segment.
    CutSegmentationStatus             status = CutSegmentationStatus::StackInactive;
    std::vector<indexed_triangle_set> pieces;
    auto segs = derive_cut_segments(*mo, Transform3d::Identity(), 0., &status, &pieces);
    REQUIRE(status == CutSegmentationStatus::Ok);
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].is_root());
    CHECK(! segs[0].is_part());
    CHECK(! segs[1].is_root());
    CHECK(segs[1].is_part());
    CHECK(segs[1].part_volume == directed->id());
    // A part segment has no plane clips. This one stands on the build plate while building along
    // +X, so its only clip is the plate itself.
    REQUIRE(segs[1].clips.size() == 1);
    CHECK(segs[1].clips[0].role == CutClipRole::Bed);
    for (int i = 0; i < 3; ++ i)
        CHECK_THAT(segs[1].build_normal[i], WithinAbs(Vec3d::UnitX()[i], 1e-9));
    // Extent along the build axis (+X): the right cube spans x = 20..30.
    CHECK_THAT(segs[1].build_z_min, WithinAbs(20., 1e-6));
    CHECK_THAT(segs[1].build_z_max, WithinAbs(30., 1e-6));
    REQUIRE(pieces.size() == 2);
    CHECK_THAT(its_volume(pieces[0]), WithinAbs(1000., 0.5));
    CHECK_THAT(its_volume(pieces[1]), WithinAbs(1000., 0.5));

    // Ownership: the root keeps the undirected part, the part segment exactly its part.
    CHECK(segment_includes_volume(&segs[0], *mo->volumes[0]));
    CHECK(! segment_includes_volume(&segs[0], *directed));
    CHECK(segment_includes_volume(&segs[1], *directed));
    CHECK(! segment_includes_volume(&segs[1], *mo->volumes[0]));
    CHECK(segment_includes_volume(nullptr, *directed));

    // With a plane through the undirected part, the partition ignores the directed part and the
    // part segment comes after the plane segments.
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(5, 5, 5), Vec3d::UnitZ()));
    segs = derive_cut_segments(*mo, Transform3d::Identity(), 0., &status, &pieces);
    REQUIRE(status == CutSegmentationStatus::Ok);
    REQUIRE(segs.size() == 3);
    CHECK(segs[0].is_root());
    CHECK(segs[1].base_plane == 0);
    CHECK(segs[2].is_part());
    CHECK(segs[2].part_volume == directed->id());
    CHECK(segs[2].index == 2);
    REQUIRE(pieces.size() == 3);
    CHECK_THAT(its_volume(pieces[0]) + its_volume(pieces[1]), WithinAbs(1000., 0.5));
    CHECK_THAT(its_volume(pieces[2]), WithinAbs(1000., 0.5));
}

TEST_CASE("Explicit-plane overload matches the ModelObject one", "[cutplanes]") {
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 8), Vec3d::UnitZ()));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 15), Vec3d(0.1, 0., 1.).normalized()));

    const auto from_object = derive_cut_segments(*mo);
    const auto from_planes = derive_cut_segments(merged_model_part_mesh(*mo), mo->cut_planes.planes,
                                                 Transform3d::Identity(), 0.);
    REQUIRE(from_object.size() == from_planes.size());
    for (size_t i = 0; i < from_object.size(); ++ i)
        CHECK(from_object[i] == from_planes[i]);
}

TEST_CASE("A provisional plane previews what committing it would produce", "[cutplanes]") {
    // The chunk preview derives from the stored planes with the plane under the gizmo's grabbers
    // substituted in (or appended, in free mode). That preview must be the partition the user gets
    // when they press "Add cut plane" -- otherwise the colours shift the moment they commit.
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 6), Vec3d::UnitZ()));

    const CutPlane authoring = plane_at(Vec3d(10, 10, 14), Vec3d(0., 0.2, 1.).normalized());

    std::vector<CutPlane> provisional = mo->cut_planes.planes;
    provisional.push_back(authoring);
    const auto previewed = derive_cut_segments(merged_model_part_mesh(*mo), provisional,
                                               Transform3d::Identity(), 0.);

    mo->cut_planes.planes.push_back(authoring); // what add_plane() does
    const auto committed = derive_cut_segments(*mo);

    REQUIRE(previewed.size() == committed.size());
    REQUIRE(previewed.size() == 3);
    for (size_t i = 0; i < previewed.size(); ++ i)
        CHECK(previewed[i] == committed[i]);
}

TEST_CASE("Derivation status explains an empty partition", "[cutplanes]") {
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 20., 20.) });
    CutSegmentationStatus status = CutSegmentationStatus::Ok;

    // Stack off.
    CHECK(derive_cut_segments(*mo, Transform3d::Identity(), 0., &status).empty());
    CHECK(status == CutSegmentationStatus::StackInactive);

    // A plane that misses the mesh entirely divides nothing.
    mo->cut_planes.enabled = true;
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 100), Vec3d::UnitZ()));
    CHECK(derive_cut_segments(*mo, Transform3d::Identity(), 0., &status).empty());
    CHECK(status == CutSegmentationStatus::NoSplit);

    // A real cut, but one segment would be thinner than the first layer.
    mo->cut_planes.planes.clear();
    mo->cut_planes.planes.push_back(plane_at(Vec3d(10, 10, 19.95), Vec3d::UnitZ()));
    CHECK(derive_cut_segments(*mo, Transform3d::Identity(), 0.2, &status).empty());
    CHECK(status == CutSegmentationStatus::SegmentTooThin);
    // ... and the same stack derives fine without that bound.
    CHECK(derive_cut_segments(*mo, Transform3d::Identity(), 0., &status).size() == 2);
    CHECK(status == CutSegmentationStatus::Ok);
}

TEST_CASE("A tilted segment that reaches the build plate carries the plate as a clip", "[cutplanes]") {
    // Box 20x10x20 with a steep plane, 70 degrees from vertical, meeting the plate along x = 14.
    // The far chunk builds along that tilted normal, so nothing else would stop its layers at the
    // plate. Sunk 2 mm (an un-seated import), it even owns material under the plate.
    Model model;
    ModelObject *mo = make_object(model, { make_cube(20., 10., 20.) });
    mo->cut_planes.enabled = true;
    const Vec3d n(std::cos(Geometry::deg2rad(20.)), 0., std::sin(Geometry::deg2rad(20.)));
    mo->cut_planes.planes.push_back(plane_at(Vec3d(14., 5., 2.), n));   // world (14, 5, 0) once sunk

    Transform3d sunk = Transform3d::Identity();
    sunk.translation() = Vec3d(0., 0., -2.);
    const auto segs = derive_cut_segments(*mo, sunk);
    REQUIRE(segs.size() == 2);
    CHECK(! segs[0].has_bed_clip());   // the root stops at the plate through slice_z_base()
    REQUIRE(segs[1].has_bed_clip());
    const CutSegmentClip &bed = segs[1].clips.back();
    CHECK(bed.role == CutClipRole::Bed);
    CHECK(bed.plane_index == -1);
    // world z = local z - 2 must stay >= 0:  (-z_hat) . p - (-2) <= 0.
    CHECK_THAT(bed.normal.z(), WithinAbs(-1., 1e-12));
    CHECK_THAT(bed.offset, WithinAbs(-2., 1e-12));

    // The same box lifted 5 mm clear of the plate gets no plate clip at all.
    Transform3d lifted = Transform3d::Identity();
    lifted.translation() = Vec3d(0., 0., 5.);
    const auto clear = derive_cut_segments(*mo, lifted);
    REQUIRE(clear.size() == 2);
    CHECK(! clear[1].has_bed_clip());
}

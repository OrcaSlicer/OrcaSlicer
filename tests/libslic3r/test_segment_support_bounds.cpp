#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/CutPlaneSegmentation.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Support/SegmentSupportBounds.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cmath>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// A clip is stored as "normal . p - offset <= 0". Authoring them the way support generation reads
// them -- the half-space keeping  n . p >= d, with n pointing INTO the segment -- keeps the tests
// readable and matches how derive_cut_segments negates a far-side plane.
static CutSegmentClip clip_keeping_above(const Vec3d &n, double d, CutClipRole role, int plane_index = 0)
{
    return CutSegmentClip{ -n, -d, plane_index, role };
}

static CutSegmentDescriptor non_root_segment(std::vector<CutSegmentClip> clips)
{
    CutSegmentDescriptor seg;
    seg.index      = 1;
    seg.parent     = 0;
    seg.base_plane = 0;
    seg.clips      = std::move(clips);
    return seg;
}

static bool region_contains(const Polygons &region, double x, double y)
{
    return contains(region, Point::new_scale(x, y));
}

TEST_CASE("A segment with no bounds is inactive", "[cutplanes][supports]") {
    // Nothing to clip against: every caller must fall through to stock behavior.
    const SegmentSupportBounds bounds(non_root_segment({}), Transform3d::Identity());
    CHECK_FALSE(bounds.is_active());
    CHECK(bounds.valid_region_polygon(1.).empty());
    CHECK_THAT(bounds.floor_z(Point::new_scale(0., 0.)), WithinAbs(0., 1e-12));
}

TEST_CASE("The root segment rests on the build plate, raised by the bed clearance", "[cutplanes][supports]") {
    CutSegmentDescriptor root;   // index 0, parent -1: rests on the plate, no cut faces below it
    root.index = 0;

    SECTION("Object sitting on the plate") {
        const SegmentSupportBounds bounds(root, Transform3d::Identity(), 0.2);
        REQUIRE(bounds.is_active());
        // Support may descend to a first-layer height above the plate, not onto it.
        CHECK_THAT(bounds.floor_z(Point::new_scale(30., -10.)), WithinAbs(0.2, 1e-12));
        CHECK(bounds.valid_region_polygon(0.1).empty());
        CHECK(region_contains(bounds.valid_region_polygon(0.3), 30., -10.));
    }

    SECTION("Object floating above the plate") {
        // build_z_min is where the segment's own lowest material sits, which slicing puts at
        // slice z = 0. The plate stays a real 5 mm below it rather than following the model up.
        root.build_z_min = 5.;
        const SegmentSupportBounds bounds(root, Transform3d::Identity());
        CHECK_THAT(bounds.floor_z(Point::new_scale(0., 0.)), WithinAbs(-5., 1e-12));
        CHECK(region_contains(bounds.valid_region_polygon(-4.), 0., 0.));
        CHECK(bounds.valid_region_polygon(-6.).empty());
    }
}

TEST_CASE("Floors bear load, ceilings only clip", "[cutplanes][supports]") {
    // A band segment: resting on a cut face at z = 0, sitting below a later segment's face at z = 10.
    const SegmentSupportBounds bounds(non_root_segment({
            clip_keeping_above(Vec3d::UnitZ(), 0., CutClipRole::Floor, 0),
            clip_keeping_above(- Vec3d::UnitZ(), -10., CutClipRole::Ceiling, 1) }),
        Transform3d::Identity());
    REQUIRE(bounds.is_active());

    // Both bounds clip the valid region.
    CHECK(region_contains(bounds.valid_region_polygon(5.), 12., -4.));
    CHECK(bounds.valid_region_polygon(-1.).empty());
    CHECK(bounds.valid_region_polygon(11.).empty());

    // Only the floor bears load: were the ceiling counted, the highest bound here would be 10.
    CHECK_THAT(bounds.floor_z(Point::new_scale(12., -4.)), WithinAbs(0., 1e-12));
}

TEST_CASE("Clips carry through the segment's slice frame", "[cutplanes][supports]") {
    // A tilted base plane in object-local coordinates. The segment's slice frame rotates its build
    // normal onto +Z, so the same plane must come out horizontal in the slice frame -- that is the
    // whole point of slicing a segment in its own frame.
    const Vec3d  build_normal = Vec3d(1., 0., 1.).normalized();
    const Vec3d  plane_point(3., -2., 4.);
    const double d = build_normal.dot(plane_point);

    const Eigen::Quaterniond rot = Eigen::Quaterniond::FromTwoVectors(build_normal, Vec3d::UnitZ());
    Transform3d              to_slice = Transform3d::Identity();
    to_slice.linear()      = rot.toRotationMatrix();
    to_slice.translation() = Vec3d(7., -3., -2.);   // XY centering + the z rebase, as slicing applies

    CutSegmentDescriptor seg = non_root_segment({ clip_keeping_above(build_normal, d, CutClipRole::Floor) });
    seg.build_normal = build_normal;
    const SegmentSupportBounds bounds(seg, to_slice);
    REQUIRE(bounds.is_active());

    // Horizontal in the slice frame: the same floor height everywhere, at the transformed plane.
    const double expected_z = (to_slice * plane_point).z();
    CHECK_THAT(bounds.floor_z(Point::new_scale(0., 0.)),     WithinAbs(expected_z, 1e-9));
    CHECK_THAT(bounds.floor_z(Point::new_scale(40., -25.)),  WithinAbs(expected_z, 1e-9));
    CHECK(bounds.valid_region_polygon(expected_z - 0.5).empty());
    CHECK(region_contains(bounds.valid_region_polygon(expected_z + 0.5), 40., -25.));

    // Sanity: without the frame the very same clip is a steep plane, not a floor at one height.
    const SegmentSupportBounds unrotated(seg, Transform3d::Identity());
    CHECK_THAT(unrotated.floor_z(Point::new_scale(0., 0.)), WithinAbs(d * std::sqrt(2.), 1e-9));
    CHECK(unrotated.floor_z(Point::new_scale(40., 0.)) < unrotated.floor_z(Point::new_scale(0., 0.)));
}

TEST_CASE("A vertical floor clips sideways and offers no height to descend to", "[cutplanes][supports]") {
    // Reachable when a plane is (near) parallel to the build direction: it bounds the segment in XY
    // but there is no "down" to it. floor_z must report 0 rather than an unbounded descent.
    const SegmentSupportBounds bounds(
        non_root_segment({ clip_keeping_above(Vec3d::UnitX(), 5., CutClipRole::Floor) }),
        Transform3d::Identity());
    REQUIRE(bounds.is_active());

    CHECK_THAT(bounds.floor_z(Point::new_scale(0., 0.)), WithinAbs(0., 1e-12));
    const Polygons valid = bounds.valid_region_polygon(3.);
    CHECK(region_contains(valid, 10., 0.));
    CHECK_FALSE(region_contains(valid, 0., 0.));
}

TEST_CASE("Bounds built from a derived segment agree with its cut planes", "[cutplanes][supports]") {
    Model        model;
    ModelObject *mo = model.add_object();
    mo->name = "test_object";
    mo->add_volume(make_cube(20., 20., 20.));
    mo->add_instance();
    mo->cut_planes.enabled = true;
    CutPlane lower, upper;
    lower.point  = Vec3d(10, 10, 6);
    lower.normal = Vec3d::UnitZ();
    upper.point  = Vec3d(10, 10, 14);
    upper.normal = Vec3d::UnitZ();
    mo->cut_planes.planes = { lower, upper };

    const auto segs = derive_cut_segments(*mo);
    REQUIRE(segs.size() == 3);
    const CutSegmentDescriptor &mid = segs[1];

    // The middle band builds along +Z, so its slice frame is the rebase that puts its own lowest
    // material at slice z = 0 -- exactly what PrintObject::update_slice_frame() applies.
    Transform3d to_slice = Transform3d::Identity();
    to_slice.translation() = Vec3d(0., 0., - mid.build_z_min);
    const SegmentSupportBounds bounds(mid, to_slice);
    REQUIRE(bounds.is_active());

    // Rests on the z = 6 cut face, which the rebase puts at slice z = 0; the z = 14 face above is a
    // ceiling and must not raise the floor.
    CHECK_THAT(bounds.floor_z(Point::new_scale(10., 10.)), WithinAbs(0., 1e-9));
    CHECK(region_contains(bounds.valid_region_polygon(4.), 10., 10.));
    CHECK(bounds.valid_region_polygon(-1.).empty());
    CHECK(bounds.valid_region_polygon(9.).empty());   // 8 mm band: 6..14 rebased to 0..8
}

TEST_CASE("The root's first support layer survives the bed plane raised by its own height", "[SegmentSupportBounds]")
{
    // The bed plane is raised by bed_clearance so support rests one first-layer height above the
    // plate, and the first support layer is printed at exactly that height. The root's build_z_min
    // is measured on the mesh and carries a few microns of noise below zero, which used to put the
    // plane a hair ABOVE the first layer and drop that layer entirely.
    CutSegmentDescriptor root;
    root.index       = 0;
    root.parent      = -1;
    root.build_z_min = -2e-6;
    root.build_z_max = 20.;
    const double first_layer = 0.2;
    const SegmentSupportBounds bounds(root, Transform3d::Identity(), first_layer);
    REQUIRE(bounds.is_active());
    CHECK(! bounds.valid_region_polygon(first_layer).empty());
    CHECK(  bounds.valid_region_polygon(first_layer - 0.1).empty());
}

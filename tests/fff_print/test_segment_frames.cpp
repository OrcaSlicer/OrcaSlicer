#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Support/SegmentSupportBounds.hpp"

#include "test_helpers.hpp"

#include <cmath>
#include <limits>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// Frame invariants for declarative cut-plane segments.
//
// Every segment slices in its OWN frame: PrintObject::object_to_slice() maps object-local
// coordinates to the exact centered coordinates handed to the slicer, and slice_to_object() is its
// true inverse. Support cascading maps geometry BETWEEN segments, which is only correct if those
// two are authoritative -- hence these tests pin them directly rather than inferring them from
// exported G-code, where a frame error is indistinguishable from a generator bug.

namespace {

// A cube carrying one cut plane. `volume_shift` moves the mesh away from the object origin so the
// object's raw bounding-box centre -- and therefore PrintObject::m_center_offset -- is far from
// zero, which is the precondition for the shared-centering defect exercised below.
struct SegmentedCube
{
    Slic3r::Print print;
    Model         model;

    SegmentedCube(const Vec3d &plane_normal,
                  const Vec3d &volume_shift  = Vec3d::Zero(),
                  const Vec3d &scale         = Vec3d::Ones(),
                  const Vec3d &mirror        = Vec3d::Ones(),
                  double       rotation_z    = 0.,
                  const std::vector<std::pair<std::string, std::string>> &extra_config = {})
    {
        ModelObject *object = model.add_object();
        object->name = "frame_cube";
        ModelVolume *cube = object->add_volume(Test::cube(20.),
                                               ModelVolumeType::MODEL_PART, false);
        cube->set_offset(volume_shift);

        ModelInstance *instance = object->add_instance();
        instance->set_offset(Vec3d(120., 120., 0.));
        instance->set_scaling_factor(scale);
        instance->set_mirror(mirror);
        instance->set_rotation(Vec3d(0., 0., rotation_z));

        CutPlaneStack stack;
        stack.enabled = true;
        CutPlane cp;
        // Through the middle of the cube, in the volume's own coordinates.
        cp.point  = volume_shift + Vec3d(10., 10., 10.);
        cp.normal = plane_normal;
        stack.planes.push_back(cp);
        object->cut_planes = stack;
        // ensure_on_bed() does nothing for an object whose auto_drop is clear, and the cube would
        // then sit wherever the instance transform left it. Under the mirrored variant that is
        // 20 mm UNDER the plate, which is not what this fixture means by "a cube" and not what its
        // first-layer assertions are about.
        object->auto_drop = true;
        object->ensure_on_bed();

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_nonplanar",   "1" },
            { "print_sequence",     "by group" },
            { "layer_change_gcode", "G92 E0" },
        });
        for (const auto &[key, value] : extra_config)
            config.set_deserialize_strict(key, value);
        print.apply(model, config);
    }

    // Frames are populated during slicing, and update_slicing_parameters/update_slice_frame are
    // private, so drive the real pipeline.
    void process() { print.process(); }
};

// Points spread through the object-local cube, used as frame probes.
std::vector<Vec3d> probe_points(const Vec3d &shift)
{
    std::vector<Vec3d> pts;
    for (double x : { 1., 10., 19. })
        for (double y : { 1., 10., 19. })
            for (double z : { 1., 10., 19. })
                pts.push_back(shift + Vec3d(x, y, z));
    return pts;
}

// Which PrintObject is the root is a property of the derived partition, not of emission order --
// under a mirrored instance "down" is not where a reader would guess. Ask, don't assume.
const PrintObject *root_of(const Slic3r::Print &print)
{
    for (const PrintObject *object : print.objects())
        if (object->is_segment() && object->is_root_segment())
            return object;
    return nullptr;
}

const PrintObject *non_root_of(const Slic3r::Print &print)
{
    for (const PrintObject *object : print.objects())
        if (object->is_segment() && ! object->is_root_segment())
            return object;
    return nullptr;
}

} // namespace

SCENARIO("Segment slice frames are self-consistent", "[cutplanes][frames]") {
    GIVEN("a cube cut by a tilted plane, its mesh far from the object origin") {
        SegmentedCube fixture(Vec3d(0.3, 0.1, 1.).normalized(), Vec3d(40., -25., 0.));
        REQUIRE(fixture.print.objects().size() == 2);
        fixture.process();

        const PrintObject *root  = root_of(fixture.print);
        const PrintObject *child = non_root_of(fixture.print);
        REQUIRE(root  != nullptr);
        REQUIRE(child != nullptr);

        THEN("the centre offset is genuinely non-zero, so these tests are not vacuous") {
            // Without this, the composition defect below cannot manifest and every assertion in
            // this scenario would hold trivially.
            const Vec2d c(unscale<double>(child->center_offset().x()),
                          unscale<double>(child->center_offset().y()));
            INFO("center offset = (" << c.x() << ", " << c.y() << ")");
            CHECK(c.norm() > 1.);
        }

        THEN("object_to_slice and slice_to_object are exact inverses") {
            for (const PrintObject *object : fixture.print.objects()) {
                const Transform3d round_trip = object->slice_to_object() * object->object_to_slice();
                INFO("segment " << object->segment_index());
                for (int r = 0; r < 4; ++ r)
                    for (int c = 0; c < 4; ++ c)
                        CHECK_THAT(round_trip(r, c),
                                   WithinAbs(Transform3d::Identity()(r, c), 1e-9));
            }
        }

        THEN("the child's base plane is horizontal at slice z = 0 in the child's own frame") {
            // The child builds along its base plane's normal, so the whole cut face projects to a
            // single height, and the rebase puts that height at zero. Both halves matter: a
            // rotation error tilts the face, an offset error moves it off zero.
            const Vec3d n = Vec3d(0.3, 0.1, 1.).normalized();
            const Vec3d on_plane = Vec3d(40., -25., 0.) + Vec3d(10., 10., 10.);
            for (double u : { -6., 0., 6. })
                for (double v : { -6., 0., 6. }) {
                    // Two in-plane directions, so every probe lies exactly on the cut plane.
                    const Vec3d t1 = n.cross(Vec3d::UnitX()).normalized();
                    const Vec3d t2 = n.cross(t1).normalized();
                    const Vec3d p  = on_plane + u * t1 + v * t2;
                    INFO("cut-plane probe u=" << u << " v=" << v);
                    CHECK_THAT((child->object_to_slice() * p).z(), WithinAbs(0., 1e-6));
                }
        }

        THEN("every segment's first object layer sits at its configured first-layer height") {
            for (const PrintObject *object : fixture.print.objects()) {
                INFO("segment " << object->segment_index());
                REQUIRE(! object->layers().empty());
                CHECK_THAT(object->layers().front()->print_z,
                           WithinAbs(object->slicing_parameters().first_object_layer_height, 1e-6));
            }
        }
    }
}

SCENARIO("Child-to-parent transfer must compose the authoritative frames", "[cutplanes][frames]") {
    // The design's §1 regression. A support footprint extracted in a child segment's frame and
    // handed to its parent must be mapped with
    //     parent.object_to_slice() * child.slice_to_object()
    // and NOT with parent.post_trafo() * child.post_trafo().inverse(). The shared XY centering
    // translation does not cancel across a relative rotation, so the shortcut is wrong by
    // (L - I) * c -- routinely tens of millimetres, and silently plausible-looking.
    GIVEN("a tilted cut with a mesh far from the object origin") {
        const Vec3d shift(40., -25., 0.);
        SegmentedCube fixture(Vec3d(0.35, 0.15, 1.).normalized(), shift);
        REQUIRE(fixture.print.objects().size() == 2);
        fixture.process();

        const PrintObject *parent = root_of(fixture.print);
        const PrintObject *child  = non_root_of(fixture.print);
        REQUIRE(parent != nullptr);
        REQUIRE(child  != nullptr);

        const Transform3d authoritative = parent->object_to_slice() * child->slice_to_object();
        const Transform3d shortcut      = parent->post_trafo() * child->post_trafo().inverse();

        THEN("the authoritative composition agrees with mapping through object-local space") {
            // Ground truth: both segments are views of ONE object, so a shared object-local point
            // has a known position in each segment's frame. Any correct child->parent map must
            // reproduce it.
            for (const Vec3d &p : probe_points(shift)) {
                const Vec3d in_child     = child->object_to_slice() * p;
                const Vec3d expected     = parent->object_to_slice() * p;
                const Vec3d via_composed = authoritative * in_child;
                INFO("probe " << p.x() << "," << p.y() << "," << p.z());
                CHECK_THAT((via_composed - expected).norm(), WithinAbs(0., 1e-6));
            }
        }

        THEN("the post_trafo shortcut is measurably wrong, so the test above has teeth") {
            double worst = 0.;
            for (const Vec3d &p : probe_points(shift)) {
                const Vec3d in_child  = child->object_to_slice() * p;
                const Vec3d expected  = parent->object_to_slice() * p;
                worst = std::max(worst, (shortcut * in_child - expected).norm());
            }
            INFO("worst-case error of parent.post_trafo() * child.post_trafo().inverse(): "
                 << worst << " mm");
            CHECK(worst > 1.);
        }
    }
}

SCENARIO("Segment frames survive awkward instance transforms", "[cutplanes][frames]") {
    // Rotation, non-uniform scale and mirroring all reach the frames through the instance matrix;
    // normals must travel by the inverse transpose for the cut face to stay horizontal in the
    // child's frame.
    struct Variant { const char *name; Vec3d scale; Vec3d mirror; double rot_z; };
    const std::vector<Variant> variants = {
        { "rotated 40 deg about Z",  Vec3d::Ones(),            Vec3d::Ones(),             0.698 },
        { "non-uniform scale",       Vec3d(1.3, 0.7, 1.15),    Vec3d::Ones(),             0.    },
        { "mirrored in X",           Vec3d::Ones(),            Vec3d(-1., 1., 1.),        0.    },
        { "mirrored and scaled",     Vec3d(0.8, 1.25, 1.),     Vec3d(1., -1., 1.),        0.35  },
    };

    for (const Variant &v : variants) {
        DYNAMIC_SECTION("instance " << v.name) {
            const Vec3d shift(30., -18., 0.);
            SegmentedCube fixture(Vec3d(0.3, 0.2, 1.).normalized(), shift, v.scale, v.mirror, v.rot_z);
            REQUIRE(fixture.print.objects().size() == 2);
            fixture.process();

            const PrintObject *parent = root_of(fixture.print);
            const PrintObject *child  = non_root_of(fixture.print);
            REQUIRE(parent != nullptr);
            REQUIRE(child  != nullptr);

            // Round-trip.
            const Transform3d round_trip = child->slice_to_object() * child->object_to_slice();
            for (int r = 0; r < 4; ++ r)
                for (int c = 0; c < 4; ++ c)
                    CHECK_THAT(round_trip(r, c), WithinAbs(Transform3d::Identity()(r, c), 1e-9));

            // Child->parent transfer still exact.
            const Transform3d authoritative = parent->object_to_slice() * child->slice_to_object();
            for (const Vec3d &p : probe_points(shift)) {
                const Vec3d expected = parent->object_to_slice() * p;
                CHECK_THAT((authoritative * (child->object_to_slice() * p) - expected).norm(),
                           WithinAbs(0., 1e-6));
            }

            // First layer still at the configured height, in every segment.
            for (const PrintObject *object : fixture.print.objects()) {
                INFO("segment " << object->segment_index());
                REQUIRE(! object->layers().empty());
                CHECK_THAT(object->layers().front()->print_z,
                           WithinAbs(object->slicing_parameters().first_object_layer_height, 1e-6));
            }
        }
    }
}

SCENARIO("SegmentSupportBounds built from a real PrintObject", "[cutplanes][frames][supports]") {
    // The unit tests construct descriptors and frames by hand. This one takes both from the
    // production pipeline, so a drift between what Print::apply derives and what the slicer uses
    // would surface here and nowhere else.
    struct Variant { const char *name; Vec3d scale; Vec3d mirror; };
    const std::vector<Variant> variants = {
        { "plain",       Vec3d::Ones(),          Vec3d::Ones()      },
        { "scaled",      Vec3d(1.2, 0.85, 1.1),  Vec3d::Ones()      },
        { "mirrored",    Vec3d::Ones(),          Vec3d(-1., 1., 1.) },
    };

    for (const Variant &v : variants) {
        DYNAMIC_SECTION("instance " << v.name) {
            const Vec3d shift(25., -30., 0.);
            SegmentedCube fixture(Vec3d(0.25, 0.15, 1.).normalized(), shift, v.scale, v.mirror);
            REQUIRE(fixture.print.objects().size() == 2);
            fixture.process();

            const PrintObject *root  = root_of(fixture.print);
            const PrintObject *child = non_root_of(fixture.print);
            REQUIRE(root  != nullptr);
            REQUIRE(child != nullptr);

            const SegmentSupportBounds child_bounds(*child);
            REQUIRE(child_bounds.is_active());

            // The child rests on its base plane, which its own frame makes horizontal at z = 0.
            // Support may therefore descend to 0 and no further, anywhere in XY. The tolerance is
            // 1e-4 mm, not 1e-6: floor_z takes its XY through scaled coordinates, so a probe far
            // from the origin carries ~1e-6 mm of unscale round-off.
            for (double x : { -20., 0., 20. })
                for (double y : { -20., 0., 20. }) {
                    INFO("floor probe " << x << "," << y);
                    CHECK_THAT(child_bounds.floor_z(Point::new_scale(x, y)), WithinAbs(0., 1e-4));
                }
            CHECK(child_bounds.valid_region_polygon(-1.).empty());
            CHECK(! child_bounds.valid_region_polygon(1.).empty());

            // The root sits below the same plane: that plane is its ceiling, so it must clip but
            // must not become a floor. The root's floor is the BUILD PLATE, which in the root's
            // own frame sits at -build_z_min -- that is the contract, and it is what to assert.
            // (Asserting a literal 0 instead would silently fold in "the fixture rests on the
            // bed", which is a property of the placement, not of the bounds.)
            const SegmentSupportBounds root_bounds(*root);
            REQUIRE(root_bounds.is_active());
            INFO("root build_z_min = " << root->segment()->build_z_min);
            CHECK_THAT(root_bounds.floor_z(Point::new_scale(0., 0.)),
                       WithinAbs(- root->segment()->build_z_min, 1e-4));
            CHECK(root_bounds.valid_region_polygon(- root->segment()->build_z_min - 1.).empty());

        }
    }
}

TEST_CASE("A belt root segment slices all of its material and every segment's support stays above the belt", "[cutplanes][frames][belt]") {
    // On a belt the root builds along the tilted gantry normal, projected from the instance origin,
    // so part of any object has a negative build-axis coordinate without being sunk at all. Reading
    // that as "under the plate" (the flat-bed sinking rule) left every layer on the belt side of the
    // instance origin unsliced: the leading half of the part next to the belt went missing.
    const double layer_height = 0.2;
    // Centred on the object origin in XY, as an imported model is: the belt-side half of the cube
    // then projects negative on the gantry normal.
    SegmentedCube fixture(Vec3d(0.3, 0.2, 1.).normalized(), Vec3d(-10., -10., 0.), Vec3d::Ones(), Vec3d::Ones(), 0., {
        { "belt_printer",              "1" },
        { "belt_slice_rotation",       "x" },
        { "belt_frame_tilt_angle",     "45" },
        { "belt_slice_rotation_angle", "45" },
        { "printable_area",            "0x0,250x0,250x500,0x500" },
        { "layer_height",              "0.2" },
        { "initial_layer_print_height", "0.2" },
    });
    REQUIRE(fixture.print.objects().size() == 2);
    fixture.process();

    const PrintObject *root  = root_of(fixture.print);
    const PrintObject *child = non_root_of(fixture.print);
    REQUIRE(root  != nullptr);
    REQUIRE(child != nullptr);

    const CutSegmentDescriptor &seg = *root->segment();
    INFO("root build_z_min = " << seg.build_z_min << ", build_z_max = " << seg.build_z_max);
    // The precondition for the defect: the cube's belt-side half projects negative.
    REQUIRE(seg.build_z_min < -1.);

    // The root is rebased onto its own minimum: its lowest material sits at slice z = 0, where the
    // first layer cuts it, instead of below the slicing range.
    double lowest = std::numeric_limits<double>::max();
    for (double x : { -10., 10. })
        for (double y : { -10., 10. })
            for (double z : { 0., 20. })
                lowest = std::min(lowest, (root->object_to_slice() * Vec3d(x, y, z)).z());
    CHECK_THAT(lowest, WithinAbs(0., 1e-6));
    REQUIRE(! root->layers().empty());
    CHECK_THAT(root->layers().front()->print_z, WithinAbs(layer_height, 1e-6));
    CHECK(! root->layers().front()->lslices.empty());

    // A non-root segment's own floor is its base plane, which runs on below the belt. The belt has
    // to bound its support too. Probe a point just above that base plane but under the belt.
    const SegmentSupportBounds child_bounds(*child);
    REQUIRE(child_bounds.is_active());
    Vec3d  n_bed;
    double k_bed;
    child->bed_plane_in_slice_frame(n_bed, k_bed);   // bed-frame z = n_bed . p + k_bed
    const double gxy = std::hypot(n_bed.x(), n_bed.y());
    // The child is tilted against the gantry, so the belt is not horizontal in its frame.
    REQUIRE(gxy > 0.1);
    const double z = 0.5;                            // above the child's own floor at slice z = 0
    const double t = (-1. - k_bed - n_bed.z() * z) / gxy;
    const Vec2d  xy = t * Vec2d(n_bed.x(), n_bed.y()) / gxy;
    INFO("probe " << xy.x() << "," << xy.y() << "," << z << " sits 1 mm under the belt");
    CHECK_THAT(n_bed.dot(Vec3d(xy.x(), xy.y(), z)) + k_bed, WithinAbs(-1., 1e-9));
    CHECK(! contains(child_bounds.valid_region_polygon(z), Point::new_scale(xy.x(), xy.y())));
}

namespace {

// A 20 mm cube with ONE steep plane, 70 degrees from vertical, meeting the plate along x = 14
// (object-local). The far piece -- x > 14 at the plate, leaning back to x > 6.7 at the top -- is a
// non-root segment that slices along that tilted normal, so its angled layers run down to the
// build plate. `sink` lowers the instance into the plate, the way an un-seated import sits.
struct PlateReachingCube
{
    Slic3r::Print print;
    Model         model;

    explicit PlateReachingCube(double sink)
    {
        ModelObject *object = model.add_object();
        object->name = "plate_reaching_cube";
        object->add_volume(Test::cube(20.), ModelVolumeType::MODEL_PART, false);
        ModelInstance *instance = object->add_instance();
        instance->set_offset(Vec3d(120., 120., - sink));

        CutPlaneStack stack;
        stack.enabled = true;
        CutPlane cp;
        cp.point  = Vec3d(14., 10., 0.);
        cp.normal = Vec3d(std::cos(Geometry::deg2rad(20.)), 0., std::sin(Geometry::deg2rad(20.)));
        stack.planes.push_back(cp);
        object->cut_planes = stack;

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_nonplanar",           "1" },
            { "print_sequence",             "by group" },
            { "layer_change_gcode",         "G92 E0" },
            { "layer_height",               "0.2" },
            { "initial_layer_print_height", "0.2" },
        });
        print.apply(model, config);
    }
};

// Lowest world z of any BEAD BOTTOM of `object`: every slice point, evaluated on its layer's own
// slice plane, less the h/2 * |cos(tilt)| a bead reaches below that plane (tilt = the segment's
// build normal against world Z). The plate clip promises this is never below 0.
double lowest_slice_point_above_margin(const PrintObject &object)
{
    const Vec3d n_world = (object.trafo().linear().inverse().transpose() * object.segment()->build_normal).normalized();
    const double cos_tilt = std::abs(n_world.z());
    double lowest = std::numeric_limits<double>::max();
    for (const Layer *layer : object.layers())
        for (const ExPolygon &expoly : layer->lslices)
            for (const Point &pt : to_points(expoly)) {
                const Vec3d local = object.slice_to_object() * Vec3d(unscale<double>(pt.x()), unscale<double>(pt.y()), layer->slice_z);
                lowest = std::min(lowest, (object.trafo() * local).z() - 0.5 * layer->height * cos_tilt);
            }
    return lowest;
}

} // namespace

TEST_CASE("A tilted segment that reaches the build plate stops at the plate", "[cutplanes][frames]")
{
    // 0 = resting on the plate, 2 = sunk 2 mm into it. Either way the tilted segment owns material
    // down at the plate; sunk, it also owns material UNDER it, which used to be printed there.
    const double sink = GENERATE(0., 2.);
    DYNAMIC_SECTION("sink " << sink) {
        PlateReachingCube fixture(sink);
        REQUIRE(fixture.print.objects().size() == 2);
        fixture.print.process();

        const PrintObject *root  = root_of(fixture.print);
        const PrintObject *child = non_root_of(fixture.print);
        REQUIRE(root  != nullptr);
        REQUIRE(child != nullptr);

        // Only the tilted segment gets the plate as a clip; the root already stops at the plate
        // through its clamped slice-frame base.
        CHECK(child->segment()->has_bed_clip());
        CHECK(! root->segment()->has_bed_clip());

        // No bead of the tilted segment reaches below the plate.
        REQUIRE(! child->layers().empty());
        CHECK(lowest_slice_point_above_margin(*child) >= -1e-3);

        // And it starts where material starts: no empty leading layer left behind by the clip.
        CHECK(! child->layers().front()->lslices.empty());
    }
}

TEST_CASE("A tilted segment well above the build plate gets no plate clip", "[cutplanes][frames]")
{
    // The original fixture's gentle plane through the middle of the cube: its far piece starts
    // about 6 mm above the plate, out of reach of the plate clip.
    SegmentedCube fixture(Vec3d(0.25, 0.15, 1.).normalized());
    REQUIRE(fixture.print.objects().size() == 2);
    const PrintObject *child = non_root_of(fixture.print);
    REQUIRE(child != nullptr);
    CHECK(! child->segment()->has_bed_clip());
}

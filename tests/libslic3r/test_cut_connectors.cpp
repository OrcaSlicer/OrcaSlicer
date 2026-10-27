#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <utility>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Mesh vertices are floats, so measurements through a rotated cut frame carry rounding.
constexpr double EPS = 1e-4;

// The extent of a cut part along the cut normal, measured from the cut face.
struct PartExtent
{
    double solid_face   = std::numeric_limits<double>::quiet_NaN(); // the solid's face on the cut plane
    double hole_depth   = 0.; // how far the connector hole reaches into the solid
    double hole_overlap = 0.; // how far the hole pokes out of the cut face
    double peg_height   = 0.; // how far a solid connector sticks out of the cut face
};

// Measures a part in the cut plane's frame, where the cut face is at Z = 0
// and `upper` tells which side of it the solid lies on.
PartExtent measure_part(const ModelObject* part, const Transform3d& to_cut_frame, bool upper)
{
    const double sign = upper ? 1. : -1.;
    PartExtent   ext;
    for (const ModelVolume* vol : part->volumes) {
        const BoundingBoxf3 bb       = vol->mesh().transformed_bounding_box(to_cut_frame * vol->get_matrix());
        const double        near_end = sign * (upper ? bb.min.z() : bb.max.z()); // the end nearest the cut plane
        const double        far_end  = sign * (upper ? bb.max.z() : bb.min.z()); // the end farthest from it
        if (!vol->cut_info.is_connector) {
            ext.solid_face = near_end;
        } else if (vol->is_negative_volume()) {
            ext.hole_depth   = far_end;
            ext.hole_overlap = -near_end;
        } else {
            ext.peg_height = -near_end; // a peg sticks out of the face, away from the solid
        }
    }
    return ext;
}

// A 25 mm cube centred on the origin, so a plane through the origin cuts it in half.
ModelObject* add_centred_cube(Model& model)
{
    ModelObject* object = model.add_object();
    TriangleMesh cube = make_cube(25., 25., 25.);
    cube.translate(-12.5f, -12.5f, -12.5f);
    object->add_volume(std::move(cube));
    object->add_instance();
    return object;
}

struct CutResult
{
    PartExtent upper;
    PartExtent lower;
    double     dowel_length = 0.;
};

// Cuts the cube with one connector placed the way the cut gizmo places it on the given plane:
// a plug sits on the upper side of the plane, a dowel is doubled and centred on it.
CutResult cut_cube_with_connector(const Transform3d& cut_matrix, CutConnectorType type, float depth, float depth_tolerance)
{
    Model        model;
    ModelObject* object = add_centred_cube(model);

    const Geometry::Transformation cut_trafo(cut_matrix);
    const Transform3d              rotation = cut_trafo.get_rotation_matrix();
    const Vec3d                    normal   = rotation * Vec3d::UnitZ();

    const bool  is_dowel = type == CutConnectorType::Dowel;
    const float height   = is_dowel ? 2.f * depth : depth;
    const Vec3d pos      = cut_trafo.get_offset() + (is_dowel ? Vec3d::Zero() : Vec3d(0.5 * static_cast<double>(height) * normal));

    ModelVolume* connector = object->add_volume(TriangleMesh(its_make_cylinder(1., 1.)), ModelVolumeType::NEGATIVE_VOLUME);
    connector->set_transformation(Geometry::translation_transform(pos) * rotation * Geometry::scale_transform(Vec3d(2., 2., static_cast<double>(height))));
    connector->cut_info = { type, 0.f, depth_tolerance };

    const ModelObjectCutAttributes keep_both  = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;
    const ModelObjectCutAttributes attributes = is_dowel ? keep_both | ModelObjectCutAttribute::CreateDowels : keep_both;

    Cut                    cut(object, 0, cut_matrix, attributes);
    const ModelObjectPtrs& parts = cut.perform_with_plane();
    REQUIRE(parts.size() == (is_dowel ? 3 : 2));

    const Transform3d to_cut_frame = cut_matrix.inverse();
    CutResult         result;
    result.upper = measure_part(parts[0], to_cut_frame, true);
    result.lower = measure_part(parts[1], to_cut_frame, false);
    if (is_dowel) {
        const ModelVolume* pin = parts[2]->volumes.front();
        result.dowel_length    = pin->mesh().transformed_bounding_box(pin->get_matrix()).size().z();
    }
    return result;
}

// A plane through the cube's centre and a tilted, shifted one that still cuts it with room for the connector.
Transform3d generate_cut_matrix()
{
    return GENERATE(Transform3d(Transform3d::Identity()),
                    Transform3d(Geometry::translation_transform(Vec3d(1., 2., 3.)) * Geometry::rotation_transform(Vec3d(0.4, 0.3, 0.))));
}

} // namespace

TEST_CASE("A plug hole is depth plus tolerance deep and the peg is depth high", "[CutConnectors]")
{
    const Transform3d cut_matrix = generate_cut_matrix();
    const float       depth      = 3.f;
    const float       tolerance  = GENERATE(0.f, 0.1f, 1.f);
    const auto        type       = GENERATE(CutConnectorType::Plug, CutConnectorType::Snap);

    const CutResult result = cut_cube_with_connector(cut_matrix, type, depth, tolerance);

    CHECK_THAT(result.upper.solid_face, WithinAbs(0., EPS));
    CHECK_THAT(result.upper.hole_depth, WithinAbs(static_cast<double>(depth + tolerance), EPS));
    // The hole must break through the cut face, or the boolean leaves a skin over it.
    CHECK_THAT(result.upper.hole_overlap, WithinAbs(CONNECTOR_HOLE_OVERLAP, EPS));
    CHECK_THAT(result.lower.solid_face, WithinAbs(0., EPS));
    CHECK_THAT(result.lower.peg_height, WithinAbs(static_cast<double>(depth), EPS));
}

TEST_CASE("Dowel holes each get half the tolerance and together fit the pin", "[CutConnectors]")
{
    const Transform3d cut_matrix = generate_cut_matrix();
    const float       depth      = 3.f;
    const float       tolerance  = GENERATE(0.f, 0.1f, 1.f);

    const CutResult result = cut_cube_with_connector(cut_matrix, CutConnectorType::Dowel, depth, tolerance);

    const double expected_hole = static_cast<double>(depth) + 0.5 * static_cast<double>(tolerance);
    CHECK_THAT(result.upper.solid_face, WithinAbs(0., EPS));
    CHECK_THAT(result.upper.hole_depth, WithinAbs(expected_hole, EPS));
    CHECK_THAT(result.upper.hole_overlap, WithinAbs(CONNECTOR_HOLE_OVERLAP, EPS));
    CHECK_THAT(result.lower.solid_face, WithinAbs(0., EPS));
    CHECK_THAT(result.lower.hole_depth, WithinAbs(expected_hole, EPS));
    CHECK_THAT(result.lower.hole_overlap, WithinAbs(CONNECTOR_HOLE_OVERLAP, EPS));
    CHECK_THAT(result.dowel_length, WithinAbs(2. * static_cast<double>(depth), EPS));
}

TEST_CASE("A cut with no connectors leaves a solid on each side of the plane", "[CutConnectors]")
{
    Model        model;
    ModelObject* object = add_centred_cube(model);

    Cut                    cut(object, 0, Transform3d::Identity(), ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& parts = cut.perform_with_plane();
    REQUIRE(parts.size() == 2);

    const Transform3d identity = Transform3d::Identity();
    CHECK_THAT(measure_part(parts[0], identity, true).solid_face, WithinAbs(0., EPS));
    CHECK_THAT(measure_part(parts[1], identity, false).solid_face, WithinAbs(0., EPS));
}

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

ModelObject* make_cube_object(Model& model)
{
    TriangleMesh mesh = make_cube(40.0, 40.0, 40.0);
    mesh.translate(Vec3f(-20.f, -20.f, -20.f));
    ModelObject* obj = model.add_object();
    obj->add_volume(std::move(mesh));
    obj->add_instance();
    return obj;
}

Transform3d plane_cut_matrix()
{
    return Transform3d::Identity();
}

ModelVolume* add_connector_volume(ModelObject* obj,
                                  CutConnectorType type,
                                  float depth,
                                  float height_tolerance,
                                  float radius = 3.0f)
{
    const float total_height = (type == CutConnectorType::Dowel) ? (depth * 2.0f) : depth;

    TriangleMesh mesh = TriangleMesh(its_make_cylinder(1.0, 1.0, PI / 60.0));
    ModelVolume* vol  = obj->add_volume(std::move(mesh), ModelVolumeType::NEGATIVE_VOLUME);

    Vec3d pos = Vec3d::Zero();
    if (type != CutConnectorType::Dowel)
        pos = Vec3d(0.0, 0.0, 0.5 * double(total_height));

    vol->set_transformation(
        Geometry::translation_transform(pos) *
        Geometry::scale_transform(Vec3d(radius, radius, total_height))
    );

    vol->cut_info = { type, 0.0f, height_tolerance };
    vol->name     = "Connector-1";
    return vol;
}

const ModelVolume* find_connector_volume(const ModelObject* obj,
                                         ModelVolumeType expected_type)
{
    for (const ModelVolume* vol : obj->volumes) {
        if (vol->cut_info.is_connector && vol->type() == expected_type)
            return vol;
    }
    return nullptr;
}

// A cut part's mesh is centred when it is added, so its side of the plane shows in the volume's
// bounding box in object coordinates, not in its raw vertices.
BoundingBoxf3 volume_bbox(const ModelVolume* vol)
{
    return vol->mesh().transformed_bounding_box(vol->get_matrix());
}

const ModelObject* find_part_by_side(const ModelObjectPtrs& parts, bool upper)
{
    for (const ModelObject* obj : parts) {
        for (const ModelVolume* vol : obj->volumes) {
            if (vol->type() != ModelVolumeType::MODEL_PART || vol->cut_info.is_connector)
                continue;
            const BoundingBoxf3 bb = volume_bbox(vol);
            if (upper ? bb.max.z() > 1e-3 : bb.min.z() < -1e-3)
                return obj;
        }
    }
    return nullptr;
}

double volume_depth_z(const ModelVolume* vol)
{
    return volume_bbox(vol).size().z();
}

} // namespace

TEST_CASE("Dowel connector depth tolerance is split symmetrically between upper and lower sockets", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);

    const float depth            = 10.0f;
    const float height_tolerance = 2.0f;
    const float radius           = 3.0f;

    add_connector_volume(obj, CutConnectorType::Dowel, depth, height_tolerance, radius);

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower |
        ModelObjectCutAttribute::CreateDowels;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 3);

    const ModelObject* upper = find_part_by_side(result, /*upper=*/true);
    const ModelObject* lower = find_part_by_side(result, /*upper=*/false);
    REQUIRE(upper != nullptr);
    REQUIRE(lower != nullptr);

    const ModelObject* dowel = nullptr;
    for (const ModelObject* part : result) {
        if (part != upper && part != lower) { dowel = part; break; }
    }
    REQUIRE(dowel != nullptr);

    const ModelVolume* dowel_vol = dowel->volumes.front();
    REQUIRE(dowel_vol != nullptr);
    CHECK_THAT(volume_depth_z(dowel_vol), WithinAbs(double(depth * 2.0f), 1e-3));

    const ModelVolume* upper_socket = find_connector_volume(upper, ModelVolumeType::NEGATIVE_VOLUME);
    const ModelVolume* lower_socket = find_connector_volume(lower, ModelVolumeType::NEGATIVE_VOLUME);
    REQUIRE(upper_socket != nullptr);
    REQUIRE(lower_socket != nullptr);

    double upper_socket_depth = volume_depth_z(upper_socket);
    double lower_socket_depth = volume_depth_z(lower_socket);

    const double expected_socket_depth = double(depth + height_tolerance / 2.0f);
    CHECK_THAT(upper_socket_depth, WithinAbs(expected_socket_depth, 1e-3));
    CHECK_THAT(lower_socket_depth, WithinAbs(expected_socket_depth, 1e-3));
    CHECK_THAT(upper_socket_depth, WithinAbs(lower_socket_depth, 1e-3));
}

TEST_CASE("Dowel connector with zero tolerance produces equal nominal depth sockets", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);

    const float depth            = 8.0f;
    const float height_tolerance = 0.0f;

    add_connector_volume(obj, CutConnectorType::Dowel, depth, height_tolerance);

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower |
        ModelObjectCutAttribute::CreateDowels;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);

    const ModelObject* upper = find_part_by_side(result, /*upper=*/true);
    const ModelObject* lower = find_part_by_side(result, /*upper=*/false);
    REQUIRE(upper != nullptr);
    REQUIRE(lower != nullptr);

    const ModelVolume* upper_socket = find_connector_volume(upper, ModelVolumeType::NEGATIVE_VOLUME);
    const ModelVolume* lower_socket = find_connector_volume(lower, ModelVolumeType::NEGATIVE_VOLUME);
    REQUIRE(upper_socket != nullptr);
    REQUIRE(lower_socket != nullptr);

    double upper_depth = volume_depth_z(upper_socket);
    double lower_depth = volume_depth_z(lower_socket);

    CHECK_THAT(upper_depth, WithinAbs(double(depth), 1e-3));
    CHECK_THAT(lower_depth, WithinAbs(double(depth), 1e-3));
    CHECK_THAT(upper_depth, WithinAbs(lower_depth, 1e-3));
}

TEST_CASE("Plug connector keeps male peg nominal and applies full tolerance to female socket", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);

    const float depth            = 6.0f;
    const float height_tolerance = 1.0f;

    add_connector_volume(obj, CutConnectorType::Plug, depth, height_tolerance);

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);

    const ModelObject* upper = find_part_by_side(result, /*upper=*/true);
    const ModelObject* lower = find_part_by_side(result, /*upper=*/false);
    REQUIRE(upper != nullptr);
    REQUIRE(lower != nullptr);

    const ModelVolume* female_socket = find_connector_volume(upper, ModelVolumeType::NEGATIVE_VOLUME);
    const ModelVolume* male_peg      = find_connector_volume(lower, ModelVolumeType::MODEL_PART);
    REQUIRE(female_socket != nullptr);
    REQUIRE(male_peg != nullptr);

    CHECK_THAT(volume_depth_z(male_peg),      WithinAbs(double(depth),                    1e-3));
    CHECK_THAT(volume_depth_z(female_socket), WithinAbs(double(depth + height_tolerance), 1e-3));
}

TEST_CASE("Cut with no connectors produces valid upper and lower parts", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);

    bool has_upper = false;
    bool has_lower = false;
    for (const ModelObject* part : result) {
        for (const ModelVolume* vol : part->volumes) {
            const BoundingBoxf3 bb = volume_bbox(vol);
            if (bb.max.z() > 1e-3)  has_upper = true;
            if (bb.min.z() < -1e-3) has_lower = true;
        }
    }
    CHECK(has_upper);
    CHECK(has_lower);
}

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

ModelObject* make_cube_object(Model& model)
{
    TriangleMesh mesh = make_cube(40.0, 40.0, 40.0);
    mesh.translate(Vec3f(-20.f, -20.f, -20.f));
    ModelObject* obj = model.add_object();
    obj->add_volume(mesh);
    obj->add_instance();
    return obj;
}

Transform3d plane_cut_matrix()
{
    return Transform3d::Identity();
}

CutConnector make_connector(CutConnectorType type, float height, float height_tolerance, float radius = 3.0f)
{
    CutConnector c;
    c.pos              = Vec3d(0.0, 0.0, 0.0);
    c.rotation_m       = Transform3d::Identity();
    c.radius           = radius;
    c.height           = height;
    c.radius_tolerance = 0.f;
    c.height_tolerance = height_tolerance;
    c.z_angle          = 0.f;
    c.attribs          = CutConnectorAttributes(type, CutConnectorStyle::Prism, CutConnectorShape::Circle);
    return c;
}

double mesh_max_z_in_radius(const TriangleMesh& mesh, double r2_limit, bool positive_z)
{
    double result = 0.0;
    for (const Vec3f& v : mesh.its.vertices) {
        const double r2 = v.x() * v.x() + v.y() * v.y();
        if (r2 <= r2_limit) {
            if (positive_z && v.z() > 0.f)
                result = std::max(result, static_cast<double>(v.z()));
            else if (!positive_z && v.z() < 0.f)
                result = std::max(result, static_cast<double>(-v.z()));
        }
    }
    return result;
}

} // namespace

TEST_CASE("Cut with Dowel connector produces two parts", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);
    obj->cut_connectors.push_back(make_connector(CutConnectorType::Dowel, 10.f, 0.f));

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower |
        ModelObjectCutAttribute::CreateDowels;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);
}

TEST_CASE("Cut with Plug connector produces two parts", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);
    obj->cut_connectors.push_back(make_connector(CutConnectorType::Plug, 10.f, 0.f));

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);
}

TEST_CASE("Cut with Snap connector produces two parts", "[CutConnectors]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);
    obj->cut_connectors.push_back(make_connector(CutConnectorType::Snap, 10.f, 0.f));

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);
}

TEST_CASE("Cut with no connectors produces upper and lower parts", "[CutConnectors]")
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
            for (const Vec3f& v : vol->mesh().its.vertices) {
                if (v.z() > 0.f) has_upper = true;
                if (v.z() < 0.f) has_lower = true;
            }
        }
    }
    CHECK(has_upper);
    CHECK(has_lower);
}

TEST_CASE("Cut connector height_tolerance field stores assigned value", "[CutConnectors]")
{
    const float tolerance = GENERATE(0.f, 0.1f, 0.2f, 0.35f, 0.5f);
    DYNAMIC_SECTION("height_tolerance = " << tolerance)
    {
        CutConnector c = make_connector(CutConnectorType::Dowel, 10.f, tolerance);
        CHECK_THAT(static_cast<double>(c.height_tolerance), WithinAbs(static_cast<double>(tolerance), 1e-6));
        CHECK_THAT(static_cast<double>(c.height), WithinAbs(10.0, 1e-6));
    }
}

TEST_CASE("CutConnector default construction has valid fields", "[CutConnectors]")
{
    CutConnector c;
    CHECK(c.radius > 0.f);
    CHECK(c.height > 0.f);
    CHECK(c.height_tolerance >= 0.f);
    CHECK(c.radius_tolerance >= 0.f);
}

TEST_CASE("CutConnector copy constructor preserves all fields", "[CutConnectors]")
{
    CutConnector orig = make_connector(CutConnectorType::Plug, 8.f, 0.15f, 4.f);
    CutConnector copy = orig;

    CHECK_THAT(static_cast<double>(copy.height),           WithinAbs(8.0,  1e-6));
    CHECK_THAT(static_cast<double>(copy.height_tolerance), WithinAbs(0.15, 1e-6));
    CHECK_THAT(static_cast<double>(copy.radius),           WithinAbs(4.0,  1e-6));
    CHECK(copy.attribs.type  == CutConnectorType::Plug);
    CHECK(copy.attribs.shape == CutConnectorShape::Circle);
}

TEST_CASE("CutConnector fields match between two identically constructed connectors", "[CutConnectors]")
{
    CutConnector a = make_connector(CutConnectorType::Dowel, 10.f, 0.1f);
    CutConnector b = make_connector(CutConnectorType::Dowel, 10.f, 0.1f);
    CutConnector c = make_connector(CutConnectorType::Plug,  10.f, 0.1f);

    CHECK_THAT(static_cast<double>(a.height),           WithinAbs(static_cast<double>(b.height),           1e-6));
    CHECK_THAT(static_cast<double>(a.height_tolerance), WithinAbs(static_cast<double>(b.height_tolerance), 1e-6));
    CHECK_THAT(static_cast<double>(a.radius),           WithinAbs(static_cast<double>(b.radius),           1e-6));
    CHECK(a.attribs.type == b.attribs.type);

    CHECK(a.attribs.type != c.attribs.type);
}

TEST_CASE("Issue #14512 regression: plane cut at z=0 produces non-empty upper and lower parts", "[CutConnectors][Regression]")
{
    Model model;
    ModelObject* obj = make_cube_object(model);
    obj->cut_connectors.push_back(make_connector(CutConnectorType::Dowel, 10.f, 0.2f));

    const ModelObjectCutAttributes attrs =
        ModelObjectCutAttribute::KeepUpper |
        ModelObjectCutAttribute::KeepLower |
        ModelObjectCutAttribute::CreateDowels;

    Cut cut(obj, 0, plane_cut_matrix(), attrs);
    const ModelObjectPtrs& result = cut.perform_with_plane();

    REQUIRE(result.size() >= 2);
    for (const ModelObject* part : result) {
        bool has_volumes = !part->volumes.empty();
        CHECK(has_volumes);
        if (has_volumes)
            CHECK_FALSE(part->volumes.front()->mesh().its.vertices.empty());
    }
}
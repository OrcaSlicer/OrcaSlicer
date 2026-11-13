#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Measure.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using namespace Slic3r::Measure;
using Catch::Matchers::WithinAbs;

TEST_CASE("Scaled plane normals remain perpendicular to the transformed surface", "[Measure]")
{
    const Vec3d normal = Vec3d(1, 0, 1).normalized();
    SurfaceFeature plane(SurfaceFeatureType::Plane, normal, Vec3d::Zero());
    Transform3d transform = Transform3d::Identity();
    transform.linear() = Vec3d(2, 3, 4).asDiagonal();
    plane.translate(transform);
    const Vec3d transformed_normal = std::get<1>(plane.get_plane());
    CHECK_THAT(transformed_normal.dot(transform.linear() * Vec3d(1, 0, -1)), WithinAbs(0.0, 1e-12));
    CHECK_THAT(transformed_normal.norm(), WithinAbs(1.0, 1e-12));
}

TEST_CASE("Uniform in-plane scaling preserves a circle radius", "[Measure]")
{
    SurfaceFeature circle(SurfaceFeatureType::Circle, Vec3d(1, 2, 3), Vec3d::UnitZ(), std::nullopt, 5.0);
    Transform3d transform = Transform3d::Identity();
    transform.linear() = Vec3d(2, 2, 4).asDiagonal();
    circle.translate(transform);
    const auto [center, radius, normal] = circle.get_circle();
    CHECK_THAT(radius, WithinAbs(10.0, 1e-12));
    CHECK_THAT((center - Vec3d(2, 4, 12)).norm(), WithinAbs(0.0, 1e-12));
    CHECK_THAT(normal.dot(Vec3d::UnitZ()), WithinAbs(1.0, 1e-12));
}

TEST_CASE("Circle detection uses the transformed mesh boundary", "[Measure]")
{
    auto circle_radius = [](double source_y_scale, double x_scale, double y_scale, bool noisy = false) {
        indexed_triangle_set mesh = its_make_cylinder(10., 20., PI / 90.);
        for (size_t i = 2; i < mesh.vertices.size(); ++i) {
            Vec3f &vertex = mesh.vertices[i];
            const float radial_scale = noisy && (i / 2) % 2 == 0 ? 1.0005f : 1.f;
            vertex.x() *= radial_scale;
            vertex.y() *= radial_scale * float(source_y_scale);
        }
        for (Vec3f &vertex : mesh.vertices) {
            vertex.x() *= float(x_scale);
            vertex.y() *= float(y_scale);
        }
        Measuring measuring(mesh);
        for (int i = 0; i < measuring.get_num_of_planes(); ++i) {
            const auto &features = measuring.get_plane_features(i);
            const auto circle = std::find_if(features.begin(), features.end(), [](const SurfaceFeature &feature) {
                return feature.get_type() == SurfaceFeatureType::Circle;
            });
            if (circle != features.end())
                return std::get<1>(circle->get_circle());
        }
        return -1.0;
    };

    CHECK_THAT(circle_radius(1., 2., 2.), WithinAbs(20., 0.01));
    CHECK_THAT(circle_radius(1., 1., 1.), WithinAbs(10., 0.01));
    CHECK_THAT(circle_radius(2., 2., 1.), WithinAbs(20., 0.01));
    CHECK_THAT(circle_radius(1., 1., 1., true), WithinAbs(10., 0.01));
}

TEST_CASE("Plane features follow the geometry of a stretched mesh", "[Measure]")
{
    indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    Transform3d transform = Transform3d::Identity();
    transform.linear() << 2., 0., 1., 0., 3., 0., 0., 0., 4.;
    for (Vec3f &vertex : mesh.vertices)
        vertex = (transform * vertex.cast<double>()).cast<float>();

    Measuring measuring(mesh);
    const auto normals = its_face_normals(mesh);
    std::optional<SurfaceFeature> first_plane;
    bool checked_angle = false;
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        const auto &triangle = mesh.indices[i];
        const Vec3d point = (mesh.vertices[triangle[0]].cast<double>() +
                             mesh.vertices[triangle[1]].cast<double>() +
                             mesh.vertices[triangle[2]].cast<double>()) / 3.;
        const auto feature = measuring.get_feature(i, point, Transform3d::Identity(), true);
        REQUIRE(feature.has_value());
        REQUIRE(feature->get_type() == SurfaceFeatureType::Plane);
        const Vec3d normal = std::get<1>(feature->get_plane());
        CHECK_THAT(normal.dot(normals[i].cast<double>()), WithinAbs(1., 1e-5));
        if (!first_plane) {
            first_plane = feature;
        } else if (!checked_angle) {
            const Vec3d first_normal = std::get<1>(first_plane->get_plane());
            const double normal_dot = std::abs(first_normal.dot(normal));
            if (normal_dot < 0.999 && normal_dot > 0.001) {
                const auto result = get_measurement(*first_plane, *feature);
                REQUIRE(result.angle.has_value());
                CHECK_THAT(std::abs(std::cos(result.angle->angle)), WithinAbs(normal_dot, 1e-5));
                checked_angle = true;
            }
        }
    }
    CHECK(checked_angle);
}


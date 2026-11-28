#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshDeal.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <cmath>
#include <cstddef>

using namespace Slic3r;

// Whether a point lies on the surface of the cube spanning 0..size on every axis.
static bool on_cube_surface(const Vec3f &p, float size)
{
    for (int axis = 0; axis < 3; ++axis)
        if (std::abs(p[axis]) < 1e-4f || std::abs(p[axis] - size) < 1e-4f)
            return true;
    return false;
}

static double painted_area(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &data, EnforcerBlockerType state)
{
    TriangleSelector selector(mesh);
    selector.deserialize(data);
    const indexed_triangle_set facets = selector.get_facets(state);
    double area = 0.;
    for (const stl_triangle_vertex_indices &f : facets.indices)
        area += 0.5 * double((facets.vertices[f(1)] - facets.vertices[f(0)]).cross(facets.vertices[f(2)] - facets.vertices[f(0)]).norm());
    return area;
}

TEST_CASE("Loop subdivision returns the same triangles before smoothing, on the input surface", "[TriangleMeshDeal]")
{
    const float        size = 20.f;
    const TriangleMesh cube = make_cube(size, size, size);

    bool         ok = false;
    TriangleMesh unsmoothed;
    const TriangleMesh smoothed = TriangleMeshDeal::smooth_triangle_mesh(cube, ok, &unsmoothed);
    REQUIRE(ok);

    REQUIRE(smoothed.its.indices.size() == 4 * cube.its.indices.size());
    REQUIRE(unsmoothed.its.indices == smoothed.its.indices);
    REQUIRE(unsmoothed.its.vertices.size() == smoothed.its.vertices.size());
    for (const Vec3f &v : unsmoothed.its.vertices)
        REQUIRE(on_cube_surface(v, size));
    // The smoothing rounds the corners off, which is what made the paint there get lost.
    REQUIRE_FALSE(on_cube_surface(smoothed.its.vertices.front(), size));
}

TEST_CASE("Paint remapped onto the unsmoothed subdivision keeps a face bounded by sharp edges", "[TriangleMeshDeal]")
{
    const float        size = 20.f;
    const TriangleMesh cube = make_cube(size, size, size);

    // Paint every facet of the x = size face.
    TriangleSelector selector(cube);
    for (size_t i = 0; i < cube.its.indices.size(); ++i) {
        const stl_triangle_vertex_indices &f = cube.its.indices[i];
        if (cube.its.vertices[f(0)].x() == size && cube.its.vertices[f(1)].x() == size && cube.its.vertices[f(2)].x() == size)
            selector.set_facet(int(i), EnforcerBlockerType::Extruder2);
    }
    const auto paint = selector.serialize();
    REQUIRE_THAT(painted_area(cube, paint, EnforcerBlockerType::Extruder2), Catch::Matchers::WithinRel(double(size * size), 1e-6));

    bool         ok = false;
    TriangleMesh unsmoothed;
    TriangleMeshDeal::smooth_triangle_mesh(cube, ok, &unsmoothed);
    REQUIRE(ok);

    const auto remapped = TriangleSelector::remap_painting(cube.its, paint, unsmoothed.its, Transform3d::Identity(), {});
    REQUIRE_THAT(painted_area(unsmoothed, remapped, EnforcerBlockerType::Extruder2), Catch::Matchers::WithinRel(double(size * size), 1e-3));
}

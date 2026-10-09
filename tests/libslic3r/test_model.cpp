#include <catch2/catch_all.hpp>
#include <algorithm>
#include <utility>
#include <vector>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"

using namespace Slic3r;

// convex_hull_2d does not clip geometry below the bed, so these cases avoid
// sinking transforms.
TEST_CASE("A part's 2D convex hull is its footprint projected onto the bed", "[Model]")
{
    Model model;
    ModelObject* object = model.add_object();
    // Keep the cube's raw coordinates ([0,20] on every axis): the default
    // add_volume re-centers the geometry, which would move the footprint.
    object->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);

    SECTION("identity transform yields the 20 mm square") {
        const Polygon hull   = object->convex_hull_2d(Geometry::Transformation{}.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(0.));
        CHECK(bb.min.y() == scaled(0.));
        CHECK(bb.max.x() == scaled(20.));
        CHECK(bb.max.y() == scaled(20.));
    }

    SECTION("scaling and offset move and grow the footprint") {
        Geometry::Transformation t;
        t.set_scaling_factor({2, 2, 2}); // cube now spans [0,40]
        t.set_offset({10, 5, 0});        // then shift +10 in X, +5 in Y

        const Polygon hull   = object->convex_hull_2d(t.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(10.));
        CHECK(bb.min.y() == scaled(5.));
        CHECK(bb.max.x() == scaled(50.));
        CHECK(bb.max.y() == scaled(45.));
    }
}

TEST_CASE("An object's raw mesh keeps the triangles of each part on its own vertices", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    TriangleMesh second = make_cube(10, 10, 10);
    second.translate(30, 0, 0);
    object->add_volume(std::move(second), ModelVolumeType::MODEL_PART, false);

    // Two separate cubes stay two closed components, one around each cube.
    const std::vector<indexed_triangle_set> parts = its_split(object->raw_indexed_triangle_set());
    REQUIRE(parts.size() == 2);
    std::vector<double> min_x;
    for (const indexed_triangle_set &part : parts) {
        CHECK(part.indices.size() == 12);
        min_x.push_back(bounding_box(part).min.x());
    }
    std::sort(min_x.begin(), min_x.end());
    CHECK_THAT(min_x.front(), Catch::Matchers::WithinAbs(0., 1e-4));
    CHECK_THAT(min_x.back(), Catch::Matchers::WithinAbs(30., 1e-4));
}

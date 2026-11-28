#include <iostream>
#include <fstream>
#include <catch2/catch_all.hpp>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <catch2/catch_test_macros.hpp>
#include "libslic3r/SLA/Hollowing.hpp"

#include "test_utils.hpp"

TEST_CASE("Hollow two overlapping spheres") {
    using namespace Slic3r;

    TriangleMesh sphere1 = make_sphere(10., 2 * PI / 20.), sphere2 = sphere1;

    sphere1.translate(-5.f, 0.f, 0.f);
    sphere2.translate( 5.f, 0.f, 0.f);

    sphere1.merge(sphere2);

    const auto area = [](const indexed_triangle_set &its) {
        double sum = 0.;
        for (const auto &f : its.indices)
            sum += 0.5 * double((its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]).norm());
        return sum;
    };
    const sla::InteriorPtr interior = sla::generate_interior(sphere1);
    REQUIRE(interior);
    const double merged_area   = area(sphere1.its);
    const double interior_area = area(sla::get_mesh(*interior));
    REQUIRE(interior_area > 0.);
    sla::hollow_mesh(sphere1, *interior, sla::HollowingFlags::hfRemoveInsideTriangles);
    // The cavity is added, less the roughly 400 square mm of the two spheres that lie inside each other deeper than
    // the wall, so at least 200 must go.
    CHECK(area(sphere1.its) < merged_area + interior_area - 200.);

    write_debug_obj("hollowing/twospheres.obj", sphere1);
}


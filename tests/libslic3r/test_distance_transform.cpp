#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/DistanceTransform.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

using namespace Slic3r;

namespace {
constexpr float Inf = std::numeric_limits<float>::infinity();
}

TEST_CASE("A line transform gives the squared distance to the nearest site", "[DistanceTransform]")
{
    // Sites at 1 and 6, the one at 6 already 4 away: each sample takes min((q - 1)^2, (q - 6)^2 + 4).
    const std::vector<float> f { Inf, 0.f, Inf, Inf, Inf, Inf, 4.f, Inf };
    std::vector<float>       d(f.size());
    std::vector<int>         v(f.size());
    std::vector<double>      s(f.size() + 1);
    squared_distance_transform_line(f.data(), d.data(), int(f.size()), v.data(), s.data());
    const std::vector<float> expected { 1.f, 0.f, 1.f, 4.f, 8.f, 5.f, 4.f, 5.f };
    for (size_t i = 0; i < f.size(); ++i)
        CHECK_THAT(d[i], Catch::Matchers::WithinAbs(expected[i], 1e-6));
}

TEST_CASE("A line without sites stays infinite", "[DistanceTransform]")
{
    const std::vector<float> f(5, Inf);
    std::vector<float>       d(f.size(), 0.f);
    std::vector<int>         v(f.size());
    std::vector<double>      s(f.size() + 1);
    squared_distance_transform_line(f.data(), d.data(), int(f.size()), v.data(), s.data());
    for (float x : d)
        CHECK(std::isinf(x));
}

TEST_CASE("Transforming every axis gives the Euclidean distance to the nearest site", "[DistanceTransform]")
{
    // A single site in the corner of a 5 x 4 x 3 grid.
    const Vec3i32      size(5, 4, 3);
    std::vector<float> grid(size_t(size.x()) * size.y() * size.z(), Inf);
    grid[0] = 0.f;
    for (int axis = 0; axis < 3; ++axis)
        squared_distance_transform_axis(grid, size, axis, [] {});
    for (int z = 0; z < size.z(); ++z)
        for (int y = 0; y < size.y(); ++y)
            for (int x = 0; x < size.x(); ++x)
                CHECK_THAT(grid[(size_t(z) * size.y() + y) * size.x() + x], Catch::Matchers::WithinAbs(double(x * x + y * y + z * z), 1e-6));
}

TEST_CASE("Rasterizing sets the nodes inside the outline and not those in its hole", "[DistanceTransform]")
{
    // A 6 mm square with a 2 mm square hole in the middle, on a 1 mm raster.
    auto square = [](double lo, double hi) {
        return Polygon { Point::new_scale(lo, lo), Point::new_scale(hi, lo), Point::new_scale(hi, hi), Point::new_scale(lo, hi) };
    };
    ExPolygon expolygon(square(0.5, 6.5));
    Polygon   hole = square(2.5, 4.5);
    hole.reverse();
    expolygon.holes.push_back(hole);

    constexpr int        n = 8;
    std::vector<uint8_t> nodes(n * n, 0);
    rasterize_expolygons(ExPolygons { expolygon }, Vec2d(0., 0.), 1., n, n, uint8_t(1), nodes.data());
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const bool in_square = x >= 1 && x <= 6 && y >= 1 && y <= 6;
            const bool in_hole   = x >= 3 && x <= 4 && y >= 3 && y <= 4;
            CHECK(nodes[size_t(y) * n + x] == uint8_t(in_square && !in_hole));
        }
}

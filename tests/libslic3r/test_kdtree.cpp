#include <catch2/catch_all.hpp>

#include <numeric>
#include <random>
#include <vector>

#include "libslic3r/KDTreeIndirect.hpp"
#include "libslic3r/Point.hpp"

using namespace Slic3r;

TEST_CASE("Visiting the nearby points gives what collecting them gives", "[KDTree]") {
    std::mt19937                          rng(19937);
    std::uniform_real_distribution<float> coord(-50.f, 50.f);
    // Points in a box, so that a radius search returns anything from none of them to all of them.
    std::vector<Vec3f> points(2000);
    for (Vec3f &p : points)
        p = Vec3f(coord(rng), coord(rng), coord(rng));

    auto coordinate = [&points](size_t idx, size_t dimension) { return points[idx](int(dimension)); };
    KDTreeIndirect<3, float, decltype(coordinate)> tree(coordinate);
    std::vector<size_t> indices(points.size());
    std::iota(indices.begin(), indices.end(), 0);
    tree.build(indices);

    const float radius = GENERATE(0.5f, 5.f, 25.f, 200.f);
    for (int i = 0; i < 20; ++ i) {
        const Vec3f center(coord(rng), coord(rng), coord(rng));

        const std::vector<size_t> collected = find_nearby_points(tree, center, radius);
        std::vector<size_t>       visited;
        visit_nearby_points(tree, center, radius, [&visited](size_t idx) { visited.emplace_back(idx); });

        // Same points, and in the same order: a caller that keeps the first of several equally good ones
        // must get the same answer either way.
        REQUIRE(visited == collected);
    }
}

TEST_CASE("A radius search returns every point within the radius and no other", "[KDTree]") {
    std::mt19937                          rng(2024);
    std::uniform_real_distribution<float> coord(-20.f, 20.f);
    std::vector<Vec3f> points(500);
    for (Vec3f &p : points)
        p = Vec3f(coord(rng), coord(rng), coord(rng));

    auto coordinate = [&points](size_t idx, size_t dimension) { return points[idx](int(dimension)); };
    KDTreeIndirect<3, float, decltype(coordinate)> tree(coordinate);
    std::vector<size_t> indices(points.size());
    std::iota(indices.begin(), indices.end(), 0);
    tree.build(indices);

    const Vec3f center(1.f, -2.f, 3.f);
    const float radius = 7.f;

    std::vector<size_t> expected;
    for (size_t i = 0; i < points.size(); ++ i)
        if ((points[i] - center).squaredNorm() < radius * radius)
            expected.emplace_back(i);

    std::vector<size_t> visited;
    visit_nearby_points(tree, center, radius, [&visited](size_t idx) { visited.emplace_back(idx); });
    std::sort(visited.begin(), visited.end());

    REQUIRE(! expected.empty());
    REQUIRE(visited == expected);
}

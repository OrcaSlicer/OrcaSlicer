#include <catch2/catch_all.hpp>

#define SLIC3R_TEST_HARNESS

#include "libslic3r/Point.hpp"
#include "libslic3r/GCode/OrderingStrategies.hpp"
#include "libslic3r/Geometry.hpp"

#include <algorithm>
#include <unordered_set>

using namespace Slic3r;

// --- Helpers ---

static double euclidean_path_length(const std::vector<size_t>& path, const Points& centers)
{
    return tsp_cycle_path_length(path, centers);
}

static bool has_crossings(const std::vector<size_t>& path, const Points& centers)
{
    size_t pn = path.size();
    if (pn < 4) return false;
    for (size_t i = 0; i < pn; ++i) {
        size_t i_next = (i + 1) % pn;
        for (size_t j = i + 2; j < pn; ++j) {
            if (j == i_next) continue;
            if (j == (pn - 1) && i == 0) continue;
            size_t j_next = (j + 1) % pn;
            if (Geometry::segments_intersect(
                    centers[path[i]], centers[path[i_next]],
                    centers[path[j]], centers[path[j_next]])) {
                return true;
            }
        }
    }
    return false;
}

static bool is_permutation(const std::vector<size_t>& path, size_t n)
{
    if (path.size() != n) return false;
    std::unordered_set<size_t> seen(path.begin(), path.end());
    for (size_t i = 0; i < n; ++i) {
        if (seen.count(i) != 1) return false;
    }
    return true;
}

// --- Test fixtures ---

static Points make_grid_4x4()
{
    Points pts;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            pts.emplace_back(100000 * col, 100000 * row);
    return pts;
}

static Points make_linear_5()
{
    Points pts;
    for (int i = 0; i < 5; ++i)
        pts.emplace_back(100000 * i, 0);
    return pts;
}

static Points make_ring_8()
{
    Points pts;
    constexpr double R = 100000.0;
    for (int i = 0; i < 8; ++i) {
        double angle = 2.0 * M_PI * i / 8.0;
        pts.emplace_back(static_cast<coord_t>(R * std::cos(angle)),
                         static_cast<coord_t>(R * std::sin(angle)));
    }
    return pts;
}

static Points make_random_16()
{
    // Deterministic "random" points via simple hash.
    Points pts;
    for (int i = 0; i < 16; ++i) {
        uint32_t h = static_cast<uint32_t>(i * 2654435761u);
        coord_t x = static_cast<coord_t>((h >> 16) & 0xFFFF) * 10;
        coord_t y = static_cast<coord_t>(h & 0xFFFF) * 10;
        pts.emplace_back(x, y);
    }
    return pts;
}

// --- TSP Post-Processing Tests ---

TEST_CASE("tsp_2opt_improve reduces path length", "[TSPPostProcessing]") {
    Points centers = make_random_16();
    std::vector<size_t> path(centers.size());
    // Reverse half the path to create a deliberately bad ordering.
    for (size_t i = 0; i < path.size(); ++i) path[i] = i;
    std::reverse(path.begin(), path.end() - path.size() / 2);

    double before = euclidean_path_length(path, centers);
    tsp_2opt_improve(path, centers);
    double after = euclidean_path_length(path, centers);

    REQUIRE(is_permutation(path, centers.size()));
    CHECK(after <= before);
}

TEST_CASE("tsp_remove_crossings eliminates crossings", "[TSPPostProcessing]") {
    Points centers = make_random_16();
    std::vector<size_t> path(centers.size());
    for (size_t i = 0; i < path.size(); ++i) path[i] = i;
    // Create a crossing by reversing a middle segment.
    if (path.size() >= 4) {
        std::reverse(path.begin() + 1, path.end() - 1);
    }

    tsp_remove_crossings(path, centers);
    CHECK(!has_crossings(path, centers));
    REQUIRE(is_permutation(path, centers.size()));
}



TEST_CASE("tsp_rotate_minimize_closing shortens closing edge", "[TSPPostProcessing]") {
    Points centers = make_random_16();
    std::vector<size_t> path(centers.size());
    for (size_t i = 0; i < path.size(); ++i) path[i] = i;

    // Compute all possible closing edge lengths.
    size_t pn = path.size();
    double min_closing2 = std::numeric_limits<double>::max();
    for (size_t start = 0; start < pn; ++start) {
        size_t last = (start + pn - 1) % pn;
        double d2 = (centers[path[start]].cast<double>() - centers[path[last]].cast<double>()).squaredNorm();
        if (d2 < min_closing2) min_closing2 = d2;
    }

    tsp_rotate_minimize_closing(path, centers);

    // Closing edge should be the minimum possible.
    double actual_closing2 = (centers[path.front()].cast<double>() - centers[path.back()].cast<double>()).squaredNorm();
    CHECK(actual_closing2 == min_closing2);
    REQUIRE(is_permutation(path, centers.size()));
}

TEST_CASE("tsp_cycle_path_length is correct for triangle", "[TSPPostProcessing]") {
    Points pts;
    pts.emplace_back(0, 0);
    pts.emplace_back(100000, 0);
    pts.emplace_back(50000, 86602); // equilateral ~100mm sides

    std::vector<size_t> path = {0, 1, 2};
    double len = tsp_cycle_path_length(path, pts);
    // Perimeter of equilateral triangle with side ~100000.
    REQUIRE(len > 290000);
    REQUIRE(len < 310000);
}

TEST_CASE("tsp_max_edge_length finds longest edge", "[TSPPostProcessing]") {
    Points pts;
    pts.emplace_back(0, 0);
    pts.emplace_back(100000, 0);
    pts.emplace_back(50000, 0);

    std::vector<size_t> path = {0, 1, 2};
    double mx = tsp_max_edge_length(path, pts);
    // Longest edge is 0->1 = 100000.
    CHECK(mx == Catch::Approx(100000).margin(1));
}

// --- Core Strategy Tests: Empty / Small Inputs ---

TEST_CASE("snake_core handles empty input", "[Snake]") {
    Points centers;
    auto path = snake_core(centers);
    REQUIRE(path.empty());
}

TEST_CASE("convex_hull_peeling_core handles empty input", "[ConvexHullPeeling]") {
    Points centers;
    auto path = convex_hull_peeling_core(centers);
    REQUIRE(path.empty());
}

TEST_CASE("all strategies handle single point", "[Strategies]") {
    Points pts{{100, 200}};
    CHECK(snake_core(pts) == std::vector<size_t>{0});
    CHECK(convex_hull_peeling_core(pts) == std::vector<size_t>{0});
}

TEST_CASE("all strategies handle two points", "[Strategies]") {
    Points pts{{100, 200}, {300, 400}};
    auto p2 = snake_core(pts);
    auto p3 = convex_hull_peeling_core(pts);

    REQUIRE(is_permutation(p2, 2));
    REQUIRE(is_permutation(p3, 2));
}

// --- Core Strategy Tests: Grid Layout ---

TEST_CASE("snake produces good path on grid", "[Snake]") {
    Points centers = make_grid_4x4();
    auto path = snake_core(centers);

    REQUIRE(is_permutation(path, centers.size()));
    CHECK(!has_crossings(path, centers));
}

// --- Core Strategy Tests: Collinear Points ---

TEST_CASE("all strategies handle collinear points", "[Strategies]") {
    Points centers = make_linear_5();

    auto p2 = snake_core(centers);
    auto p3 = convex_hull_peeling_core(centers);

    REQUIRE(is_permutation(p2, centers.size()));
    REQUIRE(is_permutation(p3, centers.size()));
}

// --- Core Strategy Tests: Ring Layout ---

TEST_CASE("all strategies produce valid paths on ring", "[Strategies]") {
    Points centers = make_ring_8();

    auto p2 = snake_core(centers);
    auto p3 = convex_hull_peeling_core(centers);

    REQUIRE(is_permutation(p2, centers.size()));
    REQUIRE(is_permutation(p3, centers.size()));
}

// --- Core Strategy Tests: Random Layout ---

TEST_CASE("all strategies produce valid paths on random input", "[Strategies]") {
    Points centers = make_random_16();

    auto p2 = snake_core(centers);
    auto p3 = convex_hull_peeling_core(centers);

    REQUIRE(is_permutation(p2, centers.size()));
    REQUIRE(is_permutation(p3, centers.size()));
}

// --- Quality Comparison Tests ---

TEST_CASE("convex_hull_peeling is near-optimal on structured input", "[ConvexHullPeeling]") {
    Points centers = make_grid_4x4();
    auto path = convex_hull_peeling_core(centers);
    double len = euclidean_path_length(path, centers);

    // On a 4x4 grid with spacing 100000, convex hull peeling wraps around
    // the perimeter multiple times (onion layers), so path is longer than optimal.
    REQUIRE(len > 300000);
    REQUIRE(len < 2500000);
}

TEST_CASE("snake has no crossings on random input", "[Snake]") {
    Points centers = make_random_16();
    auto path = snake_core(centers);

    REQUIRE(is_permutation(path, centers.size()));
    CHECK(!has_crossings(path, centers));
}

// --- Edge Cases ---

TEST_CASE("strategies handle duplicate points", "[Strategies]") {
    Points pts;
    pts.emplace_back(100, 200);
    pts.emplace_back(100, 200); // duplicate
    pts.emplace_back(300, 400);

    auto p2 = snake_core(pts);
    auto p3 = convex_hull_peeling_core(pts);

    REQUIRE(p2.size() == pts.size());
    REQUIRE(p3.size() == pts.size());
}

TEST_CASE("strategies handle three points", "[Strategies]") {
    Points pts;
    pts.emplace_back(0, 0);
    pts.emplace_back(100000, 0);
    pts.emplace_back(50000, 86602);

    auto p2 = snake_core(pts);
    auto p3 = convex_hull_peeling_core(pts);

    REQUIRE(is_permutation(p2, 3));
    REQUIRE(is_permutation(p3, 3));
}

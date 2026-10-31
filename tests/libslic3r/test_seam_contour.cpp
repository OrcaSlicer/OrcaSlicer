#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/GCode/SeamContour.hpp"
#include "libslic3r/Point.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::SeamContour;
namespace {
Contour circle(double radius, double x, bool hole = false, size_t count = 64) {
    Contour c;
    c.hole = hole;
    for (size_t i = 0; i < count; ++i) {
        const double a = 2.0 * std::acos(-1.0) * double(i) / double(count);
        c.points.emplace_back(x + radius * std::cos(a), radius * std::sin(a));
    }
    return c;
}
Contour rectangle(double left, double right, double bottom, double top) {
    Contour c;
    c.points = {Vec2d(left, bottom), Vec2d(right, bottom), Vec2d(right, top), Vec2d(left, top)};
    return c;
}
} // namespace

TEST_CASE("Indexed continuation preserves local holes among many distant contours", "[SeamContour][Regression]") {
    const size_t side = GENERATE(1, 10, 40);
    const double shrink = GENERATE(0.0, 0.1);
    const bool reverse_order = GENERATE(false, true);
    Layers layers(2);
    for (size_t layer = 0; layer < 2; ++layer) {
        layers[layer].push_back(rectangle(-2, 3*double(side), -2, 3*double(side)));
        for (size_t y = 0; y < side; ++y)
            for (size_t x = 0; x < side; ++x) {
                Contour hole = circle(0.8 - layer*shrink, 3*double(x), true, 16);
                for (auto &p : hole.points) p.y() += 3*double(y);
                layers[layer].push_back(std::move(hole));
            }
        if (reverse_order) std::reverse(layers[layer].begin(), layers[layer].end());
    }
    Resolver resolver(std::move(layers));
    const size_t count = side*side;
    for (size_t hole : {size_t(0), count/2, count-1}) {
        const size_t index = reverse_order ? count-1-hole : hole+1;
        const auto result = resolver.resolve(0, index, 0, 1);
        REQUIRE(result.outcome == Outcome::Unique);
        REQUIRE(result.contours.size() == 1);
        CHECK(result.contours.front() == index);
    }
}

TEST_CASE("Unknown boundary geometry preserves unrestricted continuation near either layer", "[SeamContour][Regression]") {
    const size_t unknown_layer = GENERATE(0, 1);
    const bool nearby = GENERATE(false, true);
    Layers layers{{rectangle(0, 10, 0, 10)}, {rectangle(0.2, 10.2, 0.2, 10.2)}};
    Contour unknown = nearby ? rectangle(-1, 11, -1, 11) : rectangle(100, 110, 100, 110);
    unknown.reliable = false; // Its artificial closing edge cannot establish a material state.
    layers[unknown_layer].push_back(std::move(unknown));
    Resolver resolver(std::move(layers));
    const auto result = resolver.resolve(0, 0, 0, 1);
    if (nearby) {
        CHECK(result.outcome == Outcome::None);
        CHECK(result.contours.empty());
    } else {
        REQUIRE(result.outcome == Outcome::Unique);
        CHECK(result.contours.front() == 0);
    }
    CHECK(resolver.resolve(unknown_layer, 1, 0, unknown_layer == 0 ? 1 : -1).outcome == Outcome::None);
}

TEST_CASE("Unknown reverse geometry preserves both split continuations for the legacy search", "[SeamContour][Regression]") {
    const bool mirrored = GENERATE(false, true);
    const bool descending = GENERATE(false, true);
    const size_t unknown_layer = GENERATE(0, 1);
    Contour source = rectangle(0, 10, 0, 10);
    source.points.insert(source.points.begin() + 1, Vec2d(5, 0));
    Layers layers{{source}, {rectangle(0, mirrored ? 3.8 : 4.9, 0, 10),
                              rectangle(mirrored ? 5.1 : 6.2, 10, 0, 10)}};
    if (descending) std::reverse(layers.begin(), layers.end());
    const size_t source_layer = descending ? 1 : 0;
    const int direction = descending ? -1 : 1;
    // Both reliable walls remain eligible before the unknown path is introduced.
    const auto baseline = Resolver(layers).resolve(source_layer, 0, 1, direction);
    REQUIRE(baseline.outcome == Outcome::Multiple);
    REQUIRE(baseline.contours == (std::vector<size_t>{0, 1}));
    Contour unknown;
    const double x = mirrored ? 2.8 : 7.2;
    unknown.points = {Vec2d(x, -0.2), Vec2d(x, 0.2)};
    unknown.reliable = false;
    // Outside the direct query, this path is near only one candidate's reverse query.
    layers[unknown_layer].push_back(std::move(unknown));
    const auto result = Resolver(std::move(layers)).resolve(source_layer, 0, 1, direction);
    CHECK(result.outcome == Outcome::None);
    CHECK(result.contours.empty());
}

TEST_CASE("Unknown reverse geometry invalidates a bounds certificate for the legacy search", "[SeamContour][Regression]") {
    const bool descending = GENERATE(false, true);
    const size_t unknown_layer = GENERATE(0, 1);
    Layers layers{{rectangle(0, 10, 0, 10)},
                  {rectangle(0.02, 10.02, 0.02, 10.02), rectangle(-10, -0.08, 0, 10)}};
    if (descending) std::reverse(layers.begin(), layers.end());
    const size_t source_layer = descending ? 1 : 0;
    const int direction = descending ? -1 : 1;
    // The near-coincident wall wins the reliable fast path, without a competing noise-band wall.
    const auto baseline = Resolver(layers).resolve(source_layer, 0, 0, direction);
    REQUIRE(baseline.outcome == Outcome::Unique);
    REQUIRE(baseline.method == Method::Bounds);
    REQUIRE(baseline.contours == (std::vector<size_t>{0}));
    Contour unknown;
    unknown.points = {Vec2d(1.61, 0), Vec2d(1.61, 0.04)};
    unknown.reliable = false;
    // The shifted foot sees this path; the direct query and the alternative foot do not.
    layers[unknown_layer].push_back(std::move(unknown));
    const auto result = Resolver(std::move(layers)).resolve(source_layer, 0, 0, direction);
    CHECK(result.outcome == Outcome::None);
    CHECK(result.contours.empty());
}

TEST_CASE("Unknown geometry near a rejected tube wall preserves the reliable continuation", "[SeamContour][Regression]") {
    const bool descending = GENERATE(false, true);
    const size_t unknown_layer = GENERATE(0, 1);
    const double width = 0.4, outer_radius = 4.0, inner_radius = 3.0, shift = 0.3;
    Options options;
    options.radius_width_factor = 4.0;
    const double radius = width * options.radius_width_factor;
    Layers layers{{circle(outer_radius, 0), circle(inner_radius, 0, true)},
                  {circle(outer_radius, shift, false, 97), circle(inner_radius, shift, true, 93)}};
    for (auto &layer : layers)
        for (auto &contour : layer) contour.width = contour.nozzle = width;
    if (descending) std::reverse(layers.begin(), layers.end());
    const size_t source_layer = descending ? 1 : 0;
    const int direction = descending ? -1 : 1;
    // The shifted outer wall is a local match; the hole's opposing normal rejects it first.
    const auto baseline = Resolver(layers, options).resolve(source_layer, 0, 0, direction, radius);
    REQUIRE(baseline.outcome == Outcome::Unique);
    REQUIRE(baseline.method == Method::Local);
    REQUIRE(baseline.contours == (std::vector<size_t>{0}));
    Contour unknown;
    const double x = inner_radius + shift - radius + 0.2;
    // Only a premature reverse query from the rejected hole would encounter this path.
    unknown.points = {Vec2d(x, -0.05), Vec2d(x, 0.05)};
    unknown.reliable = false;
    layers[unknown_layer].push_back(std::move(unknown));
    const auto result = Resolver(std::move(layers), options).resolve(source_layer, 0, 0, direction, radius);
    CHECK(result.outcome == Outcome::Unique);
    CHECK(result.contours == (std::vector<size_t>{0}));
}

TEST_CASE("A split or merge keeps ambiguous continuations near the slit", "[SeamContour][Regression]") {
    Resolver resolver({{rectangle(0, 10, 0, 10)},
                       {rectangle(0, 4.8, 0, 10), rectangle(5.2, 10, 0, 10)}});
    REQUIRE(resolver.resolve(0, 0, 0, 1).outcome == Outcome::Unique);
    CHECK(resolver.resolve(0, 0, 0, 1).contours.front() == 0);
    const auto merged = resolver.resolve(1, 0, 0, -1);
    REQUIRE(merged.outcome == Outcome::Unique);
    CHECK(merged.contours.front() == 0);
    // A vertex at the slit belongs to a separate source outline with the same bottom edge.
    Contour source = rectangle(0, 10, 0, 10);
    source.points.insert(source.points.begin()+1, Vec2d(5,0));
    Resolver at_slit({{source}, {rectangle(0,4.8,0,10), rectangle(5.2,10,0,10)}});
    const auto ambiguous = at_slit.resolve(0,0,1,1);
    CHECK(ambiguous.outcome == Outcome::Multiple);
    CHECK(ambiguous.contours == (std::vector<size_t>{0,1}));
}

TEST_CASE("Contour continuation preserves both tube walls despite candidate ordering", "[SeamContour]") {
    const double thickness = GENERATE(0.8, 1.2, 2.0);
    const double shift = GENERATE(0.0, 0.1, 0.4);
    Resolver resolver({{circle(4, 0), circle(4-thickness, 0, true)},
                       {circle(4-thickness, shift, true, 93), circle(4, shift, false, 97)}});
    for (size_t wall = 0; wall < 2; ++wall)
        for (size_t vertex = 0; vertex < 64; ++vertex) {
            const auto result = resolver.resolve(0, wall, vertex, 1);
            REQUIRE(result.outcome == Outcome::Unique);
            CHECK(result.contours.front() == 1-wall);
        }
}

TEST_CASE("Contour continuation follows an open wall after it becomes a hole", "[SeamContour][Regression]") {
    const bool reverse_order = GENERATE(false, true);
    Contour open = rectangle(0, 10, 0, 10);
    // Closing the right-hand slit changes boundary type without changing its local material side.
    open.points = {Vec2d(0, 0), Vec2d(10, 0), Vec2d(10, 1.2), Vec2d(6, 1.2),
                   Vec2d(6, 2.4), Vec2d(10, 2.4), Vec2d(10, 10), Vec2d(0, 10)};
    Contour hole = rectangle(6.004, 9.8, 1.204, 2.396);
    hole.hole = true;
    std::vector<Contour> closed{rectangle(0, 10, 0, 10), hole};
    if (reverse_order) std::reverse(closed.begin(), closed.end());
    const size_t hole_index = reverse_order ? 0 : 1;
    Resolver resolver({{open}, closed});
    const auto forward = resolver.resolve(0, 0, 3, 1);
    REQUIRE(forward.outcome == Outcome::Unique);
    REQUIRE(forward.contours.size() == 1);
    CHECK(forward.contours.front() == hole_index);
    CHECK(forward.method == Method::Local);
    const auto backward = resolver.resolve(1, hole_index, 0, -1);
    REQUIRE(backward.outcome == Outcome::Unique);
    REQUIRE(backward.contours.size() == 1);
    CHECK(backward.contours.front() == 0);
}

TEST_CASE("Matching contour bounds cannot certify a transverse wall across material", "[SeamContour][Regression]") {
    Contour open = rectangle(0, 10, 0, 10);
    open.points = {Vec2d(0, 0), Vec2d(10, 0), Vec2d(10, 1.2), Vec2d(6, 1.2),
                   Vec2d(6, 2.4), Vec2d(10, 2.4), Vec2d(10, 10), Vec2d(0, 10)};
    // The box is identical and the wrong outer wall is inside the legacy search radius.
    Resolver resolver({{open}, {rectangle(0, 10, 0, 10)}});
    const auto result = resolver.resolve(0, 0, 3, 1);
    CHECK(result.outcome == Outcome::None);
    CHECK(result.contours.empty());
}

TEST_CASE("A bounds match cannot hide another near-coincident local continuation", "[SeamContour][Regression]") {
    Contour source = rectangle(0, 10, 0, 10);
    source.points.insert(source.points.begin()+1, Vec2d(5, 0));
    // Only one box matches, but both local bottom edges are equally plausible.
    Resolver resolver({{source}, {rectangle(0, 10, 0, 10), rectangle(0, 13, 0, 10)}});
    const auto result = resolver.resolve(0, 0, 1, 1);
    CHECK(result.outcome == Outcome::Multiple);
    CHECK(result.contours.size() == 2);
}

TEST_CASE("A printed Benchy edge continues into the new hole after the opening closes", "[SeamContour][Regression]") {
    std::ifstream input(std::string(TEST_DATA_DIR) + "/seam_contour_benchy_split.txt");
    REQUIRE(bool(input));
    Layers layers(2);
    // Immutable printed contours from adjacent layers reproduce the real failure independently of chain weights.
    for (auto &layer : layers) {
        size_t count = 0;
        input >> count;
        layer.resize(count);
        for (Contour &c : layer) {
            size_t points = 0;
            input >> c.hole >> c.width >> c.nozzle >> points;
            for (size_t i = 0; i < points; ++i) {
                double x = 0., y = 0.;
                input >> x >> y;
                c.points.emplace_back(x, y);
            }
        }
    }
    REQUIRE(bool(input));
    Resolver resolver(std::move(layers));
    const auto forward = resolver.resolve(0, 0, 9, 1);
    REQUIRE(forward.outcome == Outcome::Unique);
    REQUIRE(forward.contours.size() == 1);
    CHECK(forward.contours.front() == 0);
    CHECK(forward.method == Method::Local);
    const auto backward = resolver.resolve(1, 0, 91, -1);
    REQUIRE(backward.outcome == Outcome::Unique);
    REQUIRE(backward.contours.size() == 1);
    CHECK(backward.contours.front() == 0);
}

TEST_CASE("Contour fallback rejects the translated-annulus false positive", "[SeamContour]") {
    // The true outer wall is outside the radius; the closer hole must never replace it.
    Resolver resolver({{circle(2, 0), circle(1, 0, true)}, {circle(2, 1.8), circle(1, 1.8, true)}});
    CHECK(resolver.resolve(0, 0, 0, 1).outcome != Outcome::Unique);
}

TEST_CASE("Contour fallback follows a local wall despite changed global bounds", "[SeamContour]") {
    Resolver resolver({{rectangle(0, 10, 0, 10)}, {rectangle(0, 13, 0, 10)}});
    const auto result = resolver.resolve(0, 0, 0, 1);
    REQUIRE(result.outcome == Outcome::Unique);
    CHECK(result.method == Method::Local);
    CHECK(result.contours.front() == 0);
}

TEST_CASE("Contour fallback handles an inclined wall without raster overlap", "[SeamContour]") {
    Resolver resolver({{rectangle(0, 10, 0, 10)}, {rectangle(-0.7, 13, -0.7, 10)}});
    const auto result = resolver.resolve(0, 0, 0, 1);
    CHECK(result.outcome == Outcome::Unique);
    CHECK(result.method == Method::Local);
}

TEST_CASE("Contour fallback does not resolve coincident competing surfaces by index", "[SeamContour]") {
    Resolver resolver({{rectangle(0, 10, 0, 10)},
                       {rectangle(0, 13, 0, 10), rectangle(0, 14, 0, 10)}});
    const auto result = resolver.resolve(0, 0, 0, 1);
    CHECK(result.outcome == Outcome::Multiple);
    CHECK(result.contours.size() == 2);
}

TEST_CASE("Contour proximity includes a long edge with distant endpoints", "[SeamContour]") {
    Contour current = rectangle(0, 10, 0, 10);
    current.points.insert(current.points.begin()+1, Vec2d(5, 0));
    Resolver resolver({{current}, {rectangle(0, 10, 0, 10)}});
    CHECK(resolver.resolve(0, 0, 1, 1).outcome == Outcome::Unique);
}

TEST_CASE("Contour dominance needs an advantage in distance and normal agreement", "[SeamContour]") {
    const bool reverse = GENERATE(false, true);
    Contour source = rectangle(-5, 5, 0, 10);
    source.points.insert(source.points.begin()+1, Vec2d(0, 0));
    Contour tilted = rectangle(0.3, 5, 0, 13);
    tilted.points[1].y() = 4.7 * std::tan(10.0 * std::acos(-1.0) / 180.0);
    // Both branches are admissible, but the left one wins by both configured margins.
    std::vector<Contour> target{rectangle(-5, -0.02, 0, 13), tilted};
    if (reverse) std::reverse(target.begin(), target.end());
    const auto result = Resolver({{source}, target}).resolve(0, 0, 1, 1);
    REQUIRE(result.outcome == Outcome::Unique);
    REQUIRE(result.contours.size() == 1);
    CHECK(result.contours.front() == (reverse ? 1 : 0));
}

TEST_CASE("Contour dominance keeps alternatives without enough distance advantage", "[SeamContour]") {
    Contour source = rectangle(-5, 5, 0, 10);
    source.points.insert(source.points.begin()+1, Vec2d(0, 0));
    Contour tilted = rectangle(0.08, 5, 0, 13);
    tilted.points[1].y() = 4.92 * std::tan(10.0 * std::acos(-1.0) / 180.0);
    const Layers layers{{source}, {rectangle(-5, -0.02, 0, 13), tilted}};
    const auto tied = Resolver(layers).resolve(0, 0, 1, 1);
    CHECK(tied.outcome == Outcome::Multiple);
    CHECK(tied.contours.size() == 2);
    Options options;
    options.dominance_width_factor = 0.1; // A smaller distance margin permits elimination.
    const auto result = Resolver(layers, options).resolve(0, 0, 1, 1);
    REQUIRE(result.outcome == Outcome::Unique);
    CHECK(result.contours.front() == 0);
}

TEST_CASE("Contour dominance keeps alternatives without enough angular advantage", "[SeamContour]") {
    Contour source = rectangle(-5, 5, 0, 10);
    source.points.insert(source.points.begin()+1, Vec2d(0, 0));
    Contour tilted = rectangle(0.3, 5, 0, 13);
    tilted.points[1].y() = 4.7 * std::tan(3.0 * std::acos(-1.0) / 180.0);
    const Layers layers{{source}, {rectangle(-5, -0.02, 0, 13), tilted}};
    CHECK(Resolver(layers).resolve(0, 0, 1, 1).contours.size() == 2);
    Options options;
    options.dominance_angle_degrees = 1.0; // Distance alone is insufficient with the default angular margin.
    CHECK(Resolver(layers, options).resolve(0, 0, 1, 1).contours.size() == 1);
}

TEST_CASE("Contour dominance keeps a farther branch with better normal agreement", "[SeamContour]") {
    Contour source = rectangle(-5, 5, 0, 10);
    source.points.insert(source.points.begin()+1, Vec2d(0, 0));
    Contour tilted = rectangle(-5, -0.02, 0, 13);
    tilted.points[0].y() = -4.98 * std::tan(10.0 * std::acos(-1.0) / 180.0);
    // The nearer branch is worse in orientation, so neither may discard the other.
    const auto result = Resolver({{source}, {tilted, rectangle(0.3, 5, 0, 13)}}).resolve(0, 0, 1, 1);
    CHECK(result.outcome == Outcome::Multiple);
    CHECK(result.contours.size() == 2);
}

TEST_CASE("Contour fallback accepts a source included in several reverse continuations", "[SeamContour]") {
    // A merge can legitimately map back to both lower contours.
    Resolver resolver({{rectangle(-5, -0.02, 0, 10), rectangle(0.02, 5, 0, 10)},
                       {rectangle(-5, 5, 0, 13)}});
    const auto result = resolver.resolve(0, 0, 1, 1);
    CHECK(result.outcome == Outcome::Unique);
    CHECK(result.contours.size() == 1);
}

TEST_CASE("Contour continuation refuses a disappeared wall and handles layer ends", "[SeamContour]") {
    Resolver resolver({{circle(2, 0)}, {circle(2, 20)}});
    CHECK(resolver.resolve(0, 0, 0, 1).outcome == Outcome::None);
    CHECK(resolver.resolve(0, 0, 0, -1).outcome == Outcome::None);
}

TEST_CASE("Contour continuation is independent of loop start and duplicates", "[SeamContour]") {
    Contour next = circle(2, 0.1);
    std::rotate(next.points.begin(), next.points.begin()+17, next.points.end());
    next.points.push_back(next.points.front());
    Resolver resolver({{circle(2, 0)}, {next}});
    const auto first = resolver.resolve(0, 0, 0, 1);
    const auto cached = resolver.resolve(0, 0, 0, 1);
    REQUIRE(first.outcome == Outcome::Unique);
    CHECK(cached.outcome == first.outcome);
    CHECK(cached.contours == first.contours);
}

TEST_CASE("Contour continuation finds the facing side of a shifted thin contour", "[SeamContour][Regression]") {
    Contour source = rectangle(0.2, 0.7, 0, 10);
    source.points.insert(source.points.begin()+2, Vec2d(0.7, 5));
    // The nearer left edge opposes the source normal; the right edge is its valid continuation.
    Resolver resolver({{source}, {rectangle(0.5, 1.0, 0, 10)}});
    const auto result = resolver.resolve(0, 0, 2, 1);
    REQUIRE(result.outcome == Outcome::Unique);
    CHECK(result.contours.front() == 0);
    CHECK(result.method == Method::Local);
}

TEST_CASE("Contour continuation respects the caller radius across cached queries", "[SeamContour][Regression]") {
    Contour source = rectangle(0, 10, 0, 10);
    source.width = 0.1;
    source.points.insert(source.points.begin()+2, Vec2d(10, 5));
    Resolver resolver({{source}, {rectangle(2, 12, 0, 10)}});
    // A wider seed permits this wall; the current layer's narrow width must not silently replace it.
    CHECK(resolver.resolve(0, 0, 2, 1, 2.4).outcome == Outcome::Unique);
    CHECK(resolver.resolve(0, 0, 2, 1, 0.4).outcome == Outcome::None);
    CHECK(resolver.resolve(0, 0, 2, 1).outcome == Outcome::None);
    CHECK(resolver.resolve(0, 0, 2, 1, 2.4).outcome == Outcome::Unique);
}

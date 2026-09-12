#include <catch2/catch_all.hpp>

#include "slic3r/GUI/AI/Model/SurfaceSelectionRefinement.hpp"

#include <numeric>

using namespace Slic3r;
namespace refinement = Slic3r::AI::SurfaceSelectionRefinement;

namespace {
struct Strip {
    indexed_triangle_set mesh;
    std::vector<RGBA> colors;
    std::vector<size_t> roi;
    explicit Strip(size_t columns = 4, bool split_colors = true) {
        for (size_t x = 0; x <= columns; ++x) {
            mesh.vertices.emplace_back(float(x), 0, 0);
            mesh.vertices.emplace_back(float(x), 1, 0);
            const RGBA color = split_colors && x >= columns / 2 ? RGBA {1, 0, 0, 1} : RGBA {0, 0, 1, 1};
            colors.push_back(color); colors.push_back(color);
        }
        for (size_t x = 0; x < columns; ++x) {
            const int i = int(x * 2);
            mesh.indices.emplace_back(i, i + 2, i + 1);
            mesh.indices.emplace_back(i + 2, i + 3, i + 1);
        }
        roi.resize(mesh.indices.size());
        std::iota(roi.begin(), roi.end(), size_t(0));
    }
};
bool has(const std::vector<size_t>& faces, size_t face) {
    return std::find(faces.begin(), faces.end(), face) != faces.end();
}
}

TEST_CASE("Boundary refinement honors positive and protected seeds and follows an appearance boundary", "[SurfaceSelectionRefinement]")
{
    Strip strip;
    const auto result = refinement::refine(strip.mesh, strip.colors, strip.roi, {0}, {7});
    REQUIRE(result.error.empty());
    REQUIRE_FALSE(result.canceled);
    REQUIRE(has(result.faces, 0));
    REQUIRE(has(result.faces, 1));
    REQUIRE_FALSE(has(result.faces, 6));
    REQUIRE_FALSE(has(result.faces, 7));
}

TEST_CASE("Explicit protection wins over the same appearance on adjacent faces", "[SurfaceSelectionRefinement]")
{
    Strip strip(4, false);
    const auto result = refinement::refine(strip.mesh, strip.colors, strip.roi, {0, 1}, {2, 3});
    REQUIRE(result.error.empty());
    REQUIRE(has(result.faces, 0));
    REQUIRE(has(result.faces, 1));
    REQUIRE_FALSE(has(result.faces, 2));
    REQUIRE_FALSE(has(result.faces, 3));
    // The protected column is a barrier. The unseeded region beyond it must
    // not become selected just because it has identical blue appearance.
    REQUIRE_FALSE(has(result.faces, 4));
    REQUIRE_FALSE(has(result.faces, 5));
    REQUIRE_FALSE(has(result.faces, 6));
    REQUIRE_FALSE(has(result.faces, 7));
}

TEST_CASE("Refinement cannot expand outside the coarse region", "[SurfaceSelectionRefinement]")
{
    Strip strip;
    const std::vector<size_t> roi {0, 1, 2, 3};
    const auto result = refinement::refine(strip.mesh, strip.colors, roi, {0}, {3});
    REQUIRE(result.error.empty());
    REQUIRE(has(result.faces, 0));
    for (size_t face : result.faces) REQUIRE(has(roi, face));
}

TEST_CASE("Disconnected same color parts need their own positive seed", "[SurfaceSelectionRefinement]")
{
    Strip strip;
    const int start = int(strip.mesh.vertices.size());
    strip.mesh.vertices.insert(strip.mesh.vertices.end(), {{0, 0, 0.01f}, {1, 0, 0.01f}, {0, 1, 0.01f}});
    strip.colors.insert(strip.colors.end(), 3, RGBA {0, 0, 1, 1});
    strip.mesh.indices.emplace_back(start, start + 1, start + 2);
    const size_t disconnected = strip.mesh.indices.size() - 1;
    strip.roi.push_back(disconnected);
    const auto result = refinement::refine(strip.mesh, strip.colors, strip.roi, {0}, {7});
    REQUIRE(result.error.empty());
    REQUIRE_FALSE(has(result.faces, disconnected));
    const auto seeded = refinement::refine(strip.mesh, strip.colors, strip.roi, {0, disconnected}, {7});
    REQUIRE(seeded.error.empty());
    REQUIRE(has(seeded.faces, disconnected));
}

TEST_CASE("Refinement leaves geometry and original color measurements unchanged", "[SurfaceSelectionRefinement]")
{
    Strip strip;
    const auto vertices = strip.mesh.vertices;
    const auto indices = strip.mesh.indices;
    const auto colors = strip.colors;
    const auto result = refinement::refine(strip.mesh, strip.colors, strip.roi, {0}, {7});
    REQUIRE(result.error.empty());
    REQUIRE(strip.colors == colors);
    REQUIRE(strip.mesh.vertices.size() == vertices.size());
    REQUIRE(strip.mesh.indices.size() == indices.size());
    for (size_t i = 0; i < vertices.size(); ++i) REQUIRE((strip.mesh.vertices[i] - vertices[i]).isZero());
    for (size_t i = 0; i < indices.size(); ++i) REQUIRE((strip.mesh.indices[i] - indices[i]).isZero());
}

TEST_CASE("Refinement requests both kinds of seed without replacing the current mask", "[SurfaceSelectionRefinement]")
{
    Strip strip;
    const auto missing_foreground = refinement::refine(strip.mesh, strip.colors, strip.roi, {}, {7});
    REQUIRE_FALSE(missing_foreground.error.empty());
    REQUIRE(missing_foreground.faces.empty());
    const auto missing_protection = refinement::refine(strip.mesh, strip.colors, strip.roi, {0}, {});
    REQUIRE_FALSE(missing_protection.error.empty());
    REQUIRE(missing_protection.faces.empty());
    const auto outside_protection = refinement::refine(strip.mesh, strip.colors, {0, 1}, {0}, {7});
    REQUIRE_FALSE(outside_protection.error.empty());
    REQUIRE(outside_protection.faces.empty());
    const auto conflicting = refinement::refine(strip.mesh, strip.colors, strip.roi, {0}, {0});
    REQUIRE_FALSE(conflicting.error.empty());
    REQUIRE(conflicting.faces.empty());
}

TEST_CASE("Refinement bounds the size of the unique coarse region", "[SurfaceSelectionRefinement]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh.indices.resize(refinement::maximum_roi_faces + 1, stl_triangle_vertex_indices(0, 1, 2));
    std::vector<size_t> roi(mesh.indices.size());
    std::iota(roi.begin(), roi.end(), size_t(0));
    const auto result = refinement::refine(mesh, std::vector<RGBA>(3, RGBA {0, 0, 1, 1}), roi, {0}, {1});
    REQUIRE(result.limit_exceeded);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE(result.faces.empty());
}

TEST_CASE("Repeated ROI and seed entries do not change the refined selection", "[SurfaceSelectionRefinement]")
{
    Strip strip;
    const auto ordinary = refinement::refine(strip.mesh, strip.colors, strip.roi, {0}, {7});
    const auto original_roi = strip.roi;
    strip.roi.insert(strip.roi.end(), original_roi.begin(), original_roi.end());
    const auto repeated = refinement::refine(strip.mesh, strip.colors, strip.roi, {0, 0}, {7, 7});
    REQUIRE(ordinary.error.empty());
    REQUIRE(repeated.error.empty());
    REQUIRE(repeated.faces == ordinary.faces);
}

TEST_CASE("Cancellation discards refinement candidates during bounded work", "[SurfaceSelectionRefinement]")
{
    Strip strip(1500);
    size_t calls = 0;
    const auto result = refinement::refine(strip.mesh, strip.colors, strip.roi, {0}, {strip.roi.back()},
                                         [&]() { return ++calls >= 3; });
    REQUIRE(result.canceled);
    REQUIRE(result.error.empty());
    REQUIRE(result.faces.empty());
    REQUIRE(calls >= 3);
}

TEST_CASE("Invalid source data cannot produce a refinement candidate", "[SurfaceSelectionRefinement]")
{
    Strip strip;
    const auto missing_colors = refinement::refine(strip.mesh, {}, strip.roi, {0}, {7});
    REQUIRE_FALSE(missing_colors.error.empty());
    REQUIRE(missing_colors.faces.empty());
    auto bad_roi = strip.roi;
    bad_roi.push_back(strip.mesh.indices.size());
    const auto wrong_mesh = refinement::refine(strip.mesh, strip.colors, bad_roi, {0}, {7});
    REQUIRE_FALSE(wrong_mesh.error.empty());
    REQUIRE(wrong_mesh.faces.empty());
}

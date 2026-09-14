#include <catch2/catch_all.hpp>

#include "slic3r/GUI/AI/Model/SurfaceSelection.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionRefinement.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

using namespace Slic3r;
namespace selection = Slic3r::AI::SurfaceSelection;

namespace {
void add_triangle(indexed_triangle_set& mesh, const Vec3f& a, const Vec3f& b, const Vec3f& c)
{
    const int first = int(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(), {a, b, c});
    mesh.indices.emplace_back(first, first + 1, first + 2);
}

struct Surface {
    AI::VertexColorRegionEditor editor;
    double scale {1.0};
    explicit Surface(indexed_triangle_set mesh, double scale_ = 1.0) : scale(scale_)
    {
        std::string error;
        for (auto& v : mesh.vertices) v *= float(scale);
        std::vector<RGBA> colors(mesh.vertices.size(), RGBA {0.1f, 0.3f, 0.2f, 1.0f});
        REQUIRE(editor.initialize(std::move(mesh), std::move(colors), error));
    }
    auto project() const {
        return [this](const Vec3f& p) -> std::optional<Vec2d> { return Vec2d(p.x() / scale, p.y() / scale); };
    }
    auto pick() const {
        return [this](const Vec2d& p) {
            return editor.pick_face(Vec3d(p.x() * scale, p.y() * scale, 100.0 * scale), Vec3d(0.0, 0.0, -1.0));
        };
    }
};
}

TEST_CASE("A screen brush selects the front surface without painting an occluded rear surface", "[SurfaceSelection]")
{
    indexed_triangle_set mesh;
    add_triangle(mesh, {0, 0, 0}, {10, 0, 0}, {0, 10, 0});
    add_triangle(mesh, {0, 0, 1}, {10, 0, 1}, {0, 10, 1});
    Surface surface(std::move(mesh));
    const auto result = selection::visible_brush_faces(surface.editor.mesh(), {{2, 2}}, 0.2, surface.project(), surface.pick());
    REQUIRE(result.faces == std::vector<size_t> {1});
    REQUIRE_FALSE(result.canceled);
    REQUIRE(result.visibility_queries > 0);
}

TEST_CASE("A small lasso selects an intersected large triangle even when its center lies outside", "[SurfaceSelection]")
{
    indexed_triangle_set mesh;
    add_triangle(mesh, {0, 0, 0}, {10, 0, 0}, {0, 10, 0});
    Surface surface(std::move(mesh));
    const auto result = selection::visible_polygon_faces(surface.editor.mesh(), {{1, 1}, {2, 1}, {2, 2}, {1, 2}},
                                                          surface.project(), surface.pick());
    REQUIRE(result.faces == std::vector<size_t> {0});
}

TEST_CASE("A crossing lasso selects faces whose vertices and center are outside the lasso", "[SurfaceSelection]")
{
    indexed_triangle_set mesh;
    add_triangle(mesh, {0, 0, 0}, {10, 0, 0}, {0, 10, 0});
    Surface surface(std::move(mesh));
    const auto result = selection::visible_polygon_faces(surface.editor.mesh(), {{-1, 1}, {11, 1}, {11, 1.2}, {-1, 1.2}},
                                                          surface.project(), surface.pick());
    REQUIRE(result.faces == std::vector<size_t> {0});
}

TEST_CASE("A concave lasso excludes the open notch while selecting both arms", "[SurfaceSelection]")
{
    indexed_triangle_set mesh;
    add_triangle(mesh, {0.2f, 2, 0}, {0.8f, 2, 0}, {0.2f, 2.6f, 0});
    add_triangle(mesh, {3.2f, 2, 0}, {3.8f, 2, 0}, {3.2f, 2.6f, 0});
    add_triangle(mesh, {1.2f, 2, 0}, {1.8f, 2, 0}, {1.2f, 2.6f, 0});
    Surface surface(std::move(mesh));
    const std::vector<Vec2d> polygon {{0, 0}, {4, 0}, {4, 4}, {3, 4}, {3, 1}, {1, 1}, {1, 4}, {0, 4}};
    const auto result = selection::visible_polygon_faces(surface.editor.mesh(), polygon, surface.project(), surface.pick());
    REQUIRE(result.faces == std::vector<size_t> {0, 1});
}

TEST_CASE("A continuous brush covers a fast drag without crossing to nearby same color parts", "[SurfaceSelection]")
{
    indexed_triangle_set mesh;
    add_triangle(mesh, {4, 0, 0}, {6, 0, 0}, {4, 2, 0});
    add_triangle(mesh, {4, 3, 0}, {6, 3, 0}, {4, 5, 0});
    Surface surface(std::move(mesh));
    const auto result = selection::visible_brush_faces(surface.editor.mesh(), {{-10, 0.4}, {10, 0.4}}, 0.1,
                                                       surface.project(), surface.pick());
    REQUIRE(result.faces == std::vector<size_t> {0});
    REQUIRE(result.tested_faces == 1);
}

TEST_CASE("Screen brush coverage remains consistent when model dimensions change", "[SurfaceSelection]")
{
    const double scale = GENERATE(0.1, 1.0, 100.0);
    indexed_triangle_set mesh;
    add_triangle(mesh, {0, 0, 0}, {10, 0, 0}, {0, 10, 0});
    add_triangle(mesh, {20, 0, 0}, {30, 0, 0}, {20, 10, 0});
    Surface surface(std::move(mesh), scale);
    const auto result = selection::visible_brush_faces(surface.editor.mesh(), {{1, 1}}, 0.5, surface.project(), surface.pick());
    REQUIRE(result.faces == std::vector<size_t> {0});
}

TEST_CASE("Selection cancellation discards the partial selection", "[SurfaceSelection]")
{
    indexed_triangle_set mesh;
    for (int i = 0; i < 300; ++i)
        add_triangle(mesh, {float(i), 0, 0}, {float(i) + 0.5f, 0, 0}, {float(i), 0.5f, 0});
    Surface surface(std::move(mesh));
    size_t calls = 0;
    const auto result = selection::visible_brush_faces(surface.editor.mesh(), {{-1, 0.1}, {301, 0.1}}, 0.5,
        surface.project(), surface.pick(), [&]() { return ++calls >= 3; });
    REQUIRE(result.canceled);
    REQUIRE(result.faces.empty());
    REQUIRE(result.tested_faces > 0);
}

TEST_CASE("Invalid gestures and projections produce no selected faces", "[SurfaceSelection]")
{
    indexed_triangle_set mesh;
    add_triangle(mesh, {0, 0, 0}, {10, 0, 0}, {0, 10, 0});
    Surface surface(std::move(mesh));
    const auto no_projection = [](const Vec3f&) -> std::optional<Vec2d> { return std::nullopt; };
    REQUIRE(selection::visible_brush_faces(surface.editor.mesh(), {{1, 1}}, 1.0, no_projection, surface.pick()).faces.empty());
    REQUIRE(selection::visible_brush_faces(surface.editor.mesh(), {{1, 1}}, -1.0, surface.project(), surface.pick()).faces.empty());
    REQUIRE(selection::visible_polygon_faces(surface.editor.mesh(), {{0, 0}, {1, 1}}, surface.project(), surface.pick()).faces.empty());
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(selection::visible_polygon_faces(surface.editor.mesh(), {{0, 0}, {1, 1}, {nan, 2}}, surface.project(), surface.pick()).faces.empty());
}

TEST_CASE("Selection requires a successful visibility hit even when the lasso covers the model", "[SurfaceSelection]")
{
    indexed_triangle_set mesh;
    add_triangle(mesh, {0, 0, 0}, {10, 0, 0}, {0, 10, 0});
    Surface surface(std::move(mesh));
    const auto missed = [](const Vec2d&) -> std::optional<size_t> { return std::nullopt; };
    const auto result = selection::visible_polygon_faces(surface.editor.mesh(), {{-1, -1}, {11, -1}, {11, 11}, {-1, 11}},
                                                          surface.project(), missed);
    REQUIRE(result.faces.empty());
    REQUIRE(result.visibility_queries > 0);
}

TEST_CASE("An explicitly supplied local artifact supports measured visible surface editing", "[SurfaceSelection][.LocalEditBenchmark]")
{
    const char* fixture = std::getenv("ORCASLICER_LOCAL_EDIT_FIXTURE");
    if (!fixture || !*fixture) SKIP("Set ORCASLICER_LOCAL_EDIT_FIXTURE to an existing local OBJ or GLB to run this opt-in benchmark.");
    using Clock = std::chrono::steady_clock;
    const auto elapsed_ms = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };
    nlohmann::json metrics { {"kind", "offline-local-edit-benchmark"}, {"view", "+X orthographic"},
                             {"gui_acceptance", false}, {"pick_witness_instrumentation", true} };
    TriangleMesh loaded;
    ObjInfo colors;
    std::string error;
    auto started = Clock::now();
    // This adapter reads the explicitly supplied artifact; it has no provider
    // calls. The benchmark neither generates nor writes a model.
    INFO("Fixture: " << fixture);
    REQUIRE(AI::load_model_artifact(boost::filesystem::path(fixture), loaded, colors, error));
    metrics["load_ms"] = elapsed_ms(started);
    metrics["faces"] = loaded.its.indices.size();
    metrics["vertices"] = loaded.its.vertices.size();
    REQUIRE_FALSE(loaded.its.vertices.empty());
    Vec3d minimum = loaded.its.vertices.front().cast<double>(), maximum = minimum;
    for (const auto& v : loaded.its.vertices) { minimum = minimum.cwiseMin(v.cast<double>()); maximum = maximum.cwiseMax(v.cast<double>()); }
    const Vec3d center = (minimum + maximum) * 0.5;
    const double extent = std::max(maximum.y() - minimum.y(), maximum.z() - minimum.z());
    REQUIRE(extent > 0.0);
    const double scale = 800.0 / extent;
    const double origin_x = maximum.x() + std::max(1.0, (maximum - minimum).norm());
    AI::VertexColorRegionEditor editor;
    started = Clock::now();
    REQUIRE(editor.initialize(std::move(loaded.its), std::move(colors.vertex_colors), error));
    metrics["prepare_editor_ms"] = elapsed_ms(started);
    const auto project = [&](const Vec3f& point) -> std::optional<Vec2d> {
        return Vec2d(400.0 + (point.y() - center.y()) * scale, 400.0 - (point.z() - center.z()) * scale);
    };
    const auto ray_origin = [&](const Vec2d& point) { return Vec3d(origin_x, center.y() + (point.x() - 400.0) / scale,
                                                                 center.z() - (point.y() - 400.0) / scale); };
    std::unordered_map<size_t, Vec2d> visible_witnesses;
    const auto pick = [&](const Vec2d& point) {
        const auto hit = editor.pick_face(ray_origin(point), Vec3d(-1, 0, 0));
        if (hit) visible_witnesses.emplace(*hit, point);
        return hit;
    };
    const std::vector<Vec2d> polygon {{320, 320}, {480, 320}, {480, 480}, {320, 480}};
    started = Clock::now();
    const auto roi = selection::visible_polygon_faces(editor.mesh(), polygon, project, pick);
    metrics["lasso_ms"] = elapsed_ms(started);
    metrics["roi_faces"] = roi.faces.size();
    metrics["tested_faces"] = roi.tested_faces;
    metrics["visibility_queries"] = roi.visibility_queries;
    REQUIRE_FALSE(roi.canceled);
    REQUIRE_FALSE(roi.faces.empty());
    // Check an actual first-hit witness for every selected face. A face whose
    // center is hidden may still have a visible sliver, so do not assume that
    // center visibility alone defines the result.
    size_t invalid_witnesses = 0;
    for (size_t face : roi.faces) {
        const auto found = visible_witnesses.find(face);
        if (found == visible_witnesses.end()) { ++invalid_witnesses; continue; }
        const auto hit = editor.pick_face(ray_origin(found->second), Vec3d(-1, 0, 0));
        if (!hit || *hit != face) ++invalid_witnesses;
    }
    metrics["invalid_visibility_witnesses"] = invalid_witnesses;
    REQUIRE(invalid_witnesses == 0);
    if (roi.faces.size() >= 2 && roi.faces.size() <= AI::SurfaceSelectionRefinement::maximum_roi_faces) {
        // Use two far-apart actual visible witnesses, independent of portrait,
        // clothing, or color labels. Both brush masks are confined to the ROI.
        const Vec2d positive_point = visible_witnesses.at(roi.faces.front());
        size_t background_face = roi.faces.front();
        double farthest = -1;
        for (size_t face : roi.faces) {
            const double distance = (visible_witnesses.at(face) - positive_point).squaredNorm();
            if (distance > farthest) { farthest = distance; background_face = face; }
        }
        const Vec2d negative_point = visible_witnesses.at(background_face);
        std::unordered_set<size_t> roi_ids(roi.faces.begin(), roi.faces.end());
        started = Clock::now();
        const auto positive_brush = selection::visible_brush_faces(editor.mesh(), {positive_point}, 3.0, project, pick);
        metrics["foreground_brush_ms"] = elapsed_ms(started);
        started = Clock::now();
        const auto negative_brush = selection::visible_brush_faces(editor.mesh(), {negative_point}, 3.0, project, pick);
        metrics["protection_brush_ms"] = elapsed_ms(started);
        std::vector<size_t> foreground, background;
        std::unordered_set<size_t> foreground_ids;
        for (size_t face : positive_brush.faces) if (roi_ids.count(face)) { foreground.push_back(face); foreground_ids.insert(face); }
        for (size_t face : negative_brush.faces) if (roi_ids.count(face) && !foreground_ids.count(face)) background.push_back(face);
        if (foreground.empty()) { foreground.push_back(roi.faces.front()); foreground_ids.insert(roi.faces.front()); }
        if (background.empty() && !foreground_ids.count(background_face)) background.push_back(background_face);
        if (!background.empty()) {
            started = Clock::now();
            const auto refined = AI::SurfaceSelectionRefinement::refine(editor.mesh(), editor.vertex_colors(), roi.faces, foreground, background);
            metrics["refine_ms"] = elapsed_ms(started);
            metrics["refined_faces"] = refined.faces.size();
            metrics["foreground_seeds"] = foreground.size();
            metrics["protected_seeds"] = background.size();
            INFO(refined.error);
            REQUIRE(refined.error.empty());
            REQUIRE_FALSE(refined.canceled);
            std::unordered_set<size_t> selected(refined.faces.begin(), refined.faces.end());
            size_t seed_errors = 0, outside_roi = 0;
            for (size_t face : foreground) if (!selected.count(face)) ++seed_errors;
            for (size_t face : background) if (selected.count(face)) ++seed_errors;
            for (size_t face : refined.faces) if (!roi_ids.count(face)) ++outside_roi;
            metrics["hard_seed_errors"] = seed_errors;
            metrics["faces_outside_roi"] = outside_roi;
            REQUIRE(seed_errors == 0);
            REQUIRE(outside_roi == 0);
        } else metrics["refinement_skipped"] = "The two test brushes overlap every candidate face.";
    } else metrics["refinement_skipped"] = "The central ROI is outside the 2 to 150000 face benchmark range.";
    std::cout << "LOCAL_EDIT_BENCHMARK " << metrics.dump() << '\n';
}

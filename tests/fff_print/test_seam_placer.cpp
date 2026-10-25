#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"
#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/GCode/SeamContour.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <utility>
#include <cstddef>
#include "libslic3r/Point.hpp"
#include <vector>
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Print.hpp"

using namespace Slic3r;

namespace {
struct PipelineFixture {
    Model model;
    Print print;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    explicit PipelineFixture(bool trapezoid = false, float height = 0.4f)
    {
        auto mesh = its_make_cube(20, 20, height);
        if (trapezoid) {
            // Opposite painted sides have deliberately different lengths: 20 mm and 6 mm.
            for (auto &vertex : mesh.vertices)
                if (vertex.y() == 20.0f) vertex.x() = 7.0f + 0.3f * vertex.x();
        }
        config.set_deserialize_strict("seam_position", "back"); // Rear needs no visibility ray tracing.
        config.set_deserialize_strict("layer_height", "0.2");
        config.set_deserialize_strict("initial_layer_print_height", "0.2");
        config.set_deserialize_strict("outer_wall_line_width", "0.4");
        config.set_deserialize_strict("initial_layer_line_width", "0.4");
        config.set_deserialize_strict("wall_loops", "1");
        config.set_deserialize_strict("raft_layers", "0");
        config.set_deserialize_strict("gcode_comments", "1"); // Match init_print so later apply calls change only the model.
        Test::init_print({TriangleMesh(std::move(mesh))}, print, model, config);
    }

    void paint(bool all_faces)
    {
        auto &volume = *model.objects.front()->volumes.front();
        const auto &mesh = volume.mesh();
        const auto bounds = mesh.bounding_box();
        TriangleSelector selector(mesh);
        size_t painted = 0;
        for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
            const auto &face = mesh.its.indices[i];
            bool lower = true, upper = true;
            for (int j = 0; j < 3; ++j) {
                const auto &v = mesh.its.vertices[face[j]];
                lower = lower && std::abs(double(v.y()) - bounds.min.y()) < 1e-6;
                upper = upper && std::abs(double(v.y()) - bounds.max.y()) < 1e-6;
            }
            if (all_faces || lower || upper) {
                selector.set_facet(int(i), EnforcerBlockerType::ENFORCER);
                ++painted;
            }
        }
        REQUIRE(painted > 0);
        volume.seam_facets.set(selector);
        print.apply(model, config);
    }

    PrintObject &prepare()
    {
        REQUIRE(print.objects().size() == 1);
        auto &object = *print.get_object(0);
        object.slice(); // Real layers/regions are sufficient: each test supplies its own perimeter loops.
        REQUIRE_FALSE(object.layers().empty());
        REQUIRE_FALSE(object.layers().front()->regions().empty());
        return object;
    }

    Points points_in_layer(const PrintObject &object, const std::vector<Vec2d> &xy) const
    {
        const auto &volume = *object.model_object()->volumes.front();
        const auto minimum = volume.mesh().bounding_box().min;
        const Transform3d transform = object.trafo_centered() * volume.get_matrix();
        Points points;
        for (const auto &point : xy) {
            // add_volume centers the mesh; restore its local offset before applying the slicing transform.
            const Vec3d local = minimum + Vec3d(point.x(), point.y(), 0.2);
            const Vec3d placed = transform * local;
            points.emplace_back(scale_(placed.x()), scale_(placed.y()));
        }
        return points;
    }
};

// Wall loop of the given role, as one path or one path per edge; inset_idx is set the way the perimeter generators do.
ExtrusionLoop make_loop(Points points, ExtrusionRole role, int inset_idx, bool separate_paths = false)
{
    REQUIRE(points.size() >= 3);
    points.push_back(points.front());
    ExtrusionPaths paths;
    if (separate_paths) {
        for (size_t i = 1; i < points.size(); ++i) {
            ExtrusionPath path(role, 0.08, 0.4f, 0.2f);
            path.polyline = Polyline3(Polyline(Points{points[i - 1], points[i]}));
            paths.push_back(std::move(path));
        }
    } else {
        ExtrusionPath path(role, 0.08, 0.4f, 0.2f);
        path.polyline = Polyline3(Polyline(std::move(points)));
        paths.push_back(std::move(path));
    }
    ExtrusionLoop loop(std::move(paths));
    loop.inset_idx = inset_idx;
    return loop;
}

void append_loop(LayerRegion &region, Points points, bool separate_paths = false)
{
    // Inject deterministic external loops while keeping the real layer, region and paint-query machinery.
    region.perimeters.append(make_loop(std::move(points), erExternalPerimeter, -1, separate_paths));
}

LayerRegion &clear_first_layer(PrintObject &object)
{
    Layer &layer = *object.layers().front();
    for (LayerRegion *region : layer.regions()) region->perimeters.clear();
    return *layer.get_region(0);
}

// Replaces the perimeters of every layer with one external loop through outline(layer index).
template<typename Outline> void set_loop_on_every_layer(PrintObject &object, Outline &&outline)
{
    for (size_t i = 0; i < object.layers().size(); ++i) {
        Layer &layer = *object.layers()[i];
        for (LayerRegion *region : layer.regions()) region->perimeters.clear();
        append_loop(*layer.get_region(0), outline(i));
    }
}

// Start of each layer's loop after the seam is placed, in unscaled XY.
std::vector<Vec2d> placed_seams(const SeamPlacer &placer, const PrintObject &object)
{
    std::vector<Vec2d> starts;
    for (const Layer *layer : object.layers()) {
        const auto *loop = dynamic_cast<const ExtrusionLoop *>(layer->get_region(0)->perimeters.entities.front());
        REQUIRE(loop != nullptr);
        ExtrusionLoop copy = *loop;
        float overhang = 0.0f;
        placer.place_seam(layer, copy, Point(0, 0), overhang);
        starts.push_back(unscale(copy.first_point()));
    }
    return starts;
}

using SeamType = SeamPlacerImpl::EnforcedBlockedSeamPoint;

// Seam candidates of one loop through xy at z = 0; Neutral with angle 0 unless given. The loop starts at
// `offset`, after candidates of another loop.
std::vector<SeamPlacerImpl::SeamCandidate> loop_candidates(SeamPlacerImpl::Perimeter &perimeter, const std::vector<Vec2d> &xy,
                                                           const std::vector<SeamType> &types = {},
                                                           const std::vector<float> &angles = {}, size_t offset = 0)
{
    static SeamPlacerImpl::Perimeter other;
    perimeter.start_index = offset;
    perimeter.end_index = offset + xy.size();
    std::vector<SeamPlacerImpl::SeamCandidate> points;
    points.reserve(offset + xy.size());
    for (size_t i = 0; i < offset; ++i)
        points.emplace_back(Vec3f(100.0f + float(i), 100.0f, 0.0f), other, 0.0f, SeamType::Enforced);
    for (size_t i = 0; i < xy.size(); ++i)
        points.emplace_back(Vec3f(float(xy[i].x()), float(xy[i].y()), 0.0f), perimeter, angles.empty() ? 0.0f : angles[i],
                            types.empty() ? SeamType::Neutral : types[i]);
    return points;
}

Vec2d placed_xy(const std::vector<SeamPlacerImpl::SeamCandidate> &points, size_t index, const Vec2d &target)
{
    return SeamPlacerImpl::place_on_loop(points, index, Vec3f(float(target.x()), float(target.y()), 0.0f))
        .head<2>()
        .cast<double>();
}
} // namespace

TEST_CASE("Paint proximity beyond painted facet heights remains a soft seam preference", "[SeamPlacer][Regression]")
{
    PipelineFixture fixture;
    auto &volume = *fixture.model.objects.front()->volumes.front();
    const auto &mesh = volume.mesh();
    const double bottom = mesh.bounding_box().min.z();
    TriangleSelector selector(mesh);
    // Only paint below the slicing plane: intersecting side paint exercises the separate exact-range tests.
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        const auto &face = mesh.its.indices[i];
        bool on_bottom = true;
        for (int j = 0; j < 3; ++j)
            on_bottom = on_bottom && std::abs(mesh.its.vertices[face[j]].z() - bottom) < 1e-6;
        if (on_bottom) selector.set_facet(int(i), EnforcerBlockerType::ENFORCER);
    }
    volume.seam_facets.set(selector);
    fixture.print.apply(fixture.model, fixture.config);
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);
    append_loop(region, fixture.points_in_layer(object, {{0, 0}, {20, 0}, {20, 20}, {0, 20}}));
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 1);
    // The horizontal painted face is below the slice, but the old paint-radius test still marks candidates.
    CHECK(std::count_if(data.points.begin(), data.points.end(), [](const auto &point) {
        return point.type == SeamPlacerImpl::EnforcedBlockedSeamPoint::Enforced;
    }) > 0);
}

TEST_CASE("Painted seams prefer the longer candidate patch regardless of contour origin", "[SeamPlacer][Regression]")
{
    const bool clockwise = GENERATE(false, true);
    const bool wrapped = GENERATE(false, true);
    CAPTURE(clockwise, wrapped);
    PipelineFixture fixture(true);
    fixture.paint(false);
    PrintObject &object = fixture.prepare();
    REQUIRE(object.model_object()->volumes.size() == 1);
    CHECK_FALSE(object.model_object()->volumes.front()->is_precise_seam());

    auto &region = clear_first_layer(object);
    // A neutral loop ensures that patch indices are offsets in the layer, not zero-based local indices.
    append_loop(region, fixture.points_in_layer(object, {{9, 8}, {11, 8}, {10, 10}}));
    Points outline = fixture.points_in_layer(object, {{10, 20}, {7, 20}, {3.5, 10}, {0, 0}, {10, 0}, {20, 0}, {13, 20}});
    if (clockwise) std::reverse(outline.begin() + 1, outline.end()); // Keep the same starting vertex.
    if (!wrapped) {
        const Point neutral = fixture.points_in_layer(object, {{3.5, 10}}).front();
        const auto start = std::find(outline.begin(), outline.end(), neutral);
        REQUIRE(start != outline.end());
        std::rotate(outline.begin(), start, outline.end());
    }
    append_loop(region, std::move(outline));
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 2);
    const auto &perimeter = data.perimeters[1];
    REQUIRE(perimeter.start_index > 0);
    REQUIRE(perimeter.end_index > perimeter.start_index);
    using Type = SeamPlacerImpl::EnforcedBlockedSeamPoint;
    CHECK((data.points[perimeter.start_index].type == Type::Enforced) == wrapped);
    if (wrapped) CHECK(data.points[perimeter.end_index - 1].type == Type::Enforced);

    const auto extremes = fixture.points_in_layer(object, {{10, 0}, {10, 20}});
    const double bottom_y = unscale<double>(extremes[0].y()), top_y = unscale<double>(extremes[1].y());
    size_t bottom_count = 0, top_count = 0, centers = 0;
    for (size_t i = perimeter.start_index; i < perimeter.end_index; ++i) {
        const auto &candidate = data.points[i];
        if (candidate.type == Type::Enforced) {
            // Include the small paint-radius fringe at the ends of each face.
            const bool bottom = std::abs(candidate.position.y() - bottom_y) < 0.5;
            const bool top = std::abs(candidate.position.y() - top_y) < 0.5;
            const bool on_painted_face = bottom || top;
            CAPTURE(candidate.position.x(), candidate.position.y());
            CHECK(on_painted_face);
            bottom_count += bottom;
            top_count += top;
        }
        if (candidate.central_enforcer) {
            ++centers;
            CHECK(candidate.type == Type::Enforced);
            CHECK_THAT(double(candidate.position.y()), Catch::Matchers::WithinAbs(bottom_y, 0.5));
        }
    }
    REQUIRE(top_count > 0);
    REQUIRE(bottom_count > top_count);
    CHECK(centers == 1); // The old wrapped-length formula instead selected the short top patch.
}

TEST_CASE("Full surface paint preserves unpainted candidates and placed seams", "[SeamPlacer][Regression]")
{
    const std::string mode = GENERATE("back", "aligned", "random");
    const bool bevel = GENERATE(false, true);
    CAPTURE(mode, bevel);
    std::vector<std::vector<Vec3f>> expected_candidates;
    std::vector<Vec2d> expected_seams;
    for (const bool painted : {false, true}) {
        PipelineFixture fixture(false, 2.0f);
        fixture.config.set_deserialize_strict("seam_position", mode);
        if (painted) fixture.paint(true);
        fixture.print.apply(fixture.model, fixture.config); // Apply the requested seam mode in both comparisons.
        PrintObject &object = fixture.prepare();
        const std::vector<Vec2d> outline = bevel ?
            std::vector<Vec2d>{{0, 1}, {1, 0}, {19, 0}, {20, 1}, {20, 19}, {19, 20}, {1, 20}, {0, 19}} :
            std::vector<Vec2d>{{0, 0}, {20, 0}, {20, 20}, {0, 20}};
        set_loop_on_every_layer(object, [&](size_t) { return fixture.points_in_layer(object, outline); });
        SeamPlacer placer;
        placer.init(fixture.print, [] {});
        const auto &layers = placer.m_seam_per_object.at(&object).layers;
        if (!painted) {
            for (const auto &layer : layers) {
                std::vector<Vec3f> positions;
                for (const auto &candidate : layer.points) positions.push_back(candidate.position);
                expected_candidates.push_back(std::move(positions));
            }
            expected_seams = placed_seams(placer, object);
        } else {
            REQUIRE(layers.size() == expected_candidates.size());
            for (size_t i = 0; i < layers.size(); ++i) {
                // Equality of candidates catches paint oversampling even when a sharp seam happens to match.
                REQUIRE(layers[i].points.size() == expected_candidates[i].size());
                for (size_t j = 0; j < layers[i].points.size(); ++j) {
                    const auto &candidate = layers[i].points[j];
                    CHECK(candidate.type == SeamType::Neutral);
                    CHECK_FALSE(candidate.central_enforcer);
                    CHECK_THAT(double((candidate.position - expected_candidates[i][j]).norm()),
                               Catch::Matchers::WithinAbs(0.0, 1e-6));
                }
            }
            const auto actual = placed_seams(placer, object);
            REQUIRE(actual.size() == expected_seams.size());
            for (size_t i = 0; i < actual.size(); ++i)
                CHECK_THAT((actual[i] - expected_seams[i]).norm(), Catch::Matchers::WithinAbs(0.0, 1e-6));
        }
    }
}

TEST_CASE("Full surface paint preserves strong Precise Seam positions", "[SeamPlacer][PreciseSeam][Regression]")
{
    std::vector<Vec2d> expected;
    for (const bool painted : {false, true}) {
        PipelineFixture fixture(false, 2.0f);
        fixture.config.set_deserialize_strict("seam_position", "aligned");
        if (painted) fixture.paint(true);
        auto *part = fixture.model.objects.front()->volumes.front();
        const Vec3d center = part->get_matrix() * (part->mesh().bounding_box().min + Vec3d(10, 0, 1));
        auto *helper = fixture.model.objects.front()->add_volume(make_cube(4, 4, 4));
        helper->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
        helper->set_offset(center); // Cross the front side without adding another printing surface.
        fixture.print.apply(fixture.model, fixture.config);
        PrintObject &object = fixture.prepare();
        set_loop_on_every_layer(object, [&](size_t) {
            return fixture.points_in_layer(object, {{0, 0}, {20, 0}, {20, 20}, {0, 20}});
        });
        SeamPlacer placer;
        placer.init(fixture.print, [] {});
        for (const auto &layer : placer.m_seam_per_object.at(&object).layers) {
            REQUIRE(layer.perimeters.size() == 1);
            const auto &perimeter = layer.perimeters.front();
            REQUIRE(perimeter.precise_seam_point.has_value());
            // Strong placement stays exact.
            CHECK_THAT(double((perimeter.final_seam_position - *perimeter.precise_seam_point).norm()),
                       Catch::Matchers::WithinAbs(0.0, 1e-6));
        }
        const auto actual = placed_seams(placer, object);
        if (!painted) expected = actual;
        else {
            REQUIRE(actual.size() == expected.size());
            for (size_t i = 0; i < actual.size(); ++i)
                CHECK_THAT((actual[i] - expected[i]).norm(), Catch::Matchers::WithinAbs(0.0, 1e-6));
        }
    }
}

TEST_CASE("Complete side-wall paint is neutral away from unpainted caps", "[SeamPlacer][Regression]")
{
    PipelineFixture fixture(false, 2.0f);
    auto &volume = *fixture.model.objects.front()->volumes.front();
    const auto &mesh = volume.mesh();
    TriangleSelector selector(mesh);
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        const auto &face = mesh.its.indices[i];
        const auto &a = mesh.its.vertices[face[0]], &b = mesh.its.vertices[face[1]], &c = mesh.its.vertices[face[2]];
        if (std::abs((b - a).cross(c - a).z()) < 1e-6)
            selector.set_facet(int(i), EnforcerBlockerType::ENFORCER); // Keep the top and bottom gray.
    }
    volume.seam_facets.set(selector);
    fixture.print.apply(fixture.model, fixture.config);
    PrintObject &object = fixture.prepare();
    set_loop_on_every_layer(object, [&](size_t) {
        return fixture.points_in_layer(object, {{0, 0}, {20, 0}, {20, 20}, {0, 20}});
    });
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &layers = placer.m_seam_per_object.at(&object).layers;
    REQUIRE(layers.size() == 10);
    for (const auto &candidate : layers[4].points) CHECK(candidate.type == SeamType::Neutral);
    // Near a gray cap the proof is intentionally conservative and retains the old paint semantics.
    CHECK(std::any_of(layers.front().points.begin(), layers.front().points.end(), [](const auto &candidate) {
        return candidate.type == SeamType::Enforced;
    }));
}

TEST_CASE("A nearby gray or blocked facet prevents full-green normalization", "[SeamPlacer][Regression]")
{
    const auto other = GENERATE(EnforcerBlockerType::NONE, EnforcerBlockerType::BLOCKER);
    PipelineFixture fixture(false, 2.0f);
    auto &volume = *fixture.model.objects.front()->volumes.front();
    const auto &mesh = volume.mesh();
    const float front = mesh.bounding_box().min.y();
    TriangleSelector selector(mesh);
    bool changed = false;
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        selector.set_facet(int(i), EnforcerBlockerType::ENFORCER);
        const auto &face = mesh.its.indices[i];
        if (!changed && std::abs(mesh.its.vertices[face[0]].y() - front) < 1e-6 &&
            std::abs(mesh.its.vertices[face[1]].y() - front) < 1e-6 &&
            std::abs(mesh.its.vertices[face[2]].y() - front) < 1e-6) {
            selector.set_facet(int(i), other); // Vertices may still see neighboring green facets within line width.
            changed = true;
        }
    }
    REQUIRE(changed);
    volume.seam_facets.set(selector);
    fixture.print.apply(fixture.model, fixture.config);
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);
    append_loop(region, fixture.points_in_layer(object, {{0, 0}, {20, 0}, {20, 20}, {0, 20}}));
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &points = placer.m_seam_per_object.at(&object).layers.front().points;
    CHECK(std::any_of(points.begin(), points.end(), [](const auto &candidate) { return candidate.type == SeamType::Enforced; }));
    if (other == EnforcerBlockerType::BLOCKER)
        CHECK(std::any_of(points.begin(), points.end(), [](const auto &candidate) { return candidate.type == SeamType::Blocked; }));
}

TEST_CASE("Painted seams run smoothly along a smooth contour", "[SeamPlacer][Regression]")
{
    // 10 layers of 0.2 mm: enough for the seams to be aligned as one string.
    PipelineFixture fixture(false, 2.0f);
    fixture.paint(false);
    PrintObject &object = fixture.prepare();
    REQUIRE(object.layers().size() == 10);
    // A circle of radius 6 mm touching the painted front face only near its lowest point. Its vertices
    // rotate by an irregular fraction of a vertex step on every layer, so the candidates differ between layers.
    const Vec2d center(10.0, 6.2);
    const double radius = 6.0;
    const size_t vertex_count = 120;
    const double vertex_step = 2.0 * PI / double(vertex_count);
    const double phases[] = {0.0, 0.62, 0.21, 0.87, 0.40, 0.05, 0.73, 0.31, 0.94, 0.18};
    set_loop_on_every_layer(object, [&](size_t layer) {
        std::vector<Vec2d> xy;
        const double phase = phases[layer] * vertex_step;
        for (size_t i = 0; i < vertex_count; ++i) {
            const double angle = phase + vertex_step * double(i);
            xy.emplace_back(center + radius * Vec2d(std::cos(angle), std::sin(angle)));
        }
        return fixture.points_in_layer(object, xy);
    });
    SeamPlacer placer;
    placer.init(fixture.print, [] {});

    const Vec2d placed_center = unscale(fixture.points_in_layer(object, {center}).front());
    const std::vector<Vec2d> seams = placed_seams(placer, object);
    std::vector<double> along; // Seam position along the loop, mm.
    for (size_t i = 0; i < seams.size(); ++i) {
        const Vec2d offset = seams[i] - placed_center;
        const double angle = std::atan2(offset.y(), offset.x());
        CAPTURE(i, seams[i].x(), seams[i].y());
        // The seam stays near the lowest point, where the circle meets the painted face (+-0.3 rad).
        CHECK_THAT(angle, Catch::Matchers::WithinAbs(-PI / 2.0, 0.3));
        along.push_back(radius * angle);
    }
    // The seam line may lean with the curve fitted to the candidates, but must neither step nor kink between
    // layers. The scale is the candidate spacing near paint: a 0.4 pull toward the candidate passes at least
    // 0.4 of it into the seam, while the step must stay below a quarter of it and its change below 0.15 of it.
    const double spacing = SeamPlacer::enforcer_oversampling_distance;
    for (size_t i = 1; i < along.size(); ++i) {
        CAPTURE(i, along[i - 1], along[i]);
        CHECK_THAT(along[i] - along[i - 1], Catch::Matchers::WithinAbs(0.0, 0.25 * spacing));
    }
    for (size_t i = 1; i + 1 < along.size(); ++i) {
        CAPTURE(i, along[i - 1], along[i], along[i + 1]);
        CHECK_THAT(along[i + 1] - 2.0 * along[i] + along[i - 1], Catch::Matchers::WithinAbs(0.0, 0.15 * spacing));
    }
}

TEST_CASE("Seam strings leave out no layer below their start", "[SeamPlacer][Regression]")
{
    // 41 layers of 0.2 mm with one square each. Painting the top face enforces the candidates of the top
    // two layers only (paint radius 0.4 mm), so the aligned string starts at layer 39, the first of them.
    // Layer 40 is a small square in the middle, out of reach of layer 39's seam, so the string cannot
    // continue upward and turns down from layer 39. The string is long enough that alignment tries only
    // some alternative starts, so a layer skipped when turning down stays out of it.
    PipelineFixture fixture(false, 8.2f);
    {
        auto &volume = *fixture.model.objects.front()->volumes.front();
        const auto &mesh = volume.mesh();
        const double top = mesh.bounding_box().max.z();
        TriangleSelector selector(mesh);
        for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
            bool on_top = true;
            for (int j = 0; j < 3; ++j)
                on_top = on_top && std::abs(double(mesh.its.vertices[mesh.its.indices[i][j]].z()) - top) < 1e-6;
            if (on_top)
                selector.set_facet(int(i), EnforcerBlockerType::ENFORCER);
        }
        volume.seam_facets.set(selector);
        fixture.print.apply(fixture.model, fixture.config);
    }
    PrintObject &object = fixture.prepare();
    REQUIRE(object.layers().size() == 41);
    set_loop_on_every_layer(object, [&](size_t layer) {
        if (layer == 40)
            return fixture.points_in_layer(object, {{9, 9}, {11, 9}, {11, 11}, {9, 11}});
        return fixture.points_in_layer(object, {{0, 0}, {20, 0}, {20, 20}, {0, 20}});
    });
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &layers = placer.m_seam_per_object.at(&object).layers;
    // Layer 0 is left out: with no layer below, its own seam differs and it does not join the string here.
    for (size_t i = 1; i <= 39; ++i) {
        CAPTURE(i);
        REQUIRE(layers[i].perimeters.size() == 1);
        CHECK(layers[i].perimeters.front().finalized);
    }
}

TEST_CASE("Painted seams stay at a corner of the painted side", "[SeamPlacer]")
{
    const std::string mode = GENERATE("back", "aligned");
    CAPTURE(mode);
    PipelineFixture fixture(false, 2.0f);
    fixture.config.set_deserialize_strict("seam_position", mode);
    fixture.paint(false);
    PrintObject &object = fixture.prepare();
    const std::vector<Vec2d> square = {{0, 0}, {20, 0}, {20, 20}, {0, 20}};
    set_loop_on_every_layer(object, [&](size_t) { return fixture.points_in_layer(object, square); });
    SeamPlacer placer;
    placer.init(fixture.print, [] {});

    const Points corners = fixture.points_in_layer(object, square);
    for (const Vec2d &seam : placed_seams(placer, object)) {
        double nearest = std::numeric_limits<double>::max();
        for (const Point &corner : corners)
            nearest = std::min(nearest, (unscale(corner) - seam).norm());
        CAPTURE(seam.x(), seam.y());
        // Painted sides are y = 0 and y = 20; candidates of the side walls are painted up to the 0.4 mm
        // line width from them, so the seam lies on a painted corner or within that width of it.
        CHECK(nearest <= 0.45);
    }
}

namespace {
// Paints the front face (lowest y) of the fixture's box only.
void paint_front_face(PipelineFixture &fixture, EnforcerBlockerType type = EnforcerBlockerType::ENFORCER)
{
    auto &volume = *fixture.model.objects.front()->volumes.front();
    const auto &mesh = volume.mesh();
    const double front = mesh.bounding_box().min.y();
    TriangleSelector selector(mesh);
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        bool on_front = true;
        for (int j = 0; j < 3; ++j)
            on_front = on_front && std::abs(double(mesh.its.vertices[mesh.its.indices[i][j]].y()) - front) < 1e-6;
        if (on_front)
            selector.set_facet(int(i), type);
    }
    volume.seam_facets.set(selector);
    fixture.print.apply(fixture.model, fixture.config);
}
} // namespace

TEST_CASE("Painted ranges end exactly where the paint contour crosses the layer", "[SeamPlacer][Regression]")
{
    // The front face spans x 0..20; the loop runs 0.2 mm in front of it from x = -5 to 25. Points within the
    // 0.4 mm line width of the paint used to be Enforced up to about 0.35 mm past the face, and only from the
    // vertex before the face, so the middle candidate was off the middle of the face.
    PipelineFixture fixture;
    paint_front_face(fixture);
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);
    append_loop(region, fixture.points_in_layer(object, {{-5, 0.2}, {-0.3, 0.2}, {25, 0.2}, {25, 10}, {-5, 10}}));
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 1);
    CHECK(data.perimeters.front().exact_enforced_ends);

    const auto face = fixture.points_in_layer(object, {{0, 0.2}, {20, 0.2}, {10, 0.2}});
    double first = std::numeric_limits<double>::max(), last = std::numeric_limits<double>::lowest();
    size_t centers = 0;
    for (const auto &candidate : data.points) {
        if (candidate.type != SeamType::Enforced)
            continue;
        first = std::min(first, double(candidate.position.x()));
        last = std::max(last, double(candidate.position.x()));
        if (candidate.central_enforcer) {
            ++centers;
            CHECK((candidate.position.head<2>().cast<double>() - unscale(face[2])).norm() < 1e-3);
        }
    }
    CHECK_THAT(first, Catch::Matchers::WithinAbs(unscale<double>(face[0].x()), 1e-3));
    CHECK_THAT(last, Catch::Matchers::WithinAbs(unscale<double>(face[1].x()), 1e-3));
    CHECK(centers == 1);
}

TEST_CASE("Exact paint ends do not depend on where the loop starts", "[SeamPlacer][Regression]")
{
    // The paint ends project onto the front corners of the loop. A loop repeats its first point, so at its
    // first vertex the edge behind it has zero length; origin 0 puts the end with paint behind it there.
    const int origin = GENERATE(0, 1, 2, 3);
    CAPTURE(origin);
    PipelineFixture fixture;
    paint_front_face(fixture);
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);
    auto outline = fixture.points_in_layer(object, {{19.8, 0.2}, {19.8, 19.8}, {0.2, 19.8}, {0.2, 0.2}});
    std::rotate(outline.begin(), outline.begin() + origin, outline.end());
    append_loop(region, std::move(outline));
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 1);
    CHECK(data.perimeters.front().exact_enforced_ends);

    const auto corners = fixture.points_in_layer(object, {{0.2, 0.2}, {19.8, 0.2}});
    double first = std::numeric_limits<double>::max(), last = std::numeric_limits<double>::lowest();
    for (const auto &candidate : data.points) {
        if (candidate.type != SeamType::Enforced)
            continue;
        CHECK_THAT(double(candidate.position.y()), Catch::Matchers::WithinAbs(unscale<double>(corners[0].y()), 1e-3));
        first = std::min(first, double(candidate.position.x()));
        last = std::max(last, double(candidate.position.x()));
    }
    CHECK_THAT(first, Catch::Matchers::WithinAbs(unscale<double>(corners[0].x()), 1e-3));
    CHECK_THAT(last, Catch::Matchers::WithinAbs(unscale<double>(corners[1].x()), 1e-3));
}

TEST_CASE("Random seams stay inside exact green ranges and keep varying", "[SeamPlacer][Regression]")
{
    const int origin = GENERATE(0, 1, 2);
    const bool partial_blocker = GENERATE(false, true);
    CAPTURE(origin, partial_blocker);
    // One hundred layers exercise the outgoing edge of the last green sample at many random offsets.
    PipelineFixture fixture(false, 20.0f);
    fixture.config.set_deserialize_strict("seam_position", "random");
    paint_front_face(fixture);
    if (partial_blocker) {
        const auto *part = fixture.model.objects.front()->volumes.front();
        const Vec3d center = part->get_matrix() * (part->mesh().bounding_box().min + Vec3d(10, 0, 10));
        auto *modifier = fixture.model.objects.front()->add_volume(make_cube(4, 4, 22));
        modifier->set_type(ModelVolumeType::PRECISE_SEAM_BLOCKED);
        modifier->set_offset(center); // Block x = 8..12 on the green face, throughout the object's height.
        fixture.print.apply(fixture.model, fixture.config);
    }
    PrintObject &object = fixture.prepare();
    REQUIRE(object.layers().size() == 100);
    set_loop_on_every_layer(object, [&](size_t) {
        auto outline = fixture.points_in_layer(object, {{-5, 0.2}, {10, 0.2}, {25, 0.2}, {25, 10}, {-5, 10}});
        // Origin 1 wraps the green range; origin 2 tests its closing edge after modifier normalization.
        std::rotate(outline.begin(), outline.begin() + origin, outline.end());
        return outline;
    });
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    for (const auto &layer : placer.m_seam_per_object.at(&object).layers) {
        REQUIRE(layer.perimeters.size() == 1);
        CHECK(layer.perimeters.front().exact_enforced_ends);
        if (origin == 2 && partial_blocker) {
            const auto &perimeter = layer.perimeters.front();
            // Modifiers remove the repeated loop endpoint, exposing the Enforced-to-Neutral closing edge.
            CHECK(layer.points[perimeter.end_index - 1].type == SeamType::Enforced);
            CHECK(layer.points[perimeter.start_index].type == SeamType::Neutral);
        }
        if (partial_blocker)
            CHECK(std::any_of(layer.points.begin(), layer.points.end(), [](const auto &candidate) {
                return candidate.type == SeamType::Blocked;
            }));
    }
    const auto bounds = fixture.points_in_layer(object, {{0, 0.2}, {20, 0.2}, {8, 0.2}, {12, 0.2}});
    const Vec2d first = unscale(bounds[0]), last = unscale(bounds[1]);
    const double blocked_begin = unscale<double>(bounds[2].x()), blocked_end = unscale<double>(bounds[3].x());
    double min_x = std::numeric_limits<double>::max(), max_x = std::numeric_limits<double>::lowest();
    // Check the actual split loop start: an Enforced candidate alone does not constrain Random's edge.
    for (const Vec2d &seam : placed_seams(placer, object)) {
        CAPTURE(seam.x(), seam.y());
        CHECK_THAT(seam.y(), Catch::Matchers::WithinAbs(first.y(), 1e-3));
        CHECK(seam.x() >= first.x() - 1e-3);
        CHECK(seam.x() <= last.x() + 1e-3);
        if (partial_blocker) {
            const bool inside_blocker = seam.x() > blocked_begin + 1e-3 && seam.x() < blocked_end - 1e-3;
            CHECK_FALSE(inside_blocker);
        }
        min_x = std::min(min_x, seam.x());
        max_x = std::max(max_x, seam.x());
    }
    CHECK(max_x - min_x > 1.0); // Reject a false fix that pins every layer to one green endpoint.
}

TEST_CASE("Projected paint boundaries do not color a distant contour detour", "[SeamPlacer][Regression]")
{
    const auto paint = GENERATE(EnforcerBlockerType::ENFORCER, EnforcerBlockerType::BLOCKER);
    PipelineFixture fixture;
    paint_front_face(fixture, paint);
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);
    // The projected ends span x=0..20, but the middle of this contour leaves the painted front face.
    append_loop(region, fixture.points_in_layer(object,
        {{-5, 0.2}, {5, 0.2}, {5, 10}, {15, 10}, {15, 0.2}, {25, 0.2}, {25, 15}, {-5, 15}}));
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 1);
    const auto &perimeter = data.perimeters.front();
    CHECK((paint == EnforcerBlockerType::ENFORCER ? perimeter.exact_enforced_ends : perimeter.exact_blocked_ends));
    const auto gray = fixture.points_in_layer(object, {{5, 10}, {15, 10}});
    for (const auto &point : gray) {
        const auto position = unscale(point);
        const auto candidate = std::find_if(data.points.begin(), data.points.end(), [&](const SeamPlacerImpl::SeamCandidate &p) {
            return (p.position.head<2>().cast<double>() - position).norm() < 1e-3;
        });
        REQUIRE(candidate != data.points.end());
        CHECK(candidate->type == SeamType::Neutral);
    }
    const auto expected = paint == EnforcerBlockerType::ENFORCER ? SeamType::Enforced : SeamType::Blocked;
    CHECK(std::any_of(data.points.begin(), data.points.end(), [&](const auto &p) { return p.type == expected; }));
}

TEST_CASE("Aligned painted seams follow the middle of the painted band", "[SeamPlacer][Regression]")
{
    // 10 layers with the loop of the previous test; the vertex before the face moves on every layer, which
    // used to move the middle candidate and with it the seam.
    PipelineFixture fixture(false, 2.0f);
    fixture.config.set_deserialize_strict("seam_position", "aligned");
    paint_front_face(fixture);
    PrintObject &object = fixture.prepare();
    REQUIRE(object.layers().size() == 10);
    set_loop_on_every_layer(object, [&](size_t layer) {
        const double before = -0.05 - 0.04 * double(layer);
        return fixture.points_in_layer(object, {{-5, 0.2}, {before, 0.2}, {25, 0.2}, {25, 10}, {-5, 10}});
    });
    SeamPlacer placer;
    placer.init(fixture.print, [] {});

    const Vec2d middle = unscale(fixture.points_in_layer(object, {{10, 0.2}}).front());
    for (const Vec2d &seam : placed_seams(placer, object)) {
        CAPTURE(seam.x(), seam.y());
        CHECK((seam - middle).norm() < 1e-3);
    }
}

TEST_CASE("Red paint blocks its exact range without blocking the nearby gray contour", "[SeamPlacer][Regression]")
{
    const bool reverse = GENERATE(false, true);
    const bool wrap = GENERATE(false, true);
    PipelineFixture fixture;
    paint_front_face(fixture, EnforcerBlockerType::BLOCKER);
    PrintObject &object = fixture.prepare();
    auto outline = fixture.points_in_layer(object, {{-5, 0.2}, {-0.1, 0.2}, {10, 0.2}, {20.1, 0.2},
                                                  {25, 0.2}, {25, 10}, {-5, 10}});
    if (wrap) std::rotate(outline.begin(), outline.begin() + 2, outline.end()); // Red range crosses the loop origin.
    if (reverse) std::reverse(outline.begin(), outline.end());
    append_loop(clear_first_layer(object), std::move(outline));
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 1);
    CHECK(data.perimeters.front().exact_blocked_ends);
    const auto front = fixture.points_in_layer(object, {{0, 0.2}, {20, 0.2}, {-0.1, 0.2}, {20.1, 0.2}});
    const double lower = unscale<double>(front[0].x()), upper = unscale<double>(front[1].x());
    std::vector<bool> gray_near_edge(2, false);
    double blocked_min = std::numeric_limits<double>::max(), blocked_max = -blocked_min;
    for (size_t i = 0; i < data.points.size(); ++i) {
        const auto &candidate = data.points[i];
        const double x = candidate.position.x();
        if (candidate.type == SeamType::Blocked) {
            blocked_min = std::min(blocked_min, x);
            blocked_max = std::max(blocked_max, x);
            CHECK(x >= lower - 1e-3);
            CHECK(x <= upper + 1e-3);
        }
        for (const size_t index : {size_t(2), size_t(3)})
            if ((candidate.position.head<2>().cast<double>() - unscale(front[index])).norm() < 1e-4) {
                gray_near_edge[index - 2] = true; // A closed extrusion can repeat its start vertex.
                CHECK(candidate.type == SeamType::Neutral); // Old radius test blocked both of these vertices.
                const auto target = Vec3f(float(0.5 * (lower + upper)), candidate.position.y(), candidate.position.z());
                const auto projected = SeamPlacerImpl::place_on_loop(data.points, i, target);
                // The normal projection may reach either end of the same permitted gray arc.
                const double edge_distance = projected.x() < lower ? lower - projected.x() : projected.x() - upper;
                CHECK(edge_distance > 0.0);
                CHECK(edge_distance < 3e-3); // The stop follows the actual red edge, not the old gray vertex.
            }
    }
    CHECK(gray_near_edge[0]);
    CHECK(gray_near_edge[1]);
    CHECK_THAT(blocked_min, Catch::Matchers::WithinAbs(lower, 1e-3));
    CHECK_THAT(blocked_max, Catch::Matchers::WithinAbs(upper, 1e-3));
}

TEST_CASE("Seams in every mode stay outside the exact red range", "[SeamPlacer][Regression]")
{
    const std::string mode = GENERATE(std::string("aligned"), std::string("aligned_back"), std::string("back"),
                                      std::string("nearest"), std::string("random"));
    PipelineFixture fixture(false, 2.0f);
    fixture.config.set_deserialize_strict("seam_position", mode);
    paint_front_face(fixture, EnforcerBlockerType::BLOCKER);
    PrintObject &object = fixture.prepare();
    set_loop_on_every_layer(object, [&](size_t) {
        return fixture.points_in_layer(object, {{-5, 0.2}, {-0.1, 0.2}, {10, 0.2}, {20.1, 0.2},
                                                {25, 0.2}, {25, 10}, {-5, 10}});
    });
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto front = fixture.points_in_layer(object, {{0, 0.2}, {20, 0.2}});
    const double lower = unscale<double>(front[0].x()), upper = unscale<double>(front[1].x());
    const double y = unscale<double>(front[0].y());
    for (const auto &layer : placer.m_seam_per_object.at(&object).layers) {
        const auto &perimeter = layer.perimeters.front();
        const size_t selected = perimeter.seam_index;
        CHECK(layer.points[selected].type != SeamType::Blocked);
    }
    for (const Vec2d &seam : placed_seams(placer, object)) {
        const bool inside_red = seam.x() >= lower && seam.x() <= upper && std::abs(seam.y() - y) < 1e-3;
        CHECK_FALSE(inside_red);
    }
}

TEST_CASE("Entire red surface retains the existing all-blocked seam fallback", "[SeamPlacer][Regression]")
{
    PipelineFixture fixture;
    auto &volume = *fixture.model.objects.front()->volumes.front();
    TriangleSelector selector(volume.mesh());
    for (size_t i = 0; i < volume.mesh().its.indices.size(); ++i)
        selector.set_facet(int(i), EnforcerBlockerType::BLOCKER); // No legal gray arc exists.
    volume.seam_facets.set(selector);
    fixture.print.apply(fixture.model, fixture.config);
    PrintObject &object = fixture.prepare();
    set_loop_on_every_layer(object, [&](size_t) {
        return fixture.points_in_layer(object, {{0.2, 0.2}, {19.8, 0.2}, {19.8, 19.8}, {0.2, 19.8}});
    });
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    CHECK_FALSE(data.perimeters.front().exact_blocked_ends);
    for (const auto &candidate : data.points) CHECK(candidate.type == SeamType::Blocked);
    CHECK(placed_seams(placer, object).front().allFinite());
}

TEST_CASE("Seams placed on a regular polygon with an even vertex count lie on the ray from its center", "[SeamPlacer]")
{
    const Vec2d center(3.0, -2.0);
    const double radius = 5.0;
    const size_t count = 24;
    const double step = 2.0 * PI / double(count);
    std::vector<Vec2d> xy;
    for (size_t i = 0; i < count; ++i)
        xy.emplace_back(center + radius * Vec2d(std::cos((0.3 + double(i)) * step), std::sin((0.3 + double(i)) * step)));
    SeamPlacerImpl::Perimeter perimeter;
    const auto points = loop_candidates(perimeter, xy);
    // Targets inside and outside the loop around vertex 5, placed from two neighboring candidates.
    for (const double offset : {-0.1, -0.03, 0.0, 0.05, 0.12})
        for (const double distance : {3.5, 5.0, 6.5}) {
            const double angle = (5.3 * step) + offset;
            const Vec2d target = center + distance * Vec2d(std::cos(angle), std::sin(angle));
            const Vec2d from_5 = placed_xy(points, 5, target);
            const Vec2d from_6 = placed_xy(points, 6, target);
            CAPTURE(offset, distance, from_5.x(), from_5.y());
            CHECK_THAT(std::atan2(from_5.y() - center.y(), from_5.x() - center.x()), Catch::Matchers::WithinAbs(angle, 1e-5));
            CHECK((from_5 - center).norm() <= radius + 1e-5);
            CHECK((from_5 - center).norm() >= radius * std::cos(step / 2.0) - 1e-5);
            CHECK((from_6 - from_5).norm() < 1e-5);
        }
}

TEST_CASE("Seams placed on the loop do not depend on the chosen candidate", "[SeamPlacer]")
{
    // Inserted candidates at (0, 0) and (0.2, 0) on an asymmetric painted contour; a chord around the
    // candidate would turn the target's offset across the bottom edge into a move along it.
    SeamPlacerImpl::Perimeter perimeter;
    const auto points = loop_candidates(perimeter, {{-1, 0}, {0, 0}, {0.2, 0}, {1, 0}, {1, 5}, {-6, 5}},
                                        std::vector<SeamType>(6, SeamType::Enforced));
    for (const Vec2d &target : {Vec2d(0, 3), Vec2d(0, 0.5), Vec2d(0.5, 1)}) {
        const Vec2d from_1 = placed_xy(points, 1, target);
        const Vec2d from_2 = placed_xy(points, 2, target);
        CAPTURE(target.x(), target.y(), from_1.x(), from_1.y(), from_2.x(), from_2.y());
        CHECK((from_2 - from_1).norm() < 1e-5);
    }
    // Straight above the candidate the seam stays on the bottom edge.
    CHECK((placed_xy(points, 2, {0, 0.5}) - Vec2d(0, 0)).norm() < 1e-5);
}

TEST_CASE("Seams placed on a short loop follow a moving target continuously", "[SeamPlacer]")
{
    // A 2 x 2 mm painted square with a candidate inserted at (0, 0); the target goes up along the right
    // side and beyond the loop, up to 5 mm from the candidate.
    SeamPlacerImpl::Perimeter perimeter;
    const auto points = loop_candidates(perimeter, {{-1, 0}, {0, 0}, {1, 0}, {1, 2}, {-1, 2}},
                                        std::vector<SeamType>(5, SeamType::Enforced));
    Vec2d previous = placed_xy(points, 1, {1, 0});
    for (int i = 1; i <= 500; ++i) {
        const Vec2d target(1.0, 0.01 * i);
        const Vec2d seam = placed_xy(points, 1, target);
        CAPTURE(target.y(), seam.x(), seam.y());
        if (target.y() <= 2.0)
            CHECK((seam - target).norm() < 1e-5); // A target on the loop is the seam.
        CHECK((seam - previous).norm() <= 0.0101);
        previous = seam;
    }
    // Above the loop the normals of the top side point to the square's center.
    CHECK((previous - Vec2d(0.25, 2.0)).norm() < 1e-5);
}

TEST_CASE("Seams placed on the loop stay in the allowed arc", "[SeamPlacer]")
{
    // A circle of 60 vertices; the target lies on vertex 16, beyond the arc allowed from candidate 12.
    const size_t count = 60;
    std::vector<Vec2d> xy;
    for (size_t i = 0; i < count; ++i)
        xy.emplace_back(10.0 * Vec2d(std::cos(2.0 * PI * double(i) / count), std::sin(2.0 * PI * double(i) / count)));
    const Vec2d target = xy[16];
    const Vec2d run_end = xy[13];
    const Vec2d past_end = run_end + 0.2 * (xy[14] - run_end).normalized();
    const auto placed = [&](const std::vector<SeamType> &types, bool exact_ends = false) {
        SeamPlacerImpl::Perimeter perimeter;
        perimeter.exact_enforced_ends = exact_ends;
        const auto points = loop_candidates(perimeter, xy, types);
        return placed_xy(points, 12, target);
    };
    std::vector<SeamType> types(count, SeamType::Neutral);
    // Enforced run 10..13: the seam may go 0.2 mm past its end, but not into a Blocked candidate.
    std::fill(types.begin() + 10, types.begin() + 14, SeamType::Enforced);
    CHECK((placed(types) - past_end).norm() < 1e-5);
    // A run ending exactly at the paint is not extended.
    CHECK((placed(types, true) - run_end).norm() < 1e-5);
    types[14] = SeamType::Blocked;
    CHECK((placed(types) - run_end).norm() < 1e-5);
    // A candidate that is not Enforced stays between the Blocked candidates around it.
    std::fill(types.begin() + 10, types.begin() + 14, SeamType::Neutral);
    types[40] = SeamType::Blocked;
    CHECK((placed(types) - run_end).norm() < 1e-5);
    // With the target reachable around the loop, it is the seam.
    types[40] = SeamType::Neutral;
    CHECK((placed(types) - target).norm() < 1e-5);
    types[14] = SeamType::Neutral;
    CHECK((placed(types) - target).norm() < 1e-5);
}

TEST_CASE("Seams placed on a short loop keep to the closest solution as the target moves", "[SeamPlacer][Regression]")
{
    // On a short painted square, the target moves upward left of the candidate: the solution on the left
    // side stays the closest one, and must not be traded for the farther one on the right side.
    SeamPlacerImpl::Perimeter perimeter;
    const auto points = loop_candidates(perimeter, {{-1, 0}, {0, 0}, {1, 0}, {1, 2}, {-1, 2}},
                                        std::vector<SeamType>(5, SeamType::Enforced));
    Vec2d previous = placed_xy(points, 1, {-0.1, 1.0});
    for (int i = 1; i <= 1000; ++i) {
        const Vec2d target(-0.1, 1.0 + 0.0001 * i);
        const Vec2d seam = placed_xy(points, 1, target);
        CAPTURE(target.y(), seam.x(), seam.y());
        CHECK(std::abs(seam.x() + 1.0) < 1e-5);
        CHECK((seam - previous).norm() <= 0.0011);
        previous = seam;
    }
}

TEST_CASE("Seams placed on the loop respect a painted run ending on the closing edge", "[SeamPlacer][Regression]")
{
    // The Enforced run goes around the loop from C and leaves 0.1 mm of its extension for the 10 mm closing
    // edge back to C: the target on that edge lies outside the allowed arc. The loop is also placed after
    // another loop and rotated, so that it does not start at index 0 and its run wraps around the array end.
    const std::vector<Vec2d> xy = {{0, 0}, {10, 0}, {10, 10}, {-10, 10}, {-10, 0.1}, {-10, 0}};
    std::vector<SeamType> types(6, SeamType::Enforced);
    types[5] = SeamType::Neutral;
    for (const size_t rotation : {size_t(0), size_t(3)}) {
        std::vector<Vec2d> rotated_xy = xy;
        std::vector<SeamType> rotated_types = types;
        std::rotate(rotated_xy.begin(), rotated_xy.begin() + rotation, rotated_xy.end());
        std::rotate(rotated_types.begin(), rotated_types.begin() + rotation, rotated_types.end());
        for (const size_t offset : {size_t(0), size_t(4)}) {
            CAPTURE(rotation, offset);
            SeamPlacerImpl::Perimeter perimeter;
            const auto points = loop_candidates(perimeter, rotated_xy, rotated_types, {}, offset);
            const size_t candidate = offset + (6 - rotation) % 6;
            // The end of the allowed arc closer to the target: 0.2 mm from C toward the Neutral candidate.
            CHECK((placed_xy(points, candidate, {-5, 0}) - Vec2d(-0.2, 0)).norm() < 1e-5);
        }
    }
}

TEST_CASE("Seams held at the allowed arc keep their end across the arc midpoint", "[SeamPlacer][Regression]")
{
    // A single Enforced candidate; the target moves along the far side of the loop, past the point halfway
    // around the loop from C. The end of the allowed arc closer to the target stays the same.
    std::vector<SeamType> types(5, SeamType::Neutral);
    types[0] = SeamType::Enforced;
    SeamPlacerImpl::Perimeter perimeter;
    const auto points = loop_candidates(perimeter, {{0, 0}, {10, 0}, {10, 1}, {-1, 1}, {-1, 0}}, types);
    for (int i = 0; i <= 200; ++i) {
        const Vec2d target(8.9 + 0.001 * i, 1.0);
        CAPTURE(target.x());
        CHECK((placed_xy(points, 0, target) - Vec2d(0.2, 0)).norm() < 1e-5);
    }
}

TEST_CASE("Seams placed on the loop stay at the arc end their solution leaves through", "[SeamPlacer][Regression]")
{
    // The Enforced run of C = (4.2, 0) goes around the loop and ends 0.2 mm past (0, 0); the gap up to
    // 0.2 mm before C is not allowed. The target follows the normal line through (x, 0) at a height of
    // 2.15 mm, so that (x, 0) is its closest solution, while the target stays closer to the other arc end.
    std::vector<SeamType> types(6, SeamType::Enforced);
    types[1] = SeamType::Neutral;
    SeamPlacerImpl::Perimeter perimeter;
    const auto points = loop_candidates(perimeter, {{0, 0}, {3, 0}, {4.2, 0}, {100, 0}, {100, 100}, {0, 100}}, types);
    const double a = 1.0 / std::sqrt(2.0);
    for (int i = 0; i <= 200; ++i) {
        // Normal at (x, 0), blended between (a, a) at (0, 0) and (0, 1) at (3, 0).
        const double x = 0.1 + 0.001 * i;
        const Vec2d normal(a * (1.0 - x / 3.0), a + (1.0 - a) * x / 3.0);
        const Vec2d target(x + 2.15 * normal.x() / normal.y(), 2.15);
        CAPTURE(x);
        // The float target moves the solution by about 1e-5 mm here; the sweep step is 1e-3 mm.
        CHECK((placed_xy(points, 2, target) - Vec2d(std::min(x, 0.2), 0)).norm() < 1e-4);
    }
}

TEST_CASE("Seams placed on the loop do not jump at duplicated points", "[SeamPlacer][Regression]")
{
    // Extrusion loops repeat the shared end points of their paths; both copies of the corner must get the
    // same normal.
    SeamPlacerImpl::Perimeter perimeter;
    const auto points = loop_candidates(perimeter, {{0, 0}, {9, 0}, {10, 0}, {10, 0}, {10, 10}, {0, 10}});
    Vec2d previous = placed_xy(points, 1, {10.1, 0.0});
    for (int i = 1; i <= 200; ++i) {
        const Vec2d target(10.1, -0.001 * i);
        const Vec2d seam = placed_xy(points, 1, target);
        CAPTURE(target.y(), seam.x(), seam.y());
        CHECK((seam - previous).norm() <= 0.0011);
        CHECK((seam - Vec2d(10, 0)).norm() <= 0.11);
        previous = seam;
    }
}

TEST_CASE("Corner influence is full near a corner and fades out along the loop", "[SeamPlacer]")
{
    // Candidates every 0.1 mm along the bottom of a 3 x 3 mm loop; angles are set directly.
    std::vector<Vec2d> xy;
    for (int i = 0; i <= 30; ++i)
        xy.emplace_back(0.1 * i, 0.0);
    xy.emplace_back(3.0, 3.0);
    xy.emplace_back(0.0, 3.0);
    const auto influence = [&](float corner_angle, size_t index) {
        std::vector<float> angles(xy.size(), 0.0f);
        angles[10] = corner_angle;
        SeamPlacerImpl::Perimeter perimeter;
        const auto points = loop_candidates(perimeter, xy, {}, angles);
        return double(SeamPlacerImpl::enforced_corner_influence(points, index, 0.4f, 0.8f));
    };
    const float deg = float(PI) / 180.0f;
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(influence(45.0f * deg, 10), WithinAbs(1.0, 1e-4));
    CHECK_THAT(influence(-45.0f * deg, 14), WithinAbs(1.0, 1e-4)); // Within R0 = 0.4 mm, either turn.
    CHECK_THAT(influence(45.0f * deg, 6), WithinAbs(1.0, 1e-4));
    CHECK_THAT(influence(45.0f * deg, 16), WithinAbs(0.5, 1e-4));  // Halfway through the fade band.
    CHECK_THAT(influence(45.0f * deg, 18), WithinAbs(0.0, 1e-4));  // R1 = 0.8 mm.
    CHECK_THAT(influence(45.0f * deg, 25), WithinAbs(0.0, 1e-4));
    CHECK_THAT(influence(22.5f * deg, 10), WithinAbs(0.5, 1e-4));  // Halfway between 15 and 30 degrees.
    CHECK_THAT(influence(10.0f * deg, 10), WithinAbs(0.0, 1e-4));
}

TEST_CASE("Seam painting acts only from model parts and negative volumes", "[SeamPlacer]")
{
    // Painting survives a type change, but only model parts expose it in the seam gizmo. A helper
    // painted while it was a part must not affect the seam once it is a modifier or support volume.
    // Negative volumes keep it on purpose: it is the only way to paint the wall of a hole they cut.
    const auto helper_type = GENERATE(ModelVolumeType::MODEL_PART, ModelVolumeType::NEGATIVE_VOLUME,
                                      ModelVolumeType::PARAMETER_MODIFIER, ModelVolumeType::SUPPORT_BLOCKER,
                                      ModelVolumeType::SUPPORT_ENFORCER);
    // Precise Seam helpers are left out: on the loop they would retype the candidates themselves.
    const std::string type_name = ModelVolume::type_to_string(helper_type);
    CAPTURE(type_name);
    PipelineFixture fixture;
    // A 2 mm strip along the front side: its front face lies on the loop's front edge, while its back
    // face stays farther than the paint radius. As a negative volume it cuts only the strip.
    ModelVolume *helper = fixture.model.objects.front()->add_volume(TriangleMesh(its_make_cube(20, 2, 0.4)));
    const ObjectID helper_id = helper->id();
    {
        // Paint every face of the helper while it is still a part, then change its type.
        TriangleSelector selector(helper->mesh());
        for (size_t i = 0; i < helper->mesh().its.indices.size(); ++i)
            selector.set_facet(int(i), EnforcerBlockerType::ENFORCER);
        helper->seam_facets.set(selector);
    }
    REQUIRE(helper->is_seam_painted());
    const auto count_enforced = [&]() {
        fixture.print.apply(fixture.model, fixture.config);
        PrintObject &object = fixture.prepare();
        auto &region = clear_first_layer(object);
        append_loop(region, fixture.points_in_layer(object, {{0, 0}, {20, 0}, {20, 20}, {0, 20}}));
        SeamPlacer placer;
        placer.init(fixture.print, [] {});
        const auto &data = placer.m_seam_per_object.at(&object).layers.front();
        REQUIRE(data.perimeters.size() == 1);
        // The helper volume in the print's model copy keeps its painting whatever its type.
        const auto &volumes = object.model_object()->volumes;
        const auto it = std::find_if(volumes.begin(), volumes.end(), [&](const ModelVolume *v) { return v->id() == helper_id; });
        REQUIRE(it != volumes.end());
        CHECK((*it)->is_seam_painted());
        return size_t(std::count_if(data.points.begin(), data.points.end(), [](const auto &candidate) {
            return candidate.type == SeamPlacerImpl::EnforcedBlockedSeamPoint::Enforced;
        }));
    };
    helper->set_type(helper_type);
    const bool acts = helper_type == ModelVolumeType::MODEL_PART || helper_type == ModelVolumeType::NEGATIVE_VOLUME;
    if (acts)
        CHECK(count_enforced() > 0);
    else
        CHECK(count_enforced() == 0);
    if (!acts) {
        // The painting was ignored, not lost: as a part again the helper enforces candidates.
        helper->set_type(ModelVolumeType::MODEL_PART);
        CHECK(count_enforced() > 0);
    }
}

TEST_CASE("Precise Seam removes path junction duplicates but preserves separate visits", "[SeamPlacer][PreciseSeam]")
{
    const bool enable_ps = GENERATE(false, true);
    const bool self_touch = GENERATE(false, true);
    PipelineFixture fixture;
    if (enable_ps) {
        auto *helper = fixture.model.objects.front()->add_volume(make_cube(1, 1, 1));
        helper->set_type(ModelVolumeType::PRECISE_SEAM_NEUTRAL);
        helper->set_offset(Vec3d(100, 100, 0)); // Enable normalization without intersecting the synthetic loop.
        fixture.print.apply(fixture.model, fixture.config);
    }
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);
    const std::vector<Vec2d> vertices = self_touch ? std::vector<Vec2d>{{2, 2}, {10, 2}, {18, 10}, {10, 2}, {2, 18}} :
                                                   std::vector<Vec2d>{{2, 2}, {18, 2}, {18, 18}, {2, 18}};
    const Points outline = fixture.points_in_layer(object, vertices);
    append_loop(region, outline, true);
    SeamPlacer placer;
    // A direct call outside G-code export: init() must not need an active print step.
    placer.init(fixture.print, [] {});
    // The helper never reaches the loop, so it is reported, named with its object; no helper, no warning.
    if (enable_ps) {
        CHECK(placer.precise_seam_warning().find("had no effect on the seam") != std::string::npos);
        CHECK(placer.precise_seam_warning().find("\"object.stl\"") != std::string::npos);
    } else
        CHECK(placer.precise_seam_warning().empty());
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 1);
    // Each separate path contributes both endpoints in ordinary mode; PS removes only adjacent copies.
    REQUIRE(data.points.size() == (enable_ps ? outline.size() : 2 * outline.size()));
    for (size_t i = 0; i < outline.size(); ++i) {
        const Vec2f target = unscale(outline[i]).cast<float>();
        const size_t input_count = std::count(outline.begin(), outline.end(), outline[i]);
        // Both paths convert the same integer coordinates to float; exact identity detects duplicate copies.
        const size_t actual_count = std::count_if(data.points.begin(), data.points.end(), [&](const auto &candidate) {
            return candidate.position.template head<2>() == target;
        });
        CHECK(actual_count == (enable_ps ? input_count : 2 * input_count));
    }
    if (enable_ps) {
        for (size_t i = 0; i < data.points.size(); ++i) {
            CHECK(std::isfinite(data.points[i].local_ccw_angle));
            CHECK(data.points[i].position != data.points[(i + 1) % data.points.size()].position);
        }
    }
}

TEST_CASE("Print apply synchronizes support and seam helpers through type changes and restored models", "[SeamPlacer][PreciseSeam][Print]")
{
    const int changed = GENERATE(0, 1, 2); // Support only, seam only, or both including cross-family switches.
    PipelineFixture fixture;
    auto *model_object = fixture.model.objects.front();
    auto *support = model_object->add_volume(make_cube(1, 1, 1));
    support->set_type(ModelVolumeType::SUPPORT_BLOCKER);
    auto *seam = model_object->add_volume(make_cube(1, 1, 1));
    seam->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
    fixture.print.apply(fixture.model, fixture.config);
    REQUIRE(fixture.print.objects().size() == 1);
    const PrintObject *original_print_object = fixture.print.objects().front();
    const ModelVolume *original_part = original_print_object->model_object()->volumes.front();
    const Model before(fixture.model); // A restored model snapshot preserves IDs, as the apply path requires.
    if (changed == 0 || changed == 2) {
        support->set_type(changed == 2 ? ModelVolumeType::PRECISE_SEAM_LEFT : ModelVolumeType::SUPPORT_ENFORCER);
        support->set_offset(Vec3d(3, 4, 0));
    }
    if (changed == 1 || changed == 2) {
        seam->set_type(changed == 2 ? ModelVolumeType::SUPPORT_BLOCKER : ModelVolumeType::PRECISE_SEAM_RIGHT);
        seam->set_offset(Vec3d(-3, 2, 0));
    }
    if (changed == 2) std::swap(model_object->volumes[1], model_object->volumes[2]);
    const auto check_applied = [&](const Model &expected) {
        REQUIRE(fixture.print.objects().size() == 1);
        // Helper-only changes should preserve the print object and its unaffected printable volume.
        CHECK(fixture.print.objects().front() == original_print_object);
        const auto &actual = fixture.print.objects().front()->model_object()->volumes;
        const auto &wanted = expected.objects.front()->volumes;
        REQUIRE(actual.size() == wanted.size());
        CHECK(actual.front() == original_part);
        for (size_t i = 0; i < wanted.size(); ++i) {
            CAPTURE(changed, i);
            CHECK(actual[i]->id() == wanted[i]->id());
            CHECK(actual[i]->type() == wanted[i]->type());
            CHECK(actual[i]->get_matrix().isApprox(wanted[i]->get_matrix(), 1e-9));
        }
    };
    fixture.print.apply(fixture.model, fixture.config);
    check_applied(fixture.model);
    fixture.print.apply(before, fixture.config);
    check_applied(before);
    fixture.print.apply(fixture.model, fixture.config);
    check_applied(fixture.model);
}

TEST_CASE("Precise Seam volume changes invalidate only G-code export", "[SeamPlacer][PreciseSeam][Print]")
{
    const int change = GENERATE(0, 1, 2, 3); // Add, move, retype, remove.
    CAPTURE(change);
    PipelineFixture fixture;
    ModelObject *model_object = fixture.model.objects.front();
    // A second helper stays in the object throughout: deleting down to a single volume makes
    // ModelObject::delete_volume() fold the volume transform into the instances and renew the volume
    // ID, which legitimately reslices the object regardless of Precise Seam.
    ModelVolume *keeper = model_object->add_volume(make_cube(1, 1, 1));
    keeper->set_type(ModelVolumeType::PRECISE_SEAM_NEUTRAL);
    if (change != 0) {
        ModelVolume *seam = model_object->add_volume(make_cube(1, 1, 1));
        seam->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
    }
    fixture.print.apply(fixture.model, fixture.config);
    // A full export marks every step done, so an invalidated step is visible afterwards.
    Test::gcode(fixture.print);
    REQUIRE(fixture.print.objects().size() == 1);
    const PrintObject *object = fixture.print.objects().front();
    REQUIRE(fixture.print.is_step_done(psGCodeExport));
    REQUIRE(object->is_step_done(posSlice));
    REQUIRE(object->is_step_done(posPerimeters));

    if (change == 0) {
        ModelVolume *seam = model_object->add_volume(make_cube(1, 1, 1));
        seam->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
    } else {
        ModelVolume *seam = model_object->volumes.back();
        REQUIRE(seam->is_precise_seam());
        if (change == 1) seam->set_offset(Vec3d(2, 3, 0));
        if (change == 2) seam->set_type(ModelVolumeType::PRECISE_SEAM_ENFORCED);
        if (change == 3) model_object->delete_volume(model_object->volumes.size() - 1);
    }
    fixture.print.apply(fixture.model, fixture.config);

    // The helper takes no part in slicing: the object and its layers are kept, only export reruns.
    // REQUIRE, not CHECK: a recreated PrintObject means the old one was freed and must not be read.
    REQUIRE(fixture.print.objects().size() == 1);
    REQUIRE(fixture.print.objects().front() == object);
    CHECK_FALSE(fixture.print.is_step_done(psGCodeExport));
    CHECK(object->is_step_done(posSlice));
    CHECK(object->is_step_done(posPerimeters));
}

namespace {
// Axis-aligned rectangle given by two opposite corners in layer coordinates, as placed by the fixture.
BoundingBoxf layer_rectangle(const PipelineFixture &fixture, const PrintObject &object, const Vec2d &a, const Vec2d &b)
{
    std::vector<Vec2d> corners;
    for (const Point &corner : fixture.points_in_layer(object, {a, b}))
        corners.push_back(unscale(corner));
    return BoundingBoxf(corners);
}

// True if the point lies within `tolerance` mm of the boundary of the rectangle.
bool near_rectangle_boundary(const Vec3f &p, const BoundingBoxf &rect, double tolerance)
{
    const double dx = std::min(std::abs(p.x() - rect.min.x()), std::abs(p.x() - rect.max.x()));
    const double dy = std::min(std::abs(p.y() - rect.min.y()), std::abs(p.y() - rect.max.y()));
    const bool inside_x = p.x() > rect.min.x() - tolerance && p.x() < rect.max.x() + tolerance;
    const bool inside_y = p.y() > rect.min.y() - tolerance && p.y() < rect.max.y() + tolerance;
    return (dx < tolerance && inside_y) || (dy < tolerance && inside_x);
}

// Checks that every seam candidate of the layer lies on the boundary of exactly one rectangle and that
// a perimeter never mixes candidates of two rectangles (a merged fallback polygon would). Rectangles
// must be at least 1 mm apart. Returns how many perimeters draw their candidates from each rectangle.
template<class LayerSeamsT>
std::vector<size_t> perimeters_per_rectangle(const LayerSeamsT &data, const std::vector<BoundingBoxf> &rects)
{
    std::vector<size_t> result(rects.size(), 0);
    for (const auto &perimeter : data.perimeters) {
        std::vector<size_t> hits(rects.size(), 0);
        for (size_t i = perimeter.start_index; i < perimeter.end_index; ++i) {
            const Vec3f &p = data.points[i].position;
            size_t matched = 0;
            for (size_t r = 0; r < rects.size(); ++r)
                if (near_rectangle_boundary(p, rects[r], 0.5)) {
                    ++hits[r];
                    ++matched;
                }
            CAPTURE(p.x(), p.y());
            CHECK(matched == 1); // A candidate off every outline comes from an inner loop.
        }
        size_t used = 0;
        for (size_t r = 0; r < rects.size(); ++r)
            if (hits[r] > 0) {
                ++used;
                ++result[r];
            }
        CHECK(used <= 1);
    }
    return result;
}
} // namespace

TEST_CASE("Fully overhanging outer loop is its own seam perimeter regardless of island order", "[SeamPlacer][Regression]")
{
    const bool overhang_first = GENERATE(false, true);
    CAPTURE(overhang_first);
    PipelineFixture fixture;
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);

    // Island A: ordinary outer wall. Island B: the outer wall is entirely overhang but keeps inset_idx 0,
    // followed by an inner wall that is overhang as well.
    ExtrusionEntityCollection island_a, island_b;
    island_a.append(make_loop(fixture.points_in_layer(object, {{2, 2}, {8, 2}, {8, 8}, {2, 8}}), erExternalPerimeter, 0));
    island_b.append(make_loop(fixture.points_in_layer(object, {{12, 2}, {18, 2}, {18, 8}, {12, 8}}), erOverhangPerimeter, 0));
    island_b.append(make_loop(fixture.points_in_layer(object, {{13.5, 3.5}, {16.5, 3.5}, {16.5, 6.5}, {13.5, 6.5}}), erOverhangPerimeter, 1));
    if (overhang_first) {
        region.perimeters.append(std::move(island_b));
        region.perimeters.append(std::move(island_a));
    } else {
        region.perimeters.append(std::move(island_a));
        region.perimeters.append(std::move(island_b));
    }

    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    // Exactly the two outer loops are registered; the inner loop is not merged into either of them.
    REQUIRE(data.perimeters.size() == 2);
    const auto counts = perimeters_per_rectangle(data, {layer_rectangle(fixture, object, {2, 2}, {8, 8}),
                                                        layer_rectangle(fixture, object, {12, 2}, {18, 8})});
    CHECK(counts == std::vector<size_t>{1, 1});
}

TEST_CASE("External role selects the outer loop when inset_idx is unknown", "[SeamPlacer][Regression]")
{
    PipelineFixture fixture;
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);

    // Thin-wall style loops carry the external role without a stored depth.
    ExtrusionEntityCollection island;
    island.append(make_loop(fixture.points_in_layer(object, {{2, 2}, {8, 2}, {8, 8}, {2, 8}}), erExternalPerimeter, -1));
    island.append(make_loop(fixture.points_in_layer(object, {{3.5, 3.5}, {6.5, 3.5}, {6.5, 6.5}, {3.5, 6.5}}), erPerimeter, -1));
    region.perimeters.append(std::move(island));

    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 1);
    const auto counts = perimeters_per_rectangle(data, {layer_rectangle(fixture, object, {2, 2}, {8, 8})});
    CHECK(counts == std::vector<size_t>{1});
}

TEST_CASE("A collection without a recognizable outer loop gets its own fallback polygon", "[SeamPlacer][Regression]")
{
    PipelineFixture fixture;
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);

    // The second collection has neither an external path nor a stored depth (for example, a loop loaded
    // from the slicing cache). It must not be skipped just because an earlier collection added polygons.
    ExtrusionEntityCollection island_a, island_b;
    island_a.append(make_loop(fixture.points_in_layer(object, {{2, 2}, {8, 2}, {8, 8}, {2, 8}}), erExternalPerimeter, 0));
    island_b.append(make_loop(fixture.points_in_layer(object, {{12, 2}, {18, 2}, {18, 8}, {12, 8}}), erOverhangPerimeter, -1));
    region.perimeters.append(std::move(island_a));
    region.perimeters.append(std::move(island_b));

    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 2);
    const auto counts = perimeters_per_rectangle(data, {layer_rectangle(fixture, object, {2, 2}, {8, 8}),
                                                        layer_rectangle(fixture, object, {12, 2}, {18, 8})});
    CHECK(counts == std::vector<size_t>{1, 1});
}

TEST_CASE("Mixed overhang and external outer loop is taken once", "[SeamPlacer][Regression]")
{
    // Whether or not the generator stored the depth, the external path alone identifies the loop.
    const int outer_inset_idx = GENERATE(0, -1);
    CAPTURE(outer_inset_idx);
    PipelineFixture fixture;
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);

    // The outer loop consists of one overhang path and external paths.
    ExtrusionLoop mixed = make_loop(fixture.points_in_layer(object, {{2, 2}, {8, 2}, {8, 8}, {2, 8}}), erExternalPerimeter,
                                    outer_inset_idx, true);
    mixed.paths.front().set_extrusion_role(erOverhangPerimeter);
    ExtrusionEntityCollection island;
    island.append(std::move(mixed));
    island.append(make_loop(fixture.points_in_layer(object, {{3.5, 3.5}, {6.5, 3.5}, {6.5, 6.5}, {3.5, 6.5}}), erPerimeter,
                            outer_inset_idx == 0 ? 1 : -1));
    region.perimeters.append(std::move(island));

    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 1);
    const auto counts = perimeters_per_rectangle(data, {layer_rectangle(fixture, object, {2, 2}, {8, 8})});
    CHECK(counts == std::vector<size_t>{1});
}

TEST_CASE("Contour boundary type is independent of extrusion direction", "[SeamPlacer]")
{
    const bool hole = GENERATE(false, true);
    const bool clockwise = GENERATE(false, true);
    PipelineFixture fixture;
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);
    append_loop(region, fixture.points_in_layer(object, {{2, 2}, {18, 2}, {18, 18}, {2, 18}}), false);
    auto *loop = static_cast<ExtrusionLoop*>(region.perimeters.entities.front());
    loop->set_loop_role(hole ? elrHole : elrDefault);
    if (clockwise) loop->make_clockwise(); else loop->make_counter_clockwise();
    // Geometric hole/outer identity must survive either choice of printing direction.
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 1);
    CHECK(data.perimeters.front().is_hole == hole);
}

TEST_CASE("Seam strings retain the legacy search when geometry finds no continuation", "[SeamPlacer][Regression]")
{
    PipelineFixture fixture(false, 2.0f);
    PrintObject &object = fixture.prepare();
    REQUIRE(object.layers().size() == 10);
    set_loop_on_every_layer(object, [&](size_t) {
        return fixture.points_in_layer(object, {{2, 2}, {18, 2}, {18, 18}, {2, 18}});
    });
    for (size_t i = 0; i < object.layers().size(); ++i) {
        auto *loop = static_cast<ExtrusionLoop*>(object.layers()[i]->get_region(0)->perimeters.entities.front());
        // Alternating roles deliberately fail geometric admissibility while preserving legacy candidates.
        loop->set_loop_role(i % 2 == 0 ? elrDefault : elrHole);
    }
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &layers = placer.m_seam_per_object.at(&object).layers;
    for (size_t i = 1; i < layers.size(); ++i) {
        CAPTURE(i);
        REQUIRE(layers[i].perimeters.size() == 1);
        CHECK(layers[i].perimeters.front().finalized);
    }
    SeamContour::Layers geometry(2);
    for (size_t i = 0; i < 2; ++i) {
        const auto &perimeter = layers[i].perimeters.front();
        SeamContour::Contour contour;
        contour.hole = perimeter.is_hole;
        contour.width = perimeter.flow_width;
        for (size_t j = perimeter.start_index; j < perimeter.end_index; ++j)
            contour.points.push_back(layers[i].points[j].position.head<2>().cast<double>());
        geometry[i].push_back(std::move(contour));
    }
    // Finalization alone could also pass through Unique; explicitly certify this fixture's empty result.
    const auto &start = layers.front().perimeters.front();
    CHECK(SeamContour::Resolver(std::move(geometry)).resolve(0, 0, start.seam_index-start.start_index, 1).outcome
          == SeamContour::Outcome::None);

}

TEST_CASE("Seam strings search all nondominated contour alternatives", "[SeamPlacer][Regression]")
{
    PipelineFixture fixture(false, 2.0f);
    PrintObject &object = fixture.prepare();
    REQUIRE(object.layers().size() == 10);
    for (size_t i = 0; i < object.layers().size(); ++i) {
        Layer &layer = *object.layers()[i];
        for (LayerRegion *region : layer.regions()) region->perimeters.clear();
        auto &region = *layer.get_region(0);
        if (i % 2 == 0) {
            append_loop(region, fixture.points_in_layer(object, {{2, 2}, {18, 2}, {18, 18}, {2, 18}}));
        } else {
            // Both continuations share the rear edge, but their bounds defeat unique fast matching.
            append_loop(region, fixture.points_in_layer(object, {{2, 2}, {20, 2}, {18, 18}, {2, 18}}));
            append_loop(region, fixture.points_in_layer(object, {{2, 2}, {21, 2}, {18, 18}, {2, 18}}));
        }
    }
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &layers = placer.m_seam_per_object.at(&object).layers;
    for (size_t i = 1; i < layers.size(); ++i) {
        CAPTURE(i);
        // Rejecting a nonunique geometric set would leave every short string unfinalized.
        CHECK(std::any_of(layers[i].perimeters.begin(), layers[i].perimeters.end(),
                          [](const auto &perimeter) { return perimeter.finalized; }));
    }
}

TEST_CASE("Seam alignment remains cancellable after a string is finalized", "[SeamPlacer][Regression]")
{
    PipelineFixture fixture(false, 2.0f);
    PrintObject &object = fixture.prepare();
    set_loop_on_every_layer(object, [&](size_t) {
        return fixture.points_in_layer(object, {{2, 2}, {18, 2}, {18, 18}, {2, 18}});
    });
    SeamPlacer placer;
    struct AlignmentCanceled {};
    CHECK_THROWS_AS(placer.init(fixture.print, [&] {
        const auto found = placer.m_seam_per_object.find(&object);
        if (found == placer.m_seam_per_object.end()) return;
        // Cancel only after real alignment progress, rather than during earlier pipeline callbacks.
        for (const auto &layer : found->second.layers)
            if (std::any_of(layer.perimeters.begin(), layer.perimeters.end(),
                            [](const auto &perimeter) { return perimeter.finalized; }))
                throw AlignmentCanceled{};
    }), AlignmentCanceled);
}

TEST_CASE("Seam extraction preserves unknown boundary roles for open extrusion paths", "[SeamPlacer][Regression]")
{
    const bool open = GENERATE(false, true);
    PipelineFixture fixture;
    auto &object = fixture.prepare();
    auto &region = clear_first_layer(object);
    const auto points = fixture.points_in_layer(object, {{2,2},{18,2},{18,18}});
    if (open) {
        ExtrusionPath path(erExternalPerimeter, 0.08, 0.4f, 0.2f);
        path.polyline = Polyline3(Polyline(points));
        region.perimeters.append(path); // A path has no closed-loop role even if its points look polygonal.
    } else append_loop(region, points);
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &perimeters = placer.m_seam_per_object.at(&object).layers.front().perimeters;
    REQUIRE(perimeters.size() == 1);
    CHECK(perimeters.front().contour_reliable == !open);
}

#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"
#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <cmath>
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

using namespace Slic3r;

namespace {
struct PipelineFixture {
    Model model;
    Print print;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    explicit PipelineFixture(bool trapezoid = false)
    {
        auto mesh = its_make_cube(20, 20, 0.4);
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

void append_loop(LayerRegion &region, Points points, bool separate_paths = false)
{
    // Inject deterministic external loops while keeping the real layer, region and paint-query machinery.
    REQUIRE(points.size() >= 3);
    points.push_back(points.front());
    ExtrusionPaths paths;
    if (separate_paths) {
        for (size_t i = 1; i < points.size(); ++i) {
            ExtrusionPath path(erExternalPerimeter, 0.08, 0.4f, 0.2f);
            path.polyline = Polyline3(Polyline(Points{points[i - 1], points[i]}));
            paths.push_back(std::move(path));
        }
    } else {
        ExtrusionPath path(erExternalPerimeter, 0.08, 0.4f, 0.2f);
        path.polyline = Polyline3(Polyline(std::move(points)));
        paths.push_back(std::move(path));
    }
    region.perimeters.append(ExtrusionLoop(std::move(paths)));
}

LayerRegion &clear_first_layer(PrintObject &object)
{
    Layer &layer = *object.layers().front();
    for (LayerRegion *region : layer.regions()) region->perimeters.clear();
    return *layer.get_region(0);
}
} // namespace

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

TEST_CASE("Entirely painted contours keep valid enforced seam candidates", "[SeamPlacer]")
{
    PipelineFixture fixture;
    fixture.paint(true);
    PrintObject &object = fixture.prepare();
    auto &region = clear_first_layer(object);
    append_loop(region, fixture.points_in_layer(object, {{0, 0}, {20, 0}, {20, 20}, {0, 20}}));
    SeamPlacer placer;
    placer.init(fixture.print, [] {});
    const auto &data = placer.m_seam_per_object.at(&object).layers.front();
    REQUIRE(data.perimeters.size() == 1);
    const auto &perimeter = data.perimeters.front();
    CHECK(perimeter.seam_index >= perimeter.start_index);
    CHECK(perimeter.seam_index < perimeter.end_index);
    for (const auto &candidate : data.points) {
        CHECK(candidate.type == SeamPlacerImpl::EnforcedBlockedSeamPoint::Enforced);
        CHECK_FALSE(candidate.central_enforcer); // There is no bounded patch to mark as central.
    }
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

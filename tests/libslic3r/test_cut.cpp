#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

namespace {

// A 25 mm cube split into two 12.5 mm wide halves that butt together on the plane x = 0,
// so a groove cut at the origin crosses both of them.
struct TwoPartBox
{
    ModelObject* object = nullptr;
    ModelVolume* part_a = nullptr;
    ModelVolume* part_b = nullptr;
};

ModelVolume* add_half(ModelObject* object, float min_x)
{
    TriangleMesh cube = make_cube(12.5, 25., 25.);
    cube.translate(min_x + 6.25f, -12.5f, -12.5f);
    return object->add_volume(std::move(cube));
}

TwoPartBox add_two_part_box(Model& model)
{
    TwoPartBox box;
    box.object = model.add_object();
    box.part_a = add_half(box.object, -12.5f);
    box.part_b = add_half(box.object, 0.f);
    box.object->add_instance();
    return box;
}

ModelObject* add_single_part_box(Model& model)
{
    ModelObject* object = model.add_object();
    TriangleMesh cube = make_cube(25., 25., 25.);
    cube.translate(-12.5f, -12.5f, -12.5f);
    object->add_volume(std::move(cube));
    object->add_instance();
    return object;
}

std::vector<const ModelVolume*> model_parts(const ModelObject& object)
{
    std::vector<const ModelVolume*> parts;
    for (const ModelVolume* volume : object.volumes)
        if (volume->is_model_part() && !volume->is_cut_connector())
            parts.push_back(volume);
    return parts;
}

// Runs the dovetail cut the cut gizmo runs, with the groove at the origin.
const ModelObjectPtrs& run_dovetail(Cut& cut)
{
    Cut::Groove groove;
    groove.depth           = 3.f;
    groove.width           = 2.f;
    groove.flaps_angle     = 0.5235988f; // 30 degrees
    groove.angle           = 0.0872665f; // 5 degrees
    groove.depth_tolerance = 0.1f;
    groove.width_tolerance = 0.1f;
    return cut.perform_with_groove(groove, Transform3d::Identity(), 1, 0.f, 0.f);
}

// Painting is remapped by surface overlap, so facets touching the joint would paint the neighbour as
// well: paint the outer face of the part, away from x = 0.
void paint_away_from_joint(ModelVolume* volume)
{
    const indexed_triangle_set& its   = volume->mesh().its;
    float                       min_x = std::numeric_limits<float>::max();
    for (const Vec3f& vertex : its.vertices)
        min_x = std::min(min_x, vertex.x());

    const auto on_outer_face = [min_x](const Vec3f& vertex) { return vertex.x() < min_x + 0.01f; };

    TriangleSelector selector(volume->mesh());
    int              painted = 0;
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const auto& facet = its.indices[i];
        if (on_outer_face(its.vertices[facet(0)]) && on_outer_face(its.vertices[facet(1)]) && on_outer_face(its.vertices[facet(2)])) {
            selector.set_facet(int(i), EnforcerBlockerType::Extruder2);
            ++painted;
        }
    }
    REQUIRE(painted > 0);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
}

} // namespace

TEST_CASE("A dovetail cut keeps each part's filament", "[Cut]")
{
    Model            model;
    const TwoPartBox box = add_two_part_box(model);
    box.part_b->config.set("extruder", 2);

    Cut cut(box.object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& objects = run_dovetail(cut);

    REQUIRE(objects.size() == 2);
    for (const ModelObject* object : objects) {
        std::vector<int>         filaments;
        std::vector<std::string> names;
        for (const ModelVolume* part : model_parts(*object)) {
            filaments.push_back(part->extruder_id());
            names.push_back(part->name);
        }
        std::sort(filaments.begin(), filaments.end());
        std::sort(names.begin(), names.end());
        REQUIRE(filaments == std::vector<int>{ 1, 2 });
        REQUIRE(names == std::vector<std::string>{ object->name, object->name + "_2" });
    }
}

TEST_CASE("A dovetail cut of a single-part object still leaves one part", "[Cut]")
{
    Model        model;
    ModelObject* object = add_single_part_box(model);

    Cut cut(object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& objects = run_dovetail(cut);

    REQUIRE(objects.size() == 2);
    for (const ModelObject* result : objects) {
        const std::vector<const ModelVolume*> parts = model_parts(*result);
        REQUIRE(parts.size() == 1);
        REQUIRE(parts.front()->name == result->name);
    }
}

TEST_CASE("A part that restates the object's filament merges with a part that leaves it implicit", "[Cut]")
{
    Model            model;
    const TwoPartBox box = add_two_part_box(model);
    box.object->config.set("extruder", 1);
    box.part_b->config.set("extruder", 1);

    Cut cut(box.object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& objects = run_dovetail(cut);

    REQUIRE(objects.size() == 2);
    for (const ModelObject* object : objects)
        REQUIRE(model_parts(*object).size() == 1);
}

TEST_CASE("A part that restates the object's infill merges with a part that leaves it implicit", "[Cut]")
{
    Model            model;
    const TwoPartBox box = add_two_part_box(model);
    box.object->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(50.));
    box.part_b->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(50.));

    Cut cut(box.object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& objects = run_dovetail(cut);

    REQUIRE(objects.size() == 2);
    for (const ModelObject* object : objects)
        REQUIRE(model_parts(*object).size() == 1);
}

TEST_CASE("A dovetail cut keeps the filament of a single-part object", "[Cut]")
{
    Model        model;
    ModelObject* object = add_single_part_box(model);
    object->volumes.front()->config.set("extruder", 2);

    Cut cut(object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& objects = run_dovetail(cut);

    REQUIRE(objects.size() == 2);
    for (const ModelObject* result : objects) {
        const std::vector<const ModelVolume*> parts = model_parts(*result);
        REQUIRE(parts.size() == 1);
        REQUIRE(parts.front()->extruder_id() == 2);
    }
}

TEST_CASE("A dovetail cut keeps modifier volumes", "[Cut]")
{
    Model            model;
    const TwoPartBox box = add_two_part_box(model);
    box.part_b->config.set("extruder", 2);

    TriangleMesh cube = make_cube(5., 5., 5.);
    cube.translate(-2.5f, -2.5f, -2.5f);
    ModelVolume* modifier = box.object->add_volume(std::move(cube));
    modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);

    Cut cut(box.object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& objects = run_dovetail(cut);

    REQUIRE(objects.size() == 2);
    for (const ModelObject* object : objects) {
        std::vector<int> filaments;
        size_t           others = 0;
        for (const ModelVolume* volume : object->volumes) {
            if (volume->is_model_part() && !volume->is_cut_connector())
                filaments.push_back(volume->extruder_id());
            else
                ++others;
        }
        std::sort(filaments.begin(), filaments.end());
        REQUIRE(filaments == std::vector<int>{ 1, 2 });
        REQUIRE(others == 1);
    }
}

TEST_CASE("A dovetail cut keeps per-part process settings", "[Cut]")
{
    Model            model;
    const TwoPartBox box = add_two_part_box(model);
    box.part_b->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(50.));

    Cut cut(box.object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& objects = run_dovetail(cut);

    REQUIRE(objects.size() == 2);
    for (const ModelObject* object : objects) {
        const std::vector<const ModelVolume*> parts = model_parts(*object);
        REQUIRE(parts.size() == 2);
        std::vector<std::string> densities;
        for (const ModelVolume* part : parts)
            if (part->config.has("sparse_infill_density"))
                densities.push_back(part->config.opt_serialize("sparse_infill_density"));
        REQUIRE(densities.size() == 1);
        REQUIRE(densities.front() == "50%");
    }
}

TEST_CASE("Paint stays on its own part after a dovetail cut", "[Cut]")
{
    Model            model;
    const TwoPartBox box = add_two_part_box(model);
    paint_away_from_joint(box.part_a);
    box.part_b->config.set("extruder", 2);

    Cut cut(box.object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs& objects = run_dovetail(cut);

    REQUIRE(objects.size() == 2);
    for (const ModelObject* object : objects) {
        const std::vector<const ModelVolume*> parts = model_parts(*object);
        REQUIRE(parts.size() == 2);
        std::vector<int> painted_filaments;
        for (const ModelVolume* part : parts)
            if (part->mmu_segmentation_facets.has_facets(*part, EnforcerBlockerType::Extruder2))
                painted_filaments.push_back(part->extruder_id());
        REQUIRE(painted_filaments.size() == 1);
        REQUIRE(painted_filaments.front() == 1);
    }
}

TEST_CASE("A dovetail cut keeps parts with different materials apart", "[Cut]")
{
    Model            model;
    const TwoPartBox box = add_two_part_box(model);
    box.part_a->set_material_id("mat_a");
    box.part_b->set_material_id("mat_b");

    Cut cut(box.object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& objects = run_dovetail(cut);

    REQUIRE(objects.size() == 2);
    for (const ModelObject* object : objects) {
        std::vector<std::string> materials;
        for (const ModelVolume* part : model_parts(*object)) {
            REQUIRE(part->extruder_id() == 1);
            materials.push_back(part->material_id());
        }
        std::sort(materials.begin(), materials.end());
        REQUIRE(materials == std::vector<std::string>{ "mat_a", "mat_b" });
    }
}

TEST_CASE("A dovetail cut inherits the object's filament", "[Cut]")
{
    Model            model;
    const TwoPartBox box = add_two_part_box(model);
    box.object->config.set("extruder", 3);

    Cut cut(box.object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& objects = run_dovetail(cut);

    REQUIRE(objects.size() == 2);
    for (const ModelObject* object : objects) {
        const std::vector<const ModelVolume*> parts = model_parts(*object);
        REQUIRE(parts.size() == 1);
        REQUIRE(parts.front()->extruder_id() == 3);
    }
}

TEST_CASE("Cut by contour keeps each part's filament", "[Cut]")
{
    Model        model;
    ModelObject* object = model.add_object();
    add_half(object, -12.5f);
    ModelVolume* right = add_half(object, 0.f);
    right->config.set("extruder", 2);
    object->add_instance();

    Cut cut(object, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& objects =
        cut.perform_by_contour(object, { { true, false }, { true, false } }, 0);

    REQUIRE(objects.size() == 1);
    std::vector<int> filaments;
    for (const ModelVolume* part : model_parts(*objects.front()))
        filaments.push_back(part->extruder_id());
    std::sort(filaments.begin(), filaments.end());
    REQUIRE(filaments == std::vector<int>{ 1, 2 });
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/GUI_ObjectList.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>

#include <string>
#include <cstddef>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {

ModelObject* add_box(Model& model, const std::string& name, const Vec3d& size, int filament = 1, double scale = 1.)
{
    ModelObject* object = model.add_object();
    object->name        = name;
    object->add_volume(make_cube(size.x(), size.y(), size.z()));
    object->config.set("extruder", filament);
    object->add_instance()->set_scaling_factor(Vec3d::Constant(scale));
    return object;
}

std::vector<std::string> sorted_names(const Model& model, ObjectOrder order, bool descending = false)
{
    std::vector<ModelObject*> objects = model.objects;
    sort_objects(objects, order, descending);
    std::vector<std::string> names;
    for (const ModelObject* object : objects)
        names.push_back(object->name);
    return names;
}

} // namespace

TEST_CASE("Objects sort by name in natural, case-insensitive order", "[ObjectList]")
{
    Model model;
    add_box(model, "part 10", {10., 10., 10.});
    add_box(model, "Part 2", {10., 10., 10.});
    add_box(model, "part 1", {10., 10., 10.});

    CHECK(sorted_names(model, ObjectOrder::Name) == std::vector<std::string>{"part 1", "Part 2", "part 10"});
}

TEST_CASE("Objects sort by filament and keep their order within a filament", "[ObjectList]")
{
    Model model;
    add_box(model, "a", {10., 10., 10.}, 2);
    add_box(model, "b", {10., 10., 10.}, 1);
    add_box(model, "c", {10., 10., 10.}, 2);
    add_box(model, "d", {10., 10., 10.}, 1);

    CHECK(sorted_names(model, ObjectOrder::Filament) == std::vector<std::string>{"b", "d", "a", "c"});
}

TEST_CASE("A descending sort keeps the order of objects with equal values", "[ObjectList]")
{
    Model model;
    add_box(model, "a", {10., 10., 10.}, 2);
    add_box(model, "b", {10., 10., 10.}, 1);
    add_box(model, "c", {10., 10., 10.}, 2);
    add_box(model, "d", {10., 10., 10.}, 1);

    CHECK(sorted_names(model, ObjectOrder::Filament, true) == std::vector<std::string>{"a", "c", "b", "d"});
}

TEST_CASE("Objects sort by the height of their scaled instance", "[ObjectList]")
{
    Model model;
    // 10 mm cube scaled 3x prints 30 mm tall, taller than the unscaled 20 mm box.
    add_box(model, "scaled cube", {10., 10., 10.}, 1, 3.);
    add_box(model, "box", {10., 10., 20.});
    add_box(model, "flat", {40., 40., 5.});

    CHECK(sorted_names(model, ObjectOrder::Height) == std::vector<std::string>{"flat", "box", "scaled cube"});
}

TEST_CASE("Objects sort by bounding box volume", "[ObjectList]")
{
    Model model;
    add_box(model, "flat", {40., 40., 5.});   // 8000 mm3
    add_box(model, "tall", {10., 10., 50.});  // 5000 mm3
    add_box(model, "cube", {10., 10., 10.});  // 1000 mm3

    CHECK(sorted_names(model, ObjectOrder::Size) == std::vector<std::string>{"cube", "tall", "flat"});
}

TEST_CASE("Imported part names sort only their presentation and survive cloning", "[ObjectList][OriginalNames]")
{
    Model model;
    ModelObject* object = model.add_object();
    // Include a modifier to ensure display sorting never changes its slicing precedence.
    auto* ten = object->add_volume(make_cube(10., 10., 10.));
    ten->name = "Part10";
    auto* two = object->add_volume(make_cube(2., 2., 2.));
    two->name = "part2";
    auto* duplicate = object->add_volume(make_cube(3., 3., 3.));
    duplicate->name = "part2";
    duplicate->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    const auto slicing_order = object->volumes;
    set_import_volume_order(*object, true);
    CHECK(object->volumes == slicing_order);
    CHECK(two->import_display_order < duplicate->import_display_order);
    CHECK(duplicate->import_display_order < ten->import_display_order);

    Model clone;
    const ModelObject* copied = clone.add_object(*object);
    REQUIRE(copied->volumes.size() == object->volumes.size());
    for (size_t i = 0; i < copied->volumes.size(); ++i) {
        CHECK(copied->volumes[i]->id() != object->volumes[i]->id());
        CHECK(copied->volumes[i]->import_display_order == object->volumes[i]->import_display_order);
    }
}

TEST_CASE("Unsorted imports preserve source presentation through type grouping", "[ObjectList][OriginalNames]")
{
    Model model;
    ModelObject* object = model.add_object();
    auto* modifier = object->add_volume(make_cube(2., 2., 2.));
    modifier->name = "Z modifier";
    modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    auto* part = object->add_volume(make_cube(10., 10., 10.));
    part->name = "A part";
    set_import_volume_order(*object, false);
    object->sort_volumes(true);
    CHECK(object->volumes.front() == part);
    CHECK(modifier->import_display_order < part->import_display_order);
}

TEST_CASE("Adding a part or modifier reveals a single-object import", "[ObjectList][OriginalNames]")
{
    const auto type = GENERATE(ModelVolumeType::MODEL_PART, ModelVolumeType::PARAMETER_MODIFIER);
    Model model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    object->add_volume(make_cube(2., 2., 2.));
    object->imported_parts_hidden = true;
    // An editing operation must not leave the newly added, effective geometry invisible.
    object->add_volume(make_cube(3., 3., 3.), type);
    CHECK_FALSE(object->imported_parts_hidden);
    REQUIRE(object->volumes.size() == 3);
    CHECK(object->volumes.back()->type() == type);
}

TEST_CASE("Part settings show a single-object import without changing it", "[ObjectList][OriginalNames]")
{
    Model model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    ModelVolume* body = object->add_volume(make_cube(2., 2., 2.));
    object->imported_parts_hidden = true;
    REQUIRE(object->hides_imported_parts());

    // The check only reads the object: the stored choice survives while part settings show the parts.
    body->config.set("wall_loops", 3);
    CHECK_FALSE(object->hides_imported_parts());
    CHECK(object->imported_parts_hidden);
    CHECK(body->config.has("wall_loops"));

    body->config.erase("wall_loops");
    CHECK(object->hides_imported_parts());

    TriangleSelector selector(body->mesh());
    selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    REQUIRE(body->seam_facets.set(selector));
    CHECK_FALSE(object->hides_imported_parts());
    CHECK(object->imported_parts_hidden);

    body->seam_facets.reset();
    CHECK(object->hides_imported_parts());
}

TEST_CASE("A part that repeats the object's filament has no settings of its own", "[ObjectList][OriginalNames]")
{
    struct Case { int object_filament; int part_filament; bool extra_setting; bool individual; };
    // object_filament 0 means no object filament, which prints with filament 1.
    const Case c = GENERATE(Case{0, 1, false, false}, Case{2, 2, false, false}, Case{2, 0, false, false},
                            Case{2, 1, false, true}, Case{0, 2, false, true}, Case{2, 2, true, true});
    CAPTURE(c.object_filament, c.part_filament, c.extra_setting);
    Model model;
    ModelObject* object = model.add_object();
    object->config.erase("extruder");
    if (c.object_filament > 0)
        object->config.set("extruder", c.object_filament);
    object->add_volume(make_cube(10., 10., 10.));
    ModelVolume* body = object->add_volume(make_cube(2., 2., 2.));
    body->config.set("extruder", c.part_filament);
    if (c.extra_setting)
        body->config.set("wall_loops", 3);
    object->imported_parts_hidden = true;
    CHECK(object->has_individual_part_settings() == c.individual);
    CHECK(object->hides_imported_parts() == !c.individual);
}

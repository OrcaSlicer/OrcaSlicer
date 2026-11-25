#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>

#include <string>
#include <cstddef>
#include <iterator>
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

namespace {

// The names of `objects` in their order.
std::vector<std::string> names_of(const std::vector<ModelObject*>& objects)
{
    std::vector<std::string> names;
    for (const ModelObject* object : objects)
        names.push_back(object->name);
    return names;
}

} // namespace

TEST_CASE("Order By orders by each key, ascending and descending", "[ObjectList]")
{
    // Each key gives the three another order: a is mid in filament, tallest and mid in volume; b the last filament,
    // the lowest and the largest; c the first filament, mid in height and the smallest.
    Model model;
    add_box(model, "a", {10., 10., 30.}, 2); // 3000 mm3
    add_box(model, "b", {20., 20., 10.}, 3); // 4000 mm3
    add_box(model, "c", {10., 10., 20.}, 1); // 2000 mm3
    using Names = std::vector<std::string>;
    struct Case { ObjectOrder order; Names ascending; };
    const Case c = GENERATE(Case{ObjectOrder::Name, {"a", "b", "c"}}, Case{ObjectOrder::Filament, {"c", "a", "b"}},
                            Case{ObjectOrder::Height, {"b", "c", "a"}}, Case{ObjectOrder::Size, {"c", "a", "b"}});
    const auto on_one_plate = [](std::size_t) { return 0; };
    CHECK(names_of(plate_order(model.objects, on_one_plate, 0, c.order, false)) == c.ascending);
    const Names descending(c.ascending.rbegin(), c.ascending.rend());
    CHECK(names_of(plate_order(model.objects, on_one_plate, 0, c.order, true)) == descending);
}

TEST_CASE("Order By sorts a plate's objects in the list positions they hold, and the other plates keep theirs", "[ObjectList]")
{
    Model model;
    for (const char* name : {"d", "y", "b", "x", "a"})
        add_box(model, name, {10., 10., 10.});
    // d, b and a are on plate 0; y and x on plate 1.
    const auto plate_of = [](std::size_t object) { return object == 1 || object == 3 ? 1 : 0; };
    CHECK(names_of(plate_order(model.objects, plate_of, 0, ObjectOrder::Name, false)) == std::vector<std::string>{"a", "y", "b", "x", "d"});
    CHECK(names_of(plate_order(model.objects, plate_of, 1, ObjectOrder::Name, false)) == std::vector<std::string>{"d", "x", "b", "y", "a"});
    // Every plate at once, each in its own positions.
    CHECK(names_of(plate_order(model.objects, plate_of, -1, ObjectOrder::Name, true)) == std::vector<std::string>{"d", "y", "b", "x", "a"});
    // An object on no plate keeps its place.
    const auto one_off = [](std::size_t object) { return object == 0 ? -1 : 0; };
    CHECK(names_of(plate_order(model.objects, one_off, -1, ObjectOrder::Name, false)) == std::vector<std::string>{"d", "a", "b", "x", "y"});
}

// The names of `objects` after the drops `moves`, each as the object list makes it (move_object_in_list()).
std::vector<std::string> names_after(std::vector<ModelObject*> objects, const std::vector<ObjectMove>& moves)
{
    for (const ObjectMove& move : moves)
        move_object_in_list(objects, move.from, move.to);
    return names_of(objects);
}

TEST_CASE("An order of a plate's objects becomes the drops that put them in that order, the other plates keeping theirs", "[ObjectList]")
{
    Model model;
    for (const char* name : {"a", "x", "b", "c", "y"})
        add_box(model, name, {10., 10., 10.});
    // a, b and c are on plate 0; x and y on plate 1.
    const std::vector<int> plate_of{0, 1, 0, 0, 1};

    const Ops::Outcome<std::vector<ObjectMove>> moves = object_order_moves(plate_of, 0, {3, 0, 2});
    REQUIRE(moves);
    CHECK_FALSE(moves.value().empty());
    // c, dropped on a, takes its place, and those after it move down: plate 0 reads c, a, b, and x stays before y.
    CHECK(names_after(model.objects, moves.value()) == std::vector<std::string>{"c", "a", "x", "b", "y"});

    const Ops::Outcome<std::vector<ObjectMove>> other = object_order_moves(plate_of, 1, {4, 1});
    REQUIRE(other);
    CHECK(names_after(model.objects, other.value()) == std::vector<std::string>{"a", "y", "x", "b", "c"});

    // The order they have already takes no drop.
    const Ops::Outcome<std::vector<ObjectMove>> kept = object_order_moves(plate_of, 0, {0, 2, 3});
    REQUIRE(kept);
    CHECK(kept.value().empty());
}

TEST_CASE("An order is refused when it leaves out an object of the plate, repeats one, or names one of another plate", "[ObjectList]")
{
    // Objects 0, 2 and 3 are on plate 0, 1 on plate 1, and 4 on no plate.
    const std::vector<int> plate_of{0, 1, 0, 0, -1};
    const auto refusal = [&plate_of](const std::vector<int>& order) {
        const Ops::Outcome<std::vector<ObjectMove>> moves = object_order_moves(plate_of, 0, order);
        REQUIRE_FALSE(moves);
        return moves.refusal();
    };
    const Ops::Refusal missing = refusal({3, 0});
    CHECK(std::string(missing.code) == "order_misses_object");
    CHECK(missing.names == std::vector<std::string>{"2"});
    CHECK(missing.english() == "The order leaves out objects of plate 0: 2. Name each of its objects once.");

    const Ops::Refusal repeated = refusal({0, 2, 2, 3});
    CHECK(std::string(repeated.code) == "order_repeats_object");
    CHECK(repeated.english() == "The order names object 2 twice.");

    const Ops::Refusal elsewhere = refusal({0, 1, 2, 3});
    CHECK(std::string(elsewhere.code) == "object_not_on_plate");
    CHECK(elsewhere.english() == "Object 1 is not on plate 0.");
    CHECK(std::string(refusal({0, 2, 3, 4}).code) == "object_not_on_plate");

    const Ops::Refusal unknown = refusal({0, 2, 3, 7});
    CHECK(std::string(unknown.code) == "no_such_object");
    CHECK(unknown.english() == "Object 7 does not exist: the project has objects 0 to 4.");
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

TEST_CASE("An object row reads its model object only while the model still holds it", "[ObjectList]")
{
    Model model;
    ModelObject* held = model.add_object();
    model.add_object();
    Model other;
    const ModelObject* elsewhere = other.add_object();

    CHECK(ObjectDataViewModel::live_model_object(held, model.objects) == held);
    CHECK(ObjectDataViewModel::live_model_object(elsewhere, model.objects) == nullptr);
    CHECK(ObjectDataViewModel::live_model_object(nullptr, model.objects) == nullptr);
}

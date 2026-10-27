#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/GUI_ObjectList.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <string>
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

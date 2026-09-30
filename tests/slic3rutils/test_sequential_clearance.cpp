#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "fff_print/test_helpers.hpp"
#include "slic3r/GUI/SequentialPrintClearance.hpp"

#include <map>

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {

SequentialClearanceInstance instance(coord_t x, coord_t y, double height)
{
    return {height, BoundingBox(Point(x, y), Point(x + 5, y + 10)),
        Polygon{Point(x, y), Point(x + 5, y), Point(x + 5, y + 10), Point(x, y + 10)}};
}

} // namespace

TEST_CASE("Sequential clearance preserves model print order when objects move", "[SequentialClearance][Regression]")
{
    const coord_t last_y = GENERATE(-20, 20);
    std::vector<SequentialClearanceInstance> instances{instance(20, 0, 50.), instance(0, last_y, 50.)};
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);

    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().first == instance(20, 0, 50.).hull_polygon);
    CHECK_THAT(warnings.front().second, WithinAbs(30., 1e-6));
}

TEST_CASE("Sequential clearance handles chains of overlapping Y ranges", "[SequentialClearance][Regression]")
{
    // A overlaps B, B overlaps C, but A does not overlap C. Combining X order
    // for overlaps with Y order otherwise formed the cycle B < A < C < B.
    std::vector<SequentialClearanceInstance> instances{
        instance(20, 0, 50.), instance(10, 8, 50.), instance(0, 16, 50.)};
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);

    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].first == instance(20, 0, 50.).hull_polygon);
    CHECK(warnings[1].first == instance(10, 8, 50.).hull_polygon);
    CHECK_THAT(warnings[0].second, WithinAbs(10., 1e-6));
    CHECK_THAT(warnings[1].second, WithinAbs(10., 1e-6));
}

TEST_CASE("Sequential clearance applies rod height only to positive Y overlap", "[SequentialClearance]")
{
    const coord_t next_y = GENERATE(9, 10, 11);
    std::vector<SequentialClearanceInstance> instances{instance(0, 0, 50.), instance(20, next_y, 50.)};
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);

    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().first == instances.front().hull_polygon);
    CHECK_THAT(warnings.front().second, WithinAbs(next_y < 10 ? 10. : 30., 1e-6));
}

TEST_CASE("Sequential clearance allows the last object up to printable height", "[SequentialClearance]")
{
    std::vector<SequentialClearanceInstance> instances;
    CHECK(sequential_clearance_height_polygons(instances, 200., 30., 10.).empty());

    instances.push_back(instance(0, 0, 200.));
    CHECK(sequential_clearance_height_polygons(instances, 200., 30., 10.).empty());

    instances.front().instance_height = 201.;
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);
    REQUIRE(warnings.size() == 1);
    CHECK_THAT(warnings.front().second, WithinAbs(200., 1e-6));
}

TEST_CASE("Sequential clearance warns the same rotated copy as print validation", "[SequentialClearance][Regression]")
{
    constexpr double rod_height = 10.;
    constexpr double lid_height = 30.;
    constexpr double printable_height = 200.;
    Model model = Test::model("rotated copies", make_cube(10., 10., 50.));
    ModelObject* object = model.objects.front();
    object->instances.front()->set_offset(Vec3d(150., 80., 0.));
    ModelInstance* rotated = object->add_instance();
    rotated->set_offset(Vec3d(50., 80., 0.));
    rotated->set_rotation(Z, PI / 2.);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"print_sequence", "by object"}, {"extruder_clearance_radius", 20.},
        {"extruder_clearance_height_to_lid", lid_height}, {"extruder_clearance_height_to_rod", rod_height},
        {"printable_height", printable_height}});
    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    Polygons polygons;
    std::vector<std::pair<Polygon, float>> validation_warnings;
    Print::sequential_print_clearance_valid(print, &polygons, &validation_warnings);

    std::map<ObjectID, int> print_order;
    for (const PrintObject* print_object : print.objects())
        for (const PrintInstance& print_instance : print_object->instances())
            print_order.emplace(print_instance.model_instance->id(), print_instance.model_instance->arrange_order);

    std::vector<SequentialClearanceInstance> instances;
    for (size_t i = 0; i < object->instances.size(); ++i) {
        Polygon hull = object->convex_hull_2d(object->instances[i]->get_matrix());
        instances.push_back({object->get_instance_max_z(i), hull.bounding_box(), hull, object->instances[i]->id()});
    }
    const Polygon expected_warning = instances[1].hull_polygon;
    REQUIRE(validation_warnings.size() == 1);
    CHECK(validation_warnings.front().first.bounding_box().contains(instances[1].bounding_box.center()));
    CHECK_FALSE(validation_warnings.front().first.bounding_box().contains(instances[0].bounding_box.center()));

    sort_sequential_clearance_instances(instances.begin(), instances.end(), print_order);
    const auto warnings = sequential_clearance_height_polygons(instances, printable_height, lid_height, rod_height);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().first == expected_warning);
    CHECK_THAT(warnings.front().second, WithinAbs(rod_height, 1e-6));
}

TEST_CASE("Sequential clearance keeps copy order when print order is unavailable", "[SequentialClearance]")
{
    std::vector<SequentialClearanceInstance> instances{instance(20, 0, 50.), instance(0, 0, 50.)};
    instances[0].instance_id = ObjectID(1);
    instances[1].instance_id = ObjectID(2);
    std::map<ObjectID, int> print_order{{ObjectID(1), 2}, {ObjectID(2), 1}};

    SECTION("No validation order") { print_order.clear(); }
    SECTION("New copy missing from print") { print_order.erase(ObjectID(2)); }
    SECTION("Copy has not been ordered") { print_order[ObjectID(2)] = 0; }
    SECTION("Copy has an invalid order") { print_order[ObjectID(2)] = -1; }

    sort_sequential_clearance_instances(instances.begin(), instances.end(), print_order);
    CHECK(instances[0].instance_id == ObjectID(1));
    CHECK(instances[1].instance_id == ObjectID(2));
}

TEST_CASE("Sequential clearance keeps object order when sorting copies", "[SequentialClearance]")
{
    std::vector<SequentialClearanceInstance> instances(4, instance(0, 0, 50.));
    for (size_t i = 0; i < instances.size(); ++i)
        instances[i].instance_id = ObjectID(instances.size() - i);
    const std::map<ObjectID, int> print_order{{ObjectID(1), 1}, {ObjectID(2), 2}, {ObjectID(3), 3}, {ObjectID(4), 4}};

    // The two objects were reordered in the model since the last validation.
    sort_sequential_clearance_instances(instances.begin(), instances.begin() + 2, print_order);
    sort_sequential_clearance_instances(instances.begin() + 2, instances.end(), print_order);
    CHECK(instances[0].instance_id == ObjectID(3));
    CHECK(instances[1].instance_id == ObjectID(4));
    CHECK(instances[2].instance_id == ObjectID(1));
    CHECK(instances[3].instance_id == ObjectID(2));
}

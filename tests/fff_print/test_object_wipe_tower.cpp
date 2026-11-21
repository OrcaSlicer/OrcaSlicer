#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Two 20 mm cubes on a plate with two filaments; the first one prints with both, the second with one.
// Their positions are fixed, so the tower can be looked for beside a known object.
struct TwoObjects
{
    Print print;
    Model model;

    TwoObjects(std::initializer_list<ConfigBase::SetDeserializeItem> extra, bool second_object_uses_both = false)
    {
        std::vector<TriangleMesh> meshes;
        for (double x : {40., 120.}) {
            TriangleMesh mesh = cube(20);
            mesh.translate(x, 80, 0);
            meshes.push_back(std::move(mesh));
        }
        const DynamicPrintConfig config = multifilament_config(2, extra);
        std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"wall_filament", "2"}}, {}};
        if (second_object_uses_both)
            overrides[1] = {{"wall_filament", "2"}};
        init_print(std::move(meshes), print, model, config, &overrides, /*arrange=*/false);
    }

    // The footprint of object `index` on the plate, in scaled coordinates.
    Polygon footprint(size_t index) const
    {
        const BoundingBoxf3 box = model.objects[index]->instance_bounding_box(0);
        return Polygon::new_scale(std::vector<Vec2d>{{box.min.x(), box.min.y()}, {box.max.x(), box.min.y()}, {box.max.x(), box.max.y()}, {box.min.x(), box.max.y()}});
    }
};

} // namespace

TEST_CASE("Only an object that uses several filaments gets a prime tower of its own", "[ObjectWipeTower]")
{
    TwoObjects plate({{"print_sequence", "by object"}});
    const Print::ObjectWipeTowerPlacements towers = plate.print.estimate_object_wipe_tower_placements();
    REQUIRE(towers.size() == 1);
    CHECK(towers.begin()->first->print_object->model_object() == plate.model.objects[0]);
}

TEST_CASE("Every multi-filament object gets its own tower", "[ObjectWipeTower]")
{
    TwoObjects plate({{"print_sequence", "by object"}}, /*second_object_uses_both=*/true);
    CHECK(plate.print.estimate_object_wipe_tower_placements().size() == 2);
}

TEST_CASE("An object tower stands on the bed clear of every object", "[ObjectWipeTower]")
{
    TwoObjects plate({{"print_sequence", "by object"}}, /*second_object_uses_both=*/true);
    const Print::ObjectWipeTowerPlacements towers = plate.print.estimate_object_wipe_tower_placements();
    REQUIRE(towers.size() == 2);
    Polygons bed = plate.print.get_extruder_shared_printable_polygon();
    for (const auto &[instance, tower] : towers) {
        CHECK(diff(Polygons{tower.footprint}, bed).empty());
        for (size_t object = 0; object < 2; ++object)
            CHECK(intersection(Polygons{tower.footprint}, Polygons{plate.footprint(object)}).empty());
    }
}

TEST_CASE("Object towers are not estimated when printing by layer", "[ObjectWipeTower]")
{
    TwoObjects plate({{"print_sequence", "by layer"}});
    CHECK(plate.print.estimate_object_wipe_tower_placements().empty());
}

TEST_CASE("A plate where every object prints with one filament has no object towers", "[ObjectWipeTower]")
{
    Print print;
    Model model;
    std::vector<TriangleMesh> meshes;
    for (double x : {40., 120.}) {
        TriangleMesh mesh = cube(20);
        mesh.translate(x, 80, 0);
        meshes.push_back(std::move(mesh));
    }
    init_print(std::move(meshes), print, model, multifilament_config(2, {{"print_sequence", "by object"}}), nullptr, /*arrange=*/false);
    CHECK(print.estimate_object_wipe_tower_placements().empty());
}

TEST_CASE("An object tower that does not fit on the bed is reported instead of placed", "[ObjectWipeTower]")
{
    // The bed leaves only a few millimetres around the first object, far less than any tower needs.
    TwoObjects plate({{"print_sequence", "by object"}, {"printable_area", "35x75,65x75,65x105,35x105"}});
    std::vector<StringObjectException> warnings;
    const Print::ObjectWipeTowerPlacements towers = plate.print.estimate_object_wipe_tower_placements(&warnings);
    CHECK(towers.empty());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().is_warning);
}

#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
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

TEST_CASE("Object prime towers are off unless the setting asks for them", "[ObjectWipeTower]")
{
    TwoObjects plate({{"print_sequence", "by object"}});
    CHECK_FALSE(plate.print.has_object_wipe_towers());
}

TEST_CASE("Object prime towers need the by object print sequence", "[ObjectWipeTower]")
{
    TwoObjects by_object({{"print_sequence", "by object"}, {"prime_tower_per_object", "1"}, {"wipe_tower_type", "type1"}});
    CHECK(by_object.print.has_object_wipe_towers());
    TwoObjects by_layer({{"print_sequence", "by layer"}, {"prime_tower_per_object", "1"}, {"wipe_tower_type", "type1"}});
    CHECK_FALSE(by_layer.print.has_object_wipe_towers());
}

TEST_CASE("A tower beside an object that leaves no room on the bed is a warning only when object towers are on", "[ObjectWipeTower]")
{
    // Both objects fit on the bed, but the space between them is excluded and the rest leaves only a few
    // millimetres around them, far less than any tower needs.
    const auto warnings_for = [](const char *per_object) {
        TwoObjects plate({{"print_sequence", "by object"}, {"prime_tower_per_object", per_object}, {"wipe_tower_type", "type1"},
                          {"printable_area", "35x75,145x75,145x105,35x105"}, {"bed_exclude_area", "62x70,118x70,118x110,62x110"}});
        std::vector<StringObjectException> warnings;
        plate.print.validate(&warnings);
        return std::count_if(warnings.begin(), warnings.end(), [&](const StringObjectException &warning) {
            return warning.object == plate.model.objects[0];
        });
    };
    CHECK(warnings_for("0") == 0);
    CHECK(warnings_for("1") == 1);
}

namespace {

const PrintObject &print_object_of(const Print &print, const ModelObject *model_object)
{
    for (const PrintObject *object : print.objects())
        if (object->model_object() == model_object)
            return *object;
    throw std::runtime_error("The model object is not in the print.");
}

} // namespace

TEST_CASE("No object towers are generated unless the setting asks for them", "[ObjectWipeTower]")
{
    TwoObjects plate({{"print_sequence", "by object"}, {"wipe_tower_type", "type1"}});
    plate.print.process();
    CHECK(plate.print.object_wipe_tower_data(&print_object_of(plate.print, plate.model.objects[0])) == nullptr);
    CHECK(plate.print.object_wipe_tower_placements().empty());
}

TEST_CASE("Slicing generates and places a prime tower for the multi-filament object only", "[ObjectWipeTower]")
{
    TwoObjects plate({{"print_sequence", "by object"}, {"prime_tower_per_object", "1"}, {"wipe_tower_type", "type1"}});
    plate.print.process();

    const WipeTowerData *tower = plate.print.object_wipe_tower_data(&print_object_of(plate.print, plate.model.objects[0]));
    REQUIRE(tower != nullptr);
    CHECK_FALSE(tower->tool_changes.empty());
    CHECK(plate.print.object_wipe_tower_data(&print_object_of(plate.print, plate.model.objects[1])) == nullptr);

    const Print::ObjectWipeTowerPlacements &placements = plate.print.object_wipe_tower_placements();
    REQUIRE(placements.size() == 1);
    const ObjectWipeTowerPlacement &placement = placements.begin()->second;
    CHECK(diff(Polygons{placement.footprint}, plate.print.get_extruder_shared_printable_polygon()).empty());
    for (size_t object = 0; object < 2; ++object)
        CHECK(intersection(Polygons{placement.footprint}, Polygons{plate.footprint(object)}).empty());
}

TEST_CASE("Generating an object tower leaves the layers of the object alone", "[ObjectWipeTower]")
{
    TwoObjects without({{"print_sequence", "by object"}, {"wipe_tower_type", "type1"}});
    without.print.process();
    TwoObjects with({{"print_sequence", "by object"}, {"prime_tower_per_object", "1"}, {"wipe_tower_type", "type1"}});
    with.print.process();
    const PrintObject &plain = print_object_of(without.print, without.model.objects[0]);
    const PrintObject &towered = print_object_of(with.print, with.model.objects[0]);
    CHECK(towered.layers().size() == plain.layers().size());
    CHECK(towered.support_layers().size() == plain.support_layers().size());
}

TEST_CASE("The G-code of a by-object print carries the object prime tower only when asked for", "[ObjectWipeTower]")
{
    const std::string tower_role = ";TYPE:Prime tower";
    TwoObjects without({{"print_sequence", "by object"}, {"wipe_tower_type", "type1"}});
    CHECK(gcode(without.print).find(tower_role) == std::string::npos);

    TwoObjects with({{"print_sequence", "by object"}, {"prime_tower_per_object", "1"}, {"wipe_tower_type", "type1"}});
    CHECK(gcode(with.print).find(tower_role) != std::string::npos);
}

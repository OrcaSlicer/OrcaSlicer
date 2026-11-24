// PartPlate.hpp reaches wx/wxprec.h, which needs the Windows headers first.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    // Match the GUI precompiled header: wx/msw/wrapcctl.h needs HDITEM from CommCtrl.h.
    #include <CommCtrl.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <utility>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/PartPlate.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {
DynamicPrintConfig nozzle_config(std::vector<double> nozzle_diameters,
                                 std::vector<int>    filament_map,
                                 std::vector<int>    physical_extruder_map)
{
    DynamicPrintConfig config;
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::move(nozzle_diameters)));
    config.set_key_value("filament_map", new ConfigOptionInts(std::move(filament_map)));
    config.set_key_value("physical_extruder_map", new ConfigOptionInts(std::move(physical_extruder_map)));
    return config;
}
} // namespace

TEST_CASE("A multi-nozzle printer without its own physical extruder map puts each filament on its own nozzle", "[PartPlate]")
{
    // Four nozzles, filament n on nozzle n, and the single-entry physical_extruder_map default.
    const DynamicPrintConfig config = nozzle_config({0.4, 0.4, 0.4, 0.4}, {1, 2, 3, 4}, {0});
    const PartPlate          plate;

    const int filament_id = GENERATE(1, 2, 3, 4);
    CHECK(plate.get_physical_extruder_by_filament_id(config, filament_id) == filament_id - 1);
}

TEST_CASE("A physical extruder map the profile sets decides the physical nozzle", "[PartPlate]")
{
    // Two nozzles whose physical order is the reverse of the logical one, as on a two-nozzle Bambu printer.
    const DynamicPrintConfig config = nozzle_config({0.4, 0.4}, {1, 2}, {1, 0});
    const PartPlate          plate;

    CHECK(plate.get_physical_extruder_by_filament_id(config, 1) == 1);
    CHECK(plate.get_physical_extruder_by_filament_id(config, 2) == 0);
}

TEST_CASE("A filament with no extruder to map to has no physical nozzle", "[PartPlate]")
{
    // Filament 2 is mapped to a fifth extruder the printer does not have; filament 0 and 3 do not exist.
    const DynamicPrintConfig config = nozzle_config({0.4, 0.4, 0.4, 0.4}, {1, 5}, {0});
    const PartPlate          plate;

    CHECK(plate.get_physical_extruder_by_filament_id(config, 2) == -1);
    CHECK(plate.get_physical_extruder_by_filament_id(config, 0) == -1);
    CHECK(plate.get_physical_extruder_by_filament_id(config, 3) == -1);
}

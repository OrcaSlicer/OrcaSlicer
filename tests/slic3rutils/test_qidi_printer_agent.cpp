#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

#include "slic3r/Utils/QidiPrinterAgent.hpp"

using namespace Slic3r;

TEST_CASE("A filament mapped to a Box slot saves that slot in its tool variable", "[QidiPrinterAgent]")
{
    // Filament 2 is mapped to slot 5; filaments 0 and 1 have no slot.
    // SAVE_VARIABLE reads VALUE as a Python literal, so the slot name needs its own quotes.
    CHECK(QidiPrinterAgent::build_tool_slot_script("[-1, -1, 5]") == "SAVE_VARIABLE VARIABLE=value_t2 VALUE=\"'slot5'\"");
}

TEST_CASE("Each mapped filament gets its own tool variable line", "[QidiPrinterAgent]")
{
    CHECK(QidiPrinterAgent::build_tool_slot_script("[3, -1, 0]") ==
          "SAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot3'\"\n"
          "SAVE_VARIABLE VARIABLE=value_t2 VALUE=\"'slot0'\"");
}

TEST_CASE("A mapping without a usable Box slot saves nothing", "[QidiPrinterAgent]")
{
    const std::string mapping = GENERATE(as<std::string>{},
                                         "",           // empty mapping
                                         "[]",         // no filaments
                                         "[-1, -1]",   // no filament has a slot
                                         "[16]",       // past the firmware's slot15
                                         "[\"slot1\"]", // not a slot index
                                         "{\"0\": 1}", // not an array
                                         "[1,");       // not valid JSON
    CAPTURE(mapping);
    CHECK(QidiPrinterAgent::build_tool_slot_script(mapping).empty());
}

TEST_CASE("Filaments past the firmware's last tool variable are skipped", "[QidiPrinterAgent]")
{
    // The firmware has value_t0 to value_t15: filament 15 maps to slot 1, filament 16 to slot 4.
    std::string mapping = "[";
    for (int filament = 0; filament < 15; ++filament)
        mapping += "-1, ";
    mapping += "1, 4]";

    CHECK(QidiPrinterAgent::build_tool_slot_script(mapping) == "SAVE_VARIABLE VARIABLE=value_t15 VALUE=\"'slot1'\"");
}

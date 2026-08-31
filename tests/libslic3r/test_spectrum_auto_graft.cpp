#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SpectrumAutoGraft.hpp"

using namespace Slic3r;

namespace {

DynamicPrintConfig make_cfg_with_colours(const std::vector<std::string> &hexes)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_colour", new ConfigOptionStrings(hexes));
    return cfg;
}

} // namespace

TEST_CASE("load-is-project-open is true only for model+config without restore", "[spectrum_auto_graft]")
{
    CHECK(spectrum_auto_graft_load_is_project_open(true, true, false));
    CHECK_FALSE(spectrum_auto_graft_load_is_project_open(false, true, false));
    CHECK_FALSE(spectrum_auto_graft_load_is_project_open(true, false, false));
    CHECK_FALSE(spectrum_auto_graft_load_is_project_open(true, true, true));
    CHECK_FALSE(spectrum_auto_graft_load_is_project_open(false, false, false));
}

TEST_CASE("n=4 X1C-like should auto-graft", "[spectrum_auto_graft]")
{
    DynamicPrintConfig cfg = make_cfg_with_colours(
        {"#312631", "#808080", "#FF00AA", "#AABBCC"});
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab X1 Carbon"));
    cfg.set_key_value("printer_settings_id", new ConfigOptionString("Bambu Lab X1 Carbon 0.4 nozzle"));
    CHECK(spectrum_should_auto_graft_leq4(cfg));
    CHECK_FALSE(spectrum_is_zr_ultra_s_dest(cfg));
}

TEST_CASE("n=8 should not auto-graft", "[spectrum_auto_graft]")
{
    DynamicPrintConfig cfg = make_cfg_with_colours(
        {"#01", "#02", "#03", "#04", "#05", "#06", "#07", "#08"});
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab X1 Carbon"));
    CHECK_FALSE(spectrum_should_auto_graft_leq4(cfg));
}

TEST_CASE("Ultra S model without nozzle_diameter is dest; should_graft false", "[spectrum_auto_graft]")
{
    DynamicPrintConfig cfg = make_cfg_with_colours({"#AA", "#BB", "#CC", "#DD"});
    cfg.set_key_value("printer_model", new ConfigOptionString("WonderMaker ZR Ultra S"));
    // Intentionally omit nozzle_diameter — must not crash; name gate still dest.
    CHECK(spectrum_is_zr_ultra_s_dest(cfg));
    CHECK_FALSE(spectrum_should_auto_graft_leq4(cfg));
}

TEST_CASE("4-nozzle SEMM=0 without WonderMaker name is not dest", "[spectrum_auto_graft]")
{
    DynamicPrintConfig cfg = make_cfg_with_colours({"#1", "#2", "#3", "#4"});
    cfg.set_key_value("printer_model", new ConfigOptionString("Generic Toolchanger"));
    cfg.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4, 0.4}));
    cfg.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    CHECK_FALSE(spectrum_is_zr_ultra_s_dest(cfg));
    CHECK(spectrum_should_auto_graft_leq4(cfg));
}

TEST_CASE("n=0 helper returns false", "[spectrum_auto_graft]")
{
    DynamicPrintConfig cfg = make_cfg_with_colours({});
    CHECK_FALSE(spectrum_should_auto_graft_leq4(cfg));
}

TEST_CASE("missing filament_colour helper returns false", "[spectrum_auto_graft]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab X1 Carbon"));
    CHECK_FALSE(spectrum_should_auto_graft_leq4(cfg));
}

TEST_CASE("n=1 non-dest should auto-graft", "[spectrum_auto_graft]")
{
    DynamicPrintConfig cfg = make_cfg_with_colours({"#FFFFFFFF"});
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab A1"));
    CHECK(spectrum_should_auto_graft_leq4(cfg));
}

TEST_CASE("printer_settings_id containing WonderMaker ZR Ultra is dest", "[spectrum_auto_graft]")
{
    DynamicPrintConfig cfg = make_cfg_with_colours({"#1", "#2", "#3", "#4"});
    cfg.set_key_value("printer_settings_id",
                      new ConfigOptionString("WonderMaker ZR Ultra S 0.4 nozzle"));
    CHECK(spectrum_is_zr_ultra_s_dest(cfg));
    CHECK_FALSE(spectrum_should_auto_graft_leq4(cfg));
}

TEST_CASE("restore dest>=4 overwrites source slots", "[spectrum_auto_graft]")
{
    std::vector<std::string> colour = {"#312631", "#808080", "#FF00AA", "#AABBCC"};
    const std::vector<std::string> dest = {"#AABBCC", "#11", "#22", "#33"};
    REQUIRE(spectrum_restore_dest_filament_colours(colour, dest));
    REQUIRE(colour.size() == 4);
    CHECK(colour[0] == "#AABBCC");
    CHECK(colour[1] == "#11");
    CHECK(colour[2] == "#22");
    CHECK(colour[3] == "#33");
}

TEST_CASE("restore dest size 1 resizes and pads last hex", "[spectrum_auto_graft]")
{
    std::vector<std::string> colour = {"#SRC"};
    const std::vector<std::string> dest = {"#DESTONLY"};
    REQUIRE(spectrum_restore_dest_filament_colours(colour, dest));
    REQUIRE(colour.size() == 4);
    CHECK(colour[0] == "#DESTONLY");
    CHECK(colour[1] == "#DESTONLY");
    CHECK(colour[2] == "#DESTONLY");
    CHECK(colour[3] == "#DESTONLY");
}

TEST_CASE("restore dest empty does not write", "[spectrum_auto_graft]")
{
    std::vector<std::string> colour = {"#KEEP0", "#KEEP1", "#KEEP2", "#KEEP3"};
    const std::vector<std::string> dest;
    CHECK_FALSE(spectrum_restore_dest_filament_colours(colour, dest));
    REQUIRE(colour.size() == 4);
    CHECK(colour[0] == "#KEEP0");
    CHECK(colour[1] == "#KEEP1");
    CHECK(colour[2] == "#KEEP2");
    CHECK(colour[3] == "#KEEP3");
}

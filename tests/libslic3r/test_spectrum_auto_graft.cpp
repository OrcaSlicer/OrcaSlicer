#include <catch2/catch_all.hpp>

#include <string>
#include <utility>
#include <vector>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SpectrumAutoGraft.hpp"
#include "libslic3r/MixedFilament.hpp"

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

TEST_CASE("pick PETG when session is WonderMaker PLA Basic", "[spectrum_auto_graft]")
{
    const std::vector<std::pair<std::string, std::string>> name_and_type = {
        {"WonderMaker PLA Basic", "PLA"},
        {"WonderMaker PETG Basic", "PETG"},
    };
    CHECK(spectrum_pick_filament_name_for_type("PETG", "WonderMaker PLA Basic", name_and_type) ==
          "WonderMaker PETG Basic");
}

TEST_CASE("pick prefers session name when already PETG Basic", "[spectrum_auto_graft]")
{
    const std::vector<std::pair<std::string, std::string>> name_and_type = {
        {"WonderMaker PLA Basic", "PLA"},
        {"WonderMaker PETG Basic", "PETG"},
        {"Other PETG", "PETG"},
    };
    CHECK(spectrum_pick_filament_name_for_type("PETG", "WonderMaker PETG Basic", name_and_type) ==
          "WonderMaker PETG Basic");
}

TEST_CASE("wanted PET-CF does not return PETG", "[spectrum_auto_graft]")
{
    const std::vector<std::pair<std::string, std::string>> name_and_type = {
        {"WonderMaker PLA Basic", "PLA"},
        {"WonderMaker PETG Basic", "PETG"},
    };
    CHECK(spectrum_pick_filament_name_for_type("PET-CF", "WonderMaker PLA Basic", name_and_type).empty());
}

TEST_CASE("empty wanted returns session name", "[spectrum_auto_graft]")
{
    const std::vector<std::pair<std::string, std::string>> name_and_type = {
        {"WonderMaker PLA Basic", "PLA"},
        {"WonderMaker PETG Basic", "PETG"},
    };
    CHECK(spectrum_pick_filament_name_for_type("", "WonderMaker PLA Basic", name_and_type) ==
          "WonderMaker PLA Basic");
}

TEST_CASE("no type match returns empty string", "[spectrum_auto_graft]")
{
    const std::vector<std::pair<std::string, std::string>> name_and_type = {
        {"WonderMaker PLA Basic", "PLA"},
    };
    CHECK(spectrum_pick_filament_name_for_type("PETG", "WonderMaker PLA Basic", name_and_type).empty());
}

TEST_CASE("filament_type_at pads last and empty types", "[spectrum_auto_graft]")
{
    CHECK(spectrum_filament_type_at({}, 0).empty());
    CHECK(spectrum_filament_type_at({}, 3).empty());
    const std::vector<std::string> one{"PETG"};
    CHECK(spectrum_filament_type_at(one, 0) == "PETG");
    CHECK(spectrum_filament_type_at(one, 3) == "PETG");
    const std::vector<std::string> mixed{"PETG", "PLA"};
    CHECK(spectrum_filament_type_at(mixed, 0) == "PETG");
    CHECK(spectrum_filament_type_at(mixed, 1) == "PLA");
    CHECK(spectrum_filament_type_at(mixed, 3) == "PLA");
}

// Inspected PaletteSpheres26 mix string (not the 3mf zip).
static const char *k_palette26_snapmaker_excerpt_graft =
    "1,2,0,0,50,0,g,w,m2,d1,o1,u1;1,3,0,0,50,0,g,w,m2,d1,o1,u2;1,4,0,0,50,0,g,w,m2,d1,o1,u3;"
    "2,3,0,0,50,0,g,w,m2,d1,o1,u4;2,4,0,0,50,0,g,w,m2,d1,o1,u5;3,4,0,0,50,0,g,w,m2,d1,o1,u6;"
    "1,2,1,1,50,0,g,w,m2,d0,o0,u29,12;1,2,1,1,0,0,g,w,m2,d0,o0,u47,13;1,2,1,1,0,0,g,w,m2,d0,o0,u48,14;"
    "1,2,1,1,50,0,g,w,m2,d0,o0,u49,23;1,2,1,1,50,0,g,w,m2,d0,o0,u50,24;1,2,1,1,0,0,g,w,m2,d0,o0,u51,34;"
    "1,2,1,1,33,0,g,w,m2,d0,o0,u52,112;1,2,1,1,0,0,g,w,m2,d0,o0,u53,113;1,2,1,1,0,0,g,w,m2,d0,o0,u54,114;"
    "1,2,1,1,67,0,g,w,m2,d0,o0,u55,122;1,2,1,1,33,0,g,w,m2,d0,o0,u56,123;1,2,1,1,33,0,g,w,m2,d0,o0,u57,124;"
    "1,2,1,1,0,0,g,w,m2,d0,o0,u58,133;1,2,1,1,0,0,g,w,m2,d0,o0,u59,134;1,2,1,1,0,0,g,w,m2,d0,o0,u60,144;"
    "1,2,1,1,67,0,g,w,m2,d0,o0,u61,223;1,2,1,1,67,0,g,w,m2,d0,o0,u62,224;1,2,1,1,33,0,g,w,m2,d0,o0,u63,233;"
    "1,2,1,1,33,0,g,w,m2,d0,o0,u64,234;1,2,1,1,33,0,g,w,m2,d0,o0,u65,244;1,2,1,1,0,0,g,w,m2,d0,o0,u66,334;"
    "1,2,1,1,0,0,g,w,m2,d0,o0,u67,344";

TEST_CASE("Snapmaker mix helper true only for custom-entry grammar", "[spectrum_auto_graft]")
{
    REQUIRE(spectrum_mix_looks_like_snapmaker_custom_entries(k_palette26_snapmaker_excerpt_graft));
    REQUIRE(spectrum_mix_looks_like_snapmaker_custom_entries("1,2,1,1,50,0,g,w,m2,d0,o0,u29,12"));
    REQUIRE(spectrum_mix_looks_like_snapmaker_custom_entries("1,2,1,1,50,0,g,w,m2,d0,o0,12"));
    REQUIRE_FALSE(spectrum_mix_looks_like_snapmaker_custom_entries(""));
    REQUIRE_FALSE(spectrum_mix_looks_like_snapmaker_custom_entries("1,2,1,1,1"));

    REQUIRE(spectrum_keep_imported_filament_colours(k_palette26_snapmaker_excerpt_graft));
    REQUIRE(spectrum_keep_imported_filament_colours("1,2,1,1,50,0,g,w,m2,d0,o0,u29,12"));
    REQUIRE_FALSE(spectrum_keep_imported_filament_colours(""));
    REQUIRE_FALSE(spectrum_keep_imported_filament_colours("1,2,1,1,1"));
}

#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "libslic3r/PrinterAssetRef.hpp"

using namespace Slic3r;

// Printer asset references in bed_custom_model / bed_custom_texture: "orcacloud-asset/<plugin id>/<file>".

TEST_CASE("A printer asset reference parses into its plugin id and file name", "[PrinterAssetRef]")
{
    const auto ref = parse_printer_asset_ref("orcacloud-asset/3f2a6c1e-8d4b-4c2a-9e7f-1a2b3c4d5e6f/voron_24_350.stl");
    REQUIRE(ref.has_value());
    CHECK(ref->plugin_id == "3f2a6c1e-8d4b-4c2a-9e7f-1a2b3c4d5e6f");
    CHECK(ref->filename == "voron_24_350.stl");
}

TEST_CASE("make and parse round-trip a printer asset reference", "[PrinterAssetRef]")
{
    const std::string value = make_printer_asset_ref("my-pack", "Plate-A.svg");
    CHECK(value == "orcacloud-asset/my-pack/Plate-A.svg");
    const auto ref = parse_printer_asset_ref(value);
    REQUIRE(ref.has_value());
    CHECK(ref->plugin_id == "my-pack");
    CHECK(ref->filename == "Plate-A.svg");
}

TEST_CASE("Plain bed paths are not printer asset references", "[PrinterAssetRef]")
{
    CHECK_FALSE(parse_printer_asset_ref("").has_value());
    CHECK_FALSE(parse_printer_asset_ref("/Users/me/Downloads/bed.stl").has_value());
    CHECK_FALSE(parse_printer_asset_ref("C:\\Users\\me\\bed.png").has_value());
    CHECK_FALSE(parse_printer_asset_ref("bed.stl").has_value());
    // The prefix is case-sensitive, like every config key value.
    CHECK_FALSE(parse_printer_asset_ref("OrcaCloud-Asset/pack/bed.stl").has_value());
}

TEST_CASE("Malformed printer asset references are rejected", "[PrinterAssetRef]")
{
    for (const char* value : {
             "orcacloud-asset/",
             "orcacloud-asset/pack",
             "orcacloud-asset/pack/",
             "orcacloud-asset//bed.stl",
             "orcacloud-asset/pack/../bed.stl",
             "orcacloud-asset/pack/sub/bed.stl",
             "orcacloud-asset/../bed.stl",
             "orcacloud-asset/pack/..",
             "orcacloud-asset/pack/bed .stl",
             "orcacloud-asset/pack/bed:stream.stl",
             "orcacloud-asset/pa\\ck/bed.stl",
         }) {
        INFO(value);
        CHECK_FALSE(parse_printer_asset_ref(value).has_value());
    }
    CHECK(make_printer_asset_ref("pack", "../bed.stl").empty());
    CHECK(make_printer_asset_ref("", "bed.stl").empty());
    CHECK(make_printer_asset_ref("pack one", "bed.stl").empty());
}

TEST_CASE("Windows device names are not printer asset file names", "[PrinterAssetRef]")
{
    // Windows opens these, with any extension, as devices, so the reference would resolve everywhere else.
    for (const char* filename : {"con.stl", "NUL.png", "Com1.svg", "lpt9.stl", "aux.plate.png", "prn"}) {
        INFO(filename);
        CHECK(make_printer_asset_ref("pack", filename).empty());
        CHECK_FALSE(parse_printer_asset_ref(std::string("orcacloud-asset/pack/") + filename).has_value());
    }
    for (const char* filename : {"console.stl", "com10.stl", "my-con.png", "lpt.png"}) {
        INFO(filename);
        CHECK(parse_printer_asset_ref(std::string("orcacloud-asset/pack/") + filename).has_value());
    }
}

TEST_CASE("Bed files are sorted into models and textures by extension", "[PrinterAssetRef]")
{
    CHECK(is_printer_bed_model_file("bed.STL"));
    CHECK_FALSE(is_printer_bed_model_file("bed.png"));
    CHECK(is_printer_bed_texture_file("plate.png"));
    CHECK(is_printer_bed_texture_file("plate.Svg"));
    CHECK_FALSE(is_printer_bed_texture_file("plate.jpg"));
    CHECK_FALSE(is_printer_bed_texture_file("png"));
}

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>

#include "slic3r/Utils/CrealityPrint.hpp"
#include "slic3r/Utils/CrealityPrintAgent.hpp"

using namespace Slic3r;
using json = nlohmann::json;

namespace {

// boxsInfo as a K2 Plus reports it: box 0 is the external spool holder (type 1), the CFS
// units follow with their own ids; slot 2 of the first CFS has no spool.
const std::string k2_plus_boxes = R"({"boxsInfo": {"materialBoxs": [
    {"id": 0, "state": 1, "type": 1, "materials": [{"id": 0, "state": 1, "vendor": "Generic", "type": "PLA",
                                                    "name": "Generic PLA", "color": "#0ffffff"}]},
    {"id": 1, "state": 1, "type": 0, "materials": [
        {"id": 0, "state": 1, "vendor": "Creality", "type": "PLA", "name": "Hyper PLA", "color": "#0ff0000"},
        {"id": 1, "state": 2, "vendor": "Generic", "type": "PETG", "name": "Generic PETG", "color": "#0000000"},
        {"id": 2, "state": 0, "vendor": "", "type": "", "name": "", "color": ""},
        {"id": 3, "state": 1, "vendor": "Generic", "type": "PLA", "name": "Generic PLA", "color": "#000ff00"}]},
    {"id": 2, "state": 1, "type": 0, "materials": [
        {"id": 0, "state": 1, "vendor": "Generic", "type": "ABS", "name": "Generic ABS", "color": "#00000ff"}]}
]}})";

const std::string k2_library = R"({"retMaterials": [
    {"base": {"id": "00001", "brand": "Generic", "meterialType": "PLA", "name": "Generic PLA", "minTemp": 190, "maxTemp": 240},
     "kvParam": {"pressure_advance": "0.04"}},
    {"base": {"id": "00003", "brand": "Generic", "meterialType": "PETG", "name": "Generic PETG", "minTemp": 220, "maxTemp": 270}},
    {"base": {"id": "01001", "brand": "Creality", "meterialType": "PLA", "name": "Hyper PLA", "minTemp": 190, "maxTemp": 230}},
    {"base": {"id": "01004", "brand": "Creality", "meterialType": "PLA-CF", "name": "Hyper PLA-CF", "minTemp": 200, "maxTemp": 240}}
]})";

CrealityPrintAgent::CFSMaterial preset_material(const std::string& vendor, const std::string& type, const std::string& name)
{
    CrealityPrintAgent::CFSMaterial m;
    m.vendor        = vendor;
    m.filament_type = type;
    m.name          = name;
    m.color_hex     = "#1A2B3C";
    return m;
}

} // namespace

// The Device tab addresses a slot by the AMS index fetch_filament_info() published, which skips
// the external spool holder; modifyMaterial needs the printer's own box id.
TEST_CASE("A CFS slot is addressed by the printer's box id, empty slots included", "[CFS][Creality]")
{
    int  box_id = -1;
    json slot;
    REQUIRE(CrealityPrintAgent::find_cfs_material(k2_plus_boxes, 0, 2, box_id, slot));
    CHECK(box_id == 1);
    CHECK(slot.value("state", -1) == 0);

    REQUIRE(CrealityPrintAgent::find_cfs_material(k2_plus_boxes, 1, 0, box_id, slot));
    CHECK(box_id == 2);
    CHECK(slot.value("name", std::string()) == "Generic ABS");

    CHECK_FALSE(CrealityPrintAgent::find_cfs_material(k2_plus_boxes, 2, 0, box_id, slot));
    CHECK_FALSE(CrealityPrintAgent::find_cfs_material(k2_plus_boxes, 1, 3, box_id, slot));
    CHECK_FALSE(CrealityPrintAgent::find_cfs_material("", 0, 0, box_id, slot));
}

TEST_CASE("A slot write takes its identity from the printer's material library", "[CFS][Creality]")
{
    auto hyper = preset_material("Creality", "PLA", "Hyper PLA");
    REQUIRE(CrealityPrintAgent::apply_material_library(k2_library, hyper));
    CHECK(hyper.rfid == "01001");
    CHECK(hyper.min_temp == 190);
    CHECK(hyper.max_temp == 230);

    // A third party preset has no library entry of its own: the generic of the same type.
    auto esun = preset_material("eSUN", "PETG", "eSUN PETG");
    REQUIRE(CrealityPrintAgent::apply_material_library(k2_library, esun));
    CHECK(esun.rfid == "00003");
    CHECK(esun.vendor == "Generic");
    CHECK(esun.name == "Generic PETG");

    // Temperatures and pressure advance from the preset win over the library's.
    auto tuned     = preset_material("Generic", "PLA", "Generic PLA");
    tuned.min_temp = 205;
    tuned.max_temp = 225;
    REQUIRE(CrealityPrintAgent::apply_material_library(k2_library, tuned));
    CHECK(tuned.rfid == "00001");
    CHECK(tuned.min_temp == 205);
    CHECK(tuned.max_temp == 225);
    CHECK(tuned.pressure_advance == 0.04);

    auto unknown = preset_material("Generic", "TPU", "Generic TPU");
    CHECK_FALSE(CrealityPrintAgent::apply_material_library(k2_library, unknown));
    CHECK(unknown.rfid.empty());
}

TEST_CASE("modifyMaterial uses Creality's colour spelling and clears with empty fields", "[CFS][Creality]")
{
    auto m     = preset_material("Creality", "PLA", "Hyper PLA");
    m.rfid     = "01001";
    m.min_temp = 190;
    m.max_temp = 230;
    const json write = CrealityPrintAgent::build_modify_material(1, 2, m);
    CHECK(write["boxId"] == 1);
    CHECK(write["id"] == 2);
    CHECK(write["color"] == "#01a2b3c");
    CHECK(write["rfid"] == "01001");
    CHECK(write["minTemp"].get<double>() > 189.9);

    const json clear = CrealityPrintAgent::build_modify_material(1, 2, CrealityPrintAgent::CFSMaterial());
    CHECK(clear["type"] == "");
    CHECK(clear["color"] == "");
    CHECK(clear["minTemp"] == 0);
}

TEST_CASE("A slot write is verified against what the printer reports back", "[CFS][Creality]")
{
    int  box_id = 0;
    json slot;
    REQUIRE(CrealityPrintAgent::find_cfs_material(k2_plus_boxes, 0, 0, box_id, slot));

    auto written      = preset_material("Creality", "PLA", "hyper pla");
    written.color_hex = "#FF0000";
    CHECK(CrealityPrintAgent::material_matches(slot, written));

    written.color_hex = "#00FF00";
    CHECK_FALSE(CrealityPrintAgent::material_matches(slot, written));
    CHECK_FALSE(CrealityPrintAgent::material_matches(slot, CrealityPrintAgent::CFSMaterial()));

    REQUIRE(CrealityPrintAgent::find_cfs_material(k2_plus_boxes, 0, 2, box_id, slot));
    CHECK(CrealityPrintAgent::material_matches(slot, CrealityPrintAgent::CFSMaterial()));
}

// Slot writes are limited to the K2 family; K1-family CFS, the Hi and the mini keep "not available".
TEST_CASE("Only the K2 family takes CFS slot writes", "[CFS][Creality]")
{
    for (const char* model : {"F008", "F012", "F021"}) {
        CHECK(CrealityPrint::model_supports_cfs_write(model));
        CHECK(CrealityPrint::model_supports_multi_color(model));
    }
    for (const char* model : {"K1", "K1 SE", "K1C", "K1_CFS-C", "F022", "F001", ""})
        CHECK_FALSE(CrealityPrint::model_supports_cfs_write(model));
}

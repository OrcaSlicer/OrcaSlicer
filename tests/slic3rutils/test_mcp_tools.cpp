// McpTools.hpp reaches wx's MSW headers through GLCanvas3D.hpp after OCCT has hidden the USER and GDI
// declarations they need; the Windows headers first keep them, as in test_plugin_host_api.cpp.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
#endif

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "automation/automation_test_utils.hpp"
#include "mcp_test_utils.hpp"
#include "python_test_support.hpp"

#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/McpPluginCaller.hpp"
#include "slic3r/GUI/Mcp/WindowControl.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/plugin/host/PluginHostTools.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"

#include <nlohmann/json.hpp>
#include <wx/gdicmn.h>
#include <wx/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
using Slic3r::GUI::CommandCatalog;
using Slic3r::GUI::CommandSource;
using Slic3r::GUI::NativeCommand;
using Slic3r::GUI::Mcp::Test::catalog_with_cad;
using Slic3r::GUI::Mcp::Test::OneCommandSource;
namespace NativeCommands = Slic3r::GUI::NativeCommands;

namespace {

// A Speed Dial action that runs nothing, with the id `prefix`:`name`:orca, as the registry's own are made.
class IdleAction final : public Slic3r::GUI::AppAction
{
public:
    IdleAction(const char* prefix, const char* name) : AppAction(prefix, name, "orca", "OrcaSlicer") {}

    Slic3r::GUI::AppActionRunResult run(const json& /*args*/) const override { return {}; }
};

} // namespace

TEST_CASE("an integer argument that no int holds is refused, not narrowed into another value", "[Mcp]")
{
    CHECK(arg_int({{"plate", 2}}, "plate", -1) == 2);
    CHECK(arg_int({{"plate", 2.0}}, "plate", -1) == 2);
    CHECK(arg_int(json::object(), "plate", -1) == -1);
    CHECK(arg_int({{"plate", "2"}}, "plate", -1) == -1);
    CHECK(arg_int({{"plate", std::numeric_limits<int>::min()}}, "plate", 0) == std::numeric_limits<int>::min());
    // get<int>() would make plate 0, or instance 0, of these, which the command then accepts.
    for (const json& wide : {json(std::int64_t(4294967296)), json(std::uint64_t(18446744073709551615ull)), json(-4294967297.0),
                             json(9.0e18), json(std::int64_t(std::numeric_limits<int>::max()) + 1)}) {
        INFO(wide.dump());
        try {
            arg_int({{"instance", wide}}, "instance", 0);
            FAIL("the argument was narrowed");
        } catch (const ToolFailure& f) {
            CHECK_THAT(std::string(f.what()), Catch::Matchers::ContainsSubstring("instance " + wide.dump() + " is out of range"));
            CHECK(f.details.value("code", std::string()) == "invalid_argument");
        }
    }
}

TEST_CASE("setting values serialize the way their options read them back", "[Mcp]")
{
    CHECK(to_config_string(json(3)) == "3");
    CHECK(to_config_string(json(0.2)) == "0.2");
    CHECK(to_config_string(json(true)) == "1");
    CHECK(to_config_string(json("15%")) == "15%");
    CHECK(to_config_string(json::array({0.4, 0.6}), "nozzle_diameter") == "0.4,0.6");

    // A list of strings whose entries contain the separators survives the round trip.
    const std::vector<std::string> scripts{"/usr/bin/fix --mode=a,b", "second; with a semicolon"};
    Slic3r::ConfigOptionStrings    parsed;
    parsed.deserialize(to_config_string(json(scripts), "post_process"));
    CHECK(parsed.values == scripts);

    // Points as [x, y] pairs, or already in OrcaSlicer's "XxY" form.
    const json area = json::array({json::array({0, 0}), json::array({220.5, 0}), "220x220", json::array({0, 220})});
    Slic3r::ConfigOptionPoints points;
    REQUIRE(points.deserialize(to_config_string(area, "printable_area")));
    REQUIRE(points.values.size() == 4);
    CHECK(points.values[1] == Slic3r::Vec2d(220.5, 0));
    CHECK(points.values[2] == Slic3r::Vec2d(220, 220));
    CHECK(to_config_string(json::array({10, 20}), "bed_mesh_min") == "10x20");
    // A string is already in OrcaSlicer's format, list options included.
    CHECK(to_config_string(json("a.py;b.py"), "post_process") == "a.py;b.py");

    CHECK_THROWS_AS(to_config_string(json::object()), ToolFailure);
}

TEST_CASE("action_run refuses arguments for an action that takes none", "[Mcp][SpeedDial]")
{
    // A plate's, a setting's, a recent project's and a plugin's action run what they are, whatever args says.
    for (const char* prefix : {"orca_plate_goto", "orca_setting", "orca_recent_project", "plugin_script_action", "plugin_page_action"}) {
        INFO(prefix);
        const IdleAction action(prefix, "2");
        CHECK_THAT(action_args_error(action, {{"plate", 2}}), Catch::Matchers::ContainsSubstring("takes no arguments"));
        CHECK(action_args_error(action, json::object()).empty());
    }
    // A command's own action outside the Design tab takes none, as an item with no arguments does: the refusal names the
    // command that takes them.
    const NativeCommand* filament_delete = NativeCommands::find("filament_delete");
    REQUIRE(filament_delete != nullptr);
    const std::unique_ptr<Slic3r::GUI::AppAction> own = NativeCommands::make_action(*filament_delete);
    CHECK(own->input_schema.at("properties").empty());
    CHECK(action_args_error(*own, json::object()).empty());
    const std::string refused = action_args_error(*own, {{"filament_slot", 2}});
    CHECK_THAT(refused, Catch::Matchers::ContainsSubstring("filament_delete"));
    // A menu item's action takes no arguments, or exactly its own; an item's former arguments are refused, naming the command.
    const auto [export_command, export_gcode] = find_command_action(NativeCommands::instance(), "export_gcode");
    REQUIRE(export_gcode != nullptr);
    const std::unique_ptr<Slic3r::GUI::AppAction> gcode = NativeCommands::make_action(*export_command, *export_gcode);
    CHECK(action_args_error(*gcode, json::object()).empty());
    CHECK(action_args_error(*gcode, {{"format", "gcode"}}).empty());
    const std::string with_path = action_args_error(*gcode, {{"path", "x.gcode"}});
    CHECK_FALSE(with_path.empty());
    CHECK_THAT(with_path, Catch::Matchers::ContainsSubstring("call project_export"));
#ifdef SLIC3R_CAD
    // A Design command's own action takes the command's arguments, which the command checks.
    CommandCatalog design;
    NativeCommands::register_all_commands(design, {true});
    design.seal();
    const NativeCommand* shell = design.find("solid_shell");
    REQUIRE(shell != nullptr);
    REQUIRE(shell->has_own_action);
    const std::unique_ptr<Slic3r::GUI::AppAction> design_action = NativeCommands::make_action(*shell);
    CHECK(design_action->input_schema == shell->input_schema);
    CHECK(action_args_error(*design_action, {{"thickness", 2}}).empty());
#endif
}

TEST_CASE("what action_run journals for an action of fixed arguments is a call it takes", "[Mcp][SpeedDial]")
{
    // Every command's action: its journal step's arguments are the action's own, or the value an action with an input
    // collects, and action_run takes them, so a replay of the step is never refused.
    const auto check_actions = [](const CommandCatalog& catalog) {
        std::size_t checked = 0;
        for (const NativeCommand& command : catalog.commands()) {
            std::vector<std::unique_ptr<Slic3r::GUI::AppAction>> actions;
            if (command.has_own_action)
                actions.push_back(NativeCommands::make_action(command));
            if (command.actions.is_array())
                for (const json& item : command.actions)
                    actions.push_back(NativeCommands::make_action(command, item));
            for (const std::unique_ptr<Slic3r::GUI::AppAction>& action : actions) {
                INFO(action->id());
                const json given    = action->input.empty() ? json::object() : json{{action->input, 1}};
                const json recorded = action_journal_args(*action, given);
                if (action->preset.is_null()) {
                    // A Design command's own action takes its command's arguments: the command's rewrite stands.
                    CHECK(recorded.is_null());
                    continue;
                }
                ++checked;
                CHECK(recorded == (action->input.empty() ? action->preset : given));
                CHECK(action_args_error(*action, recorded).empty());
            }
        }
        return checked;
    };
    // The 130 Speed Dial entries outside the Design tab are commands' actions: eight own and 122 items.
    // Every action of the Speed Dial is checked: none outside the Design tab takes its command's arguments.
    const std::size_t speed_dial = NativeCommands::speed_dial_actions(NativeCommands::instance()).size();
    CHECK(check_actions(NativeCommands::instance()) == speed_dial);

    const auto recorded_for = [](const CommandCatalog& catalog, const char* key, const json& given) {
        const auto [command, action] = find_command_action(catalog, key);
        REQUIRE(command != nullptr);
        return action_journal_args(
            *(action != nullptr ? NativeCommands::make_action(*command, *action) : NativeCommands::make_action(*command)), given);
    };
    CHECK(recorded_for(NativeCommands::instance(), "obj_arrange", json::object()) == json::object());
    CHECK(recorded_for(NativeCommands::instance(), "go_to_layer", {{"percent", 50}}) == json{{"percent", 50}});
#ifdef SLIC3R_CAD
    CommandCatalog design;
    NativeCommands::register_all_commands(design, {true});
    design.seal();
    CHECK(check_actions(design) > speed_dial);
    CHECK(recorded_for(design, "sk_array_polar", json::object()) == json{{"type", "circular"}});
#endif
}

TEST_CASE("a call with an action's arguments asks, and one more argument runs", "[Mcp]")
{
    // The commands whose actions ask, as the design's table "When a call does what a menu item does" lists them: called with
    // exactly an action's arguments a command is its person's form, and with silent beside them its silent form.
    const CommandCatalog& catalog = NativeCommands::instance();
    for (const char* key : {"project_open", "project_import", "project_save", "project_export", "object_add_primitive", "object_rename",
                            "plate_rename", "object_set_instances", "calibration_generate_test", "preset_import", "preset_export",
                            "plugin_install_local", "project_new", "recording_start", "recording_stop"}) {
        const NativeCommand* command = catalog.find(key);
        REQUIRE(command != nullptr);
        const std::vector<json> forms = person_forms(*command);
        REQUIRE_FALSE(forms.empty());
        for (const json& form : forms) {
            INFO(key << " " << form.dump());
            CHECK(person_form(*command, form));
            json silent      = form;
            silent["silent"] = true;
            CHECK_FALSE(person_form(*command, silent));
        }
    }
    // The actions each is called with, by the table: their arguments are a person's form.
    for (const auto& [key, args] : std::vector<std::pair<const char*, json>>{
             {"project_import", {{"archive", true}}},
             {"project_save", {{"save_as", true}}},
             {"project_export", {{"format", "sliced_3mf"}, {"plates", "all"}}},
             {"project_export", {{"format", "stl"}, {"per_object", true}}},
             {"object_add_primitive", {{"shape", "torus"}}},
             {"calibration_generate_test", {{"test", "vfa"}}}}) {
        INFO(key << " " << args.dump());
        CHECK(person_form(*catalog.find(key), args));
    }
    // Where the table's silent form is silent: true, the schema takes it beside the action's arguments, and the call is
    // valid: the default size of a primitive, a test with its dialog's defaults.
    for (const auto& [key, args] : std::vector<std::pair<const char*, json>>{{"object_add_primitive", {{"shape", "cube"}}},
                                                                               {"calibration_generate_test", {{"test", "temperature"}}}}) {
        INFO(key);
        const NativeCommand* command = catalog.find(key);
        REQUIRE(command != nullptr);
        CHECK(command->input_schema.at("properties").contains("silent"));
        json silent      = args;
        silent["silent"] = true;
        CHECK(argument_error(*command, silent).empty());
        // Neither form needs more than the variant; with nothing at all the silent form is refused for it.
        json action = args;
        CHECK(argument_error(*command, action).empty());
        json none = json::object();
        CHECK_FALSE(argument_error(*command, none).empty());
    }
    // silent says what a person's form of the command is: a call with no arguments where that is the only one, else a
    // call with an action's arguments.
    const auto silent_text = [&catalog](const char* key) {
        return catalog.find(key)->input_schema.at("properties").at("silent").at("description").get<std::string>();
    };
    CHECK_THAT(silent_text("project_new"), Catch::Matchers::ContainsSubstring("no arguments"));
    CHECK_THAT(silent_text("project_export"), !Catch::Matchers::ContainsSubstring("no arguments"));
    CHECK_THAT(silent_text("project_export"), Catch::Matchers::ContainsSubstring("Speed Dial action"));
    CHECK_THAT(silent_text("calibration_generate_test"), Catch::Matchers::ContainsSubstring("Speed Dial action"));
}

TEST_CASE("an action that collects a value takes exactly that argument", "[Mcp][SpeedDial]")
{
    json given; // what the command's function was last called with
    NativeCommand view = NativeCommands::ui_command(
        "view_it", "View it", "View",
        object_schema({{"tab", prop("string", "A tab.")},
                       {"camera", prop("string", "A camera.")},
                       {"layer_percent", {{"type", "number"}, {"minimum", 0}, {"maximum", 100}}}}),
        [&given](const json& args) {
            given = args;
            return Slic3r::GUI::AppActionRunResult{Slic3r::GUI::AppActionRunResult::Level::Succeeded};
        });
    view.actions = json::array({NativeCommands::action("view_it_tab", "Go to tab", "Commands", "", json::object(), "Goes to a tab.", "tab"),
                                NativeCommands::action("view_it_layer", "Go to layer", "Commands", "", json::object(), "Goes to a layer.",
                                                       "percent", "layer_percent"),
                                NativeCommands::action("view_it_x", "Along X", "View", "", {{"axis", "x"}}, "Along X.")});

    // Go to tab's value is the command's argument of the same name.
    const std::unique_ptr<Slic3r::GUI::AppAction> tab = NativeCommands::make_action(view, view.actions[0]);
    CHECK(tab->input == "tab");
    CHECK(tab->group_en == "Commands");
    REQUIRE(tab->input_schema["properties"].size() == 1);
    CHECK(tab->input_schema["properties"]["tab"]["type"] == "string");
    CHECK(tab->input_schema["required"] == json::array({"tab"}));
    CHECK_FALSE(action_args_error(*tab, json::object()).empty());
    CHECK_FALSE(action_args_error(*tab, {{"tab", "preview"}, {"camera", "top"}}).empty());
    CHECK(action_args_error(*tab, {{"tab", "preview"}}).empty());
    tab->run({{"tab", "preview"}});
    CHECK(given == json{{"tab", "preview"}});

    // Go to layer's value goes to the command's argument the action names.
    const std::unique_ptr<Slic3r::GUI::AppAction> layer = NativeCommands::make_action(view, view.actions[1]);
    CHECK(layer->input == "percent");
    REQUIRE(layer->input_schema["properties"].size() == 1);
    CHECK(layer->input_schema["properties"]["percent"]["type"] == "number");
    CHECK(layer->input_schema["required"] == json::array({"percent"}));
    layer->run({{"percent", 50}});
    CHECK(given == json{{"layer_percent", 50}});
    CHECK(action_call_args(&view.actions[1], {{"percent", 50}}) == json{{"layer_percent", 50}});

    // An item made without a group has none, and is in its command's section.
    const json grouped_with_command = NativeCommands::action("view_it_y", "Along Y", "", "", {{"axis", "y"}}, "Along Y.");
    CHECK_FALSE(grouped_with_command.contains("group"));
    CHECK(NativeCommands::make_action(view, grouped_with_command)->group_en == "View");

    // An action that collects nothing runs its command with its own arguments, and a command's own action with the call's.
    CHECK(action_call_args(&view.actions[2], json::object()) == json{{"axis", "x"}});
    CHECK(action_call_args(nullptr, {{"plate", 2}}) == json{{"plate", 2}});
    CHECK(action_call_args(nullptr, json::object()) == json::object());
}

TEST_CASE("Go to tab takes the Device tab as device and any other page by its id", "[Mcp][SpeedDial]")
{
    // The main window's pages as the Speed Dial's tab picker lists them, a plugin's among them.
    const std::vector<std::string> pages{"home", "design", "prepare", "preview", "monitor", "monitor_web", "multi_device", "project",
                                         "calibration", "a_plugin_page"};
    CHECK(view_tab_page("device", pages) == std::optional<std::string>("monitor"));
    CHECK(view_tab_page("preview", pages) == std::optional<std::string>("preview"));
    // The Device tab's page is reached by its word alone.
    CHECK_FALSE(view_tab_page("monitor", pages).has_value());
    // A page no word names, by its id.
    CHECK(view_tab_page("monitor_web", pages) == std::optional<std::string>("monitor_web"));
    CHECK(view_tab_page("a_plugin_page", pages) == std::optional<std::string>("a_plugin_page"));
    // An id no page has, and a word whose page the window does not show.
    CHECK_FALSE(view_tab_page("no_such_page", pages).has_value());
    CHECK_FALSE(view_tab_page("device", {"home", "prepare"}).has_value());

    // The tab picker sends the page's id, which the action gives view_set as the page's word.
    const auto [command, action] = find_command_action(NativeCommands::instance(), "go_to_tab");
    REQUIRE(command != nullptr);
    REQUIRE(action != nullptr);
    CHECK(command->key == "view_set");
    CHECK(action_call_args(action, {{"tab", "monitor"}}) == json{{"tab", "device"}});
    CHECK(action_call_args(action, {{"tab", "preview"}}) == json{{"tab", "preview"}});
    CHECK(action_call_args(action, {{"tab", "a_plugin_page"}}) == json{{"tab", "a_plugin_page"}});
    // view_set takes a plugin's page by its id.
    json plugin{{"tab", "a_plugin_page"}};
    CHECK(argument_error(*command, plugin).empty());
}

TEST_CASE("preset_export takes the folder of its dialog and has no silent form", "[Mcp]")
{
    const NativeCommand* export_ = NativeCommands::find("preset_export");
    REQUIRE(export_ != nullptr);
    CHECK(export_->input_schema.at("properties").size() == 1);
    json folder{{"path", "out"}};
    CHECK(argument_error(*export_, folder).empty());
    json silent{{"silent", true}};
    CHECK_THAT(argument_error(*export_, silent), Catch::Matchers::ContainsSubstring("silent"));
}

TEST_CASE("the filament commands take the filaments they mix or open their dialog without them, and the slot a deleted one merges into",
          "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    const NativeCommand* mixed = NativeCommands::find("filament_add_mixed");
    REQUIRE(mixed != nullptr);
    CHECK(mixed->listed);
    CHECK_FALSE(mixed->has_own_action);
    CHECK(mixed->actions.is_null());
    // Without components and ratios it opens the Add Mixed Filament dialog; the operation refuses one without the other.
    CHECK_FALSE(mixed->input_schema.contains("required"));
    json dialog = json::object();
    CHECK(argument_error(*mixed, dialog).empty());
    json half{{"components", json::array({1, 2})}, {"ratios", json::array({50, 50})}};
    CHECK(argument_error(*mixed, half).empty());
    json thirds{{"components", json::array({1, 2, 3})}, {"ratios", json::array({33, 33, 34})}};
    CHECK(argument_error(*mixed, thirds).empty());
    // Slots are numbered from 1, and a ratio is a whole percentage above 0, as the Add Mixed Filament dialog sets it.
    json fraction{{"components", json::array({1, 2})}, {"ratios", json::array({12.5, 87.5})}};
    CHECK_THAT(argument_error(*mixed, fraction), ContainsSubstring("ratios"));
    json slot_zero{{"components", json::array({0, 2})}, {"ratios", json::array({50, 50})}};
    CHECK_THAT(argument_error(*mixed, slot_zero), ContainsSubstring("components"));
    json no_share{{"components", json::array({1, 2})}, {"ratios", json::array({0, 100})}};
    CHECK_THAT(argument_error(*mixed, no_share), ContainsSubstring("ratios"));

    // Edit Mixed Filament names the mix, and takes what Add Mixed Filament takes, or nothing more for its dialog.
    const NativeCommand* edit = NativeCommands::find("filament_edit_mixed");
    REQUIRE(edit != nullptr);
    CHECK(edit->input_schema["required"] == json::array({"filament_slot"}));
    json edit_dialog{{"filament_slot", 3}};
    CHECK(argument_error(*edit, edit_dialog).empty());
    json edited{{"filament_slot", 3}, {"components", json::array({1, 2})}, {"ratios", json::array({70, 30})}};
    CHECK(argument_error(*edit, edited).empty());
    json no_slot{{"components", json::array({1, 2})}, {"ratios", json::array({70, 30})}};
    CHECK_THAT(argument_error(*edit, no_slot), ContainsSubstring("filament_slot"));

    // Delete Filament Slot keeps an action of its own, which deletes the last physical slot, and takes the slot to merge into.
    const NativeCommand* remove = NativeCommands::find("filament_delete");
    REQUIRE(remove != nullptr);
    CHECK(remove->has_own_action);
    CHECK_FALSE(remove->input_schema.contains("required"));
    json merge{{"filament_slot", 2}, {"merge_into", 1}};
    CHECK(argument_error(*remove, merge).empty());
    json merge_zero{{"filament_slot", 2}, {"merge_into", 0}};
    CHECK_THAT(argument_error(*remove, merge_zero), ContainsSubstring("merge_into"));
    // break_mix answers Merge with's question ahead, for a merge into a mixed filament that mixes the slot.
    json break_mix{{"filament_slot", 1}, {"merge_into", 3}, {"break_mix", true}};
    CHECK(argument_error(*remove, break_mix).empty());
}

TEST_CASE("project_export writes six formats, and project_import takes the entries of a ZIP archive", "[Mcp]")
{
    const NativeCommand* export_ = NativeCommands::find("project_export");
    REQUIRE(export_ != nullptr);
    std::set<std::string> formats;
    for (const json& format : export_->input_schema.at("properties").at("format").at("enum"))
        formats.insert(format.get<std::string>());
    CHECK(formats == std::set<std::string>{"gcode", "sliced_3mf", "3mf", "stl", "drc", "toolpaths_obj"});
    // Some objects, by index or by id, as the object commands name them.
    json some{{"format", "stl"}, {"path", "/out/two.stl"}, {"objects", json::array({0, 2})}};
    CHECK(argument_error(*export_, some).empty());
    json drc{{"format", "drc"}, {"path", "/out/parts"}, {"per_object", true}, {"objects", json::array({1})}};
    CHECK(argument_error(*export_, drc).empty());

    const NativeCommand* import_ = NativeCommands::find("project_import");
    REQUIRE(import_ != nullptr);
    json zip{{"paths", json::array({"/models/parts.zip"})}, {"entries", json::array({"cube.stl", "parts/cylinder.obj"})}};
    CHECK(argument_error(*import_, zip).empty());
    CHECK(import_->input_schema.at("properties").at("entries").at("items") == json{{"type", "string"}});
}

TEST_CASE("action_run says where an action it cannot find is", "[Mcp][SpeedDial]")
{
    // The text and the code of action_run's refusal of `id`.
    const auto refusal = [](const std::string& id, const CommandCatalog& catalog, bool cad_on) {
        const ToolFailure refused = unknown_action(id, catalog, cad_on);
        return std::make_pair(std::string(refused.what()), refused.details.value("code", std::string()));
    };
    // An id no action has is the call's to correct.
    const auto [typo, typo_code] = refusal("obj_arange", NativeCommands::instance(), true);
    CHECK_THAT(typo, Catch::Matchers::ContainsSubstring("action_list"));
    CHECK(typo_code == "invalid_argument");
#ifdef SLIC3R_CAD
    // A Design tab action, by its id or its key, while it is not shown: in a run with the CAD feature on at startup
    // tool_call runs its command from any tab (wrong_mode, the id being right), and in one with it off there is no such
    // command (design_unavailable).
    for (const bool cad : {true, false}) {
        CommandCatalog catalog;
        NativeCommands::register_all_commands(catalog, {cad});
        catalog.seal();
        for (const std::string id : {"orca_command:solid_shell:orca", "solid_shell"}) {
            INFO(id << (cad ? ", the CAD feature on" : ", the CAD feature off"));
            const auto [why, code] = refusal(id, catalog, cad);
            if (cad) {
                CHECK_THAT(why, Catch::Matchers::ContainsSubstring("while the Design tab is shown"));
                CHECK_THAT(why, Catch::Matchers::ContainsSubstring("tool_call {\"tool\": \"solid_shell\"}"));
                CHECK(code == "wrong_mode");
            } else {
                CHECK(why == id + " is a Design tab action, and the CAD feature was off when OrcaSlicer started: a person "
                                  "turns it on in Preferences > Features and restarts OrcaSlicer, or starts it with "
                                  "--cad");
                CHECK(code == "design_unavailable");
            }
        }
        // A folded item's action names the call of its command that runs it; with the feature off at startup, there is
        // neither, as for any other action.
        const std::string folded = "orca_command:sk_array:orca";
        INFO(folded << (cad ? ", the CAD feature on" : ", the CAD feature off"));
        if (cad)
            CHECK_THAT(refusal(folded, catalog, cad).first,
                       Catch::Matchers::ContainsSubstring("tool_call {\"tool\": \"sketch_pattern\", \"args\": {\"type\": \"linear\"}}"));
        else
            CHECK(refusal(folded, catalog, cad).first == folded + " is a Design tab action, and the CAD feature was off when "
                                                                  "OrcaSlicer started: a person turns it on in Preferences > "
                                                                  "Features and restarts OrcaSlicer, or starts it with --cad");
        // A Design command with no Speed Dial action is refused as any unknown action is.
        CHECK_THAT(refusal("orca_command:design_describe:orca", catalog, cad).first, Catch::Matchers::ContainsSubstring("action_list"));
    }
    // The CAD feature turned off while the app runs drops every Design action, though their commands stay in the
    // catalog: the action is right, and a person turns the feature back on.
    CommandCatalog with_cad;
    NativeCommands::register_all_commands(with_cad, {true});
    with_cad.seal();
    const auto [off_now, off_now_code] = refusal("solid_shell", with_cad, false);
    CHECK_THAT(off_now, Catch::Matchers::ContainsSubstring("the CAD feature is off"));
    CHECK(off_now_code == "design_unavailable");
#endif
}

TEST_CASE("a plate the project lacks is refused by its number, with the plates the project has", "[Mcp]")
{
    CHECK(no_such_plate(5, 3) == "Plate 5 does not exist (the project has plates 0..2)");
    CHECK(no_such_plate(1, 1) == "Plate 1 does not exist (the project has plate 0 only)");
}

TEST_CASE("a listing summary is a documentation's first sentence on one line", "[Mcp]")
{
    CHECK(first_sentence("Return the mesh.\nIt is a copy.") == "Return the mesh.");
    CHECK(first_sentence("A box, e.g. the bed's. More.") == "A box, e.g. the bed's.");
    CHECK(first_sentence("Version 2.5 of the\nformat") == "Version 2.5 of the format");
    CHECK(first_sentence("") == "");
}

TEST_CASE("opening a project and importing models are two commands", "[Mcp]")
{
    const NativeCommand* open    = NativeCommands::find("project_open");
    const NativeCommand* import_ = NativeCommands::find("project_import");
    REQUIRE(open != nullptr);
    REQUIRE(import_ != nullptr);
    CHECK(open->listed);
    CHECK(import_->listed);
    json as_project{{"path", "a.3mf"}, {"as", "project"}};
    CHECK_THAT(argument_error(*open, as_project), Catch::Matchers::ContainsSubstring("unknown argument as"));
    json models{{"paths", {"a.stl"}}};
    CHECK(argument_error(*import_, models).empty());
    CHECK_FALSE(open->input_schema["properties"].contains("paths"));
    CHECK_FALSE(import_->input_schema["properties"].contains("as"));
}

TEST_CASE("the Python API reference covers the orca package with constructors and named instances", "[Mcp][Python]")
{
    (void) import_orca_module();
    const json                  reference = python_api_reference();
    std::map<std::string, json> by_name;
    for (const json& entry : reference)
        by_name[entry["name"].get<std::string>()] = entry;

    // The plugin classes and pages beside orca.host.
    CHECK(by_name.count("orca.pages") == 1);
    CHECK(by_name.count("orca.script.ScriptPluginCapabilityBase") == 1);
    // A class's signature is its constructors, and its documentation goes on with what they say.
    REQUIRE(by_name.count("orca.host.Point") == 1);
    CHECK(by_name["orca.host.Point"]["signature"].get<std::string>().rfind("Point(x: ", 0) == 0);
    REQUIRE(by_name.count("orca.host.ExPolygon") == 1);
    CHECK(by_name["orca.host.ExPolygon"]["doc"].get<std::string>().find("Orientation is normalized") != std::string::npos);
    // A constant, which has no docstring, points at the documentation that says what it means.
    REQUIRE(by_name.count("orca.host.ui.PD_CAN_ABORT") == 1);
    const json& abort = by_name["orca.host.ui.PD_CAN_ABORT"];
    CHECK(abort["kind"] == "constant");
    CHECK(abort["signature"].get<std::string>().rfind("PD_CAN_ABORT = ", 0) == 0);
    CHECK(abort["doc"].get<std::string>().find("orca.host.ui.create_progress_dialog") != std::string::npos);
    // An instance a module offers by name.
    REQUIRE(by_name.count("orca.host.preset.process") == 1);
    CHECK(by_name["orca.host.preset.process"]["kind"] == "attribute");
    // An enum's values are in its documentation, also where a module exports them.
    CHECK(by_name.count("orca.host.PresetType.Print") == 0);
    CHECK(by_name.count("orca.Analysis") == 0);
    // pybind11's signature lines are the signature, not documentation.
    REQUIRE(by_name.count("orca.host.TriangleMesh.face_normals") == 1);
    const json& normals = by_name["orca.host.TriangleMesh.face_normals"];
    CHECK(normals["signature"].get<std::string>().rfind("face_normals(self)", 0) == 0);
    CHECK(normals["doc"].get<std::string>().find("face_normals(") == std::string::npos);
    // An overloaded method's signature is its overloads, not pybind11's name(*args, **kwargs) before them.
    REQUIRE(by_name.count("orca.host.preset.PresetEditor.begin_update") == 1);
    const std::string overloads = by_name["orca.host.preset.PresetEditor.begin_update"]["signature"];
    CHECK(overloads.find("*args") == std::string::npos);
    CHECK(overloads.find("slot_number") != std::string::npos);
    // A static method has its own signature and documentation, not the staticmethod type's.
    REQUIRE(by_name.count("orca.ExecutionResult.success") == 1);
    const json& success = by_name["orca.ExecutionResult.success"];
    CHECK(success["signature"].get<std::string>().rfind("success(message: str", 0) == 0);
    CHECK(success["doc"].get<std::string>().find("Success result") != std::string::npos);
    // No required parameter follows one with a default, which Python could not declare; the handler may
    // still be passed by keyword alone.
    REQUIRE(by_name.count("orca.register_operation") == 1);
    CHECK(by_name["orca.register_operation"]["signature"].get<std::string>().find("schema: object = None, handler: object = None") !=
          std::string::npos);
}

TEST_CASE("script_list_api lists a module or class at a time and script_describe_api gives one entry", "[Mcp][Python]")
{
    (void) import_orca_module();
    const json reference = python_api_reference();
    const auto names_in  = [&reference](const std::string& within) {
        std::set<std::string> names;
        for (const json& item : python_api_children(reference, within))
            names.insert(item["name"].get<std::string>());
        return names;
    };
    // The details of the refusal `call` throws, or null when it throws none.
    const auto refusal = [](const std::function<void()>& call) -> json {
        try {
            call();
        } catch (const ToolFailure& refused) {
            return refused.details;
        }
        return nullptr;
    };

    // A class's methods and properties, each with its signature and the first sentence of its documentation.
    const std::set<std::string> mesh = names_in("orca.host.TriangleMesh");
    CHECK(mesh.count("orca.host.TriangleMesh.face_normals") == 1);
    const json items = python_api_children(reference, "orca.host.TriangleMesh");
    const auto normals = std::find_if(items.begin(), items.end(),
                                      [](const json& item) { return item["name"] == "orca.host.TriangleMesh.face_normals"; });
    REQUIRE(normals != items.end());
    const json entry = python_api_entry(reference, "orca.host.TriangleMesh.face_normals");
    CHECK(*normals == json{{"name", "orca.host.TriangleMesh.face_normals"}, {"kind", entry["kind"]}, {"signature", entry["signature"]},
                           {"summary", first_sentence(entry["doc"].get<std::string>())}});
    // Only what is directly inside: orca lists its modules and classes, not what they hold.
    const std::set<std::string> root = names_in(kPythonApiRoot);
    CHECK(root.count("orca.host") == 1);
    CHECK(root.count("orca.pages") == 1);
    CHECK(root.count("orca.host.TriangleMesh") == 0);
    CHECK(names_in("orca.host").count("orca.host.TriangleMesh") == 1);
    // The listing a call without within gives is orca's.
    const NativeCommand* list = NativeCommands::find("script_list_api");
    REQUIRE(list != nullptr);
    CHECK(list->input_schema.at("properties").at("within").at("default") == json(kPythonApiRoot));

    // A within that is no module or class, a function's name among them, is refused with the package's modules.
    for (const std::string within : {"orca.nothing", "orca.register_operation"}) {
        INFO(within);
        const json refused = refusal([&] { (void) python_api_children(reference, within); });
        REQUIRE(refused.is_object());
        CHECK(refused.value("code", std::string()) == "invalid_argument");
        const json& modules = refused.at("modules");
        CHECK(std::find(modules.begin(), modules.end(), json("orca.host")) != modules.end());
        CHECK(std::find(modules.begin(), modules.end(), json("orca.host.TriangleMesh")) == modules.end());
    }

    // script_describe_api gives the whole entry, its documentation whole, and a name no entry has its three closest.
    CHECK(entry["doc"].get<std::string>().size() >= (*normals)["summary"].get<std::string>().size());
    CHECK(entry.contains("doc"));
    const json misspelt = refusal([&] { (void) python_api_entry(reference, "orca.host.TriangleMesh.face_normal"); });
    REQUIRE(misspelt.is_object());
    CHECK(misspelt.value("code", std::string()) == "invalid_argument");
    REQUIRE(misspelt.at("did_you_mean").size() == 3);
    CHECK(misspelt.at("did_you_mean").front() == "orca.host.TriangleMesh.face_normals");
}

TEST_CASE("no description starts with a bracketed tag", "[Mcp]")
{
    const std::regex tag(R"(^\[[a-z]+\] )");
    for (const NativeCommand& def : Slic3r::GUI::NativeCommands::instance().commands())
        if (def.listed) {
            INFO(def.key);
            CHECK_FALSE(std::regex_search(def.description, tag));
        }

    // The catalog keeps a listed command's description as it is given.
    CommandCatalog registry;
    NativeCommand  def;
    def.key         = "widget_click";
    def.description = "Click a widget.";
    def.listed      = true;
    registry.add(def);
    REQUIRE(registry.find("widget_click") != nullptr);
    CHECK(registry.find("widget_click")->description == "Click a widget.");
}

TEST_CASE("a guessed type is read as a command's one variant argument", "[Mcp]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    const NativeCommand*  export_ = catalog.find("project_export");
    REQUIRE(export_ != nullptr);
    json args{{"type", "stl"}, {"path", "a.stl"}};
    CHECK(argument_error(*export_, args).empty());
    CHECK(args == json{{"format", "stl"}, {"path", "a.stl"}});

    const NativeCommand* primitive = catalog.find("object_add_primitive");
    REQUIRE(primitive != nullptr);
    json shape{{"type", "cube"}, {"silent", true}};
    CHECK(argument_error(*primitive, shape).empty());
    CHECK(shape == json{{"shape", "cube"}, {"silent", true}});

    // part_add has a type of its own beside its shape: type stays the part's type.
    const NativeCommand* part = catalog.find("part_add");
    REQUIRE(part != nullptr);
    json part_args{{"object", 0}, {"type", "modifier"}};
    CHECK(argument_error(*part, part_args).empty());
    CHECK(part_args == json{{"object", 0}, {"type", "modifier"}});

    // A command with two such arguments leaves a guessed type to be refused: it could be either.
    NativeCommand two;
    two.key          = "two_variants";
    two.input_schema = object_schema({{"format", enum_prop({"stl", "obj"}, "A format.")},
                                      {"shape", enum_prop({"cube", "stl"}, "A shape.")}});
    json either{{"type", "stl"}};
    CHECK_THAT(argument_error(two, either), Catch::Matchers::ContainsSubstring("type"));
    CHECK(either == json{{"type", "stl"}});
}

namespace {

bool lists_values(const json& schema)
{
    if (schema.contains("enum"))
        return true;
    const json type = schema.value("type", json());
    if (type.is_string())
        return type == "boolean";
    return type.is_array() && !type.empty() &&
           std::all_of(type.begin(), type.end(), [](const json& t) { return t == "boolean" || t == "null"; });
}

bool with_fields(const json& schema)
{
    return schema.contains("properties") || (schema.contains("items") && schema["items"].is_object() && schema["items"].contains("properties"));
}

void check_shape(const json& param, const json& schema, const std::string& where);

// That `params`, a manual's, follow `props`, the schema's properties they were made from, with `required`.
void check_parameters(const json& params, const json& props, const json& required, const std::string& where)
{
    REQUIRE(params.is_array());
    std::set<std::string> named, in_schema;
    for (const json& p : params)
        named.insert(p.at("name").get<std::string>());
    for (auto it = props.begin(); it != props.end(); ++it)
        in_schema.insert(it.key());
    CHECK(named == in_schema);
    for (const json& p : params) {
        const std::string name = p.at("name").get<std::string>();
        INFO(where + name);
        check_shape(p, props.at(name), where + name);
        if (std::find(required.begin(), required.end(), name) != required.end())
            CHECK(p.at("required") == true);
    }
}

// A schema's type in the manual's words: "number", or "number or null" for a list.
std::string type_words(const json& type)
{
    if (type.is_string())
        return type.get<std::string>();
    std::string out;
    for (const json& t : type)
        out += (out.empty() ? "" : " or ") + t.get<std::string>();
    return out;
}

void check_shape(const json& param, const json& schema, const std::string& where)
{
    INFO(where);
    CHECK_FALSE(param.contains("schema"));
    if (schema.contains("type"))
        CHECK(param.at("type") == type_words(schema["type"]));
    else
        CHECK_FALSE(param.contains("type"));
    if (schema.contains("description"))
        CHECK(param.at("description") == schema["description"]);
    else
        CHECK_FALSE(param.contains("description"));
    static const std::pair<const char*, const char*> kBounds[] = {
        {"minimum", "min"},          {"maximum", "max"},          {"exclusiveMinimum", "exclusive_min"},
        {"minItems", "min_items"},   {"maxItems", "max_items"},   {"minLength", "min_length"},
        {"maxLength", "max_length"}, {"pattern", "pattern"},      {"format", "format"}};
    for (const auto& [from, to] : kBounds) {
        INFO(from);
        if (schema.contains(from))
            CHECK(param.at(to) == schema[from]);
        else
            CHECK_FALSE(param.contains(to));
    }
    if (schema.contains("enum"))
        CHECK(param.at("values") == schema["enum"]);
    else
        CHECK_FALSE(param.contains("values"));
    if (schema.contains("default"))
        CHECK(param.at("default") == schema["default"]);
    else
        CHECK_FALSE(param.contains("default"));
    if (schema.contains("properties"))
        check_parameters(param.at("properties"), schema["properties"], schema.value("required", json::array()), where + ".");
    // An object states its other keys: none, any, or values of a shape.
    const json type   = schema.value("type", json());
    const bool object = schema.contains("properties") || type == "object" ||
                        (type.is_array() && std::find(type.begin(), type.end(), "object") != type.end());
    if (schema.contains("additionalProperties") && schema["additionalProperties"].is_boolean())
        CHECK(param.at("other_keys") == schema["additionalProperties"]);
    else if (schema.contains("additionalProperties"))
        CHECK(param.at("other_keys").is_object());
    else if (object)
        CHECK(param.at("other_keys") == true);
    else
        CHECK_FALSE(param.contains("other_keys"));
    if (schema.contains("items"))
        check_shape(param.at("items"), schema["items"], where + "[]");
}

// Calls `f` with the path and schema of each parameter of `props` at every depth, those with fields of their own too.
void each_parameter(const json& props, const std::string& prefix, const std::function<void(const std::string&, const json&)>& f)
{
    for (auto it = props.begin(); it != props.end(); ++it) {
        const std::string path   = prefix + it.key();
        const json&       schema = it.value();
        if (schema.contains("properties"))
            each_parameter(schema["properties"], path + ".", f);
        if (schema.contains("items") && schema["items"].is_object() && schema["items"].contains("properties"))
            each_parameter(schema["items"]["properties"], path + "[].", f);
        f(path, schema);
    }
}

// The parameter at `path` (dotted, [] for an array's items) of `manual`, or nullptr.
const json* manual_parameter(const json& manual, const std::string& path)
{
    const json* params = &manual.at("parameters");
    std::size_t at     = 0;
    for (;;) {
        const std::size_t end     = path.find('.', at);
        std::string       segment = path.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const bool        items   = segment.size() > 2 && segment.compare(segment.size() - 2, 2, "[]") == 0;
        if (items)
            segment.resize(segment.size() - 2);
        const json* found = nullptr;
        for (const json& p : *params)
            if (p.at("name") == segment)
                found = &p;
        if (found == nullptr || end == std::string::npos)
            return found;
        const json* holder = items ? (found->contains("items") ? &found->at("items") : nullptr) : found;
        if (holder == nullptr || !holder->contains("properties"))
            return nullptr;
        params = &holder->at("properties");
        at     = end + 1;
    }
}

json manual_of(const CommandCatalog& catalog, const char* key)
{
    const NativeCommand* command = catalog.find(key);
    REQUIRE(command != nullptr);
    return command_manual(*command);
}

} // namespace

TEST_CASE("tool_describe's manual follows every built-in command's schema at every depth", "[Mcp][CAD]")
{
    const CommandCatalog catalog = catalog_with_cad();
    std::size_t          ruled   = 0;
    for (const NativeCommand& c : catalog.commands()) {
        INFO(c.key);
        const json manual = command_manual(c);
        CHECK(manual.at("complete") == true);
        CHECK_FALSE(manual.contains("input_schema"));
        check_parameters(manual.at("parameters"), c.input_schema.value("properties", json::object()),
                         c.input_schema.value("required", json::array()), "");
        // One sentence per rule of with_silent()'s, and one for the arguments a closed schema refuses.
        const json& rules = manual.at("rules");
        if (c.input_schema.contains("if")) {
            ++ruled;
            CHECK(std::count_if(rules.begin(), rules.end(), [](const json& r) {
                      const std::string text = r.get<std::string>();
                      return text.rfind("With any argument, ", 0) == 0 || text.rfind("Unless the arguments are exactly ", 0) == 0;
                  }) == 1);
        }
        if (c.input_schema.value("additionalProperties", true) == false)
            CHECK(std::find(rules.begin(), rules.end(), "No other arguments are accepted.") != rules.end());
        CHECK(manual.at("examples") == (c.examples.is_null() ? json::array() : c.examples));
        CHECK(manual.at("menu_items").size() == (c.actions.is_array() ? c.actions.size() : 0) + (c.has_own_action ? 1 : 0));
        CHECK(manual.at("access").at("read_only") == c.read_only);
        CHECK(manual.at("listed") == c.listed);
    }
    CHECK(ruled == 38);

    // A rule's requirement comes out in words on the parameter it makes required.
    const json constrain = manual_of(catalog, "sketch_constrain");
    const json* type     = manual_parameter(constrain, "type");
    REQUIRE(type != nullptr);
    CHECK(type->at("required") == "when any argument is given");
    CHECK(constrain.at("rules")[0] == "With any argument, type is required.");
    // A parameter tied to a target says so, and stays as the schema requires it: the handler asks for the rest.
    const json  set   = manual_of(catalog, "settings_set");
    const json* min_z = manual_parameter(set, "min_z");
    REQUIRE(min_z != nullptr);
    CHECK(min_z->at("goes_with") == json{{"target", "height_range"}});
    CHECK(min_z->at("required") == false);
    // A nested object's own required fields and closure, and other keys' values.
    const json  cut   = manual_of(catalog, "object_cut");
    const json* point = manual_parameter(cut, "plane.point");
    REQUIRE(point != nullptr);
    CHECK(point->at("required") == true);
    const json  answer = manual_of(catalog, "dialog_auto_answer");
    const json* match  = manual_parameter(answer, "rules[].match");
    REQUIRE(match != nullptr);
    CHECK(match->at("required") == true);
    const json  view   = manual_of(catalog, "view_set");
    const json* legend = manual_parameter(view, "legend");
    REQUIRE(legend != nullptr);
    CHECK(legend->at("other_keys").at("type") == "boolean");
}

TEST_CASE("tool_describe's manual names a command's menu items with their arguments", "[Mcp]")
{
    const CommandCatalog catalog = catalog_with_cad();
    const json           exports = manual_of(catalog, "project_export").at("menu_items");
    const auto gcode = std::find_if(exports.begin(), exports.end(), [](const json& i) { return i.at("title") == "Export G-code"; });
    REQUIRE(gcode != exports.end());
    CHECK(gcode->at("args") == json{{"format", "gcode"}});
    CHECK(gcode->at("section") == "Slice & Export");
    CHECK_FALSE(gcode->contains("asks_for"));
    // An item that collects a value first says which, and the argument it goes to.
    const json views = manual_of(catalog, "view_set").at("menu_items");
    const auto layer = std::find_if(views.begin(), views.end(), [](const json& i) { return i.at("title") == "Go to layer (percent)"; });
    REQUIRE(layer != views.end());
    CHECK(layer->at("asks_for") == json{{"value", "percent"}, {"argument", "layer_percent"}});
    const auto tab = std::find_if(views.begin(), views.end(), [](const json& i) { return i.at("title") == "Go to tab..."; });
    REQUIRE(tab != views.end());
    CHECK(tab->at("asks_for") == json{{"value", "tab"}, {"argument", "tab"}});
    // How to call it: by name when the tool list shows it, through tool_call when not.
    CHECK(manual_of(catalog, "plate_slice").at("call") == "plate_slice {args}");
    CHECK(manual_of(catalog, "solid_fillet").at("call") == "tool_call {tool: \"solid_fillet\", args}");
}

TEST_CASE("every built-in parameter is classified for the manual, or read through its fields", "[Mcp][CAD]")
{
    const CommandCatalog catalog = catalog_with_cad();
    // The parameters with fields of their own that name a source besides their fields, and the source.
    std::map<std::string, json> containers;
    for (const NativeCommand& c : catalog.commands()) {
        const json manual = command_manual(c);
        each_parameter(c.input_schema.value("properties", json::object()), "", [&](const std::string& path, const json& schema) {
            if (lists_values(schema))
                return;
            INFO(c.key + "." + path);
            if (with_fields(schema)) {
                const json* param = manual_parameter(manual, path);
                REQUIRE(param != nullptr);
                if (param->contains("values_from"))
                    containers[c.key + "." + path] = param->at("values_from");
                return;
            }
            const std::optional<ParameterClass> cls = parameter_class(c.key, path);
            REQUIRE(cls.has_value());
            const json* param = manual_parameter(manual, path);
            REQUIRE(param != nullptr);
            if (cls->kind == ParameterClass::Source)
                CHECK(param->at("values_from") == json(cls->tools));
            else
                CHECK_FALSE(param->contains("values_from"));
        });
    }
    // Only a command's own entry gives one a source: plane, say, is a reference in one command and a cut plane in
    // another.
    const std::map<std::string, json> with_source{{"printer_install.models", {"printer_list_models"}},
                                                  {"printer_uninstall.models", {"printer_list_models"}},
                                                  {"dialog_answer.dialog", {"app_state"}}};
    CHECK(containers == with_source);
    // The sources of names that mean different things in different commands, and of the contested ones.
    const std::vector<std::pair<std::string, std::vector<std::string>>> sources{
        {"tool_call.args", {"tool_describe"}},
        {"tool_call.tool", {"tools/list", "tool_list"}},
        {"action_run.id", {"action_list"}},
        {"action_run.args", {"action_list"}},
        {"settings_set.values", {"settings_list"}},
        {"project_set_settings.values", {"project_get_settings"}},
        {"preferences_set.values", {"preferences_get"}},
        {"settings_get.compare", {"preset_list"}},
        {"preset_select.name", {"preset_list"}},
        {"preset_list.vendor", {"preset_list"}},
        {"printer_install.models[].vendor", {"printer_list_models"}},
        {"printer_install.models", {"printer_list_models"}},
        {"dialog_answer.dialog", {"app_state"}},
        {"filament_set_flushing.prime_volume_mode", {"filament_list"}},
        {"calibration_generate_test.shaper_type", {"settings_get"}},
        {"solid_hole.at", {"body_describe"}},
        {"solid_thread.at", {"body_describe"}},
        {"job_cancel.job", {"app_state"}},
        {"dialog_answer.button", {"app_state"}},
        {"widget_click.popup_item", {"widget_click"}},
        {"solid_fillet.edges", {"body_describe"}},
        {"sketch_constrain.entities", {"sketch_describe"}},
        {"solid_rib.entity", {"sketch_describe"}},
        {"solid_pattern.entity", {"sketch_describe"}},
        {"solid_loft.profiles", {"design_describe"}},
        {"solid_align.cs_a", {"design_describe"}},
        {"body_compare.reference", {"design_describe"}},
        {"view_set.layer", {"plate_slice_stats"}},
        {"view_set.layer_z", {"plate_slice_stats"}},
        {"plate_set_custom_gcode.add[].layer", {"plate_slice_stats"}},
        {"window_open.key", {"preferences_get"}},
    };
    for (const auto& [where, tools] : sources) {
        INFO(where);
        const std::size_t dot     = where.find('.');
        const std::string command = where.substr(0, dot);
        const json        manual  = manual_of(catalog, command.c_str());
        const json*       param   = manual_parameter(manual, where.substr(dot + 1));
        REQUIRE(param != nullptr);
        CHECK(param->at("values_from") == json(tools));
    }
    // The caller's own values name no source.
    for (const char* where : {"script_run.args", "replay_run.vars", "keyboard_press.keys", "sketch_draw.entities", "preset_save.name"}) {
        INFO(where);
        const std::string w   = where;
        const std::size_t dot = w.find('.');
        const std::optional<ParameterClass> cls = parameter_class(w.substr(0, dot), w.substr(dot + 1));
        REQUIRE(cls.has_value());
        CHECK(cls->kind == ParameterClass::Free);
    }
    // A plugin's operation names its parameters itself: none is classified, whatever it is called.
    NativeCommand operation;
    operation.key          = "plugin.fixture.measure";
    operation.input_schema = object_schema({{"object", prop("integer", "An object.")}});
    const json  described  = command_manual(operation);
    const json* object     = manual_parameter(described, "object");
    REQUIRE(object != nullptr);
    CHECK_FALSE(object->contains("values_from"));
}

TEST_CASE("tool_describe keeps raw what it can't express, and says the manual is not complete", "[Mcp]")
{
    const auto manual = [](json schema) {
        NativeCommand operation;
        operation.key          = "plugin.fixture.op";
        operation.input_schema = std::move(schema);
        return command_manual(operation);
    };
    const json root_any = json::parse(R"({"type": "object", "properties": {"a": {"type": "number"}}, "anyOf": [{"required": ["a"]}]})");
    const json any_root = manual(root_any);
    CHECK(any_root.at("complete") == false);
    CHECK(any_root.at("input_schema") == root_any);

    const json sub     = json::parse(R"({"anyOf": [{"type": "number"}, {"type": "string"}]})");
    const json any_sub = manual(json{{"type", "object"}, {"properties", {{"a", sub}}}});
    CHECK(any_sub.at("complete") == false);
    CHECK(any_sub.at("parameters")[0].at("schema") == sub);
    CHECK(any_sub.contains("input_schema"));

    const json defs = json::parse(R"({"type": "object", "$defs": {"n": {"type": "number"}}, "properties": {"a": {"$ref": "#/$defs/n"}}})");
    const json ref  = manual(defs);
    CHECK(ref.at("complete") == false);
    CHECK(ref.at("input_schema").contains("$defs"));

    const json anything = manual(json::parse(R"({"type": "object", "properties": {"a": true}})"));
    CHECK(anything.at("complete") == false);
    CHECK(anything.at("parameters")[0].at("schema") == true);

    // A draft-04 exclusiveMinimum qualifies minimum: kept raw, beside the minimum it reads.
    const json draft4 = manual(json::parse(R"({"type": "object", "properties": {"a": {"type": "number", "minimum": 0, "exclusiveMinimum": true}}})"));
    CHECK(draft4.at("complete") == false);
    CHECK(draft4.at("parameters")[0].at("min") == 0);
    CHECK(draft4.at("parameters")[0].contains("schema"));

    // A top-level description has no place in the manual, whose description is the command's.
    const json described = json::parse(R"({"type": "object", "description": "A measurement.", "properties": {"a": {"type": "number"}}})");
    const json with_text = manual(described);
    CHECK(with_text.at("complete") == false);
    CHECK(with_text.at("input_schema") == described);

    // A required name with no property is a constraint no parameter carries, at the top and inside a parameter.
    const json token        = json::parse(R"({"type": "object", "properties": {}, "required": ["token"]})");
    const json needs_token  = manual(token);
    CHECK(needs_token.at("complete") == false);
    CHECK(needs_token.at("input_schema") == token);
    const json inner        = json::parse(R"({"type": "object", "properties": {}, "required": ["b"]})");
    const json inner_needs  = manual(json{{"type", "object"}, {"properties", {{"a", inner}}}});
    CHECK(inner_needs.at("complete") == false);
    CHECK(inner_needs.at("parameters")[0].at("schema") == inner);

    // A properties that is not an object lists no parameters.
    const json no_props = json::parse(R"({"type": "object", "properties": true})");
    const json unlisted = manual(no_props);
    CHECK(unlisted.at("complete") == false);
    CHECK(unlisted.at("input_schema") == no_props);

    // A nested object open to other keys says so, as an open top level does.
    const json nested = manual(json::parse(R"({"type": "object", "properties": {"opts": {"type": "object",
                                                "properties": {"x": {"type": "number"}}}}, "additionalProperties": false})"));
    CHECK(nested.at("complete") == true);
    CHECK(nested.at("parameters")[0].at("other_keys") == true);

    // A keyword kept raw may close an object its additionalProperties leaves open, so neither a parameter nor the top
    // level says it takes other keys: the raw schema tells.
    const json closed_by_all_of = json::parse(R"({"type": "object", "allOf": [{"properties": {"x": {"type": "number"}},
                                                  "additionalProperties": false}]})");
    const json nested_all_of    = manual(json{{"type", "object"}, {"properties", {{"opts", closed_by_all_of}}},
                                              {"additionalProperties", false}});
    CHECK(nested_all_of.at("complete") == false);
    CHECK(nested_all_of.at("parameters")[0].at("schema") == closed_by_all_of);
    CHECK_FALSE(nested_all_of.at("parameters")[0].contains("other_keys"));
    const json top_all_of = manual(closed_by_all_of);
    CHECK(top_all_of.at("complete") == false);
    CHECK(top_all_of.at("rules") == json::array());

    // An open top level takes arguments no parameter names; a closed one says it refuses them.
    const json open = manual(json::parse(R"({"type": "object", "additionalProperties": true})"));
    CHECK(open.at("complete") == true);
    CHECK(open.at("parameters") == json::array());
    CHECK(open.at("rules") == json::array({"Other arguments are accepted."}));
    const json closed = manual(json::parse(R"({"type": "object", "properties": {}, "additionalProperties": false})"));
    CHECK(closed.at("rules") == json::array({"No other arguments are accepted."}));

    // A type given as a boolean subschema lists no values, so another parameter's "Type x:" lead ties it to none.
    const json loose = manual(json::parse(R"({"type": "object", "properties": {"type": true,
                                               "b": {"type": "number", "description": "Type x: a width."}}})"));
    CHECK(loose.at("complete") == false);
    const json* width = manual_parameter(loose, "b");
    REQUIRE(width != nullptr);
    CHECK_FALSE(width->contains("goes_with"));

    // The keywords no built-in command uses, as a plugin's schema may: a pattern, a format, a type that takes null.
    const json keywords = json::parse(R"({"type": "object", "properties": {"code": {"type": "string", "pattern": "^[a-z]+$",
                                              "format": "date", "minLength": 2, "maxLength": 8},
                                              "w": {"type": ["number", "null"], "exclusiveMinimum": 0},
                                              "ids": {"type": "array", "items": {"type": "integer"}, "minItems": 1, "maxItems": 3}},
                                              "additionalProperties": false})");
    const json words    = manual(keywords);
    CHECK(words.at("complete") == true);
    check_parameters(words.at("parameters"), keywords["properties"], json::array(), "");
    const json* w = manual_parameter(words, "w");
    REQUIRE(w != nullptr);
    CHECK(w->at("type") == "number or null");

    // A rule of with_silent()'s shape reads its forms as the schema writes them, menu items or none.
    const json forms = manual(json::parse(R"({"type": "object", "properties": {"a": {"type": "integer"}, "b": {"type": "string"}},
                                              "if": {"not": {"enum": [{}, {"a": 1}]}}, "then": {"required": ["b"]}})"));
    CHECK(forms.at("complete") == true);
    CHECK(forms.at("rules") == json::array({"Unless the arguments are exactly {} or {\"a\":1}, b is required.",
                                            "Other arguments are accepted."}));
    const json* b = manual_parameter(forms, "b");
    REQUIRE(b != nullptr);
    CHECK(b->at("required") == "unless the arguments are exactly one of the forms rules gives");
}

TEST_CASE("tool_describe describes any command a lookup finds, and refuses a name with its reason", "[Mcp]")
{
    CommandCatalog catalog;
    NativeCommand  listed;
    listed.key    = "measure_bed";
    listed.listed = true;
    catalog.add(listed);
    NativeCommand operation;
    operation.key         = "plugin.fixture.measure";
    operation.group       = "Plugins";
    operation.description = "Offered by a plugin. Doubles a.";
    catalog.add_source(std::make_unique<OneCommandSource>(operation));
    catalog.seal();
    CHECK(describe_command(catalog, "measure_bed").at("call") == "measure_bed {args}");
    const json described = describe_command(catalog, "plugin.fixture.measure");
    CHECK(described.at("group") == "Plugins");
    CHECK(described.at("listed") == false);
    try {
        describe_command(catalog, "measure_bedd");
        FAIL("an unknown tool was not refused");
    } catch (const ToolFailure& f) {
        CHECK_THAT(std::string(f.what()), Catch::Matchers::StartsWith("Unknown tool: measure_bedd"));
        CHECK(f.details.at("code") == "invalid_argument");
        CHECK(f.details.at("did_you_mean") == json::array({"measure_bed"}));
    }
#ifdef SLIC3R_CAD
    // A Design command that this run's catalog leaves out is refused with the reason a call of it gets.
    for (const bool editor : {true, false}) {
        INFO(editor);
        CommandCatalog left_out;
        NativeCommands::register_all_commands(left_out, {false, editor});
        left_out.seal();
        try {
            describe_command(left_out, "solid_fillet");
            FAIL("a left-out Design command was described");
        } catch (const ToolFailure& f) {
            CHECK(std::string(f.what()) == left_out.unknown_command("solid_fillet"));
            CHECK(f.details.at("code") == "design_unavailable");
            CHECK_THAT(std::string(f.what()),
                       Catch::Matchers::ContainsSubstring(editor ? "the CAD feature was off" : "the G-code viewer has no Design tab"));
        }
    }
#endif
}

TEST_CASE("tool_list lists the commands the tool list does not show, a source's included", "[Mcp]")
{
    const auto command = [](const char* key, const char* group, bool listed, std::string description) {
        NativeCommand c;
        c.key         = key;
        c.group       = group;
        c.listed      = listed;
        c.description = std::move(description);
        return c;
    };
    CommandCatalog catalog;
    catalog.add(command("measure_bed", "Plugins", true, "Measure the bed. In mm."));
    catalog.add(command("view_measure", "View and windows", false, "Measure what the view shows. Agents use measure_bed."));
    catalog.add(command("object_move", "Objects", true, "Move an object."));
    // An operation's description, past the 300 characters given whole: what it does is its own first sentence.
    const std::string long_text = "Doubles a. " + std::string(320, 'x') + ".";
    catalog.add_source(std::make_unique<OneCommandSource>(command("plugin.fixture.measure", "Plugins", false, "Offered by a plugin. " + long_text)));
    catalog.seal();

    const json all = unlisted_commands(catalog, "");
    CHECK(all.at("items") == json::array({{{"name", "view_measure"}, {"group", "View and windows"},
                                           {"does", "Measure what the view shows. Agents use measure_bed."}},
                                          {{"name", "plugin.fixture.measure"}, {"group", "Plugins"}, {"does", "Doubles a."}}}));
    CHECK_THAT(all.at("use").get<std::string>(), Catch::Matchers::ContainsSubstring("tool_describe {tool}"));
    CHECK_FALSE(all.contains("note"));
    // A group in any case, a source's commands in it.
    const json plugins = unlisted_commands(catalog, "PLUGINS");
    REQUIRE(plugins.at("items").size() == 1);
    CHECK(plugins.at("items")[0].at("name") == "plugin.fixture.measure");
    // A group whose commands the tool list all shows: none, and a note saying so.
    const json objects = unlisted_commands(catalog, "objects");
    CHECK(objects.at("items").empty());
    CHECK(objects.at("note") == "tools/list shows every command of Objects");
    // A name no command's group has is refused with the groups there are.
    try {
        unlisted_commands(catalog, "commands");
        FAIL("an unknown group was not refused");
    } catch (const ToolFailure& f) {
        CHECK_THAT(std::string(f.what()), Catch::Matchers::StartsWith("Unknown group: commands"));
        CHECK(f.details.at("groups") == json::array({"Plugins", "View and windows", "Objects"}));
        CHECK(f.details.at("code") == "invalid_argument");
    }
}

TEST_CASE("tool_list gives the menu items then the Design commands by heading, in the catalog's order", "[Mcp][CAD]")
{
    const CommandCatalog catalog = catalog_with_cad();
    const json           all     = unlisted_commands(catalog, "");
    std::vector<std::string> names, expected;
    for (const json& item : all.at("items"))
        names.push_back(item.at("name").get<std::string>());
    for (const NativeCommand& c : catalog.commands())
        if (!c.listed)
            expected.push_back(c.key);
    CHECK(names == expected);
    REQUIRE(names.size() == 92);
    CHECK(std::set<std::string>(names.begin(), names.begin() + 4) ==
          std::set<std::string>{"window_save_screenshot", "script_toggle_panel", "settings_toggle_developer_mode", "view_toggle_activity_panel"});
    // Each Design command under its heading, heading by heading in the order tool_list's description indexes them.
    const std::string told    = catalog.find("tool_list")->description;
    std::size_t       indexed = 0;
    for (std::size_t i = 4; i < names.size(); ++i) {
        const json& item = all.at("items")[i];
        INFO(item.dump());
        CHECK(item.at("group") == Slic3r::GUI::kDesignGroup);
        const std::string heading = item.value("heading", std::string());
        REQUIRE_FALSE(heading.empty());
        const std::size_t at = told.find(". " + heading);
        REQUIRE(at != std::string::npos);
        CHECK(at >= indexed);
        indexed = at;
    }
    // What each does: a description of up to 300 characters whole, a longer one's first sentence.
    for (const json& item : all.at("items")) {
        const NativeCommand* c = catalog.find(item.at("name").get<std::string>());
        REQUIRE(c != nullptr);
        INFO(c->key);
        CHECK(item.at("does") == (c->description.size() <= 300 ? c->description : first_sentence(c->description)));
    }
    CHECK(unlisted_commands(catalog, "design").at("items").size() == 88);
    // The groups the refusal names: every place's and Design.
    try {
        unlisted_commands(catalog, "commands");
        FAIL("an unknown group was not refused");
    } catch (const ToolFailure& f) {
        CHECK(f.details.at("groups").size() == 14);
    }
}

#ifdef SLIC3R_CAD
TEST_CASE("tool_list refuses the Design group of a run that leaves its commands out, with their reason", "[Mcp][CAD]")
{
    // The editor with the CAD feature off, and the G-code viewer, as GUI_App builds them.
    for (const bool editor : {true, false}) {
        INFO(editor);
        CommandCatalog catalog;
        NativeCommands::register_all_commands(catalog, {false, editor});
        catalog.seal();
        const std::string* why = catalog.left_out_reason("design_unavailable");
        REQUIRE(why != nullptr);
        CHECK_THAT(*why, Catch::Matchers::ContainsSubstring(editor ? "the CAD feature was off" : "the G-code viewer has no Design tab"));
        try {
            unlisted_commands(catalog, "Design");
            FAIL("the left-out Design group was listed");
        } catch (const ToolFailure& f) {
            CHECK(f.details.at("code") == "design_unavailable");
            CHECK_THAT(std::string(f.what()), Catch::Matchers::EndsWith(*why));
            // A Design command's own call gives the same reason.
            CHECK_THAT(catalog.unknown_command("solid_fillet"), Catch::Matchers::EndsWith(*why));
        }
        // The other groups are as they are: 13 of them.
        try {
            unlisted_commands(catalog, "commands");
            FAIL("an unknown group was not refused");
        } catch (const ToolFailure& f) {
            CHECK(f.details.at("groups").size() == 13);
        }
    }
}
#endif

TEST_CASE("a plugin's call is checked as a client's, and a bad argument's refusal names tool_describe", "[Mcp]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    json                  args{{"platez", 1}};
    try {
        resolve_plugin_call(catalog, "plate_slice", args);
        FAIL("a misspelt argument was not refused");
    } catch (const Slic3r::ToolCallError& e) {
        CHECK_THAT(std::string(e.what()), Catch::Matchers::StartsWith("Invalid arguments for plate_slice: unknown argument platez"));
        CHECK(e.details.at("hint") == "tool_describe {tool: \"plate_slice\"} gives its parameters");
    }
    // Through tool_call, the refusal names the target.
    json wrapped{{"tool", "plate_slice"}, {"args", {{"platez", 1}}}};
    try {
        resolve_plugin_call(catalog, "tool_call", wrapped);
        FAIL("a misspelt argument was not refused");
    } catch (const Slic3r::ToolCallError& e) {
        CHECK(e.details.at("hint") == "tool_describe {tool: \"plate_slice\"} gives its parameters");
    }
    // An unknown tool, with the closest names.
    json none = json::object();
    try {
        resolve_plugin_call(catalog, "plate_slicee", none);
        FAIL("an unknown tool was not refused");
    } catch (const Slic3r::ToolCallError& e) {
        CHECK(e.details.at("code") == "invalid_argument");
        CHECK_FALSE(e.details.at("did_you_mean").empty());
    }
    // A good call resolves to its command, a tool_call to its target with the target's arguments.
    json ok{{"tool", "plate_slice"}, {"args", json::object()}};
    CHECK(resolve_plugin_call(catalog, "tool_call", ok)->key == "plate_slice");
    CHECK(ok == json::object());
}

TEST_CASE("window_set refuses a request the window can't take, before changing anything", "[Mcp]")
{
    WindowAbilities main;   main.main = true;
    WindowAbilities modal;  modal.modal = true; modal.can_maximize = true; modal.can_minimize = true;
    WindowAbilities fixed;  // a modeless window whose title bar offers neither
    WindowAbilities wayland; wayland.main = true; wayland.positions_known = false;

    CHECK(window_request_refusal(json::object(), main).find("size, position, state or raise") != std::string::npos);
    CHECK(window_request_refusal({{"state", "maximized"}, {"size", {800, 600}}}, main).find("maximized") != std::string::npos);
    CHECK(window_request_refusal({{"state", "minimized"}, {"position", {0, 0}}}, main).find("minimized") != std::string::npos);
    CHECK(window_request_refusal({{"state", "maximized"}}, main).empty());
    CHECK(window_request_refusal({{"state", "minimized"}}, main).empty());
    CHECK(window_request_refusal({{"state", "minimized"}}, modal).find("modal") != std::string::npos);
    CHECK(window_request_refusal({{"state", "maximized"}}, modal).empty());
    CHECK(window_request_refusal({{"state", "maximized"}}, fixed).find("maximize") != std::string::npos);
    CHECK(window_request_refusal({{"state", "normal"}}, fixed).empty());
    CHECK(window_request_refusal({{"size", {0, 600}}}, main).find("size") != std::string::npos);
    CHECK(window_request_refusal({{"size", {800}}}, main).find("[width, height]") != std::string::npos);
    CHECK(window_request_refusal({{"position", {0, 0}}}, wayland).find("X11") != std::string::npos);
    CHECK(window_request_refusal({{"position", {0, 0}}}, wayland).find("GDK_BACKEND=x11") != std::string::npos);
    CHECK(window_request_refusal({{"size", {800, 600}}, {"state", "maximized"}}, wayland).find("maximized") != std::string::npos);
    CHECK(window_request_refusal({{"state", "minimized"}}, wayland).empty());
    CHECK(window_request_refusal({{"raise", true}}, fixed).empty());
}

TEST_CASE("window_set's reply lists the fields the window changed, with a note on why", "[Mcp]")
{
    const json     sized{{"size", {800, 600}}};
    WindowSettling taken;
    taken.held  = true;
    taken.drawn = true;
    const auto note = [](const json& reply) { return reply.value("note", std::string()); };

    json reply{{"size", {800, 600}}, {"position", {10, 20}}, {"state", "normal"}};
    add_window_notes(sized, taken, reply);
    CHECK_FALSE(reply.contains("adjusted"));
    CHECK_FALSE(reply.contains("note"));

    // The window manager gave another size.
    reply = {{"size", {900, 600}}, {"state", "normal"}};
    add_window_notes(sized, taken, reply);
    CHECK(reply["adjusted"] == json::array({"size"}));
    CHECK(note(reply) == "the window manager adjusted the request");

    // A size limit held it.
    WindowSettling limited = taken;
    limited.limited        = true;
    reply                  = {{"size", {900, 600}}, {"state", "normal"}};
    add_window_notes(sized, limited, reply);
    CHECK(reply["adjusted"] == json::array({"size"}));
    CHECK(note(reply) == "size is held to the window's minimum or maximum size");

    // The window never held still in the state asked for.
    reply = {{"size", {800, 600}}, {"state", "maximized"}};
    add_window_notes({{"state", "normal"}}, WindowSettling(), reply);
    CHECK(reply["adjusted"] == json::array({"state"}));
    CHECK(note(reply).find("the window manager did not apply the request") != std::string::npos);

    // The window held still, but its 3D view never drew at its size: the note says so, not the window manager.
    WindowSettling undrawn;
    undrawn.held    = true;
    undrawn.undrawn = "OrcaSlicer's window is not being drawn";
    reply           = {{"size", {800, 600}}, {"state", "normal"}};
    add_window_notes(sized, undrawn, reply);
    CHECK_FALSE(reply.contains("adjusted"));
    CHECK(note(reply).find("the 3D view did not draw") != std::string::npos);
    CHECK(note(reply).find(undrawn.undrawn) != std::string::npos);
    CHECK(note(reply).find("window manager") == std::string::npos);

    // The wait ran out: the note gives how long it lasted.
    WindowSettling slow;
    slow.seconds = 5;
    reply        = {{"size", {800, 600}}, {"state", "normal"}};
    add_window_notes({{"state", "minimized"}}, slow, reply);
    CHECK(reply["adjusted"] == json::array({"state"}));
    CHECK(note(reply) == "the window manager did not apply the request within 5 s");

    // A Wayland compositor, or GNOME Shell on X11, that refused to activate a minimized window: the note says
    // so and how the person brings it back, not that the window manager was slow.
    WindowSettling refused;
    refused.activation = true;
    reply              = {{"size", {800, 600}}, {"state", "minimized"}};
    add_window_notes({{"state", "normal"}}, refused, reply);
    CHECK(reply["adjusted"] == json::array({"state"}));
    CHECK(note(reply).find("the window system did not bring the window back") != std::string::npos);
    CHECK(note(reply).find("taskbar") != std::string::npos);
    CHECK(note(reply).find("window manager") == std::string::npos);

    // An activation that brought the window back needs no note.
    WindowSettling activated = taken;
    activated.activation     = true;
    reply                    = {{"size", {800, 600}}, {"state", "normal"}};
    add_window_notes({{"state", "normal"}}, activated, reply);
    CHECK_FALSE(reply.contains("note"));
}

TEST_CASE("window_set sizes a window by what it shows, held to its limits", "[Mcp]")
{
    bool limited = true;
    // No invisible margins and no limits: the size asked for.
    CHECK(frame_size_for({800, 600}, {0, 0}, wxDefaultSize, wxDefaultSize, limited) == wxSize(800, 600));
    CHECK_FALSE(limited);
    // The margins of a client-side decorated window on Wayland go on top, so it shows the size asked for.
    CHECK(frame_size_for({800, 600}, {90, 90}, wxDefaultSize, wxDefaultSize, limited) == wxSize(890, 690));
    CHECK_FALSE(limited);
    // A minimum holds it, in wx's sizes, which count the margins: 836 x 539 shows 746 x 449.
    CHECK(frame_size_for({700, 400}, {90, 90}, wxSize(836, 539), wxDefaultSize, limited) == wxSize(836, 539));
    CHECK(limited);
    CHECK(frame_size_for({746, 449}, {90, 90}, wxSize(836, 539), wxDefaultSize, limited) == wxSize(836, 539));
    CHECK_FALSE(limited);
    // So does a maximum, on one side alone.
    CHECK(frame_size_for({2000, 600}, {0, 0}, wxDefaultSize, wxSize(1600, wxDefaultCoord), limited) == wxSize(1600, 600));
    CHECK(limited);
}

TEST_CASE("camera_state reads the camera view_set leaves", "[Mcp]")
{
    using Slic3r::Vec3d;
    using Slic3r::GUI::Camera;
    Camera camera;
    camera.set_viewport(0, 0, 800, 600);
    camera.set_scene_box(Slic3r::BoundingBoxf3(Vec3d(-100, -100, 0), Vec3d(100, 100, 100)));
    camera.set_type(Camera::EType::Ortho);
    camera.look_at(Vec3d(10, 20, 500), Vec3d(10, 20, 0), Vec3d::UnitY()); // straight down, +Y up the screen
    camera.set_zoom(2.0);
    const json s = camera_state(camera, 1.5);
    CHECK(s["type"] == "ortho");
    // The point it looks at goes by the name view_set takes it under.
    CHECK(s["look_at"] == json{10.0, 20.0, 0.0});
    CHECK_FALSE(s.contains("target"));
    CHECK(s["zoom"] == 2.0);
    CHECK(s["click_px_per_mm"] == 1.3333);
    CHECK(s["elevation"] == 90);
    CHECK(s["azimuth"] == -90);
    camera.look_at(Vec3d(10, -480, 0), Vec3d(10, 20, 0), Vec3d::UnitZ()); // from the front
    CHECK(camera_state(camera, 1.0)["elevation"] == 0);
    CHECK(camera_state(camera, 1.0)["azimuth"] == -90);
}

TEST_CASE("view_set takes the Design tab's camera, section view and switches", "[Mcp]")
{
    const NativeCommand* view_set = NativeCommands::find("view_set");
    REQUIRE(view_set != nullptr);
    const json& properties = view_set->input_schema.at("properties");
    for (const char* name : {"section", "origin_planes", "world_axes", "origin", "bed"}) {
        INFO(name);
        CHECK(properties.contains(name));
    }
    const json& cameras = properties.at("camera").at("enum");
    CHECK(std::find(cameras.begin(), cameras.end(), json("sketch_normal")) != cameras.end());

    json section{{"tab", "design"}, {"section", {{"on", true}, {"z", 2}, {"keep", "upper"}}}, {"origin", true}};
    CHECK(argument_error(*view_set, section).empty());
    // The section view is read whole: a key it has no use for, and a half it does not keep, are refused.
    json unknown{{"tab", "design"}, {"section", {{"on", true}, {"height", 2}}}};
    CHECK_THAT(argument_error(*view_set, unknown), Catch::Matchers::ContainsSubstring("section.height"));
    json left{{"tab", "design"}, {"section", {{"on", true}, {"keep", "left"}}}};
    CHECK_THAT(argument_error(*view_set, left), Catch::Matchers::ContainsSubstring("section.keep"));
}

TEST_CASE("a call naming an object's id is refused once its index names another object", "[Mcp]")
{
    Slic3r::Model model;
    Slic3r::ModelObject*   first    = model.add_object();
    Slic3r::ModelObject*   second   = model.add_object();
    first->name                     = "bracket";
    second->name                    = "lid";
    Slic3r::ModelVolume*   part     = first->add_volume(Slic3r::TriangleMesh());
    Slic3r::ModelInstance* instance = first->add_instance();
    Slic3r::ModelInstance* other    = first->add_instance();

    CHECK_NOTHROW(check_expected_ids(*first, nullptr, nullptr, json{{"object", 0}, {"object_id", first->id().id}}));
    CHECK_NOTHROW(check_expected_ids(*first, part, nullptr, json{{"object", 0}, {"part_id", part->id().id}}));
    CHECK_NOTHROW(check_expected_ids(*first, nullptr, instance, json{{"object", 0}, {"instance_id", instance->id().id}}));
    // Without ids the index alone decides.
    CHECK_NOTHROW(check_expected_ids(*second, nullptr, nullptr, json{{"object", 0}}));
    try {
        check_expected_ids(*second, nullptr, nullptr, json{{"object", 0}, {"object_id", first->id().id}});
        FAIL("the call was not refused");
    } catch (const ToolFailure& f) {
        CHECK(std::string(f.what()).find("lid") != std::string::npos);
        CHECK(f.details["id"] == second->id().id);
    }
    CHECK_THROWS_AS(check_expected_ids(*first, part, nullptr, json{{"part_id", part->id().id + 1000}}), ToolFailure);
    try {
        check_expected_ids(*first, nullptr, other, json{{"instance_id", instance->id().id}});
        FAIL("the call was not refused");
    } catch (const ToolFailure& f) {
        CHECK(f.details["id"] == other->id().id);
    }
}

TEST_CASE("the journal records indices without the ids beside them, except a Python step's arguments", "[Mcp]")
{
    const CommandCatalog& catalog = Slic3r::GUI::NativeCommands::instance();
    CHECK(without_guard_ids(catalog, "settings_set", json{{"object", 0}, {"object_id", 7}, {"part", 1}, {"part_id", 8}}) ==
          json{{"object", 0}, {"part", 1}});
    CHECK(without_guard_ids(catalog, "object_set_height_ranges", json{{"object", 0}, {"object_id", 7}}) == json{{"object", 0}});
    CHECK(without_guard_ids(catalog, "object_set_variable_layer_height", json{{"object", 0}, {"object_id", 7}}) == json{{"object", 0}});
    const json replay{{"replay", {{"orca_replay_version", "0.2.0"},
                                  {"steps", {{{"tool", "object_transform"}, {"args", {{"object", 1}, {"object_id", 9}}}},
                                             {{"tool", "script_run"}, {"args", {{"code", "x"}, {"args", {{"object_id", 9}}}}}}}}}}};
    const json recorded = without_guard_ids(catalog, "replay_run", replay);
    CHECK(recorded["replay"]["steps"][0]["args"] == json{{"object", 1}});
    CHECK(recorded["replay"]["steps"][1]["args"]["args"]["object_id"] == 9);
    CHECK(without_guard_ids(catalog, "script_run", json{{"code", "x"}, {"args", {{"object_id", 3}}}})["args"]["object_id"] == 3);
}

TEST_CASE("the journal records an inline replay given as JSON text as its object, without the ids beside indices", "[Mcp]")
{
    const CommandCatalog& catalog = Slic3r::GUI::NativeCommands::instance();
    const json as_text{{"replay", "// moves the lid\n"
                                  R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "object_transform", "args": {"object": 1, "object_id": 9}}]})"}};
    const json recorded = without_guard_ids(catalog, "replay_run", as_text);
    REQUIRE(recorded["replay"].is_object());
    CHECK(recorded["replay"]["steps"][0]["args"] == json{{"object", 1}});
    // Text that doesn't parse is the call's error; it is recorded as given.
    CHECK(without_guard_ids(catalog, "replay_run", json{{"replay", "{\"orca_replay_version\": \"0.2.0\","}}) == json{{"replay", "{\"orca_replay_version\": \"0.2.0\","}});
}

TEST_CASE("the journal leaves out a command's ids that stand beside their indices", "[Mcp]")
{
    const CommandCatalog& catalog = Slic3r::GUI::NativeCommands::instance();
    CHECK(without_guard_ids(catalog, "object_transform", json{{"object", 0}, {"object_id", 7}}) == json{{"object", 0}});
    // An id given alone still names its target: the command records its index once it reads it (with_target_ids()).
    CHECK(without_guard_ids(catalog, "object_transform", json{{"object_id", 7}}) == json{{"object_id", 7}});
    CHECK(without_guard_ids(catalog, "object_order", json{{"order", {1, 0}}, {"order_ids", {7, 5}}}) == json{{"order", {1, 0}}});
    CHECK(without_guard_ids(catalog, "object_arrange", json{{"fill_object", 1}, {"fill_object_id", 7}}) == json{{"fill_object", 1}});
    // Inside the tool_call, action_run and replay_run wrappers too.
    CHECK(without_guard_ids(catalog, "action_run", json{{"id", "object_transform"}, {"args", {{"object", 1}, {"object_id", 9}}}}) ==
          json{{"id", "object_transform"}, {"args", {{"object", 1}}}});
    CHECK(without_guard_ids(catalog, "tool_call", json{{"tool", "object_transform"}, {"args", {{"object", 1}, {"object_id", 9}}}}) ==
          json{{"tool", "object_transform"}, {"args", {{"object", 1}}}});
    const json replay{{"replay", {{"orca_replay_version", "0.2.0"},
                                  {"steps", {{{"tool", "tool_call"}, {"args", {{"tool", "object_transform"}, {"args", {{"object", 1}, {"object_id", 9}}}}}}}}}}};
    CHECK(without_guard_ids(catalog, "replay_run", replay)["replay"]["steps"][0]["args"]["args"] == json{{"object", 1}});
    // A plugin operation's own fields of the same names are its arguments, recorded as given.
    CHECK(without_guard_ids(catalog, "plugin.measure.area", json{{"object_id", 4}}) == json{{"object_id", 4}});
    CHECK(without_guard_ids(catalog, "tool_call", json{{"tool", "plugin.measure.area"}, {"args", {{"object_id", 4}}}})["args"]["object_id"] == 4);
    CHECK(without_guard_ids(catalog, "object_select", json{{"objects", {0, 1}}, {"object_ids", {5, 6}}}) == json{{"objects", {0, 1}}});
    CHECK(without_guard_ids(catalog, "object_select", json{{"instances", {{0, 1}}}, {"instance_ids", {5}}}) ==
          json{{"instances", {{0, 1}}}});
    CHECK(without_guard_ids(catalog, "object_select", json{{"parts", {{0, 1}}}, {"part_ids", {5}}}) == json{{"parts", {{0, 1}}}});
}

TEST_CASE("app_log keeps a line by the level its entry starts with, and a continuation line with its first line", "[Mcp]")
{
    // As the log file sink writes them (utils.cpp): the level, a tab, the time and thread, then the message, whose own
    // line breaks start lines with no level of their own.
    const std::string text = "[info]\t2026-11-17 10:00:00.000001[Thread 0x1]:loaded \"[error] notes.3mf\"\n"
                             "[error]\t2026-11-17 10:00:01.000001[Thread 0x1]:Failed to load the project:\n"
                             "  the file has no model\n"
                             "[debug]\t2026-11-17 10:00:02.000001[Thread 0x2]:frame [warning] skipped\n"
                             "  with a second line\n"
                             "[warning]\t2026-11-17 10:00:03.000001[Thread 0x1]:slow\n"
                             "[fatal]\t2026-11-17 10:00:04.000001[Thread 0x1]:out of memory\n";
    const auto read = [&text](const std::string& level, std::size_t lines) {
        std::istringstream in(text);
        return log_tail(in, level, lines);
    };
    // An info line whose message holds [error] is no error line; the error's second line comes with it.
    CHECK(read("error", 200) == std::vector<std::string>{"[error]\t2026-11-17 10:00:01.000001[Thread 0x1]:Failed to load the project:",
                                                         "  the file has no model",
                                                         "[fatal]\t2026-11-17 10:00:04.000001[Thread 0x1]:out of memory"});
    // A debug line whose message holds [warning] is no warning line, and its second line goes with it.
    const std::vector<std::string> warnings = read("warning", 200);
    REQUIRE(warnings.size() == 4);
    CHECK(warnings[2] == "[warning]\t2026-11-17 10:00:03.000001[Thread 0x1]:slow");
    // Every line at trace, the continuations among them; at info, all but the debug entry and its second line.
    CHECK(read("trace", 200).size() == 7);
    CHECK(read("info", 200).size() == 5);
    // The last lines are whole entries: a continuation line never comes without its first line.
    CHECK(read("error", 2) == std::vector<std::string>{"[fatal]\t2026-11-17 10:00:04.000001[Thread 0x1]:out of memory"});
    CHECK(read("error", 3).size() == 3);
    // An entry longer than the lines asked for gives its first lines.
    CHECK(read("error", 1) == std::vector<std::string>{"[fatal]\t2026-11-17 10:00:04.000001[Thread 0x1]:out of memory"});
    std::istringstream long_entry("[error]\tt:one\n  two\n  three\n");
    CHECK(log_tail(long_entry, "error", 2) == std::vector<std::string>{"[error]\tt:one", "  two"});
}

TEST_CASE("a plugin command refused after it changed the plugin names how the plugin is left", "[Mcp]")
{
    const json before{{"key", "cloud_one"}, {"name", "Cloud One"}, {"installed", false}, {"enabled", false}, {"error", ""}};
    const Slic3r::GUI::Ops::Refusal refusal{Slic3r::GUI::Ops::RefusalKind::Precondition, "manifest_not_found",
                                            "Plugin manifest was not found."};

    // A refusal after the call installed the plugin (a cloud download) says it is installed, and carries its row.
    json installed  = before;
    installed["installed"] = true;
    const ToolFailure after_install = plugin_refusal_after(refusal, before, installed);
    CHECK(std::string(after_install.what()) == "Plugin manifest was not found. It is installed, disabled.");
    CHECK(after_install.details.at("code") == "manifest_not_found");
    CHECK(after_install.details.at("plugin") == installed);

    // A failed download records its error on the plugin, which the error names.
    json errored     = before;
    errored["error"] = "the download timed out";
    const ToolFailure after_error = plugin_refusal_after(
        {Slic3r::GUI::Ops::RefusalKind::Failed, "download_failed", "the download timed out"}, before, errored);
    // The plugin's own error, which has no full stop, is a sentence of its own.
    CHECK(std::string(after_error.what()) ==
          "the download timed out. It is not installed, disabled, with the error \"the download timed out\".");
    CHECK(after_error.details.at("plugin") == errored);

    // A change of enabled alone is a change too: a plugin left loaded by a refused call.
    json loaded       = before;
    loaded["enabled"] = true;
    const ToolFailure after_load = plugin_refusal_after(refusal, before, loaded);
    CHECK(std::string(after_load.what()) == "Plugin manifest was not found. It is not installed, enabled.");
    CHECK(after_load.details.at("plugin") == loaded);

    // A plugin no longer listed afterwards is said so.
    const ToolFailure after_gone = plugin_refusal_after(refusal, before, json());
    CHECK(std::string(after_gone.what()) == "Plugin manifest was not found. It is no longer listed.");

    // With the plugin as it was, the refusal is bare: no state, no row.
    const ToolFailure unchanged = plugin_refusal_after(refusal, before, before);
    CHECK(std::string(unchanged.what()) == "Plugin manifest was not found.");
    CHECK(unchanged.details.at("code") == "manifest_not_found");
    CHECK_FALSE(unchanged.details.contains("plugin"));
}

TEST_CASE("plugin_enable's reply says a plugin is no longer listed", "[Mcp]")
{
    // A load that ended with the plugin gone from the list did not load, and the error says so.
    std::optional<ToolFailure> gone;
    try {
        plugin_enable_reply("my_plugin", true, /*load_started=*/true, /*still_loading=*/false, json());
    } catch (const ToolFailure& e) {
        gone = e;
    }
    REQUIRE(gone);
    CHECK(std::string(gone->what()) == "my_plugin did not load. It is no longer listed.");
    CHECK(gone->details.at("code") == "not_loaded");
    CHECK(gone->details.at("plugin").is_null());

    // Without a load, the reply carries the null row and says the plugin is no longer listed.
    const json unloaded = plugin_enable_reply("my_plugin", true, false, false, json());
    CHECK(unloaded.at("plugin").is_null());
    CHECK(unloaded.at("changed") == true);
    CHECK(unloaded.at("note") == "my_plugin is no longer listed.");

    // A load that ended with the plugin listed and on is the row; one that ended with it off is an error with its row.
    const json on{{"key", "my_plugin"}, {"installed", true}, {"enabled", true}, {"error", ""}};
    const json loaded = plugin_enable_reply("my_plugin", true, true, false, on);
    CHECK(loaded.at("plugin") == on);
    CHECK_FALSE(loaded.contains("note"));
    json off       = on;
    off["enabled"] = false;
    CHECK_THROWS_WITH(plugin_enable_reply("my_plugin", true, true, false, off), "my_plugin did not load. It is installed, disabled.");
}

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

#include "python_test_support.hpp"
#include "retired_names.hpp"

#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/WindowControl.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Camera.hpp"

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
namespace NativeCommands = Slic3r::GUI::NativeCommands;

namespace {

// Holds one command, as a plugin holds its operations while it is loaded.
class OneCommandSource final : public CommandSource
{
public:
    explicit OneCommandSource(NativeCommand command) : m_command(std::make_shared<const NativeCommand>(std::move(command))) {}

    std::shared_ptr<const NativeCommand> find(const std::string& key) const override
    {
        return key == m_command->key ? m_command : nullptr;
    }
    std::vector<std::shared_ptr<const NativeCommand>> all() const override { return {m_command}; }

private:
    std::shared_ptr<const NativeCommand> m_command;
};

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

TEST_CASE("a search matches query words at the start of a text's words, CamelCase parts included", "[Mcp]")
{
    // Short words drop out once the query has a longer one.
    CHECK(query_words("Add a Brim to it") == std::vector<std::string>{"add", "brim"});
    CHECK(query_words("z") == std::vector<std::string>{"z"});

    const std::vector<std::string> words = query_words("mesh normals");
    // "mesh" starts the second part of TriangleMesh and "normals" a word of the name: both in the name.
    CHECK(match_score(words, "orca.host.TriangleMesh.face_normals", "Per-triangle normals.") == std::pair<int, int>{2, 2});
    // A word only the text has counts, but not as a name match.
    CHECK(match_score(words, "orca.host.ModelVolume.mesh", "Face normals are not cached.") == std::pair<int, int>{2, 1});
    CHECK(match_score(words, "orca.host.Model", "The project's objects.") == std::pair<int, int>{0, 0});
    // A word is matched at the start of a word, never inside one.
    CHECK(match_score(query_words("range"), "arrange", "") == std::pair<int, int>{0, 0});
    CHECK(match_score(query_words("retract"), "retraction_length", "") == std::pair<int, int>{1, 1});
}

TEST_CASE("an action search ranks by the query's words and puts the plainer action first", "[Mcp]")
{
    // Four of the Speed Dial's actions as it lists them.
    const json listed = json::array({
        {{"id", "orca_command:obj_delete_all:orca"}, {"title", "Delete All Objects"}, {"group", "Object"},
         {"description", "Delete all objects."}},
        {{"id", "orca_command:filament_delete:orca"}, {"title", "Delete Filament Slot"}, {"group", "Filament"},
         {"description", "Delete a filament slot, as its Delete menu item does; objects on it move to another slot."}},
        {{"id", "orca_command:view_top:orca"}, {"title", "Top"}, {"group", "View"}, {"description", "Point the 3D camera: View: Top."}},
        {{"id", "orca_command:obj_delete:orca"}, {"title", "Delete Selected"}, {"group", "Object"},
         {"description", "Delete the selected objects or parts."}},
    });
    const auto ids = [](const json& ranked) {
        std::vector<std::string> out;
        for (const json& action : ranked)
            out.push_back(action["id"].get<std::string>());
        return out;
    };

    // No entry holds "delete object" as one text. Two are the Object group's deletes, which the Speed Dial
    // names by group and title, and the shorter name, the plainer action, leads; the third only
    // mentions objects.
    CHECK(ids(rank_actions(listed, query_words("delete object"))) ==
          std::vector<std::string>{"orca_command:obj_delete:orca", "orca_command:obj_delete_all:orca", "orca_command:filament_delete:orca"});
    // An entry matching more of the words in its name outranks a shorter one.
    CHECK(ids(rank_actions(listed, query_words("delete all"))).front() == "orca_command:obj_delete_all:orca");
    // "orca" and "command" are in every built-in id, so they find nothing by themselves.
    CHECK(rank_actions(listed, query_words("command")).empty());
    CHECK(rank_actions(listed, query_words("slice")).empty());
    // The entries come back whole.
    CHECK(rank_actions(listed, query_words("top")) == json::array({listed[2]}));
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
    const NativeCommand* shell = design.find("shell");
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
        int checked = 0;
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
    // The 129 Speed Dial entries outside the Design tab are commands' actions: eight own and 121 items.
    CHECK(check_actions(NativeCommands::instance()) == 129);

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
    CHECK(check_actions(design) > 129);
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
    CHECK_THAT(typo, Catch::Matchers::ContainsSubstring("action_search"));
    CHECK(typo_code == "invalid_argument");
#ifdef SLIC3R_CAD
    // A Design tab action, by its id or its key, while it is not shown: in a run with the CAD feature on at startup
    // tool_call runs its command from any tab (wrong_mode, the id being right), and in one with it off there is no such
    // command (design_unavailable).
    for (const bool cad : {true, false}) {
        CommandCatalog catalog;
        NativeCommands::register_all_commands(catalog, {cad});
        catalog.seal();
        for (const std::string id : {"orca_command:shell:orca", "shell"}) {
            INFO(id << (cad ? ", the CAD feature on" : ", the CAD feature off"));
            const auto [why, code] = refusal(id, catalog, cad);
            if (cad) {
                CHECK_THAT(why, Catch::Matchers::ContainsSubstring("while the Design tab is shown"));
                CHECK_THAT(why, Catch::Matchers::ContainsSubstring("tool_call {\"tool\": \"shell\"}"));
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
        CHECK_THAT(refusal("orca_command:describe_design:orca", catalog, cad).first, Catch::Matchers::ContainsSubstring("action_search"));
    }
    // The CAD feature turned off while the app runs drops every Design action, though their commands stay in the
    // catalog: the action is right, and a person turns the feature back on.
    CommandCatalog with_cad;
    NativeCommands::register_all_commands(with_cad, {true});
    with_cad.seal();
    const auto [off_now, off_now_code] = refusal("shell", with_cad, false);
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

TEST_CASE("a tool of the old vocabulary is no command", "[Mcp]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    REQUIRE(kRetiredTools.size() == 45);
    for (const std::string& retired : kRetiredTools) {
        INFO(retired);
        CHECK(catalog.find(retired) == nullptr);
    }

    // Each of the 42 under its name, listed, the first sentence of its description pinned (the commands that sentence
    // names under their names too).
    struct Renamed
    {
        const char* retired;
        const char* name;
        const char* first_sentence;
    };
    static const Renamed renamed[] = {
        {"search_tools", "tool_search", "Find what does something, in one search, or look up one command."},
        {"call_tool", "tool_call", "Call any command by name with its arguments: a tool, or one the tool list does not show, a menu item "
                                   "that takes no arguments, a Design tab command or an operation a plugin offers (see tool_search)."},
        {"generate_calibration_test", "calibration_generate_test",
         "Generate a Calibration menu test as its dialog's OK does: a new project named after the test with the test model, and the "
         "process, filament and printer presets changed as the test needs (presets_changed)."},
        {"export", "project_export", "Write the project to a file without a dialog."},
        {"list_presets", "preset_list",
         "Printer, process or filament presets, each with its name, alias (the name its selector shows, when another), system, user, "
         "project (kept inside the project), inherits, selected (and modified), compatible: false or installed: false when so, and "
         "vendor; a filament its filament_type and the slots using it (filament_slots), a printer its printer_model and nozzle."},
        {"select_preset", "preset_select", "Select a printer, process or filament preset by the name its selector shows (or its "
                                           "internal name), as a person would."},
        {"save_preset", "preset_save", "Save the printer, process or filament preset being edited, with its unsaved changes, as a preset "
                                       "called name, which is then selected: what the tab's Save button does through its dialog."},
        {"delete_preset", "preset_delete", "Delete a user preset or one inside the project, its file and its cloud copy, as the tab's "
                                           "Delete does, without asking."},
        {"list_plates", "plate_list",
         "Plates with name, locked, current, objects (how many are on it; object_list lists them), and the settings they print with: "
         "curr_bed_type, print_sequence, spiral_mode, first_layer_print_sequence and other_layers_print_sequence (null: Auto), with "
         "several extruders filament_map_mode and filament_map; overrides lists those the plate sets itself (plate_set_settings)."},
        {"list_objects", "object_list", "The project as the Objects panel lists it."},
        {"transform", "object_transform", "Change one instance's placement, or one part's with part."},
        {"add_part", "part_add", "Add a part, negative part, modifier, support blocker or enforcer, or precise seam to an object, as its "
                                 "context menu's Add items do: a primitive shape at a size, or a model file as Load..."},
        {"arrange", "object_arrange", "Arrange the objects of every unlocked plate as the toolbar's Arrange does, or of one plate "
                                      "(plates: an index or \"current\") as its Arrange icon does."},
        {"slice", "plate_slice", "Slice the current plate, all plates, or one plate by index, the way the toolbar's Slice button does, "
                                 "and wait for the result (or reply with a job id when it takes longer than the reply budget)."},
        {"set_height_ranges", "object_set_height_ranges",
         "Replace an object's height range modifiers (Height Range Modifier in the object list): Z ranges in mm, each printed with its "
         "own layer_height, filament and process settings, which settings_get and settings_set read and write with the target "
         "height_range."},
        {"set_variable_layer_height", "object_set_variable_layer_height",
         "Run the Variable layer height panel's Adaptive, Smooth or Reset on an object, without opening the panel: adaptive computes "
         "layer heights from the object's shape (inside a height range, the range's layer_height), smooth smooths its profile (or "
         "the one its height ranges give), reset goes back to its layer height and ranges."},
        {"set_custom_gcode_at_layer", "plate_set_custom_gcode",
         "Add or remove pauses, custom G-code, custom templates and filament changes at the start of a layer, as the Preview layer "
         "slider's menu does; they are saved per plate in the project."},
        {"get_slice_stats", "plate_slice_stats", "What the Preview shows for a sliced plate."},
        {"get_app_state", "app_state", "Snapshot of OrcaSlicer."},
        {"wait", "app_wait", "Block until a condition holds or timeout_s passes (max 50)."},
        {"get_log", "app_log", "The last lines of OrcaSlicer's current log file, optionally only those at or above a level."},
        {"cancel_job", "job_cancel", "Stop the running operation: a slice or G-code export, as the slicing progress's cancel button "
                                     "does, or a background job an MCP call started (an arrange, orient or fill bed)."},
        {"run_replay", "replay_run", "Replay a list of tool calls in order and report each step: {orca_replay_version:\"0.2.0\", name, "
                                     "vars, steps:[{tool, args, expect, optional, await, save_as, timeout_s}]}, JSON with comments."},
        {"export_replay", "replay_export",
         "Turn the calls this session made into a replay that replay_run runs: mutating calls, app_wait, window_screenshot and the "
         "recording tools, in order, with refs replaced by locators, job replies replayed with await:false, and window_screenshot "
         "and recording paths moved under ${out_dir}."},
        {"run_script", "script_run",
         "Run Python code inside OrcaSlicer as its main program (__name__ == \"__main__\") against orca.host, the whole plugin API "
         "(script_search_api searches it): read objects, meshes and settings, compute, use orca.host.ui and the preset API, and call "
         "these same tools, with their guards and undo, through orca.host.tools.call(name, args)."},
        {"search_python_api", "script_search_api",
         "Search orca, the Python API of script_run scripts and plugins, in the running build: orca.host (the project's model and "
         "meshes, presets and their editors, slicing data, orca.host.ui windows and dialogs, orca.host.tools calling these tools) and "
         "the classes, events and pages plugins build on."},
        {"answer_dialog", "dialog_answer", "Press a button of the topmost modal dialog: yes, no, ok, cancel, close, apply, a button's "
                                           "label, or a numeric id."},
        {"auto_answer_dialogs", "dialog_auto_answer", "Answer modal dialogs automatically when their title or message matches a rule's "
                                                      "regex (case-insensitive)."},
        {"select_objects", "object_select", "Select objects, instances or parts in the 3D view (the assemble view while it is shown, "
                                            "else Prepare, switching to it), as clicking them would."},
        {"screenshot", "window_screenshot",
         "Capture what OrcaSlicer shows and return it as an image, downscaled to max_dim on the long edge (default 1280), plus the "
         "full-size PNG written to save_path (or the session's output folder)."},
        {"set_view", "view_set",
         "Set up what the window shows, for screenshots and GUI work: the main tab, the 3D view (canvas: Prepare's, the preview or the "
         "assemble view, which has its own gizmos), the camera of the 3D view shown (a named direction, the default view, or fit to "
         "bed / objects / selection, then look_at, the point it looks at, an orbit to look from between the named directions, and the "
         "zoom), the preview's color scheme (picked as in the legend's combo, which shows every feature type again), legend rows, "
         "time estimate and layers (1-based, or up to a height)."},
        {"encode_recording", "recording_encode", "Encode a recording into an H.265 (or H.264) MP4 with the system ffmpeg (ORCA_FFMPEG, "
                                                 "else ffmpeg on PATH; it needs libx265 or libx264), keeping its timing."},
        {"search_actions", "action_search", "Search everything the Speed Dial can run: built-in commands (slice, export, views, object "
                                            "operations, plates, calibration wizards, help), recent projects, plugin scripts and pages."},
        {"run_action", "action_run", "Run one action by the id action_search returned (orca_command:view_front:orca, or the name "
                                     "inside it, view_front), with args, the command's arguments, when it takes some (its "
                                     "input_schema)."},
        {"list_windows", "window_list",
         "List OrcaSlicer's open windows: the main window, then the windows that don't block the app (Plugins, an object's settings, "
         "a dock panel floating on its own), then the modal dialogs in the order they opened, the topmost last."},
        {"set_window", "window_set", "Size, move, maximize, minimize or raise a window: the main window (default), the topmost modal "
                                     "dialog, or one window_list lists, also while a modal dialog is open."},
        {"list_widgets", "widget_list",
         "Describe what is on screen as an indented tree, one element per line with a ref to pass to widget_click, widget_set_value, "
         "keyboard_press and webview_run_javascript (a page element's to mouse_gesture too): the widgets of every shown window, the "
         "rows of lists, trees and tables, each shown popup after the window it opened from, the main menus, the 3D canvas's "
         "toolbar items and gizmos, each ImGui panel drawn in the last frame with its items and text, and the elements of each shown "
         "web page (dialogs, the setup wizard, sign-in, plugin windows) under its web view."},
        {"click", "widget_click", "Click a GUI element as a person would."},
        {"set_widget_value", "widget_set_value", "Set a widget's value and send the events a person's edit would."},
        {"press_keys", "keyboard_press",
         "Send a key chord (\"Ctrl+S\", \"Escape\", \"Return\", \"Delete\", \"Shift+Tab\", \"Left\", \"F5\") and/or type text to a "
         "target or, without one, to the focused window (the topmost dialog while one is open, the shown 3D view when nothing has "
         "focus)."},
        {"pick_gizmo_file", "gizmo_pick_file",
         "Do what a file picker inside the open gizmo does, with a path instead of its dialog: svg_change loads another SVG file into "
         "the SVG part being edited, svg_save_as writes that part's SVG to a new file (an existing one only with overwrite), "
         "texture_import imports a height-map image into the texture library and puts it on Texture Displacement's active layer."},
        {"run_javascript", "webview_run_javascript", "Evaluate JavaScript in one of OrcaSlicer's web views (Home, the device page, "
                                                     "dialogs, plugin windows)."},
    };
    REQUIRE(std::size(renamed) == 42);
    for (const Renamed& r : renamed) {
        INFO(r.retired << " is " << r.name);
        CHECK(std::find(kRetiredTools.begin(), kRetiredTools.end(), r.retired) != kRetiredTools.end());
        const NativeCommand* command = catalog.find(r.name);
        REQUIRE(command != nullptr);
        CHECK(command->listed);
        CHECK(first_sentence(command->description) == r.first_sentence);
    }

    // Outside the Design group 132 commands are listed: every one that runs from its arguments, all but the three menu
    // items that take none (Task 3's 98, the seven Parts 4a to 4c add, Part 5a's three, Part 5b's two, Part 5c's one,
    // Part 5d's four, the seven gizmo commands, the six printing commands and the filament list's four).
    std::size_t listed = 0;
    for (const NativeCommand& c : catalog.commands())
        if (c.listed && c.group != Slic3r::GUI::kDesignGroup)
            ++listed;
    CHECK(listed == 132);
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

TEST_CASE("script_search_api's reference covers the orca package with constructors and named instances", "[Mcp][Python]")
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

    CHECK(rank_python_api(reference, "mesh normals")[0]["name"] == "orca.host.TriangleMesh.face_normals");
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

TEST_CASE("tool_search reads one inventory", "[Mcp]")
{
    const auto command = [](const char* key, const char* group, bool listed, const char* description) {
        NativeCommand c;
        c.key          = key;
        c.group        = group;
        c.listed       = listed;
        c.description  = description;
        c.input_schema = object_schema({{"a", prop("number", "A number.")}});
        return c;
    };
    CommandCatalog catalog;
    catalog.add(command("measure_bed", "Plugins", true, "Measure the bed. In mm."));
    NativeCommand measure  = command("measure_action", "View", false, "Measure what the view shows.");
    measure.has_own_action = true;
    catalog.add(measure);
    catalog.add_source(
        std::make_unique<OneCommandSource>(command("plugin.fixture.measure", "Plugins", false, "Offered by a plugin. Doubles a.")));
    catalog.seal();

    const json found = search_commands(catalog, {{"query", "measure"}});
    CHECK(found["total"] == 3);
    std::map<std::string, json> items;
    for (const json& item : found["items"])
        items[item["name"].get<std::string>()] = item;
    REQUIRE(items.size() == 3);
    // A listed command's schema is in the tool list; the others' are nowhere else.
    CHECK(items["measure_bed"]["listed"] == true);
    CHECK(items["measure_bed"]["summary"] == "Measure the bed.");
    CHECK_FALSE(items["measure_bed"].contains("input_schema"));
    for (const char* unlisted : {"measure_action", "plugin.fixture.measure"}) {
        INFO(unlisted);
        CHECK(items[unlisted]["listed"] == false);
        CHECK(items[unlisted]["input_schema"]["properties"].contains("a"));
    }
    // An operation's first sentence only says a plugin offers it.
    CHECK(items["plugin.fixture.measure"]["description"] == "Offered by a plugin. Doubles a.");
    CHECK(found["use"].get<std::string>().find("tool_call {tool, args}") != std::string::npos);

    // {tool} describes any of them.
    const json described = search_commands(catalog, {{"tool", "measure_action"}});
    CHECK(described["listed"] == false);
    CHECK(described["input_schema"]["properties"].contains("a"));
    // A name no command has is the call's argument to correct.
    try {
        search_commands(catalog, {{"tool", "measure_nothing"}});
        FAIL("an unknown tool was not refused");
    } catch (const ToolFailure& f) {
        CHECK(f.details.value("code", std::string()) == "invalid_argument");
    }

    // group narrows the names and the matches, in any case, a source's commands included.
    CHECK(search_commands(catalog, json::object())["names"] == json::array({"measure_bed", "measure_action", "plugin.fixture.measure"}));
    CHECK(search_commands(catalog, {{"group", "PLUGINS"}})["names"] == json::array({"measure_bed", "plugin.fixture.measure"}));
    CHECK(search_commands(catalog, {{"group", "view"}})["names"] == json::array({"measure_action"}));
    const json in_view = search_commands(catalog, {{"group", "View"}, {"query", "measure"}});
    REQUIRE(in_view["items"].size() == 1);
    CHECK(in_view["items"][0]["name"] == "measure_action");
    // A group no command has is refused with the groups there are.
    try {
        search_commands(catalog, {{"group", "commands"}});
        FAIL("an unknown group was not refused");
    } catch (const ToolFailure& f) {
        CHECK(std::string(f.what()).find("Unknown group: commands") != std::string::npos);
        CHECK(f.details["groups"] == json::array({"Plugins", "View"}));
        CHECK(f.details.value("code", std::string()) == "invalid_argument");
    }
}

TEST_CASE("tool_search's groups are the places' headings and Design", "[Mcp]")
{
    CommandCatalog catalog;
    NativeCommands::register_all_commands(catalog, {true});
    catalog.seal();
    const auto names_of = [&catalog](const char* group) {
        const json               found = search_commands(catalog, {{"group", group}});
        std::vector<std::string> names;
        for (const json& name : found["names"])
            names.push_back(name.get<std::string>());
        return names;
    };

    // A place's heading, in any case: its commands alone.
    const std::vector<std::string> objects = names_of("Objects");
    REQUIRE_FALSE(objects.empty());
    for (const std::string& name : objects) {
        INFO(name);
        CHECK(name.rfind("object_", 0) == 0);
    }
    CHECK(names_of("objects") == objects);
    // Every Design tab command, as the group has been.
    const std::vector<std::string> design = names_of("Design");
    for (const NativeCommand& c : catalog.commands())
        if (c.group == Slic3r::GUI::kDesignGroup) {
            INFO(c.key);
            CHECK(std::count(design.begin(), design.end(), c.key) == 1);
        }
    CHECK(design.size() == 88);
    // The groups of how a tool acts are no group.
    for (const char* gone : {"api", "core", "ui", "Commands"}) {
        INFO(gone);
        try {
            search_commands(catalog, {{"group", gone}});
            FAIL("an unknown group was not refused");
        } catch (const ToolFailure& f) {
            CHECK_THAT(std::string(f.what()), Catch::Matchers::ContainsSubstring(std::string("Unknown group: ") + gone));
        }
    }
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

TEST_CASE("tool_search finds a Design command by the name of a menu item folded into it", "[Mcp][CAD]")
{
    CommandCatalog catalog;
    NativeCommands::register_all_commands(catalog, {true});
    catalog.seal();
    for (const auto& [query, command] : {std::pair<const char*, const char*>{"rounded rectangle", "sketch_draw"},
                                         std::pair<const char*, const char*>{"polar array", "sketch_pattern"}}) {
        INFO(query);
        const json found = search_commands(catalog, {{"query", query}});
        REQUIRE_FALSE(found["items"].empty());
        CHECK(found["items"][0]["name"] == command);
    }
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

    // A Wayland compositor that refused to activate a minimized window: the note says so and how the person
    // brings it back, not that the window manager was slow.
    WindowSettling refused;
    refused.activation = true;
    reply              = {{"size", {800, 600}}, {"state", "minimized"}};
    add_window_notes({{"state", "normal"}}, refused, reply);
    CHECK(reply["adjusted"] == json::array({"state"}));
    CHECK(note(reply).find("the compositor did not bring the window back") != std::string::npos);
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
    Slic3r::ModelObject* first  = model.add_object();
    Slic3r::ModelObject* second = model.add_object();
    first->name                 = "bracket";
    second->name                = "lid";
    Slic3r::ModelVolume* part   = first->add_volume(Slic3r::TriangleMesh());

    CHECK_NOTHROW(check_expected_ids(*first, nullptr, json{{"object", 0}, {"object_id", first->id().id}}));
    CHECK_NOTHROW(check_expected_ids(*first, part, json{{"object", 0}, {"part_id", part->id().id}}));
    // Without expected ids the index alone decides, as before.
    CHECK_NOTHROW(check_expected_ids(*second, nullptr, json{{"object", 0}}));
    try {
        check_expected_ids(*second, nullptr, json{{"object", 0}, {"object_id", first->id().id}});
        FAIL("the call was not refused");
    } catch (const ToolFailure& f) {
        CHECK(std::string(f.what()).find("lid") != std::string::npos);
        CHECK(f.details["id"] == second->id().id);
    }
    CHECK_THROWS_AS(check_expected_ids(*first, part, json{{"part_id", part->id().id + 1000}}), ToolFailure);
}

TEST_CASE("the journal records indices without expected ids, except a Python step's arguments", "[Mcp]")
{
    const CommandCatalog& catalog = Slic3r::GUI::NativeCommands::instance();
    CHECK(without_expected_ids(catalog, "settings_set", json{{"object", 0}, {"object_id", 7}, {"part_id", 8}}) == json{{"object", 0}});
    CHECK(without_expected_ids(catalog, "object_set_height_ranges", json{{"object", 0}, {"object_id", 7}}) == json{{"object", 0}});
    CHECK(without_expected_ids(catalog, "object_set_variable_layer_height", json{{"object", 0}, {"object_id", 7}}) == json{{"object", 0}});
    const json replay{{"replay", {{"orca_replay_version", "0.2.0"},
                                  {"steps", {{{"tool", "object_transform"}, {"args", {{"object", 1}, {"object_id", 9}}}},
                                             {{"tool", "script_run"}, {"args", {{"code", "x"}, {"args", {{"object_id", 9}}}}}}}}}}};
    const json recorded = without_expected_ids(catalog, "replay_run", replay);
    CHECK(recorded["replay"]["steps"][0]["args"] == json{{"object", 1}});
    CHECK(recorded["replay"]["steps"][1]["args"]["args"]["object_id"] == 9);
    CHECK(without_expected_ids(catalog, "script_run", json{{"code", "x"}, {"args", {{"object_id", 3}}}})["args"]["object_id"] == 3);
}

TEST_CASE("the journal records an inline replay given as JSON text as its object, without expected ids", "[Mcp]")
{
    const CommandCatalog& catalog = Slic3r::GUI::NativeCommands::instance();
    const json as_text{{"replay", "// moves the lid\n"
                                  R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "object_transform", "args": {"object": 1, "object_id": 9}}]})"}};
    const json recorded = without_expected_ids(catalog, "replay_run", as_text);
    REQUIRE(recorded["replay"].is_object());
    CHECK(recorded["replay"]["steps"][0]["args"] == json{{"object", 1}});
    // Text that doesn't parse is the call's error; it is recorded as given.
    CHECK(without_expected_ids(catalog, "replay_run", json{{"replay", "{\"orca_replay_version\": \"0.2.0\","}}) == json{{"replay", "{\"orca_replay_version\": \"0.2.0\","}});
}

TEST_CASE("the journal strips a command's id arguments", "[Mcp]")
{
    const CommandCatalog& catalog = Slic3r::GUI::NativeCommands::instance();
    CHECK(without_expected_ids(catalog, "object_transform", json{{"object", 0}, {"object_id", 7}}) == json{{"object", 0}});
    // Inside the tool_call and replay_run wrappers too.
    CHECK(without_expected_ids(catalog, "tool_call", json{{"tool", "object_transform"}, {"args", {{"object", 1}, {"object_id", 9}}}}) ==
          json{{"tool", "object_transform"}, {"args", {{"object", 1}}}});
    const json replay{{"replay", {{"orca_replay_version", "0.2.0"},
                                  {"steps", {{{"tool", "tool_call"}, {"args", {{"tool", "object_transform"}, {"args", {{"object", 1}, {"object_id", 9}}}}}}}}}}};
    CHECK(without_expected_ids(catalog, "replay_run", replay)["replay"]["steps"][0]["args"]["args"] == json{{"object", 1}});
    // A plugin operation's own fields of the same names are its arguments, recorded as given.
    CHECK(without_expected_ids(catalog, "plugin.measure.area", json{{"object_id", 4}}) == json{{"object_id", 4}});
    CHECK(without_expected_ids(catalog, "tool_call", json{{"tool", "plugin.measure.area"}, {"args", {{"object_id", 4}}}})["args"]["object_id"] == 4);
    CHECK(without_expected_ids(catalog, "object_select", json{{"objects", {0, 1}}, {"object_ids", {5, 6}}}) == json{{"objects", {0, 1}}});
    CHECK(without_expected_ids(catalog, "object_select", json{{"instances", {{0, 1}}}, {"instance_object_ids", {5}}}) ==
          json{{"instances", {{0, 1}}}});
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

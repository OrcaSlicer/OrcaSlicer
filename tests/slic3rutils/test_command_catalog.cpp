#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "automation/automation_test_utils.hpp"
#include "mcp_test_utils.hpp"

#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpPersonContext.hpp"
#include "slic3r/GUI/Mcp/McpProtocol.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// The catalog's cases that need the GUI library: the process catalog NativeCommands builds, the commands its
// helpers make, and a person's context. The others are in tests/automation/test_command_catalog.cpp.

using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
using Slic3r::GUI::AppActionRunResult;
using Slic3r::GUI::CommandCatalog;
using Slic3r::GUI::CommandCompletion;
using Slic3r::GUI::CommandFn;
using Slic3r::GUI::GuardClass;
using Slic3r::GUI::NativeCommand;
using Slic3r::GUI::ToolCaller;
using Slic3r::GUI::ToolContext;
using Slic3r::GUI::ToolFailure;
using Slic3r::GUI::ToolResult;
using Slic3r::GUI::Mcp::Test::catalog_with_cad;
using Slic3r::GUI::Mcp::Test::command;
using Slic3r::GUI::Mcp::Test::keys_of;
using Slic3r::GUI::Mcp::Test::OneCommandSource;
using Slic3r::GUI::Mcp::Test::TestContext;
namespace Mcp            = Slic3r::GUI::Mcp;
namespace NativeCommands = Slic3r::GUI::NativeCommands;
namespace Ops            = Slic3r::GUI::Ops;

namespace {

json no_arguments() { return {{"type", "object"}, {"properties", json::object()}}; }

// The entry of command `key` in a reference, or null.
json reference_entry(const json& reference, const std::string& key)
{
    for (const json& entry : reference.at("commands"))
        if (entry.at("key") == key)
            return entry;
    return nullptr;
}

// The lines of `text`, split at "\n".
std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream       in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

// Where `actual` first differs from `expected`, for a message: the line and both versions of it; empty when they
// are equal.
std::string first_difference(const std::string& actual, const std::string& expected)
{
    if (actual == expected)
        return {};
    const std::vector<std::string> a = lines_of(actual), e = lines_of(expected);
    for (std::size_t i = 0; i < std::min(a.size(), e.size()); ++i)
        if (a[i] != e[i])
            return "line " + std::to_string(i + 1) + " is `" + a[i] + "` where the catalog has `" + e[i] + "`";
    if (a.size() == e.size())
        return "the file does not end with the newline the catalog's dump ends with";
    return "the file has " + std::to_string(a.size()) + " lines where the catalog has " + std::to_string(e.size());
}

// The groups of `keys`' commands in `catalog`, each run of one group once, in their order.
std::vector<std::string> group_runs(const CommandCatalog& catalog, const std::vector<std::string>& keys)
{
    std::vector<std::string> runs;
    for (const std::string& key : keys)
        if (const NativeCommand* c = catalog.find(key); c != nullptr && (runs.empty() || runs.back() != c->group))
            runs.push_back(c->group);
    return runs;
}

// Whether `key` is named as a read: one of its words, between underscores, says what the command reads or waits for.
bool named_as_read(const std::string& key)
{
    static const std::set<std::string> kReadWords{"list", "get",  "describe", "state",     "status",
                                                  "stats", "log", "wait",     "screenshot"};
    for (std::size_t start = 0; start <= key.size();) {
        const std::size_t end = std::min(key.find('_', start), key.size());
        if (kReadWords.count(key.substr(start, end - start)) != 0)
            return true;
        start = end + 1;
    }
    return false;
}

} // namespace

TEST_CASE("the process catalog gives the Speed Dial its commands' actions", "[CommandCatalog]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    CHECK(catalog.sealed());
    const std::vector<NativeCommands::SpeedDialAction> speed_dial = NativeCommands::speed_dial_actions(catalog);
    CHECK_FALSE(speed_dial.empty());
    for (const NativeCommands::SpeedDialAction& t : speed_dial) {
        REQUIRE(t.command != nullptr);
        INFO(t.command->key);
        // A command's own action, or an item of its actions.
        if (t.action == nullptr)
            CHECK(t.command->has_own_action);
        else
            CHECK(t.command->actions.is_array());
    }
    // The Speed Dial's own lookup reads the same catalog.
    CHECK(NativeCommands::find("plate_slice") == catalog.find("plate_slice"));
}

TEST_CASE("every action is found by its key and runs its command with arguments that fit its schema", "[CommandCatalog][SpeedDial]")
{
    // Which command each action folds into, and the arguments it runs it with, are in the command reference
    // (tests/data/command_catalog.json); here every action is checked to be a call its command takes.
    const CommandCatalog& catalog = NativeCommands::instance();
    for (const NativeCommands::SpeedDialAction& t : NativeCommands::speed_dial_actions(catalog)) {
        REQUIRE(t.command != nullptr);
        const NativeCommand& command = *t.command;
        const std::string    key     = t.action != nullptr ? t.action->at("key").get<std::string>() : command.key;
        INFO(key << " -> " << command.key);
        const auto [found, action] = Slic3r::GUI::find_command_action(catalog, key);
        CHECK(found == t.command);
        CHECK(action == t.action);
        if (t.action == nullptr) {
            // The command's own action, a person's form of it.
            CHECK(command.has_own_action);
            CHECK(Mcp::person_form(command, json::object()));
            json none = Slic3r::GUI::action_call_args(nullptr, json::object());
            CHECK(Mcp::argument_error(command, none).empty());
            continue;
        }
        const std::string input = t.action->value("input", std::string());
        // An action that collects nothing is a person's form of its command; one that collects a value is not, as its
        // value comes from the Speed Dial's second phase.
        CHECK(Mcp::person_form(command, t.action->at("args")) == input.empty());
        // What a pick runs the command with: the action's arguments, and the value its second phase collects.
        json value = json::object();
        if (!input.empty())
            value[input] = input == "percent" ? json(50) : json("preview");
        json call = Slic3r::GUI::action_call_args(t.action, value);
        CHECK(Mcp::argument_error(command, call).empty());
    }

    // The actions that ask need none of what the silent form requires; one more argument is the silent form.
    const NativeCommand* save   = catalog.find("project_save");
    const NativeCommand* export_ = catalog.find("project_export");
    const NativeCommand* import_ = catalog.find("project_import");
    REQUIRE(save != nullptr);
    REQUIRE(export_ != nullptr);
    REQUIRE(import_ != nullptr);
    json save_as{{"save_as", true}};
    CHECK(Mcp::argument_error(*save, save_as).empty());
    json gcode{{"format", "gcode"}};
    CHECK(Mcp::argument_error(*export_, gcode).empty());
    json archive{{"archive", true}};
    CHECK(Mcp::argument_error(*import_, archive).empty());
    json overwrite{{"overwrite", true}};
    CHECK_THAT(Mcp::argument_error(*save, overwrite), ContainsSubstring("path"));
    json gcode_overwrite{{"format", "gcode"}, {"overwrite", true}};
    CHECK_THAT(Mcp::argument_error(*export_, gcode_overwrite), ContainsSubstring("path"));
}

TEST_CASE("a menu item's action runs its command with the item's arguments", "[SpeedDial]")
{
    // One action of each command that has actions, and each of view_set's kinds of action: an action's key, its command,
    // the arguments it runs it with, and the Speed Dial's token for the value it collects with the argument that takes it.
    struct Pair
    {
        const char* key;
        const char* command;
        json        args;
        const char* input     = "";
        const char* input_arg = "";
    };
    const std::vector<Pair> pairs{
        {"new_project", "project_new", json::object()},
        {"load_project", "project_open", json::object()},
        {"import_zip_archive", "project_import", {{"archive", true}}},
        {"save_project_as", "project_save", {{"save_as", true}}},
        {"export_stl_multi", "project_export", {{"format", "stl"}, {"per_object", true}}},
        {"select_all", "object_select", {{"all", true}}},
        {"add_primitive_torus", "object_add_primitive", {{"shape", "torus"}}},
        {"add_handy_orca_cube", "object_add_handy_model", {{"model", "orca_cube"}}},
        {"add_primitive_text", "object_add_text", json::object()},
        {"add_primitive_svg", "object_add_svg", json::object()},
        {"obj_delete_all", "object_delete", {{"all", true}}},
        {"obj_rename", "object_rename", json::object()},
        {"obj_instances_down", "object_set_instances", {{"delta", -1}}},
        {"obj_instances_to_objects", "object_separate_instances", json::object()},
        {"obj_split_parts", "object_split", {{"into", "parts"}}},
        {"obj_assemble", "object_assemble", json::object()},
        {"obj_order_by_height", "object_order", {{"by", "height"}}},
        {"obj_drop", "object_drop", json::object()},
        {"obj_mirror_y", "object_mirror", {{"axis", "y"}}},
        {"obj_center", "object_center", json::object()},
        {"obj_fit_volume", "object_scale_to_fit", json::object()},
        {"obj_arrange", "object_arrange", json::object()},
        {"obj_orient", "object_auto_orient", json::object()},
        {"obj_type_support_enforcer", "part_change_type", {{"type", "support_enforcer"}}},
        {"plate_add", "plate_add", json::object()},
        {"plate_delete", "plate_delete", json::object()},
        {"plate_duplicate", "plate_duplicate", json::object()},
        {"plate_rename", "plate_rename", json::object()},
        {"plate_toggle_lock", "plate_lock", json::object()},
        {"plate_goto", "plate_select", {{"plate", 0}}},
        {"slice_all", "plate_slice", {{"plates", "all"}, {"preview", true}}},
        {"calib_pressure_advance", "calibration_generate_test", {{"test", "pressure_advance"}}},
        {"import_configs", "preset_import", json::object()},
        {"export_config", "preset_export", json::object()},
        {"sync_presets", "preset_sync", json::object()},
        {"help_migrate_filament_presets", "preset_migrate_filaments", json::object()},
        {"mode_expert", "settings_set_mode", {{"mode", "expert"}}},
        {"toggle_developer_mode", "settings_toggle_developer_mode", json::object()},
        {"undo", "undo", json::object()},
        {"redo", "redo", json::object()},
        {"install_plugin", "app_open_web_page", {{"page", "plugin_hub"}}},
        {"help_check_updates", "app_check_for_updates", json::object()},
        {"help_open_config_folder", "app_show_config_folder", json::object()},
        {"help_benchmark_3d_scene", "app_benchmark_3d_scene", json::object()},
        {"view_top", "view_set", {{"camera", "top"}}},
        {"view_toggle_perspective", "view_set", {{"projection", "toggle"}}},
        {"reset_window_layout", "view_set", {{"layout", "default"}}},
        {"go_to_tab", "view_set", json::object(), "tab"},
        {"go_to_layer", "view_set", json::object(), "percent", "layer_percent"},
        {"open_preferences", "window_open", {{"window", "preferences"}}},
        {"take_screenshot", "window_save_screenshot", json::object()},
        {"filament_add", "filament_add", json::object()},
        {"filament_delete", "filament_delete", json::object()},
        {"sync_ams", "filament_sync_ams", json::object()},
        {"send_gcode", "printer_send", json::object()},
        {"install_local_plugin", "plugin_install_local", json::object()},
        {"refresh_plugins", "plugin_refresh", json::object()},
        {"start_recording", "recording_start", json::object()},
        {"stop_recording", "recording_stop", json::object()},
        {"toggle_run_script_panel", "script_toggle_panel", json::object()},
    };
    const CommandCatalog& catalog = NativeCommands::instance();
    for (const Pair& p : pairs) {
        INFO(p.key << " -> " << p.command << " " << p.args.dump());
        const auto [command, command_action] = Slic3r::GUI::find_command_action(catalog, p.key);
        REQUIRE(command != nullptr);
        CHECK(command->key == p.command);
        // Its AppAction runs its command, with what action_call_args() makes of a pick that collects nothing.
        const std::unique_ptr<Slic3r::GUI::AppAction> action = command_action != nullptr ?
                                                                   NativeCommands::make_action(*command, *command_action) :
                                                                   NativeCommands::make_action(*command);
        CHECK(action->command == p.command);
        CHECK(Slic3r::GUI::action_call_args(command_action, json::object()) == p.args);
        CHECK(action->input == p.input);
        if (command_action != nullptr)
            CHECK(command_action->value("input_arg", std::string()) == p.input_arg);
        // The command's own action is the one whose key is the command's name.
        CHECK((command_action == nullptr) == (std::string(p.key) == p.command));
    }
}

TEST_CASE("every command outside the Design tab is under the heading its name gives", "[CommandCatalog]")
{
    std::set<std::string> headings;
    for (const NativeCommand& c : NativeCommands::instance().commands()) {
        if (c.group == Slic3r::GUI::kDesignGroup)
            continue;
        INFO(c.key);
        // A command whose name starts with no place has no heading.
        const char* heading = NativeCommands::place_heading(c.key);
        REQUIRE(heading != nullptr);
        CHECK(c.group == heading);
        headings.insert(c.group);
    }
    // Every place has commands, and there is no other.
    const std::vector<std::string> places = NativeCommands::place_headings();
    CHECK(headings == std::set<std::string>(places.begin(), places.end()));

    // tool_list's group says which there are.
    const NativeCommand* list = NativeCommands::find("tool_list");
    REQUIRE(list != nullptr);
    const std::string group = list->input_schema.at("properties").at("group").at("description").get<std::string>();
    for (const std::string& heading : places)
        CHECK_THAT(group, ContainsSubstring(heading));
    CHECK_THAT(group, ContainsSubstring(Slic3r::GUI::kDesignGroup));

    // The screenshot commands are under Recordings, replays and scripts, apart from the other window commands.
    const auto heading_of = [](const char* key) {
        const char* heading = NativeCommands::place_heading(key);
        return heading != nullptr ? std::string(heading) : std::string("(none)");
    };
    CHECK(heading_of("window_screenshot") == "Recordings, replays and scripts");
    CHECK(heading_of("window_save_screenshot") == "Recordings, replays and scripts");
    CHECK(heading_of("window_list") == "View and windows");
    // undo and redo are a place's word of their own; a menu item's id and a word that is no place have none.
    CHECK(heading_of("undo") == "The app");
    CHECK(heading_of("obj_mirror_x") == "(none)");
    CHECK(heading_of("objects_list") == "(none)");
    CHECK(heading_of("") == "(none)");
}

TEST_CASE("each place's file adds only its place's commands", "[CommandCatalog]")
{
    // Each file under Mcp/Tools/ holds one place's commands (ObjectTools.cpp two); the catalog groups a command by its
    // key wherever it is added, so only this keeps a command out of another place's file.
    struct PlaceFile
    {
        const char*           file;
        void                  (*add)(CommandCatalog&);
        std::set<std::string> headings;
    };
    const std::vector<PlaceFile> files{
        {"CatalogTools.cpp", [](CommandCatalog& c) { Mcp::register_catalog_tools(c, ""); }, {"Tools and actions"}},
        {"AppTools.cpp", [](CommandCatalog& c) { Mcp::register_app_tools(c, false); }, {"The app"}},
        {"ProjectTools.cpp", Mcp::register_project_tools, {"The project"}},
        {"ObjectTools.cpp", Mcp::register_object_tools, {"Objects", "Parts"}},
        {"PlateTools.cpp", Mcp::register_plate_tools, {"Plates"}},
        {"PresetTools.cpp", Mcp::register_preset_tools, {"Presets and settings"}},
        {"FilamentTools.cpp", Mcp::register_filament_tools, {"Filaments"}},
        {"PrinterTools.cpp", Mcp::register_printer_tools, {"Printers and prints"}},
        {"PluginTools.cpp", Mcp::register_plugin_tools, {"Plugins"}},
        {"ViewTools.cpp", Mcp::register_view_tools, {"View and windows"}},
        {"RecordingTools.cpp", Mcp::register_recording_tools, {"Recordings, replays and scripts"}},
        {"GuiTools.cpp", Mcp::register_gui_tools, {"Driving the GUI"}},
    };
    std::set<std::string> added;
    for (const PlaceFile& place : files) {
        INFO(place.file);
        CommandCatalog catalog;
        place.add(catalog);
        CHECK_FALSE(catalog.commands().empty());
        for (const NativeCommand& c : catalog.commands()) {
            INFO(c.key);
            const char* heading = NativeCommands::place_heading(c.key);
            REQUIRE(heading != nullptr);
            CHECK(place.headings.count(heading) == 1);
            CHECK(added.insert(c.key).second);
        }
    }
    // Together the files add every command outside the Design tab.
    std::set<std::string> expected;
    for (const NativeCommand& c : NativeCommands::instance().commands())
        if (c.group != Slic3r::GUI::kDesignGroup)
            expected.insert(c.key);
    CHECK(added == expected);
}

TEST_CASE("every command that runs from arguments is in the tool list, by place", "[CommandCatalog]")
{
    const CommandCatalog&    catalog = NativeCommands::instance();
    std::vector<std::string> listed;
    for (const NativeCommand& c : catalog.commands()) {
        if (c.group == Slic3r::GUI::kDesignGroup)
            continue;
        if (c.listed) {
            listed.push_back(c.key);
            continue;
        }
        // A command tools/list leaves out is a menu item with no argument form, which tool_list lists and tool_call
        // runs: it takes no arguments and is its one item's command.
        INFO(c.key);
        const json properties = c.input_schema.value("properties", json::object());
        CHECK(properties.empty());
        CHECK_FALSE(c.has_own_action);
        REQUIRE(c.actions.is_array());
        CHECK(c.actions.size() == 1);
    }

    // The listing of the other commands, the manual, the generic call and the app's state first; then each heading's
    // commands together, in the places' order (NativeCommands::place_headings()), Driving the GUI last.
    REQUIRE(listed.size() > 4);
    const std::vector<std::string> first{"tool_list", "tool_describe", "tool_call", "app_state"};
    CHECK(std::vector<std::string>(listed.begin(), listed.begin() + 4) == first);
    const std::vector<std::string> places = NativeCommands::place_headings();
    REQUIRE_FALSE(places.empty());
    CHECK(places.back() == "Driving the GUI");
    CHECK(group_runs(catalog, std::vector<std::string>(listed.begin() + 4, listed.end())) == places);

    // With the Design tab, its commands sit between the last of the places and Driving the GUI.
    const CommandCatalog with_design = catalog_with_cad();
    const std::vector<std::string> keys = keys_of(with_design.commands());
    REQUIRE(keys.size() > 4);
    std::vector<std::string> with_design_headings = places;
    with_design_headings.insert(with_design_headings.end() - 1, Slic3r::GUI::kDesignGroup);
    CHECK(group_runs(with_design, std::vector<std::string>(keys.begin() + 4, keys.end())) == with_design_headings);
}

TEST_CASE("no command searches by words, and none sends an agent to a search", "[CommandCatalog]")
{
    // An agent reads lists and picks from them: the query a command takes is a name filter, which keeps the entries
    // holding the text as written.
    std::set<std::string> with_query;
    for (const NativeCommand& c : NativeCommands::instance().commands())
        if (c.input_schema.contains("properties") && c.input_schema["properties"].contains("query"))
            with_query.insert(c.key);
    CHECK(with_query == std::set<std::string>{"preset_list", "printer_list_models", "widget_list"});
    // No description, the parameters' included, names a search that is gone.
    for (const NativeCommand& c : NativeCommands::instance().commands()) {
        INFO(c.key);
        const std::string text = c.description + ' ' + c.input_schema.dump();
        for (const char* gone : {"tool_search", "action_search", "script_search_api"})
            CHECK(text.find(gone) == std::string::npos);
        if (c.key == "settings_get" || c.key == "settings_list" || c.key == "preferences_get")
            CHECK(text.find("query") == std::string::npos);
    }
}

TEST_CASE("only deleting a preset, uninstalling a plugin and stopping a print are labelled destructive", "[CommandCatalog]")
{
    // A read changes nothing in the app or the project, and its name says so (named_as_read()). Take Screenshot's
    // command is named as one but saves a PNG and shows a notification; an agent's screenshot is window_screenshot.
    const std::set<std::string> named_as_read_but_not{"window_save_screenshot"};
    // The commands made from menu items whose item reaches a printer, the cloud account or the network.
    const std::set<std::string> menu_items_reaching_out{"printer_send",          "preset_sync",       "filament_sync_ams",
                                                        "app_check_for_updates", "app_open_web_page", "plugin_refresh"};
    // The plugin commands that run a plugin's code or reach the cloud account: a plugin switched on or loaded again
    // runs its code, a cloud plugin is downloaded, and Unsubscribe ends a subscription.
    const std::set<std::string> plugins_reaching_out{"plugin_enable", "plugin_reload", "plugin_uninstall"};
    // The printing commands: the Print button's dialogs print on a printer, the host test asks the print host, and the
    // job commands send the printer a command. printer_status reads what the app holds and reaches nothing.
    const std::set<std::string> printing_reaching_out{"print_start", "printer_test_host", "print_pause", "print_resume", "print_stop"};
    std::set<std::string> destructive;
    for (const NativeCommand& c : NativeCommands::instance().commands()) {
        if (c.group == Slic3r::GUI::kDesignGroup)
            continue;
        INFO(c.key);
        if (c.destructive)
            destructive.insert(c.key);
        const bool read = named_as_read(c.key) && named_as_read_but_not.count(c.key) == 0;
        CHECK(c.read_only == read);
        if (c.listed) {
            CHECK_FALSE(c.description.empty());
            CHECK(c.input_schema.value("type", std::string()) == "object");
        }
    }
    CHECK(destructive == std::set<std::string>{"preset_delete", "plugin_uninstall", "print_stop"});
    // An exception is a command the rule would call a read, so a renamed or removed one cannot stay listed.
    for (const std::string& key : named_as_read_but_not) {
        INFO(key);
        CHECK(NativeCommands::find(key) != nullptr);
        CHECK(named_as_read(key));
    }
    std::set<std::string> reaching_out = menu_items_reaching_out;
    reaching_out.insert(plugins_reaching_out.begin(), plugins_reaching_out.end());
    reaching_out.insert(printing_reaching_out.begin(), printing_reaching_out.end());
    for (const std::string& key : reaching_out) {
        INFO(key);
        const NativeCommand* c = NativeCommands::find(key);
        REQUIRE(c != nullptr);
        CHECK(c->open_world);
    }
}

TEST_CASE("the plugin commands name the plugin they act on, and an install replaces one only when told to", "[CommandCatalog]")
{
    const auto required = [](const char* key) {
        const NativeCommand* c = NativeCommands::find(key);
        REQUIRE(c != nullptr);
        std::set<std::string> names;
        for (const json& name : c->input_schema.value("required", json::array()))
            names.insert(name.get<std::string>());
        return names;
    };
    CHECK(required("plugin_enable") == std::set<std::string>{"plugin", "enabled"});
    CHECK(required("plugin_uninstall") == std::set<std::string>{"plugin"});
    CHECK(required("plugin_reload") == std::set<std::string>{"plugin"});
    CHECK(required("plugin_list").empty());
    CHECK(required("plugin_refresh").empty());
    const NativeCommand* enable = NativeCommands::find("plugin_enable");
    REQUIRE(enable != nullptr);
    CHECK(enable->input_schema.at("properties").contains("capability"));
    const NativeCommand* reload = NativeCommands::find("plugin_reload");
    REQUIRE(reload != nullptr);
    CHECK(reload->input_schema.at("properties").contains("clear_cache"));
    const NativeCommand* install = NativeCommands::find("plugin_install_local");
    REQUIRE(install != nullptr);
    CHECK(install->input_schema.at("properties").at("overwrite").at("type") == "boolean");
}

TEST_CASE("a call with no arguments is not titled as an action that collects a value", "[CommandCatalog]")
{
    // view_set's Go to tab and Go to layer have no arguments of their own: their call takes its value from the Speed
    // Dial's second phase, so a call with none is view_set's, and a call with the value is the action's command with it.
    const NativeCommand* view = NativeCommands::find("view_set");
    REQUIRE(view != nullptr);
    CHECK(Slic3r::GUI::action_with_args(*view, json::object()) == nullptr);
    CHECK(Slic3r::GUI::call_title(*view, json::object()) == view->title);
    // An action that collects nothing titles its call.
    CHECK(Slic3r::GUI::call_title(*view, {{"camera", "top"}}) == "View: Top");
}

TEST_CASE("the new-project, save and recording commands take their person's form with no arguments", "[CommandCatalog]")
{
    for (const char* key : {"project_new", "project_save", "recording_start", "recording_stop"}) {
        INFO(key);
        const NativeCommand* c = NativeCommands::find(key);
        REQUIRE(c != nullptr);
        CHECK(Mcp::person_form(*c, json::object()));
    }
    const NativeCommand* fresh = NativeCommands::find("project_new");
    REQUIRE(fresh != nullptr);
    json silent{{"silent", true}};
    CHECK(Mcp::argument_error(*fresh, silent).empty());
    CHECK_FALSE(Mcp::person_form(*fresh, silent));

    // Its silent form requires path, which its person's form does not.
    const NativeCommand* save = NativeCommands::find("project_save");
    REQUIRE(save != nullptr);
    json none = json::object();
    CHECK(Mcp::argument_error(*save, none).empty());
    json path{{"path", "a.3mf"}};
    CHECK(Mcp::argument_error(*save, path).empty());
    json only_silent{{"silent", true}};
    CHECK_THAT(Mcp::argument_error(*save, only_silent), ContainsSubstring("path"));
}

TEST_CASE("every key of the process catalog is unique, tools and commands together", "[CommandCatalog]")
{
    std::set<std::string> keys;
    for (const NativeCommand& c : NativeCommands::instance().commands()) {
        INFO(c.key);
        CHECK(keys.insert(c.key).second);
    }
    // The commands made from menu items and the typed tools are both there.
    CHECK(keys.count("object_mirror") == 1);
    CHECK(keys.count("app_state") == 1);
}

TEST_CASE("the typed tools are listed and the merged commands are both", "[CommandCatalog]")
{
    // A tool whose menu item is one of its actions, with the item's title, section and icon.
    for (const auto& [key, action_key, title] : {std::make_tuple("project_new", "new_project", "New Project"),
                                                 std::make_tuple("project_save", "save_project", "Save Project"),
                                                 std::make_tuple("recording_start", "start_recording", "Start Recording"),
                                                 std::make_tuple("recording_stop", "stop_recording", "Stop Recording")}) {
        INFO(key);
        const NativeCommand* merged = NativeCommands::find(key);
        REQUIRE(merged != nullptr);
        CHECK(merged->listed);
        CHECK_FALSE(merged->has_own_action);
        const auto [command, action] = Slic3r::GUI::find_command_action(NativeCommands::instance(), action_key);
        CHECK(command == merged);
        REQUIRE(action != nullptr);
        CHECK(action->at("title") == title);
        CHECK(action->at("group") == "Commands");
        CHECK_FALSE(action->at("icon").get<std::string>().empty());
    }

    const NativeCommand* tool = NativeCommands::find("app_state");
    REQUIRE(tool != nullptr);
    CHECK(tool->listed);
    CHECK_FALSE(tool->has_own_action);

    // A command made from menu items is listed too, as it runs from its arguments: its menu items are its actions.
    const NativeCommand* menu_items = NativeCommands::find("object_mirror");
    REQUIRE(menu_items != nullptr);
    CHECK(menu_items->listed);
    CHECK_FALSE(menu_items->has_own_action);
    CHECK(menu_items->actions.size() == 3);
}

TEST_CASE("the Speed Dial's actions are under the menu items' keys, not the tools' names", "[CommandCatalog]")
{
    // An item's key is its menu item's id, which no command has; only a command's own action has the command's key.
    const CommandCatalog& catalog = NativeCommands::instance();
    std::set<std::string> speed_dial;
    for (const NativeCommands::SpeedDialAction& t : NativeCommands::speed_dial_actions(catalog)) {
        const std::string key = t.action != nullptr ? t.action->at("key").get<std::string>() : t.command->key;
        INFO(key);
        speed_dial.insert(key);
        if (t.action != nullptr)
            CHECK(catalog.find(key) == nullptr);
    }
    CHECK(speed_dial.count("new_project") == 1);
    CHECK(speed_dial.count("project_new") == 0);
    CHECK(speed_dial.count("app_state") == 0);
    // plate_slice's actions are slice_and_preview and slice_all.
    CHECK(speed_dial.count("plate_slice") == 0);
}

TEST_CASE("a merged command accepts silent and publishes its form", "[CommandCatalog]")
{
    const NativeCommand* save = NativeCommands::find("project_save");
    REQUIRE(save != nullptr);
    CHECK_FALSE(save->input_schema.contains("required"));
    CHECK(save->input_schema["then"]["required"] == json::array({"path"}));
    json none = json::object();
    CHECK(Mcp::argument_error(*save, none).empty());
    json path{{"path", "C:/a.3mf"}};
    CHECK(Mcp::argument_error(*save, path).empty());
    json silent{{"silent", true}};
    CHECK_THAT(Mcp::argument_error(*save, silent), ContainsSubstring("path"));

    const NativeCommand* fresh = NativeCommands::find("project_new");
    REQUIRE(fresh != nullptr);
    const json& properties = fresh->input_schema["properties"];
    for (const char* name : {"silent", "discard_changes", "preset_changes"}) {
        INFO(name);
        CHECK(properties.contains(name));
    }
}

TEST_CASE("a merged command's call with an action's arguments is the person's form", "[CommandCatalog]")
{
    int        tool_calls = 0;
    json       person_args; // what each run of the person's form was given
    const json schema{{"type", "object"},
                      {"properties", {{"path", {{"type", "string"}}}, {"save_as", {{"type", "boolean"}}}}},
                      {"required", {"path"}}};
    auto tool = NativeCommands::long_command("k", "K", "", schema, [&tool_calls](const json&, ToolContext&) {
        ++tool_calls;
        return ToolResult::ok({{"tool", true}});
    });
    tool.has_own_action = true;
    tool.actions        = json::array({NativeCommands::action("k_as", "K as", "Commands", "", {{"save_as", true}}, "Saves as.")});
    const NativeCommand merged = NativeCommands::with_person_form(tool, [&person_args](const json& action_args) {
        person_args.push_back(action_args);
        return AppActionRunResult{AppActionRunResult::Level::Succeeded, "Asked"};
    });
    CHECK(merged.input_schema["properties"].contains("silent"));
    // Neither person's form needs what the silent one requires.
    CHECK(merged.input_schema["if"] == json{{"not", {{"enum", json::array({json::object(), {{"save_as", true}}})}}}});

    TestContext      agent;
    const ToolResult asked = merged.run(json::object(), agent);
    CHECK(asked.data["ran"] == "k");
    merged.run({{"save_as", true}}, agent);
    CHECK(person_args == json::array({json::object(), {{"save_as", true}}}));
    CHECK(tool_calls == 0);
    const ToolResult given = merged.run({{"path", "a"}}, agent);
    CHECK(person_args.size() == 2);
    CHECK(tool_calls == 1);
    CHECK(given.data["tool"] == true);
}

TEST_CASE("a pick of an action whose menu item and tool are one function runs it in place, and a call runs the tool", "[CommandCatalog]")
{
    int  tool_calls = 0;
    json person_args = json::array(); // what each run of the shared function for a pick was given
    auto tool        = NativeCommands::long_command("k", "K", "", no_arguments(), [&tool_calls](const json&, ToolContext&) {
        ++tool_calls;
        return ToolResult::ok({{"tool", true}});
    });
    tool.actions = json::array({NativeCommands::action("k_item", "Item", "View", "", {{"item", true}}, "The menu item's function."),
                                NativeCommands::action("k_tool", "Tool", "View", "", {{"camera", "top"}}, "The tool's function."),
                                NativeCommands::action("k_input", "Input", "View", "", json::object(), "A value it collects.", "percent",
                                                       "level")});
    const NativeCommand merged = NativeCommands::with_menu_item_forms(
        tool,
        [&person_args](const json& action_args) {
            person_args.push_back(action_args);
            return AppActionRunResult{AppActionRunResult::Level::Succeeded};
        },
        /*person_form_behind_dialogs=*/false, std::vector<json>{json{{"camera", "top"}}});
    // The menu item's action and the tool's are person's forms of the command; the action that collects a value is not.
    CHECK(Mcp::person_form(merged, {{"item", true}}));
    CHECK(Mcp::person_form(merged, {{"camera", "top"}}));
    CHECK_FALSE(Mcp::person_form(merged, {{"level", 50}}));

    // An automated call: the menu item's action runs the item, the tool's own form and the input action's call run the tool.
    TestContext agent;
    merged.run({{"item", true}}, agent);
    CHECK(person_args == json::array({{{"item", true}}}));
    CHECK(merged.run({{"camera", "top"}}, agent).data["tool"] == true);
    CHECK(merged.run({{"level", 50}}, agent).data["tool"] == true);
    CHECK(tool_calls == 2);
    CHECK(agent.submitted.empty());

    // A person's pick of the tool's own form, and of the input action with its value, runs the shared function in place:
    // nothing is submitted as a job, and the tool's function does not run.
    TestContext person(/*person=*/true);
    const ToolResult picked = merged.run({{"camera", "top"}}, person);
    CHECK_FALSE(picked.is_error);
    REQUIRE(picked.notice != nullptr);
    CHECK(picked.notice->level == AppActionRunResult::Level::Succeeded);
    merged.run({{"level", 50}}, person);
    CHECK(person_args == json::array({{{"item", true}}, {{"camera", "top"}}, {{"level", 50}}}));
    CHECK(person.submitted.empty());
    CHECK(tool_calls == 2);
    // Any other call of a person is the tool's, which a long command starts as a job.
    merged.run({{"camera", "front"}}, person);
    CHECK(person.submitted == std::vector<std::string>{"k"});
}

TEST_CASE("an agent's call of a person's form is checked as a mutate call, a person's is not", "[CommandCatalog]")
{
    int  person_calls = 0;
    auto tool         = NativeCommands::long_command("k", "K", "", no_arguments(), [](const json&, ToolContext&) {
        return ToolResult::ok();
    });
    tool.guard          = GuardClass::Inspect; // as the recording tools run, behind dialogs
    tool.has_own_action = true;
    const auto person   = [&person_calls](const json&) {
        ++person_calls;
        return AppActionRunResult{AppActionRunResult::Level::Succeeded, "Asked"};
    };
    const NativeCommand opens = NativeCommands::with_person_form(tool, person);
    const NativeCommand quiet = NativeCommands::with_person_form(tool, person, /*person_form_behind_dialogs=*/true);

    // An agent's call asks the runtime's UI for the mutate check before the person's form runs: a context
    // without a runtime refuses it, and the form never runs.
    TestContext agent;
    CHECK_THROWS_AS(opens.run(json::object(), agent), std::logic_error);
    CHECK(person_calls == 0);
    // A form that opens nothing runs where the tool does, and a person's pick is never checked.
    CHECK_FALSE(quiet.run(json::object(), agent).is_error);
    CHECK(person_calls == 1);
    TestContext picked(/*person=*/true);
    CHECK_FALSE(opens.run(json::object(), picked).is_error);
    CHECK(person_calls == 2);
    // The tool's own form is not the person's: no check.
    CHECK_FALSE(opens.run({{"silent", true}}, agent).is_error);
    CHECK(person_calls == 2);
}

TEST_CASE("a silent call is journaled silent", "[CommandCatalog]")
{
    NativeCommand tool = NativeCommands::long_command("k", "K", "", no_arguments(), [](const json& args, ToolContext& ctx) {
        ctx.set_journal_args(args);
        return ToolResult::ok();
    });
    tool.has_own_action        = true;
    const NativeCommand merged = NativeCommands::with_person_form(tool, [](const json&) { return AppActionRunResult{}; });

    TestContext agent;
    merged.run({{"silent", true}}, agent);
    CHECK(agent.journaled == json{{"silent", true}});
}

TEST_CASE("a recording started with arguments lasts an hour by default", "[CommandCatalog]")
{
    const NativeCommand* start = NativeCommands::find("recording_start");
    REQUIRE(start != nullptr);
    CHECK_THAT(start->input_schema["properties"]["max_seconds"]["description"].get<std::string>(), ContainsSubstring("default 3600"));
}

TEST_CASE("the catalog needs no runtime", "[CommandCatalog]")
{
    // No AutomationRuntime is made in this test: the catalog is built without one, and its listing is the tool list.
    const CommandCatalog& catalog = NativeCommands::instance();
    struct NoApp final : ToolCaller
    {
        ToolResult call(const NativeCommand&, const json&, const json&) override { return ToolResult::error("no app"); }
    } caller;
    Mcp::McpProtocol protocol(catalog, caller, Mcp::ServerInfo{});
    const Mcp::PostReply reply = protocol.handle_post({json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}.dump(), {}, {}, {}});
    REQUIRE(reply.status == 200);
    const json            tools = json::parse(reply.body)["result"]["tools"];
    std::set<std::string> names;
    for (const json& tool : tools)
        names.insert(tool["name"].get<std::string>());
    CHECK(names.count("app_state") == 1);
    CHECK(names.count("tool_call") == 1);
    CHECK(names.count("object_mirror") == 1);
    // A menu item with no argument form is not.
    CHECK(names.count("settings_toggle_developer_mode") == 0);
}

TEST_CASE("a command made from menu items is found by the one lookup", "[CommandCatalog]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    CHECK(catalog.lookup("object_mirror") != nullptr);
    // A misspelt key is answered with the closest command, which tool_call runs, not with action_run of an action.
    const std::string unknown = catalog.unknown_command("object_miror", /*suggest=*/true);
    CHECK_THAT(unknown, ContainsSubstring("object_mirror"));
    CHECK_THAT(unknown, !ContainsSubstring("action_run"));
}

TEST_CASE("a command the catalog leaves out is unknown with the reason", "[CommandCatalog]")
{
    CommandCatalog catalog;
    catalog.add(command("hold"));
    catalog.leave_out("holes", "the feature was off", "feature_off");
    catalog.seal();
    REQUIRE(catalog.left_out("holes") != nullptr);
    CHECK(*catalog.left_out("holes") == "the feature was off");
    CHECK(catalog.left_out("hold") == nullptr);
    // The reason, and no close key: the key names the command the caller meant.
    CHECK(catalog.unknown_command("holes", /*suggest=*/true) == "Unknown tool: holes; the feature was off");
    CHECK(catalog.unknown_command("holx", /*suggest=*/true) == "Unknown tool: holx (did you mean hold?)");
    // A left-out key's name is right, so its error carries what keeps it from the run; any other is the call's to correct.
    CHECK(catalog.unknown_command_code("holes") == "feature_off");
    CHECK(catalog.unknown_command_code("holx") == "invalid_argument");
    CHECK_THROWS_AS(catalog.leave_out("solid_shell", "too late", "feature_off"), std::logic_error);

    // A run with the CAD feature off at startup leaves out the Design commands, and says how a person turns it
    // on; the G-code viewer leaves them out whatever the preference, and says to open the editor. A build without
    // CAD has no Design commands to leave out.
    CommandCatalog no_cad;
    NativeCommands::register_all_commands(no_cad, {false});
    no_cad.seal();
    CommandCatalog viewer;
    NativeCommands::register_all_commands(viewer, {false, /*editor=*/false});
    viewer.seal();
#ifdef SLIC3R_CAD
    for (const std::string key : {"solid_extrude", "solid_hole", "design_describe"}) {
        INFO(key);
        CHECK(no_cad.unknown_command(key, /*suggest=*/true) ==
              "Unknown tool: " + key + "; the CAD feature was off when OrcaSlicer started: a person turns it on in "
                                       "Preferences > Features and restarts OrcaSlicer, or starts it with --cad");
        CHECK(no_cad.unknown_command_code(key) == "design_unavailable");
        CHECK(viewer.unknown_command(key, /*suggest=*/true) ==
              "Unknown tool: " + key + "; the G-code viewer has no Design tab: a person opens the project in OrcaSlicer's editor");
        CHECK(viewer.unknown_command_code(key) == "design_unavailable");
    }
#else
    CHECK(no_cad.left_out("solid_extrude") == nullptr);
#endif
    CHECK(no_cad.left_out("object_arrange") == nullptr);
}

TEST_CASE("every id argument of a schema is in id_args", "[CommandCatalog]")
{
    for (const NativeCommand& c : NativeCommands::instance().commands()) {
        INFO(c.key);
        std::set<std::string> in_schema;
        if (c.input_schema.is_object() && c.input_schema.contains("properties"))
            for (const Mcp::IdArgument& arg : Mcp::kIdArguments) {
                // An id names what its index does, so a schema with one has the other.
                INFO(arg.id);
                CHECK(c.input_schema["properties"].contains(arg.id) == c.input_schema["properties"].contains(arg.index));
                if (c.input_schema["properties"].contains(arg.id))
                    in_schema.insert(arg.id);
            }
        CHECK(std::set<std::string>(c.id_args.begin(), c.id_args.end()) == in_schema);
    }
    for (const char* key : {"object_transform", "object_select", "settings_set", "object_arrange", "object_order"}) {
        INFO(key);
        const NativeCommand* takes_ids = NativeCommands::find(key);
        REQUIRE(takes_ids != nullptr);
        CHECK_FALSE(takes_ids->id_args.empty());
    }
}

TEST_CASE("the process catalog is built once", "[CommandCatalog]")
{
    // Built here on first use, if no test before this one built it.
    const CommandCatalog& catalog = NativeCommands::instance();
    CHECK_THROWS_AS(NativeCommands::build({}), std::logic_error);
    CHECK(&NativeCommands::instance() == &catalog);
}

TEST_CASE("the Speed Dial has the commands' own actions and their items, in the catalog's order", "[CommandCatalog]")
{
    CommandCatalog catalog;
    catalog.add(command("first", true));
    catalog.add(command("typed_tool", false));
    NativeCommand folded = command("folded", true);
    folded.actions       = json::array({NativeCommands::action("folded_x", "X", "View", "", {{"axis", "x"}}, "Along X."),
                                        NativeCommands::action("folded_y", "Y", "View", "", {{"axis", "y"}}, "Along Y.")});
    catalog.add(std::move(folded));
    // A listed command's actions are in the Speed Dial as any other's are.
    NativeCommand items_only = command("items_only", false);
    items_only.listed        = true;
    items_only.actions = json::array({NativeCommands::action("items_only_tab", "Tab", "Commands", "", json::object(), "A tab.", "tab")});
    catalog.add(std::move(items_only));
    catalog.seal();

    const std::vector<NativeCommands::SpeedDialAction> speed_dial = NativeCommands::speed_dial_actions(catalog);
    REQUIRE(speed_dial.size() == 5);
    CHECK(speed_dial[0].command == catalog.find("first"));
    CHECK(speed_dial[0].action == nullptr);
    CHECK(speed_dial[1].command == catalog.find("folded"));
    CHECK(speed_dial[1].action == nullptr);
    CHECK(speed_dial[2].action == &catalog.find("folded")->actions[0]);
    CHECK(speed_dial[3].action == &catalog.find("folded")->actions[1]);
    CHECK(speed_dial[4].command == catalog.find("items_only"));
    REQUIRE(speed_dial[4].action != nullptr);
    CHECK(speed_dial[4].action->at("input") == "tab");
}

TEST_CASE("a sealed catalog is read from eight threads", "[CommandCatalog]")
{
    // Keys the catalog holds, misspells and lacks; each thread asks about them in turn.
    const std::vector<std::string> keys{"plate_slice", "undo", "plate_slic", "project_expor", "no_such_command"};
    struct Answer
    {
        const NativeCommand*     found = nullptr;
        std::vector<std::string> similar;

        bool operator==(const Answer& other) const { return found == other.found && similar == other.similar; }
    };
    constexpr std::size_t    thread_count = 8;
    constexpr std::size_t    rounds       = 1000;
    std::vector<std::vector<Answer>> answers(thread_count);
    {
        std::vector<std::thread> threads;
        for (std::size_t t = 0; t < thread_count; ++t)
            threads.emplace_back([&keys, &answers, t] {
                // The first of them to get here may build the process catalog.
                const CommandCatalog& catalog = NativeCommands::instance();
                answers[t].reserve(rounds);
                for (std::size_t i = 0; i < rounds; ++i) {
                    const std::string& key = keys[i % keys.size()];
                    answers[t].push_back({catalog.find(key), catalog.similar(key)});
                }
            });
        for (std::thread& thread : threads)
            thread.join();
    }

    for (std::size_t t = 0; t < thread_count; ++t) {
        INFO("thread " << t);
        REQUIRE(answers[t].size() == rounds);
        CHECK(answers[t] == answers.front());
    }
    // And they are the right answers.
    const std::vector<Answer>& first = answers.front();
    REQUIRE(first[0].found != nullptr);
    CHECK(first[0].found->key == "plate_slice");
    CHECK(first[2].found == nullptr);
    REQUIRE_FALSE(first[2].similar.empty());
    CHECK(first[2].similar.front() == "plate_slice");
    CHECK(first[4].found == nullptr);
}

TEST_CASE("a person's pick runs the function in place", "[CommandCatalog]")
{
    int                 calls = 0;
    const NativeCommand c     = NativeCommands::ui_command("k", "K", "The app", no_arguments(), [&calls](const json&) {
        ++calls;
        return AppActionRunResult{AppActionRunResult::Level::Succeeded, "Done"};
    });
    Mcp::PersonContext    person;
    const ToolResult      result = c.run(json::object(), person);
    CHECK(calls == 1);
    REQUIRE(result.notice != nullptr);
    CHECK(result.notice->level == AppActionRunResult::Level::Succeeded);
    CHECK(result.notice->message == "Done");
}

TEST_CASE("a person's pick shows a command's refusal as an error", "[CommandCatalog]")
{
    const NativeCommand refuses = NativeCommands::ui_command("k", "K", "The app", no_arguments(), [](const json&) -> AppActionRunResult {
        throw ToolFailure("Nothing is selected");
    });
    const AppActionRunResult shown = NativeCommands::run(refuses);
    CHECK(shown.level == AppActionRunResult::Level::Error);
    CHECK(shown.message == "Nothing is selected");
    CHECK_FALSE(shown.done);
}

TEST_CASE("an exception that is not a command's refusal leaves a person's pick", "[CommandCatalog]")
{
    // A defect, which the application's handler deals with as it does one from any other event handler.
    const NativeCommand breaks = NativeCommands::ui_command("k", "K", "The app", no_arguments(), [](const json&) -> AppActionRunResult {
        throw std::out_of_range("no such element");
    });
    CHECK_THROWS_AS(NativeCommands::run(breaks), std::out_of_range);
}

TEST_CASE("a long command picked by a person is submitted", "[CommandCatalog]")
{
    int             calls = 0;
    const CommandFn fn    = [&calls](const json&, ToolContext&) {
        ++calls;
        return ToolResult::ok();
    };
    TestContext person(true);
    NativeCommands::long_command("k", "K", "The app", no_arguments(), fn).run(json::object(), person);
    CHECK(person.submitted == std::vector<std::string>{"k"});
    CHECK(calls == 0);

    const NativeCommand in_place = NativeCommands::long_command("k", "K", "The app", no_arguments(), fn, true);
    CHECK(in_place.inline_call);
    in_place.run(json::object(), person);
    CHECK(calls == 1);
    CHECK(person.submitted.size() == 1);

    // An agent's call is already a job: the function runs.
    TestContext agent;
    NativeCommands::long_command("k", "K", "The app", no_arguments(), fn).run(json::object(), agent);
    CHECK(calls == 2);
    CHECK(agent.submitted.empty());
}

TEST_CASE("a person's submit refuses an unknown command, and starts no job without OrcaSlicer", "[CommandCatalog]")
{
    // The lookup comes first; starting the runtime, the job and its notices need the app, which no unit test runs.
    Mcp::PersonContext person;
    CHECK_THROWS_WITH(person.submit("no_such_command", json::object()), ContainsSubstring("Unknown command no_such_command"));
    CHECK_THROWS_AS(person.submit("script_run", json::object()), ToolFailure);
}

TEST_CASE("an Export command called with arguments takes the project, and with none does not", "[CommandCatalog]")
{
    const auto fails = [](const json&) { return AppActionRunResult{AppActionRunResult::Level::Error, "Nothing to export", false}; };
    const NativeCommand exporting = NativeCommands::ui_command("k", "K", "The app", no_arguments(), fails, CommandCompletion::Deferred);
    CHECK(exporting.completion == CommandCompletion::Deferred);

    TestContext           given;
    const ToolResult      refused = exporting.run({{"path", "a.gcode"}}, given);
    CHECK(given.operations == 1);
    CHECK(refused.is_error);
    CHECK(refused.message == "Nothing to export");

    TestContext none;
    exporting.run(json::object(), none);
    CHECK(none.operations == 0);

    const NativeCommand plain = NativeCommands::ui_command("k", "K", "The app", no_arguments(), fails);
    TestContext         either;
    plain.run({{"path", "a.gcode"}}, either);
    plain.run(json::object(), either);
    CHECK(either.operations == 0);
}

TEST_CASE("an action with a second phase takes its value, typed as its command's argument", "[CommandCatalog]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    for (const auto& [key, input, value] : {std::make_tuple("go_to_tab", "tab", json("preview")), std::make_tuple("go_to_layer", "percent", json(50))}) {
        INFO(key);
        const auto [command, command_action] = Slic3r::GUI::find_command_action(catalog, key);
        REQUIRE(command != nullptr);
        REQUIRE(command_action != nullptr);
        const std::unique_ptr<Slic3r::GUI::AppAction> action = NativeCommands::make_action(*command, *command_action);
        CHECK(action->input == input);
        CHECK(action->input_schema["required"] == json::array({input}));
        // The pick's value reaches view_set under its argument, which checks it.
        json call = Slic3r::GUI::action_call_args(command_action, {{input, value}});
        CHECK(Mcp::argument_error(*command, call).empty());
    }
    // The layer is a share of the layer range.
    const NativeCommand* view = NativeCommands::find("view_set");
    REQUIRE(view != nullptr);
    json over{{"layer_percent", 150}};
    CHECK_THAT(Mcp::argument_error(*view, over), ContainsSubstring("layer_percent"));
}

TEST_CASE("a rename to an empty name is refused before anything is renamed", "[CommandCatalog]")
{
    const NativeCommand* rename = NativeCommands::find("object_rename");
    REQUIRE(rename != nullptr);
    // No app runs here: the refusal comes before the command looks at the object list.
    TestContext           agent;
    const ToolResult      refused = rename->run({{"name", ""}}, agent);
    CHECK(refused.is_error);
    CHECK_THAT(refused.message, ContainsSubstring("name"));
}

TEST_CASE("undo takes a history", "[CommandCatalog]")
{
    for (const char* key : {"undo", "redo"}) {
        INFO(key);
        const NativeCommand* command = NativeCommands::find(key);
        REQUIRE(command != nullptr);
        CHECK(command->has_own_action);
        CHECK(command->listed);
        json design{{"history", "design"}};
        CHECK(Mcp::argument_error(*command, design).empty());
        json project{{"history", "project"}};
        CHECK(Mcp::argument_error(*command, project).empty());
        // With none, the history of the tab shown.
        json none = json::object();
        CHECK(Mcp::argument_error(*command, none).empty());
        json sketch{{"history", "sketch"}};
        CHECK_THAT(Mcp::argument_error(*command, sketch), ContainsSubstring("history"));
    }
}

TEST_CASE("a person's undo shows nothing where the Edit menu's is greyed, an agent's is refused", "[CommandCatalog]")
{
    // No Design tab is built here, so the Edit menu's Undo and Redo of its history would be greyed.
    const json design{{"history", "design"}};
    for (const char* key : {"undo", "redo"}) {
        INFO(key);
        const NativeCommand* command = NativeCommands::find(key);
        REQUIRE(command != nullptr);
        TestContext           person(true);
        const ToolResult      picked = command->run(design, person);
        REQUIRE(picked.notice != nullptr);
        CHECK(picked.notice->message.empty());
        CHECK(picked.notice->level != AppActionRunResult::Level::Error);
        // Nothing was done: the project's history is not taken back in the Design history's place.
        CHECK_FALSE(picked.notice->done);
        // An agent's call reaches the Design history's gate, which says why it cannot run.
        TestContext agent;
        CHECK_THROWS_AS(command->run(design, agent), ToolFailure);
    }
}

TEST_CASE("a command's result reads as action_run's did", "[CommandCatalog]")
{
    using Level = AppActionRunResult::Level;
    const ToolResult done = NativeCommands::from_run_result("k", {Level::Succeeded, "Done"});
    CHECK_FALSE(done.is_error);
    CHECK(done.data == json{{"ran", "k"}, {"level", "success"}, {"message", "Done"}});
    CHECK(NativeCommands::from_run_result("k", {Level::Busy, "Wait"}).is_error);
    CHECK(NativeCommands::from_run_result("k", {Level::Error, "Failed"}).is_error);
    // A command that did nothing fails, though the Speed Dial shows it as information.
    CHECK(NativeCommands::from_run_result("k", {Level::Info, "Select an object first.", false}).is_error);

    // What a command adds to its reply joins the reply's fields.
    const ToolResult ordered = NativeCommands::from_run_result("k", {Level::Succeeded, {}, true, R"({"descending": true})"});
    CHECK(ordered.data == json{{"ran", "k"}, {"level", "success"}, {"descending", true}});

    Ops::Refusal why;
    why.code  = "no_objects";
    why.msgid = "There are no objects.";
    AppActionRunResult refused{Level::Error, "translated", false};
    refused.refusal              = std::make_shared<const Ops::Refusal>(why);
    const ToolResult failed      = NativeCommands::from_run_result("k", refused);
    CHECK(failed.is_error);
    CHECK(failed.message == "There are no objects.");
    CHECK(failed.data["code"] == "no_objects");
    CHECK_FALSE(failed.data.contains("hint"));

    // A refusal worded for the GUI keeps its text, which the person reads too, and tells an agent's call what to do
    // in the commands' terms, by either route a refusal takes to it.
    Ops::Refusal gizmo;
    gizmo.code  = "gizmo_open";
    gizmo.msgid = "Error: Please close all toolbar menus first";
    refused.refusal                 = std::make_shared<const Ops::Refusal>(gizmo);
    const ToolResult behind_gizmo   = NativeCommands::from_run_result("k", refused);
    CHECK(behind_gizmo.message == "Error: Please close all toolbar menus first");
    CHECK_THAT(behind_gizmo.data.value("hint", std::string()), ContainsSubstring("object_select {none: true}"));
    const ToolFailure operation_refused = Mcp::refused(gizmo);
    CHECK(std::string(operation_refused.what()) == "Error: Please close all toolbar menus first");
    CHECK(operation_refused.details.at("hint") == behind_gizmo.data.at("hint"));
    // The two gizmo_open refusals are told apart by the call: Select All's is about the view shown, the others' about
    // Prepare's, which the assemble view shows only after view_set.
    CHECK_THAT(behind_gizmo.data.value("hint", std::string()), ContainsSubstring("view_set {canvas: \"3d\"}"));
}

TEST_CASE("a refusal that asks for a GUI step or a selection names the calls that do it instead", "[CommandCatalog]")
{
    const auto hint_of = [](const char* code) {
        Ops::Refusal refusal;
        refusal.code  = code;
        refusal.msgid = "Refused.";
        return Mcp::refusal_hint(refusal);
    };
    // Add Primitive's person's form refuses while an object is selected; a size or silent adds it whatever is selected.
    const std::string primitive = hint_of("object_selected");
    CHECK_THAT(primitive, ContainsSubstring("object_select {none: true}"));
    // The Speed Dial's add_primitive actions take no arguments, so the hint names the command that takes them.
    CHECK_THAT(primitive, ContainsSubstring("object_add_primitive with size or silent: true"));
    CHECK_THAT(primitive, ContainsSubstring("size"));
    CHECK_THAT(primitive, ContainsSubstring("silent: true"));
    CHECK_THAT(primitive, ContainsSubstring("part_add"));
    // The refusals that ask for a selection name the targets a call gives instead.
    for (const char* code : {"nothing_selected", "one_object_only", "wrong_selection", "too_few_objects", "several_instances"}) {
        INFO(code);
        CHECK_THAT(hint_of(code), ContainsSubstring("name the targets in the call"));
    }
    // A step only a person takes has no command to name.
    for (const char* code : {"not_signed_in", "no_printer", "no_printer_connected", "no_objects"}) {
        INFO(code);
        CHECK(hint_of(code).empty());
    }
}

#ifdef SLIC3R_CAD
// The file is the reference of the build with every command, so a build without CAD does not check it.
TEST_CASE("the checked-in command reference matches the catalog", "[CommandCatalog][Reference]")
{
    const CommandCatalog catalog = catalog_with_cad();
    const std::string dump = Slic3r::GUI::catalog_reference(catalog).dump(1) + "\n";
    const std::string path = std::string(TEST_DATA_DIR) + "/command_catalog.json";

    const char* update = std::getenv("ORCA_UPDATE_COMMAND_REFERENCE");
    if (update != nullptr && std::string(update) == "1") {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << dump;
        out.close();
        INFO(path);
        CHECK(out.good());
        return;
    }

    std::ifstream in(path, std::ios::binary);
    std::string   file(std::istreambuf_iterator<char>(in), {});
    // A checkout may turn the file's line ends into CRLF; the dump has no carriage return of its own, since JSON
    // writes one inside a string as \r.
    file.erase(std::remove(file.begin(), file.end(), '\r'), file.end());
    const std::string difference = in.is_open() ? first_difference(file, dump) : "the file is missing";
    INFO("tests/data/command_catalog.json is not the catalog's reference: " << difference
         << ". Regenerate it by running slic3rutils_tests \"[Reference]\" with ORCA_UPDATE_COMMAND_REFERENCE=1 in the "
            "environment, check the file in, then regenerate the skill's actions.md with "
            "tools/mcp/gen_skill_reference.py <skills checkout>.");
    CHECK(difference.empty());
}
#endif

TEST_CASE("the reference leaves out nothing a client is told", "[CommandCatalog][Reference]")
{
    CommandCatalog catalog;
    NativeCommands::register_all_commands(catalog, {true});
    catalog.add_source(std::make_unique<OneCommandSource>("plugin.x.y"));
    catalog.seal();
    const json reference = Slic3r::GUI::catalog_reference(catalog);

    // Every command added, in that order, and none of a source.
    std::vector<std::string> keys;
    for (const json& entry : reference.at("commands"))
        keys.push_back(entry.at("key").get<std::string>());
    CHECK(keys == keys_of(catalog.commands()));

    std::set<std::string> listed;
    for (const NativeCommand& command : catalog.commands()) {
        INFO(command.key);
        const json entry = reference_entry(reference, command.key);
        CHECK(entry == Slic3r::GUI::command_reference(command));
        if (!entry.at("listed").get<bool>())
            continue;
        listed.insert(command.key);
        const json told = Mcp::McpProtocol::tool_to_json(command);
        CHECK(entry["description"] == told["description"]);
        CHECK(entry["input_schema"] == told["inputSchema"]);
        CHECK(entry["title"] == told.value("title", std::string()));
        const json& annotations = told["annotations"];
        CHECK(entry["read_only"] == annotations["readOnlyHint"]);
        CHECK(entry["destructive"] == annotations["destructiveHint"]);
        CHECK(entry["idempotent"] == annotations["idempotentHint"]);
        CHECK(entry["open_world"] == annotations["openWorldHint"]);
    }
    // The entries marked listed are the tools a client's tools/list names.
    struct NoApp final : ToolCaller
    {
        ToolResult call(const NativeCommand&, const json&, const json&) override { return ToolResult::error("no app"); }
    } caller;
    Mcp::McpProtocol protocol(catalog, caller, Mcp::ServerInfo{});
    const Mcp::PostReply reply = protocol.handle_post({json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}.dump(), {}, {}, {}});
    REQUIRE(reply.status == 200);
    const json            told_list = json::parse(reply.body);
    std::set<std::string> tools;
    for (const json& tool : told_list["result"]["tools"])
        tools.insert(tool["name"].get<std::string>());
    CHECK_FALSE(tools.empty());
    CHECK(listed == tools);
}

TEST_CASE("the reference shows both forms of a two-form command", "[CommandCatalog][Reference]")
{
    const CommandCatalog catalog = catalog_with_cad();
    const json reference = Slic3r::GUI::catalog_reference(catalog);
    // Each with what its person's forms are: {} alone, or the arguments of each of its actions.
    std::vector<std::pair<std::string, json>> two_forms{
        {"project_save", {{"not", {{"enum", json::array({json::object(), {{"save_as", true}}})}}}}}};
#ifdef SLIC3R_CAD
    two_forms.emplace_back("solid_fillet", json{{"minProperties", 1}});
#endif
    for (const auto& [key, person_forms] : two_forms) {
        INFO(key);
        const json entry = reference_entry(reference, key);
        REQUIRE(entry.is_object());
        const json& schema = entry.at("input_schema");
        CHECK(schema.at("properties").contains("silent"));
        // The silent form's requirement holds for any call but a person's form, which needs none of it.
        CHECK_FALSE(schema.contains("required"));
        CHECK(schema.at("if") == person_forms);
        REQUIRE(schema.at("then").contains("required"));
        CHECK_FALSE(schema.at("then").at("required").empty());
    }
    CHECK(reference_entry(reference, "project_save")["input_schema"]["then"]["required"] == json::array({"path"}));
}

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "automation/automation_test_utils.hpp"
#include "retired_names.hpp"

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
#include <map>
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
using Slic3r::GUI::Mcp::Test::command;
using Slic3r::GUI::Mcp::Test::OneCommandSource;
namespace Mcp            = Slic3r::GUI::Mcp;
namespace NativeCommands = Slic3r::GUI::NativeCommands;
namespace Ops            = Slic3r::GUI::Ops;

namespace {

json no_arguments() { return {{"type", "object"}, {"properties", json::object()}}; }

// A call's context without the MCP core: UI closures run in place and nothing is left to wait for. It
// counts the operations a call begins and records the commands it submits as jobs.
class TestContext final : public ToolContext
{
public:
    explicit TestContext(bool person = false) : m_person(person) {}

    bool           person() const override { return m_person; }
    json           run_ui(const std::function<json()>& fn) override { return fn(); }
    json           observe_ui(const std::function<json()>& /*fn*/) override { return true; }
    const std::string& job_id() const override { return m_job; }
    bool           cancelled() const override { return false; }
    ToolResult call_child(const std::string& /*tool*/, const json& /*args*/) override { return ToolResult::error("no child calls"); }
    std::set<std::string> lineage() const override { return {}; }
    void                  set_journal_args(json args) override { journaled = std::move(args); }
    void                  begin_operation() override { ++operations; }
    ToolResult            submit(const std::string& key, const json& /*args*/) override
    {
        submitted.push_back(key);
        return ToolResult::ok();
    }

    int                      operations = 0;
    std::vector<std::string> submitted;
    json                     journaled; // what the call last recorded for the journal

private:
    bool        m_person;
    std::string m_job;
};

std::vector<std::string> keys_of(const std::vector<NativeCommand>& commands)
{
    std::vector<std::string> keys;
    for (const NativeCommand& c : commands)
        keys.push_back(c.key);
    return keys;
}

// Every built-in command, the Design tab's among them in a build with CAD, as a run with the Design tab on holds
// them. Not sealed yet.
CommandCatalog catalog_with_design()
{
    CommandCatalog catalog;
    NativeCommands::register_all_commands(catalog, {true});
    return catalog;
}

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

// The design's command set outside the Design tab, heading by heading in the order tools/list gives them, Driving the
// GUI last, after the Design tab's commands: the 137 commands of the catalog, its typed tools, the commands made from
// menu items, the seven Parts 4a to 4c add (Printable, Fix Model, Auto Drop, Clone, Reload from disk, Replace 3D file,
// Add Mixed Filament), the three of Part 5a (the Printer Selection dialog's list, install and uninstall), the two of
// Part 5b (the System Filaments dialog's install and uninstall), Part 5c's write of the Preferences dialog's preferences
// and the four of Part 5d (the Plugins window's list, a switch, Delete or Unsubscribe, and Reload), the seven gizmo
// commands (Paint, Measure, Simplify, Mesh Boolean, Cut, Lay on Face and Brim Ears), and the six printing commands (the
// Print button's Print, the print host's test, and the Device tab's status, Pause, Resume and Stop).
// A part that adds a command adds it here.
const std::vector<std::pair<std::string, std::set<std::string>>> kPlaces{
    {"Tools and actions", {"tool_list", "tool_describe", "tool_call", "action_list", "action_run"}},
    {"The app",
     {"app_state", "app_log", "app_wait", "job_cancel", "undo", "redo", "app_open_web_page", "app_check_for_updates",
      "app_show_config_folder", "app_benchmark_3d_scene", "preferences_get", "preferences_set"}},
    {"The project",
     {"project_new", "project_open", "project_import", "project_save", "project_get_settings", "project_set_settings", "project_export"}},
    {"Objects",
     {"object_list", "object_select", "object_add_primitive", "object_add_handy_model", "object_add_text", "object_add_svg",
      "object_delete", "object_rename", "object_set_instances", "object_separate_instances", "object_split", "object_assemble",
      "object_order", "object_transform", "object_drop", "object_mirror", "object_center", "object_scale_to_fit", "object_arrange",
      "object_auto_orient", "object_set_variable_layer_height", "object_set_height_ranges", "object_set_printable", "object_fix",
      "object_set_auto_drop", "object_clone", "object_reload_from_disk", "object_paint", "object_measure", "object_simplify", "object_cut",
      "object_lay_on_face", "object_set_brim_ears"}},
    {"Parts", {"part_add", "part_change_type", "part_replace", "part_boolean"}},
    {"Plates",
     {"plate_list", "plate_get_settings", "plate_set_settings", "plate_add", "plate_delete", "plate_duplicate", "plate_rename",
      "plate_lock", "plate_select", "plate_slice", "plate_slice_stats", "plate_set_custom_gcode", "calibration_generate_test"}},
    {"Presets and settings",
     {"preset_list", "preset_select", "preset_save", "preset_delete", "settings_list", "settings_get", "settings_set", "preset_import",
      "preset_export", "preset_sync", "settings_set_mode", "preset_migrate_filaments", "settings_toggle_developer_mode"}},
    {"Filaments",
     {"filament_list", "filament_add", "filament_delete", "filament_sync_ams", "filament_add_mixed", "filament_edit_mixed",
      "filament_set_colour", "filament_set_flushing", "filament_install", "filament_uninstall"}},
    {"Printers and prints",
     {"printer_send", "printer_list_models", "printer_install", "printer_uninstall", "print_start", "printer_test_host",
      "printer_status", "print_pause", "print_resume", "print_stop"}},
    {"Plugins", {"plugin_list", "plugin_install_local", "plugin_enable", "plugin_reload", "plugin_uninstall", "plugin_refresh"}},
    {"View and windows", {"view_set", "window_list", "window_set", "window_open"}},
    {"Recordings, replays and scripts",
     {"script_list_api", "script_describe_api", "window_screenshot", "recording_start", "recording_stop", "recording_encode", "replay_run", "replay_export",
      "script_run", "window_save_screenshot", "script_toggle_panel"}},
    {"Driving the GUI",
     {"widget_list", "widget_click", "widget_set_value", "keyboard_press", "mouse_gesture", "webview_run_javascript", "dialog_answer",
      "dialog_auto_answer", "gizmo_pick_file"}},
};

// The groups of `keys`' commands in `catalog`, each run of one group once, in their order.
std::vector<std::string> group_runs(const CommandCatalog& catalog, const std::vector<std::string>& keys)
{
    std::vector<std::string> runs;
    for (const std::string& key : keys)
        if (const NativeCommand* c = catalog.find(key); c != nullptr && (runs.empty() || runs.back() != c->group))
            runs.push_back(c->group);
    return runs;
}

} // namespace

TEST_CASE("the process catalog gives the Speed Dial its 129 command actions", "[CommandCatalog]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    CHECK(catalog.sealed());
    const std::vector<NativeCommands::SpeedDialAction> speed_dial = NativeCommands::speed_dial_actions(catalog);
    CHECK(speed_dial.size() == 129);
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

TEST_CASE("every Speed Dial action of today is still a command's action", "[CommandCatalog][SpeedDial]")
{
    // The Speed Dial's 129 actions of commands, each with its id, title, section, icon and input: each is an action of
    // the Speed Dial with that presentation, and the Speed Dial has no other.
    std::ifstream in(std::string(TEST_DATA_DIR) + "/speed_dial_actions.json", std::ios::binary);
    REQUIRE(in.is_open());
    const json fixture = json::parse(in);
    REQUIRE(fixture.size() == 129);

    const CommandCatalog&    catalog = NativeCommands::instance();
    std::vector<std::string> expected;
    for (const json& item : fixture) {
        const std::string key = item.at("key");
        INFO(key);
        expected.push_back(key);
        const auto [command, command_action] = Slic3r::GUI::find_command_action(catalog, key);
        REQUIRE(command != nullptr);
        const std::unique_ptr<Slic3r::GUI::AppAction> action = command_action != nullptr ?
                                                                   NativeCommands::make_action(*command, *command_action) :
                                                                   NativeCommands::make_action(*command);
        REQUIRE(action != nullptr);
        CHECK(action->id() == "orca_command:" + key + ":orca");
        CHECK(action->title_en == item.at("title").get<std::string>());
        CHECK(action->group_en == item.at("group").get<std::string>());
        CHECK(action->icon == item.at("icon").get<std::string>());
        CHECK(action->input == item.at("input").get<std::string>());
    }

    std::vector<std::string> actions;
    for (const NativeCommands::SpeedDialAction& t : NativeCommands::speed_dial_actions(catalog))
        actions.push_back(t.action != nullptr ? t.action->at("key").get<std::string>() : t.command->key);
    std::sort(expected.begin(), expected.end());
    std::sort(actions.begin(), actions.end());
    CHECK(actions == expected);
}

TEST_CASE("a menu item's id is an action's key, not a command", "[CommandCatalog][SpeedDial]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    REQUIRE(kRetiredMenuItems.size() == 121);
    for (const std::string& item : kRetiredMenuItems) {
        INFO(item);
        CHECK(catalog.find(item) == nullptr);
        const auto [command, action] = Slic3r::GUI::find_command_action(catalog, item);
        CHECK(command != nullptr);
        CHECK(action != nullptr);
    }
}

TEST_CASE("every action is its command's, with the design's arguments, and they fit its schema", "[CommandCatalog][SpeedDial]")
{
    // Each of the 129 actions with the command the design folds its menu item into and the arguments it runs it with.
    std::ifstream in(std::string(TEST_DATA_DIR) + "/speed_dial_action_calls.json", std::ios::binary);
    REQUIRE(in.is_open());
    const json fixture = json::parse(in);
    REQUIRE(fixture.size() == 129);

    const CommandCatalog& catalog = NativeCommands::instance();
    std::size_t           folded  = 0;
    for (const json& item : fixture) {
        const std::string key  = item.at("key");
        const std::string name = item.at("command");
        INFO(key << " -> " << name);
        const auto [command, action] = Slic3r::GUI::find_command_action(catalog, key);
        REQUIRE(command != nullptr);
        REQUIRE(command->key == name);
        ++folded;
        if (key == name) {
            // The command's own action, a person's form of it.
            CHECK(command->has_own_action);
            CHECK(action == nullptr);
            CHECK(item.at("args") == json::object());
            CHECK(Mcp::person_form(*command, json::object()));
            json none = Slic3r::GUI::action_call_args(nullptr, json::object());
            CHECK(Mcp::argument_error(*command, none).empty());
            continue;
        }
        REQUIRE(action != nullptr);
        CHECK(action->at("args") == item.at("args"));
        const std::string input = item.value("input", std::string());
        CHECK(action->value("input", std::string()) == input);
        CHECK(action->value("input_arg", std::string()) == item.value("input_arg", std::string()));
        // An action that collects nothing is a person's form of its command; one that collects a value is not, as its
        // value comes from the Speed Dial's second phase.
        CHECK(Mcp::person_form(*command, action->at("args")) == input.empty());
        // What a pick runs the command with: the action's arguments, and the value its second phase collects.
        json value = json::object();
        if (!input.empty())
            value[input] = input == "percent" ? json(50) : json("preview");
        json call = Slic3r::GUI::action_call_args(action, value);
        CHECK(Mcp::argument_error(*command, call).empty());
    }
    // Every action of today, under the command the design folds its menu item into.
    CHECK(folded == 129);

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
    REQUIRE(pairs.size() == 60);
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

TEST_CASE("outside the Design tab the catalog has its commands and 129 actions", "[CommandCatalog]")
{
    // The section the Speed Dial shows each action in, by its key.
    std::ifstream in(std::string(TEST_DATA_DIR) + "/speed_dial_actions.json", std::ios::binary);
    REQUIRE(in.is_open());
    std::map<std::string, std::string> section;
    for (const json& item : json::parse(in))
        section[item.at("key").get<std::string>()] = item.at("group").get<std::string>();

    std::size_t           commands = 0, items = 0;
    std::set<std::string> own;
    for (const NativeCommand& c : NativeCommands::instance().commands()) {
        if (c.group == Slic3r::GUI::kDesignGroup)
            continue;
        ++commands;
        if (c.actions.is_array())
            items += c.actions.size();
        if (!c.has_own_action)
            continue;
        INFO(c.key);
        own.insert(c.key);
        // An own action is in the section the Speed Dial shows its key in.
        CHECK(c.action_group == section.at(c.key));
    }
    // The commands outside the Design tab: the typed tools, the commands made from menu items, the seven Parts 4a to 4c
    // add, Part 5a's three, Part 5b's two, Part 5c's one, Part 5d's four, the seven gizmo commands, the six printing
    // commands, the four of the filament list (filament_list, filament_edit_mixed, filament_set_colour and
    // filament_set_flushing), tool_describe and script_describe_api.
    CHECK(commands == 137);
    // The eight commands named as their menu items keep an action of their own; the other 121 actions are items.
    CHECK(own == std::set<std::string>{"plate_add", "plate_delete", "plate_duplicate", "plate_rename", "filament_add", "filament_delete",
                                       "undo", "redo"});
    CHECK(items == 121);
}

TEST_CASE("the commands outside the Design tab are the design's, under its headings", "[CommandCatalog]")
{
    std::map<std::string, std::set<std::string>> by_heading;
    for (const NativeCommand& c : NativeCommands::instance().commands()) {
        if (c.group == Slic3r::GUI::kDesignGroup)
            continue;
        INFO(c.key);
        // A command whose name starts with no place has no heading.
        const char* heading = NativeCommands::place_heading(c.key);
        REQUIRE(heading != nullptr);
        CHECK(c.group == heading);
        by_heading[c.group].insert(c.key);
    }
    std::size_t commands = 0;
    for (const auto& [heading, keys] : kPlaces) {
        INFO(heading);
        CHECK(by_heading[heading] == keys);
        commands += keys.size();
    }
    CHECK(by_heading.size() == kPlaces.size());
    CHECK(commands == 137);

    // tool_list's group says which there are.
    const NativeCommand* list = NativeCommands::find("tool_list");
    REQUIRE(list != nullptr);
    const std::string group = list->input_schema.at("properties").at("group").at("description").get<std::string>();
    for (const auto& place : kPlaces)
        CHECK_THAT(group, ContainsSubstring(place.first));
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

TEST_CASE("every command that runs from arguments is in the tool list, by place", "[CommandCatalog]")
{
    const CommandCatalog&    catalog = NativeCommands::instance();
    std::vector<std::string> listed;
    std::set<std::string>    unlisted;
    for (const NativeCommand& c : catalog.commands()) {
        if (c.group == Slic3r::GUI::kDesignGroup)
            continue;
        if (c.listed)
            listed.push_back(c.key);
        else
            unlisted.insert(c.key);
    }
    // The three menu items with no argument form, which tool_list lists and tool_call runs.
    CHECK(unlisted == std::set<std::string>{"window_save_screenshot", "script_toggle_panel", "settings_toggle_developer_mode"});
    // Task 3's 98, the seven Parts 4a to 4c add, Part 5a's three, Part 5b's two, Part 5c's one, Part 5d's four, the
    // seven gizmo commands, the six printing commands, the filament list's four, tool_describe and script_describe_api.
    CHECK(listed.size() == 134);

    // The listing of the other commands, the manual, the generic call and the app's state first; then each heading's
    // commands together, in the design's order, Driving the GUI last.
    REQUIRE(listed.size() > 4);
    const std::vector<std::string> first{"tool_list", "tool_describe", "tool_call", "app_state"};
    CHECK(std::vector<std::string>(listed.begin(), listed.begin() + 4) == first);
    std::vector<std::string> headings;
    for (const auto& place : kPlaces)
        headings.push_back(place.first);
    CHECK(group_runs(catalog, std::vector<std::string>(listed.begin() + 4, listed.end())) == headings);

    // With the Design tab, its commands sit between the last of the places and Driving the GUI.
    CommandCatalog with_design = catalog_with_design();
    with_design.seal();
    const std::vector<std::string> keys = keys_of(with_design.commands());
    REQUIRE(keys.size() > 4);
    std::vector<std::string> with_design_headings = headings;
    with_design_headings.insert(with_design_headings.end() - 1, Slic3r::GUI::kDesignGroup);
    CHECK(group_runs(with_design, std::vector<std::string>(keys.begin() + 4, keys.end())) == with_design_headings);
}

TEST_CASE("only deleting a preset, uninstalling a plugin and stopping a print are labelled destructive", "[CommandCatalog]")
{
    // The reads: they change nothing in the app or the project.
    const std::set<std::string> reads{"app_state",          "app_wait",       "app_log",         "tool_list",         "tool_describe",
                                      "preset_list",        "settings_list",  "settings_get",    "preferences_get",
                                      "plate_get_settings", "project_get_settings", "plate_list", "object_list",
                                      "window_screenshot",  "plate_slice_stats",    "script_list_api", "script_describe_api", "action_list",
                                      "window_list",        "widget_list",          "printer_list_models", "plugin_list",
                                      "printer_status",     "filament_list"};
    // The commands made from menu items whose item reaches a printer, the cloud account or the network.
    const std::set<std::string> menu_items_reaching_out{"printer_send",          "preset_sync",       "filament_sync_ams",
                                                        "app_check_for_updates", "app_open_web_page", "plugin_refresh"};
    // The plugin commands that run a plugin's code or reach the cloud account: a plugin switched on or loaded again
    // runs its code, a cloud plugin is downloaded, and Unsubscribe ends a subscription.
    const std::set<std::string> plugins_reaching_out{"plugin_enable", "plugin_reload", "plugin_uninstall"};
    // The printing commands: the Print button's dialogs print on a printer, the host test asks the print host, and the
    // job commands send the printer a command. printer_status reads what the app holds and reaches nothing.
    const std::set<std::string> printing_reaching_out{"print_start", "printer_test_host", "print_pause", "print_resume", "print_stop"};
    std::set<std::string> destructive, read_only;
    for (const NativeCommand& c : NativeCommands::instance().commands()) {
        if (c.group == Slic3r::GUI::kDesignGroup)
            continue;
        INFO(c.key);
        if (c.destructive)
            destructive.insert(c.key);
        if (c.read_only)
            read_only.insert(c.key);
        if (c.listed) {
            CHECK_FALSE(c.description.empty());
            CHECK(c.input_schema.value("type", std::string()) == "object");
        }
    }
    CHECK(destructive == std::set<std::string>{"preset_delete", "plugin_uninstall", "print_stop"});
    CHECK(read_only == reads);
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

TEST_CASE("the four commands with a person's form take it with no arguments", "[CommandCatalog]")
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
    // Every command's action in the Speed Dial, by its key: a typed tool is there only through its menu items' actions.
    const std::set<std::string> actions{
        "add_handy_3dbenchy", "add_handy_autodesk_fdm_test", "add_handy_cali_cat", "add_handy_orca_badge",
        "add_handy_orca_cube", "add_handy_orca_string_hell", "add_handy_orca_tolerance_test", "add_handy_orcasliced_combo",
        "add_handy_stanford_bunny", "add_handy_voron_cube", "add_primitive_cone", "add_primitive_cube",
        "add_primitive_cylinder", "add_primitive_disc", "add_primitive_sphere", "add_primitive_svg", "add_primitive_text",
        "add_primitive_torus", "calib_cornering", "calib_flow_ratio", "calib_input_shaping_damp", "calib_input_shaping_freq",
        "calib_max_volumetric", "calib_pressure_advance", "calib_retraction", "calib_temperature", "calib_vfa", "deselect_all",
        "export_3mf", "export_all_sliced_file", "export_config", "export_drc_multi", "export_drc_single", "export_gcode",
        "export_sliced_file", "export_stl", "export_stl_multi", "export_toolpaths_obj", "filament_add", "filament_delete",
        "go_to_layer", "go_to_tab", "help_about", "help_benchmark_3d_scene", "help_check_updates", "help_keyboard_shortcuts",
        "help_migrate_filament_presets", "help_network_test", "help_open_config_folder", "help_setup_wizard",
        "help_tip_of_the_day", "help_troubleshoot", "import_configs", "import_file", "import_zip_archive",
        "install_local_plugin", "install_plugin", "load_project", "mode_advanced", "mode_expert", "mode_simple", "new_project",
        "obj_arrange", "obj_assemble", "obj_center", "obj_delete", "obj_delete_all", "obj_drop", "obj_fit_volume",
        "obj_instances_down", "obj_instances_to_objects", "obj_instances_up", "obj_mirror_x", "obj_mirror_y", "obj_mirror_z",
        "obj_order_by_filament", "obj_order_by_height", "obj_order_by_name", "obj_order_by_size", "obj_orient", "obj_rename",
        "obj_set_instances", "obj_split_objects", "obj_split_parts", "obj_type_modifier", "obj_type_negative", "obj_type_part",
        "obj_type_precise_seam", "obj_type_support_blocker", "obj_type_support_enforcer", "open_action_replay", "open_plugins",
        "open_preferences", "open_wiki", "open_youtube", "plate_add", "plate_delete", "plate_duplicate", "plate_goto",
        "plate_rename", "plate_toggle_lock", "preset_bundle", "redo", "refresh_plugins", "reset_window_layout", "save_project",
        "save_project_as", "select_all", "send_gcode", "slice_all", "slice_and_preview", "start_recording", "stop_recording",
        "sync_ams", "sync_presets", "take_screenshot", "toggle_developer_mode", "toggle_run_script_panel", "undo", "view_bottom",
        "view_default", "view_fit_bed", "view_front", "view_iso", "view_left", "view_rear", "view_right",
        "view_toggle_perspective", "view_top"};
    REQUIRE(actions.size() == 129);

    std::set<std::string> speed_dial;
    for (const NativeCommands::SpeedDialAction& t : NativeCommands::speed_dial_actions(NativeCommands::instance()))
        speed_dial.insert(t.action != nullptr ? t.action->at("key").get<std::string>() : t.command->key);
    CHECK(speed_dial == actions);
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
        ToolResult call(const NativeCommand&, const json&) override { return ToolResult::error("no app"); }
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
    CHECK_THROWS_AS(catalog.leave_out("shell", "too late", "feature_off"), std::logic_error);

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
    for (const std::string key : {"extrude", "hole", "describe_design"}) {
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
    CHECK(no_cad.left_out("extrude") == nullptr);
#endif
    CHECK(no_cad.left_out("object_arrange") == nullptr);
}

TEST_CASE("every expected-id argument of a schema is in id_args", "[CommandCatalog]")
{
    for (const NativeCommand& c : NativeCommands::instance().commands()) {
        INFO(c.key);
        std::set<std::string> in_schema;
        if (c.input_schema.is_object() && c.input_schema.contains("properties"))
            for (const char* name : {"object_id", "part_id", "object_ids", "part_ids", "instance_object_ids"})
                if (c.input_schema["properties"].contains(name))
                    in_schema.insert(name);
        CHECK(std::set<std::string>(c.id_args.begin(), c.id_args.end()) == in_schema);
    }
    for (const char* key : {"object_transform", "object_select", "settings_set"}) {
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
    CommandCatalog catalog = catalog_with_design();
    catalog.seal();
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
    CommandCatalog catalog = catalog_with_design();
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
        ToolResult call(const NativeCommand&, const json&) override { return ToolResult::error("no app"); }
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
    CommandCatalog catalog = catalog_with_design();
    catalog.seal();
    const json reference = Slic3r::GUI::catalog_reference(catalog);
    // Each with what its person's forms are: {} alone, or the arguments of each of its actions.
    std::vector<std::pair<std::string, json>> two_forms{
        {"project_save", {{"not", {{"enum", json::array({json::object(), {{"save_as", true}}})}}}}}};
#ifdef SLIC3R_CAD
    two_forms.emplace_back("fillet", json{{"minProperties", 1}});
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

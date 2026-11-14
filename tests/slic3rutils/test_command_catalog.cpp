#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/Mcp/McpPersonContext.hpp"
#include "slic3r/GUI/Mcp/McpProtocol.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
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
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
using Slic3r::GUI::AppActionRunResult;
using Slic3r::GUI::CommandCatalog;
using Slic3r::GUI::CommandCompletion;
using Slic3r::GUI::CommandFn;
using Slic3r::GUI::CommandSource;
using Slic3r::GUI::NativeCommand;
namespace Mcp            = Slic3r::GUI::Mcp;
namespace NativeCommands = Slic3r::GUI::NativeCommands;
namespace Ops            = Slic3r::GUI::Ops;

namespace {

NativeCommand command(std::string key, bool in_palette = false)
{
    NativeCommand c;
    c.key        = std::move(key);
    c.in_palette = in_palette;
    return c;
}

json no_arguments() { return {{"type", "object"}, {"properties", json::object()}}; }

// A call's context without the MCP core: UI closures run in place and nothing is left to wait for. It
// counts the operations a call begins and records the commands it submits as jobs.
class TestContext final : public Mcp::ToolContext
{
public:
    explicit TestContext(bool person = false) : m_person(person) {}

    bool           person() const override { return m_person; }
    json           run_ui(const std::function<json()>& fn) override { return fn(); }
    json           observe_ui(const std::function<json()>& /*fn*/) override { return true; }
    const std::string& job_id() const override { return m_job; }
    bool           cancelled() const override { return false; }
    Mcp::ToolResult call_child(const std::string& /*tool*/, const json& /*args*/) override { return Mcp::ToolResult::error("no child calls"); }
    std::set<std::string> lineage() const override { return {}; }
    void                  set_journal_args(json args) override { journaled = std::move(args); }
    void                  begin_operation() override { ++operations; }
    Mcp::ToolResult       submit(const std::string& key, const json& /*args*/) override
    {
        submitted.push_back(key);
        return Mcp::ToolResult::ok();
    }

    int                      operations = 0;
    std::vector<std::string> submitted;
    json                     journaled; // what the call last recorded for the journal

private:
    bool        m_person;
    std::string m_job;
};

// Holds one command, as a plugin's operations are held while it is loaded.
class OneCommandSource final : public CommandSource
{
public:
    explicit OneCommandSource(std::string key) : m_command(std::make_shared<const NativeCommand>(command(std::move(key)))) {}

    std::shared_ptr<const NativeCommand> find(const std::string& key) const override
    {
        return key == m_command->key ? m_command : nullptr;
    }
    std::vector<std::shared_ptr<const NativeCommand>> all() const override { return {m_command}; }

private:
    std::shared_ptr<const NativeCommand> m_command;
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

} // namespace

TEST_CASE("a key added twice is refused", "[CommandCatalog]")
{
    CommandCatalog catalog;
    catalog.add(command("a"));
    CHECK_THROWS_AS(catalog.add(command("a")), std::logic_error);
    CHECK(catalog.commands().size() == 1);
}

TEST_CASE("a sealed catalog refuses additions", "[CommandCatalog]")
{
    CommandCatalog catalog;
    catalog.add(command("a"));
    CHECK_FALSE(catalog.sealed());
    catalog.seal();
    CHECK(catalog.sealed());
    CHECK_THROWS_AS(catalog.add(command("b")), std::logic_error);
    CHECK_THROWS_AS(catalog.add_source(std::make_unique<OneCommandSource>("plugin.x.y")), std::logic_error);
    CHECK(catalog.find("b") == nullptr);
}

TEST_CASE("find sees added commands, lookup also a source's", "[CommandCatalog]")
{
    CommandCatalog catalog;
    catalog.add(command("slice"));
    catalog.add_source(std::make_unique<OneCommandSource>("plugin.x.y"));
    catalog.seal();

    CHECK(catalog.find("plugin.x.y") == nullptr);
    const std::shared_ptr<const NativeCommand> from_source = catalog.lookup("plugin.x.y");
    REQUIRE(from_source != nullptr);
    CHECK(from_source->key == "plugin.x.y");

    const NativeCommand* added = catalog.find("slice");
    REQUIRE(added != nullptr);
    CHECK(catalog.lookup("slice").get() == added);

    CHECK(catalog.lookup("plugin.x.z") == nullptr);
    REQUIRE(catalog.source_commands().size() == 1);
    CHECK(catalog.source_commands().front()->key == "plugin.x.y");
}

TEST_CASE("commands keep the order they were added in", "[CommandCatalog]")
{
    CommandCatalog catalog;
    for (const char* key : {"c", "a", "b"})
        catalog.add(command(key));
    catalog.seal();
    CHECK(keys_of(catalog.commands()) == std::vector<std::string>{"c", "a", "b"});
}

TEST_CASE("a Design command's title is translated in the Design context, any other's in none", "[CommandCatalog]")
{
    NativeCommand design;
    design.key   = "shell";
    design.title = "Shell";
    design.group = Slic3r::GUI::kDesignGroup;
    REQUIRE(Slic3r::GUI::title_context(design) != nullptr);
    CHECK(std::string(Slic3r::GUI::title_context(design)) == "Design");
    NativeCommand other;
    other.key   = "save_project";
    other.title = "Save project";
    other.group = "Commands";
    CHECK(Slic3r::GUI::title_context(other) == nullptr);
}

TEST_CASE("the process catalog's palette is the Speed Dial's commands", "[CommandCatalog]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    CHECK(catalog.sealed());
    const std::vector<const NativeCommand*> palette = NativeCommands::palette_commands(catalog);
    CHECK(palette.size() == 129);
    for (const NativeCommand* c : palette) {
        INFO(c->key);
        CHECK(c->in_palette);
    }
    // The Speed Dial's own lookup reads the same catalog.
    CHECK(NativeCommands::find("slice_all") == catalog.find("slice_all"));
}

TEST_CASE("every key of the process catalog is unique, tools and commands together", "[CommandCatalog]")
{
    std::set<std::string> keys;
    for (const NativeCommand& c : NativeCommands::instance().commands()) {
        INFO(c.key);
        CHECK(keys.insert(c.key).second);
    }
    // The Speed Dial's commands and the typed tools are both there.
    CHECK(keys.count("obj_arrange") == 1);
    CHECK(keys.count("get_app_state") == 1);
}

TEST_CASE("the typed tools are listed and the four merged commands are both", "[CommandCatalog]")
{
    for (const char* key : {"new_project", "save_project", "start_recording", "stop_recording"}) {
        INFO(key);
        const NativeCommand* merged = NativeCommands::find(key);
        REQUIRE(merged != nullptr);
        CHECK(merged->listed);
        CHECK(merged->in_palette);
        // The Speed Dial's title and section.
        CHECK(merged->group == "Commands");
        CHECK_FALSE(merged->icon.empty());
    }
    CHECK(NativeCommands::find("new_project")->title == "New Project");

    const NativeCommand* tool = NativeCommands::find("get_app_state");
    REQUIRE(tool != nullptr);
    CHECK(tool->listed);
    CHECK_FALSE(tool->in_palette);

    const NativeCommand* tile = NativeCommands::find("obj_arrange");
    REQUIRE(tile != nullptr);
    CHECK_FALSE(tile->listed);
    CHECK(tile->in_palette);
}

TEST_CASE("no typed tool is in the palette", "[CommandCatalog]")
{
    // Every tile of the Speed Dial: its own commands, the four merged ones among them, and no other typed tool.
    const std::set<std::string> tiles{
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
        "sync_ams", "sync_presets", "take_screenshot", "toggle_developer_mode", "toggle_run_script", "undo", "view_bottom",
        "view_default", "view_fit_bed", "view_front", "view_iso", "view_left", "view_rear", "view_right",
        "view_toggle_perspective", "view_top"};
    REQUIRE(tiles.size() == 129);

    std::set<std::string> palette;
    for (const NativeCommand* c : NativeCommands::palette_commands(NativeCommands::instance()))
        palette.insert(c->key);
    CHECK(palette == tiles);
    CHECK(palette.count("new_project") == 1);
    CHECK(palette.count("get_app_state") == 0);
    CHECK(palette.count("slice") == 0);
}

TEST_CASE("a merged command accepts silent and publishes its form", "[CommandCatalog]")
{
    const NativeCommand* save = NativeCommands::find("save_project");
    REQUIRE(save != nullptr);
    CHECK_FALSE(save->input_schema.contains("required"));
    CHECK(save->input_schema["then"]["required"] == json::array({"path"}));
    json none = json::object();
    CHECK(Mcp::argument_error(*save, none).empty());
    json path{{"path", "C:/a.3mf"}};
    CHECK(Mcp::argument_error(*save, path).empty());
    json silent{{"silent", true}};
    CHECK_THAT(Mcp::argument_error(*save, silent), ContainsSubstring("path"));

    const NativeCommand* fresh = NativeCommands::find("new_project");
    REQUIRE(fresh != nullptr);
    const json& properties = fresh->input_schema["properties"];
    for (const char* name : {"silent", "discard_changes", "preset_changes"}) {
        INFO(name);
        CHECK(properties.contains(name));
    }
}

TEST_CASE("a merged command's call with no arguments is the person's form", "[CommandCatalog]")
{
    int        tool_calls = 0, person_calls = 0;
    const json schema{{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}, {"required", {"path"}}};
    const auto tool = NativeCommands::long_command("k", "K", "", schema, [&tool_calls](const json&, Mcp::ToolContext&) {
        ++tool_calls;
        return Mcp::ToolResult::ok({{"tool", true}});
    });
    const NativeCommand merged = NativeCommands::with_person_form(tool, "K", "Commands", "menu_save", [&person_calls] {
        ++person_calls;
        return AppActionRunResult{AppActionRunResult::Level::Success, "Asked"};
    });
    CHECK(merged.in_palette);
    CHECK(merged.title == "K");
    CHECK(merged.group == "Commands");
    CHECK(merged.icon == "menu_save");
    CHECK(merged.input_schema["properties"].contains("silent"));

    TestContext           agent;
    const Mcp::ToolResult asked = merged.run(json::object(), agent);
    CHECK(person_calls == 1);
    CHECK(tool_calls == 0);
    CHECK(asked.data["ran"] == "k");
    const Mcp::ToolResult given = merged.run({{"path", "a"}}, agent);
    CHECK(person_calls == 1);
    CHECK(tool_calls == 1);
    CHECK(given.data["tool"] == true);
}

TEST_CASE("an agent's call of a person's form is checked as a mutate call, a person's is not", "[CommandCatalog]")
{
    int  person_calls = 0;
    auto tool         = NativeCommands::long_command("k", "K", "", no_arguments(), [](const json&, Mcp::ToolContext&) {
        return Mcp::ToolResult::ok();
    });
    tool.guard        = Mcp::GuardClass::Inspect; // as the recording tools run, behind dialogs
    const auto person = [&person_calls] {
        ++person_calls;
        return AppActionRunResult{AppActionRunResult::Level::Success, "Asked"};
    };
    const NativeCommand opens = NativeCommands::with_person_form(tool, "K", "Commands", "", person);
    const NativeCommand quiet = NativeCommands::with_person_form(tool, "K", "Commands", "", person, /*person_form_behind_dialogs=*/true);

    // An agent's call asks the service's UI for the mutate check before the person's form runs: a context
    // without a service refuses it, and the form never runs.
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
    const NativeCommand tool = NativeCommands::long_command("k", "K", "", no_arguments(), [](const json& args, Mcp::ToolContext& ctx) {
        ctx.set_journal_args(args);
        return Mcp::ToolResult::ok();
    });
    const NativeCommand merged = NativeCommands::with_person_form(tool, "K", "Commands", "", [] { return AppActionRunResult{}; });

    TestContext agent;
    merged.run({{"silent", true}}, agent);
    CHECK(agent.journaled == json{{"silent", true}});
}

TEST_CASE("a recording started with arguments lasts an hour by default", "[CommandCatalog]")
{
    const NativeCommand* start = NativeCommands::find("start_recording");
    REQUIRE(start != nullptr);
    CHECK_THAT(start->input_schema["properties"]["max_seconds"]["description"].get<std::string>(), ContainsSubstring("default 3600"));
}

TEST_CASE("the catalog needs no service", "[CommandCatalog]")
{
    // No McpService is made in this test: the catalog is built without one, and its listing is the tool list.
    const CommandCatalog& catalog = NativeCommands::instance();
    struct NoApp final : Mcp::ToolCaller
    {
        Mcp::ToolResult call(const NativeCommand&, const json&) override { return Mcp::ToolResult::error("no app"); }
    } caller;
    Mcp::McpProtocol protocol(catalog, caller, Mcp::ServerInfo{});
    const Mcp::PostReply reply = protocol.handle_post({json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}.dump(), {}, {}, {}});
    REQUIRE(reply.status == 200);
    const json            tools = json::parse(reply.body)["result"]["tools"];
    std::set<std::string> names;
    for (const json& tool : tools)
        names.insert(tool["name"].get<std::string>());
    CHECK(names.count("get_app_state") == 1);
    CHECK(names.count("call_tool") == 1);
    CHECK(names.count("obj_arrange") == 0);
}

TEST_CASE("a Speed Dial command is found by the one lookup", "[CommandCatalog]")
{
    const CommandCatalog& catalog = NativeCommands::instance();
    CHECK(catalog.lookup("obj_arrange") != nullptr);
    // A misspelt key is answered with the closest command, which call_tool runs, not with a tile's run_action.
    const std::string unknown = catalog.unknown_command("obj_arange", /*suggest=*/true);
    CHECK_THAT(unknown, ContainsSubstring("obj_arrange"));
    CHECK_THAT(unknown, !ContainsSubstring("run_action"));
}

TEST_CASE("a command the catalog leaves out is unknown with the reason", "[CommandCatalog]")
{
    CommandCatalog catalog;
    catalog.add(command("hold"));
    catalog.leave_out("holes", "the feature was off");
    catalog.seal();
    REQUIRE(catalog.left_out("holes") != nullptr);
    CHECK(*catalog.left_out("holes") == "the feature was off");
    CHECK(catalog.left_out("hold") == nullptr);
    // The reason, and no close key: the key names the command the caller meant.
    CHECK(catalog.unknown_command("holes", /*suggest=*/true) == "Unknown tool: holes; the feature was off");
    CHECK(catalog.unknown_command("holx", /*suggest=*/true) == "Unknown tool: holx (did you mean hold?)");
    CHECK_THROWS_AS(catalog.leave_out("shell", "too late"), std::logic_error);

    // A run with the CAD feature off at startup leaves out the Design commands, and says how a person turns it
    // on; a build without CAD has no Design commands to leave out.
    CommandCatalog no_cad;
    NativeCommands::register_all_commands(no_cad, {false});
    no_cad.seal();
#ifdef SLIC3R_CAD
    for (const std::string key : {"extrude", "hole", "describe_scene"}) {
        INFO(key);
        CHECK(no_cad.unknown_command(key, /*suggest=*/true) ==
              "Unknown tool: " + key + "; the CAD feature was off when OrcaSlicer started: a person turns it on in "
                                       "Preferences > Features and restarts OrcaSlicer");
    }
#else
    CHECK(no_cad.left_out("extrude") == nullptr);
#endif
    CHECK(no_cad.left_out("obj_arrange") == nullptr);
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
    for (const char* key : {"transform", "select_objects", "set_settings"}) {
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

TEST_CASE("the palette is the commands with in_palette", "[CommandCatalog]")
{
    CommandCatalog catalog;
    catalog.add(command("first", true));
    catalog.add(command("typed_tool", false));
    catalog.add(command("last", true));
    catalog.seal();

    const std::vector<const NativeCommand*> palette = NativeCommands::palette_commands(catalog);
    REQUIRE(palette.size() == 2);
    CHECK(palette[0] == catalog.find("first"));
    CHECK(palette[1] == catalog.find("last"));
}

TEST_CASE("a sealed catalog is read from eight threads", "[CommandCatalog]")
{
    // Keys the catalog holds, misspells and lacks; each thread asks about them in turn.
    const std::vector<std::string> keys{"slice_all", "undo", "slice_al", "export_gcod", "no_such_command"};
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
    CHECK(first[0].found->key == "slice_all");
    CHECK(first[2].found == nullptr);
    REQUIRE_FALSE(first[2].similar.empty());
    CHECK(first[2].similar.front() == "slice_all");
    CHECK(first[4].found == nullptr);
}

TEST_CASE("the person's form is the call with no arguments", "[CommandCatalog]")
{
    CHECK(Mcp::person_form(json::object()));
    CHECK(Mcp::person_form(json()));
    CHECK_FALSE(Mcp::person_form({{"silent", true}}));
    CHECK_FALSE(Mcp::person_form({{"path", "a"}}));
    CHECK(Mcp::silent_args({{"silent", true}, {"path", "a"}}) == json{{"path", "a"}});
}

TEST_CASE("a two-form schema requires its arguments only when some are given", "[CommandCatalog]")
{
    const json s = Mcp::with_silent({{"type", "object"},
                                     {"properties", {{"path", {{"type", "string"}}}, {"overwrite", {{"type", "boolean"}}}}},
                                     {"required", {"path"}},
                                     {"additionalProperties", false}});
    CHECK_FALSE(s.contains("required"));
    CHECK(s["then"]["required"] == json::array({"path"}));
    CHECK(s["properties"]["silent"]["enum"] == json::array({true}));
    CHECK(Mcp::validate_arguments(s, json::object()).empty());
    CHECK(Mcp::validate_arguments(s, {{"path", "a"}}).empty());
    CHECK_THAT(Mcp::validate_arguments(s, {{"overwrite", true}}), ContainsSubstring("path"));
    CHECK_THAT(Mcp::validate_arguments(s, {{"silent", true}}), ContainsSubstring("path"));
    CHECK_THAT(Mcp::validate_arguments(s, {{"silent", false}}), ContainsSubstring("silent"));
}

TEST_CASE("a person's pick runs the function in place", "[CommandCatalog]")
{
    int                 calls = 0;
    const NativeCommand c     = NativeCommands::ui_command("k", "K", "Commands", no_arguments(), [&calls](const json&) {
        ++calls;
        return AppActionRunResult{AppActionRunResult::Level::Success, "Done"};
    });
    Mcp::PersonContext    person;
    const Mcp::ToolResult result = c.run(json::object(), person);
    CHECK(calls == 1);
    REQUIRE(result.notice != nullptr);
    CHECK(result.notice->level == AppActionRunResult::Level::Success);
    CHECK(result.notice->message == "Done");
}

TEST_CASE("a long command picked by a person is submitted", "[CommandCatalog]")
{
    int             calls = 0;
    const CommandFn fn    = [&calls](const json&, Mcp::ToolContext&) {
        ++calls;
        return Mcp::ToolResult::ok();
    };
    TestContext person(true);
    NativeCommands::long_command("k", "K", "Commands", no_arguments(), fn).run(json::object(), person);
    CHECK(person.submitted == std::vector<std::string>{"k"});
    CHECK(calls == 0);

    const NativeCommand in_place = NativeCommands::long_command("k", "K", "Commands", no_arguments(), fn, true);
    CHECK(in_place.inline_call);
    in_place.run(json::object(), person);
    CHECK(calls == 1);
    CHECK(person.submitted.size() == 1);

    // An agent's call is already a job: the function runs.
    TestContext agent;
    NativeCommands::long_command("k", "K", "Commands", no_arguments(), fn).run(json::object(), agent);
    CHECK(calls == 2);
    CHECK(agent.submitted.empty());
}

TEST_CASE("a person's submit refuses an unknown command, and starts no job without OrcaSlicer", "[CommandCatalog]")
{
    // The lookup comes first; starting the runtime, the job and its notices need the app, which no unit test runs.
    Mcp::PersonContext person;
    CHECK_THROWS_WITH(person.submit("no_such_command", json::object()), ContainsSubstring("Unknown command no_such_command"));
    CHECK_THROWS_AS(person.submit("run_script", json::object()), Mcp::ToolFailure);
}

TEST_CASE("an Export command called with arguments takes the project, and with none does not", "[CommandCatalog]")
{
    const auto fails = [](const json&) { return AppActionRunResult{AppActionRunResult::Level::Error, "Nothing to export", false}; };
    const NativeCommand exporting = NativeCommands::ui_command("k", "K", "Commands", no_arguments(), fails, CommandCompletion::Export);
    CHECK(exporting.completion == CommandCompletion::Export);

    TestContext           given;
    const Mcp::ToolResult refused = exporting.run({{"path", "a.gcode"}}, given);
    CHECK(given.operations == 1);
    CHECK(refused.is_error);
    CHECK(refused.message == "Nothing to export");

    TestContext none;
    exporting.run(json::object(), none);
    CHECK(none.operations == 0);

    const NativeCommand plain = NativeCommands::ui_command("k", "K", "Commands", no_arguments(), fails);
    TestContext         either;
    plain.run({{"path", "a.gcode"}}, either);
    plain.run(json::object(), either);
    CHECK(either.operations == 0);
}

TEST_CASE("a command with a second phase refuses a call without its value", "[CommandCatalog]")
{
    json none = json::object();
    CHECK_THAT(Mcp::argument_error(*NativeCommands::find("go_to_tab"), none), ContainsSubstring("tab"));
    none = json::object();
    CHECK_THAT(Mcp::argument_error(*NativeCommands::find("go_to_layer"), none), ContainsSubstring("percent"));
    json percent{{"percent", 50}};
    CHECK(Mcp::argument_error(*NativeCommands::find("go_to_layer"), percent).empty());
}

TEST_CASE("a rename to an empty name is refused before anything is renamed", "[CommandCatalog]")
{
    const NativeCommand* rename = NativeCommands::find("obj_rename");
    REQUIRE(rename != nullptr);
    // No app runs here: the refusal comes before the command looks at the object list.
    TestContext           agent;
    const Mcp::ToolResult refused = rename->run({{"name", ""}}, agent);
    CHECK(refused.is_error);
    CHECK_THAT(refused.message, ContainsSubstring("name"));
}

TEST_CASE("undo takes a history", "[CommandCatalog]")
{
    for (const char* key : {"undo", "redo"}) {
        INFO(key);
        const NativeCommand* command = NativeCommands::find(key);
        REQUIRE(command != nullptr);
        CHECK(command->in_palette);
        CHECK_FALSE(command->listed);
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
        const Mcp::ToolResult picked = command->run(design, person);
        REQUIRE(picked.notice != nullptr);
        CHECK(picked.notice->message.empty());
        CHECK(picked.notice->level != AppActionRunResult::Level::Error);
        // Nothing was done: the project's history is not taken back in the Design history's place.
        CHECK_FALSE(picked.notice->done);
        // An agent's call reaches the Design history's gate, which says why it cannot run.
        TestContext agent;
        CHECK_THROWS_AS(command->run(design, agent), Mcp::ToolFailure);
    }
}

TEST_CASE("a command's result reads as run_action's did", "[CommandCatalog]")
{
    using Level = AppActionRunResult::Level;
    const Mcp::ToolResult done = NativeCommands::from_run_result("k", {Level::Success, "Done"});
    CHECK_FALSE(done.is_error);
    CHECK(done.data == json{{"ran", "k"}, {"level", "success"}, {"message", "Done"}});
    CHECK(NativeCommands::from_run_result("k", {Level::Busy, "Wait"}).is_error);
    CHECK(NativeCommands::from_run_result("k", {Level::Error, "Failed"}).is_error);
    // A command that did nothing fails, though the palette shows it as information.
    CHECK(NativeCommands::from_run_result("k", {Level::Info, "Select an object first.", false}).is_error);

    // What a command adds to its reply joins the reply's fields.
    const Mcp::ToolResult ordered = NativeCommands::from_run_result("k", {Level::Success, {}, true, R"({"descending": true})"});
    CHECK(ordered.data == json{{"ran", "k"}, {"level", "success"}, {"descending", true}});

    Ops::Refusal why;
    why.code  = "no_objects";
    why.msgid = "There are no objects.";
    AppActionRunResult refused{Level::Error, "translated", false};
    refused.refusal              = std::make_shared<const Ops::Refusal>(why);
    const Mcp::ToolResult failed = NativeCommands::from_run_result("k", refused);
    CHECK(failed.is_error);
    CHECK(failed.message == "There are no objects.");
    CHECK(failed.data["code"] == "no_objects");
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
    struct NoApp final : Mcp::ToolCaller
    {
        Mcp::ToolResult call(const NativeCommand&, const json&) override { return Mcp::ToolResult::error("no app"); }
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

TEST_CASE("a reference entry has the command's fields, and its examples only when it has some", "[CommandCatalog][Reference]")
{
    NativeCommand c = command("export_it", true);
    c.title         = "Export It";
    c.group         = "Commands";
    c.description   = "Exports it.";
    c.input_schema  = {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}};
    c.guard         = Mcp::GuardClass::Recover;
    c.completion    = CommandCompletion::Export;
    c.operations    = {"export_gcode"};
    c.open_world    = true;
    const json entry = Slic3r::GUI::command_reference(c);
    CHECK(entry == json{{"key", "export_it"},
                        {"title", "Export It"},
                        {"group", "Commands"},
                        {"description", "Exports it."},
                        {"input_schema", c.input_schema},
                        {"listed", false},
                        {"in_palette", true},
                        {"read_only", false},
                        {"destructive", false},
                        {"idempotent", false},
                        {"open_world", true},
                        {"guard", "recover"},
                        {"completion", "export"},
                        {"operations", json::array({"export_gcode"})}});

    const json call{{"tool", "export_it"}, {"args", {{"path", "a"}}}};
    c.guard          = Mcp::GuardClass::Inspect;
    c.completion     = CommandCompletion::None;
    c.examples       = json::array({json{{"title", "Export"}, {"calls", json::array({call})}}});
    const json other = Slic3r::GUI::command_reference(c);
    CHECK(other["guard"] == "inspect");
    CHECK(other["completion"] == "none");
    CHECK(other["examples"] == c.examples);
    c.guard = Mcp::GuardClass::Mutate;
    CHECK(Slic3r::GUI::command_reference(c)["guard"] == "mutate");
}

TEST_CASE("the reference shows both forms of a two-form command", "[CommandCatalog][Reference]")
{
    CommandCatalog catalog = catalog_with_design();
    catalog.seal();
    const json reference = Slic3r::GUI::catalog_reference(catalog);
    std::vector<std::string> two_forms{"save_project"};
#ifdef SLIC3R_CAD
    two_forms.push_back("fillet");
#endif
    for (const std::string& key : two_forms) {
        INFO(key);
        const json entry = reference_entry(reference, key);
        REQUIRE(entry.is_object());
        const json& schema = entry.at("input_schema");
        CHECK(schema.at("properties").contains("silent"));
        // The silent form's requirement holds once a call gives any argument; the person's form needs none.
        CHECK_FALSE(schema.contains("required"));
        CHECK(schema.at("if") == json{{"minProperties", 1}});
        REQUIRE(schema.at("then").contains("required"));
        CHECK_FALSE(schema.at("then").at("required").empty());
    }
    CHECK(reference_entry(reference, "save_project")["input_schema"]["then"]["required"] == json::array({"path"}));
}

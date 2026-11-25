#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "automation_test_utils.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
using Slic3r::GUI::CommandCatalog;
using Slic3r::GUI::CommandCompletion;
using Slic3r::GUI::CommandFn;
using Slic3r::GUI::GuardClass;
using Slic3r::GUI::NativeCommand;
using Slic3r::GUI::Mcp::Test::command;
using Slic3r::GUI::Mcp::Test::OneCommandSource;
namespace Mcp = Slic3r::GUI::Mcp;

namespace {

std::vector<std::string> keys_of(const std::vector<NativeCommand>& commands)
{
    std::vector<std::string> keys;
    for (const NativeCommand& c : commands)
        keys.push_back(c.key);
    return keys;
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
    catalog.add(command("plate_slice"));
    catalog.add_source(std::make_unique<OneCommandSource>("plugin.x.y"));
    catalog.seal();

    CHECK(catalog.find("plugin.x.y") == nullptr);
    const std::shared_ptr<const NativeCommand> from_source = catalog.lookup("plugin.x.y");
    REQUIRE(from_source != nullptr);
    CHECK(from_source->key == "plugin.x.y");

    const NativeCommand* added = catalog.find("plate_slice");
    REQUIRE(added != nullptr);
    CHECK(catalog.lookup("plate_slice").get() == added);

    CHECK(catalog.lookup("plugin.x.z") == nullptr);
    REQUIRE(catalog.source_commands().size() == 1);
    CHECK(catalog.source_commands().front()->key == "plugin.x.y");
}

TEST_CASE("a set of commands left out together gives its reason by its code", "[CommandCatalog]")
{
    CommandCatalog catalog;
    catalog.add(command("plate_slice"));
    catalog.leave_out("solid_fillet", "the feature was off", "design_unavailable");
    catalog.leave_out("solid_extrude", "the feature was off", "design_unavailable");
    catalog.leave_out("holes", "another reason", "feature_off");
    catalog.seal();
    const std::string* design = catalog.left_out_reason("design_unavailable");
    REQUIRE(design != nullptr);
    CHECK(*design == "the feature was off");
    // The reason is the one a call of any of them gets after its name.
    CHECK(catalog.unknown_command("solid_fillet") == "Unknown tool: solid_fillet; " + *design);
    CHECK(catalog.left_out_reason("no_such_code") == nullptr);
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
    design.key   = "solid_shell";
    design.title = "Shell";
    design.group = Slic3r::GUI::kDesignGroup;
    REQUIRE(Slic3r::GUI::title_context(design) != nullptr);
    CHECK(std::string(Slic3r::GUI::title_context(design)) == "Design");
    NativeCommand other;
    other.key   = "project_save";
    other.title = "Save project";
    other.group = "The project";
    CHECK(Slic3r::GUI::title_context(other) == nullptr);
}

TEST_CASE("a command's function is typed in the catalog's own namespace", "[CommandCatalog]")
{
    STATIC_REQUIRE(std::is_same_v<CommandFn, std::function<Slic3r::GUI::ToolResult(const nlohmann::json&, Slic3r::GUI::ToolContext&)>>);
}

TEST_CASE("the person's form is the call with no arguments", "[CommandCatalog]")
{
    CHECK(Mcp::person_form(json::object()));
    CHECK(Mcp::person_form(json()));
    CHECK_FALSE(Mcp::person_form({{"silent", true}}));
    CHECK_FALSE(Mcp::person_form({{"path", "a"}}));
    CHECK(Mcp::silent_args({{"silent", true}, {"path", "a"}}) == json{{"path", "a"}});
}

TEST_CASE("a call with exactly an action's arguments is the person's form", "[CommandCatalog]")
{
    // A command with an action of its own, an action that mirrors along X and one that collects a tab in the Speed Dial.
    NativeCommand c = command("view_it", /*has_own_action=*/true);
    c.actions       = json::array({json{{"key", "view_x"}, {"title", "X"}, {"group", "View"}, {"icon", ""}, {"description", ""},
                                        {"args", {{"axis", "x"}}}},
                                   json{{"key", "view_tab"}, {"title", "Tab"}, {"group", "Commands"}, {"icon", ""},
                                        {"description", ""}, {"args", json::object()}, {"input", "tab"}}});
    CHECK(Mcp::person_form(c, json()));
    CHECK(Mcp::person_form(c, json::object()));
    CHECK(Mcp::person_form(c, {{"axis", "x"}}));
    CHECK_FALSE(Mcp::person_form(c, {{"axis", "y"}}));
    CHECK_FALSE(Mcp::person_form(c, {{"axis", "x"}, {"silent", true}}));
    CHECK_FALSE(Mcp::person_form(c, {{"silent", true}}));

    // An action that collects a value is no person's form: its value comes from the Speed Dial's second phase.
    NativeCommand collects = command("go_to");
    collects.actions       = json::array({json{{"key", "go_to_tab"}, {"title", "Tab"}, {"group", "Commands"}, {"icon", ""},
                                               {"description", ""}, {"args", json::object()}, {"input", "tab"}}});
    CHECK_FALSE(Mcp::person_form(collects, json::object()));
}

TEST_CASE("what the silent form requires is not asked of a person's form", "[CommandCatalog]")
{
    const json schema{{"type", "object"},
                      {"properties", {{"path", {{"type", "string"}}}, {"save_as", {{"type", "boolean"}}}, {"overwrite", {{"type", "boolean"}}}}},
                      {"required", {"path"}}};
    const json s = Mcp::with_silent(schema, {json::object(), {{"save_as", true}}});
    CHECK(s["if"] == json{{"not", {{"enum", json::array({json::object(), {{"save_as", true}}})}}}});
    CHECK(s["then"]["required"] == json::array({"path"}));
    // The two person's forms and a silent one with what it requires.
    CHECK(Mcp::validate_arguments(s, json::object()).empty());
    CHECK(Mcp::validate_arguments(s, {{"save_as", true}}).empty());
    CHECK(Mcp::validate_arguments(s, {{"path", "a.3mf"}}).empty());
    // Silent forms without it.
    CHECK_THAT(Mcp::validate_arguments(s, {{"overwrite", true}}), ContainsSubstring("path"));
    CHECK_THAT(Mcp::validate_arguments(s, {{"save_as", true}, {"overwrite", true}}), ContainsSubstring("path"));
    CHECK_THAT(Mcp::validate_arguments(s, {{"silent", true}}), ContainsSubstring("path"));

    // The person's form with no arguments alone is written as a client's validator reads it most simply.
    CHECK(Mcp::with_silent(schema)["if"] == json{{"minProperties", 1}});
    CHECK(Mcp::with_silent(schema, {json::object()})["if"] == json{{"minProperties", 1}});
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

TEST_CASE("a reference entry has the command's fields, and its examples and actions only when it has some", "[CommandCatalog][Reference]")
{
    NativeCommand c = command("export_it", true);
    c.title         = "Export It";
    c.group         = "The project";
    c.description   = "Exports it.";
    c.input_schema  = {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}};
    c.guard         = GuardClass::Recover;
    c.completion    = CommandCompletion::Deferred;
    c.operations    = {"start_gcode_export"};
    c.open_world    = true;
    const json entry = Slic3r::GUI::command_reference(c);
    CHECK(entry == json{{"key", "export_it"},
                        {"title", "Export It"},
                        {"group", "The project"},
                        {"description", "Exports it."},
                        {"input_schema", c.input_schema},
                        {"listed", false},
                        {"has_own_action", true},
                        {"read_only", false},
                        {"destructive", false},
                        {"idempotent", false},
                        {"open_world", true},
                        {"guard", "recover"},
                        {"completion", "deferred"},
                        {"operations", json::array({"start_gcode_export"})}});

    const json call{{"tool", "export_it"}, {"args", {{"path", "a"}}}};
    c.guard          = GuardClass::Inspect;
    c.completion     = CommandCompletion::Immediate;
    c.examples       = json::array({json{{"title", "Export"}, {"calls", json::array({call})}}});
    const json other = Slic3r::GUI::command_reference(c);
    CHECK(other["guard"] == "inspect");
    CHECK(other["completion"] == "immediate");
    CHECK(other["examples"] == c.examples);
    CHECK_FALSE(other.contains("actions"));
    c.guard = GuardClass::Mutate;
    CHECK(Slic3r::GUI::command_reference(c)["guard"] == "mutate");
    c.actions = json::array({json{{"key", "export_it_pdf"}, {"title", "Export as PDF"}, {"icon", ""}, {"description", ""},
                                  {"args", {{"format", "pdf"}}}}});
    CHECK(Slic3r::GUI::command_reference(c)["actions"] == c.actions);

    // What a person sees of its own action, once it has any.
    CHECK_FALSE(entry.contains("icon"));
    CHECK_FALSE(entry.contains("input"));
    CHECK_FALSE(entry.contains("action_group"));
    c.icon         = "menu_save";
    c.input        = "tab";
    c.action_group = "Commands";
    const json shown = Slic3r::GUI::command_reference(c);
    CHECK(shown["icon"] == "menu_save");
    CHECK(shown["input"] == "tab");
    CHECK(shown["action_group"] == "Commands");
}

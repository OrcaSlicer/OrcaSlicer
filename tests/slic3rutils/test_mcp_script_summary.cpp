#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/Mcp/McpScript.hpp"
#include "slic3r/GUI/Mcp/McpScriptSummary.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;

namespace {

using Flags = std::vector<std::string>;

const Flags kEveryFlag{"writes_file", "changes_presets", "replaces_project", "sends_to_printer"};

// Describes the one step of a script built around `step`, run from C:/scripts with output in C:/out.
StepDescription describe(const std::string& step, const std::string& vars = "{}")
{
    std::string err;
    const auto  script = parse_script(R"({"orca_script": 1, "vars": )" + vars + R"(, "steps": [)" + step + "]}", err);
    INFO(err);
    REQUIRE(script.has_value());
    ScriptContext ctx;
    ctx.script_dir = "C:/scripts";
    ctx.out_dir    = "C:/out";
    return describe_step(script->steps[0], *script, ctx);
}

Flags flags_of(const StepDescription& d)
{
    Flags out;
    for (StepFlag f : d.flags)
        out.push_back(flag_id(f));
    return out;
}

// An absolute path on this platform: Speed Dial params name files only by absolute paths, and
// "C:/a" is relative outside Windows.
std::string abs_path(const std::string& name)
{
#ifdef _WIN32
    return "C:/a/" + name;
#else
    return "/a/" + name;
#endif
}

std::string action(const std::string& id, const std::string& param = {})
{
    json args{{"id", id}};
    if (!param.empty())
        args["param"] = param;
    return json{{"tool", "run_action"}, {"args", args}}.dump();
}

} // namespace

TEST_CASE("open_files is flagged by what it opens", "[Mcp]")
{
    struct Case
    {
        std::string step;
        std::string vars;
        Flags       flags;
        std::string replace;
    };
    const Case c = GENERATE(values<Case>({
        {R"({"tool": "open_files", "args": {"paths": ["C:/a/p.3mf"]}})", "{}", {"replaces_project"}, "checked"},
        {R"({"tool": "open_files", "args": {"paths": ["C:/a/p.3mf"], "as": "geometry"}})", "{}", {}, ""},
        {R"({"tool": "open_files", "args": {"paths": ["C:/a/p.3mf", "C:/a/b.stl"]}})", "{}", {}, ""},
        {R"({"tool": "open_files", "args": {"paths": ["C:/a/b.stl"]}})", "{}", {}, ""},
        {R"({"tool": "open_files", "args": {"paths": ["C:/a/m.step"]}})", "{}", {}, ""},
        {R"({"tool": "open_files", "args": {"paths": ["C:/a/x.gcode"]}})", "{}", {"replaces_project"}, "app_asked"},
        {R"({"tool": "open_files", "args": {"paths": ["${vars.project}"]}})", R"({"project": "C:/a/p.3mf"})",
         {"replaces_project"}, "checked"},
    }));
    INFO(c.step);
    const StepDescription d = describe(c.step, c.vars);
    CHECK(flags_of(d) == c.flags);
    CHECK(d.replace == c.replace);
}

TEST_CASE("project tools replace the project after checking for unsaved changes", "[Mcp]")
{
    const StepDescription fresh = describe(R"({"tool": "new_project", "args": {"discard_changes": true}})");
    CHECK(flags_of(fresh) == Flags{"replaces_project"});
    CHECK(fresh.replace == "checked");

    const StepDescription calib = describe(R"({"tool": "calibrate", "args": {"test": "temperature"}})");
    CHECK(flags_of(calib) == Flags{"changes_presets", "replaces_project"});
    CHECK(calib.replace == "checked");
}

TEST_CASE("speed dial actions are flagged by their id", "[Mcp]")
{
    struct Case
    {
        std::string id;
        std::string param;
        Flags       flags;
        std::string replace;
    };
    const Case c = GENERATE(values<Case>({
        {"orca_command:calib_flow_ratio:orca", "", {"changes_presets", "replaces_project"}, "app_asked"},
        {"orca_command:send_gcode:orca", "", {"sends_to_printer"}, ""},
        {"orca_command:open_preferences:orca", "", {}, ""},
        {"orca_command:slice_all:orca", "", {}, ""},
        {"orca_command:export_gcode:orca", "C:/x/a.gcode", {"writes_file"}, ""},
        {"orca_command:export_gcode:orca", "", {"writes_file"}, ""},
        {"orca_command:export_config:orca", "C:/x", {"writes_file"}, ""},
        {"orca_command:obj_set_instances:orca", "3", {}, ""},
        {"orca_command:save_project_as:orca", "C:/x/a.3mf", {"writes_file"}, ""},
        {"orca_command:new_project:orca", "", {"replaces_project"}, "app_asked"},
        {"orca_command:import_file:orca", abs_path("p.3mf"), {"replaces_project"}, "app_asked"},
        {"orca_command:import_file:orca", json{{"path", abs_path("p.3mf")}, {"overwrite", true}}.dump(), {"replaces_project"}, "app_asked"},
        {"orca_command:import_file:orca", json{{"path", abs_path("x.gcode")}}.dump(), {"replaces_project"}, "app_asked"},
        {"orca_command:import_file:orca", abs_path("b.stl"), {}, ""},
        {"orca_recent_project:C:/p/a.3mf:orca", "", {"replaces_project"}, "app_asked"},
        {"orca_plate_goto:1:orca", "", {}, ""},
        {"orca_setting:layer_height:1", "", {}, ""},
        {"plugin_script_action:tool:plugin", "", kEveryFlag, "app_asked"},
        {"plugin_page_action:page:plugin", "", kEveryFlag, "app_asked"},
    }));
    INFO(c.id << " " << c.param);
    const StepDescription d = describe(action(c.id, c.param));
    CHECK(flags_of(d) == c.flags);
    CHECK(d.replace == c.replace);
}

TEST_CASE("plugin actions are taken to answer dialogs", "[Mcp]")
{
    CHECK(describe(action("plugin_script_action:tool:plugin")).answers_dialogs);
    CHECK_FALSE(describe(action("orca_command:new_project:orca")).answers_dialogs);
}

TEST_CASE("speed dial actions describe their command and file", "[Mcp]")
{
    const auto [id, param, what] = GENERATE(table<std::string, std::string, json>({
        {"orca_command:import_file:orca", abs_path("p.3mf"), {{"kind", "command"}, {"key", "import_file"}, {"param", "p.3mf"}}},
        {"orca_command:import_file:orca", json{{"path", abs_path("p.3mf")}, {"overwrite", true}}.dump(),
         {{"kind", "command"}, {"key", "import_file"}, {"param", "p.3mf"}}},
        {"orca_command:import_file:orca", json{{"path", abs_path("x.gcode")}}.dump(), {{"kind", "command"}, {"key", "import_file"}, {"param", "x.gcode"}}},
        {"orca_command:go_to_layer:orca", "50", {{"kind", "command"}, {"key", "go_to_layer"}, {"param", "50"}}},
        {"orca_command:slice_all:orca", "", {{"kind", "command"}, {"key", "slice_all"}}},
        {"orca_recent_project:C:/p/a.3mf:orca", "", {{"kind", "recent_project"}, {"name", "a.3mf"}}},
        {"orca_setting:layer_height:1", "", {{"kind", "value"}, {"text", "layer_height"}}},
        {"plugin_script_action:tool:plugin", "", {{"kind", "value"}, {"text", "plugin_script_action:tool:plugin"}}},
    }));
    INFO(id << " " << param);
    CHECK(describe(action(id, param)).what == what);
}

TEST_CASE("files written inside the output folder are not flagged", "[Mcp]")
{
    const auto [step, flags] = GENERATE(table<std::string, Flags>({
        {R"({"tool": "screenshot", "args": {"save_path": "C:/out/a.png"}})", {}},
        {R"({"tool": "screenshot", "args": {"save_path": "${out_dir}/a.png"}})", {}},
        {R"({"tool": "screenshot", "args": {"save_path": "C:/x/a.png"}})", {"writes_file"}},
        {R"({"tool": "screenshot", "args": {"target": "canvas"}})", {}},
        {R"({"tool": "export_script", "args": {"path": "${out_dir}/s.json"}})", {}},
        {R"({"tool": "export_script", "args": {"path": "C:/x/s.json"}})", {"writes_file"}},
        {R"({"tool": "gizmo_file", "args": {"action": "svg_save_as", "path": "C:/x/a.svg"}})", {"writes_file"}},
        {R"({"tool": "gizmo_file", "args": {"action": "svg_change", "path": "C:/x/a.svg"}})", {}},
        {R"({"tool": "export", "args": {"path": "${out_dir}/a.gcode"}})", {"writes_file"}},
        {R"({"tool": "save_preset", "args": {"type": "filament", "name": "Mine"}})", {"changes_presets"}},
        {R"({"tool": "delete_preset", "args": {"type": "filament", "name": "Mine"}})", {"changes_presets"}},
    }));
    INFO(step);
    CHECK(flags_of(describe(step)) == flags);
}

TEST_CASE("steps that can answer a dialog are told apart", "[Mcp]")
{
    const auto [tool, answers] = GENERATE(table<std::string, bool>({
        {"dialog_auto_answer", true},
        {"dialog_respond", true},
        {"ui_click", true},
        {"ui_key", true},
        {"ui_input", false},
        {"ui_mouse", false},
        {"webview_eval", false},
    }));
    INFO(tool);
    const StepDescription d = describe(json{{"tool", tool}}.dump());
    CHECK(d.answers_dialogs == answers);
    CHECK(flags_of(d).empty());
}

TEST_CASE("nested scripts join their steps' descriptions", "[Mcp]")
{
    const StepDescription replacing = describe(
        R"({"tool": "run_script", "args": {"script": {"orca_script": 1, "steps": [{"tool": "new_project", "args": {"discard_changes": true}}]}}})");
    CHECK(flags_of(replacing) == Flags{"replaces_project"});
    CHECK(replacing.replace == "checked");
    CHECK(replacing.what == json{{"kind", "script"}, {"steps", 1}});

    const StepDescription own_output = describe(
        R"({"tool": "run_script", "args": {"script": {"orca_script": 1, "steps": [{"tool": "screenshot", "args": {"save_path": "${out_dir}/n.png"}}]}}})");
    CHECK(flags_of(own_output).empty());

    // The outer script substitutes an inline script's placeholders before the nested run starts, as
    // run_script does: the step's own vars come too late, and its out_dir doesn't move ${out_dir}.
    const StepDescription outer_vars = describe(
        R"({"tool": "run_script", "args": {"script": {"orca_script": 1, "steps": [{"tool": "open_files", "args": {"paths": ["${vars.p}"]}}]}}})",
        R"({"p": "C:/a/p.3mf"})");
    CHECK(flags_of(outer_vars) == Flags{"replaces_project"});
    CHECK(outer_vars.replace == "checked");

    const StepDescription step_vars = describe(
        R"({"tool": "run_script", "args": {"vars": {"p": "C:/a/p.3mf"}, "script": {"orca_script": 1, "vars": {"p": "C:/a/q.3mf"}, "steps": [{"tool": "open_files", "args": {"paths": ["${vars.p}"]}}]}}})");
    CHECK(flags_of(step_vars).empty());
    CHECK(step_vars.replace == "");

    const StepDescription own_folder = describe(
        R"({"tool": "run_script", "args": {"out_dir": "C:/elsewhere", "script": {"orca_script": 1, "steps": [{"tool": "screenshot", "args": {"save_path": "${out_dir}/n.png"}}]}}})");
    CHECK(flags_of(own_folder).empty());

    const StepDescription mixed = describe(
        R"({"tool": "run_script", "args": {"script": {"orca_script": 1, "steps": [{"tool": "run_action", "args": {"id": "orca_command:new_project:orca"}}, {"tool": "new_project"}]}}})");
    CHECK(mixed.replace == "checked");

    const StepDescription clicking = describe(
        R"({"tool": "run_script", "args": {"script": {"orca_script": 1, "steps": [{"tool": "ui_click", "args": {"target": {"label": "No"}}}]}}})");
    CHECK(clicking.answers_dialogs);

    // Five levels of nesting: past the inspection depth, so the effect can't be read.
    std::string deep = R"({"orca_script": 1, "steps": [{"tool": "new_project"}]})";
    for (int i = 0; i < 5; ++i)
        deep = R"({"orca_script": 1, "steps": [{"tool": "run_script", "args": {"script": )" + deep + "}}]}";
    const StepDescription too_deep = describe(R"({"tool": "run_script", "args": {"script": )" + deep + "}}");
    CHECK(flags_of(too_deep) == kEveryFlag);
    CHECK(too_deep.replace == "checked");
    CHECK(too_deep.answers_dialogs);
    CHECK(too_deep.what == json{{"kind", "script"}, {"steps", 1}});

    const StepDescription invalid = describe(R"({"tool": "run_script", "args": {"script": {"orca_script": 1, "steps": [{"tool": 3}]}}})");
    CHECK(flags_of(invalid) == kEveryFlag);
    REQUIRE(invalid.what.value("kind", "") == "value");
    CHECK(invalid.what.value("text", "").find("step 1") != std::string::npos);

    const StepDescription missing = describe(R"({"tool": "run_script", "args": {"path": "C:/no/such/script.json"}})");
    CHECK(flags_of(missing) == kEveryFlag);
    CHECK(missing.replace == "checked");
    CHECK(missing.answers_dialogs);
    CHECK(missing.what == json{{"kind", "files"}, {"names", {"script.json"}}});
}

// The runner describes steps without the saved jobs a single step runs with: ${jobs.NAME} resolves
// to a job id such as "j12", never a path, so the effects read the same either way.
TEST_CASE("saved jobs don't change a step's flags", "[Mcp]")
{
    const std::string step = GENERATE(as<std::string>{}, R"({"tool": "open_files", "args": {"paths": ["${jobs.p}"]}})",
                                      R"({"tool": "screenshot", "args": {"save_path": "${jobs.p}/a.png"}})",
                                      R"({"tool": "run_action", "args": {"id": "orca_command:import_file:orca", "param": "${jobs.p}"}})");
    INFO(step);
    std::string err;
    const auto  script = parse_script(R"({"orca_script": 1, "steps": [)" + step + "]}", err);
    REQUIRE(script.has_value());
    ScriptContext without;
    without.out_dir = "C:/out";
    ScriptContext with = without;
    with.jobs          = json{{"p", "j12"}};
    const StepDescription a = describe_step(script->steps[0], *script, without);
    const StepDescription b = describe_step(script->steps[0], *script, with);
    CHECK(flags_of(a) == flags_of(b));
    CHECK(a.replace == b.replace);
    CHECK(a.answers_dialogs == b.answers_dialogs);
}

TEST_CASE("a step's what names what it acts on", "[Mcp]")
{
    const auto [step, what] = GENERATE(table<std::string, json>({
        {R"({"tool": "set_settings", "args": {"values": {"layer_height": 0.12, "wall_loops": "3"}}})",
         {{"kind", "settings"}, {"pairs", {"layer_height = 0.12", "wall_loops = 3"}}}},
        {R"({"tool": "open_files", "args": {"paths": ["C:/m/3DBenchy.stl", "C:/m/cube.stl"]}})",
         {{"kind", "files"}, {"names", {"3DBenchy.stl", "cube.stl"}}}},
        {R"({"tool": "screenshot", "args": {"save_path": "${out_dir}/preview.png"}})", {{"kind", "files"}, {"names", {"preview.png"}}}},
        {R"({"tool": "slice"})", {{"kind", "plates"}, {"plates", "current"}}},
        {R"({"tool": "slice", "args": {"plates": 2}})", {{"kind", "plates"}, {"plates", 2}}},
        {R"({"tool": "arrange"})", {{"kind", "plates"}, {"plates", "all"}}},
        {R"({"tool": "select_preset", "args": {"type": "filament", "name": "Generic PLA"}})", {{"kind", "preset"}, {"name", "Generic PLA"}}},
        {R"({"tool": "wait", "args": {"for": "idle"}})", {{"kind", "value"}, {"text", "idle"}}},
        {R"({"tool": "dialog_respond", "args": {"button": "no"}})", {{"kind", "value"}, {"text", "no"}}},
        {R"({"tool": "transform", "args": {"object": 1}})", {{"kind", "value"}, {"text", "object: 1"}}},
        {R"({"tool": "get_state"})", nullptr},
    }));
    INFO(step);
    CHECK(describe(step).what == what);
}

TEST_CASE("a step with an expect checks its result", "[Mcp]")
{
    CHECK(describe(R"({"tool": "slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}})").checks_result);
    CHECK_FALSE(describe(R"({"tool": "slice"})").checks_result);
}

TEST_CASE("results are described from the tools' real replies", "[Mcp]")
{
    CHECK(describe_result("open_files", {{"objects", 1}, {"names", {"Benchy"}}, {"plates", 1}}) == json{{"kind", "objects"}, {"count", 1}});
    CHECK(describe_result("calibrate", {{"objects", 3}, {"plates", 1}}) == json{{"kind", "objects"}, {"count", 3}});
    CHECK(describe_result("open_files", {{"opened", "gcode"}, {"path", "C:/a/x.gcode"}}) == json{{"kind", "file"}, {"name", "x.gcode"}});
    CHECK(describe_result("slice", {{"plates", {{{"index", 0}, {"outcome", "ok"}}, {{"index", 1}, {"outcome", "error"}}}}}) ==
          json{{"kind", "plates"}, {"ok", 1}, {"other", 1}});
    CHECK(describe_result("screenshot", {{"path", "C:/out/a.png"}}) == json{{"kind", "file"}, {"name", "a.png"}});
    CHECK(describe_result("gizmo_file", {{"saved", "C:/x/a.svg"}}) == json{{"kind", "file"}, {"name", "a.svg"}});
    CHECK(describe_result("run_script", {{"passed", false}, {"failed_step", 2}, {"steps", json::array()}}) ==
          json{{"kind", "script"}, {"passed", false}, {"failed_step", 2}});
    CHECK(describe_result("run_script", {{"passed", true}, {"steps", json::array()}}) == json{{"kind", "script"}, {"passed", true}});
    CHECK(describe_result("get_state", {{"ready", true}}).is_null());

    CHECK(result_files({{"saved", "C:/x/a.svg"}}) == std::vector<std::string>{"C:/x/a.svg"});
    CHECK(result_files({{"path", "C:/out/a.png"}}) == std::vector<std::string>{"C:/out/a.png"});
    CHECK(result_files({{"path", ""}}).empty());
    CHECK(result_files(json::object()).empty());
}

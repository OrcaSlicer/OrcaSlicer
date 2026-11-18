#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/Mcp/McpReplay.hpp"
#include "slic3r/GUI/Mcp/McpReplaySummary.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
using Slic3r::GUI::CommandCatalog;
using Slic3r::GUI::NativeCommand;

namespace {

using Flags = std::vector<std::string>;

const Flags kEveryFlag{"writes_file", "changes_presets", "replaces_project", "sends_to_printer"};

// The commands the cases call by their key, with tiles and places in the tool list as the app's catalog has them:
// commands whose menu rows are items of their tiles, view_set's two among them collecting a value, and object_mirror's
// two, one of which has a Speed Dial section of its own; plate_delete, with a tile of its own; all of them listed, as
// they run from their arguments. And export_drawing, made up, with a tile of its own and one that runs it with
// {"format": "pdf"}, which the tool list does not show, as a Design command. Every other tool the cases name is a
// typed tool, which the summary describes by its name.
CommandCatalog tile_catalog()
{
    CommandCatalog catalog;
    const auto     tile = [](const char* key, const char* title, json args) {
        return json{{"key", key}, {"title", title}, {"icon", ""}, {"description", ""}, {"args", std::move(args)}};
    };
    // A tile whose second phase collects a value under `input`, which goes to the command's `input_arg`.
    const auto collects = [&tile](const char* key, const char* title, const char* input, const char* input_arg) {
        json item         = tile(key, title, json::object());
        item["input"]     = input;
        item["input_arg"] = input_arg;
        return item;
    };
    const auto add = [&catalog](const char* key, bool listed, bool in_palette, json tiles) {
        NativeCommand command;
        command.key        = key;
        command.listed     = listed;
        command.in_palette = in_palette;
        command.tiles      = std::move(tiles);
        catalog.add(std::move(command));
    };
    add("project_new", true, false, json::array({tile("new_project", "New Project", json::object())}));
    add("project_open", true, false, json::array({tile("load_project", "Load Project", json::object())}));
    add("project_import", true, false, json::array({tile("import_file", "Import 3MF/STL/STEP/SVG/OBJ/AMF", json::object())}));
    add("project_save", true, false,
        json::array({tile("save_project", "Save Project", json::object()), tile("save_project_as", "Save Project As", {{"save_as", true}})}));
    add("project_export", true, false, json::array({tile("export_gcode", "Export G-code", {{"format", "gcode"}})}));
    add("calibration_generate_test", true, false,
        json::array({tile("calib_flow_ratio", "Flow Ratio Calibration", {{"test", "flow_ratio"}})}));
    add("plate_slice", true, false, json::array({tile("slice_all", "Slice All Plates", {{"plates", "all"}, {"preview", true}})}));
    add("view_set", true, false,
        json::array({tile("view_top", "View: Top", {{"camera", "top"}}), collects("go_to_tab", "Go to tab...", "tab", "tab"),
                     collects("go_to_layer", "Go to layer (percent)", "percent", "layer_percent")}));
    add("object_add_primitive", true, false, json::array({tile("add_primitive_cube", "Cube", {{"shape", "cube"}})}));
    add("object_set_instances", true, false, json::array({tile("obj_set_instances", "Set Number of Instances", json::object())}));
    json mirror_x     = tile("obj_mirror_x", "Mirror X", {{"axis", "x"}});
    mirror_x["group"] = "Object";
    add("object_mirror", true, false, json::array({std::move(mirror_x), tile("obj_mirror_y", "Mirror Y", {{"axis", "y"}})}));
    add("preset_export", true, false, json::array({tile("export_config", "Export Preset Bundle", json::object())}));
    add("preset_sync", true, false, json::array({tile("sync_presets", "Sync Presets", json::object())}));
    add("printer_send", true, false, json::array({tile("send_gcode", "Send G-code to printer host", json::object())}));
    add("window_open", true, false,
        json::array({tile("open_preferences", "Preferences", {{"window", "preferences"}}),
                     tile("preset_bundle", "Open Preset Bundle", {{"window", "preset_bundle"}})}));
    add("plate_delete", true, true, json());
    add("export_drawing", false, true, json::array({tile("export_drawing_pdf", "Export as PDF", {{"format", "pdf"}})}));
    catalog.seal();
    return catalog;
}

// Describes the one step of a replay built around `step`, run from C:/replays with output in C:/out.
StepDescription describe(const std::string& step, const std::string& vars = "{}")
{
    std::string err;
    const auto  replay = parse_replay(R"({"orca_replay_version": "0.2.0", "vars": )" + vars + R"(, "steps": [)" + step + "]}", err);
    INFO(err);
    REQUIRE(replay.has_value());
    ReplayContext ctx;
    ctx.replay_dir = "C:/replays";
    ctx.out_dir    = "C:/out";
    return describe_step(tile_catalog(), replay->steps[0], *replay, ctx);
}

Flags flags_of(const StepDescription& d)
{
    Flags out;
    for (StepFlag f : d.flags)
        out.push_back(flag_id(f));
    return out;
}

// An absolute path on this platform: Speed Dial commands name files only by absolute paths, and
// "C:/a" is relative outside Windows.
std::string abs_path(const std::string& name)
{
#ifdef _WIN32
    return "C:/a/" + name;
#else
    return "/a/" + name;
#endif
}

std::string action(const std::string& id, const json& command_args = json::object())
{
    json args{{"id", id}};
    if (!command_args.empty())
        args["args"] = command_args;
    return json{{"tool", "action_run"}, {"args", args}}.dump();
}

} // namespace

TEST_CASE("project_open replaces the project and project_import adds to it", "[Mcp]")
{
    struct Case
    {
        std::string step;
        std::string vars;
        Flags       flags;
        std::string replace;
    };
    const Case c = GENERATE(values<Case>({
        {R"({"tool": "project_open", "args": {"path": "C:/a/p.3mf"}})", "{}", {"replaces_project"}, "checked"},
        {R"({"tool": "project_open", "args": {"path": "C:/a/x.gcode"}})", "{}", {"replaces_project"}, "app_asked"},
        {R"({"tool": "project_open", "args": {"path": "C:/a/b.stl"}})", "{}", {}, ""},
        {R"({"tool": "project_open", "args": {"path": "${vars.project}"}})", R"({"project": "C:/a/p.3mf"})",
         {"replaces_project"}, "checked"},
        {R"({"tool": "project_import", "args": {"paths": ["C:/a/p.3mf"]}})", "{}", {}, ""},
        {R"({"tool": "project_import", "args": {"paths": ["C:/a/p.3mf", "C:/a/b.stl"]}})", "{}", {}, ""},
        {R"({"tool": "project_import", "args": {"paths": ["C:/a/m.step"]}})", "{}", {}, ""},
    }));
    INFO(c.step);
    const StepDescription d = describe(c.step, c.vars);
    CHECK(flags_of(d) == c.flags);
    CHECK(d.replace == c.replace);
}

TEST_CASE("project tools replace the project after checking for unsaved changes", "[Mcp]")
{
    const StepDescription fresh = describe(R"({"tool": "project_new", "args": {"discard_changes": true}})");
    CHECK(flags_of(fresh) == Flags{"replaces_project"});
    CHECK(fresh.replace == "checked");

    const StepDescription calib = describe(R"({"tool": "calibration_generate_test", "args": {"test": "temperature", "start": 230}})");
    CHECK(flags_of(calib) == Flags{"changes_presets", "replaces_project"});
    CHECK(calib.replace == "checked");

    // Silent with its defaults, it checks as with arguments; with none, it asks as New Project does.
    CHECK(describe(R"({"tool": "project_new", "args": {"silent": true}})").replace == "checked");
    const StepDescription asked = describe(R"({"tool": "project_new", "args": {}})");
    CHECK(flags_of(asked) == Flags{"replaces_project"});
    CHECK(asked.replace == "app_asked");
    // A test called with its tile's arguments opens its dialog, as the Calibration menu does.
    const StepDescription dialog = describe(R"({"tool": "calibration_generate_test", "args": {"test": "flow_ratio"}})");
    CHECK(flags_of(dialog) == Flags{"changes_presets", "replaces_project"});
    CHECK(dialog.replace == "app_asked");
}

TEST_CASE("a command called by name is described as its tile is", "[Mcp]")
{
    const auto [key, tile_key, args, flags] = GENERATE_COPY(table<std::string, std::string, json, Flags>({
        {"preset_export", "export_config", json::object(), {"writes_file"}},
        {"preset_sync", "sync_presets", json::object(), {"changes_presets"}},
        {"window_open", "preset_bundle", {{"window", "preset_bundle"}}, {"changes_presets"}},
        {"printer_send", "send_gcode", json::object(), {"sends_to_printer"}},
        {"plate_delete", "plate_delete", json::object(), {}},
    }));
    INFO(key << " " << args.dump());
    const StepDescription by_name = describe(json{{"tool", key}, {"args", args}}.dump());
    const StepDescription tile    = describe(action("orca_command:" + tile_key + ":orca"));
    CHECK(flags_of(by_name) == flags);
    CHECK(flags_of(by_name) == flags_of(tile));
    CHECK(by_name.replace == tile.replace);
    CHECK(by_name.what == tile.what);
    CHECK(describe_call(tile_catalog(), key, args) == tile.what);
}

TEST_CASE("a tile that runs its command with arguments is described as that call is", "[Mcp]")
{
    const auto [key, tile, args, flags] = GENERATE_COPY(table<std::string, std::string, json, Flags>({
        {"project_export", "export_gcode", {{"format", "gcode"}}, {"writes_file"}},
        {"calibration_generate_test", "calib_flow_ratio", {{"test", "flow_ratio"}}, {"changes_presets", "replaces_project"}},
        {"object_set_instances", "obj_set_instances", json::object(), {}},
        {"object_mirror", "obj_mirror_y", {{"axis", "y"}}, {}},
    }));
    INFO(key << " " << args.dump());
    const json        what{{"kind", "command"}, {"key", key}, {"tile", tile}};
    const std::string id = "orca_command:" + tile + ":orca";
    // The tile's action with no arguments and with its own, and the call of its command with them.
    const StepDescription picked = describe(action(id));
    const StepDescription given  = describe(action(id, args));
    const StepDescription called = describe(json{{"tool", key}, {"args", args}}.dump());
    for (const StepDescription* d : {&picked, &given, &called}) {
        CHECK(d->what == what);
        CHECK(flags_of(*d) == flags);
        CHECK(d->replace == picked.replace);
    }
    CHECK(describe_call(tile_catalog(), key, args) == what);
    CHECK(describe_call(tile_catalog(), "action_run", {{"id", id}}) == what);
    CHECK(describe_call(tile_catalog(), "action_run", {{"id", tile}}) == what);
}

TEST_CASE("a call with other arguments than a tile's is described by its arguments", "[Mcp]")
{
    const json what{{"kind", "command"}, {"key", "export_drawing"}, {"args", {"format: svg"}}};
    CHECK(describe(R"({"tool": "export_drawing", "args": {"format": "svg"}})").what == what);
    CHECK(describe(action("orca_command:export_drawing:orca", {{"format", "svg"}})).what == what);
    // The command's own tile with the other tile's arguments is that tile.
    CHECK(describe(action("orca_command:export_drawing:orca", {{"format", "pdf"}})).what ==
          json{{"kind", "command"}, {"key", "export_drawing"}, {"tile", "export_drawing_pdf"}});
    // A command made from rows called with arguments no tile has is described as a typed tool's call, with what it
    // does whichever way it is called.
    const StepDescription exported = describe(json{{"tool", "preset_export"}, {"args", {{"path", abs_path("presets")}}}}.dump());
    CHECK(exported.what == json{{"kind", "files"}, {"names", {"presets"}}});
    CHECK(flags_of(exported) == Flags{"writes_file"});
    CHECK(flags_of(describe(json{{"tool", "preset_import"}, {"args", {{"path", abs_path("a.json")}}}}.dump())) == Flags{"changes_presets"});
}

TEST_CASE("a tile that collects a value is described with it, and a call without it is no tile", "[Mcp]")
{
    // Go to layer's percent reaches view_set as layer_percent, which the description shows with the tile.
    CHECK(describe(action("orca_command:go_to_layer:orca", {{"percent", 50}})).what ==
          json{{"kind", "command"}, {"key", "view_set"}, {"tile", "go_to_layer"}, {"args", {"layer_percent: 50"}}});
    CHECK(describe(action("orca_command:go_to_tab:orca", {{"tab", "preview"}})).what ==
          json{{"kind", "command"}, {"key", "view_set"}, {"tile", "go_to_tab"}, {"args", {"tab: preview"}}});
    // view_set called with no arguments is no tile's call: the two tiles with nothing of their own collect a value.
    CHECK(describe_call(tile_catalog(), "view_set", json::object()).is_null());
    CHECK(describe_call(tile_catalog(), "view_set", {{"camera", "top"}}) ==
          json{{"kind", "command"}, {"key", "view_set"}, {"tile", "view_top"}});
}

TEST_CASE("a step that calls a tile's command by its key is described from the catalog it is given", "[Mcp]")
{
    std::string err;
    const auto  replay = parse_replay(R"({"orca_replay_version": "0.2.0", "steps": [
                                          {"tool": "filament_delete", "args": {}},
                                          {"tool": "filament_delete", "args": {"filament_slot": 2}}]})",
                                      err);
    INFO(err);
    REQUIRE(replay.has_value());
    ReplayContext ctx;
    ctx.out_dir = "C:/out";

    // filament_delete as the app's catalog has it: listed, with a tile of its own.
    CommandCatalog catalog;
    NativeCommand  filament_delete;
    filament_delete.key        = "filament_delete";
    filament_delete.in_palette = true;
    filament_delete.listed     = true;
    catalog.add(std::move(filament_delete));
    catalog.seal();
    const StepDescription tile = describe_step(catalog, replay->steps[0], *replay, ctx);
    REQUIRE(tile.what.is_object());
    CHECK(tile.what.value("kind", "") == "command");
    CHECK(tile.what.value("key", "") == "filament_delete");
    // With arguments of its own it is its silent form, described from them as a typed tool's call is.
    CHECK(describe_step(catalog, replay->steps[1], *replay, ctx).what == json{{"kind", "value"}, {"text", "filament_slot: 2"}});

    // Without the command in the catalog, the step is described from its arguments, as a typed tool's call is.
    const StepDescription unknown = describe_step(CommandCatalog(), replay->steps[0], *replay, ctx);
    const std::string     kind    = unknown.what.is_object() ? unknown.what.value("kind", "") : std::string();
    CHECK(kind != "command");
}

TEST_CASE("speed dial actions are flagged by their id", "[Mcp]")
{
    // A tile's id reaches its command through find_tile(), and the command and its arguments decide the flags: a tile's
    // call and a call of its command with those arguments, or with a path beside them, are flagged alike.
    struct Case
    {
        std::string step;
        Flags       flags;
        std::string replace;
    };
    const Case c = GENERATE_COPY(values<Case>({
        {action("orca_command:calib_flow_ratio:orca"), {"changes_presets", "replaces_project"}, "app_asked"},
        {action("orca_command:send_gcode:orca"), {"sends_to_printer"}, ""},
        {action("orca_command:open_preferences:orca"), {}, ""},
        {action("orca_command:slice_all:orca"), {}, ""},
        {action("orca_command:export_gcode:orca"), {"writes_file"}, ""},
        {action("orca_command:export_config:orca"), {"writes_file"}, ""},
        {action("orca_command:sync_presets:orca"), {"changes_presets"}, ""},
        {action("orca_command:preset_bundle:orca"), {"changes_presets"}, ""},
        {action("orca_command:obj_set_instances:orca"), {}, ""},
        {action("orca_command:save_project_as:orca"), {"writes_file"}, ""},
        {action("orca_command:new_project:orca"), {"replaces_project"}, "app_asked"},
        {action("orca_command:load_project:orca"), {"replaces_project"}, "app_asked"},
        {action("orca_command:import_file:orca"), {}, ""},
        // A tile by the key inside its id, as action_run takes it.
        {action("export_gcode"), {"writes_file"}, ""},
        {action("new_project"), {"replaces_project"}, "app_asked"},
        {action("send_gcode"), {"sends_to_printer"}, ""},
        {action("plate_delete"), {}, ""},
        {action("go_to_layer", {{"percent", 50}}), {}, ""},
        {json{{"tool", "project_export"}, {"args", {{"format", "gcode"}}}}.dump(), {"writes_file"}, ""},
        {json{{"tool", "project_export"}, {"args", {{"format", "gcode"}, {"path", abs_path("a.gcode")}}}}.dump(), {"writes_file"}, ""},
        {action("orca_recent_project:C:/p/a.3mf:orca"), {"replaces_project"}, "app_asked"},
        {action("orca_plate_goto:1:orca"), {}, ""},
        {action("orca_setting:layer_height:1"), {}, ""},
        {action("plugin_replay_action:tool:plugin"), kEveryFlag, "app_asked"},
        {action("plugin_page_action:page:plugin"), kEveryFlag, "app_asked"},
        // A name inside an id that is no tile may be another action's, whose effect can't be read.
        {action("page"), kEveryFlag, "app_asked"},
    }));
    INFO(c.step);
    const StepDescription d = describe(c.step);
    CHECK(flags_of(d) == c.flags);
    CHECK(d.replace == c.replace);
}

TEST_CASE("plugin actions are taken to answer dialogs", "[Mcp]")
{
    CHECK(describe(action("plugin_replay_action:tool:plugin")).answers_dialogs);
    CHECK_FALSE(describe(action("orca_command:new_project:orca")).answers_dialogs);
}

TEST_CASE("speed dial actions describe their command and file", "[Mcp]")
{
    const json none = json::object();
    const auto [id, args, what] = GENERATE_COPY(table<std::string, json, json>({
        {"orca_command:import_file:orca", none, {{"kind", "command"}, {"key", "project_import"}, {"tile", "import_file"}}},
        {"orca_command:save_project_as:orca", none, {{"kind", "command"}, {"key", "project_save"}, {"tile", "save_project_as"}}},
        {"orca_command:export_config:orca", none, {{"kind", "command"}, {"key", "preset_export"}, {"tile", "export_config"}}},
        {"orca_command:go_to_layer:orca", {{"percent", 50}},
         {{"kind", "command"}, {"key", "view_set"}, {"tile", "go_to_layer"}, {"args", {"layer_percent: 50"}}}},
        {"orca_command:add_primitive_cube:orca", none, {{"kind", "command"}, {"key", "object_add_primitive"}, {"tile", "add_primitive_cube"}}},
        {"orca_command:slice_all:orca", none, {{"kind", "command"}, {"key", "plate_slice"}, {"tile", "slice_all"}}},
        // A tile with a Speed Dial section of its own is described as any other.
        {"orca_command:obj_mirror_x:orca", none, {{"kind", "command"}, {"key", "object_mirror"}, {"tile", "obj_mirror_x"}}},
        {"orca_command:obj_mirror_y:orca", none, {{"kind", "command"}, {"key", "object_mirror"}, {"tile", "obj_mirror_y"}}},
        // A tile by the key inside its id, as action_run takes it, a command's own tile among them.
        {"export_gcode", none, {{"kind", "command"}, {"key", "project_export"}, {"tile", "export_gcode"}}},
        {"go_to_layer", {{"percent", 50}},
         {{"kind", "command"}, {"key", "view_set"}, {"tile", "go_to_layer"}, {"args", {"layer_percent: 50"}}}},
        {"plate_delete", none, {{"kind", "command"}, {"key", "plate_delete"}}},
        {"orca_recent_project:C:/p/a.3mf:orca", none, {{"kind", "recent_project"}, {"name", "a.3mf"}}},
        {"orca_setting:layer_height:1", none, {{"kind", "value"}, {"text", "layer_height"}}},
        {"plugin_replay_action:tool:plugin", none, {{"kind", "value"}, {"text", "plugin_replay_action:tool:plugin"}}},
    }));
    INFO(id << " " << args.dump());
    CHECK(describe(action(id, args)).what == what);
}

TEST_CASE("files written inside the output folder are not flagged", "[Mcp]")
{
    const auto [step, flags] = GENERATE(table<std::string, Flags>({
        {R"({"tool": "window_screenshot", "args": {"save_path": "C:/out/a.png"}})", {}},
        {R"({"tool": "window_screenshot", "args": {"save_path": "${out_dir}/a.png"}})", {}},
        {R"({"tool": "window_screenshot", "args": {"save_path": "C:/x/a.png"}})", {"writes_file"}},
        {R"({"tool": "window_screenshot", "args": {"target": "canvas"}})", {}},
        {R"({"tool": "replay_export", "args": {"path": "${out_dir}/s.json"}})", {}},
        {R"({"tool": "replay_export", "args": {"path": "C:/x/s.json"}})", {"writes_file"}},
        {R"({"tool": "gizmo_pick_file", "args": {"action": "svg_save_as", "path": "C:/x/a.svg"}})", {"writes_file"}},
        {R"({"tool": "gizmo_pick_file", "args": {"action": "svg_change", "path": "C:/x/a.svg"}})", {}},
        // A path rule may answer a save picker, so its path is a file the step may write.
        {R"({"tool": "dialog_auto_answer", "args": {"rules": [{"match": "Save", "button": "ok", "path": "C:/x/a.3mf"}]}})", {"writes_file"}},
        {R"({"tool": "dialog_auto_answer", "args": {"rules": [{"match": "Save", "button": "ok", "path": "${out_dir}/a.3mf"}]}})", {}},
        {R"({"tool": "dialog_auto_answer", "args": {"rules": [{"match": "Save", "button": "cancel"}]}})", {}},
        {R"({"tool": "dialog_auto_answer", "args": {"rules": [{"match": "Discard", "button": "no"}, {"match": "Save", "button": "ok", "path": "C:/x/a.3mf", "overwrite": true}]}})",
         {"writes_file"}},
        // An encode deletes its source unless it keeps the raw video file.
        {R"({"tool": "recording_encode", "args": {"path": "C:/x/a.mkv", "save_path": "${out_dir}/a.mp4"}})", {"writes_file"}},
        {R"({"tool": "recording_encode", "args": {"path": "C:/x/a.mkv", "save_path": "${out_dir}/a.mp4", "keep_raw_video_file": true}})", {}},
        {R"({"tool": "recording_encode", "args": {"path": "${out_dir}/a.mkv"}})", {}},
        {R"({"tool": "project_export", "args": {"path": "${out_dir}/a.gcode"}})", {"writes_file"}},
        {R"({"tool": "preset_save", "args": {"type": "filament", "name": "Mine"}})", {"changes_presets"}},
        {R"({"tool": "preset_delete", "args": {"type": "filament", "name": "Mine"}})", {"changes_presets"}},
    }));
    INFO(step);
    CHECK(flags_of(describe(step)) == flags);
}

TEST_CASE("steps that can answer a dialog are told apart", "[Mcp]")
{
    const auto [tool, answers] = GENERATE(table<std::string, bool>({
        {"dialog_auto_answer", true},
        {"dialog_answer", true},
        {"widget_click", true},
        {"keyboard_press", true},
        {"widget_set_value", false},
        {"mouse_gesture", false},
        {"webview_run_javascript", false},
    }));
    INFO(tool);
    const StepDescription d = describe(json{{"tool", tool}}.dump());
    CHECK(d.answers_dialogs == answers);
    CHECK(flags_of(d).empty());
}

TEST_CASE("nested replays join their steps' descriptions", "[Mcp]")
{
    const StepDescription replacing = describe(
        R"({"tool": "replay_run", "args": {"replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": "project_new", "args": {"discard_changes": true}}]}}})");
    CHECK(flags_of(replacing) == Flags{"replaces_project"});
    CHECK(replacing.replace == "checked");
    CHECK(replacing.what == json{{"kind", "replay"}, {"steps", 1}});

    // An inline replay given as JSON text, comments included, reads as its object does.
    const std::string     text    = "// starts over\n" R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "project_new", "args": {"discard_changes": true}}]})";
    const StepDescription as_text = describe(json{{"tool", "replay_run"}, {"args", {{"replay", text}}}}.dump());
    CHECK(flags_of(as_text) == Flags{"replaces_project"});
    CHECK(as_text.what == json{{"kind", "replay"}, {"steps", 1}});

    const StepDescription own_output = describe(
        R"({"tool": "replay_run", "args": {"replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": "window_screenshot", "args": {"save_path": "${out_dir}/n.png"}}]}}})");
    CHECK(flags_of(own_output).empty());

    // The outer replay substitutes an inline replay's placeholders before the nested run starts, as
    // replay_run does: the step's own vars come too late, and its out_dir doesn't move ${out_dir}.
    const StepDescription outer_vars = describe(
        R"({"tool": "replay_run", "args": {"replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": "project_open", "args": {"path": "${vars.p}"}}]}}})",
        R"({"p": "C:/a/p.3mf"})");
    CHECK(flags_of(outer_vars) == Flags{"replaces_project"});
    CHECK(outer_vars.replace == "checked");

    const StepDescription step_vars = describe(
        R"({"tool": "replay_run", "args": {"vars": {"p": "C:/a/p.3mf"}, "replay": {"orca_replay_version": "0.2.0", "vars": {"p": "C:/a/q.3mf"}, "steps": [{"tool": "project_open", "args": {"path": "${vars.p}"}}]}}})");
    CHECK(flags_of(step_vars).empty());
    CHECK(step_vars.replace == "");

    const StepDescription own_folder = describe(
        R"({"tool": "replay_run", "args": {"out_dir": "C:/elsewhere", "replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": "window_screenshot", "args": {"save_path": "${out_dir}/n.png"}}]}}})");
    CHECK(flags_of(own_folder).empty());

    const StepDescription mixed = describe(
        R"({"tool": "replay_run", "args": {"replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": "action_run", "args": {"id": "orca_command:new_project:orca"}}, {"tool": "project_new", "args": {"silent": true}}]}}})");
    CHECK(mixed.replace == "checked");

    const StepDescription clicking = describe(
        R"({"tool": "replay_run", "args": {"replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": "widget_click", "args": {"target": {"label": "No"}}}]}}})");
    CHECK(clicking.answers_dialogs);

    // Five levels of nesting: past the inspection depth, so the effect can't be read.
    std::string deep = R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "project_new"}]})";
    for (int i = 0; i < 5; ++i)
        deep = R"({"orca_replay_version": "0.2.0", "steps": [{"tool": "replay_run", "args": {"replay": )" + deep + "}}]}";
    const StepDescription too_deep = describe(R"({"tool": "replay_run", "args": {"replay": )" + deep + "}}");
    CHECK(flags_of(too_deep) == kEveryFlag);
    CHECK(too_deep.replace == "checked");
    CHECK(too_deep.answers_dialogs);
    CHECK(too_deep.what == json{{"kind", "replay"}, {"steps", 1}});

    const StepDescription invalid = describe(R"({"tool": "replay_run", "args": {"replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": 3}]}}})");
    CHECK(flags_of(invalid) == kEveryFlag);
    REQUIRE(invalid.what.value("kind", "") == "value");
    CHECK(invalid.what.value("text", "").find("step 1") != std::string::npos);

    const StepDescription broken_text = describe(json{{"tool", "replay_run"}, {"args", {{"replay", "{\"orca_replay_version\": \"0.2.0\",\n \"steps\": ["}}}}.dump());
    CHECK(flags_of(broken_text) == kEveryFlag);
    REQUIRE(broken_text.what.value("kind", "") == "value");
    CHECK(broken_text.what.value("text", "").find("line 2") != std::string::npos);

    const StepDescription missing = describe(R"({"tool": "replay_run", "args": {"path": "C:/no/such/replay.json"}})");
    CHECK(flags_of(missing) == kEveryFlag);
    CHECK(missing.replace == "checked");
    CHECK(missing.answers_dialogs);
    CHECK(missing.what == json{{"kind", "files"}, {"names", {"replay.json"}}});
}

// The runner describes steps without the saved jobs a single step runs with: ${jobs.NAME} resolves
// to a job id such as "j12", never a path, so the effects read the same either way.
TEST_CASE("saved jobs don't change a step's flags", "[Mcp]")
{
    const std::string step = GENERATE(as<std::string>{}, R"({"tool": "project_open", "args": {"path": "${jobs.p}"}})",
                                      R"({"tool": "window_screenshot", "args": {"save_path": "${jobs.p}/a.png"}})",
                                      R"({"tool": "preset_export", "args": {"path": "${jobs.p}"}})");
    INFO(step);
    std::string err;
    const auto  replay = parse_replay(R"({"orca_replay_version": "0.2.0", "steps": [)" + step + "]}", err);
    REQUIRE(replay.has_value());
    ReplayContext without;
    without.out_dir = "C:/out";
    ReplayContext with = without;
    with.jobs          = json{{"p", "j12"}};
    const StepDescription a = describe_step(tile_catalog(), replay->steps[0], *replay, without);
    const StepDescription b = describe_step(tile_catalog(), replay->steps[0], *replay, with);
    CHECK(flags_of(a) == flags_of(b));
    CHECK(a.replace == b.replace);
    CHECK(a.answers_dialogs == b.answers_dialogs);
}

TEST_CASE("a step's what names what it acts on", "[Mcp]")
{
    const auto [step, what] = GENERATE(table<std::string, json>({
        {R"({"tool": "settings_set", "args": {"values": {"layer_height": 0.12, "wall_loops": "3"}}})",
         {{"kind", "settings"}, {"pairs", {"layer_height = 0.12", "wall_loops = 3"}}}},
        {R"({"tool": "project_import", "args": {"paths": ["C:/m/3DBenchy.stl", "C:/m/cube.stl"]}})",
         {{"kind", "files"}, {"names", {"3DBenchy.stl", "cube.stl"}}}},
        {R"({"tool": "window_screenshot", "args": {"save_path": "${out_dir}/preview.png"}})", {{"kind", "files"}, {"names", {"preview.png"}}}},
        {R"({"tool": "plate_slice"})", {{"kind", "plates"}, {"plates", "current"}}},
        {R"({"tool": "plate_slice", "args": {"plates": 2}})", {{"kind", "plates"}, {"plates", 2}}},
        {R"({"tool": "object_arrange"})", {{"kind", "plates"}, {"plates", "all"}}},
        {R"({"tool": "preset_select", "args": {"type": "filament", "name": "Generic PLA"}})", {{"kind", "preset"}, {"name", "Generic PLA"}}},
        {R"({"tool": "app_wait", "args": {"for": "idle"}})", {{"kind", "value"}, {"text", "idle"}}},
        {R"({"tool": "dialog_answer", "args": {"button": "no"}})", {{"kind", "value"}, {"text", "no"}}},
        {R"({"tool": "object_transform", "args": {"object": 1}})", {{"kind", "value"}, {"text", "object: 1"}}},
        {R"({"tool": "app_state"})", nullptr},
    }));
    INFO(step);
    CHECK(describe(step).what == what);
}

TEST_CASE("a step with an expect checks its result", "[Mcp]")
{
    CHECK(describe(R"({"tool": "plate_slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}})").checks_result);
    CHECK_FALSE(describe(R"({"tool": "plate_slice"})").checks_result);
}

TEST_CASE("results are described from the tools' real replies", "[Mcp]")
{
    CHECK(describe_result("project_import", {{"objects", 1}, {"names", {"Benchy"}}, {"plates", 1}}) == json{{"kind", "objects"}, {"count", 1}});
    CHECK(describe_result("calibration_generate_test", {{"objects", 3}, {"plates", 1}}) == json{{"kind", "objects"}, {"count", 3}});
    CHECK(describe_result("project_open", {{"opened", "gcode"}, {"path", "C:/a/x.gcode"}}) == json{{"kind", "file"}, {"name", "x.gcode"}});
    CHECK(describe_result("plate_slice", {{"plates", {{{"index", 0}, {"outcome", "ok"}}, {{"index", 1}, {"outcome", "error"}}}}}) ==
          json{{"kind", "plates"}, {"ok", 1}, {"other", 1}});
    CHECK(describe_result("window_screenshot", {{"path", "C:/out/a.png"}}) == json{{"kind", "file"}, {"name", "a.png"}});
    CHECK(describe_result("gizmo_pick_file", {{"saved", "C:/x/a.svg"}}) == json{{"kind", "file"}, {"name", "a.svg"}});
    CHECK(describe_result("replay_run", {{"passed", false}, {"failed_step", 2}, {"steps", json::array()}}) ==
          json{{"kind", "replay"}, {"passed", false}, {"failed_step", 2}});
    CHECK(describe_result("replay_run", {{"passed", true}, {"steps", json::array()}}) == json{{"kind", "replay"}, {"passed", true}});
    CHECK(describe_result("app_state", {{"ready", true}}).is_null());

    CHECK(result_files({{"saved", "C:/x/a.svg"}}) == std::vector<std::string>{"C:/x/a.svg"});
    CHECK(result_files({{"path", "C:/out/a.png"}}) == std::vector<std::string>{"C:/out/a.png"});
    CHECK(result_files({{"path", ""}}).empty());
    CHECK(result_files(json::object()).empty());
}

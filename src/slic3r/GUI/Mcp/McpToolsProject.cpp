// open_files, new_project, save_project and export: files in and out without file dialogs.

#include "libslic3r/Format/ProjectStrategy.hpp"
#include "McpPaths.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Model.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

bool has_extension(const fs::path& p, std::initializer_list<const char*> exts)
{
    const std::string name = path_to_utf8(p.filename());
    for (const char* e : exts)
        if (boost::algorithm::iends_with(name, e))
            return true;
    return false;
}

json objects_summary(Plater& plater)
{
    json names = json::array();
    for (const ModelObject* o : plater.model().objects)
        names.push_back(o->name);
    return {{"objects", plater.model().objects.size()}, {"names", std::move(names)}, {"plates", plater.get_partplate_list().get_plate_count()}};
}

// Discards unsaved changes without a prompt when the caller said so, or fails.
void require_clean_or_discard(Plater& plater, bool discard)
{
    if (!plater.is_project_dirty())
        return;
    if (!discard)
        throw ToolFailure("The project has unsaved changes; save_project first, or pass discard_changes: true");
    plater.new_project(/*skip_confirm=*/true, /*silent=*/true);
}

} // namespace

void register_project_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    reg.add(mutate_tool(
        "open_files", "Open files",
        "Open model files (STL, 3MF, STEP, OBJ, AMF, SVG...), a project (.3mf) or G-code by absolute path, without a file "
        "dialog. Models are added to the current project. A .3mf opens as a project (replacing the current one: pass "
        "discard_changes when it has unsaved changes) unless as is \"geometry\". Importing can open a dialog (STEP mesh "
        "options, for instance); the reply is then blocked_by_dialog.",
        object_schema({{"paths", array_prop({{"type", "string"}}, "Absolute paths.")},
                       {"as", enum_prop({"auto", "geometry", "project"}, "How to open .3mf files. Default auto (= project).")},
                       {"discard_changes", prop("boolean", "Replace a project with unsaved changes without asking.")}},
                      {"paths"}),
        [](const json& args, ToolContext& ctx) {
            std::vector<fs::path> paths;
            for (const json& p : args["paths"]) {
                std::string err;
                auto        resolved = resolve_input_path(p.get<std::string>(), err);
                if (!resolved)
                    throw ToolFailure(err);
                paths.push_back(*resolved);
            }
            if (paths.empty())
                throw ToolFailure("paths is empty");
            const std::string as      = arg_string(args, "as", "auto");
            const bool        discard = arg_bool(args, "discard_changes", false);
            json out = ctx.run_ui([&]() -> json {
                Plater& plater = ui_plater();
                if (paths.size() == 1 && has_extension(paths[0], {".gcode", ".bgcode"})) {
                    plater.load_gcode(from_path(paths[0]));
                    return {{"opened", "gcode"}, {"path", path_to_utf8(paths[0])}};
                }
                if (paths.size() == 1 && as != "geometry" && has_extension(paths[0], {".3mf"})) {
                    require_clean_or_discard(plater, discard);
                    plater.load_project(from_path(paths[0]));
                    json o   = objects_summary(plater);
                    o["opened"] = "project";
                    return o;
                }
                const size_t before = plater.model().objects.size();
                const auto   loaded = plater.load_files(paths, LoadStrategy::LoadModel);
                if (loaded.empty() && plater.model().objects.size() == before)
                    throw ToolFailure("Nothing was loaded (see get_log for the reason)");
                json o      = objects_summary(plater);
                o["opened"] = "models";
                o["added"]  = plater.model().objects.size() - before;
                return o;
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(mutate_tool(
        "new_project", "New project",
        "Start a new, empty project. Fails when the current project has unsaved changes unless discard_changes is true.",
        object_schema({{"discard_changes", prop("boolean", "Drop unsaved changes without asking.")}}),
        [](const json& args, ToolContext& ctx) {
            const bool discard = arg_bool(args, "discard_changes", false);
            json       out     = ctx.run_ui([&] {
                Plater& plater = ui_plater();
                if (plater.is_project_dirty() && !discard)
                    throw ToolFailure("The project has unsaved changes; save_project first, or pass discard_changes: true");
                plater.new_project(/*skip_confirm=*/true, /*silent=*/true);
                return objects_summary(plater);
            });
            settle(ctx);
            return ToolResult::ok(out);
        },
        true));

    reg.add(mutate_tool(
        "save_project", "Save the project",
        "Save the project as a .3mf at an absolute path without a dialog; it becomes the project's file.",
        object_schema({{"path", prop("string", "Absolute .3mf path.")}, {"overwrite", prop("boolean", "Replace an existing file.")}}, {"path"}),
        [](const json& args, ToolContext& ctx) {
            std::string err;
            auto        path = resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err);
            if (!path)
                throw ToolFailure(err);
            ctx.run_ui([&] {
                ui_plater().save_project(true, from_path(*path));
                return json();
            });
            settle(ctx);
            if (!fs::exists(*path))
                throw ToolFailure("The project was not saved (see get_log)");
            return ToolResult::ok({{"path", path_to_utf8(*path)}, {"bytes", fs::file_size(*path)}});
        }));

    reg.add(mutate_tool(
        "export", "Export",
        "Write the project to a file without a dialog. gcode: the current plate's G-code (slice first; runs in the "
        "background and the call completes when the file is written). sliced_3mf: the sliced plate (or all plates) as "
        ".gcode.3mf. 3mf: the model without slicing data. stl: all objects as one STL. plate selects the plate first.",
        object_schema({{"format", enum_prop({"gcode", "sliced_3mf", "3mf", "stl"}, "What to write.")},
                       {"path", prop("string", "Absolute output path.")},
                       {"plate", {{"type", {"string", "integer"}}, {"description", "\"current\" (default), a plate index, or \"all\" (sliced_3mf only)."}}},
                       {"overwrite", prop("boolean", "Replace an existing file.")}},
                      {"format", "path"}),
        [](const json& args, ToolContext& ctx) {
            const std::string format = arg_string(args, "format");
            std::string       err;
            auto              path = resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err);
            if (!path)
                throw ToolFailure(err);
            const json plate = args.value("plate", json("current"));
            const bool all   = plate.is_string() && plate.get<std::string>() == "all";
            if (all && format != "sliced_3mf")
                throw ToolFailure("plate \"all\" applies to sliced_3mf only");
            json started = ctx.run_ui([&]() -> json {
                // A G-code export finishes in the background: held from before it starts.
                if (format == "gcode")
                    ctx.begin_operation();
                Plater& plater = ui_plater();
                if (plater.model().objects.empty())
                    throw ToolFailure("The project has no objects");
                if (plate.is_number_integer()) {
                    const int index = plate.get<int>();
                    if (index < 0 || index >= plater.get_partplate_list().get_plate_count())
                        throw ToolFailure("plate index out of range");
                    plater.select_plate(index);
                }
                if (format == "gcode") {
                    if (plater.is_slicing_any())
                        throw ToolFailure("Slicing is in progress; wait {for:\"idle\"} and export again");
                    if (!plater.get_partplate_list().get_curr_plate()->is_slice_result_valid())
                        throw ToolFailure("The plate is not sliced; slice it first");
                    const uint64_t before   = plater.export_seq();
                    const uint64_t revision = plater.project_revision();
                    plater.export_gcode_to(*path);
                    return {{"before", before}, {"after", plater.export_seq()}, {"revision", revision}};
                }
                if (format == "sliced_3mf") {
                    if (!plater.export_gcode_3mf(all, *path))
                        throw ToolFailure("The sliced file was not written; slice first and check for errors");
                } else if (format == "3mf") {
                    plater.export_core_3mf(from_path(*path));
                } else {
                    plater.export_stl(false, false, false, FT_STL, from_path(*path));
                }
                return json();
            });
            if (format == "gcode")
                return await_gcode_export(ctx, started["before"], started["after"], started["revision"],
                                          "The export did not start; check get_state.notifications");
            settle(ctx);
            if (!fs::exists(*path))
                throw ToolFailure("The file was not written (see get_log)");
            return ToolResult::ok({{"path", path_to_utf8(*path)}, {"bytes", fs::file_size(*path)}});
        }));
}

}}} // namespace Slic3r::GUI::Mcp

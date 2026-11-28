// The place The project: project_new, project_open, project_import, project_save and project_export, the project's
// files in and out without file dialogs, and called as their menu items' actions, with them; and
// project_get_settings and project_set_settings, the project's own settings, which the settings code of
// PresetTools.cpp reads and writes.

#include "slic3r/GUI/Mcp/AutomationRuntime.hpp"
#include "slic3r/GUI/Mcp/JobRegistry.hpp"
#include "slic3r/GUI/Mcp/McpPaths.hpp"
#include "slic3r/GUI/Mcp/McpTargets.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/Operations/ExportOps.hpp"
#include "slic3r/GUI/Operations/ObjectOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/ProjectOps.hpp"
#include "slic3r/GUI/Operations/RunStatus.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Model.hpp"

#include <wx/defs.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
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

} // namespace

json objects_summary(Plater& plater)
{
    json names = json::array();
    for (const ModelObject* o : plater.model().objects)
        names.push_back(o->name);
    return {{"objects", plater.model().objects.size()}, {"names", std::move(names)}, {"plates", plater.get_partplate_list().get_plate_count()}};
}

namespace {

json project_json(const Ops::ProjectSummary& project)
{
    return {{"objects", project.object_names.size()}, {"names", project.object_names}, {"plates", project.plates}};
}

// The answer about the project's unsaved changes that a command replacing the project gives: discard them, else refuse.
Ops::UnsavedChanges unsaved_changes_answer(bool discard) { return discard ? Ops::UnsavedChanges::Discard : Ops::UnsavedChanges::Refuse; }

// What a command that replaces the project adds to the refusal of unsaved changes.
constexpr const char* kUnsavedChangesHint = "project_save first, or pass discard_changes: true.";

} // namespace

void require_project_discardable(bool discard)
{
    if (const std::optional<Ops::Refusal> refusal = Ops::unsaved_changes_refusal(unsaved_changes_answer(discard)))
        throw refused(*refusal, kUnsavedChangesHint);
}

ToolResult await_gcode_export(ToolContext& ctx, const Ops::ExportTicket& ticket)
{
    json done = poll_ui(
        ctx,
        [&ticket]() -> json {
            ui_plater();
            std::string             error;
            const Ops::ExportStatus status = Ops::gcode_export_state(ticket, &error);
            if (status == Ops::ExportStatus::Pending)
                return json();
            json out{{"export", Ops::to_string(status)}, {"path", ticket.path}};
            if (!error.empty())
                out["error"] = error;
            return out;
        },
        std::chrono::minutes(10), "the export to finish");
    const std::string status = done["export"];
    if (status == "ok")
        return ToolResult::ok(done);
    if (status == "superseded") {
        done["note"] = "The project changed while exporting; the file reflects the project as it was when the export started.";
        return ToolResult::ok(done);
    }
    return ToolResult::error("The export ended " + status, done);
}

namespace {

// project_export's format for a path given without one, from its extension.
std::string format_of(const std::string& path)
{
    static const std::pair<const char*, const char*> formats[]{{".gcode.3mf", "sliced_3mf"}, {".gcode", "gcode"}, {".3mf", "3mf"},
                                                               {".stl", "stl"},           {".drc", "drc"},     {".obj", "toolpaths_obj"}};
    for (const auto& [ext, format] : formats)
        if (boost::algorithm::iends_with(path, ext))
            return format;
    throw ToolFailure("Give format (gcode, sliced_3mf, 3mf, stl, drc or toolpaths_obj), or a path ending in .gcode, .gcode.3mf, "
                      ".3mf, .stl, .drc or .obj");
}

} // namespace

void register_project_tools(CommandCatalog& catalog)
{
    NativeCommand open = reaching({"open_project"}, mutate_tool(
        "project_open", L("Open a project"),
        "Open a project (.3mf) by absolute path, without a file dialog, replacing the current project, with the settings it "
        "was saved with whatever the Preferences' Load behavior says; when the current project has unsaved changes that "
        "fails, changing nothing, unless `discard_changes`. A G-code file replaces the project too, to show its toolpaths: "
        "with unsaved changes the app asks first (`blocked_by_dialog`), whatever `discard_changes` says. `project_import` adds "
        "models, or a project's objects alone, to the current project. Called with no arguments it is Load Project: the "
        "Open dialog asks for the file (`blocked_by_dialog`), after asking about the unsaved changes. Reply: opened (project "
        "or gcode), and objects, names and plates for a project, path for a G-code.",
        object_schema({{"path", prop("string", "Absolute path of a .3mf project, or of a G-code file (.gcode, .bgcode).")},
                       {"discard_changes", prop("boolean", "Drop the project's unsaved changes rather than fail when a project "
                                                           "replaces it.")}},
                      {"path"}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            std::string        err;
            const auto         path = resolve_input_path(arg_string(args, "path"), err);
            if (!path)
                throw ToolFailure(err);
            const bool gcode = has_extension(*path, {".gcode", ".bgcode"});
            if (!gcode && !has_extension(*path, {".3mf"}))
                throw ToolFailure("project_open opens a .3mf project or a G-code file; project_import adds models to the project");
            // A run the user let drop unsaved changes (Run Script's "don't save") counts as discard_changes.
            const bool discard = arg_bool(args, "discard_changes", false) || runtime.jobs().discard_allowed_in(ctx.lineage());
            json out = ctx.run_ui([&]() -> json {
                Plater& plater = ui_plater();
                if (gcode) {
                    plater.load_gcode(from_path(*path));
                    return {{"opened", "gcode"}, {"path", path_to_utf8(*path)}};
                }
                Ops::OpenProjectArgs open;
                open.path    = *path;
                open.unsaved = unsaved_changes_answer(discard);
                const Ops::Outcome<Ops::ProjectSummary> opened = Ops::open_project(open);
                if (!opened)
                    throw refused(opened.refusal(), kUnsavedChangesHint);
                json o      = project_json(opened.value());
                o["opened"] = "project";
                return o;
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));
    open.actions = json::array({NativeCommands::action("load_project", L("Load Project"), L("Commands"), "menu_open", json::object(),
                                                       "Open a project through the Open dialog (it asks first if the current project "
                                                       "has unsaved changes); project_open with a path opens that file without a "
                                                       "dialog.")});
    catalog.add(NativeCommands::with_person_form(std::move(open), [](const json&) { return MenuItems::load_project(); }));

    NativeCommand import_ = reaching({"import_models"}, mutate_tool(
        "project_import", L("Import models"),
        "Add the models of files (STL, 3MF, STEP, OBJ, AMF, SVG...) to the current project by absolute path, without a file "
        "dialog, as one undo step; a .3mf adds its objects only, not its settings (`project_open` opens it as a project). A "
        ".zip archive gives the model files it holds (STL, OBJ, AMF, 3MF, STEP; a .3mf its objects only, as above), as Import "
        "ZIP Archive does: those entries names, by their paths in the archive, else all of them, in place of its file list; "
        "an entry the archive lacks is refused with the names it has, before anything is imported. Importing can open a "
        "dialog (STEP mesh options, for instance); the reply is then `blocked_by_dialog`. Called with no arguments it is "
        "Import 3MF/STL/STEP/SVG/OBJ/AMF, and with `archive: true` alone Import ZIP Archive: a file dialog asks for the files, "
        "or for the archive and then which of its files to import (`blocked_by_dialog`). Reply: opened (models), added (how "
        "many objects), archives (each archive's path and the entries taken) and the project's objects, names and plates.",
        object_schema({{"paths", array_prop({{"type", "string"}}, "Absolute paths of model files, or of a .zip archive of them.")},
                       {"entries", {{"type", "array"},
                                    {"items", {{"type", "string"}}},
                                    {"minItems", 1},
                                    {"description", "The files to take from the one .zip archive among paths, by their paths "
                                                    "in it (\"parts/cube.stl\"). Default: every model file it holds."}}},
                       {"archive", prop("boolean", "true, alone: ask for a ZIP archive in a file dialog and import the models "
                                                   "it holds, as Import ZIP Archive does.")}},
                      {"paths"}),
        [](const json& args, ToolContext& ctx) {
            if (arg_bool(args, "archive", false))
                throw ToolFailure("archive asks for the archive in a file dialog, so it goes alone; paths imports files without one");
            if (args.contains("entries")) {
                const json& given = args["paths"];
                const auto  zips  = std::count_if(given.begin(), given.end(), [](const json& p) {
                    return boost::algorithm::iends_with(p.get<std::string>(), ".zip");
                });
                if (zips == 0)
                    throw ToolFailure("entries names files of a .zip archive: give the archive in paths");
                if (zips > 1)
                    throw ToolFailure("entries names the files of one archive: give one .zip in paths");
            }
            std::vector<fs::path> paths;
            for (const json& p : args["paths"]) {
                std::string err;
                auto        resolved = resolve_input_path(p.get<std::string>(), err);
                if (!resolved)
                    throw ToolFailure(err);
                if (has_extension(*resolved, {".gcode", ".bgcode"}))
                    throw ToolFailure(p.get<std::string>() + " is a G-code file, which project_open opens to show its toolpaths");
                paths.push_back(*resolved);
            }
            if (paths.empty())
                throw ToolFailure("paths is empty");
            json out = ctx.run_ui([&]() -> json {
                Plater&               plater = ui_plater();
                Ops::ImportModelsArgs models;
                models.paths = paths;
                if (args.contains("entries"))
                    models.entries = args["entries"].get<std::vector<std::string>>();
                const Ops::Outcome<Ops::ImportModelsResult> imported = Ops::import_models(models);
                if (!imported) {
                    const bool nothing = std::string(imported.refusal().code) == "nothing_loaded";
                    throw refused(imported.refusal(), nothing ? "See app_log for the reason." : "", "entries");
                }
                json o      = objects_summary(plater);
                o["opened"] = "models";
                o["added"]  = imported.value().added;
                if (!imported.value().archives.empty()) {
                    json archives = json::array();
                    for (const Ops::ImportedArchive& archive : imported.value().archives)
                        archives.push_back({{"path", path_to_utf8(archive.path)}, {"entries", archive.entries}});
                    o["archives"] = std::move(archives);
                }
                return o;
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));
    import_.actions = json::array(
        {NativeCommands::action("import_file", L("Import 3MF/STL/STEP/SVG/OBJ/AMF"), L("Import"), "menu_open", json::object(),
                                "Import models (3MF/STL/STEP/SVG/OBJ/AMF) through a file dialog, as one undo step; a project "
                                "file's settings are not loaded (project_open opens a project). project_import with paths adds "
                                "files' geometry without a dialog."),
         NativeCommands::action("import_zip_archive", L("Import ZIP Archive"), L("Import"), "menu_open", {{"archive", true}},
                                "Import models from a ZIP archive (opens a file dialog).")});
    catalog.add(NativeCommands::with_person_form(std::move(import_), [](const json& action_args) {
        return action_args.value("archive", false) ? MenuItems::import_zip_archive() : MenuItems::import_file();
    }));

    // New Project's menu item is its action: with no arguments it asks as the item does.
    NativeCommand project_new = reaching({"new_project"}, mutate_tool(
        "project_new", L("New Project"),
        "Start a new, empty project. Called with no arguments, it asks as New Project does, about the unsaved changes "
        "if there are any: the reply is then `blocked_by_dialog`, and `dialog_answer` answers it. With arguments, or `silent: "
        "true`, it asks nothing: it fails, changing nothing, when the current project has unsaved changes, unless "
        "`discard_changes`. The presets' unsaved changes carry over to the new project unless `preset_changes` is \"discard\", as "
        "New Project's prompt offers. Reply, with arguments: objects, names, plates; when presets had or have unsaved "
        "changes, discarded and modified list per preset the keys dropped and the keys still unsaved. With no arguments "
        "the reply is ran and level, also once a prompt is answered, whatever the answer: Cancel keeps the project.",
        object_schema({{"discard_changes", prop("boolean", "Drop the project's unsaved changes rather than fail; for the "
                                                           "presets', see `preset_changes`.")},
                       {"preset_changes", enum_prop({"keep", "discard"}, "The presets' unsaved changes, not the project's: keep "
                                                                  "them for the new project (default) or drop them.")}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            const bool discard         = arg_bool(args, "discard_changes", false) || runtime.jobs().discard_allowed_in(ctx.lineage());
            const bool discard_presets = arg_string(args, "preset_changes", "keep") == "discard";
            json       out             = ctx.run_ui([&] {
                ui_plater();
                const json          before = unsaved_changes();
                Ops::NewProjectArgs fresh;
                fresh.unsaved                = unsaved_changes_answer(discard);
                fresh.discard_preset_changes = discard_presets;
                const Ops::Outcome<Ops::ProjectSummary> started = Ops::new_project(fresh);
                if (!started)
                    throw refused(started.refusal(), kUnsavedChangesHint);
                json o = project_json(started.value());
                o.update(unsaved_changes_outcome(before));
                return o;
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));
    project_new.actions = json::array({NativeCommands::action("new_project", L("New Project"), L("Commands"), "menu_open", json::object(),
                                                                   "Runs project_new with no arguments, as New Project does: a new, empty "
                                                                   "project, after asking about the unsaved changes if there are any.")});
    catalog.add(NativeCommands::with_person_form(std::move(project_new), [](const json&) {
        if (Plater* plater = wxGetApp().plater())
            plater->new_project();
        return AppActionRunResult{AppActionRunResult::Level::Succeeded};
    }));

    // Save Project's and Save Project As's menu items are its actions: with no arguments, or with save_as alone, it saves as
    // the item does.
    NativeCommand save = reaching({"save_project_as"}, mutate_tool(
        "project_save", L("Save Project"),
        "Save the project. Called with no arguments, it saves as Save Project does: to the project's file (reply: ran, "
        "level), or, when it has none, through a file dialog (`blocked_by_dialog`; a native picker needs `dialog_auto_answer` "
        "registered first); with `save_as: true` alone, as Save Project As does, always through the file dialog. With path, "
        "it saves a .3mf there without the file dialog, and the file becomes the project's (reply: path, bytes); a name "
        "ending in .gcode.3mf is refused (`project_export` writes that). Either way the save can still open a dialog "
        "(`blocked_by_dialog`): a warning when settings name a plugin OrcaSlicer cannot identify, or an error when the file "
        "cannot be written. A save that writes nothing, its file dialog or that warning cancelled or the write failed, "
        "fails with `project_not_saved`, and a file already there stays as it was.",
        object_schema({{"path", prop("string", "Absolute .3mf path.")},
                       {"overwrite", prop("boolean", "Replace an existing file.")},
                       {"save_as", prop("boolean", "true, alone: ask for the file in the save dialog, as Save Project As does. With "
                                                   "path it adds nothing: path is the new file.")}},
                      {"path"}),
        [](const json& args, ToolContext& ctx) {
            std::string err;
            auto        path = resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err);
            if (!path)
                throw ToolFailure(err);
            ctx.run_ui([&] {
                ui_plater();
                Ops::SaveProjectAsArgs save;
                save.path = *path;
                if (const Ops::Outcome<Ops::Done> saved = Ops::save_project_as(save); !saved)
                    throw refused(saved.refusal(), std::string(saved.refusal().code) == "sliced_file_name" ?
                                                       "That is project_export {format: \"sliced_3mf\"} (plates \"all\" for every plate)." :
                                                       "See app_log.");
                return json();
            });
            settle(ctx);
            return ToolResult::ok({{"path", path_to_utf8(*path)}, {"bytes", fs::file_size(*path)}});
        }));
    save.actions = json::array(
        {NativeCommands::action("save_project", L("Save Project"), L("Commands"), "menu_save", json::object(),
                                "Runs project_save with no arguments, as Save Project does: to the project's file, or through a "
                                "file dialog when it has none."),
         NativeCommands::action("save_project_as", L("Save Project As"), L("Commands"), "menu_save", {{"save_as", true}},
                                "Save the project as a .3mf file through the save dialog. project_save with a path saves there "
                                "without a dialog; a name ending in .gcode.3mf is refused (project_export {format: \"sliced_3mf\"} "
                                "writes that).")});
    save = NativeCommands::with_person_form(std::move(save), [](const json& action_args) {
        if (action_args.value("save_as", false))
            return MenuItems::save_project_as();
        if (Plater* plater = wxGetApp().plater())
            return NativeCommands::project_save_result(plater->save_project(false));
        return AppActionRunResult{AppActionRunResult::Level::Succeeded};
    });
    // Every call but the two actions' requires path, and a call with path skips the file dialog: silent has nothing to add.
    save.input_schema["properties"]["silent"]["description"] =
        "Goes with path, which every call but {} and `{save_as: true}` requires, and adds nothing to it: a call with path skips "
        "the file dialog already.";
    catalog.add(save);

    NativeCommand export_ = reaching(
        {"start_gcode_export", "export_sliced_3mf", "export_model_3mf", "export_stl", "export_toolpaths_obj"},
        with_target_ids(mutate_tool(
        "project_export", L("Export"),
        "Write the project to a file without a dialog. gcode: the plate's G-code (slice first; it is written in the "
        "background, and the call returns once it is). `sliced_3mf`: the sliced plate, or all plates, as .gcode.3mf (slice "
        "first: a plate that is not sliced is written without G-code). 3mf: the model without slicing data. stl and drc "
        "(Draco): the objects as one file, or with `per_object` each in a file of its own, named after it, into the folder "
        "path names; every object of every plate, or those objects names. `toolpaths_obj`: the toolpaths the Preview shows, "
        "as an OBJ, refused while it shows none (slice the plate and show the Preview first). Without format, the path's "
        "extension (.gcode, .gcode.3mf, .3mf, .stl, .drc, .obj) says which. Called with exactly the arguments of one of the "
        "File > Export menu items (format alone, `sliced_3mf` with plates \"all\", stl or drc with `per_object`), it asks for the file "
        "or folder in the dialog that item opens (`blocked_by_dialog`). A file per object of a project with no objects, and the "
        "toolpaths while the Preview shows none, are refused in either form. Reply: path and bytes; for gcode, path and "
        "export (ok, or superseded with a note when the project changed while it was written); for `per_object`, path and "
        "files (each file written); for stl and drc, objects (those written).",
        with_targets(object_schema({{"format", enum_prop({"gcode", "sliced_3mf", "3mf", "stl", "drc", "toolpaths_obj"},
                                                         "What to write; default from the path's extension.")},
                                    {"path", prop("string", "Absolute output path: the file, or with `per_object` an existing folder.")},
                                    {"plates", {{"type", {"string", "integer"}},
                                                {"description", "\"current\" (default), a plate index, which is selected first and is "
                                                                "the plate gcode and `sliced_3mf` write, or \"all\" (`sliced_3mf` only)."}}},
                                    {"per_object", prop("boolean", "stl and drc: a file per object, into a folder. Default false.")},
                                    {"overwrite", prop("boolean", "Replace an existing file.")}},
                                   {"path"}),
                     kObjects, false),
        [](const json& args, ToolContext& ctx) {
            const std::string format     = args.contains("format") ? arg_string(args, "format") : format_of(arg_string(args, "path"));
            const bool        per_object = arg_bool(args, "per_object", false);
            const bool        meshes     = format == "stl" || format == "drc";
            if (per_object && !meshes)
                throw ToolFailure("per_object goes with format stl or drc");
            if (per_object && (args.contains("plates") || args.contains("overwrite")))
                throw ToolFailure("per_object writes every object into the folder path names: plates and overwrite go with one file");
            if (format == "toolpaths_obj" && args.contains("plates"))
                throw ToolFailure("toolpaths_obj writes the toolpaths the Preview shows: plates goes with the other formats");
            const json plate = args.value("plates", json("current"));
            const bool all   = plate.is_string() && plate.get<std::string>() == "all";
            if (plate.is_string() && !all && plate.get<std::string>() != "current")
                throw ToolFailure("plates is \"all\", \"current\" or a plate index");
            if (all && format != "sliced_3mf")
                throw ToolFailure("plates \"all\" applies to sliced_3mf only");
            std::string err;
            const auto  path = per_object ? resolve_output_dir(arg_string(args, "path"), err) :
                                            resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err);
            if (!path)
                throw ToolFailure(err);
            std::optional<Ops::ExportTicket> ticket;
            json                             written = json::object(); // what the reply adds for stl and drc
            ctx.run_ui([&]() -> json {
                // A G-code export finishes in the background: held from before it starts.
                if (format == "gcode")
                    ctx.begin_operation();
                Plater& plater = ui_plater();
                const std::optional<int> index = plate.is_number_integer() ? std::optional<int>(plate.get<int>()) : std::nullopt;
                // The export's own refusals of the object indices given come before the target check, which
                // refuses an index the project lacks without no_such_object's code.
                std::vector<int> indices;
                if (args.contains("object") && args.at("object").is_number_integer())
                    indices.push_back(args.at("object").get<int>());
                for (const json& entry : args.value("objects", json::array()))
                    if (entry.is_number_integer())
                        indices.push_back(entry.get<int>());
                if (const std::optional<Ops::Refusal> refusal = Ops::export_objects_refusal(format, indices, per_object, plater.model()))
                    throw refused(*refusal);
                // The objects named, by index or id; none named: every object.
                Targets named;
                if (names_targets(args)) {
                    const Ops::Outcome<Targets> found = resolve_targets(plater.model(), nullptr, args, kObjects, false);
                    if (!found)
                        throw refused(found.refusal());
                    named = found.value();
                }
                if (const std::optional<Ops::Refusal> refusal =
                        Ops::export_objects_refusal(format, named.objects, per_object, plater.model()))
                    throw refused(*refusal);
                if (meshes) {
                    Ops::ExportStlArgs mesh;
                    mesh.path       = *path;
                    mesh.plate      = index;
                    mesh.objects    = named.objects;
                    mesh.per_object = per_object;
                    mesh.draco      = format == "drc";
                    const Ops::Outcome<std::vector<fs::path>> files = Ops::export_stl(mesh);
                    if (!files)
                        throw refused(files.refusal(), std::string(files.refusal().code) == "file_not_written" ? "See app_log." : "");
                    Targets exported = named;
                    if (exported.objects.empty())
                        for (int o = 0; o < int(plater.model().objects.size()); ++o)
                            exported.objects.push_back(o);
                    written = targets_reply(plater.model(), exported);
                    if (per_object) {
                        written["files"] = json::array();
                        for (const fs::path& file : files.value())
                            written["files"].push_back(path_to_utf8(file));
                    }
                    return json();
                }
                if (format == "toolpaths_obj") {
                    if (const Ops::Outcome<Ops::Done> drawn = Ops::export_toolpaths_obj({*path}); !drawn)
                        throw refused(drawn.refusal(), std::string(drawn.refusal().code) == "no_toolpaths" ?
                                                           "plate_slice {preview: true} slices the plate and shows it." :
                                                           "See app_log.");
                    return json();
                }
                if (format == "gcode") {
                    Ops::ExportGcodeArgs gcode;
                    gcode.path  = *path;
                    gcode.plate = index;
                    const Ops::Outcome<Ops::ExportTicket> started = Ops::start_gcode_export(gcode);
                    if (!started) {
                        const std::string code = started.refusal().code;
                        throw refused(started.refusal(), code == "slicing_in_progress" ? "app_wait {for:\"idle\"} waits for it." :
                                                         code == "export_not_started"  ? "Check app_state.notifications." :
                                                                                         "");
                    }
                    ticket = started.value();
                    return json();
                }
                Ops::ExportSlicedArgs sliced;
                sliced.path       = *path;
                sliced.all_plates = all;
                sliced.plate      = index;
                Ops::ExportFileArgs file;
                file.path  = *path;
                file.plate = index;
                const Ops::Outcome<Ops::Done> done = format == "sliced_3mf" ? Ops::export_sliced_3mf(sliced) : Ops::export_model_3mf(file);
                if (!done)
                    throw refused(done.refusal(), std::string(done.refusal().code) == "file_not_written" ? "See app_log." : "");
                return json();
            });
            if (ticket)
                return await_gcode_export(ctx, *ticket);
            settle(ctx);
            json reply{{"path", path_to_utf8(*path)}};
            if (!per_object)
                reply["bytes"] = fs::file_size(*path);
            reply.update(written);
            return ToolResult::ok(reply);
        })));
    // The File > Export menu items, each with the dialog it asks for the file or folder in.
    export_.actions = json::array({
        NativeCommands::action("export_gcode", L("Export G-code"), L("Slice & Export"), "custom-gcode_gcode", {{"format", "gcode"}},
                               "Export the current plate's G-code through the export dialog, in the background. project_export "
                               "with a path exports the sliced plate there without a dialog, and the call returns when the file "
                               "is written; a plate that is not sliced is refused (slice first)."),
        NativeCommands::action("export_sliced_file", L("Export Sliced File"), L("Slice & Export"), "save", {{"format", "sliced_3mf"}},
                               "Export the current plate's sliced file (.gcode.3mf) through a file dialog; project_export with a "
                               "path writes there without one."),
        NativeCommands::action("export_all_sliced_file", L("Export All Sliced Files"), L("Slice & Export"), "save",
                               {{"format", "sliced_3mf"}, {"plates", "all"}},
                               "Export every plate's sliced file into one .gcode.3mf through a file dialog; project_export with a "
                               "path writes there without one."),
        NativeCommands::action("export_3mf", L("Export 3MF"), L("Slice & Export"), "menu_save", {{"format", "3mf"}},
                               "Export the model as a .3mf without slicing data, through a file dialog; project_export with a path "
                               "writes there without one."),
        NativeCommands::action("export_stl", L("Export STL"), L("Slice & Export"), "save", {{"format", "stl"}},
                               "Export all objects as one STL through a file dialog; project_export with a path writes there "
                               "without one."),
        NativeCommands::action("export_stl_multi", L("Export All Objects as STLs"), L("Export"), "save",
                               {{"format", "stl"}, {"per_object", true}},
                               "Export each object as its own STL into the folder a dialog asks for; project_export with a folder "
                               "as path writes there without one."),
        NativeCommands::action("export_drc_single", L("Export All Objects as DRC (one file)"), L("Export"), "save", {{"format", "drc"}},
                               "Export all objects as one Draco (.drc) file."),
        NativeCommands::action("export_drc_multi", L("Export All Objects as DRCs"), L("Export"), "save",
                               {{"format", "drc"}, {"per_object", true}},
                               "Export each object as its own Draco (.drc) file."),
        NativeCommands::action("export_toolpaths_obj", L("Export Toolpaths as OBJ"), L("Export"), "custom-gcode_gcode",
                               {{"format", "toolpaths_obj"}}, "Export the sliced toolpaths as an OBJ."),
    });
    catalog.add(NativeCommands::with_person_form(std::move(export_), [](const json& action_args) {
        const std::string format     = action_args.at("format");
        const bool        per_object = action_args.value("per_object", false);
        if (format == "gcode")
            return MenuItems::export_gcode();
        if (format == "sliced_3mf")
            return MenuItems::export_sliced_file(action_args.value("plates", std::string()) == "all");
        if (format == "3mf")
            return MenuItems::export_3mf();
        if (format == "stl")
            return per_object ? MenuItems::export_stl_multi() : MenuItems::export_stl();
        if (format == "drc")
            return per_object ? MenuItems::export_drc_multi() : MenuItems::export_drc_single();
        return MenuItems::export_toolpaths_obj();
    }));

    catalog.add(inspect_tool(
        "project_get_settings", L("Read the project's settings"),
        "Read the project's own settings for its plates: the plate type (`curr_bed_type`), the prime tower position per plate "
        "(`wipe_tower_x`, `wipe_tower_y`) and, with several extruders, the filament grouping (`filament_map_mode`, `filament_map`); "
        "all of them, or those named in keys. The filament list's colours, mixed filaments and flushing are `filament_list`'s. "
        "Each item has its value and meta; meta.offered lists the choices this printer has, and `meta.read_only` marks what is "
        "set elsewhere.",
        object_schema({{"keys", array_prop({{"type", "string"}}, "Setting keys, e.g. [\"curr_bed_type\"].")},
                       {"include_meta", prop("boolean", "Include labels, types and ranges. Default true.")}}),
        [](const json& args, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([&] {
                ui_plater();
                return read_project_settings(args);
            }));
        }));

    catalog.add(mutate_tool(
        "project_set_settings", L("Change the project's settings"),
        "Change the project's own settings as their controls write them: `curr_bed_type`, as the plate type selector, and "
        "wipe_tower_x/y (mm from the plate's origin, one per plate), as a prime tower's drag. The filament list's colours "
        "and flushing are set with `filament_set_colour` and `filament_set_flushing`. Values are checked before anything "
        "changes; a prime tower move is one undo step. Reply: applied, changed, and a note when the app moved a prime tower "
        "that did not fit.",
        object_schema({{"values", {{"type", "object"}, {"description", "`{key: value}`, or `{key: {index, value}}` for one entry"}}}},
                      {"values"}),
        [](const json& args, ToolContext& ctx) {
            const json values = args.value("values", json::object());
            if (values.empty())
                throw ToolFailure("values must be a non-empty object {key: value}");
            return set_project_values(values, ctx, false);
        }));
}

}}} // namespace Slic3r::GUI::Mcp

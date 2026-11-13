#include "NativeCommands.hpp"

#include "calib_dlg.hpp"
#include "Camera.hpp"
#include "CommandCatalog.hpp"
#include "CreatePresetsDialog.hpp"
#include "DailyTips.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "GUI_ObjectList.hpp"
#include "Gizmos/GLGizmoSVG.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "I18N.hpp"
#include "Shortcuts.hpp"
#include "Jobs/Job.hpp"
#include "Jobs/Worker.hpp"
#include "MainFrame.hpp"
#include "RunScriptPanel.hpp"
#include "ObjectDataViewModel.hpp"
#include "NetworkTestDialog.hpp"
#include "Notebook.hpp"
#include "Operations/ExportOps.hpp"
#include "Operations/Outcome.hpp"
#include "Operations/PlateOps.hpp"
#include "Operations/PresetOps.hpp"
#include "Operations/ProjectOps.hpp"
#include "Plater.hpp"
#include "PluginsDialog.hpp"
#include "PartPlate.hpp"
#include "PlateSettingsDialog.hpp"
#include "RecordingDialog.hpp"
#include "Preferences.hpp"
#include "SceneBenchmark.hpp"
#include "format.hpp"
#include "DeviceCore/DevManager.h"
#include "Mcp/McpPaths.hpp"
#include "Mcp/McpPersonContext.hpp"
#include "Mcp/McpRecorder.hpp"
#include "Mcp/McpService.hpp"
#include "Mcp/McpTools.hpp"
#ifdef SLIC3R_CAD
#include "CAD/DesignPanel.hpp"
#include "Mcp/McpToolsCad.hpp"
#endif
#include "Mcp/McpTypes.hpp"
#include "Mcp/RecordingSettings.hpp"
#include "Mcp/ScreenCapture.hpp"
#include "Mcp/UiInvoker.hpp"
#include "Mcp/VideoEncode.hpp"
#include "NotificationManager.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <initializer_list>
#include <libslic3r/Model.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include "libslic3r/libslic3r.h"
#include <memory>
#include <mutex>
#include "slic3r/GUI/ActionRegistry.hpp"
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include <vector>
#include <wx/app.h>
#include <wx/dataview.h>
#include <wx/filedlg.h>
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/string.h>
#include <wx/utils.h>

class wxWindow;

namespace Slic3r { namespace GUI {

using nlohmann::json;

namespace {

// Plate ops are an FFF feature: SLA has a single plate and no plate UI, gcode-only mode has no
// editable project - so gate every plate op on FFF + the normal editor.
bool is_fff_plater(Plater* plater) { return plater && plater->printer_technology() == ptFFF && !plater->only_gcode_mode(); }

AppActionRunResult plate_unavailable() { return {AppActionRunResult::Level::Info, _L("Plates are a filament (FFF) feature.")}; }

// Switch to the Prepare (3D) panel so object/calibration ops have a live canvas + selection, and
// the notebook page label matches. A no-op when the 3D panel is already shown.
void ensure_3d_view(Plater* plater)
{
    if (plater && !plater->is_view3D_shown()) {
        plater->select_view_3D("3D");
        if (MainFrame* mf = wxGetApp().mainframe; mf)
            mf->select_tab(TAB_ID_PREPARE);
    }
}

// Object op guard + run: object ops read the Prepare canvas selection, so ensure that view first so
// a launch from another tab doesn't report a spuriously empty selection.
AppActionRunResult object_op(Plater* plater, bool (*ok)(Plater*), void (*op)(Plater*))
{
    if (!plater)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first."), false};
    ensure_3d_view(plater);
    if (!ok(plater))
        return {AppActionRunResult::Level::Info, _L("Select an object first."), false};
    op(plater);
    return {AppActionRunResult::Level::Success};
}

// What a command answers when its operation refuses: the refusal in the UI's language, shown at
// `level`, as a command that did nothing; automation reads the refusal itself.
AppActionRunResult refused(const Ops::Refusal& refusal, AppActionRunResult::Level level = AppActionRunResult::Level::Error)
{
    AppActionRunResult result{level, refusal.translated(), false};
    result.refusal = std::make_shared<const Ops::Refusal>(refusal);
    return result;
}

AppActionRunResult worker_busy() { return {AppActionRunResult::Level::Info, _L("Wait for the running background job to finish."), false}; }

// Arrange as the toolbar runs it: on every unlocked plate, in the background. No objects and a running
// job each get their own message.
AppActionRunResult arrange_all(Plater* plater)
{
    if (!plater)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first."), false};
    if (const Ops::Outcome<Ops::Done> started = Ops::start_arrange({}); !started)
        return refused(started.refusal(), AppActionRunResult::Level::Info);
    return {AppActionRunResult::Level::Success};
}

// Orient as the toolbar runs it, with the guards arrange has.
AppActionRunResult orient_all(Plater* plater)
{
    if (!plater)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first."), false};
    ensure_3d_view(plater);
    if (plater->model().objects.empty())
        return {AppActionRunResult::Level::Info, _L("There are no objects to orient."), false};
    if (!plater->get_ui_job_worker().is_idle())
        return worker_busy();
    // A plate's Auto Rotate icon leaves the plate-only state behind.
    plater->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
    plater->orient();
    return {AppActionRunResult::Level::Success};
}

// How many undo and how many redo items the project's active history lists, as the canvas's Undo and Redo lists
// show them (Plater::undo_redo_string_getter()). Both are counted from the active snapshot, so they change exactly
// when it moves, which is when Plater::undo() or redo() takes back or puts back an entry of the history.
std::pair<int, int> project_history_items(Plater& plater)
{
    const auto count = [&plater](bool undo) {
        int         items = 0;
        const char* text  = nullptr;
        while (plater.undo_redo_string_getter(undo, items, &text))
            ++items;
        return items;
    };
    return {count(true), count(false)};
}

// The project's Undo, or Redo, as the Edit menu's. Plater::undo() steps back past every snapshot that does not modify
// the project to the nearest one that does and loads it; redo() steps forward the same way. Each leaves the active
// snapshot where it is when there is no such record, while a background job holds the model, and (undo) in the
// assemble view for a record that is not a gizmo's. So the history's items unchanged mean no entry was taken back or
// put back, and changed mean one was. Success is that: an entry of the history taken back or put back, as the Undo
// list shows it. It is not a promise that the project differs afterwards, since the plater classes a snapshot by its
// type, not by what it changed, and an action can leave one that changed nothing (renaming an object to the name it
// has). When no entry moved, a person's pick shows nothing, as the menu does, and an agent's call (`agent`) is
// refused.
AppActionRunResult project_undo_redo(bool redo, bool agent)
{
    Plater* const             plater = wxGetApp().plater();
    const std::pair<int, int> before = agent && plater != nullptr ? project_history_items(*plater) : std::pair<int, int>();
    if (plater != nullptr)
        redo ? plater->redo() : plater->undo();
    if (agent && (plater == nullptr || project_history_items(*plater) == before))
        throw Mcp::ToolFailure(redo ? "Nothing was redone in the project's history (nothing to put back, or a background job is running)"
                                    : "Nothing was undone in the project's history (nothing to take back, or a background job is running)");
    return {AppActionRunResult::Level::Success, {}, true, json{{"history", "project"}}.dump()};
}

// An automated call of Undo, or Redo, on the history args.history names: "design", the Design tab's, or
// "project". Without it the history is the shown tab's, as the Edit menu's Undo and Redo choose it. The reply says
// which history it was and, for the Design tab's, what Mcp::cad_undo_redo() replies. A refusal is thrown as
// Mcp::ToolFailure.
AppActionRunResult undo_redo(const json& args, bool redo)
{
    std::string history = args.value("history", std::string());
    if (history.empty()) {
        history = "project";
#ifdef SLIC3R_CAD
        if (MainFrame* mf = wxGetApp().mainframe; mf != nullptr && mf->shown_design_panel() != nullptr)
            history = "design";
#endif
    }
    if (history == "design") {
#ifdef SLIC3R_CAD
        json reply{{"history", "design"}, {"result", Mcp::cad_undo_redo(redo)}};
        return {AppActionRunResult::Level::Success, {}, true, reply.dump()};
#else
        throw Mcp::ToolFailure("The CAD feature is off: this build of OrcaSlicer has no Design tab");
#endif
    }
    return project_undo_redo(redo, /*agent=*/true);
}

// A person's pick of Undo, or Redo: what the Edit menu's item does, on the history args.history names or else the
// shown tab's, through the function the menu calls. It shows nothing: where the menu's item would be greyed it does
// nothing, and otherwise the Design tab's status line or the canvas shows what changed, as after the menu's.
AppActionRunResult picked_undo_redo(const json& args, bool redo)
{
    const std::string history = args.value("history", std::string());
    // What a pick that does nothing shows: nothing.
    const AppActionRunResult greyed{AppActionRunResult::Level::Info, {}, false};
#ifdef SLIC3R_CAD
    DesignPanel* design = nullptr;
    if (history.empty()) {
        if (MainFrame* mf = wxGetApp().mainframe)
            design = mf->shown_design_panel();
    } else if (history == "design")
        design = DesignPanel::if_built();
    if (design != nullptr || history == "design") {
        if (design == nullptr || !design->menu_can_undo_redo(redo))
            return greyed;
        design->menu_undo_redo(redo);
        return {AppActionRunResult::Level::Success, {}, true, json{{"history", "design"}}.dump()};
    }
#else
    // A build without CAD has no Design tab, so no history of it to take back.
    if (history == "design")
        return greyed;
#endif
    return project_undo_redo(redo, /*agent=*/false);
}

// The object list, once the Prepare view is shown; the object structure commands act on its selected
// rows, as the context menu items they stand for do.
ObjectList* object_list_in_3d_view()
{
    Plater*     plater = wxGetApp().plater();
    ObjectList* list   = wxGetApp().obj_list();
    if (plater == nullptr || list == nullptr)
        return nullptr;
    ensure_3d_view(plater);
    return list;
}

// The object list's selected rows, or none unless every one is of one of `types` (ItemType bits).
wxDataViewItemArray selected_rows(const ObjectList& list, int types)
{
    wxDataViewItemArray rows;
    list.GetSelections(rows);
    for (const wxDataViewItem& row : rows)
        if ((list.GetModel()->GetItemType(row) & types) == 0)
            return {};
    return rows;
}

AppActionRunResult with_reply(const nlohmann::json& reply)
{
    AppActionRunResult r{AppActionRunResult::Level::Success};
    r.reply = reply.dump();
    return r;
}

// What an Order By left: the sorted plates' objects in their new order, and whether they run
// descending (choosing the order the objects already have reverses it).
AppActionRunResult ordered(Plater& plater, ObjectOrder order, bool all_plates)
{
    PartPlateList&         plates  = plater.get_partplate_list();
    const ModelObjectPtrs& objects = plater.model().objects;
    nlohmann::json         list    = nlohmann::json::array();
    bool                   descending = false;
    for (int plate = 0; plate < plates.get_plate_count(); ++plate) {
        if (!all_plates && plate != plates.get_curr_plate_index())
            continue;
        std::vector<ModelObject*> on_plate;
        for (size_t i = 0; i < objects.size(); ++i)
            if (plates.find_instance(int(i), 0) == plate) {
                on_plate.push_back(objects[i]);
                list.push_back({{"object", i}, {"name", objects[i]->name}, {"plate", plate}});
            }
        std::vector<ModelObject*> ascending = on_plate;
        sort_objects(ascending, order, false);
        descending = descending || ascending != on_plate;
    }
    return with_reply({{"order", list}, {"descending", descending}});
}

// Jumps the Preview to the layer a 0-100 percent of its layers stands for. The percent moves the top
// of the shown range; its bottom stays. The caller has already switched to the Preview.
AppActionRunResult go_to_layer(double pct)
{
    pct = std::clamp(pct, 0.0, 100.0);

    const Ops::Outcome<int> count = Ops::layer_count();
    if (!count)
        return refused(count.refusal(), AppActionRunResult::Level::Info);
    Ops::ShowLayerRangeArgs range;
    range.last = int(std::lround(pct / 100.0 * double(count.value() - 1))) + 1;
    if (const Ops::Outcome<Ops::ShownLayers> shown = Ops::show_layer_range(range); !shown)
        return refused(shown.refusal(), AppActionRunResult::Level::Info);
    return {AppActionRunResult::Level::Success};
}

// The file `args` names ({"path", "overwrite"?}), checked for reading (`output` false) or for writing, where
// a file that exists is replaced only with overwrite. nullopt with an Error in `fail` when the path is
// missing or unusable.
std::optional<boost::filesystem::path> path_argument(const json& args, bool output, AppActionRunResult& fail)
{
    std::string                            err = "path is required";
    std::optional<boost::filesystem::path> path;
    if (const std::string given = args.value("path", std::string()); !given.empty())
        path = output ? Mcp::resolve_output_path(given, args.value("overwrite", false), err) : Mcp::resolve_input_path(given, err);
    if (!path)
        fail = {AppActionRunResult::Level::Error, from_u8(err)};
    return path;
}

// The existing directory `args` names ({"path"}), for an export of several files. nullopt with an Error in
// `fail` when the path is missing or no directory.
std::optional<boost::filesystem::path> dir_argument(const json& args, AppActionRunResult& fail)
{
    std::string                            err = "path is required";
    std::optional<boost::filesystem::path> dir;
    if (const std::string given = args.value("path", std::string()); !given.empty())
        dir = Mcp::resolve_output_dir(given, err);
    if (!dir)
        fail = {AppActionRunResult::Level::Error, from_u8(err)};
    return dir;
}

// The plater of the running app, or nullptr: before the main window is up, and in a unit test, which has no
// GUI_App.
Plater* app_plater()
{
    auto* app = dynamic_cast<GUI_App*>(wxTheApp);
    return app != nullptr ? app->plater() : nullptr;
}

const char* level_name(AppActionRunResult::Level level)
{
    switch (level) {
    case AppActionRunResult::Level::Success: return "success";
    case AppActionRunResult::Level::Error: return "error";
    case AppActionRunResult::Level::Busy: return "busy";
    default: return "info";
    }
}

// What a command that wrote `path` answers, or its operation's refusal.
AppActionRunResult written(const Ops::Outcome<Ops::Done>& outcome, const boost::filesystem::path& path)
{
    return outcome ? AppActionRunResult{AppActionRunResult::Level::Success, from_u8(path.string())} : refused(outcome.refusal());
}

// A screenshot or recording saved into the MCP output folder, which a person would not find by themselves: a
// notification names the file, with a button that opens its folder.
void notify_saved(const wxString& text, const boost::filesystem::path& path)
{
    if (Plater* plater = wxGetApp().plater(); plater != nullptr && !wxGetApp().is_closing())
        plater->get_notification_manager()->push_file_saved_notification(into_u8(text), Mcp::path_to_utf8(path),
                                                                         Mcp::path_to_utf8(path.parent_path()));
}

// For automation: the file a command saved, which its notification shows the person.
std::string path_reply(const boost::filesystem::path& path) { return nlohmann::json{{"path", Mcp::path_to_utf8(path)}}.dump(); }

// Why a recording ended when Stop Recording ended it; an agent's stop_recording reports it.
constexpr const char* kStoppedInApp = "stopped_in_app";

void notify_error(const wxString& text)
{
    if (Plater* plater = wxGetApp().plater(); plater != nullptr && !wxGetApp().is_closing())
        plater->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                                                               NotificationManager::NotificationLevel::ErrorNotificationLevel, into_u8(text));
}

// Recordings are the MCP service's: the one a person's pick reaches (Mcp::PersonContext::service()), which with the
// server off is started without its endpoint. nullptr when it cannot start, as while OrcaSlicer closes.
Mcp::McpService* recording_service()
{
    try {
        return &Mcp::PersonContext().service();
    } catch (const Mcp::ToolFailure&) {
        return nullptr;
    }
}

// Encodes the recording at `path` to an MP4 beside it once its file is closed, and tells the person how that
// went. False when it cannot start: OrcaSlicer is closing.
bool encode_when_closed(const boost::filesystem::path& path, const Mcp::RecordingSettings& settings)
{
    Mcp::McpService* service = wxGetApp().mcp_service();
    if (service == nullptr || wxGetApp().is_closing())
        return false;
    return service->encode_when_closed(Mcp::encode_arguments(settings, Mcp::path_to_utf8(path)), [](const Mcp::ToolResult& r) {
        if (r.is_error)
            // TRN %1% is why, in English.
            notify_error(format_wxstr(_L("The recording was saved, but could not be encoded: %1%"), r.message));
        else
            notify_saved(_L("Recording encoded"), Mcp::path_from_utf8(r.data.value("path", std::string())));
    });
}

} // namespace

// The dialog shows the remembered settings and remembers what it starts with. Encoding needs an ffmpeg: the
// dialog says so, and the recording is then not encoded. Neither a run's end nor the server turning off ends
// the recording. Whatever does end it says where it went: Stop Recording itself, or the recording's `ended` for
// an agent's stop_recording, the limit or a write error; with encode, `ended` also encodes it.
AppActionRunResult NativeCommands::start_person_recording()
{
    if (recording_active())
        return {AppActionRunResult::Level::Info, _L("A recording is already running."), false};
    AppConfig&             config   = *wxGetApp().app_config;
    Mcp::RecordingSettings settings = Mcp::load_recording_settings(config);
    std::string            ffmpeg_err;
    const bool             has_ffmpeg = Mcp::find_ffmpeg(ffmpeg_err).has_value();
    RecordingDialog        dialog(wxGetApp().mainframe, settings);
    if (dialog.ShowModal() != wxID_OK)
        return {AppActionRunResult::Level::Info, _L("Recording cancelled."), false};
    settings = dialog.settings();
    Mcp::save_recording_settings(config, settings);
    config.save();

    Mcp::McpService* service = recording_service();
    if (service == nullptr)
        return {};
    // The switch is remembered as the person left it; without an ffmpeg this recording is not encoded.
    settings.encode                = settings.encode && has_ffmpeg;
    Mcp::McpRecorder&     recorder = service->recorder();
    Mcp::RecordingOptions options;
    options.quality     = settings.quality;
    options.captions    = settings.captions;
    options.pointer     = settings.pointer;
    options.max_seconds = 3600;
    options.path        = Mcp::timestamped_path(capture_folder(settings), "recording", ".mkv", {".mp4"});
    options.owner       = Mcp::kPersonOwner;
    options.ended       = [path = options.path, settings](const std::string& reason) {
        if (settings.encode && encode_when_closed(path, settings))
            notify_saved(_L("Recording saved. Encoding it to MP4..."), path);
        else if (reason != kStoppedInApp)
            notify_saved(_L("Recording saved"), path);
    };
    const std::shared_ptr<Mcp::RecordingFiles>& files = recorder.files();
    const uint64_t                              owner = files->new_owner();
    if (const std::string why = files->reserve(owner, {{options.path, Mcp::RecordingFiles::Use::Recording}}); !why.empty())
        // TRN %1% is why, in English.
        return {AppActionRunResult::Level::Error, format_wxstr(_L("The recording could not start: %1%"), why), false};
    try {
        recorder.start(options, owner); // owns the reservation from here, whether it starts or not
    } catch (const std::exception& e) {
        return {AppActionRunResult::Level::Error, format_wxstr(_L("The recording could not start: %1%"), e.what()), false};
    }
    nlohmann::json reply = Mcp::encode_arguments(settings, Mcp::path_to_utf8(options.path));
    reply["quality"]     = settings.quality;
    reply["captions"]    = settings.captions;
    reply["pointer"]     = settings.pointer;
    reply["encode"]      = settings.encode;
    // TRN "Stop Recording" is the command that ends the recording.
    return {AppActionRunResult::Level::Success, _L("Recording started. Stop Recording saves it."), true, reply.dump()};
}

AppActionRunResult NativeCommands::stop_recording_in_app()
{
    Mcp::McpService*     service = wxGetApp().mcp_service();
    const nlohmann::json state   = service != nullptr && service->started() ? service->recorder().state() : nlohmann::json();
    if (state.is_null() || state.value("ending", false))
        return AppActionRunResult{AppActionRunResult::Level::Info, _L("No recording is running."), false};
    // What the window shows now, even if the timer has not ticked since, before the notification shows.
    service->recorder().capture_now();
    // The recording may be an agent's, a replay's or a script's; the person stopped it all the same, so they are told
    // where it went. Before it ends: a person's recording that is to be encoded says so instead, as it ends.
    const boost::filesystem::path path = Mcp::path_from_utf8(state["path"].get<std::string>());
    notify_saved(_L("Recording saved"), path);
    service->recorder().end(kStoppedInApp);
    return AppActionRunResult{AppActionRunResult::Level::Success, {}, true, path_reply(path)};
}

namespace {

// Select a named camera view. Plater::select_view dispatches to the current panel.
AppActionRunResult view_command(Plater* plater, const std::string& dir)
{
    if (plater)
        plater->select_view(dir);
    return {AppActionRunResult::Level::Success};
}

// Calibration wizards. Routes through MainFrame::run_calibration, the same entry point as the
// Calibration menu (which caches most of the wizard dialogs).
AppActionRunResult calib_command(CalibKind kind)
{
    MainFrame* mf = wxGetApp().mainframe;
    if (!mf)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first.")};
    ensure_3d_view(wxGetApp().plater());
    mf->run_calibration(kind);
    return {AppActionRunResult::Level::Success};
}

// Imports the preset file at `path` (Ops::import_presets()) and words what came of it: the message lists
// the presets imported, replaced and kept.
AppActionRunResult import_preset_file(const boost::filesystem::path& path, bool overwrite)
{
    Ops::ImportPresetsArgs presets;
    presets.path      = path;
    presets.overwrite = overwrite;
    const Ops::Outcome<Ops::ImportedPresets> imported = Ops::import_presets(presets);
    if (!imported)
        return refused(imported.refusal());

    wxString   message;
    const auto add = [&message](const wxString& text, const std::vector<std::string>& list) {
        if (!list.empty())
            message += (message.empty() ? "" : " ") + format_wxstr(text, boost::algorithm::join(list, ", "));
    };
    // TRN %1% is a comma-separated list of preset names.
    add(_L("Imported presets: %1%."), imported.value().imported);
    // TRN %1% is a comma-separated list of preset names.
    add(_L("Replaced presets: %1%."), imported.value().replaced);
    // TRN %1% is a comma-separated list of preset names.
    add(_L("Kept the existing presets: %1%."), imported.value().kept);
    return {AppActionRunResult::Level::Success, message};
}

// Thin AppAction wrapper for one catalog entry: identity and presentation come from the catalog,
// run() routes back to it. The id is keyed by the stable catalog key (not the display title), so a
// rename or a UI-language switch never re-keys the action.
struct CommandAction : AppAction
{
    AppActionRunResult run(const json& args) const override { return NativeCommands::run(command, args); }

    // `title` is the command's title in the UI's language.
    CommandAction(const NativeCommand& c, std::string title)
        : AppAction(AppActionId{AppAction::compose_id(kCommandPrefix, c.key, kOrcaSourceKey)}, std::move(title), kOrcaSourceKey,
                    kOrcaSourceName)
    {
        this->kind     = AppActionKind::Command;
        this->group    = I18N::translate_utf8(c.group);
        this->title_en = c.title;
        this->group_en = c.group;
        this->input    = c.input;
        this->icon     = c.icon;
        this->automation_description  = c.description;
        this->input_schema            = c.input_schema;
        this->command                 = c.key;
        this->completes_in_background = c.completion == CommandCompletion::Export;
    }
};

// A description of each command for agents (English, never shown in the palette): what it does, what its
// arguments stand for and what it replies.
void describe_commands(std::vector<NativeCommand>& commands)
{
    struct Detail
    {
        const char* key;
        const char* description;
    };
    static const Detail details[] = {
        {"slice_and_preview", "Slice the current plate and show the Preview tab. Returns at once; use the slice tool to wait for the result."},
        {"slice_all", "Slice every plate and show the Preview tab. Returns at once; use the slice tool to wait for results."},
        {"go_to_layer", "Show the Preview tab at a layer chosen as a percentage of the layer range; an error while the Preview has no layers (slice first)."},
        {"go_to_tab", "Switch to a main window tab."},
        {"load_project", "Open a project. With a path, opens that file without a dialog (asks first if the current project has unsaved changes)."},
        {"save_project_as", "Save the project as a .3mf file. With a path, saves there without a dialog; a name ending in .gcode.3mf is refused (export_sliced_file writes that)."},
        {"undo", "Take back the last change of a history, as Edit > Undo does. history design is the Design tab's: the last "
                 "feature added, edited, deleted, moved, shown or hidden, or renamed, or variable changed, or in a sketch session "
                 "the last drawn shape; project is the project's. Without history, the Design tab's while it is shown, else the "
                 "project's. Reply: history and, for the Design tab's, result: scope (design or sketch), undo_depth, redo_depth, "
                 "features, bodies and generation; entities in a sketch."},
        {"redo", "Put back what undo took back, until the next change, as Edit > Redo does, in the history undo takes. Reply "
                 "as undo's."},
        {"open_action_replay", "Open the Action Replay window, where a person opens a replay file (an orca_replay), reads its steps "
                               "and replays them. Agents run replays with the run_replay tool instead."},
        {"toggle_run_script", "Show or hide the Run Script panel, where a person writes and runs Python scripts cell by cell. Agents "
                              "run Python with the run_script tool instead."},
        {"take_screenshot", "Save a PNG of the main window in the folder Start Recording saves in (the person's Downloads unless "
                            "they chose another) and show the person a notification naming it; the reply has its path. Agents use "
                            "the screenshot tool, which returns the image."},
        {"open_preferences", "Open the Preferences dialog on a tab, scrolled to one preference when given (shown on its own tab). Its "
                             "controls are named after their preference keys: set_widget_value {\"target\": {\"name\": key, \"within\": "
                             "\"Preferences\"}} changes one through the dialog; get_settings scope preferences reads them."},
        {"mode_simple", "Switch the settings view to Simple mode."},
        {"mode_advanced", "Switch the settings view to Advanced mode."},
        {"mode_expert", "Switch the settings view to Expert mode."},
        {"toggle_developer_mode", "Turn Developer mode on or off."},
        {"export_gcode", "Export the current plate's G-code. With a path, exports there without a dialog once the plate is sliced, in the "
                         "background, and the call returns when the file is written; with no arguments, it opens the export dialog."},
        {"export_stl", "Export all objects as one STL. With a path, writes there without a dialog."},
        {"export_3mf", "Export the model as a .3mf without slicing data. With a path, writes there without a dialog."},
        {"export_sliced_file", "Export the current plate's sliced file (.gcode.3mf). With a path, writes there without a dialog."},
        {"export_all_sliced_file", "Export every plate's sliced file into one .gcode.3mf. With a path, writes there without a dialog."},
        {"send_gcode", "Open the Send G-code to printer host dialog for the current plate, as the Print button does for a printer with a print host (OctoPrint, Moonraker, PrusaLink, Duet...). Set the file name with set_widget_value and press Upload or Upload and Print with answer_dialog; the plate is sliced if needed, then uploaded in the background: get_app_state.uploads shows the row's state, and a failed upload also opens an error dialog. A replay uploads again. Fails for a printer without a print host, or one that prints through the Bambu network or printer agents."},
        {"export_stl_multi", "Export each object as its own STL. With a directory, writes there without a dialog."},
        {"export_drc_single", "Export all objects as one Draco (.drc) file."},
        {"export_drc_multi", "Export each object as its own Draco (.drc) file."},
        {"export_toolpaths_obj", "Export the sliced toolpaths as an OBJ."},
        {"export_config", "Export presets as a bundle or ZIP archives. Opens a dialog: click the label of the kind to export, check "
                          "its rows (each named after a printer preset or a filament), press OK, then answer the result message. "
                          "With a directory, OK writes there instead of asking for a folder."},
        {"import_file", "Import models (3MF/STL/STEP/SVG/OBJ/AMF). With a path, adds that file's geometry without a dialog, as one undo step; "
                        "a project file's settings are not loaded (load_project opens a project)."},
        {"import_zip_archive", "Import models from a ZIP archive (opens a file dialog)."},
        {"import_configs", "Import presets. With a path, imports a preset .json, a .zip of them or an .orca_printer, "
                           ".orca_filament or .orca_bundle without a dialog, and the message names the presets imported, "
                           "replaced and kept; a .gcode loads the settings it was sliced with."},
        {"view_default", "Reset the 3D camera to the default view and fit the plate."},
        {"view_fit_bed", "Fit the build plate in the 3D view."},
        {"view_toggle_perspective", "Switch the 3D camera between perspective and orthographic."},
        {"reset_window_layout", "Restore the default window layout."},
        {"obj_delete", "Delete the selected objects or parts."},
        {"obj_delete_all", "Delete all objects."},
        {"select_all", "Select all objects in the 3D view."},
        {"deselect_all", "Clear the selection."},
        {"obj_mirror_x", "Mirror the selection along X."},
        {"obj_mirror_y", "Mirror the selection along Y."},
        {"obj_mirror_z", "Mirror the selection along Z."},
        {"obj_split_objects", "Split the selected object into separate objects."},
        {"obj_split_parts", "Split the selected object into parts."},
        {"obj_center", "Center the selection on its plate."},
        {"obj_drop", "Drop the selection onto the bed."},
        {"obj_fit_volume", "Scale the selection to fit the print volume."},
        {"obj_instances_up", "Add an instance of the selected object."},
        {"obj_instances_down", "Remove an instance of the selected object."},
        {"obj_arrange", "Arrange all objects on the unlocked plates with the saved Arrange options, as the toolbar does; the "
                        "call returns when the arrangement is done. The arrange tool also takes options, one plate or fill bed."},
        {"obj_orient", "Auto-orient the selection, or every object when nothing is selected; the call returns when it is done."},
        // Object structure: the object list's Change Type, Rename, instance and Assemble items, and the plate
        // menu's Order By. Arguments stand in for the item's dialog.
        {"obj_type_part", "Make the selected parts (select_objects {parts} first) solid parts, as Change Type does. Parts are listed by type, so part indices change."},
        {"obj_type_negative", "Make the selected parts negative parts, as Change Type does; not the object's last solid part. Parts are listed by type, so part indices change."},
        {"obj_type_modifier", "Make the selected parts modifiers, as Change Type does; not the object's last solid part. Parts are listed by type, so part indices change."},
        {"obj_type_support_blocker", "Make the selected parts support blockers, as Change Type does; not the object's last solid part, nor a text or SVG part. Parts are listed by type, so part indices change."},
        {"obj_type_support_enforcer", "Make the selected parts support enforcers, as Change Type does; not the object's last solid part, nor a text or SVG part. Parts are listed by type, so part indices change."},
        {"obj_type_precise_seam", "Make the selected parts Center precise seams (seams keep their subtype), as Change Type does; not the object's last solid part, nor a text or SVG part. Parts are listed by type, so part indices change."},
        {"obj_rename", "Rename the selected object or part (select one). With a name, skips the Rename dialog; an empty name, or one with characters a file name cannot hold, is refused."},
        {"obj_set_instances", "Give the selected object this many instances, in one undo step. With a count, skips the dialog. New instances overlap; arrange afterwards. The reply has object and instances."},
        {"obj_assemble", "Assemble two or more selected objects into one object of parts named Assembly. They are removed and it is added last, so object indices change; the reply has its index (object) and name."},
        {"obj_instances_to_objects", "Make each instance of the selected object a separate object (select_objects {objects: [n]}); with some of its instances selected, only those. New objects are added last; the reply lists the objects the instances are in now (objects)."},
        {"obj_order_by_name", "Order the current plate's objects by name, which sets the object list and print order, as the plate menu's Order By does. Choosing the order they already have reverses it. Object indices change; the reply lists the new order (order: [{object, name, plate}]) and descending."},
        {"obj_order_by_filament", "Order the current plate's objects by filament, which sets the object list and print order, as the plate menu's Order By does. Choosing the order they already have reverses it. Object indices change; the reply lists the new order (order: [{object, name, plate}]) and descending."},
        {"obj_order_by_height", "Order the current plate's objects by height, which sets the object list and print order, as the plate menu's Order By does. Choosing the order they already have reverses it. Object indices change; the reply lists the new order (order: [{object, name, plate}]) and descending."},
        {"obj_order_by_size", "Order the current plate's objects by size (bounding box volume), which sets the object list and print order, as the plate menu's Order By does. Choosing the order they already have reverses it. Object indices change; the reply lists the new order (order: [{object, name, plate}]) and descending."},
        {"add_primitive_svg", "Add an SVG as a new object, or as a part of the selected object. With a path, uses that file without a dialog; the part is created in the background (wait for idle), and the SVG gizmo opens on it."},
        // Shapes: a size skips the Primitive size dialog.
        {"add_primitive_cube", "Add a cube object to the current plate; fails while an object is selected. With a size, skips the size dialog."},
        {"add_primitive_cylinder", "Add a cylinder object to the current plate; fails while an object is selected. With a size, skips the size dialog. The shape may then be turned toward the part-cooling fan, so the size is along its own axes before that turn."},
        {"add_primitive_sphere", "Add a sphere object to the current plate; fails while an object is selected. With a size, skips the size dialog."},
        {"add_primitive_cone", "Add a cone object to the current plate; fails while an object is selected. With a size, skips the size dialog. The shape may then be turned toward the part-cooling fan, so the size is along its own axes before that turn."},
        {"add_primitive_disc", "Add a disc object to the current plate; fails while an object is selected. With a size, skips the size dialog. The shape may then be turned toward the part-cooling fan, so the size is along its own axes before that turn."},
        {"add_primitive_torus", "Add a torus object to the current plate; fails while an object is selected. With a size, skips the size dialog. The shape may then be turned toward the part-cooling fan, so the size is along its own axes before that turn."},
        {"plate_add", "Add a plate."},
        {"plate_duplicate", "Duplicate the current plate."},
        {"plate_delete", "Delete the current plate."},
        {"plate_rename", "Rename the current plate (opens a dialog)."},
        {"plate_toggle_lock", "Lock or unlock the current plate."},
        {"plate_goto", "Make a plate the current one; an index the project lacks is an error."},
        {"sync_ams", "Synchronize the filament list from the connected printer's AMS."},
        {"preset_bundle", "Open the preset bundle manager."},
        {"sync_presets", "Synchronize user presets with the cloud account."},
        {"help_keyboard_shortcuts", "Show the keyboard shortcuts dialog."},
        {"help_setup_wizard", "Open the setup wizard."},
        {"help_open_config_folder", "Open the configuration folder in the file manager."},
        {"help_migrate_filament_presets", "Migrate legacy user filament presets."},
        {"help_troubleshoot", "Open the troubleshooting center."},
        {"help_network_test", "Open the network test dialog."},
        {"help_benchmark_3d_scene", "Benchmark rendering of the current 3D scene."},
        {"help_tip_of_the_day", "Show a tip of the day."},
        {"help_check_updates", "Check for a newer OrcaSlicer."},
        {"help_about", "Show the About dialog."},
        {"open_wiki", "Open the OrcaSlicer wiki in the browser."},
        {"open_youtube", "Open the OrcaSlicer YouTube channel in the browser."},
        {"open_plugins", "Open the plugins dialog."},
        {"refresh_plugins", "Reload plugins."},
        {"install_plugin", "Open the plugin hub to install a plugin."},
        {"install_local_plugin", "Install a plugin from a .py or .whl file, as the Plugins dialog's Install local plugin does. With a "
                                 "path, installs that file without a dialog. Replacing a plugin installed already asks first "
                                 "(answer with answer_dialog); the message names the plugin installed, or why it was not."},
        // Filament slots
        {"filament_add", "Add a filament slot, as the + button above the filament list does; the message names the new slot. It "
                         "takes the next colour and the last slot's preset (change them with select_preset filament_slot and "
                         "set_settings scope project filament_colour). It goes after the physical slots: mixed filaments move "
                         "up by one, and objects on them follow."},
        {"filament_delete", "Delete a filament slot, as its Delete menu item does; objects on it move to another slot. A mixed "
                            "filament (project scope filament_is_mixed) is refused."},
    };
    // The operations (GUI/Operations) a command reaches once it has what it needs to run without asking.
    static const std::pair<const char*, std::vector<std::string>> reaches[] = {
        {"slice_and_preview", {"start_slice"}},
        {"slice_all", {"start_slice"}},
        {"go_to_layer", {"show_layer_range"}},
        {"save_project_as", {"save_project_as"}},
        {"export_gcode", {"start_gcode_export"}},
        {"export_stl", {"export_stl"}},
        {"export_3mf", {"export_model_3mf"}},
        {"export_sliced_file", {"export_sliced_3mf"}},
        {"export_all_sliced_file", {"export_sliced_3mf"}},
        {"send_gcode", {"send_gcode_to_host"}},
        {"import_file", {"import_models"}},
        {"import_configs", {"import_presets"}},
        {"obj_arrange", {"start_arrange"}},
        {"obj_set_instances", {"set_instance_count"}},
        {"plate_toggle_lock", {"set_plate_locked"}},
        {"plate_goto", {"select_plate"}},
    };
    for (NativeCommand& c : commands) {
        if (const auto ops = std::find_if(std::begin(reaches), std::end(reaches), [&](const auto& r) { return c.key == r.first; });
            ops != std::end(reaches))
            c.operations = ops->second;
        auto it = std::find_if(std::begin(details), std::end(details), [&](const Detail& d) { return c.key == d.key; });
        if (it != std::end(details)) {
            c.description = it->description;
        } else if (c.key.rfind("calib_", 0) == 0) {
            c.description = "Open the " + c.title +
                            " dialog; the generate_calibration_test tool runs the same test with typed parameters and no dialog.";
        } else if (c.key.rfind("view_", 0) == 0) {
            c.description = "Point the 3D camera: " + c.title + ".";
        } else if (c.key.rfind("add_primitive_", 0) == 0 || c.key.rfind("add_handy_", 0) == 0) {
            c.description = "Add a " + c.title + " object to the current plate.";
        }
    }
}

} // namespace

void NativeCommands::register_all_commands(CommandCatalog& catalog, [[maybe_unused]] const CatalogOptions& options)
{
    // The Speed Dial's commands: each has a tile.
    std::vector<NativeCommand> out;
    // Adds `command` with a tile. The reference is valid until the next command is added: long enough to say
    // what arguments the command takes, as in add(...).input_schema = Mcp::object_schema({...}).
    const auto tile = [&out](NativeCommand command) -> NativeCommand& {
        command.in_palette = true;
        out.push_back(std::move(command));
        return out.back();
    };
    // `title` and `group` are the untranslated English msgids, L("...")-marked at each call so xgettext
    // extracts them. A command takes no arguments unless its schema is given afterwards; with none, a
    // dialog command asks as a person's pick does.
    using Body = std::function<AppActionRunResult(const json& args)>;
    auto add   = [&tile](std::string key, std::string title, std::string group, Body fn, std::string input = {},
                       std::string icon = {}) -> NativeCommand& {
        NativeCommand& c = tile(ui_command(std::move(key), std::move(title), std::move(group), Mcp::object_schema({}), std::move(fn)));
        c.input          = std::move(input);
        c.icon           = std::move(icon);
        return c;
    };
    // Presentation-first overload: keeps the tile icon next to the title/group it belongs to.
    auto add_with_icon = [&add](std::string key, std::string title, std::string group, std::string icon, Body fn,
                                std::string input = {}) -> NativeCommand& {
        return add(std::move(key), std::move(title), std::move(group), std::move(fn), std::move(input), std::move(icon));
    };
    // The argument of a command that opens or imports a file, in place of its file dialog.
    const auto input_file = [](const char* what) { return Mcp::prop("string", std::string("Absolute path of ") + what + "."); };
    // The arguments of a command that writes a file, in place of its save dialog.
    const auto output_file = [](const char* what) {
        return Mcp::object_schema({{"path", Mcp::prop("string", std::string("Absolute path of the ") + what + " to write.")},
                                   {"overwrite", Mcp::prop("boolean", "Replace a file that exists already. Default false.")}});
    };

    // ---- Slice & Export ----
    // Slices through the toolbar's path, so it counts as a user slice request (Plater::request_slice).
    add_with_icon("slice_and_preview", L("Slice and Preview"), L("Slice & Export"), "media_play", [](const json&) {
        if (wxGetApp().plater() != nullptr)
            Ops::start_slice({});
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("slice_all", L("Slice All Plates"), L("Slice & Export"), "media_play", [](const json&) {
        if (wxGetApp().plater() != nullptr) {
            Ops::SliceArgs slice;
            slice.all_plates = true;
            Ops::start_slice(slice);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // Go to layer and Go to tab have one form, which needs its value: the palette collects it in its second
    // phase (`input`) and passes it as their argument.
    add_with_icon(
        "go_to_layer", L("Go to layer (percent)"), L("Commands"), "height_range_layer",
        [](const json& args) {
            Plater* plater = wxGetApp().plater();
            if (plater == nullptr)
                return AppActionRunResult{AppActionRunResult::Level::Success};
            plater->select_view_3D("Preview", false);
            if (MainFrame* mf = wxGetApp().mainframe; mf)
                mf->select_tab(TAB_ID_PREVIEW);
            return go_to_layer(args.at("percent").get<double>());
        },
        "percent")
        .input_schema = Mcp::object_schema({{"percent", {{"type", "number"},
                                                         {"minimum", 0},
                                                         {"maximum", 100},
                                                         {"description", "The layer to show, as a percentage of the layer range."}}}},
                                           {"percent"});
    add(
        "go_to_tab", L("Go to tab..."), L("Commands"),
        [](const json& args) {
            MainFrame* mf = wxGetApp().mainframe;
            if (mf == nullptr)
                return AppActionRunResult{};
            const std::string tab = args.at("tab").get<std::string>();
            // select_tab() shows the last tab for an empty id, and leaves the tab as it is for an id no page has.
            if (mf->m_tabpanel == nullptr || mf->m_tabpanel->FindPageByName(from_u8(tab)) == wxNOT_FOUND)
                // TRN %1% is the tab id the command was given, such as "preview".
                return AppActionRunResult{AppActionRunResult::Level::Error, format_wxstr(_L("No tab has the id \"%1%\"."), tab), false};
            mf->select_tab(from_u8(tab));
            // The tab can refuse to open: the Device tab of a Bambu Lab printer without its network plugin.
            if (mf->m_tabpanel->GetSelectedPageName() != from_u8(tab))
                // TRN %1% is the id of the tab that could not be opened, such as "monitor".
                return AppActionRunResult{AppActionRunResult::Level::Error, format_wxstr(_L("The \"%1%\" tab could not be opened."), tab), false};
            return AppActionRunResult{AppActionRunResult::Level::Success};
        },
        "tab")
        .input_schema = Mcp::object_schema(
        {{"tab", Mcp::prop("string", "The tab's id: home, prepare, preview, monitor, project, calibration, or a plugin page's id.")}},
        {"tab"});

    add_with_icon("load_project", L("Load Project"), L("Commands"), "menu_open", [](const json& args) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (Mcp::person_form(args)) {
            plater->load_project();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_argument(args, false, fail);
        if (!path)
            return fail;
        plater->load_project(from_path(*path));
        return AppActionRunResult{AppActionRunResult::Level::Success};
    }).input_schema = Mcp::object_schema({{"path", input_file("a .3mf project")}});
    add_with_icon("save_project_as", L("Save Project As"), L("Commands"), "menu_save", [](const json& args) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (Mcp::person_form(args)) {
            plater->save_project(true);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_argument(args, true, fail);
        if (!path)
            return fail;
        return written(Ops::save_project_as({*path}), *path);
    }).input_schema = output_file(".3mf project");
    // Undo and Redo act on the shown tab's history, as the Edit menu's do; history names one for automation,
    // which acts whatever tab is shown.
    const json history = Mcp::object_schema(
        {{"history", Mcp::enum_prop({"design", "project"}, "The history: design, the Design tab's (with the CAD feature on), "
                                                           "or project. Default: the Design tab's while it is shown, else "
                                                           "the project's.")}});
    // A person's pick does what the Edit menu's item does, and shows nothing where it would be greyed. An
    // automated call, run_action of the tile included, acts on the history undo_redo() picks: the Design tab's
    // through its gate, whose refusals say why, and the project's as the Edit menu's item does.
    const auto picked_as_menu = [](NativeCommand& command, bool redo) {
        const CommandFn picked = ui_command(command.key, command.title, command.group, command.input_schema,
                                            [redo](const json& args) { return picked_undo_redo(args, redo); })
                                     .run;
        command.run = [picked, automated = std::move(command.run)](const json& args, Mcp::ToolContext& ctx) {
            return ctx.person() ? picked(args, ctx) : automated(args, ctx);
        };
    };
    NativeCommand& undo = add_with_icon("undo", L("Undo"), L("Commands"), "menu_undo", [](const json& args) { return undo_redo(args, false); });
    undo.input_schema   = history;
    picked_as_menu(undo, false);
#ifdef SLIC3R_CAD
    // An example calls Design commands, so only a catalog that holds them documents the Design history.
    if (options.cad)
        undo.examples = json::parse(R"json([
            {"title": "Take back an extrude, its sketch with it", "calls": [
                {"tool": "extrude", "args": {"width": 20, "height": 20, "distance": 5}},
                {"tool": "undo", "args": {"history": "design"}}]},
            {"title": "Take back the last shape drawn in a sketch", "calls": [
                {"tool": "sketch_begin", "args": {"plane": "XY"}},
                {"tool": "sketch_add", "args": {"rect": [-15, -10, 15, 10]}},
                {"tool": "sketch_add", "args": {"entities": [{"type": "circle", "center": [0, 0], "radius": 4}]}},
                {"tool": "undo", "args": {"history": "design"}}]}])json");
#endif
    NativeCommand& redo = add_with_icon("redo", L("Redo"), L("Commands"), "menu_redo", [](const json& args) { return undo_redo(args, true); });
    redo.input_schema   = history;
    picked_as_menu(redo, true);
#ifdef SLIC3R_CAD
    if (options.cad)
        redo.examples = json::parse(R"json([
            {"title": "Put back an extrude undo took back", "calls": [
                {"tool": "extrude", "args": {"width": 20, "height": 20, "distance": 5}},
                {"tool": "undo", "args": {"history": "design"}},
                {"tool": "redo", "args": {"history": "design"}}]},
            {"title": "Put back the shape undo took off a sketch", "calls": [
                {"tool": "sketch_begin", "args": {"plane": "XY"}},
                {"tool": "sketch_add", "args": {"rect": [-15, -10, 15, 10]}},
                {"tool": "sketch_add", "args": {"entities": [{"type": "circle", "center": [0, 0], "radius": 4}]}},
                {"tool": "undo", "args": {"history": "design"}},
                {"tool": "redo", "args": {"history": "design"}}]}])json");
#endif
    add_with_icon("toggle_run_script", L("Run Script"), L("Commands"), "media_play", [](const json&) {
        if (wxGetApp().mainframe != nullptr)
            wxGetApp().run_script_panel().toggle();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("open_action_replay", L("Action Replay"), L("Commands"), "loop", [](const json&) {
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->show_action_replay();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    // The tabs of the Preferences dialog, by the names open_preferences takes.
    static const std::pair<const char*, PreferencesTab> preferences_tabs[] = {{"general", PreferencesTab::General},
                                                                              {"files", PreferencesTab::Files},
                                                                              {"behaviour", PreferencesTab::Behaviour},
                                                                              {"control", PreferencesTab::Control},
                                                                              {"graphics", PreferencesTab::Graphics},
                                                                              {"online", PreferencesTab::Online},
                                                                              {"developer", PreferencesTab::Developer}};
    std::vector<std::string> preferences_tab_names;
    for (const auto& [name, tab] : preferences_tabs)
        preferences_tab_names.emplace_back(name);
    add_with_icon("open_preferences", L("Preferences"), L("Commands"), "cog", [](const json& args) {
        if (Mcp::person_form(args)) {
            wxGetApp().open_preferences();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        // The tab to show, and the preference whose control is scrolled into view and focused.
        const std::string tab = args.value("tab", std::string());
        const auto it = std::find_if(std::begin(preferences_tabs), std::end(preferences_tabs), [&](const auto& t) { return tab == t.first; });
        if (it == std::end(preferences_tabs))
            return AppActionRunResult{AppActionRunResult::Level::Error,
                                      from_u8("tab is required: general, files, behaviour, control, graphics, online or developer")};
        wxGetApp().open_preferences(it->second, args.value("key", std::string()));
        return AppActionRunResult{AppActionRunResult::Level::Success};
    }).input_schema = Mcp::object_schema(
        {{"tab", Mcp::enum_prop(preferences_tab_names, "The tab to show.")},
         {"key", Mcp::prop("string", "A preference on that tab, e.g. auto_arrange on behaviour: its control is scrolled into view and focused.")}});

    // ---- Screenshots and recordings ---- (the MCP screenshot and recording tools' defaults, for the person)
    add_with_icon("take_screenshot", L("Take Screenshot"), L("Commands"), "screenshot", [](const json&) {
        MainFrame* mf = wxGetApp().mainframe;
        if (mf == nullptr)
            return AppActionRunResult{};
        std::string                       err;
        const std::optional<Mcp::Capture> cap = Mcp::capture_window(*mf, err);
        if (!cap)
            // TRN %1% is why, in English.
            return AppActionRunResult{AppActionRunResult::Level::Error, format_wxstr(_L("The screenshot could not be taken: %1%"), err), false};
        const boost::filesystem::path path =
            Mcp::timestamped_path(capture_folder(Mcp::load_recording_settings(*wxGetApp().app_config)), "screenshot", ".png");
        if (!cap->image.SaveFile(from_path(path), wxBITMAP_TYPE_PNG))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("The screenshot could not be saved."), false};
        notify_saved(_L("Screenshot saved"), path);
        return AppActionRunResult{AppActionRunResult::Level::Success, {}, true, path_reply(path)};
    });

    // ---- Mode ----
    add_with_icon("mode_simple", L("Mode: Simple"), L("Mode"), "advanced", [](const json&) {
        wxGetApp().set_mode(comSimple);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("mode_advanced", L("Mode: Advanced"), L("Mode"), "advanced", [](const json&) {
        wxGetApp().set_mode(comAdvanced);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("mode_expert", L("Mode: Expert"), L("Mode"), "advanced", [](const json&) {
        wxGetApp().set_mode(comExpert);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    // Mirrors Preferences > Developer > Developer mode: flip the flag, persist, refresh the UI.
    add_with_icon("toggle_developer_mode", L("Toggle Developer Mode"), L("Mode"), "advanced", [](const json&) {
        GUI_App& app = wxGetApp();
        const bool on = !app.app_config->get_bool("developer_mode");
        app.app_config->set_bool("developer_mode", on);
        app.app_config->save();
        app.update_mode();
        return AppActionRunResult{AppActionRunResult::Level::Success, on ? _L("Developer mode enabled.") : _L("Developer mode disabled.")};
    });

    // ---- Export pipeline ----
    // The G-code is written in the background: a call that gives the file waits for it.
    tile(ui_command(
             "export_gcode", L("Export G-code"), L("Slice & Export"), output_file(".gcode file"),
             [](const json& args) {
                 Plater* plater = wxGetApp().plater();
                 if (plater == nullptr)
                     return AppActionRunResult{};
                 if (Mcp::person_form(args)) {
                     plater->export_gcode(false);
                     return AppActionRunResult{AppActionRunResult::Level::Success};
                 }
                 AppActionRunResult fail;
                 const auto         path = path_argument(args, true, fail);
                 if (!path)
                     return fail;
                 Ops::ExportGcodeArgs gcode;
                 gcode.path = *path;
                 if (const Ops::Outcome<Ops::ExportTicket> started = Ops::start_gcode_export(gcode); !started)
                     return refused(started.refusal());
                 return AppActionRunResult{AppActionRunResult::Level::Info, from_u8(path->string())};
             },
             CommandCompletion::Export))
        .icon = "custom-gcode_gcode";
    add_with_icon("export_stl", L("Export STL"), L("Slice & Export"), "save", [](const json& args) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (Mcp::person_form(args)) {
            plater->export_stl();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_argument(args, true, fail);
        if (!path)
            return fail;
        return written(Ops::export_stl({*path}), *path);
    }).input_schema = output_file(".stl file");
    add_with_icon("export_3mf", L("Export 3MF"), L("Slice & Export"), "menu_save", [](const json& args) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (Mcp::person_form(args)) {
            plater->export_core_3mf();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_argument(args, true, fail);
        if (!path)
            return fail;
        return written(Ops::export_model_3mf({*path}), *path);
    }).input_schema = output_file(".3mf file");
    auto export_sliced = [](bool all) {
        return [all](const json& args) {
            Plater* plater = wxGetApp().plater();
            if (plater == nullptr)
                return AppActionRunResult{};
            if (Mcp::person_form(args)) {
                plater->export_gcode_3mf(all);
                return AppActionRunResult{AppActionRunResult::Level::Success};
            }
            AppActionRunResult fail;
            const auto         path = path_argument(args, true, fail);
            if (!path)
                return fail;
            Ops::ExportSlicedArgs sliced;
            sliced.path       = *path;
            sliced.all_plates = all;
            return written(Ops::export_sliced_3mf(sliced), *path);
        };
    };
    add_with_icon("export_sliced_file", L("Export Sliced File"), L("Slice & Export"), "save", export_sliced(false)).input_schema =
        output_file(".gcode.3mf file");
    add_with_icon("export_all_sliced_file", L("Export All Sliced Files"), L("Slice & Export"), "save", export_sliced(true)).input_schema =
        output_file(".gcode.3mf file");

    // ---- Send to print host ----
    // The Print button's send for a printer with a print host: the Send G-code dialog, then the upload queue. Where
    // the button would not send to a print host, the command says why instead of doing nothing.
    add_with_icon("send_gcode", L("Send G-code to printer host"), L("Slice & Export"), "printer", [](const json&) {
        if (wxGetApp().plater() == nullptr)
            return AppActionRunResult{};
        if (const Ops::Outcome<Ops::Done> sent = Ops::send_gcode_to_host(); !sent)
            return refused(sent.refusal());
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Calibration ----
    // The tab-strip calib_sf glyph is drawn white for the dark tab bar and vanishes on the palette's
    // light tile, so each wizard borrows the matching settings-group icon instead (gray + accent green).
    add_with_icon("calib_temperature", L("Temperature Calibration"), L("Calibration"), "param_temperature",
        [](const json&) { return calib_command(CalibKind::Temperature); });
    add_with_icon("calib_max_volumetric", L("Max Volumetric Speed Calibration"), L("Calibration"), "param_volumetric_speed",
        [](const json&) { return calib_command(CalibKind::MaxVolumetric); });
    add_with_icon("calib_pressure_advance", L("Pressure Advance Calibration"), L("Calibration"), "param_flow_ratio_and_pressure_advance",
        [](const json&) { return calib_command(CalibKind::PressureAdvance); });
    add_with_icon("calib_flow_ratio", L("Flow Ratio Calibration"), L("Calibration"), "param_flow_ratio_and_pressure_advance",
        [](const json&) { return calib_command(CalibKind::FlowRatio); });
    add_with_icon("calib_retraction", L("Retraction Calibration"), L("Calibration"), "param_retraction",
        [](const json&) { return calib_command(CalibKind::Retraction); });
    add_with_icon("calib_cornering", L("Cornering Calibration"), L("Calibration"), "param_precision",
        [](const json&) { return calib_command(CalibKind::Cornering); });
    add_with_icon("calib_input_shaping_freq", L("Input Shaping Frequency Calibration"), L("Calibration"), "param_resonance_avoidance",
        [](const json&) { return calib_command(CalibKind::InputShapingFreq); });
    add_with_icon("calib_input_shaping_damp", L("Input Shaping Damping Calibration"), L("Calibration"), "param_resonance_avoidance",
        [](const json&) { return calib_command(CalibKind::InputShapingDamp); });
    add_with_icon("calib_vfa", L("VFA Calibration"), L("Calibration"), "param_speed", [](const json&) { return calib_command(CalibKind::VFA); });

    // ---- View ----
    // Titles are L("...")-marked here (not via a variable) so xgettext can extract them.
    for (auto [key, dir, title] :
         std::initializer_list<std::tuple<const char*, const char*, std::string>>{{"view_top", "top", L("View: Top")},
                                                                                  {"view_bottom", "bottom", L("View: Bottom")},
                                                                                  {"view_front", "front", L("View: Front")},
                                                                                  {"view_rear", "rear", L("View: Rear")},
                                                                                  {"view_left", "left", L("View: Left")},
                                                                                  {"view_right", "right", L("View: Right")},
                                                                                  {"view_iso", "iso", L("View: Isometric")}}) {
        std::string k = key, d = dir;
        add(k, title, L("View"),
            [d](const json&) { return view_command(wxGetApp().plater(), d); });
    }
    add("view_default", L("View: Default"), L("View"), [](const json&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            plater->select_view("plate");
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->zoom_to_bed();
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("view_fit_bed", L("Fit Bed to View"), L("View"), [](const json&) {
        if (Plater* plater = wxGetApp().plater())
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->zoom_to_bed();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("view_toggle_perspective", L("Toggle Perspective"), L("View"), [](const json&) {
        if (Plater* plater = wxGetApp().plater())
            plater->get_camera().select_next_type();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("reset_window_layout", L("Reset Window Layout"), L("View"), "toolbar_reset", [](const json&) {
        if (Plater* plater = wxGetApp().plater())
            plater->reset_window_layout();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Object ----
    add_with_icon("obj_delete", L("Delete Selected"), L("Object"), "delete", [](const json&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return !p->is_selection_empty(); }, [](Plater* p) { p->remove_selected(); });
    });
    add_with_icon("obj_delete_all", L("Delete All Objects"), L("Object"), "delete", [](const json&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_delete_all(); }, [](Plater* p) { p->delete_all_objects_from_model(); });
    });
    add("select_all", L("Select All Objects"), L("Object"), [](const json&) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        ensure_3d_view(plater);
        plater->select_all();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("deselect_all", L("Deselect All"), L("Object"), [](const json&) {
        if (Plater* plater = wxGetApp().plater())
            plater->deselect_all();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("obj_mirror_x", L("Mirror X"), L("Object"), "menu_mirror_x", [](const json&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::X); });
    });
    add_with_icon("obj_mirror_y", L("Mirror Y"), L("Object"), "menu_mirror_y", [](const json&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::Y); });
    });
    add_with_icon("obj_mirror_z", L("Mirror Z"), L("Object"), "menu_mirror_z", [](const json&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::Z); });
    });
    add_with_icon("obj_split_objects", L("Split to Objects"), L("Object"), "menu_split_objects", [](const json&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_split_to_objects(); }, [](Plater* p) { p->split_object(true); });
    });
    add_with_icon("obj_split_parts", L("Split to Parts"), L("Object"), "menu_split_parts", [](const json&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_split_to_volumes(); }, [](Plater* p) { p->split_volume(); });
    });
    add("obj_center", L("Center Selected on Plate"), L("Object"), [](const json&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return !p->is_selection_empty(); }, [](Plater* p) { p->center_selection(); });
    });
    add_with_icon("obj_drop", L("Drop to Bed"), L("Object"), "toolbar_flatten", [](const json&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->get_selection().can_drop(); }, [](Plater* p) { p->drop_selection(); });
    });
    add("obj_fit_volume", L("Scale to Fit Print Volume"), L("Object"), [](const json&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_scale_to_print_volume(); },
            [](Plater* p) { p->scale_selection_to_fit_print_volume(); });
    });
    add_with_icon("obj_instances_up", L("Increase Instances"), L("Object"), "instance_add", [](const json&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_increase_instances(); }, [](Plater* p) { p->increase_instances(); });
    });
    add_with_icon("obj_instances_down", L("Decrease Instances"), L("Object"), "instance_remove", [](const json&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_decrease_instances(); }, [](Plater* p) { p->decrease_instances(); });
    });
    add_with_icon("obj_arrange", L("Auto-Arrange"), L("Object"), "toolbar_arrange", [](const json&) {
        return arrange_all(wxGetApp().plater());
    });
    add_with_icon("obj_orient", L("Auto-Orient"), L("Object"), "toolbar_orient", [](const json&) {
        return orient_all(wxGetApp().plater());
    });

    // ---- Object structure ---- (the object list's context menus and the plate menu's Order By)
    // Change Type is in the part menus only: without part rows, set_volume_type() would change every
    // part of a selected object, which no menu offers.
    auto add_type = [&](std::string key, std::string title, ModelVolumeType type) {
        add(std::move(key), std::move(title), L("Object"), [type](const json&) {
            ObjectList* list = object_list_in_3d_view();
            if (list == nullptr)
                return AppActionRunResult{};
            if (selected_rows(*list, itVolume).empty())
                return AppActionRunResult{AppActionRunResult::Level::Info, _L("Select a part first."), false};
            const wxString why = list->set_volume_type(type, true, /*show_errors=*/false);
            if (!why.empty())
                return AppActionRunResult{AppActionRunResult::Level::Error, why};
            return AppActionRunResult{AppActionRunResult::Level::Success};
        });
    };
    add_type("obj_type_part", L("Change Type to Part"), ModelVolumeType::MODEL_PART);
    add_type("obj_type_negative", L("Change Type to Negative Part"), ModelVolumeType::NEGATIVE_VOLUME);
    add_type("obj_type_modifier", L("Change Type to Modifier"), ModelVolumeType::PARAMETER_MODIFIER);
    add_type("obj_type_support_blocker", L("Change Type to Support Blocker"), ModelVolumeType::SUPPORT_BLOCKER);
    add_type("obj_type_support_enforcer", L("Change Type to Support Enforcer"), ModelVolumeType::SUPPORT_ENFORCER);
    add_type("obj_type_precise_seam", L("Change Type to Precise Seam"), ModelVolumeType::PRECISE_SEAM_CENTER);
    add("obj_rename", L("Rename"), L("Object"), [](const json& args) {
        // The Rename dialog leaves the name as it is when it is emptied, so a call has to give one.
        const std::string name = args.value("name", std::string());
        if (!Mcp::person_form(args) && name.empty())
            return AppActionRunResult{AppActionRunResult::Level::Error,
                                      from_u8("A name is needed: give the new name, or no arguments for the Rename dialog.")};
        ObjectList* list = object_list_in_3d_view();
        if (list == nullptr)
            return AppActionRunResult{};
        const wxDataViewItemArray rows = selected_rows(*list, itObject | itVolume);
        if (rows.size() != 1)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Select one object or part first."), false};
        if (Mcp::person_form(args))
            list->rename_item();
        else if (!list->rename_item(rows.front(), from_u8(name)))
            return AppActionRunResult{AppActionRunResult::Level::Error,
                                      _L("Invalid name, the following characters are not allowed:") + " <>:/\\|?*\""};
        return AppActionRunResult{AppActionRunResult::Level::Success};
    }).input_schema = Mcp::object_schema({{"name", Mcp::prop("string", "The new name, in place of the Rename dialog.")}});
    add_with_icon("obj_set_instances", L("Set Number of Instances"), L("Object"), "instance_add", [](const json& args) {
        Plater*     plater = wxGetApp().plater();
        ObjectList* list   = object_list_in_3d_view();
        if (list == nullptr)
            return AppActionRunResult{};
        // A selection to make and a job to wait for are told as information, the other reasons as errors.
        const auto refused_count = [](const Ops::Refusal& refusal) {
            const std::string code = refusal.code;
            return refused(refusal, code == "no_object_selected" || code == "job_running" ? AppActionRunResult::Level::Info :
                                                                                               AppActionRunResult::Level::Error);
        };
        if (Mcp::person_form(args)) {
            // The asking path: the item's dialog, enabled as the menu enables it.
            if (const std::optional<Ops::Refusal> refusal = Ops::instance_count_refusal())
                return refused_count(*refusal);
            plater->set_number_of_copies();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        const Ops::Outcome<Ops::InstanceCount> set = Ops::set_instance_count({args.at("count").get<int>()});
        if (!set)
            return refused_count(set.refusal());
        return with_reply({{"object", set.value().object}, {"instances", set.value().instances}});
    }).input_schema = Mcp::object_schema({{"count", {{"type", "integer"},
                                                     {"minimum", 1},
                                                     {"description", "The total number of instances, up to 1000, in place of the dialog."}}}});
    add("obj_assemble", L("Assemble"), L("Object"), [](const json&) {
        ObjectList* list = object_list_in_3d_view();
        if (list == nullptr)
            return AppActionRunResult{};
        // Assemble is in the menu of two or more selected objects only; can_merge_to_multipart_object()
        // alone accepts one, which merge() would replace with an "Assembly" of one.
        if (selected_rows(*list, itObject | itInstance).size() < 2)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Select two or more objects first."), false};
        if (!list->can_merge_to_multipart_object())
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("Cut objects can't be assembled.")};
        list->merge(true);
        const ModelObjectPtrs& objects = wxGetApp().plater()->model().objects;
        return with_reply({{"object", objects.size() - 1}, {"name", objects.back()->name}});
    });
    add("obj_instances_to_objects", L("Set as Individual Objects"), L("Object"), [](const json&) {
        Plater*     plater = wxGetApp().plater();
        ObjectList* list   = object_list_in_3d_view();
        if (list == nullptr)
            return AppActionRunResult{};
        if (!plater->can_set_instance_to_object())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Select an object with more than one instance first."), false};
        const int    obj_idx = plater->get_selected_object_idx();
        const size_t before  = plater->model().objects.size();
        list->split_instances();
        nlohmann::json objects = nlohmann::json::array({obj_idx});
        for (size_t i = before; i < plater->model().objects.size(); ++i)
            objects.push_back(i);
        return with_reply({{"objects", objects}});
    });
    auto add_order = [&](std::string key, std::string title, ObjectOrder order) {
        add(std::move(key), std::move(title), L("Object"), [order](const json& args) {
            Plater*     plater = wxGetApp().plater();
            ObjectList* list   = object_list_in_3d_view();
            if (list == nullptr)
                return AppActionRunResult{};
            const bool all = args.value("all", false);
            if (all ? plater->model().objects.empty() : plater->get_partplate_list().get_selected_plate()->empty())
                return AppActionRunResult{AppActionRunResult::Level::Info, _L("There are no objects to order."), false};
            // order_objects() only shows a notification while a gizmo is open.
            if (plater->get_view3D_canvas3D()->get_gizmos_manager().get_current_type() != GLGizmosManager::Undefined)
                return AppActionRunResult{AppActionRunResult::Level::Error, _L("Error: Please close all toolbar menus first")};
            list->order_objects(order, all);
            return ordered(*plater, order, all);
        }).input_schema = Mcp::object_schema({{"all", Mcp::prop("boolean", "Order the objects of every plate. Default false: the current plate's.")}});
    };
    add_order("obj_order_by_name", L("Order by Name"), ObjectOrder::Name);
    add_order("obj_order_by_filament", L("Order by Filament"), ObjectOrder::Filament);
    add_order("obj_order_by_height", L("Order by Height"), ObjectOrder::Height);
    add_order("obj_order_by_size", L("Order by Size"), ObjectOrder::Size);

    // ---- Add Primitive ---- (the Add > Add Primitive submenu; creates a new object)
    // A size holds the dimensions the size dialog asks for, in its order, and skips the dialog; `size_help` names
    // them.
    auto add_primitive = [&](std::string key, std::string title, std::string icon, const char* type_name, const char* size_help) {
        add_with_icon(std::move(key), std::move(title), L("Add Primitive"), std::move(icon), [type_name, size_help](const json& args) {
            Plater*     plater = wxGetApp().plater();
            ObjectList* list   = wxGetApp().obj_list();
            if (plater == nullptr || list == nullptr)
                return AppActionRunResult{};
            ensure_3d_view(plater);
            if (!list->can_add_shape_object())
                return AppActionRunResult{AppActionRunResult::Level::Error,
                                          _L("Deselect all objects to add a primitive object. To add a shape to the selected object, "
                                             "use its Add Part menu.")};
            const std::vector<double> size = args.value("size", std::vector<double>());
            if (!Mcp::person_form(args) && !is_primitive_size(type_name, size))
                return AppActionRunResult{AppActionRunResult::Level::Error,
                                          from_u8(std::string("The size is ") + size_help + ", each above 0.")};
            list->load_generic_subobject(type_name, ModelVolumeType::INVALID, true, size);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }).input_schema = Mcp::object_schema({{"size", Mcp::array_prop({{"type", "number"}}, std::string("The size, ") + size_help +
                                                                                            ", each above 0, in place of the size dialog.")}});
    };
    add_primitive("add_primitive_cube", L("Cube"), "menu_obj_cube", "Cube", "[X, Y, Z] in mm");
    add_primitive("add_primitive_cylinder", L("Cylinder"), "menu_obj_cylinder", "Cylinder", "[diameter, height] in mm");
    add_primitive("add_primitive_sphere", L("Sphere"), "menu_obj_sphere", "Sphere", "[diameter] in mm");
    add_primitive("add_primitive_cone", L("Cone"), "menu_obj_cone", "Cone", "[base diameter, height] in mm");
    add_primitive("add_primitive_disc", L("Disc"), "menu_obj_disc", "Disc", "[diameter, height] in mm");
    add_primitive("add_primitive_torus", L("Torus"), "menu_obj_torus", "Torus", "[inner diameter, tube diameter] in mm");
    add_with_icon("add_primitive_text", L("Text"), L("Add Primitive"), "menu_obj_text", [](const json&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            ensure_3d_view(plater);
            if (GLCanvas3D* canvas = plater->canvas3D())
                canvas->clear_popup_menu_position();
            MenuFactory::add_text_volume(ModelVolumeType::INVALID);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("add_primitive_svg", L("SVG"), L("Add Primitive"), "menu_obj_svg", [](const json& args) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        ensure_3d_view(plater);
        GLCanvas3D* canvas = plater->canvas3D();
        if (canvas != nullptr)
            canvas->clear_popup_menu_position();
        if (Mcp::person_form(args)) {
            MenuFactory::add_svg_volume(ModelVolumeType::INVALID);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        // What the dialog path does once a file is picked: a new object with nothing selected, else
        // a part of the selected object.
        AppActionRunResult fail;
        const auto         path = path_argument(args, false, fail);
        if (!path)
            return fail;
        auto* svg = canvas != nullptr ? dynamic_cast<GLGizmoSVG*>(canvas->get_gizmos_manager().get_gizmo(GLGizmosManager::Svg)) : nullptr;
        if (svg == nullptr || !svg->create_volume(into_u8(from_path(*path)), ModelVolumeType::MODEL_PART))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("The SVG file could not be added.")};
        return AppActionRunResult{AppActionRunResult::Level::Success};
    }).input_schema = Mcp::object_schema({{"path", input_file("an .svg file")}});

    // ---- Add Handy models ---- (the Add > Add Handy models submenu)
    const std::vector<MenuFactory::HandyModel>& handy = MenuFactory::handy_models();
    for (std::size_t i = 0; i < handy.size(); ++i) {
        add("add_handy_" + std::string(handy[i].key), handy[i].label, L("Add Handy models"),
            [i](const json&) {
                if (Plater* plater = wxGetApp().plater())
                    ensure_3d_view(plater);
                MenuFactory::load_handy_model(i);
                return AppActionRunResult{AppActionRunResult::Level::Success};
            });
    }

    // ---- Plate ----
    add_with_icon("plate_add", L("Add Plate"), L("Plate"), "toolbar_add_plate", [](const json&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_add_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot add another plate (maximum reached).")};
        plater->add_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_duplicate", L("Duplicate Plate"), L("Plate"), "menu_copy", [](const json&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_add_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot duplicate a plate (maximum reached).")};
        plater->duplicate_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_delete", L("Delete Plate"), L("Plate"), "delete", [](const json&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_delete_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot delete the only plate.")};
        plater->delete_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_rename", L("Rename Plate"), L("Plate"), "plate_name_edit", [](const json&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlate* curr = plater->get_partplate_list().get_curr_plate();
        PlateNameEditDialog dlg((wxWindow*) wxGetApp().mainframe, wxID_ANY, _L("Edit Plate Name"));
        dlg.set_plate_name(from_u8(curr->get_plate_name()));
        if (dlg.ShowModal() == wxID_YES)
            plater->rename_plate(plater->get_partplate_list().get_curr_plate_index(), dlg.get_plate_name().ToUTF8().data());
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_toggle_lock", L("Toggle Plate Lock"), L("Plate"), "lock_normal", [](const json&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlateList&          plates = plater->get_partplate_list();
        Ops::SetPlateLockedArgs lock;
        lock.locked = !plates.is_locked(plates.get_curr_plate_index());
        if (const Ops::Outcome<Ops::PlateLocked> locked = Ops::set_plate_locked(lock); !locked)
            return refused(locked.refusal());
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_goto", L("Go to Plate"), L("Plate"), "go_next_plate", [](const json& args) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlateList& plates = plater->get_partplate_list();
        const int count       = plates.get_plate_count();
        if (count <= 0)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("No plates available.")};
        Ops::SelectPlateArgs plate;
        plate.plate = args.value("plate", plate.plate);
        if (const Ops::Outcome<Ops::Done> selected = Ops::select_plate(plate); !selected)
            return refused(selected.refusal());
        return AppActionRunResult{AppActionRunResult::Level::Success};
    }).input_schema = Mcp::object_schema({{"plate", Mcp::prop("integer", "The plate's 0-based index. Default 0.")}});

    // ---- Printer / device connection ----
    add_with_icon("sync_ams", L("Synchronize Filament List from AMS"), L("Printer"), "ams_fila_sync", [](const json&) {
        Plater* plater     = wxGetApp().plater();
        DeviceManager* dev = wxGetApp().getDeviceManager();
        if (dev && dev->get_selected_machine() && plater) {
            plater->sidebar().sync_ams_list();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        return AppActionRunResult{AppActionRunResult::Level::Info, _L("Connect a printer to synchronize the AMS filament list.")};
    });

    // ---- Filament slots ----
    // The + and - buttons above the filament list and a slot's Delete menu item. They show for printers
    // that take several filaments through one extruder (Sidebar::show_SEMM_buttons()), and a G-code file
    // shown in place of a project has its own filament list.
    const auto filament_slots_unavailable = [](Plater* plater) -> std::optional<AppActionRunResult> {
        if (!is_fff_plater(plater) || plater->using_exported_file())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Filament slots belong to a filament (FFF) project, not to a G-code file.")};
        if (!Sidebar::should_show_SEMM_buttons())
            return AppActionRunResult{AppActionRunResult::Level::Info,
                                      _L("This printer has one filament per extruder; change the number of extruders in the printer settings.")};
        return std::nullopt;
    };
    add_with_icon("filament_add", L("Add Filament Slot"), L("Filament"), "add_filament", [filament_slots_unavailable](const json&) {
        Plater* plater = wxGetApp().plater();
        if (const auto refused = filament_slots_unavailable(plater))
            return *refused;
        const PresetBundle& bundle = *wxGetApp().preset_bundle;
        if (bundle.filament_presets.size() >= MAXIMUM_EXTRUDER_NUMBER)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("No more filament slots can be added.")};
        // A new physical slot goes after the physical ones; mixed filaments move up by one.
        const size_t slot = bundle.num_physical_filaments() + 1;
        plater->sidebar().add_filament();
        plater->sidebar().update_filaments_counter();
        return AppActionRunResult{AppActionRunResult::Level::Success, format_wxstr(_L("Added filament slot %1%."), slot)};
    });
    add_with_icon("filament_delete", L("Delete Filament Slot"), L("Filament"), "delete_filament", [filament_slots_unavailable](const json& args) {
        Plater* plater = wxGetApp().plater();
        if (const auto refused = filament_slots_unavailable(plater))
            return *refused;
        const PresetBundle& bundle   = *wxGetApp().preset_bundle;
        const size_t        physical = bundle.num_physical_filaments();
        if (physical < 2)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("The only filament slot cannot be deleted.")};
        // Without a slot, the last physical one, as the - button.
        size_t slot = physical;
        if (args.contains("slot")) {
            slot = args.at("slot").get<size_t>();
            if (slot < 1 || slot > bundle.filament_presets.size())
                return AppActionRunResult{AppActionRunResult::Level::Error,
                                          format_wxstr(_L("Filament slot must be 1 to %1%."), bundle.filament_presets.size())};
            if (bundle.is_mixed_filament(slot - 1))
                return AppActionRunResult{AppActionRunResult::Level::Error,
                                          format_wxstr(_L("Filament slot %1% is a mixed filament; delete it from the mixed filament list."), slot)};
        }
        plater->sidebar().delete_filament(slot - 1);
        plater->sidebar().update_filaments_counter();
        return AppActionRunResult{AppActionRunResult::Level::Success, format_wxstr(_L("Deleted filament slot %1%."), slot)};
    }).input_schema = Mcp::object_schema(
        {{"slot", {{"type", "integer"},
                   {"minimum", 1},
                   {"description", "The 1-based filament slot. Default: the last physical slot, as the - button."}}}});

    // ---- Presets / cloud ----
    add_with_icon("preset_bundle", L("Open Preset Bundle"), L("Presets"), "menu_edit_preset", [](const json&) {
        wxGetApp().open_presetbundledialog();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("sync_presets", L("Sync Presets"), L("Presets"), "printer_sync_ok", [](const json&) {
        if (!wxGetApp().is_user_login())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Sign in to sync presets.")};
        wxGetApp().restart_sync_user_preset();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Import ----
    add_with_icon("import_file", L("Import 3MF/STL/STEP/SVG/OBJ/AMF"), L("Import"), "menu_open", [](const json& args) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (Mcp::person_form(args)) {
#ifdef __APPLE__
            plater->add_model();
#else
            plater->add_file();
#endif
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_argument(args, false, fail);
        if (!path)
            return fail;
        Ops::ImportModelsArgs models;
        models.paths = {*path};
        if (const Ops::Outcome<Ops::ImportModelsResult> imported = Ops::import_models(models); !imported)
            return refused(imported.refusal());
        return AppActionRunResult{AppActionRunResult::Level::Success};
    }).input_schema = Mcp::object_schema({{"path", input_file("a model file")}});
    add_with_icon("import_zip_archive", L("Import ZIP Archive"), L("Import"), "menu_open", [](const json&) {
        if (Plater* plater = wxGetApp().plater())
            plater->import_zip_archive();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("import_configs", L("Import Configs"), L("Import"), "menu_open", [](const json& args) {
        MainFrame* mf = wxGetApp().mainframe;
        if (mf == nullptr)
            return AppActionRunResult{};
        if (Mcp::person_form(args)) {
            mf->load_config_file();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_argument(args, false, fail);
        if (!path)
            return fail;
        const bool overwrite = args.value("on_conflict", std::string("skip")) == "overwrite";
        const std::string file = Mcp::path_to_utf8(*path);
        if (is_gcode_file(file)) {
            if (!mf->load_config_file(file))
                return AppActionRunResult{AppActionRunResult::Level::Error, _L("The configuration was not loaded.")};
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        for (const char* ext : {".json", ".zip", ".orca_printer", ".orca_filament", ".orca_bundle"})
            if (boost::iends_with(file, ext))
                return import_preset_file(*path, overwrite);
        return AppActionRunResult{AppActionRunResult::Level::Error,
                                  "Not a preset file: give a .json, .zip, .orca_printer, .orca_filament, .orca_bundle or .gcode"};
    }).input_schema = Mcp::object_schema(
        {{"path", input_file("a preset .json, a .zip of them, an .orca_printer, .orca_filament or .orca_bundle, or a .gcode")},
         {"on_conflict", Mcp::enum_prop({"skip", "overwrite"}, "What happens to a preset that exists already. Default skip.")}});

    // ---- Export extras ----
    // The argument of a command that writes several files, in place of its folder dialog.
    const auto output_dir = Mcp::object_schema({{"path", Mcp::prop("string", "Absolute path of an existing directory to write into.")}});
    add_with_icon("export_stl_multi", L("Export All Objects as STLs"), L("Export"), "save", [](const json& args) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (Mcp::person_form(args)) {
            plater->export_stl(false, false, true);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         dir = dir_argument(args, fail);
        if (!dir)
            return fail;
        plater->export_stl(false, false, true, FT_STL, from_path(*dir));
        return AppActionRunResult{AppActionRunResult::Level::Success, from_u8(dir->string())};
    }).input_schema = output_dir;
    add_with_icon("export_drc_single", L("Export All Objects as DRC (one file)"), L("Export"), "save", [](const json&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_stl(false, false, false, FT_DRC);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_drc_multi", L("Export All Objects as DRCs"), L("Export"), "save", [](const json&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_stl(false, false, true, FT_DRC);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_toolpaths_obj", L("Export Toolpaths as OBJ"), L("Export"), "custom-gcode_gcode", [](const json&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_toolpaths_to_obj();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_config", L("Export Preset Bundle"), L("Export"), "save", [](const json& args) {
        if (Mcp::person_form(args)) {
            if (MainFrame* mf = wxGetApp().mainframe)
                mf->export_config();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         dir = dir_argument(args, fail);
        if (!dir)
            return fail;
        // The dialog MainFrame::export_config() opens, given the folder its OK writes to.
        ExportConfigsDialog dlg(nullptr, from_path(*dir));
        if (dlg.ShowModal() != wxID_OK)
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("Nothing was exported.")};
        return AppActionRunResult{AppActionRunResult::Level::Success, from_path(*dir)};
    }).input_schema = output_dir;

    // ---- Help ---- (mirrors the top-bar Help menu, plus the wiki/YouTube links)
    add("help_keyboard_shortcuts", L("Keyboard Shortcuts"), L("Help"), [](const json&) {
        wxGetApp().keyboard_shortcuts(ShortcutContext::Global);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_setup_wizard", L("Setup Wizard"), L("Help"), [](const json&) {
        wxGetApp().ShowUserGuide();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_open_config_folder", L("Show Configuration Folder"), L("Help"), "open_project", [](const json&) {
        Slic3r::GUI::desktop_open_datadir_folder();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_migrate_filament_presets", L("Migrate Filament Presets"), L("Help"), [](const json&) {
        wxGetApp().check_legacy_user_filament_presets(true);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_troubleshoot", L("Troubleshoot Center"), L("Help"), [](const json&) {
        wxGetApp().troubleshoot();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_network_test", L("Open Network Test"), L("Help"), [](const json&) {
        NetworkTestDialog dlg(wxGetApp().mainframe);
        dlg.ShowModal();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_benchmark_3d_scene", L("Benchmark 3D Scene"), L("Help"), [](const json&) {
        run_scene_benchmark();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_tip_of_the_day", L("Show Tip of the Day"), L("Help"), "info", [](const json&) {
        if (Plater* plater = wxGetApp().plater()) {
            plater->get_dailytips()->open();
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->set_as_dirty();
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_check_updates", L("Check for Updates"), L("Help"), "refresh", [](const json&) {
        wxGetApp().check_new_version_sf(true, 1);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_about", L("About OrcaSlicer"), L("Help"), "OrcaSlicer_gradient_circle", [](const json&) {
        Slic3r::GUI::about();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("open_wiki", L("Open Wiki"), L("Help"), "link_wiki_img", [](const json&) {
        wxLaunchDefaultBrowser("https://www.orcaslicer.com/wiki/", wxBROWSER_NEW_WINDOW);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("open_youtube", L("Open YouTube Channel"), L("Help"), [](const json&) {
        wxLaunchDefaultBrowser("https://www.youtube.com/@OfficialOrcaSlicer/videos", wxBROWSER_NEW_WINDOW);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Plugins ----
    add("open_plugins", L("Open Plugins"), L("Plugins"), [](const json&) {
        wxGetApp().open_plugins_dialog();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("refresh_plugins", L("Refresh Plugins"), L("Plugins"), [](const json&) {
        wxGetApp().refresh_plugins();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("install_plugin", L("Install Plugin"), L("Plugins"), [](const json&) {
        open_plugin_hub();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("install_local_plugin", L("Install Local Plugin"), L("Plugins"), [](const json& args) {
        MainFrame* mf = wxGetApp().mainframe;
        if (mf == nullptr)
            return AppActionRunResult{};
        boost::filesystem::path package;
        if (Mcp::person_form(args)) {
            wxFileDialog dialog(mf, _L("Select plugin package"), wxEmptyString, wxEmptyString, _L("Plugin files (*.py;*.whl)|*.py;*.whl"),
                                wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() != wxID_OK)
                return AppActionRunResult{AppActionRunResult::Level::Success};
            package = into_path(dialog.GetPath());
        } else {
            AppActionRunResult fail;
            const auto         path = path_argument(args, false, fail);
            if (!path)
                return fail;
            package = *path;
        }
        wxString   message;
        const bool installed = install_local_plugin_package(package, mf, message);
        wxGetApp().update_plugins_dialog();
        if (message.IsEmpty()) // the overwrite prompt was cancelled
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("The plugin was not installed."), false};
        return AppActionRunResult{installed ? AppActionRunResult::Level::Success : AppActionRunResult::Level::Error, message};
    }).input_schema = Mcp::object_schema({{"path", input_file("a .py or .whl plugin package")}});

    describe_commands(out);
    for (NativeCommand& c : out)
        catalog.add(std::move(c));

    // The typed tools, family by family, in the order tools/list shows them: the app's state, the search and the generic
    // call first, then the project data, replays, Python and recordings, and the Speed Dial and the GUI-driving tools
    // last. Between them the Design tab's commands, which no listing shows, only while the CAD feature is on.
    Mcp::register_app_tools(catalog);
    Mcp::register_api_tools(catalog);
    Mcp::register_project_tools(catalog);
    Mcp::register_config_tools(catalog);
    Mcp::register_scene_tools(catalog);
    Mcp::register_layer_tools(catalog);
    Mcp::register_view_tools(catalog);
    Mcp::register_replay_tools(catalog);
    Mcp::register_python_tools(catalog);
    Mcp::register_record_tools(catalog);
#ifdef SLIC3R_CAD
    if (options.cad)
        Mcp::register_cad_commands(catalog);
#endif
    Mcp::register_action_tools(catalog);
    Mcp::register_ui_tools(catalog);
}

namespace {

// The process catalog: `process_catalog` owns it and is set once, under `catalog_mutex`; `published`
// points at it from then on, so that instance() reads it without the lock.
std::mutex                            catalog_mutex;
std::unique_ptr<const CommandCatalog> process_catalog;
std::atomic<const CommandCatalog*>    published{nullptr};

// Builds, seals and publishes the process catalog. Under catalog_mutex.
const CommandCatalog& publish_catalog(const NativeCommands::CatalogOptions& options)
{
    auto catalog = std::make_unique<CommandCatalog>();
    NativeCommands::register_all_commands(*catalog, options);
    catalog->seal();
    process_catalog = std::move(catalog);
    published.store(process_catalog.get(), std::memory_order_release);
    return *process_catalog;
}

} // namespace

void NativeCommands::build(const CatalogOptions& options)
{
    const std::lock_guard<std::mutex> lock(catalog_mutex);
    if (process_catalog != nullptr)
        throw std::logic_error("The command catalog is built already");
    publish_catalog(options);
}

const CommandCatalog& NativeCommands::instance()
{
    if (const CommandCatalog* catalog = published.load(std::memory_order_acquire))
        return *catalog;
    const std::lock_guard<std::mutex> lock(catalog_mutex);
    return process_catalog != nullptr ? *process_catalog : publish_catalog({});
}

std::vector<const NativeCommand*> NativeCommands::palette_commands(const CommandCatalog& catalog)
{
    std::vector<const NativeCommand*> out;
    for (const NativeCommand& c : catalog.commands())
        // The Design tab's tiles are there only while the tab is shown (ActionRegistry::materialize_design_actions()).
        if (c.in_palette && c.group != kDesignGroup)
            out.push_back(&c);
    return out;
}

wxString NativeCommands::tile_title(const NativeCommand& command)
{
    const char* context = title_context(command);
    return context != nullptr ? I18N::translate(command.title, context) : I18N::translate(command.title);
}

std::unique_ptr<AppAction> NativeCommands::make_action(const NativeCommand& command)
{
    return std::make_unique<CommandAction>(command, into_u8(tile_title(command)));
}

const NativeCommand* NativeCommands::find(const std::string& key) { return instance().find(key); }

NativeCommand NativeCommands::ui_command(std::string key, std::string title, std::string group, json schema,
                                         std::function<AppActionRunResult(const json& args)> fn, CommandCompletion completion)
{
    NativeCommand c;
    c.key          = std::move(key);
    c.title        = std::move(title);
    c.group        = std::move(group);
    c.input_schema = std::move(schema);
    c.completion   = completion;
    c.run          = [key = c.key, fn = std::move(fn), completion](const json& args, Mcp::ToolContext& ctx) {
        if (ctx.person()) {
            AppActionRunResult shown = fn(args);
            Mcp::ToolResult    reply = from_run_result(key, shown);
            reply.notice             = std::make_shared<const AppActionRunResult>(std::move(shown));
            return reply;
        }
        // An export given its file holds the project from before it starts, and the call waits for the file.
        const bool         exports = completion == CommandCompletion::Export && !Mcp::person_form(args);
        AppActionRunResult result;
        Ops::ExportTicket  ticket; // empty without a plater
        ctx.run_ui([&]() -> json {
            Plater* plater = exports ? app_plater() : nullptr;
            if (exports)
                ctx.begin_operation();
            if (plater != nullptr) {
                ticket.seq_before = plater->export_seq();
                ticket.revision   = plater->project_revision();
            }
            result = fn(args);
            if (plater != nullptr) {
                ticket.seq_after = plater->export_seq();
                if (const auto run = plater->export_run(ticket.seq_after))
                    ticket.path = run->path;
            }
            return json();
        });
        Mcp::ToolResult reply = from_run_result(key, result);
        if (!exports) {
            Mcp::settle(ctx);
            return reply;
        }
        if (reply.is_error)
            return reply;
        if (ticket.seq_after == ticket.seq_before)
            return Mcp::ToolResult::error(result.message.empty() ? "The export did not start." : into_u8(result.message));
        return Mcp::await_gcode_export(ctx, ticket);
    };
    return c;
}

NativeCommand NativeCommands::long_command(std::string key, std::string title, std::string group, json schema, CommandFn fn, bool inline_call)
{
    NativeCommand c;
    c.key          = std::move(key);
    c.title        = std::move(title);
    c.group        = std::move(group);
    c.input_schema = std::move(schema);
    c.inline_call  = inline_call;
    c.run          = [key = c.key, fn = std::move(fn), inline_call](const json& args, Mcp::ToolContext& ctx) {
        // A person's pick is on the UI thread, which a long function must not hold.
        if (ctx.person() && !inline_call)
            return ctx.submit(key, args);
        return fn(args, ctx);
    };
    return c;
}

NativeCommand NativeCommands::with_person_form(NativeCommand tool, std::string title, std::string group, std::string icon,
                                               std::function<AppActionRunResult()> person, bool person_form_behind_dialogs)
{
    // The person's form runs as a ui_command() function does: in place for a person's pick, in one UI closure
    // for an automated call, whose reply then says what the pick showed.
    const CommandFn asks    = ui_command(tool.key, title, group, Mcp::object_schema({}), [person](const json&) { return person(); }).run;
    const bool      checked = !person_form_behind_dialogs && tool.guard != Mcp::GuardClass::Mutate;
    tool.title              = std::move(title);
    tool.group              = std::move(group);
    tool.icon               = std::move(icon);
    tool.in_palette         = true;
    tool.input_schema       = Mcp::with_silent(std::move(tool.input_schema));
    tool.run = [key = tool.key, person = std::move(person), asks, checked, silent = std::move(tool.run)](const json& args, Mcp::ToolContext& ctx) {
        if (!Mcp::person_form(args))
            return silent(args, ctx);
        if (!checked || ctx.person())
            return asks(args, ctx);
        // An agent's call of a tool whose guard lets it run behind dialogs: the person's form opens what a pick
        // opens, so it is refused while a modal dialog is open or another call's nested loop runs, in the UI
        // closure that runs it.
        Mcp::UiInvoker& ui = ctx.service().ui();
        return ui_command(key, {}, {}, Mcp::object_schema({}), [&person, &ui](const json&) {
                   ui.require_mutate();
                   return person();
               }).run(args, ctx);
    };
    return tool;
}

Mcp::ToolResult NativeCommands::from_run_result(const std::string& key, const AppActionRunResult& result)
{
    // An operation's refusal is worded once, in English here as in a tool's reply.
    const std::string message = result.refusal ? result.refusal->english() : into_u8(result.message);
    // A command that did nothing (no selection, no objects) fails here, though the palette shows it as
    // information.
    if (result.level == AppActionRunResult::Level::Error || result.level == AppActionRunResult::Level::Busy || !result.done) {
        json details;
        if (result.refusal) {
            details = {{"code", result.refusal->code}};
            if (!result.refusal->names.empty())
                details["names"] = result.refusal->names;
        }
        return Mcp::ToolResult::error(message.empty() ? "The action failed" : message, std::move(details));
    }
    json out{{"ran", key}, {"level", level_name(result.level)}};
    if (!message.empty())
        out["message"] = message;
    if (!result.reply.empty())
        if (json reply = json::parse(result.reply, nullptr, /*allow_exceptions=*/false); reply.is_object())
            out.update(reply);
    return Mcp::ToolResult::ok(std::move(out));
}

AppActionRunResult NativeCommands::run(const std::string& key, const json& args)
{
    if (wxGetApp().is_closing())
        return {};
    const NativeCommand* command = find(key);
    if (command == nullptr)
        return {AppActionRunResult::Level::Info, _L("Unknown command.")};
    const auto failed = [](const std::string& why) { return AppActionRunResult{AppActionRunResult::Level::Error, from_u8(why), false}; };
    json       checked = args;
    if (const std::string err = Mcp::argument_error(*command, checked); !err.empty())
        return failed(err);
    Mcp::PersonContext person;
    try {
        const Mcp::ToolResult reply = command->run(checked, person);
        if (reply.notice)
            return *reply.notice;
        if (reply.is_error)
            return failed(reply.message);
        return {AppActionRunResult::Level::Success};
    } catch (const std::exception& e) {
        return failed(e.what());
    }
}

void NativeCommands::notify_result(const AppActionRunResult& result)
{
    if (result.level == AppActionRunResult::Level::Busy || result.message.IsEmpty() || wxGetApp().plater() == nullptr)
        return;
    wxGetApp().plater()->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                                                                      result.level == AppActionRunResult::Level::Error ?
                                                                          NotificationManager::NotificationLevel::ErrorNotificationLevel :
                                                                          NotificationManager::NotificationLevel::RegularNotificationLevel,
                                                                      into_u8(result.message));
}

bool NativeCommands::recording_active()
{
    Mcp::McpService* service = wxGetApp().mcp_service();
    return service != nullptr && service->started() && service->recorder().active();
}

}} // namespace Slic3r::GUI

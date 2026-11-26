#include "NativeCommands.hpp"
#include "NativeCommandMenuItems.hpp"

#include "ActionIds.hpp"
#include "calib_dlg.hpp"
#include "Camera.hpp"
#include "CommandCatalog.hpp"
#include "CommandTypes.hpp"
#include "CreatePresetsDialog.hpp"
#include "DailyTips.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "Gizmos/GLGizmoSVG.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "I18N.hpp"
#include "Shortcuts.hpp"
#include "MainFrame.hpp"
#include "AgentActivityPanel.hpp"
#include "RunScriptPanel.hpp"
#include "NetworkTestDialog.hpp"
#include "Operations/ExportOps.hpp"
#include "Operations/ObjectOps.hpp"
#include "Operations/Outcome.hpp"
#include "Operations/PlateOps.hpp"
#include "Operations/PluginOps.hpp"
#include "Operations/PresetOps.hpp"
#include "Operations/PrinterOps.hpp"
#include "Operations/ProjectOps.hpp"
#include "Plater.hpp"
#include "PluginsDialog.hpp"
#include "RecordingDialog.hpp"
#include "Preferences.hpp"
#include "SceneBenchmark.hpp"
#include "Selection.hpp"
#include "format.hpp"
#include "DeviceCore/DevManager.h"
#include "Mcp/McpArguments.hpp"
#include "Mcp/McpPaths.hpp"
#include "Mcp/McpPersonContext.hpp"
#include "Mcp/McpRecorder.hpp"
#include "Mcp/McpTargets.hpp"
#include "Mcp/AutomationRuntime.hpp"
#include "Mcp/McpTools.hpp"
#ifdef SLIC3R_CAD
#include "CAD/DesignPanel.hpp"
#include "Mcp/McpToolsDesign.hpp"
#endif
#include "Mcp/RecordingSettings.hpp"
#include "Mcp/ScreenCapture.hpp"
#include "Mcp/UiInvoker.hpp"
#include "Mcp/VideoEncode.hpp"
#include "NotificationManager.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
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
#include <iterator>
#include <memory>
#include <mutex>
#include "slic3r/GUI/ActionRegistry.hpp"
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <vector>
#include <wx/app.h>
#include <wx/defs.h>
#include <wx/filedlg.h>
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/string.h>
#include <wx/utils.h>

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

using NativeCommands::refused;

// The action of `command` that collects a value whose call `args` is: the action's args and the value, under its
// input_arg (or its input where it has none), and nothing else; nullptr when none is.
const json* action_with_input(const NativeCommand& command, const json& args)
{
    if (!command.actions.is_array() || !args.is_object())
        return nullptr;
    for (const json& action : command.actions) {
        const std::string input = action.value("input", std::string());
        if (input.empty())
            continue;
        json rest = args;
        if (rest.erase(action.value("input_arg", input)) == 1 && rest == action.at("args"))
            return &action;
    }
    return nullptr;
}

// Arrange as the toolbar runs it: on every unlocked plate, in the background. No objects and a running
// job each get their own message.
AppActionRunResult arrange_all(Plater* plater)
{
    if (!plater)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first."), false};
    if (const Ops::Outcome<Ops::Done> started = Ops::start_arrange({}); !started)
        return refused(started.refusal(), AppActionRunResult::Level::Info);
    return {AppActionRunResult::Level::Succeeded};
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
        throw ToolFailure(redo ? "Nothing was redone in the project's history (nothing to put back, or a background job is running)"
                               : "Nothing was undone in the project's history (nothing to take back, or a background job is running)");
    return {AppActionRunResult::Level::Succeeded, {}, true, json{{"history", "project"}}.dump()};
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
    case AppActionRunResult::Level::Succeeded: return "success";
    case AppActionRunResult::Level::Error: return "error";
    case AppActionRunResult::Level::Busy: return "busy";
    default: return "info";
    }
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

// Why a recording ended when Stop Recording ended it; an agent's recording_stop reports it.
constexpr const char* kStoppedInApp = "stopped_in_app";

void notify_error(const wxString& text)
{
    if (Plater* plater = wxGetApp().plater(); plater != nullptr && !wxGetApp().is_closing())
        plater->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                                                               NotificationManager::NotificationLevel::ErrorNotificationLevel, into_u8(text));
}

// Encodes the recording at `path` to an MP4 beside it once its file is closed, and tells the person how that
// went. False when it cannot start: OrcaSlicer is closing, or the system cannot start a thread.
bool encode_when_closed(const boost::filesystem::path& path, const Mcp::RecordingSettings& settings)
{
    Mcp::AutomationRuntime* runtime = wxGetApp().automation_if_started();
    if (runtime == nullptr || wxGetApp().is_closing())
        return false;
    const auto done = [](const ToolResult& r) {
        if (r.is_error)
            // TRN %1% is why, in English.
            notify_error(format_wxstr(_L("The recording was saved, but could not be encoded: %1%"), r.message));
        else
            notify_saved(_L("Recording encoded"), Mcp::path_from_utf8(r.data.value("path", std::string())));
    };
    return runtime->recorder().encode_when_closed(*runtime, Mcp::encode_arguments(settings, Mcp::path_to_utf8(path)), done);
}

} // namespace

// The dialog shows the remembered settings and remembers what it starts with. Encoding needs an ffmpeg: the
// dialog says so, and the recording is then not encoded. Neither a run's end nor the server turning off ends
// the recording. Whatever does end it says where it went: Stop Recording itself, or the recording's `ended` for
// an agent's recording_stop, the limit or a write error; with encode, `ended` also encodes it.
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

    // Recordings are the automation runtime's, which with the server off starts without its endpoint. None while
    // OrcaSlicer closes.
    Mcp::AutomationRuntime* runtime = wxGetApp().automation();
    if (runtime == nullptr)
        return {};
    // The switch is remembered as the person left it; without an ffmpeg this recording is not encoded.
    settings.encode                = settings.encode && has_ffmpeg;
    Mcp::McpRecorder&     recorder = runtime->recorder();
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
    reply["jpeg_quality"] = settings.quality;
    reply["captions"]    = settings.captions;
    reply["pointer"]     = settings.pointer;
    reply["encode"]      = settings.encode;
    // TRN "Stop Recording" is the command that ends the recording.
    return {AppActionRunResult::Level::Succeeded, _L("Recording started. Stop Recording saves it."), true, reply.dump()};
}

AppActionRunResult NativeCommands::stop_recording_in_app()
{
    Mcp::AutomationRuntime* runtime = wxGetApp().automation_if_started();
    const nlohmann::json    state   = runtime != nullptr ? runtime->recorder().state() : nlohmann::json();
    if (state.is_null() || state.value("ending", false))
        return AppActionRunResult{AppActionRunResult::Level::Info, _L("No recording is running."), false};
    // What the window shows now, even if the timer has not ticked since, before the notification shows.
    runtime->recorder().capture_now();
    // The recording may be an agent's, a replay's or a script's; the person stopped it all the same, so they are told
    // where it went. Before it ends: a person's recording that is to be encoded says so instead, as it ends.
    const boost::filesystem::path path = Mcp::path_from_utf8(state["path"].get<std::string>());
    notify_saved(_L("Recording saved"), path);
    runtime->recorder().end(kStoppedInApp);
    return AppActionRunResult{AppActionRunResult::Level::Succeeded, {}, true, path_reply(path)};
}

namespace {

// Calibration wizards. Routes through MainFrame::run_calibration, the same entry point as the
// Calibration menu (which caches most of the wizard dialogs).
AppActionRunResult calib_command(CalibKind kind)
{
    MainFrame* mf = wxGetApp().mainframe;
    if (!mf)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first.")};
    ensure_3d_view(wxGetApp().plater());
    mf->run_calibration(kind);
    return {AppActionRunResult::Level::Succeeded};
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
    return {AppActionRunResult::Level::Succeeded, message};
}

// Thin AppAction wrapper for one catalog entry's own action, or one of its items: identity and presentation come from
// the catalog, run() runs the catalog's command, which outlives the AppAction. The id is keyed by the stable catalog
// key (not the display title), so a rename or a UI-language switch never re-keys it.
struct CommandAction : AppAction
{
    // A Speed Dial pick (ActionRegistry::run()): `args` is empty for a plain pick, or the value an action with an input
    // collects. The action's own arguments join them (action_call_args()), and NativeCommands::run() checks the result
    // against the command's schema.
    AppActionRunResult run(const json& args) const override
    {
        return NativeCommands::run(m_command, action_call_args(m_action.is_null() ? nullptr : &m_action, args));
    }

    // The command's own action, in its action_group, else its group. `title` is the command's title in the UI's
    // language. A Design command's takes the command's arguments, which its gizmo or dialog would otherwise ask for; any
    // other's takes none, as an item of actions with no arguments does, and runs the command as a person's pick does.
    CommandAction(const NativeCommand& c, std::string title)
        : CommandAction(c, c.key, std::move(title), c.title, c.action_group.empty() ? c.group : c.action_group)
    {
        this->input                   = c.input;
        this->icon                    = c.icon;
        this->automation_description  = c.description;
        this->input_schema            = c.input_schema;
        this->completes_in_background = c.completion == CommandCompletion::Deferred;
        if (c.group != kDesignGroup) {
            this->input_schema = Mcp::object_schema({});
            this->preset       = json::object();
        }
    }

    // `action`, an item of `c`'s actions, in its group, else the command's. `title` is the action's title in the UI's
    // language.
    CommandAction(const NativeCommand& c, const json& action, std::string title)
        : CommandAction(c, action.at("key").get<std::string>(), std::move(title), action.at("title").get<std::string>(),
                        action.value("group", c.group))
    {
        m_action                     = action;
        this->icon                   = action.at("icon").get<std::string>();
        this->automation_description = action.at("description").get<std::string>();
        this->preset                 = action.at("args");
        this->input                  = action.value("input", std::string());
        this->input_schema           = Mcp::object_schema({});
        // The value the Speed Dial's second phase collects, typed as the command's argument it goes to.
        if (!this->input.empty()) {
            const std::string to       = action.value("input_arg", this->input);
            const json&       property = c.input_schema.at("properties").at(to);
            this->input_schema         = {{"type", "object"},
                                          {"properties", {{this->input, property}}},
                                          {"required", json::array({this->input})},
                                          {"additionalProperties", false}};
        }
    }

private:
    // Under id `key`, with the English `title_en`, in the Speed Dial section `group_en`.
    CommandAction(const NativeCommand& c, const std::string& key, std::string title, std::string title_en, std::string group_en)
        : AppAction(AppActionId{AppAction::compose_id(kCommandPrefix, key, kOrcaSourceKey)}, std::move(title), kOrcaSourceKey,
                    kOrcaSourceName)
        , m_command(c)
    {
        this->kind     = AppActionKind::Command;
        this->group    = I18N::translate_utf8(group_en);
        this->title_en = std::move(title_en);
        this->group_en = std::move(group_en);
        this->command  = c.key;
    }

    const NativeCommand& m_command;
    json                 m_action; // the item of m_command's actions; null for its own action
};

// The Preferences dialog's tabs, by the names window_open's tab gives them.
constexpr std::pair<const char*, PreferencesTab> kPreferencesTabs[] = {{"general", PreferencesTab::General},
                                                                     {"files", PreferencesTab::Files},
                                                                     {"behaviour", PreferencesTab::Behaviour},
                                                                     {"control", PreferencesTab::Control},
                                                                     {"graphics", PreferencesTab::Graphics},
                                                                     {"online", PreferencesTab::Online},
                                                                     {"developer", PreferencesTab::Developer}};

} // namespace

// ---- The menu items that are actions of a command (NativeCommandMenuItems.hpp) ----

json MenuItems::input_file(const char* what) { return Mcp::prop("string", std::string("Absolute path of ") + what + "."); }

AppActionRunResult MenuItems::load_project()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    plater->load_project();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::save_project_as()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    return NativeCommands::project_save_result(plater->save_project(true));
}

AppActionRunResult MenuItems::export_gcode()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    plater->export_gcode(false);
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::export_stl()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    plater->export_stl();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::export_3mf()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    plater->export_core_3mf();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::export_sliced_file(bool all_plates)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    plater->export_gcode_3mf(all_plates);
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::calib(CalibKind kind) { return calib_command(kind); }

// ---- Object ----
AppActionRunResult MenuItems::obj_arrange() { return arrange_all(wxGetApp().plater()); }

// ---- Add Primitive ---- (the Add > Add Primitive submenu's Text and SVG)
namespace {

// The part type object_add_text and object_add_svg pass where the action passes ModelVolumeType::INVALID, which adds a
// part to the selected object, or a new object when nothing is selected. A call that names an object selects it first,
// as a click would, and adds a part of `type` to it (a part when left out); `type` alone adds to the selected object and
// is refused with nothing selected or with parts of several objects selected, where the action would make a new object,
// which has no part type. The new part lands on one of the selected solid parts (find_closest() in EmbossJob.cpp), so
// both select the object whole first: a selected negative part or modifier alone would leave none, and a new object.
Ops::Outcome<ModelVolumeType> part_type(Plater& plater, const json& args)
{
    const bool named = Mcp::names_targets(args);
    if (!named && !args.contains("type"))
        return ModelVolumeType::INVALID;
    const std::string type = args.value("type", std::string("part"));
    Ops::Targets      object;
    if (named) {
        const Ops::Outcome<Ops::Targets> resolved = Mcp::resolve_targets(plater.model(), nullptr, args, Mcp::kObjects, true);
        if (!resolved)
            return resolved.refusal();
        object = resolved.value();
    } else {
        const Selection& selection = Ops::shown_prepare_canvas().get_selection();
        if (selection.is_empty())
            return Ops::nothing_selected();
        if (selection.get_object_idx() < 0)
            return Ops::one_object_only();
        object.objects = {selection.get_object_idx()};
    }
    {
        // Adding the part is the call's undo step; selecting its object is not a step of its own.
        Plater::SuppressSnapshots quiet(&plater);
        Ops::select_targets(object);
    }
    return type == "negative" ? ModelVolumeType::NEGATIVE_VOLUME :
           type == "modifier" ? ModelVolumeType::PARAMETER_MODIFIER :
                                ModelVolumeType::MODEL_PART;
}

} // namespace

AppActionRunResult MenuItems::add_primitive_text(const json& args)
{
    Plater* plater = wxGetApp().plater();
    if (plater) {
        ensure_3d_view(plater);
        const Ops::Outcome<ModelVolumeType> type = part_type(*plater, args);
        if (!type)
            return refused(type.refusal());
        if (GLCanvas3D* canvas = plater->canvas3D())
            canvas->clear_popup_menu_position();
        MenuFactory::add_text_volume(type.value());
    }
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::add_primitive_svg(const json& args)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    ensure_3d_view(plater);
    GLCanvas3D* canvas = plater->canvas3D();
    if (canvas != nullptr)
        canvas->clear_popup_menu_position();
    const Ops::Outcome<ModelVolumeType> type = part_type(*plater, args);
    if (!type)
        return refused(type.refusal());
    // Without a path the file dialog asks for the file, as the action does; the object and type named still apply.
    if (!args.contains("path")) {
        MenuFactory::add_svg_volume(type.value());
        return AppActionRunResult{AppActionRunResult::Level::Succeeded};
    }
    // What the dialog path does once a file is picked: a new object with nothing selected, else
    // a part of the selected object.
    AppActionRunResult fail;
    const auto         path = path_argument(args, false, fail);
    if (!path)
        return fail;
    auto* svg = canvas != nullptr ? dynamic_cast<GLGizmoSVG*>(canvas->get_gizmos_manager().get_gizmo(GLGizmosManager::Svg)) : nullptr;
    const ModelVolumeType volume_type = type.value() == ModelVolumeType::INVALID ? ModelVolumeType::MODEL_PART : type.value();
    if (svg == nullptr || !svg->create_volume(into_u8(from_path(*path)), volume_type))
        return AppActionRunResult{AppActionRunResult::Level::Error, _L("The SVG file could not be added.")};
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

// ---- Add Handy models ---- (the Add > Add Handy models submenu)
AppActionRunResult MenuItems::add_handy(std::size_t index)
{
    if (Plater* plater = wxGetApp().plater())
        ensure_3d_view(plater);
    MenuFactory::load_handy_model(index);
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

// ---- Plate ----
AppActionRunResult MenuItems::plate_add()
{
    Plater* plater = wxGetApp().plater();
    if (!is_fff_plater(plater))
        return plate_unavailable();
    if (!plater->can_add_plate())
        return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot add another plate (maximum reached).")};
    plater->add_plate();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

// ---- Import ----
AppActionRunResult MenuItems::import_file()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
#ifdef __APPLE__
    plater->add_model();
#else
    plater->add_file();
#endif
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::import_zip_archive()
{
    if (Plater* plater = wxGetApp().plater())
        plater->import_zip_archive();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

// ---- Export extras ----
AppActionRunResult MenuItems::export_stl_multi()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    if (const std::optional<Ops::Refusal> refusal = Ops::export_objects_refusal("stl", {}, true, plater->model()))
        return refused(*refusal, AppActionRunResult::Level::Info);
    plater->export_stl(false, false, true);
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::export_drc_single()
{
    if (Plater* plater = wxGetApp().plater())
        plater->export_stl(false, false, false, FT_DRC);
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::export_drc_multi()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    if (const std::optional<Ops::Refusal> refusal = Ops::export_objects_refusal("drc", {}, true, plater->model()))
        return refused(*refusal, AppActionRunResult::Level::Info);
    plater->export_stl(false, false, true, FT_DRC);
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::export_toolpaths_obj()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    if (const std::optional<Ops::Refusal> refusal = Ops::toolpaths_refusal())
        return refused(*refusal, AppActionRunResult::Level::Info);
    plater->export_toolpaths_to_obj();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

// ---- Presets and settings ----
AppActionRunResult MenuItems::import_configs(const json& args)
{
    MainFrame* mf = wxGetApp().mainframe;
    if (mf == nullptr)
        return AppActionRunResult{};
    if (Mcp::person_form(args)) {
        mf->load_config_file();
        return AppActionRunResult{AppActionRunResult::Level::Succeeded};
    }
    AppActionRunResult fail;
    const auto         path = path_argument(args, false, fail);
    if (!path)
        return fail;
    const bool        overwrite = args.value("on_conflict", std::string("skip")) == "overwrite";
    const std::string file      = Mcp::path_to_utf8(*path);
    if (is_gcode_file(file)) {
        if (!mf->load_config_file(file))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("The configuration was not loaded.")};
        return AppActionRunResult{AppActionRunResult::Level::Succeeded};
    }
    for (const char* ext : {".json", ".zip", ".orca_printer", ".orca_filament", ".orca_bundle"})
        if (boost::iends_with(file, ext))
            return import_preset_file(*path, overwrite);
    return AppActionRunResult{AppActionRunResult::Level::Error,
                              "Not a preset file: give a .json, .zip, .orca_printer, .orca_filament, .orca_bundle or .gcode"};
}

AppActionRunResult MenuItems::export_config(const json& args)
{
    if (Mcp::person_form(args)) {
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->export_config();
        return AppActionRunResult{AppActionRunResult::Level::Succeeded};
    }
    AppActionRunResult fail;
    const auto         dir = dir_argument(args, fail);
    if (!dir)
        return fail;
    // The dialog MainFrame::export_config() opens, given the folder its OK writes to.
    ExportConfigsDialog dlg(nullptr, from_path(*dir));
    if (dlg.ShowModal() != wxID_OK)
        return AppActionRunResult{AppActionRunResult::Level::Error, _L("Nothing was exported.")};
    return AppActionRunResult{AppActionRunResult::Level::Succeeded, from_path(*dir)};
}

AppActionRunResult MenuItems::migrate_filament_presets()
{
    wxGetApp().check_legacy_user_filament_presets(true);
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::set_mode(const std::string& mode)
{
    static const std::pair<const char*, ConfigOptionMode> modes[] = {{"simple", comSimple}, {"advanced", comAdvanced}, {"expert", comExpert}};
    for (const auto& [name, value] : modes)
        if (mode == name) {
            wxGetApp().set_mode(value);
            return AppActionRunResult{AppActionRunResult::Level::Succeeded};
        }
    return AppActionRunResult{AppActionRunResult::Level::Error, from_u8("mode is simple, advanced or expert"), false};
}

// What Preferences > Developer > Developer mode does: flip the flag, keep it, refresh the UI.
AppActionRunResult MenuItems::toggle_developer_mode()
{
    GUI_App&   app = wxGetApp();
    const bool on  = !app.app_config->get_bool("developer_mode");
    app.app_config->set_bool("developer_mode", on);
    app.app_config->save();
    app.update_mode();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded, on ? _L("Developer mode enabled.") : _L("Developer mode disabled.")};
}

// ---- The app ----
// The Design tab's history is "design", else "project". Without args.history it is the shown tab's, as the Edit menu's
// Undo and Redo choose it. The reply says which history it was and, for the Design tab's, what Mcp::design_undo_redo()
// replies.
AppActionRunResult MenuItems::undo_redo(const json& args, bool redo)
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
        json reply{{"history", "design"}, {"result", Mcp::design_undo_redo(redo)}};
        return {AppActionRunResult::Level::Succeeded, {}, true, reply.dump()};
#else
        throw ToolFailure("The CAD feature is off: this build of OrcaSlicer has no Design tab");
#endif
    }
    return project_undo_redo(redo, /*agent=*/true);
}

// Through the function the menu calls. It shows nothing: where the menu's item would be greyed it does nothing, and
// otherwise the Design tab's status line or the canvas shows what changed, as after the menu's.
AppActionRunResult MenuItems::picked_undo_redo(const json& args, bool redo)
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
        return {AppActionRunResult::Level::Succeeded, {}, true, json{{"history", "design"}}.dump()};
    }
#else
    // A build without CAD has no Design tab, so no history of it to take back.
    if (history == "design")
        return greyed;
#endif
    // Plater::undo() and redo() act wherever they are called from; the menu's item is what keeps them to the 3D
    // view with no job running.
    const Plater* plater = wxGetApp().plater();
    if (plater == nullptr || !(redo ? plater->can_redo() : plater->can_undo()))
        return greyed;
    return project_undo_redo(redo, /*agent=*/false);
}

AppActionRunResult MenuItems::open_web_page(const std::string& page)
{
    if (page == "wiki")
        wxLaunchDefaultBrowser("https://www.orcaslicer.com/wiki/", wxBROWSER_NEW_WINDOW);
    else if (page == "youtube")
        wxLaunchDefaultBrowser("https://www.youtube.com/@OfficialOrcaSlicer/videos", wxBROWSER_NEW_WINDOW);
    else if (page == "plugin_hub")
        open_plugin_hub();
    else
        return AppActionRunResult{AppActionRunResult::Level::Error, from_u8("page is wiki, youtube or plugin_hub"), false};
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::check_for_updates()
{
    wxGetApp().check_new_version_sf(true, 1);
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::show_config_folder()
{
    Slic3r::GUI::desktop_open_datadir_folder();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::benchmark_3d_scene()
{
    run_scene_benchmark();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

// ---- View and windows ----
AppActionRunResult MenuItems::toggle_perspective()
{
    if (Plater* plater = wxGetApp().plater())
        plater->get_camera().select_next_type();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::reset_window_layout()
{
    if (Plater* plater = wxGetApp().plater())
        plater->reset_window_layout();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

std::vector<std::string> MenuItems::preferences_tabs()
{
    std::vector<std::string> names;
    for (const auto& [name, tab] : kPreferencesTabs)
        names.emplace_back(name);
    return names;
}

AppActionRunResult MenuItems::open_window(const std::string& window, const json& args)
{
    GUI_App& app = wxGetApp();
    if (window == "preferences") {
        if (Mcp::person_form(args)) {
            app.open_preferences();
            return AppActionRunResult{AppActionRunResult::Level::Succeeded};
        }
        // The tab to show, and the preference whose control is scrolled into view and focused.
        const std::string tab = args.value("tab", std::string());
        const auto it = std::find_if(std::begin(kPreferencesTabs), std::end(kPreferencesTabs), [&](const auto& t) { return tab == t.first; });
        if (it == std::end(kPreferencesTabs))
            return AppActionRunResult{AppActionRunResult::Level::Error,
                                      from_u8("tab is required: general, files, behaviour, control, graphics, online or developer")};
        app.open_preferences(it->second, args.value("key", std::string()));
    } else if (window == "plugins") {
        app.open_plugins_dialog();
    } else if (window == "action_replay") {
        if (MainFrame* mf = app.mainframe)
            mf->show_action_replay();
    } else if (window == "preset_bundle") {
        app.open_presetbundledialog();
    } else if (window == "keyboard_shortcuts") {
        app.keyboard_shortcuts(ShortcutContext::Global);
    } else if (window == "setup_wizard") {
        app.ShowUserGuide();
    } else if (window == "network_test") {
        NetworkTestDialog dlg(app.mainframe);
        dlg.ShowModal();
    } else if (window == "troubleshoot") {
        app.troubleshoot();
    } else if (window == "tip_of_the_day") {
        if (Plater* plater = app.plater()) {
            plater->get_dailytips()->open();
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->set_as_dirty();
        }
    } else if (window == "about") {
        Slic3r::GUI::about();
    } else {
        return AppActionRunResult{AppActionRunResult::Level::Error, from_u8("Unknown window \"" + window + "\""), false};
    }
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

// A PNG of the main window, saved where the person's recordings go, as the MCP screenshot tool's default.
AppActionRunResult MenuItems::take_screenshot()
{
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
    return AppActionRunResult{AppActionRunResult::Level::Succeeded, {}, true, path_reply(path)};
}

// ---- Filaments, printers and plugins ----
AppActionRunResult MenuItems::sync_ams()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return AppActionRunResult{};
    const std::optional<Ops::Refusal> refusal = plater->sidebar().sync_ams_list();
    if (!refusal)
        return AppActionRunResult{AppActionRunResult::Level::Succeeded};
    // With no printer the menu item says so; otherwise the warning it showed said why.
    if (std::string(refusal->code) == "no_printer")
        return refused(*refusal, AppActionRunResult::Level::Info);
    return Mcp::quiet_refusal(*refusal);
}

// The Print button's Send for the selected printer: the dialog that sends to a Bambu printer's storage, or the Send
// G-code dialog of a print host, then the upload queue. Where the button offers no such send, it says why instead of
// doing nothing.
AppActionRunResult MenuItems::send_gcode(const json& args)
{
    if (wxGetApp().plater() == nullptr)
        return AppActionRunResult{};
    Ops::OpenPrintDialogArgs op;
    op.send = true;
    if (const std::string err = Mcp::read_print_plates(args, op.plate, op.all); !err.empty())
        return AppActionRunResult{AppActionRunResult::Level::Error, from_u8(err)};
    if (const Ops::Outcome<Ops::Done> opened = Ops::open_print_dialog(op); !opened)
        return refused(opened.refusal());
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

AppActionRunResult MenuItems::install_local_plugin(const json& args)
{
    MainFrame* mf = wxGetApp().mainframe;
    if (mf == nullptr)
        return AppActionRunResult{};
    boost::filesystem::path package;
    // The person's form asks before it replaces a plugin installed already; a call with a path is told by overwrite.
    std::optional<bool> overwrite;
    if (Mcp::person_form(args)) {
        wxFileDialog dialog(mf, _L("Select plugin package"), wxEmptyString, wxEmptyString, _L("Plugin files (*.py;*.whl)|*.py;*.whl"),
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK)
            return AppActionRunResult{AppActionRunResult::Level::Succeeded};
        package = into_path(dialog.GetPath());
    } else {
        AppActionRunResult fail;
        const auto         path = path_argument(args, false, fail);
        if (!path)
            return fail;
        package   = *path;
        overwrite = args.value("overwrite", false);
    }
    wxString                    message;
    std::optional<Ops::Refusal> replace_refused;
    const bool                  installed = install_local_plugin_package(package, mf, message, overwrite, &replace_refused);
    wxGetApp().update_plugins_dialog();
    if (replace_refused)
        return refused(*replace_refused, AppActionRunResult::Level::Error);
    if (message.IsEmpty()) // the overwrite prompt was cancelled
        return AppActionRunResult{AppActionRunResult::Level::Info, _L("The plugin was not installed."), false};
    return AppActionRunResult{installed ? AppActionRunResult::Level::Succeeded : AppActionRunResult::Level::Error, message};
}

AppActionRunResult MenuItems::refresh_plugins()
{
    wxGetApp().refresh_plugins();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

// ---- Scripts ----
AppActionRunResult MenuItems::toggle_run_script_panel()
{
    if (wxGetApp().mainframe != nullptr)
        wxGetApp().run_script_panel().toggle();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

// ---- Agent Activity ----
AppActionRunResult MenuItems::toggle_agent_activity_panel()
{
    if (wxGetApp().mainframe != nullptr)
        wxGetApp().agent_activity_panel().toggle();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

namespace {

// The headings of the places, in the order tools/list gives them.
constexpr const char* kToolsAndActions = "Tools and actions";
constexpr const char* kTheApp          = "The app";
constexpr const char* kTheProject      = "The project";
constexpr const char* kObjects         = "Objects";
constexpr const char* kParts           = "Parts";
constexpr const char* kPlates          = "Plates";
constexpr const char* kPresets         = "Presets and settings";
constexpr const char* kFilaments       = "Filaments";
constexpr const char* kPrinters        = "Printers and prints";
constexpr const char* kPlugins         = "Plugins";
constexpr const char* kViewAndWindows  = "View and windows";
constexpr const char* kRecordings      = "Recordings, replays and scripts";
constexpr const char* kDrivingTheGui   = "Driving the GUI";
constexpr const char* kHeadingOrder[]  = {kToolsAndActions, kTheApp,     kTheProject,     kObjects,   kParts,
                                          kPlates,          kPresets,    kFilaments,      kPrinters,  kPlugins,
                                          kViewAndWindows,  kRecordings, kDrivingTheGui};

// The place a command's first word names, by that word: a name is its place, then its verb. undo and redo, shared with
// the Design tab, have no place word: they are the app's. A preference is kept by the app, not by a preset or a
// project, so preferences is the app's too, and calibration builds a test plate's project, so it is the plates'.
constexpr std::pair<const char*, const char*> kPlaceWords[] = {
    {"tool", kToolsAndActions},  {"action", kToolsAndActions}, {"app", kTheApp},
    {"job", kTheApp},            {"preferences", kTheApp},     {"undo", kTheApp},
    {"redo", kTheApp},           {"project", kTheProject},     {"object", kObjects},
    {"part", kParts},            {"plate", kPlates},           {"calibration", kPlates},
    {"preset", kPresets},        {"settings", kPresets},       {"filament", kFilaments},
    {"printer", kPrinters},      {"print", kPrinters},         {"plugin", kPlugins},
    {"view", kViewAndWindows},   {"window", kViewAndWindows},  {"widget", kDrivingTheGui},
    {"keyboard", kDrivingTheGui}, {"mouse", kDrivingTheGui},   {"dialog", kDrivingTheGui},
    {"webview", kDrivingTheGui}, {"gizmo", kDrivingTheGui},    {"recording", kRecordings},
    {"replay", kRecordings},     {"script", kRecordings}};

// The commands grouped apart from their first word, looked up before it: a screenshot is a recording's still, so the
// agent's capture and the person's menu item are with the recordings, not with the windows.
constexpr std::pair<const char*, const char*> kPlaceExceptions[] = {{"window_screenshot", kRecordings},
                                                                    {"window_save_screenshot", kRecordings}};

// Where `command`, in its place's group, comes in tools/list: the listing of the other commands, the manual, the
// generic call and the app's state first, as what an agent starts with, then the places in kHeadingOrder.
std::ptrdiff_t list_rank(const NativeCommand& command)
{
    static const char* const kFirst[] = {"tool_list", "tool_describe", "tool_call", "app_state"};
    if (const auto first = std::find(std::begin(kFirst), std::end(kFirst), command.key); first != std::end(kFirst))
        return first - std::begin(kFirst);
    const auto place = std::find_if(std::begin(kHeadingOrder), std::end(kHeadingOrder),
                                    [&command](const char* heading) { return command.group == heading; });
    return std::ptrdiff_t(std::size(kFirst)) + (place - std::begin(kHeadingOrder));
}

} // namespace

const char* NativeCommands::place_heading(const std::string& key)
{
    for (const auto& [name, heading] : kPlaceExceptions)
        if (key == name)
            return heading;
    const std::string word = key.substr(0, key.find('_'));
    for (const auto& [first, heading] : kPlaceWords)
        if (word == first)
            return heading;
    return nullptr;
}

std::vector<std::string> NativeCommands::place_headings() { return {std::begin(kHeadingOrder), std::end(kHeadingOrder)}; }

std::vector<std::pair<std::string, std::string>> NativeCommands::place_exceptions()
{
    return {std::begin(kPlaceExceptions), std::end(kPlaceExceptions)};
}

void NativeCommands::register_all_commands(CommandCatalog& catalog, [[maybe_unused]] const CatalogOptions& options)
{
    // The places' commands are gathered first, so that each takes its place's group and its place in the order
    // (list_rank()); a stable sort keeps a place's commands in the order its file adds them.
    // The Design tab's commands, only while the CAD feature is on, go before the GUI-driving ones; tool_list's
    // description names them then, and undo's and redo's examples show the Design tab's history.
    std::string design_index;
#ifdef SLIC3R_CAD
    if (options.cad)
        design_index = Mcp::design_command_index();
#endif
    // The places register into a catalog of their own, which keeps only their commands for the sort: a source or a
    // left-out record added to it would be lost with it. So the plugin operations' source is added to `catalog` apart
    // (register_plugin_operations()), as are the Design commands and their left-out records.
    CommandCatalog places;
    Mcp::register_catalog_tools(places, design_index);
    Mcp::register_app_tools(places, options.cad);
    Mcp::register_project_tools(places);
    Mcp::register_object_tools(places);
    Mcp::register_plate_tools(places);
    Mcp::register_preset_tools(places);
    Mcp::register_filament_tools(places);
    Mcp::register_printer_tools(places);
    Mcp::register_plugin_tools(places);
    Mcp::register_view_tools(places);
    Mcp::register_recording_tools(places);
    Mcp::register_gui_tools(places);

    std::vector<NativeCommand> placed(places.commands().begin(), places.commands().end());
    for (NativeCommand& command : placed) {
        const char* heading = place_heading(command.key);
        if (heading == nullptr)
            throw std::logic_error("The command " + command.key + " has no place");
        command.group = heading;
    }
    std::stable_sort(placed.begin(), placed.end(),
                     [](const NativeCommand& a, const NativeCommand& b) { return list_rank(a) < list_rank(b); });

    Mcp::register_plugin_operations(catalog);
    const auto gui = std::find_if(placed.begin(), placed.end(), [](const NativeCommand& c) { return c.group == kDrivingTheGui; });
    for (auto it = placed.begin(); it != gui; ++it)
        catalog.add(std::move(*it));
#ifdef SLIC3R_CAD
    if (options.cad)
        Mcp::register_design_tools(catalog);
    else
        Mcp::leave_out_design_tools(catalog, options.editor);
#endif
    for (auto it = gui; it != placed.end(); ++it)
        catalog.add(std::move(*it));
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

json NativeCommands::action(std::string key, std::string title, std::string group, std::string icon, json args, std::string description,
                            std::string input, std::string input_arg)
{
    json item{{"key", std::move(key)},   {"title", std::move(title)},
              {"icon", std::move(icon)}, {"description", std::move(description)}, {"args", std::move(args)}};
    // Without a group of its own the item is in its command's section.
    if (!group.empty())
        item["group"] = std::move(group);
    if (!input.empty())
        item["input"] = std::move(input);
    if (!input_arg.empty())
        item["input_arg"] = std::move(input_arg);
    return item;
}

std::vector<NativeCommands::SpeedDialAction> NativeCommands::speed_dial_actions(const CommandCatalog& catalog)
{
    std::vector<SpeedDialAction> out;
    for (const NativeCommand& c : catalog.commands()) {
        // The Design tab's actions are there only while the tab is shown (ActionRegistry::materialize_design_actions()).
        if (c.group == kDesignGroup)
            continue;
        if (c.has_own_action)
            out.push_back({&c, nullptr});
        if (c.actions.is_array())
            for (const json& item : c.actions)
                out.push_back({&c, &item});
    }
    return out;
}

namespace {

// `title`, a title of `command` or of one of its actions, in the UI's language.
wxString translated_title(const NativeCommand& command, const std::string& title)
{
    const char* context = title_context(command);
    return context != nullptr ? I18N::translate(title, context) : I18N::translate(title);
}

} // namespace

wxString NativeCommands::action_title(const NativeCommand& command) { return translated_title(command, command.title); }

std::unique_ptr<AppAction> NativeCommands::make_action(const NativeCommand& command)
{
    return std::make_unique<CommandAction>(command, into_u8(action_title(command)));
}

std::unique_ptr<AppAction> NativeCommands::make_action(const NativeCommand& command, const json& command_action)
{
    return std::make_unique<CommandAction>(command, command_action,
                                           into_u8(translated_title(command, command_action.at("title").get<std::string>())));
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
    c.run          = [key = c.key, fn = std::move(fn), completion](const json& args, ToolContext& ctx) {
        if (ctx.person()) {
            AppActionRunResult shown = fn(args);
            ToolResult         reply = from_run_result(key, shown);
            reply.notice             = std::make_shared<const AppActionRunResult>(std::move(shown));
            return reply;
        }
        // An export given its file holds the project from before it starts, and the call waits for the file.
        const bool         exports = completion == CommandCompletion::Deferred && !Mcp::person_form(args);
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
        ToolResult reply = from_run_result(key, result);
        if (!exports) {
            Mcp::settle(ctx);
            return reply;
        }
        if (reply.is_error)
            return reply;
        if (ticket.seq_after == ticket.seq_before)
            return ToolResult::error(result.message.empty() ? "The export did not start." : into_u8(result.message));
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
    c.run          = [key = c.key, fn = std::move(fn), inline_call](const json& args, ToolContext& ctx) {
        // A person's pick is on the UI thread, which a long function must not hold.
        if (ctx.person() && !inline_call)
            return ctx.submit(key, args);
        return fn(args, ctx);
    };
    return c;
}

AppActionRunResult NativeCommands::refused(const Ops::Refusal& refusal, AppActionRunResult::Level level)
{
    AppActionRunResult result{level, refusal.translated(), false};
    result.refusal = std::make_shared<const Ops::Refusal>(refusal);
    return result;
}

NativeCommand NativeCommands::with_person_form(NativeCommand tool, std::function<AppActionRunResult(const json& action_args)> person,
                                               bool person_form_behind_dialogs)
{
    tool.input_schema = Mcp::with_silent(std::move(tool.input_schema), Mcp::person_forms(tool));
    return with_menu_item_forms(std::move(tool), std::move(person), person_form_behind_dialogs);
}

NativeCommand NativeCommands::with_menu_item_forms(NativeCommand tool, std::function<AppActionRunResult(const json& action_args)> person,
                                             bool person_form_behind_dialogs, std::vector<json> own_forms)
{
    // The actions a call's arguments are matched against, kept apart from the tool this returns.
    NativeCommand forms;
    forms.has_own_action = tool.has_own_action;
    forms.actions        = tool.actions;
    // The person's form runs as a ui_command() function does: in place for a person's pick, in one UI closure
    // for an automated call, whose reply then says what the pick showed. It is given the action's arguments, {} for
    // the tool's own action.
    const auto given = [](const json& args) { return Mcp::person_form(args) ? json::object() : args; };
    const CommandFn asks = ui_command(tool.key, tool.title, tool.group, Mcp::object_schema({}),
                                      [person, given](const json& args) { return person(given(args)); })
                               .run;
    const bool checked = !person_form_behind_dialogs && tool.guard != GuardClass::Mutate;
    tool.run = [key = tool.key, forms = std::move(forms), own_forms = std::move(own_forms), person = std::move(person), given, asks,
                checked, silent = std::move(tool.run)](const json& args, ToolContext& ctx) {
        const bool own = std::find(own_forms.begin(), own_forms.end(), given(args)) != own_forms.end();
        // A person's pick of an action whose menu item is the tool's own function, or of an action that collects a value,
        // runs that function in place, as a menu item does: no job.
        if (ctx.person() && (own || action_with_input(forms, args) != nullptr))
            return asks(args, ctx);
        if (own || !Mcp::person_form(forms, args))
            return silent(args, ctx);
        if (!checked || ctx.person())
            return asks(args, ctx);
        // An agent's call of a tool whose guard lets it run behind dialogs: the person's form opens what a pick
        // opens, so it is refused while a modal dialog is open or another call's nested loop runs, in the UI
        // closure that runs it.
        Mcp::UiInvoker& ui = ctx.runtime().ui();
        return ui_command(key, {}, {}, Mcp::object_schema({}), [&person, &ui, &given](const json& action_args) {
                   ui.require_mutate();
                   return person(given(action_args));
               }).run(args, ctx);
    };
    return tool;
}

ToolResult NativeCommands::from_run_result(const std::string& key, const AppActionRunResult& result)
{
    // An operation's refusal is worded once, in English here as in a tool's reply.
    const std::string message = result.refusal ? result.refusal->english() : into_u8(result.message);
    // A command that did nothing (no selection, no objects) fails here, though the Speed Dial shows it as
    // information.
    if (result.level == AppActionRunResult::Level::Error || result.level == AppActionRunResult::Level::Busy || !result.done) {
        json details;
        if (result.refusal) {
            details = {{"code", result.refusal->code}};
            if (!result.refusal->names.empty())
                details["names"] = result.refusal->names;
            // The message is the person's; the hint is what an agent's call does instead.
            if (std::string hint = Mcp::refusal_hint(*result.refusal); !hint.empty())
                details["hint"] = std::move(hint);
        }
        return ToolResult::error(message.empty() ? "The action failed" : message, std::move(details));
    }
    json out{{"ran", key}, {"level", level_name(result.level)}};
    if (!message.empty())
        out["message"] = message;
    if (!result.reply.empty())
        if (json reply = json::parse(result.reply, nullptr, /*allow_exceptions=*/false); reply.is_object())
            out.update(reply);
    return ToolResult::ok(std::move(out));
}

AppActionRunResult NativeCommands::run(const std::string& key, const json& args)
{
    if (wxGetApp().is_closing())
        return {};
    const NativeCommand* command = find(key);
    if (command == nullptr)
        return {AppActionRunResult::Level::Info, _L("Unknown command.")};
    return run(*command, args);
}

AppActionRunResult NativeCommands::run(const NativeCommand& command, const json& args)
{
    const auto failed = [](const std::string& why) { return AppActionRunResult{AppActionRunResult::Level::Error, from_u8(why), false}; };
    json       checked = args;
    if (const std::string err = Mcp::argument_error(command, checked); !err.empty())
        return failed(err);
    Mcp::PersonContext person;
    try {
        const ToolResult reply = command.run(checked, person);
        if (reply.notice)
            return *reply.notice;
        if (reply.is_error)
            return failed(reply.message);
        return {AppActionRunResult::Level::Succeeded};
    } catch (const ToolFailure& failure) {
        return failed(failure.what());
    } catch (const std::exception& e) {
        // The log names the command, which the application's handler does not know.
        BOOST_LOG_TRIVIAL(error) << "The command " << command.key << " threw: " << e.what();
        throw;
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

AppActionRunResult NativeCommands::project_save_result(int answer)
{
    if (answer == wxID_YES)
        return AppActionRunResult{AppActionRunResult::Level::Succeeded};
    AppActionRunResult not_saved{AppActionRunResult::Level::Info, {}, false};
    not_saved.refusal = std::make_shared<const Ops::Refusal>(Ops::project_not_saved());
    return not_saved;
}

bool NativeCommands::recording_active()
{
    Mcp::AutomationRuntime* runtime = wxGetApp().automation_if_started();
    return runtime != nullptr && runtime->recorder().active();
}

}} // namespace Slic3r::GUI

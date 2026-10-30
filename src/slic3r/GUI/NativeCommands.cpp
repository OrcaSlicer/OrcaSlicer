#include "NativeCommands.hpp"

#include "calib_dlg.hpp"
#include "Camera.hpp"
#include "CreatePresetsDialog.hpp"
#include "DailyTips.hpp"
#include "GCodeViewer.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "GUI_ObjectList.hpp"
#include "Gizmos/GLGizmoSVG.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "I18N.hpp"
#include "Shortcuts.hpp"
#include "IMSlider.hpp"
#include "Jobs/Job.hpp"
#include "Jobs/Worker.hpp"
#include "MainFrame.hpp"
#include "ObjectDataViewModel.hpp"
#include "NetworkTestDialog.hpp"
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
#include "Mcp/McpRecorder.hpp"
#include "Mcp/McpService.hpp"
#include "Mcp/McpTypes.hpp"
#include "Mcp/RecordingSettings.hpp"
#include "Mcp/ScreenCapture.hpp"
#include "Mcp/VideoEncode.hpp"
#include "NotificationManager.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/constants.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <initializer_list>
#include <libslic3r/Model.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include "libslic3r/libslic3r.h"
#include <memory>
#include "slic3r/GUI/ActionRegistry.hpp"
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#include <vector>
#include <wx/dataview.h>
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/string.h>
#include <wx/utils.h>

class wxWindow;

namespace Slic3r { namespace GUI {

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

// Arrange and orient as the toolbar runs them: on every plate, in the background. Plater::can_arrange()
// is false with no objects or while another job runs, and each gets its own message.
AppActionRunResult arrange_op(Plater* plater, void (*op)(Plater*), const wxString& no_objects)
{
    if (!plater)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first."), false};
    ensure_3d_view(plater);
    if (plater->model().objects.empty())
        return {AppActionRunResult::Level::Info, no_objects, false};
    if (!plater->get_ui_job_worker().is_idle())
        return {AppActionRunResult::Level::Info, _L("Wait for the running background job to finish."), false};
    // A plate's Arrange or Auto Rotate icon leaves the plate-only state behind.
    plater->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
    op(plater);
    return {AppActionRunResult::Level::Success};
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

// Jump the preview to a layer selected by a 0-100 percent of the layer range. The caller has already
// switched to Preview (which may request a slice); if a slicer result is present the slider is
// repositioned immediately, otherwise the jump is a no-op until the user re-slices.
void go_to_layer(Plater* plater, const std::string& param)
{
    if (!plater)
        return;
    double pct = 50.0;
    try {
        pct = std::stod(param);
    } catch (const std::exception&) {}
    pct = std::clamp(pct, 0.0, 100.0);

    GLCanvas3D* canvas = plater->get_current_canvas3D();
    if (!canvas)
        return;
    GCodeViewer& viewer = canvas->get_gcode_viewer();
    IMSlider* layers    = viewer.get_layers_slider();
    IMSlider* moves     = viewer.get_moves_slider();
    if (!layers || layers->GetMaxValue() <= 0)
        return;

    const double max = double(layers->GetMaxValue());
    const int target = int(std::lround(pct / 100.0 * max));
    layers->SetHigherValue(target);
    if (layers->is_one_layer())
        layers->SetLowerValue(target);
    layers->set_as_dirty();
    if (moves) {
        moves->SetHigherValue(moves->GetMaxValue());
        moves->set_as_dirty();
    }
}

// The path a command was given, checked for reading (`output` false) or writing. nullopt with an
// Error in `fail` when the parameter is unusable.
std::optional<boost::filesystem::path> path_param(const std::string& param, bool output, AppActionRunResult& fail)
{
    std::string err;
    auto        pp = Mcp::parse_path_param(param, err);
    std::optional<boost::filesystem::path> path;
    if (pp)
        path = output ? Mcp::resolve_output_path(pp->path, pp->overwrite, err) : Mcp::resolve_input_path(pp->path, err);
    if (!path)
        fail = {AppActionRunResult::Level::Error, from_u8(err)};
    return path;
}

AppActionRunResult written(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    if (boost::filesystem::exists(path, ec))
        return {AppActionRunResult::Level::Success, from_u8(path.string())};
    return {AppActionRunResult::Level::Error, _L("The file was not written.")};
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

// Recordings are the MCP service's. With the server off, Start Recording starts the service without its endpoint,
// as the Run Script dialog does. nullptr while OrcaSlicer closes.
Mcp::McpService* recording_service()
{
    if (Mcp::McpService* s = wxGetApp().mcp_service(); s == nullptr || !s->started())
        wxGetApp().start_mcp_service(false);
    Mcp::McpService* s = wxGetApp().mcp_service();
    return s != nullptr && s->started() && !s->stopping() ? s : nullptr;
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

// The person's recording: the main window, with the overlays the settings ask for, for up to an hour,
// into the folder the settings name. The dialog shows the remembered settings, with what `param` overrides,
// and remembers what it starts with; a silent `param` starts with them at once and remembers
// nothing. Encoding needs an ffmpeg: the dialog says so and records without encoding, and a silent start that
// would encode is refused. Neither a script's end nor the server turning off ends the recording. Whatever does
// end it says where it went: Stop Recording itself, or the recording's `ended` for an agent's stop_recording,
// the limit or a write error; with encode, `ended` also encodes it.
AppActionRunResult start_recording(const std::string& param)
{
    if (NativeCommands::recording_active())
        return {AppActionRunResult::Level::Info, _L("A recording is already running."), false};
    AppConfig&             config   = *wxGetApp().app_config;
    Mcp::RecordingSettings settings = Mcp::load_recording_settings(config);
    bool                   silent   = false;
    if (!param.empty())
        if (const std::string err = Mcp::apply_recording_overrides(settings, nlohmann::json::parse(param, nullptr, false), silent);
            !err.empty())
            return {AppActionRunResult::Level::Error, from_u8(err), false};
    std::string ffmpeg_err;
    const bool  has_ffmpeg = Mcp::find_ffmpeg(ffmpeg_err).has_value();
    if (silent && settings.encode && !has_ffmpeg)
        // TRN %1% is why, in English.
        return {AppActionRunResult::Level::Error,
                format_wxstr(_L("The recording could not be encoded: %1%. Install ffmpeg, or start without encoding."), ffmpeg_err), false};
    if (!silent) {
        RecordingDialog dialog(wxGetApp().mainframe, settings);
        if (dialog.ShowModal() != wxID_OK)
            return {AppActionRunResult::Level::Info, _L("Recording cancelled."), false};
        settings = dialog.settings();
        Mcp::save_recording_settings(config, settings);
        config.save();
    }

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

// What import_configs does with a preset that exists already: the `on_conflict` of its param
// {"path": ..., "on_conflict": "skip" | "overwrite"}, skip by default. `overwrite`, which other path
// params use for an output file, is refused rather than ignored. nullopt with an Error in `fail`.
std::optional<bool> import_overwrites(const std::string& param, AppActionRunResult& fail)
{
    const auto first = param.find_first_not_of(" \t");
    if (first == std::string::npos || param[first] != '{')
        return false;
    const nlohmann::json j        = nlohmann::json::parse(param, nullptr, false);
    const nlohmann::json conflict = j.is_object() ? j.value("on_conflict", nlohmann::json("skip")) : nlohmann::json();
    if (!j.is_object() || j.contains("overwrite") || (conflict != "skip" && conflict != "overwrite")) {
        fail = {AppActionRunResult::Level::Error, "on_conflict is \"skip\" (default) or \"overwrite\": what happens to a preset that exists already"};
        return std::nullopt;
    }
    return conflict == "overwrite";
}

// Imports preset files from `path` (UTF-8) as the Import Configs dialog does, answering for each preset
// that exists already as the dialog's Yes or No would, so that every one of them is asked about and can
// be named: the message lists the presets imported, replaced and kept.
AppActionRunResult import_preset_file(MainFrame& mf, const std::string& path, bool overwrite)
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    const auto          names  = [&bundle] {
        std::set<std::pair<int, std::string>> out;
        for (const PresetCollection* presets : std::initializer_list<const PresetCollection*>{&bundle.prints, &bundle.filaments, &bundle.printers})
            for (const Preset& preset : presets->get_presets())
                out.emplace(int(presets->type()), preset.name);
        return out;
    };
    const auto               before = names();
    std::vector<std::string> replaced, kept;
    mf.import_configs({path}, [&](const std::string& name) {
        (overwrite ? replaced : kept).push_back(name);
        return overwrite ? 1 : 0; // never "to all", which would stop the questions
    });
    std::vector<std::string> added;
    for (const auto& name : names())
        if (before.count(name) == 0)
            added.push_back(name.second);

    wxString   message;
    const auto add = [&message](const wxString& text, const std::vector<std::string>& list) {
        if (!list.empty())
            message += (message.empty() ? "" : " ") + format_wxstr(text, boost::algorithm::join(list, ", "));
    };
    // TRN %1% is a comma-separated list of preset names.
    add(_L("Imported presets: %1%."), added);
    // TRN %1% is a comma-separated list of preset names.
    add(_L("Replaced presets: %1%."), replaced);
    // TRN %1% is a comma-separated list of preset names.
    add(_L("Kept the existing presets: %1%."), kept);
    if (message.empty())
        return {AppActionRunResult::Level::Error,
                _L("No presets were imported.") + _L("\nHint: Make sure you have added the corresponding printer before importing the configs.")};
    return {AppActionRunResult::Level::Success, message};
}

constexpr const char* kCommandPrefix = "orca_command";

// Thin AppAction wrapper for one catalog entry: identity and presentation come from the catalog,
// run() routes back to it. The id is keyed by the stable catalog key (not the display title), so a
// rename or a UI-language switch never re-keys the action.
struct CommandAction : AppAction
{
    std::string command_key;

    AppActionRunResult run(const std::string& param) const override { return NativeCommands::run(command_key, param); }

    explicit CommandAction(const NativeCommand& c)
        : AppAction(AppActionId{AppAction::compose_id(kCommandPrefix, c.key, kOrcaSourceKey)}, I18N::translate_utf8(c.title), kOrcaSourceKey,
                    kOrcaSourceName)
        , command_key(c.key)
    {
        this->kind     = AppActionKind::Command;
        this->group    = I18N::translate_utf8(c.group);
        this->title_en = c.title;
        this->group_en = c.group;
        this->input    = c.input;
        this->icon     = c.icon;
        this->automation_description  = c.description;
        this->param_type              = NativeCommands::param_type_name(c.param.type);
        this->param_help              = c.param.help;
        this->completes_in_background = c.completion == CommandCompletion::Export;
    }
};

// What automation needs to call each command: a description for agents (English, never shown in the
// palette), the meaning of `param`, and whether the effect finishes after run() returns. Commands
// without an entry run immediately and ignore `param`.
void describe_commands(std::vector<NativeCommand>& commands)
{
    struct Detail
    {
        const char*       key;
        const char*       description;
        CommandParamType  param = CommandParamType::None;
        const char*       param_help = "";
        CommandCompletion completion = CommandCompletion::None;
    };
    static const Detail details[] = {
        {"slice_and_preview", "Slice the current plate and show the Preview tab. Returns at once; use the slice tool to wait for the result."},
        {"slice_all", "Slice every plate and show the Preview tab. Returns at once; use the slice tool to wait for results."},
        {"go_to_layer", "Show the Preview tab at a layer chosen as a percentage of the layer range.", CommandParamType::Percent, "0-100"},
        {"go_to_tab", "Switch to a main window tab.", CommandParamType::Tab, "tab id: home, prepare, preview, monitor, project, calibration, or a plugin page id"},
        {"load_project", "Open a project. With a path, opens that file without a dialog (asks first if the current project has unsaved changes).", CommandParamType::Path, "absolute path of a .3mf project"},
        {"save_project", "Save the project to its current file (asks for a name if it has none)."},
        {"save_project_as", "Save the project as a .3mf file. With a path, saves there without a dialog.", CommandParamType::Path, "absolute output path, or {\"path\": ..., \"overwrite\": true}"},
        {"new_project", "Start a new, empty project (asks first if the current one has unsaved changes)."},
        {"undo", "Undo the last change to the project."},
        {"redo", "Redo the last undone change."},
        {"open_script_runner", "Open the Run Script dialog, where a person opens or pastes an orca_script, reads its steps and runs "
                               "them. Agents run scripts with the run_script tool instead."},
        {"take_screenshot", "Save a PNG of the main window in the folder Start Recording saves in (the person's Downloads unless "
                            "they chose another) and show the person a notification naming it; the reply has its path. Agents use "
                            "the screenshot tool, which returns the image."},
        {"start_recording", "Start the person's recording of the main window, for up to an hour, with the settings the Start "
                            "Recording dialog remembers: JPEG quality, whether tool calls are captioned and the pointer highlighted, "
                            "whether and how it is encoded to MP4 once it ends, and the "
                            "folder both go in (the person's Downloads unless they chose another). Without silent, the dialog opens "
                            "with them (the param's values filled in): set its controls with ui_input (each is named after its "
                            "param key) and answer it with dialog_respond \"Start recording\"; it remembers what it starts with. "
                            "With silent, the recording starts at once and nothing is remembered; one that would encode is refused "
                            "when no ffmpeg is found. The reply has the path and the settings used. Neither a script's end nor the "
                            "server turning off ends it. With encode, the encode starts once the recording's file is closed, as an "
                            "encode_recording job in get_state.jobs: wait {for: \"job\"} with its id returns the MP4's path, or "
                            "an error. Agents record with start_recording instead.",
         CommandParamType::Text,
         "JSON object, all optional: quality (30-95), captions (bool), pointer (bool), encode (bool), encode_preset (small, "
         "balanced, quality, compatible; applied before the fields), codec (h265, h264), crf (0-51), speed (ultrafast ... "
         "veryslow), profile (h264: high, main, baseline; h265: main, main10), folder (absolute path of an existing "
         "directory), silent (bool)"},
        {"stop_recording", "Stop the running recording, whoever started it, without waiting for its file, and show the person a "
                           "notification naming it; the reply has its path. A later stop_recording reports it as ended "
                           "stopped_in_app. A recording Start Recording made with encode is then encoded: find its "
                           "encode_recording job in get_state.jobs. Agents stop their own recordings with stop_recording, which "
                           "waits and reports."},
        {"open_preferences", "Open the Preferences dialog on a tab, scrolled to one preference when given (shown on its own tab). Its "
                             "controls are named after their preference keys: ui_input {\"target\": {\"name\": key, \"within\": "
                             "\"Preferences\"}} changes one through the dialog; get_settings scope preferences reads them.",
         CommandParamType::Text, "general, control, graphics, online or developer, optionally :<key>, e.g. control:auto_arrange"},
        {"mode_simple", "Switch the settings view to Simple mode."},
        {"mode_advanced", "Switch the settings view to Advanced mode."},
        {"mode_expert", "Switch the settings view to Expert mode."},
        {"toggle_developer_mode", "Turn Developer mode on or off."},
        {"export_gcode", "Export the current plate's G-code. With a path, exports there without a dialog once the plate is sliced; the export runs in the background.", CommandParamType::Path, "absolute .gcode path, or {\"path\": ..., \"overwrite\": true}", CommandCompletion::Export},
        {"export_stl", "Export all objects as one STL. With a path, writes there without a dialog.", CommandParamType::Path, "absolute .stl path, or {\"path\": ..., \"overwrite\": true}"},
        {"export_3mf", "Export the model as a .3mf without slicing data. With a path, writes there without a dialog.", CommandParamType::Path, "absolute .3mf path, or {\"path\": ..., \"overwrite\": true}"},
        {"export_sliced_file", "Export the current plate's sliced file (.gcode.3mf). With a path, writes there without a dialog.", CommandParamType::Path, "absolute .3mf path, or {\"path\": ..., \"overwrite\": true}"},
        {"export_all_sliced_file", "Export every plate's sliced file into one .gcode.3mf. With a path, writes there without a dialog.", CommandParamType::Path, "absolute .3mf path, or {\"path\": ..., \"overwrite\": true}"},
        {"send_gcode", "Open the Send G-code to printer host dialog for the current plate, as the Print button does for a printer with a print host (OctoPrint, Moonraker, PrusaLink, Duet...). Set the file name with ui_input and press Upload or Upload and Print with dialog_respond; the plate is sliced if needed, then uploaded in the background: get_state.uploads shows the row's state, and a failed upload also opens an error dialog. A replay uploads again. Fails for a printer without a print host, or one that prints through the Bambu network or printer agents."},
        {"export_stl_multi", "Export each object as its own STL. With a directory, writes there without a dialog.", CommandParamType::Path, "absolute path of an existing directory"},
        {"export_drc_single", "Export all objects as one Draco (.drc) file."},
        {"export_drc_multi", "Export each object as its own Draco (.drc) file."},
        {"export_toolpaths_obj", "Export the sliced toolpaths as an OBJ."},
        {"export_config", "Export presets as a bundle or ZIP archives. Opens a dialog: click the label of the kind to export, check "
                          "the presets (their checkboxes are named after them), press OK, then answer the result message. With a "
                          "directory, OK writes there instead of asking for a folder.",
         CommandParamType::Path, "absolute path of an existing directory"},
        {"import_file", "Import models (3MF/STL/STEP/SVG/OBJ/AMF). With a path, imports that file without a dialog.", CommandParamType::Path, "absolute path of a model file"},
        {"import_zip_archive", "Import models from a ZIP archive (opens a file dialog)."},
        {"import_configs", "Import presets. With a path, imports a preset .json, a .zip of them or an .orca_printer, "
                           ".orca_filament or .orca_bundle without a dialog, and the message names the presets imported, "
                           "replaced and kept; a .gcode loads the settings it was sliced with.",
         CommandParamType::Path, "absolute path, or {\"path\": ..., \"on_conflict\": \"overwrite\"} to replace presets that exist "
                                 "already (default \"skip\")"},
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
        // menu's Order By. A param stands in for the item's dialog.
        {"obj_type_part", "Make the selected parts (select {volumes} first) solid parts, as Change Type does. Parts are listed by type, so part indices change."},
        {"obj_type_negative", "Make the selected parts negative parts, as Change Type does; not the object's last solid part. Parts are listed by type, so part indices change."},
        {"obj_type_modifier", "Make the selected parts modifiers, as Change Type does; not the object's last solid part. Parts are listed by type, so part indices change."},
        {"obj_type_support_blocker", "Make the selected parts support blockers, as Change Type does; not the object's last solid part, nor a text or SVG part. Parts are listed by type, so part indices change."},
        {"obj_type_support_enforcer", "Make the selected parts support enforcers, as Change Type does; not the object's last solid part, nor a text or SVG part. Parts are listed by type, so part indices change."},
        {"obj_type_precise_seam", "Make the selected parts Center precise seams (seams keep their subtype), as Change Type does; not the object's last solid part, nor a text or SVG part. Parts are listed by type, so part indices change."},
        {"obj_rename", "Rename the selected object or part (select one). With a name, skips the Rename dialog; a name with characters a file name cannot hold is refused.", CommandParamType::Text, "the new name"},
        {"obj_set_instances", "Give the selected object this many instances, in one undo step. With a number, skips the dialog. New instances overlap; arrange afterwards. The reply has object and instances.", CommandParamType::Index, "total number of instances, 1-1000"},
        {"obj_assemble", "Assemble two or more selected objects into one object of parts named Assembly. They are removed and it is added last, so object indices change; the reply has its index (object) and name."},
        {"obj_instances_to_objects", "Make each instance of the selected object a separate object (select {objects: [n]}); with some of its instances selected, only those. New objects are added last; the reply lists the objects the instances are in now (objects)."},
        {"obj_order_by_name", "Order the current plate's objects by name, which sets the object list and print order, as the plate menu's Order By does. Choosing the order they already have reverses it. Object indices change; the reply lists the new order (order: [{object, name, plate}]) and descending.", CommandParamType::Text, "all, to order every plate"},
        {"obj_order_by_filament", "Order the current plate's objects by filament, which sets the object list and print order, as the plate menu's Order By does. Choosing the order they already have reverses it. Object indices change; the reply lists the new order (order: [{object, name, plate}]) and descending.", CommandParamType::Text, "all, to order every plate"},
        {"obj_order_by_height", "Order the current plate's objects by height, which sets the object list and print order, as the plate menu's Order By does. Choosing the order they already have reverses it. Object indices change; the reply lists the new order (order: [{object, name, plate}]) and descending.", CommandParamType::Text, "all, to order every plate"},
        {"obj_order_by_size", "Order the current plate's objects by size (bounding box volume), which sets the object list and print order, as the plate menu's Order By does. Choosing the order they already have reverses it. Object indices change; the reply lists the new order (order: [{object, name, plate}]) and descending.", CommandParamType::Text, "all, to order every plate"},
        {"add_primitive_svg", "Add an SVG as a new object, or as a part of the selected object. With a path, uses that file without a dialog; the part is created in the background (wait for idle), and the SVG gizmo opens on it.", CommandParamType::Path, "absolute path of an .svg file"},
        // Shapes: a size skips the Primitive size dialog.
        {"add_primitive_cube", "Add a cube object to the current plate; fails while an object is selected. With a size, skips the size dialog.", CommandParamType::Text, "X Y Z in mm, separated by spaces"},
        {"add_primitive_cylinder", "Add a cylinder object to the current plate; fails while an object is selected. With a size, skips the size dialog. The shape may then be turned toward the part-cooling fan, so the size is along its own axes before that turn.", CommandParamType::Text, "diameter and height in mm, separated by a space"},
        {"add_primitive_sphere", "Add a sphere object to the current plate; fails while an object is selected. With a size, skips the size dialog.", CommandParamType::Text, "diameter in mm"},
        {"add_primitive_cone", "Add a cone object to the current plate; fails while an object is selected. With a size, skips the size dialog. The shape may then be turned toward the part-cooling fan, so the size is along its own axes before that turn.", CommandParamType::Text, "base diameter and height in mm, separated by a space"},
        {"add_primitive_disc", "Add a disc object to the current plate; fails while an object is selected. With a size, skips the size dialog. The shape may then be turned toward the part-cooling fan, so the size is along its own axes before that turn.", CommandParamType::Text, "diameter and height in mm, separated by a space"},
        {"add_primitive_torus", "Add a torus object to the current plate; fails while an object is selected. With a size, skips the size dialog. The shape may then be turned toward the part-cooling fan, so the size is along its own axes before that turn.", CommandParamType::Text, "inner diameter and tube diameter in mm, separated by a space"},
        {"plate_add", "Add a plate."},
        {"plate_duplicate", "Duplicate the current plate."},
        {"plate_delete", "Delete the current plate."},
        {"plate_rename", "Rename the current plate (opens a dialog)."},
        {"plate_toggle_lock", "Lock or unlock the current plate."},
        {"plate_goto", "Make a plate the current one.", CommandParamType::Index, "0-based plate index"},
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
        {"install_local_plugin", "Install a plugin from a local file (opens a file dialog)."},
        // Filament slots
        {"filament_add", "Add a filament slot, as the + button above the filament list does; the message names the new slot. It "
                         "takes the next colour and the last slot's preset (change them with select_preset filament_slot and "
                         "set_settings scope project filament_colour). It goes after the physical slots: mixed filaments move "
                         "up by one, and objects on them follow."},
        {"filament_delete", "Delete a filament slot, as its Delete menu item does; objects on it move to another slot. A mixed "
                            "filament (project scope filament_is_mixed) is refused.", CommandParamType::Index,
         "1-based filament slot; without it, the last physical slot, as the - button"},
    };
    for (NativeCommand& c : commands) {
        auto it = std::find_if(std::begin(details), std::end(details), [&](const Detail& d) { return c.key == d.key; });
        if (it != std::end(details)) {
            c.description = it->description;
            c.param       = {it->param, it->param_help};
            c.completion  = it->completion;
        } else if (c.key.rfind("calib_", 0) == 0) {
            c.description = "Open the " + c.title + " dialog; the calibrate tool runs the same test with typed parameters and no dialog.";
        } else if (c.key.rfind("view_", 0) == 0) {
            c.description = "Point the 3D camera: " + c.title + ".";
        } else if (c.key.rfind("add_primitive_", 0) == 0 || c.key.rfind("add_handy_", 0) == 0) {
            c.description = "Add a " + c.title + " object to the current plate.";
        }
    }
}

std::vector<NativeCommand> build_command_catalog()
{
    std::vector<NativeCommand> out;
    // `title` and `group` are the untranslated English msgids, L("...")-marked at each call so xgettext
    // extracts them.
    auto add = [&](std::string key, std::string title, std::string group, std::function<AppActionRunResult(const std::string&)> runner,
                   std::string input = {}, std::string icon = {}) {
        out.push_back({std::move(key), std::move(title), std::move(group), std::move(input), std::move(icon), std::move(runner)});
    };
    // Presentation-first overload: keeps the tile icon next to the title/group it belongs to.
    auto add_with_icon = [&](std::string key, std::string title, std::string group, std::string icon,
                             std::function<AppActionRunResult(const std::string&)> runner, std::string input = {}) {
        add(std::move(key), std::move(title), std::move(group), std::move(runner), std::move(input), std::move(icon));
    };

    // ---- Slice & Export ----
    // Slices through the toolbar's path, so it counts as a user slice request (Plater::request_slice).
    add_with_icon("slice_and_preview", L("Slice and Preview"), L("Slice & Export"), "media_play", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            plater->request_slice(false);
            plater->select_view_3D("Preview", false);
            if (MainFrame* mf = wxGetApp().mainframe; mf)
                mf->select_tab(TAB_ID_PREVIEW);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("slice_all", L("Slice All Plates"), L("Slice & Export"), "media_play", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            plater->request_slice(true);
            if (MainFrame* mf = wxGetApp().mainframe; mf)
                mf->select_tab(TAB_ID_PREVIEW);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    add_with_icon(
        "go_to_layer", L("Go to layer (percent)"), L("Commands"), "height_range_layer",
        [](const std::string& param) {
            Plater* plater = wxGetApp().plater();
            if (plater) {
                plater->select_view_3D("Preview", false);
                if (MainFrame* mf = wxGetApp().mainframe; mf)
                    mf->select_tab(TAB_ID_PREVIEW);
                go_to_layer(plater, param);
            }
            return AppActionRunResult{AppActionRunResult::Level::Success};
        },
        "percent");

    // "go_to_tab" is two-phase: the palette collects the tab after activating it, then hands the tab
    // id back as `param` (same contract as go_to_layer's percent).
    add(
        "go_to_tab", L("Go to tab..."), L("Commands"),
        [](const std::string& param) {
            if (MainFrame* mf = wxGetApp().mainframe; mf && !param.empty())
                mf->select_tab(from_u8(param));
            return AppActionRunResult{AppActionRunResult::Level::Success};
        },
        "tab");

    add_with_icon("load_project", L("Load Project"), L("Commands"), "menu_open", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->load_project();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, false, fail);
        if (!path)
            return fail;
        plater->load_project(from_path(*path));
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("save_project", L("Save Project"), L("Commands"), "menu_save", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->save_project(false);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("save_project_as", L("Save Project As"), L("Commands"), "menu_save", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->save_project(true);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, true, fail);
        if (!path)
            return fail;
        if (plater->save_project(true, from_path(*path)) != wxID_YES && !boost::filesystem::exists(*path))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("The project was not saved.")};
        return written(*path);
    });
    add_with_icon("new_project", L("New Project"), L("Commands"), "menu_open", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->new_project();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("undo", L("Undo"), L("Commands"), "menu_undo", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->undo();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("redo", L("Redo"), L("Commands"), "menu_redo", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->redo();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("open_script_runner", L("Run Script"), L("Commands"), "media_play", [](const std::string&) {
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->show_script_runner();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("open_preferences", L("Preferences"), L("Commands"), "cog", [](const std::string& param) {
        if (param.empty()) {
            wxGetApp().open_preferences();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        // "tab[:key]": the tab to show, and the preference whose control is scrolled into view and focused.
        static const std::pair<const char*, PreferencesTab> tabs[] = {{"general", PreferencesTab::General},
                                                                      {"control", PreferencesTab::Control},
                                                                      {"graphics", PreferencesTab::Graphics},
                                                                      {"online", PreferencesTab::Online},
                                                                      {"developer", PreferencesTab::Developer}};
        const std::size_t colon = param.find(':');
        const std::string tab   = param.substr(0, colon);
        const auto        it    = std::find_if(std::begin(tabs), std::end(tabs), [&](const auto& t) { return tab == t.first; });
        if (it == std::end(tabs))
            return AppActionRunResult{AppActionRunResult::Level::Error,
                                      from_u8("Unknown tab \"" + tab + "\": general, control, graphics, online or developer")};
        wxGetApp().open_preferences(it->second, colon == std::string::npos ? std::string() : param.substr(colon + 1));
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Screenshots and recordings ---- (the MCP screenshot and recording tools' defaults, for the person)
    add_with_icon("take_screenshot", L("Take Screenshot"), L("Commands"), "monitor_recording_off", [](const std::string&) {
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
    add_with_icon("start_recording", L("Start Recording"), L("Commands"), "monitor_recording_on", start_recording);
    add_with_icon("stop_recording", L("Stop Recording"), L("Commands"), "video_state_off", [](const std::string&) {
        Mcp::McpService*     service = wxGetApp().mcp_service();
        const nlohmann::json state   = service != nullptr && service->started() ? service->recorder().state() : nlohmann::json();
        if (state.is_null() || state.value("ending", false))
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("No recording is running."), false};
        // What the window shows now, even if the timer has not ticked since, before the notification shows.
        service->recorder().capture_now();
        // The recording may be an agent's or a script's; the person stopped it all the same, so they are told
        // where it went. Before it ends: a person's recording that is to be encoded says so instead, as it ends.
        const boost::filesystem::path path = Mcp::path_from_utf8(state["path"].get<std::string>());
        notify_saved(_L("Recording saved"), path);
        service->recorder().end(kStoppedInApp);
        return AppActionRunResult{AppActionRunResult::Level::Success, {}, true, path_reply(path)};
    });

    // ---- Mode ----
    add_with_icon("mode_simple", L("Mode: Simple"), L("Mode"), "advanced", [](const std::string&) {
        wxGetApp().set_mode(comSimple);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("mode_advanced", L("Mode: Advanced"), L("Mode"), "advanced", [](const std::string&) {
        wxGetApp().set_mode(comAdvanced);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("mode_expert", L("Mode: Expert"), L("Mode"), "advanced", [](const std::string&) {
        wxGetApp().set_mode(comExpert);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    // Mirrors Preferences > Developer > Developer mode: flip the flag, persist, refresh the UI.
    add_with_icon("toggle_developer_mode", L("Toggle Developer Mode"), L("Mode"), "advanced", [](const std::string&) {
        GUI_App& app = wxGetApp();
        const bool on = !app.app_config->get_bool("developer_mode");
        app.app_config->set_bool("developer_mode", on);
        app.app_config->save();
        app.update_mode();
        return AppActionRunResult{AppActionRunResult::Level::Success, on ? _L("Developer mode enabled.") : _L("Developer mode disabled.")};
    });

    // ---- Export pipeline ----
    add_with_icon("export_gcode", L("Export G-code"), L("Slice & Export"), "custom-gcode_gcode", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->export_gcode(false);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, true, fail);
        if (!path)
            return fail;
        if (plater->is_slicing_any())
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("Slicing is in progress. Wait for it to finish and export again.")};
        if (!plater->export_gcode_to(*path))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("The export could not start. Slice the plate first and check for slicing errors.")};
        return AppActionRunResult{AppActionRunResult::Level::Info, from_u8(path->string())};
    });
    add_with_icon("export_stl", L("Export STL"), L("Slice & Export"), "save", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->export_stl();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, true, fail);
        if (!path)
            return fail;
        plater->export_stl(false, false, false, FT_STL, from_path(*path));
        return written(*path);
    });
    add_with_icon("export_3mf", L("Export 3MF"), L("Slice & Export"), "menu_save", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->export_core_3mf();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, true, fail);
        if (!path)
            return fail;
        plater->export_core_3mf(from_path(*path));
        return written(*path);
    });
    auto export_sliced = [](bool all) {
        return [all](const std::string& param) {
            Plater* plater = wxGetApp().plater();
            if (plater == nullptr)
                return AppActionRunResult{};
            if (param.empty()) {
                plater->export_gcode_3mf(all);
                return AppActionRunResult{AppActionRunResult::Level::Success};
            }
            AppActionRunResult fail;
            const auto         path = path_param(param, true, fail);
            if (!path)
                return fail;
            if (!plater->export_gcode_3mf(all, *path))
                return AppActionRunResult{AppActionRunResult::Level::Error, _L("The sliced file was not written. Slice first and check for slicing errors.")};
            return written(*path);
        };
    };
    add_with_icon("export_sliced_file", L("Export Sliced File"), L("Slice & Export"), "save", export_sliced(false));
    add_with_icon("export_all_sliced_file", L("Export All Sliced Files"), L("Slice & Export"), "save", export_sliced(true));

    // ---- Send to print host ----
    // The Print button's send for a printer with a print host: the Send G-code dialog, then the upload queue. Where
    // the button would not send to a print host, the command says why instead of doing nothing.
    add_with_icon("send_gcode", L("Send G-code to printer host"), L("Slice & Export"), "printer", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        PresetBundle& bundle = *wxGetApp().preset_bundle;
        if (bundle.use_bbl_network() || wxGetApp().app_config->get_bool("use_printer_agents"))
            // TRN "Print plate" is the print button's action, which sends to such a printer through its own dialog.
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("This printer prints with Print plate, not through a print host.")};
        // The Print button sends only to a printer with a print host address, as MainFrame::can_send_gcode() checks.
        const auto* host = bundle.printers.get_edited_preset().config.option<ConfigOptionString>("print_host");
        if (host == nullptr || host->value.empty())
            return AppActionRunResult{AppActionRunResult::Level::Error,
                                      _L("This printer has no print host. Set one in the printer's connection settings.")};
        plater->send_gcode_legacy();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Calibration ----
    // The tab-strip calib_sf glyph is drawn white for the dark tab bar and vanishes on the palette's
    // light tile, so each wizard borrows the matching settings-group icon instead (gray + accent green).
    add_with_icon("calib_temperature", L("Temperature Calibration"), L("Calibration"), "param_temperature",
        [](const std::string&) { return calib_command(CalibKind::Temperature); });
    add_with_icon("calib_max_volumetric", L("Max Volumetric Speed Calibration"), L("Calibration"), "param_volumetric_speed",
        [](const std::string&) { return calib_command(CalibKind::MaxVolumetric); });
    add_with_icon("calib_pressure_advance", L("Pressure Advance Calibration"), L("Calibration"), "param_flow_ratio_and_pressure_advance",
        [](const std::string&) { return calib_command(CalibKind::PressureAdvance); });
    add_with_icon("calib_flow_ratio", L("Flow Ratio Calibration"), L("Calibration"), "param_flow_ratio_and_pressure_advance",
        [](const std::string&) { return calib_command(CalibKind::FlowRatio); });
    add_with_icon("calib_retraction", L("Retraction Calibration"), L("Calibration"), "param_retraction",
        [](const std::string&) { return calib_command(CalibKind::Retraction); });
    add_with_icon("calib_cornering", L("Cornering Calibration"), L("Calibration"), "param_precision",
        [](const std::string&) { return calib_command(CalibKind::Cornering); });
    add_with_icon("calib_input_shaping_freq", L("Input Shaping Frequency Calibration"), L("Calibration"), "param_resonance_avoidance",
        [](const std::string&) { return calib_command(CalibKind::InputShapingFreq); });
    add_with_icon("calib_input_shaping_damp", L("Input Shaping Damping Calibration"), L("Calibration"), "param_resonance_avoidance",
        [](const std::string&) { return calib_command(CalibKind::InputShapingDamp); });
    add_with_icon("calib_vfa", L("VFA Calibration"), L("Calibration"), "param_speed", [](const std::string&) { return calib_command(CalibKind::VFA); });

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
            [d](const std::string&) { return view_command(wxGetApp().plater(), d); });
    }
    add("view_default", L("View: Default"), L("View"), [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            plater->select_view("plate");
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->zoom_to_bed();
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("view_fit_bed", L("Fit Bed to View"), L("View"), [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->zoom_to_bed();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("view_toggle_perspective", L("Toggle Perspective"), L("View"), [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->get_camera().select_next_type();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("reset_window_layout", L("Reset Window Layout"), L("View"), "toolbar_reset", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->reset_window_layout();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Object ----
    add_with_icon("obj_delete", L("Delete Selected"), L("Object"), "delete", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return !p->is_selection_empty(); }, [](Plater* p) { p->remove_selected(); });
    });
    add_with_icon("obj_delete_all", L("Delete All Objects"), L("Object"), "delete", [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_delete_all(); }, [](Plater* p) { p->delete_all_objects_from_model(); });
    });
    add("select_all", L("Select All Objects"), L("Object"), [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        ensure_3d_view(plater);
        plater->select_all();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("deselect_all", L("Deselect All"), L("Object"), [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->deselect_all();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("obj_mirror_x", L("Mirror X"), L("Object"), "menu_mirror_x", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::X); });
    });
    add_with_icon("obj_mirror_y", L("Mirror Y"), L("Object"), "menu_mirror_y", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::Y); });
    });
    add_with_icon("obj_mirror_z", L("Mirror Z"), L("Object"), "menu_mirror_z", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::Z); });
    });
    add_with_icon("obj_split_objects", L("Split to Objects"), L("Object"), "menu_split_objects", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_split_to_objects(); }, [](Plater* p) { p->split_object(true); });
    });
    add_with_icon("obj_split_parts", L("Split to Parts"), L("Object"), "menu_split_parts", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_split_to_volumes(); }, [](Plater* p) { p->split_volume(); });
    });
    add("obj_center", L("Center Selected on Plate"), L("Object"), [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return !p->is_selection_empty(); }, [](Plater* p) { p->center_selection(); });
    });
    add_with_icon("obj_drop", L("Drop to Bed"), L("Object"), "toolbar_flatten", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->get_selection().can_drop(); }, [](Plater* p) { p->drop_selection(); });
    });
    add("obj_fit_volume", L("Scale to Fit Print Volume"), L("Object"), [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_scale_to_print_volume(); },
            [](Plater* p) { p->scale_selection_to_fit_print_volume(); });
    });
    add_with_icon("obj_instances_up", L("Increase Instances"), L("Object"), "instance_add", [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_increase_instances(); }, [](Plater* p) { p->increase_instances(); });
    });
    add_with_icon("obj_instances_down", L("Decrease Instances"), L("Object"), "instance_remove", [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_decrease_instances(); }, [](Plater* p) { p->decrease_instances(); });
    });
    add_with_icon("obj_arrange", L("Auto-Arrange"), L("Object"), "toolbar_arrange", [](const std::string&) {
        return arrange_op(wxGetApp().plater(), [](Plater* p) { p->arrange(); }, _L("There are no objects to arrange."));
    });
    add_with_icon("obj_orient", L("Auto-Orient"), L("Object"), "toolbar_orient", [](const std::string&) {
        return arrange_op(wxGetApp().plater(), [](Plater* p) { p->orient(); }, _L("There are no objects to orient."));
    });

    // ---- Object structure ---- (the object list's context menus and the plate menu's Order By)
    // Change Type is in the part menus only: without part rows, set_volume_type() would change every
    // part of a selected object, which no menu offers.
    auto add_type = [&](std::string key, std::string title, ModelVolumeType type) {
        add(std::move(key), std::move(title), L("Object"), [type](const std::string&) {
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
    add("obj_rename", L("Rename"), L("Object"), [](const std::string& param) {
        ObjectList* list = object_list_in_3d_view();
        if (list == nullptr)
            return AppActionRunResult{};
        const wxDataViewItemArray rows = selected_rows(*list, itObject | itVolume);
        if (rows.size() != 1)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Select one object or part first."), false};
        if (param.empty())
            list->rename_item();
        else if (!list->rename_item(rows.front(), from_u8(param)))
            return AppActionRunResult{AppActionRunResult::Level::Error,
                                      _L("Invalid name, the following characters are not allowed:") + " <>:/\\|?*\""};
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("obj_set_instances", L("Set Number of Instances"), L("Object"), "instance_add", [](const std::string& param) {
        Plater*     plater = wxGetApp().plater();
        ObjectList* list   = object_list_in_3d_view();
        if (list == nullptr)
            return AppActionRunResult{};
        const std::optional<int> count = param.empty() ? std::nullopt : NativeCommands::parse_count(param);
        if (!param.empty() && (!count || *count > 1000))
            return AppActionRunResult{AppActionRunResult::Level::Error, from_u8("The number of instances is a whole number from 1 to 1000.")};
        if (count == 0)
            return AppActionRunResult{AppActionRunResult::Level::Error, from_u8("0 instances would delete the object; use obj_delete.")};
        const int obj_idx = plater->get_selected_object_idx();
        if (obj_idx < 0)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Select an object first."), false};
        // The menu item is enabled by can_increase_instances(); each of its reasons gets a message.
        const ModelObject* object = plater->model().objects[obj_idx];
        if (list->has_selected_cut_object())
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("A cut object keeps its number of instances.")};
        if (std::any_of(object->instances.begin(), object->instances.end(), [](const ModelInstance* i) { return !i->printable; }))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("Make every instance of the object printable first.")};
        if (!plater->can_increase_instances())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Wait for the running background job to finish."), false};
        if (!count) {
            plater->set_number_of_copies();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        plater->set_number_of_copies(*count);
        return with_reply({{"object", obj_idx}, {"instances", object->instances.size()}});
    });
    add("obj_assemble", L("Assemble"), L("Object"), [](const std::string&) {
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
    add("obj_instances_to_objects", L("Set as Individual Objects"), L("Object"), [](const std::string&) {
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
        add(std::move(key), std::move(title), L("Object"), [order](const std::string& param) {
            Plater*     plater = wxGetApp().plater();
            ObjectList* list   = object_list_in_3d_view();
            if (list == nullptr)
                return AppActionRunResult{};
            if (!param.empty() && param != "all")
                return AppActionRunResult{AppActionRunResult::Level::Error,
                                          from_u8("The param is all, to order every plate, or nothing for the current plate.")};
            const bool all = param == "all";
            if (all ? plater->model().objects.empty() : plater->get_partplate_list().get_selected_plate()->empty())
                return AppActionRunResult{AppActionRunResult::Level::Info, _L("There are no objects to order."), false};
            // order_objects() only shows a notification while a gizmo is open.
            if (plater->get_view3D_canvas3D()->get_gizmos_manager().get_current_type() != GLGizmosManager::Undefined)
                return AppActionRunResult{AppActionRunResult::Level::Error, _L("Error: Please close all toolbar menus first")};
            list->order_objects(order, all);
            return ordered(*plater, order, all);
        });
    };
    add_order("obj_order_by_name", L("Order by Name"), ObjectOrder::Name);
    add_order("obj_order_by_filament", L("Order by Filament"), ObjectOrder::Filament);
    add_order("obj_order_by_height", L("Order by Height"), ObjectOrder::Height);
    add_order("obj_order_by_size", L("Order by Size"), ObjectOrder::Size);

    // ---- Add Primitive ---- (the Add > Add Primitive submenu; creates a new object)
    // A size param holds the dimensions the size dialog asks for, in its order, and skips the dialog.
    auto add_primitive = [&](std::string key, std::string title, std::string icon, const char* type_name) {
        add_with_icon(key, std::move(title), L("Add Primitive"), std::move(icon), [key, type_name](const std::string& param) {
            Plater*     plater = wxGetApp().plater();
            ObjectList* list   = wxGetApp().obj_list();
            if (plater == nullptr || list == nullptr)
                return AppActionRunResult{};
            ensure_3d_view(plater);
            if (!list->can_add_shape_object())
                return AppActionRunResult{AppActionRunResult::Level::Error,
                                          _L("Deselect all objects to add a primitive object. To add a shape to the selected object, "
                                             "use its Add Part menu.")};
            std::vector<double> size;
            if (!param.empty()) {
                const std::optional<std::vector<double>> parsed = NativeCommands::parse_sizes(param);
                if (!parsed || parsed->size() != primitive_size_names(type_name).size())
                    return AppActionRunResult{AppActionRunResult::Level::Error,
                                              from_u8(std::string("The size is ") + NativeCommands::find(key)->param.help + ".")};
                size = *parsed;
            }
            list->load_generic_subobject(type_name, ModelVolumeType::INVALID, true, size);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        });
    };
    add_primitive("add_primitive_cube", L("Cube"), "menu_obj_cube", "Cube");
    add_primitive("add_primitive_cylinder", L("Cylinder"), "menu_obj_cylinder", "Cylinder");
    add_primitive("add_primitive_sphere", L("Sphere"), "menu_obj_sphere", "Sphere");
    add_primitive("add_primitive_cone", L("Cone"), "menu_obj_cone", "Cone");
    add_primitive("add_primitive_disc", L("Disc"), "menu_obj_disc", "Disc");
    add_primitive("add_primitive_torus", L("Torus"), "menu_obj_torus", "Torus");
    add_with_icon("add_primitive_text", L("Text"), L("Add Primitive"), "menu_obj_text", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            ensure_3d_view(plater);
            if (GLCanvas3D* canvas = plater->canvas3D())
                canvas->clear_popup_menu_position();
            MenuFactory::add_text_volume(ModelVolumeType::INVALID);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("add_primitive_svg", L("SVG"), L("Add Primitive"), "menu_obj_svg", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        ensure_3d_view(plater);
        GLCanvas3D* canvas = plater->canvas3D();
        if (canvas != nullptr)
            canvas->clear_popup_menu_position();
        if (param.empty()) {
            MenuFactory::add_svg_volume(ModelVolumeType::INVALID);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        // What the dialog path does once a file is picked: a new object with nothing selected, else
        // a part of the selected object.
        AppActionRunResult fail;
        const auto         path = path_param(param, false, fail);
        if (!path)
            return fail;
        auto* svg = canvas != nullptr ? dynamic_cast<GLGizmoSVG*>(canvas->get_gizmos_manager().get_gizmo(GLGizmosManager::Svg)) : nullptr;
        if (svg == nullptr || !svg->create_volume(into_u8(from_path(*path)), ModelVolumeType::MODEL_PART))
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("The SVG file could not be added.")};
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Add Handy models ---- (the Add > Add Handy models submenu)
    const std::vector<MenuFactory::HandyModel>& handy = MenuFactory::handy_models();
    for (std::size_t i = 0; i < handy.size(); ++i) {
        add("add_handy_" + std::string(handy[i].key), handy[i].label, L("Add Handy models"),
            [i](const std::string&) {
                if (Plater* plater = wxGetApp().plater())
                    ensure_3d_view(plater);
                MenuFactory::load_handy_model(i);
                return AppActionRunResult{AppActionRunResult::Level::Success};
            });
    }

    // ---- Plate ----
    add_with_icon("plate_add", L("Add Plate"), L("Plate"), "toolbar_add_plate", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_add_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot add another plate (maximum reached).")};
        plater->add_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_duplicate", L("Duplicate Plate"), L("Plate"), "menu_copy", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_add_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot duplicate a plate (maximum reached).")};
        plater->duplicate_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_delete", L("Delete Plate"), L("Plate"), "delete", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_delete_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot delete the only plate.")};
        plater->delete_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_rename", L("Rename Plate"), L("Plate"), "plate_name_edit", [](const std::string&) {
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
    add_with_icon("plate_toggle_lock", L("Toggle Plate Lock"), L("Plate"), "lock_normal", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlateList& plates = plater->get_partplate_list();
        const int index       = plates.get_curr_plate_index();
        plater->take_snapshot("lock partplate");
        plates.lock_plate(index, !plates.is_locked(index));
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_goto", L("Go to Plate"), L("Plate"), "go_next_plate", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlateList& plates = plater->get_partplate_list();
        const int count       = plates.get_plate_count();
        if (count <= 0)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("No plates available.")};
        int index = 0;
        try {
            index = std::stoi(param);
        } catch (const std::exception&) {}
        index = std::clamp(index, 0, count - 1);
        plater->select_plate(index, false);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Printer / device connection ----
    add_with_icon("sync_ams", L("Synchronize Filament List from AMS"), L("Printer"), "ams_fila_sync", [](const std::string&) {
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
    add_with_icon("filament_add", L("Add Filament Slot"), L("Filament"), "add_filament", [filament_slots_unavailable](const std::string&) {
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
    add_with_icon("filament_delete", L("Delete Filament Slot"), L("Filament"), "delete_filament", [filament_slots_unavailable](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (const auto refused = filament_slots_unavailable(plater))
            return *refused;
        const PresetBundle& bundle   = *wxGetApp().preset_bundle;
        const size_t        physical = bundle.num_physical_filaments();
        if (physical < 2)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("The only filament slot cannot be deleted.")};
        // Without a slot, the last physical one, as the - button.
        size_t slot = physical;
        if (!param.empty()) {
            try {
                slot = size_t(std::stoul(param));
            } catch (const std::exception&) {
                slot = 0;
            }
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
    });

    // ---- Presets / cloud ----
    add_with_icon("preset_bundle", L("Open Preset Bundle"), L("Presets"), "menu_edit_preset", [](const std::string&) {
        wxGetApp().open_presetbundledialog();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("sync_presets", L("Sync Presets"), L("Presets"), "printer_sync_ok", [](const std::string&) {
        if (!wxGetApp().is_user_login())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Sign in to sync presets.")};
        wxGetApp().restart_sync_user_preset();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Import ----
    add_with_icon("import_file", L("Import 3MF/STL/STEP/SVG/OBJ/AMF"), L("Import"), "menu_open", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
#ifdef __APPLE__
            plater->add_model();
#else
            plater->add_file();
#endif
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path = path_param(param, false, fail);
        if (!path)
            return fail;
        if (plater->load_files(std::vector<boost::filesystem::path>{*path}).empty())
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("Nothing was imported.")};
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("import_zip_archive", L("Import ZIP Archive"), L("Import"), "menu_open", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->import_zip_archive();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("import_configs", L("Import Configs"), L("Import"), "menu_open", [](const std::string& param) {
        MainFrame* mf = wxGetApp().mainframe;
        if (mf == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            mf->load_config_file();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        AppActionRunResult fail;
        const auto         path      = path_param(param, false, fail);
        const auto         overwrite = path ? import_overwrites(param, fail) : std::nullopt;
        if (!overwrite)
            return fail;
        const std::string file = Mcp::path_to_utf8(*path);
        if (is_gcode_file(file)) {
            if (!mf->load_config_file(file))
                return AppActionRunResult{AppActionRunResult::Level::Error, _L("The configuration was not loaded.")};
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        for (const char* ext : {".json", ".zip", ".orca_printer", ".orca_filament", ".orca_bundle"})
            if (boost::iends_with(file, ext))
                return import_preset_file(*mf, file, *overwrite);
        return AppActionRunResult{AppActionRunResult::Level::Error,
                                  "Not a preset file: give a .json, .zip, .orca_printer, .orca_filament, .orca_bundle or .gcode"};
    });

    // ---- Export extras ----
    add_with_icon("export_stl_multi", L("Export All Objects as STLs"), L("Export"), "save", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return AppActionRunResult{};
        if (param.empty()) {
            plater->export_stl(false, false, true);
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        std::string err;
        auto        pp  = Mcp::parse_path_param(param, err);
        auto        dir = pp ? Mcp::resolve_output_dir(pp->path, err) : std::nullopt;
        if (!dir)
            return AppActionRunResult{AppActionRunResult::Level::Error, from_u8(err)};
        plater->export_stl(false, false, true, FT_STL, from_path(*dir));
        return AppActionRunResult{AppActionRunResult::Level::Success, from_u8(dir->string())};
    });
    add_with_icon("export_drc_single", L("Export All Objects as DRC (one file)"), L("Export"), "save", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_stl(false, false, false, FT_DRC);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_drc_multi", L("Export All Objects as DRCs"), L("Export"), "save", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_stl(false, false, true, FT_DRC);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_toolpaths_obj", L("Export Toolpaths as OBJ"), L("Export"), "custom-gcode_gcode", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_toolpaths_to_obj();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_config", L("Export Preset Bundle"), L("Export"), "save", [](const std::string& param) {
        if (param.empty()) {
            if (MainFrame* mf = wxGetApp().mainframe)
                mf->export_config();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        std::string err;
        auto        pp  = Mcp::parse_path_param(param, err);
        auto        dir = pp ? Mcp::resolve_output_dir(pp->path, err) : std::nullopt;
        if (!dir)
            return AppActionRunResult{AppActionRunResult::Level::Error, from_u8(err)};
        // The dialog MainFrame::export_config() opens, given the folder its OK writes to.
        ExportConfigsDialog dlg(nullptr, from_path(*dir));
        if (dlg.ShowModal() != wxID_OK)
            return AppActionRunResult{AppActionRunResult::Level::Error, _L("Nothing was exported.")};
        return AppActionRunResult{AppActionRunResult::Level::Success, from_path(*dir)};
    });

    // ---- Help ---- (mirrors the top-bar Help menu, plus the wiki/YouTube links)
    add("help_keyboard_shortcuts", L("Keyboard Shortcuts"), L("Help"), [](const std::string&) {
        wxGetApp().keyboard_shortcuts(ShortcutContext::Global);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_setup_wizard", L("Setup Wizard"), L("Help"), [](const std::string&) {
        wxGetApp().ShowUserGuide();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_open_config_folder", L("Show Configuration Folder"), L("Help"), "open_project", [](const std::string&) {
        Slic3r::GUI::desktop_open_datadir_folder();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_migrate_filament_presets", L("Migrate Filament Presets"), L("Help"), [](const std::string&) {
        wxGetApp().check_legacy_user_filament_presets(true);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_troubleshoot", L("Troubleshoot Center"), L("Help"), [](const std::string&) {
        wxGetApp().troubleshoot();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_network_test", L("Open Network Test"), L("Help"), [](const std::string&) {
        NetworkTestDialog dlg(wxGetApp().mainframe);
        dlg.ShowModal();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_benchmark_3d_scene", L("Benchmark 3D Scene"), L("Help"), [](const std::string&) {
        run_scene_benchmark();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_tip_of_the_day", L("Show Tip of the Day"), L("Help"), "info", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater()) {
            plater->get_dailytips()->open();
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->set_as_dirty();
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_check_updates", L("Check for Updates"), L("Help"), "refresh", [](const std::string&) {
        wxGetApp().check_new_version_sf(true, 1);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_about", L("About OrcaSlicer"), L("Help"), "OrcaSlicer_gradient_circle", [](const std::string&) {
        Slic3r::GUI::about();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("open_wiki", L("Open Wiki"), L("Help"), "link_wiki_img", [](const std::string&) {
        wxLaunchDefaultBrowser("https://www.orcaslicer.com/wiki/", wxBROWSER_NEW_WINDOW);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("open_youtube", L("Open YouTube Channel"), L("Help"), [](const std::string&) {
        wxLaunchDefaultBrowser("https://www.youtube.com/@OfficialOrcaSlicer/videos", wxBROWSER_NEW_WINDOW);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Plugins ----
    add("open_plugins", L("Open Plugins"), L("Plugins"), [](const std::string&) {
        wxGetApp().open_plugins_dialog();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("refresh_plugins", L("Refresh Plugins"), L("Plugins"), [](const std::string&) {
        wxGetApp().refresh_plugins();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("install_plugin", L("Install Plugin"), L("Plugins"), [](const std::string&) {
        open_plugin_hub();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("install_local_plugin", L("Install Local Plugin"), L("Plugins"), [](const std::string&) {
        wxGetApp().install_local_plugin();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    describe_commands(out);
    return out;
}

} // namespace

const std::vector<NativeCommand>& NativeCommands::catalog()
{
    static const std::vector<NativeCommand> commands = build_command_catalog();
    return commands;
}

std::unique_ptr<AppAction> NativeCommands::make_action(const NativeCommand& command)
{
    return std::make_unique<CommandAction>(command);
}

const NativeCommand* NativeCommands::find(const std::string& key)
{
    for (const NativeCommand& c : catalog())
        if (c.key == key)
            return &c;
    return nullptr;
}

const char* NativeCommands::param_type_name(CommandParamType type)
{
    switch (type) {
    case CommandParamType::Percent: return "percent";
    case CommandParamType::Tab: return "tab";
    case CommandParamType::Path: return "path";
    case CommandParamType::Index: return "index";
    case CommandParamType::Text: return "text";
    default: return "none";
    }
}

std::optional<std::vector<double>> NativeCommands::parse_sizes(const std::string& param)
{
    std::vector<std::string> tokens;
    boost::split(tokens, param, boost::is_any_of(" ,\t"), boost::token_compress_on);
    std::vector<double> sizes;
    for (const std::string& token : tokens) {
        if (token.empty())
            continue;
        size_t       end   = 0;
        const double value = string_to_double_decimal_point(token, &end);
        if (end != token.size() || !std::isfinite(value) || value <= 0.)
            return std::nullopt;
        sizes.push_back(value);
    }
    if (sizes.empty())
        return std::nullopt;
    return sizes;
}

std::optional<int> NativeCommands::parse_count(const std::string& param)
{
    const std::string text  = boost::algorithm::trim_copy(param);
    int               value = 0;
    const char*       end   = text.data() + text.size();
    const auto        res   = std::from_chars(text.data(), end, value);
    if (text.empty() || res.ec != std::errc() || res.ptr != end || value < 0)
        return std::nullopt;
    return value;
}

AppActionRunResult NativeCommands::run(const std::string& key, const std::string& param)
{
    GUI_App& app = wxGetApp();
    if (app.is_closing())
        return {};
    for (const NativeCommand& c : catalog())
        if (c.key == key)
            return c.runner(param);
    return {AppActionRunResult::Level::Info, _L("Unknown command.")};
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

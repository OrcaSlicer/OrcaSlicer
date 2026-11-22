// app_state, app_wait and app_log: what an agent reads before and after acting; undo and redo; and the app's
// commands made from the Help menu's items.

#include "JobRegistry.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "McpEndpoint.hpp"
#include "McpPaths.hpp"
#include "McpRecorder.hpp"
#include "libslic3r/Config.hpp"
#include "AutomationRuntime.hpp"
#include "McpTools.hpp"
#include "ModalTracker.hpp"
#include "PythonScriptsDialog.hpp"
#include "ScreenCapture.hpp"
#include "UiInspector.hpp"
#include "WindowControl.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PrintHostDialogs.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#ifdef SLIC3R_CAD
#include "McpToolsCad.hpp"
#endif
#if defined(__WXGTK__)
#include "slic3r/GUI/LinuxDisplayBackend.hpp"
#endif

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/defs.h>
#include <wx/display.h>
#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/vector.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <istream>
#include <string>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

const char* platform_name()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const char* display_backend()
{
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "cocoa";
#elif defined(__WXGTK__)
    switch (get_linux_display_backend()) {
    case LinuxDisplayBackend::X11: return "x11";
    case LinuxDisplayBackend::Wayland: return "wayland";
    default: return "unknown";
    }
#else
    return "unknown";
#endif
}

const char* mode_name(ConfigOptionMode mode)
{
    switch (mode) {
    case comSimple: return "simple";
    case comAdvanced: return "advanced";
    case comExpert: return "expert";
    case comDevelop: return "develop";
    default: return "simple";
    }
}

// The app as an agent sees it. UI thread.
json app_state(AutomationRuntime& runtime)
{
    // A session that never opened the MCP endpoint has none.
    const McpEndpoint* endpoint = wxGetApp().mcp_endpoint();
    json state{{"version", SLIC3R_VERSION},
               {"platform", platform_name()},
               {"display_backend", display_backend()},
               {"ready", runtime.ready()},
               {"dialogs", runtime.modals().snapshot()},
               {"closed_menus", runtime.inspector().closed_menus()},
               {"auto_answer_rules", runtime.modals().rules_json()},
               {"mcp", endpoint != nullptr ? endpoint->describe() : json{{"serving", false}}},
               {"python",
                {{"available", PythonInterpreter::instance().is_initialized()},
                 {"scripts_unrestricted", python_scripts_unrestricted(*wxGetApp().app_config)}}},
               {"jobs", runtime.jobs().list()}};
    if (json recording = runtime.recorder().state(); !recording.is_null())
        state["recording"] = std::move(recording);
    // Each display's work area, where window_set can place a window.
    json displays = json::array();
    for (unsigned i = 0; i < wxDisplay::GetCount(); ++i) {
        const wxDisplay display(i);
        json            area = display_area(i);
        area["scale"]        = display.GetScaleFactor();
        area["primary"]      = display.IsPrimary();
        displays.push_back(std::move(area));
    }
    state["displays"] = std::move(displays);
    // Whether a person is signed in to the Orca cloud and to Bambu Lab's, and the Bambu network plug-in as the app found
    // it: installed (the setting that has the app load it), loaded, its version and why it did not load.
    GUI_App& app = wxGetApp();
    state["account"] = {{"signed_in", app.is_user_login(ORCA_CLOUD_PROVIDER)}, {"bambu_signed_in", app.is_user_login(BBL_CLOUD_PROVIDER)}};
    const bool                    loaded     = NetworkAgent::is_network_module_loaded();
    const NetworkLibraryLoadError load_error = NetworkAgent::get_load_error();
    state["network_plugin"] = {{"installed", app.app_config->get_bool("installed_networking")},
                               {"loaded", loaded},
                               {"version", loaded ? NetworkAgent::get_version() : std::string()},
                               {"error", load_error.has_error ? load_error.message : std::string()}};
    // The folder Help > Show Configuration Folder opens: the presets, the app configuration and the logs.
    state["config_folder"] = data_dir();
    MainFrame* frame  = wxGetApp().mainframe;
    Plater*    plater = wxGetApp().plater();
    if (frame == nullptr || plater == nullptr)
        return state;

    // The size and state window_list reports, and the place where the platform knows it.
    const wxSize      size        = window_size(*frame);
    const std::string frame_state = window_state(*frame);
    state["window"]               = {{"title", into_u8(frame->GetTitle())},
                                     {"size", {size.x, size.y}},
                                     {"maximized", frame_state == "maximized"},
                                     {"minimized", frame_state == "minimized"},
                                     {"shown", frame->IsShown()}};
    if (window_positions_known()) {
        const wxPoint pos            = frame->GetScreenPosition();
        state["window"]["position"] = {pos.x, pos.y};
    }
    if (Notebook* tabs = frame->m_tabpanel; tabs != nullptr && tabs->GetSelection() != wxNOT_FOUND)
        state["tab"] = into_u8(tabs->GetPageName(size_t(tabs->GetSelection())));
    // The page exists only when the CAD preference was on at startup.
#ifdef SLIC3R_CAD
    state["design_tab"] = frame->m_design_page != nullptr;
    if (json note = cad_note(NativeCommands::instance()); !note.is_null())
        state["cad_note"] = std::move(note);
#else
    state["design_tab"] = false;
#endif
    state["mode"]               = mode_name(wxGetApp().get_mode());
    state["printer_technology"] = plater->printer_technology() == ptFFF ? "FFF" : "SLA";
    state["project"]            = {{"name", into_u8(plater->get_project_name())},
                                   {"path", into_u8(plater->get_project_filename(".3mf"))},
                                   {"dirty", plater->is_project_dirty()}};

    PresetBundle& bundle = *wxGetApp().preset_bundle;
    json          filaments = json::array();
    for (const std::string& name : bundle.filament_presets)
        filaments.push_back(name);
    state["presets"] = {{"printer", bundle.printers.get_selected_preset_name()},
                        {"process", bundle.prints.get_selected_preset_name()},
                        {"filaments", std::move(filaments)}};

    PartPlateList& plates = plater->get_partplate_list();
    state["plates"]       = {{"count", plates.get_plate_count()}, {"current", plates.get_curr_plate_index()}};
    // The current plate's toolpaths, as the Preview shows them; null without any, or while the Preview
    // still holds another plate's.
    GLCanvas3D* preview = plater->get_preview_canvas3D();
    state["preview"]    = preview != nullptr && viewer_holds(preview->get_gcode_viewer(), plates.get_curr_plate()->get_slice_result()) ?
                              preview_state(preview->get_gcode_viewer()) :
                              json();
    if (!state["preview"].is_null())
        state["preview"]["plate"] = plates.get_curr_plate_index();
    // The camera of the 3D view shown, the Design view's on the Design tab, which swaps its camera into the
    // plater's while it is shown.
    if (GLCanvas3D* canvas = shown_canvas(); canvas != nullptr && canvas->get_wxglcanvas()->IsShownOnScreen())
        state["camera"] = camera_state(plater->get_camera(), canvas->get_scale());
    state["view"] = view_state(*plater);
    // The open gizmo, on the canvas shown first.
    state["gizmo"] = nullptr;
    std::vector<GLCanvas3D*> canvases = live_canvases();
    std::stable_partition(canvases.begin(), canvases.end(), [&](GLCanvas3D* c) { return c == plater->get_current_canvas3D(); });
    for (GLCanvas3D* c : canvases)
        if (const auto type = c->get_gizmos_manager().get_current_type(); type != GLGizmosManager::Undefined) {
            state["gizmo"] = {{"name", gizmo_name(type)}, {"canvas", canvas_name(c)}};
            break;
        }
    state["objects"]      = plater->model().objects.size();
    state["slicing"]      = plater->is_slicing_any();
    // The selection object_select acts on: the assemble view's while it is shown, else Prepare's.
    GLCanvas3D* assemble = plater->get_assmeble_canvas3D();
    GLCanvas3D* scene    = assemble != nullptr && plater->get_current_canvas3D() == assemble ? assemble : plater->get_view3D_canvas3D();
    state["selection"]   = scene != nullptr ? selection_state(*scene) : json();
    // The session's print host uploads, as the upload queue lists them; `id` is its ID column.
    json uploads = json::array();
    if (const PrintHostQueueDialog* queue = frame->printhost_queue_dlg())
        for (size_t row = 0; row < queue->job_count(); ++row) {
            const PrintHostQueueDialog::JobInfo job = queue->job_info(row);
            json upload{{"id", row + 1}, {"state", job.state}, {"progress", job.progress}, {"host", job.host}, {"filename", job.filename}};
            if (!job.error.empty())
                upload["error"] = job.error;
            uploads.push_back(std::move(upload));
        }
    state["uploads"] = std::move(uploads);

    json notes = json::array();
    for (const NotificationSummary& n : plater->get_notification_manager()->summaries())
        notes.push_back({{"type", n.type}, {"level", n.level}, {"text", n.text}});
    state["notifications"] = std::move(notes);
    return state;
}

// Whether `condition` holds now. UI thread.
bool condition_holds(AutomationRuntime& runtime, const std::string& condition, const std::set<std::string>& own_jobs)
{
    if (condition == "ready")
        return runtime.ready();
    if (condition == "dialog")
        return runtime.modals().any_open();
    if (condition == "no_dialog")
        return !runtime.modals().any_open();
    if (condition == "idle") {
        if (runtime.modals().any_open() || runtime.jobs().any_running_except(own_jobs))
            return false;
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return true;
        if (plater->is_slicing_any() || !plater->get_ui_job_worker().is_idle())
            return false;
        // Gizmos working in the background (Simplify's calculation, a texture preview).
        const auto canvases = live_canvases();
        return std::none_of(canvases.begin(), canvases.end(), [](GLCanvas3D* c) { return c->get_gizmos_manager().is_busy(); });
    }
    throw ToolFailure("Unknown condition " + condition);
}

} // namespace

std::vector<std::string> log_tail(std::istream& in, const std::string& min_level, std::size_t lines)
{
    // The log file sink starts an entry with its level, "[error]\t" (set_log_path_and_level(), utils.cpp); a line
    // without one continues the message of the entry above it.
    static const std::vector<std::string> levels{"trace", "debug", "info", "warning", "error", "fatal"};
    const auto level_of = [](const std::string& line) {
        for (std::size_t i = 0; i < levels.size(); ++i)
            if (const std::string field = "[" + levels[i] + "]\t"; line.compare(0, field.size(), field) == 0)
                return int(i);
        return -1;
    };
    const int min = int(std::find(levels.begin(), levels.end(), min_level) - levels.begin());
    // The newest entries kept, each its first line and those that continue it, as many as fit in `lines`.
    std::deque<std::vector<std::string>> entries;
    std::size_t                          held = 0;
    // Lines before the file's first entry, which continue an entry of an earlier file, have no level to filter by.
    bool        keeping = min == 0;
    std::string line;
    while (std::getline(in, line)) {
        if (const int level = level_of(line); level >= 0) {
            keeping = level >= min;
            if (keeping)
                entries.emplace_back();
        } else if (keeping && entries.empty()) {
            entries.emplace_back();
        }
        if (!keeping)
            continue;
        entries.back().push_back(std::move(line));
        ++held;
        while (held > lines && entries.size() > 1) {
            held -= entries.front().size();
            entries.pop_front();
        }
    }
    // An entry longer than `lines` gives its first lines.
    std::vector<std::string> out;
    for (std::vector<std::string>& entry : entries)
        for (std::string& kept : entry)
            if (out.size() < lines)
                out.push_back(std::move(kept));
    return out;
}

void register_app_tools(CommandCatalog& catalog, [[maybe_unused]] bool cad)
{
    catalog.add(inspect_tool(
        "app_state", L("Get app state"),
        "Snapshot of OrcaSlicer. Call it first and whenever you are unsure what the app is doing. The app: version, platform "
        "and display backend, whether it is ready, the main window, the displays (each one's work area x, y, width and "
        "height, as `window_set` takes positions, its scale and whether it is primary), the MCP endpoint (mcp), python "
        "(available: Python runs "
        "in this build; `scripts_unrestricted`: a script's own file, network, process and thread access is not held to "
        "OrcaSlicer's folders, which only a person turns on, in Preferences > AI features > Python scripts or for one run "
        "with --unrestricted-scripts), `design_tab` "
        "(the Design tab is on, and `tool_call` runs its commands, `tool_list`'s group Design) and, when they do not run, "
        "`cad_note`, how a person turns the CAD feature on; account (`signed_in` to the Orca cloud, `bambu_signed_in` to Bambu "
        "Lab's), `network_plugin` (installed, loaded, version, error: the Bambu network plug-in as the app found it) and "
        "`config_folder` (presets, app configuration and logs). The view outside the Design tab (view: `sidebar_collapsed`, "
        "`sidebar_width`, `legend_collapsed`, `gcode_window`, projection, section and the display switches labels, overhangs, "
        "outline, wireframe, gridlines, axes and navigator, under the names `view_set` takes them). "
        "The project: current tab and mode, printer technology, name, path and unsaved changes, "
        "selected printer/process/filament presets, plates (count, current), object count, the selection (objects, the "
        "instances `[[object, instance]]` they are selected in and, when parts rather than whole instances are selected, "
        "parts `[[object, part]]`, in the indices `object_select` takes; `wipe_tower` when a prime tower is selected; canvas is the 3D "
        "view it belongs to), slicing state, "
        "the Preview's view type, time mode and layer range (1-based, with their Z), and the camera of the 3D view shown "
        "(camera: type, `look_at`, position, zoom, `click_px_per_mm`, elevation and azimuth, as `view_set` replies with it). What "
        "is going on: open modal "
        "dialogs, each with its title, buttons (`enabled: false` when disabled) and a message that reads it as shown: its "
        "text and its controls row by row, fields `[value]`, choices [value ▾], checkboxes `[x]` / [ ] / [-], radio and "
        "selector options (*) / ( ), list rows \"- row | cells\", secrets [******], \" (disabled)\" after a disabled "
        "control; menus closed unseen (a menu a call opened that no reply listed), `dialog_auto_answer` rules in force, "
        "notifications, print host uploads (state, progress, error), the open gizmo, running jobs and a running "
        "recording.",
        object_schema({}), [](const json&, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            return ToolResult::ok(ctx.run_ui([&runtime] { return app_state(runtime); }));
        }));

    NativeCommand wait = inspect_tool(
        "app_wait", L("Wait for a condition"),
        "Block until a condition holds or `timeout_s` passes (max 50). `for: \"ready\"` (startup finished), \"idle\" (no dialog, "
        "no running job, nothing slicing or arranging, no gizmo working in the background; a `script_run` run counts only "
        "through the calls it makes), \"dialog\" / \"no_dialog\", or \"job\" with a job id an earlier call returned, whose "
        "result is then the reply. On timeout the reply is `{satisfied: false}`, or for a job `{status: \"running\", job, "
        "note}`: call it again.",
        object_schema({{"for", enum_prop({"ready", "idle", "dialog", "no_dialog", "job"}, "What to wait for; default job when job "
                                                                                          "is given, else idle.")},
                       {"job", prop("string", "Job id, for \"job\".")},
                       {"timeout_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 50}, {"description", "Default 30."}}}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            const std::string what    = arg_string(args, "for", args.contains("job") ? "job" : "idle");
            const auto        timeout = std::chrono::milliseconds(int(arg_double(args, "timeout_s", 30) * 1000));
            const auto        start   = std::chrono::steady_clock::now();
            if (what == "job") {
                const std::string job = arg_string(args, "job");
                if (job.empty())
                    throw ToolFailure("job is required for \"job\"");
                require_own_job(ctx, runtime.jobs(), job, "waits for");
                if (auto r = runtime.jobs().wait(job, timeout, [&ctx] { return ctx.cancelled(); }))
                    return *r;
                return ToolResult::ok({{"status", "running"}, {"job", job}, {"note", "Still running; wait again."}});
            }
            const std::set<std::string> own = ctx.lineage();
            for (;;) {
                const bool holds = ctx.run_ui([&] { return json(condition_holds(runtime, what, own)); }).get<bool>();
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
                if (holds)
                    return ToolResult::ok({{"satisfied", true}, {"waited_ms", waited.count()}});
                if (waited >= timeout || ctx.cancelled())
                    return ToolResult::ok({{"satisfied", false}, {"waited_ms", waited.count()}});
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        });
    wait.journal = true; // a replay needs the same waits
    catalog.add(wait);

    catalog.add(inspect_tool(
        "app_log", L("Read the log"),
        "The last lines of OrcaSlicer's current log file, optionally only those at or above a level. An entry's level is "
        "the one its first line starts with (`[error]`); the lines of its message after the first come with it, and the "
        "reply holds whole entries.",
        object_schema({{"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000},
                                  {"description", "The most lines, of the newest whole entries; an entry longer than that gives its "
                                                  "first lines. Default 50."}}},
                       {"level", enum_prop({"trace", "debug", "info", "warning", "error", "fatal"}, "Minimum level.")}}),
        [](const json& args, ToolContext&) {
            const boost::filesystem::path file = get_log_file_name();
            if (file.empty())
                throw ToolFailure("No log file is open");
            flush_logs();
            boost::nowide::ifstream in(path_to_utf8(file).c_str());
            const std::vector<std::string> tail = log_tail(in, arg_string(args, "level", "trace"),
                                                           std::size_t(arg_int(args, "lines", 50)));
            return ToolResult::ok({{"file", path_to_utf8(file)}, {"lines", json(tail)}});
        }));

    // Undo and Redo act on the shown tab's history, as the Edit menu's do; history names one for automation, which acts
    // whatever tab is shown. Each has an action of its own, in the Speed Dial's section Commands.
    const json history = object_schema(
        {{"history", enum_prop({"design", "project"}, "The history: design, the Design tab's (with the CAD feature on), or project. "
                                                      "Default: the Design tab's while it is shown, else the project's.")}});
    // A person's pick does what the Edit menu's item does, and shows nothing where it would be greyed. An automated
    // call, action_run of its action included, acts on the history MenuItems::undo_redo() picks: the Design tab's through its
    // gate, whose refusals say why, and the project's as the Edit menu's item does.
    const auto history_command = [&history](std::string key, std::string title, std::string icon, std::string description, bool redo) {
        NativeCommand command = menu_command(std::move(key), std::move(title), std::move(description), history, json(),
                                            [redo](const json& args) { return MenuItems::undo_redo(args, redo); });
        const CommandFn picked = NativeCommands::ui_command(command.key, command.title, command.group, history,
                                                            [redo](const json& args) { return MenuItems::picked_undo_redo(args, redo); })
                                     .run;
        command.run = [picked, automated = std::move(command.run)](const json& args, ToolContext& ctx) {
            return ctx.person() ? picked(args, ctx) : automated(args, ctx);
        };
        command.has_own_action = true;
        command.action_group   = L("Commands");
        command.icon           = std::move(icon);
        return command;
    };
    NativeCommand undo = history_command(
        "undo", L("Undo"), "menu_undo",
        "Take back the last change of a history, as Edit > Undo does. history design is the Design tab's: the last feature "
        "added, edited, deleted, moved, shown or hidden, or renamed, or variable changed, or in a sketch session the last "
        "drawn shape; project is the project's. Without history, the Design tab's while it is shown, else the project's. "
        "Reply: history and, for the Design tab's, result: scope (design or sketch), `undo_depth`, `redo_depth`, features, "
        "bodies and generation; entities in a sketch.",
        false);
    NativeCommand redo = history_command("redo", L("Redo"), "menu_redo",
                                         "Put back what undo took back, until the next change, as Edit > Redo does, in the history "
                                         "undo takes. Reply as undo's.",
                                         true);
#ifdef SLIC3R_CAD
    // An example calls Design commands, so only a catalog that holds them documents the Design history.
    if (cad) {
        undo.examples = json::parse(R"json([
            {"title": "Take back an extrude, its sketch with it", "calls": [
                {"tool": "extrude", "args": {"width": 20, "height": 20, "distance": 5}},
                {"tool": "undo", "args": {"history": "design"}}]},
            {"title": "Take back the last shape drawn in a sketch", "calls": [
                {"tool": "sketch_create", "args": {"plane": "XY"}},
                {"tool": "sketch_draw", "args": {"type": "rectangle_2_point", "p0": [-15, -10], "p1": [15, 10]}},
                {"tool": "sketch_draw", "args": {"entities": [{"type": "circle_center", "center": [0, 0], "radius": 4}]}},
                {"tool": "undo", "args": {"history": "design"}}]}])json");
        redo.examples = json::parse(R"json([
            {"title": "Put back an extrude undo took back", "calls": [
                {"tool": "extrude", "args": {"width": 20, "height": 20, "distance": 5}},
                {"tool": "undo", "args": {"history": "design"}},
                {"tool": "redo", "args": {"history": "design"}}]},
            {"title": "Put back the shape undo took off a sketch", "calls": [
                {"tool": "sketch_create", "args": {"plane": "XY"}},
                {"tool": "sketch_draw", "args": {"type": "rectangle_2_point", "p0": [-15, -10], "p1": [15, 10]}},
                {"tool": "sketch_draw", "args": {"entities": [{"type": "circle_center", "center": [0, 0], "radius": 4}]}},
                {"tool": "undo", "args": {"history": "design"}},
                {"tool": "redo", "args": {"history": "design"}}]}])json");
    }
#endif
    catalog.add(std::move(undo));
    catalog.add(std::move(redo));

    // ---- The app's commands made from the Help and Plugins menus' items ----
    using NativeCommands::action;
    NativeCommand web_page = menu_command(
        "app_open_web_page", L("Open a web page"),
        "Open one of OrcaSlicer's web pages in the default web browser, as its menu item does: wiki, the OrcaSlicer wiki; "
        "youtube, the OrcaSlicer YouTube channel; `plugin_hub`, the plugin hub's page, where a person picks a plugin to install. "
        "Nothing opens in OrcaSlicer.",
        object_schema({{"page", enum_prop({"wiki", "youtube", "plugin_hub"}, "The page.")}}, {"page"}),
        json::array({action("open_wiki", L("Open Wiki"), L("Help"), "link_wiki_img", {{"page", "wiki"}},
                            "Open the OrcaSlicer wiki in the browser."),
                     action("open_youtube", L("Open YouTube Channel"), L("Help"), "", {{"page", "youtube"}},
                            "Open the OrcaSlicer YouTube channel in the browser."),
                     action("install_plugin", L("Install Plugin"), L("Plugins"), "", {{"page", "plugin_hub"}},
                            "Open the plugin hub's page in the default web browser, where a person picks a plugin to install; "
                            "nothing opens in OrcaSlicer.")}),
        [](const json& args) { return MenuItems::open_web_page(arg_string(args, "page")); });
    // The browser it opens reaches the network.
    web_page.open_world = true;
    catalog.add(std::move(web_page));
    const std::string updates = "Check for a newer OrcaSlicer.";
    NativeCommand check_updates = menu_command("app_check_for_updates", L("Check for Updates"), updates, object_schema({}),
                                              json::array({action("help_check_updates", L("Check for Updates"), L("Help"), "refresh",
                                                                  json::object(), updates)}),
                                              [](const json&) { return MenuItems::check_for_updates(); });
    // It asks the update server.
    check_updates.open_world = true;
    catalog.add(std::move(check_updates));
    const std::string folder = "Open the configuration folder in the file manager.";
    catalog.add(menu_command("app_show_config_folder", L("Show Configuration Folder"), folder, object_schema({}),
                            json::array({action("help_open_config_folder", L("Show Configuration Folder"), L("Help"), "open_project",
                                                json::object(), folder)}),
                            [](const json&) { return MenuItems::show_config_folder(); }));
    const std::string benchmark = "Time the 3D views on a test model. It asks first (Start or Cancel), then replaces the project with "
                                  "the OrcaSliced Combo as New Project does, which may ask about unsaved changes, slices it and shows "
                                  "the results.";
    catalog.add(menu_command("app_benchmark_3d_scene", L("Benchmark 3D Scene"), benchmark, object_schema({}),
                            json::array({action("help_benchmark_3d_scene", L("Benchmark 3D Scene"), L("Help"), "", json::object(),
                                                benchmark)}),
                            [](const json&) { return MenuItems::benchmark_3d_scene(); }));
}

}}} // namespace Slic3r::GUI::Mcp

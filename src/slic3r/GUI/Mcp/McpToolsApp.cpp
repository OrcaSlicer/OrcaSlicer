// get_state, wait and get_log: what an agent reads before and after acting.

#include "JobRegistry.hpp"
#include "McpPaths.hpp"
#include "libslic3r/Config.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "ModalTracker.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#if defined(__WXGTK__)
#include "slic3r/GUI/LinuxDisplayBackend.hpp"
#endif

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/gdicmn.h>
#include <wx/vector.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
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
json app_state(McpService& service)
{
    json state{{"version", SLIC3R_VERSION},
               {"platform", platform_name()},
               {"display_backend", display_backend()},
               {"ready", service.ready()},
               {"dialogs", service.modals().snapshot()},
               {"auto_answer_rules", service.modals().rules_json()},
               {"mcp", service.describe()},
               {"jobs", service.jobs().list()}};
    MainFrame* frame  = wxGetApp().mainframe;
    Plater*    plater = wxGetApp().plater();
    if (frame == nullptr || plater == nullptr)
        return state;

    const wxRect rect = frame->GetScreenRect();
    state["window"]   = {{"title", into_u8(frame->GetTitle())},
                         {"size", {rect.width, rect.height}},
                         {"position", {rect.x, rect.y}},
                         {"maximized", frame->IsMaximized()},
                         {"minimized", frame->IsIconized()},
                         {"shown", frame->IsShown()}};
    if (Notebook* tabs = frame->m_tabpanel; tabs != nullptr && tabs->GetSelection() != wxNOT_FOUND)
        state["tab"] = into_u8(tabs->GetPageName(size_t(tabs->GetSelection())));
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

    json notes = json::array();
    for (const NotificationSummary& n : plater->get_notification_manager()->summaries())
        notes.push_back({{"type", n.type}, {"level", n.level}, {"text", n.text}});
    state["notifications"] = std::move(notes);
    return state;
}

// Whether `condition` holds now. UI thread.
bool condition_holds(McpService& service, const std::string& condition, const std::set<std::string>& own_jobs)
{
    if (condition == "ready")
        return service.ready();
    if (condition == "dialog")
        return service.modals().any_open();
    if (condition == "no_dialog")
        return !service.modals().any_open();
    if (condition == "idle") {
        if (service.modals().any_open() || service.jobs().any_running_except(own_jobs))
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

void register_app_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    reg.add(inspect_tool(
        "get_state", "Get app state",
        "Snapshot of OrcaSlicer: version, platform, whether it is ready, current tab and mode, project, selected printer/process/"
        "filament presets, plates, object count, slicing state, the open gizmo, open modal dialogs, notifications and running "
        "jobs. Call it "
        "first and whenever you are unsure what the app is doing.",
        object_schema({}), [&service](const json&, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&service] { return app_state(service); })); }));

    ToolDef wait = inspect_tool(
        "wait", "Wait for a condition",
        "Block until a condition holds or timeout_s passes (max 50). for: \"ready\" (startup finished), \"idle\" (no dialog, "
        "no running job, nothing slicing or arranging, no gizmo working in the background), \"dialog\" / \"no_dialog\", or "
        "\"job\" with a job id returned by "
        "an earlier call - then the reply is that job's result. Returns {satisfied:false} on timeout; call it again.",
        object_schema({{"for", enum_prop({"ready", "idle", "dialog", "no_dialog", "job"}, "What to wait for.")},
                       {"job", prop("string", "Job id, for \"job\".")},
                       {"timeout_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 50}, {"description", "Default 30."}}}},
                      {"for"}),
        [&service](const json& args, ToolContext& ctx) {
            const std::string what    = arg_string(args, "for");
            const auto        timeout = std::chrono::milliseconds(int(arg_double(args, "timeout_s", 30) * 1000));
            const auto        start   = std::chrono::steady_clock::now();
            if (what == "job") {
                const std::string job = arg_string(args, "job");
                if (job.empty())
                    throw ToolFailure("job is required for \"job\"");
                if (auto r = service.jobs().wait(job, timeout))
                    return *r;
                return ToolResult::ok({{"status", "running"}, {"job", job}, {"note", "Still running; wait again."}});
            }
            const std::set<std::string> own = ctx.lineage();
            for (;;) {
                const bool holds = ctx.run_ui([&] { return json(condition_holds(service, what, own)); }).get<bool>();
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
                if (holds)
                    return ToolResult::ok({{"satisfied", true}, {"waited_ms", waited.count()}});
                if (waited >= timeout || ctx.cancelled())
                    return ToolResult::ok({{"satisfied", false}, {"waited_ms", waited.count()}});
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        });
    wait.journal = true; // a replay needs the same waits
    reg.add(wait);

    reg.add(inspect_tool(
        "get_log", "Read the log",
        "The last lines of OrcaSlicer's current log file, optionally only those at or above a level.",
        object_schema({{"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}, {"description", "Default 200."}}},
                       {"level", enum_prop({"trace", "debug", "info", "warning", "error", "fatal"}, "Minimum level.")}}),
        [](const json& args, ToolContext&) {
            const boost::filesystem::path file = get_log_file_name();
            if (file.empty())
                throw ToolFailure("No log file is open");
            static const std::vector<std::string> levels{"trace", "debug", "info", "warning", "error", "fatal"};
            const std::string min_level = arg_string(args, "level", "trace");
            const auto        min_it    = std::find(levels.begin(), levels.end(), min_level);
            const std::size_t lines     = std::size_t(arg_int(args, "lines", 200));
            flush_logs();
            boost::nowide::ifstream in(path_to_utf8(file).c_str());
            std::deque<std::string> tail;
            std::string             line;
            while (std::getline(in, line)) {
                bool keep = min_it == levels.begin();
                for (auto it = min_it; !keep && it != levels.end(); ++it)
                    keep = line.find("[" + *it + "]") != std::string::npos;
                if (!keep)
                    continue;
                tail.push_back(line);
                if (tail.size() > lines)
                    tail.pop_front();
            }
            return ToolResult::ok({{"file", path_to_utf8(file)}, {"lines", json(std::vector<std::string>(tail.begin(), tail.end()))}});
        }));
}

}}} // namespace Slic3r::GUI::Mcp

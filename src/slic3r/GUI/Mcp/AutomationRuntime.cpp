#include "AutomationRuntime.hpp"

#include "ActivityLog.hpp"
#include "AutomationRuntimePtr.hpp"
#include "FollowedThreads.hpp"
#include "JobRegistry.hpp"
#include "McpCore.hpp"
#include "McpRecorder.hpp"
#include "ModalTracker.hpp"
#include "UiInspector.hpp"
#include "UiInvoker.hpp"
#include "WebPages.hpp"
#include "WindowControl.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/SyntheticInput.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/thread.h>
#include <wx/timer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

class WxDispatcher final : public UiDispatcher
{
public:
    void post(std::function<void()> fn) override
    {
        // CallAfter queues a thread-safe event; without an app the closure never runs and its
        // caller gives up after the queue timeout.
        if (wxTheApp == nullptr)
            return;
        wxTheApp->CallAfter(std::move(fn));
#ifdef __WXMSW__
        // CallAfter wakes only wx's own loop. In a native modal loop, a menu's or a system dialog's, wx runs
        // queued events from a message hook when the loop takes a message off the thread's queue, so give it one
        // rather than wait for an unrelated message.
        wxWakeUpMainThread();
#endif
    }
};

// Whether the Prepare canvas has loaded OpenGL: its functions, shaders and the ImGui font.
bool canvas_gl_ready()
{
    Plater*     plater = wxGetApp().plater();
    GLCanvas3D* canvas = plater != nullptr ? plater->canvas3D() : nullptr;
    return canvas == nullptr || canvas->is_initialized();
}

// Whether the main window has tab pages still to build at idle.
bool pages_pending()
{
    const MainFrame* frame = wxGetApp().mainframe;
    return frame != nullptr && frame->prebuild_pending();
}

// The Plater's UI job worker, as McpCore watches it for jobs that calls start.
WorkerHooks ui_job_worker_hooks()
{
    WorkerHooks hooks;
    hooks.busy = [] {
        Plater* plater = wxGetApp().plater();
        return plater != nullptr && !plater->get_ui_job_worker().is_idle();
    };
    hooks.cancel = [] {
        if (Plater* plater = wxGetApp().plater())
            plater->get_ui_job_worker().cancel();
    };
    return hooks;
}

// The scope key of a plugin's calls, which no job id ("j<n>") can equal.
std::string plugin_scope_key(const std::string& plugin_key) { return "plugin:" + plugin_key; }

} // namespace

void AutomationRuntimeDeleter::operator()(AutomationRuntime* runtime) const
{
    runtime->stop();
    if (runtime->drain(std::chrono::seconds(10)))
        delete runtime;
    else
        BOOST_LOG_TRIVIAL(error) << "Automation runtime: a call did not return; leaving it in place";
}

AutomationRuntime::AutomationRuntime() = default;

// Reached only through AutomationRuntimeDeleter, after stop() joined the followed calls' threads and drain() saw
// every thread leave.
AutomationRuntime::~AutomationRuntime() = default;

GuardState AutomationRuntime::guards_on_ui() const
{
    GuardState g;
    g.closing   = m_stopping || wxTheApp == nullptr || wxGetApp().is_closing();
    g.suspended = m_suspended;
    // Whether a dialog is open is all a guard needs. Describing the dialogs reads every control of each, so it is left
    // to the replies that name them (UiInvoker's open_dialogs()): made for every queued closure, it would keep the UI
    // thread from the dialog's own events while many calls wait.
    if (m_modals) {
        wxDialog* const top = m_modals->top();
        g.modal             = top != nullptr;
        g.top_dialog        = m_modals->serial(top);
        // EndModal() hides a wx dialog at once; a native message box is never shown as a wx window.
        g.top_closing = top != nullptr && !is_native_dialog(top) && !top->IsShown();
    }
    return g;
}

bool AutomationRuntime::start(std::string& err)
{
    if (m_stopping) {
        err = "The automation runtime has stopped for exit";
        return false;
    }
    if (m_core == nullptr) {
        m_dispatcher = std::make_unique<WxDispatcher>();
        m_modals     = std::make_unique<ModalTracker>();
        m_recorder   = std::make_unique<McpRecorder>(*m_modals, [this] { return m_generation.load(); }, [this] { return m_suspended.load(); },
                                                   [this](std::function<void()> fn) { post_ui(std::move(fn)); });
        // The pane reads the log on the UI thread, once for any number of changes, finding the runtime again there:
        // it may be gone by the time the closure runs.
        m_activity   = std::make_unique<ActivityLog>(m_jobs, [this] {
            post_ui([] {
                if (AutomationRuntime* runtime = wxGetApp().automation_if_started(); runtime != nullptr && runtime->m_activity_listener)
                    runtime->m_activity_listener();
            });
        });
        // A call's window for taking menus is the inspector's: none before there is one, and it takes no menu
        // while the GUI instruments are off.
        m_invoker    = std::make_unique<UiInvoker>(*m_dispatcher, [this] { return guards_on_ui(); }, [this] { return m_generation.load(); },
                                                [this] { return m_modals->snapshot(); },
                                                [this](const std::string& tool, bool running) {
                                                    if (m_inspector)
                                                        m_inspector->ui_activity(tool, running);
                                                });
        // A plugin's calls are refused from the moment it unloads, before end_plugin_scope() comes.
        m_jobs.set_owner_liveness([](const std::string& plugin) { return PluginManager::instance().is_plugin_loaded(plugin); });
        CoreTiming timing;
        timing.reply_budget_cap = reply_budget_cap_from(boost::nowide::getenv("ORCA_MCP_REPLY_BUDGET_MS"));
        if (timing.reply_budget_cap.count() > 0)
            BOOST_LOG_TRIVIAL(info) << "MCP reply budget capped at " << timing.reply_budget_cap.count()
                                    << " ms by ORCA_MCP_REPLY_BUDGET_MS";
        m_core       = std::make_unique<McpCore>(NativeCommands::instance(), *m_invoker, m_jobs, &m_journal, timing,
                                                 ui_job_worker_hooks(), std::vector<CallObserver*>{m_recorder.get(), m_activity.get()});
        // A command reaches the runtime through its call's context (ToolContext::runtime()).
        m_core->set_runtime(this);
        m_modals->set_on_answered([this](int layer, const std::string& title, const std::string& message, const std::string& pattern,
                                         const std::string& button) { rule_answered(layer, title, message, pattern, button); });
        m_web_presses = std::make_shared<WebPresses>();
        m_modals->set_before_press([this] { release_web_presses(m_web_presses, nullptr); });
        BOOST_LOG_TRIVIAL(info) << "Automation runtime started";
    }
    return true;
}

void AutomationRuntime::enable_gui_instruments()
{
    // The inspector is built on the modal tracker, so nothing comes before start().
    if (m_instruments_on || m_modals == nullptr)
        return;
    if (m_inspector == nullptr)
        m_inspector = std::make_unique<UiInspector>(*m_modals, [this] { return m_generation.load(); });
    // Once stop() has taken the instruments down, a call still on the UI thread's stack gets an inspector with
    // nothing installed.
    if (m_stopping)
        return;
    m_instruments_on = true;
    track_window_states(true);
    m_inspector->capture_menus(true);
    if (ImGuiWrapper* imgui = wxGetApp().imgui())
        imgui->set_item_recording(true);
    // ImGui items are recorded as a frame draws them, and a view already shown draws none until it changes.
    if (GLCanvas3D* canvas = shown_canvas()) {
        canvas->set_as_dirty();
        wxWakeUpIdle();
    }
    BOOST_LOG_TRIVIAL(info) << "Automation GUI instruments on";
}

void AutomationRuntime::disable_gui_instruments()
{
    if (!m_instruments_on)
        return;
    m_instruments_on = false;
    m_inspector->release_held(nullptr);
    m_inspector->capture_menus(false);
    SyntheticInput::clear();
    if (wxTheApp != nullptr)
        if (ImGuiWrapper* imgui = wxGetApp().imgui())
            imgui->set_item_recording(false);
    track_window_states(false);
    BOOST_LOG_TRIVIAL(info) << "Automation GUI instruments off";
}

void AutomationRuntime::hold_gui_instruments()
{
    ++m_instrument_holds;
    enable_gui_instruments();
}

void AutomationRuntime::release_gui_instruments()
{
    if (m_instrument_holds > 0)
        --m_instrument_holds;
    sync_gui_instruments();
}

bool AutomationRuntime::gui_instruments_wanted() const
{
    if (m_instrument_holds > 0)
        return true;
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    return m_client_scope_open || !m_run_rule_layers.empty();
}

void AutomationRuntime::sync_gui_instruments()
{
    if (m_stopping)
        return;
    if (gui_instruments_wanted())
        enable_gui_instruments();
    else
        disable_gui_instruments();
}

void AutomationRuntime::post_gui_instruments_sync()
{
    // Found again on the UI thread: the runtime may be gone by the time the closure runs.
    if (m_dispatcher && !m_stopping)
        m_dispatcher->post([] {
            if (AutomationRuntime* runtime = wxGetApp().automation_if_started(); runtime != nullptr)
                runtime->sync_gui_instruments();
        });
}

UiInspector& AutomationRuntime::inspector()
{
    // Only a command reaches it, so the runtime is started and the inspector is made here if need be.
    if (!m_instruments_on)
        enable_gui_instruments();
    return *m_inspector;
}

void AutomationRuntime::stop()
{
    // Before the stop flag: a run's stop does nothing once the runtime is stopping, and it must cancel
    // the run's slice while the main window is still there.
    if (m_core != nullptr && !m_stopping)
        stop_followed();
    if (m_core == nullptr || m_stopping.exchange(true))
        return;
    BOOST_LOG_TRIVIAL(info) << "Automation runtime stopping";
    // Its file is finished on the recorder's own threads; this waits a little for them, not for the UI.
    m_recorder->shutdown(std::chrono::seconds(5));
    if (m_ready_timer)
        m_ready_timer->Stop();
    // Wake everything that waits, so running calls return.
    m_invoker->shutdown();
    m_jobs.wake_all();
    m_core->shutdown();
    // What a client left behind ends with the stop, also when no endpoint closed its scope first.
    close_client_scope();
    // The waits above are woken, so the followed calls return; one followed from here on is refused.
    m_followed.join_all();
    {
        // Run rules too: no run survives the stop.
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        for (const auto& [job, layers] : m_run_rule_layers)
            for (int layer : layers)
                m_modals->pop_rules(layer);
        m_run_rule_layers.clear();
    }
    release_web_presses(m_web_presses, nullptr);
    disable_gui_instruments();
}

bool AutomationRuntime::acquire_lease()
{
    if (m_stopping)
        return false;
    ++m_leases;
    // A stop that came meanwhile may already be draining: give the lease back rather than race it.
    if (m_stopping) {
        --m_leases;
        return false;
    }
    return true;
}

void AutomationRuntime::release_lease() { --m_leases; }

bool AutomationRuntime::drain(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // A lease's holder finds the core stopped and returns at once.
    while (m_leases.load() > 0) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (m_core == nullptr)
        return true;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return m_core->drain(std::max(left, std::chrono::milliseconds(0)));
}

AutoAnswerSet AutomationRuntime::set_auto_answer(std::vector<AutoAnswerRule> rules, const std::vector<std::string>& ancestry)
{
    // A plugin's call is owned by its plugin's scope when no run in its chain is nearer.
    const std::string plugin = ancestry.empty() ? std::string() : m_jobs.owner(ancestry.front());
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    const AutoAnswerSet set = [&]() -> AutoAnswerSet {
        // The nearest running replay or script in the caller's job chain owns the rules.
        for (const std::string& job : ancestry)
            if (auto it = m_run_rule_layers.find(job); it != m_run_rule_layers.end()) {
                const bool paused = m_paused_scopes.count(job) != 0;
                if (rules.empty())
                    return {"run", 0, paused};
                it->second.push_back(m_modals->push_rules(std::move(rules), paused));
                return {"run", it->second.back(), paused};
            }
        if (!plugin.empty()) {
            if (auto it = m_run_rule_layers.find(plugin_scope_key(plugin)); it != m_run_rule_layers.end()) {
                if (rules.empty())
                    return {"plugin", 0, false};
                it->second.push_back(m_modals->push_rules(std::move(rules)));
                return {"plugin", it->second.back(), false};
            }
            if (m_unloaded_plugins.count(plugin) != 0)
                throw ToolFailure("The plugin that made this call has been unloaded; its rules were not applied");
        }
        // A call that was running when the client scope closed must not bring its client's rules back.
        if (!m_client_scope_open)
            throw ToolFailure("The MCP server was turned off while the rules were being set; they were not applied");
        if (m_session_rule_layer != 0)
            m_modals->pop_rules(m_session_rule_layer);
        m_session_rule_layer = rules.empty() ? 0 : m_modals->push_rules(std::move(rules));
        return {"session", m_session_rule_layer, false};
    }();
    // Still under the rules lock, which an answer's report takes before it reaches the activity log (rule_answered()):
    // the log knows whose the layer is before any answer by it.
    if (set.layer != 0 && !ancestry.empty())
        m_activity->rules_set(ancestry.front(), set.layer);
    return set;
}

void AutomationRuntime::begin_run_scope(const std::string& job)
{
    {
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        m_run_rule_layers.emplace(job, std::vector<int>{});
    }
    // A run drives the GUI, also one started with the GUI instruments off.
    post_gui_instruments_sync();
}

void AutomationRuntime::end_run_scope(const std::string& job, bool wait_recording)
{
    // Noted before the scope goes: from then on a call of this run finds no live scope, and must not be
    // taken for a client's.
    if (m_recorder)
        m_recorder->owner_ended(job);
    {
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        auto it = m_run_rule_layers.find(job);
        if (it == m_run_rule_layers.end())
            return;
        if (m_modals)
            for (int layer : it->second)
                m_modals->pop_rules(layer);
        m_run_rule_layers.erase(it);
        m_paused_scopes.erase(job);
    }
    // A recording the run started ends with it, its file closed before the run's call returns.
    if (m_recorder)
        m_recorder->end_owned_by(job, "run_ended", wait_recording);
    release_run_presses(job);
    // After the presses' release, which is queued the same way: the scope may have been the last to drive the GUI.
    post_gui_instruments_sync();
}

void AutomationRuntime::set_run_scope_paused(const std::string& job, bool paused)
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    auto                        it = m_run_rule_layers.find(job);
    if (it == m_run_rule_layers.end())
        return;
    if (paused)
        m_paused_scopes.insert(job);
    else
        m_paused_scopes.erase(job);
    if (m_modals)
        for (int layer : it->second)
            m_modals->set_paused(layer, paused);
}

void AutomationRuntime::release_run_presses(const std::string& job)
{
    // Found again on the UI thread: the runtime may be gone by the time the closure runs.
    if (m_dispatcher && !m_stopping)
        m_dispatcher->post([job] {
            if (AutomationRuntime* runtime = wxGetApp().automation_if_started(); runtime != nullptr) {
                if (runtime->m_inspector)
                    runtime->m_inspector->release_held(&job);
                release_web_presses(runtime->m_web_presses, &job);
            }
        });
}

void AutomationRuntime::cancel_operations_of(const std::string& job)
{
    for (const std::string& op : m_jobs.running_operations())
        if (m_jobs.lineage(op).count(job) != 0 && !m_core->cancel_worker(op))
            if (Plater* plater = wxGetApp().plater())
                plater->cancel_slicing();
}

void AutomationRuntime::rule_answered(int layer, const std::string& title, const std::string& message, const std::string& pattern,
                                      const std::string& button)
{
    // Whose the layer is: a run's, a plugin's, else a client's.
    std::string       run, plugin;
    const std::string prefix = plugin_scope_key({});
    {
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        for (const auto& [owner, layers] : m_run_rule_layers)
            if (std::find(layers.begin(), layers.end(), layer) != layers.end()) {
                if (owner.compare(0, prefix.size(), prefix) == 0)
                    plugin = owner.substr(prefix.size());
                else
                    run = owner;
                break;
            }
    }
    m_activity->dialog_answered(layer, run, plugin, {{"title", title}, {"message", message}, {"match", pattern}, {"button", button}});
}

std::string AutomationRuntime::run_owner(const std::vector<std::string>& ancestry) const
{
    const std::string           plugin = ancestry.empty() ? std::string() : m_jobs.owner(ancestry.front());
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    for (const std::string& job : ancestry)
        if (m_run_rule_layers.count(job) != 0)
            return job;
    if (!plugin.empty() && m_run_rule_layers.count(plugin_scope_key(plugin)) != 0)
        return plugin_scope_key(plugin);
    // A call of an unloaded plugin must not leave anything nothing would end.
    if (!plugin.empty() && m_unloaded_plugins.count(plugin) != 0)
        throw ToolFailure("The plugin that made this call has been unloaded");
    return {};
}

bool AutomationRuntime::begin_plugin_scope(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    if (m_unloaded_plugins.count(plugin_key) != 0)
        return false;
    m_run_rule_layers.emplace(plugin_scope_key(plugin_key), std::vector<int>{});
    return true;
}

void AutomationRuntime::end_plugin_scope(const std::string& plugin_key)
{
    {
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        m_unloaded_plugins.insert(plugin_key);
    }
    // No call of the plugin starts from now on, one it was admitted for included, and the ones running stop,
    // as a stopped replay's steps do.
    m_jobs.block_owner(plugin_key);
    for (const std::string& job : m_jobs.running_owned_by(plugin_key))
        m_jobs.request_stop(job);
    // Not waited for: an unloading plugin's recording is closed by the recorder's own threads.
    end_run_scope(plugin_scope_key(plugin_key), false);
}

void AutomationRuntime::plugin_loaded(const std::string& plugin_key)
{
    {
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        m_unloaded_plugins.erase(plugin_key);
    }
    m_jobs.unblock_owner(plugin_key);
}

void AutomationRuntime::open_client_scope()
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    m_client_scope_open = true;
}

bool AutomationRuntime::client_scope_open() const
{
    std::lock_guard<std::mutex> lock(m_rules_mutex);
    return m_client_scope_open;
}

void AutomationRuntime::close_client_scope()
{
    // A client's recording ends with its scope; a run's carries on until the run ends.
    if (m_recorder)
        m_recorder->end_owned_by({}, "endpoint_closed", false);
    // So does a press a client's mouse_gesture left held, on a widget or a page.
    if (m_web_presses) {
        const std::string client;
        if (m_inspector)
            m_inspector->release_held(&client);
        release_web_presses(m_web_presses, &client);
    }
    {
        // A client's rules end with its scope; a person using the app afterwards must not have
        // dialogs answered for them.
        std::lock_guard<std::mutex> lock(m_rules_mutex);
        m_client_scope_open = false;
        if (m_session_rule_layer != 0)
            m_modals->pop_rules(m_session_rule_layer);
        m_session_rule_layer = 0;
    }
    // The endpoint may have been the last to drive the GUI.
    sync_gui_instruments();
}

void AutomationRuntime::suspend()
{
    // The main window is about to be rebuilt; runs stop while its plater is still the current one.
    stop_followed();
    if (m_recorder)
        m_recorder->suspend();
    m_suspended = true;
    ++m_generation;
}

void AutomationRuntime::resume()
{
    ++m_generation;
    m_suspended = false;
    if (m_recorder)
        m_recorder->resume();
}

void AutomationRuntime::mark_ready()
{
    if (m_ready || m_ready_timer)
        return;
    // Starting on Home, the Prepare canvas loads OpenGL at idle after post_init(), and until then a
    // 3MF save draws its thumbnail with no GL functions loaded. The main window builds its tab pages
    // at idle then too, and a page's web view registers its "wx" handler with a script it waits for in
    // a nested loop; on GTK, calls that came in meanwhile have left that wait unfinished for good,
    // and with it every later web view's window.wx, a dialog page's own buttons included. So ready
    // waits for the canvas and the pages. When the hidden canvas can't get a GL context, the first
    // draw of Prepare loads it instead, and the pages build only while no input comes, so ready stops
    // waiting after 10 s.
    const auto settled = [] { return canvas_gl_ready() && !pages_pending(); };
    if (settled()) {
        set_ready();
        return;
    }
    const auto since = std::chrono::steady_clock::now();
    m_ready_timer    = std::make_unique<wxTimer>();
    m_ready_timer->Bind(wxEVT_TIMER, [this, since, settled](wxTimerEvent&) {
        if (m_stopping || (!settled() && std::chrono::steady_clock::now() - since < std::chrono::seconds(10)))
            return;
        m_ready_timer->Stop();
        if (!canvas_gl_ready())
            BOOST_LOG_TRIVIAL(warning) << "Automation runtime ready before the 3D view loaded OpenGL";
        if (pages_pending())
            BOOST_LOG_TRIVIAL(warning) << "Automation runtime ready before the main window built its tab pages";
        set_ready();
    });
    m_ready_timer->Start(100);
}

void AutomationRuntime::set_ready()
{
    m_ready = true;
    for (std::function<void()>& fn : std::exchange(m_on_ready, {}))
        fn();
}

void AutomationRuntime::on_ready(std::function<void()> fn)
{
    if (m_ready)
        fn();
    else
        m_on_ready.push_back(std::move(fn));
}

void AutomationRuntime::post_ui(std::function<void()> fn)
{
    if (m_dispatcher)
        m_dispatcher->post(std::move(fn));
}

bool AutomationRuntime::follow(FollowedCall call)
{
    if (m_core == nullptr || m_stopping)
        return false;
    std::function<void()> stop = call.stop;
    // An exception that leaves a thread's function ends the process, so none leaves this one: a `ready` that throws
    // gives the call up, what the call throws is its error result, and what `ended` throws is logged.
    return m_followed.start(
        [this, call = std::move(call)]() mutable {
            const auto failed = [&call](const char* what) {
                BOOST_LOG_TRIVIAL(error) << "Automation runtime: following " << call.command.key << " failed: " << what;
            };
            try {
                if (call.ready && !call.ready(call.args))
                    return;
            } catch (const std::exception& e) {
                failed(e.what());
                return;
            } catch (...) {
                failed("unknown exception");
                return;
            }
            std::optional<ToolResult> r;
            try {
                r = call_to_end(call.command, call.args, call.person, call.origin);
            } catch (const std::exception& e) {
                r = ToolResult::error(std::string("Internal error: ") + e.what());
            } catch (...) {
                r = ToolResult::error("Internal error");
            }
            if (!call.ended)
                return;
            try {
                call.ended(r);
            } catch (const std::exception& e) {
                failed(e.what());
            } catch (...) {
                failed("unknown exception");
            }
        },
        std::move(stop));
}

void AutomationRuntime::stop_followed() { m_followed.stop_all(); }

std::optional<ToolResult> AutomationRuntime::call_to_end(const NativeCommand& def, const json& args, bool person, const json& origin)
{
    ToolResult        r      = person ? m_core->call_person(def, args, origin) : m_core->call(def, args, origin);
    const std::string status = r.is_error ? std::string() : r.data.value("status", "");
    if ((status != "running" && status != "blocked_by_dialog") || !r.data.contains("job"))
        return r;
    const std::string         job = r.data["job"].get<std::string>();
    std::optional<ToolResult> done;
    while (!done && !m_stopping) {
        try {
            done = m_jobs.wait(job, std::chrono::seconds(1));
        } catch (const ToolFailure& f) {
            done = ToolResult::error(f.what()); // the job's result was lost
        }
    }
    return done;
}

}}} // namespace Slic3r::GUI::Mcp

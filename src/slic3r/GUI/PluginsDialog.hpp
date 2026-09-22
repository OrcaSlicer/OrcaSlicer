#ifndef slic3r_PluginsDialog_hpp_
#define slic3r_PluginsDialog_hpp_

#include "Widgets/WebViewHostDialog.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "PluginSource.hpp"
#include "PluginStatus.hpp"
#include "PluginSort.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"

#include <atomic>
#include <boost/log/trivial.hpp>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <wx/evtloop.h>
#include <wx/app.h>
#include <wx/progdlg.h>
#include <wx/string.h>
#include <wx/timer.h>

#include <boost/filesystem.hpp>

class wxTimer;

namespace Slic3r {

class PluginCapabilityInterface;
struct PluginCapabilityId;
enum class PluginCapabilityType;

namespace GUI {

// Dialog-independent plugin-management actions, shared by the Plugins dialog and the speed dial:
// they never require the webview dialog to be open.

// Rescans local plugins and (optionally) re-fetches cloud metadata. Blocking: run off the UI
// thread. Used by PluginsDialog (behind its progress dialog) and GUI_App::refresh_plugins().
void refresh_plugin_metadata_blocking(bool fetch_cloud);

// Opens the Cloud plugin hub in the default browser. No dialog needed.
void open_plugin_hub();

// Synchronously installs a local plugin package (.py/.whl). Runs on the UI thread but keeps it
// responsive by performing the install on a worker behind a modal progress dialog. `parent` owns
// the overwrite prompt and the progress dialog. On success `message` carries the localized
// confirmation; on a user-cancelled overwrite it is empty; on failure it carries the reason.
bool install_local_plugin_package(const boost::filesystem::path& package_file, wxWindow* parent, wxString& message);

namespace detail {

// Shared worker + modal-progress machinery: pulse a progress dialog while `run` executes on a
// detached worker, then run `on_finish` back on the UI thread. `alive`, when non-null, gates both
// the pulse and `on_finish` so a worker outliving its dialog can't touch freed windows; pass null
// for a dialog-independent caller. `restore` runs after the progress dialog is destroyed and before
// `on_finish`, so a webview host can re-raise itself. `finish_after_dialog_destroyed` still calls
// `on_finish` (without touching the dialog) when the host died, so a waiting loop can exit.
template<typename Run, typename OnFinish>
void run_off_thread_with_progress(Run&& run,
                                  OnFinish&& on_finish,
                                  wxWindow* parent,
                                  const wxString& title,
                                  const wxString& message,
                                  int maximum,
                                  int style,
                                  std::shared_ptr<std::atomic<bool>> alive,
                                  bool finish_after_dialog_destroyed,
                                  std::function<void()> restore)
{
    wxProgressDialog* progress = new wxProgressDialog(title, message, maximum, parent, style);
    wxTimer* timer             = new wxTimer();

    timer->Bind(wxEVT_TIMER, [alive, progress, message](wxTimerEvent&) {
        if ((!alive || alive->load(std::memory_order_acquire)) && progress)
            progress->Pulse(message);
    });

    timer->Start(100);

    std::thread([alive,
                 progress,
                 timer,
                 run                                            = std::forward<Run>(run),
                 on_finish                                      = std::forward<OnFinish>(on_finish),
                 finish_after_dialog_destroyed,
                 restore                                        = std::move(restore)]() mutable {
        try {
            run();
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "Plugin dialog worker failed: " << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Plugin dialog worker failed with an unknown exception";
        }

        if (wxTheApp == nullptr)
            return;

        wxTheApp->CallAfter([alive,
                             progress,
                             timer,
                             on_finish = std::move(on_finish),
                             finish_after_dialog_destroyed,
                             restore   = std::move(restore)]() mutable {
            timer->Stop();
            delete timer;

            if (!alive || alive->load(std::memory_order_acquire)) {
                progress->Destroy();
                if (restore)
                    restore();
                on_finish();
            } else if (finish_after_dialog_destroyed) {
                on_finish();
            }
        });
    }).detach();
}

// Wait for a worker behind a progress dialog, returning its result (or rethrowing). The waiting
// loop stays responsive because it pumps the event loop the worker posts its completion into.
template<typename Run>
std::invoke_result_t<std::decay_t<Run>&> run_wait_with_progress(Run&& run,
                                                                wxWindow* parent,
                                                                const wxString& title,
                                                                const wxString& message,
                                                                int maximum,
                                                                int style,
                                                                std::shared_ptr<std::atomic<bool>> alive,
                                                                std::function<void()> restore)
{
    using Result = std::invoke_result_t<std::decay_t<Run>&>;

    bool finished = false;
    wxEventLoop loop;
    auto on_finish = [&finished, &loop]() {
        finished = true;
        if (loop.IsRunning())
            loop.Exit();
    };

    if constexpr (std::is_void_v<Result>) {
        struct WaitState
        {
            std::mutex mutex;
            std::exception_ptr exception;
        };

        auto state = std::make_shared<WaitState>();
        run_off_thread_with_progress(
            [run = std::forward<Run>(run), state]() mutable {
                try {
                    run();
                } catch (...) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->exception = std::current_exception();
                }
            },
            on_finish, parent, title, message, maximum, style, std::move(alive), /*finish_after_dialog_destroyed=*/true, std::move(restore));

        if (!finished)
            loop.Run();

        std::exception_ptr exception;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            exception = state->exception;
        }
        if (exception)
            std::rethrow_exception(exception);
    } else {
        using StoredResult = std::decay_t<Result>;
        struct WaitState
        {
            std::mutex mutex;
            std::optional<StoredResult> result;
            std::exception_ptr exception;
        };

        auto state = std::make_shared<WaitState>();
        run_off_thread_with_progress(
            [run = std::forward<Run>(run), state]() mutable {
                try {
                    StoredResult result = run();
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->result.emplace(std::move(result));
                } catch (...) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->exception = std::current_exception();
                }
            },
            on_finish, parent, title, message, maximum, style, std::move(alive), /*finish_after_dialog_destroyed=*/true, std::move(restore));

        if (!finished)
            loop.Run();

        std::optional<StoredResult> result;
        std::exception_ptr exception;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->result)
                result.emplace(std::move(*state->result));
            exception = state->exception;
        }
        if (exception)
            std::rethrow_exception(exception);
        return std::move(*result);
    }
}

} // namespace detail

class PluginsDialog : public Slic3r::GUI::WebViewHostDialog
{
public:
    PluginsDialog(wxWindow* parent,
                  wxWindowID id         = wxID_ANY,
                  const wxString& title = wxT(""),
                  const wxPoint& pos    = wxDefaultPosition,
                  const wxSize& size    = wxDefaultSize,
                  long style            = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX | wxRESIZE_BORDER);

    ~PluginsDialog();

    void set_open_terminal_dlg_fn();
    void update_plugin_dialog_ui();

private:
    void open_plugin_on_cloud(const std::string& sharing_token);
    void open_plugin_hub();
    void on_script_message(const nlohmann::json& payload) override;
    // Runs one web command on a clean main-loop stack; see on_script_message.
    void handle_web_command(const nlohmann::json& payload);
    // Re-raises this dialog after a transient modal it opened (file dialog, message box,
    // progress dialog). Native macOS panels end by re-activating the app's main window
    // (the mainframe) instead of this webview-hosting dialog, burying it; wx only
    // compensates for generic wxDialog modals (wxDialog::EndModal raises the parent).
    void restore_z_order();

    void send_plugins();
    void set_plugin_sort(const std::string& sort_key, const std::string& sort_order);
    nlohmann::json build_plugins_payload() const;

    bool get_descriptor(const std::string& plugin_key, Slic3r::PluginDescriptor& descriptor) const;

    void refresh_plugin_metadata_async(const wxString& title, const wxString& message, bool fetch_cloud);
    void prompt_for_missing_plugins();
    void refresh_plugins();
    void toggle_plugin(const std::string& plugin_key, bool enabled);
    void toggle_plugin_capability(const std::string& plugin_key, PluginCapabilityType type, const std::string& capability_name, bool enabled);
    void handle_plugin_menu_action(const std::string& plugin_key, const std::string& action);

    void install_plugin_from_file();
    bool install_plugin_package(const std::string& package_path);
    bool install_cloud_plugin(const std::string& uuid, const std::string& version, const wxString& name);
    void run_script_plugin_capability(const std::string& plugin_key, const std::string& capability_name);
    // Config tab. Both are scoped to the full capability ID: a request naming a
    // capability that is gone or not configurable is refused rather than served from, or written
    // to, some other entry.
    void send_capability_config(const PluginCapabilityId& id);
    void save_capability_config(const PluginCapabilityId& id, const nlohmann::json& config);
    void restore_capability_config(const PluginCapabilityId& id);
    // Pushes a one-line result into the web footer status bar (level: "success" | "warn" | "error" | "info"),
    // used for every plugin/capability operation instead of a modal box so the dialog stays non-disruptive.
    void show_status(const wxString& message, const char* level);
    // Best-effort human-readable name for a plugin_key (falls back to the key itself).
    wxString plugin_display_name(const std::string& plugin_key) const;
    // Turns the pending "Activating..." status into "Activated"/"Failed to activate" once an
    // asynchronous plugin load reported via update_plugin_dialog_ui() finishes. No-op otherwise.
    void resolve_pending_activation();
    void update_plugin(const std::string& plugin_key);

    void open_plugin_folder(const Slic3r::PluginDescriptor& plugin);
    void delete_local_plugin(const Slic3r::PluginDescriptor& plugin);
    void unsubscribe_cloud_plugin(const Slic3r::PluginDescriptor& plugin);
    void reload_local_plugin(const std::string& plugin_key, bool clear_cache);
    void reinstall_cloud_plugin(const Slic3r::PluginDescriptor& plugin);

    // In the future, we can allow users to choose which plugin version they want to install.
    template<typename Run, typename OnFinish>
    void run_with_dialog(Run&& run,
                         OnFinish&& on_finish,
                         const wxString& title,
                         const wxString& message,
                         int maximum = 100,
                         int style   = wxPD_APP_MODAL | wxPD_AUTO_HIDE, // | wxPD_CAN_ABORT for cancel button
                         bool finish_after_dialog_destroyed = false)
    {
        detail::run_off_thread_with_progress(std::forward<Run>(run), std::forward<OnFinish>(on_finish), this, title, message, maximum, style,
                                             m_alive, finish_after_dialog_destroyed, [this] { restore_z_order(); });
    }

    template<typename Run>
    std::invoke_result_t<std::decay_t<Run>&> run_with_dialog_wait(Run&& run,
                                                                  const wxString& title,
                                                                  const wxString& message,
                                                                  int maximum = 100,
                                                                  int style   = wxPD_APP_MODAL | wxPD_AUTO_HIDE)
    {
        return detail::run_wait_with_progress(std::forward<Run>(run), this, title, message, maximum, style, m_alive, [this] { restore_z_order(); });
    }

    std::function<void()> m_open_terminal_dlg_fn;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    PluginSortKey m_plugin_sort_key       = PluginSortKey::None;
    PluginSortOrder m_plugin_sort_order   = PluginSortOrder::Asc;

    // Plugin whose asynchronous activation is in flight, awaited by resolve_pending_activation().
    // Empty when no activation is pending.
    std::string m_activating_plugin_key;
};

} // namespace GUI
} // namespace Slic3r

#endif

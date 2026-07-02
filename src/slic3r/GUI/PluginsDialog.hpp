#ifndef slic3r_PluginsDialog_hpp_
#define slic3r_PluginsDialog_hpp_

#include "Widgets/WebViewHostDialog.hpp"
#include "PluginSource.hpp"
#include "PluginStatus.hpp"
#include "PluginSort.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"

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
#include <wx/progdlg.h>
#include <wx/string.h>
#include <wx/timer.h>

class wxTimer;

namespace Slic3r {

struct LoadedPluginCapability;
enum class PluginCapabilityType;

namespace GUI {

class PluginsDialog : public Slic3r::GUI::WebViewHostDialog
{
public:
    PluginsDialog(wxWindow* parent,
                  wxWindowID id         = wxID_ANY,
                  const wxString& title = wxT(""),
                  const wxPoint& pos    = wxDefaultPosition,
                  const wxSize& size    = wxDefaultSize,
                  long style            = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX);

    ~PluginsDialog();

    void set_open_terminal_dlg_fn();
    void update_plugin_dialog_ui();

private:
    void open_plugin_on_cloud(const std::string& sharing_token);
    void open_plugin_hub();
    void on_script_message(const nlohmann::json& payload) override;

    void send_plugins();
    void set_plugin_sort(const std::string& sort_key, const std::string& sort_order);
    nlohmann::json build_plugins_payload() const;

    bool get_descriptor(const std::string& plugin_key, Slic3r::PluginDescriptor& descriptor) const;
    std::shared_ptr<LoadedPluginCapability> get_capability(const std::string& plugin_key, PluginCapabilityType type, const std::string& capability_name) const;

    void refresh_plugin_catalog_async(const wxString& title, const wxString& message, bool fetch_cloud);
    void refresh_plugins();
    void toggle_plugin(const std::string& plugin_key, bool enabled);
    void toggle_plugin_capability(const std::string& plugin_key, PluginCapabilityType type, const std::string& capability_name, bool enabled);
    void handle_plugin_menu_action(const std::string& plugin_key, const std::string& action);

    void install_plugin_from_file();
    bool install_plugin_package(const std::string& package_path);
    bool install_cloud_plugin(const std::string& uuid, const std::string& version, const wxString& name);
    void run_script_plugin(const std::string& plugin_key, const std::string& capability_name);
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
    void reinstall_local_plugin(const std::string& plugin_key);
    void reinstall_cloud_plugin(const Slic3r::PluginDescriptor& plugin);
    void delete_mine_local_and_cloud_plugin(const std::string& plugin_key);

    // In the future, we can allow users to choose which plugin version they want to install.
    template<typename Run, typename OnFinish>
    void run_with_dialog(Run&& run,
                         OnFinish&& on_finish,
                         const wxString& title,
                         const wxString& message,
                         int maximum = 100,
                         int style   = wxPD_APP_MODAL | wxPD_AUTO_HIDE)
    {
        wxProgressDialog* progress = new wxProgressDialog(title, message, maximum, this, style);
        wxTimer* timer             = new wxTimer();

        timer->Bind(wxEVT_TIMER, [progress, message](wxTimerEvent&) {
            if (progress)
                progress->Pulse(message);
        });

        timer->Start(100);

        std::thread([this,
                     progress,
                     timer,
                     run       = std::forward<Run>(run),
                     on_finish = std::forward<OnFinish>(on_finish)]() mutable {
            try {
                run();
            } catch (...) {
            }

            CallAfter([progress, timer, on_finish = std::move(on_finish)]() mutable {
                timer->Stop();
                delete timer;

                progress->Destroy();
                on_finish();
            });
        }).detach();
    }

    template<typename Run>
    std::invoke_result_t<std::decay_t<Run>&> run_with_dialog_wait(Run&& run,
                                                                  const wxString& title,
                                                                  const wxString& message,
                                                                  int maximum = 100,
                                                                  int style   = wxPD_APP_MODAL | wxPD_AUTO_HIDE)
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
            run_with_dialog(
                [run = std::forward<Run>(run), state]() mutable {
                    try {
                        run();
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->exception = std::current_exception();
                    }
                },
                on_finish, title, message, maximum, style);

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
            run_with_dialog(
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
                on_finish, title, message, maximum, style);

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

    std::function<void()> m_open_terminal_dlg_fn;
    PluginSortKey m_plugin_sort_key       = PluginSortKey::Status;
    PluginSortOrder m_plugin_sort_order   = PluginSortOrder::Asc;

    // Serializes run_script_plugin. With main-thread execution a plugin's orca.host.ui modal
    // (message/show_dialog) or the result message box pumps a nested event loop, which could
    // re-dispatch the web "run_script_plugin" command and start a second, overlapping run.
    bool m_script_running = false;

    // Plugin whose asynchronous activation is in flight, awaited by resolve_pending_activation().
    // Empty when no activation is pending.
    std::string m_activating_plugin_key;
};

} // namespace GUI
} // namespace Slic3r

#endif

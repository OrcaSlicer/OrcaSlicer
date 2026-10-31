#pragma once

#include "McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// Posts closures to the UI thread. The app implementation uses wxApp::CallAfter; tests drain a
// queue by hand to play the UI thread.
class UiDispatcher
{
public:
    virtual ~UiDispatcher() = default;
    virtual void post(std::function<void()> fn) = 0;
};

// App state the guards need, read on the UI thread when a closure is about to run.
struct GuardState
{
    bool           closing   = false; // the app is shutting down
    bool           suspended = false; // the main frame is being recreated (language change)
    bool           modal     = false; // a modal dialog or an ImGui modal popup is open
    nlohmann::json dialogs   = nlohmann::json::array();
    std::uint64_t  top_dialog = 0;    // the topmost modal dialog's serial (ModalTracker::serial()), 0 for none
};

// Runs closures on the UI thread for tool handlers, which run on other threads. A closure re-checks
// the guards on the UI thread before doing anything, because the app can change between posting
// and running (a modal can open, the app can start closing, the UI can be rebuilt). A caller gives
// up only on a closure that has not started, which then never runs; once started, the closure may
// use the caller's stack, so the caller waits for it to finish, through shutdown too.
class UiInvoker
{
public:
    // `generation` (any thread) counts UI rebuilds; a closure posted before a rebuild does not run
    // after it. `open_dialogs` (any thread) describes the open modals outermost first, as
    // ModalTracker::snapshot() does; it names a native dialog that holds the UI thread when the
    // UI thread itself can't answer. `activity` (UI thread) is told when a closure that passed its
    // guards starts (`running` true) and when it ends, with the calling tool's name: the GUI
    // automation keeps native menus from opening while a call acts on the UI.
    UiInvoker(UiDispatcher& dispatcher, std::function<GuardState()> guards_on_ui, std::function<uint64_t()> generation = {},
              std::function<nlohmann::json()> open_dialogs = {},
              std::function<void(const std::string& tool, bool running)> activity = {});
    ~UiInvoker();

    // Runs `fn` on the UI thread under `guard` and returns its value. Throws ToolFailure when a
    // guard refuses, when the UI thread does not pick the closure up within `queue_timeout`
    // (the closure is then abandoned and never runs; the failure names the native dialog holding
    // the UI thread, if that is why), or on shutdown. `tool` names the caller in busy messages.
    nlohmann::json run(GuardClass guard, const std::string& tool, const std::function<nlohmann::json()>& fn,
                       std::chrono::milliseconds queue_timeout = std::chrono::seconds(10));

    // The open modals from `open_dialogs`, read without the UI thread; an empty array without one.
    nlohmann::json open_dialogs() const;

    // UI-thread state for the request thread's blocked_by_dialog decision: whether any UI call is
    // running (i.e. we are inside its nested event loop) and the guard state. nullopt when the UI
    // thread does not answer within `timeout`.
    struct Probe
    {
        bool        call_running = false;
        std::string running_tool;
        GuardState  guards;
    };
    std::optional<Probe> probe(std::chrono::milliseconds timeout);

    // Applies the mutate guard from inside a closure that run() is executing under a weaker class:
    // the GUI input tools run as recover calls so they can answer the topmost dialog, and call
    // this once their target turns out to be elsewhere. Throws ToolFailure when another call's
    // nested loop is running or a modal dialog is open. UI thread only.
    void require_mutate() const;

    // Wakes every waiting caller with "shutting down"; closures that run afterwards do nothing.
    void shutdown();
    bool is_shutdown() const;

private:
    struct Shared;
    std::shared_ptr<Shared> m_shared;
    UiDispatcher&           m_dispatcher;
};

// True when the topmost (last) of `dialogs`, a snapshot like ModalTracker::snapshot(), is native.
// A native dialog can hold the UI thread: on macOS, file and folder pickers and message boxes pause
// wx's pending events, and with them every closure posted to the UI thread, until they close.
bool topmost_dialog_is_native(const nlohmann::json& dialogs);

}}} // namespace Slic3r::GUI::Mcp

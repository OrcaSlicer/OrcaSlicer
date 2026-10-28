#include "McpTypes.hpp"
#include "UiInvoker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

enum class CallState { Queued, Running, Done, Abandoned };

struct UiCall
{
    std::mutex              mutex;
    std::condition_variable cv;
    CallState               state = CallState::Queued;
    nlohmann::json          value;
    std::string             error;
    nlohmann::json          error_details;
};

} // namespace

// State shared with posted closures, which may run after the invoker is gone.
struct UiInvoker::Shared
{
    std::function<GuardState()> guards_on_ui;
    std::atomic<bool>           stopped{false};

    // UI-thread only: the calls currently executing, outermost first. More than one means a call is
    // running inside another call's nested event loop.
    std::vector<std::string> running_tools;

    std::mutex                           waiters_mutex;
    std::vector<std::weak_ptr<UiCall>>   waiters;
};

UiInvoker::UiInvoker(UiDispatcher& dispatcher, std::function<GuardState()> guards_on_ui)
    : m_shared(std::make_shared<Shared>()), m_dispatcher(dispatcher)
{
    m_shared->guards_on_ui = std::move(guards_on_ui);
}

UiInvoker::~UiInvoker() { shutdown(); }

nlohmann::json UiInvoker::run(GuardClass guard, const std::string& tool, const std::function<nlohmann::json()>& fn,
                              std::chrono::milliseconds queue_timeout)
{
    if (m_shared->stopped)
        throw ToolFailure("OrcaSlicer is shutting down");

    auto call = std::make_shared<UiCall>();
    {
        std::lock_guard<std::mutex> lock(m_shared->waiters_mutex);
        auto& w = m_shared->waiters;
        w.erase(std::remove_if(w.begin(), w.end(), [](const std::weak_ptr<UiCall>& c) { return c.expired(); }), w.end());
        w.push_back(call);
    }

    m_dispatcher.post([shared = m_shared, call, guard, tool, fn]() {
        {
            std::lock_guard<std::mutex> lock(call->mutex);
            if (call->state != CallState::Queued)
                return; // abandoned by its caller
            call->state = CallState::Running;
        }
        std::string    error;
        nlohmann::json details;
        nlohmann::json value;
        if (shared->stopped) {
            error = "OrcaSlicer is shutting down";
        } else {
            const GuardState g = shared->guards_on_ui ? shared->guards_on_ui() : GuardState{};
            if (g.closing)
                error = "OrcaSlicer is shutting down";
            else if (g.suspended)
                error = "The UI is being recreated (language change); retry in a moment";
            else if (guard == GuardClass::Mutate && g.modal) {
                error   = "A modal dialog is open; answer it first (ui_snapshot, dialog_respond)";
                details = {{"dialogs", g.dialogs}};
            } else if (guard == GuardClass::Mutate && !shared->running_tools.empty())
                error = "Busy: " + shared->running_tools.front() + " is still running; only inspection and dialog tools can run now";
        }
        if (error.empty()) {
            shared->running_tools.push_back(tool);
            try {
                value = fn();
            } catch (const ToolFailure& f) {
                error   = f.what();
                details = f.details;
            } catch (const std::exception& e) {
                error = std::string("Internal error: ") + e.what();
            } catch (...) {
                error = "Internal error";
            }
            shared->running_tools.pop_back();
        }
        {
            std::lock_guard<std::mutex> lock(call->mutex);
            call->state         = CallState::Done;
            call->value         = std::move(value);
            call->error         = std::move(error);
            call->error_details = std::move(details);
        }
        call->cv.notify_all();
    });

    std::unique_lock<std::mutex> lock(call->mutex);
    call->cv.wait_for(lock, queue_timeout, [&] { return call->state != CallState::Queued || m_shared->stopped; });
    if (call->state == CallState::Queued) {
        call->state = CallState::Abandoned;
        throw ToolFailure(m_shared->stopped ? "OrcaSlicer is shutting down"
                                            : "The UI thread is busy and did not run the call; it was not run, so it is safe to retry");
    }
    // Once running, the closure finishes on its own; wait for it (or for shutdown).
    call->cv.wait(lock, [&] { return call->state == CallState::Done || m_shared->stopped; });
    if (call->state != CallState::Done)
        throw ToolFailure("OrcaSlicer is shutting down");
    if (!call->error.empty())
        throw ToolFailure(call->error, call->error_details);
    return std::move(call->value);
}

std::optional<UiInvoker::Probe> UiInvoker::probe(std::chrono::milliseconds timeout)
{
    if (m_shared->stopped)
        return std::nullopt;
    struct ProbeCall
    {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    done = false;
        Probe                   probe;
    };
    auto call = std::make_shared<ProbeCall>();
    m_dispatcher.post([shared = m_shared, call]() {
        Probe p;
        p.call_running = !shared->running_tools.empty();
        if (p.call_running)
            p.running_tool = shared->running_tools.front();
        if (!shared->stopped && shared->guards_on_ui)
            p.guards = shared->guards_on_ui();
        {
            std::lock_guard<std::mutex> lock(call->mutex);
            call->probe = std::move(p);
            call->done  = true;
        }
        call->cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(call->mutex);
    if (!call->cv.wait_for(lock, timeout, [&] { return call->done; }))
        return std::nullopt;
    return call->probe;
}

void UiInvoker::require_mutate() const
{
    // The calling closure is the last entry; anything before it is a call whose nested loop we
    // are running in.
    if (m_shared->guards_on_ui) {
        const GuardState g = m_shared->guards_on_ui();
        if (g.modal)
            throw ToolFailure("A modal dialog is open; answer it first (ui_snapshot, dialog_respond)", {{"dialogs", g.dialogs}});
    }
    if (m_shared->running_tools.size() > 1)
        throw ToolFailure("Busy: " + m_shared->running_tools.front() +
                          " is still running; only inspection and dialog tools can run now");
}

void UiInvoker::shutdown()
{
    if (m_shared->stopped.exchange(true))
        return;
    std::vector<std::weak_ptr<UiCall>> waiters;
    {
        std::lock_guard<std::mutex> lock(m_shared->waiters_mutex);
        waiters.swap(m_shared->waiters);
    }
    for (auto& weak : waiters)
        if (auto call = weak.lock()) {
            { std::lock_guard<std::mutex> lock(call->mutex); }
            call->cv.notify_all();
        }
}

bool UiInvoker::is_shutdown() const { return m_shared->stopped; }

}}} // namespace Slic3r::GUI::Mcp

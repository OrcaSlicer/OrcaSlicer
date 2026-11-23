#pragma once

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/Mcp/JobRegistry.hpp"
#include "slic3r/GUI/Mcp/McpCore.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/UiInvoker.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Fixtures of the automation core's tests. The slic3rutils suite uses them too, for the cases that run the
// core with a type of the GUI library.
namespace Slic3r { namespace GUI { namespace Mcp { namespace Test {

inline NativeCommand command(std::string key, bool has_own_action = false)
{
    NativeCommand c;
    c.key            = std::move(key);
    c.has_own_action = has_own_action;
    return c;
}

// Holds one command, as a plugin's operations are held while it is loaded.
class OneCommandSource final : public CommandSource
{
public:
    explicit OneCommandSource(std::string key) : m_command(std::make_shared<const NativeCommand>(command(std::move(key)))) {}

    std::shared_ptr<const NativeCommand> find(const std::string& key) const override
    {
        return key == m_command->key ? m_command : nullptr;
    }
    std::vector<std::shared_ptr<const NativeCommand>> all() const override { return {m_command}; }

private:
    std::shared_ptr<const NativeCommand> m_command;
};

// Runs handlers directly on the calling thread, as if every call were inline.
class DirectCaller final : public ToolCaller
{
public:
    std::vector<std::string>    called;  // the tools it ran, in order
    std::vector<nlohmann::json> origins; // and who the protocol said made each call

    ToolResult call(const NativeCommand& def, const nlohmann::json& args, const nlohmann::json& origin = nlohmann::json()) override
    {
        called.push_back(def.key);
        origins.push_back(origin);
        struct Context final : ToolContext
        {
            std::string id;
            nlohmann::json run_ui(const std::function<nlohmann::json()>& fn) override { return fn(); }
            nlohmann::json observe_ui(const std::function<nlohmann::json()>& fn) override { return fn(); }
            const std::string& job_id() const override { return id; }
            bool cancelled() const override { return false; }
            ToolResult call_child(const std::string&, const nlohmann::json&) override { return ToolResult::error("no children"); }
            std::set<std::string> lineage() const override { return {}; }
            void set_journal_args(nlohmann::json) override {}
            void begin_operation() override {}
        } ctx;
        return def.run(args, ctx);
    }
};

// A UI thread with an event loop. Closures may run a nested loop (loop_until) the way a modal
// dialog does, so other closures keep running while they are on the stack.
class FakeUiThread final : public UiDispatcher
{
public:
    FakeUiThread() : m_thread([this] { loop_until([this] { return m_stop.load(); }); }) {}
    ~FakeUiThread() override
    {
        m_stop = true;
        m_cv.notify_all();
        m_thread.join();
    }

    void post(std::function<void()> fn) override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(fn));
        }
        m_cv.notify_all();
    }

    // Runs queued closures until `done` returns true. Call it only on the UI thread.
    void loop_until(const std::function<bool()>& done)
    {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::milliseconds(5), [&] { return !m_queue.empty(); });
                if (m_queue.empty()) {
                    lock.unlock();
                    if (done())
                        return;
                    continue;
                }
                fn = std::move(m_queue.front());
                m_queue.pop_front();
            }
            fn();
            if (done())
                return;
        }
    }

private:
    std::mutex                        m_mutex;
    std::condition_variable           m_cv;
    std::deque<std::function<void()>> m_queue;
    std::atomic<bool>                 m_stop{false};
    std::thread                       m_thread;
};

struct Harness
{
    FakeUiThread ui;
    std::mutex   guards_mutex;
    GuardState   guards;
    // The guards' dialogs double as the snapshot ModalTracker publishes to other threads.
    UiInvoker    invoker{ui,
                      [this] {
                          std::lock_guard<std::mutex> lock(guards_mutex);
                          return guards;
                      },
                      {},
                      [this] {
                          std::lock_guard<std::mutex> lock(guards_mutex);
                          return guards.dialogs;
                      }};
    JobRegistry  jobs;
    Journal      journal;
    CommandCatalog registry;
    CoreTiming   timing = [] {
        CoreTiming t;
        t.soft_budget   = std::chrono::milliseconds(100);
        t.probe_timeout = std::chrono::milliseconds(500);
        return t;
    }();
    std::unique_ptr<McpCore> core;

    void start() { core = std::make_unique<McpCore>(registry, invoker, jobs, &journal, timing); }

    void set_modal(bool modal, nlohmann::json dialogs = nlohmann::json::array())
    {
        std::lock_guard<std::mutex> lock(guards_mutex);
        guards.modal   = modal;
        guards.dialogs = std::move(dialogs);
    }

    // A modal dialog's nested event loop on the UI thread, open until `release` is set.
    void modal_loop(std::atomic<bool>& release, nlohmann::json dialogs = nlohmann::json::array({{{"title", "Dialog"}}}))
    {
        set_modal(true, std::move(dialogs));
        ui.loop_until([&] { return release.load(); });
        set_modal(false);
    }

    // Dialogs that hold the UI thread until `release` is set: they are published, but nothing posted
    // meanwhile runs, as when macOS pauses wx's pending events behind a native file picker.
    void hold_ui(std::atomic<bool>& release, nlohmann::json dialogs)
    {
        set_modal(true, std::move(dialogs));
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        set_modal(false);
    }

    ~Harness()
    {
        invoker.shutdown();
        jobs.wake_all();
        if (core)
            core->shutdown();
    }

    ToolResult call(const std::string& tool, const nlohmann::json& args = nlohmann::json::object())
    {
        return core->call(*registry.find(tool), args);
    }
};

}}}} // namespace Slic3r::GUI::Mcp::Test

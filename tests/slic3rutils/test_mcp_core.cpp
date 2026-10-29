#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/JobRegistry.hpp"
#include "slic3r/GUI/Mcp/McpCore.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/GUI/Mcp/UiInvoker.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <vector>
#include <future>
#include <mutex>
#include <string>
#include <thread>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

// Plays the UI thread: closures queue up until the test drains them on its own thread.
class ManualDispatcher final : public UiDispatcher
{
public:
    void post(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(fn));
    }

    // Runs queued closures (including ones queued while running) until the queue is empty.
    void drain()
    {
        for (;;) {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_queue.empty())
                    return;
                fn = std::move(m_queue.front());
                m_queue.pop_front();
            }
            fn();
        }
    }

    // Keeps draining until `done` returns true or `timeout` passes, like an event loop.
    bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = 5000ms)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            drain();
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

private:
    std::mutex                        m_mutex;
    std::deque<std::function<void()>> m_queue;
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
                m_cv.wait_for(lock, 5ms, [&] { return !m_queue.empty(); });
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

// A file picker as ModalTracker describes it: native, and untitled on macOS.
const json kNativePicker = json::array({{{"title", ""}, {"message", "Choose one or more files"}, {"native", true}}});

// Sets `flag` when the test leaves, failed assertions included, so a closure holding the fake UI
// thread lets go before the harness joins that thread. Declare the flag before the harness and this
// after it: the flag must outlive that join, and be set before it starts.
struct SetOnExit
{
    std::atomic<bool>& flag;
    ~SetOnExit() { flag = true; }
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
    ToolRegistry registry;
    CoreTiming   timing = [] {
        CoreTiming t;
        t.soft_budget   = 100ms;
        t.probe_timeout = 500ms;
        return t;
    }();
    std::unique_ptr<McpCore> core;

    void start() { core = std::make_unique<McpCore>(registry, invoker, jobs, &journal, timing); }

    void set_modal(bool modal, json dialogs = json::array())
    {
        std::lock_guard<std::mutex> lock(guards_mutex);
        guards.modal   = modal;
        guards.dialogs = std::move(dialogs);
    }

    // A modal dialog's nested event loop on the UI thread, open until `release` is set.
    void modal_loop(std::atomic<bool>& release, json dialogs = json::array({{{"title", "Dialog"}}}))
    {
        set_modal(true, std::move(dialogs));
        ui.loop_until([&] { return release.load(); });
        set_modal(false);
    }

    // Dialogs that hold the UI thread until `release` is set: they are published, but nothing posted
    // meanwhile runs, as when macOS pauses wx's pending events behind a native file picker.
    void hold_ui(std::atomic<bool>& release, json dialogs)
    {
        set_modal(true, std::move(dialogs));
        while (!release)
            std::this_thread::sleep_for(5ms);
        set_modal(false);
    }

    ~Harness()
    {
        invoker.shutdown();
        jobs.wake_all();
        if (core)
            core->shutdown();
    }

    ToolResult call(const std::string& tool, const json& args = json::object()) { return core->call(*registry.find(tool), args); }
};

ToolDef tool(const std::string& name, GuardClass guard, ToolHandler handler)
{
    ToolDef d;
    d.name    = name;
    d.guard   = guard;
    d.handler = std::move(handler);
    return d;
}

} // namespace

TEST_CASE("a UI call that is never picked up is abandoned and never runs", "[Mcp]")
{
    ManualDispatcher ui;
    UiInvoker        invoker(ui, [] { return GuardState{}; });
    bool             ran = false;
    std::string      error;
    try {
        invoker.run(GuardClass::Mutate, "t", [&] { ran = true; return json(); }, 100ms);
    } catch (const ToolFailure& f) {
        error = f.what();
    }
    CHECK(error.find("safe to retry") != std::string::npos);
    ui.drain();
    CHECK_FALSE(ran);
}

TEST_CASE("a UI call that times out behind a native dialog names the dialog", "[Mcp]")
{
    ManualDispatcher ui;
    UiInvoker        invoker(ui, [] { return GuardState{}; }, {}, [] { return kNativePicker; });
    bool             ran = false;
    std::string      error;
    json             details;
    try {
        invoker.run(GuardClass::Inspect, "get_state", [&] { ran = true; return json(); }, 100ms);
    } catch (const ToolFailure& f) {
        error   = f.what();
        details = f.details;
    }
    CHECK(error.find("native dialog") != std::string::npos);
    CHECK(details.value("dialogs", json()) == kNativePicker);
    ui.drain();
    CHECK_FALSE(ran);
}

TEST_CASE("a UI call that times out behind a wx dialog keeps the busy message", "[Mcp]")
{
    ManualDispatcher ui;
    // The topmost dialog has no "native" field, as Orca's own dialogs in these tests don't.
    const json dialogs = json::array({kNativePicker[0], {{"title", "Preferences"}}});
    UiInvoker  invoker(ui, [] { return GuardState{}; }, {}, [&] { return dialogs; });
    std::string error;
    try {
        invoker.run(GuardClass::Inspect, "get_state", [] { return json(); }, 100ms);
    } catch (const ToolFailure& f) {
        error = f.what();
    }
    CHECK(error.find("safe to retry") != std::string::npos);
    ui.drain();
}

TEST_CASE("a UI call returns the closure's value", "[Mcp]")
{
    ManualDispatcher ui;
    UiInvoker        invoker(ui, [] { return GuardState{}; });
    auto fut = std::async(std::launch::async, [&] { return invoker.run(GuardClass::Inspect, "t", [] { return json(42); }); });
    ui.pump_until([&] { return fut.wait_for(0ms) == std::future_status::ready; });
    CHECK(fut.get() == json(42));
}

TEST_CASE("shutdown wakes waiting callers and later closures do nothing", "[Mcp]")
{
    ManualDispatcher ui;
    UiInvoker        invoker(ui, [] { return GuardState{}; });
    std::atomic<bool> ran{false};
    auto fut = std::async(std::launch::async, [&]() -> std::string {
        try {
            invoker.run(GuardClass::Mutate, "t", [&] { ran = true; return json(); });
        } catch (const ToolFailure& f) {
            return f.what();
        }
        return "returned";
    });
    std::this_thread::sleep_for(50ms);
    invoker.shutdown();
    CHECK(fut.get().find("shutting down") != std::string::npos);
    ui.drain();
    CHECK_FALSE(ran);
}

TEST_CASE("guards refuse mutate calls behind a modal, while closing and while suspended", "[Mcp]")
{
    ManualDispatcher ui;
    GuardState       guards;
    UiInvoker        invoker(ui, [&] { return guards; });
    auto attempt = [&](GuardClass g) -> std::string {
        auto fut = std::async(std::launch::async, [&]() -> std::string {
            try {
                invoker.run(g, "t", [] { return json(); });
                return "ok";
            } catch (const ToolFailure& f) {
                return f.what();
            }
        });
        ui.pump_until([&] { return fut.wait_for(0ms) == std::future_status::ready; });
        return fut.get();
    };
    guards.modal = true;
    CHECK(attempt(GuardClass::Mutate).find("modal dialog") != std::string::npos);
    CHECK(attempt(GuardClass::Inspect) == "ok");
    CHECK(attempt(GuardClass::Recover) == "ok");
    guards       = {};
    guards.suspended = true;
    CHECK(attempt(GuardClass::Inspect).find("recreated") != std::string::npos);
    guards           = {};
    guards.closing   = true;
    CHECK(attempt(GuardClass::Inspect).find("shutting down") != std::string::npos);
}

TEST_CASE("a mutate call is refused inside another call's nested loop but inspect runs", "[Mcp]")
{
    ManualDispatcher ui;
    UiInvoker        invoker(ui, [] { return GuardState{}; });
    std::string      nested_mutate, nested_inspect;
    std::atomic<bool> release{false};

    auto outer = std::async(std::launch::async, [&] {
        return invoker.run(GuardClass::Mutate, "outer", [&] {
            // A nested event loop: other closures run while this one is still on the stack.
            auto m = std::async(std::launch::async, [&]() -> std::string {
                try {
                    invoker.run(GuardClass::Mutate, "inner", [] { return json(); });
                    return "ok";
                } catch (const ToolFailure& f) {
                    return f.what();
                }
            });
            auto i = std::async(std::launch::async, [&] { return invoker.run(GuardClass::Inspect, "look", [] { return json("seen"); }); });
            ui.pump_until([&] { return m.wait_for(0ms) == std::future_status::ready && i.wait_for(0ms) == std::future_status::ready; });
            nested_mutate  = m.get();
            nested_inspect = i.get().get<std::string>();
            return json();
        });
    });
    ui.pump_until([&] { return outer.wait_for(0ms) == std::future_status::ready; });
    (void) outer.get();
    CHECK(nested_mutate.find("Busy: outer") != std::string::npos);
    CHECK(nested_inspect == "seen");
}

TEST_CASE("a recover call that targets outside the dialog applies the mutate guard itself", "[Mcp]")
{
    ManualDispatcher ui;
    GuardState       guards;
    UiInvoker        invoker(ui, [&] { return guards; });
    auto attempt = [&]() -> std::string {
        auto fut = std::async(std::launch::async, [&]() -> std::string {
            try {
                invoker.run(GuardClass::Recover, "ui_click", [&] {
                    invoker.require_mutate();
                    return json();
                });
                return "ok";
            } catch (const ToolFailure& f) {
                return f.what();
            }
        });
        ui.pump_until([&] { return fut.wait_for(0ms) == std::future_status::ready; });
        return fut.get();
    };
    CHECK(attempt() == "ok");
    guards.modal = true;
    CHECK(attempt().find("modal dialog") != std::string::npos);
    guards = {};

    // Inside another call's nested loop it is busy, as a mutate call would be.
    std::string nested;
    auto outer = std::async(std::launch::async, [&] {
        return invoker.run(GuardClass::Mutate, "outer", [&] {
            auto m = std::async(std::launch::async, [&]() -> std::string {
                try {
                    invoker.run(GuardClass::Recover, "ui_click", [&] {
                        invoker.require_mutate();
                        return json();
                    });
                    return "ok";
                } catch (const ToolFailure& f) {
                    return f.what();
                }
            });
            ui.pump_until([&] { return m.wait_for(0ms) == std::future_status::ready; });
            nested = m.get();
            return json();
        });
    });
    ui.pump_until([&] { return outer.wait_for(0ms) == std::future_status::ready; });
    (void) outer.get();
    CHECK(nested.find("Busy: outer") != std::string::npos);
}

TEST_CASE("a job outlives its reply and wait() returns its result", "[Mcp]")
{
    Harness h;
    ToolDef slow = tool("slow", GuardClass::Mutate, [](const json&, ToolContext&) {
        std::this_thread::sleep_for(600ms);
        return ToolResult::ok({{"done", true}});
    });
    slow.budget_ms = 200;
    h.registry.add(slow);
    h.start();

    const ToolResult envelope = h.call("slow");
    REQUIRE_FALSE(envelope.is_error);
    CHECK(envelope.data["status"] == "running");
    const std::string job = envelope.data["job"];
    auto result = h.jobs.wait(job, 3000ms);
    REQUIRE(result.has_value());
    CHECK(result->data["done"] == true);
}

TEST_CASE("blocked_by_dialog is reported after the soft budget when the probe sees a modal", "[Mcp]")
{
    Harness           h;
    std::atomic<bool> release{false};
    h.registry.add(tool("open_modal", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        ctx.run_ui([&] {
            h.modal_loop(release, json::array({{{"title", "Unsaved changes"}}}));
            return json();
        });
        return ToolResult::ok({{"closed", true}});
    }));
    h.start();

    const ToolResult envelope = h.call("open_modal");
    CHECK(envelope.data["status"] == "blocked_by_dialog");
    CHECK(envelope.data["dialogs"][0]["title"] == "Unsaved changes");
    release = true;
    auto result = h.jobs.wait(envelope.data["job"], 3000ms);
    REQUIRE(result.has_value());
    CHECK(result->data["closed"] == true);
}

TEST_CASE("blocked_by_dialog is reported from the snapshot when a native dialog holds the UI thread", "[Mcp]")
{
    std::atomic<bool> release{false};
    Harness           h;
    SetOnExit         releaser{release};
    h.registry.add(tool("pick_file", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        ctx.run_ui([&] {
            h.hold_ui(release, kNativePicker);
            return json();
        });
        return ToolResult::ok({{"closed", true}});
    }));
    h.start();

    const ToolResult envelope = h.call("pick_file");
    release                   = true;
    REQUIRE(envelope.data["status"] == "blocked_by_dialog");
    CHECK(envelope.data["dialogs"] == kNativePicker);
    const std::string note = envelope.data.value("note", "");
    CHECK(note.find("only a person") != std::string::npos);
    CHECK(note.find("tools that need the UI thread fail") != std::string::npos);
    CHECK(note.find("Answer the dialog") == std::string::npos);
    auto result = h.jobs.wait(envelope.data["job"], 3000ms);
    REQUIRE(result.has_value());
    CHECK(result->data["closed"] == true);
}

TEST_CASE("a held UI thread under a wx dialog is not reported as blocked_by_dialog", "[Mcp]")
{
    std::atomic<bool> release{false};
    Harness           h;
    SetOnExit         releaser{release};
    ToolDef           hold = tool("hold", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        ctx.run_ui([&] {
            h.hold_ui(release, json::array({kNativePicker[0], {{"title", "Preferences"}}}));
            return json();
        });
        return ToolResult::ok();
    });
    hold.budget_ms = 1000;
    h.registry.add(hold);
    h.start();

    const ToolResult envelope = h.call("hold");
    release                   = true;
    CHECK(envelope.data["status"] == "running");
    CHECK(h.jobs.wait(envelope.data["job"], 3000ms).has_value());
}

TEST_CASE("shutdown during an unanswered probe replies shutting down", "[Mcp]")
{
    std::atomic<bool> release{false};
    Harness           h;
    SetOnExit         releaser{release};
    h.registry.add(tool("pick_file", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        ctx.run_ui([&] {
            h.hold_ui(release, kNativePicker);
            return json();
        });
        return ToolResult::ok();
    }));
    h.start();

    // 300 ms lands inside the probe window: past the 100 ms soft budget, before the 500 ms probe
    // gives up. A stop that lands earlier gets the same reply from the check before the probe.
    auto reply = std::async(std::launch::async, [&] { return h.call("pick_file"); });
    std::this_thread::sleep_for(300ms);
    h.invoker.shutdown();
    const ToolResult r = reply.get();
    release            = true;
    CHECK(r.is_error);
    CHECK(r.message.find("shutting down") != std::string::npos);
}

TEST_CASE("a native dialog that the probe sees gets the native note", "[Mcp]")
{
    std::atomic<bool> release{false};
    Harness           h;
    SetOnExit         releaser{release};
    h.registry.add(tool("pick_file", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        ctx.run_ui([&] {
            h.modal_loop(release, kNativePicker);
            return json();
        });
        return ToolResult::ok();
    }));
    h.start();

    const ToolResult envelope = h.call("pick_file");
    release                   = true;
    REQUIRE(envelope.data["status"] == "blocked_by_dialog");
    const std::string note = envelope.data.value("note", "");
    CHECK(note.find("only a person") != std::string::npos);
    CHECK(note.find("tools that need the UI thread fail") == std::string::npos);
    CHECK(h.jobs.wait(envelope.data["job"], 3000ms).has_value());
}

TEST_CASE("inspect tools run inline and recover tools run as jobs while another job is blocked", "[Mcp]")
{
    Harness           h;
    std::atomic<bool> release{false};
    h.registry.add(tool("open_modal", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        ctx.run_ui([&] {
            h.modal_loop(release);
            return json();
        });
        return ToolResult::ok();
    }));
    ToolDef look = tool("look", GuardClass::Inspect, [](const json&, ToolContext& ctx) {
        return ToolResult::ok({{"job", ctx.job_id()}, {"ui", ctx.run_ui([] { return json("ok"); })}});
    });
    look.inline_call = true;
    h.registry.add(look);
    h.registry.add(tool("answer", GuardClass::Recover, [&](const json&, ToolContext& ctx) {
        ctx.run_ui([&] { release = true; return json(); });
        return ToolResult::ok({{"job", ctx.job_id()}});
    }));
    h.start();

    const ToolResult blocked = h.call("open_modal");
    REQUIRE(blocked.data["status"] == "blocked_by_dialog");
    const ToolResult seen = h.call("look");
    CHECK(seen.data["job"] == "");
    CHECK(seen.data["ui"] == "ok");
    const ToolResult answered = h.call("answer");
    CHECK_FALSE(answered.is_error);
    CHECK(answered.data["job"].get<std::string>().rfind("j", 0) == 0);
    CHECK(h.jobs.wait(blocked.data["job"], 3000ms).has_value());
}

TEST_CASE("a recover call that opens another modal returns blocked_by_dialog", "[Mcp]")
{
    Harness           h;
    std::atomic<bool> release{false};
    h.registry.add(tool("click_edit", GuardClass::Recover, [&](const json&, ToolContext& ctx) {
        ctx.run_ui([&] {
            h.modal_loop(release);
            return json();
        });
        return ToolResult::ok();
    }));
    h.start();
    const ToolResult r = h.call("click_edit");
    CHECK(r.data["status"] == "blocked_by_dialog");
    release = true;
    CHECK(h.jobs.wait(r.data["job"], 3000ms).has_value());
}

TEST_CASE("every mutate call is refused while a slice job runs, but cancel and recover tools run", "[Mcp]")
{
    Harness           h;
    std::atomic<bool> release{false};
    ToolDef slice = tool("slice", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        ctx.begin_operation();
        while (!release)
            std::this_thread::sleep_for(10ms);
        return ToolResult::ok();
    });
    slice.budget_ms = 100;
    h.registry.add(slice);
    h.registry.add(tool("new_project", GuardClass::Mutate, [](const json&, ToolContext&) { return ToolResult::ok(); }));
    ToolDef cancel = tool("cancel", GuardClass::Mutate, [&](const json&, ToolContext&) {
        release = true;
        return ToolResult::ok();
    });
    cancel.allowed_during_operation = true;
    h.registry.add(cancel);
    h.registry.add(tool("answer", GuardClass::Recover, [](const json&, ToolContext&) { return ToolResult::ok(); }));
    h.start();

    const ToolResult running = h.call("slice");
    REQUIRE(running.data["status"] == "running");
    const ToolResult refused = h.call("new_project");
    CHECK(refused.is_error);
    CHECK(refused.message.find("wait for it or cancel it") != std::string::npos);
    CHECK_FALSE(h.call("answer").is_error);
    CHECK_FALSE(h.call("cancel").is_error);
    CHECK(h.jobs.wait(running.data["job"], 3000ms).has_value());
    CHECK_FALSE(h.call("new_project").is_error);
}

TEST_CASE("without a blocking slot a job call returns its envelope at once and an inline call returns pending", "[Mcp]")
{
    Harness h;
    h.timing.blocking_slots = 1;
    std::atomic<bool> release{false};
    ToolDef hold = tool("hold", GuardClass::Inspect, [&](const json&, ToolContext&) {
        while (!release)
            std::this_thread::sleep_for(10ms);
        return ToolResult::ok();
    });
    hold.inline_call = true;
    h.registry.add(hold);
    h.registry.add(tool("work", GuardClass::Mutate, [](const json&, ToolContext&) {
        std::this_thread::sleep_for(50ms);
        return ToolResult::ok();
    }));
    h.start();

    auto holder = std::async(std::launch::async, [&] { return h.core->call(*h.registry.find("hold"), json::object()); });
    std::this_thread::sleep_for(100ms);
    const auto start = std::chrono::steady_clock::now();
    const ToolResult job = h.core->call(*h.registry.find("work"), json::object());
    CHECK(job.data["status"] == "running");
    const ToolResult pending = h.core->call(*h.registry.find("hold"), json::object());
    CHECK(pending.data["status"] == "pending");
    CHECK(std::chrono::steady_clock::now() - start < 500ms);
    release = true;
    (void) holder.get();
    CHECK(h.jobs.wait(job.data["job"], 3000ms).has_value());
}

TEST_CASE("idle ignores the caller and its ancestors", "[Mcp]")
{
    JobRegistry jobs;
    const std::string script = jobs.create("run_script", "");
    const std::string step   = jobs.create("slice", script);
    const std::string other  = jobs.create("open_files", "");
    CHECK(jobs.lineage(step) == std::set<std::string>{step, script});
    CHECK(jobs.any_running_except(jobs.lineage(step)));
    jobs.finish(other, ToolResult::ok());
    CHECK_FALSE(jobs.any_running_except(jobs.lineage(step)));
    jobs.begin_operation(step);
    CHECK(jobs.running_operations() == std::vector<std::string>{step});
}

TEST_CASE("the job registry caps running jobs and forgets old finished ones", "[Mcp]")
{
    JobRegistry jobs;
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < JobRegistry::kMaxRunning; ++i)
        ids.push_back(jobs.create("t", ""));
    CHECK_THROWS_AS(jobs.create("t", ""), ToolFailure);
    for (const std::string& id : ids)
        jobs.finish(id, ToolResult::ok());
    for (std::size_t i = 0; i < JobRegistry::kKeepFinished; ++i)
        jobs.finish(jobs.create("t", ""), ToolResult::ok());
    CHECK_THROWS_AS(jobs.wait(ids.front(), 0ms), ToolFailure);
}

TEST_CASE("a caller waits for its running UI closure even through shutdown", "[Mcp]")
{
    ManualDispatcher  ui;
    UiInvoker         invoker(ui, [] { return GuardState{}; });
    std::atomic<bool> inside{false}, release{false}, finished{false};
    auto caller = std::async(std::launch::async, [&]() -> std::string {
        try {
            invoker.run(GuardClass::Inspect, "look", [&] {
                inside = true;
                ui.pump_until([&] { return release.load(); }); // a nested event loop
                finished = true;
                return json();
            });
            return "ok";
        } catch (const ToolFailure& f) {
            return f.what();
        }
    });
    std::thread ui_thread([&] { ui.pump_until([&] { return finished.load(); }, 5000ms); });
    while (!inside)
        std::this_thread::sleep_for(5ms);
    invoker.shutdown();
    // The closure still uses the caller's stack, so the caller must not have returned.
    CHECK(caller.wait_for(200ms) == std::future_status::timeout);
    release = true;
    ui_thread.join();
    CHECK(caller.get() == "ok");
}

TEST_CASE("a closure queued before a UI rebuild does not run after it", "[Mcp]")
{
    ManualDispatcher      ui;
    std::atomic<uint64_t> generation{1};
    UiInvoker             invoker(ui, [] { return GuardState{}; }, [&] { return generation.load(); });
    std::atomic<bool>     ran{false};
    auto caller = std::async(std::launch::async, [&]() -> std::string {
        try {
            invoker.run(GuardClass::Mutate, "click", [&] {
                ran = true;
                return json();
            });
            return "ok";
        } catch (const ToolFailure& f) {
            return f.what();
        }
    });
    std::this_thread::sleep_for(50ms);
    generation = 2; // a language change rebuilt the UI while the closure waited
    ui.pump_until([&] { return caller.wait_for(0ms) == std::future_status::ready; });
    CHECK(caller.get().find("recreated") != std::string::npos);
    CHECK_FALSE(ran);
}

TEST_CASE("operation admission is checked again when a mutate call reaches the UI thread", "[Mcp]")
{
    Harness h;
    std::atomic<bool> release{false};
    h.registry.add(tool("edit", GuardClass::Mutate, [](const json&, ToolContext& ctx) {
        return ToolResult::ok(ctx.run_ui([] { return json("edited"); }));
    }));
    ToolDef slice = tool("slice", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        ctx.run_ui([&] {
            ctx.begin_operation();
            return json();
        });
        while (!release)
            std::this_thread::sleep_for(10ms);
        return ToolResult::ok();
    });
    h.registry.add(slice);
    h.start();
    // The UI thread is held, so both calls are admitted before either runs; the slice runs first.
    std::atomic<bool> hold{true};
    h.ui.post([&] {
        while (hold)
            std::this_thread::sleep_for(5ms);
    });
    auto s = std::async(std::launch::async, [&] { return h.call("slice"); });
    std::this_thread::sleep_for(100ms);
    auto e = std::async(std::launch::async, [&] { return h.call("edit"); });
    std::this_thread::sleep_for(100ms);
    hold = false;
    const ToolResult edited = e.get();
    CHECK(edited.is_error);
    CHECK(edited.message.find("Busy: job") != std::string::npos);
    release = true;
    (void) s.get();
}

TEST_CASE("only one operation runs at a time", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string a = jobs.create("slice", "");
    const std::string b = jobs.create("export", "");
    CHECK(jobs.begin_operation(a).empty());
    CHECK(jobs.begin_operation(b) == a);
    jobs.finish(a, ToolResult::ok());
    CHECK(jobs.begin_operation(b).empty());
}

TEST_CASE("ancestry lists the job chain nearest first", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string outer = jobs.create("run_script", "");
    const std::string inner = jobs.create("run_script", outer);
    const std::string step  = jobs.create("dialog_auto_answer", inner);
    CHECK(jobs.ancestry(step) == std::vector<std::string>{step, inner, outer});
}

TEST_CASE("a finished job stays while its parent script runs", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string script = jobs.create("run_script", "");
    const std::string saved  = jobs.create("run_action", script);
    jobs.finish(saved, ToolResult::ok({{"x", 1}}));
    for (std::size_t i = 0; i < JobRegistry::kKeepFinished + 5; ++i)
        jobs.finish(jobs.create("t", ""), ToolResult::ok());
    CHECK(jobs.wait(saved, 0ms).has_value());
    jobs.finish(script, ToolResult::ok());
    for (std::size_t i = 0; i < JobRegistry::kKeepFinished + 5; ++i)
        jobs.finish(jobs.create("t", ""), ToolResult::ok());
    CHECK_THROWS_AS(jobs.wait(saved, 0ms), ToolFailure);
}

TEST_CASE("finished jobs stay bounded while their parent script runs", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string script = jobs.create("run_script", "");
    const std::string first  = jobs.create("t", script);
    jobs.finish(first, ToolResult::ok());
    for (std::size_t i = 0; i < JobRegistry::kKeepFinishedMax; ++i)
        jobs.finish(jobs.create("t", script), ToolResult::ok());
    CHECK_THROWS_AS(jobs.wait(first, 0ms), ToolFailure);
    const std::string recent = jobs.create("t", script);
    jobs.finish(recent, ToolResult::ok());
    CHECK(jobs.wait(recent, 0ms).has_value());
}

TEST_CASE("the journal keeps a call waiting for its reply when it drops old calls", "[Mcp]")
{
    Journal        journal;
    const uint64_t waiting = journal.begin("slice", json::object());
    for (int i = 0; i < 6000; ++i)
        journal.end(journal.begin("set_settings", {{"values", {{"wall_loops", i}}}}), ToolResult::ok());
    journal.end(waiting, ToolResult::ok());
    const json steps = journal.to_script("s")["steps"];
    REQUIRE_FALSE(steps.empty());
    CHECK(steps[0]["tool"] == "slice");
    CHECK(steps.size() == 5000);
}

TEST_CASE("drain reports a job thread that has not returned", "[Mcp]")
{
    Harness           h;
    std::atomic<bool> release{false};
    h.registry.add(tool("stuck", GuardClass::Mutate, [&](const json&, ToolContext&) {
        while (!release)
            std::this_thread::sleep_for(10ms);
        return ToolResult::ok();
    }));
    h.timing.soft_budget = 50ms;
    h.start();
    auto call = std::async(std::launch::async, [&] { return h.call("stuck"); });
    std::this_thread::sleep_for(100ms);
    h.core->shutdown();
    CHECK_FALSE(h.core->drain(200ms));
    release = true;
    (void) call.get();
    CHECK(h.core->drain(2000ms));
}

TEST_CASE("destroying a core waits for a job thread still inside it", "[Mcp]")
{
    Harness           h;
    std::atomic<bool> release{false};
    std::atomic<bool> returned{false};
    h.registry.add(tool("stuck", GuardClass::Mutate, [&](const json&, ToolContext&) {
        while (!release)
            std::this_thread::sleep_for(10ms);
        std::this_thread::sleep_for(50ms);
        returned = true;
        return ToolResult::ok();
    }));
    h.timing.soft_budget = 50ms;
    h.start();
    auto call = std::async(std::launch::async, [&] { return h.call("stuck"); });
    std::this_thread::sleep_for(100ms);
    h.core->shutdown();
    auto destroy = std::async(std::launch::async, [&] { h.core.reset(); });
    CHECK(destroy.wait_for(300ms) == std::future_status::timeout);
    release = true;
    destroy.get();
    CHECK(returned);
    (void) call.get();
}

TEST_CASE("the journal records client calls in order and replays jobs with await false", "[Mcp]")
{
    Journal journal;
    const uint64_t a = journal.begin("run_action", {{"id", "orca_command:open_preferences:orca"}});
    journal.job_started(a, "j7");
    journal.end(a, ToolResult::ok({{"status", "blocked_by_dialog"}, {"job", "j7"}}));
    const uint64_t b = journal.begin("dialog_respond", {{"button", "close"}});
    journal.end(b, ToolResult::ok());
    const uint64_t c = journal.begin("wait", {{"for", "job"}, {"job", "j7"}});
    journal.end(c, ToolResult::ok());
    const uint64_t d = journal.begin("screenshot", {{"target", "canvas"}, {"save_path", "/tmp/x/after.png"}});
    journal.end(d, ToolResult::ok());
    const uint64_t e = journal.begin("set_settings", {{"values", {{"wall_loops", 3}}}});
    journal.end(e, ToolResult::error("no"));

    const json script = journal.to_script("repro");
    REQUIRE(script["steps"].size() == 4);
    CHECK(script["steps"][0]["await"] == false);
    CHECK(script["steps"][0]["save_as"] == "job1");
    CHECK(script["steps"][1]["tool"] == "dialog_respond");
    CHECK(script["steps"][2]["args"]["job"] == "${jobs.job1}");
    CHECK(script["steps"][3]["args"]["save_path"] == "${out_dir}/after.png");
}

TEST_CASE("only client calls of journaled tools reach the journal", "[Mcp]")
{
    Harness h;
    ToolDef look = tool("get_state", GuardClass::Inspect, [](const json&, ToolContext&) { return ToolResult::ok(); });
    look.inline_call = true;
    look.journal     = false;
    h.registry.add(look);
    ToolDef wait = tool("wait", GuardClass::Inspect, [](const json&, ToolContext&) { return ToolResult::ok(); });
    wait.inline_call = true;
    h.registry.add(wait);
    h.registry.add(tool("set_settings", GuardClass::Mutate, [](const json&, ToolContext& ctx) {
        ctx.set_journal_args({{"values", {{"wall_loops", 3}}}, {"scope", "print"}});
        return ToolResult::ok();
    }));
    h.start();
    h.call("get_state");
    h.call("wait", {{"for", "idle"}});
    h.call("set_settings", {{"values", {{"wall_loops", 3}}}});
    const json steps = h.journal.to_script("x")["steps"];
    REQUIRE(steps.size() == 2);
    CHECK(steps[0]["tool"] == "wait");
    CHECK(steps[1]["args"]["scope"] == "print");
}

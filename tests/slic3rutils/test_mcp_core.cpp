#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/Mcp/JobRegistry.hpp"
#include "slic3r/GUI/Mcp/McpCore.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/McpProtocol.hpp"
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
#include <new>
#include <set>
#include <stdexcept>
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

// Arguments whose serialized size is just over `mib` MiB.
json args_of_mib(std::size_t mib) { return {{"s", std::string(mib << 20, 'x')}}; }

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
        invoker.run(GuardClass::Inspect, "get_app_state", [&] { ran = true; return json(); }, 100ms);
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
        invoker.run(GuardClass::Inspect, "get_app_state", [] { return json(); }, 100ms);
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
                invoker.run(GuardClass::Recover, "click", [&] {
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
                    invoker.run(GuardClass::Recover, "click", [&] {
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

TEST_CASE("a call working in the topmost dialog is blocked only by another dialog on top", "[Mcp]")
{
    Harness h;
    h.registry.add(tool("page_call", GuardClass::Recover, [&](const json&, ToolContext& ctx) {
        ctx.works_in_dialog(7);
        std::this_thread::sleep_for(400ms);
        return ToolResult::ok({{"done", true}});
    }));
    h.start();
    auto top = [&](std::uint64_t serial, json dialogs) {
        std::lock_guard<std::mutex> lock(h.guards_mutex);
        h.guards.modal      = true;
        h.guards.dialogs    = std::move(dialogs);
        h.guards.top_dialog = serial;
    };

    // Its own dialog on top: past the soft budget it is still running, and its result comes back.
    top(7, json::array({{{"title", "Web dialog"}}}));
    CHECK(h.call("page_call").data["done"] == true);

    // Another dialog on top, at any depth (here the same one: its own closed and another opened).
    top(8, json::array({{{"title", "Another"}}}));
    const ToolResult blocked = h.call("page_call");
    CHECK(blocked.data["status"] == "blocked_by_dialog");
    REQUIRE(h.jobs.wait(blocked.data["job"], 3000ms).has_value());
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

TEST_CASE("a native message box with buttons gets the answer_dialog note", "[Mcp]")
{
    // As ModalTracker describes a message box whose buttons can be pressed (Windows, Linux).
    const json box = json::array({{{"title", "Plugin permission request"},
                                   {"message", "Plugin \"Fixture\" is requesting permission to create a thread."},
                                   {"native", true},
                                   {"buttons", json::array({{{"id", 5103}, {"label", "Yes"}}, {{"id", 5104}, {"label", "No"}}})}}});
    std::atomic<bool> release{false};
    Harness           h;
    SetOnExit         releaser{release};
    h.registry.add(tool("ask", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        ctx.run_ui([&] {
            h.modal_loop(release, box);
            return json();
        });
        return ToolResult::ok();
    }));
    h.start();

    const ToolResult envelope = h.call("ask");
    release                   = true;
    REQUIRE(envelope.data["status"] == "blocked_by_dialog");
    CHECK(envelope.data["dialogs"] == box);
    const std::string note = envelope.data.value("note", "");
    CHECK(note.find("answer_dialog") != std::string::npos);
    CHECK(note.find("only a person") == std::string::npos);
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

TEST_CASE("every mutate call is refused while a slice job runs, but cancel_job and recover tools run", "[Mcp]")
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
    ToolDef cancel = tool("cancel_job", GuardClass::Mutate, [&](const json&, ToolContext&) {
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
    CHECK(refused.message.find("wait for it or stop it with cancel_job") != std::string::npos);
    CHECK_FALSE(h.call("answer").is_error);
    CHECK_FALSE(h.call("cancel_job").is_error);
    CHECK(h.jobs.wait(running.data["job"], 3000ms).has_value());
    CHECK_FALSE(h.call("new_project").is_error);
}

// What a recording's CallObserver hears, in order.
struct HeardCalls final : CallObserver
{
    std::mutex               mutex;
    std::vector<std::string> events;
    void call_started(uint64_t, const ToolDef& def, const json&, const std::string& parent) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back("start " + def.name + (parent.empty() ? "" : " in a job"));
    }
    void call_args(uint64_t, const json& args) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back("args " + args.dump());
    }
    void call_ended(uint64_t, const ToolResult& result) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(std::string("end ") + (result.is_error ? "error" : "ok"));
    }
    std::vector<std::string> take()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(events, {});
    }
};

TEST_CASE("the call observer hears inline, job and child calls, and not calls refused before they run", "[Mcp]")
{
    Harness    h;
    HeardCalls heard;
    ToolDef    look = tool("look", GuardClass::Inspect, [](const json&, ToolContext&) { return ToolResult::ok(); });
    look.inline_call = true;
    h.registry.add(look);
    h.registry.add(tool("click", GuardClass::Recover, [](const json&, ToolContext& ctx) {
        // A ref resolved to its locator, as the GUI tools record it.
        ctx.set_journal_args({{"target", {{"label", "Slice"}}}});
        return ToolResult::error("gone");
    }));
    h.registry.add(tool("replay", GuardClass::Mutate, [](const json&, ToolContext& ctx) { return ctx.call_child("click", json::object()); }));
    std::atomic<bool> release{false};
    ToolDef           slice = tool("slice", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        ctx.begin_operation();
        while (!release)
            std::this_thread::sleep_for(10ms);
        return ToolResult::ok();
    });
    slice.budget_ms = 100;
    h.registry.add(slice);
    h.core = std::make_unique<McpCore>(h.registry, h.invoker, h.jobs, &h.journal, h.timing, WorkerHooks{}, &heard);

    CHECK_FALSE(h.call("look").is_error);
    CHECK(heard.take() == std::vector<std::string>{"start look", "end ok"});
    CHECK(h.call("click").is_error);
    CHECK(heard.take() == std::vector<std::string>{"start click", R"(args {"target":{"label":"Slice"}})", "end error"});
    h.call("replay");
    CHECK(heard.take() ==
          std::vector<std::string>{"start replay", "start click in a job", R"(args {"target":{"label":"Slice"}})", "end error", "end error"});

    const ToolResult running = h.call("slice");
    REQUIRE(running.data["status"] == "running");
    CHECK(heard.take() == std::vector<std::string>{"start slice"});
    CHECK(h.call("replay").is_error); // refused by admission: it never ran
    CHECK(heard.take().empty());
    release = true;
    REQUIRE(h.jobs.wait(running.data["job"], 3000ms).has_value());
    CHECK(heard.take() == std::vector<std::string>{"end ok"});
}

TEST_CASE("an observer that throws fails no call", "[Mcp]")
{
    struct Throwing final : CallObserver
    {
        void call_started(uint64_t, const ToolDef&, const json&, const std::string&) override { throw std::runtime_error("started"); }
        void call_args(uint64_t, const json&) override { throw std::bad_alloc(); }
        void call_ended(uint64_t, const ToolResult&) override { throw std::runtime_error("ended"); }
    } throwing;
    Harness h;
    ToolDef look = tool("look", GuardClass::Inspect, [](const json&, ToolContext&) { return ToolResult::ok({{"seen", true}}); });
    look.inline_call = true;
    h.registry.add(look);
    h.registry.add(tool("act", GuardClass::Mutate, [](const json&, ToolContext& ctx) {
        ctx.set_journal_args({{"x", 1}});
        return ToolResult::ok({{"done", true}});
    }));
    h.core = std::make_unique<McpCore>(h.registry, h.invoker, h.jobs, &h.journal, h.timing, WorkerHooks{}, &throwing);
    const ToolResult looked = h.call("look");
    CHECK_FALSE(looked.is_error);
    CHECK(looked.data["seen"] == true);
    const ToolResult acted = h.call("act");
    CHECK_FALSE(acted.is_error);
    CHECK(acted.data["done"] == true);
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
    const std::string replay_job = jobs.create("run_replay", "");
    const std::string step   = jobs.create("slice", replay_job);
    const std::string other  = jobs.create("open_files", "");
    CHECK(jobs.lineage(step) == std::set<std::string>{step, replay_job});
    CHECK(jobs.any_running_except(jobs.lineage(step)));
    jobs.finish(other, ToolResult::ok());
    CHECK_FALSE(jobs.any_running_except(jobs.lineage(step)));
    jobs.begin_operation(step);
    CHECK(jobs.running_operations() == std::vector<std::string>{step});
}

TEST_CASE("idle skips a job set idle, but not the calls it makes", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string run = jobs.create("run_script", "");
    jobs.set_idle(run);
    const std::string child = jobs.create("slice", run);
    CHECK(jobs.any_running_except({}));
    bool listed_idle = false;
    for (const nlohmann::json& job : jobs.list())
        if (job["id"] == run)
            listed_idle = job.value("idle", false);
    CHECK(listed_idle);
    jobs.finish(child, ToolResult::ok());
    CHECK_FALSE(jobs.any_running_except({}));
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
    const std::string outer = jobs.create("run_replay", "");
    const std::string inner = jobs.create("run_replay", outer);
    const std::string step  = jobs.create("auto_answer_dialogs", inner);
    CHECK(jobs.ancestry(step) == std::vector<std::string>{step, inner, outer});
}

TEST_CASE("a stop asked for a job reaches its descendants and no other job", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string replay_job = jobs.create("run_replay", "");
    const std::string step   = jobs.create("slice", replay_job);
    const std::string other  = jobs.create("open_files", "");
    CHECK_FALSE(jobs.stop_requested(step));
    jobs.request_stop(replay_job);
    CHECK(jobs.stop_requested(replay_job));
    CHECK(jobs.stop_requested(step));
    CHECK_FALSE(jobs.stop_requested(other));
    CHECK_FALSE(jobs.stop_requested(""));
}

TEST_CASE("a job wait with a stop check looks once at zero timeout and ends when stopped", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string done = jobs.create("slice", "");
    jobs.finish(done, ToolResult::ok({{"done", true}}));
    const std::string running = jobs.create("slice", "");
    const auto        never   = [] { return false; };

    const auto finished = jobs.wait(done, 0ms, never);
    REQUIRE(finished.has_value());
    CHECK(finished->data["done"] == true);
    CHECK_FALSE(jobs.wait(running, 0ms, never).has_value());
    CHECK_THROWS_AS(jobs.wait("j999", 0ms, never), ToolFailure);

    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(jobs.wait(running, 5000ms, [] { return true; }).has_value());
    CHECK(std::chrono::steady_clock::now() - start < 2s);
}

TEST_CASE("a call made by a stopped job sees itself cancelled", "[Mcp]")
{
    Harness h;
    // An inline call that polls for cancellation, as wait does.
    ToolDef spin = tool("spin", GuardClass::Inspect, [](const json&, ToolContext& ctx) {
        const auto end = std::chrono::steady_clock::now() + 5s;
        while (!ctx.cancelled() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(10ms);
        return ToolResult::ok({{"cancelled", ctx.cancelled()}});
    });
    spin.inline_call = true;
    h.registry.add(spin);
    ToolDef replay = tool("replay", GuardClass::Mutate, [](const json&, ToolContext& ctx) { return ctx.call_child("spin", json::object()); });
    replay.budget_ms = 50;
    h.registry.add(replay);
    h.start();

    const ToolResult envelope = h.call("replay");
    REQUIRE(envelope.data["status"] == "running");
    const auto asked = std::chrono::steady_clock::now();
    h.jobs.request_stop(envelope.data["job"]);
    const auto result = h.jobs.wait(envelope.data["job"], 3000ms);
    REQUIRE(result.has_value());
    CHECK(result->data["cancelled"] == true);
    CHECK(std::chrono::steady_clock::now() - asked < 2s);
}

TEST_CASE("a live descendant keeps its lineage after its ancestors are evicted", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string root  = jobs.create("run_replay", "");
    const std::string inner = jobs.create("run_replay", root);
    const std::string leaf  = jobs.create("wait", inner);
    jobs.finish(inner, ToolResult::ok());
    jobs.finish(root, ToolResult::ok());
    for (int i = 0; i < 70; ++i)
        jobs.finish(jobs.create("t", ""), ToolResult::ok());
    CHECK_THROWS_AS(jobs.wait(inner, 0ms), ToolFailure); // evicted
    const std::set<std::string> lineage = jobs.lineage(leaf);
    CHECK(lineage.count(root) == 1);
    CHECK(lineage.count(inner) == 1);
    CHECK(jobs.ancestry(leaf) == std::vector<std::string>{leaf, inner, root});
}

TEST_CASE("a finished job stays while its parent replay runs", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string replay_job = jobs.create("run_replay", "");
    const std::string saved  = jobs.create("run_action", replay_job);
    jobs.finish(saved, ToolResult::ok({{"x", 1}}));
    for (std::size_t i = 0; i < JobRegistry::kKeepFinished + 5; ++i)
        jobs.finish(jobs.create("t", ""), ToolResult::ok());
    CHECK(jobs.wait(saved, 0ms).has_value());
    jobs.finish(replay_job, ToolResult::ok());
    for (std::size_t i = 0; i < JobRegistry::kKeepFinished + 5; ++i)
        jobs.finish(jobs.create("t", ""), ToolResult::ok());
    CHECK_THROWS_AS(jobs.wait(saved, 0ms), ToolFailure);
}

TEST_CASE("finished jobs stay bounded while their parent replay runs", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string replay_job = jobs.create("run_replay", "");
    const std::string first  = jobs.create("t", replay_job);
    jobs.finish(first, ToolResult::ok());
    for (std::size_t i = 0; i < JobRegistry::kKeepFinishedMax; ++i)
        jobs.finish(jobs.create("t", replay_job), ToolResult::ok());
    CHECK_THROWS_AS(jobs.wait(first, 0ms), ToolFailure);
    const std::string recent = jobs.create("t", replay_job);
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
    const json steps = journal.to_replay("s")["steps"];
    REQUIRE_FALSE(steps.empty());
    CHECK(steps[0]["tool"] == "slice");
    CHECK(steps.size() == 5000);
}

TEST_CASE("the journal keeps at most 64 MiB of arguments", "[Mcp]")
{
    Journal    journal;
    const json big = args_of_mib(1);
    for (int i = 0; i < 100; ++i)
        journal.end(journal.begin("set_settings", big), ToolResult::ok());
    const json steps = journal.to_replay("s")["steps"];
    CHECK(steps.size() < 64);
    CHECK(steps.size() > 56);
}

TEST_CASE("calls waiting for their reply outlast the byte cap and go once they finish", "[Mcp]")
{
    Journal               journal;
    const json            big = args_of_mib(1);
    std::vector<uint64_t> waiting;
    for (int i = 0; i < 70; ++i)
        waiting.push_back(journal.begin("set_settings", big));
    json left;
    CHECK(journal.to_replay("s", &left)["steps"].empty());
    CHECK(left.size() == 70);
    // Each reply trims on its own, with no later begin or set_args.
    for (const uint64_t seq : waiting)
        journal.end(seq, ToolResult::ok());
    CHECK(journal.to_replay("s", &left)["steps"].size() < 64);
    CHECK(left.empty());
}

TEST_CASE("a job's call over the byte cap stays until its job ends", "[Mcp]")
{
    Journal    journal;
    const json huge = args_of_mib(65);
    json       left;

    // The reply comes first, as the job's envelope; the job ends later.
    const uint64_t seq = journal.begin("call_cad_tool", huge);
    journal.job_started(seq, "j");
    journal.end(seq, ToolResult::ok({{"status", "running"}, {"job", "j"}}));
    CHECK(journal.to_replay("s", &left)["steps"].empty());
    REQUIRE(left.size() == 1);
    CHECK(left[0] == json{{"tool", "call_cad_tool"}, {"job", "j"}});
    journal.job_ended(seq, ToolResult::ok());
    CHECK(journal.to_replay("s", &left)["steps"].empty());
    CHECK(left.empty());

    // The job ends before its reply.
    const uint64_t seq2 = journal.begin("call_cad_tool", huge);
    journal.job_started(seq2, "k");
    journal.job_ended(seq2, ToolResult::ok());
    CHECK(journal.to_replay("s", &left)["steps"].empty());
    CHECK(left.empty());
    journal.end(seq2, ToolResult::ok());
    CHECK(journal.to_replay("s", &left)["steps"].empty());
    CHECK(left.empty());
}

TEST_CASE("replacing a call's arguments moves the journal's byte total", "[Mcp]")
{
    Journal    journal;
    const json fifteen = args_of_mib(15);
    json       left;
    for (int i = 0; i < 4; ++i)
        journal.end(journal.begin("set_settings", fifteen), ToolResult::ok());
    // 60 MiB, then 70 MiB once the small call's arguments grow: the first 15 MiB call goes at once,
    // while the grown call still waits for its reply.
    const uint64_t b = journal.begin("set_settings", {{"values", {{"wall_loops", 3}}}});
    journal.set_args(b, args_of_mib(10));
    CHECK(journal.to_replay("s", &left)["steps"].size() == 3);
    CHECK(left.size() == 1);
    journal.end(b, ToolResult::ok());
    CHECK(journal.to_replay("s")["steps"].size() == 4);
    // Shrinking them back leaves 45 MiB, room for one more 15 MiB call.
    journal.set_args(b, {{"values", {{"wall_loops", 3}}}});
    journal.end(journal.begin("set_settings", fifteen), ToolResult::ok());
    CHECK(journal.to_replay("s")["steps"].size() == 5);
}

TEST_CASE("clearing the journal resets its byte total", "[Mcp]")
{
    Journal    journal;
    const json fifteen = args_of_mib(15);
    for (int i = 0; i < 4; ++i)
        journal.end(journal.begin("set_settings", fifteen), ToolResult::ok());
    journal.clear();
    // Were the cleared calls' 60 MiB still counted, this 15 MiB call would go once it replied.
    journal.end(journal.begin("set_settings", fifteen), ToolResult::ok());
    journal.end(journal.begin("set_settings", {{"values", {{"wall_loops", 3}}}}), ToolResult::ok());
    CHECK(journal.to_replay("s")["steps"].size() == 2);
}

TEST_CASE("one call bigger than the cap is dropped once it has replied", "[Mcp]")
{
    Journal        journal;
    json           left;
    const uint64_t seq = journal.begin("call_cad_tool", args_of_mib(65));
    CHECK(journal.to_replay("s", &left)["steps"].empty());
    REQUIRE(left.size() == 1);
    CHECK(left[0] == json{{"tool", "call_cad_tool"}});
    journal.end(seq, ToolResult::ok());
    CHECK(journal.to_replay("s", &left)["steps"].empty());
    CHECK(left.empty());
    // The total is back to what the remaining calls hold, so a small call stays.
    journal.end(journal.begin("set_settings", {{"values", {{"wall_loops", 3}}}}), ToolResult::ok());
    CHECK(journal.to_replay("s")["steps"].size() == 1);
}

TEST_CASE("a job refused at shutdown after it registered ends its journal entry", "[Mcp]")
{
    ManualDispatcher ui;
    McpCore*         core_ptr = nullptr;
    // A recover call's journal entry reads the open dialogs between the core's two shutdown checks:
    // shutting down there lets the call register its job, then refuses it.
    UiInvoker    invoker(ui, [] { return GuardState{}; }, {}, [&core_ptr] {
        core_ptr->shutdown();
        return json::array();
    });
    JobRegistry  jobs;
    Journal      journal;
    ToolRegistry registry;
    registry.add(tool("answer_dialog", GuardClass::Recover, [](const json&, ToolContext&) { return ToolResult::ok(); }));
    McpCore core(registry, invoker, jobs, &journal, CoreTiming{});
    core_ptr = &core;

    const ToolResult r = core.call(*registry.find("answer_dialog"), args_of_mib(65));
    REQUIRE(r.is_error);
    CHECK(r.message == "OrcaSlicer is shutting down");
    // An entry left running would hold its 65 MiB against the cap, so the next call, not it, would go.
    journal.end(journal.begin("set_settings", {{"values", {{"wall_loops", 3}}}}), ToolResult::ok());
    CHECK(journal.to_replay("s")["steps"].size() == 1);
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
    const uint64_t b = journal.begin("answer_dialog", {{"button", "close"}});
    journal.end(b, ToolResult::ok());
    const uint64_t c = journal.begin("wait", {{"for", "job"}, {"job", "j7"}});
    journal.end(c, ToolResult::ok());
    const uint64_t d = journal.begin("screenshot", {{"target", "canvas"}, {"save_path", "/tmp/x/after.png"}});
    journal.end(d, ToolResult::ok());
    const uint64_t e = journal.begin("set_settings", {{"values", {{"wall_loops", 3}}}});
    journal.end(e, ToolResult::error("no"));

    const json exported = journal.to_replay("repro");
    REQUIRE(exported["steps"].size() == 4);
    CHECK(exported["steps"][0]["await"] == false);
    CHECK(exported["steps"][0]["save_as"] == "job1");
    CHECK(exported["steps"][1]["tool"] == "answer_dialog");
    CHECK(exported["steps"][2]["args"]["job"] == "${jobs.job1}");
    CHECK(exported["steps"][3]["args"]["save_path"] == "${out_dir}/after.png");
}

TEST_CASE("only client calls of journaled tools reach the journal", "[Mcp]")
{
    Harness h;
    ToolDef look = tool("get_app_state", GuardClass::Inspect, [](const json&, ToolContext&) { return ToolResult::ok(); });
    look.inline_call = true;
    look.journal     = false;
    h.registry.add(look);
    ToolDef wait = tool("wait", GuardClass::Inspect, [](const json&, ToolContext&) { return ToolResult::ok(); });
    wait.inline_call = true;
    h.registry.add(wait);
    h.registry.add(tool("set_settings", GuardClass::Mutate, [](const json&, ToolContext& ctx) {
        ctx.set_journal_args({{"values", {{"wall_loops", 3}}}, {"scope", "process"}});
        return ToolResult::ok();
    }));
    h.start();
    h.call("get_app_state");
    h.call("wait", {{"for", "idle"}});
    h.call("set_settings", {{"values", {{"wall_loops", 3}}}});
    const json steps = h.journal.to_replay("x")["steps"];
    REQUIRE(steps.size() == 2);
    CHECK(steps[0]["tool"] == "wait");
    CHECK(steps[1]["args"]["scope"] == "process");
}

TEST_CASE("a plugin's call runs as the plugin's own job and never reaches the journal", "[Mcp]")
{
    Harness h;
    std::string seen, child_seen;
    h.registry.add(tool("get_app_state", GuardClass::Inspect, [&child_seen](const json&, ToolContext& ctx) {
        child_seen = ctx.caller_plugin();
        return ToolResult::ok();
    }));
    h.registry.add(tool("set_settings", GuardClass::Mutate, [&seen](const json&, ToolContext& ctx) {
        seen = ctx.caller_plugin();
        // A call it makes, as a replay step does, is the plugin's too.
        ctx.call_child("get_app_state", json::object());
        return ToolResult::ok({{"job", ctx.job_id()}});
    }));
    h.start();
    const ToolResult r = h.core->call_plugin(*h.registry.find("set_settings"), json::object(), "measure-plugin");
    REQUIRE_FALSE(r.is_error);
    CHECK(seen == "measure-plugin");
    CHECK(child_seen == "measure-plugin");
    CHECK(h.jobs.owner(r.data["job"].get<std::string>()) == "measure-plugin");
    CHECK(h.journal.to_replay("x")["steps"].empty());

    // A client's call has no plugin.
    h.call("set_settings");
    CHECK(seen.empty());
    CHECK(h.journal.to_replay("x")["steps"].size() == 1);
}

TEST_CASE("call_tool is journaled as the tool it calls", "[Mcp]")
{
    Harness h;
    h.registry.add(tool("set_settings", GuardClass::Mutate, [](const json&, ToolContext&) { return ToolResult::ok(); }));
    ToolDef api = tool("call_tool", GuardClass::Mutate, [](const json&, ToolContext&) -> ToolResult { throw ToolFailure("not dispatched"); });
    api.input_schema = {{"type", "object"}, {"properties", {{"tool", {{"type", "string"}}}, {"args", {{"type", "object"}}}}}};
    h.registry.add(api);
    h.start();
    McpProtocol protocol(h.registry, *h.core, ServerInfo{"orcaslicer", "OrcaSlicer", "9.9.9", ""});
    const PostReply reply = protocol.handle_post(PostRequest{json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
        {"params", {{"name", "call_tool"}, {"arguments", {{"tool", "set_settings"}, {"args", {{"values", {{"wall_loops", 3}}}}}}}}}}.dump()});
    CHECK(json::parse(reply.body)["result"].value("isError", false) == false);
    const json steps = h.journal.to_replay("x")["steps"];
    REQUIRE(steps.size() == 1);
    CHECK(steps[0]["tool"] == "set_settings");
    CHECK(steps[0]["args"]["values"]["wall_loops"] == 3);
}

TEST_CASE("a plugin's session tools act only on the jobs of its own calls", "[Mcp]")
{
    Harness h;
    h.registry.add(tool("make", GuardClass::Mutate, [](const json&, ToolContext& ctx) { return ToolResult::ok({{"job", ctx.job_id()}}); }));
    h.registry.add(tool("peek", GuardClass::Mutate, [&h](const json& args, ToolContext& ctx) {
        require_own_job(ctx, h.jobs, args.value("job", std::string()), "waits for");
        return ToolResult::ok();
    }));
    h.start();
    const std::string clients = h.call("make").data["job"].get<std::string>();
    const std::string plugins = h.core->call_plugin(*h.registry.find("make"), json::object(), "measure").data["job"].get<std::string>();

    const ToolResult foreign = h.core->call_plugin(*h.registry.find("peek"), json{{"job", clients}}, "measure");
    CHECK(foreign.is_error);
    CHECK(foreign.message.find("only the jobs of its own calls") != std::string::npos);
    CHECK_FALSE(h.core->call_plugin(*h.registry.find("peek"), json{{"job", plugins}}, "measure").is_error);
    // A client's call is not a plugin's, whoever's job it names.
    CHECK_FALSE(h.call("peek", json{{"job", plugins}}).is_error);
}

TEST_CASE("an unloaded plugin's calls start no job, nor do the children of one already running", "[Mcp]")
{
    Harness h;
    std::atomic<bool> release{false};
    ToolResult        child;
    h.registry.add(tool("make", GuardClass::Mutate, [](const json&, ToolContext&) { return ToolResult::ok(); }));
    h.registry.add(tool("slow", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        child = ctx.call_child("make", json::object());
        return ToolResult::ok();
    }));
    h.timing.soft_budget = std::chrono::milliseconds(10000);
    h.start();
    // A call that was admitted before the unload and is still running when it comes.
    std::thread running([&] { h.core->call_plugin(*h.registry.find("slow"), json::object(), "measure"); });
    while (h.jobs.running_owned_by("measure").empty())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    h.jobs.block_owner("measure");
    const ToolResult late = h.core->call_plugin(*h.registry.find("make"), json::object(), "measure");
    CHECK(late.is_error);
    CHECK(late.message.find("unloaded") != std::string::npos);
    release = true;
    running.join();
    CHECK(child.is_error);

    // Another plugin's calls and a client's are not affected; a load lifts the block.
    CHECK_FALSE(h.core->call_plugin(*h.registry.find("make"), json::object(), "other").is_error);
    CHECK_FALSE(h.call("make").is_error);
    h.jobs.unblock_owner("measure");
    CHECK_FALSE(h.core->call_plugin(*h.registry.find("make"), json::object(), "measure").is_error);
}

TEST_CASE("a plugin's calls are refused once it unloads, before the block comes, inline calls included", "[Mcp]")
{
    Harness           h;
    std::atomic<bool> loaded{true};
    std::atomic<bool> release{false};
    std::atomic<int>  looks{0};
    ToolResult        inline_child;
    ToolResult        job_child;
    // The plugin manager's state, which changes as the plugin unloads; end_plugin_scope()'s block comes later.
    h.jobs.set_owner_liveness([&](const std::string& plugin) { return plugin != "measure" || loaded; });
    ToolDef look = tool("look", GuardClass::Inspect, [&](const json&, ToolContext&) { ++looks; return ToolResult::ok(); });
    look.inline_call = true;
    h.registry.add(look);
    h.registry.add(tool("make", GuardClass::Mutate, [](const json&, ToolContext&) { return ToolResult::ok(); }));
    h.registry.add(tool("slow", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        inline_child = ctx.call_child("look", json::object());
        job_child    = ctx.call_child("make", json::object());
        return ToolResult::ok();
    }));
    h.timing.soft_budget = std::chrono::milliseconds(10000);
    h.start();
    std::thread running([&] { h.core->call_plugin(*h.registry.find("slow"), json::object(), "measure"); });
    while (h.jobs.running_owned_by("measure").empty())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    loaded = false;
    const ToolResult inline_call = h.core->call_plugin(*h.registry.find("look"), json::object(), "measure");
    CHECK(inline_call.is_error);
    CHECK(inline_call.message.find("unloaded") != std::string::npos);
    CHECK(h.core->call_plugin(*h.registry.find("make"), json::object(), "measure").is_error);
    release = true;
    running.join();
    CHECK(inline_child.is_error);
    CHECK(job_child.is_error);
    CHECK(looks == 0);

    // Another plugin's calls and a client's still run, and so do the plugin's once it is loaded again.
    CHECK_FALSE(h.core->call_plugin(*h.registry.find("look"), json::object(), "other").is_error);
    CHECK_FALSE(h.call("look").is_error);
    loaded = true;
    CHECK_FALSE(h.core->call_plugin(*h.registry.find("look"), json::object(), "measure").is_error);
    CHECK(looks == 3);
}

TEST_CASE("a plugin's job never starts once the plugin unloads, as it registers or right before its handler", "[Mcp]")
{
    // The liveness hook is asked as the call comes in, once its job is registered, and on the job's thread right
    // before its handler starts: the plugin unloads just before the asking given here.
    const int        unload_at = GENERATE(1, 2, 3);
    Harness          h;
    std::atomic<int> asked{0};
    std::atomic<int> ran{0};
    std::atomic<bool> loaded{true};
    h.jobs.set_owner_liveness([&](const std::string&) {
        if (++asked == unload_at)
            loaded = false;
        return loaded.load();
    });
    h.registry.add(tool("make", GuardClass::Mutate, [&](const json&, ToolContext&) { ++ran; return ToolResult::ok(); }));
    h.timing.soft_budget = std::chrono::milliseconds(10000);
    h.start();
    const ToolResult r = h.core->call_plugin(*h.registry.find("make"), json::object(), "measure");
    CHECK(r.is_error);
    CHECK(r.message.find("unloaded") != std::string::npos);
    CHECK(ran == 0);
    CHECK(h.jobs.running_owned_by("measure").empty());
}

TEST_CASE("a plugin's running call is cancelled the moment the plugin unloads", "[Mcp]")
{
    Harness           h;
    std::atomic<bool> loaded{true};
    std::atomic<bool> started{false};
    h.jobs.set_owner_liveness([&](const std::string&) { return loaded.load(); });
    h.registry.add(tool("spin", GuardClass::Mutate, [&](const json&, ToolContext& ctx) {
        started = true;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ctx.cancelled() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return ToolResult::ok({{"cancelled", ctx.cancelled()}});
    }));
    h.timing.soft_budget = std::chrono::milliseconds(10000);
    h.start();
    // No block and no stop request: only the plugin manager's state changes.
    std::thread unload([&] {
        while (!started)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        loaded = false;
    });
    const ToolResult r = h.core->call_plugin(*h.registry.find("spin"), json::object(), "measure");
    unload.join();
    CHECK_FALSE(r.is_error);
    CHECK(r.data.value("cancelled", false));
}

TEST_CASE("a call under a lineage node sees a stop asked for the node, inline calls included, and a renewed node's calls run", "[Mcp]")
{
    Harness h;
    // Polls for cancellation for up to `for_ms`, as wait does.
    const auto poll = [](const json& args, ToolContext& ctx) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.value("for_ms", 5000));
        while (!ctx.cancelled() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(10ms);
        return ToolResult::ok({{"cancelled", ctx.cancelled()}});
    };
    ToolDef inline_poll     = tool("inline_poll", GuardClass::Inspect, poll);
    inline_poll.inline_call = true;
    h.registry.add(inline_poll);
    h.registry.add(tool("job_poll", GuardClass::Mutate, poll));
    h.timing.soft_budget = 10000ms;
    h.start();

    const std::string session = h.jobs.create("run_script", "");
    h.jobs.set_idle(session);
    LineageNode node(h.jobs, "run_script", session);
    const auto  call = [&](const char* tool, int for_ms) {
        return std::async(std::launch::async, [&h, &node, tool, for_ms] {
            return h.core->call_child_named(tool, {{"for_ms", for_ms}}, node.id());
        });
    };
    auto inline_call = call("inline_poll", 5000);
    auto job_call    = call("job_poll", 5000);
    std::this_thread::sleep_for(300ms);
    const auto asked = std::chrono::steady_clock::now();
    h.jobs.request_stop(node.id());
    const ToolResult stopped_inline = inline_call.get();
    const ToolResult stopped_job    = job_call.get();
    CHECK(std::chrono::steady_clock::now() - asked < 1s);
    CHECK(stopped_inline.data.value("cancelled", false));
    CHECK(stopped_job.data.value("cancelled", false));

    node.renew();
    auto ran_inline = call("inline_poll", 300);
    auto ran_job    = call("job_poll", 300);
    CHECK(ran_inline.get().data["cancelled"] == false);
    CHECK(ran_job.get().data["cancelled"] == false);
    CHECK_FALSE(h.jobs.stop_requested(session));
    h.jobs.finish(session, ToolResult::ok());
}

TEST_CASE("lineage nodes leave no running job behind", "[Mcp]")
{
    JobRegistry       jobs;
    const std::string session  = jobs.create("run_script", "");
    const auto        running_nodes = [&] {
        int count = 0;
        for (const json& job : jobs.list())
            if (job["tool"] == "run_script" && job.value("parent", std::string()) == session && job["state"] == "running")
                ++count;
        return count;
    };
    {
        LineageNode node(jobs, "run_script", session);
        CHECK(running_nodes() == 1);
    }
    for (int round = 0; round < 10; ++round) {
        LineageNode node(jobs, "run_script", session);
        for (int renewal = 0; renewal < 3; ++renewal)
            node.renew();
        CHECK(running_nodes() == 1);
    }
    CHECK(running_nodes() == 0);
    jobs.finish(session, ToolResult::ok());
}

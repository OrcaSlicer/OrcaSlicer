// Scene tools: object_transform's size argument, the arguments of the commands that replace a dialog, the
// notifications MCP operations raise before the 3D view has drawn, object_arrange's options, the admission that
// holds a UI worker job a call starts, how object_list describes objects, instances and parts,
// part_add's type names and shape sizes, the object structure commands, the targets a command reads from its arguments
// and the reply that names them, the height ranges object_set_height_ranges takes and object_list reports,
// plate_set_custom_gcode's edits of the layer slider, view_set's display switches and a plate's place on the bed.
// GLCanvas3D.hpp reaches wx's MSW headers after OCCT has hidden the USER and GDI declarations they need;
// the Windows headers first keep them, as in test_plugin_host_api.cpp.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
#endif

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <admesh/stl.h>
#include <imgui/imgui.h>
#include <nlohmann/json.hpp>

#include "mcp_test_utils.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Mcp/JobRegistry.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpCore.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/McpTargets.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/UiInvoker.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Operations/ObjectOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/PartPlate.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using Slic3r::GUI::Mcp::Test::NoUiContext;
using namespace Slic3r;
using namespace Slic3r::GUI;
using nlohmann::json;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

TEST_CASE("size targets scale uniformly when one axis is given", "[Mcp][Scene]")
{
    const Vec3d current(60., 20., 5.);
    CHECK(Mcp::size_factors(current, json::array({80, nullptr, nullptr})).isApprox(Vec3d::Constant(4. / 3.)));
    CHECK(Mcp::size_factors(current, json::array({nullptr, nullptr, 10})).isApprox(Vec3d::Constant(2.)));
    CHECK(Mcp::size_factors(current, json::array({80, 30, nullptr})).isApprox(Vec3d(4. / 3., 1.5, 1.)));
    CHECK(Mcp::size_factors(current, json::array({30, 10, 5})).isApprox(Vec3d(0.5, 0.5, 1.)));

    CHECK_THROWS_AS(Mcp::size_factors(current, json::array({0, nullptr, nullptr})), ToolFailure);
    CHECK_THROWS_AS(Mcp::size_factors(current, json::array({-80, nullptr, nullptr})), ToolFailure);
    CHECK_THROWS_AS(Mcp::size_factors(current, json::array({80, 30})), ToolFailure);
    CHECK_THROWS_AS(Mcp::size_factors(current, json::array({nullptr, nullptr, nullptr})), ToolFailure);
    // A flat shape has no size to scale along its flat axis.
    CHECK_THROWS_AS(Mcp::size_factors(Vec3d(50., 50., 0.), json::array({nullptr, nullptr, 2})), ToolFailure);
}

TEST_CASE("a primitive's size has one number above 0 per dimension its dialog asks for", "[Mcp][Scene]")
{
    CHECK(is_primitive_size("Cube", {40., 40., 10.}));
    CHECK(is_primitive_size("Cylinder", {90., 4.}));
    CHECK(is_primitive_size("Sphere", {25.6}));
    CHECK(is_primitive_size("Torus", {30., 10.}));

    CHECK_FALSE(is_primitive_size("Cube", {40., 40.}));
    CHECK_FALSE(is_primitive_size("Sphere", {25., 25.}));
    CHECK_FALSE(is_primitive_size("Cube", {40., 0., 10.}));
    CHECK_FALSE(is_primitive_size("Cube", {40., -1., 10.}));
    CHECK_FALSE(is_primitive_size("Cube", {40., std::numeric_limits<double>::infinity(), 10.}));
    CHECK_FALSE(is_primitive_size("Cube", {}));
    // A shape with no size dialog takes the empty size alone.
    CHECK(is_primitive_size("Benchy", {}));
    CHECK_FALSE(is_primitive_size("Benchy", {10.}));
}

TEST_CASE("the arrange options keep Align to Y axis off while auto rotation is on", "[Mcp][Scene]")
{
    GLCanvas3D::ArrangeSettings settings;
    settings.align_to_y_axis = true;
    CHECK_FALSE(settings.apply_option_rules());
    CHECK(settings.align_to_y_axis);

    settings.enable_rotation = true;
    CHECK(settings.apply_option_rules());
    CHECK_FALSE(settings.align_to_y_axis);
    // Nothing left to turn off.
    CHECK_FALSE(settings.apply_option_rules());
    CHECK(settings.enable_rotation);
}

// A command that replaces a dialog takes, as JSON arguments, what the dialog asks; Go to layer and Go to tab, actions of
// view_set, take the value of the Speed Dial's second phase, which they need.
TEST_CASE("the commands that replace a dialog take the arguments the table gives", "[Mcp][Scene]")
{
    using Names       = std::set<std::string>;
    const auto names  = [](const char* key) {
        Names out;
        if (const NativeCommand* c = NativeCommands::find(key))
            for (const auto& [name, schema] : c->input_schema.at("properties").items())
                out.insert(name);
        return out;
    };
    const Names path{"path"};
    const Names recording{"target", "fps",   "max_dim", "jpeg_quality", "max_seconds", "captions",  "pointer", "encode",
                          "encode_preset", "codec", "crf", "speed", "profile", "keep_raw_video_file", "save_path", "overwrite",
                          "silent"};
    REQUIRE(names("preset_export") == path);
    // The SVG's file, and the object it joins as a part of a type, which the Add menu of that object would give it.
    REQUIRE(names("object_add_svg") == Names{"path", "object", "object_id", "type"});
    const Names view = names("view_set");
    for (const char* name : {"tab", "layer_percent", "camera", "projection", "layout"}) {
        INFO(name);
        CHECK(view.count(name) == 1);
    }
    // A command whose action asks takes, in its place, the arguments of the action and silent.
    REQUIRE(names("project_open") == Names{"path", "discard_changes", "silent"});
    REQUIRE(names("project_import") == Names{"paths", "entries", "archive", "silent"});
    REQUIRE(names("project_save") == Names{"path", "overwrite", "save_as", "silent"});
    REQUIRE(names("project_export") == Names{"format", "path", "objects", "object_ids", "plates", "per_object", "overwrite", "silent"});
    REQUIRE(names("preset_import") == Names{"path", "on_conflict", "silent"});
    REQUIRE(names("plugin_install_local") == Names{"path", "overwrite", "silent"});
    REQUIRE(names("window_open") == Names{"window", "tab", "key"});
    // The object commands made from menu items take their targets, by index or by the ids object_list reports.
    REQUIRE(names("object_rename") == Names{"object", "object_id", "part", "part_id", "name", "silent"});
    REQUIRE(names("object_set_instances") == Names{"object", "object_id", "count", "delta", "silent"});
    REQUIRE(names("object_delete") ==
            Names{"objects", "object_ids", "instances", "instance_ids", "parts", "part_ids", "plate", "all", "silent"});
    REQUIRE(names("object_separate_instances") == Names{"object", "object_id", "instances", "instance_ids"});
    REQUIRE(names("object_split") == Names{"object", "object_id", "into", "keep_z", "silent"});
    REQUIRE(names("object_assemble") == Names{"objects", "object_ids"});
    REQUIRE(names("part_change_type") == Names{"parts", "part_ids", "type", "subtype"});
    REQUIRE(names("object_order") == Names{"plate", "by", "order", "order_ids", "descending", "all"});
    REQUIRE(names("object_add_primitive") == Names{"shape", "size", "plate", "position", "name", "silent"});
    REQUIRE(names("object_mirror") == Names{"objects", "object_ids", "parts", "part_ids", "axis"});
    REQUIRE(names("object_center") == Names{"objects", "object_ids", "plate"});
    REQUIRE(names("object_scale_to_fit") == Names{"objects", "object_ids"});
    REQUIRE(names("object_drop") == Names{"objects", "object_ids", "parts", "part_ids"});
    REQUIRE(names("object_auto_orient") == Names{"objects", "object_ids", "plates"});
    REQUIRE(names("object_set_printable") == Names{"objects", "object_ids", "instances", "instance_ids", "printable"});
    REQUIRE(names("object_set_auto_drop") == Names{"objects", "object_ids", "auto_drop"});
    REQUIRE(names("object_fix") == Names{"objects", "object_ids", "parts", "part_ids", "plate", "all"});
    REQUIRE(names("object_clone") == Names{"objects", "object_ids", "count", "arrange"});
    REQUIRE(names("object_reload_from_disk") == Names{"objects", "object_ids", "parts", "part_ids", "all"});
    REQUIRE(names("part_replace") == Names{"object", "object_id", "part", "part_id", "path"});
    // The plate commands made from menu items take the plate, and what their dialog or icon gives.
    REQUIRE(names("plate_select") == Names{"plate"});
    REQUIRE(names("plate_delete") == Names{"plate"});
    REQUIRE(names("plate_duplicate") == Names{"plate"});
    REQUIRE(names("plate_rename") == Names{"plate", "name", "silent"});
    REQUIRE(names("plate_lock") == Names{"plate", "locked"});
    REQUIRE(names("filament_delete") == Names{"filament_slot", "merge_into", "break_mix", "confirm_shift"});
    REQUIRE(names("filament_add_mixed") == Names{"components", "ratios"});
    REQUIRE(names("filament_edit_mixed") == Names{"filament_slot", "components", "ratios"});
    REQUIRE(names("filament_set_colour") == Names{"filament_slot", "colour"});
    REQUIRE(names("recording_start") == recording);
    // The object arrange fills a plate with is named as an object, not as a count.
    CHECK(names("object_arrange").count("fill_object") == 1);
    CHECK(names("object_arrange").count("fill") == 0);

    // A command with an action of its own requires nothing: it does its person's form when it is given no arguments.
    for (const NativeCommands::SpeedDialAction& t : NativeCommands::speed_dial_actions(NativeCommands::instance())) {
        if (t.action != nullptr)
            continue;
        INFO(t.command->key);
        CHECK_FALSE(t.command->input_schema.contains("required"));
    }
}

TEST_CASE("the view, window and gesture tools are titled by all they do", "[Mcp][Scene]")
{
    const auto title = [](const char* key) {
        const NativeCommand* c = NativeCommands::find(key);
        return c != nullptr ? c->title : std::string();
    };
    CHECK(title("view_set") == "Set up the view");
    CHECK(title("window_set") == "Size, move or raise a window");
    CHECK(title("mouse_gesture") == "Mouse gesture on a 3D view or page element");
    CHECK(title("gizmo_pick_file") == "Load or save the open gizmo's SVG or texture file");
}

// The app opens on Home, so a slice or arrange an agent starts at once updates its progress
// notification before any ImGui frame has built the font. That used to dereference a null font.
TEST_CASE("a progress notification updated before the first ImGui frame does not measure text", "[Mcp][Scene]")
{
    ImGuiWrapper imgui; // a context with no frame yet, so no current font
    REQUIRE(ImGui::GetFont() == nullptr);

    NotificationManager notifications(nullptr);
    notifications.init_progress_indicator();
    notifications.progress_indicator_set_status_text("Arranging");
    notifications.progress_indicator_set_status_text("Arranging done");
    CHECK(ImGui::GetFont() == nullptr);
}

TEST_CASE("arrange options follow the Arrange options panel's rules", "[Mcp][Scene]")
{
    GLCanvas3D::ArrangeSettings settings;
    settings.distance                            = 5.f;
    settings.align_to_y_axis                     = true;
    settings.allow_multi_materials_on_same_plate = false;

    // What is not given keeps its value.
    CHECK(Mcp::apply_arrange_args(settings, {{"spacing", 10}}).empty());
    CHECK(settings.distance == 10.f);
    CHECK(settings.align_to_y_axis);
    CHECK_FALSE(settings.allow_multi_materials_on_same_plate);
    Mcp::apply_arrange_args(settings, {{"spacing", -1}});
    CHECK(settings.distance == 0.f);

    // Rotation turns Align to Y axis off, and an align_y asked for meanwhile is reported, not applied.
    CHECK(Mcp::apply_arrange_args(settings, {{"rotate", true}}).empty());
    CHECK_FALSE(settings.align_to_y_axis);
    CHECK_FALSE(Mcp::apply_arrange_args(settings, {{"align_y", true}}).empty());
    CHECK_FALSE(settings.align_to_y_axis);
    CHECK(Mcp::apply_arrange_args(settings, {{"rotate", false}, {"align_y", true}, {"multi_material", true}, {"spacing", 0.3}}).empty());
    CHECK(Mcp::arrange_settings_json(settings) == json{{"spacing", 0.3}, {"rotate", false}, {"align_y", true}, {"multi_material", true}});
}

namespace {

// Plays the UI thread: runs posted closures in order on a thread of its own.
class UiThread final : public Mcp::UiDispatcher
{
public:
    UiThread() : m_thread([this] { loop(); }) {}
    ~UiThread() override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
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

private:
    void loop()
    {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                if (m_queue.empty())
                    return;
                fn = std::move(m_queue.front());
                m_queue.pop_front();
            }
            fn();
        }
    }

    std::mutex                        m_mutex;
    std::condition_variable           m_cv;
    std::deque<std::function<void()>> m_queue;
    bool                              m_stop = false;
    std::thread                       m_thread;
};

// The UI job worker: busy until its job's end, which a cancel brings forward to `cancel_delay` from
// then, as a job that notices the cancel between steps.
struct FakeWorker
{
    using Clock = std::chrono::steady_clock;

    std::mutex                mutex;
    Clock::time_point         end;
    std::chrono::milliseconds cancel_delay{0};
    int                       cancels = 0;

    void start(std::chrono::milliseconds length)
    {
        std::lock_guard<std::mutex> lock(mutex);
        end = Clock::now() + length;
    }
    bool busy()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return Clock::now() < end;
    }
    void cancel()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++cancels;
        end = std::min(end, Clock::now() + cancel_delay);
    }
};

// A core with stand-ins for the tools that start an arrange (the typed object_arrange, action_run of the
// obj_arrange action, a click on a plate's Arrange icon, which queues its event), change the scene (a
// mutate tool, a key on the canvas), answer a dialog, read, and cancel.
struct Harness
{
    UiThread                      ui;
    FakeWorker                    worker;
    Mcp::UiInvoker                invoker{ui, [] { return Mcp::GuardState{}; }};
    Mcp::JobRegistry              jobs;
    Mcp::Journal                  journal;
    CommandCatalog                registry;
    Mcp::CoreTiming               timing;
    std::chrono::milliseconds     job_length{800};
    std::unique_ptr<Mcp::McpCore> core;

    Harness()
    {
        timing.soft_budget = 100ms;
        add("object_arrange", GuardClass::Mutate, [this](const json&, ToolContext& ctx) {
            ctx.run_ui([&] {
                worker.start(job_length);
                return json();
            });
            while (ctx.observe_ui([&] { return json(worker.busy()); }).get<bool>())
                std::this_thread::sleep_for(20ms);
            return ToolResult::ok({{"arranged", true}});
        });
        add("action_run", GuardClass::Mutate, [this](const json&, ToolContext& ctx) {
            ctx.run_ui([&] {
                worker.start(job_length);
                return json();
            });
            return ToolResult::ok({{"ran", "orca_command:obj_arrange:orca"}});
        });
        add("widget_click", GuardClass::Recover, [this](const json&, ToolContext& ctx) {
            ctx.run_ui([&] {
                core->admit({}); // the target is outside any dialog
                ui.post([this] { worker.start(job_length); });
                return json();
            });
            ctx.observe_ui([] { return json(); }); // settle
            return ToolResult::ok();
        });
        add("keyboard_press", GuardClass::Recover, [this](const json&, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([&] {
                core->admit({});
                return json("deleted");
            }));
        });
        add("dialog_answer", GuardClass::Recover,
            [](const json&, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([] { return json("pressed"); })); });
        add("edit", GuardClass::Mutate,
            [](const json&, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([] { return json("edited"); })); });
        add("read", GuardClass::Inspect,
            [this](const json&, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return json(worker.busy()); })); });
        add("job_cancel", GuardClass::Recover, [this](const json&, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([&] {
                for (const std::string& op : jobs.running_operations())
                    if (core->cancel_worker(op))
                        return json(op);
                return json();
            }));
        });
    }
    ~Harness()
    {
        invoker.shutdown();
        jobs.wake_all();
        if (core)
            core->shutdown();
    }

    void add(const std::string& name, GuardClass guard, CommandFn handler)
    {
        NativeCommand d;
        d.key                      = name;
        d.guard                    = guard;
        d.inline_call              = guard == GuardClass::Inspect;
        d.allowed_during_operation = name == "job_cancel";
        d.budget_ms                = 200;
        d.run                      = std::move(handler);
        registry.add(std::move(d));
    }
    void start()
    {
        core = std::make_unique<Mcp::McpCore>(registry, invoker, jobs, &journal, timing,
                                              Mcp::WorkerHooks{[this] { return worker.busy(); }, [this] { worker.cancel(); }});
    }
    ToolResult call(const std::string& tool) { return core->call(*registry.find(tool), json::object()); }

    // Starts a worker job with `tool`, which outlives the reply budget; returns the call's job.
    std::string start_job(const std::string& tool)
    {
        const ToolResult started = call(tool);
        REQUIRE_FALSE(started.is_error);
        REQUIRE(started.data["status"] == "running");
        return started.data["job"];
    }
    bool refused_by(const ToolResult& r, const std::string& job)
    {
        return r.is_error && r.message.find("Busy: job " + job + " is running an operation") != std::string::npos;
    }
};

} // namespace

TEST_CASE("a worker job a call starts holds admission past the reply budget until the worker is idle", "[Mcp][Scene]")
{
    Harness h;
    h.start();
    const std::string job = h.start_job("action_run");
    CHECK(h.refused_by(h.call("edit"), job));
    CHECK_FALSE(h.call("read").is_error);

    const auto done = h.jobs.wait(job, 5000ms);
    REQUIRE(done.has_value());
    CHECK_FALSE(done->is_error);
    CHECK(done->data["ran"] == "orca_command:obj_arrange:orca");
    // The job ends only once the worker is idle, so a change right after it is admitted.
    CHECK_FALSE(h.worker.busy());
    CHECK_FALSE(h.call("edit").is_error);
}

TEST_CASE("a cancelled worker job keeps admission until the worker is really idle", "[Mcp][Scene]")
{
    Harness h;
    h.job_length          = 10s;
    h.worker.cancel_delay = 800ms; // slower than the reply budget
    SECTION("job_cancel")
    {
        h.start();
        const std::string job       = h.start_job("object_arrange");
        const ToolResult cancel      = h.call("job_cancel");
        REQUIRE_FALSE(cancel.is_error);
        CHECK(cancel.data == job);
        CHECK(h.refused_by(h.call("edit"), job));
        std::this_thread::sleep_for(300ms);
        CHECK(h.worker.busy());
        CHECK(h.refused_by(h.call("edit"), job));

        const auto done = h.jobs.wait(job, 5000ms);
        REQUIRE(done.has_value());
        CHECK(done->is_error);
        CHECK(done->message.find("cancelled") != std::string::npos);
    }
    SECTION("timeout")
    {
        h.timing.worker_timeout = 300ms;
        h.start();
        const std::string job = h.start_job("action_run");
        std::this_thread::sleep_for(500ms); // past the timeout, before the cancelled job stops
        CHECK(h.worker.cancels == 1);
        CHECK(h.worker.busy());
        CHECK(h.refused_by(h.call("edit"), job));

        const auto done = h.jobs.wait(job, 5000ms);
        REQUIRE(done.has_value());
        CHECK(done->is_error);
        CHECK(done->message.find("did not finish within") != std::string::npos);
    }
    CHECK(h.worker.cancels == 1);
    CHECK_FALSE(h.worker.busy());
    CHECK_FALSE(h.call("edit").is_error);
}

TEST_CASE("a change is refused while an arrange an MCP call started runs, however it started", "[Mcp][Scene]")
{
    // The typed object_arrange, action_run of the obj_arrange action, and a recover input whose click queues the job.
    for (const char* starter : {"object_arrange", "action_run", "widget_click"}) {
        DYNAMIC_SECTION(starter)
        {
            Harness h;
            h.start();
            const std::string job = h.start_job(starter);
            CHECK(h.refused_by(h.call("edit"), job));
            const auto done = h.jobs.wait(job, 5000ms);
            REQUIRE(done.has_value());
            CHECK_FALSE(done->is_error);
            CHECK_FALSE(h.worker.busy());
            CHECK_FALSE(h.call("edit").is_error);
        }
    }
}

TEST_CASE("a scene-changing input is refused while that job runs, and a dialog answer is not", "[Mcp][Scene]")
{
    Harness h;
    h.start();
    const std::string job = h.start_job("action_run");
    CHECK(h.refused_by(h.call("keyboard_press"), job));   // Delete on the canvas
    CHECK(h.refused_by(h.call("widget_click"), job)); // a GL toolbar item
    CHECK_FALSE(h.call("dialog_answer").is_error);
    REQUIRE(h.jobs.wait(job, 5000ms).has_value());
    CHECK_FALSE(h.call("keyboard_press").is_error);
}

TEST_CASE("a worker job that starts between a call's closures is the call's, whoever started it", "[Mcp][Scene]")
{
    // The worker doesn't record who queued a job, so a person's job that starts while a call that
    // found the worker idle is still running is claimed by that call, as one its change queued is.
    Harness h;
    h.add("settle", GuardClass::Mutate, [](const json&, ToolContext& ctx) {
        ctx.run_ui([] { return json("edited"); });
        for (int i = 0; i < 10; ++i) {
            ctx.observe_ui([] { return json(); });
            std::this_thread::sleep_for(20ms);
        }
        return ToolResult::ok();
    });
    h.start();
    ToolResult      settled;
    std::thread     caller([&h, &settled] { settled = h.call("settle"); });
    std::this_thread::sleep_for(60ms);
    h.ui.post([&h] { h.worker.start(h.job_length); }); // a person starts Text typing
    caller.join();
    REQUIRE(settled.data["status"] == "running");
    const std::string job = settled.data["job"];
    CHECK(h.refused_by(h.call("edit"), job));
    REQUIRE(h.jobs.wait(job, 5000ms).has_value());
    CHECK_FALSE(h.worker.busy());
}

TEST_CASE("a worker job a person started refuses changes but not reads, and job_cancel leaves it", "[Mcp][Scene]")
{
    Harness h;
    h.start();
    h.worker.start(800ms); // Text typing
    for (const char* change : {"edit", "keyboard_press"}) {
        const ToolResult refused = h.call(change);
        CHECK(refused.is_error);
        CHECK(refused.message.find("Busy: a background job") != std::string::npos);
    }
    const ToolResult read = h.call("read");
    CHECK_FALSE(read.is_error);
    CHECK(read.data == true);
    CHECK(h.call("job_cancel").data.is_null());
    CHECK(h.worker.cancels == 0);
    while (h.worker.busy())
        std::this_thread::sleep_for(20ms);
    CHECK_FALSE(h.call("edit").is_error);
}

namespace {

void check_vec3(const json& v, const Vec3d& expected)
{
    REQUIRE(v.is_array());
    for (int i = 0; i < 3; ++i)
        CHECK_THAT(v[i].get<double>(), WithinAbs(expected[i], 1e-6));
}

} // namespace

TEST_CASE("a part's position and bounding box are on the bed", "[Mcp][Scene]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART, false);
    ModelVolume* cylinder = object->add_volume(make_cylinder(3., 4.), ModelVolumeType::MODEL_PART, false);
    cylinder->set_offset(Vec3d(5., 0., 0.));
    ModelVolume* modifier = object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER, false);
    modifier->set_offset(Vec3d(50., 0., 0.));
    ModelInstance* inst = object->add_instance();
    inst->set_offset(Vec3d(100., 100., 10.));
    inst->set_rotation(Vec3d(0., 0., PI / 2.));

    json info = Mcp::volume_info(*cylinder, 1);
    check_vec3(info["position"], Vec3d(100., 105., 10.));
    check_vec3(info["bbox"]["min"], Vec3d(97., 102., 10.));
    check_vec3(info["bbox"]["max"], Vec3d(103., 108., 14.));
    // Rotation and scale are the part's own, in the object's frame.
    check_vec3(info["rotation_deg"], Vec3d::Zero());
    check_vec3(info["scale"], Vec3d::Ones());
    CHECK_THAT(info["scaled_volume"].get<double>(), WithinAbs(info["mesh_volume"].get<double>(), 0.001));

    // The instance's scale moves the part and grows its printed volume, not its mesh volume.
    inst->set_scaling_factor(Vec3d::Constant(2.));
    info = Mcp::volume_info(*cylinder, 1);
    check_vec3(info["position"], Vec3d(100., 110., 10.));
    CHECK_THAT(info["scaled_volume"].get<double>(), WithinAbs(8. * info["mesh_volume"].get<double>(), 0.01));

    // Boxes of the object and its instances count model parts only: the modifier sticks out.
    const json object_json = Mcp::object_info(*object, 0);
    CHECK(object_json["instances"][0]["bbox"] == object_json["bbox"]);
    CHECK(object_json["bbox"]["max"][1].get<double>() < 141.);
    CHECK(object_json["parts"][2]["bbox"]["max"][1].get<double>() > 200.);
}

TEST_CASE("a mirrored copy reports mirrored", "[Mcp][Scene]")
{
    Model        model;
    ModelObject* object = model.add_object();
    ModelVolume* part   = object->add_volume(make_cube(20., 10., 5.), ModelVolumeType::MODEL_PART, false);
    object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    object->add_instance()->set_mirror(Vec3d(-1., 1., 1.));
    // Mirrored in X and in Y is a half turn, not a mirror.
    object->add_instance()->set_mirror(Vec3d(-1., -1., 1.));
    part->set_mirror(Vec3d(1., -1., 1.));

    const json info = Mcp::object_info(*object, 0);
    CHECK(info["instances"][0]["mirrored"] == false);
    CHECK(info["instances"][1]["mirrored"] == true);
    check_vec3(info["instances"][1]["scale"], Vec3d::Ones());
    CHECK(info["instances"][2]["mirrored"] == false);
    CHECK(info["parts"][0]["mirrored"] == true);
    check_vec3(info["parts"][0]["scale"], Vec3d::Ones());
    CHECK(info["parts"][1]["mirrored"] == false);
}

TEST_CASE("a part's filament falls back to its object's", "[Mcp][Scene]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false);
    object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false)->config.set("extruder", 0);
    object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false)->config.set("extruder", 3);
    object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER, false);
    object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER, false)->config.set("extruder", 4);
    object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::NEGATIVE_VOLUME, false)->config.set("extruder", 2);
    object->add_instance();

    // Unset ("Default") is filament 1 on an object.
    json info = Mcp::object_info(*object, 0);
    CHECK(info["filament"] == 1);
    CHECK(info["parts"][0]["filament"] == 1);

    object->config.set("extruder", 2);
    info = Mcp::object_info(*object, 0);
    CHECK(info["filament"] == 2);
    CHECK(info["parts"][0]["filament"] == 2);
    CHECK(info["parts"][1]["filament"] == 2);
    CHECK(info["parts"][2]["filament"] == 3);
    // A modifier without its own filament leaves the filament of what it covers alone.
    CHECK_FALSE(info["parts"][3].contains("filament"));
    CHECK(info["parts"][4]["filament"] == 4);
    CHECK_FALSE(info["parts"][5].contains("filament"));
}

TEST_CASE("a precise seam reports its subtype, and an object its auto drop", "[Mcp][Scene]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false);
    object->add_volume(make_cube(2., 2., 2.), ModelVolumeType::PRECISE_SEAM_LEFT, false);
    object->add_volume(make_cube(2., 2., 2.), ModelVolumeType::PRECISE_SEAM_NEUTRAL, false);
    object->add_instance();
    object->auto_drop = false;

    const json info = Mcp::object_info(*object, 0);
    CHECK(info["auto_drop"] == false);
    CHECK_FALSE(info["parts"][0].contains("subtype"));
    CHECK(info["parts"][1]["type"] == "precise_seam");
    CHECK(info["parts"][1]["subtype"] == "left");
    CHECK(info["parts"][2]["subtype"] == "neutral");
    CHECK_FALSE(info["parts"][1].contains("filament"));
}

TEST_CASE("object_list reports an object's mesh errors: its open edges and what was repaired", "[Mcp][Scene]")
{
    Model model;
    // A 20 mm cube with one triangle taken out, which leaves three open edges, and a closed one whose mesh the loader
    // repaired: two edges fixed and a facet reversed.
    indexed_triangle_set open = make_cube(20., 20., 20.).its;
    open.indices.pop_back();
    ModelObject* holed = model.add_object();
    holed->add_volume(TriangleMesh(std::move(open)));
    holed->add_instance();
    RepairedMeshErrors repaired;
    repaired.edges_fixed     = 2;
    repaired.facets_reversed = 1;
    ModelObject* closed      = model.add_object();
    closed->add_volume(TriangleMesh(make_cube(20., 20., 20.).its, repaired));
    closed->add_instance();

    const json errors = Mcp::object_info(*holed, 0).at("mesh_errors");
    CHECK(errors["open_edges"] == 3);
    CHECK(errors["manifold"] == false);
    CHECK(errors["edges_fixed"] == 0);
    const json fixed = Mcp::object_info(*closed, 1).at("mesh_errors");
    CHECK(fixed["open_edges"] == 0);
    CHECK(fixed["manifold"] == true);
    CHECK(fixed["edges_fixed"] == 2);
    CHECK(fixed["facets_reversed"] == 1);
    for (const char* count : {"degenerate_facets", "facets_removed", "backwards_edges"}) {
        INFO(count);
        CHECK(fixed[count] == 0);
    }
}

TEST_CASE("Printable and Auto Drop take their state, and Fix Model all the objects or some", "[Mcp][Scene]")
{
    const auto command = [](const char* key) {
        const NativeCommand* found = NativeCommands::find(key);
        REQUIRE(found != nullptr);
        return found;
    };
    // The state is a required boolean.
    for (const auto& [key, state] : {std::pair<const char*, const char*>{"object_set_printable", "printable"},
                                     std::pair<const char*, const char*>{"object_set_auto_drop", "auto_drop"}}) {
        INFO(key);
        json none{{"objects", json::array({0})}};
        CHECK_THAT(Mcp::argument_error(*command(key), none), ContainsSubstring(state));
        json word{{"objects", json::array({0})}, {state, "yes"}};
        CHECK_THAT(Mcp::argument_error(*command(key), word), ContainsSubstring(state));
        json given{{"objects", json::array({0})}, {state, false}};
        CHECK(Mcp::argument_error(*command(key), given).empty());
    }
    json instance{{"instances", json::array({json::array({0, 1})})}, {"printable", false}};
    CHECK(Mcp::argument_error(*command("object_set_printable"), instance).empty());

    // all needs no other target; a plate, objects or parts name what is repaired.
    for (json args : {json{{"all", true}}, json{{"plate", 1}}, json{{"objects", json::array({0, 2})}},
                      json{{"parts", json::array({json::array({0, 1})})}}, json::object()}) {
        INFO(args.dump());
        CHECK(Mcp::argument_error(*command("object_fix"), args).empty());
    }
    // The repair is cancelled with job_cancel {job}.
    CHECK(command("object_fix")->cancellable);
}

TEST_CASE("Fix Model and Reload refuse all or a plate given with targets, and Fix Model all with a plate", "[Mcp][Scene]")
{
    const NativeCommand* fix    = NativeCommands::find("object_fix");
    const NativeCommand* reload = NativeCommands::find("object_reload_from_disk");
    REQUIRE(fix != nullptr);
    REQUIRE(reload != nullptr);
    // Each passes the schema, and the command refuses it before it reads the app.
    const std::vector<std::tuple<const NativeCommand*, json, std::string>> refused{
        {fix, {{"all", true}, {"plate", 0}}, "Give all or plate, not both"},
        {fix, {{"all", true}, {"objects", json::array({0})}}, "all repairs every object; leave the targets out"},
        {fix, {{"plate", 0}, {"parts", json::array({json::array({0, 1})})}}, "plate repairs the objects on it; leave the targets out"},
        {reload, {{"all", true}, {"objects", json::array({0})}}, "all reloads every object; leave the targets out"}};
    for (const auto& [command, args, message] : refused) {
        INFO(command->key << " " << args.dump());
        json given = args;
        CHECK(Mcp::argument_error(*command, given).empty());
        NoUiContext ctx;
        CHECK_THROWS_WITH(command->run(args, ctx), message);
    }
}

TEST_CASE("an order of objects is by a key or the objects named, of one plate, and descending goes with a key", "[Mcp][Scene]")
{
    const NativeCommand* order = NativeCommands::find("object_order");
    REQUIRE(order != nullptr);
    // Each passes the schema, and the command refuses it before it reads the app.
    const std::vector<std::pair<json, std::string>> refused{
        {json::object(), "Give by, or the objects in order"},
        {{{"by", "name"}, {"order", json::array({1, 0})}}, "Give by or order, not both"},
        {{{"order", json::array({1, 0})}, {"all", true}}, "order names the objects of one plate: leave all out"},
        {{{"order", json::array({1, 0})}, {"descending", true}}, "descending goes with by: order gives the objects in their order"},
        {{{"by", "name"}, {"all", true}, {"plate", 0}}, "Give plate or all, not both"}};
    for (const auto& [args, message] : refused) {
        INFO(args.dump());
        json given = args;
        CHECK(Mcp::argument_error(*order, given).empty());
        NoUiContext ctx;
        CHECK_THROWS_WITH(order->run(args, ctx), message);
    }
    // An order names at least one object.
    json empty{{"order", json::array()}};
    CHECK_THAT(Mcp::argument_error(*order, empty), ContainsSubstring("order"));
}

TEST_CASE("Import takes entries with one ZIP archive, and an export a file per object in STL or DRC alone", "[Mcp][Scene]")
{
    const NativeCommand* import_ = NativeCommands::find("project_import");
    const NativeCommand* export_ = NativeCommands::find("project_export");
    REQUIRE(import_ != nullptr);
    REQUIRE(export_ != nullptr);
    // Each passes the schema, and the command refuses it before it reads the app or the files.
    const json one_cube = json::array({"cube.stl"});
    const std::vector<std::tuple<const NativeCommand*, json, std::string>> refused{
        {import_, {{"paths", json::array({"/models/cube.stl"})}, {"entries", one_cube}},
         "entries names files of a .zip archive: give the archive in paths"},
        {import_, {{"paths", json::array({"/models/a.zip", "/models/b.zip"})}, {"entries", one_cube}},
         "entries names the files of one archive: give one .zip in paths"},
        {export_, {{"format", "gcode"}, {"path", "/out/plate.gcode"}, {"per_object", true}}, "per_object goes with format stl or drc"},
        {export_, {{"format", "toolpaths_obj"}, {"path", "/out/toolpaths.obj"}, {"per_object", true}},
         "per_object goes with format stl or drc"},
        {export_, {{"format", "toolpaths_obj"}, {"path", "/out/toolpaths.obj"}, {"plates", 1}},
         "toolpaths_obj writes the toolpaths the Preview shows: plates goes with the other formats"},
        {export_, {{"format", "drc"}, {"path", "/out/objects"}, {"per_object", true}, {"plates", 1}},
         "per_object writes every object into the folder path names: plates and overwrite go with one file"},
        {export_, {{"format", "stl"}, {"path", "/out/objects"}, {"per_object", true}, {"overwrite", true}},
         "per_object writes every object into the folder path names: plates and overwrite go with one file"}};
    for (const auto& [command, args, message] : refused) {
        INFO(command->key << " " << args.dump());
        json given = args;
        CHECK(Mcp::argument_error(*command, given).empty());
        NoUiContext ctx;
        CHECK_THROWS_WITH(command->run(args, ctx), message);
    }
    // entries names one file or more; an empty list is refused by the schema.
    json no_entries{{"paths", json::array({"/models/a.zip"})}, {"entries", json::array()}};
    CHECK(Mcp::argument_error(*import_, no_entries).find("entries must hold at least 1 item") != std::string::npos);
}

TEST_CASE("view_set takes the display switches, the sidebar, and the Preview's legend fold and G-code window", "[Mcp][Scene]")
{
    const NativeCommand* view = NativeCommands::find("view_set");
    REQUIRE(view != nullptr);
    const json& properties = view->input_schema.at("properties");
    for (const char* name : {"sidebar_collapsed", "legend_collapsed", "gcode_window", "labels", "overhangs", "outline", "wireframe",
                             "gridlines", "axes", "navigator"}) {
        INFO(name);
        REQUIRE(properties.contains(name));
        CHECK(properties.at(name).at("type") == "boolean");
    }
    // The sidebar's width is pixels, or "default".
    REQUIRE(properties.contains("sidebar_width"));
    for (const json& width : {json(420), json("default")}) {
        json args{{"sidebar_width", width}};
        CHECK(Mcp::argument_error(*view, args).empty());
    }
    for (const json& width : {json(0), json(true), json(12.5)}) {
        INFO(width.dump());
        json args{{"sidebar_width", width}};
        CHECK_THAT(Mcp::argument_error(*view, args), ContainsSubstring("sidebar_width"));
    }
    // Outside the Design tab the section view is Prepare's and the Preview's, which takes on alone.
    json section{{"section", {{"on", true}}}, {"labels", true}};
    CHECK(Mcp::argument_error(*view, section).empty());

    // Each passes the schema, and the command refuses it before it changes anything: a width that is no number of pixels
    // and the Design view's cut without the Design tab, also beside a switch the call would set.
    const std::vector<std::pair<json, std::string>> refused{
        {{{"sidebar_width", "wide"}, {"labels", true}}, "sidebar_width is a width in pixels or \"default\""},
        {{{"section", {{"on", true}, {"z", 5}}}}, "z and keep are the Design view's cut"},
        {{{"tab", "prepare"}, {"section", {{"on", true}, {"keep", "upper"}}}, {"axes", false}}, "z and keep are the Design view's cut"}};
    for (const auto& [args, message] : refused) {
        INFO(args.dump());
        json given = args;
        CHECK(Mcp::argument_error(*view, given).empty());
        NoUiContext ctx;
        CHECK_THROWS_WITH(view->run(args, ctx), ContainsSubstring(message));
    }
}

TEST_CASE("a plate's origin and size are its rectangle on the bed, in bed coordinates", "[Mcp][Scene]")
{
    // The second plate of a row of 256 x 256 mm plates: the plate list moves the bed shape by the plate's position, which
    // is neither the origin the plate was made with nor the box that holds its grabber in front.
    PartPlate plate(nullptr, Vec3d(1000., 1000., 0.), 256, 256, 250., nullptr, nullptr);
    plate.set_shape({{0., 0.}, {256., 0.}, {256., 256.}, {0., 256.}}, {}, {}, {}, Vec2d(307.2, 0.), 0.f, 0.f);
    const json place = Mcp::plate_place(plate);
    REQUIRE(place.at("origin").size() == 2);
    REQUIRE(place.at("size").size() == 2);
    CHECK_THAT(place["origin"][0].get<double>(), WithinAbs(307.2, 1e-9));
    CHECK_THAT(place["origin"][1].get<double>(), WithinAbs(0., 1e-9));
    CHECK_THAT(place["size"][0].get<double>(), WithinAbs(256., 1e-9));
    CHECK_THAT(place["size"][1].get<double>(), WithinAbs(256., 1e-9));
    // A cube placed at the origin plus half the size stands in the middle of the plate.
    const BoundingBoxf3& box = plate.get_bounding_box();
    CHECK_THAT(place["origin"][0].get<double>() + place["size"][0].get<double>() / 2., WithinAbs(box.center().x(), 1e-9));
    CHECK_THAT(place["origin"][1].get<double>() + place["size"][1].get<double>() / 2., WithinAbs(box.center().y(), 1e-9));
}

TEST_CASE("Clone makes at least one copy, and Replace takes one part and the file it is replaced with", "[Mcp][Scene]")
{
    const auto command = [](const char* key) {
        const NativeCommand* found = NativeCommands::find(key);
        REQUIRE(found != nullptr);
        return found;
    };
    // A count written as a string of digits is taken as that number, as every integer argument is.
    for (json count : {json(0), json(-2), json(1.5), json("three")}) {
        INFO(count.dump());
        json args{{"objects", json::array({0})}, {"count", count}};
        CHECK_THAT(Mcp::argument_error(*command("object_clone"), args), ContainsSubstring("count"));
    }
    json three{{"objects", json::array({0})}, {"count", 3}, {"arrange", false}};
    CHECK(Mcp::argument_error(*command("object_clone"), three).empty());
    // With no count, one copy, as the Clone dialog's field starts.
    json selection = json::object();
    CHECK(Mcp::argument_error(*command("object_clone"), selection).empty());

    json no_path{{"object", 0}, {"part", 0}};
    CHECK_THAT(Mcp::argument_error(*command("part_replace"), no_path), ContainsSubstring("path"));
    json several{{"parts", json::array({json::array({0, 0})})}, {"path", "/models/cube.stl"}};
    CHECK_THAT(Mcp::argument_error(*command("part_replace"), several), ContainsSubstring("parts"));
    json one{{"object", 0}, {"part", 1}, {"path", "/models/cube.stl"}};
    CHECK(Mcp::argument_error(*command("part_replace"), one).empty());

    // A reload takes every object with all, which no other target goes with.
    json all{{"all", true}};
    CHECK(Mcp::argument_error(*command("object_reload_from_disk"), all).empty());
}

namespace {

// Three objects of one cube each: "alone" on plate 0, "twice" with an instance on plate 0 and one on
// plate 1, and "off" on no plate.
struct PlatedModel
{
    Model                         model;
    std::vector<std::vector<int>> on{{0}, {0, 1}, {-1}};
    std::vector<std::string>      plate_names{"", "second", "spare"};

    PlatedModel()
    {
        for (const char* name : {"alone", "twice", "off"}) {
            ModelObject* object = model.add_object();
            object->name        = name;
            object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false);
            for (std::size_t i = 0; i < on[model.objects.size() - 1].size(); ++i)
                object->add_instance();
        }
    }

    json list(const json& args = json::object()) const
    {
        return Mcp::objects_by_plate(model, plate_names, [this](std::size_t object, std::size_t instance) { return on[object][instance]; },
                                     args);
    }
};

} // namespace

TEST_CASE("object_list lists each plate's objects, then those on no plate", "[Mcp][Scene]")
{
    const PlatedModel scene;
    const json        out = scene.list();
    REQUIRE(out["plates"].size() == 3);
    const json& first = out["plates"][0];
    CHECK(first["index"] == 0);
    CHECK(first["name"] == "");
    REQUIRE(first["objects"].size() == 2);
    // Without detail an object is its names, counts and size: a 10 mm cube of one part.
    const json& alone = first["objects"][0];
    CHECK(alone["name"] == "alone");
    CHECK(alone["part_count"] == 1);
    REQUIRE(alone["size"].size() == 3);
    for (const json& side : alone["size"])
        CHECK_THAT(side.get<double>(), WithinAbs(10., 1e-6));
    CHECK_FALSE(alone.contains("parts"));
    CHECK_FALSE(alone.contains("bbox"));
    // An object keeps its index and id in the project, and lists the instances on this plate only, by index, with
    // their ids in the same order.
    const json& twice = first["objects"][1];
    CHECK(twice["index"] == 1);
    CHECK(twice["id"] == scene.model.objects[1]->id().id);
    CHECK(twice["instances"] == json::array({0}));
    CHECK(twice["instance_ids"] == json::array({scene.model.objects[1]->instances[0]->id().id}));

    const json& second = out["plates"][1];
    CHECK(second["name"] == "second");
    REQUIRE(second["objects"].size() == 1);
    CHECK(second["objects"][0]["index"] == 1);
    CHECK(second["objects"][0]["instances"] == json::array({1}));
    CHECK(second["objects"][0]["instance_ids"] == json::array({scene.model.objects[1]->instances[1]->id().id}));

    // A plate with nothing on it is listed.
    CHECK(out["plates"][2]["objects"].empty());
    REQUIRE(out["outside"].size() == 1);
    CHECK(out["outside"][0]["name"] == "off");
    // The object on two plates is listed under both and counted once.
    CHECK(out["total"] == 3);
    CHECK_FALSE(out.contains("next_offset"));
}

TEST_CASE("object_list with detail gives each instance's and part's entry", "[Mcp][Scene]")
{
    const PlatedModel scene;
    const json        out = scene.list({{"detail", true}});
    const json&       first = out["plates"][0];
    REQUIRE(first["objects"].size() == 2);
    CHECK(first["objects"][0]["parts"].size() == 1);
    CHECK(first["objects"][0].contains("bbox"));
    CHECK(first["objects"][0].contains("mesh_errors"));
    // The entries of the instances on this plate only, without their plate, which the listing gives.
    const json& twice = first["objects"][1];
    REQUIRE(twice["instances"].size() == 1);
    CHECK(twice["instances"][0]["index"] == 0);
    CHECK(twice["instances"][0]["id"] == scene.model.objects[1]->instances[0]->id().id);
    CHECK_FALSE(twice["instances"][0].contains("plate"));
    // The ids are in the entries, so no list of them is beside.
    CHECK_FALSE(twice.contains("instance_ids"));
    const json& second = out["plates"][1]["objects"][0];
    REQUIRE(second["instances"].size() == 1);
    CHECK(second["instances"][0]["index"] == 1);
}

TEST_CASE("object_list with a plate lists that plate alone", "[Mcp][Scene]")
{
    const PlatedModel scene;
    const json        out = scene.list({{"plate", 1}});
    REQUIRE(out["plates"].size() == 1);
    CHECK(out["plates"][0]["index"] == 1);
    REQUIRE(out["plates"][0]["objects"].size() == 1);
    CHECK(out["plates"][0]["objects"][0]["name"] == "twice");
    CHECK_FALSE(out.contains("outside"));
    CHECK(out["total"] == 1);

    CHECK_THROWS_WITH(scene.list({{"plate", 3}}), ContainsSubstring("Plate 3 does not exist (the project has plates 0..2)"));
}

TEST_CASE("object_list pages the objects, listing each under every plate it is on", "[Mcp][Scene]")
{
    const PlatedModel scene;
    // The second of the three objects is the one on two plates.
    json out = scene.list({{"offset", 1}, {"limit", 1}});
    CHECK(out["total"] == 3);
    CHECK(out["offset"] == 1);
    CHECK(out["next_offset"] == 2);
    REQUIRE(out["plates"].size() == 3);
    REQUIRE(out["plates"][0]["objects"].size() == 1);
    CHECK(out["plates"][0]["objects"][0]["name"] == "twice");
    REQUIRE(out["plates"][1]["objects"].size() == 1);
    CHECK(out["plates"][1]["objects"][0]["name"] == "twice");
    CHECK(out["outside"].empty());

    out = scene.list({{"offset", 2}, {"limit", 1}});
    CHECK(out["plates"][0]["objects"].empty());
    CHECK(out["plates"][1]["objects"].empty());
    REQUIRE(out["outside"].size() == 1);
    CHECK(out["outside"][0]["name"] == "off");
    CHECK_FALSE(out.contains("next_offset"));
}

TEST_CASE("object_list with a plate pages and counts that plate's objects", "[Mcp][Scene]")
{
    const PlatedModel scene;
    const json        out = scene.list({{"plate", 0}, {"offset", 1}, {"limit", 1}});
    CHECK(out["total"] == 2);
    CHECK(out["offset"] == 1);
    CHECK_FALSE(out.contains("next_offset"));
    REQUIRE(out["plates"][0]["objects"].size() == 1);
    CHECK(out["plates"][0]["objects"][0]["name"] == "twice");
    CHECK(out["plates"][0]["objects"][0]["instances"] == json::array({0}));
}

TEST_CASE("part types round-trip between names and ModelVolumeType", "[Mcp][Scene]")
{
    for (const char* name : {"part", "negative", "modifier", "support_blocker", "support_enforcer", "precise_seam"})
        CHECK(std::string(Mcp::volume_type_name(Mcp::volume_type_from_name(name))) == name);
    // The context menu's Add Precise Seam adds a Center seam; every subtype reads back as precise_seam.
    CHECK(Mcp::volume_type_from_name("precise_seam") == ModelVolumeType::PRECISE_SEAM_CENTER);
    for (const ModelVolumeType seam : {ModelVolumeType::PRECISE_SEAM_CENTER, ModelVolumeType::PRECISE_SEAM_LEFT, ModelVolumeType::PRECISE_SEAM_RIGHT,
                                       ModelVolumeType::PRECISE_SEAM_ENFORCED, ModelVolumeType::PRECISE_SEAM_BLOCKED,
                                       ModelVolumeType::PRECISE_SEAM_NEUTRAL})
        CHECK(std::string(Mcp::volume_type_name(seam)) == "precise_seam");
    CHECK(Mcp::volume_type_from_name("other") == ModelVolumeType::INVALID);
    CHECK(Mcp::volume_type_from_name("Part") == ModelVolumeType::INVALID);
}

TEST_CASE("a shape's size needs one positive number per field of the size dialog", "[Mcp][Scene]")
{
    CHECK(primitive_size_names("Cube").size() == 3);
    CHECK(primitive_size_names("Cylinder").size() == 2);
    CHECK(primitive_size_names("Cone").size() == 2);
    CHECK(primitive_size_names("Disc").size() == 2);
    CHECK(primitive_size_names("Sphere").size() == 1);
    CHECK(std::string(primitive_size_names("Torus").front()) == "Inner diameter");
    CHECK(primitive_size_names("Slab").empty());

    CHECK(Mcp::part_shape_size("cube", json::array({40, 30, 10})) == std::vector<double>{40., 30., 10.});
    CHECK(Mcp::part_shape_size("cylinder", json::array({6.2, 3.2})) == std::vector<double>{6.2, 3.2});
    CHECK(Mcp::part_shape_size("sphere", json::array({8})) == std::vector<double>{8.});
    CHECK(Mcp::part_shape_size("torus", json::array({20, 4})) == std::vector<double>{20., 4.});
    // No size: the built-in one.
    CHECK(Mcp::part_shape_size("disc", json()).empty());

    CHECK_THROWS_AS(Mcp::part_shape_size("cube", json::array({40, 30})), ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("sphere", json::array({8, 8})), ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("cylinder", json::array({0, 3})), ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("cylinder", json::array({-6, 3})), ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("cylinder", json::array({std::nan(""), 3})), ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("cylinder", json::array({"6", 3})), ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("cylinder", json(6)), ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("slab", json()), ToolFailure);
    // The message names the fields, as the dialog does.
    try {
        Mcp::part_shape_size("torus", json::array({20}));
        FAIL("a torus with one size was accepted");
    } catch (const ToolFailure& e) {
        CHECK(std::string(e.what()) == "A torus's size is [Inner diameter, Tube diameter] in mm, each above 0");
    }
}

// The object list's context-menu items are actions of commands beside the other object and part commands.
TEST_CASE("the object structure commands sit beside the other object commands", "[Mcp][Scene]")
{
    const NativeCommand* objects = NativeCommands::find("object_delete");
    const NativeCommand* parts   = NativeCommands::find("part_add");
    REQUIRE(objects != nullptr);
    REQUIRE(parts != nullptr);
    for (const char* key : {"part_change_type", "object_rename", "object_set_instances", "object_assemble", "object_separate_instances",
                            "object_order"}) {
        INFO(key);
        const NativeCommand* c = NativeCommands::find(key);
        REQUIRE(c != nullptr);
        CHECK(c->group == (std::string(key).rfind("part_", 0) == 0 ? parts : objects)->group);
        CHECK_FALSE(c->description.empty());
        REQUIRE(c->actions.is_array());
        for (const json& action : c->actions)
            CHECK_FALSE(action.at("description").get<std::string>().empty());
    }
}

TEST_CASE("a split is into objects or parts, a subtype is a precise seam's, and an order is by name, filament, height or size", "[Mcp][Scene]")
{
    const auto command = [](const char* key) {
        const NativeCommand* found = NativeCommands::find(key);
        REQUIRE(found != nullptr);
        return found;
    };
    const auto values = [&command](const char* key, const char* arg) {
        std::set<std::string> out;
        for (const json& value : command(key)->input_schema.at("properties").at(arg).at("enum"))
            out.insert(value.get<std::string>());
        return out;
    };
    CHECK(values("object_split", "into") == std::set<std::string>{"objects", "parts"});
    for (const char* into : {"pieces", "object"}) {
        INFO(into);
        json args{{"object", 0}, {"into", into}};
        CHECK_THAT(Mcp::argument_error(*command("object_split"), args), ContainsSubstring("into"));
    }
    json split{{"object", 0}, {"into", "parts"}, {"keep_z", true}};
    CHECK(Mcp::argument_error(*command("object_split"), split).empty());

    CHECK(values("object_order", "by") == std::set<std::string>{"name", "filament", "height", "size"});

    // A subtype is one of a precise seam's, and with another type it is refused.
    CHECK(values("part_change_type", "subtype") == std::set<std::string>{"center", "left", "right", "enforced", "blocked", "neutral"});
    CHECK(Mcp::precise_seam_subtype("left") == ModelVolumeType::PRECISE_SEAM_LEFT);
    CHECK(Mcp::precise_seam_subtype("neutral") == ModelVolumeType::PRECISE_SEAM_NEUTRAL);
    CHECK(Mcp::precise_seam_subtype("part") == ModelVolumeType::INVALID);
    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false);
    object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER, false);
    object->add_instance();
    Ops::Targets modifier;
    modifier.parts = {{0, 1}};
    const std::optional<Ops::Refusal> refused =
        Ops::part_type_refusal(model, modifier, ModelVolumeType::NEGATIVE_VOLUME, ModelVolumeType::PRECISE_SEAM_LEFT);
    REQUIRE(refused);
    CHECK(std::string(refused->code) == "subtype_without_seam");
    CHECK_FALSE(Ops::part_type_refusal(model, modifier, ModelVolumeType::PRECISE_SEAM_CENTER, ModelVolumeType::PRECISE_SEAM_LEFT));
}

TEST_CASE("a mirror needs its axis, a centring takes a plate and no part, and a primitive its plate, position and name", "[Mcp][Scene]")
{
    const NativeCommand* mirror = NativeCommands::find("object_mirror");
    REQUIRE(mirror != nullptr);
    for (const json& axis : {json("x"), json("y"), json("z")}) {
        json args{{"objects", json::array({0})}, {"axis", axis}};
        CHECK(Mcp::argument_error(*mirror, args).empty());
    }
    json no_axis{{"objects", json::array({0})}};
    CHECK_THAT(Mcp::argument_error(*mirror, no_axis), ContainsSubstring("axis"));
    json bad_axis{{"axis", "w"}};
    CHECK_THAT(Mcp::argument_error(*mirror, bad_axis), ContainsSubstring("axis"));

    const NativeCommand* center = NativeCommands::find("object_center");
    REQUIRE(center != nullptr);
    json on_plate{{"objects", json::array({0, 2})}, {"plate", 1}};
    CHECK(Mcp::argument_error(*center, on_plate).empty());
    json parts{{"parts", json::array({json::array({0, 0})})}};
    CHECK_THAT(Mcp::argument_error(*center, parts), ContainsSubstring("parts"));

    const NativeCommand* primitive = NativeCommands::find("object_add_primitive");
    REQUIRE(primitive != nullptr);
    json placed{{"shape", "cube"}, {"size", json::array({20, 20, 20})}, {"plate", 1}, {"position", json::array({100, 100, 0})}, {"name", "block"}};
    CHECK(Mcp::argument_error(*primitive, placed).empty());
    // A call with more than an action's shape is the silent form, which asks for no size.
    CHECK_FALSE(Mcp::person_form(*primitive, placed));
    // Its size is checked as part_add's is: the size dialog's fields, each above 0.
    const std::vector<double> cylinder = Mcp::part_shape_size("cylinder", json::array({30, 10}));
    REQUIRE(cylinder.size() == 2);
    CHECK_THAT(cylinder[0], WithinAbs(30., 1e-9));
    CHECK_THAT(cylinder[1], WithinAbs(10., 1e-9));
    CHECK(Mcp::part_shape_size("cube", json()).empty());
    CHECK_THROWS_WITH(Mcp::part_shape_size("cube", json::array({20, 20})), ContainsSubstring("[X, Y, Z]"));
    CHECK_THROWS_AS(Mcp::part_shape_size("sphere", json::array({0})), ToolFailure);
    // Its operation names the object, so it reaches the rename's as well.
    CHECK(primitive->operations == std::vector<std::string>{"add_primitive", "rename_object"});
}

namespace {

// Three objects of cubes, named for what they have: "first" with one instance and one part, "second" with two
// instances and two parts, "third" with one of each.
struct TargetModel
{
    Model model;

    TargetModel()
    {
        for (const auto& [name, instances, parts] : std::vector<std::tuple<std::string, int, int>>{{"first", 1, 1}, {"second", 2, 2}, {"third", 1, 1}}) {
            ModelObject* object = model.add_object();
            object->name        = name;
            for (int p = 0; p < parts; ++p)
                object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false)->name = name + " part " + std::to_string(p);
            for (int i = 0; i < instances; ++i)
                object->add_instance();
        }
    }
};

constexpr unsigned kEveryKind = Mcp::kObjects | Mcp::kInstances | Mcp::kParts;

// The targets a call's `args` name, with no selection to fall back on.
Ops::Outcome<Ops::Targets> named_targets(const Model& model, const json& args, unsigned kinds, bool one = false)
{
    return Mcp::resolve_targets(model, nullptr, args, kinds, one);
}

json pairs(std::initializer_list<std::pair<int, int>> list)
{
    json out = json::array();
    for (const auto& [a, b] : list)
        out.push_back(json::array({a, b}));
    return out;
}

} // namespace

TEST_CASE("a command's targets are what its arguments name, else the selection", "[Mcp][Scene]")
{
    const TargetModel m;
    const Model&      model = m.model;

    const auto named = named_targets(model, {{"objects", json::array({0, 2})}}, Mcp::kObjects);
    REQUIRE(named);
    CHECK(named.value().objects == std::vector<int>{0, 2});
    CHECK(named.value().instances.empty());
    CHECK(named.value().parts.empty());
    CHECK_FALSE(named.value().from_selection);

    // One target, named in the singular: an instance of the second object, then one of its parts by index and id.
    const auto instance = named_targets(model, {{"object", 1}, {"instance", 1}}, kEveryKind, true);
    REQUIRE(instance);
    CHECK(instance.value().instances == std::vector<std::pair<int, int>>{{1, 1}});
    const auto part = named_targets(model, {{"object", 1}, {"part", 1}, {"part_id", model.objects[1]->volumes[1]->id().id}}, kEveryKind, true);
    REQUIRE(part);
    CHECK(part.value().parts == std::vector<std::pair<int, int>>{{1, 1}});

    // Every index and every expected id is checked before anything is returned, and a miss is named with the indices
    // there are.
    CHECK_THROWS_WITH(named_targets(model, {{"objects", json::array({0, 3})}}, Mcp::kObjects),
                      ContainsSubstring("object index 3 is out of range (the project has objects 0..2)"));
    const std::size_t other = model.objects[1]->id().id;
    CHECK_THROWS_WITH(named_targets(model, {{"objects", json::array({0})}, {"object_ids", json::array({other})}}, Mcp::kObjects),
                      ContainsSubstring(std::to_string(other)));
    CHECK_THROWS_WITH(named_targets(model, {{"parts", pairs({{1, 5}})}}, Mcp::kParts),
                      ContainsSubstring("object 1 has no part 5 (it has parts 0..1)"));
    CHECK_THROWS_WITH(named_targets(model, {{"instances", pairs({{0, 1}})}}, Mcp::kInstances),
                      ContainsSubstring("object 0 has no instance 1 (it has instance 0 only)"));
    // Both forms of one target at once.
    CHECK_THROWS_AS(named_targets(model, {{"object", 1}, {"objects", json::array({0})}}, Mcp::kObjects), ToolFailure);
    // An index wider than an int is out of range as given, not narrowed to another index (4294967296 to 0).
    CHECK_THROWS_WITH(named_targets(model, {{"objects", json::array({std::int64_t(4294967296)})}}, Mcp::kObjects),
                      ContainsSubstring("object index 4294967296 is out of range"));
    CHECK_THROWS_WITH(named_targets(model, {{"object", 1}, {"part", std::int64_t(4294967296)}}, kEveryKind, true),
                      ContainsSubstring("object 1 has no part 4294967296"));
    CHECK_THROWS_WITH(named_targets(model, {{"instances", json::array({json::array({std::int64_t(4294967296), 0})})}}, Mcp::kInstances),
                      ContainsSubstring("object index 4294967296 is out of range"));
    // An unsigned index past int64 is named as given, not as the negative number a narrowing makes of it.
    CHECK_THROWS_WITH(named_targets(model, {{"objects", json::array({std::uint64_t(18446744073709551615ull)})}}, Mcp::kObjects),
                      ContainsSubstring("object index 18446744073709551615 is out of range"));
    CHECK_THROWS_WITH(named_targets(model, {{"parts", json::array({json::array({1, std::uint64_t(18446744073709551615ull)})})}},
                                    Mcp::kParts),
                      ContainsSubstring("object 1 has no part 18446744073709551615"));
    // A whole number written as a float is an index, as the schema's integer type takes it, in any form.
    const auto as_floats = named_targets(model, {{"objects", json::array({0.0, 2.0})}}, Mcp::kObjects);
    REQUIRE(as_floats);
    CHECK(as_floats.value().objects == std::vector<int>{0, 2});
    const auto float_part = named_targets(model, {{"parts", json::array({json::array({1.0, 1.0})})}}, Mcp::kParts);
    REQUIRE(float_part);
    CHECK(float_part.value().parts == std::vector<std::pair<int, int>>{{1, 1}});
    const auto float_object = named_targets(model, {{"object", 1.0}, {"instance", 1.0}}, kEveryKind, true);
    REQUIRE(float_object);
    CHECK(float_object.value().instances == std::vector<std::pair<int, int>>{{1, 1}});
    // A float past int64, which the schema's integer type does not take either (Mcp::is_whole_number()), is no index.
    CHECK_THROWS_WITH(named_targets(model, {{"objects", json::array({9.3e18})}}, Mcp::kObjects),
                      ContainsSubstring("an entry of objects is an object index"));
    // Each is the call's to correct, by whichever route it came.
    for (const json& wrong : {json{{"objects", json::array({0, 3})}}, json{{"object", 1}, {"objects", json::array({0})}}}) {
        INFO(wrong.dump());
        try {
            named_targets(model, wrong, Mcp::kObjects);
            FAIL("the targets were not refused");
        } catch (const ToolFailure& f) {
            CHECK(f.details.value("code", std::string()) == "invalid_argument");
        }
    }

    // With none named, the targets are the selection, and a null selection holds nothing.
    const auto none = named_targets(model, json::object(), kEveryKind);
    REQUIRE_FALSE(none);
    CHECK(std::string(none.refusal().code) == "nothing_selected");
    // A command of one target refuses two.
    const auto two = named_targets(model, {{"objects", json::array({0, 2})}}, Mcp::kObjects, true);
    REQUIRE_FALSE(two);
    CHECK(std::string(two.refusal().code) == "one_object_only");

    // A command's schema has the forms of the kinds it takes, and no other.
    NativeCommand command;
    command.input_schema = Mcp::with_targets(Mcp::object_schema({}), Mcp::kObjects, false);
    json parts{{"parts", pairs({{1, 0}})}};
    CHECK_THAT(Mcp::argument_error(command, parts), ContainsSubstring("parts"));
    json objects{{"objects", json::array({1})}};
    CHECK(Mcp::argument_error(command, objects).empty());
    const auto names = [](const std::vector<Mcp::Property>& props) {
        std::set<std::string> out;
        for (const Mcp::Property& p : props)
            out.insert(p.first);
        return out;
    };
    CHECK(names(Mcp::target_props(kEveryKind, false)) ==
          std::set<std::string>{"objects", "object_ids", "instances", "instance_ids", "parts", "part_ids"});
    CHECK(names(Mcp::target_props(kEveryKind, true)) ==
          std::set<std::string>{"object", "object_id", "instance", "instance_id", "part", "part_id"});
}

TEST_CASE("a reply names what the command acted on", "[Mcp][Scene]")
{
    const TargetModel m;
    Ops::Targets      acted;
    acted.objects = {0};
    acted.parts   = {{1, 1}};
    const json reply = Mcp::targets_reply(m.model, acted);
    REQUIRE(reply.at("objects").size() == 1);
    CHECK(reply["objects"][0] == json{{"index", 0}, {"object_id", m.model.objects[0]->id().id}, {"name", "first"}});
    REQUIRE(reply.at("parts").size() == 1);
    CHECK(reply["parts"][0] ==
          json{{"object", 1}, {"part", 1}, {"part_id", m.model.objects[1]->volumes[1]->id().id}, {"name", "second part 1"}});
    // Only the kinds it holds.
    CHECK_FALSE(reply.contains("instances"));

    // An instance with its own id and its object's; one the object no longer has, a deleted one's index, without.
    Ops::Targets instances;
    instances.instances = {{1, 1}, {1, 5}};
    const json named    = Mcp::targets_reply(m.model, instances);
    REQUIRE(named.at("instances").size() == 2);
    CHECK(named["instances"][0] == json{{"object", 1},
                                        {"instance", 1},
                                        {"object_id", m.model.objects[1]->id().id},
                                        {"instance_id", m.model.objects[1]->instances[1]->id().id},
                                        {"name", "second"}});
    CHECK_FALSE(named["instances"][1].contains("instance_id"));
}

TEST_CASE("a command's targets are named by index or by the ids object_list reports", "[Mcp][Scene]")
{
    const TargetModel  m;
    const Model&       model  = m.model;
    const ModelObject& first  = *model.objects[0];
    const ModelObject& second = *model.objects[1];
    const ModelObject& third  = *model.objects[2];
    const auto         refusal = [&model](const json& args, unsigned kinds, bool one = false) {
        try {
            named_targets(model, args, kinds, one);
        } catch (const ToolFailure& f) {
            return std::make_pair(std::string(f.what()), f.details.value("code", std::string()));
        }
        return std::make_pair(std::string("not refused"), std::string());
    };

    // An object's id names it alone, and a list of ids their objects, in the order given.
    const auto by_id = named_targets(model, {{"object_id", second.id().id}}, kEveryKind, true);
    REQUIRE(by_id);
    CHECK(by_id.value().objects == std::vector<int>{1});
    const auto listed = named_targets(model, {{"object_ids", json::array({third.id().id, first.id().id})}}, Mcp::kObjects);
    REQUIRE(listed);
    CHECK(listed.value().objects == std::vector<int>{2, 0});
    // A part's or an instance's id names its object too.
    const auto part = named_targets(model, {{"part_id", second.volumes[1]->id().id}}, kEveryKind, true);
    REQUIRE(part);
    CHECK(part.value().parts == std::vector<std::pair<int, int>>{{1, 1}});
    const auto instance = named_targets(model, {{"instance_id", second.instances[1]->id().id}}, kEveryKind, true);
    REQUIRE(instance);
    CHECK(instance.value().instances == std::vector<std::pair<int, int>>{{1, 1}});
    const auto parts = named_targets(model, {{"part_ids", json::array({second.volumes[0]->id().id, first.volumes[0]->id().id})}},
                                     Mcp::kParts);
    REQUIRE(parts);
    CHECK(parts.value().parts == std::vector<std::pair<int, int>>{{1, 0}, {0, 0}});
    const auto instances = named_targets(model, {{"instance_ids", json::array({second.instances[0]->id().id})}}, Mcp::kInstances);
    REQUIRE(instances);
    CHECK(instances.value().instances == std::vector<std::pair<int, int>>{{1, 0}});
    // The object by id and the part by index.
    const auto mixed = named_targets(model, {{"object_id", second.id().id}, {"part", 1}}, kEveryKind, true);
    REQUIRE(mixed);
    CHECK(mixed.value().parts == std::vector<std::pair<int, int>>{{1, 1}});

    // Given both, the id guards the index: instance 0 of the second object is not the instance with that id.
    CHECK_THAT(refusal({{"object", 1}, {"instance", 0}, {"instance_id", second.instances[1]->id().id}}, kEveryKind, true).first,
               ContainsSubstring("not the expected id"));
    CHECK_THAT(refusal({{"instances", json::array({json::array({1, 0})})}, {"instance_ids", json::array({second.instances[1]->id().id})}},
                       Mcp::kInstances)
                   .first,
               ContainsSubstring("not the expected id"));
    // A part's id of another object than the call names.
    CHECK(refusal({{"object", 0}, {"part_id", second.volumes[1]->id().id}}, kEveryKind, true) ==
          std::make_pair("part_id " + std::to_string(second.volumes[1]->id().id) + " is of object 1, not of object 0",
                         std::string("invalid_argument")));
    // An id the model has nothing with, as a project opened again gives every object a new one.
    const std::size_t none = third.instances[0]->id().id + 100000;
    for (const auto& [args, kinds, text] : std::vector<std::tuple<json, unsigned, std::string>>{
             {json{{"object_id", none}}, kEveryKind, "No object has id "},
             {json{{"part_ids", json::array({none})}}, Mcp::kParts, "No part has id "},
             {json{{"instance_ids", json::array({none})}}, Mcp::kInstances, "No instance has id "}}) {
        INFO(args.dump());
        const auto [what, code] = refusal(args, kinds, args.contains("object_id"));
        CHECK(what.rfind(text + std::to_string(none), 0) == 0);
        CHECK(code == "invalid_argument");
    }
    // An id given as an index is refused with what that id names.
    CHECK_THAT(refusal({{"object", second.id().id}}, kEveryKind, true).first,
               ContainsSubstring(std::to_string(second.id().id) + " is the id of object 1"));

    // The ids stay beside the indices they are read as, so the command still checks them where it acts.
    CHECK(Mcp::with_indices(model, {{"object_id", third.id().id}}) == json{{"object", 2}, {"object_id", third.id().id}});
    CHECK(Mcp::with_indices(model, {{"part_id", second.volumes[1]->id().id}}) ==
          json{{"object", 1}, {"part", 1}, {"part_id", second.volumes[1]->id().id}});
    // The objects outside the targets: Fill bed's, an order's and a Design commit's replaced ones.
    CHECK(Mcp::with_indices(model, {{"fill_object_id", second.id().id}})["fill_object"] == 1);
    CHECK(Mcp::with_indices(model, {{"order_ids", json::array({third.id().id, first.id().id, second.id().id})}})["order"] ==
          json::array({2, 0, 1}));
    CHECK(Mcp::with_indices(model, {{"replace_ids", json::array({first.id().id})}})["replace"] == json::array({0}));
    // Given with their indices, those ids are checked there, as no reading of the targets reads them.
    CHECK_NOTHROW(Mcp::with_indices(model, {{"order", json::array({1, 0})}, {"order_ids", json::array({second.id().id, first.id().id})}}));
    CHECK_THROWS_WITH(Mcp::with_indices(model, {{"replace", json::array({1})}, {"replace_ids", json::array({first.id().id})}}),
                      ContainsSubstring("not the expected id"));
    CHECK_THROWS_WITH(Mcp::with_indices(model, {{"order", json::array({1, 0})}, {"order_ids", json::array({first.id().id})}}),
                      ContainsSubstring("order_ids takes one id per entry of order"));
}

TEST_CASE("a command's target is optional, and a plate defaults to the current one", "[Mcp][Scene]")
{
    const auto required = [](const json& schema) {
        std::set<std::string> out;
        for (const json* list : {&schema, schema.contains("then") ? &schema.at("then") : nullptr})
            if (list != nullptr)
                for (const json& name : list->value("required", json::array()))
                    out.insert(name.get<std::string>());
        return out;
    };
    // With no object, the object selected in the Prepare view.
    for (const char* key : {"object_set_variable_layer_height", "object_set_height_ranges", "object_transform", "part_add"}) {
        INFO(key);
        const NativeCommand* command = NativeCommands::find(key);
        REQUIRE(command != nullptr);
        CHECK(command->input_schema.at("properties").contains("object"));
        CHECK(required(command->input_schema).count("object") == 0);
    }
    // With no plate, the current plate.
    for (const NativeCommand& command : NativeCommands::instance().commands()) {
        INFO(command.key);
        CHECK(required(command.input_schema).count("plate") == 0);
    }
}

namespace {

// What the object list gives a new range: the object's layer height and its filament ("Default").
DynamicPrintConfig new_range_config(double layer_height = 0.2)
{
    DynamicPrintConfig config;
    config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    config.set_key_value("extruder", new ConfigOptionInt(0));
    return config;
}

// object_set_height_ranges' parse of `ranges` for an object that has `current`.
t_layer_config_ranges parse_ranges(const json& ranges, const t_layer_config_ranges& current = {})
{
    return Mcp::layer_config_ranges_from_json(ranges, current, new_range_config());
}

json range(double min_z, double max_z)
{
    return {{"min_z", min_z}, {"max_z", max_z}};
}

} // namespace

TEST_CASE("height ranges are checked before the object changes", "[Mcp][Scene][Layers]")
{
    // Sorted by height; touching ranges are fine; each new one starts from the new-range config.
    const t_layer_config_ranges ranges = parse_ranges(json::array({range(10, 20), range(0, 10)}));
    REQUIRE(ranges.size() == 2);
    const auto& [low, low_config]   = *ranges.begin();
    const auto& [high, high_config] = *std::next(ranges.begin());
    CHECK(low == t_layer_height_range(0., 10.));
    CHECK(high == t_layer_height_range(10., 20.));
    CHECK_THAT(low_config.opt_float("layer_height"), WithinAbs(0.2, 1e-9));
    CHECK(low_config.opt_int("extruder") == 0);
    CHECK(high_config.get() == new_range_config());
    CHECK(parse_ranges(json::array()).empty());

    // An overlap names both ranges, whatever order they come in.
    CHECK_THROWS_WITH(parse_ranges(json::array({range(12, 20), range(0, 15)})), ContainsSubstring("0-15 and 12-20 overlap"));
    CHECK_THROWS_WITH(parse_ranges(json::array({range(0, 15), range(0, 15)})), ContainsSubstring("overlap"));
    CHECK_THROWS_WITH(parse_ranges(json::array({range(0, 20), range(5, 10)})), ContainsSubstring("0-20 and 5-10 overlap"));
    // Empty, reversed, below the bed or without a top.
    CHECK_THROWS_WITH(parse_ranges(json::array({range(5, 5)})), ContainsSubstring("0 <= min_z < max_z"));
    CHECK_THROWS_AS(parse_ranges(json::array({range(6, 5)})), ToolFailure);
    CHECK_THROWS_AS(parse_ranges(json::array({range(-1, 5)})), ToolFailure);
    CHECK_THROWS_AS(parse_ranges(json::array({json{{"min_z", 0}}})), ToolFailure);
}

TEST_CASE("a range that stays keeps its settings, and a new one starts from the object's layer height", "[Mcp][Scene][Layers]")
{
    // A range is min_z and max_z; its settings are the settings tools' target height_range.
    const NativeCommand* set_ranges = NativeCommands::find("object_set_height_ranges");
    REQUIRE(set_ranges != nullptr);
    const json& item = set_ranges->input_schema.at("properties").at("ranges").at("items");
    CHECK(item.at("properties").contains("min_z"));
    CHECK(item.at("properties").contains("max_z"));
    CHECK_FALSE(item.at("properties").contains("values"));
    json with_values{{"object", 0}, {"ranges", json::array({{{"min_z", 0}, {"max_z", 5}, {"values", {{"wall_loops", 3}}}}})}};
    CHECK_THAT(Mcp::argument_error(*set_ranges, with_values), ContainsSubstring("unknown argument ranges[0].values"));

    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    DynamicPrintConfig fine = new_range_config(0.12);
    fine.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));
    fine.set_key_value("extruder", new ConfigOptionInt(2));
    object->layer_config_ranges[{0., 5.}].assign_config(std::move(fine));
    object->layer_config_ranges[{5., 10.}].assign_config(new_range_config(0.28));

    // [0, 5] stays; [5, 10] goes and [5, 12] comes, at the object's layer height (0.16 here).
    const t_layer_config_ranges after = Mcp::layer_config_ranges_from_json(json::array({range(0, 5), range(5, 12)}),
                                                                           object->layer_config_ranges, new_range_config(0.16));
    REQUIRE(after.size() == 2);
    const auto& [kept, kept_config]   = *after.begin();
    const auto& [added, added_config] = *std::next(after.begin());
    CHECK(kept == t_layer_height_range(0., 5.));
    CHECK(kept_config.get() == object->layer_config_ranges.at({0., 5.}).get());
    CHECK_THAT(kept_config.opt_float("layer_height"), WithinAbs(0.12, 1e-9));
    CHECK(kept_config.opt_serialize("sparse_infill_density") == "40%");
    CHECK(kept_config.opt_int("extruder") == 2);
    CHECK(added == t_layer_height_range(5., 12.));
    CHECK(added_config.get() == new_range_config(0.16));
}

TEST_CASE("a height range's layer height is checked against the printer's for the range's filament", "[Mcp][Scene][Layers]")
{
    int        asked  = -1;
    const auto bounds = [&asked](int extruder) {
        asked = extruder;
        return std::make_pair(0.08, 0.28);
    };
    DynamicPrintConfig range = new_range_config();
    range.set_key_value("extruder", new ConfigOptionInt(2));
    DynamicPrintConfig thick;
    thick.set_key_value("layer_height", new ConfigOptionFloat(0.4));
    CHECK_THROWS_WITH(Mcp::check_height_range_layer_height(thick, range, bounds),
                      ContainsSubstring("layer_height 0.4 is outside this printer's 0.08-0.28"));
    CHECK(asked == 2);
    // With the filament given beside it, that filament's.
    thick.set_key_value("extruder", new ConfigOptionInt(1));
    CHECK_THROWS_AS(Mcp::check_height_range_layer_height(thick, range, bounds), ToolFailure);
    CHECK(asked == 1);
    DynamicPrintConfig fine;
    fine.set_key_value("layer_height", new ConfigOptionFloat(0.12));
    CHECK_NOTHROW(Mcp::check_height_range_layer_height(fine, range, bounds));
    // A call that gives no layer height checks none: the range editor checks only what is typed.
    asked = -1;
    DynamicPrintConfig infill;
    infill.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));
    CHECK_NOTHROW(Mcp::check_height_range_layer_height(infill, new_range_config(0.4), bounds));
    CHECK(asked == -1);
}

TEST_CASE("range settings take the keys the range settings page offers", "[Mcp][Scene][Layers]")
{
    const auto has = [](const std::vector<std::string>& keys, const char* key) { return std::find(keys.begin(), keys.end(), key) != keys.end(); };
    const std::vector<std::string> two = Mcp::layer_range_option_keys(2);
    for (const char* key : {"layer_height", "extruder", "sparse_infill_density", "wall_loops", "sparse_infill_filament_id"})
        CHECK(has(two, key));
    // Object-only settings, filament settings and internal options are not a range's.
    for (const char* key : {"enable_support", "brim_type", "nozzle_temperature", "print_extruder_id"})
        CHECK_FALSE(has(two, key));
    // With one filament the object list offers no per-feature filament.
    CHECK_FALSE(has(Mcp::layer_range_option_keys(1), "sparse_infill_filament_id"));
    // What a height range takes as a target is these.
    CHECK(Mcp::target_options("height_range", 2) == two);
}

TEST_CASE("object_list reports an object's height ranges", "[Mcp][Scene][Layers]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(20., 20., 30.), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    CHECK_FALSE(Mcp::object_info(*object, 0).contains("layer_config_ranges"));

    DynamicPrintConfig open = new_range_config();
    open.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));
    object->layer_config_ranges[{15., 30.}].assign_config(std::move(open));
    object->layer_config_ranges[{0., 15.}].assign_config(new_range_config(0.12));
    const json info = Mcp::object_info(*object, 0);
    REQUIRE(info["layer_config_ranges"].size() == 2);
    const json& low = info["layer_config_ranges"][0];
    CHECK(low["min_z"] == 0.);
    CHECK(low["max_z"] == 15.);
    CHECK(low["values"] == json{{"extruder", "0"}, {"layer_height", "0.12"}});
    CHECK(info["layer_config_ranges"][1]["values"]["sparse_infill_density"] == "40%");
    // Ranges are not the object's own overrides.
    CHECK(info["overrides"].empty());

    // Its ranges given back, each its min_z and max_z, keep every range with its settings.
    json bounds = json::array();
    for (const json& listed : info["layer_config_ranges"])
        bounds.push_back(range(listed["min_z"].get<double>(), listed["max_z"].get<double>()));
    const t_layer_config_ranges again = parse_ranges(bounds, object->layer_config_ranges);
    REQUIRE(again.size() == object->layer_config_ranges.size());
    for (auto a = again.cbegin(), b = object->layer_config_ranges.cbegin(); a != again.cend(); ++a, ++b) {
        CHECK(a->first == b->first);
        CHECK(a->second.get() == b->second.get());
    }
}

TEST_CASE("object_list reports a variable layer height profile as the object list does", "[Mcp][Scene][Layers]")
{
    // [z0, h0, z1, h1, ...]: the object list marks no flat bottom-to-top pair as variable. Heights are
    // rounded to 0.001 mm.
    CHECK(Mcp::layer_height_profile_json({}).is_null());
    CHECK(Mcp::layer_height_profile_json({0., 0.2, 30., 0.2}).is_null());
    CHECK(Mcp::layer_height_profile_json({0., 0.2, 10., 0.1, 30., 0.2804}) == json{{"points", 3}, {"min_height", 0.1}, {"max_height", 0.28}});

    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_sphere(10.), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    object->ensure_on_bed();
    CHECK_FALSE(Mcp::object_info(*object, 0).contains("layer_height_profile"));

    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const SlicingParameters  params = PrintObject::slicing_parameters(config, *object, float(object->max_z()), Vec3d::Ones());
    const auto               within = [&params](const json& profile) {
        CHECK(profile["min_height"].get<double>() >= params.min_layer_height - 1e-3);
        CHECK(profile["max_height"].get<double>() <= params.max_layer_height + 1e-3);
    };

    // What Adaptive stores on a sphere: thin layers near its poles, thick ones at its equator.
    object->layer_height_profile.set(layer_height_profile_adaptive(params, *object, 0.5f));
    json profile = Mcp::object_info(*object, 0)["layer_height_profile"];
    CHECK(profile["points"].get<std::size_t>() == object->layer_height_profile.get().size() / 2);
    CHECK(profile["min_height"].get<double>() < profile["max_height"].get<double>());
    within(profile);

    // What Smooth stores on an object without a profile: the profile its height ranges give, smoothed,
    // so the ranges' heights bound it.
    object->layer_height_profile.clear();
    object->layer_config_ranges.clear();
    object->layer_config_ranges[{0., 10.}].assign_config(new_range_config(0.1));
    object->layer_config_ranges[{10., 20.}].assign_config(new_range_config(0.25));
    std::vector<double> from_ranges;
    PrintObject::update_layer_height_profile(*object, params, from_ranges);
    object->layer_height_profile.set(smooth_height_profile(from_ranges, params, HeightProfileSmoothingParams()));
    profile = Mcp::object_info(*object, 0)["layer_height_profile"];
    REQUIRE(profile.is_object());
    CHECK(profile["min_height"].get<double>() >= 0.1 - 1e-3);
    CHECK(profile["max_height"].get<double>() <= 0.25 + 1e-3);
    CHECK(profile["min_height"].get<double>() < profile["max_height"].get<double>());
    within(profile);
}

TEST_CASE("layer code types have the names plate_list reads", "[Mcp][Scene][Layers]")
{
    for (const auto& [name, type] : std::vector<std::pair<std::string, CustomGCode::Type>>{{"pause", CustomGCode::PausePrint},
                                                                                         {"custom", CustomGCode::Custom},
                                                                                         {"template", CustomGCode::Template},
                                                                                         {"change_filament", CustomGCode::ToolChange}}) {
        CHECK(Mcp::custom_gcode_type_name(type) == name);
        CHECK(Mcp::custom_gcode_type_from_name(name) == type);
    }
    // An old project's colour change is listed, but the slider adds none.
    CHECK(std::string(Mcp::custom_gcode_type_name(CustomGCode::ColorChange)) == "color_change");
    CHECK(Mcp::custom_gcode_type_from_name("color_change") == CustomGCode::Unknown);
    CHECK(Mcp::custom_gcode_type_from_name("Pause") == CustomGCode::Unknown);
}

TEST_CASE("layer G-codes are edited on the layer slider as its menus edit them", "[Mcp][Scene][Layers]")
{
    // Ten 0.2 mm layers and two filaments, as the Preview loads a plate whose objects print with filament 1.
    IMSlider slider(0, 9, 0, 9);
    slider.SetSliderValues({0.2, 0.4, 0.6, 0.8, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0});
    slider.SetExtruderColors({"#FFFFFF", "#000000"});
    slider.SetManipulationMode(CustomGCode::MultiAsSingle);
    // The plater's handler: the plate takes the slider's codes.
    Model      model;
    const auto hand_over = [&] {
        CHECK(slider.is_need_post_tick_event());
        // The handler invalidates the plate's slice on these types only.
        const CustomGCode::Type type = slider.get_post_tick_event_type();
        CHECK((type == CustomGCode::PausePrint || type == CustomGCode::Custom || type == CustomGCode::Template || type == CustomGCode::ToolChange));
        slider.reset_post_tick_event();
        model.plates_custom_gcodes[0] = slider.GetTicksValues();
        return model.plates_custom_gcodes[0].gcodes;
    };
    const auto edit = [&](const json& args, int filaments = 2, bool has_template = false, const std::string& no_change = {}) {
        return Mcp::edit_layer_gcode(slider, args, filaments, has_template, no_change);
    };
    const auto refused = [&](const json& args, const std::string& message, int filaments = 2, const std::string& no_change = {}) {
        const CustomGCode::Info before = slider.GetTicksValues();
        CHECK_THROWS_WITH(edit(args, filaments, false, no_change), ContainsSubstring(message));
        CHECK(slider.GetTicksValues() == before);
        CHECK_FALSE(slider.is_need_post_tick_event());
    };

    // A pause by layer number and custom G-code by a height between layers, which is on the layer whose top
    // is above it.
    REQUIRE(edit({{"add", {{{"layer", 3}, {"type", "pause"}}, {{"z", 0.9}, {"type", "custom"}, {"gcode", "M104 S215"}}}}}));
    std::vector<CustomGCode::Item> codes = hand_over();
    REQUIRE(codes.size() == 2);
    CHECK_THAT(codes[0].print_z, WithinAbs(0.6, 1e-9));
    CHECK(codes[0].type == CustomGCode::PausePrint);
    CHECK_THAT(codes[1].print_z, WithinAbs(1.0, 1e-9));
    CHECK(codes[1].type == CustomGCode::Custom);
    CHECK(codes[1].extra == "M104 S215");

    // The same codes again change nothing and hand nothing over.
    CHECK_FALSE(edit({{"add", {{{"z", 0.6}, {"type", "pause"}}, {{"layer", 5}, {"type", "custom"}, {"gcode", "M104 S215"}}}}}));
    CHECK_FALSE(slider.is_need_post_tick_event());

    // A filament change carries the filament's colour; another filament on that layer switches it, as
    // Change Filament on the tick does; new custom text replaces the old, as Edit Custom G-code does.
    REQUIRE(edit({{"add", {{{"layer", 7}, {"type", "change_filament"}, {"filament", 2}}}}}));
    codes = hand_over();
    REQUIRE(codes.size() == 3);
    CHECK(codes[2].type == CustomGCode::ToolChange);
    CHECK(codes[2].extruder == 2);
    CHECK(codes[2].color == "#000000");
    REQUIRE(edit({{"add", {{{"layer", 7}, {"type", "change_filament"}, {"filament", 1}}, {{"layer", 5}, {"type", "custom"}, {"gcode", "M106 S0"}}}}}));
    codes = hand_over();
    REQUIRE(codes.size() == 3);
    CHECK(codes[1].extra == "M106 S0");
    CHECK(codes[2].extruder == 1);
    CHECK(codes[2].color == "#FFFFFF");

    // The slider's own add turns any code on its layer into a filament change, so a code of another type
    // is refused before the slider is touched, unless the call removes it first.
    refused({{"add", {{{"layer", 3}, {"type", "change_filament"}, {"filament", 2}}}}}, "layer 3 has a pause code; remove it first (remove: [{layer: 3}])");
    refused({{"add", {{{"layer", 7}, {"type", "pause"}}}}}, "layer 7 has a change_filament code");
    REQUIRE(edit({{"remove", {{{"layer", 3}}}}, {"add", {{{"layer", 3}, {"type", "custom"}, {"gcode", "M117 Magnets"}}}}}));
    codes = hand_over();
    REQUIRE(codes.size() == 3);
    CHECK(codes[0].type == CustomGCode::Custom);
    CHECK(codes[0].extra == "M117 Magnets");

    // Every check passes before anything changes.
    refused({{"remove", {{{"layer", 9}}}}}, "No code on layer 9 (codes on layers 3, 5, 7)");
    refused({{"remove", {{{"layer", 3}}}}, {"add", {{{"layer", 4}, {"type", "pause"}}, {{"layer", 11}, {"type", "pause"}}}}}, "layer must be 1..10 (z 0.2..2)");
    refused({{"add", {{{"z", 2.1}, {"type", "pause"}}}}}, "z 2.1 is above the top layer (2 mm)");
    refused({{"add", {{{"layer", 4}, {"z", 0.8}, {"type", "pause"}}}}}, "Give layer or z for each code");
    refused({{"add", {{{"type", "pause"}}}}}, "Give layer or z for each code");
    refused({{"add", {{{"layer", 4}, {"type", "pause"}}, {{"z", 0.75}, {"type", "pause"}}}}}, "Two codes for layer 4");
    refused({{"add", {{{"layer", 4}, {"type", "pause"}, {"filament", 2}}}}}, "filament goes with type change_filament");
    refused({{"add", {{{"layer", 4}, {"type", "change_filament"}}}}}, "change_filament needs filament");
    refused({{"add", {{{"layer", 4}, {"type", "custom"}}}}}, "custom needs gcode");
    refused({{"add", {{{"layer", 4}, {"type", "custom"}, {"gcode", std::string(1024, 'G')}}}}}, "1 to 1023 bytes");
    refused({{"add", {{{"layer", 4}, {"type", "template"}}}}}, "template_custom_gcode is empty");
    refused({{"add", {{{"layer", 4}, {"type", "color_change"}}}}}, "type must be pause, custom, template or change_filament");
    refused({{"add", {{{"layer", 4}, {"type", "change_filament"}, {"filament", 3}}}}}, "filament must be 1..2");
    refused({{"add", {{{"layer", 4}, {"type", "change_filament"}, {"filament", 2}}}}}, "The filaments changed since the plate was sliced", 3);
    refused({{"add", {{{"layer", 4}, {"type", "change_filament"}, {"filament", 2}}}}}, "The plate takes no filament change: spiral vase", 2,
            "spiral vase (spiral_mode) is on");
    CHECK(edit({{"add", {{{"layer", 4}, {"type", "template"}}}}}, 2, true));
    codes = hand_over();
    REQUIRE(codes.size() == 4);
    CHECK(codes[1].type == CustomGCode::Template);

    // Clear removes every code, then the call's adds go on the empty plate.
    REQUIRE(edit({{"clear", true}, {"add", {{{"layer", 10}, {"type", "pause"}}}}}));
    codes = hand_over();
    REQUIRE(codes.size() == 1);
    CHECK_THAT(codes[0].print_z, WithinAbs(2.0, 1e-9));
    REQUIRE(edit({{"clear", true}}));
    CHECK(hand_over().empty());
    CHECK_FALSE(edit({{"clear", true}}));

    // On one filament the menus offer no filament change.
    slider.SetExtruderColors({"#FFFFFF"});
    refused({{"add", {{{"layer", 4}, {"type", "change_filament"}, {"filament", 1}}}}}, "change_filament needs two or more filaments", 1);

    // Why the occupant is checked: the slider's add switches a pause to a filament change. And without a
    // layer the menus' edits act on the active thumb.
    slider.SetExtruderColors({"#FFFFFF", "#000000"});
    REQUIRE(edit({{"add", {{{"layer", 2}, {"type", "pause"}}}}}));
    hand_over();
    CHECK(slider.add_code_as_tick(CustomGCode::ToolChange, 2, 1));
    CHECK(hand_over()[0].type == CustomGCode::ToolChange);
    CHECK_FALSE(slider.add_code_as_tick(CustomGCode::PausePrint, -1, 1));
    slider.SetHigherValue(6);
    CHECK(slider.add_code_as_tick(CustomGCode::PausePrint));
    codes = hand_over();
    REQUIRE(codes.size() == 2);
    CHECK_THAT(codes[1].print_z, WithinAbs(1.4, 1e-9));
}

// The gizmo commands, each with the place its name gives it.
const std::vector<std::pair<std::string, std::string>> kGizmoCommands{
    {"object_paint", "Objects"}, {"object_measure", "Objects"}, {"object_simplify", "Objects"}, {"part_boolean", "Parts"}, {"object_cut", "Objects"},
    {"object_lay_on_face", "Objects"}, {"object_set_brim_ears", "Objects"}};

TEST_CASE("the gizmo commands are listed under their places and take no silent", "[Mcp][Gizmos]")
{
    for (const auto& [key, place] : kGizmoCommands) {
        INFO(key);
        const NativeCommand* command = NativeCommands::find(key);
        REQUIRE(command != nullptr);
        CHECK(command->listed);
        CHECK(command->group == place);
        CHECK_FALSE(command->read_only);
        // Whether a call opens or acts is read from its arguments, never from silent.
        CHECK_FALSE(command->input_schema.at("properties").contains("silent"));
        CHECK(std::find(command->operations.begin(), command->operations.end(), "open_gizmo") != command->operations.end());
        // Each says how a client works the open gizmo, and shows two calls.
        CHECK_THAT(command->description, ContainsSubstring("widget_list {include_imgui: true}"));
        CHECK_THAT(command->description, ContainsSubstring("object_select {none: true}"));
        REQUIRE(command->examples.is_array());
        CHECK(command->examples.size() == 2);
    }
}

TEST_CASE("object_paint's types name the four painting gizmos", "[Mcp][Gizmos]")
{
    const NativeCommand* paint = NativeCommands::find("object_paint");
    REQUIRE(paint != nullptr);
    const json& type = paint->input_schema.at("properties").at("type");
    CHECK(type.at("enum") == json::array({"supports", "seam", "color", "fuzzy_skin"}));
    // The type names the gizmo a call opens, so it is required.
    CHECK(paint->input_schema.at("required") == json::array({"type"}));
}

TEST_CASE("a gizmo command's acting arguments are schema properties its description names", "[Mcp][Gizmos]")
{
    for (const auto& [key, place] : kGizmoCommands) {
        INFO(key);
        const NativeCommand* command = NativeCommands::find(key);
        REQUIRE(command != nullptr);
        const std::vector<std::string>& acting = Mcp::gizmo_acting_args(key);
        if (acting.empty()) {
            // A command that only opens says nothing of acting.
            CHECK(command->description.find("Acts from arguments:") == std::string::npos);
            continue;
        }
        // Each marked as code, as descriptions mark argument names.
        std::string names;
        for (const std::string& name : acting) {
            CHECK(command->input_schema.at("properties").contains(name));
            names += (names.empty() ? "`" : ", `") + name + "`";
        }
        CHECK_THAT(command->description, ContainsSubstring("Acts from arguments: " + names + "."));
    }
    // A command that is not a gizmo command has none.
    CHECK(Mcp::gizmo_acting_args("object_list").empty());
}

TEST_CASE("text and SVG take the object they become a part of and the part's type, and their actions stay as they were",
          "[Mcp][Gizmos]")
{
    for (const auto& [key, action_id] : std::vector<std::pair<std::string, std::string>>{{"object_add_text", "add_primitive_text"},
                                                                                         {"object_add_svg", "add_primitive_svg"}}) {
        INFO(key);
        const NativeCommand* command = NativeCommands::find(key);
        REQUIRE(command != nullptr);
        const json& properties = command->input_schema.at("properties");
        CHECK(properties.contains("object"));
        CHECK(properties.contains("object_id"));
        CHECK(properties.at("type").at("enum") == json::array({"part", "negative", "modifier"}));
        // An object's id is checked against its index, and left out of the journal.
        CHECK(std::find(command->id_args.begin(), command->id_args.end(), "object_id") != command->id_args.end());
        // The Speed Dial's action is the menu item as it was: it takes no arguments.
        const auto [owner, action] = Slic3r::GUI::find_command_action(NativeCommands::instance(), action_id);
        CHECK(owner == command);
        REQUIRE(action != nullptr);
        CHECK(action->at("args") == json::object());
    }
    CHECK(NativeCommands::find("object_add_svg")->input_schema.at("properties").contains("path"));
}

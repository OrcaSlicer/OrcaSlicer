// Scene tools: transform's size argument, the primitive size param, the notifications MCP
// operations raise before the 3D view has drawn, arrange's options, the admission that holds a
// UI worker job a call starts, how list_objects describes objects, instances and parts,
// add_part's type names and shape sizes, the params of the object structure commands, the height
// ranges set_layer_ranges takes and list_objects reports, and set_layer_gcode's edits of the layer slider.
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

#include <imgui/imgui.h>
#include <nlohmann/json.hpp>

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Mcp/JobRegistry.hpp"
#include "slic3r/GUI/Mcp/McpCore.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/GUI/Mcp/UiInvoker.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/NotificationManager.hpp"

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
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

    CHECK_THROWS_AS(Mcp::size_factors(current, json::array({0, nullptr, nullptr})), Mcp::ToolFailure);
    CHECK_THROWS_AS(Mcp::size_factors(current, json::array({-80, nullptr, nullptr})), Mcp::ToolFailure);
    CHECK_THROWS_AS(Mcp::size_factors(current, json::array({80, 30})), Mcp::ToolFailure);
    CHECK_THROWS_AS(Mcp::size_factors(current, json::array({nullptr, nullptr, nullptr})), Mcp::ToolFailure);
    // A flat shape has no size to scale along its flat axis.
    CHECK_THROWS_AS(Mcp::size_factors(Vec3d(50., 50., 0.), json::array({nullptr, nullptr, 2})), Mcp::ToolFailure);
}

TEST_CASE("primitive size params parse numbers separated by spaces or commas", "[Mcp][Scene]")
{
    CHECK(NativeCommands::parse_sizes("90 4") == std::vector<double>{90., 4.});
    CHECK(NativeCommands::parse_sizes("90,4") == std::vector<double>{90., 4.});
    CHECK(NativeCommands::parse_sizes(" 40, 40  10.5 ") == std::vector<double>{40., 40., 10.5});
    CHECK(NativeCommands::parse_sizes("25.6") == std::vector<double>{25.6});

    CHECK_FALSE(NativeCommands::parse_sizes("").has_value());
    CHECK_FALSE(NativeCommands::parse_sizes(" , ").has_value());
    CHECK_FALSE(NativeCommands::parse_sizes("90 x").has_value());
    CHECK_FALSE(NativeCommands::parse_sizes("90mm 4").has_value());
    CHECK_FALSE(NativeCommands::parse_sizes("90 0").has_value());
    CHECK_FALSE(NativeCommands::parse_sizes("90 -4").has_value());
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

// A core with stand-ins for the tools that start an arrange (the typed arrange, run_action
// obj_arrange, a click on a plate's Arrange icon, which queues its event), change the scene (a
// mutate tool, a key on the canvas), answer a dialog, read, and cancel.
struct Harness
{
    UiThread                      ui;
    FakeWorker                    worker;
    Mcp::UiInvoker                invoker{ui, [] { return Mcp::GuardState{}; }};
    Mcp::JobRegistry              jobs;
    Mcp::Journal                  journal;
    Mcp::ToolRegistry             registry;
    Mcp::CoreTiming               timing;
    std::chrono::milliseconds     job_length{800};
    std::unique_ptr<Mcp::McpCore> core;

    Harness()
    {
        timing.soft_budget = 100ms;
        add("arrange", Mcp::GuardClass::Mutate, [this](const json&, Mcp::ToolContext& ctx) {
            ctx.run_ui([&] {
                worker.start(job_length);
                return json();
            });
            while (ctx.observe_ui([&] { return json(worker.busy()); }).get<bool>())
                std::this_thread::sleep_for(20ms);
            return Mcp::ToolResult::ok({{"arranged", true}});
        });
        add("run_action", Mcp::GuardClass::Mutate, [this](const json&, Mcp::ToolContext& ctx) {
            ctx.run_ui([&] {
                worker.start(job_length);
                return json();
            });
            return Mcp::ToolResult::ok({{"ran", "obj_arrange"}});
        });
        add("ui_click", Mcp::GuardClass::Recover, [this](const json&, Mcp::ToolContext& ctx) {
            ctx.run_ui([&] {
                core->admit({}); // the target is outside any dialog
                ui.post([this] { worker.start(job_length); });
                return json();
            });
            ctx.observe_ui([] { return json(); }); // settle
            return Mcp::ToolResult::ok();
        });
        add("ui_key", Mcp::GuardClass::Recover, [this](const json&, Mcp::ToolContext& ctx) {
            return Mcp::ToolResult::ok(ctx.run_ui([&] {
                core->admit({});
                return json("deleted");
            }));
        });
        add("dialog_respond", Mcp::GuardClass::Recover,
            [](const json&, Mcp::ToolContext& ctx) { return Mcp::ToolResult::ok(ctx.run_ui([] { return json("pressed"); })); });
        add("edit", Mcp::GuardClass::Mutate,
            [](const json&, Mcp::ToolContext& ctx) { return Mcp::ToolResult::ok(ctx.run_ui([] { return json("edited"); })); });
        add("read", Mcp::GuardClass::Inspect,
            [this](const json&, Mcp::ToolContext& ctx) { return Mcp::ToolResult::ok(ctx.run_ui([&] { return json(worker.busy()); })); });
        add("cancel", Mcp::GuardClass::Recover, [this](const json&, Mcp::ToolContext& ctx) {
            return Mcp::ToolResult::ok(ctx.run_ui([&] {
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

    void add(const std::string& name, Mcp::GuardClass guard, Mcp::ToolHandler handler)
    {
        Mcp::ToolDef d;
        d.name                     = name;
        d.guard                    = guard;
        d.inline_call              = guard == Mcp::GuardClass::Inspect;
        d.allowed_during_operation = name == "cancel";
        d.budget_ms                = 200;
        d.handler                  = std::move(handler);
        registry.add(std::move(d));
    }
    void start()
    {
        core = std::make_unique<Mcp::McpCore>(registry, invoker, jobs, &journal, timing,
                                              Mcp::WorkerHooks{[this] { return worker.busy(); }, [this] { worker.cancel(); }});
    }
    Mcp::ToolResult call(const std::string& tool) { return core->call(*registry.find(tool), json::object()); }

    // Starts a worker job with `tool`, which outlives the reply budget; returns the call's job.
    std::string start_job(const std::string& tool)
    {
        const Mcp::ToolResult started = call(tool);
        REQUIRE_FALSE(started.is_error);
        REQUIRE(started.data["status"] == "running");
        return started.data["job"];
    }
    bool refused_by(const Mcp::ToolResult& r, const std::string& job)
    {
        return r.is_error && r.message.find("Busy: job " + job + " is running an operation") != std::string::npos;
    }
};

} // namespace

TEST_CASE("a worker job a call starts holds admission past the reply budget until the worker is idle", "[Mcp][Scene]")
{
    Harness h;
    h.start();
    const std::string job = h.start_job("run_action");
    CHECK(h.refused_by(h.call("edit"), job));
    CHECK_FALSE(h.call("read").is_error);

    const auto done = h.jobs.wait(job, 5000ms);
    REQUIRE(done.has_value());
    CHECK_FALSE(done->is_error);
    CHECK(done->data["ran"] == "obj_arrange");
    // The job ends only once the worker is idle, so a change right after it is admitted.
    CHECK_FALSE(h.worker.busy());
    CHECK_FALSE(h.call("edit").is_error);
}

TEST_CASE("a cancelled worker job keeps admission until the worker is really idle", "[Mcp][Scene]")
{
    Harness h;
    h.job_length          = 10s;
    h.worker.cancel_delay = 800ms; // slower than the reply budget
    SECTION("cancel")
    {
        h.start();
        const std::string job       = h.start_job("arrange");
        const Mcp::ToolResult cancel = h.call("cancel");
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
        const std::string job = h.start_job("run_action");
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
    // The typed arrange, run_action obj_arrange, and a recover input whose click queues the job.
    for (const char* starter : {"arrange", "run_action", "ui_click"}) {
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
    const std::string job = h.start_job("run_action");
    CHECK(h.refused_by(h.call("ui_key"), job));   // Delete on the canvas
    CHECK(h.refused_by(h.call("ui_click"), job)); // a GL toolbar item
    CHECK_FALSE(h.call("dialog_respond").is_error);
    REQUIRE(h.jobs.wait(job, 5000ms).has_value());
    CHECK_FALSE(h.call("ui_key").is_error);
}

TEST_CASE("a worker job that starts between a call's closures is the call's, whoever started it", "[Mcp][Scene]")
{
    // The worker doesn't record who queued a job, so a person's job that starts while a call that
    // found the worker idle is still running is claimed by that call, as one its change queued is.
    Harness h;
    h.add("settle", Mcp::GuardClass::Mutate, [](const json&, Mcp::ToolContext& ctx) {
        ctx.run_ui([] { return json("edited"); });
        for (int i = 0; i < 10; ++i) {
            ctx.observe_ui([] { return json(); });
            std::this_thread::sleep_for(20ms);
        }
        return Mcp::ToolResult::ok();
    });
    h.start();
    Mcp::ToolResult settled;
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

TEST_CASE("a worker job a person started refuses changes but not reads, and cancel leaves it", "[Mcp][Scene]")
{
    Harness h;
    h.start();
    h.worker.start(800ms); // Text typing
    for (const char* change : {"edit", "ui_key"}) {
        const Mcp::ToolResult refused = h.call(change);
        CHECK(refused.is_error);
        CHECK(refused.message.find("Busy: a background job") != std::string::npos);
    }
    const Mcp::ToolResult read = h.call("read");
    CHECK_FALSE(read.is_error);
    CHECK(read.data == true);
    CHECK(h.call("cancel").data.is_null());
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
    CHECK(object_json["volumes"][2]["bbox"]["max"][1].get<double>() > 200.);
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
    CHECK(info["volumes"][0]["mirrored"] == true);
    check_vec3(info["volumes"][0]["scale"], Vec3d::Ones());
    CHECK(info["volumes"][1]["mirrored"] == false);
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
    CHECK(info["volumes"][0]["filament"] == 1);

    object->config.set("extruder", 2);
    info = Mcp::object_info(*object, 0);
    CHECK(info["filament"] == 2);
    CHECK(info["volumes"][0]["filament"] == 2);
    CHECK(info["volumes"][1]["filament"] == 2);
    CHECK(info["volumes"][2]["filament"] == 3);
    // A modifier without its own filament leaves the filament of what it covers alone.
    CHECK_FALSE(info["volumes"][3].contains("filament"));
    CHECK(info["volumes"][4]["filament"] == 4);
    CHECK_FALSE(info["volumes"][5].contains("filament"));
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
    CHECK_FALSE(info["volumes"][0].contains("subtype"));
    CHECK(info["volumes"][1]["type"] == "precise_seam");
    CHECK(info["volumes"][1]["subtype"] == "left");
    CHECK(info["volumes"][2]["subtype"] == "neutral");
    CHECK_FALSE(info["volumes"][1].contains("filament"));
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

    CHECK_THROWS_AS(Mcp::part_shape_size("cube", json::array({40, 30})), Mcp::ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("sphere", json::array({8, 8})), Mcp::ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("cylinder", json::array({0, 3})), Mcp::ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("cylinder", json::array({-6, 3})), Mcp::ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("cylinder", json::array({std::nan(""), 3})), Mcp::ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("cylinder", json::array({"6", 3})), Mcp::ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("cylinder", json(6)), Mcp::ToolFailure);
    CHECK_THROWS_AS(Mcp::part_shape_size("slab", json()), Mcp::ToolFailure);
    // The message names the fields, as the dialog does.
    try {
        Mcp::part_shape_size("torus", json::array({20}));
        FAIL("a torus with one size was accepted");
    } catch (const Mcp::ToolFailure& e) {
        CHECK(std::string(e.what()) == "A torus's size is [Inner diameter, Tube diameter] in mm, each above 0");
    }
}

TEST_CASE("an instance count param is a whole number", "[Mcp][Scene]")
{
    CHECK(NativeCommands::parse_count("6") == 6);
    CHECK(NativeCommands::parse_count(" 12 ") == 12);
    CHECK(NativeCommands::parse_count("0") == 0);
    CHECK(NativeCommands::parse_count("1000") == 1000);

    CHECK_FALSE(NativeCommands::parse_count("").has_value());
    CHECK_FALSE(NativeCommands::parse_count("-1").has_value());
    CHECK_FALSE(NativeCommands::parse_count("6.5").has_value());
    CHECK_FALSE(NativeCommands::parse_count("6 7").has_value());
    CHECK_FALSE(NativeCommands::parse_count("1e3").has_value());
    CHECK_FALSE(NativeCommands::parse_count("six").has_value());
    CHECK_FALSE(NativeCommands::parse_count("99999999999").has_value());
}

// The object list's context-menu commands are Speed Dial commands beside the other object commands,
// and a param stands in for each one's dialog.
TEST_CASE("the object structure commands take the params that replace their dialogs", "[Mcp][Scene]")
{
    const NativeCommand* peer = NativeCommands::find("obj_delete");
    REQUIRE(peer != nullptr);
    struct Expected
    {
        const char*      key;
        CommandParamType param;
    };
    for (const Expected& e : {Expected{"obj_type_part", CommandParamType::None}, Expected{"obj_type_negative", CommandParamType::None},
                              Expected{"obj_type_modifier", CommandParamType::None},
                              Expected{"obj_type_support_blocker", CommandParamType::None},
                              Expected{"obj_type_support_enforcer", CommandParamType::None},
                              Expected{"obj_type_precise_seam", CommandParamType::None}, Expected{"obj_rename", CommandParamType::Text},
                              Expected{"obj_set_instances", CommandParamType::Index}, Expected{"obj_assemble", CommandParamType::None},
                              Expected{"obj_instances_to_objects", CommandParamType::None},
                              Expected{"obj_order_by_name", CommandParamType::Text}, Expected{"obj_order_by_filament", CommandParamType::Text},
                              Expected{"obj_order_by_height", CommandParamType::Text}, Expected{"obj_order_by_size", CommandParamType::Text}}) {
        INFO(e.key);
        const NativeCommand* c = NativeCommands::find(e.key);
        REQUIRE(c != nullptr);
        CHECK(c->group == peer->group);
        CHECK(c->param.type == e.param);
        CHECK_FALSE(c->description.empty());
        CHECK(c->param.help.empty() == (e.param == CommandParamType::None));
    }
}

namespace {

// What the object list gives a new range: the object's layer height and its filament ("Default").
DynamicPrintConfig new_range_config()
{
    DynamicPrintConfig config;
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("extruder", new ConfigOptionInt(0));
    return config;
}

// set_layer_ranges' parse on a two-filament printer whose layer heights are 0.08-0.28.
t_layer_config_ranges parse_ranges(const json& ranges)
{
    return Mcp::layer_config_ranges_from_json(ranges, new_range_config(), 2, [](int) { return std::make_pair(0.08, 0.28); });
}

json range(double min_z, double max_z, json values = json::object())
{
    return {{"min_z", min_z}, {"max_z", max_z}, {"values", std::move(values)}};
}

} // namespace

TEST_CASE("height ranges are checked before the object changes", "[Mcp][Scene][Layers]")
{
    // Sorted by height; touching ranges are fine; each starts from the new-range config.
    const t_layer_config_ranges ranges = parse_ranges(
        json::array({range(10, 20, {{"sparse_infill_density", "25%"}}), range(0, 10, {{"layer_height", 0.12}, {"extruder", "2"}})}));
    REQUIRE(ranges.size() == 2);
    const auto& [low, low_config]   = *ranges.begin();
    const auto& [high, high_config] = *std::next(ranges.begin());
    CHECK(low == t_layer_height_range(0., 10.));
    CHECK(high == t_layer_height_range(10., 20.));
    CHECK_THAT(low_config.opt_float("layer_height"), WithinAbs(0.12, 1e-9));
    CHECK(low_config.opt_int("extruder") == 2);
    CHECK_THAT(high_config.opt_float("layer_height"), WithinAbs(0.2, 1e-9));
    CHECK(high_config.opt_int("extruder") == 0);
    CHECK(high_config.opt_serialize("sparse_infill_density") == "25%");
    CHECK(parse_ranges(json::array()).empty());

    // An overlap names both ranges, whatever order they come in.
    CHECK_THROWS_WITH(parse_ranges(json::array({range(12, 20), range(0, 15)})), ContainsSubstring("0-15 and 12-20 overlap"));
    CHECK_THROWS_WITH(parse_ranges(json::array({range(0, 15), range(0, 15)})), ContainsSubstring("overlap"));
    CHECK_THROWS_WITH(parse_ranges(json::array({range(0, 20), range(5, 10)})), ContainsSubstring("0-20 and 5-10 overlap"));
    // Empty, reversed, below the bed or without a top.
    CHECK_THROWS_WITH(parse_ranges(json::array({range(5, 5)})), ContainsSubstring("0 <= min_z < max_z"));
    CHECK_THROWS_AS(parse_ranges(json::array({range(6, 5)})), Mcp::ToolFailure);
    CHECK_THROWS_AS(parse_ranges(json::array({range(-1, 5)})), Mcp::ToolFailure);
    CHECK_THROWS_AS(parse_ranges(json::array({json{{"min_z", 0}}})), Mcp::ToolFailure);

    // A layer height given is checked against the printer's, with the range's filament.
    int asked = -1;
    const auto bounds = [&asked](int extruder) {
        asked = extruder;
        return std::make_pair(0.08, 0.28);
    };
    CHECK_THROWS_WITH(Mcp::layer_config_ranges_from_json(json::array({range(0, 5, {{"layer_height", 0.4}, {"extruder", 2}})}),
                                                         new_range_config(), 2, bounds),
                      ContainsSubstring("layer_height 0.4 is outside this printer's 0.08-0.28"));
    CHECK(asked == 2);
    // The object's layer height a range starts from is not checked, as the range editor checks only what is typed.
    DynamicPrintConfig thick = new_range_config();
    thick.set_key_value("layer_height", new ConfigOptionFloat(0.4));
    CHECK(Mcp::layer_config_ranges_from_json(json::array({range(0, 5)}), thick, 2, bounds).size() == 1);

    // Filaments are 0 (the object's) up to the slot count.
    CHECK_THROWS_WITH(parse_ranges(json::array({range(0, 5, {{"extruder", 3}})})), ContainsSubstring("1-2"));
    CHECK_THROWS_AS(parse_ranges(json::array({range(0, 5, {{"extruder", -1}})})), Mcp::ToolFailure);
    // Values are parsed strictly.
    CHECK_THROWS_WITH(parse_ranges(json::array({range(0, 5, {{"sparse_infill_density", "lots"}})})),
                      ContainsSubstring("Invalid value for sparse_infill_density"));
    CHECK_THROWS_AS(parse_ranges(json::array({range(0, 5, {{"wall_loops", nullptr}})})), Mcp::ToolFailure);
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

    try {
        parse_ranges(json::array({range(0, 5, {{"enable_support", true}})}));
        FAIL("an object setting was accepted for a range");
    } catch (const Mcp::ToolFailure& e) {
        CHECK_THAT(e.what(), ContainsSubstring("not a height range setting"));
    }
    try {
        parse_ranges(json::array({range(0, 5, {{"sparse_infill_dens", "20%"}})}));
        FAIL("an unknown key was accepted");
    } catch (const Mcp::ToolFailure& e) {
        REQUIRE(e.details.contains("similar"));
        CHECK(std::find(e.details["similar"].begin(), e.details["similar"].end(), json("sparse_infill_density")) != e.details["similar"].end());
    }
}

TEST_CASE("list_objects reports an object's height ranges", "[Mcp][Scene][Layers]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(20., 20., 30.), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    CHECK_FALSE(Mcp::object_info(*object, 0).contains("layer_config_ranges"));

    object->layer_config_ranges = parse_ranges(json::array({range(15, 30, {{"sparse_infill_density", "40%"}}), range(0, 15, {{"layer_height", 0.12}})}));
    const json info = Mcp::object_info(*object, 0);
    REQUIRE(info["layer_config_ranges"].size() == 2);
    const json& low = info["layer_config_ranges"][0];
    CHECK(low["min_z"] == 0.);
    CHECK(low["max_z"] == 15.);
    CHECK(low["values"] == json{{"extruder", "0"}, {"layer_height", "0.12"}});
    CHECK(info["layer_config_ranges"][1]["values"]["sparse_infill_density"] == "40%");
    // Ranges are not the object's own overrides.
    CHECK(info["overrides"].empty());

    // What list_objects shows can be given back unchanged.
    const t_layer_config_ranges again = parse_ranges(info["layer_config_ranges"]);
    REQUIRE(again.size() == object->layer_config_ranges.size());
    for (auto a = again.cbegin(), b = object->layer_config_ranges.cbegin(); a != again.cend(); ++a, ++b) {
        CHECK(a->first == b->first);
        CHECK(a->second.get() == b->second.get());
    }
}

TEST_CASE("list_objects reports a variable layer height profile as the object list does", "[Mcp][Scene][Layers]")
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
    object->layer_config_ranges = parse_ranges(json::array({range(0, 10, {{"layer_height", 0.1}}), range(10, 20, {{"layer_height", 0.25}})}));
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

TEST_CASE("layer code types have the names list_plates reads", "[Mcp][Scene][Layers]")
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

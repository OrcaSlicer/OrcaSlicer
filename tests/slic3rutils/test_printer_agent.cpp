#include <catch2/catch_all.hpp>

#include <functional>
#include <slic3r/Utils/BBLPrinterAgent.hpp>
#include <slic3r/Utils/IPrinterAgent.hpp>
#include <slic3r/Utils/MoonrakerPrinterAgent.hpp>
#include <memory>
#include <slic3r/Utils/NetworkAgentFactory.hpp>

#include <catch2/catch_test_macros.hpp>
#include <pybind11/gil.h>
#include <pybind11/pytypes.h>
#include <catch2/catch_message.hpp>
#include "catch2/matchers/catch_matchers.hpp"
#include "catch2/matchers/catch_matchers_floating_point.hpp"
#include "python_test_support.hpp"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <atomic>
#include <chrono>
#include <future>
#include <nlohmann/json.hpp>
#include <slic3r/Utils/bambu_networking.hpp>
#include <string>
#include <thread>
#include <pybind11/cast.h>
#include <utility>
#include <vector>

namespace Slic3r { class ICloudServiceAgent; }
namespace Slic3r { class IPrinterAgent; }

using namespace Slic3r;
namespace py = pybind11;

namespace {

// Releases a promise on scope exit, so a throwing REQUIRE cannot leave a parked detached
// thread (and any destructor that joins it) blocked forever.
class ScopedPromiseRelease
{
public:
    explicit ScopedPromiseRelease(std::shared_ptr<std::promise<void>> p) : m_p(std::move(p)) {}
    ~ScopedPromiseRelease()
    {
        if (m_p) {
            try {
                m_p->set_value();
            } catch (...) {
                // promise already satisfied
            }
        }
    }
    ScopedPromiseRelease(const ScopedPromiseRelease&) = delete;
    ScopedPromiseRelease& operator=(const ScopedPromiseRelease&) = delete;

private:
    std::shared_ptr<std::promise<void>> m_p;
};

} // namespace

class MoonrakerParserProbe : public MoonrakerPrinterAgent
{
public:
    using MoonrakerPrinterAgent::parse_nozzle_diameter;

    explicit MoonrakerParserProbe(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}
};

// Drives BBLPrinterAgent's legacy pushing.start decision without the proprietary plug-in:
// the wire seams record what would have been sent, and the clock is controllable.
class BblLegacyPushingProbe : public BBLPrinterAgent
{
public:
    using BBLPrinterAgent::note_inbound_message;

    struct Sent
    {
        bool        lan = false;
        std::string payload;
    };

    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::vector<Sent>                     sent;

protected:
    int send_cloud_message(std::string, std::string json_str, int, int) override
    {
        sent.push_back({false, std::move(json_str)});
        return 0;
    }

    int send_lan_message(std::string, std::string json_str, int, int) override
    {
        sent.push_back({true, std::move(json_str)});
        return 0;
    }

    std::chrono::steady_clock::time_point legacy_clock_now() const override { return now; }
};

namespace {

std::string pushing_command(const BblLegacyPushingProbe::Sent& sent)
{
    return nlohmann::json::parse(sent.payload)
        .value("pushing", nlohmann::json::object())
        .value("command", std::string());
}

} // namespace

TEST_CASE("Moonraker parses nozzle diameter from configfile settings", "[MoonrakerPrinterAgent]")
{
    const auto response = nlohmann::json::parse(R"({
        "result": {
            "status": {
                "configfile": {
                    "settings": {
                        "extruder": {
                            "nozzle_diameter": 0.6
                        }
                    }
                }
            }
        }
    })");

    CHECK_THAT(MoonrakerParserProbe::parse_nozzle_diameter(response), Catch::Matchers::WithinAbs(0.6f, 1e-4f));
}

TEST_CASE("Moonraker parses nozzle diameter from raw config and tolerates missing data", "[MoonrakerPrinterAgent]")
{
    const auto raw_config_response = nlohmann::json::parse(R"({
        "result": {
            "status": {
                "configfile": {
                    "config": {
                        "extruder": {
                            "nozzle_diameter": "0.8"
                        }
                    }
                }
            }
        }
    })");
    const auto missing_response = nlohmann::json::object();

    CHECK_THAT(MoonrakerParserProbe::parse_nozzle_diameter(raw_config_response), Catch::Matchers::WithinAbs(0.8f, 1e-4f));
    CHECK_THAT(MoonrakerParserProbe::parse_nozzle_diameter(missing_response), Catch::Matchers::WithinAbs(0.0f, 1e-6f));
}

// why: an agent without a Bambu-dialect translation must refuse these commands before any network or wx path.
TEST_CASE("default AMS commands report not supported", "[MoonrakerPrinterAgent]")
{
    MoonrakerPrinterAgent agent("");

    CHECK(agent.command_ams_refresh_rfid("dev", 123, 1, 0, false) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.command_ams_calibrate("dev", 1, 2, false) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.command_ams_select_tray("dev", "123", 3, false) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
}

TEST_CASE("Moonraker light name matching", "[MoonrakerPrinterAgent]")
{
    CHECK(moonraker_is_light_name("caselight"));
    CHECK(moonraker_is_light_name("LED_STRIP"));
    CHECK_FALSE(moonraker_is_light_name("beeper"));
    CHECK(moonraker_is_light_name("FLASHLIGHT_SWITCH"));
    CHECK(moonraker_is_light_name("MODLELIGHT_SWITCH"));
}

TEST_CASE("Moonraker webcam selection skips disabled webcams and prefers the first enabled one",
          "[MoonrakerPrinterAgent]")
{
    const auto response = nlohmann::json::parse(R"({
        "result": { "webcams": [
            { "name": "disabled", "enabled": false, "stream_url": "http://192.168.1.9:8080/stream" },
            { "name": "enabled",  "enabled": true,  "stream_url": "http://192.168.1.9:8080/stream" }
        ]}
    })");

    MoonrakerWebcamSelection selection;
    REQUIRE(moonraker_parse_webcam_list(response, "http://192.168.1.9:7125", selection));
    CHECK(selection.name == "enabled");
    CHECK(selection.url == "http://192.168.1.9:8080/stream");
    CHECK(selection.mode == CameraStreamMode::http);
    CHECK(selection.error.empty());
}

TEST_CASE("Moonraker webcam selection resolves relative URLs, maps rtsp, and rejects other schemes",
          "[MoonrakerPrinterAgent]")
{
    const auto relative = nlohmann::json::parse(R"({
        "result": { "webcams": [ { "name": "cam", "snapshot_url": "/webcam/?action=snapshot" } ] }
    })");
    MoonrakerWebcamSelection rel;
    REQUIRE(moonraker_parse_webcam_list(relative, "http://192.168.1.9:7125", rel));
    // Relative URLs use the printer web root, without the Moonraker API port.
    CHECK(rel.url == "http://192.168.1.9/webcam/?action=snapshot");
    CHECK(rel.mode == CameraStreamMode::http_snapshot);

    const auto rtsp = nlohmann::json::parse(R"({
        "result": { "webcams": [ { "name": "cam", "stream_url": "rtsp://192.168.1.9:554/live" } ] }
    })");
    MoonrakerWebcamSelection rt;
    REQUIRE(moonraker_parse_webcam_list(rtsp, "http://192.168.1.9:7125", rt));
    CHECK(rt.mode == CameraStreamMode::rtsp);

    const auto unsupported = nlohmann::json::parse(R"({
        "result": { "webcams": [ { "name": "cam", "stream_url": "weird://host/x" } ] }
    })");
    MoonrakerWebcamSelection bad;
    CHECK_FALSE(moonraker_parse_webcam_list(unsupported, "http://192.168.1.9:7125", bad));
    CHECK(bad.error == "Unsupported webcam URL");
}

TEST_CASE("Moonraker webcam selection reports no webcam and malformed structure", "[MoonrakerPrinterAgent]")
{
    const auto empty = nlohmann::json::parse(R"({ "result": { "webcams": [] } })");
    MoonrakerWebcamSelection none;
    CHECK_FALSE(moonraker_parse_webcam_list(empty, "http://host:7125", none));
    CHECK(none.error == "No enabled webcam");

    const auto disabled_only = nlohmann::json::parse(R"({
        "result": { "webcams": [ { "name": "disabled", "enabled": false, "stream_url": "http://host/stream" } ] }
    })");
    MoonrakerWebcamSelection off;
    CHECK_FALSE(moonraker_parse_webcam_list(disabled_only, "http://host:7125", off));

    const auto malformed = nlohmann::json::parse(R"({ "result": { "nope": 1 } })");
    MoonrakerWebcamSelection shape;
    CHECK_FALSE(moonraker_parse_webcam_list(malformed, "http://host:7125", shape));
    CHECK(shape.error == "Unexpected JSON structure");
}

// ===========================================================================
// UNIT - handle_request's not-supported default.
// The agent is the only thing that knows what it can translate, so an untranslated
// command has to say so instead of returning success and letting the UI believe the
// control worked. Guards the inverse too: the pushing namespace is genuinely
// satisfied by the websocket status stream, and it re-fires from the keepalive timer
// roughly once a second, so it must stay a success or it would raise a dialog on a
// timer. Only branches that touch neither the network nor wx are exercised.
// ===========================================================================
TEST_CASE("Moonraker reports untranslated commands as not supported", "[MoonrakerPrinterAgent]")
{
    MoonrakerPrinterAgent agent("");

    CHECK(agent.send_message("dev", R"({"print":{"command":"ams_change_filament"}})", 0, 0) ==
          ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.send_message("dev", R"({"system":{"command":"set_door_stat"}})", 0, 0) ==
          ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.send_message("dev", R"({"xcam":{"command":"xcam_control_set"}})", 0, 0) ==
          ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);

    CHECK(agent.send_message("dev", R"({"pushing":{"command":"pushall"}})", 0, 0) == BAMBU_NETWORK_SUCCESS);
    CHECK(agent.send_message("dev", R"({"pushing":{"command":"start"}})", 0, 0) == BAMBU_NETWORK_SUCCESS);

    // why: malformed input is a different failure than an untranslated command, and the
    // default must not swallow it into a misleading not-supported verdict.
    CHECK(agent.send_message("dev", "{not json", 0, 0) == BAMBU_NETWORK_ERR_INVALID_RESULT);
}

TEST_CASE("Bambu legacy compatibility adds one internal pushing.start after silence", "[BBLPrinterAgent]")
{
    BblLegacyPushingProbe agent;
    const auto             start = agent.now;
    const std::string      pushall = R"({"pushing":{"command":"pushall","sequence_id":"20001"}})";

    agent.note_inbound_message("legacy-lan");
    agent.note_mqtt_alive_support("legacy-lan", false);

    // A report just arrived: the shared pushall is forwarded unchanged and needs no start.
    REQUIRE(agent.send_message_to_printer("legacy-lan", pushall, 0, 0) == 0);
    REQUIRE(agent.sent.size() == 1);
    CHECK(agent.sent.front().lan);
    CHECK(agent.sent.front().payload == pushall);

    // The shared liveness probe fires after 20 s of silence. The agent adds its
    // compatibility start before forwarding the same shared pushall.
    agent.now = start + std::chrono::seconds(21);
    agent.sent.clear();
    REQUIRE(agent.send_message_to_printer("legacy-lan", pushall, 0, 0) == 0);
    REQUIRE(agent.sent.size() == 2);
    CHECK(agent.sent[0].lan);
    CHECK(pushing_command(agent.sent[0]) == "start");
    CHECK(agent.sent[1].lan);
    CHECK(agent.sent[1].payload == pushall);

    // The cloud path keeps the same routing and the same first-request suppression.
    agent.note_inbound_message("legacy-cloud");
    agent.note_mqtt_alive_support("legacy-cloud", false);
    agent.now = start + std::chrono::seconds(21);
    agent.sent.clear();
    REQUIRE(agent.send_message("legacy-cloud", pushall, 0, 0) == 0);
    REQUIRE(agent.sent.size() == 1);
    CHECK_FALSE(agent.sent.front().lan);
    CHECK(agent.sent.front().payload == pushall);

    // Once observed, the internal start goes out over the cloud transport, not LAN.
    agent.now = start + std::chrono::seconds(42);
    agent.sent.clear();
    REQUIRE(agent.send_message("legacy-cloud", pushall, 0, 0) == 0);
    REQUIRE(agent.sent.size() == 2);
    CHECK_FALSE(agent.sent[0].lan);
    CHECK(pushing_command(agent.sent[0]) == "start");
    CHECK_FALSE(agent.sent[1].lan);
    CHECK(agent.sent[1].payload == pushall);
}

TEST_CASE("Bambu legacy starts are throttled and wait for fresh silence", "[BBLPrinterAgent]")
{
    BblLegacyPushingProbe agent;
    const auto             start = agent.now;
    const std::string      pushall = R"({"pushing":{"command":"pushall","sequence_id":"20001"}})";

    agent.note_inbound_message("legacy-throttle");
    agent.note_mqtt_alive_support("legacy-throttle", false);
    // The first connect request is forwarded without a start.
    REQUIRE(agent.send_message("legacy-throttle", pushall, 0, 0) == 0);
    REQUIRE(agent.sent.size() == 1);
    agent.sent.clear();

    agent.now = start + std::chrono::seconds(21);
    REQUIRE(agent.send_message("legacy-throttle", pushall, 0, 0) == 0);
    REQUIRE(agent.sent.size() == 2);
    CHECK(pushing_command(agent.sent[0]) == "start");

    // Still silent, but inside the start throttle: forward only.
    agent.now = start + std::chrono::seconds(22);
    agent.sent.clear();
    REQUIRE(agent.send_message("legacy-throttle", pushall, 0, 0) == 0);
    REQUIRE(agent.sent.size() == 1);
    CHECK(agent.sent.front().payload == pushall);

    // Past the throttle and still silent: start again.
    agent.now = start + std::chrono::seconds(37);
    agent.sent.clear();
    REQUIRE(agent.send_message("legacy-throttle", pushall, 0, 0) == 0);
    REQUIRE(agent.sent.size() == 2);
    CHECK(pushing_command(agent.sent[0]) == "start");

    // A new report proves the stream is alive; a pushall 10 s later is an ordinary
    // status request, not the silence workaround.
    agent.note_inbound_message("legacy-throttle");
    agent.now = start + std::chrono::seconds(47);
    agent.sent.clear();
    REQUIRE(agent.send_message("legacy-throttle", pushall, 0, 0) == 0);
    REQUIRE(agent.sent.size() == 1);
    CHECK(agent.sent.front().payload == pushall);
}

TEST_CASE("Bambu pushing.start respects the reported capability and observation window", "[BBLPrinterAgent]")
{
    BblLegacyPushingProbe agent;
    const auto             start = agent.now;
    const std::string      pushall = R"({"pushing":{"command":"pushall","sequence_id":"20001"}})";

    SECTION("a known supporting printer is never started")
    {
        agent.note_inbound_message("supported");
        agent.note_mqtt_alive_support("supported", true);
        REQUIRE(agent.send_message("supported", pushall, 0, 0) == 0); // first request
        REQUIRE(agent.sent.size() == 1);
        agent.now = start + std::chrono::seconds(21);
        agent.sent.clear();
        REQUIRE(agent.send_message("supported", pushall, 0, 0) == 0);
        REQUIRE(agent.sent.size() == 1);
        CHECK(agent.sent.front().payload == pushall);
    }

    SECTION("an unknown capability keeps the active stream untouched")
    {
        agent.note_inbound_message("unknown-active");
        REQUIRE(agent.send_message("unknown-active", pushall, 0, 0) == 0); // first request
        REQUIRE(agent.sent.size() == 1);
        agent.now = start + std::chrono::seconds(19);
        agent.sent.clear();
        REQUIRE(agent.send_message("unknown-active", pushall, 0, 0) == 0);
        REQUIRE(agent.sent.size() == 1);
        CHECK(agent.sent.front().payload == pushall);
    }

    SECTION("an unknown capability uses the same 20 s legacy silence threshold")
    {
        // Old code treated an unreported capability as legacy and started after 20 s.
        agent.note_inbound_message("unknown-legacy");
        REQUIRE(agent.send_message("unknown-legacy", pushall, 0, 0) == 0); // first request
        REQUIRE(agent.sent.size() == 1);
        agent.now = start + std::chrono::seconds(21);
        agent.sent.clear();
        REQUIRE(agent.send_message("unknown-legacy", pushall, 0, 0) == 0);
        REQUIRE(agent.sent.size() == 2);
        CHECK(pushing_command(agent.sent[0]) == "start");
        CHECK(agent.sent[1].payload == pushall);
    }

    SECTION("selection time is the silence reference when no report arrives")
    {
        agent.set_user_selected_machine("selected-only"); // seeds activity before the plug-in call
        agent.now = start + std::chrono::seconds(21);
        REQUIRE(agent.send_message("selected-only", pushall, 0, 0) == 0); // first request
        REQUIRE(agent.sent.size() == 1);

        // Selection was 42 s ago; the next request uses the legacy fallback.
        agent.now = start + std::chrono::seconds(42);
        agent.sent.clear();
        REQUIRE(agent.send_message("selected-only", pushall, 0, 0) == 0);
        REQUIRE(agent.sent.size() == 2);
        CHECK(pushing_command(agent.sent[0]) == "start");
        CHECK(agent.sent[1].payload == pushall);
    }

    SECTION("a printer that has never reported is observed before the fallback")
    {
        // The initial-connect pushall starts the observation but must not add a start.
        REQUIRE(agent.send_message("never-reported", pushall, 0, 0) == 0);
        REQUIRE(agent.sent.size() == 1);
        CHECK(agent.sent.front().payload == pushall);

        agent.now = start + std::chrono::seconds(19);
        agent.sent.clear();
        REQUIRE(agent.send_message("never-reported", pushall, 0, 0) == 0);
        REQUIRE(agent.sent.size() == 1);
        CHECK(agent.sent.front().payload == pushall);

        agent.now = start + std::chrono::seconds(21);
        agent.sent.clear();
        REQUIRE(agent.send_message("never-reported", pushall, 0, 0) == 0);
        REQUIRE(agent.sent.size() == 2);
        CHECK(pushing_command(agent.sent[0]) == "start");
        CHECK(agent.sent[1].payload == pushall);
    }

    SECTION("unrelated commands are forwarded untouched")
    {
        agent.note_inbound_message("other");
        agent.note_mqtt_alive_support("other", false);
        agent.now = start + std::chrono::seconds(21);
        const std::string pause     = R"({"print":{"command":"pause"}})";
        const std::string stop      = R"({"pushing":{"command":"stop","sequence_id":"20002"}})";
        // Contains the pushall token so the legacy check actually parses it, but the
        // command is not a string.
        const std::string nonstring = R"({"pushing":{"command":["pushall"],"sequence_id":"20003"}})";
        REQUIRE(agent.send_message("other", pause, 0, 0) == 0);
        REQUIRE(agent.send_message("other", stop, 0, 0) == 0);
        // A non-string command must not throw out of the legacy interception; the
        // original payload is forwarded exactly like any other unrecognized command.
        REQUIRE(agent.send_message("other", nonstring, 0, 0) == 0);
        REQUIRE(agent.sent.size() == 3);
        CHECK(agent.sent[0].payload == pause);
        CHECK(agent.sent[1].payload == stop);
        CHECK(agent.sent[2].payload == nonstring);
    }
}

// why: IPrinterAgent::fetch_filament_info is the single virtual hook derived agents override.
// Pull-mode overrides must be synchronous (the caller reads DevFilaSystem as soon as it
// returns), but subscription overrides may be fire-and-forget: QidiPrinterAgent/Snapmaker
// spawn a detached thread when asked for subscription updates. QidiPrinterAgent is `final`,
// so this probes the subscription contract with a controllable double instead.
TEST_CASE("a fire-and-forget subscription fetch is not waited on by the caller",
          "[MoonrakerPrinterAgent]")
{
    class RecordingAgent : public Slic3r::MoonrakerPrinterAgent
    {
    public:
        explicit RecordingAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

        // Shared so the detached proxy fetch never touches `this`: a throwing REQUIRE
        // then cannot leave it dereferencing a destroyed agent.
        std::shared_ptr<std::atomic<bool>>  invoked{std::make_shared<std::atomic<bool>>(false)};
        std::shared_ptr<std::promise<void>> release_gate{std::make_shared<std::promise<void>>()};
        std::shared_ptr<std::promise<void>> done_promise{std::make_shared<std::promise<void>>()};

        bool fetch_filament_info(std::string /*dev_id*/, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override
        {
            // Only the subscription path is allowed to be fire-and-forget.
            if (sync_mode != FilamentSyncMode::subscription)
                return true;

            auto invoked_p      = invoked;
            auto release_gate_p = release_gate;
            auto done_promise_p = done_promise;
            std::thread([invoked_p, release_gate_p, done_promise_p]() {
                invoked_p->store(true);
                // Block here until the test explicitly releases us, proving the caller
                // (fetch_filament_info) does not wait for this to run.
                release_gate_p->get_future().wait();
                done_promise_p->set_value();
            }).detach();
            return true;
        }
    };

    auto agent = std::make_shared<RecordingAgent>(std::string{});
    auto done_future = agent->done_promise->get_future();
    ScopedPromiseRelease release_gate_guard{agent->release_gate};

    bool immediate_result = agent->fetch_filament_info("test-dev", FilamentSyncMode::subscription);

    // fetch_filament_info must return before its background work completes — prove
    // it by confirming the background call is still blocked on the gate right now.
    REQUIRE(immediate_result == true);
    REQUIRE(done_future.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);

    // Now let the background call finish and confirm it actually ran (polymorphic dispatch).
    agent->release_gate->set_value();
    REQUIRE(done_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE(agent->invoked->load() == true);
}

namespace {

// Globals so a parked proxy fetch thread never dereferences a freed agent.
std::atomic<int>  g_deferred_fetch_running{0};
std::atomic<bool> g_deferred_destroy_returned{false};

// Releases the given gates, then joins on scope exit: so a throwing REQUIRE cannot leave
// the thread blocked (deadlocking the join) or let it std::terminate.
class ScopedJoiner
{
public:
    ScopedJoiner(std::thread& t, std::shared_ptr<std::promise<void>> gate1, std::shared_ptr<std::promise<void>> gate2)
        : m_thread(t), m_gates{std::move(gate1), std::move(gate2)}
    {}
    ~ScopedJoiner()
    {
        for (auto& gate : m_gates) {
            if (gate) {
                try {
                    gate->set_value();
                } catch (...) {
                    // promise already satisfied
                }
            }
        }
        if (m_thread.joinable()) m_thread.join();
    }
    ScopedJoiner(const ScopedJoiner&) = delete;
    ScopedJoiner& operator=(const ScopedJoiner&) = delete;

private:
    std::thread&                        m_thread;
    std::shared_ptr<std::promise<void>> m_gates[2];
};

// A fetch that parks before touching the in-flight counter, so teardown's wait can
// observe zero first.
class DeferredFetchAgent : public MoonrakerPrinterAgent
{
public:
    explicit DeferredFetchAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

    // Shared so a parked proxy fetch can never outlive the stack that owns it.
    std::shared_ptr<std::promise<void>> entered{std::make_shared<std::promise<void>>()};
    std::shared_ptr<std::promise<void>> allow_fetch{std::make_shared<std::promise<void>>()};
    std::shared_ptr<std::promise<void>> allow_finish{std::make_shared<std::promise<void>>()};
    std::shared_ptr<std::promise<void>> running{std::make_shared<std::promise<void>>()};

    // Runs the callable on the command worker, which teardown joins.
    void post(std::function<void()> fn) { enqueue_command(std::move(fn)); }

    bool fetch_filament_info(std::string /*dev_id*/, FilamentSyncMode /*sync_mode*/ = FilamentSyncMode::pull) override
    {
        // Resumes after ~DeferredFetchAgent destroyed these members; snapshot up front.
        auto entered_p      = entered;
        auto allow_fetch_p  = allow_fetch;
        auto allow_finish_p = allow_finish;
        auto running_p      = running;

        entered_p->set_value();
        allow_fetch_p->get_future().wait();

        filament_fetch_in_flight.fetch_add(1, std::memory_order_relaxed);
        std::thread([this, finish = std::move(allow_finish_p), running = std::move(running_p)] {
            struct InFlightGuard
            {
                MoonrakerPrinterAgent& owner;
                ~InFlightGuard() { owner.release_fetch_slot(); }
            } guard{*this};

            g_deferred_fetch_running.fetch_add(1, std::memory_order_relaxed);
            running->set_value();
            finish->get_future().wait();
            g_deferred_fetch_running.fetch_sub(1, std::memory_order_relaxed);
        }).detach();
        return true;
    }
};

} // namespace

// REGRESSION - teardown must not return while a fetch it started is in flight.
// The command worker parks a fetch before it reserves the in-flight slot, forcing
// the "wait already observed zero" interleaving deterministically.
TEST_CASE("an agent's destruction waits for a fetch started by its worker during teardown",
          "[MoonrakerPrinterAgent][Regression]")
{
    g_deferred_fetch_running.store(0);
    g_deferred_destroy_returned.store(false);

    auto agent        = std::make_shared<DeferredFetchAgent>(std::string{});
    auto entered      = agent->entered;
    auto allow_fetch  = agent->allow_fetch;
    auto allow_finish = agent->allow_finish;
    auto running      = agent->running;

    // Safety net for the pre-destroyer failure paths: release both gates before the
    // agent is destroyed (declared after it, so destroyed before it).
    ScopedPromiseRelease release_finish{allow_finish};
    ScopedPromiseRelease release_fetch{allow_fetch};

    // Park a fetch inside the command worker while the agent is still complete.
    agent->post([ptr = agent.get()] { ptr->fetch_filament_info("dev", FilamentSyncMode::pull); });
    REQUIRE(entered->get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    // Destroy on another thread so this one can drive the parked fetch.
    std::thread destroyer([owned = std::move(agent)]() mutable {
        owned.reset();
        g_deferred_destroy_returned.store(true);
    });
    // Releases both gates before joining, so a failing REQUIRE cannot deadlock the join.
    ScopedJoiner join_destroyer{destroyer, allow_fetch, allow_finish};

    // Let the worker reserve the in-flight slot and spawn its fetch, then wait until it
    // is genuinely parked (no polling).
    allow_fetch->set_value();
    REQUIRE(running->get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(g_deferred_fetch_running.load() == 1);

    // A correct teardown cannot return while the fetch is parked; give a buggy one time.
    for (int i = 0; i < 200 && !g_deferred_destroy_returned.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (g_deferred_destroy_returned.load()) {
        // Bug: teardown returned with a fetch still running. Don't release allow_finish.
        CHECK(g_deferred_fetch_running.load() == 0);
        return;
    }

    // Fixed order: destruction is still blocked on the in-flight fetch.
    allow_finish->set_value();
    destroyer.join();
    CHECK(g_deferred_destroy_returned.load());
    CHECK(g_deferred_fetch_running.load() == 0);
}

// ===========================================================================
// UNIT - printer-agent registry duplicate handling.
// Confirms a duplicate agent id is rejected so a plugin cannot shadow a built-in
// or previously registered agent.
// ===========================================================================
TEST_CASE("printer-agent registry register / lookup / duplicate-reject", "[PrinterAgent]")
{
    // why: the registry is process-global state shared by the test binary, and
    // Catch2 may run cases in any order. Use an id that cannot collide with
    // built-ins or other cases. Avoid SECTIONs because each section re-runs the
    // body and would register the same id twice.
    const std::string id = "orca-test::registry-probe-7f3a";
    auto stub_factory = [](std::shared_ptr<ICloudServiceAgent>, const std::string&)
        -> std::shared_ptr<IPrinterAgent> { return nullptr; };

    REQUIRE_FALSE(NetworkAgentFactory::is_printer_agent_registered(id));

    REQUIRE(NetworkAgentFactory::register_printer_agent(id, "Registry Probe", stub_factory));
    REQUIRE(NetworkAgentFactory::is_printer_agent_registered(id));

    // Re-registering the same id is rejected and does not replace the entry.
    REQUIRE_FALSE(NetworkAgentFactory::register_printer_agent(id, "Impostor", stub_factory));

    // The first registration's display name survives the rejected duplicate.
    const PrinterAgentInfo* info = NetworkAgentFactory::get_printer_agent_info(id);
    REQUIRE(info != nullptr);
    CHECK(info->display_name == "Registry Probe");

    // It appears exactly once in the UI-population list.
    auto agents = NetworkAgentFactory::get_registered_printer_agents();
    int  count  = 0;
    for (const auto& a : agents)
        if (a.id == id)
            ++count;
    CHECK(count == 1);
}

// ===========================================================================
// INTEGRATION - the orca.printer_agent Python binding surface.
// Boots the embedded interpreter and asserts the C++ to Python contract that
// every printer-agent plugin subclasses. If a binding is renamed or removed,
// plugins fail at runtime even though C++ still compiles.
// ===========================================================================

TEST_CASE("orca.printer_agent binding surface", "[integration][Python]")
{
    ensure_python_initialized();
    py::gil_scoped_acquire gil;
    py::module_ orca = import_orca_module();

    REQUIRE(py::hasattr(orca, "printer_agent"));
    py::object pa = orca.attr("printer_agent");

    // The base class every printer-agent plugin subclasses.
    REQUIRE(py::hasattr(pa, "PrinterAgentBase"));
    py::object base = pa.attr("PrinterAgentBase");
    for (const char* method : { "get_agent_info", "connect_printer", "disconnect_printer",
                                "send_message", "send_message_to_printer",
                                "command_ams_refresh_rfid", "command_ams_calibrate",
                                "command_ams_select_tray", "command_start_camera",
                                "command_xyz_abs", "command_auto_leveling", "command_go_home",
                                "command_set_bed", "command_set_nozzle", "command_axis_control",
                                "start_discovery", "bind_detect",
                                "start_print", "get_filament_sync_mode", "tool_display_order" }) {
        CAPTURE(method);
        CHECK(py::hasattr(base, method));
    }

    // AgentInfo value type - the registry identity the host reads (id is the key).
    REQUIRE(py::hasattr(pa, "AgentInfo"));
    py::object info = pa.attr("AgentInfo")("moonraker", "Moonraker", "1.0", "test agent");
    CHECK(info.attr("id").cast<std::string>() == "moonraker");
    CHECK(info.attr("name").cast<std::string>() == "Moonraker");

    // FilamentSyncMode enum the host queries to pick pull vs subscription.
    REQUIRE(py::hasattr(pa, "FilamentSyncMode"));
    py::object mode = pa.attr("FilamentSyncMode");
    CHECK(py::hasattr(mode, "Pull"));
    CHECK(py::hasattr(mode, "Subscription"));
    CHECK(py::hasattr(mode, "None_"));

    REQUIRE(py::hasattr(pa, "CameraStreamMode"));
    py::object camera_mode = pa.attr("CameraStreamMode");
    CHECK(py::hasattr(camera_mode, "HTTPS"));
    CHECK_FALSE(py::hasattr(camera_mode, "WebRTC"));

    // Plugin-type enum exposed at module root (host reads it without the GIL).
    CHECK(py::hasattr(orca, "PluginType"));
}

// ===========================================================================
// INTEGRATION - plugin-registration API and discovery-context guards.
// These are the symbols every plugin package uses: the @orca.plugin decorator,
// orca.base, orca.register_capability, and the capability base modules. Checking
// them in the lightweight embedded-interpreter test catches binding breakage
// before the plugin-loader test needs to run.
// ===========================================================================
TEST_CASE("orca plugin-registration API surface + discovery-context guards", "[integration][Python]")
{
    ensure_python_initialized();
    py::gil_scoped_acquire gil;
    py::module_ orca = import_orca_module();

    // Module-level surface every plugin package relies on.
    // note: no "gcode" module here - this branch has no G-code capability module;
    // PostProcessing exists only as a PluginType value.
    for (const char* name : { "plugin", "register_capability", "base", "PythonPluginBase",
                              "PluginType", "PluginResult", "script", "printer_agent", "host" }) {
        CAPTURE(name);
        CHECK(py::hasattr(orca, name));
    }

    // Plugin package base and capability base contract.
    CHECK(py::hasattr(orca.attr("base"), "register_capabilities"));
    py::object cap_base = orca.attr("PythonPluginBase");
    for (const char* method : { "get_name", "get_type", "on_load", "on_unload" }) {
        CAPTURE(method);
        CHECK(py::hasattr(cap_base, method));
    }

    // The script capability module exposes its own base class.
    CHECK(py::hasattr(orca.attr("script"), "ScriptPluginCapabilityBase"));

    // PluginType enum carries the values that route a capability, including PrinterConnection.
    // note: no PostProcessing value on this branch's binding.
    py::object types = orca.attr("PluginType");
    for (const char* value : { "PrinterConnection", "Script" }) {
        CAPTURE(value);
        CHECK(py::hasattr(types, value));
    }

    // note: this is testing behavior, not normal plugin loading.
    // These APIs should only work while Orca is actively loading a plugin.
    try {
        // Calls Python's orca.register_capability(0) from C++.
        // 0 is intentionally bogus. The important part is that there is no active
        // plugin load context, so the function should reject the call immediately.
        orca.attr("register_capability")(py::int_(0));

        // If the call above does NOT throw, the test fails here.
        FAIL("register_capability outside discovery context must raise");
    } catch (const py::error_already_set& error) {
        // pybind11 wraps Python exceptions as py::error_already_set.
        // This checks the Python exception type is ValueError.
        CHECK(error.matches(PyExc_ValueError));
    }

    try {
        // This is the function behind @orca.plugin.
        // Same logic as above.
        orca.attr("plugin")(py::int_(0));

        FAIL("@orca.plugin outside discovery context must raise");
    } catch (const py::error_already_set& error) {
        CHECK(error.matches(PyExc_ValueError));
    }
}

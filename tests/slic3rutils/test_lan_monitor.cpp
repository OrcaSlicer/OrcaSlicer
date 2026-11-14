#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <chrono>
#include <limits>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <slic3r/Utils/LanMonitorSnapshot.hpp>
#include <slic3r/Utils/BambuCameraSource.hpp>
#include <slic3r/Utils/PrinterNetworkTypes.hpp>
#include <slic3r/Utils/JsonReport.hpp>
#include <slic3r/Utils/LanMonitorSession.hpp>

using Slic3r::LanMonitorSnapshot;
using Slic3r::merge_json_report;
using json = nlohmann::json;

TEST_CASE("LAN monitoring keeps each printer's job telemetry independent", "[LanMonitor]")
{
    LanMonitorSnapshot first, second;
    first.update({{"print", {{"subtask_name", "First"}, {"gcode_state", "RUNNING"}, {"mc_percent", 26}, {"mc_remaining_time", 80}}}});
    second.update({{"print", {{"subtask_name", "Second"}, {"gcode_state", "FINISH"}, {"mc_percent", 100}, {"mc_remaining_time", 0}}}});
    first.update({{"print", {{"mc_percent", 27}}}});
    CHECK(first.job_name == "First");
    REQUIRE(first.remaining_seconds);
    CHECK(*first.remaining_seconds == 4800);
    REQUIRE(first.progress);
    CHECK(*first.progress == 27);
    CHECK(second.print_state == "FINISH");
    REQUIRE(second.progress);
    CHECK(*second.progress == 100);
    REQUIRE(second.remaining_seconds);
    CHECK(*second.remaining_seconds == 0);
}

TEST_CASE("LAN monitoring accepts integer and numeric string estimates", "[LanMonitor]")
{
    const auto value = GENERATE(json(90), json("90"));
    LanMonitorSnapshot snapshot;
    CHECK(snapshot.update({{"print", {{"mc_percent", value}, {"mc_remaining_time", value}}}}));
    REQUIRE(snapshot.progress);
    CHECK(*snapshot.progress == 90);
    REQUIRE(snapshot.remaining_seconds);
    CHECK(*snapshot.remaining_seconds == 5400);
}

TEST_CASE("LAN monitoring preserves valid estimates when a report contains invalid values", "[LanMonitor]")
{
    const auto value = GENERATE(json(nullptr), json(true), json(-1), json(1.5), json("invalid"),
                                json("2minutes"), json(" 2"), json("+2"), json(""),
                                json(std::numeric_limits<std::uint64_t>::max()));
    LanMonitorSnapshot snapshot;
    snapshot.update({{"print", {{"mc_percent", 25}, {"mc_remaining_time", 10}}}});
    snapshot.update({{"print", {{"mc_percent", value}, {"mc_remaining_time", value}}}});
    CHECK(snapshot.progress == std::optional<int>(25));
    CHECK(snapshot.remaining_seconds == std::optional<int>(600));
}

TEST_CASE("LAN monitoring leaves unavailable estimates unknown and bounds their ranges", "[LanMonitor]")
{
    LanMonitorSnapshot snapshot;
    CHECK_FALSE(snapshot.progress);
    CHECK_FALSE(snapshot.remaining_seconds);
    CHECK_FALSE(snapshot.update(json::array()));
    CHECK_FALSE(snapshot.update({{"print", nullptr}}));
    snapshot.update({{"print", {{"mc_percent", 101}, {"mc_remaining_time", std::numeric_limits<int>::max()}}}});
    CHECK_FALSE(snapshot.progress);
    CHECK_FALSE(snapshot.remaining_seconds);
    CHECK(snapshot.to_json()["progress"].is_null());
}

TEST_CASE("LAN monitoring clears the previous job's estimates when job identity changes", "[LanMonitor]")
{
    LanMonitorSnapshot snapshot;
    snapshot.update({{"print", {{"subtask_id", "first"}, {"subtask_name", "First"}, {"mc_percent", 90}, {"mc_remaining_time", 1}}}});
    snapshot.update({{"print", {{"subtask_id", "second"}, {"gcode_state", "PREPARE"}}}});
    CHECK(snapshot.job_id == "second");
    CHECK(snapshot.job_name.empty());
    CHECK(snapshot.print_state == "PREPARE");
    CHECK_FALSE(snapshot.progress);
    CHECK_FALSE(snapshot.remaining_seconds);
}

TEST_CASE("LAN reports become stale after thirty seconds independently", "[LanMonitor]")
{
    using Status = Slic3r::LanMonitorStatus;
    using namespace std::chrono_literals;
    const auto now = Status::Clock::now();
    Status first, second;
    first.state = second.state = Status::State::Online;
    CHECK_FALSE(first.live(now));
    const json sample = {{"reports", 1}, {"age_seconds", 0},
                         {"status", {{"subtask_name", "Part"}, {"progress", 50}, {"remaining_seconds", 120}}}};
    REQUIRE(first.accept_sample(sample, now));
    REQUIRE(second.accept_sample(sample, now + 20s));
    CHECK(first.live(now + 30s));
    CHECK_FALSE(first.live(now + 31s));
    CHECK(second.live(now + 31s));
    first.state = Status::State::AuthenticationError;
    CHECK_FALSE(first.live(now));
    CHECK(first.job.remaining_seconds == std::optional<int>(120));
}

TEST_CASE("LAN jobs with a reused subtask id clear the previous job's estimates", "[LanMonitor]")
{
    LanMonitorSnapshot snapshot;
    snapshot.update({{"print", {{"subtask_id", "0"}, {"subtask_name", "First"}, {"mc_percent", 100}, {"mc_remaining_time", 0}}}});
    snapshot.update({{"print", {{"subtask_id", "0"}, {"subtask_name", "Second"}, {"gcode_state", "PREPARE"}}}});
    CHECK(snapshot.job_name == "Second");
    CHECK(snapshot.print_state == "PREPARE");
    CHECK_FALSE(snapshot.progress);
    CHECK_FALSE(snapshot.remaining_seconds);
}

TEST_CASE("Malformed worker samples cannot replace valid printer telemetry", "[LanMonitor]")
{
    Slic3r::LanMonitorStatus status;
    const auto invalid = GENERATE(json(nullptr), json::array(), json{{"reports", 0}, {"age_seconds", 0}, {"status", json::object()}},
        json{{"reports", 1}, {"age_seconds", -1}, {"status", json::object()}},
        json{{"reports", 1}, {"age_seconds", 0}, {"status", {{"gcode_state", false}}}});
    status.job.job_name = "Keep";
    CHECK_FALSE(status.accept_sample(invalid));
    CHECK(status.job.job_name == "Keep");
    CHECK_FALSE(status.received);
}

TEST_CASE("Report deltas retain omitted values and replace arrays and nulls", "[LanMonitor]")
{
    json report = {{"print", {{"subtask_id", "first"}, {"mc_percent", 2}, {"mc_remaining_time", 20}, {"ams", json::array({1, 2})}}}};
    merge_json_report({{"print", {{"mc_percent", 3}, {"ams", json::array({4})}, {"new", nullptr}}}}, report);
    LanMonitorSnapshot snapshot;
    snapshot.update(report);
    CHECK(snapshot.progress == std::optional<int>(3));
    CHECK(snapshot.remaining_seconds == std::optional<int>(1200));
    CHECK(report["print"]["ams"] == json::array({4}));
    CHECK(report["print"]["new"].is_null());
}

TEST_CASE("LAN camera sources keep the target and credentials scoped to each printer", "[LanMonitor]")
{
    Slic3r::BambuCameraSource first, second;
    first.host = "192.0.2.1";
    first.password = "first";
    first.device = "one";
    second.host = "192.0.2.2";
    second.password = "second";
    second.device = "two";
    CHECK(first.url().find("192.0.2.1.?port=6000&user=bblp&passwd=first") != std::string::npos);
    CHECK(first.url().find("&device=one") != std::string::npos);
    CHECK(second.url().find("&device=two") != std::string::npos);
    CHECK(second.url().find("first") == std::string::npos);
    second.protocol = Slic3r::LVL_Disable;
    CHECK(second.url().empty());
}

TEST_CASE("Worker callbacks stay with their session and expire when it is replaced or removed", "[LanMonitor]")
{
    Slic3r::LanMonitorSession first, second;
    const json online = {{"kind", "connection"}, {"state", 0}};
    first.consume(online, first.generation);
    CHECK(first.status.state == Slic3r::LanMonitorStatus::State::Online);
    CHECK(second.status.state == Slic3r::LanMonitorStatus::State::Connecting);
    const auto old_generation = first.generation++;
    first.consume({{"kind", "connection"}, {"state", 1}, {"reason", "authentication"}}, old_generation);
    first.finished(old_generation);
    CHECK(first.status.state == Slic3r::LanMonitorStatus::State::Online);
    CHECK(first.backoff == 1);
    first.active = false;
    first.consume({{"kind", "error"}}, first.generation);
    CHECK(first.status.state == Slic3r::LanMonitorStatus::State::Online);
}

TEST_CASE("Camera metadata arriving during startup preserves the active connection", "[LanMonitor]")
{
    Slic3r::BambuCameraSource before;
    before.host = "192.0.2.1";
    before.password = "paired-code";
    before.device = "printer";
    auto initialized = before;
    initialized.device_version = "01.08.00";
    initialized.network_version = "02.03.00.62";
    initialized.client_id = "client";
    initialized.client_version = "2.4.0";
    CHECK(before.url() != initialized.url());
    CHECK(before.same_connection(initialized));
}

TEST_CASE("Camera target and credential changes replace the active connection", "[LanMonitor]")
{
    Slic3r::BambuCameraSource source;
    source.host = "192.0.2.1";
    source.password = "paired-code";
    source.device = "printer";
    const auto field = GENERATE(0, 1, 2, 3, 4);
    auto changed = source;
    if (field == 0) changed.host = "192.0.2.2";
    if (field == 1) changed.password = "new-code";
    if (field == 2) changed.device = "other-printer";
    if (field == 3) changed.user = "other-user";
    if (field == 4) changed.protocol = Slic3r::LVL_Rtsps;
    CHECK_FALSE(source.same_connection(changed));
}

TEST_CASE("Transient worker failures back off independently and authentication requires retry", "[LanMonitor]")
{
    using Session = Slic3r::LanMonitorSession;
    using State = Slic3r::LanMonitorStatus::State;
    using namespace std::chrono_literals;
    Session first, second;
    const auto now = Session::Clock::now();
    for (int attempt = 0; attempt < 8; ++attempt) first.finished(first.generation, now);
    CHECK(first.retry_at == now + 30s);
    CHECK(second.backoff == 1);
    CHECK(second.status.state == State::Connecting);
    second.consume({{"kind", "connection"}, {"state", 1}, {"reason", "authentication"}}, second.generation);
    second.finished(second.generation, now);
    CHECK(second.status.state == State::AuthenticationError);
    second.consume({{"kind", "connection"}, {"state", "invalid"}}, second.generation);
    CHECK(second.status.state == State::AuthenticationError);
}

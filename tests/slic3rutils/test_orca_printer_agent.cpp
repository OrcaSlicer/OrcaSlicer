#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <libslic3r/ProjectTask.hpp>
#include <slic3r/GUI/FilamentMappingUtils.hpp>
#include <slic3r/Utils/IPrinterAgent.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp>
#include <slic3r/Utils/OrcaPrinterAgent.hpp>

#include "catch2/matchers/catch_matchers.hpp"
#include "orca_mqtt_mock_broker.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <slic3r/Utils/bambu_networking.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Slic3r::OrcaPrinterAgent;

namespace {
// Probe exposes the protected internals the tests drive.
struct Probe : OrcaPrinterAgent {
    using OrcaPrinterAgent::OrcaPrinterAgent;
    using OrcaPrinterAgent::deliver_to_sink;
    using OrcaPrinterAgent::parse_lan_endpoint;
    using OrcaPrinterAgent::make_lan_client_id;
    using OrcaPrinterAgent::lan_connection_target;
    using OrcaPrinterAgent::build_filament_mapping;
    using OrcaPrinterAgent::build_gcode_file_payload;
    using OrcaPrinterAgent::prepare_outgoing_request;
    using OrcaPrinterAgent::parse_file_list;
    using OrcaPrinterAgent::parse_thumbnail_path;
    using OrcaPrinterAgent::encode_file_path;
    using OrcaPrinterAgent::parse_file_metadata;

    using OrcaPrinterAgent::FileTransport;
    using OrcaPrinterAgent::preferred_file_transport;
    using OrcaPrinterAgent::build_files_list_request;
    using OrcaPrinterAgent::build_files_metadata_request;
    using OrcaPrinterAgent::build_files_delete_request;
    using OrcaPrinterAgent::build_files_thumbnail_request;
    using OrcaPrinterAgent::parse_opcp_file_list;
    using OrcaPrinterAgent::parse_opcp_file_metadata;
    using OrcaPrinterAgent::parse_opcp_thumbnail;
    using OrcaPrinterAgent::file_reply_requires_http_fallback;
};

// Shared helpers for the OPCP file-transport tests below.
bool wait_for(const std::function<bool()>& predicate, int attempts = 400, int sleep_ms = 10)
{
    for (int i = 0; i < attempts; ++i) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    return predicate();
}

// First recorded files.<command> request, optionally narrowed to one path.
std::string find_files_request(const orca_mqtt_test::MockBroker& broker, const std::string& command, const std::string& path)
{
    for (const std::string& request : broker.received_requests()) {
        if (request.find("\"files\"") == std::string::npos)
            continue;
        if (request.find("\"command\":\"" + command + "\"") == std::string::npos)
            continue;
        if (!path.empty() && request.find("\"path\":\"" + path + "\"") == std::string::npos)
            continue;
        return request;
    }
    return {};
}

std::string request_sequence_id(const std::string& request)
{
    const nlohmann::json parsed = nlohmann::json::parse(request, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return {};
    const auto files_it = parsed.find("files");
    if (files_it == parsed.end() || !files_it->is_object())
        return {};
    return files_it->value("sequence_id", std::string());
}

std::string files_list_reply(const std::string& sequence_id, const std::string& entries_json, bool truncated = false)
{
    return R"({"files":{"command":"list","sequence_id":")" + sequence_id + R"(","result":"success","errno":0,"entries":)" +
           entries_json + R"(,"truncated":)" + (truncated ? "true" : "false") + R"(}})";
}

bool connect_and_subscribe(Probe& agent, const orca_mqtt_test::MockBroker& broker, const std::string& dev_id)
{
    const auto endpoint = broker.host_port();
    agent.connect_printer(
        Slic3r::PrinterConnectionParams{dev_id, endpoint.first + ":" + endpoint.second, "orcasonar", "code", "", false, ""});
    return wait_for([&] { return broker.connect_count() >= 1 && broker.is_subscribed("device/" + dev_id + "/report"); });
}

struct ListOutcome {
    std::mutex                            mutex;
    int                                   result = 999;
    std::vector<Slic3r::PrinterFileEntry> files;
};

const char* const kFilesListCapabilities =
    R"({"info":{"command":"get_capabilities","supported_commands":["files.list"],"capabilities":{"protocol":{"supported_commands":[]}}}})";
}

TEST_CASE("OrcaPrinterAgent forwards a status payload to on_message_fn", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    std::string got_id, got_payload;
    agent.set_on_message_fn([&](std::string id, std::string p){ got_id = std::move(id); got_payload = std::move(p); });
    agent.deliver_to_sink("dev-1", R"({"print":{"command":"push_status"}})", /*local=*/false);
    CHECK(got_id == "dev-1");
    CHECK(got_payload.find("push_status") != std::string::npos);
}

TEST_CASE("OrcaPrinterAgent stamps the get_capabilities nozzle diameter onto push_status frames", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    std::string last_payload;
    agent.set_on_message_fn([&](std::string, std::string p){ last_payload = std::move(p); });

    // Before any capabilities reply, a push_status frame is forwarded untouched.
    agent.deliver_to_sink("dev-1", R"({"print":{"command":"push_status","mc_percent":10}})", /*local=*/false);
    CHECK(last_payload.find("nozzle_diameter") == std::string::npos);

    // The get_capabilities reply is forwarded verbatim; its topology nozzle diameter
    // is cached for the device.
    agent.deliver_to_sink(
        "dev-1",
        R"({"info":{"command":"get_capabilities","capabilities":{"topology":{"tools":[{"id":"T0","nozzle":{"diameter_mm":0.4}}]}}}})",
        /*local=*/false);
    CHECK(last_payload.find("\"command\":\"get_capabilities\"") != std::string::npos);
    CHECK(last_payload.find("\"print\"") == std::string::npos);

    // Later push_status frames for that device get the cached diameter plus a neutral
    // nozzle_type, so MachineObject::parse_json's legacy nozzle parser can run.
    agent.deliver_to_sink("dev-1", R"({"print":{"command":"push_status","mc_percent":20}})", /*local=*/false);
    CHECK(last_payload.find("\"nozzle_diameter\":0.4") != std::string::npos);
    CHECK(last_payload.find("\"nozzle_type\":\"N/A\"") != std::string::npos);

    // A different device is unaffected.
    agent.deliver_to_sink("dev-2", R"({"print":{"command":"push_status"}})", /*local=*/false);
    CHECK(last_payload.find("nozzle_diameter") == std::string::npos);

    // A frame that already carries real nozzle data is not overridden.
    agent.deliver_to_sink("dev-1", R"({"print":{"command":"push_status","nozzle_diameter":0.6}})", /*local=*/false);
    CHECK(last_payload.find("\"nozzle_diameter\":0.6") != std::string::npos);
    CHECK(last_payload.find("N/A") == std::string::npos);
}

TEST_CASE("OrcaPrinterAgent owns connector capabilities and fails closed for AMS commands", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    agent.deliver_to_sink("orca-caps", R"({
        "info": {
            "command": "get_capabilities",
            "supported_features": {"fms": true, "filament_slots": true, "filament_mapping": true},
            "supported_commands": ["print.ams_get_rfid"],
            "capabilities": {
                "protocol": {
                    "features": {"filament_mapping": true},
                    "supported_commands": ["print.ams_change_filament"]
                }
            }
        }
    })", false);

    CHECK(agent.supports_feature("orca-caps", "filament_mapping"));
    CHECK(agent.supports_command("orca-caps", "print.ams_filament_setting"));
    CHECK(agent.supports_command("orca-caps", "print.ams_change_filament"));
    CHECK(agent.supports_command("orca-caps", "print.ams_get_rfid"));
    CHECK_FALSE(agent.supports_command("orca-caps", "print.ams_control"));

    agent.deliver_to_sink("orca-caps", R"({
        "info": {
            "command": "get_capabilities",
            "supported_features": {"fms": false, "filament_slots": false, "filament_mapping": false},
            "supported_commands": ["print.ams_control"],
            "capabilities": {"protocol": {"features": {}, "supported_commands": []}}
        }
    })", false);

    CHECK_FALSE(agent.supports_feature("orca-caps", "filament_mapping"));
    CHECK_FALSE(agent.supports_command("orca-caps", "print.ams_filament_setting"));
    CHECK_FALSE(agent.supports_command("orca-caps", "print.ams_change_filament"));
    CHECK_FALSE(agent.supports_command("orca-caps", "print.ams_control"));
    CHECK_FALSE(agent.supports_command("orca-caps", "print.ams_calibrate"));
    CHECK(agent.supports_command("orca-caps", "print.gcode_file"));
}

TEST_CASE("OrcaPrinterAgent clears cached capabilities when a device is unbound", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    agent.deliver_to_sink("orca-forget", R"({
        "info": {
            "command": "get_capabilities",
            "supported_features": {"filament_slots": true},
            "supported_commands": [],
            "capabilities": {"protocol": {"features": {}, "supported_commands": []}}
        }
    })", false);
    REQUIRE(agent.supports_command("orca-forget", "print.ams_filament_setting"));

    agent.unbind("orca-forget");
    CHECK_FALSE(agent.supports_command("orca-forget", "print.ams_filament_setting"));
}

TEST_CASE("OrcaPrinterAgent removes setting_id from AMS metadata and gates the request", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    agent.deliver_to_sink("orca-ams-write", R"({
        "info": {
            "command": "get_capabilities",
            "supported_features": {"fms": false, "filament_slots": true},
            "supported_commands": [],
            "capabilities": {"protocol": {"features": {"filament_slots": true}, "supported_commands": []}}
        }
    })", false);

    const std::string request = R"({"print":{"command":"ams_filament_setting","sequence_id":"9","tray_info_idx":"GFL99","setting_id":"preset-setting"}})";
    std::string command;
    std::string prepared;
    CHECK(agent.prepare_outgoing_request("orca-ams-write", request, command, prepared) == BAMBU_NETWORK_SUCCESS);
    CHECK(command == "print.ams_filament_setting");
    const nlohmann::json parsed = nlohmann::json::parse(prepared);
    CHECK_FALSE(parsed["print"].contains("setting_id"));
    CHECK(parsed["print"]["tray_info_idx"] == "GFL99");

    agent.deliver_to_sink("orca-ams-write", R"({
        "info": {
            "command": "get_capabilities",
            "supported_features": {"filament_slots": false},
            "capabilities": {"protocol": {"features": {"filament_slots": false}}}
        }
    })", false);
    CHECK(agent.prepare_outgoing_request("orca-ams-write", request, command, prepared) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
}

TEST_CASE("OrcaPrinterAgent::parse_lan_endpoint", "[OrcaPrinterAgent]") {
    std::string h, p;
    REQUIRE(Probe::parse_lan_endpoint("192.168.1.9", h, p));
    CHECK(h == "192.168.1.9"); CHECK(p == "8280");
    REQUIRE(Probe::parse_lan_endpoint("http://host.local:9000/x", h, p));
    CHECK(h == "host.local"); CHECK(p == "9000");
    CHECK_FALSE(Probe::parse_lan_endpoint("", h, p));
}

TEST_CASE("OrcaPrinterAgent::make_lan_client_id is stable and prefixed", "[OrcaPrinterAgent]") {
    const auto a = Probe::make_lan_client_id("dev-1");
    const auto b = Probe::make_lan_client_id("dev-1");
    CHECK(a == b);                                   // drawn once per process
    CHECK(a.rfind("orcaslicer-lan-dev-1-", 0) == 0);
}

TEST_CASE("OrcaPrinterAgent::parse_file_list normalizes Moonraker entries", "[OrcaPrinterAgent]") {
    const std::vector<Slic3r::PrinterFileEntry> files = Probe::parse_file_list(R"({
        "result": [
            {"path": "sub/foo.gcode", "modified": 1700000000.75, "size": 1234, "permissions": "rw"},
            {"path": "bar.gcode", "modified": 42, "size": 7}
        ]
    })");
    REQUIRE(files.size() == 2);
    CHECK(files[0].path == "sub/foo.gcode");
    CHECK(files[0].name == "foo.gcode");
    CHECK(files[0].size == 1234);
    CHECK(files[0].modified == 1700000000);   // fractional seconds truncate
    CHECK(files[1].path == "bar.gcode");
    CHECK(files[1].name == "bar.gcode");
    CHECK(files[1].size == 7);
    CHECK(files[1].modified == 42);
}

TEST_CASE("OrcaPrinterAgent::parse_file_list handles an empty result", "[OrcaPrinterAgent]") {
    CHECK(Probe::parse_file_list(R"({"result": []})").empty());
}

TEST_CASE("OrcaPrinterAgent::parse_file_list rejects malformed JSON", "[OrcaPrinterAgent]") {
    CHECK(Probe::parse_file_list("not json").empty());
    CHECK(Probe::parse_file_list(R"({"result": "nope"})").empty());
}

TEST_CASE("OrcaPrinterAgent::parse_file_list skips entries missing fields", "[OrcaPrinterAgent]") {
    const std::vector<Slic3r::PrinterFileEntry> files = Probe::parse_file_list(R"({
        "result": [
            {"size": 5},
            {"path": ""},
            {"path": "kept.gcode"},
            "not-an-object"
        ]
    })");
    REQUIRE(files.size() == 1);
    CHECK(files[0].path == "kept.gcode");
    CHECK(files[0].name == "kept.gcode");
    CHECK(files[0].size == 0);
    CHECK(files[0].modified == 0);
}

TEST_CASE("OrcaPrinterAgent::parse_thumbnail_path picks the largest width", "[OrcaPrinterAgent]") {
    const std::string path = Probe::parse_thumbnail_path(R"({
        "result": [
            {"width": 32, "height": 32, "thumbnail_path": ".thumbs/foo.gcode-32x32.png"},
            {"width": 300, "height": 300, "thumbnail_path": ".thumbs/foo.gcode-300x300.png"},
            {"width": 100, "height": 100, "thumbnail_path": ".thumbs/foo.gcode-100x100.png"}
        ]
    })");
    CHECK(path == ".thumbs/foo.gcode-300x300.png");
}

TEST_CASE("OrcaPrinterAgent::parse_thumbnail_path accepts both key spellings", "[OrcaPrinterAgent]") {
    CHECK(Probe::parse_thumbnail_path(R"({"result": [{"width": 32, "relative_path": ".thumbs/old.png"}]})") == ".thumbs/old.png");
    CHECK(Probe::parse_thumbnail_path(R"({"result": [{"width": 32, "thumbnail_path": ".thumbs/new.png"}]})") == ".thumbs/new.png");
}

TEST_CASE("OrcaPrinterAgent::parse_thumbnail_path handles an empty result", "[OrcaPrinterAgent]") {
    CHECK(Probe::parse_thumbnail_path(R"({"result": []})").empty());
}

TEST_CASE("OrcaPrinterAgent::parse_thumbnail_path rejects malformed JSON", "[OrcaPrinterAgent]") {
    CHECK(Probe::parse_thumbnail_path("not json").empty());
    CHECK(Probe::parse_thumbnail_path(R"({"result": "nope"})").empty());
}

TEST_CASE("OrcaPrinterAgent::parse_thumbnail_path skips an entry without a path", "[OrcaPrinterAgent]") {
    CHECK(Probe::parse_thumbnail_path(R"({"result": [{"width": 300, "height": 300}]})").empty());
    CHECK(Probe::parse_thumbnail_path(R"({"result": [{"width": 300, "thumbnail_path": 7}]})").empty());
}

TEST_CASE("OrcaPrinterAgent::encode_file_path preserves separators and encodes segments", "[OrcaPrinterAgent]") {
    CHECK(Probe::encode_file_path("foo.gcode") == "foo.gcode");                          // flat path
    CHECK(Probe::encode_file_path("sub/foo.gcode") == "sub/foo.gcode");                  // '/' kept as separator
    CHECK(Probe::encode_file_path("a/b/c.gcode") == "a/b/c.gcode");
    CHECK(Probe::encode_file_path("sub dir/my file #1.gcode") == "sub%20dir/my%20file%20%231.gcode");
    CHECK(Probe::encode_file_path("design+part.gcode") == "design%2Bpart.gcode");
}

TEST_CASE("OrcaPrinterAgent::parse_file_metadata parses the Moonraker fields", "[OrcaPrinterAgent]") {
    using Catch::Matchers::WithinAbs;
    const Slic3r::PrinterFileMetadata meta = Probe::parse_file_metadata(R"({
        "result": {
            "filename": "sub/foo.gcode",
            "size": 1234,
            "modified": 1700000000.5,
            "estimated_time": 3725,
            "filament_total": 10500.5,
            "filament_weight_total": 31.6,
            "thumbnails": []
        }
    })");
    CHECK(meta.estimated_time == 3725);
    CHECK_THAT(meta.filament_total, WithinAbs(10500.5, 1e-9));
    CHECK_THAT(meta.filament_weight, WithinAbs(31.6, 1e-9));
}

TEST_CASE("OrcaPrinterAgent::parse_file_metadata defaults missing fields to zero", "[OrcaPrinterAgent]") {
    using Catch::Matchers::WithinAbs;
    const Slic3r::PrinterFileMetadata meta = Probe::parse_file_metadata(R"({"result": {"filename": "foo.gcode"}})");
    CHECK(meta.estimated_time == 0);
    CHECK_THAT(meta.filament_total, WithinAbs(0.0, 1e-12));
    CHECK_THAT(meta.filament_weight, WithinAbs(0.0, 1e-12));
}

TEST_CASE("OrcaPrinterAgent::parse_file_metadata yields zeros for a malformed reply", "[OrcaPrinterAgent]") {
    using Catch::Matchers::WithinAbs;
    const Slic3r::PrinterFileMetadata malformed = Probe::parse_file_metadata("not json");
    CHECK(malformed.estimated_time == 0);
    CHECK_THAT(malformed.filament_total, WithinAbs(0.0, 1e-12));
    CHECK_THAT(malformed.filament_weight, WithinAbs(0.0, 1e-12));

    const Slic3r::PrinterFileMetadata wrong_type = Probe::parse_file_metadata(R"({"result": "nope"})");
    CHECK(wrong_type.estimated_time == 0);
    CHECK_THAT(wrong_type.filament_total, WithinAbs(0.0, 1e-12));
}

TEST_CASE("filament mapping is keyed by the ams_mapping2 array position", "[OrcaPrinterAgent]") {
    const nlohmann::json mapping = Probe::build_filament_mapping(
        R"([{"ams_id":1,"slot_id":5},{"ams_id":255,"slot_id":255},{"ams_id":255,"slot_id":0}])");
    REQUIRE(mapping.is_array());
    REQUIRE(mapping.size() == 2);
    CHECK(mapping[0]["filament_index"] == 0);
    CHECK(mapping[0]["ams_id"] == 1);
    CHECK(mapping[0]["slot_id"] == 5);
    // The unmatched middle entry is dropped; the third entry keeps index 2.
    CHECK(mapping[1]["filament_index"] == 2);
    CHECK(mapping[1]["ams_id"] == 255);
    CHECK(mapping[1]["slot_id"] == 0);
}

// Index-correlation merge gate (plan PR 3). `ams_mapping2` is built one entry
// per logical filament, so its array position is the identifier the generated
// G-code toolchange passes to the Klipper macro (`next_filament_id`). The
// serializer must key `filament_index` by that position and must never
// re-densify after dropping unused/sentinel entries, or a used filament would
// be aimed at the wrong lane. This test covers the plan's matrix: preset order
// differing from used order, a middle filament unused, and external slots.
TEST_CASE("filament mapping index correlates with the ams_mapping2 position", "[OrcaPrinterAgent]") {
    // Positions 0..4. Used filaments are 0, 2 and 4; 1 and 3 are unused.
    const nlohmann::json mapping = Probe::build_filament_mapping(
        R"([{"ams_id":0,"slot_id":1},{"ams_id":255,"slot_id":255},{"ams_id":2,"slot_id":3},{"ams_id":255,"slot_id":255},{"ams_id":255,"slot_id":0}])");
    REQUIRE(mapping.size() == 3);
    CHECK(mapping[0]["filament_index"] == 0);
    CHECK(mapping[1]["filament_index"] == 2);
    CHECK(mapping[2]["filament_index"] == 4); // external slot keeps its position
    CHECK(mapping[2]["ams_id"] == 255);
    CHECK(mapping[2]["slot_id"] == 0);
    // No re-densification: a used filament after a dropped sentinel keeps its
    // original logical index.
    for (const auto& entry : mapping)
        CHECK(entry.contains("filament_index"));
}

TEST_CASE("filament mapping is omitted when nothing remains", "[OrcaPrinterAgent]") {
    CHECK(Probe::build_filament_mapping("").empty());
    CHECK(Probe::build_filament_mapping(R"([{"ams_id":255,"slot_id":255}])").empty());
    CHECK(Probe::build_filament_mapping("not json").empty());
    CHECK(Probe::build_filament_mapping(R"({"ams_id":1,"slot_id":0})").empty()); // not an array
}

// The GUI capability gate and the agent serializer must classify the same
// entries as engaged. External slots ({255,0}/{254,0}) are normalized as-is, so
// they engage; only the {255,255} unmatched sentinel is dropped. A mismatch lets
// an entry past the GUI and refused late with a generic publish error.
TEST_CASE("the GUI mapping gate engages exactly the entries the serializer sends", "[OrcaPrinterAgent]") {
    using Slic3r::GUI::has_engaged_filament_mapping;
    CHECK_FALSE(has_engaged_filament_mapping(""));
    CHECK_FALSE(has_engaged_filament_mapping("[]"));
    CHECK_FALSE(has_engaged_filament_mapping("not json"));
    CHECK_FALSE(has_engaged_filament_mapping(R"([{"ams_id":255,"slot_id":255}])"));
    CHECK(has_engaged_filament_mapping(R"([{"ams_id":255,"slot_id":0}])"));   // external main
    CHECK(has_engaged_filament_mapping(R"([{"ams_id":254,"slot_id":0}])"));   // external deputy
    CHECK(has_engaged_filament_mapping(R"([{"ams_id":0,"slot_id":0}])"));     // box slot
    CHECK(has_engaged_filament_mapping(R"([{"ams_id":255,"slot_id":255},{"ams_id":1,"slot_id":2}])"));

    for (const char* s : {"", "[]", "not json", R"([{"ams_id":255,"slot_id":255}])",
                          R"([{"ams_id":255,"slot_id":0}])", R"([{"ams_id":254,"slot_id":0}])",
                          R"([{"ams_id":0,"slot_id":0}])",
                          R"([{"ams_id":255,"slot_id":255},{"ams_id":1,"slot_id":2}])"}) {
        CHECK(has_engaged_filament_mapping(s) == !Probe::build_filament_mapping(s).empty());
    }
}

// The used-unmapped refusal reads m_ams_mapping_result: an entry with no target carries
// empty ams_id/slot_id, while an external-spool assignment (ams_id 255/254) is a real
// target. A wholly unmapped print reports no target and falls to the existing send flow.
TEST_CASE("used-filament targets split mapped from unmapped", "[OrcaPrinterAgent]") {
    using Slic3r::GUI::has_any_mapped_target;
    using Slic3r::GUI::has_used_filament_without_target;

    Slic3r::FilamentInfo box;      box.ams_id      = "0";   box.slot_id = "1";   // box slot
    Slic3r::FilamentInfo external; external.ams_id = "255"; external.slot_id = "0"; // external spool
    Slic3r::FilamentInfo unmapped;                                                // no target

    CHECK_FALSE(has_used_filament_without_target({box, external}));
    CHECK(has_used_filament_without_target({box, unmapped}));
    CHECK(has_used_filament_without_target({external, unmapped}));
    CHECK(has_used_filament_without_target({unmapped}));   // wholly unmapped: all-invalid flow, not this refusal
    CHECK(has_any_mapped_target({box, unmapped}));
    CHECK(has_any_mapped_target({external, unmapped}));
    CHECK_FALSE(has_any_mapped_target({unmapped}));
}

// A device with no AMS units has one source, the external spool. OrcaSlicer's
// auto-mapping force-selects it for every filament; that is not a lane choice, so it
// must not gate the print or reach print.gcode_file.
TEST_CASE("a no-AMS device drops the forced external-spool selection", "[OrcaPrinterAgent]") {
    using Slic3r::GUI::drop_forced_external_selection;
    using Slic3r::GUI::has_engaged_filament_mapping;

    std::string external_only = R"([{"ams_id":255,"slot_id":0}])";
    drop_forced_external_selection(/*device_has_ams=*/false, external_only);
    CHECK(external_only.empty());
    CHECK_FALSE(has_engaged_filament_mapping(external_only));
    CHECK(Probe::build_filament_mapping(external_only).empty());

    std::string box_mapping = R"([{"ams_id":0,"slot_id":2}])";
    drop_forced_external_selection(/*device_has_ams=*/true, box_mapping);
    CHECK(box_mapping == R"([{"ams_id":0,"slot_id":2}])");
    CHECK(has_engaged_filament_mapping(box_mapping));
}

// An empty mapping must leave the gcode_file payload byte-identical to today:
// exactly command, sequence_id and param, with no filament_mapping key.
TEST_CASE("gcode_file payload omits filament_mapping when the map is empty", "[OrcaPrinterAgent]") {
    const nlohmann::json empty = Probe::build_gcode_file_payload("7", "job.gcode", nlohmann::json::array());
    REQUIRE(empty.contains("print"));
    CHECK(empty["print"].size() == 3);
    CHECK(empty["print"]["command"] == "gcode_file");
    CHECK(empty["print"]["sequence_id"] == "7");
    CHECK(empty["print"]["param"] == "job.gcode");
    CHECK_FALSE(empty["print"].contains("filament_mapping"));

    const nlohmann::json mapping = Probe::build_filament_mapping(R"([{"ams_id":1,"slot_id":0},{"ams_id":255,"slot_id":255}])");
    const nlohmann::json with    = Probe::build_gcode_file_payload("8", "job.gcode", mapping);
    REQUIRE(with["print"].contains("filament_mapping"));
    REQUIRE(with["print"]["filament_mapping"].size() == 1);
    CHECK(with["print"]["filament_mapping"][0]["filament_index"] == 0);
}

// The defensive gate: a mapped print is refused before anything is published when the
// connector never advertised filament_mapping. The GUI send gates make this visible first;
// this covers callers that bypass them (calibration, plugin). A sentinel-only mapping does
// not engage the gate and falls through to the normal publish path.
TEST_CASE("an engaged mapping is refused when the connector never advertised filament_mapping", "[OrcaPrinterAgent]") {
    OrcaPrinterAgent agent("/tmp");
    Slic3r::PrintParams params;
    params.dev_id       = "dev-no-mapping-cap";
    params.dst_file     = "/tmp/job.gcode";
    params.ams_mapping2 = R"([{"ams_id":0,"slot_id":2}])";

    CHECK(agent.start_sdcard_print(params, {}, {}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);

    params.ams_mapping2 = R"([{"ams_id":255,"slot_id":255}])";
    CHECK(agent.start_sdcard_print(params, {}, {}) == BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED);
}

// The FTP "send with record" transport does not exist on OrcaSonar. It must
// report a non-success result so PrintJob falls back to start_print() rather
// than treating a print that was never sent as successful.
TEST_CASE("start_local_print_with_record never reports silent success", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    Slic3r::PrintParams params;
    const int rc = agent.start_local_print_with_record(params, {}, {}, {});
    CHECK(rc < 0);
}

TEST_CASE("connect_printer wires up a LAN Config", "[OrcaPrinterAgent][.integration]") {
    Probe agent("/tmp");
    Slic3r::PrinterConnectionParams params{
        "dev-1", "10.255.255.1", "orcasonar", "code", "", false, ""
    };
    const int rc = agent.connect_printer(params);
    CHECK(rc == BAMBU_NETWORK_SUCCESS);
    CHECK(agent.lan_connection_target() == "ws://10.255.255.1:8280/mqtt");
    CHECK(agent.get_user_selected_machine().empty());   // LAN path must not touch the cloud selection
    agent.disconnect_printer();
}

TEST_CASE("post-connect sequence is subscribe then 4 requests in order", "[OrcaPrinterAgent]") {
    struct SeqProbe : OrcaPrinterAgent {
        using OrcaPrinterAgent::OrcaPrinterAgent;
        std::vector<std::string> calls;
        void emit_connect_sequence(const std::string& dev_id,
            std::function<void(const std::string&)> /*sub*/,
            std::function<void(const std::string&)> /*req*/) override {
            OrcaPrinterAgent::emit_connect_sequence(dev_id,
                [&](const std::string& id){ calls.push_back("sub:" + id); },
                [&](const std::string& body){ calls.push_back(body); });
        }
    } probe("/tmp");
    probe.run_connect_sequence_for_test("dev-1");
    REQUIRE(probe.calls.size() == 5);
    CHECK(probe.calls[0] == "sub:dev-1");
    CHECK(probe.calls[1].find("\"pushing\"") != std::string::npos);
    CHECK(probe.calls[1].find("\"start\"")   != std::string::npos);
    CHECK(probe.calls[2].find("pushall")     != std::string::npos);
    CHECK(probe.calls[3].find("get_version") != std::string::npos);
    CHECK(probe.calls[4].find("get_capabilities") != std::string::npos);
    for (auto& c : probe.calls)
        if (auto pos = c.find("sequence_id"); pos != std::string::npos)
            CHECK(c.substr(pos).find("\"2") != std::string::npos);
}

// Hidden: spawns the connect worker and attempts a real (failing) connect.
TEST_CASE("selecting a cloud printer configures the fleet socket", "[OrcaPrinterAgent][.integration]") {
    auto cloud = std::make_shared<Slic3r::OrcaCloudServiceAgent>("/tmp");
    cloud->set_api_base_url("api.example.com");
    OrcaPrinterAgent agent("/tmp");
    agent.set_cloud_agent(cloud);

    agent.set_user_selected_machine("printer-uuid-1");
    // The configure runs on the connect worker; poll rather than racing it.
    std::string url;
    for (int i = 0; i < 300; ++i) {
        url = cloud->selected_printer_mqtt_url();
        if (!url.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(url == "wss://api.example.com/api/v1/printers/mqtt");

    agent.set_user_selected_machine("");     // selection changes do not tear down the fleet socket
    CHECK(cloud->selected_printer_mqtt_url() == "wss://api.example.com/api/v1/printers/mqtt");
}

TEST_CASE("a stale-generation inbound message is dropped", "[OrcaPrinterAgent]") {
    struct GenProbe : OrcaPrinterAgent {
        using OrcaPrinterAgent::OrcaPrinterAgent;
        using OrcaPrinterAgent::make_lan_message_handler;   // expose for the test
    };
    GenProbe agent("/tmp");
    int hits = 0;
    agent.set_on_message_fn([&](std::string, std::string){ ++hits; });
    auto handler_gen1 = agent.make_lan_message_handler(/*generation=*/1);
    // m_lan_generation starts at 0; two bumps -> 2, so the epoch-1 handler is stale.
    agent.bump_lan_generation_for_test();
    agent.bump_lan_generation_for_test();
    handler_gen1("dev-1", "{}");                            // late callback from gen 1
    CHECK(hits == 0);
}

TEST_CASE("connect_server does not start an MQTT socket", "[OrcaCloud]") {
    auto cloud = std::make_shared<Slic3r::OrcaCloudServiceAgent>("/tmp");
    cloud->set_api_base_url("127.0.0.1:1");     // no session -> connect_server short-circuits before any probe
    cloud->connect_server();
    REQUIRE(cloud->get_mqtt_connection() != nullptr);       // created in the ctor
    CHECK_FALSE(cloud->get_mqtt_connection()->is_running()); // never started
    CHECK(cloud->selected_printer_mqtt_url().empty());
}

TEST_CASE("send_message* reject when there is no connection", "[OrcaPrinterAgent]") {
    OrcaPrinterAgent agent("/tmp");   // no cloud agent, no LAN connection
    CHECK(agent.send_message("d", "{}", 0, 0)            == BAMBU_NETWORK_ERR_INVALID_HANDLE);
    CHECK(agent.send_message_to_printer("d", "{}", 0, 0) == BAMBU_NETWORK_ERR_INVALID_HANDLE);
    CHECK(agent.send_message("", "{}", 0, 0)             == BAMBU_NETWORK_ERR_INVALID_HANDLE);   // empty dev_id
}

TEST_CASE("send_message_to_printer publishes on the LAN connection", "[OrcaPrinterAgent]") {
    orca_mqtt_test::MockBroker broker;
    OrcaPrinterAgent agent("/tmp");
    const auto ep = broker.host_port();
    agent.connect_printer(Slic3r::PrinterConnectionParams{"dev-1", ep.first + ":" + ep.second, "orcasonar", "code", "", false, ""});

    for (int i = 0; i < 150 && broker.connect_count() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(broker.connect_count() >= 1);

    CHECK(agent.send_message_to_printer("dev-1", R"({"print":{"command":"pause","sequence_id":"20007"}})", 0, 0)
          == BAMBU_NETWORK_SUCCESS);

    // on_connected also publishes 4 requests; poll until "pause" specifically shows up.
    bool saw_pause = false;
    for (int i = 0; i < 150 && !saw_pause; ++i) {
        for (const auto& r : broker.received_requests())
            if (r.find("pause") != std::string::npos) { saw_pause = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(saw_pause);
    agent.disconnect_printer();
}

TEST_CASE("destroying an agent mid-connect does not hang or crash", "[OrcaPrinterAgent]") {
    for (int i = 0; i < 20; ++i) {
        auto agent = std::make_unique<OrcaPrinterAgent>("/tmp");
        agent->connect_printer(Slic3r::PrinterConnectionParams{"dev-1", "127.0.0.1:1", "orcasonar", "code", "", false, ""});  // nothing listening: instant ECONNREFUSED
        agent.reset();   // ~OrcaPrinterAgent must stop the conn, join the thread, and not hang/crash
    }
    SUCCEED();
}

// ============================================================================
// OPCP printer-file transport
// ============================================================================

TEST_CASE("the file transport follows the advertised files.list capability", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");

    // No manifest yet: probe OPCP briefly, then fall back to HTTP.
    CHECK(Probe::preferred_file_transport("transport-unknown") == Probe::FileTransport::probe);

    // Manifest advertises files.list: use OPCP.
    agent.deliver_to_sink("transport-opcp", kFilesListCapabilities, true);
    CHECK(Probe::preferred_file_transport("transport-opcp") == Probe::FileTransport::opcp);

    // Manifest is known and lacks files.list: use HTTP.
    agent.deliver_to_sink("transport-http",
        R"({"info":{"command":"get_capabilities","supported_commands":["print.gcode_file"],"capabilities":{"protocol":{"supported_commands":[]}}}})",
        true);
    CHECK(Probe::preferred_file_transport("transport-http") == Probe::FileTransport::http);
}

TEST_CASE("the files.* request builders match the OPCP wire contract", "[OrcaPrinterAgent]") {
    const nlohmann::json list = nlohmann::json::parse(Probe::build_files_list_request("7", ""));
    CHECK(list["files"]["command"] == "list");
    CHECK(list["files"]["sequence_id"] == "7");
    CHECK(list["files"]["root"] == "gcodes");
    CHECK(list["files"]["path"] == "");

    const nlohmann::json metadata = nlohmann::json::parse(Probe::build_files_metadata_request("8", "sub/model.gcode"));
    CHECK(metadata["files"]["command"] == "metadata");
    CHECK(metadata["files"]["root"] == "gcodes");
    CHECK(metadata["files"]["path"] == "sub/model.gcode");

    const nlohmann::json del = nlohmann::json::parse(Probe::build_files_delete_request("9", "sub/model.gcode"));
    CHECK(del["files"]["command"] == "delete");
    CHECK(del["files"]["path"] == "sub/model.gcode");

    const nlohmann::json thumb = nlohmann::json::parse(Probe::build_files_thumbnail_request("10", "sub/model.gcode"));
    CHECK(thumb["files"]["command"] == "thumbnail");
    CHECK(thumb["files"]["max_dimension"] == 0);
    CHECK(thumb["files"]["path"] == "sub/model.gcode");
}

TEST_CASE("parse_opcp_file_list splits files from directories", "[OrcaPrinterAgent]") {
    const auto page = Probe::parse_opcp_file_list(R"({
        "files": {
            "command": "list",
            "sequence_id": "1",
            "result": "success",
            "errno": 0,
            "root": "gcodes",
            "path": "",
            "entries": [
                {"name": "a.gcode", "path": "a.gcode", "is_dir": false, "size": 10, "modified": 100.5},
                {"name": "sub", "path": "sub", "is_dir": true, "size": 0, "modified": 0}
            ],
            "truncated": false
        }
    })");
    CHECK(page.success);
    REQUIRE(page.files.size() == 1);
    CHECK(page.files[0].path == "a.gcode");
    CHECK(page.files[0].name == "a.gcode");
    CHECK(page.files[0].size == 10);
    CHECK(page.files[0].modified == 100);
    REQUIRE(page.dirs.size() == 1);
    CHECK(page.dirs[0] == "sub");
    CHECK_FALSE(page.truncated);
}

TEST_CASE("parse_opcp_file_list reports truncation and failed replies", "[OrcaPrinterAgent]") {
    const auto truncated = Probe::parse_opcp_file_list(R"({
        "files": {"command": "list", "sequence_id": "1", "result": "success", "errno": 0, "entries": [], "truncated": true}
    })");
    CHECK(truncated.success);
    CHECK(truncated.truncated);

    const std::string failed_reply = R"({"files":{"command":"list","sequence_id":"1","result":"error","errno":-19,"entries":[]}})";
    const auto failed = Probe::parse_opcp_file_list(failed_reply);
    CHECK_FALSE(failed.success);
    CHECK(Probe::file_reply_requires_http_fallback(failed_reply));
    CHECK_FALSE(Probe::file_reply_requires_http_fallback(
        R"({"files":{"command":"list","sequence_id":"1","result":"success","errno":0,"entries":[]}})"));
}

TEST_CASE("parse_opcp_file_metadata reads the OPCP fields", "[OrcaPrinterAgent]") {
    using Catch::Matchers::WithinAbs;
    const Slic3r::PrinterFileMetadata meta = Probe::parse_opcp_file_metadata(R"({
        "files": {"command": "metadata", "result": "success", "errno": 0,
                  "estimated_time": 3725, "filament_total": 10500.5, "filament_weight_total": 31.6}
    })");
    CHECK(meta.estimated_time == 3725);
    CHECK_THAT(meta.filament_total, WithinAbs(10500.5, 1e-9));
    CHECK_THAT(meta.filament_weight, WithinAbs(31.6, 1e-9));

    const Slic3r::PrinterFileMetadata empty = Probe::parse_opcp_file_metadata(R"({"files": {"command": "metadata"}})");
    CHECK(empty.estimated_time == 0);
    CHECK_THAT(empty.filament_total, WithinAbs(0.0, 1e-12));
    CHECK_THAT(empty.filament_weight, WithinAbs(0.0, 1e-12));
}

TEST_CASE("parse_opcp_thumbnail decodes the base64 PNG payload", "[OrcaPrinterAgent]") {
    // "aGVsbG8=" is base64 for "hello".
    const std::string decoded = Probe::parse_opcp_thumbnail(
        R"({"files":{"command":"thumbnail","result":"success","errno":0,"width":300,"height":300,"size":5,"data":"aGVsbG8="}})");
    CHECK(decoded == "hello");

    // data absent means the file has no embedded thumbnail.
    CHECK(Probe::parse_opcp_thumbnail(
        R"({"files":{"command":"thumbnail","result":"success","errno":0,"width":300,"height":300,"size":0}})").empty());
    CHECK(Probe::parse_opcp_thumbnail("not json").empty());
}

TEST_CASE("parse_opcp_thumbnail rejects unpadded and malformed base64", "[OrcaPrinterAgent]") {
    // Unpadded: Beast's decoded_size() requires a length multiple of four, so this
    // must be rejected rather than decoded into an undersized buffer.
    CHECK(Probe::parse_opcp_thumbnail(
        R"({"files":{"command":"thumbnail","result":"success","errno":0,"data":"aGVsbG8"}})").empty());
    // Non-alphabet characters must not decode as a partial image (length 8 here,
    // so this exercises the character check rather than the length check).
    CHECK(Probe::parse_opcp_thumbnail(
        R"({"files":{"command":"thumbnail","result":"success","errno":0,"data":"aGVs*G8="}})").empty());
    // Padding is only valid closing the final group.
    CHECK(Probe::parse_opcp_thumbnail(
        R"({"files":{"command":"thumbnail","result":"success","errno":0,"data":"aG=s"}})").empty());
    CHECK(Probe::parse_opcp_thumbnail(
        R"({"files":{"command":"thumbnail","result":"success","errno":0,"data":"===="}})").empty());
    // A correctly padded payload still decodes.
    CHECK(Probe::parse_opcp_thumbnail(
        R"({"files":{"command":"thumbnail","result":"success","errno":0,"data":"aGVsbG8="}})") == "hello");
}

TEST_CASE("list_printer_files parses an OPCP files.list reply", "[OrcaPrinterAgent]") {
    orca_mqtt_test::MockBroker broker;
    Probe                        agent("/tmp");
    const std::string            dev_id = "opcp-list-flat";
    REQUIRE(connect_and_subscribe(agent, broker, dev_id));
    agent.deliver_to_sink(dev_id, kFilesListCapabilities, true);

    auto outcome = std::make_shared<ListOutcome>();
    agent.list_printer_files(dev_id, [outcome](int result, std::vector<Slic3r::PrinterFileEntry> files) {
        std::lock_guard<std::mutex> lock(outcome->mutex);
        outcome->result = result;
        outcome->files  = std::move(files);
    });

    std::string request;
    REQUIRE(wait_for([&] { request = find_files_request(broker, "list", ""); return !request.empty(); }));
    const std::string sequence_id = request_sequence_id(request);
    REQUIRE_FALSE(sequence_id.empty());
    broker.push_report(dev_id, files_list_reply(sequence_id,
        R"([{"name":"a.gcode","path":"a.gcode","is_dir":false,"size":1234,"modified":1700000000.5}])"));

    REQUIRE(wait_for([&] { std::lock_guard<std::mutex> lock(outcome->mutex); return outcome->result != 999; }));
    {
        std::lock_guard<std::mutex> lock(outcome->mutex);
        CHECK(outcome->result == BAMBU_NETWORK_SUCCESS);
        REQUIRE(outcome->files.size() == 1);
        CHECK(outcome->files[0].name == "a.gcode");
        CHECK(outcome->files[0].size == 1234);
        CHECK(outcome->files[0].modified == 1700000000);
    }
    agent.disconnect_printer();
}

TEST_CASE("list_printer_files walks OPCP subdirectories", "[OrcaPrinterAgent]") {
    orca_mqtt_test::MockBroker broker;
    Probe                        agent("/tmp");
    const std::string            dev_id = "opcp-list-recursive";
    REQUIRE(connect_and_subscribe(agent, broker, dev_id));
    agent.deliver_to_sink(dev_id, kFilesListCapabilities, true);

    auto outcome = std::make_shared<ListOutcome>();
    agent.list_printer_files(dev_id, [outcome](int result, std::vector<Slic3r::PrinterFileEntry> files) {
        std::lock_guard<std::mutex> lock(outcome->mutex);
        outcome->result = result;
        outcome->files  = std::move(files);
    });

    // Reply to each files.list request the walker emits: root first, then "sub".
    std::set<std::string> replied;
    const bool            finished = wait_for([&] {
        {
            std::lock_guard<std::mutex> lock(outcome->mutex);
            if (outcome->result != 999)
                return true;
        }
        for (const std::string& request : broker.received_requests()) {
            if (request.find("\"command\":\"list\"") == std::string::npos)
                continue;
            const std::string sequence_id = request_sequence_id(request);
            if (sequence_id.empty() || replied.count(sequence_id) != 0)
                continue;
            replied.insert(sequence_id);

            const nlohmann::json parsed = nlohmann::json::parse(request, nullptr, false);
            const std::string    path   = parsed.is_object() ? parsed["files"].value("path", std::string()) : std::string();
            if (path.empty())
                broker.push_report(dev_id, files_list_reply(sequence_id,
                    R"([{"name":"root.gcode","path":"root.gcode","is_dir":false,"size":11,"modified":100.5},)"
                    R"({"name":"sub","path":"sub","is_dir":true,"size":0,"modified":0}])"));
            else if (path == "sub")
                broker.push_report(dev_id, files_list_reply(sequence_id,
                    R"([{"name":"nested.gcode","path":"sub/nested.gcode","is_dir":false,"size":22,"modified":200.5}])"));
        }
        return false;
    });

    REQUIRE(finished);
    {
        std::lock_guard<std::mutex> lock(outcome->mutex);
        CHECK(outcome->result == BAMBU_NETWORK_SUCCESS);
        REQUIRE(outcome->files.size() == 2);
        CHECK(outcome->files[0].path == "root.gcode");
        CHECK(outcome->files[1].path == "sub/nested.gcode");
        CHECK(outcome->files[1].size == 22);
    }
    agent.disconnect_printer();
}

TEST_CASE("an unanswered OPCP file listing times out and reports failure", "[OrcaPrinterAgent]") {
    orca_mqtt_test::MockBroker broker;
    Probe                        agent("/tmp");
    const std::string            dev_id = "opcp-list-timeout";
    REQUIRE(connect_and_subscribe(agent, broker, dev_id));
    agent.deliver_to_sink(dev_id, kFilesListCapabilities, true);
    agent.set_file_opcp_timeouts_for_test(/*probe=*/150, /*request=*/150);

    auto result = std::make_shared<std::atomic<int>>(999);
    agent.list_printer_files(dev_id, [result](int r, std::vector<Slic3r::PrinterFileEntry>) { result->store(r); });

    REQUIRE(wait_for([&] { return !find_files_request(broker, "list", "").empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let the request wait expire
    agent.disconnect_printer();                                  // free the broker so the HTTP fallback fails fast

    REQUIRE(wait_for([&] { return result->load() != 999; }, 500, 10));
    CHECK(result->load() != BAMBU_NETWORK_SUCCESS);
}

TEST_CASE("disconnect_printer wakes a pending file request", "[OrcaPrinterAgent]") {
    orca_mqtt_test::MockBroker broker;
    Probe                        agent("/tmp");
    const std::string            dev_id = "opcp-list-disconnect";
    REQUIRE(connect_and_subscribe(agent, broker, dev_id));
    agent.deliver_to_sink(dev_id, kFilesListCapabilities, true);
    // Far longer than the assertion window, so only fail_pending_file_requests
    // can wake the worker.
    agent.set_file_opcp_timeouts_for_test(/*probe=*/60000, /*request=*/60000);

    auto result = std::make_shared<std::atomic<int>>(999);
    agent.list_printer_files(dev_id, [result](int r, std::vector<Slic3r::PrinterFileEntry>) { result->store(r); });

    REQUIRE(wait_for([&] { return !find_files_request(broker, "list", "").empty(); }));
    REQUIRE(result->load() == 999); // still waiting before the disconnect

    agent.disconnect_printer();
    REQUIRE(wait_for([&] { return result->load() != 999; }, 200, 10)); // 2s, well under the 60s wait
    CHECK(result->load() != BAMBU_NETWORK_SUCCESS);
}

TEST_CASE("an errno -19 files reply falls back to HTTP", "[OrcaPrinterAgent]") {
    const std::string failed_reply = R"({"files":{"command":"list","sequence_id":"1","result":"error","errno":-19,"entries":[]}})";
    CHECK(Probe::file_reply_requires_http_fallback(failed_reply));
    CHECK_FALSE(Probe::parse_opcp_file_list(failed_reply).success);

    orca_mqtt_test::MockBroker broker;
    Probe                        agent("/tmp");
    const std::string            dev_id = "opcp-list-errno";
    REQUIRE(connect_and_subscribe(agent, broker, dev_id));
    agent.deliver_to_sink(dev_id, kFilesListCapabilities, true);
    agent.set_file_opcp_timeouts_for_test(/*probe=*/500, /*request=*/500);

    auto result = std::make_shared<std::atomic<int>>(999);
    agent.list_printer_files(dev_id, [result](int r, std::vector<Slic3r::PrinterFileEntry>) { result->store(r); });

    std::string request;
    REQUIRE(wait_for([&] { request = find_files_request(broker, "list", ""); return !request.empty(); }));
    const std::string sequence_id = request_sequence_id(request);
    REQUIRE_FALSE(sequence_id.empty());
    broker.push_report(dev_id,
        R"({"files":{"command":"list","sequence_id":")" + sequence_id + R"(","result":"error","errno":-19,"entries":[]}})");
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // let the worker consume the failed reply
    agent.disconnect_printer();                                  // free the broker so the HTTP fallback fails fast

    REQUIRE(wait_for([&] { return result->load() != 999; }, 500, 10));
    CHECK(result->load() != BAMBU_NETWORK_SUCCESS);
}

TEST_CASE("files replies are consumed and never forwarded to the GUI", "[OrcaPrinterAgent]") {
    orca_mqtt_test::MockBroker broker;
    Probe                        agent("/tmp");
    const std::string            dev_id = "opcp-list-routing";
    REQUIRE(connect_and_subscribe(agent, broker, dev_id));
    agent.deliver_to_sink(dev_id, kFilesListCapabilities, true);

    std::atomic<int> forwarded{0};
    agent.set_on_local_message_fn([&forwarded](std::string, std::string) { forwarded.fetch_add(1); });

    auto outcome = std::make_shared<ListOutcome>();
    agent.list_printer_files(dev_id, [outcome](int result, std::vector<Slic3r::PrinterFileEntry> files) {
        std::lock_guard<std::mutex> lock(outcome->mutex);
        outcome->result = result;
        outcome->files  = std::move(files);
    });

    std::string request;
    REQUIRE(wait_for([&] { request = find_files_request(broker, "list", ""); return !request.empty(); }));
    const std::string sequence_id = request_sequence_id(request);
    REQUIRE_FALSE(sequence_id.empty());

    // The matching reply satisfies the pending request and is not forwarded.
    agent.deliver_to_sink(dev_id,
        files_list_reply(sequence_id, R"([{"name":"a.gcode","path":"a.gcode","is_dir":false,"size":1,"modified":1}])"), true);
    REQUIRE(wait_for([&] { std::lock_guard<std::mutex> lock(outcome->mutex); return outcome->result != 999; }));
    CHECK(forwarded.load() == 0);

    // An unmatched files report is dropped too.
    agent.deliver_to_sink(dev_id,
        R"({"files":{"command":"list","sequence_id":"no-such-sequence","result":"success","errno":0,"entries":[]}})"), true);
    CHECK(forwarded.load() == 0);

    // An unrelated report still reaches the sink.
    agent.deliver_to_sink(dev_id, R"({"print":{"command":"push_status"}})", true);
    CHECK(forwarded.load() == 1);

    agent.disconnect_printer();
}

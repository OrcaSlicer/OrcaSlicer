#ifdef _WIN32
// DeviceManager.hpp includes COM headers whose byte typedef must precede std::byte.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "slic3r/Utils/ElegooCC2PrinterAgent.hpp"

#include "fake_mqtt_broker.hpp"
#include "filament_test_collection.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/FilamentSync.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/MqttWebSocketClient.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Slic3r::ElegooCC2PrinterAgent;
using Error      = ElegooCC2PrinterAgent::Error;
using SystemInfo = ElegooCC2PrinterAgent::SystemInfo;
using CanvasTray = ElegooCC2PrinterAgent::CanvasTray;
using CanvasUnit = ElegooCC2PrinterAgent::CanvasUnit;

using filament_test::ecc2_filaments;
using filament_test::FilamentTestCollection;
using filament_test::populate;
using filament_test::ScopedLatinCtype;

namespace {

constexpr const char* cc2_system_info_body = R"json({
  "error_code": 0,
  "system_info": {
    "hardware_version": "",
    "hostname": "Elegoo Centauri Carbon 2",
    "ip": "192.0.2.10",
    "machine_model": "Centauri Carbon 2",
    "protocol_version": "1.0.0",
    "sn": "TESTSN01",
    "software_version": {
      "mcu_version": "00.00.00.00",
      "ota_version": "02.01.01.13",
      "soc_version": ""
    }
  }
})json";

// Captured "result" of method 2005 from an Elegoo Centauri Carbon 2 Combo, firmware 02.01.01.13, slot 0 loaded.
constexpr const char* cc2_canvas_result_fixture = R"json({
  "canvas_info": {
    "active_canvas_id": 0,
    "active_tray_id": -1,
    "auto_refill": false,
    "canvas_list": [
      {
        "canvas_id": 0,
        "connected": 1,
        "tray_list": [
          {
            "brand": "ELEGOO",
            "filament_code": "0x000A",
            "filament_color": "#000000",
            "filament_name": "RAPID PLA+",
            "filament_type": "PLA",
            "max_nozzle_temp": 230,
            "min_nozzle_temp": 190,
            "status": 1,
            "tray_id": 0
          },
          {
            "brand": "",
            "filament_code": "0x0000",
            "filament_color": "",
            "filament_name": "",
            "filament_type": "",
            "max_nozzle_temp": 0,
            "min_nozzle_temp": 0,
            "status": 0,
            "tray_id": 1
          },
          {
            "brand": "",
            "filament_code": "0x0000",
            "filament_color": "",
            "filament_name": "",
            "filament_type": "",
            "max_nozzle_temp": 0,
            "min_nozzle_temp": 0,
            "status": 0,
            "tray_id": 2
          },
          {
            "brand": "",
            "filament_code": "0x0000",
            "filament_color": "",
            "filament_name": "",
            "filament_type": "",
            "max_nozzle_temp": 0,
            "min_nozzle_temp": 0,
            "status": 0,
            "tray_id": 3
          }
        ]
      }
    ]
  },
  "error_code": 0
})json";

nlohmann::json make_tray(const nlohmann::json& tray_id, const nlohmann::json& status = 0, const std::string& name = "")
{
    return {
        {"brand", ""},          {"filament_code", "0x0000"}, {"filament_color", ""}, {"filament_name", name}, {"filament_type", ""},
        {"max_nozzle_temp", 0}, {"min_nozzle_temp", 0},      {"status", status},     {"tray_id", tray_id},
    };
}

nlohmann::json make_canvas_result(const nlohmann::json& tray_list)
{
    return {
        {"error_code", 0},
        {"canvas_info", {{"canvas_list", nlohmann::json::array({{{"canvas_id", 0}, {"connected", 1}, {"tray_list", tray_list}}})}}},
    };
}

} // namespace

TEST_CASE("The CC2 system info gives serial and model", "[ElegooCC2PrinterAgent]")
{
    SystemInfo info;
    CHECK(ElegooCC2PrinterAgent::parse_system_info(200, cc2_system_info_body, info) == Error::None);
    CHECK(info.sn == "TESTSN01");
    CHECK(info.machine_model == "Centauri Carbon 2");
}

TEST_CASE("A 401 HTML answer is reported as unauthorized", "[ElegooCC2PrinterAgent]")
{
    SystemInfo info;
    const std::string html = "<!DOCTYPE html><html><head><title>401 Unauthorized</title></head><body>401 Unauthorized</body></html>";
    CHECK(ElegooCC2PrinterAgent::parse_system_info(401, html, info) == Error::Unauthorized);
}

TEST_CASE("A system info request that got no HTTP status is reported as a failed connection", "[ElegooCC2PrinterAgent]")
{
    SystemInfo info;
    CHECK(ElegooCC2PrinterAgent::parse_system_info(0, "", info) == Error::ConnectFailed);
}

TEST_CASE("A non 200 HTTP status or unparsable system info JSON is rejected", "[ElegooCC2PrinterAgent]")
{
    SystemInfo info;
    CHECK(ElegooCC2PrinterAgent::parse_system_info(500, cc2_system_info_body, info) == Error::HttpError);
    CHECK(ElegooCC2PrinterAgent::parse_system_info(404, "not found", info) == Error::HttpError);
    CHECK(ElegooCC2PrinterAgent::parse_system_info(200, "{not valid json", info) == Error::BadSystemInfo);
}

TEST_CASE("A non zero error code or a serial with topic characters is rejected", "[ElegooCC2PrinterAgent]")
{
    SystemInfo info;
    nlohmann::json bad_code = nlohmann::json::parse(cc2_system_info_body);
    bad_code["error_code"]  = 1;
    CHECK(ElegooCC2PrinterAgent::parse_system_info(200, bad_code.dump(), info) == Error::BadSystemInfo);

    const std::string bad_sn = GENERATE(std::string("a/b"), std::string("a+b"), std::string("a#b"), std::string(""), std::string(65, 'A'));
    CAPTURE(bad_sn);
    nlohmann::json doc       = nlohmann::json::parse(cc2_system_info_body);
    doc["system_info"]["sn"] = bad_sn;
    CHECK(ElegooCC2PrinterAgent::parse_system_info(200, doc.dump(), info) == Error::BadSystemInfo);
}

TEST_CASE("Another machine model is not a CC2", "[ElegooCC2PrinterAgent]")
{
    SystemInfo info;
    nlohmann::json doc                  = nlohmann::json::parse(cc2_system_info_body);
    doc["system_info"]["machine_model"] = "Centauri 2";
    CHECK(ElegooCC2PrinterAgent::parse_system_info(200, doc.dump(), info) == Error::NotCC2);
}

TEST_CASE("The CC2 fixture yields one unit with a black PLA in slot zero and three empty slots", "[ElegooCC2PrinterAgent]")
{
    const nlohmann::json result = nlohmann::json::parse(cc2_canvas_result_fixture);
    std::vector<CanvasUnit> units;
    REQUIRE(ElegooCC2PrinterAgent::parse_canvas_info(result, units) == Error::None);
    REQUIRE(units.size() == 1);

    const CanvasTray& slot0 = units[0][0];
    CHECK(slot0.status == 1);
    CHECK(slot0.type == "PLA");
    CHECK(slot0.name == "RAPID PLA+");
    CHECK(slot0.brand == "ELEGOO");
    CHECK(slot0.color == "#000000");
    CHECK(slot0.max_nozzle_temp == 230);

    for (std::size_t i = 1; i < 4; ++i)
        CHECK(units[0][i].status == 0);
}

TEST_CASE("Slots are placed by tray id, not by list order", "[ElegooCC2PrinterAgent]")
{
    const nlohmann::json result = make_canvas_result(nlohmann::json::array({
        make_tray(3, 0, "slot3"),
        make_tray(1, 0, "slot1"),
        make_tray(0, 1, "slot0"),
        make_tray(2, 0, "slot2"),
    }));
    std::vector<CanvasUnit> units;
    REQUIRE(ElegooCC2PrinterAgent::parse_canvas_info(result, units) == Error::None);
    REQUIRE(units.size() == 1);
    CHECK(units[0][0].name == "slot0");
    CHECK(units[0][0].status == 1);
    CHECK(units[0][1].name == "slot1");
    CHECK(units[0][2].name == "slot2");
    CHECK(units[0][3].name == "slot3");
}

TEST_CASE("A missing, duplicated or out of range tray id rejects the whole answer", "[ElegooCC2PrinterAgent]")
{
    const std::string kind = GENERATE(std::string("missing"), std::string("duplicate"), std::string("out_of_range"),
                                      std::string("negative"), std::string("string_id"));
    CAPTURE(kind);

    nlohmann::json trays = nlohmann::json::array();
    if (kind == "missing") {
        trays = nlohmann::json::array({make_tray(0), make_tray(1), make_tray(3)});
    } else if (kind == "duplicate") {
        trays = nlohmann::json::array({make_tray(0), make_tray(1), make_tray(1), make_tray(3)});
    } else if (kind == "out_of_range") {
        trays = nlohmann::json::array({make_tray(0), make_tray(1), make_tray(2), make_tray(4)});
    } else if (kind == "negative") {
        trays = nlohmann::json::array({make_tray(0), make_tray(1), make_tray(2), make_tray(-1)});
    } else {
        trays = nlohmann::json::array({make_tray("0"), make_tray(1), make_tray(2), make_tray(3)});
    }

    std::vector<CanvasUnit> units;
    CHECK(ElegooCC2PrinterAgent::parse_canvas_info(make_canvas_result(trays), units) == Error::BadResponse);
    CHECK(units.empty());
}

TEST_CASE("An unknown slot status rejects the whole answer", "[ElegooCC2PrinterAgent]")
{
    const nlohmann::json bad_status = GENERATE(nlohmann::json(3), nlohmann::json(-1), nlohmann::json("1"));
    CAPTURE(bad_status);

    const nlohmann::json trays = nlohmann::json::array({
        make_tray(0, bad_status),
        make_tray(1, 0),
        make_tray(2, 0),
        make_tray(3, 0),
    });
    std::vector<CanvasUnit> units;
    CHECK(ElegooCC2PrinterAgent::parse_canvas_info(make_canvas_result(trays), units) == Error::BadResponse);
    CHECK(units.empty());
}

TEST_CASE("A bad second connected unit leaves the output empty", "[ElegooCC2PrinterAgent]")
{
    const nlohmann::json valid_unit = {
        {"canvas_id", 0},
        {"connected", 1},
        {"tray_list", nlohmann::json::array({make_tray(0), make_tray(1), make_tray(2), make_tray(3)})},
    };
    const nlohmann::json bad_unit = {
        {"canvas_id", 1},
        {"connected", 1},
        {"tray_list", nlohmann::json::array({make_tray(0), make_tray(1), make_tray(3)})},
    };
    const nlohmann::json result = {
        {"error_code", 0},
        {"canvas_info", {{"canvas_list", nlohmann::json::array({valid_unit, bad_unit})}}},
    };

    std::vector<CanvasUnit> units;
    CHECK(ElegooCC2PrinterAgent::parse_canvas_info(result, units) == Error::BadResponse);
    CHECK(units.empty());
}

TEST_CASE("A unit whose connected flag is missing or not 0 or 1 rejects the whole answer", "[ElegooCC2PrinterAgent]")
{
    const bool malformed_first = GENERATE(true, false);
    const std::string kind     = GENERATE(as<std::string>{}, "missing", "string", "float", "two", "negative");
    CAPTURE(malformed_first, kind);

    const auto trays          = nlohmann::json::array({make_tray(0), make_tray(1), make_tray(2), make_tray(3)});
    nlohmann::json valid_unit = {{"canvas_id", 0}, {"connected", 1}, {"tray_list", trays}};
    nlohmann::json bad_unit   = {{"canvas_id", 1}, {"tray_list", trays}};
    if (kind == "string")
        bad_unit["connected"] = "1";
    else if (kind == "float")
        bad_unit["connected"] = 1.0;
    else if (kind == "two")
        bad_unit["connected"] = 2;
    else if (kind == "negative")
        bad_unit["connected"] = -1;
    const auto canvas_list      = malformed_first ? nlohmann::json::array({bad_unit, valid_unit}) :
                                                    nlohmann::json::array({valid_unit, bad_unit});
    const nlohmann::json result = {{"error_code", 0}, {"canvas_info", {{"canvas_list", canvas_list}}}};

    std::vector<CanvasUnit> units;
    CHECK(ElegooCC2PrinterAgent::parse_canvas_info(result, units) == Error::BadResponse);
    CHECK(units.empty());
}

TEST_CASE("No connected unit means no CANVAS", "[ElegooCC2PrinterAgent]")
{
    const nlohmann::json empty_list = {
        {"error_code", 0},
        {"canvas_info", {{"canvas_list", nlohmann::json::array()}}},
    };
    std::vector<CanvasUnit> units;
    CHECK(ElegooCC2PrinterAgent::parse_canvas_info(empty_list, units) == Error::NoCanvas);
    CHECK(units.empty());

    const nlohmann::json disconnected = {
        {"error_code", 0},
        {"canvas_info",
         {{"canvas_list",
           nlohmann::json::array({{{"canvas_id", 0},
                                   {"connected", 0},
                                   {"tray_list", nlohmann::json::array({make_tray(0), make_tray(1), make_tray(2), make_tray(3)})}}})}}},
    };
    CHECK(ElegooCC2PrinterAgent::parse_canvas_info(disconnected, units) == Error::NoCanvas);
    CHECK(units.empty());
}

TEST_CASE("More than seven connected units reject the whole answer", "[ElegooCC2PrinterAgent]")
{
    const std::size_t unit_count = GENERATE(std::size_t(7), std::size_t(8));
    nlohmann::json canvas_list   = nlohmann::json::array();
    for (std::size_t i = 0; i < unit_count; ++i)
        canvas_list.push_back({{"canvas_id", i},
                               {"connected", 1},
                               {"tray_list", nlohmann::json::array({make_tray(0), make_tray(1), make_tray(2), make_tray(3)})}});
    const nlohmann::json result = {{"error_code", 0}, {"canvas_info", {{"canvas_list", canvas_list}}}};

    CAPTURE(unit_count);
    std::vector<CanvasUnit> units;
    if (unit_count == 7) {
        CHECK(ElegooCC2PrinterAgent::parse_canvas_info(result, units) == Error::None);
        CHECK(units.size() == 7);
    } else {
        CHECK(ElegooCC2PrinterAgent::parse_canvas_info(result, units) == Error::BadResponse);
        CHECK(units.empty());
    }
}

TEST_CASE("A canvas answer without canvas_info or with error_code is rejected", "[ElegooCC2PrinterAgent]")
{
    const std::string kind = GENERATE(std::string("empty"), std::string("no_canvas_info"), std::string("nonzero_error"),
                                      std::string("missing_error_code"), std::string("non_integer_error_code"));
    CAPTURE(kind);

    nlohmann::json result;
    if (kind == "empty") {
        result = nlohmann::json::object();
    } else if (kind == "no_canvas_info") {
        result = {{"error_code", 0}};
    } else if (kind == "nonzero_error") {
        result               = nlohmann::json::parse(cc2_canvas_result_fixture);
        result["error_code"] = 5;
    } else if (kind == "missing_error_code") {
        result = nlohmann::json::parse(cc2_canvas_result_fixture);
        result.erase("error_code");
    } else {
        result               = nlohmann::json::parse(cc2_canvas_result_fixture);
        result["error_code"] = "0";
    }

    std::vector<CanvasUnit> units;
    CHECK(ElegooCC2PrinterAgent::parse_canvas_info(result, units) == Error::BadResponse);
    CHECK(units.empty());
}

TEST_CASE("Status two counts as a present spool", "[ElegooCC2PrinterAgent]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);

    CanvasUnit unit{};
    unit[0] = {2, "ELEGOO", "PLA", "PLA", "#112233", 220};

    const auto slots = ElegooCC2PrinterAgent::to_ams_slots({unit}, filaments);
    REQUIRE(slots.size() == 1);
    CHECK(slots[0][0].present);
    CHECK(slots[0][0].type == "PLA");
    CHECK(slots[0][0].color_rgba == "112233FF");
    CHECK(slots[0][0].filament_id == "OFvgE0Zh");
    CHECK(slots[0][0].nozzle_temp_max == 220);
}

TEST_CASE("An unrecognised spool stays present without a preset", "[ElegooCC2PrinterAgent]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);

    CanvasUnit unit{};
    unit[0] = {1, "ELEGOO", "RAPID PLA+", "", "#FF0000", 230};
    unit[1] = {1, "ELEGOO", "RAPID PLA+", "PLA", "", 230};
    unit[2] = {1, "ELEGOO", "RAPID PLA+", "   ", "#00FF00", 230};

    const auto slots = ElegooCC2PrinterAgent::to_ams_slots({unit}, filaments);
    REQUIRE(slots.size() == 1);

    CHECK(slots[0][0].present);
    CHECK(slots[0][0].type.empty());
    CHECK(slots[0][0].color_rgba == "FF0000FF");
    CHECK(slots[0][0].filament_id.empty());

    CHECK(slots[0][1].present);
    CHECK(slots[0][1].color_rgba.empty());
    CHECK(slots[0][1].filament_id.empty());

    CHECK(slots[0][2].present);
    CHECK(slots[0][2].type.empty());
    CHECK(slots[0][2].color_rgba == "00FF00FF");
    CHECK(slots[0][2].filament_id.empty());
}

TEST_CASE("Slot types are trimmed and uppercased and non positive nozzle temperatures are omitted", "[ElegooCC2PrinterAgent]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);

    CanvasUnit unit{};
    unit[0] = {1, "ELEGOO", "PLA", "  pla\t", "#44f1ff", -5};

    const auto slots = ElegooCC2PrinterAgent::to_ams_slots({unit}, filaments);
    REQUIRE(slots.size() == 1);
    CHECK(slots[0][0].present);
    CHECK(slots[0][0].type == "PLA");
    CHECK(slots[0][0].color_rgba == "44F1FFFF");
    CHECK(slots[0][0].filament_id == "OFvgE0Zh");
    CHECK(slots[0][0].nozzle_temp_max == 0);
}

TEST_CASE("A slot type is upper cased in its ASCII letters only, whatever the locale", "[ElegooCC2PrinterAgent]")
{
    const ScopedLatinCtype latin;
    FilamentTestCollection filaments;

    CanvasUnit unit{};
    // "pla" and U+54D1 in UTF-8: a single-byte locale would upper case the lead byte 0xE5 and break the sequence.
    unit[0] = {1, "", "", "\tpla \xE5\x93\x91 ", "#000000", 0};

    const auto slots = ElegooCC2PrinterAgent::to_ams_slots({unit}, filaments);
    REQUIRE(slots.size() == 1);
    CHECK(slots[0][0].type == "PLA \xE5\x93\x91");
}

TEST_CASE("The fixture maps slot zero to the Rapid PLA+ preset", "[ElegooCC2PrinterAgent]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);

    const nlohmann::json result = nlohmann::json::parse(cc2_canvas_result_fixture);
    std::vector<CanvasUnit> units;
    REQUIRE(ElegooCC2PrinterAgent::parse_canvas_info(result, units) == Error::None);

    const auto slots = ElegooCC2PrinterAgent::to_ams_slots(units, filaments);
    REQUIRE(slots.size() == 1);
    CHECK(slots[0][0].present);
    CHECK(slots[0][0].type == "PLA");
    CHECK(slots[0][0].filament_id == "OFkhhUS0");
    CHECK(slots[0][0].color_rgba == "000000FF");
    CHECK(slots[0][0].nozzle_temp_max == 230);
    CHECK_FALSE(slots[0][1].present);
    CHECK_FALSE(slots[0][2].present);
    CHECK_FALSE(slots[0][3].present);
}

namespace {

using mqtt_test::FakeMqttBroker;
using Session = FakeMqttBroker::Session;
using Clock   = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr const char* printer_ip = "192.168.100.10";

Slic3r::DynamicPrintConfig make_printer_config(const std::string& host, const std::string& port, const std::string& apikey)
{
    Slic3r::DynamicPrintConfig config;
    config.set_key_value("print_host", new Slic3r::ConfigOptionString(host));
    config.set_key_value("printhost_port", new Slic3r::ConfigOptionString(port));
    config.set_key_value("printhost_apikey", new Slic3r::ConfigOptionString(apikey));
    return config;
}

// What a CC2 broker answers. An empty register_error leaves the registration unanswered.
struct BrokerScript
{
    std::uint8_t connack_code  = 0;
    std::string register_error = "ok";
    std::vector<nlohmann::json> responses;
};

// What the broker saw, written by its thread and read after join().
struct BrokerLog
{
    std::string client_id;
    std::string user;
    std::string password;
    std::uint16_t subscribe_packet_id = 0;
    std::vector<std::pair<std::string, std::uint8_t>> subscriptions; // Filter and requested QoS
    std::vector<std::string> payloads;
    bool disconnected = false;
};

std::uint16_t read_u16(const std::vector<std::uint8_t>& body, std::size_t pos)
{ return static_cast<std::uint16_t>((body.at(pos) << 8) | body.at(pos + 1)); }

// Throws on a string that runs past the body, which fails the broker and so the test.
std::string read_mqtt_string(const std::vector<std::uint8_t>& body, std::size_t& pos)
{
    const std::size_t size = read_u16(body, pos);
    if (size > body.size() - pos - 2)
        throw std::out_of_range("MQTT string runs past the packet");
    std::string out(body.begin() + pos + 2, body.begin() + pos + 2 + size);
    pos += 2 + size;
    return out;
}

bool ends_with(const std::string& s, const std::string& suffix)
{ return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0; }

std::function<void(Session&)> cc2_broker(const BrokerScript& script, BrokerLog& log)
{
    return [script, &log](Session& session) {
        // CONNECT body: protocol name, level, flags, keepalive, then client id, user and password.
        const auto connect = session.read_packet();
        std::size_t pos    = 10;
        log.client_id      = read_mqtt_string(connect.body, pos);
        log.user           = read_mqtt_string(connect.body, pos);
        log.password       = read_mqtt_string(connect.body, pos);
        session.send({0x20, 2, 0, script.connack_code});
        if (script.connack_code != 0)
            return;
        // SUBSCRIBE body: packet id, then each filter with its requested QoS.
        const auto subscribe    = session.read_packet();
        log.subscribe_packet_id = read_u16(subscribe.body, 0);
        for (std::size_t sub_pos = 2; sub_pos < subscribe.body.size();) {
            std::string filter = read_mqtt_string(subscribe.body, sub_pos);
            log.subscriptions.emplace_back(std::move(filter), subscribe.body.at(sub_pos++));
        }
        session.send(
            {0x90, 4, static_cast<std::uint8_t>(log.subscribe_packet_id >> 8), static_cast<std::uint8_t>(log.subscribe_packet_id), 1, 1});

        const std::string prefix = "elegoo/TESTSN01/" + log.client_id + "/";
        for (;;) {
            const auto packet = session.read_packet();
            if (packet.type == 14) {
                log.disconnected = true;
                return;
            }
            const auto publish = Slic3r::Mqtt::parse_publish(packet);
            if (!publish)
                continue;
            log.payloads.push_back(publish->payload);
            if (ends_with(publish->topic, "/api_register") && !script.register_error.empty()) {
                const nlohmann::json answer = {{"client_id", log.client_id}, {"error", script.register_error}};
                session.send(Slic3r::Mqtt::encode_publish(prefix + "register_response", answer.dump()));
            } else if (ends_with(publish->topic, "/api_request")) {
                for (const auto& response : script.responses)
                    session.send(Slic3r::Mqtt::encode_publish(prefix + "api_response", response.dump()));
            }
        }
    };
}

nlohmann::json canvas_response(int id, int method)
{ return {{"id", id}, {"method", method}, {"result", nlohmann::json::parse(cc2_canvas_result_fixture)}}; }

Error query(const FakeMqttBroker& broker, const std::string& client_id, nlohmann::json& result, Clock::duration budget = 3s)
{ return ElegooCC2PrinterAgent::query_canvas("127.0.0.1", broker.port(), "TESTSN01", "tok", client_id, Clock::now() + budget, result); }

struct LocalMachine
{
    Slic3r::AppConfig config;
    Slic3r::DeviceManager manager{nullptr, false, &config};
    Slic3r::MachineObject* obj = nullptr;

    explicit LocalMachine(const std::string& dev_id, const std::string& access_code = "")
    {
        Slic3r::BBLocalMachine machine;
        machine.dev_id = dev_id;
        machine.dev_ip = dev_id;
        obj            = manager.insert_local_device(machine, "lan", "free", "", access_code);
    }
};

} // namespace

TEST_CASE("A CC2 preset with a host yields a target bound to its device id", "[ElegooCC2PrinterAgent]")
{
    const auto config = make_printer_config(printer_ip, "", "abc");
    const auto target = ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2",
                                                              Slic3r::MachineObject::dev_id_from_address(printer_ip, ""));
    REQUIRE(target.has_value());
    CHECK(target->host == printer_ip);
    CHECK(target->token == "abc");
}

TEST_CASE("An empty access code falls back to the ElegooLink default", "[ElegooCC2PrinterAgent]")
{
    const auto config = make_printer_config(printer_ip, "", "");
    const auto target = ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2",
                                                              Slic3r::MachineObject::dev_id_from_address(printer_ip, ""));
    REQUIRE(target.has_value());
    CHECK(target->token == "123456");
}

TEST_CASE("A preset for another model yields no target", "[ElegooCC2PrinterAgent]")
{
    const auto config = make_printer_config(printer_ip, "", "abc");
    CHECK_FALSE(ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-C2", Slic3r::MachineObject::dev_id_from_address(printer_ip, "")));
}

TEST_CASE("A device id that does not belong to the preset yields no target", "[ElegooCC2PrinterAgent]")
{
    const auto config = make_printer_config(printer_ip, "", "abc");
    CHECK_FALSE(
        ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2", Slic3r::MachineObject::dev_id_from_address("192.168.100.11", "")));
}

TEST_CASE("A host with a scheme, a path and a port yields the bare host and that port", "[ElegooCC2PrinterAgent]")
{
    const std::string print_host = "http://192.168.100.10:8080/index.html";
    const auto config            = make_printer_config(print_host, "", "abc");
    const auto target            = ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2",
                                                                         Slic3r::MachineObject::dev_id_from_address(print_host, ""));
    REQUIRE(target.has_value());
    CHECK(target->host == printer_ip);
    CHECK(target->http_port == "8080");
}

TEST_CASE("The preset port overrides a port written into the host", "[ElegooCC2PrinterAgent]")
{
    const std::string print_host = "192.168.100.10:8080";
    const auto config            = make_printer_config(print_host, "3030", "abc");
    const auto target            = ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2",
                                                                         Slic3r::MachineObject::dev_id_from_address(print_host, "3030"));
    REQUIRE(target.has_value());
    CHECK(target->host == printer_ip);
    CHECK(target->http_port == "3030");
}

TEST_CASE("A host that is not a plain name or IPv4 address yields no target", "[ElegooCC2PrinterAgent]")
{
    const std::string print_host = GENERATE(as<std::string>{}, "192.168.100.10?token=abc", "user@192.168.100.10", "[fe80::1]:80",
                                            "192.168.100.10:80a", "192.168.100.10:", "", "http://", "http://192.168.100.10/path?X-Token=x",
                                            "http://192.168.100.10/path#X-Token=x", "http://192.168.100.10/path@x");
    CAPTURE(print_host);
    const auto config = make_printer_config(print_host, "", "abc");
    CHECK_FALSE(ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2", Slic3r::MachineObject::dev_id_from_address(print_host, "")));
}

TEST_CASE("A host with an explicit https scheme yields no target", "[ElegooCC2PrinterAgent]")
{
    // The agent speaks plain HTTP and WebSocket only, and must not send the access code unencrypted to a secure address.
    const std::string print_host = GENERATE(as<std::string>{}, "https://192.168.100.10", "https://192.168.100.10:8443/index.html",
                                            "HTTPS://192.168.100.10");
    CAPTURE(print_host);
    const auto config = make_printer_config(print_host, "", "abc");
    CHECK_FALSE(ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2", Slic3r::MachineObject::dev_id_from_address(print_host, "")));
}

TEST_CASE("A host with a byte outside ASCII yields no target", "[ElegooCC2PrinterAgent]")
{
    // A single-byte locale classifies such bytes as letters, so the check must not depend on the locale.
    const ScopedLatinCtype latin;
    const std::string print_host = "printer\xE9.local";
    const auto config            = make_printer_config(print_host, "", "abc");
    CHECK_FALSE(ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2", Slic3r::MachineObject::dev_id_from_address(print_host, "")));
}

TEST_CASE("A preset port that is not a number yields no target", "[ElegooCC2PrinterAgent]")
{
    const auto config = make_printer_config(printer_ip, "80/x", "abc");
    CHECK_FALSE(ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2", Slic3r::MachineObject::dev_id_from_address(printer_ip, "80/x")));
}

TEST_CASE("A config without the host keys yields no target", "[ElegooCC2PrinterAgent]")
{
    const Slic3r::DynamicPrintConfig config;
    CHECK_FALSE(ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2", ""));
    CHECK_FALSE(ElegooCC2PrinterAgent::resolve_target(config, "Elegoo-CC2", printer_ip));
}

TEST_CASE("A printer preset without a printer model has no model id", "[ElegooCC2PrinterAgent]")
{
    Slic3r::Preset printer(Slic3r::Preset::TYPE_PRINTER, "CC2");
    CHECK_FALSE(ElegooCC2PrinterAgent::printer_model_id(printer, nullptr));
    printer.config.set_key_value("printer_model", new Slic3r::ConfigOptionString("Elegoo Centauri Carbon 2"));
    CHECK(ElegooCC2PrinterAgent::printer_model_id(printer, nullptr).has_value());
}

TEST_CASE("The query registers, reads CANVAS, then disconnects", "[ElegooCC2PrinterAgent]")
{
    BrokerLog log;
    BrokerScript script;
    script.responses = {canvas_response(1, 2005)};
    FakeMqttBroker broker(cc2_broker(script, log));

    nlohmann::json result;
    CHECK(query(broker, "orcaabc123", result) == Error::None);
    broker.join();

    CHECK(result.contains("canvas_info"));
    CHECK(broker.published_topics() == std::vector<std::string>{"elegoo/TESTSN01/api_register", "elegoo/TESTSN01/orcaabc123/api_request"});
    CHECK(log.client_id == "orcaabc123");
    CHECK(log.user == "elegoo");
    CHECK(log.password == "tok");
    CHECK(log.subscribe_packet_id != 0);
    CHECK(log.subscriptions == std::vector<std::pair<std::string, std::uint8_t>>{{"elegoo/TESTSN01/orcaabc123/register_response", 1},
                                                                                 {"elegoo/TESTSN01/orcaabc123/api_response", 1}});
    REQUIRE(log.payloads.size() == 2);
    const auto registration = nlohmann::json::parse(log.payloads[0]);
    CHECK(registration.at("client_id") == "orcaabc123");
    CHECK(registration.at("request_id") == "orcaabc123");
    for (const auto& payload : log.payloads) {
        CAPTURE(payload);
        CHECK(nlohmann::json::parse(payload).value("method", 0) != 1048);
    }
    CHECK(nlohmann::json::parse(log.payloads[1]) == nlohmann::json{{"id", 1}, {"method", 2005}});
    CHECK(log.disconnected);
}

TEST_CASE("Already registered is accepted", "[ElegooCC2PrinterAgent]")
{
    BrokerLog log;
    BrokerScript script;
    script.register_error = "already registered";
    script.responses      = {canvas_response(1, 2005)};
    FakeMqttBroker broker(cc2_broker(script, log));

    nlohmann::json result;
    CHECK(query(broker, "orcaabc123", result) == Error::None);
    broker.join();
    CHECK(result.contains("canvas_info"));
}

TEST_CASE("A too-many-clients answer fails without retrying", "[ElegooCC2PrinterAgent]")
{
    BrokerLog log;
    BrokerScript script;
    script.register_error = "too many clients";
    script.responses      = {canvas_response(1, 2005)};
    FakeMqttBroker broker(cc2_broker(script, log));

    nlohmann::json result;
    CHECK(query(broker, "orcaabc123", result) == Error::TooManyClients);
    broker.join();
    CHECK(broker.published_topics() == std::vector<std::string>{"elegoo/TESTSN01/api_register"});
}

TEST_CASE("A response with another id or method is ignored until the right one arrives", "[ElegooCC2PrinterAgent]")
{
    BrokerLog log;
    BrokerScript script;
    auto expected                = canvas_response(1, 2005);
    expected["result"]["marker"] = "expected";
    script.responses             = {canvas_response(7, 2005), canvas_response(1, 2004), expected};
    FakeMqttBroker broker(cc2_broker(script, log));

    nlohmann::json result;
    CHECK(query(broker, "orcaabc123", result) == Error::None);
    broker.join();
    CHECK(result.value("marker", "") == "expected");
}

TEST_CASE("A wrong password is an MQTT rejection", "[ElegooCC2PrinterAgent]")
{
    BrokerLog log;
    BrokerScript script;
    script.connack_code = 5;
    FakeMqttBroker broker(cc2_broker(script, log));

    // A result left from an earlier query must not survive a failed one.
    nlohmann::json result = {{"stale", true}};
    CHECK(query(broker, "orcaabc123", result) == Error::MqttRejected);
    CHECK(result.is_null());
}

TEST_CASE("A silent printer makes the query time out within the budget", "[ElegooCC2PrinterAgent]")
{
    BrokerLog log;
    BrokerScript script;
    script.register_error.clear();
    FakeMqttBroker broker(cc2_broker(script, log));

    nlohmann::json result;
    const auto start = Clock::now();
    CHECK(query(broker, "orcaabc123", result, 500ms) == Error::Timeout);
    CHECK(Clock::now() - start < 1500ms);
}

TEST_CASE("A snapshot makes the machine ready for filament sync without status data", "[ElegooCC2PrinterAgent]")
{
    LocalMachine machine("cc2-snapshot");
    REQUIRE(machine.obj != nullptr);
    CHECK_FALSE(machine.obj->is_filament_info_ready());

    ElegooCC2PrinterAgent::mark_snapshot_ready(*machine.obj);

    CHECK(machine.obj->is_filament_info_ready());
    CHECK_FALSE(machine.obj->is_info_ready());
    CHECK(machine.obj->printer_type == "Elegoo-CC2");
    CHECK(machine.obj->m_push_count == 0);
    CHECK(machine.obj->module_vers.empty());
}

TEST_CASE("A failed read after a successful one clears the snapshot readiness and keeps the trays", "[ElegooCC2PrinterAgent]")
{
    LocalMachine machine("cc2-failed-read");
    REQUIRE(machine.obj != nullptr);

    auto doc = Slic3r::FilamentSync::build_ams_print_json(
        {{Slic3r::FilamentSync::AmsSlot{true, "PLA", "000000FF", "OFkhhUS0", 230}, {}, {}, {}}});
    // ParseV1_0 resolves these fields through wxGetApp().preset_bundle, even for empty trays.
    for (auto& tray : doc.at("ams").at("ams").at(0).at("tray")) {
        tray.erase("tray_info_idx");
        tray.erase("tray_type");
    }
    const auto system = machine.obj->GetFilaSystem();
    Slic3r::DevFilaSystemParser::ParseV1_0(doc, machine.obj, system.get(), false);
    ElegooCC2PrinterAgent::mark_snapshot_ready(*machine.obj);

    ElegooCC2PrinterAgent::mark_snapshot_failed(*machine.obj);

    CHECK_FALSE(machine.obj->is_filament_info_ready());
    // With no push the empty-list message stays "not connected" rather than "no filament system".
    CHECK(machine.obj->m_push_count == 0);
    const auto* unit = system->GetAmsById("0");
    REQUIRE(unit != nullptr);
    const auto* tray = unit->GetTray("0");
    REQUIRE(tray != nullptr);
    CHECK(tray->is_exists);
}

TEST_CASE("A snapshot satisfies the checks Sidebar::sync_ams_list runs after the fetch", "[ElegooCC2PrinterAgent]")
{
    LocalMachine machine("cc2-sync-checks");
    REQUIRE(machine.obj != nullptr);

    ElegooCC2PrinterAgent::mark_snapshot_ready(*machine.obj);

    CHECK(machine.obj->get_show_printer_type() == "Elegoo-CC2");
    REQUIRE(machine.obj->GetExtderSystem() != nullptr);
    // The agent does not report a nozzle diameter, so any preset diameter matches.
    CHECK(machine.obj->GetExtderSystem()->NozzleDiameterMatchesOrUnknown(0, 0.4f));
    CHECK(Slic3r::DevPrinterConfigUtil::is_printer_model_compatible("Elegoo-CC2", *machine.obj));
    CHECK(machine.obj->is_lan_mode_printer());
}

TEST_CASE("A device inserted the way select_machine does is selectable for the CC2 agent", "[ElegooCC2PrinterAgent]")
{
    const std::string dev_id = Slic3r::MachineObject::dev_id_from_address(printer_ip, "");
    LocalMachine machine(dev_id, "88888888");
    REQUIRE(machine.obj != nullptr);
    // A live agent stamps its id on insertion; the manager here has none.
    const std::string agent_id    = ElegooCC2PrinterAgent::get_agent_info_static().id;
    machine.obj->printer_agent_id = agent_id;

    const auto machines = machine.manager.get_my_machine_list(agent_id);
    CHECK(machines.count(dev_id) == 1);
}

TEST_CASE("Unsupported operations say so instead of succeeding", "[ElegooCC2PrinterAgent]")
{
    ElegooCC2PrinterAgent agent("");
    Slic3r::PrintParams params;
    Slic3r::detectResult detect;
    CHECK(agent.send_message("dev", "{}", 0, 0) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.send_message_to_printer("dev", "{}", 0, 0) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.bind_detect(printer_ip, "secure", detect) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.start_print(params, nullptr, nullptr, nullptr) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.start_local_print_with_record(params, nullptr, nullptr, nullptr) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.start_send_gcode_to_sdcard(params, nullptr, nullptr, nullptr) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.start_local_print(params, nullptr, nullptr) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.start_sdcard_print(params, nullptr, nullptr) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
}

TEST_CASE("Connecting succeeds at once without a network session or a callback", "[ElegooCC2PrinterAgent]")
{
    ElegooCC2PrinterAgent agent("");
    bool called = false;
    CHECK(agent.set_on_local_connect_fn([&](int, std::string, std::string) { called = true; }) == 0);
    CHECK(agent.set_on_printer_connected_fn([&](std::string) { called = true; }) == 0);
    CHECK(agent.set_on_ssdp_msg_fn([&](std::string) { called = true; }) == 0);
    CHECK(agent.set_on_subscribe_failure_fn([&](std::string) { called = true; }) == 0);
    CHECK(agent.set_on_message_fn([&](std::string, std::string) { called = true; }) == 0);
    CHECK(agent.set_on_user_message_fn([&](std::string, std::string) { called = true; }) == 0);
    CHECK(agent.set_on_local_message_fn([&](std::string, std::string) { called = true; }) == 0);
    CHECK(agent.set_queue_on_main_fn([&](std::function<void()>) { called = true; }) == 0);
    CHECK(agent.set_server_callback([&](std::string, int) { called = true; }) == 0);

    Slic3r::PrinterConnectionParams params;
    params.dev_id    = printer_ip;
    params.host      = printer_ip;
    params.password  = "88888888";
    const auto start = Clock::now();
    CHECK(agent.connect_printer(params) == BAMBU_NETWORK_SUCCESS);
    CHECK(Clock::now() - start < 100ms);
    CHECK_FALSE(called);
    CHECK(agent.disconnect_printer() == BAMBU_NETWORK_SUCCESS);
    CHECK(agent.start_discovery(true, false));
    CHECK(agent.set_user_selected_machine(printer_ip) == 0);
    CHECK(agent.get_user_selected_machine() == printer_ip);
}

TEST_CASE("The client id stays the same for the life of the agent", "[ElegooCC2PrinterAgent]")
{
    ElegooCC2PrinterAgent agent("");
    BrokerScript script;
    script.responses = {canvas_response(1, 2005)};

    std::array<BrokerLog, 2> logs;
    for (BrokerLog& log : logs) {
        FakeMqttBroker broker(cc2_broker(script, log));
        nlohmann::json result;
        CHECK(query(broker, agent.client_id(), result) == Error::None);
        broker.join();
    }

    CHECK(agent.client_id().rfind("orca", 0) == 0);
    CHECK(agent.client_id().size() == 10);
    for (const BrokerLog& log : logs) {
        CHECK(log.client_id == agent.client_id());
        REQUIRE_FALSE(log.payloads.empty());
        CHECK(nlohmann::json::parse(log.payloads[0]).at("client_id") == agent.client_id());
    }
}

TEST_CASE("The agent pulls filament info and identifies itself as elegoo_cc2", "[ElegooCC2PrinterAgent]")
{
    ElegooCC2PrinterAgent agent("");
    CHECK(agent.get_filament_sync_mode() == Slic3r::FilamentSyncMode::pull);
    CHECK(agent.get_agent_info().id == "elegoo_cc2");
}

TEST_CASE("The factory creates the CC2 agent by its id", "[ElegooCC2PrinterAgent]")
{
    // create_printer_agent_by_id caches the agent; drop it so later tests start clean.
    struct CacheGuard
    {
        ~CacheGuard() { Slic3r::NetworkAgentFactory::clear_printer_agent_cache(); }
    } cache_guard;
    Slic3r::NetworkAgentFactory::register_all_agents();
    CHECK(Slic3r::NetworkAgentFactory::get_printer_agent_info(Slic3r::ELEGOO_CC2_PRINTER_AGENT_ID) != nullptr);
    const auto agent = Slic3r::NetworkAgentFactory::create_printer_agent_by_id("elegoo_cc2", nullptr, "");
    REQUIRE(agent != nullptr);
    CHECK(agent->get_agent_info().id == "elegoo_cc2");
}

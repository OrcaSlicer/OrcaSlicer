#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <string>
#include <vector>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/Utils/KobraX.hpp"
#include "slic3r/Utils/KobraXPrinterAgent.hpp"
#include "slic3r/Utils/KobraXSession.hpp"

using namespace Slic3r;
using json = nlohmann::json;

namespace {

// Reports captured from a Kobra X on firmware 2.0.2.8 (Kobra Control's tests/kobra_captures.py), anonymised
const json INFO_REPORT = json::parse(R"({"action": "report", "code": 200, "msg": "done", "state": "done", "type": "info",
  "data": {"fan_speed_pct": 0,
           "features": {"auto_leveling_support": true, "camera_timelapse_support": true, "flow_calibration_support": true},
           "model": "Anycubic Kobra X", "print_speed_mode": 2, "printerName": "Anycubic Kobra X", "project": null,
           "state": "free",
           "temp": {"curr_hotbed_temp": 47, "curr_nozzle_temp": 37, "target_hotbed_temp": 0, "target_nozzle_temp": 0},
           "urls": {"fileUploadurl": "http://192.0.2.10:18910/gcode_upload?s=***",
                    "rtspUrl": "http://192.0.2.10:18088/live/StReAmTk"},
           "version": "2.0.2.8"}})");

json slot(int index, std::vector<int> color, int status = 5)
{
    return {{"brand_name", "public"}, {"color", color}, {"consumables_percent", 100}, {"index", index},
            {"status", status},       {"type", "PLA"},  {"weight", 1000}};
}

json ace_report(json slots)
{
    return {{"action", "getInfo"},
            {"code", 200},
            {"state", "success"},
            {"type", "multiColorBox"},
            {"data",
             {{"head_tools_model", 0},
              {"multi_color_box",
               json::array({{{"auto_feed", 1}, {"humidity", 32}, {"id", 0}, {"loaded_slot", -1}, {"slots", slots}, {"status", 1}}})}}}};
}

// The print head's feeder (box -1, the spool holder) next to an ACE, the topology KX-Bridge captured on a Kobra X
// (tests/test_ace_rfid_vendor_matching.py, head_tools_model 1). The head's fourth inlet is the ACE's.
json hub_report(json holder, json ace)
{
    return {{"action", "getInfo"},
            {"code", 200},
            {"state", "success"},
            {"type", "multiColorBox"},
            {"data",
             {{"head_tools_model", 1},
              {"multi_color_box",
               json::array({{{"id", -1}, {"loaded_slot", -1}, {"slots", holder}},
                            {{"auto_feed", 1}, {"humidity", 32}, {"id", 0}, {"loaded_slot", -1}, {"slots", ace}, {"status", 1}}})}}}};
}

json seven_sources()
{
    return hub_report(json::array({slot(0, {255, 255, 255}), slot(1, {0, 0, 0}, 0), slot(2, {20, 40, 60}), slot(3, {0, 0, 0}, 0)}),
                      json::array({slot(0, {225, 6, 0}), slot(1, {124, 77, 58}), slot(2, {212, 185, 150}), slot(3, {101, 137, 70})}));
}

json print_report(const std::string& action, const std::string& state, json data = json::object(), int code = 200)
{
    return {{"action", action}, {"code", code}, {"state", state}, {"type", "print"}, {"data", data}};
}

const char* const TEST_CERT = R"(-----BEGIN CERTIFICATE-----
MIIBZzCCAQ6gAwIBAgIUdAzdgoL0Rb3EUKixkyI7DW1GsZIwCgYIKoZIzj0EAwIw
GTEXMBUGA1UEAwwOQW55Y3ViaWNTbGljZXIwIBcNMjYxMTE3MjIyNzI1WhgPMjEy
NjEwMjQyMjI3MjVaMBkxFzAVBgNVBAMMDkFueWN1YmljU2xpY2VyMFkwEwYHKoZI
zj0CAQYIKoZIzj0DAQcDQgAEZMoB6VOBnrge6W3lClVWFgB4HSMAQNtQ3yH9TFiX
DthdNVQHveUr4d4saG02hcDvxnb4VpmYQS2cMvQCI/GzUaMyMDAwHQYDVR0OBBYE
FGF8uAKoVUNTK2qwbhDu0BiKymziMA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0E
AwIDRwAwRAIgeTngqBPXXu1LJPjWv9lr3bkM/vPUU1e0BH9F00lPxI4CIHGthQVI
uffzpM1HPbZ9PG5tRTeLkDkHVqT4ovJFeTau
-----END CERTIFICATE-----)";
// The certificate's key, and a key that does not belong to it. Throwaway test material, CN=AnycubicSlicer.
const char* const TEST_KEY = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQguftYtN1iybOlCGgz
99nhK//jHzEgejpEWX8a5cJU99KhRANCAARkygHpU4GeuB7pbeUKVVYWAHgdIwBA
21DfIf1MWJcO2F01VAe95Svh3ixobTaFwO/GdvhWmZhBLZwy9AIj8bNR
-----END PRIVATE KEY-----)";
const char* const OTHER_KEY = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgOYiZRY4zxOl9wdW9
vbjxngBA9hrszqxuS1BUeRMpc32hRANCAAQrwaLzHViENM68yhC04I1VLG2PL5BI
r71krNM63CXcJ174OrLMhT6jKos7anOLPXdk8x+SZy9AnRIWldgL5szy
-----END PRIVATE KEY-----)";

} // namespace

TEST_CASE("The credential request is signed as the printer checks it", "[KobraX]")
{
    CHECK(KobraX::ctrl_sign("0123456789abcdefFEDCBA9876543210", "1700000000000", "abcdef") == "2c64e080cbdca2b106087b704c8ca793");
}

TEST_CASE("The printer's credential answer is decrypted, and its own address ignored", "[KobraX]")
{
    // AES-128-CBC with token[16:32], made with Kobra Control's aes.py
    const std::string token  = "0123456789abcdefFEDCBA9876543210";
    const std::string answer = R"({"code": 200, "data": {"info": "J0zaQfUFw0O6Vu224FYbturYJLwePzq/h3WCgYZ6W+gKEhJIetPeaNdH+6sO+Qjy79T2La9kX1GCc3Tpp38fkR4Y4G9xS6RdLp96yJpVPOq3UZm5l0xoa0SEK6H92vnlQl/w7q9L6Ys2uNNy/7DgAtAM68EekNIiAAEdWV5vYxJF9FYG5pXDcFios+nknkDe", "token": "ivivivivivivivIV"}})";
    KobraX::Credentials creds;
    std::string         error;
    REQUIRE(KobraX::parse_ctrl_answer(token, answer, "192.0.2.10", creds, error));
    CHECK(creds.host == "192.0.2.10");
    CHECK(creds.username == "user1");
    CHECK(creds.password == "pass1");
    CHECK(creds.device_id == "0123456789abcdef0123456789abcdef");
    CHECK(creds.mode_id == "20030");

    SECTION("with the wrong token it fails")
    {
        CHECK_FALSE(KobraX::parse_ctrl_answer("0123456789abcdef0000000000000000", answer, "192.0.2.10", creds, error));
        CHECK_FALSE(error.empty());
    }
    SECTION("an answer without data fails")
    {
        CHECK_FALSE(KobraX::parse_ctrl_answer(token, R"({"code": 403})", "192.0.2.10", creds, error));
    }
}

TEST_CASE("MQTT packets are framed as MQTT 3.1.1", "[KobraX]")
{
    const std::string connect = KobraX::Mqtt::connect("orca", "u", "p", 60);
    CHECK(connect == std::string("\x10\x16\x00\x04MQTT\x04\xC2\x00\x3C\x00\x04orca\x00\x01u\x00\x01p", 24));
    CHECK(KobraX::Mqtt::pingreq() == std::string("\xC0\x00", 2));

    // A payload of 200 bytes needs two length bytes
    const std::string topic   = "a/b";
    const std::string payload(200, 'x');
    std::string       buffer = KobraX::Mqtt::publish(topic, payload) + std::string("\xD0\x00", 2);
    CHECK(uint8_t(buffer[1]) == (205 % 128 | 0x80));

    SECTION("split reads are reassembled")
    {
        std::string                       stream;
        std::vector<KobraX::Mqtt::Packet> packets;
        for (char c : buffer) {
            stream += c;
            REQUIRE(KobraX::Mqtt::split(stream, packets));
        }
        REQUIRE(packets.size() == 2);
        CHECK(stream.empty());
        std::string got_topic, got_payload;
        REQUIRE(KobraX::Mqtt::parse_publish(packets[0], got_topic, got_payload));
        CHECK(got_topic == topic);
        CHECK(got_payload == payload);
        CHECK(packets[1].type == KobraX::Mqtt::PINGRESP);
    }
    SECTION("a QoS 1 publish carries a packet id before its payload")
    {
        const KobraX::Mqtt::Packet packet{KobraX::Mqtt::PUBLISH, 2, std::string("\x00\x03" "a/b" "\x00\x07" "{}", 9)};
        std::string got_topic, got_payload;
        REQUIRE(KobraX::Mqtt::parse_publish(packet, got_topic, got_payload));
        CHECK(got_payload == "{}");
    }
    SECTION("a remaining length of five bytes is refused")
    {
        std::string                       bad("\x30\xFF\xFF\xFF\xFF\x01", 6);
        std::vector<KobraX::Mqtt::Packet> packets;
        CHECK_FALSE(KobraX::Mqtt::split(bad, packets));
    }
}

TEST_CASE("Topics follow the printer's mode and device ids", "[KobraX]")
{
    const KobraX::Credentials creds{"192.0.2.10", "u", "p", "dev", "20030"};
    CHECK(KobraX::publish_topic(creds, "print") == "anycubic/anycubicCloud/v1/slicer/printer/20030/dev/print");
    CHECK(KobraX::subscribe_topic(creds) == "anycubic/anycubicCloud/v1/printer/public/20030/dev/#");
    CHECK(KobraX::report_suffix("anycubic/anycubicCloud/v1/printer/public/20030/dev/info/report") == "info/report");
}

TEST_CASE("An idle printer's info report becomes an idle Device tab status", "[KobraX]")
{
    KobraX::State state;
    state.apply("info/report", INFO_REPORT);
    const json p = state.push_status(true)["print"];
    CHECK(p["gcode_state"] == "IDLE");
    CHECK(p["nozzle_temper"] == 37);
    CHECK(p["bed_temper"] == 47);
    CHECK(p["sdcard"] == true);
    CHECK(p["home_flag"] == (1 << 8 | 1 << 16));
    CHECK(p["support_timelapse"] == true);
    CHECK(p["ipcam"]["ipcam_dev"] == "1");
    CHECK(p["spd_lvl"] == 2);
    CHECK(state.camera_url == "http://192.0.2.10:18088/live/StReAmTk");
    CHECK(state.upload_url == "http://192.0.2.10:18910/gcode_upload?s=***");
    CHECK(state.firmware == "2.0.2.8");

    SECTION("a disconnected printer has no storage, so nothing is sent to it")
    {
        const json offline = state.push_status(false)["print"];
        CHECK(offline["sdcard"] == false);
        CHECK((offline["home_flag"].get<int>() & 0x300) == 0);
    }
}

TEST_CASE("ACE slots become AMS trays", "[KobraX]")
{
    KobraX::State state;
    state.apply("multiColorBox/report",
                ace_report(json::array({slot(0, {225, 6, 0}), slot(1, {124, 77, 58}), slot(2, {0, 0, 0}, 0), slot(3, {101, 137, 70})})));
    const json ams = state.push_status(true, [](const std::string& type) { return "GF" + type; })["print"]["ams"];
    CHECK(ams["ams_exist_bits"] == "1");
    CHECK(ams["tray_exist_bits"] == "B");
    REQUIRE(ams["ams"].size() == 1);
    const json& trays = ams["ams"][0]["tray"];
    REQUIRE(trays.size() == 4);
    CHECK(trays[0]["tray_color"] == "E10600FF");
    CHECK(trays[0]["tray_info_idx"] == "GFPLA");
    CHECK(trays[0]["remain"] == 100);
    CHECK(trays[2]["tray_slot_placeholder"] == "1");

    SECTION("a refused slot edit keeps the slots")
    {
        state.apply("multiColorBox/report", {{"action", "setInfo"}, {"state", "failed"}, {"data", json::array()}});
        CHECK(state.slots().size() == 4);
    }
}

TEST_CASE("The spool holder and the ACE are seven sources in two units", "[KobraX]")
{
    KobraX::State state;
    state.apply("multiColorBox/report", seven_sources());

    const std::vector<KobraX::Slot> slots = state.slots();
    REQUIRE(slots.size() == 7);
    // ACE 1 first as unit A (trays 0..3, print slots 4..7), the holder's 3 positions after it as unit B
    for (int i = 0; i < 4; ++i) {
        CHECK(slots[i].tray == i);
        CHECK(slots[i].ams_index == 4 + i);
        CHECK_FALSE(slots[i].holder);
    }
    for (int i = 0; i < 3; ++i) {
        CHECK(slots[4 + i].tray == 4 + i);
        CHECK(slots[4 + i].ams_index == i);
        CHECK(slots[4 + i].holder);
    }
    CHECK(slots[6].r == 20);
    CHECK(slots[6].b == 60);

    const json ams = state.push_status(true, [](const std::string& type) { return "GF" + type; })["print"]["ams"];
    CHECK(ams["ams_exist_bits"] == "3");
    CHECK(ams["tray_exist_bits"] == "5F"); // ACE 0..3, holder positions 1 and 3 (trays 4, 6)
    REQUIRE(ams["ams"].size() == 2);
    const json& ace = ams["ams"][0];
    const json& holder = ams["ams"][1];
    CHECK(ace["id"] == "0");
    CHECK(ace["display_name"] == "ACE 1");
    CHECK(holder["id"] == "1");
    CHECK(holder["display_name"] == "Spool holder");
    CHECK(holder["info"] == "0002");
    // The Device tab draws only 4-tray units: the holder's missing fourth position is an empty placeholder
    REQUIRE(ace["tray"].size() == 4);
    REQUIRE(holder["tray"].size() == 4);
    CHECK(ace["tray"][0]["tray_color"] == "E10600FF");
    CHECK(holder["tray"][0]["tray_color"] == "FFFFFFFF");
    CHECK(holder["tray"][0]["tray_info_idx"] == "GFPLA");
    CHECK(holder["tray"][1]["tray_slot_placeholder"] == "1");
    CHECK(holder["tray"][2]["tray_color"] == "14283CFF");
    CHECK(holder["tray"][3]["id"] == "3");
    CHECK(holder["tray"][3]["tray_slot_placeholder"] == "1");

    SECTION("the holder alone, without an ACE, is one unit of its 4 slots")
    {
        KobraX::State head_only;
        head_only.apply("multiColorBox/report",
                        {{"state", "success"},
                         {"data", {{"multi_color_box", json::array({{{"id", -1}, {"slots", json::array({slot(0, {1, 2, 3}), slot(1, {1, 2, 3}), slot(2, {1, 2, 3}), slot(3, {1, 2, 3})})}}})}}}});
        const json units = head_only.push_status(true)["print"]["ams"]["ams"];
        REQUIRE(units.size() == 1);
        CHECK(units[0]["display_name"] == "Spool holder");
        CHECK(head_only.slots()[3].ams_index == 3);
    }
}

TEST_CASE("Chained ACE units and the spool holder: N x 4 + 3 sources", "[KobraX]")
{
    json report = seven_sources();
    report["data"]["multi_color_box"].push_back(
        {{"id", 1}, {"loaded_slot", -1}, {"slots", json::array({slot(0, {1, 2, 3}), slot(1, {4, 5, 6}), slot(2, {7, 8, 9}), slot(3, {0, 0, 0}, 0)})}});
    KobraX::State state;
    state.apply("multiColorBox/report", report);
    const std::vector<KobraX::Slot> slots = state.slots();
    REQUIRE(slots.size() == 11);
    CHECK(slots[4].tray == 4);
    CHECK(slots[4].ams_index == 8); // ACE 2 slot 1: 4 + 1 * 4 + 0
    CHECK(slots[8].holder);
    CHECK(slots[8].tray == 8);     // the holder is the unit after the last ACE
    CHECK(slots[8].ams_index == 0);
    const json units = state.push_status(true)["print"]["ams"]["ams"];
    REQUIRE(units.size() == 3);
    CHECK(units[1]["display_name"] == "ACE 2");
    CHECK(units[2]["display_name"] == "Spool holder");
}

TEST_CASE("Holder and ACE trays map to their own print slots", "[KobraX]")
{
    KobraX::State state;
    state.apply("multiColorBox/report", seven_sources());
    json        mapping;
    std::string error;

    // Filament 1 on holder position 1 (tray 4), 2 on ACE slot 2 (tray 1), 3 on holder position 3 (tray 6)
    REQUIRE(KobraX::box_mapping("[4, 1, 6]", state.slots(), mapping, error));
    REQUIRE(mapping.size() == 3);
    CHECK(mapping[0]["ams_index"] == 0);
    CHECK(mapping[0]["ams_color"] == json::array({255, 255, 255, 255}));
    CHECK(mapping[1]["ams_index"] == 5);
    CHECK(mapping[2]["paint_index"] == 2);
    CHECK(mapping[2]["ams_index"] == 2);

    SECTION("an empty holder position and the placeholder are refused")
    {
        CHECK_FALSE(KobraX::box_mapping("[5]", state.slots(), mapping, error));
        CHECK(error.find("slot 5 is empty") != std::string::npos);
        error.clear();
        CHECK_FALSE(KobraX::box_mapping("[7]", state.slots(), mapping, error));
        CHECK(error.find("no slot 7") != std::string::npos);
    }
    SECTION("without a choice all seven sources are used in tray order, never an empty one")
    {
        REQUIRE(KobraX::box_mapping("", state.slots(), mapping, error));
        REQUIRE(mapping.size() == 7);
        const std::vector<int> expected = {4, 5, 6, 7, 0, 2, 2}; // holder 2 is empty: the last loaded source stands in
        for (size_t i = 0; i < expected.size(); ++i)
            CHECK(mapping[i]["ams_index"] == expected[i]);
    }
}

TEST_CASE("A print's phases, progress and end are followed", "[KobraX]")
{
    KobraX::State state;
    state.apply("info/report", INFO_REPORT);

    state.apply("print/report", print_report("start", "checking", {{"filename", "/cube.gcode"}, {"taskid", 77}}));
    CHECK(state.view_state() == "printing");
    CHECK(state.push_status(true)["print"]["gcode_state"] == "PREPARE");
    CHECK(state.filename == "cube.gcode");
    CHECK(state.task_id == "77");

    state.apply("print/report", print_report("start", "hotbed_heating", {{"progress", 0}}));
    CHECK(state.push_status(true)["print"]["stg_cur"] == 2);

    state.apply("print/report", print_report("start", "printing", {{"progress", 40}, {"curr_layer", 20}, {"total_layers", 50}, {"remain_time", 3}}));
    json p = state.push_status(true)["print"];
    CHECK(p["gcode_state"] == "RUNNING");
    CHECK(p["mc_percent"] == 40);
    CHECK(p["layer_num"] == 20);
    CHECK(p["mc_remaining_time"] == 3);

    // After the last layer the printer reports progress 0: progress never goes back within a job
    state.apply("print/report", print_report("start", "printing", {{"progress", 0}}));
    CHECK(state.push_status(true)["print"]["mc_percent"] == 40);

    SECTION("a finished print keeps its layers")
    {
        state.apply("print/report", print_report("finish", "finished", {{"progress", 100}}));
        p = state.push_status(true)["print"];
        CHECK(p["gcode_state"] == "FINISH");
        CHECK(p["layer_num"] == 20);
        CHECK(p["subtask_name"] == "");
    }
    SECTION("a print the printer ends itself is an error, a cancel is not")
    {
        state.apply("print/report", print_report("stop", "stoped", json::object(), 10111));
        CHECK(state.view_state() == "error");
        state.apply("print/report", print_report("start", "checking"));
        state.apply("print/report", print_report("stop", "stoped", json::object(), 200));
        CHECK(state.view_state() == "cancelled");
    }
    SECTION("another client's answer does not change the job")
    {
        state.apply("print/report", print_report("getSliceParam", "done", {{"progress", 99}}));
        CHECK(state.push_status(true)["print"]["mc_percent"] == 40);
    }
}

TEST_CASE("The chosen trays become the print's slot mapping", "[KobraX]")
{
    KobraX::State state;
    state.apply("multiColorBox/report",
                ace_report(json::array({slot(0, {225, 6, 0}), slot(1, {124, 77, 58}), slot(2, {0, 0, 0}, 0), slot(3, {101, 137, 70})})));
    json        mapping;
    std::string error;

    REQUIRE(KobraX::box_mapping("[3, -1, 0]", state.slots(), mapping, error));
    REQUIRE(mapping.size() == 2);
    CHECK(mapping[0]["paint_index"] == 0);
    CHECK(mapping[0]["ams_index"] == 3);
    CHECK(mapping[1]["paint_index"] == 2);
    CHECK(mapping[1]["ams_index"] == 0);
    CHECK(mapping[1]["material_type"] == "PLA");

    SECTION("an empty slot is refused")
    {
        CHECK_FALSE(KobraX::box_mapping("[2]", state.slots(), mapping, error));
        CHECK(error.find("slot 2 is empty") != std::string::npos);
    }
    SECTION("without a choice the loaded slots are used in order, never an empty one")
    {
        REQUIRE(KobraX::box_mapping("", state.slots(), mapping, error));
        REQUIRE(mapping.size() == 4);
        CHECK(mapping[2]["ams_index"] == 3);
        CHECK(mapping[3]["ams_index"] == 3);
    }
}

TEST_CASE("print/start names the folder and the file separately", "[KobraX]")
{
    const json data = KobraX::print_start_data("cube.gcode", 1234, json::array(), true, false, true);
    CHECK(data["filepath"] == "/");
    CHECK(data["filename"] == "cube.gcode");
    CHECK(data["ams_settings"]["use_ams"] == false);
    CHECK(data["task_settings"]["auto_leveling"] == 1);
    CHECK(data["task_settings"]["timelapse"]["status"] == 1);
}

TEST_CASE("Upload names are safe for the printer", "[KobraX]")
{
    CHECK(KobraXPrinterAgent::upload_name("Cube #1/2", "x.3mf") == "Cube _1_2.gcode");
    CHECK(KobraXPrinterAgent::upload_name("", "/tmp/plate.gcode.3mf") == "plate.gcode");
    CHECK(KobraXPrinterAgent::upload_name("...", "") == "print.gcode");
}

TEST_CASE("Only a later phase proves a print start", "[KobraX]")
{
    CHECK(KobraXPrinterAgent::start_outcome("print/report", print_report("start", "checking")) == -1);
    CHECK(KobraXPrinterAgent::start_outcome("print/report", print_report("start", "printing")) == -1);
    CHECK(KobraXPrinterAgent::start_outcome("print/report", print_report("start", "hotbed_heating")) == 1);
    CHECK(KobraXPrinterAgent::start_outcome("print/report", print_report("stop", "stoped", json::object(), 10111)) == 0);
    CHECK(KobraXPrinterAgent::start_outcome("info/report", print_report("start", "hotbed_heating")) == -1);
}

TEST_CASE("The client identity is found among other PEM blocks", "[KobraX]")
{
    namespace fs = boost::filesystem;
    const fs::path path = fs::temp_directory_path() / fs::unique_path("kobrax-%%%%-%%%%.bin");
    {
        // As inside the slicer's library: binary noise, a key that is not the certificate's, then the pair
        fs::ofstream file(path, std::ios::binary);
        file << std::string("\x7F" "ELF\0\0", 6) << OTHER_KEY << "\n\0\0" << TEST_CERT << "\n" << TEST_KEY << "\n";
    }
    KobraX::ClientIdentity identity;
    CHECK(KobraX::read_client_identity(path.string(), identity));
    CHECK(identity.cert_pem == TEST_CERT);
    CHECK(identity.key_pem == TEST_KEY);
    fs::remove(path);

    {
        fs::ofstream file(path, std::ios::binary);
        file << TEST_CERT << "\n" << OTHER_KEY << "\n";
    }
    CHECK_FALSE(KobraX::read_client_identity(path.string(), identity));
    fs::remove(path);
}

TEST_CASE("A user copy of the client identity wins over the bundled one", "[KobraX]")
{
    namespace fs = boost::filesystem;
    const fs::path bundled = fs::path(PROFILES_DIR).parent_path() / "cert" / "anycubic";
    const fs::path user    = fs::temp_directory_path() / fs::unique_path("kobrax-user-%%%%-%%%%");
    fs::create_directories(user);
    {
        fs::ofstream(user / "anycubic_slicer.pem", std::ios::binary) << TEST_CERT << "\n";
        fs::ofstream(user / "anycubic_slicer_key.pem", std::ios::binary) << TEST_KEY << "\n";
    }
    KobraX::ClientIdentity identity;
    std::string            error;
    CHECK(KobraX::find_client_identity(user.string(), bundled.string(), identity, error));
    CHECK(identity.cert_pem == TEST_CERT);
    CHECK(identity.key_pem == TEST_KEY);
    fs::remove_all(user);
}

TEST_CASE("The bundled client identity is a matching CN=AnycubicSlicer pair", "[KobraX]")
{
    namespace fs = boost::filesystem;
    const fs::path bundled = fs::path(PROFILES_DIR).parent_path() / "cert" / "anycubic";
    const fs::path both    = fs::temp_directory_path() / fs::unique_path("kobrax-bundled-%%%%-%%%%.pem");
    {
        fs::ofstream out(both, std::ios::binary);
        out << fs::ifstream(bundled / "anycubic_slicer.pem", std::ios::binary).rdbuf() << "\n"
            << fs::ifstream(bundled / "anycubic_slicer_key.pem", std::ios::binary).rdbuf() << "\n";
    }
    // read_client_identity checks the CN and that the key is the certificate's
    KobraX::ClientIdentity identity;
    CHECK(KobraX::read_client_identity(both.string(), identity));
    fs::remove(both);

    // With no user copy the search still ends with an identity: an installed Anycubic Slicer Next's or the bundled one
    std::string error;
    CHECK(KobraX::find_client_identity("", bundled.string(), identity, error));
    CHECK(error.empty());
}

namespace {
KobraX::Control control(const json& print, const std::string& state, std::vector<KobraX::Message>& out)
{
    out.clear();
    return KobraX::device_control(print, state, "77", out);
}
json gcode(const std::string& line) { return {{"command", "gcode_line"}, {"param", line}, {"sequence_id", "5"}}; }
} // namespace

TEST_CASE("Device tab controls become Kobra X messages", "[KobraX]")
{
    using KobraX::Control;
    std::vector<KobraX::Message> out;

    SECTION("temperatures: tempature/set on the web topic when idle, print/update during a print")
    {
        REQUIRE(control(gcode("M104 S215\n"), "idle", out) == Control::Sent);
        REQUIRE(out.size() == 1);
        CHECK(out[0].kind == "web");
        CHECK(out[0].type == "tempature");
        CHECK(out[0].action == "set");
        CHECK(out[0].data == json({{"type", 0}, {"target_nozzle_temp", 215}, {"target_hotbed_temp", 0}}));
        REQUIRE(control(gcode("M140 S60"), "paused", out) == Control::Sent);
        CHECK(out[0].data["type"] == 1);
        CHECK(out[0].data["target_hotbed_temp"] == 60);
        REQUIRE(control({{"command", "set_bed_temp"}, {"temp", 500}}, "idle", out) == Control::Sent);
        CHECK(out[0].data["target_hotbed_temp"] == 120); // clamped
        REQUIRE(control({{"command", "set_nozzle_temp"}, {"target_temp", 220}, {"extruder_index", 0}}, "printing", out) == Control::Sent);
        CHECK(out[0].kind == "web");
        CHECK(out[0].type == "print");
        CHECK(out[0].action == "update");
        CHECK(out[0].data == json({{"taskid", "77"}, {"settings", {{"target_nozzle_temp", 220}}}}));
        CHECK(control({{"command", "set_nozzle_temp"}, {"target_temp", 220}, {"extruder_index", 1}}, "idle", out) == Control::Unsupported);
        CHECK(control(gcode("M104 S215"), "busy", out) == Control::Refused);
    }
    SECTION("part fan: fan/setSpeed, plus print/update during a print")
    {
        REQUIRE(control(gcode("M106 P1 S255"), "idle", out) == Control::Sent);
        REQUIRE(out.size() == 1);
        CHECK(out[0].kind == "slicer");
        CHECK(out[0].type == "fan");
        CHECK(out[0].action == "setSpeed");
        CHECK(out[0].data["fan_speed_pct"] == 100);
        REQUIRE(control({{"command", "set_fan"}, {"fan_index", 1}, {"speed", 40}}, "printing", out) == Control::Sent);
        REQUIRE(out.size() == 2);
        CHECK(out[1].data == json({{"taskid", "77"}, {"settings", {{"fan_speed_pct", 40}}}}));
        CHECK(control({{"command", "set_fan"}, {"fan_index", 2}, {"speed", 40}}, "idle", out) == Control::Unsupported);
        CHECK(control(gcode("M106 P2 S255"), "idle", out) == Control::Unsupported);
    }
    SECTION("homing and jogs: axis/move, only while idle")
    {
        REQUIRE(control(gcode("G28"), "idle", out) == Control::Sent);
        CHECK(out[0].type == "axis");
        CHECK(out[0].data == json({{"axis", 5}, {"move_type", 2}, {"distance", 0.0}}));
        REQUIRE(control(gcode("G28 Z"), "finished", out) == Control::Sent);
        CHECK(out[0].data["axis"] == 3);
        CHECK(control(gcode("G28 X"), "idle", out) == Control::Unsupported);
        REQUIRE(control(gcode("G91 \nG1 Y-10.0 F3000\nG90 \n"), "idle", out) == Control::Sent);
        CHECK(out[0].data == json({{"axis", 2}, {"move_type", 0}, {"distance", 10.0}}));
        REQUIRE(control(gcode("G91\nG1 Z500\nG90"), "idle", out) == Control::Sent);
        CHECK(out[0].data == json({{"axis", 3}, {"move_type", 1}, {"distance", 50.0}})); // one jog is at most 50 mm
        CHECK(control(gcode("G28"), "printing", out) == Control::Refused);
        CHECK(out.empty());
        CHECK(control(gcode("G90"), "printing", out) == Control::NoOp);
        CHECK(control(gcode("G1 X10"), "idle", out) == Control::Unsupported);
    }
    SECTION("speed: print_speed 1-3 is the Kobra's print_speed_mode, during a print only")
    {
        REQUIRE(control({{"command", "print_speed"}, {"param", "3"}}, "printing", out) == Control::Sent);
        CHECK(out[0].kind == "web");
        CHECK(out[0].data == json({{"taskid", "77"}, {"settings", {{"print_speed_mode", 3}}}}));
        CHECK(control({{"command", "print_speed"}, {"param", "4"}}, "printing", out) == Control::Unsupported);
        CHECK(control({{"command", "print_speed"}, {"param", "2"}}, "idle", out) == Control::Refused);
    }
    SECTION("anything else stays unsupported")
    {
        CHECK(control({{"command", "ams_change_filament"}}, "idle", out) == Control::Unsupported);
        CHECK(control({{"command", "gcode_line"}}, "idle", out) == Control::Unsupported);
    }
}

TEST_CASE("The Kobra X has no chamber temperature to show", "[KobraX]")
{
    KobraX::State state;
    state.apply("info/report", INFO_REPORT);
    CHECK(state.push_status(true)["print"]["support_chamber_temp_display"] == false);
}

TEST_CASE("Requests go to the slicer or the web topic", "[KobraX]")
{
    KobraX::Credentials creds;
    creds.mode_id   = "20030";
    creds.device_id = "dev";
    CHECK(KobraX::publish_topic(creds, "fan") == "anycubic/anycubicCloud/v1/slicer/printer/20030/dev/fan");
    CHECK(KobraX::publish_topic(creds, "tempature", "web") == "anycubic/anycubicCloud/v1/web/printer/20030/dev/tempature");
}

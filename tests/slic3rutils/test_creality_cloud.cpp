#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include "slic3r/Utils/CrealityCloud.hpp"

using namespace Slic3r;
namespace CC = Slic3r::CrealityCloud;
using nlohmann::json;

TEST_CASE("Creality Cloud login reply fills the session", "[CrealityCloud]")
{
    CC::Session session;
    std::string error;
    const std::string body =
        R"({"code":0,"msg":"ok","result":{"token":"tok","userId":12345,"user_info":{"nickName":"Maker","avatar":"a.png"}}})";
    REQUIRE(CC::parse_login(body, session, error) == CC::CODE_OK);
    CHECK(session.token == "tok");
    CHECK(session.user_id == "12345");
    CHECK(session.nickname == "Maker");
    CHECK(session.valid());

    SECTION("an unfinished registration is not a session")
    {
        CC::Session pending;
        CHECK(CC::parse_login(R"({"code":0,"result":{"token":"t","userId":"0"}})", pending, error) == CC::CODE_PARSE);
    }
    SECTION("a server error keeps its code and message")
    {
        CC::Session failed;
        CHECK(CC::parse_login(R"({"code":3,"msg":"wrong password"})", failed, error) == 3);
        CHECK(error == "wrong password");
    }
}

TEST_CASE("Creality Cloud device list keeps cloud printers only", "[CrealityCloud]")
{
    const std::string body = R"({"code":0,"result":{"totalCount":3,"list":[
        {"type":9,"deviceName":"dn1","tbId":"tb1","aliasName":"K2 Plus","model":"F008","deviceState":0,"multicolor":1,
         "printInfo":{"modelName":"benchy","printProgress":42,"printLeftTime":600}},
        {"type":1,"deviceName":"dn2","tbId":"tb2","aliasName":"","model":"CR-K1 Max","deviceState":-1},
        {"type":3,"deviceName":"lan-only","model":"Ender"}]}})";
    std::vector<CC::Device> devices;
    std::string             error;
    REQUIRE(CC::parse_devices(body, devices, error) == CC::CODE_OK);
    REQUIRE(devices.size() == 2);
    CHECK(devices[0].dn == "dn1");
    CHECK(devices[0].tb_id == "tb1");
    CHECK(devices[0].online);
    CHECK(devices[0].multi_color);
    CHECK(devices[0].progress == 42);
    CHECK(devices[0].print_file == "benchy");
    CHECK_FALSE(devices[1].online);
    CHECK(devices[1].model == "K1 Max");
    CHECK(devices[1].name == "K1 Max"); // No alias: falls back to the model
}

TEST_CASE("Creality Cloud upload helpers", "[CrealityCloud]")
{
    SECTION("object key matches Creality Print's naming")
    {
        CHECK(CC::gcode_object_key("Benchy.gcode", "0123abcd") == "model/slice/BE1F7D86071D6D86ED7A2EC1FB104481.gcode.gz");
        CHECK(CC::gcode_object_key("Benchy", "0123abcd") == CC::gcode_object_key("Benchy.gcode", "0123abcd"));
    }
    SECTION("OSS V1 signature")
    {
        const std::string sig = CC::oss_signature("secret", "PUT", "", "application/octet-stream", "Thu, 17 Nov 2005 18:49:58 GMT",
                                                  {{"x-oss-security-token", "STS"}}, "/bucket/model/slice/A.gcode.gz");
        CHECK(sig == "XIDJqa5L+WNQdMj/i4cVE00rGY8=");
    }
    SECTION("upload record and credentials")
    {
        CC::UploadedGcode g;
        std::string       error;
        REQUIRE(CC::parse_uploaded_gcode(R"({"code":0,"result":{"list":[{"id":"g1","name":"benchy","filekey":"model/slice/X.gcode.gz"}]}})", g,
                                         error) == CC::CODE_OK);
        CHECK(g.cdn_url() == "https://file-cdn.creality.com/model/slice/X.gcode.gz");

        CC::OssCredentials creds;
        REQUIRE(CC::parse_aliyun_info(R"({"code":0,"result":{"aliyunInfo":{"accessKeyId":"id","secretAccessKey":"sk","sessionToken":"st"}}})",
                                      creds, error) == CC::CODE_OK);
        REQUIRE(CC::parse_oss_info(R"({"code":0,"result":{"info":{"endpoint":"oss-us-east-1.aliyuncs.com","file":{"bucket":"b"}}}})",
                                   creds, error) == CC::CODE_OK);
        CHECK(creds.security_token == "st");
        CHECK(creds.bucket == "b");
        CHECK(CC::parse_aliyun_info(R"({"code":4,"msg":"token expired"})", creds, error) == CC::CODE_TOKEN_INVALID);
    }
    SECTION("gzip output inflates back to the input")
    {
        namespace fs        = boost::filesystem;
        const fs::path dir  = fs::temp_directory_path() / fs::unique_path("cc-gz-%%%%");
        fs::create_directories(dir);
        const std::string text(100000, 'G');
        {
            boost::nowide::ofstream out((dir / "in.gcode").string(), std::ios::binary);
            out << text;
        }
        REQUIRE(CC::gzip_file((dir / "in.gcode").string(), (dir / "out.gz").string(), {}));
        gzFile gz = gzopen((dir / "out.gz").string().c_str(), "rb");
        REQUIRE(gz != nullptr);
        std::string back(text.size() + 16, '\0');
        const int   n = gzread(gz, back.data(), unsigned(back.size()));
        gzclose(gz);
        back.resize(n > 0 ? size_t(n) : 0);
        CHECK(back == text);
        fs::remove_all(dir);
    }
}

TEST_CASE("Creality Cloud print task body", "[CrealityCloud]")
{
    CC::UploadedGcode g;
    g.id       = "g1";
    g.file_key = "model/slice/X.gcode.gz";

    SECTION("single-colour printer sends no CFS fields")
    {
        const json body = CC::add_task_body("dn1", g, "benchy", {}, false, false);
        CHECK(body["deviceName"] == "dn1");
        CHECK(body["gcodeId"] == "g1");
        CHECK(body["gcodeFilePath"] == "https://file-cdn.creality.com/model/slice/X.gcode.gz");
        CHECK_FALSE(body.contains("enableCfs"));
    }
    SECTION("CFS mapping")
    {
        const auto slots = CC::filament_slots("[0, 5, -1, 255]", {"PLA", "PETG", "PLA"}, {"#ff0000", "#00FF00", "#0000ff"});
        REQUIRE(slots.size() == 2);
        CHECK(slots[0].slot_id == "T1A");
        CHECK(slots[1].slot_id == "T2B");
        CHECK(slots[1].extruder == 2);

        const json body = CC::add_task_body("dn1", g, "benchy", slots, true, true);
        CHECK(body["enableCfs"] == 1);
        REQUIRE(body["filamentsList"].size() == 2);
        CHECK(body["filamentsList"][0] == json({{"cId", "T1A"}, {"filamentId", "T1A"}, {"filamentType", "PLA"}, {"filamentsColor", "#FFFF0000"}}));
        CHECK(body["filamentsList"][1]["filamentId"] == "T1B"); // Second G-code extruder
        CHECK(body["filamentsList"][1]["cId"] == "T2B");
    }
    SECTION("CFS off on a CFS printer")
    {
        const json body = CC::add_task_body("dn1", g, "benchy", {}, true, false);
        CHECK(body["enableCfs"] == 0);
        CHECK_FALSE(body.contains("filamentsList"));
    }
}

TEST_CASE("Creality Cloud device state translates to the device panel messages", "[CrealityCloud]")
{
    CC::Device d;
    d.dn       = "dn1";
    d.name     = "K2";
    d.model    = "F008";
    d.online   = true;
    d.state    = 1;
    d.progress = 10;

    const json ssdp = CC::ssdp_payload(d);
    CHECK(ssdp["dev_id"] == "dn1");
    CHECK(ssdp["connect_type"] == "cloud");
    CHECK(ssdp["status"] == "Printing");
    CHECK(ssdp["online"] == true);

    const json telemetry = CC::parse_timeseries(
        R"({"state":[{"ts":1,"value":5}],"printProgress":[{"ts":1,"value":"77"}],"nozzleTemp":[{"ts":1,"value":"210.5"}]})");
    const json status = CC::status_payload(d, telemetry);
    CHECK(status["print"]["gcode_state"] == "PAUSE");
    CHECK(status["print"]["mc_percent"] == 77);
    CHECK(status["print"]["nozzle_temper"].get<float>() == 210.5f);

    d.online = false;
    CHECK(CC::status_payload(d, json::object())["print"]["gcode_state"] == "OFFLINE");
}

TEST_CASE("Creality Cloud extruder labels", "[CrealityCloud]")
{
    CHECK(CC::extruder_slot_label(1) == "T1A");
    CHECK(CC::extruder_slot_label(4) == "T1D");
    CHECK(CC::extruder_slot_label(5) == "T2A");
    CHECK(CC::extruder_slot_label(16) == "T4D");
}

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/LanDevicePool.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

using namespace Slic3r;

namespace {

DynamicPrintConfig printer_config(const std::string& agent, const std::string& host, const std::string& port = "",
                                  const std::string& api_key = "")
{
    DynamicPrintConfig config;
    config.set_key_value("printer_agent", new ConfigOptionString(agent));
    config.set_key_value("print_host", new ConfigOptionString(host));
    config.set_key_value("printhost_port", new ConfigOptionString(port));
    config.set_key_value("printhost_apikey", new ConfigOptionString(api_key));
    config.set_key_value("printhost_user", new ConfigOptionString(""));
    config.set_key_value("printhost_password", new ConfigOptionString(""));
    config.set_key_value("printhost_cafile", new ConfigOptionString(""));
    return config;
}

} // namespace

TEST_CASE("A preset with a print host and a printer agent is a Multi-device LAN printer", "[LanDevicePool]")
{
    const auto device = lan_device_from_config("U1 left", printer_config("snapmaker", "http://192.0.2.10:7125/", "", "key"), "797581801");
    REQUIRE(device);
    CHECK(device->dev_id == "192.0.2.10:7125");
    CHECK(device->host == "192.0.2.10:7125");
    CHECK(device->agent_id == "snapmaker");
    CHECK(device->password == "key");
    CHECK(device->printer_type == "797581801");
    CHECK_FALSE(device->use_ssl);

    const auto secure = lan_device_from_config("Kobra", printer_config("kobracontrol", "https://192.0.2.11", "18910"), "");
    REQUIRE(secure);
    CHECK(secure->use_ssl);
    CHECK(secure->host == "192.0.2.11");
    CHECK(secure->port == "18910");
}

TEST_CASE("Bambu printers, the orca stub and presets without a host stay off the Multi-device LAN list", "[LanDevicePool]")
{
    CHECK_FALSE(lan_device_from_config("X1", printer_config("bbl", "192.0.2.12"), ""));
    CHECK_FALSE(lan_device_from_config("Any", printer_config("orca", "192.0.2.13"), ""));
    CHECK_FALSE(lan_device_from_config("Any", printer_config("", "192.0.2.13"), ""));
    CHECK_FALSE(lan_device_from_config("U1", printer_config("snapmaker", ""), ""));
}

TEST_CASE("A Multi-device print names the printer it is sent to", "[LanDevicePool]")
{
    const auto device = lan_device_from_config("U1 right", printer_config("u1control", "192.0.2.20", "7125", "secret"), "");
    REQUIRE(device);
    PrintParams params{};
    params.dev_id   = "192.0.2.20:7125";
    params.dev_ip   = "192.0.2.99"; // whatever the page filled in
    params.filename = "/tmp/plate.gcode.3mf";
    const PrintParams sent = lan_print_params(*device, params);
    CHECK(sent.dev_ip == "192.0.2.20");
    CHECK(sent.password == "secret");
    CHECK(sent.connection_type == "lan");
    CHECK(sent.dev_name == "U1 right");
    CHECK(sent.filename == "/tmp/plate.gcode.3mf");
}

TEST_CASE("A renamed Multi-device printer keeps its connection, a new address or key does not", "[LanDevicePool]")
{
    const auto device = lan_device_from_config("U1", printer_config("u1control", "192.0.2.20", "7125", "secret"), "797581801");
    REQUIRE(device);
    const auto renamed = lan_device_from_config("U1 left", printer_config("u1control", "192.0.2.20", "7125", "secret"), "");
    REQUIRE(renamed);
    CHECK_FALSE(*renamed == *device);
    CHECK(renamed->same_connection(*device));
    CHECK_FALSE(lan_device_from_config("U1", printer_config("u1control", "192.0.2.20", "7125", "other"), "")->same_connection(*device));
    CHECK_FALSE(lan_device_from_config("U1", printer_config("snapmaker", "192.0.2.20", "7125", "secret"), "")->same_connection(*device));
}

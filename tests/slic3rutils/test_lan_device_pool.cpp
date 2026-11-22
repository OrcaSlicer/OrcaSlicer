#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/LanDevicePool.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/OrcaPrinterAgent.hpp"
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

// A printer agent that records what the pool asks of it
class FakeAgent final : public OrcaPrinterAgent
{
public:
    FakeAgent() : OrcaPrinterAgent("") {}

    int connect_printer(const PrinterConnectionParams& params) override
    {
        connected.push_back(params);
        return 0;
    }
    int disconnect_printer() override
    {
        ++disconnects;
        return 0;
    }
    int start_local_print(PrintParams params, OnUpdateStatusFn, WasCancelledFn) override
    {
        printed.push_back(std::move(params));
        return 0;
    }
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override
    {
        on_connected = std::move(fn);
        return 0;
    }

    std::vector<PrinterConnectionParams> connected;
    std::vector<PrintParams>             printed;
    int                                  disconnects{0};
    OnPrinterConnectedFn                 on_connected;
};

// Agent ids the fake factory serves; only the first is registered, so prune() drops the other's agents
constexpr char REGISTERED_AGENT[]   = "pool-test-agent";
constexpr char UNREGISTERED_AGENT[] = "pool-test-gone";

// A pool on the test's thread: background jobs and UI-thread calls run at once
struct PoolFixture
{
    std::vector<std::shared_ptr<FakeAgent>> agents; // every agent made, in order
    bool                                    factory_fails{false};
    std::shared_ptr<LanDevicePool>          pool;

    PoolFixture()
    {
        NetworkAgentFactory::register_printer_agent(REGISTERED_AGENT, "Pool test", [](std::shared_ptr<ICloudServiceAgent>, const std::string&) {
            return std::make_shared<FakeAgent>();
        });
        LanDevicePool::Hooks hooks;
        hooks.create_agent = [this](const std::string&) -> std::shared_ptr<IPrinterAgent> {
            if (factory_fails)
                return nullptr;
            agents.push_back(std::make_shared<FakeAgent>());
            return agents.back();
        };
        hooks.run_on_main       = [](const LanDevicePool::Task& task) { task(); };
        hooks.run_in_background = [](const LanDevicePool::Task& task) { task(); };
        pool                    = std::make_shared<LanDevicePool>(nullptr, hooks);
    }

    std::optional<int> print(const std::string& dev_id) const
    {
        PrintParams params{};
        params.dev_id = dev_id;
        return pool->start_print(dev_id, params, nullptr, nullptr);
    }
};

LanDevice device(const std::string& name, const std::string& host, const std::string& agent = REGISTERED_AGENT, const std::string& key = "")
{
    auto result = lan_device_from_config(name, printer_config(agent, host, "", key), "");
    REQUIRE(result);
    return *result;
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

TEST_CASE("Only the Multi-device printers picked in Manage devices are connected", "[LanDevicePool]")
{
    PoolFixture f;
    const LanDevice left = device("Left", "192.0.2.30"), right = device("Right", "192.0.2.31");
    f.pool->sync({left, right}, {left.dev_id});

    CHECK(f.pool->machines().size() == 2);
    CHECK(f.pool->owns(right.dev_id));
    REQUIRE(f.agents.size() == 1);
    REQUIRE(f.agents[0]->connected.size() == 1);
    CHECK(f.agents[0]->connected[0].host == "192.0.2.30");

    // A refresh with nothing changed connects nothing more
    f.pool->sync({left, right}, {left.dev_id});
    CHECK(f.agents.size() == 1);

    // Picked later: connected then; unpicked: disconnected
    f.pool->sync({left, right}, {right.dev_id});
    REQUIRE(f.agents.size() == 2);
    CHECK(f.agents[0]->disconnects == 1);
    REQUIRE(f.agents[1]->connected.size() == 1);
    CHECK(f.agents[1]->connected[0].host == "192.0.2.31");
}

TEST_CASE("A print to a Multi-device LAN printer goes through its own agent, or nowhere", "[LanDevicePool]")
{
    PoolFixture f;
    const LanDevice left = device("Left", "192.0.2.30", REGISTERED_AGENT, "key"), right = device("Right", "192.0.2.31");
    f.pool->sync({left, right}, {left.dev_id});

    CHECK(f.print(left.dev_id) == 0);
    REQUIRE(f.agents[0]->printed.size() == 1);
    CHECK(f.agents[0]->printed[0].dev_ip == "192.0.2.30");
    CHECK(f.agents[0]->printed[0].password == "key");

    // Listed but not connected: an error, never the live agent
    CHECK(f.print(right.dev_id) == BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED);
    // Not a printer of the pool (a Bambu printer): the live agent's
    CHECK_FALSE(f.print("01P00A000000000").has_value());

    // Removed since the print was queued: an error, and its agent is gone
    f.pool->sync({right}, {left.dev_id});
    CHECK_FALSE(f.pool->owns(left.dev_id));
    CHECK(f.agents[0]->disconnects == 1);
    CHECK(f.print(left.dev_id) == BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED);
    CHECK(f.agents[0]->printed.size() == 1);
}

TEST_CASE("A Multi-device printer whose agent cannot be made does not fall back to the live agent", "[LanDevicePool]")
{
    PoolFixture f;
    f.factory_fails      = true;
    const LanDevice left = device("Left", "192.0.2.30");
    f.pool->sync({left}, {left.dev_id});
    CHECK(f.pool->owns(left.dev_id));
    CHECK(f.print(left.dev_id) == BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED);

    // Not tried again on every refresh
    f.factory_fails = false;
    f.pool->sync({left}, {left.dev_id});
    CHECK(f.agents.empty());
}

TEST_CASE("A Multi-device printer with a new key is connected again, with the same MachineObject", "[LanDevicePool]")
{
    PoolFixture f;
    const LanDevice old_key = device("Left", "192.0.2.30", REGISTERED_AGENT, "old");
    f.pool->sync({old_key}, {old_key.dev_id});
    MachineObject* machine = f.pool->machines().at(old_key.dev_id);
    const OnPrinterConnectedFn old_callback = f.agents[0]->on_connected;

    const LanDevice new_key = device("Left", "192.0.2.30", REGISTERED_AGENT, "new");
    f.pool->sync({new_key}, {new_key.dev_id});
    REQUIRE(f.agents.size() == 2);
    CHECK(f.agents[0]->disconnects == 1);
    REQUIRE(f.agents[1]->connected.size() == 1);
    CHECK(f.agents[1]->connected[0].password == "new");
    CHECK(f.pool->machines().at(new_key.dev_id) == machine);

    // The old agent's callbacks reach nothing any more
    machine->set_online_state(false);
    old_callback("");
    CHECK_FALSE(machine->is_online());

    // Renamed only: the connection stays
    f.pool->sync({device("Left renamed", "192.0.2.30", REGISTERED_AGENT, "new")}, {new_key.dev_id});
    CHECK(f.agents.size() == 2);
    CHECK(machine->get_dev_name() == "Left renamed");
}

TEST_CASE("A Multi-device agent's callbacks reach its printer by the pool's dev_id", "[LanDevicePool]")
{
    PoolFixture f;
    const LanDevice left = device("Left", "192.0.2.30");
    f.pool->sync({left}, {left.dev_id});
    MachineObject* machine = f.pool->machines().at(left.dev_id);
    REQUIRE_FALSE(machine->is_online());

    f.agents[0]->on_connected("an id the agent made up");
    CHECK(machine->is_online());
}

TEST_CASE("Pruning drops the agents of a printer agent that is no longer registered", "[LanDevicePool]")
{
    PoolFixture f;
    const LanDevice kept = device("Kept", "192.0.2.30"), gone = device("Gone", "192.0.2.31", UNREGISTERED_AGENT);
    f.pool->sync({kept, gone}, {kept.dev_id, gone.dev_id});
    REQUIRE(f.agents.size() == 2);

    f.pool->prune();
    CHECK(f.agents[0]->disconnects == 0);
    CHECK(f.agents[1]->disconnects == 1);
    // Still listed, but nothing reaches the old instance
    CHECK(f.pool->owns(gone.dev_id));
    CHECK(f.print(gone.dev_id) == BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED);
    CHECK(f.agents[1]->printed.empty());
}

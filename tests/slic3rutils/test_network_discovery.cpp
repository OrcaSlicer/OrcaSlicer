#include <catch2/catch_all.hpp>

#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

using namespace Slic3r;

namespace {

class DiscoverySource : public MoonrakerPrinterAgent
{
public:
    DiscoverySource() : MoonrakerPrinterAgent("") {}

    OnMsgArrivedFn discovery_callback;

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override
    {
        discovery_callback = std::move(fn);
        return 0;
    }
};

} // namespace

TEST_CASE("Queued discovery cannot cross printer agent sessions", "[NetworkDiscovery]")
{
    auto source = std::make_shared<DiscoverySource>();
    NetworkAgent network(nullptr, source);
    std::vector<std::function<void()>> pending;
    std::vector<std::string> delivered;
    network.set_queue_on_main_fn([&](std::function<void()> fn) { pending.push_back(std::move(fn)); });
    network.set_on_ssdp_msg_fn([&](std::string payload) { delivered.push_back(std::move(payload)); });

    auto outgoing_callback = source->discovery_callback;
    outgoing_callback("unsaved-device");
    outgoing_callback("saved-moonraker-device");
    REQUIRE(delivered.empty());
    REQUIRE(pending.size() == 2);

    bool expect_active = true;
    SECTION("Switch to another agent")
    {
        network.set_printer_agent(std::make_shared<DiscoverySource>());
    }
    SECTION("Clear the live agent")
    {
        network.set_printer_agent(nullptr);
        expect_active = false;
    }
    SECTION("Switch away and back to the cached agent")
    {
        network.set_printer_agent(std::make_shared<DiscoverySource>());
        network.set_printer_agent(source);
    }
    SECTION("Unregister discovery")
    {
        network.set_on_ssdp_msg_fn(nullptr);
        expect_active = false;
    }
    SECTION("Replace the discovery listener")
    {
        network.set_on_ssdp_msg_fn([&](std::string payload) { delivered.push_back(std::move(payload)); });
    }

    for (auto& fn : pending)
        fn();
    pending.clear();
    REQUIRE(delivered.empty());

    outgoing_callback("late-outgoing-device");
    REQUIRE(pending.empty());
    REQUIRE(delivered.empty());

    if (expect_active) {
        auto active = std::dynamic_pointer_cast<DiscoverySource>(network.get_printer_agent());
        REQUIRE(active);
        REQUIRE(active->discovery_callback);
        active->discovery_callback("active-device");
        REQUIRE(delivered.empty());
        REQUIRE(pending.size() == 1);
        pending.front()();
        REQUIRE(delivered == std::vector<std::string>{"active-device"});
    }
}

TEST_CASE("Discovery callbacks expire when networking is destroyed", "[NetworkDiscovery]")
{
    auto source = std::make_shared<DiscoverySource>();
    std::vector<std::function<void()>> pending;
    unsigned delivered = 0;
    {
        NetworkAgent network(nullptr, source);
        network.set_queue_on_main_fn([&](std::function<void()> fn) { pending.push_back(std::move(fn)); });
        network.set_on_ssdp_msg_fn([&](std::string) { ++delivered; });
        source->discovery_callback("queued-device");
    }

    REQUIRE(pending.size() == 1);
    pending.front()();
    pending.clear();
    source->discovery_callback("late-device");
    REQUIRE(pending.empty());
    REQUIRE(delivered == 0);
}

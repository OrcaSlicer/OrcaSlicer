#include <catch2/catch_all.hpp>

#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"

using namespace Slic3r;

namespace {

class DiscoveryAgent : public MoonrakerPrinterAgent
{
public:
    using MoonrakerPrinterAgent::MoonrakerPrinterAgent;
    using MoonrakerPrinterAgent::device_info;
    mutable unsigned fetch_count = 0;

protected:
    bool fetch_device_info(const std::string&, const std::string&, MoonrakerDeviceInfo& info, std::string&) const override
    {
        ++fetch_count;
        info.dev_name = "Test printer";
        return true;
    }
};

} // namespace

TEST_CASE("Moonraker discovery waits for a complete device identity", "[MoonrakerDiscovery]")
{
    DiscoveryAgent agent("");
    unsigned announcements = 0;
    nlohmann::json announcement;
    agent.set_on_ssdp_msg_fn([&](std::string payload) {
        ++announcements;
        announcement = nlohmann::json::parse(payload);
    });

    agent.start_discovery(true, false);
    REQUIRE(announcements == 0);
    REQUIRE(agent.fetch_count == 0);

    agent.device_info.dev_id = "127.0.0.1:7125";
    agent.device_info.dev_ip = agent.device_info.dev_id;
    agent.device_info.base_url = "http://127.0.0.1:7125";
    agent.start_discovery(true, false);
    REQUIRE(announcements == 1);
    REQUIRE(agent.fetch_count == 1);
    REQUIRE(announcement.at("dev_id") == agent.device_info.dev_id);
    REQUIRE(announcement.at("dev_ip") == agent.device_info.dev_ip);
    REQUIRE(announcement.at("dev_name") == "Test printer");
}

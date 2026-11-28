#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

using namespace Slic3r;

namespace {

class DiscoverySource : public MoonrakerPrinterAgent
{
public:
    explicit DiscoverySource(std::string id, bool scans = true)
        : MoonrakerPrinterAgent(""), scans(scans), m_info{std::move(id), "Discovery source", "1.0", "test agent"}
    {
    }

    AgentInfo get_agent_info() override { return m_info; }
    bool discovers_on_lan() const override { return scans; }

    bool start_discovery(bool start, bool) override
    {
        discovering = start;
        ++(start ? starts : stops);
        return true;
    }

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override
    {
        discovery_callback = std::move(fn);
        return 0;
    }

    bool scans;
    bool discovering{false};
    int starts{0};
    int stops{0};
    OnMsgArrivedFn discovery_callback;

private:
    AgentInfo m_info;
};

struct Rig
{
    std::vector<std::function<void()>> pending;
    std::vector<std::pair<std::string, std::string>> delivered; // agent id, payload
    NetworkAgent network{nullptr, nullptr};

    Rig()
    {
        network.set_queue_on_main_fn([this](std::function<void()> fn) { pending.push_back(std::move(fn)); });
        network.set_on_discovery_fn(
            [this](const std::string& agent_id, std::string payload) { delivered.emplace_back(agent_id, std::move(payload)); });
    }

    void run_pending()
    {
        auto queued = std::move(pending);
        pending.clear();
        for (auto& fn : queued)
            fn();
    }
};

using Agents = std::vector<std::shared_ptr<IPrinterAgent>>;

} // namespace

TEST_CASE("Every scanning agent discovers at once and reports under its own id", "[NetworkDiscovery]")
{
    Rig rig;
    auto a = std::make_shared<DiscoverySource>("agent-a");
    auto b = std::make_shared<DiscoverySource>("agent-b");
    rig.network.sync_discovery(Agents{a, b}, {});

    CHECK(a->discovering);
    CHECK(b->discovering);

    a->discovery_callback("from-a");
    b->discovery_callback("from-b");
    rig.run_pending();
    REQUIRE(rig.delivered.size() == 2);
    CHECK(rig.delivered[0] == std::make_pair(std::string("agent-a"), std::string("from-a")));
    CHECK(rig.delivered[1] == std::make_pair(std::string("agent-b"), std::string("from-b")));
}

TEST_CASE("A disabled agent does not start discovery", "[NetworkDiscovery]")
{
    Rig rig;
    auto a = std::make_shared<DiscoverySource>("agent-a");
    auto b = std::make_shared<DiscoverySource>("agent-b");

    SECTION("Not live")
    {
        rig.network.sync_discovery(Agents{a, b}, {"agent-b"});
    }
    SECTION("Live")
    {
        rig.network.set_printer_agent(b);
        rig.network.sync_discovery(Agents{a, b}, {"agent-b"});
    }

    CHECK(a->discovering);
    CHECK_FALSE(b->discovering);
    CHECK(b->starts == 0);
    CHECK_FALSE(rig.network.is_discovering("agent-b"));
}

TEST_CASE("The disabled agents setting round trips through the app config", "[NetworkDiscovery]")
{
    AppConfig config;
    CHECK(config.get_disabled_discovery_agents().empty());

    config.set_disabled_discovery_agents({"orca", "bbl"});
    CHECK(config.get_disabled_discovery_agents() == std::set<std::string>{"bbl", "orca"});
    CHECK(config.get("disabled_discovery_agents") == "bbl;orca");
}

TEST_CASE("An agent that does not scan reports only while it is live", "[NetworkDiscovery]")
{
    Rig rig;
    auto scanner = std::make_shared<DiscoverySource>("scanner");
    auto host    = std::make_shared<DiscoverySource>("host-reporter", false);

    rig.network.sync_discovery(Agents{scanner, host}, {});
    CHECK(scanner->discovering);
    CHECK_FALSE(host->discovering);

    rig.network.set_printer_agent(host);
    rig.network.sync_discovery(Agents{scanner, host}, {});
    CHECK(host->discovering);

    rig.network.set_printer_agent(scanner);
    rig.network.sync_discovery(Agents{scanner, host}, {});
    CHECK_FALSE(host->discovering);
    CHECK(host->stops == 1);
}

TEST_CASE("An agent switch leaves the scanning agents' discovery running", "[NetworkDiscovery]")
{
    Rig rig;
    auto a = std::make_shared<DiscoverySource>("agent-a");
    auto b = std::make_shared<DiscoverySource>("agent-b");
    rig.network.set_printer_agent(a);
    rig.network.sync_discovery(Agents{a, b}, {});

    auto early = a->discovery_callback;
    rig.network.set_printer_agent(b);
    rig.network.sync_discovery(Agents{a, b}, {});

    CHECK(a->discovering);
    CHECK(b->discovering);
    CHECK(a->starts == 1);
    CHECK(a->stops == 0);
    CHECK(b->starts == 1);

    // The callback registered before the switch is still the live one and still delivers.
    early("after-switch");
    rig.run_pending();
    REQUIRE(rig.delivered.size() == 1);
    CHECK(rig.delivered[0].first == "agent-a");
}

TEST_CASE("Events from a torn-down agent are dropped", "[NetworkDiscovery]")
{
    Rig rig;
    auto a = std::make_shared<DiscoverySource>("agent-a");
    auto b = std::make_shared<DiscoverySource>("agent-b");
    rig.network.sync_discovery(Agents{a, b}, {});

    auto outgoing = a->discovery_callback;
    outgoing("queued-before");
    REQUIRE(rig.pending.size() == 1);

    SECTION("It leaves the discovering set")
    {
        rig.network.sync_discovery(Agents{b}, {});
    }
    SECTION("It is disabled")
    {
        rig.network.sync_discovery(Agents{a, b}, {"agent-a"});
    }
    SECTION("It stops scanning, as when the plug-in unloads")
    {
        a->scans = false;
        rig.network.sync_discovery(Agents{a, b}, {});
    }
    SECTION("It is deregistered")
    {
        rig.network.stop_discovery("agent-a");
    }
    SECTION("Discovery stops everywhere")
    {
        rig.network.stop_all_discovery();
        CHECK_FALSE(b->discovering);
    }

    CHECK_FALSE(a->discovering);
    CHECK(a->stops == 1);

    rig.run_pending();
    outgoing("late");
    CHECK(rig.pending.empty());
    CHECK(rig.delivered.empty());
}

TEST_CASE("Clearing the listener drops queued discovery events", "[NetworkDiscovery]")
{
    Rig rig;
    auto a = std::make_shared<DiscoverySource>("agent-a");
    rig.network.sync_discovery(Agents{a}, {});
    auto callback = a->discovery_callback;
    callback("queued");
    REQUIRE(rig.pending.size() == 1);

    rig.network.set_on_discovery_fn(nullptr);
    rig.run_pending();
    callback("late");
    CHECK(rig.pending.empty());
    CHECK(rig.delivered.empty());
}

TEST_CASE("Replacing an agent under the same id restarts discovery on the new object", "[NetworkDiscovery]")
{
    Rig rig;
    auto old_agent = std::make_shared<DiscoverySource>("agent-a");
    rig.network.sync_discovery(Agents{old_agent}, {});
    auto stale = old_agent->discovery_callback;

    auto new_agent = std::make_shared<DiscoverySource>("agent-a");
    rig.network.sync_discovery(Agents{new_agent}, {});

    CHECK_FALSE(old_agent->discovering);
    CHECK(new_agent->discovering);
    CHECK(old_agent->discovery_callback == nullptr);
    stale("stale");
    CHECK(rig.pending.empty());
}

TEST_CASE("Discovery callbacks expire when networking is destroyed", "[NetworkDiscovery]")
{
    auto source = std::make_shared<DiscoverySource>("agent-a");
    std::vector<std::function<void()>> pending;
    unsigned delivered = 0;
    {
        NetworkAgent network(nullptr, nullptr);
        network.set_queue_on_main_fn([&](std::function<void()> fn) { pending.push_back(std::move(fn)); });
        network.set_on_discovery_fn([&](const std::string&, std::string) { ++delivered; });
        network.sync_discovery(Agents{source}, {});
        source->discovery_callback("queued-device");
    }

    REQUIRE(pending.size() == 1);
    pending.front()();
    pending.clear();
    source->discovery_callback("late-device");
    REQUIRE(pending.empty());
    REQUIRE(delivered == 0);
}

TEST_CASE("A disabled live agent discovers while a picker requests it", "[NetworkDiscovery]")
{
    Rig rig;
    auto live = std::make_shared<DiscoverySource>("agent-a");
    rig.network.set_printer_agent(live);
    rig.network.sync_discovery(Agents{live}, {"agent-a"});
    REQUIRE_FALSE(live->discovering);

    rig.network.set_live_discovery_requested(true);
    rig.network.sync_discovery(Agents{live}, {"agent-a"});
    CHECK(live->discovering);
    live->discovery_callback("while-open");
    rig.run_pending();
    CHECK(rig.delivered.size() == 1);

    rig.network.set_live_discovery_requested(false);
    rig.network.sync_discovery(Agents{live}, {"agent-a"});
    CHECK_FALSE(live->discovering);
    CHECK_FALSE(rig.network.is_discovering("agent-a"));
}

TEST_CASE("Stopping all discovery can spare one agent", "[NetworkDiscovery]")
{
    Rig rig;
    auto a = std::make_shared<DiscoverySource>("agent-a");
    auto b = std::make_shared<DiscoverySource>("agent-b");
    rig.network.sync_discovery(Agents{a, b}, {});

    rig.network.stop_all_discovery("agent-b");
    CHECK_FALSE(a->discovering);
    CHECK(b->discovering);
    CHECK(rig.network.is_discovering("agent-b"));
}

namespace {
int s_constructed = 0;

void register_counting_agent(const std::string& id, bool scans, bool scans_on_lan)
{
    NetworkAgentFactory::register_printer_agent(
        id, id,
        [id, scans](std::shared_ptr<ICloudServiceAgent>, const std::string&) -> std::shared_ptr<IPrinterAgent> {
            ++s_constructed;
            return std::make_shared<DiscoverySource>(id, scans);
        },
        scans_on_lan);
}

struct CacheGuard
{
    ~CacheGuard() { NetworkAgentFactory::clear_printer_agent_cache(); }
};
} // namespace

TEST_CASE("Refreshing discovery candidates does not create an uncached agent", "[NetworkDiscovery]")
{
    const CacheGuard guard;
    register_counting_agent("candidate-uncached", true, false);
    register_counting_agent("candidate-builtin-scanner", true, true);
    register_counting_agent("candidate-opt-in", true, false);

    s_constructed   = 0;
    const auto none = [](const std::string&) { return std::shared_ptr<ICloudServiceAgent>(); };
    const auto find = [](const auto& agents, const std::string& id) {
        return std::any_of(agents.begin(), agents.end(), [&](const auto& a) { return a->get_agent_info().id == id; });
    };

    auto candidates = NetworkAgentFactory::get_discovery_candidates(none, "");
    CHECK(s_constructed == 1);
    CHECK(find(candidates, "candidate-builtin-scanner"));
    CHECK_FALSE(find(candidates, "candidate-uncached"));
    CHECK_FALSE(find(candidates, "candidate-opt-in"));

    // Once a plugin-style agent exists in the cache (it was selected), it joins and may discover.
    REQUIRE(NetworkAgentFactory::create_printer_agent_by_id("candidate-opt-in", nullptr, "") != nullptr);
    candidates = NetworkAgentFactory::get_discovery_candidates(none, "");
    CHECK(s_constructed == 2);
    CHECK(find(candidates, "candidate-opt-in"));
    CHECK_FALSE(find(candidates, "candidate-uncached"));

    NetworkAgent network(nullptr, nullptr);
    network.sync_discovery(candidates, {});
    CHECK(network.is_discovering("candidate-opt-in"));
    CHECK_FALSE(network.is_discovering("candidate-uncached"));
}

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include "slic3r/GUI/DeviceCore/DevAgentOrder.h"

using namespace Slic3r;

namespace {

std::vector<size_t> indices_of(const std::vector<DevAgentListEntry>& entries)
{
    std::vector<size_t> out;
    for (const auto& e : entries)
        out.push_back(e.index);
    return out;
}

} // namespace

TEST_CASE("Printer list puts the live agent first then other agents in registration order", "[DeviceManager]")
{
    const std::vector<std::string> registration{"orca", "bbl", "moonraker"};
    const std::vector<std::string> agents{"moonraker", "bbl", "orca", "moonraker", "orca", "bbl"};

    const auto order = order_printers_by_agent(agents, "bbl", registration);

    REQUIRE(order.size() == agents.size());
    // live agent bbl (1, 5), then orca (2, 4), then moonraker (0, 3); each keeps its input order
    CHECK(indices_of(order) == std::vector<size_t>{1, 5, 2, 4, 0, 3});
}

TEST_CASE("Printer list puts printers of unknown agents last", "[DeviceManager]")
{
    const std::vector<std::string> registration{"orca", "bbl"};
    const std::vector<std::string> agents{"gone", "", "bbl", "orca", "other"};

    const auto order = order_printers_by_agent(agents, "orca", registration);

    CHECK(indices_of(order) == std::vector<size_t>{3, 2, 0, 1, 4});
}

TEST_CASE("Printer list marks only live agent printers usable", "[DeviceManager]")
{
    const std::vector<std::string> agents{"bbl", "orca", "gone", "orca"};

    const auto order = order_printers_by_agent(agents, "orca", {"orca", "bbl"});

    REQUIRE(order.size() == 4);
    for (const auto& e : order) {
        INFO("printer " << e.index);
        CHECK(e.usable == (agents[e.index] == "orca"));
    }
}

TEST_CASE("Printer list of a single agent keeps its order", "[DeviceManager]")
{
    const std::vector<std::string> agents{"orca", "orca", "orca"};

    const auto order = order_printers_by_agent(agents, "orca", {"orca", "bbl"});

    CHECK(indices_of(order) == std::vector<size_t>{0, 1, 2});
    for (const auto& e : order)
        CHECK(e.usable);
}

TEST_CASE("Printer list is empty for no printers", "[DeviceManager]")
{
    CHECK(order_printers_by_agent({}, "orca", {"orca"}).empty());
}

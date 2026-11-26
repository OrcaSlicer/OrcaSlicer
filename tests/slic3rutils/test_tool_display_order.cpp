#include "slic3r/GUI/DeviceCore/ToolDisplayOrder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace Slic3r;

TEST_CASE("A tool display order is the identity unless a printer reorders it", "[ToolDisplayOrder]")
{
    const ToolDisplayOrder order{{0, 1, 2, 3}};

    REQUIRE(order.is_valid());
    CHECK(order.size() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(order.tool_at(i) == i);
        CHECK(order.display_index_of(i) == i);
    }
}

TEST_CASE("A Bambu dual-nozzle display order swaps the two tools", "[ToolDisplayOrder]")
{
    const ToolDisplayOrder order{{1, 0}};

    REQUIRE(order.is_valid());
    CHECK(order.size() == 2);
    CHECK(order.tool_at(0) == 1);
    CHECK(order.tool_at(1) == 0);
    CHECK(order.display_index_of(0) == 1);
    CHECK(order.display_index_of(1) == 0);
}

TEST_CASE("A tool display order supports any permutation of the tools", "[ToolDisplayOrder]")
{
    const ToolDisplayOrder order{{3, 1, 2, 0}};

    REQUIRE(order.is_valid());
    CHECK(order.size() == 4);
    CHECK(order.tool_at(0) == 3);
    CHECK(order.tool_at(1) == 1);
    CHECK(order.tool_at(2) == 2);
    CHECK(order.tool_at(3) == 0);
    CHECK(order.display_index_of(0) == 3);
    CHECK(order.display_index_of(1) == 1);
    CHECK(order.display_index_of(2) == 2);
    CHECK(order.display_index_of(3) == 0);
}

TEST_CASE("A rotated tool display order maps both directions independently", "[ToolDisplayOrder]")
{
    const ToolDisplayOrder order{{1, 2, 0}};

    REQUIRE(order.is_valid());
    CHECK(order.size() == 3);
    CHECK(order.tool_at(0) == 1);
    CHECK(order.tool_at(1) == 2);
    CHECK(order.tool_at(2) == 0);
    CHECK(order.display_index_of(1) == 0);
    CHECK(order.display_index_of(2) == 1);
    CHECK(order.display_index_of(0) == 2);
}

TEST_CASE("A malformed tool display order has no lookups", "[ToolDisplayOrder]")
{
    SECTION("a duplicate id")
    {
        const ToolDisplayOrder order{{0, 1, 1}};
        CHECK_FALSE(order.is_valid());
        CHECK(order.tool_at(0) == -1);
        CHECK(order.display_index_of(0) == -1);
    }

    SECTION("an id past the end of the list")
    {
        const ToolDisplayOrder order{{0, 3}};
        CHECK_FALSE(order.is_valid());
        CHECK(order.tool_at(0) == -1);
        CHECK(order.display_index_of(0) == -1);
    }

    SECTION("a negative id")
    {
        const ToolDisplayOrder order{{-1, 0}};
        CHECK_FALSE(order.is_valid());
        CHECK(order.tool_at(0) == -1);
        CHECK(order.display_index_of(0) == -1);
    }

    SECTION("more tools than the order lists")
    {
        const ToolDisplayOrder order({0, 1}, 3);
        CHECK_FALSE(order.is_valid());
        CHECK(order.tool_at(0) == -1);
        CHECK(order.display_index_of(0) == -1);
    }

    SECTION("fewer tools than the order lists")
    {
        const ToolDisplayOrder order({0, 1, 2}, 2);
        CHECK_FALSE(order.is_valid());
        CHECK(order.tool_at(0) == -1);
        CHECK(order.display_index_of(0) == -1);
    }
}

TEST_CASE("A single tool has a single display position", "[ToolDisplayOrder]")
{
    const ToolDisplayOrder order{{0}};

    REQUIRE(order.is_valid());
    CHECK(order.size() == 1);
    CHECK(order.tool_at(0) == 0);
    CHECK(order.display_index_of(0) == 0);
    CHECK(order.tool_at(-1) == -1);
    CHECK(order.tool_at(1) == -1);
    CHECK(order.display_index_of(-1) == -1);
    CHECK(order.display_index_of(1) == -1);
}

TEST_CASE("A tool display order matching the printer's tool count is valid", "[ToolDisplayOrder]")
{
    const ToolDisplayOrder order({1, 0, 3, 2}, 4);

    REQUIRE(order.is_valid());
    CHECK(order.size() == 4);
    CHECK(order.tool_at(0) == 1);
    CHECK(order.display_index_of(1) == 0);
}

TEST_CASE("Lookups outside a valid tool display order return -1", "[ToolDisplayOrder]")
{
    const ToolDisplayOrder order{{2, 0, 1}};

    REQUIRE(order.is_valid());
    CHECK(order.tool_at(-1) == -1);
    CHECK(order.tool_at(3) == -1);
    CHECK(order.display_index_of(-1) == -1);
    CHECK(order.display_index_of(3) == -1);
}

TEST_CASE("An order never supplied is invalid", "[ToolDisplayOrder]")
{
    const ToolDisplayOrder order;

    CHECK_FALSE(order.is_valid());
    CHECK(order.size() == 0);
    CHECK(order.tool_at(0) == -1);
    CHECK(order.display_index_of(0) == -1);
}

TEST_CASE("A filament resolves through the identity or a swapped physical map", "[ToolDisplayOrder]")
{
    const std::vector<int> filament_map{1, 2};

    SECTION("identity")
    {
        const std::vector<int> physical_extruder_map{0, 1};
        CHECK(resolve_filament_device_id(0, filament_map, physical_extruder_map) == 0);
        CHECK(resolve_filament_device_id(1, filament_map, physical_extruder_map) == 1);
    }

    SECTION("swap")
    {
        const std::vector<int> physical_extruder_map{1, 0};
        CHECK(resolve_filament_device_id(0, filament_map, physical_extruder_map) == 1);
        CHECK(resolve_filament_device_id(1, filament_map, physical_extruder_map) == 0);
    }
}

TEST_CASE("Bambu's [1,0] physical map resolves both filaments", "[ToolDisplayOrder]")
{
    const std::vector<int> filament_map{1, 2};
    const std::vector<int> physical_extruder_map{1, 0};

    CHECK(resolve_filament_device_id(0, filament_map, physical_extruder_map) == 1);
    CHECK(resolve_filament_device_id(1, filament_map, physical_extruder_map) == 0);
}

TEST_CASE("An invalid saved filament assignment resolves to -1", "[ToolDisplayOrder]")
{
    const std::vector<int> physical_extruder_map{0, 1};
    const std::vector<int> filament_map{1, 2};
    const std::vector<int> no_assignments;
    const std::vector<int> zero_based{0, 1};
    const std::vector<int> negative{-1, 1};
    const std::vector<int> past_the_map{1, 3};

    CHECK(resolve_filament_device_id(-1, filament_map, physical_extruder_map) == -1);
    CHECK(resolve_filament_device_id(2, filament_map, physical_extruder_map) == -1);
    CHECK(resolve_filament_device_id(0, no_assignments, physical_extruder_map) == -1);
    CHECK(resolve_filament_device_id(0, zero_based, physical_extruder_map) == -1);
    CHECK(resolve_filament_device_id(0, negative, physical_extruder_map) == -1);
    CHECK(resolve_filament_device_id(1, past_the_map, physical_extruder_map) == -1);
}

TEST_CASE("An out-of-range physical target resolves to -1", "[ToolDisplayOrder]")
{
    const std::vector<int> filament_map{1, 2};
    const std::vector<int> target_past_the_end{0, 2};
    const std::vector<int> negative_target{0, -1};
    const std::vector<int> no_devices;

    CHECK(resolve_filament_device_id(0, filament_map, target_past_the_end) == 0);
    CHECK(resolve_filament_device_id(1, filament_map, target_past_the_end) == -1);
    CHECK(resolve_filament_device_id(1, filament_map, negative_target) == -1);
    CHECK(resolve_filament_device_id(0, filament_map, no_devices) == -1);
}

TEST_CASE("Several logical tools may route to the same device", "[ToolDisplayOrder]")
{
    const std::vector<int> filament_map{1, 2};
    const std::vector<int> physical_extruder_map{0, 0};

    CHECK(resolve_filament_device_id(0, filament_map, physical_extruder_map) == 0);
    CHECK(resolve_filament_device_id(1, filament_map, physical_extruder_map) == 0);
}

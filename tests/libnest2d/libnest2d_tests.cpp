#include <catch2/catch_all.hpp>

#include "libnest2d_test_utils.hpp"

using namespace libnest2d;

// Basic behaviour of the Item type and the high-level nest() entry point:
// items copy independently, and nest() leaves degenerate or oversized items
// untouched.

TEST_CASE("Item construction and copy", "[Nesting]") {
    Item sh = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
    REQUIRE(sh.vertexCount() == 4u);

    Item sh2({ {0, 0}, {1, 0}, {1, 1}, {0, 1} });
    REQUIRE(sh2.vertexCount() == 4u);

    Item sh3 = sh2;                 // copy
    REQUIRE(sh3.vertexCount() == 4u);

    sh2 = {};                       // clearing the original leaves the copy intact
    REQUIRE(sh2.vertexCount() == 0u);
    REQUIRE(sh3.vertexCount() == 4u);
}

TEST_CASE("nest() leaves an empty or zero-area item untouched", "[Nesting]") {
    auto bin = Box(250000000, 210000000);

    std::vector<Item> items;
    items.emplace_back(Item{});             // empty item
    items.emplace_back(Item{ {0, 200} });   // zero-area item

    size_t bins = nest(items, bin);

    REQUIRE(bins == 0u);
    for (const auto &itm : items) REQUIRE(itm.binId() == BIN_ID_UNSET);
}

TEST_CASE("nest() leaves an item larger than the bin untouched", "[Nesting]") {
    auto bin = Box(250000000, 210000000);

    std::vector<Item> items;
    items.emplace_back(RectangleItem{250000001, 210000001});  // larger than the bin

    size_t bins = nest(items, bin);

    REQUIRE(bins == 0u);
    REQUIRE(items.front().binId() == BIN_ID_UNSET);
}

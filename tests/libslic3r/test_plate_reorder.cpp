#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PlateReorder.hpp"

using namespace Slic3r;

namespace {
// A 256 mm plate in a 307.2 mm cell, as the plate grid spaces them.
constexpr double PlateWidth = 256. / 307.2;
} // namespace

TEST_CASE("A cursor over a plate inserts before or after it by the half it is in", "[PlateReorder]")
{
    // 4 plates in 2 columns: plate 2 is the first plate of the second row.
    const auto left = PlateReorder::drop_target(4, 2, 0.2, 1.5, PlateWidth);
    CHECK(left.plate == 2);
    CHECK_FALSE(left.after);
    CHECK(left.insert == 2);

    const auto right = PlateReorder::drop_target(4, 2, 0.6, 1.5, PlateWidth);
    CHECK(right.plate == 2);
    CHECK(right.after);
    CHECK(right.insert == 3);
}

TEST_CASE("A cursor in the gap right of a plate inserts after it", "[PlateReorder]")
{
    const auto target = PlateReorder::drop_target(4, 2, 0.9, 0.5, PlateWidth);
    CHECK(target.plate == 0);
    CHECK(target.insert == 1);
}

TEST_CASE("A cursor past the last plate inserts after the last plate", "[PlateReorder]")
{
    // 3 plates in 2 columns: the second row has an empty cell.
    const auto target = PlateReorder::drop_target(3, 2, 1.3, 1.5, PlateWidth);
    CHECK(target.plate == 2);
    CHECK(target.after);
    CHECK(target.insert == 3);
}

TEST_CASE("A cursor outside the grid uses the nearest plate", "[PlateReorder]")
{
    CHECK(PlateReorder::drop_target(4, 2, -3., -2., PlateWidth).insert == 0);
    CHECK(PlateReorder::drop_target(4, 2, 9., 9., PlateWidth).insert == 4);
}

TEST_CASE("Inserting a plate gives its new index, or none when the order stays the same", "[PlateReorder]")
{
    CHECK(PlateReorder::index_after_insert(3, 0) == 0);
    CHECK(PlateReorder::index_after_insert(0, 4) == 3);
    CHECK(PlateReorder::index_after_insert(2, 1) == 1);
    CHECK(PlateReorder::index_after_insert(1, 3) == 2);
    CHECK(PlateReorder::index_after_insert(1, 1) == -1);
    CHECK(PlateReorder::index_after_insert(1, 2) == -1);
}

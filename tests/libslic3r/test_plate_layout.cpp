#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "libslic3r/PlateLayout.hpp"

using namespace Slic3r;
using PlateLayout::Cell;

namespace {
constexpr int Limit = 36;
}

TEST_CASE("A plate dropped on a free cell is the only plate that moves", "[PlateLayout]")
{
    const std::vector<Cell> cells{{0, 0}, {1, 0}, {2, 0}};
    const auto              moves = PlateLayout::plan_drop(cells, 2, Cell(0, 1), Limit);
    REQUIRE(moves.size() == 1);
    CHECK(moves.at(2) == Cell(0, 1));
}

TEST_CASE("A plate dropped in its own row shifts the plates in between toward the cell it leaves", "[PlateLayout]")
{
    const std::vector<Cell> cells{{0, 0}, {1, 0}, {2, 0}};
    SECTION("moving right to left")
    {
        const auto moves = PlateLayout::plan_drop(cells, 2, Cell(0, 0), Limit);
        REQUIRE(moves.size() == 3);
        CHECK(moves.at(2) == Cell(0, 0));
        CHECK(moves.at(0) == Cell(1, 0));
        CHECK(moves.at(1) == Cell(2, 0));
    }
    SECTION("moving left to right")
    {
        const auto moves = PlateLayout::plan_drop(cells, 0, Cell(1, 0), Limit);
        REQUIRE(moves.size() == 2);
        CHECK(moves.at(0) == Cell(1, 0));
        CHECK(moves.at(1) == Cell(0, 0));
    }
}

TEST_CASE("A plate dropped in another row pushes the plate there and its right neighbors to the first free cell", "[PlateLayout]")
{
    // Row 0: plates 0, 1 and a gap, then plate 2. Row 1: plate 3.
    const std::vector<Cell> cells{{0, 0}, {1, 0}, {3, 0}, {0, 1}};
    const auto              moves = PlateLayout::plan_drop(cells, 3, Cell(0, 0), Limit);
    REQUIRE(moves.size() == 3);
    CHECK(moves.at(3) == Cell(0, 0));
    CHECK(moves.at(0) == Cell(1, 0));
    CHECK(moves.at(1) == Cell(2, 0));
}

TEST_CASE("A plate can be dropped left of and above cell (0, 0)", "[PlateLayout]")
{
    const std::vector<Cell> cells{{0, 0}, {1, 0}};
    CHECK(PlateLayout::plan_drop(cells, 1, Cell(-1, 0), Limit).at(1) == Cell(-1, 0));
    CHECK(PlateLayout::plan_drop(cells, 1, Cell(0, -1), Limit).at(1) == Cell(0, -1));
}

TEST_CASE("A drop is refused on the plate's own cell or past the limit", "[PlateLayout]")
{
    const std::vector<Cell> cells{{0, 0}, {1, 0}};
    CHECK(PlateLayout::plan_drop(cells, 1, Cell(1, 0), Limit).empty());
    CHECK(PlateLayout::plan_drop(cells, 1, Cell(Limit, 0), Limit).empty());
    CHECK(PlateLayout::plan_drop(cells, 1, Cell(0, -Limit), Limit).empty());
    CHECK(PlateLayout::plan_drop(cells, 2, Cell(0, 1), Limit).empty());
}

TEST_CASE("A push that would run past the limit is refused", "[PlateLayout]")
{
    const std::vector<Cell> cells{{Limit - 1, 0}, {0, 1}};
    CHECK(PlateLayout::plan_drop(cells, 1, Cell(Limit - 1, 0), Limit).empty());
}

TEST_CASE("Closing gaps slides each row left and removes empty rows", "[PlateLayout]")
{
    const std::vector<Cell> cells{{3, 0}, {-1, 0}, {2, 2}, {5, 2}, {1, -1}};
    const auto              closed = PlateLayout::close_gaps(cells);
    CHECK(closed[4] == Cell(0, 0));
    CHECK(closed[1] == Cell(0, 1));
    CHECK(closed[0] == Cell(1, 1));
    CHECK(closed[2] == Cell(0, 2));
    CHECK(closed[3] == Cell(1, 2));
}

TEST_CASE("Reading order goes by row, then by column", "[PlateLayout]")
{
    const std::vector<Cell> cells{{1, 1}, {2, 0}, {0, 1}, {-1, 0}};
    CHECK(PlateLayout::reading_order(cells) == std::vector<int>{3, 1, 2, 0});
}

TEST_CASE("The next free cell fills a hole before extending the top row", "[PlateLayout]")
{
    CHECK(PlateLayout::next_free_cell({}) == Cell(0, 0));
    CHECK(PlateLayout::next_free_cell({{0, 0}, {1, 0}, {1, 1}}) == Cell(0, 1));
    CHECK(PlateLayout::next_free_cell({{-1, 0}, {0, 0}}) == Cell(1, 0));
}

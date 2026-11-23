#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/PreviewMarks.hpp"

#include <vector>

using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

TEST_CASE("Marks closer than the minimum distance chain into one group", "[PreviewMarks]")
{
    const std::vector<TickGroup> groups = group_ticks_by_position({100.f, 105.f, 110.f, 200.f}, 6.f);

    REQUIRE(groups.size() == 2);
    CHECK(groups[0].first == 0);
    CHECK(groups[0].count == 3);
    CHECK(groups[1].first == 3);
    CHECK(groups[1].count == 1);
    CHECK_THAT(groups[1].pos, WithinAbs(200.f, 0.01f));
}

TEST_CASE("Marks far enough apart stay separate", "[PreviewMarks]")
{
    const std::vector<TickGroup> groups = group_ticks_by_position({100.f, 120.f, 140.f}, 6.f);

    CHECK(groups.size() == 3);
    CHECK(group_ticks_by_position({}, 6.f).empty());
}

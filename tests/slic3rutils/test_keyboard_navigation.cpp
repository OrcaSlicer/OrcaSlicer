#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <wx/defs.h>

#include "slic3r/GUI/wxExtensions.hpp"

TEST_CASE("Left and Right step through a control row and wrap at its ends", "[KeyboardNavigation]")
{
    const int count = 4;
    CHECK(row_focus_target(1, count, WXK_RIGHT) == 2);
    CHECK(row_focus_target(1, count, WXK_LEFT) == 0);
    CHECK(row_focus_target(count - 1, count, WXK_RIGHT) == 0);
    CHECK(row_focus_target(0, count, WXK_LEFT) == count - 1);
}

TEST_CASE("Home and End jump to the first and last control of a row", "[KeyboardNavigation]")
{
    const int count   = 5;
    const int current = GENERATE(0, 2, 4);
    CHECK(row_focus_target(current, count, WXK_HOME) == 0);
    CHECK(row_focus_target(current, count, WXK_END) == count - 1);
}

TEST_CASE("Keys that do not move within a control row return no target", "[KeyboardNavigation]")
{
    const int key = GENERATE(as<int>{}, WXK_UP, WXK_DOWN, WXK_TAB, WXK_RETURN, 'A');
    CHECK(row_focus_target(1, 3, key) == -1);
}

TEST_CASE("An empty control row returns no target", "[KeyboardNavigation]")
{
    CHECK(row_focus_target(0, 0, WXK_RIGHT) == -1);
    CHECK(row_focus_target(0, 0, WXK_HOME) == -1);
}

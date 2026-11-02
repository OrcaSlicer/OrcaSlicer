// The preview UI is linked by slic3rutils_tests.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <algorithm>
#include <limits>
#include <cstddef>
#include <vector>

#include "slic3r/GUI/GCodeTextView.hpp"
#include "slic3r/GUI/GCodeNavigation.hpp"
#include "slic3r/GUI/IMSlider.hpp"

using Slic3r::GUI::GCodeTextView;

TEST_CASE("G-code text bounds include the first and last source lines", "[GCodeTextView]")
{
    const uint64_t count = GENERATE(uint64_t(1), uint64_t(3), uint64_t(100));
    GCodeTextView view;
    view.update(count, 20, 1);
    CHECK(view.first_line() == 1);
    CHECK(view.last_line() == std::min(count, uint64_t(20)));
    view.scroll(std::numeric_limits<int>::max());
    CHECK(view.last_line() == count);
    view.scroll(std::numeric_limits<int>::min());
    CHECK(view.first_line() == 1);
}

TEST_CASE("Empty and zero-height G-code viewports have no readable row", "[GCodeTextView]")
{
    GCodeTextView view;
    view.update(0, 20, 0);
    CHECK(view.last_line() == 0);
    view.scroll(3);
    CHECK(view.first_line() == 1);
    view.update(100, 0, 100);
    CHECK(view.last_line() == 0);
    CHECK(view.first_line() == 1);
}

TEST_CASE("G-code browsing does not follow path updates until explicitly resumed", "[GCodeTextView]")
{
    GCodeTextView view;
    view.update(100, 10, 50);
    CHECK(view.first_line() == 45);
    view.scroll(3);
    CHECK_FALSE(view.following());
    view.update(100, 10, 80);
    CHECK(view.first_line() == 48);
    view.resume_following();
    view.update(100, 10, 80);
    CHECK(view.first_line() == 75);
}

TEST_CASE("G-code browsing survives resize and resets for another result", "[GCodeTextView]")
{
    GCodeTextView view;
    view.update(100, 10, 1);
    view.scroll_to(85);
    view.update(100, 12, 1);
    CHECK(view.first_line() == 85);
    view.update(100, 30, 1);
    CHECK(view.first_line() == 71);
    CHECK(view.last_line() == 100);
    view.reset();
    view.update(3, 30, 3);
    CHECK(view.following());
    CHECK(view.first_line() == 1);
    CHECK(view.last_line() == 3);
}

TEST_CASE("Programmatic preview slider updates do not count as user input", "[GCodeTextView]")
{
    Slic3r::GUI::IMSlider slider(0, 9, 0, 9, wxSL_HORIZONTAL);
    slider.SetHigherValue(5);
    CHECK(slider.is_dirty());
    CHECK_FALSE(slider.consume_user_interaction());
    slider.SetSelectionSpan(0, 4);
    CHECK_FALSE(slider.consume_user_interaction());
    slider.switch_one_layer_mode();
    CHECK(slider.consume_user_interaction());
    CHECK_FALSE(slider.consume_user_interaction());
}

TEST_CASE("G-code navigation resolves source gaps and keeps complete command groups", "[GCodeTextView][GCodeNavigation]")
{
    const std::vector<uint32_t> ids{10, 10, 20, 20, 20, 30};
    const auto [requested, expected_line, first, last] = GENERATE(
        table<uint32_t, uint32_t, size_t, size_t>({
            {1, 10, 0, 1}, {10, 10, 0, 1}, {19, 10, 0, 1},
            {20, 20, 2, 4}, {21, 20, 2, 4}, {1000, 30, 5, 5}}));
    const auto target = Slic3r::GUI::find_gcode_navigation_target(requested, ids.size(),
        [&](size_t i) { return ids[i]; }, true);
    REQUIRE(target.has_value());
    CHECK(target->line_id == expected_line);
    CHECK(target->first_vertex == first);
    CHECK(target->last_vertex == last);
}

TEST_CASE("G-code navigation rejects missing source identities", "[GCodeTextView][GCodeNavigation]")
{
    const auto zero = [](size_t) { return uint32_t(0); };
    CHECK_FALSE(Slic3r::GUI::find_gcode_navigation_target(1, 0, zero, true));
    CHECK_FALSE(Slic3r::GUI::find_gcode_navigation_target(0, 3, zero, true));
    CHECK_FALSE(Slic3r::GUI::find_gcode_navigation_target(1, 3, zero, true));
    const std::vector<uint32_t> ids{0, 0, 10, 10};
    const auto target = Slic3r::GUI::find_gcode_navigation_target(1, ids.size(),
        [&](size_t i) { return ids[i]; }, true);
    REQUIRE(target.has_value());
    CHECK(target->first_vertex == 2);
    CHECK(target->last_vertex == 3);
}

TEST_CASE("G-code navigation falls back safely for nonmonotonic source identities", "[GCodeTextView][GCodeNavigation]")
{
    const std::vector<uint32_t> ids{10, 10, 30, 20, 20, 30};
    const auto target = Slic3r::GUI::find_gcode_navigation_target(25, ids.size(),
        [&](size_t i) { return ids[i]; }, false);
    REQUIRE(target.has_value());
    CHECK(target->line_id == 20);
    CHECK(target->first_vertex == 3);
    CHECK(target->last_vertex == 4);
    const auto leading = Slic3r::GUI::find_gcode_navigation_target(1, ids.size(),
        [&](size_t i) { return ids[i]; }, false);
    REQUIRE(leading.has_value());
    CHECK(leading->line_id == 10);
}

TEST_CASE("Ordered G-code navigation does not scan the whole file", "[GCodeTextView][GCodeNavigation]")
{
    size_t reads = 0;
    const auto target = Slic3r::GUI::find_gcode_navigation_target(12345, 1000000000,
        [&](size_t i) { ++reads; return uint32_t(i / 2 + 1); }, true);
    REQUIRE(target.has_value());
    CHECK(target->line_id == 12345);
    CHECK(target->last_vertex == 24689);
    CHECK(reads < 80);
}

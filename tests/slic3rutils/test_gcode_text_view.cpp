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

#include "slic3r/GUI/GCodeTextView.hpp"
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

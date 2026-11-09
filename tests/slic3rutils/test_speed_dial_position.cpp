#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/SpeedDialDialog.hpp"

#include <wx/gdicmn.h>

using namespace Slic3r::GUI;

namespace {

// Matches kPopupWidth / kPopupMaxHeight in SpeedDialDialog.cpp: the dial derives its origin from this
// box rather than from its current client size, so the content-driven resize never moves the window.
const wxSize kDialBox(540, 282);

} // namespace

TEST_CASE("Speed dial origin centres the dial box on the parent window", "[SpeedDialPosition][SpeedDial]")
{
    const wxRect parent(120, 80, 1600, 1200);
    const wxRect work_area(0, 0, 1920, 1200);

    const wxPoint origin = speed_dial_origin(parent, work_area, kDialBox);

    REQUIRE(origin.x == 650); // 120 + (1600 - 540) / 2
    REQUIRE(origin.y == 539); //  80 + (1200 - 282) / 2
}

TEST_CASE("Speed dial origin follows the parent when the parent is moved", "[SpeedDialPosition][SpeedDial]")
{
    const wxRect work_area(0, 0, 1920, 1200);
    const wxRect first(120, 80, 1600, 1200);
    const wxRect moved(400, 300, 1600, 1200);

    const wxPoint before = speed_dial_origin(first, work_area, kDialBox);
    const wxPoint after  = speed_dial_origin(moved, work_area, kDialBox);

    REQUIRE(after.x - before.x == 280);
    REQUIRE(after.y - before.y == 220);
}

TEST_CASE("Speed dial origin follows the parent when the parent is resized", "[SpeedDialPosition][SpeedDial]")
{
    const wxRect work_area(0, 0, 1920, 1200);
    const wxRect wide(200, 100, 1400, 1000);
    const wxRect narrow(200, 100, 1000, 700);

    const wxPoint before = speed_dial_origin(wide, work_area, kDialBox);
    const wxPoint after  = speed_dial_origin(narrow, work_area, kDialBox);

    REQUIRE(after.x - before.x == -200); // (1000 - 1400) / 2
    REQUIRE(after.y - before.y == -150); // ( 700 - 1000) / 2
}

TEST_CASE("Speed dial origin stays negative on a monitor left of the primary", "[SpeedDialPosition][SpeedDial]")
{
    const wxRect work_area(-1920, 0, 1920, 1080);
    const wxRect parent(-1800, 100, 1600, 1000);

    const wxPoint origin = speed_dial_origin(parent, work_area, kDialBox);

    REQUIRE(origin.x == -1270); // -1800 + (1600 - 540) / 2
    REQUIRE(origin.y == 459);   //   100 + (1000 - 282) / 2
}

TEST_CASE("Speed dial origin is clamped into the work area when the parent is mostly off screen", "[SpeedDialPosition][SpeedDial]")
{
    const wxRect work_area(0, 0, 1920, 1200);
    const wxRect parent(1800, -800, 1600, 1200);

    const wxPoint origin = speed_dial_origin(parent, work_area, kDialBox);

    REQUIRE(origin.x == 1380); // centred would be 2330, clamped to 1920 - 540
    REQUIRE(origin.y == 0);    // centred would be -341, clamped to the work-area top
}

TEST_CASE("Speed dial origin is pinned to a work area smaller than the dial box", "[SpeedDialPosition][SpeedDial]")
{
    const wxRect work_area(0, 0, 300, 100);
    const wxRect parent(0, 0, 300, 100);

    const wxPoint origin = speed_dial_origin(parent, work_area, kDialBox);

    REQUIRE(origin.x == work_area.x);
    REQUIRE(origin.y == work_area.y);
}

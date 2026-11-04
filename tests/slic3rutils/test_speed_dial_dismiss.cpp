#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/SpeedDialDialog.hpp"

using namespace Slic3r::GUI;

// The dial hides only after a settle delay, and only if it is still shown and really lost the focus.
// Hiding on the raw deactivation event makes the dial dismiss itself right after the first spacebar
// open, because wxGTK can deliver wxEVT_ACTIVATE(false) to a window that still holds the focus.

TEST_CASE("Speed dial stays shown while it still holds the focus", "[SpeedDialDismiss][SpeedDial]")
{
    // The GTK stray-deactivation case: a deactivation arrives, but the dial is still active.
    REQUIRE_FALSE(speed_dial_should_hide(true, true));
}

TEST_CASE("Speed dial hides once the focus has really left it", "[SpeedDialDismiss][SpeedDial]")
{
    // The user clicked into the application behind the dial, or switched to another application.
    REQUIRE(speed_dial_should_hide(true, false));
}

TEST_CASE("Hidden speed dial never hides again", "[SpeedDialDismiss][SpeedDial]")
{
    // Already hidden: a second hide would restart the delay and re-trigger any dismissal side effects.
    REQUIRE_FALSE(speed_dial_should_hide(false, true));
    REQUIRE_FALSE(speed_dial_should_hide(false, false));
}

TEST_CASE("Speed dial dismiss decision follows the shown and active truth table", "[SpeedDialDismiss][SpeedDial]")
{
    // The rule in full, so a refactor cannot go back to hiding on the raw deactivation event.
    // shown && !active is the only combination that hides.
    REQUIRE_FALSE(speed_dial_should_hide(true, true));
    REQUIRE(speed_dial_should_hide(true, false));
    REQUIRE_FALSE(speed_dial_should_hide(false, true));
    REQUIRE_FALSE(speed_dial_should_hide(false, false));
}

// Order of the main window tabs the user dragged into place (Notebook::MergePageOrder and
// Notebook::OrderedPosition), without a window.
#include <catch2/catch_test_macros.hpp>

#include <slic3r/GUI/Notebook.hpp>

using Order = std::vector<wxString>;

TEST_CASE("Tab order keeps pages missing from the bar in place", "[TabOrder]")
{
    // "plugin.c" sits behind the plugin overflow button, "x" and "calibration" are printer-specific
    // tabs: none of them is in the bar.
    const Order old_order{"home", "x", "prepare", "calibration", "plugin.a", "plugin.b", "plugin.c"};
    CHECK(Notebook::MergePageOrder(old_order, {"home", "prepare", "plugin.b", "plugin.a"}) ==
          Order{"home", "x", "prepare", "calibration", "plugin.b", "plugin.a", "plugin.c"});
    CHECK(Notebook::MergePageOrder(old_order, {"prepare", "home", "plugin.a", "plugin.b"}) ==
          Order{"x", "prepare", "home", "calibration", "plugin.a", "plugin.b", "plugin.c"});
    CHECK(Notebook::MergePageOrder({}, {"home", "", "prepare"}) == Order{"home", "prepare"});
    CHECK(Notebook::MergePageOrder({"gone", "home"}, {"home"}) == Order{"gone", "home"});
}

TEST_CASE("Tab order places a page among the pages already in the bar", "[TabOrder]")
{
    const Order order{"home", "prepare", "preview", "device", "project", "design"};

    // Design asks to go right after Home but was dragged to the end.
    CHECK(Notebook::OrderedPosition(order, {"home", "prepare", "preview", "device", "project"}, "design", 1) == 5);
    // Home is added first, Prepare asks for the end, but Home was dragged after Prepare.
    CHECK(Notebook::OrderedPosition({"prepare", "home"}, {"home"}, "prepare", 1) == 0);
    // A page without a place goes where it asked to.
    CHECK(Notebook::OrderedPosition(order, {"home", "prepare"}, "calibration", 1) == 1);
    // Pages without a place in the bar do not pin the position.
    CHECK(Notebook::OrderedPosition(order, {"home", "plugin.p.x", "project"}, "device", 2) == 2);
    CHECK(Notebook::OrderedPosition(order, {"home", "plugin.p.x", "project"}, "device", 0) == 1);
}

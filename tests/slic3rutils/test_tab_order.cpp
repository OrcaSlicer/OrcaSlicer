// Order of the main window tabs the user dragged into place (Notebook::MergePageOrder and
// Notebook::OrderedPosition), without a window. Built-in and plugin tabs share one order.
#include <catch2/catch_test_macros.hpp>

#include <slic3r/GUI/Notebook.hpp>

using Order = std::vector<wxString>;

TEST_CASE("Tab order keeps pages missing from the bar in place", "[TabOrder]")
{
    // "plugin.c" sits behind the plugin overflow button, "x" and "calibration" are printer-specific
    // tabs: none of them is in the bar.
    const Order old_order{"home", "x", "prepare", "plugin.a", "calibration", "plugin.b", "plugin.c"};

    // A plugin tab dragged between built-in tabs, a built-in tab dragged after a plugin tab.
    CHECK(Notebook::MergePageOrder(old_order, {"home", "plugin.b", "prepare", "plugin.a"}) ==
          Order{"home", "calibration", "plugin.b", "x", "prepare", "plugin.a", "plugin.c"});
    CHECK(Notebook::MergePageOrder(old_order, {"plugin.a", "plugin.b", "home", "prepare"}) ==
          Order{"plugin.a", "calibration", "plugin.b", "home", "x", "prepare", "plugin.c"});

    CHECK(Notebook::MergePageOrder({}, {"home", "", "prepare"}) == Order{"home", "prepare"});
    // Nothing followed it, so it stays last.
    CHECK(Notebook::MergePageOrder({"home", "gone"}, {"home"}) == Order{"home", "gone"});
}

TEST_CASE("Tab order places a page among the pages already in the bar", "[TabOrder]")
{
    const Order order{"home", "plugin.a", "prepare", "preview", "device", "project", "design", "plugin.b"};

    // Design asks to go right after Home but was dragged after Project.
    CHECK(Notebook::OrderedPosition(order, {"home", "plugin.a", "prepare", "preview", "device", "project", "plugin.b"}, "design", 1) == 6);
    // A plugin page added at the end of the bar goes between built-in tabs.
    CHECK(Notebook::OrderedPosition(order, {"home", "prepare", "preview"}, "plugin.a", 3) == 1);
    // Home is added first, Prepare asks for the end, but Home was dragged after Prepare.
    CHECK(Notebook::OrderedPosition({"prepare", "home"}, {"home"}, "prepare", 1) == 0);
    // A page without a place (a newly installed plugin) goes where it asked to: the end.
    CHECK(Notebook::OrderedPosition(order, {"home", "prepare"}, "plugin.new", 2) == 2);
    // Pages without a place in the order do not pin the position.
    CHECK(Notebook::OrderedPosition(order, {"home", "plugin.new", "project"}, "device", 2) == 2);
    CHECK(Notebook::OrderedPosition(order, {"home", "plugin.new", "project"}, "device", 0) == 1);
}

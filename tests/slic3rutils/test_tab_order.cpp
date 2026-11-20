// Order of the main window tabs the user dragged into place (Notebook::MergePageOrder and
// Notebook::OrderedPosition), without a window. Built-in and plugin tabs share one order.
#include <catch2/catch_test_macros.hpp>

#include <slic3r/GUI/Notebook.hpp>
#include <slic3r/plugin/host/PluginPages.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

#include <optional>
#include <vector>

#include <wx/string.h>

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

TEST_CASE("Plugin tabs that fit follow the bar width", "[TabOrder]")
{
    // macOS at 100 %, worked out from the widgets' sizing code:
    // - a tab is 136 px (Button's minimum, wider than the labels here), icon-only Home and the
    //   overflow button 40 px, so the six built-in tabs take 40 + 5 * 136 = 720 px;
    // - Slice/Print with their dropdowns take 422 px, only on Prepare and Preview; the two spacers
    //   beside them 34 px and the margin in front 15 px on every tab.
    const int built_in = 720, spacers = 34 + 15, slice_print = 422;
    auto fit = [&](int window, int plugins, bool slice_print_shown) {
        const int room = window - built_in - spacers - (slice_print_shown ? slice_print : 0);
        return Slic3r::PluginPages::fitting_tabs(room, std::vector<int>(plugins, 136), 40);
    };

    // Before this change it was 5 at every width.
    CHECK(fit(1920, 6, false) == 6); // Home, Device, plugin tabs: all six, no overflow button
    CHECK(fit(1920, 9, false) == 8); // 8 and the overflow button
    CHECK(fit(1920, 6, true) == 5);  // Prepare and Preview, next to Slice/Print
    CHECK(fit(1400, 7, true) == 1);
    CHECK(fit(1400, 7, false) == 4);
    CHECK(fit(2600, 7, true) == 7);
    CHECK(fit(2723, 15, true) == 10);
    CHECK(fit(1100, 7, true) == 0);

    // Longer labels take more room.
    CHECK(Slic3r::PluginPages::fitting_tabs(471, {136, 200, 136}, 40) == 2);
    CHECK(Slic3r::PluginPages::fitting_tabs(472, {136, 200, 136}, 40) == 3);
}

TEST_CASE("Plugin tabs shown before the overflow button follow the saved order", "[TabOrder]")
{
    using Slic3r::PluginCapabilityId;
    using Slic3r::PluginCapabilityType;
    using Ids = std::vector<PluginCapabilityId>;
    auto id = [](const char* name) { return PluginCapabilityId{PluginCapabilityType::Pages, name, name}; };
    const auto golf = id("Golf"), charlie = id("Charlie"), delta = id("Delta"), foxtrot = id("Foxtrot"),
               bravo = id("Bravo"), alpha = id("Alpha"), echo = id("Echo");
    const Ids order{golf, charlie, delta, foxtrot, bravo, alpha, echo};

    // Nothing picked from the overflow menu: the first page in the overflow gets the last slot.
    CHECK(Slic3r::PluginPages::plugin_tab_ids(order, 5, std::nullopt) == Ids{golf, charlie, delta, foxtrot, bravo});
    // A page picked from the overflow menu keeps the last slot.
    CHECK(Slic3r::PluginPages::plugin_tab_ids(order, 5, echo) == Ids{golf, charlie, delta, foxtrot, echo});
    // A pick that is no longer in the overflow does not take the slot.
    CHECK(Slic3r::PluginPages::plugin_tab_ids(order, 5, charlie) == Ids{golf, charlie, delta, foxtrot, bravo});
    CHECK(Slic3r::PluginPages::plugin_tab_ids(order, 7, alpha) == order);
    CHECK(Slic3r::PluginPages::plugin_tab_ids(order, 1, std::nullopt) == Ids{golf});

    // Echo picked from the overflow menu, then a tab without Slice/Print where all pages fit: Echo
    // shows in its own slot and stays picked, so back on Prepare it takes the last slot again.
    const std::optional<PluginCapabilityId> picked = echo;
    CHECK(Slic3r::PluginPages::plugin_tab_ids(order, 7, picked) == order);
    CHECK(Slic3r::PluginPages::plugin_tab_ids(order, 5, picked) == Ids{golf, charlie, delta, foxtrot, echo});
}

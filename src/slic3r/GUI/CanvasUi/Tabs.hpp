#pragma once

#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// Folder tabs standing on the bottom of a row tabs_list_height high, from the cursor: the selected one opens
// onto the content under the row, covering the row's bottom divider, which the caller draws with the row's
// background.
bool tabs_list(const char* id, int& selected, const std::vector<std::string>& labels);

struct TabsBarOptions
{
    float       width{0.f};
    // Before the reset button when not empty, such as how many settings differ from their defaults.
    std::string changed_count;
    std::string reset_label;
    bool        can_reset{false};
};

struct TabsBarResult
{
    bool changed{false};
    bool reset{false};
};

// A row of folder tabs on the tabs list's background, from the cursor and options.width wide, with its divider,
// and at its end a counter and a reset button on the line of the tab labels.
TabsBarResult tabs_bar(const char* id, int& selected, const std::vector<std::string>& labels, const TabsBarOptions& options);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

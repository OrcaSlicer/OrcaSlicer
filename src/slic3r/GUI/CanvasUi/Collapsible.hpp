#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Button.hpp"
#include "Layout.hpp"
#include "ScrollArea.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Metrics;

// Settings that open under a panel's header inside its card: the panel keeps whether they
// are open, the trigger opens and closes them, and the panel part shows them over the content under them.

// Shown pressed in while the panel is open.
bool collapsible_trigger(const char* id, const std::string& label, bool& open, ButtonOptions options);
// Esc closes the panel, unless a field holds the keyboard or a popup took the key. Before the card is laid out, so
// that the frame of the key lays it out closed.
void collapsible_close_on_escape(bool& open);

// The height of the panel for content_height of content: its bar on top and the padding around the content.
float collapsible_panel_height(const Metrics& metrics, float content_height);

struct CollapsiblePanelOptions
{
    float                    width{0.f};
    // As tall as it is shown, at most collapsible_panel_height(); the rest scrolls.
    float                    height{0.f};
    float                    content_height{0.f};
    // The bar: tabs when there are any, and at its end a counter, such as of the settings changed, and a reset.
    std::vector<std::string> tabs;
    int*                     tab{nullptr};
    std::string              changed_count;
    std::string              reset_label;
    bool                     can_reset{false};
    // How far the content under the panel reaches down; the panel's shadow falls on it.
    float                    below{0.f};
};

// The open panel at pos, open from construction to destruction. Its content is stacked from the cursor it leaves,
// content_width() wide. It is drawn after the content under it, so that its shadow lies on that content.
class CollapsiblePanel
{
public:
    CollapsiblePanel(const char* id, const Point& pos, const CollapsiblePanelOptions& options);
    ~CollapsiblePanel();
    CollapsiblePanel(const CollapsiblePanel&)            = delete;
    CollapsiblePanel& operator=(const CollapsiblePanel&) = delete;

    float content_width() const { return m_content_width; }
    bool  reset() const { return m_reset; }

private:
    Rect                      m_rect;
    float                     m_below{0.f};
    float                     m_content_width{0.f};
    bool                      m_reset{false};
    std::optional<ScrollArea> m_scroll;
    std::optional<Group>      m_group;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "Card.hpp"
#include "Paint.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Metrics;

struct PanelCardOptions
{
    // The top corner on the side of the column the panel stands in.
    Point at;
    bool  right{false};
    // From at down to the bottom of the room the column leaves the panel.
    float room{0.f};
    // Between the header and the body, as settings opened under the header.
    float extra{0.f};
    bool  collapsed{false};
    // Zero takes the metrics' panel width.
    float width{0.f};

    float card_width(const Metrics& metrics) const;
};

// Whether a panel is collapsed, saved on each change. A press of its collapse button is taken at the start of the next frame,
// before anything is laid out for the old state.
class PanelCollapse
{
public:
    std::function<void(bool)> save;

    bool collapsed() const { return m_collapsed; }
    void load(bool collapsed) { m_collapsed = collapsed; }
    void set(bool collapsed);
    void toggle_next_frame() { m_pending = true; }
    // Whether a press in the last frame changed the state.
    bool take_pending();

private:
    bool m_collapsed{false};
    bool m_pending{false};
};

// A panel of a column over the scene: a card, a header with a collapse button at its left, and a body that
// scrolls within the room left. The body is measured as it is drawn, into body_content, which the panel keeps from frame to
// frame, so that the next frame fits the card to it.
class PanelCard
{
public:
    PanelCard(const char* id, const PanelCardOptions& options, float& body_content);

    const Card& card() const { return *m_card; }
    Point       origin() const { return m_card->rect().min(); }
    Rect        rect() const { return m_card->rect(); }
    float       width() const { return m_width; }
    float       header() const;

    bool  collapse_button(const char* id, bool collapsed, const std::string& tooltip) const;
    float title_left() const;

    // Under the header and the extra: a divider, and the body inset from the card's sides; draw lays the content out from the
    // top it is given and returns its height.
    void body(const char* id, float inset, const std::function<float(const Painter&, float left, float top, float width)>& draw) const;

private:
    PanelCardOptions    m_options;
    float&              m_content;
    float               m_width{0.f};
    float               m_body{0.f};
    std::optional<Card> m_card;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

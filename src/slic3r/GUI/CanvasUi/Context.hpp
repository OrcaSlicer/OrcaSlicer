#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>

#include "Font.hpp"
#include "Tokens.hpp"
#include "Metrics.hpp"
#include "Types.hpp"

struct ImFont;

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Theme;
class IconCache;

struct Pointer;
struct ZoneState;

// The pointer shape a component asks for over itself; the canvas shows it once the frame is drawn.
enum class Cursor : unsigned char { Default, ResizeHorizontal };

// What the toolkit needs for one ImGui frame of a canvas: the theme and sizes, the time, the tooltip
// timer, and the frames the components ask for. The canvas renders only on events and when it is
// dirty, so a component that animates or waits for a tooltip asks for its next frame here, and the
// canvas turns the request into a frame after this one ends.

class Context
{
public:
    using Clock = std::chrono::steady_clock;

    const Theme&      theme() const { return *m_theme; }
    const Metrics&    metrics() const { return m_metrics; }
    Clock::time_point now() const { return m_now; }
    int               frame() const { return m_frame; }

    void request_frame() { m_frame_requested = true; }
    void request_cursor(Cursor cursor) { m_cursor = cursor; }

    void request_frame_at(Clock::time_point when);

    // Whether the tooltip of the item id, hovered in this frame, is due. Until it is, the frame it
    // becomes due in is asked for. Another id, or a frame where nothing asks, starts the wait again.
    bool tooltip_due(unsigned id);

    // The interface font as it is now, and its bold face, for selected segments and titles.
    Font regular_font() const;
    Font bold_font() const;
    Font caption_font() const;
    Font mono_font(MonoStyle style, MonoSize size) const;

    // The context of the frame being drawn. There always is one while the toolkit draws.
    static Context* current();

private:
    friend class CanvasFrame;
    friend struct IconAccess;
    friend Pointer   pointer();
    friend ZoneState zone(const char* id, const Rect& rect);

    // Whether a press of the left button at pos, in this frame, is the second of a double click: within 300 ms and a few
    // pixels of the last one, by the frame's own clock. ImGui's own test runs on its clamped frame time, so presses
    // seconds apart in an idle canvas look close to it. Asked any number of times in a frame, it answers the same.
    bool        double_click(bool pressed, const Point& pos);
    static Font own_size(ImFont* face);

    const Theme*                     m_theme{nullptr};
    Metrics                          m_metrics;
    Clock::time_point                m_now;
    int                              m_frame{0};
    bool                             m_frame_requested{false};
    Cursor                           m_cursor{Cursor::Default};
    std::optional<Clock::time_point> m_wake_at;
    unsigned                         m_tooltip_id{0};
    Clock::time_point                m_tooltip_since;
    bool                             m_tooltip_asked{false};
    std::optional<Clock::time_point> m_click_asked_at;
    bool                             m_double_click{false};
    std::optional<Clock::time_point> m_last_click;
    Point                            m_last_click_pos;
    IconCache*                       m_icons{nullptr};
    Fonts                            m_fonts;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

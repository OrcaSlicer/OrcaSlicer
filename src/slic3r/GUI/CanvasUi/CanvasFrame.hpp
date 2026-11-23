#pragma once

#include <optional>

#include "Context.hpp"
#include "Font.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

class IconCache;

// The canvas side of a toolkit frame: it starts the frame with the theme, the sizes, the time, the icons and the
// fonts, ends it, and reads what the components asked for. Panels may not include it (CheckCanvasPanels), so
// they cannot restart the frame they draw in.
class CanvasFrame
{
public:
    explicit CanvasFrame(Context& context) : m_context(context) {}

    // frame counts the frames of every canvas, which share one ImGui context.
    void begin(bool dark, float scale, Context::Clock::time_point now, int frame, IconCache* icons, const Fonts& fonts);
    void end();

    bool                                       frame_requested() const { return m_context.m_frame_requested; }
    Cursor                                     cursor() const { return m_context.m_cursor; }
    std::optional<Context::Clock::time_point> wake_at() const { return m_context.m_wake_at; }

private:
    Context& m_context;
};

// Makes a context current for a scope and gives the previous one back at its end, so that a canvas
// painted from inside the frame of another leaves the outer frame its context.
class ScopedContext
{
public:
    explicit ScopedContext(Context& context);
    ~ScopedContext();
    ScopedContext(const ScopedContext&)            = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    Context* m_previous;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

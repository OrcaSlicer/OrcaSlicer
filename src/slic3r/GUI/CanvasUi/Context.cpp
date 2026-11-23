#include "Context.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>

#include <imgui/imgui.h>

#include "CanvasFrame.hpp"
#include "Font.hpp"

#include "Metrics.hpp"
#include "Theme.hpp"
#include "Tokens.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

static Context* s_current = nullptr;

bool Context::double_click(bool pressed, const Point& pos)
{
    if (m_click_asked_at == m_now)
        return m_double_click;
    m_click_asked_at = m_now;
    m_double_click   = false;
    if (!pressed)
        return false;
    const float slop = m_metrics.px(space<6>());
    m_double_click   = m_last_click && m_now - *m_last_click <= std::chrono::milliseconds(300) && std::abs(pos.x - m_last_click_pos.x) <= slop &&
                     std::abs(pos.y - m_last_click_pos.y) <= slop;
    m_last_click     = m_double_click ? std::nullopt : std::optional<Clock::time_point>(m_now);
    m_last_click_pos = pos;
    return m_double_click;
}

Context* Context::current()
{
    assert(s_current != nullptr);
    return s_current;
}

ScopedContext::ScopedContext(Context& context) : m_previous(s_current) { s_current = &context; }

ScopedContext::~ScopedContext() { s_current = m_previous; }

void Context::request_frame_at(Clock::time_point when)
{
    if (!m_wake_at || when < *m_wake_at)
        m_wake_at = when;
}

Font Context::regular_font() const { return Font(ImGui::GetFont(), ImGui::GetFontSize()); }

Font Context::own_size(ImFont* face) { return Font(face, face->FontSize * face->Scale); }

Font Context::bold_font() const { return m_fonts.bold != nullptr ? own_size(m_fonts.bold) : regular_font(); }

Font Context::caption_font() const { return regular_font().resized(metrics().px(TextRole::caption.size)); }

Font Context::mono_font(MonoStyle style, MonoSize size) const
{
    ImFont* const face = m_fonts.mono[size_t(style) * MONO_FONT_SIZES.size() + size_t(size)];
    return face != nullptr ? own_size(face) : regular_font();
}

bool Context::tooltip_due(unsigned id)
{
    m_tooltip_asked = true;
    if (id != m_tooltip_id) {
        m_tooltip_id    = id;
        m_tooltip_since = m_now;
    }
    const Clock::time_point due = m_tooltip_since + duration::tooltip;
    if (m_now >= due)
        return true;
    request_frame_at(due);
    return false;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

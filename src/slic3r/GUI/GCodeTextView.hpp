#pragma once

#include <algorithm>
#include <cstdint>

namespace Slic3r::GUI {

// Source lines are one-based. Browsing is independent of the preview's current line.
class GCodeTextView
{
public:
    void update(uint64_t line_count, uint64_t visible_count, uint64_t current_line)
    {
        m_line_count = line_count;
        m_visible_count = std::min(line_count, visible_count);
        if (m_following) {
            const uint64_t half = m_visible_count / 2;
            m_first_line = current_line > half ? current_line - half : 1;
        }
        m_first_line = std::clamp(m_first_line, uint64_t(1), max_first_line());
    }

    void scroll_to(uint64_t first_line)
    {
        m_following = false;
        m_first_line = std::clamp(first_line, uint64_t(1), max_first_line());
    }

    void scroll(int lines)
    {
        const uint64_t distance = lines < 0 ? -int64_t(lines) : lines;
        scroll_to(lines < 0 ? m_first_line - std::min(distance, m_first_line - 1) :
            m_first_line + std::min(distance, max_first_line() - m_first_line));
    }

    void resume_following() { m_following = true; }
    void reset() { *this = GCodeTextView(); }
    bool following() const { return m_following; }
    uint64_t first_line() const { return m_first_line; }
    uint64_t last_line() const { return m_visible_count == 0 ? 0 : m_first_line + m_visible_count - 1; }
    uint64_t visible_count() const { return m_visible_count; }

private:
    uint64_t max_first_line() const { return m_visible_count == 0 ? 1 : m_line_count - m_visible_count + 1; }

    uint64_t m_line_count{0};
    uint64_t m_visible_count{0};
    uint64_t m_first_line{1};
    bool m_following{true};
};

} // namespace Slic3r::GUI

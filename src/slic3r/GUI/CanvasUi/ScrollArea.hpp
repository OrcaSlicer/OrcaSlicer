#pragma once

#include "Paint.hpp"
#include "Types.hpp"

struct ImDrawList;

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// A region of the given size that scrolls content_height of content up and down with the wheel and a thin
// scrollbar. Open from construction to destruction; its content starts at its top left corner.
class ScrollArea
{
public:
    ScrollArea(const char* id, const Size& size, float content_height);
    ~ScrollArea();
    ScrollArea(const ScrollArea&)            = delete;
    ScrollArea& operator=(const ScrollArea&) = delete;

    // The top left corner of the content, moving as it scrolls.
    Point   origin() const { return m_origin; }
    Painter painter() const;

private:
    Point       m_origin;
    ImDrawList* m_list{nullptr};
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

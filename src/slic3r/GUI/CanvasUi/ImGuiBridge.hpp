#pragma once

#include <imgui/imgui.h>

#include "Corners.hpp"
#include "Font.hpp"
#include "Icons.hpp"
#include "Tokens.hpp"
#include "Types.hpp"

// The toolkit's own ImGui side, for the toolkit and its tests only: panels see the toolkit's types, never
// ImGui's.

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

inline ImVec2 im(const Point& p) { return ImVec2(p.x, p.y); }
inline ImVec2 im(const Size& s) { return ImVec2(s.width, s.height); }
inline ImVec4 im(const Color& c) { return ImVec4(c.r, c.g, c.b, c.a); }
inline Point  point(const ImVec2& v) { return Point(v.x, v.y); }
inline ImU32  u32(const Color& c) { return ImGui::GetColorU32(im(c)); }
inline ImFont* face_of(const Font& font) { return font.m_face; }

// AddRectFilled and AddRect of ImDrawList, with the toolkit's shape of corner.
void fill_rounded(ImDrawList& list, const ImVec2& min, const ImVec2& max, ImU32 color, float radius, Corners corners = Corners::All);
void stroke_rounded(ImDrawList& list, const ImVec2& min, const ImVec2& max, ImU32 color, float radius, float width,
                    Corners corners = Corners::All);

// The fill and the outline of the current window, past the clip ImGui insets a window by, half its padding,
// so that its corners are the toolkit's.
void fill_window(ImU32 color, float radius);
void stroke_window(ImU32 color, float radius, float width);

// A soft shadow around the current window, rounded as it is: drawn in the window's own draw list and only
// outside its rectangle, so that it lies over the windows behind and under the window itself.
void draw_window_shadow(float rounding, const Elevation& level);

// Snapped to the pixel grid.
void draw_icon(ImDrawList& list, Icon icon, const ImVec2& center, float size, ImU32 color);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

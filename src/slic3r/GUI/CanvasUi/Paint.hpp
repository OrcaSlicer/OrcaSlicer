#pragma once

#include <string_view>
#include <vector>

#include "Corners.hpp"
#include "Icons.hpp"
#include "Types.hpp"

struct ImDrawList;

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

class Font;

enum class GradientAxis : unsigned char { Horizontal, Vertical };

// Shapes and text over what is already drawn in a layer: a card's, or the scene's under every panel.
class Painter
{
public:
    static Painter background();
    // The layer the component being drawn lies in.
    static Painter current();

    void fill_rect(const Rect& rect, const Color& color, float radius = 0.f, Corners corners = Corners::All) const;
    void stroke_rect(const Rect& rect, const Color& color, float width = 1.f, float radius = 0.f, Corners corners = Corners::All) const;
    // From one colour at the left or top edge to the other at the right or bottom edge.
    void fill_gradient(const Rect& rect, const Color& from, const Color& to, GradientAxis axis) const;
    // Through the stops spaced evenly from the left or top edge to the right or bottom edge.
    void fill_gradient(const Rect& rect, const std::vector<Color>& stops, GradientAxis axis) const;
    void line(const Point& a, const Point& b, const Color& color, float width = 1.f) const;
    // A divider of one outline in the border colour, along the bottom of the band that ends at bottom.
    void divider(float left, float right, float bottom) const;
    void polyline(const std::vector<Point>& points, const Color& color, float width, bool closed) const;
    // Wound clockwise on the screen, so that the antialiased fringe falls outside; softness widens the fringe,
    // as for a shadow.
    void fill_convex(const std::vector<Point>& points, const Color& color, float softness = 0.f) const;
    void fill_circle(const Point& center, float radius, const Color& color) const;
    void stroke_circle(const Point& center, float radius, const Color& color, float width = 1.f) const;
    void text(const Font& font, const Point& pos, const Color& color, std::string_view text) const;
    // From the band's left, centred on its height.
    void text(const Font& font, const Rect& band, const Color& color, std::string_view text) const;
    void text_wrapped(const Font& font, const Point& pos, float width, const Color& color, std::string_view text) const;
    // Centred on center and laid in a plane, unclipped: right and down are the screen offsets of one pixel of
    // the text.
    void text_in_plane(const Font& font, std::string_view text, const Color& color, const Point& center, const Point& right,
                       const Point& down) const;
    void icon(Icon icon, const Point& center, float size, const Color& color) const;
    // The shadow that settings opened across [left, right] cast down from their bottom edge at y onto the content under them.
    void shadow_below(float left, float right, float y) const;

    // Paints only inside the rectangle, and inside what was clipped already, for the scope.
    class Clip
    {
    public:
        Clip(const Painter& painter, const Rect& rect);
        ~Clip();
        Clip(const Clip&)            = delete;
        Clip& operator=(const Clip&) = delete;

    private:
        ImDrawList& m_list;
    };

    // Fills without the antialiased fringe for the scope, so that shapes sharing an edge leave no seam.
    class SharpFills
    {
    public:
        explicit SharpFills(const Painter& painter);
        ~SharpFills();
        SharpFills(const SharpFills&)            = delete;
        SharpFills& operator=(const SharpFills&) = delete;

    private:
        ImDrawList& m_list;
        int         m_flags;
    };

private:
    friend class Card;
    friend class ScrollArea;
    explicit Painter(ImDrawList& list) : m_list(&list) {}

    ImDrawList* m_list;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

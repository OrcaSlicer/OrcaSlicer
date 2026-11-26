#pragma once

#include <array>
#include <chrono>

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// The scales every size of the toolkit and its panels comes from, in logical pixels. A size is made only
// through its scale, so a number off the scale does not compile; Metrics turns them into screen pixels.

class Space
{
public:
    constexpr float logical() const { return m_logical; }

private:
    constexpr explicit Space(float logical) : m_logical(logical) {}
    float m_logical;

    template<int N> friend constexpr Space space();
};

// In steps of 2 px, and the single pixel.
template<int N> constexpr Space space()
{
    static_assert(N == 1 || (N > 0 && N % 2 == 0), "spacing goes in steps of 2 px");
    return Space(float(N));
}

inline constexpr float RADIUS_BASE = 8.f;

// The steps of the radius, all from one base.
class Radius
{
public:
    static const Radius sm;
    static const Radius md;
    static const Radius lg;
    static const Radius xl;

    constexpr float logical() const { return m_logical; }

private:
    constexpr explicit Radius(float logical) : m_logical(logical) {}
    float m_logical;
};

constexpr Radius Radius::sm{RADIUS_BASE - 4.f};
constexpr Radius Radius::md{RADIUS_BASE - 2.f};
constexpr Radius Radius::lg{RADIUS_BASE};
constexpr Radius Radius::xl{RADIUS_BASE + 4.f};

class Stroke
{
public:
    static const Stroke sm;
    static const Stroke md;

    constexpr float logical() const { return m_logical; }

private:
    constexpr explicit Stroke(float logical) : m_logical(logical) {}
    float m_logical;
};

constexpr Stroke Stroke::sm{1.f};
constexpr Stroke Stroke::md{1.5f};

class IconSize
{
public:
    constexpr float logical() const { return m_logical; }

private:
    constexpr explicit IconSize(float logical) : m_logical(logical) {}
    float m_logical;

    template<int N> friend constexpr IconSize icon_size();
};

template<int N> constexpr IconSize icon_size()
{
    static_assert(N == 12 || N == 14 || N == 16 || N == 18, "icons are 12, 14, 16 or 18 px");
    return IconSize(float(N));
}

// The interface font of Orca, which the other sizes are drawn against.
inline constexpr int INTERFACE_FONT_SIZE = 15;

class FontSize
{
public:
    constexpr float logical() const { return m_logical; }

private:
    constexpr explicit FontSize(float logical) : m_logical(logical) {}
    float m_logical;

    template<int N> friend constexpr FontSize font_size();
};

template<int N> constexpr FontSize font_size()
{
    static_assert(N == 9 || N == 11 || N == 12 || N == 13 || N == 14 || N == INTERFACE_FONT_SIZE, "font sizes are 9, 11, 12, 13, 14 or 15 px");
    return FontSize(float(N));
}

// The roles text is set in, each a size with the height of its line: component heights are a line of their role
// and padding on the spacing scale. Body and title are the interface font of Orca; code is the monospace font at
// its largest size, the height of a line of code whatever size the user picks.
struct TextRole
{
    FontSize size;
    Space    leading;

    static const TextRole caption;
    // Section titles, column headings and tooltips: a step under the body.
    static const TextRole label;
    static const TextRole body;
    static const TextRole title;
    static const TextRole code;
};

inline constexpr TextRole TextRole::caption{font_size<11>(), space<14>()};
inline constexpr TextRole TextRole::label{font_size<13>(), space<18>()};
inline constexpr TextRole TextRole::body{font_size<INTERFACE_FONT_SIZE>(), space<20>()};
inline constexpr TextRole TextRole::title{font_size<INTERFACE_FONT_SIZE>(), space<20>()};
inline constexpr TextRole TextRole::code{font_size<14>(), space<18>()};

// How high a surface stands over what is under it: how far its shadow spreads and how dark it is, against the shadow role
// of the theme. A card stands on the scene, a popup over the card it opens from. A flat surface has no shadow.
struct Elevation
{
    Space spread;
    float opacity;

    static const Elevation card;
    static const Elevation overlay;
};

inline constexpr Elevation Elevation::card{space<4>(), 1.f};
inline constexpr Elevation Elevation::overlay{space<6>(), 1.25f};

// How far a rounded corner stretches into a curve of continuous curvature, from 0, a circular arc, to 1.
inline constexpr float CORNER_SMOOTHING = .6f;

inline constexpr std::array<int, 3> MONO_FONT_SIZES = {11, 12, 14};

namespace duration {
inline constexpr std::chrono::milliseconds tooltip{500};
inline constexpr std::chrono::milliseconds turn{250};
} // namespace duration

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

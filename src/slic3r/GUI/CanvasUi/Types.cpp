#include "Types.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

Color rgb(int hex, float alpha)
{
    return Color(float((hex >> 16) & 0xFF) / 255.f, float((hex >> 8) & 0xFF) / 255.f, float(hex & 0xFF) / 255.f, alpha);
}

static float byte(float channel) { return std::round(channel * 255.f); }

Color mix(const Color& a, const Color& b, float t)
{
    const auto channel = [t](float x, float y) {
        x = byte(x);
        y = byte(y);
        return std::floor(x + (y - x) * t + .5f) / 255.f;
    };
    return Color(channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), a.a);
}

Color faded(const Color& c, float factor) { return Color(c.r, c.g, c.b, std::floor(byte(c.a) * std::clamp(factor, 0.f, 1.f) + .5f) / 255.f); }

Color blend_over(const Color& top, const Color& bottom)
{
    const float a = top.a;
    return Color(top.r * a + bottom.r * (1.f - a), top.g * a + bottom.g * (1.f - a), top.b * a + bottom.b * (1.f - a), 1.f);
}

static float luminance(const Color& c)
{
    const auto channel = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * channel(c.r) + 0.7152f * channel(c.g) + 0.0722f * channel(c.b);
}

float contrast_ratio(const Color& a, const Color& b)
{
    const float la = luminance(a);
    const float lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

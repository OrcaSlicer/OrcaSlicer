#pragma once

#include <cstdint>

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Point
{
    float x{0.f};
    float y{0.f};

    constexpr Point() = default;
    constexpr Point(float x, float y) : x(x), y(y) {}
};

constexpr Point operator+(const Point& a, const Point& b) { return Point(a.x + b.x, a.y + b.y); }
constexpr Point operator-(const Point& a, const Point& b) { return Point(a.x - b.x, a.y - b.y); }
constexpr Point operator*(const Point& p, float k) { return Point(p.x * k, p.y * k); }

struct Size
{
    float width{0.f};
    float height{0.f};

    constexpr Size() = default;
    constexpr Size(float width, float height) : width(width), height(height) {}
};

struct Rect
{
    float left{0.f};
    float top{0.f};
    float right{0.f};
    float bottom{0.f};

    constexpr Rect() = default;
    constexpr Rect(float left, float top, float right, float bottom) : left(left), top(top), right(right), bottom(bottom) {}
    constexpr Rect(const Point& min, const Size& size) : Rect(min.x, min.y, min.x + size.width, min.y + size.height) {}

    constexpr Point min() const { return Point(left, top); }
    constexpr Point max() const { return Point(right, bottom); }
    constexpr float width() const { return right - left; }
    constexpr float height() const { return bottom - top; }
    constexpr bool  contains(const Point& p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
};

// Red, green, blue and alpha from 0 to 1.
struct Color
{
    float r{0.f};
    float g{0.f};
    float b{0.f};
    float a{1.f};

    constexpr Color() = default;
    constexpr Color(float r, float g, float b, float a = 1.f) : r(r), g(g), b(b), a(a) {}

    static constexpr Color from_bytes(uint8_t r, uint8_t g, uint8_t b, float alpha = 1.f)
    {
        return Color(float(r) / 255.f, float(g) / 255.f, float(b) / 255.f, alpha);
    }

    constexpr bool operator==(const Color& other) const { return r == other.r && g == other.g && b == other.b && a == other.a; }
    constexpr bool operator!=(const Color& other) const { return !(*this == other); }
};

Color rgb(int hex, float alpha = 1.f);
// a turned toward b by t, in whole steps of a colour byte, with the alpha of a.
Color mix(const Color& a, const Color& b, float t);
// The alpha multiplied by factor, clamped to [0, 1], in whole steps of a colour byte.
Color faded(const Color& c, float factor);
Color blend_over(const Color& top, const Color& bottom);
float contrast_ratio(const Color& a, const Color& b);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

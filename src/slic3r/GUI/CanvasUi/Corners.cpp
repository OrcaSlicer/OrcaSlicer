#include "Corners.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

namespace {

constexpr float PI_F = 3.14159265358979f;

float radians(float degrees) { return degrees * PI_F / 180.f; }

Point lerp(const Point& a, const Point& b, float t) { return Point(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); }

float distance(const Point& a, const Point& b) { return std::hypot(b.x - a.x, b.y - a.y); }

void add_cubic(std::vector<Point>& out, const Point& p0, const Point& p1, const Point& p2, const Point& p3, int steps)
{
    for (int i = 1; i <= steps; ++i) {
        const float t = float(i) / float(steps), u = 1.f - t;
        out.emplace_back(u * u * u * p0.x + 3.f * u * u * t * p1.x + 3.f * u * t * t * p2.x + t * t * t * p3.x,
                         u * u * u * p0.y + 3.f * u * u * t * p1.y + 3.f * u * t * t * p2.y + t * t * t * p3.y);
    }
}

// The top right corner around the origin, from (-p, 0) on the top side to (0, p) on the right side:
// a cubic out of the side, a circular arc of the radius, and the mirrored cubic into the other side.
std::vector<Point> top_right_corner(float radius, float smoothing)
{
    constexpr int STEPS = 32;
    const float   p           = (1.f + smoothing) * radius;
    const float   arc_measure = 90.f * (1.f - smoothing);
    const float   arc_section = std::sin(radians(arc_measure / 2.f)) * radius * std::sqrt(2.f);
    const float   alpha       = (90.f - arc_measure) / 2.f;
    const float   p3_to_p4    = radius * std::tan(radians(alpha / 2.f));
    const float   beta        = 45.f * smoothing;
    const float   c           = p3_to_p4 * std::cos(radians(beta));
    const float   d           = c * std::tan(radians(beta));
    const float   b           = (p - arc_section - c - d) / 3.f;
    const float   a           = 2.f * b;

    std::vector<Point> curve{Point(-p, 0.f)};
    const Point        start(-p, 0.f);
    const Point        arc_start(start.x + a + b + c, d);
    add_cubic(curve, start, Point(start.x + a, 0.f), Point(start.x + a + b, 0.f), arc_start, STEPS);

    const Point arc_end(arc_start.x + arc_section, arc_start.y + arc_section);
    if (arc_section > 0.f) {
        const Point mid      = lerp(arc_start, arc_end, .5f);
        const float half     = .5f * distance(arc_start, arc_end);
        const float to_inner = std::sqrt(std::max(0.f, radius * radius - half * half));
        // The centre lies off the chord toward the inside of the rectangle, down and to the left.
        const Point center(mid.x - to_inner / std::sqrt(2.f), mid.y + to_inner / std::sqrt(2.f));
        const float from = std::atan2(arc_start.y - center.y, arc_start.x - center.x);
        const float to   = std::atan2(arc_end.y - center.y, arc_end.x - center.x);
        for (int i = 1; i <= STEPS; ++i) {
            const float angle = from + (to - from) * float(i) / float(STEPS);
            curve.emplace_back(center.x + radius * std::cos(angle), center.y + radius * std::sin(angle));
        }
    }
    add_cubic(curve, arc_end, Point(arc_end.x + d, arc_end.y + c), Point(arc_end.x + d, arc_end.y + b + c), Point(0.f, p), STEPS);
    return curve;
}

// The curve resampled to count points evenly spaced along it, both ends included.
std::vector<Point> resample(const std::vector<Point>& curve, int count)
{
    std::vector<float> along(curve.size(), 0.f);
    for (size_t i = 1; i < curve.size(); ++i)
        along[i] = along[i - 1] + distance(curve[i - 1], curve[i]);
    std::vector<Point> out;
    out.reserve(size_t(count));
    size_t segment = 1;
    for (int i = 0; i < count; ++i) {
        const float target = along.back() * float(i) / float(count - 1);
        while (segment + 1 < curve.size() && along[segment] < target)
            ++segment;
        const float length = along[segment] - along[segment - 1];
        out.push_back(length > 0.f ? lerp(curve[segment - 1], curve[segment], (target - along[segment - 1]) / length) : curve[segment]);
    }
    return out;
}

} // namespace

std::vector<Point> rounded_rect_outline(const Rect& rect, float radius, float smoothing, Corners corners)
{
    const float budget = .5f * std::min(rect.width(), rect.height());
    const float r      = std::clamp(radius, 0.f, std::max(budget, 0.f));
    const float s      = r > 0.f ? std::clamp(smoothing, 0.f, std::max(0.f, budget / r - 1.f)) : 0.f;

    std::vector<Point> corner = r > 0.f ? resample(top_right_corner(r, s), CORNER_POINTS) : std::vector<Point>(size_t(CORNER_POINTS));
    const std::array<Point, 4>   origins = {Point(rect.left, rect.top), Point(rect.right, rect.top), Point(rect.right, rect.bottom),
                                            Point(rect.left, rect.bottom)};
    const std::array<Corners, 4> flags   = {Corners::TopLeft, Corners::TopRight, Corners::BottomRight, Corners::BottomLeft};

    std::vector<Point> outline;
    outline.reserve(4 * size_t(CORNER_POINTS));
    for (size_t k = 0; k < 4; ++k) {
        const bool round = r > 0.f && has(corners, flags[k]);
        for (const Point& local : corner) {
            if (!round) {
                outline.push_back(origins[k]);
                continue;
            }
            // Turned clockwise on the screen from the top right corner: three quarter turns make the top left one.
            Point q = local;
            for (size_t turn = 0; turn < (k + 3) % 4; ++turn)
                q = Point(-q.y, q.x);
            outline.emplace_back(origins[k].x + q.x, origins[k].y + q.y);
        }
    }
    return outline;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r

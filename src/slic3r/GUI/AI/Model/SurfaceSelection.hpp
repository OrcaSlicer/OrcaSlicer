#pragma once

#include "libslic3r/TriangleMesh.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r::AI::SurfaceSelection {

struct Result {
    std::vector<size_t> faces;
    bool canceled {false};
    size_t tested_faces {0};
    size_t visibility_queries {0};
};

namespace detail {
constexpr double epsilon = 1e-9;
using Triangle = std::array<Vec2d, 3>;

inline double cross(const Vec2d& a, const Vec2d& b) { return a.x() * b.y() - a.y() * b.x(); }

inline Vec2d closest(const Vec2d& p, const Vec2d& a, const Vec2d& b)
{
    const Vec2d edge = b - a;
    const double length = edge.squaredNorm();
    return a + edge * (length <= epsilon ? 0.0 : std::clamp((p - a).dot(edge) / length, 0.0, 1.0));
}

inline bool in_triangle(const Vec2d& p, const Triangle& t)
{
    const double a = cross(t[1] - t[0], p - t[0]);
    const double b = cross(t[2] - t[1], p - t[1]);
    const double c = cross(t[0] - t[2], p - t[2]);
    return (a >= -epsilon && b >= -epsilon && c >= -epsilon) ||
           (a <= epsilon && b <= epsilon && c <= epsilon);
}

inline bool in_polygon(const Vec2d& p, const std::vector<Vec2d>& polygon)
{
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec2d& a = polygon[j];
        const Vec2d& b = polygon[i];
        if ((closest(p, a, b) - p).squaredNorm() <= epsilon * epsilon)
            return true;
        if ((a.y() > p.y()) != (b.y() > p.y()) &&
            p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x())
            inside = !inside;
    }
    return inside;
}

inline std::optional<Vec2d> intersection(const Vec2d& a, const Vec2d& b,
                                       const Vec2d& c, const Vec2d& d)
{
    const Vec2d ab = b - a, cd = d - c;
    const double divisor = cross(ab, cd);
    if (std::abs(divisor) <= epsilon)
        return std::nullopt;
    const double u = cross(c - a, cd) / divisor, v = cross(c - a, ab) / divisor;
    if (u < -epsilon || u > 1.0 + epsilon || v < -epsilon || v > 1.0 + epsilon)
        return std::nullopt;
    return Vec2d(a + std::clamp(u, 0.0, 1.0) * ab);
}

struct Bounds {
    Vec2d minimum, maximum;
    explicit Bounds(const Vec2d& p) : minimum(p), maximum(p) {}
    void add(const Vec2d& p) { minimum = minimum.cwiseMin(p); maximum = maximum.cwiseMax(p); }
    void expand(double radius) { minimum.array() -= radius; maximum.array() += radius; }
    bool overlaps(const Bounds& b) const {
        return minimum.x() <= b.maximum.x() && maximum.x() >= b.minimum.x() &&
               minimum.y() <= b.maximum.y() && maximum.y() >= b.minimum.y();
    }
};

// Project is supplied by the view; Pick returns the first visible mesh face at
// the same screen coordinate (normally using the editor's existing ray BVH).
// Only sample points within BOTH the face and gesture can authorize selection.
// Selection is conservative at sub-face occlusion edges, never through-surface.
template<class Project, class Pick, class Candidates, class Contains>
Result scan(const indexed_triangle_set& mesh, const Bounds& bounds, Project&& project, Pick&& pick,
            Candidates&& candidates, Contains&& contains, const std::function<bool()>& canceled)
{
    Result result;
    auto should_cancel = [&]() {
        if (canceled && canceled()) {
            result.faces.clear();
            result.canceled = true;
        }
        return result.canceled;
    };
    std::vector<std::optional<Vec2d>> projected;
    projected.reserve(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        if ((i & 1023) == 0 && should_cancel()) return result;
        auto p = project(mesh.vertices[i]);
        if (p && !p->allFinite()) p.reset();
        projected.push_back(p);
    }
    std::vector<Vec2d> samples;
    for (size_t face = 0; face < mesh.indices.size(); ++face) {
        if ((face & 255) == 0 && should_cancel()) return result;
        Triangle triangle;
        bool valid = true;
        for (size_t corner = 0; corner < 3; ++corner) {
            const auto vertex = mesh.indices[face][corner];
            if (vertex < 0 || size_t(vertex) >= projected.size() || !projected[vertex]) {
                valid = false;
                break;
            }
            triangle[corner] = *projected[vertex];
        }
        if (!valid || std::abs(cross(triangle[1] - triangle[0], triangle[2] - triangle[0])) <= epsilon)
            continue;
        Bounds triangle_bounds(triangle[0]);
        triangle_bounds.add(triangle[1]); triangle_bounds.add(triangle[2]);
        if (!bounds.overlaps(triangle_bounds)) continue;
        ++result.tested_faces;
        const Vec2d center = (triangle[0] + triangle[1] + triangle[2]) / 3.0;
        auto visible = [&](const Vec2d& p) {
            if (!contains(p) || !in_triangle(p, triangle)) return false;
            ++result.visibility_queries;
            const auto hit = pick(p);
            return hit && *hit == face;
        };
        if (visible(center)) {
            result.faces.push_back(face);
            continue;
        }
        samples.clear();
        candidates(triangle, triangle_bounds, samples);
        bool selected = false;
        for (const Vec2d& point : samples) {
            // Shared-edge rays may hit either neighbor. Try a small step into
            // this face first, without expanding the user's requested region.
            if (visible(point * 0.999 + center * 0.001) || visible(point)) {
                selected = true;
                break;
            }
        }
        if (selected) result.faces.push_back(face);
    }
    should_cancel();
    return result;
}
} // namespace detail

// Polygon may be concave; its final edge is closed implicitly. All coordinates
// are in the same screen units as Project/Pick, independent of model scale.
template<class Project, class Pick>
Result visible_polygon_faces(const indexed_triangle_set& mesh, const std::vector<Vec2d>& polygon,
                             Project&& project, Pick&& pick, const std::function<bool()>& canceled = {})
{
    if (polygon.size() < 3 || std::any_of(polygon.begin(), polygon.end(), [](const Vec2d& p) { return !p.allFinite(); }))
        return {};
    detail::Bounds bounds(polygon.front());
    for (const auto& p : polygon) bounds.add(p);
    auto contains = [&](const Vec2d& p) { return detail::in_polygon(p, polygon); };
    auto candidates = [&](const detail::Triangle& triangle, const detail::Bounds&, std::vector<Vec2d>& points) {
        for (const auto& p : triangle) if (contains(p)) points.push_back(p);
        for (const auto& p : polygon) if (detail::in_triangle(p, triangle)) points.push_back(p);
        for (size_t i = 0; i < polygon.size(); ++i)
            for (size_t edge = 0; edge < 3; ++edge)
                if (auto p = detail::intersection(polygon[i], polygon[(i + 1) % polygon.size()],
                                                  triangle[edge], triangle[(edge + 1) % 3]))
                    points.push_back(*p);
    };
    return detail::scan(mesh, bounds, std::forward<Project>(project), std::forward<Pick>(pick), candidates, contains, canceled);
}

// Continuous circular brush: one point is a dab; successive points form
// capsules, so fast drags leave no sampling gaps. No color/part priors are used.
template<class Project, class Pick>
Result visible_brush_faces(const indexed_triangle_set& mesh, const std::vector<Vec2d>& stroke,
                           double radius, Project&& project, Pick&& pick,
                           const std::function<bool()>& canceled = {})
{
    if (stroke.empty() || !std::isfinite(radius) || radius <= 0.0 ||
        std::any_of(stroke.begin(), stroke.end(), [](const Vec2d& p) { return !p.allFinite(); }))
        return {};
    detail::Bounds bounds(stroke.front());
    std::vector<detail::Bounds> segment_bounds;
    const size_t segment_count = std::max(size_t(1), stroke.size() - 1);
    for (const auto& p : stroke) bounds.add(p);
    bounds.expand(radius);
    for (size_t i = 0; i < segment_count; ++i) {
        detail::Bounds segment(stroke[i]);
        segment.add(stroke[std::min(i + 1, stroke.size() - 1)]);
        segment.expand(radius);
        segment_bounds.push_back(segment);
    }
    auto contains = [&](const Vec2d& p) {
        const detail::Bounds point_bounds(p);
        for (size_t i = 0; i < segment_count; ++i)
            if (segment_bounds[i].overlaps(point_bounds) &&
                (p - detail::closest(p, stroke[i], stroke[std::min(i + 1, stroke.size() - 1)])).squaredNorm() <= radius * radius)
                return true;
        return false;
    };
    auto candidates = [&](const detail::Triangle& triangle, const detail::Bounds& triangle_bounds, std::vector<Vec2d>& points) {
        for (const auto& p : triangle) if (contains(p)) points.push_back(p);
        for (size_t i = 0; i < segment_count; ++i) {
            if (!segment_bounds[i].overlaps(triangle_bounds)) continue;
            const Vec2d& a = stroke[i];
            const Vec2d& b = stroke[std::min(i + 1, stroke.size() - 1)];
            if (detail::in_triangle(a, triangle)) points.push_back(a);
            if (detail::in_triangle(b, triangle)) points.push_back(b);
            for (size_t edge = 0; edge < 3; ++edge) {
                const Vec2d& v = triangle[edge];
                const Vec2d& w = triangle[(edge + 1) % 3];
                points.push_back(detail::closest(a, v, w));
                points.push_back(detail::closest(b, v, w));
                if (auto p = detail::intersection(a, b, v, w)) points.push_back(*p);
            }
        }
    };
    return detail::scan(mesh, bounds, std::forward<Project>(project), std::forward<Pick>(pick), candidates, contains, canceled);
}

} // namespace Slic3r::AI::SurfaceSelection

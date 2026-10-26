#include "SeamContour.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>
#include <limits>
#include <utility>

namespace Slic3r::SeamContour {
namespace {
struct Foot {
    Vec2d point = Vec2d::Zero();
    double distance = std::numeric_limits<double>::infinity();
};
struct Witness {
    size_t contour;
    double distance;
    double angle_degrees;
};
struct BoxEntry {
    size_t index;
    AABBTreeIndirect::Tree2d::BoundingBox box;
    size_t idx() const { return index; }
    const AABBTreeIndirect::Tree2d::BoundingBox &bbox() const { return box; }
    Vec2d centroid() const { return box.center(); }
};

double box_distance(const Contour &c, const Vec2d &p) {
    return ((c.min - p).cwiseMax(Vec2d::Zero()) + (p - c.max).cwiseMax(Vec2d::Zero())).norm();
}

Foot closest(const Contour &c, const Vec2d &p, double stop = -1.0,
             const std::vector<Vec2d> *source_normals = nullptr, double normal_cosine = 0.0) {
    Foot result;
    if (stop >= 0.0 && !source_normals)
        for (const Vec2d &vertex : c.points)
            if ((vertex - p).norm() <= stop) return {vertex, (vertex - p).norm()};
    for (size_t i = 0; i < c.points.size(); ++i) {
        const Vec2d &a = c.points[i];
        const Vec2d d = c.points[(i + 1) % c.points.size()] - a;
        const double length2 = d.squaredNorm();
        if (source_normals) {
            if (length2 <= 1e-18) continue;
            const Vec2d normal = (c.hole ? -1.0 : 1.0) * Vec2d(d.y(), -d.x()) / std::sqrt(length2);
            // A thin contour's nearest opposite side must not hide its slightly farther facing side.
            if (std::none_of(source_normals->begin(), source_normals->end(), [&](const Vec2d &n) {
                    return n.dot(normal) >= normal_cosine;
                })) continue;
        }
        const double t = length2 > 0.0 ? std::clamp((p - a).dot(d) / length2, 0.0, 1.0) : 0.0;
        const Vec2d q = a + t * d;
        const double distance = (q - p).norm();
        if (distance < result.distance) result = {q, distance};
        if (result.distance <= stop) break; // A proximity certificate needs no exact closest point.
    }
    return result;
}

std::vector<Vec2d> normals(const Contour &c, const Vec2d &p) {
    std::vector<Vec2d> result;
    for (size_t i = 0; i < c.points.size(); ++i) {
        const Vec2d &a = c.points[i];
        const Vec2d d = c.points[(i + 1) % c.points.size()] - a;
        if (d.squaredNorm() <= 1e-18) continue;
        const double t = std::clamp((p - a).dot(d) / d.squaredNorm(), 0.0, 1.0);
        // Every caller supplies a vertex or closest foot on this boundary; avoid a second full search.
        if ((a + t * d - p).norm() <= 1e-6) {
            // Candidate polygons are CCW; a hole has material on their opposite side.
            const Vec2d n(d.y(), -d.x());
            result.push_back((c.hole ? -1.0 : 1.0) * n.normalized());
        }
    }
    return result;
}

} // namespace

std::vector<size_t> Resolver::query(size_t layer, const Vec2d &p, double reach) const {
    std::vector<size_t> result;
    const Vec2d margin = Vec2d::Constant(reach);
    AABBTreeIndirect::traverse(m_indices[layer],
        AABBTreeIndirect::intersecting(AABBTreeIndirect::Tree2d::BoundingBox(p - margin, p + margin)),
        [&](const auto &node) { result.push_back(node.idx); return true; });
    // Tree traversal order must not alter tie-breaking in the existing contour predicates.
    std::sort(result.begin(), result.end());
    return result;
}

bool Resolver::in_material(size_t layer, const Vec2d &p) const {
    bool inside = false;
    for (size_t index : query(layer, p, 0.0)) {
        const Contour &c = m_layers[layer][index];
        if (!c.reliable) continue; // select() already falls back wherever unknown geometry can affect a probe.
        if ((p.array() < c.min.array()).any() || (p.array() > c.max.array()).any()) continue;
        for (size_t i = 0; i < c.points.size(); ++i) {
            const Vec2d &a = c.points[i], &b = c.points[(i + 1) % c.points.size()];
            if ((a.y() > p.y()) != (b.y() > p.y()) &&
                p.x() < a.x() + (p.y() - a.y()) * (b.x() - a.x()) / (b.y() - a.y()))
                inside = !inside;
        }
    }
    return inside;
}

bool Resolver::near_boundary(size_t layer, const Vec2d &p, double noise) const {
    for (size_t index : query(layer, p, noise)) {
        const Contour &c = m_layers[layer][index];
        if (box_distance(c, p) <= noise && closest(c, p, noise).distance <= noise) return true;
    }
    return false;
}

Resolver::Resolver(Layers layers, Options options, std::function<void()> throw_if_canceled) :
    m_layers(std::move(layers)), m_options(options) {
    m_indices.resize(m_layers.size());
    for (size_t i = 0; i < m_layers.size(); ++i) {
        if (throw_if_canceled) throw_if_canceled(); // Index construction on dense objects remains cancellable.
        std::vector<BoxEntry> boxes;
        for (size_t j = 0; j < m_layers[i].size(); ++j) {
            Contour &c = m_layers[i][j];
            c.min = Vec2d::Constant(std::numeric_limits<double>::infinity());
            c.max = -c.min;
            for (const Vec2d &p : c.points) { c.min = c.min.cwiseMin(p); c.max = c.max.cwiseMax(p); }
            if (!c.points.empty()) boxes.push_back({j, AABBTreeIndirect::Tree2d::BoundingBox(c.min, c.max)});
        }
        // Build once per layer; all seeds, reverse matches and material probes reuse the boxes.
        m_indices[i].build(std::move(boxes));
    }
}

Result Resolver::select(size_t layer, size_t contour, const Vec2d &p, int direction, double radius,
                        bool check_reverse) const {
    const size_t target = size_t(ptrdiff_t(layer) + direction);
    const Contour &source = m_layers[layer][contour];
    const auto &next = m_layers[target];
    const double bounds = source.nozzle * m_options.bounds_nozzle_factor;
    const double noise = source.width * m_options.noise_width_factor;
    const double reach = std::max({radius, bounds, noise});
    const auto candidates = query(target, p, reach);
    // An open path near either end or a material probe makes the geometric restriction uncertain.
    for (size_t i : query(layer, p, reach))
        if (!m_layers[layer][i].reliable) return {Outcome::None, Method::Local, {}, true};
    for (size_t i : candidates)
        if (!next[i].reliable) return {Outcome::None, Method::Local, {}, true};
    std::vector<size_t> fast, nearby;
    for (size_t i : candidates) {
        const Contour &c = next[i];
        if (c.points.size() < 3) continue;
        // A wall can become a hole when an opening closes; local geometry decides that continuation.
        if (box_distance(c, p) <= radius) nearby.push_back(i);
        if (c.hole == source.hole && (c.min - source.min).cwiseAbs().maxCoeff() <= bounds &&
            (c.max - source.max).cwiseAbs().maxCoeff() <= bounds) fast.push_back(i);
    }
    const auto source_normals = normals(source, p);
    const auto compatible_foot = [&](const Contour &c) {
        // Across a type change, use the closest patch itself: another side can mimic a shifted annulus.
        return closest(c, p, -1.0, c.hole == source.hole ? &source_normals : nullptr,
                       m_options.normal_cosine);
    };
    const auto normal_agreement = [&](const Contour &c, const Vec2d &foot) {
        double agreement = -1.0;
        const auto target_normals = normals(c, foot);
        for (const Vec2d &a : source_normals)
            for (const Vec2d &b : target_normals) agreement = std::max(agreement, a.dot(b));
        return agreement;
    };
    if (fast.size() == 1) {
        const size_t index = fast.front();
        const Foot foot = compatible_foot(next[index]);
        // Bounds alone cannot certify a wall: only a near-coincident, consistently oriented patch can.
        if (foot.distance <= noise && normal_agreement(next[index], foot.point) >= m_options.normal_cosine) {
            bool competing = false;
            for (size_t other : nearby) {
                if (other == index || box_distance(next[other], p) > noise) continue;
                const Foot alternative = compatible_foot(next[other]);
                if (alternative.distance <= noise &&
                    normal_agreement(next[other], alternative.point) >= m_options.normal_cosine) {
                    competing = true;
                    break;
                }
            }
            // A competing local wall or failed reverse match must reach the full fallback.
            if (!competing) {
                if (!check_reverse) return {Outcome::Unique, Method::Bounds, {index}};
                const Result back = select(target, index, foot.point, -direction, radius, false);
                // An uncertain certificate must leave the entire legacy candidate set unrestricted.
                if (back.uncertain) return back;
                if (std::find(back.contours.begin(), back.contours.end(), contour) != back.contours.end())
                    return {Outcome::Unique, Method::Bounds, {index}};
            }
        }
    }
    std::vector<Witness> witnessed;
    for (size_t i : nearby) {
        const Foot foot = compatible_foot(next[i]);
        if (foot.distance > radius) continue;
        const double agreement = normal_agreement(next[i], foot.point);
        if (agreement < m_options.normal_cosine) continue;
        bool band = true;
        if (foot.distance > noise) {
            // Three witnesses reject connectors through material (or air) shared by both layers.
            for (double t : std::array<double, 3>{0.25, 0.5, 0.75}) {
                const Vec2d probe = p + t * (foot.point - p);
                if (in_material(layer, probe) == in_material(target, probe) &&
                    !near_boundary(layer, probe, noise) && !near_boundary(target, probe, noise)) {
                    band = false;
                    break;
                }
            }
        }
        if (!band) continue;
        if (check_reverse) {
            const Result back = select(target, i, foot.point, -direction, radius, false);
            if (back.uncertain) return back;
            // Several reverse continuations are valid if they still include the source wall.
            if (std::find(back.contours.begin(), back.contours.end(), contour) == back.contours.end()) continue;
        }
        witnessed.push_back({i, foot.distance, std::acos(std::clamp(agreement, -1.0, 1.0)) *
                                                 180.0 / std::acos(-1.0)});
    }
    Result result;
    const double distance_margin = std::max(1e-6, source.width * m_options.dominance_width_factor);
    const double angle_margin = std::max(1e-6, m_options.dominance_angle_degrees);
    for (const Witness &candidate : witnessed) {
        // A contour is discarded only with a meaningful advantage in every scored criterion.
        const bool dominated = std::any_of(witnessed.begin(), witnessed.end(), [&](const Witness &other) {
            return other.distance + distance_margin <= candidate.distance &&
                   other.angle_degrees + angle_margin <= candidate.angle_degrees;
        });
        if (!dominated) result.contours.push_back(candidate.contour);
    }
    result.outcome = result.contours.empty() ? Outcome::None :
                     result.contours.size() == 1 ? Outcome::Unique : Outcome::Multiple;
    return result;
}

Result Resolver::resolve(size_t layer, size_t contour, size_t vertex, int direction, double radius) const {
    Result result;
    if (direction != -1 && direction != 1) return result;
    const Contour &source = m_layers.at(layer).at(contour);
    if (!source.reliable) return {Outcome::None, Method::Local, {}, true};
    if (radius < 0.0) radius = source.width * m_options.radius_width_factor;
    if (!std::isfinite(radius)) return result;
    // Alternative seeds may have different widths; their actual KD search radii cannot share a cache entry.
    const Key key(layer, contour, vertex, direction, radius);
    const auto cached = m_cache.find(key);
    if (cached != m_cache.end()) return cached->second;
    const ptrdiff_t target = ptrdiff_t(layer) + direction;
    if (target >= 0 && size_t(target) < m_layers.size() && source.points.size() >= 3) {
        const Vec2d &p = source.points.at(vertex);
        result = select(layer, contour, p, direction, radius, true);
    }
    m_cache.emplace(key, result);
    return result;
}

} // namespace Slic3r::SeamContour

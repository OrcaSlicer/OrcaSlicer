#include "SurfaceSliceSidecar.hpp"

#include "SurfaceColorAnnotation.hpp"
#include "../AABBTreeLines.hpp"
#include "../Point.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace Slic3r::CoExtrusion {

struct SurfaceSliceLayer::Index {
    explicit Index(const Lines &lines) : distancer(lines) {}
    AABBTreeLines::LinesDistancer<Line> distancer;
};

SurfaceSliceLayer::SurfaceSliceLayer() = default;
SurfaceSliceLayer::~SurfaceSliceLayer() = default;
SurfaceSliceLayer::SurfaceSliceLayer(SurfaceSliceLayer&&) noexcept = default;
SurfaceSliceLayer& SurfaceSliceLayer::operator=(SurfaceSliceLayer&&) noexcept = default;

void SurfaceSliceLayer::append(
    ObjectID volume_id,
    const MeshSliceLines &lines,
    const SurfaceColorAnnotation &colors,
    std::optional<SurfaceColorId> fallback_color_id)
{
    m_index.reset();
    m_color_boundary_points.clear();
    m_lines.reserve(m_lines.size() + lines.size());
    m_sources.reserve(m_sources.size() + lines.size());
    for (const MeshSliceLine &line : lines) {
        m_lines.emplace_back(line.line);
        std::optional<SurfaceColorId> color_id = colors.triangle_color(line.face_index);
        if (!color_id)
            color_id = fallback_color_id;
        m_sources.push_back({ volume_id, line.face_index, color_id });
    }
}

void SurfaceSliceLayer::finalize()
{
    // Adjacent sliced faces share endpoints. Only a change of painted color
    // within the same volume creates a seam candidate, not every mesh edge.
    m_color_boundary_points.clear();
    std::map<std::pair<ObjectID, Point>, SurfaceColorId> endpoint_colors;
    std::set<Point> boundaries;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        const SurfaceSliceSource &source = m_sources[i];
        if (!source.color_id)
            continue;
        for (const Point &point : { m_lines[i].a, m_lines[i].b }) {
            const auto [it, inserted] = endpoint_colors.emplace(
                std::make_pair(source.volume_id, point), *source.color_id);
            if (!inserted && it->second != *source.color_id)
                boundaries.insert(point);
        }
    }
    m_color_boundary_points.assign(boundaries.begin(), boundaries.end());
    if (!m_lines.empty())
        m_index = std::make_unique<Index>(m_lines);
}

std::optional<SurfaceSliceMatch> SurfaceSliceLayer::match_point(const Point &point, double max_distance_mm) const
{
    if (!m_index || max_distance_mm < 0.0)
        return std::nullopt;

    const auto [distance_scaled, line_index, nearest_point] =
        m_index->distancer.distance_from_lines_extra<false>(point);
    if (line_index >= m_sources.size() || !std::isfinite(distance_scaled))
        return std::nullopt;

    const double distance_mm = unscale<double>(distance_scaled);
    if (distance_mm > max_distance_mm)
        return std::nullopt;

    const SurfaceSliceSource &source = m_sources[line_index];
    SurfaceSliceMatch match;
    match.volume_id     = source.volume_id;
    match.face_index    = source.face_index;
    match.color_id      = source.color_id;
    match.nearest_point = Point(coord_t(std::llround(nearest_point.x())), coord_t(std::llround(nearest_point.y())));
    match.distance_mm   = distance_mm;
    return match;
}

std::optional<SurfaceSliceMatch> SurfaceSliceLayer::match_segment(
    const Line &segment,
    double max_distance_mm,
    double min_direction_alignment) const
{
    if (!m_index || max_distance_mm < 0.0 || segment.a == segment.b)
        return match_point(segment.midpoint(), max_distance_mm);

    const Point midpoint = segment.midpoint();
    const double radius_scaled = scaled<double>(max_distance_mm);
    const std::vector<size_t> candidates = m_index->distancer.all_lines_in_radius(midpoint, radius_scaled);
    const Vec2d query_direction = (segment.b - segment.a).cast<double>().normalized();

    size_t best_index = size_t(-1);
    Point  best_nearest;
    double best_distance_mm = 0.0;
    double best_score = std::numeric_limits<double>::infinity();
    for (size_t candidate_index : candidates) {
        if (candidate_index >= m_sources.size())
            continue;
        const Line &source_line = m_index->distancer.get_line(candidate_index);
        const Vec2d source_vector = (source_line.b - source_line.a).cast<double>();
        if (source_vector.squaredNorm() == 0.0)
            continue;
        const double alignment = std::abs(query_direction.dot(source_vector.normalized()));
        if (alignment < min_direction_alignment)
            continue;

        Point nearest_point;
        const double distance_mm = unscale<double>(std::sqrt(source_line.distance_to_squared(midpoint, &nearest_point)));
        if (distance_mm > max_distance_mm)
            continue;
        const double score = distance_mm + (1.0 - alignment) * max_distance_mm * 0.25;
        if (score < best_score) {
            best_index       = candidate_index;
            best_nearest     = nearest_point;
            best_distance_mm = distance_mm;
            best_score       = score;
        }
    }

    if (best_index == size_t(-1))
        return match_point(midpoint, max_distance_mm);

    const SurfaceSliceSource &source = m_sources[best_index];
    SurfaceSliceMatch match;
    match.volume_id     = source.volume_id;
    match.face_index    = source.face_index;
    match.color_id      = source.color_id;
    match.nearest_point = best_nearest;
    match.distance_mm   = best_distance_mm;
    return match;
}

bool SurfaceSliceSidecar::empty() const
{
    for (const SurfaceSliceLayer &layer : m_layers)
        if (!layer.empty())
            return false;
    return true;
}

void SurfaceSliceSidecar::append_volume(
    ObjectID volume_id,
    const std::vector<MeshSliceLines> &lines_by_layer,
    const SurfaceColorAnnotation &colors,
    std::optional<SurfaceColorId> fallback_color_id)
{
    if (m_layers.size() < lines_by_layer.size())
        m_layers.resize(lines_by_layer.size());
    for (size_t layer_index = 0; layer_index < lines_by_layer.size(); ++layer_index)
        m_layers[layer_index].append(volume_id, lines_by_layer[layer_index], colors, fallback_color_id);
}

void SurfaceSliceSidecar::finalize()
{
    for (SurfaceSliceLayer &layer : m_layers)
        layer.finalize();
}

const SurfaceSliceLayer* SurfaceSliceSidecar::layer(size_t layer_index) const
{
    return layer_index < m_layers.size() ? &m_layers[layer_index] : nullptr;
}

std::optional<SurfaceSliceMatch> SurfaceSliceSidecar::match_point(
    size_t layer_index,
    const Point &point,
    double max_distance_mm) const
{
    const SurfaceSliceLayer *source_layer = layer(layer_index);
    return source_layer == nullptr ? std::nullopt : source_layer->match_point(point, max_distance_mm);
}

std::optional<SurfaceSliceMatch> SurfaceSliceSidecar::match_segment(
    size_t layer_index,
    const Line &segment,
    double max_distance_mm,
    double min_direction_alignment) const
{
    const SurfaceSliceLayer *source_layer = layer(layer_index);
    return source_layer == nullptr ? std::nullopt :
        source_layer->match_segment(segment, max_distance_mm, min_direction_alignment);
}

} // namespace Slic3r::CoExtrusion

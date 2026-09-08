#pragma once

#include "CoExtrusionTypes.hpp"
#include "../Line.hpp"
#include "../ObjectID.hpp"
#include "../TriangleMeshSlicer.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace Slic3r::CoExtrusion {

class SurfaceColorAnnotation;

struct SurfaceSliceSource {
    ObjectID                      volume_id;
    size_t                        face_index { 0 };
    std::optional<SurfaceColorId> color_id;
};

struct SurfaceSliceMatch : public SurfaceSliceSource {
    Point  nearest_point;
    double distance_mm { 0.0 };
};

class SurfaceSliceLayer
{
public:
    SurfaceSliceLayer();
    ~SurfaceSliceLayer();
    SurfaceSliceLayer(const SurfaceSliceLayer&) = delete;
    SurfaceSliceLayer& operator=(const SurfaceSliceLayer&) = delete;
    SurfaceSliceLayer(SurfaceSliceLayer&&) noexcept;
    SurfaceSliceLayer& operator=(SurfaceSliceLayer&&) noexcept;

    void append(
        ObjectID volume_id,
        const MeshSliceLines &lines,
        const SurfaceColorAnnotation &colors,
        std::optional<SurfaceColorId> fallback_color_id = std::nullopt);
    void finalize();

    bool empty() const { return m_sources.empty(); }
    size_t size() const { return m_sources.size(); }
    const Points& color_boundary_points() const { return m_color_boundary_points; }
    std::optional<SurfaceSliceMatch> match_point(const Point &point, double max_distance_mm) const;
    std::optional<SurfaceSliceMatch> match_segment(
        const Line &segment,
        double max_distance_mm,
        double min_direction_alignment = 0.25) const;

private:
    struct Index;

    Lines                   m_lines;
    Points                  m_color_boundary_points;
    std::vector<SurfaceSliceSource> m_sources;
    std::unique_ptr<Index>  m_index;
};

class SurfaceSliceSidecar
{
public:
    explicit SurfaceSliceSidecar(size_t layer_count = 0) : m_layers(layer_count) {}

    bool empty() const;
    size_t layer_count() const { return m_layers.size(); }
    void resize(size_t layer_count) { m_layers.resize(layer_count); }

    void append_volume(
        ObjectID volume_id,
        const std::vector<MeshSliceLines> &lines_by_layer,
        const SurfaceColorAnnotation &colors,
        std::optional<SurfaceColorId> fallback_color_id = std::nullopt);
    void finalize();

    const SurfaceSliceLayer* layer(size_t layer_index) const;
    std::optional<SurfaceSliceMatch> match_point(
        size_t layer_index,
        const Point &point,
        double max_distance_mm) const;
    std::optional<SurfaceSliceMatch> match_segment(
        size_t layer_index,
        const Line &segment,
        double max_distance_mm,
        double min_direction_alignment = 0.25) const;

private:
    std::vector<SurfaceSliceLayer> m_layers;
};

} // namespace Slic3r::CoExtrusion

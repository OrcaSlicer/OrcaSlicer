#pragma once

#include "CoExtrusionTypes.hpp"
#include "../Geometry.hpp"
#include "../ObjectID.hpp"
#include "../Polyline.hpp"
#include "../TriangleMesh.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace Slic3r {

class AABBMesh;
class ModelObject;
class ModelVolume;

namespace CoExtrusion {

enum class ProvenanceSource {
    SliceIntersection,
    NearestSurfaceFallback,
};

struct SurfaceProvenance {
    ObjectID                     volume_id;
    size_t                       face_index { 0 };
    std::optional<SurfaceColorId> color_id;
    Vec3d                        surface_point { Vec3d::Zero() };
    Vec3d                        surface_normal { Vec3d::Zero() };
    double                       distance_mm { 0.0 };
    ProvenanceSource             source { ProvenanceSource::NearestSurfaceFallback };

    bool has_color() const { return color_id.has_value(); }
};

struct SurfacePathSample {
    Vec3d                            point { Vec3d::Zero() };
    double                           path_distance_mm { 0.0 };
    std::optional<SurfaceProvenance> provenance;
};

// Resolves final path points back to original ModelVolume triangles. Exact
// face provenance produced by the slicer should use provenance_for_face().
// nearest_provenance() is the bounded fallback for paths whose source was
// lost during polygon clipping or Arachne reconstruction.
class SurfaceProvenanceResolver
{
public:
    explicit SurfaceProvenanceResolver(const ModelVolume &volume);
    SurfaceProvenanceResolver(const ModelVolume &volume, const Transform3d &mesh_to_query);
    SurfaceProvenanceResolver(
        const ModelVolume &volume,
        const Transform3d &mesh_to_query,
        std::optional<SurfaceColorId> fallback_color_id);
    ~SurfaceProvenanceResolver();

    SurfaceProvenanceResolver(const SurfaceProvenanceResolver&) = delete;
    SurfaceProvenanceResolver& operator=(const SurfaceProvenanceResolver&) = delete;
    SurfaceProvenanceResolver(SurfaceProvenanceResolver&&) noexcept;
    SurfaceProvenanceResolver& operator=(SurfaceProvenanceResolver&&) noexcept;

    bool empty() const { return m_aabb == nullptr; }
    ObjectID volume_id() const { return m_volume_id; }

    std::optional<SurfaceProvenance> provenance_for_face(size_t face_index, const Vec3d &point) const;
    std::optional<SurfaceProvenance> nearest_provenance(const Vec3d &point, double max_distance_mm) const;

    std::vector<SurfacePathSample> sample_path(
        const Polyline3 &path,
        double max_segment_length_mm,
        double max_surface_distance_mm) const;

private:
    std::optional<SurfaceColorId> color_for_face(size_t face_index) const;

    ObjectID                    m_volume_id;
    indexed_triangle_set        m_mesh;
    std::vector<SurfaceColorId> m_color_ids;
    std::optional<SurfaceColorId> m_fallback_color_id;
    std::unique_ptr<AABBMesh>   m_aabb;
};

// Aggregates annotated model-part volumes and resolves the closest valid
// source across them. model_to_query must map ModelObject coordinates into the
// same coordinate system as the queried path points.
class ObjectSurfaceProvenanceResolver
{
public:
    explicit ObjectSurfaceProvenanceResolver(const ModelObject &object);
    ObjectSurfaceProvenanceResolver(const ModelObject &object, const Transform3d &model_to_query);

    bool empty() const { return m_volumes.empty(); }

    std::optional<SurfaceProvenance> provenance_for_face(
        ObjectID volume_id,
        size_t face_index,
        const Vec3d &point) const;

    std::optional<SurfaceProvenance> nearest_provenance(
        const Vec3d &point,
        double max_distance_mm) const;

private:
    std::vector<SurfaceProvenanceResolver> m_volumes;
};

} // namespace CoExtrusion
} // namespace Slic3r

#include "SurfaceProvenance.hpp"

#include "SurfaceColorAnnotation.hpp"
#include "../AABBMesh.hpp"
#include "../Model.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r::CoExtrusion {

static std::optional<SurfaceColorId> configured_surface_color_id(const ModelVolume &volume)
{
    const ModelObject *object = volume.get_object();
    if (object == nullptr)
        return std::nullopt;
    return configured_surface_color_id(*object, volume);
}

SurfaceProvenanceResolver::SurfaceProvenanceResolver(const ModelVolume &volume)
    : SurfaceProvenanceResolver(
        volume,
        volume.get_matrix(),
        configured_surface_color_id(volume))
{
}

SurfaceProvenanceResolver::SurfaceProvenanceResolver(const ModelVolume &volume, const Transform3d &mesh_to_query)
    : SurfaceProvenanceResolver(
        volume,
        mesh_to_query,
        configured_surface_color_id(volume))
{
}

SurfaceProvenanceResolver::SurfaceProvenanceResolver(
    const ModelVolume &volume,
    const Transform3d &mesh_to_query,
    std::optional<SurfaceColorId> fallback_color_id)
    : m_volume_id(volume.id())
    , m_mesh(volume.mesh().its)
    , m_color_ids(volume.coextrusion_surface_colors.data())
    , m_fallback_color_id(fallback_color_id)
{
    if (m_mesh.indices.empty() || m_mesh.vertices.empty())
        return;

    // The transformed copy makes distance queries correct for non-uniform
    // scaling and mirroring. Face order stays stable, so annotation indices
    // continue to refer to the original ModelVolume triangles.
    its_transform(m_mesh, mesh_to_query, true);
    m_aabb = std::make_unique<AABBMesh>(m_mesh);
}

SurfaceProvenanceResolver::~SurfaceProvenanceResolver() = default;

SurfaceProvenanceResolver::SurfaceProvenanceResolver(SurfaceProvenanceResolver &&other) noexcept
    : m_volume_id(other.m_volume_id)
    , m_mesh(std::move(other.m_mesh))
    , m_color_ids(std::move(other.m_color_ids))
    , m_fallback_color_id(other.m_fallback_color_id)
{
    // AABBMesh stores a pointer to the indexed_triangle_set, therefore it must
    // be rebuilt after moving the owning mesh.
    if (!m_mesh.indices.empty() && !m_mesh.vertices.empty())
        m_aabb = std::make_unique<AABBMesh>(m_mesh);
}

SurfaceProvenanceResolver& SurfaceProvenanceResolver::operator=(SurfaceProvenanceResolver &&other) noexcept
{
    if (this != &other) {
        m_volume_id = other.m_volume_id;
        m_mesh      = std::move(other.m_mesh);
        m_color_ids = std::move(other.m_color_ids);
        m_fallback_color_id = other.m_fallback_color_id;
        m_aabb.reset();
        if (!m_mesh.indices.empty() && !m_mesh.vertices.empty())
            m_aabb = std::make_unique<AABBMesh>(m_mesh);
    }
    return *this;
}

std::optional<SurfaceProvenance> SurfaceProvenanceResolver::provenance_for_face(size_t face_index, const Vec3d &point) const
{
    if (!m_aabb || face_index >= m_mesh.indices.size())
        return std::nullopt;

    SurfaceProvenance result;
    result.volume_id      = m_volume_id;
    result.face_index     = face_index;
    result.color_id       = color_for_face(face_index);
    result.surface_point  = point;
    result.surface_normal = m_aabb->normal_by_face_id(int(face_index));
    result.distance_mm    = 0.0;
    result.source         = ProvenanceSource::SliceIntersection;
    return result;
}

std::optional<SurfaceProvenance> SurfaceProvenanceResolver::nearest_provenance(
    const Vec3d &point,
    double max_distance_mm) const
{
    if (!m_aabb)
        return std::nullopt;

    int   face_index = -1;
    Vec3d closest_point = Vec3d::Zero();
    const double squared_distance = m_aabb->squared_distance(point, face_index, closest_point);
    if (face_index < 0 || size_t(face_index) >= m_mesh.indices.size() || !std::isfinite(squared_distance))
        return std::nullopt;

    if (max_distance_mm >= 0.0 && squared_distance > max_distance_mm * max_distance_mm)
        return std::nullopt;

    SurfaceProvenance result;
    result.volume_id      = m_volume_id;
    result.face_index     = size_t(face_index);
    result.color_id       = color_for_face(size_t(face_index));
    result.surface_point  = closest_point;
    result.surface_normal = m_aabb->normal_by_face_id(face_index);
    result.distance_mm    = std::sqrt(std::max(0.0, squared_distance));
    result.source         = ProvenanceSource::NearestSurfaceFallback;
    return result;
}

std::vector<SurfacePathSample> SurfaceProvenanceResolver::sample_path(
    const Polyline3 &path,
    double max_segment_length_mm,
    double max_surface_distance_mm) const
{
    std::vector<SurfacePathSample> samples;
    if (path.points.empty())
        return samples;

    const Vec3d first = unscale(path.points.front());
    samples.push_back({ first, 0.0, nearest_provenance(first, max_surface_distance_mm) });

    double accumulated_distance = 0.0;
    for (size_t segment_index = 1; segment_index < path.points.size(); ++segment_index) {
        const Vec3d from = unscale(path.points[segment_index - 1]);
        const Vec3d to   = unscale(path.points[segment_index]);
        const Vec3d delta = to - from;
        const double length = delta.norm();
        if (length <= 0.0)
            continue;

        const size_t subdivisions = max_segment_length_mm > 0.0 ?
            std::max<size_t>(1, size_t(std::ceil(length / max_segment_length_mm))) : 1;
        for (size_t subdivision = 1; subdivision <= subdivisions; ++subdivision) {
            const double ratio = double(subdivision) / double(subdivisions);
            const Vec3d point = from + ratio * delta;
            samples.push_back({
                point,
                accumulated_distance + ratio * length,
                nearest_provenance(point, max_surface_distance_mm)
            });
        }
        accumulated_distance += length;
    }

    return samples;
}

std::optional<SurfaceColorId> SurfaceProvenanceResolver::color_for_face(size_t face_index) const
{
    if (face_index >= m_color_ids.size() || m_color_ids[face_index] == SurfaceColorAnnotation::UNASSIGNED)
        return m_fallback_color_id;
    return m_color_ids[face_index];
}

ObjectSurfaceProvenanceResolver::ObjectSurfaceProvenanceResolver(const ModelObject &object)
    : ObjectSurfaceProvenanceResolver(object, Transform3d::Identity())
{
}

ObjectSurfaceProvenanceResolver::ObjectSurfaceProvenanceResolver(
    const ModelObject &object,
    const Transform3d &model_to_query)
{
    m_volumes.reserve(object.volumes.size());
    for (const ModelVolume *volume : object.volumes) {
        if (volume == nullptr || !volume->is_model_part())
            continue;
        const std::optional<SurfaceColorId> fallback_color_id = configured_surface_color_id(object, *volume);
        if (volume->has_coextrusion_surface_colors() || fallback_color_id)
            m_volumes.emplace_back(*volume, model_to_query * volume->get_matrix(), fallback_color_id);
    }
}

std::optional<SurfaceProvenance> ObjectSurfaceProvenanceResolver::provenance_for_face(
    ObjectID volume_id,
    size_t face_index,
    const Vec3d &point) const
{
    for (const SurfaceProvenanceResolver &volume : m_volumes) {
        if (volume.volume_id() == volume_id)
            return volume.provenance_for_face(face_index, point);
    }
    return std::nullopt;
}

std::optional<SurfaceProvenance> ObjectSurfaceProvenanceResolver::nearest_provenance(
    const Vec3d &point,
    double max_distance_mm) const
{
    std::optional<SurfaceProvenance> nearest;
    for (const SurfaceProvenanceResolver &volume : m_volumes) {
        std::optional<SurfaceProvenance> candidate = volume.nearest_provenance(point, max_distance_mm);
        if (candidate && (!nearest || candidate->distance_mm < nearest->distance_mm))
            nearest = std::move(candidate);
    }
    return nearest;
}

} // namespace Slic3r::CoExtrusion

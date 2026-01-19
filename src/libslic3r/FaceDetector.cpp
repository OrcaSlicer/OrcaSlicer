#include "FaceDetector.hpp"
#include "TriangleMesh.hpp"
#include "SLA/IndexedMesh.hpp"
#include "Model.hpp"
#include <unordered_set>

namespace Slic3r {
static const double BBOX_OFFSET = 2.0;
void FaceDetector::detect_exterior_face()
{
    struct MeshFacetRange {
        MeshFacetRange(TriangleMesh* tm, uint32_t facet_begin, uint32_t facet_end) :
            tm(tm), facet_begin(facet_begin), facet_end(facet_end) {}
        MeshFacetRange() : tm(nullptr), facet_begin(0), facet_end(0) {}
        TriangleMesh* tm;
        uint32_t facet_begin;
        uint32_t facet_end;
    };

    TriangleMesh object_mesh;
    std::vector<MeshFacetRange> volume_facet_ranges;
    for (int i = 0; i < m_meshes.size(); i++) {
        TriangleMesh vol_mesh = m_meshes[i];
        volume_facet_ranges.emplace_back(&m_meshes[i], object_mesh.stats().number_of_facets, object_mesh.stats().number_of_facets + vol_mesh.stats().number_of_facets);

        vol_mesh.transform(m_transfos[i]);
        object_mesh.merge(std::move(vol_mesh));
    }

    sla::IndexedMesh indexed_mesh(object_mesh);
    BoundingBoxf3 bbox = object_mesh.bounding_box();
    bbox.offset(BBOX_OFFSET);

    std::unordered_set<size_t> hit_face_indices;

    // x-axis rays
    for (double y = bbox.min.y(); y < bbox.max.y(); y += m_sample_interval) {
        for (double z = bbox.min.z(); z < bbox.max.z(); z += m_sample_interval) {
            auto hit_result = indexed_mesh.query_ray_hit({ bbox.min.x(), y, z }, { 1.0, 0.0, 0.0 });
            if (hit_result.is_hit())
                hit_face_indices.insert(hit_result.face());

            hit_result = indexed_mesh.query_ray_hit({ bbox.max.x(), y, z }, { -1.0, 0.0, 0.0 });
            if (hit_result.is_hit())
                hit_face_indices.insert(hit_result.face());
        }
    }

    // y-axis rays
    for (double x = bbox.min.x(); x < bbox.max.x(); x += m_sample_interval) {
        for (double z = bbox.min.z(); z < bbox.max.z(); z += m_sample_interval) {
            auto hit_result = indexed_mesh.query_ray_hit({ x, bbox.min.y(), z }, { 0.0, 1.0, 0.0 });
            if (hit_result.is_hit())
                hit_face_indices.insert(hit_result.face());

            hit_result = indexed_mesh.query_ray_hit({ x, bbox.max.y(), z }, { 0.0, -1.0, 0.0 });
            if (hit_result.is_hit())
                hit_face_indices.insert(hit_result.face());
        }
    }

    // z-axis rays
    for (double x = bbox.min.x(); x < bbox.max.x(); x += m_sample_interval) {
        for (double y = bbox.min.y(); y < bbox.max.y(); y += m_sample_interval) {
            auto hit_result = indexed_mesh.query_ray_hit({ x, y, bbox.min.z() }, { 0.0, 0.0, 1.0 });
            if (hit_result.is_hit())
                hit_face_indices.insert(hit_result.face());

            hit_result = indexed_mesh.query_ray_hit({ x, y, bbox.max.z() }, { 0.0, 0.0, -1.0 });
            if (hit_result.is_hit())
                hit_face_indices.insert(hit_result.face());
        }
    }

    for (size_t facet_idx : hit_face_indices) {
        TriangleMesh* tm = nullptr;
        uint32_t vol_facet_idx = 0;
        for (auto range : volume_facet_ranges) {
            if (facet_idx >= range.facet_begin && facet_idx < range.facet_end) {
                tm = range.tm;
                vol_facet_idx = facet_idx - range.facet_begin;
                break;
            }
        }

        tm->its.get_property(vol_facet_idx).type = EnumFaceTypes::eExteriorAppearance;
    }
}

}

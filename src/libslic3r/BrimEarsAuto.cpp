#include "BrimEarsAuto.hpp"

#include "ClipperUtils.hpp"
#include "Model.hpp"
#include "MultiPoint.hpp"
#include "TriangleMeshSlicer.hpp"

namespace Slic3r {

ExPolygons brim_ears_first_layer(const ModelObject& object, size_t instance)
{
    if (instance >= object.instances.size())
        return {};
    const std::vector<float> slice_height(1, 0.1f);
    MeshSlicingParamsEx      params;
    params.mode           = MeshSlicingParams::SlicingMode::Regular;
    params.closing_radius = 0.1f;
    params.extra_offset   = 0.05f;
    params.resolution     = 0.01;
    ExPolygons part_ex, negative_ex;
    const Transform3d inst = object.instances[instance]->get_matrix();
    for (const ModelVolume* v : object.volumes) {
        if (!v->is_model_part() && !v->is_negative_volume())
            continue;
        indexed_triangle_set its = v->mesh().its;
        if (its.indices.empty())
            continue;
        MeshSlicingParamsEx p(params);
        p.trafo = p.trafo * inst * v->get_matrix();
        if (p.trafo.rotation().determinant() < 0.)
            its_flip_triangles(its);
        const ExPolygons layer = slice_mesh_ex(its, slice_height, p).front();
        if (v->is_model_part())
            part_ex = union_ex(part_ex, layer);
        else
            negative_ex = union_ex(negative_ex, layer);
    }
    return diff_ex(part_ex, negative_ex);
}

Points brim_ears_corner_points(Polygon polygon, float detection_length_mm, float max_angle_deg, bool is_outer)
{
    const double angle_threshold = (180. - max_angle_deg) * PI / 180.0;
    if (detection_length_mm > 0) {
        Points points = polygon.points;
        points.push_back(points.front());
        points = MultiPoint::_douglas_peucker(points, detection_length_mm / SCALING_FACTOR);
        if (points.size() > 4)
            points.erase(points.end() - 1);
        polygon.points = points;
    }
    return is_outer ? polygon.convex_points(angle_threshold) : polygon.concave_points(angle_threshold);
}

Points brim_ears_auto_points(const ExPolygons& first_layer, float detection_length_mm, float max_angle_deg)
{
    Points out;
    for (const ExPolygon& ex : first_layer) {
        append(out, brim_ears_corner_points(ex.contour, detection_length_mm, max_angle_deg, true));
        Polygons holes = ex.holes;
        polygons_reverse(holes);
        for (const Polygon& h : holes)
            append(out, brim_ears_corner_points(h, detection_length_mm, max_angle_deg, false));
    }
    return out;
}

} // namespace Slic3r

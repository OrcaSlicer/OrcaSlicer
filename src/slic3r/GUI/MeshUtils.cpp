#include "MeshUtils.hpp"

#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CSGMesh/SliceCSGMesh.hpp"

#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"


#include <GL/glew.h>

#include <igl/unproject.h>

#include <cstdint>


namespace Slic3r {
namespace GUI {

void MeshClipper::set_behaviour(bool fill_cut, double contour_width)
{
    if (fill_cut != m_fill_cut || ! is_approx(contour_width, m_contour_width))
        m_result.reset();
    m_fill_cut = fill_cut;
    m_contour_width = contour_width;
}



void MeshClipper::set_plane(const ClippingPlane& plane)
{
    if (m_plane != plane) {
        m_plane = plane;
        m_result.reset();
    }
}


void MeshClipper::set_limiting_plane(const ClippingPlane& plane)
{
    if (m_limiting_plane != plane) {
        m_limiting_plane = plane;
        m_result.reset();
    }
}



void MeshClipper::set_mesh(const indexed_triangle_set& mesh)
{
    if (m_mesh.get() != &mesh) {
        m_mesh = &mesh;
        m_result.reset();
    }
}

void MeshClipper::set_mesh(AnyPtr<const indexed_triangle_set> &&ptr)
{
    if (m_mesh.get() != ptr.get()) {
        m_mesh = std::move(ptr);
        m_result.reset();
    }
}

void MeshClipper::set_negative_mesh(const indexed_triangle_set& mesh)
{
    if (m_negative_mesh.get() != &mesh) {
        m_negative_mesh = &mesh;
        m_result.reset();
    }
}

void MeshClipper::set_negative_mesh(AnyPtr<const indexed_triangle_set> &&ptr)
{
    if (m_negative_mesh.get() != ptr.get()) {
        m_negative_mesh = std::move(ptr);
        m_result.reset();
    }
}



void MeshClipper::set_transformation(const Geometry::Transformation& trafo)
{
    if (! m_trafo.get_matrix().isApprox(trafo.get_matrix())) {
        m_trafo = trafo;
        m_result.reset();
    }
}

void MeshClipper::render_cut(const ColorRGBA& color, const std::vector<size_t>* ignore_idxs)
{
    if (! m_result)
        recalculate_triangles();
    GLShaderProgram* curr_shader = wxGetApp().get_current_shader();
    if (curr_shader != nullptr)
        curr_shader->stop_using();

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        const Camera& camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("view_model_matrix", camera.get_view_matrix());
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        for (size_t i=0; i<m_result->cut_islands.size(); ++i) {
            if (ignore_idxs && std::binary_search(ignore_idxs->begin(), ignore_idxs->end(), i))
                continue;
            CutIsland& isl = m_result->cut_islands[i];
            isl.model.set_color(isl.disabled ? ColorRGBA(0.5f, 0.5f, 0.5f, 1.f) : color);
            isl.model.render();
        }
        shader->stop_using();
    }

    if (curr_shader != nullptr)
        curr_shader->start_using();
}


void MeshClipper::render_contour(const ColorRGBA& color, const std::vector<size_t>* ignore_idxs)
{
    if (! m_result)
        recalculate_triangles();

    GLShaderProgram* curr_shader = wxGetApp().get_current_shader();
    if (curr_shader != nullptr)
        curr_shader->stop_using();

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        const Camera& camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("view_model_matrix", camera.get_view_matrix());
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        for (size_t i=0; i<m_result->cut_islands.size(); ++i) {
            if (ignore_idxs && std::binary_search(ignore_idxs->begin(), ignore_idxs->end(), i))
                continue;
            CutIsland& isl = m_result->cut_islands[i];
            isl.model_expanded.set_color(isl.disabled ? ColorRGBA(1.f, 0.f, 0.f, 1.f) : color);
            isl.model_expanded.render();
        }
        shader->stop_using();
    }

    if (curr_shader != nullptr)
        curr_shader->start_using();
}

int MeshClipper::is_projection_inside_cut(const Vec3d& point_in) const
{
    if (!m_result || m_result->cut_islands.empty())
        return -1;
    Vec3d point = m_result->trafo.inverse() * point_in;
    Point pt_2d = Point::new_scale(Vec2d(point.x(), point.y()));

    for (int i=0; i<int(m_result->cut_islands.size()); ++i) {
        const CutIsland& isl = m_result->cut_islands[i];
        if (isl.expoly_bb.contains(pt_2d) && isl.expoly.contains(pt_2d))
            return i; // TODO: handle intersecting contours
    }
    return -1;
}

bool MeshClipper::has_valid_contour() const
{
    return m_result && std::any_of(m_result->cut_islands.begin(), m_result->cut_islands.end(), [](const CutIsland& isl) { return !isl.expoly.empty(); });
}

std::vector<Vec3d> MeshClipper::point_per_contour() const {
    std::vector<Vec3d> out;
    if (m_result == std::nullopt) {
        return out;
    }
    assert(m_result);
    for (const auto& isl : m_result->cut_islands) {
        assert(isl.expoly.contour.size() > 2);
        // Now return a point lying inside the contour but not in a hole.
        // We do this by taking a point lying close to the edge, repeating
        // this several times for different edges and distances from them.
        // (We prefer point not extremely close to the border.
        bool done = false;
        Vec2d p;
        size_t i = 1;
        while (i < isl.expoly.contour.size()) {
            const Vec2d& a = unscale(isl.expoly.contour.points[i-1]);
            const Vec2d& b = unscale(isl.expoly.contour.points[i]);
            Vec2d n = (b-a).normalized();
            std::swap(n.x(), n.y());
            n.x() = -1 * n.x();
            double f = 10.;
            while (f > 0.05) {
                p = (0.5*(b+a)) + f * n;
                if (isl.expoly.contains(Point::new_scale(p))) {
                    done = true;
                    break;
                }
                f = f/10.;
            }
            if (done)
                break;
            i += std::max(size_t(2), isl.expoly.contour.size() / 5);
        }
        // If the above failed, just return the centroid, regardless of whether
        // it is inside the contour or in a hole (we must return something).
        Vec2d c = done ? p : unscale(isl.expoly.contour.centroid());
        out.emplace_back(m_result->trafo * Vec3d(c.x(), c.y(), 0.));
    }
    return out;
}


void MeshClipper::recalculate_triangles()
{
    m_result = ClipResult();

    auto plane_mesh = Eigen::Hyperplane<double, 3>(m_plane.get_normal(), -m_plane.distance(Vec3d::Zero())).transform(m_trafo.get_matrix().inverse());
    const Vec3d up = plane_mesh.normal();
    const float height_mesh = -plane_mesh.offset();

    // Now do the cutting
    MeshSlicingParams slicing_params;
    slicing_params.trafo.rotate(Eigen::Quaternion<double, Eigen::DontAlign>::FromTwoVectors(up, Vec3d::UnitZ()));

    ExPolygons expolys;

    if (m_csgmesh.empty()) {
        if (m_mesh)
            expolys = union_ex(slice_mesh(*m_mesh, height_mesh, slicing_params));

        if (m_negative_mesh && !m_negative_mesh->empty()) {
            const ExPolygons neg_expolys = union_ex(slice_mesh(*m_negative_mesh, height_mesh, slicing_params));
            expolys = diff_ex(expolys, neg_expolys);
        }
    } else {
        expolys = std::move(csg::slice_csgmesh_ex(range(m_csgmesh), {height_mesh}, MeshSlicingParamsEx{slicing_params}).front());
    }


    // Triangulate and rotate the cut into world coords:
    Eigen::Quaterniond q;
    q.setFromTwoVectors(Vec3d::UnitZ(), up);
    Transform3d tr = Transform3d::Identity();
    tr.rotate(q);
    tr = m_trafo.get_matrix() * tr;

    m_result->trafo = tr;

    if (m_limiting_plane != ClippingPlane::ClipsNothing())
    {
        // Now remove whatever ended up below the limiting plane (e.g. sinking objects).
        // First transform the limiting plane from world to mesh coords.
        // Note that inverse of tr transforms the plane from world to horizontal.
        const Vec3d normal_old = m_limiting_plane.get_normal().normalized();
        const Vec3d normal_new = (tr.matrix().block<3,3>(0,0).transpose() * normal_old).normalized();

        // normal_new should now be the plane normal in mesh coords. To find the offset,
        // transform a point and set offset so it belongs to the transformed plane.
        Vec3d pt = Vec3d::Zero();
        const double plane_offset = m_limiting_plane.get_data()[3];
        if (std::abs(normal_old.z()) > 0.5) // normal is normalized, at least one of the coords if larger than sqrt(3)/3 = 0.57
            pt.z() = - plane_offset / normal_old.z();
        else if (std::abs(normal_old.y()) > 0.5)
            pt.y() = - plane_offset / normal_old.y();
        else
            pt.x() = - plane_offset / normal_old.x();
        pt = tr.inverse() * pt;
        const double offset = -(normal_new.dot(pt));

        if (std::abs(normal_old.dot(m_plane.get_normal().normalized())) > 0.99) {
            // The cuts are parallel, show all or nothing.
            if (normal_old.dot(m_plane.get_normal().normalized()) < 0.0 && offset < height_mesh)
                expolys.clear();
        } else {
            // The cut is a horizontal plane defined by z=height_mesh.
            // ax+by+e=0 is the line of intersection with the limiting plane.
            // Normalized so a^2 + b^2 = 1.
            const double len = std::hypot(normal_new.x(), normal_new.y());
            if (len == 0.)
                return;
            const double a = normal_new.x() / len;
            const double b = normal_new.y() / len;
            const double e = (normal_new.z() * height_mesh + offset) / len;

            // We need a half-plane to limit the cut. Get angle of the intersecting line.
            double angle = (b != 0.0) ? std::atan(-a / b) : ((a < 0.0) ? -0.5 * M_PI : 0.5 * M_PI);
            if (b > 0) // select correct half-plane
                angle += M_PI;

            // We'll take a big rectangle above x-axis and rotate and translate
            // it so it lies on our line. This will be the figure to subtract
            // from the cut. The coordinates must not overflow after the transform,
            // make the rectangle a bit smaller.
            const coord_t size = (std::numeric_limits<coord_t>::max()/2 - scale_(std::max(std::abs(e * a), std::abs(e * b)))) / 4;
            Polygons ep {Polygon({Point(-size, 0), Point(size, 0), Point(size, 2*size), Point(-size, 2*size)})};
            ep.front().rotate(angle);
            ep.front().translate(scale_(-e * a), scale_(-e * b));
            expolys = diff_ex(expolys, ep);
        }
    }

    tr.pretranslate(0.001 * m_plane.get_normal().normalized()); // to avoid z-fighting
    Transform3d tr2 = tr;
    tr2.pretranslate(0.002 * m_plane.get_normal().normalized());


    std::vector<Vec2f> triangles2d;

    for (const ExPolygon& exp : expolys) {
        triangles2d.clear();

        m_result->cut_islands.push_back(CutIsland());
        CutIsland& isl = m_result->cut_islands.back();

        if (m_fill_cut) {
            triangles2d = triangulate_expolygon_2f(exp, m_trafo.get_matrix().matrix().determinant() < 0.);
            GLModel::Geometry init_data;
            init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
            init_data.reserve_vertices(triangles2d.size());
            init_data.reserve_indices(triangles2d.size());

            // vertices + indices
            for (auto it = triangles2d.cbegin(); it != triangles2d.cend(); it = it + 3) {
                init_data.add_vertex((Vec3f)(tr * Vec3d((*(it + 0)).x(), (*(it + 0)).y(), height_mesh)).cast<float>(), (Vec3f)up.cast<float>());
                init_data.add_vertex((Vec3f)(tr * Vec3d((*(it + 1)).x(), (*(it + 1)).y(), height_mesh)).cast<float>(), (Vec3f)up.cast<float>());
                init_data.add_vertex((Vec3f)(tr * Vec3d((*(it + 2)).x(), (*(it + 2)).y(), height_mesh)).cast<float>(), (Vec3f)up.cast<float>());
                const size_t idx = it - triangles2d.cbegin();
                init_data.add_triangle((unsigned int)idx, (unsigned int)idx + 1, (unsigned int)idx + 2);
            }

            if (!init_data.is_empty())
                isl.model.init_from(std::move(init_data));
        }

        if (m_contour_width != 0. && ! exp.contour.empty()) {
            triangles2d.clear();

            // The contours must not scale with the object. Check the scale factor
            // in the respective directions, create a scaled copy of the ExPolygon
            // offset it and then unscale the result again.

            Transform3d t = tr;
            t.translation() = Vec3d::Zero();
            double scale_x = (t * Vec3d::UnitX()).norm();
            double scale_y = (t * Vec3d::UnitY()).norm();

            // To prevent overflow after scaling, downscale the input if needed:
            double extra_scale = 1.;
            coord_t limit = coord_t(std::min(std::numeric_limits<coord_t>::max() / (2. * std::max(1., scale_x)), std::numeric_limits<coord_t>::max() / (2. * std::max(1., scale_y))));
            coord_t max_coord = 0;
            for (const Point& pt : exp.contour)
                max_coord = std::max(max_coord, std::max(std::abs(pt.x()), std::abs(pt.y())));
            if (max_coord + m_contour_width >= limit)
                extra_scale = 0.9 * double(limit) / max_coord;

            ExPolygon exp_copy = exp;
            if (extra_scale != 1.)
                exp_copy.scale(extra_scale);
            exp_copy.scale(scale_x, scale_y);

            ExPolygons expolys_exp = offset_ex(exp_copy, scale_(m_contour_width));
            expolys_exp = diff_ex(expolys_exp, ExPolygons({exp_copy}));

            for (ExPolygon& e : expolys_exp) {
                e.scale(1./scale_x, 1./scale_y);
                if (extra_scale != 1.)
                    e.scale(1./extra_scale);
            }


            triangles2d = triangulate_expolygons_2f(expolys_exp, m_trafo.get_matrix().matrix().determinant() < 0.);
            GLModel::Geometry init_data = GLModel::Geometry();
            init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
            init_data.reserve_vertices(triangles2d.size());
            init_data.reserve_indices(triangles2d.size());

            // vertices + indices
            for (auto it = triangles2d.cbegin(); it != triangles2d.cend(); it = it + 3) {
                init_data.add_vertex((Vec3f)(tr2 * Vec3d((*(it + 0)).x(), (*(it + 0)).y(), height_mesh)).cast<float>(), (Vec3f)up.cast<float>());
                init_data.add_vertex((Vec3f)(tr2 * Vec3d((*(it + 1)).x(), (*(it + 1)).y(), height_mesh)).cast<float>(), (Vec3f)up.cast<float>());
                init_data.add_vertex((Vec3f)(tr2 * Vec3d((*(it + 2)).x(), (*(it + 2)).y(), height_mesh)).cast<float>(), (Vec3f)up.cast<float>());
                const size_t idx = it - triangles2d.cbegin();
                init_data.add_triangle((unsigned short)idx, (unsigned short)idx + 1, (unsigned short)idx + 2);
            }

            if (!init_data.is_empty())
                isl.model_expanded.init_from(std::move(init_data));
        }

        isl.expoly = std::move(exp);
        isl.expoly_bb = get_extents(isl.expoly);

        Point centroid_scaled = isl.expoly.contour.centroid();
        Vec3d centroid_world = m_result->trafo * Vec3d(unscale(centroid_scaled).x(), unscale(centroid_scaled).y(), 0.);
        isl.hash = isl.expoly.contour.size() + size_t(std::abs(100.*centroid_world.x())) + size_t(std::abs(100.*centroid_world.y())) + size_t(std::abs(100.*centroid_world.z()));
    }

    // Now sort the islands so they are in defined order. This is a hack needed by cut gizmo, which sometimes
    // flips the normal of the cut, in which case the contours stay the same but their order may change.
    std::sort(m_result->cut_islands.begin(), m_result->cut_islands.end(), [](const CutIsland& a, const CutIsland& b) {
        return a.hash < b.hash;
    });
}


Vec3f MeshRaycaster::get_triangle_normal(size_t facet_idx) const
{
    return m_normals[facet_idx];
}

void MeshRaycaster::line_from_mouse_pos(const Vec2d& mouse_pos, const Transform3d& trafo, const Camera& camera, Vec3d& point, Vec3d& direction)
{
    CameraUtils::ray_from_screen_pos(camera, mouse_pos, point, direction);
    Transform3d inv = trafo.inverse();
    point     = inv*point;
    direction = inv.linear()*direction;
}

bool MeshRaycaster::unproject_on_mesh(const Vec2d& mouse_pos, const Transform3d& trafo, const Camera& camera,
                                      Vec3f& position, Vec3f& normal, const ClippingPlane* clipping_plane,
                                      size_t* facet_idx, bool sinking_limit) const
{
    Vec3d point;
    Vec3d direction;
    CameraUtils::ray_from_screen_pos(camera, mouse_pos, point, direction);
    Transform3d inv = trafo.inverse();
    point     = inv*point;
    direction = inv.linear()*direction;

    std::vector<AABBMesh::hit_result> hits = m_emesh.query_ray_hits(point, direction);

    if (hits.empty())
        return false; // no intersection found

    unsigned i = 0;

    // Remove points that are obscured or cut by the clipping plane.
    // Also, remove anything below the bed (sinking objects).
    for (i=0; i<hits.size(); ++i) {
        Vec3d transformed_hit = trafo * hits[i].position();
        if (transformed_hit.z() >= (sinking_limit ? SINKING_Z_THRESHOLD : -std::numeric_limits<double>::max()) &&
            (!clipping_plane || !clipping_plane->is_point_clipped(transformed_hit)))
            break;
    }

    if (i==hits.size() || (hits.size()-i) % 2 != 0) {
        // All hits are either clipped, or there is an odd number of unclipped
        // hits - meaning the nearest must be from inside the mesh.
        return false;
    }

    // Now stuff the points in the provided vector and calculate normals if asked about them:
    position = hits[i].position().cast<float>();
    normal = hits[i].normal().cast<float>();

    if (facet_idx)
        *facet_idx = hits[i].face();

    return true;
}



bool MeshRaycaster::intersects_line(Vec3d point, Vec3d direction, const Transform3d& trafo) const 
{
    Transform3d trafo_inv = trafo.inverse();
    Vec3d to = trafo_inv * (point + direction);
    point = trafo_inv * point;
    direction = (to-point).normalized();

    std::vector<AABBMesh::hit_result> hits      = m_emesh.query_ray_hits(point, direction);
    std::vector<AABBMesh::hit_result> neg_hits  = m_emesh.query_ray_hits(point, -direction);

    return !hits.empty() || !neg_hits.empty();
}


std::vector<unsigned> MeshRaycaster::get_unobscured_idxs(const Geometry::Transformation& trafo, const Camera& camera, const std::vector<Vec3f>& points,
                                                       const ClippingPlane* clipping_plane) const
{
    std::vector<unsigned> out;

    const Transform3d instance_matrix_no_translation_no_scaling = trafo.get_rotation_matrix();
    Vec3d direction_to_camera = -camera.get_dir_forward();
    Vec3d direction_to_camera_mesh = (instance_matrix_no_translation_no_scaling.inverse() * direction_to_camera).normalized().eval();
    direction_to_camera_mesh = direction_to_camera_mesh.cwiseProduct(trafo.get_scaling_factor());
    const Transform3d inverse_trafo = trafo.get_matrix().inverse();

    for (size_t i=0; i<points.size(); ++i) {
        const Vec3f& pt = points[i];
        if (clipping_plane && clipping_plane->is_point_clipped(pt.cast<double>()))
            continue;

        bool is_obscured = false;
        // Cast a ray in the direction of the camera and look for intersection with the mesh:
        std::vector<AABBMesh::hit_result> hits;
        // Offset the start of the ray by EPSILON to account for numerical inaccuracies.
        hits = m_emesh.query_ray_hits((inverse_trafo * pt.cast<double>() + direction_to_camera_mesh * EPSILON),
                                      direction_to_camera_mesh);

        if (! hits.empty()) {
            // If the closest hit facet normal points in the same direction as the ray,
            // we are looking through the mesh and should therefore discard the point:
            if (hits.front().normal().dot(direction_to_camera_mesh.cast<double>()) > 0)
                is_obscured = true;

            // Eradicate all hits that the caller wants to ignore
            for (unsigned j=0; j<hits.size(); ++j) {
                if (clipping_plane && clipping_plane->is_point_clipped(trafo.get_matrix() * hits[j].position())) {
                    hits.erase(hits.begin()+j);
                    --j;
                }
            }

            // FIXME: the intersection could in theory be behind the camera, but as of now we only have camera direction.
            // Also, the threshold is in mesh coordinates, not in actual dimensions.
            if (! hits.empty())
                is_obscured = true;
        }
        if (! is_obscured)
            out.push_back(i);
    }
    return out;
}

bool MeshRaycaster::closest_hit(const Vec2d& mouse_pos, const Transform3d& trafo, const Camera& camera,
    Vec3f& position, Vec3f& normal, const ClippingPlane* clipping_plane, size_t* facet_idx) const
{
    Vec3d point;
    Vec3d direction;
    line_from_mouse_pos(mouse_pos, trafo, camera, point, direction);

    const std::vector<AABBMesh::hit_result> hits = m_emesh.query_ray_hits(point, direction.normalized());

    if (hits.empty())
        return false; // no intersection found

    size_t hit_id = 0;
    if (clipping_plane != nullptr) {
        while (hit_id < hits.size() && clipping_plane->is_point_clipped(trafo * hits[hit_id].position())) {
            ++hit_id;
        }
    }

    if (hit_id == hits.size())
        return false; // all points are obscured or cut by the clipping plane.

    const AABBMesh::hit_result& hit = hits[hit_id];

    position = hit.position().cast<float>();
    normal = hit.normal().cast<float>();

    if (facet_idx != nullptr)
        *facet_idx = hit.face();

    return true;
}

Vec3f MeshRaycaster::get_closest_point(const Vec3f& point, Vec3f* normal) const
{
    int idx = 0;
    Vec3d closest_point;
    Vec3d pointd = point.cast<double>();
    m_emesh.squared_distance(pointd, idx, closest_point);
    if (normal)
        // TODO: consider: get_normal(m_emesh, pointd).cast<float>();
        *normal = m_normals[idx];

    return closest_point.cast<float>();
}

int MeshRaycaster::get_closest_facet(const Vec3f &point) const
{
    int   facet_idx = 0;
    Vec3d closest_point;
    m_emesh.squared_distance(point.cast<double>(), facet_idx, closest_point);
    return facet_idx;
}

} // namespace GUI
} // namespace Slic3r

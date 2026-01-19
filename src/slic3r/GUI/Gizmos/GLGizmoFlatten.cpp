#include "GLGizmoFlatten.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Model.hpp"

#include <numeric>

#include <GL/glew.h>

namespace Slic3r {
namespace GUI {


GLGizmoFlatten::GLGizmoFlatten(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoFlatten::on_mouse(const wxMouseEvent &mouse_event)
{
    if (mouse_event.LeftDown()) {
        if (m_hover_id != -1) {
            Selection &selection = m_parent.get_selection();
            if (selection.is_single_full_instance()) {
                // Rotate the object so the normal points downward:
                selection.flattening_rotate(m_planes[m_hover_id].normal);
                m_parent.do_rotate(L("Gizmo-Place on Face"));
                wxGetApp().obj_manipul()->set_dirty();
            }
            return true;
        }
    }
    else if (mouse_event.LeftUp())
        return m_hover_id != -1;

    return false;
}

void GLGizmoFlatten::data_changed(bool is_serializing)
{
    const Selection &  selection    = m_parent.get_selection();
    const ModelObject *model_object = nullptr;
    int                instance_id = -1;
    if (selection.is_single_full_instance() ||
        selection.is_from_single_object() ) {        
        model_object = selection.get_model()->objects[selection.get_object_idx()];
        instance_id = selection.get_instance_idx();
    }    
    set_flattening_data(model_object, instance_id);
}

bool GLGizmoFlatten::on_init()
{
    m_shortcut_key = WXK_CONTROL_F;
    return true;
}

void GLGizmoFlatten::on_set_state()
{
}

CommonGizmosDataID GLGizmoFlatten::on_get_requirements() const
{
    return CommonGizmosDataID::SelectionInfo;
}

std::string GLGizmoFlatten::on_get_name() const
{
    return _u8L("Lay on face");
}

bool GLGizmoFlatten::on_is_activable() const
{
    // This is assumed in GLCanvas3D::do_rotate, do not change this
    // without updating that function too.
    return m_parent.get_selection().is_single_full_instance();
}

void GLGizmoFlatten::on_render()
{
    const Selection& selection = m_parent.get_selection();

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    
    shader->start_using();
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));

    if (selection.is_single_full_instance()) {
        const Transform3d& inst_matrix = selection.get_first_volume()->get_instance_transformation().get_matrix();
        const Camera& camera = wxGetApp().plater()->get_camera();
        const Transform3d model_matrix = Geometry::translation_transform(selection.get_first_volume()->get_sla_shift_z() * Vec3d::UnitZ()) * inst_matrix;
        const Transform3d view_model_matrix = camera.get_view_matrix() * model_matrix;

        shader->set_uniform("view_model_matrix", view_model_matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        if (this->is_plane_update_necessary())
            update_planes();
        for (int i = 0; i < (int)m_planes.size(); ++i) {
		    m_planes[i].vbo.model.set_color(i == m_hover_id ? GLGizmoBase::FLATTEN_HOVER_COLOR : GLGizmoBase::FLATTEN_COLOR);
            m_planes[i].vbo.model.render();
        }
    }

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

void GLGizmoFlatten::on_register_raycasters_for_picking()
{
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);

    assert(m_planes_casters.empty());

    if (!m_planes.empty()) {
        const Selection& selection = m_parent.get_selection();
        const Transform3d matrix = Geometry::translation_transform(selection.get_first_volume()->get_sla_shift_z() * Vec3d::UnitZ()) *
            selection.get_first_volume()->get_instance_transformation().get_matrix();

        for (int i = 0; i < (int)m_planes.size(); ++i) {
            m_planes_casters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, i, *m_planes[i].vbo.mesh_raycaster, matrix));
        }
    }
}

void GLGizmoFlatten::on_unregister_raycasters_for_picking()
{
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
    m_parent.set_raycaster_gizmos_on_top(false);
    m_planes_casters.clear();
}

void GLGizmoFlatten::set_flattening_data(const ModelObject* model_object, int instance_id)
{
    if (model_object != m_old_model_object || instance_id != m_old_instance_id) {
        m_planes.clear();
        if (get_state() == On) { // Only touch the raycasters if it's current
            on_unregister_raycasters_for_picking();
        }
    }
}

void GLGizmoFlatten::update_planes()
{
    const ModelObject* mo = m_c->selection_info()->model_object();
    TriangleMesh ch;
    for (const ModelVolume* vol : mo->volumes) {
        if (vol->type() != ModelVolumeType::MODEL_PART)
            continue;
        TriangleMesh vol_ch = vol->get_convex_hull();
        vol_ch.transform(vol->get_matrix());
        ch.merge(vol_ch);
    }
    ch = ch.convex_hull_3d();
    m_planes.clear();
    on_unregister_raycasters_for_picking();
    const Transform3d &inst_matrix = mo->instances.front()->get_matrix_no_offset();

    // Following constants are used for discarding too small polygons.
    const float minimal_area = 5.f; // in square mm (world coordinates)
    const float minimal_side = 1.f; // mm
    const float minimal_angle = 1.f; // degree, initial value was 10, but cause bugs

    // Now we'll go through all the facets and append Points of facets sharing the same normal.
    // This part is still performed in mesh coordinate system.
    const int                num_of_facets  = ch.facets_count();
    const std::vector<Vec3f> face_normals   = its_face_normals(ch.its);
    const std::vector<Vec3i32> face_neighbors = its_face_neighbors(ch.its);
    std::vector<int>         facet_queue(num_of_facets, 0);
    std::vector<bool>        facet_visited(num_of_facets, false);
    int                      facet_queue_cnt = 0;
    const stl_normal*        normal_ptr      = nullptr;
    int                      facet_idx       = 0;
    while (1) {
        // Find next unvisited triangle:
        for (; facet_idx < num_of_facets; ++ facet_idx)
            if (!facet_visited[facet_idx]) {
                facet_queue[facet_queue_cnt ++] = facet_idx;
                facet_visited[facet_idx] = true;
                normal_ptr = &face_normals[facet_idx];
                m_planes.emplace_back();
                break;
            }
        if (facet_idx == num_of_facets)
            break; // Everything was visited already

        while (facet_queue_cnt > 0) {
            int facet_idx = facet_queue[-- facet_queue_cnt];
            const stl_normal& this_normal = face_normals[facet_idx];
            if (std::abs(this_normal(0) - (*normal_ptr)(0)) < 0.001 && std::abs(this_normal(1) - (*normal_ptr)(1)) < 0.001 && std::abs(this_normal(2) - (*normal_ptr)(2)) < 0.001) {
                const Vec3i32 face = ch.its.indices[facet_idx];
                for (int j=0; j<3; ++j)
                    m_planes.back().vertices.emplace_back(ch.its.vertices[face[j]].cast<double>());

                facet_visited[facet_idx] = true;
                for (int j = 0; j < 3; ++ j)
                    if (int neighbor_idx = face_neighbors[facet_idx][j]; neighbor_idx >= 0 && ! facet_visited[neighbor_idx])
                        facet_queue[facet_queue_cnt ++] = neighbor_idx;
            }
        }
        m_planes.back().normal = normal_ptr->cast<double>();

        Pointf3s& verts = m_planes.back().vertices;
        // Now we'll transform all the points into world coordinates, so that the areas, angles and distances
        // make real sense.
        verts = transform(verts, inst_matrix);

        // if this is a just a very small triangle, remove it to speed up further calculations (it would be rejected later anyway):
        if (verts.size() == 3 &&
            ((verts[0] - verts[1]).norm() < minimal_side
            || (verts[0] - verts[2]).norm() < minimal_side
            || (verts[1] - verts[2]).norm() < minimal_side))
            m_planes.pop_back();
    }

    // Let's prepare transformation of the normal vector from mesh to instance coordinates.
    const Matrix3d normal_matrix = inst_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();

    // Now we'll go through all the polygons, transform the points into xy plane to process them:
    for (unsigned int polygon_id=0; polygon_id < m_planes.size(); ++polygon_id) {
        Pointf3s& polygon = m_planes[polygon_id].vertices;
        const Vec3d& normal = m_planes[polygon_id].normal;

        // transform the normal according to the instance matrix:
        const Vec3d normal_transformed = normal_matrix * normal;

        // We are going to rotate about z and y to flatten the plane
        Eigen::Quaterniond q;
        Transform3d m = Transform3d::Identity();
        m.matrix().block(0, 0, 3, 3) = q.setFromTwoVectors(normal_transformed, Vec3d::UnitZ()).toRotationMatrix();
        polygon = transform(polygon, m);

        // Now to remove the inner points. We'll misuse Geometry::convex_hull for that, but since
        // it works in fixed point representation, we will rescale the polygon to avoid overflows.
        // And yes, it is a nasty thing to do. Whoever has time is free to refactor.
        Vec3d bb_size = BoundingBoxf3(polygon).size();
        float sf = std::min(1./bb_size(0), 1./bb_size(1));
        Transform3d tr = Geometry::scale_transform({ sf, sf, 1.f });
        polygon = transform(polygon, tr);
        polygon = Slic3r::Geometry::convex_hull(polygon);
        polygon = transform(polygon, tr.inverse());

        // Calculate area of the polygons and discard ones that are too small
        float& area = m_planes[polygon_id].area;
        area = 0.f;
        for (unsigned int i = 0; i < polygon.size(); i++) // Shoelace formula
            area += polygon[i](0)*polygon[i + 1 < polygon.size() ? i + 1 : 0](1) - polygon[i + 1 < polygon.size() ? i + 1 : 0](0)*polygon[i](1);
        area = 0.5f * std::abs(area);

        bool discard = false;
        if (area < minimal_area)
            discard = true;
        else {
            // We also check the inner angles and discard polygons with angles smaller than the following threshold
            const double angle_threshold = ::cos(minimal_angle * (double)PI / 180.0);

            for (unsigned int i = 0; i < polygon.size(); ++i) {
                const Vec3d& prec = polygon[(i == 0) ? polygon.size() - 1 : i - 1];
                const Vec3d& curr = polygon[i];
                const Vec3d& next = polygon[(i == polygon.size() - 1) ? 0 : i + 1];

                if ((prec - curr).normalized().dot((next - curr).normalized()) > angle_threshold) {
                    discard = true;
                    break;
                }
            }
        }

        if (discard) {
            m_planes[polygon_id--] = std::move(m_planes.back());
            m_planes.pop_back();
            continue;
        }

        // We will shrink the polygon a little bit so it does not touch the object edges:
        Vec3d centroid = std::accumulate(polygon.begin(), polygon.end(), Vec3d(0.0, 0.0, 0.0));
        centroid /= (double)polygon.size();
        for (auto& vertex : polygon)
            vertex = 0.9f*vertex + 0.1f*centroid;

        // Polygon is now simple and convex, we'll round the corners to make them look nicer.
        // The algorithm takes a vertex, calculates middles of respective sides and moves the vertex
        // towards their average (controlled by 'aggressivity'). This is repeated k times.
        // In next iterations, the neighbours are not always taken at the middle (to increase the
        // rounding effect at the corners, where we need it most).
        const unsigned int k = 10; // number of iterations
        const float aggressivity = 0.2f;  // agressivity
        const unsigned int N = polygon.size();
        std::vector<std::pair<unsigned int, unsigned int>> neighbours;
        if (k != 0) {
            Pointf3s points_out(2*k*N); // vector long enough to store the future vertices
            for (unsigned int j=0; j<N; ++j) {
                points_out[j*2*k] = polygon[j];
                neighbours.push_back(std::make_pair((int)(j*2*k-k) < 0 ? (N-1)*2*k+k : j*2*k-k, j*2*k+k));
            }

            for (unsigned int i=0; i<k; ++i) {
                // Calculate middle of each edge so that neighbours points to something useful:
                for (unsigned int j=0; j<N; ++j)
                    if (i==0)
                        points_out[j*2*k+k] = 0.5f * (points_out[j*2*k] + points_out[j==N-1 ? 0 : (j+1)*2*k]);
                    else {
                        float r = 0.2+0.3/(k-1)*i; // the neighbours are not always taken in the middle
                        points_out[neighbours[j].first] = r*points_out[j*2*k] + (1-r) * points_out[neighbours[j].first-1];
                        points_out[neighbours[j].second] = r*points_out[j*2*k] + (1-r) * points_out[neighbours[j].second+1];
                    }
                // Now we have a triangle and valid neighbours, we can do an iteration:
                for (unsigned int j=0; j<N; ++j)
                    points_out[2*k*j] = (1-aggressivity) * points_out[2*k*j] +
                                        aggressivity*0.5f*(points_out[neighbours[j].first] + points_out[neighbours[j].second]);

                for (auto& n : neighbours) {
                    ++n.first;
                    --n.second;
                }
            }
            polygon = points_out; // replace the coarse polygon with the smooth one that we just created
        }


        // Raise a bit above the object surface to avoid flickering:
        for (auto& b : polygon)
            b(2) += 0.1f;

        // Transform back to 3D (and also back to mesh coordinates)
        polygon = transform(polygon, inst_matrix.inverse() * m.inverse());
    }

    // We'll sort the planes by area and only keep the 254 largest ones (because of the picking pass limitations):
    std::sort(m_planes.rbegin(), m_planes.rend(), [](const PlaneData& a, const PlaneData& b) { return a.area < b.area; });
    m_planes.resize(std::min((int)m_planes.size(), 254));

    // Planes are finished - let's save what we calculated it from:
    m_volumes_matrices.clear();
    m_volumes_types.clear();
    for (const ModelVolume* vol : mo->volumes) {
        m_volumes_matrices.push_back(vol->get_matrix());
        m_volumes_types.push_back(vol->type());
    }
    m_first_instance_scale = mo->instances.front()->get_scaling_factor();
    m_first_instance_mirror = mo->instances.front()->get_mirror();
    m_old_model_object = mo;
    m_old_instance_id = m_c->selection_info()->get_active_instance();

    // And finally create respective VBOs. The polygon is convex with
    // the vertices in order, so triangulation is trivial.
    for (auto& plane : m_planes) {
        indexed_triangle_set its;
        its.vertices.reserve(plane.vertices.size());
        its.indices.reserve(plane.vertices.size() / 3);
        for (size_t i = 0; i < plane.vertices.size(); ++i) {
            its.vertices.emplace_back((Vec3f)plane.vertices[i].cast<float>());
        }
        for (size_t i = 1; i < plane.vertices.size() - 1; ++i) {
            its.indices.emplace_back(0, i, i + 1); // triangle fan
        }

        plane.vbo.model.init_from(its);
        if (Geometry::Transformation(inst_matrix).is_left_handed()) {
            // we need to swap face normals in case the object is mirrored
            // for the raycaster to work properly
            for (stl_triangle_vertex_indices& face : its.indices) {
                if (its_face_normal(its, face).cast<double>().dot(plane.normal) < 0.0)
                    std::swap(face[1], face[2]);
            }
        }
        plane.vbo.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
        // vertices are no more needed, clear memory
        plane.vertices = std::vector<Vec3d>();
    }

    on_register_raycasters_for_picking();
}

bool GLGizmoFlatten::is_plane_update_necessary() const
{
    const ModelObject* mo = m_c->selection_info()->model_object();
    if (m_state != On || ! mo || mo->instances.empty())
        return false;

    if (m_planes.empty() || mo != m_old_model_object
        || mo->volumes.size() != m_volumes_matrices.size())
        return true;

    // We want to recalculate when the scale changes - some planes could (dis)appear.
    if (! mo->instances.front()->get_scaling_factor().isApprox(m_first_instance_scale)
     || ! mo->instances.front()->get_mirror().isApprox(m_first_instance_mirror))
        return true;

    for (unsigned int i=0; i < mo->volumes.size(); ++i)
        if (! mo->volumes[i]->get_matrix().isApprox(m_volumes_matrices[i])
         || mo->volumes[i]->type() != m_volumes_types[i])
            return true;

    return false;
}

} // namespace GUI
} // namespace Slic3r

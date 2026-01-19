#include "GLGizmoFaceDetector.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/SLA/IndexedMesh.hpp"
#include "libslic3r/FaceDetector.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <GL/glew.h>

#ifdef __WINDOWS__
#include <windows.h>
#include <stdio.h>
#endif

namespace Slic3r {

namespace GUI {

bool GLGizmoFaceDetector::on_init()
{
    return true;
}

std::string GLGizmoFaceDetector::on_get_name() const
{
    return (_L("Face recognition") + " [P]").ToUTF8().data();
}

void GLGizmoFaceDetector::on_render()
{
    if (model.is_initialized()) {
        model.set_color({0.f, 0.f, 1.f, 0.4f});
        model.render();
    }
}

void GLGizmoFaceDetector::on_render_input_window(float x, float y, float bottom_limit)
{
#if 0
    if (!m_c->selection_info() || !m_c->selection_info()->model_object())
        return;

    const float approx_height = m_imgui->scaled(14.0f);
    y = std::min(y, bottom_limit - approx_height);
    //BBS: GUI refactor: move gizmo to the right
#if BBS_TOOLBAR_ON_TOP
    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always, 0.5f, 0.0f);
#else
    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    ImGuiWrapper::push_toolbar_style();
    m_imgui->begin(on_get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    ImGui::PushItemWidth(m_imgui->get_style_scaling() * 150);
    ImGui::InputDouble("Sample interval", &m_sample_interval, 0.0f, 0.0f, "%.2f");

    bool btn_clicked = m_imgui->button(_L("Perform Recognition"));
    if (btn_clicked) {
        perform_recognition(m_parent.get_selection());
    }

    m_imgui->end();
    ImGuiWrapper::pop_toolbar_style();
#endif
}

void GLGizmoFaceDetector::on_set_state()
{
    if (get_state() == On) {
        model.reset();
        display_exterior_face();
    }
}

bool GLGizmoFaceDetector::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    return selection.is_single_full_instance() && !selection.is_wipe_tower();
}

void GLGizmoFaceDetector::perform_recognition(const Selection& selection)
{
    ModelObject* mo = m_c->selection_info()->model_object();
    //FaceDetector face_detector(mo, m_sample_interval);

    //face_detector.detect_exterior_face();
}

void GLGizmoFaceDetector::display_exterior_face()
{
    int cnt = 0;
    model.reset();

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3, GLModel::Geometry::EIndexType::UINT };

    const ModelObjectPtrs& objects = wxGetApp().model().objects;
    for (ModelObject* mo : objects) {
        const ModelInstance* mi = mo->instances[0];
        Transform3d inst_transfo = mi->get_matrix();
        for (ModelVolume* mv : mo->volumes) {
            TriangleMesh mesh_temp = mv->mesh();
            mesh_temp.transform(mv->get_matrix() * inst_transfo);
            indexed_triangle_set& mv_its = mesh_temp.its;
            for (int facet_idx = 0; facet_idx < mv_its.indices.size(); facet_idx++) {
                const stl_triangle_vertex_indices& facet_vert_idxs = mv_its.indices[facet_idx];
                if (mv_its.get_property(facet_idx).type != eExteriorAppearance)
                    continue;

                for (int i = 0; i < 3; ++i) {
                    init_data.add_vertex((Vec3f) mv_its.vertices[facet_vert_idxs[i]].cast<float>(), Vec3f{0.0f, 0.0f, 1.0f});
                }

                init_data.add_uint_triangle(cnt, cnt + 1, cnt + 2);
                cnt += 3;
            }
        }
    }
    model.init_from(std::move(init_data));
}

CommonGizmosDataID GLGizmoFaceDetector::on_get_requirements() const
{
    return CommonGizmosDataID::SelectionInfo;
}

} // namespace GUI
} // namespace Slic3r

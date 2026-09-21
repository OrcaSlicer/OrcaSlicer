#include "GLGizmoPrimitive.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "libslic3r/Model.hpp"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

namespace Slic3r {
namespace GUI {

GLGizmoPrimitive::GLGizmoPrimitive(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id) {}

bool GLGizmoPrimitive::on_init() { return true; }
std::string GLGizmoPrimitive::on_get_name() const { return _u8L("Primitive"); }
bool GLGizmoPrimitive::on_is_activable() const { return true; }
void GLGizmoPrimitive::on_render() {}
void GLGizmoPrimitive::on_set_state()
{ if (m_state == EState::On) { m_params = PrimitiveParams{}; m_preview_dirty = true; } }

bool GLGizmoPrimitive::on_mouse(const wxMouseEvent&) { return false; }

CommonGizmosDataID GLGizmoPrimitive::on_get_requirements() const
{ return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::InstancesHider)); }

void GLGizmoPrimitive::on_load(cereal::BinaryInputArchive& ar)
{ ar(m_params); m_preview_dirty = true; }
void GLGizmoPrimitive::on_save(cereal::BinaryOutputArchive& ar) const
{ ar(m_params); }

void GLGizmoPrimitive::apply_preset(const char*, double w, double h, double d)
{
    m_params.type = PrimitiveType::Box;
    m_params.box_w = w; m_params.box_h = h; m_params.box_d = d;
    m_preview_dirty = true;
}

static void gen_mesh_and_add(PrimitiveParams& p, const char* snap_name)
{
    TopoDS_Solid solid = GeometryEngine::make_primitive(p);
    TopoDS_Shape shape = solid;
    if (p.dressup_enabled) {
        if (p.dressup_type == DressUpType::Fillet)
            shape = GeometryEngine::apply_fillet(shape, p.dressup_radius, p.dressup_faces);
        else
            shape = GeometryEngine::apply_chamfer(shape, p.dressup_chamfer_dist, p.dressup_faces);
    }
    TriangleMesh mesh = GeometryEngine::tessellate(shape, p.linear_deflection, p.angular_deflection);
    if (mesh.its.indices.empty()) {
        wxGetApp().notification_manager()->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel, _u8L("Empty mesh generated"));
        return;
    }
    wxGetApp().plater()->take_snapshot(snap_name);
    ModelObject* mo = wxGetApp().model().add_object();
    std::string name = GeometryEngine::primitive_name(p.type);
    if (p.dressup_enabled && p.dressup_type == DressUpType::Fillet) name += " (Fillet)";
    else if (p.dressup_enabled) name += " (Chamfer)";
    mo->name = name;
    mo->add_volume(std::move(mesh))->set_new_unique_id();
    mo->ensure_on_bed();
    wxGetApp().plater()->update();
}

void GLGizmoPrimitive::apply_primitive() { gen_mesh_and_add(m_params, "Add Primitive"); }

void GLGizmoPrimitive::on_render_input_window(float x, float y, float bottom_limit)
{
    y = std::min(y, bottom_limit - ImGui::GetWindowHeight());
    const float scale = m_parent.get_scale();
    ImGuiWrapper::push_toolbar_style(scale);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin("Primitive", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove
                                  | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                                  | ImGuiWindowFlags_NoTitleBar);

    if (ImGui::CollapsingHeader("Shape", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* names[] = {"Box", "Cylinder", "Sphere", "Cone", "Torus"};
        int cur = (int)m_params.type;
        if (ImGui::Combo("##type", &cur, names, (int)PrimitiveType::COUNT)) {
            m_params.type = (PrimitiveType)cur;
            m_preview_dirty = true;
        }
        ImGui::Text("Quick:");
        ImGui::SameLine();
        if (ImGui::SmallButton("10mm")) apply_preset("10mm cube", 10, 10, 10);
        ImGui::SameLine();
        if (ImGui::SmallButton("20mm")) apply_preset("20mm cube", 20, 20, 20);
        ImGui::SameLine();
        if (ImGui::SmallButton("50mm")) apply_preset("50mm cube", 50, 50, 50);
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Dimensions", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto dim = [&](const char* label, double& val, double step=0.5, double fast=5.0) {
            ImGui::SetNextItemWidth(130);
            if (ImGui::InputDouble(label, &val, step, fast, "%.1f mm")) m_preview_dirty = true;
            if (val < 0.5) val = 0.5;
        };
        switch (m_params.type) {
        case PrimitiveType::Box:
            dim("Width (X)",  m_params.box_w);
            dim("Depth (Y)",  m_params.box_d);
            dim("Height (Z)", m_params.box_h);
            break;
        case PrimitiveType::Cylinder:
            dim("Radius", m_params.cyl_radius);
            dim("Height", m_params.cyl_height);
            break;
        case PrimitiveType::Sphere:
            dim("Radius", m_params.sph_radius);
            break;
        case PrimitiveType::Cone:
            dim("Bottom R", m_params.cone_r1);
            dim("Top R",    m_params.cone_r2);
            dim("Height",   m_params.cone_height);
            break;
        case PrimitiveType::Torus:
            dim("Major R", m_params.torus_r1);
            dim("Minor R", m_params.torus_r2, 0.1, 1.0);
            break;
        default: break;
        }
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Fillet / Chamfer")) {
        ImGui::Checkbox("Enable", &m_params.dressup_enabled);
        if (m_params.dressup_enabled) {
            static const char* dn[] = {"Fillet", "Chamfer"};
            int du = (int)m_params.dressup_type;
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo("##dtype", &du, dn, 2)) { m_params.dressup_type = (DressUpType)du; m_preview_dirty = true; }
            static const char* fn[] = {"All edges", "Top edges", "Bottom edges", "Lateral edges"};
            int fg = (int)m_params.dressup_faces;
            ImGui::SetNextItemWidth(140);
            if (ImGui::Combo("Edges", &fg, fn, 4)) { m_params.dressup_faces = (FaceGroup)fg; m_preview_dirty = true; }
            if (m_params.dressup_type == DressUpType::Fillet) {
                ImGui::SetNextItemWidth(100);
                if (ImGui::InputDouble("Radius", &m_params.dressup_radius, 0.1, 1.0, "%.1f mm")) {
                    if (m_params.dressup_radius < 0.1) m_params.dressup_radius = 0.1;
                    m_preview_dirty = true;
                }
            } else {
                ImGui::SetNextItemWidth(100);
                if (ImGui::InputDouble("Distance", &m_params.dressup_chamfer_dist, 0.1, 1.0, "%.1f mm")) {
                    if (m_params.dressup_chamfer_dist < 0.1) m_params.dressup_chamfer_dist = 0.1;
                    m_preview_dirty = true;
                }
            }
        }
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Quality")) {
        ImGui::SetNextItemWidth(130);
        if (ImGui::InputDouble("Mesh resolution", &m_params.linear_deflection, 0.001, 0.1, "%.3f mm")) {
            if (m_params.linear_deflection < 0.001) m_params.linear_deflection = 0.001;
            if (m_params.linear_deflection > 1.0)   m_params.linear_deflection = 1.0;
            m_preview_dirty = true;
        }
    }

    ImGui::Separator();

    if (ImGui::Button("Add Shape", {-1, 28}))
        apply_primitive();

    if (ImGui::Button("Close", {-1, 0}))
        m_parent.reset_all_gizmos();

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace GUI
} // namespace Slic3r

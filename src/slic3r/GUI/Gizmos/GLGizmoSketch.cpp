#include "GLGizmoSketch.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "libslic3r/Model.hpp"
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepAlgoAPI_Fuse.hxx>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

#define L(s) Slic3r::GUI::I18N::translate((s)).c_str()
#define UL(s) Slic3r::GUI::I18N::translate_utf8((s)).c_str()

namespace Slic3r {
namespace GUI {

GLGizmoSketch::GLGizmoSketch(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id) {}

bool GLGizmoSketch::on_init() { return true; }
std::string GLGizmoSketch::on_get_name() const { return _u8L("Sketch"); }
bool GLGizmoSketch::on_is_activable() const { return true; }
void GLGizmoSketch::on_render() {}
void GLGizmoSketch::on_set_state() { if (m_state == EState::On) clear_all(); }
bool GLGizmoSketch::on_mouse(const wxMouseEvent&) { return false; }

CommonGizmosDataID GLGizmoSketch::on_get_requirements() const
{ return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo)); }

void GLGizmoSketch::on_load(cereal::BinaryInputArchive& ar)
{
    ar(m_tool, m_profiles, m_plane, m_sp, m_rect_w, m_rect_h, m_circle_r, m_poly_sides, m_poly_r, m_snap_grid, m_grid_step);
    m_active_profile = -1;
}

void GLGizmoSketch::on_save(cereal::BinaryOutputArchive& ar) const
{
    ar(m_tool, m_profiles, m_plane, m_sp, m_rect_w, m_rect_h, m_circle_r, m_poly_sides, m_poly_r, m_snap_grid, m_grid_step);
}

SketchProfile& GLGizmoSketch::active_profile()
{
    if (m_active_profile < 0 || m_active_profile >= (int)m_profiles.size()) {
        m_profiles.emplace_back();
        m_active_profile = (int)m_profiles.size() - 1;
    }
    return m_profiles[m_active_profile];
}

bool GLGizmoSketch::has_closed_profile() const
{
    for (auto& p : m_profiles) if (p.closed && p.points.size() >= 3) return true;
    return false;
}

void GLGizmoSketch::clear_all()
{
    m_profiles.clear();
    m_canvas_points.clear();
    m_active_profile = -1;
}

void GLGizmoSketch::add_closed_profile()
{
    auto& ap = active_profile();
    if (ap.points.size() >= 3) {
        ap.closed = true;
        m_active_profile = -1;
    }
}

void GLGizmoSketch::delete_profile(int idx)
{
    if (idx >= 0 && idx < (int)m_profiles.size()) {
        m_profiles.erase(m_profiles.begin() + idx);
        if (m_active_profile >= (int)m_profiles.size()) m_active_profile = -1;
    }
}

Vec2d GLGizmoSketch::snap(Vec2d pt) const
{
    if (!m_snap_grid) return pt;
    double gs = m_grid_step;
    return {round(pt.x() / gs) * gs, round(pt.y() / gs) * gs};
}

void GLGizmoSketch::build_preset_profile()
{
    auto& ap = active_profile();
    ap.clear();
    auto add = [&](double x, double y) { ap.points.emplace_back(x, y); };
    switch (m_tool) {
    case SketchTool::Rectangle:
        add(-m_rect_w/2, -m_rect_h/2); add( m_rect_w/2, -m_rect_h/2);
        add( m_rect_w/2,  m_rect_h/2); add(-m_rect_w/2,  m_rect_h/2);
        ap.closed = true; m_active_profile = -1; break;
    case SketchTool::Circle:
        for (int i = 0; i <= m_circle_seg; ++i) {
            double a = 2.0*M_PI*i/m_circle_seg;
            add(cos(a)*m_circle_r, sin(a)*m_circle_r);
        }
        ap.closed = true; m_active_profile = -1; break;
    case SketchTool::Polygon:
        for (int i = 0; i < m_poly_sides; ++i) {
            double a = 2.0*M_PI*i/m_poly_sides - M_PI/2;
            add(cos(a)*m_poly_r, sin(a)*m_poly_r);
        }
        ap.closed = true; m_active_profile = -1; break;
    default: break;
    }
}

void GLGizmoSketch::handle_canvas_click(ImVec2 pos)
{
    Vec2d pt = snap({pos.x / m_canvas_scale, -pos.y / m_canvas_scale});
    if (m_tool == SketchTool::Line) {
        auto& ap = active_profile();
        if (ap.points.size() >= 3 && (pt - ap.points.front()).norm() < m_grid_step) {
            ap.points.push_back(ap.points.front());
            ap.closed = true;
            m_active_profile = -1;
            return;
        }
        ap.points.push_back(pt);
    }
}

void GLGizmoSketch::draw_canvas()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = 280, h = 200;
    ImVec2 end(pos.x+w, pos.y+h);
    float cx = pos.x+w/2, cy = pos.y+h/2;
    auto tc = [&](const ImVec2& p) { return ImVec2(cx+p.x*m_canvas_scale, cy-p.y*m_canvas_scale); };

    dl->AddRectFilled(pos, end, IM_COL32(28,28,36,255));
    dl->AddRect(pos, end, IM_COL32(55,55,68,255));

    float gs = m_grid_step;
    for (float g = 0; g < w; g += gs * m_canvas_scale) {
        ImU32 gc = (int(g/(gs*m_canvas_scale)) % 5 == 0) ? IM_COL32(60,60,75,100) : IM_COL32(45,45,55,60);
        dl->AddLine({pos.x+g,pos.y}, {pos.x+g,end.y}, gc);
    }
    for (float g = 0; g < h; g += gs * m_canvas_scale) {
        ImU32 gc = (int(g/(gs*m_canvas_scale)) % 5 == 0) ? IM_COL32(60,60,75,100) : IM_COL32(45,45,55,60);
        dl->AddLine({pos.x,pos.y+g}, {end.x,pos.y+g}, gc);
    }

    dl->AddLine({cx,pos.y},{cx,end.y}, IM_COL32(70,70,85,180), 1.5f);
    dl->AddLine({pos.x,cy},{end.x,cy}, IM_COL32(70,70,85,180), 1.5f);
    dl->AddText({end.x-12, cy+2}, IM_COL32(120,120,140,200), "X");
    dl->AddText({cx+4, pos.y+2}, IM_COL32(120,120,140,200), "Y");

    for (size_t pi = 0; pi < m_profiles.size(); ++pi) {
        auto& prof = m_profiles[pi];
        if (prof.points.size() < 2) continue;
        std::vector<ImVec2> sp;
        for (auto& p : prof.points) sp.push_back(tc({(float)p.x(), (float)p.y()}));
        if (prof.closed && sp.size() >= 3) {
            bool is_outer = (pi == 0);
            ImU32 fill = is_outer ? IM_COL32(0,180,90,35) : IM_COL32(180,60,60,35);
            ImU32 line = is_outer ? IM_COL32(0,220,100,255) : IM_COL32(220,80,80,255);
            dl->AddConvexPolyFilled(sp.data(), (int)sp.size(), fill);
            for (size_t i=0; i<sp.size(); ++i)
                dl->AddLine(sp[i], sp[(i+1)%sp.size()], line, (pi==0)?2.5f:2.0f);
            for (size_t i=0; i<sp.size()-1; ++i)
                dl->AddCircleFilled(sp[i], 3.0f, IM_COL32(255,255,255,255));
        }
    }

    auto& ap = active_profile();
    if (!ap.closed && ap.points.size() >= 1) {
        std::vector<ImVec2> sp;
        for (auto& p : ap.points) sp.push_back(tc({(float)p.x(), (float)p.y()}));
        for (size_t i=1; i<sp.size(); ++i)
            dl->AddLine(sp[i-1], sp[i], IM_COL32(0,200,255,200), 2.0f);
        for (auto& s : sp) dl->AddCircleFilled(s, 3.5f, IM_COL32(100,200,255,255));
        ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.x > pos.x && mouse.x < end.x && mouse.y > pos.y && mouse.y < end.y)
            dl->AddLine(sp.back(), mouse, IM_COL32(100,160,220,120), 1.5f);
    }

    ImGui::InvisibleButton("canvas", ImVec2(w,h));
    if (ImGui::IsItemHovered()) {
        ImVec2 m = ImGui::GetMousePos();
        Vec2d sk({(m.x-cx)/m_canvas_scale, -(m.y-cy)/m_canvas_scale});
        if (m_snap_grid) sk = snap(sk);
        auto txt = wxString::Format("X:%.1f Y:%.1f", sk.x(), sk.y()).ToStdString();
        dl->AddText({pos.x+4, end.y-16}, IM_COL32(160,160,180,200), txt.c_str());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            handle_canvas_click({(m.x-cx)/m_canvas_scale, -(m.y-cy)/m_canvas_scale});
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            auto& ap2 = active_profile();
            if (ap2.points.size() >= 3) {
                ap2.points.push_back(ap2.points.front());
                ap2.closed = true;
                m_active_profile = -1;
            }
        }
    }
}

TopoDS_Shape GLGizmoSketch::build_combined_shape()
{
    if (m_profiles.empty() || !m_profiles[0].closed)
        throw std::runtime_error("No outer profile");

    TopoDS_Wire outer_wire = m_profiles[0].to_occt_wire(m_plane);
    BRepBuilderAPI_MakeFace face_maker(outer_wire);
    if (!face_maker.IsDone()) throw std::runtime_error("Failed to make outer face");

    for (size_t i = 1; i < m_profiles.size(); ++i) {
        if (!m_profiles[i].closed) continue;
        TopoDS_Wire inner = m_profiles[i].to_occt_wire(m_plane);
        face_maker.Add(inner);
    }
    face_maker.Build();
    if (!face_maker.IsDone()) throw std::runtime_error("Failed to build face with holes");

    TopoDS_Face face = face_maker.Face();

    TopoDS_Shape shape;
    if (m_sp.revolve_deg < 360.0 && m_sp.revolve_deg > 0.0) {
        gp_Pnt o(m_plane.origin.x(), m_plane.origin.y(), m_plane.origin.z());
        gp_Dir xd(m_plane.x_axis.x(), m_plane.x_axis.y(), m_plane.x_axis.z());
        gp_Ax1 axis(o, xd);
        BRepPrimAPI_MakeRevol rev(face, axis, m_sp.revolve_deg * M_PI / 180.0);
        if (!rev.IsDone()) throw std::runtime_error("Revolve failed");
        shape = rev.Shape();
    } else {
        shape = SketchEngine::make_extrude_face(face, m_plane, m_sp.extrude_len, m_sp.extrude_sym);
    }

    if (m_sp.dressup_enabled) {
        if (m_sp.dressup_type == DressUpType::Fillet)
            shape = GeometryEngine::apply_fillet(shape, m_sp.dressup_radius, m_sp.dressup_faces);
        else
            shape = GeometryEngine::apply_chamfer(shape, m_sp.dressup_chamfer_dist, m_sp.dressup_faces);
    }
    return shape;
}

void GLGizmoSketch::on_render_input_window(float x, float y, float bottom_limit)
{
    y = std::min(y, bottom_limit - ImGui::GetWindowHeight());
    const float scale = m_parent.get_scale();
    ImGuiWrapper::push_toolbar_style(scale);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin("Sketch", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                               | ImGuiWindowFlags_NoTitleBar);

    if (ImGui::CollapsingHeader(UL("Profile"), ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* names[] = {"Line", "Rectangle", "Circle", "Polygon"};
        int cur = (int)m_tool;
        if (ImGui::Combo("##shape", &cur, names, (int)SketchTool::COUNT)) {
            m_tool = (SketchTool)cur;
            if (m_tool != SketchTool::Line) build_preset_profile();
        }
        ImGui::SameLine();
        if (m_imgui->button("+##newprofile")) m_active_profile = -1;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", UL("Start new profile (for holes)"));

        if (m_tool == SketchTool::Rectangle) {
            ImGui::SetNextItemWidth(80); if (ImGui::InputDouble("W", &m_rect_w,1,10,"%.0f")) build_preset_profile();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80); if (ImGui::InputDouble("H", &m_rect_h,1,10,"%.0f")) build_preset_profile();
        } else if (m_tool == SketchTool::Circle) {
            ImGui::SetNextItemWidth(80); if (ImGui::InputDouble("R", &m_circle_r,1,5,"%.0f")) build_preset_profile();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80); if (ImGui::SliderInt("Seg", &m_circle_seg,8,64)) build_preset_profile();
        } else if (m_tool == SketchTool::Polygon) {
            ImGui::SetNextItemWidth(80); if (ImGui::SliderInt("Sides", &m_poly_sides,3,12)) build_preset_profile();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80); if (ImGui::InputDouble("R", &m_poly_r,1,5,"%.0f")) build_preset_profile();
        } else {
            ImGui::Text("%s", UL("Click on canvas to draw"));
        }

        ImGui::Checkbox(UL("Snap to grid"), &m_snap_grid);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80); ImGui::InputFloat("Step", &m_grid_step, 1, 5, "%.0f mm");

        draw_canvas();

        if (!m_profiles.empty()) {
            ImGui::Text("%s: %zu", UL("Profiles"), m_profiles.size());
            for (int i = 0; i < (int)m_profiles.size(); ++i) {
                auto& p = m_profiles[i];
                ImGui::PushID(i);
                bool outer = (i == 0);
                ImVec4 col = outer ? ImVec4(0,1,0,1) : ImVec4(1,0.3f,0.3f,1);
                const char* label = outer ? "Outer" : "Hole";
                ImGui::TextColored(col, "%s %d: %zu pts %s", label, i+1, p.points.size(), p.closed ? "CLOSED" : "");
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) delete_profile(i);
                ImGui::PopID();
            }
        }
    }

    ImGui::Separator();

    bool is_revolve = false;
    bool has_sel = false;

    if (ImGui::CollapsingHeader(UL("Operation"), ImGuiTreeNodeFlags_DefaultOpen)) {
        static int pi = 0;
        if (ImGui::Combo(UL("Plane"), &pi, "XY (Top)\0XZ (Front)\0YZ (Side)\0"))
            m_plane = (pi==0) ? SketchPlane::XY() : (pi==1) ? SketchPlane::XZ() : SketchPlane::YZ();

        is_revolve = (m_sp.revolve_deg > 0 && m_sp.revolve_deg < 360);
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputDouble(UL("Revolve deg"), &m_sp.revolve_deg, 15, 90, "%.0f")) {
            if (m_sp.revolve_deg > 360) m_sp.revolve_deg = 360;
            if (m_sp.revolve_deg < 0) m_sp.revolve_deg = 0;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", UL("Set to 0 for extrude, >0 for revolve"));

        if (!is_revolve) {
            ImGui::SetNextItemWidth(100);
            ImGui::InputDouble(UL("Length"), &m_sp.extrude_len, 0.5, 5, "%.1f mm");
            ImGui::SameLine();
            ImGui::Checkbox(UL("Symmetric"), &m_sp.extrude_sym);
        }

        has_sel = !m_parent.get_selection().is_empty();
        if (has_sel) {
            if (ImGui::Checkbox(UL("Pocket (cut)"), &m_sp.is_pocket))
                if (m_sp.is_pocket) m_sp.dressup_enabled = false;
        } else m_sp.is_pocket = false;
    }

    ImGui::Separator();

    if (!m_sp.is_pocket && ImGui::CollapsingHeader(UL("Fillet / Chamfer"))) {
        ImGui::Checkbox(UL("Enable"), &m_sp.dressup_enabled);
        if (m_sp.dressup_enabled) {
            static const char* dn[] = {"Fillet", "Chamfer"};
            int du = (int)m_sp.dressup_type;
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo("##dtype", &du, dn, 2)) m_sp.dressup_type = (DressUpType)du;
            static const char* fn[] = {"All edges", "Top edges", "Bottom edges", "Lateral edges"};
            int fg = (int)m_sp.dressup_faces;
            ImGui::SetNextItemWidth(140);
            ImGui::Combo(UL("Edges"), &fg, fn, 4); m_sp.dressup_faces = (FaceGroup)fg;
            ImGui::SetNextItemWidth(100);
            if (m_sp.dressup_type == DressUpType::Fillet)
                ImGui::InputDouble(UL("Radius"), &m_sp.dressup_radius, 0.1, 1, "%.1f mm");
            else
                ImGui::InputDouble(UL("Distance"), &m_sp.dressup_chamfer_dist, 0.1, 1, "%.1f mm");
        }
    }

    ImGui::Separator();

    bool ok = has_closed_profile();
    if (ok) ImGui::TextColored({0,1,0,1}, "%zu %s", m_profiles.size(), UL("closed profile(s)"));
    else ImGui::TextColored({0.6f,0.6f,0.6f,1}, "%s", UL("Draw a closed profile to enable"));

    auto btn = [&](const char* label, bool enabled) {
        if (!enabled) { ImGui::PushItemFlag(ImGuiItemFlags_Disabled,true); ImGui::PushStyleColor(ImGuiCol_Button,{0.25f,0.25f,0.25f,1}); }
        bool clicked = ImGui::Button(label, {-1,0});
        if (!enabled) { ImGui::PopStyleColor(); ImGui::PopItemFlag(); }
        return clicked && enabled;
    };

    if (m_sp.is_pocket && has_sel) {
        if (btn(L("Pocket (Cut)"), ok)) apply_pocket();
    } else if (is_revolve) {
        if (btn(L("Revolve"), ok)) apply_revolve();
    } else {
        if (btn(L("Extrude"), ok)) apply_extrude();
    }

    if (ImGui::Button(L("Clear All"), {-1,0})) clear_all();
    if (ImGui::Button(L("Close"), {-1,0})) m_parent.reset_all_gizmos();

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoSketch::apply_extrude()
{
    try {
        TopoDS_Shape shape = build_combined_shape();
        TriangleMesh mesh = SketchEngine::tessellate(shape, m_sp.linear_deflection);
        if (mesh.its.indices.empty()) throw std::runtime_error("Empty result");
        wxGetApp().plater()->take_snapshot("Sketch Extrude");
        ModelObject* mo = wxGetApp().model().add_object();
        mo->name = "Extrusion";
        mo->add_volume(std::move(mesh))->set_new_unique_id();
        mo->ensure_on_bed();
        wxGetApp().plater()->update();
        clear_all();
    } catch (const std::exception& e) {
        wxGetApp().notification_manager()->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::ErrorNotificationLevel, std::string("Extrude: ")+e.what());
    }
}

void GLGizmoSketch::apply_revolve()
{
    try {
        TopoDS_Shape shape = build_combined_shape();
        TriangleMesh mesh = SketchEngine::tessellate(shape, m_sp.linear_deflection);
        if (mesh.its.indices.empty()) throw std::runtime_error("Empty result");
        wxGetApp().plater()->take_snapshot("Sketch Revolve");
        ModelObject* mo = wxGetApp().model().add_object();
        mo->name = "Revolve";
        mo->add_volume(std::move(mesh))->set_new_unique_id();
        mo->ensure_on_bed();
        wxGetApp().plater()->update();
        clear_all();
    } catch (const std::exception& e) {
        wxGetApp().notification_manager()->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::ErrorNotificationLevel, std::string("Revolve: ")+e.what());
    }
}

void GLGizmoSketch::apply_pocket()
{
    try {
        Selection& sel = m_parent.get_selection();
        int obj_idx = sel.get_object_idx();
        if (obj_idx < 0) throw std::runtime_error("No object selected");
        ModelObject* mo = wxGetApp().model().objects[obj_idx];

        TopoDS_Wire outer = m_profiles[0].to_occt_wire(m_plane);
        BRepBuilderAPI_MakeFace fm(outer);
        if (!fm.IsDone()) throw std::runtime_error("Face failed");
        for (size_t i = 1; i < m_profiles.size(); ++i)
            if (m_profiles[i].closed) fm.Add(m_profiles[i].to_occt_wire(m_plane));
        fm.Build();
        if (!fm.IsDone()) throw std::runtime_error("Face with holes failed");

        TopoDS_Shape tool = SketchEngine::make_extrude_face(fm.Face(), m_plane, m_sp.extrude_len + 5.0, false);
        TriangleMesh tool_mesh = SketchEngine::tessellate(tool, m_sp.linear_deflection);
        if (tool_mesh.its.indices.empty()) throw std::runtime_error("Tool mesh empty");

        wxGetApp().plater()->take_snapshot("Sketch Pocket");
        mo->add_volume(std::move(tool_mesh), ModelVolumeType::NEGATIVE_VOLUME)->set_new_unique_id();
        mo->ensure_on_bed();
        wxGetApp().plater()->update();
        clear_all();
        wxGetApp().notification_manager()->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel, UL("Pocket added (negative volume)"));
    } catch (const std::exception& e) {
        wxGetApp().notification_manager()->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::ErrorNotificationLevel, std::string("Pocket: ")+e.what());
    }
}

} // namespace GUI
} // namespace Slic3r

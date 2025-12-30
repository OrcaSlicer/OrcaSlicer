// Include GLGizmoBase.hpp before I18N.hpp as it includes some libigl code, which overrides our localization "L" macro.
#include "GLGizmoText.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Model.hpp"

#include "libslic3r/Shape/TextShape.hpp"

#include <numeric>

#include <boost/log/trivial.hpp>

#include <GL/glew.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>
#include "libslic3r/SVG.hpp"
#include <codecvt>
#include "wx/fontenum.h"

namespace Slic3r {
namespace GUI {

static const double PI = 3.141592653589793238;
static const wxColour FONT_TEXTURE_BG = wxColour(0, 0, 0, 0);
static const wxColour FONT_TEXTURE_FG = *wxWHITE;
static const int FONT_SIZE = 12;
static const float SELECTABLE_INNER_OFFSET = 8.0f;

static std::vector<std::string> font_black_list = {
#ifdef _WIN32
    "MT Extra",
    "Marlett",
    "Symbol",
    "Webdings",
    "Wingdings",
    "Wingdings 2",
    "Wingdings 3",
#endif
};

static const wxFontEncoding font_encoding = wxFontEncoding::wxFONTENCODING_SYSTEM;

#ifdef _WIN32
static bool load_hfont(void *hfont, DWORD &dwTable, DWORD &dwOffset, size_t &size, HDC hdc = nullptr)
{
    bool del_hdc = false;
    if (hdc == nullptr) {
        del_hdc = true;
        hdc     = ::CreateCompatibleDC(NULL);
        if (hdc == NULL) return false;
    }

    // To retrieve the data from the beginning of the file for TrueType
    // Collection files specify 'ttcf' (0x66637474).
    dwTable  = 0x66637474;
    dwOffset = 0;

    ::SelectObject(hdc, hfont);
    size = ::GetFontData(hdc, dwTable, dwOffset, NULL, 0);
    if (size == GDI_ERROR) {
        // HFONT is NOT TTC(collection)
        dwTable = 0;
        size    = ::GetFontData(hdc, dwTable, dwOffset, NULL, 0);
    }

    if (size == 0 || size == GDI_ERROR) {
        if (del_hdc) ::DeleteDC(hdc);
        return false;
    }
    return true;
}
#endif // _WIN32

bool can_load(const wxFont &font)
{
#ifdef _WIN32
    DWORD  dwTable = 0, dwOffset = 0;
    size_t size = 0;
    void* hfont = font.GetHFONT();
    if (!load_hfont(hfont, dwTable, dwOffset, size))
        return false;
    return hfont != nullptr;
#elif defined(__APPLE__)
    return true;
#elif defined(__linux__)
    return true;
#endif
    return false;
}

std::vector<std::string> init_face_names()
{
    std::vector<std::string> valid_font_names;
    wxArrayString            facenames = wxFontEnumerator::GetFacenames(font_encoding);
    std::vector<wxString>    bad_fonts;

    // validation lambda
    auto is_valid_font = [coding = font_encoding, bad = bad_fonts](const wxString &name) {
        if (name.empty())
            return false;

        // vertical font start with @, we will filter it out
        // Not sure if it is only in Windows so filtering is on all platforms
        if (name[0] == '@')
            return false;

        // previously detected bad font
        auto it = std::lower_bound(bad.begin(), bad.end(), name);
        if (it != bad.end() && *it == name)
            return false;

        wxFont wx_font(wxFontInfo().FaceName(name).Encoding(coding));
        // Faster chech if wx_font is loadable but not 100%
        // names could contain not loadable font
        if (!wx_font.IsOk())
            return false;

        if (!can_load(wx_font))
            return false;

        return true;
    };

    std::sort(facenames.begin(), facenames.end());
    for (const wxString &name : facenames) {
        if (is_valid_font(name)) {
            valid_font_names.push_back(name.ToStdString());
        }
        else {
            bad_fonts.emplace_back(name);
        }
    }
    assert(std::is_sorted(bad_fonts.begin(), bad_fonts.end()));

    for (auto iter = font_black_list.begin(); iter != font_black_list.end(); ++iter) {
        valid_font_names.erase(std::remove(valid_font_names.begin(), valid_font_names.end(), *iter), valid_font_names.end());
    }

    return valid_font_names;
}

class Line_3D
{
public:
    Line_3D(Vec3d i_a, Vec3d i_b) : a(i_a), b(i_b) {}

    double length() { return (b - a).cast<double>().norm(); }

    Vec3d vector()
    {
        Vec3d new_vec = b - a;
        new_vec.normalize();
        return new_vec;
    }

    void reverse() { std::swap(this->a, this->b); }

    Vec3d a;
    Vec3d b;
};

class Polygon_3D
{
public:
    Polygon_3D(const std::vector<Vec3d> &points) : m_points(points) {}

    std::vector<Line_3D> get_lines()
    {
        std::vector<Line_3D> lines;
        lines.reserve(m_points.size());
        if (m_points.size() > 2) {
            for (int i = 0; i < m_points.size() - 1; ++i) { lines.push_back(Line_3D(m_points[i], m_points[i + 1])); }
            lines.push_back(Line_3D(m_points.back(), m_points.front()));
        }
        return lines;
    }
    std::vector<Vec3d> m_points;
};

// for debug
void export_regions_to_svg(const Point &point, const Polygons &polylines)
{
    std::string path = "D:/svg_profiles/text_poly.svg";
    //BoundingBox bbox = get_extents(polylines);
    SVG svg(path.c_str());
    svg.draw(polylines, "green");
    svg.draw(point, "red", 5e6);
}

int preNUm(unsigned char byte)
{
    unsigned char mask = 0x80;
    int           num  = 0;
    for (int i = 0; i < 8; i++) {
        if ((byte & mask) == mask) {
            mask = mask >> 1;
            num++;
        } else {
            break;
        }
    }
    return num;
}

// https://www.jianshu.com/p/a83d398e3606
bool get_utf8_sub_strings(char *data, int len, std::vector<std::string> &out_strs)
{
    out_strs.clear();
    std::string str = std::string(data);

    int num = 0;
    int i   = 0;
    while (i < len) {
        if ((data[i] & 0x80) == 0x00) {
            out_strs.emplace_back(str.substr(i, 1));
            i++;
            continue;
        } else if ((num = preNUm(data[i])) > 2) {
            int start = i;
            i++;
            for (int j = 0; j < num - 1; j++) {
                if ((data[i] & 0xc0) != 0x80) { return false; }
                i++;
            }
            out_strs.emplace_back(str.substr(start, i - start));
        } else {
            return false;
        }
    }
    return true;
}

///////////////////////
/// GLGizmoText start
GLGizmoText::GLGizmoText(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
}

GLGizmoText::~GLGizmoText()
{
    for (int i = 0; i < m_textures.size(); i++) {
        if (m_textures[i].texture != nullptr)
            delete m_textures[i].texture;
    }
}

bool GLGizmoText::on_init()
{
    m_avail_font_names = init_face_names();

    //m_avail_font_names = init_occt_fonts();
    update_font_texture();
    m_scale = m_imgui->get_font_size();
    m_shortcut_key = WXK_CONTROL_T;

    m_grabbers.push_back(Grabber());

    reset_text_info();

    m_desc["font"]          = _L("Font");
    m_desc["size"]          = _L("Size");
    m_desc["thickness"]     = _L("Thickness");
    m_desc["text_gap"]      = _L("Text Gap");
    m_desc["angle"]         = _L("Angle");
    m_desc["embeded_depth"] = _L("Embedded\ndepth");
    m_desc["input_text"]    = _L("Input text");

    m_desc["surface"]         = _L("Surface");
    m_desc["horizontal_text"] = _L("Horizontal text");

    m_desc["rotate_text_caption"] = _L("Shift+") + _L("Mouse move up or down");
    m_desc["rotate_text"]         = _L("Rotate text");

    return true;
}

void GLGizmoText::update_font_texture()
{
    m_font_names.clear();
    for (int i = 0; i < m_textures.size(); i++) {
        if (m_textures[i].texture != nullptr)
            delete m_textures[i].texture;
    }
    m_combo_width = 0.0f;
    m_combo_height = 0.0f;
    m_textures.clear();
    m_textures.reserve(m_avail_font_names.size());
    for (int i = 0; i < m_avail_font_names.size(); i++)
    {
        GLTexture* texture = new GLTexture();
        auto face = wxString::FromUTF8(m_avail_font_names[i]);
        auto retina_scale = m_parent.get_scale();
        wxFont font { (int)round(retina_scale * FONT_SIZE), wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, face };
        int w, h, hl;
        if (texture->generate_texture_from_text(m_avail_font_names[i], font, w, h, hl, FONT_TEXTURE_BG, FONT_TEXTURE_FG)) {
            //if (h < m_imgui->scaled(2.f)) {
                TextureInfo info;
                info.texture = texture;
                info.w = w;
                info.h = h;
                info.hl = hl;
                info.font_name = m_avail_font_names[i];
                m_textures.push_back(info);
                m_combo_width = std::max(m_combo_width, static_cast<float>(texture->m_original_width));
                m_font_names.push_back(info.font_name);
            //}
        }
    }
    m_combo_height = m_imgui->scaled(32.f / 15.f);
}

bool GLGizmoText::is_mesh_point_clipped(const Vec3d &point, const Transform3d &trafo) const
{
    if (m_c->object_clipper()->get_position() == 0.)
        return false;

    auto  sel_info          = m_c->selection_info();
    Vec3d transformed_point = trafo * point;
    transformed_point(2) += sel_info->get_sla_shift();
    return m_c->object_clipper()->get_clipping_plane()->is_point_clipped(transformed_point);
}

BoundingBoxf3 GLGizmoText::bounding_box() const
{
    BoundingBoxf3                 ret;
    const Selection &             selection = m_parent.get_selection();
    const Selection::IndicesList &idxs      = selection.get_volume_idxs();
    for (unsigned int i : idxs) {
        const GLVolume *volume = selection.get_volume(i);
        if (!volume->is_modifier) ret.merge(volume->transformed_convex_hull_bounding_box());
    }
    return ret;
}

bool GLGizmoText::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    std::string text = std::string(m_text);
    if (text.empty())
        return true;

    const ModelObject *  mo        = m_c->selection_info()->model_object();
    if (m_is_modify) {
        const Selection &selection = m_parent.get_selection();
        mo                         = selection.get_model()->objects[m_object_idx];
    }
    if (mo == nullptr)
        return true;

    const Selection &    selection = m_parent.get_selection();
    const ModelInstance *mi        = mo->instances[selection.get_instance_idx()];
    const Camera &       camera    = wxGetApp().plater()->get_camera();

    if (action == SLAGizmoEventType::Moving) {
        if (shift_down && !alt_down && !control_down) {
            float angle = m_rotate_angle + 0.5 * (m_mouse_position - mouse_position).y();
            if (angle == 0)
                return true;

            while (angle < 0)
                angle += 360;

            while (angle >= 360)
                angle -= 360;

            m_rotate_angle = angle;
            m_shift_down   = true;
            m_need_update_text = true;
        } else {
            m_shift_down     = false;
            m_origin_mouse_position = mouse_position;
        }
        m_mouse_position = mouse_position;
    }
    else if (action == SLAGizmoEventType::LeftDown) {
        if (m_is_modify)
            return false;

        Plater *plater = wxGetApp().plater();
        if (!plater || m_thickness <= 0)
            return true;

        ModelObject *model_object = selection.get_model()->objects[m_object_idx];
        if (m_preview_text_volume_id > 0) {
            model_object->delete_volume(m_preview_text_volume_id);
            plater->update();
            m_preview_text_volume_id = -1;
        }

        // Precalculate transformations of individual meshes.
        std::vector<Transform3d> trafo_matrices;
        for (const ModelVolume *mv : mo->volumes) {
            if (mv->is_model_part()) { trafo_matrices.emplace_back(mi->get_transformation().get_matrix() * mv->get_matrix()); }
        }

        Vec3f  normal                       = Vec3f::Zero();
        Vec3f  hit                          = Vec3f::Zero();
        size_t facet                        = 0;
        Vec3f  closest_hit                  = Vec3f::Zero();
        Vec3f  closest_normal               = Vec3f::Zero();
        double closest_hit_squared_distance = std::numeric_limits<double>::max();
        int    closest_hit_mesh_id          = -1;

        // Cast a ray on all meshes, pick the closest hit and save it for the respective mesh
        for (int mesh_id = 0; mesh_id < int(trafo_matrices.size()); ++mesh_id) {
            MeshRaycaster mesh_raycaster = MeshRaycaster(mo->volumes[mesh_id]->mesh_ptr());

            if (mesh_raycaster.unproject_on_mesh(mouse_position, trafo_matrices[mesh_id], camera, hit, normal,
                                                                           m_c->object_clipper()->get_clipping_plane(), &facet)) {
                // In case this hit is clipped, skip it.
                if (is_mesh_point_clipped(hit.cast<double>(), trafo_matrices[mesh_id]))
                    continue;

                // Is this hit the closest to the camera so far?
                double hit_squared_distance = (camera.get_position() - trafo_matrices[mesh_id] * hit.cast<double>()).squaredNorm();
                if (hit_squared_distance < closest_hit_squared_distance) {
                    closest_hit_squared_distance = hit_squared_distance;
                    closest_hit_mesh_id          = mesh_id;
                    closest_hit                  = hit;
                    closest_normal               = normal;
                }
            }
        }

        if (closest_hit == Vec3f::Zero() && closest_normal == Vec3f::Zero())
            return true;

        m_rr = {mouse_position, closest_hit_mesh_id, closest_hit, closest_normal};

        m_is_modify = true;
        generate_text_volume(false);
        plater->update();
    }

    return true;
}

bool GLGizmoText::on_mouse(const wxMouseEvent &mouse_event)
{
    // wxCoord == int --> wx/types.h
    Vec2i32 mouse_coord(mouse_event.GetX(), mouse_event.GetY());
    Vec2d mouse_pos = mouse_coord.cast<double>();
    bool control_down           = mouse_event.CmdDown();

    if (mouse_event.Moving()) {
        gizmo_event(SLAGizmoEventType::Moving, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), control_down);
    }

    // when control is down we allow scene pan and rotation even when clicking
    // over some object
    bool grabber_contains_mouse = (get_hover_id() != -1);

    if (mouse_event.LeftDown()) {
        if ((!control_down || grabber_contains_mouse) &&            
            gizmo_event(SLAGizmoEventType::LeftDown, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false))
            // the gizmo got the event and took some action, there is no need
            // to do anything more
            return true;
    }

    return use_grabbers(mouse_event);
}

void GLGizmoText::on_register_raycasters_for_picking()
{
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);
}

void GLGizmoText::on_unregister_raycasters_for_picking() { m_parent.set_raycaster_gizmos_on_top(false); }

void GLGizmoText::on_set_state()
{
    if (m_state == EState::On) {
        if (m_parent.get_selection().is_single_volume() || m_parent.get_selection().is_single_modifier()) {
            ModelVolume *model_volume = get_selected_single_volume(m_object_idx, m_volume_idx);
            if (model_volume) {
                TextInfo text_info = model_volume->get_text_info();
                if (!text_info.m_text.empty()) {
                    load_from_text_info(text_info);
                    m_is_modify = true;
                }
            }
        }
    }
    else if (m_state == EState::Off) {
        reset_text_info();
        delete_temp_preview_text_volume();
        m_parent.use_slope(false);
        m_parent.toggle_model_objects_visibility(true);
    }
}

CommonGizmosDataID GLGizmoText::on_get_requirements() const
{
    return CommonGizmosDataID(
          int(CommonGizmosDataID::SelectionInfo)
        | int(CommonGizmosDataID::InstancesHider)
        | int(CommonGizmosDataID::Raycaster)
        | int(CommonGizmosDataID::ObjectClipper));
}

std::string GLGizmoText::on_get_name() const
{
    return _u8L("Text shape");
}

bool GLGizmoText::on_is_activable() const
{
    // This is assumed in GLCanvas3D::do_rotate, do not change this
    // without updating that function too.
    if (m_parent.get_selection().is_single_full_instance())
        return true;

    int obejct_idx, volume_idx;
    ModelVolume *model_volume = get_selected_single_volume(obejct_idx, volume_idx);
    if (model_volume)
        return !model_volume->get_text_info().m_text.empty();

    return false;
}

void GLGizmoText::on_render()
{
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));

    std::string text = std::string(m_text);
    if (text.empty()) {
        delete_temp_preview_text_volume();
        return;
    }

    ModelObject *mo = nullptr;
    mo = m_c->selection_info()->model_object();
    
    if (mo == nullptr) {
        const Selection &selection = m_parent.get_selection();
        mo = selection.get_model()->objects[m_object_idx];
    }

    if (mo == nullptr) {
        BOOST_LOG_TRIVIAL(info) << boost::format("Text: selected object is null");
        return;
    }

    // First check that the mouse pointer is on an object.
    const Selection &    selection = m_parent.get_selection();
    const ModelInstance *mi        = mo->instances[0];    
    Plater *plater = wxGetApp().plater();
    if (!plater)
        return;

    if (!m_is_modify || m_shift_down) {
        const Camera &camera = wxGetApp().plater()->get_camera();
        // Precalculate transformations of individual meshes.
        std::vector<Transform3d> trafo_matrices;
        for (const ModelVolume *mv : mo->volumes) {
            if (mv->is_model_part()) trafo_matrices.emplace_back(mi->get_transformation().get_matrix() * mv->get_matrix());
        }
        // Raycast and return if there's no hit.
        Vec2d mouse_pos;
        if (m_shift_down) {
            if (m_is_modify)
                mouse_pos = m_rr.mouse_position;
            else
                mouse_pos = m_origin_mouse_position;
        }
        else {
            mouse_pos = m_parent.get_local_mouse_position();
        }

        bool position_changed = update_raycast_cache(mouse_pos, camera, trafo_matrices);

        if (m_rr.mesh_id == -1) {
            delete_temp_preview_text_volume();
            return;
        }

        if (!position_changed && !m_need_update_text && !m_shift_down)
            return;
    }

    if (m_is_modify && m_grabbers.size() == 1) {
        std::vector<Transform3d> trafo_matrices;
        for (const ModelVolume *mv : mo->volumes) {
            if (mv->is_model_part()) {
                trafo_matrices.emplace_back(mi->get_transformation().get_matrix() * mv->get_matrix());
            }
        }

        m_mouse_position_world = trafo_matrices[m_rr.mesh_id] * Vec3d(m_rr.hit(0), m_rr.hit(1), m_rr.hit(2));

        float mean_size = (float) (GLGizmoBase::Grabber::FixedGrabberSize);

        m_grabbers[0].center       = m_mouse_position_world;
        m_grabbers[0].enabled      = true;

        GLShaderProgram *shader    = wxGetApp().get_shader("gouraud_light");
        if (shader != nullptr) {
            shader->start_using();
            shader->set_uniform("emission_factor", 0.1f);
            render_grabbers(mean_size);

            shader->stop_using();
        }
    }
    
    delete_temp_preview_text_volume();

    if (m_is_modify && !m_need_update_text)
        return;

    generate_text_volume();
    plater->update();
}

void GLGizmoText::on_dragging(const UpdateData &data)
{
    Vec2d              mouse_pos = Vec2d(data.mouse_pos.x(), data.mouse_pos.y());
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (m_is_modify) {
        const Selection &selection = m_parent.get_selection();
        mo                         = selection.get_model()->objects[m_object_idx];
    }
    if (mo == nullptr) return;

    const Selection &    selection = m_parent.get_selection();
    const ModelInstance *mi        = mo->instances[selection.get_instance_idx()];
    const Camera &       camera    = wxGetApp().plater()->get_camera();

    std::vector<Transform3d> trafo_matrices;
    for (const ModelVolume *mv : mo->volumes) {
        if (mv->is_model_part()) { trafo_matrices.emplace_back(mi->get_transformation().get_matrix() * mv->get_matrix()); }
    }

    Vec3f  normal                       = Vec3f::Zero();
    Vec3f  hit                          = Vec3f::Zero();
    size_t facet                        = 0;
    Vec3f  closest_hit                  = Vec3f::Zero();
    Vec3f  closest_normal               = Vec3f::Zero();
    double closest_hit_squared_distance = std::numeric_limits<double>::max();
    int    closest_hit_mesh_id          = -1;

    // Cast a ray on all meshes, pick the closest hit and save it for the respective mesh
    for (int mesh_id = 0; mesh_id < int(trafo_matrices.size()); ++mesh_id) {
        if (mesh_id == m_volume_idx)
            continue;

        MeshRaycaster mesh_raycaster = MeshRaycaster(mo->volumes[mesh_id]->mesh_ptr());

        if (mesh_raycaster.unproject_on_mesh(mouse_pos, trafo_matrices[mesh_id], camera, hit, normal, m_c->object_clipper()->get_clipping_plane(),
                                                                       &facet)) {
            // In case this hit is clipped, skip it.
            if (is_mesh_point_clipped(hit.cast<double>(), trafo_matrices[mesh_id])) continue;

            // Is this hit the closest to the camera so far?
            double hit_squared_distance = (camera.get_position() - trafo_matrices[mesh_id] * hit.cast<double>()).squaredNorm();
            if (hit_squared_distance < closest_hit_squared_distance) {
                closest_hit_squared_distance = hit_squared_distance;
                closest_hit_mesh_id          = mesh_id;
                closest_hit                  = hit;
                closest_normal               = normal;
            }
        }
    }

    if (closest_hit == Vec3f::Zero() && closest_normal == Vec3f::Zero()) return;

    if (closest_hit_mesh_id != -1) {
        m_rr = {mouse_pos, closest_hit_mesh_id, closest_hit, closest_normal};
        m_need_update_text = true;
    }
}

void GLGizmoText::push_button_style(bool pressed) {
    if (m_is_dark_mode) {
        if (pressed) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(43 / 255.f, 64 / 255.f, 54 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(43 / 255.f, 64 / 255.f, 54 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(43 / 255.f, 64 / 255.f, 54 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 174 / 255.f, 66 / 255.f, 1.f));
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(45.f / 255.f, 45.f / 255.f, 49.f / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(84 / 255.f, 84 / 255.f, 90 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(84 / 255.f, 84 / 255.f, 90 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(45.f / 255.f, 45.f / 255.f, 49.f / 255.f, 1.f));
        }
    }
    else {
        if (pressed) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(219 / 255.f, 253 / 255.f, 231 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(219 / 255.f, 253 / 255.f, 231 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(219 / 255.f, 253 / 255.f, 231 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 174 / 255.f, 66 / 255.f, 1.f));
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(238 / 255.f, 238 / 255.f, 238 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(238 / 255.f, 238 / 255.f, 238 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 1.f));
        }
    
    }
}

void GLGizmoText::pop_button_style() {
    ImGui::PopStyleColor(4);
}

void GLGizmoText::push_combo_style(const float scale) {
    if (m_is_dark_mode) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BG_DARK);
        ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.68f, 0.26f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImGuiWrapper::COL_WINDOW_BG_DARK);
        ImGui::PushStyleColor(ImGuiCol_Button, { 1.00f, 1.00f, 1.00f, 0.0f });
    }
    else {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BG);
        ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.68f, 0.26f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImGuiWrapper::COL_WINDOW_BG);
        ImGui::PushStyleColor(ImGuiCol_Button, { 1.00f, 1.00f, 1.00f, 0.0f });
    }
}

void GLGizmoText::pop_combo_style()
{
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(7);
}

// BBS
void GLGizmoText::on_render_input_window(float x, float y, float bottom_limit)
{
    if (m_imgui->get_font_size() != m_scale) {
        m_scale = m_imgui->get_font_size();
        update_font_texture();
    }
    if (m_textures.size() == 0) {
        BOOST_LOG_TRIVIAL(info) << "GLGizmoText has no texture";
        return;
    }

    const Selection &selection = m_parent.get_selection();
    if (selection.is_single_full_instance() || selection.is_single_full_object()) {
        const GLVolume * gl_volume = selection.get_first_volume();
        int object_idx = gl_volume->object_idx();
        if (object_idx != m_object_idx || (object_idx == m_object_idx && m_volume_idx != -1)) {
            m_object_idx = object_idx;
            m_volume_idx = -1;
            reset_text_info();
        }
    } else if (selection.is_single_volume() || selection.is_single_modifier()) {
        int object_idx, volume_idx;
        ModelVolume *model_volume = get_selected_single_volume(object_idx, volume_idx);
        if ((object_idx != m_object_idx || (object_idx == m_object_idx && volume_idx != m_volume_idx))
            && model_volume) {
            TextInfo text_info = model_volume->get_text_info();
            load_from_text_info(text_info);
            m_is_modify = true;
            m_volume_idx = volume_idx;
            m_object_idx = object_idx;
        }
    }

    const float win_h = ImGui::GetWindowHeight();
    y = std::min(y, bottom_limit - win_h);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);

    static float last_y = 0.0f;
    static float last_h = 0.0f;

    const float currt_scale = m_parent.get_scale();
    ImGuiWrapper::push_toolbar_style(currt_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0,5.0) * currt_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * currt_scale);
    GizmoImguiBegin("Text", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float space_size = m_imgui->get_style_scaling() * 8;
    const std::array<std::string, 7> cap_array = std::array<std::string, 7>{ "font", "size", "thickness", "text_gap", "angle", "embeded_depth", "input_text" };
    float caption_size  = 0.0f;
    for (const auto &t : cap_array) {
        caption_size = std::max(caption_size, m_imgui->calc_text_size(m_desc[t]).x);
    }
    caption_size += space_size + ImGui::GetStyle().WindowPadding.x;

    float input_text_size = m_imgui->scaled(10.0f);
    float button_size = ImGui::GetFrameHeight();

    ImVec2 selectable_size(std::max((input_text_size + ImGui::GetFrameHeight() * 2), m_combo_width + SELECTABLE_INNER_OFFSET * currt_scale), m_combo_height);
    float list_width = selectable_size.x + ImGui::GetStyle().ScrollbarSize + 2 * currt_scale;

    float input_size = list_width - button_size * 2 - ImGui::GetStyle().ItemSpacing.x * 4;

    ImTextureID normal_B = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TEXT_B);
    ImTextureID normal_T = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TEXT_T);
    ImTextureID normal_B_dark = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TEXT_B_DARK);
    ImTextureID normal_T_dark = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TEXT_T_DARK);

    // adjust window position to avoid overlap the view toolbar
    if (last_h != win_h || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        if (last_h != win_h)
            last_h = win_h;
        if (last_y != y)
            last_y = y;
    }

    ImGui::AlignTextToFramePadding();

    m_imgui->text(m_desc["font"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(list_width);
    push_combo_style(currt_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f * currt_scale);

    std::vector<int> filtered_items_idx;
    bool is_filtered = false;
    if (m_imgui->bbl_combo_with_filter("##Combo_Font", m_font_names[m_curr_font_idx], m_font_names, &filtered_items_idx, &is_filtered, selectable_size.y)) {
        int show_items_count = is_filtered ? filtered_items_idx.size() : m_textures.size();

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(SELECTABLE_INNER_OFFSET, 0)* currt_scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
        for (int i = 0; i < show_items_count; i++)
        {
            int idx = is_filtered ? filtered_items_idx[i] : i;
            const bool is_selected = (idx == m_curr_font_idx);
            ImTextureID icon_id = (ImTextureID)(intptr_t)(m_textures[idx].texture->get_id());
            ImVec4 tint_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if (ImGui::BBLImageSelectable(icon_id, selectable_size, { (float)m_textures[idx].w, (float)m_textures[idx].h }, m_textures[idx].hl, tint_color, { 0, 0 }, { 1, 1 }, is_selected))
            {
                m_curr_font_idx = idx;
                m_font_name = m_textures[m_curr_font_idx].font_name;
                ImGui::CloseCurrentPopup();
                m_need_update_text = true;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::PopStyleVar(3);
        ImGui::EndListBox();
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
    pop_combo_style();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["size"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(input_size);
    if(ImGui::InputFloat("###font_size", &m_font_size, 0.0f, 0.0f, "%.2f"))
        m_need_update_text = true;
    if (m_font_size < 3.0f)m_font_size = 3.0f;
    ImGui::SameLine();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * currt_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {1.0f * currt_scale, 1.0f * currt_scale });
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f * currt_scale);
    push_button_style(m_bold);
    if (ImGui::ImageButton(m_is_dark_mode ? normal_B_dark : normal_B, {button_size - 2 * ImGui::GetStyle().FramePadding.x, button_size - 2 * ImGui::GetStyle().FramePadding.y})) {
        m_bold = !m_bold;
        m_need_update_text = true;
    }
    pop_button_style();
    ImGui::SameLine();
    push_button_style(m_italic);
    if (ImGui::ImageButton(m_is_dark_mode ? normal_T_dark : normal_T, {button_size - 2 * ImGui::GetStyle().FramePadding.x, button_size - 2 * ImGui::GetStyle().FramePadding.y})) {
        m_italic = !m_italic;
        m_need_update_text = true;
    }
    pop_button_style();
    ImGui::PopStyleVar(3);

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["thickness"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(list_width);
    float old_value = m_thickness;
    ImGui::InputFloat("###text_thickness", &m_thickness, 0.0f, 0.0f, "%.2f");
    if (m_thickness < 0.1f)
        m_thickness = 0.1f;
    if (old_value != m_thickness)
        m_need_update_text = true;

    const float slider_icon_width = m_imgui->get_slider_icon_size().x;
    const float slider_width      = list_width - 1.5 * slider_icon_width - space_size;
    const float drag_left_width   = caption_size + slider_width + space_size;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["text_gap"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(slider_width);
    if (m_imgui->bbl_slider_float_style("##text_gap", &m_text_gap, -100, 100, "%.2f", 1.0f, true))
        m_need_update_text = true;
    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    if (ImGui::BBLDragFloat("##text_gap_input", &m_text_gap, 0.05f, 0.0f, 0.0f, "%.2f"))
        m_need_update_text = true;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["angle"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(slider_width);
    if (m_imgui->bbl_slider_float_style("##angle", &m_rotate_angle, 0, 360, "%.2f", 1.0f, true))
        m_need_update_text = true;
    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    if (ImGui::BBLDragFloat("##angle_input", &m_rotate_angle, 0.05f, 0.0f, 0.0f, "%.2f"))
        m_need_update_text = true;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["embeded_depth"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(list_width);
    old_value = m_embeded_depth;
    ImGui::InputFloat("###text_embeded_depth", &m_embeded_depth, 0.0f, 0.0f, "%.2f");
    if (m_embeded_depth < 0.f)
        m_embeded_depth = 0.f;
    if (old_value != m_embeded_depth)
        m_need_update_text = true;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["input_text"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(list_width);

    if(ImGui::InputText("", m_text, sizeof(m_text)))
        m_need_update_text = true;

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(x, get_cur_y);

    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    
    ImGui::SameLine(caption_size);
    ImGui::AlignTextToFramePadding();
    if (m_imgui->bbl_checkbox(m_desc["surface"], m_is_surface_text))
        m_need_update_text = true;

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (m_imgui->bbl_checkbox(m_desc["horizontal_text"], m_keep_horizontal))
        m_need_update_text = true;

    //ImGui::SameLine();
    //ImGui::AlignTextToFramePadding();
    //m_imgui->text(_L("Status") + ": ");
    //float status_cap = m_imgui->calc_text_size(_L("Status:")).x + space_size + ImGui::GetStyle().WindowPadding.x;
    //ImGui::SameLine();
    //m_imgui->text(m_is_modify ? _L("Modify") : _L("Add"));

    ImGui::PopStyleVar(2);

#if 0
    ImGuiIO& io = ImGui::GetIO();
    ImFontAtlas* atlas = io.Fonts;
    ImVec4 tint_col = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 border_col = ImVec4(0.0f, 0.0f, 0.0f, 0.8f);
    m_imgui->text(wxString("") << atlas->TexWidth << " * " << atlas->TexHeight);
    ImGui::Image(atlas->TexID, ImVec2((float)atlas->TexWidth, (float)atlas->TexHeight), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint_col, border_col);
#endif

    GizmoImguiEnd();
    ImGui::PopStyleVar(2);
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoText::show_tooltip_information(float x, float y)
{
    std::array<std::string, 1> info_array  = std::array<std::string, 1>{"rotate_text"};
    float                      caption_max = 0.f;
    for (const auto &t : info_array) { caption_max = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x); }

    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(": ").x + 35.f;

    float  font_size   = ImGui::GetFontSize();
    ImVec2 button_size = ImVec2(font_size * 1.8, font_size * 1.3);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, ImGui::GetStyle().FramePadding.y});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max](const wxString &caption, const wxString &text) {
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : info_array) draw_text_with_caption(m_desc.at(t + "_caption") + ": ", m_desc.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GLGizmoText::reset_text_info()
{
    m_font_name     = "";
    m_font_size     = 16.f;
    m_curr_font_idx = 0;
    m_bold          = true;
    m_italic        = false;
    m_thickness     = 2.f;
    strcpy(m_text, m_font_name.c_str());
    m_embeded_depth   = 0.f;
    m_rotate_angle    = 0;
    m_text_gap        = 0.f;
    m_is_surface_text = true;
    m_keep_horizontal = false;

    m_is_modify = false;
    m_grabbers[0].enabled = false;
}

bool GLGizmoText::update_text_positions(const std::vector<std::string>& texts)
{
    std::vector<double> text_lengths;
    for (int i = 0; i < texts.size(); ++i) {
        std::string alpha;
        if (texts[i] == " ") {
            alpha = "i";
        } else {
            alpha = texts[i];
        }
        TextResult text_result;
        load_text_shape(alpha.c_str(), m_font_name.c_str(), m_font_size, m_thickness + m_embeded_depth, m_bold, m_italic, text_result);
        double half_x_length = text_result.text_width / 2;
        text_lengths.emplace_back(half_x_length);
    }

    int text_num = texts.size();
    m_position_points.clear();
    m_normal_points.clear();
    ModelObject *mo = m_c->selection_info()->model_object();
    if (m_is_modify) {
        const Selection &selection    = m_parent.get_selection();
        mo = selection.get_model()->objects[m_object_idx];
    }
    if (mo == nullptr)
        return false;

    const Selection &    selection = m_parent.get_selection();
    const ModelInstance *mi        = mo->instances[selection.get_instance_idx()];

    // Precalculate transformations of individual meshes.
    std::vector<Transform3d> trafo_matrices;
    std::vector<Transform3d> rotate_trafo_matrices;
    for (const ModelVolume *mv : mo->volumes) {
        if (mv->is_model_part()) {
            trafo_matrices.emplace_back(mi->get_transformation().get_matrix() * mv->get_matrix());
            rotate_trafo_matrices.emplace_back(mi->get_transformation().get_matrix(true, false, true, true) * mv->get_matrix(true, false, true, true));
        }
    }

    if (m_rr.mesh_id == -1) {
        BOOST_LOG_TRIVIAL(info) << boost::format("Text: mrr_mesh_id is -1");
        return false;
    }

    m_mouse_position_world = trafo_matrices[m_rr.mesh_id] * Vec3d(m_rr.hit(0), m_rr.hit(1), m_rr.hit(2));
    m_mouse_normal_world   = rotate_trafo_matrices[m_rr.mesh_id] * Vec3d(m_rr.normal(0), m_rr.normal(1), m_rr.normal(2));

    TriangleMesh slice_meshs;
    int mesh_index = 0;
    int volume_index = 0;
    for (int i = 0; i < mo->volumes.size(); ++i) {
        // skip the editing text volume
        if (m_is_modify && m_volume_idx == i)
            continue;

        ModelVolume *mv = mo->volumes[i];
        if (mv->is_model_part()) {
            if (mesh_index == m_rr.mesh_id) {
                volume_index = i;
            }
            TriangleMesh vol_mesh(mv->mesh());
            vol_mesh.transform(mv->get_matrix());
            slice_meshs.merge(vol_mesh);
            mesh_index++;
        }
    }

    ModelVolume* volume = mo->volumes[volume_index];

    Vec3d temp_position = m_mouse_position_world;
    Vec3d temp_normal   = m_mouse_normal_world;

    Vec3d cut_plane = Vec3d::UnitY();
    double epson = 1e-6;
    if (!(abs(temp_normal.x()) <= epson && abs(temp_normal.y()) <= epson && abs(temp_normal.z()) > epson)) { // temp_normal != Vec3d::UnitZ()
        Vec3d v_plane   = temp_normal.cross(Vec3d::UnitZ());
        cut_plane = v_plane.cross(temp_normal);
    }

    Transform3d rotate_trans;
    rotate_trans.setIdentity();
    rotate_trans.rotate(Eigen::AngleAxisd(Geometry::deg2rad(m_rotate_angle), temp_normal));
    cut_plane = rotate_trans * cut_plane;

    m_cut_plane_dir = cut_plane;

    if (m_keep_horizontal && m_mouse_normal_world != Vec3d::UnitZ())
        m_cut_plane_dir = Vec3d::UnitZ();

    if (!m_is_surface_text) {
        m_position_points.resize(text_num);
        m_normal_points.resize(text_num);

        Vec3d pos_dir = m_cut_plane_dir.cross(m_mouse_normal_world);
        pos_dir.normalize();
        if (text_num % 2 == 1) {
            m_position_points[text_num / 2] = m_mouse_position_world;
            for (int i = 0; i < text_num / 2; ++i) {
                double left_gap = text_lengths[text_num / 2 - i - 1] + m_text_gap + text_lengths[text_num / 2 - i];
                if (left_gap < 0)
                    left_gap = 0;

                double right_gap = text_lengths[text_num / 2 + i + 1] + m_text_gap + text_lengths[text_num / 2 + i];
                if (right_gap < 0)
                    right_gap = 0;

                m_position_points[text_num / 2 - 1 - i] = m_position_points[text_num / 2 - i] - left_gap * pos_dir;
                m_position_points[text_num / 2 + 1 + i] = m_position_points[text_num / 2 + i] + right_gap * pos_dir;
            }
        } else {
            for (int i = 0; i < text_num / 2; ++i) {
                double left_gap = i == 0 ? (text_lengths[text_num / 2 - i - 1] + m_text_gap / 2) :
                    (text_lengths[text_num / 2 - i - 1] + m_text_gap + text_lengths[text_num / 2 - i]);
                if (left_gap < 0)
                    left_gap = 0;

                double right_gap = i == 0 ? (text_lengths[text_num / 2 + i] + m_text_gap / 2) :
                (text_lengths[text_num / 2 + i] + m_text_gap + text_lengths[text_num / 2 + i - 1]);
                if (right_gap < 0)
                    right_gap = 0;

                if (i == 0) {
                    m_position_points[text_num / 2 - 1 - i] = m_mouse_position_world - left_gap * pos_dir;
                    m_position_points[text_num / 2 + i]     = m_mouse_position_world + right_gap * pos_dir;
                    continue;
                }

                m_position_points[text_num / 2 - 1 - i] = m_position_points[text_num / 2 - i] - left_gap * pos_dir;
                m_position_points[text_num / 2 + i]     = m_position_points[text_num / 2 + i - 1] + right_gap * pos_dir;
            }
        }

        for (int i = 0; i < text_num; ++i) {
            m_normal_points[i] = m_mouse_normal_world;
        }

        return true;
    }

    double   phi;
    Vec3d    rotation_axis;
    Matrix3d rotation_matrix;
    Geometry::rotation_from_two_vectors(m_cut_plane_dir, Vec3d::UnitZ(), rotation_axis, phi, &rotation_matrix);
    if (abs(phi - PI) < 1e-6) {
        Transform3d transform = Transform3d::Identity();
        transform.rotate(Eigen::AngleAxisd(phi, m_mouse_normal_world));
        rotation_matrix = transform.matrix().block<3, 3>(0, 0);
    }

    Transform3d transfo1;
    transfo1.setIdentity();
    transfo1.translate(-(mi->get_transformation().get_offset() + volume->get_transformation().get_offset()));
    transfo1 = rotation_matrix * transfo1;

    Transform3d transfo2;
    transfo2.setIdentity();
    transfo2.translate(mi->get_transformation().get_offset() + volume->get_transformation().get_offset());
    Transform3d       transfo = transfo2 * transfo1;

    Vec3d click_point = transfo * temp_position;

    MeshSlicingParams slicing_params;
    slicing_params.trafo = transfo * mi->get_transformation().get_matrix() /** volume->get_transformation().get_matrix()*/;
    // for debug
    // its_write_obj(slice_meshs.its, "D:/debug_files/mesh.obj");

    // generate polygons
    const Polygons temp_polys = slice_mesh(slice_meshs.its, click_point.z(), slicing_params);

    m_mouse_position_world = click_point;
    m_mouse_normal_world   = transfo * temp_normal;

    m_mouse_position_world.x() *= 1e6;
    m_mouse_position_world.y() *= 1e6;

    // for debug
    //export_regions_to_svg(Point(m_mouse_position_world.x(), m_mouse_position_world.y()), temp_polys);

    Polygons polys = union_(temp_polys);

    auto point_in_line_rectange = [](const Line &line, const Point &point, double& distance) {
        distance = line.distance_to(point);
        return distance < line.length() / 2;
    };

    int            index     = 0;
    double  min_distance = 1e12;
    Polygon        hit_ploy;
    for (const Polygon poly : polys) {
        if (poly.points.size() == 0)
            continue;

        Lines lines = poly.lines();
        for (int i = 0; i < lines.size(); ++i) {
            Line line = lines[i];
            double distance = min_distance;
            if (point_in_line_rectange(line, Point(m_mouse_position_world.x(), m_mouse_position_world.y()), distance)) {
                if (distance < min_distance) {
                    min_distance = distance;
                    index = i;
                    hit_ploy = poly;
                }
            }
        }
    }

    if (hit_ploy.points.size() == 0) {
        BOOST_LOG_TRIVIAL(info) << boost::format("Text: the hit polygon is null");
        return false;
    }

    auto make_trafo_for_slicing = [](const Transform3d &trafo) -> Transform3d {
        auto                          t = trafo;
        static constexpr const double s = 1. / SCALING_FACTOR;
        t.prescale(Vec3d(s, s, 1.));
        return t.cast<double>();
    };
    transfo                 = make_trafo_for_slicing(transfo);
    Transform3d transfo_inv = transfo.inverse();
    std::vector<Vec3d> new_points;
    for (int i = 0; i < hit_ploy.points.size(); ++i) {
        new_points.emplace_back(transfo_inv * Vec3d(hit_ploy.points[i].x(),  hit_ploy.points[i].y(), click_point.z()));
    }
    m_mouse_position_world = transfo_inv * m_mouse_position_world;

    Polygon_3D new_polygon(new_points);
    m_position_points.resize(text_num);
    if (text_num % 2 == 1) {
        m_position_points[text_num / 2] = Vec3d(m_mouse_position_world.x(), m_mouse_position_world.y(), m_mouse_position_world.z());

        std::vector<Line_3D>  lines       = new_polygon.get_lines();
        Line_3D   line        = lines[index];
        {
            int    index1      = index;
            double left_length = (m_mouse_position_world - line.a).cast<double>().norm();
            int    left_num    = text_num / 2;
            while (left_num > 0) {
                double gap_length = (text_lengths[left_num] + m_text_gap + text_lengths[left_num - 1]);
                if (gap_length < 0)
                    gap_length = 0;

                while (gap_length > left_length) {
                    gap_length -= left_length;
                    if (index1 == 0)
                        index1 = lines.size() - 1;
                    else
                        --index1;
                    left_length = lines[index1].length();
                }

                Vec3d direction = lines[index1].vector();
                direction.normalize();
                double distance_to_a = (left_length - gap_length);
                Line_3D   new_line      = lines[index1];

                double norm_value = direction.cast<double>().norm();
                double deta_x     = distance_to_a * direction.x() / norm_value;
                double deta_y     = distance_to_a * direction.y() / norm_value;
                double deta_z     = distance_to_a * direction.z() / norm_value;
                Vec3d  new_pos    = new_line.a + Vec3d(deta_x, deta_y, deta_z);
                left_num--;
                m_position_points[left_num] = new_pos;
                left_length                 = distance_to_a;
            }
        }

        {
            int    index2       = index;
            double right_length = (line.b - m_mouse_position_world).cast<double>().norm();
            int    right_num    = text_num / 2;
            while (right_num > 0) {
                double gap_length = (text_lengths[text_num - right_num] + m_text_gap + text_lengths[text_num - right_num - 1]);
                if (gap_length < 0)
                    gap_length = 0;

                while (gap_length > right_length) {
                    gap_length -= right_length;
                    if (index2 == lines.size() - 1)
                        index2 = 0;
                    else
                        ++index2;
                    right_length = lines[index2].length();
                }

                Line_3D line2 = lines[index2];
                line2.reverse();
                Vec3d direction = line2.vector();
                direction.normalize();
                double distance_to_b = (right_length - gap_length);
                Line_3D new_line         = lines[index2];

                double norm_value = direction.cast<double>().norm();
                double deta_x     = distance_to_b * direction.x() / norm_value;
                double deta_y     = distance_to_b * direction.y() / norm_value;
                double deta_z     = distance_to_b * direction.z() / norm_value;
                Vec3d new_pos = new_line.b + Vec3d(deta_x, deta_y, deta_z);
                m_position_points[text_num - right_num] = new_pos;
                right_length                            = distance_to_b;
                right_num--;
            }
        }
    }
    else {
        for (int i = 0; i < text_num / 2; ++i) {
            std::vector<Line_3D> lines = new_polygon.get_lines();
            Line_3D              line  = lines[index];
            {
                int    index1      = index;
                double left_length = (m_mouse_position_world - line.a).cast<double>().norm();
                int    left_num    = text_num / 2;
                for (int i = 0; i < text_num / 2; ++i) {
                    double gap_length = 0;
                    if (i == 0) {
                        gap_length = m_text_gap / 2 + text_lengths[text_num / 2 - 1 - i];
                    }
                    else {
                        gap_length = text_lengths[text_num / 2 - i] + m_text_gap + text_lengths[text_num / 2 - 1 - i];
                    }
                    if (gap_length < 0)
                        gap_length = 0;

                    while (gap_length > left_length) {
                        gap_length -= left_length;
                        if (index1 == 0)
                            index1 = lines.size() - 1;
                        else
                            --index1;
                        left_length = lines[index1].length();
                    }

                    Vec3d direction = lines[index1].vector();
                    direction.normalize();
                    double distance_to_a = (left_length - gap_length);
                    Line_3D   new_line      = lines[index1];

                    double norm_value = direction.cast<double>().norm();
                    double deta_x     = distance_to_a * direction.x() / norm_value;
                    double deta_y     = distance_to_a * direction.y() / norm_value;
                    double deta_z     = distance_to_a * direction.z() / norm_value;
                    Vec3d  new_pos    = new_line.a + Vec3d(deta_x, deta_y,deta_z);

                    m_position_points[text_num / 2 - 1 - i] = new_pos;
                    left_length                         = distance_to_a;
                }
            }

            {
                int    index2       = index;
                double right_length = (line.b - m_mouse_position_world).cast<double>().norm();
                int    right_num    = text_num / 2;
                double gap_length   = 0;
                for (int i = 0; i < text_num / 2; ++i) {
                    double gap_length = 0;
                    if (i == 0) {
                        gap_length = m_text_gap / 2 + text_lengths[text_num / 2 + i];
                    } else {
                        gap_length = text_lengths[text_num / 2 + i] + m_text_gap + text_lengths[text_num / 2 + i - 1];
                    }
                    if (gap_length < 0)
                        gap_length = 0;

                    while (gap_length > right_length) {
                        gap_length -= right_length;
                        if (index2 == lines.size() - 1)
                            index2 = 0;
                        else
                            ++index2;
                        right_length = lines[index2].length();
                    }

                    Line_3D line2 = lines[index2];
                    line2.reverse();
                    Vec3d direction = line2.vector();
                    direction.normalize();
                    double distance_to_b = (right_length - gap_length);
                    Line_3D   new_line      = lines[index2];

                    double norm_value                       = direction.cast<double>().norm();
                    double deta_x                           = distance_to_b * direction.x() / norm_value;
                    double deta_y                           = distance_to_b * direction.y() / norm_value;
                    double deta_z                           = distance_to_b * direction.z() / norm_value;
                    Vec3d  new_pos                          = new_line.b + Vec3d(deta_x, deta_y, deta_z);
                    m_position_points[text_num / 2 + i]     = new_pos;
                    right_length                            = distance_to_b;
                }
            }
        }
    }

    TriangleMesh mesh       = slice_meshs;
    std::vector<double> mesh_values(m_position_points.size(), 1e9);
    m_normal_points.resize(m_position_points.size());
    auto point_in_triangle_delete_area = [](const Vec3d &point, const Vec3d &point0, const Vec3d &point1, const Vec3d &point2) {
        Vec3d p0_p  = point - point0;
        Vec3d p0_p1 = point1 - point0;
        Vec3d p0_p2 = point2 - point0;
        Vec3d p_p0  = point0 - point;
        Vec3d p_p1  = point1 - point;
        Vec3d p_p2  = point2 - point;

        double s  = p0_p1.cross(p0_p2).norm();
        double s0 = p_p0.cross(p_p1).norm();
        double s1 = p_p1.cross(p_p2).norm();
        double s2 = p_p2.cross(p_p0).norm();

        return abs(s0 + s1 + s2 - s);
    };
    for (int i = 0; i < m_position_points.size(); ++i) {
        for (auto indice : mesh.its.indices) {
            stl_vertex stl_point0 = mesh.its.vertices[indice[0]];
            stl_vertex stl_point1 = mesh.its.vertices[indice[1]];
            stl_vertex stl_point2 = mesh.its.vertices[indice[2]];

            Vec3d point0 = Vec3d(stl_point0[0], stl_point0[1], stl_point0[2]);
            Vec3d point1 = Vec3d(stl_point1[0], stl_point1[1], stl_point1[2]);
            Vec3d point2 = Vec3d(stl_point2[0], stl_point2[1], stl_point2[2]);

            point0 = mi->get_transformation().get_matrix() * point0;
            point1 = mi->get_transformation().get_matrix() * point1;
            point2 = mi->get_transformation().get_matrix() * point2;

            double abs_area = point_in_triangle_delete_area(m_position_points[i], point0, point1, point2);
            if (mesh_values[i] > abs_area) {
                mesh_values[i] = abs_area;

                Vec3d s1           = point1 - point0;
                Vec3d s2           = point2 - point0;
                m_normal_points[i] = s1.cross(s2);
                m_normal_points[i].normalize();
            }
        }
    }
    return true;
}

TriangleMesh GLGizmoText::get_text_mesh(const char* text_str, const Vec3d &position, const Vec3d &normal, const Vec3d& text_up_dir)
{
    TextResult   text_result;
    load_text_shape(text_str, m_font_name.c_str(), m_font_size, m_thickness + m_embeded_depth, m_bold, m_italic, text_result);
    TriangleMesh mesh = text_result.text_mesh;

    auto   center      = mesh.bounding_box().center();
    double mesh_offset = center.z();
    mesh.translate(-text_result.text_width / 2, -m_font_size / 4, -center.z());

    double   phi;
    Vec3d    rotation_axis;
    Matrix3d rotation_matrix;
    Geometry::rotation_from_two_vectors(Vec3d::UnitZ(), normal, rotation_axis, phi, &rotation_matrix);
    mesh.rotate(phi, rotation_axis);

    auto project_on_plane = [](const Vec3d& dir, const Vec3d& plane_normal) -> Vec3d {
        return dir - (plane_normal.dot(dir) * plane_normal.dot(plane_normal)) * plane_normal;
    };

    Vec3d old_text_dir = Vec3d::UnitY();
    old_text_dir = rotation_matrix * old_text_dir;
    Vec3d new_text_dir = project_on_plane(text_up_dir, normal);
    new_text_dir.normalize();
    Geometry::rotation_from_two_vectors(old_text_dir, new_text_dir, rotation_axis, phi, &rotation_matrix);

    if (abs(phi - PI) < EPSILON)
        rotation_axis = normal;

    mesh.rotate(phi, rotation_axis);

    const Selection &        selection               = m_parent.get_selection();
    ModelObject *            model_object            = selection.get_model()->objects[m_object_idx];
    Geometry::Transformation instance_transformation = model_object->instances[0]->get_transformation();
    Vec3d                    offset                  = position - instance_transformation.get_offset();
    offset                                           = offset + mesh_offset * normal;
    offset                                           = offset - m_embeded_depth * normal;
    mesh.translate(offset.x(), offset.y(), offset.z());

    return mesh;
}

bool GLGizmoText::update_raycast_cache(const Vec2d &mouse_position, const Camera &camera, const std::vector<Transform3d> &trafo_matrices)
{
    if (m_rr.mouse_position == mouse_position) {
        return false;
    }

    if (m_is_modify)
        return false;

    Vec3f  normal                       = Vec3f::Zero();
    Vec3f  hit                          = Vec3f::Zero();
    size_t facet                        = 0;
    Vec3f  closest_hit                  = Vec3f::Zero();
    Vec3f  closest_nromal               = Vec3f::Zero();
    double closest_hit_squared_distance = std::numeric_limits<double>::max();
    int    closest_hit_mesh_id          = -1;

    // Cast a ray on all meshes, pick the closest hit and save it for the respective mesh
    for (int mesh_id = 0; mesh_id < int(trafo_matrices.size()); ++mesh_id) {
        if (m_preview_text_volume_id != -1 && mesh_id == int(trafo_matrices.size()) - 1)
            continue;

        if (m_c->raycaster()->raycasters()[mesh_id]->unproject_on_mesh(mouse_position, trafo_matrices[mesh_id], camera, hit, normal, m_c->object_clipper()->get_clipping_plane(),
                                                                       &facet)) {
            // In case this hit is clipped, skip it.
            if (is_mesh_point_clipped(hit.cast<double>(), trafo_matrices[mesh_id]))
                continue;

            double hit_squared_distance = (camera.get_position() - trafo_matrices[mesh_id] * hit.cast<double>()).squaredNorm();
            if (hit_squared_distance < closest_hit_squared_distance) {
                closest_hit_squared_distance = hit_squared_distance;
                closest_hit_mesh_id          = mesh_id;
                closest_hit                  = hit;
                closest_nromal               = normal;
            }
        }
    }
    
    m_rr = {mouse_position, closest_hit_mesh_id, closest_hit, closest_nromal};
    return true;
}

void GLGizmoText::generate_text_volume(bool is_temp)
{
    std::string text = std::string(m_text);
    if (text.empty())
        return;

    std::wstring_convert<std::codecvt_utf8<wchar_t>> str_cnv;
    std::wstring ws = boost::nowide::widen(m_text);
    std::vector<std::string> alphas;
    for (auto w : ws) {
        alphas.push_back(str_cnv.to_bytes(w));
    }

    update_text_positions(alphas);

    if (m_position_points.size() == 0)
        return;

    TriangleMesh mesh;
    for (int i = 0; i < alphas.size(); ++i) {
        TriangleMesh sub_mesh = get_text_mesh(alphas[i].c_str(), m_position_points[i], m_normal_points[i], m_cut_plane_dir);
        mesh.merge(sub_mesh);
    }

    if (mesh.empty())
        return;

    Plater *plater = wxGetApp().plater();
    if (!plater)
        return;

    TextInfo text_info = get_text_info();
    if (m_is_modify && m_need_update_text) {
        if (m_object_idx == -1 || m_volume_idx == -1) {
            BOOST_LOG_TRIVIAL(error) << boost::format("Text: selected object_idx = %1%, volume_idx = %2%") % m_object_idx % m_volume_idx;
            return;
        }

        plater->take_snapshot("Modify Text");
        const Selection &selection        = m_parent.get_selection();
        ModelObject *    model_object     = selection.get_model()->objects[m_object_idx];
        ModelVolume *    model_volume     = model_object->volumes[m_volume_idx];
        ModelVolume *    new_model_volume = model_object->add_volume(std::move(mesh));
        new_model_volume->set_text_info(text_info);
        new_model_volume->name = model_volume->name;
        new_model_volume->set_type(model_volume->type());
        new_model_volume->config.apply(model_volume->config);
        std::swap(model_object->volumes[m_volume_idx], model_object->volumes.back());
        model_object->delete_volume(model_object->volumes.size() - 1);
        plater->update();
    } else {
        if (m_need_update_text)
            plater->take_snapshot("Add Text");
        ObjectList *obj_list = wxGetApp().obj_list();
        int volume_id = obj_list->load_mesh_part(mesh, "text_shape", text_info, is_temp);
        m_preview_text_volume_id = is_temp ? volume_id : -1;
    }
    m_need_update_text    = false;
}

void GLGizmoText::delete_temp_preview_text_volume()
{
    const Selection &selection = m_parent.get_selection();
    if (m_preview_text_volume_id > 0) {
        ModelObject *model_object = selection.get_model()->objects[m_object_idx];
        if (m_preview_text_volume_id < model_object->volumes.size()) {
            Plater *plater = wxGetApp().plater();
            if (!plater)
                return;

            model_object->delete_volume(m_preview_text_volume_id);

            plater->update();
        }
        m_preview_text_volume_id = -1;
    }
}

TextInfo GLGizmoText::get_text_info()
{
    TextInfo text_info;
    text_info.m_font_name     = m_font_name;
    text_info.m_font_size     = m_font_size;
    text_info.m_curr_font_idx = m_curr_font_idx;
    text_info.m_bold          = m_bold;
    text_info.m_italic        = m_italic;
    text_info.m_thickness     = m_thickness;
    text_info.m_text          = m_text;
    text_info.m_rr            = m_rr;
    text_info.m_embeded_depth = m_embeded_depth;
    text_info.m_rotate_angle  = m_rotate_angle;
    text_info.m_text_gap      = m_text_gap;
    text_info.m_is_surface_text = m_is_surface_text;
    text_info.m_keep_horizontal = m_keep_horizontal;
    return text_info;
}

void GLGizmoText::load_from_text_info(const TextInfo &text_info)
{
    m_font_name     = text_info.m_font_name;
    m_font_size     = text_info.m_font_size;
    m_curr_font_idx = text_info.m_curr_font_idx;
    m_bold          = text_info.m_bold;
    m_italic        = text_info.m_italic;
    m_thickness     = text_info.m_thickness;
    strcpy(m_text, text_info.m_text.c_str());
    m_rr            = text_info.m_rr;
    m_embeded_depth = text_info.m_embeded_depth;
    m_rotate_angle  = text_info.m_rotate_angle;
    m_text_gap      = text_info.m_text_gap;
    m_is_surface_text = text_info.m_is_surface_text;
    m_keep_horizontal = text_info.m_keep_horizontal;
}

} // namespace GUI
} // namespace Slic3r

#include "UVEditorCanvas.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include <glad/gl.h>

#include "3DScene.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "OpenGLManager.hpp"
#include "Plater.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/AppConfig.hpp"

namespace Slic3r::GUI {

namespace {
// Roughly Blender's UV/Image editor palette, which is what this pane is measured against. The UV_
// prefix is not decoration: COLOR_BACKGROUND (and several other COLOR_*) are Win32 system-colour
// macros from WinUser.h, and an unprefixed name here expands to an integer literal mid-declaration.
const ColorRGBA UV_COLOR_BG            = { 0.16f, 0.16f, 0.16f, 1.f };
const ColorRGBA UV_COLOR_GRID          = { 1.f, 1.f, 1.f, 0.10f };
const ColorRGBA UV_COLOR_TILE_OUTLINE  = { 1.f, 1.f, 1.f, 0.45f };
const ColorRGBA UV_COLOR_WIRE          = { 0.85f, 0.85f, 0.85f, 0.35f }; // interior edges, unselected
const ColorRGBA UV_COLOR_BOUNDARY      = { 1.f, 0.35f, 0.15f, 0.85f };   // island outline == a seam, so Blender's seam red
// Island fills (#4): unselected a light green kept translucent so the texture/checker/wireframe under
// it still reads (the user asked for both these fills *and* to see the check overlay through them, so
// the alphas stay low); the selected one switches to the app accent teal #009688 and is marked mainly
// by its bold boundary and brighter wire rather than a heavy fill.
const ColorRGBA UV_COLOR_FILL          = { 0.55f, 0.85f, 0.45f, 0.40f }; // unselected: light green
const ColorRGBA UV_COLOR_SEL_FILL      = { 0.0f, 0.588f, 0.533f, 0.20f }; // selected: #009688 teal
const ColorRGBA UV_COLOR_SEL_WIRE      = { 0.75f, 1.f, 0.7f, 0.55f };
const ColorRGBA UV_COLOR_SEL_BOUNDARY  = { 0.2f, 1.f, 0.55f, 1.f };      // bold light-green edge (#7)
const ColorRGBA UV_COLOR_DIAL          = { 1.f, 0.85f, 0.2f, 0.9f };     // rotation protractor (#11)

constexpr float SNAP_PIXELS = 28.f; // how close a boundary vertex has to come before it sticks (#2)

// The pixel format this canvas is created with has to match the one the app's single shared
// wxGLContext was created against (that of View3D's canvas, from OpenGLManager::create_wxglcanvas()),
// so this mirrors that attribute list *including its multisampling*: WGL requires the HDC passed to
// wglMakeCurrent() to have the same pixel format as the one the context was created with, and a
// differing sample count is a differing pixel format. Getting only the non-multisample half of this
// right still leaves SetCurrent() failing, which is silent - the canvas then just shows whatever
// was last in its backbuffer, i.e. nothing.
std::vector<int> gl_attrib_list()
{
    int antialiasing_samples = 4;
    if (const AppConfig *app_config = wxGetApp().app_config; app_config != nullptr) {
        const std::string value = app_config->get(SETTING_OPENGL_AA_SAMPLES);
        if (value == "0" || value == "2" || value == "4" || value == "8" || value == "16")
            antialiasing_samples = ::atoi(value.c_str());
    }
    // OpenGLManager's own auto-detection has already run by now (View3D is created before this
    // canvas), so this only reads its verdict rather than re-detecting.
    if (!OpenGLManager::can_multisample())
        antialiasing_samples = 0;

    return {
        WX_GL_RGBA,
        WX_GL_DOUBLEBUFFER,
        WX_GL_MIN_RED,        8,
        WX_GL_MIN_GREEN,      8,
        WX_GL_MIN_BLUE,       8,
        WX_GL_MIN_ALPHA,      8,
        WX_GL_DEPTH_SIZE,     24,
        WX_GL_STENCIL_SIZE,   8,
        WX_GL_SAMPLE_BUFFERS, antialiasing_samples > 0 ? GL_TRUE : GL_FALSE,
        WX_GL_SAMPLES,        antialiasing_samples,
        0
    };
}

// Wide lines are only *required* to be supported in a compatibility profile; a core-profile driver is
// allowed to reject anything but 1.0 with GL_INVALID_VALUE. In practice every desktop driver we care
// about honours it, so ask and swallow the error rather than giving up thickness everywhere.
void set_line_width(float width)
{
    ::glLineWidth(width);
    ::glGetError();
}

uint64_t undirected_edge_key(int a, int b)
{
    if (a > b)
        std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

// Signed-area test, so it works whichever way round the triangle is wound.
bool point_in_triangle(const Vec2f &p, const Vec2f &a, const Vec2f &b, const Vec2f &c)
{
    const auto cross = [](const Vec2f &u, const Vec2f &v) { return u.x() * v.y() - u.y() * v.x(); };
    const float d0 = cross(b - a, p - a);
    const float d1 = cross(c - b, p - b);
    const float d2 = cross(a - c, p - c);
    const bool  has_neg = (d0 < 0.f) || (d1 < 0.f) || (d2 < 0.f);
    const bool  has_pos = (d0 > 0.f) || (d1 > 0.f) || (d2 > 0.f);
    return !(has_neg && has_pos);
}

// Shortest signed difference between two angles, so a rotation gesture crossing +/-pi doesn't jump.
float angle_delta(float from, float to)
{
    float d = to - from;
    while (d > float(M_PI))
        d -= 2.f * float(M_PI);
    while (d < -float(M_PI))
        d += 2.f * float(M_PI);
    return d;
}

// A 2D affine as the 4x4 the "flat" shader's view_model_matrix wants.
Transform3d to_transform3d(const UVEditorCanvas::IslandTransform &m)
{
    Transform3d t  = Transform3d::Identity();
    t(0, 0) = m(0, 0); t(0, 1) = m(0, 1); t(0, 3) = m(0, 2);
    t(1, 0) = m(1, 0); t(1, 1) = m(1, 1); t(1, 3) = m(1, 2);
    return t;
}
} // namespace

UVEditorCanvas::UVEditorCanvas(wxWindow *parent)
    : wxGLCanvas(parent, wxID_ANY, gl_attrib_list().data(), wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS)
{
    // The GL canvas paints its entire surface, so background erasing is unnecessary (and would
    // otherwise race with our own rendering) - same setup as OpenGLManager::create_wxglcanvas().
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    // Shares the app's one real GL context (see class comment) rather than creating an
    // independent one - this is the same call View3D/Preview/AssembleView make in
    // GUI_Preview.cpp, and is what lets this canvas reuse wxGetApp().get_shader(...) and GLModel.
    m_context = wxGetApp().init_glcontext(*this);

    Bind(wxEVT_PAINT, &UVEditorCanvas::on_paint, this);
    Bind(wxEVT_SIZE, &UVEditorCanvas::on_size, this);
    Bind(wxEVT_LEFT_DOWN, &UVEditorCanvas::on_mouse, this);
    Bind(wxEVT_LEFT_UP, &UVEditorCanvas::on_mouse, this);
    Bind(wxEVT_RIGHT_DOWN, &UVEditorCanvas::on_mouse, this);
    Bind(wxEVT_RIGHT_UP, &UVEditorCanvas::on_mouse, this);
    Bind(wxEVT_MIDDLE_DOWN, &UVEditorCanvas::on_mouse, this);
    Bind(wxEVT_MIDDLE_UP, &UVEditorCanvas::on_mouse, this);
    Bind(wxEVT_MOTION, &UVEditorCanvas::on_mouse, this);
    Bind(wxEVT_MOUSEWHEEL, &UVEditorCanvas::on_mouse, this);
    Bind(wxEVT_LEAVE_WINDOW, &UVEditorCanvas::on_leave, this);
    Bind(wxEVT_KEY_DOWN, &UVEditorCanvas::on_key, this);
    Bind(wxEVT_ERASE_BACKGROUND, &UVEditorCanvas::on_erase_background, this);
}

// NOTE (applies to set_background_texture() too): these are called from the texture-displacement
// gizmo's ImGui panel, i.e. from *inside* the main 3D canvas's own render pass. They must therefore
// only ever mark state dirty and schedule a repaint - rendering inline from here would make this
// canvas's GL surface current in the middle of the 3D canvas's frame (and did: it was also called
// while this pane was still hidden, where wxGLCanvas::SetCurrent() refuses to switch at all and the
// GL calls that followed simply landed on the 3D canvas instead).
void UVEditorCanvas::set_islands(Islands islands)
{
    // Only frame the view when a patch first appears, not on every stroke that extends one - the
    // latter would keep yanking the view out from under a user who has panned or zoomed.
    m_needs_fit |= m_islands.indices.empty();
    // Re-uploads that keep the same island/vertex count are just a refresh (e.g. a committed vertex edit
    // re-applied), not a re-segmentation, so the selection and the picked sub-element stay valid and are
    // worth keeping. A change in either count means the charts were renumbered and both are meaningless.
    const bool same_structure = m_islands.island_count == islands.island_count &&
                                m_islands.uvs.size() == islands.uvs.size();
    m_islands    = std::move(islands);
    if (m_selected_island >= m_islands.island_count)
        m_selected_island = -1;
    if (!same_structure) {
        // A fresh unwrap renumbers charts, so any previous multi-selection is meaningless: drop it, then
        // keep the primary (if still valid) as a single-island selection so the highlight is consistent.
        m_selection.clear();
        if (m_selected_island >= 0)
            m_selection.push_back(m_selected_island);
        // Vertex/edge picks index into the old unwrap; drop them too.
        m_active_vertex = -1;
        m_active_edge   = { -1, -1 };
        m_sel_vertices.clear();
        m_sel_edges.clear();
    }

    // Boundary vertices, bucketed per island, for snapping.
    m_island_boundary_verts.assign(size_t(std::max(m_islands.island_count, 0)), {});
    std::vector<bool> seen(m_islands.uvs.size(), false);
    for (const auto &[a, b] : m_islands.boundary_edges)
        for (const int v : { a, b }) {
            if (v < 0 || size_t(v) >= m_islands.uvs.size() || seen[size_t(v)])
                continue;
            seen[size_t(v)] = true;
            const int island = m_islands.vertex_island[size_t(v)];
            if (island >= 0 && size_t(island) < m_island_boundary_verts.size())
                m_island_boundary_verts[size_t(island)].push_back(v);
        }

    m_mesh_dirty            = true;
    m_background_quad_dirty = true; // the backdrop is sized to the unwrap, so it moved too
    Refresh();
}

void UVEditorCanvas::set_island_transforms(std::vector<IslandTransform> transforms)
{
    m_transforms            = std::move(transforms);
    m_background_quad_dirty = true; // content_bounds() depends on where the islands ended up
    Refresh();
}

void UVEditorCanvas::set_island_fill_colors(std::vector<ColorRGBA> colors)
{
    m_island_fill_colors = std::move(colors);
    Refresh();
}

void UVEditorCanvas::set_uv_transform(float tiling_scale, float rotation_deg, bool tile_enabled, bool tile_mirrored)
{
    m_tiling_scale          = (std::abs(tiling_scale) > 1e-6f) ? tiling_scale : 1.f;
    m_rotation_deg          = rotation_deg;
    m_tile_enabled          = tile_enabled;
    m_tile_mirrored         = tile_mirrored;
    m_background_quad_dirty = true;
}

void UVEditorCanvas::set_background_texture(const std::vector<unsigned char> &grayscale_pixels, int width, int height)
{
    if (width <= 0 || height <= 0 || grayscale_pixels.size() != size_t(width) * size_t(height)) {
        m_background_width = m_background_height = 0;
        m_background_pixels.clear();
    } else {
        m_background_width  = width;
        m_background_height = height;
        m_background_pixels.assign(size_t(width) * size_t(height) * 4, 255);
        for (size_t i = 0; i < grayscale_pixels.size(); ++i) {
            const unsigned char g          = grayscale_pixels[i];
            m_background_pixels[i * 4 + 0] = g;
            m_background_pixels[i * 4 + 1] = g;
            m_background_pixels[i * 4 + 2] = g;
            m_background_pixels[i * 4 + 3] = 255;
        }
    }
    m_background_dirty = true;
    Refresh();
}

void UVEditorCanvas::reset_view()
{
    m_needs_fit = true;
    Refresh();
}

void UVEditorCanvas::run_command(Command cmd)
{
    switch (cmd) {
    case Command::FrameAll:   reset_view(); return;
    case Command::ToggleSnap: m_snap_enabled = !m_snap_enabled; update_status(); return;
    // The rest need the layer data the canvas doesn't hold; hand them to the gizmo.
    case Command::AverageScale:
    case Command::CutSelectedIsland:
    case Command::ProjectFromView:
    case Command::JoinSelected:
    case Command::UnjoinSelected:
        if (m_on_command)
            m_on_command(cmd);
        return;
    }
}

void UVEditorCanvas::update_status()
{
    if (!m_on_status)
        return;

    wxString msg;
    switch (m_gesture) {
    case Gesture::MoveIsland:        msg = _L("Moving island  |  release to drop, Esc to cancel"); break;
    case Gesture::RotateIsland:
    case Gesture::RotateIslandModal:
        msg = wxString::Format(_L("Rotating island: %d°  |  Shift = snap 15°, click to confirm, Esc to cancel"),
                                int(std::lround(m_rot_display_deg)));
        break;
    case Gesture::ScaleIslandModal:  msg = _L("Scaling island  |  click to confirm, Esc to cancel"); break;
    case Gesture::MoveVertex:        msg = _L("Moving vertex  |  release to drop"); break;
    case Gesture::MoveEdge:          msg = _L("Moving edge  |  release to drop"); break;
    case Gesture::Pan:               msg = _L("Panning"); break;
    case Gesture::None:
    default:
        if (m_select_mode == SelectMode::Vertex)
            msg = m_sel_vertices.size() > 1 ?
                      wxString::Format(_L("%d vertices selected  |  drag = move together, Shift/Ctrl click = add/remove"),
                                       int(m_sel_vertices.size())) :
                      _L("Vertex mode: drag a vertex to reshape  |  Shift/Ctrl click = multi-select, wheel = zoom, Home = frame");
        else if (m_select_mode == SelectMode::Edge)
            msg = m_sel_edges.size() > 1 ?
                      wxString::Format(_L("%d edges selected  |  drag = move together, Shift/Ctrl click = add/remove"),
                                       int(m_sel_edges.size())) :
                      _L("Edge mode: drag an island edge to reshape  |  Shift/Ctrl click = multi-select, wheel = zoom, Home = frame");
        else if (m_selection.size() > 1)
            msg = wxString::Format(_L("%d islands selected  |  drag = move together, Shift/Ctrl click = add/remove, R/S = rotate/scale primary"),
                                    int(m_selection.size()));
        else if (m_selected_island >= 0)
            msg = wxString::Format(_L("Island %d selected  |  drag = move, R = rotate, S = scale, Shift/Ctrl click = multi-select, Home = frame"),
                                    m_selected_island + 1);
        else
            msg = _L("Click an island to select  |  Shift/Ctrl click = multi-select, wheel = zoom, middle-drag = pan, Home = frame all");
        if (m_snap_enabled)
            msg += _L("  |  snap ON");
        break;
    }
    m_on_status(msg);
}

Vec2f UVEditorCanvas::island_uv(size_t vertex) const
{
    const Vec2f &raw = m_islands.uvs[vertex];
    const int    island = m_islands.vertex_island[vertex];
    if (island < 0 || size_t(island) >= m_transforms.size())
        return raw;
    const IslandTransform &m = m_transforms[size_t(island)];
    return m.block<2, 2>(0, 0) * raw + m.col(2);
}

void UVEditorCanvas::content_bounds(Vec2f &min_uv, Vec2f &max_uv) const
{
    // Always include the texture's first tile, so an unwrap that happens to be tiny, or absent,
    // still leaves something sensibly framed on screen.
    min_uv = Vec2f(0.f, 0.f);
    max_uv = Vec2f(1.f, 1.f);
    for (size_t i = 0; i < m_islands.uvs.size(); ++i) {
        const Vec2f uv = island_uv(i);
        min_uv = min_uv.cwiseMin(uv);
        max_uv = max_uv.cwiseMax(uv);
    }
}

void UVEditorCanvas::fit_view_to_content()
{
    Vec2f min_uv, max_uv;
    content_bounds(min_uv, max_uv);

    m_pan       = 0.5f * (min_uv + max_uv);
    m_zoom      = std::max(0.5f * (max_uv - min_uv).maxCoeff() * 1.1f, 0.05f);
    m_needs_fit = false;
}

void UVEditorCanvas::view_half_extents(float &half_w, float &half_h) const
{
    const wxSize size = GetSize();
    const float  w    = float(std::max(1, size.GetWidth()));
    const float  h    = float(std::max(1, size.GetHeight()));

    // m_zoom is the half-extent visible across the *shorter* edge; the longer edge shows
    // proportionally more. Both axes have to be driven off the same uniform scale, or the content
    // comes out squashed on one of the two pane orientations.
    const float aspect = w / h;
    half_w             = m_zoom * std::max(aspect, 1.f);
    half_h             = m_zoom * std::max(1.f / aspect, 1.f);
}

Vec2f UVEditorCanvas::screen_to_uv(const wxPoint &px) const
{
    const wxSize size = GetSize();
    const float  w    = float(std::max(1, size.GetWidth()));
    const float  h    = float(std::max(1, size.GetHeight()));

    float half_w, half_h;
    view_half_extents(half_w, half_h);

    // Inverse of the projection in render(): x maps straight through, y is negated there so that v
    // runs down the screen - which means screen-down and v-increasing agree, and this is a plain
    // scale on both axes.
    return Vec2f(m_pan.x() + (2.f * float(px.x) / w - 1.f) * half_w,
                 m_pan.y() + (2.f * float(px.y) / h - 1.f) * half_h);
}

int UVEditorCanvas::island_at(const Vec2f &uv) const
{
    int found = -1;
    for (const Vec3i32 &tri : m_islands.indices) {
        if (tri.minCoeff() < 0 || size_t(tri.maxCoeff()) >= m_islands.uvs.size())
            continue;
        if (!point_in_triangle(uv, island_uv(size_t(tri[0])), island_uv(size_t(tri[1])), island_uv(size_t(tri[2]))))
            continue;

        const int island = m_islands.vertex_island[size_t(tri[0])];
        // Islands are allowed to overlap, so a point can be inside several. Keep whichever is already
        // selected - otherwise a drag of a partly-covered island would be stolen mid-gesture by the
        // one on top of it.
        if (is_selected(island))
            return island;
        found = island;
    }
    return found;
}

void UVEditorCanvas::set_select_mode(SelectMode mode)
{
    if (m_select_mode == mode)
        return;
    m_select_mode   = mode;
    m_active_vertex = -1;
    m_active_edge   = { -1, -1 };
    m_sel_vertices.clear();
    m_sel_edges.clear();
    m_gesture       = Gesture::None;
    update_status();
    Refresh();
}

int UVEditorCanvas::vertex_at(const Vec2f &uv) const
{
    // Screen-space threshold, so the pick feels the same at every zoom.
    const wxSize size      = GetSize();
    const float  uv_per_px = 2.f * m_zoom / float(std::max(1, std::min(size.GetWidth(), size.GetHeight())));
    const float  threshold = SNAP_PIXELS * uv_per_px;

    int   best   = -1;
    float best_d = threshold * threshold;
    for (size_t i = 0; i < m_islands.uvs.size(); ++i) {
        const float d = (island_uv(i) - uv).squaredNorm();
        if (d < best_d) {
            best_d = d;
            best   = int(i);
        }
    }
    return best;
}

std::pair<int, int> UVEditorCanvas::edge_at(const Vec2f &uv) const
{
    const wxSize size      = GetSize();
    const float  uv_per_px = 2.f * m_zoom / float(std::max(1, std::min(size.GetWidth(), size.GetHeight())));
    const float  threshold = SNAP_PIXELS * uv_per_px;

    // Point-to-segment distance in texture-UV space, against the island outlines (the edges the pane
    // exists to show). Interior edges are left alone: the boundary is what a user reshapes.
    const auto seg_dist_sq = [](const Vec2f &p, const Vec2f &a, const Vec2f &b) {
        const Vec2f ab = b - a;
        const float l2 = ab.squaredNorm();
        const float t  = (l2 > 1e-12f) ? std::clamp((p - a).dot(ab) / l2, 0.f, 1.f) : 0.f;
        return (p - (a + ab * t)).squaredNorm();
    };
    std::pair<int, int> best{ -1, -1 };
    float               best_d = threshold * threshold;
    for (const auto &[a, b] : m_islands.boundary_edges) {
        if (a < 0 || b < 0 || size_t(a) >= m_islands.uvs.size() || size_t(b) >= m_islands.uvs.size())
            continue;
        const float d = seg_dist_sq(uv, island_uv(size_t(a)), island_uv(size_t(b)));
        if (d < best_d) {
            best_d = d;
            best   = { a, b };
        }
    }
    return best;
}

void UVEditorCanvas::move_vertex_raw(int v, const Vec2f &delta_uv)
{
    if (v < 0 || size_t(v) >= m_islands.uvs.size())
        return;
    const int island = m_islands.vertex_island[size_t(v)];
    // The gesture happens in texture-UV space; the stored coordinate is the raw unwrap. Undo the
    // island's own linear map (which includes the layer's tiling scale + rotation) to get there.
    Eigen::Matrix2f lin = Eigen::Matrix2f::Identity();
    if (island >= 0 && size_t(island) < m_transforms.size())
        lin = m_transforms[size_t(island)].block<2, 2>(0, 0);
    const float det = lin.determinant();
    const Vec2f raw_delta = (std::abs(det) > 1e-12f) ? Vec2f(lin.inverse() * delta_uv) : delta_uv;
    m_islands.uvs[size_t(v)] += raw_delta;
    m_mesh_dirty = true; // the edited raw uv is redrawn from rebuild_island_models() next frame
}

float UVEditorCanvas::island_rotation_deg(int island) const
{
    // The rotation baked into the island's affine, in the same y-down UV convention the gesture uses.
    // First column is scale*(cos, sin); its angle is the island's on-screen orientation.
    if (island < 0 || size_t(island) >= m_transforms.size())
        return 0.f;
    const IslandTransform &m = m_transforms[size_t(island)];
    return std::atan2(m(1, 0), m(0, 0)) * 180.f / float(M_PI);
}

Vec2f UVEditorCanvas::island_centroid(int island) const
{
    Vec2f sum   = Vec2f::Zero();
    int   count = 0;
    for (size_t i = 0; i < m_islands.uvs.size(); ++i)
        if (m_islands.vertex_island[i] == island) {
            sum += island_uv(i);
            ++count;
        }
    return (count > 0) ? Vec2f(sum / float(count)) : Vec2f::Zero();
}

Vec2f UVEditorCanvas::uv_delta_to_unwrap(const Vec2f &delta_uv) const
{
    // apply_uv_transform() maps unwrap -> uv as  uv = R(unwrap / tiling) + offset, so the inverse of
    // a *delta* (the translation drops out) is  unwrap = tiling * R^-1(delta_uv).
    const float rad = m_rotation_deg * float(M_PI) / 180.f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    return Vec2f(delta_uv.x() * cs + delta_uv.y() * sn, -delta_uv.x() * sn + delta_uv.y() * cs) * m_tiling_scale;
}

Vec2f UVEditorCanvas::snap_correction(int island) const
{
    if (!m_snap_enabled || island < 0 || size_t(island) >= m_island_boundary_verts.size())
        return Vec2f::Zero();

    // A screen-space threshold, so the magnet feels the same at every zoom level.
    const wxSize size      = GetSize();
    const float  uv_per_px = 2.f * m_zoom / float(std::max(1, std::min(size.GetWidth(), size.GetHeight())));
    const float  threshold = SNAP_PIXELS * uv_per_px;

    float best_dist_sq = threshold * threshold;
    Vec2f best         = Vec2f::Zero();
    for (const int mine : m_island_boundary_verts[size_t(island)]) {
        const Vec2f a = island_uv(size_t(mine));
        for (size_t other = 0; other < m_island_boundary_verts.size(); ++other) {
            if (int(other) == island)
                continue;
            for (const int theirs : m_island_boundary_verts[other]) {
                const Vec2f d       = island_uv(size_t(theirs)) - a;
                const float dist_sq = d.squaredNorm();
                if (dist_sq < best_dist_sq) {
                    best_dist_sq = dist_sq;
                    best         = d;
                }
            }
        }
    }
    return best;
}

std::vector<int> UVEditorCanvas::selected_edge_endpoints() const
{
    std::vector<int> verts;
    for (const auto &[a, b] : m_sel_edges)
        for (const int v : { a, b })
            if (v >= 0 && std::find(verts.begin(), verts.end(), v) == verts.end())
                verts.push_back(v);
    return verts;
}

void UVEditorCanvas::end_gesture()
{
    const bool was_editing = m_gesture == Gesture::MoveIsland || m_gesture == Gesture::RotateIsland ||
                             m_gesture == Gesture::RotateIslandModal || m_gesture == Gesture::ScaleIslandModal;

    // Stick to a neighbour at the end of a move, rather than magnetically fighting the cursor
    // throughout it - a snap that keeps re-applying mid-drag is very hard to pull *out* of.
    if (m_gesture == Gesture::MoveIsland && m_on_island_edit) {
        const Vec2f correction = snap_correction(m_selected_island);
        if (!correction.isZero())
            m_on_island_edit(m_selected_island, uv_delta_to_unwrap(correction), 0.f, 1.f, false);
    }

    if (was_editing && m_on_island_edit)
        m_on_island_edit(m_selected_island, Vec2f::Zero(), 0.f, 1.f, /* finished */ true);

    // Commit a vertex/edge edit once, on release: hand the owner the affected unwrapped vertices and
    // their new raw-unwrap coordinates so it can store the overrides and re-solve the bake preview.
    if ((m_gesture == Gesture::MoveVertex || m_gesture == Gesture::MoveEdge) && m_vertex_edit_moved &&
        m_on_vertex_edit) {
        std::vector<std::pair<int, Vec2f>> edits;
        const auto add = [&](int v) {
            if (v >= 0 && size_t(v) < m_islands.uvs.size())
                edits.emplace_back(v, m_islands.uvs[size_t(v)]);
        };
        // Commit every element of the multi-selection, not just the primary, so a group drag stores all
        // the moved vertices' overrides. Falls back to the primary if the selection is somehow empty.
        if (m_gesture == Gesture::MoveVertex) {
            if (m_sel_vertices.empty())
                add(m_active_vertex);
            else
                for (const int v : m_sel_vertices)
                    add(v);
        } else {
            const std::vector<int> endpoints = selected_edge_endpoints();
            if (endpoints.empty()) {
                add(m_active_edge.first);
                add(m_active_edge.second);
            } else
                for (const int v : endpoints)
                    add(v);
        }
        if (!edits.empty())
            m_on_vertex_edit(edits);
    }

    m_gesture           = Gesture::None;
    m_rot_raw_deg       = 0.f;
    m_rot_applied_deg   = 0.f;
    m_modal_scale_accum = 1.f;
    if (HasCapture())
        ReleaseMouse();
}

void UVEditorCanvas::on_key(wxKeyEvent &evt)
{
    const int key = evt.GetKeyCode();

    // Undo/redo while the pane has focus. Island edits already take a Plater snapshot per gesture (see
    // GLGizmoTextureDisplacement::on_island_edited), so this just drives the same global history; the
    // gizmo re-pushes the restored island transforms into the canvas on the reload that follows.
    if (evt.ControlDown() && (key == 'Z' || key == 'z')) {
        if (evt.ShiftDown()) wxGetApp().plater()->redo();
        else                 wxGetApp().plater()->undo();
        return;
    }
    if (evt.ControlDown() && (key == 'Y' || key == 'y')) {
        wxGetApp().plater()->redo();
        return;
    }

    if (m_gesture == Gesture::RotateIslandModal || m_gesture == Gesture::ScaleIslandModal) {
        if (key == WXK_ESCAPE) {
            // Put the island back exactly where the modal gesture found it, then finish.
            if (m_on_island_edit) {
                if (m_gesture == Gesture::RotateIslandModal && m_rot_applied_deg != 0.f)
                    m_on_island_edit(m_selected_island, Vec2f::Zero(), -m_rot_applied_deg, 1.f, false);
                if (m_gesture == Gesture::ScaleIslandModal && m_modal_scale_accum != 1.f)
                    m_on_island_edit(m_selected_island, Vec2f::Zero(), 0.f, 1.f / m_modal_scale_accum, false);
            }
            end_gesture();
            Refresh();
            return;
        }
        if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
            end_gesture();
            Refresh();
            return;
        }
    }

    // Frame everything. The unwrap is packed in mm and then divided by the layer's tile size, so it
    // can easily sit tens of tiles away from the texture's first one - panning back by hand from
    // there is hopeless, and without this there would be no way to reach reset_view() at all.
    if (key == WXK_HOME || key == 'F' || key == 'f') {
        reset_view();
        return;
    }

    // Blender's modal transforms: R / S, then the transform follows the mouse until it is confirmed
    // with a click or Enter, or abandoned with Esc.
    if (m_selected_island >= 0 && m_select_mode == SelectMode::Island && m_gesture == Gesture::None &&
        (key == 'R' || key == 'r' || key == 'S' || key == 's')) {
        const Vec2f uv     = screen_to_uv(ScreenToClient(wxGetMousePosition()));
        const Vec2f centre = island_centroid(m_selected_island);
        const Vec2f rel    = uv - centre;
        if (key == 'R' || key == 'r') {
            m_gesture            = Gesture::RotateIslandModal;
            m_rot_raw_deg        = 0.f;
            m_rot_applied_deg    = 0.f;
            m_rot_base_deg       = island_rotation_deg(m_selected_island);
            m_rot_display_deg    = m_rot_base_deg;
            m_gesture_last_angle = std::atan2(rel.y(), rel.x());
        } else {
            m_gesture           = Gesture::ScaleIslandModal;
            m_modal_scale_accum = 1.f;
            m_gesture_last_dist = std::max(rel.norm(), 1e-6f);
        }
        return;
    }

    evt.Skip();
}

void UVEditorCanvas::on_mouse(wxMouseEvent &evt)
{
    const wxEventType type = evt.GetEventType();
    const wxPoint     pos  = evt.GetPosition();

    // Track the pointer so the +/- add-remove hint can be drawn next to it in Vertex/Edge mode.
    m_cursor_px     = pos;
    m_cursor_inside = true;
    if (type == wxEVT_MOTION && m_select_mode != SelectMode::Island && m_gesture == Gesture::None)
        Refresh(); // animate the hint (and its +/- flip) as the pointer/modifiers move

    // Key events (R/S/Home) only arrive if this canvas has focus, and clicking it is the natural way
    // to ask for it - the pane is not in the tab order.
    if (type == wxEVT_LEFT_DOWN || type == wxEVT_RIGHT_DOWN || type == wxEVT_MIDDLE_DOWN)
        SetFocus();

    // A modal R/S is confirmed by any click, exactly as in Blender.
    if ((m_gesture == Gesture::RotateIslandModal || m_gesture == Gesture::ScaleIslandModal) &&
        (type == wxEVT_LEFT_DOWN || type == wxEVT_RIGHT_DOWN)) {
        end_gesture();
        Refresh();
        return;
    }

    if (type == wxEVT_LEFT_DOWN) {
        const Vec2f uv    = screen_to_uv(pos);
        m_drag_last_px    = pos;
        m_gesture_last_uv = uv;
        if (m_select_mode == SelectMode::Vertex) {
            const int hit       = vertex_at(uv);
            m_active_edge       = { -1, -1 };
            m_vertex_edit_moved = false;
            // Same Shift-adds / Ctrl-toggles / plain-replaces rules as island selection, so a group of
            // vertices can be picked and dragged together.
            if (hit >= 0) {
                if (evt.ShiftDown()) {
                    if (!is_vertex_selected(hit))
                        m_sel_vertices.push_back(hit);
                } else if (evt.ControlDown()) {
                    if (auto it = std::find(m_sel_vertices.begin(), m_sel_vertices.end(), hit); it != m_sel_vertices.end())
                        m_sel_vertices.erase(it);
                    else
                        m_sel_vertices.push_back(hit);
                } else if (!is_vertex_selected(hit)) {
                    m_sel_vertices.assign(1, hit);
                }
            } else if (!evt.ShiftDown() && !evt.ControlDown()) {
                m_sel_vertices.clear();
            }
            // Primary = the clicked vertex only if it is (still) selected; a Ctrl-deselect just toggles.
            m_active_vertex = (hit >= 0 && is_vertex_selected(hit)) ? hit : -1;
            m_gesture       = (m_active_vertex >= 0) ? Gesture::MoveVertex : Gesture::Pan;
        } else if (m_select_mode == SelectMode::Edge) {
            const std::pair<int, int> hit = edge_at(uv);
            m_active_vertex     = -1;
            m_vertex_edit_moved = false;
            if (hit.first >= 0) {
                if (evt.ShiftDown()) {
                    if (!is_edge_selected(hit))
                        m_sel_edges.push_back(hit);
                } else if (evt.ControlDown()) {
                    if (auto it = std::find(m_sel_edges.begin(), m_sel_edges.end(), hit); it != m_sel_edges.end())
                        m_sel_edges.erase(it);
                    else
                        m_sel_edges.push_back(hit);
                } else if (!is_edge_selected(hit)) {
                    m_sel_edges.assign(1, hit);
                }
            } else if (!evt.ShiftDown() && !evt.ControlDown()) {
                m_sel_edges.clear();
            }
            m_active_edge = (hit.first >= 0 && is_edge_selected(hit)) ? hit : std::pair<int, int>{ -1, -1 };
            m_gesture     = (m_active_edge.first >= 0) ? Gesture::MoveEdge : Gesture::Pan;
        } else {
            const int hit = island_at(uv);
            if (hit >= 0) {
                if (evt.ShiftDown()) {
                    // Shift adds to the selection (and makes the clicked one the new primary).
                    if (!is_selected(hit))
                        m_selection.push_back(hit);
                    m_selected_island = hit;
                } else if (evt.ControlDown()) {
                    // Ctrl toggles: clicking a selected island removes it (the requested "deselect"),
                    // clicking an unselected one adds it.
                    if (auto it = std::find(m_selection.begin(), m_selection.end(), hit); it != m_selection.end()) {
                        m_selection.erase(it);
                        m_selected_island = m_selection.empty() ? -1 : m_selection.back();
                    } else {
                        m_selection.push_back(hit);
                        m_selected_island = hit;
                    }
                } else {
                    // Plain click: keep the whole selection if the clicked island is already part of it (so
                    // a drag moves the group), otherwise collapse to just this one.
                    if (!is_selected(hit))
                        m_selection.assign(1, hit);
                    m_selected_island = hit;
                }
            } else if (!evt.ShiftDown() && !evt.ControlDown()) {
                // Clicking empty space with no modifier clears the selection and pans.
                m_selection.clear();
                m_selected_island = -1;
            }
            m_gesture = (m_selected_island >= 0) ? Gesture::MoveIsland : Gesture::Pan;
        }
        CaptureMouse();
        Refresh();
    } else if (type == wxEVT_RIGHT_DOWN && m_selected_island >= 0 && m_select_mode == SelectMode::Island) {
        const Vec2f rel      = screen_to_uv(pos) - island_centroid(m_selected_island);
        m_gesture            = Gesture::RotateIsland;
        m_rot_raw_deg        = 0.f;
        m_rot_applied_deg    = 0.f;
        m_rot_base_deg       = island_rotation_deg(m_selected_island);
        m_rot_display_deg    = m_rot_base_deg;
        m_gesture_last_angle = std::atan2(rel.y(), rel.x());
        CaptureMouse();
    } else if (type == wxEVT_MIDDLE_DOWN) {
        m_gesture      = Gesture::Pan;
        m_drag_last_px = pos;
        CaptureMouse();
    } else if (type == wxEVT_LEFT_UP || type == wxEVT_RIGHT_UP || type == wxEVT_MIDDLE_UP) {
        if (m_gesture != Gesture::RotateIslandModal && m_gesture != Gesture::ScaleIslandModal) {
            end_gesture();
            Refresh();
        }
    } else if (type == wxEVT_MOTION) {
        switch (m_gesture) {
        case Gesture::Pan: {
            const wxSize size  = GetSize();
            const float  scale = 2.f * m_zoom / float(std::max(1, std::min(size.GetWidth(), size.GetHeight())));
            // Both axes point the same way on screen as in UV space (v runs down), so dragging the
            // content along with the cursor is a subtraction on both.
            m_pan.x() -= float(pos.x - m_drag_last_px.x) * scale;
            m_pan.y() -= float(pos.y - m_drag_last_px.y) * scale;
            m_drag_last_px = pos;
            Refresh();
            break;
        }
        case Gesture::MoveIsland: {
            const Vec2f uv = screen_to_uv(pos);
            if (m_on_island_edit)
                m_on_island_edit(m_selected_island, uv_delta_to_unwrap(uv - m_gesture_last_uv), 0.f, 1.f, false);
            m_gesture_last_uv = uv;
            Refresh();
            break;
        }
        case Gesture::MoveVertex: {
            const Vec2f uv    = screen_to_uv(pos);
            const Vec2f delta = uv - m_gesture_last_uv;
            // Move the whole selection by the same uv-space delta (each vertex converts it through its
            // own island transform in move_vertex_raw). Falls back to the primary if none is selected.
            if (m_sel_vertices.empty())
                move_vertex_raw(m_active_vertex, delta);
            else
                for (const int v : m_sel_vertices)
                    move_vertex_raw(v, delta);
            m_gesture_last_uv   = uv;
            m_vertex_edit_moved = true;
            Refresh();
            break;
        }
        case Gesture::MoveEdge: {
            const Vec2f uv    = screen_to_uv(pos);
            const Vec2f delta = uv - m_gesture_last_uv;
            // Move every unique endpoint of every selected edge once. Falls back to the primary edge.
            const std::vector<int> endpoints = selected_edge_endpoints();
            if (endpoints.empty()) {
                move_vertex_raw(m_active_edge.first, delta);
                move_vertex_raw(m_active_edge.second, delta);
            } else
                for (const int v : endpoints)
                    move_vertex_raw(v, delta);
            m_gesture_last_uv   = uv;
            m_vertex_edit_moved = true;
            Refresh();
            break;
        }
        case Gesture::RotateIsland:
        case Gesture::RotateIslandModal: {
            const Vec2f rel   = screen_to_uv(pos) - island_centroid(m_selected_island);
            const float angle = std::atan2(rel.y(), rel.x());
            // Accumulate the raw mouse rotation incrementally so it survives crossing +/-180 degrees.
            m_rot_raw_deg += angle_delta(m_gesture_last_angle, angle) * 180.f / float(M_PI);
            m_gesture_last_angle = angle;

            // With Shift, quantise to *global* 15-degree marks (0/15/30...), i.e. snap the island's
            // absolute on-screen orientation, not 15 degrees relative to wherever it started (#10) -
            // snapping the target rather than each delta is what keeps it from juddering on a step.
            constexpr float STEP       = 15.f;
            const float     absolute   = m_rot_base_deg + m_rot_raw_deg;
            const float     target_abs = evt.ShiftDown() ? std::round(absolute / STEP) * STEP : absolute;
            const float     deg        = target_abs - (m_rot_base_deg + m_rot_applied_deg);
            if (m_on_island_edit && deg != 0.f)
                m_on_island_edit(m_selected_island, Vec2f::Zero(), deg, 1.f, false);
            m_rot_applied_deg = target_abs - m_rot_base_deg;
            m_rot_display_deg = target_abs;
            Refresh();
            break;
        }
        case Gesture::ScaleIslandModal: {
            const Vec2f rel  = screen_to_uv(pos) - island_centroid(m_selected_island);
            const float dist = std::max(rel.norm(), 1e-6f);
            const float factor = dist / m_gesture_last_dist;
            if (m_on_island_edit && factor != 1.f)
                m_on_island_edit(m_selected_island, Vec2f::Zero(), 0.f, factor, false);
            m_modal_scale_accum *= factor;
            m_gesture_last_dist = dist;
            Refresh();
            break;
        }
        default: break;
        }
    } else if (type == wxEVT_MOUSEWHEEL) {
        // Zoom about the cursor, not the view centre - otherwise zooming in on something off to the
        // side walks it straight out of the frame.
        const Vec2f before = screen_to_uv(pos);
        const float factor = std::pow(0.9f, float(evt.GetWheelRotation()) / float(evt.GetWheelDelta()));
        m_zoom             = std::clamp(m_zoom * factor, 0.001f, 5000.f);
        const Vec2f after  = screen_to_uv(pos);
        m_pan += before - after;
        Refresh();
    }
}

void UVEditorCanvas::rebuild_island_models()
{
    m_mesh_dirty = false;
    m_island_wireframe.clear();
    m_island_boundary.clear();
    m_island_fill.clear();

    const int islands = std::max(m_islands.island_count, 0);
    if (islands == 0 || m_islands.uvs.empty() || m_islands.indices.empty())
        return;

    m_island_wireframe.resize(size_t(islands));
    m_island_boundary.resize(size_t(islands));
    m_island_fill.resize(size_t(islands));

    const int vertex_count = int(m_islands.uvs.size());

    // Charts have disjoint vertex sets (compute_patch_unwrap() duplicates seam vertices per chart),
    // so every vertex belongs to exactly one island and this partition is clean.
    std::unordered_map<uint64_t, bool> is_boundary;
    is_boundary.reserve(m_islands.boundary_edges.size() * 2);
    for (const auto &[a, b] : m_islands.boundary_edges)
        is_boundary[undirected_edge_key(a, b)] = true;

    // Named, not size_t(islands) inline: the latter is a most-vexing-parse and declares a function
    // (a single identifier in the parens reads as a parameter name), which is what every
    // "subscript requires array or pointer type" error on these vectors was.
    const size_t n_islands = size_t(islands);

    // Global vertex index -> index within its own island's buffers.
    std::vector<int> local(m_islands.uvs.size(), -1);
    std::vector<std::vector<Vec2f>> island_verts(n_islands);
    for (size_t i = 0; i < m_islands.uvs.size(); ++i) {
        const int island = m_islands.vertex_island[i];
        if (island < 0 || island >= islands)
            continue;
        local[i] = int(island_verts[size_t(island)].size());
        island_verts[size_t(island)].push_back(m_islands.uvs[i]);
    }

    std::vector<GLModel::Geometry> wire(n_islands), outline(n_islands), fill(n_islands);
    for (int c = 0; c < islands; ++c) {
        for (GLModel::Geometry *g : { &wire[size_t(c)], &outline[size_t(c)] })
            g->format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        fill[size_t(c)].format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        for (GLModel::Geometry *g : { &wire[size_t(c)], &outline[size_t(c)], &fill[size_t(c)] }) {
            g->reserve_vertices(island_verts[size_t(c)].size());
            for (const Vec2f &uv : island_verts[size_t(c)])
                g->add_vertex(Vec3f(uv.x(), uv.y(), 0.f));
        }
    }

    for (const Vec3i32 &tri : m_islands.indices) {
        if (tri.minCoeff() < 0 || tri.maxCoeff() >= vertex_count)
            continue;
        const int island = m_islands.vertex_island[size_t(tri[0])];
        if (island < 0 || island >= islands)
            continue;

        fill[size_t(island)].add_triangle(unsigned(local[size_t(tri[0])]), unsigned(local[size_t(tri[1])]),
                                          unsigned(local[size_t(tri[2])]));
        // Interior edges only: the island outline is drawn separately, brighter and on top, so drawing
        // it here as well would just dim it by blending against itself.
        for (int i = 0; i < 3; ++i) {
            const int a = tri[i], b = tri[(i + 1) % 3];
            if (!is_boundary.count(undirected_edge_key(a, b)))
                wire[size_t(island)].add_line(unsigned(local[size_t(a)]), unsigned(local[size_t(b)]));
        }
    }

    for (const auto &[a, b] : m_islands.boundary_edges) {
        if (a < 0 || b < 0 || a >= vertex_count || b >= vertex_count)
            continue;
        const int island = m_islands.vertex_island[size_t(a)];
        if (island < 0 || island >= islands)
            continue;
        outline[size_t(island)].add_line(unsigned(local[size_t(a)]), unsigned(local[size_t(b)]));
    }

    for (int c = 0; c < islands; ++c) {
        if (!wire[size_t(c)].is_empty())
            m_island_wireframe[size_t(c)].init_from(std::move(wire[size_t(c)]));
        if (!outline[size_t(c)].is_empty())
            m_island_boundary[size_t(c)].init_from(std::move(outline[size_t(c)]));
        if (!fill[size_t(c)].is_empty())
            m_island_fill[size_t(c)].init_from(std::move(fill[size_t(c)]));
    }
}

void UVEditorCanvas::rebuild_grid()
{
    // One line per UV unit (i.e. per texture tile), plus a subdivision when zoomed in far enough that
    // whole tiles would be too coarse to read.
    const float span = 2.f * m_zoom;
    float       step = 1.f;
    while (step > 0.01f && span / step > 40.f)
        step *= 2.f;
    while (span / step < 4.f)
        step *= 0.5f;

    // Cover exactly the currently-visible view rectangle (plus a one-step margin), recomputed on every
    // render. The old version only rebuilt when `step` changed, so panning at a fixed zoom left the grid
    // frozen at wherever it was last built - which is the "not in whole area / not always rendered while
    // moving" the user saw. The grid is only a few dozen lines, so rebuilding it per frame is cheap.
    float half_w, half_h;
    view_half_extents(half_w, half_h);
    const Vec2f lo = m_pan - Vec2f(half_w, half_h) - Vec2f(step, step);
    const Vec2f hi = m_pan + Vec2f(half_w, half_h) + Vec2f(step, step);
    m_grid_step    = step;

    const int first_x = int(std::floor(lo.x() / step)), last_x = int(std::ceil(hi.x() / step));
    const int first_y = int(std::floor(lo.y() / step)), last_y = int(std::ceil(hi.y() / step));
    if (last_x - first_x > 4000 || last_y - first_y > 4000)
        return; // degenerate zoom; not worth drawing a grid nobody can see

    GLModel::Geometry grid;
    grid.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    unsigned index = 0;
    const auto line = [&](const Vec2f &a, const Vec2f &b) {
        grid.add_vertex(Vec3f(a.x(), a.y(), 0.f));
        grid.add_vertex(Vec3f(b.x(), b.y(), 0.f));
        grid.add_line(index, index + 1);
        index += 2;
    };
    for (int i = first_x; i <= last_x; ++i)
        line(Vec2f(float(i) * step, lo.y()), Vec2f(float(i) * step, hi.y()));
    for (int i = first_y; i <= last_y; ++i)
        line(Vec2f(lo.x(), float(i) * step), Vec2f(hi.x(), float(i) * step));

    m_grid_glmodel.reset();
    if (!grid.is_empty())
        m_grid_glmodel.init_from(std::move(grid));
}

void UVEditorCanvas::rebuild_rotation_dial()
{
    m_dial_glmodel.reset();
    const bool rotating = (m_gesture == Gesture::RotateIsland || m_gesture == Gesture::RotateIslandModal);
    if (!rotating || m_selected_island < 0)
        return;

    const wxSize size      = GetSize();
    const float  uv_per_px = 2.f * m_zoom / float(std::max(1, std::min(size.GetWidth(), size.GetHeight())));
    const float  radius    = 90.f * uv_per_px; // ~constant on-screen size regardless of zoom
    const Vec2f  centre    = island_centroid(m_selected_island);

    GLModel::Geometry dial;
    dial.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    unsigned index = 0;
    const auto line = [&](const Vec2f &a, const Vec2f &b) {
        dial.add_vertex(Vec3f(a.x(), a.y(), 0.f));
        dial.add_vertex(Vec3f(b.x(), b.y(), 0.f));
        dial.add_line(index, index + 1);
        index += 2;
    };
    const auto on_ring = [&](float deg, float r) {
        const float a = deg * float(M_PI) / 180.f;
        return centre + r * Vec2f(std::cos(a), std::sin(a));
    };

    // The ring itself.
    constexpr int SEG = 72;
    for (int i = 0; i < SEG; ++i)
        line(on_ring(float(i) * 360.f / SEG, radius), on_ring(float(i + 1) * 360.f / SEG, radius));
    // A tick every 15 degrees (the snap marks), longer on the cardinals.
    for (int d = 0; d < 360; d += 15) {
        const bool cardinal = (d % 90) == 0;
        line(on_ring(float(d), radius * (cardinal ? 0.80f : 0.90f)), on_ring(float(d), radius * (cardinal ? 1.08f : 1.0f)));
    }
    // The needle at the island's current absolute angle.
    line(centre, on_ring(m_rot_display_deg, radius * 1.12f));

    if (!dial.is_empty())
        m_dial_glmodel.init_from(std::move(dial));
}

void UVEditorCanvas::rebuild_background_quad()
{
    m_background_glmodel.reset();
    m_background_quad_dirty = false;
    if (m_background_width <= 0 || m_background_height <= 0)
        return;

    Vec2f lo, hi;
    content_bounds(lo, hi);
    if (m_tile_enabled) {
        // Snap out to whole tiles. Only cosmetic, but it keeps the backdrop's edge on a tile
        // boundary instead of slicing a brick in half at an arbitrary place.
        lo = Vec2f(std::floor(lo.x()), std::floor(lo.y()));
        hi = Vec2f(std::ceil(hi.x()), std::ceil(hi.y()));
    } else {
        // Tiling off: the sampler reads 0 outside the first tile and nothing else exists, so the
        // backdrop is exactly that one tile (GL_CLAMP_TO_BORDER in render() gives it the same
        // black surround, rather than smearing the edge texels outward).
        lo = Vec2f(0.f, 0.f);
        hi = Vec2f(1.f, 1.f);
    }

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3T2 };
    init_data.reserve_vertices(4);
    init_data.reserve_indices(6);
    // Texcoord == position: the height sampling maps one whole texture onto each unit square of uv
    // (see DecodedHeightTexture::sample()), so uv *is* the texture coordinate. That also puts the
    // texture's first pixel row at v = 0, exactly where sample() reads it.
    init_data.add_vertex(Vec3f(lo.x(), lo.y(), 0.f), Vec2f(lo.x(), lo.y()));
    init_data.add_vertex(Vec3f(hi.x(), lo.y(), 0.f), Vec2f(hi.x(), lo.y()));
    init_data.add_vertex(Vec3f(hi.x(), hi.y(), 0.f), Vec2f(hi.x(), hi.y()));
    init_data.add_vertex(Vec3f(lo.x(), hi.y(), 0.f), Vec2f(lo.x(), hi.y()));
    init_data.add_triangle(0, 1, 2);
    init_data.add_triangle(0, 2, 3);
    m_background_glmodel.init_from(std::move(init_data));
}

void UVEditorCanvas::rebuild_background_texture()
{
    m_background_texture.reset();
    m_background_dirty = false;
    if (m_background_width > 0 && m_background_height > 0)
        m_background_texture.load_from_raw_data(m_background_pixels, (unsigned int) m_background_width,
                                                 (unsigned int) m_background_height, false);
    // The quad's extent doesn't depend on the pixels, but it does have to exist alongside them.
    m_background_quad_dirty = true;
}

void UVEditorCanvas::on_paint(wxPaintEvent & /*evt*/)
{
    wxPaintDC dc(this);
    render();
}

void UVEditorCanvas::on_size(wxSizeEvent &evt)
{
    evt.Skip();
    // Refresh() alone only *schedules* a repaint, which Windows can coalesce/delay until a live
    // resize drag ends, leaving stale wrong-aspect-ratio content on screen throughout the drag.
    // Update() flushes it immediately - still through the normal paint path (unlike calling
    // render() directly), which matters because this canvas shares the app's one GL context with
    // the 3D view and must not hijack it outside its own paint.
    Refresh();
    Update();
}

void UVEditorCanvas::on_leave(wxMouseEvent &evt)
{
    evt.Skip();
    if (m_cursor_inside) {
        m_cursor_inside = false;
        if (m_select_mode != SelectMode::Island)
            Refresh(); // the +/- hint was following the cursor; drop it now the pointer is gone
    }
}

void UVEditorCanvas::render()
{
    if (m_context == nullptr || !IsShownOnScreen())
        return;

    // wxGLCanvas::SetCurrent() genuinely fails (returns false) on a canvas that isn't shown on
    // screen, and can also fail on a pixel-format mismatch with the context - and every GL call
    // made afterwards would then run against whichever context *is* current, which here is the main
    // 3D canvas mid-frame. Bail instead of corrupting it.
    if (!SetCurrent(*m_context))
        return;

    if (m_mesh_dirty)
        rebuild_island_models();
    if (m_background_dirty)
        rebuild_background_texture();
    if (m_background_quad_dirty)
        rebuild_background_quad();
    if (m_needs_fit)
        fit_view_to_content();
    rebuild_grid();
    rebuild_rotation_dial();

    const wxSize size = GetSize();
    glsafe(::glViewport(0, 0, size.GetWidth(), size.GetHeight()));
    glsafe(::glClearColor(UV_COLOR_BG.r(), UV_COLOR_BG.g(), UV_COLOR_BG.b(), 1.f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    // Orthographic, centered on m_pan. Y is negated so v runs down the screen, which is what puts
    // the texture's first pixel row (v = 0, see rebuild_background_quad()) at the top rather than
    // upside down.
    float half_w, half_h;
    view_half_extents(half_w, half_h);

    Transform3d projection_matrix = Transform3d::Identity();
    projection_matrix(0, 0)       =  1.0 / std::max(double(half_w), 1e-6);
    projection_matrix(1, 1)       = -1.0 / std::max(double(half_h), 1e-6);
    Transform3d view_matrix       = Transform3d::Identity();
    view_matrix.translate(Vec3d(-double(m_pan.x()), -double(m_pan.y()), 0.0));

    const auto draw = [&](GLModel &model, const ColorRGBA &color, const Transform3d &model_matrix) {
        if (!model.is_initialized())
            return;
        GLShaderProgram *shader = wxGetApp().get_shader("flat");
        if (shader == nullptr)
            return;
        shader->start_using();
        shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        shader->set_uniform("projection_matrix", projection_matrix);
        model.set_color(color);
        model.render();
        shader->stop_using();
    };
    const Transform3d identity = Transform3d::Identity();

    if (m_background_glmodel.is_initialized()) {
        if (GLShaderProgram *shader = wxGetApp().get_shader("flat_texture")) {
            shader->start_using();
            shader->set_uniform("view_model_matrix", view_matrix);
            shader->set_uniform("projection_matrix", projection_matrix);
            glsafe(::glActiveTexture(GL_TEXTURE0));
            glsafe(::glBindTexture(GL_TEXTURE_2D, m_background_texture.get_id()));
            // Repeat exactly the way DecodedHeightTexture::sample() does, so the backdrop under an
            // island really is the texels that island will sample. With tiling off, sample() returns
            // 0 outside the first tile - a black border, not a smeared edge, which is what
            // CLAMP_TO_BORDER reproduces (GLTexture's own default of GL_REPEAT would not).
            const GLint wrap = m_tile_enabled ? (m_tile_mirrored ? GL_MIRRORED_REPEAT : GL_REPEAT) : GL_CLAMP_TO_BORDER;
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap));
            if (!m_tile_enabled) {
                constexpr GLfloat border[4] = { 0.f, 0.f, 0.f, 1.f };
                glsafe(::glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border));
            }
            m_background_glmodel.render();
            glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
            shader->stop_using();
        }
    }

    set_line_width(1.f);
    draw(m_grid_glmodel, UV_COLOR_GRID, identity);

    // The texture's first tile. Always drawn, even with nothing painted, so the pane always has a
    // fixed landmark: the unwrap is packed in mm and divided by the tile size, so it is routinely
    // many tiles away from here, and without this there is no way to tell "the islands are somewhere
    // else" apart from "there are no islands".
    if (!m_tile_outline_glmodel.is_initialized()) {
        GLModel::Geometry tile;
        tile.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        tile.reserve_vertices(4);
        tile.reserve_indices(8);
        tile.add_vertex(Vec3f(0.f, 0.f, 0.f));
        tile.add_vertex(Vec3f(1.f, 0.f, 0.f));
        tile.add_vertex(Vec3f(1.f, 1.f, 0.f));
        tile.add_vertex(Vec3f(0.f, 1.f, 0.f));
        for (unsigned i = 0; i < 4; ++i)
            tile.add_line(i, (i + 1) % 4);
        m_tile_outline_glmodel.init_from(std::move(tile));
    }
    set_line_width(2.f);
    draw(m_tile_outline_glmodel, UV_COLOR_TILE_OUTLINE, identity);

    const auto island_matrix = [this, &identity](int c) {
        return (size_t(c) < m_transforms.size()) ? to_transform3d(m_transforms[size_t(c)]) : identity;
    };

    // Island fills first, as a translucent wash under the wires: every island gets a faint one so it
    // reads as a solid patch rather than a wire cage, and the selected one gets the accent teal (#7).
    // When a distortion heatmap has been supplied (set_island_fill_colors), unselected islands take
    // their heatmap colour instead of the default wash; the selected one still wins its highlight.
    for (int c = 0; c < int(m_island_fill.size()); ++c) {
        ColorRGBA fill = UV_COLOR_FILL;
        if (size_t(c) < m_island_fill_colors.size())
            fill = m_island_fill_colors[size_t(c)];
        draw(m_island_fill[size_t(c)], is_selected(c) ? UV_COLOR_SEL_FILL : fill, island_matrix(c));
    }

    set_line_width(1.f);
    for (int c = 0; c < int(m_island_wireframe.size()); ++c)
        draw(m_island_wireframe[size_t(c)], is_selected(c) ? UV_COLOR_SEL_WIRE : UV_COLOR_WIRE, island_matrix(c));

    // The island outlines - i.e. exactly the edges the seam angle cut the patch along. Drawn last
    // and brightest, because "where does one island end and the next begin" is the single thing this
    // pane exists to answer. The selected one gets a bold edge (#7).
    for (int c = 0; c < int(m_island_boundary.size()); ++c) {
        const bool selected = is_selected(c);
        set_line_width(selected ? 3.f : 1.5f);
        draw(m_island_boundary[size_t(c)], selected ? UV_COLOR_SEL_BOUNDARY : UV_COLOR_BOUNDARY, island_matrix(c));
    }
    set_line_width(1.f);

    // The rotation protractor, on top of everything while a rotation gesture is live (#11).
    if (m_dial_glmodel.is_initialized()) {
        set_line_width(2.f);
        draw(m_dial_glmodel, UV_COLOR_DIAL, identity);
        set_line_width(1.f);
    }

    // Vertex/Edge mode handles: a small square drawn over the picked vertex (or each endpoint of the
    // picked edge), so it is obvious which sub-element a drag will move. The already-drawn boundary
    // sits between an edge's two markers, so the pair reads as "this edge".
    if (m_select_mode != SelectMode::Island &&
        (!m_sel_vertices.empty() || !m_sel_edges.empty() || m_active_vertex >= 0 || m_active_edge.first >= 0)) {
        if (!m_vertex_marker_glmodel.is_initialized()) {
            GLModel::Geometry q;
            q.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
            q.reserve_vertices(4);
            q.reserve_indices(6);
            q.add_vertex(Vec3f(-0.5f, -0.5f, 0.f));
            q.add_vertex(Vec3f(0.5f, -0.5f, 0.f));
            q.add_vertex(Vec3f(0.5f, 0.5f, 0.f));
            q.add_vertex(Vec3f(-0.5f, 0.5f, 0.f));
            q.add_triangle(0, 1, 2);
            q.add_triangle(0, 2, 3);
            m_vertex_marker_glmodel.init_from(std::move(q));
        }
        const float uv_per_px = 2.f * m_zoom / float(std::max(1, std::min(size.GetWidth(), size.GetHeight())));
        const float marker    = uv_per_px * 10.f; // ~10 px square, constant on screen at any zoom
        const auto  draw_marker = [&](int v) {
            if (v < 0 || size_t(v) >= m_islands.uvs.size())
                return;
            const Vec2f  p = island_uv(size_t(v));
            Transform3d  m = Transform3d::Identity();
            m.translate(Vec3d(double(p.x()), double(p.y()), 0.0));
            m.scale(double(marker));
            draw(m_vertex_marker_glmodel, UV_COLOR_SEL_BOUNDARY, m);
        };
        // Mark every element of the multi-selection (falling back to the primary when nothing is in the
        // set yet), so a Shift/Ctrl group is all visibly highlighted.
        if (m_select_mode == SelectMode::Vertex) {
            if (m_sel_vertices.empty())
                draw_marker(m_active_vertex);
            else
                for (const int v : m_sel_vertices)
                    draw_marker(v);
        } else {
            if (m_sel_edges.empty()) {
                draw_marker(m_active_edge.first);
                draw_marker(m_active_edge.second);
            } else
                for (const auto &[a, b] : m_sel_edges) {
                    draw_marker(a);
                    draw_marker(b);
                }
        }
    }

    // Add/remove hint next to the cursor in Vertex/Edge mode: a green '+' when a click will add to the
    // selection (plain or Shift), a red '-' when Ctrl is held and a click will remove one - the UV-side
    // twin of the 3D paint cursor's own sign. Rebuilt each frame at the pointer, sized in pixels.
    if (m_select_mode != SelectMode::Island && m_cursor_inside) {
        const float uv_per_px = 2.f * m_zoom / float(std::max(1, std::min(size.GetWidth(), size.GetHeight())));
        const Vec2f centre    = screen_to_uv(m_cursor_px) + Vec2f(14.f, -14.f) * uv_per_px; // up-right of the pointer
        const float r         = 6.f * uv_per_px;
        const bool  removing  = wxGetKeyState(WXK_CONTROL);

        GLModel::Geometry sign;
        sign.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        unsigned   idx = 0;
        const auto seg = [&](const Vec2f &a, const Vec2f &b) {
            sign.add_vertex(Vec3f(a.x(), a.y(), 0.f));
            sign.add_vertex(Vec3f(b.x(), b.y(), 0.f));
            sign.add_line(idx, idx + 1);
            idx += 2;
        };
        seg(centre - Vec2f(r, 0.f), centre + Vec2f(r, 0.f)); // the '-' bar, shared by both signs
        if (!removing)
            seg(centre - Vec2f(0.f, r), centre + Vec2f(0.f, r)); // the extra stroke that makes it a '+'

        m_cursor_sign_glmodel.reset();
        if (!sign.is_empty())
            m_cursor_sign_glmodel.init_from(std::move(sign));
        set_line_width(3.f);
        draw(m_cursor_sign_glmodel, removing ? ColorRGBA(1.f, 0.30f, 0.25f, 0.95f) : ColorRGBA(0.35f, 0.90f, 0.45f, 0.95f),
             identity);
        set_line_width(1.f);
    }

    // The GL context is shared with the 3D view; leave the bits we touched as we found them.
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    SwapBuffers();

    // Cheap, and render() runs on every gesture change (each Refresh), so the status line stays
    // current without threading update_status() through every mouse/key transition. The panel
    // guards against relayout when the text is unchanged.
    update_status();
}

// -----------------------------------------------
// UVEditorPanel
// -----------------------------------------------

namespace {
enum : int {
    ID_UV_FRAME = wxID_HIGHEST + 4200,
    ID_UV_SNAP,
    ID_UV_AVG_SCALE,
    ID_UV_CUT,
    ID_UV_PROJECT,
    ID_UV_JOIN,
    ID_UV_UNJOIN,
};
} // namespace

UVEditorPanel::UVEditorPanel(wxWindow *parent) : wxPanel(parent, wxID_ANY)
{
    // Icon + label buttons: each has its own dedicated SVG (see the map below), with the label kept
    // alongside it. A missing SVG simply leaves the button showing only its label - never bitmap-less
    // garbage - so the bar stays usable before the art lands.
    auto *bar = new wxBoxSizer(wxHORIZONTAL);
    const auto set_icon = [this](wxAnyButton *b, const std::string &iconname) {
        const wxBitmap bmp = create_scaled_bitmap(iconname, this, 16);
        if (bmp.IsOk())
            b->SetBitmap(bmp);
    };
    const auto add_button = [&](int id, const std::string &iconname, const wxString &label, const wxString &tip) {
        auto *b = new wxButton(this, id, label, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        set_icon(b, iconname);
        b->SetToolTip(tip);
        bar->Add(b, 0, wxALL, 2);
        b->Bind(wxEVT_BUTTON, &UVEditorPanel::on_tool, this);
        return b;
    };
    add_button(ID_UV_FRAME, "texture_displacement_frame", _L("Frame"), _L("Frame all islands (Home)"));
    m_snap_button = new wxToggleButton(this, ID_UV_SNAP, _L("Snap"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    set_icon(m_snap_button, "texture_displacement_snap");
    m_snap_button->SetToolTip(_L("Snap islands together when dragging"));
    bar->Add(m_snap_button, 0, wxALL, 2);
    m_snap_button->Bind(wxEVT_TOGGLEBUTTON, &UVEditorPanel::on_tool, this);
    add_button(ID_UV_AVG_SCALE, "texture_displacement_avg_scale", _L("Avg scale"), _L("Give every island the same texel density"));
    add_button(ID_UV_CUT, "texture_displacement_cut", _L("Cut"), _L("Split the selected island across its long axis"));
    add_button(ID_UV_JOIN, "texture_displacement_join", _L("Join"), _L("Unfold the selected island onto its nearest neighbour along their shared edge"));
    add_button(ID_UV_UNJOIN, "texture_displacement_join", _L("Unjoin"), _L("Send the selected island back to its own packed position"));

    m_canvas = new UVEditorCanvas(this);
    m_canvas->set_status_callback([this](const wxString &text) {
        if (m_status != nullptr && m_status->GetLabel() != text) {
            m_status->SetLabel(text);
            m_status->Refresh();
        }
    });

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_status->SetForegroundColour(wxColour(180, 180, 180));

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(bar, 0, wxEXPAND);
    sizer->Add(m_canvas, 1, wxEXPAND);
    sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 3);
    SetSizer(sizer);
}

void UVEditorPanel::on_tool(wxCommandEvent &evt)
{
    switch (evt.GetId()) {
    case ID_UV_FRAME:     m_canvas->run_command(UVEditorCanvas::Command::FrameAll); break;
    case ID_UV_SNAP:      m_canvas->run_command(UVEditorCanvas::Command::ToggleSnap); break;
    case ID_UV_AVG_SCALE: m_canvas->run_command(UVEditorCanvas::Command::AverageScale); break;
    case ID_UV_CUT:       m_canvas->run_command(UVEditorCanvas::Command::CutSelectedIsland); break;
    case ID_UV_JOIN:      m_canvas->run_command(UVEditorCanvas::Command::JoinSelected); break;
    case ID_UV_UNJOIN:    m_canvas->run_command(UVEditorCanvas::Command::UnjoinSelected); break;
    default:              evt.Skip(); return;
    }
    // Keep the toggle button's visual state in step with the canvas, which owns the flag.
    if (m_snap_button != nullptr)
        m_snap_button->SetValue(m_canvas->snap_enabled());
}

} // namespace Slic3r::GUI

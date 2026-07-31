#include "DesignCanvas.hpp"

#include "SketchInlineEditor.hpp"
#include "GLCanvas3D.hpp"
#include "OpenGLManager.hpp"
#include "3DBed.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "3DScene.hpp"
#include "MeshUtils.hpp"   // ClippingPlane (section view)
#include "libslic3r/Config.hpp"
#include <boost/algorithm/string/predicate.hpp>

#include <wx/glcanvas.h>
#include <wx/sizer.h>
#include <wx/frame.h>
#include <wx/stattext.h>
#include <wx/toplevel.h>

namespace Slic3r {
namespace GUI {

DesignCanvas::DesignCanvas(wxWindow* parent)
    : wxPanel()
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0))
        return;

    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this);
    if (m_canvas_widget == nullptr)
        return;

    m_canvas = new GLCanvas3D(m_canvas_widget, m_bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));
    m_canvas->allow_multisample(OpenGLManager::can_multisample());
    m_canvas->set_config(wxGetApp().plater()->config());
    m_canvas->set_model(&m_model);
    // Reuse the editor's shared slicing process: GLCanvas3D::render() (via
    // _max_bounding_box) dereferences the process when canvas type == View3D.
    // Passing nullptr segfaults; this mirrors View3D/Preview/AssembleView.
    m_canvas->set_process(wxGetApp().plater()->get_background_process());
    m_canvas->set_type(GLCanvas3D::ECanvasType::CanvasView3D);

    m_canvas->enable_picking(false);   // viewport face/edge picking is custom (TODO)
    m_canvas->enable_moving(false);
    m_canvas->enable_gizmos(false);
    m_canvas->enable_selection(false); // stock volume selection unused; solid highlight is tree-driven
    m_canvas->enable_main_toolbar(false);
    m_canvas->enable_select_plate_toolbar(false);
    m_canvas->enable_assemble_view_toolbar(false);
    m_canvas->enable_separator_toolbar(false);
    m_canvas->enable_collapse_toolbar(false);
    m_canvas->enable_plate_chrome(false);
    m_canvas->enable_labels(false);
    m_canvas->set_axes_at_bed_center(true);   // triad at bed centre = modeling origin

    m_canvas->set_design_sketch_tool(&m_sketch_tool);
    m_sketch_tool.on_commit = [this](const SketchProfile& prof, const SketchPlane& pl) {
        if (m_on_sketch_commit) m_on_sketch_commit(prof, pl);
        if (m_canvas) m_canvas->set_as_dirty();
        if (m_canvas_widget) m_canvas_widget->Refresh();
    };
    m_sketch_tool.on_commit_entities = [this](const std::vector<SketchEntity>& ents,
                                              const std::vector<SketchEntityConstraintDef>& cons,
                                              const SketchPlane& pl) {
        if (m_on_sketch_entities_commit) m_on_sketch_entities_commit(ents, cons, pl);
        if (m_canvas) m_canvas->set_as_dirty();
        if (m_canvas_widget) m_canvas_widget->Refresh();
    };

    // Onshape-style in-canvas value editor, floating over the GL canvas. The tool hands
    // us a screen pixel (device px) + a commit/cancel pair; we convert to logical client
    // px and wrap the callbacks so each one re-solves and re-renders the viewport.
    m_inline_editor = std::make_unique<SketchInlineEditor>(m_canvas_widget);
    m_sketch_tool.on_inline_edit = [this](wxPoint screen_px, double current,
                                          const std::string& title,
                                          std::function<void(double)> commit,
                                          std::function<void()> cancel) {
        if (!m_inline_editor) { if (cancel) cancel(); return; }
        // The tool hands us canvas device px; convert to logical client px, then to
        // absolute screen coords for the floating editor frame.
        const double s = m_canvas_widget ? m_canvas_widget->GetContentScaleFactor() : 1.0;
        const wxPoint client_pt(int(screen_px.x / s), int(screen_px.y / s));
        const wxPoint scr = m_canvas_widget ? m_canvas_widget->ClientToScreen(client_pt) : client_pt;
        // Freeze the sketch tool while the field is open so a stray click/move on the GL
        // canvas can't draw under the floating editor; released on commit or cancel.
        m_sketch_tool.set_inline_busy(true);
        m_inline_editor->open(scr, current, title,
            [this, commit](double v) {
                m_sketch_tool.set_inline_busy(false);
                if (commit) commit(v);
                request_repaint();
            },
            [this, cancel]() {
                m_sketch_tool.set_inline_busy(false);
                if (cancel) cancel();
                request_repaint();
            });
    };
    // Let the tool force-close the field (keep-as-drawn) — polyline right-click/double-click
    // ends the chain even while a per-segment value field is open.
    m_sketch_tool.on_inline_dismiss = [this]() {
        if (m_inline_editor) m_inline_editor->cancel();
    };

    // Bottom-right viewport HUD: a borderless, non-focusable float label showing the active
    // tool's current values. Top-level (a child widget is hidden by the GL surface, same as
    // the inline editor). Fed every frame by the tool's on_readout; empty text hides it.
    {
        wxWindow* top = wxGetTopLevelParent(m_canvas_widget);
        m_hud = new wxFrame(top, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                            wxFRAME_NO_TASKBAR | wxBORDER_NONE | wxFRAME_FLOAT_ON_PARENT |
                            wxSTAY_ON_TOP | wxTRANSPARENT_WINDOW);
        m_hud->SetBackgroundColour(wxColour(28, 30, 34));
        m_hud_label = new wxStaticText(m_hud, wxID_ANY, wxEmptyString);
        m_hud_label->SetForegroundColour(wxColour(0x46, 0xE0, 0xC8));   // teal, reads on dark bed
        wxFont f = m_hud_label->GetFont(); f.MakeBold(); m_hud_label->SetFont(f);
        auto* hs = new wxBoxSizer(wxHORIZONTAL);
        hs->Add(m_hud_label, 0, wxALL, 6);
        m_hud->SetSizerAndFit(hs);
        m_hud->Hide();
    }
    m_sketch_tool.on_readout = [this](const std::string& s) { set_readout(s); };

    refresh_bed();

    m_canvas->bind_event_handlers();

    // The Design GL canvas only receives key events (Esc to exit/enter Select, Ctrl+Z undo)
    // while it holds keyboard focus. Clicking a side-panel button steals focus, after which
    // Esc/Ctrl+Z silently do nothing until the viewport is clicked again. Restore focus
    // whenever the pointer enters the viewport (focus-follows-mouse, standard CAD behaviour).
    m_canvas_widget->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
        // …but NOT while an inline value field is open: the field floats over the canvas, so
        // the smallest pointer jiggle re-enters the viewport and would yank focus off the
        // field (the "no cursor focus on the number, click to focus" bug).
        if (m_canvas_widget && !m_sketch_tool.inline_busy()) m_canvas_widget->SetFocus();
        e.Skip();
    });


    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_canvas_widget, 1, wxEXPAND);
    SetSizer(sizer);
    SetMinSize(wxSize(300, 300));
}

DesignCanvas::~DesignCanvas()
{
    if (m_hud) m_hud->Destroy();
    delete m_canvas;
    delete m_canvas_widget;
}

// Distinct per-body colours (Onshape-style). Body 0 keeps the familiar gold; the rest
// cycle through a small saturated palette so coexisting solids read as separate parts.
static ColorRGBA body_palette(int body_idx)
{
    static const ColorRGBA kPalette[] = {
        ColorRGBA(0.86f, 0.66f, 0.20f, 1.0f),  // gold
        ColorRGBA(0.30f, 0.62f, 0.90f, 1.0f),  // blue
        ColorRGBA(0.45f, 0.78f, 0.42f, 1.0f),  // green
        ColorRGBA(0.86f, 0.45f, 0.40f, 1.0f),  // coral
        ColorRGBA(0.70f, 0.52f, 0.86f, 1.0f),  // violet
        ColorRGBA(0.90f, 0.70f, 0.35f, 1.0f),  // amber
    };
    const int n = int(sizeof(kPalette) / sizeof(kPalette[0]));
    return kPalette[((body_idx % n) + n) % n];
}

void DesignCanvas::request_repaint()
{
    if (!m_canvas)
        return;
    m_canvas->set_as_dirty();

    if (m_sw_gl < 0) {
        // Cache the backend once it's known; the renderer string is empty until
        // GL is initialised, so stay "unknown" and take the safe direct path till then.
        const std::string& r = OpenGLManager::get_gl_info().get_renderer();
        if (!r.empty())
            m_sw_gl = (boost::icontains(r, "llvmpipe") || boost::icontains(r, "softpipe") ||
                       boost::icontains(r, "swrast")   || boost::icontains(r, "software")) ? 1 : 0;
    }

    if (m_sw_gl == 0) {
        if (m_canvas_widget)
            m_canvas_widget->Refresh();   // hardware GL: paint cycle drives render()
    } else {
        m_canvas->render();               // software GL or backend not yet known
    }
}

void DesignCanvas::force_repaint()
{
    if (m_canvas == nullptr || m_canvas_widget == nullptr)
        return;

    CallAfter([this]() {
        if (m_canvas == nullptr || m_canvas_widget == nullptr)
            return;
        m_canvas->set_as_dirty();
        m_canvas_widget->Refresh();
        m_canvas_widget->Update();   // synchronous: an expose may never come after a page show
    });
}

void DesignCanvas::reload(bool keep_view)
{
    m_canvas->reset_volumes();

    for (int i = 0; i < (int)m_model.objects.size(); ++i)
        m_canvas->load_object(m_model, i);

    const ColorRGBA sel_gold(0.40f, 0.82f, 1.0f, 1.0f);   // cyan tint = solid selected
    const ColorRGBA ghost(0.26f, 0.66f, 1.0f, 0.45f);

    const auto& volumes = m_canvas->get_volumes().volumes;
    for (auto* v : volumes) {
        int obj_idx = v->object_idx();
        if (obj_idx == 0) {
            // Object 0 holds one volume per body — colour each by its body index so
            // multiple coexisting solids are visually distinct (Onshape per-part colour).
            const int b = v->volume_idx();
            bool hidden = (b >= 0 && b < int(m_body_visible.size())) && !m_body_visible[b];
            // Preview-only mode (fillet/chamfer/draft, once a valid target is picked): hide
            // every base body so only the result ghost is on screen until Confirm.
            if (m_body_hidden) hidden = true;
            v->is_active = !hidden;   // per-body visibility toggle
            if (!hidden) {
                // Selection tint wins; otherwise the per-body override (Color tool) or the
                // auto palette via body_color().
                ColorRGBA c = m_body_selected ? sel_gold : body_color(b);
                if (b == m_hl_body_target)    c = ColorRGBA(0.30f, 0.90f, 0.70f, 1.0f); // target = teal-green
                else if (b == m_hl_body_tool) c = ColorRGBA(1.00f, 0.55f, 0.15f, 1.0f); // tool = orange
                if (m_body_translucent) c.a(0.30f);
                v->set_color(c);
            }
        } else if (obj_idx == 1) {
            // The ghost is normally a faint blue overlay on the visible body. In preview-only
            // mode it IS the result (base bodies hidden), so render it opaque so it reads as a
            // finished solid rather than a see-through hint.
            v->set_color(m_body_hidden ? ColorRGBA(0.40f, 0.82f, 1.0f, 1.0f) : ghost);
        }
    }

    if (!keep_view) {
        if (m_first_frame && !m_model.objects.empty()) {
            m_canvas->select_view("iso");
            m_canvas->zoom_to_volumes();
            m_first_frame = false;
        }
    }

    m_canvas->set_as_dirty();
    if (m_canvas_widget)
        m_canvas_widget->Refresh();
}

void DesignCanvas::set_mesh(const TriangleMesh& mesh)
{
    if (m_model.objects.empty()) {
        auto* obj = m_model.add_object();
        obj->add_volume(mesh);
        obj->add_instance();
    } else {
        ModelObject* obj = m_model.objects.front();
        obj->clear_volumes();
        obj->add_volume(mesh);
        if (obj->instances.empty())
            obj->add_instance();
    }

    reload(!m_first_frame);
}

void DesignCanvas::set_bodies(const std::vector<TriangleMesh>& body_meshes,
                              const std::vector<bool>& visible)
{
    // Object 0 carries one GLVolume per body so reload() can colour each distinctly.
    // Falls back to a single-volume object when there's only one body (identical look
    // to the old set_mesh path). Picking still uses the combined mesh via set_solid_pick.
    m_body_visible = visible;   // empty => all visible; reload() reads this per volume
    if (body_meshes.empty()) { clear_mesh(); return; }

    ModelObject* obj = m_model.objects.empty() ? m_model.add_object()
                                               : m_model.objects.front();
    obj->clear_volumes();
    for (const TriangleMesh& m : body_meshes)
        obj->add_volume(m);
    if (obj->instances.empty())
        obj->add_instance();

    reload(!m_first_frame);
}

void DesignCanvas::clear_mesh()
{
    if (!m_model.objects.empty()) {
        m_model.delete_object((size_t)0);
        reload(true);
    }
}

void DesignCanvas::set_preview_mesh(const TriangleMesh& mesh)
{
    // Remove existing ghost (object 1) if present
    if (m_model.objects.size() > 1)
        m_model.delete_object((size_t)1);

    auto* obj = m_model.add_object();
    obj->add_volume(mesh);
    obj->add_instance();

    reload(true);
}

void DesignCanvas::clear_preview()
{
    if (m_model.objects.size() > 1) {
        m_model.delete_object((size_t)1);
        reload(true);
    }
}

void DesignCanvas::fit_view()
{
    if (m_canvas && !m_model.objects.empty()) {
        m_canvas->zoom_to_volumes();
        m_canvas->set_as_dirty();
        if (m_canvas_widget)
            m_canvas_widget->Refresh();
    }
}

void DesignCanvas::set_view(const std::string& view_name)
{
    if (m_canvas) {
        m_canvas->select_view(view_name);
        m_canvas->zoom_to_volumes();
        m_canvas->set_as_dirty();
        if (m_canvas_widget)
            m_canvas_widget->Refresh();
    }
}

void DesignCanvas::begin_sketch(const SketchPlane& plane, DesignSketchTool::Mode mode)
{
    m_sketch_tool.begin(plane, mode);
    if (m_canvas) m_canvas->set_as_dirty();
    if (m_canvas_widget) m_canvas_widget->Refresh();
}

void DesignCanvas::set_sketch_plane(const SketchPlane& plane)
{
    m_sketch_tool.set_plane(plane);   // keeps the 2D entities; only the carrier plane changes
    if (m_canvas) m_canvas->set_as_dirty();
    if (m_canvas_widget) m_canvas_widget->Refresh();
}

void DesignCanvas::edit_sketch(const std::vector<SketchEntity>& entities,
                               const std::vector<SketchEntityConstraintDef>& constraints,
                               const SketchPlane& plane)
{
    m_sketch_tool.begin_edit(entities, constraints, plane);
    if (m_canvas) m_canvas->set_as_dirty();
    if (m_canvas_widget) m_canvas_widget->Refresh();
}

void DesignCanvas::set_sketch_tool(DesignSketchTool::Mode mode)
{
    m_sketch_tool.set_tool(mode);
    if (m_canvas) m_canvas->set_as_dirty();
    if (m_canvas_widget) m_canvas_widget->Refresh();
}

void DesignCanvas::set_sketch_construction(bool c)
{
    m_sketch_tool.set_construction(c);
}

void DesignCanvas::set_sketch_polygon_sides(int n)
{
    m_sketch_tool.set_polygon_sides(n);
}

void DesignCanvas::set_sketch_polygon_circumscribed(bool c)
{
    m_sketch_tool.set_polygon_circumscribed(c);
}

void DesignCanvas::finish_sketch()
{
    m_sketch_tool.finish();
    if (m_canvas) m_canvas->set_as_dirty();
    if (m_canvas_widget) m_canvas_widget->Refresh();
}

// Sync the Design bed to the CURRENT printer bed. Done on every tab activation, not just at
// construction: the panel is built early (before the active printer profile is fully applied),
// so a one-shot read picked up the 200x200 default while the real bed (e.g. 270x270) only
// loaded later — leaving the PartPlate grid spilling past the smaller bed quad.
void DesignCanvas::refresh_bed()
{
    const DynamicPrintConfig* config = wxGetApp().plater()->config();
    if (!config) return;
    const auto* bed_shape_opt = config->opt<ConfigOptionPoints>("printable_area");
    if (!bed_shape_opt) return;
    double printable_height = 100.0;
    const auto* ph_opt = config->opt<ConfigOptionFloat>("printable_height");
    if (ph_opt) printable_height = ph_opt->value;
    m_bed.set_shape(bed_shape_opt->values, printable_height, {}, {}, "", false);  // mainline added extruder_areas/heights params
}

void DesignCanvas::set_show_bed(bool b)
{
    if (!m_canvas) return;
    if (m_canvas->get_show_bed() == b) return;   // no repaint for a no-op toggle
    m_canvas->set_show_bed(b);
    request_repaint();
}

bool DesignCanvas::is_sketching() const { return m_sketch_tool.is_active(); }

void DesignCanvas::cancel_sketch()
{
    m_sketch_tool.cancel();
    if (m_canvas) m_canvas->set_as_dirty();
    if (m_canvas_widget) m_canvas_widget->Refresh();
}

void DesignCanvas::set_on_sketch_commit(std::function<void(const SketchProfile&, const SketchPlane&)> cb)
{
    m_on_sketch_commit = std::move(cb);
}

void DesignCanvas::set_on_sketch_entities_commit(
    std::function<void(const std::vector<SketchEntity>&,
                       const std::vector<SketchEntityConstraintDef>&,
                       const SketchPlane&)> cb)
{
    m_on_sketch_entities_commit = std::move(cb);
}

void DesignCanvas::set_on_segment_drawn(std::function<void(double, double)> cb)
{
    m_sketch_tool.on_segment_drawn = std::move(cb);
}

void DesignCanvas::set_on_cursor_metrics(std::function<void(double, double, bool)> cb)
{
    m_sketch_tool.on_cursor_metrics = std::move(cb);
}

void DesignCanvas::set_on_solve_state(std::function<void(int, bool, bool)> cb)
{
    m_sketch_tool.on_solve_state = std::move(cb);
}

void DesignCanvas::apply_segment_length(double len)
{
    m_sketch_tool.apply_segment_length(len);
    request_repaint();
}

void DesignCanvas::keep_segment_as_drawn()
{
    m_sketch_tool.keep_segment_as_drawn();
    request_repaint();
}

void DesignCanvas::set_on_sketch_selection_changed(std::function<void(int)> cb)
{
    m_sketch_tool.on_selection_changed = std::move(cb);
}

void DesignCanvas::set_on_sketch_face_selected(std::function<void()> cb)
{
    m_sketch_tool.on_face_selected = std::move(cb);
}

void DesignCanvas::set_on_display_sketch_selected(std::function<void(int, int)> cb)
{
    m_sketch_tool.on_display_sketch_selected = std::move(cb);
}

std::vector<SketchEntity> DesignCanvas::selected_loop_entities() const
{
    return m_sketch_tool.selected_loop_entities();
}

std::vector<std::vector<int>> DesignCanvas::region_entity_indices(const std::vector<SketchEntity>& ents) const
{
    return m_sketch_tool.region_entity_indices(ents);
}

void DesignCanvas::clear_loop_pick()
{
    m_sketch_tool.clear_display_pick();
}

void DesignCanvas::set_solid_pick(const std::vector<CadBody>* bodies, const TriangleMesh* mesh,
                                  const std::vector<int>* tri_face, const std::vector<int>* tri_body,
                                  const std::vector<bool>* visible,
                                  const std::vector<Transform3d>* xform)
{
    m_color_bodies = bodies;   // stable address (m_doc.bodies); reload() reads colour overrides
    m_sketch_tool.set_solid_pick(bodies, mesh, tri_face, tri_body, visible, xform);
}

// Effective display colour for a body: per-body override (Color tool) when set, else the
// auto body-index palette. body_palette() is the file-static helper defined above reload().
ColorRGBA DesignCanvas::body_color(int body) const
{
    if (m_color_bodies != nullptr && body >= 0 && body < int(m_color_bodies->size())
        && (*m_color_bodies)[body].has_color)
        return (*m_color_bodies)[body].color;
    return body_palette(body);
}

void DesignCanvas::begin_move_body(int body, const Vec3d& pivot, const Transform3d& base_xform,
                                   double body_radius)
{
    m_sketch_tool.set_move_gizmo(body, pivot, base_xform, body_radius);
    request_repaint();
}

void DesignCanvas::clear_move_gizmo()
{
    m_sketch_tool.clear_move_gizmo();
    request_repaint();
}

bool DesignCanvas::moving_body() const { return m_sketch_tool.moving_body(); }

void DesignCanvas::set_on_body_move_changed(std::function<void(int, const Transform3d&)> cb)
{
    m_sketch_tool.on_body_move_changed = std::move(cb);
}

bool DesignCanvas::begin_fillet_gizmo(const Vec3d& body_centroid, double radius)
{
    const bool ok = m_sketch_tool.set_fillet_gizmo(body_centroid, radius);
    request_repaint();
    return ok;
}

void DesignCanvas::clear_fillet_gizmo()
{
    m_sketch_tool.clear_fillet_gizmo();
    request_repaint();
}

bool DesignCanvas::filleting() const { return m_sketch_tool.filleting(); }

void DesignCanvas::set_on_fillet_radius_changed(std::function<void(double)> cb)
{
    m_sketch_tool.on_fillet_radius_changed = std::move(cb);
}

void DesignCanvas::begin_hole_gizmo(const SketchPlane& plane, double x, double y,
                                    double diameter, double depth, bool through)
{
    m_sketch_tool.set_hole_gizmo(plane, x, y, diameter, depth, through);
    request_repaint();
}

void DesignCanvas::set_hole_face_bounds(bool has, double umin, double umax, double vmin, double vmax)
{
    m_sketch_tool.set_hole_face_bounds(has, umin, umax, vmin, vmax);
}

void DesignCanvas::clear_hole_gizmo()
{
    m_sketch_tool.clear_hole_gizmo();
    request_repaint();
}

bool DesignCanvas::holing() const { return m_sketch_tool.holing(); }

void DesignCanvas::set_on_hole_changed(std::function<void(double, double, double, double)> cb)
{
    m_sketch_tool.on_hole_changed = std::move(cb);
}

void DesignCanvas::begin_thread_gizmo(const SketchPlane& plane, double x, double y,
                                      double radius, double height)
{
    m_sketch_tool.set_thread_gizmo(plane, x, y, radius, height);
    request_repaint();
}

void DesignCanvas::clear_thread_gizmo()
{
    m_sketch_tool.clear_thread_gizmo();
    request_repaint();
}

bool DesignCanvas::threading() const { return m_sketch_tool.threading(); }

void DesignCanvas::set_on_thread_changed(std::function<void(double, double, double, double)> cb)
{
    m_sketch_tool.on_thread_changed = std::move(cb);
}

void DesignCanvas::begin_shell_gizmo(const Vec3d& face_centroid, const Vec3d& inward_dir,
                                     double thickness)
{
    m_sketch_tool.set_shell_gizmo(face_centroid, inward_dir, thickness);
    request_repaint();
}

void DesignCanvas::clear_shell_gizmo()
{
    m_sketch_tool.clear_shell_gizmo();
    request_repaint();
}

bool DesignCanvas::shelling() const { return m_sketch_tool.shelling(); }

void DesignCanvas::set_on_shell_thickness_changed(std::function<void(double)> cb)
{
    m_sketch_tool.on_shell_thickness_changed = std::move(cb);
}

void DesignCanvas::begin_revolve_gizmo(const SketchPlane& plane, const Vec2d& centroid,
                                       int axis_sel, double angle, bool flip)
{
    m_sketch_tool.set_revolve_gizmo(plane, centroid, axis_sel, angle, flip);
    request_repaint();
}

void DesignCanvas::clear_revolve_gizmo()
{
    m_sketch_tool.clear_revolve_gizmo();
    request_repaint();
}

bool DesignCanvas::revolving() const { return m_sketch_tool.revolving(); }

void DesignCanvas::set_on_revolve_angle_changed(std::function<void(double)> cb)
{
    m_sketch_tool.on_revolve_angle_changed = std::move(cb);
}

void DesignCanvas::set_draft_gizmo(const Vec3d& face_centroid, const Vec3d& face_normal, double angle)
{
    m_sketch_tool.set_draft_gizmo(face_centroid, face_normal, angle);
    request_repaint();
}

void DesignCanvas::clear_draft_gizmo()
{
    m_sketch_tool.clear_draft_gizmo();
    request_repaint();
}

bool DesignCanvas::drafting() const { return m_sketch_tool.drafting(); }

void DesignCanvas::set_on_draft_angle_changed(std::function<void(double)> cb)
{
    m_sketch_tool.set_on_draft_angle_changed(std::move(cb));
}

void DesignCanvas::set_cut_gizmo(const SketchPlane& plane, double offset, const Vec3d& body_center, double half_extent)
{
    m_sketch_tool.set_cut_gizmo(plane, offset, body_center, half_extent);
    request_repaint();
}

void DesignCanvas::clear_cut_gizmo()
{
    m_sketch_tool.clear_cut_gizmo();
    request_repaint();
}

bool DesignCanvas::cutting() const { return m_sketch_tool.cutting(); }

void DesignCanvas::set_on_cut_offset_changed(std::function<void(double)> cb)
{
    m_sketch_tool.set_on_cut_offset_changed(std::move(cb));
}

void DesignCanvas::begin_pattern_gizmo(const SketchPlane& plane, const Vec3d& body_centroid,
                                       bool circular, int count, int dir, double spacing, double angle)
{
    m_sketch_tool.set_pattern_gizmo(plane, body_centroid, circular, count, dir, spacing, angle);
    request_repaint();
}

void DesignCanvas::clear_pattern_gizmo()
{
    m_sketch_tool.clear_pattern_gizmo();
    request_repaint();
}

bool DesignCanvas::patterning() const { return m_sketch_tool.patterning(); }

void DesignCanvas::set_on_pattern_changed(std::function<void(double)> cb)
{
    m_sketch_tool.on_pattern_changed = std::move(cb);
}

void DesignCanvas::set_on_solid_selection_changed(std::function<void(int, int, int, int)> cb)
{
    m_sketch_tool.on_solid_selection_changed = std::move(cb);
}

void DesignCanvas::set_on_place_on_face(std::function<bool()> cb)
{
    m_sketch_tool.on_place_on_face = std::move(cb);
}

void DesignCanvas::select_body(int body)
{
    m_sketch_tool.select_body(body);
    request_repaint();
}

void DesignCanvas::set_extrude_gizmo(const SketchPlane& plane, const Vec2d& centroid,
                                     double depth, double depth2, bool two_sided, bool flip)
{
    m_sketch_tool.set_extrude_gizmo(plane, centroid, depth, depth2, two_sided, flip);
    request_repaint();
}

void DesignCanvas::clear_extrude_gizmo()
{
    m_sketch_tool.clear_extrude_gizmo();
    request_repaint();
}

void DesignCanvas::set_on_extrude_depth_changed(std::function<void(double, bool)> cb)
{
    m_sketch_tool.on_extrude_depth_changed = std::move(cb);
}

void DesignCanvas::set_datum_gizmo(const SketchPlane& plane, double usize, double vsize,
                                   const Vec3d& base_origin, const Vec3d& base_normal,
                                   double offset, bool offset_on)
{
    m_sketch_tool.set_datum_gizmo(plane, usize, vsize, base_origin, base_normal, offset, offset_on);
    request_repaint();
}

void DesignCanvas::clear_datum_gizmo()
{
    m_sketch_tool.clear_datum_gizmo();
    request_repaint();
}

void DesignCanvas::set_on_datum_size_changed(std::function<void(double, double)> cb)
{
    m_sketch_tool.on_datum_size_changed = std::move(cb);
}

void DesignCanvas::set_on_datum_offset_changed(std::function<void(double)> cb)
{
    m_sketch_tool.on_datum_offset_changed = std::move(cb);
}

void DesignCanvas::set_base_pick(std::vector<SketchPlane> planes, std::vector<int> bases,
                                std::vector<std::string> labels)
{
    m_sketch_tool.set_base_pick(std::move(planes), std::move(bases), std::move(labels));
    request_repaint();
}

void DesignCanvas::clear_base_pick()
{
    m_sketch_tool.clear_base_pick();
    request_repaint();
}

void DesignCanvas::set_on_datum_base_picked(std::function<void(int)> cb)
{
    m_sketch_tool.on_datum_base_picked = std::move(cb);
}

void DesignCanvas::set_on_sketch_exit(std::function<void()> cb)
{
    m_sketch_tool.on_exit = std::move(cb);
}

void DesignCanvas::set_on_move_exit(std::function<void()> cb)
{
    m_sketch_tool.on_move_exit = std::move(cb);
}

void DesignCanvas::set_on_context_menu(std::function<void(const wxPoint&)> cb)
{
    m_on_context_menu = std::move(cb);
    if (!m_canvas_widget || m_ctx_bound)
        return;
    m_ctx_bound = true;
    // Bound AFTER GLCanvas3D's own handlers, so this runs first and can consume the event.
    // It only consumes when it actually opens the offer; every other right-click still falls
    // through to the polyline-chain end and the move gizmo, which were there first.
    m_canvas_widget->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent& e) {
        if (m_on_context_menu && !is_sketching() && !inline_busy()) {
            m_on_context_menu(m_canvas_widget->ClientToScreen(e.GetPosition()));
            return;   // consumed
        }
        e.Skip();
    });
}

void DesignCanvas::set_on_undo_redo(std::function<void(bool)> cb)
{
    m_sketch_tool.on_undo_redo = std::move(cb);
}

void DesignCanvas::set_display_sketches(std::vector<DesignSketchTool::DisplaySketch> ds)
{
    m_sketch_tool.set_display_sketches(std::move(ds));
    // Overlay changed programmatically (no mouse event) — force a repaint.
    request_repaint();
}

void DesignCanvas::set_datum_planes(std::vector<SketchPlane> planes, std::vector<Vec2d> sizes)
{
    m_sketch_tool.set_datum_planes(std::move(planes), std::move(sizes));
    request_repaint();
}

bool DesignCanvas::toggle_planes()
{
    const bool on = m_sketch_tool.toggle_show_planes();
    request_repaint();
    return on;
}

bool DesignCanvas::toggle_axes()
{
    const bool on = m_sketch_tool.toggle_show_axes();
    request_repaint();
    return on;
}

void DesignCanvas::set_section_plane(bool on, double z, bool keep_upper)
{
    m_section_on = on;
    // The kept half must read as a SOLID part, never a see-through ghost: make sure no leftover
    // preview translucency is applied while the section is on. Guarded — a no-op if already opaque.
    if (on) { set_body_translucent(false); set_body_hidden(false); }
    if (m_canvas) {
        if (on) {
            // GLCanvas3D turns the two clipping planes into a Z-RANGE: set_z_range(-p0.offset,
            // p1.offset). keep_upper=false keeps the LOWER half (z in [-1e5, z]); keep_upper=true
            // keeps the OPPOSITE, UPPER half (z in [z, +1e5]). Only HIDES geometry — no bodies.
            if (keep_upper) {
                m_canvas->set_clipping_plane(0, ClippingPlane(Vec3d(0.0, 0.0, 1.0), -z));     // min_z = z
                m_canvas->set_clipping_plane(1, ClippingPlane(Vec3d(0.0, 0.0, 1.0), 1.0e5));  // max_z = +1e5
            } else {
                m_canvas->set_clipping_plane(0, ClippingPlane(Vec3d(0.0, 0.0, 1.0), 1.0e5));  // min_z = -1e5
                m_canvas->set_clipping_plane(1, ClippingPlane(Vec3d(0.0, 0.0, 1.0), z));      // max_z = z
            }
            m_canvas->set_use_clipping_planes(true);
        } else {
            m_canvas->set_use_clipping_planes(false);
        }
        m_canvas->set_as_dirty();
    }
    request_repaint();
}

double DesignCanvas::model_mid_z() const
{
    const BoundingBoxf3 bb = m_model.bounding_box_exact();
    return bb.defined ? bb.center().z() : 0.0;
}

void DesignCanvas::set_readout(const std::string& text)
{
    if (!m_hud || !m_hud_label || !m_canvas_widget) return;
    if (text == m_hud_last) return;                 // only touch the WM on a real change
    m_hud_last = text;
    if (text.empty()) { m_hud->Hide(); return; }
    m_hud_label->SetLabel(wxString::FromUTF8(text));
    m_hud->Fit();
    // Anchor to the canvas's bottom-right corner with a small margin (screen coords).
    const wxSize  cs = m_canvas_widget->GetClientSize();
    const wxSize  hs = m_hud->GetSize();
    const wxPoint br = m_canvas_widget->ClientToScreen(
        wxPoint(cs.GetWidth() - hs.GetWidth() - 12, cs.GetHeight() - hs.GetHeight() - 12));
    if (!m_hud->IsShown()) m_hud->Show();           // Show before Move (GTK ignores pre-map Move)
    m_hud->Move(br);
    m_hud->Raise();
}

void DesignCanvas::set_body_highlight(bool on)
{
    if (m_body_selected == on) return;
    m_body_selected = on;
    reload(true);   // recolours the body volume (selected = cyan tint)
}

void DesignCanvas::set_operand_bodies(int target_body, int tool_body)
{
    if (m_hl_body_target == target_body && m_hl_body_tool == tool_body) return;
    m_hl_body_target = target_body;
    m_hl_body_tool   = tool_body;
    reload(true);      // recolours the body volumes (same idiom set_body_highlight uses)
}

void DesignCanvas::set_highlight_sketches(std::vector<std::pair<int, ColorRGBA>> hl)
{
    m_sketch_tool.set_highlight_sketches(std::move(hl));
    request_repaint();
}

void DesignCanvas::set_body_translucent(bool on)
{
    if (m_body_translucent == on) return;
    m_body_translucent = on;
    reload(true);   // re-applies object-0 alpha so the solid fades for the fillet preview
}

void DesignCanvas::set_body_hidden(bool on)
{
    if (m_body_hidden == on) return;
    m_body_hidden = on;
    reload(true);   // hides/show base bodies + flips the ghost opaque/faint for preview-only mode
}

void DesignCanvas::delete_selected_sketch_entities()
{
    m_sketch_tool.delete_selected();
    request_repaint();
}

bool DesignCanvas::inline_busy() const
{
    return m_sketch_tool.inline_busy();
}

bool DesignCanvas::undo_last_sketch_entity()
{
    const bool did = m_sketch_tool.undo_last_entity();
    if (did) request_repaint();
    return did;
}

bool DesignCanvas::delete_selected_or_last_sketch_entity()
{
    const bool did = m_sketch_tool.delete_selected_or_last();
    if (did) request_repaint();
    return did;
}

void DesignCanvas::clear_sketch_selection()
{
    m_sketch_tool.clear_selection();
    request_repaint();
}

DesignSketchTool::DimType DesignCanvas::sketch_dimension_kind() const
{
    return m_sketch_tool.dimension_kind();
}

double DesignCanvas::sketch_dimension_current() const
{
    return m_sketch_tool.dimension_current();
}

void DesignCanvas::apply_sketch_dimension(double v)
{
    m_sketch_tool.apply_dimension(v);
    request_repaint();
}

void DesignCanvas::open_inline_value(double current, std::function<void(double)> commit,
                                     std::function<void()> cancel)
{
    if (!m_inline_editor || !m_canvas_widget) { if (cancel) cancel(); return; }
    // Host-driven value entry (committed-feature Constrain path): the trigger is a toolbar
    // button. Anchor the field OVER the picked geometry (same as the draw-then-edit tools) when
    // the tool can project it; else fall back to the viewport centre, where the sketch is in
    // view. GetScreenRect collapses GetClientSize()+ClientToScreen() into one call; if the GL
    // canvas reports degenerate geometry (transiently, right after a re-layout), fall back to the
    // always-realised top-level window so the editor never lands in the top-left corner.
    wxRect r = m_canvas_widget->GetScreenRect();
    if (r.GetWidth() <= 1 || r.GetHeight() <= 1) {
        if (wxWindow* top = wxGetTopLevelParent(m_canvas_widget))
            r = top->GetScreenRect();
    }
    wxPoint scr(r.GetLeft() + r.GetWidth() / 2, r.GetTop() + r.GetHeight() / 2);
    wxPoint anchor;
    if (m_sketch_tool.constrain_value_anchor(anchor)) {     // device px in the canvas viewport
        const double s = m_canvas_widget->GetContentScaleFactor();
        scr = m_canvas_widget->ClientToScreen(wxPoint(int(anchor.x / s), int(anchor.y / s)));
    }
    // Freeze the canvas so focus-follows-mouse can't steal keyboard focus off the field — the
    // same fix the draw-then-edit path uses (cursor focus stays on the field, no pre-click).
    m_sketch_tool.set_inline_busy(true);
    m_inline_editor->open(scr, current, "",
        [this, commit](double v) {
            m_sketch_tool.set_inline_busy(false);
            if (commit) commit(v);
            request_repaint();
        },
        [this, cancel]() {
            m_sketch_tool.set_inline_busy(false);
            if (cancel) cancel();
            request_repaint();
        });
}

void DesignCanvas::set_on_dimension_pick_complete(std::function<void(double)> cb)
{
    m_sketch_tool.on_dimension_pick_complete = std::move(cb);
}

DesignSketchTool::DimType DesignCanvas::pending_dimension_type() const
{
    return m_sketch_tool.pending_dimension_type();
}

void DesignCanvas::set_sketch_dimension_value(double v)
{
    m_sketch_tool.set_dimension_value(v);
    request_repaint();
}

void DesignCanvas::cancel_sketch_dimension()
{
    m_sketch_tool.cancel_dimension_value();
    request_repaint();
}

void DesignCanvas::begin_constrain(const SketchProfile& prof, const SketchPlane& plane)
{
    m_sketch_tool.begin_constrain(prof, plane);
    // The overlay must appear immediately (no mouse move to trigger a repaint).
    request_repaint();
}

void DesignCanvas::begin_imported_transform(
        int feat, const std::vector<std::vector<std::vector<Vec2d>>>& base_regions,
        const SketchPlane& plane, const Vec2d& offset, double scale_x, double scale_y)
{
    m_sketch_tool.begin_imported_transform(feat, base_regions, plane, offset, scale_x, scale_y);
    request_repaint();
}

void DesignCanvas::set_on_imported_transform(std::function<void(int, Vec2d, double, double)> cb)
{
    m_sketch_tool.on_imported_transform = std::move(cb);
}

void DesignCanvas::end_constrain()
{
    // cancel() clears m_active + the picked-segment/entity indices, so the
    // constrain overlay (highlighted picks) disappears on the next render.
    m_sketch_tool.cancel();
    request_repaint();
}

bool DesignCanvas::is_constraining() const { return m_sketch_tool.is_constraining(); }

bool DesignCanvas::selected_segment(int& a, int& b) const
{
    return m_sketch_tool.selected_segment(a, b);
}

void DesignCanvas::update_constrain_profile(const std::vector<Vec2d>& pts)
{
    m_sketch_tool.set_profile_points(pts);
    request_repaint();
}

void DesignCanvas::begin_constrain_entities(const std::vector<SketchEntity>& ents,
                                            const SketchPlane& plane)
{
    m_sketch_tool.begin_constrain_entities(ents, plane);
    request_repaint();
}

bool DesignCanvas::is_constraining_entities() const
{
    return m_sketch_tool.is_constraining_entities();
}

bool DesignCanvas::selected_constrain_entities(int& e0, int& e1) const
{
    return m_sketch_tool.selected_constrain_entities(e0, e1);
}

int DesignCanvas::selected_constrain_axis() const
{
    return m_sketch_tool.pick2();
}

bool DesignCanvas::pick0_point(Vec2d& out) const
{
    return m_sketch_tool.pick0_point(out);
}

void DesignCanvas::update_constrain_entities(const std::vector<SketchEntity>& ents)
{
    m_sketch_tool.set_constrain_entities(ents);
    request_repaint();
}

void DesignCanvas::set_constraint_highlight(std::vector<int> entities)
{
    m_sketch_tool.set_constraint_highlight(std::move(entities));
    request_repaint();
}

void DesignCanvas::set_constraint_glyphs(std::vector<SketchEntityConstraintDef> cons)
{
    m_sketch_tool.set_constraint_glyphs(std::move(cons));
    request_repaint();
}

}} // namespace Slic3r::GUI

#include "slic3r/GUI/CAD/DesignCanvas.hpp"

#include "slic3r/GUI/CAD/SketchInlineEditor.hpp"
#include "slic3r/GUI/CAD/DesignInteraction.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/3DBed.hpp"
#include "slic3r/GUI/Camera.hpp"   // N: look down the sketch plane normal
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"   // the current plate's origin: the first view's offset
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/Event.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/MeshUtils.hpp"   // ClippingPlane (section view)
#include "libslic3r/Config.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>

#include <imgui.h>
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include "libslic3r/CAD/SketchEngine.hpp"
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include "libslic3r/Color.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"
#include "libslic3r/Point.hpp"
#include <utility>
#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/BoundingBox.hpp"
#include <wx/glcanvas.h>
#include <wx/panel.h>
#include <wx/setup.h>
#include <wx/stopwatch.h>   // wxGetLocalTimeMillis: the right-click vs right-hold budget
#include <wx/sizer.h>
#include <wx/frame.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/time.h>
#include <wx/toplevel.h>
#include <wx/window.h>
#include "libslic3r/PrintConfig.hpp"

#if defined(__WXMSW__) && wxUSE_POPUPWIN
#include <wx/popupwin.h>
extern wxPopupWindow* wxCurrentPopupWindow;
#endif

namespace Slic3r {
namespace GUI {

// The Design tab's iso view is the CAD one: a true isometric from the front-right corner, which
// shows the navigator's Front, Right and Top faces. Camera's own "iso" looks from the front-left
// at 45 degrees and stays Prepare's and the thumbnails'.
static void select_iso_view(Camera& camera)
{
    const Vec3d target = camera.get_target();
    camera.look_at(target + camera.get_distance() * Vec3d(1., -1., 1.).normalized(), target, Vec3d::UnitZ());
    camera.auto_type(Camera::EType::Perspective);
}

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
    // Nothing to slice here, but GLCanvas3D derefs the process unguarded every frame, so it
    // cannot be null. Borrow the plater's, as the editor canvases do.
    m_canvas->set_process(&wxGetApp().plater()->background_process());
    m_canvas->set_type(GLCanvas3D::ECanvasType::CanvasView3D);
    m_canvas->set_studio_lighting(true);   // see GLCanvas3D::m_studio_lighting and phong.fs

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
    m_canvas->enable_sinking_contours(false); // they would be sliced from the plater's meshes
    m_canvas->set_design_canvas(true);   // home-position bed, triad and CAD grid at the modeling origin

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
    m_inline_editor = std::make_unique<SketchInlineEditor>();
    // The tool draws it: it owns the frame's ImGui pass and the render scale. Handing it a raw
    // pointer rather than the unique_ptr keeps the ownership where it was.
    m_sketch_tool.inline_editor = m_inline_editor.get();
    // SCHEDULE a paint, do not render one. request_repaint() renders SYNCHRONOUSLY on software
    // GL, and this callback runs from inside DesignSketchTool::render() — so using it here asks
    // for a render from within a render. The frames stopped after nine, which is what a
    // re-entrancy guard giving up looks like. Refresh() posts a paint event instead: the current
    // frame finishes, the event loop runs (which is where ImGui's queued characters are consumed),
    // and the next frame starts clean.
    // MEASUREMENT: does a typed character reach the GL canvas at all? Everything downstream of
    // this point is known good (ImGui reports want_text=1 and our InputText active), so if these
    // lines do not appear the character never got past the panel's CHAR_HOOK / the focus chain,
    // and no amount of work inside the field will help. Skips always: a pure observer.
    if (m_canvas_widget != nullptr && std::getenv("ORCA_CAD_UXTRACE")) {
        m_canvas_widget->Bind(wxEVT_CHAR, [](wxKeyEvent& e) {
            fprintf(stderr, "[UX] canvas_char key=%d\n", e.GetKeyCode());
            fflush(stderr);
            e.Skip();
        });
    }
    m_inline_editor->request_frame = [this] {
        // BOTH halves, and the dirty flag first: GLCanvas3D's paint handler returns without
        // rendering when the canvas is not marked dirty, so a bare Refresh() posts an event that
        // draws nothing and the frames still stop. request_repaint() does exactly this pair on
        // the hardware path; what it must NOT do here is its software path, which renders
        // synchronously — and this callback runs from inside render().
        if (m_canvas)        m_canvas->set_as_dirty();
        if (m_canvas_widget) m_canvas_widget->Refresh(false);
    };
    m_sketch_tool.on_inline_edit = [this](wxPoint screen_px, double current,
                                          const std::string& title,
                                          std::function<void(double)> commit,
                                          std::function<void()> cancel) {
        if (!m_inline_editor) { if (cancel) cancel(); return; }
        // The tool hands us canvas device px and the field is now drawn IN the canvas, so this
        // is already the coordinate space it wants — no conversion to screen coordinates, and no
        // window to place there.
        // Freeze the sketch tool while the field is open so a stray click/move on the GL
        // canvas can't draw under the field; released on commit or cancel.
        m_sketch_tool.set_inline_busy(true);
        // AND PUT THE KEYBOARD ON THE CANVAS. The field is drawn by ImGui, and ImGui is fed from
        // GLCanvas3D's own key handler, so a key only reaches it if the canvas is the focused
        // widget. That is a focus move WITHIN one window — the toolkit's business, not the window
        // manager's, which is the whole point of not being a window any more — but it still has
        // to be asked for: after a toolbar click or a tree selection the focus is elsewhere in
        // the panel, and the field would sit there taking nothing.
        if (m_canvas_widget) m_canvas_widget->SetFocus();
        m_inline_editor->open(screen_px, current, title,
            [this, commit](double v) {
                m_sketch_tool.set_inline_busy(false);
                if (commit) commit(v);
                // A refused value re-opens the same field (SketchInlineEditor::refuse).
                if (m_inline_editor && m_inline_editor->is_open()) m_sketch_tool.set_inline_busy(true);
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
    m_sketch_tool.on_inline_commit = [this]() {
        if (m_inline_editor) m_inline_editor->commit();
    };

    // The two viewport chips — the active tool's values bottom-right, the status line bottom-left
    // — are drawn by the canvas itself, in the tool's ImGui pass: they go away with the canvas,
    // the tab and the window, and never take the keyboard.
    m_sketch_tool.on_readout      = [this](const std::string& s) { set_readout(s); };
    m_sketch_tool.render_overlays = [this] { render_hud(); };

    refresh_bed();

    // The view this canvas opens on. Built lazily, on the way into the Design tab, so this
    // is the view the user is looking at right now — moved off the current plate onto the
    // Design bed, which stays at the printer bed's home whichever plate is current — turned to
    // the iso view Home returns to, keeping its target and zoom.
    m_parked_camera = wxGetApp().plater()->get_camera();
    if (PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate())
        m_parked_camera.translate_world(-plate->get_origin());
    select_iso_view(m_parked_camera);

    // Before any of this class's own Binds below: wx calls dynamically bound handlers in
    // reverse order of binding, and GLCanvas3D swallows several events without skipping them —
    // wxEVT_RIGHT_UP and wxEVT_ENTER_WINDOW in on_mouse, and wxEVT_SIZE in on_size, which is
    // just `m_dirty = true;`. For those, whatever is bound LAST is the only handler that runs.
    // The context menu and the focus-follows-mouse depend on running first, which is only true
    // while this call stays ahead of them.
    m_canvas->bind_event_handlers();

    // The Design GL canvas only receives key events (Esc to exit/enter Select, Ctrl+Z undo)
    // while it holds keyboard focus. Clicking a side-panel button steals focus, after which
    // Esc/Ctrl+Z silently do nothing until the viewport is clicked again. Restore focus
    // whenever the pointer enters the viewport (focus-follows-mouse, standard CAD behaviour).
    m_canvas_widget->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
        // …but NOT while an inline value field is open: the field floats over the canvas, so
        // the smallest pointer jiggle re-enters the viewport and would yank focus off the
        // field (the "no cursor focus on the number, click to focus" bug).
        bool take_focus = m_canvas_widget && !m_sketch_tool.inline_busy();
#if defined(__WXMSW__) && wxUSE_POPUPWIN
        // …nor while a popup is open. A ribbon dropdown opens over the viewport, and the pointer
        // crosses the viewport on its way to it: taking focus then reactivates the frame, which
        // on MSW dismisses the popup under the pointer. GLCanvas3D::on_mouse has the same guard.
        take_focus = take_focus && !wxCurrentPopupWindow;
#endif
        if (take_focus) m_canvas_widget->SetFocus();
        e.Skip();
    });


    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_canvas_widget, 1, wxEXPAND);
    SetSizer(sizer);
    SetMinSize(wxSize(300, 300));
}

// Both are idempotent, and neither destroys anything: the destructor still owns that.
void DesignCanvas::unbind_canvas_event_handlers()
{
    if (m_canvas) m_canvas->unbind_event_handlers();
}

void DesignCanvas::reset_canvas_volumes()
{
    if (m_canvas) m_canvas->reset_volumes();
}

DesignCanvas::~DesignCanvas()
{
    delete m_canvas;
    delete m_canvas_widget;
}

// Distinct per-body colours (Onshape-style). Body 0 keeps the familiar gold; the rest
// cycle through a small saturated palette so coexisting solids read as separate parts. No entry
// may sit near the selection cyan (design_selection_color): a blue or teal body would look
// selected, and its selected faces would barely stand out.
static ColorRGBA body_palette(int body_idx)
{
    static const ColorRGBA kPalette[] = {
        ColorRGBA(0.86f, 0.66f, 0.20f, 1.0f),  // gold
        ColorRGBA(0.84f, 0.42f, 0.66f, 1.0f),  // rose
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

    // NOTHING may touch GL before the canvas has initialised it. The backend probe below calls
    // OpenGLManager::get_gl_info().get_renderer(), which runs GLInfo::detect() -> glGetString
    // with no context current and, before init_opengl(), no loaded function pointers — a
    // segfault at startup with no window and nothing in the log. Anything that asks for a
    // repaint while the panel is still being built lands here, so the guard belongs at the top
    // rather than around the render() call: the crash was in the PROBE, not in the paint.
    if (!m_canvas->is_initialized()) {
        if (m_canvas_widget)
            m_canvas_widget->Refresh();   // the first real paint draws the current state anyway
        return;
    }

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

// ImGui's display size is shared by every canvas and only refreshed when a canvas sees its own
// size change, so the canvas taking over must re-announce its size (Plater does the same between
// Prepare and Preview). Otherwise the overlays anchored to it, the FPS counter first, are laid
// out for the other canvas.
void DesignCanvas::enter_viewport()
{
    if (!m_camera_swapped) swap_camera();
    if (m_canvas) { m_canvas->reset_old_size(); m_canvas->set_as_dirty(); }
}

void DesignCanvas::leave_viewport()
{
    if (m_camera_swapped) swap_camera();
    if (Plater* plater = wxGetApp().plater())
        if (GLCanvas3D* editor = plater->get_current_canvas3D())
            editor->reset_old_size();
}

void DesignCanvas::swap_camera()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr) return;
    std::swap(plater->get_camera(), m_parked_camera);
    m_camera_swapped = !m_camera_swapped;
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

void DesignCanvas::repaint_now()
{
    if (m_canvas == nullptr || m_canvas_widget == nullptr)
        return;
    request_repaint();            // mark dirty + Refresh (hardware GL) or render (software GL)
    m_canvas_widget->Update();    // service the pending paint immediately (a modal popup owns the loop)
}

void DesignCanvas::reload()
{
    m_canvas->reset_volumes();

    for (int i = 0; i < (int)m_model.objects.size(); ++i)
        m_canvas->load_object(m_model, i);

    const ColorRGBA ghost(0.26f, 0.66f, 1.0f, 0.45f);

    const auto& volumes = m_canvas->get_volumes().volumes;
    for (auto* v : volumes) {
        int obj_idx = v->object_idx();
        if (obj_idx == 0) {
            // Object 0 holds the bodies (rebuild_bodies): each volume in its body's colour, so
            // coexisting solids read as distinct parts (Onshape per-part colour), or in the
            // selection colour when it holds the body's selected faces.
            const int  vi  = v->volume_idx();
            const int  b   = vi < int(m_volumes.size()) ? m_volumes[vi].body : vi;
            const bool lit = vi < int(m_volumes.size()) && m_volumes[vi].lit;
            bool hidden = (b >= 0 && b < int(m_body_visible.size())) && !m_body_visible[b];
            // Preview-only mode (fillet/chamfer/draft, once a valid target is picked): hide
            // every base body so only the result ghost is on screen until Confirm.
            if (m_body_hidden) hidden = true;
            v->is_active = !hidden;   // per-body visibility toggle
            if (!hidden) {
                ColorRGBA c = lit ? design_selection_color() : body_color(b);
                if (b == m_hl_body_target)    c = ColorRGBA(0.30f, 0.90f, 0.70f, 1.0f); // target = teal-green
                else if (b == m_hl_body_tool) c = ColorRGBA(1.00f, 0.55f, 0.15f, 1.0f); // tool = orange
                if (m_body_translucent) c.a(0.30f);
                else if (m_xray_focus >= 0 && b != m_xray_focus) c.a(0.25f);
                v->set_color(c);
            }
        } else if (obj_idx == 1) {
            // The ghost is the whole resulting model, normally drawn as a faint blue overlay on
            // the visible bodies. Every face the feature leaves alone is in both, at the same
            // depth, so the ghost is drawn with a depth bias: the bodies win on those faces instead
            // of the two copies z-fighting, and the ghost shows only where the result reaches past
            // the bodies. In preview-only mode it IS the result (base bodies hidden), so render it
            // opaque so it reads as a finished solid rather than a see-through hint.
            v->set_color(m_body_hidden ? ColorRGBA(0.40f, 0.82f, 1.0f, 1.0f) : ghost);
            v->depth_bias = true;
        }
    }

    // The camera stays where the user left it, even for the first body: Home and a double-click
    // fit on demand.
    m_canvas->set_as_dirty();
    if (m_canvas_widget)
        m_canvas_widget->Refresh();
}

void DesignCanvas::set_bodies(const std::vector<TriangleMesh>* body_meshes,
                              const std::vector<bool>& visible)
{
    // Object 0 is built by rebuild_bodies; picking uses the combined mesh from set_solid_pick.
    m_body_visible = visible;   // empty => all visible; reload() reads this per volume
    if (body_meshes == nullptr || body_meshes->empty()) { clear_mesh(); return; }

    m_body_meshes = body_meshes;
    m_sketch_tool.refresh_body_edges();   // of the bodies set_solid_pick() pointed the tool at
    m_lit_faces   = m_sketch_tool.selected_faces();
    rebuild_bodies();
    reload();
}

void DesignCanvas::clear_mesh()
{
    m_body_meshes = nullptr;
    m_volumes.clear();
    // No solid, so no edge lines, pick or hover either, as after a rebuild that leaves no body.
    m_sketch_tool.set_solid_pick(nullptr, nullptr, nullptr, nullptr);
    if (!m_model.objects.empty()) {
        m_model.delete_object((size_t)0);
        reload();
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

    reload();
}

void DesignCanvas::clear_preview()
{
    if (m_model.objects.size() > 1) {
        m_model.delete_object((size_t)1);
        reload();
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

bool DesignCanvas::zoom_to_box(BoundingBoxf3 box)
{
    if (m_canvas == nullptr || !box.defined)
        return false;
    DesignSketchTool::pad_box(box);
    m_canvas->zoom_to_box(box);
    request_repaint();
    return true;
}

void DesignCanvas::set_view(const std::string& view_name)
{
    if (m_canvas) {
        if (view_name == "iso")
            select_iso_view(wxGetApp().plater()->get_camera());   // the Design camera while the tab is shown
        else
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

bool DesignCanvas::edit_sketch_selection_value()
{
    const bool ok = m_sketch_tool.open_selection_dimension_editor();
    if (ok) request_repaint();
    return ok;
}

int DesignCanvas::toggle_sketch_construction_selection()
{
    const int n = m_sketch_tool.toggle_selection_construction();
    if (n > 0) request_repaint();
    return n;
}

bool DesignCanvas::add_sketch_regions(
    const std::vector<std::vector<std::vector<Vec2d>>>& regions)
{
    const bool ok = m_sketch_tool.add_imported_regions(regions);
    if (ok) request_repaint();
    return ok;
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
    // No position: the Design bed stays at the printer bed's home, whichever plate is current.
    m_bed.set_shape(bed_shape_opt->values, printable_height, {}, {}, "", false);  // mainline added extruder_areas/heights params
}

void DesignCanvas::set_show_bed(bool b)
{
    if (!m_canvas) return;
    if (m_canvas->get_show_bed() == b) return;   // no repaint for a no-op toggle
    m_canvas->set_show_bed(b);
    request_repaint();
}

void DesignCanvas::set_sidebar_collapse(std::function<CollapseSide()> side, std::function<void()> toggle)
{
    if (!m_canvas || !m_canvas_widget || !setup_collapse_toolbar(m_collapse_toolbar, toggle))
        return;
    m_canvas->set_collapse_toolbar(&m_collapse_toolbar, std::move(side));
    m_canvas->enable_collapse_toolbar(true);
    // Shift+Tab: the canvas posts this for its sidebar, as Prepare's canvases do.
    m_canvas_widget->Bind(EVT_GLCANVAS_COLLAPSE_SIDEBAR, [toggle](SimpleEvent&) { toggle(); });
}

void DesignCanvas::set_sidebar_collapse_tooltip(const std::string& tooltip)
{
    m_collapse_toolbar.set_tooltip(m_collapse_toolbar.get_item_id("collapse_sidebar"), tooltip);
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

void DesignCanvas::set_on_sketch_step(std::function<void(DesignSketchTool::Mode, int, int)> cb)
{
    m_sketch_tool.on_step_changed = std::move(cb);
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

void DesignCanvas::set_on_sketch_face_selected(std::function<void(int)> cb)
{
    m_sketch_tool.on_face_selected = std::move(cb);
}

void DesignCanvas::set_on_display_sketch_selected(std::function<void(int, int, int)> cb)
{
    m_sketch_tool.on_display_sketch_selected = std::move(cb);
}

void DesignCanvas::set_on_display_sketch_activated(std::function<void(int)> cb)
{
    m_sketch_tool.on_display_sketch_activated = std::move(cb);
}

std::vector<SketchEntity> DesignCanvas::selected_loop_entities() const
{
    return m_sketch_tool.selected_loop_entities();
}

std::vector<std::vector<int>> DesignCanvas::region_entity_indices(const std::vector<SketchEntity>& ents) const
{
    return m_sketch_tool.region_entity_indices(ents);
}

std::vector<std::vector<int>> DesignCanvas::region_entity_indices_with_holes(const std::vector<SketchEntity>& ents) const
{
    return m_sketch_tool.region_entity_indices_with_holes(ents);
}

void DesignCanvas::clear_loop_pick()
{
    m_sketch_tool.clear_display_pick();
}

void DesignCanvas::clear_solid_pick()
{
    m_sketch_tool.clear_solid_selection();
    sync_selected_faces();
}

void DesignCanvas::set_loop_pick(int feature, int region)
{
    m_sketch_tool.set_display_pick(feature, region);
    request_repaint();
}

int DesignCanvas::loop_pick_feature() const
{
    return m_sketch_tool.display_pick();
}

int DesignCanvas::loop_pick_region() const
{
    return m_sketch_tool.display_pick_region();
}

void DesignCanvas::set_escalate_on_repick(bool on)
{
    m_sketch_tool.set_escalate_on_repick(on);
}

void DesignCanvas::set_solid_pick(const std::vector<CadBody>* bodies, const TriangleMesh* mesh,
                                  const std::vector<int>* tri_face, const std::vector<int>* tri_body,
                                  const std::vector<bool>* visible,
                                  const std::vector<Transform3d>* xform)
{
    m_color_bodies = bodies;   // stable address (m_doc.bodies); reload() reads colour overrides
    m_tri_face     = tri_face;
    m_sketch_tool.set_solid_pick(bodies, mesh, tri_face, tri_body, visible, xform);
    sync_selected_faces();     // the tool just reset its pick
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
                                       const Vec3d& axis_origin, const Vec3d& axis_dir, double angle, bool flip)
{
    m_sketch_tool.set_revolve_gizmo(plane, centroid, axis_origin, axis_dir, angle, flip);
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

std::vector<int> DesignCanvas::selected_solid_edges() const
{
    return m_sketch_tool.selected_edges();
}

void DesignCanvas::set_on_solid_selection_changed(std::function<void(int, int, int, int)> cb)
{
    // Wrapped so every pick change in the tool also re-splits the filled faces.
    m_on_solid_selection_changed = std::move(cb);
    m_sketch_tool.on_solid_selection_changed = [this](int level, int body, int face, int edge) {
        sync_selected_faces();
        if (m_on_solid_selection_changed)
            m_on_solid_selection_changed(level, body, face, edge);
    };
}

void DesignCanvas::set_on_empty_pick(std::function<void()> cb)
{
    m_sketch_tool.on_empty_pick = std::move(cb);
}

void DesignCanvas::select_body(int body)
{
    m_sketch_tool.select_body(body);
    sync_selected_faces();
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

void DesignCanvas::set_helix_gizmo(const SketchPlane& plane, double radius, double pitch,
                                   double height, double taper, bool left_handed)
{
    m_sketch_tool.set_helix_gizmo(plane, radius, pitch, height, taper, left_handed);
    request_repaint();
}

void DesignCanvas::clear_helix_gizmo()
{
    m_sketch_tool.clear_helix_gizmo();
    request_repaint();
}

void DesignCanvas::set_on_helix_changed(std::function<void(double, double, double)> cb)
{
    m_sketch_tool.on_helix_changed = std::move(cb);
}

void DesignCanvas::set_rib_gizmo(const SketchPlane& plane, const Vec2d& p0, const Vec2d& p1,
                                 double thickness)
{
    m_sketch_tool.set_rib_gizmo(plane, p0, p1, thickness);
    request_repaint();
}

void DesignCanvas::clear_rib_gizmo()
{
    m_sketch_tool.clear_rib_gizmo();
    request_repaint();
}

void DesignCanvas::set_on_rib_thickness_changed(std::function<void(double)> cb)
{
    m_sketch_tool.on_rib_thickness_changed = std::move(cb);
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

void DesignCanvas::set_selected_base(std::function<int()> cb)
{
    m_sketch_tool.selected_base = std::move(cb);
}

void DesignCanvas::set_on_sketch_exit(std::function<void()> cb)
{
    m_sketch_tool.on_exit = std::move(cb);
}

void DesignCanvas::set_on_sketch_exit_refused(std::function<void()> cb)
{
    m_sketch_tool.on_exit_refused = std::move(cb);
}

void DesignCanvas::set_on_sketch_notice(std::function<void(const std::string&, bool)> cb)
{
    m_sketch_tool.on_notice = std::move(cb);
}

void DesignCanvas::set_on_context_menu(std::function<void(const wxPoint&)> cb)
{
    m_on_context_menu = std::move(cb);
    if (!m_canvas_widget || m_ctx_bound)
        return;
    m_ctx_bound = true;
    // Bound AFTER GLCanvas3D's own handlers, so this runs first and can consume the event.
    // It only consumes when it actually opens the offer.
    // Right-drag may pan or orbit (Preferences > Control). Without remembering where the press
    // landed, every such drag ended by popping the offer over wherever the camera stopped — the
    // menu appearing as the reward for moving the view. A right-click is the release of a
    // STATIONARY press (kCadRightClickDriftPx); only that reaches the sketch tool or the offer.
    m_canvas_widget->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& e) {
        m_ctx_press     = e.GetPosition();
        m_ctx_travelled = false;
        // Drop any press the tool still keeps (only a click's release takes it, so a pan's stays)
        // before the canvas offers it this one, which ImGui may take instead.
        m_sketch_tool.drop_right_click();
        e.Skip();     // the canvas still needs the press to seed a pan or an orbit
    });
    m_canvas_widget->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        if (e.RightIsDown()) {
            const wxPoint d = e.GetPosition() - m_ctx_press;
            if (std::max(std::abs(d.x), std::abs(d.y)) > kCadRightClickDriftPx) m_ctx_travelled = true;
        }
        e.Skip();
    });
    m_canvas_widget->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent& e) {
        const wxPoint d  = e.GetPosition() - m_ctx_press;
        // Click, or navigation? A press that travelled panned or orbited; one that did not, did not.
        const bool is_click = !m_ctx_travelled && std::max(std::abs(d.x), std::abs(d.y)) <= kCadRightClickDriftPx;
        // Ending a chain or abandoning an anchor uses the click up.
        const bool terminated = is_click && m_canvas && m_sketch_tool.take_right_click(*m_canvas);
        if (terminated)
            m_canvas->set_as_dirty();   // drawn by the canvas's own RightUp (e.Skip below) or at idle
        else if (m_on_context_menu && !inline_busy() && is_click) {
            // The menu belongs to what you POINTED AT — and pointing happened at the PRESS, not
            // at the release, so the raycast uses the press position. Within a 3 px budget the
            // two are the same pixel in practice; using the press is what makes that a
            // guarantee rather than a coincidence. Pick first, so a right-click on a line offers
            // that line's verbs instead of the empty-selection vocabulary. Selecting an entity
            // that is already selected is a no-op, so a multi-entity pick survives a
            // right-click on one of its members.
            if (m_canvas && m_sketch_tool.select_at_screen(*m_canvas, m_ctx_press.x, m_ctx_press.y))
                request_repaint();
            m_on_context_menu(m_canvas_widget->ClientToScreen(m_ctx_press));
            return;   // consumed
        }
        e.Skip();
    });
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

void DesignCanvas::set_mate_connectors(std::vector<DesignSketchTool::MateConnectorGlyph> g)
{
    m_sketch_tool.set_mate_connectors(std::move(g));
    request_repaint();
}

void DesignCanvas::set_mate_links(std::vector<std::pair<Vec3d, Vec3d>> l)
{
    m_sketch_tool.set_mate_links(std::move(l));
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
    if (text == m_hud_last) return;
    m_hud_last = text;
    if (m_canvas) m_canvas->set_as_dirty();   // drawn by the next frame (the tool feeds this from one)
}

void DesignCanvas::set_status_text(const wxString& text, const wxColour& colour)
{
    if (text == m_status_hud_last && colour == m_status_hud_colour) return;
    m_status_hud_last   = text;
    m_status_hud_colour = colour;
    request_repaint();
}

void DesignCanvas::render_hud()
{
    if (m_hud_last.empty() && m_status_hud_last.IsEmpty()) return;
    ImGuiWrapper& imgui  = *wxGetApp().imgui();
    const ImVec2  ds     = ImGui::GetIO().DisplaySize;
    const float   em     = imgui.get_style_scaling();   // follows the font, so the DPI
    const float   margin = 12.f * em;
    const int     flags  = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    auto chip = [&](const char* id, const std::string& text, float x, float pivot_x, float wrap,
                    const ImVec4* colour) {
        ImGuiWrapper::push_common_window_style(m_canvas->get_scale());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * em, 4.f * em));
        imgui.set_next_window_pos(x, ds.y - margin, ImGuiCond_Always, pivot_x, 1.f);
        imgui.begin(std::string(id), flags);
        if (wrap > 0.f) ImGui::PushTextWrapPos(wrap);
        if (colour) ImGui::PushStyleColor(ImGuiCol_Text, *colour);
        ImGui::TextUnformatted(text.c_str());
        if (colour) ImGui::PopStyleColor();
        if (wrap > 0.f) ImGui::PopTextWrapPos();
        imgui.end();
        ImGui::PopStyleVar();
        ImGuiWrapper::pop_common_window_style();
    };
    if (!m_status_hud_last.IsEmpty()) {
        // Past the view cube and the round view buttons, which own the bottom-left corner. Asked
        // of the canvas, which lays them out: they follow the monitor's DPI on Windows, where `em`
        // does not, so a fixed inset in `em` let them cover the start of the line at 150%.
        // A sentence can be a sentence: it wraps to the room left of the readout chip.
        const float  left = m_canvas->get_canvas_toolbar_right() + margin;
        const ImVec4 col  = m_status_hud_colour.IsOk()
            ? ImVec4(m_status_hud_colour.Red() / 255.f, m_status_hud_colour.Green() / 255.f,
                     m_status_hud_colour.Blue() / 255.f, 1.f)
            : ImVec4();
        chip("##design_status", m_status_hud_last.ToUTF8().data(), left, 0.f,
             std::max(ds.x * 0.6f - left, 120.f * em), m_status_hud_colour.IsOk() ? &col : nullptr);
    }
    if (!m_hud_last.empty())
        chip("##design_readout", m_hud_last, ds.x - margin, 1.f, 0.f, &ImGuiWrapper::COL_ORCA);
}

void DesignCanvas::set_highlight_faces(const std::vector<std::pair<int, int>>& faces)
{
    m_sketch_tool.set_highlight_faces(faces);
    sync_selected_faces();
    request_repaint();
}

void DesignCanvas::sync_selected_faces()
{
    // Deferred to the end of the current event, which may change the selection several times:
    // every re-split reloads all the bodies. A set_bodies in between splits by the current
    // selection itself and leaves this nothing to do.
    if (m_split_pending)
        return;
    m_split_pending = true;
    CallAfter([this] {
        m_split_pending = false;
        std::vector<std::pair<int, int>> want = m_sketch_tool.selected_faces();
        if (want == m_lit_faces)
            return;
        m_lit_faces = std::move(want);
        if (m_body_meshes == nullptr || m_model.objects.empty())
            return;
        rebuild_bodies();
        reload();
    });
}

// Object 0: one volume per body, with its selected faces split off into a volume of their own.
// The per-triangle face ids (all bodies, in order) match triangles to faces; when their count
// does not match the meshes, nothing is split.
void DesignCanvas::rebuild_bodies()
{
    ModelObject* obj = m_model.objects.empty() ? m_model.add_object() : m_model.objects.front();
    obj->clear_volumes();
    m_volumes.clear();
    const std::vector<TriangleMesh>& meshes = *m_body_meshes;
    size_t ntri = 0;
    for (const TriangleMesh& m : meshes)
        ntri += m.its.indices.size();
    const bool mapped = m_tri_face != nullptr && m_tri_face->size() == ntri;
    size_t off = 0;
    for (int b = 0; b < int(meshes.size()); ++b) {
        const TriangleMesh& mesh = meshes[b];
        const auto first = std::lower_bound(m_lit_faces.begin(), m_lit_faces.end(), std::make_pair(b, INT_MIN));
        const auto last  = std::lower_bound(first, m_lit_faces.end(), std::make_pair(b + 1, INT_MIN));
        if (!mapped || first == last) {
            obj->add_volume(mesh);
            m_volumes.push_back({ b, false });
        } else {
            indexed_triangle_set parts[2];   // [0] the rest of the body, [1] its selected faces
            for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
                const int  f   = (*m_tri_face)[off + i];
                const bool lit = std::binary_search(first, last, std::make_pair(b, f));
                parts[lit].indices.push_back(mesh.its.indices[i]);
            }
            for (int lit = 0; lit < 2; ++lit) {
                if (parts[lit].indices.empty())
                    continue;
                parts[lit].vertices = mesh.its.vertices;
                its_compactify_vertices(parts[lit]);
                obj->add_volume(TriangleMesh(std::move(parts[lit])));
                m_volumes.push_back({ b, lit == 1 });
            }
        }
        off += mesh.its.indices.size();
    }
    if (obj->instances.empty())
        obj->add_instance();
}

void DesignCanvas::set_operand_bodies(int target_body, int tool_body)
{
    if (m_hl_body_target == target_body && m_hl_body_tool == tool_body) return;
    m_hl_body_target = target_body;
    m_hl_body_tool   = tool_body;
    reload();      // recolours the body volumes
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
    reload();   // re-applies object-0 alpha so the solid fades for the fillet preview
}

void DesignCanvas::set_xray_focus(int body)
{
    if (m_xray_focus == body) return;
    m_xray_focus = body;
    m_sketch_tool.set_pick_only_body(body);
    reload();   // re-applies per-body alpha so the non-focused bodies fade
}

void DesignCanvas::set_body_hidden(bool on)
{
    if (m_body_hidden == on) return;
    m_body_hidden = on;
    m_sketch_tool.set_body_edges_hidden(on);
    reload();   // hides/show base bodies + flips the ghost opaque/faint for preview-only mode
}

bool DesignCanvas::delete_selected_sketch_entities()
{
    if (m_sketch_tool.selection().empty()) return false;
    m_sketch_tool.delete_selected();
    request_repaint();
    return true;
}

bool DesignCanvas::inline_busy() const
{
    // Two sources, still: the TOOL's flag says a value is pending, the editor says a field is
    // drawn. They agree now that the field is not a window — the orphan state (logically closed,
    // still on screen, still eating keys) cannot be represented when there is nothing to leave
    // mapped — but the union costs nothing and is the honest question to ask.
    return m_sketch_tool.inline_busy()
           || (m_inline_editor && m_inline_editor->is_open());
}

bool DesignCanvas::inline_has_focus() const
{
    return m_inline_editor && m_inline_editor->has_focus();
}

void DesignCanvas::inline_commit()
{
    if (m_inline_editor) m_inline_editor->commit();
}

void DesignCanvas::inline_cancel()
{
    if (m_inline_editor) m_inline_editor->cancel();
}

void DesignCanvas::request_sketch_exit()
{
    m_sketch_tool.request_exit();
    request_repaint();
}

bool DesignCanvas::live_sketch_has_work() const
{
    return m_sketch_tool.live_sketch_has_work();
}

bool DesignCanvas::redo_last_sketch_entity()
{
    const bool did = m_sketch_tool.redo_last_entity();
    if (did) request_repaint();
    return did;
}

bool DesignCanvas::undo_last_sketch_entity()
{
    const bool did = m_sketch_tool.undo_last_entity();
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
    // the tool can project it; else fall back to the middle of the canvas, where the sketch is
    // in view. Everything here is canvas DEVICE px, the space the field is drawn in.
    const wxSize  cs = m_canvas_widget->GetClientSize();
    const double  sf = m_canvas_widget->GetContentScaleFactor();
    wxPoint anchor(int(cs.GetWidth() * sf) / 2, int(cs.GetHeight() * sf) / 2);
    m_sketch_tool.constrain_value_anchor(anchor);          // device px in the canvas viewport
    m_sketch_tool.set_inline_busy(true);
    m_canvas_widget->SetFocus();   // same reason as the draw-then-edit path: ImGui reads the
                                   // canvas's key events, so the canvas must be the focused widget

    m_inline_editor->open(anchor, current, "",
        [this, commit](double v) {
            m_sketch_tool.set_inline_busy(false);
            if (commit) commit(v);
            if (m_inline_editor && m_inline_editor->is_open()) m_sketch_tool.set_inline_busy(true);
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

int DesignCanvas::sketch_selection_count() const
{
    return int(m_sketch_tool.selection().size());
}

bool DesignCanvas::view_normal_to_sketch()
{
    if (m_canvas == nullptr) return false;
    const SketchPlane& pl = m_sketch_tool.plane();
    Camera& cam = wxGetApp().plater()->get_camera();
    // Keep the distance: this is an orientation change, not a zoom. The plane's own y axis is
    // the up vector, so "up" on screen is up in sketch coordinates — which is what makes a
    // dimension typed after pressing N land where the eye expects it.
    const double dist = cam.get_distance();
    cam.look_at(pl.origin + pl.normal * dist, pl.origin, pl.y_axis);
    request_repaint();
    return true;
}

bool DesignCanvas::sketch_abort_gesture()
{
    if (!m_sketch_tool.abort_gesture()) return false;
    request_repaint();     // the rubber band is gone; the canvas must stop drawing it
    return true;
}

bool DesignCanvas::sketch_confirm_pending()
{
    if (!m_sketch_tool.confirm_pending()) return false;
    request_repaint();
    return true;
}

bool DesignCanvas::sketch_disarm_tool()
{
    if (!m_sketch_tool.disarm_tool()) return false;
    request_repaint();
    return true;
}

bool DesignCanvas::drawing_in_progress() const
{
    return m_sketch_tool.gesture_pending();
}

bool DesignCanvas::has_any_selection() const
{
    return m_sketch_tool.has_solid_selection() || m_sketch_tool.sketch_has_selection()
        || m_sketch_tool.display_pick() >= 0;
}

bool DesignCanvas::clear_any_selection()
{
    if (!has_any_selection()) return false;
    // All three, unconditionally: which is live depends on the mode, and Esc at idle means
    // "nothing is picked" in any of them. clear_selection() reports through the tool's own
    // on_selection_changed; the solid side has no such notification, so the panel refreshes what
    // depends on it (see DesignPanel::escape).
    m_sketch_tool.clear_selection();
    m_sketch_tool.clear_solid_selection();
    m_sketch_tool.clear_display_pick();
    // clear_solid_selection() is silent by design (recomputes call it while ids are invalid), but
    // the panel mirrors the pick to aim Extrude and the dress-up tools. An Esc that cleared the
    // highlight without telling the panel would leave those aimed at a body nothing points to.
    if (m_sketch_tool.on_solid_selection_changed)
        m_sketch_tool.on_solid_selection_changed(0, -1, -1, -1);
    request_repaint();
    return true;
}

bool DesignCanvas::sketch_first_selected_type(SketchEntity::Type& out) const
{
    return m_sketch_tool.first_selected_type(out);
}

const std::vector<int>& DesignCanvas::sketch_selection() const
{
    return m_sketch_tool.selection();
}

const std::vector<SketchEntity>& DesignCanvas::sketch_entities() const
{
    return m_sketch_tool.entities();
}

int DesignCanvas::sketch_constraint_count() const
{
    return int(m_sketch_tool.constraints().size());
}

const std::vector<SketchEntityConstraintDef>& DesignCanvas::sketch_constraints() const
{
    return m_sketch_tool.constraints();
}

bool DesignCanvas::remove_sketch_constraint(int idx)
{
    const bool removed = m_sketch_tool.remove_constraint(idx);
    if (removed) request_repaint();
    return removed;
}

void DesignCanvas::set_on_sketch_constraints_changed(std::function<void()> cb)
{
    m_sketch_tool.on_constraints_changed = std::move(cb);
}

bool DesignCanvas::try_add_sketch_constraints(const std::vector<SketchEntityConstraintDef>& defs)
{
    return m_sketch_tool.try_add_constraints(defs);
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

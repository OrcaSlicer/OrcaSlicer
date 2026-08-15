#include "DesignPanel.hpp"
#include "DesignCanvas.hpp"
#include "DesignSketchTool.hpp"
#include "DesignOffer.hpp"                // generated offer table — see docs/ux/tool_atlas.json
#include "libslic3r/GeometryEngine.hpp"   // face_by_index for face-extrude gizmo anchor
#include "libslic3r/TriangleMesh.hpp"     // mesh import: STL/OBJ -> indexed_triangle_set
#include "libslic3r/Format/OBJ.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>          // the offer/atlas join check reports on the log
#include <Standard_Failure.hxx>

#include <cassert>
#include <map>
#include <set>
#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/spinctrl.h>
#include <wx/listctrl.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/statline.h>
#include <wx/statbmp.h>
#include <wx/image.h>
#include <wx/font.h>
#include <wx/textdlg.h>
#include <wx/filedlg.h>
#include <wx/dialog.h>
#include <wx/colordlg.h>
#include <wx/menu.h>
#include <wx/progdlg.h>
#include <wx/utils.h>    // wxWindowDisabler, wxMilliSleep
#include <wx/msgdlg.h>   // wxMessageBox

#include <string>
#include <memory>
#include <functional>
#include <cmath>
#include <algorithm>
#include <thread>
#include <atomic>

#include "slic3r/GUI/wxExtensions.hpp"   // ScalableButton, create_scaled_bitmap
#include "Widgets/Label.hpp"             // HarmonyOS Sans fonts (Head_*/Body_*) shared with the rest of Orca
#include "Widgets/DropDown.hpp"          // Orca-themed combo dropdown (white/teal selector) for the tool flyouts
#include "Widgets/Button.hpp"            // Orca-styled Button (ButtonStyle/ButtonType) — same look as Prepare
#include "Widgets/CheckBox.hpp"          // Orca teal check (label lives in the row's left column)
#include "Widgets/ComboBox.hpp"          // Orca dropdown — replaces wxChoice in every Design card
#include "Widgets/StaticBox.hpp"         // Prepare's rounded white card frame around each tool dialog
#include "libslic3r/SketchImport.hpp"    // text_to_regions / svg_to_regions
#include "libslic3r/ThreadStandards.hpp" // ISO metric / Unified imperial thread tables
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"

// English-only pin for the Design tab (see snaporca-design-ux-contract): one lever
// de-translates this whole TU so our strings never half-translate against the host's
// localized chrome. Host UI still follows the app locale; only this tab is pinned EN.
// GOTCHA: every _L(...) in this file must take a STRING LITERAL (FromUTF8 wants const char*).
#ifdef _L
#undef _L
#endif
#define _L(s) wxString::FromUTF8(s)

namespace Slic3r { namespace GUI {

// Mesh -> B-rep import defaults (see GeometryEngine::mesh_to_brep).
// Tolerance is a vertex-dedup cell, not a sew tolerance: 10 um is well under any printable
// feature yet coarse enough to weld the float noise a mesh exporter leaves on shared vertices.
static constexpr double MESH_IMPORT_TOLERANCE        = 0.01;   // mm
// Merge coplanar neighbours so the body has real, pickable faces instead of one face per
// triangle. 5 deg tolerates the small normal jitter of an exported/scanned flat face while
// still keeping genuinely curved regions faceted.
static constexpr double MESH_IMPORT_MERGE_ANGLE_DEG  = 5.0;
// Above this, warn before converting: the build is one OCCT face per triangle.
static constexpr size_t MESH_IMPORT_TRIANGLE_WARN    = 50000;

// Two-column parameter form (label left, control right), matching Prepare's row idiom. The
// control column grows, so every control lines up on the panel's right edge instead of sitting
// at its natural width beside the label — the forms used a plain non-growable grid before, which
// is why Design's rows looked nothing like Prepare's.
static wxFlexGridSizer* two_col_form()
{
    auto* f = new wxFlexGridSizer(2, 6, 8);
    f->AddGrowableCol(1, 1);
    f->SetFlexibleDirection(wxHORIZONTAL);
    return f;
}


// Orca's dropdown, sized like Prepare's sidebar combos. Read-only: every Design
// picker is a fixed list, never free text.
static ComboBox* make_combo(wxWindow* parent)
{
    // Explicit width: ComboBox's natural best size is its widest item, which inflated the
  // sidebar's virtual width past the viewport and left the panel horizontally scrolled —
  // that is what clipped the first letter of every label. The form column grows anyway.
    auto* c = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition,
                           wxSize(parent->FromDIP(90), parent->FromDIP(24)), 0, nullptr, wxCB_READONLY);
    c->SetMinSize(wxSize(parent->FromDIP(90), parent->FromDIP(24)));
    return c;
}

// Append a row that carries a feature/body index as client data.
//
// It must go through SetClientData(), not Append()'s clientData argument. Orca's ComboBox keeps
// client data in its own vector and its Append() writes that vector directly, never routing
// through wxItemContainer — so the container's m_clientDataItemsType stays wxClientData_None.
// wxItemContainer::GetClientData() opens with
//     wxCHECK_MSG( HasClientUntypedData(), NULL, ... );
// and wxCHECK_MSG is an early RETURN, not a debug-only assert. So the read handed back NULL for
// every row and every caller resolved it to index 0 no matter what the user had picked — silently,
// because index 0 is a legal answer. SetClientData() flips the type to wxClientData_Void, after
// which the value survives the round trip.
static int combo_append_index(ComboBox* c, const wxString& label, int index)
{
    const int row = c->Append(label, wxNullBitmap);
    c->SetClientData(unsigned(row), reinterpret_cast<void*>(intptr_t(index)));
    return row;
}

// Format a value with the international ('.') decimal separator regardless of the
// app's LC_NUMERIC locale (wx sets it to the user locale at startup). snprintf may
// emit a comma, so normalise it.
static wxString en_format(double v, int digits = 2)
{
    char fmt[16];
    std::snprintf(fmt, sizeof(fmt), "%%.%df", digits);
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    for (char* c = buf; *c; ++c) if (*c == ',') *c = '.';
    return wxString::FromUTF8(buf);
}
// Parse a user-typed value accepting either '.' or ',' as the decimal separator.
static bool en_parse(const wxString& text, double& out)
{
    wxString t(text);
    t.Replace(wxT(","), wxT("."));
    return t.ToCDouble(&out);
}

// Design-tab chrome tokens. The dark branch returns the EXACT legacy values so the
// (correct) dark theme stays byte-identical; the light branch maps each onto Orca's
// light surface so the ribbon/sidebar follow the app theme instead of staying black.
static bool     dp_dark()         { return wxGetApp().dark_mode(); }
static wxColour dp_ribbon_bg()    { return dp_dark() ? wxColour(0x36,0x36,0x3C) : wxColour(0xEC,0xEC,0xEE); }
static wxColour dp_ribbon_hover() { return dp_dark() ? wxColour(0x4D,0x4D,0x54) : wxColour(0xD7,0xD7,0xDB); }
static wxColour dp_panel_bg()     { return dp_dark() ? wxColour(0x2D,0x2D,0x30) : wxColour(0xFB,0xFB,0xFD); }
static wxColour dp_sec_text()     { return dp_dark() ? wxColour(0x81,0x81,0x83) : wxColour(0x66,0x66,0x68); }
static wxColour dp_ctl_text()     { return dp_dark() ? wxColour(0xC8,0xC8,0xC8) : wxColour(0x35,0x35,0x37); }
static wxColour dp_item_text()    { return dp_dark() ? wxColour(0xE0,0xE0,0xE0) : wxColour(0x2C,0x2C,0x2E); }
static wxColour dp_item_dim()     { return dp_dark() ? wxColour(0x80,0x80,0x80) : wxColour(0xA0,0xA0,0xA2); }

// Prepare's control-outline grey, sampled from its sidebar: #4A4A51 on the #2D2D31 dark
// panel, #DBDBDB on light. Every framed thing in Design uses this so the tab matches.
static wxColour dp_border_col()   { return dp_dark() ? wxColour(0x4A,0x4A,0x51) : wxColour(0xDB,0xDB,0xDB); }

// A tool card: Prepare's rounded white-bordered panel (Plater.cpp's panel_printer_preset
// idiom — radius 8, #EEEEEE border, green on hover). Every card's controls are parented
// to it, so the border actually encloses them.
static StaticBox* make_card(wxWindow* parent)
{
    auto* c = new StaticBox(parent);
    c->SetCornerRadius(8);
    c->SetBorderColorNormal(dp_border_col());   // no hover accent: the frame is not clickable
    return c;
}

// Prepare outlines every numeric field (rounded, #4A4A51 on dark). wxSpinCtrlDouble is a
// native GTK control that cannot draw that, and Orca's own SpinInput is int-only — it would
// silently truncate a 2.5 mm radius. So the double spin keeps its arrows and validation and
// sits inside an Orca StaticBox that supplies the frame. spin_frame() returns that box, which
// is what goes into the layout sizer.
static wxSpinCtrlDouble* make_spin(wxWindow* parent, double val,
                                   double mn = 0.1, double mx = 1000.0)
{
    auto* box = new StaticBox(parent);
    box->SetCornerRadius(4);
    box->SetBorderColorNormal(dp_border_col());
    auto* s = new wxSpinCtrlDouble(box, wxID_ANY, "", wxDefaultPosition, wxSize(90, -1),
                                   wxSP_ARROW_KEYS | wxBORDER_NONE);
    s->SetRange(mn, mx);
    s->SetDigits(2);
    s->SetValue(val);
    // THE WHEEL SCROLLS THE PANEL, IT DOES NOT EDIT THE VALUE. wxSpinCtrlDouble takes the wheel
    // whenever the pointer is over it, so a card taller than the panel could not be scrolled past
    // without silently incrementing whatever field happened to be under the cursor — measured:
    // eight notches turned an X hint from 1,00 into 6,00, and the same gesture over an Extrude
    // distance or a mate Offset is a silent model change made by someone who thought they were
    // navigating. Skipping the event lets it reach the scrolled cards panel. A spin the user has
    // deliberately focused still takes the wheel, which is the one case where editing is meant.
    s->Bind(wxEVT_MOUSEWHEEL, [s](wxMouseEvent& e) {
        if (wxWindow::FindFocus() == s) e.Skip();          // focused: wheel edits, as expected
        else if (wxWindow* p = s->GetParent())             // otherwise hand it to the panel
            wxPostEvent(p, e);
    });
    auto* sz = new wxBoxSizer(wxHORIZONTAL);
    sz->Add(s, 1, wxEXPAND | wxALL, parent->FromDIP(2));
    box->SetSizer(sz);
    return s;
}

// The framed spin's parent IS its frame; lay that out instead of the bare control.
static wxWindow* spin_frame(wxWindow* spin) { return spin->GetParent(); }

static SketchPlane plane_from_index(int i)
{
    switch (i) {
        case 1:  return SketchPlane::XZ();
        case 2:  return SketchPlane::YZ();
        default: return SketchPlane::XY();
    }
}

// Inverse of plane_from_index: recover the ComboBox row from a plane's normal.
// XY normal=(0,0,1)->0, XZ normal=(0,1,0)->1, YZ normal=(1,0,0)->2.
static int index_from_plane(const SketchPlane& p)
{
    if (std::abs(p.normal.y()) > 0.5) return 1; // XZ
    if (std::abs(p.normal.x()) > 0.5) return 2; // YZ
    return 0;                                   // XY
}

// Does this plane survive a round trip through the Hole/Thread plane dropdown? The dropdown holds
// only XY/XZ/YZ and index_from_plane SNAPS anything else to the nearest of the three, so a plane
// that is not one of them cannot be restored from the row and has to be carried explicitly.
// Origin is compared too, not just the axes: a face plane parallel to XY but 12 mm up snaps to
// row 0 and would otherwise come back at z=0. Vector norms rather than isApprox, which compares
// relative to magnitude and so is useless against the zero origin.
// modeling_origin is not optional: hole_plane()/thread_plane() ADD it to the dropdown plane before
// the feature stores it, so on a document with a shifted origin every dropdown hole would fail a
// bare XY/XZ/YZ comparison and be mistaken for a face pick.
static bool is_base_plane(const SketchPlane& p, const Vec3d& modeling_origin)
{
    SketchPlane  b = plane_from_index(index_from_plane(p));
    b.origin += modeling_origin;
    const double e = 1e-6;
    return (p.origin - b.origin).norm() < e && (p.normal - b.normal).norm() < e
        && (p.x_axis - b.x_axis).norm() < e && (p.y_axis - b.y_axis).norm() < e;
}

// #2: a sketch plane on `face` with origin at the face centroid and normal pointing INTO the
// solid, so a positioned hole drills inward and its (x,y) read as the offset from the face
// centre. A hole is rotationally symmetric, so the arbitrary in-plane basis is harmless.
static SketchPlane face_plane_inward(const TopoDS_Face& face)
{
    SketchPlane p;
    p.origin = GeometryEngine::face_centroid_world(face);
    p.normal = (-GeometryEngine::face_normal_world(face)).normalized();   // inward
    // Align the in-plane x-axis with the face's LONGEST straight edge so the (u,v) frame matches
    // the face sides — then "distance from a side" (the hole construction dims) reads correctly.
    Vec3d x(0, 0, 0); double best = 0;
    for (const TopoDS_Edge& e : GeometryEngine::edges_of_face(face)) {
        const std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(e);
        if (pts.size() < 2) continue;
        Vec3d d = pts.back() - pts.front();
        d = d - p.normal * d.dot(p.normal);        // project the edge direction into the plane
        const double len = d.norm();
        if (len > best) { best = len; x = d / len; }
    }
    if (best < 1e-9) {   // curved/edgeless face: fall back to an arbitrary in-plane basis
        const Vec3d ref = std::abs(p.normal.z()) < 0.9 ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
        x = ref.cross(p.normal).normalized();
    }
    p.x_axis = x.normalized();
    p.y_axis = p.normal.cross(p.x_axis).normalized();
    return p;
}

// Highest upward-facing planar face of a solid — the surface the user is looking down on.
// Hole placement defaults here (instead of the z=0 datum) so the footprint sits on the top
// face at the right depth, not on the model's underside where a top-view drag reads parallax-
// shifted. Returns -1 if the shape has no clearly-upward face.
static int top_face_index_of(const TopoDS_Shape& shape)
{
    int best = -1; double bestz = -1e30;
    const int n = GeometryEngine::face_count(shape);
    for (int i = 0; i < n; ++i) {
        const TopoDS_Face f = GeometryEngine::face_by_index(shape, i);
        if (f.IsNull()) continue;
        const Vec3d nrm = GeometryEngine::face_normal_world(f);
        if (nrm.z() < 0.5) continue;                 // only faces pointing substantially up
        const Vec3d c = GeometryEngine::face_centroid_world(f);
        if (c.z() > bestz) { bestz = c.z(); best = i; }
    }
    return best;
}

DesignPanel::DesignPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    // Left column: a slim feature-tree + docked tool-dialog column. All form
    // controls are parented to m_form so it can scroll independently of the
    // live GL viewport. The tool buttons live in the top toolbar (built below).
    m_form = new wxScrolledWindow(this, wxID_ANY);
    // The sidebar/panel never carried an explicit background, so in light theme it
    // inherited the dark window colour and stayed black. Paint it on the light surface;
    // dark is left untouched (it already reads correctly via inheritance).
    if (!dp_dark()) {
        SetBackgroundColour(dp_panel_bg());
        m_form->SetBackgroundColour(dp_panel_bg());
    }

    auto* root = new wxBoxSizer(wxVERTICAL);
    {
        auto* hdr = new wxStaticText(m_form, wxID_ANY, _L("Design"));
        hdr->SetFont(Label::Head_16);   // Orca shared HarmonyOS section-title font
        root->Add(hdr, 0, wxLEFT | wxRIGHT | wxTOP, 12);
        root->AddSpacer(2);
    }

    // === Top contextual toolbar (Onshape-style icon strip) ===
    // Parented to the panel (sits above the form/viewport row). Only the active
    // mode's group is shown; the others are hidden by set_ui_mode().
    // Scrollable ribbon: on a narrow/windowed screen the far-right action bar (Confirm/Cancel)
    // used to be clipped off the edge with no way to reach it. Horizontal-only scroll (vertical
    // rate 0) keeps it reachable; on a wide screen the stretch spacer still pins it far-right.
    m_toolbar = new wxScrolledWindow(this, wxID_ANY);
    m_toolbar->SetScrollRate(15, 0);
    m_toolbar->ShowScrollbars(wxSHOW_SB_DEFAULT, wxSHOW_SB_NEVER);
    // Theme-aware tool ribbon: dark uses Orca's elevated surface (#36363C / hover
    // #4D4D54); light maps onto the app's light chrome so the strip follows the theme.
    m_toolbar->SetBackgroundColour(dp_ribbon_bg());

    const wxColour tb_bg    = dp_ribbon_bg();
    const wxColour tb_hover = dp_ribbon_hover();
    auto icon_btn = [this, tb_bg, tb_hover](const char* icon, const wxString& tip) {
        // Prepare's main toolbar: 40 px icon cell, 4 px gap (GLToolbar::Default_Icons_Size
        // and set_gap_size(4)) -> 44 px pitch. Match it exactly.
        auto* b = new ScalableButton(m_toolbar, wxID_ANY, icon, "", wxSize(40, 40),
                                     wxDefaultPosition, wxBU_EXACTFIT | wxBORDER_NONE, false, 34);
        b->SetToolTip(tip);
        b->SetBackgroundColour(tb_bg);
        m_tool_btns.push_back(b);
        // Hover affordance, honouring the active-tool teal state.
        b->Bind(wxEVT_ENTER_WINDOW, [this, b, tb_hover](wxMouseEvent& e) {
            b->SetBackgroundColour(b == m_active_tool_btn ? wxColour(0x52, 0xC7, 0xB8) : tb_hover);
            b->Refresh(); e.Skip(); });
        b->Bind(wxEVT_LEAVE_WINDOW, [this, b, tb_bg](wxMouseEvent& e) {
            b->SetBackgroundColour(b == m_active_tool_btn ? wxColour(0x00, 0x96, 0x88) : tb_bg);
            b->Refresh(); e.Skip(); });
        // Mark this tool active (teal) on press — a separate event from the
        // button's command handler, so it never swallows the click action.
        b->Bind(wxEVT_LEFT_DOWN, [this, b](wxMouseEvent& e) {
            set_active_tool_btn(b); e.Skip(); });
        return b;
    };
    // Small grey group caption (Onshape-style section hint) for each toolbar mode.
    auto caption = [this](const wxString& t) {
        auto* s = new wxStaticText(m_toolbar, wxID_ANY, t);
        wxFont f = Label::Body_12; f.SetWeight(wxFONTWEIGHT_BOLD);
        s->SetFont(f);
        s->SetForegroundColour(dp_sec_text());   // Orca dark secondary text
        return s;
    };
    auto add_sep = [this](wxSizer* row) {
        row->AddSpacer(5);
        row->Add(new wxStaticLine(m_toolbar, wxID_ANY, wxDefaultPosition, wxSize(1, 22), wxLI_VERTICAL),
                 0, wxALIGN_CENTER_VERTICAL);
        row->AddSpacer(5);
    };

    // Shared sketch-tool selector: begins a session on first use, then switches
    // the active entity tool. The Construction toggle marks following entities as
    // construction geometry (excluded from the wire).
    m_construction = new wxCheckBox(m_toolbar, wxID_ANY, _L("Construction"));
    m_construction->SetForegroundColour(dp_ctl_text());
    auto select_tool = [this](DesignSketchTool::Mode mode, const wxString& hint) {
        if (!m_viewport) return;
        if (!m_viewport->is_sketching()) {
            wxString on;
            const SketchPlane plane = sketch_plane_from_selection(on);
            m_viewport->begin_sketch(plane, mode);
            m_construction->SetValue(false);   // a fresh session starts non-construction
            m_sketch_on = on;                  // shown with the tool hint, so the target is visible
            // The face has been CONSUMED as the sketch plane, so drop the pick. Leaving it live
            // meant the next Extrude saw a selected face and push/pulled it instead of extruding
            // the sketch just drawn — the same trap the imported-art path already guards against.
            if (m_sel_solid_face >= 0 || m_pick_face >= 0) {
                m_sel_solid_face = m_sel_solid_edge = m_sel_solid_body = -1;
                m_pick_face = m_pick_face_body = -1;
            }
        } else {
            m_viewport->set_sketch_tool(mode);
        }
        m_viewport->set_sketch_construction(m_construction->GetValue());
        m_status->SetForegroundColour(wxNullColour);
        set_status(m_sketch_on.IsEmpty() ? hint
                           : wxString::Format(_L("%s  ·  on %s"), hint, m_sketch_on));
        m_status->Refresh();
    };

    // Sketch-tool shortcuts (single letters, active only while a sketch is open). Family tools
    // bind to their default mode; the other modes stay in the toolbar flyout. Registered here
    // where select_tool is in scope; the closures run at key-press time (members are live by then).
    auto sk_key = [this, select_tool](int ch, DesignSketchTool::Mode m, const wxString& h) {
        m_keys_sketch[ch] = [this, select_tool, m, h] { select_tool(m, h); };
    };
    sk_key('L', DesignSketchTool::Mode::Line,         _L("Line — click start, then end"));
    sk_key('R', DesignSketchTool::Mode::CornerRect,   _L("Rectangle — click two opposite corners"));
    sk_key('C', DesignSketchTool::Mode::CenterCircle, _L("Circle — click center, then radius"));
    sk_key('A', DesignSketchTool::Mode::ThreePointArc,_L("Arc — click start, end, then a point"));
    sk_key('S', DesignSketchTool::Mode::Slot,         _L("Slot — two centerline ends, then end radius"));
    sk_key('E', DesignSketchTool::Mode::Ellipse,      _L("Ellipse — center, major end, minor point"));
    sk_key('B', DesignSketchTool::Mode::BSpline,      _L("Spline — click control points"));
    sk_key('P', DesignSketchTool::Mode::Point,        _L("Point — click to place"));
    sk_key('D', DesignSketchTool::Mode::Dimension,    _L("Dimension — click 2 points or an entity"));
    sk_key('T', DesignSketchTool::Mode::Trim,         _L("Trim — click a segment to trim it"));
    sk_key('X', DesignSketchTool::Mode::Extend,       _L("Extend — click a line/arc to extend it"));
    sk_key('O', DesignSketchTool::Mode::Offset,       _L("Offset — pick an entity, drag the distance"));
    sk_key('M', DesignSketchTool::Mode::Mirror,       _L("Mirror — pick axis, then entities"));
    sk_key('F', DesignSketchTool::Mode::Fillet,       _L("Fillet — pick two lines, set the radius"));
    sk_key('H', DesignSketchTool::Mode::Chamfer,      _L("Chamfer — pick two lines, set the distance"));
    // Polygon needs its side count / circumscribed flag pushed to the tool before it starts.
    m_keys_sketch['G'] = [this, select_tool] {
        if (m_viewport) {
            push_polygon_params();
        }
        select_tool(DesignSketchTool::Mode::Polygon, _L("Polygon — click center, then a vertex"));
    };
    // Constrain (finish the live sketch + enter constrain), and Construction toggle.
    m_keys_sketch['K'] = [this] { enter_constrain_inline(); };
    m_keys_sketch['Q'] = [this] {
        if (m_construction) {
            m_construction->SetValue(!m_construction->GetValue());
            if (m_viewport && m_viewport->is_sketching())
                m_viewport->set_sketch_construction(m_construction->GetValue());
        }
    };

    // Shift+letter encoder for the feature-tool shortcuts (registered via FeatVar::key below,
    // and explicitly for the standalone feature buttons).
    auto SHIFT = [](int ch) { return ch | SC_SHIFT; };

    // View toggles (single letters, active when no sketch is open): P origin planes, A world
    // axes, X section view (Alt+Wheel slides the cut). Distinct from Shift+P/Shift+X features.
    auto status_flag = [this](const wxString& on_msg, const wxString& off_msg, bool on) {
        m_status->SetForegroundColour(wxNullColour);
        set_status(on ? on_msg : off_msg);
        m_status->Refresh();
    };
    m_keys_feature['P'] = [this, status_flag] {
        if (m_viewport) status_flag(_L("Origin planes shown"), _L("Origin planes hidden"),
                                    m_viewport->toggle_planes());
    };
    m_keys_feature['A'] = [this, status_flag] {
        if (m_viewport) status_flag(_L("World axes shown"), _L("World axes hidden"),
                                    m_viewport->toggle_axes());
    };
    m_keys_feature['X'] = [this] { toggle_section_view(); };   // toggle the single section on/off

    // Home: axonometric view, fitted to the model.
    //
    // DesignCanvas::set_view() and fit_view() were written and then never called from
    // anywhere in the tree, so the Design viewport had NO way back to a standard view —
    // no key, no button, nothing but orbiting by hand until the model happened to be in
    // frame. A camera left pointing along the bed plane looks exactly like a renderer
    // that has failed, which is how this was found.
    //
    // Home rather than a letter because every letter A-Z is already a Shift+letter tool
    // shortcut, and because Home is the reset-the-view key users arrive with. set_view()
    // already does select_view + zoom_to_volumes, so this is fit and orient in one.
    m_keys_feature[WXK_HOME] = [this] {
        if (!m_viewport) return;
        m_viewport->set_view("iso");
        set_status(_L("Isometric view, fitted"));
    };

    // Shared flyout glyph tint (used by BOTH the feature and sketch toolbars). Re-tint each
    // design_* glyph to the DropDown's resolved TEXT colour so it reads on the popup in either
    // theme: text_color is 0x363636, which darkModeColorFor() maps to a light tone in dark mode
    // (the popup bg is darkModeColorFor(white) = dark) and leaves dark in light mode. The alpha
    // (the glyph shape) is preserved; only RGB is replaced.
    // ponytail: wxBitmap(img) drops the HiDPI scale factor (no scale ctor before wx 3.1.6); the
    // deploy target runs at scale 1.0, so this is exact there.
    const wxColour drop_icon_col = StateColor::darkModeColorFor(wxColour(0x36, 0x36, 0x36));
    auto tint = [](wxBitmap bmp, const wxColour& c) -> wxBitmap {
        if (!bmp.IsOk()) return bmp;
        wxImage img = bmp.ConvertToImage();
        if (!img.HasAlpha()) img.InitAlpha();
        const int w = img.GetWidth(), h = img.GetHeight();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                img.SetRGB(x, y, c.Red(), c.Green(), c.Blue());
        return wxBitmap(img);
    };

    // --- Feature group: Sketch / Extrude / Fillet-Chamfer / Hole / Thread / Constrain
    m_tb_feature = new wxBoxSizer(wxHORIZONTAL);
    // Buttons are created in whatever order reads best in code, but laid out in the order the
    // user specified. fadd() records into tb_slot; the flush below the doc group emits them.
    // A dropdown occupies two entries (button + chevron) that must stay adjacent.
    std::map<std::string, std::vector<wxWindow*>> tb_slot;
    // THE TOOLBAR IS CHROME. Every CAD verb is reached from the offer, so a tool's button is
    // still BUILT — that is what registers its "fly:<family>#<row>" address and its Shift+key —
    // but it is never placed on the bar. Hiding rather than skipping construction is deliberate:
    // the addresses are created inside the widget-building loops, so not building would silently
    // delete 42 verbs from the offer while they still rendered. snaporca-7ih records the cleanup
    // that lets the construction go away too.
    // What stays: the two doc-row imports (consumed by add_doc below) and the view controls,
    // which are chrome_only in the atlas and so have no offer row to fall back on.
    static const std::set<std::string> kBarKeep = { "step", "mesh", "place", "section", "flip" };
    auto fadd = [&tb_slot](const char* id, wxWindow* w) {
        if (kBarKeep.count(id) == 0) { w->Hide(); return; }
        tb_slot[id].push_back(w);
    };
    m_tb_feature->Add(caption(_L("FEATURES")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    {
        // Onshape-style FEATURE flyouts: same themed-DropDown pattern as the sketch toolbar
        // (tinted glyphs, Body_14 measure, content-width popup) but each entry runs an
        // arbitrary action — the existing per-feature handler — instead of selecting a Mode.
        struct FeatVar { const char* icon; wxString tip; wxString hint; std::function<void()> action; int key = 0; };
        struct FeatFlyout {
            std::vector<DropDown::Item> items;   // mainline DropDown is Item-based (text/tip/icon per row)
            std::vector<std::function<void()>> actions;
            std::vector<std::string> icon_names;
            ScalableButton* btn = nullptr;
            DropDown drop;                 // declared LAST: destroyed before the vector it references
            FeatFlyout() : drop(items) {}
        };
        auto feat_dropdown = [&](const char* id, const char* def_icon, const wxString& grp, std::vector<FeatVar> vars) {
            // REGISTER FIRST, BUILD SECOND. A verb's two addresses — "fly:<family>#<row>" for the
            // offer and its Shift+key — are data. The widget is one door onto them, not their
            // owner. Doing this before any wxWindow exists is what lets the build below be skipped
            // outright for a family the bar no longer carries. It used to sit INSIDE the build
            // loop, so a retired family still had to be constructed and then Hide()n: skipping it
            // would have deleted 42 verbs from the offer while their rows still rendered and did
            // nothing when picked. snaporca-7ih.
            // Keyed on "fly:<family>#<row>" so the generated table can name a variant without the
            // item struct growing a field at 26 call sites.
            for (size_t i = 0; i < vars.size(); ++i) {
                if (vars[i].key) m_keys_feature[vars[i].key] = vars[i].action;   // key runs the same action
                m_verb_actions["fly:" + std::string(id) + "#" + std::to_string(i)] = vars[i].action;
            }
            if (kBarKeep.count(id) == 0)
                return;   // reached from the offer alone — no button, no chevron, no popup
            auto* b = icon_btn(def_icon, grp);
            b->SetFont(Label::Body_14);   // measure popup labels in the popup's font (no truncation)
            auto fo = std::make_shared<FeatFlyout>();
            for (auto& v : vars) {
                DropDown::Item it;
                it.text = v.tip;
                it.tip  = v.hint;
                it.icon = tint(create_scaled_bitmap(v.icon, m_form, 18), drop_icon_col);
                fo->items.push_back(it);
                fo->actions.push_back(std::move(v.action));
                fo->icon_names.emplace_back(v.icon);
            }
            fo->btn = b;
            fo->drop.Create(b);
            fo->drop.SetUseContentWidth(true, false);
            fo->drop.Invalidate(true);
            FeatFlyout* fp = fo.get();
            fo->drop.Bind(wxEVT_COMBOBOX, [this, fp](wxCommandEvent& e) {
                int i = e.GetInt();
                if (i >= 0 && i < (int) fp->actions.size()) {
                    fp->btn->SetBitmap_(fp->icon_names[i]);   // button face follows the last pick
                    fp->actions[i]();
                    set_active_tool_btn(fp->btn);
                }
            });
            b->Bind(wxEVT_BUTTON, [b, fp](wxCommandEvent&) {
                // Force a fresh content measure before Popup() (ComboBox does this via the
                // private autoPosition()); otherwise the popup maps at a stale narrow size.
                fp->drop.Invalidate(true);
                fp->drop.SetUseContentWidth(false, false);
                fp->drop.SetUseContentWidth(true, false);
                wxPoint pos = b->ClientToScreen(wxPoint(0, -6));
                fp->drop.Position(pos, wxSize(0, b->GetSize().y + 12));
                fp->drop.Popup();
            });
            m_flyout_keepalive.push_back(fo);
            fadd(id, b);
            auto* chev = new wxStaticText(m_toolbar, wxID_ANY, wxString::FromUTF8("\xE2\x96\xBE"));
            chev->SetForegroundColour(dp_sec_text());
            chev->SetFont(Label::Body_9);
            fadd(id, chev);
        };

        auto* b_sketch = icon_btn("design_sketch", _L("Sketch"));
        // Sketch means a tool. Pressing it used to set the mode and then ask, unconditionally,
        // for the very thing the user had just done — click XZ, read "XZ plane selected, press
        // Sketch", press Sketch, and be told to click a reference plane. The plane was never
        // lost (m_ref_plane holds it and begin_sketch captures it when the first tool is armed);
        // the sentence was simply false, and with right-click excluded in sketch mode there was
        // no door to the tools at all, so the only way on was the toolbar this tab is retiring.
        std::function<void()> act_sketch = [this] {
            set_ui_mode(UiMode::Sketch);
            wxString where;
            const bool have_plane = sketch_plane_target(where);
            m_status->SetForegroundColour(wxNullColour);
            set_status(have_plane
                ? wxString::Format(_L("Sketching on %s — pick a tool"), where)
                : _L("Click a face or a reference plane in the viewport, then a sketch tool"));
            m_status->Refresh();
            if (m_sketch_hint) {   // the card must agree with the status line, not argue with it
                m_sketch_hint->SetLabel(have_plane
                    ? wxString::Format(_L("Drawing on %s.\nPick a tool, or press Menu for the list."), where)
                    : _L("Click a face or a reference plane, then a sketch tool."));
                m_sketch_hint->Refresh();
                m_cards->Layout();
            }
            // Hand over the tools rather than naming them in a status line. CallAfter so the
            // mode change has settled before a modal menu takes the loop; the menu carries each
            // tool's shortcut, so pressing the key instead of picking a row costs nothing.
            if (have_plane)
                CallAfter([this] { show_offer_menu(offer_anchor()); });
        };
        b_sketch->Bind(wxEVT_BUTTON, [act_sketch](wxCommandEvent&) { act_sketch(); });
        m_keys_feature[SHIFT('S')] = act_sketch;
        fadd("sketch", b_sketch);
        // Add material: every feature that grows new solid material — from a profile
        // (extrude/revolve/sweep/loft), from a face (thicken) or from a line (rib).
        feat_dropdown("material", "design_extrude", _L("Add material (extrude / revolve / sweep / loft / thicken / rib)"), {
            {"design_extrude", _L("Extrude"), _L("Extrude a sketch profile, or push/pull a picked face"),
             [this] {
                // Onshape push/pull: an explicitly picked solid face (Face-level cycle, no loop
                // selected) is extruded as the profile — this takes priority over re-extruding an
                // already-consumed sketch (resolve_extrude_sketch always returns the last Sketch).
                if (m_sel_solid_face >= 0 && !m_doc.body.IsNull() && m_sel_sketch_region < 0) {
                    m_extrude_face_src   = m_sel_solid_face;
                    m_extrude_sketch_ref = -1;
                    open_tool(Tool::Extrude);
                    return;
                }
                m_extrude_face_src   = -1;   // ordinary sketch/loop extrude
                m_extrude_sketch_ref = resolve_extrude_sketch();
                if (m_extrude_sketch_ref < 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Create a sketch, or pick a solid face, first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::Extrude);
             }, SHIFT('E')},
            {"design_revolve", _L("Revolve"), _L("Revolve a profile about an axis"),
             [this] {
                m_revolve_sketch_ref = resolve_extrude_sketch();
                if (m_revolve_sketch_ref < 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Create a sketch profile to revolve first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::Revolve);
             }, SHIFT('R')},
            {"design_sweep", _L("Sweep"), _L("Sweep a profile along a path"),
             [this] {
                m_sweep_profile_ref = resolve_extrude_sketch();
                m_sweep_path_ref    = -1;   // fresh sweep: default the picker to the first sketch
                if (m_sweep_profile_ref < 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Create a profile sketch to sweep first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::Sweep);
             }, SHIFT('W')},
            {"design_loft", _L("Loft"), _L("Loft (skin) between two or more profiles"),
             [this] {
                // Loft skins 2+ profile sketches; need at least two to be meaningful.
                int n = 0;
                for (const auto& f : m_doc.features)
                    if (f.type == CadFeatureType::Sketch) ++n;
                if (n < 2) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Create at least two profile sketches to loft"));
                    m_status->Refresh();
                    return;
                }
                m_loft_refs.clear();   // fresh loft: nothing pre-checked
                open_tool(Tool::Loft);
             }, SHIFT('L')},
            {"design_thicken", _L("Thicken"), _L("Offset a solid face into a thin plate (new body)"),
             [this] {
                if (m_doc.bodies.empty()) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Thicken needs a solid body — add or import one first"));
                    m_status->Refresh();
                    return;
                }
                {
                    m_thicken_body->Clear();
                    for (size_t i = 0; i < m_doc.bodies.size(); ++i) {
                        const std::string& n = m_doc.bodies[i].name;
                        m_thicken_body->Append(n.empty() ? wxString::Format(_L("Body %zu"), i + 1) : wxString::FromUTF8(n));
                    }
                    if (m_thicken_body->GetCount() > 0)
                        m_thicken_body->SetSelection(std::min(selected_body_default(),
                                                       int(m_thicken_body->GetCount()) - 1));
                }
                // KEEP a face the user has already picked. Clearing it unconditionally was right
                // when the only door was a toolbar button, which carries no selection: you pressed
                // Thicken and were then asked to point at something. Reached from the offer the
                // verb is invoked ON a face, so discarding it opened the card reading "(pick a
                // solid face)" over an immediate "thicken: face not found" — the user pointed at
                // the face and the card said it could not find one. snaporca-kgx.
                // The index is per-body, so it only survives if the body combo landed on the body
                // it came from; selected_body_default() above returns exactly that when valid.
                if (m_thicken_body->GetSelection() != m_sel_solid_body)
                    m_sel_solid_face = -1;
                open_tool(Tool::Thicken);   // syncs m_thicken_face_label from m_sel_solid_face
             }, 0},
            {"design_rib", _L("Rib"), _L("Grow a thin wall from an open sketch line, fused to a body"),
             [this] {
                if (m_doc.bodies.empty()) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Rib needs a solid body — add or import one first"));
                    m_status->Refresh();
                    return;
                }
                {
                    m_rib_body->Clear();
                    for (size_t i = 0; i < m_doc.bodies.size(); ++i) {
                        const std::string& n = m_doc.bodies[i].name;
                        m_rib_body->Append(n.empty() ? wxString::Format(_L("Body %zu"), i + 1) : wxString::FromUTF8(n));
                    }
                    if (m_rib_body->GetCount() > 0)
                        m_rib_body->SetSelection(std::min(selected_body_default(),
                                                       int(m_rib_body->GetCount()) - 1));
                }
                {
                    m_rib_sketch->Clear();
                    for (int i = 0; i < int(m_doc.features.size()); ++i) {
                        // 3-arg Append: ComboBox's own Append(text, bitmap) hides
                        // wxItemContainer's (text, void*) — see the Sweep picker.
                        // Project features too: they carry Line entities the kernel ribs from
                        // just like a drawn sketch, so a projected body edge is a valid path.
                        if (m_doc.features[i].type == CadFeatureType::Sketch ||
                            m_doc.features[i].type == CadFeatureType::Project)
                            combo_append_index(m_rib_sketch, wxString::FromUTF8(m_doc.features[i].name), i);
                    }
                    if (m_rib_sketch->GetCount() > 0) m_rib_sketch->SetSelection(0);
                    else {
                        m_status->SetForegroundColour(wxColour(235, 110, 110));
                        set_status(_L("Create a sketch with an open line first"));
                        m_status->Refresh();
                        return;
                    }
                }
                open_tool(Tool::Rib);
             }, 'R'},   // plain R, not SHIFT+R (that is Revolve): every SHIFT letter A-Z
                       // was already taken, and the feature map already carries unshifted
                       // keys (P, A, X). Rib was the only verb in this dropdown with no
                       // shortcut at all, which also made its card unreachable to the rig.
        });

        auto* b_pattern = icon_btn("design_pattern", _L("Pattern"));
        std::function<void()> act_pattern = [this] {
            // Pattern replicates an existing body — needs at least one solid.
            if (m_doc.bodies.empty()) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                set_status(_L("Create a solid body to pattern first"));
                m_status->Refresh();
                return;
            }
            open_tool(Tool::Pattern);
        };
        b_pattern->Bind(wxEVT_BUTTON, [act_pattern](wxCommandEvent&) { act_pattern(); });
        m_keys_feature[SHIFT('N')] = act_pattern;
        fadd("pattern", b_pattern);
        m_body_gates.push_back({b_pattern, 1, b_pattern->GetToolTipText(),
                                _L("Pattern — needs a solid body to replicate")});

        // Surface: sheet-body tools (extrude / revolve / loft / fill / offset / thicken)
        // The drawer BUTTON is design_surface, not design_extrude: sharing a face with the
        // Add-material drawer made the two buttons indistinguishable in the bar. The entries
        // inside may reuse the solid glyphs — a menu row carries its own text label.
        feat_dropdown("surface", "design_surface", _L("Surface (extrude / revolve / loft / fill / offset / thicken)"), {
            {"design_extrude", _L("Surface Extrude"), _L("Extrude a sketch into a sheet body (no end caps)"),
             [this] {
                m_surf_extrude_sketch_ref = resolve_extrude_sketch();
                if (m_surf_extrude_sketch_ref < 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Create a sketch first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::SurfaceExtrude);
             }, SHIFT('G')},
            {"design_revolve", _L("Surface Revolve"), _L("Revolve a sketch profile into a sheet body"),
             [this] {
                m_surf_revolve_sketch_ref = resolve_extrude_sketch();
                if (m_surf_revolve_sketch_ref < 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Create a sketch profile to revolve first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::SurfaceRevolve);
             }, SHIFT('J')},
            {"design_loft", _L("Surface Loft"), _L("Loft (skin) between 2+ profiles, open (no end caps)"),
             [this] {
                int n = 0;
                for (const auto& f : m_doc.features)
                    if (f.type == CadFeatureType::Sketch) ++n;
                if (n < 2) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Create at least two profile sketches to loft"));
                    m_status->Refresh();
                    return;
                }
                m_surf_loft_refs.clear();
                open_tool(Tool::SurfaceLoft);
             }, SHIFT('O')},
            {"design_surface", _L("Surface Fill"), _L("Fill a sketch boundary with a smooth face"),
             [this] {
                m_surf_fill_sketch_ref = resolve_extrude_sketch();
                if (m_surf_fill_sketch_ref < 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Create a sketch profile to fill first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::SurfaceFill);
             }, SHIFT('Q')},
            {"design_offset", _L("Surface Offset"), _L("Offset a sheet body's shell by a signed distance"),
             [this] {
                populate_sheet_body_choices(m_surf_offset_body);
                if (m_surf_offset_body->GetCount() == 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("No sheet body to offset — create a surface feature first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::SurfaceOffset);
             }, SHIFT('U')},
            {"design_thicken", _L("Thicken Surface"), _L("Thicken a sheet body into a solid"),
             [this] {
                populate_sheet_body_choices(m_surf_thicken_body);
                if (m_surf_thicken_body->GetCount() == 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("No sheet body to thicken — create a surface feature first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::ThickenSurface);
             }, SHIFT('V')},
        });

        feat_dropdown("plane", "design_plane", _L("Datum / Curve (plane / axis / coord sys / helix / project)"), {
            {"design_plane", _L("Plane"), _L("Reference plane (offset / tilt / midplane / tangent / two edges / coincident)"),
             [this] {
                 populate_plane_choices(m_plane_base);
                 reset_plane_refs();
                 open_tool(Tool::Plane);
             }, SHIFT('P')},
            {"design_line", _L("Axis"), _L("Datum axis (two points, face normal, cylinder centerline, two planes, along edge)"),
             [this] {
                 populate_plane_choices(m_axis_plane_a);
                 populate_plane_choices(m_axis_plane_b);
                 reset_axis_refs();
                 open_tool(Tool::Axis);
             }, SHIFT('A')},
            {"design_point", _L("Coord Sys"), _L("Datum coordinate system (world point, or face + direction edge)"),
             [this] {
                 reset_coordsys_refs();
                 open_tool(Tool::CoordSys);
             }, SHIFT('C')},
            {"design_thread", _L("Helix"), _L("Helical curve (spring path) — use as a sweep path for coils / springs / augers"),
             [this] {
                 populate_plane_choices(m_helix_plane);
                 open_tool(Tool::Helix);
             }, 0},
            // Project belongs here, not in Dress-up: it consumes a body but PRODUCES sketch
            // geometry, so it is reference/curve creation like the four above, not a finishing op.
            {"design_sketch", _L("Project"), _L("Project body edges onto a plane as sketch entities"),
             [this] {
                if (m_doc.bodies.empty()) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Project needs a body — add or import one first"));
                    m_status->Refresh();
                    return;
                }
                {
                    m_proj_source_body->Clear();
                    for (size_t i = 0; i < m_doc.bodies.size(); ++i) {
                        const std::string& n = m_doc.bodies[i].name;
                        m_proj_source_body->Append(n.empty() ? wxString::Format(_L("Body %zu"), i + 1) : wxString::FromUTF8(n));
                    }
                    if (m_proj_source_body->GetCount() > 0)
                        m_proj_source_body->SetSelection(std::min(selected_body_default(),
                                                       int(m_proj_source_body->GetCount()) - 1));
                }
                populate_plane_choices(m_proj_plane);
                m_sel_solid_face = -1;
                m_proj_face_label->SetLabel(_L("(all edges)"));
                open_tool(Tool::Project);
             }, 0},
        });

        // Placement — operations that MOVE a body without changing its shape. Transform and
        // Mirror place one body directly; a Mate places one body relative to another. They were
        // in Dress-up (fillet/draft/shell) and Datum, which mixed shape-finishing and reference
        // geometry with rigid-body placement.
        // Own slot id: "place" is taken by the Place-on-Face button, and put() formats slot
        // item 0 as the control and every later item as its chevron, so sharing a slot would
        // bottom-align this drawer's button like a chevron.
        feat_dropdown("placement", "design_move", _L("Placement (transform / mirror / mate)"), {
            {"design_move", _L("Transform"), _L("Move and/or rotate an existing body"),
             [this] {
                if (m_doc.bodies.empty()) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Transform needs a body — add or import one first"));
                    m_status->Refresh();
                    return;
                }
                {
                    m_xf_body->Clear();
                    for (size_t i = 0; i < m_doc.bodies.size(); ++i) {
                        const std::string& n = m_doc.bodies[i].name;
                        m_xf_body->Append(n.empty() ? wxString::Format(_L("Body %zu"), i + 1) : wxString::FromUTF8(n));
                    }
                    if (m_xf_body->GetCount() > 0)
                        m_xf_body->SetSelection(std::min(selected_body_default(),
                                                       int(m_xf_body->GetCount()) - 1));
                }
                open_tool(Tool::Transform);
             }, SHIFT('Y')},
            {"design_mirror", _L("Mirror"), _L("Reflect a body about a plane"),
             [this] {
                if (m_doc.bodies.empty()) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Mirror needs a body — add or import one first"));
                    m_status->Refresh();
                    return;
                }
                {
                    m_mirror_body->Clear();
                    for (size_t i = 0; i < m_doc.bodies.size(); ++i) {
                        const std::string& n = m_doc.bodies[i].name;
                        m_mirror_body->Append(n.empty() ? wxString::Format(_L("Body %zu"), i + 1) : wxString::FromUTF8(n));
                    }
                    if (m_mirror_body->GetCount() > 0)
                        m_mirror_body->SetSelection(std::min(selected_body_default(),
                                                       int(m_mirror_body->GetCount()) - 1));
                }
                populate_plane_choices(m_mirror_plane);
                open_tool(Tool::Mirror);
             }, SHIFT('Z')},
            {"design_c_coincident", _L("Mate"), _L("Assembly: align two CoordSys features (fastened, planar, revolute, slider, cylindrical)"),
             [this] {
                 open_tool(Tool::Mate);
             }, 0},
        });

        auto* b_boolean = icon_btn("design_boolean", _L("Boolean (combine bodies)"));
        std::function<void()> act_boolean = [this] {
            // A body-body boolean needs at least two solids to combine.
            if (m_doc.bodies.size() < 2) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                set_status(_L("Boolean needs two bodies — create or import a second solid"));
                m_status->Refresh();
                return;
            }
            populate_body_choices();
            open_tool(Tool::Boolean);
        };
        b_boolean->Bind(wxEVT_BUTTON, [act_boolean](wxCommandEvent&) { act_boolean(); });
        m_keys_feature[SHIFT('B')] = act_boolean;
        fadd("boolean", b_boolean);
        m_body_gates.push_back({b_boolean, 2, b_boolean->GetToolTipText(),
                                _L("Boolean — needs two solid bodies to combine")});

        auto* b_cut = icon_btn("design_cut", _L("Cut (split a body with a plane)"));
        std::function<void()> act_cut = [this] {
            // A plane cut needs at least one solid to slice.
            if (m_doc.bodies.empty()) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                set_status(_L("Create a solid body to cut first"));
                m_status->Refresh();
                return;
            }
            populate_plane_choices(m_cut_plane);
            populate_body_choices();
            open_tool(Tool::Cut);
        };
        b_cut->Bind(wxEVT_BUTTON, [act_cut](wxCommandEvent&) { act_cut(); });
        m_keys_feature[SHIFT('X')] = act_cut;
        fadd("cut", b_cut);
        m_body_gates.push_back({b_cut, 1, b_cut->GetToolTipText(),
                                _L("Cut — needs a solid body to slice")});

        // Color — override the selected body's display colour (per-body, survives recompute).
        auto* b_color = icon_btn("color_palette", _L("Color — set the selected body's display colour"));
        b_color->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_set_body_color(); });
        fadd("color", b_color);
        m_verb_actions["btn:colour"] = [this] { on_set_body_color(); };
        m_verb_actions["btn:delete"] = [this] { on_delete_feature(); };
        m_verb_actions["btn:delete_body"] = [this] { on_delete_body(); };
        m_verb_actions["btn:edit"]   = [this] { on_edit_feature(); };
        m_verb_actions["btn:mass"]   = [this] { on_mass_properties(); };
        // Reachable from the offer menu on a SELECTED SKETCH, not only from the toolbar icon.
        // A user evaluating against Onshape reported that "adding constraints seems to be
        // missing" — with nineteen constraint types and a solver shipped. The only paths in
        // were an icon-only button and a sketch-mode-only offer row filed under "Reference",
        // so after finishing a sketch there was no affordance where users actually look.
        m_verb_actions["btn:constrain"] = [this] {
            on_begin_constrain();
            if (m_viewport && (m_viewport->is_constraining() || m_viewport->is_constraining_entities()))
                set_ui_mode(UiMode::Constrain);
        };

        // Dress-up: finishing operations on the faces and edges of an existing solid — nothing
        // that moves a body (see the Placement drawer) and nothing that creates geometry.
        feat_dropdown("dressup", "design_dressup", _L("Fillet / chamfer / draft / shell / delete face"), {
            {"design_dressup", _L("Fillet / Chamfer"), _L("Round or bevel a picked edge"),
             [this] { open_tool(Tool::Dressup); }, SHIFT('F')},
            {"design_draft", _L("Draft (taper a face)"), _L("Tilt a picked face by a draft angle"),
             [this] { open_tool(Tool::Draft); }, SHIFT('D')},
            {"design_shell", _L("Shell"), _L("Hollow the body to a wall thickness, opening a picked face"),
             [this] { open_tool(Tool::Shell); }, SHIFT('K')},
            {"design_delete", _L("Delete Face"), _L("Remove faces from a body and heal the solid"),
             [this] {
                if (m_doc.bodies.empty()) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Delete Face needs a body — add or import one first"));
                    m_status->Refresh();
                    return;
                }
                {
                    m_del_face_body->Clear();
                    for (size_t i = 0; i < m_doc.bodies.size(); ++i) {
                        const std::string& n = m_doc.bodies[i].name;
                        m_del_face_body->Append(n.empty() ? wxString::Format(_L("Body %zu"), i + 1) : wxString::FromUTF8(n));
                    }
                    if (m_del_face_body->GetCount() > 0)
                        m_del_face_body->SetSelection(std::min(selected_body_default(),
                                                       int(m_del_face_body->GetCount()) - 1));
                }
                m_del_faces.clear();
                m_del_face_list->SetLabel(_L("(none)"));
                open_tool(Tool::DeleteFace);
             }, 0},
        });

        // Hole / Thread — drilling into a solid (both face-aware)
        feat_dropdown("hole", "design_hole", _L("Hole / thread"), {
            {"design_hole", _L("Hole"), _L("Drill a hole, centred on a picked face or placed on a plane"),
             [this] {
                // #2: drill on the picked solid face, centred on it (origin = face centroid,
                // normal = inward). Otherwise fall back to the plane dropdown. m_hole_x/y then
                // read as the offset from the face centre (editable for precise placement).
                m_hole_on_face   = false;
                m_hole_face_body = -1;
                m_hole_has_bounds = false;
                set_hole_target_label(-1);
                // Use the explicitly-picked face; otherwise default to the top face of the
                // selected (or first) body so the hole lands on the surface being viewed, not
                // the z=0 datum under the model. The XY/XZ/YZ dropdown still overrides.
                int hb = m_sel_solid_body, hf = m_sel_solid_face;
                if (hf < 0 && !m_doc.bodies.empty()) {
                    hb = (hb >= 0 && hb < int(m_doc.bodies.size())) ? hb : 0;
                    hf = top_face_index_of(m_doc.bodies[hb].shape);
                }
                if (hf >= 0 && hb >= 0 && hb < int(m_doc.bodies.size())) {
                    const TopoDS_Face face = GeometryEngine::face_by_index(
                        m_doc.bodies[hb].shape, hf);
                    if (!face.IsNull()) {
                        m_hole_face_plane = face_plane_inward(face);
                        m_hole_on_face    = true;
                        m_hole_face_body  = hb;
                        set_hole_target_label(hf);   // may be the top-face default, not a pick
                        // Face (u,v) extents so the hole dims read from the sides (#2 Part B).
                        m_hole_has_bounds = GeometryEngine::face_plane_bounds(
                            face, m_hole_face_plane.origin, m_hole_face_plane.x_axis,
                            m_hole_face_plane.y_axis, m_hole_umin, m_hole_umax, m_hole_vmin, m_hole_vmax);
                        if (m_hole_x) m_hole_x->SetValue(0.0);   // start at the face centre
                        if (m_hole_y) m_hole_y->SetValue(0.0);
                        // Reflect the face's orientation in the dropdown so it doesn't keep
                        // showing a stale "XY" while the hole actually drills on this face.
                        if (m_hole_plane)
                            m_hole_plane->SetSelection(index_from_plane(m_hole_face_plane));
                    }
                }
                open_tool(Tool::Hole);
             }, SHIFT('H')},
            {"design_thread", _L("Thread"), _L("Thread a cylindrical surface (inner bore / outer) or a circular edge"),
             [this] {
                // Driven by a picked CYLINDRICAL surface (inner bore = internal, outer = external)
                // OR a circular EDGE (a cylinder's rim) — axis + diameter come from the geometry, so
                // the user never types a radius. The diameter field shows what was derived.
                m_thread_on_face   = false;
                m_thread_face_body = -1;
                set_thread_target_label(-1, -1);
                GeometryEngine::CylinderFace cf;
                bool from_face = false;   // which of the two the cylinder actually came from
                if (m_sel_solid_body >= 0 && m_sel_solid_body < int(m_doc.bodies.size())) {
                    const TopoDS_Shape& shape = m_doc.bodies[m_sel_solid_body].shape;
                    if (m_sel_solid_face >= 0)
                        cf = GeometryEngine::cylinder_of_face(GeometryEngine::face_by_index(shape, m_sel_solid_face));
                    from_face = cf.ok;
                    if (!cf.ok && m_sel_solid_edge >= 0)
                        cf = GeometryEngine::circle_of_edge(GeometryEngine::edge_by_index(shape, m_sel_solid_edge));
                }
                if (cf.ok) {
                    SketchPlane p;                       // plane on the axis (origin at the base)
                    p.origin = cf.base;
                    p.normal = cf.axis;
                    const Vec3d ref = std::abs(cf.axis.z()) < 0.9 ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
                    p.x_axis = ref.cross(cf.axis).normalized();
                    p.y_axis = cf.axis.cross(p.x_axis).normalized();
                    m_thread_face_plane = p;
                    m_thread_on_face    = true;
                    m_thread_face_body  = m_sel_solid_body;
                    set_thread_target_label(from_face ? m_sel_solid_face : -1,
                                            from_face ? -1 : m_sel_solid_edge);
                    infer_thread_spec(2.0 * cf.radius);   // M diameter + pitch + depth from the cylinder
                    if (m_thread_height && cf.height > 1e-6) m_thread_height->SetValue(cf.height);
                    if (m_thread_internal) m_thread_internal->SetValue(cf.internal);
                    if (m_thread_x) m_thread_x->SetValue(0.0);   // on the axis
                    if (m_thread_y) m_thread_y->SetValue(0.0);
                } else if (m_sel_solid_face >= 0 || m_sel_solid_edge >= 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Pick a cylindrical surface (bore / outer) or a circular edge for a thread"));
                    m_status->Refresh();
                }
                open_tool(Tool::Thread);
             }, SHIFT('T')},
        });
        // Text / SVG insert tools live in the SKETCH toolbar (they produce 2D profiles =
        // sketches), not here. STEP stays in Features: it imports a whole B-rep solid.
        // Import STEP — standalone: a STEP comes in as a whole editable B-rep body, not a profile.
        auto* b_step = icon_btn("design_step", _L("Import STEP (editable B-rep solid)"));
        b_step->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_import_step(); });
        m_keys_feature[SHIFT('I')] = [this] { on_import_step(); };
        fadd("step", b_step);
        // Import mesh — same destination as STEP (an editable B-rep body), but the geometry has
        // to be reconstructed from triangles first (GeometryEngine::mesh_to_brep).
        auto* b_mesh = icon_btn("param_triangles", _L("Import mesh (STL/OBJ) as an editable B-rep solid"));
        b_mesh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_import_mesh(); });
        m_keys_feature[SHIFT('M')] = [this] { on_import_mesh(); };
        fadd("mesh", b_mesh);
        auto* b_constrain = icon_btn("design_constrain", _L("Constrain selected sketch"));
        b_constrain->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            on_begin_constrain();
            if (m_viewport && (m_viewport->is_constraining() || m_viewport->is_constraining_entities()))
                set_ui_mode(UiMode::Constrain);
        });
        fadd("constrain", b_constrain);
    }

    // --- Sketch group: plane + entity tools + Construction + Finish
    m_tb_sketch = new wxBoxSizer(wxHORIZONTAL);
    // Sketch carries the most tools of any mode: pack it tight (no inter-icon gap) so the
    // whole entity palette fits without crowding the right-hand actions.
    // Same for the sketch bar: the drawing tools live in the offer. Only Delete selected stays,
    // via sadd_bar. sadd() still runs so sk_key/fly: registrations and the flyout popups survive.
    auto sadd     = [](wxWindow* w) { w->Hide(); };
    auto sadd_bar = [this](wxWindow* w) { m_tb_sketch->Add(w, 0, wxALIGN_CENTER_VERTICAL); };
    m_tb_sketch->Add(caption(_L("SKETCH")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    {
        // The plane/orientation choice lives in the docked Sketch card (Phase 3),
        // not in the toolbar; the toolbar carries only the drawing tools.
        auto skbtn = [&](const char* icon, DesignSketchTool::Mode mode,
                         const wxString& tip, const wxString& hint) {
            auto* b = icon_btn(icon, tip);
            b->Bind(wxEVT_BUTTON, [select_tool, mode, hint](wxCommandEvent&) { select_tool(mode, hint); });
            sadd(b);
        };
        // Onshape-style family flyout, rendered with Orca's themed DropDown
        // (white/teal selector, #DBDBDB border, HarmonyOS Body_14) — same widget
        // as the settings combo dropdowns. The button shows the current variant's
        // icon; clicking drops the variants; a small chevron marks it as a group.
        struct SkVar { const char* icon; DesignSketchTool::Mode mode; wxString tip; wxString hint; };
        struct ToolFlyout {
            std::vector<DropDown::Item> items;   // mainline DropDown is Item-based (text/tip/icon per row)
            std::vector<DesignSketchTool::Mode> modes;
            std::vector<wxString> hints;
            std::vector<std::string> icon_names;
            ScalableButton* btn = nullptr;
            DropDown drop;                 // declared LAST: destroyed before the vector it references
            ToolFlyout() : drop(items) {}
        };
        auto dropdown = [&](const char* def_icon, const wxString& grp, std::vector<SkVar> vars) {
            // Register first, build second — the same law as feat_dropdown, for the same reason.
            // The offer reaches each tool by its ratified address; without these the offer could
            // name a family but only ever arm its FIRST tool: picking "Rectangle" ran key:R and
            // gave you a corner rectangle, with oblique and rounded unreachable. Keyed on the icon
            // id (already unique per family) so no call site grows an argument. snaporca-6vs.
            for (size_t i = 0; i < vars.size(); ++i) {
                const DesignSketchTool::Mode mode = vars[i].mode;
                const wxString               hint = vars[i].hint;
                m_verb_actions["fly:" + std::string(def_icon) + "#" + std::to_string(i)] =
                    [mode, hint, select_tool] { select_tool(mode, hint); };
            }
            if (kBarKeep.count(def_icon) == 0)
                return;   // the drawing tools live in the offer; nothing of this family is built
            auto* b = icon_btn(def_icon, grp);
            // messureSize() measures labels with the PARENT's font (this button) but the
            // popup draws them in Body_14 — so an under-sized button font truncates rows.
            // The button is icon-only (no label), so giving it Body_14 is invisible and
            // makes the content-width measure match the draw.
            b->SetFont(Label::Body_14);
            auto fo = std::make_shared<ToolFlyout>();
            for (auto& v : vars) {
                DropDown::Item it;
                it.text = v.tip;
                it.tip  = v.hint;
                it.icon = tint(create_scaled_bitmap(v.icon, m_form, 18), drop_icon_col);
                fo->items.push_back(it);
                fo->modes.push_back(v.mode);
                fo->hints.push_back(v.hint);
                fo->icon_names.emplace_back(v.icon);
            }
            fo->btn = b;
            fo->drop.Create(b);
            fo->drop.SetUseContentWidth(true, false);
            fo->drop.Invalidate(true);
            ToolFlyout* fp = fo.get();
            fo->drop.Bind(wxEVT_COMBOBOX, [this, fp, select_tool](wxCommandEvent& e) {
                int i = e.GetInt();
                if (i >= 0 && i < (int) fp->modes.size()) {
                    fp->btn->SetBitmap_(fp->icon_names[i]);
                    select_tool(fp->modes[i], fp->hints[i]);
                    set_active_tool_btn(fp->btn);
                }
            });
            b->Bind(wxEVT_BUTTON, [b, fp](wxCommandEvent&) {
                // autoPosition()/messureSize() are private; ComboBox calls them before
                // Popup() so the window is sized to its content first. Without that the
                // popup maps at a stale narrow size and labels ellipsize ("Oblique
                // rectang…"). Force a fresh content measure by toggling use_content_width
                // (messureSize only runs when the flag actually changes), then show.
                fp->drop.Invalidate(true);
                fp->drop.SetUseContentWidth(false, false);
                fp->drop.SetUseContentWidth(true, false);
                wxPoint pos = b->ClientToScreen(wxPoint(0, -6));
                fp->drop.Position(pos, wxSize(0, b->GetSize().y + 12));
                fp->drop.Popup();
            });
            m_flyout_keepalive.push_back(fo);
            sadd(b);
            auto* chev = new wxStaticText(m_toolbar, wxID_ANY, wxString::FromUTF8("\xE2\x96\xBE"));
            chev->SetForegroundColour(dp_sec_text());
            chev->SetFont(Label::Body_9);
            sadd(chev);   // follows its button off the bar
        };
        skbtn("design_select",    DesignSketchTool::Mode::Select,           _L("Select"),
              _L("Click to select; Shift to add; double-click for a whole loop"));
        skbtn("design_dimension", DesignSketchTool::Mode::Dimension, _L("Dimension"),
              _L("Click 2 points or a line / circle / arc to place a dimension"));
        // (separator dropped: the group it divided is now reached from the offer)
        dropdown("design_line", _L("Line / polyline"), {
            {"design_line",     DesignSketchTool::Mode::Line,     _L("Line"),     _L("Click start, then end — then set the exact length")},
            {"design_polyline", DesignSketchTool::Mode::Polyline, _L("Polyline"), _L("Click points; click first / right-click to close the loop")} });
        dropdown("design_rect", _L("Rectangle"), {
            {"design_rect",         DesignSketchTool::Mode::CornerRect,  _L("Corner rectangle"),  _L("Click two opposite corners")},
            {"design_crect",        DesignSketchTool::Mode::CenterRect,  _L("Center rectangle"),  _L("Click center, then a corner")},
            {"design_rect_oblique", DesignSketchTool::Mode::ObliqueRect, _L("Oblique rectangle"), _L("Click two corners of one edge, then a point for the width")},
            {"design_rect_rounded", DesignSketchTool::Mode::RoundedRect, _L("Rounded rectangle"), _L("Click two opposite corners, then a point for the corner radius")} });
        dropdown("design_circle", _L("Circle"), {
            {"design_circle",    DesignSketchTool::Mode::CenterCircle,     _L("Center circle"),  _L("Click center, then radius")},
            {"design_circle2pt", DesignSketchTool::Mode::TwoPointCircle,   _L("2-point circle"), _L("Click two ends of the diameter")},
            {"design_circle3pt", DesignSketchTool::Mode::ThreePointCircle, _L("3-point circle"), _L("Click three points on the circle")} });
        dropdown("design_arc3pt", _L("Arc"), {
            {"design_arc3pt",     DesignSketchTool::Mode::ThreePointArc, _L("3-point arc"),      _L("Click start, end, then a point on the arc")},
            {"design_tangentarc", DesignSketchTool::Mode::TangentArc,    _L("Tangent arc"),      _L("Click start (on the last entity) then end")},
            {"design_arc_center", DesignSketchTool::Mode::CenterArc,     _L("Center-point arc"), _L("Click center, then start, then a point for the end angle")} });
        dropdown("design_slot", _L("Slot"), {
            {"design_slot",     DesignSketchTool::Mode::Slot,    _L("Slot"),     _L("Click two centerline ends, then a point for the end radius")},
            {"design_slot_arc", DesignSketchTool::Mode::ArcSlot, _L("Arc slot"), _L("Click center, start, end, then a point for the width")} });
        dropdown("design_ellipse", _L("Ellipse"), {
            {"design_ellipse",     DesignSketchTool::Mode::Ellipse,    _L("Ellipse"),        _L("Click center, a major-axis end, then a point for the minor axis")},
            {"design_ellipse_arc", DesignSketchTool::Mode::EllipseArc, _L("Elliptical arc"), _L("Click center, major-axis end, minor point, then arc start and end")} });
        skbtn("design_bspline", DesignSketchTool::Mode::BSpline, _L("Spline"),
              _L("Click control points; double-click or right-click to finish"));
        skbtn("design_point",   DesignSketchTool::Mode::Point,   _L("Point"),
              _L("Click to place a point"));
        // (separator dropped: the group it divided is now reached from the offer)
        // Insert tools — Text / SVG produce a 2D profile (a sketch), so they belong with
        // the sketch tools, not in the generic Features strip. Each places the art
        // in-canvas, then commits via the Insert card's Confirm.
        {
            // In Sketch MODE the art must land in the sketch — but begin_sketch() does not run
            // until the first tool is armed, so pressing Sketch and then Text would find no
            // session and commit a separate feature. Arm Select first: it starts the session on
            // the picked plane without drawing anything, so Text behaves the same whether or not
            // you had already drawn a line. (MODE vs SESSION — the distinction that has bitten
            // this panel before.)
            auto ensure_sketch = [this, select_tool] {
                if (m_ui_mode == UiMode::Sketch && m_viewport && !m_viewport->is_sketching())
                    select_tool(DesignSketchTool::Mode::Select,
                                _L("Select — click to select; Shift to add"));
            };
            auto* b_text = icon_btn("design_text", _L("Text — emboss text as a profile"));
            b_text->Bind(wxEVT_BUTTON, [this, ensure_sketch](wxCommandEvent&) {
                ensure_sketch(); on_add_text(); });
            sadd(b_text);
            auto* b_svg = icon_btn("design_svg", _L("SVG — import an outline as a profile"));
            b_svg->Bind(wxEVT_BUTTON, [this, ensure_sketch](wxCommandEvent&) {
                ensure_sketch(); on_import_svg(); });
            sadd(b_svg);
            // …and reachable from the offer's Create row. These were on the atlas's chrome_only
            // list under "document-level actions act on the DOCUMENT, not on a selection" — which
            // is not what they do: both call add_imported_sketch(), which drops the art ON a
            // picked solid face (SketchPlane::from_face, centred on it) exactly as Sketch does.
            // Selection-consuming profile creators, so they belong with the other Create verbs.
            m_verb_actions["btn:text"] = [this, ensure_sketch] { ensure_sketch(); on_add_text(); };
            m_verb_actions["btn:svg"]  = [this, ensure_sketch] { ensure_sketch(); on_import_svg(); };
        }
        // (separator dropped: the group it divided is now reached from the offer)
        // In-canvas edit-op tools (drag gizmo / click label), grouped by family.
        dropdown("design_filletedge", _L("Fillet / chamfer"), {
            {"design_filletedge", DesignSketchTool::Mode::Fillet,  _L("Fillet"),  _L("Pick two lines, then drag the arrow or click the radius to set it")},
            {"design_chamfer",    DesignSketchTool::Mode::Chamfer, _L("Chamfer"), _L("Pick two lines, then drag the arrow or click the distance to set it")} });
        skbtn("design_offset", DesignSketchTool::Mode::Offset, _L("Offset"),
              _L("Pick an entity, then drag the arrow or click the distance; click empty to apply"));
        skbtn("design_mirror", DesignSketchTool::Mode::Mirror, _L("Mirror"),
              _L("Pick a mirror-axis line, then the entities to mirror; click empty to apply"));
        // Trim / Extend scissors — standalone sketch tools (NOT inside Constrain): click a
        // segment to cut it back to / out to its nearest intersection. One cut per click.
        skbtn("design_trim",   DesignSketchTool::Mode::Trim,   _L("Trim"),
              _L("Click a segment to trim it back to its nearest intersection; right-click exits"));
        skbtn("design_extend", DesignSketchTool::Mode::Extend, _L("Extend"),
              _L("Click a line or arc to extend it to the nearest entity; right-click exits"));
        // Constrain — grouped with the edit tools so it's easy to find (nde #13: it was buried
        // far-right next to Construction and went unnoticed). Commits the live sketch in place
        // and drops into Constrain mode (geometric/dimensional palette).
        auto* b_constrain_sk = icon_btn("design_constrain",
            _L("Constrain — add geometric/dimensional relations"));
        b_constrain_sk->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { enter_constrain_inline(); });
        sadd(b_constrain_sk);
        dropdown("design_move", _L("Move / rotate / scale"), {
            {"design_move",   DesignSketchTool::Mode::Move,   _L("Move (translate)"),        _L("Pick entities, then drag the handle or click the distance; click empty to apply")},
            {"design_rotate", DesignSketchTool::Mode::Rotate, _L("Rotate (about centroid)"), _L("Pick entities, then drag around the pivot or click the angle; click empty to apply")},
            {"design_scale",  DesignSketchTool::Mode::Scale,  _L("Scale (about centroid)"),  _L("Pick entities, then drag the handle or click the factor; click empty to apply")} });
        dropdown("design_array", _L("Linear / polar array"), {
            {"design_array",      DesignSketchTool::Mode::Array,      _L("Linear array"),                 _L("Pick entities, drag the spacing handle, click the count; click empty to apply")},
            {"design_polararray", DesignSketchTool::Mode::PolarArray, _L("Polar array (about centroid)"), _L("Pick entities, drag the sweep handle, click the count; click empty to apply")} });

        // Polygon's side count and fit are TOOL PARAMETERS, so they are chosen from the tool:
        // the offer's Create > Polygon submenu. They used to sit inline in this row, then in a
        // sidebar card; both put the choice somewhere you had to leave the geometry to reach,
        // and the count cannot be recovered afterwards (a drawn polygon's inline editor offers
        // Side and Angle, never the count). snaporca-e1p.
        auto arm_polygon = [this, select_tool] {
            push_polygon_params();
            select_tool(DesignSketchTool::Mode::Polygon,
                        wxString::Format(_L("Click center then a vertex — %d sides, %s"),
                                         m_poly_sides,
                                         m_poly_circumscribed ? _L("circumscribed") : _L("inscribed")));
        };
        for (int n : {3, 4, 5, 6, 8, 12})
            m_verb_actions["btn:poly#" + std::to_string(n)] =
                [this, arm_polygon, n] { m_poly_sides = n; arm_polygon(); };
        for (int c : {0, 1})
            m_verb_actions["btn:polyfit#" + std::to_string(c)] =
                [this, arm_polygon, c] { m_poly_circumscribed = (c == 1); arm_polygon(); };

        // Same defect, one level up: a combo whose entries are different VERBS wearing a single
        // name. "Dress-up" is Fillet or Chamfer; "Combine" is union, subtract or intersect;
        // "Pattern" is linear or circular. The choice decides WHAT YOU ARE DOING, so it belongs
        // where you chose the tool — not behind a card you must open to discover it existed.
        // Each address opens the tool exactly as its shortcut does, then says which one.
        // The members are read at INVOCATION, not capture: the cards are built after this row.
        // snaporca-e1p.
        auto open_feature = [this](int key) {
            auto it = m_keys_feature.find(key);
            if (it != m_keys_feature.end() && it->second) it->second();
        };
        // SHIFT() is a constructor-local helper, so resolve the codes here rather than inside
        // the stored lambdas, which outlive it.
        const int k_dress = SHIFT('F'), k_bool = SHIFT('B'), k_pat = SHIFT('N');
        // Choose FIRST, then open: open_tool() titles the card from m_dressup_type, so setting
        // it afterwards left the header reading "Fillet 1" over a chamfer. Nothing in the
        // opener resets the combo, so this order is safe.
        m_verb_actions["btn:dress#0"] = [this, open_feature, k_dress] {
            if (m_dressup_type) m_dressup_type->SetSelection(0); open_feature(k_dress); };
        m_verb_actions["btn:dress#1"] = [this, open_feature, k_dress] {
            if (m_dressup_type) m_dressup_type->SetSelection(1); open_feature(k_dress); };
        for (int op = 0; op < 3; ++op)
            m_verb_actions["btn:bool#" + std::to_string(op)] = [this, open_feature, k_bool, op] {
                open_feature(k_bool);
                if (m_bool_op) { m_bool_op->SetSelection(op); refresh_preview(); } };
        for (int t = 0; t < 2; ++t)
            m_verb_actions["btn:pat#" + std::to_string(t)] = [this, open_feature, k_pat, t] {
                open_feature(k_pat); if (m_pattern_type) m_pattern_type->SetSelection(t); };

        auto* b_poly = icon_btn("design_polygon", _L("Polygon"));
        b_poly->Bind(wxEVT_BUTTON, [arm_polygon](wxCommandEvent&) { arm_polygon(); });
        sadd(b_poly);
        // (separator dropped: the group it divided is now reached from the offer)
        m_construction->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            if (m_viewport && m_viewport->is_sketching())
                m_viewport->set_sketch_construction(m_construction->GetValue()); });
        // STAYS on the bar. Construction is not a tool, it is a persistent MODE — the same kind
        // of thing as the Bed checkbox — and the sketch bar is already shown only in Sketch mode,
        // so it appears exactly while it can apply. Hiding it left Q and the offer's Construction
        // row still toggling a checkbox nobody could see: you could not tell whether the next
        // line would be construction geometry. A stateful toggle has to show its state.
        sadd_bar(m_construction);
        add_sep(m_tb_sketch);
        auto* b_del = icon_btn("design_delete", _L("Delete selected"));
        b_del->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_viewport) m_viewport->delete_selected_sketch_entities(); });
        sadd_bar(b_del);   // keep-list: Delete selected stays on the bar
        // Finish sketch = the unified ✓ Confirm in the action bar (tool_confirm).
    }

    // --- Constrain group: geometric constraints + dimensions + edit ops + Done
    m_tb_constrain = new wxBoxSizer(wxHORIZONTAL);
    auto cadd = [this](wxWindow* w) { m_tb_constrain->Add(w, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2); };
    m_tb_constrain->Add(caption(_L("CONSTRAIN")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    {
        auto cbtn = [&](const char* icon, const wxString& tip, SketchConstraintType type) {
            auto* b = icon_btn(icon, tip);
            b->Bind(wxEVT_BUTTON, [this, type](wxCommandEvent&) { apply_constraint(type); });
            cadd(b);
        };
        cbtn("design_c_horizontal",    _L("Horizontal"),    SketchConstraintType::Horizontal);
        cbtn("design_c_vertical",      _L("Vertical"),      SketchConstraintType::Vertical);
        cbtn("design_c_parallel",      _L("Parallel"),      SketchConstraintType::Parallel);
        cbtn("design_c_perpendicular", _L("Perpendicular"), SketchConstraintType::Perpendicular);
        cbtn("design_c_coincident",    _L("Coincident"),    SketchConstraintType::Coincident);
        cbtn("design_c_equal",         _L("Equal length"),  SketchConstraintType::EqualLength);
        cbtn("design_c_concentric",    _L("Concentric"),    SketchConstraintType::Concentric);
        cbtn("design_c_tangent",       _L("Tangent"),       SketchConstraintType::Tangent);
        cbtn("design_c_midpoint",      _L("Midpoint"),      SketchConstraintType::Midpoint);
        cbtn("design_c_symmetric",     _L("Symmetric"),     SketchConstraintType::Symmetric);
        cbtn("design_c_angle",         _L("Angle"),         SketchConstraintType::Angle);
        cbtn("design_c_radius",        _L("Radius"),        SketchConstraintType::Radius);
        cbtn("design_c_diameter",      _L("Diameter"),      SketchConstraintType::Diameter);
        cbtn("design_c_fix",           _L("Fix point (anchor in place)"), SketchConstraintType::Fix);
        // Trim/Extend are now standalone SKETCH scissors (Mode::Trim/Extend) in the sketch
        // toolbar, NOT Constrain buttons. The other edit ops (Mirror/Offset/Fillet/Chamfer/
        // Move/…) are first-class sketch tools too. Done constraining = the action-bar ✓.
    }

    // Unified action bar: the ONE Confirm/Cancel surface for every tool and mode. Lives at
    // the right end of the ribbon (the "tool dashboard"); shown only while a tool/mode is
    // active (update_action_bar). Replaces the 13 per-card buttons + sketch Finish + Done.
    m_tb_action = new wxBoxSizer(wxHORIZONTAL);
    {
        auto* ok = new wxButton(m_toolbar, wxID_ANY, _L("✓ Confirm"));
        ok->SetForegroundColour(*wxWHITE);
        ok->SetBackgroundColour(wxColour(0x00, 0x96, 0x88));   // Orca teal accent
        ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { tool_confirm(); });
        m_confirm_btns.push_back(ok);   // refresh_preview greys this on an invalid candidate
        auto* no = new wxButton(m_toolbar, wxID_ANY, _L("✗ Cancel"));
        no->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { tool_cancel(); });
        m_tb_action->Add(ok, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_tb_action->Add(no, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    }

    // Persistent Undo/Redo group: always visible (not mode-gated like the tool groups), so
    // history is reachable from Feature, Sketch and Constrain alike. These are momentary
    // actions, so — unlike icon_btn — they are NOT registered in m_tool_btns and never take
    // the teal active-tool highlight. They route to the SAME do_undo_redo as the keyboard
    // Ctrl+Z / Ctrl+Shift+Z path, and are greyed by update_undo_redo_buttons().
    m_tb_history = new wxBoxSizer(wxHORIZONTAL);
    {
        auto hist_btn = [this, tb_bg, tb_hover](const char* icon, const wxString& tip) {
            auto* b = new ScalableButton(m_toolbar, wxID_ANY, icon, "", wxSize(40, 40),
                                         wxDefaultPosition, wxBU_EXACTFIT | wxBORDER_NONE, false, 34);
            b->SetToolTip(tip);
            b->SetBackgroundColour(tb_bg);
            b->Bind(wxEVT_ENTER_WINDOW, [b, tb_hover](wxMouseEvent& e) {
                if (b->IsEnabled()) { b->SetBackgroundColour(tb_hover); b->Refresh(); } e.Skip(); });
            b->Bind(wxEVT_LEAVE_WINDOW, [b, tb_bg](wxMouseEvent& e) {
                b->SetBackgroundColour(tb_bg); b->Refresh(); e.Skip(); });
            return b;
        };
        m_btn_undo = hist_btn("menu_undo", _L("Undo (Ctrl+Z)"));
        m_btn_undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { do_undo_redo(false); });
        m_btn_redo = hist_btn("menu_redo", _L("Redo (Ctrl+Shift+Z)"));
        m_btn_redo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { do_undo_redo(true); });
        m_btn_undo->Enable(false);   // nothing to undo/redo on a fresh document
        m_btn_redo->Enable(false);
        m_tb_history->Add(m_btn_undo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        m_tb_history->Add(m_btn_redo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    }

    // Document actions, left of Undo/Redo and always visible (not mode-gated like the tools).
    // Same styling as the history pair: momentary actions, never the teal active-tool state.
    m_tb_doc = new wxBoxSizer(wxHORIZONTAL);
    const wxColour tb_glyph_col  = StateColor::darkModeColorFor(wxColour(0x36, 0x36, 0x36));
    const wxColour tb_commit_col(0x00, 0x96, 0x88);   // Orca Confirm accent
    {
        // Some Orca glyphs (toolbar_add_plate, toolbar_flatten) are drawn for a light toolbar and
        // come out the same tone as this dark one — Commit was effectively invisible. Re-tint
        // those: Commit in the teal accent it carries as the tab's primary action, the rest in
        // the same grey the other toolbar glyphs resolve to.
        auto doc_btn = [this, tb_bg, tb_hover, &tint](const char* icon, const wxString& tip,
                                                      const wxColour* glyph = nullptr) {
            auto* b = new ScalableButton(m_toolbar, wxID_ANY, icon, "", wxSize(40, 40),
                                         wxDefaultPosition, wxBU_EXACTFIT | wxBORDER_NONE, false, 34);
            if (glyph != nullptr)
                b->SetBitmap(tint(create_scaled_bitmap(icon, m_toolbar, 42), *glyph));
            b->SetToolTip(tip);
            b->SetBackgroundColour(tb_bg);
            b->Bind(wxEVT_ENTER_WINDOW, [b, tb_hover](wxMouseEvent& e) {
                if (b->IsEnabled()) { b->SetBackgroundColour(tb_hover); b->Refresh(); } e.Skip(); });
            b->Bind(wxEVT_LEAVE_WINDOW, [b, tb_bg](wxMouseEvent& e) {
                b->SetBackgroundColour(tb_bg); b->Refresh(); e.Skip(); });
            return b;
        };
        auto add_doc = [this](ScalableButton* b) {
            m_tb_doc->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4); };

        auto* b_new = doc_btn("add", _L("New Design — clear the feature tree"));
        b_new->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_new_design(); });
        add_doc(b_new);
        add_doc(static_cast<ScalableButton*>(tb_slot["step"][0]));   // 2. Import STEP
        add_doc(static_cast<ScalableButton*>(tb_slot["mesh"][0]));   // 3. Import mesh
        auto* b_export = doc_btn("save", _L("Export STEP…"));
        b_export->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_export_step(); });
        add_doc(b_export);

        // View option, not a document action: hide the printer bed to model without it. Lives in
        // this row because it must stay reachable with no tool open — a card would come and go.
        m_show_bed = new CheckBox(m_toolbar);
        m_show_bed->SetValue(true);                 // bed visible by default, as the tab opens today
        m_show_bed->SetToolTip(_L("Show the printer bed and its plate grid"));
        // wxEVT_TOGGLEBUTTON, NOT wxEVT_CHECKBOX: Orca's CheckBox derives from
        // wxBitmapToggleButton (Widgets/CheckBox.hpp), so a wxEVT_CHECKBOX handler never fires.
        // Read the control rather than the event so the state cannot disagree with the glyph.
        m_show_bed->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            if (m_viewport) m_viewport->set_show_bed(m_show_bed->GetValue());
            e.Skip();
        });
        auto* bed_lbl = new wxStaticText(m_toolbar, wxID_ANY, _L("Bed"));
        bed_lbl->SetForegroundColour(dp_sec_text());
        m_tb_doc->Add(m_show_bed, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
        m_tb_doc->Add(bed_lbl,    0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);

        // These act on bodies / the view, so they ride in the feature group, in the slots
        // the user assigned them (9, 11bis, 16).
        auto* b_place = doc_btn("toolbar_flatten", _L("Place on Face (F) — lay the picked face on the bed"),
                                &tb_glyph_col);
        b_place->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { place_on_face(); });
        tb_slot["place"].push_back(b_place);

        auto* b_section = doc_btn("split_parts", _L("Section View — hide part of the model to see inside. "
                                                   "PageUp/PageDown move the plane; Delete removes it."));
        b_section->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { toggle_section_view(); });
        tb_slot["section"].push_back(b_section);

        m_section_flip_btn = doc_btn("design_mirror", _L("Flip Section — show the opposite half"));
        m_section_flip_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { flip_section_view(); });
        m_section_flip_btn->Enable(false);   // only usable while a section view is active
        tb_slot["flip"].push_back(m_section_flip_btn);

        // Commit is the tab's primary action and sits far right, next to Confirm/Cancel.
        m_tb_commit = new wxBoxSizer(wxHORIZONTAL);
        auto* b_commit = doc_btn("toolbar_add_plate", _L("Commit to Plate — send the solid to Prepare"),
                                 &tb_commit_col);
        b_commit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_commit(); });
        m_tb_commit->Add(b_commit, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    }

    // Feature-group layout, in the requested left-to-right order.
    {
        auto put = [&](const char* id) {
            auto it = tb_slot.find(id);
            if (it == tb_slot.end()) return;
            for (size_t i = 0; i < it->second.size(); ++i)
                m_tb_feature->Add(it->second[i], 0,
                                  i == 0 ? (wxALIGN_CENTER_VERTICAL | wxRIGHT)
                                         : (wxALIGN_BOTTOM | wxBOTTOM | wxRIGHT),
                                  i == 0 ? 4 : 5);
        };
        put("sketch");    put("constrain");            // 7, 7bis
        add_sep(m_tb_feature);
        put("material");  put("place");   put("placement"); put("plane"); // 8, 9, 9bis, 10
        put("dressup");   put("section");                // 11, 11bis
        put("hole");      put("boolean"); put("cut");   // 12, 13, 14
        put("surface");   put("color");   put("flip");  // 14bis, 15, 16
        put("pattern");                                 // kept, at the end
    }

    auto* tbrow = new wxBoxSizer(wxHORIZONTAL);
    tbrow->AddSpacer(8);
    tbrow->Add(m_tb_doc,       0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    add_sep(tbrow);
    tbrow->Add(m_tb_history,   0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    add_sep(tbrow);
    tbrow->Add(m_tb_feature,   0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    tbrow->Add(m_tb_sketch,    0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    tbrow->Add(m_tb_constrain, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    tbrow->AddStretchSpacer();
    tbrow->Add(m_tb_commit,    0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    tbrow->Add(m_tb_action,    0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    tbrow->AddSpacer(8);
    m_toolbar->SetSizer(tbrow);
    // Apply the body gates once now: an empty document is exactly the state the bug was reported
    // in, and feed_bodies() has not run yet on a fresh tab.
    update_body_gates();

    // Onshape-style dialog-card header: feature icon + bold title. out receives
    // the title control so open_tool() can retitle it per feature.
    auto card_header = [](wxWindow* card, const char* icon, const wxString& title, wxStaticText*& out) -> wxSizer* {
        auto* h  = new wxBoxSizer(wxHORIZONTAL);
        auto* ic = new wxStaticBitmap(card, wxID_ANY, create_scaled_bitmap(icon, card, 18));
        out = new wxStaticText(card, wxID_ANY, title);
        out->SetFont(Label::Head_14);   // Orca shared HarmonyOS card-title font
        h->Add(ic,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        h->Add(out, 0, wxALIGN_CENTER_VERTICAL);
        return h;
    };

    // Every tool dialog lives inside ONE framed card (only one is ever visible), so the
    // active dialog reads as a bordered panel like Prepare's sidebar boxes instead of
    // free-floating rows. `cards` is the frame's sizer; open_tool() shows/hides within it.
    m_cards = make_card(m_form);
    auto* cards = new wxBoxSizer(wxVERTICAL);
    m_cards->SetSizer(cards);
    root->Add(m_cards, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    // --- Sketch dialog (shape definition only — no distance/mode) ---
    auto* form = two_col_form();

    m_shape = make_combo(m_cards);
    m_shape->Append(_L("Rectangle"));
    m_shape->Append(_L("Circle"));
    m_shape->SetSelection(0);
    form->Add(new wxStaticText(m_cards, wxID_ANY, _L("Shape")), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(m_shape, 0, wxEXPAND);

    // NO plane row. A sketch takes its plane from what is picked in the VIEWPORT — a planar face
    // on a solid, or one of the reference-plane ghosts clicked in 3D — resolved by
    // sketch_plane_from_selection(). A three-row XY/XZ/YZ combo could not express either of those
    // targets, so it displayed a value that was at best redundant and at worst false. snaporca-e1p.

    m_width = make_spin(m_cards, 20);
    form->Add(new wxStaticText(m_cards, wxID_ANY, _L("Width / X")), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(spin_frame(m_width), 0, wxEXPAND);

    m_height = make_spin(m_cards, 20);
    form->Add(new wxStaticText(m_cards, wxID_ANY, _L("Height / Y")), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(spin_frame(m_height), 0, wxEXPAND);

    m_radius = make_spin(m_cards, 10);
    form->Add(new wxStaticText(m_cards, wxID_ANY, _L("Radius")), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(spin_frame(m_radius), 0, wxEXPAND);

    m_box_sketch = new wxBoxSizer(wxVERTICAL);
    m_box_sketch->Add(card_header(m_cards, "design_sketch", _L("Sketch"), m_hdr_sketch), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_sketch->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_box_sketch->Add(form, 0, wxEXPAND | wxALL, 12);
    cards->Add(m_box_sketch, 0, wxEXPAND);

    // --- Move / Rotate body ---
    {
        auto* mform = two_col_form();
        m_move_dx = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        mform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Distance X")), 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(spin_frame(m_move_dx), 0, wxEXPAND);
        m_move_dy = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        mform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Distance Y")), 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(spin_frame(m_move_dy), 0, wxEXPAND);
        m_move_dz = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        mform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Distance Z")), 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(spin_frame(m_move_dz), 0, wxEXPAND);

        m_move_axis = make_combo(m_cards);
        m_move_axis->Append(_L("X"));
        m_move_axis->Append(_L("Y"));
        m_move_axis->Append(_L("Z"));
        m_move_axis->SetSelection(2);
        mform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Rotation axis")), 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(m_move_axis, 0, wxEXPAND);

        m_move_angle = make_spin(m_cards, 0.0, -360.0, 360.0);
        mform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Angle °")), 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(spin_frame(m_move_angle), 0, wxEXPAND);

        for (wxSpinCtrlDouble* sp : { m_move_dx, m_move_dy, m_move_dz, m_move_angle })
            sp->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent& e) { apply_move_card(); e.Skip(); });
        m_move_axis->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) { apply_move_card(); e.Skip(); });

        m_box_move = new wxBoxSizer(wxVERTICAL);
        m_box_move->Add(card_header(m_cards, "design_move", _L("Move / Rotate"), m_hdr_move), 0,
                        wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_move->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
        m_box_move->Add(mform, 0, wxEXPAND | wxALL, 12);
        cards->Add(m_box_move, 0, wxEXPAND);
    }

    // --- Polygon (sketch tool options) ---
    {
        // NO polygon card. Sides and Circumscribed are tool parameters and are chosen from the
        // tool — the offer's Create > Polygon submenu names the common side counts and the two
        // fits, and arming from there sets both. A spin field on the left could not be reached
        // without leaving the geometry, and the count is unrecoverable afterwards: the inline
        // editor a drawn polygon opens offers Side and Angle, never the count. snaporca-e1p.
    }

    // --- Extrude dialog (consumes the selected sketch) ---
    m_box_extrude = new wxBoxSizer(wxVERTICAL);
    m_box_extrude->Add(card_header(m_cards, "design_extrude", _L("Extrude"), m_hdr_extrude), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_extrude->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_extrude_sketch_label = new wxStaticText(m_cards, wxID_ANY, _L("Sketch: —"));
    m_box_extrude->Add(m_extrude_sketch_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    {
        auto* eform = two_col_form();

        m_distance = make_spin(m_cards, 10);
        eform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Extrude dist")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(spin_frame(m_distance), 0, wxEXPAND);

        // End condition (order MUST match ExtrudeEnd: Blind/Symmetric/TwoSided/ThroughAll/
        // UpToFace/UpToVertex). Up-to-face uses the currently click-selected solid face.
        m_extrude_end = make_combo(m_cards);
        for (const char* s : { "Blind", "Symmetric", "Two-sided", "Through all",
                               "Up to face", "Up to vertex" })
            m_extrude_end->Append(s);
        m_extrude_end->SetSelection(0);
        eform->Add(new wxStaticText(m_cards, wxID_ANY, _L("End")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(m_extrude_end, 0, wxEXPAND);

        m_distance2 = make_spin(m_cards, 5, 0.0, 100000.0);   // second-side depth (Two-sided)
        eform->Add(new wxStaticText(m_cards, wxID_ANY, _L("2nd dist")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(spin_frame(m_distance2), 0, wxEXPAND);

        m_taper = make_spin(m_cards, 0.0, -89.0, 89.0);       // draft angle (deg)
        eform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Taper °")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(spin_frame(m_taper), 0, wxEXPAND);

        m_mode = make_combo(m_cards);
        // Order is load-bearing: index maps to BooleanMode (New=0, Add=1, Cut=2, Intersect=3).
        // Labels use Onshape wording so the choice reads as the user thinks of it.
        m_mode->Append(_L("New body"));   // separate coexisting solid
        m_mode->Append(_L("Join"));       // fuse into the target body (was "Add")
        m_mode->Append(_L("Cut"));        // subtract from the target body
        m_mode->Append(_L("Intersect"));  // keep only the overlap
        m_mode->SetSelection(0);
        eform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Result")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(m_mode, 0, wxEXPAND);

        m_flip = new CheckBox(m_cards);
        eform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Flip direction")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(m_flip, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);

        m_box_extrude->Add(eform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_extrude, 0, wxEXPAND);

    // --- Dress-up (Fillet / Chamfer) ---
    auto* dform = two_col_form();

    m_dressup_type = make_combo(m_cards);
    m_dressup_type->Append(_L("Fillet"));
    m_dressup_type->Append(_L("Chamfer"));
    m_dressup_type->SetSelection(0);
    dform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Type")), 0, wxALIGN_CENTER_VERTICAL);
    dform->Add(m_dressup_type, 0, wxEXPAND);

    // The card can dress ONE picked edge or a whole face-group, and which one it will do is
    // decided by a viewport pick the card previously said nothing about — so a user who had not
    // picked an edge saw only the group combo and concluded per-edge rounding did not exist,
    // while a user who HAD picked one saw the combo still reading "All" and was told the opposite
    // of what Confirm would do. This row states the actual target, as Shell and Draft already do.
    m_dressup_edge_label = new wxStaticText(m_cards, wxID_ANY, _L("(no edge picked — group below)"));
    dform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Target")), 0, wxALIGN_CENTER_VERTICAL);
    dform->Add(m_dressup_edge_label, 0, wxALIGN_CENTER_VERTICAL);

    m_face_group = make_combo(m_cards);
    m_face_group->Append(_L("Top"));      // index 0 -> FaceGroup::Top
    m_face_group->Append(_L("Bottom"));   // 1 -> Bottom
    m_face_group->Append(_L("Lateral"));  // 2 -> Lateral
    m_face_group->Append(_L("All"));      // 3 -> All
    m_face_group->SetSelection(3);
    dform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Edges")), 0, wxALIGN_CENTER_VERTICAL);
    dform->Add(m_face_group, 0, wxEXPAND);

    m_dressup_size = make_spin(m_cards, 2.0);
    dform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Size (r/dist)")), 0, wxALIGN_CENTER_VERTICAL);
    dform->Add(spin_frame(m_dressup_size), 0, wxEXPAND);

    m_box_dressup = new wxBoxSizer(wxVERTICAL);
    m_box_dressup->Add(card_header(m_cards, "design_dressup", _L("Fillet / Chamfer"), m_hdr_dressup), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_dressup->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_box_dressup->Add(dform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    cards->Add(m_box_dressup, 0, wxEXPAND);

    // --- Hole (positioned circular cut) ---
    auto* hform = two_col_form();

    m_hole_plane = make_combo(m_cards);
    m_hole_plane->Append(_L("XY"));
    m_hole_plane->Append(_L("XZ"));
    m_hole_plane->Append(_L("YZ"));
    m_hole_plane->SetSelection(0);
    // Picking a plane here is an explicit choice: drop any on-face hijack (a stale face pick
    // could keep m_hole_on_face true, so the dropdown was ignored and the hole drilled on the
    // face's plane instead of the chosen XY/XZ/YZ).
    m_hole_plane->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) {
        m_hole_on_face    = false;
        m_hole_has_bounds = false;
        set_hole_target_label(-1);
        update_hole_gizmo();
        refresh_preview();
        e.Skip();
    });
    hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Hole plane")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(m_hole_plane, 0, wxEXPAND);

    m_hole_diameter = make_spin(m_cards, 6.0);
    hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Diameter")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(spin_frame(m_hole_diameter), 0, wxEXPAND);

    m_hole_depth = make_spin(m_cards, 10.0);
    hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Depth (blind)")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(spin_frame(m_hole_depth), 0, wxEXPAND);

    m_hole_x = make_spin(m_cards, 0.0, -1000.0, 1000.0);
    hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Pos X")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(spin_frame(m_hole_x), 0, wxEXPAND);

    m_hole_y = make_spin(m_cards, 0.0, -1000.0, 1000.0);
    hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Pos Y")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(spin_frame(m_hole_y), 0, wxEXPAND);

    m_hole_through = new CheckBox(m_cards);
    m_hole_through->SetValue(true);
    hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Through")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(m_hole_through, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);

    m_hole_target_label = new wxStaticText(m_cards, wxID_ANY, _L("(none — uses Hole plane)"));
    hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("On face")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(m_hole_target_label, 0, wxALIGN_CENTER_VERTICAL);

    m_box_hole = new wxBoxSizer(wxVERTICAL);
    m_box_hole->Add(card_header(m_cards, "design_hole", _L("Hole"), m_hdr_hole), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_hole->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_box_hole->Add(hform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    cards->Add(m_box_hole, 0, wxEXPAND);

    // --- Thread (helical) ---
    auto* tform = two_col_form();

    m_thread_plane = make_combo(m_cards);
    m_thread_plane->Append(_L("XY"));
    m_thread_plane->Append(_L("XZ"));
    m_thread_plane->Append(_L("YZ"));
    m_thread_plane->SetSelection(0);
    tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Thread plane")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_plane, 0, wxEXPAND);

    // Standard designation picker — fills pitch/depth (and nominal radius) from the
    // ISO metric / Unified imperial tables. "Custom" leaves the manual spins alone.
    m_thread_std = make_combo(m_cards);
    m_thread_std->Append(_L("Custom"));
    for (const ThreadSpec& s : thread_standards())
        m_thread_std->Append(s.name);
    m_thread_std->SetSelection(0);
    m_thread_std->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { apply_thread_standard(); });
    tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Standard")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_std, 0, wxEXPAND);

    // Threads are specified by DIAMETER (M6 = Ø6); the value is derived from the picked cylindrical
    // surface / circular edge, so it's a readout users rarely type. Stored field holds the diameter.
    m_thread_radius = make_spin(m_cards, 10.0);
    tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Diameter")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(spin_frame(m_thread_radius), 0, wxEXPAND);

    m_thread_pitch = make_spin(m_cards, 2.0);
    tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Pitch")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(spin_frame(m_thread_pitch), 0, wxEXPAND);

    m_thread_height = make_spin(m_cards, 10.0);
    tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Length")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(spin_frame(m_thread_height), 0, wxEXPAND);

    m_thread_depth = make_spin(m_cards, 1.0);
    tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Thread depth")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(spin_frame(m_thread_depth), 0, wxEXPAND);

    m_thread_x = make_spin(m_cards, 0.0, -1000.0, 1000.0);
    tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Pos X")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(spin_frame(m_thread_x), 0, wxEXPAND);

    m_thread_y = make_spin(m_cards, 0.0, -1000.0, 1000.0);
    tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Pos Y")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(spin_frame(m_thread_y), 0, wxEXPAND);

    m_thread_internal = new CheckBox(m_cards);
    m_thread_internal->SetValue(false);
    // External rod uses the major radius; an internal tapped bore uses the minor
    // (tap-drill) radius — re-derive the nominal radius when the role flips.
    m_thread_internal->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        apply_thread_standard();
        e.Skip();
    });
    tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Internal")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_internal, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);

    m_thread_target_label = new wxStaticText(m_cards, wxID_ANY, _L("(none — uses Thread plane)"));
    tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("On face")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_target_label, 0, wxALIGN_CENTER_VERTICAL);

    m_box_thread = new wxBoxSizer(wxVERTICAL);
    m_box_thread->Add(card_header(m_cards, "design_thread", _L("Thread"), m_hdr_thread), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_thread->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_box_thread->Add(tform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    cards->Add(m_box_thread, 0, wxEXPAND);

    // --- Revolve (sweep a sketch profile about an in-plane axis) ---
    m_box_revolve = new wxBoxSizer(wxVERTICAL);
    m_box_revolve->Add(card_header(m_cards, "design_revolve", _L("Revolve"), m_hdr_revolve), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_revolve->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_revolve_sketch_label = new wxStaticText(m_cards, wxID_ANY, _L("Sketch: —"));
    m_box_revolve->Add(m_revolve_sketch_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    {
        auto* rform = two_col_form();

        m_revolve_angle = make_spin(m_cards, 360.0, 1.0, 360.0);
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Angle °")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(spin_frame(m_revolve_angle), 0, wxEXPAND);

        m_revolve_axis = make_combo(m_cards);
        m_revolve_axis->Append(_L("Plane X"));
        m_revolve_axis->Append(_L("Plane Y"));
        m_revolve_axis->SetSelection(0);
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Axis")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_revolve_axis, 0, wxEXPAND);

        m_revolve_mode = make_combo(m_cards);
        m_revolve_mode->Append(_L("New"));
        m_revolve_mode->Append(_L("Add"));
        m_revolve_mode->Append(_L("Cut"));
        m_revolve_mode->Append(_L("Intersect"));
        m_revolve_mode->SetSelection(0);
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Mode")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_revolve_mode, 0, wxEXPAND);

        m_revolve_flip = new CheckBox(m_cards);
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Flip direction")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_revolve_flip, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);

        m_box_revolve->Add(rform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_revolve, 0, wxEXPAND);

    // --- Sweep (sweep a profile sketch along a path sketch) ---
    m_box_sweep = new wxBoxSizer(wxVERTICAL);
    m_box_sweep->Add(card_header(m_cards, "design_sweep", _L("Sweep"), m_hdr_sweep), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_sweep->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_sweep_profile_label = new wxStaticText(m_cards, wxID_ANY, _L("Profile: —"));
    m_box_sweep->Add(m_sweep_profile_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    {
        auto* sform = two_col_form();

        m_sweep_path = make_combo(m_cards);
        sform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Path")), 0, wxALIGN_CENTER_VERTICAL);
        sform->Add(m_sweep_path, 0, wxEXPAND);

        m_sweep_mode = make_combo(m_cards);
        m_sweep_mode->Append(_L("New"));
        m_sweep_mode->Append(_L("Add"));
        m_sweep_mode->Append(_L("Cut"));
        m_sweep_mode->Append(_L("Intersect"));
        m_sweep_mode->SetSelection(0);
        sform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Mode")), 0, wxALIGN_CENTER_VERTICAL);
        sform->Add(m_sweep_mode, 0, wxEXPAND);

        m_box_sweep->Add(sform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_sweep, 0, wxEXPAND);

    // --- Pattern (replicate the target body: linear or circular) ---
    m_box_pattern = new wxBoxSizer(wxVERTICAL);
    m_box_pattern->Add(card_header(m_cards, "design_pattern", _L("Pattern"), m_hdr_pattern), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_pattern->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* pform = two_col_form();

        m_pattern_type = make_combo(m_cards);
        m_pattern_type->Append(_L("Linear"));
        m_pattern_type->Append(_L("Circular"));
        m_pattern_type->SetSelection(0);
        pform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Type")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(m_pattern_type, 0, wxEXPAND);

        m_pattern_count = make_spin(m_cards, 3, 1, 999);
        pform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Count")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(spin_frame(m_pattern_count), 0, wxEXPAND);

        m_pattern_spacing = make_spin(m_cards, 20.0, 0.01, 100000.0);
        pform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Spacing")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(spin_frame(m_pattern_spacing), 0, wxEXPAND);

        m_pattern_dir = make_combo(m_cards);
        m_pattern_dir->Append(_L("Plane X"));
        m_pattern_dir->Append(_L("Plane Y"));
        m_pattern_dir->SetSelection(0);
        pform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Direction")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(m_pattern_dir, 0, wxEXPAND);

        m_pattern_angle = make_spin(m_cards, 360.0, 1.0, 360.0);
        pform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Total angle°")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(spin_frame(m_pattern_angle), 0, wxEXPAND);

        m_box_pattern->Add(pform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_pattern, 0, wxEXPAND);

    // --- Boolean (combine two existing bodies: union / subtract / intersect) ---
    m_box_boolean = new wxBoxSizer(wxVERTICAL);
    m_box_boolean->Add(card_header(m_cards, "design_boolean", _L("Boolean"), m_hdr_boolean), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_boolean->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* bform = two_col_form();

        m_bool_op = make_combo(m_cards);
        m_bool_op->Append(_L("Union (join)"));
        m_bool_op->Append(_L("Subtract (cut)"));
        m_bool_op->Append(_L("Intersect"));
        m_bool_op->SetSelection(0);
        m_bool_op->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        bform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Operation")), 0, wxALIGN_CENTER_VERTICAL);
        bform->Add(m_bool_op, 0, wxEXPAND);

        m_bool_target = make_combo(m_cards);
        m_bool_target->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        bform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Target (kept)")), 0, wxALIGN_CENTER_VERTICAL);
        bform->Add(m_bool_target, 0, wxEXPAND);

        m_bool_tool = make_combo(m_cards);
        m_bool_tool->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        bform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Tool")), 0, wxALIGN_CENTER_VERTICAL);
        bform->Add(m_bool_tool, 0, wxEXPAND);

        // Fuzzy tolerance (mm): the main use is a tool body cutting a destination — a small
        // tolerance lets near-coincident mating faces resolve into a clean cut instead of a
        // failed boolean or sliver faces. 0 = exact.
        m_bool_tol = make_spin(m_cards, 0.0, 0.0, 100.0);
        m_bool_tol->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        bform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Tolerance")), 0, wxALIGN_CENTER_VERTICAL);
        bform->Add(spin_frame(m_bool_tol), 0, wxEXPAND);

        m_box_boolean->Add(bform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        m_bool_keep = new CheckBox(m_cards);
        m_bool_keep->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { refresh_preview(); });
        auto* keeprow = new wxBoxSizer(wxHORIZONTAL);
        keeprow->Add(new wxStaticText(m_cards, wxID_ANY, _L("Keep tool body")), 0, wxALIGN_CENTER_VERTICAL);
        keeprow->AddStretchSpacer();
        keeprow->Add(m_bool_keep, 0, wxALIGN_CENTER_VERTICAL);
        m_box_boolean->Add(keeprow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_boolean, 0, wxEXPAND);

    // --- Cut (split a body with a plane): parameters only; ✓/✗ live on the ribbon ---
    m_box_cut = new wxBoxSizer(wxVERTICAL);
    m_box_cut->Add(card_header(m_cards, "design_cut", _L("Cut"), m_hdr_cut), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_cut->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* cform = two_col_form();

        m_cut_target = make_combo(m_cards);
        m_cut_target->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        cform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Body")), 0, wxALIGN_CENTER_VERTICAL);
        cform->Add(m_cut_target, 0, wxEXPAND);

        m_cut_plane = make_combo(m_cards);
        m_cut_plane->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        cform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Plane")), 0, wxALIGN_CENTER_VERTICAL);
        cform->Add(m_cut_plane, 0, wxEXPAND);

        m_cut_offset = make_spin(m_cards, 0.0, -10000.0, 10000.0);
        m_cut_offset->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        cform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Offset")), 0, wxALIGN_CENTER_VERTICAL);
        cform->Add(spin_frame(m_cut_offset), 0, wxEXPAND);

        m_box_cut->Add(cform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        // The cut always leaves BOTH pieces as separate bodies (a non-destructive split);
        // delete one from the tree afterwards if you only want a half.
    }
    cards->Add(m_box_cut, 0, wxEXPAND);

    // --- Insert (Text / SVG placement): Confirm/Cancel for the in-canvas art transform ---
    m_box_insert = new wxBoxSizer(wxVERTICAL);
    m_box_insert->Add(card_header(m_cards, "design_text", _L("Insert"), m_hdr_insert), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_insert->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_box_insert->Add(new wxStaticText(m_cards, wxID_ANY,
        _L("Drag a corner to size, the centre to move.\nConfirm or Cancel in the toolbar above.")),
        0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    cards->Add(m_box_insert, 0, wxEXPAND);

    // --- Plane (datum/reference plane: offset + tilt from a base plane; no solid) ---
    m_box_plane = new wxBoxSizer(wxVERTICAL);
    m_box_plane->Add(card_header(m_cards, "design_plane", _L("Plane"), m_hdr_plane), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_plane->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        // Plane type chooses which inputs matter (Onshape/Fusion parity):
        //   Offset      = Base (or Face A) + Offset (+ Tilt about a base axis)
        //   Angle       = Edge A (line) + Base/Face A reference + Angle°
        //   Midplane    = Face A + Face B (halfway between)
        //   Tangent     = Face A (a cylinder) + Angle° around its axis
        //   Two edges   = Edge A + Edge B
        //   Coincident  = Face A (lie on that face)
        m_plane_type = make_combo(m_cards);
        for (const wxString& t : { _L("Offset"), _L("Angle"), _L("Midplane"),
                                   _L("Tangent"), _L("Two edges"), _L("Coincident") })
            m_plane_type->Append(t);
        m_plane_type->SetSelection(0);
        m_box_plane->Add(new wxStaticText(m_cards, wxID_ANY, _L("Plane type")), 0, wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_plane->Add(m_plane_type, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        auto* plform = two_col_form();

        m_plane_base = make_combo(m_cards);
        populate_plane_choices(m_plane_base);   // XY/XZ/YZ + any existing datum planes
        plform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Base")), 0, wxALIGN_CENTER_VERTICAL);
        plform->Add(m_plane_base, 0, wxEXPAND);

        m_plane_offset = make_spin(m_cards, 20.0, -100000.0, 100000.0);
        plform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Offset")), 0, wxALIGN_CENTER_VERTICAL);
        plform->Add(spin_frame(m_plane_offset), 0, wxEXPAND);

        m_plane_tilt = make_spin(m_cards, 0.0, -180.0, 180.0);
        plform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Angle°")), 0, wxALIGN_CENTER_VERTICAL);
        plform->Add(spin_frame(m_plane_tilt), 0, wxEXPAND);

        m_plane_tilt_axis = make_combo(m_cards);
        m_plane_tilt_axis->Append(_L("Base X"));
        m_plane_tilt_axis->Append(_L("Base Y"));
        m_plane_tilt_axis->SetSelection(0);
        plform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Tilt axis")), 0, wxALIGN_CENTER_VERTICAL);
        plform->Add(m_plane_tilt_axis, 0, wxEXPAND);

        // Contextual reference picks: arm a target, then click a solid face/edge in the canvas.
        auto pick_row = [&](const wxString& label, wxButton*& btn, wxStaticText*& lbl, PlanePick target) {
            btn = new wxButton(m_cards, wxID_ANY, label);
            lbl = new wxStaticText(m_cards, wxID_ANY, _L("(none)"));
            btn->Bind(wxEVT_BUTTON, [this, target](wxCommandEvent&) { arm_plane_pick(target); });
            plform->Add(btn);
            plform->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        };
        pick_row(_L("Pick Face A"), m_plane_pick_faceA, m_plane_faceA_lbl, PlanePick::FaceA);
        pick_row(_L("Pick Face B"), m_plane_pick_faceB, m_plane_faceB_lbl, PlanePick::FaceB);
        pick_row(_L("Pick Edge A"), m_plane_pick_edgeA, m_plane_edgeA_lbl, PlanePick::EdgeA);
        pick_row(_L("Pick Edge B"), m_plane_pick_edgeB, m_plane_edgeB_lbl, PlanePick::EdgeB);

        m_plane_usize = make_spin(m_cards, 60.0, 1.0, 100000.0);
        plform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Size U")), 0, wxALIGN_CENTER_VERTICAL);
        plform->Add(spin_frame(m_plane_usize), 0, wxEXPAND);
        m_plane_vsize = make_spin(m_cards, 60.0, 1.0, 100000.0);
        plform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Size V")), 0, wxALIGN_CENTER_VERTICAL);
        plform->Add(spin_frame(m_plane_vsize), 0, wxEXPAND);

        m_box_plane->Add(plform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_plane, 0, wxEXPAND);

    // --- Loft (skin a solid through 2+ ordered profile sketches) ---
    m_box_loft = new wxBoxSizer(wxVERTICAL);
    m_box_loft->Add(card_header(m_cards, "design_loft", _L("Loft"), m_hdr_loft), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_loft->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_box_loft->Add(new wxStaticText(m_cards, wxID_ANY, _L("Profiles (check 2+, in order):")),
                    0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_loft_list = new wxCheckListBox(m_cards, wxID_ANY, wxDefaultPosition, wxSize(-1, 120));
    m_loft_list->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent&) { refresh_preview(); });
    m_box_loft->Add(m_loft_list, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    {
        auto* lform = two_col_form();

        m_loft_mode = make_combo(m_cards);
        m_loft_mode->Append(_L("New"));
        m_loft_mode->Append(_L("Add"));
        m_loft_mode->Append(_L("Cut"));
        m_loft_mode->Append(_L("Intersect"));
        m_loft_mode->SetSelection(0);
        lform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Mode")), 0, wxALIGN_CENTER_VERTICAL);
        lform->Add(m_loft_mode, 0, wxEXPAND);

        m_box_loft->Add(lform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    m_loft_ruled = new CheckBox(m_cards);
    {
        auto* lrow = new wxBoxSizer(wxHORIZONTAL);
        lrow->Add(new wxStaticText(m_cards, wxID_ANY, _L("Ruled (straight) sections")), 0, wxALIGN_CENTER_VERTICAL);
        lrow->AddStretchSpacer();
        lrow->Add(m_loft_ruled, 0, wxALIGN_CENTER_VERTICAL);
        m_box_loft->Add(lrow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_loft, 0, wxEXPAND);

    // --- Surface Extrude (sheet body from sketch) ---
    m_box_surf_extrude = new wxBoxSizer(wxVERTICAL);
    m_box_surf_extrude->Add(card_header(m_cards, "design_extrude", _L("Surface Extrude"), m_hdr_surf_extrude), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_surf_extrude->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_surf_extrude_sketch_label = new wxStaticText(m_cards, wxID_ANY, _L("Sketch: —"));
    m_box_surf_extrude->Add(m_surf_extrude_sketch_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    {
        auto* sform = two_col_form();
        m_surf_extrude_distance = make_spin(m_cards, 10);
        sform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Distance")), 0, wxALIGN_CENTER_VERTICAL);
        sform->Add(spin_frame(m_surf_extrude_distance), 0, wxEXPAND);
        m_box_surf_extrude->Add(sform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_surf_extrude, 0, wxEXPAND);

    // --- Surface Revolve (sheet body from sketch about axis) ---
    m_box_surf_revolve = new wxBoxSizer(wxVERTICAL);
    m_box_surf_revolve->Add(card_header(m_cards, "design_revolve", _L("Surface Revolve"), m_hdr_surf_revolve), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_surf_revolve->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_surf_revolve_sketch_label = new wxStaticText(m_cards, wxID_ANY, _L("Sketch: —"));
    m_box_surf_revolve->Add(m_surf_revolve_sketch_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    {
        auto* rform = two_col_form();
        m_surf_revolve_angle = make_spin(m_cards, 360.0, 1.0, 360.0);
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Angle °")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(spin_frame(m_surf_revolve_angle), 0, wxEXPAND);
        m_surf_revolve_axis = make_combo(m_cards);
        m_surf_revolve_axis->Append(_L("Plane X"));
        m_surf_revolve_axis->Append(_L("Plane Y"));
        m_surf_revolve_axis->SetSelection(0);
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Axis")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_surf_revolve_axis, 0, wxEXPAND);
        m_surf_revolve_flip = new CheckBox(m_cards);
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Flip direction")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_surf_revolve_flip, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
        m_box_surf_revolve->Add(rform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_surf_revolve, 0, wxEXPAND);

    // --- Surface Loft (sheet skin through 2+ ordered profile sketches) ---
    m_box_surf_loft = new wxBoxSizer(wxVERTICAL);
    m_box_surf_loft->Add(card_header(m_cards, "design_loft", _L("Surface Loft"), m_hdr_surf_loft), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_surf_loft->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_box_surf_loft->Add(new wxStaticText(m_cards, wxID_ANY, _L("Profiles (check 2+, in order):")),
                         0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_surf_loft_list = new wxCheckListBox(m_cards, wxID_ANY, wxDefaultPosition, wxSize(-1, 120));
    m_surf_loft_list->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent&) { refresh_preview(); });
    m_box_surf_loft->Add(m_surf_loft_list, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    m_surf_loft_ruled = new CheckBox(m_cards);
    {
        auto* lrow = new wxBoxSizer(wxHORIZONTAL);
        lrow->Add(new wxStaticText(m_cards, wxID_ANY, _L("Ruled (straight) sections")), 0, wxALIGN_CENTER_VERTICAL);
        lrow->AddStretchSpacer();
        lrow->Add(m_surf_loft_ruled, 0, wxALIGN_CENTER_VERTICAL);
        m_box_surf_loft->Add(lrow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_surf_loft, 0, wxEXPAND);

    // --- Surface Fill (one-face sheet from sketch boundary) ---
    m_box_surf_fill = new wxBoxSizer(wxVERTICAL);
    m_box_surf_fill->Add(card_header(m_cards, "design_surface", _L("Surface Fill"), m_hdr_surf_fill), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_surf_fill->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_surf_fill_sketch_label = new wxStaticText(m_cards, wxID_ANY, _L("Sketch: —"));
    m_box_surf_fill->Add(m_surf_fill_sketch_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_surf_fill->Add(new wxStaticText(m_cards, wxID_ANY,
                          _L("Fills the sketch's closed boundary with a smooth face.")),
                         0, wxLEFT | wxRIGHT, 12);
    cards->Add(m_box_surf_fill, 0, wxEXPAND);

    // --- Surface Offset (offset a sheet body) ---
    m_box_surf_offset = new wxBoxSizer(wxVERTICAL);
    m_box_surf_offset->Add(card_header(m_cards, "design_offset", _L("Surface Offset"), m_hdr_surf_offset), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_surf_offset->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* oform = two_col_form();
        m_surf_offset_body = make_combo(m_cards);
        m_surf_offset_body->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        oform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Sheet body")), 0, wxALIGN_CENTER_VERTICAL);
        oform->Add(m_surf_offset_body, 0, wxEXPAND);
        m_surf_offset_distance = make_spin(m_cards, 1.0, -100000.0, 100000.0);
        m_surf_offset_distance->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        oform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Offset")), 0, wxALIGN_CENTER_VERTICAL);
        oform->Add(spin_frame(m_surf_offset_distance), 0, wxEXPAND);
        m_box_surf_offset->Add(oform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_surf_offset->Add(new wxStaticText(m_cards, wxID_ANY,
                                  _L("Offsets a sheet body's shell. Positive = outward, negative = inward.")),
                               0, wxLEFT | wxRIGHT, 12);
    }
    cards->Add(m_box_surf_offset, 0, wxEXPAND);

    // --- Thicken Surface (thicken a sheet body into a solid) ---
    m_box_surf_thicken = new wxBoxSizer(wxVERTICAL);
    m_box_surf_thicken->Add(card_header(m_cards, "design_thicken", _L("Thicken Surface"), m_hdr_surf_thicken), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_surf_thicken->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* tform = two_col_form();
        m_surf_thicken_body = make_combo(m_cards);
        m_surf_thicken_body->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Sheet body")), 0, wxALIGN_CENTER_VERTICAL);
        tform->Add(m_surf_thicken_body, 0, wxEXPAND);
        m_surf_thicken_thickness = make_spin(m_cards, 1.0, 0.01, 100000.0);
        m_surf_thicken_thickness->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Thickness")), 0, wxALIGN_CENTER_VERTICAL);
        tform->Add(spin_frame(m_surf_thicken_thickness), 0, wxEXPAND);
        m_surf_thicken_flip = new CheckBox(m_cards);
        m_surf_thicken_flip->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { refresh_preview(); });
        tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Flip direction")), 0, wxALIGN_CENTER_VERTICAL);
        tform->Add(m_surf_thicken_flip, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
        m_box_surf_thicken->Add(tform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_surf_thicken->Add(new wxStaticText(m_cards, wxID_ANY,
                                  _L("Thickens a sheet body into a solid. Flip reverses the direction.")),
                               0, wxLEFT | wxRIGHT, 12);
    }
    cards->Add(m_box_surf_thicken, 0, wxEXPAND);

    // --- Shell (hollow the current body to a wall thickness, removing one picked face) ---
    auto* sform = two_col_form();
    m_shell_thickness = make_spin(m_cards, 2.0, 0.01, 100000.0);
    sform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Thickness")), 0, wxALIGN_CENTER_VERTICAL);
    sform->Add(spin_frame(m_shell_thickness), 0, wxEXPAND);
    m_shell_face_label = new wxStaticText(m_cards, wxID_ANY, _L("(all faces — closed hollow)"));
    sform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Open face")), 0, wxALIGN_CENTER_VERTICAL);
    sform->Add(m_shell_face_label, 0, wxALIGN_CENTER_VERTICAL);

    m_box_shell = new wxBoxSizer(wxVERTICAL);
    m_box_shell->Add(card_header(m_cards, "design_shell", _L("Shell"), m_hdr_shell), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_shell->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_box_shell->Add(new wxStaticText(m_cards, wxID_ANY,
                        _L("Pick a solid face to open it, then set the wall thickness.")),
                     0, wxLEFT | wxRIGHT, 12);
    m_box_shell->Add(sform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    cards->Add(m_box_shell, 0, wxEXPAND);

    // --- Draft (taper a single picked solid face about the body bottom) ---
    auto* drform = two_col_form();
    m_draft_angle = make_spin(m_cards, 5.0, -89.0, 89.0);
    drform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Angle (°)")), 0, wxALIGN_CENTER_VERTICAL);
    drform->Add(spin_frame(m_draft_angle), 0, wxEXPAND);
    m_draft_face_label = new wxStaticText(m_cards, wxID_ANY, _L("(pick a side face)"));
    drform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Face")), 0, wxALIGN_CENTER_VERTICAL);
    drform->Add(m_draft_face_label, 0, wxALIGN_CENTER_VERTICAL);

    m_box_draft = new wxBoxSizer(wxVERTICAL);
    m_box_draft->Add(card_header(m_cards, "design_draft", _L("Draft"), m_hdr_draft), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_draft->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_box_draft->Add(new wxStaticText(m_cards, wxID_ANY,
                        _L("Pick a side face, then set the draft angle. The face pivots about the body base.")),
                     0, wxLEFT | wxRIGHT, 12);
    m_box_draft->Add(drform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    cards->Add(m_box_draft, 0, wxEXPAND);

    // --- Transform (rigid move/rotate of an existing body) ---
    m_box_transform = new wxBoxSizer(wxVERTICAL);
    m_box_transform->Add(card_header(m_cards, "design_move", _L("Transform"), m_hdr_transform), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_transform->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* xform = two_col_form();
        m_xf_body = make_combo(m_cards);
        m_xf_body->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        xform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Body")), 0, wxALIGN_CENTER_VERTICAL);
        xform->Add(m_xf_body, 0, wxEXPAND);
        m_xf_dx = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_xf_dx->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        xform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Translate X")), 0, wxALIGN_CENTER_VERTICAL);
        xform->Add(spin_frame(m_xf_dx), 0, wxEXPAND);
        m_xf_dy = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_xf_dy->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        xform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Translate Y")), 0, wxALIGN_CENTER_VERTICAL);
        xform->Add(spin_frame(m_xf_dy), 0, wxEXPAND);
        m_xf_dz = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_xf_dz->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        xform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Translate Z")), 0, wxALIGN_CENTER_VERTICAL);
        xform->Add(spin_frame(m_xf_dz), 0, wxEXPAND);
        m_xf_axis = make_combo(m_cards);
        m_xf_axis->Append(_L("X"));
        m_xf_axis->Append(_L("Y"));
        m_xf_axis->Append(_L("Z"));
        m_xf_axis->SetSelection(2);  // default Z
        m_xf_axis->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        xform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Rotate axis")), 0, wxALIGN_CENTER_VERTICAL);
        xform->Add(m_xf_axis, 0, wxEXPAND);
        m_xf_angle = make_spin(m_cards, 0.0, -360.0, 360.0);
        m_xf_angle->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        xform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Angle (°)")), 0, wxALIGN_CENTER_VERTICAL);
        xform->Add(spin_frame(m_xf_angle), 0, wxEXPAND);
        m_xf_pivot_x = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_xf_pivot_x->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        xform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Pivot X")), 0, wxALIGN_CENTER_VERTICAL);
        xform->Add(spin_frame(m_xf_pivot_x), 0, wxEXPAND);
        m_xf_pivot_y = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_xf_pivot_y->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        xform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Pivot Y")), 0, wxALIGN_CENTER_VERTICAL);
        xform->Add(spin_frame(m_xf_pivot_y), 0, wxEXPAND);
        m_xf_pivot_z = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_xf_pivot_z->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        xform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Pivot Z")), 0, wxALIGN_CENTER_VERTICAL);
        xform->Add(spin_frame(m_xf_pivot_z), 0, wxEXPAND);
        m_box_transform->Add(xform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_xf_copy = new CheckBox(m_cards);
        m_xf_copy->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { refresh_preview(); });
        auto* cpro = new wxBoxSizer(wxHORIZONTAL);
        cpro->Add(new wxStaticText(m_cards, wxID_ANY, _L("Keep original (make a copy)")), 0, wxALIGN_CENTER_VERTICAL);
        cpro->AddStretchSpacer();
        cpro->Add(m_xf_copy, 0, wxALIGN_CENTER_VERTICAL);
        m_box_transform->Add(cpro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_transform->Add(new wxStaticText(m_cards, wxID_ANY,
                               _L("Translate and/or rotate the selected body. Rotation is applied about the pivot, then translation follows.")),
                            0, wxLEFT | wxRIGHT, 12);
    }
    cards->Add(m_box_transform, 0, wxEXPAND);

    // --- Mirror (reflect a body about a plane) ---
    m_box_mirror = new wxBoxSizer(wxVERTICAL);
    m_box_mirror->Add(card_header(m_cards, "design_mirror", _L("Mirror"), m_hdr_mirror), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_mirror->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* mform = two_col_form();
        m_mirror_body = make_combo(m_cards);
        m_mirror_body->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        mform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Body")), 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(m_mirror_body, 0, wxEXPAND);
        m_mirror_plane = make_combo(m_cards);
        m_mirror_plane->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        mform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Mirror plane")), 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(m_mirror_plane, 0, wxEXPAND);
        m_box_mirror->Add(mform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_mirror_keep = new CheckBox(m_cards);
        m_mirror_keep->SetValue(true);
        m_mirror_keep->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { refresh_preview(); });
        auto* mkrow = new wxBoxSizer(wxHORIZONTAL);
        mkrow->Add(new wxStaticText(m_cards, wxID_ANY, _L("Keep original")), 0, wxALIGN_CENTER_VERTICAL);
        mkrow->AddStretchSpacer();
        mkrow->Add(m_mirror_keep, 0, wxALIGN_CENTER_VERTICAL);
        m_box_mirror->Add(mkrow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_mirror->Add(new wxStaticText(m_cards, wxID_ANY,
                            _L("Reflect the selected body about a plane. The mirror is always a new body; keeping the original gives you both.")),
                         0, wxLEFT | wxRIGHT, 12);
    }
    cards->Add(m_box_mirror, 0, wxEXPAND);

    // --- Thicken (offset one solid face into a thin plate) ---
    m_box_thicken = new wxBoxSizer(wxVERTICAL);
    m_box_thicken->Add(card_header(m_cards, "design_thicken", _L("Thicken"), m_hdr_thicken), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_thicken->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* tform = two_col_form();
        m_thicken_body = make_combo(m_cards);
        m_thicken_body->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Body")), 0, wxALIGN_CENTER_VERTICAL);
        tform->Add(m_thicken_body, 0, wxEXPAND);
        m_thicken_face_label = new wxStaticText(m_cards, wxID_ANY, _L("(pick a solid face)"));
        tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Face")), 0, wxALIGN_CENTER_VERTICAL);
        tform->Add(m_thicken_face_label, 0, wxALIGN_CENTER_VERTICAL);
        m_thicken_thickness = make_spin(m_cards, 2.0, 0.01, 100000.0);
        m_thicken_thickness->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        tform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Thickness")), 0, wxALIGN_CENTER_VERTICAL);
        tform->Add(spin_frame(m_thicken_thickness), 0, wxEXPAND);
        m_box_thicken->Add(tform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_thicken_flip = new CheckBox(m_cards);
        m_thicken_flip->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { refresh_preview(); });
        auto* trow = new wxBoxSizer(wxHORIZONTAL);
        trow->Add(new wxStaticText(m_cards, wxID_ANY, _L("Flip direction")), 0, wxALIGN_CENTER_VERTICAL);
        trow->AddStretchSpacer();
        trow->Add(m_thicken_flip, 0, wxALIGN_CENTER_VERTICAL);
        m_box_thicken->Add(trow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_thicken->Add(new wxStaticText(m_cards, wxID_ANY,
                              _L("Pick a solid face and offset it into a new thin body. Not for sheet bodies — use Thicken Surface for those.")),
                           0, wxLEFT | wxRIGHT, 12);
    }
    cards->Add(m_box_thicken, 0, wxEXPAND);

    // --- Rib (thin wall from an open sketch line, fused to a body) ---
    m_box_rib = new wxBoxSizer(wxVERTICAL);
    m_box_rib->Add(card_header(m_cards, "design_rib", _L("Rib"), m_hdr_rib), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_rib->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* rform = two_col_form();
        m_rib_body = make_combo(m_cards);
        m_rib_body->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Target body")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_rib_body, 0, wxEXPAND);
        m_rib_sketch = make_combo(m_cards);
        m_rib_sketch->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Sketch")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_rib_sketch, 0, wxEXPAND);
        m_rib_entity = new wxSpinCtrl(m_cards, wxID_ANY, "", wxDefaultPosition, wxSize(90, -1),
                                      wxSP_ARROW_KEYS | wxBORDER_SIMPLE);
        m_rib_entity->SetRange(0, 999);
        m_rib_entity->SetValue(0);
        m_rib_entity->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { refresh_preview(); });
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Entity index")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_rib_entity, 0, wxEXPAND);
        m_rib_thickness = make_spin(m_cards, 2.0, 0.01, 100000.0);
        m_rib_thickness->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Thickness")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(spin_frame(m_rib_thickness), 0, wxEXPAND);
        m_rib_depth = make_spin(m_cards, 10.0, 0.01, 100000.0);
        m_rib_depth->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        rform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Depth")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(spin_frame(m_rib_depth), 0, wxEXPAND);
        m_box_rib->Add(rform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_rib->Add(new wxStaticText(m_cards, wxID_ANY,
                          _L("Grow a thin wall from an open sketch Line entity, fused to the target body. Entity index is the position of the open Line in the sketch.")),
                       0, wxLEFT | wxRIGHT, 12);
    }
    cards->Add(m_box_rib, 0, wxEXPAND);

    // --- Project (project body edges onto a plane as sketch entities) ---
    m_box_project = new wxBoxSizer(wxVERTICAL);
    m_box_project->Add(card_header(m_cards, "design_sketch", _L("Project"), m_hdr_project), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_project->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* pform = two_col_form();
        m_proj_source_body = make_combo(m_cards);
        m_proj_source_body->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        pform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Source body")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(m_proj_source_body, 0, wxEXPAND);
        m_proj_face_label = new wxStaticText(m_cards, wxID_ANY, _L("(all edges)"));
        pform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Face")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(m_proj_face_label, 0, wxALIGN_CENTER_VERTICAL);
        m_proj_plane = make_combo(m_cards);
        populate_plane_choices(m_proj_plane);
        m_proj_plane->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        pform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Target plane")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(m_proj_plane, 0, wxEXPAND);
        m_box_project->Add(pform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_project->Add(new wxStaticText(m_cards, wxID_ANY,
                              _L("Project the edges of a body onto a plane as sketch entities. Pick a face to project only its edges, or leave the face empty to project the whole body.")),
                           0, wxLEFT | wxRIGHT, 12);
    }
    cards->Add(m_box_project, 0, wxEXPAND);

    // --- Delete Face (remove faces, heal the solid) ---
    m_box_delete_face = new wxBoxSizer(wxVERTICAL);
    m_box_delete_face->Add(card_header(m_cards, "design_delete", _L("Delete Face"), m_hdr_delete_face), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_delete_face->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* dform = two_col_form();
        m_del_face_body = make_combo(m_cards);
        m_del_face_body->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        dform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Body")), 0, wxALIGN_CENTER_VERTICAL);
        dform->Add(m_del_face_body, 0, wxEXPAND);
        m_del_face_add_btn = new wxButton(m_cards, wxID_ANY, _L("Add picked face"));
        m_del_face_add_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            // Say why nothing happened. Clicking with no face picked used to be a silent no-op,
            // which is indistinguishable from the button being broken.
            if (m_sel_solid_face < 0) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                set_status(_L("Click a face on the body first, then Add picked face"));
                m_status->Refresh();
                return;
            }
            // Adding the same face twice puts a duplicate id in delete_faces, which the
            // defeaturing algorithm has no reason to cope with. Re-clicking is a no-op, not an error.
            if (std::find(m_del_faces.begin(), m_del_faces.end(), m_sel_solid_face) != m_del_faces.end()) {
                m_status->SetForegroundColour(wxNullColour);
                set_status(wxString::Format(_L("Face %d is already in the list"), m_sel_solid_face));
                m_status->Refresh();
                return;
            }
            {
                m_del_faces.push_back(m_sel_solid_face);
                wxString s;
                for (size_t i = 0; i < m_del_faces.size(); ++i) {
                    if (i > 0) s += ", ";
                    s += wxString::Format("Face %d", m_del_faces[i]);
                }
                m_del_face_list->SetLabel(s.empty() ? _L("(none)") : s);
                m_del_face_list->GetParent()->Layout();
                m_del_face_list->Refresh();
                refresh_preview();
            }
        });
        dform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Faces to delete")), 0, wxALIGN_CENTER_VERTICAL);
        dform->Add(m_del_face_add_btn, 0, wxALIGN_CENTER_VERTICAL);
        m_del_face_list = new wxStaticText(m_cards, wxID_ANY, _L("(none)"));
        dform->Add(0, 0);
        dform->Add(m_del_face_list, 0, wxALIGN_CENTER_VERTICAL);
        m_box_delete_face->Add(dform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_delete_face->Add(new wxStaticText(m_cards, wxID_ANY,
                                  _L("Pick a solid face, then click 'Add picked face' to accumulate faces. Confirm to remove all listed faces and heal the solid.")),
                               0, wxLEFT | wxRIGHT, 12);
    }
    cards->Add(m_box_delete_face, 0, wxEXPAND);

    // --- Helix (helical curve) ---
    m_box_helix = new wxBoxSizer(wxVERTICAL);
    m_box_helix->Add(card_header(m_cards, "design_thread", _L("Helix"), m_hdr_helix), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_helix->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* hform = two_col_form();
        m_helix_plane = make_combo(m_cards);
        populate_plane_choices(m_helix_plane);
        m_helix_plane->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
        hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Plane")), 0, wxALIGN_CENTER_VERTICAL);
        hform->Add(m_helix_plane, 0, wxEXPAND);
        m_helix_radius = make_spin(m_cards, 10.0, 0.01, 10000.0);
        m_helix_radius->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Radius")), 0, wxALIGN_CENTER_VERTICAL);
        hform->Add(spin_frame(m_helix_radius), 0, wxEXPAND);
        m_helix_pitch = make_spin(m_cards, 5.0, 0.01, 10000.0);
        m_helix_pitch->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Pitch")), 0, wxALIGN_CENTER_VERTICAL);
        hform->Add(spin_frame(m_helix_pitch), 0, wxEXPAND);
        m_helix_height = make_spin(m_cards, 20.0, 0.01, 10000.0);
        m_helix_height->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        hform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Height")), 0, wxALIGN_CENTER_VERTICAL);
        hform->Add(spin_frame(m_helix_height), 0, wxEXPAND);
        m_box_helix->Add(hform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_helix_left_handed = new CheckBox(m_cards);
        m_helix_left_handed->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { refresh_preview(); });
        auto* hrow = new wxBoxSizer(wxHORIZONTAL);
        hrow->Add(new wxStaticText(m_cards, wxID_ANY, _L("Left-handed")), 0, wxALIGN_CENTER_VERTICAL);
        hrow->AddStretchSpacer();
        hrow->Add(m_helix_left_handed, 0, wxALIGN_CENTER_VERTICAL);
        m_box_helix->Add(hrow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        m_helix_taper = make_spin(m_cards, 0.0, -89.0, 89.0);
        m_helix_taper->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        auto* tvalform = two_col_form();
        tvalform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Taper (°)")), 0, wxALIGN_CENTER_VERTICAL);
        tvalform->Add(spin_frame(m_helix_taper), 0, wxEXPAND);
        m_box_helix->Add(tvalform, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
        m_box_helix->Add(new wxStaticText(m_cards, wxID_ANY,
                            _L("A helical curve (spring path). Use as a sweep path to build springs, coils, or augers.")),
                         0, wxLEFT | wxRIGHT, 12);
    }
    cards->Add(m_box_helix, 0, wxEXPAND);

    // --- Axis (datum axis: line through two points or derived from geometry) ---
    m_box_axis = new wxBoxSizer(wxVERTICAL);
    m_box_axis->Add(card_header(m_cards, "design_line", _L("Axis"), m_hdr_axis), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_axis->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        m_axis_type = make_combo(m_cards);
        for (const wxString& t : { _L("Two points"), _L("Face normal"), _L("Cylinder centerline"),
                                   _L("Plane intersection"), _L("Along edge") })
            m_axis_type->Append(t);
        m_axis_type->SetSelection(0);
        m_box_axis->Add(new wxStaticText(m_cards, wxID_ANY, _L("Axis type")), 0, wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_axis->Add(m_axis_type, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        auto* axform = two_col_form();

        auto ax_pick = [&](const wxString& label, wxButton*& btn, wxStaticText*& lbl, AxisPick target) {
            btn = new wxButton(m_cards, wxID_ANY, label);
            lbl = new wxStaticText(m_cards, wxID_ANY, _L("(none)"));
            btn->Bind(wxEVT_BUTTON, [this, target](wxCommandEvent&) { arm_axis_pick(target); });
            axform->Add(btn);
            axform->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        };
        ax_pick(_L("Pick Face"),  m_axis_pick_face, m_axis_face_lbl, AxisPick::Face);
        ax_pick(_L("Pick Edge"),  m_axis_pick_edge, m_axis_edge_lbl, AxisPick::Edge);

        m_axis_plane_a = make_combo(m_cards);
        populate_plane_choices(m_axis_plane_a);
        axform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Plane A")), 0, wxALIGN_CENTER_VERTICAL);
        axform->Add(m_axis_plane_a, 0, wxEXPAND);
        m_axis_plane_b = make_combo(m_cards);
        populate_plane_choices(m_axis_plane_b);
        axform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Plane B")), 0, wxALIGN_CENTER_VERTICAL);
        axform->Add(m_axis_plane_b, 0, wxEXPAND);

        m_axis_p1x = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_axis_p1y = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_axis_p1z = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        axform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Point 1 X")), 0, wxALIGN_CENTER_VERTICAL);
        axform->Add(spin_frame(m_axis_p1x), 0, wxEXPAND);
        axform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Point 1 Y")), 0, wxALIGN_CENTER_VERTICAL);
        axform->Add(spin_frame(m_axis_p1y), 0, wxEXPAND);
        axform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Point 1 Z")), 0, wxALIGN_CENTER_VERTICAL);
        axform->Add(spin_frame(m_axis_p1z), 0, wxEXPAND);

        m_axis_p2x = make_spin(m_cards, 10.0, -100000.0, 100000.0);
        m_axis_p2y = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_axis_p2z = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        axform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Point 2 X")), 0, wxALIGN_CENTER_VERTICAL);
        axform->Add(spin_frame(m_axis_p2x), 0, wxEXPAND);
        axform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Point 2 Y")), 0, wxALIGN_CENTER_VERTICAL);
        axform->Add(spin_frame(m_axis_p2y), 0, wxEXPAND);
        axform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Point 2 Z")), 0, wxALIGN_CENTER_VERTICAL);
        axform->Add(spin_frame(m_axis_p2z), 0, wxEXPAND);

        m_box_axis->Add(axform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_axis, 0, wxEXPAND);

    // --- CoordSys (datum coordinate system: point + orthonormal frame) ---
    m_box_coordsys = new wxBoxSizer(wxVERTICAL);
    m_box_coordsys->Add(card_header(m_cards, "design_point", _L("Coord Sys"), m_hdr_coordsys), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_coordsys->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        m_coordsys_type = make_combo(m_cards);
        for (const wxString& t : { _L("Point (world)"), _L("Face + direction") })
            m_coordsys_type->Append(t);
        m_coordsys_type->SetSelection(0);
        m_box_coordsys->Add(new wxStaticText(m_cards, wxID_ANY, _L("Type")), 0, wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_coordsys->Add(m_coordsys_type, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        m_cs_body = make_combo(m_cards);
        m_cs_body->Append(_L("(all)"));
        m_cs_body->SetSelection(0);
        m_cs_body->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            if (m_viewport) m_viewport->set_xray_focus(m_cs_body->GetSelection() - 1);
        });
        m_box_coordsys->Add(new wxStaticText(m_cards, wxID_ANY, _L("Body")), 0, wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_coordsys->Add(m_cs_body, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        auto* body_hint = new wxStaticText(m_cards, wxID_ANY,
            _L("Restrict picking to one body — the others go see-through and stop catching clicks"));
        body_hint->SetForegroundColour(dp_sec_text());
        m_box_coordsys->Add(body_hint, 0, wxLEFT | wxRIGHT | wxTOP, 12);

        auto* csform = two_col_form();

        m_cs_x = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_cs_y = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_cs_z = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        csform->Add(new wxStaticText(m_cards, wxID_ANY, _L("X")), 0, wxALIGN_CENTER_VERTICAL);
        csform->Add(spin_frame(m_cs_x), 0, wxEXPAND);
        csform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Y")), 0, wxALIGN_CENTER_VERTICAL);
        csform->Add(spin_frame(m_cs_y), 0, wxEXPAND);
        csform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Z")), 0, wxALIGN_CENTER_VERTICAL);
        csform->Add(spin_frame(m_cs_z), 0, wxEXPAND);

        auto cs_pick = [&](const wxString& label, wxButton*& btn, wxStaticText*& lbl, CoordSysPick target) {
            btn = new wxButton(m_cards, wxID_ANY, label);
            lbl = new wxStaticText(m_cards, wxID_ANY, _L("(none)"));
            btn->Bind(wxEVT_BUTTON, [this, target](wxCommandEvent&) { arm_coordsys_pick(target); });
            csform->Add(btn);
            csform->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        };
        cs_pick(_L("Pick Face"), m_cs_pick_face, m_cs_face_lbl, CoordSysPick::Face);

        // ponytail: edge pick for CoordSys with rotation-direction hint
        m_cs_pick_edge = new wxButton(m_cards, wxID_ANY, _L("Edge (sets in-plane direction)"));
        m_cs_edge_lbl = new wxStaticText(m_cards, wxID_ANY, _L("(none)"));
        m_cs_pick_edge->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { arm_coordsys_pick(CoordSysPick::Edge); });
        csform->Add(m_cs_pick_edge);
        csform->Add(m_cs_edge_lbl, 0, wxALIGN_CENTER_VERTICAL);
        auto* edge_hint = new wxStaticText(m_cards, wxID_ANY,
            _L("Without an edge the frame takes X from the face's first edge — pick one to choose it yourself"));
        edge_hint->SetForegroundColour(dp_sec_text());
        csform->Add(0, 0);   // empty left column
        csform->Add(edge_hint, 0, wxALIGN_CENTER_VERTICAL);

        m_cs_hx = make_spin(m_cards, 1.0, -100000.0, 100000.0);
        m_cs_hy = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        m_cs_hz = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        csform->Add(new wxStaticText(m_cards, wxID_ANY, _L("X hint X")), 0, wxALIGN_CENTER_VERTICAL);
        csform->Add(spin_frame(m_cs_hx), 0, wxEXPAND);
        csform->Add(new wxStaticText(m_cards, wxID_ANY, _L("X hint Y")), 0, wxALIGN_CENTER_VERTICAL);
        csform->Add(spin_frame(m_cs_hy), 0, wxEXPAND);
        csform->Add(new wxStaticText(m_cards, wxID_ANY, _L("X hint Z")), 0, wxALIGN_CENTER_VERTICAL);
        csform->Add(spin_frame(m_cs_hz), 0, wxEXPAND);

        m_box_coordsys->Add(csform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_coordsys, 0, wxEXPAND);

    // --- Mate (assembly: align two CoordSys features) ---
    m_box_mate = new wxBoxSizer(wxVERTICAL);
    m_box_mate->Add(card_header(m_cards, "design_c_coincident", _L("Mate"), m_hdr_mate), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_mate->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        auto* mform = two_col_form();

        m_mate_kind = make_combo(m_cards);
        m_mate_kind->Append(_L("Fastened — 6 DOF locked, B driven onto A exactly"));
        m_mate_kind->Append(_L("Planar — z aligned, offset distance; free in-plane slide"));
        m_mate_kind->Append(_L("Revolute — axes collinear; free rotation about z"));
        m_mate_kind->Append(_L("Slider — orientation locked; free axial slide"));
        m_mate_kind->Append(_L("Cylindrical — axes collinear; free spin + axial slide"));
        m_mate_kind->SetSelection(0);
        mform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Kind")), 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(m_mate_kind, 0, wxEXPAND);

        // Populate the CoordSys pickers on open; show "A (fixed)" and "B (moves)" combos.
        m_mate_cs_a = make_combo(m_cards);
        mform->Add(new wxStaticText(m_cards, wxID_ANY, _L("CS A (fixed)")), 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(m_mate_cs_a, 0, wxEXPAND);

        m_mate_cs_b = make_combo(m_cards);
        mform->Add(new wxStaticText(m_cards, wxID_ANY, _L("CS B (moves)")), 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(m_mate_cs_b, 0, wxEXPAND);

        m_offset_label = new wxStaticText(m_cards, wxID_ANY, _L("Offset"));
        m_mate_offset = make_spin(m_cards, 0.0, -100000.0, 100000.0);
        mform->Add(m_offset_label, 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(spin_frame(m_mate_offset), 0, wxEXPAND);

        m_angle_label = new wxStaticText(m_cards, wxID_ANY, _L("Angle \u00b0"));
        m_mate_angle = make_spin(m_cards, 0.0, -360.0, 360.0);
        mform->Add(m_angle_label, 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(spin_frame(m_mate_angle), 0, wxEXPAND);

        m_mate_flip = new CheckBox(m_cards);
        auto* frow = new wxBoxSizer(wxHORIZONTAL);
        frow->Add(new wxStaticText(m_cards, wxID_ANY, _L("Flip (oppose z axes)")), 0, wxALIGN_CENTER_VERTICAL);
        frow->AddStretchSpacer();
        frow->Add(m_mate_flip, 0, wxALIGN_CENTER_VERTICAL);
        mform->Add(0, 0);
        mform->Add(frow, 0, wxEXPAND);

        // Retitle offset/angle labels when the kind changes.
        m_mate_kind->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) {
            const int kind = m_mate_kind->GetSelection();
            switch (kind) {
            case 0: // Fastened
                m_offset_label->SetLabel(_L("Offset"));
                m_angle_label->SetLabel(_L("Angle \u00b0"));
                break;
            case 1: // Planar
                m_offset_label->SetLabel(_L("Plane distance"));
                m_angle_label->SetLabel(_L("Angle \u00b0"));
                break;
            case 2: // Revolute
                m_offset_label->SetLabel(_L("Axial position"));
                m_angle_label->SetLabel(_L("Angle \u00b0"));
                break;
            case 3: // Slider
                m_offset_label->SetLabel(_L("Axial position"));
                m_angle_label->SetLabel(_L("Angle \u00b0"));
                break;
            case 4: // Cylindrical
                m_offset_label->SetLabel(_L("Axial position"));
                m_angle_label->SetLabel(_L("Angle \u00b0"));
                break;
            }
            m_cards->Layout(); m_form->Layout(); m_form->FitInside();
            refresh_preview();
            e.Skip();
        });

        m_box_mate->Add(mform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_mate, 0, wxEXPAND);
    // Refresh the confirm state whenever the mate inputs change.
    m_mate_cs_a->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
    m_mate_cs_b->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { refresh_preview(); });
    m_mate_offset->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
    m_mate_angle->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
    m_mate_flip->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { refresh_preview(); });

    // --- Expression binding card (visible during feature edit only) ---
    // Lets the user bind a numeric field to a document-variable expression. Field-name
    // combo is populated per feature type and is editable for power users. Set applies
    // the binding with checkpoint+recompute+undo-on-failure; Clear removes it.
    m_box_expr = new wxBoxSizer(wxVERTICAL);
    {
        wxStaticText* expr_hdr = nullptr;
        m_box_expr->Add(card_header(m_cards, "design_constrain", _L("Expression"), expr_hdr), 0,
                        wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_expr->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);

        auto* eform = two_col_form();

        // The only editable combo in this panel, hence not make_combo(): that passes
        // wxCB_READONLY, and Orca's ComboBox HIDES its text ctrl under that style
        // (ComboBox.cpp:51), so there is nothing to type into and no SetEditable() to
        // turn it back on. Constructed with style 0, the ctrl is shown with
        // wxTE_PROCESS_ENTER and GetTextLabel() returns what the user typed — which is
        // what makes the 21 unmapped feature types reachable at all.
        m_expr_field = new ComboBox(m_cards, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                    wxSize(FromDIP(90), FromDIP(24)), 0, nullptr, 0);
        m_expr_field->SetMinSize(wxSize(FromDIP(90), FromDIP(24)));
        eform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Field")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(m_expr_field, 0, wxEXPAND);

        m_expr_text = new wxTextCtrl(m_cards, wxID_ANY, "", wxDefaultPosition,
                                     wxSize(FromDIP(120), FromDIP(24)),
                                     wxTE_PROCESS_ENTER | wxBORDER_SIMPLE);
        m_expr_text->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { on_set_expr(); });
        eform->Add(new wxStaticText(m_cards, wxID_ANY, _L("Expr")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(m_expr_text, 0, wxEXPAND);

        m_box_expr->Add(eform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        auto* brow = new wxBoxSizer(wxHORIZONTAL);
        m_expr_set_btn   = new wxButton(m_cards, wxID_ANY, _L("Set"), wxDefaultPosition, wxSize(50, 24));
        m_expr_clear_btn = new wxButton(m_cards, wxID_ANY, _L("Clear"), wxDefaultPosition, wxSize(50, 24));
        m_expr_set_btn->Bind(wxEVT_BUTTON,   [this](wxCommandEvent&) { on_set_expr(); });
        m_expr_clear_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_clear_expr(); });
        brow->Add(m_expr_set_btn,   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        brow->Add(m_expr_clear_btn, 0, wxALIGN_CENTER_VERTICAL);
        brow->AddStretchSpacer();
        m_box_expr->Add(brow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        m_expr_status = new wxStaticText(m_cards, wxID_ANY, _L("(no bindings)"));
        m_expr_status->SetForegroundColour(dp_sec_text());
        m_box_expr->Add(m_expr_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    }
    cards->Add(m_box_expr, 0, wxEXPAND);

    // --- Docked value-entry card (Onshape Button->Dialog->Confirm for dimensions) ---
    m_box_value = new wxBoxSizer(wxVERTICAL);
    {
        // Header title doubles as the operation label (set by request_value()).
        m_box_value->Add(card_header(m_cards, "design_constrain", _L("Value"), m_value_label), 0, wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_value->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
        auto* vrow = new wxBoxSizer(wxHORIZONTAL);
        // wxTE_PROCESS_ENTER so the user can just type a value and press Enter to
        // apply it (the natural CAD-dimension gesture), not only click Confirm.
        // Plain text field (not a spin control): on wxGTK the native GtkSpinButton
        // formats per the user locale (comma) with no clean override, so we own the
        // formatting here to guarantee international '.' decimals.
        m_value_input = new wxTextCtrl(m_cards, wxID_ANY, "", wxDefaultPosition,
                                       wxSize(90, -1), wxTE_PROCESS_ENTER);
        m_value_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { confirm_value(); });
        vrow->Add(new wxStaticText(m_cards, wxID_ANY, _L("Value")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        vrow->Add(m_value_input, 0, wxALIGN_CENTER_VERTICAL);
        m_box_value->Add(vrow, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    }
    cards->Add(m_box_value, 0, wxEXPAND);

    // --- Sketch-entry card (Phase 3): plane/orientation, opens on "New sketch",
    //     persists until Finish. The toolbar holds only the drawing tools. ---
    m_box_sketch_session = new wxBoxSizer(wxVERTICAL);
    m_box_sketch_session->Add(card_header(m_cards, "design_sketch", _L("Sketch"), m_hdr_sketch_session),
                              0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_sketch_session->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    {
        // NO plane dropdown. The sketch plane comes from what is picked in the VIEWPORT — a face
        // on a solid, or one of the reference-plane ghosts — because that is where the user is
        // looking and pointing. A combo duplicated that decision somewhere the geometry could not
        // see it, and once a face could be picked it went further and displayed a stale row that
        // contradicted the real target. snaporca-e1p.
        // Kept as a member, not a local: the card has to be able to STOP saying this. It asked
        // for a plane even when one had just been picked, directly contradicting the status line
        // two inches below it, which by then read "Sketching on XZ".
        m_sketch_hint = new wxStaticText(m_cards, wxID_ANY,
            _L("Click a face or a reference plane, then a sketch tool."));
        m_sketch_hint->SetForegroundColour(dp_sec_text());
        m_box_sketch_session->Add(m_sketch_hint, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 12);
    }
    cards->Add(m_box_sketch_session, 0, wxEXPAND);

    // --- Constraint-manager card (C3.4): list of the constrained sketch's
    //     entity-constraints; each row selects (highlights) + deletes. Shown only
    //     in Constrain mode; rebuilt by rebuild_constraint_list().
    m_box_constraints = new wxBoxSizer(wxVERTICAL);
    m_box_constraints->Add(card_header(m_cards, "design_constrain", _L("Constraints"), m_hdr_constraints),
                           0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_constraints->Add(new wxStaticLine(m_cards), 0, wxEXPAND | wxALL, 8);
    m_constraint_rows = new wxBoxSizer(wxVERTICAL);
    m_box_constraints->Add(m_constraint_rows, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    cards->Add(m_box_constraints, 0, wxEXPAND);

    // Wrap every card label now that the whole stack exists. wxStaticText never wraps on its
    // own, so a one-sentence help line under a card sets that card's sizer min width to the
    // width of the entire sentence — the same failure make_combo's explicit width fixes above,
    // and with the same symptom: the card is wider than the sidebar, so every control in it is
    // clipped at the panel's right edge. Found by click-testing Transform, Mirror and Coord Sys,
    // whose hints are the longest. 240 px sits under m_form's 264 px minimum, so a wrapped hint
    // always fits; the short two-column labels are far below it and Wrap() leaves them alone.
    for (wxWindow* w : m_cards->GetChildren())
        if (auto* t = dynamic_cast<wxStaticText*>(w))
            t->Wrap(240);

    // Feature tree card. Same idiom as Prepare's sections (icon + Head_14 title + rule) via the
    // shared card_header helper, instead of the bare micro-label this used to be; the row-edit
    // actions live in the header, as Prepare puts its section actions.
    m_tree_box = make_card(m_form);
    auto* tree_inner = new wxBoxSizer(wxVERTICAL);
    m_tree_box->SetSizer(tree_inner);
    root->Add(m_tree_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    m_hdr_tree_row = new wxBoxSizer(wxHORIZONTAL);
    m_hdr_tree_row->Add(card_header(m_tree_box, "design_sketch", _L("Feature tree"), m_hdr_tree), 0,
                        wxALIGN_CENTER_VERTICAL);
    m_hdr_tree_row->AddStretchSpacer();
    tree_inner->Add(m_hdr_tree_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
              FromDIP(SidebarProps::ContentMargin()));
    tree_inner->Add(new wxStaticLine(m_tree_box), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
              FromDIP(SidebarProps::TitlebarMargin()));
    m_tree = new wxTreeCtrl(m_tree_box, wxID_ANY, wxDefaultPosition, wxSize(-1, 64),
                            wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES |
                            wxTR_FULL_ROW_HIGHLIGHT | wxBORDER_SIMPLE);
    if (!dp_dark()) m_tree->SetBackgroundColour(dp_panel_bg());
    // Per-feature-type icons (indices match tree_icon_for): sketch/extrude/dressup/hole/thread.
    m_tree_images = new wxImageList(16, 16);
    m_tree_images->Add(create_scaled_bitmap("design_sketch",  nullptr, 16)); // 0 Sketch
    m_tree_images->Add(create_scaled_bitmap("design_extrude", nullptr, 16)); // 1 Extrude
    m_tree_images->Add(create_scaled_bitmap("design_dressup", nullptr, 16)); // 2 Fillet/Chamfer
    m_tree_images->Add(create_scaled_bitmap("design_hole",    nullptr, 16)); // 3 Hole
    m_tree_images->Add(create_scaled_bitmap("design_thread",  nullptr, 16)); // 4 Thread
    m_tree_images->Add(create_scaled_bitmap("design_shell",   nullptr, 16)); // 5 Shell
    m_tree->AssignImageList(m_tree_images);
    tree_inner->Add(m_tree, 0, wxEXPAND | wxALL, 12);

    // Selecting a body-producing feature (Extrude/Fillet/Chamfer/Hole/Thread) in the
    // tree highlights the solid in the viewport; a Sketch row clears the highlight
    // (its face is already shown via the persistent sketch overlay).
    m_tree->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent&) {
        if (!m_viewport) return;
        // Bodies live in the Parts list now; picking a feature here drops any body selection
        // so the two lists can't both claim to be "the target".
        if (m_parts) m_parts->UnselectAll();
        const int sel = tree_selection();
        const bool body = (sel >= 0 && sel < int(m_doc.features.size()) &&
                           m_doc.features[sel].type != CadFeatureType::Sketch &&
                           !m_doc.body.IsNull());
        m_viewport->set_body_highlight(body);
        // A conflicting mate says WHY on selection, and names the one action that resolves it.
        // Suppress is the generic per-feature enable toggle (the eye), so the answer is already
        // one click away on a row the user has just selected — the message points at it instead
        // of describing a problem with no way out.
        if (const std::string* why = (sel >= 0) ? mate_conflict_reason(sel) : nullptr) {
            m_status->SetForegroundColour(wxColour(235, 110, 110));
            set_status(wxString::FromUTF8(*why) + _L(" — the eye suppresses this mate"));
            m_status->Refresh();
        }
    });

    // Double-click a row = Edit, the same gesture that re-opens a committed sketch on the canvas.
    // Without it the row only highlights and the feature looks dead until the user finds the
    // Edit button in the section header.
    m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent&) { on_edit_feature(); });

    // Feature-tree edit actions: act on the selected feature (delete / reorder). These sit in the
    // card header (Prepare puts its section actions there too) rather than on a loose row below.
    {
        wxBoxSizer* trow = m_hdr_tree_row;
        auto edit_btn = [this](const char* icon, const wxString& tip) {
            // Header-sized: reads as a section action, not a primary control.
            auto* b = new ScalableButton(m_tree_box, wxID_ANY, icon, "", wxSize(24, 24),
                                         wxDefaultPosition, wxBU_EXACTFIT | wxBORDER_NONE, false, 20);
            b->SetToolTip(tip);
            return b;
        };
        auto* edit = edit_btn("design_edit", _L("Edit"));
        edit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_edit_feature(); });
        auto* move = edit_btn("design_move", _L("Move body / Scale imported Text-SVG"));
        move->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            const int sel = tree_selection();
            if (sel != wxNOT_FOUND && sel < int(m_doc.features.size()) &&
                !m_doc.features[sel].imported_regions.empty()) {
                on_transform_imported(sel);
            } else if (m_sel_solid_body >= 0 && m_sel_solid_body < int(m_doc.bodies.size())) {
                on_move_body();   // translate the selected body with the 3-axis gizmo
            } else {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                set_status(_L("Select a body to move it, or an imported Text/SVG to scale"));
                m_status->Refresh();
            }
        });
        auto* vis = edit_btn("design_eye", _L("Show / hide"));
        vis->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_toggle_visibility(); });
        auto* del  = edit_btn("design_delete", _L("Delete"));
        del->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            // A body row deletes the feature that made it. on_delete_body() already resolves
            // CadBody::source_feature and asks for confirmation by name; it was reachable only
            // from the right-click offer, so this button answered a selected body row with
            // "select the FEATURE that created this body" — an instruction the user cannot act
            // on, since the tree does not say which feature that is. It does now.
            if (tree_body_selection() >= 0) on_delete_body();
            else                            on_delete_feature();
        });
        auto* up   = edit_btn("design_moveup", _L("Move up"));
        up->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_move_feature(-1); });
        auto* down = edit_btn("design_movedown", _L("Move down"));
        down->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_move_feature(+1); });
        m_btn_interfere = edit_btn("color_palette", _L("Check interference — find overlapping bodies"));
        m_btn_interfere->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_check_interference(); });
        const int gap = FromDIP(SidebarProps::ElementSpacing());
        trow->Add(edit, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        trow->Add(move, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        trow->Add(vis,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        trow->Add(del,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        trow->Add(up,   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        trow->Add(down, 0, wxALIGN_CENTER_VERTICAL);
        // Interference check sits after a rule: it reports, it does not edit the recipe.
        trow->AddSpacer(8);
        trow->Add(new wxStaticLine(m_tree_box, wxID_ANY, wxDefaultPosition, wxSize(1, 22), wxLI_VERTICAL),
                  0, wxALIGN_CENTER_VERTICAL);
        trow->AddSpacer(8);
        trow->Add(m_btn_interfere, 0, wxALIGN_CENTER_VERTICAL);
        // trow IS the header sizer — already added to root above the tree.
    }

    // Parts list (Onshape's Features + Parts split). Bodies used to be appended after the
    // features INSIDE the feature tree, so they were pushed out of view as the history grew —
    // with no way to select a body at all. Their own list keeps them reachable regardless.
    m_parts_box = make_card(m_form);
    auto* parts_inner = new wxBoxSizer(wxVERTICAL);
    m_parts_box->SetSizer(parts_inner);
    root->Add(m_parts_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    m_parts_hdr = new wxBoxSizer(wxHORIZONTAL);
    m_parts_hdr->Add(card_header(m_parts_box, "design_extrude", _L("Bodies"), m_parts_label), 0,
                     wxALIGN_CENTER_VERTICAL);
    parts_inner->Add(m_parts_hdr, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
              FromDIP(SidebarProps::ContentMargin()));
    m_parts_rule = new wxStaticLine(m_parts_box);
    parts_inner->Add(m_parts_rule, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
              FromDIP(SidebarProps::TitlebarMargin()));
    m_parts_hdr->ShowItems(false);   // no bodies yet on a fresh document
    m_parts_rule->Hide();
    m_parts = new wxTreeCtrl(m_parts_box, wxID_ANY, wxDefaultPosition, wxSize(-1, 48),
                             wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES |
                             wxTR_FULL_ROW_HIGHLIGHT | wxBORDER_SIMPLE);
    if (!dp_dark()) m_parts->SetBackgroundColour(dp_panel_bg());
    parts_inner->Add(m_parts, 0, wxEXPAND | wxALL, 12);
    // Start hidden: a fresh document has no bodies, and refresh_parts() only runs on the first
    // tree rebuild — until then an empty box would sit under the header.
    m_parts->Hide();
    m_parts_label->Hide();
    // Taking a body from the list means the SAME state change however it was asked for, so the
    // normalisation lives here and not inside a selection handler. That distinction is not
    // pedantry: SelectItem() on a row that is ALREADY selected fires no SEL_CHANGED at all, so a
    // version of this that only ran on selection left a stale vertex/edge from an earlier
    // viewport pick in place — and offer_selection_kind() tests vertex FIRST, so right-clicking
    // the body row served the VERTEX offer (Fillet greyed, Mirror in place of Repeat) while the
    // row sat highlighted. Measured on the rig 2026-08-02; it is invisible from the code alone.
    auto apply_body_row = [this](int b) {
        if (!m_viewport || b < 0) return;
        // One selection at a time: a body row and a feature row mean different things to the
        // op bar, so clear the feature tree's highlight when a body takes over.
        if (m_tree) m_tree->UnselectAll();
        m_viewport->set_body_highlight(false);   // the per-body overlay does the tint
        m_viewport->select_body(b);              // also drops the vertex/edge marker
        m_sel_solid_body   = b;
        m_sel_solid_face   = m_sel_solid_edge = -1;
        m_sel_solid_vertex = false;
        m_pick_face = m_pick_face_body = -1;   // chosen from the list, no face was pointed at
        m_status->SetForegroundColour(wxNullColour);
        set_status(wxString::Format(_L("Body %d selected — right-click for what applies to it"), b + 1));
        m_status->Refresh();
    };
    m_parts->Bind(wxEVT_TREE_SEL_CHANGED, [this, apply_body_row](wxTreeEvent&) {
        apply_body_row(tree_body_selection());
    });
    // The third door onto the offer, after the viewport right-click and the Menu key. A body ROW
    // is an unambiguous body, so the offer reports BodySolid and the body verbs act on the row you
    // can see highlighted. That is the confirmation a face pick cannot give: pointing at a face
    // lights the face, never the body the verb will actually change. The status line above has
    // been promising this right-click since before it existed.
    m_parts->Bind(wxEVT_TREE_ITEM_MENU, [this, apply_body_row](wxTreeEvent& e) {
        if (e.GetItem().IsOk())
            m_parts->SelectItem(e.GetItem());   // the row under the cursor, never a stale one
        apply_body_row(tree_body_selection());  // unconditional — see above, SelectItem on an
                                                // already-selected row raises no event
        // GetPoint() is tree-client; it is (-1,-1) when the KEYBOARD menu key raised this, so fall
        // back to the shared anchor rather than popping the menu at a garbage coordinate.
        const wxPoint p = e.GetPoint();
        const wxPoint screen = (p.x >= 0 && p.y >= 0) ? m_parts->ClientToScreen(p) : offer_anchor();
        // Let the modal menu take the loop after this handler returns — same CallAfter as the
        // sketch path, which learned it the hard way.
        CallAfter([this, screen] { show_offer_menu(screen); });
    });

    // --- Variables (document-scope named expressions) ---
    // Below the feature tree + parts, always visible. wxListCtrl in report mode with two
    // columns (Name, Expression). Add / edit open a small dialog; delete removes the
    // selected row. Every mutation goes through checkpoint+recompute+undo-on-failure.
    m_var_box = make_card(m_form);
    auto* var_inner = new wxBoxSizer(wxVERTICAL);
    m_var_box->SetSizer(var_inner);
    {
        auto* var_hdr = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText* var_hdr_title = nullptr;
        var_hdr->Add(card_header(m_var_box, "design_constrain", _L("Variables"), var_hdr_title), 0,
                     wxALIGN_CENTER_VERTICAL);
        var_hdr->AddStretchSpacer();
        m_btn_add_var  = new wxButton(m_var_box, wxID_ANY, _L("+"), wxDefaultPosition, wxSize(30, 24));
        m_btn_edit_var = new wxButton(m_var_box, wxID_ANY, _L("Edit"), wxDefaultPosition, wxSize(50, 24));
        m_btn_del_var  = new wxButton(m_var_box, wxID_ANY, _L("Del"), wxDefaultPosition, wxSize(42, 24));
        m_btn_add_var->Bind(wxEVT_BUTTON,  [this](wxCommandEvent&) { on_add_variable(); });
        m_btn_edit_var->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_edit_variable(); });
        m_btn_del_var->Bind(wxEVT_BUTTON,  [this](wxCommandEvent&) { on_remove_variable(); });
        const int vhgap = FromDIP(SidebarProps::ElementSpacing());
        var_hdr->Add(m_btn_add_var,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, vhgap);
        var_hdr->Add(m_btn_edit_var, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, vhgap);
        var_hdr->Add(m_btn_del_var,  0, wxALIGN_CENTER_VERTICAL);
        var_inner->Add(var_hdr, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                       FromDIP(SidebarProps::ContentMargin()));
        var_inner->Add(new wxStaticLine(m_var_box), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                       FromDIP(SidebarProps::TitlebarMargin()));

        m_var_list = new wxListCtrl(m_var_box, wxID_ANY, wxDefaultPosition,
                                    wxSize(-1, FromDIP(64)), wxLC_REPORT | wxLC_SINGLE_SEL);
        if (!dp_dark()) m_var_list->SetBackgroundColour(dp_panel_bg());
        m_var_list->AppendColumn(_L("Name"),       wxLIST_FORMAT_LEFT, FromDIP(90));
        m_var_list->AppendColumn(_L("Expression"), wxLIST_FORMAT_LEFT, FromDIP(120));
        var_inner->Add(m_var_list, 0, wxEXPAND | wxALL, FromDIP(SidebarProps::ContentMargin()));
    }
    root->Add(m_var_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    // Orca-styled full-width buttons (ButtonType::Expanded), so the Design sidebar reads like
    // Prepare's instead of showing raw OS-default wxButtons.
    const int cm = FromDIP(SidebarProps::ContentMargin());

    m_status = new wxStaticText(m_form, wxID_ANY, "");
    m_status->Hide();   // storage only — the line is drawn over the viewport, see set_status()
    m_status_default_fg = m_status->GetForegroundColour();   // capture BEFORE any caller writes
    root->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    // DoF / constraint-state readout (P3). Dedicated line so it never clobbers the
    // tool hint in m_status; updated by the on_solve_state callback after each solve.
    m_dof_status = new wxStaticText(m_form, wxID_ANY, "");
    {
        wxFont f = m_dof_status->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        m_dof_status->SetFont(f);
    }
    root->Add(m_dof_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);


    m_shape->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) { on_shape_changed(); e.Skip(); });
    on_shape_changed();

    // Any parameter edit refreshes the translucent preview. Command events from the
    // spin/choice/checkbox children propagate up to m_form, so one binding each suffices.
    m_form->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent& e) { refresh_preview(); e.Skip(); });
    m_form->Bind(wxEVT_COMBOBOX,         [this](wxCommandEvent& e)    { refresh_preview(); e.Skip(); });
    m_form->Bind(wxEVT_CHECKBOX,       [this](wxCommandEvent& e)    { refresh_preview(); e.Skip(); });
    m_form->Bind(wxEVT_TOGGLEBUTTON,   [this](wxCommandEvent& e)    { refresh_preview(); e.Skip(); });

    m_form->SetSizer(root);

    // Start with every tool dialog hidden (only the toolbar + tree + Commit show).
    root->Show(m_parts_box, false, false);   // no bodies on a fresh document
    root->Show(m_cards,     false, false);   // no tool open yet: don't draw an empty frame
    cards->Show(m_box_sketch,  false, true);
    cards->Show(m_box_move,    false, true);
    cards->Show(m_box_extrude, false, true);
    cards->Show(m_box_revolve, false, true);
    cards->Show(m_box_sweep, false, true);
    cards->Show(m_box_pattern, false, true);
    cards->Show(m_box_plane, false, true);
    cards->Show(m_box_axis, false, true);
    cards->Show(m_box_coordsys, false, true);
    cards->Show(m_box_loft, false, true);
    cards->Show(m_box_draft, false, true);
    cards->Show(m_box_boolean, false, true);
    cards->Show(m_box_cut,     false, true);
    cards->Show(m_box_insert,  false, true);
    cards->Show(m_box_dressup, false, true);
    cards->Show(m_box_hole,    false, true);
    cards->Show(m_box_thread,  false, true);
    cards->Show(m_box_shell,   false, true);
    cards->Show(m_box_surf_extrude, false, true);
    cards->Show(m_box_surf_revolve, false, true);
    cards->Show(m_box_surf_loft,    false, true);
    cards->Show(m_box_surf_fill,    false, true);
    cards->Show(m_box_surf_offset,  false, true);
    cards->Show(m_box_surf_thicken, false, true);
    cards->Show(m_box_value,   false, true);
    cards->Show(m_box_sketch_session, false, true);
    cards->Show(m_box_constraints,    false, true);
    cards->Show(m_box_expr,          false, true);
    // A card added to the cards sizer is VISIBLE until something hides it. close_tool()'s
    // hide-all only runs on a tool switch, so any card missing from THIS block renders
    // stacked in the sidebar from the moment the tab opens. These eight were wired into
    // close_tool() but not here, which is what bloated the panel.
    cards->Show(m_box_transform,   false, true);
    cards->Show(m_box_mirror,      false, true);
    cards->Show(m_box_thicken,     false, true);
    cards->Show(m_box_rib,         false, true);
    cards->Show(m_box_project,     false, true);
    cards->Show(m_box_delete_face, false, true);
    cards->Show(m_box_helix,       false, true);
    cards->Show(m_box_mate,        false, true);

    m_form->FitInside();
    m_form->SetScrollRate(0, 10);   // vertical only, like Prepare's sidebar: never scroll labels out
    m_form->SetMinSize(wxSize(264, -1));

    // Right column: a small view toolbar over the live 3D viewport that mirrors
    // the CadDocument body.
    m_viewport = new DesignCanvas(this);

    m_viewport->set_on_sketch_commit([this](const SketchProfile& prof, const SketchPlane& plane) {
        m_doc.checkpoint();   // undo boundary: committing a sketch
        m_feature_counter++;
        m_doc.add_sketch_profile(prof, plane, "Sketch" + std::to_string(m_feature_counter));
        m_doc.recompute();
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("Sketch created — select it, then right-click to Extrude"));
        refresh_tree();
    });

    m_viewport->set_on_sketch_entities_commit(
        [this](const std::vector<SketchEntity>& ents,
               const std::vector<SketchEntityConstraintDef>& cons,
               const SketchPlane& plane) {
            if (ents.empty()) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                set_status(_L("Sketch empty — nothing committed"));
                m_status->Refresh();
                return;
            }
            m_doc.checkpoint();   // undo boundary: committing / re-editing an entity sketch
            // Re-edit of a committed entity sketch: REPLACE it in place (keep its name +
            // tree position) instead of appending a duplicate.
            if (m_edit_index >= 0 && m_edit_index < int(m_doc.features.size()) &&
                m_doc.features[m_edit_index].type == CadFeatureType::Sketch) {
                CadFeature edited = m_doc.features[m_edit_index];
                edited.entities           = ents;
                edited.entity_constraints = cons;
                edited.plane              = plane;
                if (m_doc.replace_feature(m_edit_index, edited)) {
                    if (!cons.empty()) m_doc.solve_sketch_feature(m_edit_index);
                    m_doc.recompute();
                    m_status->SetForegroundColour(wxNullColour);
                    set_status(_L("Sketch updated"));
                    m_edit_index = -1;
                    refresh_tree();
                    sync_sketch_display();
                }
                return;
            }
            m_feature_counter++;
            const int sk = m_doc.add_sketch_entities(ents, plane,
                               "Sketch" + std::to_string(m_feature_counter), cons);
            if (!cons.empty()) m_doc.solve_sketch_feature(sk);   // enforce driving dimensions
            m_doc.recompute();
            m_status->SetForegroundColour(wxNullColour);
            set_status(cons.empty()
                ? _L("Sketch created — select it, then right-click to Extrude")
                : wxString::Format(_L("Sketch created (%zu driving dims) — select it, then right-click to Extrude"),
                                   cons.size()));
            refresh_tree();
            sync_sketch_display();   // keep the just-committed sketch visible as a face
        });

    // Live length/angle readout while drawing a Line/Polyline segment.
    m_viewport->set_on_cursor_metrics([this](double len, double ang_deg, bool locked) {
        double a = ang_deg; if (a < 0.0) a += 360.0;   // show bearing 0..360
        m_status->SetForegroundColour(wxNullColour);
        set_status(wxString::Format(L"L %.2f mm   %.1f°%s",
                                            len, a, locked ? L"  (locked)" : L""));
        m_status->Refresh();
    });

    // DoF feedback (P3): after each live solve, report constraint state on its own
    // line. Green = fully constrained, red = conflicting, neutral = N remaining DoF.
    m_viewport->set_on_solve_state([this](int dof, bool ok, bool has_constraints) {
        // Remembered as well as shown: the readout is fed ONLY by a live solve, and entering
        // Constrain mode triggers none — so without a cache the very line the user goes to
        // Constrain to read stays blank until they happen to change something.
        m_dof_last = dof; m_dof_last_ok = ok; m_dof_last_has = has_constraints;
        apply_dof_status(dof, ok, has_constraints);
    });

    // Selection (Select tool): reflect the count in the status line.
    m_viewport->set_on_sketch_selection_changed([this](int count) {
        m_status->SetForegroundColour(wxNullColour);
        set_status(count > 0
            ? wxString::Format(_L("%d selected — Delete removes them"), count)
            : _L("Click to select; click a filled face to extrude; Shift to add"));
        m_status->Refresh();
    });

    // Onshape flow: clicking inside a closed-loop face commits the sketch and opens
    // the Extrude dialog (with a ghost preview) targeting that sketch.
    m_viewport->set_on_sketch_face_selected([this](int region) {
        if (!m_viewport) return;
        m_viewport->finish_sketch();                 // commit live sketch (synchronous)
        m_extrude_sketch_ref = resolve_extrude_sketch();
        if (m_extrude_sketch_ref < 0) {
            m_status->SetForegroundColour(wxColour(235, 110, 110));
            set_status(_L("Could not resolve the sketch to extrude"));
            m_status->Refresh();
            return;
        }
        set_ui_mode(UiMode::Feature);
        open_tool(Tool::Extrude);
        // AFTER open_tool, not before: opening the tool re-derives the selection state, so a
        // region recorded ahead of it is wiped before Extrude ever reads it.
        m_sel_sketch_feat   = m_extrude_sketch_ref;
        m_sel_sketch_region = region;
        m_sel_solid_face = m_sel_solid_edge = -1;
        m_pick_face = m_pick_face_body = -1;
        m_viewport->set_loop_pick(m_extrude_sketch_ref, region);
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("Face selected — set the depth and Confirm"));
        m_status->Refresh();
    });

    // Clicking a committed sketch loop on the plate (no live session) selects THAT loop:
    // the viewport highlights only it (cyan) and its Sketch feature's tree row is selected.
    // The (feature, region) pair is remembered so Extrude builds just that one loop.
    m_viewport->set_on_display_sketch_selected([this](int feat, int region, int entity) {
        if (feat < 0 || feat >= int(m_doc.features.size())) return;
        m_sel_sketch_feat   = feat;
        m_sel_sketch_region = region;
        // Last pick wins (symmetric with the solid-pick handler): selecting a sketch loop drops
        // any stale solid face/edge pick so Extrude treats this loop as the profile.
        m_sel_solid_face = m_sel_solid_edge = -1;
        m_pick_face = m_pick_face_body = -1;
        // Rib card open: point at the LINE, do not type its index. 'Entity index' was a bare
        // wxSpinCtrl ranged 0-999 standing in for a line sitting visible on screen, with nothing
        // anywhere telling you which integer was which — the purest case of the thing this epic
        // exists to remove. Only a stroke hit carries an entity (an interior click is a region,
        // not a line), so a click inside a loop deliberately leaves the field alone rather than
        // resetting it to something arbitrary. The sketch picker follows the same pick, so
        // pointing at a line in a different sketch retargets both together. snaporca-3648.
        if (m_active == Tool::Rib && entity >= 0) {
            if (m_rib_sketch != nullptr)
                for (unsigned i = 0; i < m_rib_sketch->GetCount(); ++i)
                    if (int(reinterpret_cast<intptr_t>(m_rib_sketch->GetClientData(i))) == feat) {
                        m_rib_sketch->SetSelection(i);
                        break;
                    }
            if (m_rib_entity != nullptr) m_rib_entity->SetValue(entity);
            refresh_preview();
        }
        // Sweep card open: the sketch you point at becomes the PATH. The profile is already
        // settled selection-first — you pick a sketch and then invoke Sweep, which is the design
        // law's own canonical example — so the path is the input that was still trapped in a
        // combo. The picker excludes the profile, so pointing at the profile correctly does
        // nothing. e1p item 6; same shape as the Boolean and Mirror fixes.
        if (m_active == Tool::Sweep && m_sweep_path != nullptr) {
            for (unsigned i = 0; i < m_sweep_path->GetCount(); ++i) {
                if (int(reinterpret_cast<intptr_t>(m_sweep_path->GetClientData(i))) != feat) continue;
                m_sweep_path->SetSelection(i);
                m_sweep_path_ref = feat;
                refresh_preview();
                break;
            }
        }
        // Loft card open: pointing at a sketch TOGGLES its membership, so a loft is built by
        // clicking the profiles in the viewport instead of hunting rows in a check-list. The
        // list stays visible as the typed half and still shows the order, which matters here —
        // loft order is not commutative.
        if (m_active == Tool::Loft && m_loft_list != nullptr) {
            for (size_t i = 0; i < m_loft_sketch_idx.size() && i < m_loft_list->GetCount(); ++i) {
                if (m_loft_sketch_idx[i] != feat) continue;
                m_loft_list->Check(unsigned(i), !m_loft_list->IsChecked(unsigned(i)));
                refresh_preview();
                break;
            }
        }
        set_tree_selection(feat);
        m_status->SetForegroundColour(wxNullColour);
        set_status(region >= 0
            // "Region", not "Loop": what is selected — and what Extrude will consume — is the
            // bounded area including any holes in it, not a single closed curve.
            ? _L("Region selected — right-click to Extrude, or double-click to edit")
            : _L("Sketch selected — right-click to Extrude, or double-click to edit"));
        m_status->Refresh();
    });

    // Double-click a committed sketch stroke: open THAT sketch for editing, where its entities
    // are individually selectable and their quotes editable. on_edit_feature already does the
    // whole job — it was only ever reachable from the tree, which is precisely the side-panel
    // dependency being retired. The pick has already lit the right tree row by the time the
    // double-click arrives, but set it again so the path does not depend on that ordering.
    m_viewport->set_on_display_sketch_activated([this](int feat) {
        if (feat < 0 || feat >= int(m_doc.features.size())) return;
        set_tree_selection(feat);
        on_edit_feature();
    });

    // F key (Prepare's Place on Face): the tool forwards it here when the Design viewport
    // has focus; we lay the selected body face on the bed. Returns false when no face is
    // selected so the key can fall through to the default handler.
    m_viewport->set_on_place_on_face([this]() { return place_on_face(); });

    // Clicking a solid cycles whole -> face -> edge. The tool draws the cyan overlay for ALL
    // levels now (per-body, so other bodies stay untinted) — no whole-compound set_body_highlight.
    m_viewport->set_on_solid_selection_changed([this](int level, int body, int face, int edge) {
        // A pick that fell through the move gizmo (clicked off the arrows) exits move mode.
        if (m_viewport->moving_body()) m_viewport->clear_move_gizmo();
        // Remember which body + face/edge so Extrude / dress-up target the RIGHT body.
        m_sel_solid_body = (level >= 1) ? body : -1;
        m_sel_solid_face = (level == 2) ? face : -1;   // 4 = Vertex: a corner is not its face
        m_sel_solid_edge = (level == 3) ? edge : -1;
        m_sel_solid_vertex = (level == 4);
        // Keep the hit face even at whole-body level: the cycle's first click means "this body",
        // but the user pointed AT a face and a sketch should be able to use it. snaporca-3a2.
        m_pick_face_body = (level >= 1) ? body : -1;
        m_pick_face      = (level >= 1) ? face : -1;
        // Last pick wins: selecting a solid drops any stale committed-sketch loop selection.
        // Otherwise a leftover loop keeps `m_sel_sketch_region >= 0`, which blocks the face
        // push/pull branch in Extrude (`m_sel_solid_face >= 0 && m_sel_sketch_region < 0`) and
        // makes Extrude build a DETACHED new body from the last sketch instead of push/pulling
        // the face the user just clicked.
        if (level >= 1) { m_sel_sketch_region = -1; m_sel_sketch_feat = -1; }
        // Say what got picked. Without this the ONLY feedback is the viewport highlight, so a
        // pick that registers but draws faintly is indistinguishable from one that never
        // happened — which is precisely how this failure was reported and why it resisted
        // diagnosis. Cards that show their own labels still do. The label itself is written
        // ONCE, at the end of this handler — a second writer here only ever produced text that
        // the later one overwrote before a frame was drawn, and reading it as the live string
        // is how a vertex pick came to be "fixed" in a branch that never reaches the screen.
        // If the Fillet/Chamfer card is open, re-anchor (or drop) the radius arrow on the new pick
        // and rebuild the ghost — once an edge is picked the preview-only mode hides the base body.
        if (m_active == Tool::Dressup) { sync_dressup_target(); update_fillet_gizmo(); refresh_preview(); }
        // Boolean card open: the VIEWPORT is how you choose the two operands. Until now they
        // could only come from two combos — the one control the charter names for this tool
        // (e1p item 4), and the same pair snaporca-7xx caught silently resolving every row to
        // index 0. The highlight already flowed card -> viewport; this closes the loop the
        // other way. First pick is the target (kept), second is the tool (consumed); the
        // combos mirror both, so the typed half of L2 still works and still round-trips.
        // A body cannot be both operands — that boolean is a no-op — but the resolution is to
        // SWAP, never to refuse: the pick is the user pointing at a body, and it has to win.
        // Refusing it was measurably wrong. Both combos are pre-filled when the card opens
        // (target = body 0, tool = a different one), so the very first viewport pick usually
        // names a body the other slot already holds; ignoring it left the defaults in place and
        // the commit produced body0 - body1 (75000 mm3) when the picks asked for body1 - body0
        // (43000 mm3). Swapping keeps the pair distinct AND honours the click.
        if (m_active == Tool::Boolean && m_sel_solid_body >= 0) {
            ComboBox* dst   = (m_bool_next_slot == 0) ? m_bool_target : m_bool_tool;
            ComboBox* other = (m_bool_next_slot == 0) ? m_bool_tool   : m_bool_target;
            const int row   = m_sel_solid_body;          // selection index == body index
            if (dst != nullptr && row < int(dst->GetCount())) {
                const int prev = dst->GetSelection();
                dst->SetSelection(row);
                if (other != nullptr && other->GetSelection() == row && prev >= 0 && prev != row)
                    other->SetSelection(prev);            // displaced operand takes the old slot
                m_bool_next_slot ^= 1;
                refresh_preview();                        // re-tints the operands + rebuilds the ghost
            }
        }
        // Mirror card open: the body you point at is the body that gets mirrored. Same one-way
        // flow Boolean had (snaporca-310o) and the same fix — the combo stays as the typed half.
        // Only one operand here, so there is no slot to alternate and no swap to do.
        if (m_active == Tool::Mirror && m_sel_solid_body >= 0 && m_mirror_body != nullptr &&
            m_sel_solid_body < int(m_mirror_body->GetCount())) {
            m_mirror_body->SetSelection(m_sel_solid_body);   // selection index == body index
            refresh_preview();
        }
        // If the Shell card is open, a face pick chooses the open face: update the label + gizmo
        // + ghost so the hollow updates live.
        if (m_active == Tool::Shell) {
            m_shell_face_label->SetLabel(m_sel_solid_face >= 0
                ? wxString::Format(_L("Face %d"), m_sel_solid_face)
                : _L("(all faces — closed hollow)"));
            refresh_preview();   // rebuilds the shell ghost + re-anchors the thickness gizmo
        }
        // Thicken card open: the face pick IS the feature's input, and until now nothing here
        // wrote its label — the value reached m_sel_solid_face and Confirm worked, but the card
        // read "(pick a solid face)" however many faces you had picked. Invisible because the
        // ghost did update, so the card looked wrong while behaving right.
        if (m_active == Tool::Thicken) {
            m_thicken_face_label->SetLabel(m_sel_solid_face >= 0
                ? wxString::Format(_L("Face %d"), m_sel_solid_face)
                : _L("(pick a solid face)"));
            refresh_preview();
        }
        // Draft card open: a face pick chooses the face to taper; update label + ghost live.
        if (m_active == Tool::Draft) {
            m_draft_face_label->SetLabel(m_sel_solid_face >= 0
                ? wxString::Format(_L("Face %d"), m_sel_solid_face)
                : _L("(pick a side face)"));
            refresh_preview();
        }
        // Hole card open: clicking a solid FACE re-targets the hole ONTO that face (Orca-style),
        // so the hole lives on the object's face — not on a stale dropdown/datum plane. Uses the
        // face under the cursor from the FIRST click (handle_solid_click reports it even at the
        // Whole level), so no whole->face cycle is needed.
        if (m_active == Tool::Hole && face >= 0 && body >= 0 && body < int(m_doc.bodies.size())) {
            const TopoDS_Face fc = GeometryEngine::face_by_index(m_doc.bodies[body].shape, face);
            if (!fc.IsNull()) {
                m_hole_face_plane = face_plane_inward(fc);
                m_hole_on_face    = true;
                m_hole_face_body  = body;
                set_hole_target_label(face);
                m_hole_has_bounds = GeometryEngine::face_plane_bounds(
                    fc, m_hole_face_plane.origin, m_hole_face_plane.x_axis,
                    m_hole_face_plane.y_axis, m_hole_umin, m_hole_umax, m_hole_vmin, m_hole_vmax);
                if (m_hole_x) m_hole_x->SetValue(0.0);   // centre of the picked face
                if (m_hole_y) m_hole_y->SetValue(0.0);
                if (m_hole_plane) m_hole_plane->SetSelection(index_from_plane(m_hole_face_plane));
                refresh_preview();   // re-place the gizmo + ghost on the new face
            }
        }
        // Thread card open: clicking a cylindrical face or circular edge re-derives the thread.
        if (m_active == Tool::Thread && body >= 0 && body < int(m_doc.bodies.size())) {
            const TopoDS_Shape& shape = m_doc.bodies[body].shape;
            GeometryEngine::CylinderFace cf;
            if (face >= 0)
                cf = GeometryEngine::cylinder_of_face(GeometryEngine::face_by_index(shape, face));
            const bool from_face = cf.ok;   // which of the two the cylinder actually came from
            if (!cf.ok && edge >= 0)
                cf = GeometryEngine::circle_of_edge(GeometryEngine::edge_by_index(shape, edge));
            if (cf.ok) {
                SketchPlane p; p.origin = cf.base; p.normal = cf.axis;
                const Vec3d ref = std::abs(cf.axis.z()) < 0.9 ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
                p.x_axis = ref.cross(cf.axis).normalized();
                p.y_axis = cf.axis.cross(p.x_axis).normalized();
                m_thread_face_plane = p;
                m_thread_on_face    = true;
                m_thread_face_body  = m_sel_solid_body;
                set_thread_target_label(from_face ? face : -1, from_face ? -1 : edge);
                infer_thread_spec(2.0 * cf.radius);   // M diameter + pitch + depth from the cylinder
                if (m_thread_height && cf.height > 1e-6) m_thread_height->SetValue(cf.height);
                if (m_thread_internal) m_thread_internal->SetValue(cf.internal);
                refresh_preview();
            }
        }
        // Plane tool with a pick armed: capture the right kind of reference (face for Face A/B,
        // edge for Edge A/B). If the click wasn't the right kind, stay armed so the user retries.
        if (m_active == Tool::Plane && m_plane_pick != PlanePick::None) {
            bool got = false;
            switch (m_plane_pick) {
            case PlanePick::FaceA: if (m_sel_solid_face >= 0) { m_pl_faceA_body = m_sel_solid_body; m_pl_faceA = m_sel_solid_face; got = true; } break;
            case PlanePick::FaceB: if (m_sel_solid_face >= 0) { m_pl_faceB_body = m_sel_solid_body; m_pl_faceB = m_sel_solid_face; got = true; } break;
            case PlanePick::EdgeA: if (m_sel_solid_edge >= 0) { m_pl_edgeA_body = m_sel_solid_body; m_pl_edgeA = m_sel_solid_edge; got = true; } break;
            case PlanePick::EdgeB: if (m_sel_solid_edge >= 0) { m_pl_edgeB_body = m_sel_solid_body; m_pl_edgeB = m_sel_solid_edge; got = true; } break;
            default: break;
            }
            if (got) { m_plane_pick = PlanePick::None; refresh_plane_labels(); }
            if (got && m_viewport) m_viewport->set_escalate_on_repick(true);
        }
        // Axis tool with a pick armed: face or edge.
        if (m_active == Tool::Axis && m_axis_pick != AxisPick::None) {
            bool got = false;
            switch (m_axis_pick) {
            case AxisPick::Face: if (m_sel_solid_face >= 0) { m_ax_face_body = m_sel_solid_body; m_ax_face = m_sel_solid_face; got = true; } break;
            case AxisPick::Edge: if (m_sel_solid_edge >= 0) { m_ax_face_body = m_sel_solid_body; m_ax_edge = m_sel_solid_edge; got = true; } break;
            default: break;
            }
            if (got) { m_axis_pick = AxisPick::None; refresh_axis_labels(); }
            if (got && m_viewport) m_viewport->set_escalate_on_repick(true);
        }
        // CoordSys tool with a pick armed: face or edge.
        if (m_active == Tool::CoordSys && m_coordsys_pick != CoordSysPick::None) {
            bool got = false;
            switch (m_coordsys_pick) {
            case CoordSysPick::Face: if (m_sel_solid_face >= 0) { m_cs_face_body = m_sel_solid_body; m_cs_face = m_sel_solid_face; got = true; } break;
            case CoordSysPick::Edge: if (m_sel_solid_edge >= 0) { m_cs_face_body = m_sel_solid_body; m_cs_edge = m_sel_solid_edge; got = true; } break;
            default: break;
            }
            if (got) {
                // A picked face or edge is only meaningful to FaceAndDirection. Left on the
                // default Point (world), datum_frame ignores the pick entirely and resolves the
                // connector to coordsys_point — which is (0,0,0) unless the user typed
                // otherwise. Two such connectors then share one frame, so a mate between them
                // computes an IDENTITY transform: the feature commits, the recompute succeeds,
                // and nothing moves. Picking a face IS the choice of a face-based frame, so make
                // the type follow the pick rather than asking for it twice.
                if (m_coordsys_type && m_coordsys_type->GetSelection() != (int)CoordSysType::FaceAndDirection)
                    m_coordsys_type->SetSelection((int)CoordSysType::FaceAndDirection);
                m_coordsys_pick = CoordSysPick::None;
                refresh_coordsys_labels();
            }
            if (got && m_viewport) m_viewport->set_escalate_on_repick(true);
        }
        m_status->SetForegroundColour(wxNullColour);
        const int nb = int(m_doc.bodies.size());
        const wxString bodytag = (nb > 1) ? wxString::Format(_L("Body %d "), body + 1) : wxString();
        // Each sub-element line ends by naming the NEXT click (snaporca-gem). Escalation to the
        // whole body is a gesture nothing on screen would otherwise reveal, and the status line
        // is the only surface that can teach it at the moment it applies. It REPLACES the old
        // per-level verb hints ("right-click to push/pull it", "Fillet/Chamfer to dress it")
        // rather than joining them: the line is clipped at the panel edge past ~55 characters
        // (set_status's Wrap() does not take effect — snaporca-8cc), and those verbs are shown
        // with their icons in the offer anyway, while this gesture is shown nowhere else.
        // Both clauses fit now that the line is drawn over the viewport instead of squeezed
        // into the panel. Say "what applies to it", never "verbs" — that is this codebase's
        // word for a tool-offer entry, not a word the drawing office uses, and the plane and
        // bodies-list lines already say it the right way.
        set_status(level == 4 ? bodytag + _L("vertex selected — click again for the whole body")
                         : level == 1 ? bodytag + _L("selected (whole body) — right-click for what applies to it")
                         : level == 2 ? bodytag + wxString::Format(_L("face %d selected — right-click to push/pull it, or click again for the whole body"), face)
                         : level == 3 ? bodytag + wxString::Format(_L("edge %d selected — Fillet/Chamfer to dress it, or click again for the whole body"), edge)
                                      : _L("Nothing selected"));
        m_status->Refresh();
    });

    // Visual Extrude gizmo (C5b): dragging/editing the in-canvas depth arrow writes the
    // matching spin field and re-previews (which re-feeds the gizmo with the new depth).
    m_viewport->set_on_extrude_depth_changed([this](double depth, bool second) {
        // One arrow, three tools: Extrude owns the two-sided pair, the other two have a single
        // distance each. Routing here rather than arming three near-identical gizmos is the whole
        // reason these two tools got a handle at all — the arrow was already written.
        if (m_active == Tool::SurfaceExtrude) {
            if (m_surf_extrude_distance) m_surf_extrude_distance->SetValue(depth);
        } else if (m_active == Tool::Thicken) {
            if (m_thicken_thickness) m_thicken_thickness->SetValue(depth);
        } else if (m_active == Tool::Rib) {
            if (m_rib_depth) m_rib_depth->SetValue(depth);   // depth; thickness has its own in-plane pair
        } else if (m_active == Tool::SurfaceOffset) {
            if (m_surf_offset_distance) m_surf_offset_distance->SetValue(depth);
        } else if (m_active == Tool::ThickenSurface) {
            if (m_surf_thicken_thickness) m_surf_thicken_thickness->SetValue(depth);
        } else if (second) { if (m_distance2) m_distance2->SetValue(depth); }
        else               { if (m_distance)  m_distance->SetValue(depth); }
        refresh_preview();
    });

    // Datum-plane resize handles (C3): a handle drag reports the new u/v extent. Mirror it into the
    // Size spins and, when editing a committed datum, into the feature so the rendered rectangle
    // follows live. SetValue doesn't emit a command event, so no refresh_preview recursion.
    m_viewport->set_on_datum_size_changed([this](double u, double v) {
        if (m_plane_usize) m_plane_usize->SetValue(u);
        if (m_plane_vsize) m_plane_vsize->SetValue(v);
        if (m_edit_index >= 0 && m_edit_index < int(m_doc.features.size()) &&
            m_doc.features[m_edit_index].type == CadFeatureType::Plane) {
            m_doc.features[m_edit_index].plane_u_size = u;
            m_doc.features[m_edit_index].plane_v_size = v;
            refresh_datum_planes();   // committed datum rectangle follows the drag
        }
        m_viewport->request_repaint();
    });

    // Offset arrow drag: mirror the new offset into the spin + (when editing) the committed feature.
    m_viewport->set_on_datum_offset_changed([this](double off) {
        if (m_plane_offset) m_plane_offset->SetValue(off);
        if (m_edit_index >= 0 && m_edit_index < int(m_doc.features.size()) &&
            m_doc.features[m_edit_index].type == CadFeatureType::Plane) {
            m_doc.features[m_edit_index].plane_offset = off;
            refresh_datum_planes();
        }
        m_viewport->request_repaint();
    });

    // Helix handles: a drag reports the whole triple, since pitch and height are coupled
    // through the turn count and reading one without the others would show a stale curve.
    m_viewport->set_on_helix_changed([this](double radius, double pitch, double height) {
        if (m_helix_radius) m_helix_radius->SetValue(radius);
        if (m_helix_pitch)  m_helix_pitch->SetValue(pitch);
        if (m_helix_height) m_helix_height->SetValue(height);
        update_helix_gizmo();      // re-feed so the curve follows the drag
        m_viewport->request_repaint();
    });

    // Rib thickness handle: writes the spin and re-feeds the ghost, same as the depth arrow.
    m_viewport->set_on_rib_thickness_changed([this](double thickness) {
        if (m_rib_thickness) m_rib_thickness->SetValue(thickness);
        refresh_preview();   // Rib HAS a solid ghost, so the full preview path is correct here
    });

    // Clicking a ghost base plane sets the base graphically (replaces the dropdown). A base pick
    // drops any offset-from-face choice so the picked base plane wins, then re-resolves the preview.
    m_viewport->set_on_datum_base_picked([this](int base) {
        if (m_active == Tool::Plane) {
            // Plane tool open: the click sets the datum's base plane (replaces the dropdown).
            if (m_plane_base && base >= 0 && base < int(m_plane_base->GetCount()))
                m_plane_base->SetSelection(base);
            m_pl_faceA_body = m_pl_faceA = -1;
            refresh_plane_labels();
            refresh_preview();   // re-resolve the frame + move the gizmo/ghosts to the new base
        } else {
            // Clicking a reference plane in 3D IS how a sketch plane is chosen now. Record it and,
            // when a session is already live, re-plane it immediately: begin_sketch captures the
            // plane at first-tool-pick, so without this the entities would stay on the old plane
            // while the committed feature landed on the new one.
            if (base >= 0) {
                m_ref_plane = base;
                m_plane_picked = true;                 // chosen, not merely defaulted to
                m_pick_face = m_pick_face_body = -1;   // last pick wins: a plane beats a stale face
                if (m_viewport && m_viewport->is_sketching())
                    m_viewport->set_sketch_plane(plane_from_choice(m_ref_plane));
            }
            const char* nm = (base == 0) ? "XY" : (base == 1) ? "XZ" : (base == 2) ? "YZ" : "datum";
            m_status->SetForegroundColour(wxColour(120, 210, 120));
            // Both halves named the TOOLBAR, which no longer carries either button: the tools
            // moved to the offer. Name the gesture that actually works in each mode, and say
            // what a plain click does, since the two are easy to confuse on a plane.
            set_status(m_ui_mode == UiMode::Sketch
                ? wxString::Format(_L("%s plane selected — right-click for the drawing tools"), nm)
                : wxString::Format(_L("%s plane selected — right-click to sketch on it, "
                                      "or click an object to select it"), nm));
            m_status->Refresh();
        }
    });

    // Move-body gizmo (M5): each drag/edit reports the body's new translation. Store it as a
    // display-only per-body transform and re-feed the moved meshes (the OCCT shape is untouched,
    // so face/edge ids the dress-up ops target stay valid).
    m_viewport->set_on_body_move_changed([this](int body, const Transform3d& xform) {
        sync_body_xform();
        if (body < 0 || body >= int(m_body_xform.size())) return;
        m_body_xform[body] = xform;   // full move+rotate transform, baked into the mesh at Commit
        feed_bodies();   // rebuilds the transformed meshes in place + refreshes display + pick
        // When the move gizmo is serving the Transform card, mirror the drag into the card's
        // numeric fields (the card is the typed half, the gizmo the geometry-first half).
        if (body == m_xf_gizmo_body && m_active == Tool::Transform) {
            const Transform3d d = xform * m_xf_gizmo_base.inverse();
            const Vec3d       t = d.translation();
            if (m_xf_dx) m_xf_dx->SetValue(t.x());
            if (m_xf_dy) m_xf_dy->SetValue(t.y());
            if (m_xf_dz) m_xf_dz->SetValue(t.z());
            const Eigen::AngleAxisd aa(Eigen::Quaterniond(d.linear()).normalized());
            // The gizmo's rings are per-world-axis, so a ring drag yields an exactly axial
            // rotation and this is lossless for the normal interaction. A composed multi-ring
            // pose is not axial; the card can only express one axis, so report the dominant one
            // rather than refusing to answer.
            int ax = 0; double best = std::abs(aa.axis().x());
            if (std::abs(aa.axis().y()) > best) { ax = 1; best = std::abs(aa.axis().y()); }
            if (std::abs(aa.axis().z()) > best) { ax = 2; best = std::abs(aa.axis().z()); }
            const double sign = (aa.axis()[ax] < 0.0) ? -1.0 : 1.0;
            if (m_xf_axis)  m_xf_axis->SetSelection(ax);
            if (m_xf_angle) m_xf_angle->SetValue(sign * aa.angle() * 180.0 / M_PI);
        }
        const int nb = int(m_doc.bodies.size());
        const wxString tag = (nb > 1) ? wxString::Format(_L("Body %d "), body + 1) : wxString();
        const Vec3d t = xform.translation();
        m_status->SetForegroundColour(wxNullColour);
        set_status(tag + wxString::Format(_L("placed (%.1f, %.1f, %.1f) mm — drag arrows to move, rings to rotate"),
                                                  t.x(), t.y(), t.z()));
        m_status->Refresh();
    });

    // Fillet/Chamfer radius gizmo: dragging (or editing) the edge-anchored arrow writes the
    // Dress-up size and refreshes the ghost. SetValue is silent in wx, so refresh explicitly.
    m_viewport->set_on_fillet_radius_changed([this](double radius) {
        if (m_dressup_size) m_dressup_size->SetValue(radius);
        refresh_preview();   // rebuilds the candidate fillet ghost at the new radius
    });

    // Hole gizmo: dragging/editing the centre, diameter, or depth handle writes the four Hole-card
    // spins and refreshes the ghost. SetValue is silent in wx, so refresh explicitly.
    m_viewport->set_on_hole_changed([this](double x, double y, double diameter, double depth) {
        if (m_hole_x)        m_hole_x->SetValue(x);
        if (m_hole_y)        m_hole_y->SetValue(y);
        if (m_hole_diameter) m_hole_diameter->SetValue(diameter);
        if (m_hole_depth)    m_hole_depth->SetValue(depth);
        refresh_preview();   // rebuilds the candidate hole ghost at the new position/size
    });

    // Thread gizmo: dragging/editing the centre, radius, or length handle writes the Thread-card
    // spins and refreshes the ghost (SetValue is silent in wx).
    m_viewport->set_on_thread_changed([this](double x, double y, double radius, double height) {
        if (m_thread_x)      m_thread_x->SetValue(x);
        if (m_thread_y)      m_thread_y->SetValue(y);
        if (m_thread_radius) m_thread_radius->SetValue(2.0 * radius);   // gizmo reports radius; field = diameter
        if (m_thread_height) m_thread_height->SetValue(height);
        refresh_preview();
    });

    // Shell gizmo: dragging/editing the inward thickness arrow writes the Shell-card thickness
    // and refreshes the ghost (SetValue is silent in wx).
    m_viewport->set_on_shell_thickness_changed([this](double thickness) {
        if (m_shell_thickness) m_shell_thickness->SetValue(thickness);
        refresh_preview();
    });

    m_viewport->set_on_revolve_angle_changed([this](double angle) {
        if (m_revolve_angle) m_revolve_angle->SetValue(angle);
        refresh_preview();
    });

    m_viewport->set_on_draft_angle_changed([this](double angle) {
        if (m_draft_angle) m_draft_angle->SetValue(angle);
        refresh_preview();
    });

    // Cut gizmo: dragging the offset arrow writes the Cut-card offset and refreshes the ghost.
    m_viewport->set_on_cut_offset_changed([this](double v) {
        if (m_cut_offset) m_cut_offset->SetValue(v);
        refresh_preview();
    });

    m_viewport->set_on_pattern_changed([this](double value) {
        // Linear drag feeds spacing; circular drag feeds angle. The card knows which is live.
        if (m_pattern_type && m_pattern_type->GetSelection() == 1) {
            if (m_pattern_angle) m_pattern_angle->SetValue(value);
        } else if (m_pattern_spacing) {
            m_pattern_spacing->SetValue(value);
        }
        refresh_preview();
    });

    // Esc exits the active sketch tool: drop the live session, restore Feature mode +
    // the committed-sketch overlay (an in-progress draw is discarded). The tool's layered
    // request_exit only calls this once it's an idle Select session.
    m_viewport->set_on_sketch_exit([this]() {
        // While placing imported Text/SVG art, right-click = Confirm (keep the art) — the
        // Insert card is the explicit gate, this is the in-canvas shortcut to it.
        if (m_active == Tool::Insert) { finalize_insert(); return; }
        if (m_viewport) m_viewport->cancel_sketch();
        m_edit_index = -1;
        set_ui_mode(UiMode::Feature);
        sync_sketch_display();
        refresh_tree();
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("Tool exited"));
        m_status->Refresh();
    });

    // Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y) from the viewport → feature-history undo/redo.
    m_viewport->set_on_undo_redo([this](bool redo) { do_undo_redo(redo); });

    // Esc = the unified Cancel everywhere. Feature cards had no key exit (only the button);
    // CHAR_HOOK on the panel catches Esc from the card or viewport and routes to tool_cancel.
    // Sketch/Constrain keep the viewport's per-gesture Esc (abort the current point first),
    // so we only intercept Esc here when a feature/insert card is the thing to dismiss.
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        const int  key  = e.GetKeyCode();
        const bool ctrl = e.ControlDown() || e.CmdDown();
        const bool sketching = (m_ui_mode == UiMode::Sketch) && m_viewport && m_viewport->is_sketching();
        // Which key MAP applies is a question about the MODE, not about whether a session is
        // already running. Gating the sketch map on is_sketching() made it unreachable by
        // keyboard: is_sketching() only turns true inside select_tool()'s begin_sketch(), and
        // select_tool() is what the sketch keys call — so the first letter after entering
        // sketch mode fell through to the feature map, matched nothing (feature keys are
        // Shift+letter), and did nothing. The mouse worked only because the toolbar flyout
        // reaches select_tool() directly. That is why all 17 keys read as dead. snaporca-0ud.
        const bool sketch_mode = (m_ui_mode == UiMode::Sketch);
        // Never steal editing keys from a focused text field or an open in-canvas value field —
        // Delete/Ctrl+Z there must edit the text, not the model.
        const bool in_text = (dynamic_cast<wxTextCtrl*>(wxWindow::FindFocus()) != nullptr)
                             || (m_viewport && m_viewport->inline_busy());

        if (getenv("SNAPORCA_KEYTRACE")) {
            wxWindow* fw = wxWindow::FindFocus();
            fprintf(stderr, "[KEYTRACE] key=%d ui_mode=%d is_sketching=%d in_text=%d inline_busy=%d focus=%s\n",
                    key, int(m_ui_mode), (m_viewport && m_viewport->is_sketching()) ? 1 : 0, in_text ? 1 : 0,
                    (m_viewport && m_viewport->inline_busy()) ? 1 : 0,
                    fw ? (const char*) fw->GetClassInfo()->GetClassName() : "(none)");
            fflush(stderr);
        }
        const bool dismissable = m_active != Tool::None || (m_viewport && m_viewport->moving_body());
        if (key == WXK_ESCAPE && dismissable) { tool_cancel(); return; }

        // The offer from the keyboard (charter 4.1): the Menu key, or Shift+F10 for keyboards that
        // do not have one. Same menu the right-click opens — show_offer_menu already decides which
        // half of the map applies via sketch_map_applies(), so nothing about the content is decided
        // here. With no press to anchor it, offer_anchor() puts it on the viewport.
        // WXK_MENU is the GTK code for the physical Menu key (GDK_KEY_Menu); wxMSW instead sends
        // WXK_WINDOWS_MENU for VK_APPS and maps Alt to WXK_ALT, so accepting both is safe
        // everywhere. CallAfter because the menu is modal: let the key event finish dispatching
        // before a nested loop takes the queue, the same reason the Sketch entry does it.
        if (!in_text && !ctrl
            && (key == WXK_MENU || key == WXK_WINDOWS_MENU || (key == WXK_F10 && e.ShiftDown()))) {
            CallAfter([this] { show_offer_menu(offer_anchor()); });
            return;
        }

        // Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y — undo/redo handled here (not only in the GL canvas) so
        // it works even when the canvas lost keyboard focus. In a sketch, undo drops the last entity.
        if (!in_text && ctrl && (key == 'Z' || key == 'z' || key == WXK_CONTROL_Z ||
                                 key == 'Y' || key == 'y' || key == WXK_CONTROL_Y)) {
            const bool redo = (key == 'Y' || key == 'y' || key == WXK_CONTROL_Y) || e.ShiftDown();
            if (sketching) { if (!redo) m_viewport->undo_last_sketch_entity(); }
            else           { do_undo_redo(redo); }
            return;
        }
        // Delete — the selected sketch entities (or the last drawn one if none is selected), or the
        // selected feature in Feature mode. Focus-independent, same reason as undo above.
        if (!in_text && key == WXK_DELETE) {
            if (sketching) { m_viewport->delete_selected_or_last_sketch_entity(); return; }
            if (m_ui_mode == UiMode::Feature && m_active == Tool::None
                && tree_selection() != wxNOT_FOUND) { on_delete_feature(); return; }
        }
        // Section view controls while it is on (Alt+Wheel is unreliable under remote desktops / is
        // grabbed by GLCanvas3D, so the keyboard drives it): PageUp/PageDown move the plane, F flips
        // which half is kept (so you can inspect the opposite part).
        if (!in_text && !sketch_mode && m_section_on) {
            if (key == WXK_PAGEUP || key == WXK_PAGEDOWN) {
                m_section_cut_z += (key == WXK_PAGEUP ? 2.0 : -2.0);
                if (m_viewport) m_viewport->set_section_plane(true, m_section_cut_z, m_section_upper);
                return;
            }
            if (key == 'F' || key == 'f') { flip_section_view(); return; }
        }
        // Tool shortcuts (Onshape-style). While a sketch is open, single letters drive sketch
        // tools; otherwise Shift+letter drives feature tools and single letters drive view
        // toggles / section. Ctrl-combos and focused text fields are never intercepted.
        // A SKETCH SHORTCUT MAY LEAVE AN OPEN VALUE FIELD. in_text is true while an inline
        // dimension editor is up, and drawing a rectangle opens one automatically for its
        // Width/Height — so after a rectangle every single-letter tool key was swallowed and the
        // sketch could not be continued at all. A value field holds a NUMBER; a letter is never
        // meant for it, so a letter that names a sketch tool is unambiguously a tool switch.
        // set_tool() commits the field on the way through, so the typed value is kept.
        // Deliberately narrow: sketch mode only, only keys that are actually bound, and only the
        // in-canvas field — a focused wxTextCtrl elsewhere (the variables table, a card spin)
        // still keeps every key.
        const bool inline_field_only = (dynamic_cast<wxTextCtrl*>(wxWindow::FindFocus()) == nullptr)
                                       && m_viewport && m_viewport->inline_busy();
        if (in_text && inline_field_only && sketch_mode && !ctrl) {
            const int up2 = (key >= 'a' && key <= 'z') ? key - 'a' + 'A' : key;
            auto it2 = m_keys_sketch.find(up2);
            if (it2 != m_keys_sketch.end()) { it2->second(); return; }
        }
        if (!in_text && !ctrl) {
            const int up = (key >= 'a' && key <= 'z') ? key - 'a' + 'A' : key;   // normalise case
            if (sketch_mode) {
                auto it = m_keys_sketch.find(up);
                if (it != m_keys_sketch.end()) { it->second(); return; }
            } else {
                auto it = m_keys_feature.find(up | (e.ShiftDown() ? SC_SHIFT : 0));
                if (it != m_keys_feature.end()) { it->second(); return; }
            }
        }
        e.Skip();
    });

    // Right-click finishes the move gizmo in the viewport; mirror that on the panel so the
    // action bar (shown while moving) hides and the move state clears.
    m_viewport->set_on_move_exit([this]() { m_move_body = -1; show_move_card(false); update_action_bar(); });

    // The offer (§4.1): right-click the geometry, get the verbs that apply to it. Left-click
    // still only selects, so pointing at things stays quiet.
    m_viewport->set_on_context_menu([this](const wxPoint& p) { show_offer_menu(p); });

    // The Line tool's length and the Dimension tool's value are both entered in-canvas now
    // (live quote labels + the floating SketchInlineEditor), so the old docked-card
    // callbacks (on_segment_drawn / on_dimension_pick_complete) are no longer wired.

    // Imported-art bbox transform streams the live offset/scale back here; write them to
    // the feature and re-sync the overlay so the art tracks the drag.
    m_viewport->set_on_imported_transform([this](int feat, Vec2d off, double sx, double sy) {
        if (feat < 0 || feat >= int(m_doc.features.size())) return;
        CadFeature& f = m_doc.features[feat];
        if (f.imported_regions.empty()) return;
        f.import_offset  = off;
        f.import_scale_x = sx;
        f.import_scale_y = sy;
        m_doc.recompute();
        sync_sketch_display();
    });

    auto* vcol = new wxBoxSizer(wxVERTICAL);
    // The bottom 3D-navigator orb handles all view orientation, so no separate view buttons.
    // Fit view is a double-click on the viewport (the tool intercepts it -> zoom_to_volumes).
    vcol->Add(m_viewport, 1, wxEXPAND);

    // Onshape layout: top toolbar over [ slim left column | center viewport ].
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    body->Add(m_form, 0, wxEXPAND);
    body->Add(vcol,   1, wxEXPAND);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(m_toolbar, 0, wxEXPAND);
    outer->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND);
    outer->Add(body, 1, wxEXPAND);
    SetSizer(outer);

    // The atlas says a verb is wired; the registrations above say what it runs. Nothing checks
    // that the two agree, and a broken join is INVISIBLE — the row renders, is enabled, and does
    // nothing when picked. That defect has shipped three times (edit_feature and sk_move carrying
    // action:null, then a whole flyout family the moment its widget stopped being built), and it
    // cannot be seen by reading either side alone. Verify the join once, here, where every
    // registration is complete. Cheap: 86 map lookups, once per panel.
    {
        std::vector<std::string> dead;
        for (int i = 0; i < kOfferVerbCount; ++i) {
            const OfferVerb& v = kOfferVerbs[i];
            if (v.action == nullptr || *v.action == '\0')
                continue;               // kernel support, no GUI path — the row shows disabled
            const std::string a(v.action);
            bool ok = false;
            if (a.rfind("key:", 0) == 0) {   // resolved exactly as run_offer_action() resolves it
                const std::string k = a.substr(4);
                if (k.size() >= 3 && k[0] == 'S' && k[1] == '+') {
                    auto it = m_keys_feature.find(int(k[2]) | SC_SHIFT);
                    ok = it != m_keys_feature.end() && bool(it->second);
                } else if (!k.empty()) {
                    auto it = m_keys_sketch.find(int(k[0]));
                    ok = it != m_keys_sketch.end() && bool(it->second);
                }
            } else {
                auto it = m_verb_actions.find(a);
                ok = it != m_verb_actions.end() && bool(it->second);
            }
            if (!ok)
                dead.emplace_back(std::string(v.id) + " -> " + a);
        }
        for (const std::string& d : dead)
            BOOST_LOG_TRIVIAL(error) << "Design offer: wired verb has no action: " << d;
        assert(dead.empty());   // debug builds stop here; release ships the log line
    }

    set_ui_mode(UiMode::Feature);
}

void DesignPanel::set_active_tool_btn(ScalableButton* b)
{
    // Onshape-style: the active tool's button gets the Orca accent (teal); the
    // rest revert to the ribbon surface. nullptr clears the whole strip.
    m_active_tool_btn = b;
    const wxColour bg = dp_ribbon_bg(), teal(0x00, 0x96, 0x88);
    for (auto* btn : m_tool_btns) {
        if (btn == nullptr) continue;
        btn->SetBackgroundColour(btn == b ? teal : bg);
        btn->Refresh();
    }
}

// Paint the DoF line. Split out of the solve callback so entering Constrain can re-apply the
// last known state — see m_dof_last.
void DesignPanel::apply_dof_status(int dof, bool ok, bool has_constraints)
{
    if (!m_dof_status) return;
    if (!has_constraints) {
        m_dof_status->SetLabel(wxString());
    } else if (!ok) {
        m_dof_status->SetForegroundColour(wxColour(235, 80, 80));
        m_dof_status->SetLabel(_L("✗ Conflicting constraints"));
    } else if (dof == 0) {
        m_dof_status->SetForegroundColour(wxColour(80, 200, 110));
        m_dof_status->SetLabel(_L("✓ Fully constrained"));
    } else if (dof > 0) {
        m_dof_status->SetForegroundColour(dp_ctl_text());
        m_dof_status->SetLabel(wxString::Format(_L("%d degrees of freedom"), dof));
    } else {
        m_dof_status->SetLabel(wxString());
    }
    m_dof_status->Show(!m_dof_status->GetLabel().IsEmpty());
    m_dof_status->Refresh();
    update_cards_frame(); m_form->Layout();
}

void DesignPanel::set_ui_mode(UiMode m)
{
    m_ui_mode = m;
    if (m != UiMode::Sketch) m_sketch_on.clear();   // no stale "on the picked face" on the next hint
    // The DoF readout describes a SKETCH's constraint state, so it means nothing back in Feature
    // mode — where it nonetheless stayed on screen after every Confirm, Cancel and Escape
    // (snaporca-752). Cleared here rather than at those three exits because this is the one place
    // all of them pass through, and a fourth exit added later would otherwise reintroduce it.
    // Constrain mode keeps it: that is where the number is the whole point.
    if (m == UiMode::Feature && m_dof_status != nullptr) {
        m_dof_status->SetLabel(wxString());
        m_dof_status->Show(false);
    }
    // Constrain is where the number is the whole point, and clearing it on the way out of
    // Sketch left it blank on the way in — no solve fires merely because the mode changed.
    if (m == UiMode::Constrain)
        apply_dof_status(m_dof_last, m_dof_last_ok, m_dof_last_has);
    wxSizer* s = m_toolbar->GetSizer();
    s->Show(m_tb_feature,   m == UiMode::Feature,   true);
    s->Show(m_tb_sketch,    m == UiMode::Sketch,    true);
    s->Show(m_tb_constrain, m == UiMode::Constrain, true);
    m_toolbar->Layout();
    m_toolbar->FitInside();   // refresh the horizontal scroll range for the new group widths
    set_active_tool_btn(nullptr);   // no tool selected right after a mode switch
    // Phase 3: the docked Sketch card (plane/orientation) shows for the whole Sketch
    // session and hides on Finish/Constrain.
    if (m_box_sketch_session != nullptr && m_form != nullptr && m_cards->GetSizer() != nullptr) {
        if (m == UiMode::Sketch && m_hdr_sketch_session != nullptr)
            m_hdr_sketch_session->SetLabel(wxString::Format(_L("Sketch %d"), m_feature_counter + 1));
        m_cards->GetSizer()->Show(m_box_sketch_session, m == UiMode::Sketch, true);
        update_cards_frame(); m_form->Layout();
        m_form->FitInside();
    }
    // Constraint-manager card follows Constrain mode; rebuilt from the active feature.
    if (m_box_constraints != nullptr && m_form != nullptr && m_cards->GetSizer() != nullptr) {
        if (m == UiMode::Constrain)
            rebuild_constraint_list();
        else
            m_cards->GetSizer()->Show(m_box_constraints, false, true);
        update_cards_frame(); m_form->Layout();
        m_form->FitInside();
    }
    update_action_bar();   // Sketch/Constrain modes show the unified ✓/✗; Feature idle hides it
    // The origin planes follow the mode: entering Sketch offers them even when a body exists,
    // leaving it takes them back. Without this they would only refresh on the next tree
    // rebuild, which is not an event that happens when you merely press Sketch.
    update_reference_planes();
}

void DesignPanel::on_shape_changed()
{
    bool rect = (m_shape->GetSelection() == 0);
    m_width->Enable(rect);
    m_height->Enable(rect);
    m_radius->Enable(!rect);
}

void DesignPanel::set_status_ok()
{
    set_status(wxString::Format(_L("OK — %zu triangles"),
                                        m_doc.display_mesh.its.indices.size()));
    if (m_viewport != nullptr) {
        m_viewport->clear_move_gizmo();   // a recompute invalidates the gizmo's body centroid
        rebuild_disp_meshes();            // apply per-body Move transforms to the display/pick meshes
        // Point the solid-pick at the fresh body + TRANSFORMED pick mesh (stable address) + the
        // per-body xform vector (for edge sampling). Resets the whole/face/edge selection, whose
        // ids invalidate on every recompute. Null body is handled inside.
        m_viewport->set_solid_pick(&m_doc.bodies, &m_disp_pick_mesh,
                                   &m_doc.display_tri_face, &m_doc.display_tri_body,
                                   &m_body_visible, &m_body_xform);
        feed_bodies();
    }
    sync_sketch_display();
}

// Draw every committed sketch that no enabled Extrude consumes, so a sketch stays
// visible (as a translucent face + outline) when it is not part of the solid — e.g.
// after its Extrude is removed, or right after Finish.
void DesignPanel::sync_sketch_display()
{
    if (m_viewport == nullptr) return;
    const int n = int(m_doc.features.size());
    std::vector<bool> consumed(n, false);
    // Per-loop extrudes (sketch_ref < 0) carry a verbatim copy of the one loop they
    // consumed; collect those so that loop is hidden from its source sketch overlay.
    std::vector<std::vector<SketchEntity>> consumed_loops;
    for (const CadFeature& f : m_doc.features) {
        if (f.type != CadFeatureType::Extrude || !f.enabled) continue;
        if (f.sketch_ref >= 0 && f.sketch_ref < n)        consumed[f.sketch_ref] = true;
        else if (f.sketch_ref < 0 && !f.entities.empty()) consumed_loops.push_back(f.entities);
    }
    // Two loops match when their entities are the same geometry in the same order — the
    // per-loop extrude stored a verbatim copy, so this is an exact comparison.
    auto same_loop = [](const std::vector<SketchEntity>& a, const std::vector<SketchEntity>& b) {
        if (a.size() != b.size() || a.empty()) return false;
        auto eq = [](const Vec2d& u, const Vec2d& v) { return (u - v).squaredNorm() < 1e-10; };
        for (size_t k = 0; k < a.size(); ++k) {
            const SketchEntity& x = a[k]; const SketchEntity& y = b[k];
            if (x.type != y.type || !eq(x.p0, y.p0) || !eq(x.p1, y.p1) ||
                !eq(x.center, y.center) || std::abs(x.radius - y.radius) > 1e-7) return false;
        }
        return true;
    };

    std::vector<DesignSketchTool::DisplaySketch> ds;
    for (int i = 0; i < n; ++i) {
        const CadFeature& f = m_doc.features[i];
        if (f.type != CadFeatureType::Sketch || consumed[i] || !f.enabled)
            continue;
        if (!f.entities.empty()) {
            if (consumed_loops.empty()) {
                ds.push_back({ f.entities, f.plane, i });
            } else {
                // Drop the entities of any loop already extruded; keep the rest (other
                // loops + non-loop entities) so they stay visible and selectable.
                std::vector<char> drop(f.entities.size(), 0);
                for (const std::vector<int>& loop : m_viewport->region_entity_indices_with_holes(f.entities)) {
                    std::vector<SketchEntity> es;
                    for (int ei : loop)
                        if (ei >= 0 && ei < int(f.entities.size())) es.push_back(f.entities[ei]);
                    bool gone = false;
                    for (const std::vector<SketchEntity>& c : consumed_loops)
                        if (same_loop(es, c)) { gone = true; break; }
                    if (gone)
                        for (int ei : loop)
                            if (ei >= 0 && ei < int(drop.size())) drop[ei] = 1;
                }
                std::vector<SketchEntity> shown;
                for (int ei = 0; ei < int(f.entities.size()); ++ei)
                    if (!drop[ei]) shown.push_back(f.entities[ei]);
                if (!shown.empty()) ds.push_back({ std::move(shown), f.plane, i });
            }
        } else if (!f.imported_regions.empty()) {
            // Imported art (Text/SVG) carries no solver entities; synthesize
            // closed line loops from each region contour so it shows as an
            // outline overlay (display only — never stored on the feature).
            // Apply the feature's placement transform so the overlay tracks
            // moves / scales.
            const auto regions = transform_regions(f.imported_regions, f.import_offset,
                                                    f.import_scale_x, f.import_scale_y);
            std::vector<SketchEntity> lines;
            for (const auto& region : regions)
                for (const auto& contour : region) {
                    const int m = int(contour.size());
                    for (int k = 0; k < m; ++k) {
                        SketchEntity e;
                        e.type = SketchEntity::Type::Line;
                        e.p0   = contour[k];
                        e.p1   = contour[(k + 1) % m];
                        lines.push_back(e);
                    }
                }
            if (!lines.empty())
                ds.push_back({ std::move(lines), f.plane, i });
        }
    }
    m_viewport->set_display_sketches(std::move(ds));
}

void DesignPanel::on_add_text()
{
    wxTextEntryDialog dlg(this, _L("Text to insert:"), _L("Text"), wxEmptyString);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const wxString text = dlg.GetValue();
    if (text.empty())
        return;
    const std::string utf8(text.ToUTF8().data());
    // Insert at a default height; resize in-canvas via the bbox handles (Move/Scale).
    add_imported_sketch(text_to_regions(utf8, 10.0), _L("Text"));
}

void DesignPanel::on_import_svg()
{
    wxFileDialog dlg(this, _L("Import SVG"), wxEmptyString, wxEmptyString,
                     "SVG files (*.svg)|*.svg|All files|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string path(dlg.GetPath().ToUTF8().data());
    // Import at 1:1; resize in-canvas via the bbox handles (Move/Scale).
    add_imported_sketch(svg_to_regions(path, 1.0), _L("SVG"));
}

// Run a long CAD computation off the UI thread. A big STEP costs tens of seconds in OCCT
// (parse + tessellate); running it inline froze the whole window — the compositor marked the
// app unresponsive and nothing repainted. The dialog is app-modal, so the document cannot be
// touched while the worker owns it. Exceptions must not escape the worker: `work` is expected
// to swallow them (OCCT throws Standard_Failure, which is not a std::exception).
static void run_off_ui_thread(wxWindow* parent, const wxString& message, const std::function<void()>& work)
{
    std::atomic<bool> done{false};
    std::thread worker([&work, &done]() {
        work();
        done.store(true, std::memory_order_release);
    });

    // Input stays blocked for the whole operation: the worker owns the document, so nothing
    // in the UI may mutate it meanwhile. The dialog only appears if the work is actually slow —
    // a fillet on a small body finishes in milliseconds and must not flash a dialog.
    wxWindowDisabler disabler;
    std::unique_ptr<wxProgressDialog> dlg;
    int elapsed_ms = 0;
    while (!done.load(std::memory_order_acquire)) {
        if (dlg == nullptr && elapsed_ms >= 300)
            dlg = std::make_unique<wxProgressDialog>(_L("SnapOrca"), message, 100, parent,
                                                     wxPD_AUTO_HIDE | wxPD_SMOOTH);
        if (dlg != nullptr)
            dlg->Pulse();
        wxYield();               // keep the window painting instead of going unresponsive
        wxMilliSleep(30);
        elapsed_ms += 30;
    }
    worker.join();
}

// Rebuild the document off the UI thread. Every feature op (fillet, cut, shell, boolean, ...)
// goes through recompute(), and on a heavy imported solid that is seconds of OCCT work — inline
// it freezes the window. OCCT throws Standard_Failure, which is not a std::exception and would
// terminate the process if it escaped the worker, so both are caught here.
// Keep the Model's copy of the recipe in step with the document.
//
// This used to be written in exactly ONE place — on_commit(), as a side effect of Commit to
// Plate — so a user who modelled for an hour and pressed Ctrl+S saved a project containing no
// feature history at all, and the app reported success (snaporca-vjk5). The 3MF exporter was
// never at fault: nothing had handed it a recipe.
//
// Every save path (Ctrl+S, Save As, autosave, crash recovery) reads model.cad_recipe, so
// keeping it current after each document change is what makes all of them correct at once,
// rather than teaching each one to ask the Design tab. Commit to Plate means "send this to the
// slicer"; making saving depend on it was the bug, not the cure.
//
// Cost is a serialization per recompute — tens of KB against an OCCT rebuild that just ran.
void DesignPanel::sync_recipe_to_model()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr) return;
    // An empty document CLEARS it, so a non-CAD project never carries a stale recipe.
    plater->model().cad_recipe = m_doc.features.empty() ? std::string() : m_doc.serialize_recipe();
}

bool DesignPanel::recompute_guarded(const wxString& message)
{
    bool ok = false;
    run_off_ui_thread(this, message, [this, &ok]() {
        try {
            ok = m_doc.recompute();
        } catch (const Standard_Failure& e) {
            const char* what = e.GetMessageString();
            m_doc.error = (what != nullptr && *what != '\0') ? what : "OCCT failure";
            ok = false;
        } catch (const std::exception& e) {
            m_doc.error = e.what();
            ok = false;
        }
    });
    // Only on success: a failed recompute leaves the document mid-edit, and persisting that
    // would save a model the user never had.
    if (ok) sync_recipe_to_model();
    return ok;
}

void DesignPanel::on_import_step()
{
    wxFileDialog dlg(this, _L("Import STEP"), wxEmptyString, wxEmptyString,
                     "STEP files (*.step;*.stp)|*.step;*.stp|All files|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string path(dlg.GetPath().ToUTF8().data());
    std::string err;
    // Keep the OCCT B-rep (don't mesh it like the slicer importer): each top-level solid
    // becomes a coexisting CadBody, fully editable by the on-face/edge feature tools.
    std::vector<TopoDS_Shape> solids;
    run_off_ui_thread(this, _L("Reading STEP…"), [&]() {
        try {
            solids = GeometryEngine::read_step_solids(path, err);
        } catch (const Standard_Failure& e) {
            const char* what = e.GetMessageString();
            err = (what != nullptr && *what != '\0') ? what : "OCCT failure";
        } catch (const std::exception& e) {
            err = e.what();
        }
    });
    if (solids.empty()) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(err.empty() ? _L("No solids found in STEP")
                                       : (_L("STEP import failed: ") + wxString::FromUTF8(err)));
        m_status->Refresh();
        return;
    }
    m_doc.checkpoint();   // undo boundary: importing STEP solids
    for (const TopoDS_Shape& s : solids) {
        m_feature_counter++;
        CadFeature f;
        f.type           = CadFeatureType::Import;
        f.name           = std::string("STEP") + std::to_string(m_feature_counter);
        f.imported_solid = s;
        f.mode           = BooleanMode::New;   // each solid is its own coexisting body
        m_doc.features.push_back(f);
    }
    bool rebuilt = false;
    run_off_ui_thread(this, _L("Rebuilding model…"), [&]() {
        try {
            rebuilt = m_doc.recompute();
        } catch (const Standard_Failure& e) {
            const char* what = e.GetMessageString();
            m_doc.error = (what != nullptr && *what != '\0') ? what : "OCCT failure";
        } catch (const std::exception& e) {
            m_doc.error = e.what();
        }
    });
    if (!rebuilt) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("STEP import failed: ") + wxString::FromUTF8(m_doc.error));
        m_status->Refresh();
        return;
    }
    set_ui_mode(UiMode::Feature);   // imported solids live in the feature timeline
    refresh_tree();
    set_tree_selection(int(m_doc.features.size()) - 1);
    set_status_ok();                // canonical post-recompute viewport/pick/parts refresh
    m_status->SetForegroundColour(wxNullColour);
    set_status(wxString::Format(
        _L("Imported %d solid(s) — pick a face or edge, then Fillet / Cut / Shell to modify"),
        int(solids.size())));
    m_status->Refresh();
}

// Import a triangle mesh as a real B-rep body: the triangles are rebuilt into OCCT faces with
// shared topology, then coplanar neighbours are merged so the result has pickable CAD faces
// rather than one face per triangle. Lands in the same CadFeatureType::Import as a STEP, so
// every downstream feature tool (fillet / cut / shell / face-extrude) works on it unchanged.
void DesignPanel::on_import_mesh()
{
    wxFileDialog dlg(this, _L("Import mesh"), wxEmptyString, wxEmptyString,
                     "Mesh files (*.stl;*.obj)|*.stl;*.obj|All files|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string path(dlg.GetPath().ToUTF8().data());

    auto fail = [this](const wxString& msg) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(msg);
        m_status->Refresh();
    };

    // Load the triangles with the slicer's own readers — no new mesh dependency.
    TriangleMesh mesh;
    const std::string ext = boost::algorithm::to_lower_copy(
        boost::filesystem::path(path).extension().string());
    if (ext == ".stl") {
        if (!mesh.ReadSTLFile(path.c_str())) { fail(_L("Could not read the STL file")); return; }
    } else if (ext == ".obj") {
        ObjInfo     obj_info;
        std::string obj_err;
        if (!load_obj(path.c_str(), &mesh, obj_info, obj_err)) {
            fail(_L("Could not read the OBJ file: ") + wxString::FromUTF8(obj_err));
            return;
        }
    } else {
        fail(_L("Unsupported mesh format (STL and OBJ are supported)"));
        return;
    }
    if (mesh.its.indices.empty()) { fail(_L("The mesh contains no triangles")); return; }

    // One planar face per triangle before merging, so the cost is driven by the triangle count.
    // A dense organic scan has few coplanar neighbours to merge away and stays heavy afterwards;
    // warn rather than silently freezing the CAD kernel on every subsequent recompute.
    if (mesh.its.indices.size() > MESH_IMPORT_TRIANGLE_WARN) {
        const wxString q = wxString::Format(
            _L("This mesh has %d triangles. Every triangle becomes a B-rep face before coplanar "
               "merging, so importing it may take a long time and leave a body that is slow to "
               "edit. Decimating the mesh first is usually better.\n\nImport anyway?"),
            int(mesh.its.indices.size()));
        if (wxMessageBox(q, _L("Large mesh"), wxYES_NO | wxICON_WARNING, this) != wxYES)
            return;
    }

    wxBusyCursor busy;
    GeometryEngine::MeshBrepStats stats;
    TopoDS_Shape shape;
    try {
        shape = GeometryEngine::mesh_to_brep(mesh.its, MESH_IMPORT_TOLERANCE,
                                             MESH_IMPORT_MERGE_ANGLE_DEG, stats);
    } catch (const std::exception& e) {
        fail(_L("Mesh conversion failed: ") + wxString::FromUTF8(e.what()));
        return;
    } catch (const Standard_Failure& e) {   // OCCT throws outside std::exception
        fail(_L("Mesh conversion failed: ") + wxString::FromUTF8(
                 e.GetMessageString() ? e.GetMessageString() : "OCCT error"));
        return;
    }
    if (shape.IsNull()) { fail(_L("Mesh conversion produced no geometry")); return; }

    m_doc.checkpoint();   // undo boundary: importing a mesh as a B-rep body
    m_feature_counter++;
    CadFeature f;
    f.type           = CadFeatureType::Import;
    f.name           = std::string("Mesh") + std::to_string(m_feature_counter);
    f.imported_solid = shape;
    f.mode           = BooleanMode::New;   // its own coexisting body, like a STEP solid
    m_doc.features.push_back(f);

    if (!recompute_guarded(_L("Rebuilding model…"))) {
        fail(_L("Mesh import failed: ") + wxString::FromUTF8(m_doc.error));
        return;
    }
    set_ui_mode(UiMode::Feature);
    refresh_tree();
    set_tree_selection(int(m_doc.features.size()) - 1);
    set_status_ok();

    // Report what the mesh actually was, never dress an open shell up as a solid: if it is not
    // watertight, say so and say why (boundary vs non-manifold edges) — that is a defect in the
    // source mesh the user needs to know about before they start cutting features into it.
    if (stats.is_solid) {
        m_status->SetForegroundColour(wxNullColour);
        set_status(wxString::Format(
            _L("Imported solid — %d triangles → %d faces, volume %.2f mm³. Pick a face or edge, "
               "then Fillet / Cut / Shell to modify"),
            stats.kept_tris, stats.faces_final, stats.volume));
    } else {
        m_status->SetForegroundColour(wxColour(220, 160, 60));   // warning, not an error
        set_status(wxString::Format(
            _L("Imported as an open shell (not watertight): %d boundary edge(s), %d non-manifold "
               "edge(s) — %d triangles → %d faces. The source mesh has holes or duplicated "
               "geometry; boolean features may fail on it"),
            stats.boundary_edges, stats.nonmanifold_edges, stats.kept_tris, stats.faces_final));
    }
    m_status->Refresh();
}

void DesignPanel::add_imported_sketch(
    const std::vector<std::vector<std::vector<Vec2d>>>& regions,
    const wxString& base_name)
{
    if (regions.empty()) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("No importable geometry found"));
        m_status->Refresh();
        return;
    }
    // DRAWING, not importing: if a sketch is open, the art belongs IN it. The outlines become
    // ordinary line entities, so they can be constrained, trimmed and extruded with everything
    // else on that plane. Committing a separate Sketch feature while the user is mid-sketch put
    // the text on its own plane-origin feature and left the sketch they were drawing untouched.
    if (m_viewport && m_viewport->is_sketching() && m_viewport->add_sketch_regions(regions)) {
        m_status->SetForegroundColour(wxNullColour);
        set_status(wxString::Format(_L("%s added to the sketch — Confirm to commit it"),
                                            base_name));
        m_status->Refresh();
        return;
    }
    m_doc.checkpoint();   // undo boundary: importing Text/SVG art
    m_feature_counter++;
    CadFeature f;
    f.type            = CadFeatureType::Sketch;
    f.name            = std::string(base_name.ToUTF8().data()) + std::to_string(m_feature_counter);
    f.imported_regions = regions;

    // #4: when a solid face is selected, drop the art ON that face, centred on it (ready to
    // engrave). Otherwise place it on the draw-plane dropdown at the plane origin (legacy).
    bool on_face = false;
    if (m_sel_solid_face >= 0 && m_sel_solid_body >= 0 && m_sel_solid_body < int(m_doc.bodies.size())) {
        const TopoDS_Face face =
            GeometryEngine::face_by_index(m_doc.bodies[m_sel_solid_body].shape, m_sel_solid_face);
        if (!face.IsNull()) {
            f.plane = SketchPlane::from_face(face);
            const Vec3d cw   = GeometryEngine::face_centroid_world(face);
            const Vec2d c_uv = f.plane.project(cw, f.plane.normal);   // face centre in plane (u,v)
            Vec2d lo(1e30, 1e30), hi(-1e30, -1e30);                   // bbox of the imported art
            for (const auto& reg : regions)
                for (const auto& loop : reg)
                    for (const Vec2d& p : loop) { lo = lo.cwiseMin(p); hi = hi.cwiseMax(p); }
            f.import_offset    = c_uv - 0.5 * (lo + hi);              // centre the art on the face
            f.import_on_face   = true;
            f.import_face_body = m_sel_solid_body;
            on_face = true;
        }
    }
    if (!on_face) {
        f.plane = plane_from_choice(m_ref_plane);
    }

    // Drop the live face selection (its body is now remembered on import_face_body): otherwise
    // the next Extrude would push/pull that face instead of extruding the placed art.
    m_sel_solid_face = m_sel_solid_edge = m_sel_solid_body = -1;

    m_doc.features.push_back(f);
    m_doc.recompute();   // a lone sketch yields an empty body; that is expected
    refresh_tree();
    const int newidx = int(m_doc.features.size()) - 1;
    set_tree_selection(newidx);   // select the new art
    sync_sketch_display();
    on_transform_imported(newidx);   // in-canvas place/size gizmo ON
    // The feature is provisional until the user explicitly Confirms (Onshape gate). The
    // Insert card carries Confirm/Cancel; Cancel undoes this insert.
    m_insert_feat = newidx;
    open_insert_card(base_name);
}

// Show the Insert Confirm/Cancel card while the imported art is being placed/sized.
void DesignPanel::open_insert_card(const wxString& base_name)
{
    m_active = Tool::Insert;
    if (m_hdr_insert) m_hdr_insert->SetLabel(base_name);
    wxSizer* s = m_cards->GetSizer();
    s->Show(m_box_insert, true, true);
    update_cards_frame(); m_form->Layout();
    m_form->FitInside();
    update_action_bar();   // surface the unified ✓/✗
    m_status->SetForegroundColour(wxNullColour);
    set_status(base_name + _L(" — drag to place/size, then Confirm"));
    m_status->Refresh();
}

// Confirm: keep the placed art and leave the placement gizmo. The feature is already in
// the timeline (added provisionally); we just tear down the transient tool/gizmo state.
void DesignPanel::finalize_insert()
{
    const int feat = m_insert_feat;
    m_insert_feat = -1;
    if (m_viewport) m_viewport->cancel_sketch();   // exit the TransformArt gizmo
    close_tool();                                  // hides the Insert card, clears m_active
    set_ui_mode(UiMode::Feature);                  // imported art lives in the feature timeline
    if (feat >= 0 && feat < int(m_doc.features.size())) set_tree_selection(feat);
    sync_sketch_display();
    refresh_tree();
    set_status_ok();
}

// Cancel: discard the provisional insert (undo restores the pre-insert feature list).
void DesignPanel::cancel_insert()
{
    m_insert_feat = -1;
    if (m_viewport) m_viewport->cancel_sketch();   // exit the TransformArt gizmo
    m_doc.undo();                                  // remove the just-added imported feature
    close_tool();
    set_ui_mode(UiMode::Feature);
    sync_sketch_display();
    refresh_tree();
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Insert cancelled"));
    m_status->Refresh();
}

void DesignPanel::on_transform_imported(int feat_idx)
{
    if (feat_idx < 0 || feat_idx >= int(m_doc.features.size()) || !m_viewport)
        return;
    const CadFeature& f = m_doc.features[feat_idx];
    if (f.imported_regions.empty())
        return;
    // In-canvas bbox handles (replaces the Move/Scale dialog): drag a corner to scale,
    // the centre to move. Values stream back via set_on_imported_transform.
    m_viewport->begin_imported_transform(feat_idx, f.imported_regions, f.plane,
                                         f.import_offset, f.import_scale_x, f.import_scale_y);
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Drag a corner to scale, the centre to move — right-click when done"));
    m_status->Refresh();
}

void DesignPanel::on_add_sketch()
{
    SketchShape shape = (m_shape->GetSelection() == 1) ? SketchShape::Circle
                                                        : SketchShape::Rectangle;
    wxString where;                                            // named for the status line
    SketchPlane plane = sketch_plane_from_selection(where);    // picked face, else the 3D plane click
    m_feature_counter++;
    m_doc.add_sketch(shape, plane, m_width->GetValue(), m_height->GetValue(),
                     m_radius->GetValue(), "Sketch" + std::to_string(m_feature_counter));
    m_doc.recompute();  // a lone sketch yields an empty body; that is expected
    m_status->SetForegroundColour(wxNullColour);
    set_status(wxString::Format(_L("Sketch added on %s — select it, then right-click to Extrude"), where));
    refresh_tree();
}

// Extrude should consume only the clicked loop when a specific region of the resolved
// sketch is selected and that loop actually has entities.
bool DesignPanel::extrude_uses_loop() const
{
    return m_viewport != nullptr
        && m_sel_sketch_region >= 0
        && m_extrude_sketch_ref >= 0
        && m_extrude_sketch_ref == m_sel_sketch_feat
        && m_extrude_sketch_ref < int(m_doc.features.size())
        && !m_viewport->selected_loop_entities().empty();
}

void DesignPanel::on_add_extrude()
{
    BooleanMode mode = static_cast<BooleanMode>(m_mode->GetSelection());  // New/Add/Cut/Intersect
    m_feature_counter++;
    const std::string name = "Extrude" + std::to_string(m_feature_counter);
    int idx = -1;
    if (m_extrude_face_src >= 0) {
        // Onshape face-extrude: the picked solid face is the profile (no sketch wire).
        idx = m_doc.add_extrude_face(m_extrude_face_src, m_distance->GetValue(), false, mode, name);
        m_extrude_face_src = -1;          // consume the face-profile selection
    } else if (extrude_uses_loop()) {
        // Extrude just the selected loop (its entity subset), leaving the source sketch's
        // other loops intact and still selectable.
        if (::getenv("SNAPORCA_PICK_TRACE"))
            std::fprintf(stderr, "[pick] on_add_extrude: feat=%d reg=%d ents=%zu\n",
                         m_extrude_sketch_ref, m_sel_sketch_region,
                         m_viewport->selected_loop_entities().size());
        idx = m_doc.add_extrude_entities(m_viewport->selected_loop_entities(),
                                         m_doc.features[m_extrude_sketch_ref].plane,
                                         m_distance->GetValue(), false, mode, name);
        m_sel_sketch_region = -1;        // consume the loop selection
        m_viewport->clear_loop_pick();   // drop the now-stale loop highlight
    } else {
        idx = m_doc.add_extrude(m_extrude_sketch_ref, m_distance->GetValue(), false, mode, name);
    }
    // Carry the Onshape end-condition / taper / flip / up-to-face onto the new feature so the
    // committed solid matches the preview (build_candidate sets the same fields).
    if (idx >= 0 && idx < int(m_doc.features.size())) {
        CadFeature& f = m_doc.features[idx];
        f.extrude_end = static_cast<ExtrudeEnd>(m_extrude_end->GetSelection());
        f.distance2   = m_distance2->GetValue();
        f.taper_deg   = m_taper->GetValue();
        f.flip        = m_flip->GetValue();
        f.up_to_face  = (f.extrude_end == ExtrudeEnd::UpToFace) ? m_sel_solid_face : -1;
        f.target_body = m_sel_solid_body;   // multi-body: act on the picked body (-1 = last)
        // On-face Text/SVG remembers its host body even after the face pick was cleared by
        // the placement recompute, so the engraving Cut hits the right solid.
        if (m_extrude_sketch_ref >= 0 && m_extrude_sketch_ref < int(m_doc.features.size())
            && m_doc.features[m_extrude_sketch_ref].import_on_face)
            f.target_body = m_doc.features[m_extrude_sketch_ref].import_face_body;
    }
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_dressup()
{
    if (m_doc.body.IsNull()) {
        set_status(_L("Add a solid (sketch + extrude) first"));
        return;
    }
    FaceGroup fg = static_cast<FaceGroup>(m_face_group->GetSelection()); // Top=0..All=3
    double    sz = m_dressup_size->GetValue();
    bool      fillet = (m_dressup_type->GetSelection() == 0);

    m_feature_counter++;
    // A click-selected solid edge targets THAT edge; otherwise dress the whole face-group.
    int didx = -1;
    if (m_sel_solid_edge >= 0) {
        if (fillet)
            didx = m_doc.add_fillet(sz, m_sel_solid_edge, "Fillet" + std::to_string(m_feature_counter));
        else
            didx = m_doc.add_chamfer(sz, m_sel_solid_edge, "Chamfer" + std::to_string(m_feature_counter));
    } else if (fillet)
        didx = m_doc.add_fillet(sz, fg, "Fillet" + std::to_string(m_feature_counter));
    else
        didx = m_doc.add_chamfer(sz, fg, "Chamfer" + std::to_string(m_feature_counter));
    // Dress the picked body (its face/edge ids are body-local). -1 = last body.
    if (didx >= 0 && didx < int(m_doc.features.size()))
        m_doc.features[didx].target_body = m_sel_solid_body;

    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

// Hole and Thread LATCH the geometry they were opened or picked on; Thicken / Shell / Draft read
// the live selection instead. Both models are right for what they are — a placement tool with its
// own plane state, versus an operation whose operand IS the selected face — and the latch is the
// kinder of the two now that a click on empty canvas clears the selection (snaporca-od0): a stray
// click costs a Thicken pick, and costs a Hole nothing. What was missing is that nothing in the
// Hole/Thread card NAMED the latched face, so after such a click the only words on screen were
// the viewport's "Nothing selected" — over a ghost still drawn on the face Confirm would drill.
// That reads as a contradiction and was filed as one (snaporca-200). The card now says what it
// holds, the way the other three cards already do. Pass -1 for "none, using the plane dropdown".
void DesignPanel::set_hole_target_label(int face)
{
    if (m_hole_target_label)
        m_hole_target_label->SetLabel(face >= 0 ? wxString::Format(_L("Face %d"), face)
                                                : _L("(none — uses Hole plane)"));
}

// Thread also latches onto a circular EDGE (a cylinder's rim), so it has two kinds to name.
void DesignPanel::set_thread_target_label(int face, int edge)
{
    if (!m_thread_target_label) return;
    m_thread_target_label->SetLabel(
        face >= 0 ? wxString::Format(_L("Face %d"), face)
      : edge >= 0 ? wxString::Format(_L("Edge %d"), edge)
                  : _L("(none — uses Thread plane)"));
}

SketchPlane DesignPanel::hole_plane() const
{
    if (m_hole_on_face) return m_hole_face_plane;
    SketchPlane p = plane_from_index(m_hole_plane->GetSelection());
    p.origin += m_doc.modeling_origin;
    return p;
}

void DesignPanel::on_add_hole()
{
    if (m_doc.body.IsNull()) {
        set_status(_L("Add a solid (sketch + extrude) first"));
        return;
    }
    SketchPlane plane   = hole_plane();
    double      dia     = m_hole_diameter->GetValue();
    double      depth   = m_hole_depth->GetValue();
    bool        through = m_hole_through->GetValue();
    double      px      = m_hole_x->GetValue();
    double      py      = m_hole_y->GetValue();

    m_feature_counter++;
    const int hidx = m_doc.add_hole(dia, depth, through, px, py, plane,
                                    "Hole" + std::to_string(m_feature_counter));
    // On-face holes drill the body the face belongs to (even after the pick was cleared).
    if (m_hole_on_face && hidx >= 0 && hidx < int(m_doc.features.size()))
        m_doc.features[hidx].target_body = m_hole_face_body;

    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

SketchPlane DesignPanel::thread_plane() const
{
    if (m_thread_on_face) return m_thread_face_plane;
    SketchPlane p = plane_from_index(m_thread_plane->GetSelection());
    p.origin += m_doc.modeling_origin;
    return p;
}

void DesignPanel::apply_thread_standard()
{
    if (!m_thread_std) return;
    const int sel = m_thread_std->GetSelection();
    if (sel <= 0) return;   // 0 = "Custom" → leave the manual spins untouched

    const ThreadSpec* s = find_thread_standard(
        m_thread_std->GetString(sel).utf8_string());
    if (!s) return;

    // Pitch and depth are the defining "measures" of the standard — always apply.
    if (m_thread_pitch) m_thread_pitch->SetValue(s->pitch_mm);
    if (m_thread_depth) m_thread_depth->SetValue(s->thread_depth_mm());

    // Nominal diameter: external rod = major diameter; internal tapped bore = minor (tap-drill)
    // diameter. On a picked cylindrical surface/edge the diameter comes from the real geometry,
    // so don't override it there. (The field holds DIAMETER.)
    if (!m_thread_on_face && m_thread_radius) {
        const bool internal = m_thread_internal && m_thread_internal->GetValue();
        const double d = internal ? s->minor_diameter_mm() : s->major_diameter_mm;
        m_thread_radius->SetValue(d);
    }

    if (m_status)
        set_status(wxString::Format(_L("Thread standard: %s  (pitch %.3g mm)"),
                                            m_thread_std->GetString(sel), s->pitch_mm));
}

void DesignPanel::infer_thread_spec(double diameter)
{
    // Snap to the nearest standard thread by nominal (major) diameter, so picking a Ø9.9 boss
    // gives M10 — the M diameter, pitch AND depth all follow from the cylinder's base diameter.
    const auto& stds = thread_standards();
    int best = -1; double bestErr = 1e30;
    for (int i = 0; i < int(stds.size()); ++i) {
        const double e = std::abs(stds[i].major_diameter_mm - diameter);
        if (e < bestErr) { bestErr = e; best = i; }
    }
    if (best < 0) { if (m_thread_radius) m_thread_radius->SetValue(diameter); return; }
    const ThreadSpec& s = stds[best];
    if (m_thread_std)    m_thread_std->SetSelection(best + 1);    // row 0 is "Custom"
    if (m_thread_radius) m_thread_radius->SetValue(s.major_diameter_mm);  // field = DIAMETER
    if (m_thread_pitch)  m_thread_pitch->SetValue(s.pitch_mm);
    if (m_thread_depth)  m_thread_depth->SetValue(s.thread_depth_mm());
}

void DesignPanel::on_add_thread()
{
    bool internal = m_thread_internal->GetValue();
    if (internal && m_doc.body.IsNull()) {
        set_status(_L("Thread needs a solid body — add or import one first"));
        return;
    }
    SketchPlane plane = thread_plane();

    m_feature_counter++;
    const int tidx = m_doc.add_thread(m_thread_radius->GetValue() * 0.5, m_thread_pitch->GetValue(),
                     m_thread_height->GetValue(), m_thread_depth->GetValue(),
                     internal, m_thread_x->GetValue(), m_thread_y->GetValue(),
                     plane, "Thread" + std::to_string(m_feature_counter));
    // On-surface internal thread taps the body the cylindrical face belongs to.
    if (m_thread_on_face && tidx >= 0 && tidx < int(m_doc.features.size()))
        m_doc.features[tidx].target_body = m_thread_face_body;

    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

void DesignPanel::on_add_revolve()
{
    if (m_revolve_sketch_ref < 0 || m_revolve_sketch_ref >= int(m_doc.features.size())) {
        set_status(_L("Pick a sketch profile to revolve first"));
        return;
    }
    const BooleanMode mode = static_cast<BooleanMode>(m_revolve_mode->GetSelection());
    if (mode != BooleanMode::New && m_doc.body.IsNull()) {
        set_status(_L("Revolve needs a solid body — add or import one first"));
        return;
    }
    m_feature_counter++;
    m_doc.add_revolve(m_revolve_sketch_ref, m_revolve_angle->GetValue(),
                      m_revolve_axis->GetSelection(), m_revolve_flip->GetValue(),
                      mode, "Revolve" + std::to_string(m_feature_counter));

    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

void DesignPanel::on_add_sweep()
{
    if (m_sweep_profile_ref < 0 || m_sweep_profile_ref >= int(m_doc.features.size())) {
        set_status(_L("Pick a profile sketch to sweep first"));
        return;
    }
    const int sel = m_sweep_path->GetSelection();
    const int path_ref = (sel != wxNOT_FOUND)
        ? int(reinterpret_cast<intptr_t>(m_sweep_path->GetClientData(sel))) : -1;
    if (path_ref < 0) {
        set_status(_L("Pick a path sketch for the sweep"));
        return;
    }
    const BooleanMode mode = static_cast<BooleanMode>(m_sweep_mode->GetSelection());
    if (mode != BooleanMode::New && m_doc.body.IsNull()) {
        set_status(_L("Sweep needs a solid body — add or import one first"));
        return;
    }
    m_feature_counter++;
    m_doc.add_sweep(m_sweep_profile_ref, path_ref, mode,
                    "Sweep" + std::to_string(m_feature_counter));

    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

void DesignPanel::on_add_loft()
{
    // Collect the checked profile sketches in list (recipe) order.
    std::vector<int> refs;
    for (unsigned i = 0; i < m_loft_list->GetCount(); ++i)
        if (m_loft_list->IsChecked(i) && i < m_loft_sketch_idx.size())
            refs.push_back(m_loft_sketch_idx[i]);
    if (refs.size() < 2) {
        set_status(_L("Check at least two profile sketches to loft"));
        return;
    }
    const BooleanMode mode = static_cast<BooleanMode>(m_loft_mode->GetSelection());
    if (mode != BooleanMode::New && m_doc.body.IsNull()) {
        set_status(_L("Loft needs a solid body — add or import one first"));
        return;
    }
    m_feature_counter++;
    m_doc.add_loft(refs, m_loft_ruled->GetValue(), mode,
                   "Loft" + std::to_string(m_feature_counter));

    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

void DesignPanel::on_add_surface_extrude()
{
    if (m_surf_extrude_sketch_ref < 0 || m_surf_extrude_sketch_ref >= int(m_doc.features.size())) {
        set_status(_L("Pick a sketch profile to extrude first"));
        return;
    }
    m_feature_counter++;
    m_doc.add_surface_extrude(m_surf_extrude_sketch_ref, m_surf_extrude_distance->GetValue(),
                              "SurfaceExtrude" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_surface_revolve()
{
    if (m_surf_revolve_sketch_ref < 0 || m_surf_revolve_sketch_ref >= int(m_doc.features.size())) {
        set_status(_L("Pick a sketch profile to revolve first"));
        return;
    }
    m_feature_counter++;
    m_doc.add_surface_revolve(m_surf_revolve_sketch_ref, m_surf_revolve_angle->GetValue(),
                              m_surf_revolve_axis->GetSelection(),
                              "SurfaceRevolve" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_surface_loft()
{
    std::vector<int> refs;
    for (unsigned i = 0; i < m_surf_loft_list->GetCount(); ++i)
        if (m_surf_loft_list->IsChecked(i) && i < m_surf_loft_sketch_idx.size())
            refs.push_back(m_surf_loft_sketch_idx[i]);
    if (refs.size() < 2) {
        set_status(_L("Check at least two profile sketches to loft"));
        return;
    }
    m_feature_counter++;
    m_doc.add_surface_loft(refs, m_surf_loft_ruled->GetValue(),
                           "SurfaceLoft" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_surface_fill()
{
    if (m_surf_fill_sketch_ref < 0 || m_surf_fill_sketch_ref >= int(m_doc.features.size())) {
        set_status(_L("Pick a sketch to fill first"));
        return;
    }
    m_feature_counter++;
    m_doc.add_surface_fill(m_surf_fill_sketch_ref,
                           "SurfaceFill" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_surface_offset()
{
    const int sel = sheet_choice_body(m_surf_offset_body);
    if (sel < 0 || sel >= int(m_doc.bodies.size())) {
        set_status(_L("Select a sheet body first"));
        return;
    }
    m_feature_counter++;
    m_doc.add_surface_offset(sel, m_surf_offset_distance->GetValue(),
                             "SurfaceOffset" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_thicken_surface()
{
    const int sel = sheet_choice_body(m_surf_thicken_body);
    if (sel < 0 || sel >= int(m_doc.bodies.size())) {
        set_status(_L("Select a sheet body first"));
        return;
    }
    m_feature_counter++;
    m_doc.add_thicken_surface(sel, m_surf_thicken_thickness->GetValue(),
                              m_surf_thicken_flip->GetValue(),
                              "ThickenSurface" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

// Compose the same matrix apply_transform() builds in the kernel: rotate about the pivot first,
// then translate. Kept here so the preview and the committed feature can never disagree.
static Transform3d xf_compose(const Vec3d& t, const Vec3d& axis, const Vec3d& pivot, double deg)
{
    Transform3d r = Transform3d::Identity();
    if (std::abs(deg) > 1e-12 && axis.norm() > 1e-9)
        r = Transform3d(Eigen::Translation3d(pivot))
          * Transform3d(Eigen::AngleAxisd(deg * M_PI / 180.0, axis.normalized()))
          * Transform3d(Eigen::Translation3d(-pivot));
    return Transform3d(Eigen::Translation3d(t)) * r;
}

// Live preview for the Transform card. The gizmo already writes its drag into the body's display
// transform; the typed fields must use the SAME channel or the numbers and the geometry disagree
// until Confirm — which is what makes typing a distance look like it did nothing. In edit mode the
// committed transform is already baked into the kernel geometry, so undo it first: without that,
// typing 80 over a committed 34 would show the body at 114.
void DesignPanel::xf_live_preview()
{
    if (m_active != Tool::Transform || m_xf_dx == nullptr) return;
    if (m_xf_copy && m_xf_copy->GetValue()) return;   // a copy leaves the original where it is
    sync_body_xform();
    const int sel = m_xf_body ? m_xf_body->GetSelection() : wxNOT_FOUND;
    const int b   = (sel != wxNOT_FOUND) ? sel : int(m_body_xform.size()) - 1;
    if (b < 0 || b >= int(m_body_xform.size())) return;

    if (m_xf_prev_body != b) {
        xf_clear_preview();                 // the card retargeted: hand the old body back first
        m_xf_prev_body = b;
        m_xf_prev_base = (b == m_xf_gizmo_body) ? m_xf_gizmo_base : m_body_xform[b];
    }
    const int   ax   = m_xf_axis->GetSelection();
    const Vec3d axis = (ax == 0) ? Vec3d::UnitX() : (ax == 1) ? Vec3d::UnitY() : Vec3d::UnitZ();
    const Vec3d pivot(m_xf_pivot_x->GetValue(), m_xf_pivot_y->GetValue(), m_xf_pivot_z->GetValue());
    const Transform3d want = xf_compose(Vec3d(m_xf_dx->GetValue(), m_xf_dy->GetValue(),
                                              m_xf_dz->GetValue()),
                                        axis, pivot, m_xf_angle->GetValue());
    Transform3d undo = Transform3d::Identity();
    if (m_edit_index >= 0 && m_edit_index < int(m_doc.features.size())) {
        const CadFeature& f = m_doc.features[m_edit_index];
        if (f.type == CadFeatureType::Transform && !f.xf_copy)
            undo = xf_compose(f.xf_translate, f.xf_axis, f.xf_pivot, f.xf_angle_deg).inverse();
    }
    m_body_xform[b] = want * undo * m_xf_prev_base;
    feed_bodies();
}

// Hand a previewed body back to the pose it had before the card touched it. Every exit from the
// Transform card passes through here, so a preview can never survive into the committed document.
void DesignPanel::xf_clear_preview()
{
    if (m_xf_prev_body < 0) return;
    sync_body_xform();
    if (m_xf_prev_body < int(m_body_xform.size()))
        m_body_xform[m_xf_prev_body] = m_xf_prev_base;
    m_xf_prev_body = -1;
    feed_bodies();
}

void DesignPanel::on_add_transform()
{
    xf_clear_preview();   // the feature performs this motion parametrically; the preview must go
    // The gizmo baked its drag into the display transform so the body followed the cursor.
    // The feature about to be created performs that same motion parametrically, so hand the
    // body back to its pre-drag pose first or it moves twice.
    if (m_xf_gizmo_body >= 0) {
        sync_body_xform();
        if (m_xf_gizmo_body < int(m_body_xform.size()))
            m_body_xform[m_xf_gizmo_body] = m_xf_gizmo_base;
        if (m_viewport) m_viewport->clear_move_gizmo();
        feed_bodies();   // set_status_ok() re-feeds on success, but the recompute-ERROR path
                         // below does not — without this the body would keep showing the
                         // dragged pose after a Transform that failed to build.
        m_xf_gizmo_body = -1;
        m_move_body     = -1;
    }
    if (m_doc.bodies.empty()) {
        set_status(_L("Transform needs a body — add or import one first"));
        return;
    }
    const int sel = m_xf_body->GetSelection();
    const int target = (sel != wxNOT_FOUND) ? sel : -1;
    const Vec3d trans(m_xf_dx->GetValue(), m_xf_dy->GetValue(), m_xf_dz->GetValue());
    const int ax = m_xf_axis->GetSelection();
    const Vec3d axis = (ax == 0) ? Vec3d(1, 0, 0) : (ax == 1) ? Vec3d(0, 1, 0) : Vec3d(0, 0, 1);
    const Vec3d pivot(m_xf_pivot_x->GetValue(), m_xf_pivot_y->GetValue(), m_xf_pivot_z->GetValue());
    m_feature_counter++;
    m_doc.add_transform(target, trans, axis, pivot, m_xf_angle->GetValue(), m_xf_copy->GetValue(),
                        "Transform" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_mirror()
{
    if (m_doc.bodies.empty()) {
        set_status(_L("Mirror needs a body — add or import one first"));
        return;
    }
    const int sel = m_mirror_body->GetSelection();
    const int target = (sel != wxNOT_FOUND) ? sel : -1;
    const BooleanMode mode = m_mirror_keep->GetValue() ? BooleanMode::New : BooleanMode::Add;
    m_feature_counter++;
    m_doc.add_mirror(plane_from_choice(m_mirror_plane->GetSelection()), target, mode,
                     "Mirror" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_thicken()
{
    if (m_doc.bodies.empty()) {
        set_status(_L("Thicken needs a solid body — add or import one first"));
        return;
    }
    if (m_sel_solid_face < 0) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Pick a solid face to thicken first"));
        m_status->Refresh();
        return;
    }
    const int sel = m_thicken_body->GetSelection();
    const int target = (sel != wxNOT_FOUND) ? sel : -1;
    m_feature_counter++;
    m_doc.add_thicken(target, m_sel_solid_face, m_thicken_thickness->GetValue(),
                      m_thicken_flip->GetValue(), "Thicken" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_rib()
{
    if (m_doc.bodies.empty()) {
        set_status(_L("Rib needs a solid body — add or import one first"));
        return;
    }
    const int bsel = m_rib_body->GetSelection();
    const int target = (bsel != wxNOT_FOUND) ? bsel : -1;
    const int ssel = m_rib_sketch->GetSelection();
    const int sketch_ref = (ssel != wxNOT_FOUND)
                               ? int(reinterpret_cast<intptr_t>(m_rib_sketch->GetClientData(ssel))) : -1;
    if (sketch_ref < 0) {
        set_status(_L("Pick a sketch with an open line first"));
        return;
    }
    m_feature_counter++;
    m_doc.add_rib(sketch_ref, m_rib_entity->GetValue(), m_rib_thickness->GetValue(),
                  m_rib_depth->GetValue(), target, "Rib" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_project()
{
    if (m_doc.bodies.empty()) {
        set_status(_L("Project needs a body — add or import one first"));
        return;
    }
    const int sel = m_proj_source_body->GetSelection();
    const int src_body = (sel != wxNOT_FOUND) ? sel : -1;
    const int face = (m_sel_solid_face >= 0) ? m_sel_solid_face : -1;
    m_feature_counter++;
    m_doc.add_project_edges(src_body, {}, face,
                            plane_from_choice(m_proj_plane->GetSelection()),
                            "Project" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_delete_face()
{
    if (m_doc.bodies.empty()) {
        set_status(_L("Delete Face needs a body — add or import one first"));
        return;
    }
    if (m_del_faces.empty()) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Add at least one face to delete first"));
        m_status->Refresh();
        return;
    }
    const int sel = m_del_face_body->GetSelection();
    const int target = (sel != wxNOT_FOUND) ? sel : -1;
    m_feature_counter++;
    m_doc.add_delete_face(target, m_del_faces, "DeleteFace" + std::to_string(m_feature_counter));
    m_del_faces.clear();  // consumed; fresh state for the next use
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_helix()
{
    m_feature_counter++;
    m_doc.add_helix(plane_from_choice(m_helix_plane->GetSelection()),
                    m_helix_radius->GetValue(), m_helix_pitch->GetValue(),
                    m_helix_height->GetValue(), m_helix_left_handed->GetValue(),
                    m_helix_taper->GetValue(), "Helix" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_mate()
{
    const int sel_a = m_mate_cs_a->GetSelection();
    const int sel_b = m_mate_cs_b->GetSelection();
    if (sel_a == wxNOT_FOUND || sel_b == wxNOT_FOUND) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Mate needs two CoordSys features — create them first"));
        m_status->Refresh();
        return;
    }
    const int cs_a = int(reinterpret_cast<intptr_t>(m_mate_cs_a->GetClientData(sel_a)));
    const int cs_b = int(reinterpret_cast<intptr_t>(m_mate_cs_b->GetClientData(sel_b)));
    if (cs_a == cs_b) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Mate: CS A and CS B must be different CoordSys features"));
        m_status->Refresh();
        return;
    }
    m_feature_counter++;
    int idx = m_doc.add_mate(m_mate_kind->GetSelection(), cs_a, cs_b,
                             m_mate_offset->GetValue(), m_mate_angle->GetValue(),
                             m_mate_flip->GetValue(),
                             "Mate" + std::to_string(m_feature_counter));
    if (idx < 0) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Mate rejected"));
        m_status->Refresh();
        return;
    }
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_check_interference()
{
    if (m_doc.bodies.size() < 2) {
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("No interference — need at least two solid bodies to check"));
        m_status->Refresh();
        return;
    }
    const auto pairs = m_doc.check_interference();
    if (pairs.empty()) {
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("No interference found"));
        m_status->Refresh();
        return;
    }
    double worst = 0;
    for (const auto& p : pairs)
        if (p.volume > worst) worst = p.volume;
    m_status->SetForegroundColour(wxNullColour);
    set_status(wxString::Format(_L("%zu interference pairs, worst %.2f mm³"),
                                        pairs.size(), worst));
    m_status->Refresh();
    wxString msg = _L("Interference pairs:\n\n");
    for (const auto& p : pairs) {
        // 1-based, like every other body label in this panel and in the parts tree:
        // reporting "Body 1" for what the tree calls "Body 2" is worse than no name.
        wxString na = wxString::Format(_L("Body %d"), p.body_a + 1);
        wxString nb = wxString::Format(_L("Body %d"), p.body_b + 1);
        if (p.body_a >= 0 && p.body_a < int(m_doc.bodies.size()) && !m_doc.bodies[p.body_a].name.empty())
            na = wxString::FromUTF8(m_doc.bodies[p.body_a].name);
        if (p.body_b >= 0 && p.body_b < int(m_doc.bodies.size()) && !m_doc.bodies[p.body_b].name.empty())
            nb = wxString::FromUTF8(m_doc.bodies[p.body_b].name);
        msg += wxString::Format("%s <-> %s: %.4f mm³\n", na, nb, p.volume);
    }
    wxMessageBox(msg, _L("Interference"), wxOK, this);
}

// Mass properties of the selected solid. A report, not a feature: it never checkpoints, never
// recomputes and never opens a card, which is why it sits beside the interference check rather
// than in the on_add_* family. The caller only reaches us with m_sel_solid_body in range.
void DesignPanel::on_mass_properties()
{
    const auto mp = GeometryEngine::mass_properties(m_doc.bodies[m_sel_solid_body].shape);
    if (!mp.valid) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Mass properties could not be computed for this body"));
        m_status->Refresh();
        return;
    }
    // 1-based, and the body's own name when it has one — the same wording the parts list uses.
    wxString name = wxString::Format(_L("Body %d"), m_sel_solid_body + 1);
    if (!m_doc.bodies[m_sel_solid_body].name.empty())
        name = wxString::FromUTF8(m_doc.bodies[m_sel_solid_body].name);
    if (!mp.is_solid) {
        // Sheet body: quoting a volume here would be inventing material that is not there.
        m_status->SetForegroundColour(wxNullColour);
        set_status(wxString::Format(_L("%s: sheet body — %.2f cm² of surface, no volume"),
                                    name, mp.surface_area / 100.0));
        m_status->Refresh();
        wxMessageBox(wxString::Format(_L("%s\n\nSheet body (open shell)\nSurface area: %.2f cm²\n\n"
                                         "A sheet encloses no material, so it has no volume. "
                                         "Thicken it into a solid to get one."),
                                      name, mp.surface_area / 100.0),
                     _L("Mass properties"), wxOK, this);
        return;
    }
    m_status->SetForegroundColour(wxNullColour);
    set_status(wxString::Format(_L("%s: %.3f cm³, %.2f cm²"),
                                        name, mp.volume / 1000.0, mp.surface_area / 100.0));
    m_status->Refresh();
    wxMessageBox(wxString::Format(_L("%s\n\nVolume: %.3f cm³\nSurface area: %.2f cm²"),
                                  name, mp.volume / 1000.0, mp.surface_area / 100.0),
                 _L("Mass properties"), wxOK, this);
}

// The rows are only the SHEET bodies, so a row index is NOT a body index — with a solid at 0
// and a sheet at 1 the single row is row 0 but body 1. Every caller must therefore read the
// real body index out of the client data (3-arg Append; the 2-arg form takes a bitmap), never
// GetSelection(). Getting this wrong targets a solid and the kernel rejects it with
// "target is not a sheet", which reads as a kernel bug rather than a picker bug.
void DesignPanel::populate_sheet_body_choices(ComboBox* c) const
{
    if (!c) return;
    const int keep = c->GetSelection();
    c->Clear();
    for (size_t i = 0; i < m_doc.bodies.size(); ++i) {
        if (!CadDocument::is_sheet_shape(m_doc.bodies[i].shape)) continue;
        const std::string& n = m_doc.bodies[i].name;
        combo_append_index(c, n.empty() ? wxString::Format(_L("Body %zu"), i + 1)
                                        : wxString::FromUTF8(n), int(i));
    }
    if (c->GetCount() > 0)
        c->SetSelection(std::min(std::max(keep, 0), int(c->GetCount()) - 1));
}

// Real body index behind the current row of a sheet-filtered picker, or -1.
int DesignPanel::sheet_choice_body(ComboBox* c)
{
    if (!c) return -1;
    const int sel = c->GetSelection();
    if (sel == wxNOT_FOUND) return -1;
    return int(reinterpret_cast<intptr_t>(c->GetClientData(sel)));
}

// Select the row whose body index is `body`, so a re-edit restores the stored target rather
// than treating it as a row number.
void DesignPanel::select_sheet_choice(ComboBox* c, int body)
{
    if (!c) return;
    for (unsigned i = 0; i < c->GetCount(); ++i) {
        if (int(reinterpret_cast<intptr_t>(c->GetClientData(i))) == body) { c->SetSelection(int(i)); return; }
    }
    if (c->GetCount() > 0) c->SetSelection(0);
}

void DesignPanel::on_add_pattern()
{
    if (m_doc.bodies.empty()) {
        set_status(_L("Pattern needs a solid body — add or import one first"));
        return;
    }
    const bool circular = (m_pattern_type->GetSelection() == 1);
    const int  target   = (m_sel_solid_body >= 0 && m_sel_solid_body < int(m_doc.bodies.size()))
                          ? m_sel_solid_body : -1;
    m_feature_counter++;
    m_doc.add_pattern(circular, int(m_pattern_count->GetValue()),
                      m_pattern_spacing->GetValue(), m_pattern_dir->GetSelection(),
                      m_pattern_angle->GetValue(), target,
                      "Pattern" + std::to_string(m_feature_counter));

    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

int DesignPanel::selected_body_default() const
{
    const int n = int(m_doc.bodies.size());
    if (n <= 0) return 0;
    return (m_sel_solid_body >= 0 && m_sel_solid_body < n) ? m_sel_solid_body : 0;
}

void DesignPanel::populate_body_choices(int as_of_feature)
{
    // Re-editing a Boolean: list the bodies as they existed just before it ran, so a tool body
    // it consumed still shows and the saved target/tool selections round-trip. Replay a copy of
    // the recipe truncated to [0, as_of_feature). Fall back to the live bodies if the replay is
    // degenerate (e.g. fewer than the saved refs need).
    const std::vector<CadBody>* src = &m_doc.bodies;
    std::vector<CadBody> as_of;
    if (as_of_feature >= 0 && as_of_feature <= int(m_doc.features.size())) {
        CadDocument tmp = m_doc;
        tmp.features.resize(as_of_feature);
        if (tmp.recompute() && !tmp.bodies.empty()) { as_of = tmp.bodies; src = &as_of; }
    }
    auto fill = [&](ComboBox* c, int def) {
        if (!c) return;
        c->Clear();
        for (size_t i = 0; i < src->size(); ++i) {
            const std::string& n = (*src)[i].name;
            c->Append(n.empty() ? wxString::Format(_L("Body %zu"), i + 1) : wxString::FromUTF8(n));
        }
        if (c->GetCount() > 0)
            c->SetSelection(std::min(def, int(c->GetCount()) - 1));   // selection index == body index
    };
    const int picked = selected_body_default();
    fill(m_bool_target, picked);
    // …and the tool body is a DIFFERENT one: defaulting both to the same body is a no-op the
    // user has to notice and undo. Fall back to the neighbour of whatever was picked.
    fill(m_bool_tool, picked == 0 ? 1 : 0);
    fill(m_cut_target, picked);
}

void DesignPanel::fill_body_choice(ComboBox* c, int as_of_feature, int want)
{
    if (!c) return;
    // Same replay as populate_body_choices, for the single-combo tools. A stored target_body
    // indexes the body list AS IT WAS just before that feature ran; listing the final bodies
    // instead makes the saved index select whatever now sits at that position, which is a
    // different body as soon as a later Cut splits one (indices shift up) or a later Boolean
    // consumes its tool body (indices shift down).
    const std::vector<CadBody>* src = &m_doc.bodies;
    std::vector<CadBody> as_of;
    if (as_of_feature >= 0 && as_of_feature <= int(m_doc.features.size())) {
        CadDocument tmp = m_doc;
        tmp.features.resize(as_of_feature);
        if (tmp.recompute() && !tmp.bodies.empty()) { as_of = tmp.bodies; src = &as_of; }
    }
    c->Clear();
    for (size_t i = 0; i < src->size(); ++i) {
        const std::string& n = (*src)[i].name;
        c->Append(n.empty() ? wxString::Format(_L("Body %zu"), i + 1) : wxString::FromUTF8(n));
    }
    if (want >= 0 && want < int(c->GetCount())) c->SetSelection(want);
    else if (c->GetCount() > 0)                 c->SetSelection(0);
}

void DesignPanel::on_add_boolean()
{
    if (m_doc.bodies.size() < 2) {
        set_status(_L("Boolean needs two solid bodies — add or import a second one"));
        return;
    }
    const int sel = m_bool_op->GetSelection();
    const BooleanMode op = (sel == 1) ? BooleanMode::Cut
                         : (sel == 2) ? BooleanMode::Intersect
                                      : BooleanMode::Add;   // 0 = Union
    m_feature_counter++;
    m_doc.add_boolean(op, m_bool_target->GetSelection(), m_bool_tool->GetSelection(),
                      m_bool_keep->GetValue(), m_bool_tol->GetValue(), -1, -1,
                      "Boolean" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_cut()
{
    if (m_doc.bodies.empty()) {
        set_status(_L("Cut needs a solid body — add or import one first"));
        return;
    }
    m_feature_counter++;
    m_doc.add_cut(plane_from_choice(m_cut_plane->GetSelection()), m_cut_offset->GetValue(),
                  /*flip*/ false, /*keep_upper*/ true, /*keep_lower*/ true,
                  m_cut_target->GetSelection(), "Cut" + std::to_string(m_feature_counter));
    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::populate_plane_choices(ComboBox* c) const
{
    if (!c) return;
    const int keep = c->GetSelection();
    c->Clear();
    c->Append(_L("XY")); c->Append(_L("XZ")); c->Append(_L("YZ"));
    for (const auto& dp : m_doc.resolve_datum_planes())
        c->Append(wxString::FromUTF8(dp.first));
    c->SetSelection((keep >= 0 && keep < int(c->GetCount())) ? keep : 0);
}

wxString DesignPanel::ref_plane_name(int row) const
{
    if (row == 1) return "XZ";
    if (row == 2) return "YZ";
    if (row >= 3) {
        const auto datums = m_doc.resolve_datum_planes();
        const int di = row - 3;
        if (di < int(datums.size())) return wxString::FromUTF8(datums[di].first);
    }
    return "XY";
}

SketchPlane DesignPanel::plane_from_choice(int row) const
{
    if (row < 3) {                                // 0=XY,1=XZ,2=YZ through the modeling origin
        SketchPlane p = plane_from_index(row);
        p.origin += m_doc.modeling_origin;
        return p;
    }
    // Datums are already in world coords (resolve_datum_planes applied the origin to their base).
    auto datums = m_doc.resolve_datum_planes();
    const int di = row - 3;
    if (di >= 0 && di < int(datums.size())) return datums[di].second;
    SketchPlane p = SketchPlane::XY(); p.origin += m_doc.modeling_origin; return p;
}

// A sketch goes where the user pointed. A planar face picked in the viewport wins outright; only
// when nothing is picked do we fall back to the reference plane, which itself is normally set by
// clicking one of the ghost planes in 3D (on_datum_base_picked) rather than by opening the combo.
// Before this, a picked face was ignored and the only way onto it was to build a Coincident datum
// plane first and then find it in a dropdown — three steps and a junk feature in the tree for the
// most common gesture in solid modelling. snaporca-3a2.
SketchPlane DesignPanel::sketch_plane_from_selection(wxString& what) const
{
    SketchPlane p;
    // An explicitly face-level selection first, then the face merely CLICKED ON while the whole
    // body was selected. The second case is the common one: one click on a face is what a user
    // means by "select this face", and requiring the cycle's second click to make it count is the
    // whole reason this looked unfixed.
    const int fb = (m_sel_solid_face >= 0 && m_sel_solid_body >= 0) ? m_sel_solid_body : m_pick_face_body;
    const int fi = (m_sel_solid_face >= 0 && m_sel_solid_body >= 0) ? m_sel_solid_face : m_pick_face;
    if (fb >= 0 && fi >= 0 && m_doc.plane_of_face(fb, fi, p)) {
        what = (m_doc.bodies.size() > 1)
             ? wxString::Format(_L("the picked face of Body %d"), fb + 1)
             : _L("the picked face");
        return p;
    }
    what = ref_plane_name(m_ref_plane);
    return plane_from_choice(m_ref_plane);
}

// Is there a sketch target the USER chose, and what is it called? Distinct from
// sketch_plane_from_selection, which always answers because it falls back to m_ref_plane —
// a caller that wants to say "sketching on X" needs to know whether there is anything to fall
// back FROM, so it can ask for a plane instead of claiming one the user never picked.
bool DesignPanel::sketch_plane_target(wxString& what) const
{
    const int fb = (m_sel_solid_face >= 0 && m_sel_solid_body >= 0) ? m_sel_solid_body : m_pick_face_body;
    const int fi = (m_sel_solid_face >= 0 && m_sel_solid_body >= 0) ? m_sel_solid_face : m_pick_face;
    SketchPlane p;
    if (fb >= 0 && fi >= 0 && m_doc.plane_of_face(fb, fi, p)) {
        what = (m_doc.bodies.size() > 1)
             ? wxString::Format(_L("the picked face of Body %d"), fb + 1)
             : _L("the picked face");
        return true;
    }
    if (m_plane_picked) { what = ref_plane_name(m_ref_plane); return true; }
    return false;
}

// ---------------------------------------------------------------------------------------------
// The object-driven offer (charter §4.1). Right-click the geometry and get a vertical list in the
// ratified row order, with the verbs that do not apply DISABLED IN PLACE carrying their reason.
// The rows come from DesignOffer.hpp, generated from docs/ux/tool_atlas.json — the same file the
// mockups are drawn from, so a drawing and the product cannot drift apart.
// ---------------------------------------------------------------------------------------------

bool DesignPanel::sketch_map_applies() const
{
    return m_ui_mode == UiMode::Sketch || (m_viewport && m_viewport->is_sketching());
}

// Which kind of thing is selected, as an OfferSel. Classified by the level the pick cycle has
// actually REACHED, so the menu describes what is highlighted — a header that names a face while
// the whole body is lit would be lying, and this menu's whole value is that it tells the truth
// about the selection. (Sketching on the face you merely clicked is unaffected: that path is
// sketch_plane_from_selection, which deliberately uses m_pick_face. snaporca-3a2.)
int DesignPanel::offer_selection_kind() const
{
    if (sketch_map_applies()) {
        // Classify WHAT is selected in the sketch, rather than collapsing every sketch state to
        // SkNone. The offer table has always described verbs for a selected line, arc, point or
        // pair — Trim, Extend, Fillet, Chamfer, Offset, Mirror, the arrays, Move/Rotate/Scale,
        // Constrain, Delete — but nothing ever RETURNED those kinds, so all thirteen were
        // unreachable from the menu and the sketch-editing vocabulary did not exist in the one
        // place this app tells users to look. A user comparing against Onshape reported exactly
        // that about constraints (OrcaSlicer PR #15238).
        const int n = m_viewport ? m_viewport->sketch_selection_count() : 0;
        if (n >= 2) return int(OfferSel::Sk2Ent);
        if (n == 1) {
            SketchEntity::Type t = SketchEntity::Type::Line;
            if (m_viewport && m_viewport->sketch_first_selected_type(t)) {
                switch (t) {
                case SketchEntity::Type::Line:  return int(OfferSel::SkLine);
                case SketchEntity::Type::Point: return int(OfferSel::SkPoint);
                // Arc, Circle, Ellipse, EllipseArc, BSpline all take the curve vocabulary.
                default:                        return int(OfferSel::SkArc);
                }
            }
        }
        return int(OfferSel::SkNone);
    }

    const int nb = int(m_doc.bodies.size());
    if (m_sel_solid_vertex && m_sel_solid_body >= 0 && m_sel_solid_body < nb)
        return int(OfferSel::Vertex);
    if (m_sel_solid_edge >= 0 && m_sel_solid_body >= 0 && m_sel_solid_body < nb) {
        const TopoDS_Edge e = GeometryEngine::edge_by_index(m_doc.bodies[m_sel_solid_body].shape,
                                                            m_sel_solid_edge);
        return int(GeometryEngine::circle_of_edge(e).ok ? OfferSel::EdgeCirc : OfferSel::EdgeStr);
    }
    if (m_sel_solid_face >= 0 && m_sel_solid_body >= 0 && m_sel_solid_body < nb) {
        SketchPlane p;
        if (m_doc.plane_of_face(m_sel_solid_body, m_sel_solid_face, p))
            return int(OfferSel::FacePlanar);
        const TopoDS_Face f = GeometryEngine::face_by_index(m_doc.bodies[m_sel_solid_body].shape,
                                                            m_sel_solid_face);
        return int(GeometryEngine::cylinder_of_face(f).ok ? OfferSel::FaceCyl : OfferSel::FaceOther);
    }
    if (m_sel_sketch_region >= 0)
        return int(OfferSel::SkLoop);
    if (m_sel_solid_body >= 0 && m_sel_solid_body < nb)
        return int(CadDocument::is_sheet_shape(m_doc.bodies[m_sel_solid_body].shape)
                   ? OfferSel::BodySheet : OfferSel::BodySolid);
    return int(OfferSel::None);
}

// Route a row to the code that already implements the verb. Nothing here re-implements a tool:
// the offer is a second door onto the same room, which is what keeps the toolbar, the shortcuts
// and the menu from drifting into three behaviours.
void DesignPanel::run_offer_action(const char* action)
{
    if (!action || !*action)
        return;
    const std::string a(action);
    if (a.rfind("key:", 0) == 0) {
        const std::string k = a.substr(4);
        if (k.size() >= 3 && k[0] == 'S' && k[1] == '+') {          // "S+E" -> Shift+E, model mode
            auto it = m_keys_feature.find(int(k[2]) | SC_SHIFT);
            if (it != m_keys_feature.end() && it->second) it->second();
        } else if (!k.empty()) {                                     // "L" -> sketch-mode letter
            auto it = m_keys_sketch.find(int(k[0]));
            if (it != m_keys_sketch.end() && it->second) it->second();
        }
        return;
    }
    auto it = m_verb_actions.find(a);
    if (it != m_verb_actions.end() && it->second)
        it->second();
}

wxPoint DesignPanel::offer_anchor() const
{
    const wxPoint mouse = wxGetMousePosition();
    if (!m_viewport)
        return mouse;
    const wxRect r = m_viewport->GetScreenRect();
    if (r.Contains(mouse))
        return mouse;
    return wxPoint(r.x + r.width / 2, r.y + r.height / 2);
}

// Append a verb row carrying the same glyph its toolbar button shows. The icon name comes from
// the atlas, so a verb and its icon are declared in one place; a null or unknown name leaves
// the row text-only rather than drawing a blank square.
//
// THE BITMAP MUST BE SET BEFORE Append(). wxGTK builds the GtkMenuItem inside Append and reads
// GetBitmap() right there (gtk/menu.cpp) — it makes a gtk_image_menu_item only if a bitmap is
// already present, and a plain one otherwise. Setting it on the item Append() returns is too
// late and silently does nothing, which is exactly how the first attempt failed. This is why
// Orca's own append_menu_item() constructs, sets, then appends.
// One place that writes the status line, so every hint wraps instead of clipping at the panel
// edge. wxStaticText::Wrap() is destructive, which is fine here: the label is replaced whole
// each time, never appended to.
void DesignPanel::set_status(const wxString& text)
{
    if (m_status == nullptr) return;
    m_status->SetLabel(text);   // the ONE place that may call SetLabel directly
    // m_status is HIDDEN and kept only as the owner of the text and its colour — every caller
    // sets the colour on it just before calling here, so this stays the one place that knows
    // both. What the user reads is drawn along the BASE OF THE VIEWPORT: in the panel the line
    // was clipped at ~73 characters with no warning and no wrap (Wrap() never took effect —
    // snaporca-8cc), which silently length-limited every hint in the tab. The viewport's bottom
    // margin has the whole window width, so a sentence can be a sentence.
    if (m_viewport != nullptr) {
        // wxNullColour means "no opinion", and the dark default text colour is nearly invisible
        // on the dark HUD; only a colour a caller actually chose (the error red, the plane-pick
        // green) is carried over. Compared against the colour the label was CREATED with —
        // comparing against the parent's foreground instead reported "chosen" for every line,
        // and the neutral text came out the panel's grey.
        const wxColour fg = m_status->GetForegroundColour();
        m_viewport->set_status_text(text, fg != m_status_default_fg ? fg
                                                                    : wxColour(0xDD, 0xE1, 0xE6));
    }
}

wxMenuItem* DesignPanel::append_offer_item(wxMenu* menu, int id, const wxString& text,
                                           const OfferVerb& v)
{
    auto* item = new wxMenuItem(menu, id, text);
    if (v.icon != nullptr && *v.icon != '\0') {
        const wxBitmap bmp = create_scaled_bitmap(v.icon, this, 16);
        if (bmp.IsOk())
            item->SetBitmap(bmp);
    }
    menu->Append(item);
    return item;
}

void DesignPanel::show_offer_menu(const wxPoint& screen_pos)
{
    const int      kind = offer_selection_kind();
    const uint32_t bit  = offer_bit(OfferSel(kind));
    // Which verb MAP applies is a question about the MODE, not about whether a session is
    // running — the same distinction the keyboard already had to learn (snaporca-0ud). Gated on
    // is_sketching() the offer opened on entering a sketch showing the FEATURE rows, every one
    // of them refusing the sketch selection, so it read as a menu of nine dead entries.
    const bool     sketching = sketch_map_applies();

    const int bodies = int(m_doc.bodies.size());
    int sketches = 0;
    for (const auto& f : m_doc.features)
        if (f.type == CadFeatureType::Sketch) ++sketches;
    bool sheet = false;
    for (const auto& b : m_doc.bodies)
        if (CadDocument::is_sheet_shape(b.shape)) { sheet = true; break; }

    auto applies = [&](const OfferVerb& v) {
        return (v.accepts & bit) && v.need_bodies <= bodies && v.need_sketches <= sketches
               && (!v.need_sheet || sheet);
    };
    // Names and reasons live in the generated table as plain literals; they are the same strings
    // the toolbar already ships, so the catalogue already carries their translations.
    // Scope the lookup to the APPLICATION catalog. A bare wxGetTranslation() searches every
    // loaded catalog, wxWidgets' own wxstd included — so on a non-English system the two row
    // names that happen to be wx standard strings came back translated while the other six,
    // which wx does not know, stayed English. On an Italian desktop the offer read
    // "Create / Add material / Rimuovi / Fillet / chamfer / draft / Repeat / Transform /
    // Reference / Modify": one menu, two languages, and not because anything was mistranslated.
    // The same trap is waiting for any locale — Supprimer, Löschen, Eliminar. Naming the domain
    // means these strings are translated by OUR catalogue or not at all, which is consistent
    // either way.
    auto tr = [](const char* s) {
        return wxGetTranslation(wxString::FromUTF8(s), SLIC3R_APP_KEY);
    };
    auto label = [&](const OfferVerb& v) {
        wxString s = tr(v.name);
        if (v.key && *v.key) s += "\t" + wxString::FromUTF8(v.key);
        return s;
    };

    wxMenu menu;
    std::vector<const OfferVerb*> bound;      // menu id offset -> verb
    const int base = wxID_HIGHEST + 4200;

    for (int row = 0; row < kOfferRowCount; ++row) {
        std::vector<const OfferVerb*> live, family;
        for (int i = 0; i < kOfferVerbCount; ++i) {
            const OfferVerb& v = kOfferVerbs[i];
            if (v.row != row || v.sketch_mode != sketching) continue;
            family.push_back(&v);
            if (applies(v)) live.push_back(&v);
        }
        if (family.empty())
            continue;                                   // no verb of this family in this mode
        const wxString fam = tr(kOfferRowNames[row]);

        if (live.empty()) {
            // DISABLED IN PLACE, with the reason. This is the row that makes the list worth
            // having: a control that cannot be used still says what it is and what you would
            // have to do first, in the product's own words (L7).
            //
            // The reason must be TRUE for the situation in front of the user. Taking the first
            // refusal in the family printed "Transform needs a body — add or import one first"
            // on a document that has a body, because the real obstacle was that nothing was
            // selected. So: prefer the refusal of a verb that accepts THIS selection and fails
            // only on document state — that message is about the actual blocker. If no verb in
            // the family accepts this selection at all, the honest thing is to say so, or say
            // nothing.
            const char* why = nullptr;
            for (const OfferVerb* v : family)
                if ((v->accepts & bit) && v->refusal) { why = v->refusal; break; }
            wxString s = fam;
            if (why)
                s += wxString::FromUTF8("   —   ") + tr(why);
            else if (OfferSel(kind) == OfferSel::None)
                s += wxString::FromUTF8("   —   ") + _L("select something first");
            menu.Append(base + int(bound.size()), s)->Enable(false);
            bound.push_back(nullptr);
        } else if (live.size() == 1) {
            append_offer_item(&menu, base + int(bound.size()), label(*live[0]), *live[0])
                ->Enable(live[0]->action != nullptr);
            bound.push_back(live[0]);
        } else {
            // Two levels, not one. A verb with a `family` joins a nested submenu of that name
            // (Rectangle -> corner / centre / oblique / rounded); one without sits directly in
            // the row. Families keep the order of their first member, so the row's layout is
            // stable across selections — the whole point of a fixed address.
            auto* sub = new wxMenu();
            std::vector<std::pair<std::string, wxMenu*>> groups;   // insertion-ordered
            for (const OfferVerb* v : live) {
                wxMenu* target = sub;
                if (v->family && *v->family) {
                    auto it = std::find_if(groups.begin(), groups.end(),
                                           [&](const auto& g) { return g.first == v->family; });
                    if (it == groups.end()) {
                        auto* g = new wxMenu();
                        groups.emplace_back(v->family, g);
                        sub->AppendSubMenu(g, tr(v->family));
                        target = g;
                    } else {
                        target = it->second;
                    }
                }
                append_offer_item(target, base + int(bound.size()), label(*v), *v)
                    ->Enable(v->action != nullptr);
                bound.push_back(v);
            }
            menu.AppendSubMenu(sub, fam);
        }
    }

    // --- Mate palette section (snaporca-lukg part B) ---
    // Fed by CadDocument::mate_options() so the offer can never disagree with the kernel about
    // which assembly mates a connector pair admits. Shown only when the document holds at least
    // two ENABLED CoordSys features: below that the whole section would be one permanently dead
    // row, which is noise rather than information.
    // Set while a hovered mate row is showing its ghost, so the cleanup after PopupMenu can tell
    // "I put that there" from "some other tool's preview was already on screen".
    bool             mate_ghost = false;
    std::vector<int> cs_features;
    for (int i = 0; i < int(m_doc.features.size()); ++i)
        if (m_doc.features[i].type == CadFeatureType::CoordSys && m_doc.features[i].enabled)
            cs_features.push_back(i);
    if (cs_features.size() >= 2) {
        // Which two connectors the types would apply to. The Mate card's combos when it is open —
        // so the offer and the card can never disagree about the pair — otherwise the first two
        // enabled CoordSys features, which is a defensible default precisely because the header row
        // below NAMES it. An offer that acts on an unnamed pair would be worse than no offer.
        int cs_a = -1, cs_b = -1;
        if (m_active == Tool::Mate && m_mate_cs_a && m_mate_cs_b
            && m_mate_cs_a->GetSelection() != wxNOT_FOUND
            && m_mate_cs_b->GetSelection() != wxNOT_FOUND) {
            cs_a = int(reinterpret_cast<intptr_t>(m_mate_cs_a->GetClientData(m_mate_cs_a->GetSelection())));
            cs_b = int(reinterpret_cast<intptr_t>(m_mate_cs_b->GetClientData(m_mate_cs_b->GetSelection())));
        }
        if (cs_a < 0 || cs_b < 0) { cs_a = cs_features[0]; cs_b = cs_features[1]; }

        // The generated verb rows own [base, base + bound.size()); kOfferVerbCount is 87, so a
        // gap of 500 keeps this section's ids clear of that range and lets its own handler index
        // by a distinct offset.
        const int mate_base = base + 500;

        menu.AppendSeparator();
        // B is the connector on the body that MOVES (CadDocument.hpp:317), so B is the arrow's
        // destination — "A first" invites the opposite guess.
        const wxString name_a = wxString::FromUTF8(m_doc.features[cs_a].name);
        const wxString name_b = wxString::FromUTF8(m_doc.features[cs_b].name);
        menu.Append(mate_base, wxString::Format(_L("Mate: %s  →  %s"), name_a, name_b))->Enable(false);

        // A switch of literal _L() calls, not an array indexed by kind. _L is a gettext macro:
        // the extractor scans the SOURCE for literals, so _L(table[i]) compiles fine and then
        // silently ships five strings that are never in the catalogue and can never be
        // translated. These names appear nowhere else in the tree, so the array version would
        // have been their only occurrence.
        auto mate_kind_name = [](int kind) -> wxString {
            switch (kind) {
            case 0:  return _L("Fastened");
            case 1:  return _L("Planar");
            case 2:  return _L("Revolute");
            case 3:  return _L("Slider");
            default: return _L("Cylindrical");
            }
        };
        // Five rows, always, in kind order. Never reordered, never filtered — a menu that changes
        // shape between invocations destroys the motor memory experts rely on.
        const std::vector<CadDocument::MateOption> opts = m_doc.mate_options(cs_a, cs_b);
        for (int i = 0; i < int(opts.size()); ++i) {
            const CadDocument::MateOption& o = opts[i];
            wxString label = mate_kind_name(o.kind);
            if (!o.viable)
                label += wxString::FromUTF8("   —   ") + wxString::FromUTF8(o.reason);
            menu.Append(mate_base + 1 + i, label)->Enable(o.viable);
        }

        // Hovering a row shows the RESULT, not a description of it (epic gap G3). preview() moves
        // the body on a throwaway copy of the document, so nothing is written until the click —
        // the "commit nothing until you choose" behaviour the palette was asked for.
        auto drop_ghost = [this, &mate_ghost]() {
            if (!mate_ghost) return;
            m_viewport->clear_preview();
            m_viewport->set_body_hidden(false);
            m_viewport->repaint_now();
            mate_ghost = false;
        };
        menu.Bind(wxEVT_MENU_HIGHLIGHT,
                  [this, cs_a, cs_b, opts, mate_base, &mate_ghost, drop_ghost](wxMenuEvent& e) {
            const int i = e.GetMenuId() - (mate_base + 1);
            // Off the palette (a verb row, the header, or nothing) — a stale ghost from the row
            // you just left is worse than none, so it goes as soon as the cursor does.
            if (i < 0 || i >= int(opts.size()) || !opts[i].viable) { drop_ghost(); return; }
            std::string err;
            mate_ghost = show_mate_ghost(opts[i].kind, cs_a, cs_b, 0.0, 0.0, false, err);
            m_viewport->repaint_now();   // synchronous: the popup owns the loop, a queued repaint is never serviced
        });

        menu.Bind(wxEVT_MENU, [this, cs_a, cs_b, opts, mate_base, drop_ghost](wxCommandEvent& e) {
            const int i = e.GetId() - (mate_base + 1);
            if (i < 0 || i >= int(opts.size()) || !opts[i].viable) return;
            drop_ghost();         // the real bodies are about to become the ghost's pose
            m_doc.checkpoint();   // undo boundary: committing a mate from the offer
            int idx = m_doc.add_mate(opts[i].kind, cs_a, cs_b, 0.0, 0.0, false,
                                     "Mate" + std::to_string(++m_feature_counter));
            if (idx < 0) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                set_status(_L("Mate rejected"));
                m_status->Refresh();
                return;
            }
            if (!recompute_guarded(_L("Rebuilding model…")))
                set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
            else
                set_status_ok();
            refresh_tree();
        });
    }

    // Hovering a row explains it. The offer is the only door to these tools now, so a bare
    // name is not enough — and the hint arrives while you are still choosing.
    // BOUND TO THE VERB ID RANGE, NOT THE WHOLE MENU. These two used to be unfiltered, and
    // wxWidgets pushes dynamic entries to the FRONT of the handler list, so being bound LAST made
    // them run FIRST for every id — including the mate rows at mate_base+1 (base+501) above. Both
    // fall out of `bound`'s range there and return WITHOUT e.Skip(), which wx reads as "handled",
    // so the mate handlers never ran: hovering a mate kind showed no ghost and left the previous
    // status message on screen, and clicking one created nothing at all. The whole mate palette
    // enumerated perfectly and fired nothing. Restricting the range keeps each half to its own ids
    // regardless of bind order.
    menu.Bind(wxEVT_MENU_HIGHLIGHT, [this, &bound, base](wxMenuEvent& e) {
        const int i = e.GetMenuId() - base;
        if (i < 0 || i >= int(bound.size()) || bound[i] == nullptr || bound[i]->hint == nullptr)
            return;
        m_status->SetForegroundColour(wxNullColour);
        set_status(wxGetTranslation(wxString::FromUTF8(bound[i]->hint)));
        m_status->Update();   // the popup owns the loop; without this the line repaints late
    }, base, base + 499);   // 499: the mate section starts at base + 500 (see mate_base)
    menu.Bind(wxEVT_MENU, [this, &bound, base](wxCommandEvent& e) {
        const int i = e.GetId() - base;
        if (i >= 0 && i < int(bound.size()) && bound[i])
            run_offer_action(bound[i]->action);
    }, base, base + 499);   // 499: the mate section starts at base + 500 (see mate_base)
    PopupMenu(&menu, ScreenToClient(screen_pos));
    // PopupMenu is modal, so by here the menu is gone and any command it raised has already run.
    // A hover ghost that outlived the menu it belonged to would leave the committed bodies hidden
    // behind a preview nothing can dismiss — bind nothing to the close event, just clean up here.
    if (mate_ghost) {
        m_viewport->clear_preview();
        m_viewport->set_body_hidden(false);
        m_viewport->request_repaint();
    }
}

void DesignPanel::apply_plane_refs(CadFeature& f) const
{
    f.plane_type       = (PlaneType)m_plane_type->GetSelection();
    f.plane_face_body  = m_pl_faceA_body;  f.plane_face  = m_pl_faceA;
    f.plane_face2_body = m_pl_faceB_body;  f.plane_face2 = m_pl_faceB;
    f.plane_edge_body  = m_pl_edgeA_body;  f.plane_edge  = m_pl_edgeA;
    f.plane_edge2_body = m_pl_edgeB_body;  f.plane_edge2 = m_pl_edgeB;
    f.plane_u_size     = m_plane_usize->GetValue();
    f.plane_v_size     = m_plane_vsize->GetValue();
}

void DesignPanel::refresh_plane_labels()
{
    auto txt = [](int idx) { return idx >= 0 ? wxString::Format("#%d", idx) : wxString(_L("(none)")); };
    if (m_plane_faceA_lbl) m_plane_faceA_lbl->SetLabel(txt(m_pl_faceA));
    if (m_plane_faceB_lbl) m_plane_faceB_lbl->SetLabel(txt(m_pl_faceB));
    if (m_plane_edgeA_lbl) m_plane_edgeA_lbl->SetLabel(txt(m_pl_edgeA));
    if (m_plane_edgeB_lbl) m_plane_edgeB_lbl->SetLabel(txt(m_pl_edgeB));
}

void DesignPanel::reset_plane_refs()
{
    m_pl_faceA_body = m_pl_faceA = -1;  m_pl_faceB_body = m_pl_faceB = -1;
    m_pl_edgeA_body = m_pl_edgeA = -1;  m_pl_edgeB_body = m_pl_edgeB = -1;
    m_plane_pick = PlanePick::None;
    if (m_viewport) m_viewport->set_escalate_on_repick(true);   // pick abandoned
    refresh_plane_labels();
}

void DesignPanel::arm_plane_pick(PlanePick target)
{
    m_plane_pick = target;
    // While a pick is armed, a click must CAPTURE the face under the cursor, never escalate to
    // the whole body. Without this, clicking a face that already happens to be selected reads as
    // a repeat pick, selects the body, and the capture is silently lost — the label stays
    // "(none)" and the user has no idea why. The capture path below already restores the flag,
    // and reset_plane_refs() restores it when the pick is abandoned; only the arm side was missing.
    if (m_viewport) m_viewport->set_escalate_on_repick(false);
    const bool face = (target == PlanePick::FaceA || target == PlanePick::FaceB);
    m_status->SetForegroundColour(wxNullColour);
    set_status(face ? _L("Click a solid FACE in the viewport")
                            : _L("Click a solid EDGE in the viewport"));
    m_status->Refresh();
}

// --- Axis helpers ---

void DesignPanel::apply_axis_refs(CadFeature& f) const
{
    f.axis_type = (AxisType)m_axis_type->GetSelection();
    f.axis_face = m_ax_face;
    f.axis_edge = m_ax_edge;
    f.axis_plane_a = m_axis_plane_a->GetSelection();
    f.axis_plane_b = m_axis_plane_b->GetSelection();
    f.axis_p1 = Vec3d(m_axis_p1x->GetValue(), m_axis_p1y->GetValue(), m_axis_p1z->GetValue());
    f.axis_p2 = Vec3d(m_axis_p2x->GetValue(), m_axis_p2y->GetValue(), m_axis_p2z->GetValue());
    f.axis_body = m_ax_face_body;
}

void DesignPanel::refresh_axis_labels()
{
    auto txt = [](int idx) { return idx >= 0 ? wxString::Format("#%d", idx) : wxString(_L("(none)")); };
    if (m_axis_face_lbl) m_axis_face_lbl->SetLabel(txt(m_ax_face));
    if (m_axis_edge_lbl) m_axis_edge_lbl->SetLabel(txt(m_ax_edge));
}

void DesignPanel::reset_axis_refs()
{
    m_ax_face_body = m_ax_face = -1;
    m_ax_edge = -1;
    m_axis_pick = AxisPick::None;
    if (m_viewport) m_viewport->set_escalate_on_repick(true);   // pick abandoned
    refresh_axis_labels();
}

void DesignPanel::arm_axis_pick(AxisPick target)
{
    m_axis_pick = target;
    if (m_viewport) m_viewport->set_escalate_on_repick(false);   // same as arm_plane_pick
    m_status->SetForegroundColour(wxNullColour);
    set_status(target == AxisPick::Face ? _L("Click a solid FACE in the viewport")
                                                : _L("Click a solid EDGE in the viewport"));
    m_status->Refresh();
}

// --- CoordSys helpers ---

void DesignPanel::apply_coordsys_refs(CadFeature& f) const
{
    f.coordsys_type  = (CoordSysType)m_coordsys_type->GetSelection();
    f.coordsys_point = Vec3d(m_cs_x->GetValue(), m_cs_y->GetValue(), m_cs_z->GetValue());
    f.coordsys_body  = m_cs_face_body;
    f.coordsys_face  = m_cs_face;
    f.coordsys_edge  = m_cs_edge;
    f.coordsys_x_hint = Vec3d(m_cs_hx->GetValue(), m_cs_hy->GetValue(), m_cs_hz->GetValue());
}

void DesignPanel::refresh_coordsys_labels()
{
    auto txt = [](int idx) { return idx >= 0 ? wxString::Format("#%d", idx) : wxString(_L("(none)")); };
    if (m_cs_face_lbl) m_cs_face_lbl->SetLabel(txt(m_cs_face));
    if (m_cs_edge_lbl) m_cs_edge_lbl->SetLabel(txt(m_cs_edge));
}

void DesignPanel::refresh_cs_body_choice()
{
    if (!m_cs_body) return;
    const int keep = m_cs_body->GetSelection();
    m_cs_body->Clear();
    m_cs_body->Append(_L("(all)"));
    for (size_t b = 0; b < m_doc.bodies.size(); ++b)
        m_cs_body->Append(wxString::Format(_L("Body %d"), int(b) + 1));
    const int sel = (keep > 0 && keep < int(m_cs_body->GetCount())) ? keep : 0;
    m_cs_body->SetSelection(sel);
    // The combo and the viewport focus are ONE state, so they must not be written separately.
    // When the body list shrinks, `keep` falls out of range and the selection silently drops to
    // "(all)" — while the viewport stayed focused on the old index, leaving every other body at
    // 25% alpha and picking locked to a body that may no longer exist. That is the exact mirror
    // of the open_tool ordering bug (combo says Body N, viewport opaque); this one says "(all)"
    // and stays dimmed.
    //
    // Guarded on the CoordSys card being the ACTIVE tool because it is the only card that owns
    // this focus. In the edit path this function runs BEFORE open_tool, with the previous tool
    // still active, so the guard is false and the caller's explicit set_xray_focus still wins.
    if (m_viewport != nullptr && m_active == Tool::CoordSys)
        m_viewport->set_xray_focus(sel - 1);
}

void DesignPanel::reset_coordsys_refs()
{
    m_cs_face_body = m_cs_face = -1;
    m_cs_edge = -1;
    m_coordsys_pick = CoordSysPick::None;
    if (m_viewport) m_viewport->set_escalate_on_repick(true);   // pick abandoned
    refresh_coordsys_labels();
    if (m_cs_body) m_cs_body->SetSelection(0);
    if (m_viewport) m_viewport->set_xray_focus(-1);
    refresh_cs_body_choice();
}

void DesignPanel::arm_coordsys_pick(CoordSysPick target)
{
    m_coordsys_pick = target;
    // While this pick is armed the click the card asked for must reach it, so the whole-body
    // escalation is off: clicking the face the card is pointing at is the ANSWER here, not a
    // request for its body.
    if (m_viewport) m_viewport->set_escalate_on_repick(false);
    m_status->SetForegroundColour(wxNullColour);
    set_status(target == CoordSysPick::Face ? _L("Click a solid FACE in the viewport")
                                                    : _L("Click a solid EDGE in the viewport"));
    m_status->Refresh();
}

bool DesignPanel::on_add_plane()
{
    // Refuse here rather than let the kernel substitute. Every method in CadDocument's plane
    // dispatch falls back to offset_angle_plane() when its references are missing, so picking
    // Tangent and confirming with nothing selected used to produce an offset plane reported as
    // a success — the user asks for one construction and silently receives another. The kernel
    // keeps its fallback (it must return SOMETHING), but no user gesture should reach it.
    auto refuse = [this](const wxString& why) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(why);
        m_status->Refresh();
    };
    switch ((PlaneType)m_plane_type->GetSelection()) {
    case PlaneType::Angle:
        if (m_pl_edgeA < 0) { refuse(_L("An angled plane needs an edge to tilt about — pick Edge A")); return false; }
        break;
    case PlaneType::Midplane:
        if (m_pl_faceA < 0 || m_pl_faceB < 0) { refuse(_L("A midplane needs two faces — pick Face A and Face B")); return false; }
        if (m_pl_faceA == m_pl_faceB && m_pl_faceA_body == m_pl_faceB_body) {
            // Well-defined but useless: the midplane of a face with itself is that same face.
            refuse(_L("Face A and Face B are the same face — a midplane needs two different faces"));
            return false;
        }
        break;
    case PlaneType::Tangent:
        // The face must also be cylindrical; that check stays in the kernel, which has the geometry.
        if (m_pl_faceA < 0) { refuse(_L("A tangent plane needs a cylindrical face — pick Face A")); return false; }
        break;
    case PlaneType::TwoEdges:
        if (m_pl_edgeA < 0 || m_pl_edgeB < 0) { refuse(_L("This plane needs two edges — pick Edge A and Edge B")); return false; }
        break;
    default:
        break;   // Offset and Coincident are meaningful with no reference: they use the base plane
    }

    m_feature_counter++;
    int idx = m_doc.add_plane(m_plane_base->GetSelection(), m_plane_offset->GetValue(),
                    m_plane_tilt->GetValue(), m_plane_tilt_axis->GetSelection(),
                    "Plane" + std::to_string(m_feature_counter));
    if (idx >= 0 && idx < int(m_doc.features.size())) apply_plane_refs(m_doc.features[idx]);
    m_doc.recompute();   // datum-only docs yield no body; that is expected/benign
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Plane added — pick it as a sketch plane"));
    refresh_tree();
    return true;
}

void DesignPanel::on_add_axis()
{
    m_feature_counter++;
    int idx = m_doc.add_axis((AxisType)m_axis_type->GetSelection(),
                             "Axis" + std::to_string(m_feature_counter));
    if (idx >= 0 && idx < int(m_doc.features.size())) apply_axis_refs(m_doc.features[idx]);
    m_doc.recompute();
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Axis added"));
    refresh_tree();
}

void DesignPanel::on_add_coordsys()
{
    m_feature_counter++;
    Vec3d pt(m_cs_x->GetValue(), m_cs_y->GetValue(), m_cs_z->GetValue());
    int idx = m_doc.add_coordsys((CoordSysType)m_coordsys_type->GetSelection(), pt,
                                 "Coord" + std::to_string(m_feature_counter));
    if (idx >= 0 && idx < int(m_doc.features.size())) apply_coordsys_refs(m_doc.features[idx]);
    m_doc.recompute();
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Coord Sys added"));
    refresh_tree();
}

void DesignPanel::on_add_shell()
{
    if (m_doc.body.IsNull()) {
        set_status(_L("Shell needs a solid body — add or import one first"));
        return;
    }
    const int face = (m_sel_solid_face >= 0) ? m_sel_solid_face : -1;

    m_feature_counter++;
    m_doc.add_shell(m_shell_thickness->GetValue(), face, m_sel_solid_body,
                    "Shell" + std::to_string(m_feature_counter));

    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

void DesignPanel::on_add_draft()
{
    if (m_doc.body.IsNull()) {
        set_status(_L("Draft needs a solid body — add or import one first"));
        return;
    }
    if (m_sel_solid_face < 0) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Draft needs a picked face — click a side face first"));
        m_status->Refresh();
        return;
    }

    m_feature_counter++;
    m_doc.add_draft(m_draft_angle->GetValue(), m_sel_solid_face, m_sel_solid_body,
                    "Draft" + std::to_string(m_feature_counter));

    if (!recompute_guarded(_L("Rebuilding model…")))
        set_status(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

int DesignPanel::tree_icon_for(CadFeatureType t)
{
    switch (t) {
    case CadFeatureType::Sketch:  return 0;
    case CadFeatureType::Extrude: return 1;
    case CadFeatureType::Fillet:
    case CadFeatureType::Chamfer: return 2;
    case CadFeatureType::Hole:    return 3;
    case CadFeatureType::Thread:  return 4;
    case CadFeatureType::Shell:   return 5;
    case CadFeatureType::Revolve: return 1;
    case CadFeatureType::Sweep:   return 1;
    case CadFeatureType::Pattern: return 1;
    case CadFeatureType::Plane:   return 0;   // datum plane: sketch-family icon
    case CadFeatureType::Loft:    return 1;
    case CadFeatureType::Draft:   return 5;   // dressup-family icon
    case CadFeatureType::Import:  return 1;   // imported solid: solid-family icon
    case CadFeatureType::Boolean: return 1;   // body-body combine: solid-family icon
    case CadFeatureType::Cut:     return 1;   // plane split: solid-family icon
    case CadFeatureType::Axis:    return 0;   // datum axis: sketch-family icon
    case CadFeatureType::CoordSys: return 0;  // datum coord sys: sketch-family icon
    case CadFeatureType::SurfaceExtrude:  return 1;
    case CadFeatureType::SurfaceRevolve:  return 1;
    case CadFeatureType::SurfaceLoft:     return 1;
    case CadFeatureType::SurfaceFill:     return 1;
    case CadFeatureType::ThickenSurface:  return 1;
    case CadFeatureType::SurfaceOffset:   return 1;
    case CadFeatureType::Transform:       return 1;   // solid-family icon
    case CadFeatureType::Mirror:          return 1;   // solid-family icon
    case CadFeatureType::Thicken:         return 1;   // solid-family icon
    case CadFeatureType::Rib:             return 1;   // solid-family icon
    case CadFeatureType::Project:         return 0;   // sketch-family icon (produces sketch)
    case CadFeatureType::DeleteFace:      return 5;   // dressup-family icon
    case CadFeatureType::Helix:           return 4;   // thread-family icon (curve)
    case CadFeatureType::Mate:            return 2;   // dressup-family icon (assembly)
    }
    return 0;
}

// The reason detect_mate_conflicts() recorded for this feature, or nullptr. A linear scan: an
// assembly has a handful of mates and at most that many conflicts, so a map would cost more to
// build than the scans it saves.
const std::string* DesignPanel::mate_conflict_reason(int feature) const
{
    for (const auto& c : m_doc.mate_conflicts)
        if (c.first == feature) return &c.second;
    return nullptr;
}

// What to say when nothing is selected and no tool is open. Lives in one place because it is
// needed from two: after an edit empties the document, and at startup — where after_tree_edit()
// has never run, which is exactly why a fresh tab used to show a blank line.
wxString DesignPanel::idle_hint() const
{
    return m_doc.features.empty()
        ? _L("Nothing yet — import a STEP or a mesh from the toolbar,\n"
             "or click a reference plane and right-click it to start a sketch.")
        : _L("No solid yet — select a sketch and right-click it to Extrude.");
}

// The Design tab is no longer the visible page (snaporca-dlj). The status line is a popup floating
// over the GL canvas, so it does NOT go away when this page does — it stayed up over Prepare and
// over the home screen, still reading like a live Design selection ("selected (whole body) —
// right-click for what applies to it") on a tab that has no such selection and no such menu.
// Nothing else in the panel needs to know: the popup keeps its text and comes straight back.
void DesignPanel::on_tab_hidden()
{
    if (m_viewport) m_viewport->show_status_hud(false);
}

void DesignPanel::on_tab_shown()
{
    if (m_viewport) m_viewport->show_status_hud(true);   // ...and back on the way in

    if (m_active == Tool::None && m_doc.display_mesh.its.indices.empty())
        set_status(idle_hint());   // first paint: the tab has never been edited

    if (m_viewport) m_viewport->refresh_bed();

    // Modeling origin = bed centre, set BEFORE any recompute/datum-resolve so sketches and datums
    // land in the middle of the bed (not the bed corner = world 0).
    if (Plater* pl = wxGetApp().plater()) {
        const Vec2d bc = pl->build_volume().bed_center();
        m_doc.modeling_origin = Vec3d(bc.x(), bc.y(), 0.0);
    }

    // Rehydrate the parametric model from a freshly loaded project (the 3MF carried the
    // recipe in Metadata/SnapOrca_cad.bin). Only when nothing is in progress here, so we
    // never clobber an active design when the user just toggles back to the Design tab.
    if (m_doc.features.empty()) {
        if (Plater* plater = wxGetApp().plater()) {
            const std::string& blob = plater->model().cad_recipe;
            if (!blob.empty()) load_recipe(blob);
        }
    }
    update_reference_planes();   // entering the Design tab: show the XY/XZ/YZ planes if no object yet
    sync_sidebar_width();        // keep the panel as wide as Prepare's so the canvas edge doesn't jump
    if (m_viewport) m_viewport->force_repaint();   // the page was just re-shown: paint it for real
}

// Match Prepare's sidebar width instead of hardcoding one. Design used a fixed 264 px against
// Prepare's ~467, so the canvas edge jumped sideways on every tab switch; reading the live width
// also means the two stay aligned if Orca ever changes its sidebar.
void DesignPanel::sync_sidebar_width()
{
    if (m_form == nullptr) return;
    Plater* pl = wxGetApp().plater();
    if (pl == nullptr) return;
    const int w = pl->sidebar().GetSize().GetWidth();
    if (w < 200) return;                       // sidebar not laid out yet — keep what we have
    if (m_form->GetMinSize().GetWidth() == w) return;
    m_form->SetMinSize(wxSize(w, -1));
    Layout();
}

void DesignPanel::load_recipe(const std::string& blob)
{
    if (blob.empty()) return;
    if (!m_doc.deserialize_recipe(blob)) {
        // Carry the kernel's reason. deserialize_recipe distinguishes three cases that matter
        // very differently to the person reading this — saved by a NEWER build, saved by an
        // OLDER one, or genuinely unreadable — and replacing all three with one sentence left
        // the user unable to tell "update SnapOrca" from "your file is damaged". Same
        // error-loss class as the 31 McpControl sites (1de72de9ed): the message exists, it was
        // simply not passed on.
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(m_doc.error.empty()
                   ? _L("Could not restore the CAD model from this project")
                   : _L("Could not restore the CAD model: ") + wxString::FromUTF8(m_doc.error));
        m_status->Refresh();
        return;
    }
    m_feature_counter = int(m_doc.features.size());
    feed_bodies();    // push the restored bodies into the viewport
    refresh_tree();   // rebuild the feature tree from the restored recipe
    set_status_ok();
}

void DesignPanel::refresh_tree()
{
    // Preserve the selected row across the rebuild — wxTreeCtrl::DeleteAllItems
    // drops the selection, which made every edit/add feel like it "lost" the
    // selection (and broke Edit/Move/Delete on the just-touched feature).
    const int keep = tree_selection();

    m_tree->DeleteAllItems();
    m_tree_items.clear();
    m_tree_body_items.clear();
    wxTreeItemId root = m_tree->AddRoot("root");
    // Datum/reference planes carry no solid; feed them to the viewport so they render as
    // translucent rectangles (otherwise a Plane feature is invisible in the canvas).
    refresh_datum_planes();
    update_reference_planes();   // body added/removed -> show/hide the XY/XZ/YZ origin planes
    for (size_t fi = 0; fi < m_doc.features.size(); ++fi) {
        const CadFeature& f = m_doc.features[fi];
        const int img = tree_icon_for(f.type);
        wxTreeItemId id = m_tree->AppendItem(root, wxString::FromUTF8(f.name), img, img);
        // Three states, in this order of precedence:
        //   disabled  -> dim. A SUPPRESSED mate is the user's answer to a conflict, so it must
        //                read as suppressed rather than keep shouting about the conflict.
        //   conflict  -> warn. detect_mate_conflicts() refills m_doc.mate_conflicts on every
        //                recompute; the mark goes on the row that carries it, because the tree
        //                is where the user is already looking for which feature to change.
        //   otherwise -> normal.
        // A conflict is NOT a document error — the document still evaluates — so the row is
        // marked and never hidden, and the reason goes to the status line on selection rather
        // than into a modal that interrupts without offering an action.
        m_tree->SetItemTextColour(id, !f.enabled                            ? dp_item_dim()
                                    : mate_conflict_reason(int(fi)) != nullptr ? wxColour(235, 110, 110)
                                                                               : dp_item_text());
        m_tree_items.push_back(id);
    }
    refresh_parts();   // bodies live in their own list below the tree, never clipped by history
    if (keep >= 0 && keep < int(m_tree_items.size()))
        m_tree->SelectItem(m_tree_items[keep]);

    // Size the tree to its content (clamped) so it doesn't waste a fixed-height block when
    // there are few features, and scrolls internally past ~9 rows instead of growing forever.
    const int rows  = int(m_tree_items.size());   // bodies are in their own list now
    const int rowH  = std::max(m_tree->GetCharHeight() + 8, 20);
    const int shown = std::min(std::max(rows, 1), 9);
    const wxSize ts(-1, shown * rowH + 8);
    m_tree->SetMinSize(ts);
    m_tree->SetMaxSize(ts);
    if (m_form && m_form->GetSizer()) { update_cards_frame(); m_form->Layout(); m_form->FitInside(); }
}

// Rebuild the Parts list from the document's bodies, preserving the selected row so a
// recompute (fillet, hole, ...) doesn't drop the user's body selection under them.
void DesignPanel::refresh_parts()
{
    if (m_parts == nullptr) return;
    const int keep = tree_body_selection();

    m_parts->DeleteAllItems();
    m_tree_body_items.clear();
    wxTreeItemId proot = m_parts->AddRoot("root");

    sync_body_visible();   // keep flags parallel before reading them for the row colour
    for (size_t b = 0; b < m_doc.bodies.size(); ++b) {
        // Label "Body N" (matches the viewport/status); the originating feature name is kept
        // on the CadBody for tooltips/debug but isn't shown as the row label.
        const bool vis = b >= m_body_visible.size() || m_body_visible[b];
        wxTreeItemId id = m_parts->AppendItem(proot, wxString::Format(_L("Body %zu"), b + 1));
        // Hidden bodies are greyed so the show/hide state reads at a glance (eye toggle).
        m_parts->SetItemTextColour(id, vis ? dp_item_text() : dp_item_dim());
        m_tree_body_items.push_back(id);
    }

    // Hide the whole block until there is something to list, so an empty document doesn't
    // show a stray empty box.
    const bool any = !m_tree_body_items.empty();
    m_parts->Show(any);
    if (m_parts_label) m_parts_label->Show(any);
    if (m_parts_hdr)   m_parts_hdr->ShowItems(any);   // icon + title live in this sizer
    if (m_parts_rule)  m_parts_rule->Show(any);
    // ...and the frame with it, or an empty bordered box floats there.
    if (m_parts_box && m_form && m_form->GetSizer()) m_form->GetSizer()->Show(m_parts_box, any, false);

    if (any) {
        const int rowH  = std::max(m_parts->GetCharHeight() + 8, 20);
        const int shown = std::min(int(m_tree_body_items.size()), 6);   // scrolls past 6
        const wxSize ps(-1, shown * rowH + 8);
        m_parts->SetMinSize(ps);
        m_parts->SetMaxSize(ps);
        if (keep >= 0 && keep < int(m_tree_body_items.size()))
            m_parts->SelectItem(m_tree_body_items[keep]);
    }
    if (m_form && m_form->GetSizer()) { update_cards_frame(); m_form->Layout(); m_form->FitInside(); }
}

int DesignPanel::tree_body_selection() const
{
    if (m_parts == nullptr) return -1;
    const wxTreeItemId sel = m_parts->GetSelection();
    if (!sel.IsOk()) return -1;
    for (size_t i = 0; i < m_tree_body_items.size(); ++i)
        if (m_tree_body_items[i] == sel) return int(i);
    return -1;
}

void DesignPanel::update_section_flip_btn()
{
    if (m_section_flip_btn) m_section_flip_btn->Enable(m_section_on);
}

void DesignPanel::toggle_section_view()
{
    if (!m_viewport) return;
    m_section_on = !m_section_on;
    m_status->SetForegroundColour(wxNullColour);
    if (m_section_on) {
        m_section_cut_z = m_viewport->model_mid_z();   // start at the model's mid-height
        m_section_upper = false;                       // keep the lower half by default
        m_viewport->set_section_plane(true, m_section_cut_z, m_section_upper);
        set_status(_L("Section view on — hides half the model to see inside; "
                              "PageUp / PageDown move the plane, Flip shows the other half"));
    } else {
        m_viewport->set_section_plane(false, 0.0);
        set_status(_L("Section view off"));
    }
    m_status->Refresh();
    update_section_flip_btn();
}

void DesignPanel::flip_section_view()
{
    if (!m_viewport || !m_section_on) return;
    m_section_upper = !m_section_upper;
    m_viewport->set_section_plane(true, m_section_cut_z, m_section_upper);
    m_status->SetForegroundColour(wxNullColour);
    set_status(wxString::Format(_L("Section view — showing the %s half"),
        m_section_upper ? _L("upper") : _L("lower")));
    m_status->Refresh();
}

void DesignPanel::sync_body_visible()
{
    // Keep the visibility vector parallel to bodies; newly-created bodies default visible.
    // Bodies are appended in feature order, so existing indices keep their flag on resize.
    m_body_visible.resize(m_doc.bodies.size(), true);
}

void DesignPanel::sync_body_xform()
{
    // Parallel to bodies; new bodies default to identity (no move). Stable on resize.
    m_body_xform.resize(m_doc.bodies.size(), Transform3d::Identity());
}

// Build the display + pick meshes with each body's Move transform applied. The pick mesh is
// re-merged from the transformed per-body meshes IN THE SAME body order as tessellate_bodies,
// so display_tri_face/display_tri_body stay aligned. m_disp_pick_mesh keeps a stable address —
// the tool holds a pointer to it, so an in-place rebuild updates picking without re-pointing.
void DesignPanel::rebuild_disp_meshes()
{
    sync_body_visible();
    sync_body_xform();
    const std::vector<TriangleMesh>& src = m_doc.display_body_meshes;

    bool any = false;
    for (const Transform3d& t : m_body_xform)
        if (!t.isApprox(Transform3d::Identity())) { any = true; break; }

    if (!any) {                          // no body moved: identical to the untransformed meshes
        m_disp_body_meshes = src;
        m_disp_pick_mesh   = m_doc.display_mesh;
        return;
    }

    m_disp_body_meshes.clear();
    m_disp_body_meshes.reserve(src.size());
    m_disp_pick_mesh = TriangleMesh{};
    for (size_t b = 0; b < src.size(); ++b) {
        TriangleMesh m = src[b];
        if (b < m_body_xform.size()) m.transform(m_body_xform[b]);
        m_disp_pick_mesh.merge(m);       // same order as tessellate_bodies -> tri_* stay aligned
        m_disp_body_meshes.push_back(std::move(m));
    }
}

void DesignPanel::feed_bodies()
{
    // The body count just changed, so the tools that consume a body may have become reachable or
    // unreachable. Before the early return below: the gating is about the toolbar, not the canvas.
    update_body_gates();
    // Rebuild the transformed meshes first so every display-refresh path (recompute, tint,
    // visibility, live move) shows the bodies at their current Move offsets. The solid-pick
    // keeps a STABLE pointer to m_disp_pick_mesh / m_body_visible / m_body_xform (rebuilt in
    // place), so it needs no re-call here — the whole/face/edge selection survives a move drag.
    if (m_viewport == nullptr) return;
    rebuild_disp_meshes();
    m_viewport->set_bodies(m_disp_body_meshes, m_body_visible);
}

void DesignPanel::on_move_body()
{
    const int b = m_sel_solid_body;
    if (m_viewport == nullptr) return;
    if (b < 0 || b >= int(m_doc.display_body_meshes.size())) {
        // Never fail silently here: the caller gates on bodies.size() while this needs a
        // tessellated per-body mesh, and when those disagreed the click did nothing at all.
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(b < 0 ? _L("Select a body first — click it in the viewport or the Bodies list")
                                 : _L("That body has no display mesh yet — recompute first"));
        m_status->Refresh();
        return;
    }
    sync_body_xform();
    // Delta gizmo: pivot at the body's CURRENT world centroid; the tool composes the drag deltas
    // onto its current pose, so move + rotate both work (incl. on an already place-on-face'd body).
    const Transform3d base = m_body_xform[b];
    const BoundingBoxf3 bb = m_doc.display_body_meshes[b].bounding_box();
    const Vec3d pivot = base * bb.center();
    // Bounding-sphere radius (world): the gizmo scales with it so the rotation rings sit clear of
    // the body instead of collapsing into a tangle inside it — same sizing rule as Orca's Prepare
    // gizmos. The scale part of `base` is applied so a scaled body still gets a correct radius.
    const double radius = (base.linear() * (bb.size() * 0.5)).norm();
    m_viewport->begin_move_body(b, pivot, base, radius);
    m_move_body = b;          // for the action bar: Cancel reverts to this pose
    m_move_prev = base;
    // Reset the numeric fields to "no change" and show the card beside the drag gizmo.
    if (m_move_dx) m_move_dx->SetValue(0.0);
    if (m_move_dy) m_move_dy->SetValue(0.0);
    if (m_move_dz) m_move_dz->SetValue(0.0);
    if (m_move_angle) m_move_angle->SetValue(0.0);
    show_move_card(true);
    update_action_bar();      // surface the unified ✓/✗ while moving
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Drag the arrows to move, the rings to rotate — then Confirm (Esc cancels)"));
    m_status->Refresh();
}

// Transform card (add mode): arm the same move gizmo on the card's body so the geometry-first
// control is what the user drags, while the card's numeric fields mirror the result. The card
// stays visible as the typed half — the gizmo owns the drag, the fields own the numbers.
void DesignPanel::arm_transform_gizmo()
{
    int b = tree_body_selection();
    if (b < 0) b = m_sel_solid_body;
    if (b < 0 && m_xf_body) b = m_xf_body->GetSelection();
    if (b < 0 || b >= int(m_doc.display_body_meshes.size())) return;   // card alone still works
    if (m_xf_body && b < int(m_xf_body->GetCount())) m_xf_body->SetSelection(b);   // feature must be created against the gizmo's body
    sync_body_xform();
    const Transform3d base = m_body_xform[b];
    const BoundingBoxf3 bb = m_doc.display_body_meshes[b].bounding_box();
    const Vec3d pivot = base * bb.center();
    const double radius = (base.linear() * (bb.size() * 0.5)).norm();
    if (m_viewport) m_viewport->begin_move_body(b, pivot, base, radius);
    m_xf_gizmo_body = b;
    m_xf_gizmo_base = base;
    m_move_body = b;          // the existing revert paths key off these two
    m_move_prev = base;
    // Write the pivot into the card so the parametric feature reproduces what was dragged;
    // dx/dy/dz and angle start at zero (the gizmo reports deltas from this pose).
    if (m_xf_pivot_x) m_xf_pivot_x->SetValue(pivot.x());
    if (m_xf_pivot_y) m_xf_pivot_y->SetValue(pivot.y());
    if (m_xf_pivot_z) m_xf_pivot_z->SetValue(pivot.z());
    if (m_xf_dx) m_xf_dx->SetValue(0.0);
    if (m_xf_dy) m_xf_dy->SetValue(0.0);
    if (m_xf_dz) m_xf_dz->SetValue(0.0);
    if (m_xf_angle) m_xf_angle->SetValue(0.0);
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Drag the arrows to move, the rings to rotate — the numbers follow"));
    m_status->Refresh();
}

// Color tool: open a colour picker on the selected body and store a per-body display-colour
// override on its CadBody. The override is carried across recompute() by body index and is
// read back by DesignCanvas::body_color()/reload(), so the body keeps its colour through edits.
void DesignPanel::on_set_body_color()
{
    // Same body-selection source Move / visibility use: the Parts-list row first, falling
    // back to the in-canvas picked solid so either selection path works.
    int b = tree_body_selection();
    if (b < 0) b = m_sel_solid_body;
    if (b < 0 || b >= int(m_doc.bodies.size())) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Select a body first"));
        m_status->Refresh();
        return;
    }

    // Seed the picker with the body's current effective colour (override or auto palette).
    const ColorRGBA cur = (m_viewport != nullptr) ? m_viewport->body_color(b)
                                                  : m_doc.bodies[b].color;
    wxColourData data;
    data.SetColour(wxColour(cur.r_uchar(), cur.g_uchar(), cur.b_uchar()));
    wxColourDialog dlg(this, &data);
    if (dlg.ShowModal() != wxID_OK) return;

    const wxColour picked = dlg.GetColourData().GetColour();
    m_doc.bodies[b].has_color = true;
    m_doc.bodies[b].color = ColorRGBA((unsigned char)picked.Red(), (unsigned char)picked.Green(),
                                      (unsigned char)picked.Blue(), (unsigned char)255);
    feed_bodies();   // same refresh path the visibility toggle uses → viewport updates immediately

    m_status->SetForegroundColour(wxNullColour);
    set_status(wxString::Format(_L("Body %d colour set"), b + 1));
    m_status->Refresh();
}

// Prepare's "Place on Face" (F), ported to Design. Pick a body face, then this rotates the
// body so that face's outward normal points straight down (-Z) and drops it onto the bed —
// Orca's exact math (Selection::flattening_rotate). Writes the per-body display transform
// m_body_xform (baked into the mesh at Commit), like the Move gizmo; no shape mutation.
// Returns false (with a hint) when no body face is selected, so the F key can fall through.
bool DesignPanel::place_on_face()
{
    const int b = m_sel_solid_body;
    if (b < 0 || b >= int(m_doc.bodies.size()) || m_sel_solid_face < 0
        || b >= int(m_doc.display_body_meshes.size())) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Click a face on the solid, then press F"));
        m_status->Refresh();
        return false;
    }
    const TopoDS_Face face = GeometryEngine::face_by_index(m_doc.bodies[b].shape, m_sel_solid_face);
    if (face.IsNull()) return false;
    sync_body_xform();
    const Transform3d old_x = m_body_xform[b];
    // Outward face normal in the body's CURRENT displayed orientation.
    const Vec3d n = (old_x.linear() * GeometryEngine::face_normal_world(face)).normalized();
    if (!n.allFinite() || n.norm() < 0.5) return false;
    // Align that normal with the down vector (-Z): the face ends up on the bed.
    const Transform3d R(Eigen::Quaterniond().setFromTwoVectors(n, -Vec3d::UnitZ()));
    // Rotate about the body's current world centroid so it spins in place, not about the origin.
    const Vec3d c = old_x * m_doc.display_body_meshes[b].bounding_box().center();
    Transform3d x = Eigen::Translation3d(c) * R * Eigen::Translation3d(-c) * old_x;
    // Drop the re-oriented body so its lowest point sits on the bed (min Z -> 0).
    TriangleMesh probe = m_doc.display_body_meshes[b];
    probe.transform(x);
    x = Transform3d(Eigen::Translation3d(0.0, 0.0, -probe.bounding_box().min.z())) * x;
    m_body_xform[b] = x;
    set_status_ok();   // rebuild display/pick meshes, re-point picking; resets face selection
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Placed on face — body laid flat on the bed"));
    m_status->Refresh();
    return true;
}

int DesignPanel::tree_selection() const
{
    const wxTreeItemId sel = m_tree->GetSelection();
    if (!sel.IsOk()) return wxNOT_FOUND;
    for (size_t i = 0; i < m_tree_items.size(); ++i)
        if (m_tree_items[i] == sel) return int(i);
    return wxNOT_FOUND;
}

void DesignPanel::set_tree_selection(int row)
{
    if (row >= 0 && row < int(m_tree_items.size()))
        m_tree->SelectItem(m_tree_items[row]);
}

void DesignPanel::after_tree_edit(bool ok)
{
    update_undo_redo_buttons();
    refresh_tree();
    refresh_variables();
    if (!ok) {
        // The edit was rolled back (recompute failed); the body is unchanged.
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Edit rejected: ") + wxString::FromUTF8(m_doc.error));
        m_status->Refresh();
        return;
    }
    sync_recipe_to_model();   // deletes, reorders and suppressions change the document too
    m_status->SetForegroundColour(wxNullColour);
    if (m_doc.display_mesh.its.indices.empty()) {
        if (m_viewport != nullptr) m_viewport->clear_mesh();
        sync_sketch_display();   // empty body: show any un-consumed committed sketch
        // An empty document used to blank this line — no guidance at the one moment a newcomer
        // has none. Name the three real ways in, in the order they are reachable on screen.
        set_status(idle_hint());
    } else {
        // nde #19/20: a delete/reorder that leaves bodies behind must re-feed the per-body
        // GLVolumes — otherwise the viewport keeps showing the pre-edit solid (the deleted
        // feature's artifact lingered). feed_bodies() is idempotent for the edit/replace path.
        if (m_viewport != nullptr) feed_bodies();
        set_status_ok();
    }
    // Force a frame: under software GL (llvmpipe on the :10 test box) reload()'s scheduled
    // Refresh() is dropped, so a deleted solid stayed on screen until the next orbit.
    if (m_viewport != nullptr) m_viewport->request_repaint();
    m_status->Refresh();
}

// Erase the whole document (every feature + body) and start fresh. The single "wipe" the
// feature tree's per-row Delete can't give you — also the way out when a body has no
// removable owning feature.
void DesignPanel::on_new_design()
{
    if (m_doc.features.empty() && m_doc.bodies.empty()) { set_status_ok(); return; }
    wxMessageDialog dlg(this,
        _L("Erase all features and bodies and start a new design? This cannot be undone."),
        _L("New Design"), wxYES_NO | wxICON_EXCLAMATION);
    if (dlg.ShowModal() != wxID_YES) return;
    tool_cancel();                 // leave any active tool / sketch / constrain cleanly
    m_doc.clear();                 // features + bodies + meshes + history
    m_edit_index = -1;
    m_move_body  = -1;
    show_move_card(false);
    m_body_xform.clear();
    if (m_viewport) { m_viewport->clear_move_gizmo(); m_viewport->clear_mesh(); }
    after_tree_edit(true);         // rebuild the (now empty) tree + clear the viewport
    update_action_bar();
    set_status_ok();
}

// The verb the offer names when you point at a body, or at any face/edge/vertex of one. A body
// is a recomputed RESULT, so what actually gets deleted is the feature that created it
// (CadBody::source_feature). That is an edit to the recipe and can take other features with it,
// so it asks first and NAMES the feature: a body disappearing from the viewport is not by itself
// evidence of which feature went, and this is the one action here that cannot be eyeballed.
void DesignPanel::on_delete_body()
{
    const int nb = int(m_doc.bodies.size());
    if (m_sel_solid_body < 0 || m_sel_solid_body >= nb) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Select a body first"));
        m_status->Refresh();
        return;
    }
    const int src = m_doc.bodies[m_sel_solid_body].source_feature;
    if (src < 0 || src >= int(m_doc.features.size())) {
        // Only reachable for a body no feature claims — a stale recipe, or a feature type that
        // broke the "never replace a whole CadBody" invariant recompute() relies on.
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("This body has no feature to delete — use New Design to start over"));
        m_status->Refresh();
        return;
    }
    const std::string& raw = m_doc.features[src].name;
    const wxString fname = raw.empty() ? wxString::Format(_L("feature %d"), src + 1)
                                       : wxString::FromUTF8(raw);
    if (wxMessageBox(wxString::Format(
                         _L("Delete %s?\n\nThat is the feature this body was made from. "
                            "Features built on it may be removed or stop working."), fname),
                     _L("Delete body"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
        return;
    // A card left open over a feature that is about to vanish goes stale — same reason
    // on_delete_feature() closes it.
    if (m_active != Tool::None || m_edit_index >= 0) {
        reset_edit_state();
        close_tool();
    }
    m_doc.checkpoint();   // undo boundary: deleting a body's feature
    m_sel_solid_body = m_sel_solid_face = m_sel_solid_edge = -1;   // the selection is about to
    m_sel_solid_vertex = false;                                    // name a body that is gone
    after_tree_edit(m_doc.remove_feature(src));
}

void DesignPanel::on_delete_feature()
{
    // A Body row has no directly-removable feature (bodies are recomputed results); guide the
    // user to delete the feature that created it, or use New Design to wipe everything.
    if (tree_body_selection() >= 0) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Select the FEATURE that created this body (or use New Design)"));
        m_status->Refresh();
        return;
    }
    int sel = tree_selection();
    if (sel == wxNOT_FOUND) {
        set_status(_L("Select a feature in the tree first"));
        m_status->Refresh();
        return;
    }
    // If a feature dialog is open (e.g. the feature is being edited), dismiss it first —
    // otherwise the deleted feature's settings card lingers in the left panel, out of sync
    // with the tree. reset_edit_state() drops the stale m_edit_index; close_tool() hides the card.
    if (m_active != Tool::None || m_edit_index >= 0) {
        reset_edit_state();
        close_tool();
    }
    m_doc.checkpoint();   // undo boundary: deleting a feature
    after_tree_edit(m_doc.remove_feature(sel));
}

void DesignPanel::on_toggle_visibility()
{
    // A selected Body row toggles that body's visibility (per-body show/hide). The solid
    // stays in the document; only its GLVolume + pickability flip. Falls through to the
    // feature-level toggle below when a feature row (not a body row) is selected.
    const int bsel = tree_body_selection();
    if (bsel >= 0) {
        sync_body_visible();
        if (bsel < int(m_body_visible.size())) {
            const bool now_visible = !m_body_visible[bsel];
            m_body_visible[bsel] = now_visible;
            if (m_viewport != nullptr) {
                feed_bodies();   // flips is_active; m_solid_visible is a stable pointer (live)
                m_viewport->set_solid_pick(&m_doc.bodies, &m_disp_pick_mesh,
                                           &m_doc.display_tri_face, &m_doc.display_tri_body,
                                           &m_body_visible, &m_body_xform);
            }
            refresh_tree();
            // Keep the row selected for repeat toggles. m_parts, NOT m_tree: these ids belong
            // to the Bodies list, and handing a foreign item to the feature tree left the row
            // unselected — so the second press of the eye found tree_body_selection() == -1 and
            // fell through to the FEATURE-level branch below instead of un-hiding the body.
            if (m_parts != nullptr && bsel < int(m_tree_body_items.size()))
                m_parts->SelectItem(m_tree_body_items[bsel]);
            m_status->SetForegroundColour(wxNullColour);
            set_status(wxString::Format(now_visible ? _L("Body %d shown")
                                                            : _L("Body %d hidden"), bsel + 1));
            m_status->Refresh();
        }
        return;
    }

    int sel = tree_selection();
    if (sel == wxNOT_FOUND || sel >= int(m_doc.features.size())) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Select a feature in the tree first"));
        m_status->Refresh();
        return;
    }
    const bool shown = !m_doc.features[sel].enabled;
    m_doc.features[sel].enabled = shown;

    // recompute() reports an all-hidden / sketch-only document as false (no
    // solid to build), but that is a VALID state for hide — so clear the body
    // explicitly instead of letting after_tree_edit treat it as a rejected edit
    // (which would skip the overlay refresh, leaving hidden art on screen).
    if (!recompute_guarded(_L("Rebuilding model…"))) {
        m_doc.body         = TopoDS_Shape();
        m_doc.display_mesh = TriangleMesh{};
        m_doc.error.clear();
    }
    refresh_tree();                       // greys the row
    set_tree_selection(sel);              // keep the toggled feature selected
    if (m_viewport != nullptr) {
        if (m_doc.display_mesh.its.indices.empty()) m_viewport->clear_mesh();
        else                                        feed_bodies();
    }
    sync_sketch_display();                // skips the hidden sketch + direct-renders
    m_status->SetForegroundColour(wxNullColour);
    set_status(shown ? _L("Feature shown") : _L("Feature hidden"));
    m_status->Refresh();
}

void DesignPanel::on_move_feature(int delta)
{
    int sel = tree_selection();
    if (sel == wxNOT_FOUND) {
        set_status(_L("Select a feature in the tree first"));
        m_status->Refresh();
        return;
    }
    int target = sel + delta;
    if (target < 0 || target >= int(m_doc.features.size()))
        return; // already at the end
    m_doc.checkpoint();   // undo boundary: reordering a feature
    if (m_doc.move_feature(sel, delta)) {
        after_tree_edit(true);
        set_tree_selection(target); // keep the moved feature selected
    } else {
        after_tree_edit(false);
    }
}

// Commit the live sketch in place, then enter Constrain mode on the just-committed sketch.
// One-click bridge from the SKETCH toolbar: removes the "Finish -> find in tree -> select ->
// Constrain" friction, so the constraint palette + Trim/Extend are reachable mid-sketch.
bool DesignPanel::enter_constrain_inline()
{
    if (m_viewport && m_viewport->is_sketching())
        m_viewport->finish_sketch();   // synchronous: packages live entities+constraints -> Sketch
    const int sk = resolve_extrude_sketch();   // last/selected Sketch feature
    if (sk < 0) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Draw a sketch first, then Constrain"));
        m_status->Refresh();
        return false;
    }
    set_tree_selection(sk);            // tree drives on_begin_constrain / the constraint manager
    on_begin_constrain(sk);
    const bool entered = m_viewport &&
        (m_viewport->is_constraining() || m_viewport->is_constraining_entities());
    if (entered) set_ui_mode(UiMode::Constrain);
    return entered;
}

void DesignPanel::on_begin_constrain(int sel_override)
{
    int sel = (sel_override >= 0) ? sel_override : tree_selection();
    // Fall back to the sketch owning the region picked in the viewport. The offer menu reaches
    // this verb from a SkLoop selection (a region clicked on screen), which carries no tree
    // selection — without this, choosing "Constrain sketch" from the offer would answer
    // "Select a sketch in the tree first" about a sketch the user has visibly selected.
    if ((sel == wxNOT_FOUND || sel >= int(m_doc.features.size())) && m_sel_sketch_feat >= 0
        && m_sel_sketch_feat < int(m_doc.features.size())) {
        sel = m_sel_sketch_feat;
        set_tree_selection(sel);       // keep the tree in step with what the viewport says
    }
    if (sel == wxNOT_FOUND || sel >= int(m_doc.features.size())) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Select a sketch in the tree first"));
        m_status->Refresh();
        return;
    }
    CadFeature& f = m_doc.features[sel];
    if (f.type != CadFeatureType::Sketch) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Selected feature is not a sketch"));
        m_status->Refresh();
        return;
    }

    // Entity sketches (Fase 4.2): pick Line entities; constraints solve against
    // entity endpoints in the kernel.
    if (!f.entities.empty()) {
        m_constrain_feat = sel;
        if (m_viewport) m_viewport->begin_constrain_entities(f.entities, f.plane);
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("Pick 1-2 lines, then a constraint; right-click exits"));
        m_status->Refresh();
        return;
    }

    // Legacy profile path (Fase 3).
    if (f.profile.points.size() < 3) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Selected feature is not a sketch"));
        m_status->Refresh();
        return;
    }
    m_constrain_feat = sel;
    // Anchor the first profile point so H/V constraints don't let the sketch
    // float freely; fix_point captures the point's current position in the solver.
    if (f.constraints.empty())
        f.constraints.push_back(SketchConstraintDef{SketchConstraintType::Fix, 0, -1, -1, -1, 0.0});
    if (m_viewport) m_viewport->begin_constrain(f.profile, f.plane);
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Pick 1-2 entities, then a constraint or dimension; right-click exits"));
    m_status->Refresh();
}

void DesignPanel::apply_entity_constraint(SketchConstraintType type)
{
    using R = SketchPointRole;
    using T = SketchConstraintType;
    int e0 = -1, e1 = -1;
    m_viewport->selected_constrain_entities(e0, e1);

    auto fail = [this](const wxString& msg) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(msg);
        m_status->Refresh();
    };

    CadFeature& feat = m_doc.features[m_constrain_feat];
    const bool needs_two = (type == T::Parallel || type == T::Perpendicular ||
                            type == T::EqualLength || type == T::Coincident ||
                            type == T::Concentric || type == T::Tangent ||
                            type == T::Angle || type == T::Midpoint ||
                            type == T::Symmetric);
    if (e0 < 0 || e0 >= int(feat.entities.size()) ||
        (needs_two && (e1 < 0 || e1 >= int(feat.entities.size())))) {
        fail(needs_two ? _L("Pick two entities first") : _L("Pick an entity first"));
        return;
    }
    auto is_round = [](const SketchEntity& e) {
        return e.type == SketchEntity::Type::Circle || e.type == SketchEntity::Type::Arc; };

    SketchEntityConstraintDef def;
    def.type  = type;
    def.value = 0.0;
    switch (type) {
    case T::Horizontal:
    case T::Vertical:
        // One line: level/plumb its own two endpoints.
        def.ea = e0; def.ra = R::P0;
        def.eb = e0; def.rb = R::P1;
        break;
    case T::Parallel:
    case T::Perpendicular:
    case T::EqualLength:
        def.ea = e0; def.eb = e1;   // two whole line segments (roles unused)
        break;
    case T::Coincident: {
        // Join the closest endpoint pair of the two picked lines.
        const SketchEntity& A = feat.entities[e0];
        const SketchEntity& B = feat.entities[e1];
        const std::pair<R, Vec2d> aps[2] = {{R::P0, A.p0}, {R::P1, A.p1}};
        const std::pair<R, Vec2d> bps[2] = {{R::P0, B.p0}, {R::P1, B.p1}};
        R ra = R::P1, rb = R::P0;
        double best = 1e30;
        for (const auto& ap : aps)
            for (const auto& bp : bps) {
                const double d = (ap.second - bp.second).squaredNorm();
                if (d < best) { best = d; ra = ap.first; rb = bp.first; }
            }
        def.ea = e0; def.ra = ra; def.eb = e1; def.rb = rb;
        break;
    }
    case T::Concentric: {
        // Two circles/arcs: make their centres coincide.
        if (!is_round(feat.entities[e0]) || !is_round(feat.entities[e1])) {
            fail(_L("Concentric needs two circles or arcs")); return;
        }
        def.ea = e0; def.ra = R::Center; def.eb = e1; def.rb = R::Center;
        break;
    }
    case T::Tangent: {
        // line+round or round+round; the kernel detects the entity types.
        const bool ok = (is_round(feat.entities[e0]) && feat.entities[e1].type == SketchEntity::Type::Line) ||
                        (is_round(feat.entities[e1]) && feat.entities[e0].type == SketchEntity::Type::Line) ||
                        (is_round(feat.entities[e0]) && is_round(feat.entities[e1]));
        if (!ok) { fail(_L("Tangent needs a line and a circle/arc, or two circles/arcs")); return; }
        def.ea = e0; def.eb = e1;
        break;
    }
    case T::Angle: {
        // Angle between two line segments; typed in-canvas at the cursor (no card),
        // pre-filled with the current angle between the picked lines.
        const int a = e0, b = e1;
        const Vec2d da = feat.entities[a].p1 - feat.entities[a].p0;
        const Vec2d db = feat.entities[b].p1 - feat.entities[b].p0;
        double cur = 90.0;
        const double na = da.norm(), nb = db.norm();
        if (na > 1e-9 && nb > 1e-9) {
            const double c = std::max(-1.0, std::min(1.0, da.dot(db) / (na * nb)));
            cur = std::acos(c) * 180.0 / M_PI;
        }
        m_viewport->open_inline_value(cur, [this, a, b](double deg) {
            SketchEntityConstraintDef d;
            d.type = T::Angle; d.ea = a; d.eb = b;
            d.value = deg * M_PI / 180.0;
            commit_entity_constraint(d);
        });
        return;   // deferred: commit runs on the typed value
    }
    case T::Midpoint: {
        // One pick is a Point, the other a Line: the point is the line's midpoint.
        const SketchEntity& A = feat.entities[e0];
        const SketchEntity& B = feat.entities[e1];
        int pt = -1, ln = -1;
        if (A.type == SketchEntity::Type::Point && B.type == SketchEntity::Type::Line) { pt = e0; ln = e1; }
        else if (B.type == SketchEntity::Type::Point && A.type == SketchEntity::Type::Line) { pt = e1; ln = e0; }
        else { fail(_L("Midpoint needs a point and a line")); return; }
        def.ea = pt; def.ra = R::P0; def.eb = ln;
        break;
    }
    case T::Symmetric: {
        // Two entities made symmetric about a third (axis) line. Picks: slot0=A,
        // slot1=B, slot2=axis. Two Points -> one pair; two Lines -> endpoint pairs.
        using ET = SketchEntity::Type;
        const int axis = m_viewport->selected_constrain_axis();
        if (axis < 0 || axis >= int(feat.entities.size()) ||
            feat.entities[axis].type != ET::Line) {
            fail(_L("Symmetric: pick two entities, then an axis line")); return;
        }
        const ET ta = feat.entities[e0].type, tb = feat.entities[e1].type;
        std::vector<SketchEntityConstraintDef> defs;
        auto mk = [&](R ra, R rb) {
            SketchEntityConstraintDef d;
            d.type = T::Symmetric;
            d.ea = e0; d.ra = ra; d.eb = e1; d.rb = rb; d.ec = axis;
            defs.push_back(d);
        };
        if (ta == ET::Point && tb == ET::Point) { mk(R::P0, R::P0); }
        else if (ta == ET::Line && tb == ET::Line) { mk(R::P0, R::P0); mk(R::P1, R::P1); }
        else { fail(_L("Symmetric needs two points or two lines + an axis")); return; }
        commit_entity_constraints(defs);
        return;   // multi-def commit done here
    }
    case T::Fix: {
        // Anchor the picked entity's reference point to its current coordinate (the
        // kernel pins it to a fixed reference). A single point — not both endpoints —
        // so it composes with any existing Horizontal/Vertical/length constraint
        // instead of duplicating it (pinning both endpoints of an already-horizontal
        // line is redundant → over-constrained). Removes 2 DoF (the entity's position);
        // combine with H/V + a dimension to reach fully constrained.
        using ET = SketchEntity::Type;
        const ET et = feat.entities[e0].type;
        def.ea = e0;
        def.ra = (et == ET::Circle || et == ET::Ellipse ||
                  et == ET::Arc    || et == ET::EllipseArc) ? R::Center : R::P0;
        break;
    }
    case T::Radius:
    case T::Diameter: {
        const SketchEntity& A = feat.entities[e0];
        if (!is_round(A)) { fail(_L("Radius/Diameter needs a circle or arc")); return; }
        const double cur = (type == T::Diameter) ? 2.0 * A.radius : A.radius;
        const int a = e0; const T tt = type;
        // Typed in-canvas at the cursor (no docked card), pre-filled with the current value.
        m_viewport->open_inline_value(cur, [this, a, tt](double v) {
            SketchEntityConstraintDef d;
            d.type = tt; d.ea = a; d.ra = R::Center; d.value = v;
            commit_entity_constraint(d);
        });
        return;   // deferred: commit runs on the typed value
    }
    default:
        fail(_L("Unsupported constraint"));
        return;
    }

    commit_entity_constraint(def);
}

void DesignPanel::commit_entity_constraint(const SketchEntityConstraintDef& def)
{
    commit_entity_constraints({ def });
}

void DesignPanel::commit_entity_constraints(const std::vector<SketchEntityConstraintDef>& defs)
{
    if (m_constrain_feat < 0 || m_constrain_feat >= int(m_doc.features.size()) ||
        !m_viewport || defs.empty())
        return;
    CadFeature& feat = m_doc.features[m_constrain_feat];
    // solve_sketch_feature rewrites entity coords even on failure, so snapshot
    // to roll back a rejected (over-constrained) addition cleanly. Multiple defs
    // (Symmetric on two lines) must solve together, so push all then resize back.
    const std::vector<SketchEntity> saved = feat.entities;
    const size_t before = feat.entity_constraints.size();
    for (const auto& d : defs) feat.entity_constraints.push_back(d);
    if (!m_doc.solve_sketch_feature(m_constrain_feat)) {
        feat.entity_constraints.resize(before);
        feat.entities = saved;
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Constraint rejected (over-constrained)"));
        m_status->Refresh();
        return;
    }
    m_doc.recompute();
    m_viewport->update_constrain_entities(m_doc.features[m_constrain_feat].entities);
    if (!m_doc.display_mesh.its.indices.empty())
        feed_bodies();
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Applied constraint"));
    m_status->Refresh();

    refresh_constrain_dof();      // P3 DoF readout for the Constrain path
    rebuild_constraint_list();    // C3.4 manager: a row appeared
}

// Re-derive the solve state of the constrained feature and mirror it into the same
// DoF readout the in-session path uses, so "✓ Fully constrained" is reachable here.
void DesignPanel::refresh_constrain_dof()
{
    if (!m_dof_status || m_constrain_feat < 0 || m_constrain_feat >= int(m_doc.features.size()))
        return;
    const CadFeature& feat = m_doc.features[m_constrain_feat];
    std::vector<SketchEntity> ents = feat.entities;   // already solved; re-solve is a cheap no-op
    const SketchSolveResult r = sketch_solve(ents, feat.entity_constraints);
    if (!r.ok) {
        m_dof_status->SetForegroundColour(wxColour(235, 80, 80));
        m_dof_status->SetLabel(_L("✗ Conflicting constraints")); m_dof_status->Show(!m_dof_status->GetLabel().IsEmpty());
    } else if (r.dof == 0) {
        m_dof_status->SetForegroundColour(wxColour(80, 200, 110));
        m_dof_status->SetLabel(_L("✓ Fully constrained")); m_dof_status->Show(!m_dof_status->GetLabel().IsEmpty());
    } else if (r.dof > 0) {
        m_dof_status->SetForegroundColour(dp_ctl_text());
        m_dof_status->SetLabel(wxString::Format(_L("%d degrees of freedom"), r.dof)); m_dof_status->Show(!m_dof_status->GetLabel().IsEmpty());
    } else {
        m_dof_status->SetLabel(wxString()); m_dof_status->Show(!m_dof_status->GetLabel().IsEmpty());
    }
    m_dof_status->Refresh();
    update_cards_frame(); m_form->Layout();
}

// Human-readable label for a constraint row, e.g. "Coincident L0·P1 — L1·P0",
// "Horizontal L2", "Radius C3 = 7.00". Entities are tagged by type letter + index.
wxString DesignPanel::constraint_label(const SketchEntityConstraintDef& d) const
{
    using T = SketchConstraintType;
    const CadFeature* feat = (m_constrain_feat >= 0 && m_constrain_feat < int(m_doc.features.size()))
                                 ? &m_doc.features[m_constrain_feat] : nullptr;
    auto tag = [&](int ei, SketchPointRole r) -> wxString {
        if (ei < 0) return wxString();
        char c = 'E';
        if (feat && ei < int(feat->entities.size())) {
            switch (feat->entities[ei].type) {
            case SketchEntity::Type::Line:       c = 'L'; break;
            case SketchEntity::Type::Circle:     c = 'C'; break;
            case SketchEntity::Type::Arc:        c = 'A'; break;
            case SketchEntity::Type::Point:      c = 'P'; break;
            case SketchEntity::Type::Ellipse:
            case SketchEntity::Type::EllipseArc: c = 'E'; break;
            case SketchEntity::Type::BSpline:    c = 'B'; break;
            }
        }
        wxString s; s << wxUniChar(c) << ei;   // avoid %c assert in Unicode build
        if (r == SketchPointRole::P1)     s += "·P1";
        else if (r == SketchPointRole::Center) s += "·Ctr";
        else if (r == SketchPointRole::P0)     s += "·P0";
        return s;
    };
    auto two = [&](const wxString& name) {
        return d.eb >= 0 ? wxString::Format("%s %s — %s", name, tag(d.ea, d.ra), tag(d.eb, d.rb))
                         : wxString::Format("%s %s", name, tag(d.ea, d.ra));
    };
    switch (d.type) {
    case T::Fix:           return wxString::Format(_L("Fix %s"), tag(d.ea, d.ra));
    case T::Coincident:    return two(_L("Coincident"));
    case T::Horizontal:    return two(_L("Horizontal"));
    case T::Vertical:      return two(_L("Vertical"));
    case T::Distance:      return wxString::Format("%s = %s", two(_L("Distance")), en_format(d.value));
    case T::LockX:         return wxString::Format(_L("Lock X %s"), tag(d.ea, d.ra));
    case T::LockY:         return wxString::Format(_L("Lock Y %s"), tag(d.ea, d.ra));
    case T::EqualLength:   return two(_L("Equal"));
    case T::Parallel:      return two(_L("Parallel"));
    case T::Perpendicular: return two(_L("Perpendicular"));
    case T::Concentric:    return two(_L("Concentric"));
    case T::Tangent:       return two(_L("Tangent"));
    case T::Midpoint:      return two(_L("Midpoint"));
    case T::Symmetric:     return wxString::Format(_L("Symmetric %s — %s / %s"),
                                                   tag(d.ea, d.ra), tag(d.eb, d.rb), tag(d.ec, d.rc));
    case T::Angle:         return wxString::Format("%s = %s°", two(_L("Angle")), en_format(d.value * 180.0 / M_PI, 1));
    case T::Radius:        return wxString::Format("%s %s = %s", _L("Radius"),   tag(d.ea, d.ra), en_format(d.value));
    case T::Diameter:      return wxString::Format("%s %s = %s", _L("Diameter"), tag(d.ea, d.ra), en_format(d.value));
    case T::PointOnLine:   return two(_L("On line"));
    case T::PointOnObject: return two(_L("On edge"));
    }
    return _L("Constraint");
}

// Rebuild the constraint-row list from the constrained feature's entity_constraints.
void DesignPanel::rebuild_constraint_list()
{
    if (m_constraint_rows == nullptr || m_form == nullptr)
        return;
    m_constraint_rows->Clear(true /* delete windows */);
    m_constraint_sel = -1;

    const bool active = (m_constrain_feat >= 0 && m_constrain_feat < int(m_doc.features.size()));
    const std::vector<SketchEntityConstraintDef> empty;
    const std::vector<SketchEntityConstraintDef>& cons =
        active ? m_doc.features[m_constrain_feat].entity_constraints : empty;

    if (m_hdr_constraints)
        m_hdr_constraints->SetLabel(wxString::Format(_L("Constraints (%d)"), int(cons.size())));

    if (cons.empty()) {
        auto* none = new wxStaticText(m_cards, wxID_ANY, _L("No constraints yet"));
        none->SetForegroundColour(dp_sec_text());
        m_constraint_rows->Add(none, 0, wxTOP, 4);
    }
    for (int i = 0; i < int(cons.size()); ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        // Delete button first (fixed left position, always visible — long labels can
        // horizontally scroll but ✗ stays put and clickable). BMP-safe ✗ glyph.
        auto* del = new wxButton(m_cards, wxID_ANY, wxString::FromUTF8("✗"),
                                 wxDefaultPosition, wxSize(26, -1));
        del->SetToolTip(_L("Delete constraint"));
        del->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { delete_constraint(i); });
        // Clickable label: selecting it highlights the referenced entities.
        auto* lbl = new wxButton(m_cards, wxID_ANY, constraint_label(cons[i]),
                                 wxDefaultPosition, wxDefaultSize, wxBU_LEFT | wxBORDER_NONE);
        lbl->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { highlight_constraint_entities(i); });
        row->Add(del, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(lbl, 1, wxALIGN_CENTER_VERTICAL);
        m_constraint_rows->Add(row, 0, wxEXPAND | wxTOP, 2);
    }

    // Feed the same list to the viewport for the on-sketch glyph badges (C3.4b).
    if (m_viewport)
        m_viewport->set_constraint_glyphs(cons);

    m_cards->GetSizer()->Show(m_box_constraints, m_ui_mode == UiMode::Constrain, true);
    update_cards_frame(); m_form->Layout();
    m_form->FitInside();
}

// Push the entities referenced by constraint `idx` to the viewport as a yellow
// highlight (toggle off if the same row is clicked again).
void DesignPanel::highlight_constraint_entities(int idx)
{
    if (!m_viewport || m_constrain_feat < 0 || m_constrain_feat >= int(m_doc.features.size()))
        return;
    const auto& cons = m_doc.features[m_constrain_feat].entity_constraints;
    if (idx < 0 || idx >= int(cons.size()))
        return;
    if (m_constraint_sel == idx) {     // second click clears
        m_constraint_sel = -1;
        m_viewport->set_constraint_highlight({});
        return;
    }
    m_constraint_sel = idx;
    const SketchEntityConstraintDef& d = cons[idx];
    std::vector<int> ents;
    for (int e : { d.ea, d.eb, d.ec })
        if (e >= 0) ents.push_back(e);
    m_viewport->set_constraint_highlight(std::move(ents));
}

// Drop constraint `idx`, re-solve the feature, and refresh viewport + list + DoF.
void DesignPanel::delete_constraint(int idx)
{
    if (m_constrain_feat < 0 || m_constrain_feat >= int(m_doc.features.size()) || !m_viewport)
        return;
    CadFeature& feat = m_doc.features[m_constrain_feat];
    if (idx < 0 || idx >= int(feat.entity_constraints.size()))
        return;
    feat.entity_constraints.erase(feat.entity_constraints.begin() + idx);
    // Re-solve the remaining system (deleting a constraint can only free DoF, so it
    // cannot fail for over-constraint; ignore the bool and refresh either way).
    m_doc.solve_sketch_feature(m_constrain_feat);
    m_doc.recompute();
    m_viewport->set_constraint_highlight({});
    m_viewport->update_constrain_entities(m_doc.features[m_constrain_feat].entities);
    if (!m_doc.display_mesh.its.indices.empty())
        feed_bodies();
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Constraint deleted"));
    m_status->Refresh();
    refresh_constrain_dof();
    rebuild_constraint_list();
}

void DesignPanel::apply_edit_op(EditOp op)
{
    if (m_constrain_feat < 0 || m_constrain_feat >= int(m_doc.features.size()) || !m_viewport ||
        !m_viewport->is_constraining_entities()) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Press Constrain on a sketch first"));
        m_status->Refresh();
        return;
    }
    auto fail = [this](const wxString& msg) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(msg);
        m_status->Refresh();
    };

    int e0 = -1, e1 = -1;
    m_viewport->selected_constrain_entities(e0, e1);
    CadFeature& feat = m_doc.features[m_constrain_feat];
    const int n = int(feat.entities.size());
    if (e0 < 0 || e0 >= n) { fail(_L("Pick an entity first")); return; }
    using Type = SketchEntity::Type;

    switch (op) {
    case EditOp::Mirror: {
        if (e1 < 0 || e1 >= n) { fail(_L("Pick the entity, then a mirror-axis line")); return; }
        const SketchEntity& axis = feat.entities[e1];
        if (axis.type != Type::Line) { fail(_L("Mirror axis must be a line")); return; }
        auto out = SketchEngine::mirror_entities({ feat.entities[e0] }, axis.p0, axis.p1);
        if (out.empty()) { fail(_L("Mirror produced nothing")); return; }
        const int mi = n;   // index the single mirrored copy lands at (n entities before push)
        for (auto& m : out) feat.entities.push_back(m);

        // C4a: bind the mirror to its source with Symmetric constraints about the
        // axis, so the pair stays mirror-symmetric under later solves and drags.
        // mirror_entities preserves P0/P1/Center ordering, so the constraints are
        // satisfied by construction; if the solver still rejects them (degenerate
        // axis, redundancy) keep the geometry and drop only the binding.
        {
            using R  = SketchPointRole;
            using CT = SketchConstraintType;
            const std::vector<SketchEntity> saved_ents = feat.entities;
            const size_t cons_before = feat.entity_constraints.size();
            SketchEntityConstraintDef d; d.type = CT::Symmetric; d.ea = e0; d.eb = mi; d.ec = e1;
            const Type st = feat.entities[e0].type;
            if (st == Type::Line) {
                d.ra = R::P0; d.rb = R::P0; feat.entity_constraints.push_back(d);
                d.ra = R::P1; d.rb = R::P1; feat.entity_constraints.push_back(d);
            } else if (st == Type::Arc || st == Type::Circle) {
                d.ra = R::Center; d.rb = R::Center; feat.entity_constraints.push_back(d);
            } else if (st == Type::Point) {
                d.ra = R::P0; d.rb = R::P0; feat.entity_constraints.push_back(d);
            }
            if (feat.entity_constraints.size() != cons_before &&
                !m_doc.solve_sketch_feature(m_constrain_feat)) {
                feat.entity_constraints.resize(cons_before);
                feat.entities = saved_ents;
            }
        }
        break;
    }
    case EditOp::Offset: {
        const int a = e0;
        request_value(_L("Offset distance (+left / -right of direction)"), 1.0, -100000.0, 100000.0,
            [this, a](double d) {
                CadFeature& f = m_doc.features[m_constrain_feat];
                if (a >= int(f.entities.size())) return;
                auto out = SketchEngine::offset_entities({ f.entities[a] }, d);
                if (out.empty()) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Offset collapsed the entity")); m_status->Refresh(); return;
                }
                const int ni = int(f.entities.size());   // offset copy lands here
                for (auto& o : out) f.entities.push_back(o);

                // C4c: bind the offset copy to its source. Offset only ADDS geometry
                // (the source is untouched), so unlike trim/fillet there are no stale
                // constraints to drop — just glue the pair. A line offset stays
                // Parallel to its source; an arc/circle offset stays Concentric (same
                // centre). Single constraint, so no degradation ladder; solve and roll
                // the binding back if the solver rejects it (keep the geometry).
                {
                    using CT = SketchConstraintType;
                    const Type st = f.entities[a].type;
                    SketchEntityConstraintDef d2; d2.ea = a; d2.eb = ni;
                    bool emit = true;
                    if (st == Type::Line)                              d2.type = CT::Parallel;
                    else if (st == Type::Arc || st == Type::Circle)    d2.type = CT::Concentric;
                    else                                               emit = false;
                    if (emit) {
                        const size_t cbefore = f.entity_constraints.size();
                        f.entity_constraints.push_back(d2);
                        if (!m_doc.solve_sketch_feature(m_constrain_feat))
                            f.entity_constraints.resize(cbefore);
                    }
                }
                after_edit_op();
            });
        return;   // deferred: edit runs on Confirm
    }
    case EditOp::Fillet: {
        if (e1 < 0 || e1 >= n) { fail(_L("Pick two lines to fillet")); return; }
        if (feat.entities[e0].type != Type::Line || feat.entities[e1].type != Type::Line) {
            fail(_L("Fillet needs two lines")); return;
        }
        const int a = e0, b = e1;
        request_value(_L("Fillet radius"), 1.0, 0.001, 100000.0, [this, a, b](double r) {
            CadFeature& f = m_doc.features[m_constrain_feat];
            if (a >= int(f.entities.size()) || b >= int(f.entities.size())) return;
            SketchEntity a_out, b_out, arc_out;
            if (!SketchEngine::fillet_lines(f.entities[a], f.entities[b], r, a_out, b_out, arc_out)) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                set_status(_L("Fillet failed (parallel lines or radius too large)"));
                m_status->Refresh(); return;
            }
            f.entities[a] = a_out;
            f.entities[b] = b_out;
            const int arc = int(f.entities.size());
            f.entities.push_back(arc_out);

            // C4b: glue the fillet arc to the two trimmed lines so it survives a
            // re-solve instead of floating free. arc.p0 sits on line a's moved
            // endpoint, arc.p1 on line b's; recover the exact endpoint roles by
            // nearest match, then emit Coincident (essential — keeps the corner
            // joined) + Tangent (smoothness). The full set can be redundant for the
            // arc, so try it first and drop tangents progressively until the solver
            // accepts it; the Coincident pins survive even if tangency is rejected.
            {
                using R  = SketchPointRole;
                using CT = SketchConstraintType;
                auto role_near = [](const SketchEntity& ln, const Vec2d& p) -> R {
                    return ((ln.p0 - p).squaredNorm() <= (ln.p1 - p).squaredNorm()) ? R::P0 : R::P1;
                };
                const R ra = role_near(f.entities[a], arc_out.p0);
                const R rb = role_near(f.entities[b], arc_out.p1);

                // Fillet trims both lines back from the shared corner, so any
                // constraint anchored to a trimmed endpoint is now stale: the corner
                // Coincident that joined (a,ra)·(b,rb), and each line's own length
                // Distance (its length just changed). Drop them before re-binding —
                // leaving them would fight the new arc geometry and reject every
                // binding below.
                auto refs = [](const SketchEntityConstraintDef& d, int e, R r) {
                    return (d.ea == e && d.ra == r) || (d.eb == e && d.rb == r);
                };
                auto self_len = [](const SketchEntityConstraintDef& d, int e) {
                    return d.type == CT::Distance && d.ea == e && d.eb == e;
                };
                auto& cs = f.entity_constraints;
                cs.erase(std::remove_if(cs.begin(), cs.end(),
                    [&](const SketchEntityConstraintDef& d) {
                        return (d.type == CT::Coincident && refs(d, a, ra) && refs(d, b, rb))
                            || self_len(d, a) || self_len(d, b);
                    }), cs.end());

                auto coin = [&](R arc_role, int ln, R ln_role) {
                    SketchEntityConstraintDef d; d.type = CT::Coincident;
                    d.ea = arc; d.ra = arc_role; d.eb = ln; d.rb = ln_role; return d;
                };
                auto tang = [&](int ln) {
                    SketchEntityConstraintDef d; d.type = CT::Tangent; d.ea = arc; d.eb = ln; return d;
                };
                const std::vector<std::vector<SketchEntityConstraintDef>> ladder = {
                    { coin(R::P0, a, ra), coin(R::P1, b, rb), tang(a), tang(b) },
                    { coin(R::P0, a, ra), coin(R::P1, b, rb), tang(a) },
                    { coin(R::P0, a, ra), coin(R::P1, b, rb) },
                };
                const std::vector<SketchEntity> saved = f.entities;
                const size_t cbefore = f.entity_constraints.size();
                for (const auto& set : ladder) {
                    for (const auto& d : set) f.entity_constraints.push_back(d);
                    if (m_doc.solve_sketch_feature(m_constrain_feat)) break;   // accepted
                    f.entity_constraints.resize(cbefore);
                    f.entities = saved;
                }
            }
            after_edit_op();
        });
        return;   // deferred: edit runs on Confirm
    }

    case EditOp::Chamfer: {
        if (e1 < 0 || e1 >= n) { fail(_L("Pick two lines to chamfer")); return; }
        if (feat.entities[e0].type != Type::Line || feat.entities[e1].type != Type::Line) {
            fail(_L("Chamfer needs two lines")); return;
        }
        const int a = e0, b = e1;
        request_value(_L("Chamfer distance"), 1.0, 0.001, 100000.0, [this, a, b](double d) {
            CadFeature& f = m_doc.features[m_constrain_feat];
            if (a >= int(f.entities.size()) || b >= int(f.entities.size())) return;
            SketchEntity a_out, b_out, seg_out;
            if (!SketchEngine::chamfer_lines(f.entities[a], f.entities[b], d, a_out, b_out, seg_out)) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                set_status(_L("Chamfer failed (parallel lines or distance too large)"));
                m_status->Refresh(); return;
            }
            f.entities[a] = a_out;
            f.entities[b] = b_out;
            const int seg = int(f.entities.size());
            f.entities.push_back(seg_out);

            // C4.6: like fillet, chamfer trims both lines back from the shared corner
            // and inserts a connecting segment. MUTATING op: drop the stale corner
            // Coincident that joined the two trimmed endpoints and each line's own
            // length Distance (lengths just changed), then pin the new segment's ends
            // onto the trimmed line endpoints with Coincident so it survives re-solve.
            {
                using R  = SketchPointRole;
                using CT = SketchConstraintType;
                auto role_near = [](const SketchEntity& ln, const Vec2d& p) -> R {
                    return ((ln.p0 - p).squaredNorm() <= (ln.p1 - p).squaredNorm()) ? R::P0 : R::P1;
                };
                const R ra = role_near(f.entities[a], seg_out.p0);
                const R rb = role_near(f.entities[b], seg_out.p1);

                auto refs = [](const SketchEntityConstraintDef& dd, int e, R r) {
                    return (dd.ea == e && dd.ra == r) || (dd.eb == e && dd.rb == r);
                };
                auto self_len = [](const SketchEntityConstraintDef& dd, int e) {
                    return dd.type == CT::Distance && dd.ea == e && dd.eb == e;
                };
                auto& cs = f.entity_constraints;
                cs.erase(std::remove_if(cs.begin(), cs.end(),
                    [&](const SketchEntityConstraintDef& dd) {
                        return (dd.type == CT::Coincident && refs(dd, a, ra) && refs(dd, b, rb))
                            || self_len(dd, a) || self_len(dd, b);
                    }), cs.end());

                auto coin = [&](R seg_role, int ln, R ln_role) {
                    SketchEntityConstraintDef dd; dd.type = CT::Coincident;
                    dd.ea = seg; dd.ra = seg_role; dd.eb = ln; dd.rb = ln_role; return dd;
                };
                const std::vector<SketchEntity> saved = f.entities;
                const size_t cbefore = f.entity_constraints.size();
                f.entity_constraints.push_back(coin(R::P0, a, ra));
                f.entity_constraints.push_back(coin(R::P1, b, rb));
                if (!m_doc.solve_sketch_feature(m_constrain_feat)) {
                    f.entity_constraints.resize(cbefore);   // keep geometry, drop pins
                    f.entities = saved;
                }
            }
            after_edit_op();
        });
        return;   // deferred: edit runs on Confirm
    }

    case EditOp::Trim:
    case EditOp::Extend: {
        // Trim accepts Line/Arc/Circle subjects; Extend accepts Line/Arc (a Circle
        // is already closed, so there is nothing to extend).
        const Type st = feat.entities[e0].type;
        const bool subject_ok = (op == EditOp::Trim)
            ? (st == Type::Line || st == Type::Arc || st == Type::Circle)
            : (st == Type::Line || st == Type::Arc);
        if (!subject_ok) {
            fail(op == EditOp::Trim ? _L("Trim works on lines, arcs and circles")
                                    : _L("Extend works on lines and arcs"));
            return;
        }
        Vec2d pick;
        if (!m_viewport->pick0_point(pick)) { fail(_L("Pick the edge to trim/extend")); return; }
        std::vector<SketchEntity> others;
        others.reserve(n > 0 ? n - 1 : 0);
        for (int i = 0; i < n; ++i)
            if (i != e0) others.push_back(feat.entities[i]);
        const SketchEntity before = feat.entities[e0];   // C4.1: detect the moved endpoint
        const bool ok = (op == EditOp::Trim)
            ? SketchEngine::trim_entity(feat.entities[e0], others, pick)
            : SketchEngine::extend_entity(feat.entities[e0], others, pick);
        if (!ok) { fail(op == EditOp::Trim ? _L("Nothing to trim at the pick")
                                           : _L("No edge to extend to")); return; }

        // C4.1: trim/extend slides ONE endpoint of the subject along its own
        // direction (line) or sweep (arc). That (a) kills the subject's
        // self-length Distance dim and (b) detaches the moved endpoint from any
        // corner Coincident/PointOn* it used to hold. Drop both stale classes,
        // then re-anchor the moved endpoint onto the entity it now lands on with a
        // PointOnObject (the bridge picks PT_ON_LINE / PT_ON_CIRCLE). A Circle
        // subject restructures into an Arc (both endpoints new) — skip the
        // re-anchor there; its self constraints (Radius/Concentric) survive, so
        // there is nothing stale to drop either.
        if (st == Type::Line || st == Type::Arc) {
            using R  = SketchPointRole;
            using CT = SketchConstraintType;
            const SketchEntity& aft = feat.entities[e0];
            R     moved = R::P0;
            Vec2d P;
            if (st == Type::Line) {
                const bool p0_moved = (before.p0 - aft.p0).squaredNorm()
                                    > (before.p1 - aft.p1).squaredNorm();
                moved = p0_moved ? R::P0 : R::P1;
                P     = p0_moved ? aft.p0 : aft.p1;
            } else {
                const bool start_moved = std::abs(before.start_angle - aft.start_angle)
                                       > std::abs(before.end_angle - aft.end_angle);
                moved = start_moved ? R::P0 : R::P1;
                const double ang = start_moved ? aft.start_angle : aft.end_angle;
                P = aft.center + aft.radius * Vec2d(std::cos(ang), std::sin(ang));
            }

            // Drop stale: subject self-length Distance + any Coincident/PointOn*
            // pinning the moved endpoint to its old corner.
            auto refs = [&](const SketchEntityConstraintDef& d, R r) {
                return (d.ea == e0 && d.ra == r) || (d.eb == e0 && d.rb == r);
            };
            auto& cs = feat.entity_constraints;
            cs.erase(std::remove_if(cs.begin(), cs.end(),
                [&](const SketchEntityConstraintDef& d) {
                    if (d.type == CT::Distance && d.ea == e0 && d.eb == e0) return true;
                    return (d.type == CT::Coincident || d.type == CT::PointOnLine
                         || d.type == CT::PointOnObject) && refs(d, moved);
                }), cs.end());

            // Find which other entity the moved endpoint now lies on (Line/Circle
            // cutters only — PT_ON_* needs a line or circle primitive).
            int cutter = -1;
            const double tol = 1e-5;
            for (int i = 0; i < int(feat.entities.size()); ++i) {
                if (i == e0) continue;
                const SketchEntity& o = feat.entities[i];
                if (o.type == Type::Line) {
                    Vec2d dv = o.p1 - o.p0;
                    const double L2 = dv.dot(dv);
                    if (L2 < 1e-18) continue;
                    const double t = (P - o.p0).dot(dv) / L2;
                    if (t < -1e-6 || t > 1.0 + 1e-6) continue;
                    if ((o.p0 + t * dv - P).norm() < tol) { cutter = i; break; }
                } else if (o.type == Type::Circle) {
                    if (std::abs((P - o.center).norm() - o.radius) < tol) { cutter = i; break; }
                }
            }

            // Re-anchor with PointOnObject; keep geometry + the stale-drop even if
            // the solver rejects the new (possibly redundant) binding.
            if (cutter >= 0) {
                const size_t cbefore = feat.entity_constraints.size();
                SketchEntityConstraintDef d; d.type = CT::PointOnObject;
                d.ea = e0; d.ra = moved; d.eb = cutter;
                feat.entity_constraints.push_back(d);
                if (!m_doc.solve_sketch_feature(m_constrain_feat))
                    feat.entity_constraints.resize(cbefore);
            }
        }
        break;
    }
    case EditOp::Array: {
        // C4.4 linear array. Additive op (originals untouched, copies appended) ->
        // no stale constraints to drop. Collect count, then spacing; the array runs
        // along the subject line's own direction (or +X for non-lines). Copies are
        // pure translates, so for lines they are Parallel + EqualLength to the
        // source by construction -> bind each copy to the source in a star web; for
        // arc/circle subjects the translate preserves radius (but not the centre),
        // so the web is a per-copy Radius dimension instead (see below).
        const int a = e0;
        request_value(_L("Array count (incl. original)"), 3.0, 2.0, 200.0,
            [this, a](double cnt_d) {
                const int count = std::max(2, int(cnt_d + 0.5));
                request_value(_L("Spacing (mm)"), 20.0, -100000.0, 100000.0,
                    [this, a, count](double sp) {
                        CadFeature& f = m_doc.features[m_constrain_feat];
                        if (a >= int(f.entities.size())) return;
                        using Type = SketchEntity::Type;
                        // Snapshot everything we need from the source BEFORE pushing the
                        // copies: push_back can reallocate f.entities and dangle any
                        // reference into it. Copy the subject by value.
                        const SketchEntity src = f.entities[a];
                        const Type src_type = src.type;
                        // Default direction: perpendicular to a line (so copies stack
                        // into a visible, non-overlapping parallel pattern rather than
                        // extending collinearly); +X for non-line subjects.
                        Vec2d dir(1.0, 0.0);
                        if (src_type == Type::Line) {
                            const Vec2d t = src.p1 - src.p0;
                            if (t.norm() > 1e-9) {
                                const Vec2d u = t.normalized();
                                dir = Vec2d(-u.y(), u.x());
                            }
                        }
                        auto copies = SketchEngine::array_entities(
                            { src }, count, sp * dir, 0.0, Vec2d(0, 0));
                        if (copies.empty()) {
                            m_status->SetForegroundColour(wxColour(235, 110, 110));
                            set_status(_L("Array produced nothing")); m_status->Refresh(); return;
                        }
                        const int base = int(f.entities.size());   // first copy index
                        for (auto& c : copies) f.entities.push_back(c);

                        if (src_type == Type::Line) {
                            using CT = SketchConstraintType;
                            // Bind every copy to the source in a star web. Emit the
                            // WHOLE web before solving (a per-constraint solve would run
                            // while later copies are still unconstrained, which the
                            // solver rejects), then degrade as a set: try Parallel +
                            // EqualLength, fall back to Parallel only (EqualLength can be
                            // rank-deficient on exact congruent copies), then to bare
                            // geometry. Keep the geometry regardless.
                            const size_t cb = f.entity_constraints.size();
                            auto build_web = [&](bool with_equal) {
                                f.entity_constraints.resize(cb);
                                for (int k = 0; k < int(copies.size()); ++k) {
                                    SketchEntityConstraintDef dp; dp.type = CT::Parallel;
                                    dp.ea = a; dp.eb = base + k;
                                    f.entity_constraints.push_back(dp);
                                    if (with_equal) {
                                        SketchEntityConstraintDef de; de.type = CT::EqualLength;
                                        de.ea = a; de.eb = base + k;
                                        f.entity_constraints.push_back(de);
                                    }
                                }
                            };
                            build_web(true);
                            if (!m_doc.solve_sketch_feature(m_constrain_feat)) {
                                build_web(false);
                                if (!m_doc.solve_sketch_feature(m_constrain_feat))
                                    f.entity_constraints.resize(cb);
                            }
                        } else if (src_type == Type::Arc || src_type == Type::Circle) {
                            // Curved subject: translation preserves the radius but
                            // marches the centres apart, so the copies are NOT
                            // concentric. There is no EQUAL_RADIUS in the constraint
                            // enum, so pin each copy's radius to the source value with
                            // a per-copy Radius dimension (keeps the array equal-radius
                            // and documents intent, mirroring the line web). Solve and
                            // roll the whole web back if the solver rejects it.
                            using CT = SketchConstraintType;
                            const size_t cb = f.entity_constraints.size();
                            for (int k = 0; k < int(copies.size()); ++k) {
                                SketchEntityConstraintDef dr; dr.type = CT::Radius;
                                dr.ea = base + k; dr.value = src.radius;
                                f.entity_constraints.push_back(dr);
                            }
                            if (!m_doc.solve_sketch_feature(m_constrain_feat))
                                f.entity_constraints.resize(cb);
                        }
                        after_edit_op();
                    });
            });
        return;   // deferred: edit runs on the two Confirms
    }
    case EditOp::Move: {
        // C4.5 Transform (move). MUTATING op: the subject is translated in place
        // (kernel transform_entities with angle=0, scale=1). Pure translation
        // PRESERVES orientation and length, so intrinsic + orientation constraints
        // survive (Horizontal/Vertical/Parallel/Perpendicular/EqualLength/Angle,
        // self-length Distance, Radius/Diameter); it BREAKS position-coupling ones
        // (Coincident/PointOn*/Concentric/Symmetric/Midpoint/Fix/LockX/LockY, and
        // any Distance tying the subject to a *different* entity). Per the governing
        // P4 insight, drop those before re-solving — otherwise the solver drags the
        // subject straight back to satisfy them and the move never sticks.
        const int a = e0;
        request_value(_L("Move dX (mm)"), 20.0, -100000.0, 100000.0,
            [this, a](double dx) {
                request_value(_L("Move dY (mm)"), 0.0, -100000.0, 100000.0,
                    [this, a, dx](double dy) {
                        CadFeature& f = m_doc.features[m_constrain_feat];
                        if (a >= int(f.entities.size())) return;
                        auto out = SketchEngine::transform_entities(
                            { f.entities[a] }, Vec2d(dx, dy), 0.0, 1.0, Vec2d(0, 0));
                        if (out.empty()) {
                            m_status->SetForegroundColour(wxColour(235, 110, 110));
                            set_status(_L("Move produced nothing")); m_status->Refresh(); return;
                        }
                        f.entities[a] = out[0];

                        using CT = SketchConstraintType;
                        auto refs_a = [&](const SketchEntityConstraintDef& d) {
                            return d.ea == a || d.eb == a || d.ec == a;
                        };
                        auto& cs = f.entity_constraints;
                        cs.erase(std::remove_if(cs.begin(), cs.end(),
                            [&](const SketchEntityConstraintDef& d) {
                                if (!refs_a(d)) return false;
                                switch (d.type) {
                                case CT::Coincident: case CT::PointOnLine: case CT::PointOnObject:
                                case CT::Concentric: case CT::Symmetric:   case CT::Midpoint:
                                case CT::Fix:        case CT::LockX:        case CT::LockY:
                                    return true;   // position-coupling: broken by translation
                                case CT::Distance:
                                    // self-length (ea==eb==a) survives translation; a
                                    // distance to a *different* entity does not.
                                    return !(d.ea == a && d.eb == a);
                                default:
                                    return false;  // orientation/length: preserved
                                }
                            }), cs.end());

                        // The surviving constraints are satisfied by construction
                        // (translation preserves them); re-solve to fold the new
                        // position in, keep the geometry even if the solver balks.
                        m_doc.solve_sketch_feature(m_constrain_feat);
                        after_edit_op();
                    });
            });
        return;   // deferred: edit runs on the two Confirms
    }
    case EditOp::Rotate: {
        // C4.5b Transform (rotate-in-place about the subject centroid). MUTATING op.
        // Rotation PRESERVES intrinsic size (length/radius) but changes the subject's
        // ORIENTATION and the POSITION of its points. So only the size constraints
        // survive (EqualLength/Radius/Diameter + self-length Distance); every
        // position- or orientation-coupling constraint is broken and must be dropped
        // before re-solving, otherwise the solver spins the subject back to satisfy
        // them and the rotation never sticks (governing P4 insight).
        const int a = e0;
        request_value(_L("Rotate angle (deg)"), 45.0, -360.0, 360.0,
            [this, a](double deg) {
                CadFeature& f = m_doc.features[m_constrain_feat];
                if (a >= int(f.entities.size())) return;
                auto centroid_of = [](const SketchEntity& e) -> Vec2d {
                    using T = SketchEntity::Type;
                    switch (e.type) {
                    case T::Line:    return 0.5 * (e.p0 + e.p1);
                    case T::Arc: case T::Circle: case T::Ellipse: case T::EllipseArc:
                                     return e.center;
                    case T::BSpline:
                        if (!e.ctrl.empty()) {
                            Vec2d s(0, 0); for (auto& p : e.ctrl) s += p;
                            return s / double(e.ctrl.size());
                        }
                        return 0.5 * (e.p0 + e.p1);
                    default:         return e.p0;   // Point
                    }
                };
                const Vec2d piv = centroid_of(f.entities[a]);
                auto out = SketchEngine::transform_entities(
                    { f.entities[a] }, Vec2d(0, 0), deg * M_PI / 180.0, 1.0, piv);
                if (out.empty()) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Rotate produced nothing")); m_status->Refresh(); return;
                }
                f.entities[a] = out[0];

                using CT = SketchConstraintType;
                auto refs_a = [&](const SketchEntityConstraintDef& d) {
                    return d.ea == a || d.eb == a || d.ec == a;
                };
                auto& cs = f.entity_constraints;
                cs.erase(std::remove_if(cs.begin(), cs.end(),
                    [&](const SketchEntityConstraintDef& d) {
                        if (!refs_a(d)) return false;
                        switch (d.type) {
                        case CT::EqualLength: case CT::Radius: case CT::Diameter:
                            return false;   // intrinsic size: preserved by rotation
                        case CT::Distance:
                            return !(d.ea == a && d.eb == a);   // self-length survives
                        default:
                            return true;    // position/orientation-coupling: broken
                        }
                    }), cs.end());

                m_doc.solve_sketch_feature(m_constrain_feat);
                after_edit_op();
            });
        return;   // deferred: edit runs on Confirm
    }
    case EditOp::Scale: {
        // C4.5c Transform (uniform scale-in-place about the subject centroid). MUTATING
        // op. Uniform scaling PRESERVES orientation and angles (Horizontal/Vertical/
        // Parallel/Perpendicular/Angle survive) but changes SIZE and point POSITIONS:
        // drop every size constraint (Radius/Diameter/EqualLength/any Distance) and
        // every position-coupling constraint before re-solving, else the solver
        // rescales the subject back to satisfy them.
        const int a = e0;
        request_value(_L("Scale factor"), 2.0, 0.01, 1000.0,
            [this, a](double sf) {
                CadFeature& f = m_doc.features[m_constrain_feat];
                if (a >= int(f.entities.size())) return;
                auto centroid_of = [](const SketchEntity& e) -> Vec2d {
                    using T = SketchEntity::Type;
                    switch (e.type) {
                    case T::Line:    return 0.5 * (e.p0 + e.p1);
                    case T::Arc: case T::Circle: case T::Ellipse: case T::EllipseArc:
                                     return e.center;
                    case T::BSpline:
                        if (!e.ctrl.empty()) {
                            Vec2d s(0, 0); for (auto& p : e.ctrl) s += p;
                            return s / double(e.ctrl.size());
                        }
                        return 0.5 * (e.p0 + e.p1);
                    default:         return e.p0;   // Point
                    }
                };
                const Vec2d piv = centroid_of(f.entities[a]);
                auto out = SketchEngine::transform_entities(
                    { f.entities[a] }, Vec2d(0, 0), 0.0, sf, piv);
                if (out.empty()) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    set_status(_L("Scale produced nothing")); m_status->Refresh(); return;
                }
                f.entities[a] = out[0];

                using CT = SketchConstraintType;
                auto refs_a = [&](const SketchEntityConstraintDef& d) {
                    return d.ea == a || d.eb == a || d.ec == a;
                };
                auto& cs = f.entity_constraints;
                cs.erase(std::remove_if(cs.begin(), cs.end(),
                    [&](const SketchEntityConstraintDef& d) {
                        if (!refs_a(d)) return false;
                        switch (d.type) {
                        case CT::Horizontal: case CT::Vertical: case CT::Parallel:
                        case CT::Perpendicular: case CT::Angle:
                            return false;   // orientation/angle: preserved by uniform scale
                        default:
                            return true;    // size + position-coupling: broken
                        }
                    }), cs.end());

                m_doc.solve_sketch_feature(m_constrain_feat);
                after_edit_op();
            });
        return;   // deferred: edit runs on Confirm
    }
    case EditOp::PolarArray: {
        // Polar array about the subject centroid. ADDITIVE op (originals untouched,
        // count-1 rotated copies appended) -> no stale constraints to drop. Copies
        // are rigid rotations of the source, so they preserve LENGTH but NOT
        // orientation: bind each copy to the source with EqualLength only (Parallel
        // does NOT hold under rotation, unlike the linear-array web). Arc/circle
        // subjects rotate about their own centre, so their copies stay Concentric +
        // equal-radius instead (see below). Dialogs: count
        // then total sweep; angle_step = sweep/count spreads them evenly (last copy
        // lands just shy of the original on a full 360).
        const int a = e0;
        request_value(_L("Polar count (incl. original)"), 6.0, 2.0, 200.0,
            [this, a](double cnt_d) {
                const int count = std::max(2, int(cnt_d + 0.5));
                request_value(_L("Total sweep (deg)"), 360.0, -360.0, 360.0,
                    [this, a, count](double sweep_deg) {
                        CadFeature& f = m_doc.features[m_constrain_feat];
                        if (a >= int(f.entities.size())) return;
                        using Type = SketchEntity::Type;
                        // Snapshot the subject by value BEFORE pushing copies: push_back
                        // can reallocate f.entities and dangle a reference into it.
                        const SketchEntity src = f.entities[a];
                        const Type src_type = src.type;
                        auto centroid_of = [](const SketchEntity& e) -> Vec2d {
                            using T = SketchEntity::Type;
                            switch (e.type) {
                            case T::Line:    return 0.5 * (e.p0 + e.p1);
                            case T::Arc: case T::Circle: case T::Ellipse: case T::EllipseArc:
                                             return e.center;
                            case T::BSpline:
                                if (!e.ctrl.empty()) {
                                    Vec2d s(0, 0); for (auto& p : e.ctrl) s += p;
                                    return s / double(e.ctrl.size());
                                }
                                return 0.5 * (e.p0 + e.p1);
                            default:         return e.p0;   // Point
                            }
                        };
                        const Vec2d  piv        = centroid_of(src);
                        const double angle_step = (sweep_deg * M_PI / 180.0) / double(count);
                        auto copies = SketchEngine::array_entities(
                            { src }, count, Vec2d(0, 0), angle_step, piv);
                        if (copies.empty()) {
                            m_status->SetForegroundColour(wxColour(235, 110, 110));
                            set_status(_L("Polar array produced nothing")); m_status->Refresh(); return;
                        }
                        const int base = int(f.entities.size());   // first copy index
                        for (auto& c : copies) f.entities.push_back(c);

                        if (src_type == Type::Line) {
                            using CT = SketchConstraintType;
                            // Rotational web: each copy is EqualLength to the source
                            // (rotation preserves length; orientation differs so NO
                            // Parallel). Emit the whole web before solving, then fall
                            // back to bare geometry if it is rank-deficient.
                            const size_t cb = f.entity_constraints.size();
                            for (int k = 0; k < int(copies.size()); ++k) {
                                SketchEntityConstraintDef de; de.type = CT::EqualLength;
                                de.ea = a; de.eb = base + k;
                                f.entity_constraints.push_back(de);
                            }
                            if (!m_doc.solve_sketch_feature(m_constrain_feat))
                                f.entity_constraints.resize(cb);
                        } else if (src_type == Type::Arc || src_type == Type::Circle) {
                            // Curved subject: the polar pivot is the subject centroid,
                            // which for an arc/circle IS its own centre. Rotating about
                            // that centre keeps every copy CONCENTRIC with the source and
                            // at the same radius (only the angular position shifts). Bind
                            // each copy with Concentric + a per-copy Radius dimension;
                            // degrade to Radius-only, then to bare geometry, keeping the
                            // geometry regardless.
                            using CT = SketchConstraintType;
                            const size_t cb = f.entity_constraints.size();
                            auto build_web = [&](bool with_concentric) {
                                f.entity_constraints.resize(cb);
                                for (int k = 0; k < int(copies.size()); ++k) {
                                    if (with_concentric) {
                                        SketchEntityConstraintDef dc; dc.type = CT::Concentric;
                                        dc.ea = a; dc.eb = base + k;
                                        f.entity_constraints.push_back(dc);
                                    }
                                    SketchEntityConstraintDef dr; dr.type = CT::Radius;
                                    dr.ea = base + k; dr.value = src.radius;
                                    f.entity_constraints.push_back(dr);
                                }
                            };
                            build_web(true);
                            if (!m_doc.solve_sketch_feature(m_constrain_feat)) {
                                build_web(false);
                                if (!m_doc.solve_sketch_feature(m_constrain_feat))
                                    f.entity_constraints.resize(cb);
                            }
                        }
                        after_edit_op();
                    });
            });
        return;   // deferred: edit runs on the two Confirms
    }
    }

    after_edit_op();
}

void DesignPanel::after_edit_op()
{
    if (m_constrain_feat < 0 || m_constrain_feat >= int(m_doc.features.size()) || !m_viewport)
        return;
    m_doc.recompute();
    m_viewport->set_constraint_highlight({});   // entity indices may have shifted
    m_viewport->update_constrain_entities(m_doc.features[m_constrain_feat].entities);
    // Refresh the committed-sketch overlay too: it caches the entity list at
    // constrain-entry, so a relocating edit (Move/Trim/Extend) would otherwise
    // leave a stale ghost of the pre-edit geometry beside the new position.
    sync_sketch_display();
    if (!m_doc.display_mesh.its.indices.empty())
        feed_bodies();
    m_status->SetForegroundColour(wxNullColour);
    set_status(_L("Applied edit"));
    m_status->Refresh();
    refresh_constrain_dof();
    rebuild_constraint_list();
}

void DesignPanel::request_value(const wxString& label, double def, double mn, double mx,
                                std::function<void(double)> cont,
                                std::function<void()> on_cancel)
{
    m_value_cont = std::move(cont);
    m_value_cancel = std::move(on_cancel);
    m_value_min = mn;
    m_value_max = mx;
    m_value_label->SetLabel(label);
    m_value_input->ChangeValue(en_format(def));   // '.' decimals, no EVT_TEXT feedback
    m_cards->GetSizer()->Show(m_box_value, true, true);
    update_cards_frame(); m_form->Layout();
    m_form->FitInside();
    m_value_input->SetFocus();
    m_value_input->SetSelection(-1, -1);   // select all so typing replaces the value
    m_status->SetForegroundColour(wxNullColour);
    set_status(label + _L(" — type a value, press Enter (or Confirm)"));
    m_status->Refresh();
}

void DesignPanel::confirm_value()
{
    if (!m_value_cont) { cancel_value(); return; }
    double v = 0.0;
    if (!en_parse(m_value_input->GetValue(), v)) { m_value_input->SetFocus(); return; }
    v = std::min(std::max(v, m_value_min), m_value_max);   // clamp to range
    auto cont = m_value_cont;            // copy, then clear before running so a
    m_value_cont = nullptr;              // re-entrant request_value can re-arm cleanly
    m_value_cancel = nullptr;            // confirmed: drop the cancel action
    m_cards->GetSizer()->Show(m_box_value, false, true);
    update_cards_frame(); m_form->Layout();
    m_form->FitInside();
    cont(v);                            // run the deferred constraint / edit-op apply
}

void DesignPanel::cancel_value()
{
    const bool was_open = (m_value_cont != nullptr);
    m_value_cont = nullptr;
    auto on_cancel = m_value_cancel;     // copy, clear, then run (re-entrancy safe)
    m_value_cancel = nullptr;
    if (m_box_value)
        m_cards->GetSizer()->Show(m_box_value, false, true);
    update_cards_frame(); m_form->Layout();
    m_form->FitInside();
    if (was_open) {
        m_status->SetForegroundColour(wxNullColour);
        set_status(wxString());
        m_status->Refresh();
    }
    if (on_cancel)
        on_cancel();                     // e.g. keep a pending line segment as drawn
}

void DesignPanel::apply_constraint(SketchConstraintType type)
{
    if (m_constrain_feat < 0 || m_constrain_feat >= int(m_doc.features.size()) ||
        m_viewport == nullptr) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Press Constrain on a sketch first"));
        m_status->Refresh();
        return;
    }

    // Entity sketches (Fase 4.2) route through the entity-constraint path.
    if (m_viewport->is_constraining_entities()) {
        apply_entity_constraint(type);
        return;
    }

    if (!m_viewport->is_constraining()) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Press Constrain on a sketch first"));
        m_status->Refresh();
        return;
    }
    int a = -1, b = -1;
    if (!m_viewport->selected_segment(a, b)) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Pick a segment in the viewport first"));
        m_status->Refresh();
        return;
    }
    CadFeature& feat = m_doc.features[m_constrain_feat];
    // solve_sketch_feature rewrites profile.points even on failure, so snapshot
    // the geometry to roll back a rejected constraint cleanly.
    const std::vector<Vec2d> saved_pts = feat.profile.points;
    feat.constraints.push_back(SketchConstraintDef{type, a, b, -1, -1, 0.0});
    if (!m_doc.solve_sketch_feature(m_constrain_feat)) {
        feat.constraints.pop_back();        // reject the non-converging addition
        feat.profile.points = saved_pts;    // and restore the pre-solve geometry
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Constraint rejected (over-constrained)"));
        m_status->Refresh();
        return;
    }
    m_doc.recompute();
    m_viewport->update_constrain_profile(m_doc.features[m_constrain_feat].profile.points);
    if (!m_doc.display_mesh.its.indices.empty())
        feed_bodies();
    m_status->SetForegroundColour(wxNullColour);
    set_status(type == SketchConstraintType::Horizontal ? _L("Applied Horizontal")
                                                                : _L("Applied Vertical"));
    m_status->Refresh();
}

void DesignPanel::reset_edit_state()
{
    m_edit_index = -1;
}

int DesignPanel::resolve_extrude_sketch() const
{
    int sel = tree_selection();
    if (sel != wxNOT_FOUND && sel < int(m_doc.features.size()) &&
        m_doc.features[sel].type == CadFeatureType::Sketch)
        return sel;
    for (int i = int(m_doc.features.size()) - 1; i >= 0; --i)
        if (m_doc.features[i].type == CadFeatureType::Sketch) return i;
    return -1;
}

void DesignPanel::load_feature_into_dialog(const CadFeature& f)
{
    switch (f.type) {
    case CadFeatureType::Sketch:
        m_shape->SetSelection(f.shape == SketchShape::Circle ? 1 : 0);
        m_width->SetValue(f.width);
        m_height->SetValue(f.height);
        m_radius->SetValue(f.radius);
        break;
    case CadFeatureType::Extrude:
        m_distance->SetValue(f.distance);
        m_mode->SetSelection(static_cast<int>(f.mode)); // New=0,Add=1,Cut=2,Intersect=3
        m_extrude_end->SetSelection(static_cast<int>(f.extrude_end));
        m_distance2->SetValue(f.distance2);
        m_taper->SetValue(f.taper_deg);
        m_flip->SetValue(f.flip);
        m_extrude_sketch_ref = f.sketch_ref;
        m_sel_solid_body = f.target_body;    // preserve which body on re-edit
        if (m_extrude_sketch_ref >= 0 && m_extrude_sketch_ref < int(m_doc.features.size()))
            m_extrude_sketch_label->SetLabel(_L("Sketch: ") +
                wxString::FromUTF8(m_doc.features[m_extrude_sketch_ref].name));
        break;
    case CadFeatureType::Fillet:
    case CadFeatureType::Chamfer:
        m_dressup_type->SetSelection(f.type == CadFeatureType::Fillet ? 0 : 1);
        m_dressup_size->SetValue(f.dressup_size);
        m_face_group->SetSelection(static_cast<int>(f.face_group));
        m_sel_solid_edge = f.dressup_edge;   // preserve edge-targeting on re-edit
        m_sel_solid_body = f.target_body;    // preserve which body on re-edit
        sync_dressup_target();               // and say which of the two the re-edit is targeting
        break;
    case CadFeatureType::Hole:
        m_hole_plane->SetSelection(index_from_plane(f.plane));
        m_hole_diameter->SetValue(f.hole_diameter);
        m_hole_depth->SetValue(f.hole_depth);
        m_hole_through->SetValue(f.hole_through);
        m_hole_x->SetValue(f.hole_x);
        m_hole_y->SetValue(f.hole_y);
        // Re-latch the on-face state FROM THE STORED FEATURE (snaporca-uif9). m_hole_on_face is
        // only ever cleared by the Hole flyout and by the plane combo, so after any on-face hole
        // it stays true — and a re-edit then drilled on whatever face was latched last, which may
        // be a different face, a different body, or a body since rebuilt. f is the only source
        // guaranteed to describe THIS hole. Not the dropdown row just set above: index_from_plane
        // snaps an arbitrary face plane to the nearest XY/XZ/YZ, so driving the re-edit from the
        // row would MOVE a hole drilled on a slanted or offset face.
        m_hole_on_face    = !is_base_plane(f.plane, m_doc.modeling_origin);
        m_hole_face_plane = f.plane;
        m_hole_face_body  = m_hole_on_face ? f.target_body : -1;
        // The face's (u,v) extent is not serialized, so the gizmo's footprint clamp has nothing
        // to stand on. Unbounded is the honest state; the stale bounds of another face are not.
        m_hole_has_bounds = false;
        if (m_hole_target_label)
            m_hole_target_label->SetLabel(m_hole_on_face
                // No face index survives in the feature, and inventing one would be worse than
                // saying so. "(none — uses Hole plane)" is the one thing that is definitely false.
                ? (f.target_body >= 0 ? wxString::Format(_L("On a face of Body %d"), f.target_body + 1)
                                      : _L("On a stored face"))
                : _L("(none — uses Hole plane)"));
        break;
    case CadFeatureType::Thread:
        m_thread_plane->SetSelection(index_from_plane(f.plane));
        m_thread_radius->SetValue(f.thread_radius * 2.0);   // field = diameter
        m_thread_pitch->SetValue(f.thread_pitch);
        m_thread_height->SetValue(f.thread_height);
        m_thread_depth->SetValue(f.thread_depth);
        m_thread_internal->SetValue(f.thread_internal);
        m_thread_x->SetValue(f.thread_x);
        m_thread_y->SetValue(f.thread_y);
        if (m_thread_std) m_thread_std->SetSelection(0);   // Custom: spins reflect the stored feature
        // Same latch, same failure, same fix as Hole above (snaporca-uif9).
        m_thread_on_face    = !is_base_plane(f.plane, m_doc.modeling_origin);
        m_thread_face_plane = f.plane;
        m_thread_face_body  = m_thread_on_face ? f.target_body : -1;
        if (m_thread_target_label)
            m_thread_target_label->SetLabel(m_thread_on_face
                ? (f.target_body >= 0 ? wxString::Format(_L("On a face of Body %d"), f.target_body + 1)
                                      : _L("On a stored face"))
                : _L("(none — uses Thread plane)"));
        break;
    case CadFeatureType::Shell:
        m_shell_thickness->SetValue(f.shell_thickness);
        m_sel_solid_face = f.shell_face;
        m_shell_face_label->SetLabel(f.shell_face >= 0
            ? wxString::Format(_L("Face %d"), f.shell_face)
            : _L("(all faces — closed hollow)"));
        break;
    case CadFeatureType::Revolve:
        m_revolve_angle->SetValue(f.revolve_angle);
        m_revolve_axis->SetSelection(f.revolve_axis);
        m_revolve_mode->SetSelection(static_cast<int>(f.mode));
        m_revolve_flip->SetValue(f.flip);
        m_revolve_sketch_ref = f.sketch_ref;
        break;
    case CadFeatureType::Sweep:
        m_sweep_profile_ref = f.sketch_ref;
        m_sweep_path_ref    = f.sweep_path_ref;   // show_tool pre-selects this in the picker
        m_sweep_mode->SetSelection(static_cast<int>(f.mode));
        break;
    case CadFeatureType::Pattern:
        m_pattern_type->SetSelection(f.pattern_circular ? 1 : 0);
        m_pattern_count->SetValue(f.pattern_count);
        m_pattern_spacing->SetValue(f.pattern_spacing);
        m_pattern_dir->SetSelection(f.pattern_dir);
        m_pattern_angle->SetValue(f.pattern_angle);
        break;
    case CadFeatureType::Plane:
        populate_plane_choices(m_plane_base);
        m_plane_base->SetSelection(f.plane_base);
        m_plane_offset->SetValue(f.plane_offset);
        m_plane_tilt->SetValue(f.plane_angle_tilt);
        m_plane_tilt_axis->SetSelection(f.plane_axis);
        m_plane_type->SetSelection((int)f.plane_type);
        m_pl_faceA_body = f.plane_face_body;  m_pl_faceA = f.plane_face;
        m_pl_faceB_body = f.plane_face2_body; m_pl_faceB = f.plane_face2;
        m_pl_edgeA_body = f.plane_edge_body;  m_pl_edgeA = f.plane_edge;
        m_pl_edgeB_body = f.plane_edge2_body; m_pl_edgeB = f.plane_edge2;
        m_plane_usize->SetValue(f.plane_u_size);
        m_plane_vsize->SetValue(f.plane_v_size);
        m_plane_pick = PlanePick::None;
        if (m_viewport) m_viewport->set_escalate_on_repick(true);   // pick abandoned
    if (m_viewport) m_viewport->set_escalate_on_repick(true);   // pick abandoned
        refresh_plane_labels();
        break;
    case CadFeatureType::Loft:
        m_loft_refs = f.loft_profile_refs;   // show_tool re-checks these in the list
        m_loft_ruled->SetValue(f.loft_ruled);
        m_loft_mode->SetSelection(static_cast<int>(f.mode));
        break;
    case CadFeatureType::Draft:
        m_draft_angle->SetValue(f.draft_angle);
        m_sel_solid_face = f.draft_face;
        m_draft_face_label->SetLabel(f.draft_face >= 0
            ? wxString::Format(_L("Face %d"), f.draft_face)
            : _L("(pick a side face)"));
        break;
    case CadFeatureType::Axis:
        m_axis_type->SetSelection((int)f.axis_type);
        m_ax_face_body = f.axis_body; m_ax_face = f.axis_face;
        m_ax_edge = f.axis_edge;
        populate_plane_choices(m_axis_plane_a);
        populate_plane_choices(m_axis_plane_b);
        m_axis_plane_a->SetSelection(f.axis_plane_a >= 0 ? f.axis_plane_a : 0);
        m_axis_plane_b->SetSelection(f.axis_plane_b >= 0 ? f.axis_plane_b : 0);
        m_axis_p1x->SetValue(f.axis_p1.x()); m_axis_p1y->SetValue(f.axis_p1.y()); m_axis_p1z->SetValue(f.axis_p1.z());
        m_axis_p2x->SetValue(f.axis_p2.x()); m_axis_p2y->SetValue(f.axis_p2.y()); m_axis_p2z->SetValue(f.axis_p2.z());
        m_axis_pick = AxisPick::None;
        if (m_viewport) m_viewport->set_escalate_on_repick(true);   // pick abandoned
    if (m_viewport) m_viewport->set_escalate_on_repick(true);   // pick abandoned
        refresh_axis_labels();
        break;
    case CadFeatureType::CoordSys:
        m_coordsys_type->SetSelection((int)f.coordsys_type);
        m_cs_x->SetValue(f.coordsys_point.x());
        m_cs_y->SetValue(f.coordsys_point.y());
        m_cs_z->SetValue(f.coordsys_point.z());
        m_cs_face_body = f.coordsys_body; m_cs_face = f.coordsys_face;
        m_cs_edge = f.coordsys_edge;
        m_cs_hx->SetValue(f.coordsys_x_hint.x());
        m_cs_hy->SetValue(f.coordsys_x_hint.y());
        m_cs_hz->SetValue(f.coordsys_x_hint.z());
        m_coordsys_pick = CoordSysPick::None;
        if (m_viewport) m_viewport->set_escalate_on_repick(true);   // pick abandoned
    if (m_viewport) m_viewport->set_escalate_on_repick(true);   // pick abandoned
        refresh_coordsys_labels();
        refresh_cs_body_choice();
        if (m_cs_body && f.coordsys_body >= 0) {
            m_cs_body->SetSelection(f.coordsys_body + 1);
            if (m_viewport) m_viewport->set_xray_focus(f.coordsys_body);
        }
        break;
    case CadFeatureType::Boolean:
        // List the bodies available to the boolean (as-of its timeline slot), so the consumed
        // tool body still appears and the saved selections below land on the right entries.
        populate_body_choices(m_edit_index);
        m_bool_op->SetSelection(f.mode == BooleanMode::Cut       ? 1
                              : f.mode == BooleanMode::Intersect ? 2 : 0);  // 0 = Union/Add
        if (f.target_body >= 0 && f.target_body < int(m_bool_target->GetCount()))
            m_bool_target->SetSelection(f.target_body);
        if (f.bool_tool_body >= 0 && f.bool_tool_body < int(m_bool_tool->GetCount()))
            m_bool_tool->SetSelection(f.bool_tool_body);
        m_bool_keep->SetValue(f.bool_keep_tool);
        m_bool_tol->SetValue(f.bool_tolerance);
        break;
    case CadFeatureType::Cut:
        // Same as-of-timeline reasoning as Boolean: a Cut splits one body into two, so the
        // live body list does not match the one this feature's target index was recorded
        // against. Replay to just before it and the stored index lands on the right entry.
        populate_body_choices(m_edit_index);
        if (f.target_body >= 0 && f.target_body < int(m_cut_target->GetCount()))
            m_cut_target->SetSelection(f.target_body);
        else if (m_cut_target->GetCount() > 0)
            m_cut_target->SetSelection(0);
        populate_plane_choices(m_cut_plane);
        m_cut_plane->SetSelection(index_from_plane(f.plane));
        m_cut_offset->SetValue(f.cut_offset);
        break;
    case CadFeatureType::SurfaceExtrude:
        m_surf_extrude_distance->SetValue(f.distance);
        m_surf_extrude_sketch_ref = f.sketch_ref;
        if (m_surf_extrude_sketch_ref >= 0 && m_surf_extrude_sketch_ref < int(m_doc.features.size()))
            m_surf_extrude_sketch_label->SetLabel(_L("Sketch: ") +
                wxString::FromUTF8(m_doc.features[m_surf_extrude_sketch_ref].name));
        break;
    case CadFeatureType::SurfaceRevolve:
        m_surf_revolve_angle->SetValue(f.revolve_angle);
        m_surf_revolve_axis->SetSelection(f.revolve_axis);
        m_surf_revolve_flip->SetValue(f.flip);
        m_surf_revolve_sketch_ref = f.sketch_ref;
        if (m_surf_revolve_sketch_ref >= 0 && m_surf_revolve_sketch_ref < int(m_doc.features.size()))
            m_surf_revolve_sketch_label->SetLabel(_L("Sketch: ") +
                wxString::FromUTF8(m_doc.features[m_surf_revolve_sketch_ref].name));
        break;
    case CadFeatureType::SurfaceLoft:
        m_surf_loft_refs = f.loft_profile_refs;
        m_surf_loft_ruled->SetValue(f.loft_ruled);
        break;
    case CadFeatureType::SurfaceFill:
        m_surf_fill_sketch_ref = f.sketch_ref;
        if (m_surf_fill_sketch_ref >= 0 && m_surf_fill_sketch_ref < int(m_doc.features.size()))
            m_surf_fill_sketch_label->SetLabel(_L("Sketch: ") +
                wxString::FromUTF8(m_doc.features[m_surf_fill_sketch_ref].name));
        break;
    case CadFeatureType::SurfaceOffset:
        populate_sheet_body_choices(m_surf_offset_body);
        m_surf_offset_distance->SetValue(f.plane_offset);
        // target_body is a BODY index; the rows are sheets only, so match, don't index.
        select_sheet_choice(m_surf_offset_body, f.target_body);
        break;
    case CadFeatureType::ThickenSurface:
        populate_sheet_body_choices(m_surf_thicken_body);
        m_surf_thicken_thickness->SetValue(f.thicken_thickness);
        m_surf_thicken_flip->SetValue(f.thicken_flip);
        select_sheet_choice(m_surf_thicken_body, f.target_body);
        break;
    case CadFeatureType::Transform: {
        fill_body_choice(m_xf_body, m_edit_index, f.target_body);
        m_xf_dx->SetValue(f.xf_translate.x());
        m_xf_dy->SetValue(f.xf_translate.y());
        m_xf_dz->SetValue(f.xf_translate.z());
        const int ax = (std::abs(f.xf_axis.x()) > 0.5) ? 0 : (std::abs(f.xf_axis.y()) > 0.5) ? 1 : 2;
        m_xf_axis->SetSelection(ax);
        m_xf_angle->SetValue(f.xf_angle_deg);
        m_xf_pivot_x->SetValue(f.xf_pivot.x());
        m_xf_pivot_y->SetValue(f.xf_pivot.y());
        m_xf_pivot_z->SetValue(f.xf_pivot.z());
        m_xf_copy->SetValue(f.xf_copy);
        break;
    }
    case CadFeatureType::Mirror: {
        fill_body_choice(m_mirror_body, m_edit_index, f.target_body);
        populate_plane_choices(m_mirror_plane);
        m_mirror_plane->SetSelection(index_from_plane(f.plane));
        m_mirror_keep->SetValue(f.mirror_keep_original);
        break;
    }
    case CadFeatureType::Thicken: {
        fill_body_choice(m_thicken_body, m_edit_index, f.target_body);
        m_sel_solid_face = f.thicken_face;
        m_thicken_face_label->SetLabel(f.thicken_face >= 0
            ? wxString::Format(_L("Face %d"), f.thicken_face)
            : _L("(pick a solid face)"));
        m_thicken_thickness->SetValue(f.thicken_thickness);
        m_thicken_flip->SetValue(f.thicken_flip);
        break;
    }
    case CadFeatureType::Rib: {
        fill_body_choice(m_rib_body, m_edit_index, f.target_body);
        {
            m_rib_sketch->Clear();
            int pre_sel = wxNOT_FOUND;
            for (int i = 0; i < int(m_doc.features.size()); ++i) {
                if (m_doc.features[i].type == CadFeatureType::Sketch) {
                    const int pos = combo_append_index(m_rib_sketch,
                                                       wxString::FromUTF8(m_doc.features[i].name), i);
                    if (i == f.rib_sketch_ref) pre_sel = pos;
                }
            }
            if (pre_sel != wxNOT_FOUND) m_rib_sketch->SetSelection(pre_sel);
            else if (m_rib_sketch->GetCount() > 0) m_rib_sketch->SetSelection(0);
        }
        m_rib_entity->SetValue(f.rib_entity);
        m_rib_thickness->SetValue(f.rib_thickness);
        m_rib_depth->SetValue(f.rib_depth);
        break;
    }
    case CadFeatureType::Project: {
        fill_body_choice(m_proj_source_body, m_edit_index, f.project_source_body);
        populate_plane_choices(m_proj_plane);
        m_proj_plane->SetSelection(index_from_plane(f.plane));
        m_sel_solid_face = f.project_face;
        m_proj_face_label->SetLabel(f.project_face >= 0
            ? wxString::Format(_L("Face %d"), f.project_face)
            : _L("(all edges)"));
        break;
    }
    case CadFeatureType::DeleteFace: {
        fill_body_choice(m_del_face_body, m_edit_index, f.target_body);
        m_del_faces = f.delete_faces;
        {
            wxString s;
            for (size_t i = 0; i < m_del_faces.size(); ++i) {
                if (i > 0) s += ", ";
                s += wxString::Format("Face %d", m_del_faces[i]);
            }
            m_del_face_list->SetLabel(s.empty() ? _L("(none)") : s);
        }
        break;
    }
    case CadFeatureType::Helix:
        populate_plane_choices(m_helix_plane);
        m_helix_plane->SetSelection(index_from_plane(f.plane));
        m_helix_radius->SetValue(f.helix_radius);
        m_helix_pitch->SetValue(f.helix_pitch);
        m_helix_height->SetValue(f.helix_height);
        m_helix_left_handed->SetValue(f.helix_left_handed);
        m_helix_taper->SetValue(f.helix_taper_deg);
        break;
    case CadFeatureType::Mate:
        m_mate_kind->SetSelection(f.mate_kind);
        // CoordSys pickers are repopulated by open_tool(Mate); pre-selection
        // is done there too via the stored feature index.
        m_mate_offset->SetValue(f.mate_offset);
        m_mate_angle->SetValue(f.mate_angle);
        m_mate_flip->SetValue(f.mate_flip);
        break;
    default: break;
    }
}

void DesignPanel::on_edit_feature()
{
    int sel = tree_selection();
    if (sel == wxNOT_FOUND) {
        set_status(_L("Select a feature in the tree first"));
        m_status->Refresh();
        return;
    }
    const CadFeature& f = m_doc.features[sel];
    reset_edit_state();

    switch (f.type) {
    case CadFeatureType::Sketch:
        // Imported Text/SVG art has no editable sketch dialog — edit means
        // move / scale its placement instead, behind the same Confirm/Cancel gate as
        // the initial insert (Cancel = undo restores the prior placement).
        if (!f.imported_regions.empty()) {
            m_doc.checkpoint();   // undo boundary: re-placing imported art
            on_transform_imported(sel);
            m_insert_feat = sel;
            open_insert_card(wxString::FromUTF8(f.name));
            break;
        }
        m_edit_index = sel;
        if (!f.entities.empty()) {
            // Entity sketcher: re-open the geometry for full in-canvas editing (handles,
            // live quotes, regular-polygon drag) in the ENTITY sketch UI (the top sketch
            // toolbar + session card), NOT the legacy parametric card. The commit handler
            // replaces this feature in place (see m_edit_index). Hide its display overlay
            // so the live tool is the only copy drawn.
            set_ui_mode(UiMode::Sketch);
            if (m_viewport) {
                m_viewport->set_display_sketches({});
                m_viewport->edit_sketch(f.entities, f.entity_constraints, f.plane);
            }
            m_status->SetForegroundColour(wxNullColour);
            set_status(_L("Editing sketch — drag a handle or click a quote to edit"));
            m_status->Refresh();
        } else {
            load_feature_into_dialog(f);
            open_tool(Tool::Sketch);
        }
        break;
    case CadFeatureType::Extrude:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Extrude);
        break;
    case CadFeatureType::Fillet:
    case CadFeatureType::Chamfer:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Dressup);
        break;
    case CadFeatureType::Hole:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Hole);
        break;
    case CadFeatureType::Thread:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Thread);
        break;
    case CadFeatureType::Shell:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Shell);
        break;
    case CadFeatureType::Revolve:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Revolve);
        break;
    case CadFeatureType::Sweep:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Sweep);
        break;
    case CadFeatureType::Pattern:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Pattern);
        break;
    case CadFeatureType::Plane:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Plane);
        break;
    case CadFeatureType::Axis:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Axis);
        break;
    case CadFeatureType::CoordSys:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::CoordSys);
        break;
    case CadFeatureType::Loft:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Loft);
        break;
    case CadFeatureType::Draft:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Draft);
        break;
    case CadFeatureType::Boolean:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Boolean);
        break;
    case CadFeatureType::SurfaceExtrude:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::SurfaceExtrude);
        break;
    case CadFeatureType::SurfaceRevolve:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::SurfaceRevolve);
        break;
    case CadFeatureType::SurfaceLoft:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::SurfaceLoft);
        break;
    case CadFeatureType::SurfaceFill:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::SurfaceFill);
        break;
    case CadFeatureType::SurfaceOffset:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::SurfaceOffset);
        break;
    case CadFeatureType::ThickenSurface:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::ThickenSurface);
        break;
    case CadFeatureType::Transform:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Transform);
        break;
    case CadFeatureType::Mirror:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Mirror);
        break;
    case CadFeatureType::Thicken:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Thicken);
        break;
    case CadFeatureType::Rib:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Rib);
        break;
    case CadFeatureType::Project:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Project);
        break;
    case CadFeatureType::DeleteFace:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::DeleteFace);
        break;
    case CadFeatureType::Helix:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Helix);
        break;
    case CadFeatureType::Mate:
        m_edit_index = sel;
        m_mate_kind->SetSelection(f.mate_kind);
        m_mate_offset->SetValue(f.mate_offset);
        m_mate_angle->SetValue(f.mate_angle);
        m_mate_flip->SetValue(f.mate_flip);
        open_tool(Tool::Mate);   // populates CS pickers; pre-selects from current selection
        // Now set the re-edit cs_a/cs_b after populating (override the open_tool default)
        {
            int pre_a = wxNOT_FOUND, pre_b = wxNOT_FOUND;
            for (unsigned i = 0; i < m_mate_cs_a->GetCount(); ++i) {
                const auto* cd = m_mate_cs_a->GetClientData(i);
                if (cd && int(reinterpret_cast<intptr_t>(cd)) == f.mate_cs_a) pre_a = int(i);
            }
            for (unsigned i = 0; i < m_mate_cs_b->GetCount(); ++i) {
                const auto* cd = m_mate_cs_b->GetClientData(i);
                if (cd && int(reinterpret_cast<intptr_t>(cd)) == f.mate_cs_b) pre_b = int(i);
            }
            if (pre_a != wxNOT_FOUND) m_mate_cs_a->SetSelection(pre_a);
            if (pre_b != wxNOT_FOUND) m_mate_cs_b->SetSelection(pre_b);
        }
        break;
    case CadFeatureType::Cut:
        m_edit_index = sel;
        load_feature_into_dialog(f);
        open_tool(Tool::Cut);
        break;
    case CadFeatureType::Import:
        // An imported solid has no parameters to re-edit: its geometry is rigid data read
        // from the file, not something rebuilt from numbers. Repositioning it is Transform's
        // job, and that feature already exists — so point there rather than invent a dialog
        // that would only duplicate it. (Imported 2D Text/SVG art is different and IS
        // re-editable; it arrives as a Sketch feature with imported_regions, handled above.)
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("An imported solid has no parameters — use Transform to move or rotate it"));
        m_status->Refresh();
        break;
    default:
        // Every CadFeatureType now has a case. Kept as a guard so a type added later
        // announces itself instead of silently swallowing the Edit click.
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("This feature type can't be edited yet"));
        m_status->Refresh();
        break;
    }
}

void DesignPanel::on_export_step()
{
    // Bake any open feature preview first, so the STEP matches what is shown (mirrors on_commit).
    if (m_active != Tool::None)
        confirm_tool();
    if (m_doc.bodies.empty()) {
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("Nothing to export — add a feature first"));
        m_status->Refresh();
        return;
    }
    wxFileDialog dlg(this, _L("Export STEP"), wxEmptyString, "model.step",
                     "STEP files (*.step;*.stp)|*.step;*.stp",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;
    sync_body_xform();   // export bodies at their displayed Move-gizmo positions
    std::string err;
    const bool ok = m_doc.export_step(dlg.GetPath().ToUTF8().data(), m_body_xform, err);
    m_status->SetForegroundColour(ok ? wxColour(120, 210, 120) : wxColour(235, 110, 110));
    set_status(ok ? _L("Exported STEP")
                          : _L("STEP export failed: ") + wxString::FromUTF8(err));
    m_status->Refresh();
}

void DesignPanel::on_commit()
{
    // A feature tool open with a live preview ghost (e.g. a fillet being previewed) is
    // NOT yet part of the body. Apply it first so "Commit to Plate" ships exactly what
    // is shown on screen, not the pre-feature solid. (confirm_tool() applies + closes.)
    if (m_active != Tool::None)
        confirm_tool();

    if (m_doc.display_mesh.its.indices.empty()) {
        set_status(_L("Nothing to commit — add a feature first"));
        return;
    }
    ObjectList* obj_list = wxGetApp().obj_list();
    if (obj_list == nullptr)
        return;

    // Multi-body: ship each (visible) body as its own plate object so they arrive on the
    // slicer plate as independent, separately-arrangeable parts (Onshape "Commit all parts").
    // Hidden bodies are skipped — what you see on the Design plate is what gets committed.
    sync_body_visible();
    rebuild_disp_meshes();   // ship moved bodies at their Move-gizmo positions
    if (m_disp_body_meshes.size() > 1) {
        int committed = 0;
        for (size_t b = 0; b < m_disp_body_meshes.size(); ++b) {
            if (b < m_body_visible.size() && !m_body_visible[b]) continue;   // skip hidden
            if (m_disp_body_meshes[b].its.indices.empty()) continue;
            obj_list->load_mesh_object(m_disp_body_meshes[b],
                                       "Design Body " + std::to_string(b + 1));
            ++committed;
        }
        if (committed == 0) {   // every body hidden — nothing to ship
            set_status(_L("All bodies hidden — show one before committing"));
            return;
        }
    } else {
        obj_list->load_mesh_object(m_disp_pick_mesh, "Design Body");
    }

    // Persist the editable parametric recipe alongside the committed meshes so the
    // saved 3MF reopens with the full feature tree, not just the baked solid. An empty
    // doc clears it, keeping non-CAD projects clean.
    sync_recipe_to_model();

    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
}

CadFeature DesignPanel::build_candidate(Tool t) const
{
    CadFeature f;
    // EDIT MODE: a feature card edits only scalar parameters. The feature's structural
    // identity — profile source (sketch_ref / entities / picked face), its plane, the
    // up-to-face target and the target body — must be preserved from the feature being
    // edited, NOT re-derived from the live tool state (which still reflects the last *add*
    // flow). GUI extrudes carry their profile as `entities` with sketch_ref = -1, which the
    // card never restores, so a fresh rebuild produced an empty profile -> a misplaced new
    // box. Seed from the original; the cases below overlay card scalars and skip the
    // structural assignments while editing.
    const bool editing = (m_edit_index >= 0 && m_edit_index < int(m_doc.features.size()));
    if (editing)
        f = m_doc.features[m_edit_index];
    switch (t) {
    case Tool::Sketch:
        f.type   = CadFeatureType::Sketch;
        f.shape  = (m_shape->GetSelection() == 0) ? SketchShape::Rectangle : SketchShape::Circle;
        // The plane is STRUCTURAL, like Extrude's profile source. While EDITING it is preserved
        // from the seeded original — the card carries no plane control and the old combo silently
        // collapsed a face plane to a base plane through the modeling origin. While ADDING it
        // comes from what is picked in the viewport. snaporca-e1p.
        if (!editing) { wxString where; f.plane = sketch_plane_from_selection(where); }
        f.width  = m_width->GetValue();
        f.height = m_height->GetValue();
        f.radius = m_radius->GetValue();
        break;
    case Tool::Extrude:
        f.type        = CadFeatureType::Extrude;
        f.distance    = m_distance->GetValue();
        f.symmetric   = false;
        f.extrude_end = static_cast<ExtrudeEnd>(m_extrude_end->GetSelection());
        f.distance2   = m_distance2->GetValue();
        f.taper_deg   = m_taper->GetValue();
        f.flip        = m_flip->GetValue();
        f.mode       = (m_mode->GetSelection() == 0) ? BooleanMode::New
                     : (m_mode->GetSelection() == 1) ? BooleanMode::Add
                     : (m_mode->GetSelection() == 2) ? BooleanMode::Cut
                                                     : BooleanMode::Intersect;
        // Profile source + up-to-face target are structural: re-derive them from the live
        // tool state only when ADDING. While editing they are preserved from the seeded
        // original (the card changed only depth/taper/flip/mode).
        if (!editing) {
            f.up_to_face = (f.extrude_end == ExtrudeEnd::UpToFace) ? m_sel_solid_face : -1;
            if (m_extrude_face_src >= 0) {
                // Face-as-profile extrude: the kernel grabs the body face by id.
                f.extrude_src_face = m_extrude_face_src;
                f.sketch_ref       = -1;
            } else if (extrude_uses_loop()) {
                // Just the click-selected loop: carry its entity subset on the feature
                // (sketch_ref = -1 -> build_sketch_wire uses f.entities).
                f.sketch_ref = -1;
                f.entities   = m_viewport->selected_loop_entities();
                f.plane      = m_doc.features[m_extrude_sketch_ref].plane;
            } else {
                f.sketch_ref = m_extrude_sketch_ref;
                // On-face engraving Cut against the host body (matches the commit).
                if (m_extrude_sketch_ref >= 0 && m_extrude_sketch_ref < int(m_doc.features.size())
                    && m_doc.features[m_extrude_sketch_ref].import_on_face)
                    f.target_body = m_doc.features[m_extrude_sketch_ref].import_face_body;
            }
        }
        break;
    case Tool::Dressup:
        f.type         = (m_dressup_type->GetSelection() == 0) ? CadFeatureType::Fillet
                                                               : CadFeatureType::Chamfer;
        f.dressup_size = m_dressup_size->GetValue();
        f.face_group   = static_cast<FaceGroup>(m_face_group->GetSelection());
        // A click-selected solid edge overrides the face-group: dress THAT edge.
        f.dressup_edge = m_sel_solid_edge;   // -1 when no edge picked
        break;
    case Tool::Hole:
        f.type          = CadFeatureType::Hole;
        f.plane         = hole_plane();
        f.hole_diameter = m_hole_diameter->GetValue();
        f.hole_depth    = m_hole_depth->GetValue();
        f.hole_through  = m_hole_through->GetValue();
        f.hole_x        = m_hole_x->GetValue();
        f.hole_y        = m_hole_y->GetValue();
        if (m_hole_on_face) f.target_body = m_hole_face_body;   // preview the right body
        break;
    case Tool::Thread:
        f.type            = CadFeatureType::Thread;
        f.plane           = thread_plane();
        f.thread_radius   = m_thread_radius->GetValue() * 0.5;   // field = diameter -> kernel radius
        f.thread_pitch    = m_thread_pitch->GetValue();
        f.thread_height   = m_thread_height->GetValue();
        f.thread_depth    = m_thread_depth->GetValue();
        f.thread_internal = m_thread_internal->GetValue();
        f.thread_x        = m_thread_x->GetValue();
        f.thread_y        = m_thread_y->GetValue();
        if (m_thread_on_face) f.target_body = m_thread_face_body;   // tap the right body
        break;
    case Tool::Shell:
        f.type            = CadFeatureType::Shell;
        f.shell_thickness = m_shell_thickness->GetValue();
        // A picked solid face opens the shell there; -1 = closed hollow.
        f.shell_face      = (m_sel_solid_face >= 0) ? m_sel_solid_face : -1;
        break;
    case Tool::Draft:
        f.type        = CadFeatureType::Draft;
        f.draft_angle = m_draft_angle->GetValue();
        f.draft_face  = (m_sel_solid_face >= 0) ? m_sel_solid_face : -1;
        break;
    case Tool::Revolve:
        f.type          = CadFeatureType::Revolve;
        f.sketch_ref    = m_revolve_sketch_ref;
        f.revolve_angle = m_revolve_angle->GetValue();
        f.revolve_axis  = m_revolve_axis->GetSelection();
        f.flip          = m_revolve_flip->GetValue();
        f.mode          = static_cast<BooleanMode>(m_revolve_mode->GetSelection());
        break;
    case Tool::Sweep: {
        f.type           = CadFeatureType::Sweep;
        f.sketch_ref     = m_sweep_profile_ref;
        const int sel    = m_sweep_path ? m_sweep_path->GetSelection() : wxNOT_FOUND;
        f.sweep_path_ref = (sel != wxNOT_FOUND)
            ? int(reinterpret_cast<intptr_t>(m_sweep_path->GetClientData(sel))) : -1;
        f.mode           = static_cast<BooleanMode>(m_sweep_mode->GetSelection());
        break;
    }
    case Tool::Pattern:
        f.type             = CadFeatureType::Pattern;
        f.pattern_circular = (m_pattern_type->GetSelection() == 1);
        f.pattern_count    = int(m_pattern_count->GetValue());
        f.pattern_spacing  = m_pattern_spacing->GetValue();
        f.pattern_dir      = m_pattern_dir->GetSelection();
        f.pattern_angle    = m_pattern_angle->GetValue();
        break;
    case Tool::Plane:
        f.type             = CadFeatureType::Plane;
        f.plane_base       = m_plane_base->GetSelection();
        f.plane_offset     = m_plane_offset->GetValue();
        f.plane_angle_tilt = m_plane_tilt->GetValue();
        f.plane_axis       = m_plane_tilt_axis->GetSelection();
        apply_plane_refs(f);   // plane_type + face/edge refs + u/v size from the card
        break;
    case Tool::Axis:
        f.type = CadFeatureType::Axis;
        apply_axis_refs(f);
        break;
    case Tool::CoordSys:
        f.type = CadFeatureType::CoordSys;
        apply_coordsys_refs(f);
        break;
    case Tool::Loft: {
        f.type      = CadFeatureType::Loft;
        f.loft_ruled = m_loft_ruled->GetValue();
        f.mode      = static_cast<BooleanMode>(m_loft_mode->GetSelection());
        f.loft_profile_refs.clear();
        for (unsigned i = 0; i < m_loft_list->GetCount(); ++i)
            if (m_loft_list->IsChecked(i) && i < m_loft_sketch_idx.size())
                f.loft_profile_refs.push_back(m_loft_sketch_idx[i]);
        break;
    }
    case Tool::Boolean: {
        f.type           = CadFeatureType::Boolean;
        const int sel    = m_bool_op->GetSelection();
        f.mode           = (sel == 1) ? BooleanMode::Cut
                         : (sel == 2) ? BooleanMode::Intersect
                                      : BooleanMode::Add;   // 0 = Union
        f.target_body    = m_bool_target->GetSelection();
        f.bool_tool_body = m_bool_tool->GetSelection();
        f.bool_keep_tool = m_bool_keep->GetValue();
        f.bool_tolerance = m_bool_tol->GetValue();   // OCCT fuzzy: robust cut on near-coincident faces
        break;
    }
    case Tool::Cut:
        f.type           = CadFeatureType::Cut;
        f.plane          = plane_from_choice(m_cut_plane->GetSelection());
        f.cut_offset     = m_cut_offset->GetValue();
        f.cut_flip       = false;
        f.cut_keep_upper = true;   // always split: keep both pieces as separate bodies
        f.cut_keep_lower = true;
        f.target_body    = m_cut_target->GetSelection();
        break;
    case Tool::SurfaceExtrude:
        f.type      = CadFeatureType::SurfaceExtrude;
        f.sketch_ref = m_surf_extrude_sketch_ref;
        f.distance   = m_surf_extrude_distance->GetValue();
        break;
    case Tool::SurfaceRevolve:
        f.type         = CadFeatureType::SurfaceRevolve;
        f.sketch_ref   = m_surf_revolve_sketch_ref;
        f.revolve_angle = m_surf_revolve_angle->GetValue();
        f.revolve_axis = m_surf_revolve_axis->GetSelection();
        f.flip         = m_surf_revolve_flip->GetValue();
        break;
    case Tool::SurfaceLoft: {
        f.type      = CadFeatureType::SurfaceLoft;
        f.loft_ruled = m_surf_loft_ruled->GetValue();
        f.loft_profile_refs.clear();
        for (unsigned i = 0; i < m_surf_loft_list->GetCount(); ++i)
            if (m_surf_loft_list->IsChecked(i) && i < m_surf_loft_sketch_idx.size())
                f.loft_profile_refs.push_back(m_surf_loft_sketch_idx[i]);
        break;
    }
    case Tool::SurfaceFill:
        f.type      = CadFeatureType::SurfaceFill;
        f.sketch_ref = m_surf_fill_sketch_ref;
        break;
    case Tool::SurfaceOffset: {
        f.type = CadFeatureType::SurfaceOffset;
        f.target_body = sheet_choice_body(m_surf_offset_body);
        f.plane_offset = m_surf_offset_distance->GetValue();
        break;
    }
    case Tool::ThickenSurface: {
        f.type = CadFeatureType::ThickenSurface;
        f.target_body       = sheet_choice_body(m_surf_thicken_body);
        f.thicken_thickness = m_surf_thicken_thickness->GetValue();
        f.thicken_flip      = m_surf_thicken_flip->GetValue();
        break;
    }
    case Tool::Transform: {
        f.type = CadFeatureType::Transform;
        const int sel = m_xf_body->GetSelection();
        f.target_body   = (sel != wxNOT_FOUND) ? sel : -1;
        f.xf_translate  = Vec3d(m_xf_dx->GetValue(), m_xf_dy->GetValue(), m_xf_dz->GetValue());
        const int ax = m_xf_axis->GetSelection();
        f.xf_axis   = (ax == 0) ? Vec3d(1, 0, 0) : (ax == 1) ? Vec3d(0, 1, 0) : Vec3d(0, 0, 1);
        f.xf_pivot  = Vec3d(m_xf_pivot_x->GetValue(), m_xf_pivot_y->GetValue(), m_xf_pivot_z->GetValue());
        f.xf_angle_deg = m_xf_angle->GetValue();
        f.xf_copy      = m_xf_copy->GetValue();
        break;
    }
    case Tool::Mirror: {
        f.type = CadFeatureType::Mirror;
        const int sel = m_mirror_body->GetSelection();
        f.target_body          = (sel != wxNOT_FOUND) ? sel : -1;
        f.plane                = plane_from_choice(m_mirror_plane->GetSelection());
        f.mirror_keep_original = m_mirror_keep->GetValue();
        f.mode                 = f.mirror_keep_original ? BooleanMode::New : BooleanMode::Add;
        break;
    }
    case Tool::Thicken: {
        f.type = CadFeatureType::Thicken;
        const int sel = m_thicken_body->GetSelection();
        f.target_body       = (sel != wxNOT_FOUND) ? sel : -1;
        f.thicken_face      = (m_sel_solid_face >= 0) ? m_sel_solid_face : -1;
        f.thicken_thickness = m_thicken_thickness->GetValue();
        f.thicken_flip      = m_thicken_flip->GetValue();
        break;
    }
    case Tool::Rib: {
        f.type = CadFeatureType::Rib;
        const int bsel = m_rib_body->GetSelection();
        f.target_body      = (bsel != wxNOT_FOUND) ? bsel : -1;
        const int ssel = m_rib_sketch->GetSelection();
        f.rib_sketch_ref = (ssel != wxNOT_FOUND)
                               ? int(reinterpret_cast<intptr_t>(m_rib_sketch->GetClientData(ssel))) : -1;
        f.rib_entity     = m_rib_entity->GetValue();
        f.rib_thickness  = m_rib_thickness->GetValue();
        f.rib_depth      = m_rib_depth->GetValue();
        break;
    }
    case Tool::Project: {
        f.type = CadFeatureType::Project;
        const int sel = m_proj_source_body->GetSelection();
        f.project_source_body = (sel != wxNOT_FOUND) ? sel : -1;
        f.plane               = plane_from_choice(m_proj_plane->GetSelection());
        if (m_sel_solid_face >= 0) {
            f.project_face = m_sel_solid_face;
        } else {
            f.project_face = -1;
        }
        f.project_edges.clear();  // empty => use project_face
        break;
    }
    case Tool::DeleteFace: {
        f.type = CadFeatureType::DeleteFace;
        const int sel = m_del_face_body->GetSelection();
        f.target_body  = (sel != wxNOT_FOUND) ? sel : -1;
        f.delete_faces = m_del_faces;
        break;
    }
    case Tool::Helix: {
        f.type = CadFeatureType::Helix;
        f.plane              = plane_from_choice(m_helix_plane->GetSelection());
        f.helix_radius       = m_helix_radius->GetValue();
        f.helix_pitch        = m_helix_pitch->GetValue();
        f.helix_height       = m_helix_height->GetValue();
        f.helix_left_handed  = m_helix_left_handed->GetValue();
        f.helix_taper_deg    = m_helix_taper->GetValue();
        break;
    }
    case Tool::Mate: {
        f.type       = CadFeatureType::Mate;
        f.mate_kind  = m_mate_kind->GetSelection();
        f.mate_cs_a  = m_mate_cs_a->GetSelection() != wxNOT_FOUND
                           ? int(reinterpret_cast<intptr_t>(m_mate_cs_a->GetClientData(m_mate_cs_a->GetSelection()))) : -1;
        f.mate_cs_b  = m_mate_cs_b->GetSelection() != wxNOT_FOUND
                           ? int(reinterpret_cast<intptr_t>(m_mate_cs_b->GetClientData(m_mate_cs_b->GetSelection()))) : -1;
        f.mate_offset = m_mate_offset->GetValue();
        f.mate_angle  = m_mate_angle->GetValue();
        f.mate_flip   = m_mate_flip->GetValue();
        break;
    }
    case Tool::Insert:   // imported art is committed by add_imported_sketch, not build_candidate
    case Tool::None:
        break;
    }
    // Boolean drives its own target/tool body from the card; every other tool targets the
    // picked body (face-extrude reads its source face there, dress-up / hole / boolean-mode
    // extrude mutate it). -1 when nothing is picked => auto (last body).
    // Targeting the picked body is an ADD-time concern; while editing, the original feature's
    // target_body is preserved from the seed (the card did not re-pick a body).
    // HAZARD: this is a negative list, so a tool that picks its own body is broken by OMISSION —
    // the card computes target_body and this line then throws it away. SurfaceOffset and
    // ThickenSurface were missing: both read a SHEET body from their own combo and both had it
    // overwritten with the picked SOLID's index. Any new card that picks a body belongs here.
    if (!editing && m_active != Tool::Boolean && m_active != Tool::Cut
        && m_active != Tool::Transform && m_active != Tool::Mirror && m_active != Tool::Thicken
        && m_active != Tool::DeleteFace && m_active != Tool::Rib && m_active != Tool::Project
        && m_active != Tool::Helix && m_active != Tool::SurfaceOffset
        && m_active != Tool::ThickenSurface)
        f.target_body = m_sel_solid_body;
    return f;
}

// Resolve the active Extrude's profile plane + a representative 2D centroid (arrow anchor)
// and push them to the viewport gizmo. Self-gates: clears the gizmo unless Extrude is open.
// Keep the Dress-up card honest about what Confirm will actually round: the single edge the
// user picked in the viewport, or the face-group. build_dressup reads m_sel_solid_edge first and
// only falls back to the group, so when an edge is picked the group combo is inert — grey it out
// rather than leave it showing a value it will not use.
void DesignPanel::sync_dressup_target()
{
    if (m_dressup_edge_label == nullptr) return;
    const bool have_edge = (m_sel_solid_edge >= 0);
    m_dressup_edge_label->SetLabel(have_edge
        ? wxString::Format(_L("Edge %d"), m_sel_solid_edge)
        : _L("(no edge picked — group below)"));
    if (m_face_group != nullptr) m_face_group->Enable(!have_edge);
    m_dressup_edge_label->Refresh();
}

void DesignPanel::update_fillet_gizmo()
{
    if (!m_viewport) return;
    // Only while the Fillet/Chamfer card is open AND a solid EDGE is the target. Face-group
    // dress-up (no picked edge) keeps the docked card with no in-canvas handle. The body centroid
    // comes from the transformed display mesh so it matches the (transformed) edge sample points.
    const bool ok = (m_active == Tool::Dressup) && m_sel_solid_edge >= 0
                    && m_sel_solid_body >= 0 && m_sel_solid_body < int(m_disp_body_meshes.size());
    if (!ok) { m_viewport->clear_fillet_gizmo(); return; }
    const Vec3d centroid = m_disp_body_meshes[m_sel_solid_body].bounding_box().center();
    m_viewport->begin_fillet_gizmo(centroid, m_dressup_size->GetValue());
}

// Push the active Hole card's plane + position + diameter/depth to the viewport gizmo.
// Grey the FEATURE buttons whose tool cannot run yet, and say why in the tooltip (snaporca-o9j).
// Tommaso reported the array controls as MISSING; they were not, but Pattern with no body
// accepted the click, opened nothing, and wrote its refusal somewhere other than where the click
// happened — from the user's seat that is indistinguishable from a dead button. A control that
// cannot act should look like it cannot act, before it is pressed.
//
// Only the three top-level buttons that carry a body-count guard are gated. The same guard also
// appears on rows INSIDE the flyouts, and those stay live: a drawer holds sketch-only entries too,
// so disabling the drawer would hide tools that are perfectly usable. Their refusal message is
// still written, and now next to the geometry.
//
// The keyboard shortcuts (Shift+N / Shift+X / Shift+B) deliberately keep running the guarded
// action rather than being gated: a key press has no greyed-out state to see, so the sentence is
// the only feedback there is.
void DesignPanel::update_body_gates()
{
    const int n = int(m_doc.bodies.size());
    for (const BodyGate& g : m_body_gates) {
        if (g.btn == nullptr) continue;
        const bool live = n >= g.min_bodies;
        g.btn->Enable(live);
        g.btn->SetToolTip(live ? g.tip_live : g.tip_gated);
    }
}

// Self-gates: clears the gizmo unless the Hole card is open.
void DesignPanel::update_hole_gizmo()
{
    if (!m_viewport) return;
    if (m_active != Tool::Hole) { m_viewport->clear_hole_gizmo(); return; }
    const SketchPlane plane = hole_plane();
    m_viewport->set_hole_face_bounds(m_hole_has_bounds, m_hole_umin, m_hole_umax,
                                     m_hole_vmin, m_hole_vmax);
    m_viewport->begin_hole_gizmo(plane,
                                 m_hole_x->GetValue(), m_hole_y->GetValue(),
                                 m_hole_diameter->GetValue(), m_hole_depth->GetValue(),
                                 m_hole_through->GetValue());
}

// Push the active Thread card's plane + position + radius/length to the viewport gizmo.
// Self-gates: clears the gizmo unless the Thread card is open.
void DesignPanel::update_thread_gizmo()
{
    if (!m_viewport) return;
    if (m_active != Tool::Thread) { m_viewport->clear_thread_gizmo(); return; }
    const SketchPlane plane = thread_plane();
    m_viewport->begin_thread_gizmo(plane,
                                   m_thread_x->GetValue(), m_thread_y->GetValue(),
                                   m_thread_radius->GetValue() * 0.5, m_thread_height->GetValue());
}

// Anchor an inward thickness arrow at the picked open face's centroid (along -outward-normal).
// Self-gates: clears unless the Shell card is open AND a face is picked. The face centroid/normal
// come from the kernel shape, then carry the body's display-only Move transform.
void DesignPanel::update_shell_gizmo()
{
    if (!m_viewport) return;
    const int b = m_sel_solid_body;
    const bool ok = (m_active == Tool::Shell) && m_sel_solid_face >= 0
                    && b >= 0 && b < int(m_doc.bodies.size());
    if (!ok) { m_viewport->clear_shell_gizmo(); return; }
    const TopoDS_Face fc = GeometryEngine::face_by_index(m_doc.bodies[b].shape, m_sel_solid_face);
    if (fc.IsNull()) { m_viewport->clear_shell_gizmo(); return; }
    Vec3d c = GeometryEngine::face_centroid_world(fc);
    Vec3d n = GeometryEngine::face_normal_world(fc);
    sync_body_xform();
    if (b < int(m_body_xform.size())) {
        c = m_body_xform[b] * c;
        n = m_body_xform[b].linear() * n;
    }
    if (n.norm() < 1e-9) { m_viewport->clear_shell_gizmo(); return; }
    // Arrow points inward (into the wall): -outward normal.
    m_viewport->begin_shell_gizmo(c, (-n).normalized(), m_shell_thickness->GetValue());
}

void DesignPanel::update_revolve_gizmo()
{
    if (!m_viewport) return;
    if (m_active != Tool::Revolve
        || m_revolve_sketch_ref < 0 || m_revolve_sketch_ref >= int(m_doc.features.size())) {
        m_viewport->clear_revolve_gizmo();
        return;
    }
    const CadFeature& sk = m_doc.features[m_revolve_sketch_ref];
    // Profile centroid in sketch coords (same rule as the Extrude gizmo: average entity centres,
    // else profile points, else the plane origin for primitive shapes).
    Vec2d centroid(0, 0);
    if (!sk.entities.empty()) {
        Vec2d acc(0, 0); int n = 0;
        for (const SketchEntity& e : sk.entities) {
            switch (e.type) {
            case SketchEntity::Type::Line:    acc += 0.5 * (e.p0 + e.p1); ++n; break;
            case SketchEntity::Type::Arc:
            case SketchEntity::Type::EllipseArc:
            case SketchEntity::Type::Circle:
            case SketchEntity::Type::Ellipse: acc += e.center; ++n; break;
            case SketchEntity::Type::Point:   acc += e.p0; ++n; break;
            case SketchEntity::Type::BSpline:
                if (!e.ctrl.empty()) {
                    Vec2d s(0, 0); for (const Vec2d& q : e.ctrl) s += q;
                    acc += s / double(e.ctrl.size()); ++n;
                }
                break;
            }
        }
        if (n > 0) centroid = acc / double(n);
    } else if (!sk.profile.points.empty()) {
        for (const Vec2d& p : sk.profile.points) centroid += p;
        centroid /= double(sk.profile.points.size());
    }
    m_viewport->begin_revolve_gizmo(sk.plane, centroid, m_revolve_axis->GetSelection(),
                                    m_revolve_angle->GetValue(), m_revolve_flip->GetValue());
}

void DesignPanel::update_draft_gizmo()
{
    if (!m_viewport) return;
    if (m_active != Tool::Draft || m_sel_solid_face < 0 || m_sel_solid_body < 0
        || m_sel_solid_body >= int(m_doc.bodies.size())) {
    m_viewport->clear_draft_gizmo();
        return;
    }
    const TopoDS_Face f = GeometryEngine::face_by_index(m_doc.bodies[m_sel_solid_body].shape, m_sel_solid_face);
    const Vec3d c = GeometryEngine::face_centroid_world(f);
    const Vec3d n = GeometryEngine::face_normal_world(f);
    m_viewport->set_draft_gizmo(c, n, m_draft_angle->GetValue());
}

void DesignPanel::update_cut_gizmo()
{
    if (!m_viewport) return;
    if (m_active != Tool::Cut) { m_viewport->clear_cut_gizmo(); return; }
    const int bi = m_cut_target ? m_cut_target->GetSelection() : -1;
    if (bi < 0 || bi >= int(m_doc.bodies.size()) || bi >= int(m_doc.display_body_meshes.size())) {
        m_viewport->clear_cut_gizmo();
        return;
    }
    const SketchPlane plane = plane_from_choice(m_cut_plane->GetSelection());
    const BoundingBoxf3 bb = m_doc.display_body_meshes[bi].bounding_box();
    const Vec3d center = bb.center();
    const double half = std::max(0.5 * (bb.max - bb.min).norm(), 10.0);
    m_viewport->set_cut_gizmo(plane, m_cut_offset->GetValue(), center, half);
}

void DesignPanel::update_operand_highlight()
{
    if (!m_viewport) return;
    // default: nothing highlighted
    int bt = -1, bl = -1;
    std::vector<std::pair<int, ColorRGBA>> sk;
    if (m_active == Tool::Boolean) {
        if (m_bool_target) bt = m_bool_target->GetSelection();
        if (m_bool_tool)   bl = m_bool_tool->GetSelection();
    } else if (m_active == Tool::Sweep) {
        if (m_sweep_profile_ref >= 0) sk.emplace_back(m_sweep_profile_ref, ColorRGBA(0.30f, 0.85f, 1.0f, 1.0f)); // profile = cyan
        if (m_sweep_path_ref    >= 0) sk.emplace_back(m_sweep_path_ref,    ColorRGBA(1.00f, 0.40f, 0.90f, 1.0f)); // path = magenta
    } else if (m_active == Tool::Loft) {
        // checked rows of m_loft_list map to feature indices via m_loft_sketch_idx
        // (exactly as build_candidate(Tool::Loft) reads them).
        if (m_loft_list)
            for (unsigned i = 0; i < m_loft_list->GetCount(); ++i)
                if (m_loft_list->IsChecked(i) && i < m_loft_sketch_idx.size())
                    sk.emplace_back(m_loft_sketch_idx[i], ColorRGBA(0.40f, 0.90f, 0.50f, 1.0f)); // profiles = green
    }
    m_viewport->set_operand_bodies(bt, bl);
    m_viewport->set_highlight_sketches(std::move(sk));
}

void DesignPanel::update_pattern_gizmo()
{
    if (!m_viewport) return;
    if (m_active != Tool::Pattern || m_doc.display_body_meshes.empty()) {
        m_viewport->clear_pattern_gizmo();
        return;
    }
    // Pattern operates in the default world XY plane (matches the kernel: linear along world X/Y,
    // circular about world Z through the origin). Anchor on the target body's bbox centre; if that
    // body carries a display-only Move transform, shift the plane origin + anchor by it so the
    // gizmo sits on the body where the ghost copies actually appear.
    const int b = (m_sel_solid_body >= 0 && m_sel_solid_body < int(m_doc.display_body_meshes.size()))
                  ? m_sel_solid_body : int(m_doc.display_body_meshes.size()) - 1;
    SketchPlane plane;   // world XY axes by default
    Vec3d base = m_doc.display_body_meshes[b].bounding_box().center();
    if (b < int(m_body_xform.size())) {
        plane.origin = m_body_xform[b].translation();
        base = m_body_xform[b] * base;
    }
    m_viewport->begin_pattern_gizmo(plane, base, m_pattern_type->GetSelection() == 1,
                                    int(m_pattern_count->GetValue()), m_pattern_dir->GetSelection(),
                                    m_pattern_spacing->GetValue(), m_pattern_angle->GetValue());
}

void DesignPanel::update_extrude_gizmo()
{
    if (!m_viewport) return;
    if (m_active != Tool::Extrude && m_active != Tool::SurfaceExtrude
        && m_active != Tool::Thicken && m_active != Tool::Rib
        && m_active != Tool::SurfaceOffset && m_active != Tool::ThickenSurface) {
        m_viewport->clear_extrude_gizmo(); return;
    }

    // SurfaceOffset and ThickenSurface: one distance each, applied to a whole SHEET body chosen
    // from a combo. A sheet has no single normal, so there is no frame to anchor an arrow on —
    // which is why these were the two tools left with no handle at all.
    //
    // The way out is not to change what the tool operates on. Both still offset or thicken the
    // ENTIRE sheet named in the combo; the picked face only says WHERE TO STAND THE ARROW. So
    // the arrow appears whenever the user has a face of that sheet under selection, and its
    // absence costs nothing — the card alone still works exactly as before. Purely additive, so
    // no existing flow changes and there is no new precondition to learn.
    if (m_active == Tool::SurfaceOffset || m_active == Tool::ThickenSurface) {
        const bool off = (m_active == Tool::SurfaceOffset);
        const int sheet = sheet_choice_body(off ? m_surf_offset_body : m_surf_thicken_body);
        // Only when the picked face belongs to the sheet the tool will actually act on:
        // an arrow standing on a different body would name the wrong thing.
        if (sheet < 0 || sheet >= int(m_doc.bodies.size())
            || m_sel_solid_body != sheet || m_sel_solid_face < 0) {
            m_viewport->clear_extrude_gizmo(); return;
        }
        TopoDS_Face f = GeometryEngine::face_by_index(m_doc.bodies[sheet].shape, m_sel_solid_face);
        if (f.IsNull()) { m_viewport->clear_extrude_gizmo(); return; }
        SketchPlane plane = SketchPlane::from_face(f);
        Vec3d c = GeometryEngine::face_centroid_world(f);
        Vec3d n = GeometryEngine::face_normal_world(f);
        if (f.Orientation() == TopAbs_REVERSED) n = -n;
        sync_body_xform();
        if (sheet < int(m_body_xform.size())) {
            c = m_body_xform[sheet] * c;
            n = m_body_xform[sheet].linear() * n;
        }
        plane.origin = c;
        if (n.norm() > 1e-9) plane.normal = n.normalized();
        wxSpinCtrlDouble* spin = off ? m_surf_offset_distance : m_surf_thicken_thickness;
        m_viewport->set_extrude_gizmo(plane, Vec2d(0, 0), spin ? spin->GetValue() : 0.0,
                                      0.0, false,
                                      !off && m_surf_thicken_flip && m_surf_thicken_flip->GetValue());
        return;
    }

    if (m_active == Tool::Rib) {
        // Rib's DEPTH is a distance along the sketch plane normal — the same arrow again,
        // anchored at the midpoint of the line the rib is built on rather than at a profile
        // centroid, because a rib's line is the whole profile.
        //
        // This covers only half of L2 for Rib: the THICKNESS is an in-plane offset either side
        // of that line and has no handle, which needs an affordance that does not exist yet.
        // Half a tool made draggable is still strictly better than none — the remaining half is
        // filed separately rather than left implied.
        const int ssel = m_rib_sketch ? m_rib_sketch->GetSelection() : wxNOT_FOUND;
        const int ref  = (ssel != wxNOT_FOUND)
                             ? int(reinterpret_cast<intptr_t>(m_rib_sketch->GetClientData(ssel))) : -1;
        if (ref < 0 || ref >= int(m_doc.features.size())) { m_viewport->clear_extrude_gizmo(); return; }
        const CadFeature& sk = m_doc.features[ref];
        const int ei = m_rib_entity ? m_rib_entity->GetValue() : 0;
        if (ei < 0 || ei >= int(sk.entities.size())) { m_viewport->clear_extrude_gizmo(); return; }
        const SketchEntity& e = sk.entities[ei];
        const Vec2d mid = (e.type == SketchEntity::Type::Line) ? Vec2d(0.5 * (e.p0 + e.p1))
                                                               : Vec2d(e.center);
        m_viewport->set_extrude_gizmo(sk.plane, mid,
                                      m_rib_depth ? m_rib_depth->GetValue() : 0.0,
                                      0.0, false, false);
        return;
    }

    if (m_active == Tool::SurfaceExtrude) {
        const int ref = m_surf_extrude_sketch_ref;
        if (ref < 0 || ref >= int(m_doc.features.size())) { m_viewport->clear_extrude_gizmo(); return; }
        const CadFeature& sk = m_doc.features[ref];
        Vec2d c(0, 0);
        if (!sk.profile.points.empty()) {
            for (const Vec2d& p : sk.profile.points) c += p;
            c /= double(sk.profile.points.size());
        }
        m_viewport->set_extrude_gizmo(sk.plane, c,
                                      m_surf_extrude_distance ? m_surf_extrude_distance->GetValue() : 0.0,
                                      0.0, false, false);
        return;
    }

    if (m_active == Tool::Thicken) {
        const int b = m_sel_solid_body;
        if (b < 0 || b >= int(m_doc.bodies.size()) || m_sel_solid_face < 0) {
            m_viewport->clear_extrude_gizmo(); return;
        }
        TopoDS_Face f = GeometryEngine::face_by_index(m_doc.bodies[b].shape, m_sel_solid_face);
        if (f.IsNull()) { m_viewport->clear_extrude_gizmo(); return; }
        SketchPlane plane = SketchPlane::from_face(f);
        Vec3d c = GeometryEngine::face_centroid_world(f);
        Vec3d n = GeometryEngine::face_normal_world(f);
        if (f.Orientation() == TopAbs_REVERSED) n = -n;   // outward
        sync_body_xform();
        if (b < int(m_body_xform.size())) { c = m_body_xform[b] * c; n = m_body_xform[b].linear() * n; }
        plane.origin = c;
        if (n.norm() > 1e-9) plane.normal = n.normalized();
        m_viewport->set_extrude_gizmo(plane, Vec2d(0, 0),
                                      m_thicken_thickness ? m_thicken_thickness->GetValue() : 0.0,
                                      0.0, false,
                                      m_thicken_flip && m_thicken_flip->GetValue());
        return;
    }

    SketchPlane plane;
    std::vector<SketchEntity> ents;
    Vec2d centroid(0, 0);
    bool  have = false, have_centroid = false;
    if (extrude_uses_loop()) {
        plane = m_doc.features[m_extrude_sketch_ref].plane;
        ents  = m_viewport->selected_loop_entities();
        have  = true;
    } else if (m_extrude_sketch_ref >= 0 && m_extrude_sketch_ref < int(m_doc.features.size())) {
        const CadFeature& sk = m_doc.features[m_extrude_sketch_ref];
        plane = sk.plane;
        ents  = sk.entities;
        have  = true;
        if (ents.empty() && !sk.profile.points.empty()) {
            for (const Vec2d& p : sk.profile.points) centroid += p;
            centroid /= double(sk.profile.points.size());
            have_centroid = true;
        }
        // Primitive shape sketches (no entities/profile) are centred at the plane origin -> (0,0).
    } else if (m_extrude_face_src >= 0 && !m_doc.bodies.empty()) {
        // Face-as-profile (push/pull): anchor the arrow at the picked face's CENTROID, pointing
        // along its outward normal. The face id is LOCAL to the owner body — route_feature reads
        // it from `context` (the target, else the last body) — so look it up on that SAME body,
        // never the whole-document compound (m_doc.body). from_face's plane origin sits at the
        // plane's canonical point near the world origin, NOT on the face, which is why the arrow
        // used to land on the bed. Carry the body's display Move transform so the arrow sits where
        // the body is actually shown.
        const int b = int(m_doc.bodies.size()) - 1;   // matches route_feature's default context
        TopoDS_Face srcf = GeometryEngine::face_by_index(m_doc.bodies[b].shape, m_extrude_face_src);
        if (!srcf.IsNull()) {
            plane = SketchPlane::from_face(srcf);
            Vec3d c = GeometryEngine::face_centroid_world(srcf);
            Vec3d n = GeometryEngine::face_normal_world(srcf);
            if (srcf.Orientation() == TopAbs_REVERSED) n = -n;   // outward, matches the kernel push
            sync_body_xform();
            if (b < int(m_body_xform.size())) { c = m_body_xform[b] * c; n = m_body_xform[b].linear() * n; }
            plane.origin = c;
            if (n.norm() > 1e-9) plane.normal = n.normalized();
            have = true;
        }
    }
    if (!have) { m_viewport->clear_extrude_gizmo(); return; }

    if (!ents.empty()) {
        Vec2d acc(0, 0); int n = 0;
        for (const SketchEntity& e : ents) {
            switch (e.type) {
            case SketchEntity::Type::Line:    acc += 0.5 * (e.p0 + e.p1); ++n; break;
            case SketchEntity::Type::Arc:
            case SketchEntity::Type::EllipseArc:
            case SketchEntity::Type::Circle:
            case SketchEntity::Type::Ellipse: acc += e.center; ++n; break;
            case SketchEntity::Type::Point:   acc += e.p0; ++n; break;
            case SketchEntity::Type::BSpline:
                if (!e.ctrl.empty()) {
                    Vec2d s(0, 0); for (const Vec2d& q : e.ctrl) s += q;
                    acc += s / double(e.ctrl.size()); ++n;
                }
                break;
            }
        }
        if (n > 0) { centroid = acc / double(n); have_centroid = true; }
    }
    (void)have_centroid;   // centroid defaults to (0,0) for primitive sketches

    const ExtrudeEnd end = static_cast<ExtrudeEnd>(m_extrude_end->GetSelection());
    m_viewport->set_extrude_gizmo(plane, centroid,
                                  m_distance->GetValue(), m_distance2->GetValue(),
                                  end == ExtrudeEnd::TwoSided, m_flip->GetValue());
}

void DesignPanel::refresh_datum_planes()
{
    if (!m_viewport) return;
    std::vector<SketchPlane> dplanes;
    for (const auto& dp : m_doc.resolve_datum_planes()) dplanes.push_back(dp.second);
    // Parallel per-plane extents, in the SAME order resolve_datum_planes emits
    // (enabled Plane features, document order), so each datum draws at its u/v size.
    std::vector<Vec2d> dsizes;
    for (const auto& f : m_doc.features)
        if (f.type == CadFeatureType::Plane && f.enabled)
            dsizes.emplace_back(f.plane_u_size, f.plane_v_size);
    m_viewport->set_datum_planes(std::move(dplanes), std::move(dsizes));
    refresh_mate_connectors();
}

// Feed the connector frames to the viewport. resolve_datum_coordsys() emits them in document order
// over enabled CoordSys features, so the feature index can be recovered by walking the same filter —
// which is what tells us whether a given frame is the one the open Mate card will DRIVE.
void DesignPanel::refresh_mate_connectors()
{
    if (!m_viewport) return;
    // Polarity is a property of the MATE, not of an open dialog: a connector that some committed
    // mate drives must read as driven whenever it is on screen, or the glyph only tells the truth
    // while a card happens to be open. The card, when open, wins — it is the live intent.
    std::map<int, int> role_by_feature;                       // feature index -> 1 fixed, 2 driven
    for (const CadFeature& mf : m_doc.features) {
        if (mf.type != CadFeatureType::Mate || !mf.enabled) continue;
        if (mf.mate_cs_a >= 0) role_by_feature[mf.mate_cs_a] = 1;
        if (mf.mate_cs_b >= 0) role_by_feature[mf.mate_cs_b] = 2;
    }
    const int cs_a = (m_active == Tool::Mate && m_mate_cs_a) ? m_mate_cs_a->GetSelection() : -1;
    const int cs_b = (m_active == Tool::Mate && m_mate_cs_b) ? m_mate_cs_b->GetSelection() : -1;

    std::vector<DesignSketchTool::MateConnectorGlyph> out;
    const auto frames = m_doc.resolve_datum_coordsys();
    size_t k = 0;
    for (size_t i = 0; i < m_doc.features.size() && k < frames.size(); ++i) {
        const CadFeature& f = m_doc.features[i];
        if (f.type != CadFeatureType::CoordSys || !f.enabled) continue;
        const auto& fr = frames[k];
        const int ordinal = int(k);
        ++k;
        if (!fr.error.empty()) continue;               // unresolved: draw nothing, never a guess
        DesignSketchTool::MateConnectorGlyph g;
        g.origin = fr.origin;
        g.x      = fr.x;
        g.y      = fr.y;
        const auto it = role_by_feature.find(int(i));
        g.role   = (ordinal == cs_b) ? 2 : (ordinal == cs_a ? 1
                 : (it != role_by_feature.end() ? it->second : 0));
        // A face-only connector whose face gave no usable in-plane edge fell back to the hint; that
        // is the case the glyph has to confess rather than absorb.
        g.roll_undefined = (f.coordsys_type == CoordSysType::FaceAndDirection
                            && f.coordsys_edge < 0);
        out.push_back(g);
    }
    m_viewport->set_mate_connectors(std::move(out));
}

void DesignPanel::update_datum_gizmo()
{
    if (!m_viewport) return;
    if (m_active != Tool::Plane) { m_viewport->clear_datum_gizmo(); update_reference_planes(); return; }

    // Resolve the candidate plane's FRAME against the doc. resolve_datum_planes() is const and
    // doesn't rebuild bodies, so transiently swap/append the candidate to read its resolved frame,
    // then restore — works for both a fresh (uncommitted) plane and an edit of a committed one.
    CadFeature f = build_candidate(Tool::Plane);
    const bool editing = (m_edit_index >= 0 && m_edit_index < int(m_doc.features.size()) &&
                          m_doc.features[m_edit_index].type == CadFeatureType::Plane);
    CadFeature saved;
    int slot;
    if (editing) { saved = m_doc.features[m_edit_index]; m_doc.features[m_edit_index] = f; slot = m_edit_index; }
    else         { m_doc.features.push_back(f); slot = int(m_doc.features.size()) - 1; }

    auto datums = m_doc.resolve_datum_planes();
    // Ordinal of `slot` among enabled Plane features = its index in the resolved list.
    int ord = -1;
    for (int i = 0; i <= slot; ++i)
        if (m_doc.features[i].type == CadFeatureType::Plane && m_doc.features[i].enabled) ++ord;
    const bool ok = (ord >= 0 && ord < int(datums.size()));
    SketchPlane frame; if (ok) frame = datums[ord].second;

    if (editing) m_doc.features[m_edit_index] = saved; else m_doc.features.pop_back();

    if (!ok) { m_viewport->clear_datum_gizmo(); update_reference_planes(); return; }

    // Offset arrow anchor = the base/face origin (datum origin walked back along its normal by the
    // offset). Works for both Offset-from-base (no tilt) and Offset-from-face exactly.
    const Vec3d anchor   = frame.origin - frame.normal * f.plane_offset;
    const bool  offset_on = (f.plane_type == PlaneType::Offset);
    m_viewport->set_datum_gizmo(frame, f.plane_u_size, f.plane_v_size,
                                anchor, frame.normal, f.plane_offset, offset_on);
    update_reference_planes();   // base ghosts (origins + datums) follow the tool/model state
}

void DesignPanel::update_helix_gizmo()
{
    if (!m_viewport) return;
    if (m_active != Tool::Helix) { m_viewport->clear_helix_gizmo(); return; }
    m_viewport->set_helix_gizmo(plane_from_choice(m_helix_plane->GetSelection()),
                                m_helix_radius ? m_helix_radius->GetValue() : 0.0,
                                m_helix_pitch  ? m_helix_pitch->GetValue()  : 1.0,
                                m_helix_height ? m_helix_height->GetValue() : 0.0,
                                m_helix_taper  ? m_helix_taper->GetValue()  : 0.0,
                                m_helix_left_handed && m_helix_left_handed->GetValue());
}

void DesignPanel::update_rib_gizmo()
{
    if (!m_viewport) return;
    if (m_active != Tool::Rib) { m_viewport->clear_rib_gizmo(); return; }
    // Same resolution the Tool::Rib branch of update_extrude_gizmo() uses — the depth arrow and
    // this share one sketch and one entity, and must never disagree about which line that is.
    const int ssel = m_rib_sketch ? m_rib_sketch->GetSelection() : wxNOT_FOUND;
    const int ref  = (ssel != wxNOT_FOUND)
                         ? int(reinterpret_cast<intptr_t>(m_rib_sketch->GetClientData(ssel))) : -1;
    if (ref < 0 || ref >= int(m_doc.features.size())) { m_viewport->clear_rib_gizmo(); return; }
    const CadFeature& sk = m_doc.features[ref];
    const int ei = m_rib_entity ? m_rib_entity->GetValue() : 0;
    if (ei < 0 || ei >= int(sk.entities.size())) { m_viewport->clear_rib_gizmo(); return; }
    const SketchEntity& e = sk.entities[ei];
    if (e.type != SketchEntity::Type::Line) { m_viewport->clear_rib_gizmo(); return; }   // rib is line-only
    m_viewport->set_rib_gizmo(sk.plane, e.p0, e.p1,
                              m_rib_thickness ? m_rib_thickness->GetValue() : 0.0);
}

// Onshape default planes: the XY/XZ/YZ reference planes are persistent, transparent, labelled, and
// larger than the bed — shown as the FALLBACK when there is no object yet. When the Plane tool is
// open they additionally surface existing datums so a base can be picked. Single authority for the
// reference-plane overlay (set/clear_base_pick).
void DesignPanel::update_reference_planes()
{
    if (!m_viewport) return;
    // Default planes pass through the modeling origin (bed centre) — same point the kernel uses to
    // resolve XY/XZ/YZ datum bases, so the ghosts, the datums and new sketches all coincide.
    const Vec3d o = m_doc.modeling_origin;
    SketchPlane xy = SketchPlane::XY(); xy.origin += o;
    SketchPlane xz = SketchPlane::XZ(); xz.origin += o;
    SketchPlane yz = SketchPlane::YZ(); yz.origin += o;
    std::vector<SketchPlane> bp = { xy, xz, yz };
    std::vector<int>         bi = { 0, 1, 2 };
    std::vector<std::string> bl = { "XY", "XZ", "YZ" };

    if (m_active == Tool::Plane) {
        // Base picking only makes sense for the Offset method (others reference faces/edges).
        if (m_plane_type && (PlaneType)m_plane_type->GetSelection() == PlaneType::Offset) {
            auto datums = m_doc.resolve_datum_planes();   // already in world coords (origin applied)
            for (int i = 0; i < int(datums.size()); ++i) {
                bp.push_back(datums[i].second); bi.push_back(3 + i); bl.push_back(datums[i].first);
            }
            m_viewport->set_base_pick(std::move(bp), std::move(bi), std::move(bl));
        } else {
            m_viewport->clear_base_pick();
        }
        return;
    }
    // Fallback (Onshape default planes): show the 3 reference planes while there is no SOLID body
    // yet — so they persist through the 2D-sketch phase and reappear after a sketch is confirmed
    // (a sketch creates no body). They no longer block selection: clicking existing geometry wins,
    // a base-plane pick only fires on a click that hit nothing else (see on_mouse fall-through).
    // Available while there is no solid yet OR while the user is actually choosing a sketch
    // plane. The second half fixes a dead end: delete a sketch on a document that still has a
    // body, press Sketch, and act_sketch says "click a face or a reference plane" — with the
    // reference planes already taken away, because a body existed. The instruction was
    // impossible to follow and there was no way to start a sketch at all short of finding a
    // face to click.
    //
    // Not simply always-on: m_dbp_active both RENDERS and picks, so three translucent planes
    // would otherwise float over every finished model. Tying them to Sketch mode shows them
    // exactly when they are the thing being chosen, and hides them again on Finish.
    if (m_doc.bodies.empty() || m_ui_mode == UiMode::Sketch)
        m_viewport->set_base_pick(std::move(bp), std::move(bi), std::move(bl));
    else
        m_viewport->clear_base_pick();
}

TriangleMesh DesignPanel::ghost_from_bodies(const std::vector<TriangleMesh>& per_body) const
{
    TriangleMesh out;
    for (size_t b = 0; b < per_body.size(); ++b) {
        TriangleMesh m = per_body[b];
        if (b < m_body_xform.size()) m.transform(m_body_xform[b]);
        out.merge(m);
    }
    return out;
}

bool DesignPanel::show_mate_ghost(int kind, int cs_a, int cs_b,
                                  double offset, double angle_deg, bool flip, std::string& err)
{
    CadFeature cand;
    cand.type        = CadFeatureType::Mate;
    cand.mate_kind   = kind;
    cand.mate_cs_a   = cs_a;
    cand.mate_cs_b   = cs_b;
    cand.mate_offset = offset;
    cand.mate_angle  = angle_deg;
    cand.mate_flip   = flip;

    TriangleMesh              mesh;
    std::vector<TriangleMesh> pbm;
    if (!m_doc.preview(cand, mesh, pbm, err)) {
        m_viewport->clear_preview();
        m_viewport->set_body_hidden(false);
        return false;
    }
    m_viewport->set_preview_mesh(ghost_from_bodies(pbm));
    // The ghost is the WHOLE assembly in its post-mate pose, not an added lump, so the committed
    // bodies have to go: left visible they would draw the mated body in both places at once and
    // z-fight everything else against its own copy. Same reason Dressup and Draft hide them.
    m_viewport->set_body_hidden(true);
    return true;
}

void DesignPanel::refresh_preview()
{
    if (m_active == Tool::None) { m_viewport->clear_preview(); return; }

    // Transform has no ghost: it moves an existing body rather than building a new one, so its
    // preview IS the body, shown through the display transform the gizmo already drives.
    if (m_active == Tool::Transform) { xf_live_preview(); return; }

    // Features that produce NO solid: a sketch, the three datums, a helix curve, and Project
    // (which emits sketch entities). They have no ghost to build, so they must not go through
    // the solid-preview path below — it finds nothing and reports "invalid: preview produced
    // no geometry", which is what Axis / CoordSys / Helix / Project did on an empty document.
    // route_feature() skips these for the same reason, so the two agree on what is not a solid.
    if (m_active == Tool::Sketch  || m_active == Tool::Plane   || m_active == Tool::Axis ||
        m_active == Tool::CoordSys || m_active == Tool::Helix   || m_active == Tool::Project) {
        m_viewport->clear_preview();
        m_status->SetForegroundColour(wxColour(120, 210, 120));
        wxString ready;
        switch (m_active) {
        case Tool::Plane:    ready = _L("Plane ready");     break;
        case Tool::Axis:     ready = _L("Axis ready");      break;
        case Tool::CoordSys: ready = _L("Coord Sys ready"); break;
        case Tool::Helix:    ready = _L("Helix ready");     break;
        case Tool::Project:  ready = _L("Project ready");   break;
        default:             ready = _L("Sketch ready");    break;
        }
        set_status(ready);
        for (wxButton* b : m_confirm_btns) if (b) b->Enable(true);
        m_status->Refresh();
        update_datum_gizmo();   // Plane card: show/refresh the in-canvas resize handles
        update_helix_gizmo();   // Helix card: draw the live curve + drag handles (no solid ghost)
        return;
    }

    if (m_active == Tool::Mate) {
        // A mate produces no NEW geometry, but it MOVES a body — and the moved assembly is
        // exactly the ghost worth showing. This branch used to clear the preview outright,
        // saying a mate has no 3D ghost; the kernel had no such opinion (preview() routes a
        // Mate candidate through apply_mate on a throwaway copy), so that one line was the
        // whole of gap G3. The card's own gate stays: Confirm is disabled when <2 CoordSys
        // exist or when A == B, because neither of those is a kernel error, only an unfinished
        // form, and the kernel's message for them would be worse than these.
        const bool has_two = m_mate_cs_a && m_mate_cs_a->GetCount() >= 2;
        const int sel_a = has_two ? m_mate_cs_a->GetSelection() : wxNOT_FOUND;
        const int sel_b = has_two ? m_mate_cs_b->GetSelection() : wxNOT_FOUND;
        const bool same = has_two && sel_a != wxNOT_FOUND && sel_b != wxNOT_FOUND && sel_a == sel_b;
        bool ok = has_two && !same;
        if (!has_two) {
            m_viewport->clear_preview();
            m_viewport->set_body_hidden(false);   // the ghost replaced them; give them back
            m_status->SetForegroundColour(wxColour(235, 110, 110));
            set_status(_L("Mate needs at least two CoordSys features"));
        } else if (same) {
            m_viewport->clear_preview();
            m_viewport->set_body_hidden(false);
            m_status->SetForegroundColour(wxColour(235, 110, 110));
            set_status(_L("Mate: CS A and CS B must be different"));
        } else {
            sync_body_xform();   // same reason as the solid path below: drop stale per-body poses
            const int cs_a = int(reinterpret_cast<intptr_t>(m_mate_cs_a->GetClientData(sel_a)));
            const int cs_b = int(reinterpret_cast<intptr_t>(m_mate_cs_b->GetClientData(sel_b)));
            std::string err;
            ok = show_mate_ghost(m_mate_kind ? m_mate_kind->GetSelection() : 0, cs_a, cs_b,
                                 m_mate_offset ? m_mate_offset->GetValue() : 0.0,
                                 m_mate_angle  ? m_mate_angle->GetValue()  : 0.0,
                                 m_mate_flip   && m_mate_flip->GetValue(), err);
            if (ok) {
                m_status->SetForegroundColour(wxColour(120, 210, 120));
                set_status(_L("Mate ready"));
            } else {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                set_status(_L("Invalid: ") + wxString::FromUTF8(err));
            }
        }
        for (wxButton* b : m_confirm_btns) if (b) b->Enable(ok);
        m_status->Refresh();
        return;
    }

    // Trim m_body_xform to the LIVE committed bodies before building the ghost. A move
    // writes a per-body display transform keyed by index; if a moved body is later deleted
    // or consumed, its stale transform must not survive and get re-applied to whatever new
    // body lands at that index — that was painting fresh extrudes as a moved+rotated ghost
    // far from the sketch. resize() drops entries beyond the current body count.
    sync_body_xform();

    CadFeature   cand = build_candidate(m_active);
    TriangleMesh mesh;
    std::string  err;
    bool         ok = false;

    // The body is displayed through its per-body Move transform (m_body_xform); the ghost is
    // built from the untransformed kernel, so without this it floats back at the origin once a
    // body has been moved. Re-merge the per-body ghost meshes with the same transforms applied.
    auto ghost_from = [this](const std::vector<TriangleMesh>& pbm) { return ghost_from_bodies(pbm); };

    const bool editing_single = (m_edit_index >= 0);
    if (editing_single) {
        // Edit-mode preview: stacking the candidate on top of the live body would
        // re-apply the feature being edited (fillet-on-fillet) — wrong, and a
        // source of OCCT failures. Instead evaluate the *replace* on a throwaway
        // copy so the ghost is the true post-edit body.
        CadDocument tmp = m_doc;
        ok = tmp.replace_feature(m_edit_index, cand);
        if (ok) mesh = ghost_from(tmp.display_body_meshes); else err = tmp.error;
    } else {
        std::vector<TriangleMesh> pbm;
        ok = m_doc.preview(cand, mesh, pbm, err);
        if (ok) mesh = ghost_from(pbm);
    }

    if (ok) {
        m_viewport->set_preview_mesh(mesh);
        m_status->SetForegroundColour(wxColour(120, 210, 120)); // ok = green
        set_status(wxString::Format(_L("Preview — %zu triangles"), mesh.its.indices.size()));
    } else {
        m_viewport->clear_preview();
        m_status->SetForegroundColour(wxColour(235, 110, 110)); // invalid = red
        set_status(_L("Invalid: ") + wxString::FromUTF8(err));
    }
    // Onshape parity: a broken candidate cannot be committed. Grey the active dialog's
    // Confirm so the user sees the gate before clicking; the red status says why.
    for (wxButton* b : m_confirm_btns)
        if (b != nullptr) b->Enable(ok);
    // Fillet/Chamfer/Draft: once the target edge/face yields a valid result, show ONLY the
    // preview (hide the base bodies) so the user sees the finished shape, not the old solid
    // doubled with the ghost. Before a valid pick the body stays visible so it can be picked.
    m_viewport->set_body_hidden((m_active == Tool::Dressup || m_active == Tool::Draft) && ok);
    m_status->Refresh();

    // Refresh the in-canvas Extrude depth arrow (self-gates: only while the Extrude card is open).
    update_extrude_gizmo();
    // Same for the Fillet/Chamfer radius arrow (self-gates: Dressup card + a picked edge).
    update_fillet_gizmo();
    // Same for the Hole footprint circle + diameter/depth arrows (self-gates: Hole card).
    update_hole_gizmo();
    // Same for the Thread footprint circle + radius/length arrows (self-gates: Thread card).
    update_thread_gizmo();
    // Same for the Shell thickness arrow on the picked face (self-gates: Shell card + a face).
    update_shell_gizmo();
    // Same for the Revolve angle-arc around the axis (self-gates: only while the Revolve card is open).
    update_revolve_gizmo();
    // Same for the Draft angle-arc around the face centroid (self-gates: only while the Draft card is open).
    update_draft_gizmo();
    // Same for the Cut plane-rectangle + offset arrow (self-gates: only while the Cut card is open).
    update_cut_gizmo();
    // Same for the Pattern spacing arrow / angle-arc (self-gates: only while the Pattern card is open).
    update_pattern_gizmo();
    // Datum-plane resize handles (self-gates: only while the Plane card is open).
    update_datum_gizmo();
    // Helix curve + handles (self-gates: only while the Helix card is open).
    update_helix_gizmo();
    // Rib slab footprint + thickness handles (self-gates: only while the Rib card is open).
    update_rib_gizmo();
    update_operand_highlight();
}

// The tool-card frame is only worth drawing when a card is actually inside it —
// otherwise an empty bordered box floats above the feature tree.
void DesignPanel::update_cards_frame()
{
    if (m_cards == nullptr || m_cards->GetSizer() == nullptr) return;
    wxSizer* root = m_form ? m_form->GetSizer() : nullptr;
    if (root == nullptr) return;
    bool any = false;
    for (const wxSizerItem* it : m_cards->GetSizer()->GetChildren())
        if (it->IsShown()) { any = true; break; }
    root->Show(m_cards, any, false);   // non-recursive: don't re-show the hidden cards inside
}

// Move/Rotate card: numeric entry that composes the same transform the drag gizmo builds —
// translation in world mm plus a rotation about the body's centroid, applied onto the pose the
// body had when Move opened (m_move_prev), so typing and dragging cannot fight each other.
void DesignPanel::apply_move_card()
{
    const int b = m_move_body;
    if (b < 0 || b >= int(m_body_xform.size()) || b >= int(m_doc.display_body_meshes.size())) return;

    const double ang = m_move_angle ? m_move_angle->GetValue() * M_PI / 180.0 : 0.0;
    const int    ax  = m_move_axis ? m_move_axis->GetSelection() : 2;
    const Vec3d  axis = (ax == 0) ? Vec3d::UnitX() : (ax == 1) ? Vec3d::UnitY() : Vec3d::UnitZ();
    const Vec3d  d(m_move_dx ? m_move_dx->GetValue() : 0.0,
                   m_move_dy ? m_move_dy->GetValue() : 0.0,
                   m_move_dz ? m_move_dz->GetValue() : 0.0);

    // Rotate about the body's own centre, not the world origin.
    const Vec3d pivot = m_move_prev * m_doc.display_body_meshes[b].bounding_box().center();
    Transform3d x = Transform3d::Identity();
    x.translate(d + pivot);
    x.rotate(Eigen::AngleAxisd(ang, axis));
    x.translate(-pivot);

    m_body_xform[b] = x * m_move_prev;
    feed_bodies();
    if (m_viewport) m_viewport->request_repaint();
    m_status->SetForegroundColour(wxNullColour);
    set_status(wxString::Format(_L("Body %d — moved (%.1f, %.1f, %.1f) mm, rotated %.1f°"),
                                        b + 1, d.x(), d.y(), d.z(),
                                        m_move_angle ? m_move_angle->GetValue() : 0.0));
    m_status->Refresh();
}

void DesignPanel::show_move_card(bool show)
{
    if (m_box_move == nullptr || m_cards == nullptr || m_cards->GetSizer() == nullptr) return;
    m_cards->GetSizer()->Show(m_box_move, show, true);
    update_cards_frame();
    if (m_form) { m_form->Layout(); m_form->FitInside(); }
}

// Push the tool's current parameters into the live sketch tool before Polygon starts. They
// come from the offer's Polygon submenu (or from the last choice made there), never from a card.
void DesignPanel::push_polygon_params()
{
    if (m_viewport == nullptr) return;
    m_viewport->set_sketch_polygon_sides(m_poly_sides);
    m_viewport->set_sketch_polygon_circumscribed(m_poly_circumscribed);
}

void DesignPanel::open_tool(Tool t)
{
    m_active = t;
    // Fillet/Chamfer/Draft no longer fade the body see-through; instead, once a valid target
    // is picked, refresh_preview hides the base bodies entirely (preview-only). Keep it opaque
    // here so the body is fully visible for picking the edge/face.
    // Body focus follows the CoordSys card: entering any other tool drops it. Read it back
    // from the combo rather than clearing outright — editing a CoordSys feature loads the
    // card (and its body) BEFORE open_tool runs, and a blind clear would strand the viewport
    // showing "Body N" while picking stayed unrestricted.
    if (m_viewport) { m_viewport->set_body_translucent(false); m_viewport->set_body_hidden(false);
                      m_viewport->set_xray_focus(t == Tool::CoordSys && m_cs_body ? m_cs_body->GetSelection() - 1 : -1); }
    wxSizer* s = m_cards->GetSizer();
    s->Show(m_box_sketch,  t == Tool::Sketch,  true);
    s->Show(m_box_extrude, t == Tool::Extrude, true);
    s->Show(m_box_dressup, t == Tool::Dressup, true);
    s->Show(m_box_hole,    t == Tool::Hole,    true);
    s->Show(m_box_thread,  t == Tool::Thread,  true);
    s->Show(m_box_shell,   t == Tool::Shell,   true);
    s->Show(m_box_revolve, t == Tool::Revolve, true);
    s->Show(m_box_sweep,   t == Tool::Sweep,   true);
    s->Show(m_box_pattern, t == Tool::Pattern, true);
    s->Show(m_box_plane,   t == Tool::Plane,   true);
    s->Show(m_box_axis,   t == Tool::Axis,   true);
    s->Show(m_box_coordsys, t == Tool::CoordSys, true);
    s->Show(m_box_loft,    t == Tool::Loft,    true);
    s->Show(m_box_boolean, t == Tool::Boolean, true);
    s->Show(m_box_cut,     t == Tool::Cut,     true);
    s->Show(m_box_draft,   t == Tool::Draft,   true);
    s->Show(m_box_surf_extrude, t == Tool::SurfaceExtrude, true);
    s->Show(m_box_surf_revolve, t == Tool::SurfaceRevolve, true);
    s->Show(m_box_surf_loft,    t == Tool::SurfaceLoft,    true);
    s->Show(m_box_surf_fill,    t == Tool::SurfaceFill,    true);
    s->Show(m_box_surf_offset,  t == Tool::SurfaceOffset,  true);
    s->Show(m_box_surf_thicken, t == Tool::ThickenSurface,  true);
    s->Show(m_box_transform,    t == Tool::Transform,      true);
    s->Show(m_box_mirror,       t == Tool::Mirror,         true);
    s->Show(m_box_thicken,      t == Tool::Thicken,        true);
    s->Show(m_box_rib,          t == Tool::Rib,            true);
    s->Show(m_box_project,      t == Tool::Project,        true);
    s->Show(m_box_delete_face,  t == Tool::DeleteFace,     true);
    s->Show(m_box_helix,        t == Tool::Helix,          true);
    s->Show(m_box_mate,         t == Tool::Mate,           true);
    s->Show(m_box_insert,  t == Tool::Insert,  true);
    s->Show(m_box_expr,   m_edit_index >= 0,  true);   // expression binding available during edit only

    if (t == Tool::Mate) {
        // Populate both CoordSys pickers with every CoordSys feature; store the real
        // feature index in client data. Keep prior selection when re-editing.
        int keep_a = -1, keep_b = -1;
        if (m_mate_cs_a->GetCount() > 0 && m_mate_cs_a->GetSelection() != wxNOT_FOUND) {
            const auto* cl = m_mate_cs_a->GetClientData(m_mate_cs_a->GetSelection());
            if (cl) keep_a = int(reinterpret_cast<intptr_t>(cl));
        }
        if (m_mate_cs_b->GetCount() > 0 && m_mate_cs_b->GetSelection() != wxNOT_FOUND) {
            const auto* cl = m_mate_cs_b->GetClientData(m_mate_cs_b->GetSelection());
            if (cl) keep_b = int(reinterpret_cast<intptr_t>(cl));
        }
        m_mate_cs_a->Clear();
        m_mate_cs_b->Clear();
        for (int i = 0; i < int(m_doc.features.size()); ++i) {
            if (m_doc.features[i].type != CadFeatureType::CoordSys) continue;
            const wxString nm = wxString::FromUTF8(m_doc.features[i].name);
            const int pos_a = combo_append_index(m_mate_cs_a, nm, i);
            const int pos_b = combo_append_index(m_mate_cs_b, nm, i);
            if (i == keep_a) m_mate_cs_a->SetSelection(pos_a);
            if (i == keep_b) m_mate_cs_b->SetSelection(pos_b);
        }
        if (keep_a < 0 && m_mate_cs_a->GetCount() > 0) m_mate_cs_a->SetSelection(0);
        if (keep_b < 0 && m_mate_cs_b->GetCount() > 0) m_mate_cs_b->SetSelection(0);
    }

    if (t == Tool::Revolve && m_revolve_sketch_ref >= 0
        && m_revolve_sketch_ref < int(m_doc.features.size()))
        m_revolve_sketch_label->SetLabel(_L("Sketch: ") +
            wxString::FromUTF8(m_doc.features[m_revolve_sketch_ref].name));

    if (t == Tool::Sweep) {
        if (m_sweep_profile_ref >= 0 && m_sweep_profile_ref < int(m_doc.features.size()))
            m_sweep_profile_label->SetLabel(_L("Profile: ") +
                wxString::FromUTF8(m_doc.features[m_sweep_profile_ref].name));
        // Populate the path picker with every Sketch feature except the profile itself;
        // the feature index rides in the entry's client data. Pre-select the stored path
        // (re-edit), else the first available sketch.
        m_sweep_path->Clear();
        int sel_idx = wxNOT_FOUND;
        for (int i = 0; i < int(m_doc.features.size()); ++i) {
            const CadFeature& sf = m_doc.features[i];
            if (sf.type != CadFeatureType::Sketch || i == m_sweep_profile_ref) continue;
            const int pos = combo_append_index(m_sweep_path, wxString::FromUTF8(sf.name), i);
            if (i == m_sweep_path_ref) sel_idx = pos;
        }
        if (sel_idx != wxNOT_FOUND) m_sweep_path->SetSelection(sel_idx);
        else if (m_sweep_path->GetCount() > 0) m_sweep_path->SetSelection(0);
    }

    if (t == Tool::Loft) {
        // List every Sketch feature; the feature index for each row rides in
        // m_loft_sketch_idx. Re-check the stored profile refs (re-edit).
        m_loft_list->Clear();
        m_loft_sketch_idx.clear();
        for (int i = 0; i < int(m_doc.features.size()); ++i) {
            const CadFeature& sf = m_doc.features[i];
            if (sf.type != CadFeatureType::Sketch) continue;
            const int row = m_loft_list->Append(wxString::FromUTF8(sf.name));
            m_loft_sketch_idx.push_back(i);
            if (std::find(m_loft_refs.begin(), m_loft_refs.end(), i) != m_loft_refs.end())
                m_loft_list->Check(row, true);
        }
    }

    if (t == Tool::SurfaceExtrude) {
        if (m_surf_extrude_sketch_ref >= 0 && m_surf_extrude_sketch_ref < int(m_doc.features.size()))
            m_surf_extrude_sketch_label->SetLabel(_L("Sketch: ") +
                wxString::FromUTF8(m_doc.features[m_surf_extrude_sketch_ref].name));
    }

    if (t == Tool::SurfaceRevolve) {
        if (m_surf_revolve_sketch_ref >= 0 && m_surf_revolve_sketch_ref < int(m_doc.features.size()))
            m_surf_revolve_sketch_label->SetLabel(_L("Sketch: ") +
                wxString::FromUTF8(m_doc.features[m_surf_revolve_sketch_ref].name));
    }

    if (t == Tool::SurfaceLoft) {
        m_surf_loft_list->Clear();
        m_surf_loft_sketch_idx.clear();
        for (int i = 0; i < int(m_doc.features.size()); ++i) {
            const CadFeature& sf = m_doc.features[i];
            if (sf.type != CadFeatureType::Sketch) continue;
            const int row = m_surf_loft_list->Append(wxString::FromUTF8(sf.name));
            m_surf_loft_sketch_idx.push_back(i);
            if (std::find(m_surf_loft_refs.begin(), m_surf_loft_refs.end(), i) != m_surf_loft_refs.end())
                m_surf_loft_list->Check(row, true);
        }
    }

    if (t == Tool::SurfaceFill) {
        if (m_surf_fill_sketch_ref >= 0 && m_surf_fill_sketch_ref < int(m_doc.features.size()))
            m_surf_fill_sketch_label->SetLabel(_L("Sketch: ") +
                wxString::FromUTF8(m_doc.features[m_surf_fill_sketch_ref].name));
    }

    // Shell and Draft both take their face from the live pick at Confirm time, but until now
    // only the PICK handler wrote their labels. So the natural order — pick the face, then open
    // the card — left Shell reading "(all faces — closed hollow)" and Draft "(pick a side face)"
    // while Confirm went on to use m_sel_solid_face regardless: the card described one operation
    // and performed another. Initialise from the current selection here, exactly as Extrude does
    // with m_extrude_face_src below. Skipped while re-editing, because load_feature_into_dialog
    // has already written the label from the feature's own stored face and runs before this.
    // Same reasoning for Dress-up, whose target is the picked EDGE (falling back to the group).
    if (t == Tool::Dressup && m_edit_index < 0)
        sync_dressup_target();

    if (t == Tool::Shell && m_edit_index < 0)
        m_shell_face_label->SetLabel(m_sel_solid_face >= 0
            ? wxString::Format(_L("Face %d"), m_sel_solid_face)
            : _L("(all faces — closed hollow)"));

    if (t == Tool::Draft && m_edit_index < 0)
        m_draft_face_label->SetLabel(m_sel_solid_face >= 0
            ? wxString::Format(_L("Face %d"), m_sel_solid_face)
            : _L("(pick a side face)"));

    if (t == Tool::Extrude) {
        if (m_extrude_face_src >= 0)
            m_extrude_sketch_label->SetLabel(
                wxString::Format(_L("Face %d (push/pull)"), m_extrude_face_src));
        else if (m_extrude_sketch_ref >= 0 && m_extrude_sketch_ref < int(m_doc.features.size()))
            m_extrude_sketch_label->SetLabel(_L("Sketch: ") +
                wxString::FromUTF8(m_doc.features[m_extrude_sketch_ref].name));
        // A fresh extrude defaults to New body — even when other bodies exist — so
        // overlapping extrudes stay SEPARATE solids instead of silently fusing. Joining
        // is opt-in (pick "Join"). Engraving art onto a face still defaults to Cut.
        // (Edit-mode keeps the feature's stored mode, set below.)
        if (m_edit_index < 0) {
            const bool on_face_import =
                m_extrude_sketch_ref >= 0 && m_extrude_sketch_ref < int(m_doc.features.size())
                && m_doc.features[m_extrude_sketch_ref].import_on_face;
            if (on_face_import) {
                m_mode->SetSelection(2);     // Cut — engrave into the face
                m_flip->SetValue(true);      // extrude inward (the face normal points out)
            } else {
                m_mode->SetSelection(0);     // New body (was: Add when a body already existed)
            }
        }
    }

    // Retitle the active card's header: edit-mode shows the feature's real name,
    // add-mode previews the type + next feature number (Onshape "Extrude 1").
    const bool editing = (m_edit_index >= 0 && m_edit_index < int(m_doc.features.size()));
    auto title = [&](const wxString& base) -> wxString {
        return editing ? wxString::FromUTF8(m_doc.features[m_edit_index].name)
                       : base + wxString::Format(" %d", m_feature_counter + 1);
    };
    switch (t) {
    case Tool::Sketch:  m_hdr_sketch->SetLabel(title(_L("Sketch")));   break;
    case Tool::Extrude: m_hdr_extrude->SetLabel(title(_L("Extrude"))); break;
    case Tool::Dressup: m_hdr_dressup->SetLabel(title(
                            m_dressup_type->GetSelection() == 0 ? _L("Fillet") : _L("Chamfer"))); break;
    case Tool::Hole:    m_hdr_hole->SetLabel(title(_L("Hole")));       break;
    case Tool::Thread:  m_hdr_thread->SetLabel(title(_L("Thread")));   break;
    case Tool::Shell:   m_hdr_shell->SetLabel(title(_L("Shell")));     break;
    case Tool::Revolve: m_hdr_revolve->SetLabel(title(_L("Revolve"))); break;
    case Tool::Sweep:   m_hdr_sweep->SetLabel(title(_L("Sweep")));     break;
    case Tool::Pattern: m_hdr_pattern->SetLabel(title(_L("Pattern"))); break;
    case Tool::Plane:   m_hdr_plane->SetLabel(title(_L("Plane")));     break;
    case Tool::Loft:    m_hdr_loft->SetLabel(title(_L("Loft")));       break;
    case Tool::Draft:   m_hdr_draft->SetLabel(title(_L("Draft")));     break;
    case Tool::Boolean: m_hdr_boolean->SetLabel(title(_L("Boolean"))); break;
    case Tool::Cut:     m_hdr_cut->SetLabel(title(_L("Cut")));         break;
    case Tool::Axis:    m_hdr_axis->SetLabel(title(_L("Axis")));       break;
    case Tool::CoordSys: m_hdr_coordsys->SetLabel(title(_L("Coord Sys"))); break;
    case Tool::SurfaceExtrude:  m_hdr_surf_extrude->SetLabel(title(_L("Surface Extrude")));  break;
    case Tool::SurfaceRevolve:  m_hdr_surf_revolve->SetLabel(title(_L("Surface Revolve")));  break;
    case Tool::SurfaceLoft:     m_hdr_surf_loft->SetLabel(title(_L("Surface Loft")));      break;
    case Tool::SurfaceFill:     m_hdr_surf_fill->SetLabel(title(_L("Surface Fill")));      break;
    case Tool::SurfaceOffset:   m_hdr_surf_offset->SetLabel(title(_L("Surface Offset")));   break;
    case Tool::ThickenSurface:  m_hdr_surf_thicken->SetLabel(title(_L("Thicken Surface")));   break;
    case Tool::Transform:   m_hdr_transform->SetLabel(title(_L("Transform")));   break;
    case Tool::Mirror:      m_hdr_mirror->SetLabel(title(_L("Mirror")));         break;
    case Tool::Thicken:     m_hdr_thicken->SetLabel(title(_L("Thicken")));       break;
    case Tool::Rib:         m_hdr_rib->SetLabel(title(_L("Rib")));               break;
    case Tool::Project:     m_hdr_project->SetLabel(title(_L("Project")));       break;
    case Tool::DeleteFace:  m_hdr_delete_face->SetLabel(title(_L("Delete Face"))); break;
    case Tool::Helix:       m_hdr_helix->SetLabel(title(_L("Helix")));           break;
    case Tool::Mate:        m_hdr_mate->SetLabel(title(_L("Mate")));             break;
    case Tool::Insert:  break;   // header set by open_insert_card()
    case Tool::None:    break;
    }

    // A card that consumes a face must say WHICH face the moment it appears. These labels used to
    // be written only by the pick handler, which runs while a card is already open — so a card
    // opened FROM a selection (the offer's whole premise) showed the previous pick, or the "(pick
    // one)" placeholder over a face it was in fact about to use. Same law as the Construction
    // toggle: a control that carries state has to show it. One writer, on open, for all three.
    {
        const bool have = m_sel_solid_face >= 0;
        const wxString face_name = have ? wxString::Format(_L("Face %d"), m_sel_solid_face) : wxString();
        if (t == Tool::Thicken && m_thicken_face_label)
            m_thicken_face_label->SetLabel(have ? face_name : _L("(pick a solid face)"));
        if (t == Tool::Shell && m_shell_face_label)
            m_shell_face_label->SetLabel(have ? face_name : _L("(all faces — closed hollow)"));
        if (t == Tool::Draft && m_draft_face_label)
            m_draft_face_label->SetLabel(have ? face_name : _L("(pick a side face)"));
    }

    if (editing) {
        populate_expr_fields(t);   // field-name combo for this feature type
        // Display current expression bindings on the edited feature
        const CadFeature& ef = m_doc.features[m_edit_index];
        if (ef.expr.empty()) {
            m_expr_status->SetLabel(_L("(no bindings)"));
            m_expr_status->SetForegroundColour(dp_sec_text());
        } else {
            wxString s;
            for (const auto& [field, e] : ef.expr) {
                if (!s.IsEmpty()) s += "; ";
                s += wxString::FromUTF8(field) + " = " + wxString::FromUTF8(e);
            }
            m_expr_status->SetLabel(s);
            m_expr_status->SetForegroundColour(dp_ctl_text());
        }
    }

    update_cards_frame(); m_form->Layout();
    m_form->FitInside();
    update_action_bar();   // a tool is now active -> show the unified ✓/✗
    refresh_preview();

    // Add-mode Transform arms the move gizmo on the card's body so the prominent verb is the
    // geometry-first control; the card mirrors the drag. Edit mode keeps today's card-only path.
    if (t == Tool::Transform && m_edit_index < 0)
        arm_transform_gizmo();
    // Opening Boolean re-aims the viewport picks at the target slot, so the first click after
    // the card appears always means "keep this one" regardless of what the last session did.
    if (t == Tool::Boolean)
        m_bool_next_slot = 0;
}

void DesignPanel::close_tool()
{
    m_active = Tool::None;
    set_active_tool_btn(nullptr);   // clear the active-tool teal highlight
    // Single revert point for the Transform gizmo: Esc, switching tools, and Cancel all pass
    // through here, so the body is never left displaced by a Transform that wasn't committed.
    xf_clear_preview();
    if (m_xf_gizmo_body >= 0) {
        sync_body_xform();
        if (m_xf_gizmo_body < int(m_body_xform.size()))
            m_body_xform[m_xf_gizmo_body] = m_xf_gizmo_base;
        if (m_viewport) m_viewport->clear_move_gizmo();
        feed_bodies();
        m_xf_gizmo_body = -1;
        m_move_body     = -1;
    }
    if (m_viewport) { m_viewport->set_body_translucent(false); m_viewport->set_body_hidden(false); m_viewport->set_xray_focus(-1); }   // restore the opaque solid
    wxSizer* s = m_cards->GetSizer();
    s->Show(m_box_sketch,  false, true);
    s->Show(m_box_extrude, false, true);
    s->Show(m_box_dressup, false, true);
    s->Show(m_box_hole,    false, true);
    s->Show(m_box_thread,  false, true);
    s->Show(m_box_shell,   false, true);
    s->Show(m_box_revolve, false, true);
    s->Show(m_box_sweep,   false, true);
    s->Show(m_box_pattern, false, true);
    s->Show(m_box_plane,   false, true);
    s->Show(m_box_axis,   false, true);
    s->Show(m_box_coordsys, false, true);
    s->Show(m_box_loft,    false, true);
    s->Show(m_box_boolean, false, true);
    s->Show(m_box_cut,     false, true);
    s->Show(m_box_draft,   false, true);
    s->Show(m_box_surf_extrude, false, true);
    s->Show(m_box_surf_revolve, false, true);
    s->Show(m_box_surf_loft,    false, true);
    s->Show(m_box_surf_fill,    false, true);
    s->Show(m_box_surf_offset,  false, true);
    s->Show(m_box_surf_thicken, false, true);
    s->Show(m_box_transform,    false, true);
    s->Show(m_box_mirror,       false, true);
    s->Show(m_box_thicken,      false, true);
    s->Show(m_box_rib,          false, true);
    s->Show(m_box_project,      false, true);
    s->Show(m_box_delete_face,  false, true);
    s->Show(m_box_helix,        false, true);
    s->Show(m_box_mate,         false, true);
    s->Show(m_box_insert,  false, true);
    s->Show(m_box_expr,    false, true);
    m_viewport->clear_preview();
    m_viewport->clear_extrude_gizmo();
    m_viewport->clear_fillet_gizmo();
    m_viewport->clear_hole_gizmo();
    m_viewport->clear_thread_gizmo();
    m_viewport->clear_shell_gizmo();
    m_viewport->clear_revolve_gizmo();
    m_viewport->clear_draft_gizmo();
    m_viewport->clear_cut_gizmo();
    m_viewport->clear_pattern_gizmo();
    m_viewport->clear_datum_gizmo();
    m_viewport->set_operand_bodies(-1, -1);
    m_viewport->set_highlight_sketches({});
    update_reference_planes();   // back to no-tool: show the origin planes if there is no object yet
    update_cards_frame(); m_form->Layout();
    m_form->FitInside();
    update_action_bar();   // no feature tool active -> hide the bar (unless a mode keeps it)
}

void DesignPanel::confirm_tool()
{
    // One undo boundary per committed feature (Extrude/Dressup/Hole/Thread/Shell, the
    // legacy Sketch card via on_add_sketch, and edit-mode replace all funnel here).
    m_doc.checkpoint();
    const bool editing_single = (m_edit_index >= 0);

    if (editing_single) {
        // Edit mode: overwrite the existing feature instead of appending.
        CadFeature cand = build_candidate(m_active);
        bool ok = m_doc.replace_feature(m_edit_index, cand);
        reset_edit_state();
        close_tool();          // clears the preview ghost
        after_tree_edit(ok);   // refresh tree/viewport/status (or "Edit rejected")
        return;
    }

    switch (m_active) {
    case Tool::Sketch:  on_add_sketch();  break;
    case Tool::Extrude: on_add_extrude(); break;
    case Tool::Dressup: on_add_dressup(); break;
    case Tool::Hole:    on_add_hole();    break;
    case Tool::Thread:  on_add_thread();  break;
    case Tool::Shell:   on_add_shell();   break;
    case Tool::Revolve: on_add_revolve(); break;
    case Tool::Sweep:   on_add_sweep();   break;
    case Tool::Pattern: on_add_pattern(); break;
    case Tool::Plane:   if (!on_add_plane()) return;   break;   // refused: keep the card and its picks
    case Tool::Loft:    on_add_loft();    break;
    case Tool::Draft:   on_add_draft();   break;
    case Tool::Boolean: on_add_boolean(); break;
    case Tool::Cut:     on_add_cut();     break;
    case Tool::Axis:    on_add_axis();    break;
    case Tool::CoordSys: on_add_coordsys(); break;
    case Tool::Mate:   on_add_mate();   break;
    case Tool::SurfaceExtrude:  on_add_surface_extrude();  break;
    case Tool::SurfaceRevolve:  on_add_surface_revolve();  break;
    case Tool::SurfaceLoft:     on_add_surface_loft();     break;
    case Tool::SurfaceFill:     on_add_surface_fill();     break;
    case Tool::SurfaceOffset:   on_add_surface_offset();   break;
    case Tool::ThickenSurface:  on_add_thicken_surface();  break;
    case Tool::Transform:    on_add_transform();    break;
    case Tool::Mirror:       on_add_mirror();       break;
    case Tool::Thicken:      on_add_thicken();      break;
    case Tool::Rib:          on_add_rib();          break;
    case Tool::Project:      on_add_project();      break;
    case Tool::DeleteFace:   on_add_delete_face();  break;
    case Tool::Helix:        on_add_helix();        break;
    case Tool::Insert:  return;   // committed via finalize_insert(), never here
    case Tool::None:    return;
    }
    close_tool(); // also clears the preview ghost; the committed body is now shown
}

void DesignPanel::cancel_tool()
{
    reset_edit_state(); // abort an in-progress edit: back to add-mode
    close_tool();
    // Cancel discards the candidate: clear the stale "Preview …"/"Invalid …"
    // label and restore the neutral idle colour (Confirm keeps its "OK" status).
    m_status->SetForegroundColour(wxNullColour);
    set_status(wxString());
    m_status->Refresh();
}

// One Confirm surface for the whole tab. Routes to the right commit by current context:
// a feature card, the Insert placement, the Sketch session, or the Constrain session.
void DesignPanel::tool_confirm()
{
    if (m_value_cont) { confirm_value(); return; }   // value card owns ribbon ✓ while a value is pending
    if (m_active == Tool::None && m_viewport && m_viewport->moving_body()) {   // keep the placement, drop the gizmo
        m_viewport->clear_move_gizmo();
        m_move_body = -1;
        show_move_card(false);
        update_action_bar();
        set_status_ok();
        return;
    }
    if (m_active == Tool::Insert) { finalize_insert(); return; }
    if (m_active != Tool::None)   { confirm_tool();   return; }
    if (m_ui_mode == UiMode::Sketch) {
        if (m_viewport && m_viewport->is_sketching()) m_viewport->finish_sketch();
        set_ui_mode(UiMode::Feature);
        return;
    }
    if (m_ui_mode == UiMode::Constrain) {
        cancel_value();
        if (m_viewport) m_viewport->end_constrain();
        m_constrain_feat = -1;
        set_ui_mode(UiMode::Feature);
        m_status->SetForegroundColour(wxNullColour);
        set_status(wxString());
        m_status->Refresh();
    }
}

// One Cancel/exit surface (also bound to Esc). Discards the active feature/insert, or a
// drawn-but-uncommitted Sketch, or exits Constrain.
void DesignPanel::tool_cancel()
{
    if (m_value_cont) { cancel_value(); return; }     // value card owns ribbon ✗ while a value is pending
    if (m_active == Tool::None && m_viewport && m_viewport->moving_body()) {   // revert to the pose at move-start
        sync_body_xform();
        if (m_move_body >= 0 && m_move_body < int(m_body_xform.size()))
            m_body_xform[m_move_body] = m_move_prev;
        m_viewport->clear_move_gizmo();
        m_move_body = -1;
        show_move_card(false);
        feed_bodies();           // re-render the reverted placement
        update_action_bar();
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("Move cancelled"));
        m_status->Refresh();
        return;
    }
    if (m_active == Tool::Insert) { cancel_insert(); return; }
    if (m_active != Tool::None)   { cancel_tool();   return; }
    if (m_ui_mode == UiMode::Sketch) {
        // Escape is how anyone dismisses the inline dimension field, and it used to cascade
        // straight through to here: first press disarmed the tool, second press dropped the
        // whole live session — a drawn rectangle gone, silently, with no undo prompt. That is
        // the "I cannot add the circle after the rectangle" report: the sketch was already
        // destroyed. Discarding real work needs the explicit Cancel button, not a key people
        // press to close a text field.
        if (m_viewport && m_viewport->live_sketch_has_work()) {
            m_status->SetForegroundColour(wxNullColour);
            set_status(_L("Sketch kept — use Confirm to keep it, Cancel to discard"));
            m_status->Refresh();
            return;
        }
        if (m_viewport) m_viewport->cancel_sketch();   // drop the live session (committed art stays)
        m_edit_index = -1;
        set_ui_mode(UiMode::Feature);
        sync_sketch_display();
        refresh_tree();
        m_status->SetForegroundColour(wxNullColour);
        set_status(wxString());
        m_status->Refresh();
        return;
    }
    if (m_ui_mode == UiMode::Constrain) {
        cancel_value();
        if (m_viewport) m_viewport->end_constrain();
        m_constrain_feat = -1;
        set_ui_mode(UiMode::Feature);
        m_status->SetForegroundColour(wxNullColour);
        set_status(wxString());
        m_status->Refresh();
    }
}

void DesignPanel::update_undo_redo_buttons()
{
    // Grey Undo/Redo to mirror exactly what do_undo_redo will do: it acts only in Feature
    // mode with no tool/dialog open (otherwise Esc is the way out), so reflect that gate here
    // as well as the document's available history.
    if (m_btn_undo == nullptr || m_btn_redo == nullptr) return;
    const bool gated = (m_ui_mode != UiMode::Feature) || (m_active != Tool::None);
    m_btn_undo->Enable(!gated && m_doc.can_undo());
    m_btn_redo->Enable(!gated && m_doc.can_redo());
}

void DesignPanel::update_action_bar()
{
    update_undo_redo_buttons();   // mode/tool changes flip the do_undo_redo gate -> refresh greying
    if (m_tb_action == nullptr || m_toolbar == nullptr) return;
    wxSizer* s = m_toolbar->GetSizer();
    if (s == nullptr) return;
    const bool active = (m_active != Tool::None)
                     || m_ui_mode == UiMode::Sketch
                     || m_ui_mode == UiMode::Constrain
                     || (m_viewport && m_viewport->moving_body());
    s->Show(m_tb_action, active, true);
    // HIDING THE BAR ORPHANS THE KEYBOARD, and that is the "app does not consent to sketch"
    // report. The ✓/✗ live in this bar, so the click that confirms a feature leaves focus on a
    // button that this very call then hides. wx does not hand that focus anywhere useful, and
    // wxEVT_CHAR_HOOK is bound on THIS PANEL — it is delivered to the focused window and
    // propagates up the parent chain, so with focus outside the panel every Design shortcut
    // stops arriving. Measured on the rig: after Confirm, shift+S produced ZERO CHAR_HOOK lines
    // (not even the modifier), while X kept focus on the main window throughout — so it was
    // never a window-manager problem. One bare canvas click restored it, which is exactly the
    // workaround users found and reported as "shift+s works but it is not intuitive".
    // The canvas is the right owner of the keyboard in this tab, so give it back explicitly.
    if (!active && m_viewport != nullptr) {
        wxWindow* f = wxWindow::FindFocus();
        bool in_bar = (f == nullptr);
        for (wxWindow* w = f; w != nullptr && !in_bar; w = w->GetParent())
            if (w == m_toolbar) in_bar = true;   // the bar is a sizer; its buttons parent to m_toolbar
        if (in_bar) m_viewport->SetFocus();
    }
    m_toolbar->Layout();
    m_toolbar->FitInside();   // refresh scroll range when the action bar shows/hides
}

void DesignPanel::do_undo_redo(bool redo)
{
    // v1: act only in Feature mode. While authoring/constraining a sketch (m_ui_mode) or
    // with a feature dialog open (m_active), Esc/Cancel is the way out — popping committed
    // history mid-tool would be ambiguous (and could orphan the tool's referenced feature).
    if (m_ui_mode != UiMode::Feature || m_active != Tool::None) {
        m_status->SetForegroundColour(wxNullColour);
        set_status(_L("Finish or cancel the current tool first (Esc)"));
        m_status->Refresh();
        return;
    }
    const bool ok = redo ? m_doc.redo() : m_doc.undo();
    if (!ok) {
        m_status->SetForegroundColour(wxNullColour);
        set_status(redo ? _L("Nothing to redo") : _L("Nothing to undo"));
        m_status->Refresh();
        return;
    }
    // The solid whole/face/edge pick and any in-place edit reference ids that recompute()
    // invalidates — drop them before refreshing from the restored document.
    m_sel_solid_body = m_sel_solid_face = m_sel_solid_edge = -1;
    m_pick_face = m_pick_face_body = -1;   // recompute() invalidated the face ids too
    reset_edit_state();
    after_tree_edit(true);   // refresh tree + viewport meshes + status from the restored doc
    m_status->SetForegroundColour(wxNullColour);
    set_status(wxString::Format(redo ? _L("Redo  (%zu more)") : _L("Undo  (%zu more)"),
                                        redo ? m_doc.redo_depth() : m_doc.undo_depth()));
    m_status->Refresh();
}

// --- Document variables panel ---------------------------------------------------------

void DesignPanel::refresh_variables()
{
    if (!m_var_list) return;
    m_var_list->DeleteAllItems();
    int row = 0;
    for (const auto& [name, expr] : m_doc.variables) {
        m_var_list->InsertItem(row, wxString::FromUTF8(name));
        m_var_list->SetItem(row, 1, wxString::FromUTF8(expr));
        ++row;
    }
}

void DesignPanel::on_add_variable()
{
    wxString name = ::wxGetTextFromUser(_L("Variable name:"), _L("Add Variable"), "", this);
    if (name.IsEmpty()) return;
    name.Trim(true).Trim(false);
    if (name.Contains(' ')) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("Variable name must not contain spaces"));
        m_status->Refresh();
        return;
    }
    wxString expr = ::wxGetTextFromUser(
        wxString::Format(_L("Expression for '%s':"), name),
        _L("Add Variable"), "0", this);
    if (expr.IsEmpty()) return;

    const std::string name_str = name.ToUTF8().data();
    const std::string expr_str = expr.ToUTF8().data();

    m_doc.checkpoint();
    m_doc.variables[name_str] = expr_str;
    bool ok = m_doc.recompute();
    // undo() recomputes, which SUCCEEDS and clears doc.error — so the reason the edit was
    // rejected is gone before anything can display it. Carry it across the rollback.
    if (!ok) { const std::string why = m_doc.error; m_doc.undo(); m_doc.error = why; }
    after_tree_edit(ok);
    refresh_variables();
}

void DesignPanel::on_edit_variable()
{
    if (!m_var_list) return;
    const long sel = m_var_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) {
        set_status(_L("Select a variable first"));
        m_status->Refresh();
        return;
    }
    const std::string name_str = m_var_list->GetItemText(sel, 0).ToUTF8().data();
    const std::string old_expr  = m_var_list->GetItemText(sel, 1).ToUTF8().data();
    wxString expr = ::wxGetTextFromUser(
        wxString::Format(_L("Expression for '%s':"), m_var_list->GetItemText(sel, 0)),
        _L("Edit Variable"), wxString::FromUTF8(old_expr), this);
    if (expr.IsEmpty()) return;

    const std::string expr_str = expr.ToUTF8().data();
    m_doc.checkpoint();
    m_doc.variables[name_str] = expr_str;
    bool ok = m_doc.recompute();
    // undo() recomputes, which SUCCEEDS and clears doc.error — so the reason the edit was
    // rejected is gone before anything can display it. Carry it across the rollback.
    if (!ok) { const std::string why = m_doc.error; m_doc.undo(); m_doc.error = why; }
    after_tree_edit(ok);
    refresh_variables();
}

void DesignPanel::on_remove_variable()
{
    if (!m_var_list) return;
    const long sel = m_var_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) {
        set_status(_L("Select a variable first"));
        m_status->Refresh();
        return;
    }
    const std::string name_str = m_var_list->GetItemText(sel, 0).ToUTF8().data();

    m_doc.checkpoint();
    m_doc.variables.erase(name_str);
    bool ok = m_doc.recompute();
    if (!ok) {
        // Capture before undo(): its recompute succeeds and clears doc.error.
        const std::string why = m_doc.error;
        m_doc.undo();
        // Almost always a feature expr still referencing the variable — but say so as the
        // likely cause and keep the real message, rather than asserting a diagnosis that
        // would be wrong for any other failure.
        m_doc.error = wxString::Format(
            _L("Variable '%s' could not be removed (likely still referenced by a feature "
               "expression): %s"),
            m_var_list->GetItemText(sel, 0), wxString::FromUTF8(why)).ToUTF8().data();
    }
    after_tree_edit(ok);
    refresh_variables();
}

// --- Expression binding ---------------------------------------------------------------

// Field-name lists per feature type. The combo is editable so a power user can type any
// valid field name; these lists pre-populate the picker for the common operations.
std::vector<std::string> DesignPanel::fields_for_tool(Tool t)
{
    using T = DesignPanel::Tool;
    switch (t) {
    case T::Sketch:           return {"width", "height", "radius"};
    case T::Extrude:          return {"distance", "distance2", "taper_deg"};
    case T::Dressup:          return {"dressup_size"};
    case T::Hole:             return {"hole_diameter", "hole_depth", "hole_x", "hole_y"};
    case T::Thread:           return {"thread_radius", "thread_pitch", "thread_height", "thread_depth", "thread_x", "thread_y"};
    case T::Shell:            return {"shell_thickness"};
    case T::Revolve:          return {"revolve_angle"};
    case T::Sweep:            return {};
    case T::Pattern:          return {"pattern_count", "pattern_spacing", "pattern_angle"};
    case T::Plane:            return {"plane_offset", "plane_angle_tilt"};
    case T::Loft:             return {};
    case T::Draft:            return {"draft_angle"};
    case T::Boolean:          return {};
    case T::Cut:              return {};
    case T::SurfaceExtrude:   return {"distance"};
    case T::SurfaceRevolve:   return {"revolve_angle"};
    case T::SurfaceLoft:      return {};
    case T::SurfaceFill:      return {};
    case T::SurfaceOffset:    return {"plane_offset"};
    case T::ThickenSurface:   return {"thicken_thickness"};
    case T::Transform:        return {};
    case T::Mirror:           return {};
    case T::Thicken:          return {"thicken_thickness"};
    case T::Rib:              return {"rib_thickness", "rib_depth"};
    case T::Project:          return {};
    case T::DeleteFace:       return {};
    case T::Helix:            return {"helix_radius", "helix_pitch", "helix_height", "helix_taper_deg"};
    case T::Axis:             return {};
    case T::CoordSys:         return {};
    case T::Mate:             return {};
    case T::Insert:           return {};
    case T::None:             return {};
    }
    return {};
}

void DesignPanel::populate_expr_fields(Tool t)
{
    if (!m_expr_field) return;
    m_expr_field->Clear();
    for (const std::string& f : fields_for_tool(t))
        m_expr_field->Append(wxString::FromUTF8(f));
    if (m_expr_field->GetCount() > 0)
        m_expr_field->SetSelection(0);
}

void DesignPanel::on_set_expr()
{
    if (m_edit_index < 0 || m_edit_index >= int(m_doc.features.size())) return;
    if (!m_expr_field || !m_expr_text) return;

    const wxString fwx = m_expr_field->GetValue();
    const wxString ewx = m_expr_text->GetValue();
    if (fwx.IsEmpty()) return;

    const std::string field = fwx.ToUTF8().data();
    const std::string expr  = ewx.ToUTF8().data();

    m_doc.checkpoint();
    m_doc.features[m_edit_index].expr[field] = expr;
    bool ok = m_doc.recompute();
    // undo() recomputes, which SUCCEEDS and clears doc.error — so the reason the edit was
    // rejected is gone before anything can display it. Carry it across the rollback.
    if (!ok) { const std::string why = m_doc.error; m_doc.undo(); m_doc.error = why; }
    after_tree_edit(ok);
    if (ok) m_expr_text->Clear();

    // Refresh the status line showing current bindings
    if (m_edit_index >= 0 && m_edit_index < int(m_doc.features.size())) {
        const CadFeature& ef = m_doc.features[m_edit_index];
        if (ef.expr.empty()) {
            m_expr_status->SetLabel(_L("(no bindings)"));
            m_expr_status->SetForegroundColour(dp_sec_text());
        } else {
            wxString s;
            for (const auto& [f, e] : ef.expr) {
                if (!s.IsEmpty()) s += "; ";
                s += wxString::FromUTF8(f) + " = " + wxString::FromUTF8(e);
            }
            m_expr_status->SetLabel(s);
            m_expr_status->SetForegroundColour(dp_ctl_text());
        }
    }
}

void DesignPanel::on_clear_expr()
{
    if (m_edit_index < 0 || m_edit_index >= int(m_doc.features.size())) return;
    if (!m_expr_field) return;

    const wxString fwx = m_expr_field->GetValue();
    if (fwx.IsEmpty()) return;

    const std::string field = fwx.ToUTF8().data();
    auto& feat_expr = m_doc.features[m_edit_index].expr;
    if (feat_expr.find(field) == feat_expr.end()) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        set_status(_L("No binding for that field"));
        m_status->Refresh();
        return;
    }

    m_doc.checkpoint();
    feat_expr.erase(field);
    bool ok = m_doc.recompute();
    // undo() recomputes, which SUCCEEDS and clears doc.error — so the reason the edit was
    // rejected is gone before anything can display it. Carry it across the rollback.
    if (!ok) { const std::string why = m_doc.error; m_doc.undo(); m_doc.error = why; }
    after_tree_edit(ok);

    if (m_edit_index >= 0 && m_edit_index < int(m_doc.features.size())) {
        const CadFeature& ef = m_doc.features[m_edit_index];
        if (ef.expr.empty()) {
            m_expr_status->SetLabel(_L("(no bindings)"));
            m_expr_status->SetForegroundColour(dp_sec_text());
        } else {
            wxString s;
            for (const auto& [f, e] : ef.expr) {
                if (!s.IsEmpty()) s += "; ";
                s += wxString::FromUTF8(f) + " = " + wxString::FromUTF8(e);
            }
            m_expr_status->SetLabel(s);
            m_expr_status->SetForegroundColour(dp_ctl_text());
        }
    }
}

}} // namespace Slic3r::GUI

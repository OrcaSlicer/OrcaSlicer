#include "DesignPanel.hpp"
#include "DesignCanvas.hpp"
#include "DesignSketchTool.hpp"
#include "libslic3r/GeometryEngine.hpp"   // face_by_index for face-extrude gizmo anchor

#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/spinctrl.h>
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

#include <string>
#include <memory>
#include <functional>
#include <cmath>
#include <cstdio>
#include <algorithm>

#include "slic3r/GUI/wxExtensions.hpp"   // ScalableButton, create_scaled_bitmap
#include "Widgets/Label.hpp"             // HarmonyOS Sans fonts (Head_*/Body_*) shared with the rest of Orca
#include "Widgets/DropDown.hpp"          // Orca-themed combo dropdown (white/teal selector) for the tool flyouts
#include "libslic3r/SketchImport.hpp"    // text_to_regions / svg_to_regions
#include "libslic3r/ThreadStandards.hpp" // ISO metric / Unified imperial thread tables
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"

// Apple-esque consistency: the Design tab speaks ONE uniform vocabulary — English — so it
// never reads as a half-Italian/half-English patchwork against the host's localized chrome.
// Bypass the gettext catalog for every _L() in this TU (the surrounding Orca UI still follows
// the app locale; only our tool/card/toolbar strings are pinned). One lever, whole file.
#ifdef _L
#undef _L
#endif
#define _L(s) wxString::FromUTF8(s)

namespace Slic3r { namespace GUI {

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

static wxSpinCtrlDouble* make_spin(wxWindow* parent, double val,
                                   double mn = 0.1, double mx = 1000.0)
{
    auto* s = new wxSpinCtrlDouble(parent, wxID_ANY, "", wxDefaultPosition, wxSize(90, -1));
    s->SetRange(mn, mx);
    s->SetDigits(2);
    s->SetValue(val);
    return s;
}

static SketchPlane plane_from_index(int i)
{
    switch (i) {
        case 1:  return SketchPlane::XZ();
        case 2:  return SketchPlane::YZ();
        default: return SketchPlane::XY();
    }
}

// Inverse of plane_from_index: recover the wxChoice row from a plane's normal.
// XY normal=(0,0,1)->0, XZ normal=(0,1,0)->1, YZ normal=(1,0,0)->2.
static int index_from_plane(const SketchPlane& p)
{
    if (std::abs(p.normal.y()) > 0.5) return 1; // XZ
    if (std::abs(p.normal.x()) > 0.5) return 2; // YZ
    return 0;                                   // XY
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
        // Prepare-toolbar-sized buttons (40px cell / 28px glyph) so the Design
        // ribbon matches the rest of the app instead of feeling tiny.
        auto* b = new ScalableButton(m_toolbar, wxID_ANY, icon, "", wxSize(52, 52),
                                     wxDefaultPosition, wxBU_EXACTFIT | wxBORDER_NONE, false, 42);
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
            const SketchPlane plane = plane_from_choice(m_draw_plane->GetSelection());
            m_viewport->begin_sketch(plane, mode);
            m_construction->SetValue(false);   // a fresh session starts non-construction
        } else {
            m_viewport->set_sketch_tool(mode);
        }
        m_viewport->set_sketch_construction(m_construction->GetValue());
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(hint);
        m_status->Refresh();
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
    auto fadd = [this](wxWindow* w) { m_tb_feature->Add(w, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2); };
    m_tb_feature->Add(caption(_L("FEATURES")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    {
        // Onshape-style FEATURE flyouts: same themed-DropDown pattern as the sketch toolbar
        // (tinted glyphs, Body_14 measure, content-width popup) but each entry runs an
        // arbitrary action — the existing per-feature handler — instead of selecting a Mode.
        struct FeatVar { const char* icon; wxString tip; wxString hint; std::function<void()> action; };
        struct FeatFlyout {
            std::vector<wxString> texts, tips;
            std::vector<wxBitmap> icons;
            std::vector<std::function<void()>> actions;
            std::vector<std::string> icon_names;
            ScalableButton* btn = nullptr;
            DropDown drop;                 // declared LAST: destroyed before the vectors it references
            FeatFlyout() : drop(texts, tips, icons) {}
        };
        auto feat_dropdown = [&](const char* def_icon, const wxString& grp, std::vector<FeatVar> vars) {
            auto* b = icon_btn(def_icon, grp);
            b->SetFont(Label::Body_14);   // measure popup labels in the popup's font (no truncation)
            auto fo = std::make_shared<FeatFlyout>();
            for (auto& v : vars) {
                fo->texts.push_back(v.tip);
                fo->tips.push_back(v.hint);
                fo->icons.push_back(tint(create_scaled_bitmap(v.icon, m_form, 18), drop_icon_col));
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
            fadd(b);
            auto* chev = new wxStaticText(m_toolbar, wxID_ANY, wxString::FromUTF8("\xE2\x96\xBE"));
            chev->SetForegroundColour(dp_sec_text());
            chev->SetFont(Label::Body_9);
            m_tb_feature->Add(chev, 0, wxALIGN_BOTTOM | wxBOTTOM | wxRIGHT, 5);
            return b;
        };

        auto* b_sketch = icon_btn("design_sketch", _L("Sketch"));
        b_sketch->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            populate_plane_choices(m_draw_plane);   // surface datum planes in the picker
            set_ui_mode(UiMode::Sketch);
            m_status->SetForegroundColour(wxNullColour);
            m_status->SetLabel(_L("Pick a plane and a sketch tool, then draw"));
            m_status->Refresh();
        });
        fadd(b_sketch);
        add_sep(m_tb_feature);
        // Add material: Extrude / Revolve / Sweep / Loft
        feat_dropdown("design_extrude", _L("Add material (extrude / revolve / sweep / loft)"), {
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
                    m_status->SetLabel(_L("Create a sketch, or pick a solid face, first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::Extrude);
             }},
            {"design_revolve", _L("Revolve"), _L("Revolve a profile about an axis"),
             [this] {
                m_revolve_sketch_ref = resolve_extrude_sketch();
                if (m_revolve_sketch_ref < 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    m_status->SetLabel(_L("Create a sketch profile to revolve first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::Revolve);
             }},
            {"design_sweep", _L("Sweep"), _L("Sweep a profile along a path"),
             [this] {
                m_sweep_profile_ref = resolve_extrude_sketch();
                m_sweep_path_ref    = -1;   // fresh sweep: default the picker to the first sketch
                if (m_sweep_profile_ref < 0) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    m_status->SetLabel(_L("Create a profile sketch to sweep first"));
                    m_status->Refresh();
                    return;
                }
                open_tool(Tool::Sweep);
             }},
            {"design_loft", _L("Loft"), _L("Loft (skin) between two or more profiles"),
             [this] {
                // Loft skins 2+ profile sketches; need at least two to be meaningful.
                int n = 0;
                for (const auto& f : m_doc.features)
                    if (f.type == CadFeatureType::Sketch) ++n;
                if (n < 2) {
                    m_status->SetForegroundColour(wxColour(235, 110, 110));
                    m_status->SetLabel(_L("Create at least two profile sketches to loft"));
                    m_status->Refresh();
                    return;
                }
                m_loft_refs.clear();   // fresh loft: nothing pre-checked
                open_tool(Tool::Loft);
             }},
        });

        auto* b_pattern = icon_btn("design_pattern", _L("Pattern"));
        b_pattern->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            // Pattern replicates an existing body — needs at least one solid.
            if (m_doc.bodies.empty()) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                m_status->SetLabel(_L("Create a solid body to pattern first"));
                m_status->Refresh();
                return;
            }
            open_tool(Tool::Pattern);
        });
        fadd(b_pattern);

        auto* b_plane = icon_btn("design_plane", _L("Plane"));
        b_plane->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            populate_plane_choices(m_plane_base);   // refresh base list w/ existing datum planes
            open_tool(Tool::Plane);
        });
        fadd(b_plane);

        auto* b_boolean = icon_btn("design_boolean", _L("Boolean (combine bodies)"));
        b_boolean->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            // A body-body boolean needs at least two solids to combine.
            if (m_doc.bodies.size() < 2) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                m_status->SetLabel(_L("Boolean needs two bodies — create or import a second solid"));
                m_status->Refresh();
                return;
            }
            populate_body_choices();
            open_tool(Tool::Boolean);
        });
        fadd(b_boolean);

        auto* b_cut = icon_btn("design_cut", _L("Cut (split a body with a plane)"));
        b_cut->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            // A plane cut needs at least one solid to slice.
            if (m_doc.bodies.empty()) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                m_status->SetLabel(_L("Create a solid body to cut first"));
                m_status->Refresh();
                return;
            }
            populate_plane_choices(m_cut_plane);
            populate_body_choices();
            open_tool(Tool::Cut);
        });
        fadd(b_cut);

        // Color — override the selected body's display colour (per-body, survives recompute).
        auto* b_color = icon_btn("color_palette", _L("Color — set the selected body's display colour"));
        b_color->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_set_body_color(); });
        fadd(b_color);

        // Dress-up: Fillet/Chamfer / Draft / Shell
        feat_dropdown("design_dressup", _L("Dress-up (fillet / chamfer / draft / shell)"), {
            {"design_dressup", _L("Fillet / Chamfer"), _L("Round or bevel a picked edge"),
             [this] { open_tool(Tool::Dressup); }},
            {"design_draft", _L("Draft (taper a face)"), _L("Tilt a picked face by a draft angle"),
             [this] { open_tool(Tool::Draft); }},
            {"design_shell", _L("Shell"), _L("Hollow the body to a wall thickness, opening a picked face"),
             [this] { open_tool(Tool::Shell); }},
        });

        // Hole / Thread — drilling into a solid (both face-aware)
        feat_dropdown("design_hole", _L("Hole / thread"), {
            {"design_hole", _L("Hole"), _L("Drill a hole, centred on a picked face or placed on a plane"),
             [this] {
                // #2: drill on the picked solid face, centred on it (origin = face centroid,
                // normal = inward). Otherwise fall back to the plane dropdown. m_hole_x/y then
                // read as the offset from the face centre (editable for precise placement).
                m_hole_on_face   = false;
                m_hole_face_body = -1;
                m_hole_has_bounds = false;
                if (m_sel_solid_face >= 0 && m_sel_solid_body >= 0
                    && m_sel_solid_body < int(m_doc.bodies.size())) {
                    const TopoDS_Face face = GeometryEngine::face_by_index(
                        m_doc.bodies[m_sel_solid_body].shape, m_sel_solid_face);
                    if (!face.IsNull()) {
                        m_hole_face_plane = face_plane_inward(face);
                        m_hole_on_face    = true;
                        m_hole_face_body  = m_sel_solid_body;
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
             }},
            {"design_thread", _L("Thread"), _L("Thread a picked cylindrical face (hole bore or cylinder)"),
             [this] {
                // #3: invoke on a picked CYLINDRICAL face — a hole bore (internal thread) or a
                // cylinder's lateral surface (external) — deriving axis, radius and internal/
                // external from it. Otherwise fall back to the plane dropdown.
                m_thread_on_face   = false;
                m_thread_face_body = -1;
                if (m_sel_solid_face >= 0 && m_sel_solid_body >= 0
                    && m_sel_solid_body < int(m_doc.bodies.size())) {
                    const TopoDS_Face face = GeometryEngine::face_by_index(
                        m_doc.bodies[m_sel_solid_body].shape, m_sel_solid_face);
                    const GeometryEngine::CylinderFace cf = GeometryEngine::cylinder_of_face(face);
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
                        if (m_thread_radius)   m_thread_radius->SetValue(cf.radius);
                        if (m_thread_height)   m_thread_height->SetValue(cf.height);
                        if (m_thread_internal) m_thread_internal->SetValue(cf.internal);
                        if (m_thread_x) m_thread_x->SetValue(0.0);   // on the axis
                        if (m_thread_y) m_thread_y->SetValue(0.0);
                    } else if (m_sel_solid_face >= 0) {
                        m_status->SetForegroundColour(wxColour(235, 110, 110));
                        m_status->SetLabel(_L("Pick a cylindrical face (hole bore or cylinder) for a thread"));
                        m_status->Refresh();
                    }
                }
                open_tool(Tool::Thread);
             }},
        });
        add_sep(m_tb_feature);
        // Text / SVG insert tools live in the SKETCH toolbar (they produce 2D profiles =
        // sketches), not here. STEP stays in Features: it imports a whole B-rep solid.
        // Import STEP — standalone: a STEP comes in as a whole editable B-rep body, not a profile.
        auto* b_step = icon_btn("design_step", _L("Import STEP (editable B-rep solid)"));
        b_step->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_import_step(); });
        fadd(b_step);
        add_sep(m_tb_feature);
        auto* b_constrain = icon_btn("design_constrain", _L("Constrain selected sketch"));
        b_constrain->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            on_begin_constrain();
            if (m_viewport && (m_viewport->is_constraining() || m_viewport->is_constraining_entities()))
                set_ui_mode(UiMode::Constrain);
        });
        fadd(b_constrain);
    }

    // --- Sketch group: plane + entity tools + Construction + Finish
    m_tb_sketch = new wxBoxSizer(wxHORIZONTAL);
    auto sadd = [this](wxWindow* w) { m_tb_sketch->Add(w, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2); };
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
            std::vector<wxString> texts, tips;
            std::vector<wxBitmap> icons;
            std::vector<DesignSketchTool::Mode> modes;
            std::vector<wxString> hints;
            std::vector<std::string> icon_names;
            ScalableButton* btn = nullptr;
            DropDown drop;                 // declared LAST: destroyed before the vectors it references
            ToolFlyout() : drop(texts, tips, icons) {}
        };
        auto dropdown = [&](const char* def_icon, const wxString& grp, std::vector<SkVar> vars) {
            auto* b = icon_btn(def_icon, grp);
            // messureSize() measures labels with the PARENT's font (this button) but the
            // popup draws them in Body_14 — so an under-sized button font truncates rows.
            // The button is icon-only (no label), so giving it Body_14 is invisible and
            // makes the content-width measure match the draw.
            b->SetFont(Label::Body_14);
            auto fo = std::make_shared<ToolFlyout>();
            for (auto& v : vars) {
                fo->texts.push_back(v.tip);
                fo->tips.push_back(v.hint);
                fo->icons.push_back(tint(create_scaled_bitmap(v.icon, m_form, 18), drop_icon_col));
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
            m_tb_sketch->Add(chev, 0, wxALIGN_BOTTOM | wxBOTTOM | wxRIGHT, 5);
            return b;
        };
        skbtn("design_select",    DesignSketchTool::Mode::Select,           _L("Select"),
              _L("Click to select; Shift to add; double-click for a whole loop"));
        skbtn("design_dimension", DesignSketchTool::Mode::Dimension, _L("Dimension"),
              _L("Click 2 points or a line / circle / arc to place a dimension"));
        add_sep(m_tb_sketch);
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
            {"design_slot",     DesignSketchTool::Mode::Slot,    _L("Slot"),     _L("Click two centerline ends, then a point for width")},
            {"design_slot_arc", DesignSketchTool::Mode::ArcSlot, _L("Arc slot"), _L("Click center, start, end, then a point for the width")} });
        dropdown("design_ellipse", _L("Ellipse"), {
            {"design_ellipse",     DesignSketchTool::Mode::Ellipse,    _L("Ellipse"),        _L("Click center, a major-axis end, then a point for the minor axis")},
            {"design_ellipse_arc", DesignSketchTool::Mode::EllipseArc, _L("Elliptical arc"), _L("Click center, major-axis end, minor point, then arc start and end")} });
        skbtn("design_bspline", DesignSketchTool::Mode::BSpline, _L("Spline"),
              _L("Click control points; double-click or right-click to finish"));
        skbtn("design_point",   DesignSketchTool::Mode::Point,   _L("Point"),
              _L("Click to place a point"));
        add_sep(m_tb_sketch);
        // Insert tools — Text / SVG produce a 2D profile (a sketch), so they belong with
        // the sketch tools, not in the generic Features strip. Each places the art
        // in-canvas, then commits via the Insert card's Confirm.
        {
            auto* b_text = icon_btn("design_text", _L("Text — emboss text as a profile"));
            b_text->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_add_text(); });
            sadd(b_text);
            auto* b_svg = icon_btn("design_svg", _L("SVG — import an outline as a profile"));
            b_svg->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_import_svg(); });
            sadd(b_svg);
        }
        add_sep(m_tb_sketch);
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
        dropdown("design_move", _L("Move / rotate / scale"), {
            {"design_move",   DesignSketchTool::Mode::Move,   _L("Move (translate)"),        _L("Pick entities, then drag the handle or click the distance; click empty to apply")},
            {"design_rotate", DesignSketchTool::Mode::Rotate, _L("Rotate (about centroid)"), _L("Pick entities, then drag around the pivot or click the angle; click empty to apply")},
            {"design_scale",  DesignSketchTool::Mode::Scale,  _L("Scale (about centroid)"),  _L("Pick entities, then drag the handle or click the factor; click empty to apply")} });
        dropdown("design_array", _L("Linear / polar array"), {
            {"design_array",      DesignSketchTool::Mode::Array,      _L("Linear array"),                 _L("Pick entities, drag the spacing handle, click the count; click empty to apply")},
            {"design_polararray", DesignSketchTool::Mode::PolarArray, _L("Polar array (about centroid)"), _L("Pick entities, drag the sweep handle, click the count; click empty to apply")} });

        m_sides = new wxSpinCtrl(m_toolbar, wxID_ANY, "6", wxDefaultPosition, wxSize(50, -1));
        m_sides->SetRange(3, 64);
        m_sides->SetValue(6);
        auto* b_poly = icon_btn("design_polygon", _L("Polygon"));
        b_poly->Bind(wxEVT_BUTTON, [this, select_tool](wxCommandEvent&) {
            if (m_viewport) {
                m_viewport->set_sketch_polygon_sides(m_sides->GetValue());
                m_viewport->set_sketch_polygon_circumscribed(m_poly_circ && m_poly_circ->GetValue());
            }
            select_tool(DesignSketchTool::Mode::Polygon, _L("Click center then a vertex")); });
        m_sides->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
            if (m_viewport) m_viewport->set_sketch_polygon_sides(m_sides->GetValue()); });
        sadd(b_poly);
        sadd(m_sides);
        m_poly_circ = new wxCheckBox(m_toolbar, wxID_ANY, _L("Circumscribed"));
        m_poly_circ->SetForegroundColour(dp_ctl_text());
        m_poly_circ->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            if (m_viewport) m_viewport->set_sketch_polygon_circumscribed(m_poly_circ->GetValue()); });
        sadd(m_poly_circ);
        add_sep(m_tb_sketch);
        // Constrain — reachable mid-sketch: commits the live sketch in place and drops into
        // Constrain mode, where the geometric/dimensional palette + Trim + Extend (scissors)
        // are picked and applied. (Trim/Extend are pick-then-apply, so they live there, not as
        // bare toolbar buttons.)
        auto* b_constrain_sk = icon_btn("design_constrain",
            _L("Constrain — add geometric/dimensional relations; Trim/Extend live here"));
        b_constrain_sk->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { enter_constrain_inline(); });
        sadd(b_constrain_sk);
        add_sep(m_tb_sketch);
        m_construction->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            if (m_viewport && m_viewport->is_sketching())
                m_viewport->set_sketch_construction(m_construction->GetValue()); });
        sadd(m_construction);
        add_sep(m_tb_sketch);
        auto* b_del = icon_btn("design_delete", _L("Delete selected"));
        b_del->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_viewport) m_viewport->delete_selected_sketch_entities(); });
        sadd(b_del);
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
            auto* b = new ScalableButton(m_toolbar, wxID_ANY, icon, "", wxSize(52, 52),
                                         wxDefaultPosition, wxBU_EXACTFIT | wxBORDER_NONE, false, 42);
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
        m_tb_history->Add(m_btn_undo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
        m_tb_history->Add(m_btn_redo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    }

    auto* tbrow = new wxBoxSizer(wxHORIZONTAL);
    tbrow->AddSpacer(8);
    tbrow->Add(m_tb_history,   0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    add_sep(tbrow);
    tbrow->Add(m_tb_feature,   0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    tbrow->Add(m_tb_sketch,    0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    tbrow->Add(m_tb_constrain, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    tbrow->AddStretchSpacer();
    tbrow->Add(m_tb_action,    0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
    m_toolbar->SetSizer(tbrow);

    // Onshape-style dialog-card header: feature icon + bold title. out receives
    // the title control so open_tool() can retitle it per feature.
    auto card_header = [this](const char* icon, const wxString& title, wxStaticText*& out) -> wxSizer* {
        auto* h  = new wxBoxSizer(wxHORIZONTAL);
        auto* ic = new wxStaticBitmap(m_form, wxID_ANY, create_scaled_bitmap(icon, m_form, 18));
        out = new wxStaticText(m_form, wxID_ANY, title);
        out->SetFont(Label::Head_14);   // Orca shared HarmonyOS card-title font
        h->Add(ic,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        h->Add(out, 0, wxALIGN_CENTER_VERTICAL);
        return h;
    };

    // --- Sketch dialog (shape definition only — no distance/mode) ---
    auto* form = new wxFlexGridSizer(2, 6, 8);

    m_shape = new wxChoice(m_form, wxID_ANY);
    m_shape->Append("Rectangle");
    m_shape->Append("Circle");
    m_shape->SetSelection(0);
    form->Add(new wxStaticText(m_form, wxID_ANY, _L("Shape")), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(m_shape);

    m_plane = new wxChoice(m_form, wxID_ANY);
    m_plane->Append("XY");
    m_plane->Append("XZ");
    m_plane->Append("YZ");
    m_plane->SetSelection(0);
    form->Add(new wxStaticText(m_form, wxID_ANY, _L("Plane")), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(m_plane);

    m_width = make_spin(m_form, 20);
    form->Add(new wxStaticText(m_form, wxID_ANY, _L("Width / X")), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(m_width);

    m_height = make_spin(m_form, 20);
    form->Add(new wxStaticText(m_form, wxID_ANY, _L("Height / Y")), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(m_height);

    m_radius = make_spin(m_form, 10);
    form->Add(new wxStaticText(m_form, wxID_ANY, _L("Radius")), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(m_radius);

    m_box_sketch = new wxBoxSizer(wxVERTICAL);
    m_box_sketch->Add(card_header("design_sketch", _L("Sketch"), m_hdr_sketch), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_sketch->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_box_sketch->Add(form, 0, wxALL, 12);
    root->Add(m_box_sketch, 0, wxEXPAND);

    // --- Extrude dialog (consumes the selected sketch) ---
    m_box_extrude = new wxBoxSizer(wxVERTICAL);
    m_box_extrude->Add(card_header("design_extrude", _L("Extrude"), m_hdr_extrude), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_extrude->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_extrude_sketch_label = new wxStaticText(m_form, wxID_ANY, _L("Sketch: —"));
    m_box_extrude->Add(m_extrude_sketch_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    {
        auto* eform = new wxFlexGridSizer(2, 6, 8);

        m_distance = make_spin(m_form, 10);
        eform->Add(new wxStaticText(m_form, wxID_ANY, _L("Extrude dist")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(m_distance);

        // End condition (order MUST match ExtrudeEnd: Blind/Symmetric/TwoSided/ThroughAll/
        // UpToFace/UpToVertex). Up-to-face uses the currently click-selected solid face.
        m_extrude_end = new wxChoice(m_form, wxID_ANY);
        for (const char* s : { "Blind", "Symmetric", "Two-sided", "Through all",
                               "Up to face", "Up to vertex" })
            m_extrude_end->Append(s);
        m_extrude_end->SetSelection(0);
        eform->Add(new wxStaticText(m_form, wxID_ANY, _L("End")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(m_extrude_end);

        m_distance2 = make_spin(m_form, 5, 0.0, 100000.0);   // second-side depth (Two-sided)
        eform->Add(new wxStaticText(m_form, wxID_ANY, _L("2nd dist")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(m_distance2);

        m_taper = make_spin(m_form, 0.0, -89.0, 89.0);       // draft angle (deg)
        eform->Add(new wxStaticText(m_form, wxID_ANY, _L("Taper °")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(m_taper);

        m_mode = new wxChoice(m_form, wxID_ANY);
        // Order is load-bearing: index maps to BooleanMode (New=0, Add=1, Cut=2, Intersect=3).
        // Labels use Onshape wording so the choice reads as the user thinks of it.
        m_mode->Append(_L("New body"));   // separate coexisting solid
        m_mode->Append(_L("Join"));       // fuse into the target body (was "Add")
        m_mode->Append(_L("Cut"));        // subtract from the target body
        m_mode->Append(_L("Intersect"));  // keep only the overlap
        m_mode->SetSelection(0);
        eform->Add(new wxStaticText(m_form, wxID_ANY, _L("Result")), 0, wxALIGN_CENTER_VERTICAL);
        eform->Add(m_mode);

        m_flip = new wxCheckBox(m_form, wxID_ANY, _L("Flip direction"));
        eform->Add(new wxStaticText(m_form, wxID_ANY, wxEmptyString));
        eform->Add(m_flip);

        m_box_extrude->Add(eform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    }
    root->Add(m_box_extrude, 0, wxEXPAND);

    // --- Dress-up (Fillet / Chamfer) ---
    auto* dform = new wxFlexGridSizer(2, 6, 8);

    m_dressup_type = new wxChoice(m_form, wxID_ANY);
    m_dressup_type->Append("Fillet");
    m_dressup_type->Append("Chamfer");
    m_dressup_type->SetSelection(0);
    dform->Add(new wxStaticText(m_form, wxID_ANY, _L("Dress-up")), 0, wxALIGN_CENTER_VERTICAL);
    dform->Add(m_dressup_type);

    m_face_group = new wxChoice(m_form, wxID_ANY);
    m_face_group->Append("Top");      // index 0 -> FaceGroup::Top
    m_face_group->Append("Bottom");   // 1 -> Bottom
    m_face_group->Append("Lateral");  // 2 -> Lateral
    m_face_group->Append("All");      // 3 -> All
    m_face_group->SetSelection(3);
    dform->Add(new wxStaticText(m_form, wxID_ANY, _L("Edges")), 0, wxALIGN_CENTER_VERTICAL);
    dform->Add(m_face_group);

    m_dressup_size = make_spin(m_form, 2.0);
    dform->Add(new wxStaticText(m_form, wxID_ANY, _L("Size (r/dist)")), 0, wxALIGN_CENTER_VERTICAL);
    dform->Add(m_dressup_size);

    m_box_dressup = new wxBoxSizer(wxVERTICAL);
    m_box_dressup->Add(card_header("design_dressup", _L("Fillet / Chamfer"), m_hdr_dressup), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_dressup->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_box_dressup->Add(dform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    root->Add(m_box_dressup, 0, wxEXPAND);

    // --- Hole (positioned circular cut) ---
    auto* hform = new wxFlexGridSizer(2, 6, 8);

    m_hole_plane = new wxChoice(m_form, wxID_ANY);
    m_hole_plane->Append("XY");
    m_hole_plane->Append("XZ");
    m_hole_plane->Append("YZ");
    m_hole_plane->SetSelection(0);
    // Picking a plane here is an explicit choice: drop any on-face hijack (a stale face pick
    // could keep m_hole_on_face true, so the dropdown was ignored and the hole drilled on the
    // face's plane instead of the chosen XY/XZ/YZ).
    m_hole_plane->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e) {
        m_hole_on_face    = false;
        m_hole_has_bounds = false;
        update_hole_gizmo();
        refresh_preview();
        e.Skip();
    });
    hform->Add(new wxStaticText(m_form, wxID_ANY, _L("Hole plane")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(m_hole_plane);

    m_hole_diameter = make_spin(m_form, 6.0);
    hform->Add(new wxStaticText(m_form, wxID_ANY, _L("Diameter")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(m_hole_diameter);

    m_hole_depth = make_spin(m_form, 10.0);
    hform->Add(new wxStaticText(m_form, wxID_ANY, _L("Depth (blind)")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(m_hole_depth);

    m_hole_x = make_spin(m_form, 0.0, -1000.0, 1000.0);
    hform->Add(new wxStaticText(m_form, wxID_ANY, _L("Pos X")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(m_hole_x);

    m_hole_y = make_spin(m_form, 0.0, -1000.0, 1000.0);
    hform->Add(new wxStaticText(m_form, wxID_ANY, _L("Pos Y")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(m_hole_y);

    m_hole_through = new wxCheckBox(m_form, wxID_ANY, _L("Through"));
    m_hole_through->SetValue(true);
    hform->Add(new wxStaticText(m_form, wxID_ANY, _L("Mode")), 0, wxALIGN_CENTER_VERTICAL);
    hform->Add(m_hole_through);

    m_box_hole = new wxBoxSizer(wxVERTICAL);
    m_box_hole->Add(card_header("design_hole", _L("Hole"), m_hdr_hole), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_hole->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_box_hole->Add(hform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    root->Add(m_box_hole, 0, wxEXPAND);

    // --- Thread (helical) ---
    auto* tform = new wxFlexGridSizer(2, 6, 8);

    m_thread_plane = new wxChoice(m_form, wxID_ANY);
    m_thread_plane->Append("XY");
    m_thread_plane->Append("XZ");
    m_thread_plane->Append("YZ");
    m_thread_plane->SetSelection(0);
    tform->Add(new wxStaticText(m_form, wxID_ANY, _L("Thread plane")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_plane);

    // Standard designation picker — fills pitch/depth (and nominal radius) from the
    // ISO metric / Unified imperial tables. "Custom" leaves the manual spins alone.
    m_thread_std = new wxChoice(m_form, wxID_ANY);
    m_thread_std->Append(_L("Custom"));
    for (const ThreadSpec& s : thread_standards())
        m_thread_std->Append(s.name);
    m_thread_std->SetSelection(0);
    m_thread_std->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { apply_thread_standard(); });
    tform->Add(new wxStaticText(m_form, wxID_ANY, _L("Standard")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_std);

    m_thread_radius = make_spin(m_form, 5.0);
    tform->Add(new wxStaticText(m_form, wxID_ANY, _L("Radius")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_radius);

    m_thread_pitch = make_spin(m_form, 2.0);
    tform->Add(new wxStaticText(m_form, wxID_ANY, _L("Pitch")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_pitch);

    m_thread_height = make_spin(m_form, 10.0);
    tform->Add(new wxStaticText(m_form, wxID_ANY, _L("Length")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_height);

    m_thread_depth = make_spin(m_form, 1.0);
    tform->Add(new wxStaticText(m_form, wxID_ANY, _L("Thread depth")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_depth);

    m_thread_x = make_spin(m_form, 0.0, -1000.0, 1000.0);
    tform->Add(new wxStaticText(m_form, wxID_ANY, _L("Pos X")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_x);

    m_thread_y = make_spin(m_form, 0.0, -1000.0, 1000.0);
    tform->Add(new wxStaticText(m_form, wxID_ANY, _L("Pos Y")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_y);

    m_thread_internal = new wxCheckBox(m_form, wxID_ANY, _L("Internal (tapped bore)"));
    m_thread_internal->SetValue(false);
    // External rod uses the major radius; an internal tapped bore uses the minor
    // (tap-drill) radius — re-derive the nominal radius when the role flips.
    m_thread_internal->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
        apply_thread_standard();
        e.Skip();
    });
    tform->Add(new wxStaticText(m_form, wxID_ANY, _L("Internal")), 0, wxALIGN_CENTER_VERTICAL);
    tform->Add(m_thread_internal);

    m_box_thread = new wxBoxSizer(wxVERTICAL);
    m_box_thread->Add(card_header("design_thread", _L("Thread"), m_hdr_thread), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_thread->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_box_thread->Add(tform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    root->Add(m_box_thread, 0, wxEXPAND);

    // --- Revolve (sweep a sketch profile about an in-plane axis) ---
    m_box_revolve = new wxBoxSizer(wxVERTICAL);
    m_box_revolve->Add(card_header("design_extrude", _L("Revolve"), m_hdr_revolve), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_revolve->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_revolve_sketch_label = new wxStaticText(m_form, wxID_ANY, _L("Sketch: —"));
    m_box_revolve->Add(m_revolve_sketch_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    {
        auto* rform = new wxFlexGridSizer(2, 6, 8);

        m_revolve_angle = make_spin(m_form, 360.0, 1.0, 360.0);
        rform->Add(new wxStaticText(m_form, wxID_ANY, _L("Angle °")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_revolve_angle);

        m_revolve_axis = new wxChoice(m_form, wxID_ANY);
        m_revolve_axis->Append("Plane X");
        m_revolve_axis->Append("Plane Y");
        m_revolve_axis->SetSelection(0);
        rform->Add(new wxStaticText(m_form, wxID_ANY, _L("Axis")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_revolve_axis);

        m_revolve_mode = new wxChoice(m_form, wxID_ANY);
        m_revolve_mode->Append("New");
        m_revolve_mode->Append("Add");
        m_revolve_mode->Append("Cut");
        m_revolve_mode->Append("Intersect");
        m_revolve_mode->SetSelection(0);
        rform->Add(new wxStaticText(m_form, wxID_ANY, _L("Mode")), 0, wxALIGN_CENTER_VERTICAL);
        rform->Add(m_revolve_mode);

        m_revolve_flip = new wxCheckBox(m_form, wxID_ANY, _L("Flip direction"));
        rform->Add(new wxStaticText(m_form, wxID_ANY, wxEmptyString));
        rform->Add(m_revolve_flip);

        m_box_revolve->Add(rform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    }
    root->Add(m_box_revolve, 0, wxEXPAND);

    // --- Sweep (sweep a profile sketch along a path sketch) ---
    m_box_sweep = new wxBoxSizer(wxVERTICAL);
    m_box_sweep->Add(card_header("design_extrude", _L("Sweep"), m_hdr_sweep), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_sweep->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_sweep_profile_label = new wxStaticText(m_form, wxID_ANY, _L("Profile: —"));
    m_box_sweep->Add(m_sweep_profile_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    {
        auto* sform = new wxFlexGridSizer(2, 6, 8);

        m_sweep_path = new wxChoice(m_form, wxID_ANY);
        sform->Add(new wxStaticText(m_form, wxID_ANY, _L("Path")), 0, wxALIGN_CENTER_VERTICAL);
        sform->Add(m_sweep_path);

        m_sweep_mode = new wxChoice(m_form, wxID_ANY);
        m_sweep_mode->Append("New");
        m_sweep_mode->Append("Add");
        m_sweep_mode->Append("Cut");
        m_sweep_mode->Append("Intersect");
        m_sweep_mode->SetSelection(0);
        sform->Add(new wxStaticText(m_form, wxID_ANY, _L("Mode")), 0, wxALIGN_CENTER_VERTICAL);
        sform->Add(m_sweep_mode);

        m_box_sweep->Add(sform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    }
    root->Add(m_box_sweep, 0, wxEXPAND);

    // --- Pattern (replicate the target body: linear or circular) ---
    m_box_pattern = new wxBoxSizer(wxVERTICAL);
    m_box_pattern->Add(card_header("design_extrude", _L("Pattern"), m_hdr_pattern), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_pattern->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    {
        auto* pform = new wxFlexGridSizer(2, 6, 8);

        m_pattern_type = new wxChoice(m_form, wxID_ANY);
        m_pattern_type->Append("Linear");
        m_pattern_type->Append("Circular");
        m_pattern_type->SetSelection(0);
        pform->Add(new wxStaticText(m_form, wxID_ANY, _L("Type")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(m_pattern_type);

        m_pattern_count = make_spin(m_form, 3, 1, 999);
        pform->Add(new wxStaticText(m_form, wxID_ANY, _L("Count")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(m_pattern_count);

        m_pattern_spacing = make_spin(m_form, 20.0, 0.01, 100000.0);
        pform->Add(new wxStaticText(m_form, wxID_ANY, _L("Spacing")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(m_pattern_spacing);

        m_pattern_dir = new wxChoice(m_form, wxID_ANY);
        m_pattern_dir->Append("Plane X");
        m_pattern_dir->Append("Plane Y");
        m_pattern_dir->SetSelection(0);
        pform->Add(new wxStaticText(m_form, wxID_ANY, _L("Direction")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(m_pattern_dir);

        m_pattern_angle = make_spin(m_form, 360.0, 1.0, 360.0);
        pform->Add(new wxStaticText(m_form, wxID_ANY, _L("Total angle°")), 0, wxALIGN_CENTER_VERTICAL);
        pform->Add(m_pattern_angle);

        m_box_pattern->Add(pform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    }
    root->Add(m_box_pattern, 0, wxEXPAND);

    // --- Boolean (combine two existing bodies: union / subtract / intersect) ---
    m_box_boolean = new wxBoxSizer(wxVERTICAL);
    m_box_boolean->Add(card_header("design_boolean", _L("Boolean"), m_hdr_boolean), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_boolean->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    {
        auto* bform = new wxFlexGridSizer(2, 6, 8);

        m_bool_op = new wxChoice(m_form, wxID_ANY);
        m_bool_op->Append("Union (join)");
        m_bool_op->Append("Subtract (cut)");
        m_bool_op->Append("Intersect");
        m_bool_op->SetSelection(0);
        m_bool_op->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_preview(); });
        bform->Add(new wxStaticText(m_form, wxID_ANY, _L("Operation")), 0, wxALIGN_CENTER_VERTICAL);
        bform->Add(m_bool_op);

        m_bool_target = new wxChoice(m_form, wxID_ANY);
        m_bool_target->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_preview(); });
        bform->Add(new wxStaticText(m_form, wxID_ANY, _L("Target (kept)")), 0, wxALIGN_CENTER_VERTICAL);
        bform->Add(m_bool_target);

        m_bool_tool = new wxChoice(m_form, wxID_ANY);
        m_bool_tool->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_preview(); });
        bform->Add(new wxStaticText(m_form, wxID_ANY, _L("Tool")), 0, wxALIGN_CENTER_VERTICAL);
        bform->Add(m_bool_tool);

        // Fuzzy tolerance (mm): the main use is a tool body cutting a destination — a small
        // tolerance lets near-coincident mating faces resolve into a clean cut instead of a
        // failed boolean or sliver faces. 0 = exact.
        m_bool_tol = make_spin(m_form, 0.0, 0.0, 100.0);
        m_bool_tol->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        bform->Add(new wxStaticText(m_form, wxID_ANY, _L("Tolerance")), 0, wxALIGN_CENTER_VERTICAL);
        bform->Add(m_bool_tol);

        m_box_boolean->Add(bform, 0, wxLEFT | wxRIGHT | wxTOP, 12);

        m_bool_keep = new wxCheckBox(m_form, wxID_ANY, _L("Keep tool body"));
        m_bool_keep->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { refresh_preview(); });
        m_box_boolean->Add(m_bool_keep, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    }
    root->Add(m_box_boolean, 0, wxEXPAND);

    // --- Cut (split a body with a plane): parameters only; ✓/✗ live on the ribbon ---
    m_box_cut = new wxBoxSizer(wxVERTICAL);
    m_box_cut->Add(card_header("design_cut", _L("Cut"), m_hdr_cut), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_cut->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    {
        auto* cform = new wxFlexGridSizer(2, 6, 8);

        m_cut_target = new wxChoice(m_form, wxID_ANY);
        m_cut_target->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_preview(); });
        cform->Add(new wxStaticText(m_form, wxID_ANY, _L("Body")), 0, wxALIGN_CENTER_VERTICAL);
        cform->Add(m_cut_target);

        m_cut_plane = new wxChoice(m_form, wxID_ANY);
        m_cut_plane->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_preview(); });
        cform->Add(new wxStaticText(m_form, wxID_ANY, _L("Plane")), 0, wxALIGN_CENTER_VERTICAL);
        cform->Add(m_cut_plane);

        m_cut_offset = make_spin(m_form, 0.0, -10000.0, 10000.0);
        m_cut_offset->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_preview(); });
        cform->Add(new wxStaticText(m_form, wxID_ANY, _L("Offset")), 0, wxALIGN_CENTER_VERTICAL);
        cform->Add(m_cut_offset);

        m_box_cut->Add(cform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
        // The cut always leaves BOTH pieces as separate bodies (a non-destructive split);
        // delete one from the tree afterwards if you only want a half.
    }
    root->Add(m_box_cut, 0, wxEXPAND);

    // --- Insert (Text / SVG placement): Confirm/Cancel for the in-canvas art transform ---
    m_box_insert = new wxBoxSizer(wxVERTICAL);
    m_box_insert->Add(card_header("design_text", _L("Insert"), m_hdr_insert), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_insert->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_box_insert->Add(new wxStaticText(m_form, wxID_ANY,
        _L("Drag a corner to size, the centre to move.\nConfirm or Cancel in the toolbar above.")),
        0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    root->Add(m_box_insert, 0, wxEXPAND);

    // --- Plane (datum/reference plane: offset + tilt from a base plane; no solid) ---
    m_box_plane = new wxBoxSizer(wxVERTICAL);
    m_box_plane->Add(card_header("design_sketch", _L("Plane"), m_hdr_plane), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_plane->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    {
        auto* plform = new wxFlexGridSizer(2, 6, 8);

        m_plane_base = new wxChoice(m_form, wxID_ANY);
        populate_plane_choices(m_plane_base);   // XY/XZ/YZ + any existing datum planes
        plform->Add(new wxStaticText(m_form, wxID_ANY, _L("Base")), 0, wxALIGN_CENTER_VERTICAL);
        plform->Add(m_plane_base);

        m_plane_offset = make_spin(m_form, 20.0, -100000.0, 100000.0);
        plform->Add(new wxStaticText(m_form, wxID_ANY, _L("Offset")), 0, wxALIGN_CENTER_VERTICAL);
        plform->Add(m_plane_offset);

        m_plane_tilt = make_spin(m_form, 0.0, -180.0, 180.0);
        plform->Add(new wxStaticText(m_form, wxID_ANY, _L("Tilt°")), 0, wxALIGN_CENTER_VERTICAL);
        plform->Add(m_plane_tilt);

        m_plane_tilt_axis = new wxChoice(m_form, wxID_ANY);
        m_plane_tilt_axis->Append("Base X");
        m_plane_tilt_axis->Append("Base Y");
        m_plane_tilt_axis->SetSelection(0);
        plform->Add(new wxStaticText(m_form, wxID_ANY, _L("Tilt axis")), 0, wxALIGN_CENTER_VERTICAL);
        plform->Add(m_plane_tilt_axis);

        m_box_plane->Add(plform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    }
    root->Add(m_box_plane, 0, wxEXPAND);

    // --- Loft (skin a solid through 2+ ordered profile sketches) ---
    m_box_loft = new wxBoxSizer(wxVERTICAL);
    m_box_loft->Add(card_header("design_extrude", _L("Loft"), m_hdr_loft), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_loft->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_box_loft->Add(new wxStaticText(m_form, wxID_ANY, _L("Profiles (check 2+, in order):")),
                    0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_loft_list = new wxCheckListBox(m_form, wxID_ANY, wxDefaultPosition, wxSize(-1, 120));
    m_loft_list->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent&) { refresh_preview(); });
    m_box_loft->Add(m_loft_list, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    {
        auto* lform = new wxFlexGridSizer(2, 6, 8);

        m_loft_mode = new wxChoice(m_form, wxID_ANY);
        m_loft_mode->Append("New");
        m_loft_mode->Append("Add");
        m_loft_mode->Append("Cut");
        m_loft_mode->Append("Intersect");
        m_loft_mode->SetSelection(0);
        lform->Add(new wxStaticText(m_form, wxID_ANY, _L("Mode")), 0, wxALIGN_CENTER_VERTICAL);
        lform->Add(m_loft_mode);

        m_box_loft->Add(lform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    }
    m_loft_ruled = new wxCheckBox(m_form, wxID_ANY, _L("Ruled (straight) sections"));
    m_box_loft->Add(m_loft_ruled, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    root->Add(m_box_loft, 0, wxEXPAND);

    // --- Shell (hollow the current body to a wall thickness, removing one picked face) ---
    auto* sform = new wxFlexGridSizer(2, 6, 8);
    m_shell_thickness = make_spin(m_form, 2.0, 0.01, 100000.0);
    sform->Add(new wxStaticText(m_form, wxID_ANY, _L("Thickness")), 0, wxALIGN_CENTER_VERTICAL);
    sform->Add(m_shell_thickness);
    m_shell_face_label = new wxStaticText(m_form, wxID_ANY, _L("(all faces — closed hollow)"));
    sform->Add(new wxStaticText(m_form, wxID_ANY, _L("Open face")), 0, wxALIGN_CENTER_VERTICAL);
    sform->Add(m_shell_face_label, 0, wxALIGN_CENTER_VERTICAL);

    m_box_shell = new wxBoxSizer(wxVERTICAL);
    m_box_shell->Add(card_header("design_dressup", _L("Shell"), m_hdr_shell), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_shell->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_box_shell->Add(new wxStaticText(m_form, wxID_ANY,
                        _L("Pick a solid face to open it, then set the wall thickness.")),
                     0, wxLEFT | wxRIGHT, 12);
    m_box_shell->Add(sform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    root->Add(m_box_shell, 0, wxEXPAND);

    // --- Draft (taper a single picked solid face about the body bottom) ---
    auto* drform = new wxFlexGridSizer(2, 6, 8);
    m_draft_angle = make_spin(m_form, 5.0, -89.0, 89.0);
    drform->Add(new wxStaticText(m_form, wxID_ANY, _L("Angle (°)")), 0, wxALIGN_CENTER_VERTICAL);
    drform->Add(m_draft_angle);
    m_draft_face_label = new wxStaticText(m_form, wxID_ANY, _L("(pick a side face)"));
    drform->Add(new wxStaticText(m_form, wxID_ANY, _L("Face")), 0, wxALIGN_CENTER_VERTICAL);
    drform->Add(m_draft_face_label, 0, wxALIGN_CENTER_VERTICAL);

    m_box_draft = new wxBoxSizer(wxVERTICAL);
    m_box_draft->Add(card_header("design_dressup", _L("Draft"), m_hdr_draft), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_draft->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_box_draft->Add(new wxStaticText(m_form, wxID_ANY,
                        _L("Pick a side face, then set the draft angle. The face pivots about the body base.")),
                     0, wxLEFT | wxRIGHT, 12);
    m_box_draft->Add(drform, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    root->Add(m_box_draft, 0, wxEXPAND);

    // --- Docked value-entry card (Onshape Button->Dialog->Confirm for dimensions) ---
    m_box_value = new wxBoxSizer(wxVERTICAL);
    {
        // Header title doubles as the operation label (set by request_value()).
        m_box_value->Add(card_header("design_constrain", _L("Value"), m_value_label), 0, wxLEFT | wxRIGHT | wxTOP, 12);
        m_box_value->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
        auto* vrow = new wxBoxSizer(wxHORIZONTAL);
        // wxTE_PROCESS_ENTER so the user can just type a value and press Enter to
        // apply it (the natural CAD-dimension gesture), not only click Confirm.
        // Plain text field (not a spin control): on wxGTK the native GtkSpinButton
        // formats per the user locale (comma) with no clean override, so we own the
        // formatting here to guarantee international '.' decimals.
        m_value_input = new wxTextCtrl(m_form, wxID_ANY, "", wxDefaultPosition,
                                       wxSize(90, -1), wxTE_PROCESS_ENTER);
        m_value_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { confirm_value(); });
        vrow->Add(new wxStaticText(m_form, wxID_ANY, _L("Value")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        vrow->Add(m_value_input, 0, wxALIGN_CENTER_VERTICAL);
        m_box_value->Add(vrow, 0, wxLEFT | wxRIGHT | wxTOP, 12);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* ok  = new wxButton(m_form, wxID_ANY, _L("✓ Confirm"));
        ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { confirm_value(); });
        auto* no  = new wxButton(m_form, wxID_ANY, _L("✗ Cancel"));
        no->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_value(); });
        row->Add(ok, 0, wxRIGHT, 8);
        row->Add(no, 0);
        m_box_value->Add(row, 0, wxALL, 12);
    }
    root->Add(m_box_value, 0, wxEXPAND);

    // --- Sketch-entry card (Phase 3): plane/orientation, opens on "New sketch",
    //     persists until Finish. The toolbar holds only the drawing tools. ---
    m_box_sketch_session = new wxBoxSizer(wxVERTICAL);
    m_box_sketch_session->Add(card_header("design_sketch", _L("Sketch"), m_hdr_sketch_session),
                              0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_sketch_session->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    {
        auto* prow = new wxBoxSizer(wxHORIZONTAL);
        prow->Add(new wxStaticText(m_form, wxID_ANY, _L("Plane")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_draw_plane = new wxChoice(m_form, wxID_ANY);
        m_draw_plane->Append("XY"); m_draw_plane->Append("XZ"); m_draw_plane->Append("YZ");
        m_draw_plane->SetSelection(0);
        prow->Add(m_draw_plane, 0, wxALIGN_CENTER_VERTICAL);
        m_box_sketch_session->Add(prow, 0, wxLEFT | wxRIGHT | wxTOP, 12);
        auto* hint = new wxStaticText(m_form, wxID_ANY,
            _L("Pick a plane, then draw. Finish (✓) when done."));
        hint->SetForegroundColour(dp_sec_text());
        m_box_sketch_session->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 12);
    }
    root->Add(m_box_sketch_session, 0, wxEXPAND);

    // --- Constraint-manager card (C3.4): list of the constrained sketch's
    //     entity-constraints; each row selects (highlights) + deletes. Shown only
    //     in Constrain mode; rebuilt by rebuild_constraint_list().
    m_box_constraints = new wxBoxSizer(wxVERTICAL);
    m_box_constraints->Add(card_header("design_constrain", _L("Constraints"), m_hdr_constraints),
                           0, wxLEFT | wxRIGHT | wxTOP, 12);
    m_box_constraints->Add(new wxStaticLine(m_form), 0, wxEXPAND | wxALL, 8);
    m_constraint_rows = new wxBoxSizer(wxVERTICAL);
    m_box_constraints->Add(m_constraint_rows, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    root->Add(m_box_constraints, 0, wxEXPAND);

    root->Add(new wxStaticText(m_form, wxID_ANY, _L("Feature tree")), 0, wxLEFT | wxTOP, 12);
    m_tree = new wxTreeCtrl(m_form, wxID_ANY, wxDefaultPosition, wxSize(-1, 140),
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
    m_tree_images->Add(create_scaled_bitmap("design_dressup", nullptr, 16)); // 5 Shell
    m_tree->AssignImageList(m_tree_images);
    root->Add(m_tree, 0, wxEXPAND | wxALL, 12);

    // Selecting a body-producing feature (Extrude/Fillet/Chamfer/Hole/Thread) in the
    // tree highlights the solid in the viewport; a Sketch row clears the highlight
    // (its face is already shown via the persistent sketch overlay).
    m_tree->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent&) {
        if (!m_viewport) return;
        // A Parts-list body row: highlight that body and make it the op target.
        const int bsel = tree_body_selection();
        if (bsel >= 0) {
            m_viewport->set_body_highlight(false);   // the per-body overlay does the tint
            m_viewport->select_body(bsel);
            m_sel_solid_body = bsel;
            m_sel_solid_face = m_sel_solid_edge = -1;
            m_status->SetForegroundColour(wxNullColour);
            m_status->SetLabel(wxString::Format(_L("Body %d selected — next Extrude / Fillet acts on it"), bsel + 1));
            m_status->Refresh();
            return;
        }
        const int sel = tree_selection();
        const bool body = (sel >= 0 && sel < int(m_doc.features.size()) &&
                           m_doc.features[sel].type != CadFeatureType::Sketch &&
                           !m_doc.body.IsNull());
        m_viewport->set_body_highlight(body);
    });

    // Feature-tree edit row: act on the selected feature (delete / reorder).
    {
        auto* trow = new wxBoxSizer(wxHORIZONTAL);
        auto edit_btn = [this](const char* icon, const wxString& tip) {
            // Enlarged to match the main ribbon's weight (largest that fits 6
            // across the ~264px form column).
            auto* b = new ScalableButton(m_form, wxID_ANY, icon, "", wxSize(36, 36),
                                         wxDefaultPosition, wxBU_EXACTFIT | wxBORDER_NONE, false, 30);
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
                m_status->SetLabel(_L("Select a body to move it, or an imported Text/SVG to scale"));
                m_status->Refresh();
            }
        });
        auto* vis = edit_btn("design_eye", _L("Show / hide"));
        vis->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_toggle_visibility(); });
        auto* del  = edit_btn("design_delete", _L("Delete"));
        del->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_delete_feature(); });
        auto* up   = edit_btn("design_moveup", _L("Move up"));
        up->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_move_feature(-1); });
        auto* down = edit_btn("design_movedown", _L("Move down"));
        down->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_move_feature(+1); });
        trow->Add(edit, 0, wxRIGHT, 4);
        trow->Add(move, 0, wxRIGHT, 4);
        trow->Add(vis,  0, wxRIGHT, 4);
        trow->Add(del,  0, wxRIGHT, 4);
        trow->Add(up,   0, wxRIGHT, 4);
        trow->Add(down, 0);
        root->Add(trow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    }

    // Prepare's "Place on Face (F)" for the selected body: pick a face, lay it flat on the bed.
    auto* place = new wxButton(m_form, wxID_ANY, _L("Place on Face (F)"));
    place->SetToolTip(_L("Select a body face (click a solid, click again to a face), then lay that face on the bed"));
    place->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { place_on_face(); });
    root->Add(place, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    m_status = new wxStaticText(m_form, wxID_ANY, "");
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

    auto* commit = new wxButton(m_form, wxID_ANY, _L("Commit to Plate"));
    commit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_commit(); });
    root->Add(commit, 0, wxALL, 12);

    m_shape->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e) { on_shape_changed(); e.Skip(); });
    on_shape_changed();

    // Any parameter edit refreshes the translucent preview. Command events from the
    // spin/choice/checkbox children propagate up to m_form, so one binding each suffices.
    m_form->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent& e) { refresh_preview(); e.Skip(); });
    m_form->Bind(wxEVT_CHOICE,         [this](wxCommandEvent& e)    { refresh_preview(); e.Skip(); });
    m_form->Bind(wxEVT_CHECKBOX,       [this](wxCommandEvent& e)    { refresh_preview(); e.Skip(); });

    m_form->SetSizer(root);

    // Start with every tool dialog hidden (only the toolbar + tree + Commit show).
    root->Show(m_box_sketch,  false, true);
    root->Show(m_box_extrude, false, true);
    root->Show(m_box_revolve, false, true);
    root->Show(m_box_sweep, false, true);
    root->Show(m_box_pattern, false, true);
    root->Show(m_box_plane, false, true);
    root->Show(m_box_loft, false, true);
    root->Show(m_box_draft, false, true);
    root->Show(m_box_boolean, false, true);
    root->Show(m_box_cut,     false, true);
    root->Show(m_box_insert,  false, true);
    root->Show(m_box_dressup, false, true);
    root->Show(m_box_hole,    false, true);
    root->Show(m_box_thread,  false, true);
    root->Show(m_box_shell,   false, true);
    root->Show(m_box_value,   false, true);
    root->Show(m_box_sketch_session, false, true);
    root->Show(m_box_constraints,    false, true);

    m_form->FitInside();
    m_form->SetScrollRate(10, 10);
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
        m_status->SetLabel(_L("Sketch created — select it and Extrude"));
        refresh_tree();
    });

    m_viewport->set_on_sketch_entities_commit(
        [this](const std::vector<SketchEntity>& ents,
               const std::vector<SketchEntityConstraintDef>& cons,
               const SketchPlane& plane) {
            if (ents.empty()) {
                m_status->SetForegroundColour(wxColour(235, 110, 110));
                m_status->SetLabel(_L("Sketch empty — nothing committed"));
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
                    m_status->SetLabel(_L("Sketch updated"));
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
            m_status->SetLabel(cons.empty()
                ? _L("Sketch created — select it and Extrude")
                : wxString::Format(_L("Sketch created (%zu driving dims) — select it and Extrude"),
                                   cons.size()));
            refresh_tree();
            sync_sketch_display();   // keep the just-committed sketch visible as a face
        });

    // Live length/angle readout while drawing a Line/Polyline segment.
    m_viewport->set_on_cursor_metrics([this](double len, double ang_deg, bool locked) {
        double a = ang_deg; if (a < 0.0) a += 360.0;   // show bearing 0..360
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(wxString::Format(L"L %.2f mm   %.1f°%s",
                                            len, a, locked ? L"  (locked)" : L""));
        m_status->Refresh();
    });

    // DoF feedback (P3): after each live solve, report constraint state on its own
    // line. Green = fully constrained, red = conflicting, neutral = N remaining DoF.
    m_viewport->set_on_solve_state([this](int dof, bool ok, bool has_constraints) {
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
        m_dof_status->Refresh();
        m_form->Layout();
    });

    // Selection (Select tool): reflect the count in the status line.
    m_viewport->set_on_sketch_selection_changed([this](int count) {
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(count > 0
            ? wxString::Format(_L("%d selected — Delete removes them"), count)
            : _L("Click to select; click a filled face to extrude; Shift to add"));
        m_status->Refresh();
    });

    // Onshape flow: clicking inside a closed-loop face commits the sketch and opens
    // the Extrude dialog (with a ghost preview) targeting that sketch.
    m_viewport->set_on_sketch_face_selected([this]() {
        if (!m_viewport) return;
        m_viewport->finish_sketch();                 // commit live sketch (synchronous)
        m_extrude_sketch_ref = resolve_extrude_sketch();
        if (m_extrude_sketch_ref < 0) {
            m_status->SetForegroundColour(wxColour(235, 110, 110));
            m_status->SetLabel(_L("Could not resolve the sketch to extrude"));
            m_status->Refresh();
            return;
        }
        set_ui_mode(UiMode::Feature);
        open_tool(Tool::Extrude);
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(_L("Face selected — set the depth and Confirm"));
        m_status->Refresh();
    });

    // Clicking a committed sketch loop on the plate (no live session) selects THAT loop:
    // the viewport highlights only it (cyan) and its Sketch feature's tree row is selected.
    // The (feature, region) pair is remembered so Extrude builds just that one loop.
    m_viewport->set_on_display_sketch_selected([this](int feat, int region) {
        if (feat < 0 || feat >= int(m_doc.features.size())) return;
        m_sel_sketch_feat   = feat;
        m_sel_sketch_region = region;
        // Last pick wins (symmetric with the solid-pick handler): selecting a sketch loop drops
        // any stale solid face/edge pick so Extrude treats this loop as the profile.
        m_sel_solid_face = m_sel_solid_edge = -1;
        set_tree_selection(feat);
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(region >= 0
            ? _L("Loop selected — Extrude it, or Edit / Delete the sketch")
            : _L("Sketch selected — Extrude it, or Edit / Delete from the tree"));
        m_status->Refresh();
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
        m_sel_solid_face = (level >= 2) ? face : -1;
        m_sel_solid_edge = (level == 3) ? edge : -1;
        // Last pick wins: selecting a solid drops any stale committed-sketch loop selection.
        // Otherwise a leftover loop keeps `m_sel_sketch_region >= 0`, which blocks the face
        // push/pull branch in Extrude (`m_sel_solid_face >= 0 && m_sel_sketch_region < 0`) and
        // makes Extrude build a DETACHED new body from the last sketch instead of push/pulling
        // the face the user just clicked.
        if (level >= 1) { m_sel_sketch_region = -1; m_sel_sketch_feat = -1; }
        // If the Fillet/Chamfer card is open, re-anchor (or drop) the radius arrow on the new pick
        // and rebuild the ghost — once an edge is picked the preview-only mode hides the base body.
        if (m_active == Tool::Dressup) { update_fillet_gizmo(); refresh_preview(); }
        // If the Shell card is open, a face pick chooses the open face: update the label + gizmo
        // + ghost so the hollow updates live.
        if (m_active == Tool::Shell) {
            m_shell_face_label->SetLabel(m_sel_solid_face >= 0
                ? wxString::Format(_L("Face %d"), m_sel_solid_face)
                : _L("(all faces — closed hollow)"));
            refresh_preview();   // rebuilds the shell ghost + re-anchors the thickness gizmo
        }
        // Draft card open: a face pick chooses the face to taper; update label + ghost live.
        if (m_active == Tool::Draft) {
            m_draft_face_label->SetLabel(m_sel_solid_face >= 0
                ? wxString::Format(_L("Face %d"), m_sel_solid_face)
                : _L("(pick a side face)"));
            refresh_preview();
        }
        m_status->SetForegroundColour(wxNullColour);
        const int nb = int(m_doc.bodies.size());
        const wxString bodytag = (nb > 1) ? wxString::Format(_L("Body %d "), body + 1) : wxString();
        m_status->SetLabel(level == 1 ? bodytag + _L("selected (whole) — click again for a face")
                         : level == 2 ? bodytag + wxString::Format(_L("face %d selected — Extrude to push/pull it, or click again for an edge"), face)
                         : level == 3 ? bodytag + wxString::Format(_L("edge %d selected — open Fillet/Chamfer to dress it, or click again to reset"), edge)
                                      : _L("Nothing selected"));
        m_status->Refresh();
    });

    // Visual Extrude gizmo (C5b): dragging/editing the in-canvas depth arrow writes the
    // matching spin field and re-previews (which re-feeds the gizmo with the new depth).
    m_viewport->set_on_extrude_depth_changed([this](double depth, bool second) {
        if (second) { if (m_distance2) m_distance2->SetValue(depth); }
        else        { if (m_distance)  m_distance->SetValue(depth); }
        refresh_preview();
    });

    // Move-body gizmo (M5): each drag/edit reports the body's new translation. Store it as a
    // display-only per-body transform and re-feed the moved meshes (the OCCT shape is untouched,
    // so face/edge ids the dress-up ops target stay valid).
    m_viewport->set_on_body_move_changed([this](int body, const Transform3d& xform) {
        sync_body_xform();
        if (body < 0 || body >= int(m_body_xform.size())) return;
        m_body_xform[body] = xform;   // full move+rotate transform, baked into the mesh at Commit
        feed_bodies();   // rebuilds the transformed meshes in place + refreshes display + pick
        const int nb = int(m_doc.bodies.size());
        const wxString tag = (nb > 1) ? wxString::Format(_L("Body %d "), body + 1) : wxString();
        const Vec3d t = xform.translation();
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(tag + wxString::Format(_L("placed (%.1f, %.1f, %.1f) mm — drag arrows to move, rings to rotate"),
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
        if (m_thread_radius) m_thread_radius->SetValue(radius);
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
        m_status->SetLabel(_L("Tool exited"));
        m_status->Refresh();
    });

    // Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y) from the viewport → feature-history undo/redo.
    m_viewport->set_on_undo_redo([this](bool redo) { do_undo_redo(redo); });

    // Esc = the unified Cancel everywhere. Feature cards had no key exit (only the button);
    // CHAR_HOOK on the panel catches Esc from the card or viewport and routes to tool_cancel.
    // Sketch/Constrain keep the viewport's per-gesture Esc (abort the current point first),
    // so we only intercept Esc here when a feature/insert card is the thing to dismiss.
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        const bool dismissable = m_active != Tool::None || (m_viewport && m_viewport->moving_body());
        if (e.GetKeyCode() == WXK_ESCAPE && dismissable) { tool_cancel(); return; }
        e.Skip();
    });

    // Right-click finishes the move gizmo in the viewport; mirror that on the panel so the
    // action bar (shown while moving) hides and the move state clears.
    m_viewport->set_on_move_exit([this]() { m_move_body = -1; update_action_bar(); });

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

void DesignPanel::set_ui_mode(UiMode m)
{
    m_ui_mode = m;
    wxSizer* s = m_toolbar->GetSizer();
    s->Show(m_tb_feature,   m == UiMode::Feature,   true);
    s->Show(m_tb_sketch,    m == UiMode::Sketch,    true);
    s->Show(m_tb_constrain, m == UiMode::Constrain, true);
    m_toolbar->Layout();
    m_toolbar->FitInside();   // refresh the horizontal scroll range for the new group widths
    set_active_tool_btn(nullptr);   // no tool selected right after a mode switch
    // Phase 3: the docked Sketch card (plane/orientation) shows for the whole Sketch
    // session and hides on Finish/Constrain.
    if (m_box_sketch_session != nullptr && m_form != nullptr && m_form->GetSizer() != nullptr) {
        if (m == UiMode::Sketch && m_hdr_sketch_session != nullptr)
            m_hdr_sketch_session->SetLabel(wxString::Format(_L("Sketch %d"), m_feature_counter + 1));
        m_form->GetSizer()->Show(m_box_sketch_session, m == UiMode::Sketch, true);
        m_form->Layout();
        m_form->FitInside();
    }
    // Constraint-manager card follows Constrain mode; rebuilt from the active feature.
    if (m_box_constraints != nullptr && m_form != nullptr && m_form->GetSizer() != nullptr) {
        if (m == UiMode::Constrain)
            rebuild_constraint_list();
        else
            m_form->GetSizer()->Show(m_box_constraints, false, true);
        m_form->Layout();
        m_form->FitInside();
    }
    update_action_bar();   // Sketch/Constrain modes show the unified ✓/✗; Feature idle hides it
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
    m_status->SetLabel(wxString::Format(_L("OK — %zu triangles"),
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
                for (const std::vector<int>& loop : m_viewport->region_entity_indices(f.entities)) {
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
    const std::vector<TopoDS_Shape> solids = GeometryEngine::read_step_solids(path, err);
    if (solids.empty()) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(err.empty() ? _L("No solids found in STEP")
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
    if (!m_doc.recompute()) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(_L("STEP import failed: ") + wxString::FromUTF8(m_doc.error));
        m_status->Refresh();
        return;
    }
    set_ui_mode(UiMode::Feature);   // imported solids live in the feature timeline
    refresh_tree();
    set_tree_selection(int(m_doc.features.size()) - 1);
    set_status_ok();                // canonical post-recompute viewport/pick/parts refresh
    m_status->SetForegroundColour(wxNullColour);
    m_status->SetLabel(wxString::Format(
        _L("Imported %d solid(s) — pick a face or edge, then Fillet / Cut / Shell to modify"),
        int(solids.size())));
    m_status->Refresh();
}

void DesignPanel::add_imported_sketch(
    const std::vector<std::vector<std::vector<Vec2d>>>& regions,
    const wxString& base_name)
{
    if (regions.empty()) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(_L("No importable geometry found"));
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
    if (!on_face)
        f.plane = m_draw_plane ? plane_from_choice(m_draw_plane->GetSelection())
                               : SketchPlane::XY();

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
    wxSizer* s = m_form->GetSizer();
    s->Show(m_box_insert, true, true);
    m_form->Layout();
    m_form->FitInside();
    update_action_bar();   // surface the unified ✓/✗
    m_status->SetForegroundColour(wxNullColour);
    m_status->SetLabel(base_name + _L(" — drag to place/size, then Confirm"));
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
    m_status->SetLabel(_L("Insert cancelled"));
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
    m_status->SetLabel(_L("Drag a corner to scale, the centre to move — right-click when done"));
    m_status->Refresh();
}

void DesignPanel::on_add_sketch()
{
    SketchShape shape = (m_shape->GetSelection() == 1) ? SketchShape::Circle
                                                        : SketchShape::Rectangle;
    SketchPlane plane = plane_from_choice(m_plane->GetSelection());
    m_feature_counter++;
    m_doc.add_sketch(shape, plane, m_width->GetValue(), m_height->GetValue(),
                     m_radius->GetValue(), "Sketch" + std::to_string(m_feature_counter));
    m_doc.recompute();  // a lone sketch yields an empty body; that is expected
    m_status->SetForegroundColour(wxNullColour);
    m_status->SetLabel(_L("Sketch added — select it and Extrude"));
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
    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_dressup()
{
    if (m_doc.body.IsNull()) {
        m_status->SetLabel(_L("Add a solid (sketch + extrude) first"));
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

    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

SketchPlane DesignPanel::hole_plane() const
{
    return m_hole_on_face ? m_hole_face_plane
                          : plane_from_index(m_hole_plane->GetSelection());
}

void DesignPanel::on_add_hole()
{
    if (m_doc.body.IsNull()) {
        m_status->SetLabel(_L("Add a solid (sketch + extrude) first"));
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

    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

SketchPlane DesignPanel::thread_plane() const
{
    return m_thread_on_face ? m_thread_face_plane
                            : plane_from_index(m_thread_plane->GetSelection());
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

    // Nominal radius: external rod = major radius; internal tapped bore = minor
    // (tap-drill) radius. On a picked cylindrical face the radius comes from the
    // real geometry, so don't override it there.
    if (!m_thread_on_face && m_thread_radius) {
        const bool internal = m_thread_internal && m_thread_internal->GetValue();
        const double d = internal ? s->minor_diameter_mm() : s->major_diameter_mm;
        m_thread_radius->SetValue(0.5 * d);
    }

    if (m_status)
        m_status->SetLabel(wxString::Format(_L("Thread standard: %s  (pitch %.3g mm)"),
                                            m_thread_std->GetString(sel), s->pitch_mm));
}

void DesignPanel::on_add_thread()
{
    bool internal = m_thread_internal->GetValue();
    if (internal && m_doc.body.IsNull()) {
        m_status->SetLabel(_L("Internal thread needs a body — add a solid first"));
        return;
    }
    SketchPlane plane = thread_plane();

    m_feature_counter++;
    const int tidx = m_doc.add_thread(m_thread_radius->GetValue(), m_thread_pitch->GetValue(),
                     m_thread_height->GetValue(), m_thread_depth->GetValue(),
                     internal, m_thread_x->GetValue(), m_thread_y->GetValue(),
                     plane, "Thread" + std::to_string(m_feature_counter));
    // On-surface internal thread taps the body the cylindrical face belongs to.
    if (m_thread_on_face && tidx >= 0 && tidx < int(m_doc.features.size()))
        m_doc.features[tidx].target_body = m_thread_face_body;

    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

void DesignPanel::on_add_revolve()
{
    if (m_revolve_sketch_ref < 0 || m_revolve_sketch_ref >= int(m_doc.features.size())) {
        m_status->SetLabel(_L("Pick a sketch profile to revolve first"));
        return;
    }
    const BooleanMode mode = static_cast<BooleanMode>(m_revolve_mode->GetSelection());
    if (mode != BooleanMode::New && m_doc.body.IsNull()) {
        m_status->SetLabel(_L("Add/Cut/Intersect revolve needs an existing body"));
        return;
    }
    m_feature_counter++;
    m_doc.add_revolve(m_revolve_sketch_ref, m_revolve_angle->GetValue(),
                      m_revolve_axis->GetSelection(), m_revolve_flip->GetValue(),
                      mode, "Revolve" + std::to_string(m_feature_counter));

    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

void DesignPanel::on_add_sweep()
{
    if (m_sweep_profile_ref < 0 || m_sweep_profile_ref >= int(m_doc.features.size())) {
        m_status->SetLabel(_L("Pick a profile sketch to sweep first"));
        return;
    }
    const int sel = m_sweep_path->GetSelection();
    const int path_ref = (sel != wxNOT_FOUND)
        ? int(reinterpret_cast<intptr_t>(m_sweep_path->GetClientData(sel))) : -1;
    if (path_ref < 0) {
        m_status->SetLabel(_L("Pick a path sketch for the sweep"));
        return;
    }
    const BooleanMode mode = static_cast<BooleanMode>(m_sweep_mode->GetSelection());
    if (mode != BooleanMode::New && m_doc.body.IsNull()) {
        m_status->SetLabel(_L("Add/Cut/Intersect sweep needs an existing body"));
        return;
    }
    m_feature_counter++;
    m_doc.add_sweep(m_sweep_profile_ref, path_ref, mode,
                    "Sweep" + std::to_string(m_feature_counter));

    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
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
        m_status->SetLabel(_L("Check at least two profile sketches to loft"));
        return;
    }
    const BooleanMode mode = static_cast<BooleanMode>(m_loft_mode->GetSelection());
    if (mode != BooleanMode::New && m_doc.body.IsNull()) {
        m_status->SetLabel(_L("Add/Cut/Intersect loft needs an existing body"));
        return;
    }
    m_feature_counter++;
    m_doc.add_loft(refs, m_loft_ruled->GetValue(), mode,
                   "Loft" + std::to_string(m_feature_counter));

    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

void DesignPanel::on_add_pattern()
{
    if (m_doc.bodies.empty()) {
        m_status->SetLabel(_L("Pattern needs a body — add a solid first"));
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

    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

void DesignPanel::populate_body_choices()
{
    auto fill = [&](wxChoice* c, int def) {
        if (!c) return;
        c->Clear();
        for (size_t i = 0; i < m_doc.bodies.size(); ++i) {
            const std::string& n = m_doc.bodies[i].name;
            c->Append(n.empty() ? wxString::Format(_L("Body %zu"), i + 1) : wxString::FromUTF8(n));
        }
        if (c->GetCount() > 0)
            c->SetSelection(std::min(def, int(c->GetCount()) - 1));   // selection index == body index
    };
    fill(m_bool_target, 0);
    fill(m_bool_tool, 1);   // default: combine body 0 (target) with body 1 (tool)
    fill(m_cut_target, 0);  // Cut tool: default to the first body
}

void DesignPanel::on_add_boolean()
{
    if (m_doc.bodies.size() < 2) {
        m_status->SetLabel(_L("Boolean needs two bodies"));
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
    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::on_add_cut()
{
    if (m_doc.bodies.empty()) {
        m_status->SetLabel(_L("Cut needs a body"));
        return;
    }
    m_feature_counter++;
    m_doc.add_cut(plane_from_choice(m_cut_plane->GetSelection()), m_cut_offset->GetValue(),
                  /*flip*/ false, /*keep_upper*/ true, /*keep_lower*/ true,
                  m_cut_target->GetSelection(), "Cut" + std::to_string(m_feature_counter));
    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();
    refresh_tree();
}

void DesignPanel::populate_plane_choices(wxChoice* c) const
{
    if (!c) return;
    const int keep = c->GetSelection();
    c->Clear();
    c->Append("XY"); c->Append("XZ"); c->Append("YZ");
    for (const auto& dp : m_doc.resolve_datum_planes())
        c->Append(wxString::FromUTF8(dp.first));
    c->SetSelection((keep >= 0 && keep < int(c->GetCount())) ? keep : 0);
}

SketchPlane DesignPanel::plane_from_choice(int row) const
{
    if (row < 3) return plane_from_index(row);   // 0=XY,1=XZ,2=YZ
    auto datums = m_doc.resolve_datum_planes();
    const int di = row - 3;
    return (di >= 0 && di < int(datums.size())) ? datums[di].second : SketchPlane::XY();
}

void DesignPanel::on_add_plane()
{
    m_feature_counter++;
    m_doc.add_plane(m_plane_base->GetSelection(), m_plane_offset->GetValue(),
                    m_plane_tilt->GetValue(), m_plane_tilt_axis->GetSelection(),
                    "Plane" + std::to_string(m_feature_counter));
    m_doc.recompute();   // datum-only docs yield no body; that is expected/benign
    m_status->SetForegroundColour(wxNullColour);
    m_status->SetLabel(_L("Plane added — pick it as a sketch plane"));
    refresh_tree();
}

void DesignPanel::on_add_shell()
{
    if (m_doc.body.IsNull()) {
        m_status->SetLabel(_L("Shell needs a body — add a solid first"));
        return;
    }
    const int face = (m_sel_solid_face >= 0) ? m_sel_solid_face : -1;

    m_feature_counter++;
    m_doc.add_shell(m_shell_thickness->GetValue(), face, m_sel_solid_body,
                    "Shell" + std::to_string(m_feature_counter));

    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
    else
        set_status_ok();

    refresh_tree();
}

void DesignPanel::on_add_draft()
{
    if (m_doc.body.IsNull()) {
        m_status->SetLabel(_L("Draft needs a body — add a solid first"));
        return;
    }
    if (m_sel_solid_face < 0) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(_L("Draft needs a picked face — click a side face first"));
        m_status->Refresh();
        return;
    }

    m_feature_counter++;
    m_doc.add_draft(m_draft_angle->GetValue(), m_sel_solid_face, m_sel_solid_body,
                    "Draft" + std::to_string(m_feature_counter));

    if (!m_doc.recompute())
        m_status->SetLabel(_L("Recompute error: ") + wxString::FromUTF8(m_doc.error));
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
    }
    return 0;
}

void DesignPanel::on_tab_shown()
{
    if (m_viewport) m_viewport->refresh_bed();
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
    if (m_viewport) {
        std::vector<SketchPlane> dplanes;
        for (const auto& dp : m_doc.resolve_datum_planes()) dplanes.push_back(dp.second);
        m_viewport->set_datum_planes(std::move(dplanes));
    }
    for (const auto& f : m_doc.features) {
        const int img = tree_icon_for(f.type);
        wxTreeItemId id = m_tree->AppendItem(root, wxString::FromUTF8(f.name), img, img);
        // Hidden (disabled) features are greyed so the show/hide state reads at a glance.
        m_tree->SetItemTextColour(id, f.enabled ? dp_item_text()
                                                : dp_item_dim());
        m_tree_items.push_back(id);
    }
    // Parts list: a Bodies group listing each independent solid. Shown only with >1 body
    // (a single body is just "the solid"); selecting a row highlights it + targets it.
    if (m_doc.bodies.size() > 1) {
        sync_body_visible();   // keep flags parallel before reading them for the row colour
        wxTreeItemId grp = m_tree->AppendItem(root, _L("Bodies"));
        m_tree->SetItemTextColour(grp, dp_sec_text());
        for (size_t b = 0; b < m_doc.bodies.size(); ++b) {
            // Label "Body N" (matches the viewport/status); the originating feature name is
            // kept on the CadBody for tooltips/debug but isn't shown as the row label.
            const bool vis = b >= m_body_visible.size() || m_body_visible[b];
            wxTreeItemId id = m_tree->AppendItem(grp, wxString::Format(_L("Body %zu"), b + 1));
            // Hidden bodies are greyed so the show/hide state reads at a glance (eye toggle).
            m_tree->SetItemTextColour(id, vis ? dp_item_text() : dp_item_dim());
            m_tree_body_items.push_back(id);
        }
        m_tree->Expand(grp);
    }
    if (keep >= 0 && keep < int(m_tree_items.size()))
        m_tree->SelectItem(m_tree_items[keep]);
}

int DesignPanel::tree_body_selection() const
{
    const wxTreeItemId sel = m_tree->GetSelection();
    if (!sel.IsOk()) return -1;
    for (size_t i = 0; i < m_tree_body_items.size(); ++i)
        if (m_tree_body_items[i] == sel) return int(i);
    return -1;
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
    if (m_viewport == nullptr || b < 0 || b >= int(m_doc.display_body_meshes.size())) return;
    sync_body_xform();
    // Delta gizmo: pivot at the body's CURRENT world centroid; the tool composes the drag deltas
    // onto its current pose, so move + rotate both work (incl. on an already place-on-face'd body).
    const Transform3d base = m_body_xform[b];
    const Vec3d pivot = base * m_doc.display_body_meshes[b].bounding_box().center();
    m_viewport->begin_move_body(b, pivot, base);
    m_move_body = b;          // for the action bar: Cancel reverts to this pose
    m_move_prev = base;
    update_action_bar();      // surface the unified ✓/✗ while moving
    m_status->SetForegroundColour(wxNullColour);
    m_status->SetLabel(_L("Drag the arrows to move, the rings to rotate — then Confirm (Esc cancels)"));
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
        m_status->SetLabel(_L("Select a body first"));
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
    m_status->SetLabel(wxString::Format(_L("Body %d colour set"), b + 1));
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
        m_status->SetLabel(_L("Pick a body face first (click a solid, then click again to a face), then press F"));
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
    m_status->SetLabel(_L("Placed on face — body laid flat on the bed"));
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
    update_undo_redo_buttons();   // every commit/undo/redo funnels through here -> refresh greying
    refresh_tree();
    if (!ok) {
        // The edit was rolled back (recompute failed); the body is unchanged.
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(_L("Edit rejected: ") + wxString::FromUTF8(m_doc.error));
        m_status->Refresh();
        return;
    }
    m_status->SetForegroundColour(wxNullColour);
    if (m_doc.display_mesh.its.indices.empty()) {
        if (m_viewport != nullptr) m_viewport->clear_mesh();
        sync_sketch_display();   // empty body: show any un-consumed committed sketch
        m_status->SetLabel(wxString());
    } else {
        set_status_ok();
    }
    m_status->Refresh();
}

void DesignPanel::on_delete_feature()
{
    int sel = tree_selection();
    if (sel == wxNOT_FOUND) {
        m_status->SetLabel(_L("Select a feature in the tree first"));
        m_status->Refresh();
        return;
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
            if (bsel < int(m_tree_body_items.size()))   // keep the row selected for repeat toggles
                m_tree->SelectItem(m_tree_body_items[bsel]);
            m_status->SetForegroundColour(wxNullColour);
            m_status->SetLabel(wxString::Format(now_visible ? _L("Body %d shown")
                                                            : _L("Body %d hidden"), bsel + 1));
            m_status->Refresh();
        }
        return;
    }

    int sel = tree_selection();
    if (sel == wxNOT_FOUND || sel >= int(m_doc.features.size())) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(_L("Select a feature in the tree first"));
        m_status->Refresh();
        return;
    }
    const bool shown = !m_doc.features[sel].enabled;
    m_doc.features[sel].enabled = shown;

    // recompute() reports an all-hidden / sketch-only document as false (no
    // solid to build), but that is a VALID state for hide — so clear the body
    // explicitly instead of letting after_tree_edit treat it as a rejected edit
    // (which would skip the overlay refresh, leaving hidden art on screen).
    if (!m_doc.recompute()) {
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
    m_status->SetLabel(shown ? _L("Feature shown") : _L("Feature hidden"));
    m_status->Refresh();
}

void DesignPanel::on_move_feature(int delta)
{
    int sel = tree_selection();
    if (sel == wxNOT_FOUND) {
        m_status->SetLabel(_L("Select a feature in the tree first"));
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
        m_status->SetLabel(_L("Draw a sketch first, then Constrain"));
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
    if (sel == wxNOT_FOUND || sel >= int(m_doc.features.size())) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(_L("Select a sketch in the tree first"));
        m_status->Refresh();
        return;
    }
    CadFeature& f = m_doc.features[sel];
    if (f.type != CadFeatureType::Sketch) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(_L("Selected feature is not a sketch"));
        m_status->Refresh();
        return;
    }

    // Entity sketches (Fase 4.2): pick Line entities; constraints solve against
    // entity endpoints in the kernel.
    if (!f.entities.empty()) {
        m_constrain_feat = sel;
        if (m_viewport) m_viewport->begin_constrain_entities(f.entities, f.plane);
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(_L("Pick 1-2 lines, then a constraint; right-click exits"));
        m_status->Refresh();
        return;
    }

    // Legacy profile path (Fase 3).
    if (f.profile.points.size() < 3) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(_L("Selected feature is not a sketch"));
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
    m_status->SetLabel(_L("Pick 1-2 entities, then a constraint or dimension; right-click exits"));
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
        m_status->SetLabel(msg);
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
        m_status->SetLabel(_L("Constraint rejected (over-constrained)"));
        m_status->Refresh();
        return;
    }
    m_doc.recompute();
    m_viewport->update_constrain_entities(m_doc.features[m_constrain_feat].entities);
    if (!m_doc.display_mesh.its.indices.empty())
        feed_bodies();
    m_status->SetForegroundColour(wxNullColour);
    m_status->SetLabel(_L("Applied constraint"));
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
        m_dof_status->SetLabel(_L("✗ Conflicting constraints"));
    } else if (r.dof == 0) {
        m_dof_status->SetForegroundColour(wxColour(80, 200, 110));
        m_dof_status->SetLabel(_L("✓ Fully constrained"));
    } else if (r.dof > 0) {
        m_dof_status->SetForegroundColour(dp_ctl_text());
        m_dof_status->SetLabel(wxString::Format(_L("%d degrees of freedom"), r.dof));
    } else {
        m_dof_status->SetLabel(wxString());
    }
    m_dof_status->Refresh();
    m_form->Layout();
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
        auto* none = new wxStaticText(m_form, wxID_ANY, _L("No constraints yet"));
        none->SetForegroundColour(dp_sec_text());
        m_constraint_rows->Add(none, 0, wxTOP, 4);
    }
    for (int i = 0; i < int(cons.size()); ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        // Delete button first (fixed left position, always visible — long labels can
        // horizontally scroll but ✗ stays put and clickable). BMP-safe ✗ glyph.
        auto* del = new wxButton(m_form, wxID_ANY, wxString::FromUTF8("✗"),
                                 wxDefaultPosition, wxSize(26, -1));
        del->SetToolTip(_L("Delete constraint"));
        del->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { delete_constraint(i); });
        // Clickable label: selecting it highlights the referenced entities.
        auto* lbl = new wxButton(m_form, wxID_ANY, constraint_label(cons[i]),
                                 wxDefaultPosition, wxDefaultSize, wxBU_LEFT | wxBORDER_NONE);
        lbl->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { highlight_constraint_entities(i); });
        row->Add(del, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(lbl, 1, wxALIGN_CENTER_VERTICAL);
        m_constraint_rows->Add(row, 0, wxEXPAND | wxTOP, 2);
    }

    // Feed the same list to the viewport for the on-sketch glyph badges (C3.4b).
    if (m_viewport)
        m_viewport->set_constraint_glyphs(cons);

    m_form->GetSizer()->Show(m_box_constraints, m_ui_mode == UiMode::Constrain, true);
    m_form->Layout();
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
    m_status->SetLabel(_L("Constraint deleted"));
    m_status->Refresh();
    refresh_constrain_dof();
    rebuild_constraint_list();
}

void DesignPanel::apply_edit_op(EditOp op)
{
    if (m_constrain_feat < 0 || m_constrain_feat >= int(m_doc.features.size()) || !m_viewport ||
        !m_viewport->is_constraining_entities()) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(_L("Press Constrain on a sketch first"));
        m_status->Refresh();
        return;
    }
    auto fail = [this](const wxString& msg) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(msg);
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
                    m_status->SetLabel(_L("Offset collapsed the entity")); m_status->Refresh(); return;
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
                m_status->SetLabel(_L("Fillet failed (parallel lines or radius too large)"));
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
                m_status->SetLabel(_L("Chamfer failed (parallel lines or distance too large)"));
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
                            m_status->SetLabel(_L("Array produced nothing")); m_status->Refresh(); return;
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
                            m_status->SetLabel(_L("Move produced nothing")); m_status->Refresh(); return;
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
                    m_status->SetLabel(_L("Rotate produced nothing")); m_status->Refresh(); return;
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
                    m_status->SetLabel(_L("Scale produced nothing")); m_status->Refresh(); return;
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
                            m_status->SetLabel(_L("Polar array produced nothing")); m_status->Refresh(); return;
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
    m_status->SetLabel(_L("Applied edit"));
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
    m_form->GetSizer()->Show(m_box_value, true, true);
    m_form->Layout();
    m_form->FitInside();
    m_value_input->SetFocus();
    m_value_input->SetSelection(-1, -1);   // select all so typing replaces the value
    m_status->SetForegroundColour(wxNullColour);
    m_status->SetLabel(label + _L(" — type a value, press Enter (or Confirm)"));
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
    m_form->GetSizer()->Show(m_box_value, false, true);
    m_form->Layout();
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
        m_form->GetSizer()->Show(m_box_value, false, true);
    m_form->Layout();
    m_form->FitInside();
    if (was_open) {
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(wxString());
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
        m_status->SetLabel(_L("Press Constrain on a sketch first"));
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
        m_status->SetLabel(_L("Press Constrain on a sketch first"));
        m_status->Refresh();
        return;
    }
    int a = -1, b = -1;
    if (!m_viewport->selected_segment(a, b)) {
        m_status->SetForegroundColour(wxColour(235, 110, 110));
        m_status->SetLabel(_L("Pick a segment in the viewport first"));
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
        m_status->SetLabel(_L("Constraint rejected (over-constrained)"));
        m_status->Refresh();
        return;
    }
    m_doc.recompute();
    m_viewport->update_constrain_profile(m_doc.features[m_constrain_feat].profile.points);
    if (!m_doc.display_mesh.its.indices.empty())
        feed_bodies();
    m_status->SetForegroundColour(wxNullColour);
    m_status->SetLabel(type == SketchConstraintType::Horizontal ? _L("Applied Horizontal")
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
        m_plane->SetSelection(index_from_plane(f.plane));
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
        break;
    case CadFeatureType::Hole:
        m_hole_plane->SetSelection(index_from_plane(f.plane));
        m_hole_diameter->SetValue(f.hole_diameter);
        m_hole_depth->SetValue(f.hole_depth);
        m_hole_through->SetValue(f.hole_through);
        m_hole_x->SetValue(f.hole_x);
        m_hole_y->SetValue(f.hole_y);
        break;
    case CadFeatureType::Thread:
        m_thread_plane->SetSelection(index_from_plane(f.plane));
        m_thread_radius->SetValue(f.thread_radius);
        m_thread_pitch->SetValue(f.thread_pitch);
        m_thread_height->SetValue(f.thread_height);
        m_thread_depth->SetValue(f.thread_depth);
        m_thread_internal->SetValue(f.thread_internal);
        m_thread_x->SetValue(f.thread_x);
        m_thread_y->SetValue(f.thread_y);
        if (m_thread_std) m_thread_std->SetSelection(0);   // Custom: spins reflect the stored feature
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
    default: break;
    }
}

void DesignPanel::on_edit_feature()
{
    int sel = tree_selection();
    if (sel == wxNOT_FOUND) {
        m_status->SetLabel(_L("Select a feature in the tree first"));
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
            m_status->SetLabel(_L("Editing sketch — drag a handle or click a quote to edit"));
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
    default: break;
    }
}

void DesignPanel::on_commit()
{
    // A feature tool open with a live preview ghost (e.g. a fillet being previewed) is
    // NOT yet part of the body. Apply it first so "Commit to Plate" ships exactly what
    // is shown on screen, not the pre-feature solid. (confirm_tool() applies + closes.)
    if (m_active != Tool::None)
        confirm_tool();

    if (m_doc.display_mesh.its.indices.empty()) {
        m_status->SetLabel(_L("Nothing to commit — add a feature first"));
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
            m_status->SetLabel(_L("All bodies hidden — show one before committing"));
            return;
        }
    } else {
        obj_list->load_mesh_object(m_disp_pick_mesh, "Design Body");
    }

    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
}

CadFeature DesignPanel::build_candidate(Tool t) const
{
    CadFeature f;
    switch (t) {
    case Tool::Sketch:
        f.type   = CadFeatureType::Sketch;
        f.shape  = (m_shape->GetSelection() == 0) ? SketchShape::Rectangle : SketchShape::Circle;
        f.plane  = plane_from_choice(m_plane->GetSelection());
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
        f.up_to_face  = (f.extrude_end == ExtrudeEnd::UpToFace) ? m_sel_solid_face : -1;
        f.mode       = (m_mode->GetSelection() == 0) ? BooleanMode::New
                     : (m_mode->GetSelection() == 1) ? BooleanMode::Add
                     : (m_mode->GetSelection() == 2) ? BooleanMode::Cut
                                                     : BooleanMode::Intersect;
        if (m_extrude_face_src >= 0) {
            // Preview the face-as-profile extrude: the kernel grabs the body face by id.
            f.extrude_src_face = m_extrude_face_src;
            f.sketch_ref       = -1;
        } else if (extrude_uses_loop()) {
            // Preview just the click-selected loop: carry its entity subset on the
            // feature (sketch_ref = -1 -> build_sketch_wire uses f.entities).
            f.sketch_ref = -1;
            f.entities   = m_viewport->selected_loop_entities();
            f.plane      = m_doc.features[m_extrude_sketch_ref].plane;
        } else {
            f.sketch_ref = m_extrude_sketch_ref;
            // Preview an on-face engraving Cut against the host body (matches the commit).
            if (m_extrude_sketch_ref >= 0 && m_extrude_sketch_ref < int(m_doc.features.size())
                && m_doc.features[m_extrude_sketch_ref].import_on_face)
                f.target_body = m_doc.features[m_extrude_sketch_ref].import_face_body;
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
        f.thread_radius   = m_thread_radius->GetValue();
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
    case Tool::Insert:   // imported art is committed by add_imported_sketch, not build_candidate
    case Tool::None:
        break;
    }
    // Boolean drives its own target/tool body from the card; every other tool targets the
    // picked body (face-extrude reads its source face there, dress-up / hole / boolean-mode
    // extrude mutate it). -1 when nothing is picked => auto (last body).
    if (m_active != Tool::Boolean && m_active != Tool::Cut)
        f.target_body = m_sel_solid_body;
    return f;
}

// Resolve the active Extrude's profile plane + a representative 2D centroid (arrow anchor)
// and push them to the viewport gizmo. Self-gates: clears the gizmo unless Extrude is open.
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
                                   m_thread_radius->GetValue(), m_thread_height->GetValue());
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
    if (m_active != Tool::Extrude) { m_viewport->clear_extrude_gizmo(); return; }

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

void DesignPanel::refresh_preview()
{
    if (m_active == Tool::None) { m_viewport->clear_preview(); return; }

    if (m_active == Tool::Sketch || m_active == Tool::Plane) {
        // A sketch / datum plane carries no 3D solid; there is no ghost to show. Always
        // valid, so just enable Confirm and clear any stale ghost.
        m_viewport->clear_preview();
        m_status->SetForegroundColour(wxColour(120, 210, 120));
        m_status->SetLabel(m_active == Tool::Plane ? _L("Plane ready") : _L("Sketch ready"));
        for (wxButton* b : m_confirm_btns) if (b) b->Enable(true);
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
    auto ghost_from = [this](const std::vector<TriangleMesh>& pbm) -> TriangleMesh {
        TriangleMesh out;
        for (size_t b = 0; b < pbm.size(); ++b) {
            TriangleMesh m = pbm[b];
            if (b < m_body_xform.size()) m.transform(m_body_xform[b]);
            out.merge(m);
        }
        return out;
    };

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
        m_status->SetLabel(wxString::Format(_L("Preview — %zu triangles"), mesh.its.indices.size()));
    } else {
        m_viewport->clear_preview();
        m_status->SetForegroundColour(wxColour(235, 110, 110)); // invalid = red
        m_status->SetLabel(_L("Invalid: ") + wxString::FromUTF8(err));
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
    // Same for the Pattern spacing arrow / angle-arc (self-gates: only while the Pattern card is open).
    update_pattern_gizmo();
}

void DesignPanel::open_tool(Tool t)
{
    m_active = t;
    // Fillet/Chamfer/Draft no longer fade the body see-through; instead, once a valid target
    // is picked, refresh_preview hides the base bodies entirely (preview-only). Keep it opaque
    // here so the body is fully visible for picking the edge/face.
    if (m_viewport) { m_viewport->set_body_translucent(false); m_viewport->set_body_hidden(false); }
    wxSizer* s = m_form->GetSizer();
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
    s->Show(m_box_loft,    t == Tool::Loft,    true);
    s->Show(m_box_boolean, t == Tool::Boolean, true);
    s->Show(m_box_cut,     t == Tool::Cut,     true);
    s->Show(m_box_draft,   t == Tool::Draft,   true);
    s->Show(m_box_insert,  t == Tool::Insert,  true);

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
            const int pos = m_sweep_path->Append(wxString::FromUTF8(sf.name),
                                                 reinterpret_cast<void*>(intptr_t(i)));
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
    case Tool::Insert:  break;   // header set by open_insert_card()
    case Tool::None:    break;
    }

    m_form->Layout();
    m_form->FitInside();
    update_action_bar();   // a tool is now active -> show the unified ✓/✗
    refresh_preview();
}

void DesignPanel::close_tool()
{
    m_active = Tool::None;
    set_active_tool_btn(nullptr);   // clear the active-tool teal highlight
    if (m_viewport) { m_viewport->set_body_translucent(false); m_viewport->set_body_hidden(false); }   // restore the opaque solid
    wxSizer* s = m_form->GetSizer();
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
    s->Show(m_box_loft,    false, true);
    s->Show(m_box_boolean, false, true);
    s->Show(m_box_cut,     false, true);
    s->Show(m_box_draft,   false, true);
    s->Show(m_box_insert,  false, true);
    m_viewport->clear_preview();
    m_viewport->clear_extrude_gizmo();
    m_viewport->clear_fillet_gizmo();
    m_viewport->clear_hole_gizmo();
    m_viewport->clear_thread_gizmo();
    m_viewport->clear_shell_gizmo();
    m_viewport->clear_revolve_gizmo();
    m_viewport->clear_pattern_gizmo();
    m_form->Layout();
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
    case Tool::Plane:   on_add_plane();   break;
    case Tool::Loft:    on_add_loft();    break;
    case Tool::Draft:   on_add_draft();   break;
    case Tool::Boolean: on_add_boolean(); break;
    case Tool::Cut:     on_add_cut();     break;
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
    m_status->SetLabel(wxString());
    m_status->Refresh();
}

// One Confirm surface for the whole tab. Routes to the right commit by current context:
// a feature card, the Insert placement, the Sketch session, or the Constrain session.
void DesignPanel::tool_confirm()
{
    if (m_viewport && m_viewport->moving_body()) {   // keep the placement, drop the gizmo
        m_viewport->clear_move_gizmo();
        m_move_body = -1;
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
        m_status->SetLabel(wxString());
        m_status->Refresh();
    }
}

// One Cancel/exit surface (also bound to Esc). Discards the active feature/insert, or a
// drawn-but-uncommitted Sketch, or exits Constrain.
void DesignPanel::tool_cancel()
{
    if (m_viewport && m_viewport->moving_body()) {   // revert to the pose at move-start
        sync_body_xform();
        if (m_move_body >= 0 && m_move_body < int(m_body_xform.size()))
            m_body_xform[m_move_body] = m_move_prev;
        m_viewport->clear_move_gizmo();
        m_move_body = -1;
        feed_bodies();           // re-render the reverted placement
        update_action_bar();
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(_L("Move cancelled"));
        m_status->Refresh();
        return;
    }
    if (m_active == Tool::Insert) { cancel_insert(); return; }
    if (m_active != Tool::None)   { cancel_tool();   return; }
    if (m_ui_mode == UiMode::Sketch) {
        if (m_viewport) m_viewport->cancel_sketch();   // drop the live session (committed art stays)
        m_edit_index = -1;
        set_ui_mode(UiMode::Feature);
        sync_sketch_display();
        refresh_tree();
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(wxString());
        m_status->Refresh();
        return;
    }
    if (m_ui_mode == UiMode::Constrain) {
        cancel_value();
        if (m_viewport) m_viewport->end_constrain();
        m_constrain_feat = -1;
        set_ui_mode(UiMode::Feature);
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(wxString());
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
        m_status->SetLabel(_L("Finish or cancel the current tool first (Esc)"));
        m_status->Refresh();
        return;
    }
    const bool ok = redo ? m_doc.redo() : m_doc.undo();
    if (!ok) {
        m_status->SetForegroundColour(wxNullColour);
        m_status->SetLabel(redo ? _L("Nothing to redo") : _L("Nothing to undo"));
        m_status->Refresh();
        return;
    }
    // The solid whole/face/edge pick and any in-place edit reference ids that recompute()
    // invalidates — drop them before refreshing from the restored document.
    m_sel_solid_body = m_sel_solid_face = m_sel_solid_edge = -1;
    reset_edit_state();
    after_tree_edit(true);   // refresh tree + viewport meshes + status from the restored doc
    m_status->SetForegroundColour(wxNullColour);
    m_status->SetLabel(wxString::Format(redo ? _L("Redo  (%zu more)") : _L("Undo  (%zu more)"),
                                        redo ? m_doc.redo_depth() : m_doc.undo_depth()));
    m_status->Refresh();
}

}} // namespace Slic3r::GUI

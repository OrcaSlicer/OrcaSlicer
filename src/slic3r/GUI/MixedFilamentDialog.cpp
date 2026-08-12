#include "MixedFilamentDialog.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/scrolwin.h>
#include <wx/textctrl.h>
#include <wx/wrapsizer.h>
#include <wx/tokenzr.h>

#include "libslic3r/Utils.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "I18N.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GradientCurveEditor.hpp"
#include "wxExtensions.hpp"
#include "Tab.hpp"
#include "libslic3r/Preset.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DropDown.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r {
namespace GUI {

static constexpr int MAX_COMPONENTS = 3;
static constexpr int MIN_COMPONENT_RATIO = 10;

// Lightweight self-painting label used for both dual-color and triple-color
// ratio percentage display.  Hover shows a rounded-rect background; click
// fires wxEVT_LEFT_DOWN which the owning dialog binds to start_ratio_editor.
class RatioLabelPanel : public wxPanel
{
public:
    RatioLabelPanel(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(_L("Click to edit ratio"));
        SetFont(::Label::Body_10);

        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { m_hovered = true;  Refresh(); e.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { m_hovered = false; Refresh(); e.Skip(); });
        Bind(wxEVT_PAINT, &RatioLabelPanel::on_paint, this);
    }

    void SetLabel(const wxString& text) override
    {
        if (m_text == text) return;
        m_text = text;
        update_best_size();
        Refresh();
    }
    wxString GetLabel() const override { return m_text; }

private:
    void update_best_size()
    {
        wxClientDC dc(this);
        dc.SetFont(GetFont());
        wxSize ts = dc.GetTextExtent(m_text);
        int pad_x = FromDIP(4), pad_y = FromDIP(3);
        SetMinSize(wxSize(ts.GetWidth() + pad_x * 2, ts.GetHeight() + pad_y * 2));
        InvalidateBestSize();
    }

    void on_paint(wxPaintEvent&)
    {
        wxBufferedPaintDC dc(this);
        wxSize sz = GetClientSize();

        wxColour parent_bg = GetParent() ? GetParent()->GetBackgroundColour()
                                         : StateColor::darkModeColorFor(*wxWHITE);
        dc.SetBrush(wxBrush(parent_bg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

        if (m_hovered) {
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#F8F8F8"))));
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#CECECE")), 1));
            dc.DrawRoundedRectangle(0, 0, sz.GetWidth(), sz.GetHeight(), FromDIP(3));
        }

        dc.SetFont(GetFont());
        dc.SetTextForeground(m_hovered ? wxColour("#00AE42")
                                       : StateColor::darkModeColorFor(wxColour("#262E30")));
        wxSize ts = dc.GetTextExtent(m_text);
        int x = (sz.GetWidth()  - ts.GetWidth())  / 2;
        int y = (sz.GetHeight() - ts.GetHeight()) / 2;
        dc.DrawText(m_text, x, y);
    }

    wxString m_text;
    bool     m_hovered{false};
};

static wxColour blend_colors(const wxColour& a, const wxColour& b, double ratio_a)
{
    unsigned char r, g, bl;
    Slic3r::filament_mixer_lerp(a.Red(), a.Green(), a.Blue(),
                                b.Red(), b.Green(), b.Blue(),
                                static_cast<float>(1.0 - ratio_a),
                                &r, &g, &bl);
    return wxColour(r, g, bl);
}

static wxColour blend_n_colors(const std::vector<wxColour>& cols, const std::vector<double>& weights)
{
    std::vector<std::string> hex_colors;
    std::vector<int> int_weights;
    for (size_t i = 0; i < cols.size() && i < weights.size(); ++i) {
        hex_colors.push_back(cols[i].GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
        // Scale double weights (e.g. 0.5) to int (5000) for blend_color_multi;
        // only relative magnitude matters.
        int_weights.push_back(static_cast<int>(std::lround(weights[i] * 10000)));
    }
    std::string hex = Slic3r::blend_color_multi(hex_colors, int_weights);
    return wxColour(hex);
}

// ---- Constructors ----

MixedFilamentDialog::MixedFilamentDialog(wxWindow* parent,
                                         const std::vector<std::string>& physical_colors,
                                         const std::vector<std::string>& physical_names,
                                         const std::vector<std::string>& physical_types)
    : DPIDialog(parent, wxID_ANY, _L("Add Mixed Filament"), wxDefaultPosition,
                wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_edit_mode(false)
    , m_physical_colors(physical_colors)
    , m_physical_names(physical_names)
    , m_physical_types(physical_types)
{
    m_result.components = {1, (physical_colors.size() >= 2) ? 2u : 1u};
    m_result.ratios     = {50, 50};
    build_ui();
    wxGetApp().UpdateDlgDarkUI(this);

    wxImage img;
    if (img.LoadFile(from_u8(Slic3r::var("mixed_filament_preview_twocolor.png")), wxBITMAP_TYPE_PNG))
        m_preview_bmp_two = wxBitmap(img);
    if (img.LoadFile(from_u8(Slic3r::var("mixed_filament_preview_threecolor.png")), wxBITMAP_TYPE_PNG))
        m_preview_bmp_three = wxBitmap(img);
}

MixedFilamentDialog::MixedFilamentDialog(wxWindow* parent,
                                         const MixedFilamentResult& existing,
                                         const std::vector<std::string>& physical_colors,
                                         const std::vector<std::string>& physical_names,
                                         const std::vector<std::string>& physical_types)
    : DPIDialog(parent, wxID_ANY, _L("Edit Mixed Filament"), wxDefaultPosition,
                wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_result(existing)
    , m_edit_mode(true)
    , m_physical_colors(physical_colors)
    , m_physical_names(physical_names)
    , m_physical_types(physical_types)
{
    if (m_result.components.size() < 2) {
        m_result.components = {1, (physical_colors.size() >= 2) ? 2u : 1u};
        m_result.ratios     = {50, 50};
    }
    if (m_result.ratios.size() >= 3) {
        int sum = 0;
        for (int r : m_result.ratios) sum += r;
        if (sum > 0) {
            m_tri_wx = (double)m_result.ratios[0] / sum;
            m_tri_wy = (double)m_result.ratios[1] / sum;
            m_tri_wz = (double)m_result.ratios[2] / sum;
        }
    }
    build_ui();
    wxGetApp().UpdateDlgDarkUI(this);

    wxImage img;
    if (img.LoadFile(from_u8(Slic3r::var("mixed_filament_preview_twocolor.png")), wxBITMAP_TYPE_PNG))
        m_preview_bmp_two = wxBitmap(img);
    if (img.LoadFile(from_u8(Slic3r::var("mixed_filament_preview_threecolor.png")), wxBITMAP_TYPE_PNG))
        m_preview_bmp_three = wxBitmap(img);
}

void MixedFilamentDialog::on_dpi_changed(const wxRect&)
{
    int h = (num_components() >= 3) ? FromDIP(680) : FromDIP(580);
    SetSize(FromDIP(439), h);
    Refresh();
}

wxColour MixedFilamentDialog::comp_colour(size_t i) const
{
    unsigned int c = comp(i);
    if (c >= 1 && c <= m_physical_colors.size())
        return wxColour(m_physical_colors[c - 1]);
    return wxColour("#D9D9D9");
}

static wxBitmap make_alpha_bitmap(int w, int h,
                                  const std::function<void(wxDC& dc)>& draw_fn)
{
    wxBitmap bmp(w, h);
    wxMemoryDC memdc;
#ifdef __WXOSX__
    bmp.UseAlpha();
    memdc.SelectObject(bmp);
#else
    {
        wxImage img(w, h);
        img.InitAlpha();
        memset(img.GetAlpha(), 0, w * h);
        bmp = wxBitmap(std::move(img));
    }
    memdc.SelectObject(bmp);
#endif
    {
#ifdef __WXMSW__
        wxGCDC dc(memdc);
#else
        wxDC& dc = memdc;
#endif
        draw_fn(dc);
    }
    memdc.SelectObject(wxNullBitmap);
    return bmp;
}

wxBitmap MixedFilamentDialog::make_swatch_bitmap(size_t idx)
{
    int swatch_sz = FromDIP(20);
    int pad_left  = FromDIP(2);
    int pad_right = FromDIP(6);
    int bmp_w = pad_left + swatch_sz + pad_right;
    int bmp_h = swatch_sz;

    // Reuse the sidebar clr_picker swatch (get_extruder_color_icon) so the
    // checkerboard (transparent.svg tiling), border and label style match the
    // sidebar exactly, instead of a self-drawn rounded rect / programmatic grid.
    std::string color_hex = "#D9D9D9";
    if (idx < m_physical_colors.size())
        color_hex = m_physical_colors[idx];
    std::string label = std::to_string(idx + 1);

    wxBitmap* icon = get_extruder_color_icon(color_hex, label, swatch_sz, swatch_sz);

    return make_alpha_bitmap(bmp_w, bmp_h, [&](wxDC& dc) {
        if (icon && icon->IsOk())
            dc.DrawBitmap(*icon, pad_left, 0);
    });
}

void MixedFilamentDialog::reset_manual_ratio_state()
{
    m_ratio_manual_order.clear();
    if (m_ratio_editor_panel)
        m_ratio_editor_panel->Hide();
    // Restore any label hidden by an in-flight editor so it can never be left
    // permanently invisible if the editor is dismissed without a commit.
    if (m_ratio_editor_anchor) {
        m_ratio_editor_anchor->Show();
        m_ratio_editor_anchor = nullptr;
    }
}

void MixedFilamentDialog::refresh_ratio_labels()
{
    if (m_label_ratio_a)
        m_label_ratio_a->SetLabel(wxString::Format(wxT("%d%%"), ratio(0)));
    if (m_label_ratio_b)
        m_label_ratio_b->SetLabel(wxString::Format(wxT("%d%%"), ratio(1)));
    if (m_ratio_sizer)
        m_ratio_sizer->Layout();
    if (m_triangle_panel)
        m_triangle_panel->Refresh();
}

void MixedFilamentDialog::sync_triangle_weights_from_ratios()
{
    if (m_result.ratios.size() < 3)
        return;

    int sum = 0;
    for (int r : m_result.ratios)
        sum += r;
    if (sum <= 0)
        return;

    m_tri_wx = (double)m_result.ratios[0] / sum;
    m_tri_wy = (double)m_result.ratios[1] / sum;
    m_tri_wz = (double)m_result.ratios[2] / sum;
}

void MixedFilamentDialog::apply_manual_ratio(size_t idx, int value)
{
    const size_t n = num_components();
    if (idx >= n)
        return;
    if (m_result.ratios.size() != n)
        m_result.ratios.assign(n, n > 0 ? 100 / (int)n : 0);
    bool manual_stale = false;
    for (size_t o : m_ratio_manual_order) {
        if (o >= n) { manual_stale = true; break; }
    }
    if (manual_stale)
        reset_manual_ratio_state();

    int max_value = (int)(100 - (n - 1) * MIN_COMPONENT_RATIO);
    value = std::clamp(value, MIN_COMPONENT_RATIO, std::max(MIN_COMPONENT_RATIO, max_value));

    if (n == 2) {
        if (idx == 0) {
            m_result.ratios[0] = value;
            m_result.ratios[1] = 100 - value;
        } else {
            m_result.ratios[1] = value;
            m_result.ratios[0] = 100 - value;
        }
        m_result.ratios[0] = std::clamp(m_result.ratios[0], MIN_COMPONENT_RATIO, 100 - MIN_COMPONENT_RATIO);
        m_result.ratios[1] = 100 - m_result.ratios[0];
    } else if (n >= 3) {
        m_result.ratios[idx] = value;
        int remaining = 100 - value;

        std::vector<size_t> others;
        int others_sum = 0;
        for (size_t i = 0; i < n; ++i) {
            if (i == idx) continue;
            others.push_back(i);
            others_sum += m_result.ratios[i];
        }

        if (!others.empty()) {
            if (others_sum > 0) {
                int assigned = 0;
                for (size_t k = 0; k < others.size(); ++k) {
                    int nv = (int)((double)remaining * m_result.ratios[others[k]] / others_sum + 0.5);
                    nv = std::max(nv, MIN_COMPONENT_RATIO);
                    m_result.ratios[others[k]] = nv;
                    assigned += nv;
                }
                while (assigned != remaining) {
                    if (assigned > remaining) {
                        int pick = -1;
                        for (size_t k = 0; k < others.size(); ++k)
                            if (m_result.ratios[others[k]] > MIN_COMPONENT_RATIO
                                && (pick < 0 || m_result.ratios[others[k]] > m_result.ratios[others[pick]]))
                                pick = (int)k;
                        if (pick < 0) break;
                        --m_result.ratios[others[pick]]; --assigned;
                    } else {
                        int pick = 0;
                        for (size_t k = 1; k < others.size(); ++k)
                            if (m_result.ratios[others[k]] > m_result.ratios[others[pick]])
                                pick = (int)k;
                        ++m_result.ratios[others[pick]]; ++assigned;
                    }
                }
            } else {
                int base = remaining / (int)others.size();
                for (size_t k = 0; k < others.size(); ++k)
                    m_result.ratios[others[k]] = base;
                m_result.ratios[others.back()] += remaining - base * (int)others.size();
            }
        }
    }

    refresh_ratio_labels();
    sync_triangle_weights_from_ratios();
    update_preview();
}

void MixedFilamentDialog::apply_dragged_triangle_ratio(int r0, int r1, int r2)
{
    if (m_result.ratios.size() < 3)
        return;

    int ratios[3] = {
        std::clamp(r0, MIN_COMPONENT_RATIO, 100),
        std::clamp(r1, MIN_COMPONENT_RATIO, 100),
        std::clamp(r2, MIN_COMPONENT_RATIO, 100)
    };

    int sum = ratios[0] + ratios[1] + ratios[2];
    while (sum > 100) {
        int idx = 0;
        for (int i = 1; i < 3; ++i) {
            if (ratios[i] > ratios[idx])
                idx = i;
        }
        if (ratios[idx] <= MIN_COMPONENT_RATIO)
            break;
        --ratios[idx];
        --sum;
    }
    while (sum < 100) {
        int idx = 0;
        for (int i = 1; i < 3; ++i) {
            if (ratios[i] < ratios[idx])
                idx = i;
        }
        ++ratios[idx];
        ++sum;
    }

    m_result.ratios[0] = ratios[0];
    m_result.ratios[1] = ratios[1];
    m_result.ratios[2] = ratios[2];
    sync_triangle_weights_from_ratios();
    reset_manual_ratio_state();
    update_preview();
}

void MixedFilamentDialog::start_ratio_editor(size_t idx, wxWindow* anchor, const wxRect& anchor_rect)
{
    if (!anchor || idx >= m_result.ratios.size())
        return;
    if (m_ratio_editor_panel && m_ratio_editor_panel->IsShown())
        commit_ratio_editor(true);

    if (!m_ratio_editor_panel) {
        wxColour bg = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
        wxColour fg = StateColor::darkModeColorFor(wxColour("#262E30"));

        m_ratio_editor_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize, wxBORDER_SIMPLE);
        m_ratio_editor_panel->SetBackgroundColour(bg);

        auto* hsizer = new wxBoxSizer(wxHORIZONTAL);

        m_ratio_editor = new wxTextCtrl(m_ratio_editor_panel, wxID_ANY, wxEmptyString,
                                        wxDefaultPosition, wxDefaultSize,
                                        wxTE_PROCESS_ENTER | wxTE_RIGHT | wxBORDER_NONE);
        m_ratio_editor->SetFont(::Label::Body_10);
        m_ratio_editor->SetMaxLength(3);
        m_ratio_editor->SetBackgroundColour(bg);
        m_ratio_editor->SetForegroundColour(fg);
        // Default wxTextCtrl best width (~140px) is too wide for the sizer to
        // shrink, which would push the "%" suffix out of the panel.  Cap the
        // editor's min width to the digits only (ratios are always two digits).
        {
            wxClientDC mdc(m_ratio_editor);
            mdc.SetFont(::Label::Body_10);
            int digits_w = mdc.GetTextExtent(wxT("88")).GetWidth();
            m_ratio_editor->SetMinSize(wxSize(digits_w + FromDIP(2), -1));
        }

        auto* pct_label = new wxStaticText(m_ratio_editor_panel, wxID_ANY, wxT("%"));
        pct_label->SetFont(::Label::Body_10);
        pct_label->SetForegroundColour(fg);
        pct_label->SetBackgroundColour(bg);
        pct_label->SetMinSize(pct_label->GetBestSize());

        hsizer->Add(m_ratio_editor, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
        hsizer->Add(pct_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
        m_ratio_editor_panel->SetSizer(hsizer);
        m_ratio_editor_panel->Hide();

        m_ratio_editor->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { commit_ratio_editor(true); });
        m_ratio_editor->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
            commit_ratio_editor(true);
            e.Skip();
        });
        m_ratio_editor->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
            if (e.GetKeyCode() == WXK_ESCAPE)
                commit_ratio_editor(false);
            else
                e.Skip();
        });
    }

    m_ratio_editor_idx = idx;

    // Keep the editor in the same window hierarchy as the clicked label so the
    // z-order is reliable and the editor fully covers the anchor (dual-color
    // labels live on the dialog, triple-color labels live on the triangle
    // panel).
    wxWindow* target_parent = anchor->GetParent();
    if (target_parent && m_ratio_editor_panel->GetParent() != target_parent)
        m_ratio_editor_panel->Reparent(target_parent);

    // Hide the label being edited to avoid its (hover-state) text leaking out
    // next to the editor; restored on commit.
    m_ratio_editor_anchor = anchor;
    anchor->Hide();

    wxPoint pos = anchor->GetPosition() + anchor_rect.GetTopLeft();
    // Match the editor to the label (hover box) size so the inline editor and
    // the hover state look identical.  A small floor keeps the "%" suffix from
    // being squeezed out on very narrow labels.
    wxSize size = anchor->GetSize();
    size.SetWidth(std::max(size.GetWidth(), FromDIP(30)));
    size.SetHeight(std::max(size.GetHeight(), FromDIP(18)));
    m_ratio_editor_panel->SetSize(wxRect(pos, size));
    m_ratio_editor_panel->Layout();
    m_ratio_editor->SetValue(wxString::Format(wxT("%d"), ratio(idx)));
    m_ratio_editor_panel->Show();
    m_ratio_editor_panel->Raise();
    m_ratio_editor->SetFocus();
    m_ratio_editor->SelectAll();
    m_ratio_editor_panel->Refresh();
    Update();
}

void MixedFilamentDialog::commit_ratio_editor(bool apply)
{
    if (!m_ratio_editor_panel || !m_ratio_editor_panel->IsShown() || m_ratio_editor_committing)
        return;

    m_ratio_editor_committing = true;

    // Restore the hidden anchor before applying the ratio, so any sizer layout
    // triggered by refresh_ratio_labels() accounts for the visible label.
    m_ratio_editor_panel->Hide();
    if (m_ratio_editor_anchor) {
        m_ratio_editor_anchor->Show();
        m_ratio_editor_anchor = nullptr;
    }

    if (apply) {
        wxString value = m_ratio_editor->GetValue();
        value.Trim(true);
        value.Trim(false);
        if (value.EndsWith(wxT("%")))
            value.RemoveLast();

        long parsed = 0;
        if (value.ToLong(&parsed))
            apply_manual_ratio(m_ratio_editor_idx, (int)parsed);
        else
            refresh_ratio_labels();
    }

    m_ratio_editor_committing = false;
}

void MixedFilamentDialog::commit_ratio_editor_from_background(wxMouseEvent& e)
{
    if (m_ratio_editor_panel && m_ratio_editor_panel->IsShown()) {
        wxPoint mouse_in_panel = m_ratio_editor_panel->ScreenToClient(wxGetMousePosition());
        if (!m_ratio_editor_panel->GetClientRect().Contains(mouse_in_panel))
            commit_ratio_editor(true);
    }
    e.Skip();
}

// ---- UI Construction ----

void MixedFilamentDialog::build_ui()
{
    const wxColour mc_bg       = StateColor::darkModeColorFor(*wxWHITE);
    const wxColour mc_bg_sub   = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
    const wxColour mc_border   = StateColor::darkModeColorFor(wxColour("#CECECE"));
    const wxColour mc_text     = StateColor::darkModeColorFor(wxColour("#262E30"));
    const wxColour mc_dim_text = StateColor::darkModeColorFor(wxColour("#ACACAC"));

    SetBackgroundColour(mc_bg);
    Bind(wxEVT_LEFT_DOWN, &MixedFilamentDialog::commit_ratio_editor_from_background, this);
    SetSize(FromDIP(439), FromDIP(580));

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    auto* top_sizer = new wxBoxSizer(wxHORIZONTAL);
    top_sizer->Add(create_preview_panel(), 0, wxALL, FromDIP(20));

    m_right_sizer = new wxBoxSizer(wxVERTICAL);
    m_right_sizer->Add(create_material_selection(), 0, wxEXPAND);
    m_right_sizer->Add(create_gradient_section(), 0, wxEXPAND | wxTOP, FromDIP(7));

    m_ratio_sizer = create_ratio_slider();
    m_right_sizer->Add(m_ratio_sizer, 0, wxEXPAND | wxTOP, FromDIP(7));

    m_triangle_sizer = create_triangle_picker();
    m_right_sizer->Add(m_triangle_sizer, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(7));

    top_sizer->Add(m_right_sizer, 1, wxTOP | wxRIGHT | wxBOTTOM, FromDIP(20));
    main_sizer->Add(top_sizer, 0, wxEXPAND);

    main_sizer->Add(create_recommendation_grid(), 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(25));

    // Warning panel: red bordered box with exclamation icon + text
    m_warning_sizer = new wxBoxSizer(wxVERTICAL);
    m_warning_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(48)));
    m_warning_panel->SetMinSize(wxSize(-1, FromDIP(48)));
    m_warning_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_warning_panel->Bind(wxEVT_PAINT, &MixedFilamentDialog::paint_warning_panel, this);
    m_warning_sizer->Add(m_warning_panel, 0, wxEXPAND);
    main_sizer->Add(m_warning_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(25));
    m_warning_panel->Hide();

    main_sizer->Add(create_button_panel(), 0, wxALIGN_RIGHT | wxALL, FromDIP(20));

    SetSizer(main_sizer);

    rebuild_all_combos();
    update_component_count_ui();
    update_preview();
    update_ok_button_state();

    Layout();
    CentreOnParent();
}

wxBoxSizer* MixedFilamentDialog::create_preview_panel()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_preview_canvas = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(129), FromDIP(129)));
    m_preview_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_preview_canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_preview_canvas);
        wxSize sz = m_preview_canvas->GetClientSize();
        size_t n = num_components();

        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

        if (n == 0) return;

        int swatch_sz = FromDIP(80);
        int x0 = (sz.GetWidth() - swatch_sz) / 2;
        int y0 = (sz.GetHeight() - swatch_sz) / 2;
        double radius = FromDIP(6);

        if (m_result.gradient_enabled && n == 2) {
            Slic3r::GradientCurve curve;
            if (!m_result.gradient_curve.empty()) {
                curve.points = m_result.gradient_curve;
            } else {
                double yStart = (m_result.gradient_direction == 0) ? kGradientMaxRatio : kGradientMinRatio;
                double yEnd   = (m_result.gradient_direction == 0) ? kGradientMinRatio : kGradientMaxRatio;
                curve.points = {{0.0, yStart, NAN, NAN}, {1.0, yEnd, NAN, NAN}};
            }

            wxColour colA = comp_colour(0);
            wxColour colB = comp_colour(1);
            const int bands = std::max(80, swatch_sz);
            double band_h = static_cast<double>(swatch_sz) / bands;
            dc.SetPen(*wxTRANSPARENT_PEN);
            for (int b = 0; b < bands; ++b) {
                double t = 1.0 - (b + 0.5) / bands;
                double r1 = Slic3r::sample_gradient_curve(curve, t);
                double r2 = 1.0 - r1;
                wxColour band_col = blend_n_colors({colA, colB}, {r1, r2});
                dc.SetBrush(wxBrush(band_col));
                int by = y0 + static_cast<int>(b * band_h);
                int bh = static_cast<int>((b + 1) * band_h) - static_cast<int>(b * band_h) + 1;
                dc.DrawRectangle(x0, by, swatch_sz, bh);
            }

            // Mask corners: overdraw a thick background-colored rounded rect frame
            // so the inner edge forms the desired rounded corners.
            // Known limitation: this assumes the panel background equals
            // darkModeColorFor(white).  wxGraphicsContext::Clip(path) is not
            // available in our wxWidgets build (only Clip(wxRegion) exists).
            int r = static_cast<int>(radius);
            wxColour bg = StateColor::darkModeColorFor(*wxWHITE);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(bg, r * 2));
            dc.DrawRoundedRectangle(x0 - r, y0 - r, swatch_sz + r * 2, swatch_sz + r * 2, radius * 2);
        } else {
            std::vector<wxColour> cols;
            std::vector<double>   weights;
            for (size_t i = 0; i < n; ++i) {
                cols.push_back(comp_colour(i));
                weights.push_back(ratio(i) / 100.0);
            }
            wxColour mixed = blend_n_colors(cols, weights);
            dc.SetBrush(wxBrush(mixed));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(x0, y0, swatch_sz, swatch_sz, radius);
        }
    });

    sizer->Add(m_preview_canvas, 0, wxALIGN_CENTER);

    auto* label = new wxStaticText(this, wxID_ANY, _L("Effect Preview"));
    label->SetForegroundColour(wxColour("#909090"));
    label->SetFont(::Label::Body_13);
    sizer->Add(label, 0, wxALIGN_CENTER | wxTOP, FromDIP(4));

    return sizer;
}

wxBoxSizer* MixedFilamentDialog::create_material_selection()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Summary panel — draws N components dynamically
    m_summary_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(234), FromDIP(40)));
    m_summary_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_summary_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_summary_panel);
        wxSize sz = m_summary_panel->GetClientSize();

        wxColour sum_bg   = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
        wxColour sum_text = StateColor::darkModeColorFor(wxColour("#262E30"));
        dc.SetBrush(wxBrush(sum_bg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

        int swatch_sz = FromDIP(20);
        int y_center = (sz.GetHeight() - swatch_sz) / 2;
        int x = FromDIP(13);

        dc.SetFont(::Label::Body_13);

        auto draw_summary_swatch = [&](size_t comp_idx) {
            unsigned int c = comp(comp_idx);
            std::string color_hex = "#D9D9D9";
            if (c >= 1 && c <= m_physical_colors.size())
                color_hex = m_physical_colors[c - 1];
            std::string label = std::to_string(c);
            wxBitmap* icon = get_extruder_color_icon(color_hex, label, swatch_sz, swatch_sz);
            if (icon && icon->IsOk())
                dc.DrawBitmap(*icon, x, y_center);
            x += swatch_sz + FromDIP(4);
        };

        if (m_result.gradient_enabled && num_components() == 2) {
            size_t idx_a = (m_result.gradient_direction == 0) ? 0 : 1;
            size_t idx_b = 1 - idx_a;
            draw_summary_swatch(idx_a);

            dc.SetTextForeground(sum_text);
            wxString arrow = wxT("\u2192");
            wxSize arrow_sz = dc.GetTextExtent(arrow);
            dc.DrawText(arrow, x, y_center + (swatch_sz - arrow_sz.GetHeight()) / 2);
            x += arrow_sz.GetWidth() + FromDIP(4);

            draw_summary_swatch(idx_b);
        } else {
            for (size_t i = 0; i < num_components(); ++i) {
                if (i > 0) {
                    dc.SetTextForeground(sum_text);
                    wxString plus = wxT("+");
                    wxSize plus_sz = dc.GetTextExtent(plus);
                    dc.DrawText(plus, x, y_center + (swatch_sz - plus_sz.GetHeight()) / 2);
                    x += plus_sz.GetWidth() + FromDIP(4);
                }
                draw_summary_swatch(i);

                dc.SetTextForeground(sum_text);
                wxString pct = wxString::Format(wxT("%d%%"), ratio(i));
                wxSize pct_sz = dc.GetTextExtent(pct);
                dc.DrawText(pct, x, y_center + (swatch_sz - pct_sz.GetHeight()) / 2);
                x += pct_sz.GetWidth() + FromDIP(4);
            }
        }
    });
    sizer->Add(m_summary_panel, 0, wxEXPAND);

    auto* sel_label = new wxStaticText(this, wxID_ANY, _L("Select Mixed Materials"));
    sel_label->SetForegroundColour(wxColour("#909090"));
    sel_label->SetFont(::Label::Body_12);
    sizer->Add(sel_label, 0, wxTOP, FromDIP(6));

    m_material_rows_sizer = new wxBoxSizer(wxVERTICAL);

    m_combo_filaments.clear();
    m_combo_to_physical.clear();
    for (size_t i = 0; i < m_result.components.size(); ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        wxString lbl_text = wxString::Format(_L("Filament %d"), (int)(i + 1));
        auto* lbl = new wxStaticText(this, wxID_ANY, lbl_text);
        lbl->SetFont(::Label::Body_12);
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

        auto* combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxSize(FromDIP(166), FromDIP(24)), 0, nullptr, wxCB_READONLY);
        combo->SetKeepDropArrow(true);
        combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_filament_changed(); });
        row->Add(combo, 1, wxALIGN_CENTER_VERTICAL);

        m_combo_filaments.push_back(combo);
        m_combo_to_physical.push_back({});
        m_material_rows_sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(9));
    }

    sizer->Add(m_material_rows_sizer, 0, wxEXPAND);

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_btn_add_material = new Button(this, _L("+ Add Material"));
    m_btn_add_material->SetBackgroundColor(wxColour("#F8F8F8"));
    m_btn_add_material->SetBorderColor(wxColour("#EEEEEE"));
    m_btn_add_material->SetTextColor(wxColour("#262E30"));
    m_btn_add_material->SetMinSize(wxSize(-1, FromDIP(24)));
    m_btn_add_material->SetCursor(wxCursor(wxCURSOR_HAND));
    m_btn_add_material->EnableTooltipEvenDisabled();
    m_btn_add_material->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_add_material(); });
    btn_sizer->Add(m_btn_add_material, 1, wxRIGHT, FromDIP(6));

    m_btn_remove_material = new Button(this, _L("- Delete Material"));
    m_btn_remove_material->SetBackgroundColor(wxColour("#F8F8F8"));
    m_btn_remove_material->SetBorderColor(wxColour("#EEEEEE"));
    m_btn_remove_material->SetTextColor(wxColour("#262E30"));
    m_btn_remove_material->SetMinSize(wxSize(-1, FromDIP(24)));
    m_btn_remove_material->SetCursor(wxCursor(wxCURSOR_HAND));
    m_btn_remove_material->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_remove_material(); });
    m_btn_remove_material->Hide();
    btn_sizer->Add(m_btn_remove_material, 1, 0, 0);

    sizer->Add(btn_sizer, 0, wxEXPAND | wxTOP, FromDIP(9));

    return sizer;
}

wxBoxSizer* MixedFilamentDialog::create_ratio_slider()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* ratio_label = new wxStaticText(this, wxID_ANY, _L("Ratio"));
    ratio_label->SetForegroundColour(wxColour("#909090"));
    ratio_label->SetFont(::Label::Body_12);
    sizer->Add(ratio_label, 0, wxBOTTOM, FromDIP(4));

    m_ratio_bar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(27)));
    m_ratio_bar->SetMinSize(wxSize(-1, FromDIP(27)));
    m_ratio_bar->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_ratio_bar->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_ratio_bar);
        wxSize sz = m_ratio_bar->GetClientSize();

        wxColour col_a = comp_colour(0), col_b = comp_colour(1);

        for (int x = 0; x < sz.GetWidth(); ++x) {
            double t = (double)x / sz.GetWidth();
            wxColour c = blend_colors(col_a, col_b, 1.0 - t);
            dc.SetPen(wxPen(c));
            dc.DrawLine(x, 0, x, sz.GetHeight());
        }

        int div_x = (int)(ratio(1) / 100.0 * sz.GetWidth());
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(80, 80, 80)), FromDIP(4)));
        dc.DrawLine(div_x, 0, div_x, sz.GetHeight());
        dc.SetPen(wxPen(*wxWHITE, FromDIP(2)));
        dc.DrawLine(div_x, 0, div_x, sz.GetHeight());
    });

    m_ratio_bar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        if (m_ratio_editor_panel && m_ratio_editor_panel->IsShown())
            commit_ratio_editor(true);
        m_dragging = true;
        m_ratio_bar->CaptureMouse();
        int new_ratio = 100 - (int)(e.GetX() * 100.0 / m_ratio_bar->GetClientSize().GetWidth() + 0.5);
        on_ratio_changed(std::max(MIN_COMPONENT_RATIO, std::min(100 - MIN_COMPONENT_RATIO, new_ratio)));
    });

    m_ratio_bar->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        if (!m_dragging) return;
        int new_ratio = 100 - (int)(e.GetX() * 100.0 / m_ratio_bar->GetClientSize().GetWidth() + 0.5);
        on_ratio_changed(std::max(MIN_COMPONENT_RATIO, std::min(100 - MIN_COMPONENT_RATIO, new_ratio)));
    });

    m_ratio_bar->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (m_dragging) {
            m_dragging = false;
            if (m_ratio_bar->HasCapture())
                m_ratio_bar->ReleaseMouse();
        }
    });

    sizer->Add(m_ratio_bar, 0, wxEXPAND);

    auto* pct_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_label_ratio_a = new RatioLabelPanel(this);
    m_label_ratio_a->SetLabel(wxString::Format(wxT("%d%%"), ratio(0)));
    m_label_ratio_b = new RatioLabelPanel(this);
    m_label_ratio_b->SetLabel(wxString::Format(wxT("%d%%"), ratio(1)));
    auto bind_ratio_click = [this](RatioLabelPanel* label, size_t idx) {
        label->Bind(wxEVT_LEFT_DOWN, [this, label, idx](wxMouseEvent&) {
            wxRect rect(wxPoint(0, 0), label->GetClientSize());
            start_ratio_editor(idx, label, rect);
        });
    };
    bind_ratio_click(m_label_ratio_a, 0);
    bind_ratio_click(m_label_ratio_b, 1);
    pct_sizer->Add(m_label_ratio_a, 0);
    pct_sizer->AddStretchSpacer(1);
    pct_sizer->Add(m_label_ratio_b, 0);
    sizer->Add(pct_sizer, 0, wxEXPAND | wxTOP, FromDIP(2));

    return sizer;
}

// ---- Triangle (ternary) ratio picker ----

// Barycentric coordinate utilities
struct TriPoint { double x, y; };

static double tri_signed_area2(TriPoint a, TriPoint b, TriPoint c)
{
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

static bool tri_contains(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2)
{
    double total = tri_signed_area2(v0, v1, v2);
    if (std::abs(total) < 1e-9) return false;
    double s0 = tri_signed_area2(p, v1, v2) / total;
    double s1 = tri_signed_area2(v0, p, v2) / total;
    double s2 = 1.0 - s0 - s1;
    return s0 >= -0.001 && s1 >= -0.001 && s2 >= -0.001;
}

static void tri_barycentric(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2,
                            double& w0, double& w1, double& w2)
{
    double total = std::abs(tri_signed_area2(v0, v1, v2));
    if (total < 1e-9) { w0 = w1 = w2 = 1.0 / 3.0; return; }
    w0 = std::abs(tri_signed_area2(p, v1, v2)) / total;
    w1 = std::abs(tri_signed_area2(v0, p, v2)) / total;
    w2 = 1.0 - w0 - w1;
    w0 = std::clamp(w0, 0.0, 1.0);
    w1 = std::clamp(w1, 0.0, 1.0);
    w2 = std::clamp(w2, 0.0, 1.0);
    double s = w0 + w1 + w2;
    if (s > 0) { w0 /= s; w1 /= s; w2 /= s; }
}

static TriPoint tri_clamp(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2)
{
    double w0, w1, w2;
    tri_barycentric(p, v0, v1, v2, w0, w1, w2);
    w0 = std::clamp(w0, 0.0, 1.0);
    w1 = std::clamp(w1, 0.0, 1.0);
    w2 = std::clamp(w2, 0.0, 1.0);
    double s = w0 + w1 + w2;
    if (s > 0) { w0 /= s; w1 /= s; w2 /= s; }
    return {w0 * v0.x + w1 * v1.x + w2 * v2.x,
            w0 * v0.y + w1 * v1.y + w2 * v2.y};
}

wxBoxSizer* MixedFilamentDialog::create_triangle_picker()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    int panel_w = FromDIP(160);
    int panel_h = FromDIP(160);
    m_triangle_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(panel_w, panel_h));
    m_triangle_panel->SetMinSize(wxSize(panel_w, panel_h));
    m_triangle_panel->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_triangle_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);

    auto get_vertices = [this]() -> std::tuple<TriPoint, TriPoint, TriPoint> {
        wxSize sz = m_triangle_panel->GetClientSize();
        double pw = sz.GetWidth(), ph = sz.GetHeight();
        double margin = FromDIP(20);
        double avail = std::min(pw, ph) - 2 * margin;
        double side = avail;
        double tri_h = side * std::sqrt(3.0) / 2.0;
        double cx = pw / 2.0;
        double top_y = (ph - tri_h) / 2.0;
        double bot_y = top_y + tri_h;
        TriPoint v0 = {cx, top_y};                             // top
        TriPoint v1 = {cx - side / 2.0, bot_y};               // bottom-left
        TriPoint v2 = {cx + side / 2.0, bot_y};               // bottom-right
        return {v0, v1, v2};
    };

    m_triangle_panel->Bind(wxEVT_PAINT, [this, get_vertices](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_triangle_panel);
        wxSize sz = m_triangle_panel->GetClientSize();
        auto [v0, v1, v2] = get_vertices();

        wxColour tri_bg = StateColor::darkModeColorFor(*wxWHITE);
        dc.SetBrush(wxBrush(tri_bg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

        wxColour c0 = comp_colour(0), c1 = comp_colour(1), c2 = comp_colour(2);

        const bool cache_valid = m_tri_cache_bmp.IsOk() &&
            m_tri_cache_size == sz &&
            m_tri_cache_c0 == c0 && m_tri_cache_c1 == c1 && m_tri_cache_c2 == c2;

        if (!cache_valid) {
            int min_y = (int)std::min({v0.y, v1.y, v2.y});
            int max_y = (int)std::max({v0.y, v1.y, v2.y});
            int min_x = (int)std::min({v0.x, v1.x, v2.x});
            int max_x = (int)std::max({v0.x, v1.x, v2.x});

            m_tri_cache_bmp = wxBitmap(sz.GetWidth(), sz.GetHeight(), 24);
            wxMemoryDC mdc(m_tri_cache_bmp);
            mdc.SetBrush(wxBrush(tri_bg));
            mdc.SetPen(*wxTRANSPARENT_PEN);
            mdc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

            for (int py = min_y; py <= max_y; ++py) {
                for (int px = min_x; px <= max_x; ++px) {
                    TriPoint p = {(double)px, (double)py};
                    if (!tri_contains(p, v0, v1, v2)) continue;
                    double w0, w1, w2;
                    tri_barycentric(p, v0, v1, v2, w0, w1, w2);
                    unsigned char mr, mg, mb;
                    if (w0 + w1 > 1e-6) {
                        float t01 = static_cast<float>(w1 / (w0 + w1));
                        Slic3r::filament_mixer_lerp(c0.Red(), c0.Green(), c0.Blue(),
                                                    c1.Red(), c1.Green(), c1.Blue(),
                                                    t01, &mr, &mg, &mb);
                        float t2 = static_cast<float>(w2);
                        Slic3r::filament_mixer_lerp(mr, mg, mb,
                                                    c2.Red(), c2.Green(), c2.Blue(),
                                                    t2, &mr, &mg, &mb);
                    } else {
                        mr = c2.Red(); mg = c2.Green(); mb = c2.Blue();
                    }
                    mdc.SetPen(wxPen(wxColour(mr, mg, mb)));
                    mdc.DrawPoint(px, py);
                }
            }

            mdc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#CECECE")), 1));
            mdc.SetBrush(*wxTRANSPARENT_BRUSH);
            wxPoint pts[3] = {{(int)v0.x, (int)v0.y}, {(int)v1.x, (int)v1.y}, {(int)v2.x, (int)v2.y}};
            mdc.DrawPolygon(3, pts);

            mdc.SelectObject(wxNullBitmap);
            m_tri_cache_c0 = c0; m_tri_cache_c1 = c1; m_tri_cache_c2 = c2;
            m_tri_cache_size = sz;
        }

        dc.DrawBitmap(m_tri_cache_bmp, 0, 0);

        // Drag handle (always redrawn on top of cached bitmap)
        double hx = m_tri_wx * v0.x + m_tri_wy * v1.x + m_tri_wz * v2.x;
        double hy = m_tri_wx * v0.y + m_tri_wy * v1.y + m_tri_wz * v2.y;
        int handle_r = FromDIP(5);
        dc.SetBrush(*wxWHITE_BRUSH);
        dc.SetPen(wxPen(wxColour("#262E30"), FromDIP(2)));
        dc.DrawCircle((int)hx, (int)hy, handle_r);

        if (m_result.ratios.size() >= 3) {
            dc.SetFont(::Label::Body_10);
            wxSize ts0 = dc.GetTextExtent(wxString::Format(wxT("%d%%"), m_result.ratios[0]));
            int top_label_y = std::max(0, (int)(v0.y - ts0.GetHeight() - FromDIP(4)));

            dc.SetFont(::Label::Body_12);
            dc.SetTextForeground(wxColour("#909090"));
            dc.DrawText(_L("Ratio"), FromDIP(2), top_label_y);

            // Position the real RatioLabelPanel children
            for (int i = 0; i < 3 && i < (int)m_triangle_ratio_labels.size(); ++i) {
                if (!m_triangle_ratio_labels[i]) continue;
                m_triangle_ratio_labels[i]->SetLabel(
                    wxString::Format(wxT("%d%%"), m_result.ratios[i]));
                wxSize lsz = m_triangle_ratio_labels[i]->GetMinSize();
                int lx = 0, ly = 0;
                if (i == 0) {
                    lx = (int)(v0.x - lsz.GetWidth() / 2);
                    ly = top_label_y;
                } else if (i == 1) {
                    lx = (int)(v1.x - lsz.GetWidth() / 2);
                    ly = (int)(v1.y + FromDIP(3));
                } else {
                    lx = (int)(v2.x - lsz.GetWidth() / 2);
                    ly = (int)(v2.y + FromDIP(3));
                }
                m_triangle_ratio_labels[i]->SetSize(lx, ly, lsz.GetWidth(), lsz.GetHeight());
            }
        }
    });

    auto handle_mouse = [this, get_vertices](wxMouseEvent& e, bool is_down) {
        auto [v0, v1, v2] = get_vertices();
        TriPoint p = {(double)e.GetX(), (double)e.GetY()};

        if (is_down) {
            // Only start dragging when the press lands inside the triangle;
            // clicks outside the triangle must not change the mix ratio.
            if (!tri_contains(p, v0, v1, v2))
                return;
            m_dragging = true;
            m_triangle_panel->CaptureMouse();
        }

        if (!m_dragging) return;

        TriPoint clamped = tri_clamp(p, v0, v1, v2);
        tri_barycentric(clamped, v0, v1, v2, m_tri_wx, m_tri_wy, m_tri_wz);

        int r0 = (int)(m_tri_wx * 100 + 0.5);
        int r1 = (int)(m_tri_wy * 100 + 0.5);
        int r2 = 100 - r0 - r1;
        r0 = std::clamp(r0, 0, 100);
        r1 = std::clamp(r1, 0, 100);
        r2 = std::clamp(r2, 0, 100);

        apply_dragged_triangle_ratio(r0, r1, r2);
    };

    // Create 3 RatioLabelPanel children on the triangle panel
    m_triangle_ratio_labels.fill(nullptr);
    for (int i = 0; i < 3; ++i) {
        auto* lbl = new RatioLabelPanel(m_triangle_panel);
        lbl->SetLabel(wxString::Format(wxT("%d%%"),
                      (i < (int)m_result.ratios.size()) ? m_result.ratios[i] : 33));
        size_t idx = (size_t)i;
        lbl->Bind(wxEVT_LEFT_DOWN, [this, lbl, idx](wxMouseEvent&) {
            wxRect rect(wxPoint(0, 0), lbl->GetClientSize());
            start_ratio_editor(idx, lbl, rect);
        });
        m_triangle_ratio_labels[i] = lbl;
    }

    m_triangle_panel->Bind(wxEVT_LEFT_DOWN, [this, handle_mouse](wxMouseEvent& e) {
        if (m_ratio_editor_panel && m_ratio_editor_panel->IsShown())
            commit_ratio_editor(true);
        handle_mouse(e, true);
    });
    m_triangle_panel->Bind(wxEVT_MOTION, [this, handle_mouse](wxMouseEvent& e) {
        if (m_dragging)
            handle_mouse(e, false);
    });
    m_triangle_panel->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (m_dragging) {
            m_dragging = false;
            if (m_triangle_panel->HasCapture())
                m_triangle_panel->ReleaseMouse();
        }
    });

    sizer->Add(m_triangle_panel, 0);

    return sizer;
}

wxBoxSizer* MixedFilamentDialog::create_gradient_section()
{
    m_gradient_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_chk_gradient = new ::CheckBox(this);
    m_chk_gradient->SetValue(m_result.gradient_enabled);
    m_chk_gradient->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { e.Skip(); on_gradient_toggled(); });
    m_gradient_sizer->Add(m_chk_gradient, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(4));

    m_label_gradient = new wxStaticText(this, wxID_ANY, _L("Gradient Effect"));
    m_label_gradient->SetFont(::Label::Body_13);
    m_gradient_sizer->Add(m_label_gradient, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    m_combo_gradient_dir = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                        wxSize(FromDIP(152), FromDIP(24)), 0, nullptr, wxCB_READONLY);
    m_combo_gradient_dir->SetKeepDropArrow(true);
    update_gradient_direction_items();
    m_combo_gradient_dir->SetSelection(m_result.gradient_direction);
    m_combo_gradient_dir->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_gradient_direction_changed(); });
    m_combo_gradient_dir->Show(m_result.gradient_enabled);

    m_gradient_sizer->Add(m_combo_gradient_dir, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(m_gradient_sizer, 0, wxEXPAND);

    // Custom curve editor: visible only when gradient is on and exactly 2 components are mixed.
    m_curve_sizer = new wxBoxSizer(wxVERTICAL);
    m_curve_editor = new GradientCurveEditor(this, comp_colour(0), comp_colour(1));
    if (!m_result.gradient_curve.empty())
        m_curve_editor->set_points(m_result.gradient_curve);
    else
        m_curve_editor->reset_to_linear((m_result.gradient_direction == 0) ? 0.9 : 0.1,
                                        (m_result.gradient_direction == 0) ? 0.1 : 0.9);
    m_curve_editor->Bind(wxEVT_GRADIENT_CURVE_CHANGED,
        [this](wxCommandEvent&) { on_gradient_curve_changed(); });
    m_curve_sizer->Add(m_curve_editor, 0, wxEXPAND | wxTOP, FromDIP(4));

    outer->Add(m_curve_sizer, 0, wxEXPAND | wxTOP, FromDIP(6));
    const bool curve_visible = m_result.gradient_enabled && num_components() == 2;
    m_curve_sizer->ShowItems(curve_visible);

    // Per-part gradient toggle sits BELOW the curve editor.
    m_per_part_gradient_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_chk_per_part_gradient = new ::CheckBox(this);
    m_chk_per_part_gradient->SetValue(m_result.per_part_gradient);
    m_chk_per_part_gradient->Bind(wxEVT_TOGGLEBUTTON,
        [this](wxCommandEvent& e) { e.Skip(); on_per_part_gradient_toggled(); });
    m_per_part_gradient_sizer->Add(m_chk_per_part_gradient, 0,
        wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(4));

    m_label_per_part_gradient = new wxStaticText(this, wxID_ANY, _L("Enable per-part gradient effect"));
    m_label_per_part_gradient->SetFont(::Label::Body_13);
    m_per_part_gradient_sizer->Add(m_label_per_part_gradient, 0,
        wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    outer->Add(m_per_part_gradient_sizer, 0, wxEXPAND | wxTOP, FromDIP(2));
    m_per_part_gradient_sizer->ShowItems(m_result.gradient_enabled);

    return outer;
}

wxBoxSizer* MixedFilamentDialog::create_recommendation_grid()
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* rec_label = new wxStaticText(this, wxID_ANY, _L("Mixing Recommendations"));
    rec_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#ACACAC")));
    rec_label->SetFont(::Label::Body_10);
    title_sizer->Add(rec_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    auto* rec_line = new wxPanel(this, wxID_ANY);
    rec_line->SetMinSize(wxSize(-1, 1));
    rec_line->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#DFDFDF")));
    title_sizer->Add(rec_line, 1, wxALIGN_CENTER_VERTICAL);

    outer->Add(title_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    m_recommendation_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(116)));
    m_recommendation_scroll->SetScrollRate(0, 5);
    m_recommendation_scroll->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F8F8")));

    m_recommendation_grid = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    auto* scroll_inner_sizer = new wxBoxSizer(wxVERTICAL);
    scroll_inner_sizer->Add(m_recommendation_grid, 1, wxEXPAND | wxLEFT | wxTOP, FromDIP(8));
    m_recommendation_scroll->SetSizer(scroll_inner_sizer);

    rebuild_recommendation_items();

    outer->Add(m_recommendation_scroll, 1, wxEXPAND | wxTOP, FromDIP(4));
    return outer;
}

void MixedFilamentDialog::rebuild_recommendation_items()
{
    if (!m_recommendation_scroll || !m_recommendation_grid)
        return;

    static constexpr int MAX_RECOMMENDATIONS = 100;

    m_recommendation_scroll->Freeze();
    m_recommendation_grid->Clear(true);

    size_t n = m_physical_colors.size();
    int count = 0;

    // Group physical filaments by type (only same-type combos are recommended)
    std::map<std::string, std::vector<size_t>> type_groups;
    for (size_t i = 0; i < n; ++i) {
        std::string t = (i < m_physical_types.size()) ? m_physical_types[i] : "PLA";
        // Skip support filaments (type ends with "-S")
        if (t.size() >= 2 && t.compare(t.size() - 2, 2, "-S") == 0)
            continue;
        type_groups[t].push_back(i);
    }

    if (num_components() >= 3) {
        // Three-color: C(g,3) x 3 variants per same-type group
        for (auto& [type, indices] : type_groups) {
            if (count >= MAX_RECOMMENDATIONS) break;
            size_t g = indices.size();
            for (size_t ai = 0; ai < g && count < MAX_RECOMMENDATIONS; ++ai) {
                for (size_t bi = ai + 1; bi < g && count < MAX_RECOMMENDATIONS; ++bi) {
                    for (size_t ci = bi + 1; ci < g && count < MAX_RECOMMENDATIONS; ++ci) {
                        size_t idx[3] = {indices[ai], indices[bi], indices[ci]};
                        // 3 variants: each filament takes the 50% role in turn
                        for (int dominant = 0; dominant < 3 && count < MAX_RECOMMENDATIONS; ++dominant) {
                            size_t i0 = idx[(dominant + 1) % 3]; // 25%
                            size_t i1 = idx[(dominant + 2) % 3]; // 25%
                            size_t i2 = idx[dominant];           // 50%

                            wxColour ca(m_physical_colors[i0]);
                            wxColour cb(m_physical_colors[i1]);
                            wxColour cc(m_physical_colors[i2]);
                            wxColour mixed = blend_n_colors({ca, cb, cc}, {0.25, 0.25, 0.50});

                            auto* item = new wxPanel(m_recommendation_scroll, wxID_ANY,
                                                     wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)));
                            item->SetBackgroundColour(mixed);
                            item->SetCursor(wxCursor(wxCURSOR_HAND));

                            unsigned int ca_1 = (unsigned int)(i0 + 1);
                            unsigned int cb_1 = (unsigned int)(i1 + 1);
                            unsigned int cc_1 = (unsigned int)(i2 + 1);
                            item->Bind(wxEVT_LEFT_UP, [this, ca_1, cb_1, cc_1](wxMouseEvent&) {
                                on_recommendation_clicked_triple(ca_1, cb_1, cc_1);
                            });
                            item->SetToolTip(wxString::Format(wxT("%s + %s + %s"),
                                wxString::FromUTF8(m_physical_names[i0]),
                                wxString::FromUTF8(m_physical_names[i1]),
                                wxString::FromUTF8(m_physical_names[i2])));

                            m_recommendation_grid->Add(item, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
                            ++count;
                        }
                    }
                }
            }
        }
    } else {
        // Two-color: C(g,2) per same-type group
        for (auto& [type, indices] : type_groups) {
            if (count >= MAX_RECOMMENDATIONS) break;
            size_t g = indices.size();
            for (size_t ai = 0; ai < g && count < MAX_RECOMMENDATIONS; ++ai) {
                for (size_t bi = ai + 1; bi < g && count < MAX_RECOMMENDATIONS; ++bi) {
                    size_t i = indices[ai];
                    size_t j = indices[bi];

                    wxColour ca(m_physical_colors[i]);
                    wxColour cb(m_physical_colors[j]);
                    wxColour mixed = blend_colors(ca, cb, 0.5);

                    auto* item = new wxPanel(m_recommendation_scroll, wxID_ANY,
                                             wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)));
                    item->SetBackgroundColour(mixed);
                    item->SetCursor(wxCursor(wxCURSOR_HAND));

                    unsigned int comp_a = (unsigned int)(i + 1);
                    unsigned int comp_b = (unsigned int)(j + 1);
                    item->Bind(wxEVT_LEFT_UP, [this, comp_a, comp_b](wxMouseEvent&) {
                        on_recommendation_clicked(comp_a, comp_b);
                    });
                    item->SetToolTip(wxString::Format(wxT("%s + %s"),
                        wxString::FromUTF8(m_physical_names[i]),
                        wxString::FromUTF8(m_physical_names[j])));

                    m_recommendation_grid->Add(item, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
                    ++count;
                }
            }
        }
    }

    m_recommendation_scroll->SetScrollbars(0, FromDIP(20), 0, 1);
    m_recommendation_scroll->FitInside();
    m_recommendation_scroll->Layout();
    m_recommendation_scroll->Thaw();
}

wxBoxSizer* MixedFilamentDialog::create_button_panel()
{
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    m_btn_cancel = new Button(this, _L("Cancel"));
    m_btn_cancel->SetBackgroundColor(*wxWHITE);
    m_btn_cancel->SetBorderColor(wxColour("#CECECE"));
    m_btn_cancel->SetTextColor(wxColour("#262E30"));
    m_btn_cancel->SetMinSize(wxSize(FromDIP(55), FromDIP(24)));
    m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    m_btn_ok = new Button(this, _L("OK"));
    m_btn_ok->SetBackgroundColor(wxColour("#00AE42"));
    m_btn_ok->SetBorderColor(wxColour("#00AE42"));
    m_btn_ok->SetTextColor(*wxWHITE);
    m_btn_ok->SetMinSize(wxSize(FromDIP(55), FromDIP(24)));
    m_btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });

    sizer->Add(m_btn_cancel, 0, wxRIGHT, FromDIP(12));
    sizer->Add(m_btn_ok, 0);

    return sizer;
}

void MixedFilamentDialog::rebuild_all_combos()
{
    m_combo_to_physical.resize(m_combo_filaments.size());

    for (size_t i = 0; i < m_combo_filaments.size(); ++i) {
        std::set<unsigned int> others_selected;
        std::set<std::string>  others_types;
        for (size_t k = 0; k < m_result.components.size(); ++k) {
            if (k == i) continue;
            unsigned int phys = m_result.components[k];
            others_selected.insert(phys);
            if (phys >= 1 && phys <= m_physical_types.size())
                others_types.insert(m_physical_types[phys - 1]);
        }

        auto* combo = m_combo_filaments[i];
        combo->Clear();
        m_combo_to_physical[i].clear();

        int restore_sel = -1;
        unsigned int cur_phys = (i < m_result.components.size()) ? m_result.components[i] : 0;

        if (cur_phys == 0) {
            combo->Append(_L("-- Select --"));
            m_combo_to_physical[i].push_back(0);
            restore_sel = 0;
        }

        for (size_t j = 0; j < m_physical_names.size(); ++j) {
            unsigned int phys_1based = (unsigned int)(j + 1);

            if (others_selected.count(phys_1based))
                continue;

            int style = 0;
            if (!others_types.empty() && !m_physical_types.empty()) {
                std::string this_type = (j < m_physical_types.size()) ? m_physical_types[j] : "PLA";
                if (others_types.find(this_type) == others_types.end())
                    style = DD_ITEM_STYLE_DIMMED;
            }

            int idx = combo->Append(wxString::FromUTF8(m_physical_names[j]), make_swatch_bitmap(j), style);
            m_combo_to_physical[i].push_back(phys_1based);

            if (phys_1based == cur_phys)
                restore_sel = idx;
        }

        if (restore_sel >= 0)
            combo->SetSelection(restore_sel);
        else if (combo->GetCount() > 0)
            combo->SetSelection(0);
    }
}

void MixedFilamentDialog::refresh_curve_editor_colors()
{
    if (m_curve_editor)
        m_curve_editor->set_colors(comp_colour(0), comp_colour(1));
}

// ---- Event Handlers ----

void MixedFilamentDialog::on_filament_changed()
{
    for (size_t i = 0; i < m_combo_filaments.size() && i < m_result.components.size(); ++i) {
        int sel = m_combo_filaments[i]->GetSelection();
        if (sel >= 0 && i < m_combo_to_physical.size() && sel < (int)m_combo_to_physical[i].size())
            m_result.components[i] = m_combo_to_physical[i][sel];
    }

    refresh_curve_editor_colors();
    rebuild_all_combos();
    update_gradient_direction_items();
    update_preview();
    update_ok_button_state();
}

void MixedFilamentDialog::on_ratio_changed(int new_ratio_a)
{
    if (m_result.ratios.size() < 2) return;
    m_result.ratios[0] = new_ratio_a;
    m_result.ratios[1] = 100 - new_ratio_a;

    reset_manual_ratio_state();
    refresh_ratio_labels();

    update_preview();
}

void MixedFilamentDialog::on_gradient_toggled()
{
    bool checked = m_chk_gradient->GetValue();

    if (checked) {
        auto& print_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (!print_config.opt_bool("enable_mixed_color_sublayer")) {
            wxMessageDialog dlg(this,
                _L("Gradient effect requires 'Mixed color sublayer' to be enabled. Enable it now?"),
                _L("Mixed Color Sublayer"),
                wxYES_NO | wxICON_QUESTION);
            if (dlg.ShowModal() == wxID_YES) {
                DynamicPrintConfig new_conf;
                new_conf.set_key_value("enable_mixed_color_sublayer", new ConfigOptionBool(true));
                wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(new_conf);
            } else {
                m_chk_gradient->SetValue(false);
                return;
            }
        }
    }

    m_result.gradient_enabled = m_chk_gradient->GetValue();

    if (m_ratio_sizer)
        m_ratio_sizer->ShowItems(!m_result.gradient_enabled && num_components() == 2);
    if (m_combo_gradient_dir)
        m_combo_gradient_dir->Show(m_result.gradient_enabled);
    if (m_per_part_gradient_sizer)
        m_per_part_gradient_sizer->ShowItems(m_result.gradient_enabled);
    if (m_curve_sizer)
        m_curve_sizer->ShowItems(m_result.gradient_enabled && num_components() == 2);
    if (!m_result.gradient_enabled) {
        m_result.per_part_gradient = false;
        if (m_chk_per_part_gradient) m_chk_per_part_gradient->SetValue(false);
    }

    // Toggling the curve editor changes the right column height (and width when
    // turning gradient on), so the dialog must follow or the recommendation list
    // gets squeezed off-screen. Same trick as 2-color -> 3-color switching.
    const wxSize new_size = compute_dialog_size();
    if (GetSize() != new_size) {
        const wxRect old_rect = GetRect();
        const wxPoint center(old_rect.x + old_rect.width / 2,
                             old_rect.y + old_rect.height / 2);
        SetSize(new_size);
        SetPosition(wxPoint(center.x - new_size.x / 2,
                            center.y - new_size.y / 2));
    }

    Layout();
    Refresh();
}

void MixedFilamentDialog::on_gradient_direction_changed()
{
    if (!m_combo_gradient_dir) return;
    m_result.gradient_direction = m_combo_gradient_dir->GetSelection();

    // Mirror the user's custom curve around y=0.5 instead of resetting it, so
    // shape work (added anchors, bent segments) survives a direction toggle.
    // reverse() flips y and tangent signs consistently; default two-point
    // linear curves end up matching the new direction exactly (0.9->0.1 <-> 0.1->0.9).
    if (m_curve_editor) {
        m_curve_editor->reverse();
        m_result.gradient_curve = m_curve_editor->get_points();
    }
    update_preview();
}

void MixedFilamentDialog::on_gradient_curve_changed()
{
    if (m_curve_editor)
        m_result.gradient_curve = m_curve_editor->get_points();
    update_preview();
}

void MixedFilamentDialog::on_per_part_gradient_toggled()
{
    if (m_chk_per_part_gradient)
        m_result.per_part_gradient = m_chk_per_part_gradient->GetValue();
}

void MixedFilamentDialog::on_add_material()
{
    size_t n = num_components();
    if (n >= (size_t)MAX_COMPONENTS) return;

    unsigned int new_comp = 0;
    for (size_t j = 0; j < m_physical_names.size(); ++j) {
        unsigned int candidate = (unsigned int)(j + 1);
        bool used = false;
        for (auto c : m_result.components)
            if (c == candidate) { used = true; break; }
        if (!used) { new_comp = candidate; break; }
    }
    if (new_comp == 0) return;
    m_result.components.push_back(new_comp);

    int each = 100 / (int)(n + 1);
    m_result.ratios.clear();
    int assigned = 0;
    for (size_t i = 0; i < n; ++i) {
        m_result.ratios.push_back(each);
        assigned += each;
    }
    m_result.ratios.push_back(100 - assigned);

    if (m_result.ratios.size() >= 3) {
        int sum = 0;
        for (int r : m_result.ratios) sum += r;
        if (sum > 0) {
            m_tri_wx = (double)m_result.ratios[0] / sum;
            m_tri_wy = (double)m_result.ratios[1] / sum;
            m_tri_wz = (double)m_result.ratios[2] / sum;
        }
    }
    reset_manual_ratio_state();

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    wxString lbl_text = wxString::Format(_L("Filament %d"), (int)(n + 1));
    auto* lbl = new wxStaticText(this, wxID_ANY, lbl_text);
    lbl->SetFont(::Label::Body_12);
    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    auto* combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize(FromDIP(166), FromDIP(24)), 0, nullptr, wxCB_READONLY);
    combo->SetKeepDropArrow(true);
    combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_filament_changed(); });
    row->Add(combo, 1, wxALIGN_CENTER_VERTICAL);

    m_combo_filaments.push_back(combo);
    m_combo_to_physical.push_back({});
    m_material_rows_sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(9));

    rebuild_all_combos();
    refresh_curve_editor_colors();
    update_component_count_ui();
    update_preview();
    update_ok_button_state();
    rebuild_recommendation_items();

    Layout();
    Refresh();
}

void MixedFilamentDialog::on_remove_material()
{
    if (num_components() <= 2)
        return;

    m_result.components.resize(2);
    m_result.ratios = {50, 50};
    m_tri_wx = 0.5;
    m_tri_wy = 0.5;
    m_tri_wz = 0.0;

    reset_manual_ratio_state();
    refresh_ratio_labels();

    if (m_material_rows_sizer && m_material_rows_sizer->GetItemCount() > 2) {
        auto* sizer_item = m_material_rows_sizer->GetItem(m_material_rows_sizer->GetItemCount() - 1);
        if (sizer_item && sizer_item->GetSizer())
            sizer_item->GetSizer()->Clear(true);
        m_material_rows_sizer->Remove(m_material_rows_sizer->GetItemCount() - 1);
    }

    if (m_combo_filaments.size() > 2)
        m_combo_filaments.pop_back();
    if (m_combo_to_physical.size() > 2)
        m_combo_to_physical.pop_back();

    rebuild_all_combos();
    refresh_curve_editor_colors();
    update_component_count_ui();
    update_preview();
    update_ok_button_state();
    rebuild_recommendation_items();
    Layout();
    Refresh();
}

void MixedFilamentDialog::on_recommendation_clicked(unsigned int comp_a, unsigned int comp_b)
{
    while (m_material_rows_sizer->GetItemCount() > 2) {
        auto* sizer_item = m_material_rows_sizer->GetItem(m_material_rows_sizer->GetItemCount() - 1);
        if (sizer_item && sizer_item->GetSizer())
            sizer_item->GetSizer()->Clear(true);
        m_material_rows_sizer->Remove(m_material_rows_sizer->GetItemCount() - 1);
    }

    while (m_combo_filaments.size() > 2)
        m_combo_filaments.pop_back();
    while (m_combo_to_physical.size() > 2)
        m_combo_to_physical.pop_back();

    m_result.components = {comp_a, comp_b};
    m_result.ratios = {50, 50};

    reset_manual_ratio_state();
    refresh_ratio_labels();

    rebuild_all_combos();
    refresh_curve_editor_colors();
    update_gradient_direction_items();
    update_component_count_ui();
    update_preview();
    update_ok_button_state();
    Layout();
    Refresh();
}

void MixedFilamentDialog::on_recommendation_clicked_triple(unsigned int a, unsigned int b, unsigned int c)
{
    // Ensure we have exactly 3 combo rows
    if (num_components() < 3) {
        // Need to add a 3rd combo row
        while (m_combo_filaments.size() < 3) {
            size_t idx = m_combo_filaments.size();
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            wxString lbl_text = wxString::Format(_L("Filament %d"), (int)(idx + 1));
            auto* lbl = new wxStaticText(this, wxID_ANY, lbl_text);
            lbl->SetFont(::Label::Body_12);
            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

            auto* combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                       wxSize(FromDIP(166), FromDIP(24)), 0, nullptr, wxCB_READONLY);
            combo->SetKeepDropArrow(true);
            combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_filament_changed(); });
            row->Add(combo, 1, wxALIGN_CENTER_VERTICAL);

            m_combo_filaments.push_back(combo);
            m_combo_to_physical.push_back({});
            m_material_rows_sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(9));
        }
    } else if (num_components() > 3) {
        while (m_material_rows_sizer->GetItemCount() > 3) {
            auto* sizer_item = m_material_rows_sizer->GetItem(m_material_rows_sizer->GetItemCount() - 1);
            if (sizer_item && sizer_item->GetSizer())
                sizer_item->GetSizer()->Clear(true);
            m_material_rows_sizer->Remove(m_material_rows_sizer->GetItemCount() - 1);
        }
        while (m_combo_filaments.size() > 3)
            m_combo_filaments.pop_back();
        while (m_combo_to_physical.size() > 3)
            m_combo_to_physical.pop_back();
    }

    m_result.components = {a, b, c};
    m_result.ratios = {25, 25, 50};
    m_tri_wx = 0.25;
    m_tri_wy = 0.25;
    m_tri_wz = 0.50;
    reset_manual_ratio_state();

    rebuild_all_combos();
    refresh_curve_editor_colors();
    update_gradient_direction_items();
    update_component_count_ui();
    update_preview();
    update_ok_button_state();
    Layout();
    Refresh();
}

void MixedFilamentDialog::update_preview()
{
    if (m_preview_canvas)  m_preview_canvas->Refresh();
    if (m_summary_panel)   m_summary_panel->Refresh();
    if (m_ratio_bar)       m_ratio_bar->Refresh();
    if (m_triangle_panel)  m_triangle_panel->Refresh();
}

void MixedFilamentDialog::paint_warning_panel(wxPaintEvent&)
{
    wxBufferedPaintDC dc(m_warning_panel);
    wxSize sz = m_warning_panel->GetClientSize();

    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

    dc.SetBrush(wxBrush(wxColour(255, 245, 245)));
    dc.SetPen(wxPen(wxColour("#E84C4C"), 1));
    dc.DrawRoundedRectangle(0, 0, sz.GetWidth(), sz.GetHeight(), FromDIP(4));

    int x = FromDIP(10);
    int cy = sz.GetHeight() / 2;

    int icon_r = FromDIP(7);
    dc.SetBrush(wxBrush(wxColour("#E84C4C")));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawCircle(x + icon_r, cy, icon_r);
    dc.SetFont(::Label::Body_10);
    dc.SetTextForeground(*wxWHITE);
    wxSize ex = dc.GetTextExtent(wxT("!"));
    dc.DrawText(wxT("!"), x + icon_r - ex.GetWidth() / 2, cy - ex.GetHeight() / 2);
    x += icon_r * 2 + FromDIP(6);

    if (m_type_mismatch_msg.empty()) return;

    dc.SetFont(::Label::Body_12);
    dc.SetTextForeground(wxColour("#E84C4C"));
    wxString msg = m_type_mismatch_msg;
    int avail_w = sz.GetWidth() - x - FromDIP(10);
    wxSize ts = dc.GetTextExtent(msg);
    if (ts.GetWidth() <= avail_w) {
        dc.DrawText(msg, x, cy - ts.GetHeight() / 2);
    } else {
        wxArrayString lines;
        wxString cur_line;
        wxArrayString words;
        wxStringTokenizer tkz(msg, wxT(" "), wxTOKEN_RET_EMPTY_ALL);
        while (tkz.HasMoreTokens()) words.Add(tkz.GetNextToken());
        if (words.empty()) words.Add(msg);
        for (size_t w = 0; w < words.size(); ++w) {
            wxString test = cur_line.empty() ? words[w] : cur_line + wxT(" ") + words[w];
            if (dc.GetTextExtent(test).GetWidth() > avail_w && !cur_line.empty()) {
                lines.Add(cur_line);
                cur_line = words[w];
            } else {
                cur_line = test;
            }
        }
        if (!cur_line.empty()) lines.Add(cur_line);
        if (lines.empty()) lines.Add(msg);
        int line_h = dc.GetTextExtent(wxT("Mg")).GetHeight();
        int total_h = (int)lines.size() * line_h;
        int y0 = (sz.GetHeight() - total_h) / 2;
        for (size_t l = 0; l < lines.size(); ++l)
            dc.DrawText(lines[l], x, y0 + (int)l * line_h);
    }
}

void MixedFilamentDialog::update_ok_button_state()
{
    if (!m_btn_ok) return;

    bool has_type_mismatch = false;
    if (!m_physical_types.empty() && m_result.components.size() >= 2) {
        std::map<std::string, std::vector<unsigned int>> type_groups;
        for (size_t i = 0; i < m_result.components.size(); ++i) {
            unsigned int phys = m_result.components[i];
            if (phys < 1 || phys > m_physical_types.size()) continue;
            type_groups[m_physical_types[phys - 1]].push_back(phys);
        }
        has_type_mismatch = type_groups.size() > 1;
        if (has_type_mismatch) {
            wxString parts;
            for (auto it = type_groups.begin(); it != type_groups.end(); ++it) {
                if (!parts.empty())
                    parts += _L(" and ");
                wxString slots;
                for (size_t j = 0; j < it->second.size(); ++j) {
                    if (!slots.empty()) slots += ", ";
                    slots += std::to_string(it->second[j]);
                }
                parts += wxString::Format(_L("Slot %s (%s)"), slots, wxString::FromUTF8(it->first));
            }
            m_type_mismatch_msg = parts + " " + _L("cannot be mixed. Please select the same filament type.");
        }
    }

    bool has_unselected = false;
    for (unsigned int c : m_result.components) {
        if (c == 0) { has_unselected = true; break; }
    }

    bool can_confirm = !has_type_mismatch && !has_unselected;
    m_btn_ok->Enable(can_confirm);
    if (has_unselected) {
        m_btn_ok->SetBackgroundColor(wxColour("#CECECE"));
        m_btn_ok->SetBorderColor(wxColour("#CECECE"));
        m_btn_ok->SetToolTip(_L("Please select a filament for all components"));
    } else if (has_type_mismatch) {
        m_btn_ok->SetBackgroundColor(wxColour("#CECECE"));
        m_btn_ok->SetBorderColor(wxColour("#CECECE"));
        m_btn_ok->SetToolTip(_L("Cannot mix different filament types"));
    } else {
        m_btn_ok->SetBackgroundColor(wxColour("#00AE42"));
        m_btn_ok->SetBorderColor(wxColour("#00AE42"));
        m_btn_ok->SetToolTip(wxEmptyString);
    }

    if (m_warning_panel) {
        m_warning_panel->Show(has_type_mismatch);
        Layout();
    }
}

void MixedFilamentDialog::update_gradient_direction_items()
{
    if (!m_combo_gradient_dir) return;

    int prev_sel = m_combo_gradient_dir->GetSelection();
    m_combo_gradient_dir->Clear();

    if (num_components() < 2) return;

    auto make_direction_bitmap = [this](size_t idx_from, size_t idx_to) -> wxBitmap {
        int swatch_sz = FromDIP(20);
        int arrow_w   = FromDIP(16);
        int gap       = FromDIP(4);
        int bmp_w = swatch_sz + gap + arrow_w + gap + swatch_sz;
        int bmp_h = swatch_sz;

        wxColour dir_text = StateColor::darkModeColorFor(wxColour("#262E30"));

        return make_alpha_bitmap(bmp_w, bmp_h, [&](wxDC& dc) {
            dc.SetFont(::Label::Body_13);

            auto draw_swatch = [&](int x, size_t idx) {
                std::string color_hex = "#D9D9D9";
                if (idx < m_physical_colors.size())
                    color_hex = m_physical_colors[idx];
                std::string label = std::to_string(idx + 1);
                wxBitmap* icon = get_extruder_color_icon(color_hex, label, swatch_sz, swatch_sz);
                if (icon && icon->IsOk())
                    dc.DrawBitmap(*icon, x, 0);
            };

            int x = 0;
            draw_swatch(x, idx_from);
            x += swatch_sz + gap;

            dc.SetTextForeground(dir_text);
            wxString arrow = wxT("\u2192");
            wxSize arrow_sz = dc.GetTextExtent(arrow);
            dc.DrawText(arrow, x + (arrow_w - arrow_sz.GetWidth()) / 2,
                               (bmp_h - arrow_sz.GetHeight()) / 2);
            x += arrow_w + gap;

            draw_swatch(x, idx_to);
        });
    };

    size_t idx_a = (comp(0) >= 1) ? comp(0) - 1 : 0;
    size_t idx_b = (comp(1) >= 1) ? comp(1) - 1 : 1;

    m_combo_gradient_dir->Append(wxT(" "), make_direction_bitmap(idx_a, idx_b));
    m_combo_gradient_dir->Append(wxT(" "), make_direction_bitmap(idx_b, idx_a));

    if (prev_sel >= 0 && prev_sel < (int)m_combo_gradient_dir->GetCount())
        m_combo_gradient_dir->SetSelection(prev_sel);
    else
        m_combo_gradient_dir->SetSelection(0);
}

wxSize MixedFilamentDialog::compute_dialog_size() const
{
    const bool is_three      = (num_components() >= 3);
    const bool curve_visible = !is_three && m_result.gradient_enabled;

    int w = FromDIP(439);
    int h = FromDIP(580);
    if (is_three) {
        h = FromDIP(680);
    } else if (curve_visible) {
        // Wider so the gradient editor can show "Material Ratio" intact;
        // +40 over the 3-color height to fit the curve editor while keeping the
        // recommendation list visible (it can still scroll if needed).
        w = FromDIP(470);
        h = FromDIP(720);
    }
    return wxSize(w, h);
}

void MixedFilamentDialog::update_component_count_ui()
{
    bool is_two   = (num_components() == 2);
    bool is_three = (num_components() >= 3);

    // Toggle ratio slider vs triangle picker
    if (m_ratio_sizer)
        m_ratio_sizer->ShowItems(is_two && !m_result.gradient_enabled);
    if (m_triangle_sizer)
        m_triangle_sizer->ShowItems(is_three);

    // 3-color: hide gradient entirely, force off
    if (m_gradient_sizer) {
        bool show_gradient = is_two;
        m_chk_gradient->Show(show_gradient);
        if (m_label_gradient) m_label_gradient->Show(show_gradient);
        m_combo_gradient_dir->Show(show_gradient && m_result.gradient_enabled);
        if (m_per_part_gradient_sizer)
            m_per_part_gradient_sizer->ShowItems(show_gradient && m_result.gradient_enabled);
        if (m_curve_sizer)
            m_curve_sizer->ShowItems(show_gradient && m_result.gradient_enabled);
    }
    if (is_three) {
        m_result.gradient_enabled = false;
        if (m_chk_gradient) m_chk_gradient->SetValue(false);
        m_result.per_part_gradient = false;
        if (m_chk_per_part_gradient) m_chk_per_part_gradient->SetValue(false);
    }

    if (m_btn_add_material) {
        bool can_add = (num_components() < (size_t)MAX_COMPONENTS && m_physical_colors.size() > num_components());
        m_btn_add_material->Enable(can_add);
        if (can_add) {
            m_btn_add_material->SetTextColor(wxColour("#262E30"));
            m_btn_add_material->SetBorderColor(wxColour("#EEEEEE"));
            m_btn_add_material->SetToolTip(wxEmptyString);
        } else {
            m_btn_add_material->SetTextColor(wxColour("#CECECE"));
            m_btn_add_material->SetBorderColor(wxColour("#EEEEEE"));
            m_btn_add_material->SetToolTip(is_three ? _L("Maximum 3 materials for mixing") : _L("Maximum number of components reached"));
        }
    }

    if (m_btn_remove_material) {
        m_btn_remove_material->Show(is_three);
        m_btn_remove_material->Enable(is_three);
        m_btn_remove_material->SetToolTip(is_three ? _L("Remove the third material") : wxString());
    }

    const wxSize new_size = compute_dialog_size();
    const wxRect old_rect = GetRect();
    const wxPoint center(old_rect.x + old_rect.width / 2,
                         old_rect.y + old_rect.height / 2);
    SetSize(new_size);
    SetPosition(wxPoint(center.x - new_size.x / 2,
                        center.y - new_size.y / 2));
    Layout();
}

} // namespace GUI
} // namespace Slic3r

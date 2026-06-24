#include "MixedFilamentDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "MixedGradientSelector.hpp"
#include "MixedColorMatchPanel.hpp"
#include "MixedFilamentBadge.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/StaticBox.hpp"

#include <wx/dcbuffer.h>
#include <wx/statline.h>
#include <wx/sizer.h>
#include <wx/bmpbuttn.h>
#include <wx/clrpicker.h>
#include <wx/colordlg.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <numeric>
#include <utility>
#include <set>
#include <sstream>

namespace Slic3r { namespace GUI {

static constexpr int SWATCH_SIZE  = 16;
static constexpr int PREVIEW_SIZE = 140;
static constexpr int STRIP_HEIGHT = 24;

// ---------------------------------------------------------------------------
// Custom Figma-style range slider for match mode "Min mix ratio"
// ---------------------------------------------------------------------------
class MatchRangeSlider : public wxPanel
{
public:
    MatchRangeSlider(wxWindow* parent, int value, int minVal, int maxVal)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
        , m_value(value), m_min(minVal), m_max(maxVal)
    {
        SetInitialSize(wxSize(FromDIP(110), FromDIP(22)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        Bind(wxEVT_PAINT, &MatchRangeSlider::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &MatchRangeSlider::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &MatchRangeSlider::on_left_up, this);
        Bind(wxEVT_MOTION, &MatchRangeSlider::on_motion, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &MatchRangeSlider::on_capture_lost, this);
    }

    int value() const { return m_value; }
    void set_value(int v) { m_value = std::clamp(v, m_min, m_max); Refresh(); }

private:
    int m_value, m_min, m_max;
    bool m_dragging = false;

    int value_from_x(int x) const {
        int tw = std::max(4, GetClientSize().x - FromDIP(10));
        int tx = FromDIP(5);
        float frac = (float)(x - tx) / (float)tw;
        return m_min + (int)(frac * (m_max - m_min) + 0.5f);
    }

    void fire_event() {
        wxCommandEvent evt(wxEVT_SLIDER, GetId());
        evt.SetEventObject(this);
        ProcessWindowEvent(evt);
    }

    void on_paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        wxSize sz = GetClientSize();
        // Erase entire background to prevent ghosting
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);

        int track_h = FromDIP(3);
        int track_y = (sz.y - track_h) / 2;
        int thumb_d = FromDIP(10);
        int track_x = FromDIP(5);
        int track_w = std::max(4, sz.x - thumb_d);

        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#EBEBEB"))));
        dc.DrawRoundedRectangle(track_x, track_y, track_w, track_h, FromDIP(2));

        float frac = (float)(m_value - m_min) / (float)std::max(1, m_max - m_min);
        int active_w = std::max(0, (int)(track_w * frac));
        if (active_w > 0) {
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#009688"))));
            dc.DrawRoundedRectangle(track_x, track_y, active_w, track_h, FromDIP(2));
        }

        int thumb_x = track_x + active_w - thumb_d / 2;
        thumb_x = std::clamp(thumb_x, track_x - thumb_d/2, track_x + track_w - thumb_d/2);
        int thumb_y = (sz.y - thumb_d) / 2;
        dc.SetBrush(wxBrush(wxColour("#FFFFFF")));
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#009688")), FromDIP(1)));
        dc.DrawEllipse(thumb_x, thumb_y, thumb_d, thumb_d);
    }

    void on_left_down(wxMouseEvent& e) {
        m_dragging = true;
        CaptureMouse();
        int v = value_from_x(e.GetX());
        v = std::clamp(v, m_min, m_max);
        if (v != m_value) { m_value = v; Refresh(); fire_event(); }
    }

    void on_left_up(wxMouseEvent&) {
        if (m_dragging) { m_dragging = false; if (HasCapture()) ReleaseMouse(); }
    }

    void on_motion(wxMouseEvent& e) {
        if (!m_dragging) return;
        int v = value_from_x(e.GetX());
        v = std::clamp(v, m_min, m_max);
        if (v != m_value) { m_value = v; Refresh(); fire_event(); }
    }

    void on_capture_lost(wxMouseCaptureLostEvent&) { m_dragging = false; }
};

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

MixedFilamentDialog::MixedFilamentDialog(wxWindow* parent,
                                     const std::vector<std::string>& filament_colours)
    : DPIDialog(parent, wxID_ANY, _L("Add Mix"),
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_filament_colours(filament_colours)
{
    m_result.component_a   = 1;
    m_result.component_b   = 2;
    m_result.mix_b_percent = 50;
    build_ui();
}

MixedFilamentDialog::MixedFilamentDialog(wxWindow* parent,
                                     const std::vector<std::string>& filament_colours,
                                     const Slic3r::MixedFilament& existing)
    : DPIDialog(parent, wxID_ANY, _L("Edit Mix"),
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_filament_colours(filament_colours)
    , m_result(existing)
{
    // Use saved UI mode when available (new format with cm token).
    // Fall back to heuristics for legacy rows without cm token.
    if (existing.ui_mode >= 0 && existing.ui_mode <= 3) {
        m_current_mode = existing.ui_mode;
    } else if (existing.gradient_enabled) {
        m_current_mode = MODE_GRADIENT;
    } else if (!MixedFilamentManager::normalize_manual_pattern(existing.manual_pattern).empty()) {
        m_current_mode = MODE_CYCLE;
    } else if (!existing.gradient_component_ids.empty() &&
             existing.distribution_mode == int(MixedFilament::Simple)) {
        m_current_mode = MODE_MATCH;
    } else if (!existing.gradient_component_ids.empty() &&
             existing.distribution_mode == int(MixedFilament::LayerCycle)) {
        // Legacy heuristic: distinguish match 3-color from ratio 3-color.
        // Fall back to MODE_MATCH when ratio_a/ratio_b are both ≤ 1.
        if (existing.ratio_a <= 1 && existing.ratio_b <= 1)
            m_current_mode = MODE_MATCH;
        else
            m_current_mode = MODE_RATIO;
    } else {
        m_current_mode = MODE_RATIO;
    }

    // Determine gradient direction from existing configuration
    // Direction 0: A→B (component_a starts dominant, transitions to component_b)
    //              Implemented as: gradient_start > gradient_end (e.g., 0.8 → 0.2)
    // Direction 1: B→A (component_b starts dominant, transitions to component_a)
    //              Implemented as: gradient_start < gradient_end (e.g., 0.2 → 0.8)
    m_gradient_direction = (existing.gradient_start >= existing.gradient_end) ? 0 : 1;

    // Restore tri-picker weights from slash-separated weight string
    if (m_current_mode == MODE_RATIO && !existing.gradient_component_weights.empty()) {
        std::vector<int> vals;
        const char *p = existing.gradient_component_weights.c_str();
        while (*p) {
            char *end;
            int v = (int)std::strtol(p, &end, 10);
            if (end == p) break;
            vals.push_back(v);
            p = end;
            if (*p == '/') ++p;
        }
        if (vals.size() >= 3) {
            int total = 0;
            for (int v : vals) total += v;
            if (total > 0) {
                m_tri_wx = vals[0] / (double)total;
                m_tri_wy = vals[1] / (double)total;
                m_tri_wz = vals[2] / (double)total;
                // Clamp each weight to be at least 10% and at most 90%
                constexpr double MIN_WEIGHT = 0.10;
                constexpr double MAX_WEIGHT = 0.90;
                m_tri_wx = std::clamp(m_tri_wx, MIN_WEIGHT, MAX_WEIGHT);
                m_tri_wy = std::clamp(m_tri_wy, MIN_WEIGHT, MAX_WEIGHT);
                m_tri_wz = std::clamp(m_tri_wz, MIN_WEIGHT, MAX_WEIGHT);
                // Renormalize after clamping
                double sum = m_tri_wx + m_tri_wy + m_tri_wz;
                if (sum > 0) { m_tri_wx /= sum; m_tri_wy /= sum; m_tri_wz /= sum; }
            }
        }
    }

    build_ui();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

wxBitmap MixedFilamentDialog::make_color_bitmap(const wxColour& c, int size)
{
    wxBitmap bmp(size, size);
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(c));
    dc.Clear();
    return bmp;
}

int MixedFilamentDialog::max_filaments_for_mode(int mode) const
{
    if (mode == MODE_RATIO)    return 3;
    if (mode == MODE_GRADIENT) return 2;
    return 4;
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void MixedFilamentDialog::build_ui()
{
    m_mode_btn_selected = m_current_mode;
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));

    auto* top_sizer = new wxBoxSizer(wxVERTICAL);
    const int M = FromDIP(8);
    const int V_GAP = FromDIP(16);

    // ---- Segmented mode buttons (Figma: white 380×60 panel, inner #F8F7F7 segment bg, tabs 80×28, 4px gap) ----
    {
        // Outer white panel (#FFFFFF bg, bottom border #F0F0F0, px=20 py=12)
        auto* mode_outer = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        mode_outer->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        auto* mode_outer_sizer = new wxBoxSizer(wxVERTICAL);

        // Inner segment container (#F8F7F7 bg, 36px height, 4px radius)
        m_mode_btn_container = new StaticBox(mode_outer, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_mode_btn_container->SetCornerRadius(FromDIP(4));
        m_mode_btn_container->SetBorderWidth(0);
        m_mode_btn_container->SetMinSize(wxSize(-1, FromDIP(36)));
        m_mode_btn_container->SetBackgroundColor(
            StateColor(std::pair(wxColour("#F8F7F7"), (int)StateColor::Normal)));
        auto* mode_sizer = new wxBoxSizer(wxHORIZONTAL);
        const std::vector<wxString> mode_names = { _L("Ratio"), _L("Cycle"), _L("Match"), _L("Gradient") };

        // Unselected: transparent (no bg) — container's #F8F7F7 shows through
        auto seg_bg  = StateColor();
        auto seg_fg  = StateColor(std::pair(wxColour("#4A4A4A"), (int)StateColor::Normal));
        auto sel_bg  = StateColor(std::pair(wxColour("#009688"), (int)StateColor::Normal));
        auto sel_fg  = StateColor(std::pair(wxColour("#FEFEFE"), (int)StateColor::Normal));

        for (int i = 0; i < 4; ++i) {
            auto* btn = new Button(m_mode_btn_container, mode_names[i]);
            btn->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
            btn->SetPaddingSize(wxSize(FromDIP(6), FromDIP(2)));
            btn->SetCornerRadius(FromDIP(4));
            btn->SetBorderWidth(0);
            btn->SetFont(Label::Body_12);
            if (i == (int)m_current_mode) {
                btn->SetBackgroundColor(sel_bg);
                btn->SetTextColor(sel_fg);
                btn->SetCanFocus(false);
            } else {
                btn->SetBackgroundColor(seg_bg);
                btn->SetTextColor(seg_fg);
            }
            btn->Bind(wxEVT_BUTTON, [this, i, seg_bg, seg_fg, sel_bg, sel_fg](wxCommandEvent&) {
                if (i == m_mode_btn_selected) return;
                if (m_mode_btn_selected >= 0 && m_mode_btn_selected < (int)m_mode_buttons.size()) {
                    m_mode_buttons[m_mode_btn_selected]->SetBackgroundColor(seg_bg);
                    m_mode_buttons[m_mode_btn_selected]->SetTextColor(seg_fg);
                    m_mode_buttons[m_mode_btn_selected]->SetCanFocus(true);
                }
                m_mode_btn_selected = i;
                m_mode_buttons[i]->SetBackgroundColor(sel_bg);
                m_mode_buttons[i]->SetTextColor(sel_fg);
                m_mode_buttons[i]->SetCanFocus(false);
                on_mode_changed(i);
            });
            m_mode_buttons.push_back(btn);
            mode_sizer->Add(btn, 1, wxEXPAND | wxALL, FromDIP(4));
        }
        m_mode_btn_container->SetSizer(mode_sizer);

        mode_outer_sizer->Add(m_mode_btn_container, 1, wxEXPAND | wxALL, FromDIP(12));
        mode_outer->SetSizer(mode_outer_sizer);

        // Bottom divider (Figma: #F0F0F0, 1px)
        auto* mode_divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        mode_divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));

        top_sizer->Add(mode_outer, 0, wxEXPAND);
        top_sizer->Add(mode_divider, 0, wxEXPAND);
    }

    // Add/remove & swap buttons are created inside Card A (see filament card block below)

    // Error banner (red, blocks confirm)
    {
        m_error_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL);
        m_error_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FDE8E8")));
        m_error_panel->Hide();
        auto* err_sizer = new wxBoxSizer(wxHORIZONTAL);
        ScalableBitmap error_bmp(m_error_panel, "error_icon_red_exclamation", 14);
        auto* error_icon = new wxStaticBitmap(m_error_panel, wxID_ANY, error_bmp.bmp());
        err_sizer->Add(error_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        err_sizer->AddSpacer(FromDIP(4));
        m_error_text = new Label(m_error_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
        m_error_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#D32F2F")));
        err_sizer->Add(m_error_text, 1, wxALL, FromDIP(8));
        m_error_panel->SetSizer(err_sizer);
        top_sizer->Add(m_error_panel, 0, wxEXPAND);
    }

    // Warning banner (orange, advisory, confirm stays enabled)
    {
        m_warning_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL);
        m_warning_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFF3EB")));
        m_warning_panel->Hide();
        auto* warn_sizer = new wxBoxSizer(wxHORIZONTAL);
        ScalableBitmap warn_bmp(m_warning_panel, "icon_warning_triangle", 14);
        auto* warn_icon = new wxStaticBitmap(m_warning_panel, wxID_ANY, warn_bmp.bmp());
        warn_sizer->Add(warn_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        warn_sizer->AddSpacer(FromDIP(4));
        m_warning_text = new Label(m_warning_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
        m_warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF842D")));
        warn_sizer->Add(m_warning_text, 1, wxALL, FromDIP(8));
        m_warning_panel->SetSizer(warn_sizer);
        top_sizer->Add(m_warning_panel, 0, wxEXPAND);
    }
    // ---- Scrolled content area ----
    m_scrolled_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition,
                                               wxSize(FromDIP(380), FromDIP(400)),
                                               wxVSCROLL | wxBORDER_NONE);
    m_scrolled_content->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));
    m_scrolled_content->SetScrollRate(0, FromDIP(8));
    // Prevent wxScrolledWindow from auto-scrolling to focused children.
    // Without this, clicking a partially-visible widget first scrolls it
    // into view instead of handling the click, requiring a second click.
    m_scrolled_content->Bind(wxEVT_CHILD_FOCUS, [](wxChildFocusEvent&) {
        // Do not evt.Skip() — that would invoke the default handler which scrolls.
    });
    auto* scroll_sizer = new wxBoxSizer(wxVERTICAL);
    scroll_sizer->AddSpacer(FromDIP(16));

    // ======== Card 1: Match Input (match mode: filament badges + target color) ========
    {
        m_match_input_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize, wxBORDER_NONE);
        m_match_input_card->SetCornerRadius(FromDIP(4));
        m_match_input_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_match_input_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_match_input_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_match_input_card->SetBorderWidth(FromDIP(1));
        m_match_input_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        auto* card1_sizer = new wxBoxSizer(wxVERTICAL);

        auto* filament_label = new wxStaticText(m_match_input_card, wxID_ANY, _L("Filaments"));
        filament_label->SetFont(Label::Body_14);
        filament_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        filament_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        card1_sizer->Add(filament_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
        card1_sizer->AddSpacer(FromDIP(12));

        m_match_badges_panel = new wxPanel(m_match_input_card, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize, wxBORDER_NONE);
        m_match_badges_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_match_badges_sizer = new wxWrapSizer(wxHORIZONTAL);
        MixedFilamentDisplayContext ctx;
        ctx.num_physical = m_filament_colours.size();
        ctx.physical_colors = m_filament_colours;
        for (int fid = 0; fid < (int)m_filament_colours.size(); ++fid) {
            MixedFilament mf;
            mf.display_color = m_filament_colours[fid];
            mf.custom = true;
            auto* badge = new MixedFilamentBadge(m_match_badges_panel, wxID_ANY, fid + 1,
                                                  mf, ctx, true, 20);
            badge->SetCanFocus(false);
            m_match_badges_sizer->Add(badge, 0, wxRIGHT | wxBOTTOM, FromDIP(12));
        }
        m_match_badges_panel->SetSizer(m_match_badges_sizer);
        card1_sizer->Add(m_match_badges_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        // Target Color label
        auto* target_label = new wxStaticText(m_match_input_card, wxID_ANY, _L("Target Color"));
        target_label->SetFont(Label::Body_14);
        target_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        target_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        card1_sizer->Add(target_label, 0, wxLEFT | wxRIGHT, FromDIP(16));
        card1_sizer->AddSpacer(FromDIP(12));

        wxColour default_target("#26A69A");

        // Color panel + Hex input
        auto* target_row = new wxBoxSizer(wxHORIZONTAL);
        m_match_target_picker = new wxPanel(m_match_input_card, wxID_ANY, wxDefaultPosition,
                                             wxSize(FromDIP(108), FromDIP(24)));
        m_match_target_picker->SetBackgroundColour(default_target);
        m_match_target_picker->SetMinSize(wxSize(FromDIP(108), FromDIP(24)));
        m_match_target_picker->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            wxColourData data;
            data.SetColour(m_match_target_picker->GetBackgroundColour());
            wxColourDialog dlg(this, &data);
            if (dlg.ShowModal() == wxID_OK) {
                wxColour c = dlg.GetColourData().GetColour();
                if (c.IsOk()) {
                    m_match_target_picker->SetBackgroundColour(c);
                    m_match_target_picker->Refresh();
                    if (m_match_hex_input)
                        m_match_hex_input->ChangeValue(c.GetAsString(wxC2S_HTML_SYNTAX).Mid(1));
                    if (m_match_panel) m_match_panel->set_target_color(c);
                    if (m_match_hex_error) {
                        m_match_hex_error = false;
                        if (m_match_hex_wrapper) m_match_hex_wrapper->Refresh();
                        if (m_error_panel) m_error_panel->Hide();
                        if (m_btn_confirm) m_btn_confirm->Enable();
                        Layout();
                    }
                }
            }
        });
        target_row->Add(m_match_target_picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

        auto* hex_wrapper = new wxPanel(m_match_input_card, wxID_ANY, wxDefaultPosition,
                                         wxSize(FromDIP(109), FromDIP(24)));
        m_match_hex_wrapper = hex_wrapper;
        hex_wrapper->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        hex_wrapper->SetMinSize(wxSize(FromDIP(109), FromDIP(24)));
        hex_wrapper->SetBackgroundStyle(wxBG_STYLE_PAINT);
        hex_wrapper->Bind(wxEVT_PAINT, [this](wxPaintEvent& evt) {
            wxWindow* w = dynamic_cast<wxWindow*>(evt.GetEventObject());
            if (!w) return;
            wxPaintDC dc(w);
            wxSize sz = w->GetClientSize();
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(m_match_hex_error ? StateColor::darkModeColorFor(wxColour("#FF0000")) : StateColor::darkModeColorFor(wxColour("#009688")), 1));
            dc.DrawRectangle(0, 0, sz.x, sz.y);
        });
        auto* hex_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto* hex_label = new wxStaticText(hex_wrapper, wxID_ANY, "Hex:");
        hex_label->SetFont(Label::Body_12);
        hex_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
        hex_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        hex_sizer->Add(hex_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(9));
        hex_sizer->AddSpacer(FromDIP(4));

        auto* hash_label = new wxStaticText(hex_wrapper, wxID_ANY, "#");
        hash_label->SetFont(Label::Body_12);
        hash_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
        hash_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        hex_sizer->Add(hash_label, 0, wxALIGN_CENTER_VERTICAL);
        hex_sizer->AddSpacer(FromDIP(2));

        m_match_hex_input = new wxTextCtrl(hex_wrapper, wxID_ANY,
                                           default_target.GetAsString(wxC2S_HTML_SYNTAX).Mid(1),
                                           wxDefaultPosition, wxSize(FromDIP(52), -1),
                                           wxTE_PROCESS_ENTER | wxBORDER_NONE);
        m_match_hex_input->SetFont(Label::Body_12);
        m_match_hex_input->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        m_match_hex_input->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_match_hex_input->SetMaxLength(6);
        // Prevent wxEVT_TEXT_ENTER from propagating to dialog default button
        m_match_hex_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
            wxString val = m_match_hex_input->GetValue();
            val.Trim(true).Trim(false);
            wxColour parsed;
            if (try_parse_color_match_hex(val, parsed)) {
                m_match_hex_error = false;
                if (m_match_hex_wrapper) m_match_hex_wrapper->Refresh();
                if (m_error_panel) m_error_panel->Hide();
                if (m_btn_confirm) m_btn_confirm->Enable();
                Layout();
                if (m_match_target_picker) { m_match_target_picker->SetBackgroundColour(parsed); m_match_target_picker->Refresh(); }
                if (m_match_panel) m_match_panel->set_target_color(parsed);
            } else {
                m_match_hex_error = true;
                if (m_match_hex_wrapper) m_match_hex_wrapper->Refresh();
                set_error(_L("Please enter a valid 6-digit Hex value."));
            }
        });
        m_match_hex_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (m_match_hex_error) {
                m_match_hex_error = false;
                if (m_match_hex_wrapper) m_match_hex_wrapper->Refresh();
                if (m_error_panel) m_error_panel->Hide();
                if (m_btn_confirm) m_btn_confirm->Enable();
                Layout();
            }
        });
        hex_sizer->Add(m_match_hex_input, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(9));
        hex_wrapper->SetSizer(hex_sizer);
        target_row->Add(hex_wrapper, 0, wxALIGN_CENTER_VERTICAL);

        card1_sizer->Add(target_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        m_match_input_card->SetSizer(card1_sizer);
        scroll_sizer->Add(m_match_input_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }
    // ======== Card A: Filament Selection ========
    {
        m_filament_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, wxBORDER_NONE);
        m_filament_card->SetCornerRadius(FromDIP(4));
        m_filament_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_filament_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_filament_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_filament_card->SetBorderWidth(FromDIP(1));
        m_filament_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_filament_card_sizer = new wxBoxSizer(wxVERTICAL);

        // Title row with add/remove/swap buttons
        auto* card_title_row = new wxBoxSizer(wxHORIZONTAL);
        m_filament_card_title = new wxStaticText(m_filament_card, wxID_ANY, _L("Filament Selection"));
        m_filament_card_title->SetFont(Label::Body_14);
        m_filament_card_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        m_filament_card_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        card_title_row->Add(m_filament_card_title, 1, wxALIGN_CENTER_VERTICAL);

        m_btn_swap_gradient_dir = new ScalableButton(m_filament_card, wxID_ANY, "reverse_arrow");
        m_btn_swap_gradient_dir->SetToolTip(_L("Swap filaments"));
        m_btn_swap_gradient_dir->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));
        m_btn_swap_gradient_dir->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_filament_rows.size() < 2) return;
            std::vector<int> selections;
            for (int i = 0; i < m_filament_rows.size(); ++i)
                selections.push_back(get_filament_index((int)i));
            std::swap(selections[0], selections[1]);
            m_gradient_direction = 0;
            rebuild_all_combos_with_selections(selections);
            update_preview();
            update_compatibility_warning();
        });
        card_title_row->Add(m_btn_swap_gradient_dir, 0, wxALIGN_CENTER_VERTICAL);

        // Remove filament button
        m_btn_remove_filament = new ScalableButton(m_filament_card, wxID_ANY, "icon_minus");
        m_btn_remove_filament->SetToolTip(_L("Remove last filament"));
        m_btn_remove_filament->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            sync_rows_to_result();
            int new_count = std::max(2, (int)m_filament_rows.size() - 1);
            if (new_count == 2) {
                m_tri_wx = 1.0/3.0; m_tri_wy = 1.0/3.0; m_tri_wz = 1.0/3.0;
            }
            resize_gradient_ids(new_count);
            wxWeakRef<wxWindow> weak_self(this);
            CallAfter([weak_self]() {
                if (!weak_self) return;
                auto* self = static_cast<MixedFilamentDialog*>(weak_self.get());
                self->rebuild_filament_rows();
                self->update_compatibility_warning();
                self->Layout(); self->Fit();
            });
        });
        card_title_row->Add(m_btn_remove_filament, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        // Add filament button
        m_btn_add_filament = new ScalableButton(m_filament_card, wxID_ANY, "icon_plus");
        m_btn_add_filament->SetToolTip(_L("Add one filament"));
        m_btn_add_filament->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            sync_rows_to_result();
            resize_gradient_ids((int)m_filament_rows.size() + 1);
            wxWeakRef<wxWindow> weak_self(this);
            CallAfter([weak_self]() {
                if (!weak_self) return;
                auto* self = static_cast<MixedFilamentDialog*>(weak_self.get());
                self->rebuild_filament_rows();
                self->update_compatibility_warning();
                self->Layout(); self->Fit();
            });
        });
        card_title_row->Add(m_btn_add_filament, 0, wxALIGN_CENTER_VERTICAL);
        m_filament_card_sizer->Add(card_title_row, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(16));

        m_filament_card_sizer->AddSpacer(FromDIP(12));

        // Filament rows panel
        m_filament_rows_panel = new wxPanel(m_filament_card);
        m_filament_rows_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_filament_rows_sizer = new wxBoxSizer(wxVERTICAL);
        m_filament_rows_panel->SetSizer(m_filament_rows_sizer);
        m_filament_card_sizer->Add(m_filament_rows_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        m_filament_card->SetSizer(m_filament_card_sizer);
        m_filament_card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) { this->SetFocus(); evt.Skip(); });
        scroll_sizer->Add(m_filament_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ======== Card B: Mix Ratio (Ratio mode) ========
    {
        m_ratio_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, wxBORDER_NONE);
        m_ratio_card->SetCornerRadius(FromDIP(4));
        m_ratio_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_ratio_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_ratio_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_ratio_card->SetBorderWidth(FromDIP(1));
        m_ratio_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_ratio_card_sizer = new wxBoxSizer(wxVERTICAL);

        // Title
        auto* ratio_title = new wxStaticText(m_ratio_card, wxID_ANY, _L("Mixing Ratio"));
        ratio_title->SetFont(Label::Body_14);
        ratio_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        ratio_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_ratio_card_sizer->Add(ratio_title, 0, wxTOP | wxLEFT | wxRIGHT, FromDIP(16));

        // Compute initial values
        int initial_val = m_result.mix_b_percent;
        wxColour col_a = (!m_filament_colours.empty())
            ? parse_mixed_color(m_filament_colours[std::max(0, (int)m_result.component_a - 1)])
            : wxColour(128, 128, 128);
        wxColour col_b = (m_filament_colours.size() > 1)
            ? parse_mixed_color(m_filament_colours[std::max(0, (int)m_result.component_b - 1)])
            : col_a;

        // ---- Gradient bar (2-color) ----
        m_ratio_gradient_spacer = m_ratio_card_sizer->AddSpacer(FromDIP(16));
        m_gradient_selector = new MixedGradientSelector(m_ratio_card, col_a, col_b, initial_val);
        m_ratio_card_sizer->Add(m_gradient_selector, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        // ---- Tri picker (3-color) ----
        m_ratio_tri_spacer = m_ratio_card_sizer->AddSpacer(FromDIP(12));
        build_tri_picker(m_ratio_card);
        m_ratio_card_sizer->Add(m_tri_picker, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT, FromDIP(16));

        // ---- Legend panel (dynamic swatches + percentage labels) ----
        m_ratio_card_sizer->AddSpacer(FromDIP(6));
        m_legend_panel = new wxPanel(m_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_legend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_legend_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_legend_panel->SetSizer(m_legend_sizer);
        m_ratio_card_sizer->Add(m_legend_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        // ---- Divider ----
        {
            m_ratio_card_sizer->AddSpacer(FromDIP(6));
            auto* divider = new wxPanel(m_ratio_card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
            m_ratio_card_sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
            m_ratio_card_sizer->AddSpacer(FromDIP(6));
        }

        // ---- Strip preview panel ----
        m_strip_panel = new wxPanel(m_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_strip_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(128)));
        m_strip_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_strip_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_strip_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_strip_panel);
            draw_strip(dc, m_strip_panel);
        });

        // ---- Dual-column preview: stripe (left) + blend (right) ----
        {
            auto* dual_preview_row = new wxBoxSizer(wxHORIZONTAL);

            // Left: preview label + stripe
            auto* left_col = new wxBoxSizer(wxVERTICAL);
            auto* preview_lbl = new wxStaticText(m_ratio_card, wxID_ANY, _L("Preview"));
            preview_lbl->SetFont(Label::Body_12);
            preview_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            preview_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            left_col->Add(preview_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));
            left_col->Add(m_strip_panel, 1, wxEXPAND);
            dual_preview_row->Add(left_col, 1, wxEXPAND | wxRIGHT, FromDIP(8));

            // Right: blend result label + blend color panel
            auto* right_col = new wxBoxSizer(wxVERTICAL);
            auto* blend_lbl = new wxStaticText(m_ratio_card, wxID_ANY, _L("Mix Effect"));
            blend_lbl->SetFont(Label::Body_12);
            blend_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            blend_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            right_col->Add(blend_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));
            m_preview_blend_panel = new wxPanel(m_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
            m_preview_blend_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(128)));
            m_preview_blend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_preview_blend_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_preview_blend_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(m_preview_blend_panel);
                dc.SetBackground(wxBrush(parse_mixed_color(compute_preview_color())));
                dc.Clear();
                wxSize sz = m_preview_blend_panel->GetClientSize();
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(180, 180, 180)), 1));
                dc.DrawRectangle(0, 0, sz.x, sz.y);
            });
            right_col->Add(m_preview_blend_panel, 1, wxEXPAND);
            dual_preview_row->Add(right_col, 1, wxEXPAND);

            m_ratio_card_sizer->Add(dual_preview_row, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        }

        m_ratio_card->SetSizer(m_ratio_card_sizer);
        m_ratio_card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) { this->SetFocus(); evt.Skip(); });
        scroll_sizer->Add(m_ratio_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ======== Match Mix Ratio Card (match mode's own card) ========
    {
        m_match_ratio_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize, wxBORDER_NONE);
        m_match_ratio_card->SetCornerRadius(FromDIP(4));
        m_match_ratio_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_match_ratio_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_match_ratio_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_match_ratio_card->SetBorderWidth(FromDIP(1));
        m_match_ratio_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_match_ratio_card_sizer = new wxBoxSizer(wxVERTICAL);

        // Title: 混合比例 (Figma: 14px Medium)
        auto* match_title = new wxStaticText(m_match_ratio_card, wxID_ANY, _L("Mixing Ratio"));
        match_title->SetFont(Label::Body_14);
        match_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        match_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_match_ratio_card_sizer->Add(match_title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        wxColour def_ca = (!m_filament_colours.empty())
            ? parse_mixed_color(m_filament_colours[0]) : wxColour(128,128,128);
        wxColour def_cb = (m_filament_colours.size() > 1)
            ? parse_mixed_color(m_filament_colours[1]) : def_ca;
        m_match_gradient_selector = new MixedGradientSelector(m_match_ratio_card, def_ca, def_cb, 50);
        m_match_gradient_selector->set_min_max(m_match_min_pct, 100 - m_match_min_pct);
        m_match_gradient_spacer = m_match_ratio_card_sizer->AddSpacer(FromDIP(16));
        m_match_ratio_card_sizer->Add(m_match_gradient_selector, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        m_match_tri_spacer = m_match_ratio_card_sizer->AddSpacer(FromDIP(12));
        build_match_tri_picker(m_match_ratio_card);
        m_match_ratio_card_sizer->Add(m_match_tri_picker, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT, FromDIP(16));

        // Legend panel (gap-[6px] below tri-picker per Figma)
        m_match_ratio_card_sizer->AddSpacer(FromDIP(6));
        m_match_legend_panel = new wxPanel(m_match_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_match_legend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_match_legend_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_match_legend_panel->SetSizer(m_match_legend_sizer);
        m_match_ratio_card_sizer->Add(m_match_legend_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        // Range slider row: 最低混色比例
        {
            m_match_range_row = new wxPanel(m_match_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
            m_match_range_row->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            auto* range_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto* range_label = new wxStaticText(m_match_range_row, wxID_ANY, _L("Min Mix Ratio"));
            range_label->SetFont(Label::Body_14);
            range_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            range_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            range_sizer->Add(range_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
            m_match_range_slider = new MatchRangeSlider(m_match_range_row, m_match_min_pct, 0, 50);
            range_sizer->Add(m_match_range_slider, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            m_match_range_value = new wxStaticText(m_match_range_row, wxID_ANY,
                                                    wxString::Format("%d%%", m_match_min_pct));
            m_match_range_value->SetFont(Label::Body_12);
            m_match_range_value->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            m_match_range_value->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            range_sizer->Add(m_match_range_value, 0, wxALIGN_CENTER_VERTICAL);
            range_sizer->AddStretchSpacer();
            m_match_range_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
                const int new_min = std::clamp(m_match_range_slider->value(), 0, 50);
                m_match_range_value->SetLabel(wxString::Format("%d%%", new_min));
                if (m_match_gradient_selector)
                    m_match_gradient_selector->set_min_max(new_min, 100 - new_min);
                bool visibility_changed = false;
                for (int w : m_swatch_min_weights) {
                    if ((w >= m_match_min_pct) != (w >= new_min)) {
                        visibility_changed = true;
                        break;
                    }
                }
                m_match_min_pct = new_min;
                if (visibility_changed)
                    rebuild_swatch_sizer();
                if (m_match_panel)
                    m_match_panel->set_min_component_percent(new_min);
            });
            m_match_range_row->SetSizer(range_sizer);
            m_match_ratio_card_sizer->Add(m_match_range_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
        }

        // Divider
        {
            m_match_ratio_card_sizer->AddSpacer(FromDIP(16));
            auto* divider = new wxPanel(m_match_ratio_card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
            m_match_ratio_card_sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
            m_match_ratio_card_sizer->AddSpacer(FromDIP(6));
        }

        // Preview section
        m_match_strip_panel = new wxPanel(m_match_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_match_strip_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(128)));
        m_match_strip_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_match_strip_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_match_strip_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_match_strip_panel);
            draw_strip(dc, m_match_strip_panel);
        });

        {
            auto* dual_preview_row = new wxBoxSizer(wxHORIZONTAL);
            auto* left_col = new wxBoxSizer(wxVERTICAL);
            auto* preview_lbl = new wxStaticText(m_match_ratio_card, wxID_ANY, _L("Preview"));
            preview_lbl->SetFont(Label::Body_12);
            preview_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            preview_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            left_col->Add(preview_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));
            left_col->Add(m_match_strip_panel, 0, wxEXPAND);
            dual_preview_row->Add(left_col, 0, wxEXPAND | wxRIGHT, FromDIP(8));

            auto* right_col = new wxBoxSizer(wxVERTICAL);
            auto* blend_lbl = new wxStaticText(m_match_ratio_card, wxID_ANY, _L("Mix Effect"));
            blend_lbl->SetFont(Label::Body_12);
            blend_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            blend_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            right_col->Add(blend_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));
            m_match_blend_panel = new wxPanel(m_match_ratio_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
            m_match_blend_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(128)));
            m_match_blend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_match_blend_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_match_blend_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(m_match_blend_panel);
                dc.SetBackground(wxBrush(parse_mixed_color(compute_preview_color())));
                dc.Clear();
                wxSize sz = m_match_blend_panel->GetClientSize();
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(180, 180, 180)), 1));
                dc.DrawRectangle(0, 0, sz.x, sz.y);
            });
            right_col->Add(m_match_blend_panel, 0, wxEXPAND);
            dual_preview_row->Add(right_col, 0, wxEXPAND);

            m_match_ratio_card_sizer->Add(dual_preview_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        }

        m_match_gradient_selector->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
            // Sync tri weights to slider for strip preview + legend
            int val = std::clamp(m_match_gradient_selector->value(), m_match_min_pct, 100 - m_match_min_pct);
            m_match_tri_wx = (100 - val) / 100.0;
            m_match_tri_wy = val / 100.0;
            m_match_tri_weights = {m_match_tri_wx, m_match_tri_wy};
            update_match_legend_labels();
            if (m_match_strip_panel)   m_match_strip_panel->Refresh();
            if (m_match_blend_panel)   m_match_blend_panel->Refresh();
            update_compatibility_warning();
        });

        m_match_ratio_card->SetSizer(m_match_ratio_card_sizer);
        scroll_sizer->Add(m_match_ratio_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ======== Gradient Effect Card: 混色效果 (shown in gradient mode) ========
    {
        m_gradient_effect_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                                wxDefaultSize, wxBORDER_NONE);
        m_gradient_effect_card->SetCornerRadius(FromDIP(4));
        m_gradient_effect_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_gradient_effect_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_gradient_effect_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_gradient_effect_card->SetBorderWidth(FromDIP(1));
        m_gradient_effect_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_gradient_effect_card_sizer = new wxBoxSizer(wxVERTICAL);

        // Mix Effect title (same font as Blended Color)
        auto* effect_title = new wxStaticText(m_gradient_effect_card, wxID_ANY, _L("Mix Effect"));
        effect_title->SetFont(Label::Body_14);
        effect_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        effect_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_gradient_effect_card_sizer->Add(effect_title, 0, wxTOP | wxLEFT | wxRIGHT, FromDIP(16));
        m_gradient_effect_card_sizer->AddSpacer(FromDIP(8));

        // Gradient preview panel
        m_preview_panel = new wxPanel(m_gradient_effect_card, wxID_ANY, wxDefaultPosition,
                                      wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
        m_preview_panel->SetMinSize(wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
        m_preview_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_preview_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_preview_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_preview_panel);
            if (m_current_mode == MODE_GRADIENT && m_filament_rows.size() >= 2) {
                wxSize sz = m_preview_panel->GetClientSize();
                int ia = std::max(0, std::min(get_filament_index(0), (int)m_filament_colours.size()-1));
                int ib = std::max(0, std::min(get_filament_index(1), (int)m_filament_colours.size()-1));
                wxColour ca = parse_mixed_color(m_filament_colours[ia]);
                wxColour cb = parse_mixed_color(m_filament_colours[ib]);
                if (m_gradient_direction != 0)
                    std::swap(ca, cb);
                wxImage img(sz.GetWidth(), sz.GetHeight());
                unsigned char* data = img.GetData();
                // Vertical gradient: top = cb-last, bottom = ca-first
                // (t=1.0 at top→pure cb, t=0 at bottom→pure ca, matching gradient_start/end direction 0)
                for (int y = 0; y < sz.GetHeight(); ++y) {
                    float t = (sz.GetHeight() > 1) ? 1.0f - float(y) / float(sz.GetHeight() - 1) : 0.5f;
                    wxColour c = blend_pair_filament_mixer(ca, cb, t);
                    for (int x = 0; x < sz.GetWidth(); ++x) {
                        int idx = (y * sz.GetWidth() + x) * 3;
                        data[idx]   = c.Red();
                        data[idx+1] = c.Green();
                        data[idx+2] = c.Blue();
                    }
                }
                dc.DrawBitmap(wxBitmap(img), 0, 0, false);
            } else {
                dc.SetBackground(wxBrush(parse_mixed_color(compute_preview_color())));
                dc.Clear();
            }
            wxSize sz = m_preview_panel->GetClientSize();
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(180, 180, 180)), 1));
            dc.DrawRectangle(0, 0, sz.x, sz.y);
        });
        m_gradient_effect_card_sizer->Add(m_preview_panel, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        m_gradient_effect_card->SetSizer(m_gradient_effect_card_sizer);
        m_gradient_effect_card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) { this->SetFocus(); evt.Skip(); });
        scroll_sizer->Add(m_gradient_effect_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ---- Match mode panel ----
    {
        m_match_panel_sizer = new wxBoxSizer(wxVERTICAL);
        wxColour initial = (m_current_mode == MODE_MATCH && !m_result.display_color.empty())
            ? parse_mixed_color(m_result.display_color)
            : StateColor::darkModeColorFor(wxColour("#26A69A"));
        m_match_panel = new MixedColorMatchPanel(m_scrolled_content, m_filament_colours, initial);
        m_match_panel_sizer->Add(m_match_panel, 0, wxEXPAND | wxALL, M);
        scroll_sizer->Add(m_match_panel_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ======== Card: Cycle Pattern ========
    {
        m_cycle_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, wxBORDER_NONE);
        m_cycle_card->SetCornerRadius(FromDIP(4));
        m_cycle_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_cycle_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_cycle_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_cycle_card->SetBorderWidth(FromDIP(1));
        m_cycle_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_cycle_card_sizer = new wxBoxSizer(wxVERTICAL);

        // Title
        auto* cycle_title = new wxStaticText(m_cycle_card, wxID_ANY, _L("Filaments"));
        cycle_title->SetFont(Label::Body_14);
        cycle_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        cycle_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_cycle_card_sizer->Add(cycle_title, 0, wxALL, FromDIP(16));

        // Filament quick buttons (20×20 badges, wrapping)
        {
            MixedFilamentDisplayContext ctx;
            ctx.num_physical = m_filament_colours.size();
            ctx.physical_colors = m_filament_colours;

            auto* btn_row = new wxWrapSizer(wxHORIZONTAL);
            for (int fid = 0; fid < (int)m_filament_colours.size(); ++fid) {
                MixedFilament mf;
                mf.display_color = m_filament_colours[fid];
                mf.custom = true;

                auto* badge = new MixedFilamentBadge(m_cycle_card, wxID_ANY, fid + 1, mf, ctx, true, 20);
                badge->SetToolTip(wxString::Format(_L("Append filament %d to pattern"), fid + 1));
                badge->Bind(wxEVT_BUTTON, [this, fid](wxCommandEvent&) {
                    if (m_pattern_ctrl) {
                        if (fid + 1 >= 10) {
                            m_pattern_ctrl->AppendText(wxString::Format("[%d]", fid + 1));
                        } else {
                            m_pattern_ctrl->AppendText(wxString::Format("%d", fid + 1));
                        }
                        validate_cycle_pattern();
                    }
                });
                btn_row->Add(badge, 0, wxRIGHT | wxBOTTOM, FromDIP(8));
                m_pattern_quick_buttons.push_back(badge);
            }
            m_cycle_card_sizer->Add(btn_row, 0, wxLEFT | wxRIGHT, FromDIP(16));
            m_cycle_card_sizer->AddSpacer(FromDIP(12));
        }

        // Pattern input (Figma: 293×30, #F0F0F0 1px border)
        {
            const std::string init_pattern = MixedFilamentManager::normalize_manual_pattern(
                m_current_mode == MODE_CYCLE ? m_result.manual_pattern : std::string());
            // StaticBox wrapper for exact #F0F0F0 border color
            auto* input_wrapper = new StaticBox(m_cycle_card, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
            input_wrapper->SetCornerRadius(0);
            input_wrapper->SetBorderWidth(FromDIP(1));
            input_wrapper->SetBorderColor(
                StateColor(std::pair(wxColour("#F0F0F0"), (int)StateColor::Normal)));
            input_wrapper->SetBackgroundColor(
                StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
            auto* wrapper_sizer = new wxBoxSizer(wxHORIZONTAL);
            m_pattern_ctrl = new wxTextCtrl(input_wrapper, wxID_ANY,
                                            from_u8(init_pattern.empty() ? "12" : init_pattern),
                                            wxDefaultPosition, wxDefaultSize,
                                            wxTE_PROCESS_ENTER | wxBORDER_NONE);
            m_pattern_ctrl->SetFont(Label::Body_14);
            m_pattern_ctrl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_pattern_ctrl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            m_pattern_ctrl->SetMargins(FromDIP(8), FromDIP(8));
            input_wrapper->SetMinSize(wxSize(FromDIP(293), FromDIP(30)));
            wrapper_sizer->Add(m_pattern_ctrl, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(1));
            input_wrapper->SetSizer(wrapper_sizer);
            m_pattern_ctrl->SetToolTip(_L("Allowed Input: Only digits, square brackets ([ and ]), and commas (,)."));
            m_pattern_ctrl->SetMaxLength(512);

            m_pattern_ctrl->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
                validate_cycle_pattern();
                event.Skip();
            });
            m_pattern_ctrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
                validate_cycle_pattern();
            });
            m_cycle_card_sizer->Add(input_wrapper, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        }

        // Divider
        {
            auto* divider = new wxPanel(m_cycle_card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
            m_cycle_card_sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, M);
        }

        // Dual-column preview: stripe (left) + blend (right)
        {
            auto* dual_preview_row = new wxBoxSizer(wxHORIZONTAL);

            // Left: preview label + stripe
            auto* left_col = new wxBoxSizer(wxVERTICAL);
            auto* preview_lbl = new wxStaticText(m_cycle_card, wxID_ANY, _L("Preview"));
            preview_lbl->SetFont(Label::Body_12);
            preview_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            preview_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            left_col->Add(preview_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));

            m_cycle_strip_panel = new wxPanel(m_cycle_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
            m_cycle_strip_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(140)));
            m_cycle_strip_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_cycle_strip_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_cycle_strip_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(m_cycle_strip_panel);
                draw_strip(dc, m_cycle_strip_panel);
            });
            left_col->Add(m_cycle_strip_panel, 1, wxEXPAND);
            dual_preview_row->Add(left_col, 1, wxEXPAND | wxRIGHT, FromDIP(8));

            // Right: blend effect label + blend color panel
            auto* right_col = new wxBoxSizer(wxVERTICAL);
            auto* blend_lbl = new wxStaticText(m_cycle_card, wxID_ANY, _L("Mix Effect"));
            blend_lbl->SetFont(Label::Body_12);
            blend_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#8F8F8F")));
            blend_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            right_col->Add(blend_lbl, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));

            m_cycle_blend_panel = new wxPanel(m_cycle_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
            m_cycle_blend_panel->SetMinSize(wxSize(FromDIP(140), FromDIP(140)));
            m_cycle_blend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_cycle_blend_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_cycle_blend_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(m_cycle_blend_panel);
                dc.SetBackground(wxBrush(parse_mixed_color(compute_preview_color())));
                dc.Clear();
                wxSize sz = m_cycle_blend_panel->GetClientSize();
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(180, 180, 180)), 1));
                dc.DrawRectangle(0, 0, sz.x, sz.y);
            });
            right_col->Add(m_cycle_blend_panel, 1, wxEXPAND);
            dual_preview_row->Add(right_col, 1, wxEXPAND);

            m_cycle_card_sizer->Add(dual_preview_row, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        }

        // Legend panel (dynamic swatches + percentage labels)
        m_cycle_legend_panel = new wxPanel(m_cycle_card, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_cycle_legend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_cycle_legend_sizer = new wxFlexGridSizer(5, FromDIP(12), FromDIP(6));
        m_cycle_legend_panel->SetSizer(m_cycle_legend_sizer);
        m_cycle_card_sizer->Add(m_cycle_legend_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        m_cycle_card->SetSizer(m_cycle_card_sizer);
        m_cycle_card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) { this->SetFocus(); evt.Skip(); });
        scroll_sizer->Add(m_cycle_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ======== Card C: Recommended Swatches ========
    {
        m_swatch_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, wxBORDER_NONE);
        m_swatch_card->SetCornerRadius(FromDIP(4));
        m_swatch_card->SetMinSize(wxSize(FromDIP(325), -1));
        m_swatch_card->SetMaxSize(wxSize(FromDIP(325), -1));
        m_swatch_card->SetBackgroundColor(
            StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        m_swatch_card->SetBorderWidth(FromDIP(1));
        m_swatch_card->SetBorderColorNormal(wxColour("#F0F0F0"));
        m_swatch_card_sizer = new wxBoxSizer(wxVERTICAL);

        auto* swatch_title = new wxStaticText(m_swatch_card, wxID_ANY, _L("Mixing Recommendations"));
        swatch_title->SetFont(Label::Body_14);
        swatch_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        swatch_title->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_swatch_card_sizer->Add(swatch_title, 0, wxALL, FromDIP(16));

        m_swatch_grid_panel = new wxPanel(m_swatch_card, wxID_ANY, wxDefaultPosition,
                                          wxDefaultSize);
        m_swatch_grid_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        build_swatch_grid();
        m_swatch_card_sizer->Add(m_swatch_grid_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        m_swatch_card->SetSizer(m_swatch_card_sizer);
        m_swatch_card->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) { this->SetFocus(); evt.Skip(); });
        scroll_sizer->Add(m_swatch_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    m_scrolled_content->SetSizer(scroll_sizer);
    top_sizer->Add(m_scrolled_content, 1, wxEXPAND);

    // ---- Bottom button panel: white bg + #F0F0F0 top border, padding 13/12/20 ----
    {
        auto* btn_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        btn_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

        auto* panel_sizer = new wxBoxSizer(wxVERTICAL);

        // #F0F0F0 top border (1px)
        auto* top_line = new wxPanel(btn_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        top_line->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
        panel_sizer->Add(top_line, 0, wxEXPAND);

        // 13px top padding - 1px line = 12px remaining top padding
        panel_sizer->AddSpacer(FromDIP(12));

        auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);

        m_btn_cancel  = new Button(btn_panel, _L("Cancel"));
        m_btn_cancel->SetMinSize(wxSize(-1, FromDIP(38)));
        m_btn_cancel->SetCornerRadius(FromDIP(4));
        m_btn_cancel->SetBorderWidth(FromDIP(1));
        m_btn_cancel->SetBorderColorNormal(wxColour("#D1D5DC"));
        m_btn_cancel->SetBackgroundColorNormal(wxColour("#FFFFFF"));
        m_btn_cancel->SetTextColorNormal(wxColour("#242424"));

        m_btn_confirm = new Button(btn_panel, _L("OK"));
        m_btn_confirm->SetMinSize(wxSize(-1, FromDIP(38)));
        m_btn_confirm->SetCornerRadius(FromDIP(4));
        m_btn_confirm->SetBorderWidth(0);
        m_btn_confirm->SetBackgroundColor(StateColor(
            std::pair(wxColour("#DFDFDF"), (int)StateColor::Disabled),
            std::pair(wxColour("#019687"), (int)StateColor::Normal)));
        m_btn_confirm->SetTextColor(StateColor(
            std::pair(wxColour("#6B6A6A"), (int)StateColor::Disabled),
            std::pair(wxColour("#FEFEFE"), (int)StateColor::Normal)));

        btn_sizer->Add(m_btn_cancel,  1, wxRIGHT, FromDIP(8));
        btn_sizer->Add(m_btn_confirm, 1);
        panel_sizer->Add(btn_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

        // 12px bottom padding
        panel_sizer->AddSpacer(FromDIP(12));

        btn_panel->SetSizer(panel_sizer);
        top_sizer->Add(btn_panel, 0, wxEXPAND);
    }

    SetSizer(top_sizer);
    SetMinClientSize(wxSize(FromDIP(380), FromDIP(666)));
    SetMaxClientSize(wxSize(FromDIP(380), FromDIP(666)));

    // Initialize match state before rebuild (avoids gradient/tri flicker)
    on_mode_changed(m_current_mode);

    rebuild_filament_rows();
    update_compatibility_warning();

    // Bind slider events
    m_gradient_selector->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        int val = m_gradient_selector->value();
        update_legend_text();
        if (m_preview_panel) m_preview_panel->Refresh();
        if (m_preview_blend_panel) m_preview_blend_panel->Refresh();
        if (m_strip_panel)   m_strip_panel->Refresh();
        update_compatibility_warning();
    });

    // --- Match-mode event wiring (does not affect ratio/cycle/gradient) ---
    if (m_match_panel) {
        m_match_panel->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
            if (m_current_mode != MODE_MATCH) return;
            auto recipe = m_match_panel->selected_recipe();
            if (!recipe.valid) return;

            int num_physical = (int)m_filament_colours.size();
            auto weights = expand_color_match_recipe_weights(recipe, num_physical);

            std::vector<std::pair<int, int>> sorted;
            for (int i = 0; i < (int)weights.size(); ++i)
                if (weights[i] > 0) sorted.push_back({i, weights[i]});
            std::sort(sorted.begin(), sorted.end(),
                      [](auto& a, auto& b) { return a.second > b.second; });

            // Use actual recipe filaments (no padding) — visibility switches based on count
            if ((int)sorted.size() >= 3) {
                sorted.resize(3);
                m_match_tri_indices = {sorted[0].first, sorted[1].first, sorted[2].first};
                int total = sorted[0].second + sorted[1].second + sorted[2].second;
                if (total > 0) {
                    m_match_tri_wx = sorted[0].second / (double)total;
                    m_match_tri_wy = sorted[1].second / (double)total;
                    m_match_tri_wz = sorted[2].second / (double)total;
                }
                m_match_tri_weights = {m_match_tri_wx, m_match_tri_wy, m_match_tri_wz};
            } else if ((int)sorted.size() >= 2) {
                if (sorted.size() >= 2) {
                    m_match_tri_indices = {sorted[0].first, sorted[1].first};
                    if (m_match_gradient_selector) {
                        int total = sorted[0].second + sorted[1].second;
                        int b_pct = (total > 0) ? (sorted[1].second * 100 / total) : 50;
                        m_match_gradient_selector->set_value(b_pct);
                    }
                    m_match_tri_weights = {sorted[0].second / 100.0, sorted[1].second / 100.0};
                }
            }

            if (m_match_gradient_selector && m_match_tri_indices.size() >= 2) {
                int ia = std::max(0, std::min(m_match_tri_indices[0], (int)m_filament_colours.size() - 1));
                int ib = std::max(0, std::min(m_match_tri_indices[1], (int)m_filament_colours.size() - 1));
                m_match_gradient_selector->set_colors(
                    parse_mixed_color(m_filament_colours[ia]),
                    parse_mixed_color(m_filament_colours[ib]));
            }

            rebuild_match_legend();
            update_ratio_or_tri_visibility();  // switch gradient/tri based on recipe filament count
            if (m_match_tri_picker)  m_match_tri_picker->Refresh();
            if (m_match_strip_panel) m_match_strip_panel->Refresh();
            if (m_match_blend_panel) m_match_blend_panel->Refresh();
            update_compatibility_warning();
        });
    }

    // --- End of match-mode event wiring ---

    m_btn_cancel->Bind(wxEVT_BUTTON,  [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    m_btn_confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_current_mode == MODE_CYCLE) {
            validate_cycle_pattern();
            if (!m_btn_confirm->IsEnabled()) return;
        }
        collect_result();
        EndModal(wxID_OK);
    });

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });

    Fit();
    CentreOnParent();

    m_scrolled_content->FitInside();
    m_scrolled_content->Scroll(0, 0);

}

// ---------------------------------------------------------------------------
// Barycentric triangle utilities
// ---------------------------------------------------------------------------

struct TriPt { double x, y; };

static double tri_area2(TriPt a, TriPt b, TriPt c)
{
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

static void tri_bary(TriPt p, TriPt v0, TriPt v1, TriPt v2,
                     double& w0, double& w1, double& w2)
{
    double total = tri_area2(v0, v1, v2);
    if (std::abs(total) < 1e-9) { w0 = w1 = w2 = 1.0 / 3.0; return; }
    w0 = tri_area2(p, v1, v2) / total;
    w1 = tri_area2(v0, p, v2) / total;
    w2 = 1.0 - w0 - w1;
}

static TriPt tri_clamp_pt(TriPt p, TriPt v0, TriPt v1, TriPt v2)
{
    double w0, w1, w2;
    tri_bary(p, v0, v1, v2, w0, w1, w2);
    return { w0 * v0.x + w1 * v1.x + w2 * v2.x,
             w0 * v0.y + w1 * v1.y + w2 * v2.y };
}

// ---------------------------------------------------------------------------

int MixedFilamentDialog::get_filament_index(int row_idx) const
{
    if (row_idx < 0 || row_idx >= (int)m_filament_rows.size()) return 0;
    const auto& row = m_filament_rows[row_idx];
    int cb_idx = row.combo->GetSelection();
    if (cb_idx < 0 || cb_idx >= (int)row.filament_indices.size()) return 0;
    return row.filament_indices[cb_idx];
}

// ---------------------------------------------------------------------------

void MixedFilamentDialog::populate_combo(ComboBox* cb, const std::set<int>& exclude_ids, int select_id,
                                         std::vector<int>& out_filament_indices)
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const std::vector<std::string>& filament_presets = preset_bundle ? preset_bundle->filament_presets : std::vector<std::string>();

    cb->Clear();
    out_filament_indices.clear();

    int select_idx = -1;
    for (int j = 0; j < (int)m_filament_colours.size(); ++j) {
        if (exclude_ids.find(j) != exclude_ids.end()) continue;

        wxString display_name;
        if (preset_bundle && j < (int)filament_presets.size()) {
            const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[j]);
            if (preset) display_name = from_u8(preset->label(false));
        }
        if (display_name.empty()) display_name = wxString::Format("F%d", j + 1);

        wxBitmap* badge_icon = get_extruder_color_icon(m_filament_colours[j], std::to_string(j + 1), FromDIP(20), FromDIP(20));
        cb->Append(display_name, badge_icon ? badge_icon->ConvertToImage() : wxNullImage);

        if (j == select_id) select_idx = out_filament_indices.size();
        out_filament_indices.push_back(j);
    }

    if (select_idx >= 0) cb->SetSelection(select_idx);
}

// ---------------------------------------------------------------------------

void MixedFilamentDialog::refresh_all_combos()
{
    for (size_t i = 0; i < m_filament_rows.size(); ++i) {
        std::set<int> used_by_others;
        for (size_t k = 0; k < m_filament_rows.size(); ++k)
            if (k != i) used_by_others.insert(get_filament_index((int)k));

        populate_combo(m_filament_rows[i].combo, used_by_others, get_filament_index((int)i),
                       m_filament_rows[i].filament_indices);
    }

    for (size_t i = 0; i < m_filament_rows.size(); ++i)
        set_combo_combined_icon(m_filament_rows[i].combo, get_filament_index((int)i));
}

// ---------------------------------------------------------------------------

void MixedFilamentDialog::rebuild_all_combos_with_selections(const std::vector<int>& selections)
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const std::vector<std::string>& filament_presets = preset_bundle ? preset_bundle->filament_presets : std::vector<std::string>();

    for (size_t i = 0; i < m_filament_rows.size(); ++i) {
        auto& row = m_filament_rows[i];
        ComboBox* cb = row.combo;

        // Build set of filaments already used by other rows
        std::set<int> used_by_others;
        for (size_t k = 0; k < m_filament_rows.size(); ++k) {
            if (k != i && k < selections.size()) {
                used_by_others.insert(selections[k]);
            }
        }

        // Clear and rebuild combo box
        cb->Clear();
        row.filament_indices.clear();

        for (int j = 0; j < (int)m_filament_colours.size(); ++j) {
            // Skip filaments already used by other rows
            if (used_by_others.find(j) != used_by_others.end()) continue;

            wxString display_name;
            if (preset_bundle && j < (int)filament_presets.size()) {
                const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[j]);
                if (preset) {
                    display_name = from_u8(preset->label(false));
                }
            }
            if (display_name.empty()) {
                display_name = wxString::Format("F%d", j + 1);
            }
            wxBitmap* icon = get_extruder_color_icon(m_filament_colours[j], std::to_string(j + 1), FromDIP(20), FromDIP(20));
            cb->Append(display_name, icon ? icon->ConvertToImage() : wxNullImage);
            row.filament_indices.push_back(j);
        }

        // Restore selection from the provided selections vector
        int sel = (i < selections.size()) ? selections[i] : 0;
        for (int k = 0; k < (int)row.filament_indices.size(); ++k) {
            if (row.filament_indices[k] == sel) {
                cb->SetSelection(k);
                break;
            }
        }
    }

    for (size_t i = 0; i < m_filament_rows.size(); ++i)
        set_combo_combined_icon(m_filament_rows[i].combo, get_filament_index((int)i));
}

// ---------------------------------------------------------------------------

void MixedFilamentDialog::set_combo_combined_icon(ComboBox* cb, int filament_idx)
{
    if (!cb || filament_idx < 0 || filament_idx >= (int)m_filament_colours.size()) return;
    const int pad = FromDIP(8), arr_w = FromDIP(8), badge_w = FromDIP(20), h = FromDIP(20), gap = FromDIP(8), text_gap = FromDIP(8);
    const int total_w = pad + arr_w + gap + badge_w + text_gap;
    // Transparent background via wxImage alpha
    wxImage img(total_w, h, true);
    img.InitAlpha();
    memset(img.GetAlpha(), 0, total_w * h);

    auto set_rgba = [&](int x, int y, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
        if (x < 0 || x >= total_w || y < 0 || y >= h) return;
        int pos = y * total_w + x;
        img.GetData()[pos * 3] = r;
        img.GetData()[pos * 3 + 1] = g;
        img.GetData()[pos * 3 + 2] = b;
        img.GetAlpha()[pos] = a;
    };

    // Arrow: paste SVG (transparent background, only arrow pixels opaque)
    ScalableBitmap ab(cb, "drop_down", arr_w);
    if (ab.bmp().IsOk()) {
        wxImage aimg = ab.bmp().ConvertToImage();
        if (!aimg.HasAlpha()) aimg.InitAlpha();
        int ax = pad, ay = (h - aimg.GetHeight()) / 2;
        for (int y = 0; y < aimg.GetHeight() && ay + y < h; ++y)
            for (int x = 0; x < aimg.GetWidth() && ax + x < total_w; ++x) {
                unsigned char* s = aimg.GetData() + (y * aimg.GetWidth() + x) * 3;
                unsigned char a = aimg.HasAlpha() ? *(aimg.GetAlpha() + y * aimg.GetWidth() + x) : 255;
                if (a > 0) set_rgba(ax + x, ay + y, s[0], s[1], s[2], a);
            }
    }

    // Badge: use get_extruder_color_icon (16×16 with number, opaque)
    const int bx = pad + arr_w + gap;
    wxBitmap* badge_bmp = get_extruder_color_icon(m_filament_colours[filament_idx],
        std::to_string(filament_idx + 1), FromDIP(20), FromDIP(20));
    if (badge_bmp) {
        wxImage bimg = badge_bmp->ConvertToImage();
        int by = (h - bimg.GetHeight()) / 2;
        for (int y = 0; y < bimg.GetHeight() && by + y < h; ++y)
            for (int x = 0; x < bimg.GetWidth() && bx + x < total_w; ++x) {
                unsigned char* s = bimg.GetData() + (y * bimg.GetWidth() + x) * 3;
                set_rgba(bx + x, by + y, s[0], s[1], s[2], 255);
            }
    }

    cb->SetIcon(wxBitmap(img));
    // SetIcon triggers Rescale→messureSize which recalculates height; re-lock to 30
    cb->SetMinSize(wxSize(-1, FromDIP(30)));
    cb->SetMaxSize(wxSize(-1, FromDIP(30)));
}

// ---------------------------------------------------------------------------

void MixedFilamentDialog::rebuild_legend()
{
    if (!m_legend_sizer) return;

    m_legend_sizer->Clear(true);
    m_legend_labels.clear();

    int n = (int)m_filament_rows.size();
    if (n < 2) return;

    // Compute weights
    std::vector<int> weights(n, 0);
    if (n == 2 && m_gradient_selector) {
        int val = m_gradient_selector->value();
        weights[0] = 100 - val;
        weights[1] = val;
    } else if (n == 3) {
        weights[0] = (int)(m_tri_wx * 100 + 0.5);
        weights[1] = (int)(m_tri_wy * 100 + 0.5);
        weights[2] = 100 - weights[0] - weights[1];
        if (weights[2] < 0) weights[2] = 0;
    } else {
        for (int i = 0; i < n; ++i)
            weights[i] = 100 / n;
    }

    MixedFilamentDisplayContext ctx;
    ctx.num_physical = m_filament_colours.size();
    ctx.physical_colors = m_filament_colours;

    for (int i = 0; i < n; ++i) {
        int idx = get_filament_index(i);
        idx = std::max(0, std::min(idx, (int)m_filament_colours.size() - 1));

        // Badge + label pair
        auto* pair = new wxBoxSizer(wxHORIZONTAL);

        MixedFilament mf;
        mf.display_color = m_filament_colours[idx];
        mf.custom = true;
        auto* badge = new MixedFilamentBadge(m_legend_panel, wxID_ANY, idx + 1, mf, ctx, true, 12);
        pair->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        auto* lbl = new wxStaticText(m_legend_panel, wxID_ANY, wxString::Format("%d%%", weights[i]));
        lbl->SetFont(Label::Body_12);
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        pair->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        m_legend_labels.push_back(lbl);

        m_legend_sizer->Add(pair, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    }

    m_legend_panel->Layout();
}

void MixedFilamentDialog::rebuild_match_legend()
{
    if (!m_match_legend_sizer) return;
    m_match_legend_sizer->Clear(true);
    m_match_legend_labels.clear();
    if (m_match_tri_indices.empty()) return;

    MixedFilamentDisplayContext ctx;
    ctx.num_physical = m_filament_colours.size();
    ctx.physical_colors = m_filament_colours;

    int num_physical = (int)m_filament_colours.size();
    std::vector<int> weights;
    // Always use tri_weights (same source as compute_preview_color / draw_strip)
    for (double w : m_match_tri_weights)
        weights.push_back((int)(w * 100 + 0.5));

    int total = 0;
    for (int w : weights) total += w;
    if (total > 0) {
        for (size_t i = 0; i < weights.size(); ++i)
            weights[i] = weights[i] * 100 / total;
    }

    for (size_t i = 0; i < m_match_tri_indices.size(); ++i) {
        int fid = m_match_tri_indices[i];
        fid = std::max(0, std::min(fid, (int)m_filament_colours.size() - 1));
        auto* pair = new wxBoxSizer(wxHORIZONTAL);
        MixedFilament mf;
        mf.display_color = m_filament_colours[fid];
        mf.custom = true;
        auto* badge = new MixedFilamentBadge(m_match_legend_panel, wxID_ANY, fid + 1, mf, ctx, true, 12);
        pair->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        int pct = (i < weights.size()) ? weights[i] : 0;
        auto* lbl = new wxStaticText(m_match_legend_panel, wxID_ANY, wxString::Format("%d%%", pct));
        lbl->SetFont(Label::Body_12);
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        pair->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        m_match_legend_labels.push_back(lbl);
        m_match_legend_sizer->Add(pair, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    }
    m_match_legend_panel->Layout();
}

void MixedFilamentDialog::update_match_legend_labels()
{
    if (m_match_legend_labels.empty() || m_match_tri_indices.empty()) return;

    std::vector<int> weights;
    int nf = (int)m_match_tri_indices.size();
    if (nf >= 3) {
        weights = {(int)(m_match_tri_wx * 100 + 0.5), (int)(m_match_tri_wy * 100 + 0.5),
                   (int)(m_match_tri_wz * 100 + 0.5)};
    } else {
        for (double w : m_match_tri_weights)
            weights.push_back((int)(w * 100 + 0.5));
    }

    int total = 0;
    for (int w : weights) total += w;
    if (total > 0) {
        for (size_t i = 0; i < weights.size(); ++i)
            weights[i] = weights[i] * 100 / total;
    }

    for (size_t i = 0; i < m_match_legend_labels.size() && i < weights.size(); ++i)
        m_match_legend_labels[i]->SetLabel(wxString::Format("%d%%", weights[i]));
}

// ---------------------------------------------------------------------------

void MixedFilamentDialog::update_legend_text()
{
    if (m_legend_labels.empty()) return;

    int n = (int)m_filament_rows.size();
    if ((int)m_legend_labels.size() != n) return;

    // Compute weights (same logic as rebuild_legend, no widget recreation)
    std::vector<int> weights(n, 0);
    if (n == 2 && m_gradient_selector) {
        int val = m_gradient_selector->value();
        weights[0] = 100 - val;
        weights[1] = val;
    } else if (n == 3) {
        weights[0] = (int)(m_tri_wx * 100 + 0.5);
        weights[1] = (int)(m_tri_wy * 100 + 0.5);
        weights[2] = 100 - weights[0] - weights[1];
        if (weights[2] < 0) weights[2] = 0;
    } else {
        for (int i = 0; i < n; ++i)
            weights[i] = 100 / n;
    }

    for (int i = 0; i < n; ++i)
        m_legend_labels[i]->SetLabel(wxString::Format("%d%%", weights[i]));
}

// ---------------------------------------------------------------------------

void MixedFilamentDialog::rebuild_filament_rows()
{
    m_filament_rows_sizer->Clear(true);
    m_filament_rows.clear();

    const int max_idx = std::max(0, (int)m_filament_colours.size() - 1);
    std::vector<int> sels;

    // Decode gradient_component_ids (supports both legacy single-char and extended /-separated formats)
    std::vector<unsigned int> decoded_ids = MixedFilamentManager::decode_gradient_component_ids(m_result.gradient_component_ids);

    // Detect "all-ids" format (match recipe): gradient_component_ids contains
    // component_a as its first entry, meaning all filaments are listed there.
    const bool all_ids_format = !decoded_ids.empty() &&
        decoded_ids[0] == m_result.component_a;

    if (all_ids_format) {
        for (unsigned int id : decoded_ids)
            sels.push_back(std::clamp(int(id - 1), 0, max_idx));
    } else {
        sels.push_back(std::clamp((int)m_result.component_a - 1, 0, max_idx));
        sels.push_back(std::clamp((int)m_result.component_b - 1, 0, max_idx));
        for (unsigned int id : decoded_ids) {
            sels.push_back(std::clamp(int(id - 1), 0, max_idx));
        }
    }

    int count = (int)sels.size();
    if (m_current_mode == MODE_GRADIENT)
        count = 2;
    else
        count = std::max(2, std::min(count, max_filaments_for_mode(m_current_mode)));

    for (int i = 0; i < count; ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* row_lbl = new wxStaticText(m_filament_rows_panel, wxID_ANY,
                                         wxString::Format(_L("Filament %d"), i + 1),
                                         wxDefaultPosition, wxSize(FromDIP(50), -1));
        row_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        row_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        row->Add(row_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

        // Note: sw (color swatch panel) is kept for internal state tracking but hidden
        // The color is now shown inside the ComboBox via get_extruder_color_icon
        auto* sw = new wxPanel(m_filament_rows_panel, wxID_ANY, wxDefaultPosition,
                               wxSize(FromDIP(SWATCH_SIZE), FromDIP(SWATCH_SIZE)));
        sw->SetMinSize(wxSize(FromDIP(SWATCH_SIZE), FromDIP(SWATCH_SIZE)));
        sw->SetBackgroundStyle(wxBG_STYLE_PAINT);
        sw->Hide();  // Hide the external swatch, color is shown in ComboBox

        auto* cb = new ComboBox(m_filament_rows_panel, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(FromDIP(200), FromDIP(30)),
                                0, nullptr, wxCB_READONLY);
        cb->SetMinSize(wxSize(-1, FromDIP(30)));
        cb->SetMaxSize(wxSize(-1, FromDIP(30)));

        // Build set of filaments already used by other rows (based on sels array)
        std::set<int> used_by_others;
        for (int k = 0; k < count; ++k) {
            if (k != i && k < (int)sels.size())
                used_by_others.insert(sels[k]);
        }

        int sel = (i < (int)sels.size()) ? sels[i] : i;
        sel = std::max(0, std::min(sel, (int)m_filament_colours.size() - 1));

        FilamentRow fr;
        fr.swatch = sw;
        fr.combo = cb;

        populate_combo(cb, used_by_others, sel, fr.filament_indices);

        wxColour init_col = parse_mixed_color(m_filament_colours[sel]);
        sw->Bind(wxEVT_PAINT, [sw, init_col](wxPaintEvent&) mutable {
            wxAutoBufferedPaintDC dc(sw);
            dc.SetBackground(wxBrush(init_col));
            dc.Clear();
        });

        // Add to m_filament_rows before binding so we can use row index
        m_filament_rows.push_back(fr);
        int row_idx = (int)m_filament_rows.size() - 1;

        cb->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            sync_rows_to_result();
            refresh_all_combos();
            rebuild_legend();
            if (m_tri_picker) m_tri_picker->Refresh();
            update_preview();
            update_compatibility_warning();
        });

        row->Add(cb, 1, wxEXPAND);

        m_filament_rows_sizer->Add(row, 0, wxEXPAND | (i < count - 1 ? wxBOTTOM : 0), FromDIP(12));
    }

    m_filament_rows_panel->Layout();
    update_ratio_or_tri_visibility();
    rebuild_legend();

    if (m_swatch_grid_panel) {
        m_swatch_grid_panel->DestroyChildren();
        build_swatch_grid();
    }

    // Refresh card containers so backgrounds repaint after child changes
    if (m_filament_card) {
        m_filament_card->Layout();
        m_filament_card->Refresh();
    }
    if (m_ratio_card) {
        m_ratio_card->Layout();
        m_ratio_card->Refresh();
    }

    // Set combined arrow+icon for newly created combos
    for (size_t i = 0; i < m_filament_rows.size(); ++i)
        set_combo_combined_icon(m_filament_rows[i].combo, get_filament_index((int)i));

    update_preview();
}

void MixedFilamentDialog::build_tri_picker(wxWindow* parent)
{
    if (!parent) parent = this;
    static constexpr int TRI_SIZE = 160;
    m_tri_picker = new wxPanel(parent, wxID_ANY, wxDefaultPosition,
                               wxSize(FromDIP(TRI_SIZE), FromDIP(TRI_SIZE)));
    m_tri_picker->SetMinSize(wxSize(FromDIP(TRI_SIZE), FromDIP(TRI_SIZE)));
    m_tri_picker->SetBackgroundStyle(wxBG_STYLE_PAINT);

    auto get_verts = [this]() -> std::tuple<TriPt, TriPt, TriPt> {
        wxSize sz = m_tri_picker->GetClientSize();
        double pw = sz.GetWidth(), ph = sz.GetHeight();
        double margin = FromDIP(3);
        double side = std::min(pw, ph) - 2 * margin;
        double tri_h = side * std::sqrt(3.0) / 2.0;
        double cx = pw / 2.0;
        double top_y = (ph - tri_h) / 2.0;
        double bot_y = top_y + tri_h;
        return { {cx, top_y}, {cx - side / 2.0, bot_y}, {cx + side / 2.0, bot_y} };
    };

    auto safe_col = [&](int row) -> wxColour {
        if (row >= (int)m_filament_rows.size()) return wxColour(128,128,128);
        int s = get_filament_index(row);
        s = std::max(0, std::min(s, (int)m_filament_colours.size()-1));
        return parse_mixed_color(m_filament_colours[s]);
    };

    m_tri_picker->Bind(wxEVT_PAINT, [this, get_verts, safe_col](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_tri_picker);
        wxSize sz = m_tri_picker->GetClientSize();
        auto [v0, v1, v2] = get_verts();

        wxColour c0 = safe_col(0), c1 = safe_col(1), c2 = safe_col(2);

        int min_y = (int)std::min({v0.y, v1.y, v2.y});
        int max_y = (int)std::max({v0.y, v1.y, v2.y});
        int min_x = (int)std::min({v0.x, v1.x, v2.x});
        int max_x = (int)std::max({v0.x, v1.x, v2.x});

        // Create image for gradient rendering (more efficient than DrawPoint)
        const int img_w = sz.GetWidth();
        const int img_h = sz.GetHeight();
        wxImage img(img_w, img_h);
        
        // Fill with background color matching card
        const wxColour bg = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
        img.SetRGB(wxRect(0, 0, img_w, img_h), bg.Red(), bg.Green(), bg.Blue());
        unsigned char* data = img.GetData();

        // Draw gradient only inside triangle
        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                TriPt p = {(double)px, (double)py};
                double w0, w1, w2;
                tri_bary(p, v0, v1, v2, w0, w1, w2);
                // Skip points outside triangle (negative barycentric coordinates)
                constexpr double EPSILON = 0.001;
                if (w0 < -EPSILON || w1 < -EPSILON || w2 < -EPSILON) continue;
                if (px < 0 || px >= img_w || py < 0 || py >= img_h) continue;

                const int idx = (py * img_w + px) * 3;
                data[idx]     = (unsigned char)std::clamp((int)(c0.Red()   * w0 + c1.Red()   * w1 + c2.Red()   * w2), 0, 255);
                data[idx + 1] = (unsigned char)std::clamp((int)(c0.Green() * w0 + c1.Green() * w1 + c2.Green() * w2), 0, 255);
                data[idx + 2] = (unsigned char)std::clamp((int)(c0.Blue()  * w0 + c1.Blue()  * w1 + c2.Blue()  * w2), 0, 255);
            }
        }

        // Draw the image
        wxBitmap bmp(img);
        dc.DrawBitmap(bmp, 0, 0);

        // Draw triangle outline (#D9D9D9 per Figma)
        wxPoint pts[3] = {{(int)v0.x, (int)v0.y}, {(int)v1.x, (int)v1.y}, {(int)v2.x, (int)v2.y}};
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#D9D9D9")), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawPolygon(3, pts);

        // Draw colored circles at each vertex (6×6, #D9D9D9 border per Figma)
        const int dot_r = FromDIP(3);
        wxPen dot_border(StateColor::darkModeColorFor(wxColour("#D9D9D9")), 1);
        dc.SetPen(dot_border);
        dc.SetBrush(wxBrush(c0));
        dc.DrawCircle(wxPoint((int)v0.x, (int)v0.y), dot_r);
        dc.SetBrush(wxBrush(c1));
        dc.DrawCircle(wxPoint((int)v1.x, (int)v1.y), dot_r);
        dc.SetBrush(wxBrush(c2));
        dc.DrawCircle(wxPoint((int)v2.x, (int)v2.y), dot_r);

        // Draw selection circle (transparent fill, white border per Figma)
        double hx = m_tri_wx*v0.x + m_tri_wy*v1.x + m_tri_wz*v2.x;
        double hy = m_tri_wx*v0.y + m_tri_wy*v1.y + m_tri_wz*v2.y;
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#FFFFFF")), 1));
        dc.DrawCircle((int)hx, (int)hy, FromDIP(5));

    });

    auto handle_mouse = [this, get_verts](wxMouseEvent& e, bool is_down) {
        auto [v0, v1, v2] = get_verts();
        TriPt p = {(double)e.GetX(), (double)e.GetY()};
        if (is_down) {
            m_tri_dragging = true;
            m_tri_picker->CaptureMouse();
        }
        if (!m_tri_dragging) return;
        TriPt clamped = tri_clamp_pt(p, v0, v1, v2);
        tri_bary(clamped, v0, v1, v2, m_tri_wx, m_tri_wy, m_tri_wz);
        // Clamp each weight to be at least 10% and at most 90%, iterate to renormalize
        constexpr double MIN_WEIGHT = 0.10;
        constexpr double MAX_WEIGHT = 0.90;
        for (int i = 0; i < 4; ++i) {
            m_tri_wx = std::clamp(m_tri_wx, MIN_WEIGHT, MAX_WEIGHT);
            m_tri_wy = std::clamp(m_tri_wy, MIN_WEIGHT, MAX_WEIGHT);
            m_tri_wz = std::clamp(m_tri_wz, MIN_WEIGHT, MAX_WEIGHT);
            double sum = m_tri_wx + m_tri_wy + m_tri_wz;
            if (sum > 0) { m_tri_wx /= sum; m_tri_wy /= sum; m_tri_wz /= sum; }
        }
        update_preview();
        m_tri_picker->Refresh();
    };

    m_tri_picker->Bind(wxEVT_LEFT_DOWN, [handle_mouse](wxMouseEvent& e) { handle_mouse(e, true); });
    m_tri_picker->Bind(wxEVT_MOTION,    [handle_mouse](wxMouseEvent& e) { handle_mouse(e, false); });
    m_tri_picker->Bind(wxEVT_LEFT_UP,   [this](wxMouseEvent&) {
        if (m_tri_dragging) {
            m_tri_dragging = false;
            if (m_tri_picker->HasCapture()) m_tri_picker->ReleaseMouse();
            update_compatibility_warning();
        }
    });
    m_tri_picker->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_tri_dragging = false;
    });
}

void MixedFilamentDialog::build_match_tri_picker(wxWindow* parent)
{
    static constexpr int TRI_SIZE = 160;
    m_match_tri_picker = new wxPanel(parent, wxID_ANY, wxDefaultPosition,
                                     wxSize(FromDIP(TRI_SIZE), FromDIP(TRI_SIZE)));
    m_match_tri_picker->SetMinSize(wxSize(FromDIP(TRI_SIZE), FromDIP(TRI_SIZE)));
    m_match_tri_picker->SetBackgroundStyle(wxBG_STYLE_PAINT);

    auto get_verts = [this]() -> std::tuple<TriPt, TriPt, TriPt> {
        wxSize sz = m_match_tri_picker->GetClientSize();
        double pw = sz.GetWidth(), ph = sz.GetHeight();
        double margin = FromDIP(3);
        double side = std::min(pw, ph) - 2 * margin;
        double tri_h = side * std::sqrt(3.0) / 2.0;
        double cx = pw / 2.0;
        double top_y = (ph - tri_h) / 2.0;
        double bot_y = top_y + tri_h;
        return { {cx, top_y}, {cx - side / 2.0, bot_y}, {cx + side / 2.0, bot_y} };
    };

    auto safe_col = [this](int row) -> wxColour {
        if (row >= (int)m_match_tri_indices.size()) return wxColour(128,128,128);
        int s = m_match_tri_indices[row];
        s = std::max(0, std::min(s, (int)m_filament_colours.size()-1));
        return parse_mixed_color(m_filament_colours[s]);
    };

    m_match_tri_picker->Bind(wxEVT_PAINT, [this, get_verts, safe_col](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_match_tri_picker);
        wxSize sz = m_match_tri_picker->GetClientSize();
        auto [v0, v1, v2] = get_verts();
        wxColour c0 = safe_col(0), c1 = safe_col(1), c2 = safe_col(2);

        int min_y = (int)std::min({v0.y, v1.y, v2.y});
        int max_y = (int)std::max({v0.y, v1.y, v2.y});
        int min_x = (int)std::min({v0.x, v1.x, v2.x});
        int max_x = (int)std::max({v0.x, v1.x, v2.x});

        const int img_w = sz.GetWidth(), img_h = sz.GetHeight();
        wxImage img(img_w, img_h);
        const wxColour bg = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
        img.SetRGB(wxRect(0, 0, img_w, img_h), bg.Red(), bg.Green(), bg.Blue());
        unsigned char* data = img.GetData();

        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                TriPt p = {(double)px, (double)py};
                double w0, w1, w2;
                tri_bary(p, v0, v1, v2, w0, w1, w2);
                constexpr double EPSILON = 0.001;
                if (w0 < -EPSILON || w1 < -EPSILON || w2 < -EPSILON) continue;
                if (px < 0 || px >= img_w || py < 0 || py >= img_h) continue;
                const int idx = (py * img_w + px) * 3;
                data[idx]     = (unsigned char)std::clamp((int)(c0.Red()   * w0 + c1.Red()   * w1 + c2.Red()   * w2), 0, 255);
                data[idx + 1] = (unsigned char)std::clamp((int)(c0.Green() * w0 + c1.Green() * w1 + c2.Green() * w2), 0, 255);
                data[idx + 2] = (unsigned char)std::clamp((int)(c0.Blue()  * w0 + c1.Blue()  * w1 + c2.Blue()  * w2), 0, 255);
            }
        }
        wxBitmap bmp(img);
        dc.DrawBitmap(bmp, 0, 0);

        wxPoint pts[3] = {{(int)v0.x, (int)v0.y}, {(int)v1.x, (int)v1.y}, {(int)v2.x, (int)v2.y}};
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#D9D9D9")), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawPolygon(3, pts);

        // Colored circles at each vertex (6×6, #D9D9D9 border)
        const int dot_r = FromDIP(3);
        wxPen dot_border(StateColor::darkModeColorFor(wxColour("#D9D9D9")), 1);
        dc.SetPen(dot_border);
        dc.SetBrush(wxBrush(c0));
        dc.DrawCircle(wxPoint((int)v0.x, (int)v0.y), dot_r);
        dc.SetBrush(wxBrush(c1));
        dc.DrawCircle(wxPoint((int)v1.x, (int)v1.y), dot_r);
        dc.SetBrush(wxBrush(c2));
        dc.DrawCircle(wxPoint((int)v2.x, (int)v2.y), dot_r);

        // Selection circle (transparent fill, white border)
        double hx = m_match_tri_wx*v0.x + m_match_tri_wy*v1.x + m_match_tri_wz*v2.x;
        double hy = m_match_tri_wx*v0.y + m_match_tri_wy*v1.y + m_match_tri_wz*v2.y;
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#FFFFFF")), 1));
        dc.DrawCircle((int)hx, (int)hy, FromDIP(5));
    });

    auto handle_mouse = [this, get_verts](wxMouseEvent& e, bool is_down) {
        auto [v0, v1, v2] = get_verts();
        TriPt p = {(double)e.GetX(), (double)e.GetY()};
        if (is_down) { m_match_tri_dragging = true; m_match_tri_picker->CaptureMouse(); }
        if (!m_match_tri_dragging) return;
        TriPt clamped = tri_clamp_pt(p, v0, v1, v2);
        tri_bary(clamped, v0, v1, v2, m_match_tri_wx, m_match_tri_wy, m_match_tri_wz);
        // Use dynamic min_component_percent (0-50%) instead of hard-coded 10%
        {
            const double min_w = std::max(0.0, m_match_min_pct / 100.0);
            const double max_w = 1.0 - 2.0 * min_w;  // each weight ≤ 100% - 2x min (leaves room for 2 others)
            m_match_tri_wx = std::clamp(m_match_tri_wx, min_w, max_w);
            m_match_tri_wy = std::clamp(m_match_tri_wy, min_w, max_w);
            m_match_tri_wz = std::clamp(m_match_tri_wz, min_w, max_w);
            double sum = m_match_tri_wx + m_match_tri_wy + m_match_tri_wz;
            if (sum > 0) { m_match_tri_wx /= sum; m_match_tri_wy /= sum; m_match_tri_wz /= sum; }
        }
        update_match_legend_labels();
        if (m_match_strip_panel) m_match_strip_panel->Refresh();
        if (m_match_blend_panel) m_match_blend_panel->Refresh();
        m_match_tri_picker->Refresh();
    };

    m_match_tri_picker->Bind(wxEVT_LEFT_DOWN, [handle_mouse](wxMouseEvent& e) { handle_mouse(e, true); });
    m_match_tri_picker->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        m_match_tri_dragging = false;
        if (m_match_tri_picker->HasCapture()) m_match_tri_picker->ReleaseMouse();
        update_compatibility_warning();
    });
    m_match_tri_picker->Bind(wxEVT_MOTION, [handle_mouse](wxMouseEvent& e) { handle_mouse(e, false); });
    m_match_tri_picker->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_match_tri_dragging = false;
    });
}

void MixedFilamentDialog::update_ratio_or_tri_visibility()
{
    bool is_ratio_mode = (m_current_mode == MODE_RATIO);
    bool is_cycle_mode = (m_current_mode == MODE_CYCLE);
    bool is_match_mode = (m_current_mode == MODE_MATCH);
    bool is_gradient_mode = (m_current_mode == MODE_GRADIENT);
    int  n             = (int)m_filament_rows.size();
    bool show_slider   = is_ratio_mode && (n == 2);
    bool show_tri      = is_ratio_mode && (n == 3);

    // Card A: Filament Selection card
    bool show_filament_rows = !is_match_mode && !is_cycle_mode;
    if (m_filament_card)      m_filament_card->Show(show_filament_rows);
    if (m_filament_rows_panel) m_filament_rows_panel->Show(show_filament_rows);

    // Card 1: Match input card (match mode only)
    if (m_match_input_card) m_match_input_card->Show(is_match_mode);

    // Card B: Ratio card — shown for both 2-color and 3-color ratio modes
    if (m_ratio_card) {
        bool show_ratio_card = is_ratio_mode && !is_match_mode && !is_gradient_mode;
        m_ratio_card->Show(show_ratio_card);
    }
    // Show/hide gradient bar vs tri-picker within card
    if (m_ratio_gradient_spacer) m_ratio_gradient_spacer->Show(show_slider);
    if (m_gradient_selector)     m_gradient_selector->Show(show_slider);
    if (m_ratio_tri_spacer)      m_ratio_tri_spacer->Show(show_tri);
    if (m_tri_picker)            m_tri_picker->Show(show_tri);
    // Gradient effect card (shown in gradient mode)
    if (m_gradient_effect_card)
        m_gradient_effect_card->Show(is_gradient_mode && !is_match_mode);

    // Match Mix Ratio card (match mode only, separate from ratio card)
    if (m_match_ratio_card) m_match_ratio_card->Show(is_match_mode);
    if (is_match_mode) {
        int n_tri = (int)m_match_tri_indices.size();
        bool match_show_slider = (n_tri == 2);
        bool match_show_tri    = (n_tri >= 3);
        if (m_match_gradient_spacer)   m_match_gradient_spacer->Show(match_show_slider);
        if (m_match_gradient_selector) m_match_gradient_selector->Show(match_show_slider);
        if (m_match_tri_spacer)        m_match_tri_spacer->Show(match_show_tri);
        if (m_match_tri_picker)        m_match_tri_picker->Show(match_show_tri);
    }

    // Cycle card
    if (m_cycle_card) m_cycle_card->Show(is_cycle_mode);

    // Match panel UI: never shown (backend only)
    if (m_match_panel_sizer) m_match_panel_sizer->ShowItems(false);

    // Card C: Swatches card
    bool show_swatches = !is_cycle_mode;
    if (m_swatch_card) m_swatch_card->Show(show_swatches);

    // Update add/remove button visibility
    bool can_remove = !is_match_mode && !is_gradient_mode && !is_cycle_mode && (n > 2);
    bool show_add = !is_match_mode && !is_gradient_mode && !is_cycle_mode && n < max_filaments_for_mode(m_current_mode);
    bool can_add = n < (int)m_filament_colours.size();
    if (m_btn_remove_filament) m_btn_remove_filament->Show(can_remove);
    if (m_btn_add_filament) {
        m_btn_add_filament->Show(show_add);
        m_btn_add_filament->Enable(can_add);
    }

    // Update filament card title visibility
    if (m_filament_card_title) m_filament_card_title->Show(show_filament_rows);

    const bool show_gradient_swap = is_gradient_mode && (n == 2);
    if (m_btn_swap_gradient_dir)
        m_btn_swap_gradient_dir->Show(show_gradient_swap);

    // Refresh match card layout when gradient/tri picker visibility changes
    if (is_match_mode && m_match_ratio_card) {
        m_match_ratio_card->Layout();
        m_match_ratio_card->Refresh();
        if (m_scrolled_content) m_scrolled_content->Layout();
        Layout();
        // Force badge panel repaint after re-layout prevents StaticBox bg from covering badges
        if (m_match_input_card) m_match_input_card->Refresh();
        if (m_match_badges_panel) m_match_badges_panel->Refresh();
    }
}

void MixedFilamentDialog::resize_gradient_ids(int target_count)
{
    int extra = target_count - 2;
    if (extra <= 0) { m_result.gradient_component_ids.clear(); return; }

    // Decode existing gradient IDs
    std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(m_result.gradient_component_ids);

    // Build set of already used filament IDs (1-based)
    std::set<unsigned int> used;
    used.insert(m_result.component_a);
    used.insert(m_result.component_b);
    for (unsigned int id : ids)
        used.insert(id);

    // Find unused filaments for new slots
    while ((int)ids.size() < extra) {
        unsigned int new_filament = 0;
        for (int j = 0; j < (int)m_filament_colours.size(); ++j) {
            unsigned int fid = (unsigned int)(j + 1);
            if (used.find(fid) == used.end()) {
                new_filament = fid;
                used.insert(fid);
                break;
            }
        }
        if (new_filament >= 1)
            ids.push_back(new_filament);
        else
            break;  // No unused filament available, stop filling
    }
    ids.resize((size_t)extra);
    m_result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
}

void MixedFilamentDialog::sync_rows_to_result()
{
    if (m_filament_rows.empty()) return;
    int a = get_filament_index(0);
    int b = (m_filament_rows.size() > 1) ? get_filament_index(1) : a;
    m_result.component_a = (unsigned int)(std::max(0, a) + 1);
    m_result.component_b = (unsigned int)(std::max(0, b) + 1);
    std::vector<unsigned int> extra_ids;
    for (int i = 2; i < (int)m_filament_rows.size(); ++i) {
        int s = get_filament_index(i);
        if (s >= 0)
            extra_ids.push_back((unsigned int)(s + 1));
    }
    m_result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(extra_ids);
}

void MixedFilamentDialog::update_compatibility_warning()
{
    if (!m_error_panel || !m_error_text || !m_warning_panel || !m_warning_text || !m_btn_confirm)
        return;

    // Collect all filament IDs referenced by the current mix
    std::vector<unsigned int> fids;

    if (m_current_mode == MODE_MATCH) {
        if (m_match_panel && m_match_panel->has_valid_recipe()) {
            auto recipe = m_match_panel->selected_recipe();
            if (recipe.component_a >= 1) fids.push_back(recipe.component_a - 1);
            if (recipe.component_b >= 1) fids.push_back(recipe.component_b - 1);
            if (!recipe.gradient_component_ids.empty()) {
                for (unsigned int id : MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids)) {
                    if (id >= 1) fids.push_back(id - 1);
                }
            }
        } else {
            for (int idx : m_match_tri_indices)
                fids.push_back((unsigned int)idx);
        }
    } else if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        sync_rows_to_result();
        const std::string raw = into_u8(m_pattern_ctrl->GetValue());
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(raw);
        auto parsed = parse_cycle_pattern(norm, (int)m_filament_colours.size());
        for (unsigned int id : parsed.ids) {
            unsigned int idx = id - 1;
            if (std::find(fids.begin(), fids.end(), idx) == fids.end())
                fids.push_back(idx);
        }
    } else {
        sync_rows_to_result();
        if (m_result.component_a >= 1) fids.push_back(m_result.component_a - 1);
        if (m_result.component_b >= 1) fids.push_back(m_result.component_b - 1);
        if (!m_result.gradient_component_ids.empty()) {
            for (unsigned int id : MixedFilamentManager::decode_gradient_component_ids(m_result.gradient_component_ids)) {
                if (id >= 1) fids.push_back(id - 1);
            }
        }
    }

    if (!is_filament_compatible(fids)) {
        if (auto pair = find_incompatible_filament_pair(fids)) {
            set_error(wxString::Format(_L("Filament %d and Filament %d cannot be mixed. Please select filaments of the same type."), pair->first, pair->second));
        } else {
            set_error(_L("Different filament types cannot be mixed. Please correct the settings."));
        }
    } else if (wxString warning_msg = get_ratio_warning_msg(); !warning_msg.empty()) {
        display_warning(warning_msg);
        if (m_btn_confirm) m_btn_confirm->Enable();
    } else if (m_current_mode == MODE_CYCLE && fids.size() == 1) {
        display_warning(_L("Same filament colors cannot produce new colors. Please select different colors for mixing."));
        if (m_btn_confirm) m_btn_confirm->Enable();
    } else if (m_current_mode == MODE_CYCLE && fids.size() > 4) {
        display_warning(_L("Excessive filaments in the mix may affect the result. Please use with caution."));
        if (m_btn_confirm) m_btn_confirm->Enable();
    } else {
        m_error_panel->Hide();
        m_warning_panel->Hide();
        m_btn_confirm->Enable();
    }
    Layout();
}

void MixedFilamentDialog::display_warning(const wxString& msg)
{
    if (!m_warning_panel || !m_warning_text || !m_error_panel)
        return;
    m_error_panel->Hide();
    m_warning_panel->Show();
    Layout(); // land panel width first, so the auto-wrap label's CalcMin gets a real width
    m_warning_text->SetLabel(msg);
    Layout(); // re-wrap with correct width; single Layout on macOS may calc height from stale zero-width
}

void MixedFilamentDialog::set_error(const wxString& msg)
{
    if (!m_error_panel || !m_error_text || !m_warning_panel)
        return;
    m_warning_panel->Hide();
    m_error_panel->Show();
    Layout(); // land panel width first, so the auto-wrap label's CalcMin gets a real width
    m_error_text->SetLabel(msg);
    if (m_btn_confirm) m_btn_confirm->Disable();
    Layout(); // re-wrap with correct width; single Layout on macOS may calc height from stale zero-width
}

wxString MixedFilamentDialog::get_ratio_warning_msg()
{
    static constexpr double HIGH_RATIO_THRESHOLD = 0.667;

    if (m_filament_rows.empty() || m_filament_colours.empty())
        return wxEmptyString;

    const int num_physical = (int)m_filament_colours.size();
    std::vector<double> ratios(num_physical, 0.0);
    double total = 0.0;

    switch (m_current_mode) {
    case MODE_RATIO:
        if (m_filament_rows.size() == 2 && m_gradient_selector) {
            int val = m_gradient_selector->value();
            int ia = get_filament_index(0);
            int ib = get_filament_index(1);
            if (ia < num_physical) { ratios[ia] = (100.0 - val) / 100.0; total += ratios[ia]; }
            if (ib < num_physical) { ratios[ib] = val / 100.0;        total += ratios[ib]; }
        } else if (m_filament_rows.size() == 3) {
            for (int i = 0; i < 3; ++i) {
                int idx = get_filament_index(i);
                if (idx < num_physical) {
                    double w = (i == 0) ? m_tri_wx : (i == 1) ? m_tri_wy : m_tri_wz;
                    ratios[idx] = w;
                    total += w;
                }
            }
        }
        break;
    case MODE_GRADIENT:
        if (m_filament_rows.size() >= 2) {
            int ia = get_filament_index(0);
            int ib = get_filament_index(1);
            if (ia < num_physical) { ratios[ia] = 0.5; total += 0.5; }
            if (ib < num_physical) { ratios[ib] = 0.5; total += 0.5; }
        }
        break;
    case MODE_MATCH:
        {
            int n_tri = (int)m_match_tri_indices.size();
            if (n_tri == 2) {
                double w0, w1;
                if (m_match_gradient_selector) {
                    int val = m_match_gradient_selector->value();
                    w0 = (100.0 - val) / 100.0;
                    w1 = val / 100.0;
                } else {
                    w0 = m_match_tri_wx;
                    w1 = m_match_tri_wy;
                }
                for (int i = 0; i < 2 && i < n_tri; ++i) {
                    int idx = std::clamp(m_match_tri_indices[i], 0, num_physical - 1);
                    double w = (i == 0) ? w0 : w1;
                    ratios[idx] = w;
                    total += w;
                }
            } else if (n_tri >= 3) {
                for (int i = 0; i < n_tri && i < 3; ++i) {
                    int idx = std::clamp(m_match_tri_indices[i], 0, num_physical - 1);
                    double w = (i == 0) ? m_match_tri_wx : (i == 1) ? m_match_tri_wy : m_match_tri_wz;
                    ratios[idx] = w;
                    total += w;
                }
            }
        }
        break;
    case MODE_CYCLE:
        return wxEmptyString;
    }

    if (total <= 0.0)
        return wxEmptyString;

    double max_ratio = 0.0;
    int    max_idx   = -1;
    for (int i = 0; i < num_physical; ++i) {
        double ratio = ratios[i] / total;
        if (ratio > max_ratio) {
            max_ratio = ratio;
            max_idx   = i;
        }
    }
    if (max_idx >= 0 && max_ratio > HIGH_RATIO_THRESHOLD) {
        return wxString::Format(_L("Filament %d ratio is too high. Mix may be affected."), max_idx + 1);

    }

    return wxEmptyString;
}

std::string MixedFilamentDialog::compute_preview_color()
{
    if (m_filament_colours.empty()) return "#808080";
    if (m_filament_rows.empty() && m_current_mode != MODE_MATCH) return "#808080";

    // Match mode: compute from current tri/gradient weights (during drag or no recipe)
    if (m_current_mode == MODE_MATCH && !m_match_tri_indices.empty()) {
        int nf = (int)m_match_tri_indices.size();
        if (nf == 2 && m_match_gradient_selector) {
            int val = m_match_gradient_selector->value();
            int ia = std::clamp(m_match_tri_indices[0], 0, (int)m_filament_colours.size() - 1);
            int ib = std::clamp(m_match_tri_indices[1], 0, (int)m_filament_colours.size() - 1);
            return MixedFilamentManager::blend_color(m_filament_colours[ia], m_filament_colours[ib], 100 - val, val);
        }
        if (nf >= 3) {
            wxColour c0 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[0], 0, (int)m_filament_colours.size() - 1)]);
            wxColour c1 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[1], 0, (int)m_filament_colours.size() - 1)]);
            wxColour c2 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[2], 0, (int)m_filament_colours.size() - 1)]);
            int r = (int)(c0.Red()*m_match_tri_wx + c1.Red()*m_match_tri_wy + c2.Red()*m_match_tri_wz + 0.5);
            int g = (int)(c0.Green()*m_match_tri_wx + c1.Green()*m_match_tri_wy + c2.Green()*m_match_tri_wz + 0.5);
            int b = (int)(c0.Blue()*m_match_tri_wx + c1.Blue()*m_match_tri_wy + c2.Blue()*m_match_tri_wz + 0.5);
            return wxString::Format("#%02X%02X%02X", std::clamp(r,0,255), std::clamp(g,0,255), std::clamp(b,0,255)).ToStdString();
        }
    }

    int n   = (int)m_filament_rows.size();
    int val = m_gradient_selector ? m_gradient_selector->value() : 50;

    auto safe_idx = [&](int raw) {
        return std::max(0, std::min(raw, (int)m_filament_colours.size() - 1));
    };

    // Cycle mode: blend by pattern frequency using the same logic as Plater.cpp
    if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        const std::string raw = into_u8(m_pattern_ctrl->GetValue());
        const std::string normalized = MixedFilamentManager::normalize_manual_pattern(raw);
        if (!normalized.empty()) {
            const size_t num_physical = m_filament_colours.size();

            // In cycle mode, tokens "1"/"2" map directly to physical filaments 1/2,
            // and tokens "3".."9" map directly to physical filaments 3..9.
            std::vector<unsigned int> sequence;
            MixedFilament dummy_mf;
            dummy_mf.component_a = 1;
            dummy_mf.component_b = 2;
            const std::vector<std::string> group_strs = MixedFilamentManager::split_pattern_groups(normalized);
            for (const std::string &group : group_strs) {
                const std::vector<std::string> tokens =
                    MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
                for (const std::string &token : tokens) {
                    const unsigned int extruder_id =
                        MixedFilamentManager::physical_filament_from_token(token, dummy_mf, num_physical);
                    if (extruder_id >= 1 && extruder_id <= num_physical)
                        sequence.push_back(extruder_id);
                }
            }

            if (!sequence.empty()) {
                std::vector<wxColour> palette;
                palette.reserve(m_filament_colours.size());
                for (const auto& s : m_filament_colours)
                    palette.push_back(parse_mixed_color(s));
                wxColour result = blend_sequence_filament_mixer(palette, sequence);
                return wxString::Format("#%02X%02X%02X", result.Red(), result.Green(), result.Blue()).ToStdString();
            }
        }
    }

    if (n == 2) {
        int ia = safe_idx(get_filament_index(0));
        int ib = safe_idx(get_filament_index(1));
        int w = (m_current_mode == MODE_RATIO) ? val : 50;
        return MixedFilamentManager::blend_color(
            m_filament_colours[ia], m_filament_colours[ib], 100 - w, w);
    }

    // 3-filament ratio mode: linear weighted average matching tri picker rendering
    if (n == 3 && m_current_mode == MODE_RATIO) {
        auto get_col = [&](int row) {
            return parse_mixed_color(m_filament_colours[safe_idx(get_filament_index(row))]);
        };
        wxColour c0 = get_col(0), c1 = get_col(1), c2 = get_col(2);
        int r = (int)(c0.Red()   * m_tri_wx + c1.Red()   * m_tri_wy + c2.Red()   * m_tri_wz + 0.5);
        int g = (int)(c0.Green() * m_tri_wx + c1.Green() * m_tri_wy + c2.Green() * m_tri_wz + 0.5);
        int b = (int)(c0.Blue()  * m_tri_wx + c1.Blue()  * m_tri_wy + c2.Blue()  * m_tri_wz + 0.5);
        return wxString::Format("#%02X%02X%02X",
            std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)).ToStdString();
    }

    int per = 100 / n;
    int rem = 100 - per * n;
    std::vector<std::pair<std::string, int>> cp;
    for (int i = 0; i < n; ++i) {
        int idx = safe_idx(get_filament_index(i));
        cp.push_back(std::make_pair(m_filament_colours[idx], per + (i == 0 ? rem : 0)));
    }
    return MixedFilamentManager::blend_color_multi(cp);
}

void MixedFilamentDialog::draw_strip(wxDC& dc, wxPanel* panel)
{
    wxSize sz = panel->GetClientSize();
    if (sz.x <= 0 || sz.y <= 0) { dc.SetBackground(*wxGREY_BRUSH); dc.Clear(); return; }
    if (m_filament_rows.empty() && m_current_mode != MODE_MATCH) {
        dc.SetBackground(*wxGREY_BRUSH); dc.Clear(); return;
    }
    int n = (int)m_filament_rows.size();
    static constexpr int STRIP_SEGMENTS = 20;
    bool vertical = (m_current_mode == MODE_RATIO && (n == 2 || n == 3))
                 || (m_current_mode == MODE_CYCLE)
                 || (m_current_mode == MODE_MATCH);
    int total_px = vertical ? sz.y : sz.x;

    // In 2-color ratio mode, build a pattern reflecting the A:B ratio
    std::vector<int> pattern;
    if (m_current_mode == MODE_RATIO && n == 2 && m_gradient_selector) {
        int val = m_gradient_selector->value();
        const int pct_b = std::clamp(val, 0, 100);
        const int pct_a = 100 - pct_b;
        int ratio_a = 1, ratio_b = 0;
        if (pct_b >= 100) {
            ratio_a = 0; ratio_b = 1;
        } else if (pct_b > 0) {
            const bool b_major = pct_b >= pct_a;
            const int major = b_major ? pct_b : pct_a;
            const int minor = b_major ? pct_a : pct_b;
            const int g = std::gcd(major, minor);
            ratio_a = b_major ? (minor / g) : (major / g);
            ratio_b = b_major ? (major / g) : (minor / g);
            // Cap total stripes to 20 for visual clarity in preview
            if (ratio_a + ratio_b > 20) {
                const double scale = 20.0 / (ratio_a + ratio_b);
                ratio_a = std::max(1, (int)std::round(ratio_a * scale));
                ratio_b = std::max(1, (int)std::round(ratio_b * scale));
            }
        }
        const int cycle = std::max(1, ratio_a + ratio_b);
        for (int pos = 0; pos < cycle; ++pos) {
            const int b_before = (pos * ratio_b) / cycle;
            const int b_after  = ((pos + 1) * ratio_b) / cycle;
            pattern.push_back((b_after > b_before) ? 1 : 0);
        }
    } else if (m_current_mode == MODE_RATIO && n == 3) {
        int w0 = std::max(1, (int)std::round(m_tri_wx * 100));
        int w1 = std::max(1, (int)std::round(m_tri_wy * 100));
        int w2 = std::max(1, 100 - w0 - w1);
        int g = std::gcd(std::gcd(w0, w1), w2);
        if (g > 1) { w0 /= g; w1 /= g; w2 /= g; }
        const int counts[3] = {w0, w1, w2};
        const int total = w0 + w1 + w2;
        int emitted[3] = {0, 0, 0};
        for (int pos = 0; pos < total; ++pos) {
            int best = 0;
            double best_score = -1e9;
            for (int i = 0; i < 3; ++i) {
                double score = double(pos + 1) * double(counts[i]) / double(total) - double(emitted[i]);
                if (score > best_score) { best_score = score; best = i; }
            }
            ++emitted[best];
            pattern.push_back(best);
        }
    } else if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        // Decode pattern using the same logic as MixedFilamentManager
        const std::string raw = into_u8(m_pattern_ctrl->GetValue());
        const std::string normalized = MixedFilamentManager::normalize_manual_pattern(raw);
        const size_t num_physical = m_filament_colours.size();

        // In cycle mode, tokens "1"/"2" map directly to physical filaments 1/2.
        MixedFilament dummy_mf;
        dummy_mf.component_a = 1;
        dummy_mf.component_b = 2;
        const std::vector<std::string> group_strs = MixedFilamentManager::split_pattern_groups(normalized);
        for (const std::string &group : group_strs) {
            const std::vector<std::string> tokens =
                MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
            for (const std::string &token : tokens) {
                const unsigned int extruder_id =
                    MixedFilamentManager::physical_filament_from_token(token, dummy_mf, num_physical);
                if (extruder_id >= 1 && extruder_id <= num_physical)
                    pattern.push_back(-(int)extruder_id);
            }
        }
        if (pattern.empty())
            for (int i = 0; i < n; ++i) pattern.push_back(i);
    } else if (m_current_mode == MODE_MATCH) {
        // Use current tri weights directly (not recipe, to reflect drag state)
        if (!m_match_tri_indices.empty()) {
            // Build pattern from current tri weights, skip zero-weight filaments
            {
                std::vector<double> wts = {m_match_tri_wx, m_match_tri_wy};
                if (m_match_tri_indices.size() >= 3) wts.push_back(m_match_tri_wz);
                std::vector<int> counts, ids;
                for (int i = 0; i < (int)wts.size() && i < (int)m_match_tri_indices.size(); ++i) {
                    int cnt = (int)std::round(wts[i] * 20.0);
                    if (cnt > 0) {
                        counts.push_back(cnt);
                        ids.push_back(m_match_tri_indices[i]);
                    }
                }
                if (!counts.empty()) {
                    int total = std::accumulate(counts.begin(), counts.end(), 0);
                    std::vector<int> emitted(counts.size(), 0);
                    for (int pos = 0; pos < total; ++pos) {
                        int best = 0; double best_score = -1e9;
                        for (int i = 0; i < (int)counts.size(); ++i) {
                            double score = double(pos + 1) * double(counts[i]) / double(total) - double(emitted[i]);
                            if (score > best_score) { best_score = score; best = i; }
                        }
                        ++emitted[best];
                        pattern.push_back(-(ids[best] + 1));
                    }
                }
            }
        }
        if (pattern.empty()) {
            for (int i = 0; i < (int)m_filament_colours.size(); ++i)
                pattern.push_back(-(i + 1));
        }
    } else {
        for (int i = 0; i < n; ++i) pattern.push_back(i);
    }

    if (pattern.empty())
        return;

    for (int s = 0; s < STRIP_SEGMENTS; ++s) {
        int entry = pattern[s % (int)pattern.size()];
        int idx;
        if (entry < 0) {
            idx = std::max(0, std::min(-entry - 1, (int)m_filament_colours.size() - 1));
        } else {
            int row = std::min(entry, n - 1);
            idx = get_filament_index(row);
            idx = std::max(0, std::min(idx, (int)m_filament_colours.size() - 1));
        }
        dc.SetBrush(wxBrush(parse_mixed_color(m_filament_colours[idx])));
        dc.SetPen(*wxTRANSPARENT_PEN);
        if (vertical) {
            int y0 = s * total_px / STRIP_SEGMENTS;
            int y1 = (s + 1) * total_px / STRIP_SEGMENTS;
            dc.DrawRectangle(0, total_px - y1, sz.x, y1 - y0);
        }
        else {
            int x0 = s * total_px / STRIP_SEGMENTS;
            int x1 = (s + 1) * total_px / STRIP_SEGMENTS;
            dc.DrawRectangle(x0, 0, x1 - x0, sz.y);
        }
    }

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(180, 180, 180)), 1));
    dc.DrawRectangle(0, 0, sz.x, sz.y);
}

void MixedFilamentDialog::build_swatch_grid()
{
    int n = (int)m_filament_colours.size();
    if (n < 2) return;

    // Limit recommended swatches to the first 6 physical filaments.
    constexpr int kMaxSwatchFilaments = 6;

    bool is_ratio_3 = (m_current_mode == MODE_RATIO) && ((int)m_filament_rows.size() == 3);
    if(is_ratio_3) {
        // In 3-row ratio mode, we can show triple combinations, but limit to the first 6 filaments to avoid combinatorial explosion.
        n = std::min(n, kMaxSwatchFilaments);
    }

    // Build candidates using the same preset logic as match mode.
    // For ratio mode with 2 rows: pair candidates at 25/50/75.
    // For ratio mode with 3 rows: pair + triple candidates.
    // For other modes: pairs at 50 only.
    struct Candidate {
        wxColour color;
        wxString tooltip;
        // For pair: row indices and b_pct. For triple: row indices and weights.
        int rows[3] = {0, 0, 0};
        int n_rows   = 2;
        int b_pct    = 50;       // used when n_rows == 2
        double wx    = 1.0/3.0;  // used when n_rows == 3
        double wy    = 1.0/3.0;
        double wz    = 1.0/3.0;
    };

    std::vector<Candidate> candidates;

    if (m_current_mode == MODE_RATIO) {
        std::vector<wxColour> palette;
        palette.reserve(n);
        for (const auto& s : m_filament_colours)
            palette.push_back(parse_mixed_color(s));

        if (is_ratio_3) {
            // Three-color mode: only show triple candidates
            auto make_triple_candidate = [&](int i, int j, int k,
                                             const std::vector<int>& input_weights) -> Candidate {
                std::vector<unsigned int> ids = {(unsigned)(i+1), (unsigned)(j+1), (unsigned)(k+1)};
                auto recipe = build_multi_color_match_candidate(palette, ids, input_weights);
                if (!recipe.valid) return {};

                Candidate c;
                c.color  = recipe.preview_color;
                c.n_rows = 3;
                // Use original input order for rows (i, j, k correspond to rows 0, 1, 2)
                c.rows[0] = i;
                c.rows[1] = j;
                c.rows[2] = k;
                // Weights correspond to the original input order
                c.wx = input_weights[0] / 100.0;
                c.wy = input_weights[1] / 100.0;
                c.wz = input_weights[2] / 100.0;
                c.tooltip = wxString::Format("F%d(%d%%)+F%d(%d%%)+F%d(%d%%)",
                    i+1, input_weights[0],
                    j+1, input_weights[1],
                    k+1, input_weights[2]);
                return c;
            };

            const std::vector<int> eq = normalize_color_match_weights({1, 1, 1}, 3);
            for (int i = 0; i < n; ++i) {
                for (int j = i + 1; j < n; ++j) {
                    for (int k = j + 1; k < n; ++k) {
                        auto c_eq = make_triple_candidate(i, j, k, eq);
                        if (c_eq.n_rows == 3) candidates.push_back(c_eq);

                        for (int dom = 0; dom < 3; ++dom) {
                            std::vector<int> dw = {25, 25, 25};
                            dw[dom] = 50;
                            auto c_dom = make_triple_candidate(i, j, k, dw);
                            if (c_dom.n_rows == 3) candidates.push_back(c_dom);
                        }
                    }
                }
            }
        } else {
            // Two-color mode: only show pair candidates at 50:50
            for (int i = 0; i < n; ++i) {
                for (int j = i + 1; j < n; ++j) {
                    auto recipe = build_pair_color_match_candidate(palette, i + 1, j + 1, 50);
                    if (!recipe.valid) continue;
                    Candidate c;
                    c.color   = recipe.preview_color;
                    c.tooltip = wxString::Format("F%d(50%%) + F%d(50%%)", i+1, j+1);
                    c.rows[0] = i; c.rows[1] = j;
                    c.n_rows  = 2;
                    c.b_pct   = 50;
                    candidates.push_back(c);
                }
            }
        }
    } else if (m_current_mode == MODE_MATCH) {
        const auto presets = build_color_match_presets(m_filament_colours, m_match_min_pct);
        for (const auto& preset : presets) {
            if (!preset.valid) continue;
            Candidate c;
            c.color   = preset.preview_color;
            c.tooltip = from_u8(summarize_color_match_recipe(preset));
            auto decoded = MixedFilamentManager::decode_gradient_component_ids(preset.gradient_component_ids);
            if (decoded.size() >= 2) {
                c.rows[0] = (int)decoded[0] - 1; c.rows[1] = (int)decoded[1] - 1;
                c.n_rows = 2; c.b_pct = preset.mix_b_percent;
                if (decoded.size() >= 3) {
                    c.rows[2] = (int)decoded[2] - 1; c.n_rows = 3;
                    auto w = decode_color_match_gradient_weights(preset.gradient_component_weights, 3);
                    if (w.size() >= 3) { c.wx = w[0]/100.0; c.wy = w[1]/100.0; c.wz = w[2]/100.0; }
                }
            } else {
                c.rows[0] = (int)preset.component_a - 1; c.rows[1] = (int)preset.component_b - 1;
                c.n_rows = 2; c.b_pct = preset.mix_b_percent;
            }
            candidates.push_back(c);
        }
    } else if (m_current_mode == MODE_GRADIENT) {
        // Gradient mode: A→B and B→A direction pairs per filament pair.
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                // 50:50 blend is symmetric — compute once, reuse for both directions.
                const wxColour blended = parse_mixed_color(
                    MixedFilamentManager::blend_color(m_filament_colours[i], m_filament_colours[j], 50, 50));

                // Direction A→B: Fi → Fj
                Candidate c_ab;
                c_ab.color   = blended;
                c_ab.tooltip = wxString::Format(L"F%d → F%d", i + 1, j + 1);
                c_ab.rows[0] = i; c_ab.rows[1] = j;
                c_ab.n_rows  = 2;
                c_ab.b_pct   = 50;
                candidates.push_back(c_ab);

                // Direction B→A: Fj → Fi
                Candidate c_ba;
                c_ba.color   = blended;
                c_ba.tooltip = wxString::Format(L"F%d → F%d", j + 1, i + 1);
                c_ba.rows[0] = j; c_ba.rows[1] = i;
                c_ba.n_rows  = 2;
                c_ba.b_pct   = 50;
                candidates.push_back(c_ba);
            }
        }
    } else {
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                std::string blended = MixedFilamentManager::blend_color(
                    m_filament_colours[i], m_filament_colours[j], 50, 50);
                Candidate c;
                c.color   = parse_mixed_color(blended);
                c.tooltip = wxString::Format("F%d + F%d", i+1, j+1);
                c.rows[0] = i; c.rows[1] = j;
                c.n_rows  = 2;
                c.b_pct   = 50;
                candidates.push_back(c);
            }
        }
    }

    if (candidates.empty()) return;
    m_swatch_min_weights.clear();
    auto* grid = new wxGridSizer(10, FromDIP(6), FromDIP(6));

    // Build display context for MixedFilamentBadge
    MixedFilamentDisplayContext ctx;
    ctx.num_physical = m_filament_colours.size();
    ctx.physical_colors = m_filament_colours;

    int badge_idx = 0;
    for (const auto& cand : candidates) {
        {
            std::vector<unsigned int> fids;
            for (int r = 0; r < cand.n_rows; ++r)
                fids.push_back(static_cast<unsigned int>(cand.rows[r]));
            if (!is_filament_compatible(fids))
                continue;
        }

        // Build a MixedFilament for the badge's display_color
        MixedFilament mf;
        mf.component_a = (unsigned int)(cand.rows[0] + 1);
        mf.component_b = (unsigned int)(cand.rows[1] + 1);
        mf.custom = true;
        mf.gradient_enabled = (m_current_mode == MODE_GRADIENT);
        mf.display_color = wxString::Format("#%02X%02X%02X",
            cand.color.Red(), cand.color.Green(), cand.color.Blue()).ToStdString();

        auto* badge = new MixedFilamentBadge(m_swatch_grid_panel, wxID_ANY, ++badge_idx, mf, ctx, false, 24);
        badge->SetToolTip(cand.tooltip);

        badge->Bind(wxEVT_BUTTON, [this, cand](wxCommandEvent&) {
            if (m_current_mode == MODE_MATCH) {
                // Directly update tri picker with preset config (no target update, no recipe search)
                if (cand.n_rows >= 3) {
                    m_match_tri_indices = {cand.rows[0], cand.rows[1], cand.rows[2]};
                    m_match_tri_wx = cand.wx; m_match_tri_wy = cand.wy; m_match_tri_wz = cand.wz;
                    m_match_tri_weights = {cand.wx, cand.wy, cand.wz};
                } else if (cand.n_rows == 2) {
                    m_match_tri_indices = {cand.rows[0], cand.rows[1]};
                    m_match_tri_weights = {(double)(100 - cand.b_pct) / 100.0, (double)cand.b_pct / 100.0};
                    m_match_tri_wx = m_match_tri_weights[0]; m_match_tri_wy = m_match_tri_weights[1];
                    if (m_match_gradient_selector) {
                        m_match_gradient_selector->set_value(cand.b_pct);
                        int ia = std::clamp(cand.rows[0], 0, (int)m_filament_colours.size() - 1);
                        int ib = std::clamp(cand.rows[1], 0, (int)m_filament_colours.size() - 1);
                        m_match_gradient_selector->set_colors(
                            parse_mixed_color(m_filament_colours[ia]),
                            parse_mixed_color(m_filament_colours[ib]));
                    }
                }
                if (m_match_gradient_selector && cand.n_rows >= 3 && m_match_tri_indices.size() >= 2) {
                    int ia = std::clamp(m_match_tri_indices[0], 0, (int)m_filament_colours.size() - 1);
                    int ib = std::clamp(m_match_tri_indices[1], 0, (int)m_filament_colours.size() - 1);
                    m_match_gradient_selector->set_colors(
                        parse_mixed_color(m_filament_colours[ia]),
                        parse_mixed_color(m_filament_colours[ib]));
                }
                rebuild_match_legend();
                update_ratio_or_tri_visibility();  // switch gradient/tri based on actual filament count
                if (m_match_tri_picker)  m_match_tri_picker->Refresh();
                if (m_match_strip_panel) m_match_strip_panel->Refresh();
                if (m_match_blend_panel) m_match_blend_panel->Refresh();
                update_compatibility_warning();
                return;
            }
            std::vector<int> selections;
            for (int r = 0; r < cand.n_rows; ++r)
                selections.push_back(cand.rows[r]);

            if (selections.size() >= 2) {
                m_result.component_a = (unsigned int)(selections[0] + 1);
                m_result.component_b = (unsigned int)(selections[1] + 1);
            }

            // Rebuild all combos with the candidate's selections
            for (size_t i = 0; i < m_filament_rows.size(); ++i) {
                std::set<int> used_by_others;
                for (size_t k = 0; k < m_filament_rows.size(); ++k)
                    if (k != i && k < selections.size())
                        used_by_others.insert(selections[k]);
                int sel = (i < selections.size()) ? selections[i] : 0;
                populate_combo(m_filament_rows[i].combo, used_by_others, sel,
                               m_filament_rows[i].filament_indices);
                set_combo_combined_icon(m_filament_rows[i].combo, sel);
            }

            if (cand.n_rows == 2 && m_current_mode == MODE_RATIO && m_gradient_selector) {
                m_gradient_selector->set_value(cand.b_pct);
                rebuild_legend();
            } else if (cand.n_rows == 2 && m_current_mode == MODE_GRADIENT) {
                // Direction is encoded in rows[] ordering; reset to 0 so
                // the gradient draws component_a (rows[0]) → component_b (rows[1]).
                m_gradient_direction = 0;
            } else if (cand.n_rows == 3) {
                m_tri_wx = cand.wx;
                m_tri_wy = cand.wy;
                m_tri_wz = cand.wz;
                rebuild_legend();
            }
            update_preview();
            update_compatibility_warning();
        });

        // Single-filament badges are always visible regardless of the min-mix slider.
        int min_w = 100;
        if (cand.n_rows == 2)
            min_w = std::min(100 - cand.b_pct, cand.b_pct);
        else if (cand.n_rows == 3)
            min_w = static_cast<int>(std::min({cand.wx, cand.wy, cand.wz}) * 100.0 + 0.5);
        // n_rows == 1 or unknown: badge is always visible (sentinel 100).
        m_swatch_min_weights.push_back(min_w);

        grid->Add(badge, 0, wxALIGN_CENTER);
    }

    m_swatch_grid_panel->SetSizer(grid);
    m_swatch_grid_panel->Layout();
}

/**
 * Rebuild the swatch grid sizer in match mode without recreating widgets.
 * Hides badges whose minimum component weight falls below m_match_min_pct
 * and reflows the remaining visible badges into a new grid sizer.
 */
void MixedFilamentDialog::rebuild_swatch_sizer()
{
    if (m_current_mode != MODE_MATCH) return;
    auto& children = m_swatch_grid_panel->GetChildren();

    // Guard: the grid panel children must correspond 1:1 with swatch badges.
    if (children.size() != m_swatch_min_weights.size()) return;

    const int cols = 10;
    auto* new_grid = new wxGridSizer(cols, FromDIP(6), FromDIP(6));
    for (size_t i = 0; i < children.size() && i < m_swatch_min_weights.size(); ++i) {
        bool visible = m_swatch_min_weights[i] >= m_match_min_pct;
        children[i]->Show(visible);
        if (visible)
            new_grid->Add(children[i], 0, wxALIGN_CENTER);
    }

    // SetSizer replaces and auto-deletes the old sizer.
    // Windows survive because Add() defaults to no ownership.
    m_swatch_grid_panel->SetSizer(new_grid);
    m_swatch_grid_panel->Layout();
    m_swatch_card->Layout();
    m_swatch_card->Refresh();
    m_scrolled_content->FitInside();
    m_scrolled_content->Refresh();
}

void MixedFilamentDialog::on_mode_changed(int mode_index)
{
    sync_rows_to_result();
    if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        const std::string raw = into_u8(m_pattern_ctrl->GetValue());
        m_result.manual_pattern = MixedFilamentManager::normalize_manual_pattern(raw);
    }
    m_current_mode = mode_index;
    int max_f = max_filaments_for_mode(mode_index);
    if ((int)m_filament_rows.size() > max_f)
        resize_gradient_ids(max_f);
    if (mode_index == MODE_MATCH) {
        // Init match filament data with default 2:1:1 ratio
        int num_physical = (int)m_filament_colours.size();
        m_match_tri_indices.clear();
        m_match_tri_weights.clear();
        // Restore saved data if editing, otherwise use default 2:1:1
        auto saved_ids = MixedFilamentManager::decode_gradient_component_ids(m_result.gradient_component_ids);
        if (saved_ids.size() >= 3) {
            // Restore from saved match data
            for (unsigned int id : saved_ids) {
                int idx = int(id - 1);
                if (idx >= 0 && idx < num_physical)
                    m_match_tri_indices.push_back(idx);
            }
            if (m_match_tri_indices.size() >= 3) {
                auto w = decode_color_match_gradient_weights(m_result.gradient_component_weights, (int)m_match_tri_indices.size());
                double total = 0;
                for (int v : w) total += v;
                if (total > 0) {
                    m_match_tri_wx = w[0] / total; m_match_tri_wy = w[1] / total; m_match_tri_wz = w[2] / total;
                }
                m_match_tri_weights = {m_match_tri_wx, m_match_tri_wy, m_match_tri_wz};
            }
        } else if (saved_ids.size() == 2) {
            // 2-color saved match
            for (unsigned int id : saved_ids) {
                int idx = int(id - 1);
                if (idx >= 0 && idx < num_physical)
                    m_match_tri_indices.push_back(idx);
            }
            m_match_tri_weights = {(double)(100 - m_result.mix_b_percent) / 100.0, (double)m_result.mix_b_percent / 100.0};
            m_match_tri_wx = m_match_tri_weights[0]; m_match_tri_wy = m_match_tri_weights[1];
        } else if (m_result.custom && m_result.component_a > 0 && m_result.component_b > 0 && num_physical >= 2) {
            // Edit dialog: 2-color match restored from component_a/b (no gradient_component_ids stored)
            m_match_tri_indices = {(int)m_result.component_a - 1, (int)m_result.component_b - 1};
            m_match_tri_weights = {(double)(100 - m_result.mix_b_percent) / 100.0, (double)m_result.mix_b_percent / 100.0};
            m_match_tri_wx = m_match_tri_weights[0]; m_match_tri_wy = m_match_tri_weights[1];
        } else if (num_physical >= 3) {
            m_match_tri_indices = {0, 1, 2};
            m_match_tri_weights = {2.0/4.0, 1.0/4.0, 1.0/4.0};
            m_match_tri_wx = 2.0/4.0; m_match_tri_wy = 1.0/4.0; m_match_tri_wz = 1.0/4.0;
        } else if (num_physical == 2) {
            m_match_tri_indices = {0, 1};
            m_match_tri_weights = {0.5, 0.5};
        }
        // Set gradient_selector colors
        if (m_match_gradient_selector && m_match_tri_indices.size() >= 2) {
            int ia = std::clamp(m_match_tri_indices[0], 0, num_physical - 1);
            int ib = std::clamp(m_match_tri_indices[1], 0, num_physical - 1);
            m_match_gradient_selector->set_colors(
                parse_mixed_color(m_filament_colours[ia]),
                parse_mixed_color(m_filament_colours[ib]));
            if (m_match_tri_indices.size() == 2)
                m_match_gradient_selector->set_value((int)(m_match_tri_weights[1] * 100 + 0.5));
        }
        // Default target color using K-M blend (same method as recipe search)
        wxColour default_target("#26A69A");
        std::vector<wxColour> palette;
        palette.reserve(num_physical);
        for (const auto& s : m_filament_colours)
            palette.push_back(parse_mixed_color(s));
        if (num_physical >= 3) {
            auto recipe = build_multi_color_match_candidate(palette, {1u, 2u, 3u}, {50, 25, 25}, m_match_min_pct);
            if (recipe.valid) default_target = recipe.preview_color;
        } else if (num_physical == 2) {
            auto recipe = build_pair_color_match_candidate(palette, 1u, 2u, 50, m_match_min_pct);
            if (recipe.valid) default_target = recipe.preview_color;
        }
        if (m_match_target_picker) m_match_target_picker->SetBackgroundColour(default_target);
        if (m_match_hex_input) m_match_hex_input->ChangeValue(default_target.GetAsString(wxC2S_HTML_SYNTAX).Mid(1));
        rebuild_match_legend();
    } else if (mode_index == MODE_CYCLE && m_pattern_ctrl) {
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(m_result.manual_pattern);
        m_pattern_ctrl->SetValue(from_u8(norm.empty() ? "12" : norm));
        rebuild_cycle_legend();
    }
    rebuild_filament_rows();
    update_ratio_or_tri_visibility();
    update_preview();
    // Clear any stale banners before re-evaluating for the new mode
    if (m_error_panel && m_warning_panel && m_btn_confirm) {
        m_error_panel->Hide();
        m_warning_panel->Hide();
        if (mode_index == MODE_CYCLE)
            validate_cycle_pattern();
        else
            update_compatibility_warning();
    }
    Layout();
    if (IsShown()) Fit();
}

void MixedFilamentDialog::update_gradient_selector_colors()
{
    if (!m_gradient_selector || m_filament_rows.size() < 2) return;
    int ia = get_filament_index(0);
    int ib = get_filament_index(1);
    ia = std::max(0, std::min(ia, (int)m_filament_colours.size()-1));
    ib = std::max(0, std::min(ib, (int)m_filament_colours.size()-1));
    wxColour ca = parse_mixed_color(m_filament_colours[ia]);
    wxColour cb = parse_mixed_color(m_filament_colours[ib]);
    m_gradient_selector->set_colors(ca, cb);

    update_legend_text();
}

void MixedFilamentDialog::rebuild_cycle_legend()
{
    if (!m_cycle_legend_sizer || !m_pattern_ctrl) return;

    m_cycle_legend_sizer->Clear(true);
    m_cycle_legend_labels.clear();

    const std::string raw = into_u8(m_pattern_ctrl->GetValue());
    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(raw);

    if (!normalized.empty()) {
        const int num_physical = (int)m_filament_colours.size();
        if (num_physical >= 2) {
            // Decode pattern to filament IDs (same logic as draw_strip / collect_result)
            MixedFilament dummy_mf;
            dummy_mf.component_a = 1;
            dummy_mf.component_b = 2;
            std::vector<unsigned int> sequence;
            const std::vector<std::string> group_strs = MixedFilamentManager::split_pattern_groups(normalized);
            for (const std::string& group : group_strs) {
                const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
                for (const auto& token : tokens) {
                    unsigned int eid = MixedFilamentManager::physical_filament_from_token(token, dummy_mf, num_physical);
                    if (eid >= 1 && eid <= (unsigned)num_physical) sequence.push_back(eid);
                }
            }

            if (!sequence.empty()) {
                // Count occurrences
                std::map<unsigned int, int> counts;
                for (unsigned int eid : sequence) counts[eid]++;

                const int total = (int)sequence.size();

                // Compute floor percentages first, then distribute remainder via largest remainders
                // to guarantee sum=100. This matches summarize_cycle_pattern_text in Plater.
                std::vector<std::pair<unsigned int, int>> sorted_cnts(counts.begin(), counts.end());
                std::sort(sorted_cnts.begin(), sorted_cnts.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });

                std::vector<int> pcts(sorted_cnts.size());
                int sum_pct = 0;
                for (size_t i = 0; i < sorted_cnts.size(); ++i) {
                    pcts[i] = int((static_cast<long long>(sorted_cnts[i].second) * 100) / total);
                    sum_pct += pcts[i];
                }

                if (sum_pct < 100) {
                    std::vector<std::pair<size_t, int>> rem;
                    rem.reserve(sorted_cnts.size());
                    for (size_t i = 0; i < sorted_cnts.size(); ++i)
                        rem.emplace_back(i, int((static_cast<long long>(sorted_cnts[i].second) * 100) % total));
                    std::sort(rem.begin(), rem.end(), [](const auto& a, const auto& b) {
                        if (a.second != b.second) return a.second > b.second;
                        return a.first < b.first;
                    });
                    for (int extra = 100 - sum_pct; extra > 0; --extra) {
                        pcts[rem.front().first]++;
                        rem.erase(rem.begin());
                    }
                }

                for (size_t i = 0; i < sorted_cnts.size(); ++i) {
                    unsigned int eid = sorted_cnts[i].first;
                    int pct         = pcts[i];
                    int idx = (int)eid - 1;
                    if (idx < 0 || idx >= num_physical) continue;

                        // Badge + label pair
                        auto* pair = new wxBoxSizer(wxHORIZONTAL);

                        MixedFilamentDisplayContext ctx;
                        ctx.num_physical = m_filament_colours.size();
                        ctx.physical_colors = m_filament_colours;
                        MixedFilament mf;
                        mf.display_color = m_filament_colours[idx];
                        mf.custom = true;
                        auto* badge = new MixedFilamentBadge(m_cycle_legend_panel, wxID_ANY, eid, mf, ctx, true, 12);
                        pair->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

                        auto* lbl = new wxStaticText(m_cycle_legend_panel, wxID_ANY, wxString::Format("%d%%", pct));
                        lbl->SetFont(Label::Body_12);
                        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
                        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
                        pair->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
                        m_cycle_legend_labels.push_back(lbl);

                        m_cycle_legend_sizer->Add(pair, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
                }
            }
        }
    }

    m_cycle_legend_panel->Layout();
    if (m_cycle_card) {
        m_cycle_card->Layout();
        m_cycle_card->Refresh();
    }
    if (m_scrolled_content) {
        m_scrolled_content->Layout();
        m_scrolled_content->FitInside();
        m_scrolled_content->Refresh();
    }
}

void MixedFilamentDialog::validate_cycle_pattern()
{
    if (!m_pattern_ctrl) return;

    const std::string raw = into_u8(m_pattern_ctrl->GetValue());

    bool has_invalid_chars = false;
    std::string filtered;
    filtered.reserve(raw.size());
    for (char c : raw) {
        if (c == ',' || c == '[' || c == ']' ||
            (c >= '0' && c <= '9')) {
            filtered.push_back(c);
        } else {
            has_invalid_chars = true;
        }
    }

    const bool has_leading_trailing_comma = !raw.empty() && (raw.front() == ',' || raw.back() == ',');

    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(filtered);
    const bool is_malformed = !filtered.empty() && normalized.empty();

    if (has_leading_trailing_comma) {
        set_error(_L("Leading or trailing commas are not allowed."));
        return;
    }

    if (has_invalid_chars) {
        set_error(_L("Invalid characters found. Only digits, square brackets ([ and ]), and commas (,) are allowed."));
        return;
    }

    if (is_malformed) {
        set_error(_L("Unrecognized pattern format. Please check the pattern syntax."));
        return;
    }

    // Validate that all directly-referenced physical filament IDs exist.
    // H4: only validate when there are at least 2 physical filaments and a non-empty pattern.
    const int num_physical = (int)m_filament_colours.size();
    if (num_physical >= 2 && !normalized.empty()) {
        auto parsed = parse_cycle_pattern(normalized, num_physical);
        if (parsed.invalid_id != 0) {
            set_error(
                wxString::Format(_L("Filament %d not recognized. Please re-enter."), (int)parsed.invalid_id));
            return;
        }
        if (!parsed.invalid_token.empty()) {
            set_error(_L("Unrecognized pattern format. Please check the pattern syntax."));
            return;
        }
    }

    update_compatibility_warning();

    const std::string final_text = normalized.empty() ? "12" : normalized;

    if (m_cycle_card) m_cycle_card->Freeze();

    if (final_text != raw) {
        m_pattern_ctrl->ChangeValue(from_u8(final_text));
    }

    if (m_cycle_strip_panel)  m_cycle_strip_panel->Refresh();
    if (m_cycle_blend_panel)  m_cycle_blend_panel->Refresh();
    rebuild_cycle_legend();

    if (m_cycle_card) {
        m_cycle_card->Thaw();
        m_cycle_card->Refresh();
    }
}

void MixedFilamentDialog::update_preview()
{
    if ((int)m_filament_rows.size() == 2)
        update_gradient_selector_colors();
    update_legend_text();
    if (m_preview_panel)       m_preview_panel->Refresh();
    if (m_preview_blend_panel) m_preview_blend_panel->Refresh();
    if (m_strip_panel)         m_strip_panel->Refresh();
    if (m_tri_picker)          m_tri_picker->Refresh();
    if (m_cycle_strip_panel)   m_cycle_strip_panel->Refresh();
    if (m_cycle_blend_panel)   m_cycle_blend_panel->Refresh();
    // Match mode widgets
    if (m_match_tri_picker)    m_match_tri_picker->Refresh();
    if (m_match_strip_panel)   m_match_strip_panel->Refresh();
    if (m_match_blend_panel)   m_match_blend_panel->Refresh();
}

void MixedFilamentDialog::collect_result()
{
    sync_rows_to_result();
    m_result.ui_mode = m_current_mode;
    int val = m_gradient_selector ? m_gradient_selector->value() : 50;
    m_result.mix_b_percent = val;
    // Default: drop Z-gradient state. Only MODE_GRADIENT re-enables it below.
    m_result.gradient_enabled = false;
    switch (m_current_mode) {
    case MODE_RATIO:
        m_result.gradient_component_weights.clear();
        if ((int)m_filament_rows.size() == 3) {
            m_result.distribution_mode = int(MixedFilament::LayerCycle);
            m_result.manual_pattern.clear();
            // Store all 3 filament ids (1-based) so gradient_component_ids has 3 entries,
            // which lets resolve() enter the weighted gradient branch.
            {
                std::vector<unsigned int> all_ids;
                for (int i = 0; i < 3; ++i) {
                    int s = get_filament_index(i);
                    if (s >= 0)
                        all_ids.push_back((unsigned int)(s + 1));
                }
                m_result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(all_ids);
            }
            int r0 = (int)(m_tri_wx * 100 + 0.5);
            int r1 = (int)(m_tri_wy * 100 + 0.5);
            int r2 = 100 - r0 - r1;
            m_result.gradient_component_weights =
                std::to_string(r0) + "/" + std::to_string(r1) + "/" + std::to_string(r2);
        } else {
            m_result.distribution_mode = int(MixedFilament::Simple);
            m_result.gradient_component_ids.clear();
            m_result.manual_pattern.clear();
            const int pct_b = std::clamp(val, 0, 100);
            int ratio_a = 1, ratio_b = 0;
            if (pct_b >= 100) {
                ratio_a = 0; ratio_b = 1;
            } else if (pct_b > 0) {
                const int pct_a      = 100 - pct_b;
                const bool b_is_major = pct_b >= pct_a;
                const int major_pct   = b_is_major ? pct_b : pct_a;
                const int minor_pct   = b_is_major ? pct_a : pct_b;
                const int g = std::gcd(major_pct, minor_pct);
                ratio_a = b_is_major ? (minor_pct / g) : (major_pct / g);
                ratio_b = b_is_major ? (major_pct / g) : (minor_pct / g);
            }
            m_result.ratio_a = std::max(0, ratio_a);
            m_result.ratio_b = std::max(0, ratio_b);
        }
        break;
    case MODE_CYCLE: {
        const std::string raw = m_pattern_ctrl ? into_u8(m_pattern_ctrl->GetValue()) : std::string();
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(raw);
        m_result.distribution_mode = int(MixedFilament::Simple);
        m_result.manual_pattern = norm.empty() ? "12" : norm;
        m_result.gradient_component_ids.clear();
        m_result.gradient_component_weights.clear();
        // In cycle mode, tokens "1"/"2" map directly to physical filaments 1/2.
        m_result.component_a = 1;
        m_result.component_b = 2;
        break;
    }
    case MODE_MATCH: {
        // Always save current tri/gradient state — reflects what user actually set
        m_result.distribution_mode = int(MixedFilament::Simple);
        m_result.manual_pattern.clear();
        m_result.ratio_a = 1;
        m_result.ratio_b = 1;
        if (!m_match_tri_indices.empty()) {
            m_result.component_a = (unsigned)(m_match_tri_indices[0] + 1);
            m_result.component_b = (unsigned)(m_match_tri_indices.size() >= 2 ? m_match_tri_indices[1] + 1 : 1);
            if (m_match_tri_indices.size() >= 3) {
                int w0 = (int)(m_match_tri_wx * 100 + 0.5);
                int w1 = (int)(m_match_tri_wy * 100 + 0.5);
                int w2 = std::max(0, 100 - w0 - w1);
                m_result.gradient_component_ids.clear();
                std::string weights_str;
                std::vector<unsigned int> match_ids;
                // Only include filaments with actual weight > 0
                if (w0 > 0) { match_ids.push_back((unsigned int)(m_match_tri_indices[0] + 1)); weights_str += std::to_string(w0); }
                if (w1 > 0) { match_ids.push_back((unsigned int)(m_match_tri_indices[1] + 1)); if (!weights_str.empty()) weights_str += "/"; weights_str += std::to_string(w1); }
                if (w2 > 0) { match_ids.push_back((unsigned int)(m_match_tri_indices[2] + 1)); if (!weights_str.empty()) weights_str += "/"; weights_str += std::to_string(w2); }
                m_result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(match_ids);
                m_result.gradient_component_weights = weights_str;
                if (match_ids.size() >= 3) {
                    m_result.distribution_mode = int(MixedFilament::LayerCycle);
                    m_result.mix_b_percent = 50;
                } else if (match_ids.size() == 2) {
                    m_result.distribution_mode = int(MixedFilament::Simple);
                    int total_w = 0;
                    if (w0 > 0) total_w += w0;
                    if (w1 > 0) total_w += w1;
                    if (w2 > 0) total_w += w2;
                    m_result.mix_b_percent = (total_w > 0) ? (100 * (w1 > 0 ? w1 : w2) / total_w) : 50;
                }
            } else {
                m_result.mix_b_percent = m_match_gradient_selector ? m_match_gradient_selector->value() : 50;
                m_result.gradient_component_ids.clear();
                // Store for constructor detection (Plater skips display for Simple mode)
                m_result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids({m_result.component_a, m_result.component_b});
                m_result.gradient_component_weights.clear();
            }
        } else {
            m_result.component_a = 1;
            m_result.component_b = 2;
            m_result.mix_b_percent = 50;
            m_result.gradient_component_ids.clear();
            m_result.gradient_component_weights.clear();
        }
        // Compute display_color from all filaments (not just component_a/b)
        {
            int nf = (int)m_match_tri_indices.size();
            if (nf == 2 && m_match_gradient_selector) {
                int ia = std::clamp(m_match_tri_indices[0], 0, (int)m_filament_colours.size() - 1);
                int ib = std::clamp(m_match_tri_indices[1], 0, (int)m_filament_colours.size() - 1);
                int val = m_match_gradient_selector->value();
                m_result.display_color = MixedFilamentManager::blend_color(m_filament_colours[ia], m_filament_colours[ib], 100 - val, val);
            } else if (nf >= 3) {
                wxColour c0 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[0], 0, (int)m_filament_colours.size() - 1)]);
                wxColour c1 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[1], 0, (int)m_filament_colours.size() - 1)]);
                wxColour c2 = parse_mixed_color(m_filament_colours[std::clamp(m_match_tri_indices[2], 0, (int)m_filament_colours.size() - 1)]);
                int r = (int)(c0.Red()*m_match_tri_wx + c1.Red()*m_match_tri_wy + c2.Red()*m_match_tri_wz + 0.5);
                int g = (int)(c0.Green()*m_match_tri_wx + c1.Green()*m_match_tri_wy + c2.Green()*m_match_tri_wz + 0.5);
                int b = (int)(c0.Blue()*m_match_tri_wx + c1.Blue()*m_match_tri_wy + c2.Blue()*m_match_tri_wz + 0.5);
                m_result.display_color = wxString::Format("#%02X%02X%02X", std::clamp(r,0,255), std::clamp(g,0,255), std::clamp(b,0,255)).ToStdString();
            }
        }
        break;
    }
    case MODE_GRADIENT:
        m_result.distribution_mode = int(MixedFilament::LayerCycle);
        // 2-filament Z gradient does not need extra component IDs/weights.
        m_result.gradient_component_ids.clear();
        m_result.gradient_component_weights.clear();
        m_result.manual_pattern.clear();
        m_result.gradient_enabled = true;
        
        // Gradient direction mapping:
        // Direction 0: A→B (component_a starts dominant at 80%, ends at 20%)
        //              gradient_start=0.8, gradient_end=0.2 (start > end)
        // Direction 1: B→A (component_a starts at 20%, ends dominant at 80%)
        //              gradient_start=0.2, gradient_end=0.8 (start < end)
        // 
        // The gradient_start/end values represent the ratio of component_a.
        // Component_b ratio is always (1 - component_a_ratio).
        
        if (m_gradient_direction == 0) {
            // A→B: component_a goes from dominant to minority
            m_result.gradient_start = MixedFilament::k_default_gradient_dominant;
            m_result.gradient_end   = MixedFilament::k_default_gradient_minority;
        } else {
            // B→A: component_a goes from minority to dominant
            m_result.gradient_start = MixedFilament::k_default_gradient_minority;
            m_result.gradient_end   = MixedFilament::k_default_gradient_dominant;
        }
        
        // Mid-blend layer ratio (50%) keeps non-gradient consumers consistent.
        m_result.mix_b_percent = 50;
        m_result.ratio_a = 1;
        m_result.ratio_b = 1;
        // Force at least 2 sublayers per layer so the LocalZ planner emits the
        // two sub-layers needed to realize the per-layer gradient ratio.
        if (m_result.local_z_max_sublayers < 2)
            m_result.local_z_max_sublayers = 2;
        break;
    }
    m_result.custom = true;
}

void MixedFilamentDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    if (m_preview_panel)
        m_preview_panel->SetMinSize(wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
    if (m_strip_panel)
        m_strip_panel->SetMinSize(wxSize(-1, FromDIP(STRIP_HEIGHT)));
    Layout(); 
    Fit();
}

}} // namespace Slic3r::GUI

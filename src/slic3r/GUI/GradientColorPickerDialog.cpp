#include "GradientColorPickerDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/Color.hpp"

#include <wx/dcbuffer.h>
#include <wx/colordlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/textctrl.h>
#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// GradientBarWidget
// ---------------------------------------------------------------------------

BEGIN_EVENT_TABLE(GradientBarWidget, wxWindow)
    EVT_PAINT        (GradientBarWidget::on_paint)
    EVT_LEFT_DOWN    (GradientBarWidget::on_left_down)
    EVT_LEFT_UP      (GradientBarWidget::on_left_up)
    EVT_MOTION       (GradientBarWidget::on_motion)
    EVT_LEFT_DCLICK  (GradientBarWidget::on_double_click)
    EVT_RIGHT_DOWN   (GradientBarWidget::on_right_down)
END_EVENT_TABLE()

GradientBarWidget::GradientBarWidget(wxWindow* parent, const FilamentGradient& gradient)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, TOTAL_H))
    , m_stops(gradient.stops)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(200, TOTAL_H));

    // ensure we always have at least two stops
    if (m_stops.size() < 2) {
        m_stops.clear();
        m_stops.push_back({ 0.0f, ColorRGBA(1.f, 0.f, 0.f, 1.f) });
        m_stops.push_back({ 1.0f, ColorRGBA(0.f, 0.f, 1.f, 1.f) });
    }
    std::sort(m_stops.begin(), m_stops.end());
    m_selected = 0; // start with a selection so the hex field has a value
}

bool GradientBarWidget::get_selected_color(ColorRGBA& out) const
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_stops.size()))
        return false;
    out = m_stops[m_selected].color;
    return true;
}

void GradientBarWidget::set_selected_color(const ColorRGBA& c)
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_stops.size()))
        return;
    m_stops[m_selected].color = c;
    m_bmp_dirty = true;
    Refresh();
    if (on_selection_changed) on_selection_changed();
}

void GradientBarWidget::add_stop()
{
    // Insert at the midpoint of the largest gap between consecutive stops.
    std::sort(m_stops.begin(), m_stops.end());
    float best_pos = 0.5f, best_gap = -1.f;
    for (size_t i = 1; i < m_stops.size(); ++i) {
        const float gap = m_stops[i].position - m_stops[i - 1].position;
        if (gap > best_gap) { best_gap = gap; best_pos = 0.5f * (m_stops[i].position + m_stops[i - 1].position); }
    }
    FilamentGradient tmp; tmp.stops = m_stops;
    m_stops.push_back({ best_pos, tmp.sample(best_pos * 500.f) });
    std::sort(m_stops.begin(), m_stops.end());
    m_selected = static_cast<int>(std::find_if(m_stops.begin(), m_stops.end(),
        [best_pos](const GradientColorStop& s){ return std::abs(s.position - best_pos) < 0.0005f; }) - m_stops.begin());
    m_bmp_dirty = true;
    Refresh();
    if (on_selection_changed) on_selection_changed();
}

void GradientBarWidget::remove_selected_stop()
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_stops.size()) || m_stops.size() <= 2)
        return;
    m_stops.erase(m_stops.begin() + m_selected);
    m_selected = std::min(m_selected, static_cast<int>(m_stops.size()) - 1);
    m_bmp_dirty = true;
    Refresh();
    if (on_selection_changed) on_selection_changed();
}

FilamentGradient GradientBarWidget::get_gradient() const
{
    FilamentGradient g;
    g.stops = m_stops;
    return g;
}

int GradientBarWidget::pos_to_x(float pos) const
{
    const int w = GetClientSize().x - 2 * MARGIN_X;
    return MARGIN_X + static_cast<int>(std::round(pos * w));
}

float GradientBarWidget::x_to_pos(int x) const
{
    const int w = GetClientSize().x - 2 * MARGIN_X;
    if (w <= 0) return 0.f;
    return std::clamp(static_cast<float>(x - MARGIN_X) / w, 0.f, 1.f);
}

int GradientBarWidget::hit_test(wxPoint pt) const
{
    for (int i = 0; i < static_cast<int>(m_stops.size()); ++i) {
        const int cx = pos_to_x(m_stops[i].position);
        if (std::abs(pt.x - cx) <= HANDLE_W && pt.y >= BAR_H)
            return i;
    }
    return -1;
}

void GradientBarWidget::rebuild_gradient_bitmap()
{
    const wxSize sz = GetClientSize();
    if (sz.x <= 0 || sz.y <= 0) return;

    const int bar_w = sz.x - 2 * MARGIN_X;
    m_gradient_bmp = wxBitmap(bar_w, BAR_H);
    wxMemoryDC mdc(m_gradient_bmp);

    for (int px = 0; px < bar_w; ++px) {
        const float pos = static_cast<float>(px) / std::max(bar_w - 1, 1);
        // find bracketing stops
        auto it = std::lower_bound(m_stops.begin(), m_stops.end(), GradientColorStop{ pos, {} });
        ColorRGBA col;
        if (it == m_stops.end()) {
            col = m_stops.back().color;
        } else if (it == m_stops.begin()) {
            col = m_stops.front().color;
        } else {
            const auto& hi = *it;
            const auto& lo = *std::prev(it);
            const float range = hi.position - lo.position;
            const float t = (range > 0.f) ? (pos - lo.position) / range : 0.f;
            col = lo.color * (1.f - t) + hi.color * t;
        }
        mdc.SetPen(wxColour(col.r_uchar(), col.g_uchar(), col.b_uchar()));
        mdc.DrawLine(px, 0, px, BAR_H);
    }
    m_bmp_dirty = false;
}

void GradientBarWidget::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    const wxSize sz = GetClientSize();
    dc.SetBackground(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    dc.Clear();

    if (m_bmp_dirty)
        rebuild_gradient_bitmap();

    // draw gradient bar
    if (m_gradient_bmp.IsOk())
        dc.DrawBitmap(m_gradient_bmp, MARGIN_X, 0);

    // draw bar border
    dc.SetPen(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(MARGIN_X, 0, sz.x - 2 * MARGIN_X, BAR_H);

    // draw handles (downward triangles)
    for (int i = 0; i < static_cast<int>(m_stops.size()); ++i) {
        const int cx = pos_to_x(m_stops[i].position);
        const ColorRGBA& c = m_stops[i].color;
        wxColour fill(c.r_uchar(), c.g_uchar(), c.b_uchar());
        wxColour border = (i == m_selected)
            ? wxColour(0, 120, 215)
            : wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

        wxPoint tri[3] = {
            { cx - HANDLE_W, BAR_H + 2 },
            { cx + HANDLE_W, BAR_H + 2 },
            { cx,            BAR_H + HANDLE_H + 1 }
        };
        dc.SetBrush(wxBrush(fill));
        dc.SetPen(wxPen(border, i == m_selected ? 2 : 1));
        dc.DrawPolygon(3, tri);
    }
}

void GradientBarWidget::on_left_down(wxMouseEvent& e)
{
    const int hit = hit_test(e.GetPosition());
    if (hit >= 0) {
        m_dragging = hit;
        m_selected = hit;
    } else if (e.GetPosition().y < BAR_H) {
        // click in the bar with no stop hit: add a new stop
        const float pos = x_to_pos(e.GetPosition().x);
        // sample current gradient color at that position
        FilamentGradient tmp;
        tmp.stops = m_stops;
        const ColorRGBA col = tmp.sample(pos * 500.f); // arbitrary cycle; sample at pos*cycle
        // actually sample at position directly
        GradientColorStop ns{ pos, col };
        m_stops.push_back(ns);
        std::sort(m_stops.begin(), m_stops.end());
        m_selected = static_cast<int>(std::find_if(m_stops.begin(), m_stops.end(),
            [pos](const GradientColorStop& s){ return std::abs(s.position - pos) < 0.001f; }) - m_stops.begin());
        m_bmp_dirty = true;
    } else {
        m_selected = -1;
    }
    CaptureMouse();
    Refresh();
    if (on_selection_changed) on_selection_changed();
}

void GradientBarWidget::on_left_up(wxMouseEvent&)
{
    m_dragging = -1;
    if (HasCapture()) ReleaseMouse();
    Refresh();
}

void GradientBarWidget::on_motion(wxMouseEvent& e)
{
    if (m_dragging < 0 || !e.LeftIsDown()) return;

    float new_pos = x_to_pos(e.GetPosition().x);
    // endpoints must stay at 0 and 1
    if (m_dragging == 0)
        new_pos = 0.f;
    else if (m_dragging == static_cast<int>(m_stops.size()) - 1)
        new_pos = 1.f;
    else
        new_pos = std::clamp(new_pos, 0.001f, 0.999f);

    m_stops[m_dragging].position = new_pos;
    std::sort(m_stops.begin(), m_stops.end());
    // re-find dragged stop after sort
    m_dragging = static_cast<int>(std::min_element(m_stops.begin(), m_stops.end(),
        [new_pos](const GradientColorStop& a, const GradientColorStop& b){
            return std::abs(a.position - new_pos) < std::abs(b.position - new_pos);
        }) - m_stops.begin());
    m_selected = m_dragging;
    m_bmp_dirty = true;
    Refresh();
    if (on_selection_changed) on_selection_changed();
}

void GradientBarWidget::on_double_click(wxMouseEvent& e)
{
    const int hit = hit_test(e.GetPosition());
    if (hit >= 0)
        pick_color_for(hit);
}

void GradientBarWidget::on_right_down(wxMouseEvent& e)
{
    const int hit = hit_test(e.GetPosition());
    if (hit >= 0 && m_stops.size() > 2) {
        m_stops.erase(m_stops.begin() + hit);
        m_selected = -1;
        m_bmp_dirty = true;
        Refresh();
        if (on_selection_changed) on_selection_changed();
    }
}

void GradientBarWidget::pick_color_for(int index)
{
    if (index < 0 || index >= static_cast<int>(m_stops.size())) return;
    // Static so custom colors persist across dialog openings within the session.
    static wxColourData s_colour_data;
    const ColorRGBA& c = m_stops[index].color;
    s_colour_data.SetColour(wxColour(c.r_uchar(), c.g_uchar(), c.b_uchar()));
    s_colour_data.SetChooseFull(true);
    wxColourDialog dlg(this, &s_colour_data);
    if (dlg.ShowModal() == wxID_OK) {
        s_colour_data = dlg.GetColourData(); // save custom colors for next time
        const wxColour chosen = s_colour_data.GetColour();
        m_stops[index].color = ColorRGBA(
            chosen.Red()   / 255.f,
            chosen.Green() / 255.f,
            chosen.Blue()  / 255.f,
            1.f);
        m_bmp_dirty = true;
        Refresh();
        if (on_selection_changed) on_selection_changed();
    }
}

// ---------------------------------------------------------------------------
// GradientColorPickerDialog
// ---------------------------------------------------------------------------

GradientColorPickerDialog::GradientColorPickerDialog(wxWindow* parent, const FilamentGradient& gradient,
                                                     double filament_diameter_mm, double filament_density_g_cm3)
    : DPIDialog(parent, wxID_ANY, _L("Gradient Filament Color Editor"),
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_diameter(filament_diameter_mm)
    , m_density(filament_density_g_cm3)
{
    auto* main = new wxBoxSizer(wxVERTICAL);

    // Instructions
    auto* hint = new wxStaticText(this, wxID_ANY,
        _L("Click a handle to select a stop, then set its color by hex below (or double-click a handle "
           "for the color picker). Add or remove stops with the buttons, or click the bar / right-click a handle."));
    hint->Wrap(440);
    main->Add(hint, 0, wxALL, 8);

    // Gradient bar
    m_bar = new GradientBarWidget(this, gradient);
    main->Add(m_bar, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    // Selected-stop hex entry + add/remove buttons (keyboard-accessible alternatives to the
    // mouse-only bar interactions; hex is how filament makers publish colors).
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(this, wxID_ANY, _L("Color (hex):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_hex = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1), wxTE_PROCESS_ENTER);
    m_hex->SetMaxLength(9); // "#RRGGBBAA"
    m_hex->SetToolTip(_L("Enter a color as #RRGGBB (e.g. the value from the filament maker)."));
    m_hex->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { commit_hex_field(); });
    m_hex->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { commit_hex_field(); e.Skip(); });
    row->Add(m_hex, 0, wxALIGN_CENTER_VERTICAL);

    auto* add_btn = new wxButton(this, wxID_ANY, _L("Add stop"));
    add_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_bar->add_stop(); });
    row->Add(add_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 8);

    auto* remove_btn = new wxButton(this, wxID_ANY, _L("Remove stop"));
    remove_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_bar->remove_selected_stop(); });
    row->Add(remove_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 4);

    main->Add(row, 0, wxEXPAND | wxALL, 8);

    // Keep the hex field synced to the bar's currently-selected stop.
    m_bar->on_selection_changed = [this]() { refresh_hex_field(); };

    // Cycle length and start offset
    auto* grid = new wxFlexGridSizer(2, 2, 6, 8);
    grid->AddGrowableCol(1);

    grid->Add(new wxStaticText(this, wxID_ANY, _L("Cycle length:")),
              0, wxALIGN_CENTER_VERTICAL);
    auto* cycle_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_cycle = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                                   wxDefaultPosition, wxDefaultSize,
                                   wxSP_ARROW_KEYS, 0.1, 1000000.0,
                                   gradient.cycle_length_mm, 1.0);
    m_cycle->SetDigits(1);
    cycle_sizer->Add(m_cycle, 1, wxEXPAND);
    // Unit selector: the canonical stored value is mm; "g" lets the user enter the cycle by
    // weight (what most filament makers publish), converted via diameter + density.
    m_cycle_unit = new wxChoice(this, wxID_ANY);
    m_cycle_unit->Append("mm");
    m_cycle_unit->Append("g");
    m_cycle_unit->SetSelection(0);
    if (m_diameter <= 0.0 || m_density <= 0.0)
        m_cycle_unit->Disable(); // can't convert without valid filament properties
    m_cycle_unit->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        // The field currently holds a value in the PREVIOUS unit; convert to the new one.
        const double v = m_cycle->GetValue();
        if (m_cycle_unit->GetSelection() == 1)
            m_cycle->SetValue(mm_to_grams(v));   // mm -> g
        else
            m_cycle->SetValue(grams_to_mm(v));   // g -> mm
    });
    cycle_sizer->Add(m_cycle_unit, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 6);
    grid->Add(cycle_sizer, 1, wxEXPAND);

    grid->Add(new wxStaticText(this, wxID_ANY, _L("Start offset (mm):")),
              0, wxALIGN_CENTER_VERTICAL);
    m_offset = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                                    wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 0.0, 100000.0,
                                    gradient.start_offset_mm, 1.0);
    grid->Add(m_offset, 1, wxEXPAND);

    main->Add(grid, 0, wxEXPAND | wxALL, 8);

    // OK / Cancel
    main->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
              wxEXPAND | wxALL, 8);

    SetSizer(main);
    Fit();
    SetMinSize(wxSize(480, -1));
    Layout();
    refresh_hex_field(); // populate hex from the initially-selected stop
    CentreOnParent();
}

void GradientColorPickerDialog::refresh_hex_field()
{
    if (m_hex == nullptr || m_bar == nullptr) return;
    ColorRGBA c;
    if (m_bar->get_selected_color(c)) {
        m_hex->ChangeValue(encode_color(c)); // ChangeValue: no EVT_TEXT, avoids re-entrancy
        m_hex->Enable(true);
    } else {
        m_hex->ChangeValue(wxEmptyString);
        m_hex->Enable(false);
    }
}

void GradientColorPickerDialog::commit_hex_field()
{
    if (m_hex == nullptr || m_bar == nullptr || !m_hex->IsEnabled()) return;
    std::string hs = m_hex->GetValue().Trim(true).Trim(false).ToStdString();
    if (hs.empty()) { refresh_hex_field(); return; }
    if (hs[0] != '#') hs = "#" + hs; // accept "RRGGBB" or "#RRGGBB"
    ColorRGBA c;
    if (decode_color(hs, c)) {
        c.a(1.0f); // gradient stops are opaque
        m_bar->set_selected_color(c);
    } else {
        refresh_hex_field(); // invalid input: revert to the current color
    }
}

FilamentGradient GradientColorPickerDialog::get_gradient() const
{
    FilamentGradient g = m_bar->get_gradient();
    const double cycle_val = m_cycle->GetValue();
    g.cycle_length_mm = static_cast<float>(
        (m_cycle_unit->GetSelection() == 1) ? grams_to_mm(cycle_val) : cycle_val);
    g.start_offset_mm  = static_cast<float>(m_offset->GetValue());
    return g;
}

double GradientColorPickerDialog::grams_to_mm(double grams) const
{
    if (m_density <= 0.0 || m_diameter <= 0.0)
        return grams;
    const double PI       = 3.14159265358979323846;
    const double r_mm     = m_diameter / 2.0;
    const double area_cm2 = (PI * r_mm * r_mm) / 100.0; // mm^2 -> cm^2
    const double vol_cm3  = grams / m_density;
    return (vol_cm3 / area_cm2) * 10.0;                 // cm -> mm
}

double GradientColorPickerDialog::mm_to_grams(double mm) const
{
    if (m_density <= 0.0 || m_diameter <= 0.0)
        return mm;
    const double PI       = 3.14159265358979323846;
    const double r_mm     = m_diameter / 2.0;
    const double area_cm2 = (PI * r_mm * r_mm) / 100.0; // mm^2 -> cm^2
    const double vol_cm3  = (mm / 10.0) * area_cm2;
    return vol_cm3 * m_density;
}

}} // namespace Slic3r::GUI

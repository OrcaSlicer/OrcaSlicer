#include "MixedFilamentDialog.hpp"

#include <algorithm>
#include <cmath>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>

#include "libslic3r/Utils.hpp"
#include "I18N.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Tab.hpp"
#include "libslic3r/Preset.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DropDown.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r {
namespace GUI {

static constexpr int MAX_COMPONENTS = 3;

static wxColour blend_colors(const wxColour& a, const wxColour& b, double ratio_a)
{
    double rb = 1.0 - ratio_a;
    return wxColour(
        (unsigned char)(a.Red()   * ratio_a + b.Red()   * rb),
        (unsigned char)(a.Green() * ratio_a + b.Green() * rb),
        (unsigned char)(a.Blue()  * ratio_a + b.Blue()  * rb));
}

static wxColour blend_n_colors(const std::vector<wxColour>& cols, const std::vector<double>& weights)
{
    double r = 0, g = 0, b = 0;
    for (size_t i = 0; i < cols.size() && i < weights.size(); ++i) {
        r += cols[i].Red()   * weights[i];
        g += cols[i].Green() * weights[i];
        b += cols[i].Blue()  * weights[i];
    }
    return wxColour((unsigned char)std::clamp(r, 0.0, 255.0),
                    (unsigned char)std::clamp(g, 0.0, 255.0),
                    (unsigned char)std::clamp(b, 0.0, 255.0));
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
    build_ui();

    wxImage img;
    if (img.LoadFile(from_u8(Slic3r::var("mixed_filament_preview_twocolor.png")), wxBITMAP_TYPE_PNG))
        m_preview_bmp_two = wxBitmap(img);
    if (img.LoadFile(from_u8(Slic3r::var("mixed_filament_preview_threecolor.png")), wxBITMAP_TYPE_PNG))
        m_preview_bmp_three = wxBitmap(img);
}

void MixedFilamentDialog::on_dpi_changed(const wxRect&)
{
    SetSize(FromDIP(439), FromDIP(580));
    Refresh();
}

wxColour MixedFilamentDialog::comp_colour(size_t i) const
{
    unsigned int c = comp(i);
    if (c >= 1 && c <= m_physical_colors.size())
        return wxColour(m_physical_colors[c - 1]);
    return wxColour("#D9D9D9");
}

wxBitmap MixedFilamentDialog::make_swatch_bitmap(size_t idx)
{
    int swatch_sz = FromDIP(16);
    int pad_left  = FromDIP(2);
    int pad_right = FromDIP(6);
    int bmp_w = pad_left + swatch_sz + pad_right;
    int bmp_h = swatch_sz;

    wxBitmap bmp(bmp_w, bmp_h);
    wxMemoryDC dc(bmp);

    wxColour col("#D9D9D9");
    if (idx < m_physical_colors.size())
        col = wxColour(m_physical_colors[idx]);

    dc.SetBrush(*wxWHITE_BRUSH);
    dc.SetPen(*wxWHITE_PEN);
    dc.DrawRectangle(0, 0, bmp_w, bmp_h);

    dc.SetBrush(wxBrush(col));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRoundedRectangle(pad_left, 0, swatch_sz, swatch_sz, FromDIP(2));

    wxString num = wxString::Format(wxT("%zu"), idx + 1);
    dc.SetFont(::Label::Body_13);
    wxSize txt_sz = dc.GetTextExtent(num);
    dc.SetTextForeground(col.GetLuminance() > 0.5 ? wxColour("#262E30") : *wxWHITE);
    dc.DrawText(num, pad_left + (swatch_sz - txt_sz.GetWidth()) / 2,
                     (swatch_sz - txt_sz.GetHeight()) / 2);

    dc.SelectObject(wxNullBitmap);
    return bmp;
}

// ---- UI Construction ----

void MixedFilamentDialog::build_ui()
{
    SetBackgroundColour(*wxWHITE);
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
    main_sizer->Add(create_button_panel(), 0, wxALIGN_RIGHT | wxALL, FromDIP(20));

    SetSizer(main_sizer);

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
        if (n == 0) return;

        const wxBitmap& src = (n >= 3) ? m_preview_bmp_three : m_preview_bmp_two;
        if (src.IsOk()) {
            wxImage scaled = src.ConvertToImage().Scale(sz.GetWidth(), sz.GetHeight(), wxIMAGE_QUALITY_BILINEAR);
            dc.DrawBitmap(wxBitmap(scaled), 0, 0, true);
        } else {
            dc.SetBrush(*wxWHITE_BRUSH);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());
        }

        std::vector<wxColour> cols;
        std::vector<double>   weights;
        for (size_t i = 0; i < n; ++i) {
            cols.push_back(comp_colour(i));
            weights.push_back(ratio(i) / 100.0);
        }

        wxColour mixed = blend_n_colors(cols, weights);
        int swatch_sz = FromDIP(16);
        dc.SetBrush(wxBrush(mixed));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(FromDIP(4), FromDIP(4), swatch_sz, swatch_sz, FromDIP(2));
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

        dc.SetBrush(wxBrush(wxColour("#F8F8F8")));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

        int swatch_sz = FromDIP(16);
        int y_center = (sz.GetHeight() - swatch_sz) / 2;
        int x = FromDIP(13);

        dc.SetFont(::Label::Body_13);

        for (size_t i = 0; i < num_components(); ++i) {
            if (i > 0) {
                dc.SetTextForeground(wxColour("#262E30"));
                dc.DrawText(wxT("+"), x, y_center);
                x += dc.GetTextExtent(wxT("+")).GetWidth() + FromDIP(4);
            }

            wxColour col = comp_colour(i);
            dc.SetBrush(wxBrush(col));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(x, y_center, swatch_sz, swatch_sz, FromDIP(2));

            wxString num = wxString::Format(wxT("%u"), comp(i));
            wxSize num_sz = dc.GetTextExtent(num);
            dc.SetTextForeground(col.GetLuminance() > 0.5 ? wxColour("#262E30") : *wxWHITE);
            dc.DrawText(num, x + (swatch_sz - num_sz.GetWidth()) / 2,
                             y_center + (swatch_sz - num_sz.GetHeight()) / 2);
            x += swatch_sz + FromDIP(4);

            dc.SetTextForeground(wxColour("#262E30"));
            wxString pct = wxString::Format(wxT("%d%%"), ratio(i));
            dc.DrawText(pct, x, y_center);
            x += dc.GetTextExtent(pct).GetWidth() + FromDIP(4);
        }
    });
    sizer->Add(m_summary_panel, 0, wxEXPAND);

    auto* sel_label = new wxStaticText(this, wxID_ANY, _L("Select Mixed Materials"));
    sel_label->SetForegroundColour(wxColour("#909090"));
    sel_label->SetFont(::Label::Body_12);
    sizer->Add(sel_label, 0, wxTOP, FromDIP(6));

    m_material_rows_sizer = new wxBoxSizer(wxVERTICAL);

    m_combo_filaments.clear();
    for (size_t i = 0; i < m_result.components.size(); ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        wxString lbl_text = wxString::Format(_L("Filament %zu"), i + 1);
        auto* lbl = new wxStaticText(this, wxID_ANY, lbl_text);
        lbl->SetFont(::Label::Body_12);
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

        auto* combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxSize(FromDIP(166), FromDIP(24)), 0, nullptr, wxCB_READONLY);
        combo->SetKeepDropArrow(true);
        for (size_t j = 0; j < m_physical_names.size(); ++j)
            combo->Append(wxString::FromUTF8(m_physical_names[j]), make_swatch_bitmap(j));
        unsigned int cur = m_result.components[i];
        if (cur >= 1 && cur <= m_physical_names.size())
            combo->SetSelection(cur - 1);
        combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_filament_changed(); });
        row->Add(combo, 1, wxALIGN_CENTER_VERTICAL);

        m_combo_filaments.push_back(combo);
        m_material_rows_sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(9));
    }

    sizer->Add(m_material_rows_sizer, 0, wxEXPAND);

    m_btn_add_material = new Button(this, _L("+ Add Material"));
    m_btn_add_material->SetBackgroundColor(wxColour("#F8F8F8"));
    m_btn_add_material->SetBorderColor(wxColour("#EEEEEE"));
    m_btn_add_material->SetTextColor(wxColour("#262E30"));
    m_btn_add_material->SetMinSize(wxSize(-1, FromDIP(23)));
    m_btn_add_material->SetCursor(wxCursor(wxCURSOR_HAND));
    m_btn_add_material->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_add_material(); });
    sizer->Add(m_btn_add_material, 0, wxEXPAND | wxTOP, FromDIP(9));

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
        dc.SetPen(wxPen(*wxWHITE, FromDIP(2)));
        dc.DrawLine(div_x, 0, div_x, sz.GetHeight());
    });

    m_ratio_bar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        m_dragging = true;
        m_ratio_bar->CaptureMouse();
        int new_ratio = 100 - (int)(e.GetX() * 100.0 / m_ratio_bar->GetClientSize().GetWidth() + 0.5);
        on_ratio_changed(std::max(10, std::min(90, new_ratio)));
    });

    m_ratio_bar->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        if (!m_dragging) return;
        int new_ratio = 100 - (int)(e.GetX() * 100.0 / m_ratio_bar->GetClientSize().GetWidth() + 0.5);
        on_ratio_changed(std::max(10, std::min(90, new_ratio)));
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
    m_label_ratio_a = new wxStaticText(this, wxID_ANY, wxString::Format(wxT("%d%%"), ratio(0)));
    m_label_ratio_a->SetFont(::Label::Body_10);
    m_label_ratio_b = new wxStaticText(this, wxID_ANY, wxString::Format(wxT("%d%%"), ratio(1)));
    m_label_ratio_b->SetFont(::Label::Body_10);
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

        dc.SetBrush(wxBrush(*wxWHITE));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

        wxColour c0 = comp_colour(0), c1 = comp_colour(1), c2 = comp_colour(2);

        // Fill triangle with per-pixel barycentric interpolation
        int min_y = (int)std::min({v0.y, v1.y, v2.y});
        int max_y = (int)std::max({v0.y, v1.y, v2.y});
        int min_x = (int)std::min({v0.x, v1.x, v2.x});
        int max_x = (int)std::max({v0.x, v1.x, v2.x});

        wxBitmap bmp(sz.GetWidth(), sz.GetHeight(), 24);
        wxMemoryDC mdc(bmp);
        mdc.SetBrush(*wxWHITE_BRUSH);
        mdc.SetPen(*wxTRANSPARENT_PEN);
        mdc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                TriPoint p = {(double)px, (double)py};
                if (!tri_contains(p, v0, v1, v2)) continue;
                double w0, w1, w2;
                tri_barycentric(p, v0, v1, v2, w0, w1, w2);
                int r = (int)(c0.Red() * w0 + c1.Red() * w1 + c2.Red() * w2);
                int g = (int)(c0.Green() * w0 + c1.Green() * w1 + c2.Green() * w2);
                int b = (int)(c0.Blue() * w0 + c1.Blue() * w1 + c2.Blue() * w2);
                mdc.SetPen(wxPen(wxColour(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255))));
                mdc.DrawPoint(px, py);
            }
        }

        // Triangle border
        mdc.SetPen(wxPen(wxColour("#CECECE"), 1));
        mdc.SetBrush(*wxTRANSPARENT_BRUSH);
        wxPoint pts[3] = {{(int)v0.x, (int)v0.y}, {(int)v1.x, (int)v1.y}, {(int)v2.x, (int)v2.y}};
        mdc.DrawPolygon(3, pts);

        // Drag handle
        double hx = m_tri_wx * v0.x + m_tri_wy * v1.x + m_tri_wz * v2.x;
        double hy = m_tri_wx * v0.y + m_tri_wy * v1.y + m_tri_wz * v2.y;
        int handle_r = FromDIP(5);
        mdc.SetBrush(*wxWHITE_BRUSH);
        mdc.SetPen(wxPen(wxColour("#262E30"), FromDIP(2)));
        mdc.DrawCircle((int)hx, (int)hy, handle_r);

        mdc.SelectObject(wxNullBitmap);
        dc.DrawBitmap(bmp, 0, 0);

        // Percentage labels near vertices
        dc.SetFont(::Label::Body_10);
        dc.SetTextForeground(wxColour("#262E30"));
        if (m_result.ratios.size() >= 3) {
            wxString s0 = wxString::Format(wxT("%d%%"), m_result.ratios[0]);
            wxString s1 = wxString::Format(wxT("%d%%"), m_result.ratios[1]);
            wxString s2 = wxString::Format(wxT("%d%%"), m_result.ratios[2]);
            wxSize ts0 = dc.GetTextExtent(s0);
            int top_label_y = std::max(0, (int)(v0.y - ts0.GetHeight() - FromDIP(4)));
            dc.DrawText(s0, (int)(v0.x - ts0.GetWidth() / 2), top_label_y);

            dc.SetFont(::Label::Body_12);
            dc.SetTextForeground(wxColour("#909090"));
            dc.DrawText(_L("Ratio"), FromDIP(2), top_label_y);
            dc.SetFont(::Label::Body_10);
            dc.SetTextForeground(wxColour("#262E30"));
            wxSize ts1 = dc.GetTextExtent(s1);
            dc.DrawText(s1, (int)(v1.x - ts1.GetWidth() / 2), (int)(v1.y + FromDIP(3)));
            wxSize ts2 = dc.GetTextExtent(s2);
            dc.DrawText(s2, (int)(v2.x - ts2.GetWidth() / 2), (int)(v2.y + FromDIP(3)));
        }
    });

    auto handle_mouse = [this, get_vertices](wxMouseEvent& e, bool is_down) {
        auto [v0, v1, v2] = get_vertices();
        TriPoint p = {(double)e.GetX(), (double)e.GetY()};

        if (is_down) {
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

        if (m_result.ratios.size() >= 3) {
            m_result.ratios[0] = r0;
            m_result.ratios[1] = r1;
            m_result.ratios[2] = r2;
        }

        update_preview();
    };

    m_triangle_panel->Bind(wxEVT_LEFT_DOWN, [handle_mouse](wxMouseEvent& e) {
        handle_mouse(e, true);
    });
    m_triangle_panel->Bind(wxEVT_MOTION, [handle_mouse](wxMouseEvent& e) {
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

    m_chk_gradient = new wxCheckBox(this, wxID_ANY, _L("Gradient Effect"));
    m_chk_gradient->SetValue(m_result.gradient_enabled);
    m_chk_gradient->SetFont(::Label::Body_13);
    m_chk_gradient->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { on_gradient_toggled(); });

    m_gradient_sizer->Add(m_chk_gradient, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(4));

    m_combo_gradient_dir = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                        wxSize(FromDIP(152), FromDIP(24)), 0, nullptr, wxCB_READONLY);
    m_combo_gradient_dir->SetKeepDropArrow(true);
    update_gradient_direction_items();
    m_combo_gradient_dir->SetSelection(m_result.gradient_direction);
    m_combo_gradient_dir->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_gradient_direction_changed(); });
    m_combo_gradient_dir->Show(m_result.gradient_enabled);

    m_gradient_sizer->Add(m_combo_gradient_dir, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    return m_gradient_sizer;
}

wxBoxSizer* MixedFilamentDialog::create_recommendation_grid()
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* rec_label = new wxStaticText(this, wxID_ANY, _L("Mixing Recommendations"));
    rec_label->SetForegroundColour(wxColour("#ACACAC"));
    rec_label->SetFont(::Label::Body_10);
    title_sizer->Add(rec_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    auto* rec_line = new wxPanel(this, wxID_ANY);
    rec_line->SetMinSize(wxSize(-1, 1));
    rec_line->SetBackgroundColour(wxColour("#ACACAC"));
    title_sizer->Add(rec_line, 1, wxALIGN_CENTER_VERTICAL);

    outer->Add(title_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    auto* scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(116)));
    scroll->SetScrollRate(0, 5);
    scroll->SetBackgroundColour(wxColour("#F8F8F8"));

    auto* grid = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);

    size_t n = m_physical_colors.size();
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            auto* item = new wxPanel(scroll, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)));
            wxColour ca(m_physical_colors[i]);
            wxColour cb(m_physical_colors[j]);
            wxColour mixed = blend_colors(ca, cb, 0.5);
            item->SetBackgroundColour(mixed);
            item->SetCursor(wxCursor(wxCURSOR_HAND));

            unsigned int comp_a = (unsigned int)(i + 1);
            unsigned int comp_b = (unsigned int)(j + 1);
            item->Bind(wxEVT_LEFT_UP, [this, comp_a, comp_b](wxMouseEvent&) {
                on_recommendation_clicked(comp_a, comp_b);
            });
            item->SetToolTip(wxString::Format(wxT("%s + %s"),
                wxString::FromUTF8(m_physical_names[i]), wxString::FromUTF8(m_physical_names[j])));

            grid->Add(item, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        }
    }

    scroll->SetSizer(grid);
    scroll->SetScrollbars(0, FromDIP(20), 0, 1);

    outer->Add(scroll, 1, wxEXPAND | wxTOP, FromDIP(4));
    return outer;
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

void MixedFilamentDialog::update_type_constraints()
{
    if (m_physical_types.empty() || m_combo_filaments.size() < 2) return;

    std::string first_type;
    bool mismatch = false;
    for (size_t i = 0; i < m_combo_filaments.size(); ++i) {
        int sel = m_combo_filaments[i]->GetSelection();
        if (sel < 0 || sel >= (int)m_physical_types.size()) continue;
        if (first_type.empty()) {
            first_type = m_physical_types[sel];
        } else if (m_physical_types[sel] != first_type) {
            mismatch = true;
            break;
        }
    }

    if (mismatch) {
        wxMessageDialog dlg(this,
            _L("Mixing different filament types (e.g. PLA + ABS) may cause print quality issues. Continue?"),
            _L("Filament Type Mismatch"),
            wxYES_NO | wxICON_WARNING);
        if (dlg.ShowModal() != wxID_YES) {
            for (size_t i = 0; i < m_combo_filaments.size() && i < m_result.components.size(); ++i) {
                unsigned int prev = m_result.components[i];
                if (prev >= 1 && prev <= m_physical_names.size())
                    m_combo_filaments[i]->SetSelection(prev - 1);
            }
            return;
        }
        for (size_t i = 0; i < m_combo_filaments.size() && i < m_result.components.size(); ++i) {
            int sel = m_combo_filaments[i]->GetSelection();
            if (sel >= 0) m_result.components[i] = (unsigned int)(sel + 1);
        }
    }
}

// ---- Event Handlers ----

void MixedFilamentDialog::on_filament_changed()
{
    update_type_constraints();

    for (size_t i = 0; i < m_combo_filaments.size() && i < m_result.components.size(); ++i) {
        int sel = m_combo_filaments[i]->GetSelection();
        if (sel >= 0)
            m_result.components[i] = (unsigned int)(sel + 1);
    }

    update_gradient_direction_items();
    update_preview();
    update_ok_button_state();
}

void MixedFilamentDialog::on_ratio_changed(int new_ratio_a)
{
    if (m_result.ratios.size() < 2) return;
    m_result.ratios[0] = new_ratio_a;
    m_result.ratios[1] = 100 - new_ratio_a;

    if (m_label_ratio_a)
        m_label_ratio_a->SetLabel(wxString::Format(wxT("%d%%"), m_result.ratios[0]));
    if (m_label_ratio_b)
        m_label_ratio_b->SetLabel(wxString::Format(wxT("%d%%"), m_result.ratios[1]));

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
        m_ratio_sizer->ShowItems(!m_result.gradient_enabled);
    if (m_combo_gradient_dir)
        m_combo_gradient_dir->Show(m_result.gradient_enabled);

    Layout();
    Refresh();
}

void MixedFilamentDialog::on_gradient_direction_changed()
{
    if (m_combo_gradient_dir)
        m_result.gradient_direction = m_combo_gradient_dir->GetSelection();
}

void MixedFilamentDialog::on_add_material()
{
    size_t n = num_components();
    if (n >= (size_t)MAX_COMPONENTS) return;

    unsigned int new_comp = 1;
    if (m_physical_colors.size() > n)
        new_comp = (unsigned int)(n + 1);
    m_result.components.push_back(new_comp);

    // Redistribute ratios equally
    int each = 100 / (int)(n + 1);
    m_result.ratios.clear();
    int assigned = 0;
    for (size_t i = 0; i < n; ++i) {
        m_result.ratios.push_back(each);
        assigned += each;
    }
    m_result.ratios.push_back(100 - assigned);

    // Initialize triangle weights
    if (m_result.components.size() == 3) {
        m_tri_wx = m_result.ratios[0] / 100.0;
        m_tri_wy = m_result.ratios[1] / 100.0;
        m_tri_wz = m_result.ratios[2] / 100.0;
    }

    // Add new combo row
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    wxString lbl_text = wxString::Format(_L("Filament %zu"), n + 1);
    auto* lbl = new wxStaticText(this, wxID_ANY, lbl_text);
    lbl->SetFont(::Label::Body_12);
    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    auto* combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize(FromDIP(166), FromDIP(24)), 0, nullptr, wxCB_READONLY);
    combo->SetKeepDropArrow(true);
    for (size_t j = 0; j < m_physical_names.size(); ++j)
        combo->Append(wxString::FromUTF8(m_physical_names[j]), make_swatch_bitmap(j));
    if (new_comp >= 1 && new_comp <= m_physical_names.size())
        combo->SetSelection(new_comp - 1);
    combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_filament_changed(); });
    row->Add(combo, 1, wxALIGN_CENTER_VERTICAL);

    m_combo_filaments.push_back(combo);
    m_material_rows_sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(9));

    update_component_count_ui();
    update_preview();
    update_ok_button_state();

    Layout();
    Refresh();
}

void MixedFilamentDialog::on_recommendation_clicked(unsigned int comp_a, unsigned int comp_b)
{
    // Reset to 2-component with these selections
    while (m_result.components.size() > 2) {
        m_result.components.pop_back();
        m_result.ratios.pop_back();
        if (!m_combo_filaments.empty() && m_combo_filaments.size() > 2)
            m_combo_filaments.pop_back();
    }

    m_result.components = {comp_a, comp_b};
    m_result.ratios = {50, 50};

    if (m_combo_filaments.size() >= 2) {
        if (comp_a >= 1 && comp_a <= m_physical_names.size())
            m_combo_filaments[0]->SetSelection(comp_a - 1);
        if (comp_b >= 1 && comp_b <= m_physical_names.size())
            m_combo_filaments[1]->SetSelection(comp_b - 1);
    }

    if (m_label_ratio_a) m_label_ratio_a->SetLabel(wxT("50%"));
    if (m_label_ratio_b) m_label_ratio_b->SetLabel(wxT("50%"));

    while (m_material_rows_sizer->GetItemCount() > 2) {
        auto* sizer_item = m_material_rows_sizer->GetItem(m_material_rows_sizer->GetItemCount() - 1);
        if (sizer_item && sizer_item->GetSizer())
            sizer_item->GetSizer()->Clear(true);
        m_material_rows_sizer->Remove(m_material_rows_sizer->GetItemCount() - 1);
    }

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

void MixedFilamentDialog::update_ok_button_state()
{
    if (!m_btn_ok) return;

    // Check for duplicate components
    bool has_dup = false;
    for (size_t i = 0; i < m_result.components.size() && !has_dup; ++i)
        for (size_t j = i + 1; j < m_result.components.size() && !has_dup; ++j)
            if (m_result.components[i] == m_result.components[j])
                has_dup = true;

    m_btn_ok->Enable(!has_dup);
    if (has_dup)
        m_btn_ok->SetToolTip(_L("Components cannot use the same filament"));
    else
        m_btn_ok->SetToolTip(wxEmptyString);
}

void MixedFilamentDialog::update_gradient_direction_items()
{
    if (!m_combo_gradient_dir) return;

    int prev_sel = m_combo_gradient_dir->GetSelection();
    m_combo_gradient_dir->Clear();

    if (num_components() < 2) return;

    auto make_direction_bitmap = [this](size_t idx_from, size_t idx_to) -> wxBitmap {
        int swatch_sz = FromDIP(16);
        int arrow_w   = FromDIP(16);
        int gap       = FromDIP(4);
        int bmp_w = swatch_sz + gap + arrow_w + gap + swatch_sz;
        int bmp_h = swatch_sz;

        wxBitmap bmp(bmp_w, bmp_h);
        wxMemoryDC dc(bmp);

        dc.SetBrush(*wxWHITE_BRUSH);
        dc.SetPen(*wxWHITE_PEN);
        dc.DrawRectangle(0, 0, bmp_w, bmp_h);
        dc.SetFont(::Label::Body_13);

        auto draw_swatch = [&](int x, size_t idx) {
            wxColour col("#D9D9D9");
            if (idx < m_physical_colors.size())
                col = wxColour(m_physical_colors[idx]);
            dc.SetBrush(wxBrush(col));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(x, 0, swatch_sz, swatch_sz, FromDIP(2));
            wxString num = wxString::Format(wxT("%zu"), idx + 1);
            wxSize txt_sz = dc.GetTextExtent(num);
            dc.SetTextForeground(col.GetLuminance() > 0.5 ? wxColour("#262E30") : *wxWHITE);
            dc.DrawText(num, x + (swatch_sz - txt_sz.GetWidth()) / 2,
                             (swatch_sz - txt_sz.GetHeight()) / 2);
        };

        int x = 0;
        draw_swatch(x, idx_from);
        x += swatch_sz + gap;

        dc.SetTextForeground(wxColour("#262E30"));
        wxString arrow = wxT("\u2192");
        wxSize arrow_sz = dc.GetTextExtent(arrow);
        dc.DrawText(arrow, x + (arrow_w - arrow_sz.GetWidth()) / 2,
                           (bmp_h - arrow_sz.GetHeight()) / 2);
        x += arrow_w + gap;

        draw_swatch(x, idx_to);

        dc.SelectObject(wxNullBitmap);
        return bmp;
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
        m_combo_gradient_dir->Show(show_gradient && m_result.gradient_enabled);
    }
    if (is_three) {
        m_result.gradient_enabled = false;
        if (m_chk_gradient) m_chk_gradient->SetValue(false);
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
            m_btn_add_material->SetToolTip(_L("Maximum number of components reached"));
        }
    }
}

} // namespace GUI
} // namespace Slic3r

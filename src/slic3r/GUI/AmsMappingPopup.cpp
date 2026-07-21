#include "AmsMappingPopup.hpp"
#include "I18N.hpp"

#include "libslic3r/Utils.hpp"
#include "libslic3r/Thread.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "DeviceCore/DevConfigUtil.h"
#include "GUI_Preview.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/StaticBox.hpp"

#include <wx/progdlg.h>
#include <wx/clipbrd.h>
#include <wx/dcgraph.h>
#include <wx/graphics.h>
#include <wx/display.h>
#include <miniz.h>
#include <algorithm>
#include <optional>
#include "Plater.hpp"
#include "BitmapCache.hpp"
#include "BindDialog.hpp"
#include "FilamentBitmapUtils.hpp"

#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevFilaSwitch.h"
#include "DeviceCore/DevMappingNozzle.h"
#include "DeviceCore/DevNozzleRack.h" // Orca: full type for m_rack.lock()->GetNozzleSystem()->GetOwner()

#include "DeviceTab/wgtDeviceNozzleSelect.h"
#include "DeviceTab/wgtMsgPanel.h"

namespace Slic3r { namespace GUI {
#define MATERIAL_ITEM_SIZE wxSize(FromDIP(65), FromDIP(50))
#define MATERIAL_REC_WHEEL_SIZE wxSize(FromDIP(17), FromDIP(16))
#define MAPPING_ITEM_REAL_SIZE wxSize(FromDIP(48), FromDIP(60))


wxDEFINE_EVENT(EVT_SET_FINISH_MAPPING, wxCommandEvent);
const int LEFT_OFFSET = 2;

 MaterialItem::MaterialItem(wxWindow *parent, wxColour mcolour, wxString mname, std::string filament_id)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {
    m_arraw_bitmap_gray  = ScalableBitmap(this, "drop_down2", FromDIP(8));
    m_arraw_bitmap_white =  ScalableBitmap(this, "topbar_dropdown", 12);
    m_transparent_mitem          = ScalableBitmap(this, "transparent_material_up", 20);
    m_filament_wheel_transparent = ScalableBitmap(this,   "filament_transparent", 25);//wxGetApp().dark_mode() ? "filament_dark_transparent"
    //m_ams_wheel_mitem = ScalableBitmap(this, "ams_wheel", FromDIP(25));
    m_ams_wheel_mitem = ScalableBitmap(this, "ams_wheel_narrow", 25);
    m_ams_not_match = ScalableBitmap(this, "filament_not_mactch", 25);
    m_rack_nozzle_bitmap = ScalableBitmap(this, "dev_rack_nozzle_print_job", 22);

    m_material_coloul = mcolour;
    m_material_name = mname;
    m_ams_coloul      = wxColour(0xEE,0xEE,0xEE);

    m_filament_id = filament_id;

#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    messure_size();
    SetBackgroundColour(*wxWHITE);

    Bind(wxEVT_PAINT, &MaterialItem::paintEvent, this);
    wxGetApp().UpdateDarkUI(this);
 }

 MaterialItem::~MaterialItem() {}

void MaterialItem::msw_rescale() {
    m_arraw_bitmap_gray.msw_rescale();
    m_arraw_bitmap_white.msw_rescale();
    m_transparent_mitem.msw_rescale();
    m_ams_wheel_mitem.msw_rescale();
}

void MaterialItem::allow_paint_dropdown(bool flag) {
    if (m_dropdown_allow_painted != flag) {
        m_dropdown_allow_painted = flag;
    }
}

void MaterialItem::set_ams_text(const wxString& txt)
{
    if (m_ams_name != txt) {
        m_ams_name = txt;
        messure_size();
        Refresh();
    }
}

void MaterialItem::set_ams_info(wxColour col, wxString txt, int ctype, std::vector<wxColour> cols, bool record_back_info)
{
    auto need_refresh = false;
    if (record_back_info) {
        m_back_ams_cols = cols;
        m_back_ams_ctype = ctype;
        m_back_ams_coloul = col;
        m_back_ams_name  = txt;
    }
    if (m_ams_cols != cols) { m_ams_cols = cols; need_refresh = true; }
    if (m_ams_ctype != ctype) { m_ams_ctype = ctype; need_refresh = true; }
    if (m_ams_coloul != col) { m_ams_coloul = col; need_refresh = true;}
    if (m_ams_name != txt) { m_ams_name = txt; need_refresh = true; }
    if (need_refresh) {
        messure_size();
        Refresh();
    }
    BOOST_LOG_TRIVIAL(info) << "set_ams_info " << m_ams_name;
}

void MaterialItem::set_nozzle_info(const wxString& mapped_nozzle_str)
{
    if (m_mapped_nozzle_str != mapped_nozzle_str) {
        m_mapped_nozzle_str = mapped_nozzle_str;
        messure_size();
        Refresh();
    }
}

void MaterialItem::set_material_cols(int ctype, const std::vector<wxColour>& cols) {
    if (m_material_ctype != ctype || m_material_cols != cols) {
        m_material_ctype = ctype;
        m_material_cols = cols;
        Refresh();
    }
}

void MaterialItem::reset_ams_info() {
    m_ams_name   = "-";
    m_ams_coloul = wxColour(0xCE, 0xCE, 0xCE);
    m_ams_cols.clear();
    m_ams_ctype = 0;
    m_mapped_nozzle_str.Clear();
    messure_size();
    Refresh();
}

void MaterialItem::disable()
{
    if (IsEnabled()) {
        m_enable = false;
    }
}

void MaterialItem::enable()
{
    if (!IsEnabled()) {
        m_enable = true;
    }
}

void MaterialItem::on_selected()
{
    if (!m_selected) {
        m_selected = true;
        Refresh();
    }
}

void MaterialItem::on_warning()
{
    if (!m_warning) {
        m_warning = true;
        Refresh();
    }
}

void MaterialItem::on_normal()
{
    if (m_selected || m_warning) {
        m_selected = false;
        m_warning  = false;
        Refresh();
    }
}


void MaterialItem::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void MaterialItem::render(wxDC &dc)
{
    if (m_ams_name.empty()) {
        m_mapping_text = "-";
    } else {
        m_mapping_text = m_ams_name;
    }

    if (m_mapping_text == "-") { m_match = false;}
    else {m_match = true;}

#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({0, 0}, size, &dc, {0, 0});

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}


void MaterialItem::match(bool mat)
{
    m_match = mat;
    Refresh();
}

static constexpr size_t MAPPED_NOZZLE_STR_MAX_CHARS_PER_LINE = 7;

static wxString s_wrap_mapped_nozzle_str(const wxString& text)
{
    if (text.length() <= MAPPED_NOZZLE_STR_MAX_CHARS_PER_LINE) {
        return text;
    }

    wxArrayString parts = wxSplit(text, ',');
    wxString wrapped;
    std::vector<wxString> buffers;
    for (auto& part : parts) {
        if (!part.empty()) {
            buffers.push_back(part.Trim(false).Trim(true));
        }

        if (buffers.size() > 2) {
            if (!wrapped.empty()) {
                wrapped += "\n";
            }

            wrapped += buffers[0];
            wrapped += " ";
            wrapped += buffers[1];
            wrapped += " ";
            wrapped += buffers[2];
            buffers.clear();
        }
    }

    if (!wrapped.empty()) {
        wrapped += "\n";
    }
    if (buffers.size() > 0) {
        wrapped += buffers[0];
    }
    if (buffers.size() > 1) {
        wrapped += " ";
        wrapped += buffers[1];
    }

    return wrapped;
}

static int s_get_mapped_nozzle_str_line_count(const wxString& text)
{
    if (text.empty()) {
        return 0;
    }

    return std::max(1, static_cast<int>(wxSplit(s_wrap_mapped_nozzle_str(text), '\n', '\0').size()));
}

void MaterialItem::doRender(wxDC& dc)
{
    wxSize size = GetSize();
    auto mcolor = m_material_coloul;
    auto acolor = m_ams_coloul;
    change_the_opacity(acolor);

    if (mcolor.Alpha() == 0)
    {
        dc.DrawBitmap(m_transparent_mitem.bmp(), FromDIP(1), FromDIP(1));
    }

    if (!IsEnabled())
    {
        mcolor = wxColour(0x90, 0x90, 0x90);
        acolor = wxColour(0x90, 0x90, 0x90);
    }

    //top
    dc.SetPen(*wxTRANSPARENT_PEN);
    if (m_material_cols.size() > 1 && IsEnabled()) {
        int top_h = FromDIP(20);
        bool drawn = false;

        wxGraphicsContext* gc = dc.GetGraphicsContext();
        if (gc) {
            double r = FromDIP(5);
            auto make_path = [&](double x, double w, bool left_round, bool right_round) {
                wxGraphicsPath p = gc->CreatePath();
                double rl = left_round  ? std::min<double>(r, std::min<double>(w, top_h)) : 0.0;
                double rr = right_round ? std::min<double>(r, std::min<double>(w, top_h)) : 0.0;
                p.MoveToPoint(x, top_h);
                p.AddLineToPoint(x, rl);
                if (rl > 0) p.AddArc(x + rl, rl, rl, M_PI, -M_PI / 2, true);
                p.AddLineToPoint(x + w - rr, 0);
                if (rr > 0) p.AddArc(x + w - rr, rr, rr, -M_PI / 2, 0, true);
                p.AddLineToPoint(x + w, top_h);
                p.CloseSubpath();
                return p;
            };

            if (m_material_ctype == 0) {
                wxGraphicsPath path = make_path(0, size.x, true, true);
                wxGraphicsGradientStops stops(m_material_cols.front(), m_material_cols.back());
                for (size_t i = 1; i + 1 < m_material_cols.size(); i++) {
                    float pos = (float)i / (m_material_cols.size() - 1);
                    stops.Add(m_material_cols[i], pos);
                }
                gc->SetBrush(gc->CreateLinearGradientBrush(0, 0, size.x, 0, stops));
                gc->FillPath(path);
            } else {
                int cols_size = m_material_cols.size();
                for (int i = 0; i < cols_size; i++) {
                    double x0 = (double)size.x * i / cols_size;
                    double w  = (i == cols_size - 1) ? (size.x - x0) : ((double)size.x / cols_size);
                    wxGraphicsPath seg = make_path(x0, w, i == 0, i == cols_size - 1);
                    gc->SetBrush(gc->CreateBrush(wxBrush(m_material_cols[i])));
                    gc->FillPath(seg);
                }
            }
            drawn = true;
        }

        if (!drawn) {
            if (m_material_ctype == 0) {
                int seg = m_material_cols.size() - 1;
                int gw = std::round((double)size.x / seg);
                for (int i = 0; i < seg; i++) {
                    int x0 = gw * i;
                    int w  = (i == seg - 1) ? (size.x - x0) : gw;
                    fill_gradient_rect_east(dc, wxRect(x0, 0, w, top_h), m_material_cols[i], m_material_cols[i + 1]);
                }
            } else {
                int cols_size = m_material_cols.size();
                int cw = std::round((double)size.x / cols_size);
                for (int i = 0; i < cols_size; i++) {
                    dc.SetBrush(wxBrush(m_material_cols[i]));
                    int x0 = cw * i;
                    int w  = (i == cols_size - 1) ? (size.x - x0) : cw;
                    dc.DrawRectangle(x0, 0, w, top_h);
                }
            }
        }
    } else {
        dc.SetBrush(wxBrush(mcolor));
        dc.DrawRoundedRectangle(0, 0, size.x, FromDIP(20), 5);

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(mcolor));
        dc.DrawRectangle(0, FromDIP(10), size.x, FromDIP(10));
    }

    // materials name
    auto up = 0;
    auto material_name_colour = mcolor.GetLuminance() < 0.6 ? *wxWHITE : wxColour(0x26, 0x2E, 0x30);
    if (mcolor.Alpha() == 0) { material_name_colour = wxColour(0x26, 0x2E, 0x30); }
    dc.SetTextForeground(material_name_colour);

    dc.SetFont(::Label::Body_12);
    if (dc.GetTextExtent(m_material_name).x > GetSize().x - 10)
    {
        dc.SetFont(::Label::Body_10);
    }

    auto material_txt_size = dc.GetTextExtent(m_material_name);
    dc.DrawText(m_material_name, wxPoint((GetSize().x - material_txt_size.x) / 2, (FromDIP(20) - material_txt_size.y) / 2));

    dc.SetPen(wxColour(0xAC, 0xAC, 0xAC));
    dc.DrawLine(FromDIP(1), FromDIP(20), FromDIP(size.x), FromDIP(20));
    up += FromDIP(20);
    up += FromDIP(2); // spacing

    //bottom rectangle in wheel bitmap, size is MATERIAL_REC_WHEEL_SIZE(22)
    auto paint_recty = up + (m_ams_wheel_mitem.GetBmpSize().y - MATERIAL_REC_WHEEL_SIZE.y) / 2;
    auto left = (size.x / 2 - MATERIAL_REC_WHEEL_SIZE.x) / 2 + FromDIP(3);
    auto right = left + MATERIAL_REC_WHEEL_SIZE.x - FromDIP(3);
    dc.SetPen(*wxTRANSPARENT_PEN);
    //bottom
    if (m_ams_cols.size() > 1)
    {
        int gwidth = std::round(MATERIAL_REC_WHEEL_SIZE.x / (m_ams_cols.size() - 1));
        //gradient
        if (m_ams_ctype == 0)
        {
            for (int i = 0; i < m_ams_cols.size() - 1; i++)
            {
                auto rect = wxRect(left, paint_recty, right - left, MATERIAL_REC_WHEEL_SIZE.y);
                fill_gradient_rect_east(dc, rect, m_ams_cols[i], m_ams_cols[i + 1]);
                left += gwidth;
            }
        }
        else
        {
            int cols_size = m_ams_cols.size();
            for (int i = 0; i < cols_size; i++)
            {
                dc.SetBrush(wxBrush(m_ams_cols[i]));
                float x = left + ((float)MATERIAL_REC_WHEEL_SIZE.x) * i / cols_size;
                if (i != cols_size - 1)
                {
                    dc.DrawRoundedRectangle(x - FromDIP(LEFT_OFFSET), paint_recty, ((float)MATERIAL_REC_WHEEL_SIZE.x) / cols_size + FromDIP(3), MATERIAL_REC_WHEEL_SIZE.y, 3);
                }
                else
                {
                    dc.DrawRoundedRectangle(x - FromDIP(LEFT_OFFSET), paint_recty, ((float)MATERIAL_REC_WHEEL_SIZE.x) / cols_size - FromDIP(1), MATERIAL_REC_WHEEL_SIZE.y, 3);
                }
            }
        }
    }
    else
    {
        if (m_match)
        {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(acolor)));
            dc.DrawRectangle((size.x / 2 - MATERIAL_REC_WHEEL_SIZE.x) / 2 + FromDIP(3) - FromDIP(LEFT_OFFSET), paint_recty, MATERIAL_REC_WHEEL_SIZE.x - FromDIP(1),
                MATERIAL_REC_WHEEL_SIZE.y);
        }
    }

    if (m_match)
    {
        dc.SetPen(wxPen(wxGetApp().dark_mode() ? wxColour(107, 107, 107) : wxColour(0xAC, 0xAC, 0xAC), FromDIP(1)));
    }
    else
    {
        dc.SetPen(wxPen(wxColour(234, 31, 48), FromDIP(1)));
    }

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(FromDIP(0), FromDIP(0), size.x - FromDIP(0), size.y, 5);

    if (m_selected)
    {
        dc.SetPen(wxPen(wxColour("#009688"), FromDIP(2))); // ORCA selected item border color (Orca accent)
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(1), size.y - FromDIP(1), 5);
    } else if (m_warning) {
        dc.SetPen(wxPen(wxColour(0xFF, 0x6F, 0x00), FromDIP(2)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(1), size.y - FromDIP(1), 5);
    }
    //#endif

    auto wheel_left = (GetSize().x / 2 - m_ams_wheel_mitem.GetBmpSize().x) / 2 + FromDIP(2) - FromDIP(LEFT_OFFSET);
    auto wheel_top = up;

    if (!m_match)
    {
        wheel_left = (size.x - m_ams_not_match.GetBmpWidth()) / 2 - FromDIP(LEFT_OFFSET);
        dc.DrawBitmap(m_ams_not_match.bmp(), wheel_left, wheel_top);
    }
    else
    {
        if (acolor.Alpha() == 0)
        {
            dc.DrawBitmap(m_filament_wheel_transparent.bmp(), wheel_left, wheel_top);
        }
        else
        {
            dc.DrawBitmap(m_ams_wheel_mitem.bmp(), wheel_left, wheel_top);
        }
    }

    // text and arrow
    dc.SetTextForeground(StateColor::darkModeColorFor(wxColour(0x26, 0x2E, 0x30)));

    int text_pos_x = 0;
    int text_pos_y = 0;
    auto mapping_txt_size = wxSize(0, 0);
    if (m_mapping_text.size() >= 4)
    {
        m_mapping_text.insert(m_mapping_text.size() / 2, "\n");
        WxFontUtils::get_suitable_font_size(FromDIP(13), dc);
        mapping_txt_size = dc.GetTextExtent(m_mapping_text);
        text_pos_y = up + (m_ams_wheel_mitem.GetBmpSize().y - mapping_txt_size.y) / 2;
        text_pos_x = mapping_txt_size.x / 4;
    }
    else
    {
        dc.SetFont(::Label::Head_12);
        mapping_txt_size = dc.GetTextExtent(m_mapping_text);
        text_pos_y = up + (m_ams_wheel_mitem.GetBmpSize().y - mapping_txt_size.y) / 2;
        text_pos_x = 0;
    }

    int arrow_left = size.x - m_arraw_bitmap_white.GetBmpSize().x - FromDIP(4) - FromDIP(LEFT_OFFSET);
    if (m_match)
    {
        dc.DrawText(m_mapping_text, wxPoint(GetSize().x / 2 + (GetSize().x / 2 - mapping_txt_size.x) / 2 - FromDIP(8) - FromDIP(LEFT_OFFSET) + text_pos_x, text_pos_y));

        int arrow_y = text_pos_y + (mapping_txt_size.y - m_arraw_bitmap_white.GetBmpHeight()) / 2;
        if ((acolor.Red() > 160 && acolor.Green() > 160 && acolor.Blue() > 160) && (acolor.Red() < 180 && acolor.Green() < 180 && acolor.Blue() < 180))
        {
            dc.DrawBitmap(m_arraw_bitmap_white.bmp(), arrow_left, arrow_y);
        }
        else
        {
            dc.DrawBitmap(m_arraw_bitmap_gray.bmp(), arrow_left, arrow_y);
        }
    }

    up += m_ams_wheel_mitem.GetBmpSize().y;


    if (!m_mapped_nozzle_str.IsEmpty()) {
        wxPen dashed_pen(wxColour("#CECECE"), 1, wxPENSTYLE_SHORT_DASH); // Orca: WXCOLOUR_GREY400 macro not in scope here
        dc.SetPen(dashed_pen);
        up += FromDIP(4); // spacing
        dc.DrawLine(FromDIP(1), up, FromDIP(size.x), up);

        int bitmap_y = up + (size.y - up - m_rack_nozzle_bitmap.GetBmpHeight()) / 2;
        int bitmap_l = wheel_left + (m_ams_wheel_mitem.GetBmpWidth() - m_rack_nozzle_bitmap.GetBmpWidth()) / 2 + FromDIP(2);
        dc.DrawBitmap(m_rack_nozzle_bitmap.bmp(), bitmap_l, bitmap_y);

        const wxString wrapped_nozzle_str = s_wrap_mapped_nozzle_str(m_mapped_nozzle_str);
        dc.SetFont(::Label::Head_12);
        // Orca: no box-fit get_suitable_font_size overload here; the wrapped slot string is short, so the
        // fixed Head_12 label font (set above) renders it cleanly.
        const auto& text_size = dc.GetTextExtent(wrapped_nozzle_str);
        int text_x = (size.x + bitmap_l + m_rack_nozzle_bitmap.GetBmpWidth() - text_size.x) / 2;
        int text_y = up + (size.y - up - text_size.y) / 2;
        dc.DrawText(wrapped_nozzle_str, text_x, text_y);
    }
}

void MaterialItem::reset_valid_info() {
    set_ams_info(m_back_ams_coloul, m_back_ams_name, m_back_ams_ctype, m_back_ams_cols);
}

void MaterialItem::messure_size()
{
    if (m_mapped_nozzle_str.empty()) {
        SetSize(wxSize(FromDIP(65), FromDIP(50)));
        SetMinSize(wxSize(FromDIP(65), FromDIP(50)));
        SetMaxSize(wxSize(FromDIP(65), FromDIP(50)));
    } else {
        const int line_count = s_get_mapped_nozzle_str_line_count(m_mapped_nozzle_str);
        const int item_height = FromDIP(84 + std::max(0, line_count - 1) * 10);
        SetSize(wxSize(FromDIP(65), item_height));
        SetMinSize(wxSize(FromDIP(65), item_height));
        SetMaxSize(wxSize(FromDIP(65), item_height));
    }
}

MaterialSyncItem::MaterialSyncItem(wxWindow *parent, wxColour mcolour, wxString mname, std::string filament_id) : MaterialItem(parent, mcolour, mname, filament_id)
{

}

MaterialSyncItem::~MaterialSyncItem() {}

int  MaterialSyncItem::get_real_offset() {
    int real_left_offset = m_dropdown_allow_painted ? LEFT_OFFSET : -2;
    return real_left_offset;
}

void MaterialSyncItem::render(wxDC &dc)
{
    wxString mapping_txt = wxEmptyString;
    if (m_ams_name.empty()) {
        mapping_txt = "-";
    } else {
        mapping_txt = m_ams_name;
    }

    if (mapping_txt == "-") {
        m_match = false;
        mapping_txt = _L("Unmapped");
        SetToolTip(_L("Upper half area:  Original\nLower half area: The filament from original project will be used when unmapped.\nAnd you can click it to modify"));
    } else {
        m_match = true;
        if (m_dropdown_allow_painted) {
            SetToolTip(_L("Upper half area:  Original\nLower half area:  Filament in AMS\nAnd you can click it to modify"));
        } else {
            SetToolTip(_L("Upper half area:  Original\nLower half area:  Filament in AMS\nAnd you cannot click it to modify"));
        }
    }
    dc.SetFont(::Label::Body_12);
    if (dc.GetTextExtent(m_material_name).x > GetSize().x - 10) {
        dc.SetFont(::Label::Body_10);
    }
    auto mapping_txt_size = dc.GetTextExtent(mapping_txt);
    m_text_pos_y          = ((float) GetSize().y * 3 / 5 - mapping_txt_size.y) / 2 + (float) GetSize().y * 2 / 5;
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({0, 0}, size, &dc, {0, 0});

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif

    auto mcolor = m_material_coloul;
    auto acolor = m_ams_coloul;
    change_the_opacity(acolor);
    if (!IsEnabled()) {
        mcolor = wxColour(0x90, 0x90, 0x90);
        acolor = wxColour(0x90, 0x90, 0x90);
    }

    // materials name
    dc.SetFont(::Label::Body_12);

    auto material_name_colour = mcolor.GetLuminance() < 0.6 ? *wxWHITE : wxColour(0x26, 0x2E, 0x30);
    if (mcolor.Alpha() == 0) { material_name_colour = wxColour(0x26, 0x2E, 0x30); }
    dc.SetTextForeground(material_name_colour);

    auto full_text = m_material_index + " " + m_material_name;
    if (dc.GetTextExtent(full_text).x > GetSize().x - 10) {
        dc.SetFont(::Label::Body_10);
    }

    auto material_txt_size = dc.GetTextExtent(full_text);
    dc.DrawText(full_text, wxPoint((GetSize().x - material_txt_size.x) / 2, ((float) GetSize().y * 2 / 5 - material_txt_size.y) / 2));
    int real_left_offset = get_real_offset();
    if (m_match) {
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour(0x26, 0x2E, 0x30)));
        dc.SetFont(::Label::Head_12);
        dc.DrawText(mapping_txt, wxPoint(GetSize().x / 2 + (GetSize().x / 2 - mapping_txt_size.x) / 2 - FromDIP(8) - FromDIP(real_left_offset), m_text_pos_y));
    }
    else {
        if (mcolor.Alpha() == 0) {//Because there is no unknown background color
            material_name_colour = StateColor::darkModeColorFor(wxColour(0x26, 0x2E, 0x30));
        }
        dc.SetTextForeground(material_name_colour);
        if (mapping_txt_size.x > GetSize().x - 10) {
            dc.SetFont(::Label::Body_10);
            mapping_txt_size = dc.GetTextExtent(mapping_txt);
        }
        dc.DrawText(mapping_txt, wxPoint(GetSize().x / 2 - mapping_txt_size.x / 2 , m_text_pos_y));
    }
}

void MaterialSyncItem::doRender(wxDC &dc)
{
    wxSize size   = GetSize();
    auto   mcolor = m_material_coloul;
    auto   acolor = m_ams_coloul;
    change_the_opacity(acolor);

    if (mcolor.Alpha() == 0) {
        dc.DrawBitmap(m_transparent_mitem.bmp(), FromDIP(0), FromDIP(0));
    }

    if (!IsEnabled()) {
        mcolor = wxColour(0x90, 0x90, 0x90);
        acolor = wxColour(0x90, 0x90, 0x90);
    }

    // top
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(mcolor));
    dc.DrawRoundedRectangle(0, 0, size.x, FromDIP(20), 5);

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(mcolor));
    dc.DrawRectangle(0, FromDIP(10), size.x, FromDIP(10));

    dc.SetPen(wxColour(0xAC, 0xAC, 0xAC));
    dc.DrawLine(FromDIP(1), FromDIP(20), FromDIP(size.x), FromDIP(20));
    // bottom rectangle in wheel bitmap, size is MATERIAL_REC_WHEEL_SIZE(22)
    auto left  = (size.x / 2 - MATERIAL_REC_WHEEL_SIZE.x) / 2 + FromDIP(3);
    auto up    = (size.y * 0.4 + (size.y * 0.6 - MATERIAL_REC_WHEEL_SIZE.y) / 2);
    auto right = left + MATERIAL_REC_WHEEL_SIZE.x - FromDIP(3);
    dc.SetPen(*wxTRANSPARENT_PEN);
    int real_left_offset = get_real_offset();
    // bottom
    if (m_match) {
        if (m_ams_cols.size() > 1) {
            int gwidth = std::round(MATERIAL_REC_WHEEL_SIZE.x / (m_ams_cols.size() - 1));
            // gradient
            if (m_ams_ctype == 0) {
                if (!m_dropdown_allow_painted) {
                    left +=  FromDIP(5);
                    right += FromDIP(5);
                }
                for (int i = 0; i < m_ams_cols.size() - 1; i++) {
                    auto rect = wxRect(left, up, right - left, MATERIAL_REC_WHEEL_SIZE.y);
                    fill_gradient_rect_east(dc, rect, m_ams_cols[i], m_ams_cols[i + 1]);
                    left += gwidth;
                }
            } else {
                if (!m_dropdown_allow_painted) {
                    left +=  FromDIP(5);
                }
                int cols_size = m_ams_cols.size();
                for (int i = 0; i < cols_size; i++) {
                    dc.SetBrush(wxBrush(m_ams_cols[i]));
                    float x = left + ((float) MATERIAL_REC_WHEEL_SIZE.x) * i / cols_size;
                    if (i != cols_size - 1) {
                        dc.DrawRoundedRectangle(x - FromDIP(real_left_offset), up, ((float) MATERIAL_REC_WHEEL_SIZE.x) / cols_size + FromDIP(3), MATERIAL_REC_WHEEL_SIZE.y, 3);
                    } else {
                        dc.DrawRoundedRectangle(x - FromDIP(real_left_offset), up, ((float) MATERIAL_REC_WHEEL_SIZE.x) / cols_size - FromDIP(1), MATERIAL_REC_WHEEL_SIZE.y, 3);
                    }
                }
            }
        } else {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(acolor)));
            dc.DrawRectangle((size.x / 2 - MATERIAL_REC_WHEEL_SIZE.x) / 2 + FromDIP(3) - FromDIP(real_left_offset), up, MATERIAL_REC_WHEEL_SIZE.x - FromDIP(1),
                             MATERIAL_REC_WHEEL_SIZE.y);
        }
    }
    else {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(mcolor));
        dc.DrawRoundedRectangle(0, FromDIP(21), size.x, size.y - FromDIP(21), 5);

        dc.DrawRectangle(0, FromDIP(21), size.x, FromDIP(10));
    }

    ////border
#if __APPLE__
    dc.SetPen(wxColour(0xAC, 0xAC, 0xAC));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(1, 1, size.x - 1, size.y - 1, 5);

    if (m_selected) {
        dc.SetPen(wxColour("#009688")); // ORCA selected item border color (Orca accent)
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(1, 1, size.x - 1, size.y - 1, 5);
    }
#else

    dc.SetPen(wxPen(wxGetApp().dark_mode() ? wxColour(107, 107, 107) : wxColour(0xAC, 0xAC, 0xAC), FromDIP(1)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, 5);

    if (m_selected) {
        dc.SetPen(wxPen(wxColour("#009688"), FromDIP(2))); // ORCA selected item border color (Orca accent)
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(1), size.y - FromDIP(1), 5);
    }
#endif

    if (m_text_pos_y > 0 && m_match && m_dropdown_allow_painted) {
        // arrow (remove arrow)
        if ((acolor.Red() > 160 && acolor.Green() > 160 && acolor.Blue() > 160) && (acolor.Red() < 180 && acolor.Green() < 180 && acolor.Blue() < 180)) {
            dc.DrawBitmap(m_arraw_bitmap_white.bmp(), size.x - m_arraw_bitmap_white.GetBmpSize().x - FromDIP(4) - FromDIP(real_left_offset), m_text_pos_y + FromDIP(3));
        } else {
            dc.DrawBitmap(m_arraw_bitmap_gray.bmp(), size.x - m_arraw_bitmap_gray.GetBmpSize().x - FromDIP(4) - FromDIP(real_left_offset), m_text_pos_y + FromDIP(3));
        }
    }
    auto wheel_left = (GetSize().x / 2 - m_ams_wheel_mitem.GetBmpSize().x) / 2 + FromDIP(2);
    auto wheel_top  = ((float) GetSize().y * 0.6 - m_ams_wheel_mitem.GetBmpSize().y) / 2 + (float) GetSize().y * 0.4;
    if (m_match) {// different with parent
        if (acolor.Alpha() == 0) {
            dc.DrawBitmap(m_filament_wheel_transparent.bmp(), wheel_left - FromDIP(real_left_offset), wheel_top);
        } else {
            dc.DrawBitmap(m_ams_wheel_mitem.bmp(), wheel_left - FromDIP(real_left_offset), wheel_top);
        }
    }
    //not draw m_ams_not_match
}

void MaterialSyncItem::set_material_index_str(std::string str) {
    m_material_index = str;
}

AmsMapingPopup::AmsMapingPopup(wxWindow *parent, bool use_in_sync_dialog) :
    PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS), m_use_in_sync_dialog(use_in_sync_dialog)
 {
     Bind(wxEVT_PAINT, &AmsMapingPopup::paintEvent, this);

#ifdef __APPLE__
     Bind(wxEVT_LEFT_DOWN, &AmsMapingPopup::on_left_down, this);
     Bind(wxEVT_MOTION, &AmsMapingPopup::on_mouse_move, this);
#endif

     SetBackgroundColour(*wxWHITE);

     m_sizer_main = new wxBoxSizer(wxVERTICAL);
     m_sizer_main_h = new wxBoxSizer(wxHORIZONTAL);
     m_sizer_ams = new wxBoxSizer(wxHORIZONTAL);
     m_sizer_ams_left = new wxBoxSizer(wxVERTICAL);
     m_sizer_ams_right = new wxBoxSizer(wxVERTICAL);
     m_sizer_ams_left_horizonal = new wxBoxSizer(wxHORIZONTAL);
     m_sizer_ams_right_horizonal = new wxBoxSizer(wxHORIZONTAL);
     m_sizer_ams_basket_left = new wxBoxSizer(wxVERTICAL);
     m_sizer_ams_basket_right = new wxBoxSizer(wxVERTICAL);

     auto title_panel = new wxPanel(this, wxID_ANY);
     title_panel->SetBackgroundColour(StateColor::darkModeColorFor("#F1F1F1"));
     title_panel->SetSize(wxSize(-1, FromDIP(30)));
     title_panel->SetMinSize(wxSize(-1, FromDIP(30)));

     m_scrolled_window = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxHSCROLL);
     m_scrolled_window->SetBackgroundColour(*wxWHITE);
     m_scrolled_window->SetScrollRate(0, FromDIP(10));

     wxBoxSizer *title_sizer_h= new wxBoxSizer(wxHORIZONTAL);
     wxBoxSizer *title_sizer_v = new wxBoxSizer(wxVERTICAL);

     m_title_text = new wxStaticText(title_panel, wxID_ANY, _L("AMS Slots"));
     m_title_text->SetForegroundColour(wxColour("#262E30"));
     m_title_text->SetFont(::Label::Body_16);
     title_sizer_v->Add(m_title_text, 0, wxALIGN_LEFT | wxLEFT,  FromDIP(15));
     title_sizer_h->Add(title_sizer_v, 1, wxALIGN_CENTER, 5);
     title_panel->SetSizer(title_sizer_h);
     title_panel->Layout();
     title_panel->Fit();

     m_left_marea_panel = new wxPanel(m_scrolled_window);
     m_left_marea_panel->SetName("left");
     m_right_marea_panel = new wxPanel(m_scrolled_window);
     m_right_marea_panel->SetName("right");
     m_left_first_text_panel  = new wxPanel(m_left_marea_panel);
     m_right_first_text_panel = new wxPanel(m_right_marea_panel);
     auto sizer_temp = new wxBoxSizer(wxHORIZONTAL);
     /*left ext*/
     m_left_extra_slot = new MappingItem(m_left_marea_panel);
     m_left_extra_slot->m_ams_id = VIRTUAL_TRAY_DEPUTY_ID;
     m_left_extra_slot->m_slot_id = 0;
     m_left_extra_slot->SetSize(wxSize(FromDIP(48), FromDIP(60)));
     m_left_extra_slot->SetMinSize(wxSize(FromDIP(48), FromDIP(60)));
     m_left_extra_slot->SetMaxSize(wxSize(FromDIP(48), FromDIP(60)));

     auto left_panel = new wxPanel(m_left_marea_panel);
     left_panel->SetSize(wxSize(FromDIP(182), FromDIP(60)));
     left_panel->SetMinSize(wxSize(FromDIP(182), FromDIP(60)));
     left_panel->SetMaxSize(wxSize(FromDIP(182), FromDIP(60)));

     sizer_temp->Add(m_left_extra_slot);
     sizer_temp->Add(left_panel);

     /*right ext*/
     m_right_extra_slot = new MappingItem(m_right_marea_panel);
     m_right_extra_slot->m_ams_id = VIRTUAL_TRAY_MAIN_ID;
     m_right_extra_slot->m_slot_id = 0;
     m_right_extra_slot->SetSize(wxSize(FromDIP(48), FromDIP(60)));
     m_right_extra_slot->SetMinSize(wxSize(FromDIP(48), FromDIP(60)));
     m_right_extra_slot->SetMaxSize(wxSize(FromDIP(48), FromDIP(60)));

     std::string amp_pt = wxGetApp().preset_bundle->printers.get_edited_preset().get_printer_type(wxGetApp().preset_bundle);
     m_single_tip_text = _L("Please select from the following filaments");
     m_left_tip_text = wxString::Format(_L("Select filament that installed to the %s"),
         _L(DevPrinterConfigUtil::get_toolhead_display_name(amp_pt, DEPUTY_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::LowerCase)));
     m_right_tip_text = wxString::Format(_L("Select filament that installed to the %s"),
         _L(DevPrinterConfigUtil::get_toolhead_display_name(amp_pt, MAIN_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::LowerCase)));

     m_left_tips = new Label(m_left_first_text_panel);
     m_left_tips->SetForegroundColour(StateColor::darkModeColorFor("0x262E30"));
     m_left_tips->SetBackgroundColour(StateColor::darkModeColorFor("0xFFFFFF"));
     m_left_tips->SetFont(::Label::Body_13);
     m_left_tips->SetLabel(m_left_tip_text);
     m_left_first_text_panel->SetSizer(m_sizer_ams_left_horizonal);

     m_sizer_ams_left->Add(m_left_first_text_panel, 0, wxEXPAND | wxBOTTOM | wxTOP , FromDIP(8));
     m_left_split_ams_sizer = create_split_sizer(m_left_marea_panel, _L("Left AMS"));
     m_sizer_ams_left->Add(m_left_split_ams_sizer, 0, wxEXPAND, 0);
     m_sizer_ams_left->Add(m_sizer_ams_basket_left, 0, wxEXPAND|wxTOP, FromDIP(8));
     m_sizer_ams_left->Add(create_split_sizer(m_left_marea_panel, _L("External")), 0, wxEXPAND|wxTOP, FromDIP(8));
     //m_sizer_ams_left->Add(m_left_extra_slot, 0, wxEXPAND|wxTOP, FromDIP(8));
     m_sizer_ams_left->Add(sizer_temp, 0, wxEXPAND | wxTOP, FromDIP(8));

     m_right_tips = new Label(m_right_first_text_panel);
     m_right_tips->SetForegroundColour(StateColor::darkModeColorFor("0x262E30"));
     m_right_tips->SetBackgroundColour(StateColor::darkModeColorFor("0xFFFFFF"));
     m_right_tips->SetFont(::Label::Body_13);
     m_right_tips->SetLabel(m_right_tip_text);

     m_sizer_ams_right_horizonal->AddStretchSpacer();

     m_reset_btn = new ScalableButton(m_scrolled_window, wxID_ANY, wxGetApp().dark_mode() ? "erase_dark" : "erase", wxEmptyString, wxDefaultSize, wxDefaultPosition,
                                      wxBU_EXACTFIT | wxNO_BORDER, true, 14);
     m_reset_btn->SetName(wxGetApp().dark_mode() ? "erase_dark" : "erase");
     m_reset_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { reset_ams_info(); });
     m_reset_btn->SetBackgroundColour(*wxWHITE);
     m_reset_btn->SetToolTip(_L("Reset current filament mapping"));

     m_sizer_ams_right_horizonal->AddStretchSpacer();
     m_sizer_ams_right_horizonal->AddSpacer(FromDIP(5));
     m_reset_btn->Hide();
     m_right_first_text_panel->SetSizer(m_sizer_ams_right_horizonal);
     const int same_height = 20;
     m_left_first_text_panel->SetMaxSize(wxSize(-1, FromDIP(same_height)));
     m_right_first_text_panel->SetMaxSize(wxSize(-1, FromDIP(same_height)));

     // content_sizer
     wxSizer *content_ams_sizer = new wxBoxSizer(wxVERTICAL);
     m_right_split_ams_sizer = create_split_sizer(m_right_marea_panel, _L("Right AMS"));
     m_right_split_ext_sizer = create_split_sizer(m_right_marea_panel, _L("External"));
     content_ams_sizer->Add(m_right_split_ams_sizer, 0, wxEXPAND, 0);
     content_ams_sizer->Add(m_sizer_ams_basket_right, 0, wxEXPAND | wxTOP, FromDIP(8));
     content_ams_sizer->Add(m_right_split_ext_sizer, 0, wxEXPAND | wxTOP, FromDIP(8));
     content_ams_sizer->Add(m_right_extra_slot, 0, wxEXPAND | wxTOP, FromDIP(8));

     wxSizer *content_sizer = new wxBoxSizer(wxHORIZONTAL);
     content_sizer->Add(content_ams_sizer, 1, wxEXPAND | wxLEFT);

     m_flush_warning_panel = new DevIconLabel(m_right_marea_panel, "warning", ""); // Orca: dev_warning icon absent; use the shipped generic warning glyph
     m_flush_warning_panel->SetAllBackgroundColor(StateColor::darkModeColorFor("#FFFFE0"));
     m_flush_warning_panel->GetLabelItem()->SetForegroundColour(StateColor::darkModeColorFor("#F09A17"));
     m_flush_warning_panel->Show(false);

     m_sizer_ams_right->Add(m_right_first_text_panel, 0, wxEXPAND | wxBOTTOM | wxTOP, FromDIP(8));
     m_sizer_ams_right->Add(m_flush_warning_panel, 0, wxBOTTOM | wxALIGN_LEFT, FromDIP(8));
     m_sizer_ams_right->Add(content_sizer, 0, wxEXPAND, 0);

     m_rack_nozzle_select = new wgtDeviceNozzleRackSelect(m_scrolled_window);
     m_rack_nozzle_select->Bind(EVT_NOZZLE_SELECT_CHANGED, &AmsMapingPopup::OnNozzleMappingSelected, this);
     m_rack_nozzle_select->Bind(EVT_NOZZLE_SELECT_CLICKED, [this](wxCommandEvent &e) { this->Dismiss(); });
     m_rack_nozzle_select->Show(false);

     m_ams_tips_msg_panel = new wgtMsgPanel(m_scrolled_window);

     m_left_marea_panel->SetSizer(m_sizer_ams_left);
     m_right_marea_panel->SetSizer(m_sizer_ams_right);

     m_sizer_ams_v = new wxBoxSizer(wxVERTICAL);

     m_split_line_panel = new wxPanel(m_scrolled_window, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(1), -1));
     m_split_line_panel->SetBackgroundColour(*wxWHITE);
     m_split_line_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent& evt) {
         wxPaintDC dc(m_split_line_panel);
         dc.SetPen(wxPen(wxColour(0xAC, 0xAC, 0xAC), 1, wxPENSTYLE_SHORT_DASH));
         dc.DrawLine(0, FromDIP(20), 0, m_sizer_ams->GetSize().GetHeight());
     });

     //m_sizer_ams->Add(m_left_marea_panel, 0, wxEXPAND, FromDIP(0));
     m_sizer_ams->Add(m_left_marea_panel, 0, wxRIGHT, FromDIP(10));
     m_sizer_ams->Add(m_split_line_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(7));
     m_sizer_ams->Add(m_right_marea_panel, 0, wxEXPAND | wxLEFT, FromDIP(10));

     m_sizer_ams_v->Add(m_reset_btn, 0, wxALIGN_RIGHT);
     m_sizer_ams_v->Add(m_sizer_ams, 0, wxEXPAND | wxBOTTOM, FromDIP(30));
     m_sizer_ams_v->AddStretchSpacer();
     m_sizer_ams_v->Add(m_ams_tips_msg_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(10)); // Orca: wxBOTTOM (REF uses the wxEdge enum by mistake)

     m_sizer_main_h->Add(m_sizer_ams_v, 0, wxEXPAND | wxRIGHT, FromDIP(10));
     m_sizer_main_h->Add(m_rack_nozzle_select, 0, wxEXPAND | wxTOP | wxLEFT, FromDIP(15));

     m_scrolled_window->SetSizer(m_sizer_main_h);
     m_sizer_main->Add(title_panel, 0, wxEXPAND | wxALL, FromDIP(2));
     m_sizer_main->Add(m_scrolled_window, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(14));
     m_sizer_main->Add( 0, 0, 0, wxTOP, FromDIP(14));

     SetSizer(m_sizer_main);
     Layout();
     Fit();

     Bind(wxEVT_SHOW, [this](wxShowEvent& e) {
         if (e.IsShown() && m_parent_item)
         {
             auto show_pos = m_parent_item->ClientToScreen(wxPoint(0, 0));
             int  display_idx = wxDisplay::GetFromWindow(m_parent_item);

             if (display_idx == wxNOT_FOUND)
                 display_idx = 0;

             wxRect screen_size = wxDisplay(display_idx).GetClientArea();
             auto   parent_size = m_parent_item->GetRect();
             auto   content_size = m_sizer_main_h->GetMinSize();
             int    popup_width  = content_size.x + FromDIP(28);
             int    popup_height = content_size.y;
             wxSize popup_size(popup_width, popup_height);

              // Horizontal Direction Processing
              if (show_pos.x + popup_size.x > screen_size.GetRight())
              {
                  show_pos.x = screen_size.GetRight() - popup_size.x;
              }

              //if popup size bigger than screen
              if (show_pos.x < screen_size.GetLeft())
              {
                  show_pos.x = screen_size.GetLeft();
                  m_scrolled_window->SetMaxSize(wxSize(screen_size.GetWidth(), popup_height));
                  m_scrolled_window->SetMinSize(wxSize(screen_size.GetWidth(), popup_height));
                  m_scrolled_window->SetScrollRate(FromDIP(10), FromDIP(10));
              }
              else
              {
                  m_scrolled_window->SetMaxSize(wxSize(popup_width, popup_height));
                  m_scrolled_window->SetMinSize(wxSize(popup_width, popup_height));
                  m_scrolled_window->SetScrollRate(0, FromDIP(10));
              }

              //Vertical Direction Processing
              int available_height_below = screen_size.GetBottom() - (show_pos.y + parent_size.height);
              int available_height_above = show_pos.y - screen_size.GetTop();

              if (popup_size.y <= available_height_below)
              {
                   show_pos.y = show_pos.y + parent_size.height;
              }
              else if (popup_size.y <= available_height_above)
              {
                  show_pos.y = show_pos.y - popup_size.y - FromDIP(34) - FromDIP(14);// title_height = FromDIP(30) + FromDIP(4)  padding = FromDIP(14)
              }
              else
              {
                  int max_available_height;

                  if (available_height_below >= available_height_above) {
                      show_pos.y           = show_pos.y + parent_size.height;
                      max_available_height = available_height_below;
                  } else {
                      show_pos.y           = screen_size.GetTop();
                      max_available_height = available_height_above;
                  }

                  int title_and_padding = FromDIP(30) + FromDIP(4) + FromDIP(14);
                  int max_scroll_height = max_available_height - title_and_padding;

                  m_scrolled_window->SetMaxSize(wxSize(m_scrolled_window->GetMaxSize().x,max_scroll_height));
              }
              Layout();
              Fit();
              this->Move(show_pos);
         }
     });
 }

 void AmsMapingPopup::reset_ams_info()
 {
     if (m_reset_callback) {
         m_reset_callback(m_material_index);
     }
 }

void AmsMapingPopup::set_reset_callback(ResetCallback callback) {
     m_reset_callback = callback;
}

void AmsMapingPopup::show_reset_button() {
    m_reset_btn->Show();
}

// Orca: dropped by the reference but still consumed by SelectMachine.
std::vector<TrayData> AmsMapingPopup::parse_ams_mapping(const std::map<std::string, DevAms*, NumericStrCompare>& amsList)
{
    std::vector<TrayData> m_tray_data;
    for (auto ams_iter = amsList.begin(); ams_iter != amsList.end(); ams_iter++) {
        BOOST_LOG_TRIVIAL(trace) << "ams_mapping ams id " << ams_iter->first.c_str();

        auto    ams_indx  = atoi(ams_iter->first.c_str());
        DevAms* ams_group = ams_iter->second;
        std::map<std::string, DevAmsTray*>::const_iterator tray_iter;

        for (tray_iter = ams_group->GetTrays().cbegin(); tray_iter != ams_group->GetTrays().cend(); tray_iter++) {
            DevAmsTray* tray_data = tray_iter->second;
            TrayData    td;

            td.id = ams_indx * AMS_TOTAL_COUNT + atoi(tray_data->id.c_str());

            if (!tray_data->is_exists) {
                td.type = EMPTY;
            } else {
                if (!tray_data->is_tray_info_ready()) {
                    td.type = THIRD;
                } else {
                    td.type          = NORMAL;
                    td.remain        = tray_data->remain;
                    td.colour        = DevAmsTray::decode_color(tray_data->color);
                    td.name          = tray_data->get_display_filament_type();
                    td.filament_type = tray_data->get_filament_type();
                }
            }

            m_tray_data.push_back(td);
        }
    }

    return m_tray_data;
}

void AmsMapingPopup::msw_rescale()
{
    m_left_extra_slot->msw_rescale();
    m_right_extra_slot->msw_rescale();
    for (auto item : m_mapping_item_list) { item->msw_rescale(); }
    for (auto container : m_amsmapping_container_list) { container->msw_rescale(); }

    m_flush_warning_panel->Rescale();
    m_rack_nozzle_select->Rescale();

    Fit();
    Refresh();
};

 void AmsMapingPopup::set_sizer_title(wxBoxSizer *sizer, wxString text) {
     if (!sizer) { return; }
     wxSizerItemList items = sizer->GetChildren();
     for (wxSizerItemList::iterator it = items.begin(); it != items.end(); ++it) {
         wxSizerItem *item       = *it;
         auto         temp_label = dynamic_cast<Label *>((item->GetWindow()));
         if (temp_label) {
             temp_label->SetLabel(text);
             break;
         }
     }
 }

 wxBoxSizer *AmsMapingPopup::create_split_sizer(wxWindow *parent, wxString text)
 {
    wxBoxSizer* sizer_split_ams = new wxBoxSizer(wxHORIZONTAL);
    auto ams_title_text = new Label(parent, text);
    ams_title_text->SetFont(::Label::Body_13);
    ams_title_text->SetBackgroundColour(*wxWHITE);
    ams_title_text->SetForegroundColour(0x909090);
    auto m_split_left_line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_split_left_line->SetBackgroundColour(0xeeeeee);
    m_split_left_line->SetMinSize(wxSize(-1, 1));
    m_split_left_line->SetMaxSize(wxSize(-1, 1));
    sizer_split_ams->Add(0, 0, 0, wxEXPAND, 0);
    sizer_split_ams->Add(ams_title_text, 0, wxALIGN_CENTER, 0);
    sizer_split_ams->Add(m_split_left_line, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND, 0);
    return sizer_split_ams;
 }

void AmsMapingPopup::update_materials_list(std::vector<std::string> list)
{
    m_materials_list = list;
}

void AmsMapingPopup::set_tag_texture(std::string texture)
{
    m_tag_material = texture;
}


bool AmsMapingPopup::is_match_material(std::string material) const
{
    return m_tag_material == material ? true : false;
}


void AmsMapingPopup::on_left_down(wxMouseEvent &evt)
{
    auto pos = ClientToScreen(evt.GetPosition());
    {//deal m_reset_btn
        auto& item   = m_reset_btn;
        auto  p_rect = item->ClientToScreen(wxPoint(0, 0));
        auto  left   = item->GetSize();
        if (pos.x > p_rect.x && pos.y > p_rect.y && pos.x < (p_rect.x + item->GetSize().x) && pos.y < (p_rect.y + item->GetSize().y)) {
            reset_ams_info();
            evt.StopPropagation();
            return;
        }
    }
    for (MappingItem *item : m_mapping_item_list) {
        auto p_rect = item->ClientToScreen(wxPoint(0, 0));
        auto left = item->GetSize();

        if (pos.x > p_rect.x && pos.y > p_rect.y && pos.x < (p_rect.x + item->GetSize().x) && pos.y < (p_rect.y + item->GetSize().y)) {
            if (item->m_tray_data.type == TrayType::NORMAL) {
                if (!m_ext_mapping_filatype_check && (item->m_ams_id == VIRTUAL_TRAY_MAIN_ID || item->m_ams_id == VIRTUAL_TRAY_DEPUTY_ID)) {
                    // Do nothing
                } else {
                    if(!is_match_material(item->m_tray_data.filament_type)) { return; }
                }
            }

            if (item->m_tray_data.type == TrayType::EMPTY && !m_mapping_from_multi_machines) return;
            if (m_show_type == ShowType::LEFT_AND_RIGHT_DYNAMIC && devPrinterUtil::IsVirtualSlot(item->m_ams_id)) return;

            if ((m_show_type == ShowType::LEFT && item->GetParent()->GetName() == "left") ||
                (m_show_type == ShowType::RIGHT && item->GetParent()->GetName() == "right") ||
                m_show_type == ShowType::LEFT_AND_RIGHT ||
                m_show_type == ShowType::LEFT_AND_RIGHT_DYNAMIC) {
                item->send_event(m_current_filament_id);
                Dismiss();
                break;
            }
        }
    }
    evt.Skip();
}

#ifdef  __APPLE__
void AmsMapingPopup::on_mouse_move(wxMouseEvent &evt)
{

    auto pos = ClientToScreen(evt.GetPosition());
    wxString tip_text;
    wxPoint tip_pos;
    for (MappingItem *item : m_mapping_item_list) {
        auto origin = item->ClientToScreen(wxPoint(0, 0));
        auto size   = item->GetSize();
        if (pos.x >= origin.x && pos.y >= origin.y && pos.x < origin.x + size.x && pos.y < origin.y + size.y)
        {
            auto *tip = item->GetToolTip();
            if (tip && !tip->GetTip().IsEmpty())
            {
                tip_text = tip->GetTip();
                tip_pos = wxPoint(origin.x, origin.y + size.y);
            }
            break;
        }
    }

    if (!tip_text.IsEmpty()) {
        if (!m_tip_popup)
        {
            m_tip_popup = new wxPopupWindow(this);
            m_tip_popup->SetBackgroundColour(wxColour(255, 255, 255));
            auto *sizer = new wxBoxSizer(wxVERTICAL);
            // Use the project Label with pixel-width, CJK-aware wrapping instead
            // of a bare wxStaticText, whose Wrap() only breaks at spaces and so
            // leaves space-less Chinese text on a single overflowing line.
            m_tip_label = new Label(m_tip_popup, wxEmptyString, LB_AUTO_WRAP);
            m_tip_label->SetMaxSize(wxSize(FromDIP(400), -1));
            m_tip_label->SetForegroundColour(*wxBLACK);
            sizer->Add(m_tip_label, 0, wxALL, 4);
            m_tip_popup->SetSizer(sizer);
            m_tip_popup->Bind(wxEVT_IDLE, [this](wxIdleEvent &) {
                if (!IsShown() && m_tip_popup) {
                    m_tip_popup->Destroy();
                    m_tip_popup = nullptr;
                    m_tip_label = nullptr;
                }
            });
        }

        if (m_tip_label->GetLabel() != tip_text) {
            m_tip_label->SetLabel(tip_text);
            m_tip_label->Wrap(FromDIP(400));
            m_tip_popup->Layout();
            m_tip_popup->Fit();
        }
        m_tip_popup->SetPosition(wxPoint(tip_pos.x, tip_pos.y));
        if (!m_tip_popup->IsShown())
            m_tip_popup->Show();
    } else {
        if (m_tip_popup && m_tip_popup->IsShown()) m_tip_popup->Hide();
    }
}
#endif


void AmsMapingPopup::OnDismiss()
{
#ifdef __APPLE__
    if (m_tip_popup && m_tip_popup->IsShown ())
        m_tip_popup->Hide();
#endif
}

bool AmsMapingPopup::ProcessLeftDown(wxMouseEvent &event)
{
    return PopupWindow::ProcessLeftDown(event);
}

void AmsMapingPopup::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#009688")), FromDIP(2))); // Orca: colorful accent border for separation (dark-mode aware)
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, 0);
}

void AmsMapingPopup::OnNozzleMappingSelected(wxCommandEvent& evt)
{
    if (auto ptr = m_rack.lock()) {
        MachineObject* obj = ptr->GetNozzleSystem()->GetOwner();
        obj->get_nozzle_mapping_result()->SetManualNozzleMappingByFila(m_current_filament_id, m_rack_nozzle_select->GetSelectedNozzlePosID());
        update_flush_waste(obj);
    }

    evt.Skip();
    Dismiss();
}

void AmsMapingPopup::update_flush_waste(MachineObject* obj)
{
    if (!obj) {
        m_flush_warning_panel->Hide();
        return;
    };

    float flush_waste_base = obj->get_nozzle_mapping_result()->GetFlushWeightBase();
    float flush_waste_current = obj->get_nozzle_mapping_result()->GetFlushWeightCurrent();
    if ((flush_waste_base != -1) && (flush_waste_current != -1) && flush_waste_current > flush_waste_base){
        m_flush_warning_panel->SetLabel(wxString::Format(_L("Printing with the current nozzle may produce an extra %0.2f g of waste."), flush_waste_current - flush_waste_base));
        m_flush_warning_panel->Show();
    } else {
        m_flush_warning_panel->Hide();
    }
}


 MappingItem::MappingItem(wxWindow *parent)
 : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    m_transparent_mapping_item = ScalableBitmap(this, "transparent_mapping_item", FromDIP(60));
    mapping_item_checked = ScalableBitmap(this, "mapping_item_checked", FromDIP(20));
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    Bind(wxEVT_PAINT, &MappingItem::paintEvent, this);
}

 MappingItem::~MappingItem()
{
}


void MappingItem::send_event(int fliament_id)
{
    wxCommandEvent event(EVT_SET_FINISH_MAPPING);
    event.SetInt(m_tray_data.id);

    wxString param = wxString::Format("%d|%d|%d|%d|%s|%d|%d|%d", m_coloul.Red(), m_coloul.Green(), m_coloul.Blue(), m_coloul.Alpha(), m_tray_index, fliament_id,
       m_tray_data.ams_id, m_tray_data.slot_id);
    event.SetString(param);

    if (send_win) {
        event.SetEventObject(send_win);
        wxPostEvent(send_win, event);
    }
}

 void MappingItem::msw_rescale()
 {
     m_transparent_mapping_item.msw_rescale();
     mapping_item_checked.msw_rescale();
     Refresh();
 }

void MappingItem::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    render(dc);

    // PrepareDC(buffdc);
    // PrepareDC(dc);
}

static void _DrawRemainArea(const MappingItem *item, const TrayData &dd, bool support_remain_dect, wxDC &dc)
{
    int to_paint_remain = dd.remain;

     /*paint invalid data as 100*/
    if (!support_remain_dect) { to_paint_remain = 100;}
    if (0 > to_paint_remain || to_paint_remain > 100) { to_paint_remain = 100; }

    wxSize size          = item->GetSize();
    int x_margin         = item->FromDIP(4);
    int y_margin         = item->FromDIP(2);
    int full_range_width = size.x;

    /*range background*/
    dc.SetPen(wxColour("#E4E4E4"));
    dc.SetBrush(wxColour("#E4E4E4"));
    int bg_height = item->FromDIP(6);
    int bg_width  = full_range_width - (2 * x_margin);
    dc.DrawRoundedRectangle(x_margin, y_margin, bg_width, bg_height, item->FromDIP(2));

    /*remain fill*/
    if (!dd.name.empty())
    {
        dc.SetPen(dd.colour);
        dc.SetBrush(dd.colour);
        int border        = item->FromDIP(1);
        int remain_width  = (bg_width - (2 * border)) * to_paint_remain / 100;
        int remain_height = bg_height - (2 * border);
        dc.DrawRoundedRectangle(x_margin + border, y_margin + border, remain_width, remain_height, item->FromDIP(2));
    }
}

void MappingItem::render(wxDC &dc)
{
      wxSize     size = GetSize();

#ifdef __WXMSW__
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({0, 0}, size, &dc, {0, 0});

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif

    /*remain*/
    auto top = 0;
    if (m_to_paint_remain)
    {
        _DrawRemainArea(this, m_tray_data, m_support_remain_detect, dc);
        top += get_remain_area_height();
    }

    // checked
    if (m_checked)
    {
        dc.DrawBitmap(mapping_item_checked.bmp(), size.x - mapping_item_checked.GetBmpWidth() - FromDIP(4), top);
    }
    top += 0.5 * mapping_item_checked.GetBmpHeight();

    // materials name
    dc.SetFont(::Label::Head_12);

    auto txt_colour = m_coloul.GetLuminance() < 0.6 ? *wxWHITE : wxColour(0x26, 0x2E, 0x30);

    if (m_unmatch || m_name == "-" ) { txt_colour = wxColour(0xCE, 0xCE, 0xCE); }
   // txt_colour      = m_unmatch ? wxColour(0xCE, 0xCE, 0xCE) : txt_colour;

    if (m_coloul.Alpha() == 0)
        txt_colour = wxColour(0x26, 0x2E, 0x30);
    if (!IsEnabled())
        txt_colour = wxColour(0xCE, 0xCE, 0xCE);

    dc.SetTextForeground(txt_colour);
    //show a1
    auto txt_size = dc.GetTextExtent(m_tray_index);
    top += FromDIP(2);
    dc.DrawText(m_tray_index, wxPoint((GetSize().x - txt_size.x) / 2, top));

    //show materials name
    top += txt_size.y + FromDIP(2);
    m_name.size() > 4 ? dc.SetFont(::Label::Body_9) : dc.SetFont(::Label::Body_12);
    if(m_name.size() > 5){
        m_name = m_name.substr(0,5) + "...";
    }
    txt_size = dc.GetTextExtent(m_name);
    dc.DrawText(m_name, wxPoint((GetSize().x - txt_size.x) / 2, top));
}

void MappingItem::set_data(const wxString &tag_name, wxColour colour, wxString name, bool remain_dect, TrayData data, bool unmatch, std::optional<wxString> tooltip_opt)
{
    m_unmatch = unmatch;
    m_tray_data = data;

    if (m_coloul != colour || m_name != name || (m_support_remain_detect != remain_dect)) {
        m_coloul = colour;
        m_name   = name;
        m_support_remain_detect = remain_dect;
        m_to_paint_remain       = (m_tray_data.ams_id != VIRTUAL_TRAY_MAIN_ID && m_tray_data.ams_id != VIRTUAL_TRAY_DEPUTY_ID);
        Refresh();
    }

    if (tooltip_opt.has_value())
    {
        SetToolTip(tooltip_opt.value());
    }
    else if (m_unmatch || (m_name == "-"))
    {
        if (m_unmatch) {
            bool is_external_spool = (m_tray_data.ams_id == VIRTUAL_TRAY_MAIN_ID || m_tray_data.ams_id == VIRTUAL_TRAY_DEPUTY_ID);

            if (!m_name.IsEmpty() && (m_name != "-")) {
                wxString msg;
                if (is_external_spool) {
                    msg = wxString::Format(_L("Tips: the filament type(%s) does not match with the filament type(%s) in the slicing file. "
                                              "If you want to use this slot, you can install %s instead of %s and change slot information on the 'Device' page."),
                                               m_name, tag_name, tag_name, m_name);
                } else {
                    msg = wxString::Format(_L("Cannot select: the filament type(%s) does not match with the filament type(%s) in the slicing file. "
                                              "If you want to use this slot, you can install %s instead of %s and change slot information on the 'Device' page."),
                                               m_name, tag_name, tag_name, m_name);
                }
                SetToolTip(msg);
            } else {
                const wxString &msg = wxString::Format(_L("Cannot select: the slot is empty or undefined. If you want to use this slot, you can install %s and change slot information on the 'Device' page."), tag_name);
                SetToolTip(msg);
            }

        } else {
            SetToolTip(_L("Cannot select: No filament loaded in current slot."));
        }
    }
    else
    {
        SetToolTip(wxEmptyString);
    }
}

void MappingItem::set_checked(bool checked)
{
    if (m_checked != checked)
    {
        m_checked = checked;
        Refresh();
    }
}

void MappingItem::doRender(wxDC &dc)
{
    wxSize size = GetSize();
    wxColour color = m_coloul;
    change_the_opacity(color);

    if (!IsThisEnabled()) { color = wxColour(0xEE, 0xEE, 0xEE); }
    dc.SetPen(color);
    dc.SetBrush(wxBrush(color));


    //draw a rectangle based on the material color, single color or muti color processing
    if (m_tray_data.material_cols.size() > 1 && !m_unmatch) {
        int left = 0;
        int gwidth = std::round(MAPPING_ITEM_REAL_SIZE.x / (m_tray_data.material_cols.size() - 1));
        //gradient
        if (m_tray_data.ctype == 0) {
            for (int i = 0; i < m_tray_data.material_cols.size() - 1; i++) {
                auto rect = wxRect(left, (size.y - MAPPING_ITEM_REAL_SIZE.y) / 2 + get_remain_area_height(), MAPPING_ITEM_REAL_SIZE.x, MAPPING_ITEM_REAL_SIZE.y);
                fill_gradient_rect_east(dc, rect, m_tray_data.material_cols[i], m_tray_data.material_cols[i + 1]);
                left += gwidth;
            }
        }
        else {
            int cols_size = m_tray_data.material_cols.size();
            for (int i = 0; i < cols_size; i++) {
                dc.SetBrush(wxBrush(m_tray_data.material_cols[i]));
                float x = (float)MAPPING_ITEM_REAL_SIZE.x * i / cols_size;
                dc.DrawRectangle(x, (size.y - MAPPING_ITEM_REAL_SIZE.y) / 2 + get_remain_area_height(), (float) MAPPING_ITEM_REAL_SIZE.x / cols_size, MAPPING_ITEM_REAL_SIZE.y);
            }
        }
    }
    else if (color.Alpha() == 0) {
        dc.DrawBitmap(m_transparent_mapping_item.bmp(), 0, (size.y - MAPPING_ITEM_REAL_SIZE.y) / 2 + get_remain_area_height());
    }
    else {
        dc.DrawRectangle(0, (size.y - MAPPING_ITEM_REAL_SIZE.y) / 2 + get_remain_area_height(), MAPPING_ITEM_REAL_SIZE.x, MAPPING_ITEM_REAL_SIZE.y);
    }

    wxColour side_colour = wxColour("#E4E4E4");

    dc.SetPen(side_colour);
    dc.SetBrush(wxBrush(side_colour));
    dc.DrawRectangle(0,                   get_remain_area_height(), FromDIP(4), size.y);
    dc.DrawRectangle(size.x - FromDIP(4), get_remain_area_height(), FromDIP(4), size.y);
}

int MappingItem::get_remain_area_height() const
{
    if (m_to_paint_remain) { return FromDIP(10);}
    return 0;
}


AmsMapingTipPopup::AmsMapingTipPopup(wxWindow *parent)
    :PopupWindow(parent, wxBORDER_NONE)
{
    SetBackgroundColour(*wxWHITE);
    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_sizer_main->Add(0, 0, 1, wxTOP, FromDIP(28));

    wxBoxSizer *m_sizer_body = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_body->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(20));

    m_panel_enable_ams = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(220), -1), wxTAB_TRAVERSAL);
    m_panel_enable_ams->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *sizer_enable_ams = new wxBoxSizer(wxVERTICAL);

    m_title_enable_ams = new wxStaticText(m_panel_enable_ams, wxID_ANY, _L("Enable AMS"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_enable_ams->SetForegroundColour(*wxBLACK);
    m_title_enable_ams->SetBackgroundColour(*wxWHITE);
    m_title_enable_ams->Wrap(-1);
    sizer_enable_ams->Add(m_title_enable_ams, 0, 0, 0);

    m_tip_enable_ams = new wxStaticText(m_panel_enable_ams, wxID_ANY, _L("Print with filament in the AMS"), wxDefaultPosition, wxDefaultSize, 0); // Orca: kept string (existing translation)
    m_tip_enable_ams->SetMinSize(wxSize(FromDIP(200), FromDIP(50)));
    m_tip_enable_ams->Wrap(FromDIP(200));
    m_tip_enable_ams->SetForegroundColour(*wxBLACK);
    m_tip_enable_ams->SetBackgroundColour(*wxWHITE);
    sizer_enable_ams->Add(m_tip_enable_ams, 0, wxTOP, 8);

    wxBoxSizer *sizer_enable_ams_img;
    sizer_enable_ams_img = new wxBoxSizer(wxVERTICAL);

    auto img_enable_ams = new wxStaticBitmap(m_panel_enable_ams, wxID_ANY, create_scaled_bitmap("monitor_upgrade_ams", this, 108), wxDefaultPosition,
                                             wxSize(FromDIP(118), FromDIP(108)), 0);
    sizer_enable_ams_img->Add(img_enable_ams, 0, wxALIGN_CENTER_HORIZONTAL, 0);

    sizer_enable_ams->Add(sizer_enable_ams_img, 1, wxEXPAND | wxTOP, FromDIP(20));

    m_panel_enable_ams->SetSizer(sizer_enable_ams);
    m_panel_enable_ams->Layout();
    m_sizer_body->Add(m_panel_enable_ams, 0, 0, 0);

    m_split_lines = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(1, FromDIP(150)), wxTAB_TRAVERSAL);
    m_split_lines->SetBackgroundColour(wxColour(238, 238, 238));

    m_sizer_body->Add(m_split_lines, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(10));

    m_panel_disable_ams = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(220), -1), wxTAB_TRAVERSAL);
    m_panel_disable_ams->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *sizer_disable_ams;
    sizer_disable_ams = new wxBoxSizer(wxVERTICAL);

    m_title_disable_ams = new wxStaticText(m_panel_disable_ams, wxID_ANY, _L("Disable AMS"), wxDefaultPosition, wxDefaultSize, 0);
    m_title_disable_ams->SetBackgroundColour(*wxWHITE);
    m_title_disable_ams->SetForegroundColour(*wxBLACK);
    m_title_disable_ams->Wrap(-1);
    sizer_disable_ams->Add(m_title_disable_ams, 0, 0, 0);

    m_tip_disable_ams = new wxStaticText(m_panel_disable_ams, wxID_ANY, _L("Print with filament on external spool"), wxDefaultPosition, wxDefaultSize, 0); // Orca: kept string (existing translation)
    m_tip_disable_ams->SetMinSize(wxSize(FromDIP(200), FromDIP(50)));
    m_tip_disable_ams->Wrap(FromDIP(200));
    m_tip_disable_ams->SetForegroundColour(*wxBLACK);
    m_tip_disable_ams->SetBackgroundColour(*wxWHITE);
    sizer_disable_ams->Add(m_tip_disable_ams, 0, wxTOP, FromDIP(8));

    wxBoxSizer *sizer_disable_ams_img;
    sizer_disable_ams_img = new wxBoxSizer(wxVERTICAL);

    auto img_disable_ams = new wxStaticBitmap(m_panel_disable_ams, wxID_ANY, create_scaled_bitmap("disable_ams_demo_icon", this, 95), wxDefaultPosition,
                                              wxSize(FromDIP(95), FromDIP(109)), 0);
    sizer_disable_ams_img->Add(img_disable_ams, 0, wxALIGN_CENTER_HORIZONTAL, 0);

    sizer_disable_ams->Add(sizer_disable_ams_img, 1, wxEXPAND | wxTOP, FromDIP(20));

    m_panel_disable_ams->SetSizer(sizer_disable_ams);
    m_panel_disable_ams->Layout();
    m_sizer_body->Add(m_panel_disable_ams, 0, 0, 0);

    m_sizer_body->Add(0, 0, 0, wxEXPAND | wxRIGHT, FromDIP(20));

    m_sizer_main->Add(m_sizer_body, 0, wxEXPAND, 0);

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(28));

    this->SetSizer(m_sizer_main);
    this->Layout();
    this->Fit();
    Bind(wxEVT_PAINT, &AmsMapingTipPopup::paintEvent, this);
}

void AmsMapingTipPopup::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    dc.SetPen(wxColour(0xAC, 0xAC, 0xAC));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, 0);
}

void AmsMapingTipPopup::OnDismiss() {}

bool AmsMapingTipPopup::ProcessLeftDown(wxMouseEvent &event) {
    return PopupWindow::ProcessLeftDown(event); }


AmsHumidityTipPopup::AmsHumidityTipPopup(wxWindow* parent)
    :PopupWindow(parent, wxBORDER_NONE)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    close_img = ScalableBitmap(this, "hum_popup_close", 24);

    m_staticText = new Label(this, _L("Current AMS humidity"));
    m_staticText->SetFont(::Label::Head_24);

    humidity_level_list = new AmsHumidityLevelList(this);
    curr_humidity_img = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("hum_level1_light", this, 132), wxDefaultPosition, wxSize(FromDIP(132), FromDIP(132)), 0);

    // Orca: kept the grammatically-corrected desiccant note (existing translation)
    m_staticText_note = new Label(this, _L("Please change the desiccant when it is too wet. The indicator may not represent accurately in following cases: when the lid is open or the desiccant pack is changed. It takes a few hours to absorb the moisture, and low temperatures also slow down the process."));
    m_staticText_note->SetMinSize(wxSize(FromDIP(680), -1));
    m_staticText_note->SetMaxSize(wxSize(FromDIP(680), -1));
    m_staticText_note->Wrap(FromDIP(680));


    Bind(wxEVT_LEFT_UP, [this](auto& e) {

        auto rect = ClientToScreen(wxPoint(0, 0));

        auto close_left     = rect.x + GetSize().x - close_img.GetBmpWidth() - FromDIP(38);
        auto close_right    = close_left + close_img.GetBmpWidth();
        auto close_top      = rect.y + FromDIP(24);
        auto close_bottom   = close_top + close_img.GetBmpHeight();

        auto mouse_pos = ClientToScreen(e.GetPosition());
        if (mouse_pos.x > close_left
            && mouse_pos.y > close_top
            && mouse_pos.x < close_right
            && mouse_pos.y < close_bottom
            )
        {
            Dismiss();
        }
        });

    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(24));
    main_sizer->Add(m_staticText, 0, wxALIGN_CENTER, 0);
    main_sizer->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(28));
    main_sizer->Add(curr_humidity_img, 0, wxALIGN_CENTER|wxLEFT|wxRIGHT, FromDIP(35));
    main_sizer->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(15));
    main_sizer->Add(humidity_level_list, 0, wxALIGN_CENTER|wxLEFT|wxRIGHT, FromDIP(35));
    main_sizer->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(6));
    main_sizer->Add(m_staticText_note, 0, wxALIGN_CENTER, 0);
    main_sizer->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(5));
    main_sizer->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(25));
    SetSizer(main_sizer);
    Layout();
    Fit();

    Bind(wxEVT_PAINT, &AmsHumidityTipPopup::paintEvent, this);
    wxGetApp().UpdateDarkUIWin(this);
}

void AmsHumidityTipPopup::set_humidity_level(int level)
{
    if (0 < level && level < 6)
    {
        current_humidity_level = level;
        std::string mode_string = wxGetApp().dark_mode() ? "_dark" : "_light";
        curr_humidity_img->SetBitmap(create_scaled_bitmap("hum_level" + std::to_string(current_humidity_level) + mode_string, this, 132));
        curr_humidity_img->Refresh();
        curr_humidity_img->Update();
    }
}

void AmsHumidityTipPopup::msw_rescale()
{
    // close image
    close_img.msw_rescale();

    // current humidity level image
    if (0 < current_humidity_level && current_humidity_level < 6)
    {
        std::string mode_string = wxGetApp().dark_mode() ? "_dark" : "_light";
        curr_humidity_img->SetBitmap(create_scaled_bitmap("hum_level" + std::to_string(current_humidity_level) + mode_string, this, 132));
    }

    // the list
    humidity_level_list->msw_rescale();

    // refresh
    Refresh();
}


void AmsHumidityTipPopup::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void AmsHumidityTipPopup::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void AmsHumidityTipPopup::doRender(wxDC& dc)
{
    //close
    dc.DrawBitmap(close_img.bmp(), GetSize().x - close_img.GetBmpWidth() - FromDIP(38), FromDIP(24));

    //background
    dc.SetPen(wxColour(0xAC, 0xAC, 0xAC));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, 0);
}



AmsTutorialPopup::AmsTutorialPopup(wxWindow* parent)
:PopupWindow(parent, wxBORDER_NONE)
{
    Bind(wxEVT_PAINT, &AmsTutorialPopup::paintEvent, this);
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* sizer_main;
    sizer_main = new wxBoxSizer(wxVERTICAL);

    text_title = new Label(this, Label::Head_14, _L("Configure which AMS slot should be used for a filament used in the print job.")); // Orca: kept string (existing translation)
    text_title->SetSize(wxSize(FromDIP(350), -1));
    text_title->Wrap(FromDIP(350));
    sizer_main->Add(text_title, 0, wxALIGN_CENTER | wxTOP, 18);


    sizer_main->Add(0, 0, 0, wxTOP, 30);

    wxBoxSizer* sizer_top;
    sizer_top = new wxBoxSizer(wxHORIZONTAL);

    img_top = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("ams_item_examples", this, 30), wxDefaultPosition, wxSize(FromDIP(50), FromDIP(30)), 0);
    sizer_top->Add(img_top, 0, wxALIGN_CENTER, 0);


    sizer_top->Add(0, 0, 0, wxLEFT, 10);

    wxBoxSizer* sizer_top_tips = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* sizer_tip_top = new wxBoxSizer(wxHORIZONTAL);

    arrows_top = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("ams_arrow", this, 8), wxDefaultPosition, wxSize(FromDIP(24), FromDIP(8)), 0);
    sizer_tip_top->Add(arrows_top, 0, wxALIGN_CENTER, 0);

    tip_top = new wxStaticText(this, wxID_ANY, _L("Filament used in this print job"), wxDefaultPosition, wxDefaultSize, 0);
    tip_top->SetForegroundColour(wxColour("#686868"));

    sizer_tip_top->Add(tip_top, 0, wxALL, 0);


    sizer_top_tips->Add(sizer_tip_top, 0, wxEXPAND, 0);


    sizer_top_tips->Add(0, 0, 0, wxTOP, 6);

    wxBoxSizer* sizer_tip_bottom = new wxBoxSizer(wxHORIZONTAL);

    arrows_bottom = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("ams_arrow", this, 8), wxDefaultPosition, wxSize(FromDIP(24), FromDIP(8)), 0);
    tip_bottom = new wxStaticText(this, wxID_ANY, _L("AMS slot used for this filament"), wxDefaultPosition, wxDefaultSize, 0);
    tip_bottom->SetForegroundColour(wxColour("#686868"));


    sizer_tip_bottom->Add(arrows_bottom, 0, wxALIGN_CENTER, 0);
    sizer_tip_bottom->Add(tip_bottom, 0, wxALL, 0);


    sizer_top_tips->Add(sizer_tip_bottom, 0, wxEXPAND, 0);


    sizer_top->Add(sizer_top_tips, 0, wxALIGN_CENTER, 0);




    wxBoxSizer* sizer_middle = new wxBoxSizer(wxHORIZONTAL);

    img_middle= new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("ams_item_examples", this, 30), wxDefaultPosition, wxSize(FromDIP(50), FromDIP(30)), 0);
    sizer_middle->Add(img_middle, 0, wxALIGN_CENTER, 0);

    tip_middle = new wxStaticText(this, wxID_ANY, _L("Click to select AMS slot manually"), wxDefaultPosition, wxDefaultSize, 0);
    tip_middle->SetForegroundColour(wxColour("#686868"));
    sizer_middle->Add(0, 0, 0,wxLEFT, 15);
    sizer_middle->Add(tip_middle, 0, wxALIGN_CENTER, 0);


    sizer_main->Add(sizer_top, 0, wxLEFT, 40);
    sizer_main->Add(0, 0, 0, wxTOP, 10);
    sizer_main->Add(sizer_middle, 0, wxLEFT, 40);
    sizer_main->Add(0, 0, 0, wxTOP, 10);


    img_botton = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("ams_mapping_examples", this, 87), wxDefaultPosition, wxDefaultSize, 0);
    sizer_main->Add(img_botton, 0, wxLEFT | wxRIGHT, 40);
    sizer_main->Add(0, 0, 0, wxTOP, 12);

    SetSizer(sizer_main);
    Layout();
    Fit();
}

void AmsTutorialPopup::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    dc.SetPen(wxColour(0xAC, 0xAC, 0xAC));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, 0);
}

void AmsTutorialPopup::OnDismiss() {}

bool AmsTutorialPopup::ProcessLeftDown(wxMouseEvent& event) {
    return PopupWindow::ProcessLeftDown(event);
}


AmsIntroducePopup::AmsIntroducePopup(wxWindow* parent)
:PopupWindow(parent, wxBORDER_NONE)
{
    Bind(wxEVT_PAINT, &AmsIntroducePopup::paintEvent, this);
    SetBackgroundColour(*wxWHITE);

    SetMinSize(wxSize(FromDIP(200), FromDIP(200)));
    SetMaxSize(wxSize(FromDIP(200), FromDIP(200)));

    wxBoxSizer* bSizer4 = new wxBoxSizer(wxVERTICAL);

    m_staticText_top = new Label(this, _L("Do not Enable AMS"));
    m_staticText_top->SetFont(::Label::Head_13);
    // m_staticText_top->SetForegroundColour(wxColour("#323A3D"));
    m_staticText_top->Wrap(-1);
    bSizer4->Add(m_staticText_top, 0, wxALL, 5);

    m_staticText_bottom =  new Label(this, _L("Print using filament on external spool.")); // Orca: kept string (existing translation)
    m_staticText_bottom->Wrap(-1);
    m_staticText_bottom->SetFont(::Label::Body_13);
    m_staticText_bottom->SetForegroundColour(wxColour("#6B6B6B"));
    bSizer4->Add(m_staticText_bottom, 0, wxALL, 5);

    wxBoxSizer* bSizer5;
    bSizer5 = new wxBoxSizer(wxHORIZONTAL);

    m_img_enable_ams = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("monitor_upgrade_ams", this, FromDIP(140)), wxDefaultPosition, wxDefaultSize, 0);
    m_img_disable_ams = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("disable_ams_demo_icon", this, FromDIP(110)), wxDefaultPosition, wxDefaultSize, 0);

    m_img_enable_ams->SetMinSize(wxSize(FromDIP(96), FromDIP(110)));
    m_img_disable_ams->SetMinSize(wxSize(FromDIP(96), FromDIP(110)));

    bSizer5->Add(m_img_enable_ams, 0, wxALIGN_CENTER, 0);
    bSizer5->Add(m_img_disable_ams, 0, wxALIGN_CENTER, 0);

    m_img_disable_ams->Hide();
    m_img_disable_ams->Hide();


    bSizer4->Add(bSizer5, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(1));


    SetSizer(bSizer4);
    Layout();
    Fit();

    wxGetApp().UpdateDarkUIWin(this);
}

void AmsIntroducePopup::set_mode(bool enable_ams)
{
    if (enable_ams) {
        m_staticText_top->SetLabelText(_L("Enable AMS"));
        m_staticText_bottom->SetLabelText(_L("Print with filament in AMS")); // Orca: kept string (existing translation)
        m_img_enable_ams->Show();
        m_img_disable_ams->Hide();
    }
    else {
        m_staticText_top->SetLabelText(_L("Do not Enable AMS"));
        m_staticText_bottom->SetLabelText(_L("Print with filament on external spool")); // Orca: kept string (existing translation)
        m_staticText_bottom->SetMinSize(wxSize(FromDIP(180), -1));
        m_staticText_bottom->Wrap(FromDIP(180));
        m_img_enable_ams->Hide();
        m_img_disable_ams->Show();
    }
    Layout();
    Fit();
}

void AmsIntroducePopup::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    dc.SetPen(wxColour(0xAC, 0xAC, 0xAC));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, 0);
}


void AmsIntroducePopup::OnDismiss() {}

bool AmsIntroducePopup::ProcessLeftDown(wxMouseEvent& event) {
    return PopupWindow::ProcessLeftDown(event);
}


MappingContainer::MappingContainer(wxWindow *parent, const wxString &ams_type, int slots_num)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    Bind(wxEVT_PAINT, &MappingContainer::paintEvent, this);

    m_ams_type  = ams_type;
    m_slots_num = slots_num;
    if (slots_num == 1)
    {
        ams_mapping_item_container = create_scaled_bitmap("ams_mapping_container_1", this, 82);
        SetMinSize(wxSize(FromDIP(74), FromDIP(82)));
        SetMaxSize(wxSize(FromDIP(74), FromDIP(82)));
    }
    else
    {
        ams_mapping_item_container = create_scaled_bitmap("ams_mapping_container_4", this, 82);
        SetMinSize(wxSize(FromDIP(230), FromDIP(82)));
        SetMaxSize(wxSize(FromDIP(230), FromDIP(82)));
    }
}

MappingContainer::~MappingContainer()
{
}


void MappingContainer::msw_rescale()
{
    if (m_slots_num == 1) {
        ams_mapping_item_container = create_scaled_bitmap("ams_mapping_container_1", this, 82);
    } else {
        ams_mapping_item_container = create_scaled_bitmap("ams_mapping_container_4", this, 82);
    }

    Refresh();
}

void MappingContainer::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void MappingContainer::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void MappingContainer::doRender(wxDC& dc)
{
    dc.DrawBitmap(ams_mapping_item_container, 0, 0);

    dc.SetFont(::Label::Head_11);
    auto size   = GetSize();
    auto extent = dc.GetTextExtent(m_ams_type);
    dc.SetTextForeground(wxColour("#F1F1F1"));
    dc.DrawText(m_ams_type, FromDIP(10), size.GetHeight() - extent.GetHeight());
}

AmsReplaceMaterialDialog::AmsReplaceMaterialDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Auto Refill"), wxDefaultPosition, wxDefaultSize, wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX)
{

#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
    SetBackgroundColour(*wxWHITE);
    create();
    wxGetApp().UpdateDlgDarkUI(this);
}

void AmsReplaceMaterialDialog::create()
{
    SetSize(wxSize(FromDIP(445), -1));
    SetMinSize(wxSize(FromDIP(445), -1));
    SetMaxSize(wxSize(FromDIP(445), -1));

    // set icon for dialog
    std::string icon_path = (boost::format("%1%/images/OrcaSlicerTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    auto m_top_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_top_line->SetBackgroundColour(wxColour(166, 169, 170));
    m_main_sizer->Add(m_top_line, 0, wxEXPAND, 0);

    m_nozzle_btn_panel = new SwitchBoard(this, _L("Left"), _L("Right"), wxSize(FromDIP(126), FromDIP(26)));
    m_nozzle_btn_panel->Hide();
    m_nozzle_btn_panel->Connect(wxCUSTOMEVT_SWITCH_POS, wxCommandEventHandler(AmsReplaceMaterialDialog::on_nozzle_selected), NULL, this);

    label_txt = new Label(this, _L("When the current material run out, the printer will continue to print in the following order."));
    label_txt->SetFont(Label::Body_13);
    label_txt->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3C")));
    label_txt->SetMinSize(wxSize(FromDIP(380), -1));
    label_txt->SetMaxSize(wxSize(FromDIP(380), -1));
    label_txt->Wrap(FromDIP(380));

    identical_filament = new Label(this, _L("Identical filament: same brand, type and color.")); // Orca: kept trailing period (existing translation)
    identical_filament->SetFont(Label::Body_13);
    identical_filament->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#009688"))); // Orca: accent teal (not brand green)

    m_scrollview_groups = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    m_scrollview_groups->SetScrollRate(5, 5);
    //m_scrollview_groups->SetMinSize(wxSize(400, 400));
    //m_scrollview_groups->SetMaxSize(wxSize(400, 400));
    m_scrollview_sizer = new wxBoxSizer(wxVERTICAL);

    m_groups_sizer = new wxWrapSizer( wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS );

    m_scrollview_sizer->Add( m_groups_sizer, 0, wxALIGN_CENTER, 0 );
    m_scrollview_groups->SetSizer(m_scrollview_sizer);
    m_scrollview_groups->Layout();



    auto m_button_sizer = new wxBoxSizer(wxHORIZONTAL);
    StateColor btn_bg_white(std::pair<wxColour, int>(AMS_CONTROL_DISABLE_COLOUR, StateColor::Disabled),
        std::pair<wxColour, int>(AMS_CONTROL_DISABLE_COLOUR, StateColor::Pressed),
        std::pair<wxColour, int>(AMS_CONTROL_DEF_BLOCK_BK_COLOUR, StateColor::Hovered),
        std::pair<wxColour, int>(AMS_CONTROL_WHITE_COLOUR, StateColor::Normal));

    StateColor btn_bd_white(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));

    StateColor btn_text_white(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));


    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed), std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)); // Orca: accent teal button (not brand green)
    m_button_sizer->Add( 0, 0, 1, wxEXPAND, 0 );

    m_main_sizer->Add(0,0,0, wxTOP, FromDIP(12));
    m_main_sizer->Add(m_nozzle_btn_panel,0, wxALIGN_CENTER_HORIZONTAL, FromDIP(30));
    m_main_sizer->Add(0,0,0, wxTOP, FromDIP(12));
    m_main_sizer->Add(label_txt,0, wxLEFT, FromDIP(30));
    m_main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));
    m_main_sizer->Add(identical_filament, 0, wxLEFT, FromDIP(30));
    m_main_sizer->Add(0,0,0, wxTOP, FromDIP(16));
    m_main_sizer->Add(m_scrollview_groups, 1, wxALIGN_CENTER, 0);
    m_main_sizer->Add(0,0,0, wxTOP, FromDIP(20));
    m_main_sizer->Add(m_button_sizer,0,wxALIGN_CENTER, FromDIP(16));
    m_main_sizer->Add(0,0,0, wxTOP, FromDIP(20));


    CenterOnParent();
    SetSizer(m_main_sizer);
    Layout();
    Fit();
}

void AmsReplaceMaterialDialog::update_machine_obj(MachineObject* obj)
{
    if (obj)
    {
        m_obj = obj;
        if (obj->GetExtderSystem()->GetTotalExtderCount() > 1 && !obj->GetFilaSwitch()->IsReady())
        {
            // Orca: SwitchBoard has no SetLabels(a,b); set its public left/right labels (left=deputy, right=main).
            m_nozzle_btn_panel->leftLabel =
                _L(DevPrinterConfigUtil::get_toolhead_display_name(obj->printer_type, DEPUTY_EXTRUDER_ID, ToolHeadComponent::Extruder, ToolHeadNameCase::TitleCase, true));
            m_nozzle_btn_panel->rightLabel =
                _L(DevPrinterConfigUtil::get_toolhead_display_name(obj->printer_type, MAIN_EXTRUDER_ID, ToolHeadComponent::Extruder, ToolHeadNameCase::TitleCase, true));
            m_nozzle_btn_panel->updateState("right");
            m_nozzle_btn_panel->Show();
        }
        else
        {
            m_nozzle_btn_panel->Hide();
        }

        update_to_nozzle(MAIN_EXTRUDER_ID);
    }
}

AmsRMGroup* AmsReplaceMaterialDialog::create_backup_group(wxString gname, std::map<std::string, wxColour> group_info, wxString material)
{
    auto grp = new AmsRMGroup(m_scrollview_groups, group_info, material, gname);
    return grp;
}

void AmsReplaceMaterialDialog::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    dc.SetPen(StateColor::darkModeColorFor(wxColour("#DBDBDB"))); // Orca: popup border color fixed for dark mode
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, 0);
}

void AmsReplaceMaterialDialog::on_dpi_changed(const wxRect& suggested_rect)
{

}


void  AmsReplaceMaterialDialog::update_to_nozzle(int nozzle_id)
{
    if (!m_obj)
    {
        return;
    }

    if (m_obj->GetExtderSystem()->GetTotalExtderCount() < nozzle_id)
    {
        return;
    }

    //update group
    int group_index = 0;
    m_groups_sizer->Clear(true);
    if (m_obj->is_support_filament_backup && m_obj->GetFilaSystem()->IsAutoRefillEnabled())
    {
        // traverse the amd list
        std::unordered_map<int, DevAmsTray*> id2tray;// tray id to tray
        try
        {
            for (const auto& ams_info : m_obj->GetFilaSystem()->GetAmsList())
            {
                for (auto& tray : ams_info.second->GetTrays()) {
                    try {
                        int ams_id  = std::stoi(ams_info.first);
                        int slot_id = std::stoi(tray.first);

                        // Orca: DevFilaSystem has no GetTrayIdByAmsSlotId wrapper; look the tray id up
                        // in the (tray_id -> {ams_id, slot_id}) index map directly.
                        int  tray_id       = -1;
                        auto tray_index_map = m_obj->GetFilaSystem()->GetTrayIndexMap();
                        auto tray_it        = std::find_if(tray_index_map.begin(), tray_index_map.end(),
                            [ams_id, slot_id](const auto& item) { return ams_id == item.second.first && slot_id == item.second.second; });
                        if (tray_it != tray_index_map.end()) { tray_id = tray_it->first; }
                        if (tray_id != -1) {
                            id2tray[tray_id] = tray.second;
                        }
                    } catch(...) {
                        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " invalid ams_id:" << ams_info.first << ", slot_id:" << tray.first;
                    }
                }
            }
        }
        catch (...) {}

        const auto& extder = m_obj->GetExtderSystem()->GetExtderById(nozzle_id);
        if (extder)
        {
            for (int filam : extder->GetFilamBackup())
            {
                std::map<std::string, wxColour> group_info;
                std::string    group_material;
                bool   is_in_tray = false;

                //get color & material
                const auto& trayid_group = DevExtder::GetBackupStatus(filam);
                for (const auto& elem : trayid_group)
                {
                    if (elem.second)
                    {
                        DevAmsTray* cur_tray = id2tray[elem.first];
                        if (!cur_tray)
                            continue;
                        auto tray_name = wxGetApp().transition_tridid(elem.first).ToStdString();
                        auto it = std::find(m_tray_used.begin(), m_tray_used.end(), tray_name);
                        if (it != m_tray_used.end())
                        {
                            is_in_tray = true;
                        }
                        group_info[tray_name] = DevAmsTray::decode_color(cur_tray->color);
                        group_material = cur_tray->get_display_filament_type();
                    }
                }

                if (group_info.size() < 2) /* do not show refill if there is one tray*/
                {
                    continue;
                }

                if (is_in_tray || m_tray_used.size() <= 0)
                {
                    m_groups_sizer->Add(create_backup_group(wxString::Format("%s%d", _L("Group"), group_index + 1), group_info, group_material), 0, wxALL, FromDIP(10));
                    group_index++;
                }
            }
        }
    }

    if (group_index > 0)
    {
        auto height = 0;
        if (group_index > 6)
        {
            height = FromDIP(550);
        }
        else
        {
            height = FromDIP(200) * (std::ceil(group_index / 2.0));
        }

        m_scrollview_groups->SetMinSize(wxSize(FromDIP(400), height));
        m_scrollview_groups->SetMaxSize(wxSize(FromDIP(400), height));
    }
    else
    {
        m_scrollview_groups->SetMinSize(wxSize(0, 0));
    }

    // update text
    if (group_index > 0)
    {
        label_txt->SetLabel(_L("When the current material runs out, the printer would use identical filament to continue printing."));
        label_txt->Wrap(FromDIP(380));
        identical_filament->Show();
    }
    else
    {
        if (!m_obj->is_support_filament_backup)
        {
            label_txt->SetLabel(_L("The printer does not currently support auto refill."));
        }
        else if (!m_obj->GetFilaSystem()->IsAutoRefillEnabled())
        {
            label_txt->SetLabelText(_L("AMS filament backup is not enabled; please enable it in the AMS settings.")); // Orca: kept semicolon (existing translation)
        }
        else
        {
            label_txt->SetLabelText(_L("When the current filament runs out, the printer will use identical filament to continue printing.\n"
                                       "*Identical filament: same brand, type and color."));
        }

        label_txt->Wrap(FromDIP(380));
        label_txt->Layout();
        identical_filament->Hide();
    }

    m_groups_sizer->Layout();
    m_scrollview_groups->Layout();

    Layout();
    Fit();
}

AmsRMGroup::AmsRMGroup(wxWindow* parent, std::map<std::string, wxColour> group_info, wxString mname, wxString group_index)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
    m_group_info.clear();
    m_group_info = group_info;
    m_material_name = mname;
    m_group_index   = group_index;

    wxWindow::Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    SetSize(wxSize(FromDIP(166), FromDIP(166)));
    SetMinSize(wxSize(FromDIP(166), FromDIP(166)));
    SetMaxSize(wxSize(FromDIP(166), FromDIP(166)));
    SetBackgroundColour(*wxWHITE);

    backup_current_use_white    =  ScalableBitmap(this, "backup_current_use1",8);
    backup_current_use_black    =  ScalableBitmap(this, "backup_current_use2", 8);
    bitmap_backup_tips_0        =  ScalableBitmap(this, "backup_tips_img", 90);
    bitmap_editable             = ScalableBitmap(this, "ams_editable", 14);
    bitmap_bg                   = ScalableBitmap(this, "back_up_ts_bk", 162);
    bitmap_editable_light       = ScalableBitmap(this, "ams_editable_light", 14);

    Bind(wxEVT_PAINT, &AmsRMGroup::paintEvent, this);
    Bind(wxEVT_LEFT_DOWN, &AmsRMGroup::on_mouse_move, this);
    wxGetApp().UpdateDarkUI(this);
}

double AmsRMGroup::GetAngle(wxPoint pointA, wxPoint pointB)
{
    double deltaX = pointA.x - pointB.x;
    double deltaY = pointA.y - pointB.y;
    double angle = atan2(deltaY, deltaX);

    angle = angle * 180.0 / M_PI;

    if (angle < 0)
        angle += 360.0;

    return angle;
}

void AmsRMGroup::on_mouse_move(wxMouseEvent& evt)
{
    wxSize     size = GetSize();
    auto mouseX = evt.GetPosition().x;
    auto mouseY = evt.GetPosition().y;

    auto click_angle = 360.0 - GetAngle(wxPoint(mouseX,mouseY), wxPoint(size.x / 2, size.x / 2));


    float ev_angle = 360.0 / m_group_info.size();
    float startAngle = 0.0;
    float endAngle = 0.0;

    for (auto iter = m_group_info.rbegin(); iter != m_group_info.rend(); ++iter) {
        std::string tray_name = iter->first;
        wxColour tray_color = iter->second;

        int x = size.x / 2;
        int y = size.y / 2;
        int radius = size.x / 2;
        endAngle += ev_angle;

        if (click_angle >= startAngle && click_angle < endAngle) {
            //to do
            set_index(tray_name);
            Refresh();
            return;
        }

        startAngle += ev_angle;
    }

    evt.Skip();
}

void AmsRMGroup::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void AmsRMGroup::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

wxPoint AmsRMGroup::CalculateEndpoint(const wxPoint& startPoint, int angle, int length)
{
    int endX = startPoint.x + length * cos(angle * M_PI / 180);
    int endY = startPoint.y + length * sin(angle * M_PI / 180);
    return wxPoint(endX, endY);
}

void AmsRMGroup::doRender(wxDC& dc)
{
    wxSize size = GetSize();

    float center_mask_radius = FromDIP(52);
    float selected_radius = FromDIP(53);

    float ev_angle = 360.0 / m_group_info.size();
    float startAngle = 0.0;
    float endAngle = 0.0;

    dc.DrawBitmap(bitmap_bg.bmp(), wxPoint((size.x - bitmap_bg.GetBmpSize().x) / 2, (size.y - bitmap_bg.GetBmpSize().y) / 2));

    for (auto iter = m_group_info.rbegin(); iter != m_group_info.rend(); ++iter) {
        std::string tray_name = iter->first;
        wxColour tray_color = iter->second;

        dc.SetPen(*wxTRANSPARENT_PEN);

      if (tray_color == *wxWHITE) dc.SetPen(wxPen(wxColour("#EEEEEE"), 2));
        dc.SetBrush(wxBrush(tray_color));

        int x = size.x / 2;
        int y = size.y / 2;
        int radius;
        if (wxGetApp().dark_mode())
            radius = size.x / 2 - int(size.x * 0.02);
        else
            radius = size.x / 2;
        endAngle += ev_angle;


        //draw body
        if (tray_color.Alpha() != 0) {
            dc.DrawEllipticArc(x - radius, y - radius, radius * 2, radius * 2, startAngle, endAngle);
            if (tray_color == *wxWHITE) dc.DrawEllipticArc(x - center_mask_radius, y - center_mask_radius, center_mask_radius * 2, center_mask_radius * 2, startAngle, endAngle);
        }

        //draw selected
        if (!m_selected_index.empty() && m_selected_index == tray_name) {
            dc.SetPen(wxPen(0xCECECE, 2));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawEllipticArc(x - radius, y - radius, radius * 2, radius * 2, startAngle, endAngle);
            dc.DrawEllipticArc(x - selected_radius, y - selected_radius,selected_radius * 2, selected_radius * 2,  startAngle, endAngle);
        }

        //...
        startAngle += ev_angle;
    }

    //draw text
    startAngle = 0.0;
    endAngle = 0.0;
    for (auto iter = m_group_info.rbegin(); iter != m_group_info.rend(); ++iter) {
        std::string tray_name = iter->first;
        wxColour tray_color = iter->second;

        int x = size.x / 2;
        int y = size.y / 2;
        float radius = size.x / 2 - 15.0;
        endAngle += ev_angle;

        float midAngle = (startAngle + endAngle) / 2;
        float x_center = size.x / 2 + radius * cos(midAngle * M_PI / 180.0);
        float y_center = size.y / 2 + radius * sin(midAngle * M_PI / 180.0);

        //draw tray
        dc.SetFont(::Label::Body_12);
        auto text_size = dc.GetTextExtent(tray_name);
        dc.SetTextForeground(tray_color.GetLuminance() < 0.6 ? *wxWHITE : wxColour("#262E30"));
        if (tray_color.Alpha() == 0) { dc.SetTextForeground(wxColour("#262E30")); }

        dc.DrawText(tray_name, x_center - text_size.x / 2, size.y - y_center - text_size.y / 2);

        //draw split line
        dc.SetPen(wxPen(*wxWHITE, 2));
        if (tray_color.Alpha() == 0) { dc.SetPen(wxPen(wxColour("#CECECE"), 2)); }
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        auto pos_sp_start = CalculateEndpoint(wxPoint(x, y), (360 - startAngle),  size.x / 2 - FromDIP(3));
        dc.DrawLine(wxPoint(x, y), pos_sp_start);

        //draw current
        //dc.DrawBitmap(backup_current_use_white.bmp(), x_center - text_size.x / 2 + FromDIP(3), size.y - y_center - text_size.y / 2 + FromDIP(11));
        //...
        startAngle += ev_angle;
    }

    //draw center mask
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(*wxWHITE));

    int x = size.x / 2;
    int y = size.y / 2;
    dc.DrawEllipticArc(x - center_mask_radius, y - center_mask_radius, center_mask_radius * 2, center_mask_radius * 2, 0, 360);

    //draw center icon
    dc.DrawBitmap(bitmap_backup_tips_0.bmp(), wxPoint((size.x - bitmap_backup_tips_0.GetBmpSize().x) / 2, (size.y - bitmap_backup_tips_0.GetBmpSize().y) / 2));
    //dc.DrawBitmap(bitmap_backup_tips_1.bmp(), wxPoint((size.x - bitmap_backup_tips_1.GetBmpSize().x) / 2, (size.y - bitmap_backup_tips_1.GetBmpSize().y) / 2));

    //draw material
    dc.SetTextForeground(wxColour("#323A3D"));
    dc.SetFont(Label::Head_15);
    auto text_size = dc.GetTextExtent(m_material_name);
    dc.DrawText(m_material_name, (size.x - text_size.x) / 2,(size.y - text_size.y) / 2 - FromDIP(12));

    dc.SetFont(Label::Body_13);
    text_size = dc.GetTextExtent(m_group_index);
    dc.DrawText(m_group_index, (size.x - text_size.x) / 2, (size.y - text_size.y) / 2 + FromDIP(10));
}

AmsHumidityLevelList::AmsHumidityLevelList(wxWindow* parent)
 : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    SetSize(wxSize(FromDIP(680), FromDIP(104)));
    SetMinSize(wxSize(FromDIP(680), FromDIP(104)));
    SetMaxSize(wxSize(FromDIP(680), FromDIP(104)));
    SetBackgroundColour(*wxWHITE);

    background_img = ScalableBitmap(this, "humidity_list_background", 104);

    for (int i = 5; i > 0; i--) {
        hum_level_img_light.push_back(ScalableBitmap(this, ("hum_level" + std::to_string(i) + "_light"), 54));
        hum_level_img_dark.push_back(ScalableBitmap(this, ("hum_level" + std::to_string(i) + "_dark"), 54));
    }

    Bind(wxEVT_PAINT, &AmsHumidityLevelList::paintEvent, this);
    wxGetApp().UpdateDarkUI(this);
}

void AmsHumidityLevelList::msw_rescale()
{
    background_img.msw_rescale();

    for (int i = 0; i < hum_level_img_light.size(); i++)
    {
        hum_level_img_light[i].msw_rescale();
    }

    for (int i = 0; i < hum_level_img_dark.size(); i++)
    {
        hum_level_img_dark[i].msw_rescale();
    }

    Refresh();
}

void AmsHumidityLevelList::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void AmsHumidityLevelList::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void AmsHumidityLevelList::doRender(wxDC& dc)
{
    dc.DrawBitmap(background_img.bmp(), 0,0);

    auto width_center = GetSize().x / 2;
    auto left = width_center - FromDIP(27) - FromDIP(46) * 2 - FromDIP(54) * 2;


    //dry / wet
    dc.SetTextForeground(wxColour("#989898"));
    dc.SetFont(::Label::Head_20);

    auto font_top = GetSize().y - dc.GetTextExtent(_L("DRY")).GetHeight();
    dc.DrawText(_L("DRY"), wxPoint(FromDIP(38), font_top / 2));
    dc.DrawText(_L("WET"), wxPoint(( GetSize().x - FromDIP(38) -  dc.GetTextExtent(_L("DRY")).GetWidth()), font_top / 2));


    //level list

    for (int i = 0; i < hum_level_img_light.size(); i++) {
        if (wxGetApp().dark_mode()) {
            dc.DrawBitmap(hum_level_img_dark[i].bmp(), left, (GetSize().y - FromDIP(54)) / 2);
        }
        else {
             dc.DrawBitmap(hum_level_img_light[i].bmp(), left, (GetSize().y - FromDIP(54)) / 2);
        }

        left += FromDIP(46) + FromDIP(54);
    }
}

DevIconLabel::DevIconLabel(wxWindow* parent, const wxString& icon, const wxString& label)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    CreateGui();

    SetIcon(icon);
    SetLabel(label);
}

void DevIconLabel::CreateGui()
{
    wxSizer* sizer_main = new wxBoxSizer(wxHORIZONTAL);
    m_icon = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap);
    m_icon->SetBackgroundColour(*wxWHITE);

    m_label = new Label(this);
    m_label->SetFont(::Label::Body_13);
    m_label->SetBackgroundColour(*wxWHITE);

    sizer_main->Add(m_icon, 0, wxALIGN_CENTER_VERTICAL, 0);
    sizer_main->Add(m_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    SetSizer(sizer_main);
    Layout();
    Fit();
}

void DevIconLabel::SetLabel(const wxString& label)
{
    m_label->SetLabel(label);
}

void DevIconLabel::SetIcon(const wxString& icon)
{
    if (m_icon_str != icon) {
        m_icon_str = icon;
        m_icon->SetBitmap(create_scaled_bitmap(icon.ToStdString(), this, 20));
        Refresh();
    }
}

void DevIconLabel::SetAllBackgroundColor(const wxColour& color)
{
    SetBackgroundColour(color);
    m_icon->SetBackgroundColour(color);
    m_label->SetBackgroundColour(color);
}

void Slic3r::GUI::DevIconLabel::Rescale()
{
    m_icon->SetBitmap(create_scaled_bitmap(m_icon_str.ToStdString(), this, 20));
    Refresh();
}

}} // namespace Slic3r::GUI

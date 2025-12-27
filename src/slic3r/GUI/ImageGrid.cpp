#include "ImageGrid.h"
#include "Printer/PrinterFileSystem.h"
#include "wxExtensions.hpp"
#include "Widgets/Label.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"

#include <wx/dcgraph.h>

#include <boost/log/trivial.hpp>

wxDEFINE_EVENT(EVT_ITEM_ACTION, wxCommandEvent);

BEGIN_EVENT_TABLE(Slic3r::GUI::ImageGrid, wxWindow)

EVT_MOTION(Slic3r::GUI::ImageGrid::mouseMoved)
EVT_ENTER_WINDOW(Slic3r::GUI::ImageGrid::mouseEnterWindow)
EVT_LEAVE_WINDOW(Slic3r::GUI::ImageGrid::mouseLeaveWindow)
EVT_MOUSEWHEEL(Slic3r::GUI::ImageGrid::mouseWheelMoved)
EVT_LEFT_DOWN(Slic3r::GUI::ImageGrid::mouseDown)
EVT_LEFT_UP(Slic3r::GUI::ImageGrid::mouseReleased)
EVT_SIZE(Slic3r::GUI::ImageGrid::resize)

// catch paint events
EVT_PAINT(Slic3r::GUI::ImageGrid::paintEvent)

END_EVENT_TABLE()

namespace Slic3r {
namespace GUI {

static constexpr int SHADOW_WIDTH = 3;

ImageGrid::ImageGrid(wxWindow * parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_buttonBackgroundColor(StateColor(
            std::make_pair(*wxWHITE, (int) StateColor::Pressed),
            std::make_pair(*wxRED, (int) StateColor::Normal)))
    , m_buttonTextColor(StateColor(
            std::make_pair(0x3B4446, (int) StateColor::Pressed),
            std::make_pair(*wxLIGHT_GREY, (int) StateColor::Hovered),
            std::make_pair(*wxWHITE, (int) StateColor::Normal)))
    , m_checked_icon(this, "check_on", 16)
    , m_unchecked_icon(this, "check_off", 16)
    , m_model_time_icon(this, "model_time", 14)
    , m_model_weight_icon(this, "model_weight", 14)
{
    m_cell_size.Set(396, 228);

    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(0xEEEEEE);
    SetFont(Label::Head_20);

    m_timer.Bind(wxEVT_TIMER, [this](auto & e) { Refresh(); });
    Rescale();
}

void ImageGrid::SetFileSystem(boost::shared_ptr<PrinterFileSystem> file_sys)
{
    if (m_file_sys) {
        m_file_sys->Unbind(EVT_MODE_CHANGED, &ImageGrid::changedEvent, this);
        m_file_sys->Unbind(EVT_FILE_CHANGED, &ImageGrid::changedEvent, this);
        m_file_sys->Unbind(EVT_THUMBNAIL, &ImageGrid::changedEvent, this);
        m_file_sys->Unbind(EVT_DOWNLOAD, &ImageGrid::changedEvent, this);
    }
    m_file_sys = file_sys;
    if (m_file_sys) {
        m_file_sys->Bind(EVT_MODE_CHANGED, &ImageGrid::changedEvent, this);
        m_file_sys->Bind(EVT_FILE_CHANGED, &ImageGrid::changedEvent, this);
        m_file_sys->Bind(EVT_THUMBNAIL, &ImageGrid::changedEvent, this);
        m_file_sys->Bind(EVT_DOWNLOAD, &ImageGrid::changedEvent, this);
    }
    m_row_count = 0;
    m_col_count = 1;
    m_row_offset = 0;
    m_scroll_offset = 0;
    UpdateFileSystem();
}

void ImageGrid::SetStatus(ScalableBitmap const & icon, wxString const &msg)
{
    int code     = m_file_sys ? m_file_sys->GetLastError() : 1;
    m_status_icon = icon;
    m_status_msg = wxString::Format(msg, code);
    BOOST_LOG_TRIVIAL(info) << "ImageGrid::SetStatus: " << m_status_msg.ToUTF8().data();
    Refresh();
}

void Slic3r::GUI::ImageGrid::SetFileType(int type, std::string const &storage)
{
    if (!m_file_sys)
        return;
    m_file_sys->SetFileType((PrinterFileSystem::FileType) type, storage);
}

void Slic3r::GUI::ImageGrid::SetGroupMode(int mode)
{
    if (!m_file_sys)
        return;
    if (m_file_sys->GetCount() == 0) {
        m_file_sys->SetGroupMode((PrinterFileSystem::GroupMode) mode);
        return;
    }
    wxSize size = GetClientSize();
    int index = (m_row_offset + 1 < m_row_count || m_row_count == 0) 
        ? m_row_offset / 4 * m_col_count 
        : ((m_file_sys->GetCount() + m_col_count - 1) / m_col_count - (size.y + m_border_size.GetHeight() - 1) / m_cell_size.GetHeight()) * m_col_count;
    auto & file = m_file_sys->GetFile(index);
    m_file_sys->SetGroupMode((PrinterFileSystem::GroupMode) mode);
    index = m_file_sys->GetIndexAtTime(file.time);
    // UpdateFileSystem(); call by changed event
    m_row_offset = index / m_col_count * 4;
    if (m_row_offset >= m_row_count)
        m_row_offset = m_row_count == 0 ? 0 : m_row_count - 1;
    m_scroll_offset = 0;
}

void Slic3r::GUI::ImageGrid::SetSelecting(bool selecting)
{
    m_selecting = selecting;
    if (m_file_sys)
        m_file_sys->SelectAll(false);
    Refresh();
}

void Slic3r::GUI::ImageGrid::DoActionOnSelection(int action) { DoAction(-1, action); }

void Slic3r::GUI::ImageGrid::ShowDownload(bool show)
{
    m_show_download = show;
    Refresh();
}

void Slic3r::GUI::ImageGrid::Rescale()
{
    m_title_mask = wxBitmap();
    m_border_mask = wxBitmap();
    UpdateFileSystem();
    auto em              = em_unit(this);
    wxSize size1{384 * em / 10, 4 * em};
    m_buttons_background = createAlphaBitmap(size1, *wxBLACK, 77, 77);
    m_buttons_background_checked = createAlphaBitmap(size1, wxColor("#FF2002"), 77, 77);
    //wxSize size2{128 * m_buttonBackgroundColor.count() * em_unit(this) / 10, 4 * em_unit(this)};
    //m_button_background = createAlphaBitmap(size2, *wxBLACK, 77, 77);
}

void Slic3r::GUI::ImageGrid::Select(size_t index)
{
    if (m_selecting) {
        m_file_sys->ToggleSelect(index);
        Refresh();
        return;
    }
    if (m_file_sys->GetGroupMode() == PrinterFileSystem::G_NONE) {
        return;
    }
    index = m_file_sys->EnterSubGroup(index);
    // UpdateFileSystem(); call by changed event
    m_row_offset = index / m_col_count * 4;
    if (m_row_offset >= m_row_count)
        m_row_offset = m_row_count == 0 ? 0 : m_row_count - 1;
    m_scroll_offset = 0;
    Refresh();
}

void Slic3r::GUI::ImageGrid::DoAction(size_t index, int action)
{
    wxCommandEvent event(EVT_ITEM_ACTION);
    event.SetEventObject(this);
    event.SetInt(action);
    event.SetExtraLong(long(index));
    ProcessEventLocally(event);
}

void Slic3r::GUI::ImageGrid::UpdateFileSystem()
{
    if (!m_file_sys) return;
    if (m_file_sys->GetFileType() < PrinterFileSystem::F_MODEL) {
        if (m_file_sys->GetGroupMode() == PrinterFileSystem::G_NONE) {
            m_cell_size.Set(396, 228);
            m_border_size.Set(384, 216);
        }
        else {
            m_cell_size.Set(496, 286);
            m_border_size.Set(480, 270);
        }
    } else {
        m_cell_size.Set(292, 288);
        m_border_size.Set(266, 264);
    }
    m_cell_size = m_cell_size * em_unit(this) / 10;
    m_border_size  = m_border_size * em_unit(this) / 10;
    m_content_rect = wxRect(SHADOW_WIDTH, SHADOW_WIDTH, m_border_size.GetWidth(), m_border_size.GetHeight());
    m_border_size += wxSize(SHADOW_WIDTH, SHADOW_WIDTH) * 2;
    UpdateLayout();
}

void ImageGrid::UpdateLayout()
{
    if (!m_file_sys) return;
    wxSize size = GetClientSize();
    wxSize title_mask_size{0, 60 * em_unit(this) / 10};
    if (m_file_sys->GetGroupMode() == PrinterFileSystem::G_NONE) {
        title_mask_size.y = 20 * em_unit(this) / 10;
        size.y -= title_mask_size.y;
    }
    int cell_width = m_cell_size.GetWidth();
    int cell_height = m_cell_size.GetHeight();
    int ncol = (size.GetWidth() - cell_width + m_border_size.GetWidth()) / cell_width;
    if (ncol <= 0) ncol = 1;
    int total_height = (m_file_sys->GetCount() + ncol - 1) / ncol * cell_height + cell_height - m_border_size.GetHeight();
    int nrow = (total_height - size.GetHeight() + cell_height / 4 - 1) / (cell_height / 4);
    m_row_offset = m_row_offset * m_col_count / ncol;
    m_col_count = ncol;
    m_row_count = nrow > 0 ? nrow + 1 : 0;
    if (m_row_offset >= m_row_count)
        m_row_offset = m_row_count == 0 ? 0 : m_row_count - 1;
    m_scroll_offset = 0;
    // create mask
    if (m_file_sys->GetGroupMode() == PrinterFileSystem::G_NONE) {
        title_mask_size.x = (m_col_count - 1) * m_cell_size.GetWidth() + m_border_size.GetWidth();
    }
    else {
        title_mask_size.x = m_border_size.x;
    }
    if (!m_title_mask.IsOk() || m_title_mask.GetSize() != title_mask_size)
        m_title_mask = createAlphaBitmap(title_mask_size, 0x6f6f6f, 255, 0);
    if (!m_border_mask.IsOk() || m_border_mask.GetSize() != m_border_size)
        m_border_mask = createShadowBorder(m_border_size, StateColor::darkModeColorFor(0xEEEEEE), em_unit(this), 3);
    UpdateFocusRange();
    Refresh();
}

void Slic3r::GUI::ImageGrid::UpdateFocusRange()
{
    if (!m_file_sys) return;
    wxSize  size = GetClientSize();
    wxPoint off;
    int     index = firstItem(size, off);
    int     count = m_col_count;
    while (off.y < size.y) {
        count += m_col_count;
        off.y += m_cell_size.y;
    }
    m_file_sys->SetFocusRange(index, count);
}

std::pair<int, size_t> Slic3r::GUI::ImageGrid::HitTest(wxPoint const &pt)
{
    if (!m_file_sys)
        return {HIT_NONE, -1};
    wxSize size  = GetClientSize();
    if (m_file_sys->GetCount() == 0) {
        if (wxRect({0, 0}, m_border_size).CenterIn(wxRect({0, 0}, size)).Contains(pt))
            return {HIT_STATUS, 0};
        return {HIT_NONE, -1};
    }
    wxPoint off;
    size_t index = firstItem(size, off);
    off          = pt - off;
    int n        = 0;
    while (off.x > m_cell_size.GetWidth() && n + 1 < m_col_count) {
        ++n;
        off.x -= m_cell_size.GetWidth();
    }
    index += n;
    while (off.y > m_cell_size.GetHeight()) {
        index += m_col_count;
        off.y -= m_cell_size.GetHeight();
    }
    if (index >= m_file_sys->GetCount()) { return {HIT_NONE, -1}; }
    if (!m_content_rect.Contains(off)) { return {HIT_NONE, -1}; }
    if (!m_selecting) {
        wxRect hover_rect{0, m_content_rect.GetHeight() - m_buttons_background.GetHeight(), m_content_rect.GetWidth(), m_buttons_background.GetHeight()};
        auto & file = m_file_sys->GetFile(index);
        int    btn  = file.IsDownload() && file.DownloadProgress() >= 0 ? 3 : 2;
        if (m_file_sys->GetFileType() == PrinterFileSystem::F_MODEL) {
            if (m_show_download)
                btn = 3;
            hover_rect.y -= m_content_rect.GetHeight() * 64 / 264;
        }
        if (hover_rect.Contains(off.x, off.y)) {
            return {HIT_ACTION, index * 4 + off.x * btn / hover_rect.GetWidth()};
        } // Two buttons
    }
    return {HIT_ITEM, index};
}

void ImageGrid::mouseMoved(wxMouseEvent& event)
{
    if (!m_hovered || m_pressed)
        return;
    auto hit = HitTest(event.GetPosition());
    if (hit != std::make_pair(m_hit_type, m_hit_item)) {
        m_hit_type = hit.first;
        m_hit_item = hit.second;
        if (hit.first == HIT_ITEM) {
            SetToolTip({});
            auto & file = m_file_sys->GetFile(hit.second);
            if (auto title = file.Title(); !title.empty()) {
                auto tip = wxString::Format(_L("File: %s\nTitle: %s\n"), from_u8(file.name), from_u8(title));
                SetToolTip(tip);
            } else {
                SetToolTip(from_u8(file.name));
            }
        } else
            SetToolTip({});
        Refresh();
    }
}

void ImageGrid::mouseEnterWindow(wxMouseEvent& event)
{
    if (!m_hovered)
        m_hovered = true;
}

void ImageGrid::mouseLeaveWindow(wxMouseEvent& event)
{
    if (m_hovered) {
        m_hovered = false;
        m_pressed = false;
        if (m_hit_type != HIT_NONE) {
            m_hit_type = HIT_NONE;
            m_hit_item = -1;
            Refresh();
        }
    }
}

void ImageGrid::mouseDown(wxMouseEvent& event)
{
    if (!m_pressed) {
        m_pressed = true;
        auto hit  = HitTest(event.GetPosition());
        m_hit_type = hit.first;
        m_hit_item = hit.second;
        if (m_hit_type >= HIT_ACTION)
            Refresh();
        SetFocus();
    }
}

void ImageGrid::mouseReleased(wxMouseEvent& event)
{
    if (!m_pressed)
        return;
    m_pressed = false;
    auto hit = HitTest(event.GetPosition());
    if (hit == std::make_pair(m_hit_type, m_hit_item)) {
        if (m_hit_type == HIT_ITEM)
            Select(m_hit_item);
        else if (m_hit_type == HIT_ACTION)
            DoAction(m_hit_item / 4, m_hit_item & 3);
        else if (m_hit_type == HIT_MODE)
            SetGroupMode(static_cast<PrinterFileSystem::GroupMode>(2 - m_hit_item));
        else if (m_hit_type == HIT_STATUS)
            m_file_sys->Retry();
        else
            Refresh();
    } else {
        Refresh();
    }
}

void ImageGrid::resize(wxSizeEvent& event)
{
    UpdateLayout();
}

void ImageGrid::mouseWheelMoved(wxMouseEvent &event)
{
    auto delta = -event.GetWheelRotation();
    m_scroll_offset += delta * 4;
    delta = m_scroll_offset / m_cell_size.GetHeight();
    m_row_offset += delta;
    if (m_row_offset < 0)
        m_row_offset = 0;
    else if (m_row_offset >= m_row_count)
        m_row_offset = m_row_count == 0 ? 0 : m_row_count - 1;
    m_scroll_offset -= delta * m_cell_size.GetHeight();
    m_timer.StartOnce(4000); // Show position bar
    UpdateFocusRange();
    Refresh();
}

void Slic3r::GUI::ImageGrid::changedEvent(wxCommandEvent& evt)
{
    evt.Skip();
    BOOST_LOG_TRIVIAL(debug) << "ImageGrid::changedEvent: " << evt.GetEventType() << " index: " << evt.GetInt() 
            << " name: " << evt.GetString().ToUTF8().data() << " extra: " << evt.GetExtraLong();
    if (evt.GetEventType() == EVT_FILE_CHANGED) {
        if (evt.GetInt() == -1)
            m_file_sys->DownloadCheckFiles(wxGetApp().app_config->get("download_path"));
        UpdateFileSystem();
    }
    else if (evt.GetEventType() == EVT_MODE_CHANGED)
        UpdateFileSystem();
    //else if (evt.GetEventType() == EVT_THUMBNAIL)
    //    RefreshRect(itemRect(evt.GetInt()), false);
    else
        Refresh();
}

void ImageGrid::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxPaintDC dc(this);
    render(dc);
}

size_t Slic3r::GUI::ImageGrid::firstItem(wxSize const &size, wxPoint &off)
{
    int size_y = size.y;
    if (m_file_sys->GetGroupMode() == PrinterFileSystem::G_NONE)
        size_y -= m_title_mask.GetHeight();
    int offx  = (size.x - (m_col_count - 1) * m_cell_size.GetWidth() - m_border_size.GetWidth()) / 2;
    int offy  = (m_row_offset + 1 < m_row_count || m_row_count == 0) ?
                    m_cell_size.GetHeight() - m_border_size.GetHeight() - m_row_offset * m_cell_size.GetHeight() / 4 + m_row_offset / 4 * m_cell_size.GetHeight() :
                    size_y - (size_y + m_border_size.GetHeight() - 1) / m_cell_size.GetHeight() * m_cell_size.GetHeight();
    int index = (m_row_offset + 1 < m_row_count || m_row_count == 0) ?
                    m_row_offset / 4 * m_col_count :
                    ((m_file_sys->GetCount() + m_col_count - 1) / m_col_count - (size_y + m_border_size.GetHeight() - 1) / m_cell_size.GetHeight()) * m_col_count;
    if (m_file_sys->GetGroupMode() == PrinterFileSystem::G_NONE)
        offy += m_title_mask.GetHeight();
    off = wxPoint{offx, offy};
    return index;
}

wxBitmap Slic3r::GUI::ImageGrid::createAlphaBitmap(wxSize size, wxColour color, int alpha1, int alpha2)
{
    wxImage image(size);
    image.InitAlpha();
    unsigned char *alpha = image.GetAlpha();
    image.SetRGB(wxRect({0, 0}, size), color.Red(), color.Green(), color.Blue());
    int d = alpha2 - alpha1;
    if (d == 0)
        memset(alpha, alpha1, size.GetWidth() * size.GetHeight());
    else
        for (int i = 0; i < size.GetHeight(); ++i)
            memset(alpha + i * size.GetWidth(), alpha1 + i * d / size.GetHeight(), size.GetWidth());
    return wxBitmap(std::move(image));
}

wxBitmap Slic3r::GUI::ImageGrid::createShadowBorder(wxSize size, wxColour color, int radius, int shadow)
{
    wxImage image(size);
    image.InitAlpha();
    memset(image.GetAlpha(), 0, size.GetWidth() * size.GetHeight());
    wxBitmap   bmp(std::move(image));
    wxMemoryDC memdc;
    memdc.SelectObject(bmp);
#ifdef __WXMSW__
    wxGCDC dc2(memdc);
#else
    wxDC &dc2(memdc);
#endif
    wxRect rc(0, 0, size.x, size.y);
    dc2.SetBrush(*wxTRANSPARENT_BRUSH);
    auto n = ((radius + shadow) * 1414 / 1000 - radius);
    dc2.SetPen(wxPen(color, n | 1));
    n = n / 2 - shadow + 1;
    rc.Inflate(n, n);
    dc2.DrawRoundedRectangle(rc, radius + shadow);
    rc.Deflate(n, n);
    rc.Deflate(shadow, shadow);
    for (int i = 0; i < shadow; ++i) {
        rc.Inflate(1, 1);
        dc2.SetPen(wxColor(0, 0, 0, 100 - i * 30));
        dc2.DrawRoundedRectangle(rc, radius + i);
    }

    memdc.SelectObject(wxNullBitmap);
    return bmp;
}

wxBitmap Slic3r::GUI::ImageGrid::createCircleBitmap(wxSize size, int borderWidth, int percent, wxColour fillColor, wxColour borderColor)
{
    wxImage image(size);
    image.InitAlpha();
    memset(image.GetAlpha(), 0, size.GetWidth() * size.GetHeight());
    wxBitmap   bmp(std::move(image));
    wxRect     rc(0, 0, size.x, size.y);
    wxMemoryDC memdc;
    memdc.SelectObject(bmp);
#ifdef __WXMSW__
    wxGCDC dc2(memdc);
#else
    wxDC &dc2(memdc);
#endif
    if (borderWidth && borderColor != wxTransparentColor) {
        int d = ceil(borderWidth / 2.0);
        rc.Deflate(d, d);
        dc2.SetPen(wxPen(borderColor, borderWidth));
    } else {
        dc2.SetPen(wxPen(fillColor));
    }
    dc2.SetBrush(wxBrush(fillColor));
    if (percent > 0)
        dc2.DrawEllipticArc(rc.GetTopLeft(), rc.GetSize(), 0, percent * 360.0 / 100);

    memdc.SelectObject(wxNullBitmap);
    return bmp;
}

static constexpr wchar_t const *TIME_FORMATS[] = {_T("%Y-%m-%d"), _T("%Y-%m"), _T("%Y")};

/*
* Here we do the actual rendering. I put it in a separate
* method so that it can work no matter what type of DC
* (e.g. wxPaintDC or wxClientDC) is used.
*/
void ImageGrid::render(wxDC& dc)
{
    wxSize size = GetClientSize();
    dc.SetPen(wxPen(GetBackgroundColour()));
    dc.SetBrush(wxBrush(GetBackgroundColour()));
    if (!m_file_sys || m_file_sys->GetCount() == 0) {
        dc.DrawRectangle({ 0, 0, size.x, size.y });
        if (!m_status_msg.IsEmpty()) {
            auto   si = m_status_icon.GetBmpSize();
            auto st   = dc.GetMultiLineTextExtent(m_status_msg);
            auto   rect = wxRect{0, 0, max(st.x, si.x), si.y + 26 + st.y}.CenterIn(wxRect({0, 0}, size));
            dc.DrawBitmap(m_status_icon.bmp(), rect.x + (rect.width - si.x) / 2, rect.y);
            dc.SetTextForeground(wxColor(0x909090));
            dc.DrawText(m_status_msg, rect.x + (rect.width - st.x) / 2, rect.GetBottom() - st.y);
        }
        return;
    }
    wxPoint off;
    size_t index = firstItem(size, off);
    // Draw background: left/right side
    dc.DrawRectangle({0, 0, off.x, size.y});
    dc.DrawRectangle({size.x - off.x - 1, 0, off.x + 1, size.y});
    // Draw line spacing at top
    if (off.y > 0)
        dc.DrawRectangle({0, 0, size.x, off.y});
    size_t start = index;
    size_t end = index;
    size_t hit_image = m_selecting ? size_t(-1) : m_hit_type == HIT_ITEM ? m_hit_item : m_hit_type == HIT_ACTION ? m_hit_item / 4 :size_t(-1);
    // Draw items with background
    while (off.y < size.y)
    {
        // Draw one line
        wxPoint pt{off.x, off.y};
        end = (index + m_col_count) < m_file_sys->GetCount() ? index + m_col_count : m_file_sys->GetCount();
        while (index < end) {
            pt += m_content_rect.GetTopLeft();
            // Draw content
            decltype(&ImageGrid::renderContent1) contentRender[] = {
                &ImageGrid::renderContent1,
                &ImageGrid::renderContent1,
                &ImageGrid::renderContent2
            };
            (this->*contentRender[m_file_sys->GetFileType()])(dc, pt, index, hit_image == index);
            pt -= m_content_rect.GetTopLeft();
            // Draw colume spacing at right
            dc.DrawRectangle({pt.x + m_border_size.GetWidth(), pt.y, m_cell_size.GetWidth() - m_border_size.GetWidth(), m_border_size.GetHeight()});
            // Draw overlay border mask
            dc.DrawBitmap(m_border_mask, pt.x, pt.y);
            ++index;
            pt.x += m_cell_size.GetWidth();
        }
        // Draw line fill items
        if (end < index + m_col_count)
            dc.DrawRectangle({pt.x, pt.y, size.x - pt.x - off.x, m_border_size.GetHeight()});
        // Draw line spacing at bottom
        dc.DrawRectangle({off.x, pt.y + m_border_size.GetHeight(), size.x - off.x * 2, m_cell_size.GetHeight() - m_border_size.GetHeight()});
        off.y += m_cell_size.GetHeight();
    }
    // Draw floating date range for non-group list
    if (m_file_sys->GetGroupMode() == PrinterFileSystem::G_NONE && m_file_sys->GetCount() > 0) {
        //dc.DrawBitmap(m_title_mask, {off.x, 0});
        dc.DrawRectangle({off.x, 0}, m_title_mask.GetSize());
        auto & file1 = m_file_sys->GetFile(start);
        auto & file2 = m_file_sys->GetFile(end - 1);
        auto date1 = wxDateTime((time_t) file1.time).Format(_L(TIME_FORMATS[m_file_sys->GetGroupMode()]));
        auto date2 = wxDateTime((time_t) file2.time).Format(_L(TIME_FORMATS[m_file_sys->GetGroupMode()]));
        dc.SetFont(Label::Head_16);
        dc.SetTextForeground(StateColor::darkModeColorFor("#262E30"));
        dc.DrawText(date1 + " - " + date2, wxPoint{off.x, 2});
    }
    // Draw bottom background
    if (off.y < size.y)
        dc.DrawRectangle({off.x, off.y, size.x - off.x * 2, size.y - off.y});
    // Draw position bar
    if (m_timer.IsRunning()) {
        int total_height = (m_file_sys->GetCount() + m_col_count - 1) / m_col_count * m_cell_size.GetHeight() + m_cell_size.GetHeight() - m_border_size.GetHeight();
        if (total_height > size.y) {
            int offset = (m_row_offset + 1 < m_row_count || m_row_count == 0) ? m_row_offset * (m_cell_size.GetHeight() / 4) : total_height - size.y;
            wxRect rect = {size.x - 16, offset * size.y / total_height, 8,
                size.y * size.y / total_height};
            dc.SetBrush(wxBrush(*wxLIGHT_GREY));
            dc.DrawRoundedRectangle(rect, 4);
        }
    }
}

void Slic3r::GUI::ImageGrid::renderContent1(wxDC &dc, wxPoint const &pt, int index, bool hit)
{
    bool selected = false;
    auto &file = m_file_sys->GetFile(index, selected);
    // Draw thumbnail
    if (file.thumbnail.IsOk()) {
        float hs = (float) m_content_rect.GetWidth() / file.thumbnail.GetWidth();
        float vs = (float) m_content_rect.GetHeight() / file.thumbnail.GetHeight();
        dc.SetUserScale(hs, vs);
        dc.DrawBitmap(file.thumbnail, {(int) (pt.x / hs), (int) (pt.y / vs)});
        dc.SetUserScale(1, 1);
        if (m_file_sys->GetGroupMode() != PrinterFileSystem::G_NONE) { dc.DrawBitmap(m_title_mask, pt); }
    }
    bool show_download_state_always = true;
    // Draw checked icon
    if (m_selecting && !show_download_state_always)
        dc.DrawBitmap(selected ? m_checked_icon.bmp() : m_unchecked_icon.bmp(), pt + wxPoint{10, 10});
    // can't handle alpha
    // dc.GradientFillLinear({pt.x, pt.y, m_border_size.GetWidth(), 60}, wxColour(0x6F, 0x6F, 0x6F, 0x99), wxColour(0x6F, 0x6F, 0x6F, 0), wxBOTTOM);
    else if (m_file_sys->GetGroupMode() == PrinterFileSystem::G_NONE) {
        wxString nonHoverText;
        wxString secondAction = m_show_download ? _L("Download") : "";
        wxString thirdAction;
        int      states = 0;
        // Draw download progress
        if (file.IsDownload()) {
            int progress = file.DownloadProgress();
            if (progress == -1) {
                secondAction = _L("Cancel");
                nonHoverText = _L("Download waiting...");
            } else if (progress < 0) {
                secondAction = _L("Retry");
                nonHoverText = _L("Download failed");
                states       = StateColor::Checked;
            } else if (progress >= 100) {
                secondAction = _L("Play");
                thirdAction  = _L("Open Folder");
                nonHoverText = _L("Download finished");
            } else {
                secondAction = _L("Cancel");
                nonHoverText = wxString::Format(_L("Downloading %d%%..."), progress);
                thirdAction  = wxString::Format(L"%d%%...", progress);
            }
        }
        if (m_file_sys->GetFileType() == PrinterFileSystem::F_MODEL) {
            if (secondAction != _L("Play"))
                thirdAction = secondAction;
            secondAction = _L("Print");
        }
        // Draw buttons on hovered item
        wxRect rect{pt.x, pt.y + m_content_rect.GetBottom() - m_buttons_background.GetHeight(), m_content_rect.GetWidth(), m_buttons_background.GetHeight()};
        wxArrayString texts;
        if (hit) {
            texts.Add(_L("Delete"));
            texts.Add(secondAction);
            if (!thirdAction.IsEmpty())
                texts.Add(thirdAction);
            renderButtons(dc, texts, rect, m_hit_type == HIT_ACTION ? m_hit_item & 3 : -1, states);
        } else if (!nonHoverText.IsEmpty()) {
            texts.Add(nonHoverText);
            renderButtons(dc, texts, rect, -1, states);
        }
    } else {
        dc.SetTextForeground(*wxWHITE); // time text color
        auto date = wxDateTime((time_t) file.time).Format(_L(TIME_FORMATS[m_file_sys->GetGroupMode()]));
        dc.DrawText(date, pt + wxPoint{24, 16});
    }
    if (m_selecting && show_download_state_always)
        dc.DrawBitmap(selected ? m_checked_icon.bmp() : m_unchecked_icon.bmp(), pt + wxPoint{10, 10});
}

void Slic3r::GUI::ImageGrid::renderContent2(wxDC &dc, wxPoint const &pt, int index, bool hit)
{
    auto &file = m_file_sys->GetFile(index);
    // Draw thumbnail & buttons
    int h = m_content_rect.GetHeight() * 64 / 264;
    m_content_rect.SetHeight(m_content_rect.GetHeight() - h);
    auto br = dc.GetBrush();
    auto pn = dc.GetPen();
    dc.SetBrush(StateColor::darkModeColorFor(0xEEEEEE));
    dc.SetPen(StateColor::darkModeColorFor(0xEEEEEE));
    dc.DrawRectangle(pt, m_content_rect.GetSize()); // Fix translucent model thumbnail
    renderContent1(dc, pt, index, hit);
    m_content_rect.SetHeight(m_content_rect.GetHeight() + h);
    // Draw info bar
    dc.SetBrush(StateColor::darkModeColorFor(*wxWHITE));
    dc.SetPen(StateColor::darkModeColorFor(*wxWHITE));
    dc.DrawRectangle(pt.x, pt.y + m_content_rect.GetHeight() - h, m_content_rect.GetWidth(), h);
    dc.SetBrush(br);
    dc.SetPen(pn);
    // Draw infos
    dc.SetFont(Label::Head_16);
    dc.SetTextForeground(StateColor::darkModeColorFor("#323A3D"));
    auto em = em_unit(this);
    wxRect rect{pt.x, pt.y + m_content_rect.GetHeight() - h, m_content_rect.GetWidth(), h / 2};
    rect.Deflate(em, 0);
    renderText2(dc, from_u8(file.name), rect);
    rect.Offset(0, h / 2);
    rect.SetWidth(rect.GetWidth() / 2 - em);
    dc.SetFont(Label::Body_13);
    dc.SetTextForeground(StateColor::darkModeColorFor("#6B6B6B"));
    renderIconText(dc, m_model_time_icon, file.Metadata("Time", "0m"), rect);
    rect.Offset(m_content_rect.GetWidth() / 2, 0);
    renderIconText(dc, m_model_weight_icon, file.Metadata("Weight", "0g"), rect);
}

void Slic3r::GUI::ImageGrid::renderButtons(wxDC &dc, wxArrayString const &texts, wxRect const &rect2, size_t hit, int states)
{
    // Draw background
    {
        wxMemoryDC mdc(states & StateColor::Checked ? m_buttons_background_checked : m_buttons_background);
        dc.Blit(rect2.GetTopLeft(), rect2.GetSize(), &mdc, {0, 0});
    }
    // Draw buttons
    wxRect rect(rect2);
    rect.SetWidth(rect.GetWidth() / texts.size());
    int state = m_pressed ? StateColor::Pressed : StateColor::Hovered;
    dc.SetFont(Label::Body_14);
    //mdc.SelectObject(m_button_background);
    for (size_t i = 0; i < texts.size(); ++i) {
        int states2 = hit == i ? state : 0;
        // Draw button background
        //dc.Blit(rect.GetTopLeft(), rect.GetSize(), &mdc, {m_buttonBackgroundColor.colorIndexForStates(states) * 128, 0});
        //dc.DrawBitmap(m_button_background, rect2.GetTopLeft());
        //dc.SetBrush(m_buttonBackgroundColor.colorForStates(states2));
        //dc.DrawRectangle(rect);
        //rect.Deflate(10, 5);
        // Draw button splitter
        auto pen = dc.GetPen();
        dc.SetPen(wxPen("#616161"));
        if (i > 0) dc.DrawLine(rect.GetLeftTop(), rect.GetLeftBottom());
        dc.SetPen(pen);
        // Draw button text
        renderText(dc, texts[i], rect, states | states2);
        //rect.Inflate(10, 5);
        rect.Offset(rect.GetWidth(), 0);
    }
    dc.SetFont(GetFont());
}

void Slic3r::GUI::ImageGrid::renderText(wxDC &dc, wxString const &text, wxRect const &rect, int states)
{
    dc.SetTextForeground(m_buttonTextColor.colorForStatesNoDark(states));
    wxRect rc({0, 0}, dc.GetTextExtent(text));
    rc = rc.CenterIn(rect);
    float fontScale = float(rect.width - 8) / rc.width;
    if (fontScale < 1) {
        auto font = dc.GetFont();
        dc.SetFont(font.Scaled(fontScale));
        wxRect rc({0, 0}, dc.GetTextExtent(text));
        rc = rc.CenterIn(rect);
        dc.DrawText(text, rc.GetTopLeft());
        dc.SetFont(font);
    } else {
        dc.DrawText(text, rc.GetTopLeft());
    }
}

void Slic3r::GUI::ImageGrid::renderText2(wxDC &dc, wxString text, wxRect const &rect)
{
    wxRect rc({0, 0}, dc.GetTextExtent(text));
    rc = rc.CenterIn(rect);
    rc.SetLeft(rect.GetLeft());
    if (rc.GetWidth() > rect.GetWidth())
        text = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, rect.GetWidth());
    dc.DrawText(text, rc.GetTopLeft());
}

void Slic3r::GUI::ImageGrid::renderIconText(wxDC & dc, ScalableBitmap const & icon, wxString text, wxRect const & rect)
{
    dc.DrawBitmap(icon.bmp(), rect.x, rect.y + (rect.height - icon.GetBmpHeight()) / 2);
    renderText2(dc, text, {rect.x + icon.GetBmpWidth() + 4, rect.y, rect.width - icon.GetBmpWidth() - 4, rect.height});
}

}}

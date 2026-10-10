#include "ProgressBar.hpp"
#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/pen.h>
#include <wx/string.h>
#include "Label.hpp"



wxDEFINE_EVENT(wxCUSTOMEVT_SET_TEMP_FINISH, wxCommandEvent);
wxDEFINE_EVENT(EVT_PROGRESS_BAR_HEIGHT_CHANGED, wxCommandEvent);
BEGIN_EVENT_TABLE(ProgressBar, wxWindow)
EVT_PAINT(ProgressBar::paintEvent)
EVT_MOTION(ProgressBar::mouseMove)
EVT_LEAVE_WINDOW(ProgressBar::mouseLeave)
END_EVENT_TABLE()

ProgressBar::ProgressBar(wxWindow *parent, wxWindowID id, int max, const wxPoint &pos, const wxSize &size, bool shown)
{
    m_shownumber = shown;
    SetBackgroundColour(wxColour(255,255,255));

    if (size.y >= miniHeight) {
        m_miniHeight = size.y;
    } else {
        m_miniHeight = miniHeight;
    }

    m_max = max;
    m_radius = m_miniHeight / 2;
    m_barHeight = m_miniHeight;
    wxSize temp_size(size.x, m_miniHeight);

    SetFont(Label::Head_12);
    create(parent, id, pos, temp_size);
}


ProgressBar::~ProgressBar() {}


void ProgressBar::create(wxWindow *parent, wxWindowID id, const wxPoint &pos,  wxSize &size)
{
    wxWindow::Create(parent, id, pos, size);
    // m_static_info = new wxStaticText(this, wxID_ANY,wxT(""),wxPoint(this->padding, 20), wxSize(GetSize().GetWidth() - this->padding * 3, -1), wxST_ELLIPSIZE_END);
    // m_static_info->Wrap(-1);

   /* wxBoxSizer *m_sizer_body  = new wxBoxSizer(wxHORIZONTAL);

     auto m_progress_bk = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
     m_progress_bk->SetBackgroundColour(wxColour(238, 130, 238));
     StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed), std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                             std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));

     wxBoxSizer *m_sizer_progress= new wxBoxSizer(wxHORIZONTAL);

     auto m_progress = new wxPanel(m_progress_bk, wxID_ANY, wxDefaultPosition, wxSize(50, -1), wxTAB_TRAVERSAL);
     m_progress->SetBackgroundColour(wxColour(128, 0, 255));

     m_sizer_progress->Add(m_progress, 0, wxEXPAND, 0);

     m_progress_bk->SetSizer(m_sizer_progress);
     m_progress_bk->Layout();
     m_sizer_progress->Fit(m_progress_bk);
     m_sizer_body->Add(m_progress_bk, 1, wxEXPAND, 0);

     this->SetSizer(m_sizer_body);
     this->Layout();*/
}


void ProgressBar::SetRadius(double radius) {
    m_radius = radius;
    Refresh();
}

void ProgressBar::SetProgressForedColour(wxColour colour)
{
    m_progress_background_colour = colour;
    Refresh();
}

void ProgressBar::SetProgressBackgroundColour(wxColour colour)
{
    m_progress_colour = colour;
     Refresh();
}

void ProgressBar::SetMarkers(const std::vector<Marker> &markers)
{
    const bool unchanged = m_markers.size() == markers.size() &&
                           std::equal(m_markers.begin(), m_markers.end(), markers.begin(),
                                      [](const auto &lhs, const auto &rhs) {
                                          return lhs.m_position == rhs.m_position && lhs.m_label == rhs.m_label;
                                      });
    if (unchanged)
        return;

    m_markers = markers;
    m_hoveredMarker = m_lastMousePosition ? findHoveredMarker(*m_lastMousePosition) : -1;
    updateControlHeight();
    Refresh();
}

void ProgressBar::SetHeight(int height)
{
    m_barHeight = std::max(1, height);
    m_minHeight = m_barHeight;
    m_radius    = m_barHeight / 2;
    updateControlHeight();
}

// The control is as tall as the bar, plus room for the label bubble while any marker has a label.
void ProgressBar::updateControlHeight()
{
    const bool hasLabel = std::any_of(m_markers.begin(), m_markers.end(), [](const auto &marker) {
        return !marker.m_label.empty();
    });
    const int height = m_barHeight + (hasLabel ? FromDIP(34) : 0);
    if (GetMinSize().GetHeight() == height && GetSize().GetHeight() == height)
        return;

    wxSize minSize = GetMinSize();
    minSize.SetHeight(height);
    wxWindow::SetMinSize(minSize);
    SetSize(GetSize().GetWidth(), height);

    // Let the owner re-layout; the event stays on this window
    wxCommandEvent event(EVT_PROGRESS_BAR_HEIGHT_CHANGED, GetId());
    event.SetEventObject(this);
    event.StopPropagation();
    ProcessWindowEvent(event);
}

void ProgressBar::Rescale()
{
    ;
}

void ProgressBar::ShowNumber(bool shown)
{
    m_shownumber = shown;
    Refresh();
}

void ProgressBar::Disable(wxString text)
{
    if (m_disable) return;
    m_disable_text = text;
    m_disable = true;
    Refresh();
}

void ProgressBar::SetValue(int step)
{
    m_disable = false;
    SetProgress(step);
}

void ProgressBar::Reset()
{
    m_step = 0;
    SetValue(0);
}

void ProgressBar::SetProgress(int step)
{
    if (step < 0) return;
    if (m_disable == false && m_step == step)
    {
        return;
    }

    m_disable = false;
    m_step = step;
    Refresh();
}


void ProgressBar::SetMinSize(const wxSize &size)
{
    if (size.y >= miniHeight) {
        m_miniHeight = size.y;
    } else {
        return;
    }

    m_barHeight = m_miniHeight;
    m_radius    = m_miniHeight / 2.4;
    wxWindow::SetMinSize({size.x, m_barHeight});
    updateControlHeight();
    // SetSize(size);
    SetRadius(m_radius);
}


void ProgressBar::paintEvent(wxPaintEvent &evt)
{

    wxPaintDC dc(this);
    render(dc);
}

// Index of the labelled marker under the cursor, or -1. Only the bar row reacts, not the bubble area below it.
int ProgressBar::findHoveredMarker(const wxPoint &position) const
{
    const wxSize size = GetClientSize();
    if (position.y < 0 || position.y > m_barHeight || size.x <= 0 || m_max <= 0)
        return -1;

    const int hoverTolerance = FromDIP(7);
    for (size_t index = 0; index < m_markers.size(); ++index) {
        if (m_markers[index].m_label.empty())
            continue;

        const int markerX = size.x * std::clamp(m_markers[index].m_position, 0, m_max) / m_max;
        if (position.x >= markerX - hoverTolerance && position.x <= markerX + hoverTolerance)
            return static_cast<int>(index);
    }
    return -1;
}

void ProgressBar::mouseMove(wxMouseEvent &evt)
{
    m_lastMousePosition = evt.GetPosition();
    const int hoveredMarker = findHoveredMarker(*m_lastMousePosition);
    if (m_hoveredMarker != hoveredMarker) {
        m_hoveredMarker = hoveredMarker;
        Refresh();
    }
    evt.Skip();
}

void ProgressBar::mouseLeave(wxMouseEvent &evt)
{
    m_lastMousePosition.reset();
    if (m_hoveredMarker != -1) {
        m_hoveredMarker = -1;
        Refresh();
    }
    evt.Skip();
}

void ProgressBar::render(wxDC &dc)
{
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

void ProgressBar::doRender(wxDC &dc)
{
    if (m_step >= m_max) m_step = m_max;
    const wxSize size      = GetClientSize();
    const int    barHeight = std::min(m_barHeight, size.y);
    // Clear the whole control first: it can be taller than the bar while a marker label has room reserved
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(GetBackgroundColour()));
    dc.DrawRectangle(0, 0, size.x, size.y);

    dc.SetPen(wxPen(m_progress_background_colour, 1));
    dc.SetBrush(wxBrush(m_progress_background_colour));
    if (m_radius == 0) {
        dc.DrawRectangle(0, 0, size.x, barHeight);
    } else {
        dc.DrawRoundedRectangle(0, 0, size.x, barHeight, m_radius);
    }

    //draw progress
    if (m_disable) {
        m_proportion = float(size.x * float(this->m_step) / float(this->m_max));
        if (m_proportion < m_radius * 2 && m_proportion != 0) { m_proportion = m_radius * 2; }

        dc.SetPen(wxPen(m_progress_colour_disable, 1));
        dc.SetBrush(wxBrush(m_progress_colour_disable));
        if (m_radius == 0) {
            dc.DrawRectangle(0, 0, m_proportion, barHeight);
        } else {
            dc.DrawRoundedRectangle(0, 0, m_proportion, barHeight, m_radius);
        }

        dc.SetFont(::Label::Head_12);
        auto textSize = dc.GetMultiLineTextExtent(m_disable_text);
        dc.SetTextForeground(wxColour(144, 144, 144));
        auto pt = wxPoint();
        pt.x    = (size.x - textSize.x) / 2;
        pt.y    = (barHeight - textSize.y) / 2;
        dc.DrawText(m_disable_text, pt);

    } else {
        m_proportion = float(size.x * float(this->m_step) / float(this->m_max));
        if (m_proportion < m_radius * 2  && m_proportion != 0) { m_proportion = m_radius * 2; }

        dc.SetPen(wxPen(m_progress_colour, 1));
        dc.SetBrush(wxBrush(m_progress_colour));
        if (m_radius == 0) {
            dc.DrawRectangle(0, 0, m_proportion, barHeight);
        } else {
            dc.DrawRoundedRectangle(0, 0, m_proportion, barHeight, m_radius);
        }

        dc.SetFont(GetFont());
        auto textSize = dc.GetMultiLineTextExtent(wxString("000%"));
        dc.SetTextForeground(wxColour(144, 144, 144));
        auto pt = wxPoint();
        pt.x    = (size.x - textSize.x) / 2;
        pt.y    = (barHeight - textSize.y) / 2;

        auto text = wxString("");
        if (m_step < 10) {
            text = wxString::Format("%d", m_step);
        } else {
            text = wxString::Format("%d", m_step);
        }

        if (m_shownumber) {
            dc.DrawText(text + wxString("%"), pt);
        }
    }

    renderMarkers(dc, size, barHeight);
}

// Markers cut a gap into the bar; the hovered one also shows its label in a bubble under the bar.
void ProgressBar::renderMarkers(wxDC &dc, const wxSize &size, int barHeight)
{
    if (m_markers.empty() || size.x <= 0 || m_max <= 0)
        return;

    const wxColour background = GetBackgroundColour();
    const int      gapWidth   = std::max(2, FromDIP(2));
    for (const auto &marker : m_markers) {
        const int position = std::clamp(marker.m_position, 0, m_max);
        const int markerX  = size.x * position / m_max;
        if (markerX > 0 && markerX < size.x) {
            dc.SetPen(wxPen(background, 1));
            dc.SetBrush(wxBrush(background));
            dc.DrawRectangle(markerX - gapWidth / 2, 0, gapWidth, barHeight);
        }
    }

    for (size_t index = 0; index < m_markers.size(); ++index) {
        const auto &marker = m_markers[index];
        if (marker.m_label.empty() || static_cast<int>(index) != m_hoveredMarker)
            continue;

        const int position = std::clamp(marker.m_position, 0, m_max);
        const int markerX  = size.x * position / m_max;
        dc.SetFont(::Label::Body_11);
        const wxSize textSize     = dc.GetTextExtent(marker.m_label);
        const int    bubbleWidth  = std::min(size.x, textSize.x + FromDIP(20));
        const int    bubbleHeight = textSize.y + FromDIP(12);
        const int    bubbleY      = barHeight + FromDIP(5);
        const int    bubbleX      = std::clamp(markerX - bubbleWidth / 2, 0, std::max(0, size.x - bubbleWidth));
        const wxColour bubbleBackground(107, 107, 107);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(bubbleBackground));
        dc.DrawRoundedRectangle(bubbleX, bubbleY, bubbleWidth, bubbleHeight, FromDIP(5));
        const wxPoint pointer[] = {{markerX, barHeight + FromDIP(1)},
                                   {markerX - FromDIP(4), bubbleY + FromDIP(1)},
                                   {markerX + FromDIP(4), bubbleY + FromDIP(1)}};
        dc.DrawPolygon(3, pointer);
        dc.SetTextForeground(*wxWHITE);
        dc.DrawText(marker.m_label, bubbleX + (bubbleWidth - textSize.x) / 2,
                    bubbleY + (bubbleHeight - textSize.y) / 2);
        break;
    }
}


void ProgressBar::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
}

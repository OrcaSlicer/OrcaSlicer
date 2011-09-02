#include "StaticLine.hpp"
#include "Label.hpp"
#include "StateColor.hpp"

#include <wx/dcclient.h>
#include <wx/dcgraph.h>

BEGIN_EVENT_TABLE(StaticLine, wxWindow)

// catch paint events
EVT_PAINT(StaticLine::paintEvent)

END_EVENT_TABLE()

StaticLine::StaticLine(wxWindow *parent, bool vertical, const wxString &label, const wxString &icon)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , vertical(vertical)
{
    wxWindow::SetBackgroundColour(parent->GetBackgroundColour());
    this->lineColor = wxColour("#EEEEEE");
    DisableFocusFromKeyboard();
    SetFont(Label::Body_14);
    wxWindow::SetLabel(label);
    SetIcon(icon);
}

void StaticLine::SetLabel(const wxString& label)
{
    wxWindow::SetLabel(label);
    messureSize();
    Refresh();
}

void StaticLine::SetIcon(const wxString &icon)
{
    this->icon = icon.IsEmpty() ? ScalableBitmap() 
        : ScalableBitmap(this, icon.ToStdString(), 18);
    messureSize();
    Refresh();
}

void StaticLine::SetLineColour(wxColour color)
{
    this->lineColor = color;
}

void StaticLine::Rescale()
{
    if (this->icon.bmp().IsOk())
        this->icon.msw_rescale();
    messureSize();
}

void StaticLine::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxPaintDC dc(this);
    render(dc);
}

void StaticLine::messureSize()
{
    wxClientDC dc(this);
    wxSize textSize = dc.GetTextExtent(GetLabel());
    wxSize szContent = textSize;
    if (this->icon.bmp().IsOk()) {
        if (szContent.y > 0) {
            // BBS norrow size between text and icon
            szContent.x += 5;
        }
        wxSize szIcon = this->icon.GetBmpSize();
        szContent.x += szIcon.x;
        if (szIcon.y > szContent.y) szContent.y = szIcon.y;
    }
    if (vertical)
        szContent.y += 10;
    else
        szContent.x += 10;
    SetMinSize(szContent);
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void StaticLine::render(wxDC& dc)
{
    wxSize size = GetSize();
    wxSize textSize;
    auto   label = GetLabel();
    if (!label.IsEmpty()) textSize = dc.GetTextExtent(label);
    wxRect titleRect{{0, 0}, size};
    titleRect.height = wxMax(icon.GetBmpHeight(), textSize.GetHeight());
    int contentWidth = icon.GetBmpWidth() + ((icon.bmp().IsOk() && textSize.GetWidth() > 0) ? 5 : 0) +
                textSize.GetWidth();
    if (vertical) titleRect.Deflate((size.GetWidth() - contentWidth) / 2, 0);
    if (icon.bmp().IsOk()) {
        dc.DrawBitmap(icon.bmp(), {0, (size.y - icon.GetBmpHeight()) / 2});
        titleRect.x += icon.GetBmpWidth() + 5;
    }
    if (!label.IsEmpty()) {
        dc.SetTextForeground(StateColor::darkModeColorFor(GetForegroundColour()));
        dc.DrawText(label, titleRect.x, (size.GetHeight() - textSize.GetHeight()) / 2);
        titleRect.x += textSize.GetWidth() + 5;
    }
    dc.SetPen(wxPen(StateColor::darkModeColorFor(lineColor)));
    if (vertical) {
        size.x /= 2;
        if (titleRect.y > 0) titleRect.y += 5;
        dc.DrawLine(size.x, titleRect.y, size.x, size.y);
    } else {
        size.y /= 2;
        dc.DrawLine(titleRect.x, size.y, size.x, size.y);
    }
}

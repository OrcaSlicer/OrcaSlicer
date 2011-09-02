#include "RoundedRectangle.hpp"
#include "../wxExtensions.hpp"
#include <wx/dcgraph.h>
#include <wx/dcclient.h>

BEGIN_EVENT_TABLE(RoundedRectangle, wxWindow)
EVT_PAINT(RoundedRectangle::OnPaint)
END_EVENT_TABLE()

 RoundedRectangle::RoundedRectangle(wxWindow *parent, wxColour col, wxPoint pos, wxSize size, double radius, int type)
     : wxWindow(parent, wxID_ANY, pos, size, wxBORDER_NONE)
{
    SetBackgroundColour(wxColour(255,255,255));
    m_type   = type;
    m_color  = col;
    m_radius = radius;
}

void RoundedRectangle::OnPaint(wxPaintEvent &evt)
{
    //draw RoundedRectangle
    if (m_type == 0) {
        wxPaintDC dc(this);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_color));
        dc.DrawRoundedRectangle(0, 0, GetSize().GetWidth(), GetSize().GetHeight(), m_radius);
    }

    //draw RoundedRectangle only board
    if (m_type == 1) {
        wxPaintDC dc(this);
        dc.SetPen(m_color);
        dc.SetBrush(wxBrush(*wxTRANSPARENT_BRUSH));
        dc.DrawRoundedRectangle(0, 0, GetSize().GetWidth(), GetSize().GetHeight(), m_radius);
    }
}

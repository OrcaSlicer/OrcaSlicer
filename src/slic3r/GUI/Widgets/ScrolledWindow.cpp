// for scroll
#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include <wx/splitter.h>
#include "ScrolledWindow.hpp"
#include "Scrollbar.hpp"

ScrolledWindow::ScrolledWindow(wxWindow *parent, wxWindowID id, wxPoint position, wxSize size, long style, int marginWidth, int scrollbarWidth, int tipLength)
    : wxScrolled<wxWindow>(parent, id, position, size, style)
{
    bool bVertical   = (style & wxVSCROLL) != 0;
    bool bHorizontal = (style & wxHSCROLL) != 0;
    m_bothDirections = bVertical & bHorizontal;
    EnableScrolling(bHorizontal, bVertical);
    ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);

    m_rightScrollbar     = NULL;
    m_bottomScrollbar    = NULL;
    m_verticalSplitter   = NULL;
    m_horizontalSplitter = NULL;

    m_marginWidth        = marginWidth;

    wxSize hsSize = size;
    hsSize.SetWidth(hsSize.GetWidth() - marginWidth);
    hsSize.SetHeight(hsSize.GetHeight() - marginWidth);

    wxSize vsSize = size;
    vsSize.SetWidth(vsSize.GetWidth() - marginWidth);


    if (bVertical) {
        m_verticalSplitter = new wxWindow(this, -1, position, vsSize);
        m_userPanel        = new wxPanel(m_verticalSplitter, -1, wxPoint(0, 0), wxSize(size.GetWidth() - marginWidth, size.GetHeight()));
        m_scroll_win      = new wxWindow(m_verticalSplitter, -1, wxPoint(size.GetWidth() - marginWidth, 0), wxSize(marginWidth, size.GetHeight()));
        m_rightScrollbar   = new MyScrollbar(m_scroll_win, -1, wxPoint(0, 0), wxSize(scrollbarWidth, size.GetHeight()), this, wxVSCROLL, scrollbarWidth, tipLength);
    } else if (bHorizontal) {
        m_horizontalSplitter = new wxSplitterWindow(this, -1, position, hsSize);
        m_userPanel          = new wxPanel(m_horizontalSplitter, -1, wxPoint(0, 0), wxSize(size.GetWidth() - marginWidth, size.GetHeight() - marginWidth));
        m_userPanel->SetBackgroundColour(parent->GetBackgroundColour());

        m_bottomScrollbar = new MyScrollbar(m_horizontalSplitter, -1, wxPoint(0, 0), wxSize(size.GetWidth() - marginWidth, marginWidth), this, wxHSCROLL, scrollbarWidth,
                                            tipLength);
        m_horizontalSplitter->SplitHorizontally(m_userPanel, m_bottomScrollbar, -marginWidth);
        m_horizontalSplitter->SetSashInvisible();
    }

    SetTargetWindow(m_userPanel); // very very important line

    Bind(wxEVT_SIZE, &ScrolledWindow::OnSize, this);
    Bind(wxEVT_SCROLLWIN_TOP, &ScrolledWindow::OnScroll, this);
    Bind(wxEVT_SCROLLWIN_BOTTOM, &ScrolledWindow::OnScroll, this);
    Bind(wxEVT_SCROLLWIN_LINEUP, &ScrolledWindow::OnScroll, this);
    Bind(wxEVT_SCROLLWIN_LINEDOWN, &ScrolledWindow::OnScroll, this);
    Bind(wxEVT_SCROLLWIN_PAGEUP, &ScrolledWindow::OnScroll, this);
    Bind(wxEVT_SCROLLWIN_PAGEDOWN, &ScrolledWindow::OnScroll, this);
    Bind(wxEVT_SCROLLWIN_THUMBTRACK, &ScrolledWindow::OnScroll, this);
    Bind(wxEVT_SCROLLWIN_THUMBRELEASE, &ScrolledWindow::OnScroll, this);
    Bind(wxEVT_MOUSEWHEEL, &ScrolledWindow::OnMouseWheel, this);
}

void ScrolledWindow::OnMouseWheel(wxMouseEvent &event)
{
    m_rightScrollbar->GetEventHandler()->ProcessEvent(event);

    // int dMotion, actualDim;
    // if (event.GetWheelRotation() > 0)
    //    dMotion = -m_pixelsPerUnit;
    // else
    //    dMotion = m_pixelsPerUnit;
    // m_previousMouse += dMotion;
    // actualDim = m_actualDim;

    // if (m_direction == wxVSCROLL) {
    //    m_viewStartInPixels += (dMotion * m_virtualDim) / (float) actualDim + 0.5; // in pixels

    //    if (m_viewStartInPixels < 0)
    //        m_viewStartInPixels = 0;
    //    else if (m_viewStartInPixels + actualDim >= m_virtualDim)
    //        m_viewStartInPixels = m_virtualDim - actualDim;

    //    m_viewStartInScrollUnits = m_viewStartInPixels / (float) m_pixelsPerUnit + 0.5; // in scroll units
    //    m_scrolledWindow->Scroll(-1, m_viewStartInScrollUnits);                         // -1 means no change in this direction
    //    SetViewStart(m_viewStartInScrollUnits);
    //} else {
    //    dMotion = -dMotion;
    //    if (m_scrolledWindow->IsBothDirections()) actualDim -= m_marginWidth; // must take into account the right margin

    //    m_viewStartInPixels += (dMotion * m_virtualDim) / (float) actualDim + 0.5; // in pixels

    //    if (m_viewStartInPixels < 0)
    //        m_viewStartInPixels = 0;
    //    else if (m_viewStartInPixels + actualDim >= m_virtualDim)
    //        m_viewStartInPixels = m_virtualDim - actualDim;

    //    m_viewStartInScrollUnits = m_viewStartInPixels / (float) m_pixelsPerUnit + 0.5; // in scroll units
    //    m_scrolledWindow->Scroll(m_viewStartInScrollUnits, -1);                         // -1 means no change in this direction
    //    SetViewStart(m_viewStartInScrollUnits);
    //}
    // Refresh();
    // Update();
}

void ScrolledWindow::SetTipColor(wxColour color)
{
    if (m_rightScrollbar) m_rightScrollbar->SetTipColor(color);
    if (m_bottomScrollbar) m_bottomScrollbar->SetTipColor(color);
}

void ScrolledWindow::Refresh()
{
    // m_rightScrollbar->SetViewStart(0);
    // m_rightScrollbar->Refresh();
    // m_rightScrollbar->Update();
    // m_userPanel->Refresh();
    // m_bottomScrollbar->SetViewStart(0);
    // m_rightScrollbar->Refresh();
    // m_bottomScrollbar->Refresh();
}

void ScrolledWindow::SetBackgroundColour(wxColour color)
{
    wxWindow::SetBackgroundColour(color); 
    m_verticalSplitter->SetBackgroundColour(color); 
    m_userPanel->SetBackgroundColour(color);
    m_scroll_win->SetBackgroundColour(color);
}

void ScrolledWindow::SetMarginColor(wxColour color)
{
    if (m_rightScrollbar) m_rightScrollbar->SetMarginColor(color);
    if (m_bottomScrollbar) m_bottomScrollbar->SetMarginColor(color);
}

void ScrolledWindow::SetScrollbarColor(wxColour color)
{
    if (m_rightScrollbar) m_rightScrollbar->SetScrollbarColor(color);
    if (m_bottomScrollbar) m_bottomScrollbar->SetScrollbarColor(color);
}

void ScrolledWindow::SetScrollbarTip(int len)
{
    if (m_rightScrollbar) m_rightScrollbar->SetScrollbarTip(len);
    if (m_bottomScrollbar) m_bottomScrollbar->SetScrollbarTip(len);
}

void ScrolledWindow::SetVirtualSize(int x, int y) { SetScrollbars(1, 1, x, y); }

void ScrolledWindow::SetVirtualSize(wxSize &size) { SetScrollbars(1, 1, size.GetWidth(), size.GetHeight()); }

void ScrolledWindow::SetScrollbars(int pixelsPerUnitX, int pixelsPerUnitY, int noUnitsX, int noUnitsY, int xPos, int yPos, bool noRefresh)
{
    wxScrolled<wxWindow>::SetScrollbars(pixelsPerUnitX, pixelsPerUnitY, noUnitsX, noUnitsY, xPos, yPos, noRefresh);
    wxScrolled<wxWindow>::SetVirtualSize(pixelsPerUnitX * noUnitsX, pixelsPerUnitY * noUnitsY); // So that GetVirtualSize gives good values
    if (m_rightScrollbar) m_rightScrollbar->SetVirtualDim(pixelsPerUnitY, noUnitsY);
    if (m_bottomScrollbar) m_bottomScrollbar->SetVirtualDim(pixelsPerUnitX, noUnitsX);
}

void ScrolledWindow::OnSize(wxSizeEvent &event)
{
    int startX, startY, virtX, virtY, clientW, clientH;

    if (!m_verticalSplitter && !m_horizontalSplitter) return;

    GetViewStart(&startX, &startY);
    GetVirtualSize(&virtX, &virtY);
    GetClientSize(&clientW, &clientH);

    // trace / log in the output / console window
    // wxString str; str.sprintf("Actual=(%d,%d). Virtual=(%d,%d)\n", clientW, clientH, virtX, virtY); OutputDebugString(str);
    if (m_verticalSplitter) m_verticalSplitter->SetSize(clientW, clientH);
    if (m_horizontalSplitter) m_horizontalSplitter->SetSize(clientW, clientH);

    if (m_rightScrollbar) {
        if (clientH >= virtY) // hide the scrollbar by enlarging the user panel
        {
            // m_verticalSplitter->SetSashPosition(clientW);
            m_userPanel->SetSize(GetClientSize());
            m_rightScrollbar->SetSize(0, clientH);
            m_userPanel->Refresh();
            m_userPanel->Update();
        } else {
            // m_verticalSplitter->SetSashPosition(clientW - m_marginWidth); // resize the splitter panes
        }
        m_rightScrollbar->SetViewStart(startY);
        m_rightScrollbar->Refresh();
        m_rightScrollbar->Update(); // we want to repaint
    }
    if (m_bottomScrollbar) {
        if (clientW >= virtX) // hide the scrollbar by enlarging the user panel
        {
            m_horizontalSplitter->SetSashPosition(clientH); // we don't need horizontal scrollbar
            if (m_rightScrollbar)
                m_userPanel->SetSize(clientW - m_marginWidth, clientH); // don't hide the vertical scrollbar
            else
                m_userPanel->SetSize(GetClientSize());

            m_bottomScrollbar->SetSize(clientW, 0);
            m_userPanel->Refresh();
            m_userPanel->Update();
        } else {
            m_horizontalSplitter->SetSashPosition(clientH - m_marginWidth); // resize the splitter panes
        }
        m_bottomScrollbar->SetViewStart(startX);
        m_bottomScrollbar->Refresh();
        m_bottomScrollbar->Update(); // we want to repaint even if the panel gets smaller
    }
    Layout();
    AdjustScrollbars();
}

void ScrolledWindow::OnScroll(wxScrollWinEvent &event)
{
    int startX, startY;
    GetViewStart(&startX, &startY);
    if (m_rightScrollbar) {
        m_rightScrollbar->SetViewStart(startY);
        m_rightScrollbar->Refresh();
        m_rightScrollbar->Update();
    }
    if (m_bottomScrollbar) {
        m_bottomScrollbar->SetViewStart(startX);
        m_bottomScrollbar->Refresh();
        m_bottomScrollbar->Update();
    }
    //	event.Skip();	// then do the regular process // uncomment if you want to use the mouse and the trackpad in the scrolled panel
}

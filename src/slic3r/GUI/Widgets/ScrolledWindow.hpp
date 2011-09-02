#pragma once
#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include <wx/splitter.h>
#include "Scrollbar.hpp"

class MyScrollbar;

class ScrolledWindow : public wxScrolled<wxWindow>
{
public:
    ScrolledWindow(wxWindow *parent, wxWindowID id, wxPoint position, wxSize size, long style, int marginWidth = 0, int scrollbarWidth = 4, int tipLength = 0);
    void OnMouseWheel(wxMouseEvent &event);
    void SetTipColor(wxColour color);
    void Refresh();
    void SetBackgroundColour(wxColour color);

    void         SetMarginColor(wxColour color);
    void         SetScrollbarColor(wxColour color);
    void         SetScrollbarTip(int len);
    virtual void SetVirtualSize(int x, int y);
    virtual void SetVirtualSize(wxSize &size);
    wxPanel *    GetPanel() { return m_userPanel; }
    // wxSplitterWindow* GetVerticalSplitter() { return m_verticalSplitter; }
    // wxSplitterWindow* GetHorizontalSplitter() { return m_horizontalSplitter; }
    bool         IsBothDirections() { return m_bothDirections; }
    virtual void SetScrollbars(int pixelsPerUnitX, int pixelsPerUnitY, int noUnitsX, int noUnitsY, int xPos = 0, int yPos = 0, bool noRefresh = false);

private:
    wxPanel *    m_userPanel; // the panel targeted by the scrolled window
    wxWindow *   m_scroll_win;
    MyScrollbar *m_rightScrollbar;
    MyScrollbar *m_bottomScrollbar;
    // wxSplitterWindow* m_verticalSplitter;
    wxWindow *        m_verticalSplitter;
    wxSplitterWindow *m_horizontalSplitter;
    int               m_marginWidth;
    bool              m_bothDirections;

    void OnSize(wxSizeEvent &WXUNUSED(event));
    void OnScroll(wxScrollWinEvent &event);
};

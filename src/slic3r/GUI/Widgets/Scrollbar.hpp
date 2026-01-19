#pragma once
#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#ifndef MAX
#define MAX(a,b)	((a)>(b)?(a):(b))
#endif

enum {BEFORE_SCROLLBAR, ON_SCROLLBAR, AFTER_SCROLLBAR, NOWHERE};
#define SCROLL_D_MOTION 4

class ScrolledWindow;

class MyScrollbar : public wxPanel
{
public:
	MyScrollbar(wxWindow *parent, wxWindowID id, wxPoint position, wxSize size, ScrolledWindow* scrolledWindow, long direction, int scrollbarWidth, int tipLength = 0);
	void SetViewStart(int start);
	void SetTipColor(wxColour color);
	void SetMarginColor(wxColour color);
	void SetScrollbarColor(wxColour color);
	void SetScrollbarTip(int len);
	void SetVirtualDim(int pixelsPerUnit, int noUnits);

private:
	long m_direction;
	int m_virtualDim;
	int m_actualDim;
	int m_pixelsPerUnit;
	int m_viewStartInPixels;
	int m_viewStartInScrollUnits;
	int m_scrollbarWidth;
	int m_marginWidth;
	int m_previousMouse;
	int m_mouseLocation;
	int m_tipLength;
	int m_minSliderSize;
	wxColour m_tipColor;
	wxColour m_marginColor;
	wxColour m_scrollbarColor;
	ScrolledWindow* m_scrolledWindow;	// the scrolledWindow whose the scrollbar is controlling

	void OnPaint(wxPaintEvent& event);
	void OnSize(wxSizeEvent& WXUNUSED(event));
	void OnEraseBackground(wxEraseEvent & event);
	void OnMouseLeftDown(wxMouseEvent &event);
	void OnMouseLeftUp(wxMouseEvent &event);
	void OnMouseMove(wxMouseEvent &event);
	void OnMouseWheel(wxMouseEvent &event);
};

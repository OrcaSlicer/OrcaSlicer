#ifndef slic3r_GUI_ImageSwitchButton_hpp_
#define slic3r_GUI_ImageSwitchButton_hpp_

#include "../wxExtensions.hpp"
#include "StateColor.hpp"
#include "StateHandler.hpp"
#include "Button.hpp"


#include <wx/tglbtn.h>

class ImageSwitchButton : public StaticBox
{
public:
    ImageSwitchButton(wxWindow *parent, ScalableBitmap &img_on, ScalableBitmap &img_off, long style = 0);

	void SetLabels(wxString const & lbl_on, wxString const & lbl_off);
    void SetImages(ScalableBitmap &img_on, ScalableBitmap &img_off);
    void SetTextColor(StateColor const &color);
	void SetValue(bool value);
	void SetPadding(int padding);

	bool GetValue() { return m_on_off; }
	void Rescale();

private:
    void messureSize();
    void paintEvent(wxPaintEvent &evt);
	void render(wxDC& dc);
    void mouseDown(wxMouseEvent &event);
    void mouseReleased(wxMouseEvent &event);
    void mouseEnterWindow(wxMouseEvent &event);
    void mouseLeaveWindow(wxMouseEvent &event);
    void sendButtonEvent();

	DECLARE_EVENT_TABLE()

private:
    ScalableBitmap m_on;
    ScalableBitmap m_off;
    bool           m_on_off;
	int            m_padding;	// size between icon and text
    bool           pressedDown = false;
	bool		   hover = false;

	wxSize	     textSize;
	wxSize       minSize;

	wxString labels[2];
    StateColor   text_color;
};

class FanSwitchButton : public StaticBox
{
public:
    FanSwitchButton(wxWindow* parent, ScalableBitmap& img_on, ScalableBitmap& img_off, long style = 0);
    void SetLabels(wxString const& lbl_on, wxString const& lbl_off);
    void SetImages(ScalableBitmap& img_on, ScalableBitmap& img_off);
    void SetTextColor(StateColor const& color);
    void SetValue(bool value);
    void SetPadding(int padding);

    bool GetValue() { return m_on_off; }
    void Rescale();
    void setFanValue(int val);

    void UseTextFan();
    void UseTextAirCondition();

private:
    void messureSize();
    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void mouseDown(wxMouseEvent& event);
    void mouseReleased(wxMouseEvent& event);
    void mouseEnterWindow(wxMouseEvent& event);
    void mouseLeaveWindow(wxMouseEvent& event);
    void sendButtonEvent();

    void SetText(const wxString &text);

    DECLARE_EVENT_TABLE()

private:
    ScalableBitmap m_on;
    ScalableBitmap m_off;
    bool           m_on_off;
    int            m_padding;	// size between icon and text
    bool           pressedDown = false;
    bool		   hover = false;

    wxSize	     textSize;
    wxSize       minSize;
    int          m_speed;

    wxString labels[2];

    wxString     m_text;
    StateColor   text_color;
};

#endif // !slic3r_GUI_SwitchButton_hpp_

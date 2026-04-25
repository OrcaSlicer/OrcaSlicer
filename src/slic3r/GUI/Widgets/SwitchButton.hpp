#ifndef slic3r_GUI_SwitchButton_hpp_
#define slic3r_GUI_SwitchButton_hpp_

#include "../wxExtensions.hpp"
#include "StateColor.hpp"
#include "StaticBox.hpp"

#include <wx/tglbtn.h>

wxDECLARE_EVENT(wxCUSTOMEVT_SWITCH_POS, wxCommandEvent);

class SwitchButton : public wxBitmapToggleButton
{
public:
	SwitchButton(wxWindow * parent = NULL, wxWindowID id = wxID_ANY);

public:
	void SetLabels(wxString const & lbl_on, wxString const & lbl_off);

	void SetTextColor(StateColor const &color);

	void SetTextColor2(StateColor const &color);

    void SetTrackColor(StateColor const &color);

	void SetThumbColor(StateColor const &color);

	void SetValue(bool value) override;

	void Rescale();

    bool SetBackgroundColour(const wxColour& colour) override;

private:
	void update();

private:
	ScalableBitmap m_on;
	ScalableBitmap m_off;

	wxString labels[2];
    StateColor   text_color;
    StateColor   text_color2;
	StateColor   track_color;
	StateColor   thumb_color;
};

class ModeSwitchButton : public StaticBox
{
public:
    ModeSwitchButton(wxWindow* parent = nullptr, wxWindowID id = wxID_ANY);

    int  GetSelection() const { return m_selection; }
    void SetSelection(int selection);
    void SelectAndNotify(int selection);

    void Rescale();
    void msw_rescale() { Rescale(); }

    bool Enable(bool enable = true) override;

protected:
    void doRender(wxDC& dc) override;

private:
    void mouseDown(wxMouseEvent& event);
    void mouseReleased(wxMouseEvent& event);
    void mouseCaptureLost(wxMouseCaptureLostEvent& event);
    int  hit_test_selection(const wxPoint& point) const;
    wxRect thumb_rect_for(int selection) const;
    void update_tooltip();

private:
    int      m_selection { 0 };
    bool     m_pressed   { false };
    wxString m_tooltips[3];
};

class SwitchBoard : public wxWindow
{
public:
    SwitchBoard(wxWindow *parent = NULL, wxString leftL = "", wxString right = "", wxSize size = wxDefaultSize);
    wxString leftLabel;
    wxString rightLabel;

	void updateState(wxString target);

	bool switch_left{false};
    bool switch_right{false};
    bool is_enable {true};

    void* client_data = nullptr;/*MachineObject* in StatusPanel*/

public:
    bool Enable(bool enable = true) override;
    bool Disable() { return Enable(false); }
    bool IsEnabled(){return is_enable;};

    void  SetClientData(void* data) { client_data = data; };
    void* GetClientData() { return client_data; };

    void SetAutoDisableWhenSwitch() { auto_disable_when_switch = true; };

protected:
    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void doRender(wxDC& dc);
    void on_left_down(wxMouseEvent& evt);

private:
    bool auto_disable_when_switch = false;
};

#endif // !slic3r_GUI_SwitchButton_hpp_

#ifndef slic3r_GUI_SwitchButton_hpp_
#define slic3r_GUI_SwitchButton_hpp_

#include "../wxExtensions.hpp"
#include "StateColor.hpp"

#include <wx/tglbtn.h>
#include "Label.hpp"
#include "Button.hpp"

wxDECLARE_EVENT(wxCUSTOMEVT_SWITCH_POS, wxCommandEvent);
wxDECLARE_EVENT(wxCUSTOMEVT_MULTISWITCH_SELECTION, wxCommandEvent);
wxDECLARE_EVENT(wxEXPAND_LEFT_DOWN, wxCommandEvent);

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
    void Enable();
    void Disable();
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

class CustomToggleButton : public wxWindow {
public:
    CustomToggleButton(wxWindow* parent, const wxString& label,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize);


    void SetLabel(const wxString& label) override;

    void SetSelectedIcon(const wxString& iconPath);
    void SetUnSelectedIcon(const wxString& iconPath);

    void SetIsSelected(bool selected);
    bool IsSelected() const;

    void set_primary_colour(wxColour col) {m_primary_colour = col;};
    void set_secondary_colour(wxColour col) {m_secondary_colour = col;};

private:
    void OnPaint(wxPaintEvent& event);
    void render(wxDC& dc);
    void doRender(wxDC& dc);
    void OnSize(wxSizeEvent& event);

    void on_left_down(wxMouseEvent& e);

    wxString m_label;
    wxBitmap m_selected_icon;
    wxBitmap m_unselected_icon;
    wxColour m_primary_colour{wxColour("#00AE42")};
    wxColour m_secondary_colour{wxColour("#DEF5E7")};

    bool m_isSelected;
};

class ExpandButton : public wxWindow {
public:
    ExpandButton(wxWindow* parent,
        std::string bmp,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize);
    
    void update_bitmap(std::string bmp);
    void msw_rescale();
private:
    std::string m_bmp_str;
    wxBitmap m_bmp;
    void OnPaint(wxPaintEvent& event);
    void render(wxDC& dc);
    void doRender(wxDC& dc);
};

class ExpandButtonHolder : public wxPanel {
public:
    ExpandButtonHolder(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize);

    //wxBoxSizer* expand_sizer{nullptr};
    wxBoxSizer* hsizer{nullptr};
    wxBoxSizer* vsizer{nullptr};
    int GetAvailable();
    void addExpandButton(wxWindowID id, std::string img);
    void ShowExpandButton(wxWindowID id, bool show);
    void updateExpandButtonBitmap(wxWindowID id, std::string bitmap);
    void EnableExpandButton(wxWindowID id, bool enb);

    void msw_rescale();
private:
    void OnPaint(wxPaintEvent& event);
    void render(wxDC& dc);
    void doRender(wxDC& dc);
};

class MultiSwitchButton : public StaticBox
{
    std::vector<Button *> btns;
    wxBoxSizer           *sizer = nullptr;
    int                   sel   = -1;

public:
    MultiSwitchButton(wxWindow *parent = nullptr, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize, long style = 0);

    ~MultiSwitchButton();

public:
    int AppendOption(const wxString &option, void *clientData = nullptr);

    void SetOptions(const std::vector<wxString> &options);

    void DeleteAllOptions();

    unsigned int GetCount() const;

    int      GetSelection() const;
    void     SetSelection(int index);
    wxString GetSelectedText() const;

    Button*  GetButton(unsigned int index) const
    {
        return index >= 0 && index < btns.size() ? btns[index] : nullptr;
    }

    wxString GetOptionText(unsigned int index) const;
    void     SetOptionText(unsigned int index, const wxString &text);

    void *GetOptionData(unsigned int index) const;
    void  SetOptionData(unsigned int index, void *clientData);

    void SetBackgroundColor(const StateColor &color);
    void SetTextColor(const StateColor &color);
    void SetButtonTextColor(int index, const StateColor &color)
    {
        if (index >= btns.size()) return;

        btns[index]->SetTextColor(color);
        btns[index]->Refresh();
    }
    void SetButtonCornerRadius(double radius);
    void SetButtonPadding(const wxSize &padding);

    void Rescale();

protected:
    void button_clicked(wxCommandEvent &event);
    void update_button_styles();

    bool send_selection_event();

private:
    StateColor m_bg_color;
    StateColor m_bg_color_grayed;
    StateColor m_text_color;
    StateColor m_text_color_grayed;
    double     m_button_radius;
    wxSize     m_button_padding;
};

#endif // !slic3r_GUI_SwitchButton_hpp_

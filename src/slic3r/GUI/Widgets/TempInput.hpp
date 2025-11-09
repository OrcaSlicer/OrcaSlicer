#ifndef slic3r_GUI_TempInput_hpp_
#define slic3r_GUI_TempInput_hpp_

#include "../wxExtensions.hpp"
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include "StaticBox.hpp"

#include <unordered_set>

wxDECLARE_EVENT(wxCUSTOMEVT_SET_TEMP_FINISH, wxCommandEvent);

enum TempInputType {
    TEMP_OF_MAIN_NOZZLE_TYPE,
    TEMP_OF_DEPUTY_NOZZLE_TYPE,
    TEMP_OF_NORMAL_TYPE
};

class TempInput : public wxNavigationEnabled<StaticBox>
{
    bool   hover;

    bool           m_read_only{false};
    bool           m_on_changing {false};
    wxSize         labelSize;
    ScalableBitmap normal_icon;
    ScalableBitmap actice_icon;
    ScalableBitmap degree_icon;
    ScalableBitmap round_scale_hint_icon;/*the size hint of icon, use to compute size*/

    StateColor   label_color;
    StateColor   text_color;

    wxTextCtrl *  text_ctrl;
    wxStaticText *warning_text;

    int  max_temp     = 0;
    int  min_temp     = 0;
    std::unordered_set<int> additional_temps;
    bool warning_mode = false;
    TempInputType m_input_type;

    int              padding_left    = 0;
    static const int TempInputWidth  = 200;
    static const int TempInputHeight = 50;
public:
    enum WarningType {
        WARNING_TOO_HIGH,
        WARNING_TOO_LOW,
        WARNING_UNKNOWN,
    };

    TempInput();

    TempInput(wxWindow *     parent,
              int            type,
              wxString       text,
              TempInputType  input_type,
              wxString       label       = "",
              wxString       normal_icon = "",
              wxString       actice_icon = "",
              const wxPoint &pos         = wxDefaultPosition,
              const wxSize & size        = wxDefaultSize,
              long           style       = 0);

public:
    void Create(wxWindow *     parent,
                wxString       text,
                wxString       label       = "",
                wxString       normal_icon = "",
                wxString       actice_icon = "",
                const wxPoint &pos         = wxDefaultPosition,
                const wxSize & size        = wxDefaultSize,
                long           style       = 0);


    wxPopupTransientWindow *wdialog{nullptr};
    int  temp_type;
    bool actice = false;
    wxString                currentTemp;


    wxString erasePending(wxString &str);

    void SetTagTemp(int temp);
    void SetTagTemp(wxString temp);

    void SetCurrTemp(int temp);
    void SetCurrTemp(wxString temp);
    void SetCurrType(TempInputType type);
    TempInputType GetCurrType(){return m_input_type;};

    bool AllisNum(std::string str);
    void SetFinish();
    void Warning(bool warn, WarningType type = WARNING_UNKNOWN);
    void SetIconActive();
    void SetIconNormal();

   void SetReadOnly(bool ro) { m_read_only = ro; }

    void SetMaxTemp(int temp);
    void SetMinTemp(int temp);
    void AddTemp(int temp) { additional_temps.insert(temp); };

    int GetType() { return temp_type; }

    wxString GetTagTemp() { return text_ctrl->GetValue(); }
    wxString GetCurrTemp() { return GetLabel(); }
    int get_max_temp() { return max_temp; }
    void SetLabel(const wxString &label);

    void SetTextColor(StateColor const &color);

    void SetLabelColor(StateColor const &color);

    virtual void Rescale();

    virtual bool Enable(bool enable = true) override;

    virtual void SetMinSize(const wxSize &size) override;

    wxTextCtrl *GetTextCtrl() { return text_ctrl; }

    wxTextCtrl const *GetTextCtrl() const { return text_ctrl; }

    bool  IsOnChanging() const { return m_on_changing; }
    void  SetOnChanging() { m_on_changing = true; }
    void  ReSetOnChanging() { m_on_changing = false; }

protected:
    virtual void DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);

    void DoSetToolTipText(wxString const &tip) override;

private:
    void ResetWaringDlg();
    bool CheckIsValidVal(bool show_warning);

    void paintEvent(wxPaintEvent &evt);

    void render(wxDC &dc);

	void messureMiniSize();
    void messureSize();

    // some useful events
    void mouseMoved(wxMouseEvent &event);
    void mouseWheelMoved(wxMouseEvent &event);
    void mouseEnterWindow(wxMouseEvent &event);
    void mouseLeaveWindow(wxMouseEvent &event);
    void keyPressed(wxKeyEvent &event);
    void keyReleased(wxKeyEvent &event);

    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_TempInput_hpp_

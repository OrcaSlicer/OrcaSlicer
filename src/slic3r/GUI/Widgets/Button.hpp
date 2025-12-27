#ifndef slic3r_GUI_Button_hpp_
#define slic3r_GUI_Button_hpp_

#include "../wxExtensions.hpp"
#include "StaticBox.hpp"

class ButtonProps
{
public:
    static int ChoiceButtonGap(){return 10;};
    static int WindowButtonGap(){return 10;};
};

enum class ButtonStyle{
    Regular,
    Confirm,
    Alert,
    Disabled,
};

enum class ButtonType{
    Compact  , // Font10  FullyRounded  For spaces with less areas
    Window   , // Font12  FullyRounded  For regular buttons in windows and not related with parameter boxes
    Choice   , // Font14  Semi-Rounded  For dialog/window choice buttons
    Parameter, // Font14  Semi-Rounded  For buttons that near parameter boxes
    Expanded , // Font14  Semi-Rounded  For full length buttons. ex. buttons in static box
};

class wxTipWindow;
class Button : public StaticBox
{
    wxRect textSize;
    wxSize minSize; // set by outer
    wxSize paddingSize;
    ScalableBitmap active_icon;
    ScalableBitmap inactive_icon;

    StateColor   text_color;

    bool pressedDown = false;
    bool m_selected  = true;
    bool canFocus  = true;
    bool isCenter    = true;
    bool vertical    = false;

    wxTipWindow* tipWindow = nullptr;

    static const int buttonWidth = 200;
    static const int buttonHeight = 50;

public:
    Button();

    Button(wxWindow* parent, wxString text, wxString icon = "", long style = 0, int iconSize = 0, wxWindowID btn_id = wxID_ANY);

    bool Create(wxWindow* parent, wxString text, wxString icon = "", long style = 0, int iconSize = 0, wxWindowID btn_id = wxID_ANY);

    void SetLabel(const wxString& label) override;

    bool SetFont(const wxFont& font) override;

    void SetIcon(const wxString& icon);

    void SetInactiveIcon(const wxString& icon);

    void SetMinSize(const wxSize& size) override;
    void SetMaxSize(const wxSize& size) override;

    void SetPaddingSize(const wxSize& size);

    void SetStyle(const ButtonStyle style /*= ButtonStyle::Regular*/, const ButtonType type /*= ButtonType::None*/);

    void SetTextColor(StateColor const &color);

    void SetTextColorNormal(wxColor const &color);

    void SetSelected(bool selected = true) { m_selected = selected; }

    bool Enable(bool enable = true) override;
    void EnableTooltipEvenDisabled();// The tip will be shown even if the button is disabled

    void SetCanFocus(bool canFocus) override;

    void SetValue(bool state);

    bool GetValue() const;

    void SetCenter(bool isCenter);

    void SetVertical(bool vertical = true);

    void Rescale();

protected:
#ifdef __WIN32__
    WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

    bool AcceptsFocus() const override;

private:
    bool m_has_style = false;
    ButtonStyle m_style;
    ButtonType  m_type;

    void paintEvent(wxPaintEvent& evt);

    void render(wxDC& dc);

    void messureSize();

    // some useful events
    void mouseDown(wxMouseEvent& event);
    void mouseReleased(wxMouseEvent& event);
    void mouseCaptureLost(wxMouseCaptureLostEvent &event);
    void keyDownUp(wxKeyEvent &event);

    // 
    void sendButtonEvent();

    // parent motion
    void OnParentMotion(wxMouseEvent& event);
    void OnParentLeave(wxMouseEvent& event);

    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_Button_hpp_

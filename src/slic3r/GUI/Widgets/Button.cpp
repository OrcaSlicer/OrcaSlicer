#include "Button.hpp"
#include "Label.hpp"

#include <wx/dcgraph.h>
#include <wx/tipwin.h>
#ifdef __APPLE__
#include "libslic3r/MacUtils.hpp"
#endif
BEGIN_EVENT_TABLE(Button, StaticBox)

EVT_LEFT_DOWN(Button::mouseDown)
EVT_LEFT_UP(Button::mouseReleased)
EVT_MOUSE_CAPTURE_LOST(Button::mouseCaptureLost)
EVT_KEY_DOWN(Button::keyDownUp)
EVT_KEY_UP(Button::keyDownUp)

// catch paint events
EVT_PAINT(Button::paintEvent)

END_EVENT_TABLE()

/*
 * Called by the system of by wxWidgets when the panel needs
 * to be redrawn. You can also trigger this call by
 * calling Refresh()/Update().
 */

Button::Button()
    : paddingSize(10, 8)
{
    background_color = StateColor(
        std::make_pair(0xF0F0F1, (int) StateColor::Disabled),
        std::make_pair(0x52c7b8, (int) StateColor::Hovered | StateColor::Checked),
        std::make_pair(0x009688, (int) StateColor::Checked),
        std::make_pair(*wxLIGHT_GREY, (int) StateColor::Hovered),
        std::make_pair(*wxWHITE, (int) StateColor::Normal));
    text_color       = StateColor(
        std::make_pair(*wxLIGHT_GREY, (int) StateColor::Disabled),
        std::make_pair(*wxBLACK, (int) StateColor::Normal));
}

Button::Button(wxWindow* parent, wxString text, wxString icon, long style, int iconSize, wxWindowID btn_id)
    : Button()
{
    Create(parent, text, icon, style, iconSize, btn_id);
}

bool Button::Create(wxWindow* parent, wxString text, wxString icon, long style, int iconSize, wxWindowID btn_id)
{
    StaticBox::Create(parent, btn_id, wxDefaultPosition, wxDefaultSize, style);
    state_handler.attach({&text_color});
    state_handler.update_binds();
    //BBS set default font
    SetFont(Label::Body_14);
    wxWindow::SetLabel(text);
    if (!icon.IsEmpty()) {
        //BBS set button icon default size to 20
        this->active_icon = ScalableBitmap(this, icon.ToStdString(), iconSize > 0 ? iconSize : 20);
    }
    messureSize();
    return true;
}

void Button::SetLabel(const wxString& label)
{
    if (label != wxWindow::GetLabel()) {
        wxWindow::SetLabel(label);
        messureSize();
        Refresh();
    }
}

bool Button::SetFont(const wxFont& font)
{
    wxWindow::SetFont(font);
    messureSize();
    Refresh();
    return true;
}

void Button::SetIcon(const wxString& icon)
{
    auto tmpBitmap = ScalableBitmap(this, icon.ToStdString(), this->active_icon.px_cnt());
    if (!icon.IsEmpty()) {
        //BBS set button icon default size to 20
        if (!tmpBitmap.bmp().IsSameAs(this->active_icon.bmp())) {
            this->active_icon = tmpBitmap;
            Refresh();
        }
    }
    else
    {
        this->active_icon = ScalableBitmap();
        Refresh();
    }
}

void Button::SetInactiveIcon(const wxString &icon)
{
    if (!icon.IsEmpty()) {
        // BBS set button icon default size to 20
        this->inactive_icon = ScalableBitmap(this, icon.ToStdString(), this->active_icon.px_cnt());
    } else {
        this->inactive_icon = ScalableBitmap();
    }
    Refresh();
}

void Button::SetMinSize(const wxSize& size)
{
    minSize = size;
    messureSize();
}

void Button::SetMaxSize(const wxSize& size)
{
    wxWindow::SetMaxSize(size);
    messureSize();
}

void Button::SetPaddingSize(const wxSize& size)
{
    paddingSize = size;
    messureSize();
}

void Button::SetTextColor(StateColor const& color)
{
    text_color = color;
    state_handler.update_binds();
    Refresh();
}

void Button::SetTextColorNormal(wxColor const &color)
{
    text_color.setColorForStates(color, 0);
    Refresh();
}

bool Button::Enable(bool enable)
{
    bool result = wxWindow::Enable(enable);
    if (result) {
        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);
    }
    return result;
}

void Button::SetCanFocus(bool canFocus) {
    StaticBox::SetCanFocus(canFocus);
    this->canFocus = canFocus;
}

void Button::SetValue(bool state)
{
    if (GetValue() == state) return;
    state_handler.set_state(state ? StateHandler::Checked : 0, StateHandler::Checked);
}

bool Button::GetValue() const { return state_handler.states() & StateHandler::Checked; }

void Button::SetCenter(bool isCenter)
{
    this->isCenter = isCenter; }

void Button::SetVertical(bool vertical)
{
    this->vertical = vertical;
    messureSize();
}

//                           Background                                             Foreground                       Border on focus
// Button Colors             0-Disabled 1-Pressed  2-Hover    3-Normal   4-Enabled  5-Disabled 6-Normal   7-Hover    8-Dark     9-Light
wxString btn_regular[10]  = {"#DFDFDF", "#DFDFDF", "#D4D4D4", "#DFDFDF", "#DFDFDF", "#6B6A6A", "#262E30", "#262E30", "#009688", "#009688"};
wxString btn_confirm[10]  = {"#DFDFDF", "#009688", "#26A69A", "#009688", "#009688", "#6B6A6A", "#FEFEFE", "#FEFEFE", "#22bfb0", "#00FFD4"};
wxString btn_alert[10]    = {"#DFDFDF", "#DFDFDF", "#E14747", "#DFDFDF", "#DFDFDF", "#6B6A6A", "#262E30", "#FFFFFD", "#009688", "#009688"};
wxString btn_disabled[10] = {"#DFDFDF", "#DFDFDF", "#DFDFDF", "#DFDFDF", "#DFDFDF", "#6B6A6A", "#6B6A6A", "#262E30", "#DFDFDF", "#DFDFDF"};

void Button::SetStyle(const ButtonStyle style, const ButtonType type)
{
    if      (type == ButtonType::Compact) {
        this->SetPaddingSize(FromDIP(wxSize(8,3)));
        this->SetCornerRadius(this->FromDIP(8));
        this->SetFont(Label::Body_10);
    }
    else if (type == ButtonType::Window) {
        this->SetSize(FromDIP(wxSize(58,24)));
        this->SetMinSize(FromDIP(wxSize(58,24)));
        this->SetCornerRadius(this->FromDIP(12));
        this->SetFont(Label::Body_12);
    }
    else if (type == ButtonType::Choice) {
        this->SetMinSize(FromDIP(wxSize(100,32)));
        this->SetPaddingSize(FromDIP(wxSize(12,8)));
        this->SetCornerRadius(this->FromDIP(4));
        this->SetFont(Label::Body_14);
    }
    else if (type == ButtonType::Parameter) {
        this->SetMinSize(FromDIP(wxSize(120,26)));
        this->SetSize(FromDIP(wxSize(120,26)));
        this->SetCornerRadius(this->FromDIP(4));
        this->SetFont(Label::Body_14);
    }
    else if (type == ButtonType::Expanded) {
        this->SetMinSize(FromDIP(wxSize(-1,32)));
        this->SetPaddingSize(FromDIP(wxSize(12,8)));
        this->SetCornerRadius(this->FromDIP(4));
        this->SetFont(Label::Body_14);
    }

    this->SetBorderWidth(this->FromDIP(1));

    bool is_dark = StateColor::darkModeColorFor("#FFFFFF") != wxColour("#FFFFFF");

    auto clr_arr = style == ButtonStyle::Regular  ? btn_regular  :
                   style == ButtonStyle::Confirm  ? btn_confirm  :
                   style == ButtonStyle::Alert    ? btn_alert    :
                   style == ButtonStyle::Disabled ? btn_disabled :
                                                    btn_regular  ;

    auto bg_color = StateColor(
        std::pair(wxColour(clr_arr[0]), (int)StateColor::Disabled),
        std::pair(wxColour(clr_arr[1]), (int)StateColor::Pressed),
        std::pair(wxColour(clr_arr[2]), (int)StateColor::Hovered),
        std::pair(wxColour(clr_arr[3]), (int)StateColor::Normal),
        std::pair(wxColour(clr_arr[4]), (int)StateColor::Enabled)
    );
    bg_color.setTakeFocusedAsHovered(false);
    this->SetBackgroundColor(bg_color);
    wxColour focus_clr = clr_arr[is_dark ? 8 : 9];
    auto border_color = StateColor(
        std::pair(wxColour(clr_arr[0]), (int)StateColor::Disabled),
        std::pair(wxColour(clr_arr[2]), (int)(StateColor::Hovered | ~StateColor::Focused)),
        std::pair(wxColour(focus_clr ), (int)StateColor::Focused),
        std::pair(wxColour(clr_arr[3]), (int)StateColor::Normal)
    );
    border_color.setTakeFocusedAsHovered(false);
    this->SetBorderColor(border_color);
    this->SetTextColor(StateColor(
        std::pair(wxColour(clr_arr[5]), (int)StateColor::Disabled),
        std::pair(wxColour(clr_arr[7]), (int)StateColor::Hovered),
        std::pair(wxColour(clr_arr[6]), (int)StateColor::Normal)
    ));

    m_has_style = true;
    m_style = style;
    m_type  = type;
}

void Button::Rescale()
{
    if (this->active_icon.bmp().IsOk())
        this->active_icon.msw_rescale();

    if (this->inactive_icon.bmp().IsOk())
        this->inactive_icon.msw_rescale();

    messureSize();

    if(m_has_style)
        SetStyle(m_style, m_type);

    Refresh();
}

void Button::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxPaintDC dc(this);
    render(dc);
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void Button::render(wxDC& dc)
{
    StaticBox::render(dc);
    int states = state_handler.states();
    wxSize size = GetSize();
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    // calc content size
    wxSize szIcon;
    wxSize textSize = this->textSize.GetSize();

    ScalableBitmap icon;
    if (m_selected || ((states & (int)StateColor::State::Hovered) != 0))
        icon = active_icon;
    else
        icon = inactive_icon;
    wxSize padding = this->paddingSize;
    int spacing = 5;
    // Wrap text
    auto text = GetLabel();
    if (vertical && textSize.x + padding.x * 2 > size.x) {
        Label::split_lines(dc, size.x - padding.x * 2, text, text, 2);
        textSize = dc.GetMultiLineTextExtent(text);
        if (padding.x * 2 + textSize.x > size.x) {
            text = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, size.x - padding.x * 2);
            textSize = dc.GetMultiLineTextExtent(text);
        }
    }
    auto szContent = textSize;
    if (icon.bmp().IsOk()) {
        if (szContent.y > 0) {
            //BBS norrow size between text and icon
            if (vertical)
                szContent.y += spacing;
            else
                szContent.x += spacing;
        }
        szIcon = icon.GetBmpSize();
        if (vertical) {
            szContent.y += szIcon.y;
            if (szIcon.x > szContent.x) szContent.x = szIcon.x;
        } else {
            szContent.x += szIcon.x;
            if (szIcon.y > szContent.y) szContent.y = szIcon.y;
        }
        if (szContent.x > size.x) {
            int d = std::min(padding.x, (szContent.x - size.x) / 2);
            padding.x -= d;
            szContent.x -= d;
        }
    }
    // move to center
    wxRect rcContent = { {0, 0}, size };
    if (isCenter) {
        wxSize offset = (size - szContent) / 2;
        if (offset.x < 0) offset.x = 0;
        rcContent.Deflate(offset.x, offset.y);
    }
    // start draw
    wxPoint pt = rcContent.GetLeftTop();
    if (icon.bmp().IsOk()) {
        if (vertical)
            pt.x += (rcContent.width - szIcon.x) / 2;
        else
            pt.y += (rcContent.height - szIcon.y) / 2;
        dc.DrawBitmap(icon.bmp(), pt);
        //BBS norrow size between text and icon
        if (vertical) {
            pt.y += szIcon.y + spacing;
            pt.x = rcContent.x;
        } else {
            pt.x += szIcon.x + spacing;
            pt.y = rcContent.y;
        }
    }
    if (!text.IsEmpty()) {
        if (vertical) {
            pt.x += (rcContent.width - textSize.x) / 2;
        } else {
            if (pt.x + textSize.x > size.x)
                text = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, size.x - pt.x);
            pt.y += (rcContent.height - textSize.y) / 2;
        }
        dc.SetTextForeground(text_color.colorForStates(states));
#if 0
        dc.SetBrush(*wxLIGHT_GREY);
        dc.SetPen(wxPen(*wxLIGHT_GREY));
        dc.DrawRectangle(pt, textSize.GetSize());
#endif
#ifdef __WXOSX__
        pt.y -= this->textSize.x / 2;
#endif
#ifdef __APPLE__
        if (Slic3r::is_mac_version_15()) {
        pt.y -= FromDIP(1);
    }
#endif
        dc.DrawText(text, pt);
    }
}

void Button::messureSize()
{
    wxClientDC dc(this);
    dc.GetTextExtent(GetLabel(), &textSize.width, &textSize.height, &textSize.x, &textSize.y);
    wxSize szContent = textSize.GetSize();
    if (this->active_icon.bmp().IsOk()) {
        if (szContent.y > 0) {
            //BBS norrow size between text and icon
            if (vertical)
                szContent.y += 5;
            else
                szContent.x += 5;
        }
        wxSize szIcon = this->active_icon.GetBmpSize();
        if (vertical) {
            szContent.y += szIcon.y;
            if (szIcon.x > szContent.x) szContent.x = szIcon.x;
        } else {
            szContent.x += szIcon.x;
            if (szIcon.y > szContent.y) szContent.y = szIcon.y;
        }
    }
    wxSize size = szContent + paddingSize * 2;
    if (minSize.GetHeight() > 0)
        size.SetHeight(minSize.GetHeight());

    if (auto w = GetMaxWidth(); w > 0 && size.GetWidth() > w) {
        size.SetWidth(GetMaxWidth());

        const wxString& tip_str = GetToolTipText();
        if (tip_str.IsEmpty()) {
            SetToolTip(GetLabel());
        }
    }

    if (minSize.GetWidth() > size.GetWidth())
        wxWindow::SetMinSize(minSize);
    else
        wxWindow::SetMinSize(size);
}

void Button::mouseDown(wxMouseEvent& event)
{
    event.Skip();
    pressedDown = true;
    if (canFocus)
        SetFocus();
    if (!HasCapture())
        CaptureMouse();
}

void Button::mouseReleased(wxMouseEvent& event)
{
    event.Skip();
    if (pressedDown) {
        pressedDown = false;
        if (HasCapture())
            ReleaseMouse();
        if (wxRect({0, 0}, GetSize()).Contains(event.GetPosition()))
            sendButtonEvent();
    }
}

void Button::mouseCaptureLost(wxMouseCaptureLostEvent &event)
{
    wxMouseEvent evt;
    mouseReleased(evt);
}

void Button::keyDownUp(wxKeyEvent &event)
{
    if (event.GetKeyCode() == WXK_SPACE || event.GetKeyCode() == WXK_RETURN) {
        wxMouseEvent evt(event.GetEventType() == wxEVT_KEY_UP ? wxEVT_LEFT_UP : wxEVT_LEFT_DOWN);
        event.SetEventObject(this);
        GetEventHandler()->ProcessEvent(evt);
        return;
    }
    if (event.GetEventType() == wxEVT_KEY_DOWN &&
        (event.GetKeyCode() == WXK_TAB || event.GetKeyCode() == WXK_LEFT || event.GetKeyCode() == WXK_RIGHT
        || event.GetKeyCode() == WXK_UP || event.GetKeyCode() == WXK_DOWN))
        HandleAsNavigationKey(event);
    else
        event.Skip();
}

void Button::sendButtonEvent()
{
    wxCommandEvent event(wxEVT_COMMAND_BUTTON_CLICKED, GetId());
    event.SetEventObject(this);
    GetEventHandler()->ProcessEvent(event);
}

#ifdef __WIN32__

WXLRESULT Button::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    if (nMsg == WM_GETDLGCODE) { return DLGC_WANTMESSAGE; }
    if (nMsg == WM_KEYDOWN) {
        wxKeyEvent event(CreateKeyEvent(wxEVT_KEY_DOWN, wParam, lParam));
        switch (wParam) {
        case WXK_RETURN: { // WXK_RETURN key is handled by default button
            GetEventHandler()->ProcessEvent(event);
            return 0;
        }
        }
    }
    return wxWindow::MSWWindowProc(nMsg, wParam, lParam);
}

#endif

bool Button::AcceptsFocus() const { return canFocus; }

void Button::EnableTooltipEvenDisabled()
{
#if defined(_MSC_VER) || defined(_WIN32)
    auto parent = this->GetParent();
    if (parent)
    {
        parent->Bind(wxEVT_MOTION, &Button::OnParentMotion, this);
        parent->Bind(wxEVT_LEAVE_WINDOW, &Button::OnParentLeave, this);
    };
#endif
};

void Button::OnParentMotion(wxMouseEvent& event)
{
    auto parent = this->GetParent();
    if (!parent) return event.Skip();

    wxPoint pos = parent->ClientToScreen(event.GetPosition());
    wxRect screen_rect = this->GetScreenRect();
    wxString tip = this->GetToolTipText();
    if (!tip.IsEmpty() && !this->IsEnabled() && screen_rect.Contains(pos))
    {
        if (!tipWindow)
        {
            tipWindow = new wxTipWindow(this, tip);
            tipWindow->Bind(wxEVT_DESTROY, [this](wxEvent& event) { this->tipWindow = nullptr;});
            tipWindow->Enable(false);
        }

        if (tipWindow->GetLabel() != tip)
        {
            tipWindow->SetLabel(tip);
        }

        tipWindow->Position(wxGetMousePosition(), wxSize(0, 0));
        tipWindow->Popup();
    }
    else
    {
        if (tipWindow)
        {
            delete tipWindow;
            tipWindow = nullptr;
        }
    }

    event.Skip();
}

void Button::OnParentLeave(wxMouseEvent& event)
{
    auto parent = this->GetParent();
    if (!parent) return event.Skip();

    if (tipWindow)
    {
        wxPoint pos = parent->ClientToScreen(event.GetPosition());
        wxRect screen_rect = this->GetScreenRect();
        wxString tip = this->GetToolTipText();
        if (!screen_rect.Contains(pos))
        {
            tipWindow->Dismiss();
            delete tipWindow;
            tipWindow = nullptr;
        }
    }

    event.Skip();
}

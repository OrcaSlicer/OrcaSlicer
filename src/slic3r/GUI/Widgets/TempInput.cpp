#include "TempInput.hpp"
#include "Label.hpp"
#include "PopupWindow.hpp"
#include "../I18N.hpp"
#include <wx/dcgraph.h>
#include "../GUI.hpp"
#include "../GUI_App.hpp"

wxDEFINE_EVENT(wxCUSTOMEVT_SET_TEMP_FINISH, wxCommandEvent);

BEGIN_EVENT_TABLE(TempInput, StaticBox)
EVT_MOTION(TempInput::mouseMoved)
EVT_ENTER_WINDOW(TempInput::mouseEnterWindow)
EVT_LEAVE_WINDOW(TempInput::mouseLeaveWindow)
EVT_KEY_DOWN(TempInput::keyPressed)
EVT_KEY_UP(TempInput::keyReleased)
EVT_MOUSEWHEEL(TempInput::mouseWheelMoved)
EVT_PAINT(TempInput::paintEvent)
END_EVENT_TABLE()


TempInput::TempInput()
    : label_color(std::make_pair(wxColour(0xAC,0xAC,0xAC), (int) StateColor::Disabled),std::make_pair(0x323A3C, (int) StateColor::Normal))
    , text_color(std::make_pair(wxColour(0xAC,0xAC,0xAC), (int) StateColor::Disabled), std::make_pair(0x6B6B6B, (int) StateColor::Normal))
{
    hover  = false;
    radius = 0;
    border_color = StateColor(std::make_pair(*wxWHITE, (int) StateColor::Disabled), std::make_pair(0x009688, (int) StateColor::Focused), std::make_pair(0x009688, (int) StateColor::Hovered),
                 std::make_pair(*wxWHITE, (int) StateColor::Normal));
    background_color = StateColor(std::make_pair(*wxWHITE, (int) StateColor::Disabled), std::make_pair(*wxWHITE, (int) StateColor::Normal));
    SetFont(Label::Body_12);
}

TempInput::TempInput(wxWindow *parent, int type, wxString text, TempInputType  input_type, wxString label, wxString normal_icon, wxString actice_icon, const wxPoint &pos, const wxSize &size, long style)
    : TempInput()
{
    actice = false;
    temp_type = type;
    m_input_type = input_type;
    Create(parent, text, label, normal_icon, actice_icon, pos, size, style);
}

void TempInput::ResetWaringDlg()
{
    if (wdialog) { wdialog->Dismiss(); }
    if (warning_mode) { Warning(false, WARNING_TOO_HIGH); }
}

bool TempInput::CheckIsValidVal(bool show_warning)
{
    auto temp = text_ctrl->GetValue();
    if (temp.ToStdString().empty())
    {
        return false;
    }

    if (!AllisNum(temp.ToStdString()))
    {
        return false;
    }

    /*show temperature range warnings*/
    auto tempint = std::stoi(temp.ToStdString());
    if (additional_temps.count(tempint) == 0)
    {
        if (tempint > max_temp)
        {
            if (show_warning) { Warning(true, WARNING_TOO_HIGH); }
            return false;
        }
        else if (tempint < min_temp)
        {
            if (show_warning) { Warning(true, WARNING_TOO_LOW); }
            return false;
        }
    }


    return true;
}

void TempInput::Create(wxWindow *parent, wxString text, wxString label, wxString normal_icon, wxString actice_icon, const wxPoint &pos, const wxSize &size, long style)
{
    StaticBox::Create(parent, wxID_ANY, pos, size, style);
    wxWindow::SetLabel(label);
    style &= ~wxALIGN_CENTER_HORIZONTAL;
    state_handler.attach({&label_color, &text_color});
    state_handler.update_binds();
    text_ctrl = new wxTextCtrl(this, wxID_ANY, text, {5, 5}, wxDefaultSize, wxTE_PROCESS_ENTER | wxBORDER_NONE, wxTextValidator(wxFILTER_NUMERIC), wxTextCtrlNameStr);
    text_ctrl->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    text_ctrl->SetMaxLength(3);
    state_handler.attach_child(text_ctrl);
    text_ctrl->Bind(wxEVT_SET_FOCUS, [this](auto &e) {
        e.SetId(GetId());
        ProcessEventLocally(e);
        e.Skip();
        if (m_read_only) return;
        // enter input mode
        auto temp = text_ctrl->GetValue();
        if (temp.length() > 0 && temp[0] == (0x5f)) {
            text_ctrl->SetValue(wxEmptyString);
        }
        if (wdialog != nullptr) { wdialog->Dismiss(); }
    });
    text_ctrl->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
        if (m_read_only) { SetCursor(wxCURSOR_ARROW); }
    });
    text_ctrl->Bind(wxEVT_KILL_FOCUS, [this](auto &e) {
        e.SetId(GetId());
        ProcessEventLocally(e);
        e.Skip();

        if (!m_on_changing) /*the wxCUSTOMEVT_SET_TEMP_FINISH event may popup a dialog, which may generate dead loop*/
        {
            ResetWaringDlg();
            SetFinish();
        }
    });
    text_ctrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &e)
    {
        if (!m_on_changing) /*the wxCUSTOMEVT_SET_TEMP_FINISH event may popup a dialog, which may generate dead loop*/
        {
            /*clear previous status*/
            ResetWaringDlg();

            /*check the value is valid or not*/
            if (CheckIsValidVal(true))
            {
                SetFinish();

                SetOnChanging();// filter in wxEVT_KILL_FOCUS while navigating
                text_ctrl->Navigate(); // quit edit mode
                ReSetOnChanging();
            }
        }
    });
    text_ctrl->Bind(wxEVT_RIGHT_DOWN, [this](auto &e) {}); // disable context menu
    text_ctrl->Bind(wxEVT_LEFT_DOWN, [this](auto &e) {
        if (m_read_only) {
            return;
        } else {
            e.Skip();
        }
    });
    text_ctrl->SetFont(Label::Body_13);
    text_ctrl->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));
    if (!normal_icon.IsEmpty()) { this->normal_icon = ScalableBitmap(this, normal_icon.ToStdString(), 16); }
    if (!actice_icon.IsEmpty()) { this->actice_icon = ScalableBitmap(this, actice_icon.ToStdString(), 16); }
    this->round_scale_hint_icon = ScalableBitmap(this, "round", 16);
    this->degree_icon = ScalableBitmap(this, "degree", 16);
    messureSize();
}


bool TempInput::AllisNum(std::string str)
{
    for (int i = 0; i < str.size(); i++) {
        int tmp = (int) str[i];
        if (tmp >= 48 && tmp <= 57) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void TempInput::SetFinish()
{
    wxCommandEvent event(wxCUSTOMEVT_SET_TEMP_FINISH);
    event.SetInt(temp_type);
    event.SetString(wxString::Format("%d", m_input_type));
    wxPostEvent(this->GetParent(), event);
}

wxString TempInput::erasePending(wxString &str)
{
    wxString tmp   = str;
    int      index = tmp.size() - 1;
    while (index != -1) {
        if (tmp[index] < '0' || tmp[index] > '9') {
            tmp.erase(index, 1);
            index--;
        } else {
            break;
        }
    }
    return tmp;
}

void TempInput::SetTagTemp(int temp)
{
    auto tp = wxString::Format("%d", temp);
    if (text_ctrl->GetValue() != tp) {
        text_ctrl->SetValue(tp);
        messureSize();
        Refresh();
    }
}

void TempInput::SetTagTemp(wxString temp)
{
    if (text_ctrl->GetValue() != temp) {
        text_ctrl->SetValue(temp);
        messureSize();
        Refresh();
    }
}

void TempInput::SetCurrTemp(int temp)
{
    auto tp = wxString::Format("%d", temp);
    if (GetLabel() != tp) {
        SetLabel(tp);
        Refresh();
    }
}

void TempInput::SetCurrTemp(wxString temp)
{
    if (GetLabel() != temp) {
        SetLabel(temp);
        Refresh();
    }
}

void TempInput::SetCurrType(TempInputType type) {
    m_input_type = type;
}

void TempInput::Warning(bool warn, WarningType type)
{
    warning_mode = warn;
    //Refresh();

    if (warning_mode) {
        if (wdialog == nullptr) {
            wdialog = new PopupWindow(this);
            wdialog->SetBackgroundColour(wxColour("#FFFFFF"));

            wdialog->SetSizeHints(wxDefaultSize, wxDefaultSize);

            wxBoxSizer *sizer_body = new wxBoxSizer(wxVERTICAL);

            auto body = new wxPanel(wdialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
            body->SetBackgroundColour(wxColour("#FFFFFF"));


            wxBoxSizer *sizer_text;
            sizer_text = new wxBoxSizer(wxHORIZONTAL);



            warning_text = new wxStaticText(body, wxID_ANY,
                                            wxEmptyString,
                                            wxDefaultPosition, wxDefaultSize,
                                            wxALIGN_CENTER_HORIZONTAL);
            warning_text->SetFont(::Label::Body_12);
            warning_text->SetForegroundColour(wxColour(255, 111, 0));
            warning_text->Wrap(-1);
            sizer_text->Add(warning_text, 1, wxEXPAND | wxTOP | wxBOTTOM, 2);

            body->SetSizer(sizer_text);
            body->Layout();
            sizer_body->Add(body, 0, wxEXPAND, 0);

            wdialog->SetSizer(sizer_body);
            wdialog->Layout();
            sizer_body->Fit(wdialog);
        }

        wxPoint pos = this->ClientToScreen(wxPoint(2, 0));
        pos.y += this->GetRect().height - (this->GetSize().y - this->text_ctrl->GetSize().y) / 2 - 2;
        wdialog->SetPosition(pos);

        wxString warning_string;
        if (type == WarningType::WARNING_TOO_HIGH)
             warning_string = _L("The maximum temperature cannot exceed ") + wxString::Format("%d", max_temp);
        else if (type == WarningType::WARNING_TOO_LOW)
             warning_string = _L("The minmum temperature should not be less than ") + wxString::Format("%d", min_temp);
        warning_text->SetLabel(warning_string);
        warning_text->Wrap(-1);
        warning_text->Fit();
        wdialog->Fit();
        wdialog->Popup();
    } else {
        if (wdialog)
            wdialog->Dismiss();
    }
}

void TempInput::SetIconActive()
{
    actice = true;
    Refresh();
}

void TempInput::SetIconNormal()
{
    actice = false;
    Refresh();
}

void TempInput::SetMaxTemp(int temp) { max_temp = temp; }

void TempInput::SetMinTemp(int temp) { min_temp = temp; }

void TempInput::SetLabel(const wxString &label)
{
    if (label != wxWindow::GetLabel()) {
        wxWindow::SetLabel(label);
        messureSize();
        Refresh();
    }
}

void TempInput::SetTextColor(StateColor const &color)
{
    text_color = color;
    state_handler.update_binds();
}

void TempInput::SetLabelColor(StateColor const &color)
{
    label_color = color;
    state_handler.update_binds();
}

void TempInput::Rescale()
{
    if (this->normal_icon.bmp().IsOk()) this->normal_icon.msw_rescale();
    if (this->actice_icon.bmp().IsOk()) this->actice_icon.msw_rescale();
    if (this->degree_icon.bmp().IsOk()) this->degree_icon.msw_rescale();
    if (this->round_scale_hint_icon.bmp().IsOk()) this->round_scale_hint_icon.msw_rescale();
    messureSize();
}

bool TempInput::Enable(bool enable)
{
    bool result = wxWindow::Enable(enable);
    if (result) {
        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);
    }
    return result;
}

void TempInput::SetMinSize(const wxSize &size)
{
    wxSize size2 = size;
    if (size2.y < 0) {
#ifdef __WXMAC__
        if (GetPeer()) // peer is not ready in Create on mac
#endif
            size2.y = GetSize().y;
    }
    wxWindow::SetMinSize(size2);
    messureMiniSize();
}

void TempInput::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (sizeFlags & wxSIZE_USE_EXISTING) return;

    padding_left = FromDIP(10);
    auto       left = padding_left;
    wxClientDC dc(this);
    if (normal_icon.bmp().IsOk()) {
        wxSize szIcon = normal_icon.GetBmpSize();
        left += szIcon.x;
    }

    // interval
    left += 9;

    if (m_input_type == TEMP_OF_MAIN_NOZZLE_TYPE || m_input_type == TEMP_OF_DEPUTY_NOZZLE_TYPE) {
        wxSize szIcon = round_scale_hint_icon.GetBmpSize();
        left += szIcon.x + 3;
    }

    // label
    dc.SetFont(::Label::Head_14);
    labelSize = dc.GetMultiLineTextExtent(wxWindow::GetLabel());
    left += labelSize.x;

    // interval
    left += 10;

    // separator
    dc.SetFont(::Label::Body_12);
    auto sepSize = dc.GetMultiLineTextExtent(wxString("/"));
    left += sepSize.x;

    // text text
    auto textSize = text_ctrl->GetTextExtent(wxString("0000"));
    text_ctrl->SetSize(textSize);
    text_ctrl->SetPosition({left, (GetSize().y - text_ctrl->GetSize().y) / 2});
}

void TempInput::DoSetToolTipText(wxString const &tip)
{
    wxWindow::DoSetToolTipText(tip);
    text_ctrl->SetToolTip(tip);
}

void TempInput::paintEvent(wxPaintEvent &evt)
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
void TempInput::render(wxDC &dc)
{
    StaticBox::render(dc);
    int    states      = state_handler.states();
    wxSize size        = GetSize();
    bool   align_right = GetWindowStyle() & wxRIGHT;

    if (warning_mode) {
        border_color = wxColour(255, 111, 0);
    } else {
        border_color = StateColor(std::make_pair(*wxWHITE, (int) StateColor::Disabled), std::make_pair(0x009688, (int) StateColor::Focused),
                                  std::make_pair(0x009688, (int) StateColor::Hovered), std::make_pair(*wxWHITE, (int) StateColor::Normal));
    }

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    // start draw
    padding_left = FromDIP(10);
    wxPoint pt = {padding_left, 0};
    wxSize szIcon;
    if (normal_icon.bmp().IsOk()) szIcon = normal_icon.GetBmpSize();
    else if (actice_icon.bmp().IsOk()) szIcon = actice_icon.GetBmpSize();

    if (actice_icon.bmp().IsOk() && actice) {
        szIcon = actice_icon.GetBmpSize();
        pt.y = (size.y - szIcon.y) / 2;
        dc.DrawBitmap(actice_icon.bmp(), pt);
    }
    else {
        actice = false;
    }
    if (normal_icon.bmp().IsOk() && !actice) {
        szIcon = normal_icon.GetBmpSize();
        pt.y = (size.y - szIcon.y) / 2;
        dc.DrawBitmap(normal_icon.bmp(), pt);
    }

    pt.x += szIcon.x + 9;

    ScalableBitmap round_icon(this, "round", 16);/*icon diffs in normal and dark mode*/
    round_icon.msw_rescale();
    if (round_icon.bmp().IsOk() && m_input_type == TEMP_OF_DEPUTY_NOZZLE_TYPE){
        wxSize szIcon = round_icon.GetBmpSize();
        pt.y = (size.y - szIcon.y) / 2;
        dc.DrawBitmap(round_icon.bmp(), pt);

        dc.SetFont(::Label::Body_12);
        auto sepSize = dc.GetMultiLineTextExtent(wxString("L"));

        const wxColour& clr = Slic3r::GUI::wxGetApp().dark_mode() ? StateColor::darkModeColorFor(*wxWHITE) : *wxWHITE;
        dc.SetTextForeground(clr);
        dc.SetTextBackground(clr);
        dc.DrawText(wxString("L"), pt.x + (szIcon.x - sepSize.x) / 2, (size.y - sepSize.y) / 2);
        pt.x += szIcon.x + 3;
    }

    if (round_icon.bmp().IsOk() && m_input_type == TEMP_OF_MAIN_NOZZLE_TYPE) {
        wxSize szIcon = round_icon.GetBmpSize();
        pt.y = (size.y - szIcon.y) / 2;
        dc.DrawBitmap(round_icon.bmp(), pt);

        dc.SetFont(::Label::Body_12);
        auto sepSize = dc.GetMultiLineTextExtent(wxString("R"));

        const wxColour& clr = Slic3r::GUI::wxGetApp().dark_mode() ? StateColor::darkModeColorFor(*wxWHITE) : *wxWHITE;
        dc.SetTextForeground(clr);
        dc.SetTextBackground(clr);
        dc.DrawText(wxString("R"), pt.x + (szIcon.x - sepSize.x) / 2, (size.y - sepSize.y) / 2);
        pt.x += szIcon.x + 3;
    }

    // label
    auto text = wxWindow::GetLabel();
    dc.SetFont(::Label::Head_14);
    labelSize = dc.GetMultiLineTextExtent(wxWindow::GetLabel());

    if (!IsEnabled()) {
        dc.SetTextForeground(wxColour(0xAC, 0xAC, 0xAC));
        dc.SetTextBackground(background_color.colorForStates((int) StateColor::Disabled));
    }
    else {
        dc.SetTextForeground(wxColour(0x32, 0x3A, 0x3D));
        dc.SetTextBackground(background_color.colorForStates((int) states));
    }


    /*if (!text.IsEmpty()) {

    }*/
    wxSize textSize = text_ctrl->GetSize();
    if (align_right) {
        if (pt.x + labelSize.x > size.x) text = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, size.x - pt.x);
        pt.y = (size.y - labelSize.y) / 2;
    } else {
        pt.y = (size.y - labelSize.y) / 2;
    }

    dc.SetTextForeground(StateColor::darkModeColorFor("#323A3C"));
    dc.DrawText(text, pt);

    // separator
    dc.SetFont(::Label::Body_12);
    auto sepSize = dc.GetMultiLineTextExtent(wxString("/"));
    dc.SetTextForeground(text_color.colorForStates(states));
    dc.SetTextBackground(background_color.colorForStates(states));
    pt.x += labelSize.x + 10;
    pt.y = (size.y - sepSize.y) / 2;
    dc.DrawText(wxString("/"), pt);

    // flag
    if (degree_icon.bmp().IsOk()) {
        auto   pos    = text_ctrl->GetPosition();
        wxSize szIcon = degree_icon.GetBmpSize();
        pt.y          = (size.y - szIcon.y) / 2;
        pt.x          = pos.x + text_ctrl->GetSize().x;
        dc.DrawBitmap(degree_icon.bmp(), pt);
    }
}


void TempInput::messureMiniSize()
{
    wxSize size = GetMinSize();

    auto width  = 0;
    auto height = 0;

    wxClientDC dc(this);
    if (normal_icon.bmp().IsOk()) {
        wxSize szIcon = normal_icon.GetBmpSize();
        width += szIcon.x;
        height = szIcon.y;
    }

    // interval
    width += 9;

    if (m_input_type == TEMP_OF_MAIN_NOZZLE_TYPE || m_input_type == TEMP_OF_DEPUTY_NOZZLE_TYPE) {
        wxSize szIcon = round_scale_hint_icon.GetBmpSize();
        width += szIcon.x;
    }
    width += 3;

    // label
    dc.SetFont(::Label::Head_14);
    labelSize = dc.GetMultiLineTextExtent(wxWindow::GetLabel());
    width += labelSize.x;
    height = labelSize.y > height ? labelSize.y : height;

    // interval
    width += 10;

    // separator
    dc.SetFont(::Label::Body_12);
    auto sepSize = dc.GetMultiLineTextExtent(wxString("/"));
    width += sepSize.x;
    height = sepSize.y > height ? sepSize.y : height;

    // text text
    auto textSize = text_ctrl->GetTextExtent(wxString("0000"));
    width += textSize.x;
    height = textSize.y > height ? textSize.y : height;

    // flag flag
    auto flagSize = degree_icon.GetBmpSize();
    width += flagSize.x;
    height = flagSize.y > height ? flagSize.y : height;

    if (size.x < width) {
        size.x = width;
    } else {
        padding_left = (size.x - width) / 2;
    }

    if (size.y < height) size.y = height;

    SetSize(size);
}


void TempInput::messureSize()
{
    wxSize size = GetSize();

    auto width  = 0;
    auto height = 0;

    wxClientDC dc(this);
    if (normal_icon.bmp().IsOk()) {
        wxSize szIcon = normal_icon.GetBmpSize();
        width += szIcon.x;
        height = szIcon.y;
    }

    // interval
    width += 9;

    if (m_input_type == TEMP_OF_MAIN_NOZZLE_TYPE || m_input_type == TEMP_OF_DEPUTY_NOZZLE_TYPE) {
        wxSize szIcon = round_scale_hint_icon.GetBmpSize();
        width += szIcon.x;
    }
    width += 3;

    // label
    dc.SetFont(::Label::Head_14);
    labelSize = dc.GetMultiLineTextExtent(wxWindow::GetLabel());
    width += labelSize.x;
    height = labelSize.y > height ? labelSize.y : height;

    // interval
    width += 10;

    // separator
    dc.SetFont(::Label::Body_12);
    auto sepSize = dc.GetMultiLineTextExtent(wxString("/"));
    width += sepSize.x;
    height = sepSize.y > height ? sepSize.y : height;

    // text text
    auto textSize = text_ctrl->GetTextExtent(wxString("0000"));
    width += textSize.x;
    height = textSize.y > height ? textSize.y : height;

    // flag flag
    auto flagSize = degree_icon.GetBmpSize();
    width += flagSize.x;
    height = flagSize.y > height ? flagSize.y : height;

    if (size.x < width) {
        size.x = width;
    } else {
        padding_left = (size.x - width) / 2;
    }

    if (size.y < height) size.y = height;

    wxSize minSize = size;
    minSize.x      = GetMinWidth();
    SetMinSize(minSize);
    SetSize(size);
}

void TempInput::mouseEnterWindow(wxMouseEvent &event)
{
    if (!hover) {
        hover = true;
        Refresh();
    }
}

void TempInput::mouseLeaveWindow(wxMouseEvent &event)
{
    if (hover) {
        hover = false;
        Refresh();
    }
}

// currently unused events
void TempInput::mouseMoved(wxMouseEvent &event) {}
void TempInput::mouseWheelMoved(wxMouseEvent &event) {}
void TempInput::keyPressed(wxKeyEvent &event) {}
void TempInput::keyReleased(wxKeyEvent &event) {}

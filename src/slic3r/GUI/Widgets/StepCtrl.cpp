#include "StepCtrl.hpp"
#include "Label.hpp"

#include <wx/dc.h>
#include <wx/pen.h>

wxDEFINE_EVENT( EVT_STEP_CHANGING, wxCommandEvent );
wxDEFINE_EVENT( EVT_STEP_CHANGED, wxCommandEvent );

BEGIN_EVENT_TABLE(StepCtrl, StepCtrlBase)
EVT_LEFT_DOWN(StepCtrl::mouseDown)
EVT_MOTION(StepCtrl::mouseMove)
EVT_LEFT_UP(StepCtrl::mouseUp)
EVT_MOUSE_CAPTURE_LOST(StepCtrl::mouseCaptureLost)
END_EVENT_TABLE()

StepCtrlBase::StepCtrlBase(wxWindow *      parent,
                   wxWindowID      id,
                   const wxPoint & pos,
                   const wxSize &  size,
                   long            style)
    : StaticBox(parent, id, pos, size, style)
    , font_tip(Label::Body_14)
    , clr_bar(0xACACAC)
    , clr_step(0xACACAC)
    , clr_text(std::make_pair(0x009688, (int) StateColor::Checked),
            std::make_pair(0x6B6B6B, (int) StateColor::Normal))
    , clr_tip(0x828280)
{
    SetFont(Label::Body_14);
    border_color     = StateColor(*wxLIGHT_GREY);
    StaticBox::radius = 0;
    //wxString reason;
    //IsTransparentBackgroundSupported(&reason);
}

StepCtrlBase::~StepCtrlBase()
{
}

int StepCtrlBase::GetSelection() const { return step; }

void StepCtrlBase::SelectItem(int item)
{
    if (item == step || item < -1 || item >= steps.size() || !sendStepCtrlEvent(true))
        return;
    step = item;
    sendStepCtrlEvent();
    Refresh();
}

void StepCtrlBase::Idle()
{
    if (step != -1) {
        step = -1;
        sendStepCtrlEvent();
        Refresh();
    }
}

bool StepCtrlBase::SetTipFont(wxFont const& font)
{
    font_tip = font;
    return true;
}

void StepCtrlBase::SetHint(wxString hint) {
    this->hint = hint;
}

int StepCtrlBase::AppendItem(const wxString &item, wxString const & tip)
{
    steps.push_back(item);
    tips.push_back(tip);
    return steps.size() - 1;
}

void StepCtrlBase::DeleteAllItems()
{
    steps.clear();
    tips.clear();
    if (step >= 0) {
        step = -1;
        sendStepCtrlEvent();
    }
}

unsigned int StepCtrlBase::GetCount() const { return steps.size(); }

wxString StepCtrlBase::GetItemText(unsigned int item) const
{
    return item < steps.size() ? steps[item] : wxString{};
}

int StepCtrlBase::GetItemUseText(wxString txt) const
{
    for(int i = 0; i < steps.size(); i++){
        if (steps[i] == txt) {
            return i;
        }
        else {
            continue;
        }
    }
    return 0;
}

void StepCtrlBase::SetItemText(unsigned int item, wxString const &value)
{
    if (item >= steps.size()) return;
    steps[item] = value;
}

bool StepCtrlBase::sendStepCtrlEvent(bool changing)
{
    wxCommandEvent event(changing ? EVT_STEP_CHANGING : EVT_STEP_CHANGED, GetId());
    event.SetEventObject(this);
    event.SetInt(step);
    GetEventHandler()->ProcessEvent(event);
    return true;
}

/* StepCtrl */

StepCtrl::StepCtrl(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
    : StepCtrlBase(parent, id, pos, size, style)
    , bmp_thumb(this, "step_thumb", 36)
{
    StaticBox::border_width = 3;
    radius = radius * bmp_thumb.GetBmpHeight() / 36;
    bar_width = bar_width * bmp_thumb.GetBmpHeight() / 36;
}

void StepCtrl::Rescale()
{
    bmp_thumb.msw_rescale();
    radius    = radius * bmp_thumb.GetBmpHeight() / 36;
    bar_width = bar_width * bmp_thumb.GetBmpHeight() / 36;
}

void StepCtrl::mouseDown(wxMouseEvent &event)
{
    wxPoint pt;
    event.GetPosition(&pt.x, &pt.y);
    wxSize size      = GetSize();
    int    itemWidth = size.x / steps.size();
    wxRect rcBar     = {0, (size.y - 60) / 2, size.x, 60};
    int    circleX   = itemWidth / 2 + itemWidth * step;
    wxRect rcThumb   = {{circleX, size.y / 2}, bmp_thumb.GetBmpSize()};
    rcThumb.x -= rcThumb.width / 2;
    rcThumb.y -= rcThumb.height / 2;
    if (rcThumb.Contains(pt)) {
        pos_thumb   = wxPoint{circleX, size.y / 2};
        drag_offset = pos_thumb - pt;
        if (!HasCapture())
            CaptureMouse();
    } else if (rcBar.Contains(pt)) {
        if (pt.x < circleX) {
            if (step > 0) SelectItem(step - 1);
        } else {
            if (step < steps.size() - 1) SelectItem(step + 1);
        }
    }
}

void StepCtrl::mouseMove(wxMouseEvent &event)
{
    if (pos_thumb == wxPoint{0, 0}) return;
    wxPoint pt;
    event.GetPosition(&pt.x, &pt.y);
    pos_thumb.x = pt.x + drag_offset.x;
    wxSize size      = GetSize();
    int    itemWidth = size.x / steps.size();
    int    index     = pos_thumb.x / itemWidth;
    if (index < 0)
        index = 0;
    else if (index >= steps.size())
        index = steps.size() - 1;
    if (index != pos_thumb.y) {
        pos_thumb.y = index;
        Refresh();
    }
}

void StepCtrl::mouseUp(wxMouseEvent &event)
{
    if (pos_thumb == wxPoint{0, 0}) return;
    wxSize size      = GetSize();
    int    itemWidth = size.x / steps.size();
    int    index     = pos_thumb.x / itemWidth;
    if (index < 0)
        index = 0;
    else if (index >= steps.size())
        index = steps.size() - 1;
    pos_thumb = {0, 0};
    SelectItem(index);
    if (HasCapture())
        ReleaseMouse();
}

void StepCtrl::mouseCaptureLost(wxMouseCaptureLostEvent &event)
{
    wxMouseEvent evt;
    mouseUp(evt);
}

void StepCtrl::doRender(wxDC &dc)
{
    if (steps.empty()) return;
    StaticBox::doRender(dc);

    wxSize size   = GetSize();
    int    states = state_handler.states();

    int    itemWidth = size.x / steps.size();
    wxRect rcBar     = {itemWidth / 2, (size.y - bar_width) / 2, size.x - itemWidth, bar_width};

    dc.SetPen(wxPen(clr_bar.colorForStates(states)));
    dc.SetBrush(wxBrush(clr_bar.colorForStates(states)));
    dc.DrawRectangle(rcBar);
    int circleX = itemWidth / 2;
    int circleY = size.y / 2;
    dc.SetPen(wxPen(clr_step.colorForStates(states)));
    dc.SetBrush(wxBrush(clr_step.colorForStates(states)));
    if (!hint.empty()) {
        dc.SetFont(font_tip);
        dc.SetTextForeground(clr_tip.colorForStates(states));
        wxSize sz = dc.GetTextExtent(hint);
        dc.DrawText(hint, dc.GetCharWidth(), circleY - FromDIP(20) - sz.y);
    }
    for (int i = 0; i < steps.size(); ++i) {
        bool check = (pos_thumb == wxPoint{0, 0} ? step : pos_thumb.y) == i;
        dc.DrawEllipse(circleX - radius, circleY - radius, radius * 2, radius * 2);
        dc.SetFont(GetFont());
        dc.SetTextForeground(clr_text.colorForStates(states | (check ? StateColor::Checked : 0)));
        wxSize sz = dc.GetTextExtent(steps[i]);
        dc.DrawText(steps[i], circleX - sz.x / 2, circleY + 20);
        if (check) {
            dc.SetFont(font_tip);
            dc.SetTextForeground(clr_tip.colorForStates(states));
            wxSize sz = dc.GetTextExtent(tips[i]);
            dc.DrawText(tips[i], circleX - sz.x / 2, circleY - 20 - sz.y);
            sz = bmp_thumb.GetBmpSize();
            dc.DrawBitmap(bmp_thumb.bmp(), circleX - sz.x / 2, circleY - sz.y / 2);
        }
        circleX += itemWidth;
    }
}

/* StepIndicator */

StepIndicator::StepIndicator(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
    : StepCtrlBase(parent, id, pos, size, style)
    , bmp_ok(this, "step_ok", 12)
{
    SetFont(Label::Body_12);
    font_tip = Label::Body_10;
    clr_bar = 0xE1E1E1;
    clr_step = StateColor(
            std::make_pair(0xACACAC, (int) StateColor::Disabled),
            std::make_pair(0x009688, 0));
    clr_text = StateColor(
            std::make_pair(0xACACAC, (int) StateColor::Disabled),
            std::make_pair(0x323A3D, (int) StateColor::Checked),
            std::make_pair(0x6B6B6B, 0));
    clr_tip = *wxWHITE;
    StaticBox::border_width = 0;
    radius    = bmp_ok.GetBmpHeight() / 2;
    bar_width = bmp_ok.GetBmpHeight() / 20;
    if (bar_width < 2) bar_width = 2;
}

void StepIndicator::Rescale()
{
    bmp_ok.msw_rescale();
    radius    = bmp_ok.GetBmpHeight() / 2;
    bar_width = bmp_ok.GetBmpHeight() / 20;
    if (bar_width < 2) bar_width = 2;
}

void StepIndicator::SelectNext() { SelectItem(step + 1); }


void StepIndicator::doRender(wxDC &dc)
{
    if (steps.empty()) return;

    StaticBox::doRender(dc);

    wxSize size   = GetSize();

    int    states = state_handler.states();
    if (!IsEnabled()) {
        states = clr_step.Disabled;
    }

    int textWidth = size.x - radius * 5;
    dc.SetFont(GetFont());
    wxString firstLine;
    if (step == 0) dc.SetFont(GetFont().Bold());
    wxSize   firstLineSize = Label::split_lines(dc, textWidth, steps.front(), firstLine);
    wxString lastLine;
    if (step == steps.size() - 1) dc.SetFont(GetFont().Bold());
    wxSize   lastLineSize = Label::split_lines(dc, textWidth, steps.back(), lastLine);
    int      firstPadding = std::max(0, firstLineSize.y / 2 - radius);
    int      lastPadding  = std::max(0, lastLineSize.y / 2 - radius);

    wxRect rcBar = {radius * 2 - bar_width / 2, radius * 2 + firstPadding, bar_width, size.y - radius * 6 - firstPadding - lastPadding};
    int    itemWidth = steps.size() == 1 ? size.y : rcBar.height / (steps.size() - 1);

    // Draw thin bar stick
    dc.SetPen(wxPen(clr_bar.colorForStates(states)));
    dc.SetBrush(wxBrush(clr_bar.colorForStates(states)));
    dc.DrawRectangle(rcBar);

    int circleX = radius * 2;
    int circleY = radius * 3 + firstPadding;
    dc.SetPen(wxPen(clr_step.colorForStates(states)));
    dc.SetBrush(wxBrush(clr_step.colorForStates(states)));
    for (int i = 0; i < steps.size(); ++i) {
        bool disabled = step > i;
        bool checked = step == i;
        // Draw circle point & texts in it
        dc.DrawEllipse(circleX - radius, circleY - radius, radius * 2, radius * 2);
        // Draw content ( icon or text ) in circle
        if (disabled) {
            wxSize sz = bmp_ok.GetBmpSize();
            dc.DrawBitmap(bmp_ok.bmp(), circleX - radius, circleY - radius);
        } else {
            dc.SetFont(font_tip);
            dc.SetTextForeground(clr_tip.colorForStates(states));
            auto tip = tips[i];
            if (tip.IsEmpty()) tip.append(1, wchar_t(L'0' + i + 1));
            wxSize sz = dc.GetTextExtent(tip);
            dc.DrawText(tip, circleX - sz.x / 2, circleY - sz.y / 2 + 1);
        }
        // Draw step text
        dc.SetTextForeground(clr_text.colorForStates(states
                | (disabled ? StateColor::Disabled : checked ? StateColor::Checked : 0)));
        dc.SetFont(checked ? GetFont().Bold() : GetFont());
        wxString text;
        wxSize textSize;
        if (i == 0) {
            text = firstLine;
            textSize = firstLineSize;
        } else if (i == steps.size() - 1) {
            text = lastLine;
            textSize = lastLineSize;
        } else {
            textSize = Label::split_lines(dc, textWidth, steps[i], text);
        }
        dc.DrawText(text, circleX + radius * 3, circleY - (textSize.y / 2));
        circleY += itemWidth;
    }
}


/* FilamentStepIndicator */

FilamentStepIndicator::FilamentStepIndicator(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : StepCtrlBase(parent, id, pos, size, style)
    , bmp_ok(this, "step_ok", 12)
{
    static Slic3r::GUI::BitmapCache cache;
    //bmp_extruder = *cache.load_png("filament_load_extruder", FromDIP(300), FromDIP(200), false, false);
    SetFont(Label::Body_12);
    font_tip = Label::Body_12;
    clr_bar = 0xE1E1E1;
    clr_step = StateColor(
        std::make_pair(0xACACAC, (int)StateColor::Disabled),
        std::make_pair(0x009688, 0));
    clr_text = StateColor(
        std::make_pair(0xACACAC, (int)StateColor::Disabled),
        std::make_pair(0x323A3D, (int)StateColor::Checked),
        std::make_pair(0x6B6B6B, 0));
    clr_tip = *wxWHITE;
    StaticBox::border_width = 0;
    radius = 9;
    bar_width = 0;
}

void FilamentStepIndicator::Rescale()
{
    bmp_ok.msw_rescale();
    radius = bmp_ok.GetBmpHeight() / 2;
    bar_width = bmp_ok.GetBmpHeight() / 20;
    if (bar_width < 2) bar_width = 2;
}

void FilamentStepIndicator::SelectNext() { SelectItem(step + 1); }


void FilamentStepIndicator::doRender(wxDC& dc)
{


    if (steps.empty()) return;

    StaticBox::doRender(dc);

    wxSize size = GetSize();

    int    states = state_handler.states();
    if (!IsEnabled()) {
        states = clr_step.Disabled;
    }

    dc.SetFont(::Label::Head_16);
    dc.SetTextForeground(wxColour(0, 150, 136));
    int circleX = 20;
    int circleY = 20;
    wxSize sz = dc.GetTextExtent(L"Loading");
    dc.DrawText(L"Loading", circleX, circleY);

    dc.SetFont(::Label::Body_13);

    //dc.DrawBitmap(bmp_extruder, FromDIP(250), circleY);
    circleY += sz.y;

    int textWidth = size.x - radius * 5;
    dc.SetFont(GetFont());
    wxString firstLine;
    if (step == 0) dc.SetFont(GetFont().Bold());
    wxSize   firstLineSize = Label::split_lines(dc, textWidth, steps.front(), firstLine);
    wxString lastLine;
    if (step == steps.size() - 1) dc.SetFont(GetFont().Bold());
    wxSize   lastLineSize = Label::split_lines(dc, textWidth, steps.back(), lastLine);
    int      firstPadding = std::max(0, firstLineSize.y / 2 - radius);
    int      lastPadding = std::max(0, lastLineSize.y / 2 - radius);

    int    itemWidth = radius * 3;

    // Draw thin bar stick
    dc.SetPen(wxPen(clr_bar.colorForStates(states)));
    dc.SetBrush(wxBrush(clr_bar.colorForStates(states)));
    //dc.DrawRectangle(rcBar);

    circleX += radius;
    circleY += radius * 3 + firstPadding;
    dc.SetPen(wxPen(clr_step.colorForStates(states)));
    dc.SetBrush(wxBrush(clr_step.colorForStates(states)));
    for (int i = 0; i < steps.size(); ++i) {
        bool disabled = step > i;
        bool checked = step == i;
        // Draw circle point & texts in it
        dc.DrawEllipse(circleX - radius, circleY - radius, radius * 2, radius * 2);
        // Draw content ( icon or text ) in circle
        if (disabled) {
            wxSize sz = bmp_ok.GetBmpSize();
            dc.DrawBitmap(bmp_ok.bmp(), circleX - sz.x / 2, circleY - sz.y / 2);
        }
        else {
            dc.SetFont(font_tip);
            dc.SetTextForeground(clr_tip.colorForStates(states));
            auto tip = tips[i];
            if (tip.IsEmpty()) tip.append(1, wchar_t(L'0' + i + 1));
            wxSize sz = dc.GetTextExtent(tip);
            dc.DrawText(tip, circleX - sz.x / 2, circleY - sz.y / 2 + 1);
        }
        // Draw step text
        dc.SetTextForeground(clr_text.colorForStates(states
            | (disabled ? StateColor::Disabled : checked ? StateColor::Checked : 0)));
        dc.SetFont(checked ? GetFont().Bold() : GetFont());
        wxString text;
        wxSize textSize;
        if (i == 0) {
            text = firstLine;
            textSize = firstLineSize;
        }
        else if (i == steps.size() - 1) {
            text = lastLine;
            textSize = lastLineSize;
        }
        else {
            textSize = Label::split_lines(dc, textWidth, steps[i], text);
        }
        dc.DrawText(text, circleX + radius * 1.5, circleY - (textSize.y / 2));
        circleY += itemWidth;
    }
}

void FilamentStepIndicator::SetSlotInformation(wxString slot) {
    this->m_slot_information = slot;
}
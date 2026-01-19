#include "AxisCtrlButton.hpp"
#include "Label.hpp"
#include "libslic3r/libslic3r.h"

#include <wx/dcclient.h>
#include <wx/dcgraph.h>

StateColor blank_bg(StateColor(std::make_pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
static const wxColour BUTTON_BG_COL = wxColour("#EEEEEE");
static const wxColour BUTTON_IN_BG_COL = wxColour("#CECECE");

static const wxColour bd = wxColour(0, 150, 136);
static const wxColour text_num_color   = wxColour("#898989");
static const wxColour BUTTON_PRESS_COL = wxColour(172, 172, 172);
static const double sqrt2 = std::sqrt(2);

BEGIN_EVENT_TABLE(AxisCtrlButton, wxWindow)
EVT_LEFT_DOWN(AxisCtrlButton::mouseDown)
EVT_LEFT_UP(AxisCtrlButton::mouseReleased)
EVT_MOTION(AxisCtrlButton::mouseMoving)
EVT_PAINT(AxisCtrlButton::paintEvent)
END_EVENT_TABLE()

#define OUTER_SIZE      FromDIP(105)
#define INNER_SIZE      FromDIP(58)
#define HOME_SIZE       FromDIP(23)
#define BLANK_SIZE      FromDIP(24)
#define GAP_SIZE        FromDIP(4)

AxisCtrlButton::AxisCtrlButton(wxWindow *parent, ScalableBitmap &icon, long stlye)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, stlye)
    , r_outer(OUTER_SIZE)
    , r_inner(INNER_SIZE)
    , r_home(HOME_SIZE)
    , r_blank(BLANK_SIZE)
    , gap(GAP_SIZE)
    , last_pos(UNDEFINED)
    , current_pos(UNDEFINED) // don't change init value
    , text_color(std::make_pair(0x6B6B6B, (int) StateColor::Disabled), std::make_pair(*wxBLACK, (int) StateColor::Normal))
	, state_handler(this)
{
    m_icon = icon;
	wxWindow::SetBackgroundColour(parent->GetBackgroundColour());

    border_color.append(bd, StateColor::Hovered);

    background_color.append(BUTTON_BG_COL, StateColor::Disabled);
    background_color.append(BUTTON_PRESS_COL, StateColor::Pressed);
    background_color.append(BUTTON_BG_COL, StateColor::Hovered);
    background_color.append(BUTTON_BG_COL, StateColor::Normal);
    background_color.append(BUTTON_BG_COL, StateColor::Enabled);

    inner_background_color.append(BUTTON_IN_BG_COL, StateColor::Disabled);
    inner_background_color.append(BUTTON_PRESS_COL, StateColor::Pressed);
    inner_background_color.append(BUTTON_IN_BG_COL, StateColor::Hovered);
    inner_background_color.append(BUTTON_IN_BG_COL, StateColor::Normal);
    inner_background_color.append(BUTTON_IN_BG_COL, StateColor::Enabled);

    state_handler.attach({ &border_color, &background_color });
    state_handler.update_binds();
}

void AxisCtrlButton::updateParams() {
    r_outer = OUTER_SIZE;
    r_inner = INNER_SIZE;
    r_home = HOME_SIZE;
    r_blank = BLANK_SIZE;
    gap = GAP_SIZE;
}

void AxisCtrlButton::SetMinSize(const wxSize& size)
{
	wxSize cur_size = GetSize();
    if (size.GetWidth() > 0 && size.GetHeight() > 0) {
        stretch = std::min((double)size.GetWidth() / cur_size.x,(double)size.GetHeight() / cur_size.y);
		minSize = size;
        updateParams();
    }
    else if (size.GetWidth() > 0) {
		stretch = (double)size.GetWidth() / cur_size.x;
		minSize.x = size.x;
        updateParams();
    }
    else if (size.GetHeight() > 0) {
		stretch = (double)size.GetHeight() / cur_size.y;
		minSize.y = size.y;
        updateParams();
    }
    else {
		stretch = 1.0;
        minSize = wxSize(228, 228);
    }
    wxWindow::SetMinSize(minSize);
    center = wxPoint(minSize.x / 2, minSize.y / 2);
}

void AxisCtrlButton::SetTextColor(StateColor const &color)
{
    text_color = color;
    state_handler.update_binds();
    Refresh();
}

void AxisCtrlButton::SetBorderColor(StateColor const& color)
{
    border_color = color;
    state_handler.update_binds();
    Refresh();
}

void AxisCtrlButton::SetBackgroundColor(StateColor const& color)
{
    background_color = color;
    state_handler.update_binds();
    Refresh();
}

void AxisCtrlButton::SetInnerBackgroundColor(StateColor const& color)
{
    inner_background_color = color;
    state_handler.update_binds();
    Refresh();
}

void AxisCtrlButton::SetBitmap(ScalableBitmap &bmp)
{
    if (&bmp  && (& bmp.bmp()) && (bmp.bmp().IsOk())) {
        m_icon = bmp;
    }
}

void AxisCtrlButton::Rescale() {
	Refresh();
}

void AxisCtrlButton::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxPaintDC dc(this);
    wxGCDC gcdc(dc);
    render(gcdc);
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void AxisCtrlButton::render(wxDC& dc)
{
    wxGraphicsContext* gc = dc.GetGraphicsContext();

    int states = state_handler.states();
	wxSize size = GetSize();

    gc->PushState();
    gc->Translate(center.x, center.y);

	//draw the outer ring
    wxGraphicsPath outer_path = gc->CreatePath();
    outer_path.AddCircle(0, 0, r_outer);
    outer_path.AddCircle(0, 0, r_inner);
    gc->SetPen(StateColor::darkModeColorFor(BUTTON_BG_COL));
    gc->SetBrush(StateColor::darkModeColorFor(BUTTON_BG_COL));
    gc->DrawPath(outer_path);

	//draw the inner ring
    wxGraphicsPath inner_path = gc->CreatePath();
    inner_path.AddCircle(0, 0, r_inner);
    inner_path.AddCircle(0, 0, r_blank);
    gc->SetPen(StateColor::darkModeColorFor(BUTTON_IN_BG_COL));
    gc->SetBrush(StateColor::darkModeColorFor(BUTTON_IN_BG_COL));
	gc->DrawPath(inner_path);

	//draw an arc in corresponding position
	if (current_pos != CurrentPos::UNDEFINED) {
		wxGraphicsPath path = gc->CreatePath();
		if (current_pos < 4) {
			path.AddArc(0, 0, r_outer, (5 - 2 * current_pos) * PI / 4, (7 - 2 * current_pos) * PI / 4, true);
			path.AddArc(0, 0, r_inner, (7 - 2 * current_pos) * PI / 4, (5 - 2 * current_pos) * PI / 4, false);
			path.CloseSubpath();
			gc->SetBrush(wxBrush(background_color.colorForStates(states)));
		}
		else if (current_pos < 8) {
			path.AddArc(0, 0, r_inner, (5 - 2 * current_pos) * PI / 4, (7 - 2 * current_pos) * PI / 4, true);
			path.AddArc(0, 0, r_blank, (7 - 2 * current_pos) * PI / 4, (5 - 2 * current_pos) * PI / 4, false);
			path.CloseSubpath();
			gc->SetBrush(wxBrush(inner_background_color.colorForStates(states)));
        }
		gc->SetPen(wxPen(border_color.colorForStates(states),2));
		gc->DrawPath(path);
	}

	//draw rectangle gap
	gc->SetPen(blank_bg.colorForStates(StateColor::Normal));
	gc->SetBrush(blank_bg.colorForStates(StateColor::Normal));
	gc->PushState();
	gc->Rotate(-PI / 4);
	gc->DrawRectangle(-sqrt2 * size.x / 2, -sqrt2 * gap / 2, sqrt2 * size.x, sqrt2 * gap);
	gc->Rotate(-PI / 2);
	gc->DrawRectangle(-sqrt2 * size.x / 2, -sqrt2 * gap / 2, sqrt2 * size.x, sqrt2 * gap);
	gc->PopState();

	// draw the home circle
    wxGraphicsPath home_path = gc->CreatePath();
    home_path.AddCircle(0, 0, r_home);
    home_path.CloseSubpath();
    gc->PushState();
    if (current_pos == 8) {
        gc->SetPen(wxPen(border_color.colorForStates(states), 2));
        gc->SetBrush(wxBrush(background_color.colorForStates(states)));
    } else {
        gc->SetPen(StateColor::darkModeColorFor(BUTTON_BG_COL));
        gc->SetBrush(StateColor::darkModeColorFor(BUTTON_BG_COL));
    }
    gc->DrawPath(home_path);

    if (m_icon.bmp().IsOk()) {
        gc->DrawBitmap(m_icon.bmp(), -1 * m_icon.GetBmpWidth() / 2, -1 * m_icon.GetBmpHeight() / 2, m_icon.GetBmpWidth(), m_icon.GetBmpHeight());
    }
    gc->PopState();

	//draw linear border of the arc
	if (current_pos != CurrentPos::UNDEFINED) {
        gc->PushState();
        gc->SetPen(wxPen(border_color.colorForStates(states), 2));

        if (current_pos == 8) {
            wxGraphicsPath line_path = gc->CreatePath();
            line_path.AddCircle(0, 0, r_home);
            gc->StrokePath(line_path);
        } else {
            wxGraphicsPath line_path1 = gc->CreatePath();
            wxGraphicsPath line_path2 = gc->CreatePath();
            if (current_pos < 4) {
                line_path1.MoveToPoint(r_inner, -sqrt2 * gap / 2);
                line_path1.AddLineToPoint(r_outer, -sqrt2 * gap / 2);
                line_path2.MoveToPoint(-r_inner, -sqrt2 * gap / 2);
                line_path2.AddLineToPoint(-r_outer, -sqrt2 * gap / 2);
            } else if (current_pos < 8) {
                line_path1.MoveToPoint(r_blank, -sqrt2 * gap / 2);
                line_path1.AddLineToPoint(r_inner, -sqrt2 * gap / 2);
                line_path2.MoveToPoint(-r_blank, -sqrt2 * gap / 2);
                line_path2.AddLineToPoint(-r_inner, -sqrt2 * gap / 2);
            }
            gc->Rotate(-(1 + 2 * current_pos) * PI / 4);
            gc->StrokePath(line_path1);
            gc->Rotate(PI / 2);
            gc->StrokePath(line_path2);
        }
        gc->PopState();
	}

	//draw text
    if (!IsEnabled())
        gc->SetFont(Label::Body_12, text_color.colorForStates(StateColor::Disabled));
    else
	    gc->SetFont(Label::Head_12, text_color.colorForStates(states));
	wxDouble w, h;
	gc->GetTextExtent("Y", &w, &h);
	gc->DrawText(wxT("Y"), -w / 2, -r_outer + (r_outer - r_inner) / 2 - h / 2);
	gc->GetTextExtent("-X", &w, &h);
	gc->DrawText(wxT("-X"), -r_outer + (r_outer - r_inner) / 2 - w / 2, - h / 2);
	gc->GetTextExtent("-Y", &w, &h);
	gc->DrawText(wxT("-Y"), -w / 2, r_outer - (r_outer - r_inner) / 2 - h / 2);
	gc->GetTextExtent("X", &w, &h);
	gc->DrawText(wxT("X"), r_outer - (r_outer - r_inner) / 2 - w / 2, -h / 2);

	gc->SetFont(Label::Body_12, text_num_color);

	gc->PushState();
	gc->Rotate(PI / 4);
	gc->GetTextExtent("+10", &w, &h);
	gc->DrawText(wxT("+10"), sqrt2 * gap, -r_outer + (r_outer - r_inner) / 2 - h / 2);
	gc->GetTextExtent("+1", &w, &h);
	gc->DrawText(wxT("+1"), sqrt2 * gap, -r_inner + (r_inner - r_blank) / 2 - h / 2);
	gc->GetTextExtent("-1", &w, &h);
	gc->DrawText(wxT("-1"), sqrt2 * gap, r_inner - (r_inner - r_blank) / 2 - h / 2);
	gc->GetTextExtent("-10", &w, &h);
	gc->DrawText(wxT("-10"), sqrt2 * gap, r_outer - (r_outer - r_inner) / 2 - h / 2);
	gc->PopState();


	gc->PopState();
}

void AxisCtrlButton::mouseDown(wxMouseEvent& event)
{
    event.Skip();
    pressedDown = true;
    SetFocus();
    CaptureMouse();
}

void AxisCtrlButton::mouseReleased(wxMouseEvent& event)
{
    event.Skip();
    if (pressedDown) {
        pressedDown = false;
        ReleaseMouse();
        if (wxRect({ 0, 0 }, GetSize()).Contains(event.GetPosition()))
            sendButtonEvent();
    }
}

void AxisCtrlButton::mouseMoving(wxMouseEvent& event)
{
    if (pressedDown)
        return;
	wxPoint mouse_pos(event.GetX(), event.GetY());
	wxPoint transformed_mouse_pos = mouse_pos - center;
	double r_temp = transformed_mouse_pos.x * transformed_mouse_pos.x + transformed_mouse_pos.y * transformed_mouse_pos.y;
	if (r_temp > r_outer * r_outer) {
		current_pos = CurrentPos::UNDEFINED;
	}
	else if (r_temp > r_inner * r_inner) {
		if (transformed_mouse_pos.y < transformed_mouse_pos.x - gap && transformed_mouse_pos.y < -transformed_mouse_pos.x - gap)
		{
			current_pos = CurrentPos::OUTER_UP;
		}
		else if (transformed_mouse_pos.y > transformed_mouse_pos.x + gap && transformed_mouse_pos.y < -transformed_mouse_pos.x - gap)
		{
			current_pos = CurrentPos::OUTER_LEFT;
		}
		else if (transformed_mouse_pos.y > transformed_mouse_pos.x + gap && transformed_mouse_pos.y > -transformed_mouse_pos.x + gap)
		{
			current_pos = CurrentPos::OUTER_DOWN;
		}
		else if (transformed_mouse_pos.y < transformed_mouse_pos.x - gap && transformed_mouse_pos.y > -transformed_mouse_pos.x + gap)
		{
			current_pos = CurrentPos::OUTER_RIGHT;
		}
        else {
            current_pos = CurrentPos::UNDEFINED;
        }
	}
	else if (r_temp > r_blank * r_blank) {
		if (transformed_mouse_pos.y < transformed_mouse_pos.x - gap && transformed_mouse_pos.y < -transformed_mouse_pos.x - gap)
		{
			current_pos = CurrentPos::INNER_UP;
		}
		else if (transformed_mouse_pos.y > transformed_mouse_pos.x + gap && transformed_mouse_pos.y < -transformed_mouse_pos.x - gap)
		{
			current_pos = CurrentPos::INNER_LEFT;
		}
		else if (transformed_mouse_pos.y > transformed_mouse_pos.x + gap && transformed_mouse_pos.y > -transformed_mouse_pos.x + gap)
		{
			current_pos = CurrentPos::INNER_DOWN;
		}
		else if (transformed_mouse_pos.y < transformed_mouse_pos.x - gap && transformed_mouse_pos.y > -transformed_mouse_pos.x + gap)
		{
			current_pos = CurrentPos::INNER_RIGHT;
		}
        else {
            current_pos = CurrentPos::UNDEFINED;
        }
    } else if (r_temp <= r_home * r_home) {
        current_pos = INNER_HOME;
    }
	if (last_pos != current_pos) {
		last_pos = current_pos;
		Refresh();
	}
}

void AxisCtrlButton::sendButtonEvent()
{
    wxCommandEvent event(wxEVT_COMMAND_BUTTON_CLICKED, GetId());
    event.SetEventObject(this);
    event.SetInt(current_pos);
    GetEventHandler()->ProcessEvent(event);
}

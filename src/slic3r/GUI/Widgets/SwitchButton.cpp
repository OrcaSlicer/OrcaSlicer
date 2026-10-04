#include "SwitchButton.hpp"
#include "Button.hpp"
#include "Label.hpp"
#include "StaticBox.hpp"

#include "../wxExtensions.hpp"

#include <wx/event.h>
#include <wx/checklst.h>
#include <wx/gdicmn.h>
#include <wx/anybutton.h>
#include <utility>
#include <wx/colour.h>
#include <wx/image.h>
#include <cstring>
#include <wx/dc.h>
#include <vector>
#include "slic3r/GUI/Widgets/StateHandler.hpp"
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/tglbtn.h>
#include <wx/string.h>
#include <wx/types.h>
#include <wx/sizer.h>
#include "../GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "../Utils/MacDarkMode.hpp"
#include "../Utils/WxFontUtils.hpp"
#ifdef __APPLE__
#include "libslic3r/MacUtils.hpp"
#endif

#ifdef __WXGTK__
#include "../GUI_Utils.hpp"
#endif

#include <wx/dcmemory.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>

#include <algorithm>

wxDEFINE_EVENT(wxCUSTOMEVT_SWITCH_POS, wxCommandEvent);
wxDEFINE_EVENT(wxCUSTOMEVT_MULTISWITCH_SELECTION, wxCommandEvent);

SwitchButton::SwitchButton(wxWindow* parent, wxWindowID id)
	: wxBitmapToggleButton(parent, id, wxNullBitmap, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxBU_EXACTFIT)
	, m_on(this, "toggle_on", 16)
	, m_off(this, "toggle_off", 16)
    , text_color(std::pair{0xfffffe, (int) StateColor::Checked}, std::pair{0x6B6B6B, (int) StateColor::Normal})
	, track_color(0xD9D9D9)
    , thumb_color(std::pair{0x009688, (int) StateColor::Checked}, std::pair{0xD9D9D9, (int) StateColor::Normal})
{
	SetBackgroundColour(StaticBox::GetParentBackgroundColor(parent));
	Bind(wxEVT_TOGGLEBUTTON, [this](auto& e) { update(); e.Skip(); });
	SetFont(Label::Body_12);

#ifdef __WXGTK__
    Slic3r::GUI::RemoveButtonBorder(this);
#endif

	Rescale();
}

void SwitchButton::SetLabels(wxString const& lbl_on, wxString const& lbl_off)
{
	labels[0] = lbl_on;
	labels[1] = lbl_off;
	Rescale();
}

void SwitchButton::SetTextColor(StateColor const& color)
{
    text_color = color;
    Rescale();
}

void SwitchButton::SetTextColor2(StateColor const &color)
{
    text_color2 = color;
    Rescale();
}

void SwitchButton::SetTrackColor(StateColor const& color)
{
    track_color = color;
    Rescale();
}

void SwitchButton::SetThumbColor(StateColor const& color)
{
    thumb_color = color;
    Rescale();
}

void SwitchButton::SetValue(bool value)
{
    if (value != GetValue()) {
        wxBitmapToggleButton::SetValue(value);
        update();
    }
}

bool SwitchButton::SetBackgroundColour(const wxColour& colour)
{
    if (wxBitmapToggleButton::SetBackgroundColour(colour)) {
        Rescale();
        return true;
    }

    return false;
}

void SwitchButton::Rescale()
{
	if (labels[0].IsEmpty()) {
		m_on.msw_rescale();
		m_off.msw_rescale();
	}
	else {
        wxBitmapToggleButton::SetBackgroundColour(StaticBox::GetParentBackgroundColor(GetParent()));
#ifdef __WXOSX__
        auto scale = Slic3r::GUI::mac_max_scaling_factor();
        int BS = (int) scale;
#else
        constexpr int BS = 1;
#endif
		wxSize thumbSize;
		wxSize trackSize;
		wxClientDC dc(this);
#ifdef __WXOSX__
        dc.SetFont(dc.GetFont().Scaled(scale));
#endif
        wxSize textSize[2];
		{
			textSize[0] = dc.GetTextExtent(labels[0]);
			textSize[1] = dc.GetTextExtent(labels[1]);
		}
		float fontScale = 0;
		{
			thumbSize = textSize[0];
			auto size = textSize[1];
			if (size.x > thumbSize.x) thumbSize.x = size.x;
			else size.x = thumbSize.x;
			thumbSize.x += BS * 12;
			thumbSize.y += BS * 6;
			trackSize.x = thumbSize.x + size.x + BS * 10;
			trackSize.y = thumbSize.y + BS * 2;
            auto maxWidth = GetMaxWidth();
#ifdef __WXOSX__
            maxWidth *= scale;
#endif
			if (trackSize.x > maxWidth) {
                fontScale   = float(maxWidth) / trackSize.x;
                thumbSize.x -= (trackSize.x - maxWidth) / 2;
                trackSize.x = maxWidth;
			}
		}
		for (int i = 0; i < 2; ++i) {
			wxMemoryDC memdc(&dc);
#ifdef __WXMSW__
			wxBitmap bmp(trackSize.x, trackSize.y);
			memdc.SelectObject(bmp);
			memdc.SetBackground(wxBrush(GetBackgroundColour()));
			memdc.Clear();
#else
            wxImage image(trackSize);
            image.InitAlpha();
            memset(image.GetAlpha(), 0, trackSize.GetWidth() * trackSize.GetHeight());
            wxBitmap bmp(std::move(image));
            memdc.SelectObject(bmp);
#endif
            memdc.SetFont(dc.GetFont());
#ifdef __WXMSW__
            const double scale = GetDPIScaleFactor();
			fontScale = scale;
#endif
            if (fontScale) {
                memdc.SetFont(dc.GetFont().Scaled(fontScale));
                textSize[0] = memdc.GetTextExtent(labels[0]);
                textSize[1] = memdc.GetTextExtent(labels[1]);
			}
			auto state = i == 0 ? StateColor::Enabled : (StateColor::Checked | StateColor::Enabled);
            {
#ifdef __WXMSW__
				wxGCDC dc2(memdc);
#else
                wxDC &dc2(memdc);
#endif
				dc2.SetBrush(wxBrush(track_color.colorForStates(state)));
				dc2.SetPen(wxPen(track_color.colorForStates(state)));
                dc2.DrawRoundedRectangle(wxRect({0, 0}, trackSize), trackSize.y / 2.0);
				dc2.SetBrush(wxBrush(thumb_color.colorForStates(StateColor::Checked | StateColor::Enabled)));
				dc2.SetPen(wxPen(thumb_color.colorForStates(StateColor::Checked | StateColor::Enabled)));
                dc2.DrawRoundedRectangle(wxRect({ i == 0 ? BS : (trackSize.x - thumbSize.x - BS), BS}, thumbSize), thumbSize.y / 2.0);
			}
            memdc.SetTextForeground(text_color.colorForStates(state ^ StateColor::Checked));
            auto text_y = BS + (thumbSize.y - textSize[0].y) / 2;
#ifdef __APPLE__
            if (Slic3r::is_mac_version_15()) {
                text_y -= FromDIP(2);
            }
#endif
            memdc.DrawText(labels[0], {BS + (thumbSize.x - textSize[0].x) / 2, text_y});
            memdc.SetTextForeground(text_color2.count() == 0 ? text_color.colorForStates(state) : text_color2.colorForStates(state));
            auto text_y_1 = BS + (thumbSize.y - textSize[1].y) / 2;
#ifdef __APPLE__
            if (Slic3r::is_mac_version_15()) {
                text_y_1 -= FromDIP(2);
            }
#endif
            memdc.DrawText(labels[1], {trackSize.x - thumbSize.x - BS + (thumbSize.x - textSize[1].x) / 2, text_y_1});
			memdc.SelectObject(wxNullBitmap);
#ifdef __WXOSX__
            bmp = wxBitmap(bmp.ConvertToImage(), -1, scale);
#elif defined(__WXMSW__)
            bmp.SetScaleFactor(scale); // ORCA
#endif
			(i == 0 ? m_off : m_on).bmp() = bmp;
		}
	}
	update();
#ifdef __WXGTK__
	wxSize bestSize = GetBestSize();
	bestSize.IncTo(m_on.GetBmpSize());
	SetSize(bestSize);
	SetMinSize(bestSize);
#else
	SetSize(m_on.GetBmpSize());
#endif
}

void SwitchButton::update()
{
	SetBitmap((GetValue() ? m_on : m_off).bmp());
}

ModeSwitchButton::ModeSwitchButton(wxWindow* parent, wxWindowID id)
{
    background_color = StateColor(
        std::make_pair(wxColour("#D9D9D9"), (int) StateColor::Disabled),
        std::make_pair(wxColour("#D9D9D9"), (int) StateColor::Normal)
    );
    border_color = StateColor(
        std::make_pair(wxColour("#D9D9D9"), (int) StateColor::Disabled),
        std::make_pair(wxColour("#D9D9D9"), (int) StateColor::Hovered | ~StateColor::Focused),
        std::make_pair(wxColour("#26A69A"), (int) StateColor::Focused),
        std::make_pair(wxColour("#D9D9D9"), (int) StateColor::Normal)
    );
    track_background = StateColor(
        std::make_pair(wxColour("#009688"), (int) StateColor::Disabled),
        std::make_pair(wxColour("#009688"), (int) StateColor::Normal)
    );
    track_border = StateColor(
        std::make_pair(wxColour("#D9D9D9"), (int) StateColor::Disabled),
        std::make_pair(wxColour("#009688"), (int) StateColor::Hovered | ~StateColor::Focused),
        std::make_pair(wxColour("#26A69A"), (int) StateColor::Focused),
        std::make_pair(wxColour("#009688"), (int) StateColor::Normal)
    );
    dot_active = StateColor(
        std::make_pair(wxColour("#FFFEFE"), (int) StateColor::Disabled),
        std::make_pair(wxColour("#FFFEFE"), (int) StateColor::Normal)
    );
    dot_dimmed = StateColor(
        std::make_pair(wxColour("#EEEEEE"), (int) StateColor::Disabled),
        std::make_pair(wxColour("#EEEEEE"), (int) StateColor::Normal)
    );
    text_color = StateColor(
        std::make_pair(wxColour("#6B6B6B"), (int) StateColor::Disabled),
        std::make_pair(wxColour("#6B6B6B"), (int) StateColor::Normal)
    );

    state_handler.attach(std::vector<StateColor const*>{&dot_active, &dot_dimmed, &text_color});
    state_handler.update_binds();

    StaticBox::Create(parent, id, wxDefaultPosition, wxDefaultSize, 0);
    SetBackgroundColour(StaticBox::GetParentBackgroundColor(parent));
    SetCursor(wxCursor(wxCURSOR_HAND));

    m_tooltips[0] = _L("Simple settings");
    m_tooltips[1] = _L("Advanced settings");
    m_tooltips[2] = _L("Expert settings");
    m_tooltips[3] = _L("Developer mode") + "\n" + _L("Launch troubleshoot center") + "...";

    Bind(wxEVT_LEFT_DOWN, &ModeSwitchButton::mouseDown, this);
    Bind(wxEVT_LEFT_UP, &ModeSwitchButton::mouseReleased, this);
    Bind(wxEVT_LEFT_DCLICK, &ModeSwitchButton::mouseDown, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &ModeSwitchButton::mouseCaptureLost, this);

    Rescale();
}

void ModeSwitchButton::SetSelection(int selection)
{
    m_selection = std::clamp(selection, 0, 2);
    update_tooltip();
    Refresh();
}

void ModeSwitchButton::SelectAndNotify(int selection)
{
    if (m_dev_mode || !IsEnabled())
        return;

    SetSelection(selection);
    Slic3r::GUI::wxGetApp().save_mode(m_selection);
}

void ModeSwitchButton::Rescale()
{
    const wxSize button_size = FromDIP(wxSize(48, 18));
    SetMinSize(button_size);
    SetMaxSize(button_size);
    SetSize(button_size);
    SetCornerRadius(button_size.y / 2.0);
    Refresh();
}

bool ModeSwitchButton::Enable(bool enable /* = true */)
{
    const bool changed = StaticBox::Enable(enable);
    if (changed){
        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);
        m_enabled = enable; // IsEnabled() not works because variable changes after paint event
        Refresh();
    }
    return changed;
}

void ModeSwitchButton::SetDevMode(bool enable /* = true */)
{
    if (enable != m_dev_mode){
        m_dev_mode = enable;
        update_tooltip();
        Refresh();
    }
}

void ModeSwitchButton::doRender(wxDC& dc)
{
    const wxRect bounds = GetClientRect();
    if (bounds.width <= 0 || bounds.height <= 0)
        return;

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(GetBackgroundColour()));
    dc.DrawRectangle(bounds);

    int    states   = state_handler.states();
    double v_center = bounds.height / 2.0;

    // Background
    dc.SetPen(wxPen(border_color.colorForStates(states), 1));
    dc.SetBrush(wxBrush(background_color.colorForStates(states)));
    dc.DrawRoundedRectangle(bounds, v_center);

    if (!m_dev_mode) {
        double dot_dist = (bounds.width - bounds.height) * 0.50;

        // Track
        dc.SetPen(wxPen(track_border.colorForStates(states), 1));
        dc.SetBrush(wxBrush(track_background.colorForStates(states)));
        wxRect track_rc = bounds;
        track_rc.width = int(v_center * 2.0 + dot_dist * m_selection);
        dc.DrawRoundedRectangle(track_rc, v_center);

        // Dots
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (int idx = 0; idx < 3; ++idx) {
            dc.SetBrush(wxBrush((idx <= m_selection ? dot_active : dot_dimmed).colorForStates(states)));
            dc.DrawCircle(wxPoint(v_center + dot_dist * idx, v_center), track_rc.height * (double)(idx == m_selection ? 0.32 : 0.16));
        }
    }
    else { // Developer mode
        wxString str = "DEV";
        int kerning = 3; // pixels between chars
        dc.SetTextForeground(text_color.colorForStates(states));

        wxCoord totalWidth = 0;
        for (char c : str)
            totalWidth += dc.GetTextExtent(wxString(c)).x + kerning;
        totalWidth -= kerning;

        wxCoord x = bounds.x + (bounds.width - totalWidth) / 2;
        wxCoord y = bounds.y + (bounds.height - dc.GetTextExtent(str).y) / 2 - 1;

        for (char c : str) {
            wxString ch(c);
            dc.DrawText(ch, x, y);
            x += dc.GetTextExtent(ch).x + kerning;
        }
    }
}

void ModeSwitchButton::mouseDown(wxMouseEvent& event)
{
    if (m_dev_mode){
        Slic3r::GUI::wxGetApp().troubleshoot();
        return;
    }

    if (!IsEnabled()) {
        event.Skip();
        return;
    }

    m_pressed = true;
    if (!HasCapture())
        CaptureMouse();

    Refresh();

    event.Skip();
}

void ModeSwitchButton::mouseReleased(wxMouseEvent& event)
{
    if (m_pressed) {
        m_pressed = false;
        if (HasCapture())
            ReleaseMouse();

        if (GetClientRect().Contains(event.GetPosition()))
            SelectAndNotify(hit_test_selection(event.GetPosition()));

        Refresh();
    }

    event.Skip();
}

void ModeSwitchButton::mouseCaptureLost(wxMouseCaptureLostEvent& event)
{
    m_pressed = false;
    Refresh();
    event.Skip();
}

int ModeSwitchButton::hit_test_selection(const wxPoint& point) const
{
    const int width = std::max(1, GetClientSize().x);
    const int x = std::clamp(point.x, 0, width - 1);
    return std::clamp((x * 3) / width, 0, 2);
}

wxRect ModeSwitchButton::thumb_rect_for(int selection) const
{
    const wxRect bounds = GetClientRect().Deflate(3);
    const int thumb_diameter = std::max(FromDIP(10), bounds.height - FromDIP(2));
    const int y = bounds.y + (bounds.height - thumb_diameter) / 2;

    const int travel = std::max(0, bounds.width - thumb_diameter);
    const int x = bounds.x + (travel * std::clamp(selection, 0, 2)) / 2;
    return wxRect(x, y, thumb_diameter, thumb_diameter);
}

void ModeSwitchButton::update_tooltip()
{
    if (m_dev_mode)
        SetToolTip(m_tooltips[3]);
    else
        SetToolTip(m_tooltips[m_selection]);
}

SwitchBoard::SwitchBoard(wxWindow *parent, wxString leftL, wxString right, wxSize size)
 : wxWindow(parent, wxID_ANY, wxDefaultPosition, size)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    SetBackgroundColour(*wxWHITE);
	leftLabel = leftL;
    rightLabel = right;

	SetMinSize(size);
	SetMaxSize(size);

    Bind(wxEVT_PAINT, &SwitchBoard::paintEvent, this);
    Bind(wxEVT_LEFT_DOWN, &SwitchBoard::on_left_down, this);

    Bind(wxEVT_ENTER_WINDOW, [this](auto &e) { SetCursor(wxCURSOR_HAND); });
    Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) { SetCursor(wxCURSOR_ARROW); });
}

void SwitchBoard::updateState(wxString target)
{
    if (target.empty()) {
        if (!switch_left && !switch_right) {
            return;
        }

        switch_left = false;
        switch_right = false;
    } else {
        if (target == "left") {
            if (switch_left && !switch_right) {
                return;
            }

            switch_left = true;
            switch_right = false;
        } else if (target == "right") {
            if (!switch_left && switch_right) {
                return;
            }

            switch_left  = false;
            switch_right = true;
        }
    }

    Refresh();
}

void SwitchBoard::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    render(dc);
}

void SwitchBoard::render(wxDC &dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({0, 0}, size, &dc, {0, 0});

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void SwitchBoard::doRender(wxDC &dc)
{
    wxColour disable_color = wxColour(0xCECECE);

    dc.SetPen(*wxTRANSPARENT_PEN);

    if (is_enable) {dc.SetBrush(wxBrush(0xeeeeee));
    } else {dc.SetBrush(disable_color);}
    dc.DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, 8);

	/*left*/
    if (switch_left) {
        is_enable ? dc.SetBrush(wxBrush(wxColour(0, 150, 136))) : dc.SetBrush(disable_color);
        dc.DrawRoundedRectangle(0, 0, GetSize().x / 2, GetSize().y, 8);
	}

    if (switch_left) {
		dc.SetTextForeground(*wxWHITE);
    } else {
        dc.SetTextForeground(0x333333);
	}

    dc.SetFont(::Label::Body_13);
    Slic3r::GUI::WxFontUtils::get_suitable_font_size(0.6 * GetSize().GetHeight(), dc);

    auto left_txt_size = dc.GetTextExtent(leftLabel);
    dc.DrawText(leftLabel, wxPoint((GetSize().x / 2 - left_txt_size.x) / 2, (GetSize().y - left_txt_size.y) / 2));

	/*right*/
    if (switch_right) {
        if (is_enable) {dc.SetBrush(wxBrush(wxColour(0, 150, 136)));
        } else {dc.SetBrush(disable_color);}
        dc.DrawRoundedRectangle(GetSize().x / 2, 0, GetSize().x / 2, GetSize().y, 8);
	}

    auto right_txt_size = dc.GetTextExtent(rightLabel);
    if (switch_right) {
        dc.SetTextForeground(*wxWHITE);
    } else {
        dc.SetTextForeground(0x333333);
    }
    dc.DrawText(rightLabel, wxPoint((GetSize().x / 2 - right_txt_size.x) / 2 + GetSize().x / 2, (GetSize().y - right_txt_size.y) / 2));

}

void SwitchBoard::on_left_down(wxMouseEvent &evt)
{
    if (!is_enable) {
        return;
    }
    int index = -1;
    auto pos = ClientToScreen(evt.GetPosition());
    auto rect = ClientToScreen(wxPoint(0, 0));

    if (pos.x > 0 && pos.x < rect.x + GetSize().x / 2) {
        switch_left = true;
        switch_right = false;
        index = 1;
    } else {
        switch_left  = false;
        switch_right = true;
        index = 0;
    }

    if (auto_disable_when_switch)
    {
        is_enable = false;// make it disable while switching
    }
    Refresh();

    wxCommandEvent event(wxCUSTOMEVT_SWITCH_POS);
    event.SetInt(index);
    wxPostEvent(this, event);
}

bool SwitchBoard::Enable(bool enable /* = true */)
{
    if (is_enable == enable)
    {
        return false;
    }

    is_enable = enable;
    Refresh();
    return true;
}

MultiSwitchButton::MultiSwitchButton(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
    : StaticBox(parent, id, pos, size, style)
    , m_bg_color(StateColor(
          std::make_pair(0xD9D9D9, (int) StateColor::NotChecked),
          std::make_pair(0x009688, (int) StateColor::Normal)))
    , m_text_color(StateColor(
          std::make_pair(0x6B6B6B, (int) StateColor::NotChecked),
          std::make_pair(0xFFFFFE, (int) StateColor::Normal)))
    , m_br_color_modified(StateColor(
          std::make_pair(0xD9D9D9, (int) StateColor::NotChecked),
          std::make_pair(0xF1754F, (int) StateColor::Normal)))
    , m_fg_color_modified(StateColor(
          std::make_pair(0xF1754E, (int) StateColor::NotChecked),
          std::make_pair(0xFFFFFE, (int) StateColor::Normal)))
    , m_button_radius(10.0)
    , m_button_padding(FromDIP(wxSize(11, 3)))
{
    SetCornerRadius(m_button_radius);
    SetBorderWidth(0);

    // Orca: a switch can hold more buttons than the layout has room for (a toolchanger lists one per
    // tool), so they live in a scrolled area: the caller caps the switch at its natural width and
    // this scrolls horizontally instead of clipping the last buttons.
    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxBORDER_NONE);
    //m_scroll->SetBackgroundColour(wxColour("#D9D9D9"));
    m_scroll->SetBackgroundColour(GetBackgroundColour());
    // The buttons are a single row, so only the horizontal bar may ever appear: a vertical one would
    // eat into the row's height.
    m_scroll->ShowScrollbars(wxSHOW_SB_DEFAULT, wxSHOW_SB_NEVER);
    m_scroll->EnableScrolling(true, false);
    m_scroll->SetScrollRate(FromDIP(10), 0);

    sizer = new wxBoxSizer(wxHORIZONTAL);
    m_scroll->SetSizer(sizer);

    auto *hsizer = new wxBoxSizer(wxVERTICAL);
    hsizer->Add(m_scroll, 1, wxEXPAND);
    SetSizer(hsizer);
    SetMinSize(wxSize(-1, options_height()));

    Bind(wxEVT_SIZE, &MultiSwitchButton::on_size, this);
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MultiSwitchButton::button_clicked, this);
    // The tags name a tool and its volume type only, so they stay compact.
    SetFont(Label::Body_10);
}

MultiSwitchButton::~MultiSwitchButton()
{
    DeleteAllOptions();
}

int MultiSwitchButton::options_height() const
{
    // With no button to measure yet, keep the placeholder height the switch starts with.
    return btns.empty() ? FromDIP(20) : btns.front()->GetMinSize().y;
}

void MultiSwitchButton::update_scroll_range()
{
    // The scrollbar range is measured against the virtual size, so it has to follow the buttons
    // whenever their labels or count change.
    m_scroll->InvalidateBestSize();
    m_scroll->FitInside();

    // A scrolled window reports its min size plus a scrollbar as its best size, never the width of
    // the buttons it holds, so the layout has to be given that width explicitly. It is also the
    // widest this switch wants to be: a row with less room squeezes it below this and it scrolls.
    const wxSize content = sizer->CalcMin();
    SetMinSize(wxSize(content.x, options_height() + scrollbar_height(content.x)));
    SetMaxSize(m_fit_to_options ? wxSize(content.x, -1) : wxDefaultSize);
    InvalidateBestSize();
}

int MultiSwitchButton::scrollbar_height(int options_width) const
{
    // The bar is drawn inside the switch, so while the buttons need more width than the row gave us
    // the switch has to be taller by the bar's height, or the bar sits on top of the buttons.
    const int width = GetClientSize().x;
    if (width <= 0 || options_width <= width)
        return 0;

    const int bar = wxSystemSettings::GetMetric(wxSYS_HSCROLL_Y, this);
    return bar > 0 ? bar : 0;
}

void MultiSwitchButton::on_size(wxSizeEvent &evt)
{
    evt.Skip();

    // The row resized, so the buttons may now overflow it (or no longer fit in it) and the room the
    // bar needs changed with that. Width does not depend on height, so this settles in one pass.
    const int height = options_height() + scrollbar_height(sizer->CalcMin().x);
    if (GetMinSize().y != height) {
        SetMinSize(wxSize(GetMinSize().x, height));
        // The switch sits in a row of this tab, and the tab in a panel that shares its height
        // with the page view, so both have to lay out again for the taller row to get its room.
        if (wxWindow *tab = GetParent()) {
            tab->Layout();
            if (wxWindow *panel = tab->GetParent())
                panel->Layout();
        }
    }
}

void MultiSwitchButton::scroll_option_into_view(Button *btn)
{
    const int width  = m_scroll->GetClientSize().x;
    const int view_x = m_scroll->GetViewStartPixels().x;
    if (width <= 0)     // not laid out yet: there is no view to scroll
        return;

    const wxRect rect   = btn->GetRect();
    const int    right  = rect.GetRight() + 1;
    int          target = view_x;
    if (rect.x < view_x)
        target = rect.x;
    else if (right > view_x + width)
        target = right - width;

    if (target == view_x)
        return;

    // Scroll() counts scroll units; round up so the whole button ends up inside the view rather
    // than a few pixels short of it.
    int step = 1;
    m_scroll->GetScrollPixelsPerUnit(&step, nullptr);
    m_scroll->Scroll((target + step - 1) / step, -1);
}

int MultiSwitchButton::AppendOption(const wxString &option, void *clientData)
{
    Button *btn = new Button();
    btn->Create(m_scroll, option);
    btn->SetFont(GetFont());
    btn->SetBackgroundColor(m_bg_color);
    btn->SetBorderColor(m_bg_color);
    btn->SetTextColor(m_text_color);
    btn->SetCornerRadius(m_button_radius);
    btn->SetPaddingSize(m_button_padding);
    btn->SetClientData(clientData);

    btns.push_back(btn);
    btns_modified.push_back(false);
    sizer->Add(btn, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

    return int(btns.size()) - 1;
}

void MultiSwitchButton::SetOptions(const std::vector<wxString> &options)
{
    DeleteAllOptions();
    for (const auto &option : options)
        AppendOption(option);

    update_scroll_range();
    Layout();
    Refresh();
}

void MultiSwitchButton::DeleteAllOptions()
{
    sel = -1;
    for (auto *btn : btns) {
        if (btn)
            btn->Destroy();
    }
    btns.clear();
    btns_modified.clear();
    if (sizer)
        sizer->Clear();
}

unsigned int MultiSwitchButton::GetCount() const
{
    return (unsigned int) btns.size();
}

int MultiSwitchButton::GetSelection() const
{
    return sel;
}

void MultiSwitchButton::SetSelection(int index)
{
    if (index < 0 || index >= (int) btns.size() || index == sel)
        return;

    sel = index;
    update_button_styles();
    send_selection_event();
    // The selected button may be scrolled out of sight, e.g. when the tab restores the active tool.
    scroll_option_into_view(btns[sel]);
    Refresh();
}

wxString MultiSwitchButton::GetSelectedText() const
{
    return sel >= 0 && sel < (int) btns.size() ? btns[sel]->GetLabel() : wxString();
}

wxString MultiSwitchButton::GetOptionText(unsigned int index) const
{
    return index < btns.size() ? btns[index]->GetLabel() : wxString();
}

void MultiSwitchButton::SetOptionText(unsigned int index, const wxString &text)
{
    if (index >= btns.size())
        return;
    btns[index]->SetLabel(text);
    update_scroll_range();
}

void *MultiSwitchButton::GetOptionData(unsigned int index) const
{
    return index < btns.size() ? btns[index]->GetClientData() : nullptr;
}

void MultiSwitchButton::SetOptionData(unsigned int index, void *clientData)
{
    if (index >= btns.size())
        return;
    btns[index]->SetClientData(clientData);
}

void MultiSwitchButton::update_button_styles()
{
    for (int i = 0; i < (int) btns.size(); ++i) {
        btns[i]->SetValue(i == sel);
        btns[i]->SetBorderColor(btns_modified[i] ? m_br_color_modified : m_bg_color  );
        btns[i]->SetTextColor(  btns_modified[i] ? m_fg_color_modified : m_text_color); 
        btns[i]->Refresh();
    }
}

void MultiSwitchButton::SetBackgroundColor(const StateColor &color)
{
    m_bg_color = color;
    update_button_styles();
}

void MultiSwitchButton::SetTextColor(const StateColor &color)
{
    m_text_color = color;
    update_button_styles();
}

void MultiSwitchButton::SetButtonCornerRadius(double radius)
{
    m_button_radius = radius;
    SetCornerRadius(radius);
    for (auto *btn : btns)
        btn->SetCornerRadius(radius);
    Layout();
    Refresh();
}

void MultiSwitchButton::SetButtonPadding(const wxSize &padding)
{
    m_button_padding = padding;
    for (auto *btn : btns)
        btn->SetPaddingSize(padding);
    update_scroll_range();
    Layout();
    Refresh();
}

void MultiSwitchButton::SetModified(int index, bool modified){
    if(index < 0 || index >= btns_modified.size())
        return;
    btns_modified[index] = modified;
    update_button_styles();
}

void MultiSwitchButton::Rescale()
{
    for (auto *btn : btns)
        btn->Rescale();
    // Rescaling can change how the labels measure, and the scrollbar range follows the buttons.
    update_scroll_range();
}

void MultiSwitchButton::button_clicked(wxCommandEvent &event)
{
    SetFocus();
    auto *btn  = event.GetEventObject();
    auto  iter = std::find(btns.begin(), btns.end(), btn);
    SetSelection(iter == btns.end() ? -1 : int(iter - btns.begin()));
}

bool MultiSwitchButton::send_selection_event()
{
    wxCommandEvent evt(wxCUSTOMEVT_MULTISWITCH_SELECTION, GetId());
    evt.SetEventObject(this);
    evt.SetInt(sel);
    evt.SetString(GetSelectedText());
    GetEventHandler()->ProcessEvent(evt);
    return true;
}

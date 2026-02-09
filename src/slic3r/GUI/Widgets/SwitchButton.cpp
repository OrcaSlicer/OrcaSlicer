#include "SwitchButton.hpp"
#include "Label.hpp"
#include "StaticBox.hpp"

#include "../wxExtensions.hpp"
#include "../Utils/MacDarkMode.hpp"
#include "../Utils/WxFontUtils.hpp"
#include "../GUI_App.hpp"
#ifdef __APPLE__
#include "libslic3r/MacUtils.hpp"
#endif

#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>

wxDEFINE_EVENT(wxCUSTOMEVT_SWITCH_POS, wxCommandEvent);
wxDEFINE_EVENT(wxCUSTOMEVT_MULTISWITCH_SELECTION, wxCommandEvent);
wxDEFINE_EVENT(wxEXPAND_LEFT_DOWN, wxCommandEvent);

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
                dc2.DrawRoundedRectangle(wxRect({0, 0}, trackSize), trackSize.y / 2);
				dc2.SetBrush(wxBrush(thumb_color.colorForStates(StateColor::Checked | StateColor::Enabled)));
				dc2.SetPen(wxPen(thumb_color.colorForStates(StateColor::Checked | StateColor::Enabled)));
				dc2.DrawRoundedRectangle(wxRect({ i == 0 ? BS : (trackSize.x - thumbSize.x - BS), BS}, thumbSize), thumbSize.y / 2);
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
#endif
			(i == 0 ? m_off : m_on).bmp() = bmp;
		}
	}
	SetSize(m_on.GetBmpSize());
	update();
}

void SwitchButton::update()
{
	SetBitmap((GetValue() ? m_on : m_off).bmp());
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

void SwitchBoard::Enable()
{
    if (is_enable == true)
    {
        return;
    }

    is_enable = true;
    Refresh();
}

void SwitchBoard::Disable()
{
    if (is_enable == false)
    {
        return;
    }

    is_enable = false;
    Refresh();
}

CustomToggleButton::CustomToggleButton(wxWindow* parent, const wxString& label, wxWindowID id, const wxPoint& pos, const wxSize& size)
    : wxWindow(parent, id, pos, size), m_isSelected(false) {
    m_label = label;
    SetSelectedIcon("switch_send_mode_tag_on"); // Default icon
    SetUnSelectedIcon("switch_send_mode_tag_off"); // Default icon
    Connect(wxEVT_PAINT, wxPaintEventHandler(CustomToggleButton::OnPaint));
    Connect(wxEVT_SIZE, wxSizeEventHandler(CustomToggleButton::OnSize));
    Bind(wxEVT_LEFT_DOWN, &CustomToggleButton::on_left_down, this);
    SetBackgroundColour(*wxWHITE);
    Slic3r::GUI::wxGetApp().UpdateDarkUIWin(this);
}

void CustomToggleButton::on_left_down(wxMouseEvent& e)
{
    SetIsSelected(true);
}

void CustomToggleButton::SetLabel(const wxString& label) {
    m_label = label;
    Refresh();
}

void CustomToggleButton::SetSelectedIcon(const wxString& iconPath) {
    m_selected_icon = create_scaled_bitmap(iconPath.ToStdString(), nullptr,  16);
    Refresh();
}

void CustomToggleButton::SetUnSelectedIcon(const wxString& iconPath) {
    m_unselected_icon = create_scaled_bitmap(iconPath.ToStdString(), nullptr,  16);
    Refresh();
}

void CustomToggleButton::SetIsSelected(bool selected) {
    m_isSelected = selected;
    Refresh();
}


bool CustomToggleButton::IsSelected() const {
    return m_isSelected;
}

void CustomToggleButton::OnPaint(wxPaintEvent& event) {
    wxPaintDC dc(this);
    render(dc);
}

void CustomToggleButton::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

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

void CustomToggleButton::doRender(wxDC& dc)
{
    wxRect rect = GetClientRect();
    wxSize textRect = dc.GetMultiLineTextExtent(m_label);
    wxSize iconRect = m_selected_icon.GetSize();
    int iconRectWidth = iconRect.GetWidth();
    int iconRectHeight = iconRect.GetHeight();
#ifdef __APPLE__
    iconRectWidth = FromDIP(16);
    iconRectHeight = FromDIP(16);
#endif
    int left = (rect.GetSize().x -  textRect.GetWidth() - iconRectWidth - FromDIP(6)) / 2;

    // Draw background
    if (m_isSelected) {
        dc.SetBrush(wxBrush(m_secondary_colour));
        dc.SetPen(wxPen(m_primary_colour));
    }
    else {
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(wxColour("#EEEEEE")));
    }
    
    dc.DrawRoundedRectangle(rect, 5);

    // Draw icon
    if (m_isSelected) {
        if (m_selected_icon.IsOk()) {
            int iconY = (rect.GetHeight() - iconRectHeight) / 2;
            dc.DrawBitmap(m_selected_icon, left, iconY, true);
            left += iconRectWidth + FromDIP(6);
        }
    } else {
        if (m_unselected_icon.IsOk()) {
            int iconY = (rect.GetHeight() - iconRectHeight) / 2;
            dc.DrawBitmap(m_unselected_icon, left, iconY, true);
            left += iconRectWidth + FromDIP(6);
        }
    }

    // Draw text
    dc.SetFont(::Label::Head_13);

    if (m_isSelected) {
        dc.SetTextForeground(m_primary_colour);
    }
    else {
        dc.SetTextForeground(Slic3r::GUI::wxGetApp().dark_mode() ? *wxWHITE:wxColour("#5C5C5C"));
    }

    int textY = (rect.GetHeight() - dc.GetCharHeight()) / 2;
    dc.DrawText(m_label, left, textY);
}
void CustomToggleButton::OnSize(wxSizeEvent& event) {
    Refresh();
    event.Skip();
}

ExpandButton::ExpandButton(wxWindow* parent,  std::string bmp, wxWindowID id, const wxPoint& pos, const wxSize& size)
    : wxWindow(parent, id, pos, size)
{
    m_bmp_str = bmp;
    m_bmp = create_scaled_bitmap(m_bmp_str, this, 18);
    SetMinSize(wxSize(FromDIP(24), FromDIP(24)));
    SetMaxSize(wxSize(FromDIP(24), FromDIP(24)));
    Bind(wxEVT_PAINT, &ExpandButton::OnPaint, this);
    Bind(wxEVT_ENTER_WINDOW, [this](auto& e) { SetCursor(wxCURSOR_HAND); });
    Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) { SetCursor(wxCURSOR_ARROW); });
    Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        wxCommandEvent event(wxEXPAND_LEFT_DOWN);
        event.SetInt(GetId());
        wxPostEvent(GetParent(), event);
    });
}

void ExpandButton::update_bitmap(std::string bmp)
{
    m_bmp = create_scaled_bitmap(bmp, this, 18);
    Refresh();
}

void ExpandButton::msw_rescale() 
{
    m_bmp = create_scaled_bitmap(m_bmp_str, this, 18);
    Refresh();
}

void ExpandButton::OnPaint(wxPaintEvent& event) {
    wxPaintDC dc(this);
    render(dc);
}

void ExpandButton::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

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

void ExpandButton::doRender(wxDC& dc)
{
    wxSize size = GetSize();
    int left = (size.GetWidth() - FromDIP(18)) / 2;
    int top = (size.GetHeight() - FromDIP(18)) / 2;
    dc.DrawBitmap(m_bmp, left, top);
}

ExpandButtonHolder::ExpandButtonHolder(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size)
    : wxPanel(parent, id, pos, size)
{
#ifdef __APPLE__
    SetBackgroundColour(wxColour("#2D2D30"));
#else
    SetBackgroundColour(wxColour("#3B4446"));
#endif
    
    hsizer = new wxBoxSizer(wxHORIZONTAL);
    hsizer->AddStretchSpacer(1);
    vsizer = new wxBoxSizer(wxVERTICAL);

    vsizer->Add(hsizer, 0, wxALIGN_CENTER, 0);

    Bind(wxEVT_PAINT, &ExpandButtonHolder::OnPaint, this);
    Bind(wxEVT_ENTER_WINDOW, [=](auto& e) {
        auto a = 1;
        });

    SetSizer(vsizer);
    Layout();
    Fit();
}

void ExpandButtonHolder::addExpandButton(wxWindowID id, std::string img)
{
    ExpandButton* expand_program = new ExpandButton(this, img, id);
    hsizer->Add(expand_program, 0, wxALIGN_CENTER|wxALL, FromDIP(3));
    ShowExpandButton(id, true);
}

void ExpandButtonHolder::ShowExpandButton(wxWindowID id, bool show)
{
    wxWindowList& children = this->GetChildren();
    for (wxWindowList::iterator it = children.begin(); it != children.end(); ++it)
    {
        wxWindow* child = *it;
        if (!child) continue;
        ExpandButton* expandBtn = dynamic_cast<ExpandButton*>(child);
        if (expandBtn != nullptr)
        {
            if (expandBtn->GetId() == id) {
                expandBtn->Show(show);
            }
        }
    }

     int length = GetAvailable();

     for (wxWindowList::iterator it = children.begin(); it != children.end(); ++it)
     {
         wxWindow* child = *it;
         if (!child) continue;
         ExpandButton* expandBtn = dynamic_cast<ExpandButton*>(child);
         if (expandBtn != nullptr)
         {
             if (length <= 1) {
                 expandBtn->SetBackgroundColour(wxColour("#3B4446"));
             }
             else {

#ifdef __APPLE__
                expandBtn->SetBackgroundColour(wxColour("#384547"));
#else
                expandBtn->SetBackgroundColour(wxColour("#242E30"));
#endif

                 
             }
         }
     }

    SetMinSize(wxSize(length * FromDIP(24) + FromDIP(24) + (length - 1) * FromDIP(6), FromDIP(24)));
    SetMaxSize(wxSize(length * FromDIP(24) + FromDIP(24) + (length - 1) * FromDIP(6), FromDIP(24)));
    Layout();
    Fit();
}

void ExpandButtonHolder::updateExpandButtonBitmap(wxWindowID id, std::string bitmap)
{
    wxWindowList& children = this->GetChildren();
    for (wxWindowList::iterator it = children.begin(); it != children.end(); ++it)
    {
        wxWindow* child = *it;
        if (!child) continue;
        ExpandButton* expandBtn = dynamic_cast<ExpandButton*>(child);
        if (expandBtn != nullptr)
        {
            if (expandBtn->GetId() == id) {
                expandBtn->update_bitmap(bitmap);
            }   
        }
    }
}

void ExpandButtonHolder::EnableExpandButton(wxWindowID id, bool enb)
{
    wxWindowList& children = this->GetChildren();
    for (wxWindowList::iterator it = children.begin(); it != children.end(); ++it)
    {
        wxWindow* child = *it;
        if (!child) continue;
        ExpandButton* expandBtn = dynamic_cast<ExpandButton*>(child);
        if (expandBtn != nullptr)
        {
            if (expandBtn->GetId() == id) {
                expandBtn->Enable(enb);
            }
        }
    }
}

int ExpandButtonHolder::GetAvailable()
{
    int count = 0;
    wxWindowList& children = this->GetChildren();
    for (wxWindowList::iterator it = children.begin(); it != children.end(); ++it)
    {
        wxWindow* child = *it;
        if (!child) continue;
        ExpandButton* expandBtn = dynamic_cast<ExpandButton*>(child);
        if (expandBtn != nullptr)
        {
            if (expandBtn->IsShown()) {
                count++;
            }
        }
    }
    return count;
}

void ExpandButtonHolder::msw_rescale()
{
    wxWindowList& children = this->GetChildren();
    for (wxWindowList::iterator it = children.begin(); it != children.end(); ++it)
    {
        wxWindow* child = *it;
        if (!child) continue;
        ExpandButton* expandBtn = dynamic_cast<ExpandButton*>(child);
        if (expandBtn != nullptr)
        {
            expandBtn->msw_rescale();
        }
    }
    Refresh();
}

void ExpandButtonHolder::OnPaint(wxPaintEvent& event) {
    wxPaintDC dc(this);
    render(dc);
}

void ExpandButtonHolder::render(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

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

void ExpandButtonHolder::doRender(wxDC& dc)
{
    wxSize size = GetSize();
    
    if (GetAvailable() > 1) {
#ifdef __APPLE__
        dc.SetBrush(wxBrush(wxColour("#384547")));
        dc.SetPen(wxPen(wxColour("#384547")));
#else
        dc.SetBrush(wxBrush(wxColour("#242E30")));
        dc.SetPen(wxPen(wxColour("#242E30")));
#endif
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(10));
    }
}

MultiSwitchButton::MultiSwitchButton(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
    : StaticBox(parent, id, pos, size, style)
    , sel(-1)
    , m_bg_color(StateColor(
        std::make_pair(0xE8E8E8, (int) StateColor::NotChecked),
        std::make_pair(0x00AE42, (int) StateColor::Normal)))
    , m_bg_color_grayed(StateColor(
        std::make_pair(0xE8E8E8, (int) StateColor::NotChecked),
        std::make_pair(0x6DC48D, (int) StateColor::Normal)))
    , m_text_color(StateColor(
        std::make_pair(0x6B6B6B, (int) StateColor::NotChecked),
        std::make_pair(0xFFFFFE, (int) StateColor::Normal)))
    , m_text_color_grayed(StateColor(
        std::make_pair(0x999999, (int) StateColor::NotChecked),
        std::make_pair(0x99DFB2, (int) StateColor::Normal)))
    , m_button_radius(10.0)
    , m_button_padding(10, 6)
{
    SetCornerRadius(m_button_radius);
    SetBorderWidth(0);

    sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->AddSpacer(8);
    auto hsizer = new wxBoxSizer(wxVERTICAL);
    hsizer->Add(sizer, 1, wxEXPAND | wxTOP | wxBOTTOM, 0);
    SetSizer(hsizer);
    SetMinSize(wxSize(-1, 20));

    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MultiSwitchButton::button_clicked, this);

    SetFont(Label::Body_12);
}

MultiSwitchButton::~MultiSwitchButton()
{
    DeleteAllOptions();
}

int MultiSwitchButton::AppendOption(const wxString &option, void *clientData)
{
    Button *btn = new Button();
    btn->Create(this, option, "", wxBORDER_NONE);
    btn->SetFont(GetFont());

    int states = state_handler.states();
    wxColor color = m_bg_color.colorForStates(states);
    btn->SetBackgroundColour(color);
    btn->SetBackgroundColor(m_bg_color);
    btn->SetTextColor(m_text_color);
    btn->SetCornerRadius(m_button_radius);
    btn->SetPaddingSize(m_button_padding);
    btn->SetClientData(clientData);

    btns.push_back(btn);

    if (btns.size() > 1) { sizer->AddSpacer(0); }
    sizer->Add(btn, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    wxSize textSize = btn->GetTextExtent(option);
    wxSize minSize  = wxSize(textSize.x + m_button_padding.x * 2 + 6, -1);
    btn->SetMinSize(minSize);

    return btns.size() - 1;
}

void MultiSwitchButton::SetOptions(const std::vector<wxString>& options)
{
    DeleteAllOptions();
    for (const auto& option : options) {
        AppendOption(option);
    }
    sizer->AddSpacer(0);
    if (btns.size() == 1) {
        btns[0]->SetLeftCornerWhite();
        btns[0]->SetRightCornerWhite();
    } else if (btns.size() > 1) {
        btns.front()->SetLeftCornerWhite();
        btns.back()->SetRightCornerWhite();
    }
    Layout();
    Refresh();
}

void MultiSwitchButton::DeleteAllOptions()
{
    sel = -1;
    for (auto btn : btns) {
        if (btn) {
            btn->Destroy();
        }
    }
    btns.clear();
    sizer->Clear(true);
    sizer->AddSpacer(0);
}

unsigned int MultiSwitchButton::GetCount() const
{
    return btns.size();
}

int MultiSwitchButton::GetSelection() const
{
    return sel;
}

void MultiSwitchButton::SetSelection(int index)
{
    if (index < 0 || index >= (int) btns.size() || index == sel) {
        return;
    }

    sel = index;
    update_button_styles();
    send_selection_event();
    Refresh();
}

wxString MultiSwitchButton::GetSelectedText() const
{
    if (sel >= 0 && sel < (int)btns.size()) {
        return btns[sel]->GetLabel();
    }
    return wxString();
}

wxString MultiSwitchButton::GetOptionText(unsigned int index) const
{
    return index < btns.size() ? btns[index]->GetLabel() : wxString();
}

void MultiSwitchButton::SetOptionText(unsigned int index, const wxString &text)
{
    if (index >= btns.size()) return;
    btns[index]->SetLabel(text);
}

void *MultiSwitchButton::GetOptionData(unsigned int index) const
{
    if (index >= btns.size()) return nullptr;
    return btns[index]->GetClientData();
}

void MultiSwitchButton::SetOptionData(unsigned int index, void *client)
{
    if (index >= btns.size()) return;
    btns[index]->SetClientData(client);
}

void MultiSwitchButton::update_button_styles()
{
    for (int i = 0; i < (int) btns.size(); ++i) {
        btns[i]->SetValue(i == sel);

        auto bg_color   = btns[i]->IsGrayed() ? m_bg_color_grayed : m_bg_color;
        auto text_color = btns[i]->IsGrayed() ? m_text_color_grayed : m_text_color;
        btns[i]->SetBackgroundColor(bg_color);
        btns[i]->SetTextColor(text_color);
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
    for (auto *btn : btns) {
        btn->SetCornerRadius(radius);
    }
    Layout();
    Refresh();
}

void MultiSwitchButton::SetButtonPadding(const wxSize &padding)
{
    m_button_padding = padding;
    for (auto *btn : btns) {
        btn->SetPaddingSize(padding);
    }
    Layout();
    Refresh();
}

void MultiSwitchButton::Rescale()
{
    for (auto *btn : btns) {
        btn->Rescale();
    }
}

void MultiSwitchButton::button_clicked(wxCommandEvent &event)
{
    SetFocus();
    auto btn  = event.GetEventObject();
    auto iter = std::find(btns.begin(), btns.end(), btn);
    SetSelection(iter == btns.end() ? -1 : iter - btns.begin());
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
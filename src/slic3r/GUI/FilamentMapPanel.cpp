#include "FilamentMapPanel.hpp"
#include "GUI_App.hpp"
#include <wx/dcbuffer.h>
#include "wx/graphics.h"

namespace Slic3r { namespace GUI {

static const wxColour BgNormalColor  = wxColour("#FFFFFF");
static const wxColour BgSelectColor  = wxColour("#E5F0EE"); // ORCA
static const wxColour BgDisableColor = wxColour("#CECECE");

static const wxColour BorderNormalColor   = wxColour("#CECECE");
static const wxColour BorderSelectedColor = wxColour("#009688");
static const wxColour BorderDisableColor  = wxColour("#EEEEEE");

static const wxColour TextNormalBlackColor = wxColour("#262E30");
static const wxColour TextNormalGreyColor = wxColour("#6B6B6B");
static const wxColour TextDisableColor = wxColour("#CECECE");

FilamentMapManualPanel::FilamentMapManualPanel(wxWindow                       *parent,
                                               const std::vector<std::string> &color,
                                               const std::vector<std::string> &type,
                                               const std::vector<int>         &filament_list,
                                               const std::vector<int>         &filament_map)
    : wxPanel(parent), m_filament_map(filament_map), m_filament_color(color), m_filament_type(type), m_filament_list(filament_list)
{
    SetBackgroundColour(BgNormalColor);

    auto top_sizer = new wxBoxSizer(wxVERTICAL);

    m_description = new Label(this, _L("We will slice according to this grouping method:"));
    top_sizer->Add(m_description, 0, wxALIGN_LEFT | wxLEFT, FromDIP(15));
    top_sizer->AddSpacer(FromDIP(8));

    auto drag_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_left_panel  = new DragDropPanel(this, _L("Left Nozzle"), false);
    m_right_panel = new DragDropPanel(this, _L("Right Nozzle"), false);
    m_switch_btn  = new ScalableButton(this, wxID_ANY, "switch_filament_maps");

    for (size_t idx = 0; idx < m_filament_map.size(); ++idx) {
        auto iter = std::find(m_filament_list.begin(), m_filament_list.end(), idx + 1);
        if (iter == m_filament_list.end()) continue;
        wxColor color = Hex2Color(m_filament_color[idx]);
        std::string type = m_filament_type[idx];
        if (m_filament_map[idx] == 1) {
            m_left_panel->AddColorBlock(color, type, idx + 1);
        } else {
            assert(m_filament_map[idx] == 2);
            m_right_panel->AddColorBlock(color, type, idx + 1);
        }
    }
    m_left_panel->SetMinSize({ FromDIP(260),-1 });
    m_right_panel->SetMinSize({ FromDIP(260),-1 });

    drag_sizer->AddStretchSpacer();
    drag_sizer->Add(m_left_panel, 1, wxALIGN_CENTER | wxEXPAND);
    drag_sizer->AddSpacer(FromDIP(7));
    drag_sizer->Add(m_switch_btn, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(1));
    drag_sizer->AddSpacer(FromDIP(7));
    drag_sizer->Add(m_right_panel, 1, wxALIGN_CENTER | wxEXPAND);
    drag_sizer->AddStretchSpacer();

    top_sizer->Add(drag_sizer, 0, wxALIGN_CENTER | wxEXPAND);

    m_tips = new Label(this, _L("Tip: You can drag the filaments to reassign them to different nozzles."));
    m_tips->SetFont(Label::Body_14);
    m_tips->SetForegroundColour(TextNormalGreyColor);
    top_sizer->AddSpacer(FromDIP(20));
    top_sizer->Add(m_tips, 0, wxALIGN_LEFT | wxLEFT, FromDIP(15));

    m_switch_btn->Bind(wxEVT_BUTTON, &FilamentMapManualPanel::OnSwitchFilament, this);

    SetSizer(top_sizer);
    Layout();
    Fit();
    GUI::wxGetApp().UpdateDarkUIWin(this);
}

void FilamentMapManualPanel::OnSwitchFilament(wxCommandEvent &)
{
    auto left_blocks  = m_left_panel->get_filament_blocks();
    auto right_blocks = m_right_panel->get_filament_blocks();

    for (auto &block : left_blocks) {
        m_right_panel->AddColorBlock(block->GetColor(), block->GetType(), block->GetFilamentId(), false);
        m_left_panel->RemoveColorBlock(block, false);
    }

    for (auto &block : right_blocks) {
        m_left_panel->AddColorBlock(block->GetColor(), block->GetType(), block->GetFilamentId(), false);
        m_right_panel->RemoveColorBlock(block, false);
    }
    this->GetParent()->Layout();
    this->GetParent()->Fit();
}

void FilamentMapManualPanel::Hide()
{
    m_left_panel->Hide();
    m_right_panel->Hide();
    m_switch_btn->Hide();
    wxPanel::Hide();
}

void FilamentMapManualPanel::Show()
{
    m_left_panel->Show();
    m_right_panel->Show();
    m_switch_btn->Show();
    wxPanel::Show();
}

GUI::FilamentMapBtnPanel::FilamentMapBtnPanel(wxWindow *parent, const wxString &label, const wxString &detail, const std::string &icon) : wxPanel(parent)
{
    SetBackgroundColour(*wxWHITE);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_hover = false;

    const int horizontal_margin = FromDIP(12);

    auto sizer = new wxBoxSizer(wxVERTICAL);

    icon_enabled = create_scaled_bitmap(icon, nullptr, 20);
    icon_disabled = create_scaled_bitmap(icon + "_disabled", nullptr, 20);

    m_btn    = new wxBitmapButton(this, wxID_ANY, icon_enabled, wxDefaultPosition, wxDefaultSize, wxNO_BORDER);
    m_btn->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_label = new wxStaticText(this, wxID_ANY, label);
    m_label->SetFont(Label::Head_14);
    m_label->SetForegroundColour(TextNormalBlackColor);

    auto label_sizer = new wxBoxSizer(wxHORIZONTAL);
    label_sizer->AddStretchSpacer();
    label_sizer->Add(m_btn, 0, wxALIGN_CENTER | wxEXPAND | wxLEFT, FromDIP(1));
    label_sizer->Add(m_label, 0, wxALIGN_CENTER | wxEXPAND| wxALL, FromDIP(3));
    label_sizer->AddStretchSpacer();

    m_disable_tip = new Label(this, _L("(Sync with printer)"));

    sizer->AddSpacer(FromDIP(32));
    sizer->Add(label_sizer, 0, wxALIGN_CENTER | wxEXPAND);
    sizer->Add(m_disable_tip, 0, wxALIGN_CENTER);
    sizer->AddSpacer(FromDIP(3));

    auto detail_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_detail          = new Label(this, detail);
    m_detail->SetFont(Label::Body_12);
    m_detail->SetForegroundColour(TextNormalGreyColor);
    m_detail->Wrap(FromDIP(180));

    detail_sizer->AddStretchSpacer();
    detail_sizer->Add(m_detail, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, horizontal_margin);
    detail_sizer->AddStretchSpacer();

    sizer->Add(detail_sizer, 0, wxALIGN_CENTER | wxEXPAND);
    sizer->AddSpacer(FromDIP(10));

    SetSizer(sizer);
    Layout();
    Fit();

    GUI::wxGetApp().UpdateDarkUIWin(this);

    auto forward_click_to_parent = [this](wxMouseEvent &event) {
        wxCommandEvent click_event(wxEVT_LEFT_DOWN, GetId());
        click_event.SetEventObject(this);
        this->ProcessEvent(click_event);
    };

    m_btn->Bind(wxEVT_LEFT_DOWN, forward_click_to_parent);
    m_label->Bind(wxEVT_LEFT_DOWN, forward_click_to_parent);
    m_detail->Bind(wxEVT_LEFT_DOWN, forward_click_to_parent);

    Bind(wxEVT_PAINT, &FilamentMapBtnPanel::OnPaint, this);
    Bind(wxEVT_ENTER_WINDOW, &FilamentMapBtnPanel::OnEnterWindow, this);
    Bind(wxEVT_LEAVE_WINDOW, &FilamentMapBtnPanel::OnLeaveWindow, this);
}

void FilamentMapBtnPanel::OnPaint(wxPaintEvent &event)
{
    wxAutoBufferedPaintDC dc(this);
    wxGraphicsContext    *gc = wxGraphicsContext::Create(dc);

    if (gc) {
        dc.Clear();
        wxRect rect = GetClientRect();
        gc->SetBrush(wxTransparentColour);
        gc->DrawRoundedRectangle(0, 0, rect.width, rect.height, 0);
        wxColour bg_color = m_selected ? BgSelectColor : BgNormalColor;

        wxColour border_color = m_hover || m_selected ? BorderSelectedColor : BorderNormalColor;

        bg_color     = StateColor::darkModeColorFor(bg_color);
        border_color = StateColor::darkModeColorFor(border_color);
        gc->SetBrush(wxBrush(bg_color));
        gc->SetPen(wxPen(border_color, 1));
        gc->DrawRoundedRectangle(1, 1, rect.width - 2, rect.height - 2, 8);
        delete gc;
    }
}

void FilamentMapBtnPanel::UpdateStatus()
{
    if (m_selected) {
        m_btn->SetBackgroundColour(BgSelectColor);
        m_label->SetBackgroundColour(BgSelectColor);
        m_detail->SetBackgroundColour(BgSelectColor);
        m_disable_tip->SetBackgroundColour(BgSelectColor);
    }
    else {
        m_btn->SetBackgroundColour(BgNormalColor);
        m_label->SetBackgroundColour(BgNormalColor);
        m_detail->SetBackgroundColour(BgNormalColor);
        m_disable_tip->SetBackgroundColour(BgNormalColor);
    }
    if (!m_enabled) {
        m_disable_tip->SetLabel(_L("(Sync with printer)"));
        m_disable_tip->SetForegroundColour(TextDisableColor);
        m_btn->SetBitmap(icon_disabled);
        m_btn->SetForegroundColour(BgDisableColor);
        m_label->SetForegroundColour(TextDisableColor);
        m_detail->SetForegroundColour(TextDisableColor);
    }
    else {
        m_disable_tip->SetLabel("");
        m_disable_tip->SetForegroundColour(TextNormalBlackColor);
        m_btn->SetBitmap(icon_enabled);
        m_btn->SetForegroundColour(BgNormalColor);
        m_label->SetForegroundColour(TextNormalBlackColor);
        m_detail->SetForegroundColour(TextNormalGreyColor);
    }
    GUI::wxGetApp().UpdateDarkUIWin(this);
}

void FilamentMapBtnPanel::OnEnterWindow(wxMouseEvent &event)
{
    if (!m_hover && m_enabled) {
        m_hover = true;
        UpdateStatus();
        Refresh();
        event.Skip();
    }
}

void FilamentMapBtnPanel::OnLeaveWindow(wxMouseEvent &event)
{
    if (m_hover) {
        wxPoint pos = this->ScreenToClient(wxGetMousePosition());
        if (this->GetClientRect().Contains(pos)) return;
        m_hover = false;
        UpdateStatus();
        Refresh();
        event.Skip();
    }
}

bool FilamentMapBtnPanel::Enable(bool enable)
{
    m_enabled = enable;
    UpdateStatus();
    Refresh();
    return true;
}

void FilamentMapBtnPanel::Select(bool selected)
{
    m_selected = selected;
    UpdateStatus();
    Refresh();
}

void GUI::FilamentMapBtnPanel::Hide()
{
    m_btn->Hide();
    m_label->Hide();
    m_detail->Hide();
    wxPanel::Hide();
}
void GUI::FilamentMapBtnPanel::Show()
{
    m_btn->Show();
    m_label->Show();
    m_detail->Show();
    wxPanel::Show();
}

FilamentMapAutoPanel::FilamentMapAutoPanel(wxWindow *parent, FilamentMapMode mode, bool machine_synced) : wxPanel(parent)
{
    const wxString AutoForFlushDetail = _L("Generates filament grouping for the left and right nozzles based on the most filament-saving principles to minimize waste.");

    const wxString AutoForMatchDetail = _L("Generates filament grouping for the left and right nozzles based on the printer's actual filament status, reducing the need for manual filament adjustment.");

    auto                  sizer              = new wxBoxSizer(wxHORIZONTAL);
    m_flush_panel                            = new FilamentMapBtnPanel(this, _L("Filament-Saving Mode"), AutoForFlushDetail, "flush_mode_panel_icon");
    m_match_panel                            = new FilamentMapBtnPanel(this, _L("Convenience Mode"), AutoForMatchDetail, "match_mode_panel_icon");

    if (!machine_synced) m_match_panel->Enable(false);

    sizer->AddStretchSpacer();
    sizer->Add(m_flush_panel, 1, wxEXPAND);
    sizer->AddSpacer(FromDIP(12));
    sizer->Add(m_match_panel, 1, wxEXPAND);
    sizer->AddStretchSpacer();

    m_flush_panel->Bind(wxEVT_LEFT_DOWN, [this](auto& event) {
        if (m_flush_panel->IsEnabled()) {
            this->OnModeSwitch(FilamentMapMode::fmmAutoForFlush);
        }
    });

    m_match_panel->Bind(wxEVT_LEFT_DOWN, [this](auto &event) {
        if (m_match_panel->IsEnabled()) {
            this->OnModeSwitch(FilamentMapMode::fmmAutoForMatch);
        }
    });

    m_mode = mode;
    UpdateStatus();

    SetSizerAndFit(sizer);
    Layout();
    GUI::wxGetApp().UpdateDarkUIWin(this);
}
void FilamentMapAutoPanel::Hide()
{
    m_flush_panel->Hide();
    m_match_panel->Hide();
    wxPanel::Hide();
}

void FilamentMapAutoPanel::Show()
{
    m_flush_panel->Show();
    m_match_panel->Show();
    wxPanel::Show();
}

void FilamentMapAutoPanel::UpdateStatus()
{
    if (m_mode == fmmAutoForFlush) {
        m_flush_panel->Select(true);
        m_match_panel->Select(false);
    } else {
        m_flush_panel->Select(false);
        m_match_panel->Select(true);
    }
}

void FilamentMapAutoPanel::OnModeSwitch(FilamentMapMode mode)
{
    m_mode = mode;
    UpdateStatus();
}

FilamentMapDefaultPanel::FilamentMapDefaultPanel(wxWindow *parent) : wxPanel(parent)
{
    auto sizer = new wxBoxSizer(wxHORIZONTAL);

    m_label = new Label(this, _L("The filament grouping method for current plate is determined by the dropdown option at the slicing plate button."));
    m_label->SetFont(Label::Body_14);
    m_label->SetBackgroundColour(*wxWHITE);
    m_label->Wrap(FromDIP(500));

    sizer->AddStretchSpacer();
    sizer->Add(m_label, 1, wxEXPAND | wxALIGN_CENTER);
    sizer->AddStretchSpacer();

    SetSizerAndFit(sizer);
    Layout();
    GUI::wxGetApp().UpdateDarkUIWin(this);
}

void FilamentMapDefaultPanel::Hide()
{
    m_label->Hide();
    wxPanel::Hide();
}

void FilamentMapDefaultPanel::Show()
{
    m_label->Show();
    wxPanel::Show();
}

}} // namespace Slic3r::GUI

#include "PurgeModeDialog.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/settings.h>
#include <wx/dcbuffer.h>

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Label.hpp"
#include "wx/graphics.h"

namespace Slic3r { namespace GUI {

static const wxColour BgNormalColor  = wxColour("#FFFFFF");
static const wxColour BgSelectColor  = wxColour("#EBF9F0");
static const wxColour BgDisableColor = wxColour("#CECECE");

static const wxColour BorderNormalColor   = wxColour("#CECECE");
static const wxColour BorderSelectedColor = wxColour("#00AE42");
static const wxColour BorderDisableColor  = wxColour("#EEEEEE");

static const wxColour TextNormalBlackColor = wxColour("#262E30");
static const wxColour TextNormalGreyColor  = wxColour("#6B6B6B");
static const wxColour TextDisableColor     = wxColour("#CECECE");
static const wxColour TextErrorColor       = wxColour("#E14747");

PurgeModeDialog::PurgeModeDialog(wxWindow *parent) : DPIDialog(parent, wxID_ANY, _L("Purge Mode Settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);
    SetMinSize(wxSize(FromDIP(520), FromDIP(320)));
    SetMaxSize(wxSize(FromDIP(520), FromDIP(320)));
    std::string icon_path = (boost::format("%1%/images/BambuStudioTitle.ico") % Slic3r::resources_dir()).str();
    SetIcon(wxIcon(Slic3r::encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    auto main_sizer = new wxBoxSizer(wxVERTICAL);

    // Options panel
    auto options_panel = new wxPanel(this);
    auto options_sizer = new wxBoxSizer(wxVERTICAL);
    auto panels_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Get current mode from preset bundle
    auto           &preset_bundle = *wxGetApp().preset_bundle;
    auto            current_mode  = preset_bundle.project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode");
    PrimeVolumeMode mode          = current_mode ? current_mode->value : pvmDefault;
    m_selected_mode               = mode;

    // Standard option
    m_standard_panel = new PurgeModeBtnPanel(options_panel, _L("Standard Mode"), _L("Performs full priming for the best print quality. Requires more material and time."),
                                             "shield");
    m_standard_panel->SetMinSize(wxSize(FromDIP(200), FromDIP(150)));
    m_standard_panel->Select(mode == pvmDefault);
    m_standard_panel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { select_option(pvmDefault); });
    panels_sizer->Add(m_standard_panel, 1, wxEXPAND | wxRIGHT, FromDIP(12));

    // Purge Saving option
    m_saving_panel = new PurgeModeBtnPanel(options_panel, _L("Prime Saving"),
                                           _L("Reduces prime waste and prints faster. May cause slight color mixing or small surface defects."), "leaf");
    m_saving_panel->SetMinSize(wxSize(FromDIP(200), FromDIP(150)));
    m_saving_panel->Select(mode == pvmSaving);
    m_saving_panel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { select_option(pvmSaving); });
    panels_sizer->Add(m_saving_panel, 1, wxEXPAND | wxLEFT, FromDIP(12));

    options_sizer->Add(panels_sizer, 0, wxEXPAND | wxALL, FromDIP(20));

    // Learn more text
    auto wiki_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto learn_more_text = new wxStaticText(options_panel, wxID_ANY, _L("Learn more about prime mode"));
    learn_more_text->SetFont(Label::Body_12);
    learn_more_text->SetForegroundColour(wxColour("#6B6B6A"));
    wiki_sizer->Add(learn_more_text, 0, wxALIGN_CENTER_VERTICAL);
    auto wiki = new WikiPanel(options_panel);
    wiki->SetWikiUrl("https://e.bambulab.com/t?c=whk9cGnoWcJbji1F");
    wiki_sizer->Add(wiki, 0, wxLEFT, FromDIP(2));
    options_sizer->Add(wiki_sizer, 0, wxLEFT | wxRIGHT, FromDIP(20));

    options_panel->SetSizer(options_sizer);
    main_sizer->Add(options_panel, 1, wxEXPAND);

    auto btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();

    StateColor ok_btn_bg(
        std::pair<wxColour, int>(wxColour("#1B8844"), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour("#3DCB73"), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour("#00AE42"), StateColor::Normal)
    );
    StateColor ok_btn_text(
        std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Normal)
    );
    StateColor cancel_btn_bg(
        std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour("#EEEEEE"), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour("#FFFFFF"), StateColor::Normal)
    );
    StateColor cancel_btn_bd(
        std::pair<wxColour, int>(wxColour("#262E30"), StateColor::Normal)
    );
    StateColor cancel_btn_text(
        std::pair<wxColour, int>(wxColour("#262E30"), StateColor::Normal)
    );

    auto ok_btn = new Button(this, _L("Confirm"));
    ok_btn->SetMinSize(wxSize(FromDIP(62), FromDIP(24)));
    ok_btn->SetCornerRadius(FromDIP(12));
    ok_btn->SetBackgroundColor(ok_btn_bg);
    ok_btn->SetFont(Label::Body_12);
    ok_btn->SetBorderColor(wxColour("#00AE42"));
    ok_btn->SetTextColor(ok_btn_text);
    ok_btn->SetId(wxID_OK);

    auto cancel_btn = new Button(this, _L("Cancel"));
    cancel_btn->SetMinSize(wxSize(FromDIP(62), FromDIP(24)));
    cancel_btn->SetCornerRadius(FromDIP(12));
    cancel_btn->SetBackgroundColor(cancel_btn_bg);
    cancel_btn->SetFont(Label::Body_12);
    cancel_btn->SetBorderColor(cancel_btn_bd);
    cancel_btn->SetTextColor(cancel_btn_text);
    cancel_btn->SetId(wxID_CANCEL);

    btn_sizer->Add(ok_btn, 0, wxRIGHT, FromDIP(12));
    btn_sizer->Add(cancel_btn, 0);

    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, FromDIP(20));

    SetSizer(main_sizer);
    Fit();
    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);
}

void PurgeModeDialog::select_option(PrimeVolumeMode mode)
{
    m_selected_mode = mode;
    update_panel_selection();
}

void PurgeModeDialog::update_panel_selection()
{
    if (m_selected_mode == pvmDefault) {
        m_standard_panel->Select(true);
        m_saving_panel->Select(false);
    } else {
        m_standard_panel->Select(false);
        m_saving_panel->Select(true);
    }
}

void PurgeModeDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    const int &em = em_unit();

    msw_buttons_rescale(this, em, {wxID_OK, wxID_CANCEL});

    const wxSize &size = wxSize(70 * em, 32 * em);
    SetMinSize(size);

    Fit();
    Refresh();
}

GUI::PurgeModeBtnPanel::PurgeModeBtnPanel(wxWindow *parent, const wxString &label, const wxString &detail, const std::string &icon_path) : wxPanel(parent)
{
    SetBackgroundColour(*wxWHITE);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_hover = false;

    const int horizontal_margin = FromDIP(12);

    auto sizer = new wxBoxSizer(wxVERTICAL);

    icon = create_scaled_bitmap(icon_path, nullptr, 20);
    m_btn = new wxStaticBitmap(this, wxID_ANY, icon, wxDefaultPosition, wxDefaultSize, wxNO_BORDER);

    check_icon = create_scaled_bitmap("completed_2", nullptr, 20);
    m_check_btn = new wxStaticBitmap(this, wxID_ANY, check_icon, wxDefaultPosition, wxDefaultSize, wxNO_BORDER);
    m_check_btn->Hide();

    // icon
    auto icon_sizer = new wxBoxSizer(wxHORIZONTAL);
    icon_sizer->Add(m_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, horizontal_margin);
    icon_sizer->AddStretchSpacer();
    icon_sizer->Add(m_check_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, horizontal_margin);

    // label
    m_label = new wxStaticText(this, wxID_ANY, label);
    m_label->SetFont(Label::Head_14);
    m_label->SetForegroundColour(TextNormalBlackColor);

    auto label_sizer = new wxBoxSizer(wxHORIZONTAL);
    label_sizer->Add(m_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, horizontal_margin);

    // detail
    auto detail_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_detail          = new Label(this, detail);
    m_detail->SetFont(Label::Body_12);
    m_detail->SetForegroundColour(TextNormalGreyColor);
    m_detail->Wrap(FromDIP(200));
    detail_sizer->Add(m_detail, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, horizontal_margin);

    sizer->AddSpacer(FromDIP(15));
    sizer->Add(icon_sizer, 0, wxEXPAND);
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(label_sizer, 0, wxEXPAND);
    sizer->AddSpacer(FromDIP(6));
    sizer->Add(detail_sizer, 0, wxEXPAND);
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

    Bind(wxEVT_PAINT, &PurgeModeBtnPanel::OnPaint, this);
    Bind(wxEVT_ENTER_WINDOW, &PurgeModeBtnPanel::OnEnterWindow, this);
    Bind(wxEVT_LEAVE_WINDOW, &PurgeModeBtnPanel::OnLeaveWindow, this);
}

void PurgeModeBtnPanel::OnPaint(wxPaintEvent &event)
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

void PurgeModeBtnPanel::UpdateStatus()
{
    if (m_selected) {
        m_btn->SetBackgroundColour(BgSelectColor);
        m_label->SetBackgroundColour(BgSelectColor);
        m_detail->SetBackgroundColour(BgSelectColor);
        if (m_check_btn) {
            m_check_btn->SetBackgroundColour(BgSelectColor);
            m_check_btn->Show();
        }
    } else {
        m_btn->SetBackgroundColour(BgNormalColor);
        m_label->SetBackgroundColour(BgNormalColor);
        m_detail->SetBackgroundColour(BgNormalColor);
        if (m_check_btn) {
            m_check_btn->SetBackgroundColour(BgNormalColor);
            m_check_btn->Hide();
        }
    }
    Layout();
    Refresh();
    GUI::wxGetApp().UpdateDarkUIWin(this);
}

void PurgeModeBtnPanel::OnEnterWindow(wxMouseEvent &event)
{
    if (!m_hover) {
        m_hover = true;
        UpdateStatus();
        Refresh();
        event.Skip();
    }
}

void PurgeModeBtnPanel::OnLeaveWindow(wxMouseEvent &event)
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

void PurgeModeBtnPanel::Select(bool selected)
{
    m_selected = selected;
    UpdateStatus();
    Refresh();
}

}} // namespace Slic3r::GUI
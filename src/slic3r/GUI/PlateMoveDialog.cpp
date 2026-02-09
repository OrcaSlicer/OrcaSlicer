#include "PlateMoveDialog.hpp"
#include "MsgDialog.hpp"

namespace Slic3r { namespace GUI {

const StateColor btn_bg_green_in_plate_swap(std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed),
                        std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered),
                        std::pair<wxColour, int>(wxColour(0, 174, 66), StateColor::Normal));
const StateColor btn_bg_disable_bg_in_plate_swap(std::pair<wxColour, int>(wxColour(205, 201, 201), StateColor::Pressed),
                                   std::pair<wxColour, int>(wxColour(205, 201, 201), StateColor::Hovered),
                                   std::pair<wxColour, int>(wxColour(205, 201, 201), StateColor::Normal));
PlateMoveDialog::PlateMoveDialog(wxWindow *parent, wxWindowID id, const wxString &title, const wxPoint &pos, const wxSize &size, long style)
    : DPIDialog(parent, id, title, pos, size, style)
{
    std::string icon_path = (boost::format("%1%/images/BambuStudioTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    SetBackgroundColour(*wxWHITE);
    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);
    auto        m_line_top   = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(305), -1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);
    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(5));

    m_sizer_main->AddSpacer(FromDIP(15));

    init_bitmaps();
    auto layout_sizer  = new wxBoxSizer(wxHORIZONTAL);
    auto content_sizer = new wxBoxSizer(wxVERTICAL);
    auto text_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto combox_sizer = new wxBoxSizer(wxHORIZONTAL);
    {
        wxStaticText *temp_title = new wxStaticText(this, wxID_ANY, _L("Move the current plate to"));
        text_sizer->Add(temp_title, 0, wxEXPAND | wxALL, FromDIP(5));
    }
    {//                                                           icon_name
        m_swipe_left_button = new ScalableButton(this, wxID_ANY, wxGetApp().dark_mode() ? "previous_normal_dark" : "previous_normal", wxEmptyString, wxDefaultSize, wxDefaultPosition,
                                                 wxBU_EXACTFIT | wxNO_BORDER, true, m_bmp_pix_cont);
        m_swipe_left_button->SetToolTip(_L("Move forward"));
        m_swipe_left_button->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
            if (!m_swipe_left_button_enable) { return; }
            m_swipe_left_button->SetBitmap(m_swipe_left_bmp_hover.bmp());
        });
        m_swipe_left_button->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
            if (!m_swipe_left_button_enable) { return; }
            m_swipe_left_button->SetBitmap(m_swipe_left_bmp_normal.bmp());
        });
        m_swipe_left_button->Bind(wxEVT_BUTTON, &PlateMoveDialog::on_previous_plate, this);
    }
    {
        m_swipe_right_button = new ScalableButton(this, wxID_ANY, wxGetApp().dark_mode() ? "next_normal_dark" : "next_normal", wxEmptyString, wxDefaultSize, wxDefaultPosition,
                                                  wxBU_EXACTFIT | wxNO_BORDER, true, m_bmp_pix_cont);
        m_swipe_right_button->SetToolTip(_L("Move backwards"));
        m_swipe_right_button->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
            if (!m_swipe_right_button_enable) { return; }
            m_swipe_right_button->SetBitmap(m_swipe_right_bmp_hover.bmp());
        });
        m_swipe_right_button->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
            if (!m_swipe_right_button_enable) { return; }
            m_swipe_right_button->SetBitmap(m_swipe_right_bmp_normal.bmp());
        });
        m_swipe_right_button->Bind(wxEVT_BUTTON, &PlateMoveDialog::on_next_plate, this);
    }
    {
        m_swipe_frontmost_button = new ScalableButton(this, wxID_ANY, wxGetApp().dark_mode() ? "frontmost_normal_dark" : "frontmost_normal", wxEmptyString, wxDefaultSize,
                                                      wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true, m_bmp_pix_cont);
        m_swipe_frontmost_button->SetToolTip(_L("Move to the front"));
        m_swipe_frontmost_button->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
            if (!m_swipe_frontmost_button_enable) { return; }
            m_swipe_frontmost_button->SetBitmap(m_swipe_frontmost_bmp_hover.bmp());
        });
        m_swipe_frontmost_button->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
            if (!m_swipe_frontmost_button_enable) { return; }
            m_swipe_frontmost_button->SetBitmap(m_swipe_frontmost_bmp_normal.bmp());
        });
        m_swipe_frontmost_button->Bind(wxEVT_BUTTON, &PlateMoveDialog::on_frontmost_plate, this);
    }
    {
        m_swipe_backmost_button = new ScalableButton(this, wxID_ANY, wxGetApp().dark_mode() ?  "backmost_normal_dark":"backmost_normal", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true,m_bmp_pix_cont);
        m_swipe_backmost_button->SetToolTip(_L("Move to the back"));
        m_swipe_backmost_button->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
            if (!m_swipe_backmost_button_enable) { return; }
            m_swipe_backmost_button->SetBitmap(m_swipe_backmost_bmp_hover.bmp());
        });
        m_swipe_backmost_button->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
            if (!m_swipe_backmost_button_enable) { return; }
            m_swipe_backmost_button->SetBitmap(m_swipe_backmost_bmp_normal.bmp());
        });
        m_swipe_backmost_button->Bind(wxEVT_BUTTON, &PlateMoveDialog::on_backmost_plate, this);
    }

    GUI::PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
    GUI::PartPlate *    curr_plate = GUI::wxGetApp().plater()->get_partplate_list().get_selected_plate();
    m_specify_plate_idx            = GUI::wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
    for (size_t i = 0; i < plate_list.get_plate_count(); i++) {
        if (i < 9) {
            m_plate_number_choices_str.Add("0" + std::to_wstring(i + 1));
        } else if (i == 9) {
            m_plate_number_choices_str.Add("10");
        } else {
            m_plate_number_choices_str.Add(std::to_wstring(i + 1));
        }
        m_plate_choices.emplace_back(i);
    }

    m_combobox_plate = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1), 0, NULL, wxCB_READONLY);
    m_combobox_plate->Bind(wxEVT_COMBOBOX, [this](auto &e) {
        if (e.GetSelection() < m_plate_choices.size()) {
            m_specify_plate_idx = e.GetSelection();
            update_swipe_button_state();
        }
    });
    combox_sizer->Add(m_swipe_frontmost_button, 0, wxALIGN_LEFT | wxEXPAND | wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    combox_sizer->Add(m_swipe_left_button, 0, wxALIGN_LEFT | wxEXPAND | wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    combox_sizer->Add(m_combobox_plate, 0, wxALIGN_LEFT | wxEXPAND | wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    combox_sizer->Add(m_swipe_right_button, 0, wxALIGN_LEFT | wxEXPAND | wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    combox_sizer->Add(m_swipe_backmost_button, 0, wxALIGN_LEFT | wxEXPAND | wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

    content_sizer->Add(text_sizer, 0, wxEXPAND | wxLEFT, FromDIP(0));
    content_sizer->Add(combox_sizer, 0, wxEXPAND | wxLEFT, FromDIP(0));

    layout_sizer->AddStretchSpacer();
    layout_sizer->Add(content_sizer, 0, wxEXPAND | wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(0));
    layout_sizer->AddStretchSpacer();
    m_sizer_main->Add(layout_sizer, 0, wxEXPAND);
    m_sizer_main->AddSpacer(FromDIP(10));

    auto  sizer_button = new wxBoxSizer(wxHORIZONTAL);

    StateColor btn_bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed), std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                            std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));

    m_button_ok = new Button(this, _L("OK"));
    //m_button_ok->SetBackgroundColor(btn_bg_green_in_plate_swap);
    m_button_ok->SetBorderColor(*wxWHITE);
    m_button_ok->SetTextColor(wxColour("#FFFFFE"));
    m_button_ok->SetFont(Label::Body_12);
    m_button_ok->SetSize(wxSize(FromDIP(58), FromDIP(24)));
    m_button_ok->SetMinSize(wxSize(FromDIP(58), FromDIP(24)));
    m_button_ok->SetCornerRadius(FromDIP(12));
    m_button_ok->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent  &e) {
        if (this->IsModal())
            EndModal(wxID_YES);
        else
            this->Close();
    });

    m_button_cancel = new Button(this, _L("Cancel"));
    m_button_cancel->SetBackgroundColor(btn_bg_white);
    m_button_cancel->SetBorderColor(wxColour(38, 46, 48));
    m_button_cancel->SetFont(Label::Body_12);
    m_button_cancel->SetSize(wxSize(FromDIP(58), FromDIP(24)));
    m_button_cancel->SetMinSize(wxSize(FromDIP(58), FromDIP(24)));
    m_button_cancel->SetCornerRadius(FromDIP(12));
    m_button_cancel->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent &e) {
        if (this->IsModal())
            EndModal(wxID_NO);
        else
            this->Close();
    });

    sizer_button->AddStretchSpacer();
    sizer_button->Add(m_button_ok, 0, wxALL, FromDIP(5));
    sizer_button->Add(m_button_cancel, 0, wxALL, FromDIP(5));
    sizer_button->Add(FromDIP(15), 0, 0, 0);

    m_sizer_main->AddSpacer(FromDIP(15));
    m_sizer_main->Add(sizer_button, 0, wxEXPAND, FromDIP(20));
    m_sizer_main->AddSpacer(FromDIP(15));

    SetSizer(m_sizer_main);
    //update ui
    update_plate_combox();
    update_swipe_button_state();

    m_swipe_left_button->SetBitmapDisabled_(m_swipe_left_bmp_disable);
    m_swipe_right_button->SetBitmapDisabled_(m_swipe_right_bmp_disable);
    m_swipe_frontmost_button->SetBitmapDisabled_(m_swipe_frontmost_bmp_disable);
    m_swipe_backmost_button->SetBitmapDisabled_(m_swipe_backmost_bmp_disable);

    Layout();
    Fit();
    //m_sizer_main->Fit(this);

    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);
}

PlateMoveDialog::~PlateMoveDialog() {
}

void PlateMoveDialog::update_ok_button_enable() {
    auto disable = m_specify_plate_idx == GUI::wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
    m_button_ok->Enable(!disable);
    m_button_ok->SetBackgroundColor(disable ? btn_bg_disable_bg_in_plate_swap :btn_bg_green_in_plate_swap);
}

void PlateMoveDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    m_button_ok->Rescale();
    m_button_cancel->Rescale();
    m_swipe_left_button->msw_rescale();
}

void PlateMoveDialog::init_bitmaps() {
    if (wxGetApp().dark_mode()) {
        m_swipe_left_bmp_normal   = ScalableBitmap(this, "previous_normal_dark", m_bmp_pix_cont);
        m_swipe_left_bmp_hover    = ScalableBitmap(this, "previous_hover_dark", m_bmp_pix_cont);
        m_swipe_left_bmp_disable  = ScalableBitmap(this, "previous_disable_dark", m_bmp_pix_cont);
        m_swipe_right_bmp_normal  = ScalableBitmap(this, "next_normal_dark", m_bmp_pix_cont);
        m_swipe_right_bmp_hover   = ScalableBitmap(this, "next_hover_dark", m_bmp_pix_cont);
        m_swipe_right_bmp_disable = ScalableBitmap(this, "next_disable_dark", m_bmp_pix_cont);

        m_swipe_frontmost_bmp_normal   = ScalableBitmap(this, "frontmost_normal_dark", m_bmp_pix_cont);
        m_swipe_frontmost_bmp_hover    = ScalableBitmap(this, "frontmost_hover_dark", m_bmp_pix_cont);
        m_swipe_frontmost_bmp_disable  = ScalableBitmap(this, "frontmost_disable_dark", m_bmp_pix_cont);
        m_swipe_backmost_bmp_normal    = ScalableBitmap(this, "backmost_normal_dark", m_bmp_pix_cont);
        m_swipe_backmost_bmp_hover     = ScalableBitmap(this, "backmost_hover_dark", m_bmp_pix_cont);
        m_swipe_backmost_bmp_disable   = ScalableBitmap(this, "backmost_disable_dark", m_bmp_pix_cont);
    } else {
        m_swipe_left_bmp_normal   = ScalableBitmap(this, "previous_normal", m_bmp_pix_cont);
        m_swipe_left_bmp_hover    = ScalableBitmap(this, "previous_hover", m_bmp_pix_cont);
        m_swipe_left_bmp_disable  = ScalableBitmap(this, "previous_disable", m_bmp_pix_cont);
        m_swipe_right_bmp_normal  = ScalableBitmap(this, "next_normal", m_bmp_pix_cont);
        m_swipe_right_bmp_hover   = ScalableBitmap(this, "next_hover", m_bmp_pix_cont);
        m_swipe_right_bmp_disable = ScalableBitmap(this, "next_disable", m_bmp_pix_cont);

        m_swipe_frontmost_bmp_normal = ScalableBitmap(this, "frontmost_normal", m_bmp_pix_cont);
        m_swipe_frontmost_bmp_hover  = ScalableBitmap(this, "frontmost_hover", m_bmp_pix_cont);
        m_swipe_frontmost_bmp_disable = ScalableBitmap(this, "frontmost_disable", m_bmp_pix_cont);
        m_swipe_backmost_bmp_normal   = ScalableBitmap(this, "backmost_normal", m_bmp_pix_cont);
        m_swipe_backmost_bmp_hover    = ScalableBitmap(this, "backmost_hover", m_bmp_pix_cont);
        m_swipe_backmost_bmp_disable  = ScalableBitmap(this, "backmost_disable", m_bmp_pix_cont);
    }
}
void PlateMoveDialog::update_swipe_button_state() {
    m_swipe_left_button_enable = true;
    m_swipe_left_button->Enable();
    m_swipe_left_button->SetBitmap(m_swipe_left_bmp_normal.bmp());
    m_swipe_right_button_enable = true;
    m_swipe_right_button->Enable();
    m_swipe_right_button->SetBitmap(m_swipe_right_bmp_normal.bmp());
    if (m_combobox_plate->GetSelection() == 0) { // auto plate_index = m_plate_choices[m_combobox_plate->GetSelection()];
        m_swipe_left_button->Disable(); // SetBitmap(m_swipe_left_bmp_disable.bmp());
        m_swipe_left_button_enable = false;
    }
    if (m_combobox_plate->GetSelection() == m_combobox_plate->GetCount() - 1) {
        m_swipe_right_button->Disable(); // m_swipe_right_button->set(m_swipe_right_bmp_disable.bmp());
        m_swipe_right_button_enable = false;
    }

    m_swipe_frontmost_button_enable = true;
    m_swipe_frontmost_button->Enable();
    m_swipe_frontmost_button->SetBitmap(m_swipe_frontmost_bmp_normal.bmp());
    m_swipe_backmost_button_enable = true;
    m_swipe_backmost_button->Enable();
    m_swipe_backmost_button->SetBitmap(m_swipe_backmost_bmp_normal.bmp());
    if (m_combobox_plate->GetSelection() == 0) { // auto plate_index = m_plate_choices[m_combobox_plate->GetSelection()];
        m_swipe_frontmost_button->Disable();  // SetBitmap(m_swipe_frontmost_bmp_disable.bmp());
        m_swipe_frontmost_button_enable = false;
    }
    if (m_combobox_plate->GetSelection() == m_combobox_plate->GetCount() - 1) {
        m_swipe_backmost_button->Disable(); // m_swipe_backmost_button->SetBitmap(m_swipe_backmost_bmp_disable.bmp());
        m_swipe_backmost_button_enable = false;
    }

    update_ok_button_enable();
}

void PlateMoveDialog::on_previous_plate(wxCommandEvent &event)
{
    auto cobox_idx = m_combobox_plate->GetSelection();
    cobox_idx--;
    if (cobox_idx < 0) { return; }
    m_combobox_plate->SetSelection(cobox_idx);
    m_specify_plate_idx = cobox_idx;

    update_swipe_button_state();
}

void PlateMoveDialog::on_next_plate(wxCommandEvent &event) {
    auto cobox_idx = m_combobox_plate->GetSelection();
    cobox_idx++;
    if (cobox_idx >= (int) m_combobox_plate->GetCount()) { return; }
    m_combobox_plate->SetSelection(cobox_idx);
    m_specify_plate_idx = cobox_idx;

    update_swipe_button_state();
}

void PlateMoveDialog::on_frontmost_plate(wxCommandEvent &event) {
    m_combobox_plate->SetSelection(0);
    m_specify_plate_idx = 0;

    update_swipe_button_state();
}

void PlateMoveDialog::on_backmost_plate(wxCommandEvent &event) {
    auto cobox_idx = m_combobox_plate->GetCount() - 1;
    m_combobox_plate->SetSelection(cobox_idx);
    m_specify_plate_idx = cobox_idx;

    update_swipe_button_state();
}

void PlateMoveDialog::update_plate_combox()
{
    if (m_combobox_plate) {
        m_combobox_plate->Clear();
        for (size_t i = 0; i < m_plate_number_choices_str.size(); i++) {
            m_combobox_plate->Append(m_plate_number_choices_str[i]);
        }
        auto iter = std::find(m_plate_choices.begin(), m_plate_choices.end(), m_specify_plate_idx);
        if (iter != m_plate_choices.end()) {
            auto index = iter - m_plate_choices.begin();
            m_combobox_plate->SetSelection(index);
        }
    }
}
}
} // namespace Slic3r::GUI
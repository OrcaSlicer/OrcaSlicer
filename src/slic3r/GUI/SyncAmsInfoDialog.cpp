#include "SyncAmsInfoDialog.hpp"

#include <thread>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/dcmemory.h>
#include "GUI_App.hpp"
#include "Tab.hpp"
#include "PartPlate.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/TextInput.hpp"
#include "Notebook.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/PlaterWorker.hpp"
#include <chrono>
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "CapsuleButton.hpp"
#include "PrePrintChecker.hpp"

#include "DeviceCore/DevConfig.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevMapping.h"
#include "DeviceCore/DevStorage.h"

using namespace Slic3r;
using namespace Slic3r::GUI;

#define OK_BUTTON_SIZE wxSize(FromDIP(90), FromDIP(24))
#define CANCEL_BUTTON_SIZE wxSize(FromDIP(58), FromDIP(24))
#define SyncAmsInfoDialogWidth  FromDIP(675)
#define SyncAmsInfoDialogHeightMIN FromDIP(620)
#define SyncAmsInfoDialogHeightMIDDLE FromDIP(630)
#define SyncAmsInfoDialogHeightMAX FromDIP(660)
#define SyncLabelWidth FromDIP(640)
#define SyncAttentionTipWidth FromDIP(550)
namespace Slic3r { namespace GUI {
wxDEFINE_EVENT(EVT_CLEAR_IPADDRESS, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_USER_MACHINE_LIST, wxCommandEvent);
wxDEFINE_EVENT(EVT_PRINT_JOB_CANCEL, wxCommandEvent);
#define SYNC_FLEX_GRID_COL 7
bool SyncAmsInfoDialog::Show(bool show)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " SyncAmsInfoDialog begin show";
    if (show) {
        if (m_two_image_panel) {
            m_two_image_panel->SetBackgroundColor(wxGetApp().dark_mode() ? wxColour(48, 48, 48, 100) : wxColour(246, 246, 246, 100));
            m_left_image_button->SetBackgroundColour(wxGetApp().dark_mode() ? wxColour(61, 61, 61, 0) : wxColour(238, 238, 238, 0));
            m_right_image_button->SetBackgroundColour(wxGetApp().dark_mode() ? wxColour(61, 61, 61, 0) : wxColour(238, 238, 238, 0));
            init_bitmaps();
        }
        if (m_options_other) { m_options_other->Hide(); }
        if (m_refresh_timer) { m_refresh_timer->Start(LIST_REFRESH_INTERVAL); }
    } else {
        m_refresh_timer->Stop();
        return DPIDialog::Show(false);
    }
    // set default value when show this dialog
    wxGetApp().UpdateDlgDarkUI(this);
    set_default(true);
    reinit_dialog();
    update_user_machine_list();
    { // hide and hide
        m_basic_panel->Hide();
        hide_no_use_controls();
    }
    bool dirty_filament = is_dirty_filament();
    if (m_is_empty_project || dirty_filament) {
        show_color_panel(false);
        show_ams_controls(false);
        m_filament_left_panel->Show(false); // empty_project
        m_filament_right_panel->Show(false);
        m_are_you_sure_title->Show(false);
        if (m_mode_combox_sizer) {
            m_mode_combox_sizer->Show(false);
        }
    }
    if (dirty_filament) {
        m_two_thumbnail_panel->Hide();
         m_confirm_title->Show();
         m_confirm_title->SetLabel(_L("Synchronizing AMS filaments will discard your modified but unsaved filament presets.\nAre you sure you want to continue?"));
    } else if (!m_check_dirty_fialment) {
        show_color_panel(true);
        m_filament_left_panel->Show(false); // empty_project
        m_filament_right_panel->Show(false);
        m_are_you_sure_title->Show(true);
        if (m_mode_combox_sizer) {
            m_mode_combox_sizer->Show(true);
            m_reset_all_btn->Hide();
        }
        m_confirm_title->SetLabel(m_undone_str);
    }
    m_scrolledWindow->FitInside();
    m_scrolledWindow->Scroll(0, 0);
    Layout();
    Fit();
    CenterOnScreen();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " SyncAmsInfoDialog finish show";
    return DPIDialog::Show(show);
}

void SyncAmsInfoDialog::updata_ui_data_after_connected_printer() {
    if (is_dirty_filament()) { return; }

    m_two_thumbnail_panel->Show(true);

    show_ams_controls(true);
    //m_specify_color_cluster_title->Show();
    m_button_cancel->Show();
}

void SyncAmsInfoDialog::update_select_layout(MachineObject *obj)
{
    m_checkbox_list["use_ams"]->Hide();
}

void SyncAmsInfoDialog::set_default_normal(const ThumbnailData &data)
{
    if (m_cur_input_thumbnail_data.is_valid() && m_left_image_button) {
        auto &   temp_data = m_cur_input_thumbnail_data;
        wxImage image(temp_data.width, temp_data.height);
        image.InitAlpha();
        for (unsigned int r = 0; r < temp_data.height; ++r) {
            unsigned int rr = (temp_data.height - 1 - r) * temp_data.width;
            for (unsigned int c = 0; c < temp_data.width; ++c) {
                unsigned char *px = (unsigned char *) temp_data.pixels.data() + 4 * (rr + c);
                image.SetRGB((int) c, (int) r, px[0], px[1], px[2]);
                image.SetAlpha((int) c, (int) r, px[3]);
            }
        }
        //image.SaveFile("preview-left.png", wxBITMAP_TYPE_PNG);
        image = image.Rescale(FromDIP(LEFT_THUMBNAIL_SIZE_WIDTH), FromDIP(LEFT_THUMBNAIL_SIZE_WIDTH), wxIMAGE_QUALITY_BOX_AVERAGE);
        m_left_image_button->SetBitmap(image);
    }
    if (data.is_valid() && m_right_image_button) {
        wxImage image(data.width, data.height);
        image.InitAlpha();
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char *px = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                image.SetRGB((int) c, (int) r, px[0], px[1], px[2]);
                image.SetAlpha((int) c, (int) r, px[3]);
            }
        }
        //image.SaveFile("preview-right.png", wxBITMAP_TYPE_PNG);
        image = image.Rescale(FromDIP(RIGHT_THUMBNAIL_SIZE_WIDTH), FromDIP(RIGHT_THUMBNAIL_SIZE_WIDTH), wxIMAGE_QUALITY_BOX_AVERAGE);
        m_right_image_button->SetBitmap(image);
        auto extruders = wxGetApp().plater()->get_partplate_list().get_plate(m_specify_plate_idx)->get_extruders();
        if (wxGetApp().plater()->get_extruders_colors().size() == extruders.size()) {
            //m_used_colors_tip_text->Hide();
        }
        else {
            //m_used_colors_tip_text->Show();
            //m_used_colors_tip_text->SetLabel("  (" + std::to_string(extruders.size()) + " " + _L("colors used") + ")");
        }
    }
    if (m_map_mode == MapModeEnum::ColorMap) {
        m_back_cur_colors_in_thumbnail = m_cur_colors_in_thumbnail;
    }
}

bool SyncAmsInfoDialog::is_must_finish_slice_then_connected_printer() {
    if (m_specify_plate_idx >= 0) {
        return false;
    }
    return true;
}

void SyncAmsInfoDialog::hide_no_use_controls() {
    show_sizer(sizer_basic_right_info,false);
    show_sizer(m_basicl_sizer, false);
    //show_sizer(sizer_split_filament, false);
    show_sizer(m_sizer_options_timelapse, false);
    show_sizer(m_sizer_options_other, false);
    show_sizer(sizer_advanced_options_title, false);
    //m_statictext_ams_msg->Hide();
}

void SyncAmsInfoDialog::show_sizer(wxSizer *sizer, bool show)
{
    if (!sizer) { return; }
    wxSizerItemList items = sizer->GetChildren();
    for (wxSizerItemList::iterator it = items.begin(); it != items.end(); ++it) {
        wxSizerItem *item = *it;
        if (wxWindow *window = item->GetWindow()) { window->Show(show); }
        if (wxSizer *son_sizer = item->GetSizer()) { show_sizer(son_sizer, show); }
    }
}

void SyncAmsInfoDialog::deal_ok()
{
    if (!m_is_empty_project) {
        if (m_map_mode == MapModeEnum::Override || !m_result.is_same_printer) {
            m_is_empty_project = true;
            m_result.direct_sync = true;
            return;
        }
        m_result.direct_sync = false;
        m_result.sync_maps.clear();
        for (size_t i = 0; i < m_ams_mapping_result.size(); i++) {
            auto temp_idx = m_ams_mapping_result[i].id;
            if (temp_idx >= 0) {
                auto &temp   = m_result.sync_maps[temp_idx];
                temp.ams_id  = m_ams_mapping_result[i].ams_id;
                temp.slot_id = m_ams_mapping_result[i].slot_id;
            }
            else{
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "check error:  m_result.sync_maps:" << temp_idx;
            }
        }
    }
}

bool SyncAmsInfoDialog::get_is_double_extruder()
{
    const auto &full_config = wxGetApp().preset_bundle->full_config();
    size_t      nozzle_nums = full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    bool use_double_extruder = nozzle_nums > 1 ? true : false;
    return use_double_extruder;
}

bool SyncAmsInfoDialog::is_dirty_filament() {
    PresetCollection *m_presets = &wxGetApp().preset_bundle->filaments;
    if (m_check_dirty_fialment && m_presets && m_presets->get_edited_preset().is_dirty) {
        return true;
    }
    return false;
}

bool SyncAmsInfoDialog::is_need_show()
{
    //init begin
    m_result.direct_sync = true;
    m_generate_fix_sizer_ams_mapping = false;
    m_ams_combo_info.clear();
    // init end
    check_empty_project();
    if (m_is_empty_project && !is_dirty_filament()) {
        return false;
    }
    auto mode = PageType::ptColorMap;
    if (m_colormap_btn) {
        m_pages->ChangeSelection(0);
        update_panel_status(mode);
        update_when_change_map_mode(mode);
        update_plate_combox();
        update_swipe_button_state();
    }
    return true;
}

wxBoxSizer *SyncAmsInfoDialog::create_sizer_thumbnail(wxButton *image_button, bool left)
{
    auto sizer_thumbnail = new wxBoxSizer(wxVERTICAL);
    if (left) {
        wxBoxSizer *text_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto        sync_text  = new Label(image_button->GetParent(), _CTX(L_CONTEXT("Original", "Sync_AMS"), "Sync_AMS"));
        sync_text->SetForegroundColour(wxColour(107, 107, 107, 100));
        text_sizer->Add(sync_text, 0, wxALIGN_CENTER | wxALL, 0);
        sizer_thumbnail->Add(sync_text, FromDIP(0), wxALIGN_CENTER | wxALL, FromDIP(4));
    }
    else {
        wxBoxSizer *text_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_after_map_text       = new Label(image_button->GetParent(), _L("After mapping"));
        m_after_map_text->SetForegroundColour(wxColour(107, 107, 107, 100));
        text_sizer->Add(m_after_map_text, 0, wxALIGN_CENTER | wxALL, 0);
        sizer_thumbnail->Add(m_after_map_text, FromDIP(0), wxALIGN_CENTER | wxALL, FromDIP(4));
    }
    sizer_thumbnail->Add(image_button, 0, wxALIGN_CENTER, 0);
    return sizer_thumbnail;
}

void SyncAmsInfoDialog::update_when_change_plate(int idx) {
    if (idx < 0) {
        return;
    }
    m_specify_plate_idx = idx;

    update_thumbnail_data_accord_plate_index(false); // share current map

    update_swipe_button_state();
    Layout();
    Fit();
}

void SyncAmsInfoDialog::update_when_change_map_mode(int idx)
{
    m_map_mode = (MapModeEnum) idx;
    show_ams_controls(true);
    if (m_map_mode == MapModeEnum::ColorMap) {
        show_color_panel(true,false);
        m_confirm_title->SetLabel(m_undone_str);
        m_after_map_text->SetLabel(_L("After mapping"));
        m_tip_text->SetLabel(m_tip_attention_color_map);
        m_scrolledWindow->SetMinSize(wxSize(-1, SyncAmsInfoDialogHeightMAX));
        m_scrolledWindow->SetMaxSize(wxSize(-1, SyncAmsInfoDialogHeightMAX));

    } else if (m_map_mode == MapModeEnum::Override) {
        show_color_panel(false,false);
        m_confirm_title->Show();
        m_confirm_title->SetLabel(m_override_undone_str);
        m_are_you_sure_title->Show();
        m_after_map_text->SetLabel(_L("After overwriting"));
        m_tip_text->SetLabel(m_tip_attention_override);
        m_scrolledWindow->SetMinSize(wxSize(-1, SyncAmsInfoDialogHeightMIN));
        m_scrolledWindow->SetMaxSize(wxSize(-1, SyncAmsInfoDialogHeightMIN));
    }
    update_map_when_change_map_mode();
    m_scrolledWindow->FitInside();
    Layout();
    Fit();
}

void SyncAmsInfoDialog::update_plate_combox()
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

wxColour SyncAmsInfoDialog::decode_ams_color(const std::string &color_str) {
    auto temp_str = color_str;
    if (temp_str.front() == '#') {
        temp_str = temp_str.substr(1);
    }
    if (temp_str.size() == 6) {
        temp_str += "FF";
    }
    return DevAmsTray::decode_color(temp_str);
}

void SyncAmsInfoDialog::update_map_when_change_map_mode()
{
    if (m_map_mode == MapModeEnum::ColorMap) {
        m_cur_colors_in_thumbnail = m_back_cur_colors_in_thumbnail;
    } else if (m_map_mode == MapModeEnum::Override) {
        if (m_ams_combo_info.empty()) {
            wxGetApp().preset_bundle->get_ams_cobox_infos(m_ams_combo_info);
        }
        for (size_t i = 0; i < m_ams_combo_info.ams_filament_colors.size(); i++) {
            auto result = decode_ams_color(m_ams_combo_info.ams_filament_colors[i]);
            if (i < m_cur_colors_in_thumbnail.size()) {
                m_cur_colors_in_thumbnail[i] = result;
            } else {
                m_cur_colors_in_thumbnail.resize(i + 1);
                m_cur_colors_in_thumbnail[i] = result;
            }
        }
    }
    update_thumbnail_data_accord_plate_index(false);
}

void SyncAmsInfoDialog::update_when_change_map_mode(wxCommandEvent &e)
{
    int win_id = e.GetId();
    auto mode = PageType(win_id);
    update_panel_status(mode);
    update_when_change_map_mode(mode);
}

void SyncAmsInfoDialog::update_panel_status(PageType page)
{
    std::vector<CapsuleButton *> button_list = {m_colormap_btn, m_override_btn};
    for (auto p : button_list) {
        if (p && p->IsSelected()) {
            p->Select(false);
        }
    }
    for (size_t i = 0; i < button_list.size(); i++) {
        if (i == int(page)) {
            button_list[i]->Select(true);
            break;
        }
    }
}

void SyncAmsInfoDialog::show_color_panel(bool flag, bool update_layout)
{
    //show_sizer(m_plate_combox_sizer, flag);
    show_advanced_settings(flag, update_layout);
    m_confirm_title->Show(flag);
    m_are_you_sure_title->Show(flag);
    if (flag) {
        auto extruders = wxGetApp().plater()->get_partplate_list().get_plate(m_specify_plate_idx)->get_extruders();
        /*if (wxGetApp().plater()->get_extruders_colors().size() != extruders.size()) {
            m_used_colors_tip_text->Show();
        }*/
    } else {
        //m_used_colors_tip_text->Hide();
    }
    if (update_layout){
        Layout();
        Fit();
    }
}

void SyncAmsInfoDialog::update_more_setting(bool layout, bool from_more_seting_text)
{
    if (!m_expand_more_settings) {
        m_advanced_options_icon->SetBitmap(create_scaled_bitmap("advanced_option3", m_scrolledWindow, 18));
        if (from_more_seting_text) {
            m_scrolledWindow->SetMinSize(wxSize(-1, SyncAmsInfoDialogHeightMIDDLE));
            m_scrolledWindow->SetMaxSize(wxSize(-1, SyncAmsInfoDialogHeightMIDDLE));
        }
    } else {
        m_advanced_options_icon->SetBitmap(create_scaled_bitmap("advanced_option4", m_scrolledWindow, 18));
        if (from_more_seting_text) {
            m_scrolledWindow->SetMinSize(wxSize(-1, SyncAmsInfoDialogHeightMAX));
            m_scrolledWindow->SetMaxSize(wxSize(-1, SyncAmsInfoDialogHeightMAX));
        }
    }
    show_sizer(m_append_color_sizer, m_expand_more_settings);
    show_sizer(m_merge_color_sizer, m_expand_more_settings);
    if (layout) {
        m_scrolledWindow->FitInside();
        Layout();
        Fit();
    }
}

void SyncAmsInfoDialog::init_bitmaps()
{
    if (wxGetApp().dark_mode()) {
        m_swipe_left_bmp_normal   = ScalableBitmap(m_scrolledWindow, "previous_item", m_bmp_pix_cont);
        m_swipe_left_bmp_hover    = ScalableBitmap(m_scrolledWindow, "previous_item_disable", m_bmp_pix_cont);
        m_swipe_left_bmp_disable  = ScalableBitmap(m_scrolledWindow, "previous_item_dark_disable", m_bmp_pix_cont);
        m_swipe_right_bmp_normal  = ScalableBitmap(m_scrolledWindow, "next_item", m_bmp_pix_cont);
        m_swipe_right_bmp_hover   = ScalableBitmap(m_scrolledWindow, "next_item_disable", m_bmp_pix_cont);
        m_swipe_right_bmp_disable = ScalableBitmap(m_scrolledWindow, "next_item_dark_disable", m_bmp_pix_cont);
    } else {
        m_swipe_left_bmp_normal   = ScalableBitmap(m_scrolledWindow, "previous_item", m_bmp_pix_cont);
        m_swipe_left_bmp_hover    = ScalableBitmap(m_scrolledWindow, "previous_item_hover", m_bmp_pix_cont);
        m_swipe_left_bmp_disable  = ScalableBitmap(m_scrolledWindow, "previous_item_disable", m_bmp_pix_cont);
        m_swipe_right_bmp_normal  = ScalableBitmap(m_scrolledWindow, "next_item", m_bmp_pix_cont);
        m_swipe_right_bmp_hover   = ScalableBitmap(m_scrolledWindow, "next_item_hover", m_bmp_pix_cont);
        m_swipe_right_bmp_disable = ScalableBitmap(m_scrolledWindow, "next_item_disable", m_bmp_pix_cont);
    }
}

void SyncAmsInfoDialog::add_two_image_control()
{// thumbnail
    m_two_thumbnail_panel = new StaticBox(m_scrolledWindow);
    m_two_thumbnail_panel->SetBorderWidth(0);
    //m_two_thumbnail_panel->SetMinSize(wxSize(FromDIP(637), -1));
    //m_two_thumbnail_panel->SetMaxSize(wxSize(FromDIP(637), -1));
    m_two_thumbnail_panel_sizer = new wxBoxSizer(wxVERTICAL);

    auto view_two_thumbnail_sizer = new wxBoxSizer(wxHORIZONTAL);
    view_two_thumbnail_sizer->AddSpacer(FromDIP(40));
    auto swipe_left__sizer = new wxBoxSizer(wxVERTICAL);
    swipe_left__sizer->AddStretchSpacer();
    init_bitmaps();
    m_swipe_left_button = new ScalableButton(m_two_thumbnail_panel, wxID_ANY, "previous_item", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true,
                                             m_bmp_pix_cont);
    m_swipe_left_button->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
        if (!m_swipe_left_button_enable) { return; }
        m_swipe_left_button->SetBitmap(m_swipe_left_bmp_hover.bmp());
    });
    m_swipe_left_button->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
        if (!m_swipe_left_button_enable) { return; }
        m_swipe_left_button->SetBitmap(m_swipe_left_bmp_normal.bmp());
    });
    m_swipe_left_button->Bind(wxEVT_BUTTON, &SyncAmsInfoDialog::to_previous_plate, this);
    swipe_left__sizer->Add(m_swipe_left_button, 0, wxALIGN_CENTER | wxEXPAND | wxALIGN_CENTER_VERTICAL);
    swipe_left__sizer->AddStretchSpacer();
    view_two_thumbnail_sizer->Add(swipe_left__sizer, 0, wxEXPAND);
    view_two_thumbnail_sizer->AddSpacer(FromDIP(24));
    {
        m_two_image_panel = new StaticBox(m_two_thumbnail_panel);
        m_two_image_panel->SetBorderWidth(0);
        //m_two_image_panel->SetForegroundColour(wxColour(248, 248, 248, 100));
        m_two_image_panel_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_left_image_button     = new wxButton(m_two_image_panel, wxID_ANY, {}, wxDefaultPosition, wxSize(FromDIP(LEFT_THUMBNAIL_SIZE_WIDTH), FromDIP(LEFT_THUMBNAIL_SIZE_WIDTH)),
                                           wxBORDER_NONE | wxBU_AUTODRAW);
        m_left_sizer_thumbnail = create_sizer_thumbnail(m_left_image_button, true);
        m_two_image_panel_sizer->Add(m_left_sizer_thumbnail, FromDIP(0), wxALIGN_LEFT | wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
        m_two_image_panel_sizer->AddSpacer(FromDIP(5));

        m_right_image_button = new wxButton(m_two_image_panel, wxID_ANY, {}, wxDefaultPosition,
                                            wxSize(FromDIP(RIGHT_THUMBNAIL_SIZE_WIDTH), FromDIP(RIGHT_THUMBNAIL_SIZE_WIDTH)),
                                            wxBORDER_NONE | wxBU_AUTODRAW);
        m_right_sizer_thumbnail = create_sizer_thumbnail(m_right_image_button, false);
        m_two_image_panel_sizer->Add(m_right_sizer_thumbnail, FromDIP(0), wxALIGN_LEFT | wxEXPAND | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(8));
        m_two_image_panel->SetSizer(m_two_image_panel_sizer);
        m_two_image_panel->Layout();
        m_two_image_panel->Fit();

        view_two_thumbnail_sizer->Add(m_two_image_panel, FromDIP(0), wxALIGN_LEFT | wxEXPAND | wxTOP, FromDIP(2));
    }
    view_two_thumbnail_sizer->AddSpacer(FromDIP(20));
    auto swipe_right__sizer = new wxBoxSizer(wxVERTICAL);
    swipe_right__sizer->AddStretchSpacer();
    m_swipe_right_button    = new ScalableButton(m_two_thumbnail_panel, wxID_ANY, "next_item", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true,
                                              m_bmp_pix_cont);
    m_swipe_right_button->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
        if (!m_swipe_right_button_enable) { return; }
        m_swipe_right_button->SetBitmap(m_swipe_right_bmp_hover.bmp());
    });
    m_swipe_right_button->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
        if (!m_swipe_right_button_enable) { return; }
        m_swipe_right_button->SetBitmap(m_swipe_right_bmp_normal.bmp());
    });
    m_swipe_right_button->Bind(wxEVT_BUTTON, &SyncAmsInfoDialog::to_next_plate, this);

    swipe_right__sizer->Add(m_swipe_right_button, 0, wxALIGN_CENTER | wxEXPAND | wxALIGN_CENTER_VERTICAL);
    swipe_right__sizer->AddStretchSpacer();
    view_two_thumbnail_sizer->Add(swipe_right__sizer, 0, wxEXPAND);
    view_two_thumbnail_sizer->AddStretchSpacer();
    m_two_thumbnail_panel_sizer->Add(view_two_thumbnail_sizer, 0, wxEXPAND | wxTOP, FromDIP(5));

    m_choose_plate_sizer         = new wxBoxSizer(wxHORIZONTAL);
    m_choose_plate_sizer->AddStretchSpacer();

    wxStaticText *chose_combox_title = new wxStaticText(m_two_thumbnail_panel, wxID_ANY, _CTX(L_CONTEXT("Plate", "Sync_AMS"), "Sync_AMS"));
    m_choose_plate_sizer->Add(chose_combox_title, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxEXPAND | wxTOP, FromDIP(6));
    m_choose_plate_sizer->AddSpacer(FromDIP(10));

    m_combobox_plate = new ComboBox(m_two_thumbnail_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(60), -1), 0, NULL, wxCB_READONLY);

    m_combobox_plate->Bind(wxEVT_COMBOBOX, [this](auto &e) {
        if (e.GetSelection() < m_plate_choices.size()) {
            update_when_change_plate(m_plate_choices[e.GetSelection()]);
        }
    });
    m_choose_plate_sizer->Add(m_combobox_plate, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(3));
    m_choose_plate_sizer->AddStretchSpacer();
    m_two_thumbnail_panel_sizer->Add(m_choose_plate_sizer, 0, wxEXPAND |wxTOP, FromDIP(4));

    m_two_thumbnail_panel->SetSizer(m_two_thumbnail_panel_sizer);
    m_two_thumbnail_panel->Layout();
    m_two_thumbnail_panel->Fit();
    m_sizer_main->Add(m_two_thumbnail_panel, FromDIP(0), wxALIGN_CENTER | wxEXPAND | wxLEFT | wxRIGHT, FromDIP(25));

    update_swipe_button_state();
}

void SyncAmsInfoDialog::to_next_plate(wxCommandEvent &event) {
    auto cobox_idx  = m_combobox_plate->GetSelection();
    cobox_idx++;
    if (cobox_idx >= m_combobox_plate->GetCount()) {
        return;
    }
    m_combobox_plate->SetSelection(cobox_idx);
    update_when_change_plate(m_plate_choices[cobox_idx]);
}

void SyncAmsInfoDialog::to_previous_plate(wxCommandEvent &event) {
    auto cobox_idx = m_combobox_plate->GetSelection();
    cobox_idx--;
    if (cobox_idx < 0) {
        return;
    }
    m_combobox_plate->SetSelection(cobox_idx);
    update_when_change_plate(m_plate_choices[cobox_idx]);
}

void SyncAmsInfoDialog::update_swipe_button_state()
{
    m_swipe_left_button_enable = true;
    m_swipe_left_button->Enable();
    m_swipe_left_button->SetBitmap(m_swipe_left_bmp_normal.bmp());
    m_swipe_right_button_enable = true;
    m_swipe_right_button->Enable();
    m_swipe_right_button->SetBitmap(m_swipe_right_bmp_normal.bmp());
    if (m_combobox_plate->GetSelection() == 0) { // auto plate_index = m_plate_choices[m_combobox_plate->GetSelection()];
        m_swipe_left_button->SetBitmap(m_swipe_left_bmp_disable.bmp());
        m_swipe_left_button_enable = false;
    }
    if (m_combobox_plate->GetSelection() == m_combobox_plate->GetCount() - 1) {
        m_swipe_right_button->SetBitmap(m_swipe_right_bmp_disable.bmp());
        m_swipe_right_button_enable = false;
    }
}

void SyncAmsInfoDialog::updata_ui_when_priner_not_same() {
    show_color_panel(false);
    m_are_you_sure_title->Show(false);
    if (m_mode_combox_sizer)
        m_mode_combox_sizer->Show(false);

    m_button_cancel->Hide();
    m_button_ok->Show();
    m_button_ok->SetLabel(_L("OK"));
    m_confirm_title->Show();
    m_confirm_title->SetLabel(_L("The connected printer does not match the currently selected printer. Please change the selected printer."));

    Layout();
    Fit();
}

SyncAmsInfoDialog::SyncAmsInfoDialog(wxWindow *parent, SyncInfo &info) :
    DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Synchronize AMS Filament Information"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_input_info(info)
    , m_export_3mf_cancel(false)
    , m_mapping_popup(AmsMapingPopup(this,true))
    , m_mapping_tip_popup(AmsMapingTipPopup(this))
    , m_mapping_tutorial_popup(AmsTutorialPopup(this))
{
    m_plater = wxGetApp().plater();
    SelectMachineDialog::init_machine_bed_types();
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    ops_auto.push_back(POItem{"auto", "Auto"});
    ops_auto.push_back(POItem{"on", "On"});
    ops_auto.push_back(POItem{"off", "Off"});

    ops_no_auto.push_back(POItem{"on", "On"});
    ops_no_auto.push_back(POItem{"off", "Off"});

    SetMinSize(wxSize(SyncAmsInfoDialogWidth, -1));
    SetMaxSize(wxSize(SyncAmsInfoDialogWidth, -1));

    // bind
    Bind(wxEVT_CLOSE_WINDOW, &SyncAmsInfoDialog::on_cancel, this);

    for (int i = 0; i < BED_TYPE_COUNT; i++) { m_bedtype_list.push_back(SelectMachineDialog::MACHINE_BED_TYPE_STRING[i]); }

    // font
    SetFont(wxGetApp().normal_font());

    // icon
    std::string icon_path = (boost::format("%1%/images/OrcaSlicerTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    Freeze();
    SetBackgroundColour(m_colour_def_color);

    m_pages = new wxSimplebook(this);
    m_pages->SetBackgroundColour(*wxWHITE);
    m_pages->SetSize(wxSize(SyncAmsInfoDialogWidth, -1));

    m_loading_page = new wxPanel(m_pages);
    m_show_page    = new wxPanel(m_pages);
    m_loading_page->SetBackgroundColour(*wxWHITE);
    m_show_page->SetBackgroundColour(*wxWHITE);
    m_pages->AddPage(m_loading_page, wxEmptyString, true);
    m_pages->AddPage(m_show_page, wxEmptyString, false);
    {//generate m_loading_page
        wxBoxSizer *page1Sizer = new wxBoxSizer(wxVERTICAL);
        page1Sizer->AddStretchSpacer();

        wxBoxSizer *loading_Sizer = new wxBoxSizer(wxHORIZONTAL);
        m_gif_ctrl = new wxAnimationCtrl(m_loading_page, wxID_ANY, wxNullAnimation, wxDefaultPosition, wxDefaultSize, wxAC_DEFAULT_STYLE);
        auto gif_path = Slic3r::var("loading.gif").c_str();
        if (m_gif_ctrl->LoadFile(gif_path)){
            m_gif_ctrl->SetSize(m_gif_ctrl->GetAnimation().GetSize());
            m_gif_ctrl->Play();

            loading_Sizer->Add(m_gif_ctrl, 0, wxALIGN_CENTER | wxALL, 0);
            loading_Sizer->AddSpacer(10);
        }
        auto loading_label = new Label(m_loading_page, _L("Loading"));
        loading_label->SetFont(::Label::Head_14);
        loading_Sizer->Add(loading_label, 0, wxALIGN_CENTER | wxALL, 0);

        page1Sizer->Add(loading_Sizer, 0, wxALIGN_CENTER | wxALL, 0);
        page1Sizer->AddStretchSpacer();
        m_loading_page->SetSizerAndFit(page1Sizer);
    }
    m_sizer_show_page = new wxBoxSizer(wxVERTICAL);
    m_sizer_this = new wxBoxSizer(wxVERTICAL);

    //wxBoxSizer *m_scroll_sizer = new wxBoxSizer(wxVERTICAL);
    m_scrolledWindow = new wxScrolledWindow(m_show_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scrolledWindow->SetBackgroundColour(*wxWHITE);
    m_scrolledWindow->SetScrollRate(0, 20);
    m_scrolledWindow->SetMinSize(wxSize(-1, SyncAmsInfoDialogHeightMAX));
    m_scrolledWindow->SetMaxSize(wxSize(-1, SyncAmsInfoDialogHeightMAX));
    m_scrolledWindow->EnableScrolling(false,true);
    m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_line_top = new wxPanel(m_scrolledWindow, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);
    //m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(11));
    auto &bSizer = m_sizer_main;
        { // content
        check_empty_project();
        //use map mode
        m_mode_combox_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_colormap_btn      = new CapsuleButton(m_scrolledWindow, PageType::ptColorMap, _L("Mapping"), true);
        m_override_btn      = new CapsuleButton(m_scrolledWindow, PageType::ptOverride, _L("Overwriting"), false);
        m_mode_combox_sizer->AddSpacer(SyncAmsInfoDialogWidth / 2.0f - FromDIP(8) / 2.0f - m_colormap_btn->GetSize().GetX());
        m_mode_combox_sizer->Add(m_colormap_btn, 0, wxALIGN_CENTER | wxEXPAND | wxALL, FromDIP(2));
        m_mode_combox_sizer->AddSpacer(FromDIP(8));
        m_mode_combox_sizer->Add(m_override_btn, 0, wxALIGN_CENTER | wxEXPAND | wxALL, FromDIP(2));
        m_mode_combox_sizer->AddSpacer(SyncAmsInfoDialogWidth / 2.0f - FromDIP(8) / 2.0f - m_override_btn->GetSize().GetX() - FromDIP(60));
        m_reset_all_btn = new ScalableButton(m_scrolledWindow, wxID_ANY, "reset_gray", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER,
                                                        true, 14);
        m_reset_all_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { reset_all_ams_info(); });
        m_reset_all_btn->SetBackgroundColour(*wxWHITE);
        m_reset_all_btn->SetToolTip(_L("Reset all filament mapping"));

        m_mode_combox_sizer->Add(m_reset_all_btn, 0, wxALIGN_LEFT | wxEXPAND | wxALL, FromDIP(2));

        m_colormap_btn->Bind(wxEVT_BUTTON, &SyncAmsInfoDialog::update_when_change_map_mode,this); // update_when_change_map_mode(e.GetSelection());
        m_override_btn->Bind(wxEVT_BUTTON, &SyncAmsInfoDialog::update_when_change_map_mode,this);

        bSizer->Add(m_mode_combox_sizer, FromDIP(0), wxEXPAND | wxALIGN_LEFT | wxTOP, FromDIP(10));
    }

    m_basic_panel = new wxPanel(m_scrolledWindow, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_basic_panel->SetBackgroundColour(*wxWHITE);
    m_basicl_sizer = new wxBoxSizer(wxHORIZONTAL);

    /*basic info right*/
    sizer_basic_right_info = new wxBoxSizer(wxVERTICAL);

    m_text_printer_msg = new Label(m_basic_panel);
    m_text_printer_msg->SetMinSize(wxSize(FromDIP(420), FromDIP(24)));
    m_text_printer_msg->SetMaxSize(wxSize(FromDIP(420), FromDIP(24)));
    m_text_printer_msg->SetFont(::Label::Body_13);
    m_text_printer_msg->Hide();

    sizer_basic_right_info->Add(m_text_printer_msg, 0, wxLEFT, 0);

    m_basicl_sizer->Add(sizer_basic_right_info, 0, wxLEFT, 0);

    m_basic_panel->SetSizer(m_basicl_sizer);
    m_basic_panel->Layout();

    /*filament area*/
    //overrider fix filament
    m_fix_filament_panel = new StaticBox(m_scrolledWindow);
    m_fix_filament_panel->SetBorderWidth(0);
    m_fix_filament_panel->SetMinSize(wxSize(FromDIP(637), -1));
    m_fix_filament_panel->SetMaxSize(wxSize(FromDIP(637), -1));
    m_fix_filament_panel_sizer = new wxBoxSizer(wxVERTICAL);

    m_fix_sizer_ams_mapping = new wxFlexGridSizer(0, SYNC_FLEX_GRID_COL, FromDIP(6), FromDIP(7));
    m_fix_filament_panel_sizer->Add(m_fix_sizer_ams_mapping, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    m_fix_filament_panel->SetSizer(m_fix_filament_panel_sizer);
    m_fix_filament_panel->Layout();
    m_fix_filament_panel->Fit();
    /*1 extruder*/
    m_filament_panel = new StaticBox(m_scrolledWindow);
    m_filament_panel->SetBorderWidth(0);
    m_filament_panel->SetMinSize(wxSize(FromDIP(637), -1));
    m_filament_panel->SetMaxSize(wxSize(FromDIP(637), -1));
    m_filament_panel_sizer = new wxBoxSizer(wxVERTICAL);

    m_sizer_ams_mapping = new wxFlexGridSizer(0, SYNC_FLEX_GRID_COL, FromDIP(6), FromDIP(7));
    m_filament_panel_sizer->Add(m_sizer_ams_mapping, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    m_filament_panel->SetSizer(m_filament_panel_sizer);
    m_filament_panel->Layout();
    m_filament_panel->Fit();

    /*left & right extruder*/
    m_sizer_filament_2extruder = new wxBoxSizer(wxHORIZONTAL);

    m_filament_left_panel = new StaticBox(m_scrolledWindow);
    m_filament_left_panel->SetBackgroundColour(wxColour("#F8F8F8"));
    m_filament_left_panel->SetBorderWidth(0);
    m_filament_left_panel->SetMinSize(wxSize(FromDIP(315), -1));
    m_filament_left_panel->SetMaxSize(wxSize(FromDIP(315), -1));

    m_filament_panel_left_sizer     = new wxBoxSizer(wxVERTICAL);
    auto left_recommend_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto left_recommend_title1      = new Label(m_filament_left_panel, _L("Left Extruder"));
    left_recommend_title1->SetFont(::Label::Head_13);
    left_recommend_title1->SetBackgroundColour(wxColour("#F8F8F8"));
    auto left_recommend_title2 = new Label(m_filament_left_panel, _L("(Recommended filament)"));
    left_recommend_title2->SetFont(::Label::Body_13);
    left_recommend_title2->SetForegroundColour(wxColour("#6B6B6B"));
    left_recommend_title2->SetBackgroundColour(wxColour("#F8F8F8"));
    left_recommend_title_sizer->Add(left_recommend_title1, 0, wxALIGN_CENTER, 0);
    left_recommend_title_sizer->Add(0, 0, 0, wxLEFT, FromDIP(4));
    left_recommend_title_sizer->Add(left_recommend_title2, 0, wxALIGN_CENTER, 0);

    m_sizer_ams_mapping_left = new wxGridSizer(0, 5, FromDIP(7), FromDIP(7));
    m_filament_panel_left_sizer->Add(left_recommend_title_sizer, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_filament_panel_left_sizer->Add(m_sizer_ams_mapping_left, 0, wxEXPAND | wxALL, FromDIP(10));
    m_filament_left_panel->SetSizer(m_filament_panel_left_sizer);
    m_filament_left_panel->Layout();

    m_filament_right_panel = new StaticBox(m_scrolledWindow);
    m_filament_right_panel->SetBorderWidth(0);
    m_filament_right_panel->SetBackgroundColour(wxColour("#F8F8F8"));
    m_filament_right_panel->SetMinSize(wxSize(FromDIP(315), -1));
    m_filament_right_panel->SetMaxSize(wxSize(FromDIP(315), -1));

    m_filament_panel_right_sizer     = new wxBoxSizer(wxVERTICAL);
    auto right_recommend_title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto right_recommend_title1      = new Label(m_filament_right_panel, _L("Right Extruder"));
    right_recommend_title1->SetFont(::Label::Head_13);
    right_recommend_title1->SetBackgroundColour(wxColour("#F8F8F8"));

    auto right_recommend_title2 = new Label(m_filament_right_panel, _L("(Recommended filament)"));
    right_recommend_title2->SetFont(::Label::Body_13);
    right_recommend_title2->SetForegroundColour(wxColour("#6B6B6B"));
    right_recommend_title2->SetBackgroundColour(wxColour("#F8F8F8"));
    right_recommend_title_sizer->Add(right_recommend_title1, 0, wxALIGN_CENTER, 0);
    right_recommend_title_sizer->Add(0, 0, 0, wxLEFT, FromDIP(4));
    right_recommend_title_sizer->Add(right_recommend_title2, 0, wxALIGN_CENTER, 0);

    m_sizer_ams_mapping_right = new wxGridSizer(0, 5, FromDIP(7), FromDIP(7));
    m_filament_panel_right_sizer->Add(right_recommend_title_sizer, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_filament_panel_right_sizer->Add(m_sizer_ams_mapping_right, 0, wxEXPAND | wxALL, FromDIP(10));
    m_filament_right_panel->SetSizer(m_filament_panel_right_sizer);
    m_filament_right_panel->Layout();

    m_sizer_filament_2extruder->Add(m_filament_left_panel, 0, wxEXPAND, 0);
    m_sizer_filament_2extruder->Add(0, 0, 1, wxEXPAND, 0);
    m_sizer_filament_2extruder->Add(m_filament_right_panel, 0, wxEXPAND, 0);
    m_sizer_filament_2extruder->Layout();

    m_filament_left_panel->Hide();//init
    m_filament_right_panel->Hide();
    m_filament_panel->Hide();//init

    sizer_advanced_options_title = new wxBoxSizer(wxHORIZONTAL);
    auto advanced_options_title  = new Label(m_scrolledWindow, _L("Advanced Options"));
    advanced_options_title->SetFont(::Label::Body_13);
    advanced_options_title->SetForegroundColour(wxColour(38, 46, 48));

    sizer_advanced_options_title->Add(0, 0, 1, wxEXPAND, 0);
    sizer_advanced_options_title->Add(advanced_options_title, 0, wxALIGN_CENTER, 0);

    advanced_options_title->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) { SetCursor(wxCURSOR_HAND); });
    advanced_options_title->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) { SetCursor(wxCURSOR_ARROW); });
    advanced_options_title->Bind(wxEVT_LEFT_DOWN, [this](auto &e) {
        if (m_options_other->IsShown()) {
            m_options_other->Hide();
        } else {
            m_options_other->Show();
        }
        Layout();
        Fit();
    });

    m_options_other           = new wxPanel(m_scrolledWindow);
    m_sizer_options_timelapse = new wxBoxSizer(wxVERTICAL);
    m_sizer_options_other     = new wxBoxSizer(wxVERTICAL);

    auto option_timelapse = new PrintOption(m_scrolledWindow, _L("Timelapse"), wxEmptyString, ops_no_auto, "timelapse");

    auto option_auto_bed_level =
        new PrintOption(m_options_other, _L("Auto Bed Leveling"),
                        _L("Check heatbed flatness. Leveling makes extruded height uniform.\n*Automatic mode: Level first (about 10 seconds). Skip if surface is fine."),
                        ops_auto, "bed_leveling");


    auto option_nozzle_offset_cali_cali =
        new PrintOption(m_options_other, _L("Nozzle Offset Calibration"),
                        _L("Calibrate nozzle offsets to enhance print quality.\n*Automatic mode: Check for calibration before printing; skip if unnecessary."), ops_auto);

    auto option_use_ams = new PrintOption(m_options_other, _L("Use AMS"), wxEmptyString, ops_no_auto);

    option_use_ams->setValue("on");
    m_sizer_options_timelapse->Add(option_timelapse, 0, wxEXPAND  | wxBOTTOM, FromDIP(5));
    m_sizer_options_other->Add(option_use_ams, 0, wxEXPAND  | wxBOTTOM, FromDIP(5));
    m_sizer_options_other->Add(option_auto_bed_level, 0, wxEXPAND  | wxBOTTOM, FromDIP(5));
    m_sizer_options_other->Add(option_nozzle_offset_cali_cali, 0, wxEXPAND  | wxBOTTOM, FromDIP(5));

    m_options_other->SetSizer(m_sizer_options_other);
    m_options_other->SetMaxSize(wxSize(-1, FromDIP(0)));
    m_checkbox_list["timelapse"]          = option_timelapse;
    m_checkbox_list["bed_leveling"]       = option_auto_bed_level;
    m_checkbox_list["use_ams"]            = option_use_ams;
    m_checkbox_list["nozzle_offset_cali"] = option_nozzle_offset_cali_cali;

    option_timelapse->Hide();
    option_auto_bed_level->Hide();
    option_nozzle_offset_cali_cali->Hide();
    option_use_ams->Hide();

    // m_sizer_main->Add(m_sizer_mode_switch, 0, wxALIGN_CENTER, 0);
   // m_sizer_main->Add(m_basic_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));
    //m_sizer_main->Add(sizer_split_filament, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));
    m_sizer_main->Add(m_filament_panel, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(15));
    m_sizer_main->Add(m_fix_filament_panel, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(15));
    m_fix_filament_panel->Show(false);
    m_sizer_main->Add(m_sizer_filament_2extruder, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));



    {//new content//tip confirm ok button
        wxBoxSizer *tip_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_attention_text      = new wxStaticText(m_scrolledWindow, wxID_ANY, _L("Tip") + ": ");
        tip_sizer->Add(m_attention_text, 0, wxALIGN_LEFT | wxTOP, FromDIP(2));
        m_tip_attention_color_map = _L("Only synchronize filament type and color, not including AMS slot information.");
        m_tip_attention_override  = _L("Replace the project filaments list sequentially based on printer filaments. And unused printer filaments will be automatically added to the end of the list.");
        m_tip_text = new Label(m_scrolledWindow, m_tip_attention_color_map, LB_AUTO_WRAP);
        m_tip_text->SetMinSize(wxSize(SyncAttentionTipWidth, -1));
        m_tip_text->SetMaxSize(wxSize(SyncAttentionTipWidth, -1));
        m_tip_text->SetForegroundColour(wxColour(107, 107, 107, 100));
        tip_sizer->Add(m_tip_text, 0, wxALIGN_LEFT | wxTOP, FromDIP(2));
        tip_sizer->AddSpacer(FromDIP(20));
        bSizer->Add(tip_sizer, 0, wxEXPAND | wxLEFT, FromDIP(25));

        add_two_image_control();

        wxBoxSizer * more_setting_sizer = new wxBoxSizer(wxVERTICAL);

        m_advace_setting_sizer         = new wxBoxSizer(wxHORIZONTAL);
        m_more_setting_tips    = new wxStaticText(m_scrolledWindow, wxID_ANY, _L("Advanced settings"));
        m_more_setting_tips->SetForegroundColour(wxColour(0, 137, 123));
        m_more_setting_tips->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
            m_expand_more_settings = !m_expand_more_settings;
            update_more_setting(true,true);
        });

        m_advace_setting_sizer->Add(m_more_setting_tips, 0, wxALIGN_LEFT | wxTOP, FromDIP(4));
        m_advanced_options_icon = new wxStaticBitmap(m_scrolledWindow, wxID_ANY, create_scaled_bitmap("advanced_option3", m_scrolledWindow, 18), wxDefaultPosition,
                                                     wxSize(FromDIP(18), FromDIP(18)));
        m_advace_setting_sizer->Add(m_advanced_options_icon, 0, wxALIGN_LEFT | wxTOP, FromDIP(4));
        more_setting_sizer->Add(m_advace_setting_sizer, 0, wxALIGN_LEFT, FromDIP(0));

        m_append_color_sizer    = new wxBoxSizer(wxHORIZONTAL);
        m_append_color_sizer->AddSpacer(FromDIP(10));

        m_append_color_checkbox = new ::CheckBox(m_scrolledWindow, wxID_ANY);
        //m_append_color_checkbox->SetForegroundColour(wxColour(107, 107, 107, 100));
        m_append_color_checkbox->SetValue(wxGetApp().app_config->get_bool("enable_append_color_by_sync_ams"));
        m_append_color_checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
            auto flag = wxGetApp().app_config->get_bool("enable_append_color_by_sync_ams");
            wxGetApp().app_config->set_bool("enable_append_color_by_sync_ams",!flag);
            m_append_color_checkbox->SetValue(!flag);
            e.Skip();
        });
        m_append_color_checkbox->Hide();
        m_append_color_sizer->Add(m_append_color_checkbox, 0, wxALIGN_LEFT | wxTOP, FromDIP(4));
        const int gap_between_checebox_and_text = 2;
        m_append_color_text                     = new Label(m_scrolledWindow, _L("Add unused AMS filaments to filaments list."));
        m_append_color_text->Hide();
        m_append_color_sizer->AddSpacer(FromDIP(gap_between_checebox_and_text));
        m_append_color_sizer->Add(m_append_color_text, 0, wxALIGN_LEFT | wxTOP, FromDIP(4));

        more_setting_sizer->Add(m_append_color_sizer, 0, wxALIGN_LEFT | wxTOP, FromDIP(4));

        m_merge_color_sizer    = new wxBoxSizer(wxHORIZONTAL);
        m_merge_color_sizer->AddSpacer(FromDIP(10));
        m_merge_color_checkbox = new ::CheckBox(m_scrolledWindow, wxID_ANY);
        //m_merge_color_checkbox->SetForegroundColour(wxColour(107, 107, 107, 100));
        m_merge_color_checkbox->SetValue(wxGetApp().app_config->get_bool("enable_merge_color_by_sync_ams"));
        m_merge_color_checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
            auto flag = wxGetApp().app_config->get_bool("enable_merge_color_by_sync_ams");
            wxGetApp().app_config->set_bool("enable_merge_color_by_sync_ams",!flag);
            m_merge_color_checkbox->SetValue(!flag);
            e.Skip();
        });
        m_merge_color_checkbox->Hide();
        m_merge_color_sizer->Add(m_merge_color_checkbox, 0, wxALIGN_LEFT | wxTOP, FromDIP(2));


        m_merge_color_text = new Label(m_scrolledWindow, _L("Automatically merge the same colors in the model after mapping."));
        m_merge_color_text->Hide();
        m_merge_color_sizer->AddSpacer(FromDIP(gap_between_checebox_and_text));
        m_merge_color_sizer->Add(m_merge_color_text, 0, wxALIGN_LEFT | wxTOP, FromDIP(2));

        more_setting_sizer->Add(m_merge_color_sizer, 0, wxALIGN_LEFT | wxTOP, FromDIP(2));

        bSizer->Add(more_setting_sizer, 0, wxEXPAND | wxLEFT, FromDIP(25));

        wxBoxSizer *confirm_boxsizer = new wxBoxSizer(wxVERTICAL);

        m_override_undone_str = _L("After being synced, this action cannot be undone.");
        m_undone_str = _L("After being synced, the project's filament presets and colors will be replaced with the mapped filament types and colors. This action cannot be undone.");
        m_confirm_title = new Label(m_scrolledWindow, m_undone_str, LB_AUTO_WRAP);
        m_confirm_title->SetMinSize(wxSize(SyncLabelWidth, -1));
        m_confirm_title->SetMaxSize(wxSize(SyncLabelWidth, -1));
        confirm_boxsizer->Add(m_confirm_title, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT, FromDIP(10));
        m_are_you_sure_title = new wxStaticText(m_scrolledWindow, wxID_ANY, _L("Are you sure to synchronize the filaments?"));
        //m_are_you_sure_title->SetFont(Label::Head_14);
        confirm_boxsizer->Add(m_are_you_sure_title, 0, wxALIGN_LEFT  | wxTOP, FromDIP(0));
        bSizer->Add(confirm_boxsizer, 0, wxALIGN_LEFT | wxLEFT , FromDIP(25));

        wxBoxSizer *warning_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_warning_text            = new wxStaticText(m_scrolledWindow, wxID_ANY, _L("Error") + ":");
        m_warning_text->SetForegroundColour(wxColour(107, 107, 107, 100));
        m_warning_text->Hide();
        warning_sizer->Add(m_warning_text, 0, wxALIGN_CENTER | wxTOP, FromDIP(2));
        bSizer->Add(warning_sizer, 0, wxEXPAND | wxLEFT, FromDIP(25));

        m_scrolledWindow->SetSizerAndFit(m_sizer_main);
        m_sizer_show_page->Add(m_scrolledWindow, 1, wxEXPAND, 0);

        wxBoxSizer *bSizer_button = new wxBoxSizer(wxHORIZONTAL);
        bSizer_button->SetMinSize(wxSize(FromDIP(100), -1));
        /* m_checkbox = new wxCheckBox(this, wxID_ANY, _L("Don't show again"), wxDefaultPosition, wxDefaultSize, 0);
         bSizer_button->Add(m_checkbox, 0, wxALIGN_LEFT);*/
        bSizer_button->AddStretchSpacer(1);
        StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed), std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                                std::pair<wxColour, int>(AMS_CONTROL_BRAND_COLOUR, StateColor::Normal));
        m_button_ok = new Button(m_show_page,  _L("Synchronize now"));
        m_button_ok->SetBackgroundColor(btn_bg_green);
        m_button_ok->SetBorderColor(*wxWHITE);
        m_button_ok->SetTextColor(wxColour("#FFFFFE"));
        m_button_ok->SetFont(Label::Body_12);
        m_button_ok->SetSize(OK_BUTTON_SIZE);
        m_button_ok->SetMinSize(OK_BUTTON_SIZE);
        m_button_ok->SetCornerRadius(FromDIP(12));
        bSizer_button->Add(m_button_ok, 0, wxALIGN_RIGHT | wxLEFT | wxTOP, FromDIP(10));

        m_button_ok->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent &e) {
            deal_ok();
            EndModal(wxID_YES);
            SetFocusIgnoringChildren();
        });

        StateColor btn_bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed), std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                                std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));

        m_button_cancel = new Button(m_show_page, m_input_info.cancel_text_to_later ? _L("Later") : _L("Cancel"));
        m_button_cancel->SetBackgroundColor(btn_bg_white);
        m_button_cancel->SetBorderColor(wxColour(38, 46, 48));
        m_button_cancel->SetFont(Label::Body_12);
        m_button_cancel->SetSize(CANCEL_BUTTON_SIZE);
        m_button_cancel->SetMinSize(CANCEL_BUTTON_SIZE);
        m_button_cancel->SetCornerRadius(FromDIP(12));
        bSizer_button->Add(m_button_cancel, 0, wxALIGN_RIGHT | wxLEFT | wxTOP, FromDIP(10));

        m_button_cancel->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent &e) {
            EndModal(wxID_CANCEL);
        });

        m_sizer_show_page->Add(bSizer_button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        m_show_page->SetSizer(m_sizer_show_page);
    }
    show_print_failed_info(false);
    m_sizer_this->Add(m_pages, 0, wxEXPAND, FromDIP(0));
    SetSizer(m_sizer_this);
    Layout();
    Fit();
    Thaw();

    init_bind();
    init_timer();
    //Centre(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);
}

void SyncAmsInfoDialog::check_empty_project()
{
    GUI::PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
    GUI::PartPlate *    curr_plate = GUI::wxGetApp().plater()->get_partplate_list().get_selected_plate();
    m_is_empty_project             = true;
    m_plate_number_choices_str.clear();
    m_plate_choices.clear();

    for (size_t i = 0; i < plate_list.get_plate_count(); i++) {
        auto temp_plate = GUI::wxGetApp().plater()->get_partplate_list().get_plate(i);
        if (!temp_plate->get_objects_on_this_plate().empty()) {
            if (m_is_empty_project) { m_is_empty_project = false; }
            if (i < 9) {
                m_plate_number_choices_str.Add("0" + std::to_wstring(i + 1));
            }
            else if (i == 9) {
                m_plate_number_choices_str.Add("10");
            }
            else {
                m_plate_number_choices_str.Add(std::to_wstring(i + 1));
            }
            m_plate_choices.emplace_back(i);
        }
    }
    if (!m_is_empty_project) {
        m_specify_plate_idx = GUI::wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
        bool not_exist = std::find(m_plate_choices.begin(), m_plate_choices.end(), m_specify_plate_idx) == m_plate_choices.end();
        if (not_exist) {
            m_specify_plate_idx = m_plate_choices[0];
        }
    }
}

void SyncAmsInfoDialog::reinit_dialog()
{
    /* reset timeout and reading printer info */
    m_timeout_count     = 0;
    m_ams_mapping_res   = false;
    m_ams_mapping_valid = false;
    m_ams_mapping_result.clear();
    m_preview_colors_in_thumbnail.clear();

    show_status(PrintDialogStatus::PrintStatusInit);
    update_show_status();
}

void SyncAmsInfoDialog::init_bind()
{
    Bind(wxEVT_TIMER, &SyncAmsInfoDialog::on_timer, this);
    Bind(EVT_SHOW_ERROR_INFO, [this](auto &e) { show_print_failed_info(true); });
    Bind(EVT_UPDATE_USER_MACHINE_LIST, &SyncAmsInfoDialog::update_printer_combobox, this);
    Bind(EVT_PRINT_JOB_CANCEL, &SyncAmsInfoDialog::on_print_job_cancel, this);
    Bind(EVT_SET_FINISH_MAPPING, &SyncAmsInfoDialog::on_set_finish_mapping, this);
    Bind(wxEVT_LEFT_DOWN, [this](auto &e) {
        check_fcous_state(this);
        e.Skip();
    });
    m_basic_panel->Bind(wxEVT_LEFT_DOWN, [this](auto &e) {
        check_fcous_state(this);
        e.Skip();
    });

    Bind(EVT_CONNECT_LAN_MODE_PRINT, [this](wxCommandEvent &e) {
        if (e.GetInt() == 0) {
            DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
            if (!dev) return;
            MachineObject *obj = dev->get_selected_machine();
            if (!obj)
                return;
        }
    });
}

void SyncAmsInfoDialog::show_print_failed_info(bool show, int code, wxString description, wxString extra)
{
   //todo
}

void SyncAmsInfoDialog::check_fcous_state(wxWindow *window)
{
    auto children = window->GetChildren();
    for (auto child : children) { check_fcous_state(child); }
}

void SyncAmsInfoDialog::popup_filament_backup()
{
    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    if (dev->get_selected_machine() /* && dev->get_selected_machine()->filam_bak.size() > 0*/) {
        AmsReplaceMaterialDialog *m_replace_material_popup = new AmsReplaceMaterialDialog(this);
        m_replace_material_popup->update_machine_obj(dev->get_selected_machine());
        m_replace_material_popup->ShowModal();
    }
}

void SyncAmsInfoDialog::prepare_mode(bool refresh_button)
{
    // disable combobox
    Enable_Auto_Refill(true);
    show_print_failed_info(false);

    m_is_in_sending_mode = false;
    if (wxIsBusy()) wxEndBusyCursor();

    if (m_print_page_mode != PrintPageModePrepare) {
        m_print_page_mode = PrintPageModePrepare;
        for (auto it = m_materialList.begin(); it != m_materialList.end(); it++) { it->second->item->enable(); }
    }
}

void SyncAmsInfoDialog::finish_mode()
{
    m_print_page_mode    = PrintPageModeFinish;
    m_is_in_sending_mode = false;
    Layout();
    Fit();
}

void SyncAmsInfoDialog::sync_ams_mapping_result(std::vector<FilamentInfo> &result)
{
    m_back_ams_mapping_result = result;
    if (result.empty()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "ams_mapping result is empty";
        for (auto it = m_materialList.begin(); it != m_materialList.end(); it++) {
            wxString ams_id  = "Ext";
            wxColour ams_col = wxColour(0xCE, 0xCE, 0xCE);
            it->second->item->set_ams_info(ams_col, ams_id, true); // sync_ams_mapping_result
        }
        return;
    }
    for (auto f = result.begin(); f != result.end(); f++) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "ams_mapping f id = " << f->id << ", tray_id = " << f->tray_id << ", color = " << f->color << ", type = " << f->type;

        MaterialHash::iterator iter = m_materialList.begin();
        while (iter != m_materialList.end()) {
            int           id   = iter->second->id;
            Material *    item = iter->second;
            auto          m    = item->item;

            if (f->id == id) {
                wxString ams_id;
                wxColour ams_col;

                if (f->tray_id == VIRTUAL_TRAY_MAIN_ID || f->tray_id == VIRTUAL_TRAY_DEPUTY_ID) {
                    ams_id = "Ext";
                }

                else if (f->tray_id >= 0) {
                    ams_id = wxGetApp().transition_tridid(f->tray_id);
                    // ams_id = wxString::Format("%02d", f->tray_id + 1);
                } else {
                    ams_id = "-";
                }

                if (!f->color.empty()) {
                    ams_col = DevAmsTray::decode_color(f->color);
                } else {
                    // default color
                    ams_col = wxColour(0xCE, 0xCE, 0xCE);
                }
                std::vector<wxColour> cols;
                for (auto col : f->colors) { cols.push_back(DevAmsTray::decode_color(col)); }
                m->set_ams_info(ams_col, ams_id, f->ctype, cols,true);//sync_ams_mapping_result
                break;
            }
            iter++;
        }
    }
    auto tab_index = (MainFrame::TabPosition) dynamic_cast<Notebook *>(wxGetApp().tab_panel())->GetSelection();
    if (tab_index == MainFrame::TabPosition::tp3DEditor || tab_index == MainFrame::TabPosition::tpPreview) {
        updata_thumbnail_data_after_connected_printer();
    }
}

bool SyncAmsInfoDialog::do_ams_mapping(MachineObject *obj_)
{
    if (!obj_) return false;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " begin do_ams_mapping result";
    obj_->get_ams_colors(m_cur_colors_in_thumbnail);
    // try color and type mapping

    const auto &full_config    = wxGetApp().preset_bundle->full_config();
    const auto &project_config = wxGetApp().preset_bundle->project_config;
    size_t      nozzle_nums    = full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    m_filaments_map            = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_real_filament_maps(project_config);

    int               filament_result = 0;
    std::vector<bool> map_opt; // four values: use_left_ams, use_right_ams, use_left_ext, use_right_ext
    if (nozzle_nums > 1) {
        map_opt         = {true, true, true, true}; // four values: use_left_ams, use_right_ams, use_left_ext, use_right_ext
        filament_result = DevMappingUtil::ams_filament_mapping(obj_, m_filaments, m_ams_mapping_result, map_opt, std::vector<int>(),
                                                     wxGetApp().app_config->get_bool("ams_sync_match_full_use_color_dist") ? false : true);
    }
    // single nozzle
    else {
        if (obj_->is_support_amx_ext_mix_mapping()) {
            map_opt         = {false, true, false, true}; // four values: use_left_ams, use_right_ams, use_left_ext, use_right_ext
            filament_result = DevMappingUtil::ams_filament_mapping(obj_, m_filaments, m_ams_mapping_result, map_opt, std::vector<int>(),
                                                         wxGetApp().app_config->get_bool("ams_sync_match_full_use_color_dist") ? false : true);
            // auto_supply_with_ext(obj_->vt_slot);
        } else {
            map_opt         = {false, true, false, false};
            filament_result = DevMappingUtil::ams_filament_mapping(obj_, m_filaments, m_ams_mapping_result, map_opt);
        }
    }

    if (filament_result == 0) {
        print_ams_mapping_result(m_ams_mapping_result);
        std::string ams_array;
        std::string ams_array2;
        std::string mapping_info;
        get_ams_mapping_result(ams_array, ams_array2, mapping_info);
        sync_ams_mapping_result(m_ams_mapping_result);
        if (ams_array.empty()) {
            reset_ams_material();
            BOOST_LOG_TRIVIAL(info) << "ams_mapping_array=[]";
        } else {
            BOOST_LOG_TRIVIAL(info) << "ams_mapping_array=" << ams_array;
            BOOST_LOG_TRIVIAL(info) << "ams_mapping_array2=" << ams_array2;
            BOOST_LOG_TRIVIAL(info) << "ams_mapping_info=" << mapping_info;
        }
        deal_only_exist_ext_spool(obj_);
        show_thumbnail_page();
        return DevMappingUtil::is_valid_mapping_result(obj_, m_ams_mapping_result);
    } else {
        BOOST_LOG_TRIVIAL(info) << "filament_result != 0";
        // do not support ams mapping try to use order mapping
        bool is_valid = DevMappingUtil::is_valid_mapping_result(obj_, m_ams_mapping_result);
        if (filament_result != 1 && !is_valid) {
            // reset invalid result
            for (int i = 0; i < m_ams_mapping_result.size(); i++) {
                m_ams_mapping_result[i].tray_id  = -1;
                m_ams_mapping_result[i].distance = 99999;
            }
        }
        sync_ams_mapping_result(m_ams_mapping_result);
        deal_only_exist_ext_spool(obj_);
        show_thumbnail_page();
        return is_valid;
    }

    return true;
}

void SyncAmsInfoDialog::deal_only_exist_ext_spool(MachineObject *obj_) {
    if (!obj_)
        return;
    if (!m_append_color_text) { return; }
    bool only_exist_ext_spool_flag = m_only_exist_ext_spool_flag = !obj_->GetFilaSystem()->HasAms();
    SetTitle(only_exist_ext_spool_flag ? _L("Synchronize Filament Information") : _L("Synchronize AMS Filament Information"));
    m_append_color_text->SetLabel(only_exist_ext_spool_flag ? _L("Add unused filaments to filaments list.") :
                                                              _L("Add unused AMS filaments to filaments list."));
    if (m_map_mode == MapModeEnum::ColorMap) {
        m_tip_attention_color_map = only_exist_ext_spool_flag ? _L("Only synchronize filament type and color, not including slot information.") :
                                                                _L("Only synchronize filament type and color, not including AMS slot information.");
        m_tip_text->SetLabel(m_tip_attention_color_map);

    }
    if (m_ams_or_ext_text_in_colormap) {
        m_ams_or_ext_text_in_colormap->SetLabel((only_exist_ext_spool_flag ? _L("Ext spool") : _L("AMS")) + ":");
    }
    if (m_ams_or_ext_text_in_override) {
        m_ams_or_ext_text_in_override->SetLabel((only_exist_ext_spool_flag ? _L("Ext spool") : _L("AMS")) + ":");
    }
}

void SyncAmsInfoDialog::show_thumbnail_page()
{
    m_pages->ChangeSelection(1);
    m_pages->SetMinSize(wxSize(SyncAmsInfoDialogWidth, -1));
    m_pages->SetMaxSize(wxSize(SyncAmsInfoDialogWidth, -1));
}

bool SyncAmsInfoDialog::get_ams_mapping_result(std::string &mapping_array_str, std::string &mapping_array_str2, std::string &ams_mapping_info)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "m_ams_mapping_result.size:" << m_ams_mapping_result.empty();
    if (m_ams_mapping_result.empty())
        return false;

    bool valid_mapping_result = true;
    int  invalid_count        = 0;
    for (int i = 0; i < m_ams_mapping_result.size(); i++) {
        if (m_ams_mapping_result[i].tray_id == -1) {
            valid_mapping_result = false;
            invalid_count++;
        }
    }

    if (invalid_count == m_ams_mapping_result.size()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "invalid_count == m_ams_mapping_result.size()";
        return false;
    } else {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "check mapping_v0_json";
        json mapping_v0_json   = json::array();
        json mapping_v1_json   = json::array();
        json mapping_info_json = json::array();

        /* get filament maps */
        std::vector<int> filament_maps;
        Plater *         plater = wxGetApp().plater();
        if (plater) {
            PartPlate *curr_plate = plater->get_partplate_list().get_curr_plate();
            if (curr_plate) {
                filament_maps = curr_plate->get_filament_maps();
            } else {
                BOOST_LOG_TRIVIAL(error) << "get_ams_mapping_result, curr_plate is nullptr";
            }
        } else {
            BOOST_LOG_TRIVIAL(error) << "get_ams_mapping_result, plater is nullptr";
        }

        for (int i = 0; i < wxGetApp().preset_bundle->filament_presets.size(); i++) {
            int  tray_id = -1;
            json mapping_item_v1;
            mapping_item_v1["ams_id"]  = 0xff;
            mapping_item_v1["slot_id"] = 0xff;
            json mapping_item;
            mapping_item["ams"]          = tray_id;
            mapping_item["targetColor"]  = "";
            mapping_item["filamentId"]   = "";
            mapping_item["filamentType"] = "";
            for (int k = 0; k < m_ams_mapping_result.size(); k++) {
                if (m_ams_mapping_result[k].id == i) {
                    tray_id                      = m_ams_mapping_result[k].tray_id;
                    mapping_item["ams"]          = tray_id;
                    mapping_item["filamentType"] = m_filaments[k].type;
                    if (i >= 0 && i < wxGetApp().preset_bundle->filament_presets.size()) {
                        auto it = wxGetApp().preset_bundle->filaments.find_preset(wxGetApp().preset_bundle->filament_presets[i]);
                        if (it != nullptr) { mapping_item["filamentId"] = it->filament_id; }
                    }
                    /* nozzle id */
                    if (i >= 0 && i < filament_maps.size()) { mapping_item["nozzleId"] = convert_filament_map_nozzle_id_to_task_nozzle_id(filament_maps[i]); }
                    // convert #RRGGBB to RRGGBBAA
                    mapping_item["sourceColor"] = m_filaments[k].color;
                    mapping_item["targetColor"] = m_ams_mapping_result[k].color;
                    if (tray_id == VIRTUAL_TRAY_MAIN_ID || tray_id == VIRTUAL_TRAY_DEPUTY_ID) { tray_id = -1; }

                    /*new ams mapping data*/
                    try {
                        if (m_ams_mapping_result[k].ams_id.empty() || m_ams_mapping_result[k].slot_id.empty()) { // invalid case
                            mapping_item_v1["ams_id"]  = VIRTUAL_TRAY_MAIN_ID;
                            mapping_item_v1["slot_id"] = VIRTUAL_TRAY_MAIN_ID;
                        } else {
                            mapping_item_v1["ams_id"]  = std::stoi(m_ams_mapping_result[k].ams_id);
                            mapping_item_v1["slot_id"] = std::stoi(m_ams_mapping_result[k].slot_id);
                        }
                    } catch (...) {}
                }
            }
            mapping_v0_json.push_back(tray_id);
            mapping_v1_json.push_back(mapping_item_v1);
            mapping_info_json.push_back(mapping_item);
        }

        mapping_array_str  = mapping_v0_json.dump();
        mapping_array_str2 = mapping_v1_json.dump();

        ams_mapping_info = mapping_info_json.dump();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "mapping_array_str:" << mapping_array_str;
        return valid_mapping_result;
    }
    return true;
}

bool SyncAmsInfoDialog::build_nozzles_info(std::string &nozzles_info)
{
    /* init nozzles info */
    json nozzle_info_json = json::array();
    nozzles_info          = nozzle_info_json.dump();

    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) return false;
    auto opt_nozzle_diameters = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    if (opt_nozzle_diameters == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "build_nozzles_info, opt_nozzle_diameters is nullptr";
        return false;
    }
    auto opt_nozzle_volume_type = preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (opt_nozzle_volume_type == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "build_nozzles_info, opt_nozzle_volume_type is nullptr";
        return false;
    }
    json nozzle_item;
    /* only o1d two nozzles has build_nozzles info now */
    if (opt_nozzle_diameters->size() != 2) { return false; }
    for (size_t i = 0; i < opt_nozzle_diameters->size(); i++) {
        if (i == (size_t) ConfigNozzleIdx::NOZZLE_LEFT) {
            nozzle_item["id"] = CloudTaskNozzleId::NOZZLE_LEFT;
        } else if (i == (size_t) ConfigNozzleIdx::NOZZLE_RIGHT) {
            nozzle_item["id"] = CloudTaskNozzleId::NOZZLE_RIGHT;
        } else {
            /* unknown ConfigNozzleIdx */
            BOOST_LOG_TRIVIAL(error) << "build_nozzles_info, unknown ConfigNozzleIdx = " << i;
            assert(false);
            continue;
        }
        nozzle_item["type"] = nullptr;
        if (i >= 0 && i < opt_nozzle_volume_type->size()) { nozzle_item["flowSize"] = get_nozzle_volume_type_cloud_string((NozzleVolumeType) opt_nozzle_volume_type->get_at(i)); }
        if (i >= 0 && i < opt_nozzle_diameters->size()) { nozzle_item["diameter"] = opt_nozzle_diameters->get_at(i); }
        nozzle_info_json.push_back(nozzle_item);
    }
    nozzles_info = nozzle_info_json.dump();
    return true;
}

bool SyncAmsInfoDialog::can_hybrid_mapping(DevExtderSystem data)
{
    // Mixed mappings are not allowed
    return false;

    if (data.GetTotalExtderCount() <= 1 || !wxGetApp().preset_bundle) return false;

    // The default two extruders are left, right, but the order of the extruders on the machine is right, left.
    // Therefore, some adjustments need to be made.
    std::vector<std::string> flow_type_of_machine;
    for (auto it = data.GetExtruders().rbegin(); it != data.GetExtruders().rend(); it++) {
        // exist field is not updated, wait add
        // if (it->exist < 3) return false;
        std::string type_str = it->GetNozzleFlowType() ? "High Flow" : "Standard";
        flow_type_of_machine.push_back(type_str);
    }
    // get the nozzle type of preset --> flow_types
    const Preset &      current_printer = wxGetApp().preset_bundle->printers.get_selected_preset();
    const Preset *      base_printer    = wxGetApp().preset_bundle->printers.get_preset_base(current_printer);
    std::string         base_name       = base_printer->name;
    auto                flow_data       = wxGetApp().app_config->get_nozzle_volume_types_from_config(base_name);
    std::vector<string> flow_types;
    boost::split(flow_types, flow_data, boost::is_any_of(","));
    if (flow_types.size() <= 1 || flow_types.size() != flow_type_of_machine.size()) return false;

    // Only when all preset nozzle types and machine nozzle types are exactly the same, return true.
    auto type = flow_types[0];
    for (int i = 0; i < flow_types.size(); i++) {
        if (flow_types[i] != type || flow_type_of_machine[i] != type) return false;
    }
    return true;
}

// When filaments cannot be matched automatically, whether to use ext for automatic supply
void SyncAmsInfoDialog::auto_supply_with_ext(std::vector<DevAmsTray> slots)
{
    if (slots.size() <= 0) return;

    for (int i = 0; i < m_ams_mapping_result.size(); i++) {
        auto it = m_ams_mapping_result[i];
        if (it.ams_id == "") {
            DevAmsTray slot("");
            if (m_filaments_map[it.id] == 1 && slots.size() > 1)
                slot = slots[1];
            else if (m_filaments_map[it.id] == 2)
                slot = slots[0];
            if (slot.id.empty()) continue;
            m_ams_mapping_result[i].ams_id  = slot.id;
            m_ams_mapping_result[i].color   = slot.color;
            m_ams_mapping_result[i].type    = slot.m_fila_type;
            m_ams_mapping_result[i].colors  = slot.cols;
            m_ams_mapping_result[i].tray_id = atoi(slot.id.c_str());
            m_ams_mapping_result[i].slot_id = "0";
        }
    }
}

bool SyncAmsInfoDialog::is_nozzle_type_match(DevExtderSystem data, wxString &error_message) const
{
    if (data.GetTotalExtderCount() <= 1 || !wxGetApp().preset_bundle) return false;

    const auto &project_config = wxGetApp().preset_bundle->project_config;
    // check nozzle used
    auto                       used_filaments = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_used_filaments();                   // 1 based
    auto                       filament_maps  = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_real_filament_maps(project_config); // 1 based
    std::map<int, std::string> used_extruders_flow;
    std::vector<int>           used_extruders; // 0 based
    for (auto f : used_filaments) {
        int filament_extruder = filament_maps[f - 1] - 1;
        if (std::find(used_extruders.begin(), used_extruders.end(), filament_extruder) == used_extruders.end()) used_extruders.emplace_back(filament_extruder);
    }

    std::sort(used_extruders.begin(), used_extruders.end());

    auto nozzle_volume_type_opt = dynamic_cast<const ConfigOptionEnumsGeneric *>(wxGetApp().preset_bundle->project_config.option("nozzle_volume_type"));
    for (auto i = 0; i < used_extruders.size(); i++) {
        if (nozzle_volume_type_opt) {
            NozzleVolumeType nozzle_volume_type = (NozzleVolumeType) (nozzle_volume_type_opt->get_at(used_extruders[i]));
            if (nozzle_volume_type == NozzleVolumeType::nvtStandard) {
                used_extruders_flow[used_extruders[i]] = "Standard";
            } else {
                used_extruders_flow[used_extruders[i]] = "High Flow";
            }
        }
    }

    vector<int> map_extruders = {1, 0};

    // The default two extruders are left, right, but the order of the extruders on the machine is right, left.
    std::vector<std::string> flow_type_of_machine;
    for (auto it = data.GetExtruders().begin(); it != data.GetExtruders().end(); it++) {
        if (it->GetNozzleFlowType() == NozzleFlowType::H_FLOW) {
            flow_type_of_machine.push_back("High Flow");
        } else if (it->GetNozzleFlowType() == NozzleFlowType::S_FLOW) {
            flow_type_of_machine.push_back("Standard");
        }
    }

    // Only when all preset nozzle types and machine nozzle types are exactly the same, return true.
    for (std::map<int, std::string>::iterator it = used_extruders_flow.begin(); it != used_extruders_flow.end(); it++) {
        if (it->first >= 0 && it->first < map_extruders.size()) {
            int target_machine_nozzle_id = map_extruders[it->first];

            if (target_machine_nozzle_id < flow_type_of_machine.size()) {
                if (flow_type_of_machine[target_machine_nozzle_id] != used_extruders_flow[it->first]) {
                    wxString pos;
                    if (target_machine_nozzle_id == DEPUTY_EXTRUDER_ID) {
                        pos = _L("left nozzle");
                    } else if ((target_machine_nozzle_id == MAIN_EXTRUDER_ID)) {
                        pos = _L("right nozzle");
                    }

                    error_message = wxString::Format(_L("The nozzle flow setting of %s(%s) doesn't match with the slicing file(%s). "
                                                        "Please make sure the nozzle installed matches with settings in printer, "
                                                        "then set the corresponding printer preset while slicing."),
                                                     pos, flow_type_of_machine[target_machine_nozzle_id], used_extruders_flow[it->first]);
                    return false;
                }
            }
        }
        else {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "check error:array bound in map_extruders" << it->first;
            return false;
        }
    }
    return true;
}

int SyncAmsInfoDialog::convert_filament_map_nozzle_id_to_task_nozzle_id(int nozzle_id)
{
    if (nozzle_id == (int) FilamentMapNozzleId::NOZZLE_LEFT) {
        return (int) CloudTaskNozzleId::NOZZLE_LEFT;
    } else if (nozzle_id == (int) FilamentMapNozzleId::NOZZLE_RIGHT) {
        return (int) CloudTaskNozzleId::NOZZLE_RIGHT;
    } else {
        /* unsupported nozzle id */
        assert(false);
        return nozzle_id;
    }
}

void SyncAmsInfoDialog::prepare(int print_plate_idx) { m_print_plate_idx = print_plate_idx; }

void SyncAmsInfoDialog::update_ams_status_msg(wxString msg, bool is_warning)
{
    if (!m_statictext_ams_msg) { return; }
    auto colour = is_warning ? wxColour(0xFF, 0x6F, 0x00) : wxColour(0x6B, 0x6B, 0x6B);
    m_statictext_ams_msg->SetForegroundColour(colour);

    if (msg.empty()) {
        if (!m_statictext_ams_msg->GetLabel().empty()) {
            m_statictext_ams_msg->SetLabel(wxEmptyString);
            m_statictext_ams_msg->Hide();
            Layout();
            Fit();
        }
    } else {
        msg = format_text(msg);

        auto str_new = msg.utf8_string();
        stripWhiteSpace(str_new);

        auto str_old = m_statictext_ams_msg->GetLabel().utf8_string();
        stripWhiteSpace(str_old);

        if (str_new != str_old) {
            if (m_statictext_ams_msg->GetLabel() != msg) {
                m_statictext_ams_msg->SetLabel(msg);
                m_statictext_ams_msg->Wrap(FromDIP(600));
                m_statictext_ams_msg->Show();
                Layout();
                Fit();
            }
        }
    }
}

void SyncAmsInfoDialog::stripWhiteSpace(std::string &str)
{
    if (str == "") { return; }

    string::iterator cur_it;
    cur_it = str.begin();

    while (cur_it != str.end()) {
        if ((*cur_it) == '\n' || (*cur_it) == ' ') {
            cur_it = str.erase(cur_it);
        } else {
            cur_it++;
        }
    }
}

wxString SyncAmsInfoDialog::format_text(wxString &m_msg)
{
    if (wxGetApp().app_config->get("language") != "zh_CN") {
        return m_msg;
    }
    if (!m_statictext_ams_msg) { return m_msg; }
    wxString out_txt      = m_msg;
    wxString count_txt    = "";
    int      new_line_pos = 0;

    for (int i = 0; i < m_msg.length(); i++) {
        auto text_size = m_statictext_ams_msg->GetTextExtent(count_txt);
        if (text_size.x < (FromDIP(400))) {
            count_txt += m_msg[i];
        } else {
            out_txt.insert(i - 1, '\n');
            count_txt = "";
        }
    }
    return out_txt;
}

void SyncAmsInfoDialog::update_priner_status_msg(wxString msg, bool is_warning)
{
    auto colour = is_warning ? wxColour(0xFF, 0x6F, 0x00) : wxColour(0x6B, 0x6B, 0x6B);
    m_text_printer_msg->SetForegroundColour(colour);

    if (msg.empty()) {
        if (!m_text_printer_msg->GetLabel().empty()) {
            m_text_printer_msg->SetLabel(wxEmptyString);
            m_text_printer_msg->Hide();
            Layout();
            Fit();
        }
    } else {
        msg = format_text(msg);

        auto str_new = msg.utf8_string();
        stripWhiteSpace(str_new);

        auto str_old = m_text_printer_msg->GetLabel().utf8_string();
        stripWhiteSpace(str_old);

        if (str_new != str_old) {
            if (m_text_printer_msg->GetLabel() != msg) {
                m_text_printer_msg->SetLabel(msg);
                m_text_printer_msg->SetMinSize(wxSize(FromDIP(420), -1));
                m_text_printer_msg->SetMaxSize(wxSize(FromDIP(420), -1));
                m_text_printer_msg->Wrap(FromDIP(420));
                m_text_printer_msg->Show();
                Layout();
                Fit();
            }
        }
    }
}

void SyncAmsInfoDialog::update_print_status_msg(wxString msg, bool is_warning, bool is_printer_msg)
{
    if (is_printer_msg) {
        update_ams_status_msg(wxEmptyString, false);
        update_priner_status_msg(msg, is_warning);
    } else {
        update_ams_status_msg(msg, is_warning);
        update_priner_status_msg(wxEmptyString, false);
    }
}

void SyncAmsInfoDialog::update_print_error_info(int code, std::string msg, std::string extra)
{
    m_print_error_code  = code;
    m_print_error_msg   = msg;
    m_print_error_extra = extra;
}

void SyncAmsInfoDialog::show_status(PrintDialogStatus status, std::vector<wxString> params)
{
    if (m_print_status != status) {
        m_result.is_same_printer = true;
        BOOST_LOG_TRIVIAL(info) << "select_machine_dialog: show_status = " << status << "(" << PrePrintChecker::get_print_status_info(status) << ")";
    }
    m_print_status = status;

    // other
    if (status == PrintDialogStatus::PrintStatusInit) {
        update_print_status_msg(wxEmptyString, false, false);
    } else if (status == PrintDialogStatus::PrintStatusInvalidPrinter) {
        update_print_status_msg(wxEmptyString, true, true);
    } else if (status == PrintDialogStatus::PrintStatusConnectingServer) {
        wxString msg_text = _L("Connecting to server...");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusReading) {
        wxString msg_text = _L("Synchronizing device information...");
        update_print_status_msg(msg_text, false, true);
    } else if (status == PrintDialogStatus::PrintStatusReadingFinished) {
        update_print_status_msg(wxEmptyString, false, true);
    } else if (status == PrintDialogStatus::PrintStatusReadingTimeout) {
        wxString msg_text = _L("Synchronizing device information timed out.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusInUpgrading) {
        wxString msg_text = _L("Cannot send a print job while the printer is updating firmware.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusInSystemPrinting) {
        wxString msg_text = _L("The printer is executing instructions. Please restart printing after it ends.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusInPrinting) {
        wxString msg_text = _L("The printer is busy with another print job.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusAmsMappingSuccess) {
        update_print_status_msg(wxEmptyString, false, false);
    } else if (status == PrintDialogStatus::PrintStatusAmsMappingInvalid) {
        update_print_status_msg(wxEmptyString, true, false);
    } else if (status == PrintDialogStatus::PrintStatusAmsMappingMixInvalid) {
        wxString msg_text = _L("Please do not mix-use the Ext with AMS.");
        update_print_status_msg(msg_text, true, false);
    } else if (status == PrintDialogStatus::PrintStatusNozzleDataInvalid) {
        wxString msg_text = _L("Invalid nozzle information, please refresh or manually set nozzle information.");
        update_print_status_msg(msg_text, true, false);
    } else if (status == PrintDialogStatus::PrintStatusNozzleMatchInvalid) {
        wxString msg_text = _L("Please check whether the nozzle type of the device is the same as the preset nozzle type.");
        update_print_status_msg(msg_text, true, false);
    } else if (status == PrintDialogStatus::PrintStatusAmsMappingU0Invalid) {
        wxString msg_text;
        if (params.size() > 1)
            msg_text = wxString::Format(_L("Filament %s does not match the filament in AMS slot %s. Please update the printer firmware to support AMS slot assignment."),
                                        params[0], params[1]);
        else
            msg_text = _L("Filament does not match the filament in AMS slot. Please update the printer firmware to support AMS slot assignment.");
    } else if (status == PrintDialogStatus::PrintStatusRefreshingMachineList) {
        update_print_status_msg(wxEmptyString, false, true);
    } else if (status == PrintDialogStatus::PrintStatusSending) {
    } else if (status == PrintDialogStatus::PrintStatusSendingCanceled) {
    } else if (status == PrintDialogStatus::PrintStatusLanModeNoSdcard) {
        wxString msg_text = _L("Storage needs to be inserted before printing via LAN.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusLanModeSDcardNotAvailable) {
        wxString msg_text = _L("Storage is not available or is in read-only mode.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusNoSdcard) {
        wxString msg_text = _L("Storage needs to be inserted before printing.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusUnsupportedPrinter) {
        wxString msg_text;
        try {
            DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
            if (!dev) return;

            // source print
            MachineObject *obj_ = dev->get_selected_machine();
            if (obj_ == nullptr) return;
            auto sourcet_print_name = obj_->get_printer_type_display_str();
            sourcet_print_name.Replace(wxT("Bambu Lab "), wxEmptyString);

            // target print
            std::string target_model_id;
            if (m_print_type == PrintFromType::FROM_NORMAL) {
                PresetBundle *preset_bundle = wxGetApp().preset_bundle;
                target_model_id             = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
            } else if (m_print_type == PrintFromType::FROM_SDCARD_VIEW) {
                if (m_required_data_plate_data_list.size() > 0) { target_model_id = m_required_data_plate_data_list[m_print_plate_idx]->printer_model_id; }
            }

            auto target_print_name = wxString(DevPrinterConfigUtil::get_printer_display_name(target_model_id));
            target_print_name.Replace(wxT("Bambu Lab "), wxEmptyString);
            msg_text = wxString::Format(_L("The selected printer (%s) is incompatible with the chosen printer profile in the slicer (%s)."), sourcet_print_name,
                                        target_print_name);
            if (m_result.is_same_printer) {
                m_result.is_same_printer = false;
                updata_ui_when_priner_not_same();
            }
            return;
        } catch (...) {}
    } else if (status == PrintDialogStatus::PrintStatusTimelapseNoSdcard) {
        wxString msg_text = _L("Storage needs to be inserted to record timelapse.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusNeedForceUpgrading) {
        wxString msg_text = _L("Cannot send the print job to a printer whose firmware is required to get updated.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusNeedConsistencyUpgrading) {
        wxString msg_text = _L("Cannot send the print job to a printer whose firmware is required to get updated.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusBlankPlate) {
        wxString msg_text = _L("Cannot send a print job for an empty plate.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusNotSupportedPrintAll) {
        wxString msg_text = _L("This printer does not support printing all plates.");
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintDialogStatus::PrintStatusTimelapseWarning) {
        wxString   msg_text;
        PartPlate *plate = m_plater->get_partplate_list().get_curr_plate();
        for (auto warning : plate->get_slice_result()->warnings) {
            if (warning.msg == NOT_GENERATE_TIMELAPSE) {
                if (warning.error_code == "10014001") {
                    msg_text = _L("When enable spiral vase mode, machines with I3 structure will not generate timelapse videos.");
                } else if (warning.error_code == "10014002") {
                    msg_text = _L("Timelapse is not supported because Print sequence is set to \"By object\".");
                }
            }
        }
        update_print_status_msg(msg_text, true, true);
    } else if (status == PrintStatusMixAmsAndVtSlotWarning) {
        wxString msg_text = _L("You selected external and AMS filament at the same time in an extruder, you will need manually change external filament.");
        update_print_status_msg(msg_text, true, false);
    }

    // m_panel_warn m_simplebook
    if (status == PrintDialogStatus::PrintStatusSending) {
        ; //
    } else {
        prepare_mode(false);
    }
}

void SyncAmsInfoDialog::init_timer()
{
    m_refresh_timer = new wxTimer();
    m_refresh_timer->SetOwner(this);
}

void SyncAmsInfoDialog::on_cancel(wxCloseEvent &event)
{
    if (m_mapping_popup.IsShown())
        m_mapping_popup.Dismiss();

    this->EndModal(wxID_CANCEL);
}

bool SyncAmsInfoDialog::is_blocking_printing(MachineObject *obj_)
{
    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return true;
    auto        target_model = obj_->printer_type;
    std::string source_model = "";

    if (m_print_type == PrintFromType::FROM_NORMAL) {
        PresetBundle *preset_bundle = wxGetApp().preset_bundle;
        source_model                = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);

    } else if (m_print_type == PrintFromType::FROM_SDCARD_VIEW) {
        if (m_required_data_plate_data_list.size() > 0) { source_model = m_required_data_plate_data_list[m_print_plate_idx]->printer_model_id; }
    }

    if (source_model != target_model) {
        std::vector<std::string>      compatible_machine = obj_->get_compatible_machine();
        vector<std::string>::iterator it                 = find(compatible_machine.begin(), compatible_machine.end(), source_model);
        if (it == compatible_machine.end()) { return true; }
    }

    return false;
}

bool SyncAmsInfoDialog::is_same_nozzle_diameters(NozzleType &tag_nozzle_type, float &nozzle_diameter)
{
    bool is_same_nozzle_diameters = true;

    float       preset_nozzle_diameters;
    std::string preset_nozzle_type;

    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return true;

    MachineObject *obj_ = dev->get_selected_machine();
    if (obj_ == nullptr) return true;

    try {
        PresetBundle *preset_bundle        = wxGetApp().preset_bundle;
        auto          opt_nozzle_diameters = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");

        const ConfigOptionEnumsGenericNullable *nozzle_type = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnumsGenericNullable>("nozzle_type");
        std::vector<std::string>                preset_nozzle_types(nozzle_type->size());
        for (size_t idx = 0; idx < nozzle_type->size(); ++idx) preset_nozzle_types[idx] = NozzleTypeEumnToStr[NozzleType(nozzle_type->values[idx])];

        std::vector<std::string> machine_nozzle_types(obj_->GetExtderSystem()->GetTotalExtderCount());
        for (size_t idx = 0; idx < obj_->GetExtderSystem()->GetTotalExtderCount(); ++idx) machine_nozzle_types[idx] = obj_->GetExtderSystem()->GetNozzleType(idx);

        auto used_filaments = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_used_filaments();                                  // 1 based
        auto filament_maps  = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_real_filament_maps(preset_bundle->project_config); // 1 based

        std::vector<int> used_extruders; // 0 based
        for (auto f : used_filaments) {
            int filament_extruder = filament_maps[f - 1] - 1;
            if (std::find(used_extruders.begin(), used_extruders.end(), filament_extruder) == used_extruders.end()) used_extruders.emplace_back(filament_extruder);
        }
        std::sort(used_extruders.begin(), used_extruders.end());

        // TODO [tao wang] : add idx mapping
        tag_nozzle_type = obj_->GetExtderSystem()->GetNozzleType(0);

        if (opt_nozzle_diameters != nullptr) {
            for (auto i = 0; i < used_extruders.size(); i++) {
                auto extruder           = used_extruders[i];
                preset_nozzle_diameters = float(opt_nozzle_diameters->get_at(extruder));
                if (preset_nozzle_diameters != obj_->GetExtderSystem()->GetNozzleDiameter(0)) { is_same_nozzle_diameters = false; }
            }
        }

    } catch (...) {}

    nozzle_diameter = preset_nozzle_diameters;

    return is_same_nozzle_diameters;
}

bool SyncAmsInfoDialog::is_same_nozzle_type(std::string &filament_type, NozzleType &tag_nozzle_type)
{
    bool is_same_nozzle_type = true;

    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return true;

    MachineObject *obj_ = dev->get_selected_machine();
    if (obj_ == nullptr) return true;

    NozzleType nozzle_type        = obj_->GetExtderSystem()->GetNozzleType(0);
    auto       printer_nozzle_hrc = Print::get_hrc_by_nozzle_type(nozzle_type);

    auto                   preset_bundle = wxGetApp().preset_bundle;
    MaterialHash::iterator iter          = m_materialList.begin();
    while (iter != m_materialList.end()) {
        Material *    item                = iter->second;
        auto               m              = item->item;
        auto          filament_nozzle_hrc = preset_bundle->get_required_hrc_by_filament_type(m->m_material_name.ToStdString());

        if (abs(filament_nozzle_hrc) > abs(printer_nozzle_hrc)) {
            filament_type = m->m_material_name.ToStdString();
            BOOST_LOG_TRIVIAL(info) << "filaments hardness mismatch: filament = " << filament_type << " printer_nozzle_hrc = " << printer_nozzle_hrc;
            is_same_nozzle_type = false;
            tag_nozzle_type     = NozzleType::ntHardenedSteel;
            return is_same_nozzle_type;
        } else {
            tag_nozzle_type = obj_->GetExtderSystem()->GetNozzleType(0);
        }

        iter++;
    }

    return is_same_nozzle_type;
}

bool SyncAmsInfoDialog::is_same_printer_model()
{
    bool           result = true;
    DeviceManager *dev    = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return result;

    MachineObject *obj_ = dev->get_selected_machine();

    if (obj_ == nullptr) { return result; }

    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle && preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle) != obj_->printer_type) {
        if ((obj_->is_support_upgrade_kit && obj_->installed_upgrade_kit) && (preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle) == "C12")) {
            return true;
        }

        BOOST_LOG_TRIVIAL(info) << "printer_model: source = " << preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
        BOOST_LOG_TRIVIAL(info) << "printer_model: target = " << obj_->printer_type;
        return false;
    }

    if (obj_->is_support_upgrade_kit && obj_->installed_upgrade_kit) {
        BOOST_LOG_TRIVIAL(info) << "printer_model: source = " << preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
        BOOST_LOG_TRIVIAL(info) << "printer_model: target = " << obj_->printer_type << " (plus)";
        return false;
    }

    return true;
}

void SyncAmsInfoDialog::show_errors(wxString &info)
{
    ConfirmBeforeSendDialog confirm_dlg(this, wxID_ANY, _L("Errors"));
    confirm_dlg.update_text(info);
    confirm_dlg.on_show();
}

void SyncAmsInfoDialog::Enable_Auto_Refill(bool enable)
{
    if (!m_ams_backup_tip) { return; }
    if (enable) {
        m_ams_backup_tip->SetForegroundColour(wxColour("#009688"));
    } else {
        m_ams_backup_tip->SetForegroundColour(wxColour(0x90, 0x90, 0x90));
    }
    m_ams_backup_tip->Refresh();
}

void SyncAmsInfoDialog::update_user_machine_list()
{
    NetworkAgent *m_agent = wxGetApp().getAgent();
    if (m_agent && m_agent->is_user_login()) {
        boost::thread get_print_info_thread = Slic3r::create_thread([this, token = std::weak_ptr(m_token)] {
            NetworkAgent *agent = wxGetApp().getAgent();
            unsigned int  http_code;
            std::string   body;
            int           result = agent->get_user_print_info(&http_code, &body);
            CallAfter([token, this, result, body] {
                if (token.expired()) { return; }
                if (result == 0) {
                    m_print_info = body;
                } else {
                    m_print_info = "";
                }
                wxCommandEvent event(EVT_UPDATE_USER_MACHINE_LIST);
                event.SetEventObject(this);
                wxPostEvent(this, event);
            });
        });
    } else {
        wxCommandEvent event(EVT_UPDATE_USER_MACHINE_LIST);
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

void SyncAmsInfoDialog::on_refresh(wxCommandEvent &event)
{
    if (m_is_empty_project) {
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "m_printer_last_select: on_refresh";
    show_status(PrintDialogStatus::PrintStatusRefreshingMachineList);

    update_user_machine_list();
}

void SyncAmsInfoDialog::on_set_finish_mapping(wxCommandEvent &evt)
{
    auto selection_data     = evt.GetString();
    auto selection_data_arr = wxSplit(selection_data.ToStdString(), '|');

    BOOST_LOG_TRIVIAL(info) << "The ams mapping selection result: data is " << selection_data;

    if (selection_data_arr.size() == 8) {
        auto ams_colour      = wxColour(wxAtoi(selection_data_arr[0]), wxAtoi(selection_data_arr[1]), wxAtoi(selection_data_arr[2]), wxAtoi(selection_data_arr[3]));
        int  old_filament_id = (int) wxAtoi(selection_data_arr[5]);
        if (m_print_type == PrintFromType::FROM_NORMAL) { // todo:support sd card
            change_default_normal(old_filament_id, ams_colour);
            final_deal_edge_pixels_data(m_preview_thumbnail_data);
            set_default_normal(m_preview_thumbnail_data); // do't reset ams
            if (!m_reset_all_btn->IsShown()) {
                m_reset_all_btn->Show();
                Layout();
                Fit();
            }
        }

        int                      ctype = 0;
        std::vector<wxColour>    material_cols;
        std::vector<std::string> tray_cols;
        for (auto mapping_item : m_mapping_popup.m_mapping_item_list) {
            if (mapping_item->m_tray_data.id == evt.GetInt()) {
                ctype         = mapping_item->m_tray_data.ctype;
                material_cols = mapping_item->m_tray_data.material_cols;
                for (auto col : mapping_item->m_tray_data.material_cols) {
                    wxString color = wxString::Format("#%02X%02X%02X%02X", col.Red(), col.Green(), col.Blue(), col.Alpha());
                    tray_cols.push_back(color.ToStdString());
                }
                break;
            }
        }

        for (auto i = 0; i < m_ams_mapping_result.size(); i++) {
            if (m_ams_mapping_result[i].id == wxAtoi(selection_data_arr[5])) {
                m_ams_mapping_result[i].tray_id = evt.GetInt();
                auto     ams_colour = wxColour(wxAtoi(selection_data_arr[0]), wxAtoi(selection_data_arr[1]), wxAtoi(selection_data_arr[2]), wxAtoi(selection_data_arr[3]));
                wxString color      = wxString::Format("#%02X%02X%02X%02X", ams_colour.Red(), ams_colour.Green(), ams_colour.Blue(), ams_colour.Alpha());
                m_ams_mapping_result[i].color  = color.ToStdString();
                m_ams_mapping_result[i].ctype  = ctype;
                m_ams_mapping_result[i].colors = tray_cols;

                m_ams_mapping_result[i].ams_id  = selection_data_arr[6].ToStdString();
                m_ams_mapping_result[i].slot_id = selection_data_arr[7].ToStdString();
            }
            BOOST_LOG_TRIVIAL(info) << "The ams mapping result: id is " << m_ams_mapping_result[i].id << "tray_id is " << m_ams_mapping_result[i].tray_id;
        }

        MaterialHash::iterator iter = m_materialList.begin();
        while (iter != m_materialList.end()) {
            Material *    item = iter->second;
            auto          m    = item->item;
            if (item->id == m_current_filament_id) {
                auto ams_colour = wxColour(wxAtoi(selection_data_arr[0]), wxAtoi(selection_data_arr[1]), wxAtoi(selection_data_arr[2]), wxAtoi(selection_data_arr[3]));
                m->set_ams_info(ams_colour, selection_data_arr[4], ctype, material_cols);//finish
            }
            iter++;
        }
    }
}

void SyncAmsInfoDialog::on_print_job_cancel(wxCommandEvent &evt)
{
    BOOST_LOG_TRIVIAL(info) << "print_job: canceled";
    show_status(PrintDialogStatus::PrintStatusInit);
    // enter prepare mode
    prepare_mode();
}

std::vector<std::string> SyncAmsInfoDialog::sort_string(std::vector<std::string> strArray)
{
    std::vector<std::string> outputArray;
    std::sort(strArray.begin(), strArray.end());
    std::vector<std::string>::iterator st;
    for (st = strArray.begin(); st != strArray.end(); st++) { outputArray.push_back(*st); }

    return outputArray;
}

bool SyncAmsInfoDialog::is_timeout()
{
    if (m_timeout_count > 15 * 1000 / LIST_REFRESH_INTERVAL) { return true; }
    return false;
}

int SyncAmsInfoDialog::update_print_required_data(
    Slic3r::DynamicPrintConfig config, Slic3r::Model model, Slic3r::PlateDataPtrs plate_data_list, std::string file_name, std::string file_path)
{
    m_required_data_plate_data_list.clear();
    m_required_data_config = config;
    m_required_data_model  = model;
    // m_required_data_plate_data_list = plate_data_list;
    for (auto i = 0; i < plate_data_list.size(); i++) {
        if (!plate_data_list[i]->gcode_file.empty()) { m_required_data_plate_data_list.push_back(plate_data_list[i]); }
    }

    m_required_data_file_name = file_name;
    m_required_data_file_path = file_path;
    return m_required_data_plate_data_list.size();
}

void SyncAmsInfoDialog::reset_timeout() { m_timeout_count = 0; }

void SyncAmsInfoDialog::update_user_printer()
{
    Slic3r::DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    // update user print info
    if (!m_print_info.empty()) {
        dev->parse_user_print_info(m_print_info);
        m_print_info = "";
    }

    // clear machine list
    m_list.clear();
    std::vector<std::string>               machine_list;
    wxArrayString                          machine_list_name;
    std::map<std::string, MachineObject *> option_list;

    // user machine list
    option_list = dev->get_my_machine_list();

    // same machine only appear once
    for (auto it = option_list.begin(); it != option_list.end(); it++) {
        if (it->second && (it->second->is_online() || it->second->is_connected())) { machine_list.push_back(it->second->get_dev_name()); }
    }

    // lan machine list
    auto lan_option_list = dev->get_local_machinelist();

    for (auto elem : lan_option_list) {
        MachineObject *mobj = elem.second;

        /* do not show printer bind state is empty */
        if (!mobj->is_avaliable()) continue;
        if (!mobj->is_online()) continue;
        if (!mobj->is_lan_mode_printer()) continue;
        if (!mobj->has_access_right()) {
            option_list[mobj->get_dev_name()] = mobj;
            machine_list.push_back(mobj->get_dev_name());
        }
    }

    machine_list = sort_string(machine_list);
    for (auto tt = machine_list.begin(); tt != machine_list.end(); tt++) {
        for (auto it = option_list.begin(); it != option_list.end(); it++) {
            if (it->second->get_dev_name() == *tt)
            {
                m_list.push_back(it->second);
                wxString dev_name_text = from_u8(it->second->get_dev_name());
                if (it->second->is_lan_mode_printer()) { dev_name_text += "(LAN)"; }
                machine_list_name.Add(dev_name_text);
                break;
            }
        }
    }
}

void SyncAmsInfoDialog::update_printer_combobox(wxCommandEvent &event)
{
    show_status(PrintDialogStatus::PrintStatusInit);
    update_user_printer();
}

void SyncAmsInfoDialog::on_timer(wxTimerEvent &event)
{
    update_show_status();

    /// show auto refill
    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    MachineObject *obj_ = dev->get_selected_machine();
    if (!obj_) return;

    if (!m_check_flag && obj_->is_info_ready()) {
        update_select_layout(obj_);
        m_check_flag = true;
    }

    if (!obj_ || !obj_->GetFilaSystem()->HasAms() || obj_->ams_exist_bits == 0 || !obj_->is_support_filament_backup || !obj_->GetExtderSystem()->HasFilamentBackup() ||
        !obj_->GetFilaSystem()->IsAutoRefillEnabled() || m_checkbox_list["use_ams"]->getValue() != "on") {
        if (m_ams_backup_tip && m_ams_backup_tip->IsShown()) {
            m_ams_backup_tip->Hide();
            img_ams_backup->Hide();
            Layout();
            Fit();
        }
    } else {
        if (m_ams_backup_tip && !m_ams_backup_tip->IsShown()) {
            m_ams_backup_tip->Show();
            img_ams_backup->Show();
            Layout();
            Fit();
        }
    }
}

void SyncAmsInfoDialog::update_show_status()
{
    // refreshing return
    if (get_status() == PrintDialogStatus::PrintStatusRefreshingMachineList)
        return;
    if (get_status() == PrintDialogStatus::PrintStatusSending)
        return;
    if (get_status() == PrintDialogStatus::PrintStatusSendingCanceled)
        return;

    NetworkAgent * agent = Slic3r::GUI::wxGetApp().getAgent();
    DeviceManager *dev   = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!agent) {
        return;
    }
    if (!dev) return;

    // blank plate has no valid gcode file
    if (is_must_finish_slice_then_connected_printer()) { return; }
    MachineObject * obj_ = dev->get_selected_machine();
    if (!obj_) {
        if (agent) {
            if (agent->is_user_login()) {
                show_status(PrintDialogStatus::PrintStatusInvalidPrinter);
            }
        }
        return;
    }

    /* check cloud machine connections */
    if (!obj_->is_lan_mode_printer()) {
        if (!agent->is_server_connected()) {
            show_status(PrintDialogStatus::PrintStatusConnectingServer);
            reset_timeout();
            return;
        }
    }

    if (!obj_->is_info_ready()) {
        if (is_timeout()) {
            BOOST_LOG_TRIVIAL(error) << "check error:machine timeout";
            m_ams_mapping_result.clear();
            sync_ams_mapping_result(m_ams_mapping_result);
            show_status(PrintDialogStatus::PrintStatusReadingTimeout);
            return;
        } else {
            m_timeout_count++;
            show_status(PrintDialogStatus::PrintStatusReading);
            return;
        }
        return;
    }

    reset_timeout();

    if (!obj_->GetConfig()->SupportPrintAllPlates() && m_print_plate_idx == PLATE_ALL_IDX) {
        show_status(PrintDialogStatus::PrintStatusNotSupportedPrintAll);
        return;
    }

    // do ams mapping if no ams result
    if (m_ams_mapping_result.empty()) {
        do_ams_mapping(obj_);
    }


    // reading done
    if (wxGetApp().app_config) {
        if (obj_->upgrade_force_upgrade) {
            show_status(PrintDialogStatus::PrintStatusNeedForceUpgrading);
            return;
        }

        if (obj_->upgrade_consistency_request) {
            show_status(PrintStatusNeedConsistencyUpgrading);
            return;
        }
    }

    if (is_blocking_printing(obj_)) {
        show_status(PrintDialogStatus::PrintStatusUnsupportedPrinter);
        return;
    } else if (obj_->is_in_upgrading()) {
        show_status(PrintDialogStatus::PrintStatusInUpgrading);
        return;
    } else if (obj_->is_system_printing()) {
        show_status(PrintDialogStatus::PrintStatusInSystemPrinting);
        return;
    } else if (obj_->is_in_printing() || obj_->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE) {
        show_status(PrintDialogStatus::PrintStatusInPrinting);
        return;
    } else if (!obj_->GetConfig()->SupportPrintWithoutSD() && (obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::NO_SDCARD)) {
        show_status(PrintDialogStatus::PrintStatusNoSdcard);
        return;
    }

    // check sdcard when if lan mode printer
    if (obj_->is_lan_mode_printer()) {
        if (obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::NO_SDCARD) {
            show_status(PrintDialogStatus::PrintStatusLanModeNoSdcard);
            return;
        } else if (obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL || obj_->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_READONLY) {
            show_status(PrintDialogStatus::PrintStatusLanModeSDcardNotAvailable);
            return;
        }
    }

    // do ams mapping if no ams result
    if (m_ams_mapping_result.empty()) { do_ams_mapping(obj_); }

    const auto &full_config = wxGetApp().preset_bundle->full_config();
    size_t      nozzle_nums = full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();

    // the nozzle type of preset and machine are different
    if (nozzle_nums > 1) {
         wxString error_message;
        if (!is_nozzle_type_match(*obj_->GetExtderSystem(), error_message)) {
            std::vector<wxString> params{error_message};
            params.emplace_back(_L("Tips: If you changed your nozzle of your printer lately, Please go to 'Device -> Printer parts' to change your nozzle setting."));
            show_status(PrintDialogStatus::PrintStatusNozzleMatchInvalid, params);
            return;
        }
    }

    // check ams and vt_slot mix use status
    {
        struct ExtruderStatus
        {
            bool has_ams{false};
            bool has_vt_slot{false};
        };
        std::vector<ExtruderStatus> extruder_status(nozzle_nums);
        for (const FilamentInfo &item : m_ams_mapping_result) {
            if (item.ams_id.empty()) continue;

            int extruder_id = obj_->get_extruder_id_by_ams_id(item.ams_id);
            if (devPrinterUtil::IsVirtualSlot(item.ams_id))
                extruder_status[extruder_id].has_vt_slot = true;
            else
                extruder_status[extruder_id].has_ams = true;
        }
        for (auto extruder : extruder_status) {
            if (extruder.has_ams && extruder.has_vt_slot) {
                show_status(PrintDialogStatus::PrintStatusMixAmsAndVtSlotWarning);
                return;
            }
        }
    }
}

bool SyncAmsInfoDialog::has_timelapse_warning()
{
    PartPlate *plate = m_plater->get_partplate_list().get_curr_plate();
    for (auto warning : plate->get_slice_result()->warnings) {
        if (warning.msg == NOT_GENERATE_TIMELAPSE) { return true; }
    }

    return false;
}

void SyncAmsInfoDialog::update_timelapse_enable_status()
{
    AppConfig *config = wxGetApp().app_config;
    if (!has_timelapse_warning()) {
        if (!config || config->get("print", "timelapse") == "0")
            m_checkbox_list["timelapse"]->setValue("off");
        else
            m_checkbox_list["timelapse"]->setValue("on");
        m_checkbox_list["timelapse"]->Enable(true);
    } else {
        m_checkbox_list["timelapse"]->setValue("off");
        m_checkbox_list["timelapse"]->Enable(false);
        if (config) { config->set_str("print", "timelapse", "0"); }
    }
}

void SyncAmsInfoDialog::reset_ams_material()
{
    MaterialHash::iterator iter = m_materialList.begin();
    while (iter != m_materialList.end()) {
        int           id      = iter->first;
        Material *    item    = iter->second;
        auto          m       = item->item;
        m->reset_ams_info();
        iter++;
    }
}

void SyncAmsInfoDialog::reset_all_ams_info()
{
    for (int i = 0; i < m_ams_mapping_result.size(); i++) {
        reset_one_ams_material(std::to_string(i+1),true);
    }
    sync_ams_mapping_result(m_ams_mapping_result);
    update_final_thumbnail_data();
    m_reset_all_btn->Hide();
    Refresh();
}

void SyncAmsInfoDialog::reset_one_ams_material(const std::string &index_str, bool reset_to_first)
{
    MaterialHash::iterator iter = m_materialList.begin();
    while (iter != m_materialList.end()) {
        int       id      = iter->first;
        Material *item    = iter->second;
        auto m    = dynamic_cast<MaterialSyncItem*> (item->item);
        if (m && m->get_material_index_str() == index_str) {
            if (reset_to_first) {
                m->reset_valid_info();
            } else {
                m->reset_ams_info();
            }

            int index = std::atoi(index_str.c_str()) - 1;
            if (index >=0 && index < m_back_ams_mapping_result.size()) {
                if (reset_to_first) {
                    m_ams_mapping_result[index] = m_back_ams_mapping_result[index];
                } else {
                    m_ams_mapping_result[index].ams_id = "";
                    m_ams_mapping_result[index].slot_id = "";
                    m_ams_mapping_result[index].color = "";
                }
            }
            break;
        }
        iter++;
    }
}

void SyncAmsInfoDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    if (ams_mapping_help_icon != nullptr) {
        ams_mapping_help_icon->msw_rescale();
        if (img_amsmapping_tip)
            img_amsmapping_tip->SetBitmap(ams_mapping_help_icon->bmp());
    }

    for (auto material1 : m_materialList) {
        material1.second->item->msw_rescale();
    }
    m_swipe_left_button->msw_rescale();
    m_swipe_right_button->msw_rescale();
    m_button_ok->SetMinSize(OK_BUTTON_SIZE);
    m_button_ok->SetCornerRadius(FromDIP(12));
    m_button_cancel->SetMinSize(CANCEL_BUTTON_SIZE);
    m_button_cancel->SetCornerRadius(FromDIP(12));
    m_merge_color_checkbox->Rescale();
    m_append_color_checkbox->Rescale();
    m_combobox_plate->Rescale();
    Fit();
    Refresh();
}

void SyncAmsInfoDialog::set_default(bool hide_some)
{
    if (m_print_type == PrintFromType::FROM_NORMAL) {
        bool is_show = true;
    } else if (m_print_type == PrintFromType::FROM_SDCARD_VIEW) {
        bool is_show = false;
    }

    wxString filename = m_plater->get_export_gcode_filename("", true, m_print_plate_idx == PLATE_ALL_IDX ? true : false);
    if (m_print_plate_idx == PLATE_ALL_IDX && filename.empty()) { filename = _L("Untitled"); }

    if (filename.empty()) {
        filename = m_plater->get_export_gcode_filename("", true);
        if (filename.empty()) filename = _L("Untitled");
    }

    fs::path    filename_path(filename.c_str());
    std::string file_name = filename_path.filename().string();
    if (from_u8(file_name).find(_L("Untitled")) != wxString::npos) {
        PartPlate *part_plate = m_plater->get_partplate_list().get_plate(m_print_plate_idx);
        if (part_plate) {
            if (std::vector<ModelObject *> objects = part_plate->get_objects_on_this_plate(); objects.size() > 0) {
                file_name = objects[0]->name;
                for (int i = 1; i < objects.size(); i++) { file_name += (" + " + objects[i]->name); }
            }
            if (file_name.size() > 100) { file_name = file_name.substr(0, 97) + "..."; }
        }
    }
    m_current_project_name = wxString::FromUTF8(file_name);

    // unsupported character filter
    m_current_project_name = from_u8(filter_characters(m_current_project_name.ToUTF8().data(), "<>[]:/\\|?*\""));

    // clear combobox
    m_list.clear();
    m_print_info          = "";
    // rset status bar

    NetworkAgent *agent = wxGetApp().getAgent();
    if (agent) {
        if (!hide_some) {
            if (agent->is_user_login()) {
                show_status(PrintDialogStatus::PrintStatusInit);
            }
        }
    }

    if (m_print_type == PrintFromType::FROM_NORMAL) {
        reset_and_sync_ams_list();
        m_cur_input_thumbnail_data = m_specify_plate_idx == -1 ? m_plater->get_partplate_list().get_curr_plate()->thumbnail_data :
                                                                 m_plater->get_partplate_list().get_plate(m_specify_plate_idx)->thumbnail_data;
        set_default_normal(m_cur_input_thumbnail_data);
    }

    Layout();
    Fit();
}

void SyncAmsInfoDialog::reset_and_sync_ams_list()
{
    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__;
    // for black list
    std::vector<std::string> materials;
    std::vector<std::string> brands;
    std::vector<std::string> display_materials;
    std::vector<std::string> m_filaments_id;
    auto                     preset_bundle = wxGetApp().preset_bundle;

    for (auto filament_name : preset_bundle->filament_presets) {
        for (int f_index = 0; f_index < preset_bundle->filaments.size(); f_index++) {
            PresetCollection *filament_presets = &wxGetApp().preset_bundle->filaments;
            Preset *          preset           = &filament_presets->preset(f_index);
            int               size             = preset_bundle->filaments.size();
            if (preset && filament_name.compare(preset->name) == 0) {
                std::string display_filament_type;
                std::string filament_type = preset->config.get_filament_type(display_filament_type);
                std::string m_filament_id = preset->filament_id;
                display_materials.push_back(display_filament_type);
                materials.push_back(filament_type);
                m_filaments_id.push_back(m_filament_id);

                std::string m_vendor_name = "";
                auto        vendor        = dynamic_cast<ConfigOptionStrings *>(preset->config.option("filament_vendor"));
                if (vendor && (vendor->values.size() > 0)) {
                    std::string vendor_name = vendor->values[0];
                    m_vendor_name           = vendor_name;
                }
                brands.push_back(m_vendor_name);
            }
        }
    }
    /*std::vector<int> extruders;
    if (m_specify_plate_idx == -1) {
        extruders = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_used_extruders();
    } else {
        extruders = wxGetApp().plater()->get_partplate_list().get_plate(m_specify_plate_idx)->get_extruders();
    }*/
    std::vector<int> extruders(wxGetApp().plater()->get_extruders_colors().size());
    std::iota(extruders.begin(), extruders.end(), 1);
    BitmapCache            bmcache;
    MaterialHash::iterator iter = m_materialList.begin();
    while (iter != m_materialList.end()) {
        int       id   = iter->first;
        Material *item = iter->second;
        item->item->Destroy();
        delete item;
        iter++;
    }

    m_sizer_ams_mapping->Clear();
    m_materialList.clear();
    m_filaments.clear();

    bool use_double_extruder = get_is_double_extruder();
    if (use_double_extruder) {
        const auto &project_config = preset_bundle->project_config;
        m_filaments_map            = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_real_filament_maps(project_config);
    }
    auto contronal_index = 0;
    int  item_index      = 1;
    bool is_first_row    = true;
    for (auto i = 0; i < extruders.size(); i++) {
        auto          extruder = extruders[i] - 1;
        auto          colour   = wxGetApp().preset_bundle->project_config.opt_string("filament_colour", (unsigned int) extruder);
        unsigned char rgb[4];
        bmcache.parse_color4(colour, rgb);

        auto colour_rgb = wxColour((int) rgb[0], (int) rgb[1], (int) rgb[2], (int) rgb[3]);
        if (extruder >= materials.size() || extruder < 0 || extruder >= display_materials.size())
            continue;

        if (contronal_index % SYNC_FLEX_GRID_COL == 0) {
            wxBoxSizer *ams_tip_sizer = new wxBoxSizer(wxVERTICAL);
            if (is_first_row) {
                is_first_row              = false;
                if (!m_original_in_colormap) {
                    m_original_in_colormap = new wxStaticText(m_filament_panel, wxID_ANY, _CTX(L_CONTEXT("Original", "Sync_AMS"), "Sync_AMS") + ":");
                    m_original_in_colormap->SetForegroundColour(wxColour(107, 107, 107, 100));
                    m_original_in_colormap->SetFont(::Label::Head_12);
                }
                ams_tip_sizer->Add(m_original_in_colormap, 0, wxALIGN_LEFT | wxTOP, FromDIP(6));

                if (!m_ams_or_ext_text_in_colormap) {
                    m_ams_or_ext_text_in_colormap = new wxStaticText(m_filament_panel, wxID_ANY, _L("AMS") + ":");
                    m_ams_or_ext_text_in_colormap->SetForegroundColour(wxColour(107, 107, 107, 100));
                    m_ams_or_ext_text_in_colormap->SetFont(::Label::Head_12);
                }
                ams_tip_sizer->Add(m_ams_or_ext_text_in_colormap, 0, wxALIGN_LEFT | wxTOP, FromDIP(9));
            }
            m_sizer_ams_mapping->Add(ams_tip_sizer, 0, wxALIGN_LEFT | wxTOP, FromDIP(2));
            contronal_index++;
        }

        MaterialSyncItem *item = nullptr;
        if (use_double_extruder) {
            if (m_filaments_map[extruder] == 1) {
                item = new MaterialSyncItem(m_filament_panel, colour_rgb, _L(display_materials[extruder])); // m_filament_left_panel//special
                m_sizer_ams_mapping->Add(item, 0, wxALL, FromDIP(5));                                   // m_sizer_ams_mapping_left
            } else if (m_filaments_map[extruder] == 2) {
                item = new MaterialSyncItem(m_filament_panel, colour_rgb, _L(display_materials[extruder])); // m_filament_right_panel
                m_sizer_ams_mapping->Add(item, 0, wxALL, FromDIP(5));                                   // m_sizer_ams_mapping_right
            }
            else {
                BOOST_LOG_TRIVIAL(error) << "check error:MaterialItem *item = nullptr";
                continue;
            }
        } else {
            item = new MaterialSyncItem(m_filament_panel, colour_rgb, _L(display_materials[extruder]));
            m_sizer_ams_mapping->Add(item, 0, wxALL, FromDIP(5));
        }
        auto item_index_str = std::to_string(item_index);
        item->set_material_index_str(item_index_str);
        item_index++;

        contronal_index++;
        item->Bind(wxEVT_LEFT_UP, [this, item, materials, extruder](wxMouseEvent &e) {});
        item->Bind(wxEVT_LEFT_DOWN, [this, item, materials, extruder, item_index_str](wxMouseEvent &e) {
            MaterialHash::iterator iter = m_materialList.begin();
            while (iter != m_materialList.end()) {
                int           id   = iter->first;
                Material *    item = iter->second;
                auto          m    = item->item;
                m->on_normal();
                iter++;
            }

            m_current_filament_id = extruder;
            item->on_selected();

            auto    mouse_pos = ClientToScreen(e.GetPosition());
            wxPoint rect      = item->ClientToScreen(wxPoint(0, 0));

            // update ams data
            DeviceManager *dev_manager = Slic3r::GUI::wxGetApp().getDeviceManager();
            if (!dev_manager) return;
            MachineObject *obj_        = dev_manager->get_selected_machine();
            const auto &   full_config = wxGetApp().preset_bundle->full_config();
            size_t         nozzle_nums = full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
            if (nozzle_nums > 1) {
                m_mapping_popup.set_show_type(ShowType::LEFT_AND_RIGHT);//special
            }
            // m_mapping_popup.set_show_type(ShowType::RIGHT);
            if (obj_) {
                if (m_mapping_popup.IsShown())
                    return;
                wxPoint pos = item->ClientToScreen(wxPoint(0, 0));
                pos.y += item->GetRect().height;
                m_mapping_popup.Move(pos);

                if (obj_ && m_checkbox_list["use_ams"]->getValue() == "on") {
                    m_mapping_popup.set_parent_item(item);
                    m_mapping_popup.set_current_filament_id(extruder);
                    m_mapping_popup.set_material_index_str(item_index_str);
                    m_mapping_popup.show_reset_button();
                    auto reset_call_back = [this](const std::string &item_index_str) {
                        reset_one_ams_material(item_index_str);
                        update_final_thumbnail_data();
                        m_mapping_popup.update_items_check_state(m_ams_mapping_result);
                        m_mapping_popup.Refresh();
                        if (!m_reset_all_btn->IsShown()) {
                            m_reset_all_btn->Show();
                        }
                    };
                    m_mapping_popup.set_reset_callback(reset_call_back);
                    m_mapping_popup.set_tag_texture(materials[extruder]);
                    m_mapping_popup.set_send_win(this);
                    m_mapping_popup.update(obj_, m_ams_mapping_result);
                    m_mapping_popup.Popup();
                }
            }
        });

        Material *material_item = new Material();
        material_item->id       = extruder;
        material_item->item     = item;
        m_materialList[i]       = material_item;

        // build for ams mapping
        if (extruder < materials.size() && extruder >= 0) {
            FilamentInfo info;
            info.id          = extruder;
            info.type        = materials[extruder];
            info.brand       = brands[extruder];
            info.filament_id = m_filaments_id[extruder];
            info.color       = wxString::Format("#%02X%02X%02X%02X", colour_rgb.Red(), colour_rgb.Green(), colour_rgb.Blue(), colour_rgb.Alpha()).ToStdString();
            m_filaments.push_back(info);
        }
    }
    if (use_double_extruder) {
        //m_filament_left_panel->Show();//SyncAmsInfoDialog::reset_and_sync_ams_list()
        //m_filament_right_panel->Show();
        m_filament_panel->Show(); // SyncAmsInfoDialog::reset_and_sync_ams_list()//special
        m_sizer_ams_mapping_left->SetCols(4);
        m_sizer_ams_mapping_left->Layout();
        m_filament_panel_left_sizer->Layout();
        m_filament_left_panel->Layout();

        m_sizer_ams_mapping_right->SetCols(4);
        m_sizer_ams_mapping_right->Layout();
        m_filament_panel_right_sizer->Layout();
        m_filament_right_panel->Layout();
    } else {
        //m_filament_left_panel->Hide();//SyncAmsInfoDialog::reset_and_sync_ams_list()
        //m_filament_right_panel->Hide();
        m_filament_panel->Show();//SyncAmsInfoDialog::reset_and_sync_ams_list()
        m_sizer_ams_mapping->SetCols(SYNC_FLEX_GRID_COL);
        m_sizer_ams_mapping->Layout();
        m_filament_panel_sizer->Layout();
    }
    // reset_ams_material();//show "-"
}

void SyncAmsInfoDialog::generate_override_fix_ams_list()
{
    if (m_generate_fix_sizer_ams_mapping) {
        return;
    }
    m_generate_fix_sizer_ams_mapping = true;
    // for black list
    std::vector<std::string> materials;
    std::vector<std::string> brands;
    std::vector<std::string> display_materials;
    std::vector<std::string> m_filaments_id;
    auto                     preset_bundle = wxGetApp().preset_bundle;

    for (auto filament_name : preset_bundle->filament_presets) {
        for (int f_index = 0; f_index < preset_bundle->filaments.size(); f_index++) {
            PresetCollection *filament_presets = &wxGetApp().preset_bundle->filaments;
            Preset *          preset           = &filament_presets->preset(f_index);
            int               size             = preset_bundle->filaments.size();
            if (preset && filament_name.compare(preset->name) == 0) {
                std::string display_filament_type;
                std::string filament_type = preset->config.get_filament_type(display_filament_type);
                std::string m_filament_id = preset->filament_id;
                display_materials.push_back(display_filament_type);
                materials.push_back(filament_type);
                m_filaments_id.push_back(m_filament_id);

                std::string m_vendor_name = "";
                auto        vendor        = dynamic_cast<ConfigOptionStrings *>(preset->config.option("filament_vendor"));
                if (vendor && (vendor->values.size() > 0)) {
                    std::string vendor_name = vendor->values[0];
                    m_vendor_name           = vendor_name;
                }
                brands.push_back(m_vendor_name);
            }
        }
    }
    if (m_ams_combo_info.empty()) {
        wxGetApp().preset_bundle->get_ams_cobox_infos(m_ams_combo_info);
    }
    std::vector<int> extruders(wxGetApp().plater()->get_extruders_colors().size());
    std::iota(extruders.begin(), extruders.end(), 1);
    BitmapCache            bmcache;
    MaterialHash::iterator iter = m_fix_materialList.begin();
    while (iter != m_fix_materialList.end()) {
        int       id   = iter->first;
        Material *item = iter->second;
        item->item->Destroy();
        delete item;
        iter++;
    }

    m_fix_sizer_ams_mapping->Clear();
    m_fix_materialList.clear();
    m_fix_filaments.clear();

    bool use_double_extruder = get_is_double_extruder();
    if (use_double_extruder) {
        const auto &project_config = preset_bundle->project_config;
        m_filaments_map            = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_real_filament_maps(project_config);
    }
    auto contronal_index = 0;
    int  item_index      = 1;
    bool is_first_row    = true;
    for (auto i = 0; i < extruders.size(); i++) {
        auto          extruder = extruders[i] - 1;
        auto          colour   = wxGetApp().preset_bundle->project_config.opt_string("filament_colour", (unsigned int) extruder);
        unsigned char rgb[4];
        bmcache.parse_color4(colour, rgb);

        auto colour_rgb = wxColour((int) rgb[0], (int) rgb[1], (int) rgb[2], (int) rgb[3]);
        if (extruder >= extruders.size() || extruder < 0 || extruder >= m_ams_combo_info.ams_filament_colors.size())
            continue;

        if (contronal_index % SYNC_FLEX_GRID_COL == 0) {
            wxBoxSizer *ams_tip_sizer = new wxBoxSizer(wxVERTICAL);
            if (is_first_row) {
                is_first_row   = false;
                if (!m_original_in_override) {
                    m_original_in_override = new wxStaticText(m_fix_filament_panel, wxID_ANY, _CTX(L_CONTEXT("Original", "Sync_AMS"), "Sync_AMS") + ":");
                    m_original_in_override->SetForegroundColour(wxColour(107, 107, 107, 100));
                    m_original_in_override->SetFont(::Label::Head_12);
                }
                ams_tip_sizer->Add(m_original_in_override, 0, wxALIGN_LEFT | wxTOP, FromDIP(6));

                if (!m_ams_or_ext_text_in_override) {
                    auto text = (m_only_exist_ext_spool_flag ? _L("Ext spool") : _L("AMS")) + ":";
                    m_ams_or_ext_text_in_override = new wxStaticText(m_fix_filament_panel, wxID_ANY, text);
                    m_ams_or_ext_text_in_override->SetForegroundColour(wxColour(107, 107, 107, 100));
                    m_ams_or_ext_text_in_override->SetFont(::Label::Head_12);
                }
                ams_tip_sizer->Add(m_ams_or_ext_text_in_override, 0, wxALIGN_LEFT | wxTOP, FromDIP(9));
            }
            m_fix_sizer_ams_mapping->Add(ams_tip_sizer, 0, wxALIGN_LEFT | wxTOP, FromDIP(2));
            contronal_index++;
        }

        MaterialSyncItem *item = nullptr;
        if (use_double_extruder) {
            if (m_filaments_map[extruder] == 1) {
                item = new MaterialSyncItem(m_fix_filament_panel, colour_rgb, _L(display_materials[extruder])); // m_filament_left_panel//special
                m_fix_sizer_ams_mapping->Add(item, 0, wxALL, FromDIP(5));                                       // m_sizer_ams_mapping_left
            } else if (m_filaments_map[extruder] == 2) {
                item = new MaterialSyncItem(m_fix_filament_panel, colour_rgb, _L(display_materials[extruder])); // m_filament_right_panel
                m_fix_sizer_ams_mapping->Add(item, 0, wxALL, FromDIP(5));                                       // m_sizer_ams_mapping_right
            } else {
                BOOST_LOG_TRIVIAL(error) << "check error:MaterialItem *item = nullptr";
                continue;
            }
        } else {
            item = new MaterialSyncItem(m_fix_filament_panel, colour_rgb, _L(display_materials[extruder]));
            m_fix_sizer_ams_mapping->Add(item, 0, wxALL, FromDIP(5));
        }
        item->set_material_index_str(std::to_string(item_index));
        item_index++;
        contronal_index++;
        item->allow_paint_dropdown(false);

        Material *material_item = new Material();
        material_item->id       = extruder;
        material_item->item     = item;
        m_fix_materialList[i]       = material_item;

        // build for ams mapping
        if (extruder < materials.size() && extruder >= 0) {
            FilamentInfo info;
            info.id          = extruder;
            info.type        = materials[extruder];
            info.brand       = brands[extruder];
            info.filament_id = m_filaments_id[extruder];
            info.color       = wxString::Format("#%02X%02X%02X%02X", colour_rgb.Red(), colour_rgb.Green(), colour_rgb.Blue(), colour_rgb.Alpha()).ToStdString();
            m_fix_filaments.push_back(info);
        }
    }
    {
        if (!m_ams_combo_info.empty()) {
            auto index = 0;
            for (auto it = m_fix_materialList.begin(); it != m_fix_materialList.end(); it++) {
                if (index >= m_ams_combo_info.ams_filament_colors.size() || index >= extruders.size()) {
                    break;
                }
                auto     ams_color = decode_ams_color(m_ams_combo_info.ams_filament_colors[index]);
                wxString ams_id    = m_ams_combo_info.ams_names[index];
                std::vector<wxColour> cols;
                for (auto col : m_ams_combo_info.ams_multi_color_filment[index]) {
                    cols.push_back(decode_ams_color(col));
                }
                it->second->item->set_ams_info(ams_color, ams_id, 0, cols);//generate_override_fix_ams_list
                index++;
            }
        }
    }
    m_fix_filament_panel->Show(); // SyncAmsInfoDialog::reset_and_sync_ams_list()
    m_fix_sizer_ams_mapping->SetCols(SYNC_FLEX_GRID_COL);
    m_fix_sizer_ams_mapping->Layout();
    m_fix_filament_panel_sizer->Layout();
}

void SyncAmsInfoDialog::clone_thumbnail_data()
{
    // record preview_colors
    if (m_preview_colors_in_thumbnail.empty()) {
        MaterialHash::iterator iter = m_materialList.begin();
        if (m_preview_colors_in_thumbnail.size() != m_materialList.size()) {
            m_preview_colors_in_thumbnail.resize(m_materialList.size());
        }
        while (iter != m_materialList.end()) {
            int       id                      = iter->first;
            Material *item                    = iter->second;
            if (item) {
                auto m = item->item;
                if (m) {
                    // exist empty or unrecognized type ams in machine
                    if (item->id >= m_cur_colors_in_thumbnail.size()) {
                        m_cur_colors_in_thumbnail.resize(item->id + 1);
                    }
                    if (item->id >= m_preview_colors_in_thumbnail.size()) {
                        m_preview_colors_in_thumbnail.resize(item->id + 1);
                    }

                    if (m->m_ams_name == "-") {
                        m_cur_colors_in_thumbnail[item->id] = m->m_material_coloul;
                    } else {
                        m_cur_colors_in_thumbnail[item->id] = m->m_ams_coloul;
                    }
                    m_preview_colors_in_thumbnail[item->id] = m->m_material_coloul;
                }
            }
            else {
                BOOST_LOG_TRIVIAL(error) << "check error:SyncAmsInfoDialog::clone_thumbnail_data:item is nullptr";
            }
            iter++;
        }
    }
    // copy data
    auto &data = m_cur_input_thumbnail_data;
    m_preview_thumbnail_data.reset();
    m_preview_thumbnail_data.set(data.width, data.height);
    if (data.width > 0 && data.height > 0) {
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char *origin_px = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                unsigned char *new_px    = (unsigned char *) m_preview_thumbnail_data.pixels.data() + 4 * (rr + c);
                for (size_t i = 0; i < 4; i++) { new_px[i] = origin_px[i]; }
            }
        }
    }
    // record_edge_pixels_data
    record_edge_pixels_data();
}

void SyncAmsInfoDialog::record_edge_pixels_data()
{
    auto is_not_in_preview_colors = [this](unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
        for (size_t i = 0; i < m_preview_colors_in_thumbnail.size(); i++) {
            wxColour render_color = adjust_color_for_render(m_preview_colors_in_thumbnail[i]);
            if (render_color.Red() == r && render_color.Green() == g && render_color.Blue() == b /*&& render_color.Alpha() == a*/) { return false; }
        }
        return true;
    };
    ThumbnailData &data        = m_cur_no_light_thumbnail_data;
    ThumbnailData &origin_data = m_cur_input_thumbnail_data;
    if (data.width > 0 && data.height > 0) {
        m_edge_pixels.resize(data.width * data.height);
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char *no_light_px        = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                unsigned char *origin_px          = (unsigned char *) origin_data.pixels.data() + 4 * (rr + c);
                m_edge_pixels[r * data.width + c] = false;
                if (origin_px[3] > 0) {
                    if (is_not_in_preview_colors(no_light_px[0], no_light_px[1], no_light_px[2], origin_px[3])) { m_edge_pixels[r * data.width + c] = true; }
                }
            }
        }
    }
}

wxColour SyncAmsInfoDialog::adjust_color_for_render(const wxColour &color)
{
    Slic3r::ColorRGBA    _temp_color_color  = {color.Red() / 255.0f, color.Green() / 255.0f, color.Blue() / 255.0f, color.Alpha() / 255.0f};
    auto                 _temp_color_color_ = adjust_color_for_rendering(_temp_color_color);
    wxColour             render_color((int) (_temp_color_color_[0] * 255.0f), (int) (_temp_color_color_[1] * 255.0f), (int) (_temp_color_color_[2] * 255.0f),
                          (int) (_temp_color_color_[3] * 255.0f));
    return render_color;
}

void SyncAmsInfoDialog::final_deal_edge_pixels_data(ThumbnailData &data)
{
    if (data.width > 0 && data.height > 0 && m_edge_pixels.size() > 0) {
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr            = (data.height - 1 - r) * data.width;
            bool         exist_rr_up   = r >= 1 ? true : false;
            bool         exist_rr_down = r <= data.height - 2 ? true : false;
            unsigned int rr_up         = exist_rr_up ? (data.height - 1 - (r - 1)) * data.width : 0;
            unsigned int rr_down       = exist_rr_down ? (data.height - 1 - (r + 1)) * data.width : 0;
            for (unsigned int c = 0; c < data.width; ++c) {
                bool           exist_c_left      = c >= 1 ? true : false;
                bool           exist_c_right     = c <= data.width - 2 ? true : false;
                unsigned int   c_left            = exist_c_left ? c - 1 : 0;
                unsigned int   c_right           = exist_c_right ? c + 1 : 0;
                unsigned char *cur_px            = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                unsigned char *relational_pxs[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
                if (exist_rr_up && exist_c_left) { relational_pxs[0] = (unsigned char *) data.pixels.data() + 4 * (rr_up + c_left); }
                if (exist_rr_up) { relational_pxs[1] = (unsigned char *) data.pixels.data() + 4 * (rr_up + c); }
                if (exist_rr_up && exist_c_right) { relational_pxs[2] = (unsigned char *) data.pixels.data() + 4 * (rr_up + c_right); }
                if (exist_c_left) { relational_pxs[3] = (unsigned char *) data.pixels.data() + 4 * (rr + c_left); }
                if (exist_c_right) { relational_pxs[4] = (unsigned char *) data.pixels.data() + 4 * (rr + c_right); }
                if (exist_rr_down && exist_c_left) { relational_pxs[5] = (unsigned char *) data.pixels.data() + 4 * (rr_down + c_left); }
                if (exist_rr_down) { relational_pxs[6] = (unsigned char *) data.pixels.data() + 4 * (rr_down + c); }
                if (exist_rr_down && exist_c_right) { relational_pxs[7] = (unsigned char *) data.pixels.data() + 4 * (rr_down + c_right); }
                if (cur_px[3] > 0 && m_edge_pixels[r * data.width + c]) {
                    int rgba_sum[4] = {0, 0, 0, 0};
                    int valid_count = 0;
                    for (size_t k = 0; k < 8; k++) {
                        if (relational_pxs[k]) {
                            if (k == 0 && m_edge_pixels[(r - 1) * data.width + c_left]) { continue; }
                            if (k == 1 && m_edge_pixels[(r - 1) * data.width + c]) { continue; }
                            if (k == 2 && m_edge_pixels[(r - 1) * data.width + c_right]) { continue; }
                            if (k == 3 && m_edge_pixels[r * data.width + c_left]) { continue; }
                            if (k == 4 && m_edge_pixels[r * data.width + c_right]) { continue; }
                            if (k == 5 && m_edge_pixels[(r + 1) * data.width + c_left]) { continue; }
                            if (k == 6 && m_edge_pixels[(r + 1) * data.width + c]) { continue; }
                            if (k == 7 && m_edge_pixels[(r + 1) * data.width + c_right]) { continue; }
                            for (size_t m = 0; m < 4; m++) { rgba_sum[m] += relational_pxs[k][m]; }
                            valid_count++;
                        }
                    }
                    if (valid_count > 0) {
                        for (size_t m = 0; m < 4; m++) { cur_px[m] = std::clamp(int(rgba_sum[m] / (float) valid_count), 0, 255); }
                    }
                }
            }
        }
    }
}

void SyncAmsInfoDialog::updata_thumbnail_data_after_connected_printer()
{
    updata_ui_data_after_connected_printer();
    update_thumbnail_data_accord_plate_index(true);
}

void SyncAmsInfoDialog::show_advanced_settings(bool flag, bool update_layout)
{
    show_sizer(m_advace_setting_sizer, flag);
    if (!flag) {
        show_sizer(m_append_color_sizer, false);
        show_sizer(m_merge_color_sizer, false);
    } else {
        update_more_setting(update_layout);
    }
}

void SyncAmsInfoDialog::show_ams_controls(bool flag)
{
    m_filament_panel->Show(flag);
    if (flag) { m_filament_panel->Fit(); }
    m_attention_text->Show(flag);
    m_tip_text->Show(flag);
}

void SyncAmsInfoDialog::update_thumbnail_data_accord_plate_index(bool allow_clone_ams_color)
{
    if (m_map_mode == MapModeEnum::Override) {
        show_advanced_settings(false,false);
        m_filament_panel->Show(false);
        m_fix_filament_panel->Show();
        generate_override_fix_ams_list();
    } else if (m_map_mode == MapModeEnum::ColorMap) {
        show_advanced_settings(true, false);
        m_filament_panel->Show();
        m_fix_filament_panel->Show(false);
    }
    // change thumbnail_data
    ThumbnailData &input_data    = m_specify_plate_idx == -1 ? m_plater->get_partplate_list().get_curr_plate()->thumbnail_data :
                                                               m_plater->get_partplate_list().get_plate(m_specify_plate_idx)->thumbnail_data;
    ThumbnailData &no_light_data = m_specify_plate_idx == -1 ? m_plater->get_partplate_list().get_curr_plate()->no_light_thumbnail_data :
                                                               m_plater->get_partplate_list().get_plate(m_specify_plate_idx)->no_light_thumbnail_data;
    if (input_data.width == 0 || input_data.height == 0 || no_light_data.width == 0 || no_light_data.height == 0) { wxGetApp().plater()->update_all_plate_thumbnails(false); }
    unify_deal_thumbnail_data(input_data, no_light_data, allow_clone_ams_color);
}

void SyncAmsInfoDialog::update_final_thumbnail_data() {
    m_preview_colors_in_thumbnail.clear();//to update m_cur_colors_in_thumbnail
    unify_deal_thumbnail_data(m_cur_input_thumbnail_data, m_cur_no_light_thumbnail_data,false);
}

void SyncAmsInfoDialog::unify_deal_thumbnail_data(ThumbnailData &input_data, ThumbnailData &no_light_data, bool allow_clone_ams_color)
{
    if (input_data.width == 0 || input_data.height == 0 || no_light_data.width == 0 || no_light_data.height == 0) {
        BOOST_LOG_TRIVIAL(error) << "SyncAmsInfoDialog::no_light_data is empty,error";
        return;
    }
    m_cur_input_thumbnail_data    = input_data;
    m_cur_no_light_thumbnail_data = no_light_data;
    clone_thumbnail_data();
    if (m_cur_colors_in_thumbnail.size() > 0) {
        change_default_normal(-1, wxColour());
        final_deal_edge_pixels_data(m_preview_thumbnail_data);
        set_default_normal(m_preview_thumbnail_data);
    }
    else {
        set_default_normal(input_data);
    }
}

void SyncAmsInfoDialog::change_default_normal(int old_filament_id, wxColour temp_ams_color)
{
    if (m_cur_colors_in_thumbnail.size() == 0) {
        BOOST_LOG_TRIVIAL(error) << "SyncAmsInfoDialog::change_default_normal:error:m_cur_colors_in_thumbnail.size() == 0";
        return;
    }
    if (old_filament_id >= 0) {
        if (old_filament_id < m_cur_colors_in_thumbnail.size()) {
            m_cur_colors_in_thumbnail[old_filament_id] = temp_ams_color;
        } else {
            BOOST_LOG_TRIVIAL(error) << "SyncAmsInfoDialog::change_default_normal:error:old_filament_id > m_cur_colors_in_thumbnail.size()";
            return;
        }
    }
    ThumbnailData &data          = m_cur_input_thumbnail_data;
    ThumbnailData &no_light_data = m_cur_no_light_thumbnail_data;
    if (data.width > 0 && data.height > 0 && data.width == no_light_data.width && data.height == no_light_data.height) {
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char *no_light_px = (unsigned char *) no_light_data.pixels.data() + 4 * (rr + c);
                unsigned char *origin_px   = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                unsigned char *new_px      = (unsigned char *) m_preview_thumbnail_data.pixels.data() + 4 * (rr + c);
                if (origin_px[3] > 0 && m_edge_pixels[r * data.width + c] == false) {
                    auto filament_id = 255 - no_light_px[3];
                    if (filament_id >= m_cur_colors_in_thumbnail.size()) { continue; }
                    wxColour temp_ams_color_in_loop = m_cur_colors_in_thumbnail[filament_id];
                    wxColour ams_color              = adjust_color_for_render(temp_ams_color_in_loop);
                    // change color
                    new_px[3] = ams_color.Alpha(); //origin_px[3]; // alpha
                    int           origin_rgb      = origin_px[0] + origin_px[1] + origin_px[2];
                    int           no_light_px_rgb = no_light_px[0] + no_light_px[1] + no_light_px[2];
                    unsigned char i               = 0;
                    if (origin_rgb >= no_light_px_rgb) { // Brighten up
                        unsigned char cur_single_color = ams_color.Red();
                        new_px[i]                      = std::clamp(cur_single_color + (origin_px[i] - no_light_px[i]), 0, 255);
                        i++;
                        cur_single_color = ams_color.Green();
                        new_px[i]        = std::clamp(cur_single_color + (origin_px[i] - no_light_px[i]), 0, 255);
                        i++;
                        cur_single_color = ams_color.Blue();
                        new_px[i]        = std::clamp(cur_single_color + (origin_px[i] - no_light_px[i]), 0, 255);
                    } else { // Dimming
                        float         ratio            = origin_rgb / (float) no_light_px_rgb;
                        unsigned char cur_single_color = ams_color.Red();
                        new_px[i]                      = std::clamp((int) (cur_single_color * ratio), 0, 255);
                        i++;
                        cur_single_color = ams_color.Green();
                        new_px[i]        = std::clamp((int) (cur_single_color * ratio), 0, 255);
                        i++;
                        cur_single_color = ams_color.Blue();
                        new_px[i]        = std::clamp((int) (cur_single_color * ratio), 0, 255);
                    }
                }
            }
        }
    } else {
        BOOST_LOG_TRIVIAL(error) << "SyncAmsInfoDialog::change_defa:no_light_data is empty,error";
    }
}

SyncAmsInfoDialog::~SyncAmsInfoDialog() {
    if (m_refresh_timer) {
        delete m_refresh_timer;
    }
    BOOST_LOG_TRIVIAL(error) << "~SyncAmsInfoDialog destruction";
}

void SyncAmsInfoDialog::set_info(SyncInfo &info)
{
    m_input_info = info;
}

void SyncAmsInfoDialog::update_lan_machine_list()
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    if (!dev) return;
    auto m_free_machine_list = dev->get_local_machinelist();

    BOOST_LOG_TRIVIAL(info) << "SelectMachinePopup update_other_devices start";

    for (auto &elem : m_free_machine_list) {
        MachineObject *mobj = elem.second;

        /* do not show printer bind state is empty */
        if (!mobj->is_avaliable()) continue;
        if (!mobj->is_online()) continue;
        if (!mobj->is_lan_mode_printer()) continue;

        if (mobj->has_access_right()) {
            auto b = mobj->get_dev_name();

            // clear machine list

            // m_comboBox_printer->Clear();
            std::vector<std::string>               machine_list;
            wxArrayString                          machine_list_name;
            std::map<std::string, MachineObject *> option_list;
        }
    }
    BOOST_LOG_TRIVIAL(info) << "SyncAmsInfoDialog update_lan_devices end";
}

SyncNozzleAndAmsDialog::SyncNozzleAndAmsDialog(InputInfo &input_info)
    : BaseTransparentDPIFrame(static_cast<wxWindow *>(wxGetApp().mainframe),
                              wxGetApp().preset_bundle->get_printer_extruder_count() == 1 ? 370 :320,
                              input_info.dialog_pos,
                              90,
                              wxGetApp().preset_bundle->get_printer_extruder_count() == 1 ? _L("Successfully synchronized nozzle information.") :
                                                                                            _L("Successfully synchronized nozzle and AMS number information."),
                              _L("Continue to sync filaments"),
                              _CTX(L_CONTEXT("Cancel", "Sync_Nozzle_AMS"), "Sync_Nozzle_AMS"),
                              DisappearanceMode::TimedDisappearance)
    , m_input_info(input_info)
{
   /* set_target_pos_and_gradual_disappearance(input_info.ams_btn_pos);
    m_move_to_target_gradual_disappearance = false;*/
}

SyncNozzleAndAmsDialog::~SyncNozzleAndAmsDialog() {}

void SyncNozzleAndAmsDialog::deal_ok() {
    on_hide();
    wxGetApp().plater()->sidebar().sync_ams_list(true);
}

void SyncNozzleAndAmsDialog::deal_cancel()
{
    on_hide();
}

static inline void UpdatePositionAlignment(wxWindow* w, wxPoint base_position, bool align_right) {
    if (!align_right) {
        base_position.x -= w->GetSize().x;
    }
    w->SetPosition(base_position);
}

void SyncNozzleAndAmsDialog::update_info(InputInfo &info) {
    m_input_info = info;
    restart();
    UpdatePositionAlignment(this, m_input_info.dialog_pos, m_input_info.dialog_pos_align_right);
}

bool SyncNozzleAndAmsDialog::Layout()
{
    BaseTransparentDPIFrame::Layout();
    UpdatePositionAlignment(this, m_input_info.dialog_pos, m_input_info.dialog_pos_align_right);
    return true;
}

FinishSyncAmsDialog::FinishSyncAmsDialog(InputInfo &input_info)
    : BaseTransparentDPIFrame(static_cast<wxWindow *>(wxGetApp().mainframe),
                              310,
                              input_info.dialog_pos,
                              68,
                              _L("Successfully synchronized color and type of filament from printer."),
                              _CTX(L_CONTEXT("OK", "FinishSyncAms"), "FinishSyncAms"),
                              "",
                              DisappearanceMode::TimedDisappearance)
    , m_input_info(input_info)
{
    m_button_cancel->Hide();
    //set_target_pos_and_gradual_disappearance(input_info.ams_btn_pos);
}

FinishSyncAmsDialog::~FinishSyncAmsDialog() {}

void FinishSyncAmsDialog::deal_ok() {
    on_hide();
}

void FinishSyncAmsDialog::update_info(InputInfo &info)
{
    m_input_info = info;
    restart();
    UpdatePositionAlignment(this, m_input_info.dialog_pos, m_input_info.dialog_pos_align_right);
}

bool FinishSyncAmsDialog::Layout()
{
    BaseTransparentDPIFrame::Layout();
    UpdatePositionAlignment(this, m_input_info.dialog_pos, m_input_info.dialog_pos_align_right);
    return true;
}

}} // namespace Slic3r
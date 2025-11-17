#include "CalibrationWizardStartPage.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r { namespace GUI {

#define CALIBRATION_START_PAGE_TEXT_MAX_LENGTH FromDIP(1000)
CalibrationStartPage::CalibrationStartPage(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    :CalibrationWizardPage(parent, id, pos, size, style)
{
    m_top_sizer = new wxBoxSizer(wxVERTICAL);
}

void CalibrationStartPage::create_when(wxWindow* parent, wxString title, wxString content)
{
    m_when_title = new Label(this, title);
    m_when_title->SetFont(Label::Head_14);
    m_when_title->Wrap(CALIBRATION_START_PAGE_TEXT_MAX_LENGTH);
    m_when_title->SetMinSize({CALIBRATION_START_PAGE_TEXT_MAX_LENGTH, -1});

    m_when_content = new Label(this, content);;
    m_when_content->SetFont(Label::Body_14);
    m_when_content->Wrap(CALIBRATION_START_PAGE_TEXT_MAX_LENGTH);
    m_when_content->SetMinSize({CALIBRATION_START_PAGE_TEXT_MAX_LENGTH, -1});
}

void CalibrationStartPage::create_about(wxWindow* parent, wxString title, wxString content)
{
    m_about_title = new Label(this, title);
    m_about_title->SetFont(Label::Head_14);
    m_about_title->Wrap(CALIBRATION_START_PAGE_TEXT_MAX_LENGTH);
    m_about_title->SetMinSize({CALIBRATION_START_PAGE_TEXT_MAX_LENGTH, -1});

    m_about_content = new Label(this, content);
    m_about_content->SetFont(Label::Body_14);
    m_about_content->Wrap(CALIBRATION_START_PAGE_TEXT_MAX_LENGTH);
    m_about_content->SetMinSize({CALIBRATION_START_PAGE_TEXT_MAX_LENGTH, -1});
}

void CalibrationStartPage::create_bitmap(wxWindow* parent, const wxBitmap& before_img, const wxBitmap& after_img)
{
    if (!m_before_bmp)
        m_before_bmp = new wxStaticBitmap(parent, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, 0);
    m_before_bmp->SetBitmap(before_img);
    if (!m_after_bmp)
        m_after_bmp = new wxStaticBitmap(parent, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, 0);
    m_after_bmp->SetBitmap(after_img);
    if (!m_images_sizer) {
        m_images_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_images_sizer->Add(m_before_bmp, 0, wxALL, 0);
        m_images_sizer->AddSpacer(FromDIP(20));
        m_images_sizer->Add(m_after_bmp, 0, wxALL, 0);
    }
}

void CalibrationStartPage::create_bitmap(wxWindow* parent, std::string before_img, std::string after_img)
{
    wxBitmap before_bmp = create_scaled_bitmap(before_img, this, 350);
    wxBitmap after_bmp = create_scaled_bitmap(after_img, this, 350);

    create_bitmap(parent, before_bmp, after_bmp);
}

void CalibrationStartPage::create_bitmap(wxWindow* parent, std::string img) {
    wxBitmap before_bmp = create_scaled_bitmap(img, this, 350);
    if (!m_bmp_intro)
        m_bmp_intro = new wxStaticBitmap(parent, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, 0);
    m_bmp_intro->SetBitmap(before_bmp);
    if (!m_images_sizer) {
        m_images_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_images_sizer->Add(m_bmp_intro, 0, wxALL, 0);
    }
}

CalibrationPAStartPage::CalibrationPAStartPage(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationStartPage(parent, id, pos, size, style)
{
    m_cali_mode = CalibMode::Calib_PA_Line;
    m_page_type = CaliPageType::CALI_PAGE_START;

    create_page(this);

    this->SetSizer(m_top_sizer);
    Layout();
    m_top_sizer->Fit(this);
}

void CalibrationPAStartPage::create_page(wxWindow* parent)
{
    m_page_caption = new CaliPageCaption(parent, CalibMode::Calib_PA_Line);
    m_page_caption->show_prev_btn(false);
    m_top_sizer->Add(m_page_caption, 0, wxEXPAND, 0);
    create_when(parent,
        _L("When do you need Flow Dynamics Calibration"),
        _L("We now have added the auto-calibration for different filaments, which is fully automated and the result will be saved into the printer for future use. "
           "You only need to do the calibration in the following limited cases:\n"
           "1. If you introduce a new filament of different brands/models or the filament is damp;\n"
           "2. If the nozzle is worn out or replaced with a new one;\n"
           "3. If the max volumetric speed or print temperature is changed in the filament setting."));

    m_top_sizer->Add(m_when_title);
    m_top_sizer->Add(m_when_content);
    m_top_sizer->AddSpacer(PRESET_GAP);

    if (wxGetApp().app_config->get_language_code() == "zh-cn") { 
        create_bitmap(parent, "cali_page_before_pa_CN", "cali_page_after_pa_CN");
    } else {
        create_bitmap(parent, "cali_page_before_pa", "cali_page_after_pa");
    }
    m_top_sizer->Add(m_images_sizer, 0, wxALL, 0);
    m_top_sizer->AddSpacer(PRESET_GAP);

    m_help_panel = new PAPageHelpPanel(parent, false);
    m_top_sizer->Add(m_help_panel, 0, wxALL, 0);
    m_top_sizer->AddSpacer(PRESET_GAP);

    create_about(parent,
        _L("About this calibration"),
        _L("Please find the details of Flow Dynamics Calibration from our wiki.\
\n\nUsually the calibration is unnecessary. When you start a single color/material print, with the \"flow dynamics calibration\" option checked in the print start menu, the printer will follow the old way, calibrate the filament before the print; When you start a multi color/material print, the printer will use the default compensation parameter for the filament during every filament switch which will have a good result in most cases.\
\n\nPlease note that there are a few cases that can make the calibration results unreliable, such as insufficient adhesion on the build plate. Improving adhesion can be achieved by washing the build plate or applying glue. For more information on this topic, please refer to our Wiki.\
\n\nThe calibration results have about 10 percent jitter in our test, which may cause the result not exactly the same in each calibration. We are still investigating the root cause to do improvements with new updates."));
    m_top_sizer->Add(m_about_title);
    m_top_sizer->Add(m_about_content);
    m_top_sizer->AddSpacer(PRESET_GAP);

    m_action_panel = new CaliPageActionPanel(parent, CalibMode::Calib_PA_Line, CaliPageType::CALI_PAGE_START);

    m_top_sizer->Add(m_action_panel, 0, wxEXPAND, 0);

#ifdef __linux__
    wxGetApp().CallAfter([this]() {
        m_when_content->SetMinSize(m_when_content->GetSize() + wxSize{ 0, wxWindow::GetCharHeight() });
        m_about_content->SetMinSize(m_about_content->GetSize() + wxSize{ 0, wxWindow::GetCharHeight() });
        Layout();
        Fit();
        });
#endif
}

void CalibrationPAStartPage::on_reset_page()
{
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, false);
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, false);
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, false);
}

void CalibrationPAStartPage::on_device_connected(MachineObject* obj)
{
    // enable all button
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, true);
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, true);
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, true);

    if (obj->get_printer_series() == PrinterSeries::SERIES_X1) {
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, true);
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, true);
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, true);

        if (obj->cali_version <= -1) {
            m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, true);
            m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, true);
            m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, true);
        }
        else {
            m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, false);
            m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, false);
            m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, false);
        }


        if (!obj->is_support_pa_calibration) {
            m_action_panel->show_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, false);
        }
    }
    else if (obj->get_printer_series() == PrinterSeries::SERIES_P1P || obj->get_printer_arch() == PrinterArch::ARCH_I3) {
        if (obj->cali_version >= 0) {
            m_action_panel->show_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, true);
            m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, false);
        }
        else
            m_action_panel->show_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, false);
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, false);
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, true);
        m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, false);
    }
}

void CalibrationPAStartPage::msw_rescale()
{
    CalibrationWizardPage::msw_rescale();
    m_help_panel->msw_rescale();
    if (wxGetApp().app_config->get_language_code() == "zh-cn") {
        create_bitmap(this, "cali_page_before_pa_CN", "cali_page_after_pa_CN");
    } else {
        create_bitmap(this, "cali_page_before_pa", "cali_page_after_pa");
    }
}

CalibrationFlowRateStartPage::CalibrationFlowRateStartPage(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationStartPage(parent, id, pos, size, style)
{
    m_cali_mode = CalibMode::Calib_Flow_Rate;

    create_page(this);

    this->SetSizer(m_top_sizer);
    Layout();
    m_top_sizer->Fit(this);
}

void CalibrationFlowRateStartPage::create_page(wxWindow* parent)
{
    m_page_caption = new CaliPageCaption(parent, CalibMode::Calib_Flow_Rate);
    m_page_caption->show_prev_btn(false);
    m_top_sizer->Add(m_page_caption, 0, wxEXPAND, 0);
    create_when(parent,
        _L("When to use Flow Rate Calibration"),
        _L("After using Flow Dynamics Calibration, there might still be some extrusion issues, such as:\n"
           "1. Over-Extrusion: Excess material on your printed object, forming blobs or zits, or the layers seem thicker than expected and not uniform\n"
           "2. Under-Extrusion: Very thin layers, weak infill strength, or gaps in the top layer of the model, even when printing slowly\n"
           "3. Poor Surface Quality: The surface of your prints seems rough or uneven\n"
           "4. Weak Structural Integrity: Prints break easily or don't seem as sturdy as they should be"));

    m_top_sizer->Add(m_when_title);
    m_top_sizer->Add(m_when_content);
    m_top_sizer->AddSpacer(PRESET_GAP);

    if (wxGetApp().app_config->get_language_code() == "zh-cn") { 
        create_bitmap(parent, "cali_page_flow_introduction_CN");
    } else {
        create_bitmap(parent, "cali_page_flow_introduction");
    }
    m_top_sizer->Add(m_images_sizer, 0, wxALL, 0);
    m_top_sizer->AddSpacer(PRESET_GAP);

    auto extra_text = new Label(parent, _L("In addition, Flow Rate Calibration is crucial for foaming materials like LW-PLA used in RC planes. These materials expand greatly when heated, and calibration provides a useful reference flow rate."));
    extra_text->SetFont(Label::Body_14);
    extra_text->Wrap(CALIBRATION_START_PAGE_TEXT_MAX_LENGTH);
    extra_text->SetMinSize({CALIBRATION_START_PAGE_TEXT_MAX_LENGTH, -1});
    m_top_sizer->Add(extra_text);
    m_top_sizer->AddSpacer(PRESET_GAP);

    create_about(parent,
        _L("About this calibration"),
        _L("Flow Rate Calibration measures the ratio of expected to actual extrusion volumes. The default setting works well in Bambu Lab printers and official filaments as they were pre-calibrated and fine-tuned. For a regular filament, you usually won't need to perform a Flow Rate Calibration unless you still see the listed defects after you have done other calibrations. For more details, please check out the wiki article."));
        
    m_top_sizer->Add(m_about_title);
    m_top_sizer->Add(m_about_content);
    m_top_sizer->AddSpacer(PRESET_GAP);

    auto auto_cali_title = new Label(parent, _L("Auto-Calibration"));
    auto_cali_title->SetFont(Label::Head_14);
    auto_cali_title->Wrap(CALIBRATION_START_PAGE_TEXT_MAX_LENGTH);
    auto_cali_title->SetMinSize({CALIBRATION_START_PAGE_TEXT_MAX_LENGTH, -1});

    auto auto_cali_content = new Label(this, 
        _L("Auto Flow Rate Calibration utilizes Bambu Lab's Micro-Lidar technology, directly measuring the calibration patterns. However, please be advised that the efficacy and accuracy of this method may be compromised with specific types of materials. Particularly, filaments that are transparent or semi-transparent, sparkling-particled, or have a high-reflective finish may not be suitable for this calibration and can produce less-than-desirable results.\
\n\nThe calibration results may vary between each calibration or filament. We are still improving the accuracy and compatibility of this calibration through firmware updates over time.\
\n\nCaution: Flow Rate Calibration is an advanced process, to be attempted only by those who fully understand its purpose and implications. Incorrect usage can lead to sub-par prints or printer damage. Please make sure to carefully read and understand the process before doing it."));
    auto_cali_content->SetFont(Label::Body_14);
    auto_cali_content->Wrap(CALIBRATION_START_PAGE_TEXT_MAX_LENGTH);
    auto_cali_content->SetMinSize({CALIBRATION_START_PAGE_TEXT_MAX_LENGTH, -1});

    m_top_sizer->Add(auto_cali_title);
    m_top_sizer->Add(auto_cali_content);
    m_top_sizer->AddSpacer(PRESET_GAP);

    m_action_panel = new CaliPageActionPanel(parent, CalibMode::Calib_Flow_Rate, CaliPageType::CALI_PAGE_START);

    m_top_sizer->Add(m_action_panel, 0, wxEXPAND, 0);

#ifdef __linux__
    wxGetApp().CallAfter([this, auto_cali_content, extra_text]() {
        m_when_content->SetMinSize(m_when_content->GetSize() + wxSize{ 0, wxWindow::GetCharHeight() });
        auto_cali_content->SetMinSize(auto_cali_content->GetSize() + wxSize{ 0, wxWindow::GetCharHeight() });
        extra_text->SetMinSize(extra_text->GetSize() + wxSize{ 0, wxWindow::GetCharHeight() });
        Layout();
        Fit();
        });
#endif
}

void CalibrationFlowRateStartPage::on_reset_page()
{
    //disable all button
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, false);
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, false);
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, false);
}

void CalibrationFlowRateStartPage::on_device_connected(MachineObject* obj)
{
    //enable all button
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, true);
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, true);
    m_action_panel->enable_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, true);

    if (obj->get_printer_series() == PrinterSeries::SERIES_P1P) {
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, false);
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, false);
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, true);

        m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, false);
    }
    else if (obj->get_printer_series() == PrinterSeries::SERIES_X1) {
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_MANAGE_RESULT, false);
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, true);
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, true);

        if (obj->cali_version <= -1) {
            m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, true);
        }
        else {
            m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, false);
            m_action_panel->bind_button(CaliPageActionType::CALI_ACTION_MANUAL_CALI, false);
        }
    }

    //is support auto cali
    if (!obj->is_support_flow_calibration) {
        m_action_panel->show_button(CaliPageActionType::CALI_ACTION_AUTO_CALI, false);
    }
}

void CalibrationFlowRateStartPage::msw_rescale()
{
    CalibrationWizardPage::msw_rescale();
    if (wxGetApp().app_config->get_language_code() == "zh-cn") {
        create_bitmap(this, "cali_page_flow_introduction_CN");
    } else {
        create_bitmap(this, "cali_page_flow_introduction");
    }
}

CalibrationMaxVolumetricSpeedStartPage::CalibrationMaxVolumetricSpeedStartPage(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : CalibrationStartPage(parent, id, pos, size, style)
{
    m_cali_mode = CalibMode::Calib_Vol_speed_Tower;

    create_page(this);

    this->SetSizer(m_top_sizer);
    Layout();
    m_top_sizer->Fit(this);
}

void CalibrationMaxVolumetricSpeedStartPage::create_page(wxWindow* parent)
{
    m_page_caption = new CaliPageCaption(parent, m_cali_mode);
    m_page_caption->show_prev_btn(false);
    m_top_sizer->Add(m_page_caption, 0, wxEXPAND, 0);
    create_when(parent, _L("When you need Max Volumetric Speed Calibration"), _L("Over-extrusion or under extrusion"));

    m_top_sizer->Add(m_when_title);
    m_top_sizer->Add(m_when_content);
    m_top_sizer->AddSpacer(PRESET_GAP);

    auto recommend_title = new Label(parent, _L("Max Volumetric Speed calibration is recommended when you print with:"));
    recommend_title->SetFont(Label::Head_14);
    recommend_title->Wrap(CALIBRATION_START_PAGE_TEXT_MAX_LENGTH);
    m_top_sizer->Add(recommend_title);
    auto recommend_text1 = new Label(parent, _L("material with significant thermal shrinkage/expansion, such as..."));
    recommend_text1->Wrap(CALIBRATION_START_PAGE_TEXT_MAX_LENGTH);
    recommend_text1->SetFont(Label::Body_14);
    m_top_sizer->Add(recommend_text1);
    auto recommend_text2 = new Label(parent, _L("materials with inaccurate filament diameter"));
    recommend_text2->Wrap(CALIBRATION_START_PAGE_TEXT_MAX_LENGTH);
    recommend_text2->SetFont(Label::Body_14);
    m_top_sizer->Add(recommend_text2);

    m_top_sizer->AddSpacer(PRESET_GAP);

    if (wxGetApp().app_config->get_language_code() == "zh-cn") {
        create_bitmap(parent, "cali_page_before_pa_CN", "cali_page_after_pa_CN");
    } else {
        create_bitmap(parent, "cali_page_before_pa", "cali_page_after_pa");
    }

    m_top_sizer->Add(m_images_sizer, 0, wxALL, 0);

    m_top_sizer->AddSpacer(PRESET_GAP);

    m_action_panel = new CaliPageActionPanel(parent, m_cali_mode, CaliPageType::CALI_PAGE_START);

    m_top_sizer->Add(m_action_panel, 0, wxEXPAND, 0);
}

void CalibrationMaxVolumetricSpeedStartPage::msw_rescale()
{
    CalibrationWizardPage::msw_rescale();
    if (wxGetApp().app_config->get_language_code() == "zh-cn") {
        create_bitmap(this, "cali_page_before_pa_CN", "cali_page_after_pa_CN");
    } else {
        create_bitmap(this, "cali_page_before_pa", "cali_page_after_pa");
    }
}

}}
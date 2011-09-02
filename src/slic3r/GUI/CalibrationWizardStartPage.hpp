#ifndef slic3r_GUI_CalibrationWizardStartPage_hpp_
#define slic3r_GUI_CalibrationWizardStartPage_hpp_

#include "CalibrationWizardPage.hpp"

namespace Slic3r { namespace GUI {



class CalibrationStartPage : public CalibrationWizardPage
{
public:
    CalibrationStartPage(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

protected:
    CalibMode m_cali_mode;

    wxBoxSizer*   m_top_sizer{ nullptr };
    wxBoxSizer*   m_images_sizer{ nullptr };
    Label*        m_when_title{ nullptr };
    Label*        m_when_content{ nullptr };
    Label*        m_about_title{ nullptr };
    Label*        m_about_content{ nullptr };
    wxStaticBitmap* m_before_bmp{ nullptr };
    wxStaticBitmap* m_after_bmp{ nullptr };
    wxStaticBitmap* m_bmp_intro{ nullptr };
    PAPageHelpPanel* m_help_panel{ nullptr };

    void create_when(wxWindow* parent, wxString title, wxString content);
    void create_about(wxWindow* parent, wxString title, wxString content);
    void create_bitmap(wxWindow* parent, const wxBitmap& before_img, const wxBitmap& after_img);
    void create_bitmap(wxWindow* parent, std::string before_img, std::string after_img);
    void create_bitmap(wxWindow* parent, std::string img);
};

class CalibrationPAStartPage : public CalibrationStartPage
{
public:
    CalibrationPAStartPage(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    void create_page(wxWindow* parent);

    void on_reset_page();
    void on_device_connected(MachineObject* obj);
    void msw_rescale() override;
};

class CalibrationFlowRateStartPage : public CalibrationStartPage
{
public:
    CalibrationFlowRateStartPage(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    void create_page(wxWindow* parent);
    void on_reset_page();
    void on_device_connected(MachineObject* obj);
    void msw_rescale() override;
};

class CalibrationMaxVolumetricSpeedStartPage : public CalibrationStartPage
{
public:
    CalibrationMaxVolumetricSpeedStartPage(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    void create_page(wxWindow* parent);
    void msw_rescale() override;
};

}} // namespace Slic3r::GUI

#endif
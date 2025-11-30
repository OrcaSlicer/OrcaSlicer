#ifndef slic3r_ExtrusionCalibration_hpp_
#define slic3r_ExtrusionCalibration_hpp_

#include "libslic3r/Preset.hpp"
#include "wxExtensions.hpp"
#include "GUI_Utils.hpp"
#include "DeviceManager.hpp"
#include "wx/clrpicker.h"
#include "Widgets/RadioBox.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/TextInput.hpp"
#include "ParamsDialog.hpp"
#include "GUI_App.hpp"
#include "wx/hyperlink.h"

#define EXTRUSION_CALIBRATION_DEF_COLOUR    wxColour(255, 255, 255)
#define EXTRUSION_CALIBRATION_GREY900       wxColour(38, 46, 48)
#define EXTRUSION_CALIBRATION_GREY800       wxColour(50, 58, 61)
#define EXTRUSION_CALIBRATION_GREY700       wxColour(107, 107, 107)
#define EXTRUSION_CALIBRATION_GREY300       wxColour(238, 238, 238)
#define EXTRUSION_CALIBRATION_GREY200       wxColour(248, 248, 248)
#define EXTRUSION_CALIBRATION_BODY_WIDTH    FromDIP(380)
#define EXTRUSION_CALIBRATION_LABEL_WIDTH   FromDIP(80)
#define EXTRUSION_CALIBRATION_WIDGET_GAP    FromDIP(18)
#define EXTRUSION_CALIBRATION_DIALOG_SIZE   wxSize(FromDIP(400), -1)
//#define EXTRUSION_CALIBRATION_DIALOG_SIZE   wxSize(FromDIP(520), -1)
#define EXTRUSION_CALIBRATION_BED_COMBOX    wxSize(FromDIP(200), FromDIP(24))
#define EXTRUSION_CALIBRATION_BUTTON_SIZE   wxSize(FromDIP(72), FromDIP(24))
#define EXTRUSION_CALIBRATION_INPUT_SIZE    wxSize(FromDIP(100), FromDIP(24))
#define EXTRUSION_CALIBRATION_BMP_SIZE      wxSize(FromDIP(256), FromDIP(256))
#define EXTRUSION_CALIBRATION_BMP_TIP_BAR   wxSize(FromDIP(256), FromDIP(40))
#define EXTRUSION_CALIBRATION_BMP_BTN_SIZE  wxSize(FromDIP(16), FromDIP(16))



namespace Slic3r { namespace GUI {

class ExtrusionCalibration : public DPIDialog
{
public:
    ExtrusionCalibration(wxWindow *parent, wxWindowID id);
    ~ExtrusionCalibration();
    void create();

    void input_value_finish();
    void update();
    bool Show(bool show) override;
    void Popup();

	void post_select_event();
    void update_machine_obj(MachineObject* obj_) { obj = obj_; };

    // input is 1 or 2
    void set_step(int step_index);

    static bool check_k_n_validation(wxString k_text, wxString n_text);
    static bool check_k_validation(wxString k_text);

    MachineObject *obj { nullptr };
    int            ams_id { 0 };        /* 0 ~ 3 */
    int            tray_id { 0 };       /* 0 ~ 3 | 254 for virtual tray id*/

    std::string    ams_filament_id;
    std::string    m_filament_type;

    std::vector<Preset*> user_filaments;

protected:
    void init_bitmaps();
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void paint(wxPaintEvent&);
    void open_bitmap(wxMouseEvent& event);
    void on_select_filament(wxCommandEvent& evt);
    void on_select_bed_type(wxCommandEvent& evt);
    void on_select_nozzle_dia(wxCommandEvent& evt);
    void on_click_cali(wxCommandEvent& evt);
    void on_click_cancel(wxCommandEvent& evt);
    void on_click_save(wxCommandEvent& evt);
    void on_click_last(wxCommandEvent& evt);
    void on_click_next(wxCommandEvent& evt);

    void update_filament_info();
    void update_combobox_filaments();
    wxString get_bed_type_incompatible(bool incompatible);
    void show_info(bool show, bool is_error, wxString text);

    int get_bed_temp(DynamicPrintConfig* config);

protected:
    wxPanel*            m_step_1_panel;
    wxPanel*            m_step_2_panel;

    // title of select filament preset
    Label*       m_filament_preset_title;
    // select a filament preset
#ifdef __APPLE__
    wxComboBox* m_comboBox_filament;
#else
    ComboBox* m_comboBox_filament;
#endif

#ifdef __APPLE__
    wxComboBox* m_comboBox_bed_type;
#else
    ComboBox* m_comboBox_bed_type;
#endif

#ifdef __APPLE__
    wxComboBox* m_comboBox_nozzle_dia;
#else
    ComboBox* m_comboBox_nozzle_dia;
#endif

    TextInput*          m_nozzle_temp;
    TextInput*          m_bed_temp;
    TextInput*          m_max_flow_ratio;
    Button*             m_cali_cancel;
    Button*             m_button_cali;
    Button*             m_button_next_step;
    Label*              m_save_cali_result_title;
    wxStaticText*       m_fill_cali_params_tips;
    wxStaticText*       m_info_text;
    wxStaticText*       m_error_text;

    wxBitmap            m_calibration_tips_open_btn_bmp;
    wxBitmap            m_calibration_tips_bmp_zh;
    wxBitmap            m_calibration_tips_bmp_en;
    wxStaticBitmap*     m_calibration_tips_static_bmp;
    // save n and k result
    wxStaticText*       m_k_param;
    TextInput*          m_k_val;
    wxStaticText*       m_n_param;
    TextInput*          m_n_val;
    Button*             m_button_last_step;
    Button*             m_button_save_result;

    bool m_is_zh{ false };
};

}} // namespace Slic3r::GUI

#endif

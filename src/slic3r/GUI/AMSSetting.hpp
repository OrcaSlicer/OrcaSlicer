#ifndef slic3r_AMSSettingDialog_hpp_
#define slic3r_AMSSettingDialog_hpp_

#include "libslic3r/Preset.hpp"
#include "wxExtensions.hpp"
#include "GUI_Utils.hpp"
#include "DeviceManager.hpp"
#include "Widgets/RadioBox.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/CheckBox.hpp"

#include "slic3r/GUI/DeviceCore/DevFilaAmsSetting.h"

#define AMS_SETTING_DEF_COLOUR wxColour(255, 255, 255)
#define AMS_SETTING_GREY800 wxColour(50, 58, 61)
#define AMS_SETTING_GREY700 wxColour(107, 107, 107)
#define AMS_SETTING_GREY200 wxColour(248, 248, 248)
#define AMS_SETTING_BODY_WIDTH FromDIP(380)
#define AMS_SETTING_BUTTON_SIZE wxSize(FromDIP(150), FromDIP(24))
#define AMS_F1_SUPPORT_INSERTION_UPDATE_DEFAULT std::string("00.00.07.89")

class AnimaIcon;
class ComboBox;
namespace Slic3r { namespace GUI {

class AMSSettingTypePanel;
class AMSSetting : public DPIDialog
{
public:
    AMSSetting(wxWindow *parent, wxWindowID id, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize, long style = wxDEFAULT_DIALOG_STYLE);
    ~AMSSetting();

public:
    void UpdateByObj(MachineObject* obj);

protected:
    void create();

    void update_ams_img(MachineObject* obj);
    void update_starting_read_mode(bool selected);
    void update_remain_mode(bool selected);
    void update_switch_filament(bool selected);
    void update_insert_material_read_mode(MachineObject* obj);
    void update_insert_material_read_mode(bool selected, std::string version);
    void update_air_printing_detection(MachineObject* obj);

    void update_firmware_switching_status();

    // event handlers
    void on_insert_material_read(wxCommandEvent& event);
    void on_starting_read(wxCommandEvent& event);
    void on_remain(wxCommandEvent& event);
    void on_switch_filament(wxCommandEvent& event);
    void on_air_print_detect(wxCommandEvent& event);
    void on_dpi_changed(const wxRect &suggested_rect) override;

protected:
    MachineObject *m_obj{nullptr};

    wxStaticText* m_static_ams_settings = nullptr;

    bool m_switching = false;
    AMSSettingTypePanel*  m_ams_type;
    //AMSSettingArrangeAMSOrder* m_ams_arrange_order;

    wxStaticBitmap* m_am_img;
    std::string     m_ams_img_name;

    wxPanel *     m_panel_body;
    wxPanel*      m_panel_Insert_material;
    CheckBox *    m_checkbox_Insert_material_auto_read;
    wxStaticText *m_title_Insert_material_auto_read;
    Label* m_tip_Insert_material_line1;
    Label* m_tip_Insert_material_line2;
    Label* m_tip_Insert_material_line3;

    CheckBox *    m_checkbox_starting_auto_read;
    wxStaticText *m_title_starting_auto_read;
    Label* m_tip_starting_line1;
    Label* m_tip_starting_line2;

    CheckBox *    m_checkbox_remain;
    wxStaticText *m_title_remain;
    Label* m_tip_remain_line1;

    CheckBox* m_checkbox_switch_filament;
    wxStaticText* m_title_switch_filament;
    Label* m_tip_switch_filament_line1;

    CheckBox* m_checkbox_air_print;
    wxStaticText* m_title_air_print;
    Label* m_tip_air_print_line;

    wxStaticText *m_tip_ams_img;
    Button *     m_button_auto_demarcate;

    wxBoxSizer *m_sizer_Insert_material_tip_inline;
    wxBoxSizer *m_sizer_starting_tip_inline;
    wxBoxSizer *m_sizer_remain_inline;
    wxBoxSizer *m_sizer_switch_filament_inline;
    wxBoxSizer *m_sizer_remain_block;
};

class AMSSettingTypePanel : public wxPanel
{
public:
    AMSSettingTypePanel(wxWindow* parent, AMSSetting* setting_dlg);
    ~AMSSettingTypePanel();

public:
    void Update(const MachineObject* obj);

private:
    void CreateGui();
    void OnAmsTypeChanged(wxCommandEvent& event);

private:
    std::weak_ptr<DevAmsSystemFirmwareSwitch> m_ams_firmware_switch;

    int m_ams_firmware_current_idx{ -1 };
    std::unordered_map<int, DevAmsSystemFirmwareSwitch::DevAmsSystemFirmware> m_ams_firmwares;

    // widgets
    AMSSetting*     m_setting_dlg;
    ComboBox*       m_type_combobox;
    Label*          m_switching_tips;
    AnimaIcon*      m_switching_icon;
};

#if 0
class AMSSettingArrangeAMSOrder : public wxPanel
{
public:
    AMSSettingArrangeAMSOrder(wxWindow* parent);

public:
    void Update(const MachineObject* obj);
    void Rescale() { m_btn_rearrange->msw_rescale(); Layout(); };

private:
    void CreateGui();
    void OnBtnRearrangeClicked(wxCommandEvent& event);

private:
    std::weak_ptr<DevAmsSystemFirmwareSwitch> m_ams_firmware_switch;
    ScalableButton* m_btn_rearrange;
};
#endif

}} // namespace Slic3r::GUI

#endif

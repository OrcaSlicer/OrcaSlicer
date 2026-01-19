#ifndef slic3r_GUI_AMXCONTROL_hpp_
#define slic3r_GUI_AMXCONTROL_hpp_

#include "../wxExtensions.hpp"
#include "StaticBox.hpp"
#include "StepCtrl.hpp"
#include "Button.hpp"
#include "AMSItem.hpp"
#include "../DeviceManager.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/AmsMappingPopup.hpp"
#include <wx/simplebook.h>
#include <wx/hyperlink.h>
#include <wx/animate.h>
#include <wx/dynarray.h>

#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"


namespace Slic3r { namespace GUI {

//Previous definitions
class uiAmsPercentHumidityDryPopup;

class AMSControl : public wxSimplebook
{
public:
    AMSControl(wxWindow *parent, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);
    ~AMSControl();

    void on_retry();

protected:
    std::string  m_current_ams;
    std::string  m_current_slot_left;
    std::string  m_current_slot_right;
    std::string  m_current_show_ams_left;
    std::string  m_current_show_ams_right;
    std::map<std::string, int> m_ams_selection;

    std::map<std::string, AMSPreview*> m_ams_preview_list;

    std::vector<AMSinfo>       m_ams_info;
    std::vector<AMSinfo>       m_ext_info;
    std::map<std::string, AmsItem*>  m_ams_item_list;
    std::map<std::string, AMSExtImage*> m_ext_image_list;

    std::string                      m_dev_id;
    std::vector<std::vector<std::string>> m_item_ids{ {}, {} };
    std::vector<std::pair<string, string>> pair_id;

    int         m_total_ext_count = 1;
    AMSextruder *m_extruder{nullptr};
    AMSRoadDownPart* m_down_road{ nullptr };

    /*items*/
    wxBoxSizer* m_sizer_ams_items{nullptr};
    wxScrolledWindow* m_panel_prv_left {nullptr};
    wxScrolledWindow* m_panel_prv_right{nullptr};
    wxBoxSizer* m_sizer_prv_left{nullptr};
    wxBoxSizer* m_sizer_prv_right{nullptr};

    /*ams */
    wxBoxSizer *m_sizer_ams_body{nullptr};
    wxBoxSizer* m_sizer_ams_area_left{nullptr};
    wxBoxSizer* m_sizer_ams_area_right{nullptr};
    wxBoxSizer* m_sizer_down_road{ nullptr };

    /*option*/
    wxBoxSizer *m_sizer_ams_option{nullptr};
    wxBoxSizer* m_sizer_option_left{nullptr};
    wxBoxSizer* m_sizer_option_mid{nullptr};
    wxBoxSizer* m_sizer_option_right{nullptr};


    AmsIntroducePopup m_ams_introduce_popup;

    //wxSimplebook *m_simplebook_right{nullptr};
    wxSimplebook *m_simplebook_ams_left{nullptr};
    wxSimplebook *m_simplebook_ams_right{ nullptr };
    wxSimplebook *m_simplebook_bottom{nullptr};
    wxPanel      *m_panel_down_road{ nullptr };
    int          m_left_page_index = 0;
    int          m_right_page_index = 0;


    wxStaticText *m_tip_right_top{nullptr};
    Label        *m_tip_load_info{nullptr};
    wxWindow *    m_amswin{nullptr};
    wxBoxSizer*   m_vams_sizer{nullptr};
    wxBoxSizer*   m_sizer_vams_tips{nullptr};

    Label*          m_ams_tip       {nullptr};

    Caninfo         m_vams_info;
    StaticBox*      m_panel_virtual {nullptr};
    AMSLib*         m_vams_lib      {nullptr};
    AMSRoad*        m_vams_road     {nullptr};


    wxBoxSizer *m_sizer_right_tip {nullptr};
    wxBoxSizer* m_sizer_ams_tips  {nullptr};

    ::StepIndicator *m_filament_load_step   {nullptr};
    ::StepIndicator *m_filament_unload_step {nullptr};
    ::StepIndicator *m_filament_vt_load_step {nullptr};

    Button *m_button_extruder_feed {nullptr};
    Button *m_button_extruder_back {nullptr};
    Button *m_button_auto_refill{ nullptr };
    wxStaticBitmap* m_button_ams_setting   {nullptr};
    wxStaticBitmap* m_img_ams_backup  {nullptr};
    wxStaticBitmap* m_img_amsmapping_tip {nullptr};
    wxStaticBitmap* m_img_vams_tip {nullptr};
    ScalableBitmap m_button_ams_setting_normal;
    ScalableBitmap m_button_ams_setting_hover;
    ScalableBitmap m_button_ams_setting_press;

    AmsHumidityTipPopup m_Humidity_tip_popup;
    uiAmsPercentHumidityDryPopup* m_percent_humidity_dry_popup;

    std::string m_last_ams_id = "";
    std::string m_last_tray_id = "";

public:
    std::string GetCurentAms();
    std::string GetCurentShowAms(AMSPanelPos pos = AMSPanelPos::RIGHT_PANEL);
    std::string GetCurrentCan(std::string amsid);
    bool        IsAmsInRightPanel(std::string ams_id);
	wxColour GetCanColour(std::string amsid, std::string canid);
    void createAms(wxSimplebook* parent, int& idx, AMSinfo info, AMSPanelPos pos);
    void createAmsPanel(wxSimplebook *parent, int &idx, std::vector<AMSinfo> infos, const std::string &series_name, const std::string &printer_type, AMSPanelPos pos, int total_ext_num);
    AMSRoadShowMode findFirstMode(AMSPanelPos pos);

    AMSModel m_ams_model{AMSModel::EXT_AMS};
    AMSModel m_ext_model{AMSModel::EXT_AMS};
    AMSModel m_is_none_ams_mode{AMSModel::EXT_AMS};
    bool     m_single_nozzle_no_ams = { true };

    void SetAmsModel(AMSModel mode, AMSModel ext_mode) {m_ams_model = mode; m_ext_model = ext_mode;};
    void AmsSelectedSwitch(wxCommandEvent& event);

    void EnableLoadFilamentBtn(bool enable, const std::string& ams_id, const std::string& can_id, const wxString& tips);
    void EnableUnLoadFilamentBtn(bool enable, const std::string& ams_id, const std::string& can_id,const wxString& tips);

    void EnterNoneAMSMode();
    void EnterGenericAMSMode();
    void EnterExtraAMSMode();

    void PlayRridLoading(wxString amsid, wxString canid);
    void StopRridLoading(wxString amsid, wxString canid);
    void ShowFilamentTip(bool hasams = true);

    void UpdatePassRoad(string ams_id, AMSPassRoadType type, AMSPassRoadSTEP step);
    void CreateAms();
    void CreateAmsDoubleNozzle(const std::string &series_name, const std::string& printer_type);
    void CreateAmsSingleNozzle(const std::string &series_name, const std::string &printer_type);
    void ClearAms();
    void UpdateAms(const std::string   &series_name,
                   const std::string   &printer_type,
                   std::vector<AMSinfo> ams_info,
                   std::vector<AMSinfo> ext_info,
                   DevExtderSystem           data,
                   std::string          dev_id,
                   bool                 is_reset = true,
                   bool                 test     = false);
    std::vector<AMSinfo> GenerateSimulateData();

    void AddAms(AMSinfo info, AMSPanelPos pos = AMSPanelPos::LEFT_PANEL);
    //void AddExtAms(int ams_id);
    void AddAmsPreview(AMSinfo info, AMSModel type);
    //void AddExtraAms(AMSinfo info);

    void AddAms(std::vector<AMSinfo> single_info, const std::string &series_name, const std::string &printer_type, AMSPanelPos pos = AMSPanelPos::LEFT_PANEL);
    void AddAmsPreview(std::vector<AMSinfo>single_info, AMSPanelPos pos);
    //void AddExtraAms(std::vector<AMSinfo>single_info);
    void SetExtruder(bool on_off, int nozzle_id, std::string ams_id, std::string slot_id);
    void SetAmsStep(std::string ams_id, std::string canid, AMSPassRoadType type, AMSPassRoadSTEP step);
    void SwitchAms(std::string ams_id);

    void msw_rescale();
    void on_filament_load(wxCommandEvent &event);
    void on_filament_unload(wxCommandEvent &event);
    void auto_refill(wxCommandEvent& event);
    void on_ams_setting_click(wxMouseEvent &event);
    void on_extrusion_cali(wxCommandEvent &event);
    void on_ams_setting_click(wxCommandEvent &event);
    void on_clibration_again_click(wxMouseEvent &event);
    void on_clibration_cancel_click(wxMouseEvent &event);
    void Reset();

    void show_noams_mode();
    void show_auto_refill(bool show);
    void enable_ams_setting(bool en);
    void show_vams_kn_value(bool show);
    void post_event(wxEvent&& event);

    virtual bool Enable(bool enable = true);
    void parse_object(MachineObject* obj);

private:
    std::string get_filament_id(const std::string& ams_id, const std::string& can_id);

public:
    std::string m_current_select;
};

}} // namespace Slic3r::GUI

#endif // !slic3r_GUI_amscontrol_hpp_

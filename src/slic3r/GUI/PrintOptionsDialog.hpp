#ifndef slic3r_GUI_PrintOptionsDialog_hpp_
#define slic3r_GUI_PrintOptionsDialog_hpp_

#include <wx/wx.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/dialog.h>

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "DeviceManager.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/ComboBox.hpp"

// Previous definitions
class SwitchBoard;

namespace Slic3r { namespace GUI {

class PrinterPartsDialog : public DPIDialog
{
protected:
    MachineObject* obj{ nullptr };

    ComboBox* nozzle_type_checkbox;
    ComboBox* nozzle_diameter_checkbox;

    Label*    nozzle_flow_type_label;
    ComboBox* nozzle_flow_type_checkbox;
    Label    *change_nozzle_tips;
    Label* m_wiki_link;
    Button* m_single_update_nozzle_button;
    Button* m_multiple_update_nozzle_button;

    ComboBox* multiple_left_nozzle_type_checkbox;
    ComboBox *multiple_left_nozzle_diameter_checkbox;
    ComboBox *multiple_left_nozzle_flow_checkbox;

    ComboBox *multiple_right_nozzle_type_checkbox;
    ComboBox *multiple_right_nozzle_diameter_checkbox;
    ComboBox *multiple_right_nozzle_flow_checkbox;

    Label *multiple_change_nozzle_tips;
    Label* multiple_wiki_link;

    wxPanel *single_panel;
    wxPanel *multiple_panel;

public:
    PrinterPartsDialog(wxWindow* parent);
    ~PrinterPartsDialog();

    void on_dpi_changed(const wxRect& suggested_rect) override;
    void update_machine_obj(MachineObject* obj_);
    bool Show(bool show) override;
    void UpdateNozzleInfo();

private:
    void  EnableEditing(bool enable);
    void  OnWikiClicked(wxMouseEvent& e);
    void  OnNozzleRefresh(wxCommandEvent& e);

    wxString GetString(NozzleType nozzle_type) const;
    wxString GetString(NozzleFlowType nozzle_flow_type) const;
    wxString GetString(float diameter) const { return wxString::FromDouble(diameter); };
};


class PrintOptionsDialog : public DPIDialog
{
protected:
    // settings
    wxScrolledWindow* m_scrollwindow;
    CheckBox* m_cb_first_layer;
    CheckBox *        m_cb_ai_monitoring;
    CheckBox* m_cb_spaghetti_detection;
    CheckBox* m_cb_purgechutepileup_detection;
    CheckBox* m_cb_nozzleclumping_detection;
    CheckBox* m_cb_airprinting_detection;
    CheckBox* m_cb_plate_mark;
    CheckBox* m_cb_auto_recovery;
    CheckBox* m_cb_save_remote_print_file_to_storage;
    CheckBox* m_cb_sup_sound;
    CheckBox* m_cb_filament_tangle;
    CheckBox* m_cb_nozzle_blob;
    CheckBox* m_cb_open_door;
    Label* text_first_layer;
    Label* text_ai_detections;
    Label* text_ai_detections_caption;
    wxPanel          *ai_refine_panel;
    wxSizerItem *ai_detections_bottom_space;
    wxSizerItem *ai_monitoring_bottom_space;
    wxSizerItem *spaghetti_bottom_space;
    wxSizerItem *purgechutepileup_bottom_space;
    wxSizerItem *nozzleclumping_bottom_space;
    wxSizerItem *airprinting_bottom_space;

    Label *           text_ai_monitoring;
    Label *           text_ai_monitoring_caption;
    Label* text_spaghetti_detection;
    Label* text_spaghetti_detection_caption0;
    Label* text_spaghetti_detection_caption1;
    Label*  text_purgechutepileup_detection;
    Label*  text_purgechutepileup_detection_caption0;
    Label* text_purgechutepileup_detection_caption1;

    Label* text_nozzleclumping_detection;
    Label* text_nozzleclumping_detection_caption0;
    Label* text_nozzleclumping_detection_caption1;

    Label *text_airprinting_detection;
    Label *text_airprinting_detection_caption0;
    Label *text_airprinting_detection_caption1;

    ComboBox *   ai_monitoring_level_list;
    ComboBox *spaghetti_detection_level_list;
    ComboBox* purgechutepileup_detection_level_list;
    ComboBox* nozzleclumping_detection_level_list;
    ComboBox* airprinting_detection_level_list;
    Label* text_plate_mark;
    Label* text_plate_mark_caption;
    Label* text_auto_recovery;
    Label* text_save_remote_print_file_to_storage;
    Label* text_save_remote_print_file_to_storage_explain;
    Label* text_sup_sound;
    Label* text_filament_tangle;
    Label* text_nozzle_blob;
    Label* text_nozzle_blob_caption;
    Label* text_open_door;
    StaticLine* line1;
    StaticLine* line2;
    StaticLine* line3;
    StaticLine* line4;
    StaticLine* line5;
    StaticLine* line6;
    StaticLine* line7;
    SwitchBoard* open_door_switch_board;
    wxBoxSizer* create_settings_group(wxWindow* parent);
    wxPanel     *m_line;


    bool print_halt = false;

public:
    PrintOptionsDialog(wxWindow* parent);
    ~PrintOptionsDialog();
    void on_dpi_changed(const wxRect &suggested_rect) override;

    void update_ai_monitor_status();
     //refine printer function options
    void update_spaghetti_detection_status();
    void update_purgechutepileup_detection_status();
    void update_nozzleclumping_detection_status();
    void update_airprinting_detection_status();

    MachineObject *obj { nullptr };

    std::vector<int> last_stage_list_info;
    int              m_state{0};
    void             update_options(MachineObject *obj_);
    void             update_machine_obj(MachineObject *obj_);
    bool             Show(bool show) override;

    enum AiMonitorSensitivityLevel {
        LOW         = 0,
        MEDIUM      = 1,
        HIGH        = 2,
        LEVELS_NUM  = 3
    };
    wxString sensitivity_level_to_label_string(enum AiMonitorSensitivityLevel level);
    std::string sensitivity_level_to_msg_string(enum AiMonitorSensitivityLevel level);

    void set_ai_monitor_sensitivity(wxCommandEvent &evt);
    void set_spaghetti_detection_sensitivity(wxCommandEvent& evt);
    void set_purgechutepileup_detection_sensitivity(wxCommandEvent &evt);
    void set_nozzleclumping_detection_sensitivity(wxCommandEvent &evt);
    void set_airprinting_detection_sensitivity(wxCommandEvent &evt);

private:
    void UpdateOptionSavePrintFileToStorage(MachineObject *obj);
    void UpdateOptionOpenDoorCheck(MachineObject *obj);
};

}} // namespace Slic3r::GUI

#endif

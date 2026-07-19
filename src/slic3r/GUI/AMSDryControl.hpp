#pragma once
#include "GUI_ObjectLayers.hpp"
#include "slic3r/GUI/Widgets/AMSItem.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/PopupWindow.hpp"

#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/I18N.hpp"

#include <chrono>
#include <optional>


//Previous defintions
class wxGrid;

namespace Slic3r {

namespace GUI {


enum class DryCtrState {
    IDLE,
    DRY_WHEN_PRINT,
    UNKNOWN
};

enum class DryCtrDev {
    N3S,
    N3F,
    UNKNOWN
};

struct DryingPreset {
    DryCtrState state;
    DryCtrDev dev;
    int dry_temp;
    int dry_time;
};

class FilamentItemPanel : public wxPanel
{
public:
    FilamentItemPanel(wxWindow* parent, const wxString& text, const std::string& icon_name = "",
                      wxWindowID id = wxID_ANY);

    void SetText(const wxString& text);
    void SetIcon(const std::string& icon_name);
    void msw_rescale();

private:
    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);

    Label* m_text_label;
    wxStaticBitmap* m_icon_bitmap;
    int m_target_size;
    std::string m_icon_name;
    ScalableBitmap m_icon;
};

class AMSFilamentPanel : public wxPanel
{
    wxBoxSizer* m_filament_sizer;
    Label* m_ams_name_label;
    int m_border_radius;
    wxPanel* m_filament_container{nullptr};
    std::vector<FilamentItemPanel*> m_filament_items;

public:
    AMSFilamentPanel(wxWindow* parent, const wxString& ams_name, wxWindowID id = wxID_ANY);

    void AddFilamentItem(const wxString& text, const std::string& icon_name);
    void AddFilamentItem(FilamentItemPanel* panel);
    void SetAmsName(const wxString& ams_name);
    void Clear();
    void msw_rescale();
private:
    void OnPaint(wxPaintEvent& event);
};


class AMSDryCtrWin : public DPIDialog
{
public:
    AMSDryCtrWin(wxWindow *parent);
    ~AMSDryCtrWin();

    void msw_rescale();
    void update(std::shared_ptr<DevFilaSystem> fila_system, MachineObject* obj);
    void set_ams_id(const std::string& ams_id);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    wxSimplebook* m_main_simplebook{nullptr};
    wxPanel* m_original_page{nullptr};

    wxWindow* m_amswin{nullptr};
    wxBoxSizer* m_sizer_ams_items{nullptr};
    wxScrolledWindow* m_panel_prv_left {nullptr};
    wxScrolledWindow* m_panel_prv_right{nullptr};
    wxBoxSizer* m_sizer_prv_left{nullptr};
    wxBoxSizer* m_sizer_prv_right{nullptr};

    // left panel related members
    ScalableBitmap m_humidity_image;
    wxStaticBitmap* m_humidity_img{nullptr};
    Label* m_image_description{nullptr};
    wxStaticBitmap* m_image_description_icon{nullptr};
    ScalableBitmap m_description_icon_bitmap;

    Label* m_humidity_data_label = nullptr;
    Label* m_temperature_data_label = nullptr;
    Label* m_time_data_label = nullptr;
    wxBoxSizer* m_time_descrition_container = nullptr;

    // right panel related members
    wxBoxSizer* m_normal_state_sizer{nullptr};
    wxBoxSizer* m_cannot_dry_sizer{nullptr};
    wxBoxSizer* m_dry_error_sizer{nullptr};
    Label* m_cannot_dry_description_label = nullptr;

    // right panel normal state
    ComboBox* m_trays_combo;
    std::vector<FilamentBaseInfo> m_tray_ids;
    wxTextCtrl* m_temperature_input;
    wxTextCtrl* m_time_input;
    Label* m_normal_description;
    Button* m_start_button{nullptr};
    Button* m_next_button{nullptr};
    Button* m_stop_button{nullptr};
    Button* m_back_button{nullptr};
    Button* m_unload_button{nullptr};

    // guide page description
    Label* m_guide_title_label{nullptr};
    Label* m_guide_description_label{nullptr};

    wxCheckBox* m_rotate_spool_toggle{nullptr};

    wxPanel* m_progress_page;
    ProgressBar* m_progress_gauge;
    wxTimer* m_progress_timer;

    std::optional<std::chrono::steady_clock::time_point> m_stop_button_restore_deadline;
    std::optional<std::chrono::steady_clock::time_point> m_unload_button_restore_deadline;

    Label* m_progress_title;
    int m_progress_value;
    int m_progress_message_index;
    std::vector<wxString> m_progress_text = {
        _L("Starting: Checking adapter connection"),
        _L("Starting: Checking filament status"),
        _L("Starting: Checking drying presets"),
        _L("Starting: Checking filament location"),
        _L("Starting: Checking air intake"),
        _L("Starting: Checking air vent")
    };

    // Guide page
    wxPanel* m_guide_page{nullptr};
    AMSFilamentPanel* m_ams_filament_panel{nullptr};
    std::map<std::string, FilamentItemPanel*> m_filament_items;
    wxStaticBitmap* m_image_placeholder{nullptr};
    ScalableBitmap m_guide_image;


    bool m_is_ams_changed = false;
    std::weak_ptr<DevFilaSystem> m_fila_system;
    struct {
        std::string m_ams_id;
        DevAmsType m_model = DevAmsType::EXT_SPOOL;
        DevAms::DryStatus m_dry_status = DevAms::DryStatus::Off;
        DevAms::DrySubStatus m_dry_sub_status = DevAms::DrySubStatus::Off;
        int m_humidity_percent;
        int m_temperature;
        int m_left_dry_time;
        float m_recommand_dry_temp;
    } m_ams_info;

    struct {
        std::unordered_map<std::string, std::string> m_filament_names;
        std::unordered_map<std::string, std::string> m_filament_type;
        std::unordered_map<std::string, int> m_dry_temp;
        std::unordered_map<std::string, int> m_dry_time;
    } m_dry_setting;

    struct {
        bool m_is_printing = false;
    } m_printer_status;

private:
    void create();
    wxBoxSizer* create_guide_page_sizer(wxPanel* parent);
    wxBoxSizer* create_main_content_section(wxPanel* parent);
    wxBoxSizer* create_guide_info_filament(wxPanel* parent);
    wxBoxSizer* create_guide_info_section(wxPanel* parent);
    wxBoxSizer* create_guide_right_section(wxPanel* parent);
    wxBoxSizer* create_main_page_sizer(wxPanel* parent);
    wxBoxSizer* create_left_panel(wxPanel* parent);
    wxBoxSizer* create_humidity_status_section(wxPanel* parent);
    wxBoxSizer* create_description_item(wxPanel* parent, const wxString& title, Label*& dataLabel);
    wxBoxSizer* create_status_descriptions_section(wxPanel* parent);

    wxBoxSizer* create_right_panel(wxPanel* parent);
    wxBoxSizer* create_normal_state_panel(wxPanel* parent);
    wxBoxSizer* create_cannot_dry_panel(wxPanel* parent);
    wxBoxSizer* create_drying_error_panel(wxPanel* parent);
    Button* create_button(wxPanel* parent, const wxString& title,
        const wxColour& background_color, const wxColour& border_color, const wxColour& text_color);

    wxBoxSizer* create_progress_page_sizer(wxPanel* parent);
    void OnProgressTimer(wxTimerEvent& event);
    void OnClose(wxCloseEvent& event);
    void OnShow(wxShowEvent& event);

    void OnFilamentSelectionChanged(wxCommandEvent& event);


    wxScrolledWindow* create_preview_scrolled_window(wxWindow* parent);

    bool check_values_changed(DevAms* dev_ams);
    int update_image(DevAmsType type, DevAms::DryStatus status, DevAms::DrySubStatus sub_status, int humidity_percent);
    void update_img_description(DevAms::DryStatus status, DevAms::DrySubStatus sub_status);
    void update_normal_description(DevAms* dev_ams);
    int update_state(DevAms* dev_ams);
    int update_dryness_status(DevAms* dev_ams);
    int update_ams_change(DevAms* dev_ams);
    int update_filament_list(DevAms* dev_ams, MachineObject* obj);
    void update_filament_guide_info(DevAms* dev_ams);
    void update_normal_state(DevAms* dev_ams);
    void update_printer_state(MachineObject* obj);

    std::shared_ptr<DevFilaSystem> get_fila_system() const;
    void start_sending_drying_command();
    void restore_stop_button_if_deadline_passed();
    void restore_unload_button_if_deadline_passed();
    void update_button_size(Button* button);

    bool is_dry_status_changed(DevAms* dev_ams);
    bool is_dry_ctr_idle(DevAms* dev_ams);
    bool is_ams_changed(DevAms* dev_ams);
    bool is_dry_ctr_idle();
    bool is_tray_changed(DevAms* dev_ams);
    bool is_dry_ctr_err(DevAms* dev_ams);

};

} // GUI
} // Slic3r

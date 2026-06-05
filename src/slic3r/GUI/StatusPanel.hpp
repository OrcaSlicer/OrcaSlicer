#ifndef slic3r_StatusPanel_hpp_
#define slic3r_StatusPanel_hpp_

#include "libslic3r/ProjectTask.hpp"
#include "DeviceManager.hpp"
#include "MonitorPage.hpp"
#include "SliceInfoPanel.hpp"
#include "CameraPopup.hpp"
#include "GUI.hpp"
#include "ThermalPreconditioningDialog.hpp"
#include <wx/panel.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/gbsizer.h>
#include <wx/webrequest.h>
#include "wxMediaCtrl2.h"
#include "MediaPlayCtrl.h"
#include "AMSSetting.hpp"
#include "Calibration.hpp"
#include "CalibrationWizardPage.hpp"
#include "PrintOptionsDialog.hpp"
#include "SafetyOptionsDialog.hpp"
#include "AMSMaterialsSetting.hpp"
#include "ExtrusionCalibration.hpp"
#include "ReleaseNote.hpp"
#include "Widgets/SwitchButton.hpp"
#include "Widgets/AxisCtrlButton.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/TempInput.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/ProgressBar.hpp"
#include "Widgets/ImageSwitchButton.hpp"
#include "Widgets/AMSControl.hpp"
#include "Widgets/FilamentLoad.hpp"
#include "Widgets/FanControl.hpp"
#include "HMS.hpp"
#include "PartSkipDialog.hpp"
#include "DeviceErrorDialog.hpp"

class StepIndicator;

#define COMMAND_TIMEOUT         5

namespace Slic3r {

class DevExtderSystem;

namespace GUI {

// Previous definitions
class MessageDialog;

enum CameraRecordingStatus {
    RECORDING_NONE,
    RECORDING_OFF_NORMAL,
    RECORDING_OFF_HOVER,
    RECORDING_ON_NORMAL,
    RECORDING_ON_HOVER,
};

enum CameraTimelapseStatus {
    TIMELAPSE_NONE,
    TIMELAPSE_OFF_NORMAL,
    TIMELAPSE_OFF_HOVER,
    TIMELAPSE_ON_NORMAL,
    TIMELAPSE_ON_HOVER,
};

enum PrintingTaskType {
    PRINGINT,
    CALIBRATION,
    NOT_CLEAR
};

enum ExtruderState {
    FILLED_LOAD,
    FILLED_UNLOAD,
    EMPTY_LOAD,
    EMPTY_UNLOAD
};

struct ScoreData
{
    int                                            rating_id;
    int                                            design_id;
    std::string                                    model_id;
    int                                            profile_id;
    int                                            star_count;
    bool                                           success_printed;
    wxString                                       comment_text;
    std::vector<std::string>                       image_url_paths;
    std::set<wxString>                             need_upload_images;
    std::vector<std::pair<wxString, std::string>>  local_to_url_image;
};

typedef std::function<void(BBLModelTask* subtask)> OnGetSubTaskFn;

class ExtruderImage : public wxWindow
{
    ScalableBitmap *m_pipe_filled_load;
    ScalableBitmap *m_pipe_filled_unload;
    ScalableBitmap *m_pipe_empty_load;
    ScalableBitmap *m_pipe_empty_unload;

    ScalableBitmap *m_pipe_filled_load_unselected;
    ScalableBitmap *m_pipe_filled_unload_unselected;
    ScalableBitmap *m_pipe_empty_load_unselected;
    ScalableBitmap *m_pipe_empty_unload_unselected;

    ScalableBitmap *m_left_extruder_active_filled;
    ScalableBitmap *m_left_extruder_active_empty;
    ScalableBitmap *m_left_extruder_unactive_filled;
    ScalableBitmap *m_left_extruder_unactive_empty;
    ScalableBitmap *m_right_extruder_active_filled;
    ScalableBitmap *m_right_extruder_active_empty;
    ScalableBitmap *m_right_extruder_unactive_filled;
    ScalableBitmap *m_right_extruder_unactive_empty;

    ScalableBitmap *m_extruder_single_nozzle_empty_load;
    ScalableBitmap *m_extruder_single_nozzle_empty_unload;
    ScalableBitmap *m_extruder_single_nozzle_filled_load;
    ScalableBitmap *m_extruder_single_nozzle_filled_unload;

    ExtruderState m_left_ext_state   = {ExtruderState::EMPTY_LOAD};
    ExtruderState m_right_ext_state  = {ExtruderState::EMPTY_LOAD};
    ExtruderState m_single_ext_state = {ExtruderState::EMPTY_LOAD};

public:
    void update(int nozzle_num, int nozzle_id);
    void update(ExtruderState single_state);
    void update(ExtruderState right_state, ExtruderState left_state);

    void msw_rescale();
    void setExtruderCount(int nozzle_num);
    void setExtruderUsed(std::string loc);
    void paintEvent(wxPaintEvent &evt);

    void     render(wxDC &dc);
    bool     m_show_state       = {false};
    int      m_nozzle_num       = 1;
    int      current_nozzle_idx = 0;
    std::string current_nozzle_loc = "";
    wxColour m_colour;

    string m_file_name;
    bool   m_ams_loading{false};
    void   doRender(wxDC &dc);
    ExtruderImage(wxWindow *parent, wxWindowID id, int nozzle_num, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);
    ~ExtruderImage();
};

class ExtruderSwithingStatus : public wxPanel
{
public:
    ExtruderSwithingStatus(wxWindow *parent);
    ~ExtruderSwithingStatus() = default;

public:
    void updateBy(MachineObject *obj);
    bool has_content_shown() const;

    void msw_rescale();

private:
    void updateBy(const DevExtderSystem* ext_system);
    void showQuitBtn(bool show);
    void showRetryBtn(bool show);

    void on_quit(wxCommandEvent &event);
    void on_retry(wxCommandEvent &event);

private:
    MachineObject *m_obj = nullptr;

    Label  *m_switching_status_label = nullptr;
    Button *m_button_quit      = nullptr;
    Button *m_button_retry     = nullptr;

    /*the last control time*/
    time_t m_last_ctrl_time = 0;
};

class ScoreDialog : public GUI::DPIDialog
{
public:
    ScoreDialog(wxWindow *parent, int design_id, std::string model_id, int profile_id, int rating_id, bool success_printed, int star_count = 0);
    ScoreDialog(wxWindow *parent, ScoreData *score_data);
    ~ScoreDialog();

    int       get_rating_id() { return m_rating_id; }
    ScoreData get_score_data();
    void      set_comment(std::string comment);
    void      set_cloud_bitmap(std::vector<std::string> cloud_bitmaps);

protected:
    enum StatusCode { 
        UPLOAD_PROGRESS = 0, 
        UPLOAD_EXIST_ISSUE, 
        UPLOAD_IMG_FAILED,
        CODE_NUMBER 
    };

    std::shared_ptr<int>     m_tocken;
    const int                m_photo_nums = 16;
    int                      m_rating_id;
    int                      m_design_id;
    std::string              m_model_id;
    int                      m_profile_id;
    int                      m_star_count;
    bool                     m_success_printed;
    std::vector<std::string> m_image_url_paths;
    StatusCode               m_upload_status_code;

    struct ImageMsg
    {
        wxString          local_image_url; //local image path
        std::string       img_url_paths; // oss url path
        vector<wxPanel *> image_broad; 
        bool              is_selected;
        bool              is_uploaded; // load
        wxBoxSizer *      image_tb_broad = nullptr;
    };

    std::vector<ScalableButton *>                  m_score_star;
    wxTextCtrl *                                   m_comment_text  = nullptr;
    Button *                                       m_button_ok     = nullptr;
    Button *                                       m_button_cancel = nullptr;
    Label *                                        m_add_photo     = nullptr;
    Label *                                        m_delete_photo  = nullptr;
    wxGridSizer *                                  m_image_sizer   = nullptr;
    wxStaticText *                                 warning_text    = nullptr;
    std::unordered_map<wxStaticBitmap *, ImageMsg> m_image;
    std::unordered_set<wxStaticBitmap *>           m_selected_image_list;

    void init();
    void update_static_bitmap(wxStaticBitmap *static_bitmap, wxImage image);
    void create_comment_text(const wxString &comment = "");
    void load_photo(const std::vector<std::pair<wxString, std::string>> &filePaths);
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void OnBitmapClicked(wxMouseEvent &event);

    wxBoxSizer * create_broad_sizer(wxStaticBitmap *bitmap, ImageMsg &cur_image_msg);
    wxBoxSizer * get_score_sizer();
    wxBoxSizer * get_star_sizer();
    wxBoxSizer * get_comment_text_sizer();
    wxBoxSizer * get_photo_btn_sizer();
    wxBoxSizer * get_button_sizer();
    wxBoxSizer * get_main_sizer(const std::vector<std::pair<wxString, std::string>> &images = std::vector<std::pair<wxString, std::string>>(), const wxString &comment = "");

    std::set<std::pair<wxStaticBitmap *, wxString>>        add_need_upload_imgs();
    std::pair<wxStaticBitmap *, ImageMsg>                  create_local_thumbnail(wxString &local_path);
    std::pair<wxStaticBitmap *, ImageMsg>                  create_oss_thumbnail(std::string &oss_path);
    
};

class PrintingTaskPanel : public wxPanel
{
public:
    PrintingTaskPanel(wxWindow* parent, PrintingTaskType type);
    ~PrintingTaskPanel();
    void create_panel(wxWindow* parent);
    

private:
    MachineObject*  m_obj{nullptr};
    ScalableBitmap  m_thumbnail_placeholder;
    std::string     m_thumbnail_bmp_display_name;
    wxBitmap        m_thumbnail_bmp_display;
    ScalableBitmap  m_bitmap_use_time;
    ScalableBitmap  m_bitmap_use_weight;
    ScalableBitmap  m_bitmap_background;

    wxPanel *       m_panel_printing_title;
    wxPanel*        m_staticline;
    wxPanel*        m_panel_error_txt;

    wxBoxSizer*     m_printing_sizer;
    wxStaticText *  m_staticText_printing;
    wxStaticText*   m_staticText_subtask_value;
    wxStaticText*   m_staticText_consumption_of_time;
    wxStaticText*   m_staticText_consumption_of_weight;
    wxStaticText*   m_printing_stage_value;
    ScalableButton* m_question_button;
    wxStaticText*   m_staticText_profile_value;
    wxStaticText*   m_staticText_progress_percent;
    wxStaticText*   m_staticText_progress_percent_icon;
    wxStaticText*   m_staticText_progress_left;
    // Orca: show print end time
    wxStaticText * m_staticText_progress_end;
    wxStaticText*   m_staticText_layers;
    wxStaticText *  m_has_rated_prompt;
    wxStaticText *  m_request_failed_info;
    wxStaticBitmap* m_bitmap_thumbnail;
    int             m_plate_index { -1 };
    wxStaticBitmap* m_bitmap_static_use_time;
    wxStaticBitmap* m_bitmap_static_use_weight;
    ScalableButton* m_button_pause_resume;
    ScalableButton* m_button_abort;
    Button*         m_button_partskip;
    Button*         m_button_market_scoring;
    Button*         m_button_clean;
    Button *                      m_button_market_retry;
    wxPanel *                     m_score_subtask_info;
    wxPanel *                     m_score_staticline;
    wxPanel *                     m_request_failed_panel;
    wxPanel                      *m_printing_stage_underline;
    wxPanel                      *m_printing_stage_panel;

    // score page
    int                           m_star_count;
    std::vector<ScalableButton *> m_score_star;
    bool                          m_star_count_dirty = false;

    // partskip button
    int m_part_skipped_count{ 0 };
    int m_part_skipped_dirty{ 0 };

    ProgressBar*    m_gauge_progress;
    Label* m_error_text;
    PrintingTaskType m_type;
    int m_brightness_value{ -1 };

public:
    void init_bitmaps();
    void init_scaled_buttons();
    void error_info_reset();
    void show_error_msg(wxString msg);
    void reset_printing_value();
    void msw_rescale();

public:
    void enable_partskip_button(MachineObject* obj, bool enable);
    void enable_pause_resume_button(bool enable, std::string type);
    void enable_abort_button(bool enable);
    void update_subtask_name(wxString name);
    void update_stage_value(wxString stage, int val);
    void update_stage_value_with_machine(wxString stage, int val, MachineObject* obj = nullptr);
    void on_stage_clicked(wxMouseEvent& event);

    // Public interface to update remaining time text in the thermal dialog
    void update_thermal_remaining_time(MachineObject* obj);
    void update_progress_percent(wxString percent, wxString icon);
    void update_left_time(wxString time);
    void update_left_time(int mc_left_time);
    void show_layers_num(bool show) { m_staticText_layers->Show(show); }
    void update_layers_num(bool show, wxString num = wxEmptyString);
    void show_priting_use_info(bool show, wxString time = wxEmptyString, wxString weight = wxEmptyString);
    void show_profile_info(bool show, wxString profile = wxEmptyString);
    void set_thumbnail_img(const wxBitmap& bmp, const std::string& bmp_name);
    void set_brightness_value(int value) { m_brightness_value = value; }
    void set_plate_index(int plate_idx = -1);
    void market_scoring_show();
    void market_scoring_hide();
    
public:
    ScalableButton* get_abort_button() {return m_button_abort;};
    ScalableButton* get_pause_resume_button() {return m_button_pause_resume;};
    Button* get_partskip_button() { return m_button_partskip; };
    Button* get_market_scoring_button() {return m_button_market_scoring;};
    Button * get_market_retry_buttom() { return m_button_market_retry; };
    Button* get_clean_button() {return m_button_clean;};
    wxStaticBitmap* get_bitmap_thumbnail() {return m_bitmap_thumbnail;};
    wxPanel *  get_request_failed_panel() { return m_request_failed_panel; }
    int get_star_count() { return m_star_count; }
    void set_star_count(int star_count);
    std::vector<ScalableButton *> &get_score_star() { return m_score_star; }
    bool get_star_count_dirty() { return m_star_count_dirty; }
    void set_star_count_dirty(bool dirty) { m_star_count_dirty = dirty; }
    int get_part_skipped_count() { return m_part_skipped_count; }
    void set_part_skipped_count(int count) { m_part_skipped_count = count; }
    int get_part_skipped_dirty() { return m_part_skipped_dirty; }
    void set_part_skipped_dirty(int dirty) { m_part_skipped_dirty = dirty; }
    void                           set_has_reted_text(bool has_rated);
    void paint(wxPaintEvent&);
};

class StatusBasePanel : public wxScrolledWindow
{
protected:
    wxBitmap m_item_placeholder;
    ScalableBitmap m_thumbnail_placeholder;
    ScalableBitmap m_thumbnail_brokenimg;
    ScalableBitmap m_thumbnail_sdcard;
    wxBitmap m_bitmap_item_prediction;
    wxBitmap m_bitmap_item_cost;
    wxBitmap m_bitmap_item_print;
    ScalableBitmap m_bitmap_speed;
    ScalableBitmap m_bitmap_speed_active;
    ScalableBitmap m_bitmap_axis_home;
    ScalableBitmap m_bitmap_lamp_on;
    ScalableBitmap m_bitmap_lamp_off;
    ScalableBitmap m_bitmap_fan_on;
    ScalableBitmap m_bitmap_fan_off;
    ScalableBitmap m_bitmap_use_time;
    ScalableBitmap m_bitmap_use_weight;
    wxBitmap m_bitmap_extruder_empty_load;
    wxBitmap m_bitmap_extruder_filled_load;
    wxBitmap m_bitmap_extruder_empty_unload;
    wxBitmap m_bitmap_extruder_filled_unload;
    wxBitmap m_bitmap_extruder_now;

    CameraRecordingStatus m_state_recording{CameraRecordingStatus::RECORDING_NONE};
    CameraTimelapseStatus m_state_timelapse{CameraTimelapseStatus::TIMELAPSE_NONE};


    CameraItem *m_setting_button;

    wxBitmap m_bitmap_camera;
    ScalableBitmap m_bitmap_sdcard_state_normal;
    ScalableBitmap m_bitmap_sdcard_state_abnormal;
    ScalableBitmap m_bitmap_sdcard_state_no;
    ScalableBitmap m_bitmap_recording_on;
    ScalableBitmap m_bitmap_recording_off;
    ScalableBitmap m_bitmap_timelapse_on;
    ScalableBitmap m_bitmap_timelapse_off;
    ScalableBitmap m_bitmap_vcamera_on;
    ScalableBitmap m_bitmap_vcamera_off;
    ScalableBitmap m_bitmap_switch_camera;

    /* title panel */
    wxPanel *       media_ctrl_panel;
    wxPanel *       m_panel_monitoring_title;
    wxPanel *       m_panel_printing_title;
    wxPanel *       m_panel_control_title;

    wxStaticText*   m_staticText_consumption_of_time;
    wxStaticText *  m_staticText_consumption_of_weight;
    Label *         m_staticText_monitoring;
    wxStaticText *  m_staticText_timelapse;
    SwitchButton *  m_bmToggleBtn_timelapse;

    wxStaticText *m_mqtt_source;

    wxStaticBitmap *m_bitmap_camera_img;
    wxStaticBitmap *m_bitmap_recording_img;
    wxStaticBitmap *m_bitmap_timelapse_img;
    wxStaticBitmap* m_bitmap_vcamera_img;
    wxStaticBitmap *m_bitmap_sdcard_img;
    wxStaticBitmap *m_bitmap_static_use_time;
    wxStaticBitmap *m_bitmap_static_use_weight;
    wxStaticBitmap* m_camera_switch_button;


    wxMediaCtrl2 *  m_media_ctrl;
    MediaPlayCtrl * m_media_play_ctrl;

    Label *         m_staticText_printing;
    wxStaticBitmap *m_bitmap_thumbnail;
    wxStaticText *  m_staticText_subtask_value;
    wxStaticText *  m_printing_stage_value;
    wxStaticText *  m_staticText_profile_value;
    ProgressBar*    m_gauge_progress;
    wxStaticText *  m_staticText_progress_percent;
    wxStaticText *  m_staticText_progress_percent_icon;
    wxStaticText *  m_staticText_progress_left;
    wxStaticText *  m_staticText_layers;
    Button *        m_button_report;
    Button *        m_button_partskip;
    ScalableButton *m_button_pause_resume;
    ScalableButton *m_button_abort;
    Button *        m_button_clean;
    wxWebView *     m_custom_camera_view{nullptr};
    wxSimplebook*   m_extruder_book;
    std::vector<ExtruderImage *> m_extruderImage;

    SwitchBoard *   m_nozzle_btn_panel;

    wxStaticText *  m_text_tasklist_caption;

    Label *  m_staticText_control;
    ImageSwitchButton *m_switch_lamp;
    int               m_switch_lamp_timeout{0};
    ImageSwitchButton *m_switch_speed;

    /* TempInput */
    wxBoxSizer *    m_misc_ctrl_sizer;
    StaticBox*      m_fan_panel;
    StaticLine *    m_line_nozzle;
    TempInput*      m_tempCtrl_nozzle;
    int             m_temp_nozzle_timeout{ 0 };
    TempInput*      m_tempCtrl_nozzle_deputy;
    int             m_temp_nozzle_deputy_timeout{ 0 };
    TempInput *     m_tempCtrl_bed;
    int             m_temp_bed_timeout {0};
    TempInput *     m_tempCtrl_chamber;
    int             m_temp_chamber_timeout {0};
    FanSwitchButton *m_switch_nozzle_fan;
    int             m_switch_nozzle_fan_timeout{0};
    FanSwitchButton *m_switch_printing_fan;
    int             m_switch_printing_fan_timeout{0};
    FanSwitchButton *m_switch_cham_fan;
    FanSwitchButton *m_switch_fan;
    int             m_switch_cham_fan_timeout{0};
    wxPanel*        m_switch_block_fan;
    int             m_nozzle_num{ 0 };
    int             m_current_nozzle_id{ 0 };

    float           m_fixed_aspect_ratio{1.8};

    AxisCtrlButton *m_bpButton_xy;
    //wxStaticText *  m_staticText_xy;
    Button *        m_bpButton_z_10;
    Button *        m_bpButton_z_1;
    Button *        m_bpButton_z_down_1;
    Button *        m_bpButton_z_down_10;
    //Button *        m_button_unload;
    wxStaticText *  m_staticText_z_tip;
    Label *         m_extruder_label;
    Button *        m_bpButton_e_10;
    Button *        m_bpButton_e_down_10;
    ExtruderSwithingStatus *m_extruder_switching_status;

    wxPanel *       m_temp_temp_line;
    wxPanel *       m_temp_extruder_line;
    wxBoxSizer*     m_ams_list;
    wxStaticText *  m_ams_debug;
    bool            m_show_ams_group{false};
    bool            m_show_filament_group{ false };

    AMSControl*     m_ams_control;
    StaticBox*      m_ams_control_box;
    wxStaticBitmap *m_ams_extruder_img;
    wxStaticBitmap* m_bitmap_extruder_img;

    wxPanel *       m_panel_separator_right;
    wxPanel *       m_panel_separotor_bottom;
    wxGridBagSizer *m_tasklist_info_sizer{nullptr};
    wxBoxSizer *    m_printing_sizer;
    wxBoxSizer *    m_tasklist_sizer;
    wxBoxSizer *    m_tasklist_caption_sizer;
    wxPanel*        m_panel_error_txt;
    wxPanel*        m_staticline;
    Label *         m_error_text;
    wxStaticText*   m_staticText_calibration_caption;
    wxStaticText*   m_staticText_calibration_caption_top;
    wxStaticText*   m_calibration_text;
    Button*         m_parts_btn;
    Button*         m_options_btn;
    Button*         m_safety_btn;
    Button*         m_calibration_btn;
    StepIndicator*  m_calibration_flow;

    wxPanel *       m_machine_ctrl_panel;
    wxPanel *       m_scale_panel;
    wxStaticBitmap* m_img_filament_loading;
    PrintingTaskPanel *       m_project_task_panel;

    FilamentLoad* m_filament_step;
    wxStaticBitmap *m_filament_load_img;

    Button *m_button_retry {nullptr};
    StaticBox* m_filament_load_box;

    // Virtual event handlers, override them in your derived class
    virtual void on_subtask_partskip(wxCommandEvent &event) { event.Skip(); }
    virtual void on_subtask_pause_resume(wxCommandEvent &event) { event.Skip(); }
    virtual void on_subtask_abort(wxCommandEvent &event) { event.Skip(); }
    virtual void on_lamp_switch(wxCommandEvent &event) { event.Skip(); }
    virtual void on_bed_temp_kill_focus(wxFocusEvent &event) { event.Skip(); }
    virtual void on_bed_temp_set_focus(wxFocusEvent &event) { event.Skip(); }
    virtual void on_nozzle_temp_kill_focus(wxFocusEvent &event) { event.Skip(); }
    virtual void on_nozzle_temp_set_focus(wxFocusEvent &event) { event.Skip(); }    
    virtual void on_nozzle_fan_switch(wxCommandEvent &event) { event.Skip(); }
    virtual void on_printing_fan_switch(wxCommandEvent &event) { event.Skip(); }
    virtual void on_axis_ctrl_z_up_10(wxCommandEvent &event) { event.Skip(); }
    virtual void on_axis_ctrl_z_up_1(wxCommandEvent &event) { event.Skip(); }
    virtual void on_axis_ctrl_z_down_1(wxCommandEvent &event) { event.Skip(); }
    virtual void on_axis_ctrl_z_down_10(wxCommandEvent &event) { event.Skip(); }
    virtual void on_axis_ctrl_e_up_10(wxCommandEvent &event) { event.Skip(); }
    virtual void on_axis_ctrl_e_down_10(wxCommandEvent &event) { event.Skip(); }
    virtual void on_nozzle_selected(wxCommandEvent &event) { event.Skip(); }
    void on_camera_source_change(wxCommandEvent& event);
    void handle_camera_source_change();
    void remove_controls();
    void on_webview_navigating(wxWebViewEvent& evt);
    void on_camera_switch_toggled(wxMouseEvent& event);
    void toggle_custom_camera();
    void toggle_builtin_camera();

public:
    StatusBasePanel(wxWindow *      parent,
                    wxWindowID      id    = wxID_ANY,
                    const wxPoint & pos   = wxDefaultPosition,
                    const wxSize &  size  = wxDefaultSize,
                    long            style = wxTAB_TRAVERSAL,
                    const wxString &name  = wxEmptyString);

    ~StatusBasePanel();

    MachineObject* obj{nullptr};
    void init_bitmaps();
    wxBoxSizer *create_monitoring_page();
    wxBoxSizer *create_machine_control_page(wxWindow *parent);

    wxBoxSizer *create_temp_axis_group(wxWindow *parent);
    wxBoxSizer *create_temp_control(wxWindow *parent);
    wxBoxSizer *create_misc_control(wxWindow *parent);
    wxBoxSizer *create_axis_control(wxWindow *parent);
    wxPanel *create_bed_control(wxWindow *parent);
    wxBoxSizer *create_extruder_control(wxWindow *parent);

    void reset_temp_misc_control();
    int before_error_code = 0;
    int skip_print_error = 0;
    wxBoxSizer *create_ams_group(wxWindow *parent);
    wxBoxSizer *create_settings_group(wxWindow *parent);
    wxBoxSizer* create_filament_group(wxWindow* parent);

	void           expand_filament_loading(wxMouseEvent &e);
    void           show_ams_group(bool show = true);
    void show_filament_load_group(bool show = true);
    MediaPlayCtrl* get_media_play_ctrl() {return m_media_play_ctrl;};
};


class StatusPanel : public StatusBasePanel
{
private:
    friend class MonitorPanel;

protected:
    std::shared_ptr<SliceInfoPopup> m_slice_info_popup;
    std::shared_ptr<ImageTransientPopup> m_image_popup;
    std::shared_ptr<CameraPopup> m_camera_popup;
    std::set<int> rated_model_id;
    AMSSetting *m_ams_setting_dlg{nullptr};
    PrinterPartsDialog*  print_parts_dlg { nullptr };
    PrintOptionsDialog*  print_options_dlg { nullptr };
    SafetyOptionsDialog* safety_options_dlg { nullptr };
    CalibrationDialog*   calibration_dlg {nullptr};
    AMSMaterialsSetting *m_filament_setting_dlg{nullptr};

    DeviceErrorDialog* m_print_error_dlg = nullptr;
    SecondaryCheckDialog* abort_dlg = nullptr;
    SecondaryCheckDialog* con_load_dlg = nullptr;
    MessageDialog *       ctrl_e_hint_dlg             = nullptr;

    SecondaryCheckDialog* sdcard_hint_dlg = nullptr;

    FanControlPopupNew* m_fan_control_popup{nullptr};

    ExtrusionCalibration *m_extrusion_cali_dlg{nullptr};
    PartSkipDialog       *m_partskip_dlg{nullptr};

    wxString     m_request_url;
    bool         m_start_loading_thumbnail = false;
    bool         m_load_sdcard_thumbnail = false;
    int          m_last_sdcard    = -1;
    int          m_last_recording = -1;
    int          m_last_timelapse = -1;
    int          m_last_extrusion = -1;
    int          m_last_vcamera   = -1;
    int          m_model_mall_request_count = 0;
    bool         m_is_load_with_temp = false;
    json         m_rating_result;

    wxWebRequest web_request;
    bool bed_temp_input    = false;
    bool nozzle_temp_input = false;
    bool cham_temp_input   = false;
    bool request_model_info_flag = false;
    int speed_lvl = 1; // 0 - 3
    int speed_lvl_timeout {0};
    boost::posix_time::ptime speed_dismiss_time;
    bool m_show_mode_changed = false;
    std::map<wxString, wxImage> img_list; // key: url, value: wxBitmap png Image
    std::map<std::string, std::string> m_print_connect_types;
    std::vector<Button *>       m_buttons;
    int last_status;
    ScoreData *m_score_data;
    wxBitmap* calib_bitmap = nullptr;
    CalibMode m_calib_mode;
    CalibrationMethod m_calib_method;
    int cali_stage;
    PrintingTaskType m_current_print_mode = PrintingTaskType::NOT_CLEAR;

    void init_scaled_buttons();
    void create_tasklist_info();
    void show_task_list_info(bool show = true);
    void update_tasklist_info();

    void on_market_scoring(wxCommandEvent &event);
    void on_market_retry(wxCommandEvent &event);
    void on_subtask_partskip(wxCommandEvent &event);
    void on_subtask_pause_resume(wxCommandEvent &event);
    void on_subtask_abort(wxCommandEvent &event);
    void on_print_error_clean(wxCommandEvent &event);
    void error_info_reset();
    void show_recenter_dialog();

    /* axis control */
    bool check_axis_z_at_home(MachineObject* obj);
    void on_axis_ctrl_xy(wxCommandEvent &event);
    void on_axis_ctrl_z_up_10(wxCommandEvent &event);
    void on_axis_ctrl_z_up_1(wxCommandEvent &event);
    void on_axis_ctrl_z_down_1(wxCommandEvent &event);
    void on_axis_ctrl_z_down_10(wxCommandEvent &event);
    void on_axis_ctrl_e_up_10(wxCommandEvent &event);
    void on_axis_ctrl_e_down_10(wxCommandEvent &event);
    void axis_ctrl_e_hint(bool up_down);

    void on_nozzle_selected(wxCommandEvent &event);
    /* temp control */
    void on_bed_temp_kill_focus(wxFocusEvent &event);
    void on_bed_temp_set_focus(wxFocusEvent &event);
    void on_set_bed_temp();
    void on_nozzle_temp_kill_focus(wxFocusEvent &event);
    void on_nozzle_temp_set_focus(wxFocusEvent &event);
    void on_set_nozzle_temp(int nozzle_id);
    void on_set_chamber_temp();

    /* extruder apis */
    void on_ams_load(SimpleEvent &event);
    void update_load_with_temp();
    void on_ams_load_curr();
    void on_ams_load_vams(wxCommandEvent& event);
    void on_ams_switch(SimpleEvent &event);
    void on_ams_unload(SimpleEvent &event);
    void on_ams_filament_backup(SimpleEvent& event);
    void on_ams_setting_click(SimpleEvent& event);
    void on_filament_edit(wxCommandEvent &event);
    void on_ext_spool_edit(wxCommandEvent &event);
    void on_filament_extrusion_cali(wxCommandEvent &event);
    void on_ams_refresh_rfid(wxCommandEvent &event);
    void on_ams_selected(wxCommandEvent &event);
    void on_ams_guide(wxCommandEvent &event);
    void on_ams_retry(wxCommandEvent &event);

    void on_fan_changed(wxCommandEvent& event);
    void on_cham_temp_kill_focus(wxFocusEvent& event);
    void on_cham_temp_set_focus(wxFocusEvent& event);
    void on_switch_speed(wxCommandEvent& event);
    void on_lamp_switch(wxCommandEvent &event);
    void on_printing_fan_switch(wxCommandEvent &event);
    void on_nozzle_fan_switch(wxCommandEvent &event);
    void on_thumbnail_enter(wxMouseEvent &event);
    void on_thumbnail_leave(wxMouseEvent &event);
    void refresh_thumbnail_webrequest(wxMouseEvent& event);
    void on_switch_vcamera(wxMouseEvent &event);
    void on_camera_enter(wxMouseEvent &event);
    void on_camera_leave(wxMouseEvent& event);
    void on_auto_leveling(wxCommandEvent &event);
    void on_xyz_abs(wxCommandEvent &event);


    void on_show_parts_options(wxCommandEvent& event);
    /* print options */
    void on_show_print_options(wxCommandEvent &event);
    /* safety options */
    void on_show_safety_options(wxCommandEvent &event);

    /* calibration */
    void on_start_calibration(wxCommandEvent &event);


    /* update apis */
    void update(MachineObject* obj);
    void show_printing_status(bool ctrl_area = true, bool temp_area = true);
    void update_left_time(int mc_left_time);
    void update_basic_print_data(bool def = false);
    void update_model_info();
    void update_subtask(MachineObject* obj);
    void update_partskip_subtask(MachineObject *obj);
    void update_cloud_subtask(MachineObject *obj);
    void update_sdcard_subtask(MachineObject *obj);
    void update_temp_ctrl(MachineObject *obj);
    void update_misc_ctrl(MachineObject *obj);
    void update_ams(MachineObject* obj);
    void update_filament_loading_panel(MachineObject* obj);

    void update_extruder_status(MachineObject* obj);
    void update_ams_control_state(std::string ams_id, std::string slot_id);
    void update_cali(MachineObject* obj);
    void update_calib_bitmap();

    void reset_printing_values();
    void on_webrequest_state(wxWebRequestEvent &evt);
    bool is_task_changed(MachineObject* obj);

    /* camera */
    void update_camera_state(MachineObject* obj);
    bool show_vcamera = false;

    // partskip button
    void update_partskip_button(MachineObject* obj);

    // printer parts options
    void update_printer_parts_options(MachineObject* obj);

public:
    void update_error_message();

public:
    StatusPanel(wxWindow *      parent,
                wxWindowID      id    = wxID_ANY,
                const wxPoint & pos   = wxDefaultPosition,
                const wxSize  & size  = wxDefaultSize,
                long            style = wxTAB_TRAVERSAL,
                const wxString &name  = wxEmptyString);
    ~StatusPanel();

    enum ThumbnailState {
        PLACE_HOLDER = 0,
        BROKEN_IMG = 1,
        TASK_THUMBNAIL = 2,
        SDCARD_THUMBNAIL = 3,
        STATE_COUNT = 4
    };

    BBLSubTask *   last_subtask{nullptr};
    std::string    last_profile_id;
    std::string    last_task_id;
    long           last_tray_exist_bits { -1 };
    long           last_ams_exist_bits { -1 };
    long           last_tray_is_bbl_bits{ -1 };
    long           last_read_done_bits{ -1 };
    long           last_reading_bits { -1 };
    long           last_ams_version { -1 };
    std::optional<int> last_cali_version;

    enum ThumbnailState task_thumbnail_state {ThumbnailState::PLACE_HOLDER};
    std::vector<int> last_stage_list_info;

    bool is_stage_list_info_changed(MachineObject* obj);

    void set_default();
    void show_status(int status);
    void set_hold_count(int& count);

    void rescale_camera_icons();
    void on_sys_color_changed();
    void msw_rescale();
};
}
}
#endif

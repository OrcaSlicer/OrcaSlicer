#ifndef slic3r_GUI_SelectMachine_hpp_
#define slic3r_GUI_SelectMachine_hpp_

#include <wx/wx.h>
#include <wx/intl.h>
#include <wx/collpane.h>
#include <wx/dataview.h>
#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/dataview.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/hyperlink.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/popupwin.h>
#include <wx/spinctrl.h>
#include <wx/artprov.h>
#include <wx/wrapsizer.h>
#include <wx/srchctrl.h>

#include <unordered_map>

#include "boost/bimap/bimap.hpp"
#include "AmsMappingPopup.hpp"
#include "ReleaseNote.hpp"
#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "DeviceManager.hpp"
#include "Plater.hpp"
#include "BBLStatusBar.hpp"
#include "BBLStatusBarPrint.hpp"
#include "PrePrintChecker.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/ScrolledWindow.hpp"
#include "Widgets/PopupWindow.hpp"
#include <wx/simplebook.h>
#include <wx/hashmap.h>

#include "Jobs/Worker.hpp"

#define  PRINT_OPT_BG_GRAY       0xF8F8F8
#define  PRINT_OPT_ITEM_BG_GRAY  0xEEEEEE


// Previous definitions
namespace Slic3r{
    class DevExtder;
}

namespace Slic3r { namespace GUI {

std::string get_nozzle_volume_type_cloud_string(NozzleVolumeType nozzle_volume_type);
void        print_ams_mapping_result(std::vector<FilamentInfo> &result);
enum PrintFromType {
    FROM_NORMAL,
    FROM_SDCARD_VIEW,
};

enum PrintPageMode {
    PrintPageModePrepare = 0,
    PrintPageModeSending,
    PrintPageModeFinish
};


class Material
{
public:
    int           id;
    MaterialItem *item;
};


enum class CloudTaskNozzleId : int
{
    NOZZLE_RIGHT    = 0,
    NOZZLE_LEFT     = 1,
};

enum class FilamentMapNozzleId : int
{
    NOZZLE_LEFT     = 1,
    NOZZLE_RIGHT    = 2,
};

enum class ConfigNozzleIdx : int
{
    NOZZLE_LEFT      = 0,
    NOZZLE_RIGHT     = 1,
};


WX_DECLARE_HASH_MAP(int, Material *, wxIntegerHash, wxIntegerEqual, MaterialHash);

#define SELECT_MACHINE_DIALOG_BUTTON_SIZE wxSize(FromDIP(57), FromDIP(32))
#define SELECT_MACHINE_DIALOG_BUTTON_SIZE2 wxSize(FromDIP(80), FromDIP(32))
#define SELECT_MACHINE_DIALOG_SIMBOOK_SIZE wxSize(FromDIP(370), FromDIP(64))
#define SELECT_MACHINE_DIALOG_SIMBOOK_SIZE2 wxSize(FromDIP(645), FromDIP(32))
#define LIST_REFRESH_INTERVAL 200
static int get_brightness_value(wxImage image) {

    wxImage grayImage = image.ConvertToGreyscale();

    int width = grayImage.GetWidth();
    int height = grayImage.GetHeight();

    int totalLuminance = 0;
    unsigned char alpha;
    int num_none_transparent = 0;
    for (int y = 0; y < height; y += 2) {

        for (int x = 0; x < width; x += 2) {

            alpha = image.GetAlpha(x, y);
            if (alpha != 0) {
                wxColour pixelColor = grayImage.GetRed(x, y);
                totalLuminance += pixelColor.Red();
                num_none_transparent = num_none_transparent + 1;
            }
        }
    }
    if (totalLuminance <= 0 || num_none_transparent <= 0) {
        return 0;
    }
    return totalLuminance / num_none_transparent;
}

struct POItem
{
    std::string key;
    wxString    value; // the display value

 public:
    bool operator==(const POItem &other) const { return key == other.key && value == other.value; }
};

#define PRINT_OPT_WIDTH  FromDIP(44)
class PrintOptionItem : public wxPanel
{
public:
    PrintOptionItem(wxWindow* parent, std::vector<POItem> ops, std::string param = "");
    ~PrintOptionItem() {};

public:
    void        setValue(std::string value);
    std::string getValue() const { return selected_key; }
    void        update_options(std::vector<POItem> ops) {
        if (m_ops != ops)
        {
            m_ops = ops;
            selected_key = "";

            auto width = ops.size() * PRINT_OPT_WIDTH + FromDIP(8);
            auto height = FromDIP(22) + FromDIP(8);
            SetMinSize(wxSize(width, height));
            SetMaxSize(wxSize(width, height));
            Refresh();
        }
    };

    bool is_enabled() const { return m_enable; }
    void enable(bool able) {
        if (m_enable != able)
        {
            m_enable = able;
            Refresh();
        }
    }

    void msw_rescale() { m_selected_bk.msw_rescale(); Refresh(); };

private:
    void OnPaint(wxPaintEvent& event);
    void render(wxDC& dc);
    void on_left_down(wxMouseEvent& evt);
    void doRender(wxDC& dc);

private:
    ScalableBitmap m_selected_bk;
    ScalableBitmap m_selected_bk_dark;
    ScalableBitmap m_selected_disabled_bk;
    ScalableBitmap m_selected_disabled_bk_dark;
    std::vector<POItem> m_ops;
    std::string selected_key;
    std::string m_param;

    bool m_enable = true;
};

class PrintOption : public wxPanel
{
private:
    std::string         m_param;
    std::vector<POItem> m_ops;
    Label              *m_printoption_title{nullptr};
    ScalableButton     *m_printoption_tips{ nullptr };
    PrintOptionItem    *m_printoption_item{nullptr};
    wxString           m_full_title;

public:
    PrintOption(wxWindow *parent, wxString title, wxString tips, std::vector<POItem> ops, std::string param = "");
    ~PrintOption(){};

public:
    void        enable(bool en);

    void        setValue(std::string value);
    std::string getValue();
    int         getValueInt();

    std::string getParam() const { return m_param; }

    bool        contain_opt(const std::string& opt_str) const;
    void        update_options(std::vector<POItem> ops, const wxString &tips);
    void        update_tooltip(const wxString &tips);
    void        update_title_display();

    void  msw_rescale();

    // override funcs
    bool  CanBeFocused() const override { return false; }

private:
    void OnPaint(wxPaintEvent &event);
    void render(wxDC &dc);
    void doRender(wxDC &dc);
};

class ThumbnailPanel : public wxPanel
{
public:
    wxBitmap        m_bitmap;
    wxStaticBitmap *m_staticbitmap{nullptr};

    ThumbnailPanel(wxWindow *parent, wxWindowID winid = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);
    ~ThumbnailPanel();

    void OnPaint(wxPaintEvent &event);
    void PaintBackground(wxDC &dc);
    void OnEraseBackground(wxEraseEvent &event);
    void set_thumbnail(wxImage &img);
    void render(wxDC &dc);

private:
    ScalableBitmap m_background_bitmap;
    wxBitmap       bitmap_with_background;
    int            m_brightness_value{-1};
};


class SendModeSwitchButton : public wxPanel
{
public:
    SendModeSwitchButton(wxWindow *parent, wxString mode, bool sel);
    ~SendModeSwitchButton(){};

public:
    void msw_rescale();
    void setSelected(bool selected);
    bool isSelected(){return is_selected;};

private:
    void OnPaint(wxPaintEvent& event);
    void render(wxDC& dc);
    void on_left_down(wxMouseEvent& evt);
    void doRender(wxDC& dc);

private:
    bool is_selected {false};
    ScalableBitmap m_img_selected;
    ScalableBitmap m_img_unselected;
    ScalableBitmap m_img_selected_tag;
    ScalableBitmap m_img_unselected_tag;
};

class PrinterInfoBox;
class SelectMachineDialog : public DPIDialog
{
private:
    int                                 m_current_filament_id{0};
    int                                 m_print_plate_idx{0};
    int                                 m_print_plate_total{0};
    int                                 m_timeout_count{0};
    int                                 m_print_error_code{0};
    bool                                m_is_in_sending_mode{ false };
    bool                                m_ams_mapping_res{ false };
    bool                                m_ams_mapping_valid{ false };
    bool                                m_export_3mf_cancel{ false };
    bool                                m_is_canceled{ false };
    bool                                m_is_rename_mode{ false };
    bool                                m_check_flag {false};
    bool                                m_ext_change_assist{ false };
    PrintPageMode                       m_print_page_mode{PrintPageMode::PrintPageModePrepare};
    std::string                         m_print_error_msg;
    std::string                         m_print_error_extra;
    std::string                         m_printer_last_select;
    std::string                         m_print_info;
    wxString                            m_current_project_name;
    PrintDialogStatus                   m_print_status { PrintStatusInit };
    wxColour                            m_colour_def_color{wxColour(255, 255, 255)};
    wxColour                            m_colour_bold_color{wxColour(38, 46, 48)};
    StateColor                          m_btn_bg_enable;

    std::unordered_map<string, PrintOption*> m_checkbox_list;
    std::list<PrintOption*>                  m_checkbox_list_order;

    std::shared_ptr<int>                m_token = std::make_shared<int>(0);
    wxString                             m_ams_tooltip;
    std::vector<wxString>               m_bedtype_list;
    std::vector<MachineObject*>         m_list;
    std::vector<FilamentInfo>           m_filaments;
    std::vector<FilamentInfo>           m_ams_mapping_result;
    std::vector<int>                    m_filaments_map;
    std::shared_ptr<BBLStatusBarPrint>  m_status_bar;
    std::unique_ptr<Worker>             m_worker;

    Slic3r::DynamicPrintConfig          m_required_data_config;
    Slic3r::Model                       m_required_data_model;
    Slic3r::PlateDataPtrs               m_required_data_plate_data_list;
    std::string                         m_required_data_file_name;
    std::string                         m_required_data_file_path;

    std::vector<POItem> ops_auto;
    std::vector<POItem> ops_no_auto;

protected:
    PrintFromType                       m_print_type{FROM_NORMAL};
    AmsMapingPopup                      m_mapping_popup{ nullptr };
    AmsMapingTipPopup                   m_mapping_tip_popup{ nullptr };
    AmsTutorialPopup                    m_mapping_tutorial_popup{ nullptr };
    MaterialHash                        m_materialList;
    Plater *                            m_plater{nullptr};
    wxPanel *                           m_options_other {nullptr};
    wxGridSizer*                        m_sizer_options{nullptr};
    wxBoxSizer*                         m_sizer_thumbnail{ nullptr };

    wxBoxSizer*                         m_basicl_sizer{ nullptr };
    wxBoxSizer*                         rename_sizer_v{ nullptr };
    wxBoxSizer*                         rename_sizer_h{ nullptr };
    wxBoxSizer*                         m_sizer_autorefill{ nullptr };
    wxBoxSizer*                         m_mapping_sugs_sizer{ nullptr };
    wxBoxSizer*                         m_change_filament_times_sizer{ nullptr };
    Button*                             m_button_ensure{ nullptr };
    wxStaticBitmap *                    m_rename_button{nullptr};
    wxStaticBitmap*                     m_staticbitmap{ nullptr };
    wxStaticBitmap*                     m_bitmap_last_plate{ nullptr };
    wxStaticBitmap*                     m_bitmap_next_plate{ nullptr };
    wxStaticBitmap*                     img_amsmapping_tip{nullptr};
    ThumbnailPanel*                     m_thumbnailPanel{ nullptr };
    wxPanel*                            m_panel_status{ nullptr };
    wxPanel*                            m_basic_panel;
    wxPanel*                            m_rename_normal_panel{nullptr};
    wxPanel*                            m_panel_sending{nullptr};
    wxPanel*                            m_panel_prepare{nullptr};
    wxPanel*                            m_panel_finish{nullptr};

    wxScrolledWindow*                   m_scroll_area{nullptr};

    wxPanel*                            m_line_top{ nullptr };
    Label*                              m_link_edit_nozzle{ nullptr };
    Label*                              m_st_txt_error_code{nullptr};
    Label*                              m_st_txt_error_desc{nullptr};
    Label*                              m_st_txt_extra_info{nullptr};
    Label*                              m_ams_backup_tip{nullptr};
    wxHyperlinkCtrl*                    m_link_network_state{ nullptr };
    wxSimplebook*                       m_rename_switch_panel{nullptr};
    wxSimplebook*                       m_simplebook{nullptr};
    wxStaticText*                       m_rename_text{nullptr};
    Label*                              m_stext_time{ nullptr };
    Label*                              m_stext_weight{ nullptr };
    PrinterMsgPanel *                   m_statictext_ams_msg{nullptr};
    Label*                              m_txt_change_filament_times{ nullptr };
    CheckBox*                           m_check_ext_change_assist{ nullptr };
    Label*                              m_label_ext_change_assist{ nullptr };

    PrinterInfoBox*                     m_printer_box { nullptr};
    PrinterMsgPanel *                   m_text_printer_msg{nullptr};
    Label*                              m_text_printer_msg_tips{ nullptr };
    wxStaticText*                       m_staticText_bed_title{ nullptr };
    wxStaticText*                       m_stext_sending{ nullptr };
    wxStaticText*                       m_statictext_finish{nullptr};
    TextInput*                          m_rename_input{nullptr};
    wxTimer*                            m_refresh_timer{ nullptr };
    std::shared_ptr<PrintJob>           m_print_job;
    wxScrolledWindow*                   m_sw_print_failed_info{nullptr};
    ScalableBitmap *                    rename_editable{nullptr};
    ScalableBitmap *                    rename_editable_light{nullptr};
    wxStaticBitmap *                    timeimg{nullptr};
    ScalableBitmap *                    print_time{nullptr};
    wxStaticBitmap *                    weightimg{nullptr};
    ScalableBitmap *                    print_weight{nullptr};
    ScalableBitmap *                    ams_mapping_help_icon{nullptr};
    wxStaticBitmap *                    img_ams_backup{nullptr};
    ThumbnailData                       m_cur_input_thumbnail_data;
    ThumbnailData                       m_cur_no_light_thumbnail_data;
    ThumbnailData                       m_preview_thumbnail_data;//when ams map change
    std::vector<wxColour>               m_preview_colors_in_thumbnail;
    std::vector<wxColour>               m_cur_colors_in_thumbnail;
    std::vector<bool>                   m_edge_pixels;

    StaticBox*                          m_filament_panel;
    StaticBox*                          m_filament_left_panel;
    StaticBox*                          m_filament_right_panel;

    wxBoxSizer*                         m_filament_panel_sizer;
    wxBoxSizer*                         m_filament_panel_left_sizer;
    wxBoxSizer*                         m_filament_panel_right_sizer;
    wxBoxSizer*                         m_sizer_filament_2extruder;

    wxGridSizer*                        m_sizer_ams_mapping{ nullptr };
    wxGridSizer*                        m_sizer_ams_mapping_left{ nullptr };
    wxGridSizer*                        m_sizer_ams_mapping_right{ nullptr };

    PrePrintChecker                     m_pre_print_checker;

public:
    static std::vector<wxString> MACHINE_BED_TYPE_STRING;
    static void                  init_machine_bed_types();
    static std::vector<std::string> MachineBedTypeString;

public:
    SelectMachineDialog(Plater *plater = nullptr);
    ~SelectMachineDialog();

    void init_bind();
    void init_timer();
    void show_print_failed_info(bool show, int code = 0, wxString description = wxEmptyString, wxString extra = wxEmptyString);
    void check_fcous_state(wxWindow* window);
    void popup_filament_backup();
    void update_select_layout(MachineObject *obj);
    void prepare_mode(bool refresh_button = true);
    void sending_mode();
    void finish_mode();
	void sync_ams_mapping_result(std::vector<FilamentInfo>& result);
    void prepare(int print_plate_idx);
    void show_status(PrintDialogStatus status, std::vector<wxString> params = std::vector<wxString>(), wxString wiki_url = wxEmptyString);
    void sys_color_changed();
    void reset_timeout();
    void update_user_printer();
    void reset_ams_material();
    void update_show_status(MachineObject* obj_ = nullptr);

    void UpdateStatusCheckWarning_ExtensionTool(MachineObject* obj_);

    void update_ams_check(MachineObject* obj);
    void update_filament_change_count();
    void on_rename_click(wxMouseEvent &event);
    void on_rename_enter();
    void update_printer_combobox(wxCommandEvent& event);
    void on_cancel(wxCloseEvent& event);
    void show_errors(wxString& info);
    void on_ok_btn(wxCommandEvent& event);
    void Enable_Auto_Refill(bool enable);
    void on_send_print();
    void clear_ip_address_config(wxCommandEvent& e);
    void on_refresh(wxCommandEvent& event);
    void on_set_finish_mapping(wxCommandEvent& evt);
    void on_print_job_cancel(wxCommandEvent& evt);
    void set_default();
    void change_materialitem_tip(bool no_ams_only_ext);
    void reset_and_sync_ams_list();
    void clone_thumbnail_data();
    void record_edge_pixels_data();
    wxColour adjust_color_for_render(const wxColour& color);
    void final_deal_edge_pixels_data(ThumbnailData& data);
    void updata_thumbnail_data_after_connected_printer();
    void unify_deal_thumbnail_data(ThumbnailData &input_data, ThumbnailData &no_light_data);
    void change_default_normal(int old_filament_id, wxColour temp_ams_color);
    void set_default_normal(const ThumbnailData&);
    void set_default_from_sdcard();
    void update_page_turn_state(bool show);
    void on_timer(wxTimerEvent& event);
    void on_selection_changed(wxCommandEvent &event);
    void Enable_Refresh_Button(bool en);
    void Enable_Send_Button(bool en);
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void update_user_machine_list();
    void update_print_status_msg();
    void update_print_error_info(int code, std::string msg, std::string extra);
    bool has_timelapse_warning(wxString& msg);
    bool has_timelapse_warning() { wxString msg; return has_timelapse_warning(msg);};
    bool can_support_pa_auto_cali();
    bool is_same_printer_model();
    bool is_blocking_printing(MachineObject* obj_);
    bool is_nozzle_hrc_matched(const DevExtder* extruder, std::string& filament_type) const;
    bool check_sdcard_for_timelpase(MachineObject* obj);
    bool is_timeout();
    int  update_print_required_data(Slic3r::DynamicPrintConfig config, Slic3r::Model model, Slic3r::PlateDataPtrs plate_data_list, std::string file_name, std::string file_path);
    void set_print_type(PrintFromType type) {m_print_type = type;};
    bool Show(bool show);
    void show_init();
    bool do_ams_mapping(MachineObject *obj_,bool use_ams);
    bool get_ams_mapping_result(std::string& mapping_array_str, std::string& mapping_array_str2, std::string& ams_mapping_info);
    bool build_nozzles_info(std::string& nozzles_info);
    bool can_hybrid_mapping(DevExtderSystem data);
    void auto_supply_with_ext(std::vector<DevAmsTray> slots);
    bool is_nozzle_type_match(DevExtderSystem data, wxString& error_message) const;
    int  convert_filament_map_nozzle_id_to_task_nozzle_id(int nozzle_id);

    PrintFromType get_print_type() {return m_print_type;};
    wxString    format_steel_name(NozzleType type);
    PrintDialogStatus  get_status() { return m_print_status; }

private:
    void EnableEditing(bool enable);

    /* update scroll area size*/
    void update_scroll_area_size();

    /* update option area*/
    void update_option_opts(MachineObject *obj);
    void update_options_layout();

    // save and restore from config
    void load_option_vals(MachineObject* obj);
    void save_option_vals();
    void save_option_vals(MachineObject *obj);

    // enbale or disable external change assist
    bool is_enable_external_change_assist(std::vector<FilamentInfo>& ams_mapping_result);
};

class PrinterInfoBox : public StaticBox
{
public:
    PrinterInfoBox(wxWindow* parent, SelectMachineDialog* select_dialog);

public:
    void  UpdatePlate(const std::string& plate_name);

    ComboBox* GetPrinterComboBox() const { return m_comboBox_printer; }
    void      SetPrinterName(const wxString& printer_name) { m_comboBox_printer->SetValue(printer_name); };
    void      SetPrinters(const std::vector<MachineObject*>& sorted_printers);

    void  EnableEditing(bool enable);
    void  EnableRefreshButton(bool enable);

    void  SetDefault(bool from_sd);

private:
    void  Create();

    void  OnBtnQuestionClicked(wxCommandEvent& event);

private:
    // owner
    SelectMachineDialog* m_select_dialog;

    Label*          m_stext_printer_title{ nullptr };
    ComboBox*       m_comboBox_printer{ nullptr };
    ScalableButton* m_button_refresh{ nullptr };
    ScalableButton* m_button_question { nullptr };

    wxStaticBitmap* m_bed_image{ nullptr };
    Label*         m_text_bed_type;
};



wxDECLARE_EVENT(EVT_SWITCH_PRINT_OPTION, wxCommandEvent);

}} // namespace Slic3r::GUI

#endif

#ifndef slic3r_SendMultiMachinePage_hpp_
#define slic3r_SendMultiMachinePage_hpp_

#include "GUI_Utils.hpp"
#include "MultiMachine.hpp"
#include "DeviceManager.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/ScrolledWindow.hpp"
#include "Widgets/PopupWindow.hpp"
#include "Widgets/TextInput.hpp"
#include "AmsMappingPopup.hpp"
#include "SelectMachine.hpp"

namespace Slic3r {
namespace GUI {
#define SEND_LEFT_PADDING_LEFT 15
#define SEND_LEFT_PRINTABLE    40
#define SEND_LEFT_DEV_NAME 250
#define SEND_LEFT_DEV_STATUS 250
#define SEND_LEFT_TAKS_STATUS 180

#define  DESIGN_SELECTOR_NOMORE_COLOR wxColour(248, 248, 248)
#define  DESIGN_GRAY900_COLOR wxColour(38, 46, 48)
#define  DESIGN_GRAY800_COLOR wxColour(50, 58, 61)
#define  DESIGN_GRAY600_COLOR wxColour(144, 144, 144)
#define  DESIGN_GRAY400_COLOR wxColour(166, 169, 170)
#define  DESIGN_RESOUTION_PREFERENCES wxSize(FromDIP(540), -1)
#define  DESIGN_COMBOBOX_SIZE wxSize(FromDIP(140), -1)
#define  DESIGN_LARGE_COMBOBOX_SIZE wxSize(FromDIP(160), -1)
#define  DESIGN_INPUT_SIZE wxSize(FromDIP(50), -1)




#define THUMBNAIL_SIZE FromDIP(128)

class RadioBox;
class AmsRadioSelector
{
public:
    wxString  m_param_name;
    int       m_groupid;
    RadioBox* m_radiobox;
    bool      m_selected = false;
};

WX_DECLARE_LIST(AmsRadioSelector, AmsRadioSelectorList);

class SendDeviceItem : public DeviceItem
{

public:
    SendDeviceItem(wxWindow* parent, MachineObject* obj);
    ~SendDeviceItem() {};

    void DrawTextWithEllipsis(wxDC& dc, const wxString& text, int maxWidth, int left, int top = 0);
    void OnEnterWindow(wxMouseEvent& evt);
    void OnLeaveWindow(wxMouseEvent& evt);
    void OnSelectedDevice(wxCommandEvent& evt);
    void OnLeftDown(wxMouseEvent& evt);
    void OnMove(wxMouseEvent& evt);

    void         paintEvent(wxPaintEvent& evt);
    void         render(wxDC& dc);
    void         doRender(wxDC& dc);
    void         post_event(wxCommandEvent&& event);
    virtual void DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);

public:
    bool m_hover{false};
    ScalableBitmap m_bitmap_check_disable;
    ScalableBitmap m_bitmap_check_off;
    ScalableBitmap m_bitmap_check_on;
};

class Plater;
class SendMultiMachinePage : public DPIDialog
{
private:
    /* dev_id -> device_item */
    std::map<std::string, SendDeviceItem*>  m_device_items;

    wxTimer*                            m_refresh_timer      = nullptr;

    // sort
    SortItem                            m_sort;
    bool                                device_name_big{ true };
    bool                                device_printable_big{ true };
    bool                                device_en_ams_big{ true };

    Button*                             m_button_send{ nullptr };
    wxScrolledWindow*                   scroll_macine_list{ nullptr };
    wxBoxSizer*                         sizer_machine_list{ nullptr };
    Plater*                             m_plater{ nullptr };

    int                                 m_print_plate_idx;
    bool                                m_is_canceled{ false };
    bool                                m_export_3mf_cancel{ false };
    AppConfig*                          app_config;

    wxPanel*                            m_main_page{ nullptr };
    wxScrolledWindow*                   m_main_scroll{ nullptr };
    wxBoxSizer*                         m_sizer_body{ nullptr };
    wxGridSizer*                        m_ams_list_sizer{ nullptr };
    AmsMapingPopup*                     m_mapping_popup{ nullptr };

    AmsRadioSelectorList                m_radio_group;
    MaterialHash                        m_material_list;
    std::map<std::string, CheckBox*>    m_checkbox_map;
    std::map<std::string, TextInput*>   m_input_map;
    std::vector<FilamentInfo>           m_filaments;
    std::vector<FilamentInfo>           m_ams_mapping_result;
    int                                 m_current_filament_id{ 0 };

    StateColor                          btn_bg_enable;

    // table head
    wxPanel*                            m_table_head_panel{ nullptr };
    wxBoxSizer*                         m_table_head_sizer{ nullptr };
    CheckBox*                           m_select_checkbox{ nullptr };
    Button*                             m_printer_name{ nullptr };
    Button*                             m_device_status{ nullptr };
    //Button*                             m_task_status{ nullptr };
    Button*                             m_ams{ nullptr };
    Button*                             m_refresh_button{ nullptr };

    // rename
    wxSimplebook*                       m_rename_switch_panel{ nullptr };
    wxPanel*                            m_rename_normal_panel{ nullptr };
    wxPanel*                            m_rename_edit_panel{ nullptr };
    TextInput*                          m_rename_input{ nullptr };
    ScalableButton*                     m_rename_button{ nullptr };
    wxBoxSizer*                         rename_sizer_v{ nullptr };
    wxBoxSizer*                         rename_sizer_h{ nullptr };
    wxStaticText*                       m_task_name{ nullptr };
    wxString                            m_current_project_name;
    bool                                m_is_rename_mode{ false };

    // title and thumbnail
    wxPanel*                            m_title_panel{ nullptr };
    wxBoxSizer*                         m_title_sizer{ nullptr };
    wxBoxSizer*                         m_text_sizer{ nullptr };
    wxStaticText*                       m_stext_time{ nullptr };
    wxStaticText*                       m_stext_weight{ nullptr };
    wxStaticBitmap*                     timeimg{ nullptr };
    ScalableBitmap*                     print_time{ nullptr };
    wxStaticBitmap*                     weightimg{ nullptr };
    ScalableBitmap*                     print_weight{ nullptr };
    wxBoxSizer*                         m_thumbnail_sizer{ nullptr };
    ThumbnailPanel*                     m_thumbnail_panel{nullptr};
    wxPanel*                            m_panel_image{ nullptr };
    wxBoxSizer*                         m_image_sizer{ nullptr };

    // tip when no device
    wxStaticText*                       m_tip_text{ nullptr };
    Button*                             m_button_add{ nullptr };

public:
    SendMultiMachinePage(Plater* plater = nullptr);
    ~SendMultiMachinePage();

    void prepare(int plate_idx);

    void on_dpi_changed(const wxRect& suggested_rect);
    void on_sys_color_changed();
    void refresh_user_device();
    void on_send(wxCommandEvent& event);
    bool Show(bool show);

    BBL::PrintParams request_params(MachineObject* obj);

    bool get_ams_mapping_result(std::string &mapping_array_str, std::string &mapping_array_str2, std::string &ams_mapping_info);
    wxBoxSizer* create_item_title(wxString title, wxWindow* parent, wxString tooltip);
    wxBoxSizer* create_item_checkbox(wxString title, wxWindow* parent, wxString tooltip, int padding_left, std::string param);
    wxBoxSizer* create_item_input(wxString str_before, wxString str_after, wxWindow* parent, wxString tooltip, std::string param);
    wxBoxSizer* create_item_radiobox(wxString title, wxWindow* parent, wxString tooltip, int groupid, std::string param);

    wxPanel* create_page();
    void sync_ams_list();
    void set_default_normal(const ThumbnailData& data);
    void set_default();
    void on_rename_enter();
    void check_fcous_state(wxWindow* window);
    void check_focus(wxWindow* window);

protected:
    void OnSelectRadio(wxMouseEvent& event);
    void on_select_radio(std::string param);
    bool get_value_radio(std::string param);
    void on_set_finish_mapping(wxCommandEvent& evt);
    void on_rename_click(wxCommandEvent& event);

    void on_timer(wxTimerEvent& event);
    void init_timer();

private:

};


} // namespace GUI
} // namespace Slic3r

#endif

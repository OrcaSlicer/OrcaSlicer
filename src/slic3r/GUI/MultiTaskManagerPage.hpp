#ifndef slic3r_MultiTaskManagerPage_hpp_
#define slic3r_MultiTaskManagerPage_hpp_

#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "MultiMachine.hpp"
#include "DeviceManager.hpp"
#include "TaskManager.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/ScrolledWindow.hpp"
#include "Widgets/PopupWindow.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { 
namespace GUI {

#define CLOUD_TASK_ITEM_MAX_WIDTH 1100
#define TASK_ITEM_MAX_WIDTH    900
#define TASK_LEFT_PADDING_LEFT 15
#define TASK_LEFT_PRINTABLE    40
#define TASK_LEFT_PRO_NAME     180
#define TASK_LEFT_DEV_NAME     150
#define TASK_LEFT_PRO_STATE    170
#define TASK_LEFT_PRO_INFO     230
#define TASK_LEFT_SEND_TIME    180

class MultiTaskItem : public DeviceItem
{
public:
    MultiTaskItem(wxWindow* parent, MachineObject* obj, int type);
    ~MultiTaskItem() {};


    void OnEnterWindow(wxMouseEvent& evt);
    void OnLeaveWindow(wxMouseEvent& evt);
    void OnSelectedDevice(wxCommandEvent& evt);
    void OnLeftDown(wxMouseEvent& evt);
    void OnMove(wxMouseEvent& evt);

    void         paintEvent(wxPaintEvent& evt);
    void         render(wxDC& dc);
    void         doRender(wxDC& dc);
    void         DrawTextWithEllipsis(wxDC& dc, const wxString& text, int maxWidth, int left, int top = 0);
    void         post_event(wxCommandEvent&& event);
    virtual void DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);

    bool m_hover{ false };
    wxString get_left_time(int mc_left_time);
    
    ScalableBitmap m_bitmap_check_disable;
    ScalableBitmap m_bitmap_check_off;
    ScalableBitmap m_bitmap_check_on;

    int          m_sending_percent{0};
    int          m_task_type{0}; //0-local 1-cloud
    wxString     m_project_name;
    wxString     m_dev_name;
    std::string  m_dev_id;
    TaskStateInfo* task_obj { nullptr };
    std::string  m_job_id;
    //std::string  m_sent_time;

    Button* m_button_resume{ nullptr };
    Button* m_button_cancel{ nullptr };
    Button* m_button_pause{ nullptr };
    Button* m_button_stop{ nullptr };

    void update_info();
    void onPause();
    void onResume();
    void onStop();
    void onCancel();
};

class LocalTaskManagerPage : public wxPanel
{
public:
    LocalTaskManagerPage(wxWindow* parent);
    ~LocalTaskManagerPage() {};

    void update_page();
    void refresh_user_device(bool clear = false);
    bool Show(bool show);
    void cancel_all(wxCommandEvent& evt);
    void msw_rescale();

private:
    SortItem                    m_sort;
    std::map<int, MultiTaskItem*> m_task_items;
    bool                        device_name_big{ true };
    bool                        device_state_big{ true };
    bool                        device_send_time{ true };

    wxPanel* m_main_panel{ nullptr };
    wxBoxSizer* m_main_sizer{ nullptr };
    wxBoxSizer* page_sizer{ nullptr };
    wxBoxSizer* m_sizer_task_list{ nullptr };
    wxScrolledWindow* m_task_list{ nullptr };
    wxStaticText* m_selected_num{ nullptr };

    // table head
    wxPanel* m_table_head_panel{ nullptr };
    wxBoxSizer* m_table_head_sizer{ nullptr };
    CheckBox* m_select_checkbox{ nullptr };
    Button* m_task_name{ nullptr };
    Button* m_printer_name{ nullptr };
    Button* m_status{ nullptr };
    Button* m_info{ nullptr };
    Button* m_send_time{ nullptr };
    Button* m_action{ nullptr };

    // ctrl button for all
    int m_sel_number{0};
    wxPanel* m_ctrl_btn_panel{ nullptr };
    wxBoxSizer* m_btn_sizer{ nullptr };
    Button* btn_stop_all{ nullptr };
    wxStaticText* m_sel_text{ nullptr };

    // tip when no device
    wxStaticText* m_tip_text{ nullptr };
};

class CloudTaskManagerPage : public wxPanel
{
public:
    CloudTaskManagerPage(wxWindow* parent);
    ~CloudTaskManagerPage();

    void update_page();
    void refresh_user_device(bool clear = false);
    std::string utc_time_to_date(std::string utc_time);
    bool Show(bool show);
    void update_page_number();
    void start_timer();
    void on_timer(wxTimerEvent& event);

    void pause_all(wxCommandEvent& evt);
    void resume_all(wxCommandEvent& evt);
    void stop_all(wxCommandEvent& evt);

    void enable_buttons(bool enable);
    void page_num_enter_evt();

    void msw_rescale();

private:
    SortItem                    m_sort;
    bool                        device_name_big{ true };
    bool                        device_state_big{ true };
    bool                        device_send_time{ true };

    /* job_id -> sel */
    std::map <std::string, MultiTaskItem*> m_task_items;

    wxPanel* m_main_panel{ nullptr };
    wxBoxSizer* page_sizer{ nullptr };
    wxBoxSizer* m_sizer_task_list{ nullptr };
    wxBoxSizer* m_main_sizer{ nullptr };
    wxScrolledWindow* m_task_list{ nullptr };
    wxStaticText* m_selected_num{ nullptr };

    // Flipping pages
    int                         m_current_page{ 0 };
    int                         m_total_page{0};
    int                         m_total_count{ 0 };
    int                         m_count_page_item{ 10 };
    bool                        prev{ false };
    bool                        next{ false };
    Button*                     btn_last_page{ nullptr };
    Button*                     btn_next_page{ nullptr };
    wxStaticText*               st_page_number{ nullptr };
    wxBoxSizer*                 m_flipping_page_sizer{ nullptr };
    wxBoxSizer*                 m_page_sizer{ nullptr };
    wxPanel*                    m_flipping_panel{ nullptr };
    wxTimer*                    m_flipping_timer{ nullptr };
    TextInput*                  m_page_num_input{ nullptr };
    Button*                     m_page_num_enter{ nullptr };

    // table head
    wxPanel*                    m_table_head_panel{ nullptr };
    wxBoxSizer*                 m_table_head_sizer{ nullptr };
    CheckBox*                   m_select_checkbox{ nullptr };
    Button*                     m_task_name{ nullptr };
    Button*                     m_printer_name{ nullptr };
    Button*                     m_status{ nullptr };
    Button*                     m_info{ nullptr };
    Button*                     m_send_time{ nullptr };
    Button*                     m_action{ nullptr };

    // ctrl button for all
    int                         m_sel_number;
    wxPanel*                    m_ctrl_btn_panel{ nullptr };
    wxBoxSizer*                 m_btn_sizer{ nullptr };
    Button*                     btn_pause_all{ nullptr };
    Button*                     btn_continue_all{ nullptr };
    Button*                     btn_stop_all{ nullptr };
    wxStaticText*               m_sel_text{ nullptr };

    // tip when no device
    wxStaticText*               m_tip_text{ nullptr };
    wxStaticText*               m_loading_text{ nullptr };
};


} // namespace GUI
} // namespace Slic3r

#endif

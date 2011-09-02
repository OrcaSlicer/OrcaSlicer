#ifndef slic3r_MultiMachineMangerPage_hpp_
#define slic3r_MultiMachineMangerPage_hpp_

#include "GUI_Utils.hpp"
#include "MultiMachine.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { 
namespace GUI {

#define DEVICE_LEFT_PADDING_LEFT 15
#define DEVICE_LEFT_DEV_NAME 180
#define DEVICE_LEFT_PRO_NAME 180
#define DEVICE_LEFT_PRO_INFO 320

class MultiMachineItem : public DeviceItem
{

public:
    MultiMachineItem(wxWindow* parent, MachineObject* obj);
    ~MultiMachineItem() {};

    void OnEnterWindow(wxMouseEvent& evt);
    void OnLeaveWindow(wxMouseEvent& evt);
    void OnLeftDown(wxMouseEvent& evt);
    void OnMove(wxMouseEvent& evt);

    void         paintEvent(wxPaintEvent& evt);
    void         render(wxDC& dc);
    void         DrawTextWithEllipsis(wxDC& dc, const wxString& text, int maxWidth,  int left, int top = 0);
    void         doRender(wxDC& dc);
    void         post_event(wxCommandEvent&& event);
    virtual void DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);

public:
    bool m_hover{ false };
    ScalableBitmap m_bitmap_check_disable;
    ScalableBitmap m_bitmap_check_off;
    ScalableBitmap m_bitmap_check_on;
    wxString get_left_time(int mc_left_time);
};
    
class MultiMachineManagerPage : public wxPanel
{
public:
    MultiMachineManagerPage(wxWindow* parent);
    ~MultiMachineManagerPage() {};

    void update_page();
    void refresh_user_device(bool clear = false);
    
    void sync_state(MachineObject* obj_);
    bool Show(bool show);

    std::vector<ObjState> extractRange(const std::vector<ObjState>& source, int start, int end);

    void start_timer();
    void update_page_number();
    void on_timer(wxTimerEvent& event);
    void clear_page();

    void page_num_enter_evt();

    void msw_rescale();

private:
    std::vector<ObjState>          m_state_objs;
    std::vector<MultiMachineItem*> m_device_items;
    SortItem                m_sort;
    bool                    device_dev_name_big{ true };
    bool                    device_state_big{ true };


    Button*                 m_button_edit{nullptr};
    wxBoxSizer*             page_sizer{ nullptr };
    wxPanel*                m_main_panel{ nullptr };
    wxBoxSizer*             m_main_sizer{nullptr};
    wxBoxSizer*             m_sizer_machine_list{nullptr};
    wxScrolledWindow*       m_machine_list{ nullptr };
    wxStaticText*           m_selected_num{ nullptr };

    // table head
    wxPanel*                m_table_head_panel{ nullptr };
    wxBoxSizer*             m_table_head_sizer{ nullptr };
    Button*                 m_printer_name{ nullptr };
    Button*                 m_task_name{ nullptr };
    Button*                 m_status{ nullptr };
    Button*                 m_action{ nullptr };
    Button*                 m_stop_all_botton{nullptr};

    // tip when no device
    wxStaticText*           m_tip_text{ nullptr };
    Button*                 m_button_add{ nullptr };

    // Flipping pages
    int                         m_current_page{ 0 };
    int                         m_total_page{ 0 };
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
};

} // namespace GUI
} // namespace Slic3r

#endif

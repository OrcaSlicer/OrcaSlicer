#ifndef slic3r_MultiMachinePage_hpp_
#define slic3r_MultiMachinePage_hpp_

#include "libslic3r/libslic3r.h"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "MultiTaskManagerPage.hpp"
#include "MultiMachineManagerPage.hpp"
#include "Tabbook.hpp"

#include "wx/button.h"

namespace Slic3r { 
namespace GUI {

#define PICK_LEFT_PADDING_LEFT 15
#define PICK_LEFT_PRINTABLE    40
#define PICK_LEFT_DEV_NAME 250
#define PICK_LEFT_DEV_STATUS 250
#define PICK_DEVICE_MAX 6
    
class MultiMachinePage : public wxPanel
{
private:
    wxTimer*                    m_refresh_timer      = nullptr;
    wxSizer*                    m_main_sizer{ nullptr };
    LocalTaskManagerPage*       m_local_task_manager{ nullptr };
    CloudTaskManagerPage*       m_cloud_task_manager{ nullptr };
    MultiMachineManagerPage*    m_machine_manager{ nullptr };
    Tabbook*                    m_tabpanel{ nullptr };

public:
    MultiMachinePage(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    ~MultiMachinePage();

    void jump_to_send_page();

    void on_sys_color_changed();
    void msw_rescale();
    bool Show(bool show);

    void init_tabpanel();
    void init_timer();
    void on_timer(wxTimerEvent& event);

    void clear_page();
};


class DevicePickItem : public DeviceItem
{

public:
    DevicePickItem(wxWindow* parent, MachineObject* obj);
    ~DevicePickItem() {};

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
    bool m_hover{ false };
    ScalableBitmap m_bitmap_check_disable;
    ScalableBitmap m_bitmap_check_off;
    ScalableBitmap m_bitmap_check_on;
};


class MultiMachinePickPage : public DPIDialog
{
private:
    AppConfig*          app_config;
    Label*              m_label{ nullptr };
    wxScrolledWindow*     scroll_macine_list{ nullptr };
    wxBoxSizer*         m_sizer_body{ nullptr };
    wxBoxSizer*                         sizer_machine_list{ nullptr };
    std::map<std::string, DevicePickItem*>  m_device_items;
    int                 m_selected_count{0};
public:
    MultiMachinePickPage(Plater* plater = nullptr);
    ~MultiMachinePickPage();

    int get_selected_count();
    void update_selected_count();
    void on_dpi_changed(const wxRect& suggested_rect);
    void on_sys_color_changed();
    void refresh_user_device();
    void on_confirm(wxCommandEvent& event);
    bool Show(bool show);
};

} // namespace GUI
} // namespace Slic3r

#endif

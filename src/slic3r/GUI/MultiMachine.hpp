#ifndef slic3r_MultiMachine_hpp_
#define slic3r_MultiMachine_hpp_

#include "GUI_Utils.hpp"
#include "DeviceManager.hpp"
#include <functional>

namespace Slic3r {
namespace GUI {


#define  DEVICE_ITEM_MAX_WIDTH 900
#define  SEND_ITEM_MAX_HEIGHT 30
#define  DEVICE_ITEM_MAX_HEIGHT 50

#define TABLE_HEAR_NORMAL_COLOUR    wxColour(238, 238, 238)
#define TABLE_HEAD_PRESSED_COLOUR   wxColour(150, 150, 150)
#define CTRL_BUTTON_NORMAL_COLOUR   wxColour(255, 255, 255)
#define CTRL_BUTTON_PRESSEN_COLOUR  wxColour(150, 150, 150)
#define TABLE_HEAD_FONT             Label::Body_13


class DeviceItem : public wxWindow
{
public:
    MachineObject* obj_{nullptr};
    int             state_online = { 0 }; //0-Offline 1-Online
    std::string     state_dev_name;     //device name
    int             state_printable{ 0 }; //0-idle 1-finish 2-failed 3-printing 4-upgrading 5-preset incompatible  6-unknown
    int             state_selected{ 0 };  //0-selected 1-unselected 2-un selectable
    int             state_enable_ams{ 0 };//0-no ams 1-enabled ams 2-not enabled ams
    int             state_device{ 0 };   //0-idle 1-finish 2-failed 3-running 4-pause  5-prepare  6-slicing 7-removed
    int             state_local_task{ 0 };  //0-padding  1-sending 2-sending finish  3-sending cancel  4-sending failed 5-TS_PRINT_SUCCESS 6- TS_PRINT_FAILED 7-TS_REMOVED 8-TS_IDLE
    int             state_cloud_task{ 0 };  //0-printing 1-printing finish 2-printing failed
    int             state_optional{0}; //0-Not optional 1-Optional
    std::string     m_send_time;

public:
    
    DeviceItem(wxWindow* parent, MachineObject* obj);
    ~DeviceItem() {};

    void on_refresh(wxCommandEvent& evt);
    void sync_state();
    wxString get_state_printable();
    wxString get_state_device();
    wxString get_local_state_task();
    wxString get_cloud_state_task();
    MachineObject* get_obj() const { return obj_; }

    int get_state_online() const { return state_online; }
    int get_state_printable() const { return state_printable; }
    int get_state_selected() const { return state_selected; }
    int get_state_enable_ams() const { return state_enable_ams; }
    int get_state_device() const { return state_device; }
    int get_state_local_task() const { return state_local_task; }
    int get_state_cloud_task() const { return state_cloud_task; }
    std::string get_state_dev_name() const { return state_dev_name; }

    void selected();
    void unselected();
    bool is_blocking_printing(MachineObject* obj_);
    void update_item(const DeviceItem* item);
};

std::vector<DeviceItem*> selected_machines(const std::vector<DeviceItem*>& dev_item_list, std::string search_text);

struct ObjState
{
    std::string     dev_id;
    std::string     state_dev_name;
    int             state_device{ 0 };
};

struct SortItem
{
    typedef std::function<bool(DeviceItem*, DeviceItem*)> SortCallBack;
    typedef std::function<bool(ObjState s1, ObjState s2) > SortMultiMachineCB;

    enum SortRule : uint8_t
    {
        SR_None = 0,
        SR_DEV_NAME = 1,
        SR_ONLINE,
        SR_PRINTABLE,
        SR_EN_AMS,
        SR_DEV_STATE,
        SR_LOCAL_TASK_STATE,
        SR_CLOUD_TASK_STATE,
        SR_SEND_TIME,
        SR_MACHINE_NAME,
        SR_MACHINE_STATE,
        SR_COUNT
    };

    SortRule rule{ SortRule::SR_None };
    bool big{ true };
    std::unordered_map<SortRule, SortCallBack> sort_map;
    SortMultiMachineCB cb;

    SortItem();
    SortItem(SortRule sr) { rule = sr; }

    SortCallBack get_call_back();
    void set_role(SortRule rule, bool big);
    void set_role(SortMultiMachineCB cb, SortRule rl, bool big);
    SortMultiMachineCB get_machine_call_back() const { return cb; }
};


wxDECLARE_EVENT(EVT_MULTI_DEVICE_SELECTED, wxCommandEvent);
wxDECLARE_EVENT(EVT_MULTI_DEVICE_SELECTED_FINHSH, wxCommandEvent);
wxDECLARE_EVENT(EVT_MULTI_DEVICE_VIEW, wxCommandEvent);
wxDECLARE_EVENT(EVT_MULTI_CLOUD_TASK_SELECTED, wxCommandEvent);
wxDECLARE_EVENT(EVT_MULTI_LOCAL_TASK_SELECTED, wxCommandEvent);
wxDECLARE_EVENT(EVT_MULTI_REFRESH, wxCommandEvent);

} // namespace GUI
} // namespace Slic3r

#endif

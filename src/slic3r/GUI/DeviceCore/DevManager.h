#pragma once
#include <mutex>
#include "libslic3r/CommonDefs.hpp"

#include "slic3r/Utils/json_diff.hpp"
#include <wx/string.h>
#include <wx/timer.h>

namespace Slic3r
{
//Previous definitions
struct BBLocalMachine;
class MachineObject;
class NetworkAgent;

namespace GUI {
class GUI_App;
};

class DeviceManagerRefresher;
class DeviceManager
{
    friend class GUI::GUI_App;
    friend class DeviceManagerRefresher;
private:
    NetworkAgent* m_agent{ nullptr };
    DeviceManagerRefresher* m_refresher{ nullptr };

    bool m_enable_mutil_machine = false;

    std::mutex listMutex;
    std::string selected_machine;                               /* dev_id */
    std::string local_selected_machine;                         /* dev_id */
    std::map<std::string, MachineObject*> localMachineList;     /* dev_id -> MachineObject*, localMachine SSDP   */
    std::map<std::string, MachineObject*> userMachineList;      /* dev_id -> MachineObject*  cloudMachine of User */

public:
    DeviceManager(NetworkAgent* agent = nullptr);
    ~DeviceManager();

public:
    NetworkAgent* get_agent() const { return m_agent; }
    void set_agent(NetworkAgent* agent) { m_agent = agent; }

    void start_refresher();
    void stop_refresher();

    MachineObject* get_selected_machine();
    bool set_selected_machine(std::string dev_id);

    // local machine
    void           set_local_selected_machine(std::string dev_id) { local_selected_machine = dev_id; };
    MachineObject* get_local_selected_machine() const { return get_local_machine(local_selected_machine); }

    // local machine list
    void erase_local_machine(std::string dev_id) { localMachineList.erase(dev_id); }
    std::map<std::string, MachineObject*> get_local_machinelist() const { return localMachineList; }
    MachineObject* get_local_machine(std::string dev_id) const
    {
        auto it = localMachineList.find(dev_id);
        return (it != localMachineList.end()) ? it->second : nullptr;
    }

    // user machine
    std::map<std::string, MachineObject*> get_user_machinelist() const { return userMachineList; }
    std::string get_first_online_user_machine() const;
    void erase_user_machine(std::string dev_id) { userMachineList.erase(dev_id); }
    void clean_user_info();

    void load_last_machine();
    void update_user_machine_list_info();
    void parse_user_print_info(std::string body);
    void reload_printer_settings();

    MachineObject* get_user_machine(std::string dev_id);

    // subscribe
    void add_user_subscribe();
    void del_user_subscribe();
    void subscribe_device_list(std::vector<std::string> dev_list);

    /* my machine*/
    MachineObject* get_my_machine(std::string dev_id);
    std::map<std::string, MachineObject*> get_my_machine_list();
    std::map<std::string, MachineObject*> get_my_cloud_machine_list();
    void modify_device_name(std::string dev_id, std::string dev_name);

    /* create machine or update machine properties */
    void on_machine_alive(std::string json_str);
    int query_bind_status(std::string& msg);

    // mutil-device
    void EnableMultiMachine(bool enable = true);
    bool IsMultiMachineEnabled() const { return m_enable_mutil_machine; }
    std::vector<std::string> subscribe_list_cache;//multiple machine subscribe list cache
    std::map<std::string, std::vector<std::string>> device_subseries;

private:
    void keep_alive();
    void check_pushing();

    void OnMachineBindStateChanged(MachineObject* obj, const std::string& new_state);
    void OnSelectedMachineLost();
    void OnSelectedMachineChanged(const std::string& pre_dev_id, const std::string& new_dev_id);


    /*TODO*/
public:
    // to remove
    MachineObject* insert_local_device(const BBLocalMachine& machine,
        std::string connection_type, std::string bind_state, std::string version,
        std::string access_code);
    static void update_local_machine(const MachineObject& m);
};

class DeviceManagerRefresher : public wxObject
{
    wxTimer* m_timer{ nullptr };
    int            m_timer_interval_msec = 1000;

    DeviceManager* m_manager{ nullptr };

public:
    DeviceManagerRefresher(DeviceManager* manger);
    ~DeviceManagerRefresher();

public:
    void Start() { m_timer->Start(m_timer_interval_msec); }
    void Stop() { m_timer->Stop(); }

protected:
    virtual void on_timer(wxTimerEvent& event);
};
};
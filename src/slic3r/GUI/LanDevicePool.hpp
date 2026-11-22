#ifndef slic3r_LanDevicePool_hpp_
#define slic3r_LanDevicePool_hpp_

#include "slic3r/Utils/bambu_networking.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {

class DeviceManager;
class DynamicPrintConfig;
class IPrinterAgent;
class MachineObject;
class NetworkAgent;
class PresetBundle;

// A LAN printer that a printer agent other than Bambu's drives, from a physical printer or a
// printer preset with a print host.
struct LanDevice
{
    std::string dev_id;       // MachineObject::dev_id_from_address(print_host, port), as the Device tab uses
    std::string name;         // the physical printer's or the preset's name
    std::string agent_id;     // printer_agent
    std::string printer_type; // the printer preset's model id, for the Multi-device tab's model checks
    std::string host;         // print_host without scheme or path
    std::string port;
    std::string username;
    std::string password;     // printhost_apikey, else printhost_password
    std::string ca_file;
    bool        use_ssl = false;

    bool operator==(const LanDevice& other) const;
    // Same agent, address and credentials: only the name or the model differ, so the connection stays
    bool same_connection(const LanDevice& other) const;
};

// The LAN device a printer config describes, or nothing: no print host, or an agent the
// Multi-device tab leaves to its own path (Bambu, or none).
std::optional<LanDevice> lan_device_from_config(const std::string& name, const DynamicPrintConfig& config, const std::string& printer_type);

// The LAN devices of the physical printers and the user's printer presets, one per dev_id.
std::vector<LanDevice> lan_devices_from_presets(const PresetBundle& bundle);

// What a print sent to `device` carries: the device's address and credentials, whatever the
// Multi-device tab filled in.
PrintParams lan_print_params(const LanDevice& device, PrintParams params);

// The LAN printers of the Multi-device tab. Each has its own MachineObject and, once the user picks it
// in "Manage devices", its own printer agent instance and connection, apart from the Device tab's
// live agent, so several printers report their status at once and a print goes to the printer it
// was sent to.
class LanDevicePool : public std::enable_shared_from_this<LanDevicePool>
{
public:
    using Task = std::function<void()>;
    // How the pool makes agents and reaches its threads. Empty members take the app's: the agent
    // factory, wxApp::CallAfter, and a worker thread of the pool's own. Tests swap them.
    struct Hooks
    {
        std::function<std::shared_ptr<IPrinterAgent>(const std::string& agent_id)> create_agent;
        std::function<void(Task)>                                                    run_on_main;
        std::function<void(Task)>                                                    run_in_background;
    };

    explicit LanDevicePool(DeviceManager* manager, Hooks hooks = {});
    ~LanDevicePool();
    LanDevicePool(const LanDevicePool&)            = delete;
    LanDevicePool& operator=(const LanDevicePool&) = delete;

    // UI thread: lists `devices`, connects the ones whose dev_id is in `selected` and disconnects the
    // others. A cheap diff: agents are created and connected in the background.
    void sync(const std::vector<LanDevice>& devices, const std::set<std::string>& selected);
    // UI thread: disconnects the devices whose agent is no longer registered as it was when created
    void prune();

    // UI thread
    std::map<std::string, MachineObject*> machines() const;
    bool                                  owns(const std::string& dev_id) const;

    // Any thread: sends a print to a device of the pool through its own agent. An error when the device
    // is not connected or was removed, never a fall back to another agent; nothing for any other dev_id.
    std::optional<int> start_print(const std::string& dev_id, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);

private:
    struct Connection;
    struct Entry
    {
        LanDevice                             device;
        std::unique_ptr<MachineObject>        machine;
        std::shared_ptr<Connection>           connection; // none until connected
        std::unique_ptr<NetworkAgent>         network;    // routes the machine's commands to the connection's agent
        uint64_t                              generation = 0; // of the current connect; other agents' callbacks are ignored
        bool                                  connecting = false;
        std::chrono::steady_clock::time_point connected_at; // the last connect attempt
        std::chrono::steady_clock::time_point last_seen;    // the last message or connect
    };

    void           disconnect(Entry& entry, std::vector<std::shared_ptr<Connection>>& released);
    void           release(std::shared_ptr<Connection> connection);
    void           on_connected(const std::string& dev_id, uint64_t generation, std::shared_ptr<Connection> connection);
    MachineObject* machine_seen(const std::string& dev_id, uint64_t generation);
    void           run_worker();

    DeviceManager*                m_manager;
    Hooks                         m_hooks;
    std::unique_ptr<NetworkAgent> m_offline; // the machines' agent while they have no connection: does nothing
    uint64_t                      m_generation = 0;
    mutable std::mutex            m_mutex; // guards m_entries and m_removed, which only the UI thread changes
    std::map<std::string, Entry>  m_entries;
    // Removed devices: the pages keep raw MachineObject pointers until they refresh, so the objects stay,
    // without an agent, and come back if the device does. Their dev_ids never fall back to another agent.
    std::map<std::string, std::unique_ptr<MachineObject>> m_removed;

    std::mutex              m_jobs_mutex;
    std::condition_variable m_jobs_cv;
    std::deque<Task>        m_jobs;
    bool                    m_stopping = false;
    std::thread             m_worker;
};

} // namespace Slic3r

#endif

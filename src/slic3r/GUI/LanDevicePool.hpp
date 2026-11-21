#ifndef slic3r_LanDevicePool_hpp_
#define slic3r_LanDevicePool_hpp_

#include "slic3r/Utils/bambu_networking.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
};

// The LAN device a printer config describes, or nothing: no print host, or an agent the
// Multi-device tab leaves to its own path (Bambu, or none).
std::optional<LanDevice> lan_device_from_config(const std::string& name, const DynamicPrintConfig& config, const std::string& printer_type);

// The LAN devices of the physical printers and the user's printer presets, one per dev_id.
std::vector<LanDevice> lan_devices_from_presets(const PresetBundle& bundle);

// What a print sent to `device` carries: the device's address and credentials, whatever the
// Multi-device tab filled in.
PrintParams lan_print_params(const LanDevice& device, PrintParams params);

// The LAN printers of the Multi-device tab. Each has its own printer agent instance, connection
// and MachineObject, apart from the Device tab's live agent, so several printers report their
// status at once and a print goes to the printer it was sent to.
class LanDevicePool : public std::enable_shared_from_this<LanDevicePool>
{
public:
    explicit LanDevicePool(DeviceManager* manager);
    ~LanDevicePool();

    // UI thread: connects new devices, and drops the ones no longer listed or whose agent is gone
    void sync(const std::vector<LanDevice>& devices);
    // UI thread: drops the devices whose agent is no longer registered as it was when created
    void prune();

    // UI thread
    std::map<std::string, MachineObject*> machines() const;
    bool owns(const std::string& dev_id) const;

    // Any thread: sends a print to the device through its own agent; nothing when the device
    // is not in the pool
    std::optional<int> start_print(const std::string& dev_id, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);

private:
    struct Entry
    {
        LanDevice                      device;
        std::string                    plugin_identifier; // of the agent when created; empty for a built-in
        std::shared_ptr<IPrinterAgent> agent;
        std::unique_ptr<NetworkAgent>  network;           // routes the MachineObject's commands to `agent`
        std::unique_ptr<MachineObject> machine;
    };

    std::unique_ptr<Entry> connect(const LanDevice& device);
    void                   disconnect(std::unique_ptr<Entry> entry);
    MachineObject*         machine_for(const std::string& dev_id) const;

    DeviceManager*                                  m_manager;
    mutable std::mutex                              m_mutex; // guards m_entries' map, not the MachineObjects
    std::map<std::string, std::unique_ptr<Entry>>   m_entries;
};

} // namespace Slic3r

#endif

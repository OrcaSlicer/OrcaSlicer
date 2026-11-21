#include "LanDevicePool.hpp"

#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

bool LanDevice::operator==(const LanDevice& other) const
{
    return dev_id == other.dev_id && name == other.name && agent_id == other.agent_id && printer_type == other.printer_type &&
           host == other.host && port == other.port && username == other.username && password == other.password &&
           ca_file == other.ca_file && use_ssl == other.use_ssl;
}

bool LanDevice::same_connection(const LanDevice& other) const
{
    return dev_id == other.dev_id && agent_id == other.agent_id && host == other.host && port == other.port &&
           username == other.username && password == other.password && ca_file == other.ca_file && use_ssl == other.use_ssl;
}

namespace {

// An offline device is connected again this often, when a page lists the devices: an agent gives up when
// the printer does not answer its first connection
constexpr auto RECONNECT_EVERY = std::chrono::seconds(60);

PrinterConnectionParams connection_params(const LanDevice& device)
{
    return {device.dev_id, device.host, device.port, device.username, device.password, device.use_ssl, device.ca_file};
}

std::string config_string(const DynamicPrintConfig& config, const char* key)
{
    return config.has(key) ? config.opt_string(key) : std::string();
}

// The model id of a printer model, as the vendor profiles list it
std::string model_id_of(const PresetBundle& bundle, const std::string& printer_model)
{
    for (const auto& vendor : bundle.vendors)
        for (const auto& model : vendor.second.models)
            if (model.name == printer_model)
                return model.model_id;
    return {};
}

} // namespace

std::optional<LanDevice> lan_device_from_config(const std::string& name, const DynamicPrintConfig& config, const std::string& printer_type)
{
    const std::string agent_id   = config_string(config, "printer_agent");
    const std::string print_host = config_string(config, "print_host");
    // Bambu printers keep their own path; "orca" is no printer agent at all
    if (agent_id.empty() || agent_id == BBL_PRINTER_AGENT_ID || agent_id == ORCA_PRINTER_AGENT_ID || print_host.empty())
        return std::nullopt;

    LanDevice device;
    device.name         = name;
    device.agent_id     = agent_id;
    device.printer_type = printer_type;
    device.port         = config_string(config, "printhost_port");
    device.dev_id       = MachineObject::dev_id_from_address(print_host, device.port);
    device.use_ssl      = boost::istarts_with(print_host, "https://");

    std::string host = print_host;
    if (const auto scheme = host.find("://"); scheme != std::string::npos)
        host = host.substr(scheme + 3);
    if (const auto slash = host.find('/'); slash != std::string::npos)
        host = host.substr(0, slash);
    device.host = host;

    device.username = config_string(config, "printhost_user");
    device.password = config_string(config, "printhost_apikey");
    if (device.password.empty())
        device.password = config_string(config, "printhost_password");
    device.ca_file = config_string(config, "printhost_cafile");
    return device;
}

std::vector<LanDevice> lan_devices_from_presets(const PresetBundle& bundle)
{
    std::vector<LanDevice> devices;
    std::set<std::string>  seen;
    const auto add = [&](const std::optional<LanDevice>& device) {
        if (device && seen.insert(device->dev_id).second)
            devices.push_back(*device);
    };

    // Physical printers first: they name the printer the user set up
    for (const PhysicalPrinter& printer : bundle.physical_printers) {
        std::string printer_type;
        if (!printer.preset_names.empty())
            if (const Preset* preset = bundle.printers.find_preset(*printer.preset_names.begin()))
                printer_type = model_id_of(bundle, config_string(preset->config, "printer_model"));
        add(lan_device_from_config(printer.name, printer.config, printer_type));
    }
    for (const Preset& preset : bundle.printers) {
        if (preset.is_default || preset.is_system)
            continue;
        add(lan_device_from_config(preset.name, preset.config, model_id_of(bundle, config_string(preset.config, "printer_model"))));
    }
    return devices;
}

PrintParams lan_print_params(const LanDevice& device, PrintParams params)
{
    params.dev_id          = device.dev_id;
    params.dev_ip          = device.host;
    params.dev_name        = device.name;
    params.username        = device.username;
    params.password        = device.password;
    params.connection_type = "lan";
    return params;
}

LanDevicePool::LanDevicePool(DeviceManager* manager) : m_manager(manager) {}

LanDevicePool::~LanDevicePool()
{
    std::map<std::string, std::unique_ptr<Entry>> entries;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        entries.swap(m_entries);
    }
    for (auto& [dev_id, entry] : entries)
        disconnect(std::move(entry));
}

std::unique_ptr<LanDevicePool::Entry> LanDevicePool::connect(const LanDevice& device)
{
    auto agent = NetworkAgentFactory::create_device_printer_agent(device.agent_id, data_dir());
    if (!agent) {
        BOOST_LOG_TRIVIAL(warning) << "LanDevicePool: no agent '" << device.agent_id << "' for " << device.name;
        return nullptr;
    }

    auto entry               = std::make_unique<Entry>();
    entry->device            = device;
    entry->plugin_identifier = NetworkAgentFactory::get_printer_agent_plugin_identifier(device.agent_id);
    entry->agent             = agent;
    entry->network           = std::make_unique<NetworkAgent>(nullptr, agent);
    entry->machine = std::make_unique<MachineObject>(m_manager, entry->network.get(), device.name, device.dev_id, device.host);
    entry->machine->dev_connection_type = "lan";
    entry->machine->printer_type        = device.printer_type;
    entry->machine->printer_agent_id    = device.agent_id;

    // Every callback reaches this device's MachineObject on the UI thread, if the pool and the device are still there
    const std::weak_ptr<LanDevicePool> pool    = weak_from_this();
    const auto                         on_main = [pool](std::function<void(LanDevicePool&)> fn) {
        GUI::wxGetApp().CallAfter([pool, fn = std::move(fn)] {
            if (auto self = pool.lock(); self && !GUI::wxGetApp().is_closing())
                fn(*self);
        });
    };
    agent->set_queue_on_main_fn([](std::function<void()> fn) { GUI::wxGetApp().CallAfter(std::move(fn)); });
    agent->set_on_local_message_fn([on_main](std::string dev_id, std::string msg) {
        on_main([dev_id, msg](LanDevicePool& self) {
            if (MachineObject* obj = self.machine_for(dev_id))
                obj->parse_json("lan", msg);
        });
    });
    agent->set_on_message_fn(nullptr);
    agent->set_on_local_connect_fn([on_main](int state, std::string dev_id, std::string) {
        on_main([state, dev_id](LanDevicePool& self) {
            if (MachineObject* obj = self.machine_for(dev_id)) {
                const bool ok = state == ConnectStatus::ConnectStatusOk;
                obj->set_online_state(ok);
                if (ok)
                    obj->command_request_push_all(true);
            }
        });
    });
    agent->set_on_printer_connected_fn([on_main](std::string dev_id) {
        on_main([dev_id](LanDevicePool& self) {
            if (MachineObject* obj = self.machine_for(dev_id))
                obj->set_online_state(true);
        });
    });

    entry->connected_at = std::chrono::steady_clock::now();
    const int result    = agent->connect_printer(connection_params(device));
    BOOST_LOG_TRIVIAL(info) << "LanDevicePool: connecting " << device.dev_id << " (" << device.agent_id << "): " << result;
    return entry;
}

void LanDevicePool::disconnect(std::unique_ptr<Entry> entry)
{
    if (!entry || !entry->agent)
        return;
    BOOST_LOG_TRIVIAL(info) << "LanDevicePool: disconnecting " << entry->device.dev_id;
    entry->agent->set_on_local_message_fn(nullptr);
    entry->agent->set_on_local_connect_fn(nullptr);
    entry->agent->set_on_printer_connected_fn(nullptr);
    entry->agent->disconnect_printer();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_retired.push_back(std::move(entry));
}

void LanDevicePool::sync(const std::vector<LanDevice>& devices)
{
    prune();
    std::vector<std::unique_ptr<Entry>> dropped;
    std::vector<LanDevice>              added;
    std::vector<std::pair<std::shared_ptr<IPrinterAgent>, LanDevice>> reconnect;
    const auto                          now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            Entry&     entry  = *it->second;
            const auto wanted = std::find_if(devices.begin(), devices.end(), [&](const LanDevice& d) { return d.dev_id == it->first; });
            // A changed device (another agent, address or password) is connected again
            if (wanted == devices.end() || !wanted->same_connection(entry.device)) {
                dropped.push_back(std::move(it->second));
                it = m_entries.erase(it);
                continue;
            }
            // A new name or model keeps the device, its connection and the pages' pointer to it
            if (!(*wanted == entry.device)) {
                entry.device = *wanted;
                entry.machine->set_dev_name(wanted->name);
                entry.machine->printer_type = wanted->printer_type;
            }
            if (!entry.machine->is_online() && now - entry.connected_at >= RECONNECT_EVERY) {
                entry.connected_at = now;
                reconnect.emplace_back(entry.agent, entry.device);
            }
            ++it;
        }
        for (const LanDevice& device : devices)
            if (m_entries.count(device.dev_id) == 0)
                added.push_back(device);
    }
    for (auto& entry : dropped)
        disconnect(std::move(entry));
    for (const auto& [agent, device] : reconnect)
        BOOST_LOG_TRIVIAL(info) << "LanDevicePool: reconnecting " << device.dev_id << ": " << agent->connect_printer(connection_params(device));
    for (const LanDevice& device : added) {
        if (auto entry = connect(device)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries.emplace(device.dev_id, std::move(entry));
        }
    }
}

void LanDevicePool::prune()
{
    std::vector<std::unique_ptr<Entry>> dropped;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            // A plugin unloaded, disabled or reloaded since: its agent instance must not be called again
            const bool registered = NetworkAgentFactory::is_printer_agent_registered(it->second->device.agent_id) &&
                                    NetworkAgentFactory::get_printer_agent_plugin_identifier(it->second->device.agent_id) ==
                                        it->second->plugin_identifier;
            if (registered) {
                ++it;
            } else {
                dropped.push_back(std::move(it->second));
                it = m_entries.erase(it);
            }
        }
    }
    for (auto& entry : dropped)
        disconnect(std::move(entry));
}

std::map<std::string, MachineObject*> LanDevicePool::machines() const
{
    std::lock_guard<std::mutex>           lock(m_mutex);
    std::map<std::string, MachineObject*> result;
    for (const auto& [dev_id, entry] : m_entries)
        result.emplace(dev_id, entry->machine.get());
    return result;
}

bool LanDevicePool::owns(const std::string& dev_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.count(dev_id) != 0;
}

MachineObject* LanDevicePool::machine_for(const std::string& dev_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto                  it = m_entries.find(dev_id);
    return it != m_entries.end() ? it->second->machine.get() : nullptr;
}

std::optional<int> LanDevicePool::start_print(const std::string& dev_id, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    std::shared_ptr<IPrinterAgent> agent;
    LanDevice                      device;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  it = m_entries.find(dev_id);
        if (it == m_entries.end()) {
            // Removed or changed since the print was queued: the live agent would send it to another printer
            const bool retired = std::any_of(m_retired.begin(), m_retired.end(), [&](const auto& e) { return e->device.dev_id == dev_id; });
            if (retired)
                BOOST_LOG_TRIVIAL(warning) << "LanDevicePool: " << dev_id << " is no longer a Multi-device printer; not printing";
            return retired ? std::optional<int>(BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED) : std::nullopt;
        }
        agent  = it->second->agent;
        device = it->second->device;
    }
    BOOST_LOG_TRIVIAL(info) << "LanDevicePool: printing " << params.filename << " on " << dev_id << " through " << device.agent_id;
    return agent->start_local_print(lan_print_params(device, std::move(params)), std::move(update_fn), std::move(cancel_fn));
}

} // namespace Slic3r

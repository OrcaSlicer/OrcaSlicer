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
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
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
// A connected device that sends nothing this long is offline. Moonraker re-sends its status every 10 s
// even when nothing changes.
constexpr auto STALE_AFTER = std::chrono::seconds(90);

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

// One printer's agent instance. The last holder, the worker thread or a sending thread, never the UI
// thread, disconnects it: a disconnect waits for the agent's threads.
struct LanDevicePool::Connection
{
    std::shared_ptr<IPrinterAgent> agent;
    std::string                    plugin_identifier; // of the agent when created; empty for a built-in

    Connection(std::shared_ptr<IPrinterAgent> agent, std::string plugin_identifier)
        : agent(std::move(agent)), plugin_identifier(std::move(plugin_identifier))
    {}
    ~Connection()
    {
        agent->set_on_local_message_fn(nullptr);
        agent->set_on_local_connect_fn(nullptr);
        agent->set_on_printer_connected_fn(nullptr);
        agent->disconnect_printer();
    }
    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;
};

LanDevicePool::LanDevicePool(DeviceManager* manager, Hooks hooks)
    : m_manager(manager), m_hooks(std::move(hooks)), m_offline(std::make_unique<NetworkAgent>(nullptr, nullptr))
{
    if (!m_hooks.create_agent)
        m_hooks.create_agent = [](const std::string& agent_id) { return NetworkAgentFactory::create_device_printer_agent(agent_id, data_dir()); };
    if (!m_hooks.run_on_main)
        m_hooks.run_on_main = [](Task task) {
            GUI::wxGetApp().CallAfter([task = std::move(task)] {
                if (!GUI::wxGetApp().is_closing())
                    task();
            });
        };
    if (!m_hooks.run_in_background)
        m_hooks.run_in_background = [this](Task task) {
            std::lock_guard<std::mutex> lock(m_jobs_mutex);
            if (m_stopping)
                return;
            m_jobs.push_back(std::move(task));
            if (!m_worker.joinable())
                m_worker = std::thread([this] { run_worker(); });
            m_jobs_cv.notify_one();
        };
}

LanDevicePool::~LanDevicePool()
{
    {
        std::lock_guard<std::mutex> lock(m_jobs_mutex);
        m_stopping = true;
    }
    m_jobs_cv.notify_one();
    if (m_worker.joinable())
        m_worker.join();
    // The app is closing: the jobs left, and the connections, go with the members
}

void LanDevicePool::run_worker()
{
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_jobs_mutex);
            m_jobs_cv.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
            if (m_stopping)
                return;
            task = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        task();
    }
}

void LanDevicePool::release(std::shared_ptr<Connection> connection)
{
    if (connection)
        m_hooks.run_in_background([connection = std::move(connection)]() mutable { connection.reset(); });
}

void LanDevicePool::disconnect(Entry& entry, std::vector<std::shared_ptr<Connection>>& released)
{
    if (entry.connection)
        BOOST_LOG_TRIVIAL(info) << "LanDevicePool: disconnecting " << entry.device.dev_id;
    entry.machine->set_online_state(false);
    entry.machine->set_agent(m_offline.get());
    entry.network.reset();
    released.push_back(std::move(entry.connection));
    entry.generation   = 0; // a connect still in flight, and the old agent's callbacks, are ignored
    entry.connecting   = false;
    entry.connected_at = {};
}

void LanDevicePool::sync(const std::vector<LanDevice>& devices, const std::set<std::string>& selected)
{
    prune();
    const auto                                                         now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<Connection>>                           released;
    std::vector<std::pair<LanDevice, uint64_t>>                        connect;
    std::vector<std::pair<std::shared_ptr<Connection>, LanDevice>>     reconnect;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            const bool listed = std::any_of(devices.begin(), devices.end(), [&](const LanDevice& d) { return d.dev_id == it->first; });
            if (listed) {
                ++it;
                continue;
            }
            disconnect(it->second, released);
            m_removed[it->first] = std::move(it->second.machine);
            it = m_entries.erase(it);
        }

        for (const LanDevice& device : devices) {
            auto [it, added] = m_entries.try_emplace(device.dev_id);
            Entry& entry     = it->second;
            if (added) {
                if (auto removed = m_removed.find(device.dev_id); removed != m_removed.end()) {
                    entry.machine = std::move(removed->second);
                    m_removed.erase(removed);
                } else {
                    entry.machine = std::make_unique<MachineObject>(m_manager, m_offline.get(), device.name, device.dev_id, device.host);
                    entry.machine->dev_connection_type = "lan";
                }
                entry.device = device;
            } else if (!device.same_connection(entry.device)) {
                // Another agent, address or password: connected again
                disconnect(entry, released);
            }
            // A new name or model keeps the device, its connection and the pages' pointer to it
            entry.device = device;
            entry.machine->set_dev_name(device.name);
            entry.machine->printer_type     = device.printer_type;
            entry.machine->printer_agent_id = device.agent_id;

            // Only the printers picked in "Manage devices" are connected
            if (selected.count(device.dev_id) == 0) {
                if (entry.connection || entry.connecting)
                    disconnect(entry, released);
                continue;
            }
            // An agent that stops reporting, without saying it lost the printer, counts as offline
            if (entry.connection && entry.machine->is_online() && now - entry.last_seen >= STALE_AFTER) {
                BOOST_LOG_TRIVIAL(info) << "LanDevicePool: no message from " << device.dev_id << ", offline";
                entry.machine->set_online_state(false);
            }
            const bool retry = entry.connected_at == std::chrono::steady_clock::time_point{} || now - entry.connected_at >= RECONNECT_EVERY;
            if (!entry.connection && !entry.connecting && retry) {
                entry.connecting   = true;
                entry.generation   = ++m_generation;
                entry.connected_at = now;
                connect.emplace_back(device, entry.generation);
            } else if (entry.connection && !entry.machine->is_online() && retry) {
                entry.connected_at = now;
                reconnect.emplace_back(entry.connection, device);
            }
        }
    }

    for (auto& connection : released)
        release(std::move(connection));
    for (auto& [connection, device] : reconnect)
        m_hooks.run_in_background([connection = std::move(connection), device = std::move(device)] {
            const int result = connection->agent->connect_printer(connection_params(device));
            BOOST_LOG_TRIVIAL(info) << "LanDevicePool: reconnecting " << device.dev_id << ": " << result;
        });

    // In the background: the agent factory may run a plugin's Python, and connecting may wait on the
    // network. The job never holds the pool, whose destructor waits for it.
    for (auto& [device, generation] : connect) {
        m_hooks.run_in_background([pool = weak_from_this(), device = std::move(device), generation = generation,
                                   create_agent = m_hooks.create_agent, run_on_main = m_hooks.run_on_main] {
            // Read before the agent is made: a plugin reloaded in between is then pruned
            const std::string plugin_identifier = NetworkAgentFactory::get_printer_agent_plugin_identifier(device.agent_id);
            std::shared_ptr<Connection> connection;
            if (auto agent = create_agent(device.agent_id)) {
                connection = std::make_shared<Connection>(agent, plugin_identifier);

                // Each callback reaches this device's MachineObject on the UI thread, by the pool's dev_id: the
                // agent serves this one printer, whatever id it reports
                const auto on_main = [pool, dev_id = device.dev_id, generation, run_on_main](std::function<void(MachineObject&)> fn) {
                    run_on_main([pool, dev_id, generation, fn = std::move(fn)] {
                        if (auto self = pool.lock())
                            if (MachineObject* obj = self->machine_seen(dev_id, generation))
                                fn(*obj);
                    });
                };
                agent->set_queue_on_main_fn([run_on_main](std::function<void()> fn) { run_on_main(std::move(fn)); });
                agent->set_on_local_message_fn([on_main](const std::string&, const std::string& msg) {
                    on_main([msg](MachineObject& obj) { obj.parse_json("lan", msg); });
                });
                agent->set_on_message_fn(nullptr);
                agent->set_on_local_connect_fn([on_main](int state, const std::string&, const std::string&) {
                    on_main([state](MachineObject& obj) {
                        const bool ok = state == ConnectStatus::ConnectStatusOk;
                        obj.set_online_state(ok);
                        if (ok)
                            obj.command_request_push_all(true);
                    });
                });
                agent->set_on_printer_connected_fn([on_main](const std::string&) {
                    on_main([](MachineObject& obj) { obj.set_online_state(true); });
                });
                const int result = agent->connect_printer(connection_params(device));
                BOOST_LOG_TRIVIAL(info) << "LanDevicePool: connecting " << device.dev_id << " (" << device.agent_id << "): " << result;
            } else {
                BOOST_LOG_TRIVIAL(warning) << "LanDevicePool: no agent '" << device.agent_id << "' for " << device.name;
            }
            run_on_main([pool, dev_id = device.dev_id, generation, connection]() mutable {
                if (auto self = pool.lock())
                    self->on_connected(dev_id, generation, std::move(connection));
            });
        });
    }
}

void LanDevicePool::on_connected(const std::string& dev_id, uint64_t generation, std::shared_ptr<Connection> connection)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  it = m_entries.find(dev_id);
        if (it != m_entries.end() && it->second.connecting && it->second.generation == generation) {
            Entry& entry     = it->second;
            entry.connecting = false;
            if (!connection)
                return; // tried again after RECONNECT_EVERY
            entry.network = std::make_unique<NetworkAgent>(nullptr, connection->agent);
            entry.machine->set_agent(entry.network.get());
            entry.connection = std::move(connection);
            entry.last_seen  = std::chrono::steady_clock::now();
            return;
        }
    }
    // Removed, changed or unpicked while connecting
    release(std::move(connection));
}

void LanDevicePool::prune()
{
    std::vector<std::shared_ptr<Connection>> released;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [dev_id, entry] : m_entries) {
            if (!entry.connection)
                continue;
            // A plugin unloaded, disabled or reloaded since: its agent instance must not be called again
            const std::string& agent_id = entry.device.agent_id;
            const bool registered = NetworkAgentFactory::is_printer_agent_registered(agent_id) &&
                                    NetworkAgentFactory::get_printer_agent_plugin_identifier(agent_id) == entry.connection->plugin_identifier;
            if (!registered)
                disconnect(entry, released);
        }
    }
    for (auto& connection : released)
        release(std::move(connection));
}

std::map<std::string, MachineObject*> LanDevicePool::machines() const
{
    std::lock_guard<std::mutex>           lock(m_mutex);
    std::map<std::string, MachineObject*> result;
    for (const auto& [dev_id, entry] : m_entries)
        result.emplace(dev_id, entry.machine.get());
    return result;
}

bool LanDevicePool::owns(const std::string& dev_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.count(dev_id) != 0;
}

MachineObject* LanDevicePool::machine_seen(const std::string& dev_id, uint64_t generation)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto                  it = m_entries.find(dev_id);
    if (it == m_entries.end() || it->second.generation != generation)
        return nullptr;
    it->second.last_seen = std::chrono::steady_clock::now();
    return it->second.machine.get();
}

std::optional<int> LanDevicePool::start_print(const std::string& dev_id, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    // Held until the print returns: a device dropped meanwhile disconnects afterwards, on this thread
    std::shared_ptr<Connection> connection;
    LanDevice                   device;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  it = m_entries.find(dev_id);
        if (it == m_entries.end() && m_removed.count(dev_id) == 0)
            return std::nullopt; // not a printer of the pool: the live agent's
        if (it == m_entries.end() || !it->second.connection) {
            // The live agent would send it to another printer
            BOOST_LOG_TRIVIAL(warning) << "LanDevicePool: " << dev_id << " is not connected, or no longer a Multi-device printer; not printing";
            return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
        }
        connection = it->second.connection;
        device     = it->second.device;
    }
    BOOST_LOG_TRIVIAL(info) << "LanDevicePool: printing " << params.filename << " on " << dev_id << " through " << device.agent_id;
    return connection->agent->start_local_print(lan_print_params(device, std::move(params)), std::move(update_fn), std::move(cancel_fn));
}

} // namespace Slic3r

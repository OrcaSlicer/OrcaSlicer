#include "NetworkAgentFactory.hpp"
#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"
#include "BBLPrinterAgent.hpp"
#include "OrcaPrinterAgent.hpp"
#include "QidiPrinterAgent.hpp"
#include "SnapmakerPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/pluginTypes/printerAgent/PrinterAgentPluginCapability.hpp"
#include "CrealityPrintAgent.hpp"
#include <algorithm>
#include <boost/log/trivial.hpp>
#include <chrono>
#include <map>
#include <mutex>
#include <utility>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

namespace Slic3r {
namespace {

static std::mutex s_registry_mutex;

struct PythonPrinterAgentRegistrationToken {};

static std::map<std::pair<std::string, std::string>, std::shared_ptr<PythonPrinterAgentRegistrationToken>>
    s_python_printer_agent_registration_tokens;

std::map<std::string, PrinterAgentInfo>& get_printer_agents()
{
    static std::map<std::string, PrinterAgentInfo> agents;
    return agents;
}

std::map<std::string, std::shared_ptr<IPrinterAgent>>& get_printer_agent_cache()
{
    static std::map<std::string, std::shared_ptr<IPrinterAgent>> cache;
    return cache;
}

std::map<std::pair<std::string, std::string>, std::string>& get_python_printer_agent_ids()
{
    static std::map<std::pair<std::string, std::string>, std::string> capability_to_agent_id;
    return capability_to_agent_id;
}

std::string plugin_printer_agent_full_ref(const PluginDescriptor& descriptor, const std::string& capability_name)
{
    const std::string identity = descriptor.is_cloud_plugin() ? descriptor.name : descriptor.plugin_key;
    return identity + ';' + descriptor.cloud_uuid() + ';' + capability_name;
}

// Helper to register a printer agent type with the standard factory pattern.
// AgentTypes that take a log_dir constructor arg use the default; BBLPrinterAgent
// (no log_dir) is registered separately.
template<typename T> void register_agent()
{
    auto info = T::get_agent_info_static();
    NetworkAgentFactory::register_printer_agent(info.id, info.name,
                                                [](std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                                   const std::string& log_dir) -> std::shared_ptr<IPrinterAgent> {
                                                    auto agent = std::make_shared<T>(log_dir);
                                                    if (cloud_agent)
                                                        agent->set_cloud_agent(cloud_agent);
                                                    return agent;
                                                });
}

} // anonymous namespace

bool NetworkAgentFactory::register_printer_agent(const std::string& id, const std::string& display_name, PrinterAgentFactory factory)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();
    return agents.emplace(id, PrinterAgentInfo(id, display_name, std::move(factory))).second;
}

bool NetworkAgentFactory::is_printer_agent_registered(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();
    return agents.find(id) != agents.end();
}

const PrinterAgentInfo* NetworkAgentFactory::get_printer_agent_info(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();
    auto it      = agents.find(id);
    return (it != agents.end()) ? &it->second : nullptr;
}

std::string NetworkAgentFactory::get_printer_agent_plugin_identifier(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();
    auto it      = agents.find(id);
    return (it != agents.end() && it->second.is_plugin()) ? it->second.plugin_identifier : std::string();
}

std::vector<PrinterAgentInfo> NetworkAgentFactory::get_registered_printer_agents()
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& agents = get_printer_agents();
    std::vector<PrinterAgentInfo> result;
    result.reserve(agents.size());

    for (const auto& pair : agents) {
        if (!pair.second.is_plugin())
            result.push_back(pair.second);
    }

    for (const auto& pair : agents) {
        if (pair.second.is_plugin())
            result.push_back(pair.second);
    }

    return result;
}

std::shared_ptr<IPrinterAgent> NetworkAgentFactory::create_printer_agent_by_id(const std::string& id,
                                                                               std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                                                               const std::string& log_dir)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);

    // Check cache first
    auto& cache   = get_printer_agent_cache();
    auto cache_it = cache.find(id);
    if (cache_it != cache.end()) {
        BOOST_LOG_TRIVIAL(info) << "Reusing cached printer agent: " << id;
        if (cloud_agent)
            cache_it->second->set_cloud_agent(cloud_agent);
        return cache_it->second;
    }

    // Not cached — create via factory
    auto& agents = get_printer_agents();
    auto it      = agents.find(id);

    if (it == agents.end()) {
        BOOST_LOG_TRIVIAL(warning) << "Unknown printer agent ID: " << id;
        return nullptr;
    }

    auto agent = it->second.factory(cloud_agent, log_dir);
    if (agent) {
        BOOST_LOG_TRIVIAL(info) << "Created and cached printer agent: " << id;
        cache[id] = agent;
    }
    return agent;
}

void NetworkAgentFactory::clear_printer_agent_cache()
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto& cache = get_printer_agent_cache();
    for (auto& pair : cache) {
        if (pair.second)
            pair.second->disconnect_printer();
    }
    cache.clear();
    BOOST_LOG_TRIVIAL(info) << "Printer agent cache cleared";
}

void NetworkAgentFactory::register_all_agents()
{
    register_agent<OrcaPrinterAgent>();
    register_agent<QidiPrinterAgent>();
    register_agent<SnapmakerPrinterAgent>();
    register_agent<CrealityPrintAgent>();  // Must come BEFORE MoonrakerPrinterAgent —
                                            // CrealityPrintAgent extends Moonraker behaviour
                                            // for K-series boards with CFS support.
    register_agent<MoonrakerPrinterAgent>();

    // Keep BBL as a built-in option. Python printer-agent plugins with the
    // same AgentInfo ID are listed separately under the plugin registry key.
    {
        auto info = BBLPrinterAgent::get_agent_info_static();
        register_printer_agent(info.id, info.name,
                               [](std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                  const std::string& /*log_dir*/) -> std::shared_ptr<IPrinterAgent> {
                                   auto agent = std::make_shared<BBLPrinterAgent>();
                                   if (cloud_agent)
                                       agent->set_cloud_agent(cloud_agent);
                                   return agent;
                               });
    }
}

std::unique_ptr<NetworkAgent> create_agent_from_config(const std::string& log_dir, AppConfig* app_config)
{
    if (!app_config)
        return std::make_unique<NetworkAgent>(nullptr, nullptr);

    // Always create Orca cloud agent as the primary provider
    auto cloud_agent = NetworkAgentFactory::create_cloud_agent(ORCA_CLOUD_PROVIDER, log_dir);
    if (!cloud_agent) {
        BOOST_LOG_TRIVIAL(error) << "Failed to create cloud agent";
    }

    auto agent = std::make_unique<NetworkAgent>(std::move(cloud_agent), nullptr);

    if (agent) {
        // create orca cloud agent first
        auto* orca_cloud = dynamic_cast<OrcaCloudServiceAgent*>(agent->get_cloud_agent().get());
        if (orca_cloud) {
            orca_cloud->configure_urls(app_config);
        }

        // Initialize third-party cloud agents from config
        auto providers = app_config->get_cloud_providers();
        for (const auto& provider : providers) {
            if (provider == ORCA_CLOUD_PROVIDER)
                continue; // Primary agent already created above
            auto third_party_agent = NetworkAgentFactory::create_cloud_agent(provider, log_dir);
            if (third_party_agent) {
                agent->add_cloud_agent(provider, std::move(third_party_agent));
                BOOST_LOG_TRIVIAL(info) << "Initialized third-party cloud agent: " << provider;
            }
        }
    }

    return agent;
}

void NetworkAgentFactory::register_python_plugin(const std::string& plugin_key)
{
    for (const auto& capability : PluginManager::instance().get_plugin_capabilities(plugin_key, PluginCapabilityType::PrinterConnection,
                                                                                   /*only_enabled=*/true))
        register_python_printer_agent(plugin_key, capability->name());
}

void NetworkAgentFactory::register_python_printer_agent(const std::string& plugin_key, const std::string& capability_name)
{
    PluginManager& plugin_manager = PluginManager::instance();

    auto cap = plugin_manager.get_plugin_capability(plugin_key, capability_name, PluginCapabilityType::PrinterConnection,
                                                     /*only_enabled=*/false);
    if (!cap) {
        BOOST_LOG_TRIVIAL(warning) << "Printer-agent capability '" << capability_name << "' not found for plugin '" << plugin_key << "'";
        return;
    }

    PluginDescriptor descriptor;
    if (!plugin_manager.try_get_plugin_descriptor(plugin_key, descriptor)) {
        BOOST_LOG_TRIVIAL(warning) << "Could not find descriptor for plugin key: '" << plugin_key;
        return;
    }
    if (!cap->is_enabled()) {
        BOOST_LOG_TRIVIAL(warning) << "Printer-agent capability '" << capability_name << "' is disabled for plugin '" << plugin_key << "'";
        return;
    }

    auto plugin = std::dynamic_pointer_cast<PrinterAgentPluginCapability>(cap);

    if (!plugin) {
        BOOST_LOG_TRIVIAL(warning) << "Loaded plugin capability '" << capability_name << "' is not a printer-agent plugin";
        return;
    }

    auto info = plugin->get_agent_info();

    if (info.id.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Printer-agent plugin '" << capability_name << "' returned an empty agent ID";
        return;
    }

    const auto capability_key = std::make_pair(plugin_key, capability_name);
    std::shared_ptr<PythonPrinterAgentRegistrationToken> registration_token;
    {
        std::lock_guard<std::mutex> lock(s_registry_mutex);
        auto token_it = std::find_if(s_python_printer_agent_registration_tokens.begin(), s_python_printer_agent_registration_tokens.end(),
                                     [&plugin_key, &capability_name](const auto& entry) {
                                         return entry.first.first == plugin_key && entry.first.second == capability_name;
                                     });
        if (token_it == s_python_printer_agent_registration_tokens.end()) {
            registration_token = std::make_shared<PythonPrinterAgentRegistrationToken>();
            s_python_printer_agent_registration_tokens.emplace(capability_key, registration_token);
        } else {
            registration_token = token_it->second;
        }
    }

    const std::string plugin_full_ref = plugin_printer_agent_full_ref(descriptor, capability_name);

    std::weak_ptr<PrinterAgentPluginCapability> weak_plugin = plugin;
    PrinterAgentFactory factory                             = [weak_plugin](std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                                const std::string& /*log_dir*/) -> std::shared_ptr<IPrinterAgent> {
        // The capability implements IPrinterAgent directly, so it is handed out as the
        // live agent (no adapter); the Python plugin services the calls.
        auto plugin = weak_plugin.lock();
        if (!plugin)
            return nullptr;

        if (cloud_agent)
            plugin->set_cloud_agent(cloud_agent);
        return plugin;
    };

    // Python work above may allow disable/unload/reload to replace this capability.
    // Re-resolve without holding the registry mutex to avoid lock inversion and reentrancy.
    // only_enabled defaults to true here, so a capability that changed identity (reload) or was
    // merely disabled both surface the same way: current_cap no longer equals cap.
    auto current_cap = plugin_manager.get_plugin_capability(plugin_key, capability_name, PluginCapabilityType::PrinterConnection);
    if (current_cap != cap) {
        BOOST_LOG_TRIVIAL(debug) << "Printer-agent capability '" << capability_name << "' from plugin '" << plugin_key
                                 << "' changed or was disabled during registration";
        return;
    }

    std::shared_ptr<IPrinterAgent> cached_agent;

    {
        std::lock_guard<std::mutex> lock(s_registry_mutex);

        auto token_it = std::find_if(s_python_printer_agent_registration_tokens.begin(), s_python_printer_agent_registration_tokens.end(),
                                     [&plugin_key, &capability_name](const auto& entry) {
                                         return entry.first.first == plugin_key && entry.first.second == capability_name;
                                     });
        const bool cap_still_enabled = cap->is_enabled();
        if (token_it == s_python_printer_agent_registration_tokens.end() || token_it->second != registration_token ||
            !cap_still_enabled) {
            BOOST_LOG_TRIVIAL(debug) << "Printer-agent capability '" << capability_name << "' from plugin '" << plugin_key
                                     << "' was disabled before registry commit";
            return;
        }

        auto& python_agent_ids = get_python_printer_agent_ids();
        for (const auto& pair : python_agent_ids) {
            if (pair.first != capability_key && pair.second == info.id) {
                BOOST_LOG_TRIVIAL(warning) << "Printer-agent plugin '" << capability_name << "' uses duplicate agent ID '" << info.id
                                           << "' already registered by capability '" << pair.first.second << "' from plugin '"
                                           << pair.first.first << "'";
                return;
            }
        }

        auto plugin_it = python_agent_ids.find(capability_key);
        if (plugin_it != python_agent_ids.end() && plugin_it->second != info.id) {
            const std::string old_agent_id = plugin_it->second;
            get_printer_agents().erase(old_agent_id);

            auto& cache   = get_printer_agent_cache();
            auto cache_it = cache.find(old_agent_id);
            if (cache_it != cache.end()) {
                cached_agent = std::move(cache_it->second);
                cache.erase(cache_it);
            }
        }

        auto& agents = get_printer_agents();
        auto agent_it = agents.find(info.id);
        if (agent_it != agents.end() && agent_it->second.plugin_identifier != plugin_full_ref) {
            BOOST_LOG_TRIVIAL(warning) << "Printer-agent plugin '" << capability_name << "' uses agent ID '" << info.id
                                       << "' already registered by '" << agent_it->second.display_name << "'";
            return;
        }

        agents.insert_or_assign(info.id, PrinterAgentInfo(info.id, info.name, plugin_full_ref, std::move(factory)));
        python_agent_ids[capability_key] = info.id;

        auto& cache   = get_printer_agent_cache();
        auto cache_it = cache.find(info.id);
        if (cache_it != cache.end()) {
            cached_agent = std::move(cache_it->second);
            cache.erase(cache_it);
        }
    }

    if (cached_agent)
        cached_agent->disconnect_printer();

    BOOST_LOG_TRIVIAL(info) << "Registered printer-agent plugin '" << capability_name << "' with agent ID '" << info.id
                            << "' and plugin ref '" << plugin_full_ref << "'";
}

void NetworkAgentFactory::deregister_python_plugin(const std::string& plugin_key)
{
    std::vector<std::string> capability_names;

    {
        std::lock_guard<std::mutex> lock(s_registry_mutex);
        for (const auto& [capability_key, token] : s_python_printer_agent_registration_tokens) {
            (void) token;
            if (capability_key.first == plugin_key)
                capability_names.push_back(capability_key.second);
        }
        for (const auto& [capability_key, agent_id] : get_python_printer_agent_ids()) {
            (void) agent_id;
            if (capability_key.first == plugin_key &&
                std::find(capability_names.begin(), capability_names.end(), capability_key.second) == capability_names.end())
                capability_names.push_back(capability_key.second);
        }
    }

    for (const std::string& capability_name : capability_names)
        deregister_python_printer_agent(plugin_key, capability_name);

    if (capability_names.empty())
        BOOST_LOG_TRIVIAL(debug) << "Python printer-agent plugin '" << plugin_key << "' has no registered capabilities";
}

void NetworkAgentFactory::deregister_python_printer_agent(const std::string& plugin_key, const std::string& capability_name)
{
    std::string agent_id;
    std::shared_ptr<IPrinterAgent> cached_agent;

    {
        std::lock_guard<std::mutex> lock(s_registry_mutex);

        auto token_it = std::find_if(s_python_printer_agent_registration_tokens.begin(),
                                     s_python_printer_agent_registration_tokens.end(),
                                     [&plugin_key, &capability_name](const auto& entry) {
                                         return entry.first.first == plugin_key &&
                                                entry.first.second == capability_name;
                                     });
        if (token_it != s_python_printer_agent_registration_tokens.end())
            s_python_printer_agent_registration_tokens.erase(token_it);

        auto& python_agent_ids = get_python_printer_agent_ids();
        auto id_it = std::find_if(python_agent_ids.begin(), python_agent_ids.end(),
                                  [&plugin_key, &capability_name](const auto& entry) {
                                      return entry.first.first == plugin_key &&
                                             entry.first.second == capability_name;
                                  });
        if (id_it == python_agent_ids.end()) {
            BOOST_LOG_TRIVIAL(debug) << "Python printer-agent capability '" << capability_name << "' from plugin '"
                                     << plugin_key << "' is not registered";
            return;
        }

        agent_id = id_it->second;
        python_agent_ids.erase(id_it);

        auto& cache = get_printer_agent_cache();
        auto cache_it = cache.find(agent_id);
        if (cache_it != cache.end()) {
            cached_agent = std::move(cache_it->second);
            cache.erase(cache_it);
        }

        get_printer_agents().erase(agent_id);
    }

    if (cached_agent)
        cached_agent->disconnect_printer();

    // The GUI may still own the active capability separately from the factory cache. Clear that
    // handle before the plugin module is torn down, otherwise NetworkAgent can retain a Python
    // object whose implementation is about to be unloaded.
    if (!agent_id.empty() && wxTheApp) {
        NetworkAgent* network_agent = GUI::wxGetApp().getAgent();
        if (network_agent) {
            const auto active_agent = network_agent->get_printer_agent();
            if (active_agent && active_agent->get_agent_info().id == agent_id)
                network_agent->set_printer_agent(nullptr);
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Deregistered Python printer-agent capability '" << capability_name << "' from plugin '"
                            << plugin_key << "' with agent ID '" << agent_id << "'";
}

bool NetworkAgentFactory::is_current_printer_agent_plugin()
{
    auto* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (!preset_bundle)
        return false;

    std::string agent_key = ORCA_PRINTER_AGENT_ID;
    if (preset_bundle->is_bbl_vendor())
        agent_key = BBL_PRINTER_AGENT_ID;

    const auto& cfg = preset_bundle->printers.get_edited_preset().config;
    if (cfg.has("printer_agent")) {
        const std::string& value = cfg.option<ConfigOptionString>("printer_agent")->value;
        if (!value.empty())
            agent_key = value;
    }

    const PrinterAgentInfo* info = get_printer_agent_info(agent_key);
    return info && info->is_plugin();
}

} // namespace Slic3r

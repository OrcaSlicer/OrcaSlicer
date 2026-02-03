#include "NetworkAgentFactory.hpp"
#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"
#include "BBLPrinterAgent.hpp"
#include "OrcaPrinterAgent.hpp"
#include "QidiPrinterAgent.hpp"
#include "SnapmakerPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include <boost/log/trivial.hpp>
#include <map>
#include <mutex>

namespace Slic3r {
namespace {

static std::mutex s_registry_mutex;

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

// Helper to register a printer agent type with the standard factory pattern.
// AgentTypes that take a log_dir constructor arg use the default; BBLPrinterAgent
// (no log_dir) is registered separately.
template<typename T>
void register_agent()
{
    auto info = T::get_agent_info_static();
    NetworkAgentFactory::register_printer_agent(
        info.id, info.name,
        [](std::shared_ptr<ICloudServiceAgent> cloud_agent,
           const std::string&                  log_dir) -> std::shared_ptr<IPrinterAgent> {
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
    auto&                       agents = get_printer_agents();
    return agents.emplace(id, PrinterAgentInfo(id, display_name, std::move(factory))).second;
}

bool NetworkAgentFactory::is_printer_agent_registered(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto&                       agents = get_printer_agents();
    return agents.find(id) != agents.end();
}

const PrinterAgentInfo* NetworkAgentFactory::get_printer_agent_info(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto&                       agents = get_printer_agents();
    auto                        it     = agents.find(id);
    return (it != agents.end()) ? &it->second : nullptr;
}

std::vector<PrinterAgentInfo> NetworkAgentFactory::get_registered_printer_agents()
{
    std::lock_guard<std::mutex>   lock(s_registry_mutex);
    auto&                         agents = get_printer_agents();
    std::vector<PrinterAgentInfo> result;
    result.reserve(agents.size());

    for (const auto& pair : agents) {
        result.push_back(pair.second);
    }

    return result;
}

std::shared_ptr<IPrinterAgent> NetworkAgentFactory::create_printer_agent_by_id(const std::string&                  id,
                                                                               std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                                                               const std::string&                  log_dir)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);

    // Check cache first
    auto& cache    = get_printer_agent_cache();
    auto  cache_it = cache.find(id);
    if (cache_it != cache.end()) {
        BOOST_LOG_TRIVIAL(info) << "Reusing cached printer agent: " << id;
        if (cloud_agent)
            cache_it->second->set_cloud_agent(cloud_agent);
        return cache_it->second;
    }

    // Not cached — create via factory
    auto& agents = get_printer_agents();
    auto  it     = agents.find(id);

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
    auto&                       cache = get_printer_agent_cache();
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
    register_agent<MoonrakerPrinterAgent>();

    // BBLPrinterAgent takes no constructor args, so register manually
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

    // Determine cloud provider from config
    bool use_orca_cloud = app_config->get_bool("use_orca_cloud");

    // Create cloud agent
    std::shared_ptr<ICloudServiceAgent> cloud_agent;
    if (use_orca_cloud || app_config->get_bool("installed_networking")) {
        CloudAgentProvider provider = use_orca_cloud ? CloudAgentProvider::Orca : CloudAgentProvider::BBL;
        cloud_agent                 = NetworkAgentFactory::create_cloud_agent(provider, log_dir);
        if (!cloud_agent) {
            BOOST_LOG_TRIVIAL(error) << "Failed to create cloud agent";
        }
    }

    // Create NetworkAgent with cloud agent only (printer agent added later when printer is selected)
    auto agent = std::make_unique<NetworkAgent>(std::move(cloud_agent), nullptr);

    if (agent && use_orca_cloud) {
        auto* orca_cloud = dynamic_cast<OrcaCloudServiceAgent*>(agent->get_cloud_agent().get());
        if (orca_cloud) {
            orca_cloud->configure_urls(app_config);
        }
    }

    return agent;
}

} // namespace Slic3r

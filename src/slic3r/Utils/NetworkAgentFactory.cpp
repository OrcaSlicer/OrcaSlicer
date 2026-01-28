#include "NetworkAgentFactory.hpp"
#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"
#include "BBLPrinterAgent.hpp"
#include "OrcaPrinterAgent.hpp"
#include "QidiPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace {

static std::mutex s_registry_mutex;

std::map<std::string, PrinterAgentInfo>& get_printer_agents()
{
    static std::map<std::string, PrinterAgentInfo> agents;
    return agents;
}

std::string& get_default_agent_id()
{
    static std::string default_id;
    return default_id;
}

} // anonymous namespace

bool NetworkAgentFactory::register_printer_agent(const std::string& id, const std::string& display_name, PrinterAgentFactory factory)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto&                       agents = get_printer_agents();

    auto result = agents.emplace(id, PrinterAgentInfo(id, display_name, std::move(factory)));

    if (result.second) {
        auto& default_id = get_default_agent_id();
        if (default_id.empty()) {
            default_id = id;
        }
        return true;
    } else {
        return false;
    }
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
    auto&                       agents = get_printer_agents();
    auto                        it     = agents.find(id);

    if (it == agents.end()) {
        BOOST_LOG_TRIVIAL(warning) << "Unknown printer agent ID: " << id;
        return nullptr;
    }

    return it->second.factory(cloud_agent, log_dir);
}

std::string NetworkAgentFactory::get_default_printer_agent_id()
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    return get_default_agent_id();
}

void NetworkAgentFactory::set_default_printer_agent_id(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    auto&                       agents = get_printer_agents();

    if (agents.find(id) != agents.end()) {
        get_default_agent_id() = id;
    }
}

void NetworkAgentFactory::register_all_agents()
{
    // Register Orca printer agent
    {
        auto info = OrcaPrinterAgent::get_agent_info_static();
        register_printer_agent(info.id, info.name,
                               [](std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                  const std::string&                  log_dir) -> std::shared_ptr<IPrinterAgent> {
                                   auto agent = std::make_shared<OrcaPrinterAgent>(log_dir);
                                   if (cloud_agent) {
                                       agent->set_cloud_agent(cloud_agent);
                                   }
                                   return agent;
                               });
    }

    // Register Qidi printer agent
    {
        auto info = QidiPrinterAgent::get_agent_info_static();
        register_printer_agent(info.id, info.name,
                               [](std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                  const std::string&                  log_dir) -> std::shared_ptr<IPrinterAgent> {
                                   auto agent = std::make_shared<QidiPrinterAgent>(log_dir);
                                   if (cloud_agent) {
                                       agent->set_cloud_agent(cloud_agent);
                                   }
                                   return agent;
                               });
    }

    // Register Moonraker printer agent
    {
        auto info = MoonrakerPrinterAgent::get_agent_info_static();
        register_printer_agent(info.id, info.name,
                               [](std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                  const std::string&                  log_dir) -> std::shared_ptr<IPrinterAgent> {
                                   auto agent = std::make_shared<MoonrakerPrinterAgent>(log_dir);
                                   if (cloud_agent) {
                                       agent->set_cloud_agent(cloud_agent);
                                   }
                                   return agent;
                               });
    }

    // Register BBL printer agent (only if bbl network agent is available)
    {
        auto info = BBLPrinterAgent::get_agent_info_static();
        register_printer_agent(info.id, info.name,
                               [](std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                  const std::string&                  log_dir) -> std::shared_ptr<IPrinterAgent> {
                                   auto agent = std::make_shared<BBLPrinterAgent>();
                                   if (cloud_agent) {
                                       agent->set_cloud_agent(cloud_agent);
                                   }
                                   return agent;
                               });
    }

}

std::unique_ptr<NetworkAgent> create_agent_from_config(const std::string& log_dir, AppConfig* app_config)
{
    // Determine cloud provider from config
    bool use_orca_cloud = false;
    if (app_config) {
        try {
            use_orca_cloud = app_config->get("use_orca_cloud") == "true" || app_config->get_bool("use_orca_cloud");
        } catch (...) {
            use_orca_cloud = false;
        }
    }

    // Create cloud agent
    std::shared_ptr<ICloudServiceAgent> cloud_agent = nullptr;
    if (use_orca_cloud || app_config->get_bool("installed_networking")) {
        CloudAgentProvider provider = use_orca_cloud ? CloudAgentProvider::Orca : CloudAgentProvider::BBL;
        cloud_agent                 = NetworkAgentFactory::create_cloud_agent(provider, log_dir);
        if (!cloud_agent) {
            BOOST_LOG_TRIVIAL(error) << "Failed to create cloud agent";
        }
    }

    // Create NetworkAgent with cloud agent only (printer agent added later)
    // We will create the printer agent later when the printer is selected, so we pass nullptr for the printer agent here.
    auto agent = NetworkAgentFactory::create_from_agents(std::move(cloud_agent), nullptr);

    if (agent && app_config && use_orca_cloud) {
        auto* orca_cloud = dynamic_cast<OrcaCloudServiceAgent*>(agent->get_cloud_agent().get());
        if (orca_cloud) {
            orca_cloud->configure_urls(app_config);
        }
    }

    return agent;
}

} // namespace Slic3r

#ifndef __NETWORK_AGENT_FACTORY_HPP__
#define __NETWORK_AGENT_FACTORY_HPP__

#include "ICloudServiceAgent.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgent.hpp"
#include "OrcaCloudServiceAgent.hpp"
#include "BBLCloudServiceAgent.hpp"
#include "BBLNetworkPlugin.hpp"
#include "libslic3r/AppConfig.hpp"
#include <memory>
#include <string>
#include <functional>
#include <vector>

namespace Slic3r {

/**
 * CloudAgentProvider - Specifies which implementation to use for each agent type.
 *
 * - Orca: Native Orca cloud implementations (OrcaCloudServiceAgent)
 * - BBL: BBL DLL wrapper implementations (BBLCloudServiceAgent)
 */
enum class CloudAgentProvider { Orca, BBL };

static constexpr char ORCA_PRINTER_AGENT_ID[] = "orca";
static constexpr char BBL_PRINTER_AGENT_ID[] = "bbl";

// Factory function type for creating printer agents
using PrinterAgentFactory =
    std::function<std::shared_ptr<IPrinterAgent>(std::shared_ptr<ICloudServiceAgent> cloud_agent, const std::string& log_dir)>;

// Information about a registered printer agent
struct PrinterAgentInfo
{
    std::string         id;           // e.g., "orca", "bbl"
    std::string         display_name; // e.g., "Orca Native", "Bambu Lab"
    PrinterAgentFactory factory;      // Function to create the agent

    PrinterAgentInfo(const std::string& id_, const std::string& display_name_, PrinterAgentFactory factory_)
        : id(id_), display_name(display_name_), factory(std::move(factory_))
    {}
};

/**
 * NetworkAgentFactory - Factory for creating network agent instances
 *
 * This factory creates cloud agents and printer agents for the networking subsystem.
 * The architecture separates cloud services (authentication, project sync) from
 * printer communication (device discovery, print jobs).
 *
 * Startup flow:
 *   1. Call register_all_agents() during app initialization
 *   2. Cloud agent created at startup via create_agent_from_config()
 *   3. Printer agent created on-demand when a printer is selected
 *
 * Usage:
 *   // At app startup (before any agent creation)
 *   NetworkAgentFactory::register_all_agents();
 *
 *   // Create NetworkAgent with cloud agent only
 *   auto agent = create_agent_from_config(log_dir, app_config);
 *
 *   // When printer is selected - create printer agent from registry
 *   auto printer = NetworkAgentFactory::create_printer_agent_by_id("orca", cloud, log_dir);
 */
class NetworkAgentFactory
{
public:
    // ========================================================================
    // Printer Agent Registry
    // ========================================================================

    /**
     * Register all built-in printer agents.
     * Must be called once during application initialization, before any
     * calls to get_registered_printer_agents() or create_printer_agent_by_id().
     */
    static void register_all_agents();

    /**
     * Register a printer agent type
     *
     * @param id Unique identifier for the agent (e.g., "orca", "bbl")
     * @param display_name Human-readable name for UI
     * @param factory Factory function to create the agent
     * @return true if registration succeeded, false if already registered
     */
    static bool register_printer_agent(const std::string& id, const std::string& display_name, PrinterAgentFactory factory);

    /**
     * Check if an agent ID is registered
     */
    static bool is_printer_agent_registered(const std::string& id);

    /**
     * Get info about a registered agent
     */
    static const PrinterAgentInfo* get_printer_agent_info(const std::string& id);

    /**
     * Get all registered printer agents (for UI population)
     */
    static std::vector<PrinterAgentInfo> get_registered_printer_agents();

    /**
     * Create a printer agent by ID (using registry)
     *
     * Returns a cached instance if one exists for the given ID, otherwise
     * creates a new agent via the registered factory and caches it.
     *
     * @param id Agent ID to create
     * @param cloud_agent Cloud agent for token access
     * @param log_dir Directory for log files
     * @return Shared pointer to IPrinterAgent, or nullptr if ID not found
     */
    static std::shared_ptr<IPrinterAgent> create_printer_agent_by_id(const std::string&                  id,
                                                                     std::shared_ptr<ICloudServiceAgent> cloud_agent,
                                                                     const std::string&                  log_dir);

    /**
     * Clear the printer agent cache.
     * Calls disconnect_printer() on each cached agent and releases all shared_ptrs.
     * Should be called during application shutdown before destroying the NetworkAgent.
     */
    static void clear_printer_agent_cache();

    // ========================================================================
    // Cloud Agent Factory
    // ========================================================================

    /**
     * Create a cloud service agent based on provider type.
     * Handles authentication, project sync, and other cloud services.
     *
     * @param provider Which implementation to use (Orca or BBL)
     * @param log_dir Directory for log files
     * @return Shared pointer to ICloudServiceAgent implementation
     */
    static std::shared_ptr<ICloudServiceAgent> create_cloud_agent(CloudAgentProvider provider, const std::string& log_dir)
    {
        switch (provider) {
        case CloudAgentProvider::Orca: return std::make_shared<OrcaCloudServiceAgent>(log_dir);
        case CloudAgentProvider::BBL: {
            auto& plugin = BBLNetworkPlugin::instance();
            if (!plugin.is_loaded()) {
                return nullptr;
            }
            if (!plugin.has_agent()) {
                plugin.create_agent(log_dir);
            }
            if (!plugin.has_agent()) {
                return nullptr;
            }
            return std::make_shared<BBLCloudServiceAgent>();
        }
        default: return nullptr;
        }
    }

private:
    // Factory is not instantiable
    NetworkAgentFactory()                                      = delete;
    ~NetworkAgentFactory()                                     = delete;
    NetworkAgentFactory(const NetworkAgentFactory&)            = delete;
    NetworkAgentFactory& operator=(const NetworkAgentFactory&) = delete;
};

/**
 * Create a NetworkAgent from AppConfig settings (main entry point)
 *
 * Creates a NetworkAgent with cloud agent only. The printer agent is created
 * separately when a printer is selected, via create_printer_agent_by_id().
 *
 * Cloud provider selection:
 *   - use_orca_cloud=true  → OrcaCloudServiceAgent (default)
 *   - use_orca_cloud=false → BBLCloudServiceAgent (requires plugin)
 *
 * @param log_dir Directory for log files
 * @param app_config Application configuration object
 * @return NetworkAgent with cloud agent, or nullptr on failure
 */
std::unique_ptr<NetworkAgent> create_agent_from_config(const std::string& log_dir, AppConfig* app_config);

} // namespace Slic3r

#endif // __NETWORK_AGENT_FACTORY_HPP__

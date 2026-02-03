#ifndef __I_PRINTER_AGENT_HPP__
#define __I_PRINTER_AGENT_HPP__

#include "bambu_networking.hpp"
#include <string>
#include <memory>

namespace Slic3r {

class ICloudServiceAgent;

/**
 * AgentInfo - Metadata structure for printer agent information.
 *
 * Contains identification and descriptive information about a printer agent
 * implementation, used for discovery and selection purposes.
 */
struct AgentInfo {
    std::string id;         ///< Unique identifier for the agent, e.g. "orca", "bbl"
    std::string name;       ///< Human-readable agent name, e.g. "Orca", "Bambu Lab"
    std::string version;    ///< Agent version string, e.g. "1.0.0"
    std::string description; ///< Brief description of the agent's capabilities, e.g. "Orca printer agent"
};

/**
 * FilamentSyncMode - Modes for filament data synchronization.
 *
 * Defines how filament information is obtained from the printer:
 * - Subscription: Real-time push updates (e.g., MQTT subscriptions)
 * - Pull: On-demand fetch via REST API (blocking call)
 * - None: Filament sync unavailable
 */
enum class FilamentSyncMode {
    none = 0,     ///< Filament synchronization not supported
    subscription, ///< Real-time push updates via subscription (e.g., MQTT)
    pull          ///< On-demand fetch via REST API (blocking call)
};

/**
 * IPrinterAgent - Interface for printer operations.
 *
 * This interface encapsulates all printer-related functionality:
 * - Direct printer communication (LAN and cloud relay)
 * - Certificate management
 * - Device discovery (SSDP)
 * - Printer binding/unbinding
 * - Print job operations
 *
 * Implementations:
 * - OrcaPrinterAgent: Stub implementation (printer ops not yet supported)
 * - BBLPrinterAgent: Wrapper around Bambu Lab's proprietary DLL
 *
 * Token Access:
 * Printer agents receive an ICloudServiceAgent instance via set_cloud_agent() to
 * access tokens for cloud-relay operations.
 */

class IPrinterAgent {
public:
    virtual ~IPrinterAgent() = default;

    // ========================================================================
    // Cloud Agent Dependency
    // ========================================================================
    /**
     * Set the cloud agent used for token access.
     * Must be called before any cloud-relay operations.
     */
    virtual void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) = 0;

    // ========================================================================
    // Communication
    // ========================================================================
    /**
     * Publish a JSON command to a printer through cloud relay.
     */
    virtual int send_message(std::string dev_id, std::string json_str, int qos, int flag) = 0;

    /**
     * Establish a direct LAN connection to a printer.
     */
    virtual int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) = 0;

    /**
     * Tear down the active LAN printer connection.
     */
    virtual int disconnect_printer() = 0;

    /**
     * Send a JSON command to a LAN printer (bypassing cloud).
     */
    virtual int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) = 0;

    // ========================================================================
    // Certificates
    // ========================================================================
    /**
     * Validate current user certificates for the printer.
     */
    virtual int check_cert() = 0;

    /**
     * Install or refresh device certificate for LAN TLS.
     */
    virtual void install_device_cert(std::string dev_id, bool lan_only) = 0;

    // ========================================================================
    // Discovery
    // ========================================================================
    /**
     * Start or stop SSDP discovery.
     */
    virtual bool start_discovery(bool start, bool sending) = 0;

    // ========================================================================
    // Binding
    // ========================================================================
    /**
     * Ping the binding endpoint to check printer readiness.
     */
    virtual int ping_bind(std::string ping_code) = 0;

    /**
     * Perform binding detection/handshake on a LAN printer.
     */
    virtual int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) = 0;

    /**
     * Execute the multi-stage printer binding workflow.
     */
    virtual int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) = 0;

    /**
     * Remove the association between account and printer.
     */
    virtual int unbind(std::string dev_id) = 0;

    /**
     * Request a one-time bind ticket from the server.
     */
    virtual int request_bind_ticket(std::string* ticket) = 0;

    /**
     * Register callback for fatal HTTP errors.
     */
    virtual int set_server_callback(OnServerErrFn fn) = 0;

    // ========================================================================
    // Machine Selection
    // ========================================================================
    /**
     * Return the currently selected printer ID.
     */
    virtual std::string get_user_selected_machine() = 0;

    /**
     * Update the selected machine preference.
     */
    virtual int set_user_selected_machine(std::string dev_id) = 0;

    // ========================================================================
    // Print Job Operations
    // ========================================================================
    /**
     * Start a fully managed cloud print.
     */
    virtual int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = 0;

    /**
     * Start a local print with cloud record.
     */
    virtual int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = 0;

    /**
     * Upload gcode to printer's SD card without starting.
     */
    virtual int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = 0;

    /**
     * Start a LAN-only print.
     */
    virtual int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) = 0;

    /**
     * Start a print from printer's SD card.
     */
    virtual int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) = 0;

    // ========================================================================
    // Callback Registration
    // ========================================================================
    /**
     * Register SSDP discovery callback.
     */
    virtual int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) = 0;

    /**
     * Register printer MQTT connection callback.
     */
    virtual int set_on_printer_connected_fn(OnPrinterConnectedFn fn) = 0;

    /**
     * Register subscription failure callback.
     */
    virtual int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) = 0;

    /**
     * Register cloud device message callback.
     */
    virtual int set_on_message_fn(OnMessageFn fn) = 0;

    /**
     * Register user-scoped message callback.
     */
    virtual int set_on_user_message_fn(OnMessageFn fn) = 0;

    /**
     * Register LAN connection status callback.
     */
    virtual int set_on_local_connect_fn(OnLocalConnectedFn fn) = 0;

    /**
     * Register LAN message callback.
     */
    virtual int set_on_local_message_fn(OnMessageFn fn) = 0;

    /**
     * Provide main thread queue callback.
     */
    virtual int set_queue_on_main_fn(QueueOnMainFn fn) = 0;

    /**
     * Get agent information.
     */
    virtual AgentInfo get_agent_info() = 0;

    // ========================================================================
    // Filament Operations
    // ========================================================================
    /**
     * Get the filament synchronization mode for this agent.
     * 
     * @return FilamentSyncMode indicating how filament data is obtained:
     *         - subscription: Real-time push updates via MQTT (no fetch needed)
     *         - pull: On-demand fetch via REST API (call fetch_filament_info())
     *         - none: Filament synchronization not supported
     */
    virtual FilamentSyncMode get_filament_sync_mode() const { return FilamentSyncMode::none; }

    /**
     * Refresh filament info from the printer synchronously.
     * Should only be called when get_filament_sync_mode() returns FilamentSyncMode::pull.
     * Populates the MachineObject's DevFilaSystem with fetched filament data.
     */
    virtual bool fetch_filament_info(std::string dev_id) { return false; }
};

} // namespace Slic3r

#endif // __I_PRINTER_AGENT_HPP__

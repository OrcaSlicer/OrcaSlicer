#ifndef __I_PRINTER_AGENT_HPP__
#define __I_PRINTER_AGENT_HPP__

#include "bambu_networking.hpp"
// why: these extend the BAMBU_NETWORK_* return space rather than opening a new one - the value
// flows through the same int domain callers already compare against BAMBU_NETWORK_SUCCESS.
// They live here and not in bambu_networking.hpp because that file is a vendor header replaced
// wholesale by header-sync commits (see c09252ce11), which would silently clobber them.
// -70xx is free: the vendor occupies -1..-25 and -10xx through -60xx.
#define ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED -7010 // no translation exists for this command
#define ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE -7020 // a translation exists; this printer lacks the capability
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <cstdint>
#include <cmath>
#include <nlohmann/json.hpp>
#include <boost/format.hpp>
#include "ICameraSignalingChannel.hpp"

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

enum class CameraStreamMode {
    none = 0,
    http,  // LAN or Cloud
    https, // LAN or Cloud over TLS
    rtsp,  // LAN only
    webrtc, // Cloud only
    http_snapshot // HTTP endpoint returning one image per request
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
 * - PrinterAgentPluginCapability: Python printer-agent plugin capability that
 *   implements IPrinterAgent directly and is handed out as the live agent
 * - BBLPrinterAgent: Wrapper around Bambu Lab's proprietary DLL
 *
 * Token Access:
 * Printer agents receive an ICloudServiceAgent instance via set_cloud_agent() to
 * access tokens for cloud-relay operations.
 */

class IPrinterAgent {
public:
    virtual ~IPrinterAgent() = default;

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

    // why: gcode is firmware dialect, not a waist concept - commands whose body is Bambu-dialect
    // gcode live on the agent that speaks it; the default is an honest refusal that MachineObject's
    // publish funnel turns into a dialog.
    virtual int command_ams_refresh_rfid(std::string, std::string, int, bool)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_ams_calibrate(std::string, int, int, bool)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_ams_select_tray(std::string, std::string, int, bool)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_start_camera(std::string)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

private:
    int publish_command_json(std::string dev_id, const nlohmann::json& j, bool lan_mode)
    {
        return lan_mode ? send_message_to_printer(dev_id, j.dump(), 0, 0)
                         : send_message(dev_id, j.dump(), 0, 0);
    }

public:
    virtual int command_xyz_abs(std::string dev_id, int sequence_id, bool lan_mode)
    {
        nlohmann::json j;
        j["print"]["command"]     = "gcode_line";
        j["print"]["param"]       = "G90 \n";
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        return publish_command_json(dev_id, j, lan_mode);
    }
    virtual int command_auto_leveling(std::string dev_id, int sequence_id, bool lan_mode)
    {
        nlohmann::json j;
        j["print"]["command"]     = "gcode_line";
        j["print"]["param"]       = "G29 \n";
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        return publish_command_json(dev_id, j, lan_mode);
    }
    virtual int command_go_home(std::string dev_id, bool is_printing, bool supports_mqtt_homing, int sequence_id, bool lan_mode)
    {
        nlohmann::json j;
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        if (supports_mqtt_homing) {
            j["print"]["command"] = "back_to_center";
        } else {
            j["print"]["command"] = "gcode_line";
            j["print"]["param"]   = is_printing ? "G28 X\n" : "G28 \n";
        }
        return publish_command_json(dev_id, j, lan_mode);
    }
    virtual int command_set_bed(std::string dev_id, int temp, bool supports_mqtt_bed_ctrl, int sequence_id, bool lan_mode)
    {
        nlohmann::json j;
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        if (supports_mqtt_bed_ctrl) {
            j["print"]["command"] = "set_bed_temp";
            j["print"]["temp"]    = temp;
        } else {
            j["print"]["command"] = "gcode_line";
            j["print"]["param"]   = (boost::format("M140 S%1%\n") % temp).str();
        }
        return publish_command_json(dev_id, j, lan_mode);
    }
    virtual int command_set_nozzle(std::string dev_id, int temp, int sequence_id, bool lan_mode)
    {
        nlohmann::json j;
        j["print"]["command"]     = "gcode_line";
        j["print"]["param"]       = (boost::format("M104 S%1%\n") % temp).str();
        j["print"]["sequence_id"] = std::to_string(sequence_id);
        return publish_command_json(dev_id, j, lan_mode);
    }

    virtual int command_axis_control(std::string dev_id, std::string axis, double unit, double input_val, int speed,
                                      bool is_core_xy, bool supports_mqtt_axis_control, int sequence_id, bool lan_mode)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

    /**
     * Default LAN account username for this agent's protocol, if it has a fixed one.
     * Returns an empty string if the agent has no fixed default (e.g. caller must supply one).
     */
    virtual std::string default_lan_username() const { return {}; }

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
    virtual int check_cert() { return BAMBU_NETWORK_SUCCESS; }

    /**
     * Install or refresh device certificate for LAN TLS.
     */
    virtual void install_device_cert(std::string dev_id, bool lan_only)
    {
        (void) dev_id;
        (void) lan_only;
    }

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
    virtual int ping_bind(std::string ping_code)
    {
        (void) ping_code;
        return BAMBU_NETWORK_SUCCESS;
    }

    /**
     * Perform binding detection/handshake on a LAN printer.
     */
    virtual int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) = 0;

    /**
     * Execute the multi-stage printer binding workflow.
     */
    virtual int bind(std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link,
                     std::string timezone, bool improved, OnUpdateStatusFn update_fn)
    {
        (void) dev_ip;
        (void) dev_id;
        (void) dev_model;
        (void) sec_link;
        (void) timezone;
        (void) improved;
        (void) update_fn;
        return BAMBU_NETWORK_SUCCESS;
    }

    /**
     * Remove the association between account and printer.
     */
    virtual int unbind(std::string dev_id)
    {
        (void) dev_id;
        return BAMBU_NETWORK_SUCCESS;
    }

    /**
     * Request a one-time bind ticket from the server.
     */
    virtual int request_bind_ticket(std::string* ticket)
    {
        if (ticket)
            *ticket = {};
        return BAMBU_NETWORK_SUCCESS;
    }

    /**
     * Fetch the cloud snapshot image captured at a print failure.
     * Returns 0 if the request was dispatched; the image body arrives via callback(body, http_status).
     */
    virtual int get_hms_snapshot(std::string dev_id, std::string file_name,
                                 std::function<void(std::string, int)> callback)
    {
        (void) dev_id;
        (void) file_name;
        (void) callback;
        return -1;
    }

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
     * Update the selected cloud machine preference.
     */
    virtual int set_user_selected_machine(std::string dev_id) = 0;

    // ========================================================================
    // Subscriptions
    // ========================================================================
    /**
     * Subscribe to a logical module (for example app- or tunnel-scoped streams).
     */
    virtual int start_subscribe(std::string module) { (void) module; return BAMBU_NETWORK_SUCCESS; }

    /**
     * Stop listening to a formerly subscribed module.
     */
    virtual int stop_subscribe(std::string module) { (void) module; return BAMBU_NETWORK_SUCCESS; }

    /**
     * Subscribe to push streams for specific device identifiers.
     */
    virtual int add_subscribe(std::vector<std::string> dev_list) { (void) dev_list; return BAMBU_NETWORK_SUCCESS; }

    /**
     * Remove device-level subscriptions.
     */
    virtual int del_subscribe(std::vector<std::string> dev_list) { (void) dev_list; return BAMBU_NETWORK_SUCCESS; }

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
     * Get the camera stream mode for this agent. This value can be deterministic and derived at
     * runtime if the printer supports multiple camera stream modes. E.g. LAN => HTTP/HTTPS/RTSP, Cloud => WebRTC.
     *
     * @return CameraStreamMode indicating how the camera stream is obtained or used:
     */
    virtual CameraStreamMode get_camera_stream_mode() const { return CameraStreamMode::none; }

    /**
     * Refresh filament info from the printer synchronously.
     * Should only be called when get_filament_sync_mode() returns FilamentSyncMode::pull.
     * Populates the MachineObject's DevFilaSystem with fetched filament data.
     */
    virtual bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) { return false; }

    /**
     * Translate one filament id across the printer boundary.
     *
     * Orca content-addresses every system filament; a printer, its AMS and its vendor cloud
     * know only that vendor's own catalog ids. An agent whose printers already speak Orca's
     * ids leaves them alone, and so does an id with no mapping.
     */
    virtual std::string to_orca_filament_id(const std::string& printer_filament_id) const { return printer_filament_id; }
    virtual std::string from_orca_filament_id(const std::string& orca_filament_id) const { return orca_filament_id; }

    /**
     * Get the current camera stream URL for this agent's active machine.
     * Only meaningful when get_camera_stream_mode() returns an HTTP, HTTPS, or RTSP mode.
     */
    virtual std::string get_camera_url() const { return {}; }
};

} // namespace Slic3r

#endif // __I_PRINTER_AGENT_HPP__

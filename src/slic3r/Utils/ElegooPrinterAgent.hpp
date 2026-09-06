#pragma once

#include "IPrinterAgent.hpp"
#include "FilamentSyncUtils.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

/**
 * ElegooPrinterAgent - Printer agent for Elegoo Centauri Carbon printers with CANVAS module.
 *
 * Implements IPrinterAgent directly (not via MoonrakerPrinterAgent) because Elegoo
 * printers use the SDCP WebSocket protocol, not Klipper/Moonraker HTTP.
 *
 * Provides:
 *   - connect_printer: WebSocket handshake to verify printer reachability
 *   - fetch_filament_info: Query CANVAS tray data via Cmd 324
 *   - FilamentSyncMode::pull
 *
 * All other IPrinterAgent methods are no-ops (no cloud, no binding, no SSDP).
 */
class ElegooPrinterAgent : public IPrinterAgent
{
public:
    explicit ElegooPrinterAgent(std::string log_dir);
    ~ElegooPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Cloud Agent Dependency
    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override { m_cloud_agent = cloud; }

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override { return 0; }
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override { return 0; }

    // Certificates
    int  check_cert() override { return 0; }
    void install_device_cert(std::string dev_id, bool lan_only) override {}

    // Discovery
    bool start_discovery(bool start, bool sending) override { return true; }

    // Binding
    int ping_bind(std::string ping_code) override { return 0; }
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override { return 0; }
    int bind(std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override { return 0; }
    int unbind(std::string dev_id) override { return 0; }
    int request_bind_ticket(std::string* ticket) override { return 0; }
    int get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback) override { return 0; }
    int set_server_callback(OnServerErrFn fn) override { return 0; }

    // Machine Selection
    std::string get_user_selected_machine() override { return m_selected_machine; }
    int set_user_selected_machine(std::string dev_id) override { m_selected_machine = dev_id; return 0; }

    // Print Job Operations
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override { return 0; }
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override { return 0; }
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override { return 0; }
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override { return 0; }
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override { return 0; }

    // Callback Registration
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override { return 0; }
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override { m_on_printer_connected = fn; return 0; }
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override { return 0; }
    int set_on_message_fn(OnMessageFn fn) override { return 0; }
    int set_on_user_message_fn(OnMessageFn fn) override { return 0; }
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override { m_on_local_connect = fn; return 0; }
    int set_on_local_message_fn(OnMessageFn fn) override { return 0; }
    int set_queue_on_main_fn(QueueOnMainFn fn) override { m_queue_on_main = fn; return 0; }

    // Filament Operations
    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::pull; }
    bool fetch_filament_info(std::string dev_id) override;

private:
    // Extract bare IP/hostname from dev_ip (strips scheme, port, path)
    std::string extract_host(const std::string& dev_ip) const;

    // Query CANVAS module via WebSocket Cmd 324
    bool fetch_canvas_tray_info(const std::string& host,
                                std::vector<AmsTrayData>& trays,
                                int& canvas_count,
                                std::string& error);

    // Normalize Elegoo filament type strings to OrcaSlicer standard types
    static std::string normalize_filament_type(const std::string& filament_type);

    // Dispatch connection callbacks
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg);
    void dispatch_printer_connected(const std::string& dev_id);

    // WebSocket constants
    static constexpr const char* ELEGOO_WS_PORT = "3030";
    static constexpr const char* ELEGOO_WS_PATH = "/websocket";
    static constexpr int         CANVAS_CMD     = 324;

    // State
    mutable std::recursive_mutex m_mutex;
    std::string                  m_dev_id;
    std::string                  m_dev_ip;
    std::string                  m_model_id;
    std::string                  m_selected_machine;
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;

    // Callbacks
    OnLocalConnectedFn   m_on_local_connect;
    OnPrinterConnectedFn m_on_printer_connected;
    QueueOnMainFn        m_queue_on_main;
};

} // namespace Slic3r

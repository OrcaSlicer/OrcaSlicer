#ifndef __MOONRAKER_PRINTER_AGENT_HPP__
#define __MOONRAKER_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace Slic3r {

class MoonrakerPrinterAgent : public IPrinterAgent
{
public:
    explicit MoonrakerPrinterAgent(std::string log_dir);
    ~MoonrakerPrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Cloud Agent Dependency
    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    // Certificates
    int check_cert() override;
    void install_device_cert(std::string dev_id, bool lan_only) override;

    // Discovery
    bool start_discovery(bool start, bool sending) override;

    // Binding
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    int request_bind_ticket(std::string* ticket) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine Selection
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;

    // Print Job Operations
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    // Callbacks
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    // Pull-mode agent (on-demand filament sync)
    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::pull; }
    bool fetch_filament_info(std::string dev_id) override;

protected:
    struct MoonrakerDeviceInfo
    {
        std::string dev_id;
        std::string dev_ip;
        std::string api_key;
        std::string base_url;
        std::string model_id;
        std::string model_name;
        std::string dev_name;
        std::string version;
        std::string klippy_state;
        bool        use_ssl = false;
    } device_info;

    // Tray data for AMS payload building
    struct AmsTrayData {
        int         slot_index = 0;      // 0-based slot index
        bool        has_filament = false;
        std::string tray_type;           // Material type (e.g., "PLA", "ASA")
        std::string tray_color;          // Raw color (#RRGGBB, 0xRRGGBB, or RRGGBBAA)
        std::string tray_info_idx;       // Setting ID (optional)
        int         bed_temp = 0;        // Optional
        int         nozzle_temp = 0;     // Optional
    };

    // Build ams JSON and call parser
    void build_ams_payload(int ams_count, int max_lane_index, const std::vector<AmsTrayData>& trays);

    // Methods that derived classes may need to override or access
    virtual bool init_device_info(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl);
    virtual bool fetch_device_info(const std::string& base_url, const std::string& api_key, MoonrakerDeviceInfo& info, std::string& error) const;

    // State access for derived classes
    mutable std::recursive_mutex       state_mutex;

    // Helpers
    bool        is_numeric(const std::string& value);
    std::string normalize_base_url(std::string host, const std::string& port);
    std::string sanitize_filename(const std::string& filename);
    std::string join_url(const std::string& base_url, const std::string& path) const;

    // Trim whitespace and convert to uppercase
    static std::string trim_and_upper(const std::string& input);

    // Map filament type to OrcaFilamentLibrary preset ID for AMS sync compatibility
    static std::string map_filament_type_to_generic_id(const std::string& filament_type);

private:
    int handle_request(const std::string& dev_id, const std::string& json_str);
    int send_version_info(const std::string& dev_id);
    int send_access_code(const std::string& dev_id);

    bool fetch_object_list(const std::string& base_url, const std::string& api_key, std::set<std::string>& objects, std::string& error) const;
    bool query_printer_status(const std::string& base_url, const std::string& api_key, nlohmann::json& status, std::string& error) const;
    bool send_gcode(const std::string& dev_id, const std::string& gcode) const;

    void announce_printhost_device();
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg);
    void dispatch_printer_connected(const std::string& dev_id);
    void dispatch_message(const std::string& dev_id, const std::string& payload);
    void start_status_stream(const std::string& dev_id, const std::string& base_url, const std::string& api_key);
    void stop_status_stream();
    void run_status_stream(std::string dev_id, std::string base_url, std::string api_key);
    void handle_ws_message(const std::string& dev_id, const std::string& payload);
    void update_status_cache(const nlohmann::json& updates);
    nlohmann::json build_print_payload_locked() const;

    // Print control helpers
    int pause_print(const std::string& dev_id);
    int resume_print(const std::string& dev_id);
    int cancel_print(const std::string& dev_id);

    // File upload
    bool upload_gcode(const std::string& local_path, const std::string& filename,
                      const std::string& base_url, const std::string& api_key,
                      OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);

    // JSON-RPC helper
    bool send_jsonrpc_command(const std::string& base_url, const std::string& api_key,
                              const nlohmann::json& request, std::string& response) const;

    // Connection thread management
    void perform_connection_async(const std::string& dev_id,
                                   const std::string& base_url,
                                   const std::string& api_key,
                                   uint64_t generation);

    // System-specific filament fetch methods
    bool fetch_hh_filament_info(std::vector<AmsTrayData>& trays, int& max_lane_index);
    bool fetch_afc_filament_info(std::vector<AmsTrayData>& trays, int& max_lane_index);

    // JSON helper methods
    static std::string safe_json_string(const nlohmann::json& obj, const char* key);
    static int safe_json_int(const nlohmann::json& obj, const char* key);
    static std::string safe_array_string(const nlohmann::json& arr, int idx);
    static int safe_array_int(const nlohmann::json& arr, int idx);
    static std::string normalize_color_value(const std::string& color);

    std::string                        ssdp_announced_host;
    std::string                        ssdp_announced_id;
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;
    std::string                        selected_machine;

    OnMsgArrivedFn       on_ssdp_msg_fn;
    OnPrinterConnectedFn on_printer_connected_fn;
    GetSubscribeFailureFn on_subscribe_failure_fn;
    OnMessageFn          on_message_fn;
    OnMessageFn          on_user_message_fn;
    OnLocalConnectedFn   on_local_connect_fn;
    OnMessageFn          on_local_message_fn;
    QueueOnMainFn        queue_on_main_fn;
    OnServerErrFn        on_server_err_fn;

    mutable std::recursive_mutex payload_mutex;
    nlohmann::json     status_cache;

    std::atomic<int>       next_jsonrpc_id{1};
    std::set<std::string>  available_objects;  // Track for feature detection

    std::atomic<bool>   ws_stop{false};
    std::atomic<bool>   ws_reconnect_requested{false};  // Flag to trigger reconnection
    std::atomic<uint64_t> ws_last_emit_ms{0};
    std::thread         ws_thread;

    // Throttling configuration for WebSocket updates
    // Critical changes (state transitions) dispatch immediately; telemetry is throttled
    static constexpr uint64_t STATUS_UPDATE_INTERVAL_MS = 1000;  // 1 update/sec for telemetry
    std::atomic<uint64_t> ws_last_dispatch_ms{0};
    std::string last_print_state;  // Track state for immediate dispatch on change

    // Connection thread management
    std::atomic<uint64_t>  connect_generation{0};
    std::thread            connect_thread;
    std::recursive_mutex   connect_mutex;
};

} // namespace Slic3r

#endif

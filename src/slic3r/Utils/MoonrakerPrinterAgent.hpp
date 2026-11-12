#ifndef __MOONRAKER_PRINTER_AGENT_HPP__
#define __MOONRAKER_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <functional>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <vector>

namespace Slic3r { class ICloudServiceAgent; }

namespace Slic3r {

// Tracks a toolhead change requested from the Device page (select_extruder), so the
// status pushed to the UI reports it in device.extruder.state the way the UI expects:
// "switching" while the T<n> command runs, then its outcome once. Without the outcome,
// a T<n> that Klipper rejects, or one for the toolhead already in use, leaves the UI
// waiting for a toolhead change that never comes. Not thread-safe; guard it externally.
class MoonrakerToolSelectTracker
{
public:
    // Switch states, as DevExtderSwitchState encodes them.
    static constexpr int SWITCH_IDLE    = 0;
    static constexpr int SWITCH_RUNNING = 2;
    static constexpr int SWITCH_FAILED  = 3;

    // Starts tracking a request and returns its id. A newer request replaces an older one, so
    // finish() takes the id and ignores the outcome of a request that is no longer tracked: the UI
    // can ask for another toolhead while an earlier script (a long follow-up macro) still runs.
    unsigned start(int target);
    void     finish(unsigned request, bool success);
    // The device.extruder.state value for one status push. Reports "switching" at least once
    // per request, then the outcome in exactly one push, then nothing pending.
    int  next_state(int extruder_count, int current_extruder);

private:
    int      m_target   = -1;
    int      m_switch   = SWITCH_IDLE;
    bool     m_reported = false;
    unsigned m_request  = 0;
};

class MoonrakerPrinterAgent : public IPrinterAgent
{
public:
    explicit MoonrakerPrinterAgent(std::string log_dir);
    ~MoonrakerPrinterAgent() override;

    // The G-code script for a Device page toolhead change: optional G28, T<index>, then the
    // optional follow-up command (first line only, trimmed).
    static std::string tool_select_script(int extruder_index, bool home_first, std::string after_macro);

    // The G-code script for the Device page extrude/retract buttons: a relative E move of
    // length_mm (negative retracts) at speed_mm_s on the active extruder, restoring the G-code
    // state afterwards. Empty when there is nothing to move.
    static std::string extrude_script(double length_mm, double speed_mm_s);

    // The toolhead a lane_data record feeds when its key names a tool ("T<n>", 0-based) and its
    // lane_name, if any, agrees; -1 when the key names no tool or the printer has no such toolhead.
    static int toolhead_from_lane_name(const std::string& lane_key, const nlohmann::json& lane_obj, int toolhead_count);

    // Whether a lane_data record that nothing maps to a toolhead still goes to the main extruder:
    // yes on a single-toolhead printer, or when AFC or Happy Hare is present (their existing
    // fallback); otherwise it is skipped.
    static bool keep_unmapped_lane(int toolhead_count, bool afc_present, bool hh_present);

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Cloud Agent Dependency
    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(const PrinterConnectionParams& params) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    // Discovery
    bool start_discovery(bool start, bool sending) override;

    // Binding
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
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
    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

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

    // Copied when the stream starts, so changes apply to the next start_status_stream()
    struct StatusStreamTimings
    {
        std::chrono::milliseconds handshake{std::chrono::seconds(10)};      // Opening and closing handshakes
        std::chrono::milliseconds close{std::chrono::seconds(1)};           // Wait for the close frame's answer before dropping the socket
        std::chrono::milliseconds idle{std::chrono::seconds(30)};           // Silence before giving up, with a ping every half of it
        std::chrono::milliseconds heartbeat{std::chrono::seconds(10)};      // Silence before the cached status is sent again
        std::chrono::milliseconds reconnect_delay{std::chrono::seconds(1)}; // First wait, doubling per consecutive failure up to 32x
    } ws_timings;

    // Tray data for AMS payload building
    struct AmsTrayData {
        int         slot_index = 0;      // 0-based slot index
        int         extruder_id = 0;     // Extruder this lane feeds (AFC extruder_index); 0 when unknown
        bool        has_filament = false;
        std::string tray_type;           // Material type (e.g., "PLA", "ASA")
        std::string tray_color;          // Raw color (#RRGGBB, 0xRRGGBB, or RRGGBBAA)
        std::string tray_info_idx;       // Setting ID (optional)
        int         bed_temp = 0;        // Optional
        int         nozzle_temp = 0;     // Optional
    };

    // Build ams JSON and call parser
    void build_ams_payload(int ams_count, int max_lane_index, const std::vector<AmsTrayData>& trays);

    // Build ams JSON with one box per extruder (best effort, for lane_data-based filament
    // changers such as AFC) and call the parser. Boxes are ordered by extruder id.
    void build_ams_payload_grouped(const std::vector<AmsTrayData>& trays);

    // Methods that derived classes may need to override or access
    virtual bool init_device_info(const std::string& dev_id, const std::string& dev_ip, const std::string& username, const std::string& password, bool use_ssl, const std::string& port);
    virtual bool fetch_device_info(const std::string& base_url, const std::string& api_key, MoonrakerDeviceInfo& info, std::string& error) const;
    void start_status_stream(const std::string& dev_id, const std::string& base_url, const std::string& api_key);
    void stop_status_stream();

    // State access for derived classes
    mutable std::recursive_mutex       state_mutex;

    // Helpers
    bool        is_numeric(const std::string& value);
    std::string normalize_base_url(bool use_ssl, const std::string& host, const std::string& port);
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
    bool send_gcode_sync(const std::string& dev_id, const std::string& gcode) const;
    void send_gcode_async(const std::string& dev_id, const std::string& gcode,
                          std::function<void(bool)> on_result = {}, long timeout_max_s = 10) const;

    void announce_printhost_device();
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg);
    void dispatch_printer_connected(const std::string& dev_id);
    void dispatch_message(const std::string& dev_id, const std::string& payload);
    // Joins a stream stopped from its own callback, so a new stream can start without waiting on it under connect_mutex
    void join_stopped_status_stream();
    void run_status_stream(std::string dev_id, std::string base_url, std::string api_key, StatusStreamTimings timings);
    void handle_ws_message(const std::string& dev_id, const std::string& payload);
    void send_heartbeat_if_due(const std::string& dev_id, std::chrono::milliseconds interval);
    void update_status_cache(const nlohmann::json& updates);
    nlohmann::json build_print_payload_locked() const;
    // Snapshot Moonraker "extruder"/"extruderN" status objects from the cache, keyed by extruder id.
    std::map<int, nlohmann::json> collect_extruder_status() const;

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
    bool fetch_moonraker_filament_data(std::vector<AmsTrayData>& trays, int& max_lane_index);
    // Lane name -> toolhead index, read from the AFC printer objects (the lane_data database has no
    // extruder field). Returns false when AFC is not present or the objects cannot be read.
    bool fetch_afc_lane_extruders(std::map<std::string, int>& lane_extruder);
    // Gate -> toolhead index, read from the Happy Hare mmu_machine units (gate ranges) and each
    // unit's configured extruder_name. Returns false when Happy Hare is not present.
    bool fetch_hh_gate_extruders(std::map<int, int>& gate_extruder);
    // Read the extruder_name of the [mmu_unit <name>] section from a configfile.config map and map
    // it to a toolhead index. Missing section/setting falls back to the main extruder (Happy Hare's
    // own default).
    static int hh_unit_extruder_from_config(const nlohmann::json& sections, const std::string& unit_name);
    // GET a Moonraker path with the device's API key and parse the JSON body.
    bool http_get_json(const std::string& path, nlohmann::json& out) const;

    // Build one BBL tray JSON object (placeholder when the tray has no filament).
    static nlohmann::json make_ams_tray_json(const std::string& tray_id, const AmsTrayData& tray);

    // Wrap the AMS boxes into the payload ParseV1_0 understands and apply the pull-mode
    // readiness state (printer_type, push counters, storage, module versions).
    void submit_ams_payload(const nlohmann::json& ams_array, unsigned long ams_exist_bits, unsigned long tray_exist_bits);

    // JSON helper methods
    static std::string safe_json_string(const nlohmann::json& obj, const char* key);
    static int safe_json_int(const nlohmann::json& obj, const char* key);
    // Accepts either a JSON number or a numeric string (AFC reports some fields as strings).
    static int safe_json_int_flexible(const nlohmann::json& obj, const char* key, int default_value);
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
    // The running toolhead change, shared with its G-code callback: Moonraker may answer minutes
    // later, after this agent is gone. Building a status push consumes the reported outcome.
    struct ToolSelectState
    {
        std::mutex                 mutex;
        MoonrakerToolSelectTracker tracker;
    };
    const std::shared_ptr<ToolSelectState> tool_select = std::make_shared<ToolSelectState>();

    std::atomic<int>       next_jsonrpc_id{1};
    std::set<std::string>  available_objects;  // Track for feature detection

    std::atomic<bool>   ws_stop{false};
    std::atomic<bool>   ws_reconnect_requested{false};  // Flag to trigger reconnection
    std::atomic<uint64_t> ws_last_emit_ms{0};
    std::thread         ws_thread;
    std::mutex          ws_abort_mutex; // Guards ws_thread and ws_abort
    std::function<void()> ws_abort; // Interrupts whatever the stream thread is waiting on, from any thread

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

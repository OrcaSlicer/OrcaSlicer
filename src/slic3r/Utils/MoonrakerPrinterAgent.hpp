#ifndef __MOONRAKER_PRINTER_AGENT_HPP__
#define __MOONRAKER_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <functional>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <vector>

namespace Slic3r { class ICloudServiceAgent; }

namespace Slic3r {

// One reported slot, before build_ams_payload turns it into a device-model tray.
struct MoonrakerAmsTrayData {
    int         slot_index = 0;      // 0-based index in the agent's slot order
    bool        has_filament = false;
    std::string tray_type;           // Material type (e.g., "PLA", "ASA")
    std::string tray_color;          // Raw color (#RRGGBB, 0xRRGGBB, or RRGGBBAA)
    std::vector<std::string> tray_colors; // Optional: every color of a multi-color spool, in order (same formats).
                                          // Used when it holds two or more; otherwise tray_color is the only one.
    bool        colors_gradient = false;  // With tray_colors: blend them as a gradient instead of drawing segments
    std::string tray_info_idx;       // Filament ID (optional)
    int         bed_temp = 0;        // Optional
    int         nozzle_temp = 0;     // Optional
    std::string tag_uid;             // Non-empty for NFC/RFID-tagged slots, which are never pushed
    std::string slot_name;           // Printer's slot name (AFC lane key, e.g. "lane1"); "" = none
    std::string unit;                // Changer unit's stable id ("Turtle_1"); "" = flat
    std::string unit_label;          // Unit display name; falls back to the id
    std::string head;                // Klipper extruder fed ("extruder1"); "" = unknown
    int         slot = 0;            // Position within its unit
    int         extruder = -1;       // 0-based extruder fed; -1 = unknown
    int         virtual_tool = -1;   // T<n> currently mapped to the slot; -1 = unknown
};

// Orca: filament-changer dialects a Moonraker printer reports slots in, and the g-code each takes.
// AFC addresses lanes by name, Happy Hare gates by index; neither carries vendor or sub-type.
namespace MoonrakerFilamentDialect {
enum class Dialect { none, afc_lane_data, happy_hare };
bool        dialect_supports_push(Dialect dialect);
std::string dialect_name(Dialect dialect);
std::vector<std::string> afc_push_scripts(const IPrinterAgent::FilamentSlotInfo& info);
std::string              happy_hare_push_script(const IPrinterAgent::FilamentSlotInfo& info);
// Print-start scripts: reset the map, assign each used tool, then SD start. tool_to_slot_1based:
// index = 0-based tool, value = 1-based slot, 0 = unassigned. Empty when a used AFC slot has no name.
std::string afc_mapping_start_script(const std::string& filename, const std::vector<int>& tool_to_slot_1based,
                                     const std::vector<std::string>& slot_names);
std::string happy_hare_mapping_start_script(const std::string& filename, const std::vector<int>& tool_to_slot_1based);
// SD start with OPENACE_MAP="[[sliced,virtual],...]" (0-based); no reset, the job map ends with the job.
std::string openace_mapping_start_script(const std::string& filename, const std::vector<int>& tool_to_slot_1based);
// Highest T<n> + 1 in /printer/gcode/help; not the count, since toolchanger T0..T3 have no help
// text and are not listed. 0 = none found.
int tool_count_from_gcode_help(const nlohmann::json& help);
// The lane_data record ("lane" = virtual tool, "unit", optional "unit_name", "slot", "extruder" or
// "extruder_index") was defined together with the AFC developers. A field the record leaves out or
// blank keeps what the tray already holds, so an AFC_stepper status object only fills gaps.
int  extruder_index_from_name(const std::string& name); // "extruder" -> 0, "extruder2" -> 2, else -1
void apply_lane_topology(const nlohmann::json& lane, MoonrakerAmsTrayData& tray);
// Every lane names its unit and its slot in that unit, so no AFC_stepper query is needed.
bool lane_data_has_topology(const nlohmann::json& lanes);
// One lane_data entry; false when it has no lane index. Leaves tray_info_idx to the caller.
bool read_lane_data_entry(const std::string& key, const nlohmann::json& lane, MoonrakerAmsTrayData& tray);
} // namespace MoonrakerFilamentDialect

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
    // Writes use the dialect the last fetch read.
    bool supports_filament_push() const override;
    bool push_filament_info(std::string dev_id, const FilamentSlotInfo& info) override;
    bool bind_device_connection(const std::string& dev_id, const std::string& address, const std::string& access_code, bool use_ssl) override;

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

    // Copied when the stream starts, so changes apply to the next start_status_stream(). A connection's retries read
    // reconnect_delay directly.
    struct StatusStreamTimings
    {
        std::chrono::milliseconds handshake{std::chrono::seconds(10)};      // Opening and closing handshakes
        std::chrono::milliseconds close{std::chrono::seconds(1)};           // Wait for the close frame's answer before dropping the socket
        std::chrono::milliseconds idle{std::chrono::seconds(30)};           // Silence before giving up, with a ping every half of it
        std::chrono::milliseconds heartbeat{std::chrono::seconds(10)};      // Silence before the cached status is sent again
        std::chrono::milliseconds reconnect_delay{std::chrono::seconds(1)}; // First wait, doubling per consecutive failure up to 32x
        std::chrono::milliseconds storage_check{std::chrono::seconds(30)};  // How often the free space of the gcodes root is re-read
    } ws_timings;

    using AmsTrayData = MoonrakerAmsTrayData;

    // Toolchanger: one 1-slot unit per lane, ams_count = lane count.
    // Box4: one AMS_LITE unit per 4 lanes, ams_count = box count.
    enum class AmsUnitShape { Toolchanger, Box4 };

    // Build ams JSON and call parser
    void build_ams_payload(int ams_count, int max_lane_index, const std::vector<AmsTrayData>& trays,
                            AmsUnitShape shape = AmsUnitShape::Box4);
    // Name shown for a filament unit, or empty for the AMS Lite name
    virtual std::string ams_unit_name(int /*ams_id*/) const { return {}; }

    // Methods that derived classes may need to override or access
    virtual bool init_device_info(const std::string& dev_id, const std::string& dev_ip, const std::string& username, const std::string& password, bool use_ssl, const std::string& port);
    // An override must give up its request once `cancelled` returns true, or the destructor waits out its timeout
    virtual bool fetch_device_info(const std::string&           base_url,
                                   const std::string&           api_key,
                                   MoonrakerDeviceInfo&         info,
                                   std::string&                 error,
                                   const std::function<bool()>& cancelled) const;
    // Starts the print of a file already uploaded to the gcodes root
    virtual bool print_uploaded_file(const PrintParams& params, const std::string& filename);
    void start_status_stream(const std::string& dev_id, const std::string& base_url, const std::string& api_key);
    void stop_status_stream();

    // Rebuilds device_info from the MachineObject when dev_id was never bound via
    // bind_device_connection(). No-op if already current.
    bool ensure_device_info(const std::string& dev_id);

    // State access for derived classes
    mutable std::recursive_mutex       state_mutex;

    // Helpers
    bool        is_numeric(const std::string& value);
    std::string normalize_base_url(bool use_ssl, const std::string& host, const std::string& port);
    std::string sanitize_filename(const std::string& filename);
    std::string join_url(const std::string& base_url, const std::string& path) const;
    bool send_gcode_sync(const std::string& dev_id, const std::string& gcode) const;
    // Posts the request to /printer/print/start
    bool send_jsonrpc_command(const std::string& base_url, const std::string& api_key,
                              const nlohmann::json& request, std::string& response) const;

    // Trim whitespace and convert to uppercase
    static std::string trim_and_upper(const std::string& input);

    // Map filament type to OrcaFilamentLibrary preset ID for AMS sync compatibility
    static std::string map_filament_type_to_generic_id(const std::string& filament_type);

    // Blocking GET of a raw body; false on any non-200. Gives up once `cancelled` returns true.
    bool fetch_body(const std::string&           url,
                    const std::string&           api_key,
                    std::string&                 response_body,
                    std::string&                 error,
                    const std::function<bool()>& cancelled = {}) const;
    // GETs a Moonraker endpoint and parses its JSON answer, giving up once `cancelled` returns true
    bool get_json(const std::string&           url,
                  const std::string&           api_key,
                  const std::function<bool()>& cancelled,
                  nlohmann::json&              json,
                  std::string&                 error) const;

private:
    int handle_request(const std::string& dev_id, const std::string& json_str);
    int send_version_info(const std::string& dev_id);
    int send_access_code(const std::string& dev_id);

    bool fetch_object_list(const std::string& base_url, const std::string& api_key, std::set<std::string>& objects, std::string& error) const;
    bool fetch_has_free_storage(const std::string& base_url, const std::string& api_key, bool& has_free, std::string& error) const;
    bool fetch_tool_count(const std::string& base_url, const std::string& api_key, int& tool_count, std::string& error) const;
    // AFC keeps a lane's unit/extruder/map in its Klipper status object, not in lane_data.
    void fetch_afc_lane_topology(std::vector<AmsTrayData>& trays) const;
    bool query_printer_status(const std::string&           base_url,
                              const std::string&           api_key,
                              nlohmann::json&              status,
                              std::string&                 error,
                              const std::function<bool()>& cancelled) const;
    void send_gcode_async(const std::string& dev_id, const std::string& gcode,
                          std::function<void(bool)> on_result = {}) const;

    void announce_printhost_device();
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg);
    void dispatch_printer_connected(const std::string& dev_id);
    void dispatch_message(const std::string& dev_id, const std::string& payload);
    // Joins a stream stopped from its own callback, so a new stream can start without waiting on it under connect_mutex
    void join_stopped_status_stream();
    void run_status_stream(std::string dev_id, std::string base_url, std::string api_key, StatusStreamTimings timings);
    void handle_ws_message(const std::string& dev_id, const std::string& payload);
    void send_heartbeat_if_due(const std::string& dev_id, std::chrono::milliseconds interval);
    void dispatch_status(const std::string& dev_id);
    // Re-reads whether the gcodes root has free space, and returns whether the answer changed
    bool refresh_storage(const std::string& base_url, const std::string& api_key);
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

    // Connection thread management
    void perform_connection_async(const std::string& dev_id,
                                   const std::string& base_url,
                                   const std::string& api_key,
                                   uint64_t generation);

    // System-specific filament fetch methods
    bool fetch_hh_filament_info(std::vector<AmsTrayData>& trays, int& max_lane_index);
    // `has_topology`: every lane carries its unit and slot (lane_data_has_topology).
    bool fetch_moonraker_filament_data(std::vector<AmsTrayData>& trays, int& max_lane_index, bool& has_topology);
    // Dialect of the last successful fetch; published with the slots so a send can re-confirm it.
    MoonrakerFilamentDialect::Dialect m_filament_dialect = MoonrakerFilamentDialect::Dialect::none;
    // Logical tools registered at the last fetch (0 = not probed).
    int m_tool_count = 0;

protected:
    // JSON helper methods, shared with the agents deriving from this one
    static std::string safe_json_string(const nlohmann::json& obj, const char* key);
    static int safe_json_int(const nlohmann::json& obj, const char* key);
    static std::string safe_array_string(const nlohmann::json& arr, int idx);
    static int safe_array_int(const nlohmann::json& arr, int idx);
    static std::string normalize_color_value(const std::string& color);

private:

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
    std::atomic<bool>      has_storage{false}; // The gcodes root has free space

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

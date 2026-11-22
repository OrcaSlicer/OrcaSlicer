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

    // The camera found at connection: the first enabled webcam Moonraker lists
    CameraStreamMode get_camera_stream_mode() const override;
    std::string      get_camera_url() const override;

    struct CameraSource
    {
        CameraStreamMode mode = CameraStreamMode::none;
        std::string      url;
    };
    // The first enabled webcam of a /server/webcams/list answer: its MJPEG or RTSP stream when the player can open it,
    // else its snapshot. Relative URLs are resolved against the origin of base_url, as the web frontends do.
    static CameraSource pick_webcam(const nlohmann::json& webcams, const std::string& base_url);
    // The status report MachineObject parses, from the cached Moonraker objects
    // chamber_led: the Klipper LED shown as the chamber light, or empty for none;
    // chamber_sensor: the Klipper object whose temperature is the chamber's, or empty for none
    static nlohmann::json build_print_payload(const nlohmann::json&        status,
                                              const std::set<std::string>& objects,
                                              bool                         has_storage,
                                              const std::string&           chamber_led    = {},
                                              const std::string&           chamber_sensor = {});
    // The speed factor (percent) M220 sets for a Device tab speed level (1 silent .. 4 ludicrous), and back
    static int speed_percent_for_level(int level);
    // The URL of the largest thumbnail in a /server/files/metadata answer for a file in the gcodes root, or empty
    static std::string thumbnail_url(const nlohmann::json& metadata, const std::string& base_url, const std::string& filename);
    static int speed_level_for_factor(double speed_factor);

    // JSON helper methods
    static std::string safe_json_string(const nlohmann::json& obj, const char* key);
    static int safe_json_int(const nlohmann::json& obj, const char* key);
    static std::string safe_array_string(const nlohmann::json& arr, int idx);
    static int safe_array_int(const nlohmann::json& arr, int idx);

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

    // Tray data for AMS payload building
    struct AmsTrayData {
        int         slot_index = 0;      // 0-based slot index
        bool        has_filament = false;
        std::string tray_type;           // Material type (e.g., "PLA", "ASA")
        std::string tray_color;          // Raw color (#RRGGBB, 0xRRGGBB, or RRGGBBAA)
        std::vector<std::string> tray_colors; // Optional: every color of a multi-color spool, in order (same formats).
                                              // Used when it holds two or more; otherwise tray_color is the only one.
        bool        colors_gradient = false;  // With tray_colors: blend them as a gradient instead of drawing segments
        std::string tray_info_idx;       // Filament ID (optional)
        int         bed_temp = 0;        // Optional
        int         nozzle_temp = 0;     // Optional
    };

    // Build ams JSON and call parser
    void build_ams_payload(int ams_count, int max_lane_index, const std::vector<AmsTrayData>& trays);
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
    // The printer's camera, read once per connection; an override must give up once `cancelled` returns true
    virtual CameraSource fetch_camera(const std::string& base_url, const std::string& api_key, const std::function<bool()>& cancelled) const;
    // The Klipper LED (the name after "led ") the Device tab switches as the chamber light; empty for none
    virtual std::string chamber_led() const { return {}; }
    // The Klipper object (full name) that reads the chamber temperature
    virtual std::string chamber_sensor() const { return "temperature_sensor chamber"; }
    // Starts the print of a file already uploaded to the gcodes root
    virtual bool print_uploaded_file(const PrintParams& params, const std::string& filename);
    // Whether the printer prints a .gcode.3mf bundle as is (its use_3mf option)
    virtual bool accepts_3mf() const;
    void start_status_stream(const std::string& dev_id, const std::string& base_url, const std::string& api_key);
    void stop_status_stream();

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

private:
    int handle_request(const std::string& dev_id, const std::string& json_str);
    int send_version_info(const std::string& dev_id);
    int send_access_code(const std::string& dev_id);

    // GETs a Moonraker endpoint and parses its JSON answer, giving up once `cancelled` returns true
    bool get_json(const std::string&           url,
                  const std::string&           api_key,
                  const std::function<bool()>& cancelled,
                  nlohmann::json&              json,
                  std::string&                 error) const;
    bool fetch_object_list(const std::string& base_url, const std::string& api_key, std::set<std::string>& objects, std::string& error) const;
    bool fetch_has_free_storage(const std::string& base_url, const std::string& api_key, bool& has_free, std::string& error) const;
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
    bool fetch_moonraker_filament_data(std::vector<AmsTrayData>& trays, int& max_lane_index);
    bool fetch_creality_cfs_data(std::vector<AmsTrayData>& trays, int& max_lane_index);

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

    // Follows the printed file's thumbnail; stream thread only
    void update_job_thumbnail(const std::string& base_url, const std::string& api_key);
    std::string        job_thumbnail_file;   // the file job_thumbnail_url is for
    std::string        job_thumbnail_failed; // the file whose metadata read failed, tried again at job_thumbnail_retry
    std::chrono::steady_clock::time_point job_thumbnail_retry;
    std::string        job_thumbnail_url; // guarded by payload_mutex

    mutable std::mutex camera_mutex;
    CameraSource       camera;

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

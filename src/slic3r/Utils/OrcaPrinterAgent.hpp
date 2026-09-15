#ifndef __ORCA_PRINTER_AGENT_HPP__
#define __ORCA_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"
#include "OrcaCloudServiceAgent.hpp"
#include "OrcaMqttConnection.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <mutex>
#include <memory>
#include <thread>

namespace Slic3r {

class OrcaCloudServiceAgent;

/**
 * OrcaPrinterAgent - OrcaSonar MQTT printer agent.
 *
 * LAN and cloud commands use the same OrcaSonar protocol payloads; only the
 * MQTT connection selected by route_send() differs.
 */
class OrcaPrinterAgent : public IPrinterAgent
{
public:
    explicit OrcaPrinterAgent(std::string log_dir);
    ~OrcaPrinterAgent() override;

    // ========================================================================
    // IPrinterAgent Interface Implementation
    // ========================================================================

    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;
    CameraStreamMode get_camera_stream_mode() const override;
    std::string get_camera_url() const override;
    std::unique_ptr<ICameraSignalingChannel>
    create_camera_signaling_channel(const std::string& dev_id) override;

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
    int bind(std::string dev_ip,
             std::string dev_id,
             std::string dev_model,
             std::string sec_link,
             std::string timezone,
             bool improved,
             OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    int request_bind_ticket(std::string* ticket) override;
    int get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine Selection
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;

    /**
     * Get agent information.
     *
     * @return AgentInfo struct containing agent identification and descriptive information
     */
    static AgentInfo get_agent_info_static();
    AgentInfo get_agent_info() override { return get_agent_info_static(); }

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

    int command_ams_refresh_rfid(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode) override;
    int command_ams_calibrate(std::string dev_id, int ams_id, int sequence_id, bool lan_mode) override;
    int command_ams_select_tray(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode) override;
    int command_start_camera(std::string dev_id) override;
    int command_xyz_abs(std::string dev_id, int sequence_id, bool lan_mode) override;
    int command_auto_leveling(std::string dev_id, int sequence_id, bool lan_mode) override;
    int command_go_home(std::string dev_id, bool is_printing, bool supports_mqtt_homing, int sequence_id, bool lan_mode) override;
    int command_set_bed(std::string dev_id, int temp, bool supports_mqtt_bed_ctrl, int sequence_id, bool lan_mode) override;
    int command_set_nozzle(std::string dev_id, int temp, int sequence_id, bool lan_mode) override;
    int command_axis_control(std::string dev_id,
                             std::string axis,
                             double unit,
                             double input_val,
                             int speed,
                             bool is_core_xy,
                             bool supports_mqtt_axis_control,
                             int sequence_id,
                             bool lan_mode) override;

    // Test-only: drive emit_connect_sequence directly (no socket).
    void run_connect_sequence_for_test(const std::string& dev_id)
    {
        emit_connect_sequence(dev_id, [](const std::string&) {}, [](const std::string&) {});
    }

    // Test-only: advance the LAN connection epoch without a connect/disconnect cycle.
    void bump_lan_generation_for_test() { ++m_lan_generation; }
    // Test-only: the same for the (independent) cloud selection epoch.
    void bump_cloud_generation_for_test() { ++m_cloud_generation; }

protected:
    // Forward one inbound printer message to on_message_fn or on_local_message_fn (marshalled onto the UI
    // thread via queue_on_main_fn when set). Body of every connection's MessageHandler.
    void deliver_to_sink(const std::string& dev_id, const std::string& payload, bool local);

    // Extract OrcaSonar's print.ipcam.stream_mode from LAN reports before they
    // are forwarded to the GUI. The getters below then read this agent-owned state.
    void parse_ipcam_info(const std::string& dev_id, const std::string& payload);

    // Orca-dialect -> Bambu-dialect compatibility shim for inbound reports: the single
    // place Orca Protocol JSON is rewritten into the shapes MachineObject::parse_json
    // already handles, so parse_json needs no Orca-specific changes. Self-contained
    // (its cache is a function-local static) and deletable together with its call site
    // once parse_json reads the Orca dialect natively. See the definition for the
    // per-rule detail. Returns the payload unchanged when no rule applies.
    std::string merge_capabilities(const std::string& dev_id, const std::string& payload);

    // Report the asynchronous LAN connection state using the same callback contract as
    // the other printer agents. The transport result cannot be returned by
    // connect_printer(), which only starts the worker.
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& message);

    // The LAN inbound-message handler for one connection generation: forwards to
    // deliver_to_sink only while `generation` is still the live epoch.
    std::function<void(const std::string&, const std::string&)> make_lan_message_handler(uint64_t generation);

    // Pure LAN-address parsing + client-id. protected static so the test Probe reaches them.
    static bool parse_lan_endpoint(const std::string& dev_ip, std::string& host, std::string& port);
    static std::string make_lan_client_id(const std::string& dev_id);
    // Test hook: the ws:// URL connect_printer built for the current LAN session ("" if none).
    std::string lan_connection_target() const;
    // Shared post-connect sequence: SUBSCRIBE, then pushing.start, pushall,
    // info.get_version, info.get_capabilities. Runs identically on LAN and cloud.
    void on_connected(const std::string& dev_id, OrcaMqttConnection* conn, uint64_t generation);

    // The post-connect command sequence, factored behind a seam so a test can
    // observe the SUBSCRIBE + 4 request payloads without a live OrcaMqttConnection.
    virtual void emit_connect_sequence(const std::string& dev_id,
                                       std::function<void(const std::string&)> subscribe,
                                       std::function<void(const std::string&)> request);
    static std::string seq(int n); // decimal string in the OrcaSlicer 20000..29999 band
    static std::string build_pushing_start(const std::string& sequence_id);
    static std::string build_pushing_stop(const std::string& sequence_id);
    static std::string build_pushall(const std::string& sequence_id);
    static std::string build_get_version(const std::string& sequence_id);
    static std::string build_get_capabilities(const std::string& sequence_id);

private:
    class OrcaSonarDiscovery;

    std::string log_dir;
    std::string selected_machine;

    enum CurrentConn { NONE, CLOUD, LAN };
    static const char* connection_type_name(CurrentConn connection);

    // The transport for the printer currently selected by the UI.  LAN and
    // cloud sessions have separate connection objects, so this is selection
    // state rather than an inference from whichever socket happens to exist.
    CurrentConn m_current_connection = NONE;

    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;
    std::unique_ptr<OrcaMqttConnection> lan_mqtt_connection;

    // Two independent epochs: a cloud (de)selection must not fence the live LAN
    // feed, and vice versa. Each transport's connect thread and inbound handler
    // compare against their own counter only.
    std::atomic<uint64_t> m_lan_generation{0};
    std::atomic<uint64_t> m_cloud_generation{0};

    // The short-lived threads that run the blocking initial connect for the current
    // LAN / cloud session. Joined members (never detached) so they cannot outlive
    // *this or the connection they hold a raw pointer to.
    std::thread m_lan_connect_thread;
    std::thread m_cloud_connect_thread;

    std::unique_ptr<OrcaSonarDiscovery> m_discovery;

    std::string m_lan_dev_id; // guarded by state_mutex
    std::string m_lan_url;    // guarded by state_mutex — the Config.url of the live LAN session
    CameraStreamMode m_camera_stream_mode = CameraStreamMode::none; // guarded by state_mutex
    std::string m_camera_url; // guarded by state_mutex

    OrcaCloudServiceAgent* get_orca_cloud_agent();

    OrcaMqttConnection* get_appropriate_mqtt_connection(bool is_lan = true);
    static bool parse_nonnegative_command_id(const std::string& value, int& result);

    // Route one command payload to device/<dev_id>/request on the LAN or the shared
    // cloud connection. The uniform send path for both send_message* overrides.
    int route_send(bool is_lan, const std::string& dev_id, const std::string& json_str);

    // Callbacks
    OnMsgArrivedFn on_ssdp_msg_fn;
    OnPrinterConnectedFn on_printer_connected_fn;
    GetSubscribeFailureFn on_subscribe_failure_fn;
    OnMessageFn on_message_fn;
    OnMessageFn on_user_message_fn;
    OnLocalConnectedFn on_local_connect_fn;
    OnMessageFn on_local_message_fn;
    QueueOnMainFn queue_on_main_fn;
    OnServerErrFn on_server_err_fn;

    mutable std::mutex state_mutex;
};

} // namespace Slic3r

#endif // __ORCA_PRINTER_AGENT_HPP__

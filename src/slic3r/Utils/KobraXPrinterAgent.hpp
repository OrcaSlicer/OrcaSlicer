#ifndef slic3r_KobraXPrinterAgent_hpp_
#define slic3r_KobraXPrinterAgent_hpp_

#include "IPrinterAgent.hpp"
#include "KobraX.hpp"
#include "bambu_networking.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r {

namespace KobraX { class Session; }

// Printer agent for the Anycubic Kobra X over its LAN protocol (see KobraX.hpp). The printer has no web
// interface, so the Device tab is the native one: printer reports become the Bambu status message
// MachineObject parses, with the ACE units shown as AMS units.
//
// Phase 1: connection, status, ACE slots, camera, upload and print start, pause, resume, stop and the
// light, the Device tab's temperatures, part fan, speed mode, homing and jogs, and the camera start. Feeding
// and AMS settings are refused with ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED.
class KobraXPrinterAgent final : public IPrinterAgent, public std::enable_shared_from_this<KobraXPrinterAgent>
{
public:
    explicit KobraXPrinterAgent(std::string log_dir);
    ~KobraXPrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override { (void) cloud; }

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(const PrinterConnectionParams& params) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    bool start_discovery(bool start, bool sending) override;
    int  bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int  set_server_callback(OnServerErrFn fn) override { (void) fn; return BAMBU_NETWORK_SUCCESS; }

    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override { (void) fn; return BAMBU_NETWORK_SUCCESS; }
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override { (void) fn; return BAMBU_NETWORK_SUCCESS; }
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::subscription; }
    CameraStreamMode get_camera_stream_mode() const override;
    int              command_start_camera(std::string dev_id) override;
    std::string      get_camera_url() const override;

    // A file name the printer accepts, ending in .gcode
    static std::string upload_name(const std::string& project_name, const std::string& path);
    // Whether a print/report settles a print start: 1 it runs, 0 it ended, -1 not decided by this report
    static int start_outcome(const std::string& suffix, const nlohmann::json& report);

private:
    struct Connection;

    void run(std::shared_ptr<Connection> connection);
    void on_report(const std::string& suffix, const nlohmann::json& payload);
    int  handle(const std::string& dev_id, const std::string& json_str);
    int  upload(const PrintParams& params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string& name, uint64_t& size);
    void dispatch_status();
    void dispatch(const std::string& dev_id, const nlohmann::json& message);
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg);
    std::shared_ptr<KobraX::Session> session() const;

    mutable std::mutex m_mutex;
    KobraX::State      m_state;
    std::shared_ptr<Connection> m_connection;
    std::string        m_selected;
    std::string        m_camera_started; // the stream URL video/startCapture was last sent for

    OnMsgArrivedFn       m_on_ssdp;
    OnPrinterConnectedFn m_on_printer_connected;
    OnMessageFn          m_on_message;
    OnLocalConnectedFn   m_on_local_connect;
    OnMessageFn          m_on_local_message;
    QueueOnMainFn        m_queue_on_main;
};

} // namespace Slic3r

#endif

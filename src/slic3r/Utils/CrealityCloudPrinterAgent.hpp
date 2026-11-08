#pragma once

#include "CrealityCloud.hpp"
#include "IPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

class ICloudServiceAgent;

// Prints to Creality printers bound to the user's Creality Cloud account, from anywhere.
//
// Discovery lists every printer on the account (one ssdp message each, refreshed while discovery runs),
// so the Device tab shows them all. Connecting starts a poll of the chosen printer that is translated
// into the Bambu-shaped push_status the device panels read. A print is uploaded to Creality Cloud and
// started with addSingleTask; the printer then downloads it itself, so it works off the local network.
// LAN printing to the same printers stays with CrealityPrintAgent ("crealityprint").
class CrealityCloudPrinterAgent : public IPrinterAgent
{
public:
    static constexpr const char* AGENT_ID = "creality_cloud";

    explicit CrealityCloudPrinterAgent(std::string log_dir);
    ~CrealityCloudPrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(const PrinterConnectionParams& params) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    bool start_discovery(bool start, bool sending) override;
    int  bind_detect(std::string, std::string, detectResult&) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int  set_server_callback(OnServerErrFn fn) override;

    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

private:
    int  cloud_print(const PrintParams& params, bool start, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
    bool find_device(const std::string& dn, CrealityCloud::Device& device);
    int  control(const std::string& dev_id, const nlohmann::json& params);

    void discovery_loop();
    void status_loop(std::string dev_id, uint64_t generation);
    void stop_discovery();
    void stop_status();

    void post(std::function<void()> fn);
    void dispatch_message(const std::string& dev_id, const std::string& payload);

    std::string m_log_dir;

    mutable std::recursive_mutex        m_mutex;
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;
    std::string                         m_selected;
    std::map<std::string, CrealityCloud::Device> m_devices; // Last device list, by dn

    OnMsgArrivedFn        m_on_ssdp;
    OnPrinterConnectedFn  m_on_printer_connected;
    GetSubscribeFailureFn m_on_subscribe_failure;
    OnMessageFn           m_on_message;
    OnMessageFn           m_on_user_message;
    OnLocalConnectedFn    m_on_local_connect;
    OnMessageFn           m_on_local_message;
    QueueOnMainFn         m_queue_on_main;
    OnServerErrFn         m_on_server_err;

    std::atomic<bool> m_discovery_stop{true};
    std::thread       m_discovery_thread;

    std::atomic<uint64_t> m_status_generation{0};
    std::thread           m_status_thread;
};

} // namespace Slic3r

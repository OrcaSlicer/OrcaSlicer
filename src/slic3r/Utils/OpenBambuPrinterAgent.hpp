#pragma once

// OpenBambuPrinterAgent - Plugin-free LAN printing for BambuLab printers.
//
// This agent implements IPrinterAgent to enable direct LAN communication
// with BambuLab printers WITHOUT requiring the proprietary network plugin.
// It supports printers in Developer/LAN-only mode.
//
// Protocol knowledge derived from:
//   https://github.com/ClusterM/open-bamboo-networking (AGPL-3.0)
//   Copyright (C) 2026 Alexey Cluster and contributors
//   https://github.com/Doridian/OpenBambuAPI
//
// Key protocols implemented:
//   - SSDP discovery on 239.255.255.250:1990
//   - MQTT over TLS on port 8883 (user "bblp", password = access code)
//   - FTPS upload on port 990 (implicit TLS)
//
// Requirements:
//   - Printer must be in Developer Mode (LAN Only Mode)
//   - Printer access code must be configured in OrcaSlicer

#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace Slic3r {

// Forward declarations
class OpenBambuMqtt;
class OpenBambuSsdp;

class OpenBambuPrinterAgent : public IPrinterAgent {
public:
    explicit OpenBambuPrinterAgent(std::string log_dir);
    ~OpenBambuPrinterAgent() override;

    // ========================================================================
    // IPrinterAgent Interface Implementation
    // ========================================================================

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

    // Binding (not supported in LAN-only mode without cloud)
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    int request_bind_ticket(std::string* ticket) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine Selection
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;

    // Agent Info
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

    // Filament
    FilamentSyncMode get_filament_sync_mode() const override;

private:
    // Helper for local print flow (shared between start_local_print variants)
    int do_local_print(PrintParams& params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
    std::string build_print_command_json(const PrintParams& params, const std::string& remote_filename, const std::string& md5);

    std::string m_log_dir;
    std::string m_selected_machine;
    std::string m_connected_dev_id;
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;

    // Protocol implementations
    std::unique_ptr<OpenBambuMqtt> m_mqtt;
    std::unique_ptr<OpenBambuSsdp> m_ssdp;

    // Callbacks
    OnMsgArrivedFn       m_on_ssdp_msg_fn;
    OnPrinterConnectedFn m_on_printer_connected_fn;
    GetSubscribeFailureFn m_on_subscribe_failure_fn;
    OnMessageFn          m_on_message_fn;
    OnMessageFn          m_on_user_message_fn;
    OnLocalConnectedFn   m_on_local_connect_fn;
    OnMessageFn          m_on_local_message_fn;
    QueueOnMainFn        m_queue_on_main_fn;
    OnServerErrFn        m_on_server_err_fn;

    mutable std::mutex m_mutex;
};

} // namespace Slic3r

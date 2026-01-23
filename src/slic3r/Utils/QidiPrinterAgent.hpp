#ifndef __QIDI_PRINTER_AGENT_HPP__
#define __QIDI_PRINTER_AGENT_HPP__

#include "OrcaPrinterAgent.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {

class QidiPrinterAgent final : public OrcaPrinterAgent
{
public:
    explicit QidiPrinterAgent(std::string log_dir);
    ~QidiPrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    bool start_discovery(bool start, bool sending) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::pull; }
    void fetch_filament_info(std::string dev_id) override;

private:
    struct PrinthostConfig
    {
        std::string host;
        std::string port;
        std::string api_key;
        std::string base_url;
        std::string model_id;
        std::string model_name;
    };

    struct QidiDeviceInfo
    {
        std::string dev_id;
        std::string dev_name;
        std::string model_id;
        std::string version;
    };

    struct QidiSlotInfo
    {
        int  slot_index      = 0;
        int  color_index     = 0;
        int  filament_type   = 0;
        int  vendor_type     = 0;
        bool filament_exists = false;
    };

    struct QidiFilamentDict
    {
        std::map<int, std::string> colors;
        std::map<int, std::string> filaments;
    };

    int handle_request(const std::string& dev_id, const std::string& json_str);
    int send_version_info(const std::string& dev_id);
    int send_access_code(const std::string& dev_id);

    bool get_printhost_config(PrinthostConfig& config) const;
    bool fetch_device_info(const std::string& base_url, const std::string& api_key, QidiDeviceInfo& info, std::string& error) const;
    bool fetch_server_info(const std::string& base_url, const std::string& api_key, std::string& version, std::string& error) const;
    bool fetch_object_list(const std::string& base_url, const std::string& api_key, std::set<std::string>& objects, std::string& error) const;

    std::string resolve_host(const std::string& dev_id) const;
    std::string resolve_api_key(const std::string& dev_id, const std::string& fallback) const;
    void        store_host(const std::string& dev_id, const std::string& host, const std::string& api_key);

    bool fetch_slot_info(const std::string& base_url,
                         const std::string& api_key,
                         std::vector<QidiSlotInfo>& slots,
                         int&                       box_count,
                         std::string&               error) const;
    bool fetch_filament_dict(const std::string& base_url, const std::string& api_key, QidiFilamentDict& dict, std::string& error) const;

    static void parse_ini_section(const std::string& content, const std::string& section_name, std::map<int, std::string>& result);
    static void parse_filament_sections(const std::string& content, std::map<int, std::string>& result);

    static std::string normalize_color(const std::string& color);
    static std::string map_filament_type_to_setting_id(const std::string& filament_type);

    void announce_printhost_device();
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg);
    void dispatch_printer_connected(const std::string& dev_id);
    void dispatch_message(const std::string& dev_id, const std::string& payload);
    void start_status_stream(const std::string& dev_id, const std::string& base_url, const std::string& api_key);
    void stop_status_stream();
    void run_status_stream(std::string dev_id, std::string base_url, std::string api_key);
    void handle_ws_message(const std::string& dev_id, const std::string& payload);
    void update_status_cache(const nlohmann::json& updates);
    nlohmann::json build_print_payload_locked(const nlohmann::json* ams_override) const;

    mutable std::mutex                 state_mutex;
    std::map<std::string, std::string> host_by_device;
    std::map<std::string, std::string> api_key_by_device;
    std::string                        ssdp_announced_host;
    std::string                        ssdp_announced_id;
    OnMsgArrivedFn                     on_ssdp_msg_fn;
    OnPrinterConnectedFn               on_printer_connected_fn;
    OnLocalConnectedFn                 on_local_connect_fn;
    OnMessageFn                        on_message_fn;
    OnMessageFn                        on_local_message_fn;
    QueueOnMainFn                      queue_on_main_fn;

    mutable std::mutex payload_mutex;
    nlohmann::json     status_cache;
    nlohmann::json     last_ams_payload;

    std::atomic<bool> ws_stop{false};
    std::atomic<uint64_t> ws_last_emit_ms{0};
    std::thread       ws_thread;
};

} // namespace Slic3r

#endif

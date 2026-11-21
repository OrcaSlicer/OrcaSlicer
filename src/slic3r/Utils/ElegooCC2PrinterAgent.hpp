#pragma once

#include "FilamentSync.hpp"
#include "IPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class ICloudServiceAgent;
class MachineObject;
class Preset;
class PresetBundle;
class PresetCollection;

// Reads the CANVAS filament slots of an Elegoo Centauri Carbon 2 on demand, over LAN, read-only.
class ElegooCC2PrinterAgent final : public IPrinterAgent
{
public:
    enum class Error {
        None,
        Unauthorized,
        HttpError,
        BadSystemInfo,
        NotCC2,
        ConnectFailed,
        Timeout,
        MqttRejected,
        SubscribeRejected,
        RegisterRejected,
        TooManyClients,
        BadResponse,
        NoCanvas,
        NoTarget,  // No CC2 preset, machine or application state to read for
        Exception, // Something threw during the read
    };

    struct SystemInfo
    {
        std::string sn;
        std::string machine_model;
    };

    struct CanvasTray
    {
        int status = 0;
        std::string brand;
        std::string name;
        std::string type;
        std::string color;
        int max_nozzle_temp = 0;
    };

    using CanvasUnit = std::array<CanvasTray, 4>;

    static Error parse_system_info(unsigned http_status, const std::string& body, SystemInfo& out);
    static Error parse_canvas_info(const nlohmann::json& result, std::vector<CanvasUnit>& out);
    static std::vector<std::array<FilamentSync::AmsSlot, 4>> to_ams_slots(const std::vector<CanvasUnit>& units,
                                                                          const PresetCollection& filaments);

    struct Target
    {
        std::string dev_id;
        std::string host;
        std::string http_port;
        std::string token;
    };

    // The vendor model id of the printer preset, or nullopt when the preset has no printer_model to look up.
    static std::optional<std::string> printer_model_id(Preset& printer, PresetBundle* preset_bundle);
    // The printer the preset points at, when it is a CC2 preset whose host is the device dev_id names.
    static std::optional<Target> resolve_target(const DynamicPrintConfig& printer_cfg,
                                                const std::string& model_id,
                                                const std::string& dev_id);
    // Registers client_id with the printer and reads its CANVAS state (method 2005) before the deadline.
    static Error query_canvas(const std::string& host,
                              const std::string& mqtt_port,
                              const std::string& sn,
                              const std::string& token,
                              const std::string& client_id,
                              std::chrono::steady_clock::time_point deadline,
                              nlohmann::json& canvas_result);
    // Publishes a complete CANVAS read into the machine.
    static void apply_snapshot(MachineObject& obj, const std::vector<std::array<FilamentSync::AmsSlot, 4>>& slots);
    static void mark_snapshot_ready(MachineObject& obj);
    // Leaves the trays alone; only the readiness of the snapshot is withdrawn.
    static void mark_snapshot_failed(MachineObject& obj);

    explicit ElegooCC2PrinterAgent(std::string log_dir);

    static AgentInfo get_agent_info_static();
    AgentInfo get_agent_info() override { return get_agent_info_static(); }
    const std::string& client_id() const { return m_client_id; }

    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(const PrinterConnectionParams& params) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    bool start_discovery(bool start, bool sending) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int set_server_callback(OnServerErrFn fn) override;
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;
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

    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::pull; }
    // Runs on the GUI thread and returns within about three seconds.
    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

private:
    // One id per agent, and the factory keeps one agent per application session.
    const std::string m_client_id;

    std::mutex m_mutex;
    std::string m_selected_machine;
};

} // namespace Slic3r

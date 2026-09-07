#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace Slic3r {

class SnapmakerPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit SnapmakerPrinterAgent(std::string log_dir);
    ~SnapmakerPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;
    int command_start_camera(std::string dev_id) override;

    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::subscription; }
    // CameraStreamMode get_camera_stream_mode() const override { return CameraStreamMode::http_snapshot; }
    CameraStreamMode get_camera_stream_mode() const override { return CameraStreamMode::rtsp; }
    // std::string get_camera_url() const override { return device_info.base_url + "/server/files/camera/monitor.jpg"; }
    std::string get_camera_url() const override { return "rtsp://100.99.196.114:8554/stream"; }

private:
    // Combine filament_type + filament_sub_type into a unified type string
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);

    void start_camera_monitor();
    void on_status_loop_tick(const std::string& dev_id) override;

    std::atomic<int64_t> m_camera_last_fire_ms{0};
};

} // namespace Slic3r

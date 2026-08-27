#pragma once

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
    FilamentSyncMode get_filament_sync_mode() const override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int command_start_camera(std::string dev_id) override;

private:
    // Combine filament_type + filament_sub_type into a unified type string
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);

    void start_camera_monitor();
    void on_status_loop_tick(const std::string& dev_id) override;

    std::atomic<int64_t> m_camera_last_fire_ms{0};
};

} // namespace Slic3r

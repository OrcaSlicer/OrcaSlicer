#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {

class PresetCollection;

class SnapmakerPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit SnapmakerPrinterAgent(std::string log_dir);
    ~SnapmakerPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    // Pick the filament_id of the closest visible base preset for a spool's vendor, type, sub type
    // and colour. Static so the matcher can be exercised without a printer.
    static std::string find_closest_color_preset_by_vendor_and_type(const PresetCollection& filaments,
                                                                    const std::string&      vendor_name,
                                                                    const std::string&      filament_type,
                                                                    const std::string&      sub_type,
                                                                    const std::string&      color_rgba);

    // Combine filament_type + filament_sub_type into a unified type string
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);

    // Records that the Device tab is showing the camera, which keeps the stock firmware's picture current
    std::string get_camera_url() const override;

    // The stock firmware's camera picture, which it renews only while asked to
    static constexpr const char* STOCK_CAMERA_PATH = "/server/files/camera/monitor.jpg";
    // camera.start_monitor when the Device tab showed the camera within `watching` and the last request is
    // `keepalive` old, else nothing. Static so the schedule can be checked without a printer.
    static bool camera_request_due(std::chrono::steady_clock::time_point now,
                                   std::chrono::steady_clock::time_point last_watched,
                                   std::chrono::steady_clock::time_point last_request);

protected:
    std::string                 chamber_led() const override { return "cavity_led"; }
    std::string                 chamber_sensor() const override { return "temperature_sensor cavity"; }
    std::set<std::string>       filament_objects() const override { return {"print_task_config", "filament_detect"}; }
    CameraSource                fetch_camera(const std::string& base_url, const std::string& api_key,
                                             const std::function<bool()>& cancelled) const override;
    std::vector<nlohmann::json> take_ws_requests() override;

private:
    mutable std::atomic<std::chrono::steady_clock::rep> m_camera_watched{0};
    std::chrono::steady_clock::time_point               m_camera_requested{};
};

} // namespace Slic3r

#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <atomic>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

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

    // Whether the firmware starts prints with server.files.start_local_print, which applies the print options and the
    // filament mapping. Snapmaker's own client uses it from firmware 1.3.0 on.
    static bool firmware_supports_print_options(const std::string& firmware_version);

    // The MAP_TABLE option: a [filament, toolhead] pair for each filament the print dialog mapped to a toolhead.
    // `ams_mapping` holds one tray index per filament, negative when unmapped; the U1's slots 0 to 3 are its toolheads.
    static nlohmann::json build_map_table(const std::string& ams_mapping);

    // The params of the server.files.start_local_print request for a file uploaded to the gcodes root
    static nlohmann::json build_start_local_print_params(const PrintParams& params, const std::string& filename);

protected:
    bool init_device_info(const std::string& dev_id,
                          const std::string& dev_ip,
                          const std::string& username,
                          const std::string& password,
                          bool               use_ssl,
                          const std::string& port) override;
    bool fetch_device_info(const std::string&           base_url,
                           const std::string&           api_key,
                           MoonrakerDeviceInfo&         info,
                           std::string&                 error,
                           const std::function<bool()>& cancelled) const override;
    bool print_uploaded_file(const PrintParams& params, const std::string& filename) override;
    void add_print_capabilities(nlohmann::json& print) const override;

private:
    // Set from the firmware version each time the printer connects
    mutable std::atomic<bool> print_options_supported{false};
};

} // namespace Slic3r

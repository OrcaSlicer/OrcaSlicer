#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <string>

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
};

} // namespace Slic3r

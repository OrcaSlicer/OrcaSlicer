#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <string>

namespace Slic3r {

namespace SnapmakerProtocol {
// CARD_UID as a 16-hex-digit tag id. The U1 sends an array of tag bytes for a tagged spool and the
// number 0 otherwise; returns "" when there is no tag or the shape is unknown.
std::string card_uid_hex(const nlohmann::json& nfc_slot);
} // namespace SnapmakerProtocol

class SnapmakerPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit SnapmakerPrinterAgent(std::string log_dir);
    ~SnapmakerPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

private:
    // Combine filament_type + filament_sub_type into a unified type string
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);
};

} // namespace Slic3r

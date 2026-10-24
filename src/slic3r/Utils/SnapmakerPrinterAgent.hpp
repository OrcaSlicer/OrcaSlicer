#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "PrintHost.hpp"
#include "bambu_networking.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// Snapmaker's SDCARD_PRINT_FILE_WITH_PARAMETERS start macro, as the touchscreen issues it. Both the
// agent and print-host send paths render it here so they cannot drift apart.
namespace SnapmakerProtocol {
// MAP_TABLE wire format: "[[0, 2], [1, 1], ...]", 0-based (filament, tool) pairs over all filaments.
std::string render_map_table(const std::vector<int>& filament_map_1based);

// filename may be PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER when the stored name is not known yet.
// Always sends BED_LEVEL: the firmware reads an absent parameter as off.
std::string build_start_script(const std::string& filename, const std::vector<int>& filament_map_1based);

// Every parameter a touchscreen start sends. NOZZLE_TEMP/FILAMENT_*/USED_* are per logical filament;
// NOZZLE_DIAMETER_LIST/FLOW_CALIBRATE_EXTRUDERS/END_UNLOAD_FILAMENT are per physical tool.
std::string build_start_script(const std::string& filename, const DevicePrintJobInfo& job);

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

    bool supports_filament_push() const override { return true; }
    bool push_filament_info(std::string dev_id, const FilamentSlotInfo& info) override;

    // The mapping is stashed and delivered on the print-start call.
    bool supports_print_time_mapping() const override { return true; }

protected:
    // Starts the print with the stashed filament map, if one was sent; AFC's own start when AFC is active
    bool print_uploaded_file(const PrintParams& params, const std::string& filename) override;

private:
    // Combine filament_type + filament_sub_type into a unified type string
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);
};

} // namespace Slic3r

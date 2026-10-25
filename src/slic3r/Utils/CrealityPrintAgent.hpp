#ifndef __CREALITY_PRINT_AGENT_HPP__
#define __CREALITY_PRINT_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class PresetCollection;

// Filament sync for Creality K-series printers with CFS.
//
// Inherits MoonrakerPrinterAgent for all communication / certificates / discovery /
// binding / print-job operations. Overrides fetch_filament_info() to query the
// K-series CFS over its port-9999 WebSocket, convert each loaded slot to an
// AmsTrayData entry, and publish via the base-class build_ams_payload() — the
// same shape used by QidiPrinterAgent and SnapmakerPrinterAgent.
//
// Model detection delegated to CrealityPrint::supports_multi_color_print() (PR #13291).
// For non-CFS K-series boards or when the WS query fails, falls back to the base
// MoonrakerPrinterAgent behaviour.
//
// Slot edits made in the Device tab arrive as Orca's "ams_filament_setting" command;
// they are written back to the CFS with the same "modifyMaterial" command Creality
// Print sends, then read back to verify and republish the slots.

class CrealityPrintAgent final : public MoonrakerPrinterAgent
{
public:
    struct CFSSlot
    {
        int         box_id  = 0;   // CFS unit index (0 for first box, 1 for chained second box)
        int         slot_id = 0;   // Slot index within the box (0..3)
        std::string color_hex;     // "#RRGGBB"
        std::string filament_type; // "PLA", "ABS", "PETG", ...
        std::string brand_name;    // "Hyper PLA", ...
        std::string vendor;        // "Creality", "eSUN", or "" if unknown
    };

    // A slot record to write back. All fields empty clears the slot.
    struct CFSMaterial
    {
        std::string rfid;          // material library id ("00001" = Generic PLA), "" if unknown
        std::string vendor;
        std::string filament_type;
        std::string name;
        std::string color_hex;     // "#RRGGBB"
        int         min_temp = 0;
        int         max_temp = 0;
        double      pressure_advance = 0.;

        bool is_clear() const { return vendor.empty() && filament_type.empty() && name.empty() && color_hex.empty(); }
    };

    explicit CrealityPrintAgent(std::string log_dir);
    ~CrealityPrintAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    // Parse the boxsInfo JSON returned by CrealityPrint::query_boxes_info() into
    // a flat list of loaded slots, plus the count of CFS boxes the printer reports.
    static bool parse_cfs_response(const std::string&    response,
                                   std::vector<CFSSlot>& slots,
                                   int&                  box_count,
                                   std::string&          error);

    // Strip PLA/PETG/... subtype suffixes ("PLA Silk", "PLA+", "ABS Pro") to base
    // type so the preset_bundle->filaments.filament_id_by_type() lookup succeeds.
    static std::string normalize_filament_type(const std::string& filament_type);

    // Score visible compatible filament presets against the CFS spool metadata and
    // return the best-matching filament_id. See implementation for scoring details.
    static std::string match_filament_preset(const PresetCollection& filaments,
                                             const std::string&      vendor,
                                             const std::string&      brand_name,
                                             const std::string&      base_type);

    // Find the raw material record of one slot in a boxsInfo response, whatever its state,
    // addressed the way fetch_filament_info() publishes it: cfs_index counts the active CFS
    // boxes in order, slot_id is the slot within the box. box_id receives the printer's own
    // box id, which is what modifyMaterial expects.
    static bool find_cfs_material(const std::string& response,
                                  int                cfs_index,
                                  int                slot_id,
                                  int&               box_id,
                                  nlohmann::json&    material);

    // Take the slot identity (library id, vendor, type, name) from the printer's material
    // library (the retMaterials answer to reqMaterials): the entry with the preset's name,
    // else the generic entry of the same type. Returns false when nothing fits.
    static bool apply_material_library(const std::string& response, CFSMaterial& material);

    // The modifyMaterial parameters for one slot.
    static nlohmann::json build_modify_material(int box_id, int slot_id, const CFSMaterial& material);

    // Whether a raw slot record (see find_cfs_material) holds what was written.
    static bool material_matches(const nlohmann::json& slot, const CFSMaterial& material);

private:
    int  set_cfs_slot(const nlohmann::json& print_cmd);
    void publish_cfs_slots(const std::vector<CFSSlot>& slots, int box_count);
};

} // namespace Slic3r

#endif

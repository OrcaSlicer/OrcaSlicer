#ifndef __CREALITY_PRINT_AGENT_HPP__
#define __CREALITY_PRINT_AGENT_HPP__

#include "MoonrakerPrinterAgent.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class PresetCollection;
class PresetBundle;
class DynamicPrintConfig;

// Filament sync for Creality K-series printers with CFS.
//
// Inherits MoonrakerPrinterAgent for all communication / certificates / discovery /
// binding / print-job operations. Owns two related entry points:
//
//   * fetch_filament_info() — the agent-driven path; queries CFS and publishes via
//     AmsTrayData / build_ams_payload(). Reached when a MachineObject is bound (BBL
//     concept; not currently created for Creality LAN hosts).
//
//   * sync_filaments_into_ams_list() — the explicit-pull path used by Sidebar's
//     "Sync filaments" button. Same data flow up to the parse step, but writes
//     directly into PresetBundle::filament_ams_list and triggers sync_ams_list()
//     so the UI updates without a MachineObject. Returns a result struct so the
//     caller can show appropriate dialogs without the agent touching wxWidgets.
//
// Model detection delegated to CrealityPrint::supports_multi_color_print() (PR #13291).
// For non-CFS K-series boards or when the WS query fails, falls back to the base
// MoonrakerPrinterAgent behaviour.

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

    explicit CrealityPrintAgent(std::string log_dir);
    ~CrealityPrintAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id) override;

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

    // Return status from sync_filaments_into_ams_list(). The agent code is GUI-free
    // so the caller (Sidebar) decides what dialog (if any) to show for each case.
    struct CFSAmsListResult
    {
        enum Status {
            Success,        // bundle->filament_ams_list populated and sync_ams_list applied
            NotCfsCapable,  // printer model has no CFS, or unreachable at configured IP
            QueryFailed,    // CFS query returned bad JSON / HTTP error (detail set)
            EmptySlots,     // CFS responded but reports no loaded slots
            NoMatches       // slots read but PresetBundle::sync_ams_list returned 0
        };
        Status      status = Success;
        std::string detail;                 // free-text error / extra info, may be empty
        int         box_count = 0;          // number of CFS boxes reported by the printer
        int         loaded_slot_count = 0;  // number of physically loaded slots seen
        int         applied_filament_count = 0; // result of PresetBundle::sync_ams_list
    };

    // Explicit-pull entry point for the Sidebar "Sync filaments" button.
    //
    // Builds a CrealityPrint host from `printer_cfg`, queries CFS slot state over its
    // port-9999 WebSocket, populates `bundle->filament_ams_list`, and triggers
    // `bundle->sync_ams_list()` so the standard preset-reconciliation path runs. Pure
    // data / model work — does not touch wxWidgets; the caller is responsible for
    // showing dialogs and refreshing the UI based on the returned Status.
    //
    // Single source of truth for CFS-to-filament sync (was previously duplicated in
    // Sidebar). The agent-driven fetch_filament_info() path still publishes through
    // AmsTrayData / build_ams_payload() and will become the primary path once Creality
    // K-series hosts have a MachineObject created for them.
    static CFSAmsListResult sync_filaments_into_ams_list(
        const DynamicPrintConfig& printer_cfg,
        PresetBundle*             bundle);
};

} // namespace Slic3r

#endif

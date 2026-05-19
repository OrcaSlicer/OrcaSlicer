#ifndef __CREALITY_PRINT_AGENT_HPP__
#define __CREALITY_PRINT_AGENT_HPP__

#include "MoonrakerPrinterAgent.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class PresetCollection;

// Filament sync for Creality K-series printers with CFS.
//
// Inherits MoonrakerPrinterAgent for all communication / certificates / discovery /
// binding / print-job operations. Overrides fetch_filament_info() to query Creality's
// web-server (port 9999, websocket) for CFS slot state and publish it via the existing
// AmsTrayData / build_ams_payload() path so loaded CFS slots appear in Orca's filament
// UI like a Bambu AMS.
//
// Model detection delegated to CrealityPrint::supports_multi_color_print() (PR #13291).
// For non-CFS K-series boards or when the WS query fails, falls back to the base
// MoonrakerPrinterAgent behaviour.
//
// The CFS-slot parser and preset matcher are also exposed as public statics so the
// Sidebar's manual "Sync from CFS" path can reuse them without going through the
// agent dispatch (which only fires when a MachineObject is bound — a BBL concept that
// doesn't apply to Moonraker-style hosts).

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
};

} // namespace Slic3r

#endif

#ifndef __CREALITY_PRINT_AGENT_HPP__
#define __CREALITY_PRINT_AGENT_HPP__

#include "MoonrakerPrinterAgent.hpp"

#include <string>
#include <vector>

namespace Slic3r {

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

class CrealityPrintAgent final : public MoonrakerPrinterAgent
{
public:
    explicit CrealityPrintAgent(std::string log_dir);
    ~CrealityPrintAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id) override;

private:
    struct CFSSlot
    {
        int         box_id  = 0;   // CFS unit index (0 for first box, 1 for chained second box)
        int         slot_id = 0;   // Slot index within the box (0..3)
        std::string color_hex;     // "#RRGGBB"
        std::string filament_type; // "PLA", "ABS", "PETG", ...
        std::string brand_name;    // "Hyper PLA", ...
        std::string vendor;        // "Creality", "eSUN", or "" if unknown
    };

    static bool parse_cfs_response(const std::string&    response,
                                   std::vector<CFSSlot>& slots,
                                   int&                  box_count,
                                   std::string&          error);

    static std::string normalize_filament_type(const std::string& filament_type);
};

} // namespace Slic3r

#endif

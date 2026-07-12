#ifndef __CREALITY_PRINT_AGENT_HPP__
#define __CREALITY_PRINT_AGENT_HPP__

#include "MoonrakerPrinterAgent.hpp"

#include <string>

namespace Slic3r {

// Filament sync for Creality K-series printers with CFS.
//
// Inherits MoonrakerPrinterAgent for all communication / certificates / discovery /
// binding / print-job operations. Overrides fetch_filament_info() to read the CFS
// through a per-product-line provider (see CrealityCfs.hpp): the K1-series exposes
// it as the Klipper `box` object over Moonraker, the K2-platform as the port-9999
// boxsInfo WebSocket. Each loaded slot is converted to an AmsTrayData entry and
// published via the base build_ams_payload() — the same shape used by
// QidiPrinterAgent and SnapmakerPrinterAgent. Falls back to the base Moonraker
// agent when no CFS is present.

class CrealityPrintAgent final : public MoonrakerPrinterAgent
{
public:
    explicit CrealityPrintAgent(std::string log_dir);
    ~CrealityPrintAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id) override;
};

} // namespace Slic3r

#endif

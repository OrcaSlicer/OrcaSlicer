#pragma once

#include "MoonrakerPrinterAgent.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// Orca: the WonderMaker ZR Ultra family. It speaks Moonraker, but on stock firmware it reports no
// MMU object of any kind: its touchscreen keeps the loaded filaments in config/tmt1.ini as palette
// INDICES into sprite assets, and presence comes from the per-tool filament sensors. That is
// unlike every other machine the Moonraker agent serves, which is exactly why it is its own agent
// rather than another branch inside the shared one -- the shared reader stays about MMU objects,
// and this printer's peculiarities cannot leak into printers we cannot test.
//
// A ZR running the vendor's WonderSync add-on DOES report an MMU object, and its data is richer,
// so the inherited readers are tried first and the file dialect only picks up what they miss.
class WonderMakerPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit WonderMakerPrinterAgent(std::string log_dir);
    ~WonderMakerPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id) override;

private:
    // Reads config/tmt1.ini and the per-tool filament sensors. False when the file is absent or
    // unparseable, i.e. this is not a stock-firmware ZR.
    bool fetch_tmt_filament_info(std::vector<AmsTrayData>& trays, int& max_lane_index);
};

} // namespace Slic3r

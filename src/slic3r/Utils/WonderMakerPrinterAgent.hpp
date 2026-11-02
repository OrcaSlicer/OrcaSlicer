#pragma once

#include "MoonrakerPrinterAgent.hpp"

#include <string>
#include <vector>
#include "IPrinterAgent.hpp"

namespace Slic3r {

// The ZR Ultra's mapping is printer state: Klipper saved variable `box_modify_t<n>` names the box
// tool n pulls from and is consumed on first selection; `_backup` is the durable copy, so both are
// written. Tool numbers are the ones the g-code emits.
namespace WonderMakerProtocol {
// "_SET_TIMELAPSE_SETUP ...\n[HYPERLAPSE ACTION=STOP\n]G30|G31\n"
std::string build_prelude(bool bed_leveling, bool time_lapse);
// box_of_tool_1based: per emitted tool, 1-based box (0 = tool unused). Each line ends in "\n".
std::string build_map_lines(const std::vector<int>& box_of_tool_1based);
std::string build_start_script(const std::string& filename, const std::vector<int>& box_of_tool_1based, bool bed_leveling, bool time_lapse);
} // namespace WonderMakerProtocol

// Orca: WonderMaker ZR Ultra. Stock firmware reports no MMU object; loaded filaments live in
// config/tmt1.ini as palette indices, presence in per-tool sensors. With the WonderSync add-on an
// MMU object exists, so the inherited readers are tried first.
class WonderMakerPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit WonderMakerPrinterAgent(std::string log_dir);
    ~WonderMakerPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

private:
    // False when config/tmt1.ini is absent or unparseable, i.e. not a stock-firmware ZR.
    bool fetch_tmt_filament_info(std::vector<AmsTrayData>& trays, int& max_lane_index);
};

} // namespace Slic3r

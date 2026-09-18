#pragma once

#include "MoonrakerPrinterAgent.hpp"
#include "PrintHost.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// The ZR Ultra's device-owned print-time mapping dialect. Unlike Snapmaker's single macro, the
// mapping is printer STATE: `box_modify_t<n>` is a Klipper saved variable naming the box logical
// tool n pulls from, consumed when that tool is first selected. Verified on hardware 2026-08-10:
// starting a print with T0->box 2 and T1->box 3 left box_modify_t0/t1 changed and t2/t3 untouched,
// with current_extruder reporting 2 while logical T0 was printing. The live variable is consumed
// on first selection while `_backup` is the durable record, so both are written.
//
// This is the ONE place that renders those commands. The tool numbers are DENSE (T0..T(n-1)) --
// the firmware has no macro past the last tool -- which is what FilamentCompaction exists to
// guarantee; see protocol_requires_dense_tool_numbering().
namespace WonderMakerProtocol {
// box_of_tool_1based: one entry per DENSE tool number, 1-based box index as picked in the send
// dialog. filename may be PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER (PrintHost.hpp).
std::string build_start_script(const std::string& filename, const std::vector<int>& box_of_tool_1based, bool bed_leveling, bool time_lapse);
std::string build_start_script(const std::string& filename, const DevicePrintJobInfo& job);
} // namespace WonderMakerProtocol

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

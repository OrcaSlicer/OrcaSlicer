#pragma once

#include "MoonrakerPrinterAgent.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// Snapmaker's device-owned print-time mapping dialect: the SDCARD_PRINT_FILE_WITH_PARAMETERS
// gcode macro, the same call the printer's own touchscreen issues to start a print with a
// filament->tool table folded in. This is the ONE place in the codebase that renders that macro
// string; both the agent send path (SnapmakerPrinterAgent::build_start_print_gcode) and the
// print-host send path (the GUI caller feeding Moonraker's generic start-script upload, see
// Plater.cpp's send_gcode_legacy) build the command through here, so the two paths can never
// drift apart. A free function rather than a static member because neither caller needs (or
// should get) SnapmakerPrinterAgent instance state to build the string.
namespace SnapmakerProtocol {
// Renders a 1-based filament->tool map into the MAP_TABLE wire format -- "[[0, 2], [1, 1], ...]",
// 0-based (filament index, tool index) pairs over the FULL project filament list, ", " separated.
// Exposed (rather than kept file-local) only because the pinned-format test
// (tests/slic3rutils/test_snapmaker_protocol.cpp) needs to check this exact rendering in
// isolation from build_start_script's surrounding macro text.
std::string render_map_table(const std::vector<int>& filament_map_1based);

// filename: the uploaded gcode's storage-relative name -- or, for a caller that doesn't know the
// server-confirmed name yet (the print-host path builds this before the upload runs),
// PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER (PrintHost.hpp): the transport substitutes the real
// name in once the upload confirms it, without needing to know this macro's syntax.
// filament_map_1based: 1-based filament->tool map (full project filament list), rendered
// internally via render_map_table -- the GUI layer hands over the raw pick, never a pre-rendered
// wire-format string, so this is the only place that owns Snapmaker's MAP_TABLE byte layout.
// Also pins BED_LEVEL on: the firmware reads an absent parameter as off, so it must be stated
// explicitly on every start (see the definition for the hardware evidence).
std::string build_start_script(const std::string& filename, const std::vector<int>& filament_map_1based);
} // namespace SnapmakerProtocol

class SnapmakerPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit SnapmakerPrinterAgent(std::string log_dir);
    ~SnapmakerPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id) override;

    // Write path: the printer accepts SET_PRINT_FILAMENT_CONFIG via the local Moonraker
    // gcode-script endpoint (same command its own device UI sends over MQTT).
    bool supports_filament_push() const override { return true; }
    bool push_filament_info(std::string dev_id, const FilamentSlotInfo& info) override;

    // Print-time mapping: the U1's mapping is owned by device firmware, delivered on the
    // print-start call rather than as its own message -- see send_filament_mapping()'s doc in
    // IPrinterAgent.hpp for the general contract.
    bool supports_print_time_mapping() const override { return true; }
    bool send_filament_mapping(const std::string& dev_id, const std::vector<int>& tool_to_slot) override;

protected:
    // Folds the mapping send_filament_mapping() stashed into the start command; falls back to
    // the generic plain-start macro when no mapping was ever stashed for this job.
    std::string build_start_print_gcode(const std::string& upload_filename) const override;

private:
    // Combine filament_type + filament_sub_type into a unified type string
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);

    // Raw 1-based filament->tool map, stashed by send_filament_mapping() and consumed (and
    // cleared) by the next build_start_print_gcode() call so a stale mapping can never leak into
    // an unrelated later job. Mutable because the consuming call is logically const (it only
    // reads job-start state) even though it clears this cache. Rendering into MAP_TABLE wire
    // format happens inside build_start_script() at consume time, not here -- this class only
    // stashes the GUI's raw pick.
    mutable std::vector<int> m_pending_filament_map;
};

} // namespace Slic3r

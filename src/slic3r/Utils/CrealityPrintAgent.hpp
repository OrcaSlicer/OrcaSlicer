#ifndef __CREALITY_PRINT_AGENT_HPP__
#define __CREALITY_PRINT_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <atomic>
#include <optional>
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

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    // The device page's Load and Unload, carried out by a K1-family board's box. Other boards and
    // commands go to the base agent.
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    // The device page's per-slot refresh: a K1-family box reads the slot's tag again before the
    // slots are read back.
    int command_ams_refresh_rfid(std::string dev_id, int ams_id, int slot_id, int sequence_id, bool lan_mode) override;

    // The script a K1-family box runs to load slot `slot_id` of box `ams_id` (both from 0), or to
    // unload whatever the toolhead holds when `load` is false. `nozzle_temp` is the temperature
    // the filament moves at; 0 or less takes the box's own default.
    static std::string change_filament_gcode(bool load, int ams_id, int slot_id, int nozzle_temp);
    // The script a K1-family box runs to read the tag of slot `slot_id` of box `ams_id` again.
    static std::string refresh_slot_gcode(int ams_id, int slot_id);

    // The slot index, four per box, whose filament a K1-family box reports in the toolhead, -1 when
    // it reports none, and nothing when the reply names no box.
    static std::optional<int> parse_loaded_slot(const std::string& response);

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

protected:
    void on_printer_connected(const std::string& dev_id) override;

private:
    // Tells the device page which slot feeds the toolhead.
    void report_loaded_slot(int loaded_slot);

    // Whether the last filament sync found a K1-family board's box, the only CFS this agent knows
    // how to command. Other boards may publish a `box` object too, but none has been tried.
    std::atomic<bool> can_command_box{false};
};

} // namespace Slic3r

#endif

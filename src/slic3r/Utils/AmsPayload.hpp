#ifndef __AMS_PAYLOAD_HPP__
#define __AMS_PAYLOAD_HPP__

#include "bambu_networking.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// One lane of a multi-material system in the neutral shape every printer agent
// shares before it is rendered into the BBL AMS payload.
struct AmsTrayData {
    int         slot_index = 0;      // 0-based global slot index
    bool        has_filament = false;
    std::string tray_type;           // Material type (e.g. "PLA", "ASA")
    std::string tray_color;          // Raw color (#RRGGBB, 0xRRGGBB, or RRGGBBAA)
    std::string tray_info_idx;       // OrcaFilamentLibrary preset id (optional)
    int         bed_temp = 0;        // Optional
    int         nozzle_temp = 0;     // Optional
};

// Normalize a driver color string to the BBL RRGGBBAA form; "00000000" when invalid.
std::string normalize_ams_color(const std::string& color);

// Map a reported material type to an OrcaFilamentLibrary generic family id.
std::string map_filament_type_to_generic_id(const std::string& filament_type);

// Parse a Moonraker `/server/database/item?namespace=lane_data` response body:
// result.value is keyed by lane, each entry carrying "lane", optional
// "material"/"color"/"bed_temp"/"nozzle_temp" and an optional presence signal
// ("has_filament" or "loaded"). Presence is preferred over material: a loaded
// lane with no declared material must not read as empty. Returns false when
// malformed or empty. Pure: tray_info_idx is left for resolve_tray_info_idx().
bool parse_moonraker_lane_data(const nlohmann::json& body,
                               std::vector<AmsTrayData>& trays,
                               int& max_lane_index);

// Outcome of one lane_data read. synced/none/unknown mirror OrcaSonar's
// REQ-STS-007 tri-state; error covers transport failure and unparsable bodies.
enum class LaneDataFetch { synced, none, unknown, error };

// Read and parse the Moonraker `lane_data` namespace from origin (OrcaSonar's
// façade or a real Moonraker — both emit the same shape). A non-empty api_key is
// sent as X-Api-Key. trays/max_lane_index are only populated on synced. The
// tri-state is preserved: 404 is unknown, an empty value object is none.
LaneDataFetch read_moonraker_lane_data(const std::string& origin,
                                       const std::string& api_key,
                                       std::vector<AmsTrayData>& trays,
                                       int& max_lane_index);

// AMS units a flat lane range renders into (4 lanes per unit, rounding up);
// 0 when there are no lanes. max_lane_index is the highest index, or -1.
inline int ams_count_for_lanes(int max_lane_index)
{
    return max_lane_index < 0 ? 0 : (max_lane_index + 4) / 4;
}

// Fill each tray's tray_info_idx from the loaded preset bundle (falling back to
// the generic family map). Reads GUI preset state, so it MUST run on the main
// thread. Ids already set by a vendor-aware resolver are left untouched.
void resolve_tray_info_idx(std::vector<AmsTrayData>& trays);

// Optional vendor-aware resolver, run on the main thread before the generic
// resolver. Agents with brand/color matching (Snapmaker, Creality) supply one
// so their results survive the shared path; the generic resolver only fills
// ids this leaves empty.
using TrayInfoResolver = std::function<void(std::vector<AmsTrayData>&)>;

// Assemble the BBL-format AMS JSON (ams[] units + ams_exist_bits/tray_exist_bits)
// that DevFilaSystemParser::ParseV1_0 consumes. Pure and GUI-free: the
// MachineObject mutation stays in build_ams_payload_for_device.
nlohmann::json build_bbl_ams_json(const std::vector<AmsTrayData>& trays,
                                  int ams_count,
                                  int max_lane_index);

// Render trays into the BBL AMS payload and populate the device's DevFilaSystem.
// The resolver and the MachineObject mutation both run on the main thread via
// queue_fn when set. printer_type: nullopt leaves obj->printer_type untouched
// (OrcaSonar, whose push_status already carries it); a value assigns it
// verbatim (Moonraker).
void build_ams_payload_for_device(const std::string& dev_id,
                                  const std::optional<std::string>& printer_type,
                                  int ams_count,
                                  int max_lane_index,
                                  const std::vector<AmsTrayData>& trays,
                                  const QueueOnMainFn& queue_fn,
                                  const TrayInfoResolver& vendor_resolver = {});

// Process-wide canonical AMS write capability (OrcaSonar REQ-STS-008), parsed
// from the info.get_capabilities reply. A device with no record (no reply yet;
// non-OrcaSonar agents never register) reports every op supported: gating only
// applies to OrcaSonar printers that answered. An answer without ams_ops
// registers an empty op set, so it gates every write.
void   register_ams_ops(const std::string& dev_id, const std::vector<std::string>& ops);
bool   ams_op_supported(const std::string& dev_id, const std::string& op);

// Whether the device has a material system, from the get_capabilities reply's
// protocol.features.fms (falling back to a non-empty ams_ops). No record reads
// false: an unconfirmed printer must not advertise filament sync.
void   register_ams_capability(const std::string& dev_id, bool has_ams);
bool   has_ams_capability(const std::string& dev_id);

// Whether the device exposes the filament-slot model, from the
// get_capabilities reply's protocol.features.filament_slots. The slot model is
// connector state, independent of fms: a printer with no material hardware
// still has slots, so this alone enables filament sync (REQ-FMS-001).
void   register_filament_slots(const std::string& dev_id, bool has_slots);
bool   has_filament_slots(const std::string& dev_id);

// Forget a device's declared capabilities, so a reconnect starts from "no
// reply yet" instead of a stale declaration.
void   clear_ams_caps(const std::string& dev_id);

} // namespace Slic3r

#endif

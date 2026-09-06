#ifndef slic3r_GUI_FilamentInventoryStore_hpp_
#define slic3r_GUI_FilamentInventoryStore_hpp_

#include <string>

#include "libslic3r/FilamentInventory.hpp"
#include "libslic3r/Preset.hpp"

namespace Slic3r {
class DevAmsTray;
namespace GUI {

// Persistence layer for the per-machine physical filament inventories, stored in a
// Slic3r::FilamentInventories (src/libslic3r/FilamentInventory.hpp) as a single JSON blob via
// AppConfig's root key "filament_inventories" (wxGetApp().app_config->get/set).
FilamentInventories load_filament_inventories();
void                save_filament_inventories(const FilamentInventories& store);

// One store/tool-count resolution preamble: loads the store into `store` and resolves the
// addressable tool count for the active printer profile (ActivePrinterSession::profile(), which
// owns the edited-vs-selected choice) in one call. Follow with
// current_inventory_for_preset(active_printer_session().profile(), store, tool_count) for the
// inventory itself.
size_t resolve_active_printer_tool_count(FilamentInventories& store);

// Resolves (creating on first use) and pads the inventory entry for THIS printer preset to at
// least tool_count tools; never truncates.
FilamentInventory& current_inventory_for_preset(const Preset& printer_preset, FilamentInventories& store, size_t tool_count);

// Orca: one blocking read of the connected printer's loaded filaments into `inv` (registry
// write-through + save), via the same agent fetch and resolve_device_tray conversion the
// materials editor's sync uses. For dialogs that need a populated inventory on open -- the
// device print dialog calls it when nothing was ever recorded, so a first-ever Print click
// offers the printer's real filaments instead of bare bootstrap tools. Returns false (inventory
// untouched) with no connection, a failed fetch, or nothing reported.
bool sync_filament_inventory_from_printer(FilamentInventories& store, FilamentInventory& inv, size_t tool_count);

// How many tools this printer preset addresses: the physical nozzle count.
size_t addressable_tool_count_of(const Preset& printer_preset);

// Resolution for display/use: the slot's preset if it still exists in `filaments`, else
// "Generic <type>" if that preset exists, else "".
// The installed "Generic <type>" preset for a material type, resolved alias-aware (canonical
// system names carry a suffix, e.g. "Generic PLA @System"). nullptr when absent or type empty.
const Preset* find_generic_filament_preset(const std::string& type, const PresetCollection& filaments);

// Orca: what one reported tray means in Orca terms. The ONE conversion from the device-side
// tray (DevAmsTray, the common denominator across pull-mode agents and push-mode agents like
// BBL's that never go through fetch_filament_info) to the physical-filament vocabulary the
// inventory, the material editor and the mapping dialog all speak. Vendor knowledge stays in
// the agents, which normalize INTO DevAmsTray; resolution semantics live here, once, so the
// dialogs cannot drift apart on what a reported spool means.
struct DeviceSlotResolution
{
    bool        present    = false; ///< a spool is physically in the slot
    bool        tag_locked = false; ///< data came from an NFC tag; the tag is authoritative
    std::string color;              ///< "#RRGGBB", empty when the tray reported none
    std::string preset;             ///< resolved preset name; empty when nothing resolves
    std::string type;               ///< the type string as the printer reported it
};

// tray may be null ("slot exists but nothing loaded" -- same meaning as !is_exists). Preset
// resolution: the agent's exact match first (setting_id carries the filament_id it matched),
// then the material's Generic preset, both alias-aware and NOT gated on the profile being
// enabled -- a profile the user never enabled still describes the spool. When nothing resolves
// at all, `preset` stays empty while `type` still reports what the printer said: the caller
// must show the bare type rather than silently keeping whatever material it showed before.
// Non-const tray: DevAmsTray::get_filament_type() canonicalizes a legacy "Support" type in
// place, so reading a tray is not const on the device side.
DeviceSlotResolution resolve_device_tray(DevAmsTray* tray, const PresetCollection& filaments);

std::string resolve_slot_preset(const PhysicalFilament& pf, const PresetCollection& filaments);

// A physical slot's short display name: resolve_slot_preset's raw preset name, shortened for
// a compact UI -- the preset's short alias when it has one (e.g. "Snapmaker PLA SnapSpeed" instead
// of "Snapmaker PLA SnapSpeed @U1"), else the same trim-at-first-" @" idiom CaliHistoryDialog uses
// for a preset with no alias (system/vendor presets carry an " @<vendor>" suffix on the raw name).
// "" when the slot's preset doesn't resolve at all -- callers that want a type-only fallback (e.g.
// pf.type) apply it themselves. Used by FilamentMapDialog's combo labels and the Slicing Result
// summary row, so the same physical slot renders identically in both.
std::string slot_display_name(const PhysicalFilament& pf, const PresetCollection& filaments);

// The single vendor derivation for a live filament preset: prefers the actual
// filament_vendor config option (the authoritative field), falling back to the first whitespace
// token of the preset's alias-or-name -- the same convention libslic3r's preset_vendor_token
// (FilamentInventory.cpp) uses when only a raw preset-name string is available, e.g. inside the
// pure auto-mapper, which cannot depend on a live Preset/config. Returns "" when neither yields
// anything. Used everywhere this dialog stack needs a preset's vendor for display or grouping
// (FilamentInventoryEditor's card, the Physical Filament combo box's vendor grouping) so they can
// never disagree on the same preset.
std::string filament_vendor_of(const Preset& preset);

// One PhysicalFilament assembly: color/type/preset/id/kind assigned the same way at every
// GUI write site (FilamentInventoryEditor::slot_from_row and FilamentMapDialog's bootstrap
// "remember these as the loaded filaments" write).
PhysicalFilament build_physical_filament(const std::string& color, const std::string& type,
                                          const std::string& preset, int id, PhysicalFilament::Kind kind);

// GUI-side adapter for Slic3r::build_project_filament_info (FilamentInventory.hpp): extracts the
// filament_colour/filament_type/filament_vendor config-option lists from `full_config` and
// delegates, pairing them with the caller's filament_presets list (0-based, same order/length).
std::vector<ProjectFilamentInfo> build_project_filament_info(const DynamicPrintConfig& full_config,
                                                               const std::vector<std::string>& filament_presets);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentInventoryStore_hpp_

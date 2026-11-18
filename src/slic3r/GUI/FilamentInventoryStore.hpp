#ifndef slic3r_GUI_FilamentInventoryStore_hpp_
#define slic3r_GUI_FilamentInventoryStore_hpp_

#include <cstddef>
#include <string>
#include <vector>

#include "libslic3r/FilamentInventory.hpp"
#include "libslic3r/Preset.hpp"

namespace Slic3r {
namespace GUI {

// Stored as one JSON blob under AppConfig's "filament_inventories" key.
FilamentInventories load_filament_inventories();
void                save_filament_inventories(const FilamentInventories& store);

// Loads `store` and returns the active printer profile's entry, created on first use and padded
// to its addressable tool count.
FilamentInventory& load_active_printer_inventory(FilamentInventories& store);

size_t addressable_tool_count_of(const Preset& printer_preset);

// One reported tray in physical-filament terms. Agents normalize into DevAmsTray; resolution
// lives here only.
struct DeviceSlotResolution
{
    bool             present    = false;
    bool             tag_locked = false; ///< data came from an NFC tag; the tag is authoritative
    PhysicalFilament filament;           ///< id 0; the slot's location even when not present
};

// Blocking read of the live printer's loaded filaments into `inv`, then saves `store`.
// `inv` is untouched unless ok(). `fetch` false reads the machine's last report without a new request.
struct DeviceSyncOutcome
{
    enum class Status { NoSession, FetchFailed, NothingReported, Unchanged, Applied } status = Status::NoSession;
    std::vector<DeviceSlotResolution> slots; // one per reported slot, inventory order
    bool ok() const { return status == Status::Unchanged || status == Status::Applied; }
};
DeviceSyncOutcome sync_filament_inventory_from_printer(FilamentInventories& store, FilamentInventory& inv, bool fetch = true);

// The slot's preset if installed, else "Generic <type>", else "". "" too when no preset was
// ever recorded.
std::string resolve_slot_preset(const PhysicalFilament& pf, const PresetCollection& filaments);

// resolve_slot_preset's alias, else its name cut at the first " @"; "" when unresolved.
std::string slot_display_name(const PhysicalFilament& pf, const PresetCollection& filaments);

// filament_vendor, else the first token of the alias-or-name (as preset_vendor_token does).
std::string filament_vendor_of(const Preset& preset);

PhysicalFilament build_physical_filament(const std::string& color, const std::string& type, const std::string& preset, int id);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentInventoryStore_hpp_

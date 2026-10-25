#include "FilamentInventoryStore.hpp"

#include "ActivePrinterSession.hpp"
#include "DeviceCore/DevDefs.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "IPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <algorithm>
#include <boost/log/trivial.hpp>
#include <libslic3r/FilamentInventory.hpp>
#include <libslic3r/Preset.hpp>
#include <cstddef>
#include <libslic3r/Config.hpp>
#include <string>
#include <memory>
#include <wx/colour.h>
#include <vector>

namespace Slic3r { namespace GUI {

static const char *FILAMENT_INVENTORIES_KEY = "filament_inventories";
// Keeps the first unparsable blob, which the next save would otherwise overwrite.
static const char *FILAMENT_INVENTORIES_CORRUPT_BACKUP_KEY = "filament_inventories_corrupt_backup";

size_t addressable_tool_count_of(const Preset &printer_preset)
{
    const auto  *nozzle_diameters = printer_preset.config.option<ConfigOptionFloats>("nozzle_diameter");
    return nozzle_diameters && !nozzle_diameters->empty() ? nozzle_diameters->size() : 1;
}

// Alias-aware (system names carry a suffix, e.g. "Generic PLA @System"); also accepts a
// compatible profile the user never enabled. nullptr when absent or type empty.
static const Preset* find_generic_filament_preset(const std::string& type, const PresetCollection& filaments)
{
    if (type.empty())
        return nullptr;
    const std::string wanted = "Generic " + type;
    if (const Preset* p = filaments.find_preset(filaments.get_preset_name_by_alias(wanted), false))
        return p;

    // get_preset_name_by_alias only sees visible profiles; a compatible but not enabled one
    // still describes the spool.
    const Preset* fallback = nullptr;
    for (auto it = filaments.begin(); it != filaments.end(); ++it) {
        if ((it->alias != wanted && it->name != wanted) || !it->is_compatible)
            continue;
        if (it->is_system)
            return &*it;
        if (fallback == nullptr)
            fallback = &*it;
    }
    return fallback;
}

std::string resolve_slot_preset(const PhysicalFilament &pf, const PresetCollection &filaments)
{
    // The Generic fallback only replaces a recorded preset that no longer resolves.
    if (pf.preset.empty())
        return std::string();
    if (filaments.find_preset(pf.preset, false) != nullptr)
        return pf.preset;
    if (const Preset* generic = find_generic_filament_preset(pf.type, filaments))
        return generic->name;
    return std::string();
}

FilamentInventory& load_active_printer_inventory(FilamentInventories &store)
{
    const Preset &printer = active_printer_session().profile();
    store = load_filament_inventories();
    return store.for_preset(printer.name, addressable_tool_count_of(printer));
}

std::string filament_vendor_of(const Preset &preset)
{
    if (const auto *vendor = preset.config.option<ConfigOptionStrings>("filament_vendor"))
        if (!vendor->values.empty() && !vendor->values.front().empty())
            return vendor->values.front();
    const std::string name = preset.alias.empty() ? preset.name : preset.alias;
    const size_t       space = name.find(' ');
    return space == std::string::npos ? std::string() : name.substr(0, space);
}

PhysicalFilament build_physical_filament(const std::string &color, const std::string &type, const std::string &preset, int id)
{
    PhysicalFilament slot;
    slot.id     = id;
    slot.color  = color;
    slot.type   = type;
    slot.preset = preset;
    return slot;
}

std::string slot_display_name(const PhysicalFilament &pf, const PresetCollection &filaments)
{
    std::string resolved = resolve_slot_preset(pf, filaments);
    if (resolved.empty())
        return resolved;
    if (const Preset *p = filaments.find_preset(resolved, false); p != nullptr && !p->alias.empty())
        return p->alias;
    return resolved.substr(0, resolved.find(" @"));
}

// tray may be null (nothing loaded). When no preset resolves, callers must show the bare
// `type` rather than keep the previous material. Non-const: get_filament_type() canonicalizes
// a legacy "Support" type in place.
static DeviceSlotResolution resolve_device_tray(DevAmsTray* tray, const PresetCollection& filaments)
{
    DeviceSlotResolution res;
    PhysicalFilament&    pf = res.filament;
    if (tray != nullptr) {
        pf.name         = tray->slot_name;
        pf.unit         = tray->unit;
        pf.unit_label   = tray->unit_label;
        pf.head         = tray->head;
        pf.slot         = tray->slot;
        pf.extruder     = tray->extruder;
        pf.virtual_tool = tray->virtual_tool;
    }
    if (tray == nullptr || !tray->is_exists)
        return res;
    res.present = true;

    if (!tray->color.empty())
        pf.color = DevAmsTray::decode_color(tray->color).GetAsString(wxC2S_HTML_SYNTAX).ToStdString();

    pf.type = tray->get_filament_type();

    // setting_id is the filament_id the agent matched. Not gated on is_visible: a profile the
    // user hasn't enabled still describes the spool.
    const Preset* target = nullptr;
    if (!tray->setting_id.empty())
        for (auto it = filaments.begin(); it != filaments.end(); ++it)
            if (it->filament_id == tray->setting_id && it->is_compatible) { target = &*it; break; }
    if (target == nullptr)
        target = find_generic_filament_preset(pf.type, filaments);
    if (target != nullptr)
        pf.preset = target->name;

    // A non-zero tag_uid means the data came from an NFC tag.
    res.tag_locked = !tray->tag_uid.empty() && tray->tag_uid.find_first_not_of('0') != std::string::npos;
    return res;
}

DeviceSyncOutcome sync_filament_inventory_from_printer(FilamentInventories& store, FilamentInventory& inv, bool fetch)
{
    using Status = DeviceSyncOutcome::Status;
    DeviceSyncOutcome outcome;

    const ActivePrinterSession& session   = active_printer_session();
    NetworkAgent*               net_agent = session.sync_agent();
    MachineObject*              machine   = session.live_machine();
    if (net_agent == nullptr || machine == nullptr)
        return outcome; // NoSession
    if (fetch && net_agent->get_filament_sync_mode() == FilamentSyncMode::pull &&
        !net_agent->fetch_filament_info(machine->get_dev_id())) {
        outcome.status = Status::FetchFailed;
        return outcome;
    }

    std::shared_ptr<DevFilaSystem> fila_system = machine->GetFilaSystem();
    if (!fila_system) {
        outcome.status = Status::FetchFailed;
        return outcome;
    }

    const PresetCollection& filaments = wxGetApp().preset_bundle->filaments;
    // GetTrayIndexMap() always includes two virtual/external-spool pseudo-trays.
    const auto&  tray_map = fila_system->GetTrayIndexMap();
    const size_t reported = std::count_if(tray_map.begin(), tray_map.end(),
                                          [](const auto& kv) { return !devPrinterUtil::IsVirtualSlot(kv.second.first); });
    if (reported == 0) {
        outcome.status = Status::NothingReported;
        return outcome;
    }
    inv.ensure_slot_count(reported);
    size_t slot_idx = 0;
    bool   applied  = false;
    for (const auto& [tray_index, slot_id] : tray_map) {
        if (devPrinterUtil::IsVirtualSlot(slot_id.first))
            continue;
        DevAmsTray* tray = fila_system->GetAmsTray(std::to_string(slot_id.first), std::to_string(slot_id.second));
        const DeviceSlotResolution res = resolve_device_tray(tray, filaments);
        outcome.slots.push_back(res);
        PhysicalFilament& cur  = inv.slots[slot_idx];
        PhysicalFilament  next = res.filament;
        // Same type and color: keep the recorded preset rather than downgrade a hand-picked one
        // to Generic on printers that only report type and color.
        if (res.present && !cur.empty() && !cur.preset.empty() && cur.type == next.type && cur.color == next.color)
            next.preset = cur.preset;
        // Keep the id so plate maps survive a colour change; an empty slot is id 0.
        if (res.present)
            next.id = cur.id;
        cur = next;
        applied |= res.present;
        ++slot_idx;
    }

    inv.dialect = fila_system->GetChangerDialect();
    inv.ensure_ids();
    save_filament_inventories(store);
    outcome.status = applied ? Status::Applied : Status::Unchanged;
    return outcome;
}

FilamentInventories load_filament_inventories()
{
    std::string serialized = wxGetApp().app_config->get(FILAMENT_INVENTORIES_KEY);
    if (serialized.empty())
        return FilamentInventories{};
    FilamentInventories store = FilamentInventories::deserialize(serialized);
    if (store.parse_error && wxGetApp().app_config->get(FILAMENT_INVENTORIES_CORRUPT_BACKUP_KEY).empty())
        wxGetApp().app_config->set(FILAMENT_INVENTORIES_CORRUPT_BACKUP_KEY, serialized);
    return store;
}

void save_filament_inventories(const FilamentInventories &store)
{
    wxGetApp().app_config->set(FILAMENT_INVENTORIES_KEY, store.serialize());
}

}} // namespace Slic3r::GUI

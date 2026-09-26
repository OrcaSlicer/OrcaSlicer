#include "FilamentInventoryStore.hpp"

#include "ActivePrinterSession.hpp"
#include "DeviceCore/DevDefs.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "Tab.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

static const char *FILAMENT_INVENTORIES_KEY = "filament_inventories";
// Written once, the first time deserialize() reports a parse failure, so the unparsable blob
// survives the next save_filament_inventories() overwrite of FILAMENT_INVENTORIES_KEY -- never
// overwritten again afterward, so it always keeps the FIRST corrupt blob seen, not the latest.
static const char *FILAMENT_INVENTORIES_CORRUPT_BACKUP_KEY = "filament_inventories_corrupt_backup";

FilamentInventory& current_inventory_for_preset(const Preset &printer_preset, FilamentInventories &store, size_t tool_count)
{
    return store.for_preset(printer_preset.name, tool_count);
}

size_t addressable_tool_count_of(const Preset &printer_preset)
{
    const auto  *nozzle_diameters = printer_preset.config.option<ConfigOptionFloats>("nozzle_diameter");
    return nozzle_diameters && !nozzle_diameters->empty() ? nozzle_diameters->size() : 1;
}

std::string resolve_slot_preset(const PhysicalFilament &pf, const PresetCollection &filaments)
{
    // Orca: the Generic-<type> fallback only stands in for a RECORDED preset that no longer
    // resolves (e.g. the preset was deleted/renamed since the slot was saved) -- it must not
    // fire for a slot that never had a preset recorded at all (pf.preset empty), such as a
    // legacy slot migrated without preset data. Those slots resolve to "" (bare-type label,
    // no profile-mismatch warning) instead of being degraded to a synthetic Generic identity.
    if (pf.preset.empty())
        return std::string();
    if (filaments.find_preset(pf.preset, false) != nullptr)
        return pf.preset;
    if (const Preset* generic = find_generic_filament_preset(pf.type, filaments))
        return generic->name;
    return std::string();
}

size_t resolve_active_printer_tool_count(FilamentInventories &store)
{
    store = load_filament_inventories();
    return addressable_tool_count_of(active_printer_session().profile());
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

PhysicalFilament build_physical_filament(const std::string &color, const std::string &type,
                                          const std::string &preset, int id, PhysicalFilament::Kind kind)
{
    PhysicalFilament slot;
    slot.kind   = kind;
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

const Preset* find_generic_filament_preset(const std::string& type, const PresetCollection& filaments)
{
    if (type.empty())
        return nullptr;
    // "Generic <type>" is usually an ALIAS -- the canonical system preset name carries a
    // suffix (e.g. "Generic PLA @System"), so resolve through the alias map first and fall
    // back to a literal name match for bundles that don't alias.
    const std::string wanted = "Generic " + type;
    if (const Preset* p = filaments.find_preset(filaments.get_preset_name_by_alias(wanted), false))
        return p;

    // Orca: get_preset_name_by_alias only considers INSTALLED profiles (is_visible), so a
    // material the printer reports whose Generic profile the user never enabled resolves to
    // nothing -- and the sync caller then keeps whatever the row said before, displaying a
    // material the printer never reported. A ZR Ultra S reporting PLA Silk showed as PLA for
    // exactly this reason, because only Generic PLA/ABS/PETG were enabled.
    //
    // An existing-but-not-enabled system profile still describes the spool correctly, so accept
    // it. Compatibility is still required: a profile for a different printer would not.
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

DeviceSyncOutcome sync_filament_inventory_from_printer(FilamentInventories& store, FilamentInventory& inv, size_t tool_count)
{
    using Status = DeviceSyncOutcome::Status;
    DeviceSyncOutcome outcome;

    const ActivePrinterSession& session   = active_printer_session();
    NetworkAgent*               net_agent = session.sync_agent();
    MachineObject*              machine   = session.live_machine();
    if (net_agent == nullptr || machine == nullptr)
        return outcome; // NoSession
    if (net_agent->get_filament_sync_mode() == FilamentSyncMode::pull &&
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
    // GetTrayIndexMap() always seeds two virtual/external-spool pseudo-trays alongside the real
    // ones; they never resolve, so the map is never an "any data?" signal on its own.
    const auto&  tray_map = fila_system->GetTrayIndexMap();
    const size_t reported = std::count_if(tray_map.begin(), tray_map.end(),
                                          [](const auto& kv) { return !devPrinterUtil::IsVirtualSlot(kv.second.first); });
    if (reported == 0) {
        outcome.status = Status::NothingReported;
        return outcome;
    }
    // The printer decides how many slots there are: a changer's lanes, the U1's extruders. The
    // nozzle-count fallback (tool_count) only sized the inventory before any sync.
    (void) tool_count;
    inv.ensure_slot_count(reported);
    size_t slot_idx = 0;
    bool   applied  = false;
    for (const auto& [tray_index, slot_id] : tray_map) {
        if (devPrinterUtil::IsVirtualSlot(slot_id.first))
            continue;
        DevAmsTray* tray = fila_system->GetAmsTray(std::to_string(slot_id.first), std::to_string(slot_id.second));
        const DeviceSlotResolution res = resolve_device_tray(tray, filaments);
        outcome.slots.push_back(res);
        PhysicalFilament& cur = inv.slots[slot_idx];
        // Orca: with the print dialog now refreshing on every open (not just bootstrap), an
        // unconditional overwrite would silently downgrade a hand-picked preset to the Generic
        // fallback each time on printers that only report type+color. When the printer reports
        // the SAME spool (type and color unchanged) and the recorded slot carries richer detail
        // (a preset), keep the recorded slot; any reported change still wins wholesale.
        const bool same_spool = res.present && !cur.empty() && !cur.preset.empty() && cur.type == res.type && cur.color == res.color;
        if (!same_spool) {
            // Keep the slot's id when it still holds a filament, so plate maps pointing at it
            // survive a colour change; an emptied slot is canonically id 0 (ensure_ids).
            cur = res.present ? build_physical_filament(res.color, res.type, res.preset, cur.id, PhysicalFilament::Kind::Manual)
                              : PhysicalFilament{};
        }
        // Where the slot sits and what it feeds are the printer's to say, every time.
        cur.name         = res.name;
        cur.unit         = res.unit;
        cur.unit_label   = res.unit_label;
        cur.head         = res.head;
        cur.slot         = res.slot;
        cur.extruder     = res.extruder;
        cur.virtual_tool = res.virtual_tool;
        applied |= res.present;
        ++slot_idx;
    }

    // Cached with the inventory so a send can check the changer still speaks the profile's
    // protocol without another round trip (and refreshed on every sync, which a send does first).
    inv.dialect = fila_system->GetChangerDialect();
    // Neither a Klipper changer nor the printer's tool count is something the user should have
    // to declare: seed both from what the printer reported, as a modification of the edited
    // printer preset the user saves (or not) the usual way. Slicing then runs against the last
    // known printer even offline; the send path re-reads the printer and re-validates against it.
    if (seed_printer_from_report(wxGetApp().preset_bundle->printers.get_edited_preset().config, inv.dialect,
                                 fila_system->GetDeviceToolCount())) {
        if (Tab* printer_tab = wxGetApp().get_tab(Preset::TYPE_PRINTER)) {
            printer_tab->update_dirty();
            printer_tab->reload_config();
        }
    }
    inv.ensure_ids();
    save_filament_inventories(store);
    outcome.status = applied ? Status::Applied : Status::Unchanged;
    return outcome;
}

DeviceSlotResolution resolve_device_tray(DevAmsTray* tray, const PresetCollection& filaments)
{
    DeviceSlotResolution res;
    if (tray != nullptr) {
        // An empty slot still has a position: the grid draws it where the printer has it.
        res.name         = tray->slot_name;
        res.unit         = tray->unit;
        res.unit_label   = tray->unit_label;
        res.head         = tray->head;
        res.slot         = tray->slot;
        res.extruder     = tray->extruder;
        res.virtual_tool = tray->virtual_tool;
    }
    // Null and "exists but nothing loaded" mean the same thing to every consumer.
    if (tray == nullptr || !tray->is_exists)
        return res;
    res.present = true;

    if (!tray->color.empty())
        res.color = DevAmsTray::decode_color(tray->color).GetAsString(wxC2S_HTML_SYNTAX).ToStdString();

    res.type = tray->get_filament_type();

    // Best target first: the agent may have resolved an exact profile for this spool
    // (DevAmsTray::setting_id carries the filament_id it matched -- see e.g.
    // SnapmakerPrinterAgent::fetch_filament_info's vendor/type/color lookup). Fall back to the
    // material's Generic preset. Neither is gated on is_visible: a profile the user has not
    // enabled still describes the spool the printer reported. Compatibility still applies -- a
    // profile for another printer would not describe it.
    const Preset* target = nullptr;
    if (!tray->setting_id.empty())
        for (auto it = filaments.begin(); it != filaments.end(); ++it)
            if (it->filament_id == tray->setting_id && it->is_compatible) { target = &*it; break; }
    if (target == nullptr)
        target = find_generic_filament_preset(res.type, filaments);
    if (target != nullptr)
        res.preset = target->name;

    // Non-zero tag_uid = the slot's data came from an NFC tag; the tag is authoritative, so
    // consumers must not offer to overwrite what it reported.
    res.tag_locked = !tray->tag_uid.empty() && tray->tag_uid.find_first_not_of('0') != std::string::npos;
    return res;
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

std::vector<ProjectFilamentInfo> build_project_filament_info(const DynamicPrintConfig &full_config,
                                                               const std::vector<std::string> &filament_presets)
{
    const auto *colors  = full_config.option<ConfigOptionStrings>("filament_colour");
    const auto *types   = full_config.option<ConfigOptionStrings>("filament_type");
    const auto *vendors = full_config.option<ConfigOptionStrings>("filament_vendor");

    static const std::vector<std::string> empty;
    return Slic3r::build_project_filament_info(colors ? colors->values : empty, types ? types->values : empty,
                                                 vendors ? vendors->values : empty, filament_presets);
}

}} // namespace Slic3r::GUI

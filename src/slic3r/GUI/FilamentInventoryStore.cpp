#include "FilamentInventoryStore.hpp"

#include "ActivePrinterSession.hpp"
#include "GUI_App.hpp"

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

size_t addressable_tool_count_of(const Preset &printer_preset, FilamentInventories &/*store*/)
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
    return addressable_tool_count_of(active_printer_session().profile(), store);
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
    return filaments.find_preset(filaments.get_preset_name_by_alias(wanted), false);
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

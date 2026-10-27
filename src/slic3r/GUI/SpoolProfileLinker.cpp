#include "SpoolProfileLinker.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <system_error>
#include <vector>

#include "GUI_App.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr int SLOTS_PER_AMS = 4;

bool is_linking_on()
{
    return wxGetApp().app_config->get_bool("link_spools_to_profiles");
}

// A color-only sync keeps the project's profiles, so there is nothing to link.
bool is_color_only_sync()
{
    return wxGetApp().app_config->get("sync_ams_filament_mode") == "1";
}

// A printer that only says "PLA" says it for every PLA, so one link would put all of them on one
// profile.
bool is_linkable(const SpoolIdentity& spool)
{
    return !spool_is_generic(spool);
}

std::string linked_preset_name(const SpoolIdentity& spool)
{
    if (!spool.has_filament || !is_linkable(spool))
        return std::string();
    return SpoolProfileMap::get().remembered(spool_mapping_key(spool), spool_variant_color(spool));
}

SpoolChoice make_choice(const PresetCollection& filaments, int slot_index, const std::string& preset_name)
{
    const Preset* preset = filaments.find_preset(preset_name);
    return SpoolChoice{slot_index, preset_name, preset != nullptr ? preset->filament_id : std::string()};
}

std::vector<SpoolChoice> remembered_choices(const PresetCollection& filaments, const std::vector<SpoolIdentity>& spools)
{
    std::vector<SpoolChoice> choices;
    for (const SpoolIdentity& spool : spools) {
        const std::string name = linked_preset_name(spool);
        if (!name.empty())
            choices.push_back(make_choice(filaments, spool.slot_index, name));
    }
    return choices;
}

// The same profile the stock sync picks for a filament id, so the link and the sync agree.
const Preset* base_preset_for(const PresetCollection& filaments, const std::string& filament_id)
{
    for (const Preset& preset : filaments.get_presets())
        if (preset.is_compatible && filaments.get_preset_base(preset) == &preset && preset.filament_id == filament_id)
            return &preset;
    return nullptr;
}

const SpoolIdentity* published_spool(const std::vector<SpoolIdentity>& spools, int slot_index)
{
    const auto found = std::find_if(spools.begin(), spools.end(),
                                    [slot_index](const SpoolIdentity& spool) { return spool.slot_index == slot_index; });
    return found != spools.end() && found->has_filament ? &*found : nullptr;
}

int unplaced_pick_for(const std::vector<SpoolChoice>& choices, const std::vector<bool>& placed, const std::string& filament_id)
{
    for (size_t pick = 0; pick < choices.size(); ++pick)
        if (!placed[pick] && !choices[pick].filament_id.empty() && choices[pick].filament_id == filament_id)
            return static_cast<int>(pick);
    return -1;
}

bool is_new(const SpoolIdentity& spool)
{
    return spool.has_filament && is_linkable(spool) && SpoolProfileMap::get().is_new_spool(spool_mapping_key(spool), spool_variant_color(spool));
}

// A lane number out of one of the AMS entry's own string options. -1 for anything that is
// missing, of another type or not a number: an entry we cannot place is an entry we skip.
int config_index(const DynamicPrintConfig& config, const char* key)
{
    const ConfigOptionStrings* option = config.option<ConfigOptionStrings>(key);
    if (option == nullptr || option->values.empty())
        return -1;
    const std::string& text   = option->values.front();
    int                index  = -1;
    const auto         result = std::from_chars(text.data(), text.data() + text.size(), index);
    return result.ec == std::errc() ? index : -1;
}

DynamicPrintConfig* ams_entry_for(PresetBundle& bundle, int slot_index)
{
    for (auto& item : bundle.filament_ams_list) {
        const int ams_id  = config_index(item.second, "ams_id");
        const int slot_id = config_index(item.second, "slot_id");
        if (ams_id >= 0 && slot_id >= 0 && ams_id * SLOTS_PER_AMS + slot_id == slot_index)
            return &item.second;
    }
    return nullptr;
}

std::string preset_string(const Preset& preset, const char* key)
{
    return preset.config.has(key) ? preset.config.opt_string(key, 0u) : std::string();
}

} // namespace

std::string spool_linked_filament_id(const SpoolIdentity& spool)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || !is_linking_on())
        return std::string();
    const std::string name = linked_preset_name(spool);
    if (name.empty())
        return std::string();
    const Preset* preset = bundle->filaments.find_preset(name);
    return preset != nullptr ? preset->filament_id : std::string();
}

void spool_link_from_slot(const std::string& dev_id, int slot_index, const std::string& filament_id)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || filament_id.empty() || !is_linking_on())
        return;

    const std::vector<SpoolIdentity> spools = SpoolProfileMap::get().spools(dev_id);
    const SpoolIdentity*             spool  = published_spool(spools, slot_index);
    if (spool == nullptr || !is_linkable(*spool))
        return;

    // The slot window only offers system profiles. A link to the user's own profile of the same
    // filament says the same thing, so it is kept rather than moved back to the system parent.
    const Preset* current = bundle->filaments.find_preset(linked_preset_name(*spool));
    if (current != nullptr && current->filament_id == filament_id)
        return;

    const Preset* picked = base_preset_for(bundle->filaments, filament_id);
    if (picked == nullptr)
        return;
    SpoolProfileMap::get().remember_spool(spool_mapping_key(*spool), picked->name, spool_display_name(*spool), spool_variant_color(*spool));
}

bool spool_sync_needs_asking(const std::string& dev_id)
{
    if (wxGetApp().preset_bundle == nullptr || !is_linking_on() || is_color_only_sync())
        return false;
    const std::vector<SpoolIdentity> spools = SpoolProfileMap::get().spools(dev_id);
    return std::any_of(spools.begin(), spools.end(), is_new);
}

void point_sync_at(const std::vector<SpoolChoice>& chosen)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;
    for (const SpoolChoice& choice : chosen) {
        const Preset*       preset = bundle->filaments.find_preset(choice.preset_name);
        DynamicPrintConfig* entry  = ams_entry_for(*bundle, choice.slot_index);
        if (preset == nullptr || choice.filament_id.empty() || entry == nullptr)
            continue;
        entry->set_key_value("filament_id", new ConfigOptionStrings{choice.filament_id});
        entry->set_key_value("filament_type", new ConfigOptionStrings{preset_string(*preset, "filament_type")});
    }
}

std::vector<SpoolChoice> spool_linked_choices(const std::string& dev_id)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || !is_linking_on() || is_color_only_sync())
        return {};
    return remembered_choices(bundle->filaments, SpoolProfileMap::get().spools(dev_id));
}

void apply_links(const std::vector<SpoolChoice>& chosen)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (chosen.empty() || bundle == nullptr)
        return;

    std::vector<bool> placed(chosen.size(), false);
    for (const size_t index : bundle->physical_filament_config_indices()) {
        const Preset* current = bundle->filaments.find_preset(bundle->filament_presets[index]);
        if (current == nullptr)
            continue;
        const int pick = unplaced_pick_for(chosen, placed, current->filament_id);
        if (pick < 0)
            continue;
        placed[pick]                    = true;
        bundle->filament_presets[index] = chosen[pick].preset_name;
    }
    bundle->update_multi_material_filament_presets();
}

}} // namespace Slic3r::GUI

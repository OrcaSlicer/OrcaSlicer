#include "FilamentCompaction.hpp"

#include "FilamentMixer.hpp"
#include "Model.hpp"
#include "Preset.hpp"
#include "PrintConfig.hpp"
#include "TriangleSelector.hpp"

#include <algorithm>
#include <memory>
#include <set>

namespace Slic3r {

// 1-based filament references that live in a region/object config. Each is "0 == inherit", so
// only non-zero values name a filament. Shared by the collector and both transforms so the two
// can never disagree about what counts as a filament reference.
static const char* s_region_filament_keys[] = {
    "outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
    "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id",
};
// The same, for the two support references -- separate because they only name a filament when
// support is actually enabled for the object.
static const char* s_support_filament_keys[] = { "support_filament", "support_interface_filament" };

bool FilamentCompaction::is_identity() const
{
    for (size_t tool = 0; tool < slot_of_tool.size(); ++tool)
        if (slot_of_tool[tool] != int(tool))
            return false;
    return true;
}

int FilamentCompaction::tool_of_slot(int slot_0based) const
{
    auto it = std::find(slot_of_tool.begin(), slot_of_tool.end(), slot_0based);
    return it == slot_of_tool.end() ? -1 : int(it - slot_of_tool.begin());
}

// The value an object takes for `key`: its own override if it has one, otherwise the project's.
static int object_or_global_int(const ModelObject& object, const DynamicPrintConfig& config, const char* key)
{
    if (const ConfigOption* opt = object.config.option(key))
        return opt->getInt();
    const ConfigOption* opt = config.option(key);
    return opt == nullptr ? 0 : opt->getInt();
}

static bool object_or_global_bool(const ModelObject& object, const DynamicPrintConfig& config, const char* key)
{
    if (const ConfigOption* opt = object.config.option(key))
        return opt->getBool();
    const ConfigOption* opt = config.option(key);
    return opt != nullptr && opt->getBool();
}

// Every 0-based slot the plate references, mixed slots included, sorted and deduplicated.
static std::vector<int> referenced_filament_slots(const Model& model, const DynamicPrintConfig& config)
{
    std::set<int> slots;
    // Every id handled here is 1-based (0 means "inherit"), so store slot-1.
    auto add_1based = [&slots](int filament_1based) {
        if (filament_1based > 0)
            slots.insert(filament_1based - 1);
    };

    for (const ModelObject* object : model.objects) {
        // Print::apply builds print objects only from printable instances, so an object parked on
        // another plate contributes no filaments -- matching what the user sees on this plate.
        const bool printable = std::any_of(object->instances.begin(), object->instances.end(),
                                           [](const ModelInstance* i) { return i->is_printable(); });
        if (!printable)
            continue;

        for (const ModelVolume* volume : object->volumes)
            // 1-based, and already the union of the volume's own filament and its
            // multi-material painting.
            for (int filament : volume->get_extruders())
                add_1based(filament);

        for (const auto& layer_range : object->layer_config_ranges)
            if (layer_range.second.has("extruder"))
                add_1based(layer_range.second.option("extruder")->getInt());

        for (const char* key : s_region_filament_keys)
            add_1based(object_or_global_int(*object, config, key));

        // Support filaments only name a filament when the object actually prints support.
        if (object_or_global_bool(*object, config, "enable_support") ||
            object_or_global_int(*object, config, "raft_layers") > 0)
            for (const char* key : s_support_filament_keys)
                add_1based(object_or_global_int(*object, config, key));
    }

    if (slots.empty())
        return {};

    auto plate_gcodes = model.plates_custom_gcodes.find(model.curr_plate_index);
    if (plate_gcodes != model.plates_custom_gcodes.end())
        for (const CustomGCode::Item& item : plate_gcodes->second.gcodes)
            if (item.type == CustomGCode::Type::ToolChange)
                add_1based(item.extruder);

    // A wipe tower filament only prints when there is more than one filament to purge between.
    if (slots.size() > 1) {
        const ConfigOption* enable_prime_tower = config.option("enable_prime_tower");
        if (enable_prime_tower != nullptr && enable_prime_tower->getBool()) {
            const ConfigOption* opt = config.option("wipe_tower_filament");
            add_1based(opt == nullptr ? 0 : opt->getInt());
        }
    }

    return std::vector<int>(slots.begin(), slots.end());
}

static bool is_mixed_slot(const DynamicPrintConfig& config, int slot_0based)
{
    const auto* is_mixed = config.option<ConfigOptionBools>("filament_is_mixed");
    return is_mixed != nullptr && size_t(slot_0based) < is_mixed->size() && is_mixed->values[slot_0based];
}

// `slots` with every mixed slot replaced by its component filaments.
static std::vector<int> physical_slots_of(const std::vector<int>& slots, const DynamicPrintConfig& config)
{
    const auto* is_mixed  = config.option<ConfigOptionBools>("filament_is_mixed");
    const auto* comp_strs = config.option<ConfigOptionStrings>("filament_mixed_components");
    if (is_mixed == nullptr || comp_strs == nullptr || !has_any_mixed_filament(is_mixed->values))
        return slots;
    const std::vector<unsigned int> expanded = expand_mixed_filaments(std::vector<unsigned int>(slots.begin(), slots.end()),
                                                                      is_mixed->values, comp_strs->values);
    return std::vector<int>(expanded.begin(), expanded.end());
}

std::vector<int> used_filament_slots(const Model& model, const DynamicPrintConfig& config)
{
    return physical_slots_of(referenced_filament_slots(model, config), config);
}

FilamentCompaction build_filament_compaction(const Model& model, const DynamicPrintConfig& config)
{
    FilamentCompaction compaction;
    const std::vector<int> referenced = referenced_filament_slots(model, config);
    compaction.slot_of_tool           = physical_slots_of(referenced, config);
    // The mixes come after every physical tool so T0..T(n-1) are exactly what the printer loads.
    for (int slot : referenced)
        if (is_mixed_slot(config, slot))
            compaction.slot_of_tool.push_back(slot);
    // A plate that already uses a dense prefix needs no renumbering; leaving the compaction
    // identity keeps the model copy and the config gather from running at all.
    if (compaction.is_identity())
        compaction.slot_of_tool.clear();
    return compaction;
}

// The renumbered value for a 1-based filament reference. Slots the plate doesn't print keep
// their value: they belong to a disabled feature, and inventing a tool number for them would
// claim a tool the plate never uses.
static int compacted_1based(const FilamentCompaction& compaction, int filament_1based)
{
    if (filament_1based <= 0)
        return filament_1based;
    const int tool = compaction.tool_of_slot(filament_1based - 1);
    return tool < 0 ? filament_1based : tool + 1;
}

static void compact_model_config(ModelConfig& model_config, const ModelConfig& source_config, const FilamentCompaction& compaction)
{
    auto compact_key = [&](const char* key) {
        if (const ConfigOption* opt = model_config.option(key)) {
            const int compacted = compacted_1based(compaction, opt->getInt());
            if (compacted != opt->getInt())
                model_config.set(key, compacted);
        }
    };
    compact_key("extruder");
    for (const char* key : s_region_filament_keys)
        compact_key(key);
    for (const char* key : s_support_filament_keys)
        compact_key(key);
    // The copy is a deterministic derivation of the source; its timestamp must say so, or every
    // rebuild reads as a fresh user edit (see apply_filament_compaction's doc).
    model_config.mirror_timestamp_of(source_config);
}

// Renumber multi-material painting. The painted states are filament ids, and the compaction is a
// permutation of them, so it has to be applied to every triangle at once -- which is exactly what
// TriangleSelector::remap_triangle_state does. Rebuilding the selector from the mesh is the only
// way in: the stored form is a bitstream whose state width is not addressable in place.
static void compact_mm_painting(ModelVolume& volume, const ModelVolume& source_volume, const FilamentCompaction& compaction)
{
    if (!volume.is_mm_painted())
        return;

    constexpr size_t max_state = size_t(EnforcerBlockerType::ExtruderMax);
    EnforcerBlockerStateMap state_map;
    for (size_t state = 0; state <= max_state; ++state)
        state_map[state] = EnforcerBlockerType(state);

    bool any_change = false;
    for (size_t tool = 0; tool < compaction.slot_of_tool.size(); ++tool) {
        // Painting state Extruder1 is filament 1, so state == slot + 1 == the 1-based id.
        const size_t from = size_t(compaction.slot_of_tool[tool]) + size_t(EnforcerBlockerType::Extruder1);
        const size_t to   = tool + size_t(EnforcerBlockerType::Extruder1);
        if (from > max_state || from == to)
            continue;
        state_map[from] = EnforcerBlockerType(to);
        any_change      = true;
    }
    if (!any_change)
        return;

    TriangleSelector selector(volume.mesh());
    selector.deserialize(volume.mmu_segmentation_facets.get_data(), true);
    selector.remap_triangle_state(state_map);
    volume.mmu_segmentation_facets.set_data(selector.serialize());
    // Painting is compared by TIMESTAMP during Print::apply; identical re-derived data with a
    // fresh stamp invalidates the finished slice on the next apply.
    volume.mmu_segmentation_facets.mirror_timestamp_of(source_volume.mmu_segmentation_facets);
    // ModelVolume::get_extruders caches the painted filament list keyed on that same timestamp,
    // and the cache came across with the copy: with the stamp mirrored it would keep answering
    // with the PROJECT slots, and Print::extruders would count a painted slot the compacted
    // config no longer has (a painted mix of two loaded filaments read as an eighth tool). Real
    // timestamps start at 1, so 0 can never match.
    volume.mmuseg_extruders.clear();
    volume.mmuseg_ts = 0;
}

void apply_filament_compaction(Model& model, const Model& source, const FilamentCompaction& compaction)
{
    if (compaction.slot_of_tool.empty())
        return;

    assert(model.objects.size() == source.objects.size());

    // Support references are renumbered unconditionally here. The collector must not claim a tool
    // for a feature that is switched off, but rewriting a reference this plate never reads is
    // harmless, and leaving it pointing at a project slot would be a latent inconsistency.
    for (size_t obj_i = 0; obj_i < model.objects.size(); ++obj_i) {
        ModelObject*       object        = model.objects[obj_i];
        const ModelObject* source_object = source.objects[obj_i];
        assert(object->id() == source_object->id());
        compact_model_config(object->config, source_object->config, compaction);
        for (auto& layer_range : object->layer_config_ranges) {
            const auto source_range = source_object->layer_config_ranges.find(layer_range.first);
            compact_model_config(layer_range.second,
                                 source_range != source_object->layer_config_ranges.end() ? source_range->second : layer_range.second,
                                 compaction);
        }
        assert(object->volumes.size() == source_object->volumes.size());
        for (size_t vol_i = 0; vol_i < object->volumes.size(); ++vol_i) {
            ModelVolume*       volume        = object->volumes[vol_i];
            const ModelVolume* source_volume = vol_i < source_object->volumes.size() ? source_object->volumes[vol_i] : volume;
            compact_model_config(volume->config, source_volume->config, compaction);
            compact_mm_painting(*volume, *source_volume, compaction);
        }
    }

    for (auto& plate_gcodes : model.plates_custom_gcodes)
        for (CustomGCode::Item& item : plate_gcodes.second.gcodes)
            if (item.type == CustomGCode::Type::ToolChange)
                item.extruder = compacted_1based(compaction, item.extruder);
}

// Every per-filament config vector, keyed by the same filament index space the model uses.
// Preset::filament_options() is the canonical list; filament_colour is excluded from it (it is
// edited outside the filament preset) and the map/index vectors are engine-side state that is
// filament-indexed all the same.
static const std::vector<std::string>& filament_indexed_keys()
{
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> out = Preset::filament_options();
        for (const char* extra : { "filament_colour", "filament_multi_colour", "filament_settings_id",
                                   "filament_ids", "filament_map", "filament_volume_map",
                                   "filament_nozzle_map", "filament_physical_map", "filament_self_index",
                                   // Project-level mixed-colour arrays, indexed like filament_colour.
                                   "filament_is_mixed", "filament_mixed_components",
                                   "filament_mixed_sublayer_ratios", "filament_mixed_gradient",
                                   "filament_mixed_gradient_range", "filament_mixed_gradient_curve",
                                   "filament_mixed_gradient_per_part" })
            out.emplace_back(extra);
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [](const std::string& key) {
                                     return key == "compatible_prints" || key == "compatible_printers" ||
                                            key == "compatible_prints_condition" ||
                                            key == "compatible_printers_condition";
                                 }),
                  out.end());
        return out;
    }();
    return keys;
}

void apply_filament_compaction(DynamicPrintConfig& config, const FilamentCompaction& compaction)
{
    if (compaction.slot_of_tool.empty())
        return;

    const ConfigOptionStrings* colours = config.option<ConfigOptionStrings>("filament_colour");
    const size_t old_count = colours == nullptr ? 0 : colours->size();
    const size_t new_count = compaction.slot_of_tool.size();
    if (old_count == 0 || new_count > old_count)
        return;

    for (const std::string& key : filament_indexed_keys()) {
        ConfigOption* opt = config.option(key, false);
        if (opt == nullptr || !opt->is_vector())
            continue;
        auto* vec = static_cast<ConfigOptionVectorBase*>(opt);
        // Only gather what is genuinely one-entry-per-filament. Anything else at this point --
        // an option the printer sizes by extruder, or one a preset left short -- is left alone
        // rather than silently reindexed.
        if (vec->size() != old_count)
            continue;
        const std::unique_ptr<ConfigOption> source(vec->clone());
        for (size_t tool = 0; tool < new_count; ++tool)
            vec->set_at(source.get(), tool, size_t(compaction.slot_of_tool[tool]));
        vec->resize(new_count);
    }

    // A mix's component list names physical filaments by their 1-based project number; those
    // just moved with the gather above. A component the plate does not print cannot occur here
    // (a used mix makes each of its components used), so every one has a tool.
    if (auto* comps = config.option<ConfigOptionStrings>("filament_mixed_components"); comps != nullptr && comps->size() == new_count)
        for (std::string& list : comps->values) {
            if (list.empty())
                continue;
            std::string renumbered;
            for (unsigned int component : parse_mixed_components(list))
                renumbered += (renumbered.empty() ? "" : ",") + std::to_string(compacted_1based(compaction, int(component)));
            list = renumbered;
        }

    // flush_volumes_matrix is one filament x filament block (row = source, column = target) PER
    // HEAD on a multi-head printer -- g-code export validates size == heads * f^2, with heads =
    // flush_multiplier.size() (see GCode::append_full_config) -- and a single block on
    // single-head machines. Gather every block on both axes. An earlier version handled only the
    // single-block layout, so on a four-head printer the matrix kept its old size while
    // filament_colour shrank, and export died with "Flush volumes matrix do not match to the
    // correct size!".
    if (auto* matrix = config.option<ConfigOptionFloats>("flush_volumes_matrix");
        matrix != nullptr && matrix->size() > 0 && matrix->size() % (old_count * old_count) == 0) {
        const size_t        heads = matrix->size() / (old_count * old_count);
        std::vector<double> gathered(heads * new_count * new_count, 0.);
        for (size_t head = 0; head < heads; ++head) {
            const size_t src_base = head * old_count * old_count;
            const size_t dst_base = head * new_count * new_count;
            for (size_t row = 0; row < new_count; ++row)
                for (size_t col = 0; col < new_count; ++col)
                    gathered[dst_base + row * new_count + col] =
                        matrix->values[src_base + size_t(compaction.slot_of_tool[row]) * old_count +
                                       size_t(compaction.slot_of_tool[col])];
        }
        matrix->values = std::move(gathered);
    }
    // flush_volumes_vector carries a fixed group of entries per filament (2 today, the
    // load/unload pair -- its default is 8 values for 4 filaments). Gather whole groups.
    if (auto* vector = config.option<ConfigOptionFloats>("flush_volumes_vector");
        vector != nullptr && vector->size() > 0 && vector->size() % old_count == 0) {
        const size_t        per_filament = vector->size() / old_count;
        std::vector<double> gathered(per_filament * new_count, 0.);
        for (size_t tool = 0; tool < new_count; ++tool)
            for (size_t entry = 0; entry < per_filament; ++entry)
                gathered[tool * per_filament + entry] =
                    vector->values[size_t(compaction.slot_of_tool[tool]) * per_filament + entry];
        vector->values = std::move(gathered);
    }

    // Scalar 1-based references in the project's print settings.
    auto compact_scalar = [&](const char* key) {
        if (auto* opt = config.option<ConfigOptionInt>(key); opt != nullptr)
            opt->value = compacted_1based(compaction, opt->value);
    };
    for (const char* key : s_region_filament_keys)
        compact_scalar(key);
    for (const char* key : s_support_filament_keys)
        compact_scalar(key);
    compact_scalar("wipe_tower_filament");
}

} // namespace Slic3r

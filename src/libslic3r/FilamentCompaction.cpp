#include "FilamentCompaction.hpp"

#include "Config.hpp"
#include "CustomGCode.hpp"
#include "FilamentMixer.hpp"
#include "Model.hpp"
#include "Preset.hpp"
#include "Print.hpp" // ModelFilaments
#include "PrintConfig.hpp"
#include "TriangleSelector.hpp"

#include <algorithm>
#include <cstddef>
#include <cassert>
#include <memory>
#include <vector>
#include <string>
#include <utility>

namespace Slic3r {

// 1-based filament references in a region/object config; 0 means inherit.
static const char* s_filament_keys[] = {
    "outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
    "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id",
    "support_filament", "support_interface_filament",
};

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

int FilamentCompaction::project_slot_of_tool(int tool) const
{
    return tool >= 0 && size_t(tool) < slot_of_tool.size() ? slot_of_tool[tool] : tool;
}

FilamentCompaction build_filament_compaction(const ModelFilaments& filaments, const std::vector<int>& demoted,
                                             const DynamicPrintConfig& config, size_t namespace_size)
{
    const auto* is_mixed  = config.option<ConfigOptionBools>("filament_is_mixed");
    const auto* comp_strs = config.option<ConfigOptionStrings>("filament_mixed_components");
    const bool  any_mixed = is_mixed != nullptr && comp_strs != nullptr && has_any_mixed_filament(is_mixed->values);

    // Physical slots, surest tier first.
    std::vector<int> order;
    const auto take = [&](const std::vector<unsigned int>& tier) {
        const std::vector<unsigned int> physical = any_mixed ? expand_mixed_filaments(tier, is_mixed->values, comp_strs->values) : tier;
        for (unsigned int slot : physical)
            if (std::find(order.begin(), order.end(), int(slot)) == order.end())
                order.push_back(int(slot));
    };
    FilamentCompaction compaction;
    take(filaments.parts);
    compaction.certain_count = order.size();
    take(filaments.features);
    take(filaments.wipe_tower);
    take(filaments.tool_changes);
    take(filaments.support);

    compaction.used_slots = order;
    std::sort(compaction.used_slots.begin(), compaction.used_slots.end());
    // A plate that fits the namespace keeps project numbering; only one reaching past it is packed.
    if (compaction.used_slots.empty() || size_t(compaction.used_slots.back()) + 1 <= namespace_size)
        return compaction;

    std::stable_partition(order.begin(), order.end(), [&demoted](int slot) {
        return std::find(demoted.begin(), demoted.end(), slot) == demoted.end();
    });
    compaction.slot_of_tool = std::move(order);
    if (any_mixed)
        for (unsigned int slot : filaments.all())
            if (slot < is_mixed->values.size() && is_mixed->values[slot])
                compaction.slot_of_tool.push_back(int(slot));
    if (compaction.is_identity())
        compaction.slot_of_tool.clear();
    return compaction;
}

// A reference to an unused slot becomes 0 (inherit); keeping its number would name whichever
// filament now holds that tool number.
static int compacted_1based(const FilamentCompaction& compaction, int filament_1based)
{
    if (filament_1based <= 0)
        return filament_1based;
    const int tool = compaction.tool_of_slot(filament_1based - 1);
    return tool < 0 ? 0 : tool + 1;
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
    for (const char* key : s_filament_keys)
        compact_key(key);
    model_config.mirror_timestamp_of(source_config);
}

// The serialized painting is a bitstream whose state width is not addressable in place, so the
// selector is rebuilt from the mesh to remap it.
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
    volume.mmu_segmentation_facets.mirror_timestamp_of(source_volume.mmu_segmentation_facets);
    // get_extruders caches the painted filaments keyed on that timestamp, so with it mirrored the
    // copied cache would keep the project slots. Real timestamps start at 1, so 0 never matches.
    volume.mmuseg_extruders.clear();
    volume.mmuseg_ts = 0;
}

void apply_filament_compaction(Model& model, const Model& source, const FilamentCompaction& compaction)
{
    if (compaction.slot_of_tool.empty())
        return;

    assert(model.objects.size() == source.objects.size());

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
            ModelVolume&       volume        = *object->volumes[vol_i];
            const ModelVolume& source_volume = *source_object->volumes[vol_i];
            compact_model_config(volume.config, source_volume.config, compaction);
            compact_mm_painting(volume, source_volume, compaction);
        }
    }

    for (auto& plate_gcodes : model.plates_custom_gcodes)
        for (CustomGCode::Item& item : plate_gcodes.second.gcodes)
            if (item.type == CustomGCode::Type::ToolChange)
                item.extruder = compacted_1based(compaction, item.extruder);
}

// Preset::filament_options() plus filament-indexed vectors that live outside the filament preset.
static const std::vector<std::string>& filament_indexed_keys()
{
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> out = Preset::filament_options();
        for (const char* extra : { "filament_colour", "filament_multi_colour", "filament_settings_id",
                                   "filament_ids", "filament_self_index",
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
        // Leave vectors not sized per filament (e.g. per-extruder ones) alone.
        if (vec->size() != old_count)
            continue;
        const std::unique_ptr<ConfigOption> source(vec->clone());
        for (size_t tool = 0; tool < new_count; ++tool)
            vec->set_at(source.get(), tool, size_t(compaction.slot_of_tool[tool]));
        vec->resize(new_count);
    }

    // Components are 1-based project numbers; a used mix makes each component used, so all have a tool.
    if (auto* comps = config.option<ConfigOptionStrings>("filament_mixed_components"); comps != nullptr && comps->size() == new_count)
        for (std::string& list : comps->values) {
            if (list.empty())
                continue;
            std::string renumbered;
            for (unsigned int component : parse_mixed_components(list))
                renumbered += (renumbered.empty() ? "" : ",") + std::to_string(compacted_1based(compaction, int(component)));
            list = renumbered;
        }

    // flush_volumes_matrix holds one filament x filament block (row = source, column = target) per
    // head; export checks size == heads * f^2.
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
    // flush_volumes_vector carries a fixed group of entries per filament (the load/unload pair).
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

    auto compact_scalar = [&](const char* key) {
        if (auto* opt = config.option<ConfigOptionInt>(key); opt != nullptr)
            opt->value = compacted_1based(compaction, opt->value);
    };
    for (const char* key : s_filament_keys)
        compact_scalar(key);
    compact_scalar("wipe_tower_filament");
}

} // namespace Slic3r

#include "FilamentCompaction.hpp"

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

std::vector<int> used_filament_slots(const Model& model, const DynamicPrintConfig& config)
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

FilamentCompaction build_filament_compaction(const Model& model, const DynamicPrintConfig& config)
{
    FilamentCompaction compaction;
    compaction.slot_of_tool = used_filament_slots(model, config);
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

static void compact_model_config(ModelConfig& model_config, const FilamentCompaction& compaction)
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
}

// Renumber multi-material painting. The painted states are filament ids, and the compaction is a
// permutation of them, so it has to be applied to every triangle at once -- which is exactly what
// TriangleSelector::remap_triangle_state does. Rebuilding the selector from the mesh is the only
// way in: the stored form is a bitstream whose state width is not addressable in place.
static void compact_mm_painting(ModelVolume& volume, const FilamentCompaction& compaction)
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
}

void apply_filament_compaction(Model& model, const FilamentCompaction& compaction)
{
    if (compaction.slot_of_tool.empty())
        return;

    // Support references are renumbered unconditionally here. The collector must not claim a tool
    // for a feature that is switched off, but rewriting a reference this plate never reads is
    // harmless, and leaving it pointing at a project slot would be a latent inconsistency.
    for (ModelObject* object : model.objects) {
        compact_model_config(object->config, compaction);
        for (auto& layer_range : object->layer_config_ranges)
            compact_model_config(layer_range.second, compaction);
        for (ModelVolume* volume : object->volumes) {
            compact_model_config(volume->config, compaction);
            compact_mm_painting(*volume, compaction);
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
                                   "filament_nozzle_map", "filament_physical_map", "filament_self_index" })
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

    // flush_volumes_matrix is filament x filament (row = source, column = target), so it gathers
    // on both axes; flush_volumes_vector is one entry per filament.
    if (auto* matrix = config.option<ConfigOptionFloats>("flush_volumes_matrix");
        matrix != nullptr && matrix->size() == old_count * old_count) {
        std::vector<double> gathered(new_count * new_count, 0.);
        for (size_t row = 0; row < new_count; ++row)
            for (size_t col = 0; col < new_count; ++col)
                gathered[row * new_count + col] =
                    matrix->values[size_t(compaction.slot_of_tool[row]) * old_count + size_t(compaction.slot_of_tool[col])];
        matrix->values = std::move(gathered);
    }
    if (auto* vector = config.option<ConfigOptionFloats>("flush_volumes_vector");
        vector != nullptr && vector->size() == old_count) {
        std::vector<double> gathered(new_count, 0.);
        for (size_t tool = 0; tool < new_count; ++tool)
            gathered[tool] = vector->values[size_t(compaction.slot_of_tool[tool])];
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

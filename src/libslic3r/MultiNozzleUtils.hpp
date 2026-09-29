#ifndef MULTI_NOZZLE_UTILS_HPP
#define MULTI_NOZZLE_UTILS_HPP

#include <vector>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include "PrintConfig.hpp"

// Multi-nozzle support types.
// Declares the filament-grouping result types the slicing pipeline needs, plus the analytic
// filament-change-time model (FilamentChangeTimeParams, NozzleStatusRecorder,
// FilamentChangeSimResult, simulate_filament_change_time) — self-contained analytic code that
// never touches the time estimator; its first consumer is the filament_group golden harness.
// The gcode.3mf serialization surface lives here too: NozzleInfo/NozzleGroupInfo
// serialize+deserialize, the device-side StaticNozzleGroupResult,
// load_nozzle_infos_with_compatibility (the backward-compat 3mf reader) and
// LayeredNozzleGroupResult::estimate_seq_flush_weight. The change-time-tuning helpers
// calc_filament_change_gap_for_assignment / find_optimal_physical_assignment (used only by the
// AMS pre-load optimizer, a later feature) are not implemented here.

namespace Slic3r {
struct FilamentInfo; // Slic3r::FilamentInfo (ProjectTask.hpp) — consumed by StaticNozzleGroupResult / the 3mf reader
namespace MultiNozzleUtils {

// Information about a single logical nozzle.
struct NozzleInfo
{
    std::string      diameter;
    NozzleVolumeType volume_type;
    int              extruder_id{-1}; // logical extruder id
    int              group_id{-1};    // logical nozzle id

    std::string serialize() const;

    bool operator<(const NozzleInfo& other) const {
        if(group_id != other.group_id) return group_id < other.group_id;
        if(extruder_id != other.extruder_id) return extruder_id < other.extruder_id;
        if(volume_type != other.volume_type) return volume_type < other.volume_type;
        return diameter < other.diameter;
    }
};

// A group of identical nozzles on one extruder (diameter + volume type + count).
struct NozzleGroupInfo
{
    std::string      diameter;
    NozzleVolumeType volume_type;
    int              extruder_id;
    int              nozzle_count;

    NozzleGroupInfo() = default;

    NozzleGroupInfo(const std::string& nozzle_diameter_, const NozzleVolumeType volume_type_, const int extruder_id_, const int nozzle_count_)
        : diameter(nozzle_diameter_), volume_type(volume_type_), extruder_id(extruder_id_), nozzle_count(nozzle_count_)
    {}

    inline bool operator<(const NozzleGroupInfo &rhs) const
    {
        if (extruder_id != rhs.extruder_id) return extruder_id < rhs.extruder_id;
        if (diameter != rhs.diameter) return diameter < rhs.diameter;
        if (volume_type != rhs.volume_type) return volume_type < rhs.volume_type;
        return nozzle_count < rhs.nozzle_count;
    }

    bool is_same_type(const NozzleGroupInfo &rhs) const
    {
        return diameter == rhs.diameter && volume_type == rhs.volume_type && extruder_id == rhs.extruder_id;
    }

    inline bool operator==(const NozzleGroupInfo &rhs) const
    {
        return diameter == rhs.diameter && volume_type == rhs.volume_type && extruder_id == rhs.extruder_id && nozzle_count == rhs.nozzle_count;
    }

    std::string serialize() const;
    static std::optional<NozzleGroupInfo> deserialize(const std::string& str);
};

// Load/unload time constants used by the filament-change-time model.
// Consumed by simulate_filament_change_time() below and carried by the grouping-context
// substrate (FilamentGroupContext::SpeedInfo).
struct FilamentChangeTimeParams
{
    float selector_load_time{0.0f};
    float selector_unload_time{0.0f};
    float standard_load_time{0.0f};
    float standard_unload_time{0.0f};
};

/**
 * @brief Abstract base for a nozzle-grouping result.
 */
class NozzleGroupResultBase
{
protected:
    bool support_dynamic_nozzle_map{false}; // whether dynamic (selector) mapping is used

public:
    NozzleGroupResultBase(bool support_dynamic_map = false) : support_dynamic_nozzle_map(support_dynamic_map) {}
    virtual ~NozzleGroupResultBase() = default;

    virtual std::optional<NozzleInfo> get_nozzle_from_id(int nozzle_id) const = 0;
    virtual std::optional<NozzleInfo> get_first_nozzle_for_filament(int filament_id) const = 0; // logical nozzle a filament first uses

    virtual std::vector<NozzleInfo> get_nozzles_for_filament(int filament_id) const = 0; // every nozzle a filament may use (across all layers)

    bool is_support_dynamic_nozzle_map() const { return support_dynamic_nozzle_map; }

    virtual int get_extruder_count() const = 0;

    virtual std::vector<NozzleInfo> get_used_nozzles_in_extruder(int extruder_id =-1) const = 0;
    virtual std::vector<int> get_used_extruders() const = 0;
    virtual std::vector<unsigned int> get_used_filaments() const = 0;
};

/**
 * @brief Layer-aware nozzle-grouping result.
 * Used by the back-end slicing code; supports per-layer nozzle mapping.
 */
class LayeredNozzleGroupResult : public NozzleGroupResultBase
{
private:
    std::vector<std::vector<int>>          _layer_filament_nozzle_maps; // per-layer filament -> nozzle map
    std::vector<std::vector<unsigned int>> _layer_filament_sequences;   // per-layer filament print order
    std::vector<int>                       _default_filament_nozzle_map; // global filament -> nozzle map
    std::vector<unsigned int>              _used_filaments;              // all used filament indices
    std::vector<NozzleInfo>                _nozzle_list;                 // global nozzle list

public:
    LayeredNozzleGroupResult(bool support_dynamic_map = false) : NozzleGroupResultBase(support_dynamic_map) {}

    // No selector: one global filament->nozzle map.
    static std::optional<LayeredNozzleGroupResult> create(
        const std::vector<int>&          filament_nozzle_map,
        const std::vector<NozzleInfo>&   nozzle_list,
        const std::vector<unsigned int>& used_filaments);

    // Selector: built from per-layer maps (each layer may differ).
    static std::optional<LayeredNozzleGroupResult> create(
        const std::vector<std::vector<int>>&          layer_filament_nozzle_maps,
        const std::vector<NozzleInfo>&                nozzle_list,
        const std::vector<unsigned int>&              used_filaments,
        const std::vector<std::vector<unsigned int>>& layer_filament_sequences);

    // Multi-nozzle without selector: resolve each requested logical nozzle to a physical nozzle.
    static std::optional<LayeredNozzleGroupResult> create(
        const std::vector<unsigned int>&                    used_filaments,
        const std::vector<int>&                             filament_map,
        const std::vector<int>&                             filament_volume_map,
        const std::vector<int>&                             filament_nozzle_map,
        const std::vector<std::map<NozzleVolumeType, int>>& nozzle_count,
        float                                               diameter);

    bool are_filaments_same_extruder(int filament_id1, int filament_id2, int layer_id = -1) const;
    bool are_filaments_same_nozzle(int filament_id1, int filament_id2, int layer_id = -1) const;
    int get_extruder_count() const override;

    std::vector<NozzleInfo> get_used_nozzles_in_extruder(int target_extruder_id = -1) const override;
    std::vector<NozzleInfo> get_used_nozzles_in_extruder(int target_extruder_id, int layer_id) const; // layer_id=-1 uses default map
    std::vector<int> get_used_extruders() const override;
    std::vector<int> get_used_extruders(int layer_id) const; // layer_id=-1 returns global extruders

    std::vector<int> get_extruder_map(bool zero_based = true, int layer_id = -1) const;
    std::vector<int> get_nozzle_map(int layer_id = -1) const;
    std::vector<int> get_volume_map(int layer_id = -1) const;

    std::vector<unsigned int> get_used_filaments() const override { return _used_filaments; }
    std::vector<unsigned int> get_used_filaments(int layer_id) const;

    std::optional<NozzleInfo> get_nozzle_for_filament(int filament_id, int layer_id = -1) const;
    std::vector<NozzleInfo> get_nozzles_for_filament(int filament_id) const override;

    std::optional<NozzleInfo> get_nozzle_from_id(int nozzle_id) const override;
    std::optional<NozzleInfo> get_first_nozzle_for_filament(int filament_id) const override;
    int get_extruder_id(int filament_id, int layer_id = -1) const;
    int get_nozzle_id(int filament_id, int layer_id = -1) const;

    size_t get_layer_count() const { return _layer_filament_nozzle_maps.size(); }
    const std::vector<int>& get_layer_filament_nozzle_map(int layer_id) const;
    const std::vector<std::vector<int>> &get_layer_filament_nozzle_maps() const { return _layer_filament_nozzle_maps; }
    const std::vector<std::vector<unsigned int>>& get_layer_filament_sequences() const { return _layer_filament_sequences; }

    // Estimate the flush weight of a filament-change sequence given the per-extruder flush matrix
    // (extruder -> from-filament -> to-filament).
    int estimate_seq_flush_weight(const std::vector<std::vector<std::vector<float>>>& flush_matrix, const std::vector<int>& filament_change_seq) const;
};

/**
 * @brief Layer-less nozzle-grouping result for the device side (static nozzle mapping only).
 * Reconstructed from a loaded gcode.3mf together with the filament/nozzle change sequences.
 */
class StaticNozzleGroupResult : public NozzleGroupResultBase
{
private:
    std::map<int, std::set<int>> _filament_to_nozzles; // every nozzle a filament may map to
    std::map<int, NozzleInfo>    _nozzle_list_map;      // used nozzles, keyed by logical nozzle id
    std::vector<int>             _filament_change_seq;  // filament sequence used to resolve first-use
    std::vector<int>             _nozzle_change_seq;    // logical-nozzle sequence paired with the filament sequence

public:
    StaticNozzleGroupResult(bool support_dynamic_map) : NozzleGroupResultBase(support_dynamic_map) {}
    // Build from a loaded 3mf, with the filament/nozzle change sequences.
    static std::optional<StaticNozzleGroupResult> create(
        const std::vector<FilamentInfo>& filaments_info,
        const std::vector<NozzleInfo>&   nozzles_info,
        const std::vector<int>&          filament_change_seq,
        const std::vector<int>&          nozzle_change_seq,
        bool support_dynamic_map);

    int get_extruder_count() const override;
    std::vector<NozzleInfo> get_used_nozzles_in_extruder(int extruder_id = -1) const override;
    std::vector<int> get_used_extruders() const override;
    std::vector<unsigned int> get_used_filaments() const override;

    std::optional<NozzleInfo> get_nozzle_from_id(int nozzle_id) const override;

    std::vector<NozzleInfo> get_nozzles_for_filament(int filament_id) const override;
    std::optional<NozzleInfo> get_first_nozzle_for_filament(int filament_id) const override;
};

// Tracks, during the filament-change simulation, which filament sits in each physical nozzle
// and which nozzle each extruder currently carries.
class NozzleStatusRecorder
{
private:
    std::unordered_map<int, int> nozzle_filament_status; // Track filament in each nozzle
    std::unordered_map<int, int> extruder_nozzle_status; // Track nozzle for each extruder
    int current_extruder_id_ = -1;                       // Track current extruder id

public:
    NozzleStatusRecorder() = default;
    bool is_nozzle_empty(int nozzle_id) const;
    int  get_filament_in_nozzle(int nozzle_id) const;
    int  get_nozzle_in_extruder(int extruder_id) const;
    int  get_current_extruder_id() const { return current_extruder_id_; }

    void clear_nozzle_status(int nozzle_id);
    void set_current_extruder_id(int extruder_id) { current_extruder_id_ = extruder_id; }

    // Update the status of a nozzle with new filament and extruder information
    void set_nozzle_status(int nozzle_id, int filament_id, int extruder_id = -1);

    // key: nozzle id, value: filament id (-1 = the nozzle carries no filament)
    const std::unordered_map<int, int>& get_nozzle_filament_map() const { return nozzle_filament_status; }
    // key: extruder id, value: nozzle id (-1 = the extruder carries no nozzle)
    const std::unordered_map<int, int>& get_extruder_nozzle_map() const { return extruder_nozzle_status; }
};

struct FilamentChangeSimResult {
    double actual_time = 0.0;
    double sliced_time = 0.0;
};

// Analytic filament-change-time model. Given the used filaments, the nozzle
// list, the filament/nozzle change sequences, each filament's AMS group and the load/unload time
// constants, it simulates AMS->selector->extruder transport (with optional AMS pre-load overlap)
// and returns the actual print time plus the slicer-estimated time. Self-contained: it never
// touches the g-code time estimator.
FilamentChangeSimResult simulate_filament_change_time(
    const std::vector<int>&           logical_filaments,
    const std::vector<NozzleInfo>&    nozzle_list,
    const std::vector<int>&           filament_change_seq,
    const std::vector<int>&           nozzle_change_seq,
    const std::vector<int>&           group_of_filament,
    const FilamentChangeTimeParams&   time_params,
    const std::vector<bool>&          ams_preload_enabled = {},
    bool                              calc_sliced_time = false);

// ==================== tool functions ====================
// Make each filament's per-layer nozzle assignment gap-free: layers where a filament is not
// extruded inherit the nozzle it last used (forward carry); layers before its first use inherit
// the first nozzle it ever uses (back-fill). Entries on layers where the filament is actually
// used stay untouched. Needed for stitched sequential maps, where consumers indexing with an
// object-local layer id must resolve the same nozzle as global-id consumers except across a
// genuine mid-print reassignment.
void normalize_nozzle_map_per_layer(std::vector<std::vector<int>>&                layer_filament_nozzle_maps,
                                    const std::vector<std::vector<unsigned int>>& layer_filaments);
std::vector<NozzleInfo> build_nozzle_list(std::vector<NozzleGroupInfo> info);
std::vector<NozzleInfo> build_nozzle_list(double diameter, const std::vector<int>& filament_nozzle_map,
                                          const std::vector<int>& filament_volume_map, const std::vector<int>& filament_map);
// Load nozzle infos from a gcode.3mf, handling backward compatibility with older 3mf that did not
// record standalone <nozzle> tags: falls back to the per-filament group_id/diameter/volume_type, and
// (for the oldest single-nozzle 3mf) to the filament_map + extruder volume types + nozzle diameters.
std::vector<NozzleInfo> load_nozzle_infos_with_compatibility(
    const std::vector<NozzleInfo>& nozzle_infos,
    const std::vector<FilamentInfo>& filament_infos,
    const std::vector<int>& filament_map,
    const std::vector<NozzleVolumeType>& extruder_volume_types,
    const std::vector<double>& nozzle_diameter
);
} // namespace MultiNozzleUtils
} // namespace Slic3r

#endif // MULTI_NOZZLE_UTILS_HPP

#ifndef MULTI_NOZZLE_UTILS_HPP
#define MULTI_NOZZLE_UTILS_HPP

#include <vector>
#include <map>
#include <optional>
#include <set>
#include "PrintConfig.hpp"

namespace Slic3r {
struct FilamentInfo;
namespace MultiNozzleUtils {

// Info for a single nozzle
struct NozzleInfo
{
    std::string      diameter;
    NozzleVolumeType volume_type;
    int              extruder_id{-1}; // logical extruder id
    int              group_id{-1};    // corresponding logical nozzle id

    std::string serialize() const;

    bool operator<(const NozzleInfo& other) const {
        if (group_id != other.group_id) return group_id < other.group_id;
        if (extruder_id != other.extruder_id) return extruder_id < other.extruder_id;
        if (volume_type != other.volume_type) return volume_type < other.volume_type;
        return diameter < other.diameter;
    }
};

// Nozzle group info passed from frontend to backend after a sync
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

struct FilamentChangeTimeParams
{
    float selector_load_time{0.0f};
    float selector_unload_time{0.0f};
    float standard_load_time{0.0f};
    float standard_unload_time{0.0f};
};

/**
 * @brief Abstract base for nozzle grouping results
 */
class NozzleGroupResultBase
{
protected:
    bool support_dynamic_nozzle_map{false}; // whether dynamic mapping (material selector) is supported

public:
    NozzleGroupResultBase(bool support_dynamic_map = false) : support_dynamic_nozzle_map(support_dynamic_map) {}
    virtual ~NozzleGroupResultBase() = default;

    virtual std::optional<NozzleInfo> get_nozzle_from_id(int nozzle_id) const = 0;
    virtual std::optional<NozzleInfo> get_first_nozzle_for_filament(int filament_id) const = 0;

    virtual std::vector<NozzleInfo> get_nozzles_for_filament(int filament_id) const = 0;

    bool is_support_dynamic_nozzle_map() const { return support_dynamic_nozzle_map; }

    virtual int get_extruder_count() const = 0;

    virtual std::vector<NozzleInfo> get_used_nozzles_in_extruder(int extruder_id = -1) const = 0;
    virtual std::vector<int> get_used_extruders() const = 0;
    virtual std::vector<unsigned int> get_used_filaments() const = 0;
};

/**
 * @brief Nozzle grouping result WITH per-layer info
 * Used by backend slicing code; supports per-layer nozzle mapping.
 */
class LayeredNozzleGroupResult : public NozzleGroupResultBase
{
private:
    std::vector<std::vector<int>>          _layer_filament_nozzle_maps;
    std::vector<std::vector<unsigned int>> _layer_filament_sequences;
    std::vector<int>                       _default_filament_nozzle_map;
    std::vector<unsigned int>              _used_filaments;
    std::vector<NozzleInfo>                _nozzle_list;

public:
    LayeredNozzleGroupResult(bool support_dynamic_map = false) : NozzleGroupResultBase(support_dynamic_map) {}

    static std::optional<LayeredNozzleGroupResult> create(
        const std::vector<int>&          filament_nozzle_map,
        const std::vector<NozzleInfo>&   nozzle_list,
        const std::vector<unsigned int>& used_filaments);

    static std::optional<LayeredNozzleGroupResult> create(
        const std::vector<std::vector<int>>&          layer_filament_nozzle_maps,
        const std::vector<NozzleInfo>&                nozzle_list,
        const std::vector<unsigned int>&              used_filaments,
        const std::vector<std::vector<unsigned int>>& layer_filament_sequences);

    static std::optional<LayeredNozzleGroupResult> create(
        const std::vector<unsigned int>&                    used_filaments,
        const std::vector<int>&                             filament_map,
        const std::vector<int>&                             filament_volume_map,
        const std::vector<int>&                             filament_nozzle_map,
        const std::vector<std::map<NozzleVolumeType, int>>& nozzle_count,
        float                                               diameter);

    bool are_filaments_same_extruder(int filament_id1, int filament_id2, int layer_id = -1) const;
    bool are_filaments_same_nozzle(int filament_id1, int filament_id2, int layer_id = -1) const;
    int  get_extruder_count() const override;

    std::vector<NozzleInfo> get_used_nozzles_in_extruder(int target_extruder_id = -1) const override;
    std::vector<NozzleInfo> get_used_nozzles_in_extruder(int target_extruder_id, int layer_id) const;
    std::vector<int> get_used_extruders() const override;
    std::vector<int> get_used_extruders(int layer_id) const;

    std::vector<int> get_extruder_map(bool zero_based = true, int layer_id = -1) const;
    std::vector<int> get_nozzle_map(int layer_id = -1) const;
    std::vector<int> get_volume_map(int layer_id = -1) const;

    std::vector<unsigned int> get_used_filaments() const override { return _used_filaments; }
    std::vector<unsigned int> get_used_filaments(int layer_id) const;

    std::optional<NozzleInfo> get_nozzle_for_filament(int filament_id, int layer_id = -1) const;
    std::vector<NozzleInfo>   get_nozzles_for_filament(int filament_id) const override;

    std::optional<NozzleInfo> get_nozzle_from_id(int nozzle_id) const override;
    std::optional<NozzleInfo> get_first_nozzle_for_filament(int filament_id) const override;
    int get_extruder_id(int filament_id, int layer_id = -1) const;
    int get_nozzle_id(int filament_id, int layer_id = -1) const;

    size_t get_layer_count() const { return _layer_filament_nozzle_maps.size(); }
    const std::vector<int>& get_layer_filament_nozzle_map(int layer_id) const;
    const std::vector<std::vector<int>>& get_layer_filament_nozzle_maps() const { return _layer_filament_nozzle_maps; }
    const std::vector<std::vector<unsigned int>>& get_layer_filament_sequences() const { return _layer_filament_sequences; }

    int estimate_seq_flush_weight(const std::vector<std::vector<std::vector<float>>>& flush_matrix, const std::vector<int>& filament_change_seq) const;
};

/**
 * @brief Nozzle grouping result WITHOUT layer info
 * Used on the device side; static nozzle mapping only.
 */
class StaticNozzleGroupResult : public NozzleGroupResultBase
{
private:
    std::map<int, std::set<int>> _filament_to_nozzles;
    std::map<int, NozzleInfo>    _nozzle_list_map;
    std::vector<int>             _filament_change_seq;
    std::vector<int>             _nozzle_change_seq;

public:
    StaticNozzleGroupResult(bool support_dynamic_map) : NozzleGroupResultBase(support_dynamic_map) {}
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

class NozzleStatusRecorder
{
private:
    std::unordered_map<int, int> nozzle_filament_status;
    std::unordered_map<int, int> extruder_nozzle_status;
    int current_extruder_id_ = -1;

public:
    NozzleStatusRecorder() = default;
    bool is_nozzle_empty(int nozzle_id) const;
    int  get_filament_in_nozzle(int nozzle_id) const;
    int  get_nozzle_in_extruder(int extruder_id) const;
    int  get_current_extruder_id() const { return current_extruder_id_; }

    void clear_nozzle_status(int nozzle_id);
    void set_current_extruder_id(int extruder_id) { current_extruder_id_ = extruder_id; }

    void set_nozzle_status(int nozzle_id, int filament_id, int extruder_id = -1);

    const std::unordered_map<int, int>& get_nozzle_filament_map() const { return nozzle_filament_status; }
    const std::unordered_map<int, int>& get_extruder_nozzle_map() const { return extruder_nozzle_status; }
};

// H2C port (BBL 2f014ce1a): split the filament-change-time simulation out of
// calc_filament_change_gap_for_assignment so callers can obtain the raw actual
// and sliced times (not just their gap).
struct FilamentChangeSimResult {
    double actual_time = 0.0;
    double sliced_time = 0.0;
};

FilamentChangeSimResult simulate_filament_change_time(
    const std::vector<int>&           logical_filaments,
    const std::vector<NozzleInfo>&    nozzle_list,
    const std::vector<int>&           filament_change_seq,
    const std::vector<int>&           nozzle_change_seq,
    const std::vector<int>&           group_of_filament,
    const FilamentChangeTimeParams&   time_params,
    const std::vector<bool>&          ams_preload_enabled = {},
    bool                              calc_sliced_time = false);

float calc_filament_change_gap_for_assignment(
    const std::vector<int>&           logical_filaments,
    const std::vector<NozzleInfo>&    nozzle_list,
    const std::vector<int>&           filament_change_seq,
    const std::vector<int>&           nozzle_change_seq,
    const std::vector<int>&           group_of_filament,
    const FilamentChangeTimeParams&   time_params,
    const std::vector<bool>&          ams_preload_enabled = {});

std::vector<int> find_optimal_physical_assignment(
    const std::vector<int>&           logical_filaments,
    const std::vector<NozzleInfo>&    nozzle_list,
    const std::vector<int>&           filament_change_seq,
    const std::vector<int>&           nozzle_change_seq,
    int                               group_count,
    const FilamentChangeTimeParams&   time_params,
    int                               max_ms = 1000);

// ==================== Utility functions ====================
std::vector<NozzleInfo> build_nozzle_list(std::vector<NozzleGroupInfo> info);
std::vector<NozzleInfo> build_nozzle_list(double diameter, const std::vector<int>& filament_nozzle_map,
                                          const std::vector<int>& filament_volume_map, const std::vector<int>& filament_map);
std::vector<NozzleInfo> load_nozzle_infos_with_compatibility(
    const std::vector<NozzleInfo>& nozzle_infos,
    const std::vector<FilamentInfo>& filament_infos,
    const std::vector<int>& filament_map,
    const std::vector<NozzleVolumeType>& extruder_volume_types,
    const std::vector<double>& nozzle_diameter
);

// ==================== Transitional: legacy MultiNozzleGroupResult ====================
// Kept until all consumers (Print, ToolOrdering, GCodeProcessor) are migrated to
// shared_ptr<NozzleGroupResultBase>. New code should use LayeredNozzleGroupResult.
class MultiNozzleGroupResult
{
private:
    std::unordered_map<int, std::unordered_map<int, NozzleInfo>> extruder_to_filament_nozzles;
    std::vector<NozzleInfo>   filament_to_nozzle;
    std::vector<unsigned int> used_filaments;
    std::vector<int>          filament_map; // extruder map
public:
    MultiNozzleGroupResult() = default;
    MultiNozzleGroupResult(const std::vector<int> &filament_nozzle_map, const std::vector<NozzleInfo> &nozzle_list, const std::vector<unsigned int>& used_filament);
    static std::optional<MultiNozzleGroupResult> init_from_slice_filament(const std::vector<int>          &filament_map,
                                                                          const std::vector<FilamentInfo> &filament_info);

    static std::optional<MultiNozzleGroupResult> init_from_cli_config(const std::vector<unsigned int>& used_filaments,
                                                                      const std::vector<int>& filament_map,
                                                                      const std::vector<int>& filament_volume_map,
                                                                      const std::vector<int>& filament_nozzle_map,
                                                                      const std::vector<std::map<NozzleVolumeType,int>>& nozzle_count,
                                                                      float diameter);

    bool                    are_filaments_same_extruder(int filament_id1, int filament_id2) const;
    bool                    are_filaments_same_nozzle(int filament_id1, int filament_id2) const;
    int                     get_extruder_count() const;

    std::vector<NozzleInfo> get_used_nozzles(const std::vector<unsigned int> &filament_list, int target_extruder_id = -1) const;
    std::vector<int>        get_used_extruders(const std::vector<unsigned int> &filament_list) const;
    std::pair<int, int>     get_used_extruders_nozzles_count(const std::vector<unsigned int> &filament_list) const;
    std::vector<int>        get_extruder_list() const;

    std::vector<int>        get_extruder_map(bool zero_based = true) const;
    std::vector<int>        get_nozzle_map() const;
    std::vector<int>        get_volume_map() const;


    int  get_config_idx_for_filament(int filament_idx, const PrintConfig& config);

    int estimate_seq_flush_weight(const std::vector<std::vector<std::vector<float>>>& flush_matrix, const std::vector<int>& filament_change_seq) const;

public:
    int                       get_extruder_id(int filament_id) const;
    int                       get_nozzle_count(int extruder_id = -1) const;
    std::vector<NozzleInfo>   get_nozzle_vec(int extruder_id = -1) const;
    std::vector<NozzleInfo>   get_used_nozzles_in_extruder(int extruder_id = -1) const { return get_nozzle_vec(extruder_id); }
    std::optional<NozzleInfo> get_nozzle_for_filament(int filament_id) const;
    std::vector<NozzleInfo> get_nozzles_for_filament(int filament_id) const {
        auto opt = get_nozzle_for_filament(filament_id);
        return opt ? std::vector<NozzleInfo>{*opt} : std::vector<NozzleInfo>{};
    }

    std::unordered_map<const PrintConfig*, std::vector<int>> config_idx_map;
};

} // namespace MultiNozzleUtils
} // namespace Slic3r

#endif // MULTI_NOZZLE_UTILS_HPP

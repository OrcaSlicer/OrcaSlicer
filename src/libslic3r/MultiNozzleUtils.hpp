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
// 存储单个喷嘴的信息
struct NozzleInfo
{
    std::string      diameter;
    NozzleVolumeType volume_type;
    int              extruder_id{ -1 }; // 逻辑挤出机id
    int              group_id{ -1 };    // 对应逻辑喷嘴id, 无实际意义
};

// 喷嘴组信息，执行同步操作后前端传递给后端的数据
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

// 分组后的结果，GCodeProcessor，发起打印页面需要
class MultiNozzleGroupResult
{
private:
    std::unordered_map<int, std::unordered_map<int, NozzleInfo>> extruder_to_filament_nozzles;
    std::vector<NozzleInfo> filament_to_nozzle;
    std::vector<unsigned int> used_filaments;
    std::vector<int> filament_map; // extruder map
public:
    MultiNozzleGroupResult() = default;
    MultiNozzleGroupResult(const std::vector<int> &filament_nozzle_map, const std::vector<NozzleInfo> &nozzle_list, const std::vector<unsigned int>& used_filament);
    static std::optional<MultiNozzleGroupResult> init_from_slice_filament(const std::vector<int>          &filament_map,
                                                                          const std::vector<FilamentInfo> &filament_info); // 1 based filament_map

    static std::optional<MultiNozzleGroupResult> init_from_cli_config(const std::vector<unsigned int>& used_filaments,
                                                                      const std::vector<int>& filament_map,
                                                                      const std::vector<int>& filament_volume_map,
                                                                      const std::vector<int>& filament_nozzle_map,
                                                                      const std::vector<std::map<NozzleVolumeType,int>>& nozzle_count,
                                                                      float diameter);

    bool                                         are_filaments_same_extruder(int filament_id1, int filament_id2) const;    // 判断两个材料是否处于同一个挤出机
    bool                                         are_filaments_same_nozzle(int filament_id1, int filament_id2) const;      // 判断两个材料是否处于同一个喷嘴
    int                                          get_extruder_count() const;                                               // 获取挤出机数量

    std::vector<NozzleInfo> get_used_nozzles(const std::vector<unsigned int> &filament_list, int target_extruder_id = -1) const; // 获取给定材料列表下指定挤出机使用的喷嘴
    std::vector<int>        get_used_extruders(const std::vector<unsigned int> &filament_list) const;                            // 获取使用的挤出机列表
    std::pair<int, int>     get_used_extruders_nozzles_count(const std::vector<unsigned int> &filament_list) const; // 获取给定材料列表下使用的挤出机，及对应的喷嘴
    std::vector<int>        get_extruder_list() const;

    std::vector<int>        get_extruder_map(bool zero_based = true) const; 
    std::vector<int>        get_nozzle_map() const;
    std::vector<int>        get_volume_map() const;


    int  get_config_idx_for_filament(int filament_idx, const PrintConfig& config);

    /**
     * @brief 预估给定序列的冲刷重量
     *
     * @param flush_matrix 换挤出机的矩阵，挤出机-起始耗材-结束耗材
     * @param filament_change_seq 换料序列
     * @return int 冲刷重量
     */
    int estimate_seq_flush_weight(const std::vector<std::vector<std::vector<float>>>& flush_matrix, const std::vector<int>& filament_change_seq) const;

public:
    int                       get_extruder_id(int filament_id) const;       // 根据材料id取逻辑挤出机id
    int                       get_nozzle_count(int extruder_id = -1) const; // 获取指定挤出机下的使用的喷嘴数量，-1表示所有挤出机的喷嘴数量总和
    std::vector<NozzleInfo>   get_nozzle_vec(int extruder_id = -1) const; // 获取指定挤出机下的使用的喷嘴列表，-1表示所有挤出机的喷嘴
    std::optional<NozzleInfo> get_nozzle_for_filament(int filament_id) const;

    std::unordered_map<const PrintConfig*, std::vector<int>> config_idx_map;
};

class NozzleStatusRecorder
{
private:
    std::unordered_map<int, int> nozzle_filament_status;

public:
    NozzleStatusRecorder() = default;
    bool is_nozzle_empty(int nozzle_id) const;
    int  get_filament_in_nozzle(int nozzle_id) const;

    void clear_nozzle_status(int nozzle_id);
    void set_nozzle_status(int nozzle_id, int filament_id);
};


std::vector<NozzleInfo> build_nozzle_list(std::vector<NozzleGroupInfo> info);
std::vector<NozzleInfo> build_nozzle_list(double diameter,const std::vector<int>& filament_nozzle_map, const std::vector<int>& filament_volume_map, const std::vector<int>& filament_map);


} // namespace MultiNozzleUtils
} // namespace Slic3r

#endif
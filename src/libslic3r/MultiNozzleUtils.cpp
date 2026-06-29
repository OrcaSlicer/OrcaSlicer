#include "MultiNozzleUtils.hpp"
#include "ProjectTask.hpp"
#include "Utils.hpp"
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace MultiNozzleUtils {

std::vector<NozzleInfo> build_nozzle_list(std::vector<NozzleGroupInfo> nozzle_groups)
{
    std::vector<NozzleInfo> ret;
    std::sort(nozzle_groups.begin(), nozzle_groups.end());
    int nozzle_id = 0;
    for (auto& group : nozzle_groups) {
        for (int i = 0; i < group.nozzle_count; ++i) {
            NozzleInfo tmp;
            tmp.diameter = group.diameter;
            tmp.extruder_id = group.extruder_id;
            tmp.volume_type = group.volume_type;
            tmp.group_id = nozzle_id++;
            ret.emplace_back(std::move(tmp));
        }
    }
    return ret;
}

std::vector<NozzleInfo> build_nozzle_list(double diameter, const std::vector<int>& filament_nozzle_map, const std::vector<int>& filament_volume_map, const std::vector<int>& filament_map)
{
    std::string diameter_str = format_diameter_to_str(diameter);
    std::map<int, std::vector<int>> nozzle_to_filaments;
    for(size_t idx = 0; idx < filament_nozzle_map.size(); ++idx){
        int nozzle_id = filament_nozzle_map[idx];
        nozzle_to_filaments[nozzle_id].emplace_back(static_cast<int>(idx));
    }
    std::vector<NozzleInfo> ret;
    for(auto& elem : nozzle_to_filaments){
        int nozzle_id = elem.first;
        auto& filaments = elem.second;
        NozzleInfo info;
        info.diameter = diameter_str;
        info.group_id = nozzle_id;
        info.extruder_id = filament_map[filaments.front()];
        info.volume_type = NozzleVolumeType(filament_volume_map[filaments.front()]);
        ret.emplace_back(std::move(info));
    }
    return ret;
}


MultiNozzleGroupResult::MultiNozzleGroupResult(const std::vector<int> &filament_nozzle_map, const std::vector<NozzleInfo> &nozzle_list, const std::vector<unsigned int>& used_filaments_)
{
    filament_map = filament_nozzle_map;
    used_filaments = used_filaments_;
    filament_to_nozzle.resize(filament_nozzle_map.size());
    for (size_t filament_idx = 0; filament_idx < filament_nozzle_map.size(); ++filament_idx) {
        int               nozzle_id                             = filament_nozzle_map[filament_idx];
        const NozzleInfo &nozzle                                = nozzle_list[nozzle_id];
        int               extruder_id                           = nozzle.extruder_id;
        extruder_to_filament_nozzles[extruder_id][filament_idx] = nozzle;
        filament_to_nozzle[filament_idx] = nozzle;
        filament_map[filament_idx] = extruder_id;
    }
}

std::optional<MultiNozzleGroupResult> MultiNozzleGroupResult::init_from_slice_filament(const std::vector<int>& filament_map, const std::vector<FilamentInfo>& filament_info)
{
    if (filament_map.empty())
        return std::nullopt;

    std::map<int, NozzleInfo> nozzle_list_map;
    std::vector<int> filament_nozzle_map = filament_map;
    std::map<int, std::vector<int>> group_ids_in_extruder;
    std::vector<unsigned int> used_filaments;

    auto volume_type_str_to_enum = ConfigOptionEnum<NozzleVolumeType>::get_enum_values();

    // used filaments
    for (size_t idx = 0; idx < filament_info.size(); ++idx) {
        int         nozzle_idx   = filament_info[idx].group_id.empty() ? -1 : filament_info[idx].group_id.front();
        int         filament_idx = filament_info[idx].id;
        int         extruder_idx = filament_map[filament_idx] - 1; // 0 based idx
        double      diameter     = filament_info[idx].nozzle_diameter;
        std::string volume_type  = filament_info[idx].nozzle_volume_type;
        if (nozzle_idx == -1) return std::nullopt; // Nozzle group id is not set, return empty optional

        used_filaments.emplace_back(filament_idx);
        group_ids_in_extruder[extruder_idx].emplace_back(nozzle_idx);

        NozzleInfo nozzle;
        nozzle.diameter    = format_diameter_to_str(diameter);
        nozzle.group_id    = nozzle_idx;
        nozzle.extruder_id = extruder_idx;
        nozzle.volume_type = NozzleVolumeType(volume_type_str_to_enum[volume_type]);
        if (!nozzle_list_map.count(nozzle_idx)) nozzle_list_map[nozzle_idx] = nozzle;

        filament_nozzle_map[filament_idx] = nozzle_idx;
    }

    // handle unused filaments to build some unused nozzles
    for (size_t idx = 0; idx < filament_map.size(); ++idx) {
        auto iter = std::find_if(filament_info.begin(), filament_info.end(), [idx](const FilamentInfo& info) {
            return info.id == static_cast<int>(idx);
            });
        if (iter != filament_info.end())
            continue;

        int extruder_idx = filament_map[idx] - 1;
        if (group_ids_in_extruder.count(extruder_idx)) {
            // reuse one nozzle in current extruder
            filament_nozzle_map[idx] = group_ids_in_extruder[extruder_idx].front();
        }
        else {
            // create a new nozzle
            int max_nozzle_idx = 0;
            for (auto& nozzle_groups : group_ids_in_extruder) {
                for (auto group_id : nozzle_groups.second)
                    max_nozzle_idx = std::max(max_nozzle_idx, group_id);
            }

            NozzleInfo nozzle;
            nozzle.diameter = nozzle_list_map[max_nozzle_idx].diameter;
            nozzle.volume_type = nozzle_list_map[max_nozzle_idx].volume_type;
            nozzle.group_id = max_nozzle_idx + 1;
            nozzle.extruder_id = extruder_idx;

            nozzle_list_map[max_nozzle_idx + 1] = nozzle;

            filament_nozzle_map[idx] = max_nozzle_idx + 1;
        }

    }


    std::vector<int> new_filament_nozzle_map = filament_nozzle_map;
    std::vector<NozzleInfo>   nozzle_list_vec;

    // reset group id for nozzles
    for (auto &elem : nozzle_list_map) {
        int  nozzle_id       = elem.first;
        auto nozzle_info     = elem.second;
        nozzle_info.group_id = nozzle_list_vec.size();
        nozzle_list_vec.emplace_back(nozzle_info);
        for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
            if (filament_nozzle_map[idx] == nozzle_id) new_filament_nozzle_map[idx] = nozzle_list_vec.size() - 1;
        }
    }

    if(new_filament_nozzle_map.empty() || nozzle_list_vec.empty())
        return std::nullopt;

    return MultiNozzleGroupResult(new_filament_nozzle_map, nozzle_list_vec, used_filaments);
}

std::optional<MultiNozzleGroupResult> MultiNozzleGroupResult::init_from_cli_config(const std::vector<unsigned int>& used_filaments,const std::vector<int>& filament_map, const std::vector<int>& filament_volume_map, const std::vector<int>& filament_nozzle_map, const std::vector<std::map<NozzleVolumeType, int>>& nozzle_count, float diameter)
{
    std::vector<MultiNozzleUtils::NozzleGroupInfo> nozzle_groups;
    for(size_t extruder_id = 0; extruder_id < nozzle_count.size(); ++extruder_id){
        for (auto elem : nozzle_count[extruder_id]) {
            MultiNozzleUtils::NozzleGroupInfo group_info;
            group_info.diameter = format_diameter_to_str(diameter);
            group_info.volume_type = elem.first;
            group_info.nozzle_count = elem.second;
            group_info.extruder_id = extruder_id;
            nozzle_groups.emplace_back(group_info);
        }
    }

    auto nozzle_list = build_nozzle_list(nozzle_groups);
    std::vector<bool> used_nozzle(nozzle_list.size(),false);

    std::map<int,int> input_nozzle_id_to_output;

    std::vector<int> output_nozzle_map(filament_nozzle_map.size(),0);

    for(auto filament_idx : used_filaments){
        NozzleVolumeType req_type = NozzleVolumeType(filament_volume_map[filament_idx]);
        int req_extruder = filament_map[filament_idx];
        int input_nozzle_idx = filament_nozzle_map[filament_idx];

        if(input_nozzle_id_to_output.find(input_nozzle_idx) != input_nozzle_id_to_output.end()){
            output_nozzle_map[filament_idx] = input_nozzle_id_to_output[input_nozzle_idx];
            continue;
        }

        int output_nozzle_idx = -1;
        for(size_t nozzle_idx = 0; nozzle_idx < nozzle_list.size(); ++nozzle_idx){
            if(used_nozzle[nozzle_idx])
                continue;
            auto& nozzle_info = nozzle_list[nozzle_idx];
            if(!(nozzle_info.extruder_id == req_extruder&& nozzle_info.volume_type == req_type))
                continue;

            output_nozzle_idx = nozzle_idx;
            input_nozzle_id_to_output[input_nozzle_idx] = output_nozzle_idx;
            used_nozzle[nozzle_idx] = true;
            break;
        }

        if(output_nozzle_idx == -1){
            return std::nullopt;
        }
        output_nozzle_map[filament_idx] = output_nozzle_idx;
    }

    return MultiNozzleGroupResult(output_nozzle_map,nozzle_list,used_filaments);
}

int MultiNozzleGroupResult::get_extruder_id(int filament_id) const
{
    for (auto &elem : extruder_to_filament_nozzles) {
        auto &filament_to_nozzle = elem.second;
        int   extruder_id        = elem.first;
        if (filament_to_nozzle.find(filament_id) == filament_to_nozzle.end()) continue;
        return extruder_id;
    }
    return -1;
}

std::optional<NozzleInfo> MultiNozzleGroupResult::get_nozzle_for_filament(int filament_id) const
{
    for (auto &elem : extruder_to_filament_nozzles) {
        auto &filament_to_nozzle = elem.second;
        int   extruder_id        = elem.first;
        auto  iter               = filament_to_nozzle.find(filament_id);
        if (iter == filament_to_nozzle.end()) continue;
        return iter->second;
    }
    return std::nullopt;
}

bool MultiNozzleGroupResult::are_filaments_same_extruder(int filament_id1, int filament_id2) const
{
    int extruder_id1 = get_extruder_id(filament_id1);
    int extruder_id2 = get_extruder_id(filament_id2);

    if (extruder_id1 == -1 || extruder_id2 == -1) return false;

    return extruder_id1 == extruder_id2;
}

bool MultiNozzleGroupResult::are_filaments_same_nozzle(int filament_id1, int filament_id2) const
{
    std::optional<NozzleInfo> nozzle_info1 = get_nozzle_for_filament(filament_id1);
    std::optional<NozzleInfo> nozzle_info2 = get_nozzle_for_filament(filament_id2);
    if (!nozzle_info1 || !nozzle_info2) return false;

    return nozzle_info1->group_id == nozzle_info2->group_id;
}

int MultiNozzleGroupResult::get_extruder_count() const { return static_cast<int>(extruder_to_filament_nozzles.size()); }

int MultiNozzleGroupResult::get_nozzle_count(int target_extruder_id) const
{
    std::set<int> nozzles;
    for (auto &elem : extruder_to_filament_nozzles) {
        auto &filament_to_nozzle = elem.second;
        int   extruder_id        = elem.first;

        if (target_extruder_id == -1 || extruder_id == target_extruder_id) {
            for (auto &filament_nozzle : filament_to_nozzle) { nozzles.insert(filament_nozzle.second.group_id); }
        }
    }
    return static_cast<int>(nozzles.size());
}

std::vector<NozzleInfo>  MultiNozzleGroupResult::get_nozzle_vec(int target_extruder_id) const
{
    std::set<int> nozzles;
    std::vector<NozzleInfo> nozzleinfo_vec;
    for (auto& elem : extruder_to_filament_nozzles) {
        auto& filament_to_nozzle = elem.second;
        int   extruder_id = elem.first;

        if (target_extruder_id == -1 || extruder_id == target_extruder_id) {
            for (auto& filament_nozzle : filament_to_nozzle) {
                int filament_id = filament_nozzle.first;
                if(std::find(used_filaments.begin(), used_filaments.end(), filament_id) == used_filaments.end())
                    continue;
                if (nozzles.count(filament_nozzle.second.group_id) == 0) {
                    nozzles.insert(filament_nozzle.second.group_id);
                    nozzleinfo_vec.push_back(filament_nozzle.second);
                }
            }
        }
    }
    return nozzleinfo_vec;
}

std::vector<NozzleInfo> MultiNozzleGroupResult::get_used_nozzles(const std::vector<unsigned int> &filament_list, int target_extruder_id) const
{
    std::vector<NozzleInfo> result;
    for (auto filament : filament_list) {
        int filament_idx = static_cast<int>(filament);
        if (filament_to_nozzle[filament_idx].extruder_id == target_extruder_id) {
            result.emplace_back(filament_to_nozzle[filament_idx]);
        }
    }
    return result;
}

std::pair<int, int> MultiNozzleGroupResult::get_used_extruders_nozzles_count(const std::vector<unsigned int> &filament_list) const
{
    std::pair<int, int> result;
    std::vector<int> mask_extruder(64,0);
    std::vector<int> mask_nozzle(64, 0);
    int extruder_count = 0, nozzle_count = 0;
    for (auto filament : filament_list) {
        int filament_idx = static_cast<int>(filament);
        auto &nozzle = filament_to_nozzle[filament_idx];

        extruder_count += (mask_extruder[nozzle.extruder_id] == 0);
        nozzle_count += (mask_nozzle[nozzle.group_id] == 0);
        mask_extruder[nozzle.extruder_id] = 1;
        mask_nozzle[nozzle.group_id] = 1;
    }
    return {extruder_count, nozzle_count};
}

std::vector<int> MultiNozzleGroupResult::get_used_extruders(const std::vector<unsigned int> &filament_list) const
{
    std::set<int> used_extruders;
    for (auto filament : filament_list) {
        int filament_idx = static_cast<int>(filament);
        int extruder_id  = get_extruder_id(filament_idx);
        if (extruder_id == -1) continue;
        used_extruders.insert(extruder_id);
        if (used_extruders.size() == extruder_to_filament_nozzles.size()) break;
    }
    return std::vector<int>(used_extruders.begin(), used_extruders.end());
}

std::vector<int> MultiNozzleGroupResult::get_extruder_list() const
{
    std::set<int> extruder_list;
    for (auto &elem : extruder_to_filament_nozzles) { extruder_list.insert(elem.first); }
    return std::vector<int>(extruder_list.begin(), extruder_list.end());
}

bool NozzleStatusRecorder::is_nozzle_empty(int nozzle_id) const
{
    auto iter = nozzle_filament_status.find(nozzle_id);
    if (iter == nozzle_filament_status.end()) return true;
    return false;
}

int NozzleStatusRecorder::get_filament_in_nozzle(int nozzle_id) const
{
    auto iter = nozzle_filament_status.find(nozzle_id);
    if (iter == nozzle_filament_status.end()) return -1;
    return iter->second;
}


int NozzleStatusRecorder::get_nozzle_in_extruder(int extruder_id) const
{
    auto iter = extruder_nozzle_status.find(extruder_id);
    if (iter == extruder_nozzle_status.end()) return -1;
    return iter->second;
}

void NozzleStatusRecorder::set_nozzle_status(int nozzle_id, int filament_id, int extruder_id)
{
    nozzle_filament_status[nozzle_id] = filament_id;
    if (extruder_id != -1) {
        extruder_nozzle_status[extruder_id] = nozzle_id;
    }
}

void NozzleStatusRecorder::clear_nozzle_status(int nozzle_id)
{
    auto iter = nozzle_filament_status.find(nozzle_id);
    if (iter == nozzle_filament_status.end()) return;
    nozzle_filament_status.erase(iter);
}

std::string NozzleGroupInfo::serialize() const
{
    std::ostringstream oss;
    oss << extruder_id << "-"
        << std::setprecision(2) << diameter << "-"
        << get_nozzle_volume_type_string(volume_type)<<"-"
        << nozzle_count;
    return oss.str();
}

std::optional<NozzleGroupInfo> NozzleGroupInfo::deserialize(const std::string &str)
{
    std::istringstream       iss(str);
    std::string              token;
    std::vector<std::string> tokens;

    while (std::getline(iss, token, '-')) { tokens.push_back(token); }

    if (tokens.size() != 4) { return std::nullopt; }

    try {
        int              extruder_id  = std::stoi(tokens[0]);
        std::string      diameter = tokens[1];
        NozzleVolumeType volume_type = NozzleVolumeType(ConfigOptionEnum<NozzleVolumeType>::get_enum_values().at(tokens[2]));
        int              nozzle_count = std::stoi(tokens[3]);

        return NozzleGroupInfo(diameter, volume_type, extruder_id, nozzle_count);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::vector<int> MultiNozzleGroupResult::get_extruder_map(bool zero_based) const
{
    if(zero_based)
        return filament_map;

    auto new_filament_map = filament_map;
    std::transform(new_filament_map.begin(), new_filament_map.end(), new_filament_map.begin(), [this](int val) { return val + 1;  });
    return new_filament_map;
}

std::vector<int> MultiNozzleGroupResult::get_nozzle_map() const
{
    std::vector<int> nozzle_map(filament_map.size());
    for (size_t idx = 0; idx < filament_to_nozzle.size(); ++idx)
        nozzle_map[idx] = filament_to_nozzle[idx].group_id;
    return nozzle_map;
}

std::vector<int> MultiNozzleGroupResult::get_volume_map() const
{
    std::vector<int> volume_map(filament_map.size());
    for (size_t idx = 0; idx < filament_to_nozzle.size(); ++idx)
        volume_map[idx] = filament_to_nozzle[idx].volume_type;
    return volume_map;
}


int MultiNozzleGroupResult::get_config_idx_for_filament(int filament_idx, const PrintConfig& config)
{
    if(auto iter=config_idx_map.find(&config); iter != config_idx_map.end()){
        return iter->second[filament_idx];
    }

    auto print_extruder_varint = config.printer_extruder_variant.values;
    auto print_extruder_id = config.printer_extruder_id.values;
    std::vector<ExtruderType> extruder_type_list;
    for (size_t idx = 0; idx < config.extruder_type.size(); ++idx)
        extruder_type_list.emplace_back(ExtruderType(config.extruder_type.values[idx]));

    std::vector<int> config_index_vec(filament_map.size());
    for(size_t idx  = 0; idx < config_index_vec.size(); ++idx){
        int extruder_id = filament_to_nozzle[idx].extruder_id;
        NozzleVolumeType volume_type = filament_to_nozzle[idx].volume_type;
        ExtruderType extruder_type = extruder_type_list[extruder_id];

        std::string variant = get_extruder_variant_string(extruder_type, volume_type);

        int target_index = 0;
        for (size_t j = 0; j < print_extruder_id.size(); ++j) {
            if (print_extruder_id[j] == extruder_id && variant == print_extruder_varint[j]) {
                target_index = j;
                break;
           }
        }

        config_index_vec[idx] = target_index;
    }
    config_idx_map[&config] = config_index_vec;
    return config_index_vec[filament_idx];

}

int MultiNozzleGroupResult::estimate_seq_flush_weight(const std::vector<std::vector<std::vector<float>>>& flush_matrix, const std::vector<int>& filament_change_seq) const
{
    auto get_weight_from_volume = [](float volume){
        return static_cast<int>(volume * 1.26 * 0.01);
    };

    float total_flush_volume = 0;
    MultiNozzleUtils::NozzleStatusRecorder recorder;
    for(auto filament: filament_change_seq){
        auto nozzle = get_nozzle_for_filament(filament);
        if(!nozzle)
            continue;

        int extruder_id = nozzle->extruder_id;
        int nozzle_id = nozzle->group_id;
        int last_filament = recorder.get_filament_in_nozzle(nozzle_id);

        if(last_filament!= -1 && last_filament != filament){
            float flush_volume = flush_matrix[extruder_id][last_filament][filament];
            total_flush_volume += flush_volume;
        }
        recorder.set_nozzle_status(nozzle_id, filament);
    }

    return get_weight_from_volume(total_flush_volume);
}



// ==================== load_nozzle_infos_with_compatibility ====================
std::vector<NozzleInfo> load_nozzle_infos_with_compatibility(
    const std::vector<NozzleInfo>& nozzle_infos,
    const std::vector<FilamentInfo>& filament_infos,
    const std::vector<int>& filament_map,
    const std::vector<NozzleVolumeType>& extruder_volume_types,
    const std::vector<double>& nozzle_diameter
)
{
    bool has_nozzle_info = !nozzle_infos.empty();
    // FilamentInfo::group_id is std::vector<int> — treat as valid if non-empty and first element >= 0.
    bool has_valid_filament_info = !filament_infos.empty() && std::all_of(filament_infos.begin(), filament_infos.end(), [](const FilamentInfo& info){
        return !info.group_id.empty() && info.group_id.front() >= 0;
    });

    if (!has_nozzle_info && !has_valid_filament_info) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": building nozzle list from filament map and volume types";

        const size_t extruder_count = nozzle_diameter.size();

        std::vector<NozzleVolumeType> volume_types_fixed = extruder_volume_types;
        volume_types_fixed.resize(extruder_count, NozzleVolumeType::nvtStandard);

        std::vector<NozzleInfo> result;
        result.reserve(extruder_count);
        for (size_t extruder_id = 0; extruder_id < extruder_count; ++extruder_id) {
            NozzleInfo info;
            info.diameter    = format_diameter_to_str(nozzle_diameter[extruder_id]);
            info.group_id    = static_cast<int>(extruder_id);
            info.extruder_id = static_cast<int>(extruder_id);
            info.volume_type = volume_types_fixed[extruder_id];
            result.emplace_back(std::move(info));
        }
        return result;
    }

    if (!has_nozzle_info) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": building nozzle list from filament info";
        std::map<int, NozzleInfo> nozzle_map;
        for (auto& filament : filament_infos) {
            int group_id = filament.group_id.empty() ? -1 : filament.group_id.front();
            if (group_id < 0 || nozzle_map.find(group_id) != nozzle_map.end()) {
                continue;
            }

            auto volume_type_str_to_enum = ConfigOptionEnum<NozzleVolumeType>::get_enum_values();

            NozzleInfo info;
            info.diameter = format_diameter_to_str(filament.nozzle_diameter);
            info.group_id = group_id;
            info.extruder_id = filament_map[filament.id] - 1;

            if (volume_type_str_to_enum.count(filament.nozzle_volume_type))
                info.volume_type = NozzleVolumeType(volume_type_str_to_enum.at(filament.nozzle_volume_type));
            else
                info.volume_type = NozzleVolumeType::nvtStandard;

            nozzle_map[group_id] = std::move(info);
        }

        std::vector<NozzleInfo> ret;
        for (auto& elem : nozzle_map) {
            ret.emplace_back(elem.second);
        }
        return ret;
    }

    auto result = nozzle_infos;
    std::sort(result.begin(), result.end());
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": using new 3mf format with " << result.size() << " nozzle infos.";
    return result;
}

// ==================== LayeredNozzleGroupResult implementation ====================
static bool has_filament_mapped_to_multiple_nozzles(const std::vector<std::vector<int>>& layer_filament_nozzle_maps,
                                                    const std::vector<unsigned int>&     used_filaments)
{
    if (layer_filament_nozzle_maps.empty() || used_filaments.empty())
        return false;

    for (auto filament_id_u : used_filaments) {
        int filament_id = static_cast<int>(filament_id_u);
        std::set<int> nozzle_ids;

        for (size_t layer_id = 0; layer_id < layer_filament_nozzle_maps.size(); ++layer_id) {
            const auto& map = layer_filament_nozzle_maps[layer_id];
            if (filament_id < 0 || filament_id >= static_cast<int>(map.size()))
                continue;

            int nozzle_id = map[filament_id];
            if (nozzle_id < 0)
                continue;

            nozzle_ids.insert(nozzle_id);
            if (nozzle_ids.size() > 1)
                return true;
        }
    }

    return false;
}

std::optional<LayeredNozzleGroupResult> LayeredNozzleGroupResult::create(
    const std::vector<int>& filament_nozzle_map,
    const std::vector<NozzleInfo>& nozzle_list,
    const std::vector<unsigned int>& used_filaments)
{
    if (filament_nozzle_map.empty() || nozzle_list.empty()) {
        return std::nullopt;
    }

    LayeredNozzleGroupResult result(false);
    result._default_filament_nozzle_map = filament_nozzle_map;
    result._nozzle_list = nozzle_list;
    result._used_filaments = used_filaments;

    return result;
}

std::optional<LayeredNozzleGroupResult> LayeredNozzleGroupResult::create(
    const std::vector<std::vector<int>>&          layer_filament_nozzle_maps,
    const std::vector<NozzleInfo>&                nozzle_list,
    const std::vector<unsigned int>&              used_filaments,
    const std::vector<std::vector<unsigned int>>& layer_filament_sequences)
{
    if (layer_filament_nozzle_maps.empty() || nozzle_list.empty()) {
        return std::nullopt;
    }

    bool support_dynamic_nozzle_map = has_filament_mapped_to_multiple_nozzles(layer_filament_nozzle_maps, used_filaments);
    LayeredNozzleGroupResult result(support_dynamic_nozzle_map);
    result._layer_filament_nozzle_maps = layer_filament_nozzle_maps;
    result._layer_filament_sequences   = layer_filament_sequences;
    result._nozzle_list                = nozzle_list;
    result._used_filaments             = used_filaments;

    if (!layer_filament_nozzle_maps.empty()) {
        result._default_filament_nozzle_map = layer_filament_nozzle_maps[0];
    }

    return result;
}

std::optional<LayeredNozzleGroupResult> LayeredNozzleGroupResult::create(
    const std::vector<unsigned int>&    used_filaments,
    const std::vector<int>&             filament_map,
    const std::vector<int>&             filament_volume_map,
    const std::vector<int>&             filament_nozzle_map,
    const std::vector<std::map<NozzleVolumeType, int>>& nozzle_count,
    float                               diameter)
{
    std::vector<NozzleGroupInfo> nozzle_groups;
    for (size_t extruder_id = 0; extruder_id < nozzle_count.size(); ++extruder_id) {
        for (auto elem : nozzle_count[extruder_id]) {
            NozzleGroupInfo group_info;
            group_info.diameter     = format_diameter_to_str(diameter);
            group_info.volume_type  = elem.first;
            group_info.nozzle_count = elem.second;
            group_info.extruder_id  = static_cast<int>(extruder_id);
            nozzle_groups.emplace_back(group_info);
        }
    }

    auto               nozzle_list = build_nozzle_list(nozzle_groups);
    std::vector<bool>  used_nozzle(nozzle_list.size(), false);
    std::map<int, int> input_nozzle_id_to_output;
    std::vector<int>   output_nozzle_map(filament_nozzle_map.size(), 0);

    for (auto filament_idx : used_filaments) {
        NozzleVolumeType req_type         = NozzleVolumeType(filament_volume_map[filament_idx]);
        int              req_extruder     = filament_map[filament_idx];
        int              input_nozzle_idx = filament_nozzle_map[filament_idx];

        if (input_nozzle_id_to_output.find(input_nozzle_idx) != input_nozzle_id_to_output.end()) {
            output_nozzle_map[filament_idx] = input_nozzle_id_to_output[input_nozzle_idx];
            continue;
        }

        int output_nozzle_idx = -1;
        for (size_t nozzle_idx = 0; nozzle_idx < nozzle_list.size(); ++nozzle_idx) {
            if (used_nozzle[nozzle_idx]) continue;

            auto& nozzle_info = nozzle_list[nozzle_idx];
            if (!(nozzle_info.extruder_id == req_extruder && nozzle_info.volume_type == req_type)) continue;

            output_nozzle_idx                           = static_cast<int>(nozzle_idx);
            input_nozzle_id_to_output[input_nozzle_idx] = output_nozzle_idx;
            used_nozzle[nozzle_idx]                     = true;
            break;
        }

        if (output_nozzle_idx == -1) { return std::nullopt; }
        output_nozzle_map[filament_idx] = output_nozzle_idx;
    }

    return create(output_nozzle_map, nozzle_list, used_filaments);
}

bool LayeredNozzleGroupResult::are_filaments_same_extruder(int filament_id1, int filament_id2, int layer_id) const
{
    std::optional<NozzleInfo> nozzle_info1 = get_nozzle_for_filament(filament_id1, layer_id);
    std::optional<NozzleInfo> nozzle_info2 = get_nozzle_for_filament(filament_id2, layer_id);

    if (!nozzle_info1 || !nozzle_info2) return false;

    return nozzle_info1->extruder_id == nozzle_info2->extruder_id;
}

bool LayeredNozzleGroupResult::are_filaments_same_nozzle(int filament_id1, int filament_id2, int layer_id) const
{
    std::optional<NozzleInfo> nozzle_info1 = get_nozzle_for_filament(filament_id1, layer_id);
    std::optional<NozzleInfo> nozzle_info2 = get_nozzle_for_filament(filament_id2, layer_id);
    if (!nozzle_info1 || !nozzle_info2) return false;

    return nozzle_info1->group_id == nozzle_info2->group_id;
}

int LayeredNozzleGroupResult::get_extruder_count() const
{
    std::set<int> extruder_ids;
    for (const auto& nozzle : _nozzle_list) { extruder_ids.insert(nozzle.extruder_id); }
    return static_cast<int>(extruder_ids.size());
}

std::vector<NozzleInfo> LayeredNozzleGroupResult::get_used_nozzles_in_extruder(int target_extruder_id) const
{
    return get_used_nozzles_in_extruder(target_extruder_id, -1);
}

std::vector<NozzleInfo> LayeredNozzleGroupResult::get_used_nozzles_in_extruder(int target_extruder_id, int layer_id) const
{
    std::set<int>           nozzle_ids;
    std::vector<NozzleInfo> result;

    std::vector<unsigned int> target_filaments = get_used_filaments(layer_id);

    for (unsigned int filament_id : target_filaments) {
        if (layer_id != -1) {
            auto nozzle_opt = get_nozzle_for_filament(static_cast<int>(filament_id), layer_id);
            if (nozzle_opt) {
                if (target_extruder_id == -1 || nozzle_opt->extruder_id == target_extruder_id) { nozzle_ids.insert(nozzle_opt->group_id); }
            }
        } else {
            auto nozzles = get_nozzles_for_filament(static_cast<int>(filament_id));
            for (const auto& nozzle : nozzles) {
                if (target_extruder_id == -1 || nozzle.extruder_id == target_extruder_id) { nozzle_ids.insert(nozzle.group_id); }
            }
        }
    }
    for (int nozzle_id : nozzle_ids) {
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) { result.push_back(_nozzle_list[nozzle_id]); }
    }
    return result;
}

std::vector<int> LayeredNozzleGroupResult::get_used_extruders() const
{
    return get_used_extruders(-1);
}

std::vector<int> LayeredNozzleGroupResult::get_used_extruders(int layer_id) const
{
    std::set<int> used_extruders;
    std::vector<unsigned int> target_filaments = get_used_filaments(layer_id);
    for (auto filament_id : target_filaments) {
        if (layer_id != -1) {
            auto nozzle_opt = get_nozzle_for_filament(static_cast<int>(filament_id), layer_id);
            if (nozzle_opt) { used_extruders.insert(nozzle_opt->extruder_id); }
        } else {
            auto nozzles = get_nozzles_for_filament(static_cast<int>(filament_id));
            for (const auto& nozzle : nozzles) { used_extruders.insert(nozzle.extruder_id); }
        }
    }
    return std::vector<int>(used_extruders.begin(), used_extruders.end());
}

std::vector<int> LayeredNozzleGroupResult::get_extruder_map(bool zero_based, int layer_id) const
{
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    std::vector<int>        extruder_map(filament_nozzle_map.size());
    for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
        int nozzle_id = filament_nozzle_map[idx];
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            extruder_map[idx] = _nozzle_list[nozzle_id].extruder_id;
        } else {
            extruder_map[idx] = -1;
        }
    }

    if (zero_based) return extruder_map;

    auto new_filament_map = extruder_map;
    std::transform(new_filament_map.begin(), new_filament_map.end(), new_filament_map.begin(), [](int val) { return val + 1; });
    return new_filament_map;
}

std::vector<int> LayeredNozzleGroupResult::get_nozzle_map(int layer_id) const
{
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    std::vector<int>        nozzle_map(filament_nozzle_map.size());
    for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
        int nozzle_id = filament_nozzle_map[idx];
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            nozzle_map[idx] = _nozzle_list[nozzle_id].group_id;
        } else {
            nozzle_map[idx] = -1;
        }
    }
    return nozzle_map;
}

std::vector<int> LayeredNozzleGroupResult::get_volume_map(int layer_id) const
{
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    std::vector<int>        volume_map(filament_nozzle_map.size());
    for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
        int nozzle_id = filament_nozzle_map[idx];
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            volume_map[idx] = _nozzle_list[nozzle_id].volume_type;
        } else {
            volume_map[idx] = -1;
        }
    }
    return volume_map;
}

std::vector<unsigned int> LayeredNozzleGroupResult::get_used_filaments(int layer_id) const
{
    if (layer_id < 0) { return _used_filaments; }
    if (layer_id >= static_cast<int>(_layer_filament_nozzle_maps.size())) { return _used_filaments; }

    if (!_layer_filament_sequences.empty() && layer_id < static_cast<int>(_layer_filament_sequences.size())) {
        return _layer_filament_sequences[layer_id];
    }
    return {};
}

std::optional<NozzleInfo> LayeredNozzleGroupResult::get_nozzle_for_filament(int filament_id, int layer_id) const
{
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);

    if (filament_id < 0 || filament_id >= static_cast<int>(filament_nozzle_map.size())) { return std::nullopt; }

    int nozzle_id = filament_nozzle_map[filament_id];
    return get_nozzle_from_id(nozzle_id);
}

std::vector<NozzleInfo> LayeredNozzleGroupResult::get_nozzles_for_filament(int filament_id) const
{
    std::set<int> nozzle_ids;

    if (!support_dynamic_nozzle_map) {
        if (filament_id >= 0 && filament_id < static_cast<int>(_default_filament_nozzle_map.size())) {
            nozzle_ids.insert(_default_filament_nozzle_map[filament_id]);
        }
    } else {
        int start_layer = 0;
        int end_layer   = static_cast<int>(_layer_filament_nozzle_maps.size());

        for (int i = start_layer; i < end_layer; ++i) {
            const auto& map = _layer_filament_nozzle_maps[i];
            if (filament_id >= 0 && filament_id < static_cast<int>(map.size())) {
                nozzle_ids.insert(map[filament_id]);
            }
        }
    }

    std::vector<NozzleInfo> result;
    for (int id : nozzle_ids) {
        if (id >= 0 && id < static_cast<int>(_nozzle_list.size())) { result.push_back(_nozzle_list[id]); }
    }
    return result;
}

std::optional<NozzleInfo> LayeredNozzleGroupResult::get_first_nozzle_for_filament(int filament_id) const
{
    if (filament_id < 0) return std::nullopt;

    if (!support_dynamic_nozzle_map) {
        if (filament_id >= static_cast<int>(_default_filament_nozzle_map.size())) return std::nullopt;
        return get_nozzle_from_id(_default_filament_nozzle_map[filament_id]);
    }

    for (size_t layer = 0; layer < _layer_filament_nozzle_maps.size(); ++layer) {
        auto layer_used_filaments = get_used_filaments(layer);
        if (std::find(layer_used_filaments.begin(), layer_used_filaments.end(), static_cast<unsigned int>(filament_id)) == layer_used_filaments.end()) {
            continue;
        }
        const auto& map = _layer_filament_nozzle_maps[layer];
        if (filament_id >= 0 && filament_id < static_cast<int>(map.size())) {
            int nozzle_id = map[filament_id];
            auto nozzle = get_nozzle_from_id(nozzle_id);
            if (nozzle) return nozzle;
        }
    }

    return std::nullopt;
}

std::optional<NozzleInfo> LayeredNozzleGroupResult::get_nozzle_from_id(int nozzle_id) const
{
    if (nozzle_id < 0 || nozzle_id >= static_cast<int>(_nozzle_list.size())) { return std::nullopt; }
    return _nozzle_list[nozzle_id];
}

int LayeredNozzleGroupResult::get_extruder_id(int filament_id, int layer_id) const
{
    auto nozzle_info = get_nozzle_for_filament(filament_id, layer_id);
    return nozzle_info ? nozzle_info->extruder_id : -1;
}

int LayeredNozzleGroupResult::get_nozzle_id(int filament_id, int layer_id) const
{
    auto nozzle_info = get_nozzle_for_filament(filament_id, layer_id);
    return nozzle_info ? nozzle_info->group_id : -1;
}

const std::vector<int>& LayeredNozzleGroupResult::get_layer_filament_nozzle_map(int layer_id) const
{
    if (layer_id >= 0 && layer_id < static_cast<int>(_layer_filament_nozzle_maps.size())) { return _layer_filament_nozzle_maps[layer_id]; }
    return _default_filament_nozzle_map;
}

int LayeredNozzleGroupResult::estimate_seq_flush_weight(const std::vector<std::vector<std::vector<float>>>& flush_matrix, const std::vector<int>& filament_change_seq) const
{
    auto get_weight_from_volume = [](float volume) {
        return static_cast<int>(volume * 1.26 * 0.01);
    };

    float total_flush_volume = 0;
    NozzleStatusRecorder recorder;
    for (auto filament : filament_change_seq) {
        auto nozzle = get_nozzle_for_filament(filament, -1);
        if (!nozzle) continue;

        int extruder_id = nozzle->extruder_id;
        int nozzle_id = nozzle->group_id;
        int last_filament = recorder.get_filament_in_nozzle(nozzle_id);

        if (last_filament != -1 && last_filament != filament) {
            if (extruder_id >= 0 && extruder_id < static_cast<int>(flush_matrix.size()) &&
                last_filament >= 0 && last_filament < static_cast<int>(flush_matrix[extruder_id].size()) &&
                filament >= 0 && filament < static_cast<int>(flush_matrix[extruder_id][last_filament].size())) {
                float flush_volume = flush_matrix[extruder_id][last_filament][filament];
                total_flush_volume += flush_volume;
            }
        }
        recorder.set_nozzle_status(nozzle_id, filament);
    }

    return get_weight_from_volume(total_flush_volume);
}

// ==================== StaticNozzleGroupResult implementation ====================

std::optional<StaticNozzleGroupResult> StaticNozzleGroupResult::create(
    const std::vector<FilamentInfo>& filaments_info,
    const std::vector<NozzleInfo>&   nozzles_info,
    const std::vector<int>&          filament_change_seq,
    const std::vector<int>&          nozzle_change_seq,
    bool support_dynamic_nozzle_map)
{
    if (filaments_info.empty() || nozzles_info.empty()) return std::nullopt;

    std::map<int, NozzleInfo>    nozzle_list_map;
    std::map<int, std::set<int>> filament_to_nozzles;

    for (auto nozzle_info : nozzles_info)
        nozzle_list_map[nozzle_info.group_id] = nozzle_info;

    for (auto filament_info : filaments_info) {
        auto fil_id = filament_info.id;
        // FilamentInfo::group_id is std::vector<int> — insert all valid (>= 0) nozzle ids.
        std::set<int> nozzles_set;
        for (int gid : filament_info.group_id) {
            if (gid >= 0)
                nozzles_set.insert(gid);
        }
        // Compat for old (single-nozzle) gcode.3mf: filament has no recorded group_id,
        // bind it to all available nozzles so preview colors resolve again (BBL 51583b79d).
        if (nozzles_set.empty()) {
            for (const auto& nozzle_entry : nozzle_list_map)
                nozzles_set.insert(nozzle_entry.first);
        }
        filament_to_nozzles[fil_id] = nozzles_set;
    }

    StaticNozzleGroupResult result(support_dynamic_nozzle_map);
    result._filament_to_nozzles = filament_to_nozzles;
    result._nozzle_list_map     = nozzle_list_map;
    result._filament_change_seq = filament_change_seq;
    result._nozzle_change_seq   = nozzle_change_seq;

    return result;
}

std::optional<NozzleInfo> StaticNozzleGroupResult::get_nozzle_from_id(int nozzle_id) const
{
    auto iter = _nozzle_list_map.find(nozzle_id);
    if (iter == _nozzle_list_map.end()) { return std::nullopt; }
    return iter->second;
}

int StaticNozzleGroupResult::get_extruder_count() const
{
    std::set<int> extruder_ids;
    for (const auto& elem : _nozzle_list_map) { extruder_ids.insert(elem.second.extruder_id); }
    return static_cast<int>(extruder_ids.size());
}

std::vector<NozzleInfo> StaticNozzleGroupResult::get_used_nozzles_in_extruder(int target_extruder_id) const
{
    std::vector<NozzleInfo> result;
    for (const auto& elem : _nozzle_list_map) {
        const auto& nozzle = elem.second;
        if (target_extruder_id == -1 || nozzle.extruder_id == target_extruder_id) {
            result.push_back(nozzle);
        }
    }
    return result;
}

std::vector<int> StaticNozzleGroupResult::get_used_extruders() const
{
    std::set<int> used_extruders;
    for (const auto& elem : _nozzle_list_map) { used_extruders.insert(elem.second.extruder_id); }
    return std::vector<int>(used_extruders.begin(), used_extruders.end());
}

std::vector<unsigned int> StaticNozzleGroupResult::get_used_filaments() const
{
    std::vector<unsigned int> used_filaments;
    used_filaments.reserve(_filament_to_nozzles.size());
    for (const auto& elem : _filament_to_nozzles) {
        if (elem.first >= 0) {
            used_filaments.push_back(static_cast<unsigned int>(elem.first));
        }
    }
    return used_filaments;
}

std::vector<NozzleInfo> StaticNozzleGroupResult::get_nozzles_for_filament(int filament_id) const
{
    auto iter = _filament_to_nozzles.find(filament_id);
    if (iter == _filament_to_nozzles.end()) { return std::vector<NozzleInfo>(); }

    std::vector<NozzleInfo> result;
    for (int nozzle_id : iter->second) {
        auto nozzle_iter = _nozzle_list_map.find(nozzle_id);
        if (nozzle_iter != _nozzle_list_map.end()) {
            result.push_back(nozzle_iter->second);
        }
    }
    return result;
}

std::optional<NozzleInfo> StaticNozzleGroupResult::get_first_nozzle_for_filament(int filament_id) const
{
    if (filament_id < 0) return std::nullopt;

    if (!_filament_change_seq.empty() && _filament_change_seq.size() == _nozzle_change_seq.size()) {
        for (size_t idx = 0; idx < _filament_change_seq.size(); ++idx) {
            if (_filament_change_seq[idx] == filament_id) {
                int nozzle_id = _nozzle_change_seq[idx];
                auto nozzle = get_nozzle_from_id(nozzle_id);
                if (nozzle) return nozzle;
            }
        }
    }

    auto iter = _filament_to_nozzles.find(filament_id);
    if (iter == _filament_to_nozzles.end()) return std::nullopt;

    for (int nozzle_id : iter->second) {
        auto nozzle = get_nozzle_from_id(nozzle_id);
        if (nozzle) return nozzle;
    }

    return std::nullopt;
}

// ==================== Free functions: filament-change cost ====================

// H2C port (BBL 2f014ce1a): core simulation; calc_filament_change_gap_for_assignment
// is now a thin wrapper over this.
FilamentChangeSimResult simulate_filament_change_time(
    const std::vector<int>&         logical_filaments,
    const std::vector<NozzleInfo>&  nozzle_list,
    const std::vector<int>&         filament_change_seq,
    const std::vector<int>&         nozzle_change_seq,
    const std::vector<int>&         group_of_filament,
    const FilamentChangeTimeParams& time_params,
    const std::vector<bool>&        ams_preload_enabled,
    bool                            calc_sliced_time)
{
    FilamentChangeSimResult result;
    if (logical_filaments.empty() || nozzle_list.empty() || filament_change_seq.empty() || nozzle_change_seq.empty())
        return result;

    const float load_ams_to_selector   = time_params.standard_load_time   - time_params.selector_load_time;
    const float unload_ams_to_selector = time_params.standard_unload_time - time_params.selector_unload_time;
    const float load_selector_to_ext   = time_params.selector_load_time;
    const float unload_ext_to_selector = time_params.selector_unload_time;

    std::unordered_map<int, int> nozzle_to_extruder;
    nozzle_to_extruder.reserve(nozzle_list.size());
    for (const auto& nozzle : nozzle_list)
        nozzle_to_extruder[nozzle.group_id] = nozzle.extruder_id;

    std::unordered_map<int, int> filament_to_group;
    filament_to_group.reserve(logical_filaments.size());
    for (size_t i = 0; i < logical_filaments.size(); ++i)
        filament_to_group[logical_filaments[i]] = group_of_filament[i];

    const auto get_group = [&](int filament_id) -> int {
        auto it = filament_to_group.find(filament_id);
        return it != filament_to_group.end() ? it->second : -1;
    };

    const auto is_preload_enabled = [&](int group_id) -> bool {
        if (group_id < 0 || group_id >= static_cast<int>(ams_preload_enabled.size()))
            return false;
        return ams_preload_enabled[group_id];
    };

    enum class Location { IN_AMS, IN_SELECTOR, IN_EXTRUDER };
    std::unordered_map<int, Location> filament_location;
    std::unordered_map<int, int>      filament_extruder;
    std::unordered_map<int, int>      extruder_filament;
    std::unordered_map<int, std::unordered_set<int>> ams_group_occupied;

    filament_location.reserve(logical_filaments.size());
    filament_extruder.reserve(logical_filaments.size());

    for (int f : logical_filaments)
        filament_location[f] = Location::IN_AMS;

    NozzleStatusRecorder sliced_recorder;

    const size_t seq_len = std::min(filament_change_seq.size(), nozzle_change_seq.size());
    double actual_time = 0.0;
    double sliced_time = 0.0;

    for (size_t i = 0; i < seq_len; ++i) {
        int B         = filament_change_seq[i];
        int nozzle_id = nozzle_change_seq[i];

        auto nozzle_iter = nozzle_to_extruder.find(nozzle_id);
        if (nozzle_iter == nozzle_to_extruder.end()) continue;

        int E = nozzle_iter->second;

        if (calc_sliced_time) {
            int old_nozzle_in_E        = sliced_recorder.get_nozzle_in_extruder(E);
            int old_filament_in_nozzle = sliced_recorder.get_filament_in_nozzle(nozzle_id);
            int old_filament_in_ext    = sliced_recorder.get_filament_in_nozzle(old_nozzle_in_E);

            bool nozzle_change   = (old_nozzle_in_E != nozzle_id);
            bool filament_change = (old_filament_in_nozzle != B);

            if (nozzle_change || filament_change) {
                if (old_filament_in_ext != -1)
                    sliced_time += time_params.standard_unload_time;
                sliced_time += time_params.standard_load_time;
            }
            sliced_recorder.set_nozzle_status(nozzle_id, B, E);
        }

        int A = -1;
        {
            auto it = extruder_filament.find(E);
            if (it != extruder_filament.end())
                A = it->second;
        }

        int group_B = get_group(B);
        int group_A = (A != -1) ? get_group(A) : -1;

        auto group_it = ams_group_occupied.find(group_B);
        if (group_it != ams_group_occupied.end()) {
            for (int X : group_it->second) {
                if (X == B) continue;
                Location loc_X = filament_location[X];
                if (loc_X == Location::IN_EXTRUDER) {
                    actual_time += unload_ext_to_selector + unload_ams_to_selector;
                    int E2 = filament_extruder[X];
                    extruder_filament.erase(E2);
                    filament_extruder.erase(X);
                } else if (loc_X == Location::IN_SELECTOR) {
                    actual_time += unload_ams_to_selector;
                }
                filament_location[X] = Location::IN_AMS;
            }
            group_it->second.clear();
        }

        bool step3_executed = false;
        float step3_time = 0.0f;
        if (A != -1 && A != B && filament_location[A] == Location::IN_EXTRUDER) {
            if (is_preload_enabled(group_A) && group_A != group_B) {
                step3_time = unload_ext_to_selector;
                filament_location[A] = Location::IN_SELECTOR;
            } else {
                step3_time = unload_ext_to_selector + unload_ams_to_selector;
                filament_location[A] = Location::IN_AMS;
                ams_group_occupied[group_A].erase(A);
            }
            extruder_filament.erase(E);
            filament_extruder.erase(A);
            step3_executed = true;
        }

        float step3_5_time = 0.0f;
        if (step3_executed &&
            filament_location[B] == Location::IN_AMS &&
            group_A != group_B &&
            is_preload_enabled(group_B)) {
            step3_5_time = load_ams_to_selector;
            filament_location[B] = Location::IN_SELECTOR;
            ams_group_occupied[group_B].insert(B);
        }

        actual_time += std::max(step3_time, step3_5_time);

        float step4_time = 0.0f;
        Location loc_B = filament_location[B];
        if (loc_B == Location::IN_AMS) {
            step4_time = load_ams_to_selector + load_selector_to_ext;
        } else if (loc_B == Location::IN_SELECTOR) {
            step4_time = load_selector_to_ext;
        }

        extruder_filament[E]  = B;
        filament_location[B]  = Location::IN_EXTRUDER;
        filament_extruder[B]  = E;
        ams_group_occupied[group_B].insert(B);

        float step6_time = 0.0f;
        if (i + 1 < seq_len) {
            int C = filament_change_seq[i + 1];
            int group_C = get_group(C);
            if (filament_location[C] == Location::IN_AMS &&
                group_C != group_B &&
                is_preload_enabled(group_C) &&
                ams_group_occupied[group_C].empty()) {
                step6_time = load_ams_to_selector;
                filament_location[C] = Location::IN_SELECTOR;
                ams_group_occupied[group_C].insert(C);
            }
        }

        actual_time += std::max(step4_time, step6_time);
    }

    result.actual_time = actual_time;
    result.sliced_time = sliced_time;
    return result;
}

float calc_filament_change_gap_for_assignment(
    const std::vector<int>&         logical_filaments,
    const std::vector<NozzleInfo>&  nozzle_list,
    const std::vector<int>&         filament_change_seq,
    const std::vector<int>&         nozzle_change_seq,
    const std::vector<int>&         group_of_filament,
    const FilamentChangeTimeParams& time_params,
    const std::vector<bool>&        ams_preload_enabled)
{
    auto r = simulate_filament_change_time(
        logical_filaments, nozzle_list, filament_change_seq,
        nozzle_change_seq, group_of_filament, time_params,
        ams_preload_enabled, /*calc_sliced_time=*/true);
    return static_cast<float>(r.actual_time - r.sliced_time);
}

std::vector<int> find_optimal_physical_assignment(
    const std::vector<int>&         logical_filaments,
    const std::vector<NozzleInfo>&  nozzle_list,
    const std::vector<int>&         filament_change_seq,
    const std::vector<int>&         nozzle_change_seq,
    int                             group_count,
    const FilamentChangeTimeParams& time_params,
    int                             max_ms)
{
    size_t count = logical_filaments.size();
    if (count == 0 || group_count <= 0) return {};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);

    std::vector<int> assignment(count, 0);
    std::vector<int> best_assignment = assignment;
    float best_gap = calc_filament_change_gap_for_assignment(logical_filaments, nozzle_list, filament_change_seq, nozzle_change_seq, assignment, time_params);

    bool done = false;
    bool timed_out = false;
    while (!done) {
        if (assignment[0] == 0) {
            float gap = calc_filament_change_gap_for_assignment(logical_filaments, nozzle_list, filament_change_seq, nozzle_change_seq, assignment, time_params);
            if (gap < best_gap) {
                best_gap = gap;
                best_assignment = assignment;
            }
        }
        for (size_t pos = 0; pos < count; ++pos) {
            assignment[pos] += 1;
            if (assignment[pos] < group_count) break;
            assignment[pos] = 0;
            if (pos == count - 1) done = true;
        }

        if (!done && std::chrono::steady_clock::now() > deadline) {
            timed_out = true;
            break;
        }
    }

    if (timed_out) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__
            << ": timed out after " << max_ms << "ms"
            << ", filament_count=" << count
            << ", group_count=" << group_count
            << ", returning best assignment found so far";
    }

    return best_assignment;
}

// ==================== NozzleInfo serialization ====================

std::string NozzleInfo::serialize() const
{
    std::ostringstream oss;
    oss << "id=\"" << group_id << "\" "
        << "extruder_id=\"" << extruder_id + 1 << "\" "
        << "nozzle_diameter=\"" << diameter << "\" "
        << "volume_type=\"" << get_nozzle_volume_type_string(volume_type) << "\"";
    return oss.str();
}

}} // namespace Slic3r::MultiNozzleUtils
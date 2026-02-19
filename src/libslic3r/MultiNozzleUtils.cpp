#include "MultiNozzleUtils.hpp"
#include "ProjectTask.hpp"
#include "Utils.hpp"

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
        int         nozzle_idx   = filament_info[idx].group_id;
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


void NozzleStatusRecorder::set_nozzle_status(int nozzle_id, int filament_id)
{
    nozzle_filament_status[nozzle_id] = filament_id;
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



}} // namespace Slic3r::MultiNozzleUtils
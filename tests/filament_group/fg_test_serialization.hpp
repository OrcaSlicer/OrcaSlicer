#ifndef FG_TEST_SERIALIZATION_HPP
#define FG_TEST_SERIALIZATION_HPP

#include <nlohmann/json.hpp>
#include <libslic3r/FilamentGroup.hpp>
#include <libslic3r/FilamentGroupUtils.hpp>
#include <libslic3r/MultiNozzleUtils.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>

using json = nlohmann::json;

// Put serializers in correct ADL namespaces for each type

namespace Slic3r {
namespace FilamentGroupUtils {

inline void to_json(json& j, const Color& c) {
    char buf[10];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
    j = std::string(buf);
}

inline void from_json(const json& j, Color& c) {
    std::string s = j.get<std::string>();
    if (s.size() >= 7 && s[0] == '#') {
        c.r = (unsigned char)std::stoi(s.substr(1, 2), nullptr, 16);
        c.g = (unsigned char)std::stoi(s.substr(3, 2), nullptr, 16);
        c.b = (unsigned char)std::stoi(s.substr(5, 2), nullptr, 16);
        c.a = (s.size() >= 9) ? (unsigned char)std::stoi(s.substr(7, 2), nullptr, 16) : 255;
    }
}

inline void to_json(json& j, const FilamentInfo& fi) {
    j = json{
        {"color", fi.color},
        {"type", fi.type},
        {"is_support", fi.is_support},
        {"usage_type", (int)fi.usage_type}
    };
}

inline void from_json(const json& j, FilamentInfo& fi) {
    fi.color = j.at("color").get<Color>();
    j.at("type").get_to(fi.type);
    j.at("is_support").get_to(fi.is_support);
    fi.usage_type = (FilamentUsageType)j.at("usage_type").get<int>();
}

inline void to_json(json& j, const MachineFilamentInfo& mfi) {
    j = json{
        {"color", mfi.color},
        {"type", mfi.type},
        {"is_support", mfi.is_support},
        {"usage_type", (int)mfi.usage_type},
        {"extruder_id", mfi.extruder_id},
        {"is_extended", mfi.is_extended}
    };
}

inline void from_json(const json& j, MachineFilamentInfo& mfi) {
    mfi.color = j.at("color").get<Color>();
    j.at("type").get_to(mfi.type);
    j.at("is_support").get_to(mfi.is_support);
    mfi.usage_type = (FilamentUsageType)j.at("usage_type").get<int>();
    j.at("extruder_id").get_to(mfi.extruder_id);
    j.at("is_extended").get_to(mfi.is_extended);
}

} // namespace FilamentGroupUtils

namespace MultiNozzleUtils {

inline void to_json(json& j, const NozzleInfo& ni) {
    j = json{
        {"diameter", ni.diameter},
        {"volume_type", (int)ni.volume_type},
        {"extruder_id", ni.extruder_id},
        {"group_id", ni.group_id}
    };
}

inline void from_json(const json& j, NozzleInfo& ni) {
    j.at("diameter").get_to(ni.diameter);
    ni.volume_type = (NozzleVolumeType)j.at("volume_type").get<int>();
    j.at("extruder_id").get_to(ni.extruder_id);
    j.at("group_id").get_to(ni.group_id);
}

inline void to_json(json& j, const FilamentChangeTimeParams& p) {
    j = json{
        {"selector_load_time", p.selector_load_time},
        {"selector_unload_time", p.selector_unload_time},
        {"standard_load_time", p.standard_load_time},
        {"standard_unload_time", p.standard_unload_time}
    };
}

inline void from_json(const json& j, FilamentChangeTimeParams& p) {
    j.at("selector_load_time").get_to(p.selector_load_time);
    j.at("selector_unload_time").get_to(p.selector_unload_time);
    j.at("standard_load_time").get_to(p.standard_load_time);
    j.at("standard_unload_time").get_to(p.standard_unload_time);
}

} // namespace MultiNozzleUtils

// ============ Helper: set<int> as JSON array ============
namespace FGTestDetail {
inline json set_to_json(const std::set<int>& s) {
    return json(std::vector<int>(s.begin(), s.end()));
}

inline std::set<int> json_to_set(const json& j) {
    auto v = j.get<std::vector<int>>();
    return std::set<int>(v.begin(), v.end());
}

inline json nvt_set_to_json(const std::set<NozzleVolumeType>& s) {
    std::vector<int> v;
    for (auto t : s) v.push_back((int)t);
    return json(v);
}

inline std::set<NozzleVolumeType> json_to_nvt_set(const json& j) {
    std::set<NozzleVolumeType> s;
    for (auto& item : j) s.insert((NozzleVolumeType)item.get<int>());
    return s;
}
} // namespace FGTestDetail

// ============ FilamentGroupContext::ModelInfo ============
inline void to_json(json& j, const FilamentGroupContext::ModelInfo& mi) {
    using namespace FGTestDetail;
    j["flush_matrix"] = mi.flush_matrix;
    j["layer_filaments"] = mi.layer_filaments;

    j["filament_info"] = json::array();
    for (auto& fi : mi.filament_info)
        j["filament_info"].push_back(fi);

    j["filament_ids"] = mi.filament_ids;

    j["unprintable_filaments"] = json::array();
    for (auto& s : mi.unprintable_filaments)
        j["unprintable_filaments"].push_back(set_to_json(s));

    json uv = json::object();
    for (auto& [fil, types] : mi.unprintable_volumes)
        uv[std::to_string(fil)] = nvt_set_to_json(types);
    j["unprintable_volumes"] = uv;
}

inline void from_json(const json& j, FilamentGroupContext::ModelInfo& mi) {
    using namespace FGTestDetail;
    j.at("flush_matrix").get_to(mi.flush_matrix);
    j.at("layer_filaments").get_to(mi.layer_filaments);

    mi.filament_info.clear();
    for (auto& item : j.at("filament_info"))
        mi.filament_info.push_back(item.get<FilamentGroupUtils::FilamentInfo>());

    j.at("filament_ids").get_to(mi.filament_ids);

    mi.unprintable_filaments.clear();
    for (auto& item : j.at("unprintable_filaments"))
        mi.unprintable_filaments.push_back(json_to_set(item));

    mi.unprintable_volumes.clear();
    if (j.contains("unprintable_volumes")) {
        for (auto& [k, v] : j.at("unprintable_volumes").items())
            mi.unprintable_volumes[std::stoi(k)] = json_to_nvt_set(v);
    }
}

// ============ FilamentGroupContext::GroupInfo ============
inline void to_json(json& j, const FilamentGroupContext::GroupInfo& gi) {
    j = json{
        {"total_filament_num", gi.total_filament_num},
        {"max_gap_threshold", gi.max_gap_threshold},
        {"mode", (int)gi.mode},
        {"strategy", (int)gi.strategy},
        {"ignore_ext_filament", gi.ignore_ext_filament},
        {"has_filament_switcher", gi.has_filament_switcher},
        {"filament_volume_map", gi.filament_volume_map}
    };
}

inline void from_json(const json& j, FilamentGroupContext::GroupInfo& gi) {
    j.at("total_filament_num").get_to(gi.total_filament_num);
    j.at("max_gap_threshold").get_to(gi.max_gap_threshold);
    gi.mode = (FGMode)j.at("mode").get<int>();
    gi.strategy = (FGStrategy)j.at("strategy").get<int>();
    j.at("ignore_ext_filament").get_to(gi.ignore_ext_filament);
    j.at("has_filament_switcher").get_to(gi.has_filament_switcher);
    j.at("filament_volume_map").get_to(gi.filament_volume_map);
}

// ============ FilamentGroupContext::MachineInfo ============
inline void to_json(json& j, const FilamentGroupContext::MachineInfo& mi) {
    j["max_group_size"] = mi.max_group_size;

    j["machine_filament_info"] = json::array();
    for (auto& vec : mi.machine_filament_info) {
        json arr = json::array();
        for (auto& mfi : vec) arr.push_back(mfi);
        j["machine_filament_info"].push_back(arr);
    }

    j["prefer_non_model_filament"] = mi.prefer_non_model_filament;
    j["master_extruder_id"] = mi.master_extruder_id;
}

inline void from_json(const json& j, FilamentGroupContext::MachineInfo& mi) {
    j.at("max_group_size").get_to(mi.max_group_size);

    mi.machine_filament_info.clear();
    for (auto& arr : j.at("machine_filament_info")) {
        std::vector<FilamentGroupUtils::MachineFilamentInfo> vec;
        for (auto& item : arr)
            vec.push_back(item.get<FilamentGroupUtils::MachineFilamentInfo>());
        mi.machine_filament_info.push_back(std::move(vec));
    }

    j.at("prefer_non_model_filament").get_to(mi.prefer_non_model_filament);
    j.at("master_extruder_id").get_to(mi.master_extruder_id);
}

// ============ FilamentGroupContext::SpeedInfo ============
inline void to_json(json& j, const FilamentGroupContext::SpeedInfo& si) {
    json fpt = json::object();
    for (auto& [fil, inner] : si.filament_print_time) {
        json inner_j = json::object();
        for (auto& [layer, time] : inner)
            inner_j[std::to_string(layer)] = time;
        fpt[std::to_string(fil)] = inner_j;
    }
    j["filament_print_time"] = fpt;
    j["extruder_change_time"] = si.extruder_change_time;
    j["filament_change_time"] = si.filament_change_time;
    j["group_with_time"] = si.group_with_time;
    j["change_time_params"] = si.change_time_params;
    j["ams_preload_enabled"] = si.ams_preload_enabled;
}

inline void from_json(const json& j, FilamentGroupContext::SpeedInfo& si) {
    si.filament_print_time.clear();
    if (j.contains("filament_print_time")) {
        for (auto& [k, v] : j.at("filament_print_time").items()) {
            int fil = std::stoi(k);
            for (auto& [k2, v2] : v.items())
                si.filament_print_time[fil][std::stoi(k2)] = v2.get<double>();
        }
    }
    j.at("extruder_change_time").get_to(si.extruder_change_time);
    j.at("filament_change_time").get_to(si.filament_change_time);
    j.at("group_with_time").get_to(si.group_with_time);
    si.change_time_params = j.at("change_time_params").get<MultiNozzleUtils::FilamentChangeTimeParams>();
    j.at("ams_preload_enabled").get_to(si.ams_preload_enabled);
}

// ============ FilamentGroupContext::NozzleInfo ============
inline void to_json(json& j, const FilamentGroupContext::NozzleInfo& ni) {
    json enl = json::object();
    for (auto& [ext, nozzles] : ni.extruder_nozzle_list)
        enl[std::to_string(ext)] = nozzles;
    j["extruder_nozzle_list"] = enl;

    j["nozzle_list"] = json::array();
    for (auto& n : ni.nozzle_list)
        j["nozzle_list"].push_back(n);

    json ns = json::object();
    for (auto& [noz, fil] : ni.nozzle_status)
        ns[std::to_string(noz)] = fil;
    j["nozzle_status"] = ns;
}

inline void from_json(const json& j, FilamentGroupContext::NozzleInfo& ni) {
    ni.extruder_nozzle_list.clear();
    for (auto& [k, v] : j.at("extruder_nozzle_list").items())
        ni.extruder_nozzle_list[std::stoi(k)] = v.get<std::vector<int>>();

    ni.nozzle_list.clear();
    for (auto& item : j.at("nozzle_list"))
        ni.nozzle_list.push_back(item.get<MultiNozzleUtils::NozzleInfo>());

    ni.nozzle_status.clear();
    if (j.contains("nozzle_status")) {
        for (auto& [k, v] : j.at("nozzle_status").items())
            ni.nozzle_status[std::stoi(k)] = v.get<int>();
    }
}

// ============ Full FilamentGroupContext ============
inline void to_json(json& j, const FilamentGroupContext& ctx) {
    json mi, gi, mai, si, ni;
    to_json(mi, ctx.model_info);
    to_json(gi, ctx.group_info);
    to_json(mai, ctx.machine_info);
    to_json(si, ctx.speed_info);
    to_json(ni, ctx.nozzle_info);
    j["model_info"] = mi;
    j["group_info"] = gi;
    j["machine_info"] = mai;
    j["speed_info"] = si;
    j["nozzle_info"] = ni;
}

inline void from_json(const json& j, FilamentGroupContext& ctx) {
    from_json(j.at("model_info"), ctx.model_info);
    from_json(j.at("group_info"), ctx.group_info);
    from_json(j.at("machine_info"), ctx.machine_info);
    from_json(j.at("speed_info"), ctx.speed_info);
    from_json(j.at("nozzle_info"), ctx.nozzle_info);
}

} // namespace Slic3r

// ============ Test-specific types in FGTest namespace ============
namespace Slic3r {
namespace FGTest {

struct TestMetadata {
    std::string id;
    std::string config_type;
    int seed = 0;
};

inline void to_json(json& j, const TestMetadata& m) {
    j = json{{"id", m.id}, {"config_type", m.config_type}, {"seed", m.seed}};
}

inline void from_json(const json& j, TestMetadata& m) {
    j.at("id").get_to(m.id);
    j.at("config_type").get_to(m.config_type);
    j.at("seed").get_to(m.seed);
}

struct TestResult {
    std::vector<int> filament_map;
    int flush_cost = 0;
    double elapsed_ms = 0;
    bool constraints_ok = true;
    std::vector<std::string> violations;
};

inline void to_json(json& j, const TestResult& r) {
    j = json{
        {"filament_map", r.filament_map},
        {"flush_cost", r.flush_cost},
        {"elapsed_ms", r.elapsed_ms},
        {"constraints_ok", r.constraints_ok},
        {"violations", r.violations}
    };
}

inline void from_json(const json& j, TestResult& r) {
    j.at("filament_map").get_to(r.filament_map);
    j.at("flush_cost").get_to(r.flush_cost);
    j.at("elapsed_ms").get_to(r.elapsed_ms);
    j.at("constraints_ok").get_to(r.constraints_ok);
    if (j.contains("violations"))
        j.at("violations").get_to(r.violations);
}

// ============ Base Result (golden baseline stored in input file) ============
struct BaseResult {
    double full_score = 0;
    int flush_cost = 0;
    bool constraints_ok = true;
};

inline void to_json(json& j, const BaseResult& g) {
    j = json{
        {"full_score", g.full_score},
        {"flush_cost", g.flush_cost},
        {"constraints_ok", g.constraints_ok}
    };
}

inline void from_json(const json& j, BaseResult& g) {
    j.at("full_score").get_to(g.full_score);
    j.at("flush_cost").get_to(g.flush_cost);
    j.at("constraints_ok").get_to(g.constraints_ok);
}

// ============ File I/O ============
struct TestCase {
    TestMetadata metadata;
    FilamentGroupContext context;
    std::optional<BaseResult> base_result;
};

inline TestCase load_test_case(const std::string& path) {
    std::ifstream f(path);
    json j = json::parse(f);
    TestCase tc;
    tc.metadata = j.at("metadata").get<TestMetadata>();
    Slic3r::from_json(j.at("context"), tc.context);
    if (j.contains("base_result"))
        tc.base_result = j.at("base_result").get<BaseResult>();
    return tc;
}

inline void save_test_case(const std::string& path, const TestCase& tc) {
    json j;
    j["metadata"] = tc.metadata;
    json ctx_j;
    Slic3r::to_json(ctx_j, tc.context);
    j["context"] = ctx_j;
    if (tc.base_result)
        j["base_result"] = *tc.base_result;
    std::ofstream f(path);
    f << j.dump(-1);
}

inline void save_result(const std::string& case_path, const TestResult& result) {
    std::string result_path = case_path;
    auto pos = result_path.rfind(".json");
    if (pos != std::string::npos)
        result_path = result_path.substr(0, pos) + ".result.json";
    else
        result_path += ".result.json";

    json j = result;
    std::ofstream f(result_path);
    f << j.dump(2);
}

inline TestResult load_result(const std::string& result_path) {
    std::ifstream f(result_path);
    json j = json::parse(f);
    return j.get<TestResult>();
}

} // namespace FGTest
} // namespace Slic3r

#endif // FG_TEST_SERIALIZATION_HPP

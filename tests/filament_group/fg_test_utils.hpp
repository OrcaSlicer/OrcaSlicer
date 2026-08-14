#ifndef FG_TEST_UTILS_HPP
#define FG_TEST_UTILS_HPP

#include "fg_test_serialization.hpp"
#include <random>
#include <algorithm>
#include <cassert>

namespace Slic3r {
namespace FGTest {

class TestRng {
public:
    explicit TestRng(int seed) : m_gen(seed) {}

    int rand_int(int lo, int hi) {
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(m_gen);
    }

    float rand_float(float lo, float hi) {
        std::uniform_real_distribution<float> dist(lo, hi);
        return dist(m_gen);
    }

    double rand_double(double lo, double hi) {
        std::uniform_real_distribution<double> dist(lo, hi);
        return dist(m_gen);
    }

    bool rand_bool(double prob = 0.5) {
        return rand_double(0, 1) < prob;
    }

    template<typename T>
    void shuffle(std::vector<T>& v) {
        std::shuffle(v.begin(), v.end(), m_gen);
    }

private:
    std::mt19937 m_gen;
};

// Generate a flush matrix for one extruder: [filament_count x filament_count]
inline std::vector<std::vector<float>> generate_flush_matrix(int filament_count, TestRng& rng) {
    std::vector<std::vector<float>> matrix(filament_count, std::vector<float>(filament_count, 0.0f));
    for (int i = 0; i < filament_count; ++i) {
        for (int j = 0; j < filament_count; ++j) {
            if (i == j)
                matrix[i][j] = 0.0f;
            else
                matrix[i][j] = rng.rand_float(10.0f, 600.0f);
        }
    }
    return matrix;
}

// Generate layer_filaments with interval characteristics
inline std::vector<std::vector<unsigned int>> generate_layer_filaments_interval(
    int num_layers, int total_filaments, const std::vector<unsigned int>& used_filaments, TestRng& rng)
{
    std::vector<std::vector<unsigned int>> layers;
    layers.reserve(num_layers);

    int n_used = (int)used_filaments.size();
    int fils_per_layer_min = std::min(2, n_used);
    int fils_per_layer_max = std::min(n_used, std::max(2, n_used / 2 + 1));

    // First layer: random subset
    int first_count = rng.rand_int(fils_per_layer_min, fils_per_layer_max);
    std::vector<unsigned int> pool = used_filaments;
    rng.shuffle(pool);
    std::vector<unsigned int> current(pool.begin(), pool.begin() + first_count);
    std::sort(current.begin(), current.end());
    layers.push_back(current);

    for (int layer = 1; layer < num_layers; ++layer) {
        // 10% chance: completely random new set (object boundary)
        if (rng.rand_bool(0.10)) {
            int count = rng.rand_int(fils_per_layer_min, fils_per_layer_max);
            pool = used_filaments;
            rng.shuffle(pool);
            current.assign(pool.begin(), pool.begin() + count);
        } else {
            // Markov: keep each filament with 70% prob, maybe add new ones
            std::vector<unsigned int> next;
            for (auto f : current) {
                if (rng.rand_bool(0.70))
                    next.push_back(f);
            }
            // Maybe add a filament not in current
            if (rng.rand_bool(0.30) || next.empty()) {
                std::vector<unsigned int> candidates;
                std::set<unsigned int> cur_set(next.begin(), next.end());
                for (auto f : used_filaments) {
                    if (!cur_set.count(f))
                        candidates.push_back(f);
                }
                if (!candidates.empty()) {
                    next.push_back(candidates[rng.rand_int(0, (int)candidates.size() - 1)]);
                }
            }
            if (next.empty())
                next.push_back(used_filaments[rng.rand_int(0, n_used - 1)]);
            current = next;
        }
        std::sort(current.begin(), current.end());
        current.erase(std::unique(current.begin(), current.end()), current.end());
        layers.push_back(current);
    }

    return layers;
}

// Generate layer_filaments where every layer is different (stress/edge)
inline std::vector<std::vector<unsigned int>> generate_layer_filaments_chaotic(
    int num_layers, int total_filaments, const std::vector<unsigned int>& used_filaments, TestRng& rng)
{
    std::vector<std::vector<unsigned int>> layers;
    int n_used = (int)used_filaments.size();
    int fils_per_layer_min = std::min(2, n_used);
    int fils_per_layer_max = n_used;

    for (int layer = 0; layer < num_layers; ++layer) {
        int count = rng.rand_int(fils_per_layer_min, fils_per_layer_max);
        std::vector<unsigned int> pool = used_filaments;
        rng.shuffle(pool);
        std::vector<unsigned int> current(pool.begin(), pool.begin() + count);
        std::sort(current.begin(), current.end());
        layers.push_back(current);
    }
    return layers;
}

// Generate layer_filaments where all layers are the same (edge)
inline std::vector<std::vector<unsigned int>> generate_layer_filaments_uniform(
    int num_layers, const std::vector<unsigned int>& used_filaments)
{
    return std::vector<std::vector<unsigned int>>(num_layers, used_filaments);
}

// Generate filament info
inline std::vector<FilamentGroupUtils::FilamentInfo> generate_filament_info(int count, TestRng& rng) {
    static const char* types[] = {"PLA", "ABS", "PETG", "TPU", "PA", "PLA-S"};
    std::vector<FilamentGroupUtils::FilamentInfo> infos;
    for (int i = 0; i < count; ++i) {
        FilamentGroupUtils::FilamentInfo fi;
        fi.color = FilamentGroupUtils::Color(
            (unsigned char)rng.rand_int(0, 255),
            (unsigned char)rng.rand_int(0, 255),
            (unsigned char)rng.rand_int(0, 255));
        fi.type = types[rng.rand_int(0, 5)];
        fi.is_support = (fi.type == "PLA-S");
        fi.usage_type = fi.is_support ? FilamentUsageType::SupportOnly : FilamentUsageType::ModelOnly;
        infos.push_back(fi);
    }
    return infos;
}

// Generate machine filament info (per extruder)
inline std::vector<std::vector<FilamentGroupUtils::MachineFilamentInfo>> generate_machine_filament_info(
    int num_extruders, int filaments_per_extruder, TestRng& rng)
{
    std::vector<std::vector<FilamentGroupUtils::MachineFilamentInfo>> result;
    for (int ext = 0; ext < num_extruders; ++ext) {
        std::vector<FilamentGroupUtils::MachineFilamentInfo> vec;
        for (int i = 0; i < filaments_per_extruder; ++i) {
            FilamentGroupUtils::MachineFilamentInfo mfi;
            mfi.color = FilamentGroupUtils::Color(
                (unsigned char)rng.rand_int(0, 255),
                (unsigned char)rng.rand_int(0, 255),
                (unsigned char)rng.rand_int(0, 255));
            mfi.type = "PLA";
            mfi.is_support = false;
            mfi.usage_type = FilamentUsageType::ModelOnly;
            mfi.extruder_id = ext;
            mfi.is_extended = (i >= 4);
            vec.push_back(mfi);
        }
        result.push_back(vec);
    }
    return result;
}

// ============ Machine Config Builders ============

// Config A: 2 extruders, 1 nozzle each
inline void build_config_a(FilamentGroupContext& ctx, int num_filaments, TestRng& rng) {
    auto& ni = ctx.nozzle_info;
    ni.nozzle_list.clear();
    ni.nozzle_list.push_back({"0.4", NozzleVolumeType::nvtStandard, 0, 0});
    ni.nozzle_list.push_back({"0.4", NozzleVolumeType::nvtStandard, 1, 1});
    ni.extruder_nozzle_list = {{0, {0}}, {1, {1}}};

    ctx.machine_info.max_group_size = {num_filaments / 2 + 1, num_filaments / 2 + 1};
    ctx.machine_info.prefer_non_model_filament = {false, true};
    ctx.machine_info.master_extruder_id = 0;
    ctx.machine_info.machine_filament_info = generate_machine_filament_info(2, 4, rng);

    ctx.group_info.filament_volume_map.assign(num_filaments, (int)NozzleVolumeType::nvtHybrid);

    ctx.model_info.unprintable_filaments.resize(2);
    ctx.model_info.flush_matrix.resize(2);
    for (int ext = 0; ext < 2; ++ext)
        ctx.model_info.flush_matrix[ext] = generate_flush_matrix(num_filaments, rng);
}

// Config B: 2 extruders, ext0 has 1 nozzle, ext1 has K nozzles (K in [2,6])
inline void build_config_b(FilamentGroupContext& ctx, int num_filaments, int k_nozzles, TestRng& rng) {
    auto& ni = ctx.nozzle_info;
    ni.nozzle_list.clear();
    ni.nozzle_list.push_back({"0.4", NozzleVolumeType::nvtStandard, 0, 0});

    static const NozzleVolumeType vol_types[] = {
        NozzleVolumeType::nvtStandard, NozzleVolumeType::nvtHighFlow, NozzleVolumeType::nvtTPUHighFlow};

    std::vector<int> ext1_nozzles;
    for (int i = 0; i < k_nozzles; ++i) {
        int group_id = i + 1;
        NozzleVolumeType vt = vol_types[rng.rand_int(0, 2)];
        ni.nozzle_list.push_back({"0.4", vt, 1, group_id});
        ext1_nozzles.push_back(group_id);
    }
    ni.extruder_nozzle_list = {{0, {0}}, {1, ext1_nozzles}};

    int ext0_max = std::max(4, num_filaments / 2 + 1);
    int ext1_max = std::max(k_nozzles * 2, num_filaments - ext0_max + 1);
    ctx.machine_info.max_group_size = {ext0_max, ext1_max};
    ctx.machine_info.prefer_non_model_filament = {false, false};
    ctx.machine_info.master_extruder_id = 0;
    ctx.machine_info.machine_filament_info = generate_machine_filament_info(2, 4, rng);

    ctx.group_info.filament_volume_map.assign(num_filaments, (int)NozzleVolumeType::nvtHybrid);

    ctx.model_info.unprintable_filaments.resize(2);
    ctx.model_info.flush_matrix.resize(2);
    for (int ext = 0; ext < 2; ++ext)
        ctx.model_info.flush_matrix[ext] = generate_flush_matrix(num_filaments, rng);
}

// Config C: 1 extruder, K nozzles (K in [3,9])
inline void build_config_c(FilamentGroupContext& ctx, int num_filaments, int k_nozzles, TestRng& rng) {
    auto& ni = ctx.nozzle_info;
    ni.nozzle_list.clear();

    static const NozzleVolumeType vol_types[] = {
        NozzleVolumeType::nvtStandard, NozzleVolumeType::nvtHighFlow,
        NozzleVolumeType::nvtHybrid, NozzleVolumeType::nvtTPUHighFlow};

    std::vector<int> nozzle_ids;
    for (int i = 0; i < k_nozzles; ++i) {
        NozzleVolumeType vt = vol_types[i % 4];
        ni.nozzle_list.push_back({"0.4", vt, 0, i});
        nozzle_ids.push_back(i);
    }
    ni.extruder_nozzle_list = {{0, nozzle_ids}};

    ctx.machine_info.max_group_size = {num_filaments};
    ctx.machine_info.prefer_non_model_filament = {false};
    ctx.machine_info.master_extruder_id = 0;
    ctx.machine_info.machine_filament_info = generate_machine_filament_info(1, 4, rng);

    ctx.group_info.filament_volume_map.assign(num_filaments, (int)NozzleVolumeType::nvtHybrid);

    ctx.model_info.unprintable_filaments.resize(1);
    ctx.model_info.flush_matrix.resize(1);
    ctx.model_info.flush_matrix[0] = generate_flush_matrix(num_filaments, rng);
}

// ============ Constraint Injection ============

// Add unprintable_filaments constraints (some filaments forbidden on some extruders)
inline void inject_unprintable_constraints(FilamentGroupContext& ctx,
                                            const std::vector<unsigned int>& used_filaments,
                                            TestRng& rng, int num_constraints) {
    int num_ext = (int)ctx.model_info.unprintable_filaments.size();
    for (int i = 0; i < num_constraints && !used_filaments.empty(); ++i) {
        int fil = used_filaments[rng.rand_int(0, (int)used_filaments.size() - 1)];
        int ext = rng.rand_int(0, num_ext - 1);
        ctx.model_info.unprintable_filaments[ext].insert(fil);
    }
    // Ensure no filament is banned from ALL extruders
    for (auto fil : used_filaments) {
        bool can_print_somewhere = false;
        for (int ext = 0; ext < num_ext; ++ext) {
            if (!ctx.model_info.unprintable_filaments[ext].count(fil)) {
                can_print_somewhere = true;
                break;
            }
        }
        if (!can_print_somewhere) {
            int ext_to_allow = rng.rand_int(0, num_ext - 1);
            ctx.model_info.unprintable_filaments[ext_to_allow].erase(fil);
        }
    }
}

// Add unprintable_volumes constraints
inline void inject_volume_constraints(FilamentGroupContext& ctx,
                                       const std::vector<unsigned int>& used_filaments,
                                       TestRng& rng, int num_constraints) {
    static const NozzleVolumeType vols[] = {
        NozzleVolumeType::nvtStandard, NozzleVolumeType::nvtHighFlow,
        NozzleVolumeType::nvtTPUHighFlow};

    for (int i = 0; i < num_constraints && !used_filaments.empty(); ++i) {
        int fil = used_filaments[rng.rand_int(0, (int)used_filaments.size() - 1)];
        NozzleVolumeType vt = vols[rng.rand_int(0, 2)];
        ctx.model_info.unprintable_volumes[fil].insert(vt);
    }
    // Ensure no filament is banned from ALL nozzle volume types present
    for (auto fil : used_filaments) {
        if (!ctx.model_info.unprintable_volumes.count(fil))
            continue;
        auto& banned = ctx.model_info.unprintable_volumes[fil];
        bool can_go_somewhere = false;
        for (auto& noz : ctx.nozzle_info.nozzle_list) {
            if (!banned.count(noz.volume_type)) {
                can_go_somewhere = true;
                break;
            }
        }
        if (!can_go_somewhere && !banned.empty()) {
            // Remove one random ban
            auto it = banned.begin();
            std::advance(it, rng.rand_int(0, (int)banned.size() - 1));
            banned.erase(it);
        }
    }
}

// ============ Full Case Builder ============

inline TestCase build_test_case(const std::string& id, const std::string& config_type,
                                int seed, int num_filaments, int num_layers,
                                bool chaotic_layers, bool with_constraints,
                                FGMode mode, FGStrategy strategy, bool group_with_time) {
    TestRng rng(seed);
    TestCase tc;
    tc.metadata.id = id;
    tc.metadata.config_type = config_type;
    tc.metadata.seed = seed;

    auto& ctx = tc.context;

    // Used filaments: 0-based indices
    std::vector<unsigned int> used_filaments;
    for (int i = 0; i < num_filaments; ++i)
        used_filaments.push_back((unsigned int)i);

    // Build machine config
    if (config_type == "A") {
        build_config_a(ctx, num_filaments, rng);
    } else if (config_type == "B") {
        int k = rng.rand_int(2, 6);
        build_config_b(ctx, num_filaments, k, rng);
    } else {
        int k = rng.rand_int(3, 9);
        build_config_c(ctx, num_filaments, k, rng);
    }

    // Layer filaments
    if (chaotic_layers)
        ctx.model_info.layer_filaments = generate_layer_filaments_chaotic(num_layers, num_filaments, used_filaments, rng);
    else
        ctx.model_info.layer_filaments = generate_layer_filaments_interval(num_layers, num_filaments, used_filaments, rng);

    // Filament info
    ctx.model_info.filament_info = generate_filament_info(num_filaments, rng);
    ctx.model_info.filament_ids.resize(num_filaments);
    for (int i = 0; i < num_filaments; ++i)
        ctx.model_info.filament_ids[i] = "GFL_" + std::to_string(i);

    // Group info
    ctx.group_info.total_filament_num = num_filaments;
    ctx.group_info.max_gap_threshold = 0.01;
    ctx.group_info.mode = mode;
    ctx.group_info.strategy = strategy;
    ctx.group_info.ignore_ext_filament = false;
    ctx.group_info.has_filament_switcher = false;

    // Speed info
    ctx.speed_info.extruder_change_time = 5.0;
    ctx.speed_info.filament_change_time = 2.0;
    ctx.speed_info.group_with_time = group_with_time;
    ctx.speed_info.change_time_params = {1.0f, 1.0f, 3.0f, 2.0f};
    int num_ext = (config_type == "C") ? 1 : 2;
    ctx.speed_info.ams_preload_enabled.assign(num_ext, true);

    // Constraints
    if (with_constraints) {
        inject_unprintable_constraints(ctx, used_filaments, rng, rng.rand_int(1, num_filaments / 2));
        if (config_type != "A")
            inject_volume_constraints(ctx, used_filaments, rng, rng.rand_int(1, 3));
    }

    // Nozzle status (initially empty)
    ctx.nozzle_info.nozzle_status.clear();

    return tc;
}

} // namespace FGTest
} // namespace Slic3r

#endif // FG_TEST_UTILS_HPP

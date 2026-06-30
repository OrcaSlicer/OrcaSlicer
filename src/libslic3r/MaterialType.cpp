#include "MaterialType.hpp"

#include <algorithm>

namespace Slic3r {
namespace {
constexpr int    DEFAULT_MIN_TEMP             = 190;
constexpr int    DEFAULT_MAX_TEMP             = 300;
constexpr int    DEFAULT_CHAMBER_MIN_TEMP     = 0;
constexpr int    DEFAULT_CHAMBER_MAX_TEMP     = 100;
constexpr double DEFAULT_ADHESION_COEFFICIENT = 1.0;
constexpr double DEFAULT_YIELD_STRENGTH       = 0.02;
constexpr double DEFAULT_THERMAL_LENGTH       = 200.0;
} // namespace

const std::vector<MaterialTypeInfo>& MaterialType::all()
{
    // Column after thermal_length: base_materials (material families; empty falls back to the name).
    static const std::vector<MaterialTypeInfo> material_types = {
        {"ABS",         190, 300, 50, 65,  1,   0.1 , 100,  {}},
        {"ABS-CF",      220, 300, 50, 65,  1,   0.1 , 100,  {"ABS"}},
        {"ABS-GF",      240, 280, 50, 65,  1,   0.1 , 100,  {"ABS"}},
        {"ASA",         220, 300, 50, 65,  1,   0.1 , 100,  {}},
        {"ASA-CF",      230, 300, 50, 65,  1,   0.1 , 100,  {"ASA"}},
        {"ASA-GF",      240, 300, 50, 65,  1,   0.1 , 100,  {"ASA"}},
        {"ASA-AERO",    240, 280, 50, 65,  1,   0.1 , 100,  {"ASA"}},
        {"BVOH",        190, 240, 0,  70,  1,   0.02, 200,  {}},
        {"CoPE",        190, 240, 0,  45,  1,   0.02, 200,  {}},
        {"EVA",         175, 220, 0,  50,  1,   0.02, 200,  {}},
        {"FLEX",        210, 230, 0,  50,  0.5, 0.02, 1000, {}},
        {"HIPS",        220, 270, 50, 60,  1,   0.02, 200,  {}},
        {"PA",          235, 280, 50, 60,  1,   0.02, 100,  {}},
        {"PA-CF",       240, 315, 50, 60,  1,   0.02, 100,  {"PA"}},
        {"PA-GF",       240, 290, 50, 60,  1,   0.02, 100,  {"PA"}},
        {"PA6",         260, 300, 50, 60,  1,   0.02, 100,  {}},
        {"PA6-CF",      230, 300, 50, 60,  1,   0.02, 100,  {"PA6"}},
        {"PA6-GF",      260, 300, 50, 60,  1,   0.02, 100,  {"PA6"}},
        {"PA11",        275, 295, 50, 60,  1,   0.02, 100,  {}},
        {"PA11-CF",     275, 295, 50, 60,  1,   0.02, 100,  {"PA11"}},
        {"PA11-GF",     275, 295, 50, 60,  1,   0.02, 100,  {"PA11"}},
        {"PA12",        250, 270, 50, 60,  1,   0.02, 100,  {}},
        {"PA12-CF",     250, 300, 50, 60,  1,   0.02, 100,  {"PA12"}},
        {"PA12-GF",     255, 270, 50, 60,  1,   0.02, 100,  {"PA12"}},
        {"PAHT",        260, 310, 55, 65,  1,   0.02, 200,  {}},
        {"PAHT-CF",     270, 310, 55, 65,  1,   0.02, 200,  {"PAHT"}},
        {"PAHT-GF",     270, 310, 55, 65,  1,   0.02, 200,  {"PAHT"}},
        {"PC",          240, 300, 60, 70,  1,   0.02, 40,   {}},
        {"PC-ABS",      230, 270, 60, 70,  1,   0.02, 80,   {"PC", "ABS"}},
        {"PC-CF",       270, 295, 60, 70,  1,   0.02, 80,   {"PC"}},
        {"PC-PBT",      260, 300, 60, 70,  1,   0.02, 40,   {"PC", "PBT"}},
        {"PCL",         130, 170, 0,  45,  1,   0.02, 200,  {}},
        {"PCTG",        220, 300, 0,  55,  2,   0.02, 200,  {}},
        {"PE",          175, 260, 45, 60,  1,   0.02, 200,  {}},
        {"PE-CF",       175, 260, 45, 60,  1,   0.02, 200,  {"PE"}},
        {"PE-GF",       230, 270, 45, 60,  1,   0.02, 200,  {"PE"}},
        {"PEBA",        200, 270, 0,  50,  0.5, 0.02, 1000, {}},
        {"PEI-1010",    370, 430, 80, 100, 1,   0.02, 200,  {}},
        {"PEI-1010-CF", 380, 430, 80, 100, 1,   0.02, 200,  {"PEI-1010"}},
        {"PEI-1010-GF", 380, 430, 80, 100, 1,   0.02, 200,  {"PEI-1010"}},
        {"PEI-9085",    350, 390, 80, 100, 1,   0.02, 200,  {}},
        {"PEI-9085-CF", 365, 390, 80, 100, 1,   0.02, 200,  {"PEI-9085"}},
        {"PEI-9085-GF", 370, 390, 80, 100, 1,   0.02, 200,  {"PEI-9085"}},
        {"PEEK",        350, 460, 80, 100, 1,   0.02, 200,  {}},
        {"PEEK-CF",     380, 410, 80, 100, 1,   0.02, 200,  {"PEEK"}},
        {"PEEK-GF",     375, 410, 80, 100, 1,   0.02, 200,  {"PEEK"}},
        {"PEKK",        325, 400, 80, 100, 1,   0.02, 200,  {}},
        {"PEKK-CF",     360, 400, 80, 100, 1,   0.02, 200,  {"PEKK"}},
        {"PES",         340, 390, 80, 100, 1,   0.02, 200,  {}},
        {"PET",         200, 290, 0,  55,  2,   0.3 , 100,  {}},
        {"PET-CF",      240, 320, 0,  55,  2,   0.3 , 100,  {"PET"}},
        {"PET-GF",      280, 320, 0,  55,  2,   0.3 , 100,  {"PET"}},
        {"PETG",        190, 260, 0,  55,  2,   0.3 , 100,  {}},
        {"PETG-CF",     230, 290, 0,  55,  1,   0.3 , 100,  {"PETG"}},
        {"PETG-GF",     210, 270, 0,  55,  1,   0.3 , 100,  {"PETG"}},
        {"PHA",         190, 250, 0,  55,  1,   0.02, 200,  {}},
        {"PI",          390, 410, 90, 100, 1,   0.02, 200,  {}},
        {"PLA",         180, 240, 0,  45,  1,   0.02, 200,  {}},
        {"PLA-AERO",    220, 270, 0,  55,  1,   0.02, 200,  {"PLA"}},
        {"PLA-CF",      190, 250, 0,  50,  1,   0.02, 200,  {"PLA"}},
        {"POM",         210, 250, 50, 65,  1,   0.02, 200,  {}},
        {"PP",          200, 240, 45, 60,  1,   0.02, 200,  {}},
        {"PP-CF",       210, 250, 45, 60,  1,   0.02, 200,  {"PP"}},
        {"PP-GF",       220, 260, 45, 60,  1,   0.02, 200,  {"PP"}},
        {"PPA-CF",      260, 300, 55, 70,  1,   0.02, 200,  {"PPA"}},
        {"PPA-GF",      260, 290, 55, 70,  1,   0.02, 200,  {"PPA"}},
        {"PPS",         300, 345, 90, 100, 1,   0.02, 200,  {}},
        {"PPS-CF",      295, 350, 90, 100, 1,   0.02, 200,  {"PPS"}},
        {"PPSU",        360, 420, 90, 100, 1,   0.02, 200,  {}},
        {"PSU",         350, 380, 90, 100, 1,   0.02, 200,  {}},
        {"PVA",         185, 250, 0,  60,  1,   0.02, 200,  {}},
        {"PVB",         190, 250, 0,  55,  1,   0.02, 200,  {}},
        {"PVDF",        245, 265, 40, 60,  1,   0.02, 200,  {}},
        {"SBS",         195, 250, 0,  55,  1,   0.02, 200,  {}},
        {"TPI",         420, 445, 90, 100, 1,   0.02, 200,  {}},
        {"TPU",         175, 260, 0,  50,  0.5, 0.02, 1000, {}}
    };

    return material_types;
}

const std::vector<BaseMaterialCompatibility>& MaterialType::base_compatibilities()
{
    // Adhesion rules between base materials.
    // A shared base material already implies compatibility, so only cross-family rules need listing here.
    // Lookups are symmetric, so each pair is listed once.
    // Extend as more bonding data becomes available.
    static const std::vector<BaseMaterialCompatibility> base_compatibilities = {
        // base    compatible      incompatible
        {"ABS",    {"ASA", "PC"},  {"PLA", "PETG", "PP"}},
        {"PC",     {"ABS"},        {}},
        {"PLA",    {},             {"PETG", "PP"}},
        {"PETG",   {},             {"PP"}},
    };

    return base_compatibilities;
}

const MaterialTypeInfo* MaterialType::find(const std::string& name)
{
    const auto& types = all();
    const auto  it    = std::find_if(types.begin(), types.end(), [&name](const MaterialTypeInfo& info) { return info.name == name; });
    return it != types.end() ? &(*it) : nullptr;
}

bool MaterialType::get_temperature_range(const std::string& type, int& min_temp, int& max_temp)
{
    min_temp = DEFAULT_MIN_TEMP;
    max_temp = DEFAULT_MAX_TEMP;

    if (const auto* info = find(type)) {
        min_temp = info->min_temp;
        max_temp = info->max_temp;
        return true;
    }

    return false;
}

bool MaterialType::get_chamber_temperature_range(const std::string& type, int& chamber_min_temp, int& chamber_max_temp)
{
    chamber_min_temp = DEFAULT_CHAMBER_MIN_TEMP;
    chamber_max_temp = DEFAULT_CHAMBER_MAX_TEMP;

    if (const auto* info = find(type)) {
        chamber_min_temp = info->chamber_min_temp;
        chamber_max_temp = info->chamber_max_temp;
        return true;
    }

    return false;
}

bool MaterialType::get_adhesion_coefficient(const std::string& type, double& adhesion_coefficient)
{
    adhesion_coefficient = DEFAULT_ADHESION_COEFFICIENT;

    if (const auto* info = find(type)) {
        adhesion_coefficient = info->adhesion_coefficient;
        return true;
    }

    return false;
}

bool MaterialType::get_yield_strength(const std::string& type, double& yield_strength)
{
    yield_strength = DEFAULT_YIELD_STRENGTH;

    if (const auto* info = find(type)) {
        yield_strength = info->yield_strength;
        return true;
    }

    return false;
}

bool MaterialType::get_thermal_length(const std::string& type, double& thermal_length)
{
    thermal_length = DEFAULT_THERMAL_LENGTH;

    if (const auto* info = find(type)) {
        thermal_length = info->thermal_length;
        return true;
    }

    return false;
}

std::vector<std::string> MaterialType::base_materials(const std::string& type)
{
    if (const auto* info = find(type); info && !info->base_materials.empty())
        return info->base_materials;
    return {type};
}

namespace {
// Adhesion rule (compatible/incompatible) between two base materials, looked up symmetrically.
bool base_in_list(const std::string& base, const std::string& other, bool incompatible)
{
    const auto& table = MaterialType::base_compatibilities();
    const auto  it    = std::find_if(table.begin(), table.end(),
                                     [&base](const BaseMaterialCompatibility& bc) { return bc.base_material == base; });
    if (it == table.end())
        return false;
    const auto& list = incompatible ? it->incompatible : it->compatible;
    return std::find(list.begin(), list.end(), other) != list.end();
}

bool bases_listed(const std::string& base_a, const std::string& base_b, bool incompatible)
{
    return base_in_list(base_a, base_b, incompatible) || base_in_list(base_b, base_a, incompatible);
}
} // namespace

MaterialCompatibility MaterialType::compatibility(const std::string& type_a, const std::string& type_b)
{
    const std::vector<std::string> bases_a = base_materials(type_a);
    const std::vector<std::string> bases_b = base_materials(type_b);

    // Compare every base-material pairing. An explicit incompatibility anywhere wins; otherwise a
    // shared base or a listed compatibility means the materials adhere.
    bool compatible = false;
    for (const std::string& ba : bases_a) {
        for (const std::string& bb : bases_b) {
            if (bases_listed(ba, bb, /*incompatible=*/true))
                return MaterialCompatibility::Incompatible;
            if (ba == bb || bases_listed(ba, bb, /*incompatible=*/false))
                compatible = true;
        }
    }

    return compatible ? MaterialCompatibility::Compatible : MaterialCompatibility::Unknown;
}

} // namespace Slic3r

#pragma once

#include <string>
#include <vector>

namespace Slic3r {

struct MaterialTypeInfo {
    std::string name;
    int min_temp;
    int max_temp;
    int chamber_min_temp;
    int chamber_max_temp;
    double adhesion_coefficient;
    double yield_strength;
    double thermal_length;
};

class MaterialType {
public:
    static const std::vector<MaterialTypeInfo>& all();

    static const MaterialTypeInfo* find(const std::string& name);

    static bool get_temperature_range(const std::string& type, int& min_temp, int& max_temp);
    static bool get_chamber_temperature_range(const std::string& type, int& chamber_min_temp, int& chamber_max_temp);
    static bool get_adhesion_coefficient(const std::string& type, double& adhesion_coefficient);
    static bool get_yield_strength(const std::string& type, double& yield_strength);
    static bool get_thermal_length(const std::string& type, double& thermal_length);
};

} // namespace Slic3r

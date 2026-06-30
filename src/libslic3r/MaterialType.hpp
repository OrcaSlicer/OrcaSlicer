#pragma once

#include <string>
#include <vector>

namespace Slic3r {

enum class MaterialCompatibility {
    Incompatible, // the two materials are known not to bond/adhere
    Unknown,      // no information either way
    Compatible    // the two materials are known to bond/adhere
};

struct MaterialTypeInfo {
    std::string name;
    int min_temp;
    int max_temp;
    int chamber_min_temp;
    int chamber_max_temp;
    double adhesion_coefficient;
    double yield_strength;
    double thermal_length;
    // Material families this type belongs to (e.g. {"PLA"} for "PLA-CF", {"PC", "ABS"} for "PC-ABS").
    // Empty falls back to the type name itself.
    std::vector<std::string> base_materials;
};

// Adhesion rules for a single base material, expressed against other base materials.
// Centralising the lists here avoids repeating them on every type/variant.
struct BaseMaterialCompatibility {
    std::string base_material;
    // Base materials this one is known to bond/adhere with (beyond itself).
    std::vector<std::string> compatible;
    // Base materials this one is known not to bond/adhere with.
    std::vector<std::string> incompatible;
};

class MaterialType {
public:
    static const std::vector<MaterialTypeInfo>& all();
    static const std::vector<BaseMaterialCompatibility>& base_compatibilities();

    static const MaterialTypeInfo* find(const std::string& name);

    static bool get_temperature_range(const std::string& type, int& min_temp, int& max_temp);
    static bool get_chamber_temperature_range(const std::string& type, int& chamber_min_temp, int& chamber_max_temp);
    static bool get_adhesion_coefficient(const std::string& type, double& adhesion_coefficient);
    static bool get_yield_strength(const std::string& type, double& yield_strength);
    static bool get_thermal_length(const std::string& type, double& thermal_length);

    // Material families of the given type ("PLA-CF" -> {"PLA"}, "PC-ABS" -> {"PC", "ABS"}).
    // Falls back to {type} when no families are configured.
    static std::vector<std::string> base_materials(const std::string& type);

    // Compatibility (adhesion) between two material types, resolved at the base-material level:
    //   Incompatible - some base of one is listed incompatible with a base of the other;
    //   Compatible   - the two share a base material, or some base of one is listed compatible
    //                  with a base of the other;
    //   Unknown      - none of the above.
    static MaterialCompatibility compatibility(const std::string& type_a, const std::string& type_b);
};

} // namespace Slic3r

#ifndef slic3r_MakerbotTranslator_hpp_
#define slic3r_MakerbotTranslator_hpp_

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r {

// Forward Declaration
class DynamicPrintConfig;

class MakerbotTranslator {
private:
    nlohmann::json toolpath_array;
    double current_a = 0.0;
    double last_slicer_e = 0.0;
    bool is_relative_e = false;

public:
    MakerbotTranslator();
    ~MakerbotTranslator() = default;

    // Track extrusion coordination strategies (M82/M83 tracking)
    void set_relative_extrusion(bool is_relative);

    // Convert standard motion directives into structured JSON commands
    void translate_move(double x, double y, double z, double e, double f_min, const std::string& feature_tag);

    // Append localized hardware events (heating matrices, fan speed arrays)
    void translate_custom_command(const std::string& function_name, const nlohmann::json& parameters);

    // Build the structural meta.json dictionary, resolving dynamic z-offsets
    nlohmann::json generate_meta(const DynamicPrintConfig& config, const std::string& bot_type) const;

    // Access raw serialization pipeline
    const nlohmann::json& get_toolpath() const;

    // Reset layout registers
    void clear();
};

} // namespace Slic3r

#endif // slic3r_MakerbotTranslator_hpp_
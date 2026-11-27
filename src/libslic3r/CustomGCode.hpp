#ifndef slic3r_CustomGCode_hpp_
#define slic3r_CustomGCode_hpp_

#include <map>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace Slic3r {

class DynamicPrintConfig;

namespace CustomGCode {

enum Type
{
    ColorChange,
    PausePrint,
    ToolChange,
    Template,
    Custom,
    Unknown,
};

struct Item
{
    bool operator<(const Item& rhs) const { return this->print_z < rhs.print_z; }
    bool operator==(const Item& rhs) const
    {
        return (rhs.print_z   == this->print_z    ) &&
               (rhs.type      == this->type       ) &&
               (rhs.extruder  == this->extruder   ) &&
               (rhs.color     == this->color      ) &&
               (rhs.extra     == this->extra      );
    }
    bool operator!=(const Item& rhs) const { return ! (*this == rhs); }
    
    double      print_z;
    Type        type;
    int         extruder;   // Informative value for ColorChangeCode and ToolChangeCode
                            // "gcode" == ColorChangeCode   => M600 will be applied for "extruder" extruder
                            // "gcode" == ToolChangeCode    => for whole print tool will be switched to "extruder" extruder
    std::string color;      // if gcode is equal to PausePrintCode, 
                            // this field is used for save a short message shown on Printer display 
    std::string extra;      // this field is used for the extra data like :
                            // - G-code text for the Type::Custom 
                            // - message text for the Type::PausePrint
    void from_json(const nlohmann::json& j);
};

enum Mode
{
    Undef,
    SingleExtruder,   // Single extruder printer preset is selected
    MultiAsSingle,    // Multiple extruder printer preset is selected, but 
                      // this mode works just for Single extruder print 
                      // (The same extruder is assigned to all ModelObjects and ModelVolumes).
    MultiExtruder     // Multiple extruder printer preset is selected
};

// string anlogue of custom_code_per_height mode
static constexpr char SingleExtruderMode[] = "SingleExtruder";
static constexpr char MultiAsSingleMode [] = "MultiAsSingle";
static constexpr char MultiExtruderMode [] = "MultiExtruder";

struct Info
{
    Mode mode = Undef;
    std::vector<Item> gcodes;

    bool operator==(const Info& rhs) const
    {
        return  (rhs.mode   == this->mode   ) &&
                (rhs.gcodes == this->gcodes );
    }
    bool operator!=(const Info& rhs) const { return !(*this == rhs); }

    void from_json(const nlohmann::json& j);
};

// If loaded configuration has a "colorprint_heights" option (if it was imported from older Slicer), 
// and if CustomGCode::Info.gcodes is empty (there is no color print data available in a new format
// then CustomGCode::Info.gcodes should be updated considering this option.
//BBS
//extern void update_custom_gcode_per_print_z_from_config(Info& info, DynamicPrintConfig* config);

// If information for custom Gcode per print Z was imported from older Slicer, mode will be undefined.
// So, we should set CustomGCode::Info.mode should be updated considering code values from items.
extern void check_mode_for_custom_gcode_per_print_z(Info& info);

// Return pairs of <print_z, 1-based extruder ID> sorted by increasing print_z from custom_gcode_per_print_z.
// print_z corresponds to the first layer printed with the new extruder.
std::vector<std::pair<double, unsigned int>> custom_tool_changes(const Info& custom_gcode_per_print_z, size_t num_extruders);

} // namespace CustomGCode

} // namespace Slic3r



#endif /* slic3r_CustomGCode_hpp_ */

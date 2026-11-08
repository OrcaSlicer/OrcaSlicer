#ifndef slic3r_GCode_StartGCodeTemperature_hpp_
#define slic3r_GCode_StartGCodeTemperature_hpp_

#include <optional>
#include <string>

namespace Slic3r {

// Checks for a printer start G-code that heats the nozzle or the bed to a hard-coded
// temperature instead of using the filament's temperature placeholders.
//
// When the start G-code contains any M104/M109 (or M140/M190) the slicer does not emit its
// own heat-up, so whatever the start G-code sets stays in effect until the slicer next
// changes the temperature. A literal such as "M109 S205" therefore silently prints a
// 260 degree filament at 205. These helpers detect that case so the slicer can warn.

// G-code family describing a temperature-setting command.
struct TemperatureCommandFamily
{
    int  mcode_no_wait;  // M104 or M140
    int  mcode_wait;     // M109 or M190
    bool include_g10;    // RepRapFirmware G10 S<temp> (nozzle only)
};

inline TemperatureCommandFamily nozzle_temperature_commands(bool include_g10) { return { 104, 109, include_g10 }; }
inline TemperatureCommandFamily bed_temperature_commands() { return { 140, 190, false }; }

// Temperature set by the last matching command in `gcode` that carries a numeric
// temperature. Tool words are ignored, matching the rule the slicer uses to decide that
// the start G-code handles heating. Returns std::nullopt when there is none.
std::optional<int> last_gcode_temperature(const std::string &gcode, const TemperatureCommandFamily &family);

// True when the last matching command with a temperature argument in `raw_template` (the
// G-code before placeholder processing) writes that argument as the plain number `value`,
// as opposed to a placeholder or expression.
bool gcode_template_last_temperature_is_literal(const std::string &raw_template, const TemperatureCommandFamily &family, int value);

// True when `raw_template` contains any matching command with a temperature argument,
// literal or placeholder.
bool gcode_template_sets_temperature(const std::string &raw_template, const TemperatureCommandFamily &family);

// Temperatures further apart than this are reported.
constexpr int START_GCODE_TEMPERATURE_TOLERANCE = 10;

// Compares the temperature in effect at the end of the processed start G-code with the
// temperature the slicer would have used. Returns the start G-code's temperature when the
// last temperature command of the template is a literal and that temperature differs from
// `expected` by more than START_GCODE_TEMPERATURE_TOLERANCE; std::nullopt otherwise. A
// temperature produced by a placeholder or expression is assumed to be intentional and is
// never reported, nor is a literal 0 (heater off, left to a firmware start macro).
std::optional<int> start_gcode_literal_temperature_mismatch(const std::string              &raw_template,
                                                            const std::string              &processed_gcode,
                                                            const TemperatureCommandFamily &family,
                                                            int                             expected);

// User-facing warning texts. `sequential` is true when objects are printed one at a time
// (by object or by group).
std::string start_gcode_nozzle_temperature_warning(int gcode_temp, int expected, const std::string &filament_name, bool sequential);
std::string start_gcode_bed_temperature_warning(int gcode_temp, int expected);

} // namespace Slic3r

#endif // slic3r_GCode_StartGCodeTemperature_hpp_

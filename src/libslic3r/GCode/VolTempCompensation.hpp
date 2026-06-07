// VolTempCompensation.hpp
// OrcaSlicer
//
// Volumetric Temperature Compensation (VTC).
// Concept & original idea: Nadir @ CN3D (n.nadir@gmail.com). License: MIT.
//
// Predictive nozzle temperature control based on *future* volumetric flow.
// After the G-code for a plate has been generated (and after the cooling
// buffer has applied its minimum-layer-time slow-downs, which are precisely
// what create the flow transitions VTC targets), this pass reconstructs the
// per-move wall-clock timeline and volumetric flow, maps flow to a target
// temperature, and injects predictive, non-blocking M104 commands ahead of
// each flow transition so the hotend reaches the right temperature exactly
// when the new flow condition begins.
//
// The temperature lead time uses an asymmetric thermal model with three
// rates (active heating, passive cooling, and the faster cooling that occurs
// while cold filament is being extruded through the melt zone). Large
// transitions are split into small micro-steps so the firmware PID tracks a
// moving target smoothly instead of overshooting on one big jump.
//
// This is a whole-buffer, in-process post-processing stage. It never emits
// M109 (the print must not pause) and clamps every setpoint to a safety band
// around the filament's nominal temperature.

#pragma once

#include <string>
#include <vector>

namespace Slic3r {

class PrintConfig;

class VolTempCompensation
{
public:
    // True if at least one of the given extruders has VTC enabled. Cheap guard
    // so the caller can skip the file round-trip entirely when the feature is off.
    static bool any_enabled(const PrintConfig &config, const std::vector<unsigned int> &extruders);

    // Read `path`, run process_gcode(), and write the result back to `path`.
    // No-op (leaves the file untouched) if no used extruder has VTC enabled.
    static void process_file(const std::string &path,
                             const PrintConfig &config,
                             const std::vector<unsigned int> &extruders);

    // Pure string transform: parse -> reconstruct timeline & flow -> schedule
    // predictive M104 commands -> inject. Exposed for unit testing without I/O.
    static std::string process_gcode(const std::string &gcode,
                                     const PrintConfig &config,
                                     const std::vector<unsigned int> &extruders);
};

} // namespace Slic3r

#ifndef slic3r_GCode_TrunnionAC_hpp_
#define slic3r_GCode_TrunnionAC_hpp_

// UNLAYERED_MOD -- native G-code output for an A/C table-table ("trunnion") 5-axis printer.
//
// The machine: a fixed vertical nozzle moving in X/Y/Z, and the part on a C turntable (about the bed
// normal) carried by an A cradle (about machine X). The A axis lies `pivot_height` above the bed
// surface, so tilting A also moves the part in Y and Z. The firmware knows nothing about this: A and
// C are plain axes, and every X/Y/Z word has to be a MACHINE position.
//
// The rest of the slicer keeps working in the bed (workpiece) frame, exactly as for the B/C machines
// where RepRapFirmware does the compensation: X/Y/Z are the tool tip relative to the part, plus a
// "; TOOL_AXIS:x,y,z" tag per orientation change and B/C staging moves. The G-code preview and the
// hotend collision check read that file. This pass runs on the EXPORTED copy only (next to the
// post-processing scripts), and turns it into machine output:
//
//   * per section, the tool axis n becomes an (A, C) pair: Rx(A) * Rz(C) * n = +Z, i.e.
//     n = (sinA sinC, sinA cosC, cosA). Of the two solutions (A, C) and (-A, C + 180) the one the
//     machine's policy and A limits allow is taken; C is unwound toward the previous value;
//   * every X/Y/Z is transformed m = Rx(A) * (Rz(C) * (p - o) + o - h) + h, with h = (0, 0, pivot
//     height) and o the C axis position, both relative to the bed centre (the workpiece origin);
//   * the slicer's B/C staging is dropped and every orientation change becomes: lift straight up to
//     a machine Z that clears everything the swing can raise (the printed part and the table, sampled
//     along the interpolated A/C path), rotate, travel over the next start point, descend. Both the
//     lift and the descent are checked against the hotend envelope;
//   * arcs stay arcs while A = 0 (I/J are turned by C) and are split into lines otherwise.
//
// At A = C = 0 workpiece and machine coordinates coincide, so start G-code passes through untouched.
// A G-code line carrying an A word is taken as machine-native custom G-code and passed through
// verbatim (its A/C become the current orientation).

#include <array>
#include <iosfwd>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

namespace TrunnionAC {

enum class Solution { SmallestMove, NegativeA, PositiveA };

// UNLAYERED_MOD 0.1.4 -- the second rotary axis. AC: a C turntable on an A cradle (about X), the
// machine this module was written for. AB: a ROUGH GUESS at a bed on three independent Z leadscrews
// ("tri-Z"), tilted about X (A) and Y (B) around a pivot in the bed plane at the leadscrews' centre;
// the firmware turns A/B into leadscrew heights. Nobody has run its G-code on a printer yet. For AB,
// Orientation::c holds B and c_continuous/c_min/c_max are B's.
enum class Kind { AC, AB };

struct Machine
{
    Kind     kind = Kind::AC;
    double   pivot_height = 0.;                 // A axis above the bed surface, mm
    double   c_offset_x = 0., c_offset_y = 0.;  // C axis relative to the bed centre at A = C = 0, mm
    double   origin_x = 0., origin_y = 0.;      // G-code X/Y of the bed centre
    double   a_min = -90., a_max = 90.;         // degrees
    bool     c_continuous = true;               // endless C: unwind toward the previous value, no limits
    double   c_min = -360., c_max = 360.;       // degrees, when not continuous
    Solution solution = Solution::SmallestMove;

    double   table_radius = 110.;   // the turntable, swept with the part during a swing, mm
    double   clearance = 5.;        // kept between the nozzle tip and the highest swept point, mm
    double   max_z = 0.;            // highest machine Z a transition may lift to; 0 = no limit
    double   travel_feed = 6000.;   // mm/min
    double   z_feed = 600.;         // mm/min
    double   rotary_feed = 1800.;   // deg/min

    // Hotend envelope (HotendEnvelope in PrintConfig.hpp): pyramid faces +X, -X, +Y, -Y in the machine
    // frame, clipped by a sphere of radius env_height about the tip. 0 height = no lift/descent check.
    double                env_height = 0.;
    std::array<double, 4> env_spread { 1., 1., 1., 1. };
    double                env_margin = 0.5;
    double                env_eps    = 0.3;
};

struct Vec3
{
    double x = 0., y = 0., z = 0.;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
};

struct Orientation
{
    double a = 0.;   // degrees
    double c = 0.;   // degrees, as emitted (unwound, may lie outside -180..180)
};

// Row-major 3x3: Rx(A) * Rz(C). Its rows are machine X, Y, Z expressed in the workpiece frame, so it
// is also the head's frame in workpiece coordinates (tool_head_frame() for a B/C machine).
std::array<double, 9> rotation(const Orientation &o);              // Rx(A) * Rz(C), the A/C machine
std::array<double, 9> rotation(const Machine &m, const Orientation &o); // per m.kind (AB: Rx(A) * Ry(B))

// The (A, C) that points tool axis `n` (workpiece frame, unit, surface -> nozzle) straight up, chosen
// relative to `prev` per the machine's policy and limits. False when no solution lies in the limits.
bool solve(const Machine &m, const Vec3 &n, const Orientation &prev, Orientation &out);

// The orientation sequence the exporter will pick for a sequence of tool axes, starting from A = C = 0.
// Used by the hotend collision check so its head frames match the exported file. An axis outside the
// limits keeps the previous orientation (the export will refuse it anyway).
std::vector<Orientation> solve_sequence(const Machine &m, const std::vector<Vec3> &axes);

Vec3 to_machine(const Machine &m, const Orientation &o, const Vec3 &p_gcode);
Vec3 to_gcode(const Machine &m, const Orientation &o, const Vec3 &p_machine);

struct Report
{
    size_t lines_in      = 0;
    size_t transitions   = 0;
    double highest_lift  = 0.;
    size_t arcs_split    = 0;
};

// Throws std::runtime_error naming the input line when the file cannot be converted safely (an axis
// outside the A limits, a lift above max_z, the hotend envelope hitting the part on a lift or descent).
Report process(std::istream &in, std::ostream &out, const Machine &m);

} // namespace TrunnionAC

// True when the printer is an A/C machine whose G-code needs this pass.
bool trunnion_ac_enabled(const DynamicPrintConfig &config);
TrunnionAC::Machine trunnion_ac_machine(const DynamicPrintConfig &config);
// Converts the G-code file at `path` in place when trunnion_ac_enabled(). Returns whether it did.
// Throws Slic3r::RuntimeError on failure, leaving the file untouched.
bool trunnion_ac_process_file(const std::string &path, const DynamicPrintConfig &config);

} // namespace Slic3r

#endif // slic3r_GCode_TrunnionAC_hpp_

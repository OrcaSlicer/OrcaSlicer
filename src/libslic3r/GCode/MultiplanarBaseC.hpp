#ifndef slic3r_GCode_MultiplanarBaseC_hpp_
#define slic3r_GCode_MultiplanarBaseC_hpp_

// UNLAYERED_MOD -- multiplanar (cut-plane) prints on a B/C machine: print the planar base at the C
// of the first angled chunk instead of at C = 0.
//
// Before this, a multiplanar group printed its base (the root segment, and any other chunk built
// straight up) at the rotary home B-90 C0, then swung C to the first angled chunk's azimuth at the
// first segment change -- up to 180 degrees of table rotation between two pieces that have to meet.
// With a vertical tool C is free (tool_axis_to_bc() reports 0 only because the azimuth of a vertical
// axis is undefined), so the base can be printed with the table already at that chunk's C: the
// first segment change then only tilts B, and the base and the chunk resting on it are laid down
// without the C axis moving in between.
//
// Coordinate convention (the one the 5-axis writer already follows, nothing new): on a B/C machine
// RepRapFirmware performs the 5-axis compensation. Every X/Y/Z the slicer writes is the nozzle tip in
// the BED frame -- the frame that turns with the table -- and B/C only tell the firmware the table
// orientation (GCode::chunk_transition_gcode(), the TrunnionAC.hpp header, the G-code viewer, which
// reads X/Y as bed coordinates whatever C says). A tilted chunk's toolpaths are mapped into that bed
// frame by post_trafo_inverse() and are never turned by C in the slicer. The base follows the same
// rule: its X/Y words are left exactly as they are and only its C word changes. The firmware turns
// them into machine X/Y = pivot + Rz(-C) * (bed - pivot) (bed_to_machine_xy() below; the same sign
// chunk_transition_gcode()'s move 2 relies on), which is the counter-rotation that keeps the printed
// geometry on the table unchanged. Turning the base toolpaths in the slicer as well would apply it
// twice and print the base rotated by C against everything that rests on it.
//
// What stays where it was: the start G-code (including a printer-fixed purge line) and the
// between-object G-code run at home, before the table turns -- the turn is a single in-place
// "G1 B-90 C<c>" written right before the group's first layer. Skirt, brim, travels, wipes and
// retracts of the base are bed-frame moves like its extrusions, so they turn with the part.
//
// Only the slicer's own B/C output (axis_selection = B/C) is affected. A/C and A/B output is turned
// into machine coordinates by GCode/TrunnionAC.cpp, which solves its own orientation from the tool
// axis (and keeps C where it was for a vertical one), so it is left alone; virtual 5-axis output has
// no rotary words at all; belt printers are left alone too.

#include "../Point.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace MultiplanarBaseC {

// A vertical tool axis pointing up: the base orientation, B = -90 with C free. The same threshold
// tool_axis_to_bc() uses to decide that the azimuth is undefined.
bool is_vertical(const Vec3d &tool_axis);

// One member visit of a By-group group: the tool axis it prints along (bed frame, unit; the same
// vector trafo_to_bc() in GCode.cpp is given) and whether it is a cut-plane segment.
struct Member
{
    Vec3d tool_axis { Vec3d::UnitZ() };
    bool  is_segment { false };
};

struct Choice
{
    // The C (radians, tool_axis_to_bc() convention) the base is to be printed at: the first angled
    // chunk's. Empty = keep today's behaviour (C = 0 for the base); `reason` says why.
    std::optional<double> c_rad;
    // The chosen first angled chunk, as indices into the `layers` argument; -1 when there is none.
    int layer  { -1 };
    int member { -1 };
    // Another angled member of the same layer wants a different C. The first one in the order given
    // was taken; the machine still swings C if the printer visits the other one first.
    bool ambiguous { false };
    std::string reason;
};

// `layers`: a group's layers in print order, each listing the members that print in it, in the order
// the group lists its instances. The first angled chunk is the first member, in that order, whose
// tool axis is not vertical. It must be a cut-plane segment (otherwise this is not a multiplanar
// base: an ordinary object with its own B/C), and something vertical must print before it.
Choice choose(const std::vector<std::vector<Member>> &layers);

// The C-axis convention between the bed frame the G-code is written in and the machine's X/Y. The C
// axis pierces `pivot` (the bed centre, as chunk_transition_gcode() has it) and turning the table by
// C turns the bed frame by +C, so with the table at C a bed point lies under machine
//     m = pivot + Rz(-C) * (bed - pivot)
// and back, bed = pivot + Rz(C) * (m - pivot). Angles in radians.
Vec2d bed_to_machine_xy(const Vec2d &bed, const Vec2d &pivot, double c_rad);
Vec2d machine_to_bed_xy(const Vec2d &machine, const Vec2d &pivot, double c_rad);

// Do the bed-frame points `footprint` (printer coordinates, plate offset removed), turned with the
// table to C, all stay inside `area` (printable_area, printer coordinates)? A point within
// `tolerance` mm of the boundary counts as inside: it absorbs the polygon approximating a round bed,
// whose vertices a turned point can otherwise miss by a fraction of a millimetre. An empty footprint
// fits. On failure `worst`, when given, receives the machine X/Y of the point furthest outside.
bool fits_at_c(const std::vector<Vec2d> &footprint, const std::vector<Vec2d> &area, const Vec2d &pivot,
               double c_rad, double tolerance = 0.1, Vec2d *worst = nullptr);

} // namespace MultiplanarBaseC
} // namespace Slic3r

#endif // slic3r_GCode_MultiplanarBaseC_hpp_

#ifndef slic3r_BeltKinematics_hpp_
#define slic3r_BeltKinematics_hpp_

#include "MachineKinematics.hpp"
#include "MachineFrameTransform.hpp"

namespace Slic3r {

class PrintConfig;
class GCodeWriter;

// Belt-printer machine frame (BELT_MOD, ported from OrcaSlicer's BeltKinematics).
//
// Forward order, as applied per emitted point:
//     machine = MachineFrameTransform( axis_remap( bed ) )
//
// The point handed in is already in the bed frame: on this fork the per-object slicing
// rotation is undone per move by GCodeWriter's segment trafo (PrintObject::post_trafo_inverse),
// so Orca's BeltBackTransform stage has no counterpart here. What remains is the machine's own
// mapping: the conventional belt axis swap (gcode_remap_*) followed by the shear/scale that
// turns bed coordinates into the tilted-gantry machine axes. That is exactly Orca's
// world_coordinates mode, so every writer here runs in it (D-006).
class BeltKinematics : public CartesianKinematics
{
public:
    // world_coordinates is Orca's signature; this fork never back-transforms (see above), so both
    // values give the same map.
    explicit BeltKinematics(const PrintConfig &config, bool world_coordinates = true);

    Vec3d to_machine(const Vec3d &p) const override;
    Vec3d to_logical(const Vec3d &machine) const override;
    // Machine -> build-volume frame. Only the machine-frame shear/scale is undone, matching what
    // GCodeProcessor's bounds validation wants. This is deliberately NOT to_logical().
    Vec3d to_build_volume(const Vec3d &machine) const override
        { return m_machine_frame.apply_inverse(machine); }

    // A belt writer always emits full XYZ on every move, whether or not any individual stage
    // reports itself active, so an identity-transform belt configuration still gets the same
    // word structure as a tilted one.
    bool must_emit_all_axes() const override { return true; }
    bool suppress_lift_at_unknown_position() const override { return true; }
    // The machine frame shears and scales, so a circle is an ellipse in machine coordinates
    // and G2/G3 cannot describe it.
    bool supports_arc_moves() const override { return false; }

    const MachineFrameTransform& machine_frame() const { return m_machine_frame; }

private:
    MachineFrameTransform m_machine_frame;
};

// Install a belt machine frame on any GCodeWriter. Any axis remap and build volume already
// configured on the writer are carried over, so this may be called before or after those setters.
void install_belt_kinematics(GCodeWriter &writer, const PrintConfig &config, bool world_coordinates = true);

} // namespace Slic3r

#endif // slic3r_BeltKinematics_hpp_

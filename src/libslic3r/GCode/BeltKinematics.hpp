#ifndef slic3r_BeltKinematics_hpp_
#define slic3r_BeltKinematics_hpp_

#include "MachineKinematics.hpp"
#include "BeltBackTransform.hpp"
#include "MachineFrameTransform.hpp"
#include "../Point.hpp"

namespace Slic3r {

class PrintConfig;
class GCodeWriter;

// Belt-printer machine frame.
//
// Forward order, as applied per emitted point:
//     machine = MachineFrameTransform( axis_remap( BeltBackTransform( logical ) ) )
//
// i.e. the slicer->world back-transform runs FIRST and the machine-frame
// shear/scale LAST, so the latter acts as a global linear transform on the
// already-placed coordinates.
//
// world_coordinates mode (the PA line / PA pattern calibration generators)
// treats the incoming point as already relative to the belt surface -- X across,
// Y along the belt, Z above it -- and therefore skips the back-transform while
// keeping the remap and the machine frame. It is a different coordinate map, not
// a writer mode, which is why it is fixed at construction.
class BeltKinematics : public CartesianKinematics
{
public:
    explicit BeltKinematics(const PrintConfig &config, bool world_coordinates = false);

    Vec3d to_machine(const Vec3d &p) const override;

    // A belt writer has always emitted full XYZ on every move, whether or not any
    // individual stage reports itself active. Making this conditional would change
    // emitted G-code for an identity-transform belt configuration.
    bool must_emit_all_axes() const override { return true; }
    bool suppress_lift_at_unknown_position() const override { return true; }
    // The machine frame shears and scales, so a circle is an ellipse in machine
    // coordinates and G2/G3 cannot describe it.
    bool supports_arc_moves() const override { return false; }

private:
    BeltBackTransform     m_back_transform;
    MachineFrameTransform m_machine_frame;
    bool                  m_world_coordinates { false };
};

// Install a belt machine frame on any GCodeWriter. Any axis remap and build
// volume already configured on the writer are carried over, so this may be
// called before or after those setters. Re-calling it with a different
// world_coordinates value swaps the map (used around the PA line generator).
void install_belt_kinematics(GCodeWriter &writer, const PrintConfig &config,
                             bool world_coordinates = false);

} // namespace Slic3r

#endif // slic3r_BeltKinematics_hpp_

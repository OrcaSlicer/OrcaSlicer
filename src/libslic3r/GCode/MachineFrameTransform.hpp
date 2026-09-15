#ifndef slic3r_MachineFrameTransform_hpp_
#define slic3r_MachineFrameTransform_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

// Post-stage machine-frame transform for belt printers.
//
// Applied in BeltGCodeWriter::to_machine_coords AFTER the back-transform and
// the gcode_remap_* axis remap.  Maps Cartesian (axis-permuted) G-code
// coordinates into the printer's physical machine frame.
//
// Derived entirely from the single belt tilt (belt_slice_rotation axis +
// belt_slice_rotation_angle): a shear coupling the height axis to the belt-feed
// axis (factor cot a) plus a 1/sin a scale on the gantry-height axis.  The expert
// belt_frame_tilt_decouple flag lets the machine-frame angle differ from the
// pre-slice rotation angle via belt_frame_tilt_angle.
class MachineFrameTransform
{
public:
    MachineFrameTransform() = default;

    // Initialize from belt printer config.  Returns true if a non-identity
    // transform was computed.  Inactive when belt_printer is disabled or
    // both shear and scale are identity.
    bool init_from_config(const PrintConfig &config);

    // Apply the transform to a point.  Returns pos unchanged if not active.
    Vec3d apply(const Vec3d &pos) const;

    // Apply the inverse transform.  Returns pos unchanged if not active.
    // Used by validators that need to compare emitted machine-frame
    // coordinates against build-volume bounds.
    Vec3d apply_inverse(const Vec3d &pos) const;

    bool is_active() const { return m_active; }

    // The composed shear*scale transform (identity when inactive). Exposed so the
    // G-code viewer can build the machine->model back-transform for the upright
    // ("designed") belt preview.
    const Transform3d& transform() const { return m_transform; }

private:
    bool        m_active            = false;
    Transform3d m_transform         = Transform3d::Identity();
    Transform3d m_transform_inverse = Transform3d::Identity();
};

} // namespace Slic3r

#endif // slic3r_MachineFrameTransform_hpp_

#ifndef slic3r_BeltBackTransform_hpp_
#define slic3r_BeltBackTransform_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

// Reverses the pre-slice remap + shear + scale transforms that
// PrintObjectSlice.cpp applies to belt printer geometry, converting G-code
// coordinates from the sliced (remapped/sheared/scaled) frame back to the
// machine's real coordinate space.
//
// Initialized once from PrintConfig, then applied per-point in
// GCodeWriter::to_machine_coords() before axis remapping.
//
// Active when gcode_back_transform is true AND at least one of:
//   - a shear axis has global mode enabled, or
//   - a pre-slice axis remap is non-identity.
class BeltBackTransform
{
public:
    BeltBackTransform() = default;

    // Initialize from belt printer config.  Rebuilds the same pre-slice remap,
    // shear, and scale matrices as PrintObjectSlice.cpp and precomputes the
    // affine inverse.  Returns true if a non-identity back-transform was computed.
    bool init_from_config(const PrintConfig &config);

    // Apply the inverse transform to a point.  Returns pos unchanged if
    // no back-transform is active.
    Vec3d apply(const Vec3d &pos) const;

    // True if a non-identity back-transform is active.
    bool is_active() const { return m_active; }

private:
    bool       m_active  = false;
    Transform3d m_inverse = Transform3d::Identity();
};

} // namespace Slic3r

#endif // slic3r_BeltBackTransform_hpp_

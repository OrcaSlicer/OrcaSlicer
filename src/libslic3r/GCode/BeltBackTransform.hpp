#ifndef slic3r_BeltBackTransform_hpp_
#define slic3r_BeltBackTransform_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

// Reverses the pre-slice rotation that PrintObjectSlice.cpp applies to belt
// printer geometry, converting G-code coordinates from the sliced (rotated)
// frame back to the machine's real coordinate space.
//
// Initialized once from PrintConfig, then applied per-point in
// BeltKinematics::to_machine() before axis remapping.
//
// Active on belt printers with a non-identity pre-slice rotation.
class BeltBackTransform
{
public:
    BeltBackTransform() = default;

    // Initialize from belt printer config.  Rebuilds the same pre-slice rotation
    // as PrintObjectSlice.cpp and precomputes the affine inverse.  Returns true if a non-identity back-transform was computed.
    bool init_from_config(const PrintConfig &config);

    // Apply the inverse transform to a point.  Returns pos unchanged if
    // no back-transform is active.
    Vec3d apply(const Vec3d &pos) const;

private:
    bool       m_active  = false;
    Transform3d m_inverse = Transform3d::Identity();
};

} // namespace Slic3r

#endif // slic3r_BeltBackTransform_hpp_

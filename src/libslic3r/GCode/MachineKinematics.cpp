#include "MachineKinematics.hpp"

namespace Slic3r {

// Moved verbatim from GCodeWriter::apply_axis_remap().
Vec3d CartesianKinematics::apply_axis_remap(const Vec3d &pos) const
{
    if (!has_axis_remap())
        return pos;
    auto remap = [this, &pos](int r) -> double {
        int axis = r % 3;
        if (r < 3) return pos[axis];
        if (r < 6) return -pos[axis];
        return m_build_vol_max[axis] - pos[axis];
    };
    return { remap(m_remap_x), remap(m_remap_y), remap(m_remap_z) };
}

// Inverse of the above.  Output axis i is fed by source axis (r_i % 3); walking
// the three outputs therefore fills every source component exactly once, so long
// as the remap is a permutation (which set_axis_remap callers guarantee).
Vec3d CartesianKinematics::apply_axis_remap_inverse(const Vec3d &machine) const
{
    if (!has_axis_remap())
        return machine;
    Vec3d out = Vec3d::Zero();
    const int r[3] = { m_remap_x, m_remap_y, m_remap_z };
    for (int i = 0; i < 3; ++i) {
        const int axis = r[i] % 3;
        if (r[i] < 3)      out[axis] = machine[i];
        else if (r[i] < 6) out[axis] = -machine[i];
        else               out[axis] = m_build_vol_max[axis] - machine[i];
    }
    return out;
}

Vec3d CartesianKinematics::to_machine(const Vec3d &p) const
{
    return this->apply_axis_remap(p);
}

Vec3d CartesianKinematics::to_logical(const Vec3d &machine) const
{
    return this->apply_axis_remap_inverse(machine);
}

} // namespace Slic3r

#include "MachineKinematics.hpp"
#include "../Point.hpp"

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

Vec3d CartesianKinematics::to_machine(const Vec3d &p) const
{
    return this->apply_axis_remap(p);
}

} // namespace Slic3r

#include "BeltKinematics.hpp"
#include "../PrintConfig.hpp"
#include "../GCodeWriter.hpp"

namespace Slic3r {

BeltKinematics::BeltKinematics(const PrintConfig &config, bool /* world_coordinates */)
{
    m_machine_frame.init_from_config(config);
}

Vec3d BeltKinematics::to_machine(const Vec3d &p) const
{
    return m_machine_frame.apply(this->apply_axis_remap(p));
}

Vec3d BeltKinematics::to_logical(const Vec3d &machine) const
{
    return this->apply_axis_remap_inverse(m_machine_frame.apply_inverse(machine));
}

void install_belt_kinematics(GCodeWriter &writer, const PrintConfig &config, bool /* world_coordinates */)
{
    writer.set_kinematics(std::make_unique<BeltKinematics>(config));
}

} // namespace Slic3r

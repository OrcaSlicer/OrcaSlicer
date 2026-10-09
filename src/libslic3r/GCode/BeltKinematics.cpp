#include "BeltKinematics.hpp"
#include "../BeltTransform.hpp"
#include "../PrintConfig.hpp"
#include "../GCodeWriter.hpp"
#include "../Point.hpp"
#include <memory>

namespace Slic3r {

BeltKinematics::BeltKinematics(const PrintConfig &config, bool world_coordinates)
    : m_world_coordinates(world_coordinates)
{
    m_back_transform.init_from_config(config);
    m_machine_frame.init_from_config(config);
}

Vec3d BeltKinematics::to_machine(const Vec3d &p) const
{
    const Vec3d after_back  = m_world_coordinates ? p : m_back_transform.apply(p);
    const Vec3d after_remap = this->apply_axis_remap(after_back);
    return m_machine_frame.apply(after_remap);
}

void install_belt_kinematics(GCodeWriter &writer, const PrintConfig &config, bool world_coordinates)
{
    writer.set_kinematics(std::make_unique<BeltKinematics>(config, world_coordinates));
}

} // namespace Slic3r

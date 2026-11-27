#include "BeltBackTransform.hpp"
#include "../BeltTransform.hpp"
#include "../Point.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

bool BeltBackTransform::init_from_config(const PrintConfig &config)
{
    m_active  = false;
    m_inverse = Transform3d::Identity();

    if (!config.belt_printer.value)
        return false;

    // Build the forward pipeline (the rotation) and store its inverse.
    Transform3d forward = BeltTransformPipeline::build_forward_transform(config);
    if (forward.isApprox(Transform3d::Identity()))
        return false;

    m_inverse = forward.inverse();
    m_active  = true;
    return true;
}

Vec3d BeltBackTransform::apply(const Vec3d &pos) const
{
    if (!m_active)
        return pos;
    return m_inverse * pos;
}

} // namespace Slic3r

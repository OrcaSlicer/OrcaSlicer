#include "SupportDirection.hpp"

#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

Transform3d SupportLean::shear() const
{
    Transform3d t = Transform3d::Identity();
    t.linear()(0, 2) = - slope.x();
    t.linear()(1, 2) = - slope.y();
    return t;
}

SupportLean support_lean(const PrintObject &object)
{
    SupportLean              lean;
    const PrintObjectConfig &cfg = object.config();
    Vec3d                    u_world;
    switch (cfg.support_direction.value) {
    case sdVertical: u_world = Vec3d::UnitZ(); break;
    case sdCustom:   u_world = Geometry::build_normal_from_bc(cfg.support_direction_b.value, cfg.support_direction_c.value); break;
    case sdLayers:
    default:         return lean;
    }

    // World direction -> slice frame. post_trafo() is R * trafo() with R the slicing rotation
    // (PrintObject::update_slice_frame), so R = post_trafo * trafo^-1; the instance's scale and
    // mirror cancel out and only the pure rotation is left. A direction (not a normal) maps
    // through the linear part itself.
    const Eigen::Matrix3d R = (object.post_trafo() * object.trafo().inverse()).linear();
    Vec3d                 u = R * u_world;
    if (u.squaredNorm() < 1e-18)
        return lean;
    u.normalize();
    // The direction names a LINE for the column, not which way along it: support always grows
    // towards +Z of the slice frame, from the floor up. A segment printing a downward-facing cut
    // face maps world-up below its horizon; the same column line runs the other way there.
    if (u.z() < 0.)
        u = - u;

    const double max_lean = std::clamp(cfg.support_direction_max_lean.value, 0., 85.);
    const double max_cos  = std::cos(Geometry::deg2rad(max_lean));
    if (u.z() < max_cos - EPSILON) {
        lean.clamped = true;
        const double rxy = std::hypot(u.x(), u.y());
        const double s   = std::sin(Geometry::deg2rad(max_lean));
        u = Vec3d(u.x() / rxy * s, u.y() / rxy * s, max_cos);
    }
    lean.slope = Vec2d(u.x() / u.z(), u.y() / u.z());
    if (! lean.active())
        lean.slope = Vec2d(0., 0.);
    return lean;
}

static void translate_polygons(Polygons &polygons, const Point &shift)
{
    for (Polygon &p : polygons)
        p.translate(shift);
}

ScopedSupportShear::ScopedSupportShear(PrintObject &object)
    : m_object(object), m_lean(support_lean(object))
{
    if (! m_lean.active())
        return;
    m_object.m_support_lean = m_lean;
    for (Layer *layer : m_object.m_layers)
        layer->translate(m_lean.shift_at(layer->print_z));
    // Demand is indexed 1:1 with layers(); an entry past the end has no layer and is never read.
    for (std::vector<Polygons> *demand : { &m_object.m_segment_support_demand, &m_object.m_segment_body_demand })
        for (size_t i = 0; i < demand->size() && i < m_object.m_layers.size(); ++ i)
            translate_polygons((*demand)[i], m_lean.shift_at(m_object.m_layers[i]->print_z));
}

ScopedSupportShear::~ScopedSupportShear()
{
    if (! m_lean.active())
        return;
    for (Layer *layer : m_object.m_layers)
        layer->translate(- m_lean.shift_at(layer->print_z));
    for (std::vector<Polygons> *demand : { &m_object.m_segment_support_demand, &m_object.m_segment_body_demand })
        for (size_t i = 0; i < demand->size() && i < m_object.m_layers.size(); ++ i)
            translate_polygons((*demand)[i], - m_lean.shift_at(m_object.m_layers[i]->print_z));
    // Everything the generator built lives in the sheared frame; its own print_z (which may differ
    // from any object layer's, and be negative below a cut floor) says exactly how far it moved.
    for (SupportLayer *layer : m_object.m_support_layers)
        layer->translate(- m_lean.shift_at(layer->print_z));
    m_object.m_support_lean = SupportLean();
}

} // namespace Slic3r

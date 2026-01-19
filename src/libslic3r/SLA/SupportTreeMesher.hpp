#ifndef SUPPORTTREEMESHER_HPP
#define SUPPORTTREEMESHER_HPP

#include "libslic3r/Point.hpp"

#include "libslic3r/SLA/SupportTreeBuilder.hpp"
#include "libslic3r/TriangleMesh.hpp"
//#include "libslic3r/SLA/Contour3D.hpp"

namespace Slic3r { namespace sla {

using Portion = std::tuple<double, double>;

inline Portion make_portion(double a, double b)
{
    return std::make_tuple(a, b);
}

indexed_triangle_set sphere(double  rho,
                            Portion portion = make_portion(0., 2. * PI),
                            double  fa      = (2. * PI / 360.));

// Down facing cylinder in Z direction with arguments:
// r: radius
// h: Height
// ssteps: how many edges will create the base circle
// sp: starting point
indexed_triangle_set cylinder(double       r,
                              double       h,
                              size_t       steps = 45,
                              const Vec3d &sp    = Vec3d::Zero());

indexed_triangle_set pinhead(double r_pin,
                             double r_back,
                             double length,
                             size_t steps = 45);

indexed_triangle_set halfcone(double       baseheight,
                              double       r_bottom,
                              double       r_top,
                              const Vec3d &pt    = Vec3d::Zero(),
                              size_t       steps = 45);

inline indexed_triangle_set get_mesh(const Head &h, size_t steps)
{
    indexed_triangle_set mesh = pinhead(h.r_pin_mm, h.r_back_mm, h.width_mm, steps);

    for (auto& p : mesh.vertices) p.z() -= (h.fullwidth() - h.r_back_mm);

    using Quaternion = Eigen::Quaternion<float>;

    // We rotate the head to the specified direction. The head's pointing
    // side is facing upwards so this means that it would hold a support
    // point with a normal pointing straight down. This is the reason of
    // the -1 z coordinate
    auto quatern = Quaternion::FromTwoVectors(Vec3f{0.f, 0.f, -1.f},
                                              h.dir.cast<float>());

    Vec3f pos = h.pos.cast<float>();
    for (auto& p : mesh.vertices) p = quatern * p + pos;

    return mesh;
}

inline indexed_triangle_set get_mesh(const Pillar &p, size_t steps)
{
    if(p.height > EPSILON) { // Endpoint is below the starting point
        // We just create a bridge geometry with the pillar parameters and
        // move the data.
        return cylinder(p.r, p.height, steps, p.endpoint());
    }

    return {};
}

inline indexed_triangle_set get_mesh(const Pedestal &p, size_t steps)
{
    return halfcone(p.height, p.r_bottom, p.r_top, p.pos, steps);
}

inline indexed_triangle_set get_mesh(const Junction &j, size_t steps)
{
    indexed_triangle_set mesh = sphere(j.r, make_portion(0, PI), 2 *PI / steps);
    Vec3f pos = j.pos.cast<float>();
    for(auto& p : mesh.vertices) p += pos;
    return mesh;
}

inline indexed_triangle_set get_mesh(const Bridge &br, size_t steps)
{
    using Quaternion = Eigen::Quaternion<float>;
    Vec3d v = (br.endp - br.startp);
    Vec3d dir = v.normalized();
    double d = v.norm();

    indexed_triangle_set mesh = cylinder(br.r, d, steps);

    auto quater = Quaternion::FromTwoVectors(Vec3f{0.f, 0.f, 1.f},
                                             dir.cast<float>());

    Vec3f startp = br.startp.cast<float>();
    for(auto& p : mesh.vertices) p = quater * p + startp;

    return mesh;
}

inline indexed_triangle_set get_mesh(const DiffBridge &br, size_t steps)
{
    double h = br.get_length();
    indexed_triangle_set mesh = halfcone(h, br.r, br.end_r, Vec3d::Zero(), steps);

    using Quaternion = Eigen::Quaternion<float>;

    // We rotate the head to the specified direction. The head's pointing
    // side is facing upwards so this means that it would hold a support
    // point with a normal pointing straight down. This is the reason of
    // the -1 z coordinate
    auto quatern = Quaternion::FromTwoVectors(Vec3f{0.f, 0.f, 1.f},
                                              br.get_dir().cast<float>());

    Vec3f startp = br.startp.cast<float>();
    for(auto& p : mesh.vertices) p = quatern * p + startp;

    return mesh;
}

}} // namespace Slic3r::sla

#endif // SUPPORTTREEMESHER_HPP

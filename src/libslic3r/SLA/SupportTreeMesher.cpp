#include "SupportTreeMesher.hpp"

namespace Slic3r { namespace sla {

indexed_triangle_set sphere(double rho, Portion portion, double fa) {

    indexed_triangle_set ret;

    // prohibit close to zero radius
    if(rho <= 1e-6 && rho >= -1e-6) return ret;

    auto& vertices = ret.vertices;
    auto& facets = ret.indices;

    // Algorithm:
    // Add points one-by-one to the sphere grid and form facets using relative
    // coordinates. Sphere is composed effectively of a mesh of stacked circles.

    // adjust via rounding to get an even multiple for any provided angle.
    double angle = (2 * PI / floor(2*PI / fa) );

    // Ring to be scaled to generate the steps of the sphere
    std::vector<double> ring;

    for (double i = 0; i < 2*PI; i+=angle) ring.emplace_back(i);

    const auto sbegin = size_t(2*std::get<0>(portion)/angle);
    const auto send = size_t(2*std::get<1>(portion)/angle);

    const size_t steps = ring.size();
    const double increment = 1.0 / double(steps);

    // special case: first ring connects to 0,0,0
    // insert and form facets.
    if (sbegin == 0)
        vertices.emplace_back(
            Vec3f(0.f, 0.f, float(-rho + increment * sbegin * 2. * rho)));

    auto id = coord_t(vertices.size());
    for (size_t i = 0; i < ring.size(); i++) {
        // Fixed scaling
        const double z = -rho + increment*rho*2.0 * (sbegin + 1.0);
        // radius of the circle for this step.
        const double r = std::sqrt(std::abs(rho*rho - z*z));
        Vec2d b = Eigen::Rotation2Dd(ring[i]) * Eigen::Vector2d(0, r);
        vertices.emplace_back(Vec3d(b(0), b(1), z).cast<float>());

        if (sbegin == 0)
            (i == 0) ? facets.emplace_back(coord_t(ring.size()), 0, 1) :
                       facets.emplace_back(id - 1, 0, id);
        ++id;
    }

    // General case: insert and form facets for each step,
    // joining it to the ring below it.
    for (size_t s = sbegin + 2; s < send - 1; s++) {
        const double z = -rho + increment * double(s * 2. * rho);
        const double r = std::sqrt(std::abs(rho*rho - z*z));

        for (size_t i = 0; i < ring.size(); i++) {
            Vec2d b = Eigen::Rotation2Dd(ring[i]) * Eigen::Vector2d(0, r);
            vertices.emplace_back(Vec3d(b(0), b(1), z).cast<float>());
            auto id_ringsize = coord_t(id - int(ring.size()));
            if (i == 0) {
                // wrap around
                facets.emplace_back(id - 1, id, id + coord_t(ring.size() - 1) );
                facets.emplace_back(id - 1, id_ringsize, id);
            } else {
                facets.emplace_back(id_ringsize - 1, id_ringsize, id);
                facets.emplace_back(id - 1, id_ringsize - 1, id);
            }
            id++;
        }
    }

    // special case: last ring connects to 0,0,rho*2.0
    // only form facets.
    if(send >= size_t(2*PI / angle)) {
        vertices.emplace_back(0.f, 0.f, float(-rho + increment*send*2.0*rho));
        for (size_t i = 0; i < ring.size(); i++) {
            auto id_ringsize = coord_t(id - int(ring.size()));
            if (i == 0) {
                // third vertex is on the other side of the ring.
                facets.emplace_back(id - 1, id_ringsize, id);
            } else {
                auto ci = coord_t(id_ringsize + coord_t(i));
                facets.emplace_back(ci - 1, ci, id);
            }
        }
    }
    id++;

    return ret;
}

indexed_triangle_set cylinder(double r, double h, size_t ssteps, const Vec3d &sp)
{
    assert(ssteps > 0);

    indexed_triangle_set ret;

    auto steps = int(ssteps);
    auto& points = ret.vertices;
    auto& indices = ret.indices;
    points.reserve(2*ssteps);
    double a = 2*PI/steps;

    Vec3d jp = sp;
    Vec3d endp = {sp(X), sp(Y), sp(Z) + h};

    // Upper circle points
    for(int i = 0; i < steps; ++i) {
        double phi = i*a;
        auto ex = float(endp(X) + r*std::cos(phi));
        auto ey = float(endp(Y) + r*std::sin(phi));
        points.emplace_back(ex, ey, float(endp(Z)));
    }

    // Lower circle points
    for(int i = 0; i < steps; ++i) {
        double phi = i*a;
        auto x = float(jp(X) + r*std::cos(phi));
        auto y = float(jp(Y) + r*std::sin(phi));
        points.emplace_back(x, y, float(jp(Z)));
    }

    // Now create long triangles connecting upper and lower circles
    indices.reserve(2*ssteps);
    auto offs = steps;
    for(int i = 0; i < steps - 1; ++i) {
        indices.emplace_back(i, i + offs, offs + i + 1);
        indices.emplace_back(i, offs + i + 1, i + 1);
    }

    // Last triangle connecting the first and last vertices
    auto last = steps - 1;
    indices.emplace_back(0, last, offs);
    indices.emplace_back(last, offs + last, offs);

    // According to the slicing algorithms, we need to aid them with generating
    // a watertight body. So we create a triangle fan for the upper and lower
    // ending of the cylinder to close the geometry.
    points.emplace_back(jp.cast<float>()); int ci = int(points.size() - 1);
    for(int i = 0; i < steps - 1; ++i)
        indices.emplace_back(i + offs + 1, i + offs, ci);

    indices.emplace_back(offs, steps + offs - 1, ci);

    points.emplace_back(endp.cast<float>()); ci = int(points.size() - 1);
    for(int i = 0; i < steps - 1; ++i)
        indices.emplace_back(ci, i, i + 1);

    indices.emplace_back(steps - 1, 0, ci);

    return ret;
}

indexed_triangle_set pinhead(double r_pin,
                             double r_back,
                             double length,
                             size_t steps)
{
    assert(steps > 0);
    assert(length >= 0.);
    assert(r_back > 0.);
    assert(r_pin > 0.);

    indexed_triangle_set mesh;

    // We create two spheres which will be connected with a robe that fits
    // both circles perfectly.

    // Set up the model detail level
    const double detail = 2 * PI / steps;

    // We don't generate whole circles. Instead, we generate only the
    // portions which are visible (not covered by the robe) To know the
    // exact portion of the bottom and top circles we need to use some
    // rules of tangent circles from which we can derive (using simple
    // triangles the following relations:

    // The height of the whole mesh
    const double h   = r_back + r_pin + length;
    double       phi = PI / 2. - std::acos((r_back - r_pin) / h);

    // To generate a whole circle we would pass a portion of (0, Pi)
    // To generate only a half horizontal circle we can pass (0, Pi/2)
    // The calculated phi is an offset to the half circles needed to smooth
    // the transition from the circle to the robe geometry

    auto &&s1 = sphere(r_back, make_portion(0, PI / 2 + phi), detail);
    auto &&s2 = sphere(r_pin, make_portion(PI / 2 + phi, PI), detail);

    for (auto &p : s2.vertices) p.z() += h;

    its_merge(mesh, s1);
    its_merge(mesh, s2);

    for (size_t idx1 = s1.vertices.size() - steps, idx2 = s1.vertices.size();
         idx1 < s1.vertices.size() - 1; idx1++, idx2++) {
        coord_t i1s1 = coord_t(idx1), i1s2 = coord_t(idx2);
        coord_t i2s1 = i1s1 + 1, i2s2 = i1s2 + 1;

        mesh.indices.emplace_back(i1s1, i2s1, i2s2);
        mesh.indices.emplace_back(i1s1, i2s2, i1s2);
    }

    auto i1s1 = coord_t(s1.vertices.size()) - coord_t(steps);
    auto i2s1 = coord_t(s1.vertices.size()) - 1;
    auto i1s2 = coord_t(s1.vertices.size());
    auto i2s2 = coord_t(s1.vertices.size()) + coord_t(steps) - 1;

    mesh.indices.emplace_back(i2s2, i2s1, i1s1);
    mesh.indices.emplace_back(i1s2, i2s2, i1s1);

    return mesh;
}

indexed_triangle_set halfcone(double       baseheight,
                              double       r_bottom,
                              double       r_top,
                              const Vec3d &pos,
                              size_t       steps)
{
    assert(steps > 0);

    if (baseheight <= 0 || steps <= 0) return {};

    indexed_triangle_set base;

    double a    = 2 * PI / steps;
    auto   last = int(steps - 1);
    Vec3d  ep{pos.x(), pos.y(), pos.z() + baseheight};
    for (size_t i = 0; i < steps; ++i) {
        double phi = i * a;
        auto x   = float(pos.x() + r_top * std::cos(phi));
        auto y   = float(pos.y() + r_top * std::sin(phi));
        base.vertices.emplace_back(x, y, float(ep.z()));
    }

    for (size_t i = 0; i < steps; ++i) {
        double phi = i * a;
        auto x   = float(pos.x() + r_bottom * std::cos(phi));
        auto y   = float(pos.y() + r_bottom * std::sin(phi));
        base.vertices.emplace_back(x, y, float(pos.z()));
    }

    base.vertices.emplace_back(pos.cast<float>());
    base.vertices.emplace_back(ep.cast<float>());

    auto &indices = base.indices;
    auto  hcenter = int(base.vertices.size() - 1);
    auto  lcenter = int(base.vertices.size() - 2);
    auto  offs    = int(steps);
    for (int i = 0; i < last; ++i) {
        indices.emplace_back(i, i + offs, offs + i + 1);
        indices.emplace_back(i, offs + i + 1, i + 1);
        indices.emplace_back(i, i + 1, hcenter);
        indices.emplace_back(lcenter, offs + i + 1, offs + i);
    }

    indices.emplace_back(0, last, offs);
    indices.emplace_back(last, offs + last, offs);
    indices.emplace_back(hcenter, last, 0);
    indices.emplace_back(offs, offs + last, lcenter);

    return base;
}

}} // namespace Slic3r::sla

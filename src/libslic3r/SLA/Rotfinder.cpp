#include <limits>

#include <libslic3r/SLA/Rotfinder.hpp>

#include <libslic3r/Execution/ExecutionTBB.hpp>
#include <libslic3r/Execution/ExecutionSeq.hpp>

#include <libslic3r/Optimize/BruteforceOptimizer.hpp>
#include <libslic3r/Optimize/NLoptOptimizer.hpp>

#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <libslic3r/Geometry.hpp>

#include <thread>

namespace Slic3r { namespace sla {

namespace {

inline const Vec3f DOWN = {0.f, 0.f, -1.f};
constexpr double POINTS_PER_UNIT_AREA = 1.f;

// Get the vertices of a triangle directly in an array of 3 points
std::array<Vec3f, 3> get_triangle_vertices(const TriangleMesh &mesh,
                                           size_t              faceidx)
{
    const auto &face = mesh.its.indices[faceidx];
    return {mesh.its.vertices[face(0)],
            mesh.its.vertices[face(1)],
            mesh.its.vertices[face(2)]};
}

std::array<Vec3f, 3> get_transformed_triangle(const TriangleMesh &mesh,
                                              const Transform3f & tr,
                                              size_t              faceidx)
{
    const auto &tri = get_triangle_vertices(mesh, faceidx);
    return {tr * tri[0], tr * tri[1], tr * tri[2]};
}

template<class T> Vec<3, T> normal(const std::array<Vec<3, T>, 3> &tri)
{
    Vec<3, T> U = tri[1] - tri[0];
    Vec<3, T> V = tri[2] - tri[0];
    return U.cross(V).normalized();
}

template<class T, class AccessFn>
T sum_score(AccessFn &&accessfn, size_t facecount, size_t Nthreads)
{
    T  initv         = 0.;
    auto   mergefn   = [](T a, T b) { return a + b; };
    size_t grainsize = facecount / Nthreads;
    size_t from = 0, to = facecount;

    return execution::reduce(ex_tbb, from, to, initv, mergefn, accessfn, grainsize);
}

// Get area and normal of a triangle
struct Facestats {
    Vec3f  normal;
    double area;

    explicit Facestats(const std::array<Vec3f, 3> &triangle)
    {
        Vec3f U = triangle[1] - triangle[0];
        Vec3f V = triangle[2] - triangle[0];
        Vec3f C = U.cross(V);
        normal = C.normalized();
        area = 0.5 * C.norm();
    }
};

// Try to guess the number of support points needed to support a mesh
double get_misalginment_score(const TriangleMesh &mesh, const Transform3f &tr)
{
    if (mesh.its.vertices.empty()) return std::nan("");

    auto accessfn = [&mesh, &tr](size_t fi) {
        Facestats fc{get_transformed_triangle(mesh, tr, fi)};

        float score = fc.area
                      * (std::abs(fc.normal.dot(Vec3f::UnitX()))
                         + std::abs(fc.normal.dot(Vec3f::UnitY()))
                         + std::abs(fc.normal.dot(Vec3f::UnitZ())));

        // We should score against the alignment with the reference planes
        return scaled<int_fast64_t>(score);
    };

    size_t facecount = mesh.its.indices.size();
    size_t Nthreads  = std::thread::hardware_concurrency();
    double S = unscaled(sum_score<int_fast64_t>(accessfn, facecount, Nthreads));

    return S / facecount;
}

// The score function for a particular face
inline double get_supportedness_score(const Facestats &fc)
{
    // Simply get the angle (acos of dot product) between the face normal and
    // the DOWN vector.
    float cosphi = fc.normal.dot(DOWN);
    float phi = 1.f - std::acos(cosphi) / float(PI);

    // Make the huge slopes more significant than the smaller slopes
    phi = phi * phi * phi;

    // Multiply with the square root of face area of the current face,
    // the area is less important as it grows.
    // This makes many smaller overhangs a bigger impact.
    return std::sqrt(fc.area) * POINTS_PER_UNIT_AREA * phi;
}

// Try to guess the number of support points needed to support a mesh
double get_supportedness_score(const TriangleMesh &mesh, const Transform3f &tr)
{
    if (mesh.its.vertices.empty()) return std::nan("");

    auto accessfn = [&mesh, &tr](size_t fi) {
        Facestats fc{get_transformed_triangle(mesh, tr, fi)};
        return scaled<int_fast64_t>(get_supportedness_score(fc));
    };

    size_t facecount = mesh.its.indices.size();
    size_t Nthreads  = std::thread::hardware_concurrency();
    double S = unscaled(sum_score<int_fast64_t>(accessfn, facecount, Nthreads));

    return S / facecount;
}

// Find transformed mesh ground level without copy and with parallel reduce.
float find_ground_level(const TriangleMesh &mesh,
                         const Transform3f & tr,
                         size_t              threads)
{
    size_t vsize = mesh.its.vertices.size();

    auto minfn = [](float a, float b) { return std::min(a, b); };

    auto accessfn = [&mesh, &tr] (size_t vi) {
        return (tr * mesh.its.vertices[vi]).z();
    };

    auto zmin = std::numeric_limits<float>::max();
    size_t granularity = vsize / threads;
    return execution::reduce(ex_tbb, size_t(0), vsize, zmin, minfn, accessfn, granularity);
}

float get_supportedness_onfloor_score(const TriangleMesh &mesh,
                                      const Transform3f & tr)
{
    if (mesh.its.vertices.empty()) return std::nan("");

    size_t Nthreads = std::thread::hardware_concurrency();

    float zmin = find_ground_level(mesh, tr, Nthreads);
    float zlvl = zmin + 0.1f; // Set up a slight tolerance from z level

    auto accessfn = [&mesh, &tr, zlvl](size_t fi) {
        std::array<Vec3f, 3> tri = get_transformed_triangle(mesh, tr, fi);
        Facestats fc{tri};

        if (tri[0].z() <= zlvl && tri[1].z() <= zlvl && tri[2].z() <= zlvl)
            return -2 * fc.area * POINTS_PER_UNIT_AREA;

        return get_supportedness_score(fc);
    };

    size_t facecount = mesh.its.indices.size();
    double S = unscaled(sum_score<int_fast64_t>(accessfn, facecount, Nthreads));

    return S / facecount;
}

using XYRotation = std::array<double, 2>;

// prepare the rotation transformation
Transform3f to_transform3f(const XYRotation &rot)
{
    Transform3f rt = Transform3f::Identity();
    rt.rotate(Eigen::AngleAxisf(float(rot[1]), Vec3f::UnitY()));
    rt.rotate(Eigen::AngleAxisf(float(rot[0]), Vec3f::UnitX()));

    return rt;
}

XYRotation from_transform3f(const Transform3f &tr)
{
    Vec3d rot3 = Geometry::Transformation{tr.cast<double>()}.get_rotation();
    return {rot3.x(), rot3.y()};
}

inline bool is_on_floor(const SLAPrintObjectConfig &cfg)
{
    auto opt_elevation = cfg.support_object_elevation.getFloat();
    auto opt_padaround = cfg.pad_around_object.getBool();

    return opt_elevation < EPSILON || opt_padaround;
}

// collect the rotations for each face of the convex hull
std::vector<XYRotation> get_chull_rotations(const TriangleMesh &mesh, size_t max_count)
{
    TriangleMesh chull = mesh.convex_hull_3d();
    double chull2d_area = chull.convex_hull().area();
    double area_threshold = chull2d_area / (scaled<double>(1e3) * scaled(1.));

    size_t facecount = chull.its.indices.size();

    struct RotArea { XYRotation rot; double area; };

    auto inputs = reserve_vector<RotArea>(facecount);

    auto rotcmp = [](const RotArea &r1, const RotArea &r2) {
        double xdiff = r1.rot[X] - r2.rot[X], ydiff = r1.rot[Y] - r2.rot[Y];
        return std::abs(xdiff) < EPSILON ? ydiff < 0. : xdiff < 0.;
    };

    auto eqcmp = [](const XYRotation &r1, const XYRotation &r2) {
        double xdiff = r1[X] - r2[X], ydiff = r1[Y] - r2[Y];
        return std::abs(xdiff) < EPSILON  && std::abs(ydiff) < EPSILON;
    };

    for (size_t fi = 0; fi < facecount; ++fi) {
        Facestats fc{get_triangle_vertices(chull, fi)};

        if (fc.area > area_threshold)  {
            auto q = Eigen::Quaternionf{}.FromTwoVectors(fc.normal, DOWN);
            XYRotation rot = from_transform3f(Transform3f::Identity() * q);
            RotArea ra = {rot, fc.area};

            auto it = std::lower_bound(inputs.begin(), inputs.end(), ra, rotcmp);

            if (it == inputs.end() || !eqcmp(it->rot, rot))
                inputs.insert(it, ra);
        }
    }

    inputs.shrink_to_fit();
    if (!max_count) max_count = inputs.size();
    std::sort(inputs.begin(), inputs.end(),
              [](const RotArea &ra, const RotArea &rb) {
                  return ra.area > rb.area;
              });

    auto ret = reserve_vector<XYRotation>(std::min(max_count, inputs.size()));
    for (const RotArea &ra : inputs) ret.emplace_back(ra.rot);

    return ret;
}

// Find the best score from a set of function inputs. Evaluate for every point.
template<size_t N, class Fn, class It, class StopCond>
std::array<double, N> find_min_score(Fn &&fn, It from, It to, StopCond &&stopfn)
{
    std::array<double, N> ret = {};

    double score = std::numeric_limits<double>::max();

    size_t Nthreads = std::thread::hardware_concurrency();
    size_t dist = std::distance(from, to);
    std::vector<double> scores(dist, score);

    execution::for_each(
        ex_tbb, size_t(0), dist, [&stopfn, &scores, &fn, &from](size_t i) {
            if (stopfn()) return;

            scores[i] = fn(*(from + i));
        },
        dist / Nthreads);

    auto it = std::min_element(scores.begin(), scores.end());

    if (it != scores.end())
        ret = *(from + std::distance(scores.begin(), it));

    return ret;
}

} // namespace



template<unsigned MAX_ITER>
struct RotfinderBoilerplate {
    static constexpr unsigned MAX_TRIES = MAX_ITER;

    int status = 0;
    TriangleMesh mesh;
    unsigned max_tries;
    const RotOptimizeParams &params;

    // Assemble the mesh with the correct transformation to be used in rotation
    // optimization.
    static TriangleMesh get_mesh_to_rotate(const ModelObject &mo)
    {
        TriangleMesh mesh = mo.raw_mesh();

        ModelInstance *mi = mo.instances[0];
        auto rotation = Vec3d::Zero();
        auto offset = Vec3d::Zero();
        Transform3d trafo_instance =
            Geometry::assemble_transform(offset, rotation,
                                         mi->get_scaling_factor(),
                                         mi->get_mirror());

        mesh.transform(trafo_instance);

        return mesh;
    }

    RotfinderBoilerplate(const ModelObject &mo, const RotOptimizeParams &p)
        : mesh{get_mesh_to_rotate(mo)}
        , params{p}
        , max_tries(p.accuracy() * MAX_TRIES)
    {

    }

    void statusfn() { params.statuscb()(++status * 100.0 / max_tries); }
    bool stopcond() { return ! params.statuscb()(-1); }
};

Vec2d find_best_misalignment_rotation(const ModelObject &      mo,
                                      const RotOptimizeParams &params)
{
    RotfinderBoilerplate<1000> bp{mo, params};

    // Preparing the optimizer.
    size_t gridsize = std::sqrt(bp.max_tries);
    opt::Optimizer<opt::AlgBruteForce> solver(
        opt::StopCriteria{}.max_iterations(bp.max_tries)
                           .stop_condition([&bp] { return bp.stopcond(); }),
        gridsize
    );

    // We are searching rotations around only two axes x, y. Thus the
    // problem becomes a 2 dimensional optimization task.
    // We can specify the bounds for a dimension in the following way:
    auto bounds = opt::bounds({ {-PI, PI}, {-PI, PI} });

    auto result = solver.to_max().optimize(
        [&bp] (const XYRotation &rot)
        {
            bp.statusfn();
            return get_misalginment_score(bp.mesh, to_transform3f(rot));
        }, opt::initvals({0., 0.}), bounds);

    return {result.optimum[0], result.optimum[1]};
}

Vec2d find_least_supports_rotation(const ModelObject &      mo,
                                   const RotOptimizeParams &params)
{
    RotfinderBoilerplate<1000> bp{mo, params};

    SLAPrintObjectConfig pocfg;
    if (params.print_config())
        pocfg.apply(*params.print_config(), true);

    pocfg.apply(mo.config.get());

    XYRotation rot;

    // Different search methods have to be used depending on the model elevation
    if (is_on_floor(pocfg)) {

        std::vector<XYRotation> inputs = get_chull_rotations(bp.mesh, bp.max_tries);
        bp.max_tries = inputs.size();

        // If the model can be placed on the bed directly, we only need to
        // check the 3D convex hull face rotations.

        auto objfn = [&bp](const XYRotation &rot) {
            bp.statusfn();
            Transform3f tr = to_transform3f(rot);
            return get_supportedness_onfloor_score(bp.mesh, tr);
        };

        rot = find_min_score<2>(objfn, inputs.begin(), inputs.end(), [&bp] {
            return bp.stopcond();
        });

    } else {
        // Preparing the optimizer.
        size_t gridsize = std::sqrt(bp.max_tries); // 2D grid has gridsize^2 calls
        opt::Optimizer<opt::AlgBruteForce> solver(
            opt::StopCriteria{}.max_iterations(bp.max_tries)
                               .stop_condition([&bp] { return bp.stopcond(); }),
            gridsize
        );

        // We are searching rotations around only two axes x, y. Thus the
        // problem becomes a 2 dimensional optimization task.
        // We can specify the bounds for a dimension in the following way:
        auto bounds = opt::bounds({ {-PI, PI}, {-PI, PI} });

        auto result = solver.to_min().optimize(
            [&bp] (const XYRotation &rot)
            {
                bp.statusfn();
                return get_supportedness_score(bp.mesh, to_transform3f(rot));
            }, opt::initvals({0., 0.}), bounds);

        // Save the result
        rot = result.optimum;
    }

    return {rot[0], rot[1]};
}

inline BoundingBoxf3 bounding_box_with_tr(const indexed_triangle_set &its,
                                          const Transform3f &tr)
{
    if (its.vertices.empty())
        return {};

    Vec3f bmin = tr * its.vertices.front(), bmax = tr * its.vertices.front();

    for (const Vec3f &p : its.vertices) {
        Vec3f pp = tr * p;
        bmin = pp.cwiseMin(bmin);
        bmax = pp.cwiseMax(bmax);
    }

    return {bmin.cast<double>(), bmax.cast<double>()};
}

Vec2d find_min_z_height_rotation(const ModelObject &mo,
                                 const RotOptimizeParams &params)
{
    RotfinderBoilerplate<1000> bp{mo, params};

    TriangleMesh chull = bp.mesh.convex_hull_3d();
    auto inputs = reserve_vector<XYRotation>(chull.its.indices.size());
    auto rotcmp = [](const XYRotation &r1, const XYRotation &r2) {
        double xdiff = r1[X] - r2[X], ydiff = r1[Y] - r2[Y];
        return std::abs(xdiff) < EPSILON ? ydiff < 0. : xdiff < 0.;
    };
    auto eqcmp = [](const XYRotation &r1, const XYRotation &r2) {
        double xdiff = r1[X] - r2[X], ydiff = r1[Y] - r2[Y];
        return std::abs(xdiff) < EPSILON  && std::abs(ydiff) < EPSILON;
    };

    for (size_t fi = 0; fi < chull.its.indices.size(); ++fi) {
        Facestats fc{get_triangle_vertices(chull, fi)};

        auto q = Eigen::Quaternionf{}.FromTwoVectors(fc.normal, DOWN);
        XYRotation rot = from_transform3f(Transform3f::Identity() * q);

        auto it = std::lower_bound(inputs.begin(), inputs.end(), rot, rotcmp);

        if (it == inputs.end() || !eqcmp(*it, rot))
            inputs.insert(it, rot);
    }

    inputs.shrink_to_fit();
    bp.max_tries = inputs.size();

    auto objfn = [&bp, &chull](const XYRotation &rot) {
        bp.statusfn();
        Transform3f tr = to_transform3f(rot);
        return bounding_box_with_tr(chull.its, tr).size().z();
    };

    XYRotation rot = find_min_score<2>(objfn, inputs.begin(), inputs.end(), [&bp] {
        return bp.stopcond();
    });

    return {rot[0], rot[1]};
}

}} // namespace Slic3r::sla

#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../ShortestPath.hpp"

#include "FillAdaptive.hpp"

// for indexed_triangle_set
#include <admesh/stl.h>

#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <numeric>

// Boost pool: Don't use mutexes to synchronize memory allocation.
#define BOOST_POOL_NO_MT
#include <boost/pool/object_pool.hpp>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/geometries/segment.hpp>
#include <boost/geometry/index/rtree.hpp>


namespace Slic3r {
namespace FillAdaptive {

// Derived from https://github.com/juj/MathGeoLib/blob/master/src/Geometry/Triangle.cpp
// The AABB-Triangle test implementation is based on the pseudo-code in
// Christer Ericson's Real-Time Collision Detection, pp. 169-172. It is
// practically a standard SAT test.
//
// Original MathGeoLib benchmark:
//    Best: 17.282 nsecs / 46.496 ticks, Avg: 17.804 nsecs, Worst: 18.434 nsecs
//
//FIXME Vojtech: The MathGeoLib contains a vectorized implementation.
template<typename Vector> 
bool triangle_AABB_intersects(const Vector &a, const Vector &b, const Vector &c, const BoundingBoxBase<Vector> &aabb)
{
    using Scalar = typename Vector::Scalar;

    Vector tMin = a.cwiseMin(b.cwiseMin(c));
    Vector tMax = a.cwiseMax(b.cwiseMax(c));

    if (tMin.x() >= aabb.max.x() || tMax.x() <= aabb.min.x()
        || tMin.y() >= aabb.max.y() || tMax.y() <= aabb.min.y()
        || tMin.z() >= aabb.max.z() || tMax.z() <= aabb.min.z())
        return false;

    Vector center = (aabb.min + aabb.max) * 0.5f;
    Vector h = aabb.max - center;

    const Vector t[3] { b-a, c-a, c-b };

    Vector ac = a - center;

    Vector n = t[0].cross(t[1]);
    Scalar s = n.dot(ac);
    Scalar r = std::abs(h.dot(n.cwiseAbs()));
    if (abs(s) >= r)
        return false;

    const Vector at[3] = { t[0].cwiseAbs(), t[1].cwiseAbs(), t[2].cwiseAbs() };

    Vector bc = b - center;
    Vector cc = c - center;

    // SAT test all cross-axes.
    // The following is a fully unrolled loop of this code, stored here for reference:
    /*
    Scalar d1, d2, a1, a2;
    const Vector e[3] = { DIR_VEC(1, 0, 0), DIR_VEC(0, 1, 0), DIR_VEC(0, 0, 1) };
    for(int i = 0; i < 3; ++i)
        for(int j = 0; j < 3; ++j)
        {
            Vector axis = Cross(e[i], t[j]);
            ProjectToAxis(axis, d1, d2);
            aabb.ProjectToAxis(axis, a1, a2);
            if (d2 <= a1 || d1 >= a2) return false;
        }
    */

    // eX <cross> t[0]
    Scalar d1 = t[0].y() * ac.z() - t[0].z() * ac.y();
    Scalar d2 = t[0].y() * cc.z() - t[0].z() * cc.y();
    Scalar tc = (d1 + d2) * 0.5f;
    r = std::abs(h.y() * at[0].z() + h.z() * at[0].y());
    if (r + std::abs(tc - d1) < std::abs(tc))
        return false;

    // eX <cross> t[1]
    d1 = t[1].y() * ac.z() - t[1].z() * ac.y();
    d2 = t[1].y() * bc.z() - t[1].z() * bc.y();
    tc = (d1 + d2) * 0.5f;
    r = std::abs(h.y() * at[1].z() + h.z() * at[1].y());
    if (r + std::abs(tc - d1) < std::abs(tc))
        return false;

    // eX <cross> t[2]
    d1 = t[2].y() * ac.z() - t[2].z() * ac.y();
    d2 = t[2].y() * bc.z() - t[2].z() * bc.y();
    tc = (d1 + d2) * 0.5f;
    r = std::abs(h.y() * at[2].z() + h.z() * at[2].y());
    if (r + std::abs(tc - d1) < std::abs(tc))
        return false;

    // eY <cross> t[0]
    d1 = t[0].z() * ac.x() - t[0].x() * ac.z();
    d2 = t[0].z() * cc.x() - t[0].x() * cc.z();
    tc = (d1 + d2) * 0.5f;
    r = std::abs(h.x() * at[0].z() + h.z() * at[0].x());
    if (r + std::abs(tc - d1) < std::abs(tc))
        return false;

    // eY <cross> t[1]
    d1 = t[1].z() * ac.x() - t[1].x() * ac.z();
    d2 = t[1].z() * bc.x() - t[1].x() * bc.z();
    tc = (d1 + d2) * 0.5f;
    r = std::abs(h.x() * at[1].z() + h.z() * at[1].x());
    if (r + std::abs(tc - d1) < std::abs(tc))
        return false;

    // eY <cross> t[2]
    d1 = t[2].z() * ac.x() - t[2].x() * ac.z();
    d2 = t[2].z() * bc.x() - t[2].x() * bc.z();
    tc = (d1 + d2) * 0.5f;
    r = std::abs(h.x() * at[2].z() + h.z() * at[2].x());
    if (r + std::abs(tc - d1) < std::abs(tc))
        return false;

    // eZ <cross> t[0]
    d1 = t[0].x() * ac.y() - t[0].y() * ac.x();
    d2 = t[0].x() * cc.y() - t[0].y() * cc.x();
    tc = (d1 + d2) * 0.5f;
    r = std::abs(h.y() * at[0].x() + h.x() * at[0].y());
    if (r + std::abs(tc - d1) < std::abs(tc))
        return false;

    // eZ <cross> t[1]
    d1 = t[1].x() * ac.y() - t[1].y() * ac.x();
    d2 = t[1].x() * bc.y() - t[1].y() * bc.x();
    tc = (d1 + d2) * 0.5f;
    r = std::abs(h.y() * at[1].x() + h.x() * at[1].y());
    if (r + std::abs(tc - d1) < std::abs(tc))
        return false;

    // eZ <cross> t[2]
    d1 = t[2].x() * ac.y() - t[2].y() * ac.x();
    d2 = t[2].x() * bc.y() - t[2].y() * bc.x();
    tc = (d1 + d2) * 0.5f;
    r = std::abs(h.y() * at[2].x() + h.x() * at[2].y());
    if (r + std::abs(tc - d1) < std::abs(tc))
        return false;

    // No separating axis exists, the AABB and triangle intersect.
    return true;
}

//    static double dist2_to_triangle(const Vec3d &a, const Vec3d &b, const Vec3d &c, const Vec3d &p)
//    {
//        double out = std::numeric_limits<double>::max();
//        const Vec3d v1 = b - a;
//        auto        l1 = v1.squaredNorm();
//        const Vec3d v2 = c - b;
//        auto        l2 = v2.squaredNorm();
//        const Vec3d v3 = a - c;
//        auto        l3 = v3.squaredNorm();

//        // Is the triangle valid?
//        if (l1 > 0. && l2 > 0. && l3 > 0.)
//        {
//            // 1) Project point into the plane of the triangle.
//            const Vec3d n = v1.cross(v2);
//            double d = (p - a).dot(n);
//            const Vec3d foot_pt = p - n * d / n.squaredNorm();

//            // 2) Maximum projection of n.
//            int proj_axis;
//            n.array().cwiseAbs().maxCoeff(&proj_axis);

//            // 3) Test whether the foot_pt is inside the triangle.
//            {
//                auto inside_triangle = [](const Vec2d& v1, const Vec2d& v2, const Vec2d& v3, const Vec2d& pt) {
//                    const double d1 = cross2(v1, pt);
//                    const double d2 = cross2(v2, pt);
//                    const double d3 = cross2(v3, pt);
//                    // Testing both CCW and CW orientations.
//                    return (d1 >= 0. && d2 >= 0. && d3 >= 0.) || (d1 <= 0. && d2 <= 0. && d3 <= 0.);
//                };
//                bool inside;
//                switch (proj_axis) {
//                case 0:
//                    inside = inside_triangle({v1.y(), v1.z()}, {v2.y(), v2.z()}, {v3.y(), v3.z()}, {foot_pt.y(), foot_pt.z()}); break;
//                case 1:
//                    inside = inside_triangle({v1.z(), v1.x()}, {v2.z(), v2.x()}, {v3.z(), v3.x()}, {foot_pt.z(), foot_pt.x()}); break;
//                default:
//                    assert(proj_axis == 2);
//                    inside = inside_triangle({v1.x(), v1.y()}, {v2.x(), v2.y()}, {v3.x(), v3.y()}, {foot_pt.x(), foot_pt.y()}); break;
//                }
//                if (inside)
//                    return (p - foot_pt).squaredNorm();
//            }

//            // 4) Find minimum distance to triangle vertices and edges.
//            out = std::min((p - a).squaredNorm(), std::min((p - b).squaredNorm(), (p - c).squaredNorm()));
//            auto t = (p - a).dot(v1);
//            if (t > 0. && t < l1)
//                out = std::min(out, (a + v1 * (t / l1) - p).squaredNorm());
//            t = (p - b).dot(v2);
//            if (t > 0. && t < l2)
//                out = std::min(out, (b + v2 * (t / l2) - p).squaredNorm());
//            t = (p - c).dot(v3);
//            if (t > 0. && t < l3)
//                out = std::min(out, (c + v3 * (t / l3) - p).squaredNorm());
//        }

//        return out;
//    }

// Ordering of children cubes.
static const std::array<Vec3d, 8> child_centers {
    Vec3d(-1, -1, -1), Vec3d( 1, -1, -1), Vec3d(-1,  1, -1), Vec3d( 1,  1, -1),
    Vec3d(-1, -1,  1), Vec3d( 1, -1,  1), Vec3d(-1,  1,  1), Vec3d( 1,  1,  1)
};

// Traversal order of octree children cells for three infill directions,
// so that a single line will be discretized in a strictly monotonic order.
static constexpr std::array<std::array<int, 8>, 3> child_traversal_order {
    std::array<int, 8>{ 2, 3, 0, 1, 6, 7, 4, 5 },
    std::array<int, 8>{ 4, 0, 6, 2, 5, 1, 7, 3 },
    std::array<int, 8>{ 1, 5, 0, 4, 3, 7, 2, 6 },
};

struct Cube
{
    Vec3d center;
#ifndef NDEBUG
    Vec3d center_octree;
#endif // NDEBUG
    std::array<Cube*, 8> children {}; // initialized to nullptrs
    Cube(const Vec3d &center) : center(center) {}
};

struct CubeProperties
{
    double edge_length;     // Lenght of edge of a cube
    double height;          // Height of rotated cube (standing on the corner)
    double diagonal_length; // Length of diagonal of a cube a face
    double line_z_distance; // Defines maximal distance from a center of a cube on Z axis on which lines will be created
    double line_xy_distance;// Defines maximal distance from a center of a cube on X and Y axis on which lines will be created
};

struct Octree
{
    // Octree will allocate its Cubes from the pool. The pool only supports deletion of the complete pool,
    // perfect for building up our octree.
    boost::object_pool<Cube>    pool;
    Cube*                       root_cube { nullptr };
    Vec3d                       origin;
    std::vector<CubeProperties> cubes_properties;

    Octree(const Vec3d &origin, const std::vector<CubeProperties> &cubes_properties)
        : root_cube(pool.construct(origin)), origin(origin), cubes_properties(cubes_properties) {}

    void insert_triangle(const Vec3d &a, const Vec3d &b, const Vec3d &c, Cube *current_cube, const BoundingBoxf3 &current_bbox, int depth);
};

void OctreeDeleter::operator()(Octree *p) {
    delete p;
}

std::pair<double, double> adaptive_fill_line_spacing(const PrintObject &print_object)
{
    // Output, spacing for icAdaptiveCubic and icSupportCubic
    double  adaptive_line_spacing = 0.;
    double  support_line_spacing = 0.;

    enum class Tristate {
        Yes,
        No,
        Maybe
    };
    struct RegionFillData {
        Tristate        has_adaptive_infill;
        Tristate        has_support_infill;
        double          density;
        double          extrusion_width;
    };
    std::vector<RegionFillData> region_fill_data;
    region_fill_data.reserve(print_object.num_printing_regions());
    bool                       build_octree                   = false;
    const std::vector<double> &nozzle_diameters               = print_object.print()->config().nozzle_diameter.values;
    double                     max_nozzle_diameter            = *std::max_element(nozzle_diameters.begin(), nozzle_diameters.end());
    double                     default_infill_extrusion_width = Flow::auto_extrusion_width(FlowRole::frInfill, float(max_nozzle_diameter));
    for (size_t region_id = 0; region_id < print_object.num_printing_regions(); ++ region_id) {
        const PrintRegionConfig &config                 = print_object.printing_region(region_id).config();
        bool                     nonempty               = config.sparse_infill_density > 0;
        bool                     has_adaptive_infill    = nonempty && config.sparse_infill_pattern == ipAdaptiveCubic;
        bool                     has_support_infill     = nonempty && config.sparse_infill_pattern == ipSupportCubic;
        double                   sparse_infill_line_width = config.sparse_infill_line_width.get_abs_value(max_nozzle_diameter);
        region_fill_data.push_back(RegionFillData({
            has_adaptive_infill ? Tristate::Maybe : Tristate::No,
            has_support_infill ? Tristate::Maybe : Tristate::No,
            config.sparse_infill_density,
            sparse_infill_line_width != 0. ? sparse_infill_line_width : default_infill_extrusion_width
        }));
        build_octree |= has_adaptive_infill || has_support_infill;
    }

    if (build_octree) {
        // Compute the average of above parameters over all layers
        for (const Layer *layer : print_object.layers())
            for (size_t region_id = 0; region_id < layer->regions().size(); ++ region_id) {
                RegionFillData &rd = region_fill_data[region_id];
                if (rd.has_adaptive_infill == Tristate::Maybe && ! layer->regions()[region_id]->fill_surfaces.empty())
                    rd.has_adaptive_infill = Tristate::Yes;
                if (rd.has_support_infill == Tristate::Maybe && ! layer->regions()[region_id]->fill_surfaces.empty())
                    rd.has_support_infill = Tristate::Yes;
            }

        double  adaptive_fill_density           = 0.;
        double  adaptive_infill_extrusion_width = 0.;
        int     adaptive_cnt                    = 0;
        double  support_fill_density            = 0.;
        double  support_infill_extrusion_width  = 0.;
        int     support_cnt                     = 0;

        for (const RegionFillData &rd : region_fill_data) {
            if (rd.has_adaptive_infill == Tristate::Yes) {
                adaptive_fill_density           += rd.density;
                adaptive_infill_extrusion_width += rd.extrusion_width;
                ++ adaptive_cnt;
            } else if (rd.has_support_infill == Tristate::Yes) {
                support_fill_density           += rd.density;
                support_infill_extrusion_width += rd.extrusion_width;
                ++ support_cnt;
            }
        }

        auto to_line_spacing = [](int cnt, double density, double extrusion_width) {
            if (cnt) {
                density         /= double(cnt);
                extrusion_width /= double(cnt);
                return extrusion_width / ((density / 100.0f) * 0.333333333f);
            } else
                return 0.;
        };
        const int n_multiline = print_object.printing_region(0).config().fill_multiline.value;
        adaptive_line_spacing = to_line_spacing(adaptive_cnt, adaptive_fill_density, adaptive_infill_extrusion_width) * n_multiline;
        support_line_spacing  = to_line_spacing(support_cnt, support_fill_density, support_infill_extrusion_width) * n_multiline;
    }

    return std::make_pair(adaptive_line_spacing, support_line_spacing);
}

// Context used by generate_infill_lines() when recursively traversing an octree in a DDA fashion
// (Digital Differential Analyzer).
struct FillContext
{
    // The angles have to agree with child_traversal_order.
    static constexpr double direction_angles[3] {
        0.,
        (2.0 * M_PI) / 3.0,
        -(2.0 * M_PI) / 3.0
    };

    FillContext(const Octree &octree, double z_position, int direction_idx) :
        cubes_properties(octree.cubes_properties),
        z_position(z_position),
        traversal_order(child_traversal_order[direction_idx]),
        cos_a(cos(direction_angles[direction_idx])),
        sin_a(sin(direction_angles[direction_idx]))
    {
        static constexpr auto unused = std::numeric_limits<coord_t>::max();
        temp_lines.assign((1 << octree.cubes_properties.size()) - 1, Line(Point(unused, unused), Point(unused, unused)));
    }

    // Rotate the point, uses the same convention as Point::rotate().
    Vec2d rotate(const Vec2d& v) { return Vec2d(this->cos_a * v.x() - this->sin_a * v.y(), this->sin_a * v.x() + this->cos_a * v.y()); }

    const std::vector<CubeProperties>  &cubes_properties;
    // Top of the current layer.
    const double                        z_position;
    // Order of traversal for this line direction.
    const std::array<int, 8>            traversal_order;
    // Rotation of the generated line for this line direction.
    const double                        cos_a;
    const double                        sin_a;

    // Linearized tree spanning a single Octree wall, used to connect lines spanning
    // neighboring Octree cells. Unused lines have the Line::a::x set to infinity.
    std::vector<Line>                   temp_lines;
    // Final output
    std::vector<Line>                   output_lines;
};

static constexpr double octree_rot[3] = { 5.0 * M_PI / 4.0, Geometry::deg2rad(215.264), M_PI / 6.0 };

Eigen::Quaterniond transform_to_world()
{
    return Eigen::AngleAxisd(octree_rot[2], Vec3d::UnitZ()) * Eigen::AngleAxisd(octree_rot[1], Vec3d::UnitY()) * Eigen::AngleAxisd(octree_rot[0], Vec3d::UnitX());
}

Eigen::Quaterniond transform_to_octree()
{
    return Eigen::AngleAxisd(- octree_rot[0], Vec3d::UnitX()) * Eigen::AngleAxisd(- octree_rot[1], Vec3d::UnitY()) * Eigen::AngleAxisd(- octree_rot[2], Vec3d::UnitZ());
}

#ifndef NDEBUG
// Verify that the traversal order of the octree children matches the line direction,
// therefore the infill line may get extended with O(1) time & space complexity.
static bool verify_traversal_order(
    FillContext  &context,
    const Cube   *cube,
    int           depth,
    const Vec2d  &line_from,
    const Vec2d  &line_to)
{
    std::array<Vec3d, 8> c;
    Eigen::Quaterniond to_world = transform_to_world();
    for (int i = 0; i < 8; ++i) {
        int j = context.traversal_order[i];
        Vec3d cntr = to_world * (cube->center_octree + (child_centers[j] * (context.cubes_properties[depth].edge_length / 4.)));
        assert(!cube->children[j] || cube->children[j]->center.isApprox(cntr));
        c[i] = cntr;
    }
    std::array<Vec3d, 10> dirs = {
        c[1] - c[0], c[2] - c[0], c[3] - c[1], c[3] - c[2], c[3] - c[0],
        c[5] - c[4], c[6] - c[4], c[7] - c[5], c[7] - c[6], c[7] - c[4]
    };
    assert(std::abs(dirs[4].z()) < 0.005);
    assert(std::abs(dirs[9].z()) < 0.005);
    assert(dirs[0].isApprox(dirs[3]));
    assert(dirs[1].isApprox(dirs[2]));
    assert(dirs[5].isApprox(dirs[8]));
    assert(dirs[6].isApprox(dirs[7]));
    Vec3d line_dir = Vec3d(line_to.x() - line_from.x(), line_to.y() - line_from.y(), 0.).normalized();
    for (auto& dir : dirs) {
        double d = dir.normalized().dot(line_dir);
        assert(d > 0.7);
    }
    return true;
}
#endif // NDEBUG

static void generate_infill_lines_recursive(
    FillContext     &context,
    const Cube      *cube,
    // Address of this wall in the octree,  used to address context.temp_lines.
    int              address,
    int              depth)
{
    assert(cube != nullptr);

    const std::vector<CubeProperties> &cubes_properties = context.cubes_properties;
    const double z_diff     = context.z_position - cube->center.z();
    const double z_diff_abs = std::abs(z_diff);

    if (z_diff_abs > cubes_properties[depth].height / 2.)
        return;

    if (z_diff_abs < cubes_properties[depth].line_z_distance) {
        // Discretize a single wall splitting the cube into two.
        const double zdist = cubes_properties[depth].line_z_distance;
        Vec2d from(
            0.5 * cubes_properties[depth].diagonal_length * (zdist - z_diff_abs) / zdist,
            cubes_properties[depth].line_xy_distance - (zdist + z_diff) / sqrt(2.));
        Vec2d to(-from.x(), from.y());
        from = context.rotate(from);
        to   = context.rotate(to);
        // Relative to cube center
        const Vec2d offset(cube->center.x(), cube->center.y());
        from += offset;
        to   += offset;
        // Verify that the traversal order of the octree children matches the line direction,
        // therefore the infill line may get extended with O(1) time & space complexity.
        assert(verify_traversal_order(context, cube, depth, from, to));
        // Either extend an existing line or start a new one.
        Line &last_line = context.temp_lines[address];
        Line  new_line(Point::new_scale(from), Point::new_scale(to));
        if (last_line.a.x() == std::numeric_limits<coord_t>::max()) {
            last_line.a = new_line.a;
        } else if ((new_line.a - last_line.b).cwiseAbs().maxCoeff() > 1000) { // SCALED_EPSILON is 100 and it is not enough
            context.output_lines.emplace_back(last_line);
            last_line.a = new_line.a;
        }
        last_line.b = new_line.b;
    }

    // left child index
    address = address * 2 + 1;
    -- depth;
    size_t i = 0;
    for (const int child_idx : context.traversal_order) {
        const Cube *child = cube->children[child_idx];
        if (child != nullptr)
            generate_infill_lines_recursive(context, child, address, depth);
        if (++ i == 4)
            // right child index
            ++ address;
    }
}

#ifndef NDEBUG
//    #define ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
#endif

#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
static void export_infill_lines_to_svg(const ExPolygon &expoly, const Polylines &polylines, const std::string &path, const Points &pts = Points())
{
    BoundingBox bbox = get_extents(expoly);
    bbox.offset(scale_(3.));

    ::Slic3r::SVG svg(path, bbox);
    svg.draw(expoly);
    svg.draw_outline(expoly, "green");
    svg.draw(polylines, "red");
    static constexpr double trim_length = scale_(0.4);
    for (Polyline polyline : polylines)
        if (! polyline.empty()) {
            Vec2d a = polyline.points.front().cast<double>();
            Vec2d d = polyline.points.back().cast<double>();
            if (polyline.size() == 2) {
                Vec2d v = d - a;
                double l = v.norm();
                if (l > 2. * trim_length) {
                    a += v * trim_length / l;
                    d -= v * trim_length / l;
                    polyline.points.front() = a.cast<coord_t>();
                    polyline.points.back() = d.cast<coord_t>();
                } else
                    polyline.points.clear();
            } else if (polyline.size() > 2) {
                Vec2d b = polyline.points[1].cast<double>();
                Vec2d c = polyline.points[polyline.points.size() - 2].cast<double>();
                Vec2d v = b - a;
                double l = v.norm();
                if (l > trim_length) {
                    a += v * trim_length / l;
                    polyline.points.front() = a.cast<coord_t>();
                } else
                    polyline.points.erase(polyline.points.begin());
                v = d - c;
                l = v.norm();
                if (l > trim_length)
                    polyline.points.back() = (d - v * trim_length / l).cast<coord_t>();
                else
                    polyline.points.pop_back();
            }
            svg.draw(polyline, "black");
        }
    svg.draw(pts, "magenta");
}
#endif /* ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT */

// Representing a T-joint (in general case) between two infill lines
// (between one end point of intersect_pl/intersect_line and 
struct Intersection
{
    // Closest line to intersect_point.
    const Line  *closest_line;

    // The line for which is computed closest line from intersect_point to closest_line
    const Line  *intersect_line;
    // Pointer to the polyline from which is computed closest_line
    Polyline    *intersect_pl;
    // Point for which is computed closest line (closest_line)
    Point        intersect_point;
    // Indicate if intersect_point is the first or the last point of intersect_pl
    bool         front;
    // Signum of intersect_line_dir.cross(closest_line.dir()):
    bool         left;

    // Indication if this intersection has been proceed
    bool         used = false;

    bool         fresh() const throw() { return ! used && ! intersect_pl->empty(); }

    Intersection(const Line &closest_line, const Line &intersect_line, Polyline *intersect_pl, const Point &intersect_point, bool front) :
        closest_line(&closest_line), intersect_line(&intersect_line), intersect_pl(intersect_pl), intersect_point(intersect_point), front(front)
    {
        // Calculate side of this intersection line of the closest line.
        Vec2d v1((this->closest_line->b - this->closest_line->a).cast<double>());
        Vec2d v2(this->intersect_line_dir());
#ifndef NDEBUG
        {
            Vec2d v1n = v1.normalized();
            Vec2d v2n = v2.normalized();
            double c = cross2(v1n, v2n);
            assert(std::abs(c) > sin(M_PI / 12.));
        }
#endif // NDEBUG
        this->left = cross2(v1, v2) > 0.;
    }

    std::optional<Line> other_hook() const {
        std::optional<Line> out;
        const Points &pts = intersect_pl->points;
        if (pts.size() >= 3)
            out = this->front ? Line(pts[1], pts[2]) : Line(pts[pts.size() - 2], pts[pts.size() - 3]);
        return out;
    }

    bool      other_hook_intersects(const Line &l, Point &pt) {
        std::optional<Line> h = other_hook();
        return h && h->intersection(l, &pt);
    }
    bool      other_hook_intersects(const Line &l) { Point pt; return this->other_hook_intersects(l, pt); }

    // Direction to intersect_point.
    Vec2d     intersect_line_dir() const throw() {
        return (this->intersect_point == intersect_line->a ? intersect_line->b - intersect_line->a : intersect_line->a - intersect_line->b).cast<double>();
    }
};

static inline Intersection* get_nearest_intersection(std::vector<std::pair<Intersection*, double>>& intersect_line, const size_t first_idx)
{
    assert(intersect_line.size() >= 2);
    bool take_next = false;
    if (first_idx == 0)
        take_next = true;
    else if (first_idx + 1 == intersect_line.size())
        take_next = false;
    else {
        // Has both prev and next.
        const std::pair<Intersection*, double> &ithis = intersect_line[first_idx];
        const std::pair<Intersection*, double> &iprev = intersect_line[first_idx - 1];
        const std::pair<Intersection*, double> &inext = intersect_line[first_idx + 1];
        take_next = iprev.first->fresh() && inext.first->fresh() ?
            inext.second - ithis.second < ithis.second - iprev.second :
            inext.first->fresh();
    }
    return intersect_line[take_next ? first_idx + 1 : first_idx - 1].first;
}

// Create a line representing the anchor aka hook extrusion based on line_to_offset 
// translated in the direction of the intersection line (intersection.intersect_line).
static Line create_offset_line(Line offset_line, const Intersection &intersection, const double scaled_offset)
{
    offset_line.translate((perp(intersection.closest_line->vector().cast<double>().normalized()) * (intersection.left ? scaled_offset : - scaled_offset)).cast<coord_t>());
    // Extend the line by a small value to guarantee a collision with adjacent lines
    offset_line.extend(coord_t(scaled_offset * 1.16)); // / cos(PI/6)
    return offset_line;
}

namespace bg  = boost::geometry;
namespace bgm = boost::geometry::model;
namespace bgi = boost::geometry::index;

// float is needed because for coord_t bgi::intersects throws "bad numeric conversion: positive overflow"
using rtree_point_t   = bgm::point<float, 2, boost::geometry::cs::cartesian>;
using rtree_segment_t = bgm::segment<rtree_point_t>;
using rtree_t         = bgi::rtree<std::pair<rtree_segment_t, size_t>, bgi::rstar<16, 4>>;

static inline rtree_point_t mk_rtree_point(const Point &pt) {
    return rtree_point_t(float(pt.x()), float(pt.y()));
}
static inline rtree_segment_t mk_rtree_seg(const Point &a, const Point &b) {
    return { mk_rtree_point(a), mk_rtree_point(b) };
}
static inline rtree_segment_t mk_rtree_seg(const Line &l) {
    return mk_rtree_seg(l.a, l.b);
}

// Create a hook based on hook_line and append it to the begin or end of the polyline in the intersection
static void add_hook(
    const Intersection &intersection, const double scaled_offset, 
    const coordf_t hook_length, double scaled_trim_distance, 
    const rtree_t &rtree, const Lines &lines_src)
{
    if (hook_length < SCALED_EPSILON)
        // Ignore open hooks.
        return;

#ifndef NDEBUG
    {
        const Vec2d  v  = (intersection.closest_line->b - intersection.closest_line->a).cast<double>();
        const Vec2d  va = (intersection.intersect_point - intersection.closest_line->a).cast<double>();
        const double l2 = v.squaredNorm();  // avoid a sqrt
        assert(l2 > 0.);
        const double t  = va.dot(v) / l2;
        assert(t > 0. && t < 1.);
        const double          d  = (t * v - va).norm();
        assert(d < 1000.);
    }
#endif // NDEBUG

    // Trim the hook start by the infill line it will connect to.
    Point hook_start;

    [[maybe_unused]] bool intersection_found = intersection.intersect_line->intersection(
        create_offset_line(*intersection.closest_line, intersection, scaled_offset),
        &hook_start);
    assert(intersection_found);

    std::optional<Line> other_hook = intersection.other_hook();

    Vec2d   hook_vector_norm = intersection.closest_line->vector().cast<double>().normalized();
    // hook_vector is extended by the thickness of the infill line, so that a collision is found against
    // the infill centerline to be later trimmed by the thickened line.
    Vector  hook_vector      = ((hook_length + 1.16 * scaled_trim_distance) * hook_vector_norm).cast<coord_t>();
    Line    hook_forward(hook_start, hook_start + hook_vector);

    auto filter_itself = [&intersection, &lines_src](const auto &item) { return item.second != (long unsigned int)(intersection.intersect_line - lines_src.data()); };

    std::vector<std::pair<rtree_segment_t, size_t>> hook_intersections;
    rtree.query(bgi::intersects(mk_rtree_seg(hook_forward)) && bgi::satisfies(filter_itself), std::back_inserter(hook_intersections));
    Point self_intersection_point;
    bool  self_intersection = other_hook && other_hook->intersection(hook_forward, &self_intersection_point);

    // Find closest intersection of a line segment starting with pt pointing in dir
    // with any of the hook_intersections, returns Euclidian distance.
    // dir is normalized.
    auto max_hook_length = [hook_length, scaled_trim_distance, &lines_src](
        const Vec2d &pt, const Vec2d &dir,
        const std::vector<std::pair<rtree_segment_t, size_t>> &hook_intersections,
        bool self_intersection, const std::optional<Line> &self_intersection_line, const Point &self_intersection_point) {
        // No hook is longer than hook_length, there shouldn't be any intersection closer than that.
        auto max_length = hook_length;
        auto update_max_length = [&max_length](double d) {
            if (d < max_length)
                max_length = d;
        };
        // Shift the trimming point away from the colliding thick line.
        auto shift_from_thick_line = [&dir, scaled_trim_distance](const Vec2d& dir2) {
            return scaled_trim_distance * std::abs(cross2(dir, dir2.normalized()));
        };

        for (const auto &hook_intersection : hook_intersections) {
            const rtree_segment_t &segment = hook_intersection.first;
            // Segment start and end points, segment vector.
            Vec2d pt2(bg::get<0, 0>(segment), bg::get<0, 1>(segment));
            Vec2d dir2 = Vec2d(bg::get<1, 0>(segment), bg::get<1, 1>(segment)) - pt2;
            // Find intersection of (pt, dir) with (pt2, dir2), where dir is normalized.
            double denom = cross2(dir, dir2);
            assert(std::abs(denom) > EPSILON);
            double t = cross2(pt2 - pt, dir2) / denom;
            if (hook_intersection.second < lines_src.size())
                // Trimming by another infill line. Reduce overlap.
                t -= shift_from_thick_line(dir2);
            update_max_length(t);
        }
        if (self_intersection) {
            double t = (self_intersection_point.cast<double>() - pt).dot(dir) - shift_from_thick_line((*self_intersection_line).vector().cast<double>());
            max_length = std::min(max_length, t);
        }
        return std::max(0., max_length);
    };

    Vec2d  hook_startf              = hook_start.cast<double>();
    double hook_forward_max_length  = max_hook_length(hook_startf, hook_vector_norm, hook_intersections, self_intersection, other_hook, self_intersection_point);
    double hook_backward_max_length = 0.;
    if (hook_forward_max_length < hook_length - SCALED_EPSILON) {
        // Try the other side.
        hook_intersections.clear();
        Line hook_backward(hook_start, hook_start - hook_vector);
        rtree.query(bgi::intersects(mk_rtree_seg(hook_backward)) && bgi::satisfies(filter_itself), std::back_inserter(hook_intersections));
        self_intersection = other_hook && other_hook->intersection(hook_backward, &self_intersection_point);
        hook_backward_max_length = max_hook_length(hook_startf, - hook_vector_norm, hook_intersections, self_intersection, other_hook, self_intersection_point);
    }

    // Take the longer hook.
    Vec2d hook_dir = (hook_forward_max_length > hook_backward_max_length ? hook_forward_max_length : - hook_backward_max_length) * hook_vector_norm;
    Point hook_end = hook_start + hook_dir.cast<coord_t>();

    Points &pl = intersection.intersect_pl->points;
    if (intersection.front) {
        pl.front() = hook_start;
        pl.emplace(pl.begin(), hook_end);
    } else {
        pl.back() = hook_start;
        pl.emplace_back(hook_end);
    }
}

#ifndef NDEBUG
bool validate_intersection_t_joint(const Intersection &intersection)
{
    const Vec2d  v = (intersection.closest_line->b - intersection.closest_line->a).cast<double>();
    const Vec2d  va = (intersection.intersect_point - intersection.closest_line->a).cast<double>();
    const double l2 = v.squaredNorm();  // avoid a sqrt
    assert(l2 > 0.);
    const double t = va.dot(v);
    assert(t > SCALED_EPSILON && t < l2 - SCALED_EPSILON);
    const double d = ((t / l2) * v - va).norm();
    assert(d < 1000.);
    return true;
}
bool validate_intersections(const std::vector<Intersection> &intersections)
{
    for (const Intersection& intersection : intersections)
        assert(validate_intersection_t_joint(intersection));
    return true;
}
#endif // NDEBUG

static Polylines connect_lines_using_hooks(Polylines &&lines, const ExPolygon &boundary, const double spacing, const coordf_t hook_length, const coordf_t hook_length_max)
{
    rtree_t rtree;
    size_t  poly_idx = 0;

    // 19% overlap, slightly lower than the allowed overlap in Fill::connect_infill()
    const float scaled_offset           = float(scale_(spacing) * 0.81);
    // 25% overlap
    const float scaled_trim_distance    = float(scale_(spacing) * 0.5 * 0.75);

    // Keeping the vector of closest points outside the loop, so the vector does not need to be reallocated.
    std::vector<std::pair<rtree_segment_t, size_t>> closest;
    // Pairs of lines touching at one end point. The pair is sorted to make the end point connection test symmetric.
    std::vector<std::pair<const Polyline*, const Polyline*>> lines_touching_at_endpoints;
    {
        // Insert infill lines into rtree, merge close collinear segments split by the infill boundary,
        // collect lines_touching_at_endpoints.
        double r2_close = Slic3r::sqr(1200.);
        for (Polyline &poly : lines) {
            assert(poly.points.size() == 2);
            if (&poly != lines.data()) {
                // Join collinear segments separated by a tiny gap. These gaps were likely created by clipping the infill lines with a concave dent in an infill boundary.
                auto collinear_segment = [&rtree, &closest, &lines, &lines_touching_at_endpoints, r2_close](const Point& pt, const Point& pt_other, const Polyline* polyline) -> std::pair<Polyline*, bool> {
                    closest.clear();
                    rtree.query(bgi::nearest(mk_rtree_point(pt), 1), std::back_inserter(closest));
                    const Polyline *other = &lines[closest.front().second];
                    double dist2_front = (other->points.front() - pt).cast<double>().squaredNorm();
                    double dist2_back  = (other->points.back() - pt).cast<double>().squaredNorm();
                    double dist2_min   = std::min(dist2_front, dist2_back);
                    if (dist2_min < r2_close) {
                        // Don't connect the segments in an opposite direction.
                        double dist2_min_other = std::min((other->points.front() - pt_other).cast<double>().squaredNorm(), (other->points.back() - pt_other).cast<double>().squaredNorm());
                        if (dist2_min_other > dist2_min) {
                            // End points of the two lines are very close, they should have been merged together if they are collinear.
                            Vec2d v1 = (pt_other - pt).cast<double>();
                            Vec2d v2 = (other->points.back() - other->points.front()).cast<double>();
                            Vec2d v1n = v1.normalized();
                            Vec2d v2n = v2.normalized();
                            // The vectors must not be collinear.
                            double d = v1n.dot(v2n);
                            if (std::abs(d) > 0.99f) {
                                // Lines are collinear, merge them.
                                rtree.remove(closest.front());
                                return std::make_pair(const_cast<Polyline*>(other), dist2_min == dist2_front);
                            } else {
                                if (polyline > other)
                                    std::swap(polyline, other);
                                lines_touching_at_endpoints.emplace_back(polyline, other);
                            }
                        }
                    }
                    return std::make_pair(static_cast<Polyline*>(nullptr), false);
                };
                auto collinear_front = collinear_segment(poly.points.front(), poly.points.back(),  &poly);
                auto collinear_back  = collinear_segment(poly.points.back(),  poly.points.front(), &poly);
                assert(! collinear_front.first || ! collinear_back.first || collinear_front.first != collinear_back.first);
                if (collinear_front.first) {
                    Polyline &other = *collinear_front.first;
                    assert(&other != &poly);
                    poly.points.front() = collinear_front.second ? other.points.back() : other.points.front();
                    other.points.clear();
                }
                if (collinear_back.first) {
                    Polyline &other = *collinear_back.first;
                    assert(&other != &poly);
                    poly.points.back() = collinear_back.second ? other.points.back() : other.points.front();
                    other.points.clear();
                }
            }
            rtree.insert(std::make_pair(mk_rtree_seg(poly.points.front(), poly.points.back()), poly_idx++));
        }
    }

    // Convert input polylines to lines_src after the colinear segments were merged.
    Lines lines_src;
    lines_src.reserve(lines.size());
    std::transform(lines.begin(), lines.end(), std::back_inserter(lines_src), [](const Polyline &pl) { 
        return pl.empty() ? Line(Point(0, 0), Point(0, 0)) : Line(pl.points.front(), pl.points.back()); });

    sort_remove_duplicates(lines_touching_at_endpoints);

    std::vector<Intersection> intersections;
    {
        // Minimum lenght of an infill line to anchor. Very short lines cannot be trimmed from both sides,
        // it does not help to anchor extremely short infill lines, it consumes too much plastic while not adding
        // to the object rigidity.
        assert(scaled_offset > scaled_trim_distance);
        const double line_len_threshold_drop_both_sides    = scaled_offset * (2. / cos(PI / 6.) + 0.5) + SCALED_EPSILON;
        const double line_len_threshold_anchor_both_sides  = line_len_threshold_drop_both_sides + scaled_offset;
        const double line_len_threshold_drop_single_side   = scaled_offset * (1. / cos(PI / 6.) + 1.5) + SCALED_EPSILON;
        const double line_len_threshold_anchor_single_side = line_len_threshold_drop_single_side + scaled_offset;
        for (size_t line_idx = 0; line_idx < lines.size(); ++ line_idx) {
            Polyline    &line        = lines[line_idx];
            if (line.points.empty())
                continue;

            Point &front_point = line.points.front();
            Point &back_point  = line.points.back();

            // Find the nearest line from the start point of the line.
            std::optional<size_t> tjoint_front, tjoint_back;
            {
                auto has_tjoint = [&closest, line_idx, &rtree, &lines, &lines_src](const Point &pt) {
                    auto filter_t_joint = [line_idx, &lines_src, pt](const auto &item) { 
                        if (item.second != line_idx) {
                            // Verify that the point projects onto the line.
                            const Line  &line = lines_src[item.second];
                            const Vec2d  v  = (line.b - line.a).cast<double>();
                            const Vec2d  va = (pt - line.a).cast<double>();
                            const double l2 = v.squaredNorm();  // avoid a sqrt
                            if (l2 > 0.) {
                                const double t = va.dot(v);
                                return t > SCALED_EPSILON && t < l2 - SCALED_EPSILON;
                            }
                        }
                        return false;
                    };
                    closest.clear();
                    rtree.query(bgi::nearest(mk_rtree_point(pt), 1) && bgi::satisfies(filter_t_joint), std::back_inserter(closest));
                    std::optional<size_t> out;
                    if (! closest.empty()) {
                        const Polyline &pl = lines[closest.front().second];
                        if (pl.points.empty()) {
                            // The closest infill line was already dropped as it was too short.
                            // Such an infill line should not make a T-joint anyways.
    #if 0 // #ifndef NDEBUG
                            const auto &seg = closest.front().first;
                            struct Linef { Vec2d a; Vec2d b; };
                            Linef l { { bg::get<0, 0>(seg), bg::get<0, 1>(seg) }, { bg::get<1, 0>(seg), bg::get<1, 1>(seg) } };
                            assert(line_alg::distance_to_squared(l, Vec2d(pt.cast<double>())) > 1000 * 1000);
    #endif // NDEBUG
                        } else if (pl.size() >= 2 && 
                            //FIXME Hoping that pl is really a line, trimmed by a polygon using ClipperUtils. Sometimes Clipper leaves some additional collinear points on the polyline, let's hope it is all right.
                            Line{ pl.front(), pl.back() }.distance_to_squared(pt) <= 1000 * 1000)
                            out = closest.front().second;
                    }
                    return out;
                };
                // Refuse to create a T-joint if the infill lines touch at their ends.
                auto filter_end_point_connections = [&lines_touching_at_endpoints, &lines, &line](std::optional<size_t> in) {
                    std::optional<size_t> out;
                    if (in) {
                        const Polyline *lo = &line;
                        const Polyline *hi = &lines[*in];
                        if (lo > hi)
                            std::swap(lo, hi);
                        if (! std::binary_search(lines_touching_at_endpoints.begin(), lines_touching_at_endpoints.end(), std::make_pair(lo, hi)))
                            // Not an end-point connection, it is a valid T-joint.
                            out = in;
                    }
                    return out;
                };
                tjoint_front = filter_end_point_connections(has_tjoint(front_point));
                tjoint_back  = filter_end_point_connections(has_tjoint(back_point));
            }

            int num_tjoints = int(tjoint_front.has_value()) + int(tjoint_back.has_value());
            if (num_tjoints > 0) {
                double line_len   = line.length();
                bool   drop       = false;
                bool   anchor     = false;
                if (num_tjoints == 1) {
                    // Connected to perimeters on a single side only, connected to another infill line on the other side.
                    drop   = line_len < line_len_threshold_drop_single_side;
                    anchor = line_len > line_len_threshold_anchor_single_side;
                } else {
                    // Not connected to perimeters at all, connected to two infill lines.
                    assert(num_tjoints == 2);                    
                    drop   = line_len < line_len_threshold_drop_both_sides;
                    anchor = line_len > line_len_threshold_anchor_both_sides;
                }
                if (drop) {
                    // Drop a very short line if connected to another infill line.
                    // Lines shorter than spacing are skipped because it is needed to shrink a line by the value of spacing.
                    // A shorter line than spacing could produce a degenerate polyline.
                    line.points.clear();
                } else if (anchor) {
                    if (tjoint_front) {
                        // T-joint of line's front point with the 'closest' line.
                        intersections.emplace_back(lines_src[*tjoint_front], lines_src[line_idx], &line, front_point, true);
                        assert(validate_intersection_t_joint(intersections.back()));
                    }
                    if (tjoint_back) {
                        // T-joint of line's back point with the 'closest' line.
                        intersections.emplace_back(lines_src[*tjoint_back],  lines_src[line_idx], &line, back_point,  false);
                        assert(validate_intersection_t_joint(intersections.back()));
                    }
                } else {
                    if (tjoint_front)
                        // T joint at the front at a 60 degree angle, the line is very short.
                        // Trim the front side.
                        front_point += ((scaled_trim_distance * 1.155) * (back_point - front_point).cast<double>().normalized()).cast<coord_t>();
                    if (tjoint_back)
                        // T joint at the front at a 60 degree angle, the line is very short.
                        // Trim the front side.
                        back_point  += ((scaled_trim_distance * 1.155) * (front_point - back_point).cast<double>().normalized()).cast<coord_t>();
                }
            }
        }
        // Remove those intersections, that point to a dropped line.
        for (auto it = intersections.begin(); it != intersections.end(); ) {
            assert(! lines[it->intersect_line - lines_src.data()].points.empty());
            if (lines[it->closest_line - lines_src.data()].points.empty()) {
                *it = intersections.back();
                intersections.pop_back();
            } else
                ++ it;
        }
    }
    assert(validate_intersections(intersections));

#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
    static int iRun = 0;
    int iStep = 0;
    {
        Points pts;
        for (const Intersection &i : intersections)
            pts.emplace_back(i.intersect_point);
        export_infill_lines_to_svg(boundary, lines, debug_out_path("FillAdaptive-Tjoints-%d.svg", iRun++), pts);
    }
#endif /* ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT */

    // Sort lexicographically by closest_line_idx and left/right orientation.
    std::sort(intersections.begin(), intersections.end(),
      [](const Intersection &i1, const Intersection &i2) {
            return (i1.closest_line == i2.closest_line) ?
                int(i1.left) < int(i2.left) :
                i1.closest_line < i2.closest_line;
        });

    std::vector<size_t> merged_with(lines.size());
    std::iota(merged_with.begin(), merged_with.end(), 0);

    // Appends the boundary polygon with all holes to rtree for detection to check whether hooks are not crossing the boundary
    {
        Point prev = boundary.contour.points.back();
        for (const Point &point : boundary.contour.points) {
            rtree.insert(std::make_pair(mk_rtree_seg(prev, point), poly_idx++));
            prev = point;
        }
        for (const Polygon &polygon : boundary.holes) {
            Point prev = polygon.points.back();
            for (const Point &point : polygon.points) {
                rtree.insert(std::make_pair(mk_rtree_seg(prev, point), poly_idx++));
                prev = point;
            }
        }
    }

    auto update_merged_polyline_idx = [&merged_with](size_t pl_idx) {
        // Update the polyline index to index which is merged
        for (size_t last = pl_idx;;) {
            size_t lower = merged_with[last];
            if (lower == last) {
                merged_with[pl_idx] = lower;
                return lower;
            }
            last = lower;
        }
        assert(false);
        return size_t(0);
    };
    auto update_merged_polyline = [&lines, update_merged_polyline_idx](Intersection& intersection) {
        // Update the polyline index to index which is merged
        size_t intersect_pl_idx = update_merged_polyline_idx(intersection.intersect_pl - lines.data());
        intersection.intersect_pl = &lines[intersect_pl_idx];
        // After polylines are merged, it is necessary to update "forward" based on if intersect_point is the first or the last point of intersect_pl.
        if (intersection.fresh()) {
            assert(intersection.intersect_pl->points.front() == intersection.intersect_point ||
                   intersection.intersect_pl->points.back() == intersection.intersect_point);
            intersection.front = intersection.intersect_pl->points.front() == intersection.intersect_point;
        }
    };

    // Merge polylines touching at their ends. This should be a very rare case, but it happens surprisingly often.
    for (auto it = lines_touching_at_endpoints.rbegin(); it != lines_touching_at_endpoints.rend(); ++ it) {
        Polyline *pl1 = const_cast<Polyline*>(it->first);
        Polyline *pl2 = const_cast<Polyline*>(it->second);
        assert(pl1 < pl2);
        // pl1 was visited for the 1st time.
        // pl2 may have alread been merged with another polyline, even with this one.
        pl2 = &lines[update_merged_polyline_idx(pl2 - lines.data())];
        assert(pl1 <= pl2);
        // Avoid closing a loop, ignore dropped infill lines.
        if (pl1 != pl2 && ! pl1->points.empty() && ! pl2->points.empty()) {
            // Merge the polylines.
            assert(pl1 < pl2);
            assert(pl1->points.size() >= 2);
            assert(pl2->points.size() >= 2);
            double d11 = (pl1->points.front() - pl2->points.front()).cast<double>().squaredNorm();
            double d12 = (pl1->points.front() - pl2->points.back()) .cast<double>().squaredNorm();
            double d21 = (pl1->points.back()  - pl2->points.front()).cast<double>().squaredNorm();
            double d22 = (pl1->points.back()  - pl2->points.back()) .cast<double>().squaredNorm();
            double d1min = std::min(d11, d12);
            double d2min = std::min(d21, d22);
            if (d1min < d2min) {
                pl1->reverse();
                if (d12 == d1min)
                    pl2->reverse();
            } else if (d22 == d2min)
                pl2->reverse();
            pl1->points.back() = (pl1->points.back() + pl2->points.front()) / 2;
            pl1->append(pl2->points.begin() + 1, pl2->points.end());
            pl2->points.clear();
            merged_with[pl2 - lines.data()] = pl1 - lines.data();
        }
    }

    // Keep intersect_line outside the loop, so it does not get reallocated.
    std::vector<std::pair<Intersection*, double>> intersect_line;
    for (size_t min_idx = 0; min_idx < intersections.size();) {
        intersect_line.clear();
        // All the nearest points (T-joints) ending at the same line are projected onto this line. Because of it, it can easily find the nearest point.
        {
            const Vec2d line_dir = intersections[min_idx].closest_line->vector().cast<double>();
            size_t max_idx = min_idx;
            for (; max_idx < intersections.size() && 
                    intersections[min_idx].closest_line == intersections[max_idx].closest_line &&
                    intersections[min_idx].left         == intersections[max_idx].left;
                    ++ max_idx)
                intersect_line.emplace_back(&intersections[max_idx], line_dir.dot(intersections[max_idx].intersect_point.cast<double>()));
            min_idx = max_idx;
            assert(intersect_line.size() > 0);
            // Sort the intersections along line_dir.
            std::sort(intersect_line.begin(), intersect_line.end(), [](const auto &i1, const auto &i2) { return i1.second < i2.second; });
        }

        if (intersect_line.size() == 1) {
            // Simple case: The current intersection is the only one touching its adjacent line.
            Intersection &first_i = *intersect_line.front().first;
            update_merged_polyline(first_i);
            if (first_i.fresh()) {
                // Try to connect left or right. If not enough space for hook_length, take the longer side.
#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
                export_infill_lines_to_svg(boundary, lines, debug_out_path("FillAdaptive-add_hook0-pre-%d-%d.svg", iRun, iStep), { first_i.intersect_point });
#endif // ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
                add_hook(first_i, scaled_offset, hook_length, scaled_trim_distance, rtree, lines_src);
#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
                export_infill_lines_to_svg(boundary, lines, debug_out_path("FillAdaptive-add_hook0-pre-%d-%d.svg", iRun, iStep), { first_i.intersect_point });
                ++ iStep;
#endif // ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
                first_i.used = true;
            }
            continue;
        }

        for (size_t first_idx = 0; first_idx < intersect_line.size(); ++ first_idx) {
            Intersection &first_i = *intersect_line[first_idx].first;
            update_merged_polyline(first_i);
            if (! first_i.fresh())
                // The intersection has been processed, or the polyline has been merged to another polyline.
                continue;

            // Get the previous or next intersection on the same line, pick the closer one.
            if (first_idx > 0)
                update_merged_polyline(*intersect_line[first_idx - 1].first);
            if (first_idx + 1 < intersect_line.size())
                update_merged_polyline(*intersect_line[first_idx + 1].first);
            Intersection &nearest_i = *get_nearest_intersection(intersect_line, first_idx);
            assert(first_i.closest_line == nearest_i.closest_line);
            assert(first_i.intersect_line != nearest_i.intersect_line);
            assert(first_i.intersect_line != first_i.closest_line);
            assert(nearest_i.intersect_line != first_i.closest_line);
            // A line between two intersections points
            Line offset_line = create_offset_line(Line(first_i.intersect_point, nearest_i.intersect_point), first_i, scaled_offset);
            // Check if both intersections lie on the offset_line and simultaneously get their points of intersecting.
            // These points are used as start and end of the hook
            Point first_i_point, nearest_i_point;
            bool could_connect = false;
            if (nearest_i.fresh()) {
                could_connect = first_i.intersect_line->intersection(offset_line, &first_i_point) &&
                                nearest_i.intersect_line->intersection(offset_line, &nearest_i_point);
                assert(could_connect);
            }
            Points &first_points  = first_i.intersect_pl->points;
            Points &second_points = nearest_i.intersect_pl->points;
            could_connect &= (nearest_i_point - first_i_point).cast<double>().squaredNorm() <= Slic3r::sqr(hook_length_max);
            if (could_connect) {
                // Both intersections are so close that their polylines can be connected.
                // Verify that no other infill line intersects this anchor line.
                closest.clear();
                rtree.query(
                    bgi::intersects(mk_rtree_seg(first_i_point, nearest_i_point)) &&
                    bgi::satisfies([&first_i, &nearest_i, &lines_src](const auto &item) 
                        { return item.second != (long unsigned int)(first_i.intersect_line - lines_src.data())
                              && item.second != (long unsigned int)(nearest_i.intersect_line - lines_src.data()); }),
                    std::back_inserter(closest));
                could_connect = closest.empty();
#if 0
                // Avoid self intersections. Maybe it is better to trim the self intersection after the connection?
                if (could_connect && first_i.intersect_pl != nearest_i.intersect_pl) {
                    Line l(first_i_point, nearest_i_point);
                    could_connect = ! first_i.other_hook_intersects(l) && ! nearest_i.other_hook_intersects(l);
                }
#endif
            }
            bool connected = false;
            if (could_connect) {
#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
                export_infill_lines_to_svg(boundary, lines, debug_out_path("FillAdaptive-connecting-pre-%d-%d.svg", iRun, iStep), { first_i.intersect_point, nearest_i.intersect_point });
#endif // ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
                // No other infill line intersects this anchor line. Extrude it as a whole.
                if (first_i.intersect_pl == nearest_i.intersect_pl) {
                    // Both intersections are on the same polyline, that means a loop is being closed.
                    assert(first_i.front != nearest_i.front);
                    if (! first_i.front)
                        std::swap(first_i_point, nearest_i_point);
                    first_points.front() = first_i_point;
                    first_points.back()  = nearest_i_point;
                    //FIXME trim the end of a closed loop a bit?
                    first_points.emplace(first_points.begin(), nearest_i_point);
                } else {
                    // Both intersections are on different polylines
                    Line  l(first_i_point, nearest_i_point);
                    l.translate((perp(first_i.closest_line->vector().cast<double>().normalized()) * (first_i.left ? scaled_trim_distance : - scaled_trim_distance)).cast<coord_t>());
                    Point pt_start, pt_end;
                    bool  trim_start = first_i  .intersect_pl->points.size() == 3 && first_i  .other_hook_intersects(l, pt_start);
                    bool  trim_end   = nearest_i.intersect_pl->points.size() == 3 && nearest_i.other_hook_intersects(l, pt_end);
                    first_points.reserve(first_points.size() + second_points.size());
                    if (first_i.front)
                        std::reverse(first_points.begin(), first_points.end());
                    if (trim_start)
                        first_points.front() = pt_start;
                    first_points.back() = first_i_point;
                    first_points.emplace_back(nearest_i_point);
                    if (nearest_i.front)
                        first_points.insert(first_points.end(), second_points.begin() + 1, second_points.end());
                    else
                        first_points.insert(first_points.end(), second_points.rbegin() + 1, second_points.rend());
                    if (trim_end)
                        first_points.back() = pt_end;
                    // Keep the polyline at the lower index slot.
                    if (first_i.intersect_pl < nearest_i.intersect_pl) {
                        second_points.clear();
                        merged_with[nearest_i.intersect_pl - lines.data()] = first_i.intersect_pl - lines.data();
                    } else {
                        second_points = std::move(first_points);
                        first_points.clear();
                        merged_with[first_i.intersect_pl - lines.data()] = nearest_i.intersect_pl - lines.data();
                    }
                }
                nearest_i.used = true;
                connected = true;
#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
                export_infill_lines_to_svg(boundary, lines, debug_out_path("FillAdaptive-connecting-post-%d-%d.svg", iRun, iStep), { first_i.intersect_point, nearest_i.intersect_point });
#endif // ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
            }
            if (! connected) {
                // Try to connect left or right. If not enough space for hook_length, take the longer side.
#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
                export_infill_lines_to_svg(boundary, lines, debug_out_path("FillAdaptive-add_hook-pre-%d-%d.svg", iRun, iStep), { first_i.intersect_point });
#endif // ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
                add_hook(first_i, scaled_offset, hook_length, scaled_trim_distance, rtree, lines_src);
#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
                export_infill_lines_to_svg(boundary, lines, debug_out_path("FillAdaptive-add_hook-post-%d-%d.svg", iRun, iStep), { first_i.intersect_point });
#endif // ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
            }
#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
            ++ iStep;
#endif // ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
            first_i.used = true;
        }
    }

    Polylines polylines_out;
    polylines_out.reserve(polylines_out.size() + std::count_if(lines.begin(), lines.end(), [](const Polyline &pl) { return !pl.empty(); }));
    for (Polyline &pl : lines)
        if (!pl.empty()) polylines_out.emplace_back(std::move(pl));
    return polylines_out;
}

#ifndef NDEBUG
bool has_no_collinear_lines(const Polylines &polylines)
{
    // Create line end point lookup.
    struct LineEnd {
        LineEnd(const Polyline *line, bool start) : line(line), start(start) {}
        const Polyline *line;
        // Is it the start or end point?
        bool            start;
        const Point&    point() const { return start ? line->points.front() : line->points.back(); }
        const Point&    other_point() const { return start ? line->points.back() : line->points.front(); }
        LineEnd         other_end() const { return LineEnd(line, !start); }
        Vec2d           vec() const { return Vec2d((this->other_point() - this->point()).cast<double>()); }
        bool operator==(const LineEnd &rhs) const { return this->line == rhs.line && this->start == rhs.start; }
    };
    struct LineEndAccessor {
        const Point* operator()(const LineEnd &pt) const { return &pt.point(); }
    };
    typedef ClosestPointInRadiusLookup<LineEnd, LineEndAccessor> ClosestPointLookupType;
    ClosestPointLookupType closest_end_point_lookup(coord_t(1001. * sqrt(2.)));
    for (const Polyline& pl : polylines) {
//        assert(pl.points.size() == 2);
        auto line_start = LineEnd(&pl, true);
        auto line_end   = LineEnd(&pl, false);

        auto assert_not_collinear = [&closest_end_point_lookup](const LineEnd &line_start) {
            std::vector<std::pair<const LineEnd*, double>> hits = closest_end_point_lookup.find_all(line_start.point());
            for (const std::pair<const LineEnd*, double> &hit : hits)
                if ((line_start.point() - hit.first->point()).cwiseAbs().maxCoeff() <= 1000) {
                    // End points of the two lines are very close, they should have been merged together if they are collinear.
                    Vec2d v1 = line_start.vec();
                    Vec2d v2 = hit.first->vec();
                    Vec2d v1n = v1.normalized();
                    Vec2d v2n = v2.normalized();
                    // The vectors must not be collinear.
                    assert(std::abs(v1n.dot(v2n)) < cos(M_PI / 12.));
                }
        };
        assert_not_collinear(line_start);
        assert_not_collinear(line_end);

        closest_end_point_lookup.insert(line_start);
        closest_end_point_lookup.insert(line_end);
    }

    return true;
}
#endif

void Filler::_fill_surface_single(
    const FillParams              &params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point> &direction,
    ExPolygon                      expolygon,
    Polylines                     &polylines_out)
{
    assert (this->adapt_fill_octree);

    Polylines all_polylines;
    {
        // 3 contexts for three directions of infill lines
        std::array<FillContext, 3> contexts { 
            FillContext { *adapt_fill_octree, this->z, 0 },
            FillContext { *adapt_fill_octree, this->z, 1 },
            FillContext { *adapt_fill_octree, this->z, 2 }
        };
        // Generate the infill lines along the octree cells, merge touching lines of the same direction.
        size_t num_lines = 0;
        for (auto &context : contexts) {
            generate_infill_lines_recursive(context, adapt_fill_octree->root_cube, 0, int(adapt_fill_octree->cubes_properties.size()) - 1);
            num_lines += context.output_lines.size() + context.temp_lines.size();
        }

#if 0
        // Collect the lines, trim them by the expolygon.
        all_polylines.reserve(num_lines);
        auto boundary = to_polygons(expolygon);
        for (auto &context : contexts) {
            Polylines lines;
            lines.reserve(context.output_lines.size() + context.temp_lines.size());
            std::transform(context.output_lines.begin(), context.output_lines.end(), std::back_inserter(lines), [](const Line& l) { return Polyline{ l.a, l.b }; });
            for (const Line &l : context.temp_lines)
                if (l.a.x() != std::numeric_limits<coord_t>::max())
                    lines.push_back({ l.a, l.b });
            // Crop all polylines
            append(all_polylines, intersection_pl(std::move(lines), boundary));
        }
//        assert(has_no_collinear_lines(all_polylines));        
#else
        // Collect the lines.
        std::vector<Line> lines;
        lines.reserve(num_lines);
        for (auto &context : contexts) {
            append(lines, context.output_lines);
            for (const Line &line : context.temp_lines)
                if (line.a.x() != std::numeric_limits<coord_t>::max())
                    lines.emplace_back(line);
        }
        // Convert lines to polylines.
        all_polylines.reserve(lines.size());
        std::transform(lines.begin(), lines.end(), std::back_inserter(all_polylines), [](const Line& l) { return Polyline{ l.a, l.b }; });

        // Apply multiline offset if needed
        multiline_fill(all_polylines, params, spacing);

        // Crop all polylines
        all_polylines = intersection_pl(std::move(all_polylines), expolygon);
#endif
    }

    if (params.multiline == 1) {
        // After intersection_pl some polylines with only one line are split into more lines
        for (Polyline& polyline : all_polylines) {
            // FIXME assert that all the points are collinear and in between the start and end point.
            if (polyline.points.size() > 2)
                polyline.points.erase(polyline.points.begin() + 1, polyline.points.end() - 1);
        }
        //    assert(has_no_collinear_lines(all_polylines));

#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
        {
            static int iRun = 0;
            export_infill_lines_to_svg(expolygon, all_polylines, debug_out_path("FillAdaptive-initial-%d.svg", iRun++));
        }
#endif /* ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT */

        const auto hook_length     = coordf_t(std::min<float>(std::numeric_limits<coord_t>::max(), scale_(params.anchor_length)));
        const auto hook_length_max = coordf_t(std::min<float>(std::numeric_limits<coord_t>::max(), scale_(params.anchor_length_max)));

    Polylines all_polylines_with_hooks = all_polylines.size() > 1 ? connect_lines_using_hooks(std::move(all_polylines), expolygon, this->spacing, hook_length, hook_length_max) : std::move(all_polylines);

#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
        {
            static int iRun = 0;
            export_infill_lines_to_svg(expolygon, all_polylines_with_hooks, debug_out_path("FillAdaptive-hooks-%d.svg", iRun++));
        }
#endif /* ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT */

        chain_or_connect_infill(std::move(all_polylines_with_hooks), expolygon, polylines_out, this->spacing, params);
    } else { 
        // if multiline  is > 1 infill is ready to connect
        chain_or_connect_infill(std::move(all_polylines), expolygon, polylines_out, this->spacing, params);
    }

#ifdef ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT
    {
        static int iRun = 0;
        export_infill_lines_to_svg(expolygon, polylines_out, debug_out_path("FillAdaptive-final-%d.svg", iRun ++));
    }
#endif /* ADAPTIVE_CUBIC_INFILL_DEBUG_OUTPUT */
}

//static double bbox_max_radius(const BoundingBoxf3 &bbox, const Vec3d &center)
//{
//    const auto p = (bbox.min - center);
//    const auto s = bbox.size();
//    double r2max = 0.;
//    for (int i = 0; i < 8; ++ i)
//        r2max = std::max(r2max, (p + Vec3d(s.x() * double(i & 1), s.y() * double(i & 2), s.z() * double(i & 4))).squaredNorm());
//    return sqrt(r2max);
//}

static std::vector<CubeProperties> make_cubes_properties(double max_cube_edge_length, double line_spacing)
{
    max_cube_edge_length += EPSILON;

    std::vector<CubeProperties> cubes_properties;
    for (double edge_length = line_spacing * 2.;; edge_length *= 2.)
    {
        CubeProperties props{};
        props.edge_length = edge_length;
        props.height = edge_length * sqrt(3);
        props.diagonal_length = edge_length * sqrt(2);
        props.line_z_distance = edge_length / sqrt(3);
        props.line_xy_distance = edge_length / sqrt(6);
        cubes_properties.emplace_back(props);
        if (edge_length > max_cube_edge_length)
            break;
    }
    // Orca: Ensure at least 2 levels so build_octree() will insert triangles.
    // Fixes scenario where adaptive fill is disconnected from walls on low densities
    if (cubes_properties.size() == 1) {
        CubeProperties p = cubes_properties.back();
        p.edge_length      *= 2.0;
        p.height           = p.edge_length * sqrt(3);
        p.diagonal_length  = p.edge_length * sqrt(2);
        p.line_z_distance  = p.edge_length / sqrt(3);
        p.line_xy_distance = p.edge_length / sqrt(6);
        cubes_properties.push_back(p);
    }
    return cubes_properties;
}

static inline bool is_overhang_triangle(const Vec3d &a, const Vec3d &b, const Vec3d &c, const Vec3d &up)
{
    // Calculate triangle normal.
    Vec3d n = (b - a).cross(c - b);
    return n.dot(up) > 0.707 * n.norm();
}

static void transform_center(Cube *current_cube, const Eigen::Matrix3d &rot)
{
#ifndef NDEBUG
    current_cube->center_octree = current_cube->center;
#endif // NDEBUG
    current_cube->center = rot * current_cube->center;
    for (auto *child : current_cube->children)
        if (child)
            transform_center(child, rot);
}

OctreePtr build_octree(
    // Mesh is rotated to the coordinate system of the octree.
    const indexed_triangle_set  &triangle_mesh,
    // Overhang triangles extracted from fill surfaces with stInternalBridge type,
    // rotated to the coordinate system of the octree.
    const std::vector<Vec3d>    &overhang_triangles, 
    coordf_t                     line_spacing,
    bool                         support_overhangs_only)
{
    assert(line_spacing > 0);
    assert(! std::isnan(line_spacing));

    BoundingBox3Base<Vec3f>     bbox(triangle_mesh.vertices);
    Vec3d                       cube_center      = bbox.center().cast<double>();
    std::vector<CubeProperties> cubes_properties = make_cubes_properties(double(bbox.size().maxCoeff()), line_spacing);
    auto                        octree           = OctreePtr(new Octree(cube_center, cubes_properties));

    if (cubes_properties.size() > 1) {
        Octree *octree_ptr = octree.get();
        double edge_length_half = 0.5 * cubes_properties.back().edge_length;
        Vec3d  diag_half(edge_length_half, edge_length_half, edge_length_half);
        int    max_depth = int(cubes_properties.size()) - 1;
        auto process_triangle = [octree_ptr, max_depth, diag_half](const Vec3d &a, const Vec3d &b, const Vec3d &c) {
            octree_ptr->insert_triangle(
                a, b, c,
                octree_ptr->root_cube,
                BoundingBoxf3(octree_ptr->root_cube->center - diag_half, octree_ptr->root_cube->center + diag_half),
                max_depth);
        };
        auto up_vector = support_overhangs_only ? Vec3d(transform_to_octree() * Vec3d(0., 0., 1.)) : Vec3d();
        for (auto &tri : triangle_mesh.indices) {
            Vec3d a = triangle_mesh.vertices[tri[0]].cast<double>();
            Vec3d b = triangle_mesh.vertices[tri[1]].cast<double>();
            Vec3d c = triangle_mesh.vertices[tri[2]].cast<double>();
            if (! support_overhangs_only || is_overhang_triangle(a, b, c, up_vector))
                process_triangle(a, b, c);
        }
        for (size_t i = 0; i < overhang_triangles.size(); i += 3)
            process_triangle(overhang_triangles[i], overhang_triangles[i + 1], overhang_triangles[i + 2]);
        {
            // Transform the octree to world coordinates to reduce computation when extracting infill lines.
            auto rot = transform_to_world().toRotationMatrix();
            transform_center(octree->root_cube, rot);
            octree->origin = rot * octree->origin;
        }
    }

    return octree;
}

void Octree::insert_triangle(const Vec3d &a, const Vec3d &b, const Vec3d &c, Cube *current_cube, const BoundingBoxf3 &current_bbox, int depth)
{
    assert(current_cube);
    assert(depth > 0);

    --depth;

    // Squared radius of a sphere around the child cube.
    // const double r2_cube = Slic3r::sqr(0.5 * this->cubes_properties[depth].height + EPSILON);

    for (size_t i = 0; i < 8; ++ i) {
        const Vec3d &child_center_dir = child_centers[i];
        // Calculate a slightly expanded bounding box of a child cube to cope with triangles touching a cube wall and other numeric errors.
        // We will rather densify the octree a bit more than necessary instead of missing a triangle.
        BoundingBoxf3 bbox;
        for (int k = 0; k < 3; ++ k) {
            if (child_center_dir[k] == -1.) {
                bbox.min[k] = current_bbox.min[k];
                bbox.max[k] = current_cube->center[k] + EPSILON;
            } else {
                bbox.min[k] = current_cube->center[k] - EPSILON;
                bbox.max[k] = current_bbox.max[k];
            }
        }
        Vec3d child_center = current_cube->center + (child_center_dir * (this->cubes_properties[depth].edge_length / 2.));
        //if (dist2_to_triangle(a, b, c, child_center) < r2_cube) {
        // dist2_to_triangle and r2_cube are commented out too.
        if (triangle_AABB_intersects(a, b, c, bbox)) {
            if (! current_cube->children[i])
                current_cube->children[i] = this->pool.construct(child_center);
            if (depth > 0)
                this->insert_triangle(a, b, c, current_cube->children[i], bbox, depth);
        }
    }
}

} // namespace FillAdaptive
} // namespace Slic3r

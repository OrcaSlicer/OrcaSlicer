#include "VoronoiInfillCgal.hpp"

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Regular_triangulation_2.h>
#include <CGAL/Delaunay_triangulation_3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <iterator>
#include <utility>
#include <vector>

namespace Slic3r::Voronoi {
namespace {

bool clip_edge(const Vec2d &min, const Vec2d &max, const Vec2d &origin, const Vec2d &direction, double &lo, double &hi)
{
    for (int axis = 0; axis < 2; ++axis) {
        if (direction[axis] == 0.) {
            if (origin[axis] < min[axis] || origin[axis] > max[axis])
                return false;
        } else {
            double a = (min[axis] - origin[axis]) / direction[axis];
            double b = (max[axis] - origin[axis]) / direction[axis];
            if (a > b)
                std::swap(a, b);
            lo = std::max(lo, a);
            hi = std::min(hi, b);
        }
    }
    return lo < hi;
}

// Convex polygon clipping against normal.dot(XY) <= limit, in millimeters.
void clip_polygon(std::vector<Vec2d> &polygon, const Vec2d &normal, double limit)
{
    if (polygon.empty())
        return;
    std::vector<Vec2d> clipped;
    Vec2d previous = polygon.back();
    double previous_distance = normal.dot(previous) - limit;
    for (const Vec2d &current : polygon) {
        const double distance = normal.dot(current) - limit;
        if ((distance <= 0.) != (previous_distance <= 0.))
            clipped.push_back(previous + (current - previous) * (previous_distance / (previous_distance - distance)));
        if (distance <= 0.)
            clipped.push_back(current);
        previous = current;
        previous_distance = distance;
    }
    polygon = std::move(clipped);
}

} // namespace

Section section(const std::vector<Vec3d> &sites, const BoundingBox &bounds, double z)
{
    using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Triangulation = CGAL::Regular_triangulation_2<Kernel>;
    // At fixed z, squared 3D distance is squared XY distance + (z-site.z)^2.
    // Its nearest-site partition is exactly a power diagram with weight
    // -(z-site.z)^2, including hidden sites whose cells do not meet this plane.
    const Vec2d center = (bounds.min.cast<double>() + bounds.max.cast<double>()) * (0.5 * SCALING_FACTOR);
    const Vec2d min = bounds.min.cast<double>() * SCALING_FACTOR - center;
    const Vec2d max = bounds.max.cast<double>() * SCALING_FACTOR - center;
    std::vector<Kernel::Weighted_point_2> input;
    input.reserve(sites.size());
    for (const Vec3d &site : sites) {
        const double dz = site.z() - z;
        input.emplace_back(Kernel::Point_2(site.x() - center.x(), site.y() - center.y()), -dz * dz);
    }
    Triangulation triangulation(input.begin(), input.end());
    if (triangulation.dimension() < 0)
        return {};

    auto point = [](const Kernel::Point_2 &p) { return Vec2d(CGAL::to_double(p.x()), CGAL::to_double(p.y())); };
    auto vector = [](const Kernel::Vector_2 &v) { return Vec2d(CGAL::to_double(v.x()), CGAL::to_double(v.y())); };
    Section result;
    result.covering_radius = 0.;
    auto measure = [&](const Vec2d &xy, const Kernel::Weighted_point_2 &site) {
        const Vec2d delta = xy - point(site.point());
        const double distance = std::sqrt(std::max(0., delta.squaredNorm() - CGAL::to_double(site.weight())));
        result.covering_radius = std::max(result.covering_radius, distance);
    };
    // On each clipped power cell, squared nearest-site distance is convex.
    // Its maximum occurs at a cell vertex: rectangle corners, clipped edge
    // endpoints, or interior diagram vertices. A one-site section still has
    // boundary corners even though it has no printable walls.
    for (const Vec2d &corner : std::array<Vec2d, 4>{min, max, Vec2d(min.x(), max.y()), Vec2d(max.x(), min.y())})
        measure(corner, triangulation.nearest_power_vertex(Kernel::Point_2(corner.x(), corner.y()))->point());
    if (triangulation.dimension() == 0) return result;
    Polylines &edges = result.walls;
    for (auto edge = triangulation.finite_edges_begin(); edge != triangulation.finite_edges_end(); ++edge) {
        const CGAL::Object dual = triangulation.dual(edge);
        Vec2d origin, direction;
        double lo = 0., hi = std::numeric_limits<double>::infinity();
        if (const auto *segment = CGAL::object_cast<Kernel::Segment_2>(&dual)) {
            origin = point(segment->source());
            direction = point(segment->target()) - origin;
            hi = 1.;
        } else if (const auto *ray = CGAL::object_cast<Kernel::Ray_2>(&dual)) {
            origin = point(ray->source());
            direction = vector(ray->to_vector());
        } else if (const auto *line = CGAL::object_cast<Kernel::Line_2>(&dual)) {
            origin = point(line->point(0));
            direction = vector(line->to_vector());
            lo = -hi;
        } else
            continue;
        if (!origin.allFinite() || !direction.allFinite() || direction.squaredNorm() == 0. ||
            !clip_edge(min, max, origin, direction, lo, hi))
            continue;
        const Vec2d a = (center + origin + lo * direction) / SCALING_FACTOR;
        const Vec2d b = (center + origin + hi * direction) / SCALING_FACTOR;
        // Both defining sites are nearest everywhere on this dual edge.
        const auto &site = edge->first->vertex(triangulation.ccw(edge->second))->point();
        measure((origin + lo * direction).eval(), site);
        measure((origin + hi * direction).eval(), site);
        const Point p(a.x(), a.y()), q(b.x(), b.y());
        if (p != q)
            edges.emplace_back(Points{p, q});
    }
    if (triangulation.dimension() == 2) {
        // Read metadata after geometry extraction. Keep the original CGAL vertex
        // type and insertion range: changing these can flip degenerate edges and
        // affect how walls on floor boundaries are clipped.
        auto key = [](const Kernel::Weighted_point_2 &p) {
            return std::array<double, 3>{CGAL::to_double(p.x()), CGAL::to_double(p.y()), CGAL::to_double(p.weight())};
        };
        std::map<std::array<double, 3>, size_t> site_indices;
        for (size_t i = 0; i < input.size(); ++i) {
            auto inserted = site_indices.emplace(key(input[i]), i);
            if (!inserted.second && inserted.first->second != size_t(-1) &&
                sites[inserted.first->second] != sites[i])
                inserted.first->second = size_t(-1); // Distinct XYZ sites with coincident power sites.
        }
        for (auto face = triangulation.finite_faces_begin(); face != triangulation.finite_faces_end(); ++face) {
            const Vec2d xy = point(triangulation.dual(face));
            if (!xy.allFinite() || xy.x() < min.x() || xy.x() > max.x() || xy.y() < min.y() || xy.y() > max.y())
                continue;
            measure(xy, face->vertex(0)->point());
            const Vec2d scaled = (xy + center) / SCALING_FACTOR;
            std::array<size_t, 3> indices;
            for (size_t i = 0; i < 3; ++i)
                indices[i] = site_indices.at(key(face->vertex(int(i))->point()));
            if (std::find(indices.begin(), indices.end(), size_t(-1)) != indices.end())
                continue;
            result.junctions.push_back({Point(scaled.x(), scaled.y()),
                {sites[indices[0]], sites[indices[1]], sites[indices[2]]}});
        }
    }
    return result;
}

std::vector<Band> shallow_bands(const std::vector<Vec3d> &sites, const BoundingBox &bounds,
                              double z, double width, double height, double row_spacing)
{
    using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Triangulation = CGAL::Delaunay_triangulation_3<Kernel>;
    const Vec2d center = (bounds.min.cast<double>() + bounds.max.cast<double>()) * (0.5 * SCALING_FACTOR);
    const Vec2d min = bounds.min.cast<double>() * SCALING_FACTOR - center;
    const Vec2d max = bounds.max.cast<double>() * SCALING_FACTOR - center;
    std::vector<Kernel::Point_3> points;
    points.reserve(sites.size());
    for (const Vec3d &site : sites)
        points.emplace_back(site.x() - center.x(), site.y() - center.y(), site.z() - z);
    Triangulation triangulation(points.begin(), points.end());
    auto position = [](Triangulation::Vertex_handle vertex) {
        const auto &p = vertex->point();
        return Vec3d(CGAL::to_double(p.x()), CGAL::to_double(p.y()), CGAL::to_double(p.z()));
    };
    std::vector<std::pair<std::array<double, 6>, Band>> faces;
    auto face = [&](Triangulation::Vertex_handle va, Triangulation::Vertex_handle vb,
                    std::vector<Triangulation::Vertex_handle> neighbors) {
        Vec3d a = position(va), b = position(vb);
        if (std::lexicographical_compare(b.data(), b.data() + 3, a.data(), a.data() + 3)) {
            std::swap(a, b);
            std::swap(va, vb);
        }
        const Vec3d normal = b - a;
        const double horizontal = normal.head<2>().norm();
        // A single bead suffices when its shift per layer is at most half its width.
        if (normal.z() == 0. || 2. * height * std::abs(normal.z()) <= width * horizontal)
            return;
        const double plane = normal.dot((a + b) * 0.5);
        const double face_z = plane / normal.z();
        if (horizontal == 0. && !(face_z > -height && face_z <= 0.))
            return;
        std::vector<Vec2d> polygon {min, Vec2d(max.x(), min.y()), max, Vec2d(min.x(), max.y())};
        // On this bisector z = face_z - slope.dot(XY). Project the part
        // between z-height and z+height; adjacent equal-height bands overlap by half.
        const Vec2d slope = normal.head<2>() / normal.z();
        double row_origin = std::numeric_limits<double>::quiet_NaN();
        if (horizontal != 0.) {
            const double slope_length = horizontal / std::abs(normal.z());
            const double strip_width = 2. * height / slope_length;
            const double rounded_width = std::ceil(strip_width / row_spacing) * row_spacing;
            // Keep the supported boundary; extend only the advancing side to
            // accommodate whole rows at Orca's unmodified flow spacing.
            clip_polygon(polygon, slope, face_z + height);
            clip_polygon(polygon, -slope, height - face_z + (rounded_width - strip_width) * slope_length);
            row_origin = (face_z + height) / slope_length + center.dot(slope / slope_length);
        }
        if (triangulation.dimension() == 3)
            triangulation.finite_adjacent_vertices(va, std::back_inserter(neighbors));
        for (auto other : neighbors) {
            if (other == vb)
                continue;
            const Vec3d c = position(other), n = c - a;
            clip_polygon(polygon, n.head<2>() - n.z() * slope,
                         n.dot((a + c) * 0.5) - n.z() * face_z);
            if (polygon.empty())
                return;
        }
        Polygon contour;
        for (const Vec2d &p : polygon) {
            const Vec2d scaled = (p + center) / SCALING_FACTOR;
            const Point point(scaled.x(), scaled.y());
            if (contour.points.empty() || contour.points.back() != point)
                contour.points.push_back(point);
        }
        if (contour.points.size() > 1 && contour.points.front() == contour.points.back())
            contour.points.pop_back();
        if (contour.points.size() >= 3 && contour.area() > 0.) {
            const Vec2d supported_side = horizontal == 0. ? Vec2d(1., 0.) :
                Vec2d(normal.head<2>() * (normal.z() > 0. ? 1. : -1.) / horizontal);
            const std::array<double, 6> key {a.x(), a.y(), a.z(), b.x(), b.y(), b.z()};
            faces.emplace_back(key, Band{ExPolygon(std::move(contour)), supported_side, row_origin});
        }
    };
    if (triangulation.dimension() == 3) {
        for (auto edge = triangulation.finite_edges_begin(); edge != triangulation.finite_edges_end(); ++edge) {
            auto a = edge->first->vertex(edge->second), b = edge->first->vertex(edge->third);
            face(a, b, {});
        }
    } else {
        // Coplanar and collinear inputs have no tetrahedra from which to obtain
        // face neighbors, so use all other sites as half-space constraints.
        std::vector<Triangulation::Vertex_handle> vertices;
        for (auto vertex = triangulation.finite_vertices_begin(); vertex != triangulation.finite_vertices_end(); ++vertex)
            vertices.push_back(vertex);
        for (size_t i = 0; i < vertices.size(); ++i) {
            std::vector<Triangulation::Vertex_handle> neighbors = vertices;
            neighbors.erase(neighbors.begin() + i);
            for (size_t j = i + 1; j < vertices.size(); ++j)
                face(vertices[i], vertices[j], neighbors);
        }
    }
    // CGAL's insertion/traversal order can vary between independent fills.
    // Give overlapping bands stable ownership using their defining site pair.
    std::sort(faces.begin(), faces.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<Band> bands;
    bands.reserve(faces.size());
    for (auto &face : faces)
        bands.push_back(std::move(face.second));
    return bands;
}

} // namespace Slic3r::Voronoi

#include "VoronoiRouting.hpp"
#include "../KDTreeIndirect.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r::Voronoi {
namespace {

size_t other(const WallGraph &graph, size_t edge, size_t vertex)
{
    return graph.edges[edge][0] == vertex ? graph.edges[edge][1] : graph.edges[edge][0];
}

uint64_t mix(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

bool site_less(const Vec3d &a, const Vec3d &b)
{
    return std::lexicographical_compare(a.data(), a.data() + 3, b.data(), b.data() + 3);
}

// Exact world coordinates identify persistent sites for any sampler, without
// imposing tile identities on structured or future nonuniform point fields.
uint64_t site_hash(const std::array<Vec3d, 3> &sites)
{
    uint64_t hash = 0x564f524f4e4f49ULL;
    for (const Vec3d &site : sites)
        for (int axis = 0; axis < 3; ++axis) {
            const double coordinate = site[axis] == 0. ? 0. : site[axis];
            uint64_t bits;
            static_assert(sizeof(bits) == sizeof(coordinate));
            std::memcpy(&bits, &coordinate, sizeof(bits));
            hash = mix(hash ^ bits);
        }
    return hash;
}

} // namespace

WallGraph build_wall_graph(const Polylines &walls, coord_t tolerance)
{
    WallGraph graph;
    Points points;
    for (const Polyline &wall : walls)
        append(points, wall.points);
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    // Buckets contain only fixed representatives, never already-snapped points.
    std::map<Point, std::vector<size_t>> buckets;
    const double pitch = double(std::max(coord_t(1), tolerance));
    for (const Point &point : points) {
        const Point key(coord_t(std::floor(double(point.x()) / pitch)), coord_t(std::floor(double(point.y()) / pitch)));
        size_t best = no_connection;
        double distance = double(tolerance) * double(tolerance);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                auto bucket = buckets.find(key + Point(dx, dy));
                if (bucket == buckets.end())
                    continue;
                for (size_t v : bucket->second) {
                    const double d = (point.cast<double>() - graph.vertices[v].point.cast<double>()).squaredNorm();
                    if (d <= distance && (best == no_connection || d < distance || v < best)) {
                        best = v;
                        distance = d;
                    }
                }
            }
        if (best == no_connection) {
            best = graph.vertices.size();
            graph.vertices.push_back({point, {}});
            buckets[key].push_back(best);
        }
        graph.indices.emplace(point, best);
    }
    for (const Polyline &wall : walls)
        for (size_t i = 1; i < wall.points.size(); ++i) {
            const size_t a = graph.indices.at(wall.points[i - 1]), b = graph.indices.at(wall.points[i]);
            if (a == b)
                continue;
            const size_t e = graph.edges.size();
            graph.edges.push_back({a, b});
            graph.vertices[a].edges.push_back(e);
            graph.vertices[b].edges.push_back(e);
        }
    // Stable fallback connections and tracing independent of CGAL edge order.
    for (size_t v = 0; v < graph.vertices.size(); ++v)
        std::sort(graph.vertices[v].edges.begin(), graph.vertices[v].edges.end(), [&](size_t a, size_t b) {
            const size_t va = other(graph, a, v), vb = other(graph, b, v);
            return va == vb ? a < b : va < vb;
        });
    return graph;
}

Connections pair_junctions(const WallGraph &graph, const std::vector<Junction> &junctions, size_t layer_id)
{
    Connections connections(graph.edges.size(), {no_connection, no_connection});
    // Multiple generating triples at one snapped vertex are degenerate, not a
    // genuine three-way junction. They use straight pairing / stable fallback.
    std::vector<const Junction *> metadata(graph.vertices.size(), nullptr);
    std::vector<bool> ambiguous(graph.vertices.size(), false);
    auto coordinate = [&graph](size_t i, size_t axis) { return double(graph.vertices[i].point[axis]); };
    KDTreeIndirect<2, double, decltype(coordinate)> vertices(coordinate, graph.vertices.size());
    for (const Junction &junction : junctions) {
        auto found = graph.indices.find(junction.point);
        size_t v = found == graph.indices.end() ? no_connection : found->second;
        if (v == no_connection) {
            // Evaluating a dual endpoint through a segment and directly through
            // its face can round to neighboring fixed-point coordinates.
            v = find_closest_point(vertices, junction.point.cast<double>().eval());
            if (v == no_connection || (graph.vertices[v].point.cast<double>() - junction.point.cast<double>()).norm() > scale_(0.000008))
                continue; // Clipped away or replaced by a floor.
        }
        if (metadata[v])
            ambiguous[v] = true;
        metadata[v] = &junction;
    }
    auto pair = [&](size_t v, size_t a, size_t b) {
        connections[a][graph.edges[a][0] == v ? 0 : 1] = b;
        connections[b][graph.edges[b][0] == v ? 0 : 1] = a;
    };
    const double straight_limit = -std::cos(0.1 * M_PI / 180.);
    for (size_t v = 0; v < graph.vertices.size(); ++v) {
        const auto &incident = graph.vertices[v].edges;
        if (incident.size() < 3) {
            if (incident.size() == 2) pair(v, incident[0], incident[1]);
            continue;
        }
        std::vector<Vec2d> directions;
        directions.reserve(incident.size());
        for (size_t edge : incident)
            directions.push_back(graph.vertices[other(graph, edge, v)].point.cast<double>() - graph.vertices[v].point.cast<double>());
        std::vector<size_t> remaining = incident;
        struct Pair { double cosine; size_t a, b; };
        std::vector<Pair> straight;
        for (size_t i = 0; i < incident.size(); ++i)
            for (size_t j = i + 1; j < incident.size(); ++j) {
                const Vec2d &a = directions[i], &b = directions[j];
                const double cosine = a.dot(b) / (a.norm() * b.norm());
                if (cosine <= straight_limit)
                    straight.push_back({cosine, i, j});
            }
        std::sort(straight.begin(), straight.end(), [](const Pair &a, const Pair &b) {
            if (a.cosine != b.cosine) return a.cosine < b.cosine;
            return std::make_pair(a.a, a.b) < std::make_pair(b.a, b.b);
        });
        for (const Pair &p : straight) {
            const size_t a = incident[p.a], b = incident[p.b];
            auto ia = std::find(remaining.begin(), remaining.end(), a);
            auto ib = std::find(remaining.begin(), remaining.end(), b);
            if (ia == remaining.end() || ib == remaining.end())
                continue;
            pair(v, a, b);
            remaining.erase(std::remove_if(remaining.begin(), remaining.end(), [&](size_t e) { return e == a || e == b; }), remaining.end());
        }
        if (incident.size() == 3 && remaining.size() == 3 && metadata[v] && !ambiguous[v]) {
            auto sites = metadata[v]->sites;
            std::sort(sites.begin(), sites.end(), site_less);
            constexpr std::array<std::array<size_t, 2>, 3> faces {{{0, 1}, {0, 2}, {1, 2}}};
            double errors[3][3];
            bool valid = true;
            for (size_t face = 0; face < 3; ++face) {
                const Vec2d normal = (sites[faces[face][1]] - sites[faces[face][0]]).head<2>();
                const double length = normal.norm();
                if (length == 0.) { valid = false; break; }
                for (size_t branch = 0; branch < 3; ++branch) {
                    errors[face][branch] = std::abs(normal.dot(directions[branch])) / length;
                }
            }
            if (valid) {
                std::array<size_t, 3> permutation {0, 1, 2}, best = permutation;
                double best_error = std::numeric_limits<double>::infinity();
                do {
                    double error = 0.;
                    for (size_t face = 0; face < 3; ++face) error += errors[face][permutation[face]];
                    if (error < best_error) { best = permutation; best_error = error; }
                } while (std::next_permutation(permutation.begin(), permutation.end()));
                if (best_error <= 3. * double(scale_(0.000008))) {
                    const uint64_t hash = site_hash(sites);
                    const size_t phase = hash % 3;
                    const size_t step = (hash >> 8) & 1 ? 1 : 2;
                    const size_t unpaired = (phase + step * (layer_id % 3)) % 3;
                    pair(v, remaining[best[(unpaired + 1) % 3]], remaining[best[(unpaired + 2) % 3]]);
                    continue;
                }
            }
        }
        // Degree two joins, boundaries, ambiguous triples and unmatched higher
        // degree branches need no site lookup or persistent tracking state.
        for (size_t i = 1; i < remaining.size(); i += 2)
            pair(v, remaining[i - 1], remaining[i]);
    }
    return connections;
}

Polylines trace_trails(const WallGraph &graph, const Connections &connections, size_t layer_id)
{
    assert(connections.size() == graph.edges.size());
    std::vector<bool> used(graph.edges.size(), false);
    Polylines trails;
    auto trace = [&](size_t v, size_t edge) {
        Polyline trail;
        trail.points.push_back(graph.vertices[v].point);
        while (edge != no_connection && !used[edge]) {
            used[edge] = true;
            v = other(graph, edge, v);
            trail.points.push_back(graph.vertices[v].point);
            edge = connections[edge][graph.edges[edge][0] == v ? 0 : 1];
        }
        if (trail.points.size() > 1) {
            if (trail.first_point() == trail.last_point()) {
                trail.points.pop_back();
                const size_t offset = layer_id % trail.points.size();
                std::rotate(trail.points.begin(), trail.points.begin() + offset, trail.points.end());
                trail.points.push_back(trail.points.front());
            }
            trails.push_back(std::move(trail));
        }
    };
    for (size_t v = 0; v < graph.vertices.size(); ++v)
        for (size_t e : graph.vertices[v].edges)
            if (!used[e] && connections[e][graph.edges[e][0] == v ? 0 : 1] == no_connection)
                trace(v, e);
    for (size_t v = 0; v < graph.vertices.size(); ++v)
        for (size_t e : graph.vertices[v].edges)
            if (!used[e])
                trace(v, e);
    return trails;
}

std::vector<PathGroup> order_groups(std::vector<PathGroup> groups, const Point &start_near)
{
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                               [](const PathGroup &group) { return group.paths.empty(); }), groups.end());
    struct Candidate { Point entry; size_t group; bool reverse; };
    std::vector<Candidate> candidates;
    candidates.reserve(2 * groups.size());
    for (size_t i = 0; i < groups.size(); ++i) {
        const Point entry = groups[i].paths.front().first_point();
        const Point exit = groups[i].paths.back().last_point();
        candidates.push_back({entry, i, false});
        if (groups[i].reversible) candidates.push_back({exit, i, true});
    }
    auto coordinate = [&candidates](size_t i, size_t axis) { return double(candidates[i].entry[axis]); };
    KDTreeIndirect<2, double, decltype(coordinate)> tree(coordinate);
    std::vector<bool> used(groups.size(), false);
    std::vector<size_t> indices(candidates.size());
    std::iota(indices.begin(), indices.end(), 0);
    tree.build(indices);
    size_t built = groups.size();
    Point current = start_near;
    std::vector<PathGroup> ordered;
    ordered.reserve(groups.size());
    while (ordered.size() < groups.size()) {
        const size_t i = find_closest_point(tree, current.cast<double>().eval(),
            [&](size_t index) { return !used[candidates[index].group]; });
        assert(i != no_connection);
        const Candidate &candidate = candidates[i];
        used[candidate.group] = true;
        PathGroup &group = groups[candidate.group];
        if (candidate.reverse) {
            std::reverse(group.paths.begin(), group.paths.end());
            for (Polyline &path : group.paths) path.reverse();
        }
        current = group.paths.back().last_point();
        ordered.push_back(std::move(group));
        const size_t remaining = groups.size() - ordered.size();
        // Rebuild without consumed candidates to avoid quadratic filtering.
        if (remaining > 0 && remaining * 2 < built) {
            indices.clear();
            for (size_t j = 0; j < candidates.size(); ++j)
                if (!used[candidates[j].group]) indices.push_back(j);
            tree.build(indices);
            built = remaining;
        }
    }
    return ordered;
}

Polylines order_trails(Polylines trails, const Point &start_near)
{
    std::vector<PathGroup> groups;
    groups.reserve(trails.size());
    for (Polyline &trail : trails) {
        Polylines paths;
        paths.push_back(std::move(trail));
        groups.push_back({std::move(paths), true});
    }
    Polylines ordered;
    ordered.reserve(trails.size());
    for (PathGroup &group : order_groups(std::move(groups), start_near))
        ordered.push_back(std::move(group.paths.front()));
    return ordered;
}

} // namespace Slic3r::Voronoi

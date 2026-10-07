#pragma once

#include "VoronoiInfillCgal.hpp"
#include <unordered_map>
#include <array>
#include <cstddef>
#include <vector>

namespace Slic3r::Voronoi {

// Routing stages consume geometry; they never generate or move control points.
struct WallGraph {
    struct Vertex { Point point; std::vector<size_t> edges; };
    std::vector<Vertex> vertices;
    std::vector<std::array<size_t, 2>> edges;
    // Original fixed-point coordinates to their bounded snap representatives.
    std::unordered_map<Point, size_t, PointHash> indices;
};
using Connections = std::vector<std::array<size_t, 2>>;
constexpr size_t no_connection = size_t(-1);

// Fixed representatives prevent transitive drift. Zero-length edges disappear;
// every other segment, including parallel duplicates, remains a distinct edge.
WallGraph build_wall_graph(const Polylines &walls, coord_t tolerance = scale_(0.000008));
Connections pair_junctions(const WallGraph &graph, const std::vector<Junction> &junctions, size_t layer_id);
Polylines trace_trails(const WallGraph &graph, const Connections &connections, size_t layer_id);
Polylines order_trails(Polylines trails, const Point &start_near);
struct PathGroup {
    Polylines paths;
    bool reversible { false };
};
// Wall trails may reverse; floor groups retain their supported-side-first sweep.
std::vector<PathGroup> order_groups(std::vector<PathGroup> groups, const Point &start_near);

} // namespace Slic3r::Voronoi

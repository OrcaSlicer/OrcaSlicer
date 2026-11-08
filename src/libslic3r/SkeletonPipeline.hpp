#pragma once

#include "Skeleton.hpp"
#include "ObjectID.hpp"
#include "TriangleMesh.hpp"
#include <Eigen/Geometry>
#include <map>
#include <optional>
#include <vector>

namespace Slic3r {

// Mesh chunk produced by skeleton cutting. One per skeleton node (set after cut).
struct SkeletonSegment {
    ObjectID    object_id;        // ModelObject that holds this chunk after cut
    Vec3d       print_axis;       // world-space unit print direction for this segment
    Transform3d print_orientation;// full orientation matrix (axis + origin)
};

// Skeleton node extended with pipeline data, index-parallel to SkeletonGraph::nodes.
struct SkeletonPipelineNode {
    Vec3d                      position;  // local-space position (mirrors SkeletonGraph::nodes[i])
    std::vector<int>           neighbors; // undirected adjacency (mirrors SkeletonGraph::adj[i])
    std::optional<SkeletonSegment> segment; // populated after perform_skeleton_cut
};

// Top-level object: one per source ModelObject, keyed by its ObjectID.
struct MeshSkeleton {
    ObjectID                          source_id;   // original ModelObject before any cuts
    SkeletonGraph                     graph;        // undirected graph
    SkeletonDAG                       dag;          // directed form, root = lowest-Z node
    std::vector<SkeletonPipelineNode> nodes;        // index-parallel to graph.nodes

    int num_nodes() const { return (int)nodes.size(); }

    // Rebuild nodes[] from graph so positions and adjacency are in sync.
    // Preserves any segment data already stored.
    void sync_from_graph()
    {
        const int n = graph.num_nodes();
        nodes.resize(n);
        for (int i = 0; i < n; ++i) {
            nodes[i].position  = graph.nodes[i];
            nodes[i].neighbors = graph.adj[i];
        }
    }
};

// Application-lifetime registry: one MeshSkeleton per ModelObject, keyed by ObjectID.
// Owned by Plater; accessed via wxGetApp().plater()->skeleton_registry().
using SkeletonRegistry = std::map<ObjectID, MeshSkeleton>;

} // namespace Slic3r

#include <iostream>
#include <vector>
#include <unordered_map>
#include <map>

#include "ItsNeighborIndex.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include "libslic3r/Execution/ExecutionSeq.hpp"

#include "tbb/parallel_sort.h"

namespace Slic3r {

FaceNeighborIndex its_create_neighbors_index_1(const indexed_triangle_set &its)
{
    // Just to be clear what type of object are we referencing
    using FaceID = size_t;
    using VertexID = uint64_t;
    using EdgeID = uint64_t;

    constexpr auto UNASSIGNED = std::numeric_limits<FaceID>::max();

    struct Edge // Will contain IDs of the two facets touching this edge
    {
        FaceID first, second;
        Edge() : first{UNASSIGNED}, second{UNASSIGNED} {}
        void   assign(FaceID fid)
        {
            first == UNASSIGNED ? first = fid : second = fid;
        }
    };

    // All vertex IDs will fit into this number of bits. (Used for hashing)
    const int max_vertex_id_bits = std::ceil(std::log2(its.vertices.size()));
    assert(max_vertex_id_bits <= 32);

    std::unordered_map< EdgeID, Edge> edge_index;

    // Edge id is constructed by concatenating two vertex ids, starting with
    // the lowest in MSB
    auto hash = [max_vertex_id_bits] (VertexID a, VertexID b) {
        if (a > b) std::swap(a, b);
        return (a << max_vertex_id_bits) + b;
    };

    // Go through all edges of all facets and mark the facets touching each edge
    for (size_t face_id = 0; face_id < its.indices.size(); ++face_id) {
        const Vec3i32 &face = its.indices[face_id];

        EdgeID e1 = hash(face(0), face(1)), e2 = hash(face(1), face(2)),
               e3 = hash(face(2), face(0));

        edge_index[e1].assign(face_id);
        edge_index[e2].assign(face_id);
        edge_index[e3].assign(face_id);
    }

    FaceNeighborIndex index(its.indices.size());

    // Now collect the neighbors for each facet into the final index
    for (size_t face_id = 0; face_id < its.indices.size(); ++face_id) {
        const Vec3i32 &face = its.indices[face_id];

        EdgeID e1 = hash(face(0), face(1)), e2 = hash(face(1), face(2)),
               e3 = hash(face(2), face(0));

        const Edge &neighs1 = edge_index[e1];
        const Edge &neighs2 = edge_index[e2];
        const Edge &neighs3 = edge_index[e3];

        std::array<size_t, 3> &neighs = index[face_id];
        neighs[0] = neighs1.first == face_id ? neighs1.second : neighs1.first;
        neighs[1] = neighs2.first == face_id ? neighs2.second : neighs2.first;
        neighs[2] = neighs3.first == face_id ? neighs3.second : neighs3.first;
    }

    return index;
}

std::vector<Vec3i32> its_create_neighbors_index_2(const indexed_triangle_set &its)
{
    std::vector<Vec3i32> out(its.indices.size(), Vec3i32(-1, -1, -1));

    // Create a mapping from triangle edge into face.
    struct EdgeToFace {
        // Index of the 1st vertex of the triangle edge. vertex_low <= vertex_high.
        int  vertex_low;
        // Index of the 2nd vertex of the triangle edge.
        int  vertex_high;
        // Index of a triangular face.
        int  face;
        // Index of edge in the face, starting with 1. Negative indices if the edge was stored reverse in (vertex_low, vertex_high).
        int  face_edge;
        bool operator==(const EdgeToFace &other) const { return vertex_low == other.vertex_low && vertex_high == other.vertex_high; }
        bool operator<(const EdgeToFace &other) const { return vertex_low < other.vertex_low || (vertex_low == other.vertex_low && vertex_high < other.vertex_high); }
    };
    std::vector<EdgeToFace> edges_map;
    edges_map.assign(its.indices.size() * 3, EdgeToFace());
    for (uint32_t facet_idx = 0; facet_idx < its.indices.size(); ++ facet_idx)
        for (int i = 0; i < 3; ++ i) {
            EdgeToFace &e2f = edges_map[facet_idx * 3 + i];
            e2f.vertex_low  = its.indices[facet_idx][i];
            e2f.vertex_high = its.indices[facet_idx][(i + 1) % 3];
            e2f.face        = facet_idx;
            // 1 based indexing, to be always strictly positive.
            e2f.face_edge   = i + 1;
            if (e2f.vertex_low > e2f.vertex_high) {
                // Sort the vertices
                std::swap(e2f.vertex_low, e2f.vertex_high);
                // and make the face_edge negative to indicate a flipped edge.
                e2f.face_edge = - e2f.face_edge;
            }
        }

    std::sort(edges_map.begin(), edges_map.end());

    // Assign a unique common edge id to touching triangle edges.
    int num_edges = 0;
    for (size_t i = 0; i < edges_map.size(); ++ i) {
        EdgeToFace &edge_i = edges_map[i];
        if (edge_i.face == -1)
            // This edge has been connected to some neighbor already.
            continue;
        // Unconnected edge. Find its neighbor with the correct orientation.
        size_t j;
        bool found = false;
        for (j = i + 1; j < edges_map.size() && edge_i == edges_map[j]; ++ j)
            if (edge_i.face_edge * edges_map[j].face_edge < 0 && edges_map[j].face != -1) {
                // Faces touching with opposite oriented edges and none of the edges is connected yet.
                found = true;
                break;
            }
        if (! found) {
            //FIXME Vojtech: Trying to find an edge with equal orientation. This smells.
            // admesh can assign the same edge ID to more than two facets (which is
            // still topologically correct), so we have to search for a duplicate of
            // this edge too in case it was already seen in this orientation
            for (j = i + 1; j < edges_map.size() && edge_i == edges_map[j]; ++ j)
                if (edges_map[j].face != -1) {
                    // Faces touching with equally oriented edges and none of the edges is connected yet.
                    found = true;
                    break;
                }
        }
        // Assign an edge index to the 1st face.
        //        out[edge_i.face](std::abs(edge_i.face_edge) - 1) = num_edges;
        if (found) {
            EdgeToFace &edge_j = edges_map[j];
            out[edge_i.face](std::abs(edge_i.face_edge) - 1) = edge_j.face;
            out[edge_j.face](std::abs(edge_j.face_edge) - 1) = edge_i.face;
            // Mark the edge as connected.
            edge_j.face = -1;
        }
        ++ num_edges;
    }

    return out;
}

std::vector<Vec3i32> its_create_neighbors_index_3(const indexed_triangle_set &its)
{
    std::vector<Vec3i32> out(its.indices.size(), Vec3i32(-1, -1, -1));

    // Create a mapping from triangle edge into face.
    struct EdgeToFace {
        // Index of the 1st vertex of the triangle edge. vertex_low <= vertex_high.
        int  vertex_low;
        // Index of the 2nd vertex of the triangle edge.
        int  vertex_high;
        // Index of a triangular face.
        int  face;
        // Index of edge in the face, starting with 1. Negative indices if the edge was stored reverse in (vertex_low, vertex_high).
        int  face_edge;
        bool operator==(const EdgeToFace &other) const { return vertex_low == other.vertex_low && vertex_high == other.vertex_high; }
        bool operator<(const EdgeToFace &other) const { return vertex_low < other.vertex_low || (vertex_low == other.vertex_low && vertex_high < other.vertex_high); }
    };
    std::vector<EdgeToFace> edges_map;
    edges_map.assign(its.indices.size() * 3, EdgeToFace());
    for (uint32_t facet_idx = 0; facet_idx < its.indices.size(); ++ facet_idx)
        for (int i = 0; i < 3; ++ i) {
            EdgeToFace &e2f = edges_map[facet_idx * 3 + i];
            e2f.vertex_low  = its.indices[facet_idx][i];
            e2f.vertex_high = its.indices[facet_idx][(i + 1) % 3];
            e2f.face        = facet_idx;
            // 1 based indexing, to be always strictly positive.
            e2f.face_edge   = i + 1;
            if (e2f.vertex_low > e2f.vertex_high) {
                // Sort the vertices
                std::swap(e2f.vertex_low, e2f.vertex_high);
                // and make the face_edge negative to indicate a flipped edge.
                e2f.face_edge = - e2f.face_edge;
            }
        }

    tbb::parallel_sort(edges_map.begin(), edges_map.end());

    // Assign a unique common edge id to touching triangle edges.
    int num_edges = 0;
    for (size_t i = 0; i < edges_map.size(); ++ i) {
        EdgeToFace &edge_i = edges_map[i];
        if (edge_i.face == -1)
            // This edge has been connected to some neighbor already.
            continue;
        // Unconnected edge. Find its neighbor with the correct orientation.
        size_t j;
        bool found = false;
        for (j = i + 1; j < edges_map.size() && edge_i == edges_map[j]; ++ j)
            if (edge_i.face_edge * edges_map[j].face_edge < 0 && edges_map[j].face != -1) {
                // Faces touching with opposite oriented edges and none of the edges is connected yet.
                found = true;
                break;
            }
        if (! found) {
            //FIXME Vojtech: Trying to find an edge with equal orientation. This smells.
            // admesh can assign the same edge ID to more than two facets (which is
            // still topologically correct), so we have to search for a duplicate of
            // this edge too in case it was already seen in this orientation
            for (j = i + 1; j < edges_map.size() && edge_i == edges_map[j]; ++ j)
                if (edges_map[j].face != -1) {
                    // Faces touching with equally oriented edges and none of the edges is connected yet.
                    found = true;
                    break;
                }
        }
        // Assign an edge index to the 1st face.
        //        out[edge_i.face](std::abs(edge_i.face_edge) - 1) = num_edges;
        if (found) {
            EdgeToFace &edge_j = edges_map[j];
            out[edge_i.face](std::abs(edge_i.face_edge) - 1) = edge_j.face;
            out[edge_j.face](std::abs(edge_j.face_edge) - 1) = edge_i.face;
            // Mark the edge as connected.
            edge_j.face = -1;
        }
        ++ num_edges;
    }

    return out;
}

FaceNeighborIndex its_create_neighbors_index_4(const indexed_triangle_set &its)
{
    // Just to be clear what type of object are we referencing
    using FaceID = size_t;
    using VertexID = uint64_t;
    using EdgeID = uint64_t;

    constexpr auto UNASSIGNED = std::numeric_limits<FaceID>::max();

    struct Edge // Will contain IDs of the two facets touching this edge
    {
        FaceID first, second;
        Edge() : first{UNASSIGNED}, second{UNASSIGNED} {}
        void   assign(FaceID fid)
        {
            first == UNASSIGNED ? first = fid : second = fid;
        }
    };

    Benchmark bm;
    bm.start();

    // All vertex IDs will fit into this number of bits. (Used for hashing)
    //    const int max_vertex_id_bits = std::ceil(std::log2(its.vertices.size()));
    //    assert(max_vertex_id_bits <= 32);

    const uint64_t Vn  = its.vertices.size();
    //    const uint64_t Fn  = 3 * its.indices.size();
    //    double MaxQ = double(Vn) * (Vn + 1) / Fn;
    //    const uint64_t Nq = MaxQ < 0 ? 0 : std::ceil(std::log2(MaxQ));
    //    const uint64_t Nr = std::ceil(std::log2(std::min(Vn * (Vn + 1), Fn)));
    //    const uint64_t Nfn = std::ceil(std::log2(Fn));

    ////    const uint64_t max_edge_ids = (uint64_t(1) << (Nq + Nr));
    //    const uint64_t max_edge_ids = MaxQ * Fn + (std::min(Vn * (Vn + 1), Fn)); //(uint64_t(1) << Nfn);
    const uint64_t Fn  = 3 * its.indices.size();
    std::vector< Edge > edge_index(3 * Fn);

    // Edge id is constructed by concatenating two vertex ids, starting with
    // the lowest in MSB
    auto hash = [Vn, Fn /*, Nr*/] (VertexID a, VertexID b) {
        if (a > b) std::swap(a, b);

        uint64_t C = Vn * a + b;
        uint64_t Q = C / Fn, R = C % Fn;

        return Q * Fn + R;
    };

    // Go through all edges of all facets and mark the facets touching each edge
    for (size_t face_id = 0; face_id < its.indices.size(); ++face_id) {
        const Vec3i32 &face = its.indices[face_id];

        EdgeID e1 = hash(face(0), face(1)), e2 = hash(face(1), face(2)),
               e3 = hash(face(2), face(0));

        edge_index[e1].assign(face_id);
        edge_index[e2].assign(face_id);
        edge_index[e3].assign(face_id);
    }

    FaceNeighborIndex index(its.indices.size());

    // Now collect the neighbors for each facet into the final index
    for (size_t face_id = 0; face_id < its.indices.size(); ++face_id) {
        const Vec3i32 &face = its.indices[face_id];

        EdgeID e1 = hash(face(0), face(1)), e2 = hash(face(1), face(2)),
               e3 = hash(face(2), face(0));

        const Edge &neighs1 = edge_index[e1];
        const Edge &neighs2 = edge_index[e2];
        const Edge &neighs3 = edge_index[e3];

        std::array<size_t, 3> &neighs = index[face_id];
        neighs[0] = neighs1.first == face_id ? neighs1.second : neighs1.first;
        neighs[1] = neighs2.first == face_id ? neighs2.second : neighs2.first;
        neighs[2] = neighs3.first == face_id ? neighs3.second : neighs3.first;
    }

    bm.stop();

    std::cout << "Creating neighbor index took: " << bm.getElapsedSec() << " seconds." << std::endl;

    return index;
}

// Create an index of faces belonging to each vertex. The returned vector can
// be indexed with vertex indices and contains a list of face indices for each
// vertex.
std::vector<std::vector<size_t>> create_vertex_faces_index(const indexed_triangle_set &its)
{
    std::vector<std::vector<size_t>> index;

    if (! its.vertices.empty()) {
        size_t res = its.indices.size() / its.vertices.size();
        index.assign(its.vertices.size(), reserve_vector<size_t>(res));
        for (size_t fi = 0; fi < its.indices.size(); ++fi) {
            auto &face = its.indices[fi];
            index[face(0)].emplace_back(fi);
            index[face(1)].emplace_back(fi);
            index[face(2)].emplace_back(fi);
        }
    }

    return index;
}

//static int get_vertex_index(size_t vertex_index, const stl_triangle_vertex_indices &triangle_indices) {
//    if (vertex_index == triangle_indices[0]) return 0;
//    if (vertex_index == triangle_indices[1]) return 1;
//    if (vertex_index == triangle_indices[2]) return 2;
//    return -1;
//}

//static Vec2crd get_edge_indices(int edge_index, const stl_triangle_vertex_indices &triangle_indices)
//{
//    int next_edge_index = (edge_index == 2) ? 0 : edge_index + 1;
//    coord_t vi0             = triangle_indices[edge_index];
//    coord_t vi1             = triangle_indices[next_edge_index];
//    return Vec2crd(vi0, vi1);
//}

static std::vector<std::vector<size_t>> create_vertex_faces_index(
    const std::vector<stl_triangle_vertex_indices>& indices, size_t count_vertices)
{
    if (count_vertices == 0) return {};
    std::vector<std::vector<size_t>> index;
    size_t res = indices.size() / count_vertices;
    index.assign(count_vertices, reserve_vector<size_t>(res));
    for (size_t fi = 0; fi < indices.size(); ++fi) {
        auto &face = indices[fi];
        index[face(0)].emplace_back(fi);
        index[face(1)].emplace_back(fi);
        index[face(2)].emplace_back(fi);
    }
    return index;
}

std::vector<Vec3crd> its_create_neighbors_index_5(const indexed_triangle_set &its)
{
    const std::vector<stl_triangle_vertex_indices> &indices = its.indices;
    size_t vertices_size = its.vertices.size();

    if (indices.empty() || vertices_size == 0) return {};
    std::vector<std::vector<size_t>> vertex_triangles = create_vertex_faces_index(indices, vertices_size);
    coord_t              no_value = -1;
    std::vector<Vec3crd> neighbors(indices.size(), Vec3crd(no_value, no_value, no_value));
    for (const stl_triangle_vertex_indices& triangle_indices : indices) {
        coord_t index = &triangle_indices - &indices.front();
        Vec3crd& neighbor = neighbors[index];
        for (int edge_index = 0; edge_index < 3; ++edge_index) {
            // check if done
            coord_t& neighbor_edge = neighbor[edge_index];
            if (neighbor_edge != no_value) continue;
            Vec2crd edge_indices = get_edge_indices(edge_index, triangle_indices);
            // IMPROVE: use same vector for 2 sides of triangle
            const std::vector<size_t> &faces = vertex_triangles[edge_indices[0]];
            for (const size_t &face : faces) {
                if (face <= index) continue;
                const stl_triangle_vertex_indices &face_indices = indices[face];
                int vertex_index = get_vertex_index(edge_indices[1], face_indices);
                // NOT Contain second vertex?
                if (vertex_index < 0) continue;
                // Has NOT oposit direction?
                if (edge_indices[0] != face_indices[(vertex_index + 1) % 3]) continue;
                neighbor_edge = face;
                neighbors[face][vertex_index] = index;
                break;
            }
            // must be paired
            assert(neighbor_edge != no_value);
        }
    }

    return neighbors;
}

std::vector<std::array<size_t, 3>> its_create_neighbors_index_6(const indexed_triangle_set &its)
{
    constexpr auto UNASSIGNED_EDGE = std::numeric_limits<uint64_t>::max();
    constexpr auto UNASSIGNED_FACE = std::numeric_limits<size_t>::max();
    struct Edge
    {
        uint64_t id      = UNASSIGNED_EDGE;
        size_t   face_id = UNASSIGNED_FACE;
        bool operator < (const Edge &e) const { return id < e.id; }
    };

    const size_t facenum = its.indices.size();

    // All vertex IDs will fit into this number of bits. (Used for hashing)
    const int max_vertex_id_bits = std::ceil(std::log2(its.vertices.size()));
    assert(max_vertex_id_bits <= 32);

    // Edge id is constructed by concatenating two vertex ids, starting with
    // the lowest in MSB
    auto hash = [max_vertex_id_bits] (uint64_t a, uint64_t b) {
        if (a > b) std::swap(a, b);
        return (a << max_vertex_id_bits) + b;
    };

    std::vector<Edge> edge_map(3 * facenum);

    // Go through all edges of all facets and mark the facets touching each edge
    for (size_t face_id = 0; face_id < facenum; ++face_id) {
        const Vec3i32 &face = its.indices[face_id];

        edge_map[face_id * 3] = {hash(face(0), face(1)), face_id};
        edge_map[face_id * 3 + 1] = {hash(face(1), face(2)), face_id};
        edge_map[face_id * 3 + 2] = {hash(face(2), face(0)), face_id};
    }

    std::sort(edge_map.begin(), edge_map.end());

    std::vector<std::array<size_t, 3>> out(facenum, {UNASSIGNED_FACE, UNASSIGNED_FACE, UNASSIGNED_FACE});

    auto add_neighbor = [](std::array<size_t, 3> &slot, size_t face_id) {
        if (slot[0] == UNASSIGNED_FACE) { slot[0] = face_id; return; }
        if (slot[1] == UNASSIGNED_FACE) { slot[1] = face_id; return; }
        if (slot[2] == UNASSIGNED_FACE) { slot[2] = face_id; return; }
    };

    for (auto it = edge_map.begin(); it != edge_map.end();) {
        size_t face_id = it->face_id;
        uint64_t edge_id = it->id;

        while (++it != edge_map.end() &&  (it->id == edge_id)) {
            size_t other_face_id = it->face_id;
            add_neighbor(out[other_face_id], face_id);
            add_neighbor(out[face_id], other_face_id);
        }
    }

    return out;
}


std::vector<std::array<size_t, 3>> its_create_neighbors_index_7(const indexed_triangle_set &its)
{
    constexpr auto UNASSIGNED_EDGE = std::numeric_limits<uint64_t>::max();
    constexpr auto UNASSIGNED_FACE = std::numeric_limits<size_t>::max();
    struct Edge
    {
        uint64_t id      = UNASSIGNED_EDGE;
        size_t   face_id = UNASSIGNED_FACE;
        bool operator < (const Edge &e) const { return id < e.id; }
    };

    const size_t facenum = its.indices.size();

    // All vertex IDs will fit into this number of bits. (Used for hashing)
    const int max_vertex_id_bits = std::ceil(std::log2(its.vertices.size()));
    assert(max_vertex_id_bits <= 32);

    // Edge id is constructed by concatenating two vertex ids, starting with
    // the lowest in MSB
    auto hash = [max_vertex_id_bits] (uint64_t a, uint64_t b) {
        if (a > b) std::swap(a, b);
        return (a << max_vertex_id_bits) + b;
    };

    std::vector<Edge> edge_map(3 * facenum);

    // Go through all edges of all facets and mark the facets touching each edge
    for (size_t face_id = 0; face_id < facenum; ++face_id) {
        const Vec3i32 &face = its.indices[face_id];

        edge_map[face_id * 3] = {hash(face(0), face(1)), face_id};
        edge_map[face_id * 3 + 1] = {hash(face(1), face(2)), face_id};
        edge_map[face_id * 3 + 2] = {hash(face(2), face(0)), face_id};
    }

    tbb::parallel_sort(edge_map.begin(), edge_map.end());

    std::vector<std::array<size_t, 3>> out(facenum, {UNASSIGNED_FACE, UNASSIGNED_FACE, UNASSIGNED_FACE});

    auto add_neighbor = [](std::array<size_t, 3> &slot, size_t face_id) {
        if (slot[0] == UNASSIGNED_FACE) { slot[0] = face_id; return; }
        if (slot[1] == UNASSIGNED_FACE) { slot[1] = face_id; return; }
        if (slot[2] == UNASSIGNED_FACE) { slot[2] = face_id; return; }
    };

    for (auto it = edge_map.begin(); it != edge_map.end();) {
        size_t face_id = it->face_id;
        uint64_t edge_id = it->id;

        while (++it != edge_map.end() &&  (it->id == edge_id)) {
            size_t other_face_id = it->face_id;
            add_neighbor(out[other_face_id], face_id);
            add_neighbor(out[face_id], other_face_id);
        }
    }

    return out;
}

FaceNeighborIndex its_create_neighbors_index_8(const indexed_triangle_set &its)
{
    // Just to be clear what type of object are we referencing
    using FaceID = size_t;
    using VertexID = uint64_t;
    using EdgeID = uint64_t;

    constexpr auto UNASSIGNED = std::numeric_limits<FaceID>::max();

    struct Edge // Will contain IDs of the two facets touching this edge
    {
        FaceID first, second;
        Edge() : first{UNASSIGNED}, second{UNASSIGNED} {}
        void   assign(FaceID fid)
        {
            first == UNASSIGNED ? first = fid : second = fid;
        }
    };

    // All vertex IDs will fit into this number of bits. (Used for hashing)
    const int max_vertex_id_bits = std::ceil(std::log2(its.vertices.size()));
    assert(max_vertex_id_bits <= 32);

    std::map< EdgeID, Edge > edge_index;

    // Edge id is constructed by concatenating two vertex ids, starting with
    // the lowest in MSB
    auto hash = [max_vertex_id_bits] (VertexID a, VertexID b) {
        if (a > b) std::swap(a, b);
        return (a << max_vertex_id_bits) + b;
    };

    // Go through all edges of all facets and mark the facets touching each edge
    for (size_t face_id = 0; face_id < its.indices.size(); ++face_id) {
        const Vec3i32 &face = its.indices[face_id];

        EdgeID e1 = hash(face(0), face(1)), e2 = hash(face(1), face(2)),
               e3 = hash(face(2), face(0));

        edge_index[e1].assign(face_id);
        edge_index[e2].assign(face_id);
        edge_index[e3].assign(face_id);
    }

    FaceNeighborIndex index(its.indices.size());

    // Now collect the neighbors for each facet into the final index
    for (size_t face_id = 0; face_id < its.indices.size(); ++face_id) {
        const Vec3i32 &face = its.indices[face_id];

        EdgeID e1 = hash(face(0), face(1)), e2 = hash(face(1), face(2)),
               e3 = hash(face(2), face(0));

        const Edge &neighs1 = edge_index[e1];
        const Edge &neighs2 = edge_index[e2];
        const Edge &neighs3 = edge_index[e3];

        std::array<size_t, 3> &neighs = index[face_id];
        neighs[0] = neighs1.first == face_id ? neighs1.second : neighs1.first;
        neighs[1] = neighs2.first == face_id ? neighs2.second : neighs2.first;
        neighs[2] = neighs3.first == face_id ? neighs3.second : neighs3.first;
    }

    return index;
}

std::vector<Vec3crd> its_create_neighbors_index_9(const indexed_triangle_set &its)
{
    return create_face_neighbors_index(ex_seq, its);
}

std::vector<Vec3i32> its_create_neighbors_index_10(const indexed_triangle_set &its)
{
    return create_face_neighbors_index(ex_tbb, its);
}

} // namespace Slic3r

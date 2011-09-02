#ifndef MESHSPLITIMPL_HPP
#define MESHSPLITIMPL_HPP

// Disable meaningless boost warning on MSVC
#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable : 4805 )
#endif

#include "TriangleMesh.hpp"
#include "libnest2d/tools/benchmark.h"
#include "Execution/ExecutionTBB.hpp"

namespace Slic3r {

template<class ExPolicy>
std::vector<Vec3i32> create_face_neighbors_index(ExPolicy &&ex, const indexed_triangle_set &its);

namespace meshsplit_detail {

template<class Its, class Enable = void> struct ItsWithNeighborsIndex_ {
    using Index = typename Its::Index;
    static const indexed_triangle_set &get_its(const Its &m) { return m.get_its();}
    static const Index &get_index(const Its &m) { return m.get_index(); }
};

// Define a default neighbors index for indexed_triangle_set
template<> struct ItsWithNeighborsIndex_<indexed_triangle_set> {
    using Index = std::vector<Vec3i32>;
    static const indexed_triangle_set &get_its(const indexed_triangle_set &its) noexcept { return its; }
    static Index get_index(const indexed_triangle_set &its) noexcept
    {
        return create_face_neighbors_index(ex_tbb, its);
    }
};

// Discover connected patches of facets one by one.
template<class NeighborIndex>
struct NeighborVisitor {
    NeighborVisitor(const indexed_triangle_set &its, const NeighborIndex &neighbor_index) :
        its(its), neighbor_index(neighbor_index) {
        m_visited.assign(its.indices.size(), false);
        m_facestack.reserve(its.indices.size());
    }
    NeighborVisitor(const indexed_triangle_set &its, NeighborIndex &&aneighbor_index) :
        its(its), neighbor_index(m_neighbor_index_data), m_neighbor_index_data(std::move(aneighbor_index)) {
        m_visited.assign(its.indices.size(), false);
        m_facestack.reserve(its.indices.size());
    }

    template<typename Visitor>
    void visit(Visitor visitor)
    {
        // find the next unvisited facet and push the index
        auto facet = std::find(m_visited.begin() + m_seed, m_visited.end(), false);
        m_seed = facet - m_visited.begin();

        if (facet != m_visited.end()) {
            // Skip this element in the next round.
            auto idx = m_seed ++;
            if (! visitor(idx))
                return;
            this->push(idx);
            m_visited[idx] = true;
            while (! m_facestack.empty()) {
                size_t facet_idx = this->pop();
                for (auto neighbor_idx : neighbor_index[facet_idx]) {
                    assert(neighbor_idx < int(m_visited.size()));
                    if (neighbor_idx >= 0 && !m_visited[neighbor_idx]) {
                        if (! visitor(size_t(neighbor_idx)))
                            return;
                        m_visited[neighbor_idx] = true;
                        this->push(stack_el(neighbor_idx));
                    }
                }
            }
        }
    }

    const indexed_triangle_set  &its;
    const NeighborIndex         &neighbor_index;

private:
    // If initialized with &&neighbor_index, take the ownership of the data.
    const NeighborIndex          m_neighbor_index_data;

    std::vector<char>            m_visited;

    using                        stack_el = size_t;
    std::vector<stack_el>        m_facestack;
    void                         push(const stack_el &s) { m_facestack.emplace_back(s); }
    stack_el                     pop() { stack_el ret = m_facestack.back(); m_facestack.pop_back(); return ret; }

    // Last face visited.
    size_t                       m_seed { 0 };
};

} // namespace meshsplit_detail

// Funky wrapper for timinig of its_split() using various neighbor index creating methods, see sandboxes/its_neighbor_index/main.cpp
template<class IndexT> struct ItsNeighborsWrapper
{
    using Index = IndexT;
    const indexed_triangle_set &its;
    const IndexT               &index_ref;
    const IndexT                index;

    // Keeping a reference to index, the caller is responsible for keeping the index alive.
    ItsNeighborsWrapper(const indexed_triangle_set &its, const IndexT &index) : its{its}, index_ref{index} {}
    // Taking ownership of the index.
    ItsNeighborsWrapper(const indexed_triangle_set &its, IndexT &&aindex) : its{its}, index_ref{index}, index(std::move(aindex)) {}

    const auto& get_its() const noexcept { return its; }
    const auto& get_index() const noexcept { return index_ref; }
};

// Can be used as the second argument to its_split to apply a functor on each
// part, instead of collecting them into a container.
template<class Fn>
struct SplitOutputFn {

    Fn fn;

    SplitOutputFn(Fn f): fn{std::move(f)} {}

    SplitOutputFn &operator *() { return *this; }
    void           operator=(indexed_triangle_set &&its) { fn(std::move(its)); }
    void           operator=(indexed_triangle_set &its) { fn(its); }
    SplitOutputFn& operator++() { return *this; };
};

// Splits a mesh into multiple meshes when possible.
template<class Its, class OutputIt>
void its_split(const Its &m, OutputIt out_it)
{
    using namespace meshsplit_detail;

    const indexed_triangle_set &its = ItsWithNeighborsIndex_<Its>::get_its(m);

    struct VertexConv {
        size_t part_id      = std::numeric_limits<size_t>::max();
        size_t vertex_image;
    };
    std::vector<VertexConv> vidx_conv(its.vertices.size());

    meshsplit_detail::NeighborVisitor visitor(its, meshsplit_detail::ItsWithNeighborsIndex_<Its>::get_index(m));

    std::vector<size_t> facets;
    for (size_t part_id = 0;; ++part_id) {
        // Collect all faces of the next patch.
        facets.clear();
        visitor.visit([&facets](size_t idx) { facets.emplace_back(idx); return true; });
        if (facets.empty())
            break;

        // Create a new mesh for the part that was just split off.
        indexed_triangle_set mesh;
        mesh.indices.reserve(facets.size());
        mesh.vertices.reserve(std::min(facets.size() * 3, its.vertices.size()));

        // Assign the facets to the new mesh.
        for (size_t face_id : facets) {
            const auto &face = its.indices[face_id];
            Vec3i32       new_face;
            for (size_t v = 0; v < 3; ++v) {
                auto vi = face(v);

                if (vidx_conv[vi].part_id != part_id) {
                    vidx_conv[vi] = {part_id, mesh.vertices.size()};
                    mesh.vertices.emplace_back(its.vertices[size_t(vi)]);
                }

                new_face(v) = vidx_conv[vi].vertex_image;
            }

            mesh.indices.emplace_back(new_face);
        }

        *out_it = std::move(mesh);
        ++out_it;
    }
}

template<class Its>
std::vector<indexed_triangle_set> its_split(const Its &its)
{
    auto ret = reserve_vector<indexed_triangle_set>(3);
    its_split(its, std::back_inserter(ret));

    return ret;
}

template<class Its>
bool its_is_splittable(const Its &m)
{
    meshsplit_detail::NeighborVisitor visitor(meshsplit_detail::ItsWithNeighborsIndex_<Its>::get_its(m), meshsplit_detail::ItsWithNeighborsIndex_<Its>::get_index(m));
    bool has_some = false;
    bool has_some2 = false;
    // Traverse the 1st patch fully.
    visitor.visit([&has_some](size_t idx) { has_some = true; return true; });
    if (has_some)
        // Just check whether there is any face of the 2nd patch.
        visitor.visit([&has_some2](size_t idx) { has_some2 = true; return false; });
    return has_some && has_some2;
}

template<class Its>
size_t its_number_of_patches(const Its &m)
{
    meshsplit_detail::NeighborVisitor visitor(meshsplit_detail::ItsWithNeighborsIndex_<Its>::get_its(m), meshsplit_detail::ItsWithNeighborsIndex_<Its>::get_index(m));
    size_t num_patches = 0;
    for (;;) {
        bool has_some = false;
        // Traverse the 1st patch fully.
        visitor.visit([&has_some](size_t idx) { has_some = true; return true; });
        if (! has_some)
            break;
        ++ num_patches;
    }
    return num_patches;
}

template<class ExPolicy>
std::vector<Vec3i32> create_face_neighbors_index(ExPolicy &&ex, const indexed_triangle_set &its)
{
    const std::vector<stl_triangle_vertex_indices> &indices = its.indices;

    if (indices.empty()) return {};

    assert(! its.vertices.empty());

    auto               vertex_triangles = VertexFaceIndex{its};
    static constexpr int no_value         = -1;
    std::vector<Vec3i32> neighbors(indices.size(),
                                 Vec3i32(no_value, no_value, no_value));

    //for (int face_idx = 0; face_idx < indices.size(); face_idx++) {
    execution::for_each(ex, size_t(0), indices.size(),
        [&neighbors, &indices, &vertex_triangles] (size_t face_idx)
        {
            Vec3i32& neighbor = neighbors[face_idx];
            const stl_triangle_vertex_indices & triangle_indices = indices[face_idx];
            for (int edge_index = 0; edge_index < 3; ++edge_index) {
                // check if done
                int& neighbor_edge = neighbor[edge_index];
                if (neighbor_edge != no_value)
                    // This edge already has a neighbor assigned.
                    continue;
                Vec2i32 edge_indices = its_triangle_edge(triangle_indices, edge_index);
                // IMPROVE: use same vector for 2 sides of triangle
                for (const size_t other_face : vertex_triangles[edge_indices[0]]) {
                    if (other_face <= face_idx) continue;
                    const stl_triangle_vertex_indices &face_indices = indices[other_face];
                    int vertex_index = its_triangle_vertex_index(face_indices, edge_indices[1]);
                    // NOT Contain second vertex?
                    if (vertex_index < 0) continue;
                    // Has NOT oposite direction?
                    if (edge_indices[0] != face_indices[(vertex_index + 1) % 3]) continue;
                    //BBS: if this neighbor has already marked before, skip it
                    if (neighbors[other_face][vertex_index] != no_value)
                        continue;
                    //BBS: the same triangle with opposite direction, also treat it as open edges
                    //if (its_triangle_vertex_the_same(face_indices, triangle_indices))
                    //    continue;
                    neighbor_edge = other_face;
                    neighbors[other_face][vertex_index] = face_idx;
                    break;
                }
            }
        }, execution::max_concurrency(ex));

    return neighbors;
}

} // namespace Slic3r

#ifdef _MSC_VER
#pragma warning( pop )
#endif

#endif // MESHSPLITIMPL_HPP

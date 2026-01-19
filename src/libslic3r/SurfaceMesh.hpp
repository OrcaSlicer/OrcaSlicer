#ifndef slic3r_SurfaceMesh_hpp_
#define slic3r_SurfaceMesh_hpp_

#include <admesh/stl.h>
#include <libslic3r/TriangleMesh.hpp>

#include "boost/container/small_vector.hpp"

namespace Slic3r {

class TriangleMesh;



enum Face_index : int;

class Halfedge_index {
    friend class SurfaceMesh;

public:
    Halfedge_index() : m_face(Face_index(-1)), m_side(0) {}
    Face_index face() const { return m_face; }
    unsigned char side() const { return m_side; }
    bool is_invalid() const { return int(m_face) < 0; }
    bool operator!=(const Halfedge_index& rhs) const { return ! ((*this) == rhs); }
    bool operator==(const Halfedge_index& rhs) const { return m_face == rhs.m_face && m_side == rhs.m_side; }

private:
    Halfedge_index(int face_idx, unsigned char side_idx) : m_face(Face_index(face_idx)), m_side(side_idx) {}

    Face_index m_face;
    unsigned char m_side;
};



class Vertex_index {
    friend class SurfaceMesh;

public:
    Vertex_index() : m_face(Face_index(-1)), m_vertex_idx(0) {}
    bool is_invalid() const { return int(m_face) < 0; }
    bool operator==(const Vertex_index& rhs) const = delete; // Use SurfaceMesh::is_same_vertex.

private:
    Vertex_index(int face_idx, unsigned char vertex_idx) : m_face(Face_index(face_idx)), m_vertex_idx(vertex_idx) {}

    Face_index m_face;
    unsigned char m_vertex_idx;
};



class SurfaceMesh {
public:
    explicit SurfaceMesh(const indexed_triangle_set& its)
    : m_its(its),
      m_face_neighbors(its_face_neighbors_par(its))
    {}
    SurfaceMesh(const SurfaceMesh&)            = delete;
    SurfaceMesh& operator=(const SurfaceMesh&) = delete;

    Vertex_index source(Halfedge_index h) const { assert(! h.is_invalid()); return Vertex_index(h.m_face, h.m_side); }
    Vertex_index target(Halfedge_index h) const { assert(! h.is_invalid()); return Vertex_index(h.m_face, h.m_side == 2 ? 0 : h.m_side + 1); }
    Face_index face(Halfedge_index h) const { assert(! h.is_invalid()); return h.m_face; }

    Halfedge_index next(Halfedge_index h)     const { assert(! h.is_invalid()); h.m_side = (h.m_side + 1) % 3; return h; }
    Halfedge_index prev(Halfedge_index h)     const { assert(! h.is_invalid()); h.m_side = (h.m_side == 0 ? 2 : h.m_side - 1); return h; }
    Halfedge_index halfedge(Vertex_index v)   const { return Halfedge_index(v.m_face, (v.m_vertex_idx == 0 ? 2 : v.m_vertex_idx - 1)); }  
    Halfedge_index halfedge(Face_index f)     const { return Halfedge_index(f, 0); }  
    Halfedge_index opposite(Halfedge_index h) const {
        if (h.is_invalid())
            return h;

        int face_idx = m_face_neighbors[h.m_face][h.m_side];
        Halfedge_index h_candidate = halfedge(Face_index(face_idx));

        if (h_candidate.is_invalid())
            return Halfedge_index(); // invalid

        for (int i=0; i<3; ++i) {
            if (is_same_vertex(source(h_candidate), target(h))) {
                // Meshes in PrusaSlicer should be fixed enough for the following not to happen.
                assert(is_same_vertex(target(h_candidate), source(h)));
                return h_candidate;
            }
            h_candidate = next(h_candidate);
        }
        return Halfedge_index(); // invalid
    }

    Halfedge_index next_around_target(Halfedge_index h) const { return opposite(next(h)); }
    Halfedge_index prev_around_target(Halfedge_index h) const { Halfedge_index op = opposite(h); return (op.is_invalid() ? Halfedge_index() : prev(op)); }
    Halfedge_index next_around_source(Halfedge_index h) const { Halfedge_index op = opposite(h); return (op.is_invalid() ? Halfedge_index() : next(op)); }
    Halfedge_index prev_around_source(Halfedge_index h) const { return opposite(prev(h)); }
    Halfedge_index halfedge(Vertex_index source, Vertex_index target) const
    {
        Halfedge_index hi(source.m_face, source.m_vertex_idx);
        assert(! hi.is_invalid());

        const Vertex_index orig_target = this->target(hi);
        Vertex_index current_target = orig_target;

        while (! is_same_vertex(current_target, target)) {
            hi = next_around_source(hi);
            if (hi.is_invalid())
                break;
            current_target = this->target(hi);
            if (is_same_vertex(current_target, orig_target))
                return Halfedge_index(); // invalid
        }

        return hi;
    }

    const stl_vertex& point(Vertex_index v) const { return m_its.vertices[m_its.indices[v.m_face][v.m_vertex_idx]]; }

    size_t degree(Vertex_index v) const
    {
        // In case the mesh is broken badly, the loop might end up to be infinite,
        // never getting back to the first halfedge. Remember list of all half-edges
        // and trip if any is encountered for the second time.
        Halfedge_index h_first = halfedge(v);
        boost::container::small_vector<Halfedge_index, 10> he_visited;
        Halfedge_index h = next_around_target(h_first);
        size_t degree = 2;
        while (! h.is_invalid() && h != h_first) {
            he_visited.emplace_back(h);
            h = next_around_target(h);
            if (std::find(he_visited.begin(), he_visited.end(), h) == he_visited.end())
                return 0;
            ++degree;
        }
        return h.is_invalid() ? 0 : degree - 1;
    }

    size_t degree(Face_index f) const {
        size_t total = 0;
        for (unsigned char i=0; i<3; ++i) {
            size_t d = degree(Vertex_index(f, i));
            if (d == 0)
                return 0;
            total += d;
        }
        assert(total - 6 >= 0);
        return total - 6; // we counted 3 halfedges from f, and one more for each neighbor
    }

    bool is_border(Halfedge_index h) const { return m_face_neighbors[h.m_face][h.m_side] == -1; }

    bool is_same_vertex(const Vertex_index& a, const Vertex_index& b) const { return m_its.indices[a.m_face][a.m_vertex_idx] == m_its.indices[b.m_face][b.m_vertex_idx]; }
    Vec3i32 get_face_neighbors(Face_index face_id) const { assert(int(face_id) < int(m_face_neighbors.size())); return m_face_neighbors[face_id]; }



private:
    const std::vector<Vec3i32> m_face_neighbors;
    const indexed_triangle_set& m_its;
};

} //namespace Slic3r

#endif // slic3r_SurfaceMesh_hpp_

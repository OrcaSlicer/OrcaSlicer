/**
 * Copyright (c) 2021-2022 Floyd M. Chitalu.
 * All rights reserved.
 *
 * NOTE: This file is licensed under GPL-3.0-or-later (default).
 * A commercial license can be purchased from Floyd M. Chitalu.
 *
 * License details:
 *
 * (A)  GNU General Public License ("GPL"); a copy of which you should have
 *      recieved with this file.
 * 	    - see also: <http://www.gnu.org/licenses/>
 * (B)  Commercial license.
 *      - email: floyd.m.chitalu@gmail.com
 *
 * The commercial license options is for users that wish to use MCUT in
 * their products for comercial purposes but do not wish to release their
 * software products under the GPL license.
 *
 * Author(s)     : Floyd M. Chitalu
 */

#ifndef MCUT_HALFEDGE_MESH_H_
#define MCUT_HALFEDGE_MESH_H_

#include "mcut/internal/math.h"
#include "mcut/internal/utils.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <vector>
#include <cstdint>

template <typename T>
class descriptor_t_ {
public:
    typedef unsigned int index_type;
    descriptor_t_() { }
    virtual ~descriptor_t_() { }
    explicit descriptor_t_(index_type i = (std::numeric_limits<index_type>::max)())
        : m_value(i)
    {
    }

    operator index_type() const { return m_value; }

    void reset() { m_value = (std::numeric_limits<index_type>::max)(); }

    bool is_valid() const
    {
        index_type inf = (std::numeric_limits<index_type>::max)();
        return m_value != inf;
    }

    descriptor_t_& operator=(const index_type& _rhs)
    {
        m_value = _rhs;
        return *this;
    }

    descriptor_t_& operator=(const T& _rhs) const
    {
        m_value = _rhs.m_value;
        return *this;
    }

    bool operator==(const T& _rhs) const
    {
        return m_value == _rhs.m_value;
    }

    bool operator!=(const T& _rhs) const
    {
        return m_value != _rhs.m_value;
    }

    bool operator<(const T& _rhs) const
    {
        return m_value < _rhs.m_value;
    }

    descriptor_t_& operator++()
    {
        ++m_value;
        return *this;
    }

    descriptor_t_& operator--()
    {
        --m_value;
        return *this;
    }

    descriptor_t_ operator++(int)
    {
        descriptor_t_ tmp(*this);
        ++m_value;
        return tmp;
    }

    descriptor_t_ operator--(int)
    {
        descriptor_t_ tmp(*this);
        --m_value;
        return tmp;
    }

    descriptor_t_& operator+=(std::ptrdiff_t n)
    {
        m_value = (unsigned int)(m_value + n);
        return *this;
    }

protected:
    unsigned int m_value;
};

class halfedge_descriptor_t : public descriptor_t_<halfedge_descriptor_t> {
public:
    halfedge_descriptor_t()
        : descriptor_t_<halfedge_descriptor_t>(std::numeric_limits<index_type>::max())
    {
    }

    explicit halfedge_descriptor_t(descriptor_t_<halfedge_descriptor_t>::index_type idx)
        : descriptor_t_<halfedge_descriptor_t>(idx)
    {
    }

    virtual ~halfedge_descriptor_t()
    {
    }
};

class edge_descriptor_t : public descriptor_t_<edge_descriptor_t> {
public:
    edge_descriptor_t()
        : descriptor_t_<edge_descriptor_t>((std::numeric_limits<index_type>::max)())
    {
    }

    explicit edge_descriptor_t(descriptor_t_<edge_descriptor_t>::index_type idx)
        : descriptor_t_<edge_descriptor_t>(idx)
    {
    }

    virtual ~edge_descriptor_t()
    {
    }
};

class face_descriptor_t : public descriptor_t_<face_descriptor_t> {
public:
    face_descriptor_t()
        : descriptor_t_<face_descriptor_t>((std::numeric_limits<index_type>::max)())
    {
    }

    explicit face_descriptor_t(descriptor_t_<face_descriptor_t>::index_type idx)
        : descriptor_t_<face_descriptor_t>(idx)
    {
    }

    virtual ~face_descriptor_t()
    {
    }
};

class vertex_descriptor_t : public descriptor_t_<vertex_descriptor_t> {
public:
    vertex_descriptor_t()
        : descriptor_t_<vertex_descriptor_t>((std::numeric_limits<index_type>::max)())
    {
    }

    explicit vertex_descriptor_t(descriptor_t_<vertex_descriptor_t>::index_type idx)
        : descriptor_t_<vertex_descriptor_t>(idx)
    {
    }

    virtual ~vertex_descriptor_t()
    {
    }
};

template <typename T>
struct id_ {
    typedef T type;
};

template <typename V>
class array_iterator_t;

struct halfedge_data_t : id_<halfedge_descriptor_t> {
    vertex_descriptor_t t; // target vertex
    face_descriptor_t f; // face
    halfedge_descriptor_t o; // opposite halfedge
    halfedge_descriptor_t n; // next halfedge
    halfedge_descriptor_t p; // previous halfedge
    edge_descriptor_t e; // edge
    

    halfedge_data_t()
    //: o(null_halfedge()), n(null_halfedge()), p(null_halfedge()), t(null_vertex()), e(null_edge()), f(null_face())
    {
    }
};

struct edge_data_t : id_<edge_descriptor_t> {
    halfedge_descriptor_t h; // primary halfedge (even idx)
};

struct face_data_t : id_<face_descriptor_t> {
    std::vector<halfedge_descriptor_t> m_halfedges;
};

struct vertex_data_t : id_<vertex_descriptor_t> {
    vec3 p; // geometry coordinates
    //std::vector<face_descriptor_t> m_faces; // ... incident to vertex // TODO: this is not needed (can be inferred from "m_halfedges")
    std::vector<halfedge_descriptor_t> m_halfedges; // ... which point to vertex (note: can be used to infer edges too)
};

typedef std::vector<vertex_data_t> vertex_array_t;
typedef std::vector<edge_data_t> edge_array_t;
typedef std::vector<halfedge_data_t> halfedge_array_t;
typedef std::vector<face_data_t> face_array_t;

typedef array_iterator_t<face_array_t> face_array_iterator_t;
typedef array_iterator_t<vertex_array_t> vertex_array_iterator_t;
typedef array_iterator_t<edge_array_t> edge_array_iterator_t;
typedef array_iterator_t<halfedge_array_t> halfedge_array_iterator_t;

/*
    Internal mesh data structure used for cutting meshes

    Memory Management

    Memory management is semi-automatic. Memory grows as more elements are added to the structure but does not shrink when elements are removed.
    When you add elements and the capacity of the underlying vector is exhausted, the vector reallocates memory.
    As descriptors are basically indices, they refer to the same element after a reallocation.
    When you remove an element it is only marked as removed.
    Internally it is put in a free list, and when you add elements to the surface mesh, they are taken from the free list in case it is not empty.

    For all elements there is a function to obtain the number of used elements, as well as the number of used [and] removed elements.
    For vertices the functions are hmesh_t::number_of_vertices() and hmesh_t::number_of_internal_vertices(), respectively.
    The first function is slightly different from the free function num_vertices(const G&) of the BGL package.

    Iterators such as hmesh_t::vertex_iterator_t only enumerate elements that are not marked as deleted.
*/
class hmesh_t {
public:
    hmesh_t();
    ~hmesh_t();

    // static member functions
    // -----------------------

    static vertex_descriptor_t null_vertex();
    static halfedge_descriptor_t null_halfedge();
    static edge_descriptor_t null_edge();
    static face_descriptor_t null_face();

    // regular member functions
    // ------------------------

    // excluding removed elements
    int number_of_vertices() const;
    int number_of_edges() const;
    int number_of_halfedges() const;
    int number_of_faces() const;

    vertex_descriptor_t source(const halfedge_descriptor_t& h) const;
    vertex_descriptor_t target(const halfedge_descriptor_t& h) const;
    halfedge_descriptor_t opposite(const halfedge_descriptor_t& h) const;
    halfedge_descriptor_t prev(const halfedge_descriptor_t& h) const;
    halfedge_descriptor_t next(const halfedge_descriptor_t& h) const;

    void set_next(const halfedge_descriptor_t& h, const halfedge_descriptor_t& nxt);
    void set_previous(const halfedge_descriptor_t& h, const halfedge_descriptor_t& prev);

    edge_descriptor_t edge(const halfedge_descriptor_t& h) const;
    face_descriptor_t face(const halfedge_descriptor_t& h) const;

    vertex_descriptor_t vertex(const edge_descriptor_t e, const int v) const;

    bool is_border(const halfedge_descriptor_t h);
    bool is_border(const edge_descriptor_t e);

    halfedge_descriptor_t halfedge(const edge_descriptor_t e, const int i) const;
    // finds a halfedge between two vertices. Returns a default constructed halfedge descriptor, if source and target are not connected.
    halfedge_descriptor_t halfedge(const vertex_descriptor_t s, const vertex_descriptor_t t, bool strict_check = false) const;
    // finds an edge between two vertices. Returns a default constructed halfedge descriptor, if source and target are not connected.
    edge_descriptor_t edge(const vertex_descriptor_t s, const vertex_descriptor_t t, bool strict_check = false) const;

    vertex_descriptor_t add_vertex(const vec3& point);

    vertex_descriptor_t add_vertex(const double& x, const double& y, const double& z);
    // adds an edges into the mesh data structure, creating incident halfedges, and returns the
    // halfedge whole target is "v1"
    halfedge_descriptor_t add_edge(const vertex_descriptor_t v0, const vertex_descriptor_t v1);
    face_descriptor_t add_face(const std::vector<vertex_descriptor_t>& vi);
    // checks whether adding this face will violate 2-manifoldness (i.e. halfedge 
    // construction rules) which would lead to creating a non-manifold edge 
    // (one that is referenced by more than 2 faces which is illegal). 
    bool is_insertable(const std::vector<vertex_descriptor_t> &vi) const;

    // also disassociates (not remove) any halfedges(s) and vertices incident to face
    void remove_face(const face_descriptor_t f);
    // also disassociates (not remove) the halfedges(s) and vertex incident to this halfedge
    void remove_halfedge(halfedge_descriptor_t h);
    // also disassociates (not remove) any face(s) incident to edge via its halfedges, and also disassociates the halfedges
    void remove_edge(const edge_descriptor_t e, bool remove_halfedges = true);
    void remove_vertex(const vertex_descriptor_t v);
    void remove_elements();

    void reset();

    int number_of_internal_faces() const;
    int number_of_internal_edges() const;
    int number_of_internal_halfedges() const;
    int number_of_internal_vertices() const;

    int number_of_vertices_removed() const;
    int number_of_edges_removed() const;
    int number_of_halfedges_removed() const;
    int number_of_faces_removed() const;

    bool is_removed(face_descriptor_t f) const;
    bool is_removed(edge_descriptor_t e) const;
    bool is_removed(halfedge_descriptor_t h) const;
    bool is_removed(vertex_descriptor_t v) const;

    void reserve_for_additional_vertices(std::uint32_t n);
    void reserve_for_additional_edges(std::uint32_t n);
    void reserve_for_additional_halfedges(std::uint32_t n);
    void reserve_for_additional_faces(std::uint32_t n);
    void reserve_for_additional_elements(std::uint32_t additional_vertices);

    ///

    template <typename I = int>
    I get_removed_elements(id_<I>)
    {
        return I(); // unused
    }

    const std::vector<vertex_descriptor_t>& get_removed_elements(id_<array_iterator_t<vertex_array_t>>) const;
    const std::vector<edge_descriptor_t>& get_removed_elements(id_<array_iterator_t<edge_array_t>>) const;
    const std::vector<halfedge_descriptor_t>& get_removed_elements(id_<array_iterator_t<halfedge_array_t>>) const;
    const std::vector<face_descriptor_t>& get_removed_elements(id_<array_iterator_t<face_array_t>>) const;

    //
    template <typename I = int>
    I elements_begin_(id_<I>)
    {
        return I(); // unused
    }

    const vertex_array_iterator_t elements_begin_(id_<vertex_array_iterator_t>, bool account_for_removed_elems = true) const;
    const edge_array_iterator_t elements_begin_(id_<edge_array_iterator_t>, bool account_for_removed_elems = true) const;
    const halfedge_array_iterator_t elements_begin_(id_<halfedge_array_iterator_t>, bool account_for_removed_elems = true) const;
    const face_array_iterator_t elements_begin_(id_<face_array_iterator_t>, bool account_for_removed_elems = true) const;

    // returns the number of removed mesh elements (vertices, edges, faces or halfedges) between [start, end)
    template <typename I>
    uint32_t count_removed_elements_in_range(const array_iterator_t<I>& start, const array_iterator_t<I>& end) const
    {
        const long long N = (uint32_t)(end - start); // length including removed elements
        MCUT_ASSERT(N >= 0);
        if (N == 0) {
            return 0;
        }

        // raw starting ptr offset
        const uint32_t start_ = (std::uint32_t)(start - elements_begin_(id_<array_iterator_t<I>> {}, false));
        uint32_t n = 0;

        for (auto elem_descr : get_removed_elements(id_<array_iterator_t<I>> {})) {
            const uint32_t descr = (uint32_t)elem_descr;

            if (descr >= start_ && (descr <= (start_ + (uint32_t)(N - 1)))) {
                ++n;
            }
        }
        return n;
    }

    const vec3& vertex(const vertex_descriptor_t& vd) const;
    // returns vector of halfedges which point to vertex (i.e. "v" is their target)
    const std::vector<halfedge_descriptor_t>& get_halfedges_around_vertex(const vertex_descriptor_t v) const;
    std::vector<vertex_descriptor_t> get_vertices_around_face(const face_descriptor_t f, uint32_t prepend_offset = 0) const;
    void get_vertices_around_face(std::vector<vertex_descriptor_t>& vertex_descriptors, const face_descriptor_t f, uint32_t prepend_offset=0) const;
    std::vector<vertex_descriptor_t> get_vertices_around_vertex(const vertex_descriptor_t v) const;
    void get_vertices_around_vertex(std::vector<vertex_descriptor_t>& vertices_around_vertex, const vertex_descriptor_t v) const;
    uint32_t get_num_vertices_around_face(const face_descriptor_t f) const;
    const std::vector<halfedge_descriptor_t>& get_halfedges_around_face(const face_descriptor_t f) const;
    const std::vector<face_descriptor_t> get_faces_around_face(const face_descriptor_t f, const std::vector<halfedge_descriptor_t>* halfedges_around_face_ = nullptr) const;
    void get_faces_around_face( std::vector<face_descriptor_t>& faces_around_face, const face_descriptor_t f, const std::vector<halfedge_descriptor_t>* halfedges_around_face_ = nullptr) const;
    uint32_t get_num_faces_around_face(const face_descriptor_t f, const std::vector<halfedge_descriptor_t>* halfedges_around_face_ = nullptr) const;
    
    // iterators
    // ---------

    vertex_array_iterator_t vertices_begin(bool account_for_removed_elems = true) const;
    vertex_array_iterator_t vertices_end() const;
    edge_array_iterator_t edges_begin(bool account_for_removed_elems = true) const;
    edge_array_iterator_t edges_end() const;
    halfedge_array_iterator_t halfedges_begin(bool account_for_removed_elems = true) const;
    halfedge_array_iterator_t halfedges_end() const;
    face_array_iterator_t faces_begin(bool account_for_removed_elems = true) const;
    face_array_iterator_t faces_end() const;

    const std::vector<vertex_descriptor_t>& get_removed_vertices() const;
    const std::vector<edge_descriptor_t>& get_removed_edges() const;
    const std::vector<halfedge_descriptor_t>& get_removed_halfedges() const;
    const std::vector<face_descriptor_t>& get_removed_faces() const;

private:
    // member variables
    // ----------------

    std::vector<vertex_data_t> m_vertices;
    std::vector<edge_data_t> m_edges;
    std::vector<halfedge_data_t> m_halfedges;
    std::vector<face_data_t> m_faces;

    // NOTE: I use std::vector because we'll have very few (typically zero)
    // elements removed at a given time. In fact removal only happens during
    // input-mesh face-partitioning to resolve floating polygons, which is
    // rare. Maybe in the future things change...
    std::vector<face_descriptor_t> m_faces_removed;
    std::vector<edge_descriptor_t> m_edges_removed;
    std::vector<halfedge_descriptor_t> m_halfedges_removed;
    std::vector<vertex_descriptor_t> m_vertices_removed;

}; // class hmesh_t {

typedef vertex_descriptor_t vd_t;
typedef halfedge_descriptor_t hd_t;
typedef edge_descriptor_t ed_t;
typedef face_descriptor_t fd_t;

void write_off(const char* fpath, const hmesh_t& mesh);
void read_off(hmesh_t& mesh, const char* fpath);

template <typename V = face_array_t>
class array_iterator_t : public V::const_iterator {
    const hmesh_t* mesh_ptr;
    typedef typename V::const_iterator std_iterator_base_class;
    typedef typename V::value_type::type element_descriptor_type;
    typename V::value_type* operator->() = delete;

public:
    array_iterator_t()
        : V::const_iterator()
        , mesh_ptr(nullptr) {};
    array_iterator_t(typename V::const_iterator it_, const hmesh_t* const mesh)
        : V::const_iterator(it_)
        , mesh_ptr(mesh)
    {
    }

    const hmesh_t* get_mesh_ptr() const
    {
        return mesh_ptr;
    }

    typename V::value_type::type operator*()
    {
        size_t raw_index = (*this) - cbegin<>(false);
        element_descriptor_type d((std::uint32_t)raw_index);
        return d;
    }

    // prefix increment (++i)
    // increment pointer to the next valid element (i.e. we skip removed elements).
    array_iterator_t<V>& operator++()
    {
        bool cur_elem_is_removed = false;
        bool reached_end = false;
        do {
            V::const_iterator::operator++();
            reached_end = (*this) == cend<>();
            cur_elem_is_removed = false;

            if (!reached_end) {
                const std::size_t diff = ((*this) - cbegin<array_iterator_t<V>>(false));
                element_descriptor_type raw_descriptor((std::uint32_t)diff); // std::distance(cbegin<array_iterator_t<V>>(false), (*this)); // O(1) ??
                cur_elem_is_removed = mesh_ptr->is_removed(raw_descriptor);
                if (!cur_elem_is_removed) {
                    break;
                }
            }

            // keep iterating until the value pointed to after the (++i) operator is a valid element
            // i.e. one that is not marked removed!

        } while (cur_elem_is_removed && !reached_end);

        return (*this);
    }

    // we provide this overide to ensure that stl functions like std::advance, work properly
    // by accounting for removed elements
    array_iterator_t<V>& operator+=(std::ptrdiff_t n)
    {
        V::const_iterator::operator+=(n); // raw ptr shift (i.e. ignoring that there may be removed elements)

        bool cur_elem_is_removed = false;
        bool reached_end = (*this) == cend<>();
        cur_elem_is_removed = mesh_ptr->is_removed(*(*this));
        while (!reached_end && cur_elem_is_removed) {
            V::const_iterator::operator++(); //++(*this);
            size_t raw_descriptor = *(*this); // (*this) - cbegin<array_iterator_t<V>>(false); //std::distance(cbegin<array_iterator_t<V>>(false), (*this)); // O(1) ??
            cur_elem_is_removed = mesh_ptr->is_removed(element_descriptor_type((std::uint32_t)raw_descriptor));
            if (!cur_elem_is_removed) {
                break;
            }

            reached_end = (*this) == cend<>();
        }
        return *this;
    }

    // The following are helper functions which are specialised (via type-deduction)
    // for the type of mesh elements that *this* iterator walks over in "mesh_ptr"
    // e.g. faces. These functions are used to determine when *this* iterator has
    // reached the end of the respective std::map data structure over which we are
    // iterating.

    template <typename I = array_iterator_t<V>>
    I cend()
    {
        return cend(id_<I>()); // https://stackoverflow.com/questions/3052579/explicit-specialization-in-non-namespace-scope
    }

    template <typename I = array_iterator_t<V>>
    I cbegin(bool account_for_removed_elems)
    {
        return cbegin(account_for_removed_elems, id_<I>());
    }

private:
    // postfix increment (i++)
    // NOTE: This overide is private to simplify implementation, and we don't need it
    array_iterator_t<V> operator++(int)
    {
        MCUT_ASSERT(false);
        return cend<>();
    }

    template <typename I = array_iterator_t<V>>
    I cend(id_<I>)
    {
        return I(); // unused stub
    }

    template <typename I = array_iterator_t<V>>
    I cbegin(bool account_for_removed_elems, id_<I>)
    {
        return I(account_for_removed_elems); // unused stub
    }

    vertex_array_iterator_t cbegin(bool account_for_removed_elems, id_<vertex_array_iterator_t> = {});
    vertex_array_iterator_t cend(id_<vertex_array_iterator_t>);

    edge_array_iterator_t cbegin(bool account_for_removed_elems, id_<edge_array_iterator_t> = {});
    edge_array_iterator_t cend(id_<edge_array_iterator_t>);

    halfedge_array_iterator_t cbegin(bool account_for_removed_elems, id_<halfedge_array_iterator_t> = {});
    halfedge_array_iterator_t cend(id_<halfedge_array_iterator_t>);

    face_array_iterator_t cbegin(bool account_for_removed_elems, id_<face_array_iterator_t> = {});
    face_array_iterator_t cend(id_<face_array_iterator_t>);
}; // class array_iterator_t : public V::const_iterator

namespace std {
#if 1
template <>
inline typename edge_array_iterator_t::difference_type distance(
    edge_array_iterator_t first,
    edge_array_iterator_t last)
{
    MCUT_ASSERT(first.get_mesh_ptr() == last.get_mesh_ptr());
    edge_array_iterator_t it = first;
    edge_array_iterator_t::difference_type dist = last - first;

    uint32_t r = it.get_mesh_ptr()->count_removed_elements_in_range(first, last);
    if (r > 0) {
        dist = dist - r;
    }

    MCUT_ASSERT(dist >= 0);

    return dist;
}
#endif
#if 0
    template <>
    void advance(
        hmesh_t::array_iterator_t<hmesh_t::edge_array_t> &iter,
        typename std::iterator_traits<hmesh_t::array_iterator_t<hmesh_t::edge_array_t>>::difference_type n);
#endif

template <>
struct hash<vertex_descriptor_t> {
    std::size_t operator()(const vertex_descriptor_t& k) const
    {
        return std::hash<typename vertex_descriptor_t::index_type>()(static_cast<typename vertex_descriptor_t::index_type>(k));
    }
};

template <>
struct hash<edge_descriptor_t> {
    std::size_t operator()(const edge_descriptor_t& k) const
    {
        return std::hash<typename edge_descriptor_t::index_type>()(static_cast<typename edge_descriptor_t::index_type>(k));
    }
};

template <>
struct hash<halfedge_descriptor_t> {
    std::size_t operator()(const halfedge_descriptor_t& k) const
    {
        return std::hash<typename halfedge_descriptor_t::index_type>()(static_cast<typename halfedge_descriptor_t::index_type>(k));
    }
};

template <>
struct hash<face_descriptor_t> {
    std::size_t operator()(const face_descriptor_t& k) const
    {
        return std::hash<typename face_descriptor_t::index_type>()(static_cast<typename face_descriptor_t::index_type>(k));
    }
};
}

#endif // #ifndef MCUT_HALFEDGE_MESH_H_
